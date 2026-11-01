// SPDX-License-Identifier: MIT
/* Score two causal point models on one unchanged controller trajectory.
 * Sensors and estimated CPU power are the only observations. Predictions
 * are saved before their five-second labels exist. Simulator state never
 * enters either fitted model except through those public observations.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lerwatt/controller.h"
#include "lerwatt/temporal.h"
#include "sim.h"

#define HORIZON 50

struct profile {
	const char *name;
	int pattern, cooling;
	double ambient, period;
	uint64_t seed;
};

struct saved {
	bool score;
	double baseline, current, candidate;
};

struct score {
	unsigned long n, ready;
	double absolute, squared;
};

static unsigned int replicate;
static bool temporal;
static bool integrated;

static void add(struct score *s, double prediction, double label)
{
	double error = prediction - label;

	s->n++;
	s->absolute += fabs(error);
	s->squared += error * error;
}

static int zone_of_cluster(const struct sim_platform *p, int cluster)
{
	for (int z = 0; z < p->nzones; z++)
		if (p->zones[z].passive && p->zones[z].cluster == cluster)
			return z;
	return -1;
}

static void demand(const struct profile *f, const struct sim_platform *p, double t,
		   uint64_t seed, double *d)
{
	for (int cpu = 0; cpu < p->ncpus; cpu++) {
		int cluster = sim_cluster_of_cpu(p, cpu);
		double v;

		switch (f->pattern) {
		case 0: v = ((int)(t / 120) % 4 + 1) * .25; break;
		case 1: v = fmod(t, f->period) < f->period / 2 ? 1 : .05; break;
		case 2: v = .1 + .3 * (((int)(t / 47) + cluster) % 4); break;
		case 3: v = .5 + .45 * sin(t * (2 * acos(-1) / f->period) + cpu / 4.0); break;
		case 4: v = fmod(t + cluster * .4, f->period) < 1.2 ? 1 : .02; break;
		case 5: {
			uint64_t hash = ((uint64_t)(t / f->period) + seed + cluster * 17) *
				6364136223846793005ull + 1;

			v = .1 + .9 * ((hash >> 32) % 1000) / 999.0;
			break;
		}
		case 6: v = t < 600 ? (fmod(t, 4) < 2 ? 1 : .05) :
			t < 900 ? .8 : (fmod(t, .6) < .3 ? 1 : .05); break;
		case 8: {
			uint64_t hash = ((uint64_t)(t / 137) + seed) * 6364136223846793005ull + 1;
			double phase = ((hash >> 32) % 1000) * f->period / 1000.0;
			v = fmod(t + phase, f->period) < f->period / 2 ? 1 : .05;
			break;
		}
		case 9: {
			double duty = .15 + .15 * ((int)(t / 97) % 5);
			v = fmod(t, f->period) < duty * f->period ? 1 : .05;
			break;
		}
		default: v = .7; break;
		}
		d[cpu] = v;
	}
}

static int evaluate(const struct profile *f)
{
	const struct sim_platform *p = &sim_rk3588;
	struct sim_state s;
	struct lw_ctl_params params;
	struct lw_ctl_state state[SIM_MAX_CLUSTERS] = { 0 };
	struct lw_thermal_model point[SIM_MAX_CLUSTERS];
	struct lw_forecaster forecast[SIM_MAX_CLUSTERS] = { 0 };
	struct lw_temporal lag[SIM_MAX_CLUSTERS] = { 0 };
	struct lw_opp opps[SIM_MAX_CLUSTERS][SIM_MAX_OPPS];
	struct saved pending[SIM_MAX_CLUSTERS][HORIZON] = { 0 };
	struct score scores[3] = { 0 };
	double previous_busy[SIM_MAX_CPUS] = { 0 }, estimated[SIM_MAX_CLUSTERS] = { 0 };
	double peak_c = 0;
	unsigned long old_ready = 0, new_ready = 0;
	uint64_t seed = f->seed ^ (uint64_t)replicate * 0x9e3779b97f4a7c15ull;

	sim_init(&s, p, f->cooling, f->ambient, f->ambient, 1.0, seed);
	lw_ctl_default_params(&params, p->passive_mc);
	params.policy = LW_POLICY_MPC;
	params.temporal_forecast = integrated;
	for (int k = 0; k < p->nclusters; k++) {
		lw_tm_init(&point[k]);
		for (int i = 0; i < p->clusters[k].nopps; i++)
			opps[k][i] = (struct lw_opp){
				.khz = p->clusters[k].opps[i].khz,
				.power_uw = sim_em_power_uw(&p->clusters[k], i),
				.perf = sim_em_perf(p, k, i),
			};
	}
	for (int tick = 0; tick < 120000; tick++) {
		double load[SIM_MAX_CPUS];
		demand(f, p, s.t_s, seed, load);
		if (f->pattern == 7 && s.t_s >= 600)
			s.cool = &p->cooling[1];
		sim_step(&s, load, .01);
		for (int z = 0; z < p->nzones; z++)
			if (p->zones[z].passive)
				peak_c = fmax(peak_c, s.zone[z].sensor_mc / 1000.0);
		if (s.critical)
			return 1;
		if ((tick + 1) % 10)
			continue;
		int period = (tick + 1) / 10, slot = period % HORIZON;
		struct lw_ctl_in inputs[SIM_MAX_CLUSTERS];
		struct lw_ctl_out outputs[SIM_MAX_CLUSTERS];
		for (int k = 0; k < p->nclusters; k++) {
			const struct sim_cluster_desc *c = &p->clusters[k];
			struct lw_ctl_in in = {
				.temp_mc = s.zone[zone_of_cluster(p, k)].sensor_mc,
				.package_mc = s.zone[0].sensor_mc,
				.cur_khz = c->opps[s.cl[k].cur_idx].khz,
				.ncpus = c->ncpus, .opps = opps[k], .nopps = c->nopps,
				.capacity = s.capacity[c->first_cpu],
				.deep_idle_latency_us = (int32_t)p->idle[p->nidle - 1].latency_us,
				.lat_crit = -1,
			};
			for (int j = 0; j < p->nclusters; j++)
				if (j != k)
					in.other_power_w += estimated[j];
			for (int i = 0; i < c->ncpus; i++) {
				int cpu = c->first_cpu + i;
				in.busy_mean += (s.cpu[cpu].stat_busy_s - previous_busy[cpu]) / .1;
				previous_busy[cpu] = s.cpu[cpu].stat_busy_s;
			}
			in.busy_mean /= c->ncpus;
			inputs[k] = in;
		}
		lw_ctl_step_group(&params, inputs, state, outputs, p->nclusters,
				  (uint64_t)period * 100000000ull, .1);
		for (int k = 0; k < p->nclusters; k++) {
			const struct lw_ctl_in *in = &inputs[k];
			struct saved *saved = &pending[k][slot];
			double observed[2] = { in->package_mc / 1000.0, in->temp_mc / 1000.0 };
			double candidates[2][3] = { { observed[0], NAN, NAN }, { observed[1], NAN, NAN } };

			peak_c = fmax(peak_c, observed[1]);
			if (saved->score) {
				add(&scores[0], saved->baseline, observed[1]);
				add(&scores[1], saved->current, observed[1]);
				add(&scores[2], saved->candidate, observed[1]);
			}
			if (temporal)
				point[k] = state[k].tm;
			else
				lw_tm_observe(&point[k], observed[1], observed[0], outputs[k].est_power_w,
					      in->other_power_w, .1);
			if (lw_tm_ready(&point[k])) {
				candidates[0][1] = lw_tm_package(&point[k], observed[0], point[k].last_total_w, 5);
				candidates[1][1] = candidates[0][1] + lw_tm_offset(&point[k], point[k].last_own_w);
				candidates[0][2] = lw_tm_forecast_package(&point[k], observed[0], 5);
				candidates[1][2] = lw_tm_forecast_hot(&point[k], observed[0], 5);
			}
			lw_forecast_step(&forecast[k], (uint64_t)period * 100000000ull, observed, candidates);
			saved->score = period >= 3000;
			saved->baseline = observed[1];
			saved->current = outputs[k].forecast_base_c;
			saved->candidate = integrated ? outputs[k].forecast_c :
				temporal ? lw_temporal_step(&lag[k], (uint64_t)period * 100000000ull,
					observed[1], outputs[k].forecast_base_c, candidates[1]) :
					forecast[k].prediction[LW_FORECAST_HOT];
			if (!isfinite(saved->current) || !isfinite(saved->candidate))
				return 1;
			if (period >= 3000 && period <= 11950) {
				old_ready += outputs[k].model_ready;
				new_ready += lw_tm_ready(&point[k]);
			}
			estimated[k] = outputs[k].est_power_w;
			s.cl[k].user_max_khz = outputs[k].ceiling_khz;
		}
	}
	printf("{\"case\":\"%s\",\"replicate\":%u,\"seed\":%llu,\"samples\":%lu,"
	       "\"persistence_mae_c\":%.6f,\"current_mae_c\":%.6f,\"candidate_mae_c\":%.6f,"
	       "\"persistence_rmse_c\":%.6f,\"current_rmse_c\":%.6f,\"candidate_rmse_c\":%.6f,"
	       "\"current_model_ready\":%lu,\"candidate_model_ready\":%lu,\"peak_c\":%.1f,"
	       "\"throttle_steps\":%llu,\"served\":%.6f}\n",
	       f->name, replicate, (unsigned long long)seed, scores[0].n,
	       scores[0].absolute / scores[0].n, scores[1].absolute / scores[1].n, scores[2].absolute / scores[2].n,
	       sqrt(scores[0].squared / scores[0].n), sqrt(scores[1].squared / scores[1].n),
	       sqrt(scores[2].squared / scores[2].n), old_ready, new_ready, peak_c,
	       (unsigned long long)s.throttle_events, s.work_done / s.work_demand);
	return scores[0].n != 26853 || !isfinite(scores[2].squared);
}

int main(int argc, char **argv)
{
	static const struct profile development[] = {
		{ "steps-passive", 0, 0, 25, 0, 7001 },
		{ "burst2-bare", 1, 2, 25, 4, 7003 },
		{ "burst03-fan", 1, 1, 35, .6, 7013 },
		{ "asymmetric-bare", 2, 2, 20, 0, 7019 },
		{ "sine-passive", 3, 0, 20, 90, 7027 },
		{ "regime-changes-bare", 6, 2, 25, 0, 7039 },
	};
	static const struct profile validation[] = {
		{ "burst17-passive-30C", 1, 0, 30, 3.4, 9001 },
		{ "burst13-bare-15C", 1, 2, 15, 26, 9007 },
		{ "asymmetric-fan-35C", 2, 1, 35, 0, 9011 },
		{ "sine-bare-30C", 3, 2, 30, 67, 9013 },
		{ "sparse-staggered-passive", 4, 0, 25, 17, 9029 },
		{ "random-fast-bare", 5, 2, 25, 11, 9041 },
		{ "random-slow-passive", 5, 0, 20, 53, 9043 },
		{ "cooling-change-35C", 7, 0, 35, 0, 9049 },
	};
	/* Fresh confirmation matrix, specified after development and before scores.
	 * Its periods, seeds and combinations did not select the learner or guard.
	 */
	static const struct profile reserved[] = {
		{ "period09-fan-25C", 1, 1, 25, .9, 11003 },
		{ "period14-bare-35C", 1, 2, 35, 1.4, 11027 },
		{ "period26-passive-15C", 1, 0, 15, 2.6, 11047 },
		{ "period58-bare-25C", 1, 2, 25, 5.8, 11057 },
		{ "period82-fan-35C", 1, 1, 35, 8.2, 11069 },
		{ "period142-passive-30C", 1, 0, 30, 14.2, 11071 },
		{ "period38-bare-15C", 1, 2, 15, 38, 11083 },
		{ "period022-passive-25C", 1, 0, 25, .22, 11113 },
		{ "period62-fan-30C", 1, 1, 30, 62, 11117 },
		{ "sparse13-bare-20C", 4, 2, 20, 13, 11119 },
		{ "sparse31-fan-35C", 4, 1, 35, 31, 11131 },
		{ "sparse7-passive-15C", 4, 0, 15, 7, 11149 },
		{ "random07-passive-35C", 5, 0, 35, .7, 11159 },
		{ "random7-bare-20C", 5, 2, 20, 7, 11161 },
		{ "random23-fan-25C", 5, 1, 25, 23, 11171 },
		{ "random61-passive-30C", 5, 0, 30, 61, 11173 },
		{ "sine11-fan-15C", 3, 1, 15, 11, 11177 },
		{ "sine41-bare-35C", 3, 2, 35, 41, 11197 },
		{ "sine113-passive-25C", 3, 0, 25, 113, 11213 },
		{ "sine191-bare-20C", 3, 2, 20, 191, 11239 },
		{ "asymmetric-passive-15C", 2, 0, 15, 0, 11243 },
		{ "asymmetric-bare-35C", 2, 2, 35, 0, 11251 },
		{ "asymmetric-fan-25C", 2, 1, 25, 0, 11257 },
		{ "phase-reset34-bare-25C", 8, 2, 25, 3.4, 11261 },
		{ "phase-reset66-passive-30C", 8, 0, 30, 6.6, 11273 },
		{ "varying-duty46-bare-15C", 9, 2, 15, 4.6, 11279 },
		{ "varying-duty94-fan-35C", 9, 1, 35, 9.4, 11287 },
		{ "cooling-change-20C", 7, 0, 20, 0, 11299 },
		{ "cooling-change-30C", 7, 2, 30, 0, 11311 },
		{ "steps-passive-35C", 0, 0, 35, 0, 11317 },
		{ "steps-bare-15C", 0, 2, 15, 0, 11321 },
		{ "steps-fan-25C", 0, 1, 25, 0, 11329 },
	};
	const struct profile *profiles = development;
	size_t n = sizeof(development) / sizeof(*development);
	int failed = 0;
	while (argc > 1 && (!strcmp(argv[1], "--temporal") || !strcmp(argv[1], "--integrated"))) {
		if (!strcmp(argv[1], "--temporal"))
			temporal = true;
		else
			integrated = true;
		argv++;
		argc--;
	}

	if (argc == 4 && !strcmp(argv[2], "--replicate")) {
		char *end;
		unsigned long value = strtoul(argv[3], &end, 10);
		if (!argv[3][0] || *end || value > 10000)
			return 1;
		replicate = (unsigned int)value;
		argc = 2;
	}
	if (argc == 2 && !strcmp(argv[1], "--reserved")) {
		profiles = reserved;
		n = sizeof(reserved) / sizeof(*reserved);
	} else if (argc == 2 && !strcmp(argv[1], "--validation")) {
		profiles = validation;
		n = sizeof(validation) / sizeof(*validation);
	} else if (argc > 1 && (argc != 2 || strcmp(argv[1], "--development"))) {
		fprintf(stderr, "usage: test_control_forecast [--temporal] [--development|--validation|--reserved] [--replicate N]\n");
		return 1;
	}
	for (size_t i = 0; i < n; i++)
		failed |= evaluate(&profiles[i]);
	return failed;
}
