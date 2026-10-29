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

// Direct3D 9 bytecode (as decoded by D3d9Program) to TGSI text, for the virgl device. The text follows graphics::ShaderModuleDesc::tgsi, and means what D3d9Program::run_vertex /
// run_fragment compute:
//
//  - Direct3D's temporaries r0..r31 are TEMP[0..31]; the emitter's own temporaries follow them (scratch copies of source operands that have a modifier TGSI has no syntax for, a work
//    register for instructions that take several TGSI instructions, and a shadow register per output). Outputs are written to their shadow registers, which start at zero as in the
//    interpreter, and copied to OUT[] at the end - which is also where the vertex shader's position gets the Y flip (and where partial writes of a register become whole ones).
//  - The vertex shader always declares the four varyings, so that any pixel shader finds the ones it reads. A pixel shader declares the ones it reads.
//  - Instructions with a shift or saturate modifier compute into the work register, scale and clamp there (MAX / MIN: the _SAT suffix is not used), and copy under the write mask.
//  - `def` constants are immediates; c0..c7 are CONST[0..7]. Relative addressing (a0, vertex shaders) becomes CONST[ADDR[0].x+n] and is refused when the shader also has `def`s (an
//    immediate cannot be addressed that way, the interpreter's relative reads would see it).
//  - if / ifc / else / endif are IF / ELSE / ENDIF; the condition of an ifc is worked out with SLT / SGE / SEQ / SNE into a register first.
//
// What differs from the interpreter, at the edges: nrm of a zero vector, lit with y == 0 and w == 0, pow of a zero base and a zero exponent, and the precision of constants (twelve
// decimals, nothing above 1e9) follow the GPU's arithmetic; a relative read outside CONST[0..7] reads what the host driver gives.
#include "Kernel/Dxgkrnl/D3d9Decoded.hpp"
#include "ObjectContainer/List.hpp"

namespace dxgk {

class TgsiEmitter {
 public:
  using Instr = D3d9Program::Instruction;

  TgsiEmitter(const D3d9Program& program, allocators::IAllocator& allocator) : p_(program), a_(allocator), body_(allocator), imms_(allocator) {}

  NtStatus run(String* out);

 private:
  static constexpr uint32_t kScratch = 32;   // 4 registers for source operands
  static constexpr uint32_t kWork = 36, kWork2 = 37, kWork3 = 38;
  static constexpr uint32_t kPosition = 39, kVarying = 40, kColor = 44;   // (kVarying: four)
  static constexpr uint32_t kSys = 45;     // the system values (SysValue order), copied from where TGSI keeps them into registers of the interpreter's shape
  static constexpr uint32_t kColorExtra = kSys + kSysValueCount;   // colour outputs 1..3 (output 0 is kColor)
  static constexpr uint32_t kTempCount = kColorExtra + graphics::kMaxColorTargets - 1;
  static uint32_t color_register(uint32_t target) { return target == 0 ? kColor : kColorExtra + target - 1; }
  static constexpr uint32_t kMaxOutput = 120000;   // (the virgl command carrying the text has a 16-bit dword count)

  struct Imm {
    uint32_t bits[4];
  };

  // --- text
  String str() { return String(a_); }
  void add(String& s, StringView v) {
    if (!s.append(v)) oom_ = true;
  }
  void add_uint(String& s, uint64_t v) {
    if (!s.append_uint(v)) oom_ = true;
  }
  void add_float(String& s, float f) {
    if (!(f == f) || f > 1e9f || f < -1e9f) {
      unsupported_ = true;
      return;
    }
    double d = static_cast<double>(f);
    if (d < 0) {
      add(s, "-");
      d = -d;
    }
    uint64_t ip = static_cast<uint64_t>(d);
    double frac = d - static_cast<double>(ip);
    uint64_t fd = static_cast<uint64_t>(frac * 1e12 + 0.5);
    if (fd >= 1000000000000ull) {
      fd -= 1000000000000ull;
      ++ip;
    }
    add_uint(s, ip);
    add(s, ".");
    char digits[12];
    for (int i = 11; i >= 0; --i) {
      digits[i] = static_cast<char>('0' + fd % 10);
      fd /= 10;
    }
    add(s, StringView(digits, 12));
  }
  String temp(uint32_t index, const char* swizzle = ".xyzw") {
    String s = str();
    add(s, "TEMP[");
    add_uint(s, index);
    add(s, "]");
    add(s, swizzle);
    return s;
  }
  static char channel(uint32_t i) { return "xyzw"[i & 3]; }
  String mask_text(uint8_t mask) {
    String s = str();
    add(s, ".");
    for (uint32_t i = 0; i < 4; ++i)
      if (mask & (1u << i)) add(s, StringView(&"xyzw"[i], 1));
    return s;
  }

  // --- immediates
  uint32_t imm4(const float v[4]) {
    Imm im;
    for (uint32_t k = 0; k < 4; ++k) im.bits[k] = __builtin_bit_cast(uint32_t, v[k]);
    for (uint64_t i = 0; i < imms_.count(); ++i) {
      bool same = true;
      for (uint32_t k = 0; k < 4; ++k) same = same && imms_[i].bits[k] == im.bits[k];
      if (same) return static_cast<uint32_t>(i);
    }
    if (!imms_.push_back(im)) {
      oom_ = true;
      return 0;
    }
    return static_cast<uint32_t>(imms_.count() - 1);
  }
  uint32_t imm4_bits(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    const float v[4] = {__builtin_bit_cast(float, x), __builtin_bit_cast(float, y), __builtin_bit_cast(float, z), __builtin_bit_cast(float, w)};
    return imm4(v);
  }
  String imm_text(const float v[4], const char* swizzle) {
    String s = str();
    add(s, "IMM[");
    add_uint(s, imm4(v));
    add(s, "]");
    add(s, swizzle);
    return s;
  }
  String splat(float x) {
    const float v[4] = {x, x, x, x};
    return imm_text(v, ".xxxx");
  }
  String zero() { return splat(0.0f); }

  // --- instructions
  void line(const char* op, const String* d = nullptr, const String* s0 = nullptr, const String* s1 = nullptr, const String* s2 = nullptr) {
    add(body_, "  ");
    add_uint(body_, inst_++);
    add(body_, ": ");
    add(body_, op);
    const String* parts[4] = {d, s0, s1, s2};
    bool first = true;
    for (const String* part : parts) {
      if (!part) continue;
      add(body_, first ? " " : ", ");
      add(body_, part->view());
      first = false;
    }
    add(body_, "\n");
  }

  String negate(const String& s) {
    String r = str();
    if (s.size() && s[0] == '-') add(r, s.view().substr(1));
    else {
      add(r, "-");
      add(r, s.view());
    }
    return r;
  }
  String absolute(const String& s) {
    String r = str();
    StringView v = s.view();
    if (v.size() && v[0] == '-') v = v.substr(1);
    if (v.size() && v[0] == '|') add(r, v);
    else {
      add(r, "|");
      add(r, v);
      add(r, "|");
    }
    return r;
  }

  // A source operand as TGSI text. `offset` is added to a constant register's number (matrix rows), `slot` picks the scratch register a modifier needs, `perm` (or null) says which
  // of the source's own swizzle channels go in each position, `scalar` repeats the first channel (the instructions that read one component).
  String operand(const Instr::Src& s, uint32_t offset, uint32_t slot, const uint8_t* perm, bool scalar);
  String component(const Instr::Src& s, uint32_t c, uint32_t slot) {
    const uint8_t perm[4] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c), static_cast<uint8_t>(c), static_cast<uint8_t>(c)};
    return operand(s, 0, slot, perm, false);
  }
  String destination(const Instr::Dst& d);
  // Where an instruction writes: the destination under its mask when one TGSI instruction will do (`direct`), else the work register, copied by end_result().
  String begin_result(const Instr& ins, bool multi, bool* direct);
  void end_result(const Instr& ins, const String& destination, bool direct);

  // `name` applied to each component of the first source that the destination writes, into the work register (copied by end_result()).
  void per_channel(const Instr& ins, const char* name, bool absolute_input, bool* direct) {
    begin_result(ins, true, direct);
    const String w = temp(kWork), z = zero();
    line("MOV", &w, &z);
    for (uint32_t c = 0; c < 4; ++c) {
      if (!(ins.dst.mask & (1u << c))) continue;
      String a = component(ins.src[0], c, c);
      if (absolute_input) a = absolute(a);
      String target = temp(kWork, "");
      add(target, ".");
      add(target, StringView(&"xyzw"[c], 1));
      line(name, &target, &a);
    }
  }
  // The first component of a source as a value UIF can test: the source itself (non-zero), or, for `is_zero`, a mask that is non-zero when the source is zero.
  String zero_test(const Instr::Src& src, bool is_zero) {
    String a = operand(src, 0, 0, nullptr, true);
    if (!is_zero) return a;   // (moved out)
    const String w = temp(kWork3, ".x");
    String z = str();
    add(z, "IMM[");
    add_uint(z, imm4_bits(0, 0, 0, 0));
    add(z, "].xxxx");
    line("USEQ", &w, &a, &z);
    return temp(kWork3, ".xxxx");
  }
  bool instruction(const Instr& ins);
  bool compare(const Instr& ins);
  void assemble(String* out);

  const D3d9Program& p_;
  allocators::IAllocator& a_;
  String body_;
  List<Imm> imms_;
  uint32_t inst_ = 0;
  bool oom_ = false, unsupported_ = false;
  bool vertex_ = false, addr_used_ = false;
  int relative_key_ = -2;   // the address register's source in the instruction being emitted (-2: none yet, -1: a0, else the integer register and component)
  bool in_used_[16] = {};
  bool sys_used_[kSysValueCount] = {};
  uint32_t max_attribute_ = 0;
  bool sampler_used_[graphics::kMaxTextureUnits] = {};
};

String TgsiEmitter::operand(const Instr::Src& s, uint32_t offset, uint32_t slot, const uint8_t* perm, bool scalar) {
  String base = str();
  switch (s.type) {
    case kTemp:
      add(base, "TEMP[");
      add_uint(base, s.number);
      add(base, "]");
      break;
    case kInput:
      if (s.slot < 0 || s.slot >= 16) {
        unsupported_ = true;
        break;
      }
      in_used_[s.slot] = true;
      if (vertex_ && static_cast<uint32_t>(s.slot) > max_attribute_) max_attribute_ = static_cast<uint32_t>(s.slot);
      add(base, "IN[");
      add_uint(base, static_cast<uint32_t>(s.slot));
      add(base, "]");
      break;
    case kAddrTex:   // (a pixel shader's texture coordinate register; in a vertex shader this is a0, which TGSI cannot read)
      if (s.slot < 0 || s.slot >= 16) {
        unsupported_ = true;
        break;
      }
      in_used_[s.slot] = true;
      add(base, "IN[");
      add_uint(base, static_cast<uint32_t>(s.slot));
      add(base, "]");
      break;
    case kSysValue:   // (copied to a register by the prologue; the stage decides which exist)
      if (s.number >= kSysValueCount || (vertex_ ? s.number != kSysVertexId && s.number != kSysInstanceId : s.number == kSysVertexId || s.number == kSysInstanceId)) {
        unsupported_ = true;
        break;
      }
      if (s.number == kSysPrimitiveId) {   // (virglrenderer's GLSL has no gl_PrimitiveID in a pixel shader without a geometry stage: refused, not lost)
        unsupported_ = true;
        break;
      }
      add(base, "TEMP[");
      add_uint(base, kSys + s.number);
      add(base, "]");
      break;
    case kConst: {
      const uint32_t reg = s.number + offset;
      if (s.relative) {
        // (a0 of Direct3D 9, or an integer register of Shader Model 4; one address per instruction, and no immediates in the way: an immediate cannot be addressed)
        const int key = s.relative_temp >= 0 ? 16 + s.relative_temp * 4 + s.relative_component : -1;
        bool addressed_def = false;   // (a def inside the range that can be addressed would be read by the interpreter; a literal beyond the buffer is not in the way)
        for (const D3d9Program::Def& d : p_.defs_.view()) addressed_def = addressed_def || !s.relative_limit || d.number < s.relative_limit;
        if (addressed_def || (s.relative_temp < 0 && s.relative_component != 0) || (s.relative_temp >= 0 && s.relative_temp >= static_cast<int>(kScratch)) || (relative_key_ != -2 && relative_key_ != key)) {
          unsupported_ = true;
          break;
        }
        if (s.relative_temp >= 0 && relative_key_ != key) {
          String component = temp(static_cast<uint32_t>(s.relative_temp), "");
          add(component, ".");
          for (int k = 0; k < 4; ++k) add(component, StringView(&"xyzw"[s.relative_component & 3], 1));
          String target = str();
          add(target, "ADDR[0].x");
          line("UARL", &target, &component);
        }
        relative_key_ = key;
        add(base, "CONST[ADDR[0].x+");
        add_uint(base, reg);
        add(base, "]");
        break;
      }
      bool found = false;
      for (const D3d9Program::Def& d : p_.defs_.view()) {
        if (d.number != reg) continue;
        base = imm_text(d.value, "");
        found = true;
        break;
      }
      if (!found) {
        if (reg >= p_.constant_registers_) {
          unsupported_ = true;
          break;
        }
        add(base, "CONST[");
        add_uint(base, reg);
        add(base, "]");
      }
      break;
    }
    default: unsupported_ = true; break;
  }
  String swz = str();
  add(swz, ".");
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t from = scalar ? 0 : perm ? perm[i] & 3 : i;
    add(swz, StringView(&"xyzw"[s.swizzle[from] & 3], 1));
  }
  String plain = str();
  add(plain, base.view());
  add(plain, swz.view());
  const bool negate_result = s.modifier == kModNeg || s.modifier == kModAbsNeg || s.modifier == kModBiasNeg || s.modifier == kModSignNeg || s.modifier == kModX2Neg;
  switch (s.modifier) {
    case kModNone: return plain;
    case kModNeg: return negate(plain);
    case kModAbs: return absolute(plain);
    case kModAbsNeg: return negate(absolute(plain));
    default: break;
  }
  // The modifiers TGSI has no syntax for are worked out into a scratch register.
  const uint32_t reg = kScratch + (slot & 3);
  String scratch = temp(reg);
  switch (s.modifier) {
    case kModBias: case kModBiasNeg: {
      const String half = splat(-0.5f);
      line("ADD", &scratch, &plain, &half);
      break;
    }
    case kModSign: case kModSignNeg: {
      const String two = splat(2.0f), minus_one = splat(-1.0f);
      line("MAD", &scratch, &plain, &two, &minus_one);
      break;
    }
    case kModComp: {
      const String one = splat(1.0f);
      const String minus = negate(plain);
      line("ADD", &scratch, &one, &minus);
      break;
    }
    case kModX2: case kModX2Neg: {
      const String two = splat(2.0f);
      line("MUL", &scratch, &plain, &two);
      break;
    }
    default: unsupported_ = true; break;
  }
  if (negate_result) return negate(scratch);
  return scratch;
}

String TgsiEmitter::destination(const Instr::Dst& d) {
  switch (d.type) {
    case kTemp: return temp(d.number, "");
    case kRastOut: return temp(kPosition, "");
    case kAttrOut:
    case kTexCrdOut:
      if (d.slot == kSlotPosition) return temp(kPosition, "");
      if (d.slot >= 0 && d.slot < static_cast<int8_t>(graphics::kMaxVaryings)) return temp(kVarying + static_cast<uint32_t>(d.slot), "");
      break;
    case kColorOut: return temp(color_register(d.slot >= 0 ? static_cast<uint32_t>(d.slot) : 0), "");
    default: break;
  }
  unsupported_ = true;
  return str();
}

String TgsiEmitter::begin_result(const Instr& ins, bool multi, bool* direct) {
  *direct = !multi && ins.dst.shift == 0 && !ins.dst.saturate;
  if (*direct) {
    String d = destination(ins.dst);
    add(d, mask_text(ins.dst.mask).view());
    return d;
  }
  return temp(kWork);
}

void TgsiEmitter::end_result(const Instr& ins, const String& dst, bool direct) {
  if (direct) return;
  const String work = temp(kWork);
  if (ins.dst.shift) {
    const float scale = ins.dst.shift > 0 ? static_cast<float>(1u << ins.dst.shift) : 1.0f / static_cast<float>(1u << -ins.dst.shift);
    const String factor = splat(scale);
    line("MUL", &work, &work, &factor);
  }
  if (ins.dst.saturate) {
    const String low = zero(), high = splat(1.0f);
    line("MAX", &work, &work, &low);
    line("MIN", &work, &work, &high);
  }
  String target = str();
  add(target, dst.view());
  add(target, mask_text(ins.dst.mask).view());
  line("MOV", &target, &work);
}

// The condition of an ifc, as 1 / 0 in the work register's x.
bool TgsiEmitter::compare(const Instr& ins) {
  const String a = operand(ins.src[0], 0, 0, nullptr, true);
  const String b = operand(ins.src[1], 0, 1, nullptr, true);
  const String w = temp(kWork3, ".x");
  switch (ins.control & 7) {
    case 1: line("SLT", &w, &b, &a); break;   // a > b
    case 2: line("SEQ", &w, &a, &b); break;
    case 3: line("SGE", &w, &a, &b); break;
    case 4: line("SLT", &w, &a, &b); break;
    case 5: line("SNE", &w, &a, &b); break;
    default: line("SGE", &w, &b, &a); break;   // a <= b (the interpreter's default)
  }
  return true;
}

bool TgsiEmitter::instruction(const Instr& ins) {
  const uint16_t op = ins.op;
  relative_key_ = -2;
  switch (op) {
    case kNop: return true;
    case kElse: line("ELSE"); return true;
    case kEndIf: line("ENDIF"); return true;
    case kIf: {
      bool value = false;
      for (const D3d9Program::DefBool& d : p_.bools_.view())
        if (d.number == ins.src[0].number) value = d.value;
      const String cond = splat(value ? 1.0f : 0.0f);
      line("IF", &cond);
      return true;
    }
    case kIfc: {
      compare(ins);
      const String cond = temp(kWork3, ".xxxx");
      line("IF", &cond);
      return true;
    }
    case kLoopBegin: line("BGNLOOP"); return true;
    case kLoopEnd: line("ENDLOOP"); return true;
    case kBreak0: line("BRK"); return true;
    case kContinue0: line("CONT"); return true;
    case kBreakNz: case kBreakZ: case kContinueNz: case kContinueZ: {   // (the test is on the 32 bits being non-zero: UIF)
      const String test = zero_test(ins.src[0], op == kBreakZ || op == kContinueZ);
      line("UIF", &test);
      line(op == kBreakNz || op == kBreakZ ? "BRK" : "CONT");
      line("ENDIF");
      return true;
    }
    case kIfNz: case kIfZ: {
      const String test = zero_test(ins.src[0], op == kIfZ);
      line("UIF", &test);
      return true;
    }
    case kDiscardNz: case kDiscardZ: {
      const String test = zero_test(ins.src[0], op == kDiscardZ);
      line("UIF", &test);
      line("KILL");
      line("ENDIF");
      return true;
    }
    case kTexKill: {
      // texkill kills the pixel when any of x, y, z of the register is negative: KILL_IF looks at all four channels, so w repeats z.
      const Instr::Src& s = ins.src[0];
      String t = str();
      if (s.type == kTemp) {
        add(t, "TEMP[");
        add_uint(t, s.number);
        add(t, "].xyzz");
      } else if (s.type == kAddrTex && s.slot >= 0 && s.slot < 16) {
        in_used_[s.slot] = true;
        add(t, "IN[");
        add_uint(t, static_cast<uint32_t>(s.slot));
        add(t, "].xyzz");
      } else {
        unsupported_ = true;
      }
      line("KILL_IF", &t);
      return true;
    }
    default: break;
  }

  if (ins.dst.type == kAddrTex) {
    // a0: mov truncates (ARL), mova rounds (ARR); only the x channel exists.
    if ((op != kMov && op != kMova) || ins.dst.mask != 1 || ins.dst.saturate || ins.dst.shift) {
      unsupported_ = true;
      return true;
    }
    const String a = operand(ins.src[0], 0, 0, nullptr, true);
    String target = str();
    add(target, "ADDR[0].x");
    line(op == kMova ? "ARR" : "ARL", &target, &a);
    return true;
  }
  if (ins.dst.mask == 0) return true;

  bool direct = false;
  const String d = destination(ins.dst);
  String t = str();
  const char* simple = nullptr;
  uint32_t sources = 0;
  bool scalar = false;
  switch (op) {
    case kMov: simple = "MOV", sources = 1; break;
    case kAdd: simple = "ADD", sources = 2; break;
    case kSub: simple = "SUB", sources = 2; break;
    case kMul: simple = "MUL", sources = 2; break;
    case kMad: simple = "MAD", sources = 3; break;
    case kMin: simple = "MIN", sources = 2; break;
    case kMax: simple = "MAX", sources = 2; break;
    case kSlt: simple = "SLT", sources = 2; break;
    case kSge: simple = "SGE", sources = 2; break;
    case kFrc: simple = "FRC", sources = 1; break;
    case kAbs: simple = "ABS", sources = 1; break;
    case kSgn: simple = "SSG", sources = 1; break;
    case kDp3: simple = "DP3", sources = 2; break;
    case kDp4: simple = "DP4", sources = 2; break;
    case kRcp: simple = "RCP", sources = 1, scalar = true; break;
    case kRsq: if (!ins.vector) simple = "RSQ", sources = 1, scalar = true; break;
    case kExp: if (!ins.vector) simple = "EX2", sources = 1, scalar = true; break;
    case kSeq: simple = "SEQ", sources = 2; break;
    case kSne: simple = "SNE", sources = 2; break;
    case kDp2: simple = "DP2", sources = 2; break;
    case kFloor: simple = "FLR", sources = 1; break;
    case kCeil: simple = "CEIL", sources = 1; break;
    case kTrunc: simple = "TRUNC", sources = 1; break;
    case kRoundNe: simple = "ROUND", sources = 1; break;
    // integers: the register's 32 bits are the value (TGSI's registers are untyped, the instruction says how to read them), comparisons give all ones or zero
    case kIAdd: simple = "UADD", sources = 2; break;
    case kINeg: simple = "INEG", sources = 1; break;
    case kIMul: case kUMul: simple = "UMUL", sources = 2; break;
    case kIMad: case kUMad: simple = "UMAD", sources = 3; break;
    case kIMin: simple = "IMIN", sources = 2; break;
    case kIMax: simple = "IMAX", sources = 2; break;
    case kUMin: simple = "UMIN", sources = 2; break;
    case kUMax: simple = "UMAX", sources = 2; break;
    case kUDiv: simple = "UDIV", sources = 2; break;
    case kURem: simple = "UMOD", sources = 2; break;
    case kAnd: simple = "AND", sources = 2; break;
    case kOr: simple = "OR", sources = 2; break;
    case kXor: simple = "XOR", sources = 2; break;
    case kNot: simple = "NOT", sources = 1; break;
    case kFtoI: simple = "F2I", sources = 1; break;
    case kFtoU: simple = "F2U", sources = 1; break;
    case kItoF: simple = "I2F", sources = 1; break;
    case kUtoF: simple = "U2F", sources = 1; break;
    case kIEq: simple = "USEQ", sources = 2; break;
    case kINe: simple = "USNE", sources = 2; break;
    case kILt: simple = "ISLT", sources = 2; break;
    case kIGe: simple = "ISGE", sources = 2; break;
    case kULt: simple = "USLT", sources = 2; break;
    case kUGe: simple = "USGE", sources = 2; break;
    default: break;
  }
  if (simple && ins.mask_result) {   // (a Shader Model 4 comparison is all ones for true: TGSI's FS comparisons)
    if (op == kSeq) simple = "FSEQ";
    else if (op == kSne) simple = "FSNE";
    else if (op == kSlt) simple = "FSLT";
    else if (op == kSge) simple = "FSGE";
  }
  if (simple) {
    t = begin_result(ins, false, &direct);
    const String s0 = operand(ins.src[0], 0, 0, nullptr, scalar);
    String s1 = str(), s2 = str();
    if (sources > 1) s1 = operand(ins.src[1], 0, 1, nullptr, false);
    if (sources > 2) s2 = operand(ins.src[2], 0, 2, nullptr, false);
    line(simple, &t, &s0, sources > 1 ? &s1 : nullptr, sources > 2 ? &s2 : nullptr);
    end_result(ins, d, direct);
    return true;
  }

  switch (op) {
    case kCmp: {   // a >= 0 ? b : c  is  CMP's (a < 0 ? c : b)
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false), b = operand(ins.src[1], 0, 1, nullptr, false), c = operand(ins.src[2], 0, 2, nullptr, false);
      line("CMP", &t, &a, &c, &b);
      break;
    }
    case kLog: {
      if (ins.vector) {
        per_channel(ins, "LG2", true, &direct);
        break;
      }
      t = begin_result(ins, false, &direct);
      const String a = absolute(operand(ins.src[0], 0, 0, nullptr, true));
      line("LG2", &t, &a);
      break;
    }
    case kPow: {
      t = begin_result(ins, false, &direct);
      const String a = absolute(operand(ins.src[0], 0, 0, nullptr, true)), b = operand(ins.src[1], 0, 1, nullptr, true);
      line("POW", &t, &a, &b);
      break;
    }
    case kLrp: {   // c + a * (b - c)
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false), b = operand(ins.src[1], 0, 1, nullptr, false), c = operand(ins.src[2], 0, 2, nullptr, false);
      const String w = temp(kWork2), minus_c = negate(c);
      line("ADD", &w, &b, &minus_c);
      line("MAD", &t, &a, &w, &c);
      break;
    }
    case kNrm: {   // a / |a.xyz|
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false);
      const String w = temp(kWork2, ".x"), wx = temp(kWork2, ".xxxx");
      line("DP3", &w, &a, &a);
      line("RSQ", &w, &wx);
      line("MUL", &t, &a, &wx);
      break;
    }
    case kDp2Add: {
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false), b = operand(ins.src[1], 0, 1, nullptr, false), c = operand(ins.src[2], 0, 2, nullptr, true);
      const String w = temp(kWork2, ".x"), wx = temp(kWork2, ".xxxx");
      line("DP2", &w, &a, &b);
      line("ADD", &t, &wx, &c);
      break;
    }
    case kCrs: {
      t = begin_result(ins, false, &direct);
      static const uint8_t kYzx[4] = {1, 2, 0, 3}, kZxy[4] = {2, 0, 1, 3};
      const String w2 = temp(kWork2), w3 = temp(kWork3);
      const String a1 = operand(ins.src[0], 0, 0, kYzx, false), b1 = operand(ins.src[1], 0, 1, kZxy, false);
      line("MUL", &w2, &a1, &b1);
      const String a2 = operand(ins.src[0], 0, 2, kZxy, false), b2 = operand(ins.src[1], 0, 3, kYzx, false);
      line("MUL", &w3, &a2, &b2);
      const String minus = negate(w3);
      line("ADD", &t, &w2, &minus);
      break;
    }
    case kTexLd: {
      const bool swizzled = ins.tex_swizzle[0] != 0 || ins.tex_swizzle[1] != 1 || ins.tex_swizzle[2] != 2 || ins.tex_swizzle[3] != 3;
      t = begin_result(ins, swizzled, &direct);
      const uint32_t unit = ins.src[1].number;
      if (unit >= graphics::kMaxTextureUnits) {
        unsupported_ = true;
        break;
      }
      sampler_used_[unit] = true;
      const String coordinate = operand(ins.src[0], 0, 0, nullptr, false);
      String sampler = str();
      add(sampler, "SAMP[");
      add_uint(sampler, unit);
      add(sampler, "]");
      String dim = str();
      add(dim, "2D");
      const String texel = swizzled ? temp(kWork2) : str();
      line((ins.control & 1) ? "TXP" : "TEX", swizzled ? &texel : &t, &coordinate, &sampler, &dim);
      if (swizzled) {
        String picked = temp(kWork2, ".");
        for (uint32_t c = 0; c < 4; ++c) add(picked, StringView(&"xyzw"[ins.tex_swizzle[c] & 3], 1));
        const String w = temp(kWork);
        line("MOV", &w, &picked);
      }
      break;
    }
    case kDiv: {   // a * (1 / b), the reciprocal taken per component
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false);
      for (uint32_t c = 0; c < 4; ++c) {
        const String bc = component(ins.src[1], c, 1 + c % 3);
        String target = temp(kWork2, "");
        add(target, ".");
        add(target, StringView(&"xyzw"[c], 1));
        line("RCP", &target, &bc);
      }
      const String w2 = temp(kWork2);
      line("MUL", &t, &a, &w2);
      break;
    }
    case kSelect: {   // a != 0 ? b : c  (the 32 bits of a, whether it is a mask or a float)
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false), b = operand(ins.src[1], 0, 1, nullptr, false), c = operand(ins.src[2], 0, 2, nullptr, false);
      line("UCMP", &t, &a, &b, &c);
      break;
    }
    case kIShl: case kIShr: case kUShr: {   // (only the low five bits of the count count)
      t = begin_result(ins, false, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, false), count = operand(ins.src[1], 0, 1, nullptr, false);
      const String w = temp(kWork2);
      String mask = str();
      add(mask, "IMM[");
      add_uint(mask, imm4_bits(31, 31, 31, 31));
      add(mask, "]");
      line("AND", &w, &count, &mask);
      line(op == kIShl ? "SHL" : op == kIShr ? "ISHR" : "USHR", &t, &a, &w);
      break;
    }
    case kTexFetch: case kTexLod: case kTexSize: {
      const bool swizzled = ins.tex_swizzle[0] != 0 || ins.tex_swizzle[1] != 1 || ins.tex_swizzle[2] != 2 || ins.tex_swizzle[3] != 3;
      t = begin_result(ins, true, &direct);
      const uint32_t unit = ins.src[1].number;
      if (unit >= graphics::kMaxTextureUnits) {
        unsupported_ = true;
        break;
      }
      sampler_used_[unit] = true;
      String sampler = str();
      add(sampler, "SAMP[");
      add_uint(sampler, unit);
      add(sampler, "]");
      String dim = str();
      add(dim, "2D");
      const String texel = temp(kWork2);
      if (op == kTexFetch) {   // ld: integer x y in the first two components, the level in the last
        const String coordinate = operand(ins.src[0], 0, 0, nullptr, false);
        line("TXF", &texel, &coordinate, &sampler, &dim);
      } else if (op == kTexLod) {   // sample_l: the coordinate, and the level next to it
        const uint8_t xyxy[4] = {0, 1, 0, 1};
        const String coordinate = operand(ins.src[0], 0, 0, xyxy, false), level = operand(ins.src[2], 0, 1, nullptr, true);
        const String xy = temp(kWork3, ".xy"), lod = temp(kWork3, ".w"), packed = temp(kWork3);
        line("MOV", &xy, &coordinate);
        line("MOV", &lod, &level);
        line("TXL", &texel, &packed, &sampler, &dim);
      } else {   // resinfo: width, height, (depth), levels of the level named by the source
        const String level = operand(ins.src[0], 0, 0, nullptr, true);
        line("TXQ", &texel, &level, &sampler, &dim);
        const String tz = temp(kWork2, ".z"), z0 = zero();
        line("MOV", &tz, &z0);
        if (ins.control != 2) {   // floats (0) or reciprocals (1)
          line("U2F", &texel, &texel);
          if (ins.control == 1) line("RCP", &texel, &texel);
        }
      }
      const String w = temp(kWork);
      if (swizzled) {
        String picked = temp(kWork2, ".");
        for (uint32_t c = 0; c < 4; ++c) add(picked, StringView(&"xyzw"[ins.tex_swizzle[c] & 3], 1));
        line("MOV", &w, &picked);
      } else {
        line("MOV", &w, &texel);
      }
      break;
    }
    case kSqrt: case kSin: case kCos: case kRsq: case kExp:   // TGSI's scalar instructions, once per component
      per_channel(ins, op == kSqrt ? "SQRT" : op == kSin ? "SIN" : op == kCos ? "COS" : op == kRsq ? "RSQ" : "EX2", false, &direct);
      break;
    // --- instructions of several TGSI instructions: built in the work register
    case kSinCos: {
      t = begin_result(ins, true, &direct);
      const String a = operand(ins.src[0], 0, 0, nullptr, true);
      const String w = temp(kWork), z = zero();
      line("MOV", &w, &z);
      if (ins.dst.mask & 1) {
        const String wx = temp(kWork, ".x");
        line("COS", &wx, &a);
      }
      if (ins.dst.mask & 2) {
        const String wy = temp(kWork, ".y");
        line("SIN", &wy, &a);
      }
      break;
    }
    case kLit: {
      t = begin_result(ins, true, &direct);
      const String ax = component(ins.src[0], 0, 0), ay = component(ins.src[0], 1, 1), aw = component(ins.src[0], 3, 2);
      const String w = temp(kWork), z = zero();
      const float one_zero[4] = {1.0f, 0.0f, 0.0f, 1.0f};
      const String init = imm_text(one_zero, ".xyzw");
      line("MOV", &w, &init);
      const String sx = temp(kWork2, ".x"), sy = temp(kWork2, ".y"), sz = temp(kWork2, ".z"), sw = temp(kWork2, ".w");
      const String syy = temp(kWork2, ".yyyy"), szz = temp(kWork2, ".zzzz"), sww = temp(kWork2, ".wwww"), sxx = temp(kWork2, ".xxxx");
      const String low = splat(-127.9961f), high = splat(127.9961f);
      line("MAX", &sx, &ax, &z);
      line("MAX", &sy, &ay, &z);
      line("MAX", &sz, &aw, &low);
      line("MIN", &sz, &szz, &high);
      line("POW", &sw, &syy, &szz);   // max(y, 0) ^ w
      const String ux = temp(kWork3, ".x"), uy = temp(kWork3, ".y"), uxx = temp(kWork3, ".xxxx"), uyy = temp(kWork3, ".yyyy");
      line("SLT", &ux, &z, &ax);   // x > 0
      line("SLT", &uy, &z, &ay);   // y > 0
      line("MUL", &ux, &uxx, &uyy);
      const String not_ux = negate(uxx);
      line("CMP", &sw, &not_ux, &sww, &z);   // (both: the power, else 0)
      const String wy = temp(kWork, ".y"), wz = temp(kWork, ".z");
      line("MOV", &wy, &sxx);
      line("MOV", &wz, &sww);
      break;
    }
    case kDst: {   // (1, a.y * b.y, a.z, b.w)
      t = begin_result(ins, true, &direct);
      const String ay = component(ins.src[0], 1, 0), by = component(ins.src[1], 1, 1), az = component(ins.src[0], 2, 2), bw = component(ins.src[1], 3, 3);
      const float unit[4] = {1.0f, 0.0f, 0.0f, 0.0f};
      const String w = temp(kWork), wy = temp(kWork, ".y"), wz = temp(kWork, ".z"), ww = temp(kWork, ".w"), init = imm_text(unit, ".xyzw");
      line("MOV", &w, &init);
      line("MUL", &wy, &ay, &by);
      line("MOV", &wz, &az);
      line("MOV", &ww, &bw);
      break;
    }
    case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: {
      t = begin_result(ins, true, &direct);
      const uint32_t rows = op == kM4x4 || op == kM3x4 ? 4 : op == kM4x3 || op == kM3x3 ? 3 : 2;
      const bool wide = op == kM4x4 || op == kM4x3;
      const String a = operand(ins.src[0], 0, 0, nullptr, false);
      const String w = temp(kWork), z = zero();
      line("MOV", &w, &z);
      for (uint32_t i = 0; i < rows; ++i) {
        const String row = operand(ins.src[1], i, 1, nullptr, false);
        String target = str();
        add(target, "TEMP[");
        add_uint(target, kWork);
        add(target, "].");
        add(target, StringView(&"xyzw"[i], 1));
        line(wide ? "DP4" : "DP3", &target, &a, &row);
      }
      break;
    }
    default: unsupported_ = true; return true;
  }
  end_result(ins, d, direct);
  return true;
}

void TgsiEmitter::assemble(String* out) {
  out->clear();
  add(*out, vertex_ ? "VERT\n" : p_.color_outputs_ > 1 ? "FRAG\n" : "FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n");
  if (vertex_) {
    for (uint32_t i = 0; i <= max_attribute_; ++i) {
      add(*out, "DCL IN[");
      add_uint(*out, i);
      add(*out, "]\n");
    }
    if (sys_used_[kSysVertexId]) add(*out, "DCL SV[0], VERTEXID\n");
    if (sys_used_[kSysInstanceId]) add(*out, "DCL SV[1], INSTANCEID\n");
    add(*out, "DCL OUT[0], POSITION\n");
    for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) {
      add(*out, "DCL OUT[");
      add_uint(*out, i + 1);
      add(*out, "], GENERIC[");
      add_uint(*out, i);
      add(*out, "]\n");
    }
  } else {
    for (uint32_t i = 0; i < 16; ++i) {
      if (!in_used_[i]) continue;
      add(*out, "DCL IN[");
      add_uint(*out, i);
      add(*out, "], GENERIC[");
      add_uint(*out, i);
      add(*out, "], PERSPECTIVE\n");
    }
    if (sys_used_[kSysPosition]) add(*out, "DCL IN[16], POSITION, LINEAR\n");
    if (sys_used_[kSysFrontFacing]) add(*out, "DCL IN[17], FACE, CONSTANT\n");
    add(*out, "DCL OUT[0], COLOR\n");
    for (uint32_t i = 1; i < p_.color_outputs_; ++i) {
      add(*out, "DCL OUT[");
      add_uint(*out, i);
      add(*out, "], COLOR[");
      add_uint(*out, i);
      add(*out, "]\n");
    }
    for (uint32_t i = 0; i < graphics::kMaxTextureUnits; ++i) {
      if (!sampler_used_[i]) continue;
      add(*out, "DCL SAMP[");
      add_uint(*out, i);
      add(*out, "]\nDCL SVIEW[");
      add_uint(*out, i);
      add(*out, "], 2D, FLOAT\n");
    }
  }
  add(*out, "DCL CONST[0..");
  add_uint(*out, p_.constant_registers_ - 1);
  add(*out, "]\n");
  if (addr_used_) add(*out, "DCL ADDR[0]\n");
  add(*out, "DCL TEMP[0..");
  add_uint(*out, kTempCount - 1);
  add(*out, "]\n");
  for (uint64_t i = 0; i < imms_.count(); ++i) {
    add(*out, "IMM[");
    add_uint(*out, i);
    // A value that is no ordinary float (an integer literal: a denormal, or all ones) is written as its bits.
    bool integer = false;
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t exponent = (imms_[i].bits[k] >> 23) & 0xFF, mantissa = imms_[i].bits[k] & 0x7FFFFF;
      integer = integer || (exponent == 0 && mantissa != 0) || exponent == 0xFF;
    }
    add(*out, integer ? "] UINT32 {" : "] FLT32 {");
    for (uint32_t k = 0; k < 4; ++k) {
      if (k) add(*out, ", ");
      if (integer) add_uint(*out, imms_[i].bits[k]);   // (decimal: the parser takes no hex)
      else add_float(*out, __builtin_bit_cast(float, imms_[i].bits[k]));
    }
    add(*out, "}\n");
  }
  add(*out, body_.view());
}

NtStatus TgsiEmitter::run(String* out) {
  vertex_ = p_.stage_ == graphics::ShaderStage::Vertex;
  for (const Instr& ins : p_.code_.view()) {
    if (ins.dst.type == kAddrTex) addr_used_ = true;
    for (uint32_t k = 0; k < ins.src_count; ++k) {
      if (ins.src[k].relative) addr_used_ = true;
      if (ins.src[k].type == kSysValue && ins.src[k].number < kSysValueCount) sys_used_[ins.src[k].number] = true;
    }
  }

  // Prologue: the outputs start at zero, and so does a0.
  const String z = zero();
  if (addr_used_) {
    String target = str();
    add(target, "ADDR[0].x");
    line("ARL", &target, &z);
  }
  // The system values, as the interpreter has them: registers whose x (or all four, for the pixel's position) carries the value, the rest zero. A vertex's numbers are integers;
  // whether a pixel faces the viewer is all ones or zero.
  for (uint32_t k = 0; k < kSysValueCount; ++k) {
    if (!sys_used_[k]) continue;
    const String reg = temp(kSys + k), reg_x = temp(kSys + k, ".x");
    String source = str();
    if (vertex_) {
      add(source, k == kSysVertexId ? "SV[0].xxxx" : "SV[1].xxxx");
    } else {
      add(source, "IN[");
      add_uint(source, 16 + k);
      add(source, k == kSysPosition ? "]" : "].xxxx");
    }
    if (k == kSysPosition) {
      line("MOV", &reg, &source);
      continue;
    }
    line("MOV", &reg, &z);
    if (k == kSysFrontFacing) line("FSGE", &reg_x, &source, &z);   // (TGSI's FACE is +1 or -1)
    else line("MOV", &reg_x, &source);
  }
  if (vertex_) {
    const String pos = temp(kPosition);
    line("MOV", &pos, &z);
    for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) {
      const String v = temp(kVarying + i);
      line("MOV", &v, &z);
    }
  } else {
    for (uint32_t i = 0; i < p_.color_outputs_; ++i) {
      const String c = temp(color_register(i));
      line("MOV", &c, &z);
    }
  }

  for (const Instr& ins : p_.code_.view()) {
    instruction(ins);
    if (oom_ || unsupported_) break;
  }

  // Epilogue: the shadows to the outputs.
  if (!oom_ && !unsupported_) {
    if (vertex_) {
      const float flip[4] = {1.0f, -1.0f, 1.0f, 1.0f};
      const String pos = temp(kPosition), factor = imm_text(flip, ".xyzw");
      String o = str();
      add(o, "OUT[0]");
      line("MUL", &o, &pos, &factor);
      for (uint32_t i = 0; i < graphics::kMaxVaryings; ++i) {
        String target = str();
        add(target, "OUT[");
        add_uint(target, i + 1);
        add(target, "]");
        const String v = temp(kVarying + i);
        line("MOV", &target, &v);
      }
    } else {
      for (uint32_t i = 0; i < p_.color_outputs_; ++i) {
        String o = str();
        add(o, "OUT[");
        add_uint(o, i);
        add(o, "]");
        const String c = temp(color_register(i));
        line("MOV", &o, &c);
      }
    }
    add(body_, "  ");
    add_uint(body_, inst_++);
    add(body_, ": END\n");
  }
  if (oom_) return status::kNoMemory;
  if (unsupported_) return status::kNotSupported;
  assemble(out);
  if (oom_) return status::kNoMemory;
  if (unsupported_ || out->size() > kMaxOutput) return status::kNotSupported;
  return status::kSuccess;
}

NtStatus D3d9Program::emit_tgsi(String* out) const {
  TgsiEmitter emitter(*this, allocator_);
  return emitter.run(out);
}

}  // namespace dxgk
