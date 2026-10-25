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

// The natives under navigator.gpu (the script side is kWebGpuPrelude in WebGpuPrelude.cpp):
//   __gpu_create() -> id of a new device
//   __gpu_ops() -> the names of the operations; __gpu(id, op, ...arguments) runs one
//   __gpu_present(id, texture, canvas, premultiplied) copies a texture into a canvas's bitmap
// Descriptors reach the natives as plain numbers, strings and arrays (the script side translates the WebGPU enumerations), and command lists as arrays of arrays.
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Objects.hpp"
#include "Script/TypedArray.hpp"
#include "WebGl/GpuDevice.hpp"

namespace browser {
namespace bind {

struct GpuState {
  explicit GpuState(allocators::IAllocator& a) : devices(a), staging(a) {}
  List<UniquePointer<wgpu::Device>> devices;
  List<uint32_t> staging;
};

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

PageObjects& objs(Vm& vm) { return impl(vm).host.objects(); }

wgpu::Device* device_of(Vm& vm, double id) {
  GpuState* s = impl(vm).gpu.get();
  if (!s || id < 1 || id > static_cast<double>(s->devices.count())) return nullptr;
  return s->devices[static_cast<uint64_t>(id) - 1].get();
}

struct Args {
  Vm& vm;
  const Value* v;
  uint32_t n;
  Value at(uint32_t i) const { return i < n ? v[i] : Value(); }
  double num(uint32_t i, double def = 0) const {
    const Value x = at(i);
    if (x.is_number()) return x.u.number;
    if (x.is_bool()) return x.u.boolean ? 1 : 0;
    if (x.is_nullish()) return def;
    double d = def;
    vm.to_number(x, &d);
    return d;
  }
  uint32_t u32(uint32_t i) const { const double d = num(i); return d <= 0 ? 0 : (d >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(d)); }
  int32_t i32(uint32_t i) const { const double d = num(i); return d <= -2147483648.0 ? INT32_MIN : (d >= 2147483647.0 ? INT32_MAX : static_cast<int32_t>(d)); }
  uint64_t u64(uint32_t i) const { const double d = num(i); return d <= 0 ? 0 : static_cast<uint64_t>(d); }
  bool flag(uint32_t i) const { const Value x = at(i); return x.is_bool() ? x.u.boolean : num(i) != 0; }
};

// Reads property `name` of an object as a number / bool / string / array.
double prop_num(Vm& vm, const Value& o, const char* name, double def = 0) {
  Value v;
  if (!o.is_object() || !vm.get_named(o, name, &v)) return def;
  if (v.is_number()) return v.u.number;
  if (v.is_bool()) return v.u.boolean ? 1 : 0;
  return def;
}
bool prop_text(Vm& vm, const Value& o, const char* name, String* out) {
  Value v;
  if (!o.is_object() || !vm.get_named(o, name, &v) || v.is_nullish()) return false;
  return arg_text(vm, v, out);
}
JsObject* prop_array(Vm& vm, const Value& o, const char* name) {
  Value v;
  if (!o.is_object() || !vm.get_named(o, name, &v) || !v.is_object()) return nullptr;
  return v.as_object();
}
double item_num(JsObject* a, uint32_t i, double def = 0) {
  if (!a || i >= a->length) return def;
  const Value v = a->items[i];
  if (v.is_number()) return v.u.number;
  if (v.is_bool()) return v.u.boolean ? 1 : 0;
  return def;
}

#define GPU_OPS(X)                                                                                                                                              \
  X(createBuffer) X(destroyBuffer) X(writeBuffer) X(readBuffer) X(createTexture) X(destroyTexture) X(createView) X(createSampler) X(createShaderModule)         \
  X(createBindGroupLayout) X(createPipelineLayout) X(createBindGroup) X(createRenderPipeline) X(createComputePipeline) X(getBindGroupLayout)                    \
  X(createQuerySet) X(submit) X(writeTexture) X(copyExternal) X(readQuery) X(lastError) X(bufferSize) X(detach)

enum Op : uint32_t {
#define X(name) op_##name,
  GPU_OPS(X)
#undef X
  op_count
};
const char* const kOpNames[] = {
#define X(name) #name,
    GPU_OPS(X)
#undef X
};

NATIVE(gpu_ops) {
  (void)self; (void)args; (void)argc;
  JsObject* a = vm.new_array(op_count);
  for (uint32_t i = 0; i < op_count; ++i) vm.array_push(a, str_value(vm, kOpNames[i]));
  *result = Value::make_object(a);
  return true;
}

NATIVE(gpu_create) {
  (void)self; (void)args; (void)argc;
  DomBindings::Impl& c = impl(vm);
  if (!c.gpu) c.gpu = UniquePointer<GpuState>::make(vm.allocator(), vm.allocator());
  if (!c.gpu) { *result = num_value(0); return true; }
  UniquePointer<wgpu::Device> d = UniquePointer<wgpu::Device>::make(vm.allocator(), vm.allocator());
  if (!d) { *result = num_value(0); return true; }
  c.gpu->devices.push_back(static_cast<UniquePointer<wgpu::Device>&&>(d));
  *result = num_value(static_cast<double>(c.gpu->devices.count()));
  return true;
}

NATIVE(gpu_present) {
  (void)self;
  const Args a{vm, args, argc};
  wgpu::Device* d = device_of(vm, a.num(0));
  *result = bool_value(false);
  dom::Node* n = node_of(a.at(2));
  if (!d || !n) return true;
  const wgpu::Texture* t = d->texture(a.u32(1));
  if (!t || t->mips.count() == 0) return true;
  GpuState& st = *impl(vm).gpu;
  const uint32_t w = t->w, h = t->h;
  st.staging.clear();
  if (!st.staging.reserve(uint64_t{w} * h)) return true;
  for (uint64_t i = 0; i < uint64_t{w} * h; ++i) st.staging.push_back(0);
  if (!d->read_canvas(a.u32(1), st.staging.view().data(), w, !a.flag(3), a.flag(3))) return true;
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = objects.canvas(n->serial, w, h);
  if (!bm) return true;
  ImageSource src;
  src.pixels = ArrayView<const uint32_t>(st.staging.view().data(), uint64_t{w} * h);
  src.width = w;
  src.height = h;
  src.pitch = w;
  bm->put_image_data(src, 0, 0, 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h));
  objects.mark_dirty();
  *result = bool_value(true);
  return true;
}

bool pixels_of(Vm& vm, const Value& source, List<uint8_t>* out, uint32_t* w, uint32_t* h) {
  ArrayView<uint8_t> raw;
  if (source.is_object()) {
    Value dv, wv, hv;
    if (vm.get_named(source, "data", &dv) && vm.get_named(source, "width", &wv) && vm.get_named(source, "height", &hv) && wv.is_number() && hv.is_number() &&
        script::typed_array_bytes(dv, &raw)) {
      *w = static_cast<uint32_t>(wv.u.number);
      *h = static_cast<uint32_t>(hv.u.number);
      const uint64_t n = uint64_t{*w} * *h * 4;
      if (raw.size() < n) return false;
      for (uint64_t i = 0; i < n; ++i) out->push_back(raw[i]);
      return true;
    }
  }
  ImageSource src;
  if (!media_source_pixels(vm, source, &src)) return false;
  *w = src.width;
  *h = src.height;
  if (!out->reserve(uint64_t{src.width} * src.height * 4)) return false;
  for (uint32_t y = 0; y < src.height; ++y)
    for (uint32_t x = 0; x < src.width; ++x) {
      const uint32_t p = src.pixels[uint64_t{y} * src.pitch + x];
      out->push_back(static_cast<uint8_t>((p >> 16) & 255));
      out->push_back(static_cast<uint8_t>((p >> 8) & 255));
      out->push_back(static_cast<uint8_t>(p & 255));
      out->push_back(static_cast<uint8_t>(p >> 24));
    }
  return true;
}

void constants_of(Vm& vm, JsObject* list, List<UniquePointer<wgpu::Device::ConstantValue>>* out) {
  if (!list) return;
  for (uint32_t i = 0; i < list->length; ++i) {
    if (!list->items[i].is_object()) continue;
    JsObject* pair = list->items[i].as_object();
    if (pair->length < 2) continue;
    UniquePointer<wgpu::Device::ConstantValue> cv = UniquePointer<wgpu::Device::ConstantValue>::make(vm.allocator(), vm.allocator());
    if (!cv) return;
    arg_text(vm, pair->items[0], &cv->name);
    cv->value = pair->items[1].is_number() ? pair->items[1].u.number : (pair->items[1].is_bool() && pair->items[1].u.boolean ? 1 : 0);
    out->push_back(static_cast<UniquePointer<wgpu::Device::ConstantValue>&&>(cv));
  }
}

// Runs one command list (the arrays an encoder recorded). Returns false with the device's error set.
bool run_command(Vm& vm, wgpu::Device& d, JsObject* cmd) {
  if (!cmd || cmd->length == 0) return true;
  auto N = [&](uint32_t i) { return item_num(cmd, i + 1); };
  auto U = [&](uint32_t i) { return static_cast<uint32_t>(N(i) < 0 ? 0 : N(i)); };
  auto L = [&](uint32_t i) { return static_cast<uint64_t>(N(i) < 0 ? 0 : N(i)); };
  switch (static_cast<uint32_t>(item_num(cmd, 0))) {
    case 1: {   // begin render pass: [1, desc]
      const Value desc = cmd->length > 1 ? cmd->items[1] : Value();
      wgpu::RenderPassDesc pd(vm.allocator());
      if (JsObject* colors = prop_array(vm, desc, "colors")) {
        for (uint32_t i = 0; i < colors->length; ++i) {
          const Value c = colors->items[i];
          wgpu::ColorAttachment ca;
          ca.view = static_cast<uint32_t>(prop_num(vm, c, "view"));
          ca.resolve = static_cast<uint32_t>(prop_num(vm, c, "resolve"));
          ca.load = prop_num(vm, c, "load") != 0;
          ca.store = prop_num(vm, c, "store", 1) != 0;
          if (JsObject* cl = prop_array(vm, c, "clear")) for (uint32_t k = 0; k < 4; ++k) ca.clear[k] = static_cast<float>(item_num(cl, k));
          pd.colors.push_back(ca);
        }
      }
      pd.depth_view = static_cast<uint32_t>(prop_num(vm, desc, "depthView"));
      pd.depth_load = prop_num(vm, desc, "depthLoad") != 0;
      pd.depth_store = prop_num(vm, desc, "depthStore", 1) != 0;
      pd.depth_read_only = prop_num(vm, desc, "depthReadOnly") != 0;
      pd.depth_clear = static_cast<float>(prop_num(vm, desc, "depthClear", 1));
      pd.stencil_load = prop_num(vm, desc, "stencilLoad") != 0;
      pd.stencil_store = prop_num(vm, desc, "stencilStore", 1) != 0;
      pd.stencil_read_only = prop_num(vm, desc, "stencilReadOnly") != 0;
      pd.stencil_clear = static_cast<uint32_t>(prop_num(vm, desc, "stencilClear"));
      pd.occlusion_set = static_cast<uint32_t>(prop_num(vm, desc, "occlusionSet"));
      return d.begin_render_pass(pd);
    }
    case 2: return d.end_render_pass();
    case 3: return d.begin_compute_pass();
    case 4: return d.set_pipeline(U(0));
    case 5: {   // [5, index, group, offsets]
      uint32_t offs[16];
      uint32_t n = 0;
      if (cmd->length > 3 && cmd->items[3].is_object()) {
        JsObject* o = cmd->items[3].as_object();
        for (uint32_t i = 0; i < o->length && n < 16; ++i) offs[n++] = static_cast<uint32_t>(item_num(o, i));
      }
      return d.set_bind_group(U(0), U(1), ArrayView<const uint32_t>(offs, n));
    }
    case 6: return d.set_vertex_buffer(U(0), U(1), L(2), L(3));
    case 7: return d.set_index_buffer(U(0), static_cast<uint8_t>(U(1)), L(2), L(3));
    case 8: return d.draw(U(0), U(1), U(2), U(3));
    case 9: return d.draw_indexed(U(0), U(1), U(2), static_cast<int32_t>(N(3)), U(4));
    case 10: return d.dispatch(U(0), U(1), U(2));
    case 11: d.set_viewport(static_cast<float>(N(0)), static_cast<float>(N(1)), static_cast<float>(N(2)), static_cast<float>(N(3)), static_cast<float>(N(4)), static_cast<float>(N(5))); return true;
    case 12: d.set_scissor(U(0), U(1), U(2), U(3)); return true;
    case 13: { const float c[4] = {static_cast<float>(N(0)), static_cast<float>(N(1)), static_cast<float>(N(2)), static_cast<float>(N(3))}; d.set_blend_constant(c); return true; }
    case 14: d.set_stencil_reference(U(0)); return true;
    case 15: return d.copy_buffer_to_buffer(U(0), L(1), U(2), L(3), L(4));
    case 16: {   // [16, buf, offset, bpr, rpi, tex, mip, x, y, z, aspect, w, h, d]
      wgpu::BufferLayout bl;
      bl.offset = L(1); bl.bytes_per_row = U(2); bl.rows_per_image = U(3);
      wgpu::ImageCopy ic;
      ic.texture = U(4); ic.mip = U(5); ic.x = U(6); ic.y = U(7); ic.z = U(8); ic.aspect = static_cast<uint8_t>(U(9));
      return d.copy_buffer_to_texture(U(0), bl, ic, U(10), U(11), U(12));
    }
    case 17: {   // [17, tex, mip, x, y, z, aspect, buf, offset, bpr, rpi, w, h, d]
      wgpu::ImageCopy ic;
      ic.texture = U(0); ic.mip = U(1); ic.x = U(2); ic.y = U(3); ic.z = U(4); ic.aspect = static_cast<uint8_t>(U(5));
      wgpu::BufferLayout bl;
      bl.offset = L(7); bl.bytes_per_row = U(8); bl.rows_per_image = U(9);
      return d.copy_texture_to_buffer(ic, U(6), bl, U(10), U(11), U(12));
    }
    case 18: {   // [18, stex, smip, sx, sy, sz, saspect, dtex, dmip, dx, dy, dz, daspect, w, h, d]
      wgpu::ImageCopy s, t;
      s.texture = U(0); s.mip = U(1); s.x = U(2); s.y = U(3); s.z = U(4); s.aspect = static_cast<uint8_t>(U(5));
      t.texture = U(6); t.mip = U(7); t.x = U(8); t.y = U(9); t.z = U(10); t.aspect = static_cast<uint8_t>(U(11));
      return d.copy_texture_to_texture(s, t, U(12), U(13), U(14));
    }
    case 19: return d.clear_buffer(U(0), L(1), L(2));
    case 20: return d.begin_occlusion(U(0));
    case 21: return d.end_occlusion();
    case 22: return d.write_timestamp(U(0), U(1));
    case 23: {   // resolveQuerySet [23, set, first, count, buf, offset]
      const wgpu::QuerySet* q = d.query_set(U(0));
      if (!q) return true;
      for (uint32_t i = 0; i < U(2) && U(1) + i < q->results.count(); ++i) {
        const uint64_t v = q->results[U(1) + i];
        uint8_t bytes[8];
        for (int k = 0; k < 8; ++k) bytes[k] = static_cast<uint8_t>(v >> (8 * k));
        if (!d.write_buffer(U(3), L(4) + uint64_t{i} * 8, ConstByteView(bytes, 8))) return false;
      }
      return true;
    }
    case 24: case 25: case 26: {   // indirect: [code, buf, offset]
      uint32_t a[5] = {0, 0, 0, 0, 0};
      uint8_t raw[20] = {};
      const uint32_t words = item_num(cmd, 0) == 25 ? 5 : (item_num(cmd, 0) == 24 ? 4 : 3);
      if (!d.read_buffer(U(0), L(1), ByteView(raw, words * 4))) return false;
      for (uint32_t i = 0; i < words; ++i) a[i] = uint32_t{raw[i * 4]} | (uint32_t{raw[i * 4 + 1]} << 8) | (uint32_t{raw[i * 4 + 2]} << 16) | (uint32_t{raw[i * 4 + 3]} << 24);
      if (item_num(cmd, 0) == 24) return d.draw(a[0], a[1], a[2], a[3]);
      if (item_num(cmd, 0) == 25) return d.draw_indexed(a[0], a[1], a[2], static_cast<int32_t>(a[3]), a[4]);
      return d.dispatch(a[0], a[1], a[2]);
    }
    default: return true;
  }
}

NATIVE(gpu_call) {
  (void)self;
  const Args all{vm, args, argc};
  wgpu::Device* d = device_of(vm, all.num(0));
  *result = Value();
  if (!d || argc < 2) return true;
  const uint32_t op = all.u32(1);
  const Args a{vm, args + 2, argc - 2};
  switch (op) {
    case op_createBuffer: *result = num_value(d->create_buffer(a.u64(0), a.u32(1))); break;
    case op_destroyBuffer: d->destroy_buffer(a.u32(0)); break;
    case op_writeBuffer: {
      ArrayView<uint8_t> raw;
      if (script::typed_array_bytes(a.at(2), &raw)) {
        uint64_t off = a.u64(3), len = a.at(4).is_nullish() ? raw.size() : a.u64(4);
        if (off > raw.size()) off = raw.size();
        if (len > raw.size() - off) len = raw.size() - off;
        *result = bool_value(d->write_buffer(a.u32(0), a.u64(1), ConstByteView(raw.data() + off, len)));
      }
      break;
    }
    case op_readBuffer: {
      ArrayView<uint8_t> raw;
      if (script::typed_array_bytes(a.at(2), &raw)) *result = bool_value(d->read_buffer(a.u32(0), a.u64(1), ByteView(raw.data(), raw.size())));
      break;
    }
    case op_createTexture: {
      String fmt(vm.allocator());
      arg_text(vm, a.at(6), &fmt);
      const wgpu::FormatDesc* f = wgpu::find_format(fmt.view());
      *result = num_value(f ? d->create_texture(a.u32(0), a.u32(1), a.u32(2), a.u32(3), a.u32(4), static_cast<uint8_t>(a.u32(5)), f, a.u32(7)) : 0);
      if (!f) d->error.assign("unknown texture format");
      break;
    }
    case op_destroyTexture: d->destroy_texture(a.u32(0)); break;
    case op_createView: {
      const wgpu::Texture* t = d->texture(a.u32(0));
      if (!t) { *result = num_value(0); break; }
      wgpu::TextureView v;
      v.texture = a.u32(0);
      const Value desc = a.at(1);
      String fmt(vm.allocator());
      v.format = prop_text(vm, desc, "format", &fmt) && !fmt.empty() ? wgpu::find_format(fmt.view()) : t->format;
      if (!v.format) v.format = t->format;
      v.dim = static_cast<uint8_t>(prop_num(vm, desc, "dim", 1));
      v.base_mip = static_cast<uint32_t>(prop_num(vm, desc, "baseMip"));
      v.mip_count = static_cast<uint32_t>(prop_num(vm, desc, "mipCount", t->mip_count - v.base_mip));
      v.base_layer = static_cast<uint32_t>(prop_num(vm, desc, "baseLayer"));
      v.layer_count = static_cast<uint32_t>(prop_num(vm, desc, "layerCount", 1));
      v.aspect = static_cast<uint8_t>(prop_num(vm, desc, "aspect"));
      if (v.base_mip >= t->mip_count || v.mip_count == 0 || v.base_mip + v.mip_count > t->mip_count) { d->error.assign("texture view mip range out of the texture"); *result = num_value(0); break; }
      *result = num_value(d->create_view(v));
      break;
    }
    case op_createSampler: {
      const Value desc = a.at(0);
      wgpu::Sampler s;
      s.params.min_filter = static_cast<uint32_t>(prop_num(vm, desc, "min", 0x2600));
      s.params.mag_filter = static_cast<uint32_t>(prop_num(vm, desc, "mag", 0x2600));
      s.params.wrap_s = static_cast<uint32_t>(prop_num(vm, desc, "ws", 0x812F));
      s.params.wrap_t = static_cast<uint32_t>(prop_num(vm, desc, "wt", 0x812F));
      s.params.wrap_r = static_cast<uint32_t>(prop_num(vm, desc, "wr", 0x812F));
      s.params.min_lod = static_cast<float>(prop_num(vm, desc, "lodMin", 0));
      s.params.max_lod = static_cast<float>(prop_num(vm, desc, "lodMax", 32));
      const uint32_t cmp = static_cast<uint32_t>(prop_num(vm, desc, "compare", 0));
      if (cmp) { s.params.compare_mode = 0x884E; s.params.compare_func = cmp; s.comparison = true; }
      *result = num_value(d->create_sampler(s));
      break;
    }
    case op_createShaderModule: {
      String code(vm.allocator()), log(vm.allocator());
      arg_text(vm, a.at(0), &code);
      const uint32_t id = d->create_shader_module(code.view(), &log);
      JsObject* o = vm.new_plain();
      vm.define_named(o, "id", num_value(id));
      vm.define_named(o, "error", str_value(vm, log.view()));
      *result = Value::make_object(o);
      break;
    }
    case op_createBindGroupLayout: {
      List<wgpu::BglEntry> entries(vm.allocator());
      if (a.at(0).is_object()) {
        JsObject* list = a.at(0).as_object();
        for (uint32_t i = 0; i < list->length; ++i) {
          if (!list->items[i].is_object()) continue;
          JsObject* e = list->items[i].as_object();
          wgpu::BglEntry b;
          b.binding = static_cast<uint32_t>(item_num(e, 0));
          b.visibility = static_cast<uint32_t>(item_num(e, 1));
          b.kind = static_cast<uint8_t>(item_num(e, 2));
          b.buffer_type = static_cast<uint8_t>(item_num(e, 3));
          b.dynamic_offset = item_num(e, 4) != 0;
          b.min_size = static_cast<uint64_t>(item_num(e, 5));
          b.sampler_type = static_cast<uint8_t>(item_num(e, 6));
          b.sample_type = static_cast<uint8_t>(item_num(e, 7));
          b.view_dim = static_cast<uint8_t>(item_num(e, 8));
          b.multisampled = item_num(e, 9) != 0;
          b.storage_access = static_cast<uint8_t>(item_num(e, 10));
          entries.push_back(b);
        }
      }
      *result = num_value(d->create_bind_group_layout(entries));
      break;
    }
    case op_createPipelineLayout: {
      uint32_t ids[16];
      uint32_t n = 0;
      if (a.at(0).is_object()) {
        JsObject* list = a.at(0).as_object();
        for (uint32_t i = 0; i < list->length && n < 16; ++i) ids[n++] = static_cast<uint32_t>(item_num(list, i));
      }
      *result = num_value(d->create_pipeline_layout(ArrayView<const uint32_t>(ids, n)));
      break;
    }
    case op_createBindGroup: {
      List<wgpu::BgEntry> entries(vm.allocator());
      if (a.at(1).is_object()) {
        JsObject* list = a.at(1).as_object();
        for (uint32_t i = 0; i < list->length; ++i) {
          if (!list->items[i].is_object()) continue;
          JsObject* e = list->items[i].as_object();
          wgpu::BgEntry b;
          b.binding = static_cast<uint32_t>(item_num(e, 0));
          b.kind = static_cast<uint8_t>(item_num(e, 1));
          b.id = static_cast<uint32_t>(item_num(e, 2));
          b.offset = static_cast<uint64_t>(item_num(e, 3));
          b.size = static_cast<uint64_t>(item_num(e, 4));
          entries.push_back(b);
        }
      }
      *result = num_value(d->create_bind_group(a.u32(0), entries));
      break;
    }
    case op_createRenderPipeline: {
      const Value desc = a.at(0);
      List<UniquePointer<wgpu::VertexBufferLayout>> buffers(vm.allocator());
      if (JsObject* bl = prop_array(vm, desc, "buffers")) {
        for (uint32_t i = 0; i < bl->length; ++i) {
          UniquePointer<wgpu::VertexBufferLayout> vb = UniquePointer<wgpu::VertexBufferLayout>::make(vm.allocator(), vm.allocator());
          if (bl->items[i].is_object()) {
            const Value b = bl->items[i];
            vb->stride = static_cast<uint64_t>(prop_num(vm, b, "stride"));
            vb->instance = prop_num(vm, b, "step") != 0;
            if (JsObject* attrs = prop_array(vm, b, "attrs"))
              for (uint32_t k = 0; k < attrs->length; ++k) {
                if (!attrs->items[k].is_object()) continue;
                JsObject* at = attrs->items[k].as_object();
                wgpu::VertexAttribute va;
                va.shader_location = static_cast<uint32_t>(item_num(at, 0));
                va.offset = static_cast<uint64_t>(item_num(at, 1));
                va.format = static_cast<uint8_t>(item_num(at, 2));
                vb->attributes.push_back(va);
              }
          }
          buffers.push_back(static_cast<UniquePointer<wgpu::VertexBufferLayout>&&>(vb));
        }
      }
      List<wgpu::ColorTarget> targets(vm.allocator());
      if (JsObject* tl = prop_array(vm, desc, "targets"))
        for (uint32_t i = 0; i < tl->length; ++i) {
          wgpu::ColorTarget ct;
          if (tl->items[i].is_object()) {
            const Value t = tl->items[i];
            String fmt(vm.allocator());
            if (prop_text(vm, t, "format", &fmt)) ct.format = wgpu::find_format(fmt.view());
            ct.write_mask = static_cast<uint8_t>(prop_num(vm, t, "mask", 15));
            JsObject* color = prop_array(vm, t, "color");
            JsObject* alpha = prop_array(vm, t, "alpha");
            if (color && alpha) {
              ct.blend = true;
              ct.color.op = static_cast<uint8_t>(item_num(color, 0)); ct.color.src = static_cast<uint8_t>(item_num(color, 1)); ct.color.dst = static_cast<uint8_t>(item_num(color, 2));
              ct.alpha.op = static_cast<uint8_t>(item_num(alpha, 0)); ct.alpha.src = static_cast<uint8_t>(item_num(alpha, 1)); ct.alpha.dst = static_cast<uint8_t>(item_num(alpha, 2));
            }
          }
          targets.push_back(ct);
        }
      wgpu::DepthStencilState dsv;
      bool has_ds = false;
      Value dsd;
      if (vm.get_named(desc, "ds", &dsd) && dsd.is_object()) {
        has_ds = true;
        String fmt(vm.allocator());
        if (prop_text(vm, dsd, "format", &fmt)) dsv.format = wgpu::find_format(fmt.view());
        dsv.depth_write = prop_num(vm, dsd, "depthWrite") != 0;
        dsv.depth_compare = static_cast<uint8_t>(prop_num(vm, dsd, "depthCompare", 7));
        auto face = [&](const char* name, wgpu::StencilFace* f) {
          if (JsObject* o = prop_array(vm, dsd, name)) {
            f->compare = static_cast<uint8_t>(item_num(o, 0, 7)); f->fail = static_cast<uint8_t>(item_num(o, 1)); f->depth_fail = static_cast<uint8_t>(item_num(o, 2)); f->pass = static_cast<uint8_t>(item_num(o, 3));
          }
        };
        face("front", &dsv.front);
        face("back", &dsv.back);
        dsv.stencil_read_mask = static_cast<uint32_t>(prop_num(vm, dsd, "readMask", 4294967295.0));
        dsv.stencil_write_mask = static_cast<uint32_t>(prop_num(vm, dsd, "writeMask", 4294967295.0));
        dsv.bias = static_cast<int32_t>(prop_num(vm, dsd, "bias"));
        dsv.bias_slope = static_cast<float>(prop_num(vm, dsd, "biasSlope"));
        dsv.bias_clamp = static_cast<float>(prop_num(vm, dsd, "biasClamp"));
      }
      String vs_entry(vm.allocator()), fs_entry(vm.allocator());
      prop_text(vm, desc, "vsEntry", &vs_entry);
      prop_text(vm, desc, "fsEntry", &fs_entry);
      List<UniquePointer<wgpu::Device::ConstantValue>> vc(vm.allocator()), fc(vm.allocator());
      constants_of(vm, prop_array(vm, desc, "vsConstants"), &vc);
      constants_of(vm, prop_array(vm, desc, "fsConstants"), &fc);
      *result = num_value(d->create_render_pipeline(static_cast<uint32_t>(prop_num(vm, desc, "layout")), static_cast<uint32_t>(prop_num(vm, desc, "vs")), vs_entry.view(),
                                                    static_cast<uint32_t>(prop_num(vm, desc, "fs")), fs_entry.view(), buffers, targets, has_ds ? &dsv : nullptr,
                                                    static_cast<uint8_t>(prop_num(vm, desc, "topology", 3)), static_cast<uint8_t>(prop_num(vm, desc, "stripFormat")),
                                                    prop_num(vm, desc, "ccw", 1) != 0, static_cast<uint8_t>(prop_num(vm, desc, "cull")), prop_num(vm, desc, "unclipped") != 0,
                                                    static_cast<uint32_t>(prop_num(vm, desc, "samples", 1)), vc, fc));
      break;
    }
    case op_createComputePipeline: {
      const Value desc = a.at(0);
      String entry(vm.allocator());
      prop_text(vm, desc, "entry", &entry);
      List<UniquePointer<wgpu::Device::ConstantValue>> cv(vm.allocator());
      constants_of(vm, prop_array(vm, desc, "constants"), &cv);
      *result = num_value(d->create_compute_pipeline(static_cast<uint32_t>(prop_num(vm, desc, "layout")), static_cast<uint32_t>(prop_num(vm, desc, "module")), entry.view(), cv));
      break;
    }
    case op_getBindGroupLayout: *result = num_value(d->pipeline_layout_bgl(a.u32(0), a.flag(1), a.u32(2))); break;
    case op_createQuerySet: *result = num_value(d->create_query_set(static_cast<uint8_t>(a.u32(0)), a.u32(1))); break;
    case op_submit: {
      // an array of command buffers, each an array of commands
      bool ok = true;
      d->error.clear();
      if (a.at(0).is_object()) {
        JsObject* buffers = a.at(0).as_object();
        for (uint32_t i = 0; i < buffers->length; ++i) {
          if (!buffers->items[i].is_object()) continue;
          JsObject* cmds = buffers->items[i].as_object();
          for (uint32_t k = 0; k < cmds->length; ++k) {
            if (!cmds->items[k].is_object()) continue;
            if (!run_command(vm, *d, cmds->items[k].as_object())) {
              ok = false;
              d->end_render_pass();
              d->end_compute_pass();
              break;
            }
          }
        }
      }
      *result = bool_value(ok);
      break;
    }
    case op_writeTexture: {
      // (tex, mip, x, y, z, aspect, data, offset, bytesPerRow, rowsPerImage, w, h, d)
      ArrayView<uint8_t> raw;
      if (!script::typed_array_bytes(a.at(6), &raw)) { *result = bool_value(false); break; }
      wgpu::ImageCopy ic;
      ic.texture = a.u32(0); ic.mip = a.u32(1); ic.x = a.u32(2); ic.y = a.u32(3); ic.z = a.u32(4); ic.aspect = static_cast<uint8_t>(a.u32(5));
      wgpu::BufferLayout bl;
      bl.offset = a.u64(7); bl.bytes_per_row = a.u32(8); bl.rows_per_image = a.u32(9);
      *result = bool_value(d->write_texture(ic, ConstByteView(raw.data(), raw.size()), bl, a.u32(10), a.u32(11), a.u32(12)));
      break;
    }
    case op_copyExternal: {
      // (tex, mip, x, y, layer, source, flipY, premultiply)
      List<uint8_t> px(vm.allocator());
      uint32_t w = 0, h = 0;
      if (!pixels_of(vm, a.at(5), &px, &w, &h)) { d->error.assign("could not read the pixels of the source"); *result = bool_value(false); break; }
      *result = bool_value(d->copy_external(a.u32(0), a.u32(1), a.u32(2), a.u32(3), a.u32(4), ConstByteView(px.view().data(), px.count()), w, h, a.flag(6), a.flag(7)));
      break;
    }
    case op_readQuery: {
      const wgpu::QuerySet* q = d->query_set(a.u32(0));
      JsObject* arr = vm.new_array(0);
      if (q)
        for (uint32_t i = a.u32(1); i < q->results.count() && i < a.u32(1) + a.u32(2); ++i) vm.array_push(arr, num_value(static_cast<double>(q->results[i])));
      *result = Value::make_object(arr);
      break;
    }
    case op_lastError: {
      *result = str_value(vm, d->error.view());
      d->error.clear();
      break;
    }
    case op_detach: {   // an ArrayBuffer that was a mapped range: it has no bytes any more
      if (a.at(0).is_object()) script::detach_array_buffer(a.at(0).as_object());
      break;
    }
    case op_bufferSize: *result = num_value(static_cast<double>(d->buffer_size(a.u32(0)))); break;
    default: break;
  }
  return true;
}

}  // namespace

bool install_gpu(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__gpu_create", gpu_create, 0);
  vm.define_native(g, "__gpu_ops", gpu_ops, 0);
  vm.define_native(g, "__gpu", gpu_call, 12);
  vm.define_native(g, "__gpu_present", gpu_present, 4);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
