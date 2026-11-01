// SPDX-License-Identifier: MIT
/*
 * Unit tests for the simulator model: kernel formulas must match the kernel
 * bit for bit, and the physics must conserve energy.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "sim.h"

static int failures;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
		failures++;						\
	}								\
} while (0)

static void test_capacity(void)
{
	const struct sim_platform *p = &sim_rk3588;

	/* 530 * 1.8 GHz vs 1024 * 2.4 GHz, normalized to 1024 and truncated. */
	CHECK(sim_capacity(p, 0) == 397, "little capacity %u", sim_capacity(p, 0));
	CHECK(sim_capacity(p, 4) == 1024, "big capacity %u", sim_capacity(p, 4));
	CHECK(sim_capacity(p, 7) == 1024, "big capacity %u", sim_capacity(p, 7));
}

static void test_energy_model(void)
{
	const struct sim_platform *p = &sim_rk3588;

	/* drivers/opp/of.c: 416 * 1000^2 * 2400 / 1e6 */
	CHECK(sim_em_power_uw(&p->clusters[1], 6) == 998400, "big top %u",
	      sim_em_power_uw(&p->clusters[1], 6));
	/* 228 * 950^2 * 1800 / 1e6 */
	CHECK(sim_em_power_uw(&p->clusters[0], 4) == 370386, "little top %u",
	      sim_em_power_uw(&p->clusters[0], 4));
	/* 228 * 675^2 * 1008 / 1e6 */
	CHECK(sim_em_power_uw(&p->clusters[0], 0) == 104713, "little bottom %u",
	      sim_em_power_uw(&p->clusters[0], 0));
	CHECK(sim_em_perf(p, 0, 4) == 397, "little top perf %u", sim_em_perf(p, 0, 4));
	CHECK(sim_em_perf(p, 0, 0) == 222, "little bottom perf %u", sim_em_perf(p, 0, 0));
	CHECK(sim_em_perf(p, 1, 0) == 512, "big bottom perf %u", sim_em_perf(p, 1, 0));
	for (int k = 0; k < p->nclusters; k++)
		for (int i = 1; i < p->clusters[k].nopps; i++)
			CHECK(sim_em_cost(p, k, i) >= sim_em_cost(p, k, i - 1) ||
			      p->clusters[k].opps[i].uv == p->clusters[k].opps[i - 1].uv,
			      "cluster %d cost not monotonic at %d", k, i);
}

static void test_tsadc(void)
{
	const struct sim_platform *p = &sim_rk3588;

	CHECK(sim_tsadc_read_mc(p, 25.0) == 25000, "25C -> %d", sim_tsadc_read_mc(p, 25.0));
	CHECK(sim_tsadc_read_mc(p, 85.0) == 85000, "85C -> %d", sim_tsadc_read_mc(p, 85.0));
	/* code 323 -> 25000 + 60000 * 38 / 65 = 60076 (driver truncates) */
	CHECK(sim_tsadc_read_mc(p, 60.0) == 60076, "60C -> %d", sim_tsadc_read_mc(p, 60.0));
	/* resolution is one ADC code, about 0.92 C between 25 and 85 C */
	CHECK(abs(sim_tsadc_read_mc(p, 70.4) - 70400) < 930, "70.4C -> %d",
	      sim_tsadc_read_mc(p, 70.4));
}

static void run(struct sim_state *s, double load, double secs)
{
	double d[SIM_MAX_CPUS];

	for (int i = 0; i < SIM_MAX_CPUS; i++)
		d[i] = load;
	for (double t = 0; t < secs; t += 0.01)
		sim_step(s, d, 0.01);
}

static void test_thermal_steady_state(void)
{
	const struct sim_platform *p = &sim_rk3588;
	struct sim_state s;
	double heat;

	sim_init(&s, p, 0, 25.0, 25.0, 1.0, 1);
	s.kernel_thermal = false;
	run(&s, 1.0, 3000);	/* ~15 die time constants */
	heat = p->soc_other_w;
	for (int k = 0; k < p->nclusters; k++)
		heat += s.cl[k].power_w;
	/* Energy balance at equilibrium: die - ambient = R * heat. */
	CHECK(fabs((s.die_c - 25.0) - heat * s.cool->r_die_amb) < 0.2,
	      "steady state die %.2f C, heat %.2f W", s.die_c, heat);
	CHECK(s.cl[1].temp_c > s.die_c, "big hot spot %.2f not above die %.2f",
	      s.cl[1].temp_c, s.die_c);
	printf("full load, passive cooling, no throttling: die %.1f C, big %.1f C, little %.1f C,"
	       " CPU heat %.2f W, board %.2f W\n", s.die_c, s.cl[1].temp_c, s.cl[0].temp_c,
	       heat - p->soc_other_w, s.board_w);
}

static void test_kernel_throttle(void)
{
	struct sim_state s;

	sim_init(&s, &sim_rk3588, 2, 25.0, 25.0, 1.0, 2);	/* bare board */
	run(&s, 1.0, 900);
	CHECK(!s.critical, "bare board reached critical despite step_wise");
	CHECK(s.cl[1].thermal_state > 0, "expected step_wise throttling on a bare board");
	CHECK(s.zone[1].sensor_mc < 90000, "big zone %d mC not held near the trip",
	      s.zone[1].sensor_mc);
	printf("bare board, full load, step_wise on: big zone %.1f C, state %d, %llu throttle steps\n",
	       s.zone[1].sensor_mc / 1000.0, s.cl[1].thermal_state,
	       (unsigned long long)s.throttle_events);
}

static void test_idle_power(void)
{
	struct sim_state s;

	sim_init(&s, &sim_rk3588, 1, 25.0, 25.0, 1.0, 3);
	run(&s, 0.0, 60);
	CHECK(s.board_w > sim_rk3588.base_w && s.board_w < sim_rk3588.base_w + 1.5,
	      "idle board power %.2f W", s.board_w);
	CHECK(s.cl[1].cur_idx == 0, "idle big cluster at OPP %d", s.cl[1].cur_idx);
	printf("idle, fan: board %.2f W, die %.1f C\n", s.board_w, s.die_c);
}

static void test_freq_qos(void)
{
	struct sim_state s;

	sim_init(&s, &sim_rk3588, 1, 25.0, 25.0, 1.0, 4);
	s.cl[1].user_max_khz = 1900000;	/* not an OPP: resolves down to 1800 MHz */
	run(&s, 1.0, 2);
	CHECK(sim_rk3588.clusters[1].opps[s.cl[1].cur_idx].khz == 1800000,
	      "max 1.9 GHz request gave %u kHz",
	      sim_rk3588.clusters[1].opps[s.cl[1].cur_idx].khz);
	s.cl[0].user_min_khz = 1500000;	/* resolves up to 1608 MHz */
	run(&s, 0.0, 2);
	CHECK(sim_rk3588.clusters[0].opps[s.cl[0].cur_idx].khz == 1608000,
	      "min 1.5 GHz request gave %u kHz",
	      sim_rk3588.clusters[0].opps[s.cl[0].cur_idx].khz);
}

int main(void)
{
	test_capacity();
	test_energy_model();
	test_tsadc();
	test_thermal_steady_state();
	test_kernel_throttle();
	test_idle_power();
	test_freq_qos();
	if (failures) {
		fprintf(stderr, "test_model: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_model: all checks passed\n");
	return 0;
}
