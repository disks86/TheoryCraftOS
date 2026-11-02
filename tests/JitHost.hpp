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
// Executable memory for the host tests: ICodeMemory on mmap/mprotect, with the W^X switch the kernel's implementation will do with page tables.
#pragma once
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include "Jit/CodeMemory.hpp"

namespace jit {

class HostCodeMemory final : public ICodeMemory {
 public:
  std::atomic<int> live{0};
  uint64_t page_size() const override { return static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }
  bool allocate(uint64_t size, CodeRegion* out) override {
    const uint64_t page = page_size();
    const uint64_t rounded = (size + page - 1) / page * page;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    out->bytes = ByteView(static_cast<uint8_t*>(p), rounded);
    ++live;
    return true;
  }
  bool make_executable(const CodeRegion& region) override {
    flush_instruction_cache(region.bytes);
    return mprotect(region.bytes.data(), region.bytes.size(), PROT_READ | PROT_EXEC) == 0;
  }
  bool make_writable(const CodeRegion& region) override { return mprotect(region.bytes.data(), region.bytes.size(), PROT_READ | PROT_WRITE) == 0; }
  void release(const CodeRegion& region) override {
    munmap(region.bytes.data(), region.bytes.size());
    --live;
  }
  bool can_execute() const override { return true; }
};

}  // namespace jit
