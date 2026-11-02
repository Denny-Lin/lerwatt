/* SPDX-License-Identifier: MIT */
/*
 * Lerwatt hardware abstraction: the real-machine interface.
 *
 * Everything here talks to the Linux kernel ABI only: sysfs, procfs, debugfs
 * and hwmon, resolved under a sysroot. On a board the sysroot is "/". When
 * lerwatt-sim runs, it maintains the same files under its own root, so this
 * code cannot tell the difference and contains no simulator logic.
 */
#ifndef LERWATT_HAL_H
#define LERWATT_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LW_MAX_CPUS	256
#define LW_MAX_POLICIES	64
#define LW_MAX_OPPS	32
#define LW_MAX_ZONES	16
#define LW_MAX_IDLE	8
#define LW_PATH_MAX	512

#define LW_SIM_MARKER	".lerwatt-sim"
#define LW_SIM_DEFAULT_ROOT "/run/lerwatt-sim"

/* One performance state. power_uw/perf come from the kernel Energy Model. */
struct lw_opp {
	uint32_t khz;
	uint32_t power_uw;	/* per CPU at 100% busy; 0 if no Energy Model */
	uint32_t perf;		/* capacity units, 0..1024 */
	bool inefficient;
};

struct lw_idle_state {
	char name[32];
	uint32_t latency_us;	/* cpuidle "latency" (entry + exit) */
	uint32_t residency_us;
};

struct lw_policy {
	int id;			/* N in cpufreq/policyN */
	int ncpus;
	int cpus[LW_MAX_CPUS];
	uint32_t capacity;	/* cpu_capacity of the policy's CPUs */
	uint32_t cpuinfo_min_khz;
	uint32_t cpuinfo_max_khz;
	char driver[32];
	char governor[32];
	bool synthetic_opps;	/* frequency-limit candidates, not a hardware OPP table */
	int nopps;
	struct lw_opp opps[LW_MAX_OPPS];	/* ascending frequency */
	bool have_em;
	int zone;		/* index into lw_system.zones, -1 if unmapped */
	int nidle;
	struct lw_idle_state idle[LW_MAX_IDLE];
};

struct lw_zone {
	int id;			/* N in thermal_zoneN */
	char type[64];
	char temp_path[LW_PATH_MAX]; /* resolved under sysroot; thermal or hwmon input */
	int32_t passive_mc;	/* lowest passive trip, INT32_MAX if none */
	int32_t critical_mc;	/* INT32_MAX if none */
};

struct lw_system {
	char sysroot[LW_PATH_MAX];
	bool simulated;		/* sysroot carries the lerwatt-sim marker */
	char sim_platform[32];
	int ncpus;
	int npolicies;
	struct lw_policy policies[LW_MAX_POLICIES];	/* sorted by first CPU */
	int nzones;
	struct lw_zone zones[LW_MAX_ZONES];
	int package_zone;	/* index into zones of the package sensor, -1 if none */
	int npackages;		/* distinct known physical package IDs; 0 when unavailable */
	bool sensor_ambiguous;	/* automatic package selection was deliberately withheld */
	char meter_path[LW_PATH_MAX];	/* hwmon power1_input, "" if none */
	char meter_name[64];
};

/* Per-period observations. */
struct lw_sample {
	uint64_t t_ns;			/* CLOCK_MONOTONIC */
	int32_t zone_mc[LW_MAX_ZONES];
	double busy_mean[LW_MAX_POLICIES];	/* 0..1 over the period */
	double busy_max[LW_MAX_POLICIES];
	uint32_t cur_khz[LW_MAX_POLICIES];
	double power_w;			/* board power if measured, else EM estimate */
	bool power_measured;
};

/* Opaque /proc/stat history used to turn counters into busy fractions. */
struct lw_cpustat {
	bool valid;
	uint64_t busy[LW_MAX_CPUS];
	uint64_t total[LW_MAX_CPUS];
};

/* ---- hal_fs.c: sysroot-relative file access and the restore journal ---- */
int lw_path(const struct lw_system *sys, char *out, size_t len, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
int lw_read_str(const char *path, char *buf, size_t len);
int lw_read_u64(const char *path, uint64_t *val);
int lw_read_s64(const char *path, int64_t *val);
int lw_write_str(const char *path, const char *val);
bool lw_exists(const char *path);

int lw_journal_open(const struct lw_system *sys);	/* creates the journal dir */
int lw_journal_save(const struct lw_system *sys, const char *path); /* record original value */
int lw_journal_restore(const struct lw_system *sys);	/* write back and remove */

/* ---- hal_discover.c ---- */
/*
 * Pick the sysroot. Order: explicit argument, real hardware ("/" with
 * cpufreq policies), a running simulator at LERWATT_SIM_ROOT or the default.
 * Returns 0 and fills sys->sysroot/simulated, or -1 with a message in err.
 */
int lw_detect(struct lw_system *sys, const char *explicit_root, char *err, size_t errlen);
int lw_discover(struct lw_system *sys, char *err, size_t errlen);
/* Explicit absolute /sys/... input in millidegrees Celsius, under sysroot. */
int lw_select_temperature_sensor(struct lw_system *sys, const char *path,
				 char *err, size_t errlen);
/* Inspect required inputs and optionally open control files writable; never writes. */
int lw_check_system(const struct lw_system *sys, bool writable, FILE *out);
void lw_describe(const struct lw_system *sys, FILE *out);

/* ---- hal_sample.c ---- */
int lw_sample(const struct lw_system *sys, struct lw_cpustat *hist, struct lw_sample *s);
/* EM-based estimate of CPU power for a policy at a frequency and busy fraction. */
double lw_em_power_w(const struct lw_policy *p, uint32_t khz, double busy);

/* ---- hal_actuate.c ---- */
struct lw_actuation {
	uint32_t max_khz[LW_MAX_POLICIES];	/* 0 = no cap from Lerwatt */
	uint32_t min_khz[LW_MAX_POLICIES];	/* 0 = no floor from Lerwatt */
	int32_t resume_latency_us[LW_MAX_POLICIES]; /* <0 = no constraint from Lerwatt */
};
/*
 * Lerwatt never raises the original ceiling: the effective ceiling is
 * min(decision, original scaling_max_freq), the floor is
 * min(effective ceiling, max(decision, original scaling_min_freq)). A thermal
 * cap can therefore lower an existing minimum. "No constraint" means
 * "put the original value back". Originals are read once at start.
 */
struct lw_actuator {
	bool dry_run;
	uint32_t orig_max[LW_MAX_POLICIES];
	uint32_t orig_min[LW_MAX_POLICIES];
	char orig_lat[LW_MAX_CPUS][16];
	uint32_t last_max[LW_MAX_POLICIES];
	uint32_t last_min[LW_MAX_POLICIES];
	char last_lat[LW_MAX_CPUS][16];
	unsigned long writes;
	unsigned long write_errors;
};
int lw_actuator_init(const struct lw_system *sys, struct lw_actuator *a, bool dry_run);
int lw_actuate(const struct lw_system *sys, struct lw_actuator *a, const struct lw_actuation *act);

uint64_t lw_now_ns(void);

#endif /* LERWATT_HAL_H */
