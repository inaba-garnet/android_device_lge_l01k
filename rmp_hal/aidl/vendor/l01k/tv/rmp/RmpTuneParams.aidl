// SPDX-License-Identifier: MIT
// PROTECTED_ES 経路の選局/受信パラメータ。現行 rf_secure の CLI flags に対応。
// 鍵・CAS 設定は含まない（librmp/TEE 内で完結）。
package vendor.l01k.tv.rmp;

@VintfStability
parcelable RmpTuneParams {
    int channel;    // UHF 13..62
    int station;    // TSID 1..0xffff
    int ecmPid;     // 0..8190
    int videoPid;   // 0..8190
    int bufferSize; // secure decoder buffer の 1 枚あたりサイズ(既定 1568768)
}
