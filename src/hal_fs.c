// SPDX-License-Identifier: MIT
/*
 * sysroot-relative file access and the restore journal.
 *
 * The journal records the original value of every kernel knob before Lerwatt
 * first writes it. A clean exit writes the values back and deletes the
 * journal; after a crash, `lerwattd --restore` does the same from the file.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lerwatt/hal.h"
#include "runtime.h"

#define JOURNAL_REL	"/run/lerwatt/restore.journal"
#define JOURNAL_MAX	(2 * LW_MAX_POLICIES + LW_MAX_CPUS)

struct journal_entry {
	char path[LW_PATH_MAX];
	char value[64];
};

static struct journal_entry journal[JOURNAL_MAX];
static int journal_len;

uint64_t lw_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int lw_path(const struct lw_system *sys, char *out, size_t len, const char *fmt, ...)
{
	char rel[LW_PATH_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(rel, sizeof(rel), fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(rel))
		return -ENAMETOOLONG;

	if (!sys->sysroot[0] || !strcmp(sys->sysroot, "/"))
		n = snprintf(out, len, "%s", rel);
	else
		n = snprintf(out, len, "%s%s", sys->sysroot, rel);
	return (n < 0 || (size_t)n >= len) ? -ENAMETOOLONG : 0;
}

bool lw_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

int lw_read_str(const char *path, char *buf, size_t len)
{
	size_t used = 0;
	ssize_t n;
	char extra;
	int fd;

	if (!len)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	while (used < len - 1) {
		n = read(fd, buf + used, len - 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int ret = -errno;

			close(fd);
			return ret;
		}
		if (!n)
			break;
		used += (size_t)n;
	}
	do {
		n = read(fd, &extra, 1);
	} while (n < 0 && errno == EINTR);
	int ret = n < 0 ? -errno : n ? -EOVERFLOW : 0;

	close(fd);
	if (ret)
		return ret;
	if (memchr(buf, '\0', used))
		return -EINVAL;
	buf[used] = '\0';
	while (used > 0 && isspace((unsigned char)buf[used - 1]))
		buf[--used] = '\0';
	return 0;
}

int lw_read_u64(const char *path, uint64_t *val)
{
	char buf[64], *end;
	unsigned long long parsed;
	int ret = lw_read_str(path, buf, sizeof(buf));

	if (ret)
		return ret;
	errno = 0;
	char *start = buf;

	while (isspace((unsigned char)*start))
		start++;
	if (*start == '-')
		return -EINVAL;
	parsed = strtoull(start, &end, 0);
	if (errno || end == start || *end)
		return -EINVAL;
	*val = (uint64_t)parsed;
	return 0;
}

int lw_read_s64(const char *path, int64_t *val)
{
	char buf[64], *end;
	long long parsed;
	int ret = lw_read_str(path, buf, sizeof(buf));

	if (ret)
		return ret;
	errno = 0;
	parsed = strtoll(buf, &end, 0);
	if (errno || end == buf || *end)
		return -EINVAL;
	*val = (int64_t)parsed;
	return 0;
}

int lw_write_str(const char *path, const char *val)
{
	size_t len = strlen(val);
	ssize_t n;
	int fd;

	/* O_TRUNC is ignored by sysfs and keeps simulator files clean. */
	fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = write(fd, val, len);
	if (n < 0) {
		int err = -errno;

		close(fd);
		return err;
	}
	if (close(fd))
		return -errno;
	return (size_t)n == len ? 0 : -EIO;
}

static int mkdir_p(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) && errno != EEXIST) {
			*p = '/';
			return -errno;
		}
		*p = '/';
	}
	if (mkdir(path, 0700) && errno != EEXIST)
		return -errno;
	return 0;
}

static int journal_file(const struct lw_system *sys, char *out, size_t len)
{
	return lw_path(sys, out, len, "%s", JOURNAL_REL);
}

static int journal_dir(const struct lw_system *sys, bool create)
{
	char path[LW_PATH_MAX], *slash;
	struct stat st;
	int ret = journal_file(sys, path, sizeof(path));
	int fd;

	if (ret)
		return ret;
	slash = strrchr(path, '/');
	*slash = '\0';
	if (create && (ret = mkdir_p(path)))
		return ret;
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		ret = -errno;
		close(fd);
		return ret;
	}
	if (st.st_uid != geteuid()) {
		close(fd);
		return -EACCES;
	}
	if (create && fchmod(fd, 0700)) {
		ret = -errno;
		close(fd);
		return ret;
	}
	if (st.st_mode & 0077) {
		/* A restore-only reader must not trust an exposed recovery directory. */
		if (!create) {
			close(fd);
			return -EACCES;
		}
	}
	return fd;
}

static int private_file(int dirfd, const char *name, int flags)
{
	struct stat st;
	/* Reject special files after opening without letting a planted FIFO
	 * block before fstat can inspect its type. Regular files ignore this flag.
	 */
	int fd = openat(dirfd, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	int ret;

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		ret = -errno;
		close(fd);
		return ret;
	}
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
		close(fd);
		return -EACCES;
	}
	if (fchmod(fd, 0600)) {
		ret = -errno;
		close(fd);
		return ret;
	}
	return fd;
}

int lw_journal_open(const struct lw_system *sys)
{
	int fd = journal_dir(sys, true);

	if (fd < 0)
		return fd;
	close(fd);
	return 0;
}

int lw_instance_lock(const struct lw_system *sys)
{
	int dirfd = journal_dir(sys, true), fd, ret;

	if (dirfd < 0)
		return dirfd;
	fd = private_file(dirfd, "instance.lock", O_RDWR | O_CREAT);
	close(dirfd);
	if (fd < 0)
		return fd;
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		ret = -errno;
		close(fd);
		return ret;
	}
	return fd;
}

static int journal_flush(const struct lw_system *sys)
{
	int dirfd = journal_dir(sys, true), fd, ret = 0;
	FILE *f;

	if (dirfd < 0)
		return dirfd;
	fd = private_file(dirfd, "restore.journal.tmp", O_WRONLY | O_CREAT);
	if (fd < 0) {
		close(dirfd);
		return fd;
	}
	if (ftruncate(fd, 0)) {
		ret = -errno;
		close(fd);
		close(dirfd);
		return ret;
	}
	f = fdopen(fd, "w");
	if (!f) {
		ret = -errno;
		close(fd);
		close(dirfd);
		return ret;
	}
	for (int i = 0; i < journal_len; i++)
		if (fprintf(f, "%s\t%s\n", journal[i].path, journal[i].value) < 0)
			ret = -EIO;
	if (fflush(f) || fsync(fileno(f)))
		ret = -errno;
	if (fclose(f) && !ret)
		ret = -errno;
	if (!ret && renameat(dirfd, "restore.journal.tmp", dirfd, "restore.journal"))
		ret = -errno;
	if (!ret && fsync(dirfd))
		ret = -errno;
	close(dirfd);
	return ret;
}

static bool journal_value_valid(const struct lw_system *sys, const char *path, const char *value)
{
	char prefix[LW_PATH_MAX];
	const char *suffix, *number;
	bool latency = false;
	char *end;
	unsigned long long v;

	if (lw_path(sys, prefix, sizeof(prefix), "/sys/devices/system/cpu/"))
		return false;
	if (strncmp(path, prefix, strlen(prefix)))
		return false;
	suffix = path + strlen(prefix);
	if (!strncmp(suffix, "cpufreq/policy", 14)) {
		number = suffix + 14;
	} else if (!strncmp(suffix, "cpu", 3)) {
		number = suffix + 3;
		latency = true;
	} else {
		return false;
	}
	if (!isdigit((unsigned char)*number))
		return false;
	while (isdigit((unsigned char)*number))
		number++;
	if (latency) {
		if (strcmp(number, "/power/pm_qos_resume_latency_us"))
			return false;
		if (!strcmp(value, "n/a"))
			return true;
	} else if (strcmp(number, "/scaling_min_freq") && strcmp(number, "/scaling_max_freq")) {
		return false;
	}
	if (!isdigit((unsigned char)*value))
		return false;
	errno = 0;
	v = strtoull(value, &end, 10);
	return !errno && !*end && (latency ? v <= INT32_MAX : v > 0 && v <= UINT32_MAX);
}

int lw_journal_save(const struct lw_system *sys, const char *path)
{
	char val[64];
	int ret;

	for (int i = 0; i < journal_len; i++)
		if (!strcmp(journal[i].path, path))
			return 0;	/* keep the first, original value */
	if (journal_len == JOURNAL_MAX)
		return -ENOSPC;
	if (strlen(path) >= sizeof(journal[0].path) || strchr(path, '\t') || strchr(path, '\n'))
		return -EINVAL;
	ret = lw_read_str(path, val, sizeof(val));
	if (ret)
		return ret;
	if (!journal_value_valid(sys, path, val))
		return -EINVAL;
	snprintf(journal[journal_len].path, sizeof(journal[0].path), "%s", path);
	snprintf(journal[journal_len].value, sizeof(journal[0].value), "%s", val);
	journal_len++;
	ret = journal_flush(sys);
	if (ret)
		journal_len--; /* An unpersisted entry must be retried before any write. */
	return ret;
}

/*
 * pm_qos_resume_latency_us reads back "0" for "no constraint" and "n/a" for a
 * zero-latency constraint, and accepts the same strings, so the saved text can
 * be written back verbatim. cpufreq limits read back the effective value; see
 * docs/SIMULATION.md for the caveat when Lerwatt starts while throttled.
 */
int lw_journal_restore(const struct lw_system *sys)
{
	char line[LW_PATH_MAX + 80];
	struct journal_entry *entries;
	int dirfd = journal_dir(sys, false), fd, ret = 0, count = 0;
	FILE *f;

	if (dirfd < 0)
		return dirfd == -ENOENT ? 0 : dirfd;
	fd = private_file(dirfd, "restore.journal", O_RDONLY);
	if (fd < 0) {
		close(dirfd);
		return fd == -ENOENT ? 0 : fd;
	}
	f = fdopen(fd, "r");
	entries = calloc(JOURNAL_MAX, sizeof(*entries));
	if (!f || !entries) {
		ret = -ENOMEM;
		if (f)
			fclose(f);
		else
			close(fd);
		free(entries);
		close(dirfd);
		return ret;
	}
	/* Validate the entire file before restoring any knob. */
	while (fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t'), *nl;

		nl = strchr(line, '\n');
		if (!tab || !nl || tab == line || tab + 1 == nl ||
		    strchr(tab + 1, '\t') || count == JOURNAL_MAX) {
			ret = -EINVAL;
			break;
		}
		*tab = '\0';
		*nl = '\0';
		if (line[0] != '/' || strlen(line) >= sizeof(entries[0].path) ||
		    strlen(tab + 1) >= sizeof(entries[0].value) ||
		    !journal_value_valid(sys, line, tab + 1)) {
			ret = -EINVAL;
			break;
		}
		for (int i = 0; i < count; i++)
			if (!strcmp(entries[i].path, line))
				ret = -EINVAL;
		if (ret)
			break;
		strcpy(entries[count].path, line);
		strcpy(entries[count].value, tab + 1);
		count++;
	}
	if (ferror(f))
		ret = -EIO;
	if (fclose(f) && !ret)
		ret = -errno;
	if (!ret && !count)
		ret = -EINVAL;
	if (!ret) {
		/* A low cap can first lower the original floor, journaling min
		 * before max. Raise ceilings back before restoring those floors;
		 * journal insertion order is not a valid CPUFreq restore order.
		 */
		for (int phase = 0; phase < 3; phase++) {
			for (int i = 0; i < count; i++) {
				const char *name = strrchr(entries[i].path, '/') + 1;
				int entry_phase = !strcmp(name, "scaling_max_freq") ? 0 :
					!strcmp(name, "scaling_min_freq") ? 1 : 2;

				if (entry_phase != phase)
					continue;
				if (lw_write_str(entries[i].path, entries[i].value)) {
					fprintf(stderr, "lerwatt: restore failed: %s <- %s\n",
						entries[i].path, entries[i].value);
					ret = -EIO;
				}
			}
		}
	}
	free(entries);
	if (!ret && unlinkat(dirfd, "restore.journal", 0))
		ret = -errno;
	if (!ret) {
		journal_len = 0;
		if (fsync(dirfd))
			ret = -errno;
	}
	close(dirfd);
	return ret;
}
