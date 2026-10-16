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

#include "console.hpp"
#include "GraphicsConsole.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Hardware/TextConsole.hpp"

namespace {
// constexpr-constructed: no static initializers run in this freestanding image.
hardware::DebugConsole g_debug;
hardware::TextConsole g_text;
GraphicsConsole g_graphics;

void putc(char c) {
  g_debug.put(c);
  if (g_graphics.attached()) g_graphics.put(c);
  else g_text.put(c);
}
}  // namespace

namespace console {
void init(efi::SystemTable* st) {
  g_debug.initialize();
  g_text.bind(st);
  g_text.initialize();
}

void use_gpu(hardware::IGraphicsProcessingUnit* gpu) { g_graphics.attach(gpu); }

void puts(const char* s) {
  while (*s) putc(*s++);
}

void put_hex(uint64_t v) {
  puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    putc("0123456789abcdef"[(v >> shift) & 0xF]);
}

void put_dec(int64_t v) {
  char digits[21];
  int n = 0;
  uint64_t magnitude = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
  do digits[n++] = static_cast<char>('0' + magnitude % 10); while (magnitude /= 10);
  if (v < 0) putc('-');
  while (n) putc(digits[--n]);
}
}  // namespace console
