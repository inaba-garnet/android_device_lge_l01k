/* SPDX-License-Identifier: MIT
 * PROTECTED_ES producer のCコア。rf_secure_main.c のRF/librmp/secure-demux/section再組立を、
 * CLI/socket を外しコールバック駆動の start/step/ack/stop に再構成したもの。
 * binder グルー(C++)から単一スレッドで駆動する前提(step と ack は同一スレッドから呼ぶ)。
 * 不変の境界: librmp は dlopen で opaque に呼ぶだけ。保護payloadを map/read しない。鍵/設定に触れない。
 */
#ifndef L01KTV_RMP_PRODUCER_CORE_H
#define L01KTV_RMP_PRODUCER_CORE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rmp_core rmp_core;

typedef struct {
	void (*on_state)(void *ctx, const char *state);                 /* "LOCKING"|"READY"|"STOPPED" */
	void (*on_frame)(void *ctx, uint32_t index, uint32_t offset,
	                 uint32_t length, uint64_t pts, uint32_t flags); /* FRAME_READY 相当 */
	void *ctx;
} rmp_core_cb;

/* 開始: rf.lock取得 → librmp初期化/局/KL → RF電源/選局/タップ。secure demux は step 内で遅延構成。
 * securefds は player が確保した保護バッファFD(呼び出し側で有効。coreがdupして保持)。
 * 失敗時 NULL を返し *err に exit code 互換(11=busy,12=err,2=引数,10=信号なし 等)。 */
rmp_core *rmp_core_start(int channel, int station, int ecmpid, int vpid, unsigned bufsize,
                         const int *securefds, unsigned count, const rmp_core_cb *cb, int *err);

/* 受信ループ1回(~100ms)。ECM継続投入→必要なら secure demux 構成(on_state READY)→
 * NEW_ES_DATA を on_frame 通知。継続中は 0、終了すべきときは result(>0 or 特定値) を返す。 */
int rmp_core_step(rmp_core *c);

/* index の保護バッファを REUSE(player消費完了)。step と同一スレッドから呼ぶこと。 */
void rmp_core_ack(rmp_core *c, uint32_t index);

/* 停止・解放(RF/KL/librmp/buffer/lock)。result を返す。c は解放される。 */
int rmp_core_stop(rmp_core *c);

#ifdef __cplusplus
}
#endif
#endif
