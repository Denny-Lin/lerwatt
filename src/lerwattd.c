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
#include "checkpoint.h"
#include "lerwatt/controller.h"
#include "lerwatt/hal.h"
#include "notify.h"
#include "runtime.h"

#ifndef LERWATT_VERSION
#define LERWATT_VERSION "source"
#endif

#define CHECKPOINT_INTERVAL_NS 300000000000ull

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
		"  --prediction nominal|cooperative  MPC predictor (default nominal); cooperative\n"
		"                     uses action-supported five-second corrections conservatively\n"
		"  --describe         print the discovered system and exit\n"
		"  --check            check hardware capabilities without changing settings\n"
		"  --temperature-sensor PATH  explicitly select a CPU/package sensor (millidegC)\n"
		"  --watchdog         supervised service mode; require systemd notify/watchdog environment\n"
		"  --model-state FILE persist compatible learned thermal parameters (active control only)\n"
		"  --version          print build version and exit\n"
		"  --build-features   print compiled optional interface support and exit\n"
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

static uint64_t policy_topology_id(const struct lw_policy *policy)
{
	bool member[LW_MAX_CPUS] = { false };
	uint64_t hash = lw_contract_topology_seed();

	for (int i = 0; i < policy->ncpus; i++)
		member[policy->cpus[i]] = true;
	for (int cpu = 0; cpu < LW_MAX_CPUS; cpu++)
		if (member[cpu])
			hash = lw_contract_topology_add_cpu(hash, (uint32_t)cpu);
	return lw_contract_topology_finish(hash, (uint32_t)policy->ncpus);
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

/* CLOCK_BOOTTIME includes suspend while the controller's MONOTONIC clock does
 * not. A resumed service must restore and rediscover rather than reuse stale
 * topology, samples or controller history. */
static int boot_clock(uint64_t *value)
{
#ifdef CLOCK_BOOTTIME
	struct timespec now;

	if (clock_gettime(CLOCK_BOOTTIME, &now))
		return -errno;
	*value = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
	return 0;
#else
	(void)value;
	return -ENOTSUP;
#endif
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
		{ "prediction", required_argument, NULL, 'A' },
		{ "describe", no_argument, NULL, 'D' },
		{ "check", no_argument, NULL, 'C' },
		{ "temperature-sensor", required_argument, NULL, 'S' },
		{ "watchdog", no_argument, NULL, 'W' },
		{ "model-state", required_argument, NULL, 'M' },
		{ "version", no_argument, NULL, 'V' },
		{ "build-features", no_argument, NULL, 'B' },
		{ "restore", no_argument, NULL, 'R' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	const char *sysroot = NULL, *telemetry = NULL, *sensor_path = NULL, *model_path = NULL;
	unsigned int period_ms = 100;
	double duration_s = 0, soft_c = 0;
	bool dry_run = false, use_contract = true, describe = false, restore = false;
	bool cross_cluster = false;
	bool check = false, actuator_ready = false, started = false;
	bool temporal_forecast = true;
	bool cooperative_prediction = false;
	bool watchdog = false, checkpoint_loaded = false;
	struct lw_notify notifier = { .fd = -1 };
	static struct lw_system sys;
	static struct lw_ctl_state st[LW_MAX_POLICIES];
	struct lw_ctl_params params;
	struct lw_contract contract;
	struct lw_actuator act;
	struct lw_cpustat hist = { 0 };
	struct sigaction sa = { 0 };
	char err[256];
	char online_path[LW_PATH_MAX], online_cpus[LW_MAX_CPUS * 12];
	FILE *tel = NULL;
	uint64_t start_ns = 0, next_ns = 0, last_ns = 0;
	uint64_t duration_deadline_ns = 0;
	uint64_t next_checkpoint_ns = 0, boot_offset_ns = 0;
	uint64_t next_topology_ns = 0;
	int opt, ret = 0;
	int lock_fd = -1;
	bool have_contract = false;
	uint64_t contract_next_try = 0;
	uint64_t topology_ids[LW_CONTRACT_MAX_CLUSTERS] = { 0 };
	int previous_matches = -1;
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
		case 'A':
			if (strcmp(optarg, "nominal") && strcmp(optarg, "cooperative")) {
				usage(stderr);
				return 1;
			}
			cooperative_prediction = !strcmp(optarg, "cooperative");
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
		case 'W': watchdog = true; break;
		case 'M': model_path = optarg; break;
		case 'V': printf("lerwattd %s\n", LERWATT_VERSION); return 0;
		case 'B':
#if defined(__linux__) && !defined(LW_NO_BPF)
			puts("contract=bpf");
#else
			puts("contract=none");
#endif
			return 0;
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
	if (cooperative_prediction && (policy != LW_POLICY_MPC || !temporal_forecast || period_ms > 100)) {
		fprintf(stderr, "lerwatt: cooperative prediction requires --policy mpc --forecast temporal and period 10..100 ms\n");
		return 1;
	}
	if ((watchdog || model_path) && (dry_run || describe || check || restore)) {
		fprintf(stderr, "lerwatt: --watchdog and --model-state require active control\n");
		return 1;
	}
	if (model_path && (model_path[0] != '/' || strlen(model_path) >= LW_PATH_MAX)) {
		fprintf(stderr, "lerwatt: --model-state must be a bounded absolute path\n");
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
	if (use_contract && period_ms > 100) {
		fprintf(stderr, "lerwatt: control period exceeds the contract's 100 ms cadence; running standalone\n");
		use_contract = false;
	}
	if (use_contract)
		for (int i = 0; i < sys.npolicies; i++)
			topology_ids[i] = policy_topology_id(&sys.policies[i]);

	lw_ctl_default_params(&params, lowest_passive(&sys));
	params.policy = policy;
	params.cross_cluster = cross_cluster;
	params.temporal_forecast = temporal_forecast;
	params.cooperative_prediction = cooperative_prediction;
	if (policy == LW_POLICY_MPC && sys.package_zone < 0)
		fprintf(stderr, "lerwatt: no package zone: --policy mpc stays on the rule policy\n");
	if (soft_c > 0)
		params.soft_limit_mc = (int32_t)(soft_c * 1000.0);
	fprintf(stderr, "lerwatt: soft limit %.1f degC, period %u ms%s\n",
		params.soft_limit_mc / 1000.0, period_ms, dry_run ? ", dry run" : "");
	if (watchdog) {
		int error = lw_notify_init(&notifier, period_ms);

		if (error) {
			fprintf(stderr, "lerwatt: invalid service watchdog environment or control period: %s\n",
				strerror(-error));
			ret = 2;
			goto finish;
		}
		if (lw_path(&sys, online_path, sizeof(online_path), "/sys/devices/system/cpu/online") ||
		    lw_read_str(online_path, online_cpus, sizeof(online_cpus)) || !online_cpus[0]) {
			fprintf(stderr, "lerwatt: service requires a readable online CPU mask\n");
			ret = 2;
			goto finish;
		}
	}

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
						     LW_CONTRACT_RETRY_NS, LW_CONTRACT_STALE_NS,
						     &contract_next_try, &c);
		fprintf(stderr, "lerwatt: Lertick transport %s\n",
			have_contract ? "opened; awaiting matching demand" : c == -ENOENT ? "absent (running alone)" :
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
	if (watchdog) {
		uint64_t boot_ns;

		if (boot_clock(&boot_ns) || boot_ns < start_ns) {
			fprintf(stderr, "lerwatt: cannot establish service suspend clock\n");
			ret = 2;
			goto finish;
		}
		boot_offset_ns = boot_ns - start_ns;
	}
	next_checkpoint_ns = start_ns + CHECKPOINT_INTERVAL_NS;
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
		if (watchdog) {
			uint64_t mono_ns = lw_now_ns(), boot_ns;
			uint64_t maximum_gap_ns = (uint64_t)period_ms * 5000000ull;

			if (maximum_gap_ns < 5000000000ull)
				maximum_gap_ns = 5000000000ull;
			if (boot_clock(&boot_ns) || mono_ns < last_ns ||
			    mono_ns - last_ns > maximum_gap_ns || boot_ns < mono_ns ||
			    boot_ns - mono_ns > boot_offset_ns + 1000000000ull) {
				fprintf(stderr, "lerwatt: service resumed or control observations became stale; stopping and restoring settings\n");
				ret = 2;
				break;
			}
			if (mono_ns >= next_topology_ns) {
				char current_cpus[sizeof(online_cpus)];

				if (lw_read_str(online_path, current_cpus, sizeof(current_cpus)) ||
				    strcmp(current_cpus, online_cpus)) {
					fprintf(stderr, "lerwatt: online CPU topology changed; stopping and restoring settings for rediscovery\n");
					ret = 2;
					break;
				}
				next_topology_ns = mono_ns + 1000000000ull;
			}
		}

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

			have_contract = lw_contract_maintain(&contract, have_contract, true, lw_now_ns(),
							     LW_CONTRACT_RETRY_NS, LW_CONTRACT_STALE_NS,
							     &contract_next_try, NULL);
			if (have_contract != was)
				fprintf(stderr, "lerwatt: Lertick transport %s\n",
					have_contract ? "opened; awaiting matching demand" : "lost (running alone)");
		}
		if (tel)
			fprintf(tel, "{\"t\":%.3f,\"t_ns\":%llu,\"mode\":\"%s\",\"power_w\":%.3f,\"measured\":%s,\"clusters\":[",
				(s.t_ns - start_ns) / 1e9, (unsigned long long)(s.t_ns - start_ns),
				sys.simulated ? "sim" : "hw", s.power_w,
				s.power_measured ? "true" : "false");

		struct lw_ctl_in inputs[LW_MAX_POLICIES];
		struct lw_ctl_out outputs[LW_MAX_POLICIES];
		uint64_t sessions[LW_CONTRACT_MAX_CLUSTERS] = { 0 };
		int matched_policies = 0;
		for (int i = 0; i < sys.npolicies; i++) {
			const struct lw_policy *p = &sys.policies[i];
			struct lt_demand_rec d;
			struct lw_ctl_in in = {
				.temp_mc = cluster_temp(&sys, &s, i),
				.busy_mean = s.busy_mean[i],
				.cur_khz = s.cur_khz[i],
				.effective_max_khz = s.max_khz[i],
				.allowed_max_khz = act.orig_max[i],
				.power_quality = p->have_em ? LW_POWER_ENERGY_MODEL : LW_POWER_PROXY,
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
			    lw_contract_demand(&contract, (unsigned int)i, topology_ids[i], lw_now_ns(),
					       LW_CONTRACT_DEMAND_MAX_AGE_NS, &d)) {
				in.lat_crit = d.lat_crit_milli / 1000.0;
				sessions[i] = d.session_id;
				matched_policies++;
			}

			inputs[i] = in;
		}
		lw_ctl_step_group(&params, inputs, st, outputs, sys.npolicies,
				  s.t_ns, dt > 0 ? dt : period_ms / 1000.0);
		if (model_path && !checkpoint_loaded) {
			int error = lw_checkpoint_load(model_path, &sys, &params, st);

			checkpoint_loaded = true;
			if (!error)
				fprintf(stderr, "lerwatt: restored compatible learned thermal parameters\n");
			else if (error != -ENOENT)
				fprintf(stderr, "lerwatt: ignoring incompatible or unreadable model checkpoint: %s\n",
					strerror(-error));
		}
		for (int i = 0; i < sys.npolicies; i++) {
			const struct lw_policy *p = &sys.policies[i];
			const struct lw_ctl_in in = inputs[i];
			const struct lw_ctl_out o = outputs[i];

			est_w[i] = o.est_power_w;
			a.max_khz[i] = o.ceiling_khz < p->opps[p->nopps - 1].khz ? o.ceiling_khz : 0;
			a.min_khz[i] = o.floor_khz;
			a.resume_latency_us[i] = o.resume_latency_us;

			if (tel)
				fprintf(tel, "%s{\"id\":%d,\"temp_c\":%.3f,\"busy\":%.3f,\"cur_mhz\":%u,"
					"\"ceil_mhz\":%u,\"budget_w\":%.3f,\"est_w\":%.3f,\"price\":%u,"
					"\"floor_mhz\":%u,\"em\":%s,\"model\":%s,\"joint\":%s,\"pred_c\":%.1f,\"forecast_c\":%.3f,"
					"\"forecast_source\":\"%s\",\"forecast_labels\":%llu,\"forecast_dropped\":%llu,\"forecast_weights\":[%.5f,%.5f,%.5f],"
					"\"forecast_base_c\":%.3f,\"forecast_correction_c\":%.3f,\"temporal_labels\":%llu,\"temporal_dropped\":%llu,\"prediction_mode\":\"%s\",\"prediction_queries\":%llu,\"prediction_corrections\":%llu,\"prediction_reason\":\"%s\",\"prediction_nominal_c\":%.3f,\"prediction_upper_c\":%.3f,\"prediction_epoch_resets\":%llu,\"observed_max_mhz\":%u}",
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
					(unsigned long long)o.temporal_dropped,
					cooperative_prediction ? "cooperative" : "nominal",
					(unsigned long long)o.prediction_queries, (unsigned long long)o.prediction_corrections,
					lw_prediction_reason_name(o.prediction_reason),
					isfinite(o.prediction_nominal_c) ? o.prediction_nominal_c : -1.0,
					isfinite(o.prediction_upper_c) ? o.prediction_upper_c : -1.0,
					(unsigned long long)st[i].predictor.support.resets, s.max_khz[i] / 1000);
		}
		if (tel) {
			fprintf(tel, "]}\n");
			if (fflush(tel) || ferror(tel)) {
				fprintf(stderr, "lerwatt: telemetry write failed; stopping and restoring settings\n");
				ret = 2;
				break;
			}
		}
		/* Sampling, the first checkpoint read or output I/O can block after
		 * the loop-entry check. Never apply a plan made from stale observations
		 * merely because that work eventually returned. */
		if (watchdog) {
			uint64_t now = lw_now_ns(), boot_ns;
			uint64_t maximum_gap_ns = (uint64_t)period_ms * 5000000ull;

			if (maximum_gap_ns < 5000000000ull)
				maximum_gap_ns = 5000000000ull;
			if (now < s.t_ns || now - s.t_ns > maximum_gap_ns ||
			    boot_clock(&boot_ns) || boot_ns < now ||
			    boot_ns - now > boot_offset_ns + 1000000000ull) {
				fprintf(stderr, "lerwatt: CPU observation became stale before actuation; stopping and restoring settings\n");
				ret = 2;
				break;
			}
		}
		if (lw_actuate(&sys, &act, &a)) {
			ret = 2;
			break;
		}
		/* Advice is published only after successful hardware actuation and a
		 * matching fresh demand identity. It is never an alternate actuator. */
		if (have_contract) {
			uint64_t published_ns = lw_now_ns();

			for (int i = 0; i < sys.npolicies; i++) {
				struct lt_demand_rec fresh;
				const struct lw_ctl_out *o = &outputs[i];
				struct lw_contract_rec record = {
					.ceiling_cap = o->ceiling_cap,
					.floor_cap = o->floor_cap,
					.energy_price_milli = o->price_milli,
					.thermal_headroom_mc = o->headroom_mc,
					.valid_until_ns = published_ns + LW_CONTRACT_TTL_NS,
					.topology_id = topology_ids[i], .session_id = sessions[i],
				};

				if (sessions[i] && lw_contract_demand(&contract, (unsigned int)i,
					    topology_ids[i], published_ns, LW_CONTRACT_DEMAND_MAX_AGE_NS, &fresh) &&
				    fresh.session_id == sessions[i])
					lw_contract_publish(&contract, (unsigned int)i, &record);
			}
		}
		const char *link = !use_contract ? "off" : matched_policies == sys.npolicies ? "connected" : "waiting";
		if (previous_matches != matched_policies) {
			fprintf(stderr, "lerwatt: Lertick link %s (%d/%d matching policies; ABI %d)\n",
				link, matched_policies, sys.npolicies, LW_CONTRACT_VERSION);
			previous_matches = matched_policies;
		}
		if (watchdog) {
			char status[160];

			snprintf(status, sizeof(status), "Controlling CPU limits; Lertick: %s", link);
			lw_notify_set_status(&notifier, status);
			int error = lw_notify_progress(&notifier, lw_now_ns());

			if (error) {
				fprintf(stderr, "lerwatt: watchdog notification failed; stopping and restoring settings: %s\n",
					strerror(-error));
				ret = 2;
				break;
			}
		}
		if (model_path && lw_now_ns() >= next_checkpoint_ns) {
			int error = lw_checkpoint_save(model_path, &sys, &params, st);

			if (error)
				fprintf(stderr, "lerwatt: model checkpoint not saved: %s; continuing with in-memory learning\n",
					strerror(-error));
			next_checkpoint_ns = lw_now_ns() + CHECKPOINT_INTERVAL_NS;
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
	/* Restore controls before optional disk I/O: a slow checkpoint must not
	 * prolong the limits left by a cleanly stopping controller. */
	if (started && model_path && !ret) {
		int error = lw_checkpoint_save(model_path, &sys, &params, st);

		if (error)
			fprintf(stderr, "lerwatt: model checkpoint not saved at shutdown: %s\n", strerror(-error));
	}
	if (started)
		fprintf(stderr, "lerwatt: stopped after %.1f s, %lu writes, %lu write errors\n",
			(lw_now_ns() - start_ns) / 1e9, act.writes, act.write_errors);
	if (tel && tel != stdout && fclose(tel)) {
		fprintf(stderr, "lerwatt: telemetry close failed: %s\n", strerror(errno));
		ret = 2;
	}
	/* Keep systemd's watchdog armed while restoration and final persistence
	 * run. A blocked cleanup must not announce that supervision can stop. */
	lw_notify_close(&notifier);
	if (lock_fd >= 0)
		close(lock_fd);
	return ret;
}
