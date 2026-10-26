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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.hpp"

namespace hardware::v3d_qpu {

namespace {
enum Mux : uint32_t { kR0, kR1, kR2, kR3, kR4, kR5, kPortA, kPortB };

struct AddDesc {
  uint32_t opcode;
  uint32_t sources;       // how many operands the operation reads
  int32_t fixed_mux_a;    // mux value that identifies the operation when it reads no first operand (-1: not fixed)
  int32_t fixed_mux_b;    // and no second
  bool swap_to_order;     // FADD: its operands are put in increasing mux order (the other order is FADDNF)
  int32_t waddr;          // the destination field is part of the operation (-1: it is the destination)
};

AddDesc add_desc(Add op) {
  switch (op) {
    case Add::Nop: return {187, 0, 0, 0, false, -1};
    case Add::Fadd: return {5, 2, -1, -1, true, -1};            // opcode 0, both inputs "no unpack" (1 << 2 | 1)
    case Add::Or: return {182, 2, -1, -1, false, -1};
    case Add::Ftoiz: return {245, 1, -1, 7, false, -1};
    case Add::Ffloor: return {246, 1, -1, 0, false, -1};
    case Add::Vfpack: return {53, 2, -1, -1, false, -1};
    case Add::LdVpmvIn: return {188, 1, -1, 0, false, -1};
    case Add::StVpmv: return {248, 2, -1, -1, false, 0};         // the destination field says which store (0: a vector at a uniform offset)
    case Add::VpmWt: return {187, 0, 6, 2, false, -1};
  }
  return {187, 0, 0, 0, false, -1};
}

struct MulDesc {
  uint32_t opcode;
  uint32_t sources;
  int32_t fixed_mux_a, fixed_mux_b;
};

MulDesc mul_desc(Mul op) {
  switch (op) {
    case Mul::Nop: return {15, 0, 0, 4};
    case Mul::Fmul: return {21, 2, -1, -1};  // opcode 16, both inputs "no unpack"
  }
  return {15, 0, 0, 4};
}

struct Ports {
  int32_t a = -1, b = -1;  // register file entries read through port A and B
  bool imm = false;
  uint8_t imm_value = 0;
};

// Picks the mux value for an operand, claiming a port for a register file entry or an immediate. False if there is no port left.
bool resolve(const Operand& o, Ports* ports, uint32_t* mux) {
  switch (o.kind) {
    case Operand::Kind::None: *mux = kR0; return true;
    case Operand::Kind::Acc:
      if (o.n > 5) return false;
      *mux = o.n;
      return true;
    case Operand::Kind::Imm:
      if (o.n > 15 || (ports->b >= 0) || (ports->imm && ports->imm_value != o.n)) return false;
      ports->imm = true;
      ports->imm_value = o.n;
      *mux = kPortB;
      return true;
    case Operand::Kind::Rf:
      if (o.n > 63) return false;
      if (ports->a == o.n) { *mux = kPortA; return true; }
      if (ports->b == o.n && !ports->imm) { *mux = kPortB; return true; }
      if (ports->a < 0) { ports->a = o.n; *mux = kPortA; return true; }
      if (ports->b < 0 && !ports->imm) { ports->b = o.n; *mux = kPortB; return true; }
      return false;
  }
  return false;
}

// The 5-bit signal field for the combinations this assembler offers (the V3D 4.2 signal table), or -1.
int32_t signal_code(const Instruction& i, bool small_imm) {
  const int signals = (i.thrsw ? 1 : 0) + (i.ldvary ? 2 : 0) + (i.ldunifrf ? 4 : 0) + (small_imm ? 8 : 0);
  switch (signals) {
    case 0: return 0;
    case 1: return 1;            // thrsw
    case 2: return 8;            // ldvary
    case 3: return 9;            // thrsw + ldvary
    case 4: return 12;           // ldunifrf
    case 5: return 13;           // thrsw + ldunifrf
    case 8: return 15;           // small immediate
    case 10: return 14;          // small immediate + ldvary
    default: return -1;
  }
}
}  // namespace

bool encode(const Instruction& i, uint64_t* word) {
  const AddDesc add = add_desc(i.add_op);
  const MulDesc mul = mul_desc(i.mul_op);
  if (i.ldvary && i.ldunifrf) return false;
  Ports ports;
  uint32_t add_a = add.fixed_mux_a >= 0 ? static_cast<uint32_t>(add.fixed_mux_a) : 0;
  uint32_t add_b = add.fixed_mux_b >= 0 ? static_cast<uint32_t>(add.fixed_mux_b) : 0;
  uint32_t mul_a = mul.fixed_mux_a >= 0 ? static_cast<uint32_t>(mul.fixed_mux_a) : 0;
  uint32_t mul_b = mul.fixed_mux_b >= 0 ? static_cast<uint32_t>(mul.fixed_mux_b) : 0;
  Operand a = i.add_a, b = i.add_b;
  if (add.sources < 1 && a.kind != Operand::Kind::None) return false;
  if (add.sources < 2 && b.kind != Operand::Kind::None) return false;
  if (mul.sources < 1 && i.mul_a.kind != Operand::Kind::None) return false;
  if (mul.sources < 2 && i.mul_b.kind != Operand::Kind::None) return false;
  if (add.sources >= 1 && !resolve(a, &ports, &add_a)) return false;
  if (add.sources >= 2 && !resolve(b, &ports, &add_b)) return false;
  if (mul.sources >= 1 && !resolve(i.mul_a, &ports, &mul_a)) return false;
  if (mul.sources >= 2 && !resolve(i.mul_b, &ports, &mul_b)) return false;
  if (add.swap_to_order && add_a > add_b) { const uint32_t t = add_a; add_a = add_b; add_b = t; }

  const int32_t sig = signal_code(i, ports.imm);
  if (sig < 0) return false;
  const bool sig_writes_address = i.ldvary || i.ldunifrf;
  if (sig_writes_address && i.sig_dest.addr > 63) return false;

  uint64_t w = 0;
  w |= static_cast<uint64_t>(mul.opcode) << 58;
  w |= static_cast<uint64_t>(sig) << 53;
  if (sig_writes_address) w |= (static_cast<uint64_t>(i.sig_dest.addr) | (i.sig_dest.magic ? 1u << 6 : 0u)) << 46;
  if (i.mul_dest.magic) w |= uint64_t{1} << 45;
  if (i.add_dest.magic && add.waddr < 0) w |= uint64_t{1} << 44;
  w |= static_cast<uint64_t>(i.mul_dest.addr & 63) << 38;
  w |= static_cast<uint64_t>(add.waddr >= 0 ? static_cast<uint32_t>(add.waddr) : (i.add_dest.addr & 63u)) << 32;
  w |= static_cast<uint64_t>(add.opcode) << 24;
  w |= static_cast<uint64_t>(mul_b) << 21;
  w |= static_cast<uint64_t>(mul_a) << 18;
  w |= static_cast<uint64_t>(add_b) << 15;
  w |= static_cast<uint64_t>(add_a) << 12;
  w |= static_cast<uint64_t>(ports.a >= 0 ? ports.a : 0) << 6;
  w |= static_cast<uint64_t>(ports.imm ? ports.imm_value : (ports.b >= 0 ? ports.b : 0));
  *word = w;
  return true;
}

bool Program::emit(const Instruction& instruction) {
  uint64_t word = 0;
  if (!ok_ || count_ >= storage_.size() || !encode(instruction, &word)) {
    ok_ = false;
    return false;
  }
  storage_[count_++] = word;
  return true;
}

bool Program::nop(uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    if (!emit(Instruction())) return false;
  return true;
}

}  // namespace hardware::v3d_qpu
