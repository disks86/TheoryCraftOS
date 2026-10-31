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

// The physical side of a RISC-V machine, for the privileged (system) mode: memory and devices behind physical addresses. An access that no device claims returns false, which the
// CPU reports as an access fault.
class IRiscVBus {
 public:
  virtual bool read(uint64_t address, uint32_t size, uint64_t* out) = 0;
  virtual bool write(uint64_t address, uint32_t size, uint64_t value) = 0;
  virtual uint64_t time() = 0;      // the `time` CSR and the CLINT's mtime
  virtual uint64_t interrupts() = 0;      // the pending bits the hardware drives: MSIP (3), MTIP (7), SEIP (9), MEIP (11)
  virtual void tick() = 0;      // one instruction retired
  virtual void idle(uint64_t wake_time) = 0;      // `wfi` with nothing pending: let time pass, up to the CPU's own next timer (wake_time, or ~0) or the bus's
 protected:
  ~IRiscVBus() = default;
};

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

  // The privileged architecture (machine, supervisor and user modes, the M and S CSRs, traps and their delegation, Sv39/Sv48/Sv57, PMP). Off by default, which is the user-mode
  // guest above. After enable_system the CPU starts in machine mode at `pc`, fetches and accesses memory through the bus, and run_system() takes traps instead of reporting
  // them. `placeholder` is only handed to the instruction code that wants a GuestMemory; it is never read in this mode. RiscV64Machine (RiscV64Machine.hpp) is a board around this.
  void enable_system(IRiscVBus& bus, GuestMemory& placeholder, uint64_t pc);
  StopReason run_system(uint64_t max_instructions);
  struct System {
    uint8_t priv = 3;      // 0 user, 1 supervisor, 3 machine
    uint64_t mstatus = 0, medeleg = 0, mideleg = 0, mie = 0, mip = 0, mtvec = 0, mscratch = 0, mepc = 0, mcause = 0, mtval = 0;
    uint64_t stvec = 0, sscratch = 0, sepc = 0, scause = 0, stval = 0, satp = 0, stimecmp = 0;
    uint64_t menvcfg = 0, senvcfg = 0, mcounteren = 0, scounteren = 0, mcountinhibit = 0, mcycle = 0, minstret = 0;
    uint64_t pmpaddr[16] = {};
    uint8_t pmpcfg[16] = {};
  };
  System& system_state() { return sys_; }
  const System& system_state() const { return sys_; }
  bool system_mode() const { return bus_ != nullptr; }

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

  // privileged mode (RiscV64Privileged.cpp)
  enum class Access : uint8_t { Fetch, Load, Store };
  struct Translation { uint64_t physical; uint8_t cause; bool ok; };
  Translation translate(uint64_t address, Access access, uint32_t size);
  bool pmp_allows(uint64_t physical, uint32_t size, Access access, uint8_t priv) const;
  bool sys_read(uint64_t address, uint32_t size, Access access, uint64_t* out);
  bool sys_write(uint64_t address, uint32_t size, uint64_t value);
  void take_trap(uint64_t cause, uint64_t tval, bool interrupt);
  bool take_interrupt();
  uint64_t pending_interrupts();
  Step sys_csr(uint32_t insn);
  bool sys_csr_read(uint32_t csr, uint64_t* out);
  bool sys_csr_write(uint32_t csr, uint64_t value);
  Step sys_instruction(uint32_t insn);      // mret, sret, wfi, sfence.vma
  Step sys_fail(uint8_t vector, uint64_t address);
  uint32_t sys_unit(uint32_t insn) const;      // 0: neither; 1: floating point; 2: vector; +4: reads only (no dirty)
  bool sys_unit_enabled(uint32_t unit) const;
  void sys_unit_dirty(uint32_t unit);

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
  uint64_t read_half(uint32_t r) const { return (s_.f[r] >> 16) == 0xFFFFFFFFFFFFull ? (s_.f[r] & 0xFFFFu) : 0x7E00u; }      // (Zfh: an unboxed half is the canonical NaN)
  void write_half(uint32_t r, uint64_t bits) { s_.f[r] = 0xFFFFFFFFFFFF0000ull | (bits & 0xFFFFu); }

  State s_;
  System sys_;
  IRiscVBus* bus_ = nullptr;
  GuestMemory* placeholder_ = nullptr;
  uint32_t raw_instruction_ = 0;      // the instruction as fetched (the 16 bits of a compressed one), for xtval
  uint64_t trap_cause_ = 0, trap_tval_ = 0;
  bool jumped_ = false;      // mret / sret set the pc themselves
  bool amo_ = false;      // inside an atomic: its load part checks for write permission
  GuestFault fault_;
  uint64_t retired_ = 0;
  uint64_t instruction_start_ = 0;
  bool reservation_valid_ = false;
  uint64_t reservation_address_ = 0;
};

}  // namespace emulation
