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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaDraw.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace gcn;

namespace {

uint64_t align_up(uint64_t v) { return (v + AmdVegaDraw::kAlignment - 1) / AmdVegaDraw::kAlignment * AmdVegaDraw::kAlignment; }

struct Plan {
  uint64_t vertex_code, pixel_code, vertex_constants, pixel_constants, table, vertices, indices, target, total;
};

// constants the shaders read are padded to the 64 rows the command processor model probes for, and to at least what the shaders say
constexpr uint64_t kConstantBytes = TgsiShader::kMaxConsts * 16;

bool make_plan(const VegaDrawRequest& r, Plan* p) {
  if (r.width == 0 || r.height == 0 || r.width > 16384 || r.height > 16384 || r.vertex_code.size() == 0 || r.pixel_code.size() == 0 || r.vertex_data.size() == 0 || r.vertex_stride == 0) return false;
  if (r.vertex_info.const_rows * 4ull > r.vertex_constants.size() || r.pixel_info.const_rows * 4ull > r.pixel_constants.size() || r.vertex_constants.size() > kConstantBytes / 4 ||
      r.pixel_constants.size() > kConstantBytes / 4)
    return false;
  uint64_t at = 0;
  auto place = [&](uint64_t bytes) { const uint64_t start = at; at = align_up(at + bytes); return start; };
  p->vertex_code = place(r.vertex_code.size() * 4ull);
  p->pixel_code = place(r.pixel_code.size() * 4ull);
  p->vertex_constants = place(kConstantBytes);
  p->pixel_constants = place(kConstantBytes);
  p->table = place(abi::kTableBytes);
  p->vertices = place(r.vertex_data.size() * 4ull);
  p->indices = place(r.indices.size() == 0 ? 4 : r.indices.size() * 4ull);
  p->target = place(AmdVegaDraw::target_bytes(r));
  p->total = at;
  return at <= (1ull << 31);
}

}  // namespace

uint64_t AmdVegaDraw::area_bytes(const VegaDrawRequest& r) {
  Plan p;
  return make_plan(r, &p) ? p.total : 0;
}

bool AmdVegaDraw::draw(AmdVegaM& card, const VegaDrawRequest& r, uint64_t area_offset, ByteView staging, ByteView pixels) {
  Plan p;
  if (!card.up() || (area_offset & (kAlignment - 1)) != 0 || !make_plan(r, &p)) return false;
  if (staging.size() < p.total || pixels.size() < target_bytes(r) || area_offset > card.layout().framebuffer_bytes || p.total > card.layout().framebuffer_bytes - area_offset) return false;
  const uint32_t vertex_count = r.indices.size() != 0 ? static_cast<uint32_t>(r.indices.size()) : r.vertex_count;
  if (vertex_count == 0 || vertex_count % 3 != 0 || r.indices.size() > 0x10000000u) return false;
  const uint64_t base = card.framebuffer_gpu_address() + area_offset;

  // the staging image of the area
  ByteWriter w(staging.first(p.total));
  w.zeros(p.total);
  auto put_words = [&](uint64_t offset, ArrayView<const uint32_t> words) {
    for (uint64_t i = 0; i < words.size(); ++i) {
      const uint32_t v = words[i];
      const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
      w.patch_bytes(offset + 4 * i, ConstByteView(b, 4));
    }
  };
  auto float_word = [](float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; };
  put_words(p.vertex_code, r.vertex_code);
  put_words(p.pixel_code, r.pixel_code);
  for (uint64_t i = 0; i < r.vertex_constants.size(); ++i) { const uint32_t v = float_word(r.vertex_constants[i]); put_words(p.vertex_constants + 4 * i, ArrayView<const uint32_t>(&v, 1)); }
  for (uint64_t i = 0; i < r.pixel_constants.size(); ++i) { const uint32_t v = float_word(r.pixel_constants[i]); put_words(p.pixel_constants + 4 * i, ArrayView<const uint32_t>(&v, 1)); }
  const uint64_t vertex_records = static_cast<uint64_t>(r.vertex_data.size()) * 4 / r.vertex_stride;
  for (uint32_t a = 0; a < abi::kMaxAttributes; ++a) {
    if (!(r.vertex_info.attributes_used >> a & 1)) continue;
    const VertexAttribute& at = r.attributes[a];
    uint32_t d[4];
    if (at.components < 1 || at.components > 4 || at.offset > r.vertex_stride || r.vertex_stride - at.offset < at.components * 4u || vertex_records == 0 || vertex_records > 0xFFFFFFFFu ||
        !make_vertex_descriptor(ArrayView<uint32_t>(d, 4), base + p.vertices + at.offset, r.vertex_stride, static_cast<uint32_t>(vertex_records), at.components))
      return false;
    put_words(p.table + a * abi::kVertexBufferBytes, ArrayView<const uint32_t>(d, 4));
  }
  put_words(p.vertices, r.vertex_data);
  if (r.indices.size() != 0) put_words(p.indices, r.indices);
  for (uint64_t i = 0; i < static_cast<uint64_t>(pitch_pixels(r.width)) * r.height; ++i)
    for (uint32_t c = 0; c < 4; ++c) { const uint32_t v = float_word(r.clear[c]); put_words(p.target + (i * 4 + c) * 4, ArrayView<const uint32_t>(&v, 1)); }
  if (!w.ok() || !card.upload(ConstByteView(staging.data(), p.total), area_offset)) return false;

  // the stream
  DrawState s;
  s.vertex.address = base + p.vertex_code;
  s.vertex.info = r.vertex_info;
  s.pixel.address = base + p.pixel_code;
  s.pixel.info = r.pixel_info;
  s.vertex_constants = base + p.vertex_constants;
  s.pixel_constants = base + p.pixel_constants;
  s.descriptor_table = base + p.table;
  s.target.address = base + p.target;
  s.target.width = r.width;
  s.target.height = r.height;
  s.target.pitch_pixels = pitch_pixels(r.width);
  for (uint32_t k = 0; k < 16; ++k) s.ps_param[k] = r.ps_param[k];
  s.index_count = vertex_count;
  if (r.indices.size() != 0) {
    s.index_address = base + p.indices;
    s.index_max = vertex_count;
  }
  uint32_t stream[AmdVegaM::kIbDwords / 2];
  Pm4Writer out(ArrayView<uint32_t>(stream, AmdVegaM::kIbDwords / 2));
  if (!emit_draw(out, s) || !out.ok()) return false;
  if (!card.submit_graphics(out.words())) return false;
  return card.download(ByteView(pixels.data(), target_bytes(r)), area_offset + p.target);
}

}  // namespace hardware
