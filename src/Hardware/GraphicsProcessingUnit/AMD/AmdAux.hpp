/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// The AUX channel of a DisplayPort or eDP connector, driven through the video BIOS: the AtomBIOS command table ProcessAuxChannelTransaction does the transaction on the wire, given the
// AUX request (the DisplayPort AUX header and payload) in the interpreter's scratch memory. Port of amdgpu_atombios_dp.c's amdgpu_atombios_dp_process_aux_ch and _aux_transfer (Copyright
// Advanced Micro Devices, Inc.; MIT license), and of the I2C-over-AUX adapter of the DRM helper (drm_dp_helper.c: drm_dp_i2c_xfer, _drain_msg, _do_msg), which makes an I2C bus of it so
// that the EDID is read as on any other (i2c_edid).
//
// The transaction limits of the table: a write has at most 12 payload bytes (the request length is 4 bits, header included), a read at most 16. The sink answers a request with an ACK,
// NACK or DEFER in the native and the I2C reply (DP 1.2 2.7.7): a defer is waited out and retried (kTries, plus one more for each I2C defer, up to kTries of them), the others fail.
//
// The eDP panel's power is AmdPanelPower's (which AmdVegaM runs before the first AUX access); the sink's DPCD is read through dpcd_read (Dpcd). Not done: link training.
class AmdAux final : public II2cBus, public IDpcdAux {
 public:
  static constexpr uint32_t kCommandTable = 78;           // GetIndexIntoMasterTable(COMMAND, ProcessAuxChannelTransaction)
  static constexpr uint8_t kNoHpd = 0xFF;
  static constexpr uint32_t kMaxPayload = 16, kMaxWritePayload = 12;
  static constexpr uint32_t kTries = 7, kRetryIntervalUs = 500;   // AUX_RETRY_INTERVAL

  // DP_AUX_*: the request codes (the high nibble of the first AUX byte)
  static constexpr uint8_t kI2cWrite = 0x0, kI2cRead = 0x1, kI2cWriteStatusUpdate = 0x2, kI2cMot = 0x4, kNativeWrite = 0x8, kNativeRead = 0x9;
  // DP_AUX_NATIVE_REPLY_* and DP_AUX_I2C_REPLY_*: the reply codes (the table's reply status, shifted down four)
  static constexpr uint8_t kNativeReplyMask = 0x3, kNativeAck = 0x0, kNativeNack = 0x1, kNativeDefer = 0x2;
  static constexpr uint8_t kI2cReplyMask = 0xC, kI2cAck = 0x0, kI2cNack = 0x4, kI2cDefer = 0x8;

  // The negative results of a transfer (the errno values the driver returns)
  static constexpr int32_t kBusy = -1, kTimedOut = -2, kIo = -3, kInvalid = -4, kRemoteIo = -5, kProtocol = -6;

  // channel: the connector's DDC line id (the AUX channel the table is told), hpd: its hot plug pin (0..5) or kNoHpd. The interpreter and its host must outlive this object.
  AmdAux(IAtomHost& host, AtomBios& bios, uint8_t channel, uint8_t hpd) : host_(host), bios_(bios), channel_(channel), hpd_(hpd) {}

  // One AUX message, the driver's struct drm_dp_aux_msg
  struct Msg {
    uint8_t request = 0;
    uint32_t address = 0;     // 20 bits
    ByteView buffer;          // the payload to write, or where to read to; its size is the message's size
    uint8_t reply = 0;        // out
  };

  // amdgpu_atombios_dp_aux_transfer: one message in one run of the command table. The bytes transferred, or a negative result. msg->reply is set when the table ran.
  int32_t aux_transfer(Msg* msg);
  // drm_dp_i2c_do_msg: aux_transfer with the retries and the reply checks (native reply, then the I2C reply of the I2C requests).
  int32_t do_msg(Msg* msg);

  // The sink's DPCD (a native read), up to kMaxPayload bytes at a time: false unless every byte came.
  bool dpcd_read(uint32_t address, ByteView out) override;
  // The sink's DPCD written (a native write), up to kMaxWritePayload bytes at a time: false unless every byte was taken.
  bool dpcd_write(uint32_t address, ConstByteView data) override;

  // The whole EDID of the sink (I2C over AUX).
  bool read_edid(ByteBuffer* out) { return i2c_edid::read(*this, out); }

  // drm_dp_i2c_xfer: the messages as I2C over AUX, each with a bare address request to start it (the transaction's "middle of transaction" flag set), its data in chunks of
  // kMaxPayload, and a bare address request without the flag to close the transaction.
  bool transfer(ArrayView<Message> messages) override;

 private:
  int32_t process_aux_ch(ConstByteView send, ByteView recv, uint8_t* ack);
  int32_t drain_msg(Msg* msg);
  void scratch_put(uint32_t offset, ConstByteView bytes);
  void scratch_get(uint32_t offset, ByteView bytes);

  IAtomHost& host_;
  AtomBios& bios_;
  uint8_t channel_;
  uint8_t hpd_;
};

}  // namespace hardware
