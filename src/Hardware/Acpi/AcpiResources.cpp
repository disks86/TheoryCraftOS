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


#include "Hardware/Acpi/AcpiResources.hpp"

namespace hardware {

namespace {

uint16_t le16(ConstByteView d, uint64_t at) { return static_cast<uint16_t>(d[at] | (d[at + 1] << 8)); }
uint32_t le32(ConstByteView d, uint64_t at) { return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) | (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24); }

// A zero-terminated string inside the descriptor (the resource source), copied; false if it has no end inside it or does not fit
bool source_string(ConstByteView d, uint64_t at, uint64_t end, char* out, uint32_t capacity) {
  uint32_t n = 0;
  for (uint64_t i = at; i < end; ++i) {
    if (d[i] == 0) { out[n] = 0; return true; }
    if (n + 1 >= capacity) return false;
    out[n++] = static_cast<char>(d[i]);
  }
  return false;
}

}  // namespace

bool parse_acpi_resources(ConstByteView d, AcpiResources* out) {
  *out = AcpiResources();
  uint64_t at = 0;
  while (at < d.size()) {
    const uint8_t tag = d[at];
    if ((tag & 0x80) == 0) {                                      // a small item: the length is in the tag
      const uint32_t length = tag & 7, type = (tag >> 3) & 0xF;
      if (at + 1 + length > d.size()) return false;
      if (type == 0xF) return true;                               // the end tag
      at += 1 + length;
      continue;
    }
    if (at + 3 > d.size()) return false;
    const uint32_t length = le16(d, at + 1), type = tag & 0x7F;
    const uint64_t body = at + 3, end = body + length;
    if (end > d.size()) return false;
    if (type == 0x06 && length >= 9) {                            // 32 bit fixed memory range
      if (out->memory_count < AcpiResources::kMaxMemory) {
        out->memory[out->memory_count].base = le32(d, body + 1);
        out->memory[out->memory_count].length = le32(d, body + 5);
        ++out->memory_count;
      }
    } else if (type == 0x09 && length >= 6) {                     // extended interrupt: flags, count, numbers
      const uint32_t count = d[body + 1];
      for (uint32_t i = 0; i < count && body + 2 + 4ull * (i + 1) <= end; ++i)
        if (out->interrupt_count < AcpiResources::kMaxInterrupts) out->interrupts[out->interrupt_count++] = le32(d, body + 2 + 4ull * i);
    } else if (type == 0x0C && length >= 20) {                    // GPIO connection
      const uint8_t connection = d[body + 1];
      const uint16_t flags = le16(d, body + 4);                   // interrupt flags (bytes 7 and 8 of the descriptor)
      const uint16_t pin_table = le16(d, body + 11), name_offset = le16(d, body + 14);
      // (offsets are from the start of the descriptor, its tag byte)
      if (connection == 0 && !out->gpio_interrupt.present && pin_table >= 3 && at + pin_table + 2 <= end && name_offset >= 3 && at + name_offset < end) {
        AcpiResources::GpioInterrupt& g = out->gpio_interrupt;
        if (!source_string(d, at + name_offset, end, g.controller, sizeof g.controller)) return false;
        g.pin = le16(d, at + pin_table);
        g.edge = (flags & 1) != 0;
        const uint32_t polarity = (flags >> 1) & 3;
        g.active_low = polarity == 1;
        g.active_both = polarity == 2;
        g.wake = (flags & 0x10) != 0;
        g.present = true;
      }
    } else if (type == 0x0E && length >= 12) {                    // serial bus connection
      const uint8_t bus_type = d[body + 2];
      const uint16_t type_data = le16(d, body + 7);
      if (bus_type == 1 && type_data >= 6 && body + 9 + type_data <= end && !out->i2c.present) {
        AcpiResources::I2c& i = out->i2c;
        i.ten_bit = (le16(d, body + 4) & 1) != 0;
        i.speed_hz = le32(d, body + 9);
        i.address = le16(d, body + 13);
        if (!source_string(d, body + 9 + type_data, end, i.controller, sizeof i.controller)) return false;
        i.present = true;
      }
    }
    at = end;
  }
  return true;
}

}  // namespace hardware
