// SPDX-License-Identifier: MIT
/*
 * lerwattd: the Lerwatt power controller.
 *
 * It finds its hardware by itself: real cpufreq on this machine wins, a
 * running lerwatt-sim is used otherwise. Nothing below this file knows which
 * one it got.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include "lerwatt/controller.h"
#include "lerwatt/hal.h"
#include "runtime.h"

#ifndef LERWATT_VERSION
#define LERWATT_VERSION "source"
#endif

/* Lertick runs are finite: look for a new run every second, drop a silent one after 2 s. */
#define CONTRACT_RETRY_NS 1000000000ull
#define CONTRACT_STALE_NS 2000000000ull

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
		"  --telemetry FILE   create a new private JSON-lines file (\"-\" = stdout)\n"
		"  --dry-run          decide and log, never write kernel knobs\n"
		"  --no-contract      do not use the Lertick contract maps\n"
		"  --policy rule|mpc  rule: PI budget (default); mpc: experimental learned\n"
		"                     thermal model with predictive ceilings\n"
		"  --cross-cluster    jointly plan MPC ceilings (requires --policy mpc)\n"
		"  --forecast temporal|adaptive  point forecast (default temporal; MPC only)\n"
		"  --describe         print the discovered system and exit\n"
		"  --check            check hardware capabilities without changing settings\n"
		"  --temperature-sensor PATH  explicitly select a CPU/package sensor (millidegC)\n"
		"  --version          print build version and exit\n"
		"  --restore          write back journaled original values and exit\n"
		"                     optional: -- /absolute/program args (hold recovery lock)\n");
}

/* Never append privileged telemetry to an existing file, symlink or device.
 * O_EXCL also prevents a link replacement between checking and opening. */
static FILE *open_telemetry(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
		      O_NONBLOCK | O_CLOEXEC, 0600);
	FILE *out;

	if (fd < 0)
		return NULL;
	out = fdopen(fd, "w");
	if (!out) {
		int saved = errno;

		close(fd);
		errno = saved;
	}
	return out;
}

/* Keep the same kernel lock across recovery and installation/removal. The
 * child inherits it too, so killing only this supervisor cannot open a gap. */
static int run_locked_command(int lock_fd, char **args)
{
	struct sigaction ignore = { .sa_handler = SIG_IGN };
	struct sigaction normal = { .sa_handler = SIG_DFL };
	pid_t child;
	int status;

	if (fcntl(lock_fd, F_SETFD, 0) < 0)
		return 2;
	sigaction(SIGINT, &ignore, NULL);
	sigaction(SIGTERM, &ignore, NULL);
	sigaction(SIGHUP, &ignore, NULL);
	child = fork();
	if (child < 0)
		return 2;
	if (!child) {
		sigaction(SIGINT, &normal, NULL);
		sigaction(SIGTERM, &normal, NULL);
		sigaction(SIGHUP, &normal, NULL);
		execv(args[0], args);
		fprintf(stderr, "lerwatt: cannot execute maintenance command: %s\n", strerror(errno));
		_exit(127);
	}
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR)
			return 2;
	return WIFEXITED(status) ? WEXITSTATUS(status) :
		WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 2;
}

static bool parse_period(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	if (!s[0] || s[0] == '-' || s[0] == '+')
		return false;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v < 10 || v > 10000)
		return false;
	*out = (unsigned int)v;
	return true;
}

static bool parse_number(const char *s, double low, double high, double *out)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (errno || end == s || *end || !isfinite(v) || v < low || v > high)
		return false;
	*out = v;
	return true;
}

static int32_t lowest_passive(const struct lw_system *sys)
{
	int32_t lo = INT32_MAX;

	for (int i = 0; i < sys->npolicies; i++) {
		int z = sys->policies[i].zone;

		if (z >= 0 && sys->zones[z].passive_mc < lo)
			lo = sys->zones[z].passive_mc;
	}
	if (sys->package_zone >= 0 && sys->zones[sys->package_zone].passive_mc < lo)
		lo = sys->zones[sys->package_zone].passive_mc;
	return lo;
}

static int32_t cluster_temp(const struct lw_system *sys, const struct lw_sample *s, int i)
{
	int32_t t = INT32_MIN;
	int z = sys->policies[i].zone;

	if (z >= 0)
		return s->zone_mc[z];
	/* An unrelated GPU/battery zone is not a CPU temperature fallback. */
	if (sys->package_zone >= 0)
		t = s->zone_mc[sys->package_zone];
	return t;
}

static int sleep_until(uint64_t deadline_ns)
{
	struct timespec ts = {
		.tv_sec = (time_t)(deadline_ns / 1000000000ull),
		.tv_nsec = (long)(deadline_ns % 1000000000ull),
	};

	while (!stop) {
		int error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

		if (error != EINTR)
			return error;
	}
	return 0;
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
		{ "policy", required_argument, NULL, 'P' },
		{ "cross-cluster", no_argument, NULL, 'G' },
		{ "forecast", required_argument, NULL, 'F' },
		{ "describe", no_argument, NULL, 'D' },
		{ "check", no_argument, NULL, 'C' },
		{ "temperature-sensor", required_argument, NULL, 'S' },
		{ "version", no_argument, NULL, 'V' },
		{ "restore", no_argument, NULL, 'R' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	const char *sysroot = NULL, *telemetry = NULL, *sensor_path = NULL;
	unsigned int period_ms = 100;
	double duration_s = 0, soft_c = 0;
	bool dry_run = false, use_contract = true, describe = false, restore = false;
	bool cross_cluster = false;
	bool check = false, actuator_ready = false, started = false;
	bool temporal_forecast = true;
	static struct lw_system sys;
	static struct lw_ctl_state st[LW_MAX_POLICIES];
	struct lw_ctl_params params;
	struct lw_contract contract;
	struct lw_actuator act;
	struct lw_cpustat hist = { 0 };
	struct sigaction sa = { 0 };
	char err[256];
	FILE *tel = NULL;
	uint64_t start_ns, next_ns, last_ns;
	uint64_t duration_deadline_ns = 0;
	int opt, ret = 0;
	int lock_fd = -1;
	bool have_contract = false;
	uint64_t contract_next_try = 0;
	enum lw_ctl_policy policy = LW_POLICY_RULE;
	double est_w[LW_MAX_POLICIES] = { 0 };

	while ((opt = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (opt) {
		case 'r': sysroot = optarg; break;
		case 'p':
			if (!parse_period(optarg, &period_ms)) {
				fprintf(stderr, "lerwatt: --period-ms must be 10..10000\n");
				return 1;
			}
			break;
		case 'd':
			if (!parse_number(optarg, 0, 31536000, &duration_s)) {
				fprintf(stderr, "lerwatt: --duration must be a finite number in 0..31536000\n");
				return 1;
			}
			break;
		case 's':
			if (!parse_number(optarg, 1, 200, &soft_c)) {
				fprintf(stderr, "lerwatt: --soft-limit must be a finite number in 1..200\n");
				return 1;
			}
			break;
		case 't': telemetry = optarg; break;
		case 'n': dry_run = true; break;
		case 'c': use_contract = false; break;
		case 'G': cross_cluster = true; break;
		case 'F':
			if (strcmp(optarg, "temporal") && strcmp(optarg, "adaptive")) {
				usage(stderr);
				return 1;
			}
			temporal_forecast = !strcmp(optarg, "temporal");
			break;
		case 'P':
			if (strcmp(optarg, "rule") && strcmp(optarg, "mpc")) {
				usage(stderr);
				return 1;
			}
			policy = strcmp(optarg, "mpc") ? LW_POLICY_RULE : LW_POLICY_MPC;
			break;
		case 'D': describe = true; break;
		case 'C': check = true; break;
		case 'S': sensor_path = optarg; break;
		case 'V': printf("lerwattd %s\n", LERWATT_VERSION); return 0;
		case 'R': restore = true; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 1;
		}
	}
	if ((optind != argc && (!restore || argv[optind][0] != '/')) ||
	    (describe + check + restore) > 1 || (restore && dry_run)) {
		fprintf(stderr, "lerwatt: unexpected arguments or incompatible inspection/restore options\n");
		usage(stderr);
		return 1;
	}
	if (period_ms < 10 || period_ms > 10000) {
		fprintf(stderr, "lerwatt: --period-ms must be 10..10000\n");
		return 1;
	}
	if (cross_cluster && policy != LW_POLICY_MPC) {
		fprintf(stderr, "lerwatt: --cross-cluster requires --policy mpc\n");
		return 1;
	}
	/* A preview never publishes control, even if Lertick maps already exist. */
	if (dry_run)
		use_contract = false;
	if (check && !sysroot)
		sysroot = "/";

	if (lw_detect(&sys, sysroot, err, sizeof(err))) {
		fprintf(stderr, "lerwatt: %s\n", err);
		return 1;
	}
	if (!dry_run && !describe && !check) {
		lock_fd = lw_instance_lock(&sys);
		if (lock_fd < 0) {
			fprintf(stderr, "lerwatt: cannot acquire controller/restore lock: %s\n",
				strerror(-lock_fd));
			return 2;
		}
	}
	if (restore) {
		ret = lw_journal_restore(&sys);
		fprintf(stderr, "lerwatt: restore %s\n", ret ? "had failures" : "done");
		if (!ret && optind < argc) {
			ret = run_locked_command(lock_fd, argv + optind);
			close(lock_fd);
			return ret;
		}
		close(lock_fd);
		return ret ? 2 : 0;
	}
	/* Never overwrite an unrecovered journal or capture its limits as originals. */
	if (!dry_run && !describe && !check && lw_journal_restore(&sys)) {
		fprintf(stderr, "lerwatt: previous settings could not be restored; refusing control\n");
		ret = 2;
		goto finish;
	}
	if (lw_discover(&sys, err, sizeof(err))) {
		fprintf(stderr, "lerwatt: %s\n", err);
		ret = 1;
		goto finish;
	}
	if (sensor_path && lw_select_temperature_sensor(&sys, sensor_path, err, sizeof(err))) {
		fprintf(stderr, "lerwatt: %s\n", err);
		ret = 1;
		goto finish;
	}
	lw_describe(&sys, stderr);
	if (describe)
		return 0;
	if (lw_check_system(&sys, !dry_run, stderr)) {
		ret = 1;
		goto finish;
	}
	if (check) {
		fprintf(stderr, "lerwatt: requested policy %s; thermal planning %s\n",
			policy == LW_POLICY_MPC ? "mpc" : "rule",
			policy == LW_POLICY_MPC ? (sys.package_zone >= 0 ?
			"available after online learning" : "unavailable: rule fallback (no package sensor)") : "rule control");
		return 0;
	}
	if (sys.npolicies > LW_CONTRACT_MAX_CLUSTERS && use_contract) {
		fprintf(stderr, "lerwatt: %d policies exceed the Lertick contract capacity; running standalone\n",
			sys.npolicies);
		use_contract = false;
	}

	lw_ctl_default_params(&params, lowest_passive(&sys));
	params.policy = policy;
	params.cross_cluster = cross_cluster;
	params.temporal_forecast = temporal_forecast;
	if (policy == LW_POLICY_MPC && sys.package_zone < 0)
		fprintf(stderr, "lerwatt: no package zone: --policy mpc stays on the rule policy\n");
	if (soft_c > 0)
		params.soft_limit_mc = (int32_t)(soft_c * 1000.0);
	fprintf(stderr, "lerwatt: soft limit %.1f degC, period %u ms%s\n",
		params.soft_limit_mc / 1000.0, period_ms, dry_run ? ", dry run" : "");

	if (lw_actuator_init(&sys, &act, dry_run)) {
		fprintf(stderr, "lerwatt: cannot create the restore journal\n");
		ret = 2;
		goto finish;
	}
	actuator_ready = true;
	memset(&contract, 0, sizeof(contract));
	if (use_contract) {
		int c = 0;

		have_contract = lw_contract_maintain(&contract, false, true, lw_now_ns(),
						     CONTRACT_RETRY_NS, CONTRACT_STALE_NS,
						     &contract_next_try, &c);
		fprintf(stderr, "lerwatt: Lertick contract %s\n",
			have_contract ? "attached" : c == -ENOENT ? "absent (running alone)" :
			strerror(-c));
	}
	if (telemetry) {
		tel = strcmp(telemetry, "-") ? open_telemetry(telemetry) : stdout;
		if (!tel) {
			fprintf(stderr, "lerwatt: cannot open telemetry %s: %s\n", telemetry, strerror(errno));
			ret = 2;
			goto finish;
		}
	}

	sa.sa_handler = on_signal;	/* no SA_RESTART: wake the sleep */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	start_ns = last_ns = lw_now_ns();
	if (duration_s > 0) {
		uint64_t duration_ns = (uint64_t)(duration_s * 1e9);

		duration_deadline_ns = duration_ns > UINT64_MAX - start_ns ?
			UINT64_MAX : start_ns + duration_ns;
	}
	started = true;
	next_ns = start_ns;
	while (!stop) {
		struct lw_sample s;
		struct lw_actuation a = { 0 };
		double dt;

		if (duration_s > 0 && lw_now_ns() >= duration_deadline_ns)
			break;
		if (lw_sample(&sys, &hist, &s)) {
			fprintf(stderr, "lerwatt: sampling failed; stopping and restoring settings\n");
			ret = 2;
			break;
		}
		bool temperatures_valid = true;
		for (int i = 0; i < sys.npolicies; i++)
			if (cluster_temp(&sys, &s, i) == INT32_MIN)
				temperatures_valid = false;
		if (!temperatures_valid) {
			fprintf(stderr, "lerwatt: CPU temperature became unavailable; stopping and restoring settings\n");
			ret = 2;
			break;
		}
		dt = (s.t_ns - last_ns) / 1e9;
		last_ns = s.t_ns;
		if (use_contract) {
			bool was = have_contract;

			have_contract = lw_contract_maintain(&contract, have_contract, true, s.t_ns,
							     CONTRACT_RETRY_NS, CONTRACT_STALE_NS,
							     &contract_next_try, NULL);
			if (have_contract != was)
				fprintf(stderr, "lerwatt: Lertick contract %s\n",
					have_contract ? "attached" : "lost (running alone)");
		}
		if (tel)
			fprintf(tel, "{\"t\":%.3f,\"t_ns\":%llu,\"mode\":\"%s\",\"power_w\":%.3f,\"measured\":%s,\"clusters\":[",
				(s.t_ns - start_ns) / 1e9, (unsigned long long)(s.t_ns - start_ns),
				sys.simulated ? "sim" : "hw", s.power_w,
				s.power_measured ? "true" : "false");

		struct lw_ctl_in inputs[LW_MAX_POLICIES];
		struct lw_ctl_out outputs[LW_MAX_POLICIES];
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
				.package_mc = sys.package_zone >= 0 ? s.zone_mc[sys.package_zone] : INT32_MIN,
			};

			for (int j = 0; j < sys.npolicies; j++)
				if (j != i)
					in.other_power_w += est_w[j];
			if (have_contract &&
			    lw_contract_demand(&contract, (unsigned int)i, s.t_ns,
					       5ull * period_ms * 1000000ull, &d))
				in.lat_crit = d.lat_crit_milli / 1000.0;

			inputs[i] = in;
		}
		lw_ctl_step_group(&params, inputs, st, outputs, sys.npolicies,
				  s.t_ns, dt > 0 ? dt : period_ms / 1000.0);
		for (int i = 0; i < sys.npolicies; i++) {
			const struct lw_policy *p = &sys.policies[i];
			const struct lw_ctl_in in = inputs[i];
			const struct lw_ctl_out o = outputs[i];

			est_w[i] = o.est_power_w;
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
				fprintf(tel, "%s{\"id\":%d,\"temp_c\":%.3f,\"busy\":%.3f,\"cur_mhz\":%u,"
					"\"ceil_mhz\":%u,\"budget_w\":%.3f,\"est_w\":%.3f,\"price\":%u,"
					"\"floor_mhz\":%u,\"em\":%s,\"model\":%s,\"joint\":%s,\"pred_c\":%.1f,\"forecast_c\":%.3f,"
					"\"forecast_source\":\"%s\",\"forecast_labels\":%llu,\"forecast_dropped\":%llu,\"forecast_weights\":[%.5f,%.5f,%.5f],"
					"\"forecast_base_c\":%.3f,\"forecast_correction_c\":%.3f,\"temporal_labels\":%llu,\"temporal_dropped\":%llu}",
					i ? "," : "", i, in.temp_mc == INT32_MIN ? -1.0 : in.temp_mc / 1000.0,
					in.busy_mean, in.cur_khz / 1000, o.ceiling_khz / 1000, o.budget_w,
					o.est_power_w, o.price_milli, o.floor_khz / 1000,
					p->have_em ? "true" : "false", o.model_ready ? "true" : "false",
					o.joint_active ? "true" : "false",
					isnan(o.predicted_c) ? -1.0 : o.predicted_c,
					isnan(o.forecast_c) ? -1.0 : o.forecast_c,
					o.forecast_source == LW_FORECAST_PERSISTENCE ? "persistence" :
					o.forecast_source == LW_FORECAST_CONSTANT ? "constant" :
					o.forecast_source == LW_FORECAST_CAUSAL ? "causal" :
					o.forecast_source == LW_FORECAST_BLEND ? "blend" :
					o.forecast_source == LW_FORECAST_TEMPORAL ? "temporal" : "unavailable",
					(unsigned long long)o.forecast_labels,
					(unsigned long long)o.forecast_dropped,
					o.forecast_weights[0], o.forecast_weights[1], o.forecast_weights[2],
					isnan(o.forecast_base_c) ? -1.0 : o.forecast_base_c,
					o.forecast_correction_c, (unsigned long long)o.temporal_labels,
					(unsigned long long)o.temporal_dropped);
		}
		if (tel) {
			fprintf(tel, "]}\n");
			if (fflush(tel) || ferror(tel)) {
				fprintf(stderr, "lerwatt: telemetry write failed; stopping and restoring settings\n");
				ret = 2;
				break;
			}
		}
		if (lw_actuate(&sys, &act, &a)) {
			ret = 2;
			break;
		}

		if (duration_s > 0 && lw_now_ns() >= duration_deadline_ns)
			break;
		next_ns += (uint64_t)period_ms * 1000000ull;
		if (next_ns < lw_now_ns())	/* overran: skip, do not burst */
			next_ns = lw_now_ns() + (uint64_t)period_ms * 1000000ull;
		if (duration_s > 0 && next_ns > duration_deadline_ns)
			next_ns = duration_deadline_ns;
		int sleep_error = sleep_until(next_ns);

		if (sleep_error) {
			fprintf(stderr, "lerwatt: control sleep failed: %s; stopping and restoring settings\n",
				strerror(sleep_error));
			ret = 2;
			break;
		}
	}

finish:
	if (have_contract)
		lw_contract_close(&contract);
	if (actuator_ready && !dry_run && lw_journal_restore(&sys)) {
		fprintf(stderr, "lerwatt: restore had failures; run lerwattd --restore\n");
		ret = 2;
	}
	if (started)
		fprintf(stderr, "lerwatt: stopped after %.1f s, %lu writes, %lu write errors\n",
			(lw_now_ns() - start_ns) / 1e9, act.writes, act.write_errors);
	if (tel && tel != stdout && fclose(tel)) {
		fprintf(stderr, "lerwatt: telemetry close failed: %s\n", strerror(errno));
		ret = 2;
	}
	if (lock_fd >= 0)
		close(lock_fd);
	return ret;
}
