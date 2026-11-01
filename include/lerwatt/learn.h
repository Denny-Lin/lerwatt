/* SPDX-License-Identifier: MIT */
/*
 * Online two-node thermal model, learned per cluster while Lerwatt runs.
 *
 *   package:  dT_pkg/dt = a * (P_own + P_other) + c * T_pkg + d
 *   hot spot: T_hot = T_pkg + r * P_own
 *
 * T_pkg comes from the package zone and T_hot from the cluster's zone; P_own
 * is the cluster's estimated CPU power (W, from the Energy Model and measured
 * busy time) and P_other the rest of the SoC's. The package is the slow node
 * (hundreds of seconds): all clusters share it and usually load together, so
 * it sees only their sum. c < 0 is the cooling rate and -d/c the temperature
 * with no CPU power, so ambient and board heat are learned too. Recursive
 * least squares with forgetting tracks slow changes (a fan, a case, the room)
 * without a calibration run.
 *
 * The hot spot settles in about a second, so to the planner it is an offset
 * above the package. r is the DC gain from power to that offset: the ratio of
 * long averages, which holds whatever the shape of the load, and which needs
 * no power steps to be observable. Bursts reach their peak offset only if they
 * outlast the hot spot's time constant, so peak_gain learns the share of
 * r * P_peak that recent peaks actually reached.
 *
 * Pure computation, like controller.c: the same code runs in lerwattd and in
 * the closed-loop test.
 */
#ifndef LERWATT_LEARN_H
#define LERWATT_LEARN_H

#include <stdbool.h>
#include <stdint.h>

#define LW_TM_N 3

struct lw_thermal_model {
	double theta[LW_TM_N];		/* a, c, d */
	double cov[LW_TM_N][LW_TM_N];
	double lambda;			/* forgetting factor per sample */
	uint64_t samples;
	double resid_c_per_s;		/* EWMA of |prediction error| of dT/dt */
	bool have_prev;
	bool have_mean;
	double prev_mean_w, prev_mean_c;	/* the last closed interval */
	double acc_s, acc_energy_j, acc_pkg;	/* the open learning interval */
	double offset_avg_c, power_avg_w;	/* long averages behind r */
	double hot_s;			/* time those averages cover */
	double peak_offset_c, peak_power_w;	/* decaying peaks behind peak_gain */
};

void lw_tm_init(struct lw_thermal_model *m);
/* One sample: both nodes now, and the power that acted since the last one. */
void lw_tm_observe(struct lw_thermal_model *m, double hot_c, double pkg_c, double p_own_w,
		   double p_other_w, double dt_s);
/* Physically plausible and trained on enough samples to act on. */
bool lw_tm_ready(const struct lw_thermal_model *m);
/* Package temperature after horizon_s at constant total power. */
double lw_tm_package(const struct lw_thermal_model *m, double pkg_c, double p_total_w,
		     double horizon_s);
/* Package temperature it settles at with constant total power. */
double lw_tm_package_steady(const struct lw_thermal_model *m, double p_total_w);
/* Hot-spot offset above the package, in steady state at p_own_w. */
double lw_tm_offset(const struct lw_thermal_model *m, double p_own_w);
/* Hot-spot peak above the package for bursts drawing p_own_w. */
double lw_tm_peak_offset(const struct lw_thermal_model *m, double p_own_w);

#endif /* LERWATT_LEARN_H */
