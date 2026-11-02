// SPDX-License-Identifier: MIT
/* Versioned learned-parameter checkpoint. Deliberately not a memory dump. */
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "checkpoint.h"
#include "lerwatt/controller.h"

#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
#error "The checkpoint format requires IEEE-754 binary64 doubles"
#endif
_Static_assert(sizeof(double) == 8, "binary64 doubles required");

#define CP_HEADER 40u
#define CP_DOUBLES 17u
#define CP_RECORD (16u + 8u * CP_DOUBLES)
#define CP_MAX (CP_HEADER + LW_MAX_POLICIES * CP_RECORD + 8u)
#define CP_MAX_AGE (30u * 24u * 60u * 60u)
#define HASH_INITIAL UINT64_C(14695981039346656037)
static const unsigned char magic[8] = { 'L', 'W', 'T', 'M', 'C', 'P', '0', '1' };

struct learned {
	bool valid;
	uint64_t samples;
	double value[CP_DOUBLES];
};

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t length)
{
	const unsigned char *p = data;
	while (length--) {
		hash ^= *p++;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static void put64(unsigned char *out, uint64_t value)
{
	for (unsigned int i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static uint64_t get64(const unsigned char *in)
{
	uint64_t value = 0;
	for (unsigned int i = 0; i < 8; i++) value |= (uint64_t)in[i] << (i * 8);
	return value;
}

static void put32(unsigned char *out, uint32_t value)
{
	for (unsigned int i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static uint32_t get32(const unsigned char *in)
{
	uint32_t value = 0;
	for (unsigned int i = 0; i < 4; i++) value |= (uint32_t)in[i] << (i * 8);
	return value;
}

static uint64_t hash_number(uint64_t hash, uint64_t value)
{
	unsigned char bytes[8];
	put64(bytes, value);
	return hash_bytes(hash, bytes, sizeof(bytes));
}

static int hash_string(uint64_t *hash, const char *text, size_t capacity)
{
	size_t length = strnlen(text, capacity);
	if (length == capacity) return -EINVAL;
	*hash = hash_number(*hash, length);
	*hash = hash_bytes(*hash, text, length);
	return 0;
}

static int fingerprint(const struct lw_system *sys, const struct lw_ctl_params *p,
		       uint64_t *out)
{
	uint64_t h = HASH_INITIAL;
	char path[LW_PATH_MAX], identity[34];
	int fd, ret;
	ssize_t size;
	const double settings[] = {
		p->kp_w_per_c, p->ki_w_per_cs, p->deadband_c, p->temp_filter_s,
		p->temp_slow_filter_s, p->up_margin, p->min_ceiling_frac, p->price_ramp_c,
		p->lat_crit_threshold, p->mpc_horizon_s, p->mpc_over_soft_c, p->mpc_margin_c,
		p->mpc_peak_memory_s, p->mpc_hold_s, p->mpc_up_wait_max_s, p->mpc_load_filter_s,
		p->mpc_package_filter_s, p->mpc_raw_gap_c, p->mpc_peak_filter_s,
	};
	if (!sys || sys->npolicies < 1 || sys->npolicies > LW_MAX_POLICIES ||
	    sys->nzones < 0 || sys->nzones > LW_MAX_ZONES ||
	    sys->package_zone < -1 || sys->package_zone >= sys->nzones)
		return -EINVAL;
	if (hash_string(&h, sys->sysroot, sizeof(sys->sysroot)) ||
	    hash_string(&h, sys->sim_platform, sizeof(sys->sim_platform))) return -EINVAL;
	/* Real-machine checkpoints must not silently migrate to a similar CPU.
	 * Fixtures/simulators are also bound to their exact selected sysroot. */
	ret = snprintf(path, sizeof(path), "%s/etc/machine-id",
		       !strcmp(sys->sysroot, "/") ? "" : sys->sysroot);
	if (ret < 0 || (size_t)ret >= sizeof(path)) return -ENAMETOOLONG;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (!strcmp(sys->sysroot, "/") || errno != ENOENT) return -errno;
		h = hash_number(h, 0);
	} else {
		struct stat st;
		if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 32 || st.st_size > 33) {
			close(fd); return -EINVAL;
		}
		do { size = read(fd, identity, sizeof(identity)); } while (size < 0 && errno == EINTR);
		close(fd);
		if (size != 32 && !(size == 33 && identity[32] == '\n')) return -EINVAL;
		for (int i = 0; i < 32; i++)
			if (!((identity[i] >= '0' && identity[i] <= '9') ||
			      (identity[i] >= 'a' && identity[i] <= 'f'))) return -EINVAL;
		h = hash_number(h, 32);
		h = hash_bytes(h, identity, 32);
	}
	h = hash_number(h, sys->simulated);
	h = hash_number(h, sys->ncpus);
	h = hash_number(h, sys->npackages);
	h = hash_number(h, sys->npolicies);
	for (int i = 0; i < sys->npolicies; i++) {
		const struct lw_policy *q = &sys->policies[i];
		int z = q->zone >= 0 ? q->zone : sys->package_zone;
		if (q->id < 0 || q->ncpus < 1 || q->ncpus > LW_MAX_CPUS ||
		    q->nopps < 1 || q->nopps > LW_MAX_OPPS || z < -1 || z >= sys->nzones)
			return -EINVAL;
		h = hash_number(h, q->id); h = hash_number(h, q->ncpus);
		for (int j = 0; j < q->ncpus; j++) {
			if (q->cpus[j] < 0 || q->cpus[j] >= LW_MAX_CPUS) return -EINVAL;
			h = hash_number(h, q->cpus[j]);
		}
		if (hash_string(&h, q->driver, sizeof(q->driver)) ||
		    hash_string(&h, q->governor, sizeof(q->governor))) return -EINVAL;
		h = hash_number(h, q->capacity); h = hash_number(h, q->cpuinfo_min_khz);
		h = hash_number(h, q->cpuinfo_max_khz); h = hash_number(h, q->have_em);
		h = hash_number(h, q->synthetic_opps); h = hash_number(h, q->nopps);
		for (int j = 0; j < q->nopps; j++) {
			h = hash_number(h, q->opps[j].khz); h = hash_number(h, q->opps[j].power_uw);
			h = hash_number(h, q->opps[j].perf); h = hash_number(h, q->opps[j].inefficient);
		}
		if (z >= 0) {
			if (hash_string(&h, sys->zones[z].temp_path, sizeof(sys->zones[z].temp_path)) ||
			    hash_string(&h, sys->zones[z].type, sizeof(sys->zones[z].type))) return -EINVAL;
			h = hash_number(h, (uint32_t)sys->zones[z].passive_mc);
			h = hash_number(h, (uint32_t)sys->zones[z].critical_mc);
		} else h = hash_number(h, UINT64_MAX);
	}
	if (sys->package_zone >= 0 && hash_string(&h, sys->zones[sys->package_zone].temp_path,
						 sizeof(sys->zones[0].temp_path))) return -EINVAL;
	h = hash_number(h, (uint32_t)p->soft_limit_mc); h = hash_number(h, (uint32_t)p->raw_limit_mc);
	h = hash_number(h, p->policy); h = hash_number(h, p->cross_cluster);
	h = hash_number(h, p->temporal_forecast); h = hash_number(h, p->down_step_ms);
	h = hash_number(h, p->up_step_ms); h = hash_number(h, p->mpc_up_step_ms);
	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		uint64_t bits;
		if (!isfinite(settings[i])) return -EINVAL;
		memcpy(&bits, &settings[i], sizeof(bits)); h = hash_number(h, bits);
	}
	*out = h;
	return 0;
}

static void extract(struct learned *r, const struct lw_thermal_model *m)
{
	unsigned int k = 0;
	memset(r, 0, sizeof(*r));
	for (int i = 0; i < LW_TM_N; i++) r->value[k++] = m->theta[i];
	for (int i = 0; i < LW_TM_N; i++)
		for (int j = 0; j < LW_TM_N; j++) r->value[k++] = m->cov[i][j];
	r->value[k++] = m->lambda; r->value[k++] = m->resid_c_per_s;
	r->value[k++] = m->offset_avg_c; r->value[k++] = m->power_avg_w;
	r->value[k++] = m->hot_s; r->samples = m->samples;
}

static bool valid_model(const struct learned *r)
{
	double l[LW_TM_N][LW_TM_N] = { { 0 } };
	for (unsigned int i = 0; i < CP_DOUBLES; i++) if (!isfinite(r->value[i])) return false;
	if (r->samples < 8 || r->samples > UINT64_C(1000000000000) ||
	    r->value[0] <= 0 || r->value[0] > 1e6 || r->value[1] >= -1e-4 || r->value[1] <= -10 ||
	    fabs(r->value[2]) > 1e6 || -r->value[2] / r->value[1] < -40 ||
	    -r->value[2] / r->value[1] > 200 || r->value[12] < .9 || r->value[12] >= 1 ||
	    r->value[13] < 0 || r->value[13] > 1000 || r->value[14] < 0 || r->value[14] > 240 ||
	    r->value[15] < 0 || r->value[15] > 1e9 || r->value[16] <= 0 || r->value[16] > 1e12)
		return false;
	/* A checkpoint must not inject an indefinite or singular RLS covariance. */
	for (int i = 0; i < LW_TM_N; i++) {
		for (int j = 0; j <= i; j++) {
			double a = r->value[3 + i * LW_TM_N + j];
			double b = r->value[3 + j * LW_TM_N + i];
			if (fabs(a) > 1e5 || fabs(a - b) > 1e-7 * fmax(1, fmax(fabs(a), fabs(b)))) return false;
			for (int n = 0; n < j; n++) a -= l[i][n] * l[j][n];
			if (i == j) {
				if (!(a > 0)) return false;
				l[i][j] = sqrt(a);
			} else l[i][j] = a / l[j][j];
		}
	}
	return true;
}

static void inject(struct lw_thermal_model *m, const struct learned *r)
{
	unsigned int k = 0;
	for (int i = 0; i < LW_TM_N; i++) m->theta[i] = r->value[k++];
	for (int i = 0; i < LW_TM_N; i++)
		for (int j = 0; j < LW_TM_N; j++) m->cov[i][j] = r->value[k++];
	m->lambda = r->value[k++]; m->resid_c_per_s = r->value[k++];
	m->offset_avg_c = r->value[k++]; m->power_avg_w = r->value[k++];
	m->hot_s = r->value[k++]; m->samples = r->samples;
}

static int parent_open(const char *path, char *name, size_t capacity)
{
	char copy[LW_PATH_MAX], *part, *next;
	struct stat st;
	int fd, child;
	size_t length;
	if (!path || path[0] != '/' || !(length = strlen(path)) || length >= sizeof(copy)) return -EINVAL;
	memcpy(copy, path + 1, length);
	fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) return -errno;
	part = copy;
	while ((next = strchr(part, '/'))) {
		*next = 0;
		if (!part[0] || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -EINVAL; }
		child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0) { int error = -errno; close(fd); return error; }
		close(fd); fd = child; part = next + 1;
	}
	if (!part[0] || !strcmp(part, ".") || !strcmp(part, "..") || strlen(part) >= capacity) {
		close(fd); return -EINVAL;
	}
	if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
		close(fd); return -EACCES;
	}
	strcpy(name, part);
	return fd;
}

static int read_blob(int dir, const char *name, unsigned char *data, size_t *length)
{
	struct stat st;
	int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	size_t used = 0;
	if (fd < 0) return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    st.st_nlink != 1 || (st.st_mode & 0077) || st.st_size < CP_HEADER + 8 || st.st_size > CP_MAX) {
		close(fd); return -EINVAL;
	}
	while (used < (size_t)st.st_size) {
		ssize_t count = read(fd, data + used, (size_t)st.st_size - used);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) { int error = count ? -errno : -EIO; close(fd); return error; }
		used += (size_t)count;
	}
	unsigned char extra;
	ssize_t count;
	do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
	int error = count < 0 ? -errno : count ? -EFBIG : 0;
	close(fd);
	if (error) return error;
	if (memcmp(data, magic, sizeof(magic)) || get32(data + 8) != 1 ||
	    get32(data + 12) < 1 || get32(data + 12) > LW_MAX_POLICIES ||
	    get32(data + 32) != CP_RECORD || get32(data + 36) != 0 ||
	    used != CP_HEADER + (size_t)get32(data + 12) * CP_RECORD + 8 ||
	    get64(data + used - 8) != hash_bytes(HASH_INITIAL, data, used - 8)) return -EINVAL;
	*length = used;
	return 0;
}

int lw_checkpoint_load(const char *path, const struct lw_system *sys,
		       const struct lw_ctl_params *params, struct lw_ctl_state *states)
{
	unsigned char data[CP_MAX];
	struct learned rows[LW_MAX_POLICIES];
	char name[256];
	uint64_t identity;
	size_t length;
	int ret, dir, trained = 0;
	time_t now;
	if (!params || !states) return -EINVAL;
	ret = fingerprint(sys, params, &identity);
	if (ret) return ret;
	dir = parent_open(path, name, sizeof(name));
	if (dir < 0) return dir;
	ret = read_blob(dir, name, data, &length); close(dir);
	if (ret) return ret;
	if (get32(data + 12) != (uint32_t)sys->npolicies || get64(data + 16) != identity) return -ESTALE;
	now = time(NULL);
	if (now < 0) return -EIO;
	uint64_t written = get64(data + 24), current = (uint64_t)now;
	if (written > current + 300 || (current > written && current - written > CP_MAX_AGE)) return -ESTALE;
	memset(rows, 0, sizeof(rows));
	for (int i = 0; i < sys->npolicies; i++) {
		const unsigned char *record = data + CP_HEADER + (size_t)i * CP_RECORD;
		if (!states[i].init) return -EAGAIN;
		if (get32(record) > 1 || get32(record + 4) != (uint32_t)sys->policies[i].id) return -EINVAL;
		rows[i].valid = get32(record) != 0; rows[i].samples = get64(record + 8);
		for (unsigned int j = 0; j < CP_DOUBLES; j++) {
			uint64_t bits = get64(record + 16 + j * 8);
			memcpy(&rows[i].value[j], &bits, sizeof(bits));
		}
		if (rows[i].valid) {
			if (!valid_model(&rows[i])) return -EINVAL;
			trained++;
		} else {
			for (unsigned int j = 8; j < CP_RECORD; j++) if (record[j]) return -EINVAL;
		}
	}
	if (!trained) return -EINVAL;
	for (int i = 0; i < sys->npolicies; i++) if (rows[i].valid) inject(&states[i].tm, &rows[i]);
	return 0;
}

int lw_checkpoint_save(const char *path, const struct lw_system *sys,
		       const struct lw_ctl_params *params, const struct lw_ctl_state *states)
{
	unsigned char data[CP_MAX] = { 0 }, old[CP_MAX];
	char name[256], temporary[128];
	uint64_t identity;
	size_t length, old_length;
	int ret, dir, fd = -1, trained = 0;
	time_t now;
	static unsigned int serial;
	if (!params || !states) return -EINVAL;
	ret = fingerprint(sys, params, &identity);
	if (ret) return ret;
	memcpy(data, magic, sizeof(magic)); put32(data + 8, 1);
	put32(data + 12, (uint32_t)sys->npolicies); put64(data + 16, identity);
	now = time(NULL); if (now < 0) return -EIO;
	put64(data + 24, (uint64_t)now); put32(data + 32, CP_RECORD);
	for (int i = 0; i < sys->npolicies; i++) {
		unsigned char *record = data + CP_HEADER + (size_t)i * CP_RECORD;
		struct learned row;
		put32(record + 4, (uint32_t)sys->policies[i].id);
		extract(&row, &states[i].tm);
		if (!states[i].init || !valid_model(&row)) continue;
		put32(record, 1); put64(record + 8, row.samples); trained++;
		for (unsigned int j = 0; j < CP_DOUBLES; j++) {
			uint64_t bits; memcpy(&bits, &row.value[j], sizeof(bits));
			put64(record + 16 + j * 8, bits);
		}
	}
	if (!trained) return 0;
	length = CP_HEADER + (size_t)sys->npolicies * CP_RECORD + 8;
	put64(data + length - 8, hash_bytes(HASH_INITIAL, data, length - 8));
	dir = parent_open(path, name, sizeof(name)); if (dir < 0) return dir;
	/* Refuse to overwrite an unrelated private root file accidentally selected
	 * by --model-state. Only a recognized checkpoint may be replaced. */
	ret = read_blob(dir, name, old, &old_length);
	if (ret && ret != -ENOENT) { close(dir); return ret; }
	for (unsigned int attempt = 0; attempt < 32; attempt++) {
		snprintf(temporary, sizeof(temporary), ".lerwatt-model.%ld.%u.tmp", (long)getpid(), ++serial);
		fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
		if (fd >= 0 || errno != EEXIST) break;
	}
	if (fd < 0) { ret = -errno; close(dir); return ret; }
	ret = fchmod(fd, 0600) ? -errno : 0;
	for (size_t used = 0; !ret && used < length;) {
		ssize_t count = write(fd, data + used, length - used);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) ret = count ? -errno : -EIO;
		else used += (size_t)count;
	}
	if (!ret && fsync(fd)) ret = -errno;
	if (close(fd) && !ret) ret = -errno;
	if (!ret && renameat(dir, temporary, dir, name)) ret = -errno;
	if (!ret && fsync(dir)) ret = -errno;
	if (ret) unlinkat(dir, temporary, 0);
	close(dir);
	return ret;
}
