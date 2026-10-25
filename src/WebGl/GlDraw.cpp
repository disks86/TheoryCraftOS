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

// Drawing for the GL context: vertex fetch and shading, primitive assembly, the per-fragment pipeline (shader, stencil, depth, blending), clears, reads, blits
// and presenting the drawing buffer.
#include "WebGl/GlContext.hpp"
#include "WebGl/GlPixels.hpp"

namespace webgl {

namespace {
constexpr uint32_t INVALID_ENUM = 0x500, INVALID_VALUE = 0x501, INVALID_OPERATION = 0x502, INVALID_FRAMEBUFFER_OPERATION = 0x506;

inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline uint8_t to_u8(float v) { return !(v > 0) ? 0 : (v >= 1 ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f)); }

bool compare_fn(uint32_t func, float a, float b) {   // a func b
  switch (func) {
    case 0x200: return false;
    case 0x201: return a < b;
    case 0x202: return a == b;
    case 0x203: return a <= b;
    case 0x204: return a > b;
    case 0x205: return a != b;
    case 0x206: return a >= b;
    default: return true;
  }
}

bool compare_u(uint32_t func, uint32_t a, uint32_t b) {
  switch (func) {
    case 0x200: return false;
    case 0x201: return a < b;
    case 0x202: return a == b;
    case 0x203: return a <= b;
    case 0x204: return a > b;
    case 0x205: return a != b;
    case 0x206: return a >= b;
    default: return true;
  }
}

uint32_t stencil_apply(uint32_t op, uint32_t cur, uint32_t ref) {
  switch (op) {
    case 0x1E00: return cur;
    case 0x0: return 0;
    case 0x1E01: return ref;
    case 0x1E02: return cur < 255 ? cur + 1 : 255;
    case 0x1E03: return cur > 0 ? cur - 1 : 0;
    case 0x150A: return ~cur & 255;
    case 0x8507: return (cur + 1) & 255;
    case 0x8508: return (cur - 1) & 255;
    default: return cur;
  }
}

float blend_factor(uint32_t f, const float src[4], const float dst[4], const float cc[4], int comp, bool alpha_channel) {
  switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 0x300: return src[alpha_channel ? 3 : comp];
    case 0x301: return 1 - src[alpha_channel ? 3 : comp];
    case 0x302: return src[3];
    case 0x303: return 1 - src[3];
    case 0x304: return dst[3];
    case 0x305: return 1 - dst[3];
    case 0x306: return dst[alpha_channel ? 3 : comp];
    case 0x307: return 1 - dst[alpha_channel ? 3 : comp];
    case 0x308: return alpha_channel ? 1.0f : (src[3] < 1 - dst[3] ? src[3] : 1 - dst[3]);
    case 0x8001: return cc[alpha_channel ? 3 : comp];
    case 0x8002: return 1 - cc[alpha_channel ? 3 : comp];
    case 0x8003: return cc[3];
    case 0x8004: return 1 - cc[3];
    default: return 1;
  }
}

float blend_op(uint32_t eq, float s, float d) {
  switch (eq) {
    case 0x800A: return s - d;
    case 0x800B: return d - s;
    case 0x8007: return s < d ? s : d;
    case 0x8008: return s > d ? s : d;
    default: return s + d;
  }
}

uint32_t read_index(const uint8_t* p, uint32_t type) {
  switch (type) {
    case 0x1401: return p[0];
    case 0x1403: { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
    default: { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
  }
}

uint32_t attrib_type_size(uint32_t t) {
  switch (t) {
    case 0x1400: case 0x1401: return 1;
    case 0x1402: case 0x1403: case 0x140B: return 2;
    default: return 4;
  }
}

}  // namespace

// --- Fragment pipeline ------------------------------------------------------------------------------------------------------------------------------------

struct FragmentPipeline final : gpu::IFragmentSink {
  GlContext& c;
  ProgramObj& p;
  GlContext::Target& t;
  shader::Machine& fm;
  const shader::Module& mod;
  bool late;                 // the fragment shader decides coverage or depth
  bool deriv;
  int32_t fc_slot = -1, ff_slot = -1, pc_slot = -1, fd_slot = -1;
  shader::DerivativeLog logx, logy;
  QueryObj* occlusion = nullptr;
  uint64_t samples = 0;
  float point_size_unused = 0;

  FragmentPipeline(GlContext& ctx, ProgramObj& prog, GlContext::Target& target, shader::Machine& m, bool derivs)
      : c(ctx), p(prog), t(target), fm(m), mod(m.module()), late(false), deriv(derivs), logx(ctx.a_), logy(ctx.a_) {
    late = mod.uses_discard || mod.writes_frag_depth;
    const int32_t a = mod.find_builtin_global(shader::kBvFragCoord), b = mod.find_builtin_global(shader::kBvFrontFacing), d = mod.find_builtin_global(shader::kBvPointCoord),
                  e = mod.find_builtin_global(shader::kBvFragDepth);
    fc_slot = a >= 0 ? static_cast<int32_t>(mod.globals[static_cast<uint64_t>(a)]->slot) : -1;
    ff_slot = b >= 0 ? static_cast<int32_t>(mod.globals[static_cast<uint64_t>(b)]->slot) : -1;
    pc_slot = d >= 0 ? static_cast<int32_t>(mod.globals[static_cast<uint64_t>(d)]->slot) : -1;
    fd_slot = e >= 0 ? static_cast<int32_t>(mod.globals[static_cast<uint64_t>(e)]->slot) : -1;
  }

  void zero_privates() {
    uint32_t* pr = fm.private_region();
    for (uint32_t i = 0; i < mod.private_slots; ++i) pr[i] = 0;
  }

  // Runs the shader for pixel (px, py) with the varyings of the fragment as they are there. Returns false if discarded or failed.
  shader::Machine::Status run_at(const gpu::Fragment& f, int32_t px, int32_t py, bool same) {
    uint32_t* pr = fm.private_region();
    zero_privates();
    float tmp[gpu::kMaxVarying];
    if (same) {
      for (uint32_t i = 0; i < p.nvar; ++i) tmp[i] = f.varyings[i];
    } else {
      f.varyings_at(px, py, tmp, p.nvar);
    }
    for (const VaryingLink& l : p.links)
      for (uint32_t k = 0; k < l.count; ++k) pr[l.fs_slot + k] = f2u(tmp[l.var_offset + k]);
    if (fc_slot >= 0) {
      pr[fc_slot] = f2u((static_cast<float>(px) + 0.5f) / static_cast<float>(t.ss));
      pr[fc_slot + 1] = f2u((static_cast<float>(py) + 0.5f) / static_cast<float>(t.ss));
      pr[fc_slot + 2] = f2u(f.z);
      pr[fc_slot + 3] = f2u(f.coord[3]);
    }
    if (ff_slot >= 0) pr[ff_slot] = f.front ? 1 : 0;
    if (pc_slot >= 0) { pr[pc_slot] = f2u(f.point_coord[0]); pr[pc_slot + 1] = f2u(f.point_coord[1]); }
    return fm.start(mod.main_function, ArrayView<const uint32_t* const>());
  }

  bool shade(const gpu::Fragment& f) {
    using S = shader::Machine::Status;
    if (deriv) {
      logx.clear();
      logy.clear();
      fm.set_derivative_record(&logx);
      if (run_at(f, f.x + 1, f.y, false) == S::Failed) { fm.set_derivative_record(nullptr); return false; }
      fm.set_derivative_record(&logy);
      if (run_at(f, f.x, f.y + 1, false) == S::Failed) { fm.set_derivative_record(nullptr); return false; }
      fm.set_derivative_replay(&logx, &logy);
      const S s = run_at(f, f.x, f.y, true);
      fm.set_derivative_record(nullptr);
      return s == S::Done;
    }
    return run_at(f, f.x, f.y, true) == S::Done;
  }

  bool stencil_pass(Image* ds, uint32_t x, uint32_t y, uint32_t layer, bool front, bool depth_pass, bool depth_known, bool* sfail_out) {
    (void)depth_known;
    const int idx = front ? 0 : 1;
    uint32_t raw[4];
    read_texel(*ds, x, y, layer, raw);
    const uint32_t cur = raw[1] & 255;
    const uint32_t ref = static_cast<uint32_t>(c.sref_[idx]) & 255;
    const uint32_t mask = c.smask_[idx] & 255;
    const bool pass = compare_u(c.sfunc_[idx], ref & mask, cur & mask);
    uint32_t op = pass ? (depth_pass ? c.sdppass_[idx] : c.sdpfail_[idx]) : c.sfail_[idx];
    const uint32_t nv = stencil_apply(op, cur, ref);
    const uint32_t wm = c.swmask_[idx] & 255;
    raw[1] = (cur & ~wm) | (nv & wm);
    write_texel(*ds, x, y, layer, raw);
    *sfail_out = !pass;
    return pass;
  }

  void fragment(const gpu::Fragment& f) override {
    const uint32_t x = static_cast<uint32_t>(f.x), y = static_cast<uint32_t>(f.y);
    if (x >= t.w || y >= t.h) return;
    const bool has_depth = t.depth && c.depth_test_ && (t.depth->info->store == Store::Depth || t.depth->info->store == Store::DepthStencil);
    const bool has_stencil = t.stencil && c.stencil_test_ && t.stencil->info->store == Store::DepthStencil;
    float z = f.z;
    // early tests
    if (!late) {
      if (has_stencil) {
        // the stencil test needs the depth result: do both now
      }
      if (!ds_test(f, x, y, z, has_depth, has_stencil, /*write=*/true, nullptr)) return;
    }
    if (!shade(f)) {
      // discarded (or failed): the stencil/depth state was not touched in the late case
      if (late && has_stencil) {   // a discarded fragment performs no stencil operations
      }
      return;
    }
    if (late) {
      if (fd_slot >= 0 && mod.writes_frag_depth) z = clamp01(u2f(fm.private_region()[fd_slot]));
      if (!ds_test(f, x, y, z, has_depth, has_stencil, true, nullptr)) return;
    }
    if (c.rasterizer_discard_) return;
    ++samples;
    write_colors(f, x, y);
    if (occlusion) occlusion->result = 1;
  }

  // Stencil and depth tests; performs the buffer updates. False if the fragment is rejected.
  bool ds_test(const gpu::Fragment& f, uint32_t x, uint32_t y, float z, bool has_depth, bool has_stencil, bool write, bool*) {
    (void)write;
    const uint32_t layer = t.depth_layer;
    bool dpass = true;
    if (has_depth) {
      uint32_t raw[4];
      read_texel(*t.depth, x, y, layer, raw);
      dpass = compare_fn(c.dfunc_, z, u2f(raw[0]));
    }
    if (has_stencil) {
      bool sfail = false;
      const bool spass = stencil_pass(t.stencil, x, y, layer, f.front, dpass, true, &sfail);
      if (!spass) return false;
    }
    if (!dpass) return false;
    if (has_depth && c.dmask_) {
      uint32_t raw[4];
      read_texel(*t.depth, x, y, layer, raw);
      raw[0] = f2u(z);
      write_texel(*t.depth, x, y, layer, raw);
    }
    return true;
  }

  void write_colors(const gpu::Fragment& f, uint32_t x, uint32_t y) {
    const uint32_t* pr = fm.private_region();
    for (const ProgramObj::Output& o : p.outputs) {
      for (uint32_t bi = 0; bi < kMaxDrawBuffers; ++bi) {
        if (!t.color[bi]) continue;
        const bool target_for_output = p.frag_writes_all ? true : (static_cast<uint32_t>(o.location) == bi);
        if (!target_for_output) continue;
        Image& img = *t.color[bi];
        const uint32_t layer = t.layer[bi];
        const FormatInfo* fi = img.info;
        // the shader's vec4 for this output
        uint32_t lanes[4] = {0, 0, 0, f2u(1.0f)};
        const uint32_t comps = o.type.rows;
        for (uint32_t k = 0; k < 4; ++k) lanes[k] = k < comps ? pr[o.slot + k] : (k == 3 ? (o.type.base == shader::Base::Float ? f2u(1.0f) : 1u) : 0u);
        if (fi->store == Store::I32x4 || fi->store == Store::U32x4) {
          uint32_t cur[4];
          read_texel(img, x, y, layer, cur);
          for (int k = 0; k < 4; ++k) if (c.cmask_[k]) cur[k] = o.type.base == shader::Base::Float ? static_cast<uint32_t>(static_cast<int32_t>(u2f(lanes[k]))) : lanes[k];
          write_texel(img, x, y, layer, cur);
        } else {
          float src[4];
          for (int k = 0; k < 4; ++k) src[k] = o.type.base == shader::Base::Float ? u2f(lanes[k]) : static_cast<float>(static_cast<int32_t>(lanes[k]));
          uint32_t raw[4];
          read_texel(img, x, y, layer, raw);
          float dst[4];
          if (fi->store == Store::U8x4) {
            for (int k = 0; k < 4; ++k) {
              dst[k] = raw[k] / 255.0f;
              if (fi->srgb && k < 3) dst[k] = srgb_to_linear(dst[k]);
            }
          } else {
            for (int k = 0; k < 4; ++k) dst[k] = u2f(raw[k]);
          }
          float res[4] = {src[0], src[1], src[2], src[3]};
          if (c.blend_) {
            if (fi->store == Store::U8x4) for (int k = 0; k < 4; ++k) src[k] = clamp01(src[k]);
            float out[4];
            for (int k = 0; k < 3; ++k) {
              const float sf = blend_factor(c.blend_sr_, src, dst, c.blend_color_, k, false), df = blend_factor(c.blend_dr_, src, dst, c.blend_color_, k, false);
              out[k] = c.blend_eq_rgb_ == 0x8007 ? (src[k] < dst[k] ? src[k] : dst[k]) : c.blend_eq_rgb_ == 0x8008 ? (src[k] > dst[k] ? src[k] : dst[k]) : blend_op(c.blend_eq_rgb_, src[k] * sf, dst[k] * df);
            }
            const float sfa = blend_factor(c.blend_sa_, src, dst, c.blend_color_, 3, true), dfa = blend_factor(c.blend_da_, src, dst, c.blend_color_, 3, true);
            out[3] = c.blend_eq_a_ == 0x8007 ? (src[3] < dst[3] ? src[3] : dst[3]) : c.blend_eq_a_ == 0x8008 ? (src[3] > dst[3] ? src[3] : dst[3]) : blend_op(c.blend_eq_a_, src[3] * sfa, dst[3] * dfa);
            for (int k = 0; k < 4; ++k) res[k] = out[k];
          }
          if (fi->store == Store::U8x4) {
            uint8_t q[4];
            for (int k = 0; k < 4; ++k) {
              float v = res[k];
              if (fi->srgb && k < 3) v = linear_to_srgb(v);
              q[k] = to_u8(v);
              if (!c.cmask_[k]) q[k] = static_cast<uint8_t>(raw[k]);
            }
            // formats without alpha keep alpha at 1
            if (fi->base == 0x1907 || fi->base == 0x8227 || fi->base == 0x1903) q[3] = 255;
            if (!c.attrs_.alpha && t.is_default) q[3] = 255;
            uint32_t w4[4] = {q[0], q[1], q[2], q[3]};
            write_texel(img, x, y, layer, w4);
          } else {
            uint32_t w4[4];
            for (int k = 0; k < 4; ++k) w4[k] = c.cmask_[k] ? f2u(res[k]) : raw[k];
            write_texel(img, x, y, layer, w4);
          }
        }
        if (t.is_default && bi == 0) c.mark_dirty(static_cast<int32_t>(x), static_cast<int32_t>(y));
      }
    }
    (void)f;
  }
};

void GlContext::mark_dirty(int32_t x, int32_t y) { dirty_.add(x, y); }

// --- Vertex processing ---------------------------------------------------------------------------------------------------------------------------------

namespace {
// Reads component k of an attribute element as a float lane (normalised or converted) or raw integer.
void read_attrib(const VertexAttrib& at, const uint8_t* base, uint32_t comps, bool want_int, uint32_t out[4]) {
  const uint32_t ts = attrib_type_size(at.type);
  for (uint32_t i = 0; i < 4; ++i) out[i] = want_int ? (i == 3 ? 1u : 0u) : (i == 3 ? f2u(1.0f) : 0u);
  if (at.type == 0x8368 || at.type == 0x8D9F) {   // 2_10_10_10 packed
    uint32_t v;
    __builtin_memcpy(&v, base, 4);
    int32_t c[4];
    if (at.type == 0x8368) { c[0] = v & 1023; c[1] = (v >> 10) & 1023; c[2] = (v >> 20) & 1023; c[3] = v >> 30; }
    else {
      auto sx = [](uint32_t b, int bits) { return static_cast<int32_t>(b << (32 - bits)) >> (32 - bits); };
      c[0] = sx(v & 1023, 10); c[1] = sx((v >> 10) & 1023, 10); c[2] = sx((v >> 20) & 1023, 10); c[3] = sx(v >> 30, 2);
    }
    for (int i = 0; i < 4; ++i) {
      if (want_int) out[i] = static_cast<uint32_t>(c[i]);
      else if (at.normalized) out[i] = f2u(at.type == 0x8368 ? c[i] / (i == 3 ? 3.0f : 1023.0f) : (c[i] < 0 ? -1.0f : 1.0f) * 0 + c[i] / (i == 3 ? 1.0f : 511.0f));
      else out[i] = f2u(static_cast<float>(c[i]));
    }
    return;
  }
  for (uint32_t i = 0; i < comps && i < 4; ++i) {
    const uint8_t* p = base + i * ts;
    float f = 0;
    int64_t iv = 0;
    switch (at.type) {
      case 0x1400: iv = static_cast<int8_t>(p[0]); f = at.normalized ? (iv < -127 ? -1.0f : iv / 127.0f) : static_cast<float>(iv); break;
      case 0x1401: iv = p[0]; f = at.normalized ? iv / 255.0f : static_cast<float>(iv); break;
      case 0x1402: { int16_t v; __builtin_memcpy(&v, p, 2); iv = v; f = at.normalized ? (v < -32767 ? -1.0f : v / 32767.0f) : static_cast<float>(v); break; }
      case 0x1403: { uint16_t v; __builtin_memcpy(&v, p, 2); iv = v; f = at.normalized ? v / 65535.0f : static_cast<float>(v); break; }
      case 0x1404: { int32_t v; __builtin_memcpy(&v, p, 4); iv = v; f = at.normalized ? static_cast<float>(v / 2147483647.0) : static_cast<float>(v); break; }
      case 0x1405: { uint32_t v; __builtin_memcpy(&v, p, 4); iv = v; f = at.normalized ? static_cast<float>(v / 4294967295.0) : static_cast<float>(v); break; }
      case 0x140B: { uint16_t v; __builtin_memcpy(&v, p, 2); f = shader::half_to_float(v); iv = static_cast<int64_t>(f); break; }
      default: __builtin_memcpy(&f, p, 4); iv = static_cast<int64_t>(f); break;
    }
    out[i] = want_int ? static_cast<uint32_t>(iv) : f2u(f);
  }
}
}  // namespace

uint32_t GlContext::fetch_index(const uint8_t* idx, uint32_t type, uint32_t i) const {
  const uint32_t sz = type == 0x1401 ? 1 : (type == 0x1403 ? 2 : 4);
  return read_index(idx + uint64_t{i} * sz, type);
}

bool GlContext::run_draw(uint32_t mode, int32_t first, int32_t count, uint32_t index_type, uint64_t index_offset, int32_t instances, bool indexed) {
  using shader::Machine;
  if (lost_) return false;
  if (mode > 6) { set_error(INVALID_ENUM); return false; }
  if (count < 0 || instances < 0 || first < 0) { set_error(INVALID_VALUE); return false; }
  ProgramObj* p = program_at(program_);
  if (!p || !p->linked) { set_error(INVALID_OPERATION); return false; }
  if (count == 0 || instances == 0) return true;
  uint32_t status;
  if (!framebuffer_complete(draw_fb_, &status)) { set_error(INVALID_FRAMEBUFFER_OPERATION); return false; }
  Target target;
  resolve_target(draw_fb_, true, &target);
  if (!target.ok) { set_error(INVALID_FRAMEBUFFER_OPERATION); return false; }
  if (draw_fb_ != 0) {
    bool any = false;
    for (auto* im : target.color) any = any || im;
    (void)any;
  }
  Vao* vao = current_vao_ptr();

  // indices
  const uint8_t* idx = nullptr;
  uint32_t max_index = 0;
  if (indexed) {
    if (index_type != 0x1401 && index_type != 0x1403 && !(index_type == 0x1405)) { set_error(INVALID_ENUM); return false; }
    const Buffer* eb = buffer_at(vao->element_buffer);
    if (!eb) { set_error(INVALID_OPERATION); return false; }
    const uint32_t sz = index_type == 0x1401 ? 1 : (index_type == 0x1403 ? 2 : 4);
    if (index_offset % sz) { set_error(INVALID_OPERATION); return false; }
    if (index_offset > eb->data.size() || uint64_t{static_cast<uint32_t>(count)} * sz > eb->data.size() - index_offset) { set_error(INVALID_OPERATION); return false; }
    idx = eb->data.view().data() + index_offset;
    const uint32_t restart = index_type == 0x1401 ? 0xFF : (index_type == 0x1403 ? 0xFFFF : 0xFFFFFFFFu);
    for (int32_t i = 0; i < count; ++i) {
      const uint32_t v = fetch_index(idx, index_type, static_cast<uint32_t>(i));
      if (v != restart && v > max_index) max_index = v;
    }
  } else {
    max_index = static_cast<uint32_t>(first + count - 1);
  }

  // machines
  if (!p->vmach) p->vmach = UniquePointer<Machine>::make(a_, *p->vsm, a_);
  if (!p->fmach) p->fmach = UniquePointer<Machine>::make(a_, *p->fsm, a_);
  if (!p->vmach || !p->vmach->valid() || !p->fmach || !p->fmach->valid()) { set_error(0x505); return false; }
  Machine& vm = *p->vmach;
  Machine& fm = *p->fmach;
  vm.set_shared(p->vs_shared.view().data());
  fm.set_shared(p->fs_shared.view().data());
  fm.set_texture_host(this);
  vm.set_texture_host(this);
  load_uniform_blocks(p);
  const shader::Module& vmod = *p->vsm;
  const shader::Module& fmod = *p->fsm;

  // check the attribute buffers reach far enough
  for (const auto& ai : p->attribs) {
    const uint32_t width = ai->type.cols > 1 ? ai->type.cols : 1;
    for (uint32_t col = 0; col < width; ++col) {
      const VertexAttrib& at = vao->attribs[ai->location + col];
      if (!at.enabled) continue;
      const Buffer* b = buffer_at(at.buffer);
      if (!b) { set_error(INVALID_OPERATION); return false; }
      const uint32_t esize = attrib_type_size(at.type) * at.size;
      const uint32_t stride = at.stride ? at.stride : esize;
      const uint64_t last = at.divisor ? (static_cast<uint64_t>(instances) - 1) / at.divisor : max_index;
      if (at.offset + last * stride + (at.type == 0x8368 || at.type == 0x8D9F ? 4 : esize) > b->data.size()) { set_error(INVALID_OPERATION); return false; }
    }
  }

  // varying flags for the rasterizer
  gpu::RasterState rs;
  const int32_t ss = static_cast<int32_t>(target.ss);
  rs.vp_x = vp_[0] * ss; rs.vp_y = vp_[1] * ss; rs.vp_w = vp_[2] * ss; rs.vp_h = vp_[3] * ss;
  rs.depth_near = depth_n_; rs.depth_far = depth_f_;
  if (scissor_test_) { rs.sc_x = sc_[0] * ss; rs.sc_y = sc_[1] * ss; rs.sc_w = sc_[2] * ss; rs.sc_h = sc_[3] * ss; }
  else { rs.sc_x = 0; rs.sc_y = 0; rs.sc_w = static_cast<int32_t>(target.w); rs.sc_h = static_cast<int32_t>(target.h); }
  rs.target_w = static_cast<int32_t>(target.w);
  rs.target_h = static_cast<int32_t>(target.h);
  rs.cull = cull_ ? (cull_mode_ == 0x404 ? 1 : (cull_mode_ == 0x405 ? 2 : 3)) : 0;
  rs.front_ccw = front_face_ == 0x0901;
  rs.offset = offset_fill_;
  rs.offset_factor = po_factor_;
  rs.offset_units = po_units_;
  rs.nvar = p->nvar;
  for (const VaryingLink& l : p->links)
    for (uint32_t k = 0; k < l.count; ++k) rs.flat[l.var_offset + k] = l.flat;
  rs.flat_first = false;
  rs.line_width = line_width_ < 1 ? 1 : (line_width_ > 16 ? 16 : line_width_);

  bool deriv = fmod.uses_derivatives;
  if (!deriv && fmod.uses_implicit_lod) {
    for (const TextureUnit& u : units_) {
      const uint32_t ids[4] = {u.tex2d, u.cube, u.tex3d, u.array};
      for (uint32_t id : ids) {
        const Texture* tx = texture_at(id);
        if (tx && tx->min_filter >= 0x2700) deriv = true;
      }
      if (u.sampler && u.sampler <= samplers_.count() && samplers_[u.sampler - 1] && samplers_[u.sampler - 1]->min_filter >= 0x2700) deriv = true;
    }
  }
  FragmentPipeline pipe(*this, *p, target, fm, deriv);
  uint32_t q = query_active_[0] ? query_active_[0] : query_active_[1];
  if (q) pipe.occlusion = query_at(q);
  fm.reset_privates();

  // vertex shading
  const int32_t vpos_g = vmod.find_builtin_global(shader::kBvPosition), vps_g = vmod.find_builtin_global(shader::kBvPointSize);
  const int32_t vid_g = vmod.find_builtin_global(shader::kBvVertexIndex), iid_g = vmod.find_builtin_global(shader::kBvInstanceIndex);
  const uint32_t pos_slot = vpos_g >= 0 ? vmod.globals[static_cast<uint64_t>(vpos_g)]->slot : 0;
  const uint32_t ps_slot = vps_g >= 0 ? vmod.globals[static_cast<uint64_t>(vps_g)]->slot : 0;

  struct Cached { int64_t key; gpu::RVertex v; };
  List<Cached> cache(a_);
  constexpr uint32_t kCache = 64;
  if (!cache.reserve(kCache)) { set_error(0x505); return false; }
  for (uint32_t i = 0; i < kCache; ++i) { Cached e; e.key = -1; cache.push_back(e); }
  uint32_t cur_instance = 0xFFFFFFFFu;

  auto shade_vertex = [&](uint32_t index, uint32_t vertex_id, uint32_t instance, gpu::RVertex* out) -> bool {
    const int64_t key = static_cast<int64_t>(index);
    Cached& slot = cache[index % kCache];
    if (cur_instance != instance) {
      for (uint32_t i = 0; i < kCache; ++i) cache[i].key = -1;
      cur_instance = instance;
    }
    if (indexed && !tf_active_ && slot.key == key) {
      *out = slot.v;
      return true;
    }
    uint32_t* pr = vm.private_region();
    for (uint32_t i = 0; i < vmod.private_slots; ++i) pr[i] = 0;
    for (const auto& ai : p->attribs) {
      const uint32_t width = ai->type.cols > 1 ? ai->type.cols : 1;
      const uint32_t comps = ai->type.rows;
      const bool want_int = ai->type.base == shader::Base::Int || ai->type.base == shader::Base::Uint;
      for (uint32_t col = 0; col < width; ++col) {
        const VertexAttrib& at = vao->attribs[ai->location + col];
        uint32_t lanes[4];
        if (at.enabled) {
          const Buffer* b = buffer_at(at.buffer);
          const uint32_t esize = attrib_type_size(at.type) * at.size;
          const uint32_t stride = at.stride ? at.stride : esize;
          const uint64_t el = at.divisor ? instance / at.divisor : index;
          read_attrib(at, b->data.view().data() + at.offset + el * stride, at.size, want_int, lanes);
        } else if (want_int) {
          for (int i = 0; i < 4; ++i) lanes[i] = static_cast<uint32_t>(at.cur_is_int ? at.cur_i[i] : static_cast<int32_t>(at.cur_f[i]));
        } else {
          for (int i = 0; i < 4; ++i) lanes[i] = f2u(at.cur_is_int ? static_cast<float>(at.cur_i[i]) : at.cur_f[i]);
        }
        for (uint32_t k = 0; k < comps; ++k) pr[ai->slot + col * comps + k] = lanes[k];
      }
    }
    if (vid_g >= 0) pr[vmod.globals[static_cast<uint64_t>(vid_g)]->slot] = vertex_id;
    if (iid_g >= 0) pr[vmod.globals[static_cast<uint64_t>(iid_g)]->slot] = instance;
    if (vm.start(vmod.main_function, ArrayView<const uint32_t* const>()) != Machine::Status::Done) return false;
    for (int i = 0; i < 4; ++i) out->pos[i] = u2f(pr[pos_slot + i]);
    out->point_size = vps_g >= 0 ? u2f(pr[ps_slot]) : 1.0f;
    for (const VaryingLink& l : p->links)
      for (uint32_t k = 0; k < l.count; ++k) out->var[l.var_offset + k] = u2f(pr[l.vs_slot + k]);
    if (tf_active_) tf_capture(*p, pr);
    slot.key = key;
    slot.v = *out;
    return true;
  };

  const bool restart_ok = indexed;
  const uint32_t restart_val = index_type == 0x1401 ? 0xFF : (index_type == 0x1403 ? 0xFFFF : 0xFFFFFFFFu);
  bool ok = true;
  for (int32_t inst = 0; inst < instances && ok; ++inst) {
    // the list of vertex indices of this draw (with restart markers)
    auto index_at = [&](int32_t i) -> int64_t {
      if (!indexed) return first + i;
      const uint32_t v = fetch_index(idx, index_type, static_cast<uint32_t>(i));
      if (restart_ok && v == restart_val) return -1;
      return v;
    };
    gpu::RVertex va, vb, vc;
    auto get = [&](int32_t i, gpu::RVertex* out) -> bool {
      const int64_t ix = index_at(i);
      if (ix < 0) return false;
      return shade_vertex(static_cast<uint32_t>(ix), static_cast<uint32_t>(ix), static_cast<uint32_t>(inst), out);
    };
    auto tri = [&](int32_t i0, int32_t i1, int32_t i2) {
      if (index_at(i0) < 0 || index_at(i1) < 0 || index_at(i2) < 0) return;
      if (!get(i0, &va) || !get(i1, &vb) || !get(i2, &vc)) { ok = false; return; }
      gpu::draw_triangle(rs, va, vb, vc, pipe);
    };
    switch (mode) {
      case 0:
        for (int32_t i = 0; i < count && ok; ++i) {
          if (index_at(i) < 0) continue;
          if (!get(i, &va)) { ok = false; break; }
          gpu::draw_point(rs, va, pipe);
        }
        break;
      case 1:
        for (int32_t i = 0; i + 1 < count && ok; i += 2) {
          if (index_at(i) < 0 || index_at(i + 1) < 0) continue;
          if (!get(i, &va) || !get(i + 1, &vb)) { ok = false; break; }
          gpu::draw_line(rs, va, vb, pipe);
        }
        break;
      case 2: case 3: {
        int32_t start = 0;
        for (int32_t i = 0; i <= count && ok; ++i) {
          const bool brk = i == count || index_at(i) < 0;
          if (!brk) {
            if (i > start) {
              if (!get(i - 1, &va) || !get(i, &vb)) { ok = false; break; }
              gpu::draw_line(rs, va, vb, pipe);
            }
          } else {
            if (mode == 2 && i - start > 2) {
              if (!get(i - 1, &va) || !get(start, &vb)) { ok = false; break; }
              gpu::draw_line(rs, va, vb, pipe);
            }
            start = i + 1;
          }
        }
        break;
      }
      case 4:
        for (int32_t i = 0; i + 2 < count && ok; i += 3) tri(i, i + 1, i + 2);
        break;
      case 5: {
        int32_t start = 0;
        for (int32_t i = 0; i <= count && ok; ++i) {
          if (i < count && index_at(i) >= 0) {
            if (i >= start + 2) {
              const int32_t k = i - start - 2;
              if (k & 1) tri(i - 1, i - 2, i);
              else tri(i - 2, i - 1, i);
            }
          } else {
            start = i + 1;
          }
        }
        break;
      }
      default: {
        int32_t start = 0;
        for (int32_t i = 0; i <= count && ok; ++i) {
          if (i < count && index_at(i) >= 0) {
            if (i >= start + 2) tri(start, i - 1, i);
          } else {
            start = i + 1;
          }
        }
        break;
      }
    }
  }
  if (!ok) set_error(INVALID_OPERATION);
  if (pipe.occlusion) pipe.occlusion->result = pipe.samples ? 1 : pipe.occlusion->result;
  ++version_;
  return ok;
}

void GlContext::draw_arrays(uint32_t mode, int32_t first, int32_t count, int32_t instances) { run_draw(mode, first, count, 0, 0, instances, false); }
void GlContext::draw_elements(uint32_t mode, int32_t count, uint32_t type, uint64_t offset, int32_t instances) { run_draw(mode, 0, count, type, offset, instances, true); }
void GlContext::draw_range_elements(uint32_t mode, uint32_t, uint32_t, int32_t count, uint32_t type, uint64_t offset) { run_draw(mode, 0, count, type, offset, 1, true); }

// --- Uniform blocks -----------------------------------------------------------------------------------------------------------------------------------

void GlContext::load_uniform_blocks(ProgramObj* p) {
  for (int s = 0; s < 2; ++s) {
    const shader::Module& m = s == 0 ? *p->vsm : *p->fsm;
    uint32_t* shared = s == 0 ? p->vs_shared.view().data() : p->fs_shared.view().data();
    for (uint64_t b = 0; b < m.blocks.count(); ++b) {
      const int32_t merged = static_cast<int32_t>(get_uniform_block_index(static_cast<uint32_t>(program_), m.blocks[b]->name.view()));
      if (merged < 0 || static_cast<uint64_t>(merged) >= p->block_bindings.count()) continue;
      const int32_t binding = p->block_bindings[static_cast<uint64_t>(merged)];
      if (binding < 0 || binding >= static_cast<int32_t>(kMaxUniformBindings)) continue;
      const BufferBinding& bb = ubo_bindings_[binding];
      const Buffer* buf = buffer_at(bb.buffer);
      ConstByteView bytes = buf ? buf->data.view() : ConstByteView();
      if (buf && bb.size && bb.offset + bb.size <= bytes.size()) bytes = bytes.subview(0, bb.offset + bb.size);
      uint32_t off = 0;
      for (uint64_t g = 0; g < m.globals.count(); ++g) {
        const shader::Global& gv = *m.globals[g];
        if (gv.block != static_cast<int32_t>(b)) continue;
        uint32_t al, sz;
        shader::layout_of(m, gv.type, shader::Layout::Std140, &al, &sz);
        off = (off + al - 1) / al * al;
        if (buf) shader::load_layout(m, gv.type, shader::Layout::Std140, bytes, (bb.offset) + off, shared + gv.slot);
        off += sz;
      }
    }
  }
}

// --- Clears -------------------------------------------------------------------------------------------------------------------------------------------------

void GlContext::clear(uint32_t mask) {
  if (lost_) return;
  if (mask & ~(0x100u | 0x400u | 0x4000u)) { set_error(INVALID_VALUE); return; }
  uint32_t status;
  if (!framebuffer_complete(draw_fb_, &status)) { set_error(INVALID_FRAMEBUFFER_OPERATION); return; }
  Target t;
  resolve_target(draw_fb_, true, &t);
  if (!t.ok) return;
  int32_t x0 = 0, y0 = 0, x1 = static_cast<int32_t>(t.w), y1 = static_cast<int32_t>(t.h);
  if (scissor_test_) {
    const int32_t k = static_cast<int32_t>(t.ss);
    x0 = sc_[0] * k > x0 ? sc_[0] * k : x0;
    y0 = sc_[1] * k > y0 ? sc_[1] * k : y0;
    x1 = (sc_[0] + sc_[2]) * k < x1 ? (sc_[0] + sc_[2]) * k : x1;
    y1 = (sc_[1] + sc_[3]) * k < y1 ? (sc_[1] + sc_[3]) * k : y1;
  }
  if (x0 >= x1 || y0 >= y1) return;
  if (mask & 0x4000) {
    for (uint32_t bi = 0; bi < kMaxDrawBuffers; ++bi) {
      Image* im = t.color[bi];
      if (!im) continue;
      const FormatInfo* fi = im->info;
      for (int32_t y = y0; y < y1; ++y)
        for (int32_t x = x0; x < x1; ++x) {
          uint32_t raw[4];
          read_texel(*im, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.layer[bi], raw);
          uint32_t w4[4];
          for (int k = 0; k < 4; ++k) {
            if (!cmask_[k]) { w4[k] = raw[k]; continue; }
            float v = clear_color_[k];
            switch (fi->store) {
              case Store::U8x4:
                if (fi->srgb && k < 3) v = linear_to_srgb(v);
                if (!attrs_.alpha && t.is_default && k == 3) v = 1;
                w4[k] = to_u8(v);
                break;
              case Store::F32x4: w4[k] = f2u(v); break;
              default: w4[k] = static_cast<uint32_t>(static_cast<int32_t>(v)); break;
            }
          }
          if (fi->base == 0x1907 || fi->base == 0x8227 || fi->base == 0x1903) { if (fi->store == Store::U8x4) w4[3] = 255; }
          write_texel(*im, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.layer[bi], w4);
        }
    }
    if (t.is_default && t.color[0]) { dirty_.add(x0, y0); dirty_.add(x1 - 1, y1 - 1); }
  }
  if ((mask & 0x100) && t.depth && dmask_) {
    for (int32_t y = y0; y < y1; ++y)
      for (int32_t x = x0; x < x1; ++x) {
        uint32_t raw[4];
        read_texel(*t.depth, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
        raw[0] = f2u(clamp01(clear_depth_));
        write_texel(*t.depth, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
      }
  }
  if ((mask & 0x400) && t.stencil && t.stencil->info->store == Store::DepthStencil) {
    for (int32_t y = y0; y < y1; ++y)
      for (int32_t x = x0; x < x1; ++x) {
        uint32_t raw[4];
        read_texel(*t.stencil, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
        const uint32_t wm = swmask_[0] & 255;
        raw[1] = (raw[1] & ~wm) | (static_cast<uint32_t>(clear_stencil_) & wm & 255);
        write_texel(*t.stencil, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
      }
  }
  ++version_;
}

void GlContext::clear_buffer(uint32_t buffer, int32_t drawbuffer, const uint32_t value[4], uint32_t kind) {
  uint32_t status;
  if (!framebuffer_complete(draw_fb_, &status)) { set_error(INVALID_FRAMEBUFFER_OPERATION); return; }
  Target t;
  resolve_target(draw_fb_, true, &t);
  if (!t.ok) return;
  int32_t x0 = 0, y0 = 0, x1 = static_cast<int32_t>(t.w), y1 = static_cast<int32_t>(t.h);
  if (scissor_test_) {
    const int32_t k = static_cast<int32_t>(t.ss);
    x0 = sc_[0] * k > x0 ? sc_[0] * k : x0;
    y0 = sc_[1] * k > y0 ? sc_[1] * k : y0;
    x1 = (sc_[0] + sc_[2]) * k < x1 ? (sc_[0] + sc_[2]) * k : x1;
    y1 = (sc_[1] + sc_[3]) * k < y1 ? (sc_[1] + sc_[3]) * k : y1;
  }
  if (buffer == 0x1800) {
    if (drawbuffer < 0 || drawbuffer >= static_cast<int32_t>(kMaxDrawBuffers)) { set_error(INVALID_VALUE); return; }
    Image* im = t.color[drawbuffer];
    if (!im) return;
    const FormatInfo* fi = im->info;
    for (int32_t y = y0; y < y1; ++y)
      for (int32_t x = x0; x < x1; ++x) {
        uint32_t raw[4], w4[4];
        read_texel(*im, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.layer[drawbuffer], raw);
        for (int k = 0; k < 4; ++k) {
          if (!cmask_[k]) { w4[k] = raw[k]; continue; }
          if (fi->store == Store::U8x4) { float v = u2f(value[k]); if (fi->srgb && k < 3) v = linear_to_srgb(v); w4[k] = to_u8(v); }
          else if (fi->store == Store::F32x4) w4[k] = kind == 0 ? value[k] : f2u(static_cast<float>(static_cast<int32_t>(value[k])));
          else w4[k] = kind == 0 ? static_cast<uint32_t>(static_cast<int32_t>(u2f(value[k]))) : value[k];
        }
        write_texel(*im, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.layer[drawbuffer], w4);
      }
    if (t.is_default && drawbuffer == 0) { dirty_.add(x0, y0); dirty_.add(x1 - 1, y1 - 1); }
  } else if (buffer == 0x1801 && t.depth && dmask_) {
    for (int32_t y = y0; y < y1; ++y)
      for (int32_t x = x0; x < x1; ++x) {
        uint32_t raw[4];
        read_texel(*t.depth, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
        raw[0] = f2u(clamp01(u2f(value[0])));
        write_texel(*t.depth, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
      }
  } else if (buffer == 0x1802 && t.stencil && t.stencil->info->store == Store::DepthStencil) {
    for (int32_t y = y0; y < y1; ++y)
      for (int32_t x = x0; x < x1; ++x) {
        uint32_t raw[4];
        read_texel(*t.stencil, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
        const uint32_t wm = swmask_[0] & 255;
        raw[1] = (raw[1] & ~wm) | (value[0] & wm & 255);
        write_texel(*t.stencil, static_cast<uint32_t>(x), static_cast<uint32_t>(y), t.depth_layer, raw);
      }
  } else if (buffer != 0x1800 && buffer != 0x1801 && buffer != 0x1802) {
    set_error(INVALID_ENUM);
  }
  ++version_;
}

void GlContext::clear_buffer_fi(int32_t, float depth, int32_t stencil) {
  const float saved_d = clear_depth_;
  const int32_t saved_s = clear_stencil_;
  clear_depth_ = depth;
  clear_stencil_ = stencil;
  clear(0x100 | 0x400);
  clear_depth_ = saved_d;
  clear_stencil_ = saved_s;
}

// --- Reads and copies --------------------------------------------------------------------------------------------------------------------------------

void GlContext::read_pixels(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t format, uint32_t type, ByteView out) {
  uint32_t status;
  if (!framebuffer_complete(read_fb_, &status)) { set_error(INVALID_FRAMEBUFFER_OPERATION); return; }
  Target t;
  resolve_target(read_fb_, false, &t);
  if (!t.ok) return;
  Image* src = nullptr;
  uint32_t layer = 0;
  if (read_fb_ == 0) src = default_read_back_ ? &def_color_ : nullptr;
  else for (uint32_t i = 0; i < kMaxDrawBuffers && !src; ++i) if (t.color[i]) { src = t.color[i]; layer = t.layer[i]; }
  Image resolved(a_);
  if (src && src->ss > 1 && read_fb_ == 0) {   // the default framebuffer is resolved implicitly
    if (!resolve_samples(*src, 0, &resolved)) { set_error(0x505); return; }
    src = &resolved;
  }
  if (!src || src->ss > 1) { set_error(INVALID_OPERATION); return; }
  uint32_t err = 0;
  // zero the destination first (pixels outside the framebuffer read as zero)
  if (out.size()) __builtin_memset(out.data(), 0, out.size());
  if (!pack_pixels(*src, x, y, w, h, layer, format, type, out, pack, &err)) set_error(err);
}

void GlContext::copy_tex_image_2d(uint32_t target, int32_t level, uint32_t internal, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t border) {
  uint32_t face;
  Texture* tx = bound_texture_for(target, &face);
  if (!tx) return;
  const FormatInfo* fi = find_format(internal);
  if (!fi || border != 0) { set_error(fi ? INVALID_VALUE : INVALID_ENUM); return; }
  Target rt;
  resolve_target(read_fb_, false, &rt);
  if (!rt.ok) { set_error(INVALID_FRAMEBUFFER_OPERATION); return; }
  Image* src = read_fb_ == 0 ? &def_color_ : nullptr;
  uint32_t layer = 0;
  if (!src) for (uint32_t i = 0; i < kMaxDrawBuffers && !src; ++i) if (rt.color[i]) { src = rt.color[i]; layer = rt.layer[i]; }
  Image resolved(a_);
  if (src && src->ss > 1) {
    if (read_fb_ != 0 || !resolve_samples(*src, 0, &resolved)) { set_error(INVALID_OPERATION); return; }
    src = &resolved;
  }
  if (!src) { set_error(INVALID_OPERATION); return; }
  Image* im = make_texture_image(tx, target, level);
  if (!im || !im->allocate(fi, w, h, 1)) { set_error(INVALID_OPERATION); return; }
  for (uint32_t yy = 0; yy < h; ++yy)
    for (uint32_t xx = 0; xx < w; ++xx) {
      const int32_t sx = x + static_cast<int32_t>(xx), sy = y + static_cast<int32_t>(yy);
      if (sx < 0 || sy < 0 || sx >= static_cast<int32_t>(src->w) || sy >= static_cast<int32_t>(src->h)) continue;
      uint32_t raw[4], lanes[4], out[4];
      read_texel(*src, static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), layer, raw);
      for (int k = 0; k < 4; ++k) lanes[k] = src->info->store == Store::U8x4 ? f2u(raw[k] / 255.0f) : raw[k];
      store_lanes(fi, lanes, src->info->store == Store::I32x4 || src->info->store == Store::U32x4, false, out);
      write_texel(*im, xx, yy, 0, out);
    }
  ++version_;
}

void GlContext::copy_tex_sub_image(uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t zo, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  uint32_t face;
  Texture* tx = bound_texture_for(target, &face);
  if (!tx) return;
  Image* im = texture_image(tx, target, level);
  if (!im) { set_error(INVALID_OPERATION); return; }
  Target rt;
  resolve_target(read_fb_, false, &rt);
  if (!rt.ok) { set_error(INVALID_FRAMEBUFFER_OPERATION); return; }
  Image* src = read_fb_ == 0 ? &def_color_ : nullptr;
  uint32_t layer = 0;
  if (!src) for (uint32_t i = 0; i < kMaxDrawBuffers && !src; ++i) if (rt.color[i]) { src = rt.color[i]; layer = rt.layer[i]; }
  Image resolved(a_);
  if (src && src->ss > 1) {
    if (read_fb_ != 0 || !resolve_samples(*src, 0, &resolved)) { set_error(INVALID_OPERATION); return; }
    src = &resolved;
  }
  if (!src) { set_error(INVALID_OPERATION); return; }
  if (xo < 0 || yo < 0 || zo < 0 || static_cast<uint64_t>(xo) + w > im->w || static_cast<uint64_t>(yo) + h > im->h || static_cast<uint32_t>(zo) >= im->d) { set_error(INVALID_VALUE); return; }
  for (uint32_t yy = 0; yy < h; ++yy)
    for (uint32_t xx = 0; xx < w; ++xx) {
      const int32_t sx = x + static_cast<int32_t>(xx), sy = y + static_cast<int32_t>(yy);
      if (sx < 0 || sy < 0 || sx >= static_cast<int32_t>(src->w) || sy >= static_cast<int32_t>(src->h)) continue;
      uint32_t raw[4], lanes[4], out[4];
      read_texel(*src, static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), layer, raw);
      for (int k = 0; k < 4; ++k) lanes[k] = src->info->store == Store::U8x4 ? f2u(raw[k] / 255.0f) : raw[k];
      store_lanes(im->info, lanes, src->info->store == Store::I32x4 || src->info->store == Store::U32x4, false, out);
      write_texel(*im, static_cast<uint32_t>(xo) + xx, static_cast<uint32_t>(yo) + yy, static_cast<uint32_t>(zo), out);
    }
  ++version_;
}

void GlContext::blit_framebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, uint32_t mask, uint32_t filter) {
  Target rt, dt;
  resolve_target(read_fb_, false, &rt);
  resolve_target(draw_fb_, true, &dt);
  if (!rt.ok || !dt.ok) { set_error(INVALID_FRAMEBUFFER_OPERATION); return; }
  if (dt.ss > 1 && rt.ss == 1) { dx0 *= dt.ss; dx1 *= dt.ss; dy0 *= dt.ss; dy1 *= dt.ss; }
  const int32_t sw = sx1 - sx0, sh = sy1 - sy0, dw = dx1 - dx0, dh = dy1 - dy0;
  if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return;
  const int32_t ddx0 = dx0 < dx1 ? dx0 : dx1, ddx1 = dx0 < dx1 ? dx1 : dx0, ddy0 = dy0 < dy1 ? dy0 : dy1, ddy1 = dy0 < dy1 ? dy1 : dy0;
  auto copy = [&](Image* src, uint32_t sl, Image* dst, uint32_t dl, bool color) {
    if (!src || !dst) return;
    for (int32_t y = ddy0; y < ddy1; ++y)
      for (int32_t x = ddx0; x < ddx1; ++x) {
        if (x < 0 || y < 0 || x >= static_cast<int32_t>(dst->w) || y >= static_cast<int32_t>(dst->h)) continue;
        if (scissor_test_ && (x < sc_[0] || y < sc_[1] || x >= sc_[0] + sc_[2] || y >= sc_[1] + sc_[3])) continue;
        // source position of the pixel centre
        const float fu = ((static_cast<float>(x - dx0) + 0.5f) / static_cast<float>(dw)) * static_cast<float>(sw) + static_cast<float>(sx0);
        const float fv = ((static_cast<float>(y - dy0) + 0.5f) / static_cast<float>(dh)) * static_cast<float>(sh) + static_cast<float>(sy0);
        auto at = [&](int32_t px, int32_t py, uint32_t out[4]) {
          px = px < 0 ? 0 : (px >= static_cast<int32_t>(src->w) ? static_cast<int32_t>(src->w) - 1 : px);
          py = py < 0 ? 0 : (py >= static_cast<int32_t>(src->h) ? static_cast<int32_t>(src->h) - 1 : py);
          read_texel(*src, static_cast<uint32_t>(px), static_cast<uint32_t>(py), sl, out);
        };
        uint32_t out[4];
        if (color && filter == 0x2601 && (src->info->store == Store::U8x4 || src->info->store == Store::F32x4)) {
          const int32_t i0 = static_cast<int32_t>(shader::ffloor(fu - 0.5f)), j0 = static_cast<int32_t>(shader::ffloor(fv - 0.5f));
          const float a = fu - 0.5f - static_cast<float>(i0), b = fv - 0.5f - static_cast<float>(j0);
          float acc[4] = {0, 0, 0, 0};
          for (int jy = 0; jy < 2; ++jy)
            for (int ix = 0; ix < 2; ++ix) {
              uint32_t raw[4];
              at(i0 + ix, j0 + jy, raw);
              const float wgt = (ix ? a : 1 - a) * (jy ? b : 1 - b);
              for (int k = 0; k < 4; ++k) acc[k] += wgt * (src->info->store == Store::U8x4 ? static_cast<float>(raw[k]) : u2f(raw[k]));
            }
          for (int k = 0; k < 4; ++k) out[k] = dst->info->store == Store::U8x4 ? (src->info->store == Store::U8x4 ? static_cast<uint32_t>(acc[k] + 0.5f) : to_u8(acc[k])) : f2u(src->info->store == Store::U8x4 ? acc[k] / 255.0f : acc[k]);
        } else {
          at(static_cast<int32_t>(shader::ffloor(fu)), static_cast<int32_t>(shader::ffloor(fv)), out);
          if (src->info->store != dst->info->store) {
            uint32_t lanes[4];
            for (int k = 0; k < 4; ++k) lanes[k] = src->info->store == Store::U8x4 ? f2u(out[k] / 255.0f) : out[k];
            store_lanes(dst->info, lanes, src->info->store == Store::I32x4 || src->info->store == Store::U32x4, false, out);
          }
        }
        write_texel(*dst, static_cast<uint32_t>(x), static_cast<uint32_t>(y), dl, out);
      }
  };
  if (mask & 0x4000) {
    Image* src = nullptr;
    uint32_t sl = 0;
    if (read_fb_ == 0) src = &def_color_;
    else for (uint32_t i = 0; i < kMaxDrawBuffers && !src; ++i) if (rt.color[i]) { src = rt.color[i]; sl = rt.layer[i]; }
    Image resolved(a_);
    if (src && src->ss > 1) {
      if (dt.ss > 1) { set_error(INVALID_OPERATION); return; }
      if (!resolve_samples(*src, sl, &resolved)) { set_error(0x505); return; }
      src = &resolved;
      sl = 0;
    }
    for (uint32_t i = 0; i < kMaxDrawBuffers; ++i)
      if (dt.color[i]) copy(src, sl, dt.color[i], dt.layer[i], true);
    if (dt.is_default && dt.color[0]) { dirty_.add(ddx0, ddy0); dirty_.add(ddx1 - 1, ddy1 - 1); }
  }
  if ((mask & 0x100) && rt.depth && dt.depth) {
    Image resolved(a_);
    Image* ds = rt.depth;
    uint32_t dl = rt.depth_layer;
    if (ds->ss > 1) {
      if (dt.ss > 1 || !resolve_samples(*ds, dl, &resolved)) { set_error(INVALID_OPERATION); return; }
      ds = &resolved;
      dl = 0;
    }
    copy(ds, dl, dt.depth, dt.depth_layer, false);
  }
  ++version_;
}

// --- Presenting --------------------------------------------------------------------------------------------------------------------------------------

bool GlContext::present(uint32_t* dst, uint32_t pitch, gpu::TouchBox* changed, bool force_all) {
  if (!def_color_.defined()) return false;
  const int32_t k = static_cast<int32_t>(def_color_.ss);
  int32_t x0 = 0, y0 = 0, x1 = static_cast<int32_t>(width_) - 1, y1 = static_cast<int32_t>(height_) - 1;
  if (!force_all && !all_dirty_) {
    if (dirty_.empty()) return false;
    x0 = dirty_.x0 < 0 ? 0 : dirty_.x0 / k;
    y0 = dirty_.y0 < 0 ? 0 : dirty_.y0 / k;
    x1 = dirty_.x1 / k >= static_cast<int32_t>(width_) ? static_cast<int32_t>(width_) - 1 : dirty_.x1 / k;
    y1 = dirty_.y1 / k >= static_cast<int32_t>(height_) ? static_cast<int32_t>(height_) - 1 : dirty_.y1 / k;
  }
  const uint8_t* px = def_color_.px.view().data();
  const uint64_t stride = uint64_t{def_color_.w} * 4;
  for (int32_t gy = y0; gy <= y1; ++gy) {
    const int32_t row = static_cast<int32_t>(height_) - 1 - gy;   // GL rows run bottom to top
    uint32_t* d = dst + static_cast<uint64_t>(row) * pitch;
    for (int32_t x = x0; x <= x1; ++x) {
      uint32_t sum[4] = {0, 0, 0, 0};   // the samples of the pixel, averaged
      for (int32_t j = 0; j < k; ++j)
        for (int32_t i = 0; i < k; ++i) {
          const uint8_t* s = px + static_cast<uint64_t>(gy * k + j) * stride + static_cast<uint64_t>(x * k + i) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += s[c];
        }
      const uint32_t n = static_cast<uint32_t>(k * k);
      uint32_t r = (sum[0] + n / 2) / n, g = (sum[1] + n / 2) / n, b = (sum[2] + n / 2) / n, a = attrs_.alpha ? (sum[3] + n / 2) / n : 255;
      if (attrs_.alpha && attrs_.premultiplied) {   // the canvas keeps straight alpha
        if (a == 0) { r = g = b = 0; }
        else if (a < 255) {
          r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
          if (r > 255) r = 255;
          if (g > 255) g = 255;
          if (b > 255) b = 255;
        }
      }
      d[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  if (changed) {
    changed->x0 = x0;
    changed->x1 = x1;
    changed->y0 = static_cast<int32_t>(height_) - 1 - y1;
    changed->y1 = static_cast<int32_t>(height_) - 1 - y0;
  }
  dirty_ = gpu::TouchBox();
  all_dirty_ = false;
  return true;
}

}  // namespace webgl
