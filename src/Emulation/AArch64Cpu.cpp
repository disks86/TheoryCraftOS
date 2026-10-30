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
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/Wide.hpp"

namespace emulation {

namespace {

constexpr uint64_t ones(uint32_t n) { return n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1; }
constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t bits_of(uint32_t insn, uint32_t hi, uint32_t lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }

uint64_t rotate_right(uint64_t v, uint32_t amount, uint32_t size) {
  amount %= size;
  if (amount == 0) return v & ones(size);
  return ((v >> amount) | (v << (size - amount))) & ones(size);
}

// DecodeBitMasks: the wmask and tmask of a logical immediate or bitfield instruction.
bool decode_bitmasks(bool n, uint32_t imms, uint32_t immr, bool immediate, uint32_t datasize, uint64_t* wmask, uint64_t* tmask) {
  const uint32_t combined = (n ? 0x40u : 0) | (~imms & 0x3Fu);
  if (combined == 0) return false;
  uint32_t len = 6;
  while (!((combined >> len) & 1)) --len;
  if (len < 1) return false;
  const uint32_t esize = 1u << len;
  if (esize > datasize) return false;
  const uint32_t levels = esize - 1;
  if (immediate && (imms & levels) == levels) return false;
  const uint32_t s = imms & levels, r = immr & levels;
  const uint32_t d = (s - r) & levels;
  uint64_t welem = ones(s + 1), telem = ones(d + 1);
  welem = rotate_right(welem, r, esize);
  uint64_t w = 0, t = 0;
  for (uint32_t i = 0; i < datasize; i += esize) {
    w |= welem << i;
    t |= telem << i;
  }
  *wmask = w & ones(datasize);
  *tmask = t & ones(datasize);
  return true;
}

uint32_t count_leading_zeros(uint64_t v, uint32_t size) {
  uint32_t n = 0;
  for (int i = static_cast<int>(size) - 1; i >= 0 && !((v >> i) & 1); --i) ++n;
  return n;
}

}  // namespace

// --- state ---------------------------------------------------------------------------------------------------------------------------------

void AArch64Cpu::reset(const process::ProgramStart& start) {
  s_ = State();
  s_.pc = start.entry;
  s_.sp = start.stack_pointer;
  s_.tpidr_el0 = start.fs_base;
  fault_ = GuestFault();
  retired_ = 0;
  exclusive_valid_ = false;
}

bool AArch64Cpu::pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const {
  if (abi != process::SyscallAbi::Linux) return false;      // (there is no AArch64 Windows interface)
  return process::decode_aarch64_linux(ArrayView<const uint64_t>(s_.x, 31), out);
}

AArch64Cpu::Step AArch64Cpu::fail(uint8_t vector, uint64_t address) {
  fault_.vector = vector;
  fault_.address = address;
  fault_.pc = instruction_start_;
  s_.pc = instruction_start_;
  return Step::Fault;
}

// Top Byte Ignore: Linux runs user code with TCR_EL1.TBI0 set, so bits 63:56 of a user address (bit 55 clear) are not part of it. Programs keep pointer tags (MTE, HWASan, Java) there.
static uint64_t untag(uint64_t address) { return (address >> 55) & 1 ? address : address & 0x00FFFFFFFFFFFFFFull; }

bool AArch64Cpu::load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out) {
  address = untag(address);
  uint8_t raw[8] = {};
  if (!m.read(address, ByteView(raw, size))) {
    fail(14, address);
    return false;
  }
  uint64_t v = 0;
  for (uint32_t i = 0; i < size; ++i) v |= uint64_t{raw[i]} << (8 * i);
  *out = v;
  return true;
}

bool AArch64Cpu::store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value) {
  address = untag(address);
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  if (!m.write(address, ConstByteView(raw, size))) {
    fail(14, address);
    return false;
  }
  return true;
}

bool AArch64Cpu::load_vec(GuestMemory& m, uint64_t address, uint32_t size, Vec* out) {
  Vec v;
  if (size == 16) {
    if (!load(m, address, 8, &v.lo) || !load(m, address + 8, 8, &v.hi)) return false;
  } else if (!load(m, address, size, &v.lo)) {
    return false;
  }
  *out = v;
  return true;
}

bool AArch64Cpu::store_vec(GuestMemory& m, uint64_t address, uint32_t size, const Vec& v) {
  if (size == 16) {
    uint64_t probe;
    if (!load(m, address + 8, 8, &probe)) return false;      // (all or nothing)
    return store(m, address, 8, v.lo) && store(m, address + 8, 8, v.hi);
  }
  return store(m, address, size, v.lo);
}

UniquePointer<IGuestCpu> AArch64Cpu::clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const {
  UniquePointer<AArch64Cpu> copy = UniquePointer<AArch64Cpu>::make(allocator, *this);
  if (!copy) return UniquePointer<IGuestCpu>();
  copy->s_.x[0] = 0;
  if (stack_pointer) copy->s_.sp = stack_pointer;
  if (set_tls) copy->s_.tpidr_el0 = tls;
  copy->exclusive_valid_ = false;
  copy->retired_ = 0;
  return UniquePointer<IGuestCpu>(static_cast<UniquePointer<AArch64Cpu>&&>(copy));
}

// --- arithmetic helpers --------------------------------------------------------------------------------------------------------------------

uint64_t AArch64Cpu::add_with_carry(uint64_t a, uint64_t b, uint64_t carry, bool sf, bool set_flags) {
  const uint32_t size = sf ? 64 : 32;
  const uint64_t mask = ones(size), sign = uint64_t{1} << (size - 1);
  a &= mask;
  b &= mask;
  const unsigned __int128 sum = static_cast<unsigned __int128>(a) + b + carry;
  const uint64_t result = static_cast<uint64_t>(sum) & mask;
  if (set_flags) {
    uint32_t f = 0;
    if (result & sign) f |= kN;
    if (result == 0) f |= kZ;
    if ((sum >> size) != 0) f |= kC;
    if (((a ^ result) & (b ^ result) & sign) != 0) f |= kV;
    s_.nzcv = f;
  }
  return result;
}

void AArch64Cpu::set_nzcv_from(uint64_t result, bool sf) {
  const uint32_t size = sf ? 64 : 32;
  uint32_t f = 0;
  if ((result >> (size - 1)) & 1) f |= kN;
  if ((result & ones(size)) == 0) f |= kZ;
  s_.nzcv = f;
}

bool AArch64Cpu::condition(uint32_t cond) const {
  const bool n = s_.nzcv & kN, z = s_.nzcv & kZ, c = s_.nzcv & kC, v = s_.nzcv & kV;
  bool r = false;
  switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return true;      // AL and NV
  }
  return (cond & 1) ? !r : r;
}

uint64_t AArch64Cpu::shift_value(uint64_t v, uint32_t type, uint32_t amount, bool sf) const {
  const uint32_t size = sf ? 64 : 32;
  v &= ones(size);
  amount %= size;
  if (amount == 0) return v;
  switch (type) {
    case 0: return (v << amount) & ones(size);
    case 1: return v >> amount;
    case 2: return static_cast<uint64_t>(sext(v, size) >> amount) & ones(size);
    default: return rotate_right(v, amount, size);
  }
}

uint64_t AArch64Cpu::extend_value(uint64_t v, uint32_t option, uint32_t shift) const {
  uint64_t r;
  switch (option) {
    case 0: r = v & 0xFF; break;
    case 1: r = v & 0xFFFF; break;
    case 2: r = v & 0xFFFFFFFFu; break;
    case 3: r = v; break;
    case 4: r = static_cast<uint64_t>(sext(v, 8)); break;
    case 5: r = static_cast<uint64_t>(sext(v, 16)); break;
    case 6: r = static_cast<uint64_t>(sext(v, 32)); break;
    default: r = v; break;
  }
  return r << shift;
}

// --- data processing: immediate ------------------------------------------------------------------------------------------------------------

AArch64Cpu::Step AArch64Cpu::data_immediate(uint32_t insn) {
  const bool sf = insn >> 31;
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31;
  switch (bits_of(insn, 25, 23)) {
    case 0: case 1: {      // ADR, ADRP
      const uint64_t imm = (uint64_t{bits_of(insn, 23, 5)} << 2) | bits_of(insn, 30, 29);
      const int64_t offset = sext(imm, 21);
      if (insn >> 31) set_reg(rd, (s_.pc & ~uint64_t{0xFFF}) + (static_cast<uint64_t>(offset) << 12), true);
      else set_reg(rd, s_.pc + static_cast<uint64_t>(offset), true);
      return Step::Ok;
    }
    case 2: {      // add/subtract immediate
      const bool sub = (insn >> 30) & 1, set = (insn >> 29) & 1;
      uint64_t imm = bits_of(insn, 21, 10);
      if ((insn >> 22) & 1) imm <<= 12;
      const uint64_t a = reg_sp(rn, sf);
      const uint64_t r = sub ? add_with_carry(a, ~imm, 1, sf, set) : add_with_carry(a, imm, 0, sf, set);
      if (set) set_reg(rd, r, sf);
      else set_reg_sp(rd, r, sf);
      return Step::Ok;
    }
    case 3: {      // SMAX, UMAX, SMIN, UMIN with an 8-bit immediate (FEAT_CSSC)
      if (bits_of(insn, 30, 29) != 0 || !((insn >> 22) & 1)) return undefined();
      const uint32_t opc = bits_of(insn, 21, 18);
      if (opc > 3) return undefined();
      const bool is_signed = !(opc & 1), is_max = !(opc & 2);
      const uint32_t imm8 = bits_of(insn, 17, 10);
      const uint32_t size = sf ? 64 : 32;
      const uint64_t b = is_signed ? static_cast<uint64_t>(sext(imm8, 8)) & ones(size) : imm8;
      const uint64_t a = reg(rn, sf);
      const bool a_greater = is_signed ? sext(a, size) > sext(b, size) : a > b;
      set_reg(rd, a == b ? a : ((a_greater == is_max) ? a : b), sf);
      return Step::Ok;
    }
    case 4: {      // logical immediate
      const uint32_t opc = bits_of(insn, 30, 29);
      uint64_t wmask, tmask;
      if (!sf && ((insn >> 22) & 1)) return undefined();
      if (!decode_bitmasks((insn >> 22) & 1, bits_of(insn, 15, 10), bits_of(insn, 21, 16), true, sf ? 64 : 32, &wmask, &tmask)) return undefined();
      const uint64_t a = reg(rn, sf);
      uint64_t r = opc == 1 ? a | wmask : opc == 2 ? a ^ wmask : a & wmask;
      if (opc == 3) {
        set_nzcv_from(r, sf);
        set_reg(rd, r, sf);
      } else {
        set_reg_sp(rd, r, sf);
      }
      return Step::Ok;
    }
    case 5: {      // move wide
      const uint32_t opc = bits_of(insn, 30, 29), hw = bits_of(insn, 22, 21);
      if (!sf && hw > 1) return undefined();
      const uint64_t imm = uint64_t{bits_of(insn, 20, 5)} << (16 * hw);
      if (opc == 0) set_reg(rd, ~imm, sf);
      else if (opc == 2) set_reg(rd, imm, sf);
      else if (opc == 3) set_reg(rd, (reg(rd, sf) & ~(uint64_t{0xFFFF} << (16 * hw))) | imm, sf);
      else return undefined();
      return Step::Ok;
    }
    case 6: {      // bitfield
      const uint32_t opc = bits_of(insn, 30, 29), immr = bits_of(insn, 21, 16), imms = bits_of(insn, 15, 10);
      const bool n = (insn >> 22) & 1;
      if (opc == 3 || n != sf) return undefined();
      uint64_t wmask, tmask;
      const uint32_t size = sf ? 64 : 32;
      if (!decode_bitmasks(n, imms, immr, false, size, &wmask, &tmask)) return undefined();
      const uint64_t src = reg(rn, sf);
      uint64_t dst = opc == 1 ? reg(rd, sf) : 0;
      uint64_t bot = (dst & ~wmask) | (rotate_right(src, immr, size) & wmask);
      uint64_t result;
      if (opc == 1) {
        result = (dst & ~tmask) | (bot & tmask);
      } else if (opc == 0) {      // SBFM
        const uint64_t top = ((src >> imms) & 1) ? ones(size) : 0;
        bot = rotate_right(src, immr, size) & wmask;
        result = (top & ~tmask) | (bot & tmask);
      } else {      // UBFM
        bot = rotate_right(src, immr, size) & wmask;
        result = bot & tmask;
      }
      set_reg(rd, result, sf);
      return Step::Ok;
    }
    case 7: {      // extract
      const uint32_t imms = bits_of(insn, 15, 10), rm = bits_of(insn, 20, 16);
      if (bits_of(insn, 30, 29) != 0 || ((insn >> 22) & 1) != (insn >> 31) || (!sf && imms >= 32)) return undefined();
      const uint32_t size = sf ? 64 : 32;
      const uint64_t hi = reg(rn, sf), lo = reg(rm, sf);
      uint64_t r = imms == 0 ? lo : ((lo >> imms) | (hi << (size - imms))) & ones(size);
      set_reg(rd, r, sf);
      return Step::Ok;
    }
    default:
      return undefined();
  }
}

// --- data processing: register -------------------------------------------------------------------------------------------------------------

AArch64Cpu::Step AArch64Cpu::data_register(uint32_t insn) {
  const bool sf = insn >> 31;
  const uint32_t rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
  const uint32_t size = sf ? 64 : 32;

  if (bits_of(insn, 28, 24) == 0x0A) {      // logical, shifted register
    const uint32_t opc = bits_of(insn, 30, 29), shift = bits_of(insn, 23, 22), amount = bits_of(insn, 15, 10);
    const bool invert = (insn >> 21) & 1;
    if (!sf && amount >= 32) return undefined();
    uint64_t b = shift_value(reg(rm, sf), shift, amount, sf);
    if (invert) b = ~b & ones(size);
    const uint64_t a = reg(rn, sf);
    uint64_t r = opc == 1 ? a | b : opc == 2 ? a ^ b : a & b;
    if (opc == 3) set_nzcv_from(r, sf);
    set_reg(rd, r, sf);
    return Step::Ok;
  }
  if (bits_of(insn, 28, 24) == 0x0B) {      // add/subtract, shifted or extended register
    const bool sub = (insn >> 30) & 1, set = (insn >> 29) & 1;
    uint64_t b;
    uint64_t a;
    if ((insn >> 21) & 1) {      // extended
      if (bits_of(insn, 23, 22) != 0) return undefined();
      const uint32_t amount = bits_of(insn, 12, 10);
      if (amount > 4) return undefined();
      b = extend_value(reg(rm, true), bits_of(insn, 15, 13), amount);
      a = reg_sp(rn, sf);
    } else {
      const uint32_t shift = bits_of(insn, 23, 22), amount = bits_of(insn, 15, 10);
      if (shift == 3 || (!sf && amount >= 32)) return undefined();
      b = shift_value(reg(rm, sf), shift, amount, sf);
      a = reg(rn, sf);
    }
    const uint64_t r = sub ? add_with_carry(a, ~b, 1, sf, set) : add_with_carry(a, b, 0, sf, set);
    if (set || !((insn >> 21) & 1)) set_reg(rd, r, sf);
    else set_reg_sp(rd, r, sf);
    return Step::Ok;
  }
  switch (bits_of(insn, 28, 21)) {
    case 0xD0: {      // add/subtract with carry
      if (bits_of(insn, 15, 10) != 0) return undefined();
      const bool sub = (insn >> 30) & 1, set = (insn >> 29) & 1;
      const uint64_t carry = (s_.nzcv & kC) ? 1 : 0;
      const uint64_t b = sub ? ~reg(rm, sf) : reg(rm, sf);
      set_reg(rd, add_with_carry(reg(rn, sf), b, carry, sf, set), sf);
      return Step::Ok;
    }
    case 0xD2: {      // conditional compare
      if (((insn >> 29) & 1) == 0 || (insn & 0x10) || (insn & 0x400)) return undefined();
      const bool negative_op = !((insn >> 30) & 1);      // (op = 1 is CCMP, which subtracts; 0 is CCMN, which adds)
      const uint32_t cond = bits_of(insn, 15, 12), nzcv = insn & 15;
      if (condition(cond)) {
        const uint64_t b = (insn >> 11) & 1 ? uint64_t{rm} : reg(rm, sf);
        const uint64_t a = reg(rn, sf);
        if (negative_op) add_with_carry(a, b, 0, sf, true);
        else add_with_carry(a, ~b, 1, sf, true);
      } else {
        s_.nzcv = nzcv << 28;
      }
      return Step::Ok;
    }
    case 0xD4: {      // conditional select
      if (((insn >> 29) & 1) || (insn & 0x800)) return undefined();
      const bool op = (insn >> 30) & 1, o2 = (insn >> 10) & 1;
      uint64_t v;
      if (condition(bits_of(insn, 15, 12))) {
        v = reg(rn, sf);
      } else {
        v = reg(rm, sf);
        if (op) v = ~v;
        if (o2) v += 1;
        if (!sf) v &= 0xFFFFFFFFu;
      }
      set_reg(rd, v, sf);
      return Step::Ok;
    }
    case 0xD6: {      // data processing, 1 or 2 sources
      if ((insn >> 29) & 1) return undefined();
      const uint32_t opcode = bits_of(insn, 15, 10);
      if ((insn >> 30) & 1) {      // 1 source
        if (rm != 0) return undefined();
        const uint64_t v = reg(rn, sf);
        uint64_t r = 0;
        switch (opcode) {
          case 0:      // RBIT
            for (uint32_t i = 0; i < size; ++i) r |= ((v >> i) & 1) << (size - 1 - i);
            break;
          case 1:      // REV16
            for (uint32_t i = 0; i < size; i += 16) r |= (((v >> i) & 0xFF) << (i + 8)) | (((v >> (i + 8)) & 0xFF) << i);
            break;
          case 2:      // REV32 (64-bit) or REV (32-bit)
            for (uint32_t i = 0; i < size; i += 32)
              for (uint32_t b = 0; b < 4; ++b) r |= ((v >> (i + 8 * b)) & 0xFF) << (i + 8 * (3 - b));
            break;
          case 3:      // REV (64-bit)
            if (!sf) return undefined();
            for (uint32_t b = 0; b < 8; ++b) r |= ((v >> (8 * b)) & 0xFF) << (8 * (7 - b));
            break;
          case 4: r = count_leading_zeros(v, size); break;
          case 6: {      // CTZ (FEAT_CSSC)
            uint32_t n = 0;
            while (n < size && !((v >> n) & 1)) ++n;
            r = n;
            break;
          }
          case 7: r = static_cast<uint64_t>(__builtin_popcountll(v)); break;      // CNT
          case 8: {      // ABS
            const int64_t signed_v = sext(v, size);
            r = static_cast<uint64_t>(signed_v < 0 ? -signed_v : signed_v) & ones(size);
            break;
          }
          case 5: {      // CLS
            const bool top = (v >> (size - 1)) & 1;
            uint32_t n = 0;
            for (int i = static_cast<int>(size) - 2; i >= 0 && (((v >> i) & 1) == (top ? 1u : 0u)); --i) ++n;
            r = n;
            break;
          }
          default: return undefined();
        }
        set_reg(rd, r, sf);
        return Step::Ok;
      }
      const uint64_t a = reg(rn, sf), b = reg(rm, sf);
      uint64_t r = 0;
      switch (opcode) {
        case 2:      // UDIV
          r = b == 0 ? 0 : a / b;
          break;
        case 3: {    // SDIV
          if (b == 0) { r = 0; break; }
          const int64_t sa = sext(a, size), sb = sext(b, size);
          const bool neg = (sa < 0) != (sb < 0);
          const uint64_t ma = sa < 0 ? ~static_cast<uint64_t>(sa) + 1 : static_cast<uint64_t>(sa);
          const uint64_t mb = sb < 0 ? ~static_cast<uint64_t>(sb) + 1 : static_cast<uint64_t>(sb);
          const uint64_t q = ma / mb;
          r = neg ? ~q + 1 : q;
          break;
        }
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: {      // CRC32B/H/W/X and CRC32CB/H/W/X
          const uint32_t width = 8u << (opcode & 3);
          if ((width == 64) != sf) return undefined();
          const uint32_t polynomial = (opcode & 4) ? 0x82F63B78u : 0xEDB88320u;
          uint32_t crc = static_cast<uint32_t>(a);
          uint64_t data = b & ones(width);
          for (uint32_t i = 0; i < width; ++i) {
            crc = (crc >> 1) ^ (((crc ^ static_cast<uint32_t>(data)) & 1) ? polynomial : 0);
            data >>= 1;
          }
          r = crc;
          set_reg(rd, r, false);
          return Step::Ok;
        }
        case 24: case 25: case 26: case 27: {      // SMAX, UMAX, SMIN, UMIN (FEAT_CSSC)
          const bool is_signed = !(opcode & 1), is_max = !(opcode & 2);
          const bool a_greater = is_signed ? sext(a, size) > sext(b, size) : a > b;
          r = (a_greater == is_max) ? a : b;
          if (a == b) r = a;
          break;
        }
        case 8: r = shift_value(a, 0, static_cast<uint32_t>(b) % size, sf); break;
        case 9: r = shift_value(a, 1, static_cast<uint32_t>(b) % size, sf); break;
        case 10: r = shift_value(a, 2, static_cast<uint32_t>(b) % size, sf); break;
        case 11: r = shift_value(a, 3, static_cast<uint32_t>(b) % size, sf); break;
        default: return undefined();
      }
      set_reg(rd, r, sf);
      return Step::Ok;
    }
    default:
      break;
  }
  if (bits_of(insn, 28, 24) == 0x1B) {      // data processing, 3 sources
    const uint32_t op31 = bits_of(insn, 23, 21), ra = bits_of(insn, 14, 10);
    const bool o0 = (insn >> 15) & 1;
    if (bits_of(insn, 30, 29) != 0) return undefined();
    const uint64_t a = reg(ra, sf);
    if (op31 == 0) {      // MADD, MSUB
      const uint64_t p = reg(rn, sf) * reg(rm, sf);
      set_reg(rd, o0 ? a - p : a + p, sf);
      return Step::Ok;
    }
    if (!sf) return undefined();
    switch (op31) {
      case 1: {      // SMADDL, SMSUBL
        const int64_t p = sext(reg(rn, true), 32) * sext(reg(rm, true), 32);
        set_reg(rd, o0 ? reg(ra, true) - static_cast<uint64_t>(p) : reg(ra, true) + static_cast<uint64_t>(p), true);
        return Step::Ok;
      }
      case 5: {      // UMADDL, UMSUBL
        const uint64_t p = (reg(rn, true) & 0xFFFFFFFFu) * (reg(rm, true) & 0xFFFFFFFFu);
        set_reg(rd, o0 ? reg(ra, true) - p : reg(ra, true) + p, true);
        return Step::Ok;
      }
      case 2: case 6: {      // SMULH, UMULH
        if (o0) return undefined();      // (Ra is should-be-one; hardware and qemu ignore it)
        uint64_t hi, lo;
        const uint64_t x = reg(rn, true), y = reg(rm, true);
        multiply_unsigned(x, y, &hi, &lo);
        if (op31 == 2) {
          if (static_cast<int64_t>(x) < 0) hi -= y;
          if (static_cast<int64_t>(y) < 0) hi -= x;
        }
        set_reg(rd, hi, true);
        return Step::Ok;
      }
      default: return undefined();
    }
  }
  return undefined();
}

// --- branches, exceptions and system instructions ------------------------------------------------------------------------------------------

AArch64Cpu::Step AArch64Cpu::system_instruction(uint32_t insn) {
  if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) return Step::Ok;      // hints: NOP, YIELD, WFE, SEV, PACIASP and friends (no pointer authentication here)
  if ((insn & 0xFFFFF0FFu) == 0xD503309Fu || (insn & 0xFFFFF0FFu) == 0xD50330BFu || (insn & 0xFFFFF0FFu) == 0xD50330DFu) return Step::Ok;      // DSB, DMB, ISB
  if ((insn & 0xFFFFF0FFu) == 0xD503305Fu) {      // CLREX
    exclusive_valid_ = false;
    return Step::Ok;
  }
  const bool read = (insn & 0xFFF00000u) == 0xD5300000u, write = (insn & 0xFFF00000u) == 0xD5100000u;
  if (!read && !write) {
    if ((insn & 0xFFF8F01Fu) == 0xD500401Fu) {      // MSR immediate (DAIFSet etc.): only SPSel/DAIF forms exist; ignore
      return Step::Ok;
    }
    return undefined();
  }
  const uint32_t rt = insn & 31;
  // (op0 is bits 20:19, op1 18:16, CRn 15:12, CRm 11:8, op2 7:5)
  auto make = [](uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) { return (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2; };
  const uint32_t k = make(bits_of(insn, 20, 19), bits_of(insn, 18, 16), bits_of(insn, 15, 12), bits_of(insn, 11, 8), bits_of(insn, 7, 5));
  uint64_t value = 0;
  if (k == make(3, 3, 13, 0, 2)) {      // TPIDR_EL0
    if (read) set_reg(rt, s_.tpidr_el0, true);
    else s_.tpidr_el0 = reg(rt, true);
  } else if (k == make(3, 3, 4, 2, 0)) {      // NZCV
    if (read) set_reg(rt, s_.nzcv, true);
    else s_.nzcv = static_cast<uint32_t>(reg(rt, true)) & 0xF0000000u;
  } else if (k == make(3, 3, 4, 4, 0)) {      // FPCR
    if (read) set_reg(rt, s_.fpcr, true);
    else s_.fpcr = static_cast<uint32_t>(reg(rt, true)) & 0x07C09F00u;
  } else if (k == make(3, 3, 4, 4, 1)) {      // FPSR
    if (read) set_reg(rt, s_.fpsr, true);
    else s_.fpsr = static_cast<uint32_t>(reg(rt, true)) & 0xF800009Fu;
  } else if (read) {
    if (k == make(3, 3, 14, 0, 2) || k == make(3, 3, 14, 0, 1)) value = retired_;      // CNTVCT_EL0 / CNTPCT_EL0: the instruction count is the clock
    else if (k == make(3, 3, 14, 0, 0)) value = 1000000000;      // CNTFRQ_EL0
    else if (k == make(3, 3, 0, 0, 7)) value = 0x10;      // DCZID_EL0: DC ZVA prohibited
    else if (k == make(3, 3, 0, 0, 1)) value = 0x8444C004;      // CTR_EL0
    else if (k == make(3, 0, 0, 0, 0)) value = 0x410FD070;      // MIDR_EL1 (an Arm implementer, a generic part)
    else if (k == make(3, 0, 0, 0, 5)) value = 0;      // MPIDR_EL1
    else if (bits_of(insn, 20, 19) == 3 && bits_of(insn, 18, 16) == 0 && bits_of(insn, 15, 12) == 0) value = 0;      // the ID registers (ID_AA64*): no optional features reported
    else return undefined();
    set_reg(rt, value, true);
  } else {
    return undefined();
  }
  return Step::Ok;
}

AArch64Cpu::Step AArch64Cpu::branch_system(GuestMemory& m, uint32_t insn) {
  (void)m;
  uint64_t next = s_.pc + 4;
  if ((insn & 0x7C000000u) == 0x14000000u) {      // B, BL
    if (insn >> 31) s_.x[30] = s_.pc + 4;
    next = s_.pc + static_cast<uint64_t>(sext(insn & 0x3FFFFFF, 26) * 4);
    s_.pc = next;
    return Step::Ok;
  }
  if ((insn & 0xFF000010u) == 0x54000000u) {      // B.cond
    if (condition(insn & 15)) next = s_.pc + static_cast<uint64_t>(sext(bits_of(insn, 23, 5), 19) * 4);
    s_.pc = next;
    return Step::Ok;
  }
  if ((insn & 0x7E000000u) == 0x34000000u) {      // CBZ, CBNZ
    const bool sf = insn >> 31, nonzero = (insn >> 24) & 1;
    const bool zero = reg(insn & 31, sf) == 0;
    if (zero != nonzero) next = s_.pc + static_cast<uint64_t>(sext(bits_of(insn, 23, 5), 19) * 4);
    s_.pc = next;
    return Step::Ok;
  }
  if ((insn & 0x7E000000u) == 0x36000000u) {      // TBZ, TBNZ
    const uint32_t bit = ((insn >> 31) << 5) | bits_of(insn, 23, 19);
    const bool set = (reg(insn & 31, true) >> bit) & 1, nonzero = (insn >> 24) & 1;
    if (set == nonzero) next = s_.pc + static_cast<uint64_t>(sext(bits_of(insn, 18, 5), 14) * 4);
    s_.pc = next;
    return Step::Ok;
  }
  if ((insn & 0xFE1FFC1Fu) == 0xD61F0000u || (insn & 0xFFFFFC1Fu) == 0xD65F0000u || (insn & 0xFFFFFBFFu) == 0xD65F0BFFu) {      // BR, BLR, RET (and RETAA/RETAB)
    const uint32_t opc = bits_of(insn, 24, 21);
    if (opc > 2) return undefined();
    if ((insn & 0xFFFFFBFFu) == 0xD65F0BFFu) {
      s_.pc = s_.x[30];
      return Step::Ok;
    }
    const uint64_t target = reg((insn >> 5) & 31, true);
    if (opc == 1) s_.x[30] = s_.pc + 4;
    s_.pc = target;
    return Step::Ok;
  }
  if ((insn & 0xFFE0001Fu) == 0xD4000001u) {      // SVC
    s_.pc += 4;
    return Step::Syscall;
  }
  if ((insn & 0xFFE0001Fu) == 0xD4200000u) return fail(3);      // BRK
  if ((insn & 0xFFE0001Fu) == 0xD4400000u) return fail(13);      // HLT
  if ((insn & 0xFFC00000u) == 0xD5000000u) {
    const Step r = system_instruction(insn);
    if (r == Step::Ok) s_.pc += 4;
    return r;
  }
  return undefined();
}

// --- loads and stores ----------------------------------------------------------------------------------------------------------------------

AArch64Cpu::Step AArch64Cpu::load_store_exclusive(GuestMemory& m, uint32_t insn) {
  const uint32_t size = 1u << (insn >> 30), rt = insn & 31, rn = (insn >> 5) & 31, rs = (insn >> 16) & 31;
  const bool o2 = (insn >> 23) & 1, load_bit = (insn >> 22) & 1, o1 = (insn >> 21) & 1;
  const uint64_t address = reg_sp(rn, true);
  if (o1 && !o2) {      // exclusive pairs (bit 31 set) and CASP (bit 31 clear)
    const bool o0 = (insn >> 15) & 1;
    const uint32_t rt2 = bits_of(insn, 14, 10);
    if (!(insn >> 31)) {      // CASP, CASPA, CASPAL, CASPL: Xs:Xs+1 compared with memory, Xt:Xt+1 stored, the old value to Xs:Xs+1
      const bool wide_pair = (insn >> 30) & 1;
      if ((rs & 1) || (rt & 1) || rt2 != 31) return undefined();
      const uint32_t half = wide_pair ? 8 : 4;
      uint64_t old_low, old_high;
      if (!load(m, address, half, &old_low) || !load(m, address + half, half, &old_high)) return Step::Fault;
      const uint64_t mask = ones(half * 8);
      if (old_low == (reg(rs, true) & mask) && old_high == (reg(rs + 1, true) & mask)) {
        if (!store(m, address, half, reg(rt, true)) || !store(m, address + half, half, reg(rt + 1, true))) return Step::Fault;
      }
      set_reg(rs, old_low, wide_pair);
      set_reg(rs + 1, old_high, wide_pair);
      return Step::Ok;
    }
    const uint32_t half = (insn >> 30) & 1 ? 8 : 4;
    if (load_bit) {      // LDXP, LDAXP
      uint64_t first, second;
      if (!load(m, address, half, &first) || !load(m, address + half, half, &second)) return Step::Fault;
      set_reg(rt, first, half == 8);
      set_reg(rt2, second, half == 8);
      exclusive_valid_ = true;
      exclusive_address_ = address;
    } else {      // STXP, STLXP
      if (exclusive_valid_ && exclusive_address_ == address) {
        if (!store(m, address, half, reg(rt, true)) || !store(m, address + half, half, reg(rt2, true))) return Step::Fault;
        set_reg(rs, 0, false);
      } else {
        set_reg(rs, 1, false);
      }
      exclusive_valid_ = false;
    }
    (void)o0;
    return Step::Ok;
  }
  if (o2 && o1) {      // CAS, CASA, CASAL, CASL
    if (bits_of(insn, 14, 10) != 31) return undefined();
    uint64_t old;
    if (!load(m, address, size, &old)) return Step::Fault;
    const uint64_t mask = ones(size * 8);
    if (old == (reg(rs, true) & mask) && !store(m, address, size, reg(rt, true) & mask)) return Step::Fault;
    set_reg(rs, old, size == 8);
    return Step::Ok;
  }
  uint64_t v;
  if (o2) {      // LDAR, STLR (and the LORegion forms)
    if (load_bit) {
      if (!load(m, address, size, &v)) return Step::Fault;
      set_reg(rt, v, true);
    } else if (!store(m, address, size, reg(rt, true))) {
      return Step::Fault;
    }
    return Step::Ok;
  }
  if (load_bit) {      // LDXR, LDAXR
    if (!load(m, address, size, &v)) return Step::Fault;
    set_reg(rt, v, true);
    exclusive_valid_ = true;
    exclusive_address_ = address;
  } else {      // STXR, STLXR: succeeds if this thread still holds the reservation
    if (exclusive_valid_ && exclusive_address_ == address) {
      if (!store(m, address, size, reg(rt, true))) return Step::Fault;
      set_reg(rs, 0, false);
    } else {
      set_reg(rs, 1, false);
    }
    exclusive_valid_ = false;
  }
  return Step::Ok;
}

AArch64Cpu::Step AArch64Cpu::atomic_memory(GuestMemory& m, uint32_t insn) {
  const uint32_t size = 1u << (insn >> 30), rt = insn & 31, rn = (insn >> 5) & 31, rs = (insn >> 16) & 31;
  const uint32_t opc = bits_of(insn, 14, 12);
  const bool o3 = (insn >> 15) & 1;
  const uint64_t address = reg_sp(rn, true);
  const uint64_t mask = ones(size * 8);
  uint64_t old;
  if (!load(m, address, size, &old)) return Step::Fault;
  if (o3 && opc == 4 && rs == 31) {      // LDAPR
    set_reg(rt, old, true);
    return Step::Ok;
  }
  const uint64_t operand = reg(rs, true) & mask;
  uint64_t result;
  if (o3) {
    if (opc != 0) return undefined();
    result = operand;      // SWP
  } else {
    switch (opc) {
      case 0: result = old + operand; break;
      case 1: result = old & ~operand; break;
      case 2: result = old ^ operand; break;
      case 3: result = old | operand; break;
      case 4: result = sext(old, size * 8) > sext(operand, size * 8) ? old : operand; break;
      case 5: result = sext(old, size * 8) < sext(operand, size * 8) ? old : operand; break;
      case 6: result = old > operand ? old : operand; break;
      default: result = old < operand ? old : operand; break;
    }
  }
  if (!store(m, address, size, result & mask)) return Step::Fault;
  set_reg(rt, old, size == 8);
  return Step::Ok;
}

AArch64Cpu::Step AArch64Cpu::load_store_pair(GuestMemory& m, uint32_t insn) {
  const uint32_t opc = insn >> 30, rt = insn & 31, rn = (insn >> 5) & 31, rt2 = (insn >> 10) & 31;
  const bool vector = (insn >> 26) & 1, load_bit = (insn >> 22) & 1;
  const uint32_t mode = bits_of(insn, 24, 23);      // 0 no-allocate, 1 post, 2 offset, 3 pre
  uint32_t size;
  if (vector) {
    if (opc == 3) return undefined();
    size = 4u << opc;
  } else {
    if (opc == 3) return undefined();
    size = opc == 2 ? 8 : 4;
  }
  const int64_t offset = sext(bits_of(insn, 21, 15), 7) * static_cast<int64_t>(size);
  uint64_t address = reg_sp(rn, true);
  const uint64_t access = mode == 1 ? address : address + static_cast<uint64_t>(offset);
  if (load_bit && rt == rt2 && !vector) return undefined();
  if (load_bit) {
    if (vector) {
      Vec a, b;
      if (!load_vec(m, access, size, &a) || !load_vec(m, access + size, size, &b)) return Step::Fault;
      s_.v[rt] = a;
      s_.v[rt2] = b;
      if (size < 16) { s_.v[rt].hi = 0; s_.v[rt2].hi = 0; }
      if (size < 8) { s_.v[rt].lo &= 0xFFFFFFFFu; s_.v[rt2].lo &= 0xFFFFFFFFu; }
    } else {
      uint64_t a, b;
      if (!load(m, access, size, &a) || !load(m, access + size, size, &b)) return Step::Fault;
      if (opc == 1) {      // LDPSW
        a = static_cast<uint64_t>(sext(a, 32));
        b = static_cast<uint64_t>(sext(b, 32));
        set_reg(rt, a, true);
        set_reg(rt2, b, true);
      } else {
        set_reg(rt, a, size == 8);
        set_reg(rt2, b, size == 8);
      }
    }
  } else if (vector) {
    if (!store_vec(m, access, size, s_.v[rt]) || !store_vec(m, access + size, size, s_.v[rt2])) return Step::Fault;
  } else if (!store(m, access, size, reg(rt, true)) || !store(m, access + size, size, reg(rt2, true))) {
    return Step::Fault;
  }
  if (mode == 1 || mode == 3) set_reg_sp(rn, mode == 1 ? address + static_cast<uint64_t>(offset) : access, true);
  return Step::Ok;
}

AArch64Cpu::Step AArch64Cpu::load_store_register(GuestMemory& m, uint32_t insn) {
  const uint32_t size = insn >> 30, rt = insn & 31, rn = (insn >> 5) & 31, opc = bits_of(insn, 23, 22);
  const bool vector = (insn >> 26) & 1;
  uint64_t address;
  uint64_t writeback = 0;
  bool do_writeback = false;
  uint32_t scale = size;
  if (vector && (opc & 2) && size == 0) scale = 4;      // 128-bit
  if (bits_of(insn, 25, 24) == 1) {      // unsigned offset
    address = reg_sp(rn, true) + (uint64_t{bits_of(insn, 21, 10)} << scale);
  } else if (!((insn >> 21) & 1)) {      // 9-bit immediate forms
    const int64_t imm = sext(bits_of(insn, 20, 12), 9);
    const uint32_t mode = bits_of(insn, 11, 10);
    const uint64_t base = reg_sp(rn, true);
    if (mode == 1) { address = base; writeback = base + static_cast<uint64_t>(imm); do_writeback = true; }
    else if (mode == 3) { address = base + static_cast<uint64_t>(imm); writeback = address; do_writeback = true; }
    else address = base + static_cast<uint64_t>(imm);      // LDUR / STUR / LDTR / STTR
  } else {      // register offset
    if (bits_of(insn, 11, 10) != 2) return undefined();
    const uint32_t option = bits_of(insn, 15, 13);
    if (!(option & 2)) return undefined();
    const uint64_t index = extend_value(reg(bits_of(insn, 20, 16), true), option, 0);
    address = reg_sp(rn, true) + (((insn >> 12) & 1) ? index << scale : index);
  }
  if (vector) {
    const uint32_t bytes = 1u << scale;
    if (scale > 4 || (opc & 2 ? !(size == 0) : false)) return undefined();
    if (opc & 1) {      // load
      Vec v;
      if (!load_vec(m, address, bytes, &v)) return Step::Fault;
      if (bytes < 16) v.hi = 0;
      if (bytes < 8) v.lo &= ones(bytes * 8);
      s_.v[rt] = v;
    } else if (!store_vec(m, address, bytes, s_.v[rt])) {
      return Step::Fault;
    }
  } else {
    const uint32_t bytes = 1u << size;
    if (opc == 0) {
      if (!store(m, address, bytes, reg(rt, true))) return Step::Fault;
    } else if (opc == 1) {
      uint64_t v;
      if (!load(m, address, bytes, &v)) return Step::Fault;
      set_reg(rt, v, true);
    } else {      // sign-extending loads (opc 2: into X, 3: into W); size 3 is PRFM
      if (size == 3) return Step::Ok;
      if (size == 2 && opc == 3) return undefined();
      uint64_t v;
      if (!load(m, address, bytes, &v)) return Step::Fault;
      const uint64_t extended = static_cast<uint64_t>(sext(v, bytes * 8));
      set_reg(rt, extended, opc == 2);
    }
  }
  if (do_writeback) set_reg_sp(rn, writeback, true);
  return Step::Ok;
}

// LDAPUR / STLUR (FEAT_LRCPC2: an unscaled signed 9-bit offset, release / acquire ordering that makes no difference here) and the FEAT_LRCPC3 STLR pre-index / LDAPR post-index forms.
AArch64Cpu::Step AArch64Cpu::load_store_ordered(GuestMemory& m, uint32_t insn) {
  const uint32_t size = insn >> 30, rt = insn & 31, rn = (insn >> 5) & 31, opc = bits_of(insn, 23, 22), mode = bits_of(insn, 11, 10);
  const bool vector = (insn >> 26) & 1;
  const uint64_t base = reg_sp(rn, true);
  uint32_t bytes = vector ? (opc >= 2 ? 16u : 1u << size) : 1u << size;
  if (vector && opc >= 2 && size != 0) return undefined();
  uint64_t address = base;
  bool writeback = false;
  uint64_t new_base = 0;
  if (mode == (vector ? 2u : 0u)) {      // (the SIMD&FP forms have 10 in bits 11:10)
    address = base + static_cast<uint64_t>(sext(bits_of(insn, 20, 12), 9));
  } else if (mode == 2 && !vector) {      // LRCPC3: STLR with pre-decrement (opc 2), LDAPR with post-increment (opc 3); the immediate field is zero
    if (bits_of(insn, 20, 12) != 0 || (opc != 2 && opc != 3) || vector || size < 2) return undefined();
    writeback = true;
    if (opc == 2) { address = base - bytes; new_base = address; }
    else new_base = base + bytes;
  } else {
    return undefined();
  }
  if (vector) {
    const bool is_load = opc & 1;
    if (is_load) {
      Vec v;
      if (!load_vec(m, address, bytes, &v)) return Step::Fault;
      if (bytes < 16) v.hi = 0;
      if (bytes < 8) v.lo &= ones(bytes * 8);
      s_.v[rt] = v;
    } else if (!store_vec(m, address, bytes, s_.v[rt])) {
      return Step::Fault;
    }
    return Step::Ok;
  }
  if (mode == 2 ? opc == 2 : opc == 0) {      // store
    if (!store(m, address, bytes, reg(rt, true))) return Step::Fault;
  } else {
    uint64_t v;
    if (!load(m, address, bytes, &v)) return Step::Fault;
    bool sign = false, wide_dest = true;
    if (mode == 0 && opc >= 2) {
      if (size == 3 || (size == 2 && opc == 3)) return undefined();
      sign = true;
      wide_dest = opc == 2;
    } else {
      wide_dest = size == 3;
    }
    if (sign) v = static_cast<uint64_t>(sext(v, bytes * 8));
    set_reg(rt, v, wide_dest);
  }
  if (writeback) set_reg_sp(rn, new_base, true);
  return Step::Ok;
}

AArch64Cpu::Step AArch64Cpu::load_store(GuestMemory& m, uint32_t insn) {
  const bool vector = (insn >> 26) & 1;
  switch (bits_of(insn, 29, 27)) {
    case 1:      // exclusives (V = 0) and the Advanced SIMD structure loads and stores (V = 1)
      if (!vector) return load_store_exclusive(m, insn);
      return ((insn >> 24) & 1) ? simd_load_store_single(m, insn) : simd_load_store_multiple(m, insn);
    case 3: {    // load register (literal), and the LRCPC2 / LRCPC3 unscaled and writeback forms (bits 25:24 = 01)
      if (bits_of(insn, 25, 24) == 1 && !((insn >> 21) & 1)) return load_store_ordered(m, insn);
      if (bits_of(insn, 25, 24) != 0) return undefined();
      const uint32_t opc = insn >> 30, rt = insn & 31;
      const uint64_t address = s_.pc + static_cast<uint64_t>(sext(bits_of(insn, 23, 5), 19) * 4);
      if (vector) {
        if (opc == 3) return undefined();
        const uint32_t bytes = 4u << opc;
        Vec v;
        if (!load_vec(m, address, bytes, &v)) return Step::Fault;
        if (bytes < 16) v.hi = 0;
        s_.v[rt] = v;
        return Step::Ok;
      }
      if (opc == 3) return Step::Ok;      // PRFM
      uint64_t v;
      if (!load(m, address, opc == 1 ? 8 : 4, &v)) return Step::Fault;
      set_reg(rt, opc == 2 ? static_cast<uint64_t>(sext(v, 32)) : v, opc != 0);
      return Step::Ok;
    }
    case 5:
      return load_store_pair(m, insn);
    case 7:
      if (bits_of(insn, 25, 24) == 0 && !vector && ((insn >> 21) & 1) && bits_of(insn, 11, 10) == 0) return atomic_memory(m, insn);
      if (bits_of(insn, 25, 24) > 1) return undefined();
      return load_store_register(m, insn);
    default:
      return undefined();
  }
}

// --- the fetch/decode loop -----------------------------------------------------------------------------------------------------------------

AArch64Cpu::Step AArch64Cpu::step(GuestMemory& m) {
  instruction_start_ = s_.pc;
  if (s_.pc & 3) return fail(13, s_.pc);
  uint8_t raw[4];
  if (!m.fetch(s_.pc, ByteView(raw, 4))) return fail(14, s_.pc);
  const uint32_t insn = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
  Step result;
  switch (bits_of(insn, 28, 25)) {
    case 8: case 9:      // 100x: data processing, immediate
      result = data_immediate(insn);
      break;
    case 10: case 11:    // 101x: branches, exceptions, system
      result = branch_system(m, insn);
      return result;      // (a branch sets the new pc itself)
    case 4: case 6: case 12: case 14:      // x1x0: loads and stores
      result = load_store(m, insn);
      break;
    case 5: case 13:     // x101: data processing, register
      result = data_register(insn);
      break;
    case 7: case 15:     // x111: scalar floating point and Advanced SIMD
      result = (((insn >> 24) & 0x1E) == 0x1E && !((insn >> 29) & 1) && !((insn >> 30) & 1)) ? floating_point(insn) : simd(insn);
      break;
    default:
      return undefined();
  }
  if (result == Step::Ok) s_.pc += 4;
  return result;
}

StopReason AArch64Cpu::run(GuestMemory& memory, uint64_t max_instructions) {
  for (uint64_t i = 0; i < max_instructions; ++i) {
    const Step r = step(memory);
    if (r == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (r == Step::Syscall) return StopReason::Syscall;
  }
  // The slice is over. Threads take turns, so another one could write the word a pending LDXR read and the STXR would still succeed (real hardware loses the reservation
  // when another observer stores to it). A turn therefore never ends inside the usual few-instruction LDXR..STXR loop; a reservation still open after a generous
  // extra allowance is dropped, which the architecture allows at any time (the STXR then fails and the program retries).
  for (uint32_t extra = 0; exclusive_valid_ && extra < 64; ++extra) {
    const Step r = step(memory);
    if (r == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (r == Step::Syscall) return StopReason::Syscall;
  }
  exclusive_valid_ = false;
  return StopReason::Budget;
}

}  // namespace emulation
