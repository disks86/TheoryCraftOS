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

#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnDraw.hpp"

namespace gcn {

namespace {

constexpr uint64_t kVertexConstAddr = 0x10000, kPixelConstAddr = 0x20000, kTableAddr = 0x30000, kVertexBufferAddr = 0x1000000;

struct Region {
  explicit Region(allocators::IAllocator& allocator) : words(allocator) {}
  uint64_t base = 0;
  List<uint32_t> words;
};

bool fill(List<uint32_t>& list, uint64_t count) {
  if (!list.reserve(count)) return false;
  for (uint64_t i = 0; i < count; ++i)
    if (!list.push_back(0u)) return false;
  return true;
}

bool fill_f(List<float>& list, uint64_t count) {
  if (!list.reserve(count)) return false;
  for (uint64_t i = 0; i < count; ++i)
    if (!list.push_back(0.0f)) return false;
  return true;
}

bool fill_bytes(List<uint8_t>& list, uint64_t count) {
  if (!list.reserve(count)) return false;
  for (uint64_t i = 0; i < count; ++i)
    if (!list.push_back(uint8_t{0})) return false;
  return true;
}

bool compare(uint32_t func, float a, float b) {
  switch (func) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
  }
}

uint8_t stencil_result(uint32_t op, uint8_t old, uint8_t ref, uint8_t value) {
  switch (op) {
    case 1: return 0;
    case 2: return 0xFF;
    case 3: return ref;
    case 4: return value;
    case 5: return old + value > 255 ? 255 : static_cast<uint8_t>(old + value);
    case 6: return old < value ? 0 : static_cast<uint8_t>(old - value);
    case 7: return static_cast<uint8_t>(~old);
    case 8: return static_cast<uint8_t>(old + value);
    case 9: return static_cast<uint8_t>(old - value);
    default: return old;
  }
}

uint8_t stencil_update(const DepthStencilState& d, uint32_t op, uint8_t old) {
  const uint8_t result = stencil_result(op, old, d.stencil_ref, d.stencil_op_value);
  return static_cast<uint8_t>((old & ~d.stencil_write_mask) | (result & d.stencil_write_mask));
}

bool factor_ok(uint32_t f) { return BlendState::supported_factor(f); }

// the blend factor of channel `ch` (3 is alpha)
float blend_factor(uint32_t f, const float s[4], const float d[4], const float c[4], uint32_t ch) {
  switch (f) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return s[ch];
    case 3: return 1.0f - s[ch];
    case 4: return s[3];
    case 5: return 1.0f - s[3];
    case 6: return d[3];
    case 7: return 1.0f - d[3];
    case 8: return d[ch];
    case 9: return 1.0f - d[ch];
    case 10: return ch == 3 ? 1.0f : (s[3] < 1.0f - d[3] ? s[3] : 1.0f - d[3]);
    case 13: return c[ch];
    case 14: return 1.0f - c[ch];
    case 19: return c[3];
    case 20: return 1.0f - c[3];
    default: return 0.0f;
  }
}

float blend_combine(uint32_t op, float s, float sf, float d, float df) {
  switch (op) {
    case 1: return s * sf - d * df;
    case 2: return s < d ? s : d;
    case 3: return s > d ? s : d;
    case 4: return d * df - s * sf;
    default: return s * sf + d * df;
  }
}

float bits_to_float(uint32_t b) {
  union { uint32_t u; float f; } c;       // (the exports are single precision bit patterns)
  c.u = b;
  return c.f;
}

class Machine final : public IEnvironment {
 public:
  explicit Machine(allocators::IAllocator& allocator)
      : vertex_const_(allocator), pixel_const_(allocator), table_(allocator), vertices_(allocator), positions_(allocator), params_(allocator) {}

  Region vertex_const_, pixel_const_, table_, vertices_;
  List<float> positions_;                  // per vertex slot: x y z w
  List<float> params_;                     // per parameter, per vertex slot: 4 floats
  uint32_t slots_ = 0;
  uint32_t param_count_ = 0;
  const uint8_t* ps_param_ = nullptr;

  // the vertex wave being run
  uint32_t batch_base_ = 0;
  // the pixel wave being run
  bool pixel_stage_ = false;
  uint32_t tri_[3] = {};
  const uint32_t* pixel_x_ = nullptr;
  const uint32_t* pixel_y_ = nullptr;
  Framebuffer* target_ = nullptr;
  ITextureSource* textures_ = nullptr;
  ITexelMemory* texel_memory_ = nullptr;
  const BlendState* blend_ = nullptr;
  uint64_t exported_ = 0;                  // the lanes of the pixel wave that wrote a colour
  bool fault_ = false;

  Region* find(uint64_t address) {
    Region* all[4] = {&vertex_const_, &pixel_const_, &table_, &vertices_};
    for (Region* r : all) {
      if (address >= r->base && (address - r->base) / 4 < r->words.count() && (address & 3) == 0) return r;
    }
    return nullptr;
  }

  bool load32(uint64_t address, uint32_t* value) override {
    Region* r = find(address);
    if (!r) return false;
    *value = r->words[(address - r->base) / 4];
    return true;
  }
  bool store32(uint64_t, uint32_t) override { return false; }

  void interpolation(uint32_t attr, uint32_t chan, uint32_t, float* p0, float* p10, float* p20) override {
    float v[3] = {0, 0, 0};
    const uint32_t param = attr < 16 ? ps_param_[attr] : 0xFFFFu;
    if (param < param_count_ && chan < 4) {
      for (uint32_t k = 0; k < 3; ++k) v[k] = params_[(static_cast<uint64_t>(param) * slots_ + tri_[k]) * 4 + chan];
    }
    *p0 = v[0];
    *p10 = v[1] - v[0];
    *p20 = v[2] - v[0];
  }

  void sample(Mimg, const uint32_t image[8], const uint32_t sampler[4], const float* coords, uint32_t, float result[4]) override {
    result[0] = result[1] = result[2] = result[3] = 0.0f;
    if ((image[3] >> 28) != 0) {                // a real T# (its resource type is set); the harness's own unit numbers have none
      if (!texel_memory_ || sample_image(*texel_memory_, ArrayView<const uint32_t>(image, 8), ArrayView<const uint32_t>(sampler, 4), coords[0], coords[1], result) != SampleStatus::Ok) {
        fault_ = true;
        result[0] = result[1] = result[2] = result[3] = 0.0f;
      }
      return;
    }
    if (textures_) textures_->sample(image[0], coords[0], coords[1], result);
  }

  void export_data(uint32_t target, uint32_t enable, bool compressed, const uint32_t values[kWaveSize][4], uint64_t lanes, bool) override {
    if (compressed) return;
    if (pixel_stage_) {
      if (target != kExpMrt0) return;
      for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
        if (!(lanes >> lane & 1)) continue;
        float* px = target_->pixel(pixel_x_[lane], pixel_y_[lane]);
        if (!px) continue;
        exported_ |= uint64_t{1} << lane;
        float src[4];
        for (uint32_t c = 0; c < 4; ++c) src[c] = enable >> c & 1 ? bits_to_float(values[lane][c]) : 0.0f;
        const BlendState& b = *blend_;
        if (b.enable) {
          const float dst[4] = {px[0], px[1], px[2], px[3]};
          float out[4];
          for (uint32_t c = 0; c < 4; ++c) {
            const bool alpha = c == 3 && b.separate_alpha;
            const uint32_t sf = alpha ? b.alpha_src : b.color_src, df = alpha ? b.alpha_dst : b.color_dst, op = alpha ? b.alpha_op : b.color_op;
            out[c] = blend_combine(op, src[c], blend_factor(sf, src, dst, b.constant, c), dst[c], blend_factor(df, src, dst, b.constant, c));
          }
          for (uint32_t c = 0; c < 4; ++c) src[c] = out[c];
        }
        for (uint32_t c = 0; c < 4; ++c)
          if ((enable >> c & 1) && (b.write_mask >> c & 1)) px[c] = src[c];
      }
      return;
    }
    float* out = nullptr;
    if (target == kExpPos0) {
      out = &positions_[0];
    } else if (target >= kExpParam0 && target - kExpParam0 < param_count_) {
      out = &params_[static_cast<uint64_t>(target - kExpParam0) * slots_ * 4];
    } else {
      return;
    }
    for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
      if (!(lanes >> lane & 1)) continue;
      const uint64_t slot = static_cast<uint64_t>(batch_base_) + lane;
      if (slot >= slots_) continue;
      for (uint32_t c = 0; c < 4; ++c)
        if (enable >> c & 1) out[slot * 4 + c] = bits_to_float(values[lane][c]);
    }
  }
};

struct Screen {
  float x, y, z, inv_w;
  bool valid;
};

}  // namespace

Framebuffer::Framebuffer(allocators::IAllocator& allocator, uint32_t width, uint32_t height) : rgba_(allocator), width_(width), height_(height) {
  valid_ = width != 0 && height != 0 && width <= 16384 && height <= 16384 && fill_f(rgba_, static_cast<uint64_t>(width) * height * 4);
}

void Framebuffer::clear(float r, float g, float b, float a) {
  const float c[4] = {r, g, b, a};
  for (uint64_t i = 0; i < rgba_.count(); ++i) rgba_[i] = c[i & 3];
}

DepthBuffer::DepthBuffer(allocators::IAllocator& allocator, uint32_t width, uint32_t height) : z_(allocator), s_(allocator), width_(width), height_(height) {
  const uint64_t count = static_cast<uint64_t>(width) * height;
  valid_ = width != 0 && height != 0 && width <= 16384 && height <= 16384 && fill_f(z_, count) && fill_bytes(s_, count);
}

void DepthBuffer::clear(float depth, uint8_t stencil) {
  for (uint64_t i = 0; i < z_.count(); ++i) z_[i] = depth;
  for (uint64_t i = 0; i < s_.count(); ++i) s_[i] = stencil;
}

DrawStatus draw(allocators::IAllocator& allocator, const DrawCall& call, Framebuffer& target, DrawStats* stats) {
  DrawStats local;
  DrawStats& st = stats ? *stats : local;
  st = DrawStats();
  if (!target.valid() || call.vertex_info.fragment || !call.pixel_info.fragment || call.vertex_code.size() == 0 || call.pixel_code.size() == 0) return DrawStatus::BadInput;
  if (call.indices.size() % 3 != 0 || call.vertex_stride == 0 || call.vertex_stride > 16383 || (call.vertex_stride & 3) != 0 || call.vertex_data.size() == 0) return DrawStatus::BadInput;
  if (call.vertex_info.const_rows > call.vertex_constants.size() / 4 || call.pixel_info.const_rows > call.pixel_constants.size() / 4) return DrawStatus::BadInput;
  if (call.indices.size() > 0x10000000u) return DrawStatus::BadInput;
  const uint64_t vertex_count = static_cast<uint64_t>(call.vertex_data.size()) * 4 / call.vertex_stride;
  for (const uint32_t index : call.indices)
    if (index >= vertex_count) return DrawStatus::BadInput;
  for (uint32_t a = 0; a < abi::kMaxAttributes; ++a) {
    const VertexAttribute& at = call.attributes[a];
    if (!(call.vertex_info.attributes_used >> a & 1)) continue;
    if (at.components < 1 || at.components > 4 || at.offset > call.vertex_stride || call.vertex_stride - at.offset < at.components * 4u) return DrawStatus::BadInput;
  }

  const BlendState& bl = call.blend;
  const DepthStencilState& ds = call.depth_stencil;
  const bool depth_used = ds.depth_test || ds.depth_write || ds.stencil_test;
  if (!factor_ok(bl.color_src) || !factor_ok(bl.color_dst) || !factor_ok(bl.alpha_src) || !factor_ok(bl.alpha_dst) || bl.color_op > 4 || bl.alpha_op > 4 || (bl.write_mask & ~0xFu) != 0)
    return DrawStatus::BadInput;
  if (ds.depth_func > 7 || ds.stencil_func > 7 || ds.fail_op > 9 || ds.zpass_op > 9 || ds.zfail_op > 9) return DrawStatus::BadInput;
  if (depth_used && (!call.depth || !call.depth->valid() || call.depth->width() != target.width() || call.depth->height() != target.height())) return DrawStatus::BadInput;

  Machine m(allocator);
  m.blend_ = &call.blend;
  m.texel_memory_ = call.texel_memory;
  m.ps_param_ = call.ps_param;            // (set from the start: code that is not what the stage should run may still interpolate)
  m.textures_ = call.textures;
  m.target_ = &target;
  m.vertex_const_.base = kVertexConstAddr;
  m.pixel_const_.base = kPixelConstAddr;
  m.table_.base = kTableAddr;
  m.vertices_.base = kVertexBufferAddr;
  if (!fill(m.vertex_const_.words, call.vertex_constants.size()) || !fill(m.pixel_const_.words, call.pixel_constants.size()) || !fill(m.table_.words, abi::kTableBytes / 4) ||
      !fill(m.vertices_.words, call.vertex_data.size()))
    return DrawStatus::OutOfMemory;
  for (uint64_t i = 0; i < call.vertex_constants.size(); ++i) {
    union { float f; uint32_t u; } c;
    c.f = call.vertex_constants[i];
    m.vertex_const_.words[i] = c.u;
  }
  for (uint64_t i = 0; i < call.pixel_constants.size(); ++i) {
    union { float f; uint32_t u; } c;
    c.f = call.pixel_constants[i];
    m.pixel_const_.words[i] = c.u;
  }
  for (uint64_t i = 0; i < call.vertex_data.size(); ++i) m.vertices_.words[i] = call.vertex_data[i];
  // the descriptor table: V# of the attributes, T# of the textures (the unit in the first dword)
  static constexpr uint32_t kDataFormat[4] = {4, 11, 13, 14};           // 32, 32_32, 32_32_32, 32_32_32_32
  for (uint32_t a = 0; a < abi::kMaxAttributes; ++a) {
    const VertexAttribute& at = call.attributes[a];
    if (at.components < 1 || at.components > 4) continue;
    const uint64_t base = kVertexBufferAddr + at.offset;
    const uint64_t w = a * abi::kVertexBufferBytes / 4;
    m.table_.words[w + 0] = static_cast<uint32_t>(base);
    m.table_.words[w + 1] = static_cast<uint32_t>(base >> 32) | (call.vertex_stride << 16);
    m.table_.words[w + 2] = static_cast<uint32_t>(vertex_count);
    m.table_.words[w + 3] = (7u << 12) | (kDataFormat[at.components - 1] << 15);
  }
  for (uint32_t s = 0; s < abi::kMaxSamplers; ++s) {
    m.table_.words[(abi::kImageOffset + s * abi::kImageBytes) / 4] = s;
    if (!(call.descriptors_set >> s & 1)) continue;
    for (uint32_t k = 0; k < 8; ++k) m.table_.words[(abi::kImageOffset + s * abi::kImageBytes) / 4 + k] = call.image_words[s][k];
    for (uint32_t k = 0; k < 4; ++k) m.table_.words[(abi::kSamplerOffset + s * abi::kSamplerBytes) / 4 + k] = call.sampler_words[s][k];
  }

  m.slots_ = static_cast<uint32_t>(call.indices.size());
  m.param_count_ = call.vertex_info.param_exports;
  if (m.slots_ == 0) return DrawStatus::Ok;
  if (!fill_f(m.positions_, static_cast<uint64_t>(m.slots_) * 4) || !fill_f(m.params_, static_cast<uint64_t>(m.slots_) * m.param_count_ * 4)) return DrawStatus::OutOfMemory;

  // the vertex shader
  Wave w;
  for (uint32_t base = 0; base < m.slots_; base += kWaveSize) {
    const uint32_t lanes = m.slots_ - base < kWaveSize ? m.slots_ - base : kWaveSize;
    w.reset(lanes);
    w.sgpr[0] = static_cast<uint32_t>(kVertexConstAddr);
    w.sgpr[1] = 0;
    w.sgpr[2] = static_cast<uint32_t>(kTableAddr);
    w.sgpr[3] = 0;
    for (uint32_t l = 0; l < lanes; ++l) w.vgpr[0][l] = call.indices[base + l];
    m.batch_base_ = base;
    if (w.run(call.vertex_code, m) != Status::Done) return DrawStatus::ShaderFault;
    ++st.vertex_waves;
  }

  // set up and rasterize the triangles
  m.pixel_stage_ = true;
  m.target_ = &target;
  m.textures_ = call.textures;
  const float fw = static_cast<float>(target.width()), fh = static_cast<float>(target.height());
  const Viewport& vp = call.viewport;
  const float xs = vp.set ? vp.x_scale : 0.5f * fw, xo = vp.set ? vp.x_offset : 0.5f * fw, ys = vp.set ? vp.y_scale : -0.5f * fh, yo = vp.set ? vp.y_offset : 0.5f * fh;
  // the pixels drawn: the target, and the scissor rectangle
  uint32_t clip_l = 0, clip_t = 0, clip_r = target.width(), clip_b = target.height();
  if (call.scissor.set) {
    if (call.scissor.left > clip_l) clip_l = call.scissor.left;
    if (call.scissor.top > clip_t) clip_t = call.scissor.top;
    if (call.scissor.right < clip_r) clip_r = call.scissor.right;
    if (call.scissor.bottom < clip_b) clip_b = call.scissor.bottom;
  }
  if (clip_l >= clip_r || clip_t >= clip_b) return DrawStatus::Ok;
  uint32_t px[kWaveSize], py[kWaveSize];
  float pi[kWaveSize], pj[kWaveSize], pz[kWaveSize];
  m.pixel_x_ = px;
  m.pixel_y_ = py;
  uint32_t pending = 0;

  auto flush = [&]() -> bool {
    if (pending == 0) return true;
    w.reset(pending);
    w.sgpr[0] = static_cast<uint32_t>(kPixelConstAddr);
    w.sgpr[1] = 0;
    w.sgpr[2] = static_cast<uint32_t>(kTableAddr);
    w.sgpr[3] = 0;
    w.sgpr[4] = 0;
    for (uint32_t l = 0; l < pending; ++l) {
      w.set_vgpr_f(0, l, pi[l]);
      w.set_vgpr_f(1, l, pj[l]);
    }
    m.exported_ = 0;
    const bool ok = w.run(call.pixel_code, m) == Status::Done && !m.fault_;
    // the pixels that wrote a colour pass: the stencil operation for passing and the depth
    for (uint32_t l = 0; l < pending; ++l) {
      if (!(m.exported_ >> l & 1)) continue;
      if (ds.stencil_test) call.depth->stencil(px[l], py[l]) = stencil_update(ds, ds.zpass_op, call.depth->stencil(px[l], py[l]));
      if (ds.depth_write) call.depth->depth(px[l], py[l]) = pz[l];
    }
    ++st.pixel_waves;
    st.pixels += pending;
    pending = 0;
    return ok;
  };

  for (uint32_t t = 0; t < m.slots_ / 3; ++t) {
    const uint32_t s0 = (t * 3), s1 = s0 + 1, s2 = s0 + 2;
    Screen v[3];
    bool ok = true;
    const uint32_t slot[3] = {s0, s1, s2};
    for (uint32_t k = 0; k < 3; ++k) {
      const float* p = &m.positions_[static_cast<uint64_t>(slot[k]) * 4];
      if (!(p[3] > 0.0f)) { ok = false; break; }
      const float inv = 1.0f / p[3];
      v[k].x = p[0] * inv * xs + xo;
      v[k].y = p[1] * inv * ys + yo;
      v[k].z = p[2] * inv * vp.z_scale + vp.z_offset;
      v[k].inv_w = inv;
      v[k].valid = true;
    }
    if (!ok) continue;
    uint32_t order[3] = {0, 1, 2};
    float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
    if (area == 0.0f || area != area) continue;
    if (area < 0.0f) { order[1] = 2; order[2] = 1; area = -area; }
    const Screen& a = v[order[0]];
    const Screen& b = v[order[1]];
    const Screen& c = v[order[2]];
    const float za = a.z, zb = b.z, zc = c.z;
    ++st.triangles;
    m.tri_[0] = slot[order[0]];
    m.tri_[1] = slot[order[1]];
    m.tri_[2] = slot[order[2]];
    // edge function of (p, q) at a point: cross(q - p, pt - p), positive inside for the positive area
    auto owns = [](float dx, float dy) { return dy < 0 || (dy == 0 && dx > 0); };
    const bool own_bc = owns(c.x - b.x, c.y - b.y), own_ca = owns(a.x - c.x, a.y - c.y), own_ab = owns(b.x - a.x, b.y - a.y);
    float min_x = a.x, max_x = a.x, min_y = a.y, max_y = a.y;
    const Screen* others[2] = {&b, &c};
    for (const Screen* s : others) {
      if (s->x < min_x) min_x = s->x;
      if (s->x > max_x) max_x = s->x;
      if (s->y < min_y) min_y = s->y;
      if (s->y > max_y) max_y = s->y;
    }
    if (max_x < static_cast<float>(clip_l) || max_y < static_cast<float>(clip_t) || min_x > static_cast<float>(clip_r) || min_y > static_cast<float>(clip_b)) continue;
    const uint32_t x0 = min_x < static_cast<float>(clip_l) ? clip_l : static_cast<uint32_t>(min_x), y0 = min_y < static_cast<float>(clip_t) ? clip_t : static_cast<uint32_t>(min_y);
    const uint32_t x1 = max_x >= static_cast<float>(clip_r) ? clip_r - 1 : static_cast<uint32_t>(max_x), y1 = max_y >= static_cast<float>(clip_b) ? clip_b - 1 : static_cast<uint32_t>(max_y);
    for (uint32_t y = y0; y <= y1; ++y) {
      for (uint32_t x = x0; x <= x1; ++x) {
        const float qx = static_cast<float>(x) + 0.5f, qy = static_cast<float>(y) + 0.5f;
        const float e_bc = (c.x - b.x) * (qy - b.y) - (c.y - b.y) * (qx - b.x);
        const float e_ca = (a.x - c.x) * (qy - c.y) - (a.y - c.y) * (qx - c.x);
        const float e_ab = (b.x - a.x) * (qy - a.y) - (b.y - a.y) * (qx - a.x);
        if (!(e_bc > 0 || (e_bc == 0 && own_bc)) || !(e_ca > 0 || (e_ca == 0 && own_ca)) || !(e_ab > 0 || (e_ab == 0 && own_ab))) continue;
        // the depth is linear in screen space; the viewport maps the clip range onto the depth range, and what is outside it is clipped
        const float z = (e_bc * za + e_ca * zb + e_ab * zc) / area;
        if (!(z >= 0.0f && z <= 1.0f)) continue;
        if (ds.stencil_test) {
          uint8_t& stored = call.depth->stencil(x, y);
          if (!compare(ds.stencil_func, static_cast<float>(ds.stencil_ref & ds.stencil_mask), static_cast<float>(stored & ds.stencil_mask))) {
            stored = stencil_update(ds, ds.fail_op, stored);
            continue;
          }
        }
        if (ds.depth_test && !compare(ds.depth_func, z, call.depth->depth(x, y))) {
          if (ds.stencil_test) call.depth->stencil(x, y) = stencil_update(ds, ds.zfail_op, call.depth->stencil(x, y));
          continue;
        }
        const float q0 = e_bc / area * a.inv_w, q1 = e_ca / area * b.inv_w, q2 = e_ab / area * c.inv_w;
        const float sum = q0 + q1 + q2;
        px[pending] = x;
        py[pending] = y;
        pi[pending] = q1 / sum;
        pj[pending] = q2 / sum;
        pz[pending] = z;
        if (++pending == kWaveSize && !flush()) return DrawStatus::ShaderFault;
      }
    }
    if (!flush()) return DrawStatus::ShaderFault;
  }
  return DrawStatus::Ok;
}

}  // namespace gcn
