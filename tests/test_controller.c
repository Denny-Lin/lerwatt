// SPDX-License-Identifier: MIT
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "lerwatt/controller.h"

#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "test_controller: %s\n", msg); return 1; } } while (0)

int main(void)
{
	struct lw_opp opp[3][3];
	struct lw_ctl_in in[3];
	struct lw_ctl_state st[3] = { 0 }, reference[3] = { 0 };
	struct lw_ctl_out out[3], expected[3];
	struct lw_ctl_params p;
	const uint64_t now = 90000000000ull; /* first trained plan must not depend on boot uptime */

	lw_ctl_default_params(&p, 85000);
	p.policy = LW_POLICY_MPC;
	p.cross_cluster = true;
	memset(in, 0, sizeof(in));
	for (int k = 0; k < 3; k++) {
		for (int i = 0; i < 3; i++) {
			opp[k][i] = (struct lw_opp) {
				.khz = 1200000 + i * 600000,
				.power_uw = k ? (uint32_t)(i + 1) * 1000000 : (uint32_t)(1 << i) * 500000,
			};
		}
		in[k] = (struct lw_ctl_in) {
			.temp_mc = 75000, .busy_mean = 1, .cur_khz = 2400000,
			.ncpus = k ? 2 : 4, .opps = opp[k], .nopps = 3,
			.capacity = k ? 1024 : 400, .lat_crit = -1, .package_mc = 70000,
		};
		lw_ctl_step(&p, &in[k], &reference[k], &expected[k], now, .1);
	}
	lw_ctl_step_group(&p, in, st, out, 3, now, .1);
	for (int k = 0; k < 3; k++)
		CHECK(out[k].ceiling_khz == expected[k].ceiling_khz && !out[k].model_ready,
		      "untrained group did not fall back to independent control");

	/* A known, learned synthetic plant: raising total power heats every
	 * cluster. Joint search must sacrifice the less efficient cluster first.
	 */
	for (int k = 0; k < 3; k++) {
		double own = k ? 6 : 8;

		lw_tm_init(&st[k].predictor.thermal);
		st[k].predictor.thermal.samples = 100;
		st[k].predictor.thermal.theta[0] = .05;
		st[k].predictor.thermal.theta[1] = -.01;
		st[k].predictor.thermal.theta[2] = .25;
		st[k].predictor.thermal.have_prev = true;
		st[k].predictor.thermal.offset_avg_c = st[k].predictor.thermal.peak_offset_c = 5;
		st[k].predictor.thermal.power_avg_w = st[k].predictor.thermal.peak_power_w = own;
		st[k].predictor.thermal.hot_s = 100;
		st[k].mpc_busy_init = true;
		st[k].mpc_busy = st[k].mpc_peak_busy = 1;
		st[k].mpc_work_khz = 2400000;
		st[k].mpc_other = in[k].other_power_w = 20 - own;
		st[k].pkg_c = 70;
		st[k].mpc_peak_c = 75;
	}
	lw_ctl_step_group(&p, in, st, out, 3, now + 1000000000ull, .1);
	CHECK(out[0].ceiling_khz == 1200000, "joint search did not reduce the less efficient cluster");
	CHECK(out[1].ceiling_khz > 1200000 || out[2].ceiling_khz > 1200000,
	      "joint search did not preserve big-cluster capacity");
	CHECK(st[0].last_group_ns != 0, "joint search was never evaluated");

	/* A sensor at the common raw limit overrides a held joint allocation. */
	in[1].temp_mc = p.raw_limit_mc;
	in[1].lat_crit = 1;
	lw_ctl_step_group(&p, in, st, out, 3, now + 1100000000ull, .1);
	CHECK(out[1].ceiling_khz == 1200000 && out[1].floor_khz <= out[1].ceiling_khz,
	      "raw guard or floor clamping was bypassed by a joint plan");
	struct lw_ctl_state trained = st[0];

	/* Without the package zone, MPC and joint planning cannot act. */
	memset(st, 0, sizeof(st));
	for (int k = 0; k < 3; k++)
		in[k].package_mc = INT32_MIN;
	lw_ctl_step_group(&p, in, st, out, 3, now, .1);
	for (int k = 0; k < 3; k++)
		CHECK(!out[k].model_ready && !st[k].last_group_ns, "missing package sensor enabled joint planning");

	/* Bound the combinatorial cost on larger discovered topologies. */
	struct lw_opp many_opp[5];
	struct lw_ctl_in many_in[LW_MAX_POLICIES];
	struct lw_ctl_state many_st[LW_MAX_POLICIES];
	struct lw_ctl_out many_out[LW_MAX_POLICIES];
	for (int i = 0; i < 5; i++)
		many_opp[i] = (struct lw_opp) { .khz = 1200000 + i * 300000, .power_uw = 500000 + i * 300000 };
	for (int k = 0; k < LW_MAX_POLICIES; k++) {
		many_in[k] = in[0];
		many_in[k].opps = many_opp;
		many_in[k].nopps = 5;
		many_in[k].package_mc = 70000;
		many_st[k] = trained;
	}
	lw_ctl_step_group(&p, many_in, many_st, many_out, LW_MAX_POLICIES, now + 2000000000ull, .1);
	for (int k = 0; k < LW_MAX_POLICIES; k++)
		CHECK(!many_out[k].joint_active, "oversized OPP product enabled joint search");

	/* Reused outputs in rule mode must not retain temporal telemetry. */
	p.policy = LW_POLICY_RULE;
	memset(&out[0], 0xa5, sizeof(out[0]));
	lw_ctl_step(&p, &in[0], &st[0], &out[0], now, .1);
	CHECK(out[0].forecast_correction_c == 0 && out[0].temporal_labels == 0 &&
	      out[0].temporal_dropped == 0, "rule mode retained stale point-correction telemetry");
	puts("test_controller: fallback, joint allocation and raw-guard checks passed");
	return 0;
}
