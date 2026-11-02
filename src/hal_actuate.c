// SPDX-License-Identifier: MIT
/*
 * Actuation: the only code that writes kernel knobs.
 *
 * Allowed writes (docs/DESIGN.md section 5): scaling_max_freq,
 * scaling_min_freq and power/pm_qos_resume_latency_us. Every target is
 * journaled before its first write, values are clamped to the hardware
 * range, a knob is written only when the decision changes, and the original
 * maximum is never raised. A lower ceiling can lower the original floor
 * to keep min <= max (see struct lw_actuator).
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lerwatt/hal.h"

static void policy_path(const struct lw_system *sys, char *out, size_t len, int id,
			const char *attr)
{
	lw_path(sys, out, len, "/sys/devices/system/cpu/cpufreq/policy%d/%s", id, attr);
}

static void latency_path(const struct lw_system *sys, char *out, size_t len, int cpu)
{
	lw_path(sys, out, len, "/sys/devices/system/cpu/cpu%d/power/pm_qos_resume_latency_us",
		cpu);
}

int lw_actuator_init(const struct lw_system *sys, struct lw_actuator *a, bool dry_run)
{
	char path[LW_PATH_MAX];

	memset(a, 0, sizeof(*a));
	a->dry_run = dry_run;
	if (sys->npolicies < 1 || sys->npolicies > LW_MAX_POLICIES)
		return -EINVAL;
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];
		uint64_t v;
		int ret;

		if (p->nopps < 1 || p->nopps > LW_MAX_OPPS ||
		    p->ncpus < 1 || p->ncpus > LW_MAX_CPUS)
			return -EINVAL;

		policy_path(sys, path, sizeof(path), p->id, "scaling_max_freq");
		ret = lw_read_u64(path, &v);
		if (ret || !v || v > UINT32_MAX)
			return ret ? ret : -EINVAL;
		a->orig_max[i] = (uint32_t)v;
		policy_path(sys, path, sizeof(path), p->id, "scaling_min_freq");
		ret = lw_read_u64(path, &v);
		if (ret || !v || v > UINT32_MAX || v > a->orig_max[i])
			return ret ? ret : -EINVAL;
		a->orig_min[i] = (uint32_t)v;
		a->last_max[i] = a->orig_max[i];
		a->last_min[i] = a->orig_min[i];
		if (a->orig_max[i] < p->cpuinfo_max_khz)
			fprintf(stderr,
				"lerwatt: policy%d starts capped at %u kHz (user limit or thermal "
				"throttling); Lerwatt will not exceed it\n", p->id, a->orig_max[i]);
		for (int c = 0; c < p->ncpus; c++) {
			int cpu = p->cpus[c];

			if (cpu < 0 || cpu >= LW_MAX_CPUS)
				return -EINVAL;
			latency_path(sys, path, sizeof(path), cpu);
			ret = lw_read_str(path, a->orig_lat[cpu], sizeof(a->orig_lat[cpu]));
			if (ret == -ENOENT)
				a->orig_lat[cpu][0] = '\0';	/* knob absent */
			else if (ret)
				return ret;
			else if (strcmp(a->orig_lat[cpu], "n/a")) {
				char *end;
				unsigned long latency;

				errno = 0;
				latency = strtoul(a->orig_lat[cpu], &end, 10);
				if (errno || end == a->orig_lat[cpu] || *end ||
				    a->orig_lat[cpu][0] == '-' || latency > INT32_MAX)
					return -EINVAL;
			}
			memcpy(a->last_lat[cpu], a->orig_lat[cpu], sizeof(a->last_lat[cpu]));
		}
	}
	return dry_run ? 0 : lw_journal_open(sys);
}

static int write_knob(const struct lw_system *sys, struct lw_actuator *a,
		      const char *path, const char *val)
{
	int ret;

	if (a->dry_run)
		return 0;
	ret = lw_journal_save(sys, path);
	if (ret) {
		fprintf(stderr, "lerwatt: cannot journal %s (%s); not writing it\n", path,
			strerror(-ret));
		a->write_errors++;
		return ret;
	}
	ret = lw_write_str(path, val);
	if (ret) {
		fprintf(stderr, "lerwatt: write %s <- %s failed: %s\n", path, val, strerror(-ret));
		a->write_errors++;
		return ret;
	}
	a->writes++;
	return 0;
}

static uint32_t clamp_khz(const struct lw_policy *p, uint32_t khz)
{
	uint32_t lo = p->cpuinfo_min_khz ? p->cpuinfo_min_khz : p->opps[0].khz;
	uint32_t hi = p->cpuinfo_max_khz ? p->cpuinfo_max_khz : p->opps[p->nopps - 1].khz;

	return khz < lo ? lo : khz > hi ? hi : khz;
}

int lw_actuate(const struct lw_system *sys, struct lw_actuator *a, const struct lw_actuation *act)
{
	char path[LW_PATH_MAX], val[16];
	int ret;

	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *p = &sys->policies[i];
		uint32_t mx = a->orig_max[i], mn = a->orig_min[i];

		if (act->max_khz[i] && clamp_khz(p, act->max_khz[i]) < mx)
			mx = clamp_khz(p, act->max_khz[i]);
		if (act->min_khz[i] && clamp_khz(p, act->min_khz[i]) > mn)
			mn = clamp_khz(p, act->min_khz[i]);
		if (mn > mx)
			mn = mx;

		/* Lower an earlier floor before lowering the ceiling beneath it. */
		if (mn < a->last_min[i]) {
			policy_path(sys, path, sizeof(path), p->id, "scaling_min_freq");
			snprintf(val, sizeof(val), "%u", mn);
			ret = write_knob(sys, a, path, val);
			if (ret)
				return ret;
			a->last_min[i] = mn;
		}
		if (mx != a->last_max[i]) {
			policy_path(sys, path, sizeof(path), p->id, "scaling_max_freq");
			snprintf(val, sizeof(val), "%u", mx);
			ret = write_knob(sys, a, path, val);
			if (ret)
				return ret;
			a->last_max[i] = mx;
		}
		if (mn != a->last_min[i]) {
			policy_path(sys, path, sizeof(path), p->id, "scaling_min_freq");
			snprintf(val, sizeof(val), "%u", mn);
			ret = write_knob(sys, a, path, val);
			if (ret)
				return ret;
			a->last_min[i] = mn;
		}
		for (int c = 0; c < p->ncpus; c++) {
			int cpu = p->cpus[c];
			int32_t lat = act->resume_latency_us[i];

			if (!a->orig_lat[cpu][0])
				continue;	/* no PM QoS knob for this CPU */
			/* Intersect with the existing per-CPU constraint: kernel text
			 * "n/a" is zero latency (strongest), whereas "0" is unbounded.
			 * The original text was validated by lw_actuator_init().
			 */
			if (lat >= 0) {
				if (!strcmp(a->orig_lat[cpu], "n/a")) {
					lat = 0;
				} else {
					unsigned long original = strtoul(a->orig_lat[cpu], NULL, 10);

					if (original && lat > (int32_t)original)
						lat = (int32_t)original;
				}
			}
			/* Kernel ABI: "0" = no constraint, "n/a" = zero latency. */
			if (lat < 0)
				snprintf(val, sizeof(val), "%s", a->orig_lat[cpu]);
			else if (lat == 0)
				snprintf(val, sizeof(val), "n/a");
			else
				snprintf(val, sizeof(val), "%d", lat);
			if (!strcmp(val, a->last_lat[cpu]))
				continue;
			latency_path(sys, path, sizeof(path), cpu);
			ret = write_knob(sys, a, path, val);
			if (ret)
				return ret;
			snprintf(a->last_lat[cpu], sizeof(a->last_lat[cpu]), "%s", val);
		}
	}
	return 0;
}
