// SPDX-License-Identifier: MIT
/*
 * The online thermal model on a plant it can represent exactly: a first-order
 * package (dT/dt = a*P + c*T + d) driven by a changing power trace and sampled
 * through 1 degC quantization, as a TSADC reports it.
 */
#include <math.h>
#include <stdio.h>

#include "lerwatt/learn.h"

static int failures;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
		failures++;						\
	}								\
} while (0)

int main(void)
{
	const double a = 0.02, c = -1.0 / 120.0, ambient = 25.0, dt = 0.1;
	const double d = -c * ambient;	/* settles at ambient with no power */
	struct lw_thermal_model m;
	double t = ambient;

	lw_tm_init(&m);
	CHECK(!lw_tm_ready(&m), "ready before any data");
	for (int k = 0; k < 20000; k++) {	/* 2000 s */
		double p = (k / 3000) % 2 ? 2.0 : 9.0;	/* 300 s steps */

		t += (a * p + c * t + d) * dt;
		lw_tm_observe(&m, floor(t), p * 0.7, p * 0.3, dt);
	}
	CHECK(lw_tm_ready(&m), "not ready after 2000 s");
	CHECK(fabs(lw_tm_steady(&m, 0, 0) - ambient) < 3.0, "ambient %.1f, want %.1f",
	      lw_tm_steady(&m, 0, 0), ambient);
	CHECK(fabs(lw_tm_steady(&m, 6.3, 2.7) - (ambient - a * 9.0 / c)) < 3.0,
	      "9 W steady state %.1f, want %.1f", lw_tm_steady(&m, 6.3, 2.7), ambient - a * 9.0 / c);
	CHECK(fabs(lw_tm_predict(&m, 50.0, 9.0, 0, 60.0) -
		   (ambient - a * 9.0 / c + (50.0 - ambient + a * 9.0 / c) * exp(c * 60.0))) < 3.0,
	      "60 s prediction off");
	printf("test_learn: a=%.4f c=%.5f ambient=%.1f after %llu samples\n", m.theta[0], m.theta[1],
	       lw_tm_steady(&m, 0, 0), (unsigned long long)m.samples);
	if (failures)
		return 1;
	printf("test_learn: all checks passed\n");
	return 0;
}
