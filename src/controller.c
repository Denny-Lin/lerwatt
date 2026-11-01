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
	p->mpc_horizon_s = 20.0;
	p->mpc_margin_c = 1.0;
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

/*
 * Power the cluster would draw at OPP i. A saturated cluster's demand above
 * its ceiling is unknown, so assume it would use every cycle it is given.
 */
static double planned_power_w(const struct lw_ctl_in *in, double slow_busy, int i)
{
	return slow_busy >= 0.9 ? full_power_w(in, i) : lw_ctl_est_power_w(in, i);
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

	if (p->policy == LW_POLICY_MPC) {
		double t_c = st->temp_f_mc / 1000.0, limit = p->soft_limit_mc / 1000.0 - p->mpc_margin_c;
		bool down_ok = now_ns - st->last_down_ns >= (uint64_t)p->down_step_ms * 1000000ull;
		bool up_ok = now_ns - st->last_up_ns >= (uint64_t)p->mpc_up_step_ms * 1000000ull &&
			     now_ns - st->last_down_ns >= (uint64_t)p->mpc_up_step_ms * 1000000ull;

		lw_tm_observe(&st->tm, t_c, out->est_power_w, in->other_power_w, dt_s);
		/* Plan on load averaged over bursts, as the rule's slow filter does. */
		if (!st->mpc_busy_init) {
			st->mpc_busy = in->busy_mean;
			st->mpc_work_khz = in->busy_mean * in->cur_khz;
			st->mpc_other = in->other_power_w;
			st->mpc_busy_init = true;
		}
		st->mpc_busy += (in->busy_mean - st->mpc_busy) *
				clampd(dt_s / p->temp_slow_filter_s, 0.0, 1.0);
		st->mpc_work_khz += (in->busy_mean * in->cur_khz - st->mpc_work_khz) *
				    clampd(dt_s / p->temp_slow_filter_s, 0.0, 1.0);
		st->mpc_other += (in->other_power_w - st->mpc_other) *
				 clampd(dt_s / p->temp_slow_filter_s, 0.0, 1.0);
		out->model_ready = lw_tm_ready(&st->tm);
		if (out->model_ready) {
			struct lw_ctl_in plan_in = *in;
			double pred, t_plan = st->temp_s_mc / 1000.0;

			/*
			 * busy_mean x cur_khz is the work done; average it rather than
			 * busy alone, which swings with the frequency schedutil picks.
			 */
			plan_in.busy_mean = st->mpc_work_khz / in->cur_khz;
			plan_in.other_power_w = st->mpc_other;
			/* Bursts swing the hot spot; plan from the slow temperature. */
			pred = lw_tm_predict(&st->tm, t_plan, planned_power_w(&plan_in, st->mpc_busy, st->ceil_idx),
					     plan_in.other_power_w, p->mpc_horizon_s);
			out->predicted_c = pred;
			/*
			 * Down as soon as the current ceiling is predicted to cross the
			 * limit within the horizon, to the highest ceiling that is not.
			 * Up one OPP only when the next one is sustainable: its steady
			 * state stays under the limit. The gap between the two tests is
			 * the hysteresis that keeps discrete OPPs from hunting.
			 */
			if ((pred > limit || he < 0) && down_ok && st->ceil_idx > min_idx) {
				int plan = min_idx;

				for (int i = st->ceil_idx - 1; i >= min_idx; i--)
					if (lw_tm_predict(&st->tm, t_plan, planned_power_w(&plan_in, st->mpc_busy, i),
							  plan_in.other_power_w, p->mpc_horizon_s) <= limit) {
						plan = i;
						break;
					}
				st->ceil_idx = plan;
				st->last_down_ns = now_ns;
				st->ceiling_changes++;
			} else if (st->ceil_idx < top && up_ok && h > p->deadband_c && cool_slow &&
				   lw_tm_steady(&st->tm, planned_power_w(&plan_in, st->mpc_busy, st->ceil_idx + 1),
						plan_in.other_power_w) <= limit) {
				st->ceil_idx++;
				st->last_up_ns = now_ns;
				st->ceiling_changes++;
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
