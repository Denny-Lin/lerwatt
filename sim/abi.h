/* SPDX-License-Identifier: MIT */
/*
 * The fake kernel ABI: the same sysfs/procfs/debugfs/hwmon paths a real
 * RK3588 board exposes, under a root directory owned by lerwatt-sim.
 */
#ifndef LERWATT_SIM_ABI_H
#define LERWATT_SIM_ABI_H

#include <stddef.h>

#include "sim.h"

#define SIM_ABI_MAX_DYN	256

struct sim_abi_file {
	char rel[160];
	char last[1024];
};

struct sim_abi {
	char root[384];
	int root_lock_fd;
	int ndyn;
	struct sim_abi_file dyn[SIM_ABI_MAX_DYN];
	unsigned long bad_writes;
};

/* Build the tree. Refuses "/" and directories that are not a simulator root. */
int sim_abi_create(struct sim_abi *a, const char *root, const struct sim_state *s,
		   int force, char *err, size_t errlen);
/* Read the knobs lerwattd writes; apply valid values to the model. */
void sim_abi_ingest(struct sim_abi *a, struct sim_state *s);
/* Write every dynamic file whose content changed. */
void sim_abi_publish(struct sim_abi *a, const struct sim_state *s);
/* Remove the live marker and unlock; retain the root identity for restarts. */
void sim_abi_close(struct sim_abi *a);

#endif /* LERWATT_SIM_ABI_H */
