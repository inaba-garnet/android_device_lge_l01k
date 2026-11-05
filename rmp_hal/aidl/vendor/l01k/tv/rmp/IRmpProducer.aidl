// SPDX-License-Identifier: MIT
// system client(player) → vendor HAL(producer)。player↔rf_secure の exec+socket+FD渡しを置換。
// secureBuffers は secure MediaCodec が確保した保護入力バッファの FD。HAL は dup して
// DMX_SET_DECODER_BUFFER に使うのみで map/read しない。
package vendor.l01k.tv.rmp;

import vendor.l01k.tv.rmp.IRmpFrameCallback;
import vendor.l01k.tv.rmp.RmpTuneParams;

@VintfStability
interface IRmpProducer {
    // 保護受信を開始。RF/librmp/secure-demux を構成し、以後 cb.onFrameReady で通知する。
    // 戻り値は sessionId(>0)。RF は rf.lock で単一所有（clear 経路と排他）。
    long start(in RmpTuneParams params, in ParcelFileDescriptor[] secureBuffers,
               in IRmpFrameCallback cb);

    // player が index の消費を終えた。HAL は当該保護バッファを REUSE する（FRAME_ACK 相当）。
    void ack(long sessionId, int index);

    // 停止(冪等)。RF/KL/librmp/保護バッファの解放完了まで戻らない。
    void stop(long sessionId);
}
