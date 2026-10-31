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
#include "Emulation/RiscV64Cpu.hpp"
#include "Utility/ByteBuffer.hpp"

namespace emulation {

// A board around RiscV64Cpu's privileged mode, laid out like qemu's `virt` machine where it matters to software: RAM at 0x80000000, a CLINT (software interrupt, timer) at
// 0x2000000, a 16550 UART at 0x10000000 whose output is collected, and the "test finisher" at 0x100000 that ends a run. There is one hart. Time advances with the instructions
// run (`kTicksPerTime` instructions per mtime unit), and a `wfi` with nothing pending skips ahead to the next timer.
class RiscV64Machine final : public IRiscVBus {
 public:
  static constexpr uint64_t kRamBase = 0x80000000ull, kClint = 0x2000000ull, kUart = 0x10000000ull, kFinisher = 0x100000ull;
  static constexpr uint64_t kTicksPerTime = 4;

  RiscV64Machine(allocators::IAllocator& allocator, process::AddressSpace& placeholder_space, uint64_t ram_bytes);
  bool ok() const { return ram_ok_; }
  // Copies `image` into RAM at `physical_address` (inside RAM).
  bool load(uint64_t physical_address, ConstByteView image);
  void start(uint64_t pc) { cpu_.enable_system(*this, placeholder_, pc); }
  // Runs until the guest writes the test finisher or `max_instructions` pass. True if the guest finished.
  bool run(uint64_t max_instructions);
  bool finished() const { return finished_; }
  uint32_t exit_code() const { return exit_code_; }
  ConstByteView console() const { return uart_out_.view(); }
  RiscV64Cpu& cpu() { return cpu_; }

  bool read(uint64_t address, uint32_t size, uint64_t* out) override;
  bool write(uint64_t address, uint32_t size, uint64_t value) override;
  uint64_t time() override { return ticks_ / kTicksPerTime; }
  uint64_t interrupts() override;
  void tick() override { ++ticks_; }
  void idle(uint64_t wake_time) override;

 private:
  RiscV64Cpu cpu_;
  GuestMemory placeholder_;
  ByteBuffer ram_;
  ByteBuffer uart_out_;
  bool ram_ok_ = false;
  uint64_t ram_size_;
  uint64_t ticks_ = 0, mtimecmp_ = ~uint64_t{0};
  uint32_t msip_ = 0;
  uint8_t uart_ier_ = 0, uart_lcr_ = 0, uart_mcr_ = 0, uart_scr_ = 0, uart_dll_ = 0, uart_dlm_ = 0;
  bool finished_ = false;
  uint32_t exit_code_ = 0;
};

}  // namespace emulation
