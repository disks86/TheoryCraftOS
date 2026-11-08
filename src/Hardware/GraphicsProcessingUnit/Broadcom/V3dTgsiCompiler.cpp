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

Instruction load_vpm(uint8_t dst, uint8_t value) {
  Instruction i;
  i.add_op = Add::LdVpmvIn;
  i.add_dest = rf_dest(dst);
  i.add_a = imm(value);
  return i;
}

Instruction load_varying(uint8_t dst) {
  Instruction i;
  i.ldvary = true;
  i.sig_dest = rf_dest(dst);
  return i;
}

Instruction reciprocal(Operand source) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = recip();
  i.add_a = i.add_b = source;
  return i;
}

Instruction reciprocal_sqrt(Operand source) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = rsqrt();
  i.add_a = i.add_b = source;
  return i;
}

Instruction exp2(Operand source) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = exp();
  i.add_a = i.add_b = source;
  return i;
}

Instruction log2(Operand source) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = log();
  i.add_a = i.add_b = source;
  return i;
}

Instruction copy_accumulator(uint8_t dst, uint8_t accumulator) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = rf_dest(dst);
  i.add_a = i.add_b = acc(accumulator);
  return i;
}

Instruction move_zero(uint8_t dst) {
  Instruction i;
  i.add_op = Add::Or;
  i.add_dest = rf_dest(dst);
  i.add_a = i.add_b = imm(0);
  return i;
}

Instruction pack(Dest dst, uint8_t a, uint8_t b) {
  Instruction i;
  i.add_op = Add::Vfpack;
  i.add_dest = dst;
  i.add_a = rf(a);
  i.add_b = rf(b);
  return i;
}

bool emit(Program& p, const Instruction& i, uint32_t source_line, const char** error, uint32_t* line) {
  return p.emit(i) || fail("QPU instruction buffer is too small or an instruction cannot be encoded", source_line, error, line);
}

bool emit_absolute(Program& program, uint8_t dst, uint8_t src, uint8_t scratch, uint32_t source_line, const char** error, uint32_t* line) {
  return emit(program, move_zero(scratch), source_line, error, line) &&
         emit(program, binary(Add::Fsub, scratch, scratch, src), source_line, error, line) &&
         emit(program, binary(Add::Fmax, dst, src, scratch), source_line, error, line);
}

bool emit_negate(Program& program, uint8_t dst, uint8_t src, uint8_t zero, uint32_t source_line, const char** error, uint32_t* line) {
  return emit(program, move_zero(zero), source_line, error, line) &&
         emit(program, binary(Add::Fsub, dst, zero, src), source_line, error, line);
}

}  // namespace

bool compile_tgsi_alu(allocators::IAllocator& allocator, StringView text, ArrayView<uint64_t> output, TgsiCompileInfo* info, const char** error, uint32_t* line,
                      ArrayView<TgsiUniformBinding> uniforms, ArrayView<const TgsiInputBinding> inputs, bool write_depth, bool terminate) {
  if (info) *info = {};
  if (error) *error = nullptr;
  if (line) *line = 0;

  gcn::TgsiShader shader(allocator);
  if (!gcn::parse_tgsi(allocator, text, &shader, error, line)) return false;
  if (shader.samplers != 0 || shader.outputs > 2 || (shader.fragment && shader.outputs > 1))
    return fail("this compiler stage accepts a fragment COLOR output or vertex POSITION and GENERIC[0] outputs", 0, error, line);
  if (shader.fragment && shader.outputs != 0 && shader.output_semantic[0] != gcn::TgsiSemantic::Color)
    return fail("fragment output must use the COLOR semantic", 0, error, line);
  if (!shader.fragment && shader.outputs != 0 && (shader.output_semantic[0] != gcn::TgsiSemantic::Position ||
      (shader.outputs == 2 && (shader.output_semantic[1] != gcn::TgsiSemantic::Generic || shader.output_semantic_index[1] != 0))))
    return fail("vertex outputs must be POSITION and optional GENERIC[0]", 0, error, line);
  if (shader.fragment && shader.inputs != 0 && (shader.inputs != 1 || shader.input_semantic[0] != gcn::TgsiSemantic::Generic ||
                                                shader.input_semantic_index[0] != 0 || shader.input_flat[0]))
    return fail("fragment input must be one smooth GENERIC[0] vec4", 0, error, line);
  const bool fragment_input = shader.fragment && shader.inputs != 0;
  const uint32_t max_temps = fragment_input ? 12 : shader.outputs == 2 && !shader.fragment ? 13 : 14;
  if (shader.outputs != 0 && shader.temps > max_temps)
    return fail("shader outputs exceed the current QPU register allocation", 0, error, line);
  if (shader.temps > kMaxTemps) return fail("shader needs more temporary registers than the current QPU allocator provides", 0, error, line);

  Program program(output);
  constexpr uint8_t kFragmentTempBase = 4, kFragmentVaryingBase = 52, kVertexVaryingBase = 56;
  const uint8_t temp_base = fragment_input ? kFragmentTempBase : 0;
  const uint8_t scratch_base = fragment_input ? 56 : static_cast<uint8_t>(shader.temps * 4);
  uint32_t register_count = temp_base + shader.temps * 4;
  uint32_t uniform_count = 0;
  constexpr uint8_t kShaderOutputRegisterBase = 60;
  if (shader.outputs != 0) {
    for (uint8_t component = 0; component < 4; ++component)
      if (!emit(program, move_zero(static_cast<uint8_t>(kShaderOutputRegisterBase + component)), 0, error, line)) return false;
    if (!shader.fragment && shader.outputs == 2)
      for (uint8_t component = 0; component < 4; ++component)
        if (!emit(program, move_zero(static_cast<uint8_t>(kVertexVaryingBase + component)), 0, error, line)) return false;
    register_count = 64;
  }
  if (fragment_input) {
    for (uint8_t component = 0; component < 4; ++component) {
      const uint8_t dest = static_cast<uint8_t>(kFragmentVaryingBase + component);
      if (!emit(program, load_varying(dest), 0, error, line) || !program.nop() || !emit(program, multiply(dest, dest, 0), 0, error, line)) return false;
      // The fragment ABI places 1/W in rf0 and the varying's C coefficient in acc5.
      Instruction interpolate;
      interpolate.add_op = Add::Fadd;
      interpolate.add_dest = rf_dest(dest);
      interpolate.add_a = rf(dest);
      interpolate.add_b = acc(5);
      if (!emit(program, interpolate, 0, error, line)) return false;
    }
  }
  for (uint64_t n = 0; n < shader.instructions.count(); ++n) {
    const gcn::TgsiInst& inst = shader.instructions[n];
    if (inst.op == gcn::TgsiOp::End) continue;
    const bool temp_destination = inst.dst.file == gcn::TgsiFile::Temp && inst.dst.index < shader.temps;
    const bool shader_output = shader.outputs != 0 && inst.dst.file == gcn::TgsiFile::Out && inst.dst.index < shader.outputs;
    if ((!temp_destination && !shader_output) || inst.dst.saturate)
      return fail("destination must be an unsaturated TEMP or the supported shader output", inst.line, error, line);
    if (inst.op != gcn::TgsiOp::Mov && inst.op != gcn::TgsiOp::Neg && inst.op != gcn::TgsiOp::Add && inst.op != gcn::TgsiOp::Sub && inst.op != gcn::TgsiOp::Mul && inst.op != gcn::TgsiOp::Div &&
        inst.op != gcn::TgsiOp::Rsq && inst.op != gcn::TgsiOp::Sqrt && inst.op != gcn::TgsiOp::Ex2 && inst.op != gcn::TgsiOp::Lg2 && inst.op != gcn::TgsiOp::Pow &&
        inst.op != gcn::TgsiOp::Mad && inst.op != gcn::TgsiOp::Min && inst.op != gcn::TgsiOp::Max && inst.op != gcn::TgsiOp::Abs && inst.op != gcn::TgsiOp::Frc && inst.op != gcn::TgsiOp::Flr &&
        inst.op != gcn::TgsiOp::Round && inst.op != gcn::TgsiOp::Trunc && inst.op != gcn::TgsiOp::Ceil && inst.op != gcn::TgsiOp::Rcp)
      return fail("TGSI operation is not implemented by the V3D QPU compiler yet", inst.line, error, line);
    const bool unary_op = inst.op == gcn::TgsiOp::Mov || inst.op == gcn::TgsiOp::Neg || inst.op == gcn::TgsiOp::Rsq || inst.op == gcn::TgsiOp::Sqrt || inst.op == gcn::TgsiOp::Ex2 || inst.op == gcn::TgsiOp::Lg2 || inst.op == gcn::TgsiOp::Abs || inst.op == gcn::TgsiOp::Frc || inst.op == gcn::TgsiOp::Flr || inst.op == gcn::TgsiOp::Round || inst.op == gcn::TgsiOp::Trunc ||
                          inst.op == gcn::TgsiOp::Ceil || inst.op == gcn::TgsiOp::Rcp;
    const uint8_t expected_sources = unary_op ? 1 : inst.op == gcn::TgsiOp::Mad ? 3 : 2;
    if (inst.src_count != expected_sources)
      return fail("unexpected number of sources for TGSI operation", inst.line, error, line);

    for (uint32_t c = 0; c < 4; ++c) {
      if (!(inst.dst.mask & (1u << c))) continue;
      const uint8_t dst = shader_output ? static_cast<uint8_t>((!shader.fragment && inst.dst.index == 1 ? kVertexVaryingBase : kShaderOutputRegisterBase) + c)
                                         : static_cast<uint8_t>(temp_base + inst.dst.index * 4 + c);
      uint8_t src[3] = {};
      uint8_t scratch_mask = 0;
      bool source_in_scratch[3] = {};
      for (uint32_t k = 0; k < inst.src_count; ++k) {
        const gcn::TgsiSrc& source = inst.src[k];
        if (source.file == gcn::TgsiFile::Temp) {
          if (source.index >= shader.temps) return fail("TEMP source index is out of range", inst.line, error, line);
          src[k] = static_cast<uint8_t>(temp_base + source.index * 4 + source.swizzle[c]);
          continue;
        }
        if (source.file == gcn::TgsiFile::In) {
          if (source.index >= shader.inputs || source.swizzle[c] > 3) return fail("vertex input source index is out of range", inst.line, error, line);
          if (fragment_input) {
            if (source.index != 0) return fail("fragment input location is not supported", inst.line, error, line);
            src[k] = static_cast<uint8_t>(kFragmentVaryingBase + source.swizzle[c]);
            continue;
          }
          const TgsiInputBinding* binding = nullptr;
          for (const TgsiInputBinding& candidate : inputs)
            if (candidate.index == source.index && candidate.component == source.swizzle[c]) {
              binding = &candidate;
              break;
            }
          if (!binding) return fail("vertex input has no VPM binding", inst.line, error, line);
          src[k] = static_cast<uint8_t>(scratch_base + k);
          source_in_scratch[k] = true;
          scratch_mask |= static_cast<uint8_t>(1u << k);
          if (register_count < static_cast<uint32_t>(src[k]) + 1) register_count = static_cast<uint32_t>(src[k]) + 1;
          if (binding->constant) {
            if (uniform_count >= uniforms.size()) return fail("uniform binding buffer is too small", inst.line, error, line);
            TgsiUniformBinding uniform;
            uniform.file = gcn::TgsiFile::None;
            uniform.index = source.index;
            uniform.component = source.swizzle[c];
            uniform.immediate_bits = binding->constant_bits;
            uniforms[uniform_count++] = uniform;
            Instruction load;
            load.ldunifrf = true;
            load.sig_dest = rf_dest(src[k]);
            if (!emit(program, load, inst.line, error, line)) return false;
          } else {
            if (binding->vpm_value > 15) return fail("VPM input value index exceeds the QPU immediate range", inst.line, error, line);
            if (!emit(program, load_vpm(src[k], binding->vpm_value), inst.line, error, line)) return false;
          }
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
        source_in_scratch[k] = true;
        scratch_mask |= static_cast<uint8_t>(1u << k);
        if (register_count < static_cast<uint32_t>(src[k]) + 1) register_count = static_cast<uint32_t>(src[k]) + 1;
        Instruction load;
        load.ldunifrf = true;
        load.sig_dest = rf_dest(src[k]);
        if (!emit(program, load, inst.line, error, line)) return false;
      }

      for (uint32_t k = 0; k < inst.src_count; ++k) {
        if (!inst.src[k].absolute) continue;
        uint8_t scratch_slot = 0;
        if (!source_in_scratch[k]) {
          while (scratch_slot < 3 && (scratch_mask & (1u << scratch_slot))) ++scratch_slot;
          if (scratch_slot == 3) return fail("no QPU scratch register is available for source absolute value", inst.line, error, line);
        }
        const uint8_t absolute_value = source_in_scratch[k] ? src[k] : static_cast<uint8_t>(scratch_base + scratch_slot);
        if (!emit_absolute(program, absolute_value, src[k], static_cast<uint8_t>(scratch_base + 3), inst.line, error, line)) return false;
        src[k] = absolute_value;
        if (!source_in_scratch[k]) scratch_mask |= static_cast<uint8_t>(1u << scratch_slot);
        source_in_scratch[k] = true;
        if (register_count < static_cast<uint32_t>(absolute_value) + 1) register_count = static_cast<uint32_t>(absolute_value) + 1;
      }

      for (uint32_t k = 0; k < inst.src_count; ++k) {
        if (!inst.src[k].negate) continue;
        uint8_t scratch_slot = 0;
        if (!source_in_scratch[k]) {
          while (scratch_slot < 3 && (scratch_mask & (1u << scratch_slot))) ++scratch_slot;
          if (scratch_slot == 3) return fail("no QPU scratch register is available for source negation", inst.line, error, line);
        }
        const uint8_t negated = source_in_scratch[k] ? src[k] : static_cast<uint8_t>(scratch_base + scratch_slot);
        if (!emit(program, move_zero(negated), inst.line, error, line)) return fail("QPU source-negation zero could not be encoded", inst.line, error, line);
        if (!emit(program, binary(Add::Fsub, negated, negated, src[k]), inst.line, error, line))
          return fail("QPU source-negation subtract could not be encoded", inst.line, error, line);
        src[k] = negated;
        if (!source_in_scratch[k]) scratch_mask |= static_cast<uint8_t>(1u << scratch_slot);
        if (register_count < static_cast<uint32_t>(negated) + 1) register_count = static_cast<uint32_t>(negated) + 1;
      }

      bool ok = false;
      switch (inst.op) {
        case gcn::TgsiOp::Neg: {
          const uint8_t zero = static_cast<uint8_t>(scratch_base + 3);
          ok = emit_negate(program, dst, src[0], zero, inst.line, error, line);
          if (register_count < static_cast<uint32_t>(zero) + 1) register_count = static_cast<uint32_t>(zero) + 1;
          break;
        }
        case gcn::TgsiOp::Abs: ok = emit_absolute(program, dst, src[0], static_cast<uint8_t>(scratch_base + 3), inst.line, error, line); break;
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
        case gcn::TgsiOp::Div: {
          const uint8_t reciprocal_result = static_cast<uint8_t>(scratch_base + 3);
          ok = emit(program, reciprocal(rf(src[1])), inst.line, error, line) && program.nop(2) &&
               emit(program, copy_accumulator(reciprocal_result, 4), inst.line, error, line) &&
               emit(program, multiply(dst, src[0], reciprocal_result), inst.line, error, line);
          if (!ok && error && !*error) fail("QPU instruction buffer is too small", inst.line, error, line);
          if (register_count < static_cast<uint32_t>(reciprocal_result) + 1) register_count = static_cast<uint32_t>(reciprocal_result) + 1;
          break;
        }
        case gcn::TgsiOp::Rsq: {
          ok = emit(program, reciprocal_sqrt(rf(src[0])), inst.line, error, line) && program.nop(2) && emit(program, copy_accumulator(dst, 4), inst.line, error, line);
          if (!ok && error && !*error) fail("QPU instruction buffer is too small", inst.line, error, line);
          break;
        }
        case gcn::TgsiOp::Sqrt: {
          const uint8_t rsqrt_result = static_cast<uint8_t>(scratch_base + 3);
          ok = emit(program, reciprocal_sqrt(rf(src[0])), inst.line, error, line) && program.nop(2) &&
               emit(program, copy_accumulator(rsqrt_result, 4), inst.line, error, line) &&
               emit(program, multiply(dst, src[0], rsqrt_result), inst.line, error, line);
          if (!ok && error && !*error) fail("QPU instruction buffer is too small", inst.line, error, line);
          if (register_count < static_cast<uint32_t>(rsqrt_result) + 1) register_count = static_cast<uint32_t>(rsqrt_result) + 1;
          break;
        }
        case gcn::TgsiOp::Ex2:
        case gcn::TgsiOp::Lg2: {
          const Instruction sfu = inst.op == gcn::TgsiOp::Ex2 ? exp2(rf(src[0])) : log2(rf(src[0]));
          ok = emit(program, sfu, inst.line, error, line) && program.nop(2) && emit(program, copy_accumulator(dst, 4), inst.line, error, line);
          if (!ok && error && !*error) fail("QPU instruction buffer is too small", inst.line, error, line);
          break;
        }
        case gcn::TgsiOp::Pow: {
          const uint8_t logarithm = static_cast<uint8_t>(scratch_base + 3);
          ok = emit(program, log2(rf(src[0])), inst.line, error, line) && program.nop(2) &&
               emit(program, copy_accumulator(logarithm, 4), inst.line, error, line) &&
               emit(program, multiply(logarithm, logarithm, src[1]), inst.line, error, line) &&
               emit(program, exp2(rf(logarithm)), inst.line, error, line) && program.nop(2) &&
               emit(program, copy_accumulator(dst, 4), inst.line, error, line);
          if (!ok && error && !*error) fail("QPU instruction buffer is too small", inst.line, error, line);
          if (register_count < static_cast<uint32_t>(logarithm) + 1) register_count = static_cast<uint32_t>(logarithm) + 1;
          break;
        }
        case gcn::TgsiOp::Frc: {
          const uint8_t whole = static_cast<uint8_t>(scratch_base + 3);
          ok = emit(program, unary(Add::Ffloor, whole, src[0]), inst.line, error, line) &&
               emit(program, binary(Add::Fsub, dst, src[0], whole), inst.line, error, line);
          if (register_count < static_cast<uint32_t>(whole) + 1) register_count = static_cast<uint32_t>(whole) + 1;
          break;
        }
        case gcn::TgsiOp::Mad: {
          uint8_t scratch_slot = 0;
          while (scratch_slot < 3 && (scratch_mask & (1u << scratch_slot))) ++scratch_slot;
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
        case gcn::TgsiOp::Rcp:
          ok = emit(program, reciprocal(rf(src[0])), inst.line, error, line) && program.nop(2) && emit(program, copy_accumulator(dst, 4), inst.line, error, line);
          if (!ok && error && !*error) fail("QPU instruction buffer is too small", inst.line, error, line);
          break;
        default: break;
      }
      if (!ok) return fail("QPU TGSI arithmetic instruction could not be encoded", inst.line, error, line);
    }
  }

  if (shader.fragment && shader.outputs != 0) {
    if (write_depth) {
      Instruction z;
      z.add_op = Add::Or;
      z.add_dest = tlbu();
      z.add_a = z.add_b = rf(2);
      if (!emit(program, z, 0, error, line)) return false;
    }
    if (!emit(program, pack(tlbu(), kShaderOutputRegisterBase, kShaderOutputRegisterBase + 1), 0, error, line) ||
        !emit(program, pack(tlb(), kShaderOutputRegisterBase + 2, kShaderOutputRegisterBase + 3), 0, error, line))
      return false;
  }

  if (terminate) {
    Instruction end;
    end.thrsw = true;
    if (!emit(program, end, 0, error, line) || !program.nop(2)) return fail("QPU code buffer is too small", 0, error, line);
  }
  if (info) {
    info->instruction_count = program.count();
    info->register_count = register_count;
    info->uniform_count = uniform_count;
    info->tlb_config_count = shader.fragment && shader.outputs != 0 ? (write_depth ? 2 : 1) : 0;
    if (!shader.fragment && shader.outputs != 0) {
      info->output_register_base = kShaderOutputRegisterBase;
      info->output_count = shader.outputs;
      info->varying_output_count = shader.outputs == 2 ? 1 : 0;
    }
    info->varying_input_count = fragment_input ? 1 : 0;
  }
  if (error) *error = nullptr;
  if (line) *line = 0;
  return true;
}

}  // namespace hardware::v3d_shader
