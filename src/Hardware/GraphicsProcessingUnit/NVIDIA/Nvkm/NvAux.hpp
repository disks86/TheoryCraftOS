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

// A DisplayPort AUX channel of the display engine (nouveau's nvkm/subdev/i2c auxg94.c, auxgf119.c, auxgm200.c), and over it the DRM helper's
// logic (drm_dp_helper.c: drm_dp_dpcd_access, drm_dp_i2c_do_msg, drm_dp_i2c_drain_msg, drm_dp_i2c_xfer): native DPCD reads and writes, and
// I2C over AUX so that the EDID is read as from any I2C bus.
//
// One hardware transaction (xfer) carries at most 16 data bytes: the data goes through four 32-bit data registers, the request
// (the AUX command in bits 15:12 and the length minus one in bits 3:0, or bit 8 for an address-only request) to the control register, the
// address to the address register; the control register's bit 16 starts it and stays set until the reply came. The status register then
// has the sink's reply code (bits 19:16), the bytes received (bits 4:0), and the error bits (8: no reply in time, 11:9: errors on the wire).
//   G94 .. GM10x : channel registers at 0x00e4c0 + 0x50 * channel (data out e4c0, data in e4d0, address e4e0, control e4e4, status e4e8)
//   GM200 ..     : at 0x00d930 + 0x50 * channel (data out d930, data in d940, address d950, control d954, status d958)
// Before each transaction the channel is claimed (an idle wait and a "request" handshake, nouveau's _aux_init) and the hybrid pad of the
// channel is put in AUX mode; afterwards the claim is released.
//
// Untrusted: the sink and the hardware answer what they like. A reply that claims more bytes than were asked for is a protocol error (not
// copied), a short reply goes on from where it stopped (I2C) or fails (DPCD access that made no progress), DEFER is retried a bounded number
// of times, and every wait is bounded.
//
// Thread-safe: a transaction holds the channel's mutex.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2c.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace nv {

enum class AuxStatus : uint8_t {
  Ok,
  NoSink,      // the channel says nothing is connected (hot plug low)
  Busy,        // the channel never became idle, or the claim handshake never completed
  Timeout,     // the transaction never completed, or the sink did not answer (the status's timeout bit)
  WireError,   // the status's error bits (a broken reply on the wire)
  Nack,        // the sink (native) or the I2C device behind it refused
  Defer,       // the sink kept deferring past the retries
  Protocol,    // a reply that does not make sense (more bytes than asked for, no progress, an unknown reply code)
  Invalid,     // no such channel on this chip, or a request the channel cannot carry
};

class NvAuxChannel final : public II2cBus, public IDpcdAux {
 public:
  static constexpr uint32_t kMaxPayload = 16;
  static constexpr uint32_t kMaxChannels = 8;            // (a bound on the register arithmetic: 0x50 per channel)
  // nouveau's waits (in 1 us polls): 1 ms for idle and for the claim, 2 ms for a transaction.
  static constexpr uint32_t kIdlePolls = 1000, kClaimPolls = 1000, kTransactionPolls = 2000;
  // drm_dp_dpcd_access: 32 tries; drm_dp_i2c_do_msg: 7 tries plus one more for each I2C defer (up to 7 of them); 400-500 us between tries
  static constexpr uint32_t kNativeTries = 32, kI2cTries = 7, kRetryDelayUs = 500;

  // DP_AUX_* requests and replies
  static constexpr uint8_t kI2cWrite = 0x0, kI2cRead = 0x1, kI2cWriteStatusUpdate = 0x2, kI2cMot = 0x4, kNativeWrite = 0x8, kNativeRead = 0x9;
  static constexpr uint8_t kNativeReplyMask = 0x3, kNativeAck = 0x0, kNativeNack = 0x1, kNativeDefer = 0x2;
  static constexpr uint8_t kI2cReplyMask = 0xc, kI2cAck = 0x0, kI2cNack = 0x4, kI2cDefer = 0x8;

  // `channel`: the AUX channel (I2cPort::aux). The hybrid pad of the channel is the channel's number.
  NvAuxChannel(INvRegisters& regs, I2cVariant variant, uint8_t channel);

  bool valid() const { return base_ != 0; }

  // One hardware transaction (nvkm_i2c_aux_xfer with retry false): `request` the AUX command, `data` the payload (written for writes, filled
  // for reads; at most kMaxPayload bytes, empty for an address-only request). On Ok, *reply is the sink's reply code and *done the bytes the
  // sink took or gave (reads: what the status says, never more than data.size(); writes: data.size() on an ACK).
  [[nodiscard]] AuxStatus xfer(uint8_t request, uint32_t address, ByteView data, uint8_t* reply, uint32_t* done);

  // IDpcdAux (native reads and writes, any length, in pieces of kMaxPayload, each retried on DEFER): false unless every byte went.
  bool dpcd_read(uint32_t address, ByteView out) override;
  bool dpcd_write(uint32_t address, ConstByteView data) override;
  // II2cBus (I2C over AUX, drm_dp_i2c_xfer): each message opened with an address-only request with MOT set, its data in pieces of
  // kMaxPayload, and the transaction closed with an address-only request without MOT.
  bool transfer(ArrayView<Message> messages) override;

  // The result of the last dpcd_read/dpcd_write/transfer.
  AuxStatus last_status() const { return last_; }
  // The whole EDID of the sink, I2C over AUX (i2c_edid::read: block 0 checked, the extension blocks through the segment pointer).
  [[nodiscard]] bool read_edid(ByteBuffer* out) { return i2c_edid::read(*this, out); }

 private:
  AuxStatus claim();
  void release();
  AuxStatus xfer_locked(uint8_t request, uint32_t address, ByteView data, uint8_t* reply, uint32_t* done);
  AuxStatus native_locked(uint8_t request, uint32_t address, ByteView data, uint32_t* done);
  AuxStatus i2c_msg_locked(uint8_t request, uint32_t address, ByteView data, uint32_t* done);
  void set_autodpcd(bool enable);

  INvRegisters& regs_;
  I2cVariant variant_;
  uint8_t channel_;
  uint32_t base_ = 0;
  AuxStatus last_ = AuxStatus::Ok;
  kernel::Mutex mutex_;
};

}  // namespace nv
}  // namespace hardware
