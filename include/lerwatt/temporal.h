/* SPDX-License-Identifier: MIT */
#ifndef LERWATT_TEMPORAL_H
#define LERWATT_TEMPORAL_H

#include <stdbool.h>
#include <stdint.h>

#define LW_TEMPORAL_FEATURES 7
#define LW_TEMPORAL_PENDING 64
#define LW_TEMPORAL_HISTORY 128

struct lw_temporal_record {
	uint64_t due_ns;
	double x[LW_TEMPORAL_FEATURES], reference, prediction;
};

struct lw_temporal_sample {
	uint64_t t_ns;
	double hot_c;
};

/* Pure point-forecast correction. Never a control or safety upper bound. */
struct lw_temporal {
	struct lw_temporal_record pending[LW_TEMPORAL_PENDING];
	struct lw_temporal_sample history[LW_TEMPORAL_HISTORY];
	unsigned int head, count, history_head, history_count;
	bool have_observation, have_issue, active;
	uint64_t last_observation_ns, last_issue_ns, last_label_ns, labels, dropped;
	double weight[LW_TEMPORAL_FEATURES];
	double loss, reference_loss, difference_squared;
	double prediction_c, correction_c, advantage, margin;
};

/* physical[0] is persistence, physical[1:2] are the existing constant and
 * causal model forecasts; missing physical experts may be NaN. reference_c
 * is the deployed delayed-feedback forecast, captured before its future label.
 */
double lw_temporal_step(struct lw_temporal *t, uint64_t now_ns, double hot_c,
			double reference_c, const double physical[3]);

#endif
