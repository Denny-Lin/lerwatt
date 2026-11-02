/* SPDX-License-Identifier: MIT */
/* Userspace access to the pinned Lerwatt <-> Lertick contract maps. */
#ifndef LERWATT_CONTRACT_IO_H
#define LERWATT_CONTRACT_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lerwatt/contract.h"

struct lw_contract {
	int lw_fd;
	int lt_fd;
	void *lw_map;			/* the lw_contract mapping itself */
	struct lw_contract_rec *lw;	/* writable alias of lw_map, or NULL */
	const struct lt_demand_rec *lt;	/* mmap, read-only */
	unsigned int nentries;
	unsigned int lw_id, lt_id; /* pinned-object identities, not model versions */
	size_t lw_len;
	size_t lt_len;
	struct lt_demand_rec lt_last[LW_CONTRACT_MAX_CLUSTERS];
	bool lt_valid[LW_CONTRACT_MAX_CLUSTERS];
	uint64_t opened_ns;		/* set by lw_contract_maintain() */
	uint64_t last_fresh_ns;		/* newest fresh demand seen */
	uint64_t next_pin_check_ns;
};

/*
 * Open both maps. Returns 0 on success, -ENOENT when Lertick is not running
 * (the normal case without a scheduler), or another negative errno.
 * want_write=false opens lw_contract read-only (the simulator only reads).
 */
int lw_contract_open(struct lw_contract *c, bool want_write);
void lw_contract_close(struct lw_contract *c);
void lw_contract_publish(struct lw_contract *c, unsigned int cluster,
			 const struct lw_contract_rec *val);
/* Latest consistent, fresh (max_age_ns) demand record; false if none. */
bool lw_contract_demand(struct lw_contract *c, unsigned int cluster, uint64_t topology_id, uint64_t now_ns,
			uint64_t max_age_ns, struct lt_demand_rec *out);

/*
 * Follow independent Lertick starts, stops and upgrades. Each attachment pins
 * new maps. While detached, try to open every retry_ns. While attached,
 * detach once no cluster has reported fresh demand for stale_ns. *next_try_ns
 * is caller state, initially zero. Returns the new attached state; *err_out
 * (optional) gets the last open result.
 */
bool lw_contract_maintain(struct lw_contract *c, bool attached, bool want_write,
			  uint64_t now_ns, uint64_t retry_ns, uint64_t stale_ns,
			  uint64_t *next_try_ns, int *err_out);

#endif /* LERWATT_CONTRACT_IO_H */
