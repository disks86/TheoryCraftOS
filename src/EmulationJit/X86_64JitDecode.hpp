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
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

// The instruction decoder of the x86-64 translator: the integer subset the translator turns into IR. Anything else (SSE, x87, string instructions, system
// instructions, prefixes it does not model) is not decoded here, which ends the block in front of it and leaves that instruction to the interpreter. The decoder
// follows the interpreter (src/Emulation/X86_64Cpu.cpp) prefix for prefix, so both agree on what an instruction means.
namespace emulation {

enum class X86Op : uint8_t {
  Invalid,
  Nop,
  Mov,        // dst <- src (register, memory or immediate)
  Lea,
  Movzx, Movsx,      // from `src_size` bytes to `size`
  Movsxd,
  Alu,        // add or and sub xor cmp: `kind` is the opcode's 3-bit operation number
  Test,
  Not, Neg, Inc, Dec,
  Imul2,      // dst <- dst * src, or dst <- src * imm (`has_imm`)
  Shift,      // `kind` 4 shl, 5 shr, 7 sar by an immediate count
  Push, Pop,
  Call, Jmp, Ret, Jcc,      // direct (`has_imm` carries the displacement) or through a register / memory operand
  Setcc, Cmov,
  Cdqe, Cqo,
  Leave,
};

struct X86Insn {
  X86Op op = X86Op::Invalid;
  uint8_t length = 0;
  uint8_t size = 8;           // operand size in bytes
  uint8_t src_size = 0;       // movzx / movsx / movsxd: the source size
  uint8_t kind = 0;           // alu operation, shift operation, condition code
  bool rex = false;           // a REX prefix was present (byte registers 4..7 mean SPL..DIL instead of AH..BH)
  bool to_reg = false;        // the register operand is the destination (alu / mov / cmov / imul)
  bool has_imm = false;
  bool accumulator = false;   // the operand is AL/AX/EAX/RAX rather than a modrm operand
  bool mem = false;           // the r/m operand is in memory
  bool indirect = false;      // call / jmp through r/m
  bool opreg = false;         // the register operand (`reg`) is encoded in the opcode itself (push, pop)
  bool rip_relative = false;
  uint8_t reg = 0;            // modrm.reg (with REX.R), or the register in the opcode (push, pop, mov imm)
  uint8_t rm = 0;             // modrm.rm (with REX.B) when the operand is a register
  uint8_t base = 0xFF, index = 0xFF, scale = 0;      // 0xFF: none
  uint8_t segment = 0;        // 0 none, 1 FS, 2 GS
  int64_t disp = 0;
  int64_t imm = 0;
};

// Decodes one instruction from `bytes` (up to 15 bytes). False if it is not in the translated subset, or the bytes ran out.
bool decode_x86(ConstByteView bytes, X86Insn* out);

// What an instruction does to the status flags (CF PF AF ZF SF OF), for knowing when a flag computation can be skipped.
enum class X86FlagUse : uint8_t {
  None,         // leaves them alone
  Reads,        // reads some of them
  WritesAll,    // overwrites all six without reading any
  Partial,      // overwrites some and keeps the others (inc, dec, imul)
};
X86FlagUse x86_flag_use(const X86Insn& insn);

}  // namespace emulation
