// SPDX-License-Identifier: MIT
/*
 * Pinned BPF array maps, mapped into userspace. No libbpf: two bpf(2)
 * commands (OBJ_GET, OBJ_GET_INFO_BY_FD) and mmap.
 *
 * The contract is never simulated. In the VM it is Lertick's real map; on a
 * board it is the same map. It lives on bpffs, outside the hardware sysroot.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

#if defined(__linux__) && !defined(LW_NO_BPF)
#include <linux/bpf.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static int bpf_obj_get(const char *path, __u32 flags)
{
	union bpf_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.pathname = (__u64)(uintptr_t)path;
	attr.file_flags = flags;
	return (int)syscall(__NR_bpf, BPF_OBJ_GET, &attr, sizeof(attr));
}

static int bpf_map_info(int fd, struct bpf_map_info *info)
{
	union bpf_attr attr;

	memset(&attr, 0, sizeof(attr));
	memset(info, 0, sizeof(*info));
	attr.info.bpf_fd = (__u32)fd;
	attr.info.info_len = sizeof(*info);
	attr.info.info = (__u64)(uintptr_t)info;
	return (int)syscall(__NR_bpf, BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr));
}

static int map_one(const char *path, size_t value_size, bool write, int *fd_out,
		   void **mem_out, size_t *len_out, unsigned int *entries_out,
		   unsigned int *id_out)
{
	struct bpf_map_info info;
	long page = sysconf(_SC_PAGESIZE);
	size_t len;
	void *mem;
	/*
	 * Always open read-write. mmap(MAP_SHARED) of a read-only file clears
	 * VM_SHARED, and bpf_map_mmap() rejects a mapping without it (EINVAL).
	 * A PROT_READ mapping stays read-only: bpf_map_mmap() also drops
	 * VM_MAYWRITE, so it cannot be made writable later.
	 */
	int fd = bpf_obj_get(path, 0);

	if (fd < 0)
		return -errno;
	if (bpf_map_info(fd, &info) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}
	if (page <= 0 || info.type != BPF_MAP_TYPE_ARRAY || info.key_size != sizeof(__u32) ||
	    info.value_size != value_size || !(info.map_flags & BPF_F_MMAPABLE) ||
	    info.max_entries != LW_CONTRACT_MAX_CLUSTERS) {
		close(fd);
		return -EPROTO;	/* layout or version mismatch with Lertick */
	}
	len = ((size_t)info.value_size * info.max_entries + page - 1) / page * page;
	mem = mmap(NULL, len, write ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		int err = -errno;

		close(fd);
		return err;
	}
	*fd_out = fd;
	*mem_out = mem;
	*len_out = len;
	*entries_out = info.max_entries;
	*id_out = info.id;
	return 0;
}

/* Opening two pins is not atomic. Recheck both identities after obtaining
 * both mappings, and periodically thereafter, so replacement during an
 * independent scheduler upgrade cannot leave a mixed old/new pair attached. */
static bool pins_current(const struct lw_contract *c)
{
	const char *paths[] = { LW_CONTRACT_PIN_LW, LW_CONTRACT_PIN_LT };
	unsigned int ids[] = { c->lw_id, c->lt_id };

	for (unsigned int i = 0; i < 2; i++) {
		struct bpf_map_info info;
		int fd = bpf_obj_get(paths[i], 0);
		bool same;

		if (fd < 0)
			return false;
		same = !bpf_map_info(fd, &info) && info.id == ids[i];
		close(fd);
		if (!same)
			return false;
	}
	return true;
}

int lw_contract_open(struct lw_contract *c, bool want_write)
{
	unsigned int n_lw = 0, n_lt = 0;
	void *lw = NULL, *lt = NULL;
	int ret;

	memset(c, 0, sizeof(*c));
	c->lw_fd = c->lt_fd = -1;
	if (access(LW_CONTRACT_PIN_LW, F_OK) || access(LW_CONTRACT_PIN_LT, F_OK))
		return -ENOENT;
	ret = map_one(LW_CONTRACT_PIN_LW, sizeof(struct lw_contract_rec), want_write,
		      &c->lw_fd, &lw, &c->lw_len, &n_lw, &c->lw_id);
	if (ret)
		return ret;
	ret = map_one(LW_CONTRACT_PIN_LT, sizeof(struct lt_demand_rec), false,
		      &c->lt_fd, &lt, &c->lt_len, &n_lt, &c->lt_id);
	if (ret) {
		munmap(lw, c->lw_len);
		close(c->lw_fd);
		c->lw_fd = -1;
		return ret;
	}
	c->lw_map = lw;
	c->lw = want_write ? lw : NULL;	/* never write through a read-only mapping */
	c->lt = lt;
	c->nentries = n_lw < n_lt ? n_lw : n_lt;
	if (c->nentries > LW_CONTRACT_MAX_CLUSTERS)
		c->nentries = LW_CONTRACT_MAX_CLUSTERS;
	if (!pins_current(c)) {
		lw_contract_close(c);
		return -ESTALE;
	}
	return 0;
}

void lw_contract_close(struct lw_contract *c)
{
	if (c->lw_map)
		munmap(c->lw_map, c->lw_len);
	if (c->lw_fd >= 0)
		close(c->lw_fd);
	if (c->lt)
		munmap((void *)c->lt, c->lt_len);
	if (c->lt_fd >= 0)
		close(c->lt_fd);
	memset(c, 0, sizeof(*c));
	c->lw_fd = c->lt_fd = -1;
}

#else /* non-Linux or standalone build without Linux BPF headers */

int lw_contract_open(struct lw_contract *c, bool want_write)
{
	(void)want_write;
	memset(c, 0, sizeof(*c));
	c->lw_fd = c->lt_fd = -1;
	return -ENOSYS;
}

void lw_contract_close(struct lw_contract *c)
{
	memset(c, 0, sizeof(*c));
	c->lw_fd = c->lt_fd = -1;
}

#endif

void lw_contract_publish(struct lw_contract *c, unsigned int cluster,
			 const struct lw_contract_rec *val)
{
	struct lw_contract_rec *r;
	__u32 s;

	if (!c->lw || cluster >= c->nentries)
		return;
	r = &c->lw[cluster];
	s = __atomic_load_n(&r->seq, __ATOMIC_RELAXED);
	if (s & 1)
		s++;	/* a previous writer died mid-update */
	__atomic_store_n(&r->seq, s + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	r->version = LW_CONTRACT_VERSION;
	r->ceiling_cap = val->ceiling_cap;
	r->floor_cap = val->floor_cap;
	r->energy_price_milli = val->energy_price_milli;
	r->thermal_headroom_mc = val->thermal_headroom_mc;
	r->valid_until_ns = val->valid_until_ns;
	r->topology_id = val->topology_id;
	r->session_id = val->session_id;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&r->seq, s + 2, __ATOMIC_RELEASE);
}

bool lw_contract_demand(struct lw_contract *c, unsigned int cluster, uint64_t topology_id, uint64_t now_ns,
			uint64_t max_age_ns, struct lt_demand_rec *out)
{
	if (!c->lt || cluster >= c->nentries || !topology_id)
		return false;
	for (int tries = 0; tries < 4; tries++) {
		const struct lt_demand_rec *r = &c->lt[cluster];
		struct lt_demand_rec copy;
		__u32 a = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE), b;

		if (!a || (a & 1))
			continue;
		memcpy(&copy, (const void *)r, sizeof(copy));
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		b = __atomic_load_n(&r->seq, __ATOMIC_RELAXED);
		if (a != b)
			continue;
		if (copy.version != LW_CONTRACT_VERSION || copy.topology_id != topology_id ||
		    !copy.session_id || !copy.updated_ns || copy.reserved ||
		    copy.pred_util > LW_CAP_ONE || copy.cpuperf_max > LW_CAP_ONE ||
		    copy.cpuperf_mean > copy.cpuperf_max || copy.lat_crit_milli > LW_MILLI_ONE) {
			c->lt_valid[cluster] = false;
			return false;
		}
		c->lt_last[cluster] = copy;
		c->lt_valid[cluster] = true;
		break;
	}
	if (!c->lt_valid[cluster])
		return false;
	/* Allow at most one publisher period of sampling skew, never an arbitrary
	 * future timestamp that would keep an old attachment fresh indefinitely. */
	if (c->lt_last[cluster].topology_id != topology_id ||
	    (c->lt_last[cluster].updated_ns > now_ns &&
	     c->lt_last[cluster].updated_ns - now_ns > LW_CONTRACT_PUBLISH_NS))
		return false;
	if (max_age_ns > LW_CONTRACT_DEMAND_MAX_AGE_NS)
		max_age_ns = LW_CONTRACT_DEMAND_MAX_AGE_NS;
	if (now_ns > c->lt_last[cluster].updated_ns &&
	    now_ns - c->lt_last[cluster].updated_ns > max_age_ns)
		return false;	/* Lertick stopped updating: treat as absent */
	if (c->lt_last[cluster].updated_ns > c->last_fresh_ns)
		c->last_fresh_ns = c->lt_last[cluster].updated_ns;
	*out = c->lt_last[cluster];
	return true;
}

bool lw_contract_maintain(struct lw_contract *c, bool attached, bool want_write,
			  uint64_t now_ns, uint64_t retry_ns, uint64_t stale_ns,
			  uint64_t *next_try_ns, int *err_out)
{
	int ret;

	if (attached) {
		uint64_t last = c->last_fresh_ns > c->opened_ns ? c->last_fresh_ns : c->opened_ns;
		bool current = true;

#if defined(__linux__) && !defined(LW_NO_BPF)
		if (now_ns >= c->next_pin_check_ns) {
			current = pins_current(c);
			c->next_pin_check_ns = now_ns + retry_ns;
		}
#endif

		if (current && (now_ns < last || now_ns - last < stale_ns))
			return true;
		lw_contract_close(c);
		*next_try_ns = now_ns;	/* a restarted Lertick has already pinned new maps */
		return false;
	}
	if (now_ns < *next_try_ns)
		return false;
	*next_try_ns = now_ns + retry_ns;
	ret = lw_contract_open(c, want_write);
	if (err_out)
		*err_out = ret;
	if (ret)
		return false;
	c->opened_ns = now_ns;
	return true;
}
