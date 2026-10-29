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
#include "Emulation/X86_64Cpu.hpp"
#include "Emulation/SoftFloat.hpp"
#include "Emulation/Wide.hpp"

namespace emulation {

namespace {

constexpr uint64_t mask_of(uint32_t size) { return size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (size * 8)) - 1; }
constexpr uint64_t sign_of(uint32_t size) { return uint64_t{1} << (size * 8 - 1); }
constexpr int64_t sign_extend(uint64_t value, uint32_t size) {
  return size >= 8 ? static_cast<int64_t>(value) : static_cast<int64_t>((value & mask_of(size)) ^ sign_of(size)) - static_cast<int64_t>(sign_of(size));
}

bool even_parity(uint8_t v) {
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return (v & 1) == 0;
}

void multiply_signed(int64_t a, int64_t b, uint64_t* hi, uint64_t* lo) {
  multiply_unsigned(static_cast<uint64_t>(a), static_cast<uint64_t>(b), hi, lo);
  if (a < 0) *hi -= static_cast<uint64_t>(b);
  if (b < 0) *hi -= static_cast<uint64_t>(a);
}

uint32_t popcount64(uint64_t v) {
  uint32_t n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

}  // namespace

// --- the start state -------------------------------------------------------------------------------------------------------------------------

void X86_64Cpu::reset(const process::ProgramStart& start) {
  s_ = State();
  selector_[0] = selector_[2] = selector_[3] = selector_[4] = selector_[5] = 0x2B;
  selector_[1] = 0x33;
  s_.rip = start.entry;
  s_.r[4] = start.stack_pointer;
  // Native Windows entry gets the first two arguments in RCX and RDX (RtlUserThreadStart); Linux programs start with zeroed registers.
  s_.r[1] = start.argument;
  s_.r[2] = start.argument2;
  s_.fs_base = start.fs_base;
  s_.gs_base = start.gs_base;
  s_.rflags = 0x202;
  fault_ = GuestFault();
  retired_ = 0;
  lwp_ = Lwp();
  cet_ = Cet();
  pasid_ = -1;
}

bool X86_64Cpu::pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const {
  process::X86UserRegisters regs;
  regs.rax = s_.r[0]; regs.rcx = s_.r[1]; regs.rdx = s_.r[2]; regs.rbx = s_.r[3]; regs.rsp = s_.r[4]; regs.rbp = s_.r[5];
  regs.rsi = s_.r[6]; regs.rdi = s_.r[7]; regs.r8 = s_.r[8]; regs.r9 = s_.r[9]; regs.r10 = s_.r[10]; regs.r11 = s_.r[11];
  return process::decode_x86_64(abi, regs, out);
}

// --- decoding --------------------------------------------------------------------------------------------------------------------------------

uint8_t X86_64Cpu::next_u8() {
  if (d_.position >= d_.available) {
    d_.truncated = true;
    return 0;
  }
  return d_.bytes[d_.position++];
}

uint64_t X86_64Cpu::next_imm(uint32_t bytes, bool sign_extend_it) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < bytes; ++i) v |= uint64_t{next_u8()} << (8 * i);
  return sign_extend_it ? static_cast<uint64_t>(sign_extend(v, bytes)) : v;
}

// ModRM, SIB and displacement. The effective address is completed by finish_decode(), once the immediates (which RIP-relative addressing counts in the
// instruction length) have been read.
bool X86_64Cpu::decode_modrm() {
  const uint8_t modrm = next_u8();
  d_.mod = modrm >> 6;
  d_.reg = static_cast<uint8_t>(((modrm >> 3) & 7) | (d_.rex_r ? 8 : 0) | (d_.rex_r4 ? 16 : 0));
  uint8_t rm = modrm & 7;
  if (d_.mod == 3) {
    d_.rm = static_cast<uint8_t>(rm | d_.ext_b());
    if (vex_.redirect) d_.rm = 16;      // (an AVX instruction run in halves: its register operand has been copied to the scratch slot)
    return !d_.truncated;
  }
  uint64_t address = 0;
  bool have_base = true;
  if (rm == 4) {
    const uint8_t sib = next_u8();
    const uint32_t scale = sib >> 6;
    const uint32_t index = ((sib >> 3) & 7) | (d_.rex_x ? 8 : 0) | (d_.rex_x4 ? 16 : 0);
    const uint32_t base = (sib & 7) | d_.ext_b();
    if ((sib & 7) == 5 && d_.mod == 0) have_base = false;
    else address = s_.r[base];
    if (index != 4) {      // (r12 and r20 can be indexes: only the plain 4 means none)
      d_.sib_index = true;
      d_.index_offset = s_.r[index] << scale;
      address += d_.index_offset;
    }
  } else if (rm == 5 && d_.mod == 0) {
    have_base = false;
    d_.rip_relative = true;
  } else {
    address = s_.r[rm | d_.ext_b()];
  }
  int64_t disp = 0;
  if (d_.mod == 1) disp = sign_extend(next_u8(), 1);
  else if (d_.mod == 2 || !have_base) disp = sign_extend(next_imm(4, false), 4);
  d_.displacement = disp;
  d_.ea = address + static_cast<uint64_t>(disp);
  d_.rm = rm;
  return !d_.truncated;
}

// Everything is read: a RIP-relative address is now relative to the real next instruction; the address size is applied.
bool X86_64Cpu::finish_decode() {
  if (d_.truncated) return false;
  if (d_.rip_relative) d_.ea = s_.rip + d_.position + static_cast<uint64_t>(d_.displacement);
  if (d_.address32 && d_.mod != 3) d_.ea &= 0xFFFFFFFFu;
  if (d_.mod != 3 && vex_.half) d_.ea += 16 * vex_.half;
  return true;
}

uint64_t X86_64Cpu::memory_address() const {
  const uint64_t base = d_.segment == 1 ? s_.fs_base : d_.segment == 2 ? s_.gs_base : 0;
  return d_.ea + base;
}

uint64_t X86_64Cpu::get_reg(uint32_t index, uint32_t size) const {
  if (size == 1) {
    if (!d_.rex_present && index >= 4 && index < 8) return (s_.r[index - 4] >> 8) & 0xFF;
    return s_.r[index] & 0xFF;
  }
  return s_.r[index] & mask_of(size);
}

void X86_64Cpu::set_reg(uint32_t index, uint32_t size, uint64_t value) {
  switch (size) {
    case 1:
      if (!d_.rex_present && index >= 4 && index < 8) s_.r[index - 4] = (s_.r[index - 4] & ~uint64_t{0xFF00}) | ((value & 0xFF) << 8);
      else s_.r[index] = (s_.r[index] & ~uint64_t{0xFF}) | (value & 0xFF);
      break;
    case 2: s_.r[index] = (s_.r[index] & ~uint64_t{0xFFFF}) | (value & 0xFFFF); break;
    case 4: s_.r[index] = value & 0xFFFFFFFFu; break;      // (a 32-bit write clears the upper half)
    default: s_.r[index] = value; break;
  }
}

X86_64Cpu::Step X86_64Cpu::fail(uint8_t vector, uint64_t address, uint32_t error) {
  fault_.vector = vector;
  fault_.address = address;
  fault_.error = vector == 14 ? error : 0;
  fault_.pc = instruction_start_;
  s_.rip = instruction_start_;
  return Step::Fault;
}

bool X86_64Cpu::load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out) {
  uint8_t raw[8] = {};
  if (!m.read(address, ByteView(raw, size))) {
    fail(14, address, 0);
    return false;
  }
  uint64_t v = 0;
  for (uint32_t i = 0; i < size; ++i) v |= uint64_t{raw[i]} << (8 * i);
  *out = v;
  return true;
}

bool X86_64Cpu::store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  if (!m.write(address, ConstByteView(raw, size))) {
    fail(14, address, 2);
    return false;
  }
  return true;
}

bool X86_64Cpu::read_rm(GuestMemory& m, uint32_t size, uint64_t* out) {
  if (d_.mod == 3) {
    *out = get_reg(d_.rm, size);
    return true;
  }
  return load(m, memory_address(), size, out);
}

bool X86_64Cpu::write_rm(GuestMemory& m, uint32_t size, uint64_t value) {
  if (d_.mod == 3) {
    set_reg(d_.rm, size, value);
    return true;
  }
  return store(m, memory_address(), size, value);
}

bool X86_64Cpu::read_xmm_rm(GuestMemory& m, Xmm* out) {
  if (d_.mod == 3) {
    *out = s_.xmm[d_.rm];
    return true;
  }
  const uint64_t a = memory_address();
  return load(m, a, 8, &out->lo) && load(m, a + 8, 8, &out->hi);
}

bool X86_64Cpu::write_xmm_rm(GuestMemory& m, const Xmm& value) {
  if (d_.mod == 3) {
    s_.xmm[d_.rm] = value;
    return true;
  }
  const uint64_t a = memory_address();
  uint64_t probe = 0;
  if (!load(m, a + 8, 8, &probe)) return false;      // (all 16 bytes or none)
  return store(m, a, 8, value.lo) && store(m, a + 8, 8, value.hi);
}

bool X86_64Cpu::push(GuestMemory& m, uint64_t value) {
  if (!store(m, s_.r[4] - 8, 8, value)) return false;
  s_.r[4] -= 8;
  return true;
}

bool X86_64Cpu::pop(GuestMemory& m, uint64_t* out) {
  if (!load(m, s_.r[4], 8, out)) return false;
  s_.r[4] += 8;
  return true;
}

// --- flags and arithmetic --------------------------------------------------------------------------------------------------------------------

void X86_64Cpu::set_result_flags(uint64_t result, uint32_t size) {
  uint64_t f = s_.rflags & ~(kZero | kSign | kParity);
  const uint64_t r = result & mask_of(size);
  if (r == 0) f |= kZero;
  if (r & sign_of(size)) f |= kSign;
  if (even_parity(static_cast<uint8_t>(r))) f |= kParity;
  s_.rflags = f;
}

void X86_64Cpu::set_logic_flags(uint64_t result, uint32_t size) {
  s_.rflags &= ~(kCarry | kOverflow | kAdjust);
  set_result_flags(result, size);
}

uint64_t X86_64Cpu::alu(uint32_t op, uint64_t a, uint64_t b, uint32_t size) {
  const uint64_t mask = mask_of(size), sign = sign_of(size);
  a &= mask;
  b &= mask;
  const uint64_t carry_in = (op == 2 || op == 3) ? (s_.rflags & kCarry) : 0;
  uint64_t result = 0;
  bool carry = false, overflow = false;
  switch (op) {
    case 0: case 2: {      // add, adc
      result = (a + b + carry_in) & mask;
      carry = size == 8 ? (result < a || (carry_in && result == a)) : ((a + b + carry_in) > mask);
      overflow = ((a ^ result) & (b ^ result) & sign) != 0;
      break;
    }
    case 3: case 5: case 7: {      // sbb, sub, cmp
      result = (a - b - carry_in) & mask;
      carry = a < b || (carry_in && a == b);
      overflow = ((a ^ b) & (a ^ result) & sign) != 0;
      break;
    }
    case 1: result = a | b; break;
    case 4: result = a & b; break;
    default: result = a ^ b; break;
  }
  if (op == 1 || op == 4 || op == 6) {
    set_logic_flags(result, size);
  } else {
    uint64_t f = s_.rflags & ~(kCarry | kOverflow | kAdjust);
    if (carry) f |= kCarry;
    if (overflow) f |= kOverflow;
    if ((a ^ b ^ result) & 0x10) f |= kAdjust;
    s_.rflags = f;
    set_result_flags(result, size);
  }
  return result;
}

// Rotates and shifts (the group-2 opcodes, /0 ROL /1 ROR /4 SHL /5 SHR /7 SAR; /2 and /3 are handled by the caller as unsupported).
uint64_t X86_64Cpu::shift(uint32_t op, uint64_t value, uint32_t count, uint32_t size) {
  const uint32_t bits = size * 8;
  count &= size == 8 ? 63 : 31;
  value &= mask_of(size);
  if (count == 0) return value;
  uint64_t result = value;
  bool carry = false;
  uint64_t f = s_.rflags;
  switch (op) {
    case 0: case 1: {      // rol, ror
      const uint32_t n = count % bits;
      if (op == 0) {
        result = n ? ((value << n) | (value >> (bits - n))) & mask_of(size) : value;
        carry = (result & 1) != 0;
        f = (f & ~(kCarry | kOverflow)) | (carry ? kCarry : 0) | ((((result >> (bits - 1)) & 1) ^ (carry ? 1 : 0)) ? kOverflow : 0);
      } else {
        result = n ? ((value >> n) | (value << (bits - n))) & mask_of(size) : value;
        carry = (result & sign_of(size)) != 0;
        f = (f & ~(kCarry | kOverflow)) | (carry ? kCarry : 0) | (((((result >> (bits - 1)) ^ (result >> (bits - 2))) & 1)) ? kOverflow : 0);
      }
      s_.rflags = f;
      return result;
    }
    case 2: case 3: {      // rcl, rcr: a rotation of the value and the carry flag together (bits + 1 wide)
      const uint32_t n = count % (bits + 1);
      const unsigned __int128 ring_mask = (static_cast<unsigned __int128>(1) << (bits + 1)) - 1;
      const unsigned __int128 ext = value | (static_cast<unsigned __int128>((f & kCarry) ? 1 : 0) << bits);
      unsigned __int128 rotated = ext;
      if (n) {
        rotated = op == 2 ? ((ext << n) | (ext >> (bits + 1 - n))) & ring_mask : ((ext >> n) | (ext << (bits + 1 - n))) & ring_mask;
      }
      result = static_cast<uint64_t>(rotated) & mask_of(size);
      carry = ((rotated >> bits) & 1) != 0;
      const bool msb = ((result >> (bits - 1)) & 1) != 0;
      const bool overflow = op == 2 ? (msb != carry) : (msb != (((result >> (bits - 2)) & 1) != 0));      // (defined for a count of one)
      s_.rflags = (f & ~(kCarry | kOverflow)) | (carry ? kCarry : 0) | (overflow ? kOverflow : 0);
      return result;
    }
    case 4: case 6:      // shl
      result = count >= bits ? 0 : (value << count) & mask_of(size);
      carry = count <= bits && ((value >> (bits - count)) & 1);
      f = (f & ~(kCarry | kOverflow)) | (carry ? kCarry : 0) | ((((result & sign_of(size)) != 0) != carry) ? kOverflow : 0);
      break;
    case 5:      // shr
      result = count >= bits ? 0 : value >> count;
      carry = count <= bits && ((value >> (count - 1)) & 1);
      f = (f & ~(kCarry | kOverflow)) | (carry ? kCarry : 0) | ((value & sign_of(size)) ? kOverflow : 0);
      break;
    default: {      // sar
      const int64_t sv = sign_extend(value, size);
      const uint32_t n = count >= bits ? bits - 1 : count;
      result = static_cast<uint64_t>(sv >> n) & mask_of(size);
      carry = ((static_cast<uint64_t>(sv) >> (count > 63 ? 63 : count - 1)) & 1) != 0;
      f = f & ~(kCarry | kOverflow);
      if (carry) f |= kCarry;
      break;
    }
  }
  s_.rflags = f & ~kAdjust;
  set_result_flags(result, size);
  return result;
}

bool X86_64Cpu::condition(uint32_t code) const {
  const uint64_t f = s_.rflags;
  const bool cf = f & kCarry, zf = f & kZero, sf = f & kSign, of = f & kOverflow, pf = f & kParity;
  bool r = false;
  switch (code >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
  }
  return (code & 1) ? !r : r;
}

// DIV / IDIV of the double-width dividend hi:lo (the registers' halves) by `divisor`, all of operand size `size`.
bool X86_64Cpu::divide(bool is_signed, uint32_t size, uint64_t hi, uint64_t lo, uint64_t divisor, uint64_t* quotient, uint64_t* remainder) {
  const uint32_t bits = size * 8;
  uint64_t dh = 0, dl = 0;
  if (size < 8) {
    dl = ((hi & mask_of(size)) << bits) | (lo & mask_of(size));
    if (is_signed) {
      dh = static_cast<uint64_t>(sign_extend(dl, size * 2) >> 63);
      dl = static_cast<uint64_t>(sign_extend(dl, size * 2));
    }
  } else {
    dh = hi;
    dl = lo;
  }
  divisor &= mask_of(size);
  if (!is_signed) {
    uint64_t q = 0, r = 0;
    if (!divide_unsigned(dh, dl, divisor, &q, &r) || q > mask_of(size)) return false;
    *quotient = q;
    *remainder = r;
    return true;
  }
  const int64_t sd = sign_extend(divisor, size);
  if (sd == 0) return false;
  const bool dividend_negative = (dh >> 63) != 0, divisor_negative = sd < 0;
  if (dividend_negative) {
    dl = ~dl + 1;
    dh = ~dh + (dl == 0 ? 1 : 0);
  }
  const uint64_t dm = divisor_negative ? static_cast<uint64_t>(-sd) : static_cast<uint64_t>(sd);
  uint64_t q = 0, r = 0;
  if (!divide_unsigned(dh, dl, dm, &q, &r)) return false;
  const uint64_t limit = sign_of(size);
  if (dividend_negative != divisor_negative ? q > limit : q >= limit) return false;
  *quotient = (dividend_negative != divisor_negative ? ~q + 1 : q) & mask_of(size);
  *remainder = (dividend_negative ? ~r + 1 : r) & mask_of(size);
  return true;
}

// --- string instructions ---------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::string_instruction(GuestMemory& m, uint8_t opcode) {
  const uint32_t size = (opcode & 1) == 0 ? 1 : static_cast<uint32_t>(operand_size());
  const bool backwards = (s_.rflags & kDirection) != 0;
  const uint64_t delta = backwards ? ~uint64_t{size} + 1 : size;
  const bool compares = opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF;
  const bool repeated = d_.rep || d_.repne;
  const uint64_t segment_base = d_.segment == 1 ? s_.fs_base : d_.segment == 2 ? s_.gs_base : 0;
  // One call does a bounded number of iterations, so a huge REP still lets the scheduler and Ctrl+C in.
  uint32_t budget = 4096;
  bool finished = true;
  while (true) {
    if (repeated && s_.r[1] == 0) break;
    uint64_t a = 0, b = 0;
    switch (opcode & 0xFE) {
      case 0xA4:      // movs
        if (!load(m, s_.r[6] + segment_base, size, &a) || !store(m, s_.r[7], size, a)) return Step::Fault;
        s_.r[6] += delta;
        s_.r[7] += delta;
        break;
      case 0xAA:      // stos
        if (!store(m, s_.r[7], size, s_.r[0])) return Step::Fault;
        s_.r[7] += delta;
        break;
      case 0xAC:      // lods
        if (!load(m, s_.r[6] + segment_base, size, &a)) return Step::Fault;
        set_reg(0, size, a);
        s_.r[6] += delta;
        break;
      case 0xA6:      // cmps
        if (!load(m, s_.r[6] + segment_base, size, &a) || !load(m, s_.r[7], size, &b)) return Step::Fault;
        alu(7, a, b, size);
        s_.r[6] += delta;
        s_.r[7] += delta;
        break;
      default:      // 0xAE scas
        if (!load(m, s_.r[7], size, &b)) return Step::Fault;
        alu(7, s_.r[0], b, size);
        s_.r[7] += delta;
        break;
    }
    if (!repeated) break;
    --s_.r[1];
    if (compares) {
      const bool zero = (s_.rflags & kZero) != 0;
      if (d_.rep ? !zero : zero) break;      // repe stops at the first difference, repne at the first match
    }
    if (--budget == 0) {
      finished = s_.r[1] == 0;
      break;
    }
  }
  if (finished) s_.rip += d_.position;      // (otherwise the instruction runs again for the rest)
  return Step::Ok;
}

// --- the one-byte opcodes --------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::step(GuestMemory& m) {
  instruction_start_ = s_.rip;
  d_ = Decoded();
  vex_ = VexState();
  d_.available = static_cast<uint32_t>(m.fetchable(s_.rip, 15));
  if (d_.available == 0) return fail(14, s_.rip);
  {
    uint8_t* const bytes = d_.bytes;
    if (!m.fetch(s_.rip, ByteView(bytes, d_.available))) return fail(14, s_.rip);
  }
  // prefixes
  uint8_t op = 0;
  for (uint32_t i = 0; i < 14; ++i) {
    op = next_u8();
    bool prefix = true;
    switch (op) {
      case 0x66: d_.operand16 = true; break;
      case 0x67: d_.address32 = true; break;
      case 0xF2: d_.repne = true; d_.rep = false; break;
      case 0xF3: d_.rep = true; d_.repne = false; break;
      case 0xF0: case 0x2E: case 0x36: case 0x3E: case 0x26: break;      // lock and the null segment overrides
      case 0x64: d_.segment = 1; break;
      case 0x65: d_.segment = 2; break;
      default: prefix = false; break;
    }
    if (!prefix) break;
  }
  if (op == 0xD5) {      // REX2 (Intel APX): M0 R4 X4 B4 W R3 X3 B3, then an opcode of the one-byte map or (M0) the 0F map
    const uint8_t payload = next_u8();
    if (d_.truncated) return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available);
    d_.rex2 = true;
    d_.rex_present = true;
    d_.rex_r4 = payload & 0x40;
    d_.rex_x4 = payload & 0x20;
    d_.rex_b4 = payload & 0x10;
    d_.rex_w = payload & 8;
    d_.rex_r = payload & 4;
    d_.rex_x = payload & 2;
    d_.rex_b = payload & 1;
    if (payload & 0x80) return step_0f(m);
    op = next_u8();
    if (d_.truncated) return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available);
    if (op == 0xA1) {      // jmpabs imm64 (REX2 with no payload bits)
      if (payload != 0) return fail(6);
      const uint64_t target = next_imm(8, false);
      if (d_.truncated) return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available);
      s_.rip = target;
      return Step::Ok;
    }
    if ((op & 0xF0) == 0x40 || op == 0xC4 || op == 0xC5 || op == 0x62 || op == 0xD5 || op == 0x0F || op == 0x66 || op == 0x67 || op == 0xF2 || op == 0xF3 || op == 0xF0 || op == 0x64 || op == 0x65 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x26) return fail(6);
  } else if ((op & 0xF0) == 0x40) {
    d_.rex_present = true;
    d_.rex_w = op & 8;
    d_.rex_r = op & 4;
    d_.rex_x = op & 2;
    d_.rex_b = op & 1;
    op = next_u8();
  }
  if (d_.truncated) return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available);
  return step_op(m, op);
}

// The instruction whose (one-byte map) opcode is `op`, with its prefixes decoded into d_.
X86_64Cpu::Step X86_64Cpu::step_op(GuestMemory& m, uint8_t op) {
  Step result = Step::Ok;
  const uint32_t osize = static_cast<uint32_t>(operand_size());
  auto advance = [&]() { s_.rip += d_.position; };
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };

  if (op == 0xC4 || op == 0xC5) return step_vex(m, op);
  if (op == 0x8F && d_.position < d_.available && (d_.bytes[d_.position] & 8)) return step_vex(m, op);      // (the XOP prefix: a pop has 0 in the modrm reg field)
  if (op == 0x62) return step_evex(m);      // (in 64-bit mode this is always the EVEX prefix)      // (in 64-bit mode these are always the VEX prefix)
  if (op == 0x0F) {
    result = step_0f(m);
    return result;
  }

  if (op < 0x40 && (op & 7) < 6) {      // the eight arithmetic operations in their six forms
    const uint32_t kind = (op >> 3) & 7, form = op & 7;
    const uint32_t size = (form & 1) ? osize : 1;
    uint64_t a = 0, b = 0;
    if (form >= 4) {      // AL/eAX, imm
      b = next_imm(size == 8 ? 4 : size, true);
      if (!finish_decode()) return bad_decode();
      a = get_reg(0, size);
      const uint64_t r = alu(kind, a, b, size);
      if (kind != 7) set_reg(0, size, r);
      advance();
      return Step::Ok;
    }
    if (!decode_modrm() || !finish_decode()) return bad_decode();
    if (form < 2) {      // r/m, reg
      if (!read_rm(m, size, &a)) return Step::Fault;
      b = get_reg(d_.reg, size);
      const uint64_t r = alu(kind, a, b, size);
      if (kind != 7 && !write_rm(m, size, r)) return Step::Fault;
    } else {             // reg, r/m
      if (!read_rm(m, size, &b)) return Step::Fault;
      a = get_reg(d_.reg, size);
      const uint64_t r = alu(kind, a, b, size);
      if (kind != 7) set_reg(d_.reg, size, r);
    }
    advance();
    return Step::Ok;
  }

  switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {      // push r64
      if (!finish_decode()) return bad_decode();
      const uint64_t v = s_.r[(op & 7) | d_.ext_b()];
      if (!push(m, v)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {      // pop r64
      if (!finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!pop(m, &v)) return Step::Fault;
      s_.r[(op & 7) | d_.ext_b()] = v;
      advance();
      return Step::Ok;
    }
    case 0x63: {      // movsxd
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!read_rm(m, 4, &v)) return Step::Fault;
      set_reg(d_.reg, osize, osize == 8 ? static_cast<uint64_t>(sign_extend(v, 4)) : v);
      advance();
      return Step::Ok;
    }
    case 0x68: case 0x6A: {      // push imm
      const uint64_t v = op == 0x68 ? next_imm(4, true) : next_imm(1, true);
      if (!finish_decode()) return bad_decode();
      if (!push(m, v)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0x69: case 0x6B: {      // imul reg, r/m, imm
      if (!decode_modrm()) return bad_decode();
      const uint64_t imm = op == 0x69 ? next_imm(osize == 2 ? 2 : 4, true) : next_imm(1, true);
      if (!finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!read_rm(m, osize, &v)) return Step::Fault;
      uint64_t hi = 0, lo = 0;
      multiply_signed(sign_extend(v, osize), sign_extend(imm, osize), &hi, &lo);
      const int64_t full = static_cast<int64_t>(lo);
      const bool overflow = osize == 8 ? (static_cast<int64_t>(hi) != (full >> 63)) : (sign_extend(lo, osize) != full);
      set_reg(d_.reg, osize, lo);
      s_.rflags = (s_.rflags & ~(kCarry | kOverflow)) | (overflow ? (kCarry | kOverflow) : 0);
      set_result_flags(lo, osize);
      advance();
      return Step::Ok;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {      // jcc rel8
      const uint64_t rel = next_imm(1, true);
      if (!finish_decode()) return bad_decode();
      advance();
      if (condition(op & 15)) s_.rip += rel;
      return Step::Ok;
    }
    case 0x80: case 0x81: case 0x83: {      // group 1: r/m, imm
      const uint32_t size = op == 0x80 ? 1 : osize;
      if (!decode_modrm()) return bad_decode();
      const uint64_t imm = op == 0x81 ? next_imm(size == 2 ? 2 : 4, true) : next_imm(1, true);
      if (!finish_decode()) return bad_decode();
      uint64_t a = 0;
      if (!read_rm(m, size, &a)) return Step::Fault;
      const uint32_t kind = d_.reg & 7;
      const uint64_t r = alu(kind, a, imm, size);
      if (kind != 7 && !write_rm(m, size, r)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0x84: case 0x85: {      // test
      const uint32_t size = op == 0x84 ? 1 : osize;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t a = 0;
      if (!read_rm(m, size, &a)) return Step::Fault;
      set_logic_flags(a & get_reg(d_.reg, size), size);
      advance();
      return Step::Ok;
    }
    case 0x86: case 0x87: {      // xchg r/m, reg
      const uint32_t size = op == 0x86 ? 1 : osize;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t a = 0;
      if (!read_rm(m, size, &a)) return Step::Fault;
      const uint64_t b = get_reg(d_.reg, size);
      if (!write_rm(m, size, b)) return Step::Fault;
      set_reg(d_.reg, size, a);
      advance();
      return Step::Ok;
    }
    case 0x88: case 0x89: {      // mov r/m, reg
      const uint32_t size = op == 0x88 ? 1 : osize;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!write_rm(m, size, get_reg(d_.reg, size))) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0x8A: case 0x8B: {      // mov reg, r/m
      const uint32_t size = op == 0x8A ? 1 : osize;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!read_rm(m, size, &v)) return Step::Fault;
      set_reg(d_.reg, size, v);
      advance();
      return Step::Ok;
    }
    case 0x8C: case 0x8E: return segment_move(m, op);      // mov r/m16, Sreg and mov Sreg, r/m16
    case 0x8D: {      // lea
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (d_.mod == 3) return fail(6);
      set_reg(d_.reg, osize, d_.ea);
      advance();
      return Step::Ok;
    }
    case 0x8F: {      // pop r/m
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t v = 0;
      const uint64_t before = s_.r[4];
      if (!pop(m, &v)) return Step::Fault;
      if (!write_rm(m, 8, v)) {
        s_.r[4] = before;
        return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0x90:      // nop, pause (F3 90)
      if (!finish_decode()) return bad_decode();
      if (d_.rex_b) {      // xchg r8, rax
        const uint64_t t = s_.r[0];
        s_.r[0] = s_.r[8];
        s_.r[8] = t;
      }
      advance();
      return Step::Ok;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {      // xchg rax, r
      if (!finish_decode()) return bad_decode();
      const uint32_t r = (op & 7) | d_.ext_b();
      const uint64_t a = get_reg(0, osize), b = get_reg(r, osize);
      set_reg(0, osize, b);
      set_reg(r, osize, a);
      advance();
      return Step::Ok;
    }
    case 0x98:      // cbw / cwde / cdqe
      if (!finish_decode()) return bad_decode();
      if (osize == 8) s_.r[0] = static_cast<uint64_t>(sign_extend(s_.r[0], 4));
      else if (osize == 4) s_.r[0] = static_cast<uint64_t>(sign_extend(s_.r[0], 2)) & 0xFFFFFFFFu;
      else set_reg(0, 2, static_cast<uint64_t>(sign_extend(s_.r[0], 1)));
      advance();
      return Step::Ok;
    case 0x99: {      // cwd / cdq / cqo
      if (!finish_decode()) return bad_decode();
      const bool negative = (get_reg(0, osize) & sign_of(osize)) != 0;
      set_reg(2, osize, negative ? mask_of(osize) : 0);
      advance();
      return Step::Ok;
    }
    case 0x9C: {      // pushfq
      if (!finish_decode()) return bad_decode();
      if (!push(m, s_.rflags & 0x3F7FD7)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xC8: {      // enter imm16, imm8
      const uint64_t allocation = next_imm(2, false);
      const uint32_t level = static_cast<uint32_t>(next_imm(1, false)) & 31;
      if (!finish_decode()) return bad_decode();
      uint64_t sp = s_.r[4] - 8;
      if (!store(m, sp, 8, s_.r[5])) return Step::Fault;
      const uint64_t frame = sp;
      uint64_t bp = s_.r[5];
      for (uint32_t i = 1; i < level; ++i) {      // copy the enclosing frames' pointers
        bp -= 8;
        uint64_t v = 0;
        if (!load(m, bp, 8, &v)) return Step::Fault;
        sp -= 8;
        if (!store(m, sp, 8, v)) return Step::Fault;
      }
      if (level > 0) {
        sp -= 8;
        if (!store(m, sp, 8, frame)) return Step::Fault;
      }
      s_.r[5] = frame;
      s_.r[4] = sp - allocation;
      advance();
      return Step::Ok;
    }
    case 0xD7: {      // xlat: AL = [RBX + AL]
      if (!finish_decode()) return bad_decode();
      const uint64_t base = d_.segment == 1 ? s_.fs_base : d_.segment == 2 ? s_.gs_base : 0;
      uint64_t v = 0;
      if (!load(m, base + (d_.address32 ? (s_.r[3] & 0xFFFFFFFFu) : s_.r[3]) + (s_.r[0] & 0xFF), 1, &v)) return Step::Fault;
      set_reg(0, 1, v);
      advance();
      return Step::Ok;
    }
    case 0xE0: case 0xE1: case 0xE2: {      // loopne, loope, loop: decrement RCX (ECX with an address-size prefix) and branch while it is not zero (and ZF agrees)
      const int64_t displacement = static_cast<int64_t>(next_imm(1, true));
      if (!finish_decode()) return bad_decode();
      uint64_t counter = d_.address32 ? (s_.r[1] & 0xFFFFFFFFu) : s_.r[1];
      counter = d_.address32 ? ((counter - 1) & 0xFFFFFFFFu) : counter - 1;
      s_.r[1] = counter;      // (the 32-bit form clears the upper half)
      const bool zero = (s_.rflags & kZero) != 0;
      const bool taken = counter != 0 && (op == 0xE2 || (op == 0xE1 ? zero : !zero));
      s_.rip += d_.position + (taken ? static_cast<uint64_t>(displacement) : 0);
      return Step::Ok;
    }
    case 0x9E: case 0x9F: {      // sahf, lahf
      if (!finish_decode()) return bad_decode();
      constexpr uint64_t kLow = kCarry | kParity | kAdjust | kZero | kSign;
      if (op == 0x9E) s_.rflags = (s_.rflags & ~kLow) | ((s_.r[0] >> 8) & kLow) | 2;
      else s_.r[0] = (s_.r[0] & ~uint64_t{0xFF00}) | (((s_.rflags & kLow) | 2) << 8);
      advance();
      return Step::Ok;
    }
    case 0x9D: {      // popfq: only the flags a user program may change
      if (!finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!pop(m, &v)) return Step::Fault;
      constexpr uint64_t kUser = kCarry | kParity | kAdjust | kZero | kSign | kDirection | kOverflow;
      s_.rflags = (s_.rflags & ~kUser) | (v & kUser) | 2;
      advance();
      return Step::Ok;
    }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {      // mov al/eax, moffs
      const uint64_t address = next_imm(d_.address32 ? 4 : 8, false);
      if (!finish_decode()) return bad_decode();
      const uint32_t size = (op & 1) ? osize : 1;
      const uint64_t base = d_.segment == 1 ? s_.fs_base : d_.segment == 2 ? s_.gs_base : 0;
      uint64_t v = 0;
      if (op < 0xA2) {
        if (!load(m, address + base, size, &v)) return Step::Fault;
        set_reg(0, size, v);
      } else if (!store(m, address + base, size, get_reg(0, size))) {
        return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
      if (!finish_decode()) return bad_decode();
      return string_instruction(m, op);
    case 0xA8: case 0xA9: {      // test al/eax, imm
      const uint32_t size = op == 0xA8 ? 1 : osize;
      const uint64_t imm = next_imm(size == 8 ? 4 : size, true);
      if (!finish_decode()) return bad_decode();
      set_logic_flags(get_reg(0, size) & imm, size);
      advance();
      return Step::Ok;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: {      // mov r8, imm8
      const uint64_t v = next_imm(1, false);
      if (!finish_decode()) return bad_decode();
      set_reg((op & 7) | d_.ext_b(), 1, v);
      advance();
      return Step::Ok;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {      // mov r, imm
      const uint64_t v = next_imm(osize, false);
      if (!finish_decode()) return bad_decode();
      set_reg((op & 7) | d_.ext_b(), osize, v);
      advance();
      return Step::Ok;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {      // group 2: shifts and rotates
      const uint32_t size = (op & 1) ? osize : 1;
      if (!decode_modrm()) return bad_decode();
      uint32_t count = 1;
      if (op < 0xD0) count = static_cast<uint32_t>(next_imm(1, false));
      else if (op >= 0xD2) count = static_cast<uint32_t>(s_.r[1] & 0xFF);
      if (!finish_decode()) return bad_decode();
      const uint32_t kind = d_.reg & 7;
      uint64_t v = 0;
      if (!read_rm(m, size, &v)) return Step::Fault;
      const uint64_t r = shift(kind, v, count, size);
      if (!write_rm(m, size, r)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xC2: case 0xC3: {      // ret
      const uint64_t extra = op == 0xC2 ? next_imm(2, false) : 0;
      if (!finish_decode()) return bad_decode();
      uint64_t target = 0;
      if (!pop(m, &target)) return Step::Fault;
      if (cet_.enabled && !cet_check_return(m, target)) {
        s_.r[4] -= 8;
        return Step::Fault;
      }
      s_.r[4] += extra;
      s_.rip = target;
      return Step::Ok;
    }
    case 0xC6: case 0xC7: {      // mov r/m, imm; xabort (C6 F8 ib) and xbegin (C7 F8 rel32)
      const uint32_t size = op == 0xC6 ? 1 : osize;
      if (!decode_modrm()) return bad_decode();
      if (d_.mod == 3 && (d_.reg & 7) == 7 && (d_.rm & 7) == 0) {
        // Transactions always abort: a processor may abort any transaction at any time, and code has to have its fallback path.
        if (op == 0xC6) {
          next_imm(1, false);      // xabort outside a transaction does nothing
          if (!finish_decode()) return bad_decode();
          advance();
          return Step::Ok;
        }
        const uint64_t displacement = next_imm(4, true);
        if (!finish_decode()) return bad_decode();
        s_.r[0] = 0;      // EAX: no abort reason, and not retryable
        s_.rip += d_.position + displacement;
        return Step::Ok;
      }
      const uint64_t imm = next_imm(size == 8 ? 4 : size, true);
      if (!finish_decode()) return bad_decode();
      if ((d_.reg & 7) != 0) return fail(6);
      if (!write_rm(m, size, imm)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xC9: {      // leave
      if (!finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!load(m, s_.r[5], 8, &v)) return Step::Fault;
      s_.r[4] = s_.r[5] + 8;
      s_.r[5] = v;
      advance();
      return Step::Ok;
    }
    case 0xCC:
      return fail(3);
    case 0xCA: case 0xCB: return far_return(m, op);
    case 0xCF: return interrupt_return(m);
    case 0xCD: {      // int n: there is no interrupt table to deliver it through; int3 is a breakpoint, anything else a general-protection fault
      const uint64_t n = next_imm(1, false);
      if (!finish_decode()) return bad_decode();
      return fail(n == 3 ? 3 : 13);
    }
    case 0xF1: return fail(1);      // int1 (icebp): a debug exception
    case 0xE3: {      // jrcxz
      const uint64_t rel = next_imm(1, true);
      if (!finish_decode()) return bad_decode();
      advance();
      if ((d_.address32 ? (s_.r[1] & 0xFFFFFFFFu) : s_.r[1]) == 0) s_.rip += rel;
      return Step::Ok;
    }
    case 0xE8: case 0xE9: case 0xEB: {      // call / jmp relative
      const uint64_t rel = op == 0xEB ? next_imm(1, true) : next_imm(4, true);
      if (!finish_decode()) return bad_decode();
      advance();
      if (op == 0xE8 && !push(m, s_.rip)) {
        s_.rip = instruction_start_;
        return Step::Fault;
      }
      if (op == 0xE8 && cet_.enabled && !cet_push(m, s_.rip)) {
        s_.r[4] += 8;
        s_.rip = instruction_start_;
        return Step::Fault;
      }
      s_.rip += rel;
      return Step::Ok;
    }
    case 0x9B:      // fwait
      if (!finish_decode()) return bad_decode();
      advance();
      return Step::Ok;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
      if (d_.apx_high()) return fail(6);
      return step_x87(m, op);
    case 0xF4:      // hlt is privileged
      return fail(13);
    case 0xF5: case 0xF8: case 0xF9: case 0xFC: case 0xFD:      // cmc clc stc cld std
      if (!finish_decode()) return bad_decode();
      if (op == 0xF5) s_.rflags ^= kCarry;
      else if (op == 0xF8) s_.rflags &= ~kCarry;
      else if (op == 0xF9) s_.rflags |= kCarry;
      else if (op == 0xFC) s_.rflags &= ~kDirection;
      else s_.rflags |= kDirection;
      advance();
      return Step::Ok;
    case 0xF6: case 0xF7: case 0xFE: case 0xFF:
      return step_group_f(m, op);
    default:
      return fail(6);
  }
}

// F6/F7 (test, not, neg, mul, imul, div, idiv) and FE/FF (inc, dec, call, jmp, push).
X86_64Cpu::Step X86_64Cpu::step_group_f(GuestMemory& m, uint8_t op) {
  const uint32_t osize = static_cast<uint32_t>(operand_size());
  const uint32_t size = (op & 1) ? osize : 1;
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm()) return bad_decode();
  const uint32_t kind = d_.reg & 7;
  uint64_t imm = 0;
  if (op < 0xF8 && kind < 2) imm = next_imm(size == 8 ? 4 : size, true);
  if (!finish_decode()) return bad_decode();
  uint64_t v = 0;
  if (op == 0xFF && (kind == 2 || kind == 4)) {      // call / jmp r/m64
    if (!read_rm(m, 8, &v)) return Step::Fault;
    s_.rip += d_.position;
    if (kind == 2 && !push(m, s_.rip)) {
      s_.rip = instruction_start_;
      return Step::Fault;
    }
    if (kind == 2 && cet_.enabled && !cet_push(m, s_.rip)) {
      s_.r[4] += 8;
      s_.rip = instruction_start_;
      return Step::Fault;
    }
    s_.rip = v;
    return Step::Ok;
  }
  if (op == 0xFF && (kind == 3 || kind == 5)) return far_branch(m, kind == 3);
  if (op == 0xFF && kind == 6) {      // push r/m64
    if (!read_rm(m, 8, &v) || !push(m, v)) return Step::Fault;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (!read_rm(m, size, &v)) return Step::Fault;
  if (op >= 0xFE) {      // inc, dec: keep CF
    if (kind > 1) return fail(6);
    const uint64_t carry = s_.rflags & kCarry;
    const uint64_t r = alu(kind == 0 ? 0 : 5, v, 1, size);
    s_.rflags = (s_.rflags & ~kCarry) | carry;
    if (!write_rm(m, size, r)) return Step::Fault;
    s_.rip += d_.position;
    return Step::Ok;
  }
  switch (kind) {
    case 0: case 1:
      set_logic_flags(v & imm, size);
      break;
    case 2:
      if (!write_rm(m, size, ~v)) return Step::Fault;
      break;
    case 3: {
      const uint64_t r = alu(5, 0, v, size);
      if (!write_rm(m, size, r)) return Step::Fault;
      s_.rflags = (s_.rflags & ~kCarry) | ((v & mask_of(size)) != 0 ? kCarry : 0);
      break;
    }
    case 4: case 5: {      // mul, imul (one operand): AX / DX:AX / EDX:EAX / RDX:RAX
      uint64_t hi = 0, lo = 0;
      const uint64_t a = get_reg(0, size);
      bool overflow;
      if (kind == 4) {
        multiply_unsigned(a, v & mask_of(size), &hi, &lo);
        if (size < 8) {
          hi = lo >> (size * 8);
          lo &= mask_of(size);
        }
        overflow = hi != 0;
      } else {
        multiply_signed(sign_extend(a, size), sign_extend(v, size), &hi, &lo);
        const int64_t low = static_cast<int64_t>(lo);
        if (size < 8) {
          overflow = sign_extend(lo, size) != low;
          hi = (lo >> (size * 8)) & mask_of(size);
          lo &= mask_of(size);
        } else {
          overflow = static_cast<int64_t>(hi) != (low >> 63);
        }
      }
      if (size == 1) {
        set_reg(0, 2, (hi << 8) | lo);
      } else {
        set_reg(0, size, lo);
        set_reg(2, size, hi);
      }
      s_.rflags = (s_.rflags & ~(kCarry | kOverflow)) | (overflow ? (kCarry | kOverflow) : 0);
      break;
    }
    default: {      // div, idiv
      uint64_t q = 0, r = 0;
      const uint64_t hi = size == 1 ? (s_.r[0] >> 8) & 0xFF : get_reg(2, size);
      const uint64_t lo = get_reg(0, size);
      if (!divide(kind == 7, size, hi, lo, v, &q, &r)) return fail(0);
      if (size == 1) {
        set_reg(0, 2, (r << 8) | q);
      } else {
        set_reg(0, size, q);
        set_reg(2, size, r);
      }
      break;
    }
  }
  s_.rip += d_.position;
  return Step::Ok;
}

// --- the 0F opcode map -----------------------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::step_0f(GuestMemory& m) { return step_0f_op(m, next_u8()); }

X86_64Cpu::Step X86_64Cpu::step_0f_op(GuestMemory& m, uint8_t op) {
  const uint32_t osize = static_cast<uint32_t>(operand_size());
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  auto advance = [&]() { s_.rip += d_.position; };
  if (d_.truncated) return bad_decode();
  if (d_.apx_high()) {      // only the instructions that work on general registers may name R16..R31
    const bool general = (op >= 0x40 && op <= 0x4F) || (op >= 0x90 && op <= 0x9F) || (op >= 0xC8 && op <= 0xCF) || op == 0x38 || op == 0x1F || (op >= 0x18 && op <= 0x1E) || op == 0xA3 ||
                         op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA || op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD || op == 0xAF || op == 0xB0 || op == 0xB1 ||
                         op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF || op == 0xC0 || op == 0xC1 || op == 0xBC || op == 0xBD || op == 0xB8 || op == 0xC3 || op == 0xC7 || op == 0xAE;
    if (!general) return fail(6);
  }

  switch (op) {
    case 0x01: return system_0f01(m);
    case 0xC7: return group9(m);
    case 0xC3: return movnti(m);
    case 0x38: {      // 0F 38 F0 / F1: movbe
      const uint8_t second = next_u8();
      if (d_.apx_high() && second != 0xF0 && second != 0xF1 && second != 0xF6 && second != 0xF8 && second != 0xF9 && second != 0xFC) return fail(6);
      if (d_.truncated) return bad_decode();
      if ((second == 0xF0 || second == 0xF1) && d_.repne) return crc32_instruction(m, second);
      if (second == 0xF6 && (d_.operand16 || d_.rep) && !d_.repne) return adx_instruction(m, d_.rep);
      if (second == 0xF6 && !d_.operand16 && !d_.rep && !d_.repne) return wrss_instruction(m, false);
      if (second == 0xF5 && d_.operand16 && !d_.rep && !d_.repne) return wrss_instruction(m, true);
      if (second == 0xF8 && (d_.rep != d_.repne)) return enqcmd_instruction(m, d_.rep);
      if (second == 0xFC && !(d_.rep && d_.repne)) return rao_instruction(m, d_.repne ? 2 : d_.rep ? 3 : d_.operand16 ? 1 : 0);
      if ((second == 0xF9 && !d_.operand16 && !d_.rep && !d_.repne) || (second == 0xF8 && d_.operand16 && !d_.rep && !d_.repne)) return direct_store(m, second);
      if (!d_.operand16 && !d_.rep && !d_.repne && second >= 0xC8 && second <= 0xCD) return crypto_map(m, 2, second, false, 0);      // the SHA extensions
      if (mmx_ssse3_opcode(2, second)) return mmx_ssse3(m, 2, second);
      if (second != 0xF0 && second != 0xF1) return legacy_sse_map(m, 2, second);
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (d_.mod == 3) return fail(6);
      auto swap = [&](uint64_t v) { uint64_t out = 0; for (uint32_t i = 0; i < osize; ++i) out = (out << 8) | ((v >> (8 * i)) & 0xFF); return out; };
      if (second == 0xF0) {
        uint64_t v;
        if (!load(m, memory_address(), osize, &v)) return Step::Fault;
        set_reg(d_.reg, osize, swap(v));
      } else if (!store(m, memory_address(), osize, swap(get_reg(d_.reg, osize)))) {
        return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0x3A: {      // 0F 3A: SSSE3 / SSE4 instructions with an immediate
      const uint8_t second = next_u8();
      if (d_.apx_high()) return fail(6);
      if (d_.truncated) return bad_decode();
      if (!d_.operand16 && !d_.rep && !d_.repne && second == 0xCC) return crypto_map(m, 3, second, false, 0);      // sha1rnds4
      if (mmx_ssse3_opcode(3, second)) return mmx_ssse3(m, 3, second);
      return legacy_sse_map(m, 3, second);
    }
    case 0xA0: case 0xA8: {      // push fs / push gs: the selector
      if (!finish_decode()) return bad_decode();
      if (!push(m, selector_[op == 0xA0 ? 4 : 5])) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xA1: case 0xA9: {      // pop fs / pop gs: the selector is kept (the base is not reloaded from a descriptor table)
      if (!finish_decode()) return bad_decode();
      const uint64_t before = s_.r[4];
      uint64_t v = 0;
      if (!pop(m, &v)) return Step::Fault;
      if (!load_selector(op == 0xA1 ? 4 : 5, static_cast<uint16_t>(v))) {
        s_.r[4] = before;
        return fail(13);
      }
      advance();
      return Step::Ok;
    }
    case 0xB2: case 0xB4: case 0xB5: return far_pointer_load(m, op);      // lss, lfs, lgs
    case 0x05:      // syscall
      if (!finish_decode()) return bad_decode();
      s_.rip += d_.position;
      s_.r[1] = s_.rip;
      s_.r[11] = s_.rflags;
      return Step::Syscall;
    case 0x0B:      // ud2
      return fail(6);
    case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:      // prefetch, multi-byte nop
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (op == 0x1E && d_.rep && d_.mod == 3 && (d_.reg & 7) == 1) return rdssp_instruction();      // rdsspd / rdsspq
      advance();
      return Step::Ok;
    case 0x31:      // rdtsc: the instruction count stands in for a clock
      if (!finish_decode()) return bad_decode();
      s_.r[0] = (retired_ * 4) & 0xFFFFFFFFu;
      s_.r[2] = ((retired_ * 4) >> 32) & 0xFFFFFFFFu;
      advance();
      return Step::Ok;
    case 0xA2: {      // cpuid: a plain x86-64 baseline
      if (!finish_decode()) return bad_decode();
      const uint32_t leaf = static_cast<uint32_t>(s_.r[0]);
      uint32_t a = 0, b = 0, c = 0, dx = 0;
      if (leaf == 0) {
        a = 0x24;
        b = 0x534F4354u;      // "TCOS"
        dx = 0x34365F58u;     // "X_64"
        c = 0x554D455Fu;      // "_EMU"... in the ebx, edx, ecx order of the vendor string
      } else if (leaf == 1) {
        a = 0x00000F00u;
        dx = (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);      // TSC, CX8, CMOV, MMX, FXSR, SSE, SSE2
        c = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 19) | (1u << 20) | (1u << 22) | (1u << 23) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 1) | (1u << 25) | (1u << 13) | (1u << 30);      // SSE3, SSSE3, FMA, CMPXCHG16B, SSE4.1, SSE4.2, MOVBE, POPCNT, XSAVE, OSXSAVE, AVX, F16C, RDRAND, PCLMULQDQ, AES
      } else if (leaf == 7 && static_cast<uint32_t>(s_.r[1]) == 1) {
        a = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 21) | (1u << 23);      // SHA512, SM3, SM4, RAO-INT, AVX-VNNI, AVX512_BF16, CMPCCXADD, AMX-FP16, AVX-IFMA
        dx = (1u << 4) | (1u << 5) | (1u << 8) | (1u << 10) | (1u << 19) | (1u << 21);      // AVX-VNNI-INT8, AVX-NE-CONVERT, AVX-VNNI-INT16, APX_F, AVX10
      } else if (leaf == 7 && static_cast<uint32_t>(s_.r[1]) == 0) {
        b = (1u << 0) | (1u << 3) | (1u << 5) | (1u << 8) | (1u << 18) | (1u << 19) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27) | (1u << 29) | (1u << 16) | (1u << 17) | (1u << 21) | (1u << 28) | (1u << 30) | (1u << 31);      // BMI1, AVX2, BMI2, SHA, AVX512F, DQ, IFMA, CD, BW, VL
        c = (1u << 1) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 14) | (1u << 22) | (1u << 27) | (1u << 28) | (1u << 29);      // VBMI, VBMI2, GFNI, VAES, VPCLMULQDQ, VNNI, BITALG, VPOPCNTDQ, RDPID, MOVDIRI, MOVDIR64B, ENQCMD
        dx = (1u << 2) | (1u << 3) | (1u << 8) | (1u << 11) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25);      // AVX512_4VNNIW, AVX512_4FMAPS, AVX512_VP2INTERSECT, RTM_ALWAYS_ABORT, AMX-BF16, AVX512_FP16, AMX-TILE, AMX-INT8
      } else if (leaf == 0xD) {
        const uint32_t sub = static_cast<uint32_t>(s_.r[1]);
        if (sub == 0) { a = 0xE00E7; b = 11136; c = 11136; dx = 0x40000000; }      // x87, SSE, AVX and the AVX-512 state; 512 + 64 + 256 + ... bytes
        else if (sub == 1) { a = 7; b = 10944; }      // XSAVEOPT, XSAVEC, XGETBV with ECX = 1; the compacted size of all the components
        else if (sub == 2) { a = 256; b = 576; }      // the AVX component: 256 bytes at offset 576
        else if (sub == 5) { a = 64; b = 1088; }      // opmask registers
        else if (sub == 6) { a = 512; b = 1152; }      // the upper halves of ZMM0..15
        else if (sub == 7) { a = 1024; b = 1664; }      // ZMM16..31
        else if (sub == 17) { a = 64; b = 2752; c = 2; }      // TILECFG (64-byte aligned in the compacted format)
        else if (sub == 18) { a = 8192; b = 2816; c = 2; }      // TILEDATA
        else if (sub == 19) { a = 128; b = 11008; }      // APX: R16..R31
        else if (sub == 62) { a = 128; b = 832; }      // LWP
      } else if (leaf == 0x1D) {      // AMX tile palettes: one, with 8 tiles of 16 rows of 64 bytes
        const uint32_t sub = static_cast<uint32_t>(s_.r[1]);
        if (sub == 0) a = 1;
        else if (sub == 1) { a = 8192 | (1024u << 16); b = 64 | (8u << 16); c = 16; }
      } else if (leaf == 0x1E && static_cast<uint32_t>(s_.r[1]) == 0) {      // TMUL: maximum K 16 rows, maximum N 64 bytes
        b = 16 | (64u << 8);
      } else if (leaf == 0x24 && static_cast<uint32_t>(s_.r[1]) == 0) {      // AVX10 enumeration: converged version 2 (the AVX-512 set and the AVX10.2 instructions), 128 / 256 / 512-bit vectors
        a = 0;
        b = 2 | (7u << 16);
      } else if (leaf == 0x80000000u) {
        a = 0x8000001Cu;
      } else if (leaf == 0x8000001Cu) {      // LWP: the value sample (LWPVAL) and LWPINS, continuous mode and time stamps; no hardware events, no interrupt. 32-byte records, a 136-byte LWPCB (17 quadwords),
        a = 1u | (1u << 1) | (1u << 29) | (1u << 30);      // six event ids, the first event interval at offset 88, 32 records at least, version 1
        b = 17u | (32u << 8) | (6u << 16) | (88u << 24);
        c = (1u << 9) | (1u << 16);
        dx = 1u | (1u << 1) | (1u << 29) | (1u << 30);
      } else if (leaf == 0x80000001u) {
        c = (1u << 0) | (1u << 5) | (1u << 11) | (1u << 15) | (1u << 16) | (1u << 21);      // LAHF/SAHF, LZCNT, XOP, LWP, FMA4, TBM
        dx = (1u << 11) | (1u << 20) | (1u << 27) | (1u << 29);      // SYSCALL, NX, RDTSCP, long mode
      }
      s_.r[0] = a;
      s_.r[3] = b;
      s_.r[1] = c;
      s_.r[2] = dx;
      advance();
      return Step::Ok;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {      // cmovcc
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!read_rm(m, osize, &v)) return Step::Fault;
      if (condition(op & 15)) set_reg(d_.reg, osize, v);
      else if (osize == 4) set_reg(d_.reg, 4, get_reg(d_.reg, 4));      // (a 32-bit cmov clears the upper half either way)
      advance();
      return Step::Ok;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {      // jcc rel32
      const uint64_t rel = next_imm(4, true);
      if (!finish_decode()) return bad_decode();
      advance();
      if (condition(op & 15)) s_.rip += rel;
      return Step::Ok;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {      // setcc
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!write_rm(m, 1, condition(op & 15) ? 1 : 0)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: {      // bt, bts, btr, btc (register or immediate bit number)
      if (!decode_modrm()) return bad_decode();
      uint64_t bit;
      uint32_t kind;
      if (op == 0xBA) {
        bit = next_imm(1, false);
        kind = d_.reg & 7;
        if (kind < 4) return fail(6);
        kind -= 4;
      } else {
        bit = get_reg(d_.reg, osize);
        kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
      }
      if (!finish_decode()) return bad_decode();
      const uint32_t bits = osize * 8;
      uint64_t address_bump = 0;
      if (d_.mod != 3 && op != 0xBA) address_bump = static_cast<uint64_t>(sign_extend(bit, osize) >> (osize == 8 ? 6 : osize == 4 ? 5 : 4)) * osize;
      uint64_t v = 0;
      const uint64_t saved_ea = d_.ea;
      d_.ea += address_bump;
      const bool ok = read_rm(m, osize, &v);
      if (!ok) return Step::Fault;
      const uint32_t n = static_cast<uint32_t>(bit % bits);
      const bool set = (v >> n) & 1;
      s_.rflags = (s_.rflags & ~kCarry) | (set ? kCarry : 0);
      if (kind != 0) {
        v = kind == 1 ? v | (uint64_t{1} << n) : kind == 2 ? v & ~(uint64_t{1} << n) : v ^ (uint64_t{1} << n);
        if (!write_rm(m, osize, v)) return Step::Fault;
      }
      d_.ea = saved_ea;
      advance();
      return Step::Ok;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {      // shld, shrd
      if (!decode_modrm()) return bad_decode();
      uint32_t count = 0;
      if (op == 0xA4 || op == 0xAC) count = static_cast<uint32_t>(next_imm(1, false));
      if (!finish_decode()) return bad_decode();
      if (op == 0xA5 || op == 0xAD) count = static_cast<uint32_t>(s_.r[1] & 0xFF);
      const uint32_t bits = osize * 8;
      count &= osize == 8 ? 63 : 31;
      uint64_t dst = 0;
      if (!read_rm(m, osize, &dst)) return Step::Fault;
      if (count == 0 || count > bits) {
        advance();
        return Step::Ok;
      }
      const uint64_t src = get_reg(d_.reg, osize);
      uint64_t r;
      bool carry;
      if (op <= 0xA5) {
        r = ((dst << count) | (count < bits ? src >> (bits - count) : 0)) & mask_of(osize);
        carry = (dst >> (bits - count)) & 1;
      } else {
        r = ((dst >> count) | (count < bits ? src << (bits - count) : 0)) & mask_of(osize);
        carry = (dst >> (count - 1)) & 1;
      }
      s_.rflags = (s_.rflags & ~(kCarry | kOverflow | kAdjust)) | (carry ? kCarry : 0) | (((r ^ dst) & sign_of(osize)) ? kOverflow : 0);
      set_result_flags(r, osize);
      if (!write_rm(m, osize, r)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xAF: {      // imul reg, r/m
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!read_rm(m, osize, &v)) return Step::Fault;
      uint64_t hi = 0, lo = 0;
      multiply_signed(sign_extend(get_reg(d_.reg, osize), osize), sign_extend(v, osize), &hi, &lo);
      const int64_t low = static_cast<int64_t>(lo);
      const bool overflow = osize == 8 ? static_cast<int64_t>(hi) != (low >> 63) : sign_extend(lo, osize) != low;
      set_reg(d_.reg, osize, lo);
      s_.rflags = (s_.rflags & ~(kCarry | kOverflow)) | (overflow ? (kCarry | kOverflow) : 0);
      set_result_flags(lo, osize);
      advance();
      return Step::Ok;
    }
    case 0xB0: case 0xB1: {      // cmpxchg
      const uint32_t size = op == 0xB0 ? 1 : osize;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t dst = 0;
      if (!read_rm(m, size, &dst)) return Step::Fault;
      alu(7, get_reg(0, size), dst, size);
      if (s_.rflags & kZero) {
        if (!write_rm(m, size, get_reg(d_.reg, size))) return Step::Fault;
      } else {
        set_reg(0, size, dst);
      }
      advance();
      return Step::Ok;
    }
    case 0xC0: case 0xC1: {      // xadd
      const uint32_t size = op == 0xC0 ? 1 : osize;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t dst = 0;
      if (!read_rm(m, size, &dst)) return Step::Fault;
      const uint64_t sum = alu(0, dst, get_reg(d_.reg, size), size);
      set_reg(d_.reg, size, dst);
      if (!write_rm(m, size, sum)) return Step::Fault;
      advance();
      return Step::Ok;
    }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {      // movzx, movsx
      const uint32_t from = (op & 1) ? 2 : 1;
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      uint64_t v = 0;
      if (!read_rm(m, from, &v)) return Step::Fault;
      if (op >= 0xBE) v = static_cast<uint64_t>(sign_extend(v, from));
      set_reg(d_.reg, osize, v);
      advance();
      return Step::Ok;
    }
    case 0xB8: case 0xBC: case 0xBD: {      // popcnt (F3), bsf / tzcnt (F3), bsr / lzcnt (F3)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (op == 0xB8 && !d_.rep) return fail(6);
      uint64_t v = 0;
      if (!read_rm(m, osize, &v)) return Step::Fault;
      const uint32_t bits = osize * 8;
      v &= mask_of(osize);
      if (op == 0xB8) {
        set_reg(d_.reg, osize, popcount64(v));
        s_.rflags = (s_.rflags & ~(kCarry | kOverflow | kAdjust | kSign | kParity)) & ~kZero;
        if (v == 0) s_.rflags |= kZero;
        advance();
        return Step::Ok;
      }
      uint32_t index = 0;
      if (op == 0xBC) {
        while (index < bits && !((v >> index) & 1)) ++index;      // (== bits for zero)
      } else {
        index = bits;
        while (index > 0 && !((v >> (index - 1)) & 1)) --index;      // number of leading zeros: bits - highest set - 1 handled below
        index = v == 0 ? bits : index - 1;
      }
      if (d_.rep) {      // tzcnt / lzcnt
        const uint32_t result = op == 0xBC ? index : (v == 0 ? bits : bits - 1 - index);
        set_reg(d_.reg, osize, result);
        s_.rflags = (s_.rflags & ~(kCarry | kZero)) | (v == 0 ? kCarry : 0) | (result == 0 ? kZero : 0);
      } else {
        if (v == 0) {
          s_.rflags |= kZero;      // the destination is left alone
        } else {
          set_reg(d_.reg, osize, index);
          s_.rflags &= ~kZero;
        }
      }
      advance();
      return Step::Ok;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {      // bswap
      if (!finish_decode()) return bad_decode();
      const uint32_t r = (op & 7) | d_.ext_b();
      uint64_t v = get_reg(r, osize), out = 0;
      for (uint32_t i = 0; i < osize; ++i) out = (out << 8) | ((v >> (8 * i)) & 0xFF);
      set_reg(r, osize, out);
      advance();
      return Step::Ok;
    }
    case 0xAE: {      // fences, ldmxcsr / stmxcsr
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      const uint32_t kind = d_.reg & 7;
      if (d_.mod == 3) {
        if (kind < 4 && d_.rep) return fsgsbase_instruction(m);
        if (kind == 5 && d_.rep) return incssp_instruction(m);      // incsspd / incsspq
        if (kind < 5) return fail(6);
      } else if (kind == 0 || kind == 1) {      // fxsave, fxrstor (the 16-byte alignment is not enforced)
        uint8_t image[512];
        if (kind == 0) {
          save_fpu_image(image);
          if (!m.write(memory_address(), ConstByteView(image, 512))) return fail(14, memory_address(), 2);
        } else {
          if (!m.read(memory_address(), ByteView(image, 512))) return fail(14, memory_address(), 0);
          load_fpu_image(image);
        }
      } else if (kind == 2) {
        uint64_t v = 0;
        if (!load(m, memory_address(), 4, &v)) return Step::Fault;
        if (v & ~uint64_t{0xFFFF}) return fail(13);      // reserved bits
        s_.mxcsr = static_cast<uint32_t>(v);
      } else if (kind == 3) {
        if (!store(m, memory_address(), 4, s_.mxcsr)) return Step::Fault;
      } else if (kind == 4 || kind == 5) {      // xsave, xrstor
        const Step r = xsave_family(m, kind);
        if (r != Step::Ok) return r;
      } else if (kind == 6 && !d_.operand16) {      // xsaveopt (66 0F AE /6 is clwb, a no-op below)
        const Step r = xsave_family(m, 6);
        if (r != Step::Ok) return r;
      } else if (kind != 6 && kind != 7) {
        return fail(6);
      }
      advance();
      return Step::Ok;
    }
    default:
      if (d_.apx_high()) return fail(6);
      return step_sse(m, op);
  }
}

// --- SSE2 integer and move instructions ------------------------------------------------------------------------------------------------------

// The integer operations on two 128-bit values that the SSE2 integer opcodes (0F 60..7F, 0F D0..FF) share; the MMX forms run them on 64-bit values held in the low half.
bool X86_64Cpu::sse_integer_valid(uint8_t op) {
  switch (op) {
    case 0x54: case 0x55: case 0x56: case 0x57: case 0xDB: case 0xDF: case 0xEB: case 0xEF:
    case 0x60: case 0x61: case 0x62: case 0x6C: case 0x68: case 0x69: case 0x6A: case 0x6D:
    case 0x74: case 0x75: case 0x76: case 0xFC: case 0xFD: case 0xFE: case 0xD4: case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xDA: case 0xDE: case 0x64: case 0x65: case 0x66:
    case 0x63: case 0x67: case 0x6B: case 0xD5: case 0xE4: case 0xE5: case 0xF4: case 0xF5: case 0xF6:
    case 0xDC: case 0xDD: case 0xD8: case 0xD9: case 0xEC: case 0xED: case 0xE8: case 0xE9: case 0xEA: case 0xEE: case 0xE0: case 0xE3:
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3:
      return true;
    default:
      return false;
  }
}

bool X86_64Cpu::sse_integer_core(uint8_t op, Xmm& dst, const Xmm& src) {
  auto unpack = [&](Xmm* d, const Xmm& s2, uint32_t width, bool high) {
    const uint32_t lanes = 128 / width;
    Xmm out;
    for (uint32_t i = 0; i < lanes / 2; ++i) {
      const uint32_t from = (high ? lanes / 2 : 0) + i;
      lane_set(&out.lo, &out.hi, width, 2 * i, lane_get(d->lo, d->hi, width, from));
      lane_set(&out.lo, &out.hi, width, 2 * i + 1, lane_get(s2.lo, s2.hi, width, from));
    }
    *d = out;
  };
  auto lanewise = [&](uint32_t width, auto fn) {
    for (uint32_t i = 0; i < 128 / width; ++i) {
      const uint64_t a = lane_get(dst.lo, dst.hi, width, i), b = lane_get(src.lo, src.hi, width, i);
      lane_set(&dst.lo, &dst.hi, width, i, fn(a, b));
    }
  };
  switch (op) {
    case 0x54: case 0xDB: dst.lo &= src.lo; dst.hi &= src.hi; break;
    case 0x55: case 0xDF: dst.lo = ~dst.lo & src.lo; dst.hi = ~dst.hi & src.hi; break;
    case 0x56: case 0xEB: dst.lo |= src.lo; dst.hi |= src.hi; break;
    case 0x57: case 0xEF: dst.lo ^= src.lo; dst.hi ^= src.hi; break;
    case 0x60: unpack(&dst, src, 8, false); break;
    case 0x61: unpack(&dst, src, 16, false); break;
    case 0x62: unpack(&dst, src, 32, false); break;
    case 0x6C: unpack(&dst, src, 64, false); break;
    case 0x68: unpack(&dst, src, 8, true); break;
    case 0x69: unpack(&dst, src, 16, true); break;
    case 0x6A: unpack(&dst, src, 32, true); break;
    case 0x6D: unpack(&dst, src, 64, true); break;
    case 0x74: lanewise(8, [](uint64_t a, uint64_t b) { return a == b ? uint64_t{0xFF} : 0; }); break;
    case 0x75: lanewise(16, [](uint64_t a, uint64_t b) { return a == b ? uint64_t{0xFFFF} : 0; }); break;
    case 0x76: lanewise(32, [](uint64_t a, uint64_t b) { return a == b ? uint64_t{0xFFFFFFFFu} : 0; }); break;
    case 0x64: lanewise(8, [](uint64_t a, uint64_t b) { return sign_extend(a, 1) > sign_extend(b, 1) ? uint64_t{0xFF} : 0; }); break;
    case 0x65: lanewise(16, [](uint64_t a, uint64_t b) { return sign_extend(a, 2) > sign_extend(b, 2) ? uint64_t{0xFFFF} : 0; }); break;
    case 0x66: lanewise(32, [](uint64_t a, uint64_t b) { return sign_extend(a, 4) > sign_extend(b, 4) ? uint64_t{0xFFFFFFFFu} : 0; }); break;
    case 0xFC: lanewise(8, [](uint64_t a, uint64_t b) { return a + b; }); break;
    case 0xFD: lanewise(16, [](uint64_t a, uint64_t b) { return a + b; }); break;
    case 0xFE: lanewise(32, [](uint64_t a, uint64_t b) { return a + b; }); break;
    case 0xD4: lanewise(64, [](uint64_t a, uint64_t b) { return a + b; }); break;
    case 0xF8: lanewise(8, [](uint64_t a, uint64_t b) { return a - b; }); break;
    case 0xF9: lanewise(16, [](uint64_t a, uint64_t b) { return a - b; }); break;
    case 0xFA: lanewise(32, [](uint64_t a, uint64_t b) { return a - b; }); break;
    case 0xFB: lanewise(64, [](uint64_t a, uint64_t b) { return a - b; }); break;
    case 0x63: case 0x67: case 0x6B: {      // packsswb, packuswb, packssdw: narrow both registers' lanes with saturation
      const uint32_t from = op == 0x6B ? 32 : 16, to = from / 2;
      const bool is_unsigned = op == 0x67;
      Xmm out;
      const uint32_t lanes = 128 / from;
      for (uint32_t i = 0; i < 2 * lanes; ++i) {
        const Xmm& pick = i < lanes ? dst : src;
        const int64_t v = sign_extend(lane_get(pick.lo, pick.hi, from, i % lanes), from / 8);
        const int64_t top = is_unsigned ? (int64_t{1} << to) - 1 : (int64_t{1} << (to - 1)) - 1, bottom = is_unsigned ? 0 : -(int64_t{1} << (to - 1));
        const int64_t clamped = v > top ? top : v < bottom ? bottom : v;
        lane_set(&out.lo, &out.hi, to, i, static_cast<uint64_t>(clamped));
      }
      dst = out;
      break;
    }
    case 0xD5: lanewise(16, [](uint64_t a, uint64_t b) { return a * b; }); break;
    case 0xE5: lanewise(16, [](uint64_t a, uint64_t b) { return static_cast<uint64_t>(sign_extend(a, 2) * sign_extend(b, 2)) >> 16; }); break;
    case 0xE4: lanewise(16, [](uint64_t a, uint64_t b) { return (a * b) >> 16; }); break;
    case 0xF4: {      // pmuludq: the low dword of each qword lane
      uint64_t hi0, lo0, hi1, lo1;
      multiply_unsigned(dst.lo & 0xFFFFFFFFu, src.lo & 0xFFFFFFFFu, &hi0, &lo0);
      multiply_unsigned(dst.hi & 0xFFFFFFFFu, src.hi & 0xFFFFFFFFu, &hi1, &lo1);
      dst.lo = lo0;
      dst.hi = lo1;
      break;
    }
    case 0xF5: {      // pmaddwd
      Xmm out;
      for (uint32_t i = 0; i < 4; ++i) {
        const int64_t p0 = sign_extend(lane_get(dst.lo, dst.hi, 16, 2 * i), 2) * sign_extend(lane_get(src.lo, src.hi, 16, 2 * i), 2);
        const int64_t p1 = sign_extend(lane_get(dst.lo, dst.hi, 16, 2 * i + 1), 2) * sign_extend(lane_get(src.lo, src.hi, 16, 2 * i + 1), 2);
        lane_set(&out.lo, &out.hi, 32, i, static_cast<uint64_t>(p0 + p1));
      }
      dst = out;
      break;
    }
    case 0xF6: {      // psadbw
      uint64_t sums[2] = {0, 0};
      for (uint32_t i = 0; i < 16; ++i) {
        const int64_t a = static_cast<int64_t>(lane_get(dst.lo, dst.hi, 8, i)), b = static_cast<int64_t>(lane_get(src.lo, src.hi, 8, i));
        sums[i / 8] += static_cast<uint64_t>(a > b ? a - b : b - a);
      }
      dst.lo = sums[0];
      dst.hi = sums[1];
      break;
    }
    case 0xDC: lanewise(8, [](uint64_t a, uint64_t b) { return a + b > 0xFF ? uint64_t{0xFF} : a + b; }); break;
    case 0xDD: lanewise(16, [](uint64_t a, uint64_t b) { return a + b > 0xFFFF ? uint64_t{0xFFFF} : a + b; }); break;
    case 0xD8: lanewise(8, [](uint64_t a, uint64_t b) { return a > b ? a - b : uint64_t{0}; }); break;
    case 0xD9: lanewise(16, [](uint64_t a, uint64_t b) { return a > b ? a - b : uint64_t{0}; }); break;
    case 0xEC: lanewise(8, [](uint64_t a, uint64_t b) { const int64_t v = sign_extend(a, 1) + sign_extend(b, 1); return static_cast<uint64_t>(v > 127 ? 127 : v < -128 ? -128 : v); }); break;
    case 0xED: lanewise(16, [](uint64_t a, uint64_t b) { const int64_t v = sign_extend(a, 2) + sign_extend(b, 2); return static_cast<uint64_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }); break;
    case 0xE8: lanewise(8, [](uint64_t a, uint64_t b) { const int64_t v = sign_extend(a, 1) - sign_extend(b, 1); return static_cast<uint64_t>(v > 127 ? 127 : v < -128 ? -128 : v); }); break;
    case 0xE9: lanewise(16, [](uint64_t a, uint64_t b) { const int64_t v = sign_extend(a, 2) - sign_extend(b, 2); return static_cast<uint64_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }); break;
    case 0xEA: lanewise(16, [](uint64_t a, uint64_t b) { return sign_extend(a, 2) < sign_extend(b, 2) ? a : b; }); break;
    case 0xEE: lanewise(16, [](uint64_t a, uint64_t b) { return sign_extend(a, 2) > sign_extend(b, 2) ? a : b; }); break;
    case 0xE0: lanewise(8, [](uint64_t a, uint64_t b) { return (a + b + 1) >> 1; }); break;
    case 0xE3: lanewise(16, [](uint64_t a, uint64_t b) { return (a + b + 1) >> 1; }); break;
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: {      // shifts by the low qword of xmm/m128
      const uint32_t width = (op & 3) == 1 ? 16 : (op & 3) == 2 ? 32 : 64;
      const bool left = op >= 0xF1, arithmetic = op == 0xE1 || op == 0xE2;
      if (arithmetic && width == 64) return false;
      const uint64_t count = src.lo;
      for (uint32_t i = 0; i < 128 / width; ++i) {
        const uint64_t v = lane_get(dst.lo, dst.hi, width, i);
        uint64_t r;
        if (arithmetic) r = static_cast<uint64_t>(sign_extend(v, width / 8) >> (count >= width ? width - 1 : count)) & ((uint64_t{1} << width) - 1);
        else if (count >= width) r = 0;
        else r = left ? v << count : v >> count;
        lane_set(&dst.lo, &dst.hi, width, i, r);
      }
      break;
    }
    case 0xDA: lanewise(8, [](uint64_t a, uint64_t b) { return a < b ? a : b; }); break;
    case 0xDE: lanewise(8, [](uint64_t a, uint64_t b) { return a > b ? a : b; }); break;
    default: break;
  }
  return true;
}

X86_64Cpu::Step X86_64Cpu::step_sse(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  auto advance = [&]() { s_.rip += d_.position; };
  const bool p66 = d_.operand16, pf3 = d_.rep, pf2 = d_.repne;
  if (mmx_opcode(op)) return mmx_instruction(m, op);
  {
    bool handled = true;
    const Step float_result = step_float(m, op, &handled);
    if (handled) return float_result;
  }

  switch (op) {
    case 0x10: case 0x11: {      // movups / movupd / movss / movsd
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      Xmm& reg = s_.xmm[d_.reg];
      if (pf3 || pf2) {
        const uint32_t size = pf3 ? 4 : 8;
        if (op == 0x10) {
          uint64_t v = 0;
          if (d_.mod == 3) {
            v = size == 4 ? s_.xmm[d_.rm].lo & 0xFFFFFFFFu : s_.xmm[d_.rm].lo;
            reg.lo = size == 4 ? (reg.lo & ~uint64_t{0xFFFFFFFFu}) | v : v;
          } else {
            if (!load(m, memory_address(), size, &v)) return Step::Fault;
            reg.lo = v;
            reg.hi = 0;
          }
        } else if (d_.mod == 3) {
          Xmm& dst = s_.xmm[d_.rm];
          dst.lo = size == 4 ? (dst.lo & ~uint64_t{0xFFFFFFFFu}) | (reg.lo & 0xFFFFFFFFu) : reg.lo;
        } else if (!store(m, memory_address(), size, reg.lo)) {
          return Step::Fault;
        }
      } else if (op == 0x10) {
        Xmm v;
        if (!read_xmm_rm(m, &v)) return Step::Fault;
        reg = v;
      } else if (!write_xmm_rm(m, reg)) {
        return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0x12: case 0x13: case 0x16: case 0x17: {      // movlps / movhps (movhlps / movlhps between registers)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      Xmm& reg = s_.xmm[d_.reg];
      const bool high = op >= 0x16;
      if ((op == 0x12 || op == 0x16) && (pf2 || pf3)) {      // SSE3: movddup (F2 12), movsldup (F3 12), movshdup (F3 16)
        if (op == 0x16 && pf2) return fail(6);
        Xmm v;
        if (pf2) {
          uint64_t q;
          if (d_.mod == 3) q = s_.xmm[d_.rm].lo;
          else if (!load(m, memory_address(), 8, &q)) return Step::Fault;
          v.lo = v.hi = q;
        } else {
          Xmm src;
          if (!read_xmm_rm(m, &src)) return Step::Fault;
          const uint32_t first = op == 0x12 ? 0 : 1;      // the even or the odd dwords, each doubled
          lane_set(&v.lo, &v.hi, 32, 0, lane_get(src.lo, src.hi, 32, first));
          lane_set(&v.lo, &v.hi, 32, 1, lane_get(src.lo, src.hi, 32, first));
          lane_set(&v.lo, &v.hi, 32, 2, lane_get(src.lo, src.hi, 32, first + 2));
          lane_set(&v.lo, &v.hi, 32, 3, lane_get(src.lo, src.hi, 32, first + 2));
        }
        s_.xmm[d_.reg] = v;
        advance();
        return Step::Ok;
      }
      if (op == 0x12 || op == 0x16) {
        if (d_.mod == 3) {
          if (high) reg.hi = s_.xmm[d_.rm].lo;
          else reg.lo = s_.xmm[d_.rm].hi;
        } else {
          uint64_t v = 0;
          if (!load(m, memory_address(), 8, &v)) return Step::Fault;
          (high ? reg.hi : reg.lo) = v;
        }
      } else {
        if (d_.mod == 3) return fail(6);
        if (!store(m, memory_address(), 8, high ? reg.hi : reg.lo)) return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0xF0: {      // lddqu (F2): an unaligned 16-byte load
      if (!pf2) return fail(6);
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (d_.mod == 3) return fail(6);
      Xmm v;
      if (!read_xmm_rm(m, &v)) return Step::Fault;
      s_.xmm[d_.reg] = v;
      advance();
      return Step::Ok;
    }
    case 0x28: case 0x29: case 0x6F: case 0x7F: {      // movaps / movapd / movdqa / movdqu
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (op == 0x6F && !p66 && !pf3) return fail(6);      // (the MMX form)
      if (op == 0x28 || op == 0x6F) {
        Xmm v;
        if (!read_xmm_rm(m, &v)) return Step::Fault;
        s_.xmm[d_.reg] = v;
      } else if (!write_xmm_rm(m, s_.xmm[d_.reg])) {
        return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0x6E: {      // movd / movq xmm, r/m
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!p66) return fail(6);
      uint64_t v = 0;
      if (!read_rm(m, d_.rex_w ? 8 : 4, &v)) return Step::Fault;
      s_.xmm[d_.reg].lo = v;
      s_.xmm[d_.reg].hi = 0;
      advance();
      return Step::Ok;
    }
    case 0x7E: {      // movd / movq r/m, xmm; F3: movq xmm, xmm/m64
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (pf3) {
        uint64_t v = 0;
        if (d_.mod == 3) v = s_.xmm[d_.rm].lo;
        else if (!load(m, memory_address(), 8, &v)) return Step::Fault;
        s_.xmm[d_.reg].lo = v;
        s_.xmm[d_.reg].hi = 0;
      } else {
        if (!p66) return fail(6);
        const uint32_t size = d_.rex_w ? 8 : 4;
        if (!write_rm(m, size, s_.xmm[d_.reg].lo & mask_of(size))) return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0xD6: {      // movq xmm/m64, xmm
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!p66) return fail(6);
      if (d_.mod == 3) {
        s_.xmm[d_.rm].lo = s_.xmm[d_.reg].lo;
        s_.xmm[d_.rm].hi = 0;
      } else if (!store(m, memory_address(), 8, s_.xmm[d_.reg].lo)) {
        return Step::Fault;
      }
      advance();
      return Step::Ok;
    }
    case 0xD7: {      // pmovmskb
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!p66 || d_.mod != 3) return fail(6);
      uint32_t mask = 0;
      for (uint32_t i = 0; i < 16; ++i) mask |= static_cast<uint32_t>((lane_get(s_.xmm[d_.rm].lo, s_.xmm[d_.rm].hi, 8, i) >> 7) & 1) << i;
      set_reg(d_.reg, 4, mask);
      advance();
      return Step::Ok;
    }
    case 0x70: {      // pshufd (66), pshuflw (F2), pshufhw (F3)
      if (!decode_modrm()) return bad_decode();
      const uint8_t order = static_cast<uint8_t>(next_imm(1, false));
      if (!finish_decode()) return bad_decode();
      if (!p66 && !pf2 && !pf3) return fail(6);      // (the MMX form)
      Xmm src;
      if (!read_xmm_rm(m, &src)) return Step::Fault;
      Xmm out = src;
      if (p66) {
        for (uint32_t i = 0; i < 4; ++i) lane_set(&out.lo, &out.hi, 32, i, lane_get(src.lo, src.hi, 32, (order >> (2 * i)) & 3));
      } else {
        const uint32_t base = pf2 ? 0 : 4;      // the low or the high four words
        for (uint32_t i = 0; i < 4; ++i) lane_set(&out.lo, &out.hi, 16, base + i, lane_get(src.lo, src.hi, 16, base + ((order >> (2 * i)) & 3)));
      }
      s_.xmm[d_.reg] = out;
      advance();
      return Step::Ok;
    }
    case 0xC5: {      // pextrw r32, xmm, imm8
      if (!decode_modrm()) return bad_decode();
      const uint32_t index = static_cast<uint32_t>(next_imm(1, false)) & 7;
      if (!finish_decode()) return bad_decode();
      if (!p66 || d_.mod != 3) return fail(6);
      set_reg(d_.reg, 4, lane_get(s_.xmm[d_.rm].lo, s_.xmm[d_.rm].hi, 16, index));
      advance();
      return Step::Ok;
    }
    case 0xC4: {      // pinsrw xmm, r32/m16, imm8
      if (!decode_modrm()) return bad_decode();
      const uint32_t index = static_cast<uint32_t>(next_imm(1, false)) & 7;
      if (!finish_decode()) return bad_decode();
      if (!p66) return fail(6);
      uint64_t v = 0;
      if (!read_rm(m, 2, &v)) return Step::Fault;
      Xmm& x = s_.xmm[d_.reg];
      lane_set(&x.lo, &x.hi, 16, index, v & 0xFFFF);
      advance();
      return Step::Ok;
    }
    case 0x71: case 0x72: case 0x73: {      // shifts by immediate: psrlw/psraw/psllw, psrld/psrad/pslld, psrlq/psrldq/psllq/pslldq
      if (!decode_modrm()) return bad_decode();
      const uint32_t count = static_cast<uint32_t>(next_imm(1, false));
      if (!finish_decode()) return bad_decode();
      if (!p66 || d_.mod != 3) return fail(6);
      const uint32_t kind = d_.reg & 7;
      Xmm& x = s_.xmm[d_.rm];
      if (op == 0x73 && (kind == 3 || kind == 7)) {      // whole-register byte shifts
        const uint32_t n = count > 16 ? 16 : count;
        Xmm out;
        for (uint32_t i = 0; i < 16; ++i) {
          const int64_t from = kind == 3 ? static_cast<int64_t>(i) + n : static_cast<int64_t>(i) - n;
          lane_set(&out.lo, &out.hi, 8, i, from >= 0 && from < 16 ? lane_get(x.lo, x.hi, 8, static_cast<uint32_t>(from)) : 0);
        }
        x = out;
      } else {
        const uint32_t width = op == 0x71 ? 16 : op == 0x72 ? 32 : 64;
        const bool left = kind == 6, arithmetic = kind == 4;
        if (kind != 2 && kind != 4 && kind != 6) return fail(6);
        for (uint32_t i = 0; i < 128 / width; ++i) {
          const uint64_t v = lane_get(x.lo, x.hi, width, i);
          uint64_t r;
          if (arithmetic) {
            if (width == 64) return fail(6);
            r = static_cast<uint64_t>(sign_extend(v, width / 8) >> (count > width - 1 ? width - 1 : count)) & ((uint64_t{1} << width) - 1);
          } else if (count >= width) {
            r = 0;
          } else {
            r = left ? (v << count) : (v >> count);
          }
          lane_set(&x.lo, &x.hi, width, i, r);
        }
      }
      advance();
      return Step::Ok;
    }
    default:
      break;
  }

  // The remaining opcodes take xmm, xmm/m128 and work on the whole register (66 prefix) or just as bits.
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  Xmm src;
  Xmm& dst = s_.xmm[d_.reg];
  if (!sse_integer_valid(op)) return fail(6);
  if (!read_xmm_rm(m, &src)) return Step::Fault;
  if (!sse_integer_core(op, dst, src)) return fail(6);
  advance();
  return Step::Ok;
}


// --- floating-point SSE and SSE2 (soft-float) -----------------------------------------------------------------------------------------------

bool X86_64Cpu::read_scalar(GuestMemory& m, uint32_t size, uint64_t* out) {
  if (d_.mod == 3) {
    *out = s_.xmm[d_.rm].lo & mask_of(size);
    return true;
  }
  return load(m, memory_address(), size, out);
}

X86_64Cpu::Step X86_64Cpu::step_float(GuestMemory& m, uint8_t op, bool* handled) {
  using namespace softfloat;
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  auto advance = [&]() { s_.rip += d_.position; };
  const bool p66 = d_.operand16, pf3 = d_.rep, pf2 = d_.repne;
  const bool scalar = pf3 || pf2, dbl = pf2 || (p66 && !pf3);      // the operand type the prefix selects
  const Format fmt = dbl ? Format::F64 : Format::F32;
  const uint32_t width = dbl ? 64 : 32, bytes = width / 8, lanes = 128 / width;
  Env env;
  env.mode = static_cast<Rounding>((s_.mxcsr >> 13) & 3);
  env.daz = (s_.mxcsr & 0x40) != 0;
  env.ftz = (s_.mxcsr & 0x8000) != 0;
  auto done = [&]() {
    if (env.flags & kInvalid) env.flags &= ~kDenormal;      // (an invalid operation takes precedence over the denormal-operand flag)
    s_.mxcsr |= env.flags;
    advance();
    return Step::Ok;
  };
  // The second operand: one element (scalar) or the whole register.
  auto source = [&](Xmm* src) {
    if (scalar) {
      src->hi = 0;
      return read_scalar(m, bytes, &src->lo);
    }
    return read_xmm_rm(m, src);
  };

  switch (op) {
    case 0x52: case 0x53: {      // rsqrtps / rsqrtss, rcpps / rcpss: the reciprocal (square root), here the correctly rounded value; no exception flags, denormal inputs are zeros and tiny results are zero
      if (p66 || pf2) return fail(6);
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      Xmm src;
      if (!source(&src)) return Step::Fault;
      Xmm& dst = s_.xmm[d_.reg];
      const uint32_t count = scalar ? 1 : lanes;
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t b = lane_get(src.lo, src.hi, 32, i);
        Env le;
        le.mode = Rounding::NearestEven;
        le.daz = le.ftz = true;
        uint64_t r;
        const bool negative = (b >> 31) & 1;
        const uint32_t exponent = (b >> 23) & 0xFF;
        if (exponent == 0xFF && (b & 0x7FFFFF)) r = b | 0x400000;      // a NaN, made quiet
        else if (exponent == 0) r = negative ? 0xFF800000u : 0x7F800000u;      // zero and denormal: infinity of the same sign
        else if (op == 0x53) r = exponent == 0xFF ? (negative ? 0x80000000u : 0) : softfloat::div(Format::F32, 0x3F800000u, b, le);
        else if (negative) r = 0xFFC00000u;      // the square root of a negative number
        else if (exponent == 0xFF) r = 0;
        else {
          Env c;
          c.mode = Rounding::NearestEven;
          const uint64_t wide = softfloat::convert(Format::F32, Format::F64, b, c);
          r = softfloat::convert(Format::F64, Format::F32, softfloat::div(Format::F64, 0x3FF0000000000000ull, sqrt(Format::F64, wide, c), c), c);
          if ((r & 0x7F800000u) == 0) r &= 0x80000000u;
        }
        if (op == 0x53 && (r & 0x7F800000u) == 0) r &= 0x80000000u;
        lane_set(&dst.lo, &dst.hi, 32, i, r);
      }
      env.flags = 0;
      return done();
    }
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {      // sqrt add mul sub min div max
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      Xmm src;
      if (!source(&src)) return Step::Fault;
      Xmm& dst = s_.xmm[d_.reg];
      const uint32_t count = scalar ? 1 : lanes;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t a = lane_get(dst.lo, dst.hi, width, i), b = lane_get(src.lo, src.hi, width, i);
        Env le = env;      // (the flags of one element: an invalid operation there takes precedence over its denormal-operand flag)
        le.flags = 0;
        uint64_t r = 0;
        switch (op) {
          case 0x51: r = sqrt(fmt, b, le); break;
          case 0x58: r = add(fmt, a, b, le); break;
          case 0x59: r = mul(fmt, a, b, le); break;
          case 0x5C: r = softfloat::sub(fmt, a, b, le); break;
          case 0x5D: r = minimum(fmt, a, b, le); break;
          case 0x5E: r = softfloat::div(fmt, a, b, le); break;
          default: r = maximum(fmt, a, b, le); break;
        }
        if (le.flags & kInvalid) le.flags &= ~kDenormal;
        env.flags |= le.flags;
        lane_set(&dst.lo, &dst.hi, width, i, r);
      }
      return done();
    }
    case 0x5A: {      // cvtps2pd (none), cvtpd2ps (66), cvtss2sd (F3), cvtsd2ss (F2)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      Xmm& dst = s_.xmm[d_.reg];
      if (scalar) {
        const Format from = pf3 ? Format::F32 : Format::F64, to = pf3 ? Format::F64 : Format::F32;
        uint64_t v = 0;
        if (!read_scalar(m, pf3 ? 4 : 8, &v)) return Step::Fault;
        const uint64_t r = convert(from, to, v, env);
        if (pf3) dst.lo = r;
        else dst.lo = (dst.lo & ~uint64_t{0xFFFFFFFFu}) | r;
      } else if (p66) {      // two doubles -> two floats in the low half
        Xmm src;
        if (!read_xmm_rm(m, &src)) return Step::Fault;
        Xmm out;
        lane_set(&out.lo, &out.hi, 32, 0, convert(Format::F64, Format::F32, src.lo, env));
        lane_set(&out.lo, &out.hi, 32, 1, convert(Format::F64, Format::F32, src.hi, env));
        dst = out;
      } else {      // two floats -> two doubles
        uint64_t v = 0;
        if (!read_scalar(m, 8, &v)) return Step::Fault;
        Xmm out;
        out.lo = convert(Format::F32, Format::F64, v & 0xFFFFFFFFu, env);
        out.hi = convert(Format::F32, Format::F64, v >> 32, env);
        dst = out;
      }
      return done();
    }
    case 0x5B: {      // cvtdq2ps (none), cvtps2dq (66), cvttps2dq (F3)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      Xmm src;
      if (!read_xmm_rm(m, &src)) return Step::Fault;
      Xmm out;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint64_t v = lane_get(src.lo, src.hi, 32, i);
        const uint64_t r = (!p66 && !pf3) ? from_int(Format::F32, sign_extend(v, 4), env) : static_cast<uint64_t>(to_int(Format::F32, v, 32, pf3, env));
        lane_set(&out.lo, &out.hi, 32, i, r);
      }
      s_.xmm[d_.reg] = out;
      env.flags &= ~kDenormal;
      return done();
    }
    case 0xE6: {      // cvtdq2pd (F3), cvtpd2dq (F2), cvttpd2dq (66)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!pf3 && !pf2 && !p66) return fail(6);
      Xmm out;
      if (pf3) {
        uint64_t v = 0;
        if (!read_scalar(m, 8, &v)) return Step::Fault;
        out.lo = from_int(Format::F64, sign_extend(v & 0xFFFFFFFFu, 4), env);
        out.hi = from_int(Format::F64, sign_extend(v >> 32, 4), env);
      } else {
        Xmm src;
        if (!read_xmm_rm(m, &src)) return Step::Fault;
        lane_set(&out.lo, &out.hi, 32, 0, static_cast<uint64_t>(to_int(Format::F64, src.lo, 32, p66, env)));
        lane_set(&out.lo, &out.hi, 32, 1, static_cast<uint64_t>(to_int(Format::F64, src.hi, 32, p66, env)));
      }
      s_.xmm[d_.reg] = out;
      env.flags &= ~kDenormal;
      return done();
    }
    case 0x2A: {      // cvtsi2ss / cvtsi2sd
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!scalar) return fail(6);
      const uint32_t size = d_.rex_w ? 8 : 4;
      uint64_t v = 0;
      if (!read_rm(m, size, &v)) return Step::Fault;
      const uint64_t r = from_int(fmt, sign_extend(v, size), env);
      Xmm& dst = s_.xmm[d_.reg];
      dst.lo = dbl ? r : (dst.lo & ~uint64_t{0xFFFFFFFFu}) | r;
      env.flags &= ~kDenormal;
      return done();
    }
    case 0x2C: case 0x2D: {      // cvttss2si / cvttsd2si (2C), cvtss2si / cvtsd2si (2D)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (!scalar) return fail(6);
      uint64_t v = 0;
      if (!read_scalar(m, bytes, &v)) return Step::Fault;
      const uint32_t size = d_.rex_w ? 8 : 4;
      const int64_t r = to_int(fmt, v, size * 8, op == 0x2C, env);
      set_reg(d_.reg, size, static_cast<uint64_t>(r));
      env.flags &= ~kDenormal;
      return done();
    }
    case 0x2E: case 0x2F: {      // ucomiss / ucomisd, comiss / comisd
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      const Format f2 = p66 ? Format::F64 : Format::F32;
      uint64_t v = 0;
      if (!read_scalar(m, p66 ? 8 : 4, &v)) return Step::Fault;
      const uint64_t a = s_.xmm[d_.reg].lo & mask_of(p66 ? 8 : 4);
      const Order order = compare(f2, a, v, op == 0x2F, env);
      uint64_t flags = 0;
      switch (order) {
        case Order::Unordered: flags = kZero | kParity | kCarry; break;
        case Order::Less: flags = kCarry; break;
        case Order::Equal: flags = kZero; break;
        default: break;
      }
      s_.rflags = (s_.rflags & ~(kZero | kParity | kCarry | kOverflow | kAdjust | kSign)) | flags;
      return done();
    }
    case 0xC2: {      // cmpps / cmppd / cmpss / cmpsd
      if (!decode_modrm()) return bad_decode();
      const uint32_t predicate = static_cast<uint32_t>(next_imm(1, false));
      if (!finish_decode()) return bad_decode();
      if (predicate > 7) return fail(6);
      Xmm src;
      if (!source(&src)) return Step::Fault;
      Xmm& dst = s_.xmm[d_.reg];
      const uint32_t count = scalar ? 1 : lanes;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t a = lane_get(dst.lo, dst.hi, width, i), b = lane_get(src.lo, src.hi, width, i);
        // 0 eq, 1 lt, 2 le, 3 unord, 4 neq, 5 nlt, 6 nle, 7 ord; the ordering ones (1, 2, 5, 6) signal on any NaN
        const Order order = compare(fmt, a, b, predicate == 1 || predicate == 2 || predicate == 5 || predicate == 6, env);
        bool result = false;
        switch (predicate) {
          case 0: result = order == Order::Equal; break;
          case 1: result = order == Order::Less; break;
          case 2: result = order == Order::Less || order == Order::Equal; break;
          case 3: result = order == Order::Unordered; break;
          case 4: result = order != Order::Equal; break;
          case 5: result = order != Order::Less; break;
          case 6: result = order != Order::Less && order != Order::Equal; break;
          default: result = order != Order::Unordered; break;
        }
        lane_set(&dst.lo, &dst.hi, width, i, result ? (dbl ? ~uint64_t{0} : uint64_t{0xFFFFFFFFu}) : 0);
      }
      return done();
    }
    case 0x14: case 0x15: {      // unpcklps/pd, unpckhps/pd
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (pf3 || pf2) return fail(6);
      Xmm src;
      if (!read_xmm_rm(m, &src)) return Step::Fault;
      Xmm& dst = s_.xmm[d_.reg];
      Xmm out;
      const uint32_t w = p66 ? 64 : 32, n = 128 / w;
      for (uint32_t i = 0; i < n / 2; ++i) {
        const uint32_t from = (op == 0x15 ? n / 2 : 0) + i;
        lane_set(&out.lo, &out.hi, w, 2 * i, lane_get(dst.lo, dst.hi, w, from));
        lane_set(&out.lo, &out.hi, w, 2 * i + 1, lane_get(src.lo, src.hi, w, from));
      }
      dst = out;
      return done();
    }
    case 0xC6: {      // shufps / shufpd
      if (!decode_modrm()) return bad_decode();
      const uint32_t order = static_cast<uint32_t>(next_imm(1, false));
      if (!finish_decode()) return bad_decode();
      if (pf3 || pf2) return fail(6);
      Xmm src;
      if (!read_xmm_rm(m, &src)) return Step::Fault;
      Xmm& dst = s_.xmm[d_.reg];
      Xmm out;
      if (p66) {
        out.lo = (order & 1) ? dst.hi : dst.lo;
        out.hi = (order & 2) ? src.hi : src.lo;
      } else {
        lane_set(&out.lo, &out.hi, 32, 0, lane_get(dst.lo, dst.hi, 32, order & 3));
        lane_set(&out.lo, &out.hi, 32, 1, lane_get(dst.lo, dst.hi, 32, (order >> 2) & 3));
        lane_set(&out.lo, &out.hi, 32, 2, lane_get(src.lo, src.hi, 32, (order >> 4) & 3));
        lane_set(&out.lo, &out.hi, 32, 3, lane_get(src.lo, src.hi, 32, (order >> 6) & 3));
      }
      dst = out;
      return done();
    }
    case 0x50: {      // movmskps / movmskpd
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (d_.mod != 3 || pf3 || pf2) return fail(6);
      const Xmm& x = s_.xmm[d_.rm];
      uint32_t mask = 0;
      const uint32_t w = p66 ? 64 : 32;
      for (uint32_t i = 0; i < 128 / w; ++i) mask |= static_cast<uint32_t>((lane_get(x.lo, x.hi, w, i) >> (w - 1)) & 1) << i;
      set_reg(d_.reg, 4, mask);
      return done();
    }
    case 0x2B: case 0xE7: {      // movntps/pd, movntdq: plain stores; SSE4a movntss / movntsd (F3 / F2 0F 2B) store a scalar
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (d_.mod == 3) return fail(6);
      if (op == 0x2B && (pf3 || pf2)) {
        if (!store(m, memory_address(), pf3 ? 4 : 8, pf3 ? s_.xmm[d_.reg].lo & 0xFFFFFFFFu : s_.xmm[d_.reg].lo)) return Step::Fault;
        return done();
      }
      if (pf3 || pf2) return fail(6);
      if (!write_xmm_rm(m, s_.xmm[d_.reg])) return Step::Fault;
      return done();
    }
    case 0x78: case 0x79:      // SSE4a: extrq (66), insertq (F2)
      if (!(p66 != pf2) || pf3) return fail(6);
      return sse4a_bitfield(m, op);
    case 0x7C: case 0x7D: case 0xD0: {      // SSE3: haddps/pd, hsubps/pd, addsubps/pd (F2: single, 66: double)
      if (!decode_modrm() || !finish_decode()) return bad_decode();
      if (pf3 || (!p66 && !pf2)) return fail(6);
      const bool is_double = p66;
      const Format f = is_double ? Format::F64 : Format::F32;
      const uint32_t w = is_double ? 64 : 32, n = 128 / w;
      Xmm src;
      if (!read_xmm_rm(m, &src)) return Step::Fault;
      const Xmm dst = s_.xmm[d_.reg];
      Xmm out;
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t a, b;
        bool subtract;
        if (op == 0xD0) {
          a = lane_get(dst.lo, dst.hi, w, i);
          b = lane_get(src.lo, src.hi, w, i);
          subtract = (i & 1) == 0;
        } else {      // the pairs of the destination fill the low half of the result, the pairs of the source the high half
          const Xmm& from = i < n / 2 ? dst : src;
          const uint32_t k = 2 * (i % (n / 2));
          a = lane_get(from.lo, from.hi, w, k);
          b = lane_get(from.lo, from.hi, w, k + 1);
          subtract = op == 0x7D;
        }
        Env le = env;
        le.flags = 0;
        const uint64_t r = subtract ? softfloat::sub(f, a, b, le) : add(f, a, b, le);
        if (le.flags & kInvalid) le.flags &= ~kDenormal;
        env.flags |= le.flags;
        lane_set(&out.lo, &out.hi, w, i, r);
      }
      s_.xmm[d_.reg] = out;
      return done();
    }
    default:
      *handled = false;
      return Step::Ok;
  }
}

// The FXSAVE image: x87 control and status words at 0 and 2, MXCSR at 24 (and its mask at 28), XMM0..15 at 160.
void X86_64Cpu::save_fpu_image(uint8_t* image) const {
  ByteView(image, 512).fill(0);
  auto put = [&](uint32_t at, uint64_t v, uint32_t n) { for (uint32_t i = 0; i < n; ++i) image[at + i] = uint8_t(v >> (8 * i)); };
  put(0, s_.fpu_control, 2);
  put(2, s_.fpu_status, 2);
  put(4, s_.fpu_valid, 1);      // the abbreviated tag word: one bit per physical register, 1 = holds a value
  put(24, s_.mxcsr, 4);
  put(28, 0xFFBF, 4);
  for (uint32_t i = 0; i < 8; ++i) {      // ST(0)..ST(7) in stack order
    const softfloat::Ext& e = x87_phys(i);
    put(32 + 16 * i, e.mant, 8);
    put(40 + 16 * i, e.se, 2);
  }
  for (uint32_t i = 0; i < 16; ++i) {
    put(160 + 16 * i, s_.xmm[i].lo, 8);
    put(168 + 16 * i, s_.xmm[i].hi, 8);
  }
}

void X86_64Cpu::load_fpu_image(const uint8_t* image) {
  auto get = [&](uint32_t at, uint32_t n) { uint64_t v = 0; for (uint32_t i = 0; i < n; ++i) v |= uint64_t{image[at + i]} << (8 * i); return v; };
  s_.fpu_control = static_cast<uint16_t>(get(0, 2));
  s_.fpu_status = static_cast<uint16_t>(get(2, 2));
  s_.fpu_valid = static_cast<uint8_t>(get(4, 1));
  s_.mxcsr = static_cast<uint32_t>(get(24, 4)) & 0xFFBF;
  for (uint32_t i = 0; i < 8; ++i) {
    softfloat::Ext& e = x87_phys(i);
    e.mant = get(32 + 16 * i, 8);
    e.se = static_cast<uint16_t>(get(40 + 16 * i, 2));
  }
  for (uint32_t i = 0; i < 16; ++i) {
    s_.xmm[i].lo = get(160 + 16 * i, 8);
    s_.xmm[i].hi = get(168 + 16 * i, 8);
  }
}

bool X86_64Cpu::get_x86_context(process::X86Context* out) const {
  for (uint32_t i = 0; i < 16; ++i) out->gpr[i] = s_.r[i];
  out->rip = s_.rip;
  out->rflags = s_.rflags;
  save_fpu_image(out->fpu);
  for (uint32_t i = 0; i < 16; ++i)
    for (uint32_t b = 0; b < 8; ++b) {
      out->ymm_high[16 * i + b] = static_cast<uint8_t>(s_.ymm_high[i].lo >> (8 * b));
      out->ymm_high[16 * i + 8 + b] = static_cast<uint8_t>(s_.ymm_high[i].hi >> (8 * b));
    }
  return true;
}

bool X86_64Cpu::set_x86_context(const process::X86Context& context) {
  for (uint32_t i = 0; i < 16; ++i) s_.r[i] = context.gpr[i];
  s_.rip = context.rip;
  s_.rflags = (context.rflags & (kCarry | kParity | kAdjust | kZero | kSign | kDirection | kOverflow)) | 0x202;
  load_fpu_image(context.fpu);
  for (uint32_t i = 0; i < 16; ++i) {
    s_.ymm_high[i] = Xmm();
    for (uint32_t b = 0; b < 8; ++b) {
      s_.ymm_high[i].lo |= uint64_t{context.ymm_high[16 * i + b]} << (8 * b);
      s_.ymm_high[i].hi |= uint64_t{context.ymm_high[16 * i + 8 + b]} << (8 * b);
    }
  }
  return true;
}

UniquePointer<IGuestCpu> X86_64Cpu::clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const {
  UniquePointer<X86_64Cpu> copy = UniquePointer<X86_64Cpu>::make(allocator, *this);
  if (!copy) return UniquePointer<IGuestCpu>();
  copy->s_.r[0] = 0;
  if (stack_pointer) copy->s_.r[4] = stack_pointer;
  if (set_tls) copy->s_.fs_base = tls;
  copy->retired_ = 0;
  return UniquePointer<IGuestCpu>(static_cast<UniquePointer<X86_64Cpu>&&>(copy));
}

// --- running ---------------------------------------------------------------------------------------------------------------------------------

StopReason X86_64Cpu::run(GuestMemory& memory, uint64_t max_instructions) {
  for (uint64_t i = 0; i < max_instructions; ++i) {
    const Step step_result = step(memory);
    if (step_result == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (step_result == Step::Syscall) return StopReason::Syscall;
  }
  return StopReason::Budget;
}

}  // namespace emulation
