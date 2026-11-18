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

#include "Hardware/Firmware/Smbios.hpp"
#include "Utility/ByteStream.hpp"
#include "uefi.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMaxStructures = 512, kMaxTable = 1u << 20;

uint8_t checksum(ConstByteView bytes) {
  uint8_t sum = 0;
  for (uint8_t b : bytes) sum = static_cast<uint8_t>(sum + b);
  return sum;
}

// String `number` (1-based) of a structure's string set, copied into `out` (cut to fit, control characters and trailing spaces left out).
void copy_string(ConstByteView strings, uint8_t number, char* out, uint32_t capacity) {
  out[0] = 0;
  if (number == 0 || capacity == 0) return;
  uint64_t at = 0;
  for (uint8_t n = 1; n < number; ++n) {                // skip to the string
    while (at < strings.size() && strings[at] != 0) ++at;
    if (at >= strings.size()) return;
    ++at;
    if (at >= strings.size() || strings[at] == 0) return;   // (the set ended: no such string)
  }
  uint32_t length = 0;
  while (at < strings.size() && strings[at] != 0) {
    const uint8_t c = strings[at++];
    if (length + 1 < capacity && c >= 0x20 && c != 0x7F) out[length++] = static_cast<char>(c);
  }
  while (length > 0 && out[length - 1] == ' ') --length;
  out[length] = 0;
}

uint16_t le16(ConstByteView v, uint32_t at) { return at + 2 <= v.size() ? static_cast<uint16_t>(v[at] | (v[at + 1] << 8)) : 0; }
uint32_t le32(ConstByteView v, uint32_t at) {
  return at + 4 <= v.size() ? static_cast<uint32_t>(v[at] | (v[at + 1] << 8) | (v[at + 2] << 16) | (uint32_t{v[at + 3]} << 24)) : 0;
}
uint8_t byte(ConstByteView v, uint32_t at) { return at < v.size() ? v[at] : 0; }

void structure(const ConstByteView f, const ConstByteView s, SmbiosInfo* out) {
  const uint8_t type = f[0];
  auto str = [&](uint32_t at, char* dst, uint32_t capacity) { copy_string(s, byte(f, at), dst, capacity); };
  switch (type) {
    case 0:
      str(4, out->bios_vendor, SmbiosInfo::kText);
      str(5, out->bios_version, SmbiosInfo::kText);
      str(8, out->bios_date, SmbiosInfo::kText);
      break;
    case 1:
      str(4, out->manufacturer, SmbiosInfo::kText);
      str(5, out->product, SmbiosInfo::kText);
      str(6, out->version, SmbiosInfo::kText);
      str(7, out->serial, SmbiosInfo::kText);
      if (f.size() >= 0x18) ByteView(out->uuid).copy_from(f.subview(8, 16));
      if (f.size() >= 0x1B) {
        str(0x19, out->sku, SmbiosInfo::kText);
        str(0x1A, out->family, SmbiosInfo::kText);
      }
      break;
    case 2:
      str(4, out->board_manufacturer, SmbiosInfo::kText);
      str(5, out->board_product, SmbiosInfo::kText);
      str(6, out->board_version, SmbiosInfo::kText);
      break;
    case 3:
      str(4, out->chassis_manufacturer, SmbiosInfo::kText);
      out->chassis_type = byte(f, 5) & 0x7F;
      break;
    case 4: {
      const uint8_t status = byte(f, 0x18);
      if (f.size() >= 0x1A && (status & 0x40) == 0) break;     // (an empty socket)
      if (out->processor_count++ == 0) {
        str(0x10, out->processor, SmbiosInfo::kText);
        out->processor_max_mhz = le16(f, 0x14);
      }
      break;
    }
    case 17: {
      if (f.size() < 0x15) break;
      uint64_t size_mib = le16(f, 0x0C);
      if (size_mib == 0 || size_mib == 0xFFFF) break;          // (an empty or unknown slot)
      if (size_mib == 0x7FFF && f.size() >= 0x20) size_mib = le32(f, 0x1C) & 0x7FFFFFFF;   // the extended size, in MiB
      else if (size_mib & 0x8000) size_mib = (size_mib & 0x7FFF) / 1024;                 // (KiB granularity)
      if (size_mib == 0) break;
      out->memory_total_mib += size_mib;
      if (out->memory_count == SmbiosInfo::kMaxMemory) break;
      SmbiosInfo::Memory& m = out->memory[out->memory_count++];
      m.size_mib = size_mib;
      m.type = byte(f, 0x12);
      const uint16_t configured = f.size() >= 0x22 ? le16(f, 0x20) : 0, rated = le16(f, 0x15);
      m.speed_mts = configured ? configured : rated;
      copy_string(s, byte(f, 0x10), m.locator, sizeof m.locator);
      copy_string(s, byte(f, 0x17), m.manufacturer, sizeof m.manufacturer);
      copy_string(s, byte(f, 0x1A), m.part, sizeof m.part);
      break;
    }
    default:
      break;
  }
}
}  // namespace

bool SmbiosInfo::is_portable() const {
  switch (chassis_type) {
    case 8: case 9: case 10: case 11: case 14: case 30: case 31: case 32: return true;   // portable, laptop, notebook, hand-held, sub-notebook, tablet, convertible, detachable
    default: return false;
  }
}

bool parse_smbios_entry(ConstByteView e, uint64_t* address, uint32_t* length, uint8_t* major, uint8_t* minor) {
  if (e.size() >= 0x18 && e[0] == '_' && e[1] == 'S' && e[2] == 'M' && e[3] == '3' && e[4] == '_') {
    const uint8_t size = e[6];
    if (size < 0x18 || size > e.size() || checksum(e.first(size)) != 0) return false;
    *major = e[7];
    *minor = e[8];
    *length = le32(e, 0x0C);                                    // (the maximum size: the walk ends at type 127)
    *address = uint64_t{le32(e, 0x10)} | (uint64_t{le32(e, 0x14)} << 32);
    return *address != 0 && *length != 0;
  }
  if (e.size() >= 0x1F && e[0] == '_' && e[1] == 'S' && e[2] == 'M' && e[3] == '_') {
    const uint8_t size = e[5];
    if (size < 0x1F || size > e.size() || checksum(e.first(size)) != 0) return false;
    if (e[0x10] != '_' || e[0x11] != 'D' || e[0x12] != 'M' || e[0x13] != 'I' || e[0x14] != '_' || checksum(e.subview(0x10, 0x0F)) != 0) return false;
    *major = e[6];
    *minor = e[7];
    *length = le16(e, 0x16);
    *address = le32(e, 0x18);
    return *address != 0 && *length != 0;
  }
  return false;
}

bool parse_smbios_table(ConstByteView table, SmbiosInfo* out) {
  const uint8_t major = out->major, minor = out->minor;
  *out = SmbiosInfo();
  out->major = major;
  out->minor = minor;
  uint64_t at = 0;
  while (at + 4 <= table.size()) {
    if (out->structures == kMaxStructures) {
      out->truncated = true;
      break;
    }
    const uint8_t type = table[at], length = table[at + 1];
    if (length < 4 || length > table.size() - at) {             // a formatted part that does not fit
      out->truncated = true;
      break;
    }
    const ConstByteView formatted = table.subview(at, length);
    // The string set: up to the double NUL (an empty set is just the two NULs).
    uint64_t end = at + length;
    bool closed = false;
    while (end + 1 < table.size()) {
      if (table[end] == 0 && table[end + 1] == 0) {
        closed = true;
        break;
      }
      ++end;
    }
    if (!closed) {
      out->truncated = true;
      break;
    }
    const ConstByteView strings = table.subview(at + length, end - (at + length) + 1);   // (with its last NUL)
    ++out->structures;
    if (type == 127) break;                                     // end of table
    structure(formatted, strings, out);
    at = end + 2;
  }
  return out->structures > 0;
}

const char* smbios_memory_type_name(uint8_t type) {
  switch (type) {
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default: return "memory";
  }
}

const char* smbios_chassis_type_name(uint8_t type) {
  switch (type) {
    case 3: return "Desktop";
    case 4: return "Low-profile desktop";
    case 6: return "Mini tower";
    case 7: return "Tower";
    case 8: return "Portable";
    case 9: return "Laptop";
    case 10: return "Notebook";
    case 13: return "All-in-one";
    case 14: return "Sub-notebook";
    case 17: return "Server";
    case 23: return "Rack mount";
    case 30: return "Tablet";
    case 31: return "Convertible";
    case 32: return "Detachable";
    case 35: return "Mini PC";
    case 36: return "Stick PC";
    default: return "Computer";
  }
}

// ------------------------------------------------------------------ the machine's

namespace {
constexpr efi::Guid kSmbios3Guid = {0xF2FD1544, 0x9794, 0x4A2C, {0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94}};
constexpr efi::Guid kSmbiosGuid = {0xEB9D2D31, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};
bool same_guid(const efi::Guid& a, const efi::Guid& b) { return __builtin_memcmp(&a, &b, sizeof(a)) == 0; }

SmbiosInfo g_info;
bool g_ready = false;

// The firmware's memory, identity-mapped as UEFI leaves it. (The one place the address becomes a pointer: what it points at is firmware
// data that stays put; its length is the entry point's, at most kMaxTable.)
ConstByteView physical_view(uint64_t address, uint64_t length) {
  return ConstByteView(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address)), length);
}
}  // namespace

bool read_system_smbios(efi::SystemTable* st) {
  if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return true;
  if (!st || !st->configuration_table) return false;
  const void* entry = nullptr;
  for (uint64_t i = 0; i < st->number_of_table_entries; ++i) {
    const efi::ConfigurationTable& t = st->configuration_table[i];
    if (same_guid(t.vendor_guid, kSmbios3Guid)) { entry = t.vendor_table; break; }
    if (!entry && same_guid(t.vendor_guid, kSmbiosGuid)) entry = t.vendor_table;
  }
  if (!entry) return false;
  uint64_t address = 0;
  uint32_t length = 0;
  SmbiosInfo info;
  if (!parse_smbios_entry(physical_view(reinterpret_cast<uintptr_t>(entry), 0x20), &address, &length, &info.major, &info.minor)) return false;
  if (length > kMaxTable) length = kMaxTable;
  if (!parse_smbios_table(physical_view(address, length), &info)) return false;
  g_info = info;
  __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
  return true;
}

const SmbiosInfo* system_smbios() { return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) ? &g_info : nullptr; }

}  // namespace hardware
