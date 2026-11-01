/* SPDX-License-Identifier: MIT */
/*
 * lerwatt-sim: a simulated SoC plus the kernel policies that would run on it.
 *
 * Scope (what this program stands in for):
 *   hardware  - OPP clocking and voltage, dynamic and leakage power, idle-state
 *               power, an RC thermal network, TSADC sensors, an INA226 meter
 *   kernel    - freq QoS aggregation, schedutil frequency selection, the
 *               step_wise governor at passive trips, critical-trip shutdown
 * Everything else is real: the VM's scheduler (EEVDF or Lertick), its tasks,
 * the Lertick contract maps and lerwattd itself.
 */
#ifndef LERWATT_SIM_H
#define LERWATT_SIM_H

#include <stdbool.h>
#include <stdint.h>

#define SIM_MAX_CPUS		16
#define SIM_MAX_CLUSTERS	4
#define SIM_MAX_OPPS		16
#define SIM_MAX_IDLE		4
#define SIM_MAX_ZONES		8
#define SIM_MAX_TSADC		8

struct sim_opp {
	uint32_t khz;
	uint32_t uv;
};

struct sim_cluster_desc {
	const char *name;
	int first_cpu;
	int ncpus;
	uint32_t dmips_mhz;	/* DT capacity-dmips-mhz */
	uint32_t dpc;		/* DT dynamic-power-coefficient, uW/MHz/V^2 */
	int nopps;
	struct sim_opp opps[SIM_MAX_OPPS];	/* ascending */
	/* Assumptions, to be calibrated on a real board: */
	double leak_w_ref;	/* per-core leakage at the top-OPP voltage and leak_t_ref_c */
	double r_to_die;	/* K/W, cluster hot spot to die */
	double c_node;		/* J/K */
};

struct sim_idle_desc {
	const char *name;
	const char *desc;
	uint32_t latency_us;	/* as exported by cpuidle: entry + exit */
	uint32_t residency_us;
	double dyn_frac;	/* share of full dynamic power while in the state */
	double leak_frac;	/* share of leakage while in the state */
};

struct sim_zone_desc {
	const char *type;
	int cluster;		/* hot spot it reads; -1 = die */
	double offset_c;	/* sensor placement offset from that node */
	bool passive;		/* has the passive trip and cools its cluster */
};

struct sim_cooling {
	const char *name;
	double r_die_amb;	/* K/W, die to ambient through package and heatsink */
	double c_die;		/* J/K, die + heat spreader + heatsink */
};

struct sim_tsadc_point {
	int code;
	int mc;
};

struct sim_platform {
	const char *name;
	int ncpus;
	int nclusters;
	struct sim_cluster_desc clusters[SIM_MAX_CLUSTERS];
	int nidle;
	struct sim_idle_desc idle[SIM_MAX_IDLE];
	int nzones;
	struct sim_zone_desc zones[SIM_MAX_ZONES];
	int32_t passive_mc;
	int32_t passive_hyst_mc;
	int32_t critical_mc;
	uint32_t passive_poll_ms;
	int ntsadc;
	struct sim_tsadc_point tsadc[SIM_MAX_TSADC];
	uint32_t tsadc_period_ms;
	uint32_t tsadc_period_hot_ms;
	double tsadc_noise_c;
	uint32_t transition_latency_ns;
	/* Board-level assumptions: */
	double base_w;		/* board power with all cores off: PMIC, DRAM idle, I/O */
	double soc_other_w;	/* GPU/NPU/interconnect idle heat inside the die */
	double mem_w_full;	/* extra DRAM + controller power at full CPU load */
	double pmic_eff;	/* conversion efficiency of the SoC rails */
	double leak_beta;	/* 1/K, leakage temperature coefficient */
	double leak_t_ref_c;
	double meter_lsb_w;	/* INA226 power register resolution */
	double meter_noise_frac;
	int ncooling;
	struct sim_cooling cooling[3];
};

extern const struct sim_platform sim_rk3588;

struct sim_cpu {
	double util_pelt;	/* frequency-invariant, capacity units, 32 ms half-life */
	double busy;		/* simulated busy share of the last tick */
	double idle_share[SIM_MAX_IDLE];
	double stat_busy_s;
	double stat_idle_s;
	double idle_time_us[SIM_MAX_IDLE];
	double idle_usage[SIM_MAX_IDLE];
	int32_t resume_latency_us;	/* <0 no constraint ("0"), 0 = "n/a" */
	bool idle_disabled[SIM_MAX_IDLE];
	double power_w;
};

struct sim_cluster {
	int cur_idx;
	uint32_t user_min_khz;	/* written by lerwattd as scaling_min_freq */
	uint32_t user_max_khz;	/* written by lerwattd as scaling_max_freq */
	int thermal_state;	/* step_wise cooling state, 0 = unthrottled */
	bool tz_initialized;
	bool tripped;
	int32_t last_poll_mc;
	double time_in_state_s[SIM_MAX_OPPS];
	uint64_t total_trans;
	double temp_c;		/* hot-spot truth */
	double power_w;
	int32_t cpuperf_req;	/* from Lertick, <0 = none (schedutil uses PELT util) */
};

struct sim_zone {
	int32_t sensor_mc;
	double next_sample_s;
};

struct sim_state {
	const struct sim_platform *plat;
	const struct sim_cooling *cool;
	double t_s;
	double ambient_c;
	double die_c;
	double s_ref;		/* host vCPU speed / a 1024-capacity core at its top OPP */
	bool kernel_thermal;	/* emulate step_wise at passive trips */
	bool critical;
	uint32_t capacity[SIM_MAX_CPUS];
	struct sim_cpu cpu[SIM_MAX_CPUS];
	struct sim_cluster cl[SIM_MAX_CLUSTERS];
	struct sim_zone zone[SIM_MAX_ZONES];
	double next_poll_s;
	double board_w;		/* truth */
	double meter_w;		/* what the meter reports */
	double energy_j;	/* integrated truth */
	double work_demand;	/* capacity-unit-seconds asked for */
	double work_done;	/* capacity-unit-seconds served */
	uint64_t throttle_events;
	uint64_t rng;
};

/* Kernel formulas, reproduced exactly (see sim/model.c for references). */
uint32_t sim_capacity(const struct sim_platform *p, int cpu);
uint32_t sim_em_power_uw(const struct sim_cluster_desc *c, int opp);
uint32_t sim_em_perf(const struct sim_platform *p, int cluster, int opp);
uint32_t sim_em_cost(const struct sim_platform *p, int cluster, int opp);
int32_t sim_tsadc_read_mc(const struct sim_platform *p, double temp_c);
int sim_cluster_of_cpu(const struct sim_platform *p, int cpu);
int sim_eff_max_idx(const struct sim_state *s, int k);
int sim_eff_min_idx(const struct sim_state *s, int k);

void sim_init(struct sim_state *s, const struct sim_platform *p, int cooling,
	      double ambient_c, double initial_c, double s_ref, uint64_t seed);
/* Advance by dt seconds. demand[cpu] = host vCPU busy share, 0..1. */
void sim_step(struct sim_state *s, const double *demand, double dt);

#endif /* LERWATT_SIM_H */
