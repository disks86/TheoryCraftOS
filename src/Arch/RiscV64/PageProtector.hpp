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
#include "Allocators/IAllocator.hpp"
#include "Jit/PageProtector.hpp"
#include "Jit/SpinFlag.hpp"

namespace arch_riscv64 {

// What the page-table walker needs from the machine; a test supplies a pretend one.
class IPagingHost {
 public:
  virtual uint64_t satp() = 0;                 // the supervisor address translation register (of the boot hart: the firmware sets all harts alike)
  virtual bool flush_tlb_all_harts() = 0;      // SFENCE.VMA on every hart; false if one did not answer
 protected:
  IPagingHost() = default;
  ~IPagingHost() = default;
};

// The host for the hart this code runs on (Arch/RiscV64/PagingHost.cpp).
IPagingHost& cpu_paging_host();

// Changes the permissions of identity-mapped memory in the Sv39 / Sv48 / Sv57 page tables the firmware left (the RISC-V UEFI firmware runs with paging on and
// marks data no-execute). A superpage that holds the range is split into smaller pages first (the table pages come from the allocator); then the leaf entries
// of the range get "R W" or "R X" in one store each, so no entry is ever writable and executable, and every hart flushes its TLB. Not enforced when
// translation is off (satp.MODE = Bare): then nothing is protected and nothing needs to be.
//
// Limits: leaf entries with the Svnapot bit are refused; the A and D bits are set along with the permissions (a hart without hardware A/D updates would
// otherwise fault on first use). Not safe against other code changing the same tables.
class PageProtector final : public jit::IPageProtector {
 public:
  PageProtector(IPagingHost& host, allocators::IAllocator& allocator);
  bool enforced() const override { return levels_ != 0; }
  bool protect(uint64_t base, uint64_t length, jit::PageAccess access) override;

 private:
  bool protect_page(uint64_t address, jit::PageAccess access);
  bool split(uint64_t* entry, int level);

  IPagingHost& host_;
  allocators::IAllocator& allocator_;
  int levels_ = 0;            // 3, 4 or 5; 0: translation is off or the mode is unknown
  uint64_t* root_ = nullptr;
  jit::SpinFlag lock_;
};

}  // namespace arch_riscv64
