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

#include "Utility/Bounds.hpp"
#include "console.hpp"
#include "Kernel/Interrupts.hpp"

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  kernel::irq_disable();
  console::break_lock();
  console::puts("\n*** BOUNDS VIOLATION: ");
  console::puts(what);
  console::puts(" (index/offset ");
  console::put_dec(static_cast<int64_t>(index));
  console::puts(", size ");
  console::put_dec(static_cast<int64_t>(size));
  console::puts(")\n");
  kernel::panic("out-of-bounds access stopped", nullptr);
}
