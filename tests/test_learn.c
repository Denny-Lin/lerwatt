// SPDX-License-Identifier: MIT
/*
 * The online thermal model on a plant it can represent: a first-order package
 * under a one-second hot spot, driven by a changing power trace and sampled
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
	const double r = 3.0, tau_hot = 1.0;
	struct lw_thermal_model m;
	double pkg = ambient, offset = 0;

	lw_tm_init(&m);
	CHECK(!lw_tm_ready(&m), "ready before any data");
	for (int k = 0; k < 20000; k++) {	/* 2000 s */
		double p = (k / 3000) % 2 ? 2.0 : 9.0;	/* 300 s steps */

		pkg += (a * p + c * pkg + d) * dt;
		offset += (r * 0.7 * p - offset) * dt / tau_hot;
		lw_tm_observe(&m, floor(pkg + offset), floor(pkg), p * 0.7, p * 0.3, dt);
	}
	CHECK(lw_tm_ready(&m), "not ready after 2000 s");
	CHECK(fabs(lw_tm_package_steady(&m, 0) - ambient) < 3.0, "ambient %.1f, want %.1f",
	      lw_tm_package_steady(&m, 0), ambient);
	CHECK(fabs(lw_tm_package_steady(&m, 9.0) - (ambient - a * 9.0 / c)) < 3.0,
	      "9 W steady state %.1f, want %.1f", lw_tm_package_steady(&m, 9.0), ambient - a * 9.0 / c);
	CHECK(fabs(lw_tm_package(&m, 50.0, 9.0, 60.0) -
		   (ambient - a * 9.0 / c + (50.0 - ambient + a * 9.0 / c) * exp(c * 60.0))) < 3.0,
	      "60 s prediction off");
	CHECK(fabs(lw_tm_offset(&m, 4.0) - r * 4.0) < 1.5, "offset %.1f, want %.1f",
	      lw_tm_offset(&m, 4.0), r * 4.0);
	printf("test_learn: a=%.4f c=%.5f ambient=%.1f after %llu samples\n", m.theta[0], m.theta[1],
	       lw_tm_package_steady(&m, 0), (unsigned long long)m.samples);

	/* Bursts shorter than the hot spot's time constant never reach r * P. */
	for (int k = 0; k < 2000; k++) {
		double p = (k / 3) % 2 ? 0.0 : 6.0;	/* 0.3 s on, 0.3 s off */

		offset += (r * p - offset) * dt / tau_hot;
		lw_tm_observe(&m, pkg + offset, pkg, p, 0, dt);
	}
	CHECK(lw_tm_peak_offset(&m, 6.0) < 0.8 * lw_tm_offset(&m, 6.0),
	      "peak offset %.1f of %.1f ignores burst length", lw_tm_peak_offset(&m, 6.0),
	      lw_tm_offset(&m, 6.0));
	if (failures)
		return 1;
	printf("test_learn: all checks passed\n");
	return 0;
}
