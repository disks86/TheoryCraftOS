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

// glBegin/glEnd and the current values. A vertex between them is a row of a float array in a fixed layout (below); glEnd draws the rows like any other array, so one draw path exists.
#include "OpenGl/GlApi.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

namespace {
// The layout of a row, in floats.
constexpr uint32_t kPos = 0, kNormal = 4, kColor = 7, kSecondary = 11, kFog = 15, kTex = 16, kEdge = 16 + 4 * kMaxTexUnits, kGeneric = kEdge + 1;
constexpr uint32_t kGenericCount = webgl::kMaxAttribs - 1;      // attributes 1..15 (0 is the position)
constexpr uint32_t kRowFloats = kGeneric + 4 * kGenericCount;
}  // namespace

void GlApi::glBegin(GLenum mode) {
  if (in_begin_end_) { error(GL_INVALID_OPERATION); return; }
  if (mode > GL_POLYGON) { error(GL_INVALID_ENUM); return; }
  in_begin_end_ = true;
  imm_mode_ = mode;
  imm_.clear();
}

void GlApi::imm_vertex(float x, float y, float z, float w) {
  if (!in_begin_end_) return;      // undefined outside glBegin/glEnd: nothing
  const uint64_t row = imm_.count();
  if (!imm_.reserve(row + kRowFloats)) { error(GL_OUT_OF_MEMORY); return; }
  const float head[4] = {x, y, z, w};
  for (float v : head) imm_.push_back(v);
  for (float v : fx_.current_normal) imm_.push_back(v);
  for (float v : fx_.current_color) imm_.push_back(v);
  for (float v : fx_.current_secondary) imm_.push_back(v);
  imm_.push_back(fx_.current_fog);
  for (const auto& t : fx_.current_tex) for (float v : t) imm_.push_back(v);
  imm_.push_back(fx_.current_edge ? 1.0f : 0.0f);
  for (uint32_t g = 1; g < webgl::kMaxAttribs; ++g) for (float v : generic_current_[g]) imm_.push_back(v);
}

void GlApi::glEnd() {
  if (!in_begin_end_) { error(GL_INVALID_OPERATION); return; }
  in_begin_end_ = false;
  imm_end();
}

void GlApi::imm_end() {
  const uint32_t vertices = static_cast<uint32_t>(imm_.count() / kRowFloats);
  if (vertices == 0) return;
  // Whatever arrays are set up stay as they are: the rows are drawn through a copy of the vertex array state.
  webgl::Vao* vao = gl_.current_vao_ptr();
  const webgl::Vao saved = *vao;
  webgl::Buffer* buffer = gl_.buffer_at(imm_buffer_);
  if (!buffer) return;
  buffer->data.clear();
  // (float rows as their bytes: a trivial type, read back by the attribute fetch through offsets)
  if (!buffer->data.append(ConstByteView(reinterpret_cast<const uint8_t*>(imm_.view().data()), imm_.count() * sizeof(float)))) { error(GL_OUT_OF_MEMORY); return; }
  const auto layout = [&](uint32_t attr, uint32_t size, uint32_t first_float) {
    webgl::VertexAttrib& at = vao->attribs[attr];
    at.enabled = true;
    at.from_client = false;
    at.buffer = imm_buffer_;
    at.size = size;
    at.type = GL_FLOAT;
    at.normalized = false;
    at.integer = false;
    at.divisor = 0;
    at.stride = kRowFloats * sizeof(float);
    at.offset = uint64_t{first_float} * sizeof(float);
  };
  layout(kAttrVertex, 4, kPos);
  layout(kAttrNormal, 3, kNormal);
  layout(kAttrColor, 4, kColor);
  layout(kAttrSecondary, 4, kSecondary);
  layout(kAttrFog, 1, kFog);
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) layout(kAttrTex0 + i, 4, kTex + 4 * i);
  if (generic_set_mask_) for (uint32_t g = 1; g < webgl::kMaxAttribs; ++g) if (generic_set_mask_ & (1u << g)) layout(g, 4, kGeneric + 4 * (g - 1));
  // Edge flags as floats (the engine reads a byte array): only needed when a polygon mode shows edges.
  draw_any(imm_mode_, 0, static_cast<int32_t>(vertices), 0, UserPtr{}, false, 1, 0, 0);
  *vao = saved;
}

void GlApi::set_color(float r, float g, float b, float a) {
  fx_.current_color[0] = r; fx_.current_color[1] = g; fx_.current_color[2] = b; fx_.current_color[3] = a;
  if (fx_.color_material && fx_.lighting) apply_color_material(fx_.current_color);
}

void GlApi::set_secondary(float r, float g, float b) {
  fx_.current_secondary[0] = r; fx_.current_secondary[1] = g; fx_.current_secondary[2] = b;
}

void GlApi::set_normal(float x, float y, float z) {
  fx_.current_normal[0] = x; fx_.current_normal[1] = y; fx_.current_normal[2] = z;
}

void GlApi::set_texcoord(uint32_t unit, float s, float t, float r, float q) {
  if (unit >= kMaxTexUnits) { error(GL_INVALID_ENUM); return; }
  float* c = fx_.current_tex[unit];
  c[0] = s; c[1] = t; c[2] = r; c[3] = q;
}

void GlApi::set_fog_coord(float f) { fx_.current_fog = f; }
void GlApi::set_index(float i) { fx_.current_index = i; }
void GlApi::set_edge(bool on) { fx_.current_edge = on; }

void GlApi::set_attrib(uint32_t index, float x, float y, float z, float w) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  if (index == 0 && in_begin_end_) { imm_vertex(x, y, z, w); return; }      // attribute 0 is the vertex
  float* c = generic_current_[index];
  c[0] = x; c[1] = y; c[2] = z; c[3] = w;
  generic_set_mask_ |= 1u << index;
  const float v[4] = {x, y, z, w};
  gl_.vertex_attrib_value(index, v, nullptr);
}

void GlApi::set_attrib_int(uint32_t index, int32_t x, int32_t y, int32_t z, int32_t w, bool) {
  if (index >= webgl::kMaxAttribs) { error(GL_INVALID_VALUE); return; }
  const int32_t v[4] = {x, y, z, w};
  gl_.vertex_attrib_value(index, nullptr, v);
}

void GlApi::set_attrib_double(uint32_t index, double x, double y, double z, double w) {
  set_attrib(index, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w));
}

void GlApi::rect(float x1, float y1, float x2, float y2) {
  if (in_begin_end_) { error(GL_INVALID_OPERATION); return; }
  glBegin(GL_POLYGON);
  imm_vertex(x1, y1, 0, 1);
  imm_vertex(x2, y1, 0, 1);
  imm_vertex(x2, y2, 0, 1);
  imm_vertex(x1, y2, 0, 1);
  glEnd();
}

}  // namespace opengl
