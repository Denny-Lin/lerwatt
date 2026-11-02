// SPDX-License-Identifier: MIT
/*
 * Closed loop in virtual time: the real control law (src/controller.c)
 * against the simulator model (sim/model.c), with no files in between.
 * It observes only what lerwattd can observe on a board: TSADC readings,
 * busy time, the current frequency and the Energy Model. It acts only the
 * way lerwattd acts: by lowering scaling_max_freq.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "lerwatt/controller.h"
#include "sim.h"

static int failures;
static bool joint_mpc;
static bool cooperative;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
		failures++;						\
	}								\
} while (0)

struct result {
	double peak_c;
	double peak_c_after_warmup;
	double s_above_trip;
	unsigned long long kernel_throttle_steps;
	unsigned long long ceiling_changes;
	unsigned long long freq_transitions;
	double energy_j;
	double served;
	double mean_w;
	unsigned long long mpc_periods;
};

static int zone_of_cluster(const struct sim_platform *p, int k)
{
	for (int z = 0; z < p->nzones; z++)
		if (p->zones[z].cluster == k && p->zones[z].passive)
			return z;
	return 0;
}

static void demand_for(const char *pattern, const struct sim_platform *p, double t, double *d)
{
	for (int c = 0; c < p->ncpus; c++) {
		bool big = sim_capacity(p, c) >= 1024;

		if (!strcmp(pattern, "full"))
			d[c] = 1.0;
		else if (!strcmp(pattern, "big"))
			d[c] = big ? 1.0 : 0.1;
		else if (!strcmp(pattern, "burst"))
			d[c] = ((long)(t / 2.0) % 2) ? 0.05 : 1.0;
		else if (!strcmp(pattern, "burst10"))
			d[c] = ((long)(t / 10.0) % 2) ? 0.05 : 1.0;
		else if (!strcmp(pattern, "burst300ms"))
			d[c] = ((long)(t / 0.3) % 2) ? 0.05 : 1.0;
		else if (!strcmp(pattern, "steps"))	/* the load changes every 120 s */
			d[c] = ((long)(t / 120.0) % 4 + 1) * 0.25;
		else	/* onebig: one cluster alone */
			d[c] = c >= 4 && c < 6 ? 1.0 : 0.05;
	}
}

static struct result run_from(const char *pattern, int cooling, double ambient_c, double start_c,
			      uint64_t seed, bool controlled, enum lw_ctl_policy policy, double secs)
{
	const struct sim_platform *p = &sim_rk3588;
	struct lw_opp opps[SIM_MAX_CLUSTERS][SIM_MAX_OPPS];
	struct lw_ctl_state st[SIM_MAX_CLUSTERS];
	double prev_busy[SIM_MAX_CPUS] = { 0 }, d[SIM_MAX_CPUS], est_w[SIM_MAX_CLUSTERS] = { 0 };
	struct lw_ctl_params params;
	struct result r = { 0 };
	struct sim_state s;
	int ticks_per_period = 10;	/* 10 ms model ticks, 100 ms control period */

	sim_init(&s, p, cooling, ambient_c, start_c, 1.0, seed);
	memset(st, 0, sizeof(st));
	lw_ctl_default_params(&params, p->passive_mc);
	params.policy = policy;
	params.cross_cluster = joint_mpc;
	params.cooperative_prediction = cooperative;
	for (int k = 0; k < p->nclusters; k++)
		for (int i = 0; i < p->clusters[k].nopps; i++) {
			opps[k][i].khz = p->clusters[k].opps[i].khz;
			opps[k][i].power_uw = sim_em_power_uw(&p->clusters[k], i);
			opps[k][i].perf = sim_em_perf(p, k, i);
		}

	for (long tick = 0; s.t_s < secs; tick++) {
		demand_for(pattern, p, s.t_s, d);
		sim_step(&s, d, 0.01);

		for (int z = 0; z < p->nzones; z++) {
			if (!p->zones[z].passive)
				continue;
			if (s.zone[z].sensor_mc / 1000.0 > r.peak_c)
				r.peak_c = s.zone[z].sensor_mc / 1000.0;
			if (s.t_s > 300 && s.zone[z].sensor_mc / 1000.0 > r.peak_c_after_warmup)
				r.peak_c_after_warmup = s.zone[z].sensor_mc / 1000.0;
			if (s.zone[z].sensor_mc >= p->passive_mc)
				r.s_above_trip += 0.01;
		}
		CHECK(!s.critical, "%s: critical trip at %.1f s", pattern, s.t_s);
		if (s.critical)
			break;

		if (!controlled || (tick + 1) % ticks_per_period)
			continue;
		struct lw_ctl_in inputs[SIM_MAX_CLUSTERS];
		struct lw_ctl_out outputs[SIM_MAX_CLUSTERS];
		for (int k = 0; k < p->nclusters; k++) {
			const struct sim_cluster_desc *c = &p->clusters[k];
			double busy = 0;
			struct lw_ctl_in in = {
				.temp_mc = s.zone[zone_of_cluster(p, k)].sensor_mc,
				.cur_khz = c->opps[s.cl[k].cur_idx].khz,
				.effective_max_khz = c->opps[sim_eff_max_idx(&s, k)].khz,
				.ncpus = c->ncpus,
				.opps = opps[k],
				.nopps = c->nopps,
				.capacity = s.capacity[c->first_cpu],
				.deep_idle_latency_us = (int32_t)p->idle[p->nidle - 1].latency_us,
				.lat_crit = -1,
				.package_mc = s.zone[0].sensor_mc,
			};
			double others = 0;

			for (int j = 0; j < p->nclusters; j++)
				if (j != k)
					others += est_w[j];
			in.other_power_w = others;

			for (int i = 0; i < c->ncpus; i++) {
				int cpu = c->first_cpu + i;

				busy += (s.cpu[cpu].stat_busy_s - prev_busy[cpu]) / 0.1;
				prev_busy[cpu] = s.cpu[cpu].stat_busy_s;
			}
			in.busy_mean = busy / c->ncpus;
			inputs[k] = in;
		}
		lw_ctl_step_group(&params, inputs, st, outputs, p->nclusters,
				  (uint64_t)(s.t_s * 1e9), 0.1);
		for (int k = 0; k < p->nclusters; k++) {
			s.cl[k].user_max_khz = outputs[k].ceiling_khz;	/* = writing scaling_max_freq */
			est_w[k] = outputs[k].est_power_w;
			r.mpc_periods += st[k].mpc_periods ? 1 : 0;
		}
	}
	for (int k = 0; k < p->nclusters; k++) {
		r.ceiling_changes += st[k].ceiling_changes;
		r.freq_transitions += s.cl[k].total_trans;
	}
	r.kernel_throttle_steps = s.throttle_events;
	r.energy_j = s.energy_j;
	r.served = s.work_demand > 0 ? s.work_done / s.work_demand : 1;
	r.mean_w = s.energy_j / s.t_s;
	return r;
}

static struct result run(const char *pattern, int cooling, bool controlled, enum lw_ctl_policy policy,
			 double secs)
{
	return run_from(pattern, cooling, 25.0, 25.0, 42, controlled, policy, secs);
}

static void report(const char *name, const char *mode, const struct result *r)
{
	printf("| %-22s | %-10s | %6.1f | %7.1f | %6llu | %6llu | %8llu | %7.2f | %6.2f%% |\n",
	       name, mode, r->peak_c_after_warmup, r->s_above_trip, r->kernel_throttle_steps,
	       r->ceiling_changes, r->freq_transitions, r->mean_w, 100 * r->served);
}

/*
 * The checks every controller passes. Bursts still swing the hot spot a few
 * degrees around the controlled mean (its time constant is about a second),
 * so the margin is checked on peaks.
 */
static void check_safe(const char *name, const char *who, const struct result *ctl)
{
	/* Requirement 1: the kernel's passive trip never engages, with 1 C of margin. */
	CHECK(ctl->kernel_throttle_steps == 0, "%s/%s: kernel throttled %llu times with Lerwatt",
	      name, who, ctl->kernel_throttle_steps);
	CHECK(ctl->peak_c_after_warmup <= sim_rk3588.passive_mc / 1000.0 - 1.0 + 0.05,
	      "%s/%s: peak %.1f C is within 1 C of the passive trip", name, who,
	      ctl->peak_c_after_warmup);
	CHECK(ctl->peak_c <= sim_rk3588.passive_mc / 1000.0 - 1.0 + 0.05,
	      "%s/%s: startup-inclusive peak %.1f C violates the margin", name, who, ctl->peak_c);
}

static const struct held_out_case {
	const char *name, *pattern;
	int cooling;
	double ambient_c, start_c;
	uint64_t seed;
} held_out[] = {
	{ "full passive, seed 7", "full", 0, 25, 25, 7 },
	{ "full bare, seed 7", "full", 2, 25, 25, 7 },
	{ "burst bare, seed 7", "burst", 2, 25, 25, 7 },
	{ "burst bare, seed 99", "burst", 2, 25, 25, 99 },
	{ "full passive, 35 C", "full", 0, 35, 35, 42 },
	{ "full bare, 35 C", "full", 2, 35, 35, 42 },
	{ "burst bare, 35 C", "burst", 2, 35, 35, 42 },
	{ "full bare, 15 C", "full", 2, 15, 15, 42 },
	{ "full passive, warm 60", "full", 0, 25, 60, 42 },
	{ "full bare, warm 70", "full", 2, 25, 70, 42 },
	{ "burst bare, warm 70", "burst", 2, 25, 70, 42 },
	{ "10 s bursts, bare", "burst10", 2, 25, 25, 42 },
	{ "300 ms bursts, bare", "burst300ms", 2, 25, 25, 42 },
	{ "10 s bursts, passive", "burst10", 0, 25, 25, 42 },
	{ "load steps, bare", "steps", 2, 25, 25, 42 },
	{ "load steps, passive", "steps", 0, 25, 25, 42 },
	{ "one cluster, bare", "onebig", 2, 25, 25, 42 },
	{ "full, fan", "full", 1, 25, 25, 42 },
	{ "burst, fan, 35 C", "burst", 1, 35, 35, 42 },
};

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cross-cluster")) joint_mpc = true;
		else if (!strcmp(argv[i], "--cooperative")) cooperative = true;
		else {
			fprintf(stderr, "usage: test_closed_loop [--cross-cluster] [--cooperative]\n");
			return 1;
		}
	}
	const double secs = 900;
	struct {
		const char *name, *pattern;
		int cooling;
	} cases[] = {
		{ "full load, passive", "full", 0 },
		{ "big-core load, passive", "big", 0 },
		{ "full load, bare board", "full", 2 },
		{ "burst load, bare board", "burst", 2 },
	};
	printf("| scenario               | controller | peak C | s >85C  | kernel | ceil   | freq     | mean W  | served  |\n");
	printf("|------------------------|------------|--------|---------|--------|--------|----------|---------|---------|\n");
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct result base = run(cases[i].pattern, cases[i].cooling, false, LW_POLICY_RULE, secs);
		struct result rule = run(cases[i].pattern, cases[i].cooling, true, LW_POLICY_RULE, secs);
		struct result mpc = run(cases[i].pattern, cases[i].cooling, true, LW_POLICY_MPC, secs);

		report(cases[i].name, "off", &base);
		report(cases[i].name, "rule", &rule);
		report(cases[i].name, "learned", &mpc);
		for (int pass = 0; pass < 2; pass++) {
			const struct result ctl = pass ? mpc : rule;
			const char *who = pass ? "learned" : "rule";

			check_safe(cases[i].name, who, &ctl);
			/* Requirement 2: no worse than the kernel alone where it matters. */
			CHECK(ctl.peak_c_after_warmup <= base.peak_c_after_warmup + 0.5,
			      "%s/%s: peak %.1f C above baseline %.1f C", cases[i].name, who,
			      ctl.peak_c_after_warmup, base.peak_c_after_warmup);
			/* Requirement 3: no hunting, under 2 ceiling moves per minute per cluster. */
			CHECK(ctl.ceiling_changes <= 3 * 2 * (unsigned long long)(secs / 60) + 20,
			      "%s/%s: %llu ceiling changes", cases[i].name, who, ctl.ceiling_changes);
		}
	}

	/* Cases the learned policy's parameters were not tuned on. */
	printf("\n| held-out case          | rule: peak / moves / served | learned: peak / moves / served |\n");
	printf("|------------------------|-----------------------------|--------------------------------|\n");
	for (size_t i = 0; i < sizeof(held_out) / sizeof(held_out[0]); i++) {
		const struct held_out_case *h = &held_out[i];
		struct result rule = run_from(h->pattern, h->cooling, h->ambient_c, h->start_c, h->seed,
					      true, LW_POLICY_RULE, secs);
		struct result mpc = run_from(h->pattern, h->cooling, h->ambient_c, h->start_c, h->seed,
					     true, LW_POLICY_MPC, secs);

		printf("| %-22s | %5.1f / %4llu / %6.2f%%      | %5.1f / %4llu / %6.2f%%         |\n",
		       h->name, rule.peak_c_after_warmup, rule.ceiling_changes, 100 * rule.served,
		       mpc.peak_c_after_warmup, mpc.ceiling_changes, 100 * mpc.served);
		check_safe(h->name, "rule", &rule);
		check_safe(h->name, "learned", &mpc);
		CHECK(mpc.ceiling_changes <= 3 * 2 * (unsigned long long)(secs / 60) + 20,
		      "%s/learned: %llu ceiling changes", h->name, mpc.ceiling_changes);
	}
	printf("\npeak C: hottest passive zone after 300 s. kernel: step_wise throttle steps. "
	       "ceil: Lerwatt ceiling moves. served: share of demanded work done.\n");
	/* Start hot enough to expose the first seconds, before any model is ready.
	 * The one-degree margin applies from startup, not just after 300 seconds.
	 */
	for (int cooling = 0; cooling < 3; cooling++) {
		for (int policy = LW_POLICY_RULE; policy <= LW_POLICY_MPC; policy++) {
			const uint64_t seeds[] = { 42, 7, 99 };

			for (int seed = 0; seed < 3; seed++) {
				for (int burst = 0; burst < 2; burst++) {
					const char *pattern = burst ? "burst" : "full";
					struct result hot = run_from(pattern, cooling, 25, 79, seeds[seed],
								     true, policy, 60);

					printf("hot start cooling=%d policy=%d seed=%llu %s: peak %.1f C, %llu throttle steps\n",
					       cooling, policy, (unsigned long long)seeds[seed], pattern,
					       hot.peak_c, hot.kernel_throttle_steps);
					CHECK(hot.peak_c <= 84.05 && !hot.kernel_throttle_steps,
					      "hot startup cooling=%d policy=%d seed=%llu %s: peak %.1f C, throttles %llu",
					      cooling, policy, (unsigned long long)seeds[seed], pattern,
					      hot.peak_c, hot.kernel_throttle_steps);
				}
			}
		}
	}
	if (failures) {
		fprintf(stderr, "test_closed_loop: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_closed_loop: all checks passed\n");
	return 0;
}
