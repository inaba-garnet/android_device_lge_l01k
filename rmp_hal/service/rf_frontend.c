/* SPDX-License-Identifier: MIT
 * rf_frontend 実装。tuner_stream.c(Codex, MIT) の RF/DVB 制御部を抽出し、
 * 単一所有ハンドルへ閉じ込めたもの。ioctl の順序と ABI は本機 kernel UAPI
 * に従う(28B PES params ほか)。CAS・secure 経路は含まない。
 */
#define _POSIX_C_SOURCE 200809L
#include "rf_frontend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/dvb/dmx.h>
#include "broadcast_dmb_drv_ifdef.h"

/* 本機 kernel UAPI であることの固定(mainline と取り違えないため)。 */
_Static_assert(sizeof(struct broadcast_dmb_init_info) == 8, "LG init ABI");
_Static_assert(sizeof(struct broadcast_dmb_set_ch_info) == 20, "LG channel ABI");
_Static_assert(sizeof(struct dmx_pes_filter_params) == 28, "Use Qualcomm kernel dmx.h, NOT mainline UAPI");
_Static_assert(offsetof(struct dmx_pes_filter_params, rec_chunk_size) == 20, "Qualcomm recording ABI");

struct rf_frontend {
	int tf; /* tuner  */
	int mf; /* demux  */
	int vf; /* dvr    */
	int powered;
	int opened;
	int tapping;
};

static int ctl(int fd, unsigned long cmd, void *arg)
{
	errno = 0;
	return ioctl(fd, cmd, arg) == 0 ? 0 : (errno ? errno : -1);
}

rf_frontend *rf_open(const rf_paths *paths, int open_tuner, char *errbuf, size_t errlen)
{
	const char *tuner = paths && paths->tuner ? paths->tuner : "/dev/broadcast_isdbt";
	const char *demux = paths && paths->demux ? paths->demux : "/dev/dvb0.demux0";
	const char *dvr   = paths && paths->dvr   ? paths->dvr   : "/dev/dvb0.dvr0";

	rf_frontend *rf = calloc(1, sizeof(*rf));
	if (!rf) {
		if (errbuf && errlen) snprintf(errbuf, errlen, "alloc");
		return NULL;
	}
	rf->tf = rf->mf = rf->vf = -1;

	if (open_tuner) {
		rf->tf = open(tuner, O_RDWR | O_CLOEXEC);
		if (rf->tf < 0) {
			if (errbuf && errlen) snprintf(errbuf, errlen, "open %s: %s", tuner, strerror(errno));
			free(rf);
			return NULL;
		}
	}
	rf->mf = open(demux, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	rf->vf = rf->mf < 0 ? -1 : open(dvr, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (rf->mf < 0 || rf->vf < 0) {
		if (errbuf && errlen) snprintf(errbuf, errlen, "open %s/%s: %s", demux, dvr, strerror(errno));
		rf_close(rf);
		return NULL;
	}
	return rf;
}

int rf_power_on(rf_frontend *rf, int reset_first)
{
	if (!rf || rf->tf < 0)
		return EINVAL;
	if (rf->powered)
		return EEXIST;
	if (reset_first) {
		/* stock driver は既にOFFでも失敗でも -1。他TVクライアント停止後のみ使う。 */
		(void)ioctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_OFF, NULL);
	}
	int e = ctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_ON, NULL);
	if (e)
		return e;
	rf->powered = 1;
	struct broadcast_dmb_init_info init = { .rf_band = 0, .segment = 13 };
	e = ctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_OPEN, &init);
	if (e)
		return e;
	rf->opened = 1;
	return 0;
}

int rf_tune(rf_frontend *rf, int channel, int tune_mode, int *sync_status, int *sync_ext)
{
	if (!rf || rf->tf < 0 || !rf->opened)
		return EINVAL;
	if (channel < 13 || channel > 62 || (tune_mode != 0 && tune_mode != 1))
		return EINVAL;
	struct broadcast_dmb_set_ch_info tune = {
		.mode = (unsigned)tune_mode, .rf_band = 0,
		.channel = (unsigned)channel, .subchannel = 0, .segment = 13,
	};
	int e = ctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_SET_CH, &tune);
	if (e)
		return e;
	struct broadcast_dmb_sync_info sync = { 0 };
	if (!ctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_DETECT_SYNC, &sync)) {
		if (sync_status) *sync_status = (int)sync.sync_status;
		if (sync_ext) *sync_ext = (int)sync.sync_ext_status;
	}
	return 0;
}

int rf_tap_start(rf_frontend *rf, int source, int keep_source)
{
	if (!rf || rf->mf < 0 || rf->vf < 0)
		return EINVAL;
	if (rf->tapping)
		return EEXIST;
	dmx_source_t input = source ? DMX_SOURCE_FRONT1 : DMX_SOURCE_FRONT0;
	int e;
	if (!keep_source && (e = ctl(rf->mf, DMX_SET_SOURCE, &input)))
		return e;
	/* TSIF の入力形式は TSPP 固定。SET_TS_PACKET_FORMAT は不要。 */
	enum dmx_tsp_format_t format = DMX_TSP_FORMAT_188;
	if ((e = ctl(rf->mf, DMX_SET_TS_OUT_FORMAT, &format)))
		return e;
	if (ioctl(rf->vf, DMX_SET_BUFFER_SIZE, 188UL * 32768UL) < 0)
		return errno ? errno : -1;
	struct dmx_pes_filter_params pes = {
		.pid = 0x2000, .input = DMX_IN_FRONTEND, .output = DMX_OUT_TS_TAP,
		.pes_type = DMX_PES_OTHER, .flags = DMX_IMMEDIATE_START,
		.rec_chunk_size = DMX_REC_BUFF_CHUNK_MIN_SIZE, .video_codec = DMX_VIDEO_CODEC_MPEG2,
	};
	if ((e = ctl(rf->mf, DMX_SET_PES_FILTER, &pes)))
		return e;
	rf->tapping = 1;
	return 0;
}

ssize_t rf_read(rf_frontend *rf, void *buf, size_t len, int timeout_ms, int *overflow)
{
	if (!rf || rf->vf < 0 || !rf->tapping) {
		errno = EINVAL;
		return -1;
	}
	struct pollfd fd = { .fd = rf->vf, .events = POLLIN };
	int n = poll(&fd, 1, timeout_ms);
	if (n < 0)
		return (errno == EINTR) ? 0 : -1;
	if (n == 0)
		return 0;
	if (fd.revents & (POLLHUP | POLLNVAL)) {
		errno = EPIPE;
		return -1;
	}
	ssize_t got = read(rf->vf, buf, len);
	if (got < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	if (got < 0 && errno == EOVERFLOW) {
		if (overflow) (*overflow)++;
		return 0;
	}
	return got; /* <0 エラー, 0 EOF相当は呼び手が判断 */
}

int rf_sig_info(rf_frontend *rf, rf_sig *out)
{
	if (!rf || rf->tf < 0 || !rf->opened || !out)
		return EINVAL;
	struct broadcast_dmb_control_info info = { 0 };
	info.cmd_info.cmd = 1;
	info.cmd_info.layer = 3;
	int e = ctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_GET_SIG_INFO, &info);
	if (e)
		return e;
	mmb_sig_info *s = &info.sig_info.info.mmb_info;
	out->cn = s->cn;
	out->rssi = s->rssi;
	out->per_a = s->per_a;
	out->per_b = s->per_b;
	out->per_fullseg = s->per_fullseg;
	out->fullseg_packets = s->total_tsp_fullseg;
	return 0;
}

void rf_close(rf_frontend *rf)
{
	if (!rf)
		return;
	if (rf->tapping && rf->mf >= 0)
		(void)ioctl(rf->mf, DMX_STOP);
	rf->tapping = 0;
	if (rf->vf >= 0) { close(rf->vf); rf->vf = -1; }
	if (rf->mf >= 0) { close(rf->mf); rf->mf = -1; }
	if (rf->opened && rf->tf >= 0)
		(void)ioctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_CLOSE, NULL);
	rf->opened = 0;
	if (rf->powered && rf->tf >= 0)
		(void)ioctl(rf->tf, LGE_BROADCAST_DMB_IOCTL_OFF, NULL);
	rf->powered = 0;
	if (rf->tf >= 0) { close(rf->tf); rf->tf = -1; }
	free(rf);
}

void rf_print_abi(void)
{
	printf("{\"lg_init_bytes\":%zu,\"lg_channel_bytes\":%zu,\"qcom_pes_bytes\":%zu,",
	       sizeof(struct broadcast_dmb_init_info), sizeof(struct broadcast_dmb_set_ch_info),
	       sizeof(struct dmx_pes_filter_params));
	printf("\"ON\":\"0x%08lx\",\"OPEN\":\"0x%08lx\",\"SET_CH\":\"0x%08lx\",\"DMX_SET_PES_FILTER\":\"0x%08lx\","
	       "\"DMX_SET_SOURCE\":\"0x%08lx\",\"DMX_SET_TS_OUT_FORMAT\":\"0x%08lx\"}\n",
	       (unsigned long)LGE_BROADCAST_DMB_IOCTL_ON, (unsigned long)LGE_BROADCAST_DMB_IOCTL_OPEN,
	       (unsigned long)LGE_BROADCAST_DMB_IOCTL_SET_CH, (unsigned long)DMX_SET_PES_FILTER,
	       (unsigned long)DMX_SET_SOURCE, (unsigned long)DMX_SET_TS_OUT_FORMAT);
}
