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

// A RISC-V RV64GC user-mode interpreter: the base integer set, M (multiply/divide), A (LR/SC and the AMOs), F and D (soft-float, with the RISC-V rules: canonical NaN
// results, NaN-boxed singles, fflags/frm in fcsr), C (compressed instructions, expanded to their 32-bit forms and executed as those), Zicsr for the floating-point and
// counter CSRs, B (Zba, Zbb, Zbs, Zbc), Zicond, the cache-block operations, V (RiscV64Vector.cpp), and fence/fence.i as no-ops. Anything else (the H extension, privileged
// instructions) stops the program with an invalid-instruction fault.
//
// Source files: RiscV64Cpu.cpp (this class's core, integer, memory, atomics, compressed expansion) and RiscV64Fp.cpp (F and D).
class RiscV64Cpu final : public IGuestCpu {
 public:
  struct State {
    uint64_t x[32] = {};
    uint64_t pc = 0;
    uint64_t f[32] = {};      // single-precision values live NaN-boxed in the low half
    uint32_t fcsr = 0;        // fflags in bits 4:0, frm in bits 7:5
    // The vector extension: VLEN = 128 bits, ELEN = 64. The 32 registers are one flat byte array (register n starts at 16 * n), which makes register groups plain slices.
    uint8_t v[32 * 16] = {};
    uint64_t vl = 0;
    uint64_t vtype = uint64_t{1} << 63;      // vill until the first vsetvli
    uint64_t vstart = 0;
    uint32_t vxrm = 0;        // fixed-point rounding mode
    uint32_t vxsat = 0;       // fixed-point saturation flag
  };

  process::UserArch arch() const override { return process::UserArch::RiscV64; }
  void reset(const process::ProgramStart& start) override;
  StopReason run(GuestMemory& memory, uint64_t max_instructions) override;
  bool pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const override;
  void finish_syscall(uint64_t result) override { s_.x[10] = result; }
  void set_thread_pointers(bool fs_changed, uint64_t fs, bool, uint64_t) override {
    if (fs_changed) s_.x[4] = fs;      // (the generic thread-pointer slot is tp)
  }
  const GuestFault& fault() const override { return fault_; }
  uint64_t instructions() const override { return retired_; }
  UniquePointer<IGuestCpu> clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const override;

  State& state() { return s_; }
  const State& state() const { return s_; }

 private:
  enum class Step : uint8_t { Ok, Syscall, Fault };

  Step fail(uint8_t vector, uint64_t address = 0);
  Step undefined() { return fail(6); }
  Step step(GuestMemory& m);
  Step execute(GuestMemory& m, uint32_t insn, uint32_t length);
  Step atomic(GuestMemory& m, uint32_t insn);
  Step system(uint32_t insn, uint32_t length);
  Step vector(GuestMemory& m, uint32_t insn);      // RiscV64Vector.cpp: OP-V (arithmetic and vsetvl)
  Step vector_memory(GuestMemory& m, uint32_t insn);      // RiscV64Vector.cpp: vector loads and stores
  friend struct VectorUnit;
  Step floating(GuestMemory& m, uint32_t insn);      // RiscV64Fp.cpp: loads, stores, fused multiply-add and OP-FP
  Step csr_access(uint32_t insn);

  void set_x(uint32_t r, uint64_t v) {
    if (r != 0) s_.x[r] = v;
  }
  bool load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out);
  bool store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value);

  // floating point helpers (RiscV64Fp.cpp)
  bool rounding_mode(uint32_t rm, softfloat::Rounding* out) const;      // false: a reserved encoding
  void fp_finish(const softfloat::Env& env);
  uint64_t read_single(uint32_t r) const;      // an unboxed single, or the canonical NaN when the register is not NaN-boxed
  void write_single(uint32_t r, uint64_t bits) { s_.f[r] = 0xFFFFFFFF00000000ull | (bits & 0xFFFFFFFFu); }

  State s_;
  GuestFault fault_;
  uint64_t retired_ = 0;
  uint64_t instruction_start_ = 0;
  bool reservation_valid_ = false;
  uint64_t reservation_address_ = 0;
};

}  // namespace emulation
