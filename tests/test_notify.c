// SPDX-License-Identifier: MIT
/* Socket-only service protocol tests; never touch CPU controls or systemd. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "notify.h"

static void receive(int fd, char *message, size_t capacity)
{
	ssize_t size = recv(fd, message, capacity - 1, 0);

	assert(size > 0);
	message[size] = '\0';
}

int main(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct lw_notify notify;
	char directory[] = "/tmp/lerwatt-notify-XXXXXX", socket_path[108], message[256];
	int server;

	assert(mkdtemp(directory));
	assert(snprintf(socket_path, sizeof(socket_path), "%s/socket", directory) < (int)sizeof(socket_path));
	strcpy(address.sun_path, socket_path);
	server = socket(AF_UNIX, SOCK_DGRAM, 0);
	assert(server >= 0);
	assert(!bind(server, (struct sockaddr *)&address,
		     (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1)));
	assert(!fcntl(server, F_SETFL, O_NONBLOCK));
	assert(!setenv("NOTIFY_SOCKET", socket_path, 1));
	assert(!unsetenv("WATCHDOG_PID"));
	assert(!unsetenv("WATCHDOG_USEC"));
	assert(lw_notify_init(&notify, 10) == -EINVAL);
	const char *invalid[] = { "0", "-1", "+1000000", "1000000us", "nan", "99999", "18446744073709551615" };

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert(!setenv("WATCHDOG_USEC", invalid[i], 1));
		assert(lw_notify_init(&notify, 10) < 0);
		assert(notify.fd == -1);
	}
	assert(!setenv("WATCHDOG_USEC", "1000000", 1));
	assert(!setenv("WATCHDOG_PID", "2147483647", 1));
	assert(lw_notify_init(&notify, 10) == -EINVAL);
	assert(!unsetenv("WATCHDOG_PID"));
	assert(lw_notify_init(&notify, 500) == -EINVAL);
	assert(!setenv("NOTIFY_SOCKET", "relative-path", 1));
	assert(lw_notify_init(&notify, 10) == -EINVAL);
	assert(!setenv("NOTIFY_SOCKET", socket_path, 1));
	assert(!lw_notify_init(&notify, 10));
	assert(fcntl(notify.fd, F_GETFD) & FD_CLOEXEC);
	assert(fcntl(notify.fd, F_GETFL) & O_NONBLOCK);
	assert(recv(server, message, sizeof(message), 0) < 0 && errno == EAGAIN);
	assert(!lw_notify_progress(&notify, 1000000000));
	receive(server, message, sizeof(message));
	assert(strstr(message, "READY=1\nWATCHDOG=1"));
	assert(notify.ready);
	assert(!lw_notify_progress(&notify, 1250000000));
	assert(recv(server, message, sizeof(message), 0) < 0 && errno == EAGAIN);
	assert(!lw_notify_progress(&notify, 1500000000));
	receive(server, message, sizeof(message));
	assert(strstr(message, "WATCHDOG=1") && !strstr(message, "READY="));
	lw_notify_set_status(&notify, "Controlling CPU limits; Lertick: connected");
	assert(!lw_notify_progress(&notify, 1500000001));
	receive(server, message, sizeof(message));
	assert(strstr(message, "Lertick: connected") && !strstr(message, "READY="));
	lw_notify_set_status(&notify, "invalid\nREADY=1");
	assert(!lw_notify_progress(&notify, 1500000002));
	assert(recv(server, message, sizeof(message), 0) < 0 && errno == EAGAIN);
	lw_notify_close(&notify);
	assert(notify.fd == -1);
	receive(server, message, sizeof(message));
	assert(strstr(message, "STOPPING=1") && !strstr(message, "WATCHDOG="));
	assert(!lw_notify_init(&notify, 10));
	close(server);
	assert(!unlink(socket_path));
	assert(lw_notify_progress(&notify, 1000000000) < 0 && !notify.ready);
	lw_notify_close(&notify);
	assert(!rmdir(directory));
#ifdef __linux__
	/* Linux systemd also uses abstract AF_UNIX addresses. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	assert(snprintf(socket_path, sizeof(socket_path), "@lerwatt-notify-%ld", (long)getpid()) < (int)sizeof(socket_path));
	strcpy(address.sun_path, socket_path);
	address.sun_path[0] = '\0';
	server = socket(AF_UNIX, SOCK_DGRAM, 0);
	assert(server >= 0);
	assert(!bind(server, (struct sockaddr *)&address,
		     (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(socket_path))));
	assert(!setenv("NOTIFY_SOCKET", socket_path, 1));
	assert(!lw_notify_init(&notify, 10));
	assert(!lw_notify_progress(&notify, 1000000000));
	receive(server, message, sizeof(message));
	assert(strstr(message, "READY=1"));
	lw_notify_close(&notify);
	close(server);
#endif
	puts("test_notify: environment validation, readiness, progress heartbeats and socket failure passed");
	return 0;
}
