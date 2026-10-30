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

void textured_vertex(const Vec4* attributes, const uint8_t* constants, ShaderIo* out) {
  const Mat4& mvp = *reinterpret_cast<const Mat4*>(constants);
  out->position = mvp * Vec4{attributes[0].x, attributes[0].y, attributes[0].z, 1.0f};
  out->varying[0] = attributes[1];
}

bool textured_fragment(const Vec4* varyings, const uint8_t*, const ITextureSet& textures, Vec4* color) {
  *color = textures.sample(0, varyings[0].x, varyings[0].y);
  return true;
}

bool textured_scaled_fragment(const Vec4* varyings, const uint8_t* constants, const ITextureSet& textures, Vec4* color) {
  const Vec4 texel = textures.sample(0, varyings[0].x, varyings[0].y);
  const float k = reinterpret_cast<const float*>(constants)[16];   // (the push constant after the matrix)
  *color = Vec4{texel.x * k, texel.y * k, texel.z * k, texel.w * k};
  return true;
}

// Group filters. The blur: 17 samples along one direction, the center and eight each way at multiples of the step (CONST[7].xy, in texture coordinates), weighted by
// the nine weights in CONST[4..6] (center first, mirrored): one pass of a box blur. The color function: the texel is made straight, goes through
// rgb' = rows * rgb + add (CONST[4..6], the add in .w), is clamped, and is made premultiplied again with alpha * CONST[7].x.
bool textured_blur_fragment(const Vec4* varyings, const uint8_t* constants, const ITextureSet& textures, Vec4* color) {
  const float* c = reinterpret_cast<const float*>(constants);
  const float* weight = c + 16;
  const float sx = c[28], sy = c[29];
  Vec4 sum = textures.sample(0, varyings[0].x, varyings[0].y);
  sum = Vec4{sum.x * weight[0], sum.y * weight[0], sum.z * weight[0], sum.w * weight[0]};
  for (int i = 1; i <= 8; ++i) {
    const float k = static_cast<float>(i), w = weight[i];
    const Vec4 a = textures.sample(0, varyings[0].x + sx * k, varyings[0].y + sy * k);
    const Vec4 b = textures.sample(0, varyings[0].x - sx * k, varyings[0].y - sy * k);
    sum = Vec4{sum.x + (a.x + b.x) * w, sum.y + (a.y + b.y) * w, sum.z + (a.z + b.z) * w, sum.w + (a.w + b.w) * w};
  }
  *color = sum;
  return true;
}

bool textured_color_fragment(const Vec4* varyings, const uint8_t* constants, const ITextureSet& textures, Vec4* color) {
  const float* c = reinterpret_cast<const float*>(constants);
  const Vec4 t = textures.sample(0, varyings[0].x, varyings[0].y);
  const float inv = 1.0f / (t.w > 0.0001f ? t.w : 0.0001f);
  const float r = t.x * inv, g = t.y * inv, b = t.z * inv;
  auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; };
  const float nr = clamp01(c[16] * r + c[17] * g + c[18] * b + c[19]);
  const float ng = clamp01(c[20] * r + c[21] * g + c[22] * b + c[23]);
  const float nb = clamp01(c[24] * r + c[25] * g + c[26] * b + c[27]);
  const float a = t.w * c[28];
  *color = Vec4{nr * a, ng * a, nb * a, a};
  return true;
}

// The shadow of a picture: its alpha moved by CONST[5].xy (texture coordinates), tinted with the straight color CONST[4], made premultiplied.
bool textured_shadow_fragment(const Vec4* varyings, const uint8_t* constants, const ITextureSet& textures, Vec4* color) {
  const float* c = reinterpret_cast<const float*>(constants);
  const float a = textures.sample(0, varyings[0].x - c[20], varyings[0].y - c[21]).w * c[19];
  *color = Vec4{c[16] * a, c[17] * a, c[18] * a, a};
  return true;
}

// A picture through a coverage mask: texture 0 (premultiplied colours) times the alpha of texture 1, at the same coordinate.
bool textured_masked_fragment(const Vec4* varyings, const uint8_t*, const ITextureSet& textures, Vec4* color) {
  const Vec4 t = textures.sample(0, varyings[0].x, varyings[0].y);
  const float k = textures.sample(1, varyings[0].x, varyings[0].y).w;
  *color = Vec4{t.x * k, t.y * k, t.z * k, t.w * k};
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

// The textured pair: the vertex shader is the coloured one (the second attribute is a float2 texture coordinate, which arrives as (u, v, 0, 1)); the fragment shader
// writes the texel of the sampler at that coordinate.
constexpr const char kTexturedFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n"
    "  1: END\n";

// The textured fragment shader that scales the texel (all four channels) by CONST[4].x, the float after the matrix.
constexpr const char kTexturedScaledFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[4]\n"
    "DCL TEMP[0]\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: MUL OUT[0], TEMP[0], CONST[4].xxxx\n"
    "  2: END\n";

constexpr const char kTexturedBlurFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[4..7]\n"
    "DCL TEMP[0..2]\n"
    "IMM[0] FLT32 { 1.0, 2.0, 3.0, 4.0}\n"
    "IMM[1] FLT32 { 5.0, 6.0, 7.0, 8.0}\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: MUL TEMP[1], TEMP[0], CONST[4].xxxx\n"
    "  2: MAD TEMP[2].xy, CONST[7].xyxx, IMM[0].xxxx, IN[0]\n"
    "  3: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  4: MAD TEMP[1], TEMP[0], CONST[4].yyyy, TEMP[1]\n"
    "  5: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[0].xxxx, IN[0]\n"
    "  6: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  7: MAD TEMP[1], TEMP[0], CONST[4].yyyy, TEMP[1]\n"
    "  8: MAD TEMP[2].xy, CONST[7].xyxx, IMM[0].yyyy, IN[0]\n"
    "  9: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  10: MAD TEMP[1], TEMP[0], CONST[4].zzzz, TEMP[1]\n"
    "  11: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[0].yyyy, IN[0]\n"
    "  12: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  13: MAD TEMP[1], TEMP[0], CONST[4].zzzz, TEMP[1]\n"
    "  14: MAD TEMP[2].xy, CONST[7].xyxx, IMM[0].zzzz, IN[0]\n"
    "  15: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  16: MAD TEMP[1], TEMP[0], CONST[4].wwww, TEMP[1]\n"
    "  17: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[0].zzzz, IN[0]\n"
    "  18: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  19: MAD TEMP[1], TEMP[0], CONST[4].wwww, TEMP[1]\n"
    "  20: MAD TEMP[2].xy, CONST[7].xyxx, IMM[0].wwww, IN[0]\n"
    "  21: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  22: MAD TEMP[1], TEMP[0], CONST[5].xxxx, TEMP[1]\n"
    "  23: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[0].wwww, IN[0]\n"
    "  24: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  25: MAD TEMP[1], TEMP[0], CONST[5].xxxx, TEMP[1]\n"
    "  26: MAD TEMP[2].xy, CONST[7].xyxx, IMM[1].xxxx, IN[0]\n"
    "  27: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  28: MAD TEMP[1], TEMP[0], CONST[5].yyyy, TEMP[1]\n"
    "  29: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[1].xxxx, IN[0]\n"
    "  30: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  31: MAD TEMP[1], TEMP[0], CONST[5].yyyy, TEMP[1]\n"
    "  32: MAD TEMP[2].xy, CONST[7].xyxx, IMM[1].yyyy, IN[0]\n"
    "  33: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  34: MAD TEMP[1], TEMP[0], CONST[5].zzzz, TEMP[1]\n"
    "  35: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[1].yyyy, IN[0]\n"
    "  36: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  37: MAD TEMP[1], TEMP[0], CONST[5].zzzz, TEMP[1]\n"
    "  38: MAD TEMP[2].xy, CONST[7].xyxx, IMM[1].zzzz, IN[0]\n"
    "  39: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  40: MAD TEMP[1], TEMP[0], CONST[5].wwww, TEMP[1]\n"
    "  41: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[1].zzzz, IN[0]\n"
    "  42: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  43: MAD TEMP[1], TEMP[0], CONST[5].wwww, TEMP[1]\n"
    "  44: MAD TEMP[2].xy, CONST[7].xyxx, IMM[1].wwww, IN[0]\n"
    "  45: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  46: MAD TEMP[1], TEMP[0], CONST[6].xxxx, TEMP[1]\n"
    "  47: MAD TEMP[2].xy, CONST[7].xyxx, -IMM[1].wwww, IN[0]\n"
    "  48: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  49: MAD TEMP[1], TEMP[0], CONST[6].xxxx, TEMP[1]\n"
    "  50: MOV OUT[0], TEMP[1]\n"
    "  51: END\n";

constexpr const char kTexturedColorFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[4..7]\n"
    "DCL TEMP[0..3]\n"
    "IMM[0] FLT32 { 0.0001, 1.0, 0.0, 0.0}\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: MAX TEMP[1].x, TEMP[0].wwww, IMM[0].xxxx\n"
    "  2: RCP TEMP[1].x, TEMP[1].xxxx\n"
    "  3: MUL TEMP[2].xyz, TEMP[0].xyzx, TEMP[1].xxxx\n"
    "  4: DP3 TEMP[3].x, CONST[4], TEMP[2]\n"
    "  5: ADD TEMP[3].x, TEMP[3].xxxx, CONST[4].wwww\n"
    "  6: DP3 TEMP[3].y, CONST[5], TEMP[2]\n"
    "  7: ADD TEMP[3].y, TEMP[3].yyyy, CONST[5].wwww\n"
    "  8: DP3 TEMP[3].z, CONST[6], TEMP[2]\n"
    "  9: ADD TEMP[3].z, TEMP[3].zzzz, CONST[6].wwww\n"
    "  10: MAX TEMP[3].xyz, TEMP[3].xyzx, IMM[0].zzzz\n"
    "  11: MIN TEMP[3].xyz, TEMP[3].xyzx, IMM[0].yyyy\n"
    "  12: MUL TEMP[1].w, TEMP[0].wwww, CONST[7].xxxx\n"
    "  13: MUL OUT[0].xyz, TEMP[3].xyzx, TEMP[1].wwww\n"
    "  14: MOV OUT[0].w, TEMP[1].wwww\n"
    "  15: END\n";

constexpr const char kTexturedMaskedFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SAMP[1]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL SVIEW[1], 2D, FLOAT\n"
    "DCL TEMP[0..1]\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: TEX TEMP[1], IN[0], SAMP[1], 2D\n"
    "  2: MUL OUT[0], TEMP[0], TEMP[1].wwww\n"
    "  3: END\n";

constexpr const char kTexturedShadowFragmentTgsi[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[4..5]\n"
    "DCL TEMP[0..2]\n"
    "  0: ADD TEMP[0].xy, IN[0].xyxx, -CONST[5].xyxx\n"
    "  1: TEX TEMP[1], TEMP[0], SAMP[0], 2D\n"
    "  2: MUL TEMP[2].x, TEMP[1].wwww, CONST[4].wwww\n"
    "  3: MUL OUT[0].xyz, CONST[4].xyzx, TEMP[2].xxxx\n"
    "  4: MOV OUT[0].w, TEMP[2].xxxx\n"
    "  5: END\n";

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
  d.qpu_program = QpuProgram::ColoredVertex;
  d.eu_program = EuProgram::ColoredVertex;
  return d;
}

ShaderModuleDesc colored_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.fragment_function = colored_fragment;
  d.tgsi = kColoredFragmentTgsi;
  d.d3d9_tokens = kColoredFragmentD3d9;
  d.d3d9_token_count = sizeof kColoredFragmentD3d9 / 4;
  d.qpu_program = QpuProgram::ColoredFragment;
  d.eu_program = EuProgram::ColoredFragment;
  return d;
}

ShaderModuleDesc textured_vertex_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Vertex;
  d.vertex_function = textured_vertex;
  d.tgsi = kColoredVertexTgsi;
  d.eu_program = EuProgram::TexturedVertex;
  return d;
}

ShaderModuleDesc textured_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.textured_fragment_function = textured_fragment;
  d.tgsi = kTexturedFragmentTgsi;
  d.eu_program = EuProgram::TexturedFragment;
  return d;
}

ShaderModuleDesc textured_scaled_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.textured_fragment_function = textured_scaled_fragment;
  d.tgsi = kTexturedScaledFragmentTgsi;   // (no Direct3D 9, QPU or Intel program: those devices refuse the pipeline)
  return d;
}

ShaderModuleDesc textured_blur_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.textured_fragment_function = textured_blur_fragment;
  d.tgsi = kTexturedBlurFragmentTgsi;
  return d;
}

ShaderModuleDesc textured_color_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.textured_fragment_function = textured_color_fragment;
  d.tgsi = kTexturedColorFragmentTgsi;
  return d;
}

ShaderModuleDesc textured_masked_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.textured_fragment_function = textured_masked_fragment;
  d.tgsi = kTexturedMaskedFragmentTgsi;
  return d;
}

ShaderModuleDesc textured_shadow_fragment_shader() {
  ShaderModuleDesc d;
  d.stage = ShaderStage::Fragment;
  d.textured_fragment_function = textured_shadow_fragment;
  d.tgsi = kTexturedShadowFragmentTgsi;
  return d;
}

void describe_textured_pipeline(GraphicsPipelineDesc* desc) {
  desc->vertex_stride = sizeof(TexturedVertex);
  desc->attribute_count = 2;
  desc->attributes[0] = {0, VertexFormat::Float3, 0};
  desc->attributes[1] = {1, VertexFormat::Float2, 12};
  desc->varying_count = 1;
  desc->texture_count = 1;
}

void describe_colored_pipeline(GraphicsPipelineDesc* desc) {
  desc->vertex_stride = sizeof(ColoredVertex);
  desc->attribute_count = 2;
  desc->attributes[0] = {0, VertexFormat::Float3, 0};
  desc->attributes[1] = {1, VertexFormat::Float4, 12};
  desc->varying_count = 1;
}

}  // namespace graphics
