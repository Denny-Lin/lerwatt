// SPDX-License-Identifier: MIT
/* Prospective five-second forecasts. Score saved predictions before observing
 * their future labels; persistence sees exactly the same current sensors.
 * This measures prediction error, independently of controller throughput.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lerwatt/learn.h"
#include "sim.h"

#define HORIZON 50 /* 100 ms samples, five seconds */

struct prediction {
	bool valid;
	double package, hot, package_base, hot_base, package_const, hot_const;
};

static int evaluate(const char *name, double burst_s, int cooling, double ambient, uint64_t seed)
{
	const struct sim_platform *p = &sim_rk3588;
	struct sim_state s;
	struct lw_thermal_model m[SIM_MAX_CLUSTERS];
	struct prediction q[SIM_MAX_CLUSTERS][HORIZON] = { 0 };
	double previous_busy[SIM_MAX_CPUS] = { 0 };
	double pkg_error = 0, pkg_base = 0, hot_error = 0, hot_base = 0, hot_sq = 0;
	double pkg_const = 0, hot_const = 0;
	unsigned long samples = 0;

	sim_init(&s, p, cooling, ambient, ambient, 1.0, seed);
	for (int k = 0; k < p->nclusters; k++)
		lw_tm_init(&m[k]);
	for (int tick = 0; tick < 120000; tick++) {
		double d[SIM_MAX_CPUS] = { 0 };

		for (int cpu = 0; cpu < p->ncpus; cpu++)
			d[cpu] = burst_s > 0 ? (fmod(s.t_s, 2 * burst_s) < burst_s ? 1 : .05) :
				((int)(s.t_s / 120) % 4 + 1) * .25;
		sim_step(&s, d, .01);
		if ((tick + 1) % 10)
			continue;
		int period = (tick + 1) / 10, slot = period % HORIZON;
		double pkg = s.zone[0].sensor_mc / 1000.0;
		double power[SIM_MAX_CLUSTERS] = { 0 }, total = 0;

		for (int k = 0; k < p->nclusters; k++) {
			const struct sim_cluster_desc *c = &p->clusters[k];

			for (int i = 0; i < c->ncpus; i++) {
				int cpu = c->first_cpu + i;
				double busy = (s.cpu[cpu].stat_busy_s - previous_busy[cpu]) / .1;

				power[k] += busy * sim_em_power_uw(c, s.cl[k].cur_idx) * 1e-6;
				previous_busy[cpu] = s.cpu[cpu].stat_busy_s;
			}
			total += power[k];
		}
		for (int k = 0; k < p->nclusters; k++) {
			double hot = 0;
			struct prediction *f = &q[k][slot];

			for (int z = 0; z < p->nzones; z++)
				if (p->zones[z].passive && p->zones[z].cluster == k)
					hot = s.zone[z].sensor_mc / 1000.0;
			if (f->valid) {
				pkg_error += fabs(f->package - pkg);
				pkg_base += fabs(f->package_base - pkg);
				hot_error += fabs(f->hot - hot);
				hot_base += fabs(f->hot_base - hot);
				hot_sq += (f->hot - hot) * (f->hot - hot);
				pkg_const += fabs(f->package_const - pkg);
				hot_const += fabs(f->hot_const - hot);
				samples++;
			}
			lw_tm_observe(&m[k], hot, pkg, power[k], total - power[k], .1);
			f->valid = period >= 3000 && lw_tm_ready(&m[k]);
			if (f->valid) {
				f->package_const = lw_tm_package(&m[k], pkg, total, 5);
				f->hot_const = f->package_const + lw_tm_offset(&m[k], power[k]);
				f->package = lw_tm_forecast_package(&m[k], pkg, 5);
				f->hot = lw_tm_forecast_hot(&m[k], pkg, 5);
				f->package_base = pkg;
				f->hot_base = hot;
				if (!isfinite(f->package) || !isfinite(f->hot))
					return 1;
			}
		}
	}
	if (samples < 1000 || !isfinite(hot_sq))
		return 1;
	printf("{\"case\":\"%s\",\"horizon_s\":5,\"samples\":%lu,"
	       "\"package_mae_c\":%.4f,\"package_persistence_mae_c\":%.4f,"
	       "\"hot_mae_c\":%.4f,\"hot_persistence_mae_c\":%.4f,\"hot_rmse_c\":%.4f,"
	       "\"constant_package_mae_c\":%.4f,\"constant_hot_mae_c\":%.4f}\n",
	       name, samples, pkg_error / samples, pkg_base / samples,
	       hot_error / samples, hot_base / samples, sqrt(hot_sq / samples), pkg_const / samples, hot_const / samples);
	return 0;
}

int main(void)
{
	int failed = 0;

	failed |= evaluate("steps-passive-seed7", 0, 0, 25, 7);
	failed |= evaluate("burst2-bare-seed99", 2, 2, 25, 99);
	failed |= evaluate("burst03-fan-35C", .3, 1, 35, 7);
	/* Additional seeds and environments reserved after the forecasting rule
	 * above was fixed; do not tune thresholds to these results.
	 */
	failed |= evaluate("steps-bare-15C-seed13579", 0, 2, 15, 13579);
	failed |= evaluate("burst2-passive-35C-seed2026", 2, 0, 35, 2026);
	failed |= evaluate("burst03-bare-seed2718", .3, 2, 25, 2718);
	if (failed)
		fprintf(stderr, "test_forecast: insufficient or non-finite forecasts\n");
	return failed;
}
