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
#include "Kernel/Thread.hpp"
#include "Kernel/RawSpinLock.hpp"

namespace {
// constexpr-constructed: no static initializers run in this freestanding image.
hardware::DebugConsole g_debug;
hardware::TextConsole g_text;
GraphicsConsole g_graphics;

// Output from several threads must not tear: one lock, taken per call (and
// held across a whole Guard block). Re-entrant for the thread that holds it; the
// holder runs with interrupts masked, so it is never descheduled while holding it.
kernel::RawSpinLock g_lock;
uint64_t g_owner = 0;  // the holder: its thread (or, before threads exist, 1 + the core); 0 = free
uint32_t g_depth = 0;

void putc(char c) {
  g_debug.put(c);
  if (g_graphics.attached()) g_graphics.put(c);
  else g_text.put(c);
}
}  // namespace

namespace console {
void lock() {
  const kernel::Thread* thread = kernel::current_thread();
  const uint64_t me = thread ? reinterpret_cast<uint64_t>(thread) : 1 + kernel::current_core();
  const uint64_t flags = kernel::irq_save();
  if (g_depth > 0 && g_owner == me) {  // nested on this core: we already hold it
    ++g_depth;
    kernel::irq_restore(flags);
    return;
  }
  kernel::irq_restore(flags);
  g_lock.lock();
  g_owner = me;
  g_depth = 1;
}

void unlock() {
  if (--g_depth == 0) {
    g_graphics.flush();  // a GPU with a separate screen copy needs the finished output pushed over
    g_owner = 0;
    g_lock.unlock();
  }
}

void break_lock() {  // for a panic: the holder may be the code that just crashed
  g_depth = 0;
  g_owner = 0;
  g_lock.reset();
}

void init(efi::SystemTable* st) {
  g_debug.initialize();
  g_text.bind(st);
  g_text.initialize();
}

void use_gpu(hardware::IGraphicsProcessingUnit* gpu) { g_graphics.attach(gpu); }

void puts(StringView text) {
  Guard guard;
  for (char c : text) putc(c);
}

void put_hex(uint64_t v) {
  Guard guard;
  puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    putc("0123456789abcdef"[(v >> shift) & 0xF]);
}

void put_dec(int64_t v) {
  Guard guard;
  char digits[21];
  int n = 0;
  uint64_t magnitude = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
  do digits[n++] = static_cast<char>('0' + magnitude % 10); while (magnitude /= 10);
  if (v < 0) putc('-');
  while (n) putc(digits[--n]);
}
}  // namespace console
