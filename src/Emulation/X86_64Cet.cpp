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
// calls and returns' mode and privilege-change cases, interrupts, supervisor shadow stacks (setssbsy and clrssbsy are #UD without them) and indirect branch tracking (endbr is a nop).
//
// enqcmd needs a valid PASID (IA32_PASID bit 31, set by the host with set_pasid()), otherwise it is #GP(0), as on a machine whose operating system has not set one up. The 64-byte command is
// written to the destination (64-byte aligned, the address in the register) with its first dword replaced by the PASID; ordinary memory accepts every command, so ZF is clear. enqcmds is the
// supervisor form: #GP(0) at CPL 3. The register forms of the same opcode are urdmsr and uwrmsr (user MSRs, listed by the host with set_user_msr(); the map 7 immediate forms are decoded in the VEX code).
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {
constexpr uint8_t kControlProtection = 21;
constexpr uint32_t kNearRet = 1, kFarRetOrIret = 2, kRstorssp = 4;
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

// A far call pushes CS and then the return address on the shadow stack; a far return or iret pops the address and then CS and compares them with the ones it takes from the data stack (error code 2).
bool X86_64Cpu::cet_push_far(GuestMemory& m, uint16_t selector, uint64_t address) {
  if (!store(m, cet_.ssp - 8, 8, selector) || !store(m, cet_.ssp - 16, 8, address)) return false;
  cet_.ssp -= 16;
  return true;
}

bool X86_64Cpu::cet_check_far(GuestMemory& m, uint16_t selector, uint64_t address) {
  uint64_t expected_address = 0, expected_selector = 0;
  if (!load(m, cet_.ssp, 8, &expected_address) || !load(m, cet_.ssp + 8, 8, &expected_selector)) return false;
  if (expected_address != address || expected_selector != selector) {
    fail(kControlProtection, 0, kFarRetOrIret);
    return false;
  }
  cet_.ssp += 16;
  return true;
}

// rstorssp (F3 0F 01 /5, m64): switch to the shadow stack whose restore token is at the operand. The token (the address 8 above the operand with bit 0 set for 64-bit mode) is replaced by a "previous
// SSP" token holding the current pointer with bits 0 and 1 set, so saveprevssp can later turn it into a restore token for the stack being left. An invalid token is #CP, with the token put back.
X86_64Cpu::Step X86_64Cpu::rstorssp_instruction(GuestMemory& m) {
  if (!cet_.enabled) return fail(6);
  const uint64_t address = memory_address();
  if (address % 8 != 0) return fail(13);
  uint64_t token = 0;
  if (!load(m, address, 8, &token)) return Step::Fault;
  const bool valid = (token & 1) && !(token & 2) && ((((token & ~uint64_t{1}) - 8) & ~uint64_t{7}) == address);
  if (!store(m, address, 8, valid ? (cet_.ssp | 3) : token)) return Step::Fault;
  if (!valid) return fail(kControlProtection, 0, kRstorssp);
  cet_.ssp = address;
  s_.rflags &= ~(kZero | kParity | kAdjust | kOverflow | kSign | kCarry);
  if (token & 4) s_.rflags |= kCarry;      // (an alignment hole)
  s_.rip += d_.position;
  return Step::Ok;
}

// saveprevssp (F3 0F 01 EA): pop the previous-SSP token that rstorssp left on the (new) shadow stack and write a restore token for the old one just below its top, so the old stack can be
// switched back to.
X86_64Cpu::Step X86_64Cpu::saveprevssp_instruction(GuestMemory& m) {
  if (!cet_.enabled) return fail(6);
  if (cet_.ssp % 8 != 0) return fail(13);
  uint64_t token = 0;
  if (!load(m, cet_.ssp, 8, &token)) return Step::Fault;
  if (s_.rflags & kCarry) return fail(13);      // (an alignment hole cannot occur in 64-bit mode)
  if (!(token & 2)) return fail(13);
  const uint64_t old_ssp = token & ~uint64_t{3};
  if (!store(m, old_ssp - 4, 4, 0) || !store(m, (old_ssp - 4) & ~uint64_t{7}, 8, old_ssp | 1)) return Step::Fault;
  cet_.ssp += 8;
  s_.rip += d_.position;
  return Step::Ok;
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
  if (d_.mod == 3) return d_.rex_w ? user_msr_instruction(supervisor, false, 0) : fail(6);      // urdmsr (F2) and uwrmsr (F3) share the opcode with enqcmd and enqcmds
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

bool X86_64Cpu::set_user_msr(uint32_t index, uint64_t value, bool writable) {
  UserMsr* free_slot = nullptr;
  for (UserMsr& entry : user_msrs_) {
    if (entry.present && entry.index == index) { entry.value = value; entry.writable = writable; return true; }
    if (!entry.present && free_slot == nullptr) free_slot = &entry;
  }
  if (free_slot == nullptr) return false;
  *free_slot = UserMsr{true, writable, index, value};
  return true;
}

bool X86_64Cpu::user_msr(uint32_t index, uint64_t* value) const {
  for (const UserMsr& entry : user_msrs_)
    if (entry.present && entry.index == index) { *value = entry.value; return true; }
  return false;
}

// urdmsr r64, r64 (F2 REX.W 0F 38 F8, mod = 3: the MSR index in ModRM.reg, the destination ModRM.rm), uwrmsr r64, r64 (F3: the index in ModRM.reg, the value in ModRM.rm), and the VEX / EVEX map 7
// forms with a 32-bit immediate index (the register is ModRM.rm). Only the MSRs the host listed are reachable; anything else, or a write to a read-only one, is #GP(0) (the user MSR table of
// IA32_USER_MSR_CTL is not modelled beyond that).
X86_64Cpu::Step X86_64Cpu::user_msr_instruction(bool write, bool use_immediate, uint32_t immediate) {
  const uint64_t index = use_immediate ? immediate : get_reg(d_.reg, 8);
  UserMsr* found = nullptr;
  for (UserMsr& entry : user_msrs_)
    if (entry.present && entry.index == index && index <= 0xFFFFFFFFu) found = &entry;
  if (found == nullptr || (write && !found->writable)) return fail(13);
  if (write) found->value = get_reg(d_.rm, 8);
  else set_reg(d_.rm, 8, found->value);
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
