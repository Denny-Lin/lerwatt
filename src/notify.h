/* SPDX-License-Identifier: MIT */
#ifndef LERWATT_NOTIFY_H
#define LERWATT_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>

struct lw_notify {
	int fd;
	uint64_t interval_ns;
	uint64_t last_ns;
	bool ready;
	bool status_dirty;
	char status[160];
};

/* Explicit service mode only. Validate systemd's socket/watchdog environment;
 * notifications never run in a separate thread that could mask a stuck loop.
 * Return 0 or negative errno. The caller closes the notifier on every exit. */
int lw_notify_init(struct lw_notify *notify, unsigned int period_ms);
int lw_notify_progress(struct lw_notify *notify, uint64_t now_ns);
/* Stored until a successful control-cycle progress call, never a heartbeat
 * from a separate status thread. */
void lw_notify_set_status(struct lw_notify *notify, const char *status);
void lw_notify_close(struct lw_notify *notify);

#endif
