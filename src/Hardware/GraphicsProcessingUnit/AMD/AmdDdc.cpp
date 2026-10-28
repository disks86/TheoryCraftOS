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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdDdc.hpp"

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license) as far as it is known here: amdgpu_i2c.c's bit-banging callbacks (amdgpu_i2c_pre_xfer,
// _post_xfer, _set_clock, _set_data, _get_clock, _get_data), written from memory of that file (it was not at hand), the algorithm of the kernel's i2c-algo-bit.c, and
// drm_do_get_edid / drm_do_probe_ddc_edid for the EDID. The register choreography is therefore not checked against the source.

namespace hardware {

namespace {
constexpr uint8_t kEdidHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
constexpr uint32_t kExtensionCountAt = 126;

bool block_ok(ConstByteView block, bool base) {
  if (block.size() != AmdDdc::kBlockBytes) return false;
  if (base && !block.first(8).equals(ConstByteView(kEdidHeader, sizeof kEdidHeader))) return false;
  uint8_t sum = 0;
  for (uint8_t b : block) sum = static_cast<uint8_t>(sum + b);
  return sum == 0;
}
}  // namespace

void AmdDdc::delay_us(uint32_t us) {
  const uint64_t start = clock_.now_us();
  while (clock_.now_us() - start < us) {
  }
}

// amdgpu_i2c_pre_xfer: the output values cleared, the pins made inputs, the pads masked for software
void AmdDdc::pre_xfer() {
  r_.write(bus_.a_clk_reg, r_.read(bus_.a_clk_reg) & ~bus_.a_clk_mask);
  r_.write(bus_.a_data_reg, r_.read(bus_.a_data_reg) & ~bus_.a_data_mask);
  r_.write(bus_.en_clk_reg, r_.read(bus_.en_clk_reg) & ~bus_.en_clk_mask);
  r_.write(bus_.en_data_reg, r_.read(bus_.en_data_reg) & ~bus_.en_data_mask);
  r_.write(bus_.mask_clk_reg, r_.read(bus_.mask_clk_reg) | bus_.mask_clk_mask);
  r_.write(bus_.mask_data_reg, r_.read(bus_.mask_data_reg) | bus_.mask_data_mask);
}

// amdgpu_i2c_post_xfer: the pads given back
void AmdDdc::post_xfer() {
  r_.write(bus_.mask_clk_reg, r_.read(bus_.mask_clk_reg) & ~bus_.mask_clk_mask);
  r_.write(bus_.mask_data_reg, r_.read(bus_.mask_data_reg) & ~bus_.mask_data_mask);
}

// a line is high when nothing pulls it low: the output is "enabled" to pull it low
void AmdDdc::set_scl(bool high) {
  uint32_t v = r_.read(bus_.en_clk_reg) & ~bus_.en_clk_mask;
  if (!high) v |= bus_.en_clk_mask;
  r_.write(bus_.en_clk_reg, v);
}

void AmdDdc::set_sda(bool high) {
  uint32_t v = r_.read(bus_.en_data_reg) & ~bus_.en_data_mask;
  if (!high) v |= bus_.en_data_mask;
  r_.write(bus_.en_data_reg, v);
}

bool AmdDdc::get_scl() { return (r_.read(bus_.y_clk_reg) & bus_.y_clk_mask) != 0; }
bool AmdDdc::get_sda() { return (r_.read(bus_.y_data_reg) & bus_.y_data_mask) != 0; }

// i2c-algo-bit: sclhi waits for a sink that stretches the clock; scllo, sdalo, sdahi are a line change and a half bit time
bool AmdDdc::scl_high() {
  set_scl(true);
  const uint64_t start = clock_.now_us();
  while (!get_scl()) {
    if (clock_.now_us() - start > kStretchTimeoutUs) {
      failed_ = true;
      return false;
    }
  }
  delay_us(kHalfPeriodUs);
  return true;
}

void AmdDdc::scl_low() {
  set_scl(false);
  delay_us(kHalfPeriodUs);
}

void AmdDdc::sda_low() {
  set_sda(false);
  delay_us(kHalfPeriodUs);
}

void AmdDdc::sda_high() {
  set_sda(true);
  delay_us(kHalfPeriodUs);
}

void AmdDdc::start() {   // i2c_start: data low while the clock is high
  set_sda(false);
  delay_us(kHalfPeriodUs);
  scl_low();
}

void AmdDdc::repeated_start() {   // i2c_repstart
  set_sda(true);
  scl_high();
  set_sda(false);
  delay_us(kHalfPeriodUs);
  scl_low();
}

void AmdDdc::stop() {   // i2c_stop
  set_sda(false);
  delay_us(kHalfPeriodUs);
  scl_high();
  set_sda(true);
  delay_us(kHalfPeriodUs);
}

bool AmdDdc::send_byte(uint8_t value) {   // i2c_outb, then the acknowledge slot
  for (int i = 7; i >= 0; --i) {
    set_sda((value >> i & 1) != 0);
    delay_us(kHalfPeriodUs);
    if (!scl_high()) return false;
    scl_low();
  }
  sda_high();
  if (!scl_high()) return false;
  const bool acknowledged = !get_sda();
  scl_low();
  return acknowledged;
}

uint8_t AmdDdc::receive_byte(bool acknowledge) {   // readbytes: i2c_inb and the acknowledge
  uint8_t value = 0;
  sda_high();
  for (int i = 0; i < 8; ++i) {
    if (!scl_high()) return value;
    value = static_cast<uint8_t>(value << 1 | (get_sda() ? 1 : 0));
    scl_low();
  }
  set_sda(!acknowledge);
  delay_us(kHalfPeriodUs);
  if (!scl_high()) return value;
  scl_low();
  return value;
}

bool AmdDdc::transfer(ArrayView<Message> messages) {
  if (!bus_.valid || messages.size() == 0) return false;
  failed_ = false;
  pre_xfer();
  bool ok = true;
  bool first = true;
  for (Message& m : messages) {
    if (first) start();
    else repeated_start();
    first = false;
    if (!send_byte(static_cast<uint8_t>(m.address << 1 | (m.read ? 1 : 0)))) {
      ok = false;
      break;
    }
    if (m.read) {
      for (uint64_t i = 0; i < m.data.size(); ++i) {
        m.data[i] = receive_byte(i + 1 < m.data.size());
        if (failed_) break;
      }
    } else {
      for (uint64_t i = 0; i < m.data.size() && ok; ++i) ok = send_byte(m.data[i]);
    }
    if (!ok || failed_) {
      ok = false;
      break;
    }
  }
  if (!failed_) stop();   // (a held clock cannot be stopped on: the lines are let go)
  set_sda(true);
  set_scl(true);
  post_xfer();
  return ok;
}

bool AmdDdc::read_block(uint32_t block, ByteView out) {
  // drm_do_probe_ddc_edid: the segment pointer (for blocks from 2 on, a segment is two blocks), the offset within it, then the read
  uint8_t segment = static_cast<uint8_t>(block / 2);
  uint8_t offset = static_cast<uint8_t>(block % 2 * kBlockBytes);
  Message messages[3];
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
  return transfer(ArrayView<Message>(messages, count));
}

bool AmdDdc::read_edid(ByteBuffer* out) {
  if (!bus_.valid) return false;
  uint8_t base[kBlockBytes] = {};
  bool have_base = false;
  for (uint32_t attempt = 0; attempt < kTries && !have_base; ++attempt)
    have_base = read_block(0, ByteView(base, kBlockBytes)) && block_ok(ConstByteView(base, kBlockBytes), true);
  if (!have_base) return false;
  out->clear();
  if (!out->append(ConstByteView(base, kBlockBytes))) return false;
  const uint32_t extensions = base[kExtensionCountAt] < kMaxExtensions ? base[kExtensionCountAt] : kMaxExtensions;
  uint32_t kept = 0;
  for (uint32_t block = 1; block <= extensions; ++block) {
    uint8_t data[kBlockBytes] = {};
    bool good = false;
    for (uint32_t attempt = 0; attempt < kTries && !good; ++attempt)
      good = read_block(block, ByteView(data, kBlockBytes)) && block_ok(ConstByteView(data, kBlockBytes), false);
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

}  // namespace hardware
