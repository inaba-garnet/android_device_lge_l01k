// SPDX-License-Identifier: MIT
// system client(player) → vendor HAL(producer)。player↔rf_secure の exec+socket+FD渡しを置換。
// secureBuffers は secure MediaCodec が確保した保護入力バッファの FD。HAL は dup して
// DMX_SET_DECODER_BUFFER に使うのみで map/read しない。
///////////////////////////////////////////////////////////////////////////////
// THIS FILE IS IMMUTABLE. DO NOT EDIT IN ANY CASE.                          //
///////////////////////////////////////////////////////////////////////////////

// This file is a snapshot of an AIDL file. Do not edit it manually. There are
// two cases:
// 1). this is a frozen version file - do not edit this in any case.
// 2). this is a 'current' file. If you make a backwards compatible change to
//     the interface (from the latest frozen version), the build system will
//     prompt you to update this file with `m <name>-update-api`.
//
// You must not make a backward incompatible change to any AIDL file built
// with the aidl_interface module type with versions property set. The module
// type is used to build AIDL files in a way that they can be used across
// independently updatable components of the system. If a device is shipped
// with such a backward incompatible change, it has a high risk of breaking
// later when a module using the interface is updated, e.g., Mainline modules.

package vendor.l01k.tv.rmp;
@VintfStability
interface IRmpProducer {
  long start(in vendor.l01k.tv.rmp.RmpTuneParams params, in ParcelFileDescriptor[] secureBuffers, in vendor.l01k.tv.rmp.IRmpFrameCallback cb);
  void ack(long sessionId, int index);
  void stop(long sessionId);
}
