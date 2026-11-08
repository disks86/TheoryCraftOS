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

#include <stdio.h>
#include <stdlib.h>
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dTgsiCompiler.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/TgsiParse.hpp"

static int failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++failed; } } while (0)

class TestAllocator final : public allocators::IAllocator {
 public:
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t a = alignment < sizeof(void*) ? sizeof(void*) : alignment;
    return aligned_alloc(a, (size + a - 1) / a * a);
  }
  void deallocate(void* p) override { free(p); }
};

static void test_arithmetic_lowering() {
  TestAllocator allocator;
  const char* source =
      "VERT\n"
      "DCL TEMP[0..1]\n"
      "0: ADD TEMP[0], TEMP[0], TEMP[1]\n"
      "1: SUB TEMP[0], TEMP[0], TEMP[1]\n"
      "2: MUL TEMP[0], TEMP[0], TEMP[1]\n"
      "3: MAD TEMP[0], TEMP[0], TEMP[1], TEMP[0]\n"
      "4: MIN TEMP[0], TEMP[0], TEMP[1]\n"
      "5: MAX TEMP[0], TEMP[0], TEMP[1]\n"
      "6: FLR TEMP[0], TEMP[0]\n"
      "7: ROUND TEMP[0], TEMP[0]\n"
      "8: TRUNC TEMP[0], TEMP[0]\n"
      "9: CEIL TEMP[0], TEMP[0]\n"
      "10: END\n";
  uint64_t words[64] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  const bool compiled = hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line);
  if (!compiled) printf("compile error at line %u: %s\n", line, error ? error : "(none)");
  CHECK(compiled);
  CHECK(error == nullptr && line == 0);
  CHECK(info.instruction_count == 47 && info.register_count == 9);
  for (uint8_t c = 0; c < 4; ++c) {
    hardware::v3d_qpu::Instruction expected;
    expected.add_op = hardware::v3d_qpu::Add::Fadd;
    expected.add_dest = hardware::v3d_qpu::rf_dest(c);
    expected.add_a = hardware::v3d_qpu::rf(c);
    expected.add_b = hardware::v3d_qpu::rf(static_cast<uint8_t>(4 + c));
    uint64_t expected_word = 0;
    CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
    CHECK(words[c] == expected_word);
  }
  CHECK(words[4] != 0 && words[8] != 0 && words[12] != 0);

  const char* masked = "VERT\nDCL TEMP[0]\n0: ADD TEMP[0].xz, TEMP[0].wzyx, TEMP[0]\n1: END\n";
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(masked), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(info.instruction_count == 5);  // two selected components and the three-word end sequence
}

static void test_rejections_and_bounds() {
  TestAllocator allocator;
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;

  const char* unsupported = "VERT\nDCL TEMP[0]\n0: RSQ TEMP[0], TEMP[0]\n1: END\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(unsupported), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error && line == 3);

  const char* modified = "VERT\nDCL TEMP[0]\n0: ADD TEMP[0], -TEMP[0], TEMP[0]\n1: END\n";
  const bool modified_compiled = hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(modified), ArrayView<uint64_t>(words), &info, &error, &line);
  CHECK(modified_compiled);
  CHECK(error == nullptr && info.instruction_count == 15);
  hardware::v3d_qpu::Instruction negate;
  negate.add_op = hardware::v3d_qpu::Add::Or;
  negate.add_dest = hardware::v3d_qpu::rf_dest(4);
  negate.add_a = negate.add_b = hardware::v3d_qpu::imm(0);
  uint64_t expected_negate = 0;
  CHECK(hardware::v3d_qpu::encode(negate, &expected_negate));
  CHECK(words[0] == expected_negate);
  negate = {};
  negate.add_op = hardware::v3d_qpu::Add::Fsub;
  negate.add_dest = hardware::v3d_qpu::rf_dest(4);
  negate.add_a = hardware::v3d_qpu::rf(4);
  negate.add_b = hardware::v3d_qpu::rf(0);
  CHECK(hardware::v3d_qpu::encode(negate, &expected_negate));
  CHECK(words[1] == expected_negate);

  const char* absolute = "VERT\nDCL TEMP[0]\n0: ABS TEMP[0].x, TEMP[0]\n1: END\n";
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(absolute), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error == nullptr && info.instruction_count == 6);

  const char* absolute_source = "VERT\nDCL TEMP[0]\n0: ADD TEMP[0].x, |TEMP[0]|, TEMP[0]\n1: END\n";
  const bool absolute_source_compiled = hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(absolute_source), ArrayView<uint64_t>(words), &info, &error, &line);
  if (!absolute_source_compiled) printf("absolute source compile error at line %u: %s\n", line, error ? error : "(none)");
  CHECK(absolute_source_compiled);
  CHECK(error == nullptr && info.instruction_count == 7);

  const char* bit_flip = "VERT\nDCL TEMP[0]\n0: ADZ TEMP[0], TEMP[0], TEMP[0]\n1: END\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(bit_flip), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error && line == 3);

  const char* truncated = "VERT\nDCL TEMP[0]\n0: ADD TEMP[0], TEMP[0],\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(truncated), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error != nullptr);

  const char* too_many_temps = "VERT\nDCL TEMP[0..15]\n0: END\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(too_many_temps), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error && line == 0);

  uint64_t tiny[2] = {};
  const char* too_small = "VERT\nDCL TEMP[0]\n0: ADD TEMP[0], TEMP[0], TEMP[0]\n1: END\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(too_small), ArrayView<uint64_t>(tiny), &info, &error, &line));
  CHECK(error && line == 3);
}

static void test_uniform_sources() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL TEMP[0]\nDCL CONST[0]\nIMM[0] FLT32 {1.25, -0.5, 3.0, 4.0}\n0: ADD TEMP[0], CONST[0], IMM[0]\n1: END\n";
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiUniformBinding bindings[8] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line,
                                               ArrayView<hardware::v3d_shader::TgsiUniformBinding>(bindings)));
  CHECK(info.uniform_count == 8 && info.instruction_count == 15 && info.register_count == 6);
  CHECK(bindings[0].file == gcn::TgsiFile::Const && bindings[0].index == 0 && bindings[0].component == 0);
  CHECK(bindings[1].file == gcn::TgsiFile::Imm && bindings[1].index == 0 && bindings[1].component == 0 && bindings[1].immediate_bits == 0x3FA00000u);
  CHECK(bindings[6].component == 3 && bindings[7].component == 3 && bindings[7].immediate_bits == 0x40800000u);

  hardware::v3d_shader::TgsiUniformBinding too_small[7] = {};
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line,
                                                ArrayView<hardware::v3d_shader::TgsiUniformBinding>(too_small)));
  CHECK(error && line == 5);
}

static void test_vertex_input_vpm_binding() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL IN[0]\nDCL TEMP[0]\n0: ADD TEMP[0], IN[0], TEMP[0]\n1: END\n";
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiInputBinding bindings[4] = {{0, 0, 3}, {0, 1, 4}, {0, 2, 5}, {0, 3, 6}};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line, {},
                                               ArrayView<const hardware::v3d_shader::TgsiInputBinding>(bindings)));
  CHECK(info.instruction_count == 11 && info.register_count == 5 && info.uniform_count == 0);
  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::LdVpmvIn;
  expected.add_dest = hardware::v3d_qpu::rf_dest(4);
  expected.add_a = hardware::v3d_qpu::imm(3);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[0] == expected_word);

  hardware::v3d_shader::TgsiInputBinding missing[1] = {{0, 0, 0}};
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line, {},
                                                ArrayView<const hardware::v3d_shader::TgsiInputBinding>(missing)));
  CHECK(error && line == 4);
}

static void test_reciprocal_sfu_lowering() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL TEMP[0]\n0: RCP TEMP[0], TEMP[0]\n1: END\n";
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(info.instruction_count == 19);
  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::Or;
  expected.add_dest = hardware::v3d_qpu::recip();
  expected.add_a = expected.add_b = hardware::v3d_qpu::rf(0);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[0] == expected_word);
  expected = {};
  expected.add_op = hardware::v3d_qpu::Add::Or;
  expected.add_dest = hardware::v3d_qpu::rf_dest(0);
  expected.add_a = expected.add_b = hardware::v3d_qpu::acc(4);
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[3] == expected_word);
}

static void test_division_lowering() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL TEMP[0..1]\n0: DIV TEMP[0], TEMP[0], TEMP[1]\n1: END\n";
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error == nullptr && info.instruction_count == 23 && info.register_count == 12);

  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::Or;
  expected.add_dest = hardware::v3d_qpu::recip();
  expected.add_a = expected.add_b = hardware::v3d_qpu::rf(4);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[0] == expected_word);
  expected = {};
  expected.mul_op = hardware::v3d_qpu::Mul::Fmul;
  expected.mul_dest = hardware::v3d_qpu::rf_dest(0);
  expected.mul_a = hardware::v3d_qpu::rf(0);
  expected.mul_b = hardware::v3d_qpu::rf(11);
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[4] == expected_word);
}

static void test_fraction_lowering() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL TEMP[0]\n0: FRC TEMP[0].x, TEMP[0]\n1: END\n";
  uint64_t words[16] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error == nullptr && info.instruction_count == 5 && info.register_count == 8);

  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::Ffloor;
  expected.add_dest = hardware::v3d_qpu::rf_dest(7);
  expected.add_a = hardware::v3d_qpu::rf(0);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[0] == expected_word);
  expected = {};
  expected.add_op = hardware::v3d_qpu::Add::Fsub;
  expected.add_dest = hardware::v3d_qpu::rf_dest(0);
  expected.add_a = hardware::v3d_qpu::rf(0);
  expected.add_b = hardware::v3d_qpu::rf(7);
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[1] == expected_word);
}

static void test_negate_opcode_lowering() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL TEMP[0]\n0: NEG TEMP[0].x, TEMP[0]\n1: END\n";
  uint64_t words[16] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error == nullptr && info.instruction_count == 5 && info.register_count == 8);

  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::Or;
  expected.add_dest = hardware::v3d_qpu::rf_dest(7);
  expected.add_a = expected.add_b = hardware::v3d_qpu::imm(0);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[0] == expected_word);
  expected = {};
  expected.add_op = hardware::v3d_qpu::Add::Fsub;
  expected.add_dest = hardware::v3d_qpu::rf_dest(0);
  expected.add_a = hardware::v3d_qpu::rf(7);
  expected.add_b = hardware::v3d_qpu::rf(0);
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[1] == expected_word);
}

static void test_fragment_color_output() {
  TestAllocator allocator;
  const char* source = "FRAG\nDCL OUT[0], COLOR\nIMM[0] FLT32 {1.0, 0.5, 0.25, 1.0}\n0: MOV OUT[0], IMM[0]\n1: END\n";
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiUniformBinding bindings[4] = {};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line,
                                               ArrayView<hardware::v3d_shader::TgsiUniformBinding>(bindings)));
  CHECK(info.instruction_count == 17 && info.register_count == 64 && info.uniform_count == 4 && info.tlb_config_count == 1);
  CHECK(bindings[0].component == 0 && bindings[1].component == 1 && bindings[3].immediate_bits == 0x3F800000u);
  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::Vfpack;
  expected.add_dest = hardware::v3d_qpu::tlbu();
  expected.add_a = hardware::v3d_qpu::rf(60);
  expected.add_b = hardware::v3d_qpu::rf(61);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[12] == expected_word);

  uint64_t depth_words[32] = {};
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(depth_words), &info, &error, &line,
                                               ArrayView<hardware::v3d_shader::TgsiUniformBinding>(bindings), {}, true));
  CHECK(info.instruction_count == 18 && info.tlb_config_count == 2);
  expected = {};
  expected.add_op = hardware::v3d_qpu::Add::Or;
  expected.add_dest = hardware::v3d_qpu::tlbu();
  expected.add_a = expected.add_b = hardware::v3d_qpu::rf(2);
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(depth_words[12] == expected_word);
}

static void test_vertex_position_output() {
  TestAllocator allocator;
  const char* source = "VERT\nDCL IN[0], POSITION\nDCL OUT[0], POSITION\nDCL TEMP[0]\n0: MOV TEMP[0], IN[0]\n1: MOV OUT[0], TEMP[0]\n2: END\n";
  uint64_t words[32] = {};
  hardware::v3d_shader::TgsiInputBinding inputs[4] = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 3}};
  hardware::v3d_shader::TgsiCompileInfo info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(source), ArrayView<uint64_t>(words), &info, &error, &line, {},
                                               ArrayView<const hardware::v3d_shader::TgsiInputBinding>(inputs)));
  CHECK(info.output_register_base == 60 && info.output_count == 1 && info.register_count == 64 && info.instruction_count == 19);
  hardware::v3d_qpu::Instruction expected;
  expected.add_op = hardware::v3d_qpu::Add::Or;
  expected.add_dest = hardware::v3d_qpu::rf_dest(60);
  expected.add_a = expected.add_b = hardware::v3d_qpu::rf(0);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(words[12] == expected_word);

  const char* wrong_semantic = "VERT\nDCL OUT[0], COLOR\n0: END\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(wrong_semantic), ArrayView<uint64_t>(words), &info, &error, &line));
  CHECK(error && line == 0);

  const char* no_temp = "VERT\nDCL IN[0], POSITION\nDCL OUT[0], POSITION\n0: MOV OUT[0], IN[0]\n1: END\n";
  uint64_t body[32] = {};
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(no_temp), ArrayView<uint64_t>(body), &info, &error, &line, {},
                                               ArrayView<const hardware::v3d_shader::TgsiInputBinding>(inputs), false, false));
  CHECK(info.output_count == 1 && info.output_register_base == 60 && info.instruction_count == 12);
  hardware::v3d_shader::TgsiInputBinding fixed_layout_inputs[8] = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 0, true, 0x3F800000},
                                                                  {1, 0, 3}, {1, 1, 4}, {1, 2, 5}, {1, 3, 6}};
  hardware::v3d_shader::TgsiUniformBinding fixed_layout_uniform[4] = {};
  const bool fixed_compiled = hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(no_temp), ArrayView<uint64_t>(body), &info, &error, &line,
                                                                      ArrayView<hardware::v3d_shader::TgsiUniformBinding>(fixed_layout_uniform),
                                                                      ArrayView<const hardware::v3d_shader::TgsiInputBinding>(fixed_layout_inputs), false, false);
  CHECK(fixed_compiled);
  CHECK(info.uniform_count == 1 && fixed_layout_uniform[0].file == gcn::TgsiFile::None && fixed_layout_uniform[0].immediate_bits == 0x3F800000u);
}

static void test_varying_interpolation_and_output() {
  TestAllocator allocator;
  const char* fragment = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n0: MOV OUT[0], IN[0]\n1: END\n";
  uint64_t fragment_words[64] = {};
  hardware::v3d_shader::TgsiCompileInfo fragment_info;
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(fragment), ArrayView<uint64_t>(fragment_words), &fragment_info, &error, &line));
  CHECK(fragment_info.varying_input_count == 1 && fragment_info.instruction_count == 29 && fragment_info.register_count == 64);
  hardware::v3d_qpu::Instruction expected;
  expected.ldvary = true;
  expected.sig_dest = hardware::v3d_qpu::rf_dest(52);
  uint64_t expected_word = 0;
  CHECK(hardware::v3d_qpu::encode(expected, &expected_word));
  CHECK(fragment_words[4] == expected_word);

  const char* vertex = "VERT\nDCL IN[0], POSITION\nDCL IN[1], GENERIC[0]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
                      "0: MOV OUT[0], IN[0]\n1: MOV OUT[1], IN[1]\n2: END\n";
  uint64_t vertex_words[64] = {};
  hardware::v3d_shader::TgsiUniformBinding vertex_uniforms[16] = {};
  hardware::v3d_shader::TgsiInputBinding inputs[8] = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 0, true, 0x3F800000},
                                                      {1, 0, 3}, {1, 1, 4}, {1, 2, 5}, {1, 3, 6}};
  hardware::v3d_shader::TgsiCompileInfo vertex_info;
  const bool vertex_compiled = hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(vertex), ArrayView<uint64_t>(vertex_words), &vertex_info, &error, &line,
                                                                      ArrayView<hardware::v3d_shader::TgsiUniformBinding>(vertex_uniforms),
                                                                      ArrayView<const hardware::v3d_shader::TgsiInputBinding>(inputs), false, false);
  CHECK(vertex_compiled);
  CHECK(vertex_info.output_count == 2 && vertex_info.varying_output_count == 1 && vertex_info.output_register_base == 60);

  const char* flat = "FRAG\nDCL IN[0], GENERIC[0], CONSTANT\nDCL OUT[0], COLOR\n0: MOV OUT[0], IN[0]\n1: END\n";
  CHECK(!hardware::v3d_shader::compile_tgsi_alu(allocator, StringView(flat), ArrayView<uint64_t>(fragment_words), &fragment_info, &error, &line));
  CHECK(error && line == 0);
}

static void test_decimal_immediates_parse_without_host_float_runtime() {
  TestAllocator allocator;
  gcn::TgsiShader shader(allocator);
  const char* source = "VERT\nIMM[0] FLT32 {1.25, -0.5, 3.14159, 1e-3}\nEND\n";
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(gcn::parse_tgsi(allocator, StringView(source), &shader, &error, &line));
  CHECK(shader.immediates.count() == 1);
  if (shader.immediates.count() == 1) {
    CHECK(__builtin_bit_cast(uint32_t, shader.immediates[0].value[0]) == 0x3FA00000u);
    CHECK(__builtin_bit_cast(uint32_t, shader.immediates[0].value[1]) == 0xBF000000u);
    CHECK(__builtin_bit_cast(uint32_t, shader.immediates[0].value[2]) == 0x40490FD0u);
    CHECK(__builtin_bit_cast(uint32_t, shader.immediates[0].value[3]) == 0x3A83126Fu);
  }
}

int main() {
  test_arithmetic_lowering();
  test_rejections_and_bounds();
  test_uniform_sources();
  test_vertex_input_vpm_binding();
  test_reciprocal_sfu_lowering();
  test_division_lowering();
  test_fraction_lowering();
  test_negate_opcode_lowering();
  test_fragment_color_output();
  test_vertex_position_output();
  test_varying_interpolation_and_output();
  test_decimal_immediates_parse_without_host_float_runtime();
  if (failed) return 1;
  puts("V3dTgsiCompilerTest passed");
  return 0;
}
