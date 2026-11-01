// SPDX-License-Identifier: MIT
/*
 * sysroot-relative file access and the restore journal.
 *
 * The journal records the original value of every kernel knob before Lerwatt
 * first writes it. A clean exit writes the values back and deletes the
 * journal; after a crash, `lerwattd --restore` does the same from the file.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lerwatt/hal.h"

#define JOURNAL_REL	"/run/lerwatt/restore.journal"
#define JOURNAL_MAX	256

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
	ssize_t n;
	int fd;

	if (!len)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
		buf[--n] = '\0';
	return 0;
}

int lw_read_u64(const char *path, uint64_t *val)
{
	char buf[64], *end;
	int ret = lw_read_str(path, buf, sizeof(buf));

	if (ret)
		return ret;
	errno = 0;
	*val = strtoull(buf, &end, 0);
	return (errno || end == buf) ? -EINVAL : 0;
}

int lw_read_s64(const char *path, int64_t *val)
{
	char buf[64], *end;
	int ret = lw_read_str(path, buf, sizeof(buf));

	if (ret)
		return ret;
	errno = 0;
	*val = strtoll(buf, &end, 0);
	return (errno || end == buf) ? -EINVAL : 0;
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
	if (mkdir(path, 0755) && errno != EEXIST)
		return -errno;
	return 0;
}

static int journal_file(const struct lw_system *sys, char *out, size_t len)
{
	return lw_path(sys, out, len, "%s", JOURNAL_REL);
}

int lw_journal_open(const struct lw_system *sys)
{
	char path[LW_PATH_MAX], *slash;
	int ret = journal_file(sys, path, sizeof(path));

	if (ret)
		return ret;
	slash = strrchr(path, '/');
	*slash = '\0';
	return mkdir_p(path);
}

static int journal_flush(const struct lw_system *sys)
{
	char path[LW_PATH_MAX], tmp[LW_PATH_MAX + 8];
	FILE *f;
	int ret = journal_file(sys, path, sizeof(path));

	if (ret)
		return ret;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return -errno;
	for (int i = 0; i < journal_len; i++)
		fprintf(f, "%s\t%s\n", journal[i].path, journal[i].value);
	if (fflush(f) || fsync(fileno(f))) {
		ret = -errno;
		fclose(f);
		return ret;
	}
	fclose(f);
	return rename(tmp, path) ? -errno : 0;
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
	ret = lw_read_str(path, val, sizeof(val));
	if (ret)
		return ret;
	snprintf(journal[journal_len].path, sizeof(journal[0].path), "%s", path);
	snprintf(journal[journal_len].value, sizeof(journal[0].value), "%s", val);
	journal_len++;
	return journal_flush(sys);
}

/*
 * pm_qos_resume_latency_us reads back "0" for "no constraint" and "n/a" for a
 * zero-latency constraint, and accepts the same strings, so the saved text can
 * be written back verbatim. cpufreq limits read back the effective value; see
 * docs/SIMULATION.md for the caveat when Lerwatt starts while throttled.
 */
int lw_journal_restore(const struct lw_system *sys)
{
	char path[LW_PATH_MAX], line[LW_PATH_MAX + 80];
	int ret = journal_file(sys, path, sizeof(path)), failures = 0;
	FILE *f;

	if (ret)
		return ret;
	f = fopen(path, "r");
	if (!f)
		return errno == ENOENT ? 0 : -errno;
	while (fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t'), *nl;

		if (!tab)
			continue;
		*tab = '\0';
		nl = strchr(tab + 1, '\n');
		if (nl)
			*nl = '\0';
		if (lw_write_str(line, tab + 1)) {
			fprintf(stderr, "lerwatt: restore failed: %s <- %s\n", line, tab + 1);
			failures++;
		}
	}
	fclose(f);
	if (failures)
		return -EIO;
	journal_len = 0;
	return unlink(path) ? -errno : 0;
}
