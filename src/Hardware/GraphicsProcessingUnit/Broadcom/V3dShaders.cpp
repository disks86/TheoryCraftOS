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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.hpp"

namespace hardware::v3d_shader {

using namespace v3d_qpu;

namespace {
Instruction ldvpm(uint8_t dest, uint8_t value_index) {
  Instruction i;
  i.add_op = Add::LdVpmvIn;
  i.add_dest = rf_dest(dest);
  i.add_a = imm(value_index);
  return i;
}
Instruction ldunif(uint8_t dest) {
  Instruction i;
  i.ldunifrf = true;
  i.sig_dest = rf_dest(dest);
  return i;
}
Instruction fmul(Dest dest, Operand a, Operand b) {
  Instruction i;
  i.mul_op = Mul::Fmul;
  i.mul_dest = dest;
  i.mul_a = a;
  i.mul_b = b;
  return i;
}
Instruction fadd(Dest dest, Operand a, Operand b) {
  Instruction i;
  i.add_op = Add::Fadd;
  i.add_dest = dest;
  i.add_a = a;
  i.add_b = b;
  return i;
}
Instruction unary(Add op, Dest dest, Operand a) {
  Instruction i;
  i.add_op = op;
  i.add_dest = dest;
  i.add_a = a;
  return i;
}
Instruction mov(Dest dest, Operand a) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = dest;
  i.add_a = a;
  i.add_b = a;
  return i;
}
Instruction stvpm(uint8_t slot, Operand value) {
  Instruction i;
  i.add_op = Add::StVpmv;
  i.add_a = imm(slot);
  i.add_b = value;
  return i;
}
Instruction vfpack(Dest dest, Operand a, Operand b) {
  Instruction i;
  i.add_op = Add::Vfpack;
  i.add_dest = dest;
  i.add_a = a;
  i.add_b = b;
  return i;
}
Instruction vpm_wait() {
  Instruction i;
  i.add_op = Add::VpmWt;
  return i;
}

// The end of every program: a thread switch signalled on the first of three instructions (the other two are its delay slots), which ends the thread.
void emit_program_end(v3d_qpu::Program& p) {
  Instruction end;
  end.thrsw = true;
  p.emit(end);
  p.nop(2);
}

// clip = MVP * (x, y, z, 1) into rf4..rf7, with the position in rf0..rf2. Reads the 16 matrix uniforms in memory order (column by column).
void emit_clip_position(v3d_qpu::Program& p) {
  constexpr uint8_t kClip = 4, kCoeff = 10, kTemp = 11;
  for (uint8_t r = 0; r < 4; ++r) {  // column 0 starts the sums
    p.emit(ldunif(kCoeff));
    p.emit(fmul(rf_dest(kClip + r), rf(kCoeff), rf(0)));
  }
  for (uint8_t column = 1; column < 3; ++column) {  // columns 1 and 2
    for (uint8_t r = 0; r < 4; ++r) {
      p.emit(ldunif(kCoeff));
      p.emit(fmul(rf_dest(kTemp), rf(kCoeff), rf(column)));
      p.emit(fadd(rf_dest(kClip + r), rf(kClip + r), rf(kTemp)));
    }
  }
  for (uint8_t r = 0; r < 4; ++r) {  // column 3 is the translation: w = 1
    p.emit(ldunif(kCoeff));
    p.emit(fadd(rf_dest(kClip + r), rf(kClip + r), rf(kCoeff)));
  }
}

// rf12 = int(floor(clip * scale * (1 / Wc))), the screen position in 1/256 pixel. `scale` is a register holding the viewport scale * 256, r4 the reciprocal.
void emit_screen_coordinate(v3d_qpu::Program& p, uint8_t clip, uint8_t scale, uint8_t slot) {
  p.emit(fmul(rf_dest(12), rf(clip), rf(scale)));
  p.emit(fmul(rf_dest(12), rf(12), acc(4)));
  p.emit(unary(Add::Ffloor, rf_dest(12), rf(12)));
  p.emit(unary(Add::Ftoiz, rf_dest(12), rf(12)));
  p.emit(stvpm(slot, rf(12)));
}
}  // namespace

bool build_colored_pipeline(ArrayView<uint64_t> storage, ShaderCode* out) {
  v3d_qpu::Program p(storage);
  constexpr uint8_t kClip = 4, kXScale = 8, kYScale = 9, kZScale = 14, kZOffset = 15;

  // --- Coordinate shader
  out->coordinate.first = p.count();
  for (uint8_t i = 0; i < 3; ++i) p.emit(ldvpm(i, i));
  emit_clip_position(p);
  p.emit(ldunif(kXScale));
  p.emit(ldunif(kYScale));
  for (uint8_t i = 0; i < 4; ++i) p.emit(stvpm(i, rf(kClip + i)));
  p.emit(mov(recip(), rf(kClip + 3)));  // 1 / Wc; the result is in r4 after two more instructions
  p.nop(2);
  emit_screen_coordinate(p, kClip, kXScale, 4);
  emit_screen_coordinate(p, kClip + 1, kYScale, 5);
  p.emit(vpm_wait());  // the VPM writes must be complete before the thread ends
  emit_program_end(p);
  out->coordinate.instructions = p.count() - out->coordinate.first;

  // --- Vertex shader
  out->vertex.first = p.count();
  for (uint8_t i = 0; i < 3; ++i) p.emit(ldvpm(i, i));
  for (uint8_t i = 0; i < 4; ++i) p.emit(ldvpm(20 + i, 3 + i));  // colour into rf20..rf23
  emit_clip_position(p);
  p.emit(ldunif(kXScale));
  p.emit(ldunif(kYScale));
  p.emit(ldunif(kZScale));
  p.emit(ldunif(kZOffset));
  p.emit(mov(recip(), rf(kClip + 3)));
  p.nop(2);
  emit_screen_coordinate(p, kClip, kXScale, 0);
  emit_screen_coordinate(p, kClip + 1, kYScale, 1);
  p.emit(fmul(rf_dest(12), rf(kClip + 2), rf(kZScale)));  // Zs = Zc * zscale / Wc + zoffset
  p.emit(fmul(rf_dest(12), rf(12), acc(4)));
  p.emit(fadd(rf_dest(12), rf(12), rf(kZOffset)));
  p.emit(stvpm(2, rf(12)));
  p.emit(stvpm(3, acc(4)));  // 1 / Wc
  for (uint8_t i = 0; i < 4; ++i) p.emit(stvpm(4 + i, rf(20 + i)));
  p.emit(vpm_wait());
  emit_program_end(p);
  out->vertex.instructions = p.count() - out->vertex.first;

  // --- Fragment shaders: rf0 = W, rf2 = Z (the payload); smooth colour = varying * W + C
  for (int with_z = 0; with_z < 2; ++with_z) {
    Program& program = with_z ? out->fragment_z : out->fragment;
    program.first = p.count();
    for (uint8_t k = 0; k < 4; ++k) {
      p.emit([k] { Instruction i; i.ldvary = true; i.sig_dest = rf_dest(10); (void)k; return i; }());
      p.nop();  // C reaches r5 two instructions after the ldvary
      p.emit(fmul(rf_dest(11), rf(10), rf(0)));
      p.emit(fadd(rf_dest(4 + k), rf(11), acc(5)));
    }
    if (with_z) p.emit(mov(tlbu(), rf(2)));  // Z from the rasteriser; takes the Z configuration word
    p.emit(vfpack(tlbu(), rf(4), rf(5)));    // r, g; takes the colour configuration word
    p.emit(vfpack(tlb(), rf(6), rf(7)));     // b, a
    emit_program_end(p);
    program.instructions = p.count() - program.first;
  }
  return p.ok();
}

uint32_t float_bits(uint32_t n) {
  if (n == 0) return 0;
  uint32_t msb = 31;
  while (!((n >> msb) & 1)) --msb;
  const uint32_t mantissa = msb <= 23 ? (n << (23 - msb)) : (n >> (msb - 23));
  return ((127 + msb) << 23) | (mantissa & 0x7FFFFF);
}

namespace {
bool fill_matrix(ArrayView<uint32_t> out, ArrayView<const uint32_t> mvp, uint32_t words) {
  if (out.size() < words || mvp.size() < 16) return false;
  for (uint32_t i = 0; i < 16; ++i) out[i] = mvp[i];
  return true;
}
}  // namespace

bool coordinate_uniforms(ArrayView<uint32_t> out, ArrayView<const uint32_t> mvp, uint32_t x_scale, uint32_t y_scale) {
  if (!fill_matrix(out, mvp, kCoordinateUniformWords)) return false;
  out[16] = x_scale;
  out[17] = y_scale;
  return true;
}

bool vertex_uniforms(ArrayView<uint32_t> out, ArrayView<const uint32_t> mvp, uint32_t x_scale, uint32_t y_scale, uint32_t z_scale, uint32_t z_offset) {
  if (!fill_matrix(out, mvp, kVertexUniformWords)) return false;
  out[16] = x_scale;
  out[17] = y_scale;
  out[18] = z_scale;
  out[19] = z_offset;
  return true;
}

}  // namespace hardware::v3d_shader
