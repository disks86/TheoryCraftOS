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
#include "Emulation/LoongArch64Cpu.hpp"

namespace emulation {

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint64_t sext32(uint64_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
constexpr uint32_t field(uint32_t v, uint32_t hi, uint32_t lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

uint64_t count_leading_zeros(uint64_t v, uint32_t width) {
  uint64_t n = 0;
  for (uint32_t i = width; i-- > 0 && !((v >> i) & 1);) ++n;
  return n;
}

uint64_t count_trailing_zeros(uint64_t v, uint32_t width) {
  uint64_t n = 0;
  for (uint32_t i = 0; i < width && !((v >> i) & 1); ++i) ++n;
  return n;
}

uint64_t reverse_bits(uint64_t v, uint32_t width) {
  uint64_t out = 0;
  for (uint32_t i = 0; i < width; ++i) out |= ((v >> i) & 1) << (width - 1 - i);
  return out;
}

uint64_t swap_bytes(uint64_t v, uint32_t bytes) {
  uint64_t out = 0;
  for (uint32_t i = 0; i < bytes; ++i) out |= ((v >> (8 * i)) & 0xFF) << (8 * (bytes - 1 - i));
  return out;
}

// Applies `each` to every `width`-bit piece of the low `total` bits.
template <typename F>
uint64_t per_piece(uint64_t v, uint32_t total, uint32_t width, F each) {
  const uint64_t mask = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
  uint64_t out = 0;
  for (uint32_t at = 0; at < total; at += width) out |= (each((v >> at) & mask) & mask) << at;
  return out;
}

// CRC-32 (the IEEE polynomial) and CRC-32C (Castagnoli's), reflected, over the low `bytes` bytes of `data`, starting from `crc`.
uint64_t crc32(uint64_t crc, uint64_t data, uint32_t bytes, uint32_t polynomial) {
  uint32_t c = static_cast<uint32_t>(crc);
  for (uint32_t i = 0; i < bytes; ++i) {
    c ^= static_cast<uint8_t>(data >> (8 * i));
    for (uint32_t bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? polynomial : 0);
  }
  return sext32(c);
}

uint64_t multiply_high_unsigned(uint64_t a, uint64_t b) {
  const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
  const uint64_t low = a0 * b0, m1 = a1 * b0, m2 = a0 * b1;
  const uint64_t carry = ((low >> 32) + (m1 & 0xFFFFFFFFu) + (m2 & 0xFFFFFFFFu)) >> 32;
  return a1 * b1 + (m1 >> 32) + (m2 >> 32) + carry;
}

uint64_t multiply_high_signed(uint64_t a, uint64_t b) {
  uint64_t high = multiply_high_unsigned(a, b);
  if (static_cast<int64_t>(a) < 0) high -= b;
  if (static_cast<int64_t>(b) < 0) high -= a;
  return high;
}

// What CPUCFG tells a program about this CPU.
uint64_t cpu_configuration(uint64_t word) {
  switch (word) {
    case 0: return 0x0014C010;      // the processor id (a 3A5000's, which is what programs know)
    // LA64, paging, 48-bit physical and virtual addresses, unaligned access, the page protections
    case 1: return 2u | (1u << 2) | (47u << 4) | (47u << 12) | (1u << 20) | (1u << 21) | (1u << 22);
    // floating point (single and double, version 1), the AM* atomics, and of version 1.1: the byte and halfword atomics and compare-and-swap
    case 2: return 1u | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 22) | (1u << 27) | (1u << 28);
    case 4: return 100000000;       // the frequency the stable counter is said to run at
    case 5: return 1u | (1u << 16);
    default: return 0;
  }
}

}  // namespace

// --- state ---------------------------------------------------------------------------------------------------------------------------------

void LoongArch64Cpu::reset(const process::ProgramStart& start) {
  s_ = State();
  s_.pc = start.entry;
  s_.r[3] = start.stack_pointer;
  s_.r[2] = start.fs_base;
  fault_ = GuestFault();
  retired_ = 0;
  reservation_valid_ = false;
}

bool LoongArch64Cpu::pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const {
  if (abi != process::SyscallAbi::Linux) return false;      // (there is no LoongArch Windows interface here)
  return process::decode_loongarch64_linux(ArrayView<const uint64_t>(s_.r, 32), out);
}

bool LoongArch64Cpu::get_signal_context(process::linuxabi::LinuxSignalContext* out) const {
  *out = process::linuxabi::LinuxSignalContext();
  out->arch = process::UserArch::LoongArch64;
  for (uint32_t i = 1; i < 32; ++i) out->x[i] = s_.r[i];
  out->pc = s_.pc;
  for (uint32_t i = 0; i < 32; ++i) out->vector[i] = s_.f[i];
  for (uint32_t i = 0; i < 8; ++i) out->pstate |= uint64_t{s_.fcc[i]} << (8 * i);
  out->fpcr = s_.fcsr;
  return true;
}

bool LoongArch64Cpu::set_signal_context(const process::linuxabi::LinuxSignalContext& context) {
  if (context.arch != process::UserArch::LoongArch64) return false;
  for (uint32_t i = 1; i < 32; ++i) s_.r[i] = context.x[i];
  s_.r[0] = 0;
  s_.pc = context.pc;
  for (uint32_t i = 0; i < 32; ++i) s_.f[i] = context.vector[i];
  for (uint32_t i = 0; i < 8; ++i) s_.fcc[i] = static_cast<uint8_t>((context.pstate >> (8 * i)) & 1);
  s_.fcsr = context.fpcr & kFcsrBits;
  reservation_valid_ = false;      // (an exception return gives up the LL/SC reservation)
  return true;
}

LoongArch64Cpu::Step LoongArch64Cpu::fail(uint8_t vector, uint64_t address, uint32_t error) {
  fault_.vector = vector;
  fault_.address = address;
  fault_.pc = instruction_start_;
  fault_.error = error;
  s_.pc = instruction_start_;
  return Step::Fault;
}

bool LoongArch64Cpu::load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out) {
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

bool LoongArch64Cpu::store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  if (!m.write(address, ConstByteView(raw, size))) {
    fail(14, address, 2);
    return false;
  }
  return true;
}

UniquePointer<IGuestCpu> LoongArch64Cpu::clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const {
  UniquePointer<LoongArch64Cpu> copy = UniquePointer<LoongArch64Cpu>::make(allocator, *this);
  if (!copy) return UniquePointer<IGuestCpu>();
  copy->s_.r[4] = 0;
  if (stack_pointer) copy->s_.r[3] = stack_pointer;
  if (set_tls) copy->s_.r[2] = tls;
  copy->reservation_valid_ = false;
  copy->retired_ = 0;
  return UniquePointer<IGuestCpu>(static_cast<UniquePointer<LoongArch64Cpu>&&>(copy));
}

StopReason LoongArch64Cpu::run(GuestMemory& memory, uint64_t max_instructions) {
  for (uint64_t done = 0; done < max_instructions; ++done) {
    const Step r = step(memory);
    if (r == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (r == Step::Syscall) return StopReason::Syscall;
  }
  // Threads take turns: a turn never ends inside an LL..SC sequence (another thread could store to the word and the SC would still succeed, where real hardware
  // loses the reservation). A reservation still open after a generous allowance is dropped, which the architecture allows at any time (the SC then fails and
  // the program tries again).
  for (uint32_t extra = 0; reservation_valid_ && extra < 64; ++extra) {
    const Step r = step(memory);
    if (r == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (r == Step::Syscall) return StopReason::Syscall;
  }
  reservation_valid_ = false;
  return StopReason::Budget;
}

LoongArch64Cpu::Step LoongArch64Cpu::step(GuestMemory& m) {
  instruction_start_ = s_.pc;
  if (s_.pc & 3) return fail(13, s_.pc);      // (an instruction address that is not a multiple of four)
  uint8_t raw[4];
  if (!m.fetch(s_.pc, ByteView(raw, 4))) return fail(14, s_.pc, 0x10);
  const uint32_t insn = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
  const Step r = execute(m, insn);
  s_.r[0] = 0;
  return r;
}

// --- instructions --------------------------------------------------------------------------------------------------------------------------
// The operand fields are in the same places in nearly every instruction: rd in bits 4:0, rj in 9:5, rk in 14:10, and immediates from bit 10 up.

LoongArch64Cpu::Step LoongArch64Cpu::execute(GuestMemory& m, uint32_t insn) {
  const uint32_t rd = field(insn, 4, 0), rj = field(insn, 9, 5);
  const uint64_t a = s_.r[rj];
  const uint64_t next = s_.pc + 4;
  const uint32_t major = insn >> 26;

  switch (major) {
    case 0x00: {
      const uint32_t op10 = insn >> 22;
      if (op10 >= 0x08) {      // an operation with a 12-bit immediate
        const uint64_t si12 = static_cast<uint64_t>(sext(insn >> 10, 12)), ui12 = field(insn, 21, 10);
        uint64_t value;
        switch (op10) {
          case 0x08: value = static_cast<int64_t>(a) < static_cast<int64_t>(si12); break;      // slti
          case 0x09: value = a < si12; break;                                                    // sltui
          case 0x0A: value = sext32(a + si12); break;                                            // addi.w
          case 0x0B: value = a + si12; break;                                                    // addi.d
          case 0x0C: value = (a & 0x000FFFFFFFFFFFFFull) | (ui12 << 52); break;                 // lu52i.d
          case 0x0D: value = a & ui12; break;                                                    // andi
          case 0x0E: value = a | ui12; break;                                                    // ori
          case 0x0F: value = a ^ ui12; break;                                                    // xori
          default: return undefined();      // (0x10 and up: the CSR, cache, TLB and other privileged instructions)
        }
        set_r(rd, value);
        s_.pc = next;
        return Step::Ok;
      }
      if (op10 >= 0x04) {      // 0x01......: floating point
        const Step r = floating(insn);
        if (r == Step::Ok) s_.pc = next;
        return r;
      }
      if (insn == 0x002B0000u || (insn & 0xFFFF8000u) == 0x002B0000u) {      // syscall (its code is not looked at, as on Linux)
        s_.pc = next;
        return Step::Syscall;
      }
      const Step r = register_operation(insn);
      if (r == Step::Ok) s_.pc = next;
      return r;
    }
    case 0x02: case 0x03: {      // fmadd and friends; fcmp, fsel
      const Step r = floating_fused(insn);
      if (r == Step::Ok) s_.pc = next;
      return r;
    }
    case 0x04:      // addu16i.d
      set_r(rd, a + (static_cast<uint64_t>(sext(insn >> 10, 16)) << 16));
      break;
    case 0x05: case 0x06: case 0x07: {
      const uint64_t si20 = static_cast<uint64_t>(sext(insn >> 5, 20));
      switch (insn >> 25) {
        case 0x0A: set_r(rd, si20 << 12); break;                                                     // lu12i.w
        case 0x0B: set_r(rd, (s_.r[rd] & 0xFFFFFFFFu) | (si20 << 32)); break;                        // lu32i.d
        case 0x0C: set_r(rd, s_.pc + (si20 << 2)); break;                                            // pcaddi
        case 0x0D: set_r(rd, (s_.pc + (si20 << 12)) & ~uint64_t{0xFFF}); break;                      // pcalau12i
        case 0x0E: set_r(rd, s_.pc + (si20 << 12)); break;                                           // pcaddu12i
        default: set_r(rd, s_.pc + (si20 << 18)); break;                                             // pcaddu18i
      }
      break;
    }
    case 0x08: case 0x09: {      // ll, sc, ldptr, stptr: a 14-bit offset in words
      const uint64_t address = a + (static_cast<uint64_t>(sext(insn >> 10, 14)) << 2);
      const uint32_t op = insn >> 24;
      const uint32_t size = (op & 2) ? 8 : 4;
      if (op == 0x20 || op == 0x22 || op == 0x24 || op == 0x26) {      // ll.w, ll.d, ldptr.w, ldptr.d
        uint64_t value;
        if (!load(m, address, size, &value)) return Step::Fault;
        if (size == 4) value = sext32(value);
        if (op < 0x24) {
          reservation_valid_ = true;
          reservation_address_ = address;
          reservation_value_ = value;
        }
        set_r(rd, value);
      } else if (op == 0x25 || op == 0x27) {      // stptr.w, stptr.d
        if (!store(m, address, size, s_.r[rd])) return Step::Fault;
      } else {      // sc.w, sc.d: stores if the reservation still stands and the word still holds what LL read
        bool stored = false;
        if (reservation_valid_ && reservation_address_ == address) {
          uint64_t now;
          if (!load(m, address, size, &now)) return Step::Fault;
          if (size == 4) now = sext32(now);
          if (now == reservation_value_) {
            if (!store(m, address, size, s_.r[rd])) return Step::Fault;
            stored = true;
          }
        }
        reservation_valid_ = false;
        set_r(rd, stored);
      }
      break;
    }
    case 0x0A: case 0x0B: {      // loads and stores with a 12-bit offset
      const uint32_t op = insn >> 22;
      const uint64_t address = a + static_cast<uint64_t>(sext(insn >> 10, 12));
      uint64_t value;
      switch (op) {
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: {      // ld.b, ld.h, ld.w, ld.d
          const uint32_t size = 1u << (op - 0xA0);
          if (!load(m, address, size, &value)) return Step::Fault;
          set_r(rd, static_cast<uint64_t>(sext(value, size * 8)));
          break;
        }
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:      // st.b, st.h, st.w, st.d
          if (!store(m, address, 1u << (op - 0xA4), s_.r[rd])) return Step::Fault;
          break;
        case 0xA8: case 0xA9: case 0xAA:      // ld.bu, ld.hu, ld.wu
          if (!load(m, address, 1u << (op - 0xA8), &value)) return Step::Fault;
          set_r(rd, value);
          break;
        case 0xAB: break;      // preld: a hint
        case 0xAC:      // fld.s
          if (!load(m, address, 4, &value)) return Step::Fault;
          write_single(rd, value);
          break;
        case 0xAD:      // fst.s
          if (!store(m, address, 4, s_.f[rd])) return Step::Fault;
          break;
        case 0xAE:      // fld.d
          if (!load(m, address, 8, &value)) return Step::Fault;
          s_.f[rd] = value;
          break;
        case 0xAF:      // fst.d
          if (!store(m, address, 8, s_.f[rd])) return Step::Fault;
          break;
        default: return undefined();      // (the vector loads and stores)
      }
      break;
    }
    case 0x0E: {
      const Step r = memory_indexed(m, insn);
      if (r == Step::Ok) s_.pc = next;
      return r;
    }
    case 0x10: case 0x11: {      // beqz, bnez: a 21-bit offset in words
      const uint64_t offset = static_cast<uint64_t>(sext((uint64_t{field(insn, 4, 0)} << 16) | field(insn, 25, 10), 21)) << 2;
      s_.pc = ((a == 0) == (major == 0x10)) ? s_.pc + offset : next;
      return Step::Ok;
    }
    case 0x12: {      // bceqz, bcnez
      const uint32_t kind = field(insn, 9, 8);
      if (kind > 1) return undefined();
      const uint64_t offset = static_cast<uint64_t>(sext((uint64_t{field(insn, 4, 0)} << 16) | field(insn, 25, 10), 21)) << 2;
      s_.pc = ((s_.fcc[field(insn, 7, 5)] == 0) == (kind == 0)) ? s_.pc + offset : next;
      return Step::Ok;
    }
    case 0x13: {      // jirl
      const uint64_t target = a + (static_cast<uint64_t>(sext(insn >> 10, 16)) << 2);
      set_r(rd, next);
      s_.pc = target;
      return Step::Ok;
    }
    case 0x14: case 0x15: {      // b, bl: a 26-bit offset in words
      const uint64_t offset = static_cast<uint64_t>(sext((uint64_t{field(insn, 9, 0)} << 16) | field(insn, 25, 10), 26)) << 2;
      if (major == 0x15) s_.r[1] = next;
      s_.pc += offset;
      return Step::Ok;
    }
    case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: {      // beq, bne, blt, bge, bltu, bgeu: rj against rd
      const uint64_t b = s_.r[rd];
      bool taken;
      switch (major) {
        case 0x16: taken = a == b; break;
        case 0x17: taken = a != b; break;
        case 0x18: taken = static_cast<int64_t>(a) < static_cast<int64_t>(b); break;
        case 0x19: taken = static_cast<int64_t>(a) >= static_cast<int64_t>(b); break;
        case 0x1A: taken = a < b; break;
        default: taken = a >= b; break;
      }
      s_.pc = taken ? s_.pc + (static_cast<uint64_t>(sext(insn >> 10, 16)) << 2) : next;
      return Step::Ok;
    }
    default: return undefined();      // (0x1C and up: the vector extensions)
  }
  s_.pc = next;
  return Step::Ok;
}

LoongArch64Cpu::Step LoongArch64Cpu::register_operation(uint32_t insn) {
  const uint32_t rd = field(insn, 4, 0), rj = field(insn, 9, 5), rk = field(insn, 14, 10);
  const uint64_t a = s_.r[rj], b = s_.r[rk];
  const uint32_t op17 = insn >> 15;      // the opcode of an instruction with three registers
  uint64_t value = 0;

  if ((insn >> 22) != 0) {      // 0x004..... to 0x00F.....: bit strings and shifts by a constant
    const uint32_t op11 = insn >> 21;
    if (op11 == 0x003) {      // bstrins.w, bstrpick.w: bits msb..lsb of the low word
      if (insn & (1u << 15)) {
        const uint32_t msb = field(insn, 20, 16), lsb = field(insn, 14, 10);
        if (lsb > msb) return undefined();
        const uint64_t mask = (uint64_t{2} << (msb - lsb)) - 1;
        set_r(rd, sext32((a >> lsb) & mask));      // bstrpick.w
        return Step::Ok;
      }
      const uint32_t msb = field(insn, 20, 16), lsb = field(insn, 14, 10);
      if (lsb > msb) return undefined();
      const uint64_t mask = ((uint64_t{2} << (msb - lsb)) - 1) << lsb;
      set_r(rd, sext32((s_.r[rd] & ~mask) | ((a << lsb) & mask)));      // bstrins.w
      return Step::Ok;
    }
    if ((insn >> 22) == 0x002 || (insn >> 22) == 0x003) {      // bstrins.d, bstrpick.d
      const uint32_t msb = field(insn, 21, 16), lsb = field(insn, 15, 10);
      if (lsb > msb) return undefined();
      const uint64_t ones = msb - lsb == 63 ? ~uint64_t{0} : (uint64_t{1} << (msb - lsb + 1)) - 1;
      if ((insn >> 22) == 0x003) set_r(rd, (a >> lsb) & ones);
      else set_r(rd, (s_.r[rd] & ~(ones << lsb)) | ((a & ones) << lsb));
      return Step::Ok;
    }
    const uint32_t amount = field(insn, 15, 10);
    switch (insn >> 16) {      // (a word shift has bit 15 set and a 5-bit amount; a doubleword shift a 6-bit one)
      case 0x0040: if (!(insn & 0x8000)) return undefined(); value = sext32(a << (amount & 31)); break;                                      // slli.w
      case 0x0041: value = a << amount; break;                                                                                             // slli.d
      case 0x0044: if (!(insn & 0x8000)) return undefined(); value = sext32((a & 0xFFFFFFFFu) >> (amount & 31)); break;                      // srli.w
      case 0x0045: value = a >> amount; break;                                                                                             // srli.d
      case 0x0048: if (!(insn & 0x8000)) return undefined(); value = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) >> (amount & 31)); break;      // srai.w
      case 0x0049: value = static_cast<uint64_t>(static_cast<int64_t>(a) >> amount); break;                                                // srai.d
      case 0x004C: {                                                                                                                       // rotri.w
        if (!(insn & 0x8000)) return undefined();
        const uint32_t w = static_cast<uint32_t>(a), n = amount & 31;
        value = sext32(n ? (w >> n) | (w << (32 - n)) : w);
        break;
      }
      case 0x004D: value = amount ? (a >> amount) | (a << (64 - amount)) : a; break;                                                      // rotri.d
      default: return undefined();
    }
    set_r(rd, value);
    return Step::Ok;
  }

  if (op17 == 0) {      // two registers
    if (rk >= 0x1C) return undefined();
    switch (insn >> 10) {
      case 0x04: value = count_leading_zeros(~a & 0xFFFFFFFFu, 32); break;      // clo.w
      case 0x05: value = count_leading_zeros(a & 0xFFFFFFFFu, 32); break;       // clz.w
      case 0x06: value = count_trailing_zeros(~a & 0xFFFFFFFFu, 32); break;     // cto.w
      case 0x07: value = count_trailing_zeros(a & 0xFFFFFFFFu, 32); break;      // ctz.w
      case 0x08: value = count_leading_zeros(~a, 64); break;                    // clo.d
      case 0x09: value = count_leading_zeros(a, 64); break;                     // clz.d
      case 0x0A: value = count_trailing_zeros(~a, 64); break;                   // cto.d
      case 0x0B: value = count_trailing_zeros(a, 64); break;                    // ctz.d
      case 0x0C: value = sext32(per_piece(a, 32, 16, [](uint64_t v) { return swap_bytes(v, 2); })); break;      // revb.2h
      case 0x0D: value = per_piece(a, 64, 16, [](uint64_t v) { return swap_bytes(v, 2); }); break;              // revb.4h
      case 0x0E: value = per_piece(a, 64, 32, [](uint64_t v) { return swap_bytes(v, 4); }); break;              // revb.2w
      case 0x0F: value = swap_bytes(a, 8); break;                                                                // revb.d
      case 0x10: value = per_piece(a, 64, 32, [](uint64_t v) { return (v >> 16) | (v << 16); }); break;         // revh.2w
      case 0x11: value = (a >> 48) | ((a >> 16) & 0xFFFF0000u) | ((a << 16) & 0xFFFF00000000ull) | (a << 48); break;      // revh.d
      case 0x12: value = sext32(per_piece(a, 32, 8, [](uint64_t v) { return reverse_bits(v, 8); })); break;     // bitrev.4b
      case 0x13: value = per_piece(a, 64, 8, [](uint64_t v) { return reverse_bits(v, 8); }); break;             // bitrev.8b
      case 0x14: value = sext32(reverse_bits(a & 0xFFFFFFFFu, 32)); break;                                       // bitrev.w
      case 0x15: value = reverse_bits(a, 64); break;                                                             // bitrev.d
      case 0x16: value = static_cast<uint64_t>(sext(a, 16)); break;             // ext.w.h
      case 0x17: value = static_cast<uint64_t>(sext(a, 8)); break;              // ext.w.b
      case 0x18: case 0x19: case 0x1A:      // rdtimel.w, rdtimeh.w, rdtime.d: the instruction count stands in for the stable counter; rj gets the counter's id
        value = (insn >> 10) == 0x18 ? sext32(retired_) : (insn >> 10) == 0x19 ? sext32(retired_ >> 32) : retired_;
        set_r(rj, 0);
        break;
      case 0x1B: value = cpu_configuration(a); break;                           // cpucfg
      default: return undefined();
    }
    set_r(rd, value);
    return Step::Ok;
  }

  switch (op17) {
    case 0x002: case 0x003:      // asrtle.d, asrtgt.d: a bound check
      if (rd != 0) return undefined();
      if (op17 == 0x002 ? a > b : a <= b) return fail(5);
      return Step::Ok;
    case 0x008: case 0x009: case 0x00A: case 0x00B: {      // alsl.w: (rj << sa) + rk
      const uint32_t sa = field(insn, 16, 15) + 1;
      set_r(rd, sext32((a << sa) + b));
      return Step::Ok;
    }
    case 0x00C: case 0x00D: case 0x00E: case 0x00F: {      // alsl.wu
      const uint32_t sa = field(insn, 16, 15) + 1;
      set_r(rd, ((a << sa) + b) & 0xFFFFFFFFu);
      return Step::Ok;
    }
    case 0x010: case 0x011: case 0x012: case 0x013: {      // bytepick.w: bytes out of rk and rj side by side
      const uint32_t sa = field(insn, 16, 15) * 8;
      set_r(rd, sext32(sa ? (b << sa) | ((a & 0xFFFFFFFFu) >> (32 - sa)) : b));
      return Step::Ok;
    }
    case 0x058: case 0x059: case 0x05A: case 0x05B: {      // alsl.d
      const uint32_t sa = field(insn, 16, 15) + 1;
      set_r(rd, (a << sa) + b);
      return Step::Ok;
    }
    default: break;
  }
  if ((insn >> 18) == 0x003) {      // bytepick.d
    const uint32_t sa = field(insn, 17, 15) * 8;
    set_r(rd, sa ? (b << sa) | (a >> (64 - sa)) : b);
    return Step::Ok;
  }

  const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
  const int32_t wa = static_cast<int32_t>(a), wb = static_cast<int32_t>(b);
  const uint32_t ua = static_cast<uint32_t>(a), ub = static_cast<uint32_t>(b);
  switch (op17) {
    case 0x020: value = sext32(a + b); break;                     // add.w
    case 0x021: value = a + b; break;                             // add.d
    case 0x022: value = sext32(a - b); break;                     // sub.w
    case 0x023: value = a - b; break;                             // sub.d
    case 0x024: value = sa < sb; break;                           // slt
    case 0x025: value = a < b; break;                             // sltu
    case 0x026: value = b == 0 ? 0 : a; break;                    // maskeqz
    case 0x027: value = b != 0 ? 0 : a; break;                    // masknez
    case 0x028: value = ~(a | b); break;                          // nor
    case 0x029: value = a & b; break;                             // and
    case 0x02A: value = a | b; break;                             // or
    case 0x02B: value = a ^ b; break;                             // xor
    case 0x02C: value = a | ~b; break;                            // orn
    case 0x02D: value = a & ~b; break;                            // andn
    case 0x02E: value = sext32(a << (b & 31)); break;             // sll.w
    case 0x02F: value = sext32(ua >> (b & 31)); break;            // srl.w
    case 0x030: value = static_cast<uint64_t>(static_cast<int64_t>(wa >> (b & 31))); break;      // sra.w
    case 0x031: value = a << (b & 63); break;                     // sll.d
    case 0x032: value = a >> (b & 63); break;                     // srl.d
    case 0x033: value = static_cast<uint64_t>(sa >> (b & 63)); break;                            // sra.d
    case 0x036: {                                                 // rotr.w
      const uint32_t n = b & 31;
      value = sext32(n ? (ua >> n) | (ua << (32 - n)) : ua);
      break;
    }
    case 0x037: {                                                 // rotr.d
      const uint32_t n = b & 63;
      value = n ? (a >> n) | (a << (64 - n)) : a;
      break;
    }
    case 0x038: value = sext32(uint64_t{ua} * ub); break;         // mul.w
    case 0x039: value = sext32(static_cast<uint64_t>(int64_t{wa} * int64_t{wb}) >> 32); break;      // mulh.w
    case 0x03A: value = sext32((uint64_t{ua} * ub) >> 32); break; // mulh.wu
    case 0x03B: value = a * b; break;                             // mul.d
    case 0x03C: value = multiply_high_signed(a, b); break;        // mulh.d
    case 0x03D: value = multiply_high_unsigned(a, b); break;      // mulh.du
    case 0x03E: value = static_cast<uint64_t>(int64_t{wa} * int64_t{wb}); break;                   // mulw.d.w
    case 0x03F: value = uint64_t{ua} * ub; break;                 // mulw.d.wu
    // Division by zero (and the one signed quotient that does not fit) traps nowhere and its result is not defined; a compiler puts `break 7` behind a
    // division where the language wants a trap. What is computed here is what the quotient and remainder by one are.
    case 0x040: value = (wb == 0 || (wa == INT32_MIN && wb == -1)) ? sext32(ua) : static_cast<uint64_t>(static_cast<int64_t>(wa / wb)); break;      // div.w
    case 0x041: value = (wb == 0 || (wa == INT32_MIN && wb == -1)) ? 0 : static_cast<uint64_t>(static_cast<int64_t>(wa % wb)); break;                // mod.w
    case 0x042: value = ub == 0 ? sext32(ua) : sext32(ua / ub); break;      // div.wu
    case 0x043: value = ub == 0 ? 0 : sext32(ua % ub); break;               // mod.wu
    case 0x044: value = (sb == 0 || (sa == INT64_MIN && sb == -1)) ? a : static_cast<uint64_t>(sa / sb); break;      // div.d
    case 0x045: value = (sb == 0 || (sa == INT64_MIN && sb == -1)) ? 0 : static_cast<uint64_t>(sa % sb); break;      // mod.d
    case 0x046: value = b == 0 ? a : a / b; break;                // div.du
    case 0x047: value = b == 0 ? 0 : a % b; break;                // mod.du
    case 0x048: case 0x049: case 0x04A: case 0x04B: value = crc32(b, a, 1u << (op17 - 0x048), 0xEDB88320u); break;      // crc.w.{b,h,w,d}.w
    case 0x04C: case 0x04D: case 0x04E: case 0x04F: value = crc32(b, a, 1u << (op17 - 0x04C), 0x82F63B78u); break;      // crcc.w.{b,h,w,d}.w
    case 0x054:      // break: 7 is what compilers put behind a division by zero (SIGFPE on Linux); the rest are breakpoints
      return fail((insn & 0x7FFF) == 7 ? 0 : 3);
    default: return undefined();
  }
  set_r(rd, value);
  return Step::Ok;
}

LoongArch64Cpu::Step LoongArch64Cpu::memory_indexed(GuestMemory& m, uint32_t insn) {
  const uint32_t rd = field(insn, 4, 0), rj = field(insn, 9, 5), rk = field(insn, 14, 10);
  const uint64_t a = s_.r[rj], b = s_.r[rk];
  const uint32_t op17 = insn >> 15;
  uint64_t value;

  if (op17 < 0x7080 && (insn & 0x38000) == 0) {      // 0x38000000 to 0x383C0000 in steps of 0x40000: an address of rj + rk
    const uint64_t address = a + b;
    const uint32_t kind = field(insn, 21, 18);
    switch (kind) {
      case 0: case 1: case 2: case 3:      // ldx.b, ldx.h, ldx.w, ldx.d
        if (!load(m, address, 1u << kind, &value)) return Step::Fault;
        set_r(rd, static_cast<uint64_t>(sext(value, 8u << kind)));
        return Step::Ok;
      case 4: case 5: case 6: case 7:      // stx.b, stx.h, stx.w, stx.d
        return store(m, address, 1u << (kind - 4), s_.r[rd]) ? Step::Ok : Step::Fault;
      case 8: case 9: case 10:      // ldx.bu, ldx.hu, ldx.wu
        if (!load(m, address, 1u << (kind - 8), &value)) return Step::Fault;
        set_r(rd, value);
        return Step::Ok;
      case 11: return Step::Ok;      // preldx
      case 12: case 13:      // fldx.s, fldx.d
        if (!load(m, address, kind == 12 ? 4 : 8, &value)) return Step::Fault;
        if (kind == 12) write_single(rd, value);
        else s_.f[rd] = value;
        return Step::Ok;
      default:      // fstx.s, fstx.d
        return store(m, address, kind == 14 ? 4 : 8, s_.f[rd]) ? Step::Ok : Step::Fault;
    }
  }
  if (op17 >= 0x70B0 && op17 <= 0x70E3) return atomic(m, insn);
  if (op17 == 0x70E4 || op17 == 0x70E5) return Step::Ok;      // dbar, ibar: nothing to order here, the threads take turns
  if (op17 >= 0x70E8 && op17 <= 0x70FF) {
    // The loads and stores with a bound check: the address is rj alone, and it has to be above rk ("gt") or not above it ("le").
    const bool fp = op17 < 0x70F0;
    const uint32_t kind = fp ? (op17 - 0x70E8) : (op17 - 0x70F0);      // fp: {ld gt, ld le, st gt, st le} x {s, d}; integer: the same x {b, h, w, d}
    const uint32_t group = fp ? kind >> 1 : kind >> 2;
    const uint32_t size = fp ? (kind & 1 ? 8 : 4) : 1u << (kind & 3);
    const bool wants_above = (group & 1) == 0, is_store = group >= 2;
    if ((a > b) != wants_above) return fail(5);
    if (is_store) return store(m, a, size, fp ? s_.f[rd] : s_.r[rd]) ? Step::Ok : Step::Fault;
    if (!load(m, a, size, &value)) return Step::Fault;
    if (!fp) set_r(rd, static_cast<uint64_t>(sext(value, size * 8)));
    else if (size == 4) write_single(rd, value);
    else s_.f[rd] = value;
    return Step::Ok;
  }
  return undefined();      // (the vector indexed loads and stores, sc.q, llacq and screl)
}

// amswap, amadd, amand, amor, amxor, ammax, ammin (and the unsigned two), each for words and doublewords, with and without a barrier (which is nothing here);
// amswap and amadd for bytes and halfwords; amcas for all four sizes. rd gets what the memory at rj held, sign-extended.
LoongArch64Cpu::Step LoongArch64Cpu::atomic(GuestMemory& m, uint32_t insn) {
  const uint32_t rd = field(insn, 4, 0), rj = field(insn, 9, 5), rk = field(insn, 14, 10);
  const uint32_t op17 = insn >> 15;
  const uint64_t address = s_.r[rj], operand = s_.r[rk];
  uint32_t size;
  enum class Kind : uint8_t { Swap, Add, And, Or, Xor, Max, Min, MaxUnsigned, MinUnsigned, CompareSwap } kind;
  if (op17 >= 0x70B0 && op17 <= 0x70B7) {      // amcas, amcas_db: b, h, w, d
    kind = Kind::CompareSwap;
    size = 1u << ((op17 - 0x70B0) & 3);
  } else if (op17 >= 0x70B8 && op17 <= 0x70BF) {      // amswap.b/h, amadd.b/h, and the two again with a barrier
    kind = ((op17 - 0x70B8) & 2) ? Kind::Add : Kind::Swap;
    size = 1u << ((op17 - 0x70B8) & 1);
  } else if (op17 >= 0x70C0 && op17 <= 0x70E3) {
    const uint32_t index = (op17 - 0x70C0) % 18;      // (the nine operations for w and d, then the same with a barrier)
    kind = static_cast<Kind>(index >> 1);
    size = (index & 1) ? 8 : 4;
  } else {
    return undefined();
  }
  // (the instruction is not defined when rd is also one of its sources; processors refuse it)
  if (kind != Kind::CompareSwap && rd != 0 && (rd == rj || rd == rk)) return undefined();
  uint64_t old;
  if (!load(m, address, size, &old)) return Step::Fault;
  const uint64_t extended = static_cast<uint64_t>(sext(old, size * 8));
  const int64_t so = static_cast<int64_t>(extended), sv = sext(operand, size * 8);
  const uint64_t mask = size == 8 ? ~uint64_t{0} : (uint64_t{1} << (size * 8)) - 1;
  uint64_t value = operand;
  bool writes = true;
  switch (kind) {
    case Kind::Swap: break;
    case Kind::Add: value = old + operand; break;
    case Kind::And: value = old & operand; break;
    case Kind::Or: value = old | operand; break;
    case Kind::Xor: value = old ^ operand; break;
    case Kind::Max: value = so > sv ? old : operand; break;
    case Kind::Min: value = so < sv ? old : operand; break;
    case Kind::MaxUnsigned: value = (old & mask) > (operand & mask) ? old : operand; break;
    case Kind::MinUnsigned: value = (old & mask) < (operand & mask) ? old : operand; break;
    case Kind::CompareSwap: writes = (old & mask) == (s_.r[rd] & mask); break;
  }
  // (the store's permission is asked for even when a compare-and-swap leaves the memory alone, as the processor does)
  if (!store(m, address, size, writes ? value : old)) return Step::Fault;
  set_r(rd, extended);
  return Step::Ok;
}

}  // namespace emulation
