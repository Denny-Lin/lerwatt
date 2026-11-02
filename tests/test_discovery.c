// SPDX-License-Identifier: MIT
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lerwatt/hal.h"

static char fixture[LW_PATH_MAX];
static struct lw_system sys;
static char error[256];

static void cpustat(void);

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *walk)
{
	(void)st;
	(void)type;
	(void)walk;
	return remove(path);
}

static void cleanup(void)
{
	if (fixture[0])
		nftw(fixture, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	fixture[0] = '\0';
}

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_discovery: %s (%s)\n", message, error);
		cleanup();
		exit(1);
	}
}

static void fresh(void)
{
	cleanup();
	snprintf(fixture, sizeof(fixture), "/tmp/lerwatt-discovery-XXXXXX");
	require(mkdtemp(fixture) != NULL, "mkdtemp");
	error[0] = '\0';
	cpustat();
}

static void fixture_path(char *out, size_t capacity, const char *relative)
{
	size_t prefix = strlen(fixture), suffix = strlen(relative);

	require(prefix < capacity && suffix < capacity - prefix, "fixture path length");
	memcpy(out, fixture, prefix);
	memcpy(out + prefix, relative, suffix + 1);
}

static void put(const char *relative, const char *format, ...)
{
	char path[LW_PATH_MAX];
	va_list args;

	fixture_path(path, sizeof(path), relative);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		require(!mkdir(path, 0700) || errno == EEXIST, "fixture mkdir");
		*p = '/';
	}
	FILE *file = fopen(path, "w");

	require(file != NULL, "fixture open");
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	require(!fclose(file), "fixture close");
}

static void erase(const char *relative)
{
	char path[LW_PATH_MAX];

	fixture_path(path, sizeof(path), relative);
	require(!unlink(path), "remove fixture input");
}

static void policy(int id, const char *cpus, const char *table)
{
	char path[192];

#define POLICY_FILE(attr, ...) do { \
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/%s", id, attr); \
	put(path, __VA_ARGS__); \
} while (0)
	POLICY_FILE("related_cpus", "%s\n", cpus);
	POLICY_FILE("cpuinfo_min_freq", "400000\n");
	POLICY_FILE("cpuinfo_max_freq", "4800000\n");
	POLICY_FILE("scaling_min_freq", "400000\n");
	POLICY_FILE("scaling_max_freq", "4800000\n");
	POLICY_FILE("scaling_cur_freq", "2400000\n");
	POLICY_FILE("scaling_driver", "intel_pstate\n");
	POLICY_FILE("scaling_governor", "powersave\n");
	if (table)
		POLICY_FILE("scaling_available_frequencies", "%s\n", table);
#undef POLICY_FILE
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", atoi(cpus));
	put(path, "0\n");
}

static void thermal(const char *type)
{
	put("/sys/class/thermal/thermal_zone0/type", "%s\n", type);
	put("/sys/class/thermal/thermal_zone0/temp", "50000\n");
	put("/sys/class/thermal/thermal_zone0/trip_point_0_type", "passive\n");
	put("/sys/class/thermal/thermal_zone0/trip_point_0_temp", "85000\n");
}

static void bound_thermal(int id, const char *type, int cpu)
{
	char path[192];

	snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", id);
	put(path, "%s\n", type);
	snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", id);
	put(path, "50000\n");
	snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/cdev0/type", id);
	put(path, "cpufreq-cpu%d\n", cpu);
}

static void cpustat(void)
{
	char text[16384];
	size_t length = 0;

	for (int cpu = 0; cpu < LW_MAX_CPUS; cpu++)
		length += (size_t)snprintf(text + length, sizeof(text) - length,
					 "cpu%d 100 0 20 1000 0 0 0 0 0 0\n", cpu);
	put("/proc/stat", "%s", text);
}

static int discover(void)
{
	if (lw_detect(&sys, fixture, error, sizeof(error)))
		return -1;
	return lw_discover(&sys, error, sizeof(error));
}

int main(void)
{
	struct lw_cpustat history = { 0 };
	struct lw_sample sample;

	fresh();
	const char *bad_roots[] = { "", "relative", "/tmp/./root", "/tmp/../root", "/tmp/." };

	for (size_t k = 0; k < sizeof(bad_roots) / sizeof(bad_roots[0]); k++)
		require(lw_detect(&sys, bad_roots[k], error, sizeof(error)) < 0, "unsafe sysroot rejected before journal use");
	char root_path[LW_PATH_MAX + 4];

	memset(root_path, '/', LW_PATH_MAX);
	root_path[LW_PATH_MAX] = '\0';
	require(lw_detect(&sys, root_path, error, sizeof(error)) < 0, "oversized sysroot rejected without truncation");
	fixture_path(root_path, sizeof(root_path), "///");
	require(!lw_detect(&sys, root_path, error, sizeof(error)) && !strcmp(sys.sysroot, fixture),
		"trailing sysroot slashes normalized for recovery");

	fresh();
	policy(0, "0-3", "400000 1200000 2400000 4800000");
	thermal("soc-thermal");
	cpustat();
	require(!discover(), "discrete discovery");
	require(sys.policies[0].nopps == 4 && !sys.policies[0].synthetic_opps, "preserve existing discrete OPPs");
	require(!lw_check_system(&sys, true, NULL), "writable capability check");
	require(!lw_sample(&sys, &history, &sample) && sample.zone_mc[sys.package_zone] == 50000, "thermal input sampling");
	uint64_t maximum;
	char path[LW_PATH_MAX];

	lw_path(&sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq");
	require(!lw_read_u64(path, &maximum) && maximum == 4800000, "capability check did not alter limits");
	erase("/proc/stat");
	require(lw_sample(&sys, &history, &sample) < 0, "missing /proc/stat is an error");

	fresh();
	for (int i = 0; i < 16; i++) {
		char cpus[16];

		snprintf(cpus, sizeof(cpus), "%d", i);
		policy(i, cpus, NULL);
	}
	thermal("x86_pkg_temp");
	cpustat();
	require(!discover() && sys.npolicies == 16, "all sixteen Intel per-CPU policies discovered");
	require(!lw_check_system(&sys, false, NULL), "Intel package sensor recognized");
	for (int i = 0; i < 16; i++) {
		const struct lw_policy *p = &sys.policies[i];
		bool intermediate = false;

		require(p->id == i && p->synthetic_opps && !strcmp(p->driver, "intel_pstate"), "ordered range-based policies");
		for (int j = 0; j < p->nopps; j++)
			intermediate |= p->opps[j].khz >= p->cpuinfo_max_khz / 2 && p->opps[j].khz < p->cpuinfo_max_khz;
		require(intermediate, "range candidates permit a ceiling below the maximum");
	}
	put("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "0\n");
	require(lw_sample(&sys, &history, &sample) < 0, "zero current frequency rejected");

	fresh();
	policy(255, "255", NULL);
	thermal("x86_pkg_temp");
	cpustat();
	require(!discover() && sys.ncpus == 256 && !lw_sample(&sys, &history, &sample), "highest supported sparse CPU ID");
	const char *bad_lists[] = { "256", "0,0", "0-2,2", "-1", "0-", "0junk", "2-1", "0," };

	for (size_t k = 0; k < sizeof(bad_lists) / sizeof(bad_lists[0]); k++) {
		fresh();
		policy(0, bad_lists[k], NULL);
		require(discover() < 0, "invalid or overflowing CPU list rejected");
	}
	fresh();
	policy(0, "0", NULL);
	policy(1, "0", NULL);
	require(discover() < 0, "CPU cannot belong to two policies");
	fresh();
	for (int i = 0; i <= LW_MAX_POLICIES; i++) {
		char cpus[16];

		snprintf(cpus, sizeof(cpus), "%d", i);
		policy(i, cpus, NULL);
	}
	require(discover() < 0 && strstr(error, "more than") != NULL, "policy overflow is explicit");

	const char *bad_tables[] = { "0", "400000 bad", "400000 400000", "400000 4900000" };

	for (size_t k = 0; k < sizeof(bad_tables) / sizeof(bad_tables[0]); k++) {
		fresh();
		policy(0, "0", bad_tables[k]);
		require(discover() < 0, "invalid OPP table rejected");
	}
	fresh();
	policy(0, "0", NULL);
	char table[1024] = "";

	for (int i = 0; i <= LW_MAX_OPPS; i++)
		snprintf(table + strlen(table), sizeof(table) - strlen(table), "%d ", 400000 + i * 100000);
	put("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies", "%s", table);
	require(discover() < 0, "OPP count overflow rejected");
	fresh();
	policy(0, "0", NULL);
	put("/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq", "0\n");
	require(discover() < 0, "unknown frequency range rejected before division");
	put("/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq", "4800000junk\n");
	require(discover() < 0, "numeric suffix rejected");
	const char *invalid_trips[] = { "-274000", "-2147483648", "-9223372036854775808", "200001" };

	for (size_t k = 0; k < sizeof(invalid_trips) / sizeof(invalid_trips[0]); k++) {
		fresh();
		policy(0, "0", NULL);
		thermal("soc-thermal");
		put("/sys/class/thermal/thermal_zone0/trip_point_0_temp", "%s\n", invalid_trips[k]);
		put("/sys/class/thermal/thermal_zone0/trip_point_1_type", "critical\n");
		put("/sys/class/thermal/thermal_zone0/trip_point_1_temp", "%s\n", invalid_trips[k]);
		require(!discover() && sys.package_zone >= 0 &&
			sys.zones[sys.package_zone].passive_mc == INT32_MAX &&
			sys.zones[sys.package_zone].critical_mc == INT32_MAX,
			"disabled and out-of-range trips ignored before narrowing");
	}

	fresh();
	policy(0, "0", NULL);
	thermal("x86_pkg_temp");
	bound_thermal(1, "skin-thermal", 0);
	bound_thermal(2, "cpu-thermal", 0);
	require(!discover() && sys.package_zone >= 0 && sys.policies[0].zone >= 0 &&
		!strcmp(sys.zones[sys.policies[0].zone].type, "cpu-thermal") &&
		!lw_check_system(&sys, false, NULL), "CPU sensor preferred over skin cooling binding");

	fresh();
	policy(0, "0", NULL);
	bound_thermal(0, "skin-thermal", 0);
	bound_thermal(1, "gpu-soc-thermal", 0);
	require(!discover() && sys.policies[0].zone < 0 && sys.package_zone < 0 &&
		lw_check_system(&sys, false, NULL) < 0, "unrelated cooling bindings do not establish CPU temperature");
	require(!lw_select_temperature_sensor(&sys, "/sys/class/thermal/thermal_zone0/temp", error, sizeof(error)) &&
		!lw_check_system(&sys, false, NULL), "unrecognized sensor requires explicit override");

	fresh();
	policy(0, "0", NULL);
	bound_thermal(0, "cpu-thermal", 0);
	bound_thermal(1, "bigcore0-thermal", 0);
	require(!discover() && sys.policies[0].zone < 0 && sys.sensor_ambiguous &&
		lw_check_system(&sys, false, NULL) < 0, "multiple CPU bindings do not select readdir first");
	put("/sys/class/hwmon/hwmon3/name", "coretemp\n");
	put("/sys/class/hwmon/hwmon3/temp1_label", "Package id 0\n");
	put("/sys/class/hwmon/hwmon3/temp1_input", "52000\n");
	require(!discover() && sys.policies[0].zone < 0 && sys.package_zone >= 0 &&
		!lw_check_system(&sys, false, NULL), "ambiguous CPU bindings use recognized package fallback");

	fresh();
	policy(0, "0", NULL);
	policy(4, "4", NULL);
	policy(6, "6", NULL);
	thermal("package-thermal");
	bound_thermal(1, "bigcore0-thermal", 4);
	bound_thermal(2, "bigcore2-thermal", 6);
	bound_thermal(3, "littlecore-thermal", 0);
	require(!discover() && sys.package_zone >= 0 &&
		sys.policies[0].zone >= 0 && sys.policies[1].zone >= 0 && sys.policies[2].zone >= 0 &&
		!strcmp(sys.zones[sys.policies[0].zone].type, "littlecore-thermal") &&
		!strcmp(sys.zones[sys.policies[1].zone].type, "bigcore0-thermal") &&
		!strcmp(sys.zones[sys.policies[2].zone].type, "bigcore2-thermal") &&
		!lw_check_system(&sys, false, NULL), "frozen RK3588 CPU sensor mappings preserved");

	fresh();
	policy(0, "0", NULL);
	put("/sys/class/hwmon/hwmon3/name", "coretemp\n");
	put("/sys/class/hwmon/hwmon3/temp1_label", "Package id 0\n");
	put("/sys/class/hwmon/hwmon3/temp1_input", "52000\n");
	put("/sys/class/hwmon/hwmon3/temp1_crit", "105000\n");
	cpustat();
	require(!discover() && sys.package_zone >= 0 && !lw_check_system(&sys, false, NULL), "coretemp package auto selection");
	require(!lw_sample(&sys, &history, &sample) && sample.zone_mc[sys.package_zone] == 52000, "hwmon temperature sampling");
	put("/sys/class/hwmon/hwmon3/temp1_fault", "0\n");
	require(!lw_check_system(&sys, false, NULL), "clear optional sensor fault accepted");
	put("/sys/class/hwmon/hwmon3/temp1_fault", "1\n");
	require(lw_check_system(&sys, false, NULL) < 0, "reported sensor fault blocks control");
	require(!lw_sample(&sys, &history, &sample) && sample.zone_mc[sys.package_zone] == INT32_MIN,
		"runtime sensor fault invalidates sampled temperature");
	put("/sys/class/hwmon/hwmon3/temp1_fault", "bad\n");
	require(lw_check_system(&sys, false, NULL) < 0, "malformed optional fault fails safely");
	erase("/sys/class/hwmon/hwmon3/temp1_fault");
	put("/sys/class/hwmon/hwmon4/name", "coretemp\n");
	put("/sys/class/hwmon/hwmon4/temp1_label", "Package id 1\n");
	put("/sys/class/hwmon/hwmon4/temp1_input", "53000\n");
	require(!discover() && sys.sensor_ambiguous && lw_check_system(&sys, false, NULL) < 0, "ambiguous sensors never select the first candidate");
	require(!lw_select_temperature_sensor(&sys, "/sys/class/hwmon/hwmon4/temp1_input", error, sizeof(error)) &&
		!lw_check_system(&sys, false, NULL), "explicit sensor override");
	require(lw_select_temperature_sensor(&sys, "/etc/passwd", error, sizeof(error)) < 0, "override stays under selected sysroot sysfs");

	fresh();
	policy(0, "0", NULL);
	put("/sys/class/hwmon/hwmon0/name", "k10temp\n");
	put("/sys/class/hwmon/hwmon0/temp1_label", "Tctl\n");
	put("/sys/class/hwmon/hwmon0/temp1_input", "66000\n");
	require(!discover() && sys.package_zone < 0 && lw_check_system(&sys, false, NULL) < 0, "Tctl is not silently treated as physical Tdie");
	put("/sys/class/hwmon/hwmon0/temp2_label", "Tdie\n");
	put("/sys/class/hwmon/hwmon0/temp2_input", "56000\n");
	require(!discover() && sys.package_zone >= 0 && !lw_check_system(&sys, false, NULL), "AMD physical Tdie recognized");
	policy(1, "1", NULL);
	put("/sys/devices/system/cpu/cpu1/topology/physical_package_id", "1\n");
	require(!discover() && sys.npackages == 2 && lw_check_system(&sys, false, NULL) < 0, "multi-package control is unsupported");
	require(!lw_select_temperature_sensor(&sys, "/sys/class/hwmon/hwmon0/temp2_input", error, sizeof(error)) &&
		lw_check_system(&sys, false, NULL) < 0, "one-sensor override cannot bypass unsupported multi-socket control");

	fresh();
	policy(0, "0-1", NULL);
	thermal("soc-thermal");
	put("/proc/stat", "cpu0 100 0 20 1000 0 0 0 0 0 0\n");
	require(!discover() && lw_sample(&sys, &history, &sample) < 0 && !history.valid,
		"missing discovered CPU statistics fail instead of reporting idle");
	lw_path(&sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq");
	require(lw_check_system(&sys, true, NULL) < 0 &&
		!lw_read_u64(path, &maximum) && maximum == 4800000,
		"offline or missing CPU statistics fail check without changing limits");
	lw_path(&sys, path, sizeof(path), "/run/lerwatt");
	require(access(path, F_OK) < 0 && errno == ENOENT, "failed capability check creates no runtime state");

	fresh();
	policy(0, "0", NULL);
	thermal("soc-thermal");
	put("/proc/stat", "cpu0 10 0 0 3000 0 0 0 0 0 0\n");
	require(!discover(), "counter reset fixture discovery");
	history.valid = true;
	history.busy[0] = 1000;
	history.total[0] = 2000;
	require(!lw_sample(&sys, &history, &sample) && sample.busy_mean[0] == 0 &&
		history.valid && history.busy[0] == 10 && history.total[0] == 3010,
		"decreasing busy counter resets history without unsigned wrap");
	put("/proc/stat", "cpu0 20 0 0 3010 0 0 0 0 0 0\n");
	require(!lw_sample(&sys, &history, &sample) &&
		sample.busy_mean[0] > .49 && sample.busy_mean[0] < .51,
		"CPU utilization resumes from the refreshed counter anchor");
	put("/proc/stat", "cpu0 1 0 0 10 0 0 0 0 0 0\n");
	require(!lw_sample(&sys, &history, &sample) && sample.busy_mean[0] == 0,
		"decreasing total counter also resets history");

	fresh();
	policy(0, "0", "400000 4800000");
	thermal("soc-thermal");
	put("/sys/kernel/debug/energy_model/cpu0/ps:400000/power", "100\n");
	put("/sys/kernel/debug/energy_model/cpu0/ps:400000/performance", "100\n");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/power", "1000\n");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance", "1024\n");
	put("/sys/kernel/debug/energy_model/cpu0/units", "milliWatts\n");
	erase("/sys/kernel/debug/energy_model/cpu0/ps:400000/performance");
	erase("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance");
	require(!discover() && sys.policies[0].have_em && sys.policies[0].opps[1].power_uw == 1000000,
		"legacy milliW EM without performance fields converts to microW");
	require(sys.policies[0].opps[0].perf == 85 && sys.policies[0].opps[1].perf == 1024,
		"legacy EM retains frequency-derived capacities");
	put("/sys/kernel/debug/energy_model/cpu0/ps:400000/performance", "100\n");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance", "1024\n");
	put("/sys/kernel/debug/energy_model/cpu0/units", "bogoWatts\n");
	require(!discover() && !sys.policies[0].have_em, "abstract EM uses relative proxy");
	erase("/sys/kernel/debug/energy_model/cpu0/units");
	put("/sys/kernel/debug/energy_model/cpu0/flags", "0x1\n");
	require(!discover() && !sys.policies[0].have_em, "ambiguous flags without kernel ABI version use proxy");
	put("/proc/sys/kernel/osrelease", "5.19.0\n");
	require(!discover() && !sys.policies[0].have_em, "old flags are not mistaken for microWatts");
	put("/proc/sys/kernel/osrelease", "6.8.0-generic\n");
	require(!discover() && sys.policies[0].have_em && sys.policies[0].opps[1].power_uw == 1000,
		"verified current microW flags retain their units");
	put("/sys/kernel/debug/energy_model/cpu0/flags", "0x5\n");
	require(!discover() && !sys.policies[0].have_em, "artificial current EM uses proxy");
	put("/sys/kernel/debug/energy_model/cpu0/flags", "0x1\n");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance", "0\n");
	require(!discover() && !sys.policies[0].have_em && sys.policies[0].opps[0].perf == 85,
		"invalid partial EM restores all derived performance values");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance", "1024\n");
	put("/sys/kernel/debug/energy_model/cpu0/units", "milliWatts\n");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/power", "4294968\n");
	require(!discover() && !sys.policies[0].have_em, "milliW conversion overflow falls back safely");
	erase("/sys/kernel/debug/energy_model/cpu0/units");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/power", "0\n");
	require(!discover() && !sys.policies[0].have_em, "zero EM power falls back safely");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/power", "1000\n");
	erase("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance");
	require(!discover() && sys.policies[0].have_em && sys.policies[0].opps[1].perf == 1024,
		"optional missing EM performance retains derived capacity");
	put("/sys/kernel/debug/energy_model/cpu0/ps:4800000/performance", "1024\n");
	put("/proc/sys/kernel/osrelease", "4294967302.1\n");
	require(!discover() && !sys.policies[0].have_em, "invalid kernel ABI version cannot enable EM units");
	put("/.lerwatt-sim", "platform=rk3588\n");
	require(!discover() && sys.simulated && sys.policies[0].have_em &&
		sys.policies[0].opps[1].power_uw == 1000, "known simulator microW ABI remains unchanged");

	cleanup();
	puts("test_discovery: topology, frequency, sensor and read-only capability fixtures passed");
	return 0;
}
