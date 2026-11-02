// SPDX-License-Identifier: MIT
/* Public-interface tests only: no model/controller, BPF or CPU hardware. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "contract.h"

int main(void)
{
	struct lt_demand_rec demand[LW_CONTRACT_MAX_CLUSTERS] = { 0 }, out;
	struct lw_contract_rec advice[LW_CONTRACT_MAX_CLUSTERS] = { 0 };
	struct lw_contract contract = { .lt = demand, .lw = advice,
		.nentries = LW_CONTRACT_MAX_CLUSTERS };
	uint64_t hash = lw_contract_topology_seed(), now = 10000000000ULL;

	for (unsigned int cpu = 0; cpu < 4; cpu++)
		hash = lw_contract_topology_add_cpu(hash, cpu);
	hash = lw_contract_topology_finish(hash, 4);
	assert(hash == 0x8d5e629bcc626c8bULL);
	assert(LW_CONTRACT_VERSION == 2);
	assert(sizeof(advice[0]) == 48 && sizeof(demand[0]) == 64);
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0] = (struct lt_demand_rec){ .seq = 2, .version = LW_CONTRACT_VERSION,
		.pred_util = 512, .cpuperf_max = 800, .cpuperf_mean = 600,
		.lat_crit_milli = 500, .updated_ns = now, .topology_id = hash, .session_id = 17 };
	assert(lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	assert(out.session_id == 17 && out.lat_crit_milli == 500);
	assert(!lw_contract_demand(&contract, 0, hash + 1, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	assert(lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	assert(!lw_contract_demand(&contract, 0, hash, now + LW_CONTRACT_DEMAND_MAX_AGE_NS + 1,
				 UINT64_MAX, &out)); /* callers cannot extend the public maximum age */
	demand[0].updated_ns = now + LW_CONTRACT_PUBLISH_NS + 1;
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].updated_ns = now;
	demand[0].seq = 4;
	assert(lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].seq = 5; /* interrupted publication can use only the bounded previous snapshot */
	assert(lw_contract_demand(&contract, 0, hash, now + 1, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	assert(!lw_contract_demand(&contract, 0, hash, now + LW_CONTRACT_DEMAND_MAX_AGE_NS + 1,
				 LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].seq = 6;
	demand[0].version = 1; /* independent old-version upgrade order: ignore, never reinterpret */
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].version = LW_CONTRACT_VERSION;
	demand[0].session_id = 0;
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].session_id = 23; /* new peer attachment needs new echoed identity */
	assert(lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	assert(out.session_id == 23);
	demand[0].lat_crit_milli = 1001;
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].lat_crit_milli = 0;
	demand[0].cpuperf_mean = demand[0].cpuperf_max + 1;
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	demand[0].cpuperf_mean = 0;
	demand[0].reserved = 1;
	assert(!lw_contract_demand(&contract, 0, hash, now, LW_CONTRACT_DEMAND_MAX_AGE_NS, &out));
	assert(!lw_contract_demand(&contract, LW_CONTRACT_MAX_CLUSTERS, hash, now, 1, &out));

	struct lw_contract_rec record = { .ceiling_cap = 700, .floor_cap = 300,
		.energy_price_milli = 400, .valid_until_ns = now + LW_CONTRACT_TTL_NS,
		.topology_id = hash, .session_id = 23 };
	lw_contract_publish(&contract, 0, &record);
	assert(advice[0].seq == 2 && advice[0].version == LW_CONTRACT_VERSION);
	assert(lw_contract_constraint_valid(&advice[0], hash, 23, now));
	assert(!lw_contract_constraint_valid(&advice[0], hash, 17, now));
	assert(!lw_contract_constraint_valid(&advice[0], hash, 23, now + LW_CONTRACT_TTL_NS + 1));
	advice[0].seq = 3;
	lw_contract_publish(&contract, 0, &record);
	assert(advice[0].seq == 6); /* previous writer died mid-publication */

	puts("test_contract: ABI2 identity, independent peer versions, bounded freshness and recovery passed");
	return 0;
}
