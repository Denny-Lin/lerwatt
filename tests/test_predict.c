// SPDX-License-Identifier: MIT
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lerwatt/controller.h"

static struct lw_predictor learned(void)
{
	struct lw_predictor p = { 0 };
	lw_predictor_init(&p);
	/* A physically plausible but biased nominal model. No weights or quality
	 * flags are injected: the existing five-second LMS must learn the ramp. */
	p.thermal.samples = 100;
	p.thermal.theta[0] = .005;
	p.thermal.theta[1] = -.01;
	p.thermal.theta[2] = .6;
	p.thermal.offset_avg_c = p.thermal.power_avg_w = 1;
	p.thermal.peak_offset_c = p.thermal.peak_power_w = 1;
	for (uint64_t i = 0; i <= 1200; i++) {
		struct lw_prediction_observation o = {
			.now_ns = i * 100000000ull, .hot_c = 78.42 + i * .002,
			.thermal_hot_c = 78.42 + i * .002, .package_c = 60,
			.own_power_w = 1, .other_power_w = 2, .dt_s = .1,
			.observed_khz = 2400000, .ceiling_khz = 2400000,
			.temporal = true, .cooperative = true,
		};
		lw_predictor_observe(&p, &o);
		if (i < 50) assert(p.temporal.labels == 0);
	}
	assert(p.temporal.active && p.temporal.labels > 100);
	return p;
}

static struct lw_prediction_query query(void)
{
	return (struct lw_prediction_query) {
		.purpose = LW_PREDICT_CONTROL, .now_ns = 120000000000ull,
		.horizon_s = 5, .package_c = 60, .anchor_c = 80.82,
		.own_mean_w = 1, .total_mean_w = 3, .own_peak_w = 1, .reference_peak_w = 1,
		.candidate_khz = 2400000, .cooperative = true, .work_capacity = 1024,
		.power_quality = LW_POWER_ENERGY_MODEL,
	};
}

static void support_and_purity(void)
{
	struct lw_predictor p = learned(), before = p;
	struct lw_prediction_query q = query();
	struct lw_prediction_result r, repeated;
	assert(lw_predictor_predict(&p, &q, &r) && r.corrected && r.upper_c > r.nominal_c);
	assert(lw_predictor_predict(&p, &q, &repeated) && !memcmp(&r, &repeated, sizeof(r)));
	assert(!memcmp(&p, &before, sizeof(p))); /* enumeration cannot train or advance time */
	q.horizon_s = 120;
	assert(lw_predictor_predict(&p, &q, &r) && !r.corrected && r.reason == LW_PREDICT_HORIZON);
	q = query(); q.candidate_khz = 1800000;
	assert(lw_predictor_predict(&p, &q, &r) && !r.corrected && r.reason == LW_PREDICT_ACTION);
	q = query(); q.total_mean_w = 4;
	assert(lw_predictor_predict(&p, &q, &r) && !r.corrected && r.reason == LW_PREDICT_POWER);
	q = query(); q.now_ns += 1000000000ull;
	assert(lw_predictor_predict(&p, &q, &r) && !r.corrected && r.reason == LW_PREDICT_STALE);
	q = query(); q.own_mean_w = NAN;
	assert(!lw_predictor_predict(&p, &q, &r));
	q = query(); q.horizon_s = INFINITY;
	assert(!lw_predictor_predict(&p, &q, &r));
	q = query(); q.purpose = LW_PREDICT_POINT; q.horizon_s = 120;
	assert(!lw_predictor_predict(&p, &q, &r));
	q = query(); p.point_c = 20;
	assert(lw_predictor_predict(&p, &q, &r) && r.upper_c == r.nominal_c);
	p = learned();
	struct lw_prediction_observation o = p.observation;
	o.now_ns += 100000000ull; o.ceiling_khz = 1800000;
	lw_predictor_observe(&p, &o);
	assert(p.support.labels == 0 && p.temporal.labels > 100 && p.support.count == 1);
	q = query(); q.now_ns = o.now_ns;
	assert(lw_predictor_predict(&p, &q, &r) && !r.corrected);
	o.now_ns = 1;
	lw_predictor_observe(&p, &o);
	assert(p.support.since_ns == 1 && !p.support.labels && !p.temporal.labels);
	struct lw_predictor duplicate = p;
	lw_predictor_observe(&p, &o);
	assert(!memcmp(&duplicate, &p, sizeof(p)));
	o.now_ns++;
	o.hot_c = NAN;
	lw_predictor_observe(&p, &o);
	assert(!lw_predictor_ready(&p) && !p.support.initialized);
}

static void control_influence(void)
{
	struct lw_ctl_params params;
	struct lw_opp opp[] = {
		{ .khz = 1200000, .power_uw = 500000 },
		{ .khz = 1800000, .power_uw = 750000 },
		{ .khz = 2400000, .power_uw = 1000000 },
	};
	struct lw_ctl_in in = {
		.temp_mc = 80820, .package_mc = 60000, .busy_mean = 1,
		.cur_khz = 2400000, .effective_max_khz = 2400000,
		.ncpus = 1, .opps = opp, .nopps = 3, .capacity = 1024,
		.lat_crit = -1, .other_power_w = 2,
	};
	struct lw_ctl_state a = { 0 }, b;
	struct lw_ctl_out ao, bo;
	lw_ctl_default_params(&params, 85000); params.policy = LW_POLICY_MPC;
	lw_ctl_step(&params, &in, &a, &ao, 0, .1);
	a.predictor = learned();
	a.temp_f_mc = a.temp_s_mc = in.temp_mc;
	a.pkg_c = 60; a.mpc_peak_c = 80.82; a.mpc_other = 2;
	a.mpc_busy = a.mpc_peak_busy = 1; a.mpc_work_khz = 2400000;
	a.mpc_busy_init = true; a.ceil_idx = 2;
	b = a;
	uint64_t now = 120100000000ull;
	lw_ctl_step(&params, &in, &a, &ao, now, .1);
	params.cooperative_prediction = true;
	lw_ctl_step(&params, &in, &b, &bo, now, .1);
	assert(ao.ceiling_khz == 2400000 && bo.ceiling_khz < ao.ceiling_khz);
	assert(bo.prediction_corrections && bo.prediction_upper_c > bo.prediction_nominal_c);
	/* The learned result changes a real MPC decision, not only telemetry.
	 * The independent raw guard must still dominate it on the next period. */
	in.temp_mc = params.raw_limit_mc; in.lat_crit = 1;
	lw_ctl_step(&params, &in, &b, &bo, now + 100000000ull, .1);
	assert(bo.ceiling_khz == 1200000 && bo.floor_khz <= bo.ceiling_khz);
	printf("control influence: original %u, cooperative %u, raw guard %u kHz\n",
	       ao.ceiling_khz, opp[1].khz, bo.ceiling_khz);
	/* Initial user limits remain outside the model's authority. */
	b = a; in.temp_mc = 80820; in.allowed_max_khz = 1800000;
	lw_ctl_step(&params, &in, &b, &bo, now + 200000000ull, .1);
	assert(bo.ceiling_khz <= in.allowed_max_khz && bo.floor_khz <= bo.ceiling_khz);
}

static void counterfactual_oracle(void)
{
	struct lw_predictor p = learned();
	struct lw_prediction_query q = query();
	struct lw_prediction_result r;
	double previous = -INFINITY;
	q.cooperative = false;
	for (int i = 0; i < 3; i++) {
		q.own_mean_w = q.own_peak_w = .5 + .25 * i;
		q.total_mean_w = 2 + q.own_mean_w;
		q.candidate_khz = 1200000 + i * 600000;
		assert(lw_predictor_predict(&p, &q, &r));
		/* Independent numerical integration of the synthetic plant, not the
		 * analytic exponential used by the implementation. */
		double package = 60;
		for (int tick = 0; tick < 50000; tick++)
			package += .0001 * (.005 * q.total_mean_w - .01 * package + .6);
		double expected = q.anchor_c + package - 60 + q.own_peak_w - 1;
		assert(fabs(r.nominal_c - expected) < 1e-6 && r.nominal_c > previous);
		assert(r.power_quality == LW_POWER_ENERGY_MODEL && r.cpu_energy_j == r.cpu_power_w * q.horizon_s);
		previous = r.nominal_c;
	}
	q.power_quality = LW_POWER_PROXY;
	assert(lw_predictor_predict(&p, &q, &r) && !r.energy_valid && r.cpu_energy_j == 0);
}

struct alternative { unsigned int observations; bool invalid, local_only; };
static void alternative_observe(void *ctx, const struct lw_prediction_observation *o)
{
	(void)o; ((struct alternative *)ctx)->observations++;
}
static bool alternative_ready(const void *ctx) { return ((const struct alternative *)ctx)->observations > 0; }
static bool alternative_predict(const void *ctx, const struct lw_prediction_query *q,
				struct lw_prediction_result *r)
{
	if (((const struct alternative *)ctx)->local_only && q->total_mean_w > 1.1)
		return false;
	*r = (struct lw_prediction_result) {
		.nominal_c = 70, .mean_c = 70, .upper_c = ((const struct alternative *)ctx)->invalid ? NAN : 70,
		.cpu_power_w = q->own_mean_w, .cpu_energy_j = q->own_mean_w * q->horizon_s,
		.work_capacity = q->work_capacity,
	};
	return true;
}
static void replaceable_backend(void)
{
	const struct lw_prediction_ops ops = { LW_PREDICT_API, alternative_observe, alternative_ready, alternative_predict };
	struct alternative context = { 0 };
	struct lw_predictor p = { 0 };
	struct lw_prediction_result r;
	struct lw_prediction_query q = query();
	assert(lw_predictor_bind(&p, &ops, &context));
	lw_predictor_init(&p);
	struct lw_prediction_observation o = { .hot_c = 70, .package_c = 60, .observed_khz = 2400000, .dt_s = .1 };
	lw_predictor_observe(&p, &o);
	assert(lw_predictor_ready(&p) && lw_predictor_predict(&p, &q, &r) && r.upper_c == 70);
	assert(p.point_c == 70 && context.observations == 1);
	context.invalid = true;
	assert(!lw_predictor_predict(&p, &q, &r));
	struct lw_prediction_ops bad = ops; bad.api++;
	assert(!lw_predictor_bind(&p, &bad, &context));
	/* The alternative implementation is called by MPC without changing the
	 * planner. Invalid model output really falls back, rather than winning. */
	struct lw_ctl_params params; lw_ctl_default_params(&params, 85000); params.policy = LW_POLICY_MPC;
	struct lw_opp opp[] = { { .khz = 1200000, .power_uw = 500000 }, { .khz = 2400000, .power_uw = 1000000 } };
	struct lw_ctl_in in = { .temp_mc = 75000, .package_mc = 60000, .cur_khz = 2400000,
		.busy_mean = 1, .ncpus = 1, .nopps = 2, .opps = opp, .capacity = 1024, .lat_crit = -1 };
	struct lw_ctl_state st = { 0 }; struct lw_ctl_out out;
	context.invalid = false;
	assert(lw_predictor_bind(&st.predictor, &ops, &context));
	lw_ctl_step(&params, &in, &st, &out, 100000000000ull, .1);
	assert(out.model_ready && st.mpc_periods == 1);
	context.invalid = true;
	lw_ctl_step(&params, &in, &st, &out, 100100000000ull, .1);
	assert(!out.model_ready && st.mpc_periods == 1 && out.prediction_reason == LW_PREDICT_INVALID);
	context.invalid = false; context.local_only = true;
	params.cross_cluster = true;
	struct lw_ctl_state states[3] = { 0 }; struct lw_ctl_in inputs[3]; struct lw_ctl_out outputs[3];
	for (int k = 0; k < 3; k++) {
		inputs[k] = in;
		assert(lw_predictor_bind(&states[k].predictor, &ops, &context));
	}
	lw_ctl_step_group(&params, inputs, states, outputs, 3, 200000000000ull, .1);
	lw_ctl_step_group(&params, inputs, states, outputs, 3, 200100000000ull, .1);
	for (int k = 0; k < 3; k++)
		assert(!outputs[k].joint_active && !outputs[k].model_ready &&
		       outputs[k].prediction_reason == LW_PREDICT_INVALID);
}

int main(void)
{
	support_and_purity(); counterfactual_oracle(); control_influence(); replaceable_backend();
	printf("test_predict: action support, delayed learning, pure queries, backend replacement and MPC influence passed (%zu bytes/predictor)\n",
	       sizeof(struct lw_predictor));
	return 0;
}
