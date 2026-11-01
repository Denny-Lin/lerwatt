// SPDX-License-Identifier: MIT
/*
 * lerwatt-sim: stands in for an RK3588 board until a real one is connected.
 *
 * It maintains the kernel ABI under --root (default /run/lerwatt-sim) and
 * advances the hardware model every tick. lerwattd attaches to it on its
 * own; on a real board lerwattd ignores it, because real cpufreq wins.
 */
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "abi.h"
#include "contract.h"
#include "load.h"
#include "sim.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: lerwatt-sim [options]\n"
		"  --root DIR          simulator root (default /run/lerwatt-sim)\n"
		"  --cooling NAME      passive (default), fan or bare\n"
		"  --ambient C         ambient temperature, default 25\n"
		"  --initial-temp C    start the die at this temperature (default: ambient)\n"
		"  --load SPEC         procstat (default), pattern:idle|full|big|burst|steps|mixed,\n"
		"                      or trace:FILE\n"
		"  --cpu-map LIST      simulated CPU for each real CPU, e.g. 4,5,6,7,0,1,2,3\n"
		"  --sref X            host vCPU speed relative to an A76 at 2.4 GHz, default 1.0\n"
		"  --tick-ms N         model step, default 10\n"
		"  --duration S        stop after S simulated seconds\n"
		"  --virtual           run as fast as possible (pattern/trace loads only)\n"
		"  --no-kernel-thermal do not emulate step_wise throttling at passive trips\n"
		"  --no-cpuperf        ignore Lertick cpuperf targets even if the contract exists\n"
		"  --log FILE          ground-truth JSON lines every 100 ms (\"-\" = stdout)\n"
		"  --seed N            noise seed\n"
		"  --force             use a non-empty --root without a marker\n");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "root", required_argument, NULL, 'r' },
		{ "cooling", required_argument, NULL, 'c' },
		{ "ambient", required_argument, NULL, 'a' },
		{ "initial-temp", required_argument, NULL, 'i' },
		{ "load", required_argument, NULL, 'l' },
		{ "cpu-map", required_argument, NULL, 'm' },
		{ "sref", required_argument, NULL, 's' },
		{ "tick-ms", required_argument, NULL, 't' },
		{ "duration", required_argument, NULL, 'd' },
		{ "virtual", no_argument, NULL, 'V' },
		{ "no-kernel-thermal", no_argument, NULL, 'K' },
		{ "no-cpuperf", no_argument, NULL, 'P' },
		{ "log", required_argument, NULL, 'L' },
		{ "seed", required_argument, NULL, 'S' },
		{ "force", no_argument, NULL, 'F' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};
	const char *root = "/run/lerwatt-sim", *cooling = "passive", *load_spec = "procstat";
	const char *map_spec = NULL, *log_path = NULL;
	double ambient = 25.0, initial = -1000, sref = 1.0, duration = 0;
	unsigned int tick_ms = 10;
	uint64_t seed = 1;
	bool virtual_time = false, kernel_thermal = true, use_cpuperf = true;
	int force = 0, cool_idx = -1, opt, ret = 0;
	static struct sim_state s;
	static struct sim_abi abi;
	struct sim_load load;
	struct lw_contract contract;
	bool have_contract = false;
	struct sigaction sa = { 0 };
	double demand[SIM_MAX_CPUS] = { 0 }, max_zone[SIM_MAX_ZONES], next_log = 0;
	char err[256];
	FILE *log = NULL;
	uint64_t start, next;

	while ((opt = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
		switch (opt) {
		case 'r': root = optarg; break;
		case 'c': cooling = optarg; break;
		case 'a': ambient = strtod(optarg, NULL); break;
		case 'i': initial = strtod(optarg, NULL); break;
		case 'l': load_spec = optarg; break;
		case 'm': map_spec = optarg; break;
		case 's': sref = strtod(optarg, NULL); break;
		case 't': tick_ms = (unsigned int)strtoul(optarg, NULL, 10); break;
		case 'd': duration = strtod(optarg, NULL); break;
		case 'V': virtual_time = true; break;
		case 'K': kernel_thermal = false; break;
		case 'P': use_cpuperf = false; break;
		case 'L': log_path = optarg; break;
		case 'S': seed = strtoull(optarg, NULL, 10); break;
		case 'F': force = 1; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 1;
		}
	}
	for (int i = 0; i < sim_rk3588.ncooling; i++)
		if (!strcmp(cooling, sim_rk3588.cooling[i].name))
			cool_idx = i;
	if (cool_idx < 0 || tick_ms < 1 || tick_ms > 100 || sref <= 0) {
		usage(stderr);
		return 1;
	}

	sim_init(&s, &sim_rk3588, cool_idx, ambient, initial > -273 ? initial : ambient, sref, seed);
	s.kernel_thermal = kernel_thermal;
	if (sim_load_open(&load, &sim_rk3588, load_spec, map_spec, err, sizeof(err))) {
		fprintf(stderr, "lerwatt-sim: %s\n", err);
		return 1;
	}
	if (virtual_time && sim_load_is_realtime(&load)) {
		fprintf(stderr, "lerwatt-sim: --virtual needs a pattern or trace load\n");
		return 1;
	}
	if (sim_abi_create(&abi, root, &s, force, err, sizeof(err))) {
		fprintf(stderr, "lerwatt-sim: %s\n", err);
		return 1;
	}
	if (use_cpuperf)
		have_contract = lw_contract_open(&contract, false) == 0;
	if (log_path)
		log = strcmp(log_path, "-") ? fopen(log_path, "w") : stdout;
	for (int z = 0; z < SIM_MAX_ZONES; z++)
		max_zone[z] = -1e9;

	fprintf(stderr, "lerwatt-sim: %s at %s, cooling %s, ambient %.1f C, load %s, tick %u ms%s%s\n",
		sim_rk3588.name, root, cooling, ambient, load_spec, tick_ms,
		virtual_time ? ", virtual time" : "",
		have_contract ? ", cpuperf from Lertick" : "");

	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	start = next = now_ns();
	while (!stop) {
		double dt = tick_ms / 1000.0;

		sim_load_sample(&load, s.t_s, demand);
		sim_abi_ingest(&abi, &s);
		for (int k = 0; k < sim_rk3588.nclusters; k++) {
			struct lt_demand_rec d;

			s.cl[k].cpuperf_req = -1;
			if (have_contract &&
			    lw_contract_demand(&contract, (unsigned int)k, now_ns(), 500000000ull, &d) &&
			    d.cpuperf_max)
				s.cl[k].cpuperf_req = (int32_t)d.cpuperf_max;
		}
		sim_step(&s, demand, dt);
		sim_abi_publish(&abi, &s);

		for (int z = 0; z < sim_rk3588.nzones; z++)
			if (s.zone[z].sensor_mc / 1000.0 > max_zone[z])
				max_zone[z] = s.zone[z].sensor_mc / 1000.0;
		if (log && s.t_s + 1e-9 >= next_log) {
			fprintf(log, "{\"t\":%.2f,\"board_w\":%.3f,\"meter_w\":%.3f,\"energy_j\":%.1f,\"die_c\":%.2f",
				s.t_s, s.board_w, s.meter_w, s.energy_j, s.die_c);
			for (int k = 0; k < sim_rk3588.nclusters; k++)
				fprintf(log, ",\"%s\":{\"c\":%.2f,\"w\":%.3f,\"mhz\":%u,\"max_mhz\":%u,\"tstate\":%d}",
					sim_rk3588.clusters[k].name, s.cl[k].temp_c, s.cl[k].power_w,
					sim_rk3588.clusters[k].opps[s.cl[k].cur_idx].khz / 1000,
					sim_rk3588.clusters[k].opps[sim_eff_max_idx(&s, k)].khz / 1000,
					s.cl[k].thermal_state);
			fprintf(log, ",\"served\":%.4f}\n",
				s.work_demand > 0 ? s.work_done / s.work_demand : 1.0);
			fflush(log);
			next_log += 0.1;
		}
		if (s.critical) {
			fprintf(stderr, "lerwatt-sim: CRITICAL trip at %.1f s: a real board would power off now\n",
				s.t_s);
			ret = 3;
			break;
		}
		if (duration > 0 && s.t_s >= duration)
			break;
		if (!virtual_time) {
			struct timespec ts;

			next += (uint64_t)tick_ms * 1000000ull;
			if (next < now_ns())
				next = now_ns();
			ts.tv_sec = (time_t)(next / 1000000000ull);
			ts.tv_nsec = (long)(next % 1000000000ull);
			while (!stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
				;
		}
	}

	sim_abi_close(&abi);
	if (have_contract)
		lw_contract_close(&contract);
	sim_load_close(&load);
	fprintf(stderr,
		"lerwatt-sim: %.1f s simulated in %.1f s: energy %.1f J (mean %.2f W), work served %.1f%%, "
		"%llu throttle steps, peak %.1f / %.1f / %.1f C (big0 / big1 / little), %lu bad writes\n",
		s.t_s, (now_ns() - start) / 1e9, s.energy_j, s.t_s > 0 ? s.energy_j / s.t_s : 0,
		s.work_demand > 0 ? 100.0 * s.work_done / s.work_demand : 100.0,
		(unsigned long long)s.throttle_events, max_zone[1], max_zone[2], max_zone[3],
		abi.bad_writes);
	if (log && log != stdout)
		fclose(log);
	return ret;
}
