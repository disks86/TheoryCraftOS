/*
 * TheoryCraftOS - a minimal UEFI operating system.
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
#include "Jit/PageProtector.hpp"

namespace arch_loongarch64 {

// The JIT's page protection on LoongArch. UEFI hands over with direct address translation (CRMD.DA, physical = virtual),
// where the hardware has no page permissions at all: there is nothing to switch and the memory stays as the firmware
// mapped it. enforced() says so, and the code memory reports that W^X is not available. (With paging on, the tables
// would be walked here; no firmware does that yet.)
class PageProtector final : public jit::IPageProtector {
 public:
  PageProtector();
  bool enforced() const override { return false; }
  bool protect(uint64_t base, uint64_t length, jit::PageAccess access) override;
};

}  // namespace arch_loongarch64
