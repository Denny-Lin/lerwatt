/* SPDX-License-Identifier: MIT */
/* Where the simulated CPUs' demand comes from. */
#ifndef LERWATT_SIM_LOAD_H
#define LERWATT_SIM_LOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sim.h"

#define SIM_LOAD_MAX_REAL 256

enum sim_load_kind { LOAD_PROCSTAT, LOAD_PATTERN, LOAD_TRACE };

struct sim_load {
	enum sim_load_kind kind;
	char pattern[32];
	const struct sim_platform *plat;
	/* procstat: real CPU r drives simulated CPU map[r] (-1 = ignored) */
	int map[SIM_LOAD_MAX_REAL];
	int nreal;
	bool valid;
	uint64_t busy[SIM_LOAD_MAX_REAL];
	uint64_t total[SIM_LOAD_MAX_REAL];
	double last[SIM_MAX_CPUS];
	/* trace */
	FILE *trace;
	double next_t;
	double next_vals[SIM_MAX_CPUS];
	bool trace_done;
};

/*
 * spec: "procstat" (this machine's /proc/stat, i.e. the VM's real load),
 * "pattern:<idle|full|big|burst|steps|mixed>", or "trace:<file>" with lines
 * "t_seconds busy_cpu0 ... busy_cpuN". map_spec ("4,5,6,7,...") lists the
 * simulated CPU for each real CPU; NULL = identity.
 */
int sim_load_open(struct sim_load *l, const struct sim_platform *p, const char *spec,
		  const char *map_spec, char *err, size_t errlen);
void sim_load_sample(struct sim_load *l, double t_s, double *demand);
void sim_load_close(struct sim_load *l);
bool sim_load_is_realtime(const struct sim_load *l);

#endif /* LERWATT_SIM_LOAD_H */
