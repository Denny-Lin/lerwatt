// SPDX-License-Identifier: MIT
/* Minimal systemd notification protocol; no libsystemd runtime dependency. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "notify.h"

static int unsigned_decimal(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	if (!text || !text[0])
		return -EINVAL;
	for (const char *p = text; *p; p++)
		if (*p < '0' || *p > '9')
			return -EINVAL;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end || !parsed)
		return -EINVAL;
	*value = (uint64_t)parsed;
	return 0;
}

int lw_notify_init(struct lw_notify *notify, unsigned int period_ms)
{
	struct sockaddr_un destination = { .sun_family = AF_UNIX };
	const char *path = getenv("NOTIFY_SOCKET");
	const char *pid = getenv("WATCHDOG_PID");
	uint64_t usec, expected_pid;
	size_t length;
	socklen_t address_length;
	int fd, error;

	memset(notify, 0, sizeof(*notify));
	notify->fd = -1;
	if (!path || !path[0] || (path[0] != '/' && path[0] != '@') ||
	    unsigned_decimal(getenv("WATCHDOG_USEC"), &usec) ||
	    usec > UINT64_MAX / 1000 || usec < 100000)
		return -EINVAL;
	if (pid && (unsigned_decimal(pid, &expected_pid) || expected_pid != (uint64_t)getpid()))
		return -EINVAL;
	notify->interval_ns = usec * 1000 / 2;
	if (!period_ms || (uint64_t)period_ms * 1000000 >= notify->interval_ns)
		return -EINVAL;
	length = strlen(path);
	if (length >= sizeof(destination.sun_path))
		return -ENAMETOOLONG;
	memcpy(destination.sun_path, path, length + 1);
	address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
	if (path[0] == '@') {
#ifdef __linux__
		destination.sun_path[0] = '\0';
		address_length--;
#else
		return -EAFNOSUPPORT;
#endif
	}
	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK) ||
	    connect(fd, (const struct sockaddr *)&destination, address_length)) {
		error = -errno;
		close(fd);
		return error;
	}
	notify->fd = fd;
	return 0;
}

static int send_notification(int fd, const char *text)
{
	size_t length = strlen(text);
	ssize_t sent;

	do {
		sent = send(fd, text, length, 0);
	} while (sent < 0 && errno == EINTR);
	return sent < 0 ? -errno : (size_t)sent == length ? 0 : -EIO;
}

int lw_notify_progress(struct lw_notify *notify, uint64_t now_ns)
{
	int error;

	if (notify->fd < 0)
		return -EINVAL;
	if (notify->ready && now_ns >= notify->last_ns &&
	    now_ns - notify->last_ns < notify->interval_ns)
		return 0;
	error = send_notification(notify->fd, notify->ready ?
		"WATCHDOG=1\nSTATUS=Controlling CPU limits" :
		"READY=1\nWATCHDOG=1\nSTATUS=Controlling CPU limits");
	if (!error) {
		notify->ready = true;
		notify->last_ns = now_ns;
	}
	return error;
}

void lw_notify_close(struct lw_notify *notify)
{
	if (notify->fd >= 0) {
		if (notify->ready)
			(void)send_notification(notify->fd, "STOPPING=1\nSTATUS=Restoring CPU settings");
		close(notify->fd);
		notify->fd = -1;
	}
}
