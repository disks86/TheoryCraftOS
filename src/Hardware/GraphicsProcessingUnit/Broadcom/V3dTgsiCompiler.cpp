/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dTgsiCompiler.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/TgsiParse.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.hpp"

namespace hardware::v3d_shader {

namespace {
using namespace v3d_qpu;

constexpr uint32_t kMaxTemps = 15;  // the fourth component of the next vector is kept for MAD's scratch

bool fail(const char* message, uint32_t at, const char** error, uint32_t* line) {
  if (error) *error = message;
  if (line) *line = at;
  return false;
}

Instruction binary(Add op, uint8_t dst, uint8_t a, uint8_t b) {
  Instruction i;
  i.add_op = op;
  i.add_dest = rf_dest(dst);
  i.add_a = rf(a);
  i.add_b = rf(b);
  return i;
}

Instruction multiply(uint8_t dst, uint8_t a, uint8_t b) {
  Instruction i;
  i.mul_op = Mul::Fmul;
  i.mul_dest = rf_dest(dst);
  i.mul_a = rf(a);
  i.mul_b = rf(b);
  return i;
}

Instruction unary(Add op, uint8_t dst, uint8_t src) {
  Instruction i;
  i.add_op = op;
  i.add_dest = rf_dest(dst);
  i.add_a = rf(src);
  return i;
}

bool emit(Program& p, const Instruction& i, uint32_t source_line, const char** error, uint32_t* line) {
  return p.emit(i) || fail("QPU instruction buffer is too small or an instruction cannot be encoded", source_line, error, line);
}

}  // namespace

bool compile_tgsi_alu(allocators::IAllocator& allocator, StringView text, ArrayView<uint64_t> output, TgsiCompileInfo* info, const char** error, uint32_t* line,
                      ArrayView<TgsiUniformBinding> uniforms) {
  if (info) *info = {};
  if (error) *error = nullptr;
  if (line) *line = 0;

  gcn::TgsiShader shader(allocator);
  if (!gcn::parse_tgsi(allocator, text, &shader, error, line)) return false;
  if (shader.inputs != 0 || shader.outputs != 0 || shader.samplers != 0)
    return fail("this compiler stage currently accepts TEMP, CONST, and IMM arithmetic only", 0, error, line);
  if (shader.temps > kMaxTemps) return fail("shader needs more temporary registers than the current QPU allocator provides", 0, error, line);

  Program program(output);
  const uint8_t scratch_base = static_cast<uint8_t>(shader.temps * 4);
  uint32_t register_count = shader.temps * 4;
  uint32_t uniform_count = 0;
  for (uint64_t n = 0; n < shader.instructions.count(); ++n) {
    const gcn::TgsiInst& inst = shader.instructions[n];
    if (inst.op == gcn::TgsiOp::End) continue;
    if (inst.dst.file != gcn::TgsiFile::Temp || inst.dst.index >= shader.temps || inst.dst.saturate)
      return fail("destination must be an unsaturated TEMP", inst.line, error, line);
    if (inst.op != gcn::TgsiOp::Mov && inst.op != gcn::TgsiOp::Add && inst.op != gcn::TgsiOp::Sub && inst.op != gcn::TgsiOp::Mul &&
        inst.op != gcn::TgsiOp::Mad && inst.op != gcn::TgsiOp::Min && inst.op != gcn::TgsiOp::Max && inst.op != gcn::TgsiOp::Flr &&
        inst.op != gcn::TgsiOp::Round && inst.op != gcn::TgsiOp::Trunc && inst.op != gcn::TgsiOp::Ceil)
      return fail("TGSI operation is not implemented by the V3D QPU compiler yet", inst.line, error, line);
    if (inst.src_count != (inst.op == gcn::TgsiOp::Mov || inst.op == gcn::TgsiOp::Flr || inst.op == gcn::TgsiOp::Round || inst.op == gcn::TgsiOp::Trunc || inst.op == gcn::TgsiOp::Ceil ? 1 : inst.op == gcn::TgsiOp::Mad ? 3 : 2))
      return fail("unexpected number of sources for TGSI operation", inst.line, error, line);

    for (uint32_t c = 0; c < 4; ++c) {
      if (!(inst.dst.mask & (1u << c))) continue;
      const uint8_t dst = static_cast<uint8_t>(inst.dst.index * 4 + c);
      uint8_t src[3] = {};
      uint8_t uniform_scratch_mask = 0;
      for (uint32_t k = 0; k < inst.src_count; ++k) {
        const gcn::TgsiSrc& source = inst.src[k];
        if (source.negate || source.absolute) return fail("source modifiers are not supported by this V3D compiler stage", inst.line, error, line);
        if (source.file == gcn::TgsiFile::Temp) {
          if (source.index >= shader.temps) return fail("TEMP source index is out of range", inst.line, error, line);
          src[k] = static_cast<uint8_t>(source.index * 4 + source.swizzle[c]);
          continue;
        }
        if (source.file != gcn::TgsiFile::Const && source.file != gcn::TgsiFile::Imm)
          return fail("source must be TEMP, CONST, or IMM", inst.line, error, line);
        if ((source.file == gcn::TgsiFile::Const && source.index >= shader.consts) ||
            (source.file == gcn::TgsiFile::Imm && source.index >= shader.immediates.count()))
          return fail("constant source index is out of range", inst.line, error, line);
        if (uniform_count >= uniforms.size()) return fail("uniform binding buffer is too small", inst.line, error, line);
        TgsiUniformBinding binding;
        binding.file = source.file;
        binding.index = source.index;
        binding.component = source.swizzle[c];
        if (source.file == gcn::TgsiFile::Imm)
          binding.immediate_bits = __builtin_bit_cast(uint32_t, shader.immediates[source.index].value[source.swizzle[c]]);
        uniforms[uniform_count++] = binding;
        src[k] = static_cast<uint8_t>(scratch_base + k);
        uniform_scratch_mask |= static_cast<uint8_t>(1u << k);
        if (register_count < static_cast<uint32_t>(src[k]) + 1) register_count = static_cast<uint32_t>(src[k]) + 1;
        Instruction load;
        load.ldunifrf = true;
        load.sig_dest = rf_dest(src[k]);
        if (!emit(program, load, inst.line, error, line)) return false;
      }

      bool ok = false;
      switch (inst.op) {
        case gcn::TgsiOp::Mov: {
          Instruction i;
          i.add_op = Add::Or;
          i.add_dest = rf_dest(dst);
          i.add_a = i.add_b = rf(src[0]);
          ok = emit(program, i, inst.line, error, line);
          break;
        }
        case gcn::TgsiOp::Add: ok = emit(program, binary(Add::Fadd, dst, src[0], src[1]), inst.line, error, line); break;
        case gcn::TgsiOp::Sub: ok = emit(program, binary(Add::Fsub, dst, src[0], src[1]), inst.line, error, line); break;
        case gcn::TgsiOp::Min: ok = emit(program, binary(Add::Fmin, dst, src[0], src[1]), inst.line, error, line); break;
        case gcn::TgsiOp::Max: ok = emit(program, binary(Add::Fmax, dst, src[0], src[1]), inst.line, error, line); break;
        case gcn::TgsiOp::Mul: ok = emit(program, multiply(dst, src[0], src[1]), inst.line, error, line); break;
        case gcn::TgsiOp::Mad: {
          uint8_t scratch_slot = 0;
          while (scratch_slot < 3 && (uniform_scratch_mask & (1u << scratch_slot))) ++scratch_slot;
          const uint8_t mad_scratch = static_cast<uint8_t>(scratch_base + scratch_slot);
          ok = emit(program, multiply(mad_scratch, src[0], src[1]), inst.line, error, line) &&
               emit(program, binary(Add::Fadd, dst, mad_scratch, src[2]), inst.line, error, line);
          if (register_count < static_cast<uint32_t>(mad_scratch) + 1) register_count = static_cast<uint32_t>(mad_scratch) + 1;
          break;
        }
        case gcn::TgsiOp::Flr: ok = emit(program, unary(Add::Ffloor, dst, src[0]), inst.line, error, line); break;
        case gcn::TgsiOp::Round: ok = emit(program, unary(Add::Fround, dst, src[0]), inst.line, error, line); break;
        case gcn::TgsiOp::Trunc: ok = emit(program, unary(Add::Ftrunc, dst, src[0]), inst.line, error, line); break;
        case gcn::TgsiOp::Ceil: ok = emit(program, unary(Add::Fceil, dst, src[0]), inst.line, error, line); break;
        default: break;
      }
      if (!ok) return false;
    }
  }

  Instruction end;
  end.thrsw = true;
  if (!emit(program, end, 0, error, line) || !program.nop(2)) return fail("QPU code buffer is too small", 0, error, line);
  if (info) {
    info->instruction_count = program.count();
    info->register_count = register_count;
    info->uniform_count = uniform_count;
  }
  if (error) *error = nullptr;
  if (line) *line = 0;
  return true;
}

}  // namespace hardware::v3d_shader
