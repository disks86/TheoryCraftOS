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

// Stand-ins for the clock and the debug console, which the desktop's 3D test window uses, and the logo the boot image carries (Logo.S), so the desktop
// compiles on the development machine. (The tests run from the repository's root.)
#include <stdint.h>
#include "Utility/StringView.hpp"

extern "C" {
extern const uint8_t tcos_logo_start[];
extern const uint8_t tcos_logo_end[];
}
asm(".section .rodata\n.global tcos_logo_start\n.global tcos_logo_end\ntcos_logo_start:\n.incbin \"assets/logo.png\"\ntcos_logo_end:\n.previous\n");

namespace hardware {
uint64_t uptime_us() { return 0; }
}
namespace console {
void debug_puts(StringView) {}
}
