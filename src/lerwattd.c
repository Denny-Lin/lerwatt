// SPDX-License-Identifier: MIT
/*
 * lerwattd: the Lerwatt power controller.
 *
 * It finds its hardware by itself: real cpufreq on this machine wins, a
 * running lerwatt-sim is used otherwise. Nothing below this file knows which
 * one it got.
 */
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "contract.h"
#include "lerwatt/controller.h"
#include "lerwatt/hal.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: lerwattd [options]\n"
		"  --sysroot DIR      use DIR instead of auto-detection (\"/\" = this machine)\n"
		"  --period-ms N      control period, default 100\n"
		"  --duration S       stop after S seconds (default: until SIGINT/SIGTERM)\n"
		"  --soft-limit C     soft thermal limit in degC (default: lowest passive trip - 5)\n"
		"  --telemetry FILE   append one JSON line per period (\"-\" = stdout)\n"
		"  --dry-run          decide and log, never write kernel knobs\n"
		"  --no-contract      do not use the Lertick contract maps\n"
		"  --describe         print the discovered system and exit\n"
		"  --restore          write back journaled original values and exit\n");
}

static int32_t lowest_passive(const struct lw_system *sys)
{
	int32_t lo = INT32_MAX;

	for (int z = 0; z < sys->nzones; z++)
		if (sys->zones[z].passive_mc < lo)
			lo = sys->zones[z].passive_mc;
	return lo;
}

static int32_t cluster_temp(const struct lw_system *sys, const struct lw_sample *s, int i)
{
	int32_t t = INT32_MIN;
	int z = sys->policies[i].zone;

	if (z >= 0)
		return s->zone_mc[z];
	for (z = 0; z < sys->nzones; z++)	/* unmapped: follow the hottest zone */
		if (s->zone_mc[z] > t)
			t = s->zone_mc[z];
	return t;
}

static void sleep_until(uint64_t deadline_ns)
{
	struct timespec ts = {
		.tv_sec = (time_t)(deadline_ns / 1000000000ull),
		.tv_nsec = (long)(deadline_ns % 1000000000ull),
	};

	while (!stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "sysroot", required_argument, NULL, 'r' },
		{ "period-ms", required_argument, NULL, 'p' },
		{ "duration", required_argument, NULL, 'd' },
		{ "soft-limit", required_argument, NULL, 's' },
		{ "telemetry", required_argument, NULL, 't' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "no-contract", no_argument, NULL, 'c' },
		{ "describe", no_argument, NULL, 'D' },
		{ "restore", no_argument, NULL, 'R' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	const char *sysroot = NULL, *telemetry = NULL;
	unsigned int period_ms = 100;
	double duration_s = 0, soft_c = 0;
	bool dry_run = false, use_contract = true, describe = false, restore = false;
	static struct lw_system sys;
	struct lw_ctl_state st[LW_MAX_POLICIES] = { 0 };
	struct lw_ctl_params params;
	struct lw_contract contract;
	struct lw_actuator act;
	struct lw_cpustat hist = { 0 };
	struct sigaction sa = { 0 };
	char err[256];
	FILE *tel = NULL;
	uint64_t start_ns, next_ns, last_ns;
	int opt, ret = 0;
	bool have_contract = false;

	while ((opt = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (opt) {
		case 'r': sysroot = optarg; break;
		case 'p': period_ms = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'd': duration_s = strtod(optarg, NULL); break;
		case 's': soft_c = strtod(optarg, NULL); break;
		case 't': telemetry = optarg; break;
		case 'n': dry_run = true; break;
		case 'c': use_contract = false; break;
		case 'D': describe = true; break;
		case 'R': restore = true; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 1;
		}
	}
	if (period_ms < 10 || period_ms > 10000) {
		fprintf(stderr, "lerwatt: --period-ms must be 10..10000\n");
		return 1;
	}

	if (lw_detect(&sys, sysroot, err, sizeof(err))) {
		fprintf(stderr, "lerwatt: %s\n", err);
		return 1;
	}
	if (restore) {
		ret = lw_journal_restore(&sys);
		fprintf(stderr, "lerwatt: restore %s\n", ret ? "had failures" : "done");
		return ret ? 2 : 0;
	}
	if (lw_discover(&sys, err, sizeof(err))) {
		fprintf(stderr, "lerwatt: %s\n", err);
		return 1;
	}
	lw_describe(&sys, stderr);
	if (describe)
		return 0;

	/* A journal left behind means the last run died: undo it first. */
	if (!dry_run && lw_journal_restore(&sys))
		fprintf(stderr, "lerwatt: warning: stale journal could not be fully restored\n");

	lw_ctl_default_params(&params, lowest_passive(&sys));
	if (soft_c > 0)
		params.soft_limit_mc = (int32_t)(soft_c * 1000.0);
	fprintf(stderr, "lerwatt: soft limit %.1f degC, period %u ms%s\n",
		params.soft_limit_mc / 1000.0, period_ms, dry_run ? ", dry run" : "");

	if (lw_actuator_init(&sys, &act, dry_run)) {
		fprintf(stderr, "lerwatt: cannot create the restore journal\n");
		return 1;
	}
	if (use_contract) {
		int c = lw_contract_open(&contract, true);

		have_contract = c == 0;
		fprintf(stderr, "lerwatt: Lertick contract %s\n",
			have_contract ? "attached" : c == -ENOENT ? "absent (running alone)" :
			strerror(-c));
	}
	if (telemetry)
		tel = strcmp(telemetry, "-") ? fopen(telemetry, "a") : stdout;

	sa.sa_handler = on_signal;	/* no SA_RESTART: wake the sleep */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	start_ns = last_ns = lw_now_ns();
	next_ns = start_ns;
	while (!stop) {
		struct lw_sample s;
		struct lw_actuation a = { 0 };
		double dt;

		lw_sample(&sys, &hist, &s);
		dt = (s.t_ns - last_ns) / 1e9;
		last_ns = s.t_ns;
		if (tel)
			fprintf(tel, "{\"t\":%.3f,\"mode\":\"%s\",\"power_w\":%.3f,\"measured\":%s,\"clusters\":[",
				(s.t_ns - start_ns) / 1e9, sys.simulated ? "sim" : "hw", s.power_w,
				s.power_measured ? "true" : "false");

		for (int i = 0; i < sys.npolicies; i++) {
			const struct lw_policy *p = &sys.policies[i];
			struct lt_demand_rec d;
			struct lw_ctl_in in = {
				.temp_mc = cluster_temp(&sys, &s, i),
				.busy_mean = s.busy_mean[i],
				.cur_khz = s.cur_khz[i],
				.ncpus = p->ncpus,
				.opps = p->opps,
				.nopps = p->nopps,
				.capacity = p->capacity,
				.deep_idle_latency_us = p->nidle >= 2 ? (int32_t)p->idle[p->nidle - 1].latency_us : -1,
				.lat_crit = -1,
			};
			struct lw_ctl_out o;

			if (have_contract &&
			    lw_contract_demand(&contract, (unsigned int)i, s.t_ns,
					       5ull * period_ms * 1000000ull, &d))
				in.lat_crit = d.lat_crit_milli / 1000.0;

			lw_ctl_step(&params, &in, &st[i], &o, s.t_ns, dt > 0 ? dt : period_ms / 1000.0);

			a.max_khz[i] = o.ceiling_khz < p->opps[p->nopps - 1].khz ? o.ceiling_khz : 0;
			a.min_khz[i] = o.floor_khz;
			a.resume_latency_us[i] = o.resume_latency_us;

			if (have_contract) {
				struct lw_contract_rec r = {
					.ceiling_cap = o.ceiling_cap,
					.floor_cap = o.floor_cap,
					.energy_price_milli = o.price_milli,
					.thermal_headroom_mc = o.headroom_mc,
					.valid_until_ns = s.t_ns + 3ull * period_ms * 1000000ull,
				};

				lw_contract_publish(&contract, (unsigned int)i, &r);
			}
			if (tel)
				fprintf(tel, "%s{\"id\":%d,\"temp_c\":%.1f,\"busy\":%.3f,\"cur_mhz\":%u,"
					"\"ceil_mhz\":%u,\"budget_w\":%.3f,\"est_w\":%.3f,\"price\":%u,"
					"\"floor_mhz\":%u,\"em\":%s}",
					i ? "," : "", i, in.temp_mc == INT32_MIN ? -1.0 : in.temp_mc / 1000.0,
					in.busy_mean, in.cur_khz / 1000, o.ceiling_khz / 1000, o.budget_w,
					o.est_power_w, o.price_milli, o.floor_khz / 1000,
					p->have_em ? "true" : "false");
		}
		if (tel) {
			fprintf(tel, "]}\n");
			fflush(tel);
		}
		if (lw_actuate(&sys, &act, &a))
			ret = 2;

		if (duration_s > 0 && (lw_now_ns() - start_ns) / 1e9 >= duration_s)
			break;
		next_ns += (uint64_t)period_ms * 1000000ull;
		if (next_ns < lw_now_ns())	/* overran: skip, do not burst */
			next_ns = lw_now_ns() + (uint64_t)period_ms * 1000000ull;
		sleep_until(next_ns);
	}

	if (have_contract)
		lw_contract_close(&contract);
	if (!dry_run && lw_journal_restore(&sys)) {
		fprintf(stderr, "lerwatt: restore had failures; run lerwattd --restore\n");
		ret = 2;
	}
	fprintf(stderr, "lerwatt: stopped after %.1f s, %lu writes, %lu write errors\n",
		(lw_now_ns() - start_ns) / 1e9, act.writes, act.write_errors);
	if (tel && tel != stdout)
		fclose(tel);
	return ret;
}
