// SPDX-License-Identifier: MIT
// vendor HAL(producer) → system client(player) への通知。保護ES payload は運ばない。
// 現行 frame_ipc(FRAME_READY/FRAME_END) と状態通知の置換。
package vendor.l01k.tv.rmp;

@VintfStability
interface IRmpFrameCallback {
    // "LOCKING" | "READY" | "STOPPED"
    oneway void onState(in String state);

    // 保護バッファ index に新 ES が入った。offset/length は範囲、pts は 33bit、
    // flags bit0 = pts_valid（frame_ipc の FRAME_READY 相当）。payload は map/read しない。
    oneway void onFrameReady(int index, int offset, int length, long pts, int flags);

    // 終了通知。result は現行 rf_secure の exit code 互換（0=正常、10..13/他=要因）。
    oneway void onEnded(int result);
}
