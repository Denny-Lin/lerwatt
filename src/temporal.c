// SPDX-License-Identifier: MIT
#include <math.h>
#include <string.h>

#include "lerwatt/forecast.h"
#include "lerwatt/temporal.h"

#define SAMPLE_NS 100000000ull
#define MAX_LATENESS_NS 250000000ull
#define LOSS_MEMORY_S 30.0
#define STEP (SAMPLE_NS / (double)(LW_FORECAST_HORIZON_NS + SAMPLE_NS))

static double clip(double v, double lo, double hi)
{
	return fmax(lo, fmin(hi, v));
}

static double lagged(const struct lw_temporal *t, uint64_t now, uint64_t lag, double hot)
{
	if (now < lag)
		return hot;
	uint64_t target = now - lag;

	/* Read only an observation at or before the requested historical time. */
	for (unsigned int i = t->history_count; i; i--) {
		const struct lw_temporal_sample *s = &t->history[
			(t->history_head + i - 1) % LW_TEMPORAL_HISTORY];

		if (s->t_ns <= target)
			return target - s->t_ns <= MAX_LATENESS_NS ? s->hot_c : hot;
	}
	return hot;
}

double lw_temporal_step(struct lw_temporal *t, uint64_t now_ns, double hot_c,
			double reference_c, const double physical[3])
{
	struct lw_temporal_record record;
	bool have_label = false;
	double x[LW_TEMPORAL_FEATURES] = { 1, 0, 0, 0, 0, 0, 0 };
	const uint64_t lags[] = { 1000000000ull, 2000000000ull, 5000000000ull, 10000000000ull };

	if (t->have_observation && now_ns < t->last_observation_ns)
		memset(t, 0, sizeof(*t));
	t->have_observation = true;
	t->last_observation_ns = now_ns;
	t->active = false;
	t->prediction_c = reference_c;
	t->correction_c = 0;
	while (t->count && t->pending[t->head].due_ns <= now_ns) {
		if (have_label)
			t->dropped++;
		record = t->pending[t->head];
		have_label = true;
		t->head = (t->head + 1) % LW_TEMPORAL_PENDING;
		t->count--;
	}
	if (have_label && (now_ns - record.due_ns > MAX_LATENESS_NS || !isfinite(hot_c))) {
		t->dropped++;
		have_label = false;
	}
	if (have_label) {
		double prediction = record.reference, norm = .25;
		double loss = (record.prediction - hot_c) * (record.prediction - hot_c);
		double base_loss = (record.reference - hot_c) * (record.reference - hot_c);
		double difference = base_loss - loss;

		for (int j = 0; j < LW_TEMPORAL_FEATURES; j++) {
			prediction += t->weight[j] * record.x[j];
			norm += record.x[j] * record.x[j];
		}
		if (isfinite(prediction) && isfinite(loss) && isfinite(base_loss) &&
		    isfinite(difference * difference)) {
			double error = clip(hot_c - prediction, -10, 10);
			double alpha = t->labels ? 1 - exp(-((now_ns - t->last_label_ns) / 1e9) /
							 LOSS_MEMORY_S) : 1;

			for (int j = 0; j < LW_TEMPORAL_FEATURES; j++)
				t->weight[j] = clip(t->weight[j] + STEP * error * record.x[j] / norm, -4, 4);
			t->loss += (loss - t->loss) * alpha;
			t->reference_loss += (base_loss - t->reference_loss) * alpha;
			t->difference_squared += (difference * difference - t->difference_squared) * alpha;
			t->last_label_ns = now_ns;
			if (t->labels != UINT64_MAX)
				t->labels++;
		} else {
			t->dropped++;
		}
	}
	if (!isfinite(hot_c) || !isfinite(reference_c))
		return reference_c;
	for (int j = 0; j < 4; j++)
		x[j + 1] = clip(hot_c - lagged(t, now_ns, lags[j], hot_c), -20, 20);
	for (int j = 1; j < 3; j++)
		x[j + 4] = isfinite(physical[j]) ? clip(physical[j] - hot_c, -20, 20) : 0;
	double delta = 0;
	for (int j = 0; j < LW_TEMPORAL_FEATURES; j++)
		delta += t->weight[j] * x[j];
	double candidate = reference_c + clip(delta, -20, 20);
	t->advantage = t->reference_loss - t->loss;
	/* Variability guard, not a confidence interval: the 30 s memory contains
	 * at most six non-overlapping five-second forecast horizons. This avoids
	 * treating the 300 overlapping labels as independent support for a small
	 * apparent advantage. It makes no independence or coverage claim.
	 */
	t->margin = sqrt(fmax(0, t->difference_squared - t->advantage * t->advantage) /
			 (LOSS_MEMORY_S / (LW_FORECAST_HORIZON_NS / 1e9)));
	t->active = t->labels && now_ns - t->last_label_ns <=
		LW_FORECAST_HORIZON_NS + MAX_LATENESS_NS &&
		t->advantage > t->margin && isfinite(candidate);
	t->prediction_c = t->active ? candidate : reference_c;
	t->correction_c = t->prediction_c - reference_c;
	if ((t->have_issue && now_ns - t->last_issue_ns < SAMPLE_NS) ||
	    now_ns > UINT64_MAX - LW_FORECAST_HORIZON_NS)
		return t->prediction_c;
	if (t->history_count == LW_TEMPORAL_HISTORY) {
		t->history_head = (t->history_head + 1) % LW_TEMPORAL_HISTORY;
		t->history_count--;
	}
	unsigned int tail = (t->history_head + t->history_count) % LW_TEMPORAL_HISTORY;
	t->history[tail] = (struct lw_temporal_sample){ now_ns, hot_c };
	t->history_count++;
	if (t->count == LW_TEMPORAL_PENDING) {
		t->head = (t->head + 1) % LW_TEMPORAL_PENDING;
		t->count--;
		t->dropped++;
	}
	tail = (t->head + t->count) % LW_TEMPORAL_PENDING;
	t->pending[tail] = (struct lw_temporal_record){
		.due_ns = now_ns + LW_FORECAST_HORIZON_NS,
		.reference = reference_c, .prediction = candidate,
	};
	memcpy(t->pending[tail].x, x, sizeof(x));
	t->count++;
	t->have_issue = true;
	t->last_issue_ns = now_ns;
	return t->prediction_c;
}
