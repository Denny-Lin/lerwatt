// SPDX-License-Identifier: MIT
/*
 * Per-period observations: zone temperatures, current frequencies, CPU busy
 * fractions from /proc/stat, and board power from an hwmon meter (or an
 * Energy Model estimate when no meter exists).
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lerwatt/hal.h"

static bool temperature_faulted(const char *path)
{
	const char *suffix = strrchr(path, '/');
	char fault_path[LW_PATH_MAX];
	uint64_t fault;
	int ret;

	if (!suffix || strncmp(suffix + 1, "temp", 4))
		return false;
	suffix += 5;
	if (!isdigit((unsigned char)*suffix))
		return false;
	while (isdigit((unsigned char)*suffix))
		suffix++;
	if (strcmp(suffix, "_input"))
		return false;
	ret = snprintf(fault_path, sizeof(fault_path), "%.*s_fault", (int)(suffix - path), path);
	if (ret < 0 || (size_t)ret >= sizeof(fault_path))
		return true;
	ret = lw_read_u64(fault_path, &fault);
	return ret != -ENOENT && (ret || fault);
}

static int read_cpustat(const struct lw_system *sys, uint64_t *busy, uint64_t *total, bool *seen)
{
	char path[LW_PATH_MAX], line[512];
	FILE *f;
	bool found = false;

	lw_path(sys, path, sizeof(path), "/proc/stat");
	f = fopen(path, "r");
	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v[10] = { 0 };
		int cpu;

		if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9')
			continue;
		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &cpu,
			   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) < 5)
			continue;
		if (cpu < 0 || cpu >= LW_MAX_CPUS)
			continue;
		/* user nice system idle iowait irq softirq steal; guest is inside user. */
		busy[cpu] = v[0] + v[1] + v[2] + v[5] + v[6];
		total[cpu] = busy[cpu] + v[3] + v[4] + v[7];
		seen[cpu] = true;
		found = true;
	}
	int ret = ferror(f) ? -EIO : found ? 0 : -EINVAL;

	fclose(f);
	return ret;
}

double lw_em_power_w(const struct lw_policy *p, uint32_t khz, double busy)
{
	int i = 0;

	while (i < p->nopps - 1 && p->opps[i].khz < khz)
		i++;
	return p->opps[i].power_uw * 1e-6 * busy;
}

int lw_sample(const struct lw_system *sys, struct lw_cpustat *hist, struct lw_sample *s)
{
	uint64_t busy[LW_MAX_CPUS] = { 0 }, total[LW_MAX_CPUS] = { 0 };
	bool seen[LW_MAX_CPUS] = { false };
	char path[LW_PATH_MAX];
	int ret;

	memset(s, 0, sizeof(*s));
	s->t_ns = lw_now_ns();

	for (int z = 0; z < sys->nzones; z++) {
		int64_t mc;

		if (sys->zones[z].temp_path[0])
			snprintf(path, sizeof(path), "%s", sys->zones[z].temp_path);
		else
			lw_path(sys, path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", sys->zones[z].id);
		s->zone_mc[z] = temperature_faulted(path) || lw_read_s64(path, &mc) ||
			       mc < -40000 || mc > 200000 ? INT32_MIN : (int32_t)mc;
	}

	ret = read_cpustat(sys, busy, total, seen);
	if (ret) {
		hist->valid = false;
		return ret;
	}
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];

		for (int c = 0; c < p->ncpus; c++) {
			int cpu = p->cpus[c];

			if (cpu < 0 || cpu >= LW_MAX_CPUS || !seen[cpu]) {
				hist->valid = false;
				return -ENODATA;
			}
			/* Hotplug/accounting resets must not underflow unsigned deltas.
			 * Use this observation as a fresh anchor for the next period. */
			if (hist->valid && (busy[cpu] < hist->busy[cpu] || total[cpu] < hist->total[cpu]))
				hist->valid = false;
		}
	}
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];
		double sum = 0, mx = 0;
		uint64_t v;

		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", p->id);
		if (lw_read_u64(path, &v) || !v || v > UINT32_MAX)
			return -EIO;
		s->cur_khz[i] = (uint32_t)v;

		for (int c = 0; c < p->ncpus; c++) {
			int cpu = p->cpus[c];
			double b = 0;

			if (cpu < 0 || cpu >= LW_MAX_CPUS)
				return -ERANGE;
			if (!ret && hist->valid && total[cpu] > hist->total[cpu])
				b = (double)(busy[cpu] - hist->busy[cpu]) /
				    (double)(total[cpu] - hist->total[cpu]);
			if (b < 0)
				b = 0;
			if (b > 1)
				b = 1;
			sum += b;
			if (b > mx)
				mx = b;
		}
		s->busy_mean[i] = sum / p->ncpus;
		s->busy_max[i] = mx;
	}
	if (!ret) {
		memcpy(hist->busy, busy, sizeof(busy));
		memcpy(hist->total, total, sizeof(total));
		hist->valid = true;
	}

	if (sys->meter_path[0]) {
		uint64_t uw;

		if (!lw_read_u64(sys->meter_path, &uw)) {
			s->power_w = uw * 1e-6;
			s->power_measured = true;
			return 0;
		}
	}
	for (int i = 0; i < sys->npolicies; i++)
		s->power_w += lw_em_power_w(&sys->policies[i], s->cur_khz[i], s->busy_mean[i]) *
			      sys->policies[i].ncpus;
	return 0;
}
