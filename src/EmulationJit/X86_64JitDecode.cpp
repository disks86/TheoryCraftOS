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
#include "EmulationJit/X86_64JitDecode.hpp"

namespace emulation {

namespace {

// Reads the instruction's bytes in order; running out of bytes marks it truncated (the caller then refuses the instruction).
class Fetch {
 public:
  explicit Fetch(ConstByteView bytes) : bytes_(bytes) {}
  uint8_t u8() {
    uint8_t v = 0;
    if (!bytes_.try_get(position_, &v)) {
      truncated_ = true;
      return 0;
    }
    ++position_;
    return v;
  }
  // A little-endian immediate of `count` bytes, sign-extended when asked.
  int64_t imm(uint32_t count, bool sign_extend) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < count; ++i) v |= uint64_t{u8()} << (8 * i);
    if (sign_extend && count < 8) {
      const uint64_t sign = uint64_t{1} << (count * 8 - 1);
      v = (v ^ sign) - sign;
    }
    return static_cast<int64_t>(v);
  }
  uint32_t position() const { return position_; }
  bool truncated() const { return truncated_; }

 private:
  ConstByteView bytes_;
  uint32_t position_ = 0;
  bool truncated_ = false;
};

struct Prefixes {
  bool operand16 = false, rep = false, repne = false;
  bool rex = false, w = false, r = false, x = false, b = false;
  uint8_t segment = 0;
};

// ModRM, SIB and displacement, exactly as the interpreter reads them.
bool decode_modrm(Fetch& f, const Prefixes& p, X86Insn* out, uint8_t* modrm_reg_field) {
  const uint8_t modrm = f.u8();
  const uint8_t mod = modrm >> 6;
  *modrm_reg_field = (modrm >> 3) & 7;
  out->reg = static_cast<uint8_t>(*modrm_reg_field | (p.r ? 8 : 0));
  const uint8_t rm = modrm & 7;
  if (mod == 3) {
    out->mem = false;
    out->rm = static_cast<uint8_t>(rm | (p.b ? 8 : 0));
    return !f.truncated();
  }
  out->mem = true;
  bool have_base = true;
  if (rm == 4) {
    const uint8_t sib = f.u8();
    out->scale = sib >> 6;
    const uint8_t index = static_cast<uint8_t>(((sib >> 3) & 7) | (p.x ? 8 : 0));
    if ((sib & 7) == 5 && mod == 0) have_base = false;
    else out->base = static_cast<uint8_t>((sib & 7) | (p.b ? 8 : 0));
    if (index != 4) out->index = index;
  } else if (rm == 5 && mod == 0) {
    have_base = false;
    out->rip_relative = true;
  } else {
    out->base = static_cast<uint8_t>(rm | (p.b ? 8 : 0));
  }
  if (mod == 1) out->disp = f.imm(1, true);
  else if (mod == 2 || !have_base) out->disp = f.imm(4, true);
  return !f.truncated();
}

}  // namespace

bool decode_x86(ConstByteView bytes, X86Insn* out) {
  *out = X86Insn();
  Fetch f(bytes);
  Prefixes p;
  uint8_t op = 0;
  bool prefix_seen_other = false;
  for (uint32_t i = 0; i < 14; ++i) {
    op = f.u8();
    bool is_prefix = true;
    switch (op) {
      case 0x66: p.operand16 = true; break;
      case 0xF2: p.repne = true; p.rep = false; break;
      case 0xF3: p.rep = true; p.repne = false; break;
      case 0x2E: case 0x36: case 0x3E: case 0x26: break;      // the null segment overrides
      case 0x64: p.segment = 1; break;
      case 0x65: p.segment = 2; break;
      case 0x67: case 0xF0: prefix_seen_other = true; break;      // address-size override and lock: left to the interpreter
      default: is_prefix = false; break;
    }
    if (!is_prefix) break;
  }
  if (prefix_seen_other) return false;
  if ((op & 0xF0) == 0x40) {
    p.rex = true;
    p.w = op & 8;
    p.r = op & 4;
    p.x = op & 2;
    p.b = op & 1;
    op = f.u8();
  }
  if (f.truncated()) return false;
  X86Insn& d = *out;
  d.rex = p.rex;
  d.segment = p.segment;
  const uint8_t osize = p.w ? 8 : p.operand16 ? 2 : 4;
  uint8_t reg_field = 0;
  // A rep prefix means something only to the string instructions and to pause / endbr; the interpreter ignores it elsewhere, but that is rare enough not to model.
  const bool has_rep = p.rep || p.repne;

  auto modrm = [&]() { return decode_modrm(f, p, &d, &reg_field); };
  auto finish = [&](X86Op what) {
    if (f.truncated() || f.position() > 15) return false;
    d.op = what;
    d.length = static_cast<uint8_t>(f.position());
    return true;
  };

  if (op == 0x0F) {
    const uint8_t op2 = f.u8();
    if (f.truncated()) return false;
    switch (op2) {
      case 0x1F:      // multi-byte nop
        if (has_rep || !modrm()) return false;
        return finish(X86Op::Nop);
      case 0x1E:      // endbr64 / endbr32 (F3 0F 1E FA / FB)
        if (!p.rep) return false;
        {
          const uint8_t b = f.u8();
          if (b != 0xFA && b != 0xFB) return false;
        }
        return finish(X86Op::Nop);
      case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
      case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (has_rep || !modrm()) return false;
        d.size = osize;
        d.kind = op2 & 15;
        d.to_reg = true;
        return finish(X86Op::Cmov);
      case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
      case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        if (p.rep || p.operand16) return false;
        d.kind = op2 & 15;
        d.imm = f.imm(4, true);
        d.has_imm = true;
        return finish(X86Op::Jcc);
      case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
      case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        if (has_rep || !modrm()) return false;
        d.size = 1;
        d.kind = op2 & 15;
        return finish(X86Op::Setcc);
      case 0xAF:
        if (has_rep || !modrm()) return false;
        d.size = osize;
        d.to_reg = true;
        return finish(X86Op::Imul2);
      case 0xB6: case 0xB7: case 0xBE: case 0xBF:
        if (has_rep || !modrm()) return false;
        d.size = osize;
        d.src_size = (op2 & 1) ? 2 : 1;
        d.to_reg = true;
        return finish(op2 >= 0xBE ? X86Op::Movsx : X86Op::Movzx);
      default:
        return false;
    }
  }

  // The eight arithmetic operations in their six forms (adc and sbb are left to the interpreter).
  if (op < 0x40 && (op & 7) < 6) {
    const uint8_t kind = (op >> 3) & 7, form = op & 7;
    if (kind == 2 || kind == 3 || has_rep) return false;
    d.kind = kind;
    d.size = (form & 1) ? osize : 1;
    if (form >= 4) {
      d.accumulator = true;
      d.has_imm = true;
      d.imm = f.imm(d.size == 8 ? 4 : d.size, true);
      return finish(X86Op::Alu);
    }
    if (!modrm()) return false;
    d.to_reg = form >= 2;
    return finish(X86Op::Alu);
  }

  switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
      if (p.operand16 || has_rep) return false;
      d.reg = static_cast<uint8_t>((op & 7) | (p.b ? 8 : 0));
      d.opreg = true;
      d.size = 8;
      return finish(op < 0x58 ? X86Op::Push : X86Op::Pop);
    case 0x63:
      if (has_rep || p.operand16 || !modrm()) return false;
      d.size = osize;
      d.src_size = 4;
      d.to_reg = true;
      return finish(X86Op::Movsxd);
    case 0x68: case 0x6A:
      if (p.operand16 || has_rep) return false;
      d.size = 8;
      d.has_imm = true;
      d.imm = op == 0x68 ? f.imm(4, true) : f.imm(1, true);
      return finish(X86Op::Push);
    case 0x69: case 0x6B:
      if (has_rep || !modrm()) return false;
      d.size = osize;
      d.to_reg = true;
      d.has_imm = true;
      d.imm = op == 0x69 ? f.imm(osize == 2 ? 2 : 4, true) : f.imm(1, true);
      return finish(X86Op::Imul2);
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
      if (p.rep || p.operand16) return false;
      d.kind = op & 15;
      d.imm = f.imm(1, true);
      d.has_imm = true;
      return finish(X86Op::Jcc);
    case 0x80: case 0x81: case 0x83: {
      if (has_rep || !modrm()) return false;
      d.size = op == 0x80 ? 1 : osize;
      d.kind = reg_field;
      if (d.kind == 2 || d.kind == 3) return false;
      d.has_imm = true;
      d.imm = op == 0x81 ? f.imm(d.size == 2 ? 2 : 4, true) : f.imm(1, true);
      return finish(X86Op::Alu);
    }
    case 0x84: case 0x85:
      if (has_rep || !modrm()) return false;
      d.size = op == 0x84 ? 1 : osize;
      return finish(X86Op::Test);
    case 0x88: case 0x89: case 0x8A: case 0x8B:
      if (has_rep || !modrm()) return false;
      d.size = (op & 1) ? osize : 1;
      d.to_reg = op >= 0x8A;
      return finish(X86Op::Mov);
    case 0x8D:
      if (has_rep || !modrm() || !d.mem) return false;
      d.size = osize;
      d.to_reg = true;
      return finish(X86Op::Lea);
    case 0x90:
      if (p.b || p.repne || p.operand16) return false;      // (xchg r8, rax, and the 66 90 form)
      return finish(X86Op::Nop);      // (pause is F3 90)
    case 0x98:
      if (has_rep || p.operand16) return false;
      d.size = osize;
      return finish(X86Op::Cdqe);
    case 0x99:
      if (has_rep || p.operand16) return false;
      d.size = osize;
      return finish(X86Op::Cqo);
    case 0xA8: case 0xA9:
      if (has_rep) return false;
      d.size = op == 0xA8 ? 1 : osize;
      d.accumulator = true;
      d.has_imm = true;
      d.imm = f.imm(d.size == 8 ? 4 : d.size, true);
      return finish(X86Op::Test);
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
      if (has_rep) return false;
      d.size = 1;
      d.reg = static_cast<uint8_t>((op & 7) | (p.b ? 8 : 0));
      d.imm = f.imm(1, false);
      d.has_imm = true;
      d.to_reg = true;
      return finish(X86Op::Mov);
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      if (has_rep) return false;
      d.size = osize;
      d.reg = static_cast<uint8_t>((op & 7) | (p.b ? 8 : 0));
      d.imm = f.imm(osize, false);
      d.has_imm = true;
      d.to_reg = true;
      return finish(X86Op::Mov);
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: {
      if (has_rep || !modrm()) return false;
      d.size = (op & 1) ? osize : 1;
      d.kind = reg_field == 6 ? 4 : reg_field;      // (/6 is an alias of shl)
      if (d.kind != 4 && d.kind != 5 && d.kind != 7) return false;
      d.has_imm = true;
      d.imm = op < 0xD0 ? f.imm(1, false) : 1;
      // The count is masked like the hardware does; a masked count of zero leaves the flags alone, which the translator does not model.
      if ((d.imm & (d.size == 8 ? 63 : 31)) == 0) return false;
      return finish(X86Op::Shift);
    }
    case 0xC2: case 0xC3:
      if (p.rep || p.operand16) return false;      // (F2 is a bnd prefix and is ignored)
      d.size = 8;
      if (op == 0xC2) {
        d.has_imm = true;
        d.imm = f.imm(2, false);
      }
      return finish(X86Op::Ret);
    case 0xC6: case 0xC7:
      if (has_rep || !modrm()) return false;
      d.size = op == 0xC6 ? 1 : osize;
      if (reg_field != 0) return false;      // (xabort, xbegin and the invalid forms)
      d.has_imm = true;
      d.imm = f.imm(d.size == 8 ? 4 : d.size, true);
      return finish(X86Op::Mov);
    case 0xC9:
      if (has_rep || p.operand16) return false;
      d.size = 8;
      return finish(X86Op::Leave);
    case 0xE8: case 0xE9: case 0xEB:
      if (p.rep || p.operand16) return false;
      d.size = 8;
      d.has_imm = true;
      d.imm = op == 0xEB ? f.imm(1, true) : f.imm(4, true);
      return finish(op == 0xE8 ? X86Op::Call : X86Op::Jmp);
    case 0xF6: case 0xF7: {
      if (has_rep || !modrm()) return false;
      d.size = (op & 1) ? osize : 1;
      d.kind = reg_field;
      if (reg_field < 2) {
        d.has_imm = true;
        d.imm = f.imm(d.size == 8 ? 4 : d.size, true);
        return finish(X86Op::Test);
      }
      if (reg_field == 2) return finish(X86Op::Not);
      if (reg_field == 3) return finish(X86Op::Neg);
      return false;      // mul, imul, div, idiv
    }
    case 0xFE: case 0xFF: {
      if (has_rep || !modrm()) return false;
      d.size = (op & 1) ? osize : 1;
      d.kind = reg_field;
      if (reg_field < 2) return finish(reg_field == 0 ? X86Op::Inc : X86Op::Dec);
      if (op == 0xFE || p.operand16) return false;
      d.size = 8;
      if (reg_field == 2) {
        d.indirect = true;
        return finish(X86Op::Call);
      }
      if (reg_field == 4) {
        d.indirect = true;
        return finish(X86Op::Jmp);
      }
      if (reg_field == 6) return finish(X86Op::Push);
      return false;
    }
    default:
      return false;
  }
}

X86FlagUse x86_flag_use(const X86Insn& i) {
  switch (i.op) {
    case X86Op::Jcc: case X86Op::Setcc: case X86Op::Cmov: return X86FlagUse::Reads;
    case X86Op::Alu: case X86Op::Test: case X86Op::Neg: case X86Op::Shift: return X86FlagUse::WritesAll;
    case X86Op::Inc: case X86Op::Dec: case X86Op::Imul2: return X86FlagUse::Partial;
    default: return X86FlagUse::None;
  }
}

}  // namespace emulation
