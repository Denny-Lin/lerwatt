// SPDX-License-Identifier: MIT
/*
 * Fake kernel ABI for lerwatt-sim.
 *
 * Read-only files are replaced atomically (write a temporary, rename), so a
 * reader never sees half a value. Files lerwattd writes (scaling_{min,max}_freq,
 * pm_qos_resume_latency_us, cpuidle stateN/disable) are never rewritten by
 * the simulator: they read back what was last written. The real kernel reads
 * back the effective cpufreq limit instead; docs/SIMULATION.md lists this and
 * the other ABI deviations.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "abi.h"

#define USER_HZ	100

static int mkdir_p(const char *path)
{
	char buf[512];

	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) && errno != EEXIST)
			return -errno;
		*p = '/';
	}
	if (mkdir(buf, 0755) && errno != EEXIST)
		return -errno;
	return 0;
}

static int write_atomic(const char *path, const char *content)
{
	char tmp[600];
	size_t len = strlen(content);
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	if (write(fd, content, len) != (ssize_t)len) {
		close(fd);
		unlink(tmp);
		return -EIO;
	}
	close(fd);
	return rename(tmp, path) ? -errno : 0;
}

static void absolute(const struct sim_abi *a, char *out, size_t len, const char *rel)
{
	snprintf(out, len, "%s%s", a->root, rel);
}

/* Create a file (and its directory) once. */
static int put(const struct sim_abi *a, const char *rel, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static int put(const struct sim_abi *a, const char *rel, const char *fmt, ...)
{
	char path[600], content[1024], *slash;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	vsnprintf(content, sizeof(content), fmt, ap);
	va_end(ap);
	absolute(a, path, sizeof(path), rel);
	slash = strrchr(path, '/');
	*slash = '\0';
	ret = mkdir_p(path);
	*slash = '/';
	return ret ? ret : write_atomic(path, content);
}

/* Register a dynamic file; publish() rewrites it only when it changes. */
static int dyn(struct sim_abi *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int dyn(struct sim_abi *a, const char *fmt, ...)
{
	va_list ap;
	int i = a->ndyn;

	if (i >= SIM_ABI_MAX_DYN)
		return -1;
	va_start(ap, fmt);
	vsnprintf(a->dyn[i].rel, sizeof(a->dyn[i].rel), fmt, ap);
	va_end(ap);
	a->dyn[i].last[0] = '\0';
	a->ndyn++;
	return i;
}

static void set_dyn(struct sim_abi *a, int i, const char *content)
{
	char path[600];

	if (i < 0 || !strcmp(a->dyn[i].last, content))
		return;
	absolute(a, path, sizeof(path), a->dyn[i].rel);
	if (!write_atomic(path, content))
		snprintf(a->dyn[i].last, sizeof(a->dyn[i].last), "%s", content);
}

static bool dir_is_empty(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	bool empty = true;

	if (!d)
		return true;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
			empty = false;
			break;
		}
	closedir(d);
	return empty;
}

/* Dynamic file indices, in creation order. */
enum { DYN_STAT, DYN_METER, DYN_FIRST };

static int idx_cur[SIM_MAX_CLUSTERS], idx_tis[SIM_MAX_CLUSTERS], idx_trans[SIM_MAX_CLUSTERS];
static int idx_cdev[SIM_MAX_CLUSTERS], idx_zone[SIM_MAX_ZONES];
static int idx_itime[SIM_MAX_CPUS][SIM_MAX_IDLE], idx_iuse[SIM_MAX_CPUS][SIM_MAX_IDLE];

int sim_abi_create(struct sim_abi *a, const char *root, const struct sim_state *s,
		   int force, char *err, size_t errlen)
{
	const struct sim_platform *p = s->plat;
	char path[600], buf[512];
	int cdev = 0;

	memset(a, 0, sizeof(*a));
	if (!root || !root[0] || !strcmp(root, "/") || !strncmp(root, "/sys", 4) ||
	    !strncmp(root, "/proc", 5)) {
		snprintf(err, errlen, "refusing to use %s as a simulator root", root ? root : "");
		return -1;
	}
	snprintf(a->root, sizeof(a->root), "%s", root);
	absolute(a, path, sizeof(path), "/" ".lerwatt-sim");
	if (!force && access(path, F_OK) && !dir_is_empty(root)) {
		snprintf(err, errlen, "%s is not empty and has no simulator marker; use --force", root);
		return -1;
	}
	unlink(path);	/* invisible to lerwattd until the tree is complete */
	if (mkdir_p(root)) {
		snprintf(err, errlen, "cannot create %s: %s", root, strerror(errno));
		return -1;
	}

	if (dyn(a, "/proc/stat") != DYN_STAT || dyn(a, "/sys/class/hwmon/hwmon0/power1_input") != DYN_METER)
		return -1;

	snprintf(buf, sizeof(buf), "0-%d\n", p->ncpus - 1);
	put(a, "/sys/devices/system/cpu/possible", "%s", buf);
	put(a, "/sys/devices/system/cpu/present", "%s", buf);
	put(a, "/sys/devices/system/cpu/online", "%s", buf);

	for (int cpu = 0; cpu < p->ncpus; cpu++) {
		char rel[160];

		snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
		put(a, rel, "%u\n", s->capacity[cpu]);
		snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpu%d/power/pm_qos_resume_latency_us", cpu);
		put(a, rel, "0\n");
		for (int k = 0; k < p->nidle; k++) {
			const struct sim_idle_desc *d = &p->idle[k];
			char base[128];

			snprintf(base, sizeof(base), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d", cpu, k);
			snprintf(rel, sizeof(rel), "%s/name", base);
			put(a, rel, "%s\n", d->name);
			snprintf(rel, sizeof(rel), "%s/desc", base);
			put(a, rel, "%s\n", d->desc);
			snprintf(rel, sizeof(rel), "%s/latency", base);
			put(a, rel, "%u\n", d->latency_us);
			snprintf(rel, sizeof(rel), "%s/residency", base);
			put(a, rel, "%u\n", d->residency_us);
			snprintf(rel, sizeof(rel), "%s/disable", base);
			put(a, rel, "0\n");
			snprintf(rel, sizeof(rel), "%s/time", base);
			put(a, rel, "0\n");
			snprintf(rel, sizeof(rel), "%s/usage", base);
			put(a, rel, "0\n");
			idx_itime[cpu][k] = dyn(a, "%s/time", base);
			idx_iuse[cpu][k] = dyn(a, "%s/usage", base);
		}
	}

	for (int k = 0; k < p->nclusters; k++) {
		const struct sim_cluster_desc *c = &p->clusters[k];
		char base[128], rel[192], cpus[128] = "", freqs[512] = "";
		int f = c->first_cpu;

		for (int i = 0; i < c->ncpus; i++)
			snprintf(cpus + strlen(cpus), sizeof(cpus) - strlen(cpus), "%s%d",
				 i ? " " : "", f + i);
		for (int i = 0; i < c->nopps; i++)
			snprintf(freqs + strlen(freqs), sizeof(freqs) - strlen(freqs), "%u ",
				 c->opps[i].khz);
		snprintf(base, sizeof(base), "/sys/devices/system/cpu/cpufreq/policy%d", f);
#define PUTP(name, ...) do { snprintf(rel, sizeof(rel), "%s/%s", base, name); put(a, rel, __VA_ARGS__); } while (0)
		PUTP("affected_cpus", "%s\n", cpus);
		PUTP("related_cpus", "%s\n", cpus);
		PUTP("cpuinfo_min_freq", "%u\n", c->opps[0].khz);
		PUTP("cpuinfo_max_freq", "%u\n", c->opps[c->nopps - 1].khz);
		PUTP("cpuinfo_transition_latency", "%u\n", p->transition_latency_ns);
		PUTP("scaling_available_frequencies", "%s\n", freqs);
		PUTP("scaling_available_governors", "schedutil performance powersave\n");
		PUTP("scaling_governor", "schedutil\n");
		PUTP("scaling_driver", "cpufreq-dt\n");
		PUTP("scaling_min_freq", "%u\n", c->opps[0].khz);
		PUTP("scaling_max_freq", "%u\n", c->opps[c->nopps - 1].khz);
		PUTP("scaling_cur_freq", "%u\n", c->opps[0].khz);
		PUTP("stats/total_trans", "0\n");
		PUTP("stats/time_in_state", "\n");
#undef PUTP
		idx_cur[k] = dyn(a, "%s/scaling_cur_freq", base);
		idx_tis[k] = dyn(a, "%s/stats/time_in_state", base);
		idx_trans[k] = dyn(a, "%s/stats/total_trans", base);
		for (int cpu = f; cpu < f + c->ncpus; cpu++) {
			snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpu%d/cpufreq", cpu);
			absolute(a, path, sizeof(path), rel);
			unlink(path);
			snprintf(buf, sizeof(buf), "../cpufreq/policy%d", f);
			if (symlink(buf, path) && errno != EEXIST)
				fprintf(stderr, "lerwatt-sim: symlink %s: %s\n", path, strerror(errno));
		}

		/* Energy Model, as debugfs shows it (DT-based EM => microwatts). */
		snprintf(base, sizeof(base), "/sys/kernel/debug/energy_model/cpu%d", f);
		snprintf(rel, sizeof(rel), "%s/cpus", base);
		put(a, rel, "%d-%d\n", f, f + c->ncpus - 1);
		snprintf(rel, sizeof(rel), "%s/flags", base);
		put(a, rel, "0x1\n");
		{
			uint32_t prev_cost = UINT32_MAX;
			bool ineff[SIM_MAX_OPPS] = { false };

			for (int i = c->nopps - 1; i >= 0; i--) {
				uint32_t cost = sim_em_cost(p, k, i);

				if (cost >= prev_cost)
					ineff[i] = true;
				else
					prev_cost = cost;
			}
			for (int i = 0; i < c->nopps; i++) {
				char ps[160];

				snprintf(ps, sizeof(ps), "%s/ps:%u", base, c->opps[i].khz);
				snprintf(rel, sizeof(rel), "%s/frequency", ps);
				put(a, rel, "%u\n", c->opps[i].khz);
				snprintf(rel, sizeof(rel), "%s/power", ps);
				put(a, rel, "%u\n", sim_em_power_uw(c, i));
				snprintf(rel, sizeof(rel), "%s/cost", ps);
				put(a, rel, "%u\n", sim_em_cost(p, k, i));
				snprintf(rel, sizeof(rel), "%s/performance", ps);
				put(a, rel, "%u\n", sim_em_perf(p, k, i));
				snprintf(rel, sizeof(rel), "%s/inefficient", ps);
				put(a, rel, "%d\n", ineff[i] ? 1 : 0);
			}
		}
	}

	for (int z = 0; z < p->nzones; z++) {
		const struct sim_zone_desc *zd = &p->zones[z];
		char base[128], rel[192];
		int t = 0;

		snprintf(base, sizeof(base), "/sys/class/thermal/thermal_zone%d", z);
		snprintf(rel, sizeof(rel), "%s/type", base);
		put(a, rel, "%s\n", zd->type);
		snprintf(rel, sizeof(rel), "%s/mode", base);
		put(a, rel, "enabled\n");
		snprintf(rel, sizeof(rel), "%s/policy", base);
		put(a, rel, "step_wise\n");
		if (zd->passive) {
			snprintf(rel, sizeof(rel), "%s/trip_point_%d_type", base, t);
			put(a, rel, "passive\n");
			snprintf(rel, sizeof(rel), "%s/trip_point_%d_temp", base, t);
			put(a, rel, "%d\n", p->passive_mc);
			snprintf(rel, sizeof(rel), "%s/trip_point_%d_hyst", base, t);
			put(a, rel, "%d\n", p->passive_hyst_mc);
			t++;
		}
		snprintf(rel, sizeof(rel), "%s/trip_point_%d_type", base, t);
		put(a, rel, "critical\n");
		snprintf(rel, sizeof(rel), "%s/trip_point_%d_temp", base, t);
		put(a, rel, "%d\n", p->critical_mc);
		snprintf(rel, sizeof(rel), "%s/trip_point_%d_hyst", base, t);
		put(a, rel, "0\n");
		snprintf(rel, sizeof(rel), "%s/temp", base);
		put(a, rel, "%d\n", s->zone[z].sensor_mc);
		idx_zone[z] = dyn(a, "%s/temp", base);

		if (zd->passive && zd->cluster >= 0) {
			const struct sim_cluster_desc *c = &p->clusters[zd->cluster];
			char cd[128], target[64];

			snprintf(cd, sizeof(cd), "/sys/class/thermal/cooling_device%d", cdev);
			snprintf(rel, sizeof(rel), "%s/type", cd);
			put(a, rel, "cpufreq-cpu%d\n", c->first_cpu);
			snprintf(rel, sizeof(rel), "%s/max_state", cd);
			put(a, rel, "%d\n", c->nopps - 1);
			snprintf(rel, sizeof(rel), "%s/cur_state", cd);
			put(a, rel, "0\n");
			idx_cdev[zd->cluster] = dyn(a, "%s/cur_state", cd);
			snprintf(rel, sizeof(rel), "%s/cdev0", base);
			absolute(a, path, sizeof(path), rel);
			snprintf(target, sizeof(target), "../cooling_device%d", cdev);
			unlink(path);
			if (symlink(target, path) && errno != EEXIST)
				fprintf(stderr, "lerwatt-sim: symlink %s: %s\n", path, strerror(errno));
			cdev++;
		}
	}

	put(a, "/sys/class/hwmon/hwmon0/name", "ina226\n");
	put(a, "/sys/class/hwmon/hwmon0/update_interval", "10\n");
	put(a, "/sys/class/hwmon/hwmon0/power1_input", "0\n");
	put(a, "/proc/stat", "\n");

	/* Marker last: lerwattd attaches only to a complete tree. */
	snprintf(buf, sizeof(buf), "platform=%s version=1 pid=%d\n", p->name, (int)getpid());
	put(a, "/" ".lerwatt-sim", "%s", buf);
	return 0;
}

static bool read_rel(const struct sim_abi *a, const char *rel, char *buf, size_t len)
{
	char path[600];
	ssize_t n;
	int fd;

	absolute(a, path, sizeof(path), rel);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return false;	/* empty while a writer truncates: keep the old value */
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return n > 0;
}

static bool parse_u32(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end || v > UINT32_MAX)
		return false;
	*out = (uint32_t)v;
	return true;
}

void sim_abi_ingest(struct sim_abi *a, struct sim_state *s)
{
	const struct sim_platform *p = s->plat;
	char rel[192], buf[64];
	uint32_t v;

	for (int k = 0; k < p->nclusters; k++) {
		int f = p->clusters[k].first_cpu;

		snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", f);
		if (read_rel(a, rel, buf, sizeof(buf))) {
			if (parse_u32(buf, &v))
				s->cl[k].user_max_khz = v;
			else
				a->bad_writes++;
		}
		snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", f);
		if (read_rel(a, rel, buf, sizeof(buf))) {
			if (parse_u32(buf, &v))
				s->cl[k].user_min_khz = v;
			else
				a->bad_writes++;
		}
	}
	for (int cpu = 0; cpu < p->ncpus; cpu++) {
		snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpu%d/power/pm_qos_resume_latency_us", cpu);
		if (read_rel(a, rel, buf, sizeof(buf))) {
			/* drivers/base/power/sysfs.c: "0" = no constraint, "n/a" = 0 us. */
			if (!strcmp(buf, "n/a"))
				s->cpu[cpu].resume_latency_us = 0;
			else if (parse_u32(buf, &v) && v <= INT32_MAX - 1)
				s->cpu[cpu].resume_latency_us = v ? (int32_t)v : -1;
			else
				a->bad_writes++;
		}
		for (int k = 1; k < p->nidle; k++) {
			snprintf(rel, sizeof(rel), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable", cpu, k);
			if (read_rel(a, rel, buf, sizeof(buf)) && parse_u32(buf, &v))
				s->cpu[cpu].idle_disabled[k] = v != 0;
		}
	}
}

void sim_abi_publish(struct sim_abi *a, const struct sim_state *s)
{
	const struct sim_platform *p = s->plat;
	char buf[1024];
	size_t n;

	for (int k = 0; k < p->nclusters; k++) {
		const struct sim_cluster_desc *c = &p->clusters[k];
		const struct sim_cluster *cl = &s->cl[k];

		snprintf(buf, sizeof(buf), "%u\n", c->opps[cl->cur_idx].khz);
		set_dyn(a, idx_cur[k], buf);
		n = 0;
		for (int i = 0; i < c->nopps; i++)
			n += snprintf(buf + n, sizeof(buf) - n, "%u %llu\n", c->opps[i].khz,
				      (unsigned long long)(cl->time_in_state_s[i] * USER_HZ));
		set_dyn(a, idx_tis[k], buf);
		snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)cl->total_trans);
		set_dyn(a, idx_trans[k], buf);
		snprintf(buf, sizeof(buf), "%d\n", cl->thermal_state);
		set_dyn(a, idx_cdev[k], buf);
	}
	for (int z = 0; z < p->nzones; z++) {
		snprintf(buf, sizeof(buf), "%d\n", s->zone[z].sensor_mc);
		set_dyn(a, idx_zone[z], buf);
	}
	snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)(s->meter_w * 1e6));
	set_dyn(a, DYN_METER, buf);

	{
		double ub = 0, ui = 0;

		for (int cpu = 0; cpu < p->ncpus; cpu++) {
			ub += s->cpu[cpu].stat_busy_s;
			ui += s->cpu[cpu].stat_idle_s;
		}
		n = (size_t)snprintf(buf, sizeof(buf), "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n",
				     (unsigned long long)(ub * USER_HZ),
				     (unsigned long long)(ui * USER_HZ));
		for (int cpu = 0; cpu < p->ncpus; cpu++)
			n += snprintf(buf + n, sizeof(buf) - n, "cpu%d %llu 0 0 %llu 0 0 0 0 0 0\n", cpu,
				      (unsigned long long)(s->cpu[cpu].stat_busy_s * USER_HZ),
				      (unsigned long long)(s->cpu[cpu].stat_idle_s * USER_HZ));
		set_dyn(a, DYN_STAT, buf);
	}
	for (int cpu = 0; cpu < p->ncpus; cpu++)
		for (int k = 0; k < p->nidle; k++) {
			snprintf(buf, sizeof(buf), "%llu\n",
				 (unsigned long long)s->cpu[cpu].idle_time_us[k]);
			set_dyn(a, idx_itime[cpu][k], buf);
			snprintf(buf, sizeof(buf), "%llu\n",
				 (unsigned long long)s->cpu[cpu].idle_usage[k]);
			set_dyn(a, idx_iuse[cpu][k], buf);
		}
}

void sim_abi_close(struct sim_abi *a)
{
	char path[600];

	absolute(a, path, sizeof(path), "/" ".lerwatt-sim");
	unlink(path);
}
