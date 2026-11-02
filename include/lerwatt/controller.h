/* SPDX-License-Identifier: MIT */
/*
 * Lerwatt control law: rule baseline, online thermal MPC and joint planning.
 *
 * Pure computation: no I/O, no clocks, no globals. lerwattd feeds it from the
 * HAL; the closed-loop test feeds it straight from the simulator model. Both
 * therefore run exactly the same decision code.
 *
 * Per cluster, every period:
 *   1. headroom h = soft limit - filtered temperature, with a deadband wide
 *      enough to cover one OPP step plus one TSADC code
 *   2. power budget B = clamp(Kp*h + I, P_min_full, P_max_full), I += Ki*h*dt
 *      with conditional integration (no wind-up)
 *   3. ceiling = highest OPP whose estimated power at the current load fits B,
 *      moved one OPP at a time, at most every down_step_ms / up_step_ms, down
 *      when the fast-filtered temperature is above the band, up only when the
 *      fast and the slow (bursts averaged out) filters are both below it.
 *      With discrete OPPs this turns the unavoidable limit cycle into a slow
 *      one: safety reacts in a second, recovery waits for the heat to leave.
 *   4. energy price ramps 0..1000 over price_ramp_c below the soft limit
 *   5. latency-critical demand (from Lertick) raises the floor and blocks
 *      the deepest idle state
 */
#ifndef LERWATT_CONTROLLER_H
#define LERWATT_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "lerwatt/hal.h"
#include "lerwatt/learn.h"
#include "lerwatt/forecast.h"
#include "lerwatt/temporal.h"
#include "lerwatt/predict.h"

enum lw_ctl_policy {
	LW_POLICY_RULE = 0,	/* PI power budget with hysteresis (default) */
	LW_POLICY_MPC = 1,	/* learned thermal model, predictive ceiling; rule until trained */
};

struct lw_ctl_params {
	int32_t soft_limit_mc;		/* default: lowest passive trip - 5 degC */
	int32_t raw_limit_mc;		/* fast guard: passive trip - 3 C; independent of policy */
	double kp_w_per_c;		/* proportional gain, W per degC */
	double ki_w_per_cs;		/* integral gain, W per degC per second */
	double deadband_c;
	double temp_filter_s;		/* fast EMA: drives the budget and down-steps */
	double temp_slow_filter_s;	/* slow EMA: an up-step also needs it below the band */
	double up_margin;		/* step up only if the next OPP fits B*(1-margin) */
	uint32_t down_step_ms;		/* min time between ceiling decreases */
	uint32_t up_step_ms;		/* min time between ceiling increases */
	double min_ceiling_frac;	/* never cap below this share of max frequency */
	double price_ramp_c;
	double lat_crit_threshold;	/* 0..1 */
	enum lw_ctl_policy policy;
	bool cross_cluster;		/* opt-in bounded joint search; MPC only */
	bool cooperative_prediction;	/* opt-in action-supported correction */
	bool temporal_forecast;		/* point forecast only; never changes planning */
	double mpc_horizon_s;		/* look-ahead of the predictive ceiling */
	double mpc_over_soft_c;		/* line the plan holds, above the soft limit */
	double mpc_margin_c;		/* a climb must predict this far under the limit */
	double mpc_peak_memory_s;	/* how long a burst's load stays in the plan */
	double mpc_hold_s;		/* a climb must stay under the limit this long */
	double mpc_up_wait_max_s;	/* longest wait after climbs that failed */
	double mpc_load_filter_s;	/* averages the load the plan assumes, over bursts */
	double mpc_package_filter_s;	/* TSADC codes are coarse */
	double mpc_raw_gap_c;		/* an unfiltered reading may pass the line by this much */
	double mpc_peak_filter_s;	/* a recent peak fades this slowly: longer than bursts */
	uint32_t mpc_up_step_ms;	/* min time between predictive increases */
};

struct lw_ctl_in {
	int32_t temp_mc;		/* INT32_MIN when unknown */
	double busy_mean;		/* 0..1, mean over the cluster's CPUs */
	uint32_t cur_khz;
	uint32_t effective_max_khz;	/* 0 when unavailable; never assumed to be actual clock */
	uint32_t allowed_max_khz;	/* initial user/firmware bound, 0 when unavailable */
	enum lw_power_quality power_quality;
	int ncpus;
	const struct lw_opp *opps;	/* ascending */
	int nopps;
	uint32_t capacity;
	int32_t deep_idle_latency_us;	/* deepest idle state's latency, -1 if none */
	double lat_crit;		/* 0..1 from Lertick, <0 when no contract */
	double other_power_w;		/* estimated CPU power of the other clusters */
	int32_t package_mc;		/* package zone, INT32_MIN when there is none */
};

struct lw_ctl_state {
	bool init;
	bool temp_init;
	double temp_f_mc;		/* fast-filtered temperature */
	double temp_s_mc;		/* slow-filtered temperature */
	double integ_w;
	int ceil_idx;
	uint64_t last_down_ns;
	uint64_t last_up_ns;
	uint64_t ceiling_changes;
	struct lw_predictor predictor;	/* replaceable observation / prediction backend */
	uint64_t mpc_periods;		/* periods decided by the model */
	bool mpc_busy_init;
	double mpc_busy;		/* slow load average for planning */
	double mpc_work_khz;		/* slow average of busy x frequency */
	double mpc_other;		/* slow average of the other clusters' power */
	double mpc_peak_busy;		/* decaying peak of busy: bursts hit the ceiling */
	double mpc_up_wait_s;		/* wait before a climb, doubled by each failed one */
	double pkg_c;			/* filtered package temperature */
	double mpc_peak_c;		/* recent peak of the filtered zone temperature */
	uint64_t last_group_ns;
	uint64_t prediction_queries, prediction_corrections;
};

struct lw_ctl_out {
	uint32_t ceiling_khz;
	uint32_t floor_khz;
	int32_t resume_latency_us;	/* <0: no constraint */
	double budget_w;
	double est_power_w;
	uint32_t price_milli;
	int32_t headroom_mc;
	uint32_t ceiling_cap;
	uint32_t floor_cap;
	bool model_ready;
	bool joint_active;
	uint64_t prediction_queries, prediction_corrections;
	enum lw_prediction_reason prediction_reason;
	double prediction_nominal_c, prediction_upper_c;
	double predicted_c;		/* model's temperature at the horizon, NAN if none */
	double forecast_c;		/* expected 5 s point forecast; not a safety bound */
	double forecast_base_c;		/* unchanged delayed-feedback reference */
	double forecast_correction_c;	/* temporal residual; base weights still describe reference */
	uint64_t temporal_labels, temporal_dropped;
	int forecast_source;		/* lw_forecast_expert, -1 when unavailable */
	uint64_t forecast_labels;
	uint64_t forecast_dropped;
	double forecast_weights[LW_FORECAST_EXPERTS];
};

void lw_ctl_default_params(struct lw_ctl_params *p, int32_t lowest_passive_mc);
void lw_ctl_step(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		 struct lw_ctl_state *st, struct lw_ctl_out *out,
		 uint64_t now_ns, double dt_s);
/* Estimated CPU power of the cluster at OPP index i, given the current load. */
double lw_ctl_est_power_w(const struct lw_ctl_in *in, int i);
/* Observe a simultaneous sample, then optionally plan across all clusters. */
void lw_ctl_step_group(const struct lw_ctl_params *p, const struct lw_ctl_in *in,
		       struct lw_ctl_state *st, struct lw_ctl_out *out, int n,
		       uint64_t now_ns, double dt_s);

#endif /* LERWATT_CONTROLLER_H */
