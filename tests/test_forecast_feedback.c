// SPDX-License-Identifier: MIT
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lerwatt/forecast.h"

static void step(struct lw_forecaster *f, uint64_t now, double y)
{
	double observed[2] = { y, y };
	double candidates[2][3] = { { y, y + 5, y - 5 }, { y, y + 5, y - 5 } };

	lw_forecast_step(f, now, observed, candidates);
}

int main(void)
{
	struct lw_forecaster f = { 0 };

	step(&f, 0, 0);
	step(&f, 4000000000ull, 4);
	assert(f.labels[0][0] == 0); /* no label from the future */
	assert(f.selected[0] == LW_FORECAST_PERSISTENCE);
	step(&f, 5000000000ull, 5);
	assert(f.labels[0][0] == 1 && f.labels[0][1] == 1);
	assert(f.loss[0][0] == 25 && f.loss[0][1] == 0);
	assert(f.selected[0] == LW_FORECAST_CONSTANT && f.weight[0][1] > 0);
	memset(&f, 0, sizeof(f));
	for (uint64_t t = 0; t <= 1000; t++)
		step(&f, t * 100000000ull, t / 10.0);
	assert(f.labels[0][0] >= 64 && f.weight[0][1] > .99);
	assert(fabs(f.prediction[0] - 105) < .01);
	assert(f.count <= 51 && f.dropped == 0);

	/* Ten-second samples cannot label a five-second forecast precisely. */
	memset(&f, 0, sizeof(f));
	for (uint64_t t = 0; t < 20; t++)
		step(&f, t * 10000000000ull, t * 10.0);
	assert(f.labels[0][0] == 0 && f.dropped == 19);
	assert(f.selected[0] == LW_FORECAST_PERSISTENCE);

	/* Missing experts never inherit another expert's evidence. */
	double obs[2] = { 5, NAN }, x[2][3] = { { 5, NAN, NAN }, { NAN, NAN, NAN } };
	memset(&f, 0, sizeof(f));
	for (uint64_t t = 0; t < 200; t++)
		lw_forecast_step(&f, t * 100000000ull, obs, x);
	assert(f.labels[0][1] == 0 && f.labels[1][0] == 0);
	assert(f.prediction[0] == 5 && isnan(f.prediction[1]));

	/* Clock rollback resets pending labels; near-u64-max cannot wrap due time. */
	step(&f, 1, 1);
	assert(f.labels[0][0] == 0 && f.count == 1);
	step(&f, UINT64_MAX - 1, 2);
	assert(f.count == 0 && f.selected[0] == LW_FORECAST_PERSISTENCE);
	puts("test_forecast_feedback: causality, delayed labels, fallback and clock checks passed");
	return 0;
}
