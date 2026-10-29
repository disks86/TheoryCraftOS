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

// Intel APX: the EVEX map 4 instructions, which are the legacy integer instructions promoted to the EVEX prefix (new data destination, no-flags, zero-upper forms and the 32 general registers)
// and the new PUSH2 / POP2, CCMPscc / CTESTscc and CFCMOVcc. The specification is Intel's "Advanced Performance Extensions Architecture Specification" (355828), chapter 3.
//
// Almost every promoted instruction does what the legacy instruction does, so it is run by the legacy code: the prefix bits are turned into the same decoder state a REX2 prefix would give, the legacy
// handler for the opcode (possibly a different one: tzcnt moved from 0F BC to map 4 F4) executes, and then the results are rearranged. NF restores the flags afterwards. An NDD form snapshots the
// legacy destination (a register, or the memory operand), lets the legacy instruction write it, moves the result to the new destination register, zeroed above the operand size, and puts the old
// destination value back. Not promoted here yet: the map 4 forms of adcx / adox, crc32, movbe, movdiri, movdir64b, enqcmd, wrss / wruss, invept / invvpid / invpcid, urdmsr / uwrmsr, and the EVEX
// extension of the EVEX and VEX instructions (so a vector instruction cannot name R16..R31 as a base or index register yet).
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {

constexpr uint64_t kStatus = 0x8D5;      // CF PF AF ZF SF OF

uint64_t size_mask(uint32_t size) { return size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (size * 8)) - 1; }

}  // namespace

X86_64Cpu::Step X86_64Cpu::apx_map4(GuestMemory& m, uint8_t p0, uint8_t p1, uint8_t p2, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (d_.operand16 || d_.rep || d_.repne || d_.rex_present) return fail(6);
  if (d_.position >= d_.available) return bad_decode();
  const uint8_t modrm = d_.bytes[d_.position];
  const bool is_reg = (modrm >> 6) == 3;
  const uint32_t reg3 = (modrm >> 3) & 7;
  const bool w = p1 >> 7, u = p1 & 4;
  const uint32_t pp = p1 & 3;
  if (is_reg && !u) return fail(6);
  if (p2 & 0xE0) return fail(6);      // EVEX.z and L'L must be zero
  d_.rex_present = true;
  d_.rex_w = w;
  d_.rex_r = !(p0 & 0x80);
  d_.rex_x = !(p0 & 0x40);
  d_.rex_b = !(p0 & 0x20);
  d_.rex_r4 = !(p0 & 0x10);
  d_.rex_b4 = p0 & 8;
  d_.rex_x4 = !is_reg && !u;
  d_.rex2 = true;
  d_.operand16 = pp == 1 && !w;
  const uint32_t osize = static_cast<uint32_t>(operand_size());

  // --- CCMP, CTEST: the low four bits of the third payload byte are the condition, bits 6:3 of the second the default flags ---
  const bool is_ccmp_op = (op >= 0x38 && op <= 0x3B) || ((op == 0x80 || op == 0x81 || op == 0x83) && reg3 == 7);
  const bool is_ctest_op = op == 0x84 || op == 0x85 || ((op == 0xF6 || op == 0xF7) && reg3 < 2);
  if (is_ccmp_op || is_ctest_op) {
    if (p2 & 0x10) return fail(6);      // ND must be zero
    const bool byte_op = op == 0x38 || op == 0x3A || op == 0x80 || op == 0x84 || op == 0xF6;
    if (pp > 1 || (byte_op && pp != 0)) return fail(6);
    const uint32_t scc = p2 & 15;
    const uint64_t saved = s_.rflags;
    const bool taken = scc == 0xA ? true : scc == 0xB ? false : condition(scc);
    const Step r = step_op(m, op);      // (the legacy cmp / test: reads the operands, faults like it, advances rip)
    if (r != Step::Ok) return r;
    if (!taken) {
      uint64_t f = saved & ~kStatus;
      if (p1 & 0x40) f |= kOverflow;
      if (p1 & 0x20) f |= kSign;
      if (p1 & 0x10) f |= kZero;
      if (p1 & 0x08) f |= kCarry | kParity;      // (PF takes the value of CF, as the specification says)
      s_.rflags = f;
    }
    return Step::Ok;
  }

  if (p2 & 3) return fail(6);      // the two low payload bits are reserved
  const bool nd = p2 & 0x10, nf = p2 & 4;
  const uint32_t v = ((~(p1 >> 3)) & 15) | ((p2 & 8) ? 0u : 16u);

  // --- PUSH2, POP2 ---
  if ((op == 0xFF && reg3 == 6) || (op == 0x8F && reg3 == 0)) {
    if (!is_reg || !nd || nf || pp != 0) return fail(6);
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    const uint32_t b = d_.rm;
    if (b == 4 || v == 4) return fail(6);
    if (s_.r[4] & 15) return fail(13);
    if (op == 0xFF) {      // push v, then push b
      const uint64_t sp = s_.r[4];
      if (!store(m, sp - 8, 8, s_.r[v]) || !store(m, sp - 16, 8, s_.r[b])) return Step::Fault;
      s_.r[4] = sp - 16;
    } else {      // pop v, then pop b
      if (b == v) return fail(6);
      uint64_t first = 0, second = 0;
      if (!load(m, s_.r[4], 8, &first) || !load(m, s_.r[4] + 8, 8, &second)) return Step::Fault;
      s_.r[v] = first;
      s_.r[b] = second;
      s_.r[4] += 16;
    }
    s_.rip += d_.position;
    return Step::Ok;
  }

  // --- CMOVcc, CFCMOVcc, SETcc ---
  if (op >= 0x40 && op <= 0x4F) {
    if (pp == 3) {      // setcc: ND is the zero-upper control
      if (nf || v != 0) return fail(6);
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      const uint64_t value = condition(op & 15) ? 1 : 0;
      if (d_.mod == 3) {
        if (nd) s_.r[d_.rm] = value;
        else set_reg(d_.rm, 1, value);
      } else if (!store(m, memory_address(), 1, value)) {
        return Step::Fault;
      }
      s_.rip += d_.position;
      return Step::Ok;
    }
    if (pp > 1) return fail(6);
    if (!nd && v != 0) return fail(6);
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    const bool taken = condition(op & 15);
    const uint64_t mask = size_mask(osize);
    const uint64_t reg_value = s_.r[d_.reg] & mask;
    uint64_t value = 0;
    if (!nd && !nf) {      // CFCMOVcc reg, r/m
      if (taken && !read_rm(m, osize, &value)) return Step::Fault;
      s_.r[d_.reg] = taken ? value & mask : 0;
    } else if (!nd) {      // CFCMOVcc r/m, reg
      if (taken) {
        if (d_.mod == 3) s_.r[d_.rm] = reg_value;
        else if (!store(m, memory_address(), osize, reg_value)) return Step::Fault;
      } else if (d_.mod == 3) {
        s_.r[d_.rm] = 0;
      }
    } else if (!nf) {      // CMOVcc ndd, reg, r/m (the memory operand is always read)
      if (!read_rm(m, osize, &value)) return Step::Fault;
      s_.r[v] = (taken ? value : reg_value) & mask;
    } else {      // CFCMOVcc ndd, reg, r/m
      if (taken) {
        if (!read_rm(m, osize, &value)) return Step::Fault;
        s_.r[v] = value & mask;
      } else {
        s_.r[v] = reg_value;
      }
    }
    s_.rip += d_.position;
    return Step::Ok;
  }

  // --- everything else: the legacy instruction, rearranged ---
  struct Info { bool zero_upper = false, nd_ok = false, nf_ok = false, byte_op = false, dest_reg = false, zero_map = true, rep = false; uint8_t legacy = 0; uint32_t immediate = 0; };
  Info info;
  info.legacy = op;
  bool known = true;
  const bool alu_row = op < 0x38 && (op & 7) < 4;
  if (alu_row) {
    const uint32_t kind = (op >> 3) & 7;
    info.nd_ok = true;
    info.nf_ok = kind != 2 && kind != 3;
    info.byte_op = !(op & 1);
    info.dest_reg = op & 2;
  } else if (op == 0x80 || op == 0x81 || op == 0x83) {      // reg 7 (cmp) was ccmp above
    info.nd_ok = true;
    info.nf_ok = reg3 != 2 && reg3 != 3;
    info.byte_op = op == 0x80;
    info.immediate = op == 0x81 ? (osize == 2 ? 2 : 4) : 1;
  } else if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
    info.nd_ok = true;
    info.nf_ok = reg3 != 2 && reg3 != 3;
    info.byte_op = !(op & 1);
    info.immediate = op <= 0xC1 ? 1 : 0;
  } else if (op == 0xF6 || op == 0xF7) {      // reg 0 and 1 were ctest above
    info.byte_op = op == 0xF6;
    if (reg3 == 2) info.nd_ok = true;
    else if (reg3 == 3) info.nd_ok = info.nf_ok = true;
    else info.nf_ok = true;
  } else if (op == 0xFE || op == 0xFF) {
    if (reg3 > 1) known = false;
    info.nd_ok = info.nf_ok = true;
    info.byte_op = op == 0xFE;
  } else if (op == 0x69 || op == 0x6B) {
    info.zero_upper = true;
    info.nf_ok = true;
    info.dest_reg = true;
    info.immediate = op == 0x69 ? (osize == 2 ? 2 : 4) : 1;
  } else if (op == 0xAF || op == 0xA5 || op == 0xAD || op == 0x24 || op == 0x2C) {
    info.zero_map = false;
    info.nd_ok = info.nf_ok = true;
    info.dest_reg = op == 0xAF;
    info.legacy = op == 0x24 ? 0xA4 : op == 0x2C ? 0xAC : op;
    info.immediate = (op == 0x24 || op == 0x2C) ? 1 : 0;
  } else if (op == 0xF4 || op == 0xF5 || op == 0x88) {
    info.zero_map = false;
    info.nf_ok = true;
    info.dest_reg = true;
    info.rep = true;
    info.legacy = op == 0xF4 ? 0xBC : op == 0xF5 ? 0xBD : 0xB8;
  } else {
    known = false;
  }
  if (!known) return fail(6);
  if (pp > 1 || (info.byte_op && pp != 0)) return fail(6);
  if (nf && !info.nf_ok) return fail(6);
  if (nd && !info.nd_ok && !info.zero_upper) return fail(6);
  const bool ndd = nd && info.nd_ok;
  if (!ndd && v != 0) return fail(6);
  const uint32_t size = info.byte_op ? 1 : osize;
  const uint64_t mask = size_mask(size);
  d_.rep = info.rep;

  // Where the legacy instruction will write: the reg field, or the r/m operand.
  uint64_t old_memory = 0, address = 0;
  bool dest_in_memory = false;
  uint32_t dest_register = 0;
  if (ndd) {
    const uint32_t start = d_.position;
    if (!decode_modrm()) return bad_decode();
    if (info.immediate) next_imm(info.immediate, false);
    if (!finish_decode()) return bad_decode();
    dest_in_memory = !info.dest_reg && d_.mod != 3;
    dest_register = info.dest_reg ? d_.reg : d_.rm;
    if (dest_in_memory) {
      address = memory_address();
      if (!load(m, address, size, &old_memory)) return Step::Fault;
    }
    d_.position = start;
    d_.truncated = false;
  }
  const uint64_t saved_flags = s_.rflags;
  const uint64_t old_register = ndd && !dest_in_memory ? s_.r[dest_register] : 0;
  const Step r = info.zero_map ? step_op(m, info.legacy) : step_0f_op(m, info.legacy);
  if (r != Step::Ok) return r;
  if (nf) s_.rflags = (s_.rflags & ~kStatus) | (saved_flags & kStatus);
  if (ndd) {
    uint64_t result = 0;
    if (dest_in_memory) {
      if (!load(m, address, size, &result)) return Step::Fault;
      if (!store(m, address, size, old_memory)) return Step::Fault;
    } else {
      result = s_.r[dest_register];
      s_.r[dest_register] = old_register;
    }
    s_.r[v] = result & mask;
  } else if (info.zero_upper && nd) {
    s_.r[d_.reg] &= mask;
  }
  return Step::Ok;
}

}  // namespace emulation
