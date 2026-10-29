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

// Intel CET shadow stacks for user-mode code, and the instructions around them: wrss, wruss, rdsspd/q, incsspd/q, and the call / ret checks (control protection fault, vector 21, for a mismatched
// return); plus enqcmd / enqcmds (enqueue a 64-byte command to a device).
//
// Shadow stacks are off until the host turns them on with enable_shadow_stack(): then every near call also pushes the return address on the shadow stack, every near ret compares its target with the
// shadow stack entry and raises #CP when they differ, and rdssp / incssp / wrss work (wrss only if the host allowed it, as IA32_U_CET.WR_SHSTK_EN would). With shadow stacks off those instructions
// are what they are on a processor without CET enabled: rdssp and incssp do nothing, wrss and wruss are #UD. Not modelled: the shadow stack page type (the stack is ordinary guest memory), far
// calls and returns, interrupts and iret, the restore tokens (rstorssp, saveprevssp, setssbsy, clrssbsy) and indirect branch tracking (endbr is a nop).
//
// enqcmd needs a valid PASID (IA32_PASID bit 31, set by the host with set_pasid()), otherwise it is #GP(0), as on a machine whose operating system has not set one up. The 64-byte command is
// written to the destination (64-byte aligned, the address in the register) with its first dword replaced by the PASID; ordinary memory accepts every command, so ZF is clear. enqcmds is the
// supervisor form: #GP(0) at CPL 3.
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {
constexpr uint8_t kControlProtection = 21;
constexpr uint32_t kNearRet = 1;
}  // namespace

// Pushes a return address on the shadow stack. On failure the fault is set and the shadow stack pointer is unchanged.
bool X86_64Cpu::cet_push(GuestMemory& m, uint64_t value) {
  if (!store(m, cet_.ssp - 8, 8, value)) return false;
  cet_.ssp -= 8;
  return true;
}

// Checks a near return target against the shadow stack top and pops it. The caller has popped the data stack and restores it when this fails.
bool X86_64Cpu::cet_check_return(GuestMemory& m, uint64_t target) {
  uint64_t expected = 0;
  if (!load(m, cet_.ssp, 8, &expected)) return false;
  if (expected != target) {
    fail(kControlProtection, 0, kNearRet);
    return false;
  }
  cet_.ssp += 8;
  return true;
}

// rdsspd / rdsspq (F3 0F 1E /1, register form): the shadow stack pointer into the register; a nop with shadow stacks off.
X86_64Cpu::Step X86_64Cpu::rdssp_instruction() {
  if (cet_.enabled) set_reg(d_.rm, d_.rex_w ? 8 : 4, cet_.ssp);
  s_.rip += d_.position;
  return Step::Ok;
}

// incsspd / incsspq (F3 0F AE /5, register form): the shadow stack pointer moves up by the count in the low byte of the register, 4 or 8 bytes each; the last entry skipped is read first, so an unmapped
// shadow stack faults before anything changes.
X86_64Cpu::Step X86_64Cpu::incssp_instruction(GuestMemory& m) {
  if (cet_.enabled) {
    const uint64_t count = get_reg(d_.rm, d_.rex_w ? 8 : 4) & 0xFF;
    const uint64_t step = d_.rex_w ? 8 : 4;
    if (count != 0) {
      uint64_t unused = 0;
      if (!load(m, cet_.ssp + (count - 1) * step, 8, &unused)) return Step::Fault;
      cet_.ssp += count * step;
    }
  }
  s_.rip += d_.position;
  return Step::Ok;
}

// wrss (NP 0F 38 F6 /r) and wruss (66 0F 38 F5 /r): store the register to the shadow stack. wrss needs shadow stacks and WRSS enabled (else #UD) and an operand-size aligned address (#GP(0)); wruss
// writes user shadow stack memory from supervisor mode, so it is #GP(0) at CPL 3 (or #UD without shadow stacks).
X86_64Cpu::Step X86_64Cpu::wrss_instruction(GuestMemory& m, bool user) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (!cet_.enabled || (!user && !cet_.wrss) || d_.mod == 3) return fail(6);
  if (user) return fail(13);
  const uint32_t size = d_.rex_w ? 8 : 4;
  const uint64_t address = memory_address();
  if (address % size != 0) return fail(13);
  if (!store(m, address, size, get_reg(d_.reg, size))) return Step::Fault;
  s_.rip += d_.position;
  return Step::Ok;
}

// enqcmd (F2 0F 38 F8) and enqcmds (F3 0F 38 F8): the destination address in the ModRM.reg register, the 64-byte command at the ModRM.rm memory operand.
X86_64Cpu::Step X86_64Cpu::enqcmd_instruction(GuestMemory& m, bool supervisor) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (d_.mod == 3) return fail(6);
  if (supervisor || pasid_ < 0) return fail(13);
  const uint64_t source = memory_address();
  const uint64_t destination = get_reg(d_.reg, d_.address32 ? 4 : 8);
  if ((destination & 63) != 0) return fail(13);
  uint8_t command[64];
  if (!m.read(source, ByteView(command, sizeof(command)))) return fail(14, source, 0);
  const uint32_t first = static_cast<uint32_t>(pasid_) & 0xFFFFF;      // PASID in bits 19:0, the rest of the dword zero (the privilege bit is clear for enqcmd)
  for (uint32_t i = 0; i < 4; ++i) command[i] = static_cast<uint8_t>(first >> (8 * i));
  if (!m.write(destination, ConstByteView(command, sizeof(command)))) return fail(14, destination, 2);
  s_.rflags &= ~(kZero | kCarry | kParity | kAdjust | kSign | kOverflow);      // ZF = 0: accepted
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
