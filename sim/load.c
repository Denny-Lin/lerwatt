// SPDX-License-Identifier: MIT
/*
 * Demand sources for lerwatt-sim.
 *
 * procstat is the realistic mode: real programs run in the VM under the real
 * scheduler (EEVDF or Lertick), and their measured busy time becomes the
 * demand placed on the simulated cores. Patterns and traces are for
 * deterministic experiments and tests.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "load.h"

static bool is_big_cpu(const struct sim_platform *p, int cpu)
{
	return sim_capacity(p, cpu) >= 1024;
}

int sim_load_open(struct sim_load *l, const struct sim_platform *p, const char *spec,
		  const char *map_spec, char *err, size_t errlen)
{
	memset(l, 0, sizeof(*l));
	l->plat = p;
	for (int r = 0; r < SIM_LOAD_MAX_REAL; r++)
		l->map[r] = r < p->ncpus ? r : -1;

	if (!strcmp(spec, "procstat")) {
		FILE *f = fopen("/proc/stat", "r");
		char line[256];

		if (!f) {
			snprintf(err, errlen, "cannot read /proc/stat: %s", strerror(errno));
			return -1;
		}
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "cpu", 3) && line[3] >= '0' && line[3] <= '9')
				l->nreal++;
		fclose(f);
		l->kind = LOAD_PROCSTAT;
		if (map_spec) {
			const char *s = map_spec;

			for (int r = 0; r < SIM_LOAD_MAX_REAL; r++)
				l->map[r] = -1;
			for (int r = 0; *s && r < SIM_LOAD_MAX_REAL; r++) {
				char *end;
				long c = strtol(s, &end, 10);

				if (end == s || c < -1 || c >= p->ncpus) {
					snprintf(err, errlen, "bad --cpu-map entry near \"%s\"", s);
					return -1;
				}
				l->map[r] = (int)c;
				s = *end == ',' ? end + 1 : end;
			}
		}
		if (l->nreal < p->ncpus && !map_spec)
			fprintf(stderr,
				"lerwatt-sim: warning: %d real CPUs for %d simulated; CPUs %d-%d stay idle. "
				"Run the VM with -smp %d or pass --cpu-map.\n",
				l->nreal, p->ncpus, l->nreal, p->ncpus - 1, p->ncpus);
		return 0;
	}
	if (!strncmp(spec, "pattern:", 8)) {
		static const char *known[] = { "idle", "full", "big", "burst", "steps", "mixed" };

		snprintf(l->pattern, sizeof(l->pattern), "%s", spec + 8);
		for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
			if (!strcmp(l->pattern, known[i])) {
				l->kind = LOAD_PATTERN;
				return 0;
			}
		snprintf(err, errlen, "unknown pattern \"%s\"", l->pattern);
		return -1;
	}
	if (!strncmp(spec, "trace:", 6)) {
		l->trace = fopen(spec + 6, "r");
		if (!l->trace) {
			snprintf(err, errlen, "cannot open trace %s: %s", spec + 6, strerror(errno));
			return -1;
		}
		l->kind = LOAD_TRACE;
		l->next_t = -1;
		return 0;
	}
	snprintf(err, errlen, "unknown load \"%s\" (procstat, pattern:NAME, trace:FILE)", spec);
	return -1;
}

static void sample_procstat(struct sim_load *l, double *demand)
{
	FILE *f = fopen("/proc/stat", "r");
	char line[512];

	for (int c = 0; c < l->plat->ncpus; c++)
		demand[c] = l->last[c];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v[10] = { 0 };
		uint64_t busy, total;
		int cpu;

		if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9')
			continue;
		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1],
			   &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 5)
			continue;
		if (cpu < 0 || cpu >= SIM_LOAD_MAX_REAL)
			continue;
		/* Steal is time the host took from this vCPU: not demand. */
		busy = v[0] + v[1] + v[2] + v[5] + v[6];
		total = busy + v[3] + v[4] + v[7];
		if (l->valid && total > l->total[cpu] && l->map[cpu] >= 0) {
			double b = (double)(busy - l->busy[cpu]) / (double)(total - l->total[cpu]);

			demand[l->map[cpu]] = b < 0 ? 0 : b > 1 ? 1 : b;
			l->last[l->map[cpu]] = demand[l->map[cpu]];
		}
		l->busy[cpu] = busy;
		l->total[cpu] = total;
	}
	fclose(f);
	l->valid = true;
}

static void sample_pattern(const struct sim_load *l, double t, double *demand)
{
	const struct sim_platform *p = l->plat;

	for (int c = 0; c < p->ncpus; c++) {
		bool big = is_big_cpu(p, c);
		double v = 0.02;

		if (!strcmp(l->pattern, "full"))
			v = 1.0;
		else if (!strcmp(l->pattern, "big"))	/* what EAS does with a heavy task set */
			v = big ? 1.0 : 0.1;
		else if (!strcmp(l->pattern, "burst"))
			v = fmod(t, 4.0) < 2.0 ? 1.0 : 0.05;
		else if (!strcmp(l->pattern, "steps"))
			v = t >= 30.0 && t < 330.0 ? 1.0 : 0.02;
		else if (!strcmp(l->pattern, "mixed"))
			v = (big ? 0.6 : 0.3) + 0.25 * sin(2 * M_PI * t / 7.0 + c);
		demand[c] = v < 0 ? 0 : v > 1 ? 1 : v;
	}
}

static void sample_trace(struct sim_load *l, double t, double *demand)
{
	char line[1024];

	for (int c = 0; c < l->plat->ncpus; c++)
		demand[c] = l->last[c];
	while (!l->trace_done && (l->next_t < 0 || l->next_t <= t)) {
		if (l->next_t >= 0)
			memcpy(l->last, l->next_vals, sizeof(l->last));
		if (!fgets(line, sizeof(line), l->trace)) {
			l->trace_done = true;
			break;
		}
		if (line[0] == '#' || line[0] == '\n')
			continue;
		{
			char *s = line, *end;
			int c = 0;

			l->next_t = strtod(s, &end);
			s = end;
			for (; c < l->plat->ncpus; c++) {
				double v = strtod(s, &end);

				if (end == s)
					break;
				l->next_vals[c] = v;
				s = end;
			}
			for (; c < l->plat->ncpus; c++)
				l->next_vals[c] = 0;
		}
	}
	for (int c = 0; c < l->plat->ncpus; c++)
		demand[c] = l->last[c];
}

void sim_load_sample(struct sim_load *l, double t_s, double *demand)
{
	switch (l->kind) {
	case LOAD_PROCSTAT: sample_procstat(l, demand); break;
	case LOAD_PATTERN: sample_pattern(l, t_s, demand); break;
	case LOAD_TRACE: sample_trace(l, t_s, demand); break;
	}
}

bool sim_load_is_realtime(const struct sim_load *l)
{
	return l->kind == LOAD_PROCSTAT;
}

void sim_load_close(struct sim_load *l)
{
	if (l->trace)
		fclose(l->trace);
	l->trace = NULL;
}
