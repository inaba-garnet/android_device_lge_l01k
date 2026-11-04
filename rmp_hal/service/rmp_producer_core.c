/* SPDX-License-Identifier: MIT
 * rmp_producer_core: rf_secure_main.c のロジックを callback 駆動へ再構成(境界は不変)。
 */
#define _POSIX_C_SOURCE 200809L
#include "rmp_producer_core.h"
#include "rf_frontend.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/dvb/dmx.h>
#include "broadcast_dmb_drv_ifdef.h"

_Static_assert(sizeof(struct dmx_cipher_operations) == 44, "Qualcomm cipher ABI");
_Static_assert(sizeof(struct dmx_secure_mode) == 4, "Qualcomm secure ABI");
_Static_assert(sizeof(struct dmx_decoder_buffers) == 140, "Qualcomm decoder ABI");
_Static_assert(sizeof(struct dmx_filter_event) == 80, "Qualcomm event ABI");

typedef int (*noarg)(void);
typedef int (*allocate)(unsigned, int *);
typedef int (*process_ecm)(unsigned, const unsigned char *);
typedef int (*free_index)(unsigned);

static uint32_t crc(const unsigned char *b, size_t n) {
	uint32_t c = 0xffffffff;
	while (n--) { c ^= (uint32_t)*b++ << 24; for (int j = 0; j < 8; j++) c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : c << 1; }
	return c;
}

/* ---- MPEG-2 section 再組立(PAT/ECM。TS framing。CAS内部ではない) ---- */
struct sec_accum { unsigned char buf[4096]; unsigned have, need; int cc; };
typedef void (*section_cb)(void *ctx, const unsigned char *sec, unsigned len);
static void sec_reset(struct sec_accum *a) { a->have = 0; a->need = 0; }
static void sec_append(struct sec_accum *a, const unsigned char *p, unsigned n, section_cb cb, void *ctx) {
	while (n) {
		if (a->have == 0 && *p == 0xff) return;
		unsigned cap = (a->have < 3) ? 3 : a->need;
		if (a->have >= 3 && a->need > sizeof(a->buf)) { sec_reset(a); return; }
		unsigned take = cap - a->have; if (take > n) take = n;
		memcpy(a->buf + a->have, p, take);
		a->have += take; p += take; n -= take;
		if (a->have >= 3) {
			a->need = 3 + (((unsigned)(a->buf[1] & 0x0f) << 8) | a->buf[2]);
			if (a->need > sizeof(a->buf)) { sec_reset(a); return; }
			if (a->have >= a->need) { cb(ctx, a->buf, a->need); sec_reset(a); }
		}
	}
}
static void sec_packet(struct sec_accum *a, const unsigned char *b, section_cb cb, void *ctx) {
	if (b[1] & 0x80) { sec_reset(a); a->cc = -1; return; }
	if (!(b[3] & 0x10)) return;
	unsigned pos = 4;
	if (b[3] & 0x20) { if (b[4] > 182) { sec_reset(a); a->cc = -1; return; } pos = 5 + b[4]; }
	if (pos >= 188) return;
	int cc = b[3] & 0x0f;
	if (a->cc >= 0 && cc != ((a->cc + 1) & 0x0f)) sec_reset(a);
	a->cc = cc;
	const unsigned char *p = b + pos; unsigned avail = 188 - pos;
	if (b[1] & 0x40) {
		unsigned ptr = p[0];
		if (1u + ptr > avail) { sec_reset(a); return; }
		if (a->have || a->need) sec_append(a, p + 1, ptr, cb, ctx);
		sec_reset(a);
		sec_append(a, p + 1 + ptr, avail - 1 - ptr, cb, ctx);
	} else if (a->have || a->need) {
		sec_append(a, p, avail, cb, ctx);
	}
}

struct ecm_ctx {
	int index; unsigned ecmpid, station;
	process_ecm rmp_ecm;
	unsigned patok, ecmok; int tsid_mismatch;
};
static void on_pat(void *v, const unsigned char *s, unsigned len) {
	struct ecm_ctx *c = (struct ecm_ctx *)v;
	if (s[0] != 0x00 || len < 12 || crc(s, len)) return;
	unsigned tsid = ((unsigned)s[3] << 8) | s[4];
	if (tsid != c->station) { c->tsid_mismatch = 1; return; }
	c->patok = 1;
}
static void on_ecm(void *v, const unsigned char *s, unsigned len) {
	struct ecm_ctx *c = (struct ecm_ctx *)v;
	if (s[0] != 0x82 && s[0] != 0x83) return;
	if (!c->patok) return;
	if (len != 182 || crc(s, len)) return;   /* 既知ECM body形式のみ(境界維持) */
	if (!c->rmp_ecm((unsigned)c->index, s + 8)) c->ecmok++;
}

struct rmp_core {
	int channel, station, ecmpid, vpid; unsigned bufsize;
	int securefds[DMX_MAX_DECODER_BUFFER_NUM]; unsigned count;
	void *lib; noarg rmp_init, rmp_term, rmp_count; allocate rmp_sta, rmp_kl;
	process_ecm rmp_ecm; free_index rmp_free;
	int initialized, index;
	rf_frontend *rf;
	int pes, pesstarted, configured;
	int lockfd;
	int pending[DMX_MAX_DECODER_BUFFER_NUM], cookie[DMX_MAX_DECODER_BUFFER_NUM];
	struct sec_accum patacc, ecmacc;
	struct ecm_ctx ctx;
	rmp_core_cb cb;
	unsigned char buf[188 * 256];
	int result;
};

static void state(rmp_core *c, const char *s) { if (c->cb.on_state) c->cb.on_state(c->cb.ctx, s); }

rmp_core *rmp_core_start(int channel, int station, int ecmpid, int vpid, unsigned bufsize,
                         const int *securefds, unsigned count, const rmp_core_cb *cb, int *err) {
	int e = 12;
	if (channel < 13 || channel > 62 || station < 1 || station > 0xffff ||
	    ecmpid < 0 || ecmpid > 8190 || vpid < 0 || vpid > 8190 ||
	    count < 2 || count > DMX_MAX_DECODER_BUFFER_NUM || !securefds || !cb) { if (err) *err = 2; return NULL; }
	rmp_core *c = (rmp_core *)calloc(1, sizeof(*c));
	if (!c) { if (err) *err = 12; return NULL; }
	c->channel = channel; c->station = station; c->ecmpid = ecmpid; c->vpid = vpid;
	c->bufsize = bufsize ? bufsize : 1568768;
	c->pes = -1; c->lockfd = -1; c->index = -1; c->cb = *cb;
	c->patacc.cc = -1; c->ecmacc.cc = -1;
	for (unsigned i = 0; i < DMX_MAX_DECODER_BUFFER_NUM; i++) c->securefds[i] = -1;
	for (unsigned i = 0; i < count; i++) { c->securefds[i] = dup(securefds[i]); if (c->securefds[i] < 0) { e = 12; goto fail; } }
	c->count = count;

	/* RF/CAS 排他: librmp初期化・RF確保の前に rf.lock を掴む(clear backend と同一inode)。O_CREATしない。 */
	{
		const char *lp = getenv("L01KTV_RF_LOCK"); if (!lp) lp = "/data/vendor/l01ktv/rf.lock";
		c->lockfd = open(lp, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		if (c->lockfd < 0) { e = 12; goto fail; }
		if (flock(c->lockfd, LOCK_EX | LOCK_NB)) { e = (errno == EWOULDBLOCK) ? 11 : 12; goto fail; }
	}

	c->lib = dlopen("/vendor/lib64/librmp.so", RTLD_NOW | RTLD_LOCAL);
	if (!c->lib) { e = 12; goto fail; }
	c->rmp_init = (noarg)dlsym(c->lib, "rmp_initialize");
	c->rmp_term = (noarg)dlsym(c->lib, "rmp_terminate");
	c->rmp_count = (noarg)dlsym(c->lib, "rmp_get_num_station_ctx");
	c->rmp_sta = (allocate)dlsym(c->lib, "rmp_allocate_station_ctx");
	c->rmp_kl = (allocate)dlsym(c->lib, "rmp_allocate_kl_index");
	c->rmp_ecm = (process_ecm)dlsym(c->lib, "rmp_process_ecm");
	c->rmp_free = (free_index)dlsym(c->lib, "rmp_free_kl_index");
	if (!c->rmp_init || !c->rmp_term || !c->rmp_count || !c->rmp_sta || !c->rmp_kl || !c->rmp_ecm || !c->rmp_free) { e = 12; goto fail; }

	if (c->rmp_init()) { e = 12; goto fail; }
	c->initialized = 1;
	{
		int handle = -1, before = c->rmp_count();
		if (c->rmp_sta((unsigned)station, &handle) || before < 0 || before != c->rmp_count()) { e = 12; goto fail; }
		if (c->rmp_kl((unsigned)handle, &c->index)) { e = 12; goto fail; }
	}
	c->ctx.index = c->index; c->ctx.ecmpid = (unsigned)ecmpid; c->ctx.station = (unsigned)station; c->ctx.rmp_ecm = c->rmp_ecm;

	{
		char errbuf[160] = {0};
		c->rf = rf_open(NULL, 1, errbuf, sizeof(errbuf));
		if (!c->rf) { e = 12; goto fail; }
		if (rf_power_on(c->rf, 1)) { e = 12; goto fail; }
		if (rf_tune(c->rf, channel, 0, NULL, NULL)) { e = 12; goto fail; }
		if (rf_tap_start(c->rf, 1, 0)) { e = 12; goto fail; }
	}
	state(c, "LOCKING");
	if (err) *err = 0;
	return c;
fail:
	if (err) *err = e;
	rmp_core_stop(c);   /* 部分初期化を解放 */
	return NULL;
}

void rmp_core_ack(rmp_core *c, uint32_t index) {
	if (!c || index >= c->count || !c->pending[index]) return;
	if (c->pes >= 0 && ioctl(c->pes, DMX_REUSE_DECODER_BUFFER, c->cookie[index]) == 0) c->pending[index] = 0;
}

int rmp_core_step(rmp_core *c) {
	if (!c) return 12;
	int ov = 0;
	ssize_t n = rf_read(c->rf, c->buf, sizeof(c->buf), 100, &ov);
	if (n < 0) { c->result = 12; return c->result; }

	for (ssize_t off = 0; off + 188 <= n; off += 188) {
		unsigned char *b = c->buf + off;
		if (b[0] != 0x47 || (b[3] & 0xc0)) continue;
		unsigned id = ((b[1] & 0x1f) << 8) | b[2];
		if (id == 0) sec_packet(&c->patacc, b, on_pat, &c->ctx);
		else if (id == (unsigned)c->ecmpid) sec_packet(&c->ecmacc, b, on_ecm, &c->ctx);
	}
	if (c->ctx.tsid_mismatch) { c->result = 13; return c->result; }

	if (c->ctx.ecmok && !c->configured) {
		c->configured = 1;
		c->pes = open("/dev/dvb0.demux0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (c->pes < 0) { c->result = 12; return c->result; }
		struct dmx_decoder_buffers buffers = {.is_linear = 1, .buffers_num = c->count, .buffers_size = c->bufsize};
		for (unsigned i = 0; i < c->count; i++) buffers.handles[i] = c->securefds[i];
		struct dmx_events_mask ev = {.disable_mask = 0, .no_wakeup_mask = 0, .wakeup_threshold = 1};
		if (ioctl(c->pes, DMX_SET_DECODER_BUFFER, &buffers) < 0 || ioctl(c->pes, DMX_SET_EVENTS_MASK, &ev) < 0) { c->result = 12; return c->result; }
		dmx_source_t src = DMX_SOURCE_FRONT1;
		struct dmx_pes_filter_params f = {.pid = (unsigned)c->vpid, .input = DMX_IN_FRONTEND, .output = DMX_OUT_DECODER, .pes_type = DMX_PES_VIDEO0, .flags = 0, .video_codec = DMX_VIDEO_CODEC_MPEG2};
		struct dmx_secure_mode sec = {.is_secured = 1};
		struct dmx_cipher_operations cip = {.pid = (unsigned)c->vpid, .operations_count = 1, .operations = {{.encrypt = 0, .key_ladder_id = (unsigned)c->index}}};
		if (ioctl(c->pes, DMX_SET_SOURCE, &src) < 0 || ioctl(c->pes, DMX_SET_PES_FILTER, &f) < 0 ||
		    ioctl(c->pes, DMX_SET_SECURE_MODE, &sec) < 0 || ioctl(c->pes, DMX_SET_CIPHER, &cip) < 0 || ioctl(c->pes, DMX_START, NULL) < 0) { c->result = 12; return c->result; }
		c->pesstarted = 1;
		state(c, "READY");
	}

	if (c->pesstarted) {
		for (int j = 0; j < 64; j++) {
			struct dmx_filter_event event = {0};
			int er = ioctl(c->pes, DMX_GET_EVENT, &event);
			if (er < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENODATA)) break;
			if (er < 0) { c->result = 12; return c->result; }
			if (event.type != DMX_EVENT_NEW_ES_DATA) continue;
			struct dmx_es_data_event_info *es = &event.params.es_data;
			int slot = -1; for (unsigned k = 0; k < c->count; k++) if (c->securefds[k] == es->buf_handle) slot = (int)k;
			if (slot < 0 || c->pending[slot]) { c->result = 12; return c->result; }
			c->cookie[slot] = es->cookie; c->pending[slot] = 1;
			if (c->cb.on_frame) c->cb.on_frame(c->cb.ctx, (uint32_t)slot, es->offset, es->data_len, es->pts, es->pts_valid ? 1u : 0u);
		}
	}
	return 0;
}

int rmp_core_stop(rmp_core *c) {
	if (!c) return 12;
	int result = c->result;
	if (result == 0) result = (c->ctx.ecmok && c->pesstarted) ? 0 : (c->ctx.patok ? 13 : 10);
	if (c->pesstarted && c->pes >= 0) ioctl(c->pes, DMX_STOP, NULL);
	if (c->pes >= 0) close(c->pes);
	for (unsigned i = 0; i < DMX_MAX_DECODER_BUFFER_NUM; i++) if (c->securefds[i] >= 0) close(c->securefds[i]);
	if (c->rf) rf_close(c->rf);
	if (c->index >= 0 && c->rmp_free) c->rmp_free((unsigned)c->index);
	if (c->initialized && c->rmp_term) c->rmp_term();
	if (c->lib) dlclose(c->lib);
	if (c->lockfd >= 0) close(c->lockfd);   /* RF/KL/librmp 解放後に rf.lock を手放す */
	state(c, "STOPPED");
	free(c);
	return result;
}
