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

#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"

// drm_do_get_edid / drm_do_probe_ddc_edid (Linux drm_edid.c; MIT license).

namespace hardware {
namespace i2c_edid {

namespace {
constexpr uint8_t kEdidHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
constexpr uint32_t kExtensionCountAt = 126;

bool block_ok(ConstByteView block, bool base) {
  if (block.size() != kBlockBytes) return false;
  if (base && !block.first(8).equals(ConstByteView(kEdidHeader, sizeof kEdidHeader))) return false;
  uint8_t sum = 0;
  for (uint8_t b : block) sum = static_cast<uint8_t>(sum + b);
  return sum == 0;
}

bool read_block(II2cBus& bus, uint32_t block, ByteView out) {
  // drm_do_probe_ddc_edid: the segment pointer (for blocks from 2 on, a segment is two blocks), the offset within it, then the read
  uint8_t segment = static_cast<uint8_t>(block / 2);
  uint8_t offset = static_cast<uint8_t>(block % 2 * kBlockBytes);
  II2cBus::Message messages[3];
  uint32_t count = 0;
  if (block > 1) {
    messages[count].address = kSegmentAddress;
    messages[count].data = ByteView(&segment, 1);
    ++count;
  }
  messages[count].address = kEdidAddress;
  messages[count].data = ByteView(&offset, 1);
  ++count;
  messages[count].address = kEdidAddress;
  messages[count].read = true;
  messages[count].data = out;
  ++count;
  return bus.transfer(ArrayView<II2cBus::Message>(messages, count));
}

}  // namespace

bool read(II2cBus& bus, ByteBuffer* out) {
  uint8_t base[kBlockBytes] = {};
  bool have_base = false;
  for (uint32_t attempt = 0; attempt < kTries && !have_base; ++attempt)
    have_base = read_block(bus, 0, ByteView(base, kBlockBytes)) && block_ok(ConstByteView(base, kBlockBytes), true);
  if (!have_base) return false;
  out->clear();
  if (!out->append(ConstByteView(base, kBlockBytes))) return false;
  const uint32_t extensions = base[kExtensionCountAt] < kMaxExtensions ? base[kExtensionCountAt] : kMaxExtensions;
  uint32_t kept = 0;
  for (uint32_t block = 1; block <= extensions; ++block) {
    uint8_t data[kBlockBytes] = {};
    bool good = false;
    for (uint32_t attempt = 0; attempt < kTries && !good; ++attempt)
      good = read_block(bus, block, ByteView(data, kBlockBytes)) && block_ok(ConstByteView(data, kBlockBytes), false);
    if (good) {
      if (!out->append(ConstByteView(data, kBlockBytes))) return false;
      ++kept;
    }
  }
  // The base block says how many extensions follow: made true (and its checksum with it) when blocks were left out or not asked for, as drm_do_get_edid does.
  if (kept != base[kExtensionCountAt]) {
    const ByteView first = out->writable_view().first(kBlockBytes);
    first[kExtensionCountAt] = static_cast<uint8_t>(kept);
    uint8_t sum = 0;
    for (uint32_t i = 0; i + 1 < kBlockBytes; ++i) sum = static_cast<uint8_t>(sum + first[i]);
    first[kBlockBytes - 1] = static_cast<uint8_t>(0 - sum);
  }
  return true;
}

}  // namespace i2c_edid
}  // namespace hardware
