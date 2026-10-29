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

// The x86-64 instructions outside the SSE / AVX / x87 families that are not in the main switch of X86_64Cpu.cpp: the 0F C7 group (cmpxchg8b/16b, xsavec, rdrand, rdseed, rdpid), the register
// forms of 0F 01 (xgetbv, xend, xtest, rdtscp), adcx / adox, rdfsbase and its relatives, movnti, movdiri and movdir64b.
#include "Emulation/HalfFloat.hpp"
#include "Emulation/SoftFloat.hpp"
#include "Emulation/X86_64Cpu.hpp"
#include "Emulation/X86_64EvexInternal.hpp"

namespace emulation {

uint64_t X86_64Cpu::next_random() {
  if (random_state_ == 0) random_state_ = 0x9E3779B97F4A7C15ull ^ (retired_ * 0xBF58476D1CE4E5B9ull) ^ s_.rip;
  random_state_ += 0x9E3779B97F4A7C15ull;      // splitmix64
  uint64_t z = random_state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

X86_64Cpu::Step X86_64Cpu::group9(GuestMemory& m) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t kind = d_.reg & 7;
  if (d_.mod != 3) {
    if (kind == 4 && !d_.operand16 && !d_.rep && !d_.repne) {      // xsavec
      const Step r = xsave_family(m, 8);
      if (r != Step::Ok) return r;
      s_.rip += d_.position;
      return Step::Ok;
    }
    if (kind != 1 || d_.operand16 || d_.rep) return fail(6);
    // cmpxchg8b / (REX.W) cmpxchg16b: compare EDX:EAX (RDX:RAX) with the memory operand; equal: store ECX:EBX (RCX:RBX), else load it into EDX:EAX. Only ZF changes. The
    // processor always writes (the old value back on a mismatch), so an unwritable operand faults either way.
    const uint64_t address = memory_address();
    const bool wide = d_.rex_w;
    const uint32_t half = wide ? 8 : 4;
    if (wide && (address & 15) != 0) return fail(13);
    uint8_t raw[16] = {};
    if (!m.read(address, ByteView(raw, 2 * half))) return fail(14, address, 0);
    auto get = [&](uint32_t at) { uint64_t v = 0; for (uint32_t b = 0; b < half; ++b) v |= uint64_t{raw[at + b]} << (8 * b); return v; };
    const uint64_t low = get(0), high = get(half);
    const uint64_t mask = wide ? ~uint64_t{0} : 0xFFFFFFFFu;
    const bool equal = low == (s_.r[0] & mask) && high == (s_.r[2] & mask);
    if (equal) {
      const uint64_t new_low = s_.r[3] & mask, new_high = s_.r[1] & mask;
      for (uint32_t b = 0; b < half; ++b) {
        raw[b] = static_cast<uint8_t>(new_low >> (8 * b));
        raw[half + b] = static_cast<uint8_t>(new_high >> (8 * b));
      }
    }
    if (!m.write(address, ConstByteView(raw, 2 * half))) return fail(14, address, 2);
    if (equal) {
      s_.rflags |= kZero;
    } else {
      s_.rflags &= ~kZero;
      s_.r[0] = low;
      s_.r[2] = high;
    }
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (d_.rep && kind == 7) {      // rdpid: IA32_TSC_AUX, which is zero here
    s_.r[d_.rm] = 0;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (!d_.rep && !d_.repne && (kind == 6 || kind == 7)) {      // rdrand, rdseed: always succeed
    const uint32_t size = operand_size();
    set_reg(d_.rm, size, next_random());
    s_.rflags = (s_.rflags & ~(kOverflow | kSign | kZero | kAdjust | kParity)) | kCarry;
    s_.rip += d_.position;
    return Step::Ok;
  }
  return fail(6);
}

X86_64Cpu::Step X86_64Cpu::system_0f01(GuestMemory& m) {
  (void)m;
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (d_.mod != 3) return fail(6);
  switch (0xC0 | ((d_.reg & 7) << 3) | (d_.rm & 7)) {
    case 0xD0:      // xgetbv: XCR0 says x87, SSE, AVX, the AVX-512 state and the AMX tile state and the APX registers are enabled
      if (static_cast<uint32_t>(s_.r[1]) != 0) return fail(13);
      s_.r[0] = 0xE00E7;
      s_.r[2] = 0;
      break;
    case 0xD5:      // xend outside a transaction
      return fail(13);
    case 0xD6:      // xtest: not in a transaction
      s_.rflags = (s_.rflags & ~(kCarry | kParity | kAdjust | kSign | kOverflow)) | kZero;
      break;
    case 0xF9:      // rdtscp: rdtsc and IA32_TSC_AUX (zero) in ECX
      s_.r[0] = (retired_ * 4) & 0xFFFFFFFFu;
      s_.r[2] = ((retired_ * 4) >> 32) & 0xFFFFFFFFu;
      s_.r[1] = 0;
      break;
    default: return fail(6);
  }
  s_.rip += d_.position;
  return Step::Ok;
}

X86_64Cpu::Step X86_64Cpu::adx_instruction(GuestMemory& m, bool overflow) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  const uint32_t size = d_.rex_w ? 8 : 4;
  uint64_t source = 0;
  if (!read_rm(m, size, &source)) return Step::Fault;
  const uint64_t flag = overflow ? kOverflow : kCarry;
  const uint64_t destination = get_reg(d_.reg, size);
  const uint64_t carry_in = (s_.rflags & flag) ? 1 : 0;
  const unsigned __int128 sum = static_cast<unsigned __int128>(destination) + source + carry_in;
  const uint64_t mask = size == 8 ? ~uint64_t{0} : 0xFFFFFFFFu;
  set_reg(d_.reg, size, static_cast<uint64_t>(sum) & mask);
  const bool carry_out = (sum >> (size * 8)) != 0;
  s_.rflags = (s_.rflags & ~flag) | (carry_out ? flag : 0);
  s_.rip += d_.position;
  return Step::Ok;
}

// (the caller has decoded the ModRM byte and the instruction)
X86_64Cpu::Step X86_64Cpu::fsgsbase_instruction(GuestMemory& m) {
  (void)m;
  const uint32_t kind = d_.reg & 7;
  const uint32_t size = d_.rex_w ? 8 : 4;
  if (kind < 2) {      // rdfsbase, rdgsbase
    set_reg(d_.rm, size, kind == 0 ? s_.fs_base : s_.gs_base);
  } else {      // wrfsbase, wrgsbase: the 32-bit form zero-extends, the 64-bit one needs a canonical address
    uint64_t v = get_reg(d_.rm, size);
    if (size == 8 && ((v >> 47) != 0 && (v >> 47) != 0x1FFFF)) return fail(13);
    (kind == 2 ? s_.fs_base : s_.gs_base) = v;
  }
  s_.rip += d_.position;
  return Step::Ok;
}

X86_64Cpu::Step X86_64Cpu::direct_store(GuestMemory& m, uint8_t op) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (d_.mod == 3) return fail(6);
  if (op == 0xF9) {      // movdiri [m], r32/r64: a store
    const uint32_t size = d_.rex_w ? 8 : 4;
    if (!store(m, memory_address(), size, get_reg(d_.reg, size))) return Step::Fault;
  } else {      // movdir64b r, [m512]: 64 bytes from the (64-byte aligned) source to the address in the register
    const uint64_t source = memory_address();
    const uint64_t destination = get_reg(d_.reg, d_.address32 ? 4 : 8);
    if ((source & 63) != 0 || (destination & 63) != 0) return fail(13);
    uint8_t block[64];
    if (!m.read(source, ByteView(block, 64))) return fail(14, source, 0);
    if (!m.write(destination, ConstByteView(block, 64))) return fail(14, destination, 2);
  }
  s_.rip += d_.position;
  return Step::Ok;
}

X86_64Cpu::Step X86_64Cpu::movnti(GuestMemory& m) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (d_.mod == 3 || d_.operand16 || d_.rep || d_.repne) return fail(6);
  const uint32_t size = d_.rex_w ? 8 : 4;
  if (!store(m, memory_address(), size, get_reg(d_.reg, size))) return Step::Fault;
  s_.rip += d_.position;
  return Step::Ok;
}

// SSE4a bit-field instructions on the low quadword: extrq takes bits [index + length - 1 : index] of the destination into its bit 0 upwards (zero-filled); insertq puts the low `length` bits of
// the source at `index` of the destination. A length of zero means 64. The upper quadword of the destination is undefined by the architecture; it is cleared, as on Zen.
X86_64Cpu::Step X86_64Cpu::sse4a_bitfield(GuestMemory& m, uint8_t op) {
  (void)m;
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const bool extract = d_.operand16;
  if (!decode_modrm()) return bad_decode();
  uint32_t length = 0, index = 0;
  const bool immediate = op == 0x78;
  if (immediate) {
    length = static_cast<uint32_t>(next_imm(1, false));
    index = static_cast<uint32_t>(next_imm(1, false));
  }
  if (!finish_decode()) return bad_decode();
  if (d_.mod != 3 || (extract && immediate && (d_.reg & 7) != 0)) return fail(6);
  Xmm& destination = s_.xmm[extract && immediate ? d_.rm : d_.reg];
  const Xmm& source = s_.xmm[d_.rm];
  if (!immediate) {      // the field comes from the second register: bits 5:0 and 13:8 of its low quadword (extrq) or high quadword (insertq)
    const uint64_t control = extract ? source.lo : source.hi;
    length = static_cast<uint32_t>(control & 0x3F);
    index = static_cast<uint32_t>((control >> 8) & 0x3F);
  }
  length &= 0x3F;
  index &= 0x3F;
  if (length == 0) length = 64;
  const uint64_t mask = length == 64 ? ~uint64_t{0} : (uint64_t{1} << length) - 1;
  if (extract) destination.lo = (destination.lo >> index) & mask;
  else destination.lo = (destination.lo & ~(mask << index)) | ((source.lo & mask) << index);
  destination.hi = 0;      // (undefined by the architecture; Zen clears it)
  s_.rip += d_.position;
  return Step::Ok;
}

// The VEX encoding of an instruction that has an EVEX form (the AVX-VNNI, AVX-IFMA, AVX-VNNI-INT8 / INT16 and vcvtneps2bf16 groups): the same operation with no write mask, no broadcast and
// a vector length of 128 or 256 bits, run by the EVEX code from a context that says so.
X86_64Cpu::Step X86_64Cpu::vex_via_evex(GuestMemory& m, uint32_t map, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  const EvexOp* def = nullptr;
  for (uint32_t i = 0; i < kEvexOpCount && !def; ++i) {
    const EvexOp& o = kEvexOps[i];
    if (o.map == map && o.pp == pp && o.opcode == op && (o.w < 0 || o.w == static_cast<int>(wide_w)) && (o.kind == kVnni || o.kind == kMadd52 || o.kind == kDot || o.kind == kBf16)) def = &o;
  }
  if (!def) return fail(6);
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  ev_ = EvexContext();
  ev_.map = static_cast<uint8_t>(map);
  ev_.pp = static_cast<uint8_t>(pp);
  ev_.opcode = op;
  ev_.w = wide_w;
  ev_.memory = d_.mod != 3;
  ev_.reg = d_.reg;
  ev_.rm = d_.rm;
  ev_.vvvv = vvvv;
  ev_.vl = vex_.wide ? 256 : 128;
  ev_.old_dest = get_vec(d_.reg);
  return evex_ext(m, *def);
}

// AVX-NE-CONVERT: bfloat16 and half-precision elements to single precision, exactly (no exceptions; a half-precision signaling NaN is quieted). The even or odd elements of a memory vector, or one
// element broadcast. Register operands do not exist.
X86_64Cpu::Step X86_64Cpu::vex_ne_convert(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv) {
  (void)vvvv;
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  if (wide_w || (op == 0xB1 && (pp == 0 || pp == 3))) return fail(6);
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (d_.mod == 3) return fail(6);
  const uint32_t count = vex_.wide ? 8 : 4;      // single-precision results
  const uint64_t address = memory_address();
  uint16_t elements[16] = {};
  const bool bfloat = pp == 2 || pp == 3;      // F3 / F2: bfloat16; NP / 66: half precision
  if (op == 0xB1) {      // broadcast one element
    uint64_t v = 0;
    if (!load(m, address, 2, &v)) return Step::Fault;
    for (uint32_t i = 0; i < count; ++i) elements[i] = static_cast<uint16_t>(v);
  } else {      // 2 * count elements; the even ones (66, F3) or the odd ones (NP, F2) are converted
    uint8_t raw[32] = {};
    if (!m.read(address, ByteView(raw, 4 * count))) return fail(14, address, 0);
    const bool odd = pp == 0 || pp == 3;
    for (uint32_t i = 0; i < count; ++i) elements[i] = static_cast<uint16_t>(raw[4 * i + (odd ? 2 : 0)] | (raw[4 * i + (odd ? 2 : 0) + 1] << 8));
  }
  Xmm out[2];
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = bfloat ? static_cast<uint32_t>(elements[i]) << 16 : half::half_to_single(elements[i]);
    Xmm& half = out[i / 4];
    uint64_t& qword = (i & 2) ? half.hi : half.lo;
    qword |= static_cast<uint64_t>(value) << (32 * (i & 1));
  }
  s_.xmm[d_.reg] = out[0];
  s_.ymm_high[d_.reg] = vex_.wide ? out[1] : Xmm();
  s_.rip += d_.position;
  return Step::Ok;
}

// AVX-512 ER (Knights Landing): vexp2ps/pd, vrcp28ps/pd/ss/sd, vrsqrt28ps/pd/ss/sd. The architecture promises a relative error below 2^-23 (exp2) and 2^-28 (rcp28, rsqrt28); the results here are
// computed far more precisely and rounded to nearest, so they are within the bounds but not necessarily the hardware's bits. Denormal inputs are zeros, denormal results are flushed (always,
// whatever MXCSR says), the rounding is to nearest, and EVEX.b with a register operand only suppresses the exception flags.
namespace {

using softfloat::Env;
using softfloat::Format;

// 2^x for a finite x in a range where the result is a normal number of `format`, computed in binary64: x = n + f, 2^f from its Taylor series, then scaled by the exact power 2^n.
uint64_t exp2_double(uint64_t x_bits, Env& env) {
  const Format f64 = Format::F64;
  Env exact;
  exact.mode = softfloat::Rounding::NearestEven;
  const uint64_t floor_x = softfloat::round_to_integral(f64, x_bits, softfloat::Rounding::Down, false, exact);
  const int64_t n = softfloat::to_int(f64, floor_x, 64, true, exact);
  const uint64_t fraction = softfloat::sub(f64, x_bits, floor_x, exact);
  // exp(fraction * ln 2)
  const uint64_t ln2 = 0x3FE62E42FEFA39EFull;
  const uint64_t t = softfloat::mul(f64, fraction, ln2, exact);
  uint64_t sum = 0x3FF0000000000000ull, term = 0x3FF0000000000000ull;      // 1.0
  for (int k = 1; k <= 24; ++k) {
    term = softfloat::div(f64, softfloat::mul(f64, term, t, exact), softfloat::from_int(f64, k, exact), exact);
    sum = softfloat::add(f64, sum, term, exact);
  }
  const uint64_t power = static_cast<uint64_t>(n + 1023) << 52;      // 2^n, n within -1022..1023
  (void)env;
  return softfloat::mul(f64, sum, power, exact);
}

}  // namespace

X86_64Cpu::Step X86_64Cpu::evex_er(GuestMemory& m, const EvexOp& defn) {
  const EvexOp* def = &defn;
  const bool scalar = (def->opcode & 1) != 0;      // CB, CD: ss / sd
  const bool is_double = ev_.w;
  const uint32_t elem = is_double ? 64 : 32;
  const Format fmt = is_double ? Format::F64 : Format::F32;
  if (!scalar && ev_.vl != 512) return fail(6);
  Vec b, result;
  const Vec a = get_vec(ev_.vvvv);
  if (scalar) {
    if (ev_.memory) {
      uint64_t v;
      if (!load(m, memory_address(), elem / 8, &v)) return Step::Fault;
      b.q[0] = v;
    } else {
      b = get_vec(ev_.rm);
    }
    result = a;
  } else if (!ev_read_rm(m, &b, elem, true, true)) {
    return Step::Fault;
  }
  Env env;
  env.mode = softfloat::Rounding::NearestEven;
  env.daz = true;
  env.ftz = true;
  const uint32_t count = scalar ? 1 : 512 / elem;
  const uint64_t one = is_double ? 0x3FF0000000000000ull : 0x3F800000u;
  const uint32_t exponent_bits = is_double ? 11 : 8, fraction_bits = is_double ? 52 : 23;
  for (uint32_t i = 0; i < count; ++i) {
    if (ev_.aaa != 0 && !ev_active(i)) {
      if (scalar) set_element(result, elem, 0, ev_.z ? 0 : element(ev_.old_dest, elem, 0));
      else set_element(result, elem, i, ev_.z ? 0 : element(ev_.old_dest, elem, i));
      continue;
    }
    uint64_t x = element(b, elem, i);
    Env le = env;
    le.flags = 0;
    uint64_t r;
    const uint64_t exponent_field = (x >> fraction_bits) & ((uint64_t{1} << exponent_bits) - 1), fraction = x & ((uint64_t{1} << fraction_bits) - 1);
    const bool negative = (x >> (fraction_bits + exponent_bits)) & 1;
    const bool is_nan = exponent_field == (uint64_t{1} << exponent_bits) - 1 && fraction != 0;
    const bool is_inf = exponent_field == (uint64_t{1} << exponent_bits) - 1 && fraction == 0;
    if (exponent_field == 0) x &= uint64_t{1} << (fraction_bits + exponent_bits);      // (DAZ)
    const bool is_zero = exponent_field == 0;
    if (is_nan) {
      if (!(fraction >> (fraction_bits - 1))) le.flags |= softfloat::kInvalid;
      r = x | (uint64_t{1} << (fraction_bits - 1));
    } else if (def->sub == 0) {      // exp2
      if (is_inf) r = negative ? 0 : x;
      else if (is_zero) r = one;
      else {
        // The result is a normal number for x in [-126, 128) (single) or [-1022, 1024) (double); above that it overflows, below it is flushed.
        Env c;
        const uint64_t as_double = is_double ? x : softfloat::convert(Format::F32, Format::F64, x, c);
        const int64_t upper = is_double ? 1024 : 128, lower = is_double ? -1022 : -126;
        if (softfloat::compare(Format::F64, as_double, softfloat::from_int(Format::F64, upper, c), false, c) != softfloat::Order::Less) {
          le.flags |= softfloat::kOverflow | softfloat::kInexact;
          r = ((((uint64_t{1} << exponent_bits) - 1)) << fraction_bits);
        } else if (softfloat::compare(Format::F64, as_double, softfloat::from_int(Format::F64, lower, c), false, c) == softfloat::Order::Less) {
          le.flags |= softfloat::kUnderflow | softfloat::kInexact;
          r = 0;
        } else {
          const uint64_t d = exp2_double(as_double, le);
          Env c2;
          c2.mode = softfloat::Rounding::NearestEven;
          r = is_double ? d : softfloat::convert(Format::F64, Format::F32, d, c2);
          // exactly 2^n for an integer x; anything else is inexact
          Env c3;
          const bool integral = softfloat::round_to_integral(Format::F64, as_double, softfloat::Rounding::Down, false, c3) == as_double;
          if (!integral) le.flags |= softfloat::kInexact;
        }
      }
    } else if (def->sub == 1) {      // rcp28
      r = softfloat::div(fmt, one, x, le);
    } else {      // rsqrt28
      if (is_zero) {
        le.flags |= softfloat::kDivideByZero;
        r = ((uint64_t{negative} << (fraction_bits + exponent_bits)) | (((uint64_t{1} << exponent_bits) - 1) << fraction_bits));
      } else if (negative) {
        le.flags |= softfloat::kInvalid;
        r = is_double ? 0xFFF8000000000000ull : 0xFFC00000u;
      } else if (is_inf) {
        r = 0;
      } else {
        Env c;
        c.mode = softfloat::Rounding::NearestEven;
        const uint64_t as_double = is_double ? x : softfloat::convert(Format::F32, Format::F64, x, c);
        const uint64_t root = softfloat::sqrt(Format::F64, as_double, c);
        const uint64_t quotient = softfloat::div(Format::F64, 0x3FF0000000000000ull, root, c);
        r = is_double ? quotient : softfloat::convert(Format::F64, Format::F32, quotient, c);
        le.flags |= c.flags & softfloat::kInexact;
        if (!(c.flags & softfloat::kInexact)) le.flags &= ~softfloat::kInexact;
      }
    }
    // a denormal result is flushed
    const uint64_t result_exponent = (r >> fraction_bits) & ((uint64_t{1} << exponent_bits) - 1);
    if (result_exponent == 0 && (r & ((uint64_t{1} << fraction_bits) - 1)) != 0) r &= uint64_t{1} << (fraction_bits + exponent_bits);
    le.flags &= ~softfloat::kDenormal;
    if (!ev_.er) s_.mxcsr |= le.flags;
    set_element(result, elem, i, r);
  }
  if (scalar) set_vec(ev_.reg, result, 128);
  else set_vec(ev_.reg, result, 512);
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
