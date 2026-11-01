/* SPDX-License-Identifier: MIT */
/*
 * Online thermal model, learned per cluster while Lerwatt runs.
 *
 *   dT/dt = a * (P_own + P_other) + c * T + d
 *
 * T is the cluster's filtered zone temperature (degC), P_own its estimated CPU
 * power (W, from the Energy Model and measured busy time) and P_other the rest
 * of the SoC's (W). All clusters share one package and usually load together,
 * so their separate contributions cannot be told apart from normal operation;
 * the model uses their sum. c < 0 is the cooling rate;
 * -d/c is the temperature with no CPU power, so ambient and board heat are
 * learned too. Recursive least squares with forgetting tracks slow changes
 * (a fan, a case, the room) without a calibration run.
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
	double prev_t, prev_own, prev_other;
	double acc_s, acc_energy_j;	/* the open 1 s learning interval */
};

void lw_tm_init(struct lw_thermal_model *m);
/* One sample: temperature now, and the power that acted since the last one. */
void lw_tm_observe(struct lw_thermal_model *m, double temp_c, double p_own_w,
		   double p_other_w, double dt_s);
/* Physically plausible and trained on enough samples to act on. */
bool lw_tm_ready(const struct lw_thermal_model *m);
/* Temperature after horizon_s at constant power (first-order solution). */
double lw_tm_predict(const struct lw_thermal_model *m, double temp_c, double p_own_w,
		     double p_other_w, double horizon_s);
/* Temperature the cluster settles at with constant power. */
double lw_tm_steady(const struct lw_thermal_model *m, double p_own_w, double p_other_w);

#endif /* LERWATT_LEARN_H */
