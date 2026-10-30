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
#include "Emulation/GuestCpu.hpp"
#include "Emulation/SoftFloat.hpp"

namespace emulation {

// An AArch64 (A64) user-mode interpreter: the integer instruction set, loads and stores (including pairs, exclusives and the LSE atomics), scalar floating point
// (soft-float, with fused multiply-add and the AArch64 NaN rules), and a subset of Advanced SIMD that covers what compilers emit for integer and floating-point loops
// and memcpy-style code. Unimplemented encodings (SVE, crypto, half-precision, the rarer NEON forms) stop the program with an invalid-instruction fault.
//
// Source files: AArch64Cpu.cpp (this class's core, integer, branches, loads and stores), AArch64Fp.cpp (scalar FP), AArch64Simd.cpp (Advanced SIMD).
class AArch64Cpu final : public IGuestCpu {
 public:
  struct Vec {
    uint64_t lo = 0, hi = 0;
  };
  struct State {
    uint64_t x[31] = {};
    uint64_t sp = 0, pc = 0;
    uint32_t nzcv = 0;      // N Z C V in bits 31..28, as in the NZCV register
    Vec v[32];
    uint32_t fpcr = 0, fpsr = 0;
    uint64_t tpidr_el0 = 0;
    // SVE at a vector length of 128 bits: Z0..Z31 are v[], a predicate has one bit per byte of the vector (16 bits), FFR is the first-fault register.
    uint16_t p[16] = {};
    uint16_t ffr = 0;
  };
  static constexpr uint32_t kN = 1u << 31, kZ = 1u << 30, kC = 1u << 29, kV = 1u << 28;

  process::UserArch arch() const override { return process::UserArch::AArch64; }
  void reset(const process::ProgramStart& start) override;
  StopReason run(GuestMemory& memory, uint64_t max_instructions) override;
  bool pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const override;
  void finish_syscall(uint64_t result) override { s_.x[0] = result; }
  void set_thread_pointers(bool fs_changed, uint64_t fs, bool, uint64_t) override {
    if (fs_changed) s_.tpidr_el0 = fs;      // (the generic thread-pointer slot is TPIDR_EL0 here)
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

  // registers: 31 is the zero register unless the _sp form is used
  uint64_t reg(uint32_t r, bool sf) const { return r == 31 ? 0 : (sf ? s_.x[r] : s_.x[r] & 0xFFFFFFFFu); }
  void set_reg(uint32_t r, uint64_t v, bool sf) {
    if (r != 31) s_.x[r] = sf ? v : v & 0xFFFFFFFFu;
  }
  uint64_t reg_sp(uint32_t r, bool sf) const { return r == 31 ? (sf ? s_.sp : s_.sp & 0xFFFFFFFFu) : reg(r, sf); }
  void set_reg_sp(uint32_t r, uint64_t v, bool sf) {
    if (r == 31) s_.sp = sf ? v : v & 0xFFFFFFFFu;
    else set_reg(r, v, sf);
  }

  bool load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out);
  bool store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value);
  bool load_vec(GuestMemory& m, uint64_t address, uint32_t size, Vec* out);      // size 1..16
  bool store_vec(GuestMemory& m, uint64_t address, uint32_t size, const Vec& v);

  uint64_t add_with_carry(uint64_t a, uint64_t b, uint64_t carry, bool sf, bool set_flags);
  bool condition(uint32_t cond) const;
  uint64_t shift_value(uint64_t v, uint32_t type, uint32_t amount, bool sf) const;
  uint64_t extend_value(uint64_t v, uint32_t option, uint32_t shift) const;
  void set_nzcv_from(uint64_t result, bool sf);

  Step data_immediate(uint32_t insn);
  Step branch_system(GuestMemory& m, uint32_t insn);
  Step load_store(GuestMemory& m, uint32_t insn);
  Step load_store_exclusive(GuestMemory& m, uint32_t insn);
  Step load_store_pair(GuestMemory& m, uint32_t insn);
  Step load_store_register(GuestMemory& m, uint32_t insn);
  Step load_store_ordered(GuestMemory& m, uint32_t insn);
  Step atomic_memory(GuestMemory& m, uint32_t insn);
  Step simd_load_store_multiple(GuestMemory& m, uint32_t insn);
  Step simd_load_store_single(GuestMemory& m, uint32_t insn);
  Step data_register(uint32_t insn);
  Step system_instruction(uint32_t insn);
  Step floating_point(uint32_t insn);      // AArch64Fp.cpp
  Step fjcvtzs(uint32_t rd, uint32_t rn);
  Step simd(uint32_t insn);                // AArch64Simd.cpp
  bool neon_shift_immediate(uint32_t insn, Step* result);
  bool neon_three_same(uint32_t insn, Step* result);      // AArch64NeonThree.cpp
  bool neon_by_element(uint32_t insn, Step* result);      // AArch64NeonElement.cpp
  bool neon_two_register(uint32_t insn, Step* result);    // AArch64NeonTwo.cpp
  bool neon_three_different(uint32_t insn, Step* result);      // AArch64NeonDiff.cpp      // AArch64NeonShift.cpp; these return false when the instruction is not of their class
  // SVE (AArch64Sve*.cpp); each returns false when the word is not of its class, and sve() then raises an undefined-instruction fault.
  Step sve(GuestMemory& m, uint32_t insn);
  bool sve_integer(uint32_t insn);          // 0x04: integer arithmetic, reductions, shifts, counting, index
  bool sve_permute(uint32_t insn);          // 0x05: dup, permutes, select, table lookup, last/first element
  bool sve_predicate(uint32_t insn);        // 0x24 / 0x25: compares, predicate logic, while, counts, immediates
  bool sve2(uint32_t insn);                 // 0x44 / 0x45: SVE2 integer
  bool sve_float(uint32_t insn);            // 0x64 / 0x65: floating point
  Step sve_memory(GuestMemory& m, uint32_t insn);      // 0x84 .. 0xE5: loads and stores
  void sve_set_flags(uint16_t mask, uint16_t result, uint32_t esz);      // PredTest
  bool crypto(uint32_t insn, Step* result);      // AArch64Crypto.cpp: AES, SHA, SM3, SM4 and pmull; false when the instruction is not one of them

  // floating point helpers (AArch64Fp.cpp)
  softfloat::Env fp_env() const;
  void fp_finish(const softfloat::Env& env);
  uint64_t fp_read(uint32_t r, softfloat::Format f) const {
    return f == softfloat::Format::F32 ? s_.v[r].lo & 0xFFFFFFFFu : (f == softfloat::Format::F16 || f == softfloat::Format::BF16) ? s_.v[r].lo & 0xFFFF : s_.v[r].lo;
  }
  void fp_write(uint32_t r, softfloat::Format f, uint64_t bits);      // a scalar write clears the rest of the register
  uint64_t fp_default_nan_fix(softfloat::Format f, uint64_t bits) const;

  State s_;
  GuestFault fault_;
  uint64_t retired_ = 0;
  uint64_t instruction_start_ = 0;
  bool exclusive_valid_ = false;
  uint64_t exclusive_address_ = 0;
};

}  // namespace emulation
