/* SPDX-License-Identifier: MIT */
#ifndef LERWATT_FORECAST_H
#define LERWATT_FORECAST_H

#include <stdbool.h>
#include <stdint.h>

/* Point forecasts only. None of this state relaxes a control/safety limit. */
#define LW_FORECAST_CHANNELS 2
#define LW_FORECAST_EXPERTS 3
#define LW_FORECAST_CHOICES (LW_FORECAST_EXPERTS + 1)
#define LW_FORECAST_PENDING 64
#define LW_FORECAST_HORIZON_NS 5000000000ull

enum lw_forecast_channel { LW_FORECAST_PACKAGE, LW_FORECAST_HOT };
enum lw_forecast_expert {
	LW_FORECAST_PERSISTENCE, LW_FORECAST_CONSTANT, LW_FORECAST_CAUSAL, LW_FORECAST_BLEND
};

struct lw_forecast_record {
	uint64_t due_ns;
	double prediction[LW_FORECAST_CHANNELS][LW_FORECAST_CHOICES];
};

struct lw_forecaster {
	struct lw_forecast_record pending[LW_FORECAST_PENDING];
	unsigned int head, count;
	bool have_observation, have_issue;
	uint64_t last_observation_ns, last_issue_ns, dropped;
	double loss[LW_FORECAST_CHANNELS][LW_FORECAST_CHOICES];
	double weight[LW_FORECAST_CHANNELS][LW_FORECAST_EXPERTS];
	double output_weight[LW_FORECAST_CHANNELS][LW_FORECAST_EXPERTS];
	uint64_t labels[LW_FORECAST_CHANNELS][LW_FORECAST_CHOICES];
	uint64_t last_label_ns[LW_FORECAST_CHANNELS][LW_FORECAST_CHOICES];
	unsigned int selected[LW_FORECAST_CHANNELS];
	double prediction[LW_FORECAST_CHANNELS];
};

/* Mature old forecasts against the observations available now, then blend
 * and store a new five-second prediction. NaN candidates/labels are absent.
 * Expert 0 is the observed-temperature persistence baseline. No clock calls
 * or future observations are made inside this pure computation.
 */
void lw_forecast_step(struct lw_forecaster *f, uint64_t now_ns,
		      const double observed[LW_FORECAST_CHANNELS],
		      const double candidate[LW_FORECAST_CHANNELS][LW_FORECAST_EXPERTS]);

#endif
