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

#include "Graphics/Graphics3D/BuiltinShaders.hpp"

namespace graphics {

namespace {
void colored_vertex(const Vec4* attributes, const uint8_t* constants, ShaderIo* out) {
  const Mat4& mvp = *reinterpret_cast<const Mat4*>(constants);
  out->position = mvp * Vec4{attributes[0].x, attributes[0].y, attributes[0].z, 1.0f};
  out->varying[0] = attributes[1];
}

bool colored_fragment(const Vec4* varyings, const uint8_t*, Vec4* color) {
  *color = varyings[0];
  return true;
}

// Push constants are CONST[0..3], the columns of the matrix.
constexpr const char kColoredVertexTgsi[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "DCL CONST[0..3]\n"
    "DCL TEMP[0]\n"
    "  0: MUL TEMP[0], CONST[0], IN[0].xxxx\n"
    "  1: MAD TEMP[0], CONST[1], IN[0].yyyy, TEMP[0]\n"
    "  2: MAD TEMP[0], CONST[2], IN[0].zzzz, TEMP[0]\n"
    "  3: ADD OUT[0], TEMP[0], CONST[3]\n"
    "  4: MOV OUT[1], IN[1]\n"
    "  5: END\n";

constexpr const char kColoredFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: END\n";

// The same two shaders as Direct3D 9 bytecode (vs_2_0, ps_2_0). Position comes in as v0, color as v1; the matrix columns are c0..c3, and c31 is
// the device's (1, -1, 1, 1) that turns Direct3D's +Y up into Vulkan's +Y down.
constexpr uint32_t kColoredVertexD3d9[] = {
    0xFFFE0200,                                                   // vs_2_0
    0x0200001F, 0x80000000, 0x900F0000,                           // dcl_position v0
    0x0200001F, 0x8000000A, 0x900F0001,                           // dcl_color v1
    0x03000005, 0x800F0000, 0xA0E40000, 0x90000000,              // mul r0, c0, v0.xxxx
    0x04000004, 0x800F0000, 0xA0E40001, 0x90550000, 0x80E40000,  // mad r0, c1, v0.yyyy, r0
    0x04000004, 0x800F0000, 0xA0E40002, 0x90AA0000, 0x80E40000,  // mad r0, c2, v0.zzzz, r0
    0x03000002, 0x800F0001, 0x80E40000, 0xA0E40003,              // add r1, r0, c3
    0x03000005, 0xC00F0000, 0x80E40001, 0xA0E4001F,              // mul oPos, r1, c31
    0x02000001, 0xD00F0000, 0x90E40001,                           // mov oD0, v1
    0x0000FFFF,                                                   // end
};
constexpr uint32_t kColoredFragmentD3d9[] = {
    0xFFFF0200,                                                   // ps_2_0
    0x02000001, 0x800F0800, 0x90E40000,                           // mov oC0, v0
    0x0000FFFF,
};
}  // namespace

ShaderModuleDesc colored_vertex_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Vertex;
  d.vertex_function = colored_vertex;
  d.tgsi = kColoredVertexTgsi;
  d.d3d9_tokens = kColoredVertexD3d9;
  d.d3d9_token_count = sizeof kColoredVertexD3d9 / 4;
  return d;
}

ShaderModuleDesc colored_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.fragment_function = colored_fragment;
  d.tgsi = kColoredFragmentTgsi;
  d.d3d9_tokens = kColoredFragmentD3d9;
  d.d3d9_token_count = sizeof kColoredFragmentD3d9 / 4;
  return d;
}

void describe_colored_pipeline(GraphicsPipelineDesc* desc) {
  desc->vertex_stride = sizeof(ColoredVertex);
  desc->attribute_count = 2;
  desc->attributes[0] = {0, VertexFormat::Float3, 0};
  desc->attributes[1] = {1, VertexFormat::Float4, 12};
  desc->varying_count = 1;
}

}  // namespace graphics
