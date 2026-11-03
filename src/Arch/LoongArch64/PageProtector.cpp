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
#include "Arch/LoongArch64/PageProtector.hpp"

namespace arch_loongarch64 {

PageProtector::PageProtector() = default;

// A protector that cannot enforce accepts every call (see jit::IPageProtector): the memory keeps whatever access it has.
bool PageProtector::protect(uint64_t, uint64_t, jit::PageAccess) { return true; }

}  // namespace arch_loongarch64
