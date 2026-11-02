// SPDX-License-Identifier: MIT
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "checkpoint.h"
#include "lerwatt/controller.h"

static struct lw_system sys;
static struct lw_ctl_params params;
static struct lw_ctl_state trained[2], fresh[2], before[2];
static unsigned char original[16384], damaged[16384];
static size_t original_length;
static char directory[LW_PATH_MAX], filename[LW_PATH_MAX], root[LW_PATH_MAX];

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *walk)
{
	(void)st; (void)type; (void)walk;
	return remove(path);
}

static void cleanup(void)
{
	if (directory[0]) nftw(directory, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
}

static void path_join(char *out, size_t size, const char *parent, const char *name)
{
	int n = snprintf(out, size, "%s/%s", parent, name);
	assert(n > 0 && (size_t)n < size);
}

static void write_bytes(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	assert(fd >= 0);
	assert(write(fd, data, size) == (ssize_t)size);
	assert(!close(fd));
}

static size_t read_bytes(const char *path, void *data, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;
	assert(fd >= 0);
	n = read(fd, data, size); assert(n >= 0);
	assert(!close(fd));
	return (size_t)n;
}

static void put64(unsigned char *out, uint64_t value)
{
	for (unsigned int i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static void checksum(unsigned char *data, size_t size)
{
	uint64_t h = UINT64_C(14695981039346656037);
	for (size_t i = 0; i < size - 8; i++) { h ^= data[i]; h *= UINT64_C(1099511628211); }
	put64(data + size - 8, h);
}

static void make_fresh(void)
{
	memset(fresh, 0, sizeof(fresh));
	for (int i = 0; i < 2; i++) {
		fresh[i].init = fresh[i].temp_init = true;
		fresh[i].integ_w = 17; fresh[i].ceil_idx = 1; fresh[i].last_down_ns = 123;
		fresh[i].temp_f_mc = 61234; fresh[i].temp_s_mc = 62345;
		fresh[i].forecaster.count = 3; fresh[i].forecaster.last_issue_ns = 998;
		fresh[i].temporal.count = 2; fresh[i].temporal.last_issue_ns = 997;
		fresh[i].tm.have_prev = true; fresh[i].tm.have_mean = false;
		fresh[i].tm.prev_mean_w = 77; fresh[i].tm.prev_mean_c = 44;
		fresh[i].tm.acc_s = .1; fresh[i].tm.acc_energy_j = .2; fresh[i].tm.acc_pkg = 6;
		fresh[i].tm.forecast_own_w = .4; fresh[i].tm.forecast_total_w = .8;
		fresh[i].tm.last_own_w = .9; fresh[i].tm.last_total_w = 1.2;
		fresh[i].tm.peak_offset_c = 4; fresh[i].tm.peak_power_w = 2;
		fresh[i].tm.load_reversals = 7; fresh[i].tm.load_direction = -1;
	}
}

static void unchanged_on_failure(void)
{
	make_fresh(); memcpy(before, fresh, sizeof(fresh));
	assert(lw_checkpoint_load(filename, &sys, &params, fresh) < 0);
	assert(!memcmp(before, fresh, sizeof(fresh)));
}

static void check_private_files(void)
{
	char other[LW_PATH_MAX], alias[LW_PATH_MAX], linked_model[LW_PATH_MAX];
	const char text[] = "unrelated private data\n";
	path_join(other, sizeof(other), directory, "unrelated");
	write_bytes(other, text, sizeof(text));
	assert(lw_checkpoint_save(other, &sys, &params, trained) < 0);
	assert(read_bytes(other, damaged, sizeof(damaged)) == sizeof(text));
	assert(!memcmp(damaged, text, sizeof(text)));
	assert(!unlink(filename));
	assert(!symlink(other, filename));
	unchanged_on_failure(); assert(lw_checkpoint_save(filename, &sys, &params, trained) < 0);
	assert(!unlink(filename));
	assert(!link(other, filename));
	unchanged_on_failure(); assert(lw_checkpoint_save(filename, &sys, &params, trained) < 0);
	assert(!unlink(filename));
	assert(!mkfifo(filename, 0600));
	alarm(2); unchanged_on_failure(); assert(lw_checkpoint_save(filename, &sys, &params, trained) < 0); alarm(0);
	assert(!unlink(filename));
	write_bytes(filename, original, original_length);
	assert(!chmod(filename, 0644));
	unchanged_on_failure(); assert(lw_checkpoint_save(filename, &sys, &params, trained) < 0);
	assert(!chmod(filename, 0600));
	assert(!chmod(directory, 0755));
	unchanged_on_failure(); assert(lw_checkpoint_save(filename, &sys, &params, trained) < 0);
	assert(!chmod(directory, 0700));
	path_join(alias, sizeof(alias), root, "private-link");
	assert(!symlink(directory, alias));
	path_join(linked_model, sizeof(linked_model), alias, "model.state");
	assert(lw_checkpoint_load(linked_model, &sys, &params, fresh) < 0);
	assert(lw_checkpoint_save(linked_model, &sys, &params, trained) < 0);
	assert(!unlink(alias));
}

int main(void)
{
	char temporary[] = "/tmp/lerwatt-checkpoint-XXXXXX", path[LW_PATH_MAX];
	char *resolved;
	struct stat st;
	assert(mkdtemp(temporary)); resolved = realpath(temporary, NULL); assert(resolved);
	/* Resolve macOS /tmp before testing the deliberate no-symlink traversal. */
	assert(strlen(resolved) < sizeof(root)); strcpy(root, resolved); free(resolved);
	path_join(directory, sizeof(directory), root, "private"); assert(!mkdir(directory, 0700));
	atexit(cleanup);
	path_join(filename, sizeof(filename), directory, "model.state");
	memset(&sys, 0, sizeof(sys));
	strcpy(sys.sysroot, root); sys.npolicies = 2; sys.ncpus = 2; sys.npackages = 1;
	sys.nzones = 1; sys.package_zone = 0;
	strcpy(sys.zones[0].temp_path, "/sys/class/hwmon/hwmon0/temp1_input");
	strcpy(sys.zones[0].type, "package"); sys.zones[0].passive_mc = 85000;
	params.policy = LW_POLICY_MPC; params.soft_limit_mc = 80000; params.raw_limit_mc = 82000;
	for (int i = 0; i < 2; i++) {
		struct lw_policy *p = &sys.policies[i];
		p->id = i; p->ncpus = 1; p->cpus[0] = i; p->capacity = 1024;
		p->cpuinfo_min_khz = 400000; p->cpuinfo_max_khz = 4800000;
		strcpy(p->driver, "intel_pstate"); strcpy(p->governor, "powersave");
		p->zone = 0; p->nopps = 2;
		p->opps[0] = (struct lw_opp){ .khz = 400000, .power_uw = 10000, .perf = 85 };
		p->opps[1] = (struct lw_opp){ .khz = 4800000, .power_uw = 1000000, .perf = 1024 };
		trained[i].init = true;
		trained[i].tm.theta[0] = .1 + i * .01; trained[i].tm.theta[1] = -.01;
		trained[i].tm.theta[2] = .25; trained[i].tm.lambda = .98;
		for (int j = 0; j < LW_TM_N; j++) trained[i].tm.cov[j][j] = .5 + j;
		trained[i].tm.samples = 32; trained[i].tm.resid_c_per_s = .2;
		trained[i].tm.offset_avg_c = 2; trained[i].tm.power_avg_w = 1; trained[i].tm.hot_s = 300;
		trained[i].tm.have_prev = trained[i].tm.have_mean = true;
		trained[i].tm.acc_s = 7; trained[i].tm.peak_offset_c = 100;
		trained[i].tm.peak_power_w = 200; trained[i].tm.load_reversals = 500;
	}
	assert(lw_checkpoint_load(filename, &sys, &params, fresh) == -ENOENT);
	assert(!lw_checkpoint_save(filename, &sys, &params, trained));
	assert(!stat(filename, &st) && (st.st_mode & 0777) == 0600);
	original_length = read_bytes(filename, original, sizeof(original));
	assert(original_length > 100 && original_length < sizeof(original));
	make_fresh(); memcpy(before, fresh, sizeof(fresh));
	assert(!lw_checkpoint_load(filename, &sys, &params, fresh));
	for (int i = 0; i < 2; i++) {
		assert(fresh[i].tm.theta[0] == trained[i].tm.theta[0]);
		assert(fresh[i].tm.cov[2][2] == trained[i].tm.cov[2][2]);
		assert(fresh[i].tm.samples == 32 && fresh[i].tm.hot_s == 300);
		assert(fresh[i].integ_w == before[i].integ_w && fresh[i].ceil_idx == before[i].ceil_idx);
		assert(!memcmp(&fresh[i].forecaster, &before[i].forecaster, sizeof(fresh[i].forecaster)));
		assert(!memcmp(&fresh[i].temporal, &before[i].temporal, sizeof(fresh[i].temporal)));
		/* Compare all thermal transients by replacing only learned values. */
		memcpy(before[i].tm.theta, fresh[i].tm.theta, sizeof(fresh[i].tm.theta));
		memcpy(before[i].tm.cov, fresh[i].tm.cov, sizeof(fresh[i].tm.cov));
		before[i].tm.lambda = fresh[i].tm.lambda; before[i].tm.samples = fresh[i].tm.samples;
		before[i].tm.resid_c_per_s = fresh[i].tm.resid_c_per_s;
		before[i].tm.offset_avg_c = fresh[i].tm.offset_avg_c;
		before[i].tm.power_avg_w = fresh[i].tm.power_avg_w; before[i].tm.hot_s = fresh[i].tm.hot_s;
	}
	assert(!memcmp(before, fresh, sizeof(fresh)));
	/* Refuse load before initialization, with no partial policy mutation. */
	make_fresh(); fresh[1].init = false; memcpy(before, fresh, sizeof(fresh));
	assert(lw_checkpoint_load(filename, &sys, &params, fresh) == -EAGAIN);
	assert(!memcmp(before, fresh, sizeof(fresh)));
	sys.policies[1].opps[1].power_uw++;
	unchanged_on_failure(); sys.policies[1].opps[1].power_uw--;
	params.soft_limit_mc++; unchanged_on_failure(); params.soft_limit_mc--;
	strcpy(sys.sysroot, directory); unchanged_on_failure(); strcpy(sys.sysroot, root);
	sys.npolicies = LW_MAX_POLICIES + 1;
	assert(lw_checkpoint_save(filename, &sys, &params, trained) == -EINVAL);
	assert(lw_checkpoint_load(filename, &sys, &params, fresh) == -EINVAL);
	sys.npolicies = 2;
	sys.policies[0].nopps = LW_MAX_OPPS + 1;
	assert(lw_checkpoint_save(filename, &sys, &params, trained) == -EINVAL);
	sys.policies[0].nopps = 2;
	/* No trained state may erase a previously useful checkpoint. */
	trained[0].tm.samples = trained[1].tm.samples = 0;
	assert(!lw_checkpoint_save(filename, &sys, &params, trained));
	assert(read_bytes(filename, damaged, sizeof(damaged)) == original_length);
	assert(!memcmp(original, damaged, original_length));
	trained[0].tm.samples = trained[1].tm.samples = 32;
	for (int failure = 0; failure < 7; failure++) {
		memcpy(damaged, original, original_length);
		if (failure == 0) damaged[original_length - 1] ^= 1; /* checksum */
		if (failure == 1) damaged[8] = 2; /* schema */
		if (failure == 2) damaged[12] = 255; /* bounded count */
		if (failure == 3) put64(damaged + 24, (uint64_t)time(NULL) - 31u * 86400u);
		if (failure == 4) put64(damaged + 40 + 16, UINT64_C(0x7ff8000000000000)); /* NaN */
		if (failure == 5) put64(damaged + 40 + 16 + 3 * 8, UINT64_C(0xbff0000000000000)); /* negative covariance */
		if (failure == 6) damaged[40] = 3; /* invalid trained flag */
		if (failure) checksum(damaged, original_length);
		write_bytes(filename, damaged, original_length); unchanged_on_failure();
	}
	/* Unknown files are never overwritten, even when root owns them. */
	write_bytes(filename, "unknown private file", 20);
	assert(lw_checkpoint_save(filename, &sys, &params, trained) < 0);
	assert(read_bytes(filename, damaged, sizeof(damaged)) == 20);
	assert(!memcmp(damaged, "unknown private file", 20));
	write_bytes(filename, original, original_length);
	check_private_files();
	/* Stable machine identity participates when provided by the selected root. */
	path_join(path, sizeof(path), root, "etc"); assert(!mkdir(path, 0700));
	path_join(path, sizeof(path), root, "etc/machine-id");
	write_bytes(path, "0123456789abcdef0123456789abcdef\n", 33);
	assert(!lw_checkpoint_save(filename, &sys, &params, trained));
	make_fresh(); assert(!lw_checkpoint_load(filename, &sys, &params, fresh));
	write_bytes(path, "1123456789abcdef0123456789abcdef\n", 33);
	unchanged_on_failure();
	puts("test_checkpoint: learned-only roundtrip, identity/schema/age validation and private atomic files passed");
	/* Remove the whole unique tree, including directory symlink tests. */
	nftw(root, remove_entry, 32, FTW_DEPTH | FTW_PHYS); directory[0] = 0;
	return 0;
}
