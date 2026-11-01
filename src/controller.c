// SPDX-License-Identifier: MIT
/* Lerwatt control law. See include/lerwatt/controller.h. */
#include <limits.h>
#include <math.h>

#include "lerwatt/controller.h"

void lw_ctl_default_params(struct lw_ctl_params *p, int32_t lowest_passive_mc)
{
	p->soft_limit_mc = (lowest_passive_mc != INT32_MAX ? lowest_passive_mc : 85000) - 5000;
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
	p->mpc_horizon_s = 5.0;
	p->mpc_over_soft_c = 2.0;
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

void lw_ctl_step(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		 struct lw_ctl_state *st, struct lw_ctl_out *out,
		 uint64_t now_ns, double dt_s)
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
	out->predicted_c = NAN;

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
