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
#include "Jit/CrossCoreTlbShootdown.hpp"
#include "Kernel/Thread.hpp"

namespace jit {

namespace {
struct Flush {
  kernel::IPagingHost* host;
  uint64_t address;
  uint64_t pages;   // 0: everything
};

void flush_this_core(void* context) {
  const Flush& f = *static_cast<const Flush*>(context);
  if (f.pages == 0) {
    f.host->invalidate_all();
    return;
  }
  for (uint64_t i = 0; i < f.pages; ++i) f.host->invalidate_page(f.address + i * kCodePage);
}
}  // namespace

bool CrossCoreTlbShootdown::invalidate_range(uint64_t address, uint64_t bytes) {
  const uint64_t start = address & ~(kCodePage - 1);
  const uint64_t end = (address + bytes + kCodePage - 1) & ~(kCodePage - 1);
  const uint64_t pages = (end - start) / kCodePage;
  Flush flush{&host_, start, pages <= kPageByPageLimit ? pages : 0};
  return kernel::run_on_every_core(flush_this_core, &flush);
}

}  // namespace jit
