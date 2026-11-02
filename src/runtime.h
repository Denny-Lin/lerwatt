/* SPDX-License-Identifier: MIT */
#ifndef LERWATT_RUNTIME_H
#define LERWATT_RUNTIME_H

struct lw_system;

/* Nonblocking, sysroot-scoped lock. Hold the returned fd through restoration;
 * close it to release ownership. Never unlink the persistent lock inode.
 * Returns a file descriptor >= 0 or a negative errno value.
 */
int lw_instance_lock(const struct lw_system *sys);

#endif /* LERWATT_RUNTIME_H */
