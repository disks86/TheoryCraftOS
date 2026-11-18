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

#include "Hardware/UsbTypeC/AcpiUcsiTransport.hpp"
#include "Hardware/Acpi/AcpiResources.hpp"
#include "Hardware/UsbTypeC/UcsiProtocol.hpp"

namespace hardware {

namespace {

// 6f8398c2-7ca4-11e4-ad36-631042b5008f as ToUUID lays it out (the first three fields little endian).
constexpr uint8_t kUcsiDsmUuid[16] = {0xC2, 0x98, 0x83, 0x6F, 0xA4, 0x7C, 0xE4, 0x11, 0xAD, 0x36, 0x63, 0x10, 0x42, 0xB5, 0x00, 0x8F};
constexpr uint32_t kMaxCrsBytes = 512;

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

}  // namespace

uint32_t AcpiUcsiTransport::find_devices(acpi::AmlInterpreter& vm, ArrayView<acpi::Node*> out) {
  uint32_t found = 0;
  acpi::Node* stack[32];
  uint32_t depth = 0;
  stack[depth++] = vm.root();
  uint32_t visited = 0;
  while (depth != 0 && visited < 8192 && found < out.size()) {
    acpi::Node* parent = stack[--depth];
    for (acpi::Node* c = vm.first_child(parent); c && found < out.size(); c = vm.next_sibling(c)) {
      ++visited;
      if (vm.is_device(c) && (vm.device_has_id(c, "PNP0CA0") || vm.device_has_id(c, "USBC000")) && present(vm, c)) out[found++] = c;
      if (depth < 32) stack[depth++] = c;
    }
  }
  return found;
}

bool AcpiUcsiTransport::dsm(uint8_t function, acpi::Value* result) {
  if (!dsm_node_) return false;
  ++dsm_calls_;
  uint8_t uuid[16];                              // (a private copy: the arguments are the method's to read)
  for (uint32_t i = 0; i < 16; ++i) uuid[i] = kUcsiDsmUuid[i];
  acpi::Value args[4];
  args[0].kind = acpi::ValueKind::Buffer;
  args[0].length = 16;
  args[0].bytes = uuid;
  args[1].kind = acpi::ValueKind::Integer;
  args[1].integer = 1;                           // revision
  args[2].kind = acpi::ValueKind::Integer;
  args[2].integer = function;
  args[3].kind = acpi::ValueKind::Package;       // no arguments: an empty package
  args[3].length = 0;
  args[3].elements = nullptr;
  acpi::Value r;
  if (!vm_.evaluate(dsm_node_, args, 4, &r)) return false;
  if (result) *result = r;
  return true;
}

bool AcpiUcsiTransport::read_bytes(uint32_t offset, ByteView out) {
  if (offset > length_ || out.size() > length_ - offset) return false;
  for (uint64_t i = 0; i < out.size(); ++i) {
    const acpi::RegionAccess a{acpi::RegionSpace::SystemMemory, base_ + offset + i, 8, 0, 0, 0};
    uint64_t v = 0;
    if (!memory_.region_read(a, &v)) return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

bool AcpiUcsiTransport::write_bytes(uint32_t offset, ConstByteView data) {
  if (offset > length_ || data.size() > length_ - offset) return false;
  for (uint64_t i = 0; i < data.size(); ++i) {
    const acpi::RegionAccess a{acpi::RegionSpace::SystemMemory, base_ + offset + i, 8, 0, 0, 0};
    if (!memory_.region_write(a, data[i])) return false;
  }
  return true;
}

bool AcpiUcsiTransport::initialize() {
  if (!device_) return false;
  acpi::Node* dsm_node = child(vm_, device_, "_DSM");
  acpi::Node* crs = child(vm_, device_, "_CRS");
  if (!dsm_node || !vm_.is_method(dsm_node) || !crs) return false;
  AcpiResources resources;
  {
    kernel::ScopedRecursiveMutex guard(vm_.mutex());   // (the buffer _CRS returns is valid until the next evaluation)
    acpi::Value v;
    if (!vm_.evaluate(crs, nullptr, 0, &v) || v.kind != acpi::ValueKind::Buffer || !v.bytes) return false;
    uint8_t copy[kMaxCrsBytes];
    const uint32_t size = v.length < kMaxCrsBytes ? v.length : kMaxCrsBytes;
    for (uint32_t i = 0; i < size; ++i) copy[i] = v.bytes[i];
    if (!parse_acpi_resources(ConstByteView(copy, size), &resources) || resources.memory_count == 0) return false;
  }
  base_ = resources.memory[0].base;
  length_ = resources.memory[0].length;
  if (base_ == 0 || length_ < ucsi::kMailboxBytesV1) return false;
  dsm_node_ = dsm_node;
  {
    // function 0: which functions there are (bit n: function n). Read and write are needed.
    kernel::ScopedRecursiveMutex guard(vm_.mutex());
    acpi::Value r;
    if (!dsm(kDsmFunctionQuery, &r)) return fail();
    uint64_t mask = 0;
    if (r.kind == acpi::ValueKind::Integer) mask = r.integer;
    else if (r.kind == acpi::ValueKind::Buffer && r.bytes && r.length >= 1) mask = r.bytes[0];
    const uint64_t needed = (1u << kDsmFunctionWrite) | (1u << kDsmFunctionRead);
    if ((mask & needed) != needed) return fail();
  }
  if (!read_version(&version_)) return fail();
  // UCSI 2.0 grew MESSAGE IN to 256 bytes (MESSAGE OUT moves to 0x110); a mailbox too small for that keeps the 1.x layout.
  if (version_ >= ucsi::kVersion2_0 && length_ >= ucsi::kMailboxBytesV2) {
    capacity_ = ucsi::kMessageBytesV2;
    message_out_ = ucsi::kOffsetMessageOutV2;
  } else {
    capacity_ = ucsi::kMessageBytesV1;
    message_out_ = ucsi::kOffsetMessageOutV1;
  }
  return true;
}

bool AcpiUcsiTransport::fail() {
  dsm_node_ = nullptr;
  return false;
}

bool AcpiUcsiTransport::read_version(uint16_t* version) {
  kernel::ScopedRecursiveMutex guard(vm_.mutex());
  uint8_t b[2];
  if (!dsm(kDsmFunctionRead) || !read_bytes(ucsi::kOffsetVersion, ByteView(b))) return false;
  *version = static_cast<uint16_t>(b[0] | (b[1] << 8));
  return true;
}

bool AcpiUcsiTransport::read_cci(uint32_t* cci) {
  kernel::ScopedRecursiveMutex guard(vm_.mutex());
  uint8_t b[4];
  if (!dsm(kDsmFunctionRead) || !read_bytes(ucsi::kOffsetCci, ByteView(b))) return false;
  *cci = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

bool AcpiUcsiTransport::read_message_in(ByteView out) {
  if (out.size() > capacity_) return false;
  kernel::ScopedRecursiveMutex guard(vm_.mutex());   // (every access to the mailbox, and so to the host's memory mapping, under the interpreter's lock)
  return read_bytes(ucsi::kOffsetMessageIn, out);
}

bool AcpiUcsiTransport::write_message_out(ConstByteView data) {
  if (data.size() > capacity_) return false;
  kernel::ScopedRecursiveMutex guard(vm_.mutex());
  return write_bytes(message_out_, data);      // (handed over with the next CONTROL)
}

bool AcpiUcsiTransport::write_control(uint64_t control) {
  kernel::ScopedRecursiveMutex guard(vm_.mutex());
  uint8_t b[8];
  for (uint32_t i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(control >> (8 * i));
  return write_bytes(ucsi::kOffsetControl, ConstByteView(b)) && dsm(kDsmFunctionWrite);
}

}  // namespace hardware
