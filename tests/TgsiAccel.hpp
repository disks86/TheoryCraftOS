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

// The reference "device" of the 3D hardware path for host tests (GlAccelTest, PiglitShaderTest --accel-machine): an IAccelerator that runs the TGSI the translator
// wrote with tests/TgsiMachine.hpp on the CPU rasterizer of src/WebGl. A picture it draws is what a device that runs the same TGSI would draw.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "WebGl/Accelerator.hpp"
#include "WebGl/GpuDevice.hpp"
#include "WebGl/Raster.hpp"
#include "Graphics/Graphics3D/Software/Tessellator.hpp"
#include "TgsiMachine.hpp"

namespace webgl {

// --- The translated shaders run as the CPU runs the GLSL --------------------------------------------------------------------------------------------------
// A reference "device": the TGSI the translator wrote is run by the test interpreter (tests/TgsiMachine.hpp), on the CPU rasterizer of src/WebGl, with the state
// of AccelState. A scene drawn by it and by the CPU path must give the same picture, which checks the translation (and GlAccel's packing of the inputs).
struct TgsiAccel : IAccelerator {
  struct Allocator final : allocators::IAllocator {      // (the tessellator's lists)
    void* allocate(uint64_t size) override { return malloc(size); }
    void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
    void deallocate(void* p) override { free(p); }
  };
  struct Prog { std::string vs, fs, gs, tcs, tes; uint32_t attributes = 0, varyings = 0, textures = 0; bool flat[16] = {}; int position = -1, face = -1; bool depth_out = false, derivatives = false; int depth_reg = 1; AccelStages st; };
  // A texture or a device image (create_image: renderable unless mipmapped; a Depth image keeps its depth in `f`, one float per texel). Mipmapped images keep level 0 only.
  struct Tex { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; std::vector<float> f; bool floats = false; bool image = false, depth = false; uint32_t levels = 1; };
  struct Tgt { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; std::vector<float> depth; };
  struct Surfaces;
  std::vector<Prog> progs;
  std::vector<Tex> texs;
  std::vector<Tgt> tgts;
  int draws = 0, refused = 0;
  const char* name() const override { return "tgsi reference"; }
  bool stages = true;   // (false: a device without geometry and tessellation stages)
  AccelLimits limits() const override {
    AccelLimits l{256, 16, 14, 16, true, true, stages, stages, true, true, 30, upload};
    l.image_colors = images;
    l.float_images = true;
    l.sampled_depth = true;
    l.mip_images = true;
    l.instancing = true;
    l.independent_blend = true;
    l.depth_bias = true;
    return l;
  }
  uint32_t images = 4;   // (0: a device without device images)
  int image_reads = 0, image_writes = 0, image_draws = 0, image_clears = 0;
  bool upload = true;   // (false: a device without write_color)
  bool write_color(uint32_t target, ConstByteView rgba) override {
    if (!upload) return false;
    Tgt& t = tgts.at(target - 1);
    if (rgba.size() != t.rgba.size()) return false;
    memcpy(t.rgba.data(), rgba.data(), rgba.size());
    return true;
  }
  uint32_t create_program(const char* vs, const char* fs, uint32_t attributes, uint32_t varyings, uint32_t textures) override {
    return create_program_stages(vs, fs, attributes, varyings, textures, AccelStages());
  }
  uint32_t create_program_stages(const char* vs, const char* fs, uint32_t attributes, uint32_t varyings, uint32_t textures, const AccelStages& stages) override {
    Prog p;
    p.vs = vs;
    p.fs = fs;
    if (stages.gs) p.gs = stages.gs;
    if (stages.tcs) p.tcs = stages.tcs;
    if (stages.tes) p.tes = stages.tes;
    p.st = stages;
    p.st.gs = p.st.tcs = p.st.tes = nullptr;
    if (getenv("TGSI_DUMP")) printf("--- vs\n%s--- tcs\n%s--- tes\n%s--- gs\n%s--- fs\n%s", vs, p.tcs.c_str(), p.tes.c_str(), p.gs.c_str(), fs);
    p.attributes = attributes;
    p.varyings = varyings;
    p.textures = textures;
    size_t at = 0;
    while ((at = p.fs.find("DCL IN[", at)) != std::string::npos) {
      const int i = atoi(p.fs.c_str() + at + 7);
      const size_t eol = p.fs.find('\n', at);
      const std::string line = p.fs.substr(at, eol - at);
      if (line.find("CONSTANT") != std::string::npos && line.find("GENERIC") != std::string::npos && i < 16) p.flat[i] = true;
      if (line.find("POSITION") != std::string::npos) p.position = i;
      if (line.find("FACE") != std::string::npos) p.face = i;
      at = eol;
    }
    p.depth_reg = declared(p.fs, "OUT", "POSITION");
    p.depth_out = p.depth_reg >= 0;
    p.derivatives = p.fs.find("DDX") != std::string::npos || p.fs.find("DDY") != std::string::npos;
    progs.push_back(p);
    return static_cast<uint32_t>(progs.size());
  }
  void destroy_program(uint32_t) override {}
  uint32_t create_image(uint32_t w, uint32_t h, AccelFormat f, uint32_t levels) override {
    if (!images || w == 0 || h == 0 || levels == 0) return 0;
    if (levels > 1 && f != AccelFormat::Rgba8 && f != AccelFormat::Rgba8Srgb) return 0;
    Tex t;
    t.w = w;
    t.h = h;
    t.image = true;
    t.levels = levels;
    t.depth = f == AccelFormat::Depth;
    t.floats = f == AccelFormat::Rgba16F || f == AccelFormat::Rgba32F;
    if (t.depth) t.f.assign(uint64_t{w} * h, 1.0f);
    else if (t.floats) t.f.assign(uint64_t{w} * h * 4, 0.0f);
    else t.rgba.assign(uint64_t{w} * h * 4, 0);
    texs.push_back(t);
    return static_cast<uint32_t>(texs.size());
  }
  bool write_image(uint32_t image, uint32_t level, ConstByteView px) override {
    Tex& t = texs.at(image - 1);
    if (!t.image || t.depth || level >= t.levels) return false;
    ++image_writes;
    const uint32_t w = t.w >> level ? t.w >> level : 1, h = t.h >> level ? t.h >> level : 1;
    if (px.size() != uint64_t{w} * h * (t.floats ? 16 : 4)) return false;
    if (level) return true;      // (only level 0 is kept: the tests sample at the texture's own size)
    if (t.floats) memcpy(t.f.data(), px.data(), px.size());
    else memcpy(t.rgba.data(), px.data(), px.size());
    return true;
  }
  bool read_image(uint32_t image, ByteView px) override {
    Tex& t = texs.at(image - 1);
    if (!t.image || t.depth) return false;
    ++image_reads;
    if (px.size() != uint64_t{t.w} * t.h * (t.floats ? 16 : 4)) return false;
    if (t.floats) memcpy(px.data(), t.f.data(), px.size());
    else memcpy(px.data(), t.rgba.data(), px.size());
    return true;
  }
  // The images of `a` as a render target: false if they are not all images of one size.
  bool surfaces(const AccelAttachments& a, Surfaces* out) {
    out->n = a.color_count;
    for (uint32_t i = 0; i < a.color_count; ++i) {
      if (!a.colors[i] || a.colors[i] > texs.size()) return false;
      Tex& t = texs[a.colors[i] - 1];
      if (!t.image || t.depth || t.levels != 1) return false;
      if (i == 0) { out->w = t.w; out->h = t.h; }
      if (t.w != out->w || t.h != out->h) return false;
      out->colors[i] = &t;
    }
    if (a.depth) {
      if (a.depth > texs.size() || !texs[a.depth - 1].depth) return false;
      Tex& d = texs[a.depth - 1];
      if (a.color_count == 0) { out->w = d.w; out->h = d.h; }
      if (d.w != out->w || d.h != out->h) return false;
      out->depth = d.f.data();
    }
    return out->w != 0;
  }
  bool clear_images(const AccelAttachments& a, bool color, const float rgba[4], bool depth, float d) override {
    Surfaces s;
    if (!surfaces(a, &s)) return false;
    ++image_clears;
    for (uint64_t i = 0; i < uint64_t{s.w} * s.h; ++i) {
      if (color)
        for (uint32_t c = 0; c < s.n; ++c)
          for (int k = 0; k < 4; ++k) {
            if (s.colors[c]->floats) s.colors[c]->f[i * 4 + k] = rgba[k];
            else s.colors[c]->rgba[i * 4 + k] = to8(rgba[k]);
          }
      if (depth && s.depth) s.depth[i] = d;
    }
    return true;
  }
  bool draw_images(const AccelAttachments& a, uint32_t program, const AccelState& st, ConstByteView vertices, uint32_t count, ConstByteView constants) override {
    Surfaces s;
    if (!surfaces(a, &s)) return false;
    for (uint32_t k = 0; k < st.texture_count; ++k)
      for (uint32_t i = 0; i < a.color_count; ++i)
        if (st.textures[k] == a.colors[i] || (a.depth && st.textures[k] == a.depth)) return false;
    ++image_draws;
    return render(s, program, st, vertices, count, constants);
  }
  uint32_t create_texture(uint32_t w, uint32_t h) override {
    texs.push_back(Tex{w, h, std::vector<uint8_t>(uint64_t{w} * h * 4)});
    return static_cast<uint32_t>(texs.size());
  }
  bool write_texture(uint32_t t, ConstByteView rgba) override {
    Tex& x = texs.at(t - 1);
    if (x.floats || rgba.size() != x.rgba.size()) return false;
    memcpy(x.rgba.data(), rgba.data(), rgba.size());
    return true;
  }
  uint32_t create_texture_float(uint32_t w, uint32_t h) override {
    Tex t{w, h, {}, std::vector<float>(uint64_t{w} * h * 4), true};
    texs.push_back(t);
    return static_cast<uint32_t>(texs.size());
  }
  bool write_texture_float(uint32_t t, ConstByteView v) override {
    Tex& x = texs.at(t - 1);
    if (!x.floats || v.size() != x.f.size() * 4) return false;
    memcpy(x.f.data(), v.data(), v.size());
    return true;
  }
  uint32_t create_target(uint32_t w, uint32_t h) override {
    tgts.push_back(Tgt{w, h, std::vector<uint8_t>(uint64_t{w} * h * 4), std::vector<float>(uint64_t{w} * h, 1.0f)});
    return static_cast<uint32_t>(tgts.size());
  }
  void destroy_target(uint32_t) override {}
  bool clear(uint32_t t, bool color, const float rgba[4], bool depth, float d) override {
    Tgt& g = tgts.at(t - 1);
    for (uint64_t i = 0; i < uint64_t{g.w} * g.h; ++i) {
      if (color)
        for (int k = 0; k < 4; ++k) g.rgba[i * 4 + k] = to8(rgba[k]);
      if (depth) g.depth[i] = d;
    }
    return true;
  }
  static uint8_t to8(float v) { return !(v > 0) ? 0 : (v >= 1 ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f)); }
  struct Sampling final : graphics::ITextureSet {
    const TgsiAccel* acc = nullptr;
    const AccelState* st = nullptr;
    graphics::Vec4 sample(uint32_t unit, float u, float v) const override {
      const Tex& t = acc->texs.at(st->textures[unit] - 1);
      const AccelSampler& sm = st->samplers[unit];
      auto texel = [&](int x, int y, int k) {
        const auto wrap = [](int i, int n, bool repeat) { return repeat ? ((i % n) + n) % n : (i < 0 ? 0 : (i >= n ? n - 1 : i)); };
        x = wrap(x, static_cast<int>(t.w), sm.repeat_s);
        y = wrap(y, static_cast<int>(t.h), sm.repeat_t);
        if (t.depth) return k == 0 ? t.f[uint64_t(y) * t.w + uint64_t(x)] : (k == 3 ? 1.0f : 0.0f);
        const uint64_t at = (uint64_t(y) * t.w + uint64_t(x)) * 4 + k;
        return t.floats ? t.f[at] : t.rgba[at] / 255.0f;
      };
      float r[4];
      const float fx = u * static_cast<float>(t.w), fy = v * static_cast<float>(t.h);
      for (int k = 0; k < 4; ++k) {
        if (!sm.linear) {
          r[k] = texel(static_cast<int>(floorf(fx)), static_cast<int>(floorf(fy)), k);
        } else {
          const float x = fx - 0.5f, y = fy - 0.5f, x0 = floorf(x), y0 = floorf(y), ax = x - x0, ay = y - y0;
          const int ix = static_cast<int>(x0), iy = static_cast<int>(y0);
          r[k] = (texel(ix, iy, k) * (1 - ax) + texel(ix + 1, iy, k) * ax) * (1 - ay) + (texel(ix, iy + 1, k) * (1 - ax) + texel(ix + 1, iy + 1, k) * ax) * ay;
        }
      }
      return {r[0], r[1], r[2], r[3]};
    }
  };
  // What a draw writes: colour images (8-bit or floats) and a depth.
  struct Surfaces {
    uint32_t w = 0, h = 0, n = 0;
    Tex* colors[8] = {};
    uint8_t* rgba8 = nullptr;   // (the default target's single colour image)
    float* depth = nullptr;
  };
  struct Pixels final : gpu::IFragmentSink {
    TgsiAccel* acc;
    Surfaces* t;
    const Prog* p;
    const AccelState* st;
    const float* constants;
    uint32_t const_rows;
    Sampling sampling;
    bool ok = true;
    void fragment(const gpu::Fragment& f) override {
      if (f.x < 0 || f.y < 0 || static_cast<uint32_t>(f.x) >= t->w || static_cast<uint32_t>(f.y) >= t->h) return;
      for (uint32_t i = 0; i < 8; ++i)      // (clip distances: interpolated after the varyings, a fragment where an enabled one is below zero is cut)
        if ((st->clip_mask >> i) & 1 && f.varyings[p->varyings * 4 + i] < 0) return;
      tgsi_test::TgsiMachine& m = machine();
      std::vector<float> vary(p->varyings * 4 + 8), log_x, log_y;
      // (the shader at pixel (px, py) with the fragment's varyings there; derivatives run it to the right and above first, as the CPU does)
      const auto run_at = [&](int32_t px, int32_t py, bool same, std::vector<float>* record) {
        m = tgsi_test::TgsiMachine();
        for (uint32_t r = 0; r < const_rows; ++r)
          for (int k = 0; k < 4; ++k) m.cst[r][k] = constants[r * 4 + k];
        if (same) for (uint64_t i = 0; i < vary.size(); ++i) vary[i] = f.varyings[i];
        else f.varyings_at(px, py, vary.data(), static_cast<uint32_t>(vary.size()));
        for (uint32_t i = 0; i < p->varyings; ++i)
          for (int k = 0; k < 4; ++k) m.in[i][k] = vary[i * 4 + k];
        if (p->position >= 0) {
          for (int k = 0; k < 4; ++k) m.in[p->position][k] = f.coord[k];
          m.in[p->position][0] = static_cast<float>(px) + 0.5f;
          m.in[p->position][1] = static_cast<float>(py) + 0.5f;
        }
        if (p->face >= 0) m.in[p->face][0] = m.in[p->face][1] = m.in[p->face][2] = m.in[p->face][3] = f.front ? 1.0f : -1.0f;
        m.textures = &sampling;
        m.derivative_record = record;
        if (!same) return m.run(p->fs);
        m.derivative_x = &log_x;
        m.derivative_y = &log_y;
        return m.run(p->fs);
      };
      if (p->derivatives && (!run_at(f.x + 1, f.y, false, &log_x) || !run_at(f.x, f.y + 1, false, &log_y))) { ok = false; return; }
      if (!run_at(f.x, f.y, true, nullptr)) { ok = false; return; }
      if (m.killed) return;
      float z = p->depth_out ? (m.out[p->depth_reg][2] < 0 ? 0 : (m.out[p->depth_reg][2] > 1 ? 1 : m.out[p->depth_reg][2])) : f.z;
      if (st->depth_bias_modes & 1) z += st->depth_bias_units / 16777216.0f;      // (filled triangles: the units only - the scenes' triangles face the window)
      const uint64_t at = uint64_t(f.y) * t->w + uint64_t(f.x);
      if (st->depth_test && t->depth) {
        const float d = t->depth[at];
        const uint8_t op = st->custom_blend ? st->depth_compare : 3;
        const bool pass = op == 0 ? false : op == 1 ? z < d : op == 2 ? z == d : op == 3 ? z <= d : op == 4 ? z > d : op == 5 ? z != d : op == 6 ? z >= d : true;
        if (!pass) return;
      }
      if (st->depth_write && t->depth) t->depth[at] = z;
      for (uint32_t c = 0; c < (t->rgba8 ? 1u : t->n); ++c) color(c, at);
    }
    void color(uint32_t c, uint64_t at) {
      static tgsi_test::TgsiMachine& m = machine();
      const bool floats = !t->rgba8 && t->colors[c]->floats;
      uint8_t* bytes = t->rgba8 ? t->rgba8 : (floats ? nullptr : t->colors[c]->rgba.data());
      float src[4], dst[4];
      for (int k = 0; k < 4; ++k) {
        src[k] = floats ? m.out[c][k] : (m.out[c][k] < 0 ? 0 : (m.out[c][k] > 1 ? 1 : m.out[c][k]));
        dst[k] = floats ? t->colors[c]->f[at * 4 + k] : bytes[at * 4 + k] / 255.0f;
      }
      float res[4] = {src[0], src[1], src[2], src[3]};
      // (per target: its own blending and mask, as AccelState::per_target says)
      AccelState::TargetBlend own;
      own.blend = st->blend;
      own.src_rgb = st->custom_blend ? st->src_rgb : 4;
      own.dst_rgb = st->custom_blend ? st->dst_rgb : 5;
      own.src_alpha = st->custom_blend ? st->src_alpha : 4;
      own.dst_alpha = st->custom_blend ? st->dst_alpha : 5;
      own.op_rgb = st->custom_blend ? st->op_rgb : 0;
      own.op_alpha = st->custom_blend ? st->op_alpha : 0;
      own.color_mask = st->custom_blend ? st->color_mask : 15;
      const AccelState::TargetBlend& tb = st->per_target && c < kAccelMaxColors ? st->targets[c] : own;
      if (tb.blend) {
        const auto factor = [&](uint8_t fct, int k) {
          switch (fct) {
            case 0: return 0.0f;
            case 1: return 1.0f;
            case 2: return src[k];
            case 3: return 1 - src[k];
            case 4: return src[3];
            case 5: return 1 - src[3];
            case 6: return dst[k];
            case 7: return 1 - dst[k];
            case 8: return dst[3];
            case 9: return 1 - dst[3];
            default: return k == 3 ? 1.0f : (src[3] < 1 - dst[3] ? src[3] : 1 - dst[3]);
          }
        };
        for (int k = 0; k < 4; ++k) {
          const bool alpha = k == 3;
          const uint8_t sf = alpha ? tb.src_alpha : tb.src_rgb, df = alpha ? tb.dst_alpha : tb.dst_rgb;
          const uint8_t op = alpha ? tb.op_alpha : tb.op_rgb;
          const float s = src[k] * factor(sf, k), d = dst[k] * factor(df, k);
          res[k] = op == 0 ? s + d : op == 1 ? s - d : op == 2 ? d - s : op == 3 ? (src[k] < dst[k] ? src[k] : dst[k]) : (src[k] > dst[k] ? src[k] : dst[k]);
        }
      }
      const uint8_t mask = tb.color_mask;
      for (int k = 0; k < 4; ++k) {
        if (!(mask & (1 << k))) continue;
        if (floats) t->colors[c]->f[at * 4 + k] = res[k];
        else bytes[at * 4 + k] = to8(res[k]);
      }
    }
    static tgsi_test::TgsiMachine& machine() {
      static tgsi_test::TgsiMachine m;
      return m;
    }
  };
  Sampling stage_textures;   // (the draw's textures for the stages before the fragment one: draw() sets it)
  // A vertex as a stage put it out: all its OUT registers (0 the position, 1 + n GENERIC[n] by the translator's numbering; the next stage's IN[v][r] is OUT[r]).
  struct Vtx { float r[tgsi_test::TgsiMachine::kRegs][4]; };
  using Machine = tgsi_test::TgsiMachine;
  // The register a declaration gives a semantic in a stage's text (DCL FILE[...][n], SEMANTIC or DCL FILE[n], SEMANTIC), or -1.
  static int declared(const std::string& text, const char* file, const char* semantic) {
    const std::string want = std::string("DCL ") + file + "[";
    const size_t sl = strlen(semantic);
    size_t at = 0;
    while ((at = text.find(want, at)) != std::string::npos) {
      const size_t eol = text.find('\n', at);
      const std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
      at = eol == std::string::npos ? text.size() : eol;
      const size_t comma = line.find(", ");
      if (comma == std::string::npos || line.compare(comma + 2, sl, semantic) != 0) continue;
      const char next = comma + 2 + sl < line.size() ? line[comma + 2 + sl] : 0;
      if (next && next != ',' && next != '[') continue;
      size_t b = want.size();
      if (line.compare(b, 2, "][") == 0) b += 2;
      return atoi(line.c_str() + b);
    }
    return -1;
  }
  static int property(const std::string& text, const char* name) {
    const size_t at = text.find(std::string("PROPERTY ") + name + " ");
    return at == std::string::npos ? -1 : atoi(text.c_str() + at + 10 + strlen(name));
  }
  static int named_property(const std::string& text, const char* name, const char* const* names, int count) {
    const size_t at = text.find(std::string("PROPERTY ") + name + " ");
    if (at == std::string::npos) return -1;
    const std::string rest = text.substr(at + 10 + strlen(name), text.find('\n', at) - at - 10 - strlen(name));
    for (int i = 0; i < count; ++i) if (rest == names[i]) return i;
    return -1;
  }
  bool stage_machine(Machine& m, const float* cf, uint32_t rows) {
    for (uint32_t r = 0; r < rows; ++r)
      for (int k = 0; k < 4; ++k) m.cst[r][k] = cf[r * 4 + k];
    m.textures = &stage_textures;
    return true;
  }
  static void set_sv(Machine& m, const std::string& text, const char* semantic, uint32_t value) {
    const int i = declared(text, "SV", semantic);
    if (i >= 0 && i < 8) m.sv[i][0] = m.sv[i][1] = m.sv[i][2] = m.sv[i][3] = __builtin_bit_cast(float, value);
  }
  bool fail_stage(const std::string& text) {
    if (getenv("TGSI_TRACE")) printf("%s\n", text.c_str());
    ++refused;
    return false;
  }
  // The geometry stage over one primitive: the primitives it makes (vertex lists of 1, 2 or 3).
  bool geometry(const Prog& p, const float* cf, uint32_t rows, const Vtx* const* in, uint32_t n, uint32_t primitive_id, std::vector<std::vector<Vtx>>* out) {
    static const char* const kOut[] = {"POINTS", "LINE_STRIP", "TRIANGLE_STRIP"};
    const int topology = named_property(p.gs, "GS_OUTPUT_PRIMITIVE", kOut, 3);
    const int invocations = property(p.gs, "GS_INVOCATIONS");
    const int max_vertices = property(p.gs, "GS_MAX_OUTPUT_VERTICES");
    for (int inv = 0; inv < (invocations > 0 ? invocations : 1); ++inv) {
      static Machine m;
      m = Machine();
      stage_machine(m, cf, rows);
      for (uint32_t v = 0; v < n; ++v) memcpy(m.in2[v], in[v]->r, sizeof in[v]->r);
      set_sv(m, p.gs, "INVOCATIONID", static_cast<uint32_t>(inv));
      const int pin = declared(p.gs, "IN", "PRIM_ID");      // (an input in the geometry stage)
      if (pin >= 0) m.in[pin][0] = __builtin_bit_cast(float, primitive_id);
      if (!m.run(p.gs)) return fail_stage(p.gs);
      std::vector<Vtx> strip;
      for (size_t e = 0; e < m.emitted.size() && static_cast<int>(e) < max_vertices; ++e) {
        if (m.emitted[e].cut) strip.clear();
        Vtx v;
        memcpy(v.r, m.emitted[e].out, sizeof v.r);
        strip.push_back(v);
        const size_t k = strip.size();
        if (topology == 0) out->push_back({v});
        else if (topology == 1 && k >= 2) out->push_back({strip[k - 2], v});
        else if (topology == 2 && k >= 3) {
          if ((k - 3) % 2 == 0) out->push_back({strip[k - 3], strip[k - 2], v});
          else out->push_back({strip[k - 2], strip[k - 3], v});
        }
      }
    }
    return true;
  }
  // The tessellation stages over one patch: the primitives the evaluation stage's vertices make.
  bool tessellation(allocators::IAllocator& alloc, const Prog& p, const float* cf, uint32_t rows, const Vtx* const* in, uint32_t n, uint32_t primitive_id, std::vector<std::vector<Vtx>>* out) {
    const int cps = property(p.tcs, "TCS_VERTICES_OUT");
    if (cps < 1 || cps > 32) return fail_stage(p.tcs);
    // the control stage: the invocations side by side, each to its next BARRIER in turn, with the outputs (OUT[v][r] and the patch registers) shared
    static Machine acc;
    acc = Machine();
    static std::vector<Machine> ms;
    ms.resize(static_cast<size_t>(cps));
    for (int inv = 0; inv < cps; ++inv) {
      Machine& m = ms[static_cast<size_t>(inv)];
      m = Machine();
      m.stop_at_barrier = true;
      stage_machine(m, cf, rows);
      for (uint32_t v = 0; v < n; ++v) memcpy(m.in2[v], in[v]->r, sizeof in[v]->r);
      set_sv(m, p.tcs, "INVOCATIONID", static_cast<uint32_t>(inv));
      set_sv(m, p.tcs, "PRIM_ID", primitive_id);
    }
    for (int round = 0; round < 64; ++round) {
      bool waiting = false;
      for (int inv = 0; inv < cps; ++inv) {
        Machine& m = ms[static_cast<size_t>(inv)];
        if (round > 0 && !m.at_barrier) continue;
        memcpy(m.out2, acc.out2, sizeof m.out2);
        memcpy(m.out, acc.out, sizeof m.out);
        if (!(round == 0 ? m.run(p.tcs) : m.resume())) return fail_stage(p.tcs);
        memcpy(acc.out2, m.out2, sizeof m.out2);
        memcpy(acc.out, m.out, sizeof m.out);
        waiting = waiting || m.at_barrier;
      }
      if (!waiting) break;
    }
    const int outer_reg = declared(p.tcs, "OUT", "TESSOUTER"), inner_reg = declared(p.tcs, "OUT", "TESSINNER");
    if (outer_reg < 0 || inner_reg < 0) return fail_stage(p.tcs);
    float outer[4], inner[2];
    for (int k = 0; k < 4; ++k) outer[k] = acc.out[outer_reg][k];
    for (int k = 0; k < 2; ++k) inner[k] = acc.out[inner_reg][k];
    const int mode = property(p.tes, "TES_PRIM_MODE"), spacing = property(p.tes, "TES_SPACING"), cw = property(p.tes, "TES_VERTEX_ORDER_CW"), points = property(p.tes, "TES_POINT_MODE");
    const graphics::TessDomain domain = mode == 1 ? graphics::TessDomain::Isoline : mode == 7 ? graphics::TessDomain::Quad : graphics::TessDomain::Triangle;
    const graphics::TessPartitioning part = spacing == 0 ? graphics::TessPartitioning::FractionalOdd : spacing == 1 ? graphics::TessPartitioning::FractionalEven : graphics::TessPartitioning::Integer;
    const graphics::TessOutput output = points == 1 ? graphics::TessOutput::Point : domain == graphics::TessDomain::Isoline ? graphics::TessOutput::Line : cw == 1 ? graphics::TessOutput::TriangleCw : graphics::TessOutput::TriangleCcw;
    graphics::Tessellator tess(alloc);
    if (!tess.tessellate(domain, part, output, outer, inner)) return true;      // (culled)
    std::vector<Vtx> made;
    for (const graphics::Tessellator::Point& pt : tess.points()) {
      static Machine m;
      m = Machine();
      stage_machine(m, cf, rows);
      for (int v = 0; v < cps; ++v) memcpy(m.in2[v], acc.out2[v], sizeof acc.out2[v]);
      memcpy(m.in, acc.out, sizeof m.in);      // (the patch registers: the same numbers in both stages)
      const float w = domain == graphics::TessDomain::Triangle ? 1.0f - pt.u - pt.v : 0.0f;
      const int c = declared(p.tes, "SV", "TESSCOORD");
      if (c >= 0) { m.sv[c][0] = pt.u; m.sv[c][1] = pt.v; m.sv[c][2] = w < 0 ? 0.0f : w; m.sv[c][3] = 0; }
      set_sv(m, p.tes, "PRIM_ID", primitive_id);
      if (!m.run(p.tes)) return fail_stage(p.tes);
      Vtx v;
      memcpy(v.r, m.out, sizeof v.r);
      made.push_back(v);
    }
    const uint32_t per = output == graphics::TessOutput::Point ? 1 : output == graphics::TessOutput::Line ? 2 : 3;
    const ArrayView<const uint16_t> ix = tess.indices();
    for (uint64_t i = 0; i + per <= ix.size(); i += per) {
      std::vector<Vtx> prim;
      for (uint32_t k = 0; k < per; ++k) prim.push_back(made.at(ix[i + k]));
      out->push_back(prim);
    }
    return true;
  }
  bool draw(uint32_t target, uint32_t program, const AccelState& st, ConstByteView vertices, uint32_t count, ConstByteView constants) override {
    Tgt& tg = tgts.at(target - 1);
    Surfaces s;
    s.w = tg.w;
    s.h = tg.h;
    s.n = 1;
    s.rgba8 = tg.rgba.data();
    s.depth = tg.depth.data();
    return render(s, program, st, vertices, count, constants);
  }
  bool render(Surfaces& t, uint32_t program, const AccelState& st, ConstByteView vertices, uint32_t count, ConstByteView constants) {
    ++draws;
    const Prog& p = progs.at(program - 1);
    const float* vf = reinterpret_cast<const float*>(vertices.data());
    const float* cf = reinterpret_cast<const float*>(constants.data());
    const uint32_t rows = static_cast<uint32_t>(constants.size() / 16);
    // (instancing: the vertices again for each instance, the per-instance attributes from after the vertices, INSTANCEID the instance)
    const uint32_t instances = st.instances ? st.instances : 1;
    uint32_t per_vertex = 0, per_instance = 0;
    for (uint32_t a = 0; a < p.attributes; ++a) ((st.instanced >> a) & 1 ? per_instance : per_vertex)++;
    const uint32_t vertices_once = count;
    count *= instances;
    std::vector<gpu::RVertex> vs(count);
    std::vector<Vtx> vouts(count);
    stage_textures.acc = this;
    stage_textures.st = &st;
    for (uint32_t v = 0; v < count; ++v) {
      static Machine m;
      m = Machine();
      stage_machine(m, cf, rows);
      const uint32_t vertex = v % vertices_once, instance = v / vertices_once;
      uint32_t pv = 0, pi = 0;
      for (uint32_t a = 0; a < p.attributes; ++a) {
        const bool inst = (st.instanced >> a) & 1;
        const uint64_t at = inst ? (uint64_t{vertices_once} * per_vertex + uint64_t{instance} * per_instance + pi++) * 4 : (uint64_t{vertex} * per_vertex + pv++) * 4;
        for (int k = 0; k < 4; ++k) m.in[a][k] = vf[at + k];
      }
      set_sv(m, p.vs, "INSTANCEID", instance);
      if (!m.run(p.vs)) { if (getenv("TGSI_TRACE")) printf("%s\n", p.vs.c_str()); ++refused; return false; }
      memcpy(vouts[v].r, m.out, sizeof vouts[v].r);
      for (int k = 0; k < 4; ++k) vs[v].pos[k] = m.out[0][k];
      for (uint32_t i = 0; i < p.varyings + 2; ++i)      // (and the clip distances after them)
        for (int k = 0; k < 4; ++k) vs[v].var[i * 4 + k] = m.out[1 + i][k];
    }
    // the geometry and tessellation stages: the primitives the rasterizer gets
    std::vector<std::vector<Vtx>> prims;
    const bool staged = !p.gs.empty() || !p.tes.empty();
    if (staged) {
      static const uint32_t kPer[6] = {3, 2, 1, 0, 4, 6};
      const uint32_t per = st.topology == 3 ? st.patch_vertices : kPer[st.topology];
      if (per == 0 || per > 32) { ++refused; return false; }
      Allocator alloc;
      uint32_t id = 0;
      uint32_t gs_id = 0;      // (after tessellation the geometry stage counts the primitives of the whole draw, not of each patch)
      for (uint32_t first = 0; first + per <= count; first += per, ++id) {
        const Vtx* in[32];
        for (uint32_t k = 0; k < per; ++k) in[k] = &vouts[first + k];
        std::vector<std::vector<Vtx>> mid;
        if (!p.tes.empty()) {
          if (!tessellation(alloc, p, cf, rows, in, per, id, &mid)) return false;
        } else {
          std::vector<Vtx> prim;
          for (uint32_t k = 0; k < per; ++k) prim.push_back(*in[k]);
          mid.push_back(prim);
        }
        if (p.gs.empty()) {
          for (auto& m : mid) prims.push_back(m);
          continue;
        }
        for (size_t q = 0; q < mid.size(); ++q) {
          const Vtx* gin[6];
          for (size_t k = 0; k < mid[q].size() && k < 6; ++k) gin[k] = &mid[q][k];
          if (!geometry(p, cf, rows, gin, static_cast<uint32_t>(mid[q].size()), p.tes.empty() ? id : gs_id++, &prims)) return false;
        }
      }
    }
    gpu::RasterState rs;
    rs.vp_x = static_cast<int32_t>(st.vp_x);
    rs.vp_y = static_cast<int32_t>(st.vp_y);
    rs.vp_w = static_cast<int32_t>(st.vp_w);
    rs.vp_h = static_cast<int32_t>(st.vp_h);
    rs.depth_near = st.depth_near;
    rs.depth_far = st.depth_far;
    rs.target_w = static_cast<int32_t>(t.w);
    rs.target_h = static_cast<int32_t>(t.h);
    if (st.scissor) { rs.sc_x = st.sc_x; rs.sc_y = st.sc_y; rs.sc_w = static_cast<int32_t>(st.sc_w); rs.sc_h = static_cast<int32_t>(st.sc_h); }
    else { rs.sc_x = rs.sc_y = 0; rs.sc_w = static_cast<int32_t>(t.w); rs.sc_h = static_cast<int32_t>(t.h); }
    rs.cull = st.cull;
    rs.front_ccw = st.front_ccw;
    rs.nvar = (p.varyings + 2) * 4;
    rs.clip_z_zero_to_one = true;
    for (uint32_t i = 0; i < p.varyings && i < 16; ++i)
      for (int k = 0; k < 4; ++k) rs.flat[i * 4 + k] = p.flat[i];
    Pixels px;
    px.acc = this;
    px.t = &t;
    px.p = &p;
    px.st = &st;
    px.constants = cf;
    px.const_rows = rows;
    px.sampling.acc = this;
    px.sampling.st = &st;
    if (staged) {
      for (const std::vector<Vtx>& prim : prims) {
        gpu::RVertex rv[3];
        for (size_t k = 0; k < prim.size() && k < 3; ++k) {
          for (int c = 0; c < 4; ++c) rv[k].pos[c] = prim[k].r[0][c];
          for (uint32_t i = 0; i < p.varyings + 2; ++i)
            for (int c = 0; c < 4; ++c) rv[k].var[i * 4 + c] = prim[k].r[1 + i][c];
        }
        if (prim.size() == 3) gpu::draw_triangle(rs, rv[0], rv[1], rv[2], px);
        else if (prim.size() == 2) gpu::draw_line(rs, rv[0], rv[1], px);
        else if (prim.size() == 1) gpu::draw_point(rs, rv[0], px);
      }
    } else if (st.topology == 0) for (uint32_t v = 0; v + 2 < count; v += 3) gpu::draw_triangle(rs, vs[v], vs[v + 1], vs[v + 2], px);
    else if (st.topology == 1) for (uint32_t v = 0; v + 1 < count; v += 2) gpu::draw_line(rs, vs[v], vs[v + 1], px);
    else for (uint32_t v = 0; v < count; ++v) gpu::draw_point(rs, vs[v], px);
    if (!px.ok) { if (getenv("TGSI_TRACE")) printf("%s\n", p.fs.c_str()); ++refused; }
    return px.ok;
  }
  bool read_color(uint32_t target, ByteView rgba) override {
    Tgt& t = tgts.at(target - 1);
    if (rgba.size() != t.rgba.size()) return false;
    memcpy(rgba.data(), t.rgba.data(), t.rgba.size());
    return true;
  }
};

}  // namespace webgl
