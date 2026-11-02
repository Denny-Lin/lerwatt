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
	p->cooperative_prediction = false;
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
static bool plan_prediction(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
			    const struct lw_ctl_state *st, int i, double total,
			    double horizon_s, struct lw_prediction_result *r)
{
	struct lw_prediction_query q = {
		.purpose = LW_PREDICT_CONTROL, .now_ns = st->predictor.observation.now_ns,
		.horizon_s = horizon_s, .package_c = st->pkg_c, .anchor_c = st->mpc_peak_c,
		.own_mean_w = plan_mean_w(in, st, i), .total_mean_w = total,
		.own_peak_w = plan_peak_w(in, st, i),
		.reference_peak_w = plan_peak_w(in, st, st->ceil_idx),
		.candidate_khz = in->opps[i].khz, .cooperative = p->cooperative_prediction,
		.work_capacity = in->ncpus * (plan_saturated(st) ? st->mpc_busy * perf_of(in, i) :
			fmin(st->mpc_work_khz / in->opps[in->nopps - 1].khz * in->capacity, perf_of(in, i))),
		.power_quality = in->power_quality,
	};
	return lw_predictor_predict(&st->predictor, &q, r);
}

static double plan_temp_c(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
			 struct lw_ctl_state *st, int i, double horizon_s)
{
	struct lw_prediction_result r;
	st->prediction_queries++;
	if (!plan_prediction(p, in, st, i, plan_mean_w(in, st, i) + st->mpc_other, horizon_s, &r))
		return INFINITY;
	st->prediction_corrections += r.corrected;
	return r.upper_c;
}

/* Change the ceiling, and expect the peak to move with the hot spot's offset. */
static void plan_move(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		      struct lw_ctl_state *st, int i)
{
	struct lw_prediction_result r;
	if (plan_prediction(p, in, st, i, plan_mean_w(in, st, i) + st->mpc_other, p->mpc_horizon_s, &r))
		st->mpc_peak_c += r.hot_delta_c;
	st->ceil_idx = i;
	st->ceiling_changes++;
}

static void observe_prediction(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
			       struct lw_ctl_state *st, const struct lw_ctl_out *out,
			       uint64_t now_ns, double dt_s, bool thermal_update)
{
	struct lw_prediction_observation o = {
		.now_ns = now_ns, .hot_c = in->temp_mc / 1000.0,
		.thermal_hot_c = st->temp_f_mc / 1000.0,
		.package_c = in->package_mc == INT32_MIN ? NAN : in->package_mc / 1000.0,
		.own_power_w = out->est_power_w, .other_power_w = in->other_power_w, .dt_s = dt_s,
		.observed_khz = in->cur_khz,
		.ceiling_khz = p->cooperative_prediction ? in->effective_max_khz : in->opps[st->ceil_idx].khz,
		.temporal = p->temporal_forecast, .cooperative = p->cooperative_prediction,
		.thermal_update = thermal_update,
	};
	lw_predictor_observe(&st->predictor, &o);
}

static void prediction_outputs(const struct lw_ctl_state *st, struct lw_ctl_out *out)
{
	out->forecast_c = st->predictor.point_c;
	out->forecast_base_c = st->predictor.base_c;
	out->forecast_correction_c = st->predictor.point_correction_c;
	out->forecast_source = st->predictor.point_source;
	out->forecast_labels = st->predictor.forecaster.labels[LW_FORECAST_HOT][LW_FORECAST_PERSISTENCE];
	out->forecast_dropped = st->predictor.forecaster.dropped;
	out->temporal_labels = st->predictor.temporal.labels;
	out->temporal_dropped = st->predictor.temporal.dropped;
	out->prediction_queries = st->prediction_queries;
	out->prediction_corrections = st->prediction_corrections;
	for (int e = 0; e < LW_FORECAST_EXPERTS; e++)
		out->forecast_weights[e] = st->predictor.forecaster.output_weight[LW_FORECAST_HOT][e];
}

static int lowest_idx_at_least(const struct lw_ctl_in *in, double khz)
{
	for (int i = 0; i < in->nopps; i++)
		if (in->opps[i].khz >= khz)
			return i;
	return in->nopps - 1;
}

static int available_top(const struct lw_ctl_params *p, const struct lw_ctl_in *in)
{
	int top = in->nopps - 1;
	if (p->cooperative_prediction && in->allowed_max_khz)
		while (top > 0 && in->opps[top].khz > in->allowed_max_khz)
			top--;
	return top;
}

static void ctl_step(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		 struct lw_ctl_state *st, struct lw_ctl_out *out,
		 uint64_t now_ns, double dt_s, bool joint)
{
	int top = available_top(p, in);
	int min_idx = lowest_idx_at_least(in, p->min_ceiling_frac * in->opps[top].khz);
	double h, he, pmin, pmax, u, budget;
	bool cool_slow;
	int target;
	bool prediction_observed = false;
	if (min_idx > top)
		min_idx = top;

	if (!st->init) {
		st->init = true;
		st->integ_w = full_power_w(in, top);	/* start uncapped */
		st->ceil_idx = top;
		st->last_down_ns = st->last_up_ns = 0;
		st->ceiling_changes = 0;
		lw_predictor_init(&st->predictor);
	}
	if (st->ceil_idx > top)
		st->ceil_idx = top;

	out->floor_khz = 0;
	out->resume_latency_us = -1;
	out->price_milli = 0;
	out->headroom_mc = 0;
	out->est_power_w = lw_ctl_est_power_w(in, lowest_idx_at_least(in, in->cur_khz));
	out->model_ready = false;
	out->joint_active = false;
	out->predicted_c = NAN;
	out->prediction_queries = out->prediction_corrections = 0;
	out->prediction_reason = LW_PREDICT_NOMINAL;
	out->prediction_nominal_c = out->prediction_upper_c = NAN;
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
		/* Preserve the original filtered thermal label and raw point label. */
		observe_prediction(p, in, st, out, now_ns, dt_s, true);
		prediction_observed = true;
		/* Plan on load averaged over bursts, as the rule's slow filter does. */
		st->mpc_busy += (in->busy_mean - st->mpc_busy) * slow;
		st->mpc_work_khz += (in->busy_mean * in->cur_khz - st->mpc_work_khz) * slow;
		st->mpc_other += (in->other_power_w - st->mpc_other) * slow;
		st->mpc_peak_busy = fmax(in->busy_mean, st->mpc_peak_busy *
					 exp(-dt_s / p->mpc_peak_memory_s));
		out->model_ready = lw_predictor_ready(&st->predictor);
		if (out->model_ready) {
			double pred = plan_temp_c(p, in, st, st->ceil_idx, p->mpc_horizon_s);
			bool over = temp_c > limit;
			/* The zone filter still holds the heat of the last step down. */
			bool settled = now_ns - st->last_down_ns >=
				       (uint64_t)(3 * p->temp_filter_s * 1e9);

			struct lw_prediction_result decision;
			if (!isfinite(pred) || !plan_prediction(p, in, st, st->ceil_idx,
				plan_mean_w(in, st, st->ceil_idx) + st->mpc_other, p->mpc_hold_s, &decision)) {
				out->model_ready = false;
				out->prediction_reason = LW_PREDICT_INVALID;
				goto rule;
			}
			out->predicted_c = pred;
			if (plan_prediction(p, in, st, st->ceil_idx,
				plan_mean_w(in, st, st->ceil_idx) + st->mpc_other, p->mpc_horizon_s, &decision)) {
				out->prediction_reason = decision.reason;
				out->prediction_nominal_c = decision.nominal_c;
				out->prediction_upper_c = decision.upper_c;
			}
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
					if (plan_temp_c(p, in, st, i, p->mpc_horizon_s) <= limit) {
						plan = i;
						break;
					}
				plan_move(p, in, st, plan);
				st->last_down_ns = now_ns;
			} else if (st->ceil_idx < top && up_ok && !over &&
				   plan_temp_c(p, in, st, st->ceil_idx + 1, p->mpc_hold_s) <=
				   limit - p->mpc_margin_c) {
				plan_move(p, in, st, st->ceil_idx + 1);
				st->last_up_ns = now_ns;
			}
			st->mpc_periods++;
			goto priced;
		}
	}

rule:
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
		if (!prediction_observed)
			observe_prediction(p, in, st, out, now_ns, dt_s, false);
		prediction_outputs(st, out);
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
	double best_work = -1, best_power = INFINITY, best_total = 0, best_excess = INFINITY;
	double limit = p->soft_limit_mc / 1000.0 + p->mpc_over_soft_c - p->mpc_margin_c;

	if (n < 1 || n > LW_MAX_POLICIES)
		return;
	for (int k = 0; k < n; k++) {
		int count;

		int top = available_top(p, &in[k]);
		lo[k] = lowest_idx_at_least(&in[k], p->min_ceiling_frac * in[k].opps[top].khz);
		if (lo[k] > top)
			lo[k] = top;
		count = top + 1 - lo[k];
		if (count < 1 || combinations > 65536u / (unsigned int)count)
			joint = false;
		else
			combinations *= (unsigned int)count;
		if (!lw_predictor_ready(&st[k].predictor) || in[k].temp_mc == INT32_MIN || in[k].package_mc == INT32_MIN)
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
		double total = 0, work = 0, predicted_power = 0, excess = 0;
		bool allowed = true;

		for (int k = 0; k < n; k++) {
			/* The shared raw guard cannot be undone by a joint plan. */
			if (in[k].temp_mc >= p->raw_limit_mc && idx[k] > lo[k])
				allowed = false;
			total += plan_mean_w(&in[k], &st[k], idx[k]);
		}
		for (int k = 0; k < n; k++) {
			struct lw_prediction_result near_result, hold_result;
			st[k].prediction_queries += 2;
			bool valid = plan_prediction(p, &in[k], &st[k], idx[k], total, p->mpc_horizon_s, &near_result) &&
				plan_prediction(p, &in[k], &st[k], idx[k], total, p->mpc_hold_s, &hold_result);
			double near = valid ? near_result.upper_c : INFINITY;
			double hold = valid ? hold_result.upper_c : INFINITY;
			if (!valid)
				allowed = false;
			else
				st[k].prediction_corrections += near_result.corrected + hold_result.corrected;
			if (valid) {
				work += near_result.work_capacity;
				predicted_power += near_result.cpu_power_w;
			}
			excess = fmax(excess, fmax(near, hold) - limit);
		}
		/* Feasibility first. If no combination fits yet, reduce the largest
		 * predicted violation rather than taking an unsafe throughput winner.
		 */
		if (allowed && (excess < best_excess - 1e-6 ||
		    (fabs(excess - best_excess) <= 1e-6 &&
		     (work > best_work + 1e-6 || (fabs(work - best_work) <= 1e-6 && predicted_power < best_power))))) {
			best_excess = excess;
			best_work = work;
			best_power = predicted_power;
			best_total = total;
			for (int k = 0; k < n; k++)
				best[k] = idx[k];
		}
		for (int k = n - 1; k >= 0; k--) {
			if (++idx[k] <= available_top(p, &in[k]))
				break;
			idx[k] = lo[k];
		}
	}
	if (best_work < 0) {
		/* A replacement may support independent predictions but reject every
		 * coupled candidate. Do not report a live joint plan or hold a model-
		 * selected action indefinitely. Re-evaluate the existing rule with
		 * dt=0: no duplicate filtering, integration, learning or labels. */
		struct lw_ctl_params fallback = *p;
		fallback.policy = LW_POLICY_RULE;
		for (int k = 0; k < n; k++) {
			if (out[k].model_ready && st[k].mpc_periods) st[k].mpc_periods--;
			ctl_step(&fallback, &in[k], &st[k], &out[k], now_ns, 0, false);
			prediction_outputs(&st[k], &out[k]);
			out[k].prediction_reason = LW_PREDICT_INVALID;
		}
		return;
	}
	for (int k = 0; k < n; k++) {
		if (best[k] != st[k].ceil_idx) {
			if (best[k] < st[k].ceil_idx)
				st[k].last_down_ns = now_ns;
			else
				st[k].last_up_ns = now_ns;
			plan_move(p, &in[k], &st[k], best[k]);
		}
		out[k].ceiling_khz = in[k].opps[best[k]].khz;
		out[k].ceiling_cap = perf_of(&in[k], best[k]);
		out[k].budget_w = plan_mean_w(&in[k], &st[k], best[k]);
		struct lw_prediction_result selected;
		if (plan_prediction(p, &in[k], &st[k], best[k], best_total, p->mpc_horizon_s, &selected)) {
			out[k].budget_w = selected.cpu_power_w;
			out[k].predicted_c = selected.upper_c;
			out[k].prediction_nominal_c = selected.nominal_c;
			out[k].prediction_upper_c = selected.upper_c;
			out[k].prediction_reason = selected.reason;
		}
		out[k].prediction_queries = st[k].prediction_queries;
		out[k].prediction_corrections = st[k].prediction_corrections;
		if (out[k].floor_khz > out[k].ceiling_khz) {
			out[k].floor_khz = out[k].ceiling_khz;
			out[k].floor_cap = out[k].ceiling_cap;
		}
	}
}
