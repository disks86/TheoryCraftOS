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
}  // namespace

ShaderModuleDesc colored_vertex_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Vertex;
  d.vertex_function = colored_vertex;
  d.tgsi = kColoredVertexTgsi;
  return d;
}

ShaderModuleDesc colored_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.fragment_function = colored_fragment;
  d.tgsi = kColoredFragmentTgsi;
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
