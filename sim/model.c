// SPDX-License-Identifier: MIT
/*
 * lerwatt-sim physics and kernel-policy emulation.
 *
 * Kernel formulas reproduced exactly (Linux v7.3):
 *   capacity     drivers/base/arch_topology.c  topology_normalize_cpu_scale()
 *   EM power     drivers/opp/of.c              _get_power(): dpc * mV^2 * MHz / 1e6
 *   EM perf/cost kernel/power/energy_model.c   em_init_performance(), em_compute_costs()
 *   schedutil    kernel/sched/cpufreq_schedutil.c  util * 1.25, lowest OPP >= target
 *   freq QoS     max of min requests, min of max requests, resolved to OPPs
 *   step_wise    drivers/thermal/gov_step_wise.c   get_target_state()
 *   TSADC        drivers/thermal/rockchip_thermal.c rk_tsadcv2_code_to_temp()
 *
 * Approximations (documented in docs/SIMULATION.md): PELT as a 32 ms
 * exponential average, idle-state residency as a function of load, a lumped
 * RC thermal network, and work-conserving speed scaling of the VM's load.
 */
#include <math.h>
#include <string.h>

#include "sim.h"

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

int sim_cluster_of_cpu(const struct sim_platform *p, int cpu)
{
	for (int k = 0; k < p->nclusters; k++) {
		const struct sim_cluster_desc *c = &p->clusters[k];

		if (cpu >= c->first_cpu && cpu < c->first_cpu + c->ncpus)
			return k;
	}
	return -1;
}

uint32_t sim_capacity(const struct sim_platform *p, int cpu)
{
	uint64_t best = 0, mine = 0;

	for (int k = 0; k < p->nclusters; k++) {
		const struct sim_cluster_desc *c = &p->clusters[k];
		uint64_t v = (uint64_t)c->dmips_mhz * c->opps[c->nopps - 1].khz;

		if (v > best)
			best = v;
		if (k == sim_cluster_of_cpu(p, cpu))
			mine = v;
	}
	return best ? (uint32_t)((mine << 10) / best) : 1024;
}

uint32_t sim_em_power_uw(const struct sim_cluster_desc *c, int opp)
{
	uint64_t mv = c->opps[opp].uv / 1000, mhz = c->opps[opp].khz / 1000;

	return (uint32_t)((uint64_t)c->dpc * mv * mv * mhz / 1000000);
}

uint32_t sim_em_perf(const struct sim_platform *p, int k, int opp)
{
	const struct sim_cluster_desc *c = &p->clusters[k];
	uint64_t cap = sim_capacity(p, c->first_cpu);

	return (uint32_t)(cap * c->opps[opp].khz / c->opps[c->nopps - 1].khz);
}

uint32_t sim_em_cost(const struct sim_platform *p, int k, int opp)
{
	uint32_t perf = sim_em_perf(p, k, opp);

	return perf ? sim_em_power_uw(&p->clusters[k], opp) * 10u / perf : 0;
}

int32_t sim_tsadc_read_mc(const struct sim_platform *p, double temp_c)
{
	const struct sim_tsadc_point *t = p->tsadc;
	double mc = temp_c * 1000.0, code_f;
	int n = p->ntsadc, i = 0, code;

	/* Hardware: temperature -> integer ADC code. */
	if (mc <= t[0].mc)
		code = t[0].code + 1;
	else if (mc >= t[n - 1].mc)
		code = t[n - 1].code;
	else {
		while (i < n - 2 && mc > t[i + 1].mc)
			i++;
		code_f = t[i].code + (mc - t[i].mc) * (t[i + 1].code - t[i].code) /
				     (double)(t[i + 1].mc - t[i].mc);
		code = (int)lround(code_f);
		if (code <= t[0].code)
			code = t[0].code + 1;
	}
	/* Driver: code -> millidegrees, truncating interpolation. */
	for (i = 1; i < n; i++)
		if (code <= t[i].code && code > t[i - 1].code)
			break;
	if (i == n)
		i = n - 1;
	return t[i - 1].mc + (int32_t)((uint32_t)(t[i].mc - t[i - 1].mc) *
				       (uint32_t)(code - t[i - 1].code) /
				       (uint32_t)(t[i].code - t[i - 1].code));
}

/* xorshift64* and Box-Muller: deterministic for a given seed. */
static double rng_uniform(uint64_t *s)
{
	*s ^= *s >> 12;
	*s ^= *s << 25;
	*s ^= *s >> 27;
	return ((*s * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
}

static double rng_normal(uint64_t *s)
{
	double u1 = rng_uniform(s), u2 = rng_uniform(s);

	if (u1 < 1e-12)
		u1 = 1e-12;
	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static int highest_le(const struct sim_cluster_desc *c, uint32_t khz)
{
	int i = c->nopps - 1;

	while (i > 0 && c->opps[i].khz > khz)
		i--;
	return i;
}

static int lowest_ge(const struct sim_cluster_desc *c, double khz)
{
	for (int i = 0; i < c->nopps; i++)
		if (c->opps[i].khz >= khz)
			return i;
	return c->nopps - 1;
}

int sim_eff_max_idx(const struct sim_state *s, int k)
{
	const struct sim_cluster_desc *c = &s->plat->clusters[k];
	int user = highest_le(c, s->cl[k].user_max_khz);
	int thermal = c->nopps - 1 - s->cl[k].thermal_state;

	if (thermal < 0)
		thermal = 0;
	return user < thermal ? user : thermal;
}

int sim_eff_min_idx(const struct sim_state *s, int k)
{
	int mn = lowest_ge(&s->plat->clusters[k], s->cl[k].user_min_khz);
	int mx = sim_eff_max_idx(s, k);

	return mn > mx ? mx : mn;
}

void sim_init(struct sim_state *s, const struct sim_platform *p, int cooling,
	      double ambient_c, double initial_c, double s_ref, uint64_t seed)
{
	memset(s, 0, sizeof(*s));
	s->plat = p;
	s->cool = &p->cooling[cooling < 0 || cooling >= p->ncooling ? 0 : cooling];
	s->ambient_c = ambient_c;
	s->die_c = initial_c > ambient_c ? initial_c : ambient_c;
	s->s_ref = s_ref > 0 ? s_ref : 1.0;
	s->kernel_thermal = true;
	s->rng = seed ? seed : 0x9e3779b97f4a7c15ull;
	for (int cpu = 0; cpu < p->ncpus; cpu++) {
		s->capacity[cpu] = sim_capacity(p, cpu);
		s->cpu[cpu].resume_latency_us = -1;
	}
	for (int k = 0; k < p->nclusters; k++) {
		const struct sim_cluster_desc *c = &p->clusters[k];

		s->cl[k].cur_idx = 0;
		s->cl[k].user_min_khz = c->opps[0].khz;
		s->cl[k].user_max_khz = c->opps[c->nopps - 1].khz;
		s->cl[k].temp_c = s->die_c;
		s->cl[k].cpuperf_req = -1;
	}
	for (int z = 0; z < p->nzones; z++) {
		const struct sim_zone_desc *zd = &p->zones[z];
		double node = zd->cluster >= 0 ? s->cl[zd->cluster].temp_c : s->die_c;

		s->zone[z].sensor_mc = sim_tsadc_read_mc(p, node + zd->offset_c);
		s->zone[z].next_sample_s = 0;
	}
}

/* schedutil: util * 1.25 mapped linearly onto the OPP range, then resolved. */
static void cpufreq_select(struct sim_state *s, int k)
{
	const struct sim_platform *p = s->plat;
	const struct sim_cluster_desc *c = &p->clusters[k];
	struct sim_cluster *cl = &s->cl[k];
	uint32_t cap = s->capacity[c->first_cpu];
	double util = 0, target_khz;
	int idx, lo = sim_eff_min_idx(s, k), hi = sim_eff_max_idx(s, k);

	if (cl->cpuperf_req >= 0) {
		/* sched_ext owns every task: schedutil reads the BPF target only. */
		util = cl->cpuperf_req;
	} else {
		for (int i = 0; i < c->ncpus; i++)
			if (s->cpu[c->first_cpu + i].util_pelt > util)
				util = s->cpu[c->first_cpu + i].util_pelt;
	}
	util = clampd(util, 0, cap);
	target_khz = util * 1.25 * c->opps[c->nopps - 1].khz / cap;
	idx = lowest_ge(c, target_khz);
	if (idx < lo)
		idx = lo;
	if (idx > hi)
		idx = hi;
	if (idx != cl->cur_idx) {
		cl->cur_idx = idx;
		cl->total_trans++;
	}
}

/* step_wise get_target_state() for the cluster's passive trip. */
static void step_wise_poll(struct sim_state *s)
{
	const struct sim_platform *p = s->plat;

	for (int z = 0; z < p->nzones; z++) {
		const struct sim_zone_desc *zd = &p->zones[z];
		struct sim_cluster *cl;
		int32_t t = s->zone[z].sensor_mc, threshold;
		int upper, state;
		bool throttle;
		int trend;

		if (!zd->passive || zd->cluster < 0)
			continue;
		cl = &s->cl[zd->cluster];
		upper = p->clusters[zd->cluster].nopps - 1;
		/* Threshold drops by the hysteresis once the trip is crossed. */
		if (t >= p->passive_mc)
			cl->tripped = true;
		else if (t < p->passive_mc - p->passive_hyst_mc)
			cl->tripped = false;
		threshold = cl->tripped ? p->passive_mc - p->passive_hyst_mc : p->passive_mc;
		throttle = t >= threshold;
		trend = t > cl->last_poll_mc ? 1 : t < cl->last_poll_mc ? -1 : 0;
		cl->last_poll_mc = t;
		state = cl->thermal_state;

		if (!cl->tz_initialized) {
			cl->tz_initialized = true;
			state = throttle ? state + 1 : 0;
		} else if (throttle) {
			if (trend > 0)
				state = state + 1;
			else if (trend < 0)
				state = state - 1 < 1 ? 1 : state - 1;
		} else if (trend < 0) {
			state = 0;	/* THERMAL_NO_TARGET */
		}
		if (state > upper)
			state = upper;
		if (state < 0)
			state = 0;
		if (state > cl->thermal_state)
			s->throttle_events++;
		cl->thermal_state = state;
	}
}

static double deep_idle_share(double busy)
{
	/* Long idle periods shrink as load rises (ASSUMED shape). */
	return busy < 0.5 ? 0.85 : 0.85 - (busy - 0.5) * 1.1;
}

void sim_step(struct sim_state *s, const double *demand, double dt)
{
	const struct sim_platform *p = s->plat;
	const double pelt_a = 1.0 - pow(0.5, dt * 1000.0 / 32.0);
	double busy_sum = 0, cpu_w = 0;

	if (dt <= 0)
		return;

	for (int k = 0; k < p->nclusters; k++) {
		const struct sim_cluster_desc *c = &p->clusters[k];
		struct sim_cluster *cl = &s->cl[k];
		const struct sim_opp *o = &c->opps[cl->cur_idx];
		double fr = (double)o->khz / c->opps[c->nopps - 1].khz;
		double v = o->uv / 1e6, vtop = c->opps[c->nopps - 1].uv / 1e6;
		double dyn = sim_em_power_uw(c, cl->cur_idx) * 1e-6;
		double leak = c->leak_w_ref * (v / vtop) *
			      exp(p->leak_beta * (cl->temp_c - p->leak_t_ref_c));

		cl->power_w = 0;
		for (int i = 0; i < c->ncpus; i++) {
			int cpu = c->first_cpu + i;
			struct sim_cpu *pc = &s->cpu[cpu];
			double perf = s->capacity[cpu] * fr;
			double want = clampd(demand[cpu], 0, 1) * s->s_ref * 1024.0;
			double busy = perf > 0 ? clampd(want / perf, 0, 1) : 1;
			double idle = 1 - busy, deep = 0, wfi;
			bool deep_ok = p->nidle >= 2 && !pc->idle_disabled[1] &&
				       (pc->resume_latency_us < 0 ||
					(uint32_t)pc->resume_latency_us >= p->idle[1].latency_us);
			double w;

			s->work_demand += want * dt;
			s->work_done += (want < perf ? want : perf) * dt;
			pc->busy = busy;
			pc->util_pelt += (busy * perf - pc->util_pelt) * pelt_a;
			if (deep_ok)
				deep = idle * clampd(deep_idle_share(busy), 0, 1);
			wfi = idle - deep;
			pc->idle_share[0] = wfi;
			pc->idle_share[1] = deep;
			w = busy * (dyn + leak) +
			    wfi * (p->idle[0].dyn_frac * dyn + p->idle[0].leak_frac * leak) +
			    deep * (p->nidle >= 2 ? p->idle[1].leak_frac * leak : leak);
			pc->power_w = w;
			cl->power_w += w;
			busy_sum += busy;

			pc->stat_busy_s += busy * dt;
			pc->stat_idle_s += idle * dt;
			pc->idle_time_us[0] += wfi * dt * 1e6;
			pc->idle_time_us[1] += deep * dt * 1e6;
			pc->idle_usage[0] += wfi * dt * 1e6 / 200.0;	/* ASSUMED mean WFI stay */
			if (p->nidle >= 2)
				pc->idle_usage[1] += deep * dt * 1e6 / (4.0 * p->idle[1].residency_us);
		}
		cl->time_in_state_s[cl->cur_idx] += dt;
		cpu_w += cl->power_w;
	}

	/* RC network: cluster hot spots -> die -> ambient. Substeps keep Euler stable. */
	for (double done = 0; done < dt - 1e-12;) {
		double h = dt - done < 0.005 ? dt - done : 0.005;
		double to_die = 0;

		for (int k = 0; k < p->nclusters; k++) {
			const struct sim_cluster_desc *c = &p->clusters[k];
			struct sim_cluster *cl = &s->cl[k];
			double q = (cl->temp_c - s->die_c) / c->r_to_die;

			cl->temp_c += h * (cl->power_w - q) / c->c_node;
			to_die += q;
		}
		s->die_c += h * (to_die + p->soc_other_w -
				 (s->die_c - s->ambient_c) / s->cool->r_die_amb) / s->cool->c_die;
		done += h;
	}

	s->board_w = p->base_w + (cpu_w + p->soc_other_w +
				  p->mem_w_full * busy_sum / p->ncpus) / p->pmic_eff;
	s->energy_j += s->board_w * dt;
	{
		double noisy = s->board_w * (1 + p->meter_noise_frac * rng_normal(&s->rng));

		s->meter_w = floor(noisy / p->meter_lsb_w + 0.5) * p->meter_lsb_w;
	}

	s->t_s += dt;

	/* TSADC: sample-and-hold, faster above the passive trip. */
	for (int z = 0; z < p->nzones; z++) {
		const struct sim_zone_desc *zd = &p->zones[z];
		double node = zd->cluster >= 0 ? s->cl[zd->cluster].temp_c : s->die_c;

		if (s->t_s + 1e-9 < s->zone[z].next_sample_s)
			continue;
		s->zone[z].sensor_mc = sim_tsadc_read_mc(p, node + zd->offset_c +
							 p->tsadc_noise_c * rng_normal(&s->rng));
		s->zone[z].next_sample_s = s->t_s +
			(s->zone[z].sensor_mc >= p->passive_mc ? p->tsadc_period_hot_ms
							       : p->tsadc_period_ms) / 1000.0;
		if (s->zone[z].sensor_mc >= p->critical_mc)
			s->critical = true;
	}

	if (s->kernel_thermal && s->t_s + 1e-9 >= s->next_poll_s) {
		step_wise_poll(s);
		s->next_poll_s = s->t_s + p->passive_poll_ms / 1000.0;
	}

	for (int k = 0; k < p->nclusters; k++)
		cpufreq_select(s, k);
}
