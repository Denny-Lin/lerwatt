// SPDX-License-Identifier: MIT
/*
 * Rockchip RK3588 (4x Cortex-A55 + 2x2 Cortex-A76).
 *
 * EXACT values come from Linux v7.3:
 *   arch/arm64/boot/dts/rockchip/rk3588-opp.dtsi   OPP frequencies and voltages,
 *                                                  clock-latency-ns
 *   arch/arm64/boot/dts/rockchip/rk3588-base.dtsi  capacity-dmips-mhz, dynamic-
 *                                                  power-coefficient, CPU_SLEEP idle
 *                                                  state, thermal zones and trips
 *   drivers/thermal/rockchip_thermal.c             rk3588_code_table, TSADC periods
 *
 * ASSUMED values are marked. They are plausible for an RK3588 single-board
 * computer and exist to be replaced by fitted numbers from a real board
 * (docs/SIMULATION.md, "Calibration").
 */
#include "sim.h"

const struct sim_platform sim_rk3588 = {
	.name = "rk3588",
	.ncpus = 8,
	.nclusters = 3,
	.clusters = {
		{
			.name = "little", .first_cpu = 0, .ncpus = 4,
			.dmips_mhz = 530, .dpc = 228,
			.nopps = 5,
			.opps = {
				{ 1008000, 675000 }, { 1200000, 712500 }, { 1416000, 762500 },
				{ 1608000, 850000 }, { 1800000, 950000 },
			},
			.leak_w_ref = 0.04,	/* ASSUMED */
			.r_to_die = 2.5,	/* ASSUMED */
			.c_node = 0.4,		/* ASSUMED */
		},
		{
			.name = "big0", .first_cpu = 4, .ncpus = 2,
			.dmips_mhz = 1024, .dpc = 416,
			.nopps = 7,
			.opps = {
				{ 1200000, 675000 }, { 1416000, 725000 }, { 1608000, 762500 },
				{ 1800000, 850000 }, { 2016000, 925000 }, { 2208000, 987500 },
				{ 2400000, 1000000 },
			},
			.leak_w_ref = 0.20,	/* ASSUMED */
			.r_to_die = 3.0,	/* ASSUMED */
			.c_node = 0.3,		/* ASSUMED */
		},
		{
			.name = "big1", .first_cpu = 6, .ncpus = 2,
			.dmips_mhz = 1024, .dpc = 416,
			.nopps = 7,
			.opps = {
				{ 1200000, 675000 }, { 1416000, 725000 }, { 1608000, 762500 },
				{ 1800000, 850000 }, { 2016000, 925000 }, { 2208000, 987500 },
				{ 2400000, 1000000 },
			},
			.leak_w_ref = 0.20,	/* ASSUMED */
			.r_to_die = 3.0,	/* ASSUMED */
			.c_node = 0.3,		/* ASSUMED */
		},
	},
	.nidle = 2,
	.idle = {
		/* state0 is always WFI on arm64 cpuidle-psci. */
		{ "WFI", "ARM WFI", 1, 1, 0.04 /* ASSUMED */, 1.0 },
		/* DT CPU_SLEEP: entry 100 us + exit 120 us, min-residency 1000 us. */
		{ "cpu-sleep", "cpu-sleep", 220, 1000, 0.0, 0.05 /* ASSUMED */ },
	},
	.nzones = 7,
	.zones = {
		/* DT order; offsets from the node they sit on are ASSUMED. */
		{ "package-thermal", -1, 0.0, false },
		{ "bigcore0-thermal", 1, 0.0, true },
		{ "bigcore2-thermal", 2, 0.0, true },
		{ "littlecore-thermal", 0, 0.0, true },
		{ "center-thermal", -1, -1.0, false },
		{ "gpu-thermal", -1, -2.0, false },
		{ "npu-thermal", -1, -2.0, false },
	},
	.passive_mc = 85000,
	.passive_hyst_mc = 2000,
	.critical_mc = 115000,
	.passive_poll_ms = 100,
	.ntsadc = 4,
	.tsadc = { { 215, -40000 }, { 285, 25000 }, { 350, 85000 }, { 395, 125000 } },
	.tsadc_period_ms = 250,		/* TSADCV2_AUTO_PERIOD_TIME */
	.tsadc_period_hot_ms = 50,	/* TSADCV2_AUTO_PERIOD_HT_TIME */
	.tsadc_noise_c = 0.3,		/* ASSUMED */
	.transition_latency_ns = 40000,	/* DT clock-latency-ns */
	.base_w = 2.2,			/* ASSUMED */
	.soc_other_w = 0.6,		/* ASSUMED */
	.mem_w_full = 0.6,		/* ASSUMED */
	.pmic_eff = 0.88,		/* ASSUMED */
	.leak_beta = 0.02,		/* ASSUMED: leakage doubles every ~35 K */
	.leak_t_ref_c = 85.0,
	.meter_lsb_w = 0.025,		/* INA226 power LSB with a 1 mA current LSB */
	.meter_noise_frac = 0.003,	/* ASSUMED */
	.ncooling = 3,
	.cooling = {
		{ "passive", 7.5, 30.0 },	/* ASSUMED: small heatsink, no fan */
		{ "fan", 3.0, 30.0 },		/* ASSUMED: heatsink + fan */
		{ "bare", 14.0, 8.0 },		/* ASSUMED: no heatsink */
	},
};
