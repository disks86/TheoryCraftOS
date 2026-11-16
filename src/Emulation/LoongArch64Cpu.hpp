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
#include "Emulation/GuestCpu.hpp"
#include "Emulation/SoftFloat.hpp"

namespace emulation {

// A LoongArch LA64 user-mode interpreter: the base integer instructions (arithmetic, shifts, bit strings, byte and bit reversal, multiply and divide, CRC, the
// bound-checked loads and stores), LL/SC and the AM* atomics (with the byte, halfword and compare-and-swap forms of version 1.1), and single and double precision
// floating point (soft-float, with the condition flags and fcsr0's enables, rounding mode, flags and cause). The vector extensions (LSX, LASX), binary
// translation (LBT) and everything privileged (CSRs, IOCSR, TLB and cache instructions) are not there: those stop the program with an invalid-instruction
// fault, and CPUCFG does not announce them.
//
// Source files: LoongArch64Cpu.cpp (this class's core, integer, memory, branches, atomics) and LoongArch64Fp.cpp (floating point).
class LoongArch64Cpu final : public IGuestCpu {
 public:
  struct State {
    uint64_t r[32] = {};      // r0 is always zero
    uint64_t pc = 0;
    uint64_t f[32] = {};      // a single-precision value is in the low half
    uint8_t fcc[8] = {};      // the condition flags, 0 or 1 each
    uint32_t fcsr = 0;        // fcsr0: enables in bits 4:0, rounding mode in 9:8, flags in 20:16, cause in 28:24
  };
  static constexpr uint32_t kFcsrBits = 0x1F1F031Fu;

  process::UserArch arch() const override { return process::UserArch::LoongArch64; }
  void reset(const process::ProgramStart& start) override;
  StopReason run(GuestMemory& memory, uint64_t max_instructions) override;
  bool pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const override;
  void finish_syscall(uint64_t result) override { s_.r[4] = result; }
  void set_thread_pointers(bool fs_changed, uint64_t fs, bool, uint64_t) override {
    if (fs_changed) s_.r[2] = fs;      // (the generic thread-pointer slot is tp)
  }
  const GuestFault& fault() const override { return fault_; }
  uint64_t instructions() const override { return retired_; }
  UniquePointer<IGuestCpu> clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const override;
  bool get_signal_context(process::linuxabi::LinuxSignalContext* out) const override;
  bool set_signal_context(const process::linuxabi::LinuxSignalContext& context) override;

  State& state() { return s_; }
  const State& state() const { return s_; }

 private:
  enum class Step : uint8_t { Ok, Syscall, Fault };

  Step fail(uint8_t vector, uint64_t address = 0, uint32_t error = 0);
  Step undefined() { return fail(6); }
  Step step(GuestMemory& m);
  Step execute(GuestMemory& m, uint32_t insn);
  Step register_operation(uint32_t insn);                    // major opcode 0: everything that only touches registers
  Step memory_indexed(GuestMemory& m, uint32_t insn);        // 0x38......: indexed and bound-checked loads and stores, the atomics, the barriers
  Step atomic(GuestMemory& m, uint32_t insn);
  Step floating(uint32_t insn);                              // LoongArch64Fp.cpp: 0x01......: arithmetic, moves, conversions
  Step floating_fused(uint32_t insn);                        // LoongArch64Fp.cpp: fmadd and friends, fcmp, fsel

  void set_r(uint32_t r, uint64_t v) {
    if (r != 0) s_.r[r] = v;
  }
  bool load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out);
  bool store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value);

  // floating point helpers (LoongArch64Fp.cpp)
  softfloat::Env fp_env() const;
  // Records what an operation raised: false (and a fault) if one of the exceptions is enabled, in which case the instruction must not write its result.
  bool fp_finish(const softfloat::Env& env, uint32_t ignore = 0);
  void write_single(uint32_t r, uint64_t bits) { s_.f[r] = 0xFFFFFFFF00000000ull | (bits & 0xFFFFFFFFu); }

  State s_;
  GuestFault fault_;
  uint64_t retired_ = 0;
  uint64_t instruction_start_ = 0;
  bool reservation_valid_ = false;
  uint64_t reservation_address_ = 0, reservation_value_ = 0;
};

}  // namespace emulation
