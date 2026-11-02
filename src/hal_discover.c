// SPDX-License-Identifier: MIT
/*
 * Sysroot detection and hardware discovery, from the kernel ABI only:
 *   cpufreq policies, OPP lists, CPU capacities, the Energy Model (debugfs),
 *   cpuidle states, thermal zones and their cpufreq cooling devices, and an
 *   hwmon power meter.
 */
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	int n;

	n = snprintf(path, sizeof(path), "%s/%s", root, LW_SIM_MARKER);
	if (n < 0 || (size_t)n >= sizeof(path))
		return false;
	if (lw_read_str(path, buf, sizeof(buf)))
		return false;
	platform[0] = '\0';
	for (char *tok = strtok(buf, " \n"); tok; tok = strtok(NULL, " \n"))
		if (!strncmp(tok, "platform=", 9))
			snprintf(platform, len, "%s", tok + 9);
	return true;
}

static int set_sysroot(struct lw_system *sys, const char *root, char *err, size_t errlen)
{
	size_t n = strlen(root);
	const char *part = root;

	if (!n || root[0] != '/' || n >= sizeof(sys->sysroot)) {
		errf(err, errlen, "sysroot must be an absolute path shorter than %zu bytes",
		     sizeof(sys->sysroot));
		return -1;
	}
	while (*part) {
		while (*part == '/')
			part++;
		const char *end = strchr(part, '/');
		size_t length = end ? (size_t)(end - part) : strlen(part);

		if ((length == 1 && part[0] == '.') ||
		    (length == 2 && part[0] == '.' && part[1] == '.')) {
			errf(err, errlen, "sysroot must not contain . or .. path components");
			return -1;
		}
		part += length;
	}
	while (n > 1 && root[n - 1] == '/')
		n--;
	memcpy(sys->sysroot, root, n);
	sys->sysroot[n] = '\0';
	return 0;
}

int lw_detect(struct lw_system *sys, const char *explicit_root, char *err, size_t errlen)
{
	const char *env = getenv("LERWATT_SIM_ROOT");
	const char *sim_root = env && *env ? env : LW_SIM_DEFAULT_ROOT;

	memset(sys, 0, sizeof(*sys));
	if (explicit_root) {
		if (set_sysroot(sys, explicit_root, err, errlen))
			return -1;
		sys->simulated = strcmp(sys->sysroot, "/") &&
				 read_marker(sys->sysroot, sys->sim_platform,
					     sizeof(sys->sim_platform));
		return 0;
	}
	/* Real hardware always wins: plugging into a board needs no flag. */
	if (has_real_cpufreq()) {
		snprintf(sys->sysroot, sizeof(sys->sysroot), "/");
		return 0;
	}
	if (set_sysroot(sys, sim_root, err, errlen))
		return -1;
	if (read_marker(sys->sysroot, sys->sim_platform, sizeof(sys->sim_platform))) {
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
	bool seen[LW_MAX_CPUS] = { false };

	while (isspace((unsigned char)*s))
		s++;
	while (*s) {
		char *end;
		long a, b;

		if (!isdigit((unsigned char)*s))
			return -EINVAL;
		errno = 0;
		a = strtol(s, &end, 10);
		if (errno || a >= LW_MAX_CPUS)
			return -ERANGE;
		b = a;
		if (*end == '-') {
			if (!isdigit((unsigned char)end[1]))
				return -EINVAL;
			errno = 0;
			b = strtol(end + 1, &end, 10);
			if (errno || b >= LW_MAX_CPUS || b < a)
				return -ERANGE;
		}
		for (long c = a; c <= b; c++) {
			if (n == max || seen[c])
				return -EINVAL;
			seen[c] = true;
			cpus[n++] = (int)c;
		}
		if (*end && *end != ',' && !isspace((unsigned char)*end))
			return -EINVAL;
		while (isspace((unsigned char)*end))
			end++;
		if (*end == ',') {
			end++;
			while (isspace((unsigned char)*end))
				end++;
			if (!*end)
				return -EINVAL;
		}
		s = end;
	}
	return n;
}

static bool numbered_name(const char *name, const char *prefix, int *id)
{
	char *end;
	long value;
	size_t n = strlen(prefix);

	if (strncmp(name, prefix, n) || !isdigit((unsigned char)name[n]))
		return false;
	errno = 0;
	value = strtol(name + n, &end, 10);
	if (errno || *end || value > INT_MAX)
		return false;
	*id = (int)value;
	return true;
}

static int parse_frequency(const char *s, uint32_t *value)
{
	char *end;
	unsigned long long v;

	if (!isdigit((unsigned char)*s))
		return -EINVAL;
	errno = 0;
	v = strtoull(s, &end, 10);
	while (isspace((unsigned char)*end))
		end++;
	if (errno || *end || !v || v > UINT32_MAX)
		return -EINVAL;
	*value = (uint32_t)v;
	return 0;
}

static int read_frequency(const char *path, uint32_t *value)
{
	char buf[64];
	int ret = lw_read_str(path, buf, sizeof(buf));

	return ret ? ret : parse_frequency(buf, value);
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

static int read_opps(const struct lw_system *sys, struct lw_policy *p)
{
	char path[LW_PATH_MAX], buf[4096];
	uint32_t khz[LW_MAX_OPPS];
	int n = 0;

	lw_path(sys, path, sizeof(path),
		"/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_frequencies", p->id);
	if (!lw_read_str(path, buf, sizeof(buf))) {
		char *save = NULL;

		for (char *tok = strtok_r(buf, " \t\r\n", &save); tok;
		     tok = strtok_r(NULL, " \t\r\n", &save)) {
			if (n == LW_MAX_OPPS)
				return -E2BIG;
			if (parse_frequency(tok, &khz[n]))
				return -EINVAL;
			n++;
		}
		if (!n)
			return -EINVAL;
	} else {
		/* Drivers without a frequency table still keep stats. */
		FILE *f;

		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/stats/time_in_state", p->id);
		f = fopen(path, "r");
		if (f) {
			char line[256], freq[64], ticks[64], extra;

			while (fgets(line, sizeof(line), f)) {
				if (n == LW_MAX_OPPS || sscanf(line, "%63s %63s %c", freq, ticks, &extra) != 2 ||
				    parse_frequency(freq, &khz[n])) {
					fclose(f);
					return -EINVAL;
				}
				n++;
			}
			if (ferror(f)) {
				fclose(f);
				return -EIO;
			}
			fclose(f);
		}
	}
	if (!n) {
		/* These are legal frequency-limit requests, not invented hardware OPPs.
		 * Range-based drivers (including Intel/AMD P-state) need intermediate
		 * candidates: just min/max can leave the controller unable to cap. */
		uint64_t span = (uint64_t)p->cpuinfo_max_khz - p->cpuinfo_min_khz;

		p->synthetic_opps = true;
		for (unsigned int i = 0; i < LW_MAX_OPPS; i++) {
			uint32_t v = p->cpuinfo_min_khz + (uint32_t)(span * i / (LW_MAX_OPPS - 1));

			if (!n || v > khz[n - 1])
				khz[n++] = v;
		}
	}
	qsort(khz, n, sizeof(khz[0]), cmp_u32);
	p->nopps = n;
	for (int i = 0; i < n; i++) {
		if (!khz[i] || (i && khz[i] == khz[i - 1]) ||
		    khz[i] < p->cpuinfo_min_khz || khz[i] > p->cpuinfo_max_khz)
			return -EINVAL;
		p->opps[i].khz = khz[i];
		p->opps[i].perf = (uint32_t)((uint64_t)p->capacity * khz[i] / khz[n - 1]);
	}
	return 0;
}

static uint32_t em_power_multiplier(const struct lw_system *sys, int cpu)
{
	char path[LW_PATH_MAX], buf[128];
	char *end, *start;
	uint64_t flags;
	unsigned long major, minor;

	/* Linux 5.15 exports explicit units; abstract bogoWatts are unusable.
	 * New flags reuse an older milliWatts bit, so flags alone are ambiguous.
	 * Verified Linux >=6.1: MICROWATTS=BIT(0), ARTIFICIAL=BIT(2).
	 */
	lw_path(sys, path, sizeof(path), "/sys/kernel/debug/energy_model/cpu%d/units", cpu);
	int ret = lw_read_str(path, buf, sizeof(buf));

	if (!ret)
		return !strcmp(buf, "milliWatts") ? 1000 : 0;
	if (ret != -ENOENT)
		return 0;
	lw_path(sys, path, sizeof(path), "/sys/kernel/debug/energy_model/cpu%d/flags", cpu);
	if (lw_read_u64(path, &flags) || !(flags & 1) || (flags & ~3ull))
		return 0;
	if (sys->simulated)
		return 1;	/* Lerwatt simulator's fixed microW ABI. */
	lw_path(sys, path, sizeof(path), "/proc/sys/kernel/osrelease");
	if (lw_read_str(path, buf, sizeof(buf)) || !isdigit((unsigned char)buf[0]))
		return 0;
	errno = 0;
	major = strtoul(buf, &end, 10);
	if (errno || major > UINT_MAX || *end != '.' || !isdigit((unsigned char)end[1]))
		return 0;
	start = end + 1;
	errno = 0;
	minor = strtoul(start, &end, 10);
	if (errno || minor > UINT_MAX || (*end && *end != '.' && *end != '-' && *end != '+') ||
	    !(major > 6 || (major == 6 && minor >= 1)))
		return 0;
	return 1;
}

static void read_em(const struct lw_system *sys, struct lw_policy *p)
{
	int cpu = p->cpus[0], found = 0;
	uint32_t multiplier = em_power_multiplier(sys, cpu);

	for (int i = 0; multiplier && i < p->nopps; i++) {
		char path[LW_PATH_MAX];
		uint64_t v;

		lw_path(sys, path, sizeof(path),
			"/sys/kernel/debug/energy_model/cpu%d/ps:%u/power", cpu, p->opps[i].khz);
		if (lw_read_u64(path, &v) || !v || v > UINT32_MAX / multiplier)
			break;
		p->opps[i].power_uw = (uint32_t)v * multiplier;
		lw_path(sys, path, sizeof(path),
			"/sys/kernel/debug/energy_model/cpu%d/ps:%u/performance", cpu, p->opps[i].khz);
		int ret = lw_read_u64(path, &v);

		/* Older EM ABIs have power/frequency only; retain derived capacity. */
		if ((ret && ret != -ENOENT) || (!ret && (!v || v > 1024)))
			break;
		if (!ret)
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
		p->opps[i].perf = (uint32_t)((uint64_t)p->capacity * p->opps[i].khz /
					  p->opps[p->nopps - 1].khz);
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
	char path[LW_PATH_MAX], buf[LW_MAX_CPUS * 12];
	bool seen[LW_MAX_CPUS] = { false };
	int packages[LW_MAX_POLICIES];
	struct dirent *e;
	DIR *d;

	sys->npolicies = sys->ncpus = sys->npackages = 0;
	lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq");
	d = opendir(path);
	if (!d) {
		errf(err, errlen, "cannot open %s", path);
		return -1;
	}
	while ((e = readdir(d))) {
		struct lw_policy *p;
		uint64_t v;
		int id;

		if (!numbered_name(e->d_name, "policy", &id))
			continue;
		if (sys->npolicies == LW_MAX_POLICIES) {
			errf(err, errlen, "more than %d cpufreq policies are unsupported", LW_MAX_POLICIES);
			goto fail;
		}
		p = &sys->policies[sys->npolicies];
		memset(p, 0, sizeof(*p));
		p->id = id;
		p->zone = -1;
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/related_cpus", p->id);
		if (lw_read_str(path, buf, sizeof(buf))) {
			errf(err, errlen, "cannot read policy%d related_cpus", p->id);
			goto fail;
		}
		p->ncpus = parse_cpulist(buf, p->cpus, LW_MAX_CPUS);
		if (p->ncpus <= 0) {
			errf(err, errlen, "policy%d has an invalid/duplicate CPU list; supported CPU IDs are 0..%d",
			     p->id, LW_MAX_CPUS - 1);
			goto fail;
		}
		for (int c = 0; c < p->ncpus; c++) {
			if (seen[p->cpus[c]]) {
				errf(err, errlen, "CPU %d occurs in more than one cpufreq policy", p->cpus[c]);
				goto fail;
			}
			seen[p->cpus[c]] = true;
		}
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_min_freq", p->id);
		if (read_frequency(path, &p->cpuinfo_min_khz)) {
			errf(err, errlen, "policy%d has no valid positive cpuinfo_min_freq", p->id);
			goto fail;
		}
		lw_path(sys, path, sizeof(path),
			"/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", p->id);
		if (read_frequency(path, &p->cpuinfo_max_khz) ||
		    p->cpuinfo_max_khz < p->cpuinfo_min_khz) {
			errf(err, errlen, "policy%d has no valid frequency range", p->id);
			goto fail;
		}
		lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity",
			p->cpus[0]);
		p->capacity = lw_read_u64(path, &v) || !v || v > 1024 ? 1024 : (uint32_t)v;
		lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_driver", p->id);
		if (lw_read_str(path, p->driver, sizeof(p->driver)))
			snprintf(p->driver, sizeof(p->driver), "unknown");
		lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", p->id);
		if (lw_read_str(path, p->governor, sizeof(p->governor)))
			snprintf(p->governor, sizeof(p->governor), "unknown");
		if (read_opps(sys, p)) {
			errf(err, errlen, "policy%d has invalid frequencies or exceeds %d candidates", p->id, LW_MAX_OPPS);
			goto fail;
		}
		read_em(sys, p);
		read_idle(sys, p);
		lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", p->cpus[0]);
		if (!lw_read_u64(path, &v) && v <= INT_MAX) {
			bool known = false;

			for (int k = 0; k < sys->npackages; k++)
				known |= packages[k] == (int)v;
			if (!known)
				packages[sys->npackages++] = (int)v;
		}
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

fail:
	closedir(d);
	return -1;
}

static int policy_of_cpu(const struct lw_system *sys, int cpu)
{
	for (int i = 0; i < sys->npolicies; i++)
		for (int c = 0; c < sys->policies[i].ncpus; c++)
			if (sys->policies[i].cpus[c] == cpu)
				return i;
	return -1;
}

static bool usable_temperature(const char *path)
{
	int64_t mc;
	const char *suffix = strrchr(path, '/');
	char fault_path[LW_PATH_MAX];
	uint64_t fault;
	int ret;

	/* A successful input read does not clear an explicitly reported fault. */
	if (suffix && !strncmp(suffix + 1, "temp", 4)) {
		suffix += 5;
		if (isdigit((unsigned char)*suffix)) {
			while (isdigit((unsigned char)*suffix))
				suffix++;
			if (!strcmp(suffix, "_input")) {
				ret = snprintf(fault_path, sizeof(fault_path), "%.*s_fault",
					       (int)(suffix - path), path);
				if (ret < 0 || (size_t)ret >= sizeof(fault_path))
					return false;
				ret = lw_read_u64(fault_path, &fault);
				if (ret != -ENOENT && (ret || fault))
					return false;
			}
		}
	}

	return !lw_read_s64(path, &mc) && mc >= -40000 && mc <= 200000;
}

static bool package_temperature_type(const char *type)
{
	return !strcmp(type, "package") || !strcmp(type, "package-thermal") ||
	       !strcmp(type, "package_thermal") || !strcmp(type, "soc") ||
	       !strcmp(type, "soc-thermal") || !strcmp(type, "soc_thermal") ||
	       !strcmp(type, "x86_pkg_temp");
}

static bool cpu_temperature_type(const char *type)
{
	const char *suffix;

	if (package_temperature_type(type) || !strcmp(type, "cpu-thermal") ||
	    !strcmp(type, "cpu_thermal") || !strcmp(type, "little-thermal") ||
	    !strcmp(type, "littlecore-thermal"))
		return true;
	if (strncmp(type, "bigcore", 7))
		return false;
	suffix = type + 7;
	if (!isdigit((unsigned char)*suffix))
		return false;
	while (isdigit((unsigned char)*suffix))
		suffix++;
	return !strcmp(suffix, "-thermal");
}

static int discover_zones(struct lw_system *sys, bool *ambiguous_bindings,
			  char *err, size_t errlen)
{
	char path[LW_PATH_MAX], buf[64];
	int binding[LW_MAX_POLICIES];
	bool ambiguous[LW_MAX_POLICIES] = { false };
	struct dirent *e;
	DIR *d;
	int packages = 0, package = -1;

	*ambiguous_bindings = false;
	for (int i = 0; i < sys->npolicies; i++)
		binding[i] = -1;
	sys->nzones = 0;
	sys->package_zone = -1;
	sys->sensor_ambiguous = sys->npackages > 1;
	lw_path(sys, path, sizeof(path), "/sys/class/thermal");
	d = opendir(path);
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		struct lw_zone *z;
		int id;

		if (!numbered_name(e->d_name, "thermal_zone", &id))
			continue;
		if (sys->nzones == LW_MAX_ZONES) {
			errf(err, errlen, "more than %d thermal zones are unsupported", LW_MAX_ZONES);
			closedir(d);
			return -1;
		}
		z = &sys->zones[sys->nzones];
		memset(z, 0, sizeof(*z));
		z->id = id;
		z->passive_mc = INT32_MAX;
		z->critical_mc = INT32_MAX;
		lw_path(sys, path, sizeof(path), "/sys/class/thermal/%s/type", e->d_name);
		if (lw_read_str(path, z->type, sizeof(z->type)))
			continue;
		lw_path(sys, z->temp_path, sizeof(z->temp_path), "/sys/class/thermal/%s/temp", e->d_name);
		for (int t = 0; t < 16; t++) {
			int64_t temp;

			lw_path(sys, path, sizeof(path),
				"/sys/class/thermal/%s/trip_point_%d_type", e->d_name, t);
			if (lw_read_str(path, buf, sizeof(buf)))
				break;
			lw_path(sys, path, sizeof(path),
				"/sys/class/thermal/%s/trip_point_%d_temp", e->d_name, t);
			if (lw_read_s64(path, &temp) || temp < -40000 || temp > 200000)
				continue;
			if (!strcmp(buf, "passive") && temp < z->passive_mc)
				z->passive_mc = (int32_t)temp;
			if (!strcmp(buf, "critical") && temp < z->critical_mc)
				z->critical_mc = (int32_t)temp;
		}
		/* A CPU cooling device can also cool skin/GPU/board zones. Only
		 * recognized, readable CPU sensors qualify as policy observations. */
		bool usable = usable_temperature(z->temp_path);

		for (int k = 0; usable && cpu_temperature_type(z->type) && k < 16; k++) {
			int cpu, pol;

			lw_path(sys, path, sizeof(path), "/sys/class/thermal/%s/cdev%d/type",
				e->d_name, k);
			if (lw_read_str(path, buf, sizeof(buf)))
				continue;
			if (sscanf(buf, "cpufreq-cpu%d", &cpu) != 1)
				continue;
			pol = policy_of_cpu(sys, cpu);
			if (pol < 0)
				continue;
			if (binding[pol] >= 0 && binding[pol] != sys->nzones)
				ambiguous[pol] = true;
			else
				binding[pol] = sys->nzones;
		}
		if (package_temperature_type(z->type) && usable) {
			packages++;
			package = sys->nzones;
		}
		sys->nzones++;
	}
	closedir(d);
	if (packages == 1 && sys->npackages <= 1)
		sys->package_zone = package;
	else if (packages > 1 || sys->npackages > 1)
		sys->sensor_ambiguous = true;
	for (int i = 0; i < sys->npolicies; i++) {
		sys->policies[i].zone = ambiguous[i] ? -1 : binding[i];
		*ambiguous_bindings |= ambiguous[i];
	}
	return 0;
}

/* Only recognized physical CPU-package inputs are selected automatically.
 * Tctl, motherboard, GPU and NVMe temperatures need an explicit choice. */
static int discover_hwmon_temperature(struct lw_system *sys, char *err, size_t errlen)
{
	struct lw_zone selected = { .id = -1, .passive_mc = INT32_MAX, .critical_mc = INT32_MAX };
	char path[LW_PATH_MAX], name[64], label[64];
	struct dirent *e;
	DIR *d;
	int candidates = 0;

	if (sys->package_zone >= 0 || sys->sensor_ambiguous)
		return 0;
	lw_path(sys, path, sizeof(path), "/sys/class/hwmon");
	d = opendir(path);
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		int id;

		if (!numbered_name(e->d_name, "hwmon", &id))
			continue;
		lw_path(sys, path, sizeof(path), "/sys/class/hwmon/%s/name", e->d_name);
		if (lw_read_str(path, name, sizeof(name)) ||
		    (strcmp(name, "coretemp") && strcmp(name, "k10temp")))
			continue;
		for (int t = 1; t <= 64; t++) {
			lw_path(sys, path, sizeof(path), "/sys/class/hwmon/%s/temp%d_label", e->d_name, t);
			if (lw_read_str(path, label, sizeof(label)))
				continue;
			if ((!strcmp(name, "coretemp") && strncmp(label, "Package id ", 11)) ||
			    (!strcmp(name, "k10temp") && strcmp(label, "Tdie")))
				continue;
			lw_path(sys, path, sizeof(path), "/sys/class/hwmon/%s/temp%d_input", e->d_name, t);
			if (!usable_temperature(path))
				continue;
			candidates++;
			snprintf(selected.temp_path, sizeof(selected.temp_path), "%s", path);
			snprintf(selected.type, sizeof(selected.type), "%.20s:%.40s", name, label);
			selected.critical_mc = INT32_MAX;
			lw_path(sys, path, sizeof(path), "/sys/class/hwmon/%s/temp%d_crit", e->d_name, t);
			int64_t critical;

			if (!lw_read_s64(path, &critical) && critical > 0 && critical <= 200000)
				selected.critical_mc = (int32_t)critical;
		}
	}
	closedir(d);
	if (candidates > 1) {
		sys->sensor_ambiguous = true;
		return 0;
	}
	if (candidates == 1) {
		if (sys->nzones == LW_MAX_ZONES) {
			errf(err, errlen, "no room for the CPU hwmon sensor; maximum is %d zones", LW_MAX_ZONES);
			return -1;
		}
		sys->package_zone = sys->nzones;
		sys->zones[sys->nzones++] = selected;
	}
	return 0;
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
	bool ambiguous_bindings;

	if (discover_policies(sys, err, errlen))
		return -1;
	if (discover_zones(sys, &ambiguous_bindings, err, errlen) ||
	    discover_hwmon_temperature(sys, err, errlen))
		return -1;
	if (ambiguous_bindings && sys->package_zone < 0)
		sys->sensor_ambiguous = true;
	discover_meter(sys);
	return 0;
}

int lw_select_temperature_sensor(struct lw_system *sys, const char *path,
				 char *err, size_t errlen)
{
	char resolved[LW_PATH_MAX];
	const char *relative = path;
	size_t n = strlen(sys->sysroot);
	int index = -1;

	if (path && strcmp(sys->sysroot, "/") && n && !strncmp(path, sys->sysroot, n) && path[n] == '/')
		relative = path + n;
	if (!relative || strncmp(relative, "/sys/", 5) || strstr(relative, "/../") ||
	    strstr(relative, "/./") || lw_path(sys, resolved, sizeof(resolved), "%s", relative)) {
		errf(err, errlen, "temperature sensor must be an absolute /sys/... input under the selected sysroot");
		return -1;
	}
	if (!usable_temperature(resolved)) {
		errf(err, errlen, "temperature sensor is unreadable or invalid: %s", resolved);
		return -1;
	}
	for (int z = 0; z < sys->nzones; z++)
		if (!strcmp(sys->zones[z].temp_path, resolved))
			index = z;
	if (index < 0) {
		if (sys->nzones == LW_MAX_ZONES) {
			errf(err, errlen, "no room for an explicit temperature sensor");
			return -1;
		}
		index = sys->nzones++;
		struct lw_zone *z = &sys->zones[index];

		memset(z, 0, sizeof(*z));
		z->id = -1;
		z->passive_mc = z->critical_mc = INT32_MAX;
		snprintf(z->type, sizeof(z->type), "explicit temperature input");
		snprintf(z->temp_path, sizeof(z->temp_path), "%s", resolved);
	}
	sys->package_zone = index;
	sys->sensor_ambiguous = false;
	for (int i = 0; i < sys->npolicies; i++)
		sys->policies[i].zone = index;
	return 0;
}

int lw_check_system(const struct lw_system *sys, bool writable, FILE *out)
{
	int failures = 0;
	char path[LW_PATH_MAX];

	if (!sys->npolicies) {
		if (out)
			fprintf(out, "lerwatt: check failed: no usable cpufreq policies\n");
		return -1;
	}
	if (sys->npackages > 1) {
		if (out)
			fprintf(out, "lerwatt: check failed: %d CPU packages detected; multi-socket control needs per-package sensor mapping, which is unsupported (a single --temperature-sensor cannot override this)\n",
				sys->npackages);
		failures++;
	}
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];
		int z = p->zone >= 0 ? p->zone : sys->package_zone;
		uint32_t low = 0, high = 0;

		if (z < 0 || z >= sys->nzones || !usable_temperature(sys->zones[z].temp_path)) {
			if (out)
				fprintf(out, "lerwatt: check failed: policy%d needs a usable CPU temperature input%s; use --temperature-sensor /sys/...\n",
					p->id, sys->sensor_ambiguous ? " (automatic mapping is ambiguous)" : "");
			failures++;
		}
		for (int k = 0; k < 2; k++) {
			lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_%s_freq",
				p->id, k ? "max" : "min");
			if (read_frequency(path, k ? &high : &low)) {
				if (out)
					fprintf(out, "lerwatt: check failed: invalid frequency limit %s\n", path);
				failures++;
			}
			if (writable) {
				int fd = open(path, O_WRONLY | O_CLOEXEC);

				if (fd < 0) {
					if (out)
						fprintf(out, "lerwatt: check failed: cannot open %s for control: %s\n", path, strerror(errno));
					failures++;
				} else {
					close(fd);
				}
			}
		}
		if (low > high) {
			if (out)
				fprintf(out, "lerwatt: check failed: policy%d minimum limit %u exceeds maximum %u kHz\n",
					p->id, low, high);
			failures++;
		}
		lw_path(sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", p->id);
		uint32_t current;

		if (read_frequency(path, &current)) {
			if (out)
				fprintf(out, "lerwatt: check failed: unavailable current frequency for policy%d\n", p->id);
			failures++;
		}
	}
	if (!failures) {
		struct lw_cpustat history = { 0 };
		struct lw_sample sample;

		if (lw_sample(sys, &history, &sample)) {
			if (out)
				fprintf(out, "lerwatt: check failed: runtime sampling is unsupported; check for missing/offline CPU records in /proc/stat, unavailable CPU statistics or current-frequency inputs\n");
			failures++;
		}
	}
	if (out)
		fprintf(out, "lerwatt: capability check %s (%s; no values written)\n",
			failures ? "failed" : "passed", writable ? "control file access" : "read-only inputs");
	return failures ? -1 : 0;
}

void lw_describe(const struct lw_system *sys, FILE *out)
{
	fprintf(out, "lerwatt: mode=%s sysroot=%s%s%s\n",
		sys->simulated ? "simulation" : "hardware", sys->sysroot,
		sys->simulated ? " platform=" : "", sys->simulated ? sys->sim_platform : "");
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];

		int zone = p->zone >= 0 ? p->zone : sys->package_zone;

		fprintf(out, "lerwatt:   cluster %d = policy%d cpus %d-%d cap %u, driver %s, governor %s, %d %s %u-%u kHz, power %s, sensor %s, %d idle states\n",
			i, p->id, p->cpus[0], p->cpus[p->ncpus - 1], p->capacity,
			p->driver, p->governor, p->nopps, p->synthetic_opps ? "synthetic limit candidates" : "OPPs",
			p->opps[0].khz, p->opps[p->nopps - 1].khz, p->have_em ? "Energy Model estimate" : "relative proxy (not measured watts)",
			zone >= 0 ? sys->zones[zone].temp_path : "unmapped", p->nidle);
	}
	fprintf(out, "lerwatt:   %d temperature inputs, %d known CPU packages, package sensor %s%s, meter %s\n", sys->nzones,
		sys->npackages,
		sys->package_zone >= 0 ? sys->zones[sys->package_zone].type : "(none)",
		sys->sensor_ambiguous ? " (ambiguous: select explicitly)" : "",
		sys->meter_path[0] ? sys->meter_name : "none (power is estimated; see per-policy source above)");
}
