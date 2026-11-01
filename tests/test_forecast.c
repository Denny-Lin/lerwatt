// SPDX-License-Identifier: MIT
/* Prospective five-second forecasts. Score saved predictions before observing
 * their future labels; persistence sees exactly the same current sensors.
 * This measures prediction error, independently of controller throughput.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lerwatt/learn.h"
#include "lerwatt/forecast.h"
#include "sim.h"

#define HORIZON 50 /* 100 ms samples, five seconds */
static unsigned int replicate;

struct prediction {
	bool valid;
	bool point_valid;
	double package, hot, package_base, hot_base, package_const, hot_const;
	double adaptive_package, adaptive_hot;
	unsigned int selected;
};

static int evaluate(const char *name, double burst_s, int cooling, double ambient, uint64_t seed, int pattern)
{
	struct sim_platform profile = sim_rk3588;
	const struct sim_platform *p = &profile;
	struct sim_state s;
	struct lw_thermal_model m[SIM_MAX_CLUSTERS];
	struct lw_forecaster forecaster[SIM_MAX_CLUSTERS] = { 0 };
	struct prediction q[SIM_MAX_CLUSTERS][HORIZON] = { 0 };
	double previous_busy[SIM_MAX_CPUS] = { 0 };
	double pkg_error = 0, pkg_base = 0, hot_error = 0, hot_base = 0, hot_sq = 0;
	double pkg_const = 0, hot_const = 0;
	double adaptive_pkg = 0, adaptive_hot = 0, adaptive_sq = 0;
	double hot_base_sq = 0;
	double all_hot = 0, all_base = 0, all_sq = 0;
	unsigned long all_samples = 0;
	unsigned long selected[LW_FORECAST_EXPERTS + 1] = { 0 };
	unsigned long samples = 0;

	seed ^= (uint64_t)replicate * 0x9e3779b97f4a7c15ull;
	if (pattern == 12)
		profile.tsadc_noise_c = .6;
	sim_init(&s, p, cooling, ambient, ambient, 1.0, seed);
	for (int k = 0; k < p->nclusters; k++)
		lw_tm_init(&m[k]);
	for (int tick = 0; tick < 120000; tick++) {
		double d[SIM_MAX_CPUS] = { 0 };
		if (pattern == 11 && s.t_s >= 600)
			s.cool = &p->cooling[1];

		for (int cpu = 0; cpu < p->ncpus; cpu++) {
			d[cpu] = burst_s > 0 ? (fmod(s.t_s, 2 * burst_s) < burst_s ? 1 : .05) :
				((int)(s.t_s / 120) % 4 + 1) * .25;
			if (pattern == 1)
				d[cpu] = .1 + .8 * fabs(fmod(s.t_s, 240) / 120 - 1);
			else if (pattern == 2)
				d[cpu] = ((int)(s.t_s / 60) + sim_cluster_of_cpu(p, cpu)) % 3 ? .05 : 1;
			else if (pattern == 3)
				d[cpu] = s.t_s < 600 ? (fmod(s.t_s, 4) < 2 ? 1 : .05) :
					s.t_s < 900 ? ((int)(s.t_s / 120) % 4 + 1) * .25 :
					(fmod(s.t_s, .6) < .3 ? 1 : .05);
			else if (pattern == 4)
				d[cpu] = fmod(s.t_s, 10) < 2 ? 1 : .05;
			else if (pattern == 5 || pattern == 8) {
				uint64_t hash = ((uint64_t)(s.t_s / (pattern == 8 ? 13 : 30)) + seed) * 6364136223846793005ull + 1;

				d[cpu] = .1 + .9 * ((hash >> 32) % 1000) / 999.0;
			}
			else if (pattern == 6) {
				double period_s = .8 + 1.8 * ((int)(s.t_s / 120) % 5);

				d[cpu] = fmod(s.t_s, period_s) < period_s / 2 ? 1 : .05;
			} else if (pattern == 7) {
				int cluster = sim_cluster_of_cpu(p, cpu);

				d[cpu] = .1 + .3 * (((int)(s.t_s / 60) + cluster) % 4);
			} else if (pattern == 9)
				d[cpu] = fmod(s.t_s, 20) < 2 ? 1 : .02;
			else if (pattern == 10)
				d[cpu] = .5 + .45 * sin(s.t_s * (2 * acos(-1) / 90) + cpu / 4.0);
			else if (pattern == 11)
				d[cpu] = .7;
			else if (pattern == 13)
				d[cpu] = fmod(s.t_s, 7) < 2 ? 1 : .05;
		}
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
			if (f->point_valid) {
				all_hot += fabs(f->adaptive_hot - hot);
				all_base += fabs(f->hot_base - hot);
				all_sq += (f->adaptive_hot - hot) * (f->adaptive_hot - hot);
				all_samples++;
			}
			if (f->valid) {
				pkg_error += fabs(f->package - pkg);
				pkg_base += fabs(f->package_base - pkg);
				hot_error += fabs(f->hot - hot);
				hot_base += fabs(f->hot_base - hot);
				hot_base_sq += (f->hot_base - hot) * (f->hot_base - hot);
				hot_sq += (f->hot - hot) * (f->hot - hot);
				pkg_const += fabs(f->package_const - pkg);
				hot_const += fabs(f->hot_const - hot);
				adaptive_pkg += fabs(f->adaptive_package - pkg);
				adaptive_hot += fabs(f->adaptive_hot - hot);
				adaptive_sq += (f->adaptive_hot - hot) * (f->adaptive_hot - hot);
				selected[f->selected]++;
				samples++;
			}
			lw_tm_observe(&m[k], hot, pkg, power[k], total - power[k], .1);
			double observed[2] = { pkg, hot };
			double candidate[2][3] = { { pkg, NAN, NAN }, { hot, NAN, NAN } };

			if (lw_tm_ready(&m[k])) {
				candidate[0][1] = lw_tm_package(&m[k], pkg, total, 5);
				candidate[1][1] = candidate[0][1] + lw_tm_offset(&m[k], power[k]);
				candidate[0][2] = lw_tm_forecast_package(&m[k], pkg, 5);
				candidate[1][2] = lw_tm_forecast_hot(&m[k], pkg, 5);
			}
			lw_forecast_step(&forecaster[k], (uint64_t)period * 100000000ull, observed, candidate);
			f->point_valid = period >= 3000;
			f->adaptive_package = forecaster[k].prediction[LW_FORECAST_PACKAGE];
			f->adaptive_hot = forecaster[k].prediction[LW_FORECAST_HOT];
			f->selected = forecaster[k].selected[LW_FORECAST_HOT];
			f->package_base = pkg;
			f->hot_base = hot;
			f->valid = period >= 3000 && lw_tm_ready(&m[k]);
			if (f->valid) {
				f->package_const = lw_tm_package(&m[k], pkg, total, 5);
				f->hot_const = f->package_const + lw_tm_offset(&m[k], power[k]);
				f->package = lw_tm_forecast_package(&m[k], pkg, 5);
				f->hot = lw_tm_forecast_hot(&m[k], pkg, 5);
				if (!isfinite(f->package) || !isfinite(f->hot) ||
				    !isfinite(f->adaptive_package) || !isfinite(f->adaptive_hot))
					return 1;
			}
		}
	}
	if (samples < 1000 || !isfinite(hot_sq))
		return 1;
	printf("{\"case\":\"%s\",\"seed\":%llu,\"replicate\":%u,\"horizon_s\":5,\"samples\":%lu,"
	       "\"package_mae_c\":%.4f,\"package_persistence_mae_c\":%.4f,"
	       "\"hot_mae_c\":%.4f,\"hot_persistence_mae_c\":%.4f,\"hot_rmse_c\":%.4f,"
	       "\"constant_package_mae_c\":%.4f,\"constant_hot_mae_c\":%.4f,"
	       "\"adaptive_package_mae_c\":%.4f,\"adaptive_hot_mae_c\":%.4f,\"adaptive_hot_rmse_c\":%.4f,"
	       "\"hot_persistence_rmse_c\":%.4f,"
	       "\"all_samples\":%lu,\"all_adaptive_hot_mae_c\":%.4f,\"all_persistence_hot_mae_c\":%.4f,\"all_adaptive_hot_rmse_c\":%.4f,"
	       "\"selected\":[%lu,%lu,%lu,%lu]}\n",
	       name, (unsigned long long)seed, replicate, samples, pkg_error / samples, pkg_base / samples,
	       hot_error / samples, hot_base / samples, sqrt(hot_sq / samples), pkg_const / samples, hot_const / samples,
	       adaptive_pkg / samples, adaptive_hot / samples, sqrt(adaptive_sq / samples), sqrt(hot_base_sq / samples),
	       all_samples, all_hot / all_samples, all_base / all_samples, sqrt(all_sq / all_samples),
	       selected[0], selected[1], selected[2], selected[3]);
	return 0;
}

int main(int argc, char **argv)
{
	int failed = 0;
	if (argc == 4 && !strcmp(argv[2], "--replicate")) {
		char *end;
		unsigned long value = strtoul(argv[3], &end, 10);

		if (!argv[3][0] || *end || value > 10000)
			return 1;
		replicate = (unsigned int)value;
		argc = 2;
	}

	bool legacy = argc == 1 || (argc == 2 && !strcmp(argv[1], "--legacy"));
	bool development = argc == 1 || (argc == 2 && !strcmp(argv[1], "--development"));
	bool validation = argc == 1 || (argc == 2 && !strcmp(argv[1], "--validation"));

	if (!legacy && !development && !validation) {
		fprintf(stderr, "usage: test_forecast [--legacy|--development|--validation] [--replicate N]\n");
		return 1;
	}
	if (legacy) {
		failed |= evaluate("steps-passive-seed7", 0, 0, 25, 7, 0);
		failed |= evaluate("burst2-bare-seed99", 2, 2, 25, 99, 0);
		failed |= evaluate("burst03-fan-35C", .3, 1, 35, 7, 0);
		failed |= evaluate("steps-bare-15C-seed13579", 0, 2, 15, 13579, 0);
		failed |= evaluate("burst2-passive-35C-seed2026", 2, 0, 35, 2026, 0);
		failed |= evaluate("burst03-bare-seed2718", .3, 2, 25, 2718, 0);
	}
	if (development) {
		failed |= evaluate("ramp-passive-seed424242", 0, 0, 25, 424242, 1);
		failed |= evaluate("alternating-clusters-bare-seed619", 0, 2, 25, 619, 2);
		failed |= evaluate("regime-changes-bare-seed701", 0, 2, 25, 701, 3);
		failed |= evaluate("duty20-bare-35C-seed883", 0, 2, 35, 883, 4);
		failed |= evaluate("random-steps-fan-15C-seed9959", 0, 1, 15, 9959, 5);
		failed |= evaluate("burst45-passive-seed1701", 45, 0, 25, 1701, 0);
	}
	/* Not used to choose the blended forecaster or its delay-derived step. */
	if (validation) {
		failed |= evaluate("varying-period-passive-30C-seed104729", 0, 0, 30, 104729, 6);
		failed |= evaluate("asymmetric-steps-bare-20C-seed130363", 0, 2, 20, 130363, 7);
		failed |= evaluate("random-fast-bare-35C-seed155921", 0, 2, 35, 155921, 8);
		failed |= evaluate("sparse-bursts-fan-seed196613", 0, 1, 25, 196613, 9);
		failed |= evaluate("sine-passive-15C-seed262147", 0, 0, 15, 262147, 10);
		failed |= evaluate("cooling-change-35C-seed327673", 0, 0, 35, 327673, 11);
		failed |= evaluate("noisy-burst-bare-seed393241", 2, 2, 25, 393241, 12);
		failed |= evaluate("duty2of7-bare-seed458879", 0, 2, 25, 458879, 13);
	}
	if (failed)
		fprintf(stderr, "test_forecast: insufficient or non-finite forecasts\n");
	return failed;
}
