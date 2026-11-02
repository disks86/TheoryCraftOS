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
#include "Emulation/RiscV64Cpu.hpp"
#include "Emulation/RiscV64Crypto.hpp"
#include "Emulation/RiscV64VectorUnit.hpp"
#include "Emulation/Wide.hpp"

namespace emulation {

namespace {

constexpr int64_t sext(uint64_t v, uint32_t bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }
constexpr uint32_t bit(uint32_t v, uint32_t i) { return (v >> i) & 1; }
constexpr uint32_t field(uint32_t v, uint32_t hi, uint32_t lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

// --- the 32-bit encodings the compressed instructions stand for -----------------------------------------------------------------------------------
uint32_t enc_r(uint32_t op, uint32_t f3, uint32_t f7, uint32_t rd, uint32_t rs1, uint32_t rs2) { return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op; }
uint32_t enc_i(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int64_t imm) { return ((static_cast<uint32_t>(imm) & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op; }
uint32_t enc_s(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int64_t imm) {
  const uint32_t i = static_cast<uint32_t>(imm) & 0xFFF;
  return ((i >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((i & 31) << 7) | op;
}
uint32_t enc_b(uint32_t f3, uint32_t rs1, uint32_t rs2, int64_t imm) {
  const uint32_t i = static_cast<uint32_t>(imm);
  return (bit(i, 12) << 31) | (field(i, 10, 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (field(i, 4, 1) << 8) | (bit(i, 11) << 7) | 0x63;
}
uint32_t enc_j(uint32_t rd, int64_t imm) {
  const uint32_t i = static_cast<uint32_t>(imm);
  return (bit(i, 20) << 31) | (field(i, 10, 1) << 21) | (bit(i, 11) << 20) | (field(i, 19, 12) << 12) | (rd << 7) | 0x6F;
}

constexpr uint32_t kLoad = 0x03, kLoadFp = 0x07, kOpImm = 0x13, kOpImm32 = 0x1B, kStore = 0x23, kStoreFp = 0x27, kOp = 0x33, kLui = 0x37, kOp32 = 0x3B, kJalr = 0x67;

// Returns 0 for an illegal or reserved compressed instruction.
uint32_t expand_compressed(uint32_t c) {
  const uint32_t funct3 = field(c, 15, 13), quadrant = c & 3;
  const uint32_t rd_full = field(c, 11, 7), rs2_full = field(c, 6, 2);
  const uint32_t rd_p = field(c, 4, 2) + 8, rs1_p = field(c, 9, 7) + 8, rs2_p = field(c, 4, 2) + 8;
  const int64_t imm6 = sext((bit(c, 12) << 5) | field(c, 6, 2), 6);
  const uint32_t shamt = (bit(c, 12) << 5) | field(c, 6, 2);
  switch (quadrant) {
    case 0: {
      const uint32_t lw_off = (field(c, 12, 10) << 3) | (bit(c, 6) << 2) | (bit(c, 5) << 6);
      const uint32_t ld_off = (field(c, 12, 10) << 3) | (field(c, 6, 5) << 6);
      switch (funct3) {
        case 0: {
          const uint32_t imm = (field(c, 10, 7) << 6) | (field(c, 12, 11) << 4) | (bit(c, 5) << 3) | (bit(c, 6) << 2);
          if (imm == 0) return 0;
          return enc_i(kOpImm, 0, rd_p, 2, imm);
        }
        case 1: return enc_i(kLoadFp, 3, rd_p, rs1_p, ld_off);
        case 2: return enc_i(kLoad, 2, rd_p, rs1_p, lw_off);
        case 3: return enc_i(kLoad, 3, rd_p, rs1_p, ld_off);
        case 4: {      // Zcb: c.lbu, c.lhu, c.lh, c.sb, c.sh
          const uint32_t byte_off = (bit(c, 5) << 1) | bit(c, 6), half_off = bit(c, 5) << 1;
          switch (field(c, 12, 10)) {
            case 0: return enc_i(kLoad, 4, rd_p, rs1_p, byte_off);      // c.lbu
            case 1: return enc_i(kLoad, bit(c, 6) ? 1 : 5, rd_p, rs1_p, half_off);      // c.lh, c.lhu
            case 2: return enc_s(kStore, 0, rs1_p, rs2_p, byte_off);      // c.sb
            case 3: return bit(c, 6) ? 0 : enc_s(kStore, 1, rs1_p, rs2_p, half_off);      // c.sh
            default: return 0;
          }
        }
        case 5: return enc_s(kStoreFp, 3, rs1_p, rs2_p, ld_off);
        case 6: return enc_s(kStore, 2, rs1_p, rs2_p, lw_off);
        case 7: return enc_s(kStore, 3, rs1_p, rs2_p, ld_off);
        default: return 0;
      }
    }
    case 1:
      switch (funct3) {
        case 0: return enc_i(kOpImm, 0, rd_full, rd_full, imm6);      // c.addi (c.nop)
        case 1: return rd_full == 0 ? 0 : enc_i(kOpImm32, 0, rd_full, rd_full, imm6);      // c.addiw
        case 2: return enc_i(kOpImm, 0, rd_full, 0, imm6);      // c.li
        case 3: {
          if (rd_full == 2) {      // c.addi16sp
            const int64_t imm = sext((bit(c, 12) << 9) | (field(c, 4, 3) << 7) | (bit(c, 5) << 6) | (bit(c, 2) << 5) | (bit(c, 6) << 4), 10);
            if (imm == 0) return 0;
            return enc_i(kOpImm, 0, 2, 2, imm);
          }
          if (imm6 == 0) return (rd_full & 1) && rd_full < 16 ? enc_i(kOpImm, 0, 0, 0, 0) : 0;      // c.lui with a zero immediate is reserved, except as Zcmop's c.mop.n (a no-operation)
          return (static_cast<uint32_t>(imm6) << 12 & 0xFFFFF000u) | (rd_full << 7) | kLui;
        }
        case 4: {
          const uint32_t kind = field(c, 11, 10);
          if (kind == 0) return enc_i(kOpImm, 5, rs1_p, rs1_p, shamt);      // c.srli
          if (kind == 1) return enc_i(kOpImm, 5, rs1_p, rs1_p, shamt | 0x400);      // c.srai
          if (kind == 2) return enc_i(kOpImm, 7, rs1_p, rs1_p, imm6);      // c.andi
          const uint32_t sub = field(c, 6, 5);
          if (!bit(c, 12)) {
            static const uint32_t f3[4] = {0, 4, 6, 7}, f7[4] = {0x20, 0, 0, 0};      // sub xor or and
            return enc_r(kOp, f3[sub], f7[sub], rs1_p, rs1_p, rs2_p);
          }
          if (sub == 0) return enc_r(kOp32, 0, 0x20, rs1_p, rs1_p, rs2_p);      // c.subw
          if (sub == 1) return enc_r(kOp32, 0, 0, rs1_p, rs1_p, rs2_p);      // c.addw
          if (sub == 2) return enc_r(kOp, 0, 1, rs1_p, rs1_p, rs2_p);      // Zcb: c.mul
          switch (field(c, 4, 2)) {
            case 0: return enc_i(kOpImm, 7, rs1_p, rs1_p, 255);      // c.zext.b
            case 1: return enc_i(kOpImm, 1, rs1_p, rs1_p, 0x604);      // c.sext.b
            case 2: return enc_r(kOp32, 4, 0x04, rs1_p, rs1_p, 0);      // c.zext.h
            case 3: return enc_i(kOpImm, 1, rs1_p, rs1_p, 0x605);      // c.sext.h
            case 4: return enc_r(kOp32, 0, 0x04, rs1_p, rs1_p, 0);      // c.zext.w (add.uw rd, rd, zero)
            case 5: return enc_i(kOpImm, 4, rs1_p, rs1_p, -1);      // c.not
            default: return 0;
          }
        }
        case 5: {      // c.j
          const int64_t off = sext((bit(c, 12) << 11) | (bit(c, 8) << 10) | (field(c, 10, 9) << 8) | (bit(c, 6) << 7) | (bit(c, 7) << 6) | (bit(c, 2) << 5) | (bit(c, 11) << 4) | (field(c, 5, 3) << 1), 12);
          return enc_j(0, off);
        }
        default: {      // c.beqz, c.bnez
          const int64_t off = sext((bit(c, 12) << 8) | (field(c, 6, 5) << 6) | (bit(c, 2) << 5) | (field(c, 11, 10) << 3) | (field(c, 4, 3) << 1), 9);
          return enc_b(funct3 == 6 ? 0 : 1, rs1_p, 0, off);
        }
      }
    default:      // quadrant 2
      switch (funct3) {
        case 0: return enc_i(kOpImm, 1, rd_full, rd_full, shamt);      // c.slli
        case 1: return enc_i(kLoadFp, 3, rd_full, 2, (bit(c, 12) << 5) | (field(c, 6, 5) << 3) | (field(c, 4, 2) << 6));      // c.fldsp
        case 2: return rd_full == 0 ? 0 : enc_i(kLoad, 2, rd_full, 2, (bit(c, 12) << 5) | (field(c, 6, 4) << 2) | (field(c, 3, 2) << 6));      // c.lwsp
        case 3: return rd_full == 0 ? 0 : enc_i(kLoad, 3, rd_full, 2, (bit(c, 12) << 5) | (field(c, 6, 5) << 3) | (field(c, 4, 2) << 6));      // c.ldsp
        case 4:
          if (!bit(c, 12)) {
            if (rs2_full == 0) return rd_full == 0 ? 0 : enc_i(kJalr, 0, 0, rd_full, 0);      // c.jr
            return enc_r(kOp, 0, 0, rd_full, 0, rs2_full);      // c.mv
          }
          if (rs2_full == 0) return rd_full == 0 ? 0x00100073u : enc_i(kJalr, 0, 1, rd_full, 0);      // c.ebreak, c.jalr
          return enc_r(kOp, 0, 0, rd_full, rd_full, rs2_full);      // c.add
        case 5: return enc_s(kStoreFp, 3, 2, rs2_full, (field(c, 12, 10) << 3) | (field(c, 9, 7) << 6));      // c.fsdsp
        case 6: return enc_s(kStore, 2, 2, rs2_full, (field(c, 12, 9) << 2) | (field(c, 8, 7) << 6));      // c.swsp
        default: return enc_s(kStore, 3, 2, rs2_full, (field(c, 12, 10) << 3) | (field(c, 9, 7) << 6));      // c.sdsp
      }
  }
}

}  // namespace

// --- state ---------------------------------------------------------------------------------------------------------------------------------

void RiscV64Cpu::reset(const process::ProgramStart& start) {
  s_ = State();
  s_.pc = start.entry;
  s_.x[2] = start.stack_pointer;
  s_.x[4] = start.fs_base;
  fault_ = GuestFault();
  retired_ = 0;
  reservation_valid_ = false;
}

bool RiscV64Cpu::pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const {
  if (abi != process::SyscallAbi::Linux) return false;      // (there is no RISC-V Windows interface)
  return process::decode_riscv64_linux(ArrayView<const uint64_t>(s_.x, 32), out);
}

RiscV64Cpu::Step RiscV64Cpu::fail(uint8_t vector, uint64_t address) {
  if (bus_) return sys_fail(vector, address);
  fault_.vector = vector;
  fault_.address = address;
  fault_.pc = instruction_start_;
  s_.pc = instruction_start_;
  return Step::Fault;
}

bool RiscV64Cpu::load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out) {
  if (bus_) return sys_read(address, size, amo_ ? Access::Store : Access::Load, out);
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

bool RiscV64Cpu::store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value) {
  if (bus_) return sys_write(address, size, value);
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  if (!m.write(address, ConstByteView(raw, size))) {
    fail(14, address);
    return false;
  }
  return true;
}

UniquePointer<IGuestCpu> RiscV64Cpu::clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const {
  UniquePointer<RiscV64Cpu> copy = UniquePointer<RiscV64Cpu>::make(allocator, *this);
  if (!copy) return UniquePointer<IGuestCpu>();
  copy->s_.x[10] = 0;
  if (stack_pointer) copy->s_.x[2] = stack_pointer;
  if (set_tls) copy->s_.x[4] = tls;
  copy->reservation_valid_ = false;
  copy->retired_ = 0;
  return UniquePointer<IGuestCpu>(static_cast<UniquePointer<RiscV64Cpu>&&>(copy));
}

StopReason RiscV64Cpu::run(GuestMemory& memory, uint64_t max_instructions) {
  for (uint64_t i = 0; i < max_instructions; ++i) {
    const Step r = step(memory);
    if (r == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (r == Step::Syscall) return StopReason::Syscall;
  }
  // Threads take turns: a turn never ends inside an LR..SC sequence (another thread could store to the word and the SC would still succeed, where real hardware loses
  // the reservation). A reservation still open after a generous allowance is dropped, which the architecture allows at any time (the SC then fails and the program retries).
  for (uint32_t extra = 0; reservation_valid_ && extra < 64; ++extra) {
    const Step r = step(memory);
    if (r == Step::Fault) return StopReason::Fault;
    ++retired_;
    if (r == Step::Syscall) return StopReason::Syscall;
  }
  reservation_valid_ = false;
  return StopReason::Budget;
}

RiscV64Cpu::Step RiscV64Cpu::step(GuestMemory& m) {
  instruction_start_ = s_.pc;
  if (bus_) {
    if (s_.pc & 1) return sys_fail(0, s_.pc);      // (an instruction address misaligned)
    uint64_t half, upper;
    if (!sys_read(s_.pc, 2, Access::Fetch, &half)) return Step::Fault;
    uint32_t insn = static_cast<uint32_t>(half);
    uint32_t length = 2;
    if ((insn & 3) == 3) {
      if (!sys_read(s_.pc + 2, 2, Access::Fetch, &upper)) return Step::Fault;
      insn |= static_cast<uint32_t>(upper) << 16;
      length = 4;
    }
    raw_instruction_ = insn;
    if (length == 2) {
      insn = expand_compressed(insn);
      if (insn == 0) return undefined();
    }
    return execute(m, insn, length);
  }
  if (s_.pc & 1) return fail(13, s_.pc);
  uint8_t raw[4];
  if (!m.fetch(s_.pc, ByteView(raw, 2))) return fail(14, s_.pc);
  uint32_t insn = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8);
  uint32_t length = 2;
  if ((insn & 3) == 3) {
    if (!m.fetch(s_.pc, ByteView(raw, 4))) return fail(14, s_.pc);
    insn |= (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
    length = 4;
  } else {
    insn = expand_compressed(insn);
    if (insn == 0) return undefined();
  }
  return execute(m, insn, length);
}

// --- B (Zba, Zbb, Zbs, Zbc) and Zicond ---------------------------------------------------------------------------------------------------

namespace {

uint64_t count_leading_zeros(uint64_t v, uint32_t width) {
  uint32_t n = 0;
  while (n < width && ((v >> (width - 1 - n)) & 1) == 0) ++n;
  return n;
}

uint64_t count_trailing_zeros(uint64_t v, uint32_t width) {
  uint32_t n = 0;
  while (n < width && ((v >> n) & 1) == 0) ++n;
  return n;
}

uint64_t population_count(uint64_t v) {
  uint64_t n = 0;
  for (; v != 0; v &= v - 1) ++n;
  return n;
}

uint64_t rotate_left(uint64_t v, uint32_t n, uint32_t width) {
  n %= width;
  const uint64_t mask = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
  v &= mask;
  return n == 0 ? v : ((v << n) | (v >> (width - n))) & mask;
}

uint64_t sign_extend_word(uint64_t v) {
  return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
}

// Claims the encodings that only the bit-manipulation and conditional-zero extensions use; the base integer paths handle the rest. Returns
// false when the instruction is not one of them.
bool bit_manipulation(uint32_t insn, uint64_t a, uint64_t b, uint64_t* out) {
  const uint32_t opcode = insn & 0x7F, funct3 = (insn >> 12) & 7, funct7 = insn >> 25, top = insn >> 20, shamt6 = (insn >> 20) & 63;
  uint64_t r;
  switch (opcode) {
    case 0x33:
      switch ((funct7 << 3) | funct3) {
        case (0x10 << 3) | 2: r = (a << 1) + b; break;      // sh1add
        case (0x10 << 3) | 4: r = (a << 2) + b; break;
        case (0x10 << 3) | 6: r = (a << 3) + b; break;
        case (0x20 << 3) | 7: r = a & ~b; break;      // andn
        case (0x20 << 3) | 6: r = a | ~b; break;      // orn
        case (0x20 << 3) | 4: r = ~(a ^ b); break;      // xnor
        case (0x05 << 3) | 4: r = static_cast<int64_t>(a) < static_cast<int64_t>(b) ? a : b; break;      // min
        case (0x05 << 3) | 5: r = a < b ? a : b; break;
        case (0x05 << 3) | 6: r = static_cast<int64_t>(a) < static_cast<int64_t>(b) ? b : a; break;
        case (0x05 << 3) | 7: r = a < b ? b : a; break;
        case (0x05 << 3) | 1: r = 0; for (uint32_t i = 0; i < 64; ++i) if ((b >> i) & 1) r ^= a << i; break;      // clmul
        case (0x05 << 3) | 2: r = 0; for (uint32_t i = 0; i < 64; ++i) if ((b >> i) & 1) r ^= a >> (63 - i); break;      // clmulr
        case (0x05 << 3) | 3: r = 0; for (uint32_t i = 1; i < 64; ++i) if ((b >> i) & 1) r ^= a >> (64 - i); break;      // clmulh
        case (0x30 << 3) | 1: r = rotate_left(a, static_cast<uint32_t>(b & 63), 64); break;      // rol
        case (0x30 << 3) | 5: r = rotate_left(a, static_cast<uint32_t>(64 - (b & 63)), 64); break;      // ror
        case (0x24 << 3) | 1: r = a & ~(uint64_t{1} << (b & 63)); break;      // bclr
        case (0x24 << 3) | 5: r = (a >> (b & 63)) & 1; break;      // bext
        case (0x34 << 3) | 1: r = a ^ (uint64_t{1} << (b & 63)); break;      // binv
        case (0x14 << 3) | 1: r = a | (uint64_t{1} << (b & 63)); break;      // bset
        case (0x07 << 3) | 5: r = b == 0 ? 0 : a; break;      // czero.eqz
        case (0x07 << 3) | 7: r = b != 0 ? 0 : a; break;      // czero.nez
        default: return false;
      }
      break;
    case 0x3B: {
      const uint32_t x = static_cast<uint32_t>(a);
      switch ((funct7 << 3) | funct3) {
        case (0x04 << 3) | 0: r = (a & 0xFFFFFFFFull) + b; break;      // add.uw
        case (0x04 << 3) | 4: if (((insn >> 20) & 31) != 0) return false; r = a & 0xFFFF; break;      // zext.h
        case (0x10 << 3) | 2: r = ((a & 0xFFFFFFFFull) << 1) + b; break;      // sh1add.uw
        case (0x10 << 3) | 4: r = ((a & 0xFFFFFFFFull) << 2) + b; break;
        case (0x10 << 3) | 6: r = ((a & 0xFFFFFFFFull) << 3) + b; break;
        case (0x30 << 3) | 1: r = sign_extend_word(rotate_left(x, static_cast<uint32_t>(b & 31), 32)); break;      // rolw
        case (0x30 << 3) | 5: r = sign_extend_word(rotate_left(x, static_cast<uint32_t>(32 - (b & 31)), 32)); break;      // rorw
        default: return false;
      }
      break;
    }
    case 0x13:
      if (funct3 == 1) {
        switch (insn >> 26) {
          case 0x0A: r = a | (uint64_t{1} << shamt6); break;      // bseti
          case 0x12: r = a & ~(uint64_t{1} << shamt6); break;      // bclri
          case 0x1A: r = a ^ (uint64_t{1} << shamt6); break;      // binvi
          case 0x18:
            switch (top) {
              case 0x600: r = count_leading_zeros(a, 64); break;      // clz
              case 0x601: r = count_trailing_zeros(a, 64); break;      // ctz
              case 0x602: r = population_count(a); break;      // cpop
              case 0x604: r = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(a))); break;      // sext.b
              case 0x605: r = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(a))); break;      // sext.h
              default: return false;
            }
            break;
          default: return false;
        }
      } else if (funct3 == 5) {
        if ((insn >> 26) == 0x12) r = (a >> shamt6) & 1;      // bexti
        else if ((insn >> 26) == 0x18 && top != 0x6B8 && top != 0x287) r = rotate_left(a, 64 - shamt6, 64);      // rori
        else if (top == 0x6B8) {      // rev8
          r = 0;
          for (uint32_t i = 0; i < 8; ++i) r |= ((a >> (i * 8)) & 0xFF) << ((7 - i) * 8);
        } else if (top == 0x287) {      // orc.b
          r = 0;
          for (uint32_t i = 0; i < 8; ++i) if ((a >> (i * 8)) & 0xFF) r |= uint64_t{0xFF} << (i * 8);
        } else return false;
      } else return false;
      break;
    case 0x1B:
      if (funct3 == 1) {
        if ((insn >> 26) == 0x02) r = (a & 0xFFFFFFFFull) << shamt6;      // slli.uw
        else if (top == 0x600) r = count_leading_zeros(a & 0xFFFFFFFFull, 32);      // clzw
        else if (top == 0x601) r = count_trailing_zeros(a & 0xFFFFFFFFull, 32);      // ctzw
        else if (top == 0x602) r = population_count(a & 0xFFFFFFFFull);      // cpopw
        else return false;
      } else if (funct3 == 5 && funct7 == 0x30) {      // roriw
        r = sign_extend_word(rotate_left(static_cast<uint32_t>(a), 32 - (shamt6 & 31), 32));
      } else return false;
      break;
    default: return false;
  }
  *out = r;
  return true;
}

}      // namespace

// --- the instructions ----------------------------------------------------------------------------------------------------------------------

RiscV64Cpu::Step RiscV64Cpu::execute(GuestMemory& m, uint32_t insn, uint32_t length) {
  const uint32_t opcode = insn & 0x7F, rd = field(insn, 11, 7), funct3 = field(insn, 14, 12), rs1 = field(insn, 19, 15), rs2 = field(insn, 24, 20), funct7 = insn >> 25;
  const int64_t imm_i = sext(insn >> 20, 12);
  const uint64_t a = s_.x[rs1], b = s_.x[rs2];
  uint64_t next = s_.pc + length;
  auto word = [](uint64_t v) { return static_cast<uint64_t>(sext(v, 32)); };
  if (opcode == kOp || opcode == kOp32 || opcode == kOpImm || opcode == kOpImm32) {
    uint64_t bits;
    if (bit_manipulation(insn, a, opcode == kOpImm || opcode == kOpImm32 ? 0 : b, &bits) || rvcrypto::scalar_crypto(insn, a, opcode == kOpImm || opcode == kOpImm32 ? 0 : b, &bits)) {
      set_x(rd, bits);
      s_.pc = next;
      return Step::Ok;
    }
  }
  const uint32_t unit = bus_ ? sys_unit(insn) : 0;      // system mode: the F/D and V units can be switched off (mstatus.FS, VS) and track whether they were written
  if (unit && !sys_unit_enabled(unit)) return undefined();
  switch (opcode) {
    case 0x37: set_x(rd, static_cast<uint64_t>(sext(insn & 0xFFFFF000u, 32))); break;      // lui
    case 0x17: set_x(rd, s_.pc + static_cast<uint64_t>(sext(insn & 0xFFFFF000u, 32))); break;      // auipc
    case 0x6F: {      // jal
      const int64_t off = sext((bit(insn, 31) << 20) | (field(insn, 19, 12) << 12) | (bit(insn, 20) << 11) | (field(insn, 30, 21) << 1), 21);
      set_x(rd, next);
      next = s_.pc + static_cast<uint64_t>(off);
      break;
    }
    case kJalr: {
      if (funct3 != 0) return undefined();
      const uint64_t target = (a + static_cast<uint64_t>(imm_i)) & ~uint64_t{1};
      set_x(rd, next);
      next = target;
      break;
    }
    case 0x63: {      // branches
      const int64_t off = sext((bit(insn, 31) << 12) | (bit(insn, 7) << 11) | (field(insn, 30, 25) << 5) | (field(insn, 11, 8) << 1), 13);
      bool taken;
      switch (funct3) {
        case 0: taken = a == b; break;
        case 1: taken = a != b; break;
        case 4: taken = static_cast<int64_t>(a) < static_cast<int64_t>(b); break;
        case 5: taken = static_cast<int64_t>(a) >= static_cast<int64_t>(b); break;
        case 6: taken = a < b; break;
        case 7: taken = a >= b; break;
        default: return undefined();
      }
      if (taken) next = s_.pc + static_cast<uint64_t>(off);
      break;
    }
    case kLoad: {
      static const uint32_t sizes[8] = {1, 2, 4, 8, 1, 2, 4, 0};
      const uint32_t size = sizes[funct3];
      if (size == 0) return undefined();
      uint64_t v;
      if (!load(m, a + static_cast<uint64_t>(imm_i), size, &v)) return Step::Fault;
      if (funct3 < 4) v = static_cast<uint64_t>(sext(v, size * 8));
      set_x(rd, v);
      break;
    }
    case kStore: {
      if (funct3 > 3) return undefined();
      const int64_t off = sext((funct7 << 5) | rd, 12);
      if (!store(m, a + static_cast<uint64_t>(off), 1u << funct3, b)) return Step::Fault;
      break;
    }
    case kOpImm: {
      const uint32_t shamt = field(insn, 25, 20);
      uint64_t r;
      switch (funct3) {
        case 0: r = a + static_cast<uint64_t>(imm_i); break;
        case 2: r = static_cast<int64_t>(a) < imm_i; break;
        case 3: r = a < static_cast<uint64_t>(imm_i); break;
        case 4: r = a ^ static_cast<uint64_t>(imm_i); break;
        case 6: r = a | static_cast<uint64_t>(imm_i); break;
        case 7: r = a & static_cast<uint64_t>(imm_i); break;
        case 1:
          if ((insn >> 26) != 0) return undefined();
          r = a << shamt;
          break;
        default:
          if ((insn >> 26) == 0) r = a >> shamt;
          else if ((insn >> 26) == 0x10) r = static_cast<uint64_t>(static_cast<int64_t>(a) >> shamt);
          else return undefined();
          break;
      }
      set_x(rd, r);
      break;
    }
    case kOpImm32: {
      const uint32_t shamt = field(insn, 24, 20);
      uint64_t r;
      switch (funct3) {
        case 0: r = word(a + static_cast<uint64_t>(imm_i)); break;
        case 1:
          if (funct7 != 0) return undefined();
          r = word(static_cast<uint32_t>(a) << shamt);
          break;
        case 5:
          if (funct7 == 0) r = word(static_cast<uint32_t>(a) >> shamt);
          else if (funct7 == 0x20) r = static_cast<uint64_t>(static_cast<int32_t>(a) >> shamt);
          else return undefined();
          break;
        default: return undefined();
      }
      set_x(rd, r);
      break;
    }
    case kOp: {
      uint64_t r;
      if (funct7 == 1) {      // M
        switch (funct3) {
          case 0: r = a * b; break;
          case 1: {      // mulh
            uint64_t hi, lo;
            multiply_unsigned(a, b, &hi, &lo);
            r = hi - (static_cast<int64_t>(a) < 0 ? b : 0) - (static_cast<int64_t>(b) < 0 ? a : 0);
            break;
          }
          case 2: {      // mulhsu
            uint64_t hi, lo;
            multiply_unsigned(a, b, &hi, &lo);
            r = hi - (static_cast<int64_t>(a) < 0 ? b : 0);
            break;
          }
          case 3: {
            uint64_t lo;
            multiply_unsigned(a, b, &r, &lo);
            break;
          }
          case 4: {      // div
            const int64_t x = static_cast<int64_t>(a), y = static_cast<int64_t>(b);
            if (y == 0) r = ~uint64_t{0};
            else if (x == INT64_MIN && y == -1) r = a;
            else r = static_cast<uint64_t>(x / y);
            break;
          }
          case 5: r = b == 0 ? ~uint64_t{0} : a / b; break;
          case 6: {
            const int64_t x = static_cast<int64_t>(a), y = static_cast<int64_t>(b);
            if (y == 0) r = a;
            else if (x == INT64_MIN && y == -1) r = 0;
            else r = static_cast<uint64_t>(x % y);
            break;
          }
          default: r = b == 0 ? a : a % b; break;
        }
      } else if (funct7 == 0 || funct7 == 0x20) {
        const bool alt = funct7 == 0x20;
        switch (funct3) {
          case 0: r = alt ? a - b : a + b; break;
          case 1: if (alt) return undefined(); r = a << (b & 63); break;
          case 2: if (alt) return undefined(); r = static_cast<int64_t>(a) < static_cast<int64_t>(b); break;
          case 3: if (alt) return undefined(); r = a < b; break;
          case 4: if (alt) return undefined(); r = a ^ b; break;
          case 5: r = alt ? static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63)) : a >> (b & 63); break;
          case 6: if (alt) return undefined(); r = a | b; break;
          default: if (alt) return undefined(); r = a & b; break;
        }
      } else {
        return undefined();
      }
      set_x(rd, r);
      break;
    }
    case kOp32: {
      const uint32_t x = static_cast<uint32_t>(a), y = static_cast<uint32_t>(b);
      uint64_t r;
      if (funct7 == 1) {
        switch (funct3) {
          case 0: r = word(uint64_t{x} * y); break;
          case 4: {
            const int32_t sx = static_cast<int32_t>(x), sy = static_cast<int32_t>(y);
            r = sy == 0 ? ~uint64_t{0} : (sx == INT32_MIN && sy == -1) ? word(x) : word(static_cast<uint32_t>(sx / sy));
            break;
          }
          case 5: r = y == 0 ? ~uint64_t{0} : word(x / y); break;
          case 6: {
            const int32_t sx = static_cast<int32_t>(x), sy = static_cast<int32_t>(y);
            r = sy == 0 ? word(x) : (sx == INT32_MIN && sy == -1) ? 0 : word(static_cast<uint32_t>(sx % sy));
            break;
          }
          case 7: r = y == 0 ? word(x) : word(x % y); break;
          default: return undefined();
        }
      } else if (funct7 == 0 || funct7 == 0x20) {
        const bool alt = funct7 == 0x20;
        switch (funct3) {
          case 0: r = word(alt ? x - y : x + y); break;
          case 1: if (alt) return undefined(); r = word(x << (y & 31)); break;
          case 5: r = alt ? static_cast<uint64_t>(static_cast<int32_t>(x) >> (y & 31)) : word(x >> (y & 31)); break;
          default: return undefined();
        }
      } else {
        return undefined();
      }
      set_x(rd, r);
      break;
    }
    case 0x0F:      // fence, fence.i, and the cache-block operations (Zicbom, Zicboz)
      if (funct3 == 2) {
        if (rd != 0) return undefined();
        const uint32_t operation = insn >> 20;
        if (operation > 2 && operation != 4) return undefined();
        if (operation == 4) {      // cbo.zero: clears the 64-byte block holding the address
          const uint64_t block = a & ~uint64_t{63};
          for (uint32_t i = 0; i < 64; i += 8) {
            if (!store(m, block + i, 8, 0)) return Step::Fault;
          }
        }      // (the other operations have no effect without caches)
        break;
      }
      if (funct3 > 1) return undefined();
      break;
    case 0x2F: {
      const Step r = atomic(m, insn);
      if (r != Step::Ok) return r;
      break;
    }
    case 0x73: {
      const Step r = system(insn, length);
      if (r != Step::Ok) {
        if (r == Step::Syscall) s_.pc = next;      // (the call returns after the ecall)
        return r;
      }
      if (jumped_) {
        jumped_ = false;
        return Step::Ok;
      }
      break;
    }
    case 0x77: {      // OP-VE: the vector cryptography extensions
      VectorUnit u(*this, m, insn);
      if (!u.prepare()) return undefined();
      const Step r = u.crypto();
      if (r != Step::Ok) return r;
      break;
    }
    case kLoadFp: case kStoreFp:
      if (funct3 != 1 && funct3 != 2 && funct3 != 3) {      // the vector loads and stores share these opcodes (width 1 is the half precision load or store)
        const Step r = vector_memory(m, insn);
        if (r != Step::Ok) return r;
        break;
      }
      [[fallthrough]];
    case 0x43: case 0x47: case 0x4B: case 0x4F: case 0x53: {
      const Step r = floating(m, insn);
      if (r != Step::Ok) return r;
      break;
    }
    case 0x57: {
      const Step r = vector(m, insn);
      if (r != Step::Ok) return r;
      break;
    }
    default: return undefined();
  }
  if (unit) sys_unit_dirty(unit);
  s_.pc = next;
  return Step::Ok;
}

RiscV64Cpu::Step RiscV64Cpu::atomic(GuestMemory& m, uint32_t insn) {
  const uint32_t rd = field(insn, 11, 7), funct3 = field(insn, 14, 12), rs1 = field(insn, 19, 15), rs2 = field(insn, 24, 20), funct5 = insn >> 27;
  if (funct3 > 4 || (funct3 == 4 && funct5 != 5)) return undefined();
  const uint32_t size = funct3 == 0 ? 1 : funct3 == 1 ? 2 : funct3 == 2 ? 4 : funct3 == 3 ? 8 : 16;      // (Zabha: bytes and halfwords; Zacas: amocas.q on 16 bytes)
  const uint64_t address = s_.x[rs1];
  if (address & (size - 1)) return fail(13, address);
  struct AmoScope { bool& f; AmoScope(bool& x, bool on) : f(x) { f = on; } ~AmoScope() { f = false; } } amo_scope(amo_, funct5 != 2);      // (system mode: the load part of an AMO checks for write permission)
  auto widen = [&](uint64_t v) { return size == 1 ? static_cast<uint64_t>(sext(v, 8)) : size == 2 ? static_cast<uint64_t>(sext(v, 16)) : size == 4 ? static_cast<uint64_t>(sext(v, 32)) : v; };
  if (funct5 == 5) {      // Zacas: amocas
    if (size == 16) {      // amocas.q works on the register pairs (rd, rd+1) and (rs2, rs2+1)
      if ((rd & 1) || (rs2 & 1)) return undefined();
      uint64_t low, high;
      if (!load(m, address, 8, &low) || !load(m, address + 8, 8, &high)) return Step::Fault;
      if (low == s_.x[rd] && high == s_.x[rd + 1] && (!store(m, address, 8, s_.x[rs2]) || !store(m, address + 8, 8, s_.x[rs2 + 1]))) return Step::Fault;
      set_x(rd, low);
      set_x(rd + 1, high);
      reservation_valid_ = false;
      return Step::Ok;
    }
    uint64_t old;
    if (!load(m, address, size, &old)) return Step::Fault;
    const uint64_t mask = size == 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * size)) - 1;
    if ((old & mask) == (s_.x[rd] & mask) && !store(m, address, size, s_.x[rs2])) return Step::Fault;
    set_x(rd, widen(old));
    reservation_valid_ = false;
    return Step::Ok;
  }
  if (size < 4 && (funct5 == 2 || funct5 == 3)) return undefined();      // (there is no lr / sc for bytes and halfwords)
  if (funct5 == 2) {      // lr
    if (rs2 != 0) return undefined();
    uint64_t v;
    if (!load(m, address, size, &v)) return Step::Fault;
    set_x(rd, widen(v));
    reservation_valid_ = true;
    reservation_address_ = address;
    return Step::Ok;
  }
  if (funct5 == 3) {      // sc
    const bool ok = reservation_valid_ && reservation_address_ == address;
    reservation_valid_ = false;
    if (ok && !store(m, address, size, s_.x[rs2])) return Step::Fault;
    set_x(rd, ok ? 0 : 1);
    return Step::Ok;
  }
  uint64_t old;
  if (!load(m, address, size, &old)) return Step::Fault;
  const uint64_t src = s_.x[rs2];
  uint64_t result;
  const int64_t so = static_cast<int64_t>(widen(old)), ss = static_cast<int64_t>(widen(src));
  const uint64_t mask = size == 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * size)) - 1;
  const uint64_t uo = old & mask, us = src & mask;
  switch (funct5) {
    case 1: result = src; break;
    case 0: result = old + src; break;
    case 4: result = old ^ src; break;
    case 12: result = old & src; break;
    case 8: result = old | src; break;
    case 16: result = so < ss ? old : src; break;
    case 20: result = so > ss ? old : src; break;
    case 24: result = uo < us ? old : src; break;
    case 28: result = uo > us ? old : src; break;
    default: return undefined();
  }
  if (!store(m, address, size, result)) return Step::Fault;
  set_x(rd, widen(old));
  reservation_valid_ = false;
  return Step::Ok;
}

RiscV64Cpu::Step RiscV64Cpu::system(uint32_t insn, uint32_t) {
  const uint32_t funct3 = field(insn, 14, 12);
  if (funct3 == 0) {
    if (bus_) return sys_instruction(insn);
    if (insn == 0x00000073) return Step::Syscall;      // ecall
    if (insn == 0x00100073) return fail(3);            // ebreak
    if (insn == 0x00D00073 || insn == 0x01D00073) return Step::Ok;      // Zawrs: wrs.nto, wrs.sto (the reservation set is never lost, so they return at once)
    return undefined();
  }
  if (funct3 == 4) {      // Zimop: mop.r.n and mop.rr.n write zero to rd (no instruction redefines them here)
    const bool mop_r = (insn >> 31) == 1 && ((insn >> 28) & 3) == 0 && ((insn >> 22) & 0xF) == 7 && !((insn >> 25) & 1);
    const bool mop_rr = (insn >> 31) == 1 && ((insn >> 28) & 3) == 0 && ((insn >> 25) & 1);
    if (!mop_r && !mop_rr) return undefined();
    set_x(field(insn, 11, 7), 0);
    return Step::Ok;
  }
  return csr_access(insn);
}

RiscV64Cpu::Step RiscV64Cpu::csr_access(uint32_t insn) {
  const uint32_t rd = field(insn, 11, 7), funct3 = field(insn, 14, 12), rs1 = field(insn, 19, 15), csr = insn >> 20;
  if (bus_) return sys_csr(insn);
  if (funct3 == 4) return undefined();
  const bool immediate = funct3 >= 5;
  const uint64_t operand = immediate ? rs1 : s_.x[rs1];
  const uint32_t kind = funct3 & 3;      // 1 write, 2 set, 3 clear
  const bool writes = kind == 1 || rs1 != 0;
  uint64_t old;
  switch (csr) {
    case 0x001: old = s_.fcsr & 31; break;
    case 0x002: old = (s_.fcsr >> 5) & 7; break;
    case 0x003: old = s_.fcsr & 0xFF; break;
    case 0xC00: case 0xC01: old = retired_ * 4; break;      // cycle, time: the instruction count stands in for a clock
    case 0xC02: old = retired_; break;      // instret
    case 0x008: old = s_.vstart; break;
    case 0x009: old = s_.vxsat; break;
    case 0x00A: old = s_.vxrm; break;
    case 0x00F: old = (s_.vxrm << 1) | s_.vxsat; break;
    case 0xC20: old = s_.vl; break;
    case 0xC21: old = s_.vtype; break;
    case 0xC22: old = 16; break;      // vlenb
    default: return undefined();
  }
  if (writes) {
    if (csr >= 0xC00) return undefined();      // read-only
    uint64_t v = kind == 1 ? operand : kind == 2 ? (old | operand) : (old & ~operand);
    switch (csr) {
      case 0x001: s_.fcsr = (s_.fcsr & ~31u) | (static_cast<uint32_t>(v) & 31); break;
      case 0x002: s_.fcsr = (s_.fcsr & ~0xE0u) | ((static_cast<uint32_t>(v) & 7) << 5); break;
      case 0x008: s_.vstart = v & 127; break;
      case 0x009: s_.vxsat = v & 1; break;
      case 0x00A: s_.vxrm = v & 3; break;
      case 0x00F: s_.vxsat = v & 1; s_.vxrm = (v >> 1) & 3; break;
      default: s_.fcsr = static_cast<uint32_t>(v) & 0xFF; break;
    }
  }
  set_x(rd, old);
  return Step::Ok;
}

}  // namespace emulation
