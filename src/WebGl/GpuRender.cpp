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

// WebGPU passes: binding resources to shaders, render passes (vertex fetch, rasterization, the per-fragment pipeline) and compute dispatches.
#include "WebGl/GlPixels.hpp"
#include "WebGl/GpuDevice.hpp"

namespace wgpu {

using shader::Base;
using shader::GlobalKind;

namespace {
inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline uint8_t to_u8(float v) { return !(v > 0) ? 0 : (v >= 1 ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f)); }

bool compare(uint8_t fn, float a, float b) {   // a fn b (never, less, equal, less-equal, greater, not-equal, greater-equal, always)
  switch (fn) {
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

uint32_t stencil_op(uint8_t op, uint32_t cur, uint32_t ref) {
  switch (op) {
    case 1: return 0;
    case 2: return ref;
    case 3: return ~cur & 255;
    case 4: return cur < 255 ? cur + 1 : 255;
    case 5: return cur > 0 ? cur - 1 : 0;
    case 6: return (cur + 1) & 255;
    case 7: return (cur - 1) & 255;
    default: return cur;
  }
}

float factor(uint8_t f, const float src[4], const float dst[4], const float cc[4], int comp, bool alpha) {
  switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return src[alpha ? 3 : comp];
    case 3: return 1 - src[alpha ? 3 : comp];
    case 4: return src[3];
    case 5: return 1 - src[3];
    case 6: return dst[alpha ? 3 : comp];
    case 7: return 1 - dst[alpha ? 3 : comp];
    case 8: return dst[3];
    case 9: return 1 - dst[3];
    case 10: return alpha ? 1.0f : (src[3] < 1 - dst[3] ? src[3] : 1 - dst[3]);
    case 11: return cc[alpha ? 3 : comp];
    case 12: return 1 - cc[alpha ? 3 : comp];
    default: return 1;
  }
}

float apply_op(uint8_t op, float s, float d, float sf, float df) {
  switch (op) {
    case 1: return s * sf - d * df;
    case 2: return d * df - s * sf;
    case 3: return s < d ? s : d;
    case 4: return s > d ? s : d;
    default: return s * sf + d * df;
  }
}

// Vertex attribute formats: decode one attribute into lanes (floats, or raw integers when `ints`).
void decode_vertex(uint8_t fmt, const uint8_t* p, uint32_t lanes[4], bool* ints) {
  static const struct { uint8_t kind; uint8_t comps; } kTable[] = {
      {2, 2}, {2, 4}, {3, 2}, {3, 4}, {0, 2}, {0, 4}, {1, 2}, {1, 4}, {5, 2}, {5, 4}, {6, 2}, {6, 4}, {4, 2}, {4, 4}, {13, 2}, {13, 4}, {7, 2}, {7, 4}, {10, 1}, {10, 2}, {10, 3}, {10, 4},
      {8, 1}, {8, 2}, {8, 3}, {8, 4}, {9, 1}, {9, 2}, {9, 3}, {9, 4}, {12, 4}, {14, 4}};
  *ints = false;
  if (fmt >= sizeof(kTable) / sizeof(kTable[0])) { lanes[0] = lanes[1] = lanes[2] = 0; lanes[3] = f2u(1.0f); return; }
  const uint8_t kind = kTable[fmt].kind, n = kTable[fmt].comps;
  *ints = kind == 2 || kind == 3 || kind == 5 || kind == 6 || kind == 8 || kind == 9;
  uint32_t out[4] = {0, 0, 0, *ints ? 1u : f2u(1.0f)};
  if (kind == 12) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    out[0] = f2u((v & 1023) / 1023.0f); out[1] = f2u(((v >> 10) & 1023) / 1023.0f); out[2] = f2u(((v >> 20) & 1023) / 1023.0f); out[3] = f2u((v >> 30) / 3.0f);
  } else if (kind == 14) {   // unorm8x4-bgra
    out[0] = f2u(p[2] / 255.0f); out[1] = f2u(p[1] / 255.0f); out[2] = f2u(p[0] / 255.0f); out[3] = f2u(p[3] / 255.0f);
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      switch (kind) {
        case 0: out[i] = f2u(p[i] / 255.0f); break;
        case 1: { const float x = static_cast<int8_t>(p[i]) / 127.0f; out[i] = f2u(x < -1 ? -1 : x); break; }
        case 2: out[i] = p[i]; break;
        case 3: out[i] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(p[i]))); break;
        case 4: { uint16_t v; __builtin_memcpy(&v, p + 2 * i, 2); out[i] = f2u(v / 65535.0f); break; }
        case 13: { int16_t v; __builtin_memcpy(&v, p + 2 * i, 2); const float x = v / 32767.0f; out[i] = f2u(x < -1 ? -1 : x); break; }
        case 5: { uint16_t v; __builtin_memcpy(&v, p + 2 * i, 2); out[i] = v; break; }
        case 6: { int16_t v; __builtin_memcpy(&v, p + 2 * i, 2); out[i] = static_cast<uint32_t>(static_cast<int32_t>(v)); break; }
        case 7: { uint16_t v; __builtin_memcpy(&v, p + 2 * i, 2); out[i] = f2u(shader::half_to_float(v)); break; }
        default: __builtin_memcpy(&out[i], p + 4 * i, 4); break;
      }
    }
  }
  for (int i = 0; i < 4; ++i) lanes[i] = out[i];
}

uint32_t vertex_size(uint8_t fmt) {
  static const uint8_t kSizes[] = {2, 4, 2, 4, 2, 4, 2, 4, 4, 8, 4, 8, 4, 8, 4, 8, 4, 8, 4, 8, 12, 16, 4, 8, 12, 16, 4, 8, 12, 16, 4, 4};
  return fmt < sizeof(kSizes) ? kSizes[fmt] : 0;
}

}  // namespace

// --- Binding resources to a shader -----------------------------------------------------------------------------------------------------------------------

namespace {
shader::Layout layout_for(const shader::Global& g) { return g.storage ? shader::Layout::Natural : shader::Layout::WgslUniform; }
}

bool Device::setup_stage(shader::Module& m, List<uint32_t>* region, List<uint32_t>*) {
  struct Slice { const BgEntry* e; Buffer* b; uint64_t offset, size; };
  // lengths of runtime-sized arrays
  for (uint64_t gi = 0; gi < m.globals.count(); ++gi) {
    shader::Global& g = *m.globals[gi];
    if (!g.shared || g.binding < 0 || g.kind != GlobalKind::Buffer) continue;
    if (g.group < 0 || g.group >= 8 || bind_groups_[g.group] == 0) return fail("a bind group is missing");
    const BindGroup& bg = *groups_[bind_groups_[g.group] - 1];
    const BgEntry* e = nullptr;
    for (const BgEntry& x : bg.entries.view()) if (x.binding == static_cast<uint32_t>(g.binding)) e = &x;
    if (!e || e->kind != 0) return fail("a binding is missing from the bind group");
    const Buffer* b = buf(e->id);
    if (!b || b->destroyed) return fail("a bound buffer is destroyed");
    uint64_t avail = b->data.size() > e->offset ? b->data.size() - e->offset : 0;
    if (e->size && e->size < avail) avail = e->size;
    uint32_t al, sz;
    if (g.type.array == shader::kRuntimeArray) {
      shader::layout_of(m, g.type.element(), shader::Layout::Natural, &al, &sz);
      const uint32_t stride = (sz + al - 1) / al * al;
      shader::set_runtime_length(m, static_cast<uint32_t>(gi), stride ? static_cast<uint32_t>(avail / stride) : 0);
    } else if (g.type.base == Base::Struct) {
      const shader::StructDef* sd = m.struct_of(g.type);
      if (sd && sd->fields.count() && sd->fields[sd->fields.count() - 1].type.array == shader::kRuntimeArray) {
        const uint32_t last = static_cast<uint32_t>(sd->fields.count() - 1);
        const uint32_t off = shader::field_offset(m, *sd, last, shader::Layout::Natural);
        shader::layout_of(m, sd->fields[last].type.element(), shader::Layout::Natural, &al, &sz);
        const uint32_t stride = (sz + al - 1) / al * al;
        shader::set_runtime_length(m, static_cast<uint32_t>(gi), stride && avail > off ? static_cast<uint32_t>((avail - off) / stride) : 0);
      }
    }
  }
  region->clear();
  if (!region->reserve(m.shared_slots + 1)) return fail("out of memory");
  for (uint32_t i = 0; i < m.shared_slots + 1; ++i) region->push_back(0);
  uint32_t* base = region->view().data();
  for (uint64_t gi = 0; gi < m.globals.count(); ++gi) {
    const shader::Global& g = *m.globals[gi];
    if (!g.shared || g.binding < 0) continue;
    const BindGroup& bg = *groups_[bind_groups_[g.group] - 1];
    const BindGroupLayout& layout = *bgls_[bg.layout - 1];
    const BgEntry* e = nullptr;
    for (const BgEntry& x : bg.entries.view()) if (x.binding == static_cast<uint32_t>(g.binding)) e = &x;
    if (!e) return fail("a binding is missing from the bind group");
    if (g.kind == GlobalKind::Buffer) {
      uint64_t offset = e->offset;
      // dynamic offsets follow the layout's order
      uint32_t k = 0;
      for (const BglEntry& le : layout.entries.view()) {
        if (le.binding == static_cast<uint32_t>(g.binding)) { if (le.dynamic_offset && k < dyn_count_[g.group]) offset += dyn_all_[dyn_start_[g.group] + k]; break; }
        if (le.dynamic_offset) ++k;
      }
      const Buffer* b = buf(e->id);
      ConstByteView bytes = b->data.view();
      if (offset > bytes.size()) return fail("buffer binding out of range");
      bytes = bytes.subview(offset);
      if (e->size && e->size < bytes.size()) bytes = bytes.subview(0, e->size);
      shader::load_layout(m, g.type, layout_for(g), bytes, 0, base + g.slot);
    } else if (g.type.base == Base::SamplerState) {
      const Sampler* s = e->id && e->id <= samplers_.count() ? samplers_[e->id - 1].get() : nullptr;
      base[g.slot] = register_bound(nullptr, s);
    } else if (g.type.base == Base::Texture) {
      const TextureView* v = view(e->id);
      if (!v) return fail("a bound texture view is missing");
      base[g.slot] = register_bound(v, nullptr);
    }
  }
  return true;
}

bool Device::write_back(shader::Module& m, List<uint32_t>& region) {
  uint32_t* base = region.view().data();
  for (uint64_t gi = 0; gi < m.globals.count(); ++gi) {
    const shader::Global& g = *m.globals[gi];
    if (!g.shared || g.binding < 0 || g.kind != GlobalKind::Buffer || !g.storage || g.is_read_only) continue;
    const BindGroup& bg = *groups_[bind_groups_[g.group] - 1];
    const BindGroupLayout& layout = *bgls_[bg.layout - 1];
    const BgEntry* e = nullptr;
    for (const BgEntry& x : bg.entries.view()) if (x.binding == static_cast<uint32_t>(g.binding)) e = &x;
    if (!e) continue;
    uint64_t offset = e->offset;
    uint32_t k = 0;
    for (const BglEntry& le : layout.entries.view()) {
      if (le.binding == static_cast<uint32_t>(g.binding)) { if (le.dynamic_offset && k < dyn_count_[g.group]) offset += dyn_all_[dyn_start_[g.group] + k]; break; }
      if (le.dynamic_offset) ++k;
    }
    Buffer* b = buf(e->id);
    ByteView bytes = b->data.writable_view();
    if (offset > bytes.size()) continue;
    bytes = bytes.subview(offset);
    if (e->size && e->size < bytes.size()) bytes = bytes.subview(0, e->size);
    shader::store_layout(m, g.type, layout_for(g), bytes, 0, base + g.slot);
  }
  return true;
}

bool Device::set_bind_group(uint32_t index, uint32_t group, ArrayView<const uint32_t> offsets) {
  if (index >= 8) return fail("bind group index out of range");
  if (group > groups_.count()) return fail("bad bind group");
  bind_groups_[index] = group;
  dyn_start_[index] = static_cast<uint32_t>(dyn_all_.count());
  dyn_count_[index] = static_cast<uint32_t>(offsets.size());
  for (uint32_t o : offsets) dyn_all_.push_back(o);
  return true;
}

// --- Compute ---------------------------------------------------------------------------------------------------------------------------------------------

bool Device::begin_compute_pass() {
  if (pass_) return fail("a pass is already open");
  pass_ = 2;
  cur_pipeline_ = 0;
  for (auto& g : bind_groups_) g = 0;
  return true;
}

bool Device::end_compute_pass() {
  pass_ = 0;
  return true;
}

bool Device::set_pipeline(uint32_t id) {
  cur_pipeline_ = id;
  return true;
}

namespace {

// Fills the argument lanes of an entry point from builtin values and returns the pointers for Machine::start.
struct EntryArgs {
  explicit EntryArgs(allocators::IAllocator& a) : lanes(a), ptrs(a) {}
  List<uint32_t> lanes;
  List<const uint32_t*> ptrs;
};

struct IoSource {
  // (builtin, location) -> up to four lanes; returns the lanes available
  virtual uint32_t get(int32_t builtin, int32_t location, uint32_t out[4]) = 0;

 protected:
  ~IoSource() = default;
};

bool build_args(const shader::Module& m, const shader::Function& fn, IoSource& io, EntryArgs* out) {
  uint32_t total = 0;
  for (uint64_t i = 0; i < fn.params.count(); ++i) total += m.slots_of(fn.params[i].type);
  if (!out->lanes.reserve(total + 1)) return false;
  for (uint32_t i = 0; i < total + 1; ++i) out->lanes.push_back(0);
  uint32_t off = 0;
  List<uint32_t> offsets(out->lanes.count() ? m.allocator : m.allocator);
  for (uint64_t i = 0; i < fn.params.count(); ++i) {
    const shader::Param& p = fn.params[i];
    uint32_t* dst = out->lanes.view().data() + off;
    uint32_t tmp[4] = {0, 0, 0, 0};
    if (p.type.base == Base::Struct) {
      const shader::StructDef* sd = m.struct_of(p.type);
      if (sd)
        for (uint64_t f = 0; f < sd->fields.count(); ++f) {
          const shader::Field& fld = sd->fields[f];
          const uint32_t n = io.get(fld.builtin, fld.location, tmp);
          const uint32_t want = m.slots_of(fld.type);
          for (uint32_t k = 0; k < want && k < 4; ++k) dst[fld.offset + k] = k < n ? tmp[k] : (k == 3 ? (fld.type.base == Base::Float ? f2u(1.0f) : 1u) : 0u);
        }
    } else {
      const uint32_t n = io.get(p.builtin, p.location, tmp);
      const uint32_t want = m.slots_of(p.type);
      for (uint32_t k = 0; k < want && k < 4; ++k) dst[k] = k < n ? tmp[k] : (k == 3 ? (p.type.base == Base::Float ? f2u(1.0f) : 1u) : 0u);
    }
    out->ptrs.push_back(dst);
    off += m.slots_of(p.type);
  }
  return true;
}

struct ComputeIo final : IoSource {
  uint32_t local[3], global[3], wg[3], num[3], index;
  uint32_t get(int32_t builtin, int32_t, uint32_t out[4]) override {
    switch (builtin) {
      case shader::kBvLocalInvocationId: for (int i = 0; i < 3; ++i) out[i] = local[i]; return 3;
      case shader::kBvGlobalInvocationId: for (int i = 0; i < 3; ++i) out[i] = global[i]; return 3;
      case shader::kBvWorkgroupId: for (int i = 0; i < 3; ++i) out[i] = wg[i]; return 3;
      case shader::kBvNumWorkgroups: for (int i = 0; i < 3; ++i) out[i] = num[i]; return 3;
      case shader::kBvLocalInvocationIndex: out[0] = index; return 1;
      default: return 0;
    }
  }
};

}  // namespace

bool Device::dispatch(uint32_t x, uint32_t y, uint32_t z) {
  using shader::Machine;
  if (pass_ != 2) return fail("dispatch outside a compute pass");
  if (!cur_pipeline_ || cur_pipeline_ > cps_.count()) return fail("no compute pipeline set");
  ComputePipeline& cp = *cps_[cur_pipeline_ - 1];
  shader::Module& m = *cp.module;
  const shader::Function& fn = *m.functions[static_cast<uint64_t>(cp.fn)];
  if (x == 0 || y == 0 || z == 0) return true;
  const uint32_t wx = fn.wg_x, wy = fn.wg_y, wz = fn.wg_z;
  const uint64_t invocations = uint64_t{wx} * wy * wz;
  if (invocations == 0 || invocations > 256) return fail("workgroup too large");
  bound_.clear();
  List<uint32_t> region(a_);
  if (!setup_stage(m, &region, nullptr)) return false;
  // workgroup variables are zeroed for every workgroup
  List<UniquePointer<Machine>> machines(a_);
  for (uint64_t i = 0; i < invocations; ++i) {
    UniquePointer<Machine> mc = UniquePointer<Machine>::make(a_, m, a_, 4096);
    if (!mc || !mc->valid()) return fail("out of memory");
    mc->set_shared(region.view().data());
    mc->set_texture_host(this);
    machines.push_back(static_cast<UniquePointer<Machine>&&>(mc));
  }
  // constants of the module (module-scope `const`) in the shared region are evaluated by each machine's reset_privates() (they are private)
  List<uint8_t> state(a_);
  bool ok = true;
  const uint64_t groups_total = uint64_t{x} * y * z;
  if (groups_total > (1u << 20)) return fail("too many workgroups");
  for (uint32_t gz = 0; gz < z && ok; ++gz)
    for (uint32_t gy = 0; gy < y && ok; ++gy)
      for (uint32_t gx = 0; gx < x && ok; ++gx) {
        uint32_t* base = region.view().data();
        for (uint64_t gi = 0; gi < m.globals.count(); ++gi) {
          const shader::Global& g = *m.globals[gi];
          if (g.kind == GlobalKind::Workgroup)
            for (uint32_t k = 0; k < m.slots_of(g.type); ++k) base[g.slot + k] = 0;
        }
        state.clear();
        List<UniquePointer<EntryArgs>> args(a_);
        for (uint64_t i = 0; i < invocations; ++i) {
          ComputeIo io;
          const uint32_t lx = static_cast<uint32_t>(i % wx), ly = static_cast<uint32_t>((i / wx) % wy), lz = static_cast<uint32_t>(i / (uint64_t{wx} * wy));
          io.local[0] = lx; io.local[1] = ly; io.local[2] = lz;
          io.wg[0] = gx; io.wg[1] = gy; io.wg[2] = gz;
          io.num[0] = x; io.num[1] = y; io.num[2] = z;
          io.global[0] = gx * wx + lx; io.global[1] = gy * wy + ly; io.global[2] = gz * wz + lz;
          io.index = static_cast<uint32_t>(i);
          UniquePointer<EntryArgs> ea = UniquePointer<EntryArgs>::make(a_, a_);
          if (!ea || !build_args(m, fn, io, ea.get())) return fail("out of memory");
          Machine& mc = *machines[i];
          if (!mc.reset_privates()) return fail(mc.failure() ? mc.failure() : "shader initialisation failed");
          const Machine::Status st = mc.start(cp.fn, ArrayView<const uint32_t* const>(ea->ptrs.view().data(), ea->ptrs.count()));
          state.push_back(static_cast<uint8_t>(st));
          if (st == Machine::Status::Failed) { ok = false; fail(mc.failure() ? mc.failure() : "shader failed"); break; }
          args.push_back(static_cast<UniquePointer<EntryArgs>&&>(ea));
        }
        // invocations that stopped at a barrier go on together
        for (uint32_t rounds = 0; ok && rounds < 100000; ++rounds) {
          bool any = false;
          for (uint64_t i = 0; i < invocations; ++i) {
            if (state[i] != static_cast<uint8_t>(Machine::Status::Yield)) continue;
            any = true;
            const Machine::Status st = machines[i]->resume();
            state[i] = static_cast<uint8_t>(st);
            if (st == Machine::Status::Failed) { ok = false; fail(machines[i]->failure() ? machines[i]->failure() : "shader failed"); break; }
          }
          if (!any) break;
        }
      }
  if (!ok) return false;
  return write_back(m, region);
}

// --- Render passes ---------------------------------------------------------------------------------------------------------------------------------

struct Device::Target {
  Image* color[8] = {};
  uint32_t layer[8] = {};
  const FormatDesc* cfmt[8] = {};
  uint32_t count = 0;
  Image* ds = nullptr;
  uint32_t ds_layer = 0;
  const FormatDesc* dfmt = nullptr;
  uint32_t w = 0, h = 0;
  uint32_t ss = 1;
};

namespace {
bool view_image_for_pass(Device& d, const TextureView* v, Image** img, uint32_t* layer, const FormatDesc** fmt) {
  const Texture* t = d.texture(v->texture);
  if (!t || t->destroyed || v->base_mip >= t->mips.count()) return false;
  Texture* tm = d.texture_mut(v->texture);
  *img = tm->mips[v->base_mip].get();
  *layer = v->base_layer;
  *fmt = t->format;
  return true;
}
}  // namespace

void decode_vertex_attribute(uint8_t fmt, const uint8_t* p, uint32_t lanes[4], bool* ints) { decode_vertex(fmt, p, lanes, ints); }
uint32_t vertex_attribute_bytes(uint8_t fmt) { return vertex_size(fmt); }

bool Device::begin_render_pass(const RenderPassDesc& d) {
  if (pass_) return fail("a pass is already open");
  UniquePointer<Target> unused;
  pass_storage_ = UniquePointer<RenderPassDesc>::make(a_, a_);
  if (!pass_storage_) return fail("out of memory");
  for (const ColorAttachment& c : d.colors.view()) pass_storage_->colors.push_back(c);
  pass_storage_->depth_view = d.depth_view; pass_storage_->depth_load = d.depth_load; pass_storage_->depth_store = d.depth_store; pass_storage_->depth_read_only = d.depth_read_only;
  pass_storage_->depth_clear = d.depth_clear; pass_storage_->stencil_load = d.stencil_load; pass_storage_->stencil_store = d.stencil_store;
  pass_storage_->stencil_read_only = d.stencil_read_only; pass_storage_->stencil_clear = d.stencil_clear; pass_storage_->occlusion_set = d.occlusion_set;
  pass_desc_ = pass_storage_.get();
  pass_ = 1;
  cur_pipeline_ = 0;
  for (auto& g : bind_groups_) g = 0;
  for (auto& v : vbuf_) v = VBind();
  ibuf_ = VBind();
  have_viewport_ = false;
  have_scissor_ = false;
  stencil_ref_ = 0;
  for (int i = 0; i < 4; ++i) blend_constant_[i] = 0;
  occlusion_index_ = 0xFFFFFFFFu;
  // clears (the load operations that are not "load")
  for (const ColorAttachment& c : d.colors.view()) {
    const TextureView* v = view(c.view);
    Image* im;
    uint32_t layer;
    const FormatDesc* fd;
    if (!v || !view_image_for_pass(*this, v, &im, &layer, &fd)) return fail("a colour attachment is gone");
    if (c.load) continue;
    const webgl::FormatInfo* fi = im->info;
    for (uint32_t y = 0; y < im->h; ++y)
      for (uint32_t x = 0; x < im->w; ++x) {
        uint32_t w4[4];
        for (int k = 0; k < 4; ++k) {
          switch (fi->store) {
            case webgl::Store::U8x4: { float vv = c.clear[k]; if (fi->srgb && k < 3) vv = webgl::linear_to_srgb(vv); w4[k] = to_u8(vv); break; }
            case webgl::Store::F32x4: w4[k] = f2u(c.clear[k]); break;
            default: w4[k] = static_cast<uint32_t>(static_cast<int32_t>(c.clear[k])); break;
          }
        }
        if (fi->base == 0x1907 || fi->base == 0x8227 || fi->base == 0x1903) w4[3] = fi->store == webgl::Store::U8x4 ? 255 : f2u(1.0f);
        webgl::write_texel(*im, x, y, layer, w4);
      }
  }
  if (d.depth_view) {
    const TextureView* v = view(d.depth_view);
    Image* im;
    uint32_t layer;
    const FormatDesc* fd;
    if (v && view_image_for_pass(*this, v, &im, &layer, &fd)) {
      for (uint32_t y = 0; y < im->h; ++y)
        for (uint32_t x = 0; x < im->w; ++x) {
          uint32_t raw[4];
          webgl::read_texel(*im, x, y, layer, raw);
          if (fd->depth && !d.depth_load && !d.depth_read_only) raw[0] = f2u(clamp01(d.depth_clear));
          if (fd->stencil && !d.stencil_load && !d.stencil_read_only) raw[1] = d.stencil_clear & 255;
          webgl::write_texel(*im, x, y, layer, raw);
        }
    }
  }
  hw_begin(d);
  return true;
}

// The attachments are resolved when a draw happens (the views are stable); the clears happen here, so begin one step later.
bool Device::end_render_pass() {
  hw_end();
  pass_ = 0;
  pass_desc_ = nullptr;
  // resolve targets: copy the (single-sampled) attachment over
  if (pass_storage_) {
    for (const ColorAttachment& c : pass_storage_->colors.view()) {
      if (!c.resolve) continue;
      const TextureView *sv = view(c.view), *rv = view(c.resolve);
      Image *si, *ri;
      uint32_t sl, rl;
      const FormatDesc *sf, *rf;
      if (!sv || !rv || !view_image_for_pass(*this, sv, &si, &sl, &sf) || !view_image_for_pass(*this, rv, &ri, &rl, &rf)) continue;
      if (si->ss > 1 && ri->ss == 1) {
        Image tmp(a_);
        if (!webgl::resolve_samples(*si, sl, &tmp)) continue;
        for (uint32_t y = 0; y < tmp.h && y < ri->h; ++y)
          for (uint32_t x = 0; x < tmp.w && x < ri->w; ++x) {
            uint32_t raw[4];
            webgl::read_texel(tmp, x, y, 0, raw);
            webgl::write_texel(*ri, x, y, rl, raw);
          }
        continue;
      }
      for (uint32_t y = 0; y < si->h && y < ri->h; ++y)
        for (uint32_t x = 0; x < si->w && x < ri->w; ++x) {
          uint32_t raw[4];
          webgl::read_texel(*si, x, y, sl, raw);
          webgl::write_texel(*ri, x, y, rl, raw);
        }
    }
    pass_storage_.reset();
  }
  return true;
}

void Device::set_viewport(float x, float y, float w, float h, float mn, float mx) {
  viewport_[0] = x; viewport_[1] = y; viewport_[2] = w; viewport_[3] = h; viewport_[4] = mn; viewport_[5] = mx;
  have_viewport_ = true;
}
void Device::set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  scissor_[0] = x; scissor_[1] = y; scissor_[2] = w; scissor_[3] = h;
  have_scissor_ = true;
}
bool Device::set_vertex_buffer(uint32_t slot, uint32_t buffer, uint64_t offset, uint64_t size) {
  if (slot >= 16) return fail("vertex buffer slot out of range");
  vbuf_[slot].buffer = buffer; vbuf_[slot].offset = offset; vbuf_[slot].size = size;
  return true;
}
bool Device::set_index_buffer(uint32_t buffer, uint8_t format, uint64_t offset, uint64_t size) {
  ibuf_.buffer = buffer; ibuf_.offset = offset; ibuf_.size = size;
  ibuf_format_ = format;
  return true;
}
bool Device::begin_occlusion(uint32_t index) { occlusion_index_ = index; occlusion_count_ = 0; return true; }
bool Device::end_occlusion() {
  if (pass_desc_ || pass_storage_) {
    const uint32_t qs = pass_storage_ ? pass_storage_->occlusion_set : 0;
    if (qs && qs <= query_sets_.count() && occlusion_index_ < query_sets_[qs - 1]->results.count()) query_sets_[qs - 1]->results[occlusion_index_] = occlusion_count_;
  }
  occlusion_index_ = 0xFFFFFFFFu;
  return true;
}
bool Device::write_timestamp(uint32_t set, uint32_t index) {
  if (!set || set > query_sets_.count() || index >= query_sets_[set - 1]->results.count()) return fail("bad query");
  clock_ += 1000;
  query_sets_[set - 1]->results[index] = clock_;
  return true;
}

struct RenderSink final : gpu::IFragmentSink {
  Device& d;
  RenderPipeline& rp;
  Device::Target& t;
  shader::Machine& fm;
  const shader::Module& mod;
  const shader::Function& fn;
  shader::DerivativeLog logx, logy;
  bool late, deriv;
  uint64_t samples = 0;
  EntryArgs args;

  RenderSink(Device& dev, RenderPipeline& pipe, Device::Target& target, shader::Machine& m, bool derivs)
      : d(dev), rp(pipe), t(target), fm(m), mod(m.module()), fn(*m.module().functions[static_cast<uint64_t>(pipe.fs_fn)]), logx(dev.a_), logy(dev.a_), late(false), deriv(derivs),
        args(dev.a_) {
    late = mod.uses_discard;
    for (uint64_t i = 0; i < fn.params.count(); ++i) {
      (void)i;
    }
    // does the shader write frag_depth?
    const shader::StructDef* sd = mod.struct_of(fn.ret);
    if (fn.ret_builtin == shader::kBvFragDepth) late = true;
    if (sd)
      for (uint64_t i = 0; i < sd->fields.count(); ++i)
        if (sd->fields[i].builtin == shader::kBvFragDepth) late = true;
  }

  struct FragIo final : IoSource {
    const gpu::Fragment* f = nullptr;
    const float* var = nullptr;
    int32_t px = 0, py = 0;
    float ss = 1;
    uint32_t get(int32_t builtin, int32_t location, uint32_t out[4]) override {
      switch (builtin) {
        case shader::kBvPosition: out[0] = f2u((px + 0.5f) / ss); out[1] = f2u((py + 0.5f) / ss); out[2] = f2u(f->z); out[3] = f2u(f->coord[3]); return 4;
        case shader::kBvFrontFacing: out[0] = f->front ? 1 : 0; return 1;
        case shader::kBvSampleIndex: out[0] = 0; return 1;
        case shader::kBvSampleMask: out[0] = 0xFFFFFFFFu; return 1;
        default: break;
      }
      if (location >= 0 && location < 16) {
        for (int k = 0; k < 4; ++k) out[k] = f2u(var[location * 4 + k]);
        return 4;
      }
      return 0;
    }
  };

  shader::Machine::Status run_at(const gpu::Fragment& f, int32_t px, int32_t py, bool same) {
    float tmp[gpu::kMaxVarying];
    if (same) for (uint32_t i = 0; i < gpu::kMaxVarying; ++i) tmp[i] = f.varyings[i];
    else f.varyings_at(px, py, tmp, gpu::kMaxVarying);
    FragIo io;
    io.f = &f; io.var = tmp; io.px = px; io.py = py; io.ss = static_cast<float>(t.ss);
    args.lanes.clear();
    args.ptrs.clear();
    if (!build_args(mod, fn, io, &args)) return shader::Machine::Status::Failed;
    if (!fm.reset_privates()) return shader::Machine::Status::Failed;
    return fm.start(rp.fs_fn, ArrayView<const uint32_t* const>(args.ptrs.view().data(), args.ptrs.count()));
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

  bool ds_test(const gpu::Fragment& f, uint32_t x, uint32_t y, float z) {
    if (!rp.has_depth || !t.ds) return true;
    const DepthStencilState& s = rp.ds;
    const bool stencil_on = t.dfmt->stencil && (s.front.compare != 7 || s.back.compare != 7 || s.front.fail || s.front.depth_fail || s.front.pass || s.back.fail || s.back.depth_fail || s.back.pass);
    uint32_t raw[4];
    webgl::read_texel(*t.ds, x, y, t.ds_layer, raw);
    bool dpass = true;
    if (t.dfmt->depth) dpass = compare(s.depth_compare, z, u2f(raw[0]));
    if (stencil_on) {
      const StencilFace& sf = f.front ? s.front : s.back;
      const uint32_t cur = raw[1] & 255, ref = d.stencil_ref_ & 255, mask = s.stencil_read_mask & 255;
      const bool spass = compare(sf.compare, static_cast<float>(ref & mask), static_cast<float>(cur & mask));
      const uint8_t op = spass ? (dpass ? sf.pass : sf.depth_fail) : sf.fail;
      const uint32_t nv = stencil_op(op, cur, ref), wm = s.stencil_write_mask & 255;
      raw[1] = (cur & ~wm) | (nv & wm);
      if (!spass) { webgl::write_texel(*t.ds, x, y, t.ds_layer, raw); return false; }
    }
    if (!dpass) { if (stencil_on) webgl::write_texel(*t.ds, x, y, t.ds_layer, raw); return false; }
    if (t.dfmt->depth && s.depth_write) raw[0] = f2u(z);
    webgl::write_texel(*t.ds, x, y, t.ds_layer, raw);
    return true;
  }

  void fragment(const gpu::Fragment& f) override {
    const uint32_t x = static_cast<uint32_t>(f.x), y = static_cast<uint32_t>(f.y);
    if (x >= t.w || y >= t.h) return;
    float z = f.z;
    if (!late && !ds_test(f, x, y, z)) return;
    if (!shade(f)) return;
    const uint32_t* pr = fm.result();
    const shader::Type rt = fm.result_type();
    if (late) {
      const shader::StructDef* sd = mod.struct_of(rt);
      if (sd)
        for (uint64_t i = 0; i < sd->fields.count(); ++i)
          if (sd->fields[i].builtin == shader::kBvFragDepth) z = clamp01(u2f(pr[sd->fields[i].offset]));
      if (fn.ret_builtin == shader::kBvFragDepth) z = clamp01(u2f(pr[0]));
      if (!ds_test(f, x, y, z)) return;
    }
    ++samples;
    if (d.occlusion_index_ != 0xFFFFFFFFu) ++d.occlusion_count_;
    write_targets(pr, rt, x, y);
  }

  void write_targets(const uint32_t* pr, const shader::Type& rt, uint32_t x, uint32_t y) {
    const shader::StructDef* sd = mod.struct_of(rt);
    for (uint32_t ti = 0; ti < t.count && ti < 8; ++ti) {
      if (!t.color[ti]) continue;
      // find the output for this target
      const uint32_t* src = nullptr;
      uint32_t comps = 4;
      shader::Base sb = Base::Float;
      if (sd) {
        for (uint64_t i = 0; i < sd->fields.count(); ++i)
          if (sd->fields[i].location == static_cast<int32_t>(ti)) { src = pr + sd->fields[i].offset; comps = sd->fields[i].type.rows; sb = sd->fields[i].type.base; }
      } else if (fn.ret_location == static_cast<int32_t>(ti) || (fn.ret_location < 0 && ti == 0 && fn.ret_builtin == shader::kBvNone)) {
        src = pr; comps = rt.rows; sb = rt.base;
      }
      if (!src) continue;
      const ColorTarget* ct = ti < rp.targets.count() ? &rp.targets[ti] : nullptr;
      Image& img = *t.color[ti];
      const webgl::FormatInfo* fi = img.info;
      uint32_t lanes[4];
      for (uint32_t k = 0; k < 4; ++k) lanes[k] = k < comps ? src[k] : (k == 3 ? (sb == Base::Float ? f2u(1.0f) : 1u) : 0u);
      const uint8_t mask = ct ? ct->write_mask : 15;
      uint32_t raw[4];
      webgl::read_texel(img, x, y, t.layer[ti], raw);
      if (fi->store == webgl::Store::I32x4 || fi->store == webgl::Store::U32x4) {
        uint32_t w4[4];
        for (int k = 0; k < 4; ++k) w4[k] = (mask & (1 << k)) ? lanes[k] : raw[k];
        webgl::write_texel(img, x, y, t.layer[ti], w4);
        continue;
      }
      float src_f[4], dst_f[4];
      for (int k = 0; k < 4; ++k) {
        src_f[k] = sb == Base::Float ? u2f(lanes[k]) : static_cast<float>(static_cast<int32_t>(lanes[k]));
        if (fi->store == webgl::Store::U8x4) { dst_f[k] = raw[k] / 255.0f; if (fi->srgb && k < 3) dst_f[k] = webgl::srgb_to_linear(dst_f[k]); }
        else dst_f[k] = u2f(raw[k]);
      }
      float res[4] = {src_f[0], src_f[1], src_f[2], src_f[3]};
      if (ct && ct->blend) {
        if (fi->store == webgl::Store::U8x4) for (int k = 0; k < 4; ++k) src_f[k] = clamp01(src_f[k]);
        for (int k = 0; k < 3; ++k) {
          const float sf = factor(ct->color.src, src_f, dst_f, d.blend_constant_, k, false), df = factor(ct->color.dst, src_f, dst_f, d.blend_constant_, k, false);
          res[k] = apply_op(ct->color.op, src_f[k], dst_f[k], sf, df);
        }
        const float sfa = factor(ct->alpha.src, src_f, dst_f, d.blend_constant_, 3, true), dfa = factor(ct->alpha.dst, src_f, dst_f, d.blend_constant_, 3, true);
        res[3] = apply_op(ct->alpha.op, src_f[3], dst_f[3], sfa, dfa);
      }
      uint32_t w4[4];
      for (int k = 0; k < 4; ++k) {
        if (!(mask & (1 << k))) { w4[k] = raw[k]; continue; }
        if (fi->store == webgl::Store::U8x4) {
          float v = res[k];
          if (fi->srgb && k < 3) v = webgl::linear_to_srgb(v);
          w4[k] = to_u8(v);
        } else {
          w4[k] = f2u(res[k]);
        }
      }
      if (fi->base == 0x1907 || fi->base == 0x8227 || fi->base == 0x1903) { if (fi->store == webgl::Store::U8x4) w4[3] = 255; else w4[3] = f2u(1.0f); }
      webgl::write_texel(img, x, y, t.layer[ti], w4);
    }
  }
};

namespace {
struct VertexIo final : IoSource {
  const uint32_t (*attrs)[4];       // by location
  const bool* present;
  uint32_t vertex_index, instance_index;
  uint32_t get(int32_t builtin, int32_t location, uint32_t out[4]) override {
    if (builtin == shader::kBvVertexIndex) { out[0] = vertex_index; return 1; }
    if (builtin == shader::kBvInstanceIndex) { out[0] = instance_index; return 1; }
    if (location >= 0 && location < 16 && present[location]) {
      for (int k = 0; k < 4; ++k) out[k] = attrs[location][k];
      return 4;
    }
    return 0;
  }
};
}  // namespace

bool Device::draw_primitives(uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, bool indexed, uint32_t) {
  using shader::Machine;
  if (pass_ != 1 || !pass_storage_) return fail("draw outside a render pass");
  if (!cur_pipeline_ || cur_pipeline_ > rps_.count()) return fail("no render pipeline set");
  RenderPipeline& rp = *rps_[cur_pipeline_ - 1];
  if (hw_pass_) {
    if (hw_draw(rp, count, first, base_vertex, instances, first_instance, indexed)) return true;
    hw_sync();   // the hardware cannot take this draw: the texture gets the picture so far and the rest of the pass is the CPU's
    hw_pass_ = false;
  }
  // resolve the attachments
  Target tg;
  const RenderPassDesc& pd = *pass_storage_;
  for (uint32_t i = 0; i < pd.colors.count() && i < 8; ++i) {
    const TextureView* v = view(pd.colors[i].view);
    if (!v) continue;
    Image* im;
    if (!view_image_for_pass(*this, v, &im, &tg.layer[i], &tg.cfmt[i])) return fail("an attachment is gone");
    tg.color[i] = im;
    tg.count = i + 1;
    tg.w = im->w; tg.h = im->h; tg.ss = im->ss;
  }
  if (pd.depth_view) {
    const TextureView* v = view(pd.depth_view);
    if (v && view_image_for_pass(*this, v, &tg.ds, &tg.ds_layer, &tg.dfmt)) { if (!tg.w) { tg.w = tg.ds->w; tg.h = tg.ds->h; tg.ss = tg.ds->ss; } }
  }
  if (!tg.w) return fail("a render pass needs an attachment");
  // pipeline state against the pass
  RenderSink* sinkp = nullptr;
  (void)sinkp;
  bound_.clear();
  List<uint32_t> vregion(a_), fregion(a_);
  if (!setup_stage(*rp.vs, &vregion, nullptr)) return false;
  if (rp.fs && !setup_stage(*rp.fs, &fregion, nullptr)) return false;
  Machine vm(*rp.vs, a_);
  if (!vm.valid()) return fail("out of memory");
  vm.set_shared(vregion.view().data());
  vm.set_texture_host(this);
  UniquePointer<Machine> fm;
  if (rp.fs) {
    fm = UniquePointer<Machine>::make(a_, *rp.fs, a_);
    if (!fm || !fm->valid()) return fail("out of memory");
    fm->set_shared(fregion.view().data());
    fm->set_texture_host(this);
  }
  const shader::Function& vfn = *rp.vs->functions[static_cast<uint64_t>(rp.vs_fn)];

  gpu::RasterState rs;
  const float vx = have_viewport_ ? viewport_[0] : 0, vy = have_viewport_ ? viewport_[1] : 0, vw = have_viewport_ ? viewport_[2] : static_cast<float>(tg.w / tg.ss),
              vh = have_viewport_ ? viewport_[3] : static_cast<float>(tg.h / tg.ss);
  const int32_t kss = static_cast<int32_t>(tg.ss);
  rs.vp_x = static_cast<int32_t>(vx) * kss; rs.vp_y = static_cast<int32_t>(vy) * kss; rs.vp_w = static_cast<int32_t>(vw) * kss; rs.vp_h = static_cast<int32_t>(vh) * kss;
  rs.depth_near = have_viewport_ ? viewport_[4] : 0; rs.depth_far = have_viewport_ ? viewport_[5] : 1;
  rs.sc_x = have_scissor_ ? static_cast<int32_t>(scissor_[0]) * kss : 0; rs.sc_y = have_scissor_ ? static_cast<int32_t>(scissor_[1]) * kss : 0;
  rs.sc_w = have_scissor_ ? static_cast<int32_t>(scissor_[2]) * kss : static_cast<int32_t>(tg.w); rs.sc_h = have_scissor_ ? static_cast<int32_t>(scissor_[3]) * kss : static_cast<int32_t>(tg.h);
  rs.target_w = static_cast<int32_t>(tg.w); rs.target_h = static_cast<int32_t>(tg.h);
  // y is flipped into window space (row 0 is the top), which mirrors the winding
  rs.front_ccw = !rp.front_ccw;
  rs.cull = rp.cull == 1 ? 1 : (rp.cull == 2 ? 2 : 0);
  rs.clip_z_zero_to_one = true;
  rs.depth_clip = !rp.unclipped_depth;
  rs.flat_first = true;
  rs.offset = rp.has_depth && (rp.ds.bias != 0 || rp.ds.bias_slope != 0);
  rs.offset_factor = rp.ds.bias_slope;
  rs.offset_units = static_cast<float>(rp.ds.bias);
  rs.nvar = 64;
  // flat flags come from the vertex outputs
  const shader::StructDef* vout = rp.vs->struct_of(vfn.ret);
  if (vout)
    for (uint64_t i = 0; i < vout->fields.count(); ++i)
      if (vout->fields[i].location >= 0 && vout->fields[i].location < 16 && vout->fields[i].flat)
        for (int k = 0; k < 4; ++k) rs.flat[vout->fields[i].location * 4 + k] = true;
  // integer varyings are always flat
  if (vout)
    for (uint64_t i = 0; i < vout->fields.count(); ++i)
      if (vout->fields[i].location >= 0 && vout->fields[i].location < 16 && vout->fields[i].type.base != Base::Float)
        for (int k = 0; k < 4; ++k) rs.flat[vout->fields[i].location * 4 + k] = true;

  const bool deriv = rp.fs && (rp.fs->uses_derivatives || rp.fs->uses_implicit_lod);
  UniquePointer<RenderSink> sink;
  if (rp.fs) sink = UniquePointer<RenderSink>::make(a_, *this, rp, tg, *fm, deriv);
  // a pipeline without a fragment shader still writes depth
  struct DepthOnly final : gpu::IFragmentSink {
    Device& dev; RenderPipeline& p; Target& tgt; uint64_t n = 0;
    DepthOnly(Device& d, RenderPipeline& pp, Target& tt) : dev(d), p(pp), tgt(tt) {}
    void fragment(const gpu::Fragment& f) override {
      if (!p.has_depth || !tgt.ds) { ++n; return; }
      uint32_t raw[4];
      const uint32_t x = static_cast<uint32_t>(f.x), y = static_cast<uint32_t>(f.y);
      webgl::read_texel(*tgt.ds, x, y, tgt.ds_layer, raw);
      if (tgt.dfmt->depth && !compare(p.ds.depth_compare, f.z, u2f(raw[0]))) return;
      ++n;
      if (tgt.dfmt->depth && p.ds.depth_write) { raw[0] = f2u(f.z); webgl::write_texel(*tgt.ds, x, y, tgt.ds_layer, raw); }
    }
  } depth_only(*this, rp, tg);
  gpu::IFragmentSink& out = rp.fs ? static_cast<gpu::IFragmentSink&>(*sink) : static_cast<gpu::IFragmentSink&>(depth_only);

  // index buffer
  const uint8_t* idx = nullptr;
  uint32_t index_size = ibuf_format_ == 0 ? 2 : 4;
  if (indexed) {
    const Buffer* ib = buf(ibuf_.buffer);
    if (!ib || ib->destroyed) return fail("no index buffer is set");
    if (ibuf_.offset > ib->data.size()) return fail("index buffer range");
    idx = ib->data.view().data() + ibuf_.offset;
    if ((first + count) * uint64_t{index_size} > ib->data.size() - ibuf_.offset) return fail("indices out of the buffer");
  }
  const uint32_t restart = ibuf_format_ == 0 ? 0xFFFF : 0xFFFFFFFFu;
  const bool strip = rp.topology == 2 || rp.topology == 4;

  for (uint32_t inst = 0; inst < instances; ++inst) {
    auto vertex_at = [&](uint32_t i, gpu::RVertex* v, bool* is_restart) -> bool {
      uint32_t vi = first + i;
      *is_restart = false;
      if (indexed) {
        uint32_t raw = 0;
        if (index_size == 2) { uint16_t r; __builtin_memcpy(&r, idx + (first + i) * 2u, 2); raw = r; }
        else __builtin_memcpy(&raw, idx + (first + i) * 4u, 4);
        if (strip && raw == restart) { *is_restart = true; return true; }
        vi = static_cast<uint32_t>(static_cast<int64_t>(raw) + base_vertex);
      }
      uint32_t attrs[16][4] = {};
      bool present[16] = {};
      for (uint64_t bi = 0; bi < rp.buffers.count(); ++bi) {
        const VertexBufferLayout& layout = *rp.buffers[bi];
        const VBind& vb = vbuf_[bi];
        const Buffer* b = buf(vb.buffer);
        if (!b || b->destroyed) continue;
        const uint64_t el = layout.instance ? first_instance + inst : vi;
        for (const VertexAttribute& at : layout.attributes.view()) {
          if (at.shader_location >= 16) continue;
          const uint64_t pos = vb.offset + el * layout.stride + at.offset;
          if (pos + vertex_size(at.format) > b->data.size()) { for (int k = 0; k < 4; ++k) attrs[at.shader_location][k] = 0; present[at.shader_location] = true; continue; }
          bool ints;
          decode_vertex(at.format, b->data.view().data() + pos, attrs[at.shader_location], &ints);
          present[at.shader_location] = true;
        }
      }
      VertexIo io;
      io.attrs = attrs; io.present = present; io.vertex_index = vi; io.instance_index = first_instance + inst;
      EntryArgs ea(a_);
      if (!build_args(*rp.vs, vfn, io, &ea)) return false;
      if (!vm.reset_privates()) return false;
      if (vm.start(rp.vs_fn, ArrayView<const uint32_t* const>(ea.ptrs.view().data(), ea.ptrs.count())) != Machine::Status::Done) return false;
      const uint32_t* r = vm.result();
      for (uint32_t k = 0; k < gpu::kMaxVarying; ++k) v->var[k] = 0;
      v->point_size = 1;
      if (vout) {
        for (uint64_t fi = 0; fi < vout->fields.count(); ++fi) {
          const shader::Field& f = vout->fields[fi];
          if (f.builtin == shader::kBvPosition) { v->pos[0] = u2f(r[f.offset]); v->pos[1] = -u2f(r[f.offset + 1]); v->pos[2] = u2f(r[f.offset + 2]); v->pos[3] = u2f(r[f.offset + 3]); }
          else if (f.location >= 0 && f.location < 16) for (uint32_t k = 0; k < rp.vs->slots_of(f.type) && k < 4; ++k) v->var[f.location * 4 + k] = u2f(r[f.offset + k]);
        }
      } else if (vfn.ret_builtin == shader::kBvPosition) {
        v->pos[0] = u2f(r[0]); v->pos[1] = -u2f(r[1]); v->pos[2] = u2f(r[2]); v->pos[3] = u2f(r[3]);
      }
      return true;
    };
    gpu::RVertex va, vb2, vc;
    bool ra, rb, rc;
    bool ok = true;
    auto tri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
      if (!vertex_at(i0, &va, &ra) || !vertex_at(i1, &vb2, &rb) || !vertex_at(i2, &vc, &rc)) { ok = false; return; }
      if (ra || rb || rc) return;
      gpu::draw_triangle(rs, va, vb2, vc, out);
    };
    switch (rp.topology) {
      case 0:
        for (uint32_t i = 0; i < count && ok; ++i) { if (!vertex_at(i, &va, &ra)) { ok = false; break; } if (!ra) gpu::draw_point(rs, va, out); }
        break;
      case 1:
        for (uint32_t i = 0; i + 1 < count && ok; i += 2) {
          if (!vertex_at(i, &va, &ra) || !vertex_at(i + 1, &vb2, &rb)) { ok = false; break; }
          if (!ra && !rb) gpu::draw_line(rs, va, vb2, out);
        }
        break;
      case 2: {
        uint32_t start = 0;
        for (uint32_t i = 0; i <= count && ok; ++i) {
          bool restart_here = i == count;
          if (!restart_here) { if (!vertex_at(i, &va, &ra)) { ok = false; break; } restart_here = ra; }
          if (restart_here) { start = i + 1; continue; }
          if (i > start) {
            if (!vertex_at(i - 1, &va, &ra) || !vertex_at(i, &vb2, &rb)) { ok = false; break; }
            gpu::draw_line(rs, va, vb2, out);
          }
        }
        break;
      }
      case 3:
        for (uint32_t i = 0; i + 2 < count && ok; i += 3) tri(i, i + 1, i + 2);
        break;
      default: {
        uint32_t start = 0;
        for (uint32_t i = 0; i <= count && ok; ++i) {
          bool restart_here = i == count;
          if (!restart_here) { gpu::RVertex tmp; bool r; if (!vertex_at(i, &tmp, &r)) { ok = false; break; } restart_here = r; }
          if (restart_here) { start = i + 1; continue; }
          if (i >= start + 2) {
            const uint32_t k = i - start - 2;
            if (k & 1) tri(i - 1, i - 2, i);
            else tri(i - 2, i - 1, i);
          }
        }
        break;
      }
    }
    if (!ok) return fail(vm.failure() ? vm.failure() : "vertex shader failed");
  }
  if (rp.fs && !write_back(*rp.fs, fregion)) return false;
  return write_back(*rp.vs, vregion);
}

bool Device::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
  return draw_primitives(vertex_count, first_vertex, 0, instance_count, first_instance, false, 0);
}
bool Device::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t base_vertex, uint32_t first_instance) {
  return draw_primitives(index_count, first_index, base_vertex, instance_count, first_instance, true, 0);
}

}  // namespace wgpu
