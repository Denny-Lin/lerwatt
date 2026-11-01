// SPDX-License-Identifier: MIT
#include <math.h>
#include <string.h>

#include "lerwatt/forecast.h"

#define SAMPLE_NS 100000000ull
#define MAX_LATENESS_NS 250000000ull
#define LOSS_MEMORY_S 30.0
/* Limit one update by the number of outstanding feedback steps. A larger
 * per-sample step can chase opposite phases of a periodic workload while
 * its labels are still five seconds behind the current forecast.
 */
#define LEARNING_RATE (SAMPLE_NS / (double)(LW_FORECAST_HORIZON_NS + SAMPLE_NS))
#define NOISE_FLOOR_SQUARED .25

/* Euclidean projection of the two non-baseline weights onto w>=0, sum(w)<=1.
 * The remaining weight belongs to persistence, keeping predictions inside
 * the range of the finite experts even during load changes.
 */
static void project(double *w)
{
	w[1] = fmax(0, w[1]);
	w[2] = fmax(0, w[2]);
	if (w[1] + w[2] > 1) {
		double shift = (w[1] + w[2] - 1) / 2;

		w[1] -= shift;
		w[2] -= shift;
		if (w[1] < 0) {
			w[1] = 0;
			w[2] = 1;
		} else if (w[2] < 0) {
			w[1] = 1;
			w[2] = 0;
		}
	}
	w[0] = 1 - w[1] - w[2];
}

void lw_forecast_step(struct lw_forecaster *f, uint64_t now_ns,
		      const double observed[LW_FORECAST_CHANNELS],
		      const double candidate[LW_FORECAST_CHANNELS][LW_FORECAST_EXPERTS])
{
	struct lw_forecast_record matured;
	bool have_matured = false;
	double learned[LW_FORECAST_CHANNELS];

	if (f->have_observation && now_ns < f->last_observation_ns)
		memset(f, 0, sizeof(*f));
	f->have_observation = true;
	f->last_observation_ns = now_ns;
	/* If sampling was delayed, use the closest expired forecast once rather
	 * than counting many old forecasts against the same current label.
	 */
	while (f->count && f->pending[f->head].due_ns <= now_ns) {
		if (have_matured)
			f->dropped++;
		matured = f->pending[f->head];
		have_matured = true;
		f->head = (f->head + 1) % LW_FORECAST_PENDING;
		f->count--;
	}
	if (have_matured && now_ns - matured.due_ns > MAX_LATENESS_NS) {
		f->dropped++;
		have_matured = false;
	}
	if (have_matured) {
		for (int c = 0; c < LW_FORECAST_CHANNELS; c++) {
			if (isfinite(observed[c]) && isfinite(matured.prediction[c][0])) {
				double delta[LW_FORECAST_EXPERTS] = { 0 };
				double pred = matured.prediction[c][0], norm = NOISE_FLOOR_SQUARED;

				for (int e = 1; e < LW_FORECAST_EXPERTS; e++) {
					if (isfinite(matured.prediction[c][e]))
						delta[e] = matured.prediction[c][e] - matured.prediction[c][0];
					pred += f->weight[c][e] * delta[e];
					norm += delta[e] * delta[e];
				}
				if (isfinite(pred) && isfinite(norm)) {
					double error = observed[c] - pred;

					for (int e = 1; e < LW_FORECAST_EXPERTS; e++)
						f->weight[c][e] += LEARNING_RATE * error * delta[e] / norm;
					project(f->weight[c]);
				}
			}
			for (int e = 0; e < LW_FORECAST_CHOICES; e++) {
				double error, alpha;

				if (!isfinite(observed[c]) || !isfinite(matured.prediction[c][e]))
					continue;
				error = fabs(matured.prediction[c][e] - observed[c]);
				error *= error;
				if (!isfinite(error))
					continue;
				alpha = f->labels[c][e] ? 1 - exp(-
					((now_ns - f->last_label_ns[c][e]) / 1e9) / LOSS_MEMORY_S) : 1;
				f->loss[c][e] += (error - f->loss[c][e]) * alpha;
				f->last_label_ns[c][e] = now_ns;
				if (f->labels[c][e] != UINT64_MAX)
					f->labels[c][e]++;
			}
		}
	}
	for (int c = 0; c < LW_FORECAST_CHANNELS; c++) {
		f->selected[c] = LW_FORECAST_PERSISTENCE;
		f->prediction[c] = candidate[c][0];
		for (int e = 1; e < LW_FORECAST_EXPERTS; e++)
			if (!isfinite(candidate[c][e]))
				f->weight[c][e] = 0;
		project(f->weight[c]);
		learned[c] = candidate[c][0];
		for (unsigned int e = 1; e < LW_FORECAST_EXPERTS; e++) {
			if (isfinite(candidate[c][e]))
				learned[c] += f->weight[c][e] * (candidate[c][e] - candidate[c][0]);
		}
		/* The blend is an expert too. Use only completed squared errors to
		 * avoid publishing it when a simpler forecast is currently better.
		 */
		for (unsigned int e = 1; e < LW_FORECAST_CHOICES; e++) {
			double pred = e == LW_FORECAST_BLEND ? learned[c] : candidate[c][e];

			if (isfinite(pred) && f->labels[c][e] && f->labels[c][0] &&
			    f->loss[c][e] < f->loss[c][f->selected[c]])
				f->selected[c] = e;
		}
		f->prediction[c] = f->selected[c] == LW_FORECAST_BLEND ? learned[c] : candidate[c][f->selected[c]];
		for (unsigned int e = 0; e < LW_FORECAST_EXPERTS; e++)
			f->output_weight[c][e] = f->selected[c] == LW_FORECAST_BLEND ?
				f->weight[c][e] : (double)(f->selected[c] == e);
	}
	if ((f->have_issue && now_ns - f->last_issue_ns < SAMPLE_NS) ||
	    now_ns > UINT64_MAX - LW_FORECAST_HORIZON_NS)
		return;
	if (f->count == LW_FORECAST_PENDING) {
		f->head = (f->head + 1) % LW_FORECAST_PENDING;
		f->count--;
		f->dropped++;
	}
	unsigned int tail = (f->head + f->count) % LW_FORECAST_PENDING;
	f->pending[tail].due_ns = now_ns + LW_FORECAST_HORIZON_NS;
	for (int c = 0; c < LW_FORECAST_CHANNELS; c++) {
		for (int e = 0; e < LW_FORECAST_EXPERTS; e++)
			f->pending[tail].prediction[c][e] = candidate[c][e];
		f->pending[tail].prediction[c][LW_FORECAST_BLEND] = learned[c];
	}
	f->count++;
	f->last_issue_ns = now_ns;
	f->have_issue = true;
}
