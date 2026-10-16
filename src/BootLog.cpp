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

#include "BootLog.hpp"
#include "console.hpp"

namespace bootlog {

namespace {
uint32_t g_warnings = 0, g_faults = 0;

// The lines so far, for replay() (plain arrays: no static constructors in this image).
constexpr uint32_t kHistory = 48;
struct Saved {
  Level level;
  char line[200];
};
Saved g_history[kHistory];
uint32_t g_saved = 0;

struct Style {
  const char* tag;
  uint32_t color;
};
Style style_of(Level level) {
  switch (level) {
    case Level::Ok: return {"[ ok ] ", 0x60E060};
    case Level::Warning: return {"[warn] ", 0xF0D040};
    case Level::Fault: return {"[FAIL] ", 0xFF5050};
    default: return {"[info] ", 0};
  }
}

void print(Level level, const char* line) {
  const Style style = style_of(level);
  console::set_color(style.color);
  console::puts(style.tag);
  console::set_color(0);
  console::puts(line);
  console::puts("\n");
}
}  // namespace

Entry::Entry(Level level, StringView subsystem) : level_(level) {
  text(subsystem);
  text(": ");
}

void Entry::put(char c) {
  if (length_ + 1 < sizeof(line_)) line_[length_++] = c;
}

Entry& Entry::text(StringView s) {
  for (char c : s) put(c);
  return *this;
}

Entry& Entry::dec(uint64_t v) {
  char digits[21];
  int n = 0;
  do digits[n++] = static_cast<char>('0' + v % 10); while (v /= 10);
  while (n) put(digits[--n]);
  return *this;
}

Entry& Entry::hex(uint64_t v) {
  text("0x");
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const uint32_t d = (v >> shift) & 0xF;
    if (d || started || shift == 0) {
      put("0123456789abcdef"[d]);
      started = true;
    }
  }
  return *this;
}

Entry& Entry::hex2(uint8_t v) {
  put("0123456789abcdef"[v >> 4]);
  put("0123456789abcdef"[v & 15]);
  return *this;
}

Entry& Entry::size(uint64_t bytes) {
  if (bytes >= (1ull << 30)) return dec(bytes >> 30).text(" GiB");
  if (bytes >= (1ull << 20)) return dec(bytes >> 20).text(" MiB");
  if (bytes >= (1ull << 10)) return dec(bytes >> 10).text(" KiB");
  return dec(bytes).text(" B");
}

Entry::~Entry() {
  if (level_ == Level::Warning) ++g_warnings;
  if (level_ == Level::Fault) ++g_faults;
  line_[length_] = 0;
  console::Guard guard;
  print(level_, line_);
  if (g_saved < kHistory) {
    g_history[g_saved].level = level_;
    for (uint32_t i = 0; i <= length_; ++i) g_history[g_saved].line[i] = line_[i];
    ++g_saved;
  }
}

void replay() {
  console::Guard guard;
  console::set_debug_output(false);
  for (uint32_t i = 0; i < g_saved; ++i) print(g_history[i].level, g_history[i].line);
  console::set_debug_output(true);
}

uint32_t warnings() { return g_warnings; }
uint32_t faults() { return g_faults; }

}  // namespace bootlog
