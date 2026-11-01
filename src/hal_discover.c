// SPDX-License-Identifier: MIT
/*
 * Sysroot detection and hardware discovery, from the kernel ABI only:
 *   cpufreq policies, OPP lists, CPU capacities, the Energy Model (debugfs),
 *   cpuidle states, thermal zones and their cpufreq cooling devices, and an
 *   hwmon power meter.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lerwatt/hal.h"

static void errf(char *err, size_t len, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void errf(char *err, size_t len, const char *fmt, ...)
{
	va_list ap;

	if (!err || !len)
		return;
	va_start(ap, fmt);
	vsnprintf(err, len, fmt, ap);
	va_end(ap);
}

static bool has_real_cpufreq(void)
{
	DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
	struct dirent *e;
	bool found = false;

	if (!d)
		return false;
	while ((e = readdir(d))) {
		char p[LW_PATH_MAX];

		if (strncmp(e->d_name, "policy", 6))
			continue;
		snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpufreq/%s/scaling_max_freq",
			 e->d_name);
		if (lw_exists(p)) {
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

static bool read_marker(const char *root, char *platform, size_t len)
{
	char path[LW_PATH_MAX], buf[256];

	snprintf(path, sizeof(path), "%s/%s", root, LW_SIM_MARKER);
	if (lw_read_str(path, buf, sizeof(buf)))
		return false;
	platform[0] = '\0';
	for (char *tok = strtok(buf, " \n"); tok; tok = strtok(NULL, " \n"))
		if (!strncmp(tok, "platform=", 9))
			snprintf(platform, len, "%s", tok + 9);
	return true;
}

int lw_detect(struct lw_system *sys, const char *explicit_root, char *err, size_t errlen)
{
	const char *env = getenv("LERWATT_SIM_ROOT");
	const char *sim_root = env && *env ? env : LW_SIM_DEFAULT_ROOT;

	memset(sys, 0, sizeof(*sys));
	if (explicit_root) {
		snprintf(sys->sysroot, sizeof(sys->sysroot), "%s", explicit_root);
		sys->simulated = strcmp(explicit_root, "/") &&
				 read_marker(explicit_root, sys->sim_platform,
					     sizeof(sys->sim_platform));
		return 0;
	}
	/* Real hardware always wins: plugging into a board needs no flag. */
	if (has_real_cpufreq()) {
		snprintf(sys->sysroot, sizeof(sys->sysroot), "/");
		return 0;
	}
	if (read_marker(sim_root, sys->sim_platform, sizeof(sys->sim_platform))) {
		snprintf(sys->sysroot, sizeof(sys->sysroot), "%s", sim_root);
		sys->simulated = true;
		return 0;
	}
	errf(err, errlen,
	     "no cpufreq policies on this machine and no simulator at %s; "
	     "start lerwatt-sim or pass --sysroot", sim_root);
	return -1;
}

static int parse_cpulist(const char *s, int *cpus, int max)
{
	int n = 0;

	while (*s) {
		char *end;
		long a = strtol(s, &end, 10), b;

		if (end == s)
			break;
		b = a;
		if (*end == '-')
			b = strtol(end + 1, &end, 10);
		for (long c = a; c <= b && n < max; c++)
			cpus[n++] = (int)c;
		while (*end == ',' || *end == ' ')
			end++;
		s = end;
	}
	return n;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

static void read_opps(const struct lw_system *sys, struct lw_policy *p)
{
	char path[LW_PATH_MAX], buf[1024];
	uint32_t khz[LW_MAX_OPPS];
	int n = 0;

	lw_path(sys, path, sizeof(path),
		"/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_frequencies", p->id);
	if (!lw_read_str(path, buf, sizeof(buf))) {
		for (char *tok = strtok(buf, " "); tok && n < LW_MAX_OPPS; tok = strtok(NULL, " "))
			khz[n++] = (uint32_t)strtoul(tok, NULL, 10);
	} else {
		/* Drivers without a frequency table still keep stats. */
		FILE *f;

		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/stats/time_in_state", p->id);
		f = fopen(path, "r");
		if (f) {
			unsigned long freq, ticks;

			while (n < LW_MAX_OPPS && fscanf(f, "%lu %lu", &freq, &ticks) == 2)
				khz[n++] = (uint32_t)freq;
			fclose(f);
		}
	}
	if (!n) {
		khz[n++] = p->cpuinfo_min_khz;
		if (p->cpuinfo_max_khz != p->cpuinfo_min_khz)
			khz[n++] = p->cpuinfo_max_khz;
	}
	qsort(khz, n, sizeof(khz[0]), cmp_u32);
	p->nopps = n;
	for (int i = 0; i < n; i++) {
		p->opps[i].khz = khz[i];
		p->opps[i].perf = (uint32_t)((uint64_t)p->capacity * khz[i] / khz[n - 1]);
	}
}

static void read_em(const struct lw_system *sys, struct lw_policy *p)
{
	int cpu = p->cpus[0], found = 0;

	for (int i = 0; i < p->nopps; i++) {
		char path[LW_PATH_MAX];
		uint64_t v;

		lw_path(sys, path, sizeof(path),
			"/sys/kernel/debug/energy_model/cpu%d/ps:%u/power", cpu, p->opps[i].khz);
		if (lw_read_u64(path, &v))
			continue;
		p->opps[i].power_uw = (uint32_t)v;
		lw_path(sys, path, sizeof(path),
			"/sys/kernel/debug/energy_model/cpu%d/ps:%u/performance", cpu, p->opps[i].khz);
		if (!lw_read_u64(path, &v))
			p->opps[i].perf = (uint32_t)v;
		lw_path(sys, path, sizeof(path),
			"/sys/kernel/debug/energy_model/cpu%d/ps:%u/inefficient", cpu, p->opps[i].khz);
		if (!lw_read_u64(path, &v))
			p->opps[i].inefficient = v != 0;
		found++;
	}
	p->have_em = found == p->nopps && found > 0;
	if (p->have_em)
		return;
	/*
	 * No Energy Model: fall back to a cubic proxy (1 W per CPU at the top
	 * OPP). Budgets are then relative units, which keeps the control law
	 * well-defined; telemetry reports em=no so nobody mistakes them for watts.
	 */
	for (int i = 0; i < p->nopps; i++) {
		double r = (double)p->opps[i].khz / p->opps[p->nopps - 1].khz;

		p->opps[i].power_uw = (uint32_t)(1e6 * r * r * r);
		p->opps[i].inefficient = false;
	}
}

static void read_idle(const struct lw_system *sys, struct lw_policy *p)
{
	for (int k = 0; k < LW_MAX_IDLE; k++) {
		char path[LW_PATH_MAX];
		uint64_t v;
		struct lw_idle_state *s = &p->idle[p->nidle];

		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name", p->cpus[0], k);
		if (lw_read_str(path, s->name, sizeof(s->name)))
			break;
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpu%d/cpuidle/state%d/latency", p->cpus[0], k);
		s->latency_us = lw_read_u64(path, &v) ? 0 : (uint32_t)v;
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpu%d/cpuidle/state%d/residency", p->cpus[0], k);
		s->residency_us = lw_read_u64(path, &v) ? 0 : (uint32_t)v;
		p->nidle++;
	}
}

static int cmp_policy(const void *a, const void *b)
{
	const struct lw_policy *x = a, *y = b;

	return x->cpus[0] - y->cpus[0];
}

static int discover_policies(struct lw_system *sys, char *err, size_t errlen)
{
	char path[LW_PATH_MAX], buf[256];
	struct dirent *e;
	DIR *d;

	lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq");
	d = opendir(path);
	if (!d) {
		errf(err, errlen, "cannot open %s", path);
		return -1;
	}
	while ((e = readdir(d)) && sys->npolicies < LW_MAX_POLICIES) {
		struct lw_policy *p = &sys->policies[sys->npolicies];
		uint64_t v;

		if (strncmp(e->d_name, "policy", 6))
			continue;
		memset(p, 0, sizeof(*p));
		p->id = atoi(e->d_name + 6);
		p->zone = -1;
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/related_cpus", p->id);
		if (lw_read_str(path, buf, sizeof(buf)))
			continue;
		p->ncpus = parse_cpulist(buf, p->cpus, LW_MAX_CPUS);
		if (!p->ncpus)
			continue;
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_min_freq", p->id);
		p->cpuinfo_min_khz = lw_read_u64(path, &v) ? 0 : (uint32_t)v;
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", p->id);
		p->cpuinfo_max_khz = lw_read_u64(path, &v) ? 0 : (uint32_t)v;
		lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity",
			p->cpus[0]);
		p->capacity = lw_read_u64(path, &v) ? 1024 : (uint32_t)v;
		read_opps(sys, p);
		read_em(sys, p);
		read_idle(sys, p);
		sys->npolicies++;
	}
	closedir(d);
	if (!sys->npolicies) {
		errf(err, errlen, "no usable cpufreq policies under %s", sys->sysroot);
		return -1;
	}
	qsort(sys->policies, sys->npolicies, sizeof(sys->policies[0]), cmp_policy);
	for (int i = 0; i < sys->npolicies; i++)
		for (int c = 0; c < sys->policies[i].ncpus; c++)
			if (sys->policies[i].cpus[c] + 1 > sys->ncpus)
				sys->ncpus = sys->policies[i].cpus[c] + 1;
	return 0;
}

static int policy_of_cpu(const struct lw_system *sys, int cpu)
{
	for (int i = 0; i < sys->npolicies; i++)
		for (int c = 0; c < sys->policies[i].ncpus; c++)
			if (sys->policies[i].cpus[c] == cpu)
				return i;
	return -1;
}

static void discover_zones(struct lw_system *sys)
{
	char path[LW_PATH_MAX], buf[64];
	struct dirent *e;
	DIR *d;

	sys->package_zone = -1;
	lw_path(sys, path, sizeof(path), "/sys/class/thermal");
	d = opendir(path);
	if (!d)
		return;
	while ((e = readdir(d)) && sys->nzones < LW_MAX_ZONES) {
		struct lw_zone *z = &sys->zones[sys->nzones];

		if (strncmp(e->d_name, "thermal_zone", 12))
			continue;
		memset(z, 0, sizeof(*z));
		z->id = atoi(e->d_name + 12);
		z->passive_mc = INT32_MAX;
		z->critical_mc = INT32_MAX;
		lw_path(sys, path, sizeof(path), "/sys/class/thermal/%s/type", e->d_name);
		if (lw_read_str(path, z->type, sizeof(z->type)))
			continue;
		for (int t = 0; t < 16; t++) {
			int64_t temp;

			lw_path(sys, path, sizeof(path),
				"/sys/class/thermal/%s/trip_point_%d_type", e->d_name, t);
			if (lw_read_str(path, buf, sizeof(buf)))
				break;
			lw_path(sys, path, sizeof(path),
				"/sys/class/thermal/%s/trip_point_%d_temp", e->d_name, t);
			if (lw_read_s64(path, &temp))
				continue;
			if (!strcmp(buf, "passive") && temp < z->passive_mc)
				z->passive_mc = (int32_t)temp;
			if (!strcmp(buf, "critical") && temp < z->critical_mc)
				z->critical_mc = (int32_t)temp;
		}
		/* Map the zone to the policy whose CPUs it cools. */
		for (int k = 0; k < 16; k++) {
			int cpu, pol;

			lw_path(sys, path, sizeof(path), "/sys/class/thermal/%s/cdev%d/type",
				e->d_name, k);
			if (lw_read_str(path, buf, sizeof(buf)))
				continue;
			if (sscanf(buf, "cpufreq-cpu%d", &cpu) != 1)
				continue;
			pol = policy_of_cpu(sys, cpu);
			if (pol >= 0 && sys->policies[pol].zone < 0)
				sys->policies[pol].zone = sys->nzones;
		}
		if (sys->package_zone < 0 && (strstr(z->type, "package") || strstr(z->type, "soc")))
			sys->package_zone = sys->nzones;
		sys->nzones++;
	}
	closedir(d);
}

static void discover_meter(struct lw_system *sys)
{
	char path[LW_PATH_MAX];
	struct dirent *e;
	DIR *d;

	lw_path(sys, path, sizeof(path), "/sys/class/hwmon");
	d = opendir(path);
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "hwmon", 5))
			continue;
		lw_path(sys, path, sizeof(path), "/sys/class/hwmon/%s/power1_input", e->d_name);
		if (!lw_exists(path))
			continue;
		snprintf(sys->meter_path, sizeof(sys->meter_path), "%s", path);
		lw_path(sys, path, sizeof(path), "/sys/class/hwmon/%s/name", e->d_name);
		if (lw_read_str(path, sys->meter_name, sizeof(sys->meter_name)))
			snprintf(sys->meter_name, sizeof(sys->meter_name), "%.*s",
				 (int)sizeof(sys->meter_name) - 1, e->d_name);
		break;
	}
	closedir(d);
}

int lw_discover(struct lw_system *sys, char *err, size_t errlen)
{
	if (discover_policies(sys, err, errlen))
		return -1;
	discover_zones(sys);
	discover_meter(sys);
	return 0;
}

void lw_describe(const struct lw_system *sys, FILE *out)
{
	fprintf(out, "lerwatt: mode=%s sysroot=%s%s%s\n",
		sys->simulated ? "simulation" : "hardware", sys->sysroot,
		sys->simulated ? " platform=" : "", sys->simulated ? sys->sim_platform : "");
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];

		fprintf(out, "lerwatt:   cluster %d = policy%d cpus %d-%d cap %u, %d OPPs %u-%u kHz, EM %s, zone %s, %d idle states\n",
			i, p->id, p->cpus[0], p->cpus[p->ncpus - 1], p->capacity, p->nopps,
			p->opps[0].khz, p->opps[p->nopps - 1].khz, p->have_em ? "yes" : "no",
			p->zone >= 0 ? sys->zones[p->zone].type : "(hottest)", p->nidle);
	}
	fprintf(out, "lerwatt:   %d thermal zones, package %s, meter %s\n", sys->nzones,
		sys->package_zone >= 0 ? sys->zones[sys->package_zone].type : "(none)",
		sys->meter_path[0] ? sys->meter_name : "none (Energy Model estimate)");
}
