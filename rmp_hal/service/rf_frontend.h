/* SPDX-License-Identifier: MIT
 * rf_frontend: L-01K の ISDB-T チューナー(FC8300)と Qualcomm DVB demux の
 * 「RF共通コア」。電源・選局・SYNC・全PIDタップ・停止だけを担う。
 *
 * 設計意図（TV_BACKEND_OWNERSHIP.txt §3-§4）:
 *   RF を実際に操作する主体を1受信につき必ず1つにする。本ライブラリは
 *   ハンドルを1つ発行し、RF への ioctl はそのハンドル経由に限定する。
 *   状態機械で二重電源投入・二重タップ・順序違反を拒否する（EEXIST 等）。
 *
 * 範囲外（出力ステージ側。rf_read が渡す RAW_TS をどう使うかは呼び手の責任）:
 *   stdout への出力形式、CAS adapter 接続、secure demux(DECODER_BUFFER/
 *   SECURE_MODE/CIPHER/GET_EVENT) などは本ライブラリに含めない。
 */
#ifndef L01KTV_RF_FRONTEND_H
#define L01KTV_RF_FRONTEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rf_frontend rf_frontend;

/* NULL メンバは既定パス(/dev/broadcast_isdbt, /dev/dvb0.demux0, /dev/dvb0.dvr0)。 */
typedef struct {
	const char *tuner;
	const char *demux;
	const char *dvr;
} rf_paths;

/* GET_SIG_INFO の抜粋（CAS に無関係な受信品質のみ）。 */
typedef struct {
	int cn;
	int rssi;
	int per_a;
	int per_b;
	int per_fullseg;
	int fullseg_packets;
} rf_sig;

/* 失敗時は errno 相当を返す(>0)。errbuf には短い説明を書く(任意)。 */

/* ハンドルを1つ作る。demux/dvr は常に開く。tuner は open_tuner!=0 のときだけ開く
 * (no-tune: 既に選局済みのソースへ demux だけで付く場合は0)。 */
rf_frontend *rf_open(const rf_paths *paths, int open_tuner, char *errbuf, size_t errlen);

/* 電源: (reset_first なら OFF) -> ON -> OPEN(rf_band0, seg13)。tuner を開いていること。
 * 二重呼び出しは EEXIST。 */
int rf_power_on(rf_frontend *rf, int reset_first);

/* 選局: SET_CH(channel 13..62, tune_mode 0/1) -> DETECT_SYNC。
 * sync_status/sync_ext は非NULLなら SYNC 値を受け取る。要 rf_power_on。 */
int rf_tune(rf_frontend *rf, int channel, int tune_mode, int *sync_status, int *sync_ext);

/* 全PIDタップ開始: (keep_source==0 なら DMX_SET_SOURCE) -> TS_OUT_FORMAT(188)
 * -> DMX_SET_BUFFER_SIZE -> DMX_SET_PES_FILTER(pid=0x2000, OUT_TS_TAP, IMMEDIATE_START)。
 * source: 0=FRONT0, 1=FRONT1。二重呼び出しは EEXIST。 */
int rf_tap_start(rf_frontend *rf, int source, int keep_source);

/* dvr から RAW_TS を読む。timeout_ms 待つ。戻り値: >0 読めたbyte数, 0 データ無し(timeout),
 * -1 エラー(errno)。overflow!=NULL なら EOVERFLOW 発生時に 1 を足す。要 rf_tap_start。 */
ssize_t rf_read(rf_frontend *rf, void *buf, size_t len, int timeout_ms, int *overflow);

/* 受信品質。要 rf_power_on。失敗時 errno を返す。 */
int rf_sig_info(rf_frontend *rf, rf_sig *out);

/* 停止と解放(冪等): DMX_STOP -> CLOSE -> OFF -> fd close。NULL 可。 */
void rf_close(rf_frontend *rf);

/* 診断: ABI サイズと主要 ioctl 番号を JSON で stdout へ。ハンドル不要。 */
void rf_print_abi(void);

#ifdef __cplusplus
}
#endif
#endif /* L01KTV_RF_FRONTEND_H */
