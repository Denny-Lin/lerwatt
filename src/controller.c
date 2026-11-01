// SPDX-License-Identifier: MIT
/* Lerwatt control law. See include/lerwatt/controller.h. */
#include <limits.h>
#include <math.h>

#include "lerwatt/controller.h"

void lw_ctl_default_params(struct lw_ctl_params *p, int32_t lowest_passive_mc)
{
	p->soft_limit_mc = (lowest_passive_mc != INT32_MAX ? lowest_passive_mc : 85000) - 5000;
	p->raw_limit_mc = (lowest_passive_mc != INT32_MAX ? lowest_passive_mc : 85000) - 3000;
	p->kp_w_per_c = 0.25;
	p->ki_w_per_cs = 0.05;
	p->deadband_c = 1.0;
	p->temp_filter_s = 1.0;
	p->temp_slow_filter_s = 10.0;
	p->up_margin = 0.03;
	p->down_step_ms = 1000;
	p->up_step_ms = 10000;
	p->min_ceiling_frac = 0.5;
	p->price_ramp_c = 5.0;
	p->lat_crit_threshold = 0.5;
	p->policy = LW_POLICY_RULE;
	p->cross_cluster = false;
	p->temporal_forecast = true;
	p->mpc_horizon_s = 5.0;
	p->mpc_over_soft_c = p->deadband_c;	/* same filtered line as the rule */
	p->mpc_margin_c = 0.5;
	p->mpc_peak_memory_s = 30.0;
	p->mpc_hold_s = 120.0;
	p->mpc_up_wait_max_s = 320.0;
	p->mpc_load_filter_s = 30.0;
	p->mpc_package_filter_s = 2.0;
	p->mpc_raw_gap_c = 1.0;
	p->mpc_peak_filter_s = 10.0;
	p->mpc_up_step_ms = 10000;
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

double lw_ctl_est_power_w(const struct lw_ctl_in *in, int i)
{
	double fmax = in->opps[in->nopps - 1].khz;
	/* Frequency-invariant load: share of the top OPP's throughput in use. */
	double inv = in->busy_mean * (double)in->cur_khz / fmax;
	double busy = clampd(inv * fmax / in->opps[i].khz, 0.0, 1.0);

	return in->ncpus * busy * in->opps[i].power_uw * 1e-6;
}

static double full_power_w(const struct lw_ctl_in *in, int i)
{
	return in->ncpus * in->opps[i].power_uw * 1e-6;
}

static uint32_t perf_of(const struct lw_ctl_in *in, int i)
{
	if (in->opps[i].perf)
		return in->opps[i].perf;
	return (uint32_t)((uint64_t)in->capacity * in->opps[i].khz / in->opps[in->nopps - 1].khz);
}

/* A cluster that saturates, at least in bursts, draws all it is given while busy. */
static bool plan_saturated(const struct lw_ctl_state *st)
{
	return st->mpc_peak_busy >= 0.9;
}

/* Mean power at OPP i: what heats the package. */
static double plan_mean_w(const struct lw_ctl_in *in, const struct lw_ctl_state *st, int i)
{
	struct lw_ctl_in w = *in;

	if (plan_saturated(st))
		return st->mpc_busy * full_power_w(in, i);
	/*
	 * busy_mean x cur_khz is the work done; average it rather than busy
	 * alone, which swings with the frequency schedutil picks.
	 */
	w.busy_mean = st->mpc_work_khz / in->cur_khz;
	return lw_ctl_est_power_w(&w, i);
}

/* Power while busy at OPP i: what the hot spot follows. */
static double plan_peak_w(const struct lw_ctl_in *in, const struct lw_ctl_state *st, int i)
{
	return plan_saturated(st) ? full_power_w(in, i) : plan_mean_w(in, st, i);
}

/*
 * Hottest the cluster's zone gets within horizon_s at OPP i: the recent peak,
 * moved by what the model says changes. Working from the measured peak cancels
 * any bias in the model's absolute temperatures.
 */
static double plan_temp_c(const struct lw_ctl_in *in, const struct lw_ctl_state *st, int i,
			  double horizon_s)
{
	double total = plan_mean_w(in, st, i) + st->mpc_other;

	return st->mpc_peak_c + lw_tm_package(&st->tm, st->pkg_c, total, horizon_s) - st->pkg_c +
	       lw_tm_peak_offset(&st->tm, plan_peak_w(in, st, i)) -
	       lw_tm_peak_offset(&st->tm, plan_peak_w(in, st, st->ceil_idx));
}

/* Change the ceiling, and expect the peak to move with the hot spot's offset. */
static void plan_move(const struct lw_ctl_in *in, struct lw_ctl_state *st, int i)
{
	st->mpc_peak_c += lw_tm_peak_offset(&st->tm, plan_peak_w(in, st, i)) -
			  lw_tm_peak_offset(&st->tm, plan_peak_w(in, st, st->ceil_idx));
	st->ceil_idx = i;
	st->ceiling_changes++;
}

static int lowest_idx_at_least(const struct lw_ctl_in *in, double khz)
{
	for (int i = 0; i < in->nopps; i++)
		if (in->opps[i].khz >= khz)
			return i;
	return in->nopps - 1;
}

static void ctl_step(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		 struct lw_ctl_state *st, struct lw_ctl_out *out,
		 uint64_t now_ns, double dt_s, bool joint)
{
	int top = in->nopps - 1;
	int min_idx = lowest_idx_at_least(in, p->min_ceiling_frac * in->opps[top].khz);
	double h, he, pmin, pmax, u, budget;
	bool cool_slow;
	int target;

	if (!st->init) {
		st->init = true;
		st->integ_w = full_power_w(in, top);	/* start uncapped */
		st->ceil_idx = top;
		st->last_down_ns = st->last_up_ns = 0;
		st->ceiling_changes = 0;
		lw_tm_init(&st->tm);
	}

	out->floor_khz = 0;
	out->resume_latency_us = -1;
	out->price_milli = 0;
	out->headroom_mc = 0;
	out->est_power_w = lw_ctl_est_power_w(in, lowest_idx_at_least(in, in->cur_khz));
	out->model_ready = false;
	out->joint_active = false;
	out->predicted_c = NAN;
	out->forecast_c = NAN;
	out->forecast_base_c = NAN;
	out->forecast_correction_c = 0;
	out->temporal_labels = 0;
	out->temporal_dropped = 0;
	out->forecast_source = -1;
	out->forecast_labels = 0;
	out->forecast_dropped = 0;
	for (int e = 0; e < LW_FORECAST_EXPERTS; e++)
		out->forecast_weights[e] = e == 0 ? 1 : 0;

	if (in->temp_mc == INT32_MIN) {
		/* No sensor: never cap blindly. */
		st->ceil_idx = top;
		out->budget_w = full_power_w(in, top);
		goto done;
	}

	if (!st->temp_init) {
		st->temp_f_mc = st->temp_s_mc = in->temp_mc;
		st->temp_init = true;
	} else {
		st->temp_f_mc += (in->temp_mc - st->temp_f_mc) *
				 (p->temp_filter_s > 0 ? clampd(dt_s / p->temp_filter_s, 0.0, 1.0) : 1.0);
		st->temp_s_mc += (in->temp_mc - st->temp_s_mc) *
				 (p->temp_slow_filter_s > 0 ?
				  clampd(dt_s / p->temp_slow_filter_s, 0.0, 1.0) : 1.0);
	}
	h = (p->soft_limit_mc - st->temp_f_mc) / 1000.0;
	cool_slow = (p->soft_limit_mc - st->temp_s_mc) / 1000.0 > p->deadband_c;
	he = fabs(h) <= p->deadband_c ? 0.0 : (h > 0 ? h - p->deadband_c : h + p->deadband_c);
	pmin = full_power_w(in, min_idx);
	pmax = full_power_w(in, top);

	/* PI with conditional integration: no wind-up at either rail. */
	u = p->kp_w_per_c * he + st->integ_w;
	if (!((u >= pmax && he > 0) || (u <= pmin && he < 0)))
		st->integ_w += p->ki_w_per_cs * he * dt_s;
	st->integ_w = clampd(st->integ_w, pmin, pmax);
	budget = clampd(p->kp_w_per_c * he + st->integ_w, pmin, pmax);
	out->budget_w = budget;

	target = min_idx;
	for (int i = top; i >= min_idx; i--) {
		double need = lw_ctl_est_power_w(in, i);

		/* Hysteresis: climbing needs a margin, holding does not. */
		if (i > st->ceil_idx)
			need /= 1.0 - p->up_margin;
		if (need <= budget) {
			target = i;
			break;
		}
	}

	/* A hot start or a fast ramp cannot wait for the EMA and one-second
	 * OPP descent. The same raw guard applies before either policy, including
	 * the model's training period. Retain the kernel's independent trips.
	 */
	if (in->temp_mc >= p->raw_limit_mc) {
		if (st->ceil_idx != min_idx) {
			st->ceil_idx = min_idx;
			st->ceiling_changes++;
		}
		st->integ_w = pmin;
		st->last_down_ns = now_ns;
		out->budget_w = pmin;
		goto priced;
	}

	if (p->policy == LW_POLICY_MPC && in->package_mc != INT32_MIN) {
		double limit = p->soft_limit_mc / 1000.0 + p->mpc_over_soft_c;
		bool down_ok = now_ns - st->last_down_ns >= (uint64_t)p->down_step_ms * 1000000ull;
		bool up_ok;
		double temp_c, slow = clampd(dt_s / p->mpc_load_filter_s, 0.0, 1.0);

		if (!st->mpc_busy_init) {
			st->mpc_busy = st->mpc_peak_busy = in->busy_mean;
			st->mpc_work_khz = in->busy_mean * in->cur_khz;
			st->mpc_other = in->other_power_w;
			st->pkg_c = in->package_mc / 1000.0;
			st->mpc_peak_c = st->temp_f_mc / 1000.0;
			st->mpc_up_wait_s = p->mpc_up_step_ms / 1000.0;
			st->mpc_busy_init = true;
		}
		up_ok = now_ns - st->last_up_ns >= (uint64_t)(st->mpc_up_wait_s * 1e9) &&
			now_ns - st->last_down_ns >= (uint64_t)(st->mpc_up_wait_s * 1e9);
		st->pkg_c += (in->package_mc / 1000.0 - st->pkg_c) *
			     clampd(dt_s / p->mpc_package_filter_s, 0.0, 1.0);
		/* A burst's raw peak outruns the filter: it counts, less the gap. */
		temp_c = fmax(st->temp_f_mc, in->temp_mc - p->mpc_raw_gap_c * 1000.0) / 1000.0;
		st->mpc_peak_c = fmax(temp_c, st->mpc_peak_c + (temp_c - st->mpc_peak_c) *
				      clampd(dt_s / p->mpc_peak_filter_s, 0.0, 1.0));
		lw_tm_observe(&st->tm, st->temp_f_mc / 1000.0, in->package_mc / 1000.0, out->est_power_w,
			      in->other_power_w, dt_s);
		/* Plan on load averaged over bursts, as the rule's slow filter does. */
		st->mpc_busy += (in->busy_mean - st->mpc_busy) * slow;
		st->mpc_work_khz += (in->busy_mean * in->cur_khz - st->mpc_work_khz) * slow;
		st->mpc_other += (in->other_power_w - st->mpc_other) * slow;
		st->mpc_peak_busy = fmax(in->busy_mean, st->mpc_peak_busy *
					 exp(-dt_s / p->mpc_peak_memory_s));
		out->model_ready = lw_tm_ready(&st->tm);
		if (out->model_ready) {
			double pred = plan_temp_c(in, st, st->ceil_idx, p->mpc_horizon_s);
			bool over = temp_c > limit;
			/* The zone filter still holds the heat of the last step down. */
			bool settled = now_ns - st->last_down_ns >=
				       (uint64_t)(3 * p->temp_filter_s * 1e9);

			out->predicted_c = pred;
			if (joint) {
				st->mpc_periods++;
				goto priced;
			}
			/*
			 * Down as soon as the current ceiling is predicted to cross the
			 * limit within the horizon, to the highest ceiling that is not,
			 * one OPP at a time if none is. Up one OPP only when the next
			 * one would stay under the limit for mpc_hold_s: the package's
			 * heat capacity pays for time above the sustainable frequency,
			 * and the hold is what keeps discrete OPPs from hunting.
			 */
			if ((pred > limit || (over && settled)) && down_ok && st->ceil_idx > min_idx) {
				int plan = st->ceil_idx - 1;

				/*
				 * Down soon after a climb means the plan was wrong for
				 * this load: wait longer before the next one.
				 */
				st->mpc_up_wait_s = now_ns - st->last_up_ns < (uint64_t)(p->mpc_hold_s * 1e9) ?
						    fmin(st->mpc_up_wait_s * 2, p->mpc_up_wait_max_s) :
						    p->mpc_up_step_ms / 1000.0;

				for (int i = plan; i >= min_idx; i--)
					if (plan_temp_c(in, st, i, p->mpc_horizon_s) <= limit) {
						plan = i;
						break;
					}
				plan_move(in, st, plan);
				st->last_down_ns = now_ns;
			} else if (st->ceil_idx < top && up_ok && !over &&
				   plan_temp_c(in, st, st->ceil_idx + 1, p->mpc_hold_s) <=
				   limit - p->mpc_margin_c) {
				plan_move(in, st, st->ceil_idx + 1);
				st->last_up_ns = now_ns;
			}
			st->mpc_periods++;
			goto priced;
		}
	}

	/*
	 * One OPP at a time; down only above the band, up only below it, and
	 * faster down than up.
	 */
	if (target < st->ceil_idx && he < 0 &&
	    now_ns - st->last_down_ns >= (uint64_t)p->down_step_ms * 1000000ull) {
		st->ceil_idx--;
		st->last_down_ns = now_ns;
		st->ceiling_changes++;
	} else if (target > st->ceil_idx && he > 0 && cool_slow &&
		   now_ns - st->last_up_ns >= (uint64_t)p->up_step_ms * 1000000ull &&
		   now_ns - st->last_down_ns >= (uint64_t)p->up_step_ms * 1000000ull) {
		st->ceil_idx++;
		st->last_up_ns = now_ns;
		st->ceiling_changes++;
	}
priced:
	if (p->policy == LW_POLICY_MPC) {
		double observed[2] = {
			in->package_mc == INT32_MIN ? NAN : in->package_mc / 1000.0,
			in->temp_mc / 1000.0,
		};
		double candidate[2][3] = {
			{ observed[0], NAN, NAN }, { observed[1], NAN, NAN },
		};

		if (in->package_mc != INT32_MIN && lw_tm_ready(&st->tm)) {
			candidate[0][1] = lw_tm_package(&st->tm, observed[0], st->tm.last_total_w, 5);
			candidate[1][1] = candidate[0][1] + lw_tm_offset(&st->tm, st->tm.last_own_w);
			candidate[0][2] = lw_tm_forecast_package(&st->tm, observed[0], 5);
			candidate[1][2] = lw_tm_forecast_hot(&st->tm, observed[0], 5);
		}
		lw_forecast_step(&st->forecaster, now_ns, observed, candidate);
		out->forecast_c = st->forecaster.prediction[LW_FORECAST_HOT];
		out->forecast_base_c = out->forecast_c;
		out->forecast_source = (int)st->forecaster.selected[LW_FORECAST_HOT];
		out->forecast_labels = st->forecaster.labels[LW_FORECAST_HOT][LW_FORECAST_PERSISTENCE];
		out->forecast_dropped = st->forecaster.dropped;
		if (p->temporal_forecast) {
			out->forecast_c = lw_temporal_step(&st->temporal, now_ns, observed[1],
							 out->forecast_base_c, candidate[1]);
			out->forecast_correction_c = st->temporal.correction_c;
			out->temporal_labels = st->temporal.labels;
			out->temporal_dropped = st->temporal.dropped;
			if (st->temporal.active)
				out->forecast_source = LW_FORECAST_TEMPORAL;
		}
		for (int e = 0; e < LW_FORECAST_EXPERTS; e++)
			out->forecast_weights[e] = st->forecaster.output_weight[LW_FORECAST_HOT][e];
	}
	if (st->ceil_idx < min_idx)
		st->ceil_idx = min_idx;

	out->headroom_mc = p->soft_limit_mc - in->temp_mc;
	if (p->price_ramp_c > 0)
		out->price_milli = (uint32_t)lround(1000.0 *
			clampd((in->temp_mc - (p->soft_limit_mc - p->price_ramp_c * 1000.0)) /
			       (p->price_ramp_c * 1000.0), 0.0, 1.0));

done:
	out->ceiling_khz = in->opps[st->ceil_idx].khz;
	out->ceiling_cap = perf_of(in, st->ceil_idx);
	out->floor_cap = 0;
	if (in->lat_crit >= 0 && in->lat_crit >= p->lat_crit_threshold) {
		int f = lowest_idx_at_least(in, 0.5 * in->opps[top].khz);

		if (f > st->ceil_idx)
			f = st->ceil_idx;
		out->floor_khz = in->opps[f].khz;
		out->floor_cap = perf_of(in, f);
		if (in->deep_idle_latency_us > 0)
			out->resume_latency_us = in->deep_idle_latency_us - 1;
	}
}

void lw_ctl_step(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		 struct lw_ctl_state *st, struct lw_ctl_out *out,
		 uint64_t now_ns, double dt_s)
{
	ctl_step(p, in, st, out, now_ns, dt_s, false);
}

/* No platform constants: enumerate only small products of discovered OPPs.
 * Larger topologies, missing sensors/EMs and untrained models keep the
 * independent controller. All inputs and model state are real HAL observations.
 */
void lw_ctl_step_group(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		       struct lw_ctl_state *st, struct lw_ctl_out *out, int n,
		       uint64_t now_ns, double dt_s)
{
	int lo[LW_MAX_POLICIES], idx[LW_MAX_POLICIES], best[LW_MAX_POLICIES];
	unsigned int combinations = 1;
	bool joint = p->cross_cluster && p->policy == LW_POLICY_MPC && n > 1 && n <= LW_MAX_POLICIES;
	double best_work = -1, best_power = INFINITY, best_excess = INFINITY;
	double limit = p->soft_limit_mc / 1000.0 + p->mpc_over_soft_c - p->mpc_margin_c;

	if (n < 1 || n > LW_MAX_POLICIES)
		return;
	for (int k = 0; k < n; k++) {
		int count;

		lo[k] = lowest_idx_at_least(&in[k], p->min_ceiling_frac * in[k].opps[in[k].nopps - 1].khz);
		count = in[k].nopps - lo[k];
		if (count < 1 || combinations > 65536u / (unsigned int)count)
			joint = false;
		else
			combinations *= (unsigned int)count;
		if (!lw_tm_ready(&st[k].tm) || in[k].temp_mc == INT32_MIN || in[k].package_mc == INT32_MIN)
			joint = false;
		for (int i = 0; i < in[k].nopps; i++)
			if (!in[k].opps[i].power_uw)
				joint = false;
	}
	for (int k = 0; k < n; k++) {
		ctl_step(p, &in[k], &st[k], &out[k], now_ns, dt_s, joint);
		out[k].joint_active = joint;
	}
	/* Keep an allocation for the same horizon that justified it. Re-solving
	 * every 10 s lets sensor codes move work back and forth between clusters.
	 * The common raw guard remains immediate during this hold.
	 */
	if (!joint || (st[0].last_group_ns &&
	    now_ns - st[0].last_group_ns < (uint64_t)(p->mpc_hold_s * 1e9)))
		return;
	st[0].last_group_ns = now_ns;
	for (int k = 0; k < n; k++)
		idx[k] = lo[k];
	for (unsigned int c = 0; c < combinations; c++) {
		double total = 0, work = 0, excess = 0;
		bool allowed = true;

		for (int k = 0; k < n; k++) {
			/* The shared raw guard cannot be undone by a joint plan. */
			if (in[k].temp_mc >= p->raw_limit_mc && idx[k] > lo[k])
				allowed = false;
			total += plan_mean_w(&in[k], &st[k], idx[k]);
			work += in[k].ncpus * (plan_saturated(&st[k]) ?
				st[k].mpc_busy * perf_of(&in[k], idx[k]) :
				fmin(st[k].mpc_work_khz / in[k].opps[in[k].nopps - 1].khz * in[k].capacity,
				     perf_of(&in[k], idx[k])));
		}
		for (int k = 0; k < n; k++) {
			double delta = lw_tm_peak_offset(&st[k].tm, plan_peak_w(&in[k], &st[k], idx[k])) -
				lw_tm_peak_offset(&st[k].tm, plan_peak_w(&in[k], &st[k], st[k].ceil_idx));
			double near = st[k].mpc_peak_c + delta +
				lw_tm_package(&st[k].tm, st[k].pkg_c, total, p->mpc_horizon_s) - st[k].pkg_c;
			double hold = st[k].mpc_peak_c + delta +
				lw_tm_package(&st[k].tm, st[k].pkg_c, total, p->mpc_hold_s) - st[k].pkg_c;

			excess = fmax(excess, fmax(near, hold) - limit);
		}
		/* Feasibility first. If no combination fits yet, reduce the largest
		 * predicted violation rather than taking an unsafe throughput winner.
		 */
		if (allowed && (excess < best_excess - 1e-6 ||
		    (fabs(excess - best_excess) <= 1e-6 &&
		     (work > best_work + 1e-6 || (fabs(work - best_work) <= 1e-6 && total < best_power))))) {
			best_excess = excess;
			best_work = work;
			best_power = total;
			for (int k = 0; k < n; k++)
				best[k] = idx[k];
		}
		for (int k = n - 1; k >= 0; k--) {
			if (++idx[k] < in[k].nopps)
				break;
			idx[k] = lo[k];
		}
	}
	if (best_work < 0)
		return;
	for (int k = 0; k < n; k++) {
		if (best[k] != st[k].ceil_idx) {
			if (best[k] < st[k].ceil_idx)
				st[k].last_down_ns = now_ns;
			else
				st[k].last_up_ns = now_ns;
			plan_move(&in[k], &st[k], best[k]);
		}
		out[k].ceiling_khz = in[k].opps[best[k]].khz;
		out[k].ceiling_cap = perf_of(&in[k], best[k]);
		out[k].budget_w = plan_mean_w(&in[k], &st[k], best[k]);
		out[k].predicted_c = st[k].mpc_peak_c +
			lw_tm_package(&st[k].tm, st[k].pkg_c, best_power, p->mpc_horizon_s) - st[k].pkg_c;
		if (out[k].floor_khz > out[k].ceiling_khz) {
			out[k].floor_khz = out[k].ceiling_khz;
			out[k].floor_cap = out[k].ceiling_cap;
		}
	}
}
