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

#include "Hardware/GraphicsProcessingUnit/Multi/AppleGmux.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

// ---------------------------------------------------------------------------------------------------------------------------------------------
// _CRS

namespace {

constexpr uint8_t kSmallIoPort = 0x08, kSmallFixedIo = 0x09, kSmallEnd = 0x0F;
constexpr uint8_t kLargeMemory32Fixed = 0x06, kLargeDword = 0x07, kLargeWord = 0x08, kLargeQword = 0x0A;
constexpr uint8_t kAddressSpaceMemory = 0, kAddressSpaceIo = 1;

void take_io(AppleGmuxResource* out, uint64_t base, uint64_t length) {
  if (out->has_io || length == 0 || base > 0xFFFF || length > 0x10000 - base) return;   // (the 16-bit port space only)
  out->has_io = true;
  out->io_base = static_cast<uint16_t>(base);
  out->io_length = static_cast<uint16_t>(length > 0xFFFF ? 0xFFFF : length);
}

void take_memory(AppleGmuxResource* out, uint64_t base, uint64_t length) {
  if (out->has_memory || length == 0) return;
  out->has_memory = true;
  out->memory_base = base;
  out->memory_length = length;
}

// A Word, DWord or QWord address space descriptor's body (after the length field): type, flags, type flags, then granularity, minimum,
// maximum, translation and length, each `width` bytes.
void address_space(ConstByteView body, uint32_t width, AppleGmuxResource* out) {
  ByteReader in(body);
  const uint8_t type = in.u8();
  in.skip(2);
  auto field = [&]() -> uint64_t { return width == 2 ? in.le16() : width == 4 ? in.le32() : in.le64(); };
  (void)field();                 // granularity
  const uint64_t minimum = field();
  (void)field();                 // maximum
  (void)field();                 // translation
  const uint64_t length = field();
  if (!in.ok()) return;
  if (type == kAddressSpaceIo) take_io(out, minimum, length);
  else if (type == kAddressSpaceMemory) take_memory(out, minimum, length);
}

}  // namespace

bool apple_gmux_parse_resources(ConstByteView crs, AppleGmuxResource* out) {
  AppleGmuxResource r;
  ByteReader in(crs);
  while (!in.at_end()) {
    const uint8_t tag = in.u8();
    if ((tag & 0x80) == 0) {
      const uint8_t type = static_cast<uint8_t>((tag >> 3) & 0x0F);
      ConstByteView body = in.take(tag & 0x07);
      if (!in.ok()) return false;
      if (type == kSmallEnd) break;
      ByteReader b(body);
      if (type == kSmallIoPort && body.size() >= 7) {
        b.skip(1);                                 // decode
        const uint16_t minimum = b.le16();
        b.skip(3);                                 // maximum, alignment
        const uint8_t length = b.u8();
        if (b.ok()) take_io(&r, minimum, length);
      } else if (type == kSmallFixedIo && body.size() >= 3) {
        const uint16_t base = static_cast<uint16_t>(b.le16() & 0x03FF);
        const uint8_t length = b.u8();
        if (b.ok()) take_io(&r, base, length);
      }
    } else {
      const uint8_t type = static_cast<uint8_t>(tag & 0x7F);
      const uint16_t length = in.le16();
      ConstByteView body = in.take(length);
      if (!in.ok()) return false;
      if (type == kLargeMemory32Fixed && body.size() >= 9) {
        ByteReader b(body);
        b.skip(1);
        const uint32_t base = b.le32();
        const uint32_t size = b.le32();
        if (b.ok()) take_memory(&r, base, size);
      } else if (type == kLargeWord) {
        address_space(body, 2, &r);
      } else if (type == kLargeDword) {
        address_space(body, 4, &r);
      } else if (type == kLargeQword) {
        address_space(body, 8, &r);
      }
    }
  }
  if (!r.has_io && !r.has_memory) return false;
  *out = r;
  return true;
}

namespace {

// A method or name directly below `device` (find() alone would also look in the scopes above it).
acpi::Node* child(acpi::AmlInterpreter& vm, acpi::Node* device, const char* name) {
  acpi::Node* n = vm.find(name, device);
  return n && vm.parent(n) == device ? n : nullptr;
}

bool present(acpi::AmlInterpreter& vm, acpi::Node* device) {
  acpi::Node* sta = child(vm, device, "_STA");
  if (!sta) return true;
  uint64_t value = 0;
  return vm.evaluate_integer(sta, &value) && (value & 1);
}

constexpr uint32_t kMaxCrsBytes = 512, kMaxStack = 32, kMaxVisited = 8192;

}  // namespace

acpi::Node* AppleGmux::find_device(acpi::AmlInterpreter& vm) {
  acpi::Node* stack[kMaxStack];
  uint32_t depth = 0;
  stack[depth++] = vm.root();
  uint32_t visited = 0;
  while (depth != 0 && visited < kMaxVisited) {
    acpi::Node* parent = stack[--depth];
    for (acpi::Node* c = vm.first_child(parent); c && visited < kMaxVisited; c = vm.next_sibling(c)) {
      ++visited;
      if (vm.is_device(c) && vm.device_has_id(c, "APP000B") && present(vm, c)) return c;
      if (depth < kMaxStack) stack[depth++] = c;
    }
  }
  return nullptr;
}

bool AppleGmux::find_resource(acpi::AmlInterpreter& vm, acpi::Node* device, AppleGmuxResource* out) {
  if (!device) return false;
  acpi::Node* crs = child(vm, device, "_CRS");
  if (!crs) return false;
  kernel::ScopedRecursiveMutex guard(vm.mutex());   // (the buffer _CRS returns is valid until the next evaluation)
  acpi::Value v;
  if (!vm.evaluate(crs, nullptr, 0, &v) || v.kind != acpi::ValueKind::Buffer || !v.bytes) return false;
  uint8_t copy[kMaxCrsBytes];
  const uint32_t size = v.length < kMaxCrsBytes ? v.length : kMaxCrsBytes;
  (void)ByteView(copy, size).copy_from(ConstByteView(v.bytes, size));
  return apple_gmux_parse_resources(ConstByteView(copy, size), out);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Register access

bool AppleGmux::window_fits(uint16_t length) const {
  return length_ >= length && base_ <= 0xFFFF - (length - 1);
}

bool AppleGmux::is_indexed() {
  io_.write8(port(kIndexScratch), 0xAA);
  io_.write8(port(kIndexScratch + 1), 0x55);
  io_.write8(port(kIndexScratch + 2), 0x00);
  const uint16_t value = static_cast<uint16_t>(io_.read8(port(kIndexScratch)) | (io_.read8(port(kIndexScratch + 1)) << 8));
  return value == 0x55AA;
}

// Bit 0 of the write-index port is set while a transaction is in flight. Before a new one: drain a finished one (reading the read-index port
// acknowledges it) until the bit clears.
bool AppleGmux::index_wait_ready() {
  uint8_t status = io_.read8(port(kIndexWrite));
  for (uint32_t i = 0; i < kHandshakePolls; ++i) {
    if ((status & 0x01) == 0) return true;
    (void)io_.read8(port(kIndexRead));
    status = io_.read8(port(kIndexWrite));
    delay();
  }
  if ((status & 0x01) == 0) return true;
  ++handshake_timeouts_;
  return false;
}

// After the register number is written: wait for the gmux to set bit 0 (done), then acknowledge it by reading the read-index port.
bool AppleGmux::index_wait_complete() {
  uint8_t status = io_.read8(port(kIndexWrite));
  bool done = (status & 0x01) != 0;
  for (uint32_t i = 0; i < kHandshakePolls && !done; ++i) {
    status = io_.read8(port(kIndexWrite));
    done = (status & 0x01) != 0;
    if (!done) delay();
  }
  if (!done) {
    ++handshake_timeouts_;
    return false;
  }
  (void)io_.read8(port(kIndexRead));
  return true;
}

bool AppleGmux::read8(uint16_t reg, uint8_t* out) {
  kernel::ScopedMutex guard(mutex_);
  if (layout_ == AppleGmuxLayout::Classic) {
    *out = io_.read8(port(reg));
    return true;
  }
  if (layout_ != AppleGmuxLayout::Indexed || !index_wait_ready()) return false;
  io_.write8(port(kIndexRead), static_cast<uint8_t>(reg));
  if (!index_wait_complete()) return false;
  *out = io_.read8(port(kIndexValue));
  return true;
}

bool AppleGmux::write8(uint16_t reg, uint8_t value) {
  kernel::ScopedMutex guard(mutex_);
  if (layout_ == AppleGmuxLayout::Classic) {
    io_.write8(port(reg), value);
    return true;
  }
  if (layout_ != AppleGmuxLayout::Indexed) return false;
  io_.write8(port(kIndexValue), value);
  if (!index_wait_ready()) return false;
  io_.write8(port(kIndexWrite), static_cast<uint8_t>(reg));
  return index_wait_complete();
}

bool AppleGmux::read32(uint16_t reg, uint32_t* out) {
  kernel::ScopedMutex guard(mutex_);
  if (layout_ == AppleGmuxLayout::Classic) {
    *out = io_.read32(port(reg));
    return true;
  }
  if (layout_ != AppleGmuxLayout::Indexed || !index_wait_ready()) return false;
  io_.write8(port(kIndexRead), static_cast<uint8_t>(reg));
  if (!index_wait_complete()) return false;
  *out = io_.read32(port(kIndexValue));
  return true;
}

bool AppleGmux::write32(uint16_t reg, uint32_t value) {
  kernel::ScopedMutex guard(mutex_);
  const uint16_t target = layout_ == AppleGmuxLayout::Classic ? reg : kIndexValue;
  if (layout_ == AppleGmuxLayout::None) return false;
  for (uint16_t i = 0; i < 4; ++i) io_.write8(port(static_cast<uint16_t>(target + i)), static_cast<uint8_t>(value >> (8 * i)));
  if (layout_ == AppleGmuxLayout::Classic) return true;
  if (!index_wait_ready()) return false;
  io_.write8(port(kIndexWrite), static_cast<uint8_t>(reg));
  return index_wait_complete();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Detection

bool AppleGmux::detect() {
  layout_ = AppleGmuxLayout::None;
  max_brightness_ = 0;
  if (!window_fits(kMinIoLength)) return false;
  uint8_t version[3];
  for (uint16_t i = 0; i < 3; ++i) version[i] = io_.read8(port(static_cast<uint16_t>(kVersionMajor + i)));
  if (version[0] != 0xFF || version[1] != 0xFF || version[2] != 0xFF) {
    layout_ = AppleGmuxLayout::Classic;
    for (uint32_t i = 0; i < 3; ++i) version_[i] = version[i];
  } else {
    // All ones: either nothing answers, or the registers are behind the index (the scratch registers are only probed where the window
    // reaches them).
    if (!window_fits(kMinIndexedIoLength) || !is_indexed()) return false;
    layout_ = AppleGmuxLayout::Indexed;
    uint32_t v = 0;
    if (!read32(kVersionMajor, &v)) {
      layout_ = AppleGmuxLayout::None;
      return false;
    }
    version_[0] = static_cast<uint8_t>(v >> 24);
    version_[1] = static_cast<uint8_t>(v >> 16);
    version_[2] = static_cast<uint8_t>(v >> 8);
  }
  uint32_t max = 0;
  if (read32(kMaxBrightness, &max)) max_brightness_ = max > kBrightnessMask ? kBrightnessMask : max;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Switching

bool AppleGmux::write_switch(uint16_t reg, GpuMuxTarget target, uint8_t integrated, uint8_t discrete) {
  return write8(reg, target == GpuMuxTarget::Integrated ? integrated : discrete);
}

bool AppleGmux::read_switch(uint16_t reg, uint8_t integrated, GpuMuxTarget* out) {
  uint8_t value = 0;
  if (!read8(reg, &value)) return false;
  *out = value == integrated ? GpuMuxTarget::Integrated : GpuMuxTarget::Discrete;
  return true;
}

bool AppleGmux::active(GpuMuxTarget* out) { return read_switch(kSwitchGetDisplay, 2, out); }
bool AppleGmux::active_ddc(GpuMuxTarget* out) { return read_switch(kSwitchDdc, 1, out); }
bool AppleGmux::active_external(GpuMuxTarget* out) { return read_switch(kSwitchGetExternal, 2, out); }

bool AppleGmux::switch_to(GpuMuxTarget target) {
  if (!write_switch(kSwitchDdc, target, 1, 2)) return false;
  if (!write_switch(kSwitchDisplay, target, 2, 3)) return false;
  return !external_switchable_ || write_switch(kSwitchExternal, target, 2, 3);
}

bool AppleGmux::switch_ddc(GpuMuxTarget target) {
  // (Linux: pre-retina models, the classic gmux, can switch the panel's DDC separately; retina models need the eDP set up on both GPUs.)
  if (layout_ != AppleGmuxLayout::Classic) return false;
  return write_switch(kSwitchDdc, target, 1, 2);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Power

bool AppleGmux::power_on() {
  if (!write8(kDiscretePower, 1) || !write8(kDiscretePower, 3)) return false;
  discrete_on_ = true;
  return true;
}

bool AppleGmux::power_off() {
  if (!write8(kDiscretePower, 1) || !write8(kDiscretePower, 0)) return false;
  discrete_on_ = false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Backlight

bool AppleGmux::max_brightness(uint32_t* out) {
  if (layout_ == AppleGmuxLayout::None || max_brightness_ == 0) return false;
  *out = max_brightness_;
  return true;
}

bool AppleGmux::brightness(uint32_t* out) {
  uint32_t v = 0;
  if (!read32(kBrightness, &v)) return false;
  *out = v & kBrightnessMask;
  return true;
}

bool AppleGmux::set_brightness(uint32_t value) {
  if (max_brightness_ == 0) return false;
  return write32(kBrightness, value > max_brightness_ ? max_brightness_ : value);
}

bool AppleGmux::percent(uint8_t* out) {
  uint32_t v = 0;
  if (max_brightness_ == 0 || !brightness(&v)) return false;
  if (v > max_brightness_) v = max_brightness_;
  *out = static_cast<uint8_t>((static_cast<uint64_t>(v) * 100 + max_brightness_ / 2) / max_brightness_);
  return true;
}

bool AppleGmux::set_percent(uint8_t percent) {
  if (max_brightness_ == 0) return false;
  if (percent > 100) percent = 100;
  return set_brightness(static_cast<uint32_t>((static_cast<uint64_t>(max_brightness_) * percent + 50) / 100));
}

}  // namespace hardware
