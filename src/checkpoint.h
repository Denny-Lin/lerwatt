/* SPDX-License-Identifier: MIT */
#ifndef LERWATT_CHECKPOINT_H
#define LERWATT_CHECKPOINT_H

struct lw_system;
struct lw_ctl_params;
struct lw_ctl_state;

/* Load only after the first fresh controller step initialized every policy.
 * Only learned thermal coefficients/covariance and long-term hot-spot gain
 * are restored. Control settings, filters, clocks, fitting intervals and
 * pending point forecasts remain fresh. A failed load changes no state.
 * Returns 0 on success, -ENOENT when absent, or another negative errno.
 */
int lw_checkpoint_load(const char *path, const struct lw_system *sys,
		       const struct lw_ctl_params *params, struct lw_ctl_state *states);

/* Parent must already be an owned private directory. An untrained save is a
 * no-op; an existing unrelated/corrupt file is never overwritten. The file
 * is private, versioned, identity-bound, checksummed and atomically replaced.
 */
int lw_checkpoint_save(const char *path, const struct lw_system *sys,
		       const struct lw_ctl_params *params, const struct lw_ctl_state *states);

#endif
