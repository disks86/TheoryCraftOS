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

#include "Hardware/WirelessInterface/Intel/IwlErrorLog.hpp"

namespace hardware {

bool iwl_read_error_log(IwlPcieDevice& device, uint32_t table, IwlErrorLog* out) {
  *out = IwlErrorLog();
  if (table == 0 || (table & 3) != 0 || table == 0xFFFFFFFFu) return false;
  if (!device.grab_nic_access()) return false;
  device.read_mem(table, out->words, IwlErrorLog::kWords);
  device.release_nic_access();
  return true;
}

uint32_t iwl_format_error_log(const IwlErrorLog& log, char* out, uint32_t capacity) {
  if (capacity == 0) return 0;
  uint32_t n = 0;
  auto text = [&](const char* s) { while (*s && n + 1 < capacity) out[n++] = *s++; };
  auto hex = [&](uint32_t v) {
    text("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
      const uint32_t digit = (v >> shift) & 0xF;
      if (n + 1 < capacity) out[n++] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
    }
  };
  if (!log.valid()) {
    text("firmware stopped without an error log");
  } else {
    text("firmware error ");
    hex(log.error_id());
    text(": data ");
    hex(log.data1());
    text(" ");
    hex(log.data2());
    text(" ");
    hex(log.data3());
    text(", ilink2 ");
    hex(log.ilink2());
    text(", blink2 ");
    hex(log.blink2());
    text(", hcmd ");
    hex(log.hcmd());
    text(", last command ");
    hex(log.last_command_id());
  }
  out[n] = 0;
  return n;
}

}  // namespace hardware
