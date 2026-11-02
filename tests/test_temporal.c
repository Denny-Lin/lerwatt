// SPDX-License-Identifier: MIT
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lerwatt/temporal.h"

static double step(struct lw_temporal *t, uint64_t now, double hot, double reference)
{
	double physical[3] = { hot, hot + 5, hot + 5 };
	return lw_temporal_step(t, now, hot, reference, physical);
}

int main(void)
{
	struct lw_temporal t = { 0 };

	assert(step(&t, 0, 0, 0) == 0 && t.labels == 0 && !t.active);
	step(&t, 4900000000ull, 4.9, 4.9);
	assert(t.labels == 0);
	step(&t, 5000000000ull, 5, 5);
	assert(t.labels == 1);
	memset(&t, 0, sizeof(t));
	for (uint64_t i = 0; i <= 3000; i++)
		step(&t, i * 100000000ull, i / 10.0, i / 10.0);
	assert(t.active && fabs(t.prediction_c - 305) < .1);
	assert(t.count <= 51 && t.history_count == LW_TEMPORAL_HISTORY && t.dropped == 0);
	assert(t.correction_c >= -20 && t.correction_c <= 20);
	for (int j = 0; j < LW_TEMPORAL_FEATURES; j++)
		assert(t.weight[j] >= -4 && t.weight[j] <= 4);
	step(&t, 400000000000ull, 400, 400);
	assert(!t.active && t.prediction_c == 400); /* stale quality is not fresh evidence */

	/* A correct reference is retained; the zero residual is not evidence to
	 * replace it. Delayed loss compares saved predictions, not refitted ones. */
	memset(&t, 0, sizeof(t));
	for (uint64_t i = 0; i <= 1000; i++) {
		double reference = i / 10.0 + 5;
		assert(step(&t, i * 100000000ull, i / 10.0, reference) == reference);
	}
	assert(!t.active && t.reference_loss == 0 && t.correction_c == 0);

	/* Labels more than 250 ms late cannot train the five-second target. */
	memset(&t, 0, sizeof(t));
	for (uint64_t i = 0; i < 20; i++)
		step(&t, i * 10000000000ull, i, i);
	assert(t.labels == 0 && t.dropped == 19 && !t.active);
	step(&t, 1, 1, 1);
	assert(t.labels == 0 && t.count == 1 && t.history_count == 1);
	step(&t, UINT64_MAX - 1, 2, 2);
	assert(t.count == 0 && t.labels == 0);

	memset(&t, 0, sizeof(t));
	assert(isnan(step(&t, 0, NAN, NAN)));
	assert(t.count == 0 && t.history_count == 0);
	step(&t, 1, DBL_MAX, DBL_MAX);
	step(&t, 5000000001ull, -DBL_MAX, -DBL_MAX);
	assert(t.labels == 0 && t.dropped == 1 && isfinite(t.prediction_c));

	printf("test_temporal: delayed labels, causal history, reference fallback and bounds passed (%zu bytes/cluster)\n",
	       sizeof(struct lw_temporal));
	return 0;
}
