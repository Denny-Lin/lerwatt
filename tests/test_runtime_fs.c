// SPDX-License-Identifier: MIT
/* Runtime safety checks operate only on an isolated temporary sysroot. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lerwatt/hal.h"
#include "runtime.h"

#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "test_runtime_fs: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

static char test_root[LW_PATH_MAX];

static void remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *entry;

	if (lstat(path, &st))
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path);
		return;
	}
	dir = opendir(path);
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		char child[LW_PATH_MAX + 256];

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		remove_tree(child);
	}
	closedir(dir);
	rmdir(path);
}

static void cleanup(void)
{
	if (test_root[0])
		remove_tree(test_root);
}

static void create_file(const char *path, const char *text)
{
	char parent[LW_PATH_MAX];
	int fd;
	size_t len = strlen(text);

	snprintf(parent, sizeof(parent), "%s", path);
	for (char *p = parent + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(parent, 0700) && errno != EEXIST) {
			perror("mkdir fixture");
			exit(1);
		}
		*p = '/';
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, text, len) != (ssize_t)len || close(fd)) {
		perror("write fixture");
		exit(1);
	}
}

static bool rejects_fifo(const struct lw_system *sys, const char *knob, bool restore)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return false;
	if (!child) {
		int ret;

		alarm(1); /* A regression must fail the test instead of hanging it. */
		ret = restore ? lw_journal_restore(sys) : lw_journal_save(sys, knob);
		_exit(ret < 0 ? 0 : 1);
	}
	return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

int main(void)
{
	static struct lw_system sys;
	struct lw_actuator actuator;
	struct lw_actuation action = { 0 };
	struct stat st, lock_st;
	char max_path[LW_PATH_MAX], min_path[LW_PATH_MAX], lat_path[LW_PATH_MAX];
	char journal_path[LW_PATH_MAX], tmp_path[LW_PATH_MAX], lock_path[LW_PATH_MAX];
	char numeric_path[LW_PATH_MAX], buf[64], contents[2 * LW_PATH_MAX + 160];
	char template[] = "/tmp/lerwatt-runtime-XXXXXX";
	uint64_t unsigned_value = 99;
	int64_t signed_value = 99;
	int lock_fd, second, status;
	pid_t child;

	CHECK(mkdtemp(template), "cannot create temporary sysroot");
	snprintf(test_root, sizeof(test_root), "%s", template);
	atexit(cleanup);
	snprintf(sys.sysroot, sizeof(sys.sysroot), "%s", test_root);
	sys.npolicies = 1;
	sys.ncpus = 1;
	sys.policies[0] = (struct lw_policy) {
		.id = 0, .ncpus = 1, .cpus = { 0 }, .nopps = 2,
		.cpuinfo_min_khz = 1000, .cpuinfo_max_khz = 2000,
		.opps = { { .khz = 1000 }, { .khz = 2000 } },
	};
	CHECK(!lw_path(&sys, max_path, sizeof(max_path), "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"), "max path");
	CHECK(!lw_path(&sys, min_path, sizeof(min_path), "/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"), "min path");
	CHECK(!lw_path(&sys, lat_path, sizeof(lat_path), "/sys/devices/system/cpu/cpu0/power/pm_qos_resume_latency_us"), "latency path");
	CHECK(!lw_path(&sys, journal_path, sizeof(journal_path), "/run/lerwatt/restore.journal"), "journal path");
	CHECK(!lw_path(&sys, tmp_path, sizeof(tmp_path), "/run/lerwatt/restore.journal.tmp"), "temporary journal path");
	CHECK(!lw_path(&sys, lock_path, sizeof(lock_path), "/run/lerwatt/instance.lock"), "lock path");
	CHECK(!lw_path(&sys, numeric_path, sizeof(numeric_path), "/numeric"), "numeric path");
	create_file(max_path, "2000\n");
	create_file(min_path, "1000\n");
	create_file(lat_path, "0\n");

	create_file(numeric_path, "123junk\n");
	CHECK(lw_read_u64(numeric_path, &unsigned_value) < 0 && unsigned_value == 99, "unsigned reader accepted trailing junk");
	create_file(numeric_path, "-1\n");
	CHECK(lw_read_u64(numeric_path, &unsigned_value) < 0, "unsigned reader accepted a negative number");
	CHECK(!lw_read_s64(numeric_path, &signed_value) && signed_value == -1, "signed reader lost negative values");
	create_file(numeric_path, "18446744073709551616\n");
	CHECK(lw_read_u64(numeric_path, &unsigned_value) < 0, "unsigned reader accepted overflow");
	create_file(numeric_path, "123456789\n");
	CHECK(lw_read_str(numeric_path, buf, 4) == -EOVERFLOW, "string reader silently truncated a value");
	create_file(numeric_path, "42\n");
	CHECK(!lw_read_u64(numeric_path, &unsigned_value) && unsigned_value == 42, "unsigned reader rejected valid sysfs text");
	create_file(max_path, "2000junk\n");
	CHECK(lw_actuator_init(&sys, &actuator, false) < 0, "actuator guessed an unreadable original cap");
	create_file(max_path, "2000\n");

	lock_fd = lw_instance_lock(&sys);
	CHECK(lock_fd >= 0, "cannot acquire first instance lock");
	CHECK(!fstat(lock_fd, &lock_st) && (lock_st.st_mode & 0777) == 0600, "lock is not private");
	CHECK(!lw_path(&sys, buf, sizeof(buf), "/run/lerwatt"), "private runtime dir path");
	CHECK(!stat(buf, &st) && (st.st_mode & 0777) == 0700, "runtime directory is not private");
	second = lw_instance_lock(&sys);
	CHECK(second == -EWOULDBLOCK || second == -EAGAIN, "second instance acquired a live lock");
	child = fork();
	CHECK(child >= 0, "fork failed");
	if (!child) {
		int fd = lw_instance_lock(&sys);

		if (fd >= 0)
			close(fd);
		_exit(fd == -EWOULDBLOCK || fd == -EAGAIN ? 0 : 1);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "another process acquired a live lock");
	close(lock_fd);
	lock_fd = lw_instance_lock(&sys);
	CHECK(lock_fd >= 0 && !fstat(lock_fd, &st) && st.st_ino == lock_st.st_ino, "lock release replaced its persistent inode");

	CHECK(!lw_actuator_init(&sys, &actuator, false), "actuator initialization failed");
	action.max_khz[0] = 1500;
	action.min_khz[0] = 1200;
	action.resume_latency_us[0] = 50;
	CHECK(!mkdir(tmp_path, 0700), "cannot inject journal failure");
	CHECK(lw_actuate(&sys, &actuator, &action) < 0, "journal failure did not stop actuation");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 2000, "max changed without a persisted journal");
	CHECK(!lw_read_u64(min_path, &unsigned_value) && unsigned_value == 1000, "actuation continued after an earlier error");
	CHECK(!lw_exists(journal_path), "failed journal was published");
	CHECK(!rmdir(tmp_path), "cannot remove injected journal fault");
	CHECK(!lw_actuate(&sys, &actuator, &action), "journal append was not retried after persistence failure");
	CHECK(!stat(journal_path, &st) && (st.st_mode & 0777) == 0600, "journal is not private");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 1500, "valid actuation did not write its cap");
	CHECK(!lw_journal_restore(&sys), "restoration failed");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 2000, "cap original was lost after a journal retry");
	CHECK(!lw_read_u64(min_path, &unsigned_value) && unsigned_value == 1000, "floor original was lost");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "0"), "PM QoS original was lost");
	CHECK(!lw_exists(journal_path), "clean restoration left a journal");
	CHECK(!mkfifo(journal_path, 0600), "cannot create recovery FIFO fault");
	CHECK(rejects_fifo(&sys, max_path, true), "recovery FIFO blocked before file type validation");
	CHECK(!unlink(journal_path), "cannot remove recovery FIFO fault");
	CHECK(!mkfifo(tmp_path, 0600), "cannot create temporary journal FIFO fault");
	CHECK(rejects_fifo(&sys, max_path, false), "temporary journal FIFO blocked before file type validation");
	CHECK(!unlink(tmp_path), "cannot remove temporary journal FIFO fault");

	/* Capping below an original floor journals min before max. Recovery
	 * must restore the ceiling first so a real CPUFreq driver can accept it.
	 */
	create_file(min_path, "1800\n");
	CHECK(!lw_actuator_init(&sys, &actuator, false), "high-floor actuator initialization failed");
	action.min_khz[0] = 0;
	action.max_khz[0] = 1500;
	action.resume_latency_us[0] = -1;
	CHECK(!lw_actuate(&sys, &actuator, &action), "cap below original floor failed");
	CHECK(!lw_read_u64(min_path, &unsigned_value) && unsigned_value == 1500, "floor was not lowered before its ceiling");
	CHECK(!lw_journal_restore(&sys), "high-floor restoration failed");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 2000, "high-floor cap original was lost");
	CHECK(!lw_read_u64(min_path, &unsigned_value) && unsigned_value == 1800, "high-floor original was lost");
	/* Aliasing two fixture knobs makes write order observable without any
	 * test hooks: max then min must leave the shared inode containing 1800.
	 */
	CHECK(!unlink(min_path) && !link(max_path, min_path), "cannot create restore-order observer");
	snprintf(contents, sizeof(contents), "%s\t1800\n%s\t2000\n", min_path, max_path);
	create_file(journal_path, contents);
	CHECK(!lw_journal_restore(&sys), "ordered restoration failed");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 1800, "recovery restored a floor before its ceiling");
	CHECK(!unlink(min_path), "cannot remove restore-order observer");
	create_file(max_path, "2000\n");
	create_file(min_path, "1000\n");
	action.min_khz[0] = 1200;
	action.resume_latency_us[0] = 50;

	/* A valid first line must not be applied when a later record is malformed. */
	snprintf(contents, sizeof(contents), "%s\t1900\nmalformed record\n", max_path);
	create_file(journal_path, contents);
	CHECK(lw_journal_restore(&sys) < 0, "malformed journal was accepted");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 2000, "malformed journal partially restored a knob");
	CHECK(lw_exists(journal_path), "malformed recovery evidence was deleted");
	snprintf(contents, sizeof(contents), "%s\t0\n", numeric_path);
	create_file(journal_path, contents);
	CHECK(lw_journal_restore(&sys) < 0 && lw_exists(journal_path), "journal could target an unrelated file");
	CHECK(!unlink(journal_path), "cannot clear malformed fixture");

	/* A failed knob write retains recovery data and stops before other writes. */
	CHECK(!lw_actuator_init(&sys, &actuator, false), "second actuator initialization failed");
	CHECK(!lw_journal_save(&sys, max_path), "cannot prepare a persisted original");
	CHECK(!unlink(max_path) && !mkdir(max_path, 0700), "cannot inject knob write failure");
	CHECK(lw_actuate(&sys, &actuator, &action) < 0, "knob write failure was ignored");
	CHECK(!lw_read_u64(min_path, &unsigned_value) && unsigned_value == 1000, "other knobs changed after a write failure");
	CHECK(lw_journal_restore(&sys) < 0 && lw_exists(journal_path), "failed restore deleted its journal");
	CHECK(!rmdir(max_path), "cannot clear knob fault");
	create_file(max_path, "1500");
	CHECK(!lw_journal_restore(&sys), "retrying restoration failed");
	CHECK(!lw_read_u64(max_path, &unsigned_value) && unsigned_value == 2000, "retrying restoration lost the original cap");

	/* Preserve the special zero-latency PM QoS string verbatim. */
	create_file(lat_path, "n/a\n");
	CHECK(!lw_actuator_init(&sys, &actuator, false), "n/a PM QoS was rejected");
	action.resume_latency_us[0] = 25;
	CHECK(!lw_actuate(&sys, &actuator, &action) && !lw_journal_restore(&sys), "n/a PM QoS round trip failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "n/a"), "n/a PM QoS was not restored verbatim");

	/* A new latency budget may tighten, but must never relax, an existing
	 * request from the user or another power manager.
	 */
	create_file(lat_path, "30\n");
	CHECK(!lw_actuator_init(&sys, &actuator, false), "constrained PM QoS initialization failed");
	action.resume_latency_us[0] = 200;
	CHECK(!lw_actuate(&sys, &actuator, &action), "constrained PM QoS actuation failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "30"), "PM QoS relaxed an original 30 us constraint");
	action.resume_latency_us[0] = 10;
	CHECK(!lw_actuate(&sys, &actuator, &action), "stricter PM QoS request failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "10"), "PM QoS rejected a stricter 10 us request");
	action.resume_latency_us[0] = 0;
	CHECK(!lw_actuate(&sys, &actuator, &action), "zero-latency PM QoS request failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "n/a"), "zero-latency request did not use kernel n/a semantics");
	CHECK(!lw_journal_restore(&sys), "constrained PM QoS restoration failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "30"), "PM QoS did not restore the original 30 us request");
	create_file(lat_path, "n/a\n");
	CHECK(!lw_actuator_init(&sys, &actuator, false), "zero-latency PM QoS initialization failed");
	action.resume_latency_us[0] = 200;
	CHECK(!lw_actuate(&sys, &actuator, &action), "original zero-latency actuation failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "n/a"), "PM QoS relaxed an original n/a constraint");
	CHECK(!lw_journal_restore(&sys), "original zero-latency restoration failed");
	CHECK(!lw_read_str(lat_path, buf, sizeof(buf)) && !strcmp(buf, "n/a"), "PM QoS lost the original n/a request");

	/* A root process must not follow a planted journal temporary symlink. */
	CHECK(!symlink(numeric_path, tmp_path), "cannot create a symlink fault");
	CHECK(lw_journal_save(&sys, max_path) < 0, "journal followed a temporary symlink");
	CHECK(!lw_read_u64(numeric_path, &unsigned_value) && unsigned_value == 42, "temporary symlink target was overwritten");
	CHECK(!unlink(tmp_path), "cannot remove temporary symlink fault");

	/* The journal must fit every supported CPU QoS request and both limits
	 * of every supported policy, rather than an unrelated fixed constant.
	 */
	for (int i = 0; i < LW_MAX_POLICIES; i++) {
		for (int maximum = 0; maximum < 2; maximum++) {
			char path[LW_PATH_MAX];

			CHECK(!lw_path(&sys, path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_%s_freq", i, maximum ? "max" : "min"), "capacity frequency path");
			create_file(path, maximum ? "2000\n" : "1000\n");
			CHECK(!lw_journal_save(&sys, path), "journal cannot fit all supported policy limits");
		}
	}
	for (int cpu = 0; cpu < LW_MAX_CPUS; cpu++) {
		char path[LW_PATH_MAX];

		CHECK(!lw_path(&sys, path, sizeof(path), "/sys/devices/system/cpu/cpu%d/power/pm_qos_resume_latency_us", cpu), "capacity QoS path");
		create_file(path, "0\n");
		CHECK(!lw_journal_save(&sys, path), "journal cannot fit all supported CPU QoS requests");
	}
	CHECK(!lw_journal_restore(&sys) && !lw_exists(journal_path), "full-capacity journal did not restore");
	close(lock_fd);
	puts("test_runtime_fs: ok (strict reads, lock lifetime, journal faults, restore validation)");
	return 0;
}
