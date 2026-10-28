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

#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"
#include "uefi.hpp"

// Output goes to the QEMU debug console (port 0x402) and to the screen: the
// GPU-drawn console once one is attached, the firmware text console before.
namespace console {
void init(efi::SystemTable* st);
// Routes screen output through `gpu` (clearing it). Call again after changing
// the GPU's mode. nullptr returns to the firmware text console.
void use_gpu(hardware::IGraphicsProcessingUnit* gpu);
// Draws a picture at the top left of a graphics screen and keeps the text below it (see GraphicsConsole::show_banner). Does nothing on the
// firmware text console.
void show_banner(ArrayView<const uint32_t> pixels, uint32_t width, uint32_t height);
// Color of the text that follows on a graphics screen (0x00RRGGBB, 0 = default); ignored by the
// firmware text console and the debug console.
void set_color(uint32_t rgb);
// Turns the QEMU debug console (port 0x402) output on or off; the screen is unaffected. For
// redrawing text the debug console has already seen.
void set_debug_output(bool enabled);
void puts(StringView text);  // a literal or any C string converts implicitly
// Text for the debug console only (port 0x402), never the screen: safe to use while something else is drawing.
void debug_puts(StringView text);
// Holds the console for several calls, so a line from one thread is not torn by
// output from another. Nests; always release in reverse order (use Guard).
void lock();
void unlock();
void break_lock();  // panic only
class Guard {
 public:
  Guard() { lock(); }
  ~Guard() { unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};
void put_hex(uint64_t v);
void put_dec(int64_t v);
}  // namespace console
