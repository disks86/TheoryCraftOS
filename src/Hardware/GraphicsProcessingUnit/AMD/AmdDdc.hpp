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
#include "Allocators/IAllocator.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// The DDC channel of a connector, driven by software through the GPIO pins of the card, and the EDID read over it. The pins are the bus record of AtomI2c; the driver's choice, with its
// hardware I2C engine off by default (the amdgpu_hw_i2c module parameter), is the same: amdgpu_i2c.c's pre_xfer, getscl, getsda, setscl, setsda and post_xfer on the Linux i2c bit-banging
// algorithm (i2c-algo-bit.c) with a 10 us bit time and the 2.2 ms clock stretching limit that amdgpu sets (VESA's). The EDID is read as drm_do_get_edid does: block 0 at address 0x50 offset
// 0 (a write of the offset, a repeated start, a read), checked (header and checksum, four tries), then its extension blocks (the segment pointer at 0x30 for blocks from 2 on).
//
// Not the DisplayPort connectors': their "DDC" is the AUX channel (I2C over AUX, with the AtomBIOS command table ProcessAuxChannelTransaction): AmdAux.
// And differences, because the lines are an untrusted device's: the extension blocks are limited to kMaxExtensions, a block that never checks is left out rather than kept, and no wait is
// longer than the stretch limit.
class AmdDdc final : public II2cBus {
 public:
  static constexpr uint32_t kHalfPeriodUs = 5;            // i2c-algo-bit: (udelay + 1) / 2 with amdgpu's udelay of 10
  static constexpr uint32_t kStretchTimeoutUs = 2200;     // bit.timeout = usecs_to_jiffies(2200), "from VESA"
  static constexpr uint8_t kEdidAddress = i2c_edid::kEdidAddress, kSegmentAddress = i2c_edid::kSegmentAddress;
  static constexpr uint32_t kBlockBytes = i2c_edid::kBlockBytes, kTries = i2c_edid::kTries, kMaxExtensions = i2c_edid::kMaxExtensions;

  AmdDdc(IVcnRegisters& registers, const IClock& clock, const AtomI2c::Bus& bus) : r_(registers), clock_(clock), bus_(bus) {}

  // The whole EDID (the base block and the extension blocks that checked) into *out, replacing it. False if the bus is not valid or block 0 never checked.
  bool read_edid(ByteBuffer* out);

  // The bus level: one transfer of messages (II2cBus). The transfer ends with a stop (also when it fails).
  bool transfer(ArrayView<Message> messages) override;

 private:
  void pre_xfer();
  void post_xfer();
  void set_scl(bool high);
  void set_sda(bool high);
  bool get_scl();
  bool get_sda();
  void delay_us(uint32_t us);
  bool scl_high();     // release the clock and wait for it to go high (the sink may hold it low)
  void scl_low();
  void sda_low();
  void sda_high();
  void start();
  void repeated_start();
  void stop();
  bool send_byte(uint8_t value);      // true if acknowledged
  uint8_t receive_byte(bool acknowledge);

  IVcnRegisters& r_;
  const IClock& clock_;
  AtomI2c::Bus bus_;
  bool failed_ = false;   // the clock was held low past the limit in this transfer
};

}  // namespace hardware
