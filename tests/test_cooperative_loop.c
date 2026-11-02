// SPDX-License-Identifier: MIT
/* Same finite FIFO jobs for A/B. The plant and its counterfactual truth never
 * enter the predictor. This is a simulated board, not hardware energy data. */
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lerwatt/controller.h"
#include "sim.h"

#define JOBS 360
#define CPUS 8
struct job { double arrival, remaining; };
struct measurement {
	double makespan, energy, peak, p99, model_mae, point_mae, control_cpu_s;
	double offered_work;
	uint64_t model_n, point_n, corrections, queries, moves, transitions, throttles;
};
struct saved_point { uint64_t due; double value; bool valid; };

static int order(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static double work(int pattern, int cpu, int second)
{
	double cap = sim_capacity(&sim_rk3588, cpu);
	switch (pattern) {
	case 0: return .8 * cap; /* sustained, capacity-normalized requests */
	case 1: return (second / 2 % 2 ? .05 : 1.0) * 1024;
	case 2: return (cpu < 4 ? .9 : .5) * cap;
	case 4: return .99 * 1024; /* sustained queue during an ambient ramp */
	default: return ((second * 1103515245u + cpu * 12345u + 7u) % 1000) / 1000.0 * cap;
	}
}

static struct measurement run(int pattern, int cooling, uint64_t seed, bool joint, bool cooperative)
{
	const struct sim_platform *platform = &sim_rk3588;
	struct lw_opp opp[SIM_MAX_CLUSTERS][SIM_MAX_OPPS];
	struct lw_ctl_state state[SIM_MAX_CLUSTERS] = { 0 };
	struct lw_ctl_params params;
	struct sim_state sim;
	struct measurement m = { 0 };
	struct job jobs[CPUS][JOBS];
	int head[CPUS] = { 0 }, arrived = 0, completed = 0;
	double delay[CPUS * JOBS], previous_busy[CPUS] = { 0 }, estimated[SIM_MAX_CLUSTERS] = { 0 };
	struct saved_point point[SIM_MAX_CLUSTERS][64] = { 0 };
	struct saved_point nominal[SIM_MAX_CLUSTERS][64] = { 0 };
	unsigned int sample = 0;
	int delays = 0;
	sim_init(&sim, platform, cooling, 25, 25, 1, seed);
	lw_ctl_default_params(&params, platform->passive_mc);
	params.policy = LW_POLICY_MPC; params.cross_cluster = joint;
	params.cooperative_prediction = cooperative;
	for (int k = 0; k < platform->nclusters; k++)
		for (int i = 0; i < platform->clusters[k].nopps; i++) {
			opp[k][i] = (struct lw_opp) {
				.khz = platform->clusters[k].opps[i].khz,
				.power_uw = sim_em_power_uw(&platform->clusters[k], i),
				.perf = sim_em_perf(platform, k, i),
			};
		}
	for (uint64_t tick = 0; tick < 240000; tick++) {
		if (pattern == 4)
			sim.ambient_c = 25 + fmin(10, fmax(0, sim.t_s - 150) * .02);
		/* Arrivals are clocked independently of frequency and controller. */
		if (arrived < JOBS && tick >= (uint64_t)arrived * 100) {
			for (int cpu = 0; cpu < CPUS; cpu++) {
				jobs[cpu][arrived] = (struct job) { .arrival = sim.t_s,
					.remaining = work(pattern, cpu, arrived) };
				m.offered_work += jobs[cpu][arrived].remaining;
			}
			arrived++;
		}
		double demand[CPUS], service[CPUS];
		for (int cpu = 0; cpu < CPUS; cpu++) {
			double pending = 0;
			for (int j = head[cpu]; j < arrived; j++) pending += jobs[cpu][j].remaining;
			demand[cpu] = fmin(1, pending / (.01 * 1024));
			int k = sim_cluster_of_cpu(platform, cpu);
			const struct sim_cluster_desc *c = &platform->clusters[k];
			double capacity = sim.capacity[cpu] * (double)c->opps[sim.cl[k].cur_idx].khz /
				c->opps[c->nopps - 1].khz;
			service[cpu] = fmin(demand[cpu] * 1024, capacity) * .01;
		}
		sim_step(&sim, demand, .01);
		assert(!sim.critical);
		for (int cpu = 0; cpu < CPUS; cpu++) {
			double served = service[cpu];
			while (head[cpu] < arrived && jobs[cpu][head[cpu]].remaining <= served + 1e-9) {
				struct job *j = &jobs[cpu][head[cpu]];
				served = fmax(0, served - j->remaining); j->remaining = 0;
				delay[delays++] = sim.t_s - j->arrival;
				head[cpu]++; completed++;
			}
			if (head[cpu] < arrived) jobs[cpu][head[cpu]].remaining -= served;
		}
		for (int z = 0; z < platform->nzones; z++)
			if (platform->zones[z].passive) m.peak = fmax(m.peak, sim.zone[z].sensor_mc / 1000.0);
		if (completed == CPUS * JOBS) break;
		if ((tick + 1) % 10) continue;
		struct lw_ctl_in in[SIM_MAX_CLUSTERS];
		struct lw_ctl_out out[SIM_MAX_CLUSTERS];
		for (int k = 0; k < platform->nclusters; k++) {
			const struct sim_cluster_desc *c = &platform->clusters[k];
			int zone = -1;
			for (int z = 0; z < platform->nzones; z++)
				if (platform->zones[z].cluster == k && platform->zones[z].passive) zone = z;
			assert(zone >= 0);
			in[k] = (struct lw_ctl_in) {
				.temp_mc = sim.zone[zone].sensor_mc, .package_mc = sim.zone[0].sensor_mc,
				.cur_khz = c->opps[sim.cl[k].cur_idx].khz,
				.effective_max_khz = c->opps[sim_eff_max_idx(&sim, k)].khz,
				.ncpus = c->ncpus, .opps = opp[k], .nopps = c->nopps,
				.capacity = sim.capacity[c->first_cpu], .lat_crit = -1,
			};
			for (int j = 0; j < platform->nclusters; j++) if (j != k) in[k].other_power_w += estimated[j];
			for (int j = 0; j < c->ncpus; j++) {
				int cpu = c->first_cpu + j;
				in[k].busy_mean += (sim.cpu[cpu].stat_busy_s - previous_busy[cpu]) / .1;
				previous_busy[cpu] = sim.cpu[cpu].stat_busy_s;
			}
			in[k].busy_mean /= c->ncpus;
			unsigned int slot = sample % 64;
			if (point[k][slot].valid) {
				assert(point[k][slot].due == sample);
				m.point_mae += fabs(point[k][slot].value - in[k].temp_mc / 1000.0); m.point_n++;
			}
			if (nominal[k][slot].valid) {
				assert(nominal[k][slot].due == sample);
				m.model_mae += fabs(nominal[k][slot].value - in[k].temp_mc / 1000.0); m.model_n++;
			}
		}
		clock_t begin = clock();
		lw_ctl_step_group(&params, in, state, out, platform->nclusters,
				  (uint64_t)llround(sim.t_s * 1e9), .1);
		m.control_cpu_s += (double)(clock() - begin) / CLOCKS_PER_SEC;
		for (int k = 0; k < platform->nclusters; k++) {
			/* 50-period horizon; ring is 64 but target is recorded explicitly. */
			unsigned int current = sample % 64, future = (sample + 50) % 64;
			point[k][current].valid = nominal[k][current].valid = false;
			point[k][future] = (struct saved_point) { sample + 50, out[k].forecast_c,
				isfinite(out[k].forecast_c) };
			nominal[k][future] = (struct saved_point) { sample + 50, out[k].forecast_base_c,
				isfinite(out[k].forecast_base_c) };
			sim.cl[k].user_max_khz = out[k].ceiling_khz;
			estimated[k] = out[k].est_power_w;
		}
		sample++;
	}
	assert(completed == CPUS * JOBS && delays == CPUS * JOBS);
	assert(fabs(sim.work_done - m.offered_work) < 1e-7 * m.offered_work);
	assert(m.peak <= 84.05 && sim.throttle_events == 0);
	m.makespan = sim.t_s; m.energy = sim.energy_j; m.throttles = sim.throttle_events;
	qsort(delay, delays, sizeof(delay[0]), order); m.p99 = delay[(delays * 99 + 99) / 100 - 1];
	if (m.point_n) m.point_mae /= m.point_n;
	if (m.model_n) m.model_mae /= m.model_n;
	for (int k = 0; k < platform->nclusters; k++) {
		m.corrections += state[k].prediction_corrections; m.queries += state[k].prediction_queries;
		m.moves += state[k].ceiling_changes; m.transitions += sim.cl[k].total_trans;
	}
	return m;
}

int main(int argc, char **argv)
{
	bool full = argc == 2 && !strcmp(argv[1], "--full");
	assert(argc == 1 || full);
	const uint64_t seeds[] = { 42, 7, 99 };
	for (int joint = 0; joint <= 1; joint++)
		for (int pattern = 0; pattern < 5; pattern++)
			for (int seed = 0; seed < (full ? 3 : 1); seed++)
				for (int mode = 0; mode < 2; mode++) {
					struct measurement m = run(pattern, 2, seeds[seed], joint, mode);
					printf("{\"pattern\":%d,\"seed\":%llu,\"joint\":%s,\"mode\":\"%s\","
					       "\"jobs_completed\":%d,\"backlog\":0,\"offered_work\":%.6f,\"throttle_steps\":%llu,\"makespan_s\":%.3f,\"simulated_board_j\":%.6f,"
					       "\"peak_c\":%.3f,\"job_p99_s\":%.3f,\"point_mae_c\":%.6f,\"reference_mae_c\":%.6f,"
					       "\"forecast_points\":%llu,\"correction_queries\":%llu,\"queries\":%llu,"
					       "\"ceiling_moves\":%llu,\"frequency_transitions\":%llu,\"control_cpu_s\":%.6f}\n",
					       pattern, (unsigned long long)seeds[seed], joint ? "true" : "false", mode ? "cooperative" : "nominal",
					       CPUS * JOBS, m.offered_work, (unsigned long long)m.throttles,
					       m.makespan, m.energy, m.peak, m.p99, m.point_mae, m.model_mae,
					       (unsigned long long)m.point_n, (unsigned long long)m.corrections, (unsigned long long)m.queries,
					       (unsigned long long)m.moves, (unsigned long long)m.transitions, m.control_cpu_s);
				}
	return 0;
}
