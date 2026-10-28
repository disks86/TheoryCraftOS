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
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An I2C bus as the EDID reader needs it: a transfer of messages, which follow each other with a repeated start and end with a stop (also when the transfer fails). The Linux
// i2c_adapter's master_xfer. The DDC pins of a connector (AmdDdc) and the AUX channel of a DisplayPort connector (AmdAux) are two of them.
class II2cBus {
 public:
  struct Message {
    uint8_t address = 0;      // 7 bits
    bool read = false;
    ByteView data;            // what to write, or where to read to
  };
  virtual bool transfer(ArrayView<Message> messages) = 0;

 protected:
  II2cBus() = default;
  ~II2cBus() = default;
};

// The EDID of the monitor on a bus, as drm_do_get_edid reads it: block 0 at address 0x50 offset 0 (a write of the offset, a repeated start, a read), checked (header and checksum, four
// tries), then its extension blocks (the segment pointer at 0x30 for blocks from 2 on).
//
// Differences, because the lines are an untrusted device's: the extension blocks are limited to kMaxExtensions and a block that never checks is left out rather than kept.
namespace i2c_edid {
constexpr uint8_t kEdidAddress = 0x50, kSegmentAddress = 0x30;
constexpr uint32_t kBlockBytes = 128, kTries = 4, kMaxExtensions = 3;

// The whole EDID (the base block and the extension blocks that checked) into *out, replacing it. False if block 0 never checked.
bool read(II2cBus& bus, ByteBuffer* out);
}  // namespace i2c_edid

}  // namespace hardware
