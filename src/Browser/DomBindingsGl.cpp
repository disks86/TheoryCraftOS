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

// The natives under WebGLRenderingContext and WebGL2RenderingContext (the script side is kWebGlPrelude in WebGlPrelude.cpp):
//   __gl_create(canvas, es3, attributes) -> id (0: could not be made)
//   __gl_ops() -> the names of the operations; __gl(id, op, ...arguments) runs one
//   __gl_present(id), __gl_resize(id, width, height)
// A context draws into its own drawing buffer; present copies what changed into the canvas's bitmap, so drawImage(), toDataURL() and the page see it.
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Objects.hpp"
#include "Script/TypedArray.hpp"
#include "WebGl/GlContext.hpp"

namespace browser {
namespace bind {

struct GlEntry {
  explicit GlEntry(allocators::IAllocator& a) : staging(a) {}
  UniquePointer<webgl::GlContext> ctx;
  uint32_t serial = 0;
  List<uint32_t> staging;
};

struct GlState {
  explicit GlState(allocators::IAllocator& a) : items(a) {}
  List<UniquePointer<GlEntry>> items;
};

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

PageObjects& objs(Vm& vm) { return impl(vm).host.objects(); }

GlEntry* entry_of(Vm& vm, double id) {
  GlState* s = impl(vm).gl.get();
  if (!s || id < 1 || id > static_cast<double>(s->items.count())) return nullptr;
  return s->items[static_cast<uint64_t>(id) - 1].get();
}

// Copies what has been drawn into the canvas's bitmap.
void present_entry(Vm& vm, GlEntry& e) {
  webgl::GlContext& gl = *e.ctx;
  const uint64_t need = uint64_t{gl.width()} * gl.height();
  if (e.staging.count() != need) {
    e.staging.clear();
    if (!e.staging.reserve(need)) return;
    for (uint64_t i = 0; i < need; ++i) e.staging.push_back(0);
  }
  const bool first = e.staging.count() == need;
  gpu::TouchBox box;
  if (!gl.present(e.staging.view().data(), gl.width(), &box, false)) return;
  (void)first;
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = objects.canvas(e.serial, gl.width(), gl.height());
  if (!bm) return;
  ImageSource src;
  src.pixels = ArrayView<const uint32_t>(e.staging.view().data(), need);
  src.width = gl.width();
  src.height = gl.height();
  src.pitch = gl.width();
  const int32_t w = box.x1 - box.x0 + 1, h = box.y1 - box.y0 + 1;
  if (w <= 0 || h <= 0) return;
  bm->put_image_data(src, 0, 0, box.x0, box.y0, w, h);
  objects.mark_dirty();
}

// --- Arguments --------------------------------------------------------------------------------------------------------------------------------------------

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
    if (x.is_object()) {
      Value h;
      if (vm.get_named(x, "__h", &h) && h.is_number()) return h.u.number;
    }
    double d = def;
    vm.to_number(x, &d);
    return d;
  }
  uint32_t u32(uint32_t i) const {
    const double d = num(i);
    return d <= 0 ? 0 : (d >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(d));
  }
  uint64_t u64(uint32_t i) const { const double d = num(i); return d <= 0 ? 0 : static_cast<uint64_t>(d); }
  int32_t i32(uint32_t i) const {
    const double d = num(i);
    return d <= -2147483648.0 ? INT32_MIN : (d >= 2147483647.0 ? INT32_MAX : static_cast<int32_t>(d));
  }
  float f32(uint32_t i) const { return static_cast<float>(num(i)); }
  bool flag(uint32_t i) const {
    const Value x = at(i);
    return x.is_bool() ? x.u.boolean : num(i) != 0;
  }
  bool bytes(uint32_t i, ArrayView<uint8_t>* out) const { return script::typed_array_bytes(at(i), out); }
  bool text(uint32_t i, String* out) const { return arg_text(vm, at(i), out); }
};

Value make_number_array(Vm& vm, const double* values, uint32_t n) {
  JsObject* a = vm.new_array(n);
  for (uint32_t i = 0; i < n; ++i) vm.array_push(a, Value::make_number(values[i]));
  return Value::make_object(a);
}

#define GL_OPS(X)                                                                                                                                              \
  X(create) X(destroy) X(isObject) X(bindBuffer) X(bindBufferRange) X(bufferData) X(bufferSubData) X(getBufferSubData) X(copyBufferSubData)                    \
  X(getBufferParameter) X(activeTexture) X(bindTexture) X(bindSampler) X(texImage) X(texSubImage) X(texStorage) X(copyTexImage2D) X(copyTexSubImage)         \
  X(texParameter) X(getTexParameter) X(generateMipmap) X(samplerParameter) X(pixelStorei) X(bindRenderbuffer) X(renderbufferStorage)                         \
  X(getRenderbufferParameter) X(bindFramebuffer) X(framebufferTexture) X(framebufferRenderbuffer) X(checkFramebufferStatus)                                  \
  X(getFramebufferAttachmentParameter) X(drawBuffers) X(readBuffer) X(invalidateFramebuffer) X(blitFramebuffer) X(enable) X(disable) X(isEnabled)           \
  X(viewport) X(scissor) X(depthRange) X(blendColor) X(blendEquationSeparate) X(blendFuncSeparate) X(colorMask) X(depthMask) X(depthFunc)                   \
  X(stencilFuncSeparate) X(stencilOpSeparate) X(stencilMaskSeparate) X(cullFace) X(frontFace) X(polygonOffset) X(lineWidth) X(clearColor) X(clearDepth)   \
  X(clearStencil) X(sampleCoverage) X(getParameter) X(getError) X(useProgram) X(shaderSource) X(compileShader) X(getShaderParameter) X(getShaderInfoLog)    \
  X(getShaderSource) X(attachShader) X(detachShader) X(linkProgram) X(validateProgram) X(getProgramParameter) X(getProgramInfoLog) X(bindAttribLocation)    \
  X(getAttribLocation) X(getActiveUniform) X(getActiveAttrib) X(getUniformLocation) X(uniform) X(getUniform) X(getUniformBlockIndex)                        \
  X(getActiveUniformBlockName) X(getActiveUniformBlockParameter) X(uniformBlockBinding) X(getFragDataLocation) X(getAttachedShaders)                         \
  X(transformFeedbackVaryings) X(enableVertexAttribArray) X(disableVertexAttribArray) X(vertexAttribPointer) X(vertexAttribDivisor) X(vertexAttrib)        \
  X(getVertexAttrib) X(bindVertexArray) X(clear) X(clearBuffer) X(clearBufferfi) X(drawArrays) X(drawElements) X(drawRangeElements) X(readPixels)          \
  X(beginQuery) X(endQuery) X(getQueryParameter) X(getQuery) X(currentProgram) X(boundObject) X(lose) X(restore) X(attributes) X(beginTransformFeedback) X(endTransformFeedback)

enum Op : uint32_t {
#define X(name) op_##name,
  GL_OPS(X)
#undef X
  op_count
};

const char* const kOpNames[] = {
#define X(name) #name,
    GL_OPS(X)
#undef X
};

NATIVE(gl_ops) {
  (void)self; (void)args; (void)argc;
  JsObject* a = vm.new_array(op_count);
  for (uint32_t i = 0; i < op_count; ++i) vm.array_push(a, str_value(vm, kOpNames[i]));
  *result = Value::make_object(a);
  return true;
}

// The pixels of a script image source (<img>, <canvas>, <video>, ImageData) as RGBA bytes.
bool source_rgba(Vm& vm, const Value& source, List<uint8_t>* out, uint32_t* w, uint32_t* h) {
  ArrayView<uint8_t> raw;
  if (source.is_object()) {
    Value dv, wv, hv;
    if (vm.get_named(source, "data", &dv) && vm.get_named(source, "width", &wv) && vm.get_named(source, "height", &hv) && wv.is_number() && hv.is_number() &&
        script::typed_array_bytes(dv, &raw)) {   // ImageData
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

NATIVE(gl_create) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  if (!n || !n->is_tag("canvas")) return vm.throw_error(ErrorKind::TypeError, "not a canvas");
  DomBindings::Impl& c = impl(vm);
  if (!c.gl) c.gl = UniquePointer<GlState>::make(vm.allocator(), vm.allocator());
  if (!c.gl) { *result = num_value(0); return true; }
  const Args a{vm, args, argc};
  const Value attrs = arg_at(args, argc, 2);
  webgl::GlContext::Attributes at;
  auto get = [&](const char* name, bool def) {
    Value v;
    if (attrs.is_object() && vm.get_named(attrs, name, &v) && !v.is_undefined()) return Vm::to_boolean(v);
    return def;
  };
  at.alpha = get("alpha", true);
  at.depth = get("depth", true);
  at.stencil = get("stencil", false);
  at.antialias = get("antialias", true);
  at.premultiplied = get("premultipliedAlpha", true);
  at.preserve = get("preserveDrawingBuffer", false);
  uint32_t w = 300, h = 150;
  StringView sv;
  uint64_t value;
  if (dom::get_attribute(n, "width", &sv) && sv.trimmed().parse_uint(&value) && value > 0 && value <= 8192) w = static_cast<uint32_t>(value);
  if (dom::get_attribute(n, "height", &sv) && sv.trimmed().parse_uint(&value) && value > 0 && value <= 8192) h = static_cast<uint32_t>(value);
  UniquePointer<GlEntry> e = UniquePointer<GlEntry>::make(vm.allocator(), vm.allocator());
  if (!e) { *result = num_value(0); return true; }
  e->serial = n->serial;
  e->ctx = UniquePointer<webgl::GlContext>::make(vm.allocator(), vm.allocator(), a.flag(1), w, h, at);
  if (!e->ctx || !e->ctx->valid()) { *result = num_value(0); return true; }
  c.gl->items.push_back(static_cast<UniquePointer<GlEntry>&&>(e));
  // make the canvas bitmap the size of the drawing buffer; a fresh one is transparent
  {
    PageObjects& objects = objs(vm);
    PageObjects::Lock lock(objects);
    objects.canvas(n->serial, w, h);
  }
  *result = num_value(static_cast<double>(c.gl->items.count()));
  return true;
}

NATIVE(gl_present) {
  (void)self;
  GlEntry* e = entry_of(vm, Args{vm, args, argc}.num(0));
  if (e) present_entry(vm, *e);
  *result = Value();
  return true;
}

NATIVE(gl_resize) {
  (void)self;
  const Args a{vm, args, argc};
  GlEntry* e = entry_of(vm, a.num(0));
  if (e) {
    e->ctx->resize(a.u32(1), a.u32(2));
    PageObjects& objects = objs(vm);
    PageObjects::Lock lock(objects);
    objects.canvas(e->serial, e->ctx->width(), e->ctx->height());
  }
  *result = Value();
  return true;
}

bool make_pixels_arg(Vm& vm, const Args& a, uint32_t i, ConstByteView* bytes, List<uint8_t>* store, uint32_t* w, uint32_t* h, bool* from_source) {
  *from_source = false;
  const Value d = a.at(i);
  ArrayView<uint8_t> raw;
  if (d.is_nullish()) return false;
  if (script::typed_array_bytes(d, &raw)) {
    *bytes = ConstByteView(raw.data(), raw.size());
    return true;
  }
  if (source_rgba(vm, d, store, w, h)) {
    *bytes = ConstByteView(store->view().data(), store->count());
    *from_source = true;
    return true;
  }
  return false;
}

NATIVE(gl_call) {
  (void)self;
  const Args all{vm, args, argc};
  GlEntry* e = entry_of(vm, all.num(0));
  *result = Value();
  if (!e || argc < 2) return true;
  webgl::GlContext& gl = *e->ctx;
  const uint32_t op = all.u32(1);
  // arguments after the context and the operation
  const Args a{vm, args + 2, argc - 2};
  bool touches_screen = false;
  switch (op) {
    case op_create: {
      const uint32_t kind = a.u32(0);
      uint32_t id = 0;
      switch (kind) {
        case 'b': id = gl.create_buffer(); break;
        case 't': id = gl.create_texture(); break;
        case 'f': id = gl.create_framebuffer(); break;
        case 'r': id = gl.create_renderbuffer(); break;
        case 's': id = gl.create_shader(a.u32(1)); break;
        case 'p': id = gl.create_program(); break;
        case 'v': id = gl.create_vao(); break;
        case 'a': id = gl.create_sampler(); break;
        case 'q': id = gl.create_query(); break;
        case 'x': id = gl.create_transform_feedback(); break;
        default: break;
      }
      *result = num_value(id);
      break;
    }
    case op_destroy: gl.delete_object(a.u32(0), a.u32(1)); break;
    case op_isObject: *result = bool_value(gl.is_object(a.u32(0), a.u32(1))); break;
    case op_bindBuffer: gl.bind_buffer(a.u32(0), a.u32(1)); break;
    case op_bindBufferRange: gl.bind_buffer_range(a.u32(0), a.u32(1), a.u32(2), static_cast<uint64_t>(a.num(3)), static_cast<uint64_t>(a.num(4))); break;
    case op_bufferData: {
      ArrayView<uint8_t> raw;
      if (a.bytes(1, &raw)) gl.buffer_data(a.u32(0), ConstByteView(raw.data(), raw.size()), raw.size(), a.u32(2));
      else if (a.at(1).is_number()) gl.buffer_data(a.u32(0), ConstByteView(), static_cast<uint64_t>(a.num(1) < 0 ? 0 : a.num(1)), a.u32(2));
      else gl.set_error(0x501);
      break;
    }
    case op_bufferSubData: {
      ArrayView<uint8_t> raw;
      if (a.bytes(2, &raw)) gl.buffer_sub_data(a.u32(0), static_cast<uint64_t>(a.num(1)), ConstByteView(raw.data(), raw.size()));
      else gl.set_error(0x501);
      break;
    }
    case op_getBufferSubData: {
      ArrayView<uint8_t> raw;
      if (a.bytes(2, &raw)) gl.get_buffer_sub_data(a.u32(0), static_cast<uint64_t>(a.num(1)), ByteView(raw.data(), raw.size()));
      break;
    }
    case op_copyBufferSubData: gl.copy_buffer_sub_data(a.u32(0), a.u32(1), static_cast<uint64_t>(a.num(2)), static_cast<uint64_t>(a.num(3)), static_cast<uint64_t>(a.num(4))); break;
    case op_getBufferParameter: {
      const uint32_t id = gl.bound_buffer(a.u32(0));
      if (!id) { gl.set_error(0x502); break; }
      *result = num_value(a.u32(1) == 0x8764 ? static_cast<double>(gl.buffer_size(id)) : gl.buffer_usage(id));
      break;
    }
    case op_activeTexture: gl.active_texture(a.u32(0)); break;
    case op_bindTexture: gl.bind_texture(a.u32(0), a.u32(1)); break;
    case op_bindSampler: gl.bind_sampler(a.u32(0), a.u32(1)); break;
    case op_texImage: {
      // (target, level, internal, w, h, d, border, format, type, pixels, is3d)
      uint32_t w = a.u32(3), h = a.u32(4);
      ConstByteView bytes;
      List<uint8_t> store(vm.allocator());
      bool from_source = false;
      bool has;
      if (a.at(9).is_number() || (gl.bound_buffer(0x88EC) && !a.at(9).is_nullish())) {   // an offset into the pixel unpack buffer
        has = a.at(9).is_number() && gl.pixel_unpack_view(a.u64(9), &bytes);
        if (!has) { gl.set_error(0x502); break; }
      } else {
        has = make_pixels_arg(vm, a, 9, &bytes, &store, &w, &h, &from_source);
      }
      const uint32_t format = from_source ? 0x1908 : a.u32(7), type = from_source ? 0x1401 : a.u32(8);
      gl.tex_image(a.u32(0), a.i32(1), a.u32(2), w, h, a.u32(5), a.i32(6), format, type, bytes, has, a.flag(10));
      break;
    }
    case op_texSubImage: {
      // (target, level, xo, yo, zo, w, h, d, format, type, pixels, is3d)
      uint32_t w = a.u32(5), h = a.u32(6);
      ConstByteView bytes;
      List<uint8_t> store(vm.allocator());
      bool from_source = false;
      if (a.at(10).is_number() || (gl.bound_buffer(0x88EC) && !a.at(10).is_nullish())) {
        if (!a.at(10).is_number() || !gl.pixel_unpack_view(a.u64(10), &bytes)) { gl.set_error(0x502); break; }
      } else if (!make_pixels_arg(vm, a, 10, &bytes, &store, &w, &h, &from_source)) { gl.set_error(0x501); break; }
      const uint32_t format = from_source ? 0x1908 : a.u32(8), type = from_source ? 0x1401 : a.u32(9);
      gl.tex_sub_image(a.u32(0), a.i32(1), a.i32(2), a.i32(3), a.i32(4), w, h, a.u32(7), format, type, bytes, a.flag(11));
      break;
    }
    case op_texStorage: gl.tex_storage(a.u32(0), a.u32(1), a.u32(2), a.u32(3), a.u32(4), a.u32(5)); break;
    case op_copyTexImage2D: gl.copy_tex_image_2d(a.u32(0), a.i32(1), a.u32(2), a.i32(3), a.i32(4), a.u32(5), a.u32(6), a.i32(7)); break;
    case op_copyTexSubImage: gl.copy_tex_sub_image(a.u32(0), a.i32(1), a.i32(2), a.i32(3), a.i32(4), a.i32(5), a.i32(6), a.u32(7), a.u32(8)); break;
    case op_texParameter: gl.tex_parameter(a.u32(0), a.u32(1), a.num(2)); break;
    case op_getTexParameter: {
      double d;
      if (gl.get_tex_parameter(a.u32(0), a.u32(1), &d)) *result = num_value(d);
      break;
    }
    case op_generateMipmap: gl.generate_mipmap(a.u32(0)); break;
    case op_samplerParameter: gl.sampler_parameter(a.u32(0), a.u32(1), a.num(2)); break;
    case op_pixelStorei: gl.pixel_store(a.u32(0), a.num(1)); break;
    case op_bindRenderbuffer: gl.bind_renderbuffer(a.u32(1)); break;
    case op_renderbufferStorage: gl.renderbuffer_storage(a.u32(0), a.u32(1), a.u32(2), a.u32(3)); break;
    case op_getRenderbufferParameter: {
      double d;
      if (gl.get_renderbuffer_parameter(a.u32(0), &d)) *result = num_value(d);
      break;
    }
    case op_bindFramebuffer: gl.bind_framebuffer(a.u32(0), a.u32(1)); break;
    case op_framebufferTexture: gl.framebuffer_texture(a.u32(0), a.u32(1), a.u32(2), a.u32(3), a.i32(4), a.i32(5)); break;
    case op_framebufferRenderbuffer: gl.framebuffer_renderbuffer(a.u32(0), a.u32(1), a.u32(3)); break;
    case op_checkFramebufferStatus: *result = num_value(gl.check_framebuffer_status(a.u32(0))); break;
    case op_getFramebufferAttachmentParameter: {
      double d;
      if (gl.get_framebuffer_attachment_parameter(a.u32(0), a.u32(1), a.u32(2), &d)) *result = num_value(d);
      break;
    }
    case op_drawBuffers: {
      const Value arr = a.at(0);
      uint32_t list[webgl::kMaxDrawBuffers + 1];
      uint32_t n = 0;
      if (arr.is_object())
        for (uint32_t i = 0; i < arr.as_object()->length && n <= webgl::kMaxDrawBuffers; ++i) {
          const Value x = arr.as_object()->items[i];
          list[n++] = x.is_number() ? static_cast<uint32_t>(x.u.number) : 0;
        }
      gl.draw_buffers(ArrayView<const uint32_t>(list, n));
      break;
    }
    case op_readBuffer: gl.read_buffer(a.u32(0)); break;
    case op_invalidateFramebuffer: break;
    case op_blitFramebuffer:
      gl.blit_framebuffer(a.i32(0), a.i32(1), a.i32(2), a.i32(3), a.i32(4), a.i32(5), a.i32(6), a.i32(7), a.u32(8), a.u32(9));
      touches_screen = true;
      break;
    case op_enable: gl.enable(a.u32(0), true); break;
    case op_disable: gl.enable(a.u32(0), false); break;
    case op_isEnabled: *result = bool_value(gl.is_enabled(a.u32(0))); break;
    case op_viewport: gl.viewport(a.i32(0), a.i32(1), a.i32(2), a.i32(3)); break;
    case op_scissor: gl.scissor(a.i32(0), a.i32(1), a.i32(2), a.i32(3)); break;
    case op_depthRange: gl.depth_range(a.f32(0), a.f32(1)); break;
    case op_blendColor: gl.blend_color(a.f32(0), a.f32(1), a.f32(2), a.f32(3)); break;
    case op_blendEquationSeparate: gl.blend_equation_separate(a.u32(0), a.u32(1)); break;
    case op_blendFuncSeparate: gl.blend_func_separate(a.u32(0), a.u32(1), a.u32(2), a.u32(3)); break;
    case op_colorMask: gl.color_mask(a.flag(0), a.flag(1), a.flag(2), a.flag(3)); break;
    case op_depthMask: gl.depth_mask(a.flag(0)); break;
    case op_depthFunc: gl.depth_func(a.u32(0)); break;
    case op_stencilFuncSeparate: gl.stencil_func_separate(a.u32(0), a.u32(1), a.i32(2), a.u32(3)); break;
    case op_stencilOpSeparate: gl.stencil_op_separate(a.u32(0), a.u32(1), a.u32(2), a.u32(3)); break;
    case op_stencilMaskSeparate: gl.stencil_mask_separate(a.u32(0), a.u32(1)); break;
    case op_cullFace: gl.cull_face(a.u32(0)); break;
    case op_frontFace: gl.front_face(a.u32(0)); break;
    case op_polygonOffset: gl.polygon_offset(a.f32(0), a.f32(1)); break;
    case op_lineWidth: gl.line_width(a.f32(0)); break;
    case op_clearColor: gl.clear_color(a.f32(0), a.f32(1), a.f32(2), a.f32(3)); break;
    case op_clearDepth: gl.clear_depth(a.f32(0)); break;
    case op_clearStencil: gl.clear_stencil(a.i32(0)); break;
    case op_sampleCoverage: gl.sample_coverage(a.f32(0), a.flag(1)); break;
    case op_getParameter: {
      double out[16];
      uint32_t count;
      if (gl.get_integer(a.u32(0), out, &count)) *result = count == 1 ? num_value(out[0]) : make_number_array(vm, out, count);
      break;
    }
    case op_getError: *result = num_value(gl.get_error()); break;
    case op_useProgram: gl.use_program(a.u32(0)); break;
    case op_shaderSource: {
      String s(vm.allocator());
      if (a.text(1, &s)) gl.shader_source(a.u32(0), s.view());
      break;
    }
    case op_compileShader: gl.compile_shader(a.u32(0)); break;
    case op_getShaderParameter: {
      double d;
      if (gl.shader_status(a.u32(0), a.u32(1), &d)) *result = num_value(d);
      break;
    }
    case op_getShaderInfoLog: *result = str_value(vm, gl.shader_log(a.u32(0))); break;
    case op_getShaderSource: *result = str_value(vm, gl.shader_src(a.u32(0))); break;
    case op_attachShader: gl.attach_shader(a.u32(0), a.u32(1)); break;
    case op_detachShader: gl.detach_shader(a.u32(0), a.u32(1)); break;
    case op_linkProgram: gl.link_program(a.u32(0)); break;
    case op_validateProgram: gl.validate_program(a.u32(0)); break;
    case op_getProgramParameter: {
      double d;
      if (gl.program_status(a.u32(0), a.u32(1), &d)) *result = num_value(d);
      break;
    }
    case op_getProgramInfoLog: *result = str_value(vm, gl.program_log(a.u32(0))); break;
    case op_bindAttribLocation: {
      String s(vm.allocator());
      if (a.text(2, &s)) gl.bind_attrib_location(a.u32(0), a.u32(1), s.view());
      break;
    }
    case op_getAttribLocation: {
      String s(vm.allocator());
      if (a.text(1, &s)) *result = num_value(gl.get_attrib_location(a.u32(0), s.view()));
      break;
    }
    case op_getActiveUniform: {
      const webgl::UniformLeaf* u = gl.active_uniform(a.u32(0), a.u32(1));
      if (!u) break;
      JsObject* arr = vm.new_array(3);
      vm.array_push(arr, str_value(vm, u->name.view()));
      vm.array_push(arr, num_value(u->size));
      vm.array_push(arr, num_value(u->gl_type));
      *result = Value::make_object(arr);
      break;
    }
    case op_getActiveAttrib: {
      const webgl::AttribInfo* u = gl.active_attrib(a.u32(0), a.u32(1));
      if (!u) break;
      JsObject* arr = vm.new_array(3);
      vm.array_push(arr, str_value(vm, u->name.view()));
      vm.array_push(arr, num_value(u->size));
      vm.array_push(arr, num_value(u->gl_type));
      *result = Value::make_object(arr);
      break;
    }
    case op_getUniformLocation: {
      String s(vm.allocator());
      if (a.text(1, &s)) {
        const int32_t loc = gl.get_uniform_location(a.u32(0), s.view());
        *result = loc < 0 ? Value::make_null() : num_value(loc);
      }
      break;
    }
    case op_uniform: {
      // (location, kind, cols, rows, data)
      const int32_t loc = a.at(0).is_nullish() ? -1 : a.i32(0);
      const uint32_t kind = a.u32(1), cols = a.u32(2), rows = a.u32(3);
      const Value d = a.at(4);
      List<uint32_t> lanes(vm.allocator());
      ArrayView<uint8_t> raw;
      if (script::typed_array_bytes(d, &raw)) {
        const uint64_t words = raw.size() / 4;
        for (uint64_t i = 0; i < words; ++i) {
          uint32_t v;
          __builtin_memcpy(&v, raw.data() + i * 4, 4);
          lanes.push_back(v);
        }
        // typed arrays of another kind than the call: an Int32Array given to uniform1fv is an error in WebGL; convert here
      } else if (d.is_object()) {
        for (uint32_t i = 0; i < d.as_object()->length; ++i) {
          const Value x = d.as_object()->items[i];
          const double dv = x.is_number() ? x.u.number : (x.is_bool() ? (x.u.boolean ? 1 : 0) : 0);
          lanes.push_back(kind == 'f' ? __builtin_bit_cast(uint32_t, static_cast<float>(dv)) : static_cast<uint32_t>(static_cast<int64_t>(dv)));
        }
      } else if (d.is_number()) {
        lanes.push_back(kind == 'f' ? __builtin_bit_cast(uint32_t, static_cast<float>(d.u.number)) : static_cast<uint32_t>(static_cast<int64_t>(d.u.number)));
      }
      const uint32_t per = cols * rows;
      if (per == 0 || lanes.count() == 0 || lanes.count() % per != 0) { gl.set_error(0x501); break; }
      gl.uniform(loc, kind, cols, rows, static_cast<uint32_t>(lanes.count() / per), lanes.view().data());
      break;
    }
    case op_getUniform: {
      double out[16];
      uint32_t count = 0;
      if (gl.get_uniform(a.u32(0), a.i32(1), out, &count)) *result = count == 1 ? num_value(out[0]) : make_number_array(vm, out, count);
      break;
    }
    case op_getUniformBlockIndex: {
      String s(vm.allocator());
      if (a.text(1, &s)) *result = num_value(gl.get_uniform_block_index(a.u32(0), s.view()));
      break;
    }
    case op_getActiveUniformBlockName: *result = str_value(vm, gl.uniform_block_name(a.u32(0), a.u32(1))); break;
    case op_getActiveUniformBlockParameter: {
      double d;
      if (gl.uniform_block_info(a.u32(0), a.u32(1), a.u32(2), &d)) *result = num_value(d);
      break;
    }
    case op_uniformBlockBinding: gl.uniform_block_binding(a.u32(0), a.u32(1), a.u32(2)); break;
    case op_getFragDataLocation: {
      String s(vm.allocator());
      if (a.text(1, &s)) *result = num_value(gl.get_frag_data_location(a.u32(0), s.view()));
      break;
    }
    case op_getAttachedShaders: {
      uint32_t ids[2];
      const uint32_t n = gl.attached_shaders(a.u32(0), ids, 2);
      double d[2] = {static_cast<double>(ids[0]), static_cast<double>(ids[1])};
      *result = make_number_array(vm, d, n);
      break;
    }
    case op_transformFeedbackVaryings: {
      const Value arr = a.at(1);
      List<String> names(vm.allocator());
      if (arr.is_object())
        for (uint32_t i = 0; i < arr.as_object()->length; ++i) {
          String s(vm.allocator());
          arg_text(vm, arr.as_object()->items[i], &s);
          names.push_back(static_cast<String&&>(s));
        }
      List<StringView> views(vm.allocator());
      for (uint64_t i = 0; i < names.count(); ++i) views.push_back(names[i].view());
      gl.transform_feedback_varyings(a.u32(0), ArrayView<const StringView>(views.view().data(), views.count()), a.u32(2));
      break;
    }
    case op_enableVertexAttribArray: gl.enable_vertex_attrib(a.u32(0), true); break;
    case op_disableVertexAttribArray: gl.enable_vertex_attrib(a.u32(0), false); break;
    case op_vertexAttribPointer: gl.vertex_attrib_pointer(a.u32(0), a.i32(1), a.u32(2), a.flag(3), a.u32(4), static_cast<uint64_t>(a.num(5)), a.flag(6)); break;
    case op_vertexAttribDivisor: gl.vertex_attrib_divisor(a.u32(0), a.u32(1)); break;
    case op_vertexAttrib: {
      // (index, kind, x, y, z, w): kind 'f', 'i' or 'u'
      const uint32_t kind = a.u32(1);
      if (kind == 'f') {
        const float v[4] = {a.f32(2), a.f32(3), a.f32(4), a.f32(5)};
        gl.vertex_attrib_value(a.u32(0), v, nullptr);
      } else {
        const int32_t v[4] = {a.i32(2), a.i32(3), a.i32(4), a.i32(5)};
        gl.vertex_attrib_value(a.u32(0), nullptr, v);
      }
      break;
    }
    case op_getVertexAttrib: {
      double out[4];
      uint32_t count;
      if (gl.get_vertex_attrib(a.u32(0), a.u32(1), out, &count)) *result = count == 1 ? num_value(out[0]) : make_number_array(vm, out, count);
      break;
    }
    case op_bindVertexArray: gl.bind_vao(a.u32(0)); break;
    case op_clear: gl.clear(a.u32(0)); touches_screen = true; break;
    case op_clearBuffer: {
      // (buffer, drawbuffer, kind, values)
      uint32_t lanes[4] = {0, 0, 0, 0};
      const Value d = a.at(3);
      ArrayView<uint8_t> raw;
      if (script::typed_array_bytes(d, &raw)) {
        for (uint32_t i = 0; i < 4 && i * 4 + 4 <= raw.size(); ++i) __builtin_memcpy(&lanes[i], raw.data() + i * 4, 4);
      } else if (d.is_object()) {
        for (uint32_t i = 0; i < 4 && i < d.as_object()->length; ++i) {
          const double dv = d.as_object()->items[i].is_number() ? d.as_object()->items[i].u.number : 0;
          lanes[i] = a.u32(2) == 'f' ? __builtin_bit_cast(uint32_t, static_cast<float>(dv)) : static_cast<uint32_t>(static_cast<int64_t>(dv));
        }
      }
      gl.clear_buffer(a.u32(0), a.i32(1), lanes, a.u32(2) == 'f' ? 0 : (a.u32(2) == 'i' ? 1 : 2));
      touches_screen = true;
      break;
    }
    case op_clearBufferfi: gl.clear_buffer_fi(a.i32(1), a.f32(2), a.i32(3)); touches_screen = true; break;
    case op_drawArrays: gl.draw_arrays(a.u32(0), a.i32(1), a.i32(2), a.at(3).is_nullish() ? 1 : a.i32(3)); touches_screen = true; break;
    case op_drawElements: gl.draw_elements(a.u32(0), a.i32(1), a.u32(2), static_cast<uint64_t>(a.num(3)), a.at(4).is_nullish() ? 1 : a.i32(4)); touches_screen = true; break;
    case op_drawRangeElements: gl.draw_range_elements(a.u32(0), a.u32(1), a.u32(2), a.i32(3), a.u32(4), static_cast<uint64_t>(a.num(5))); touches_screen = true; break;
    case op_readPixels: {
      ArrayView<uint8_t> raw;
      if (a.at(6).is_number()) {   // an offset into the pixel pack buffer
        ByteView into;
        if (!gl.pixel_pack_view(a.u64(6), &into)) { gl.set_error(0x502); break; }
        gl.read_pixels(a.i32(0), a.i32(1), a.u32(2), a.u32(3), a.u32(4), a.u32(5), into);
        break;
      }
      if (gl.bound_buffer(0x88EB) || !a.bytes(6, &raw)) { gl.set_error(0x502); break; }
      gl.read_pixels(a.i32(0), a.i32(1), a.u32(2), a.u32(3), a.u32(4), a.u32(5), ByteView(raw.data(), raw.size()));
      break;
    }
    case op_beginQuery: gl.begin_query(a.u32(0), a.u32(1)); break;
    case op_endQuery: gl.end_query(a.u32(0)); break;
    case op_getQueryParameter: {
      double d;
      if (gl.query_result(a.u32(0), a.u32(1), &d)) *result = num_value(d);
      break;
    }
    case op_getQuery: *result = num_value(gl.current_query(a.u32(0))); break;
    case op_currentProgram: *result = num_value(gl.current_program()); break;
    case op_boundObject: {
      // (what): the handle bound to a target, for getParameter()
      const uint32_t what = a.u32(0);
      double out[1];
      uint32_t count;
      if (gl.get_integer(what, out, &count)) *result = num_value(out[0]);
      break;
    }
    case op_beginTransformFeedback: gl.begin_transform_feedback(a.u32(0)); break;
    case op_endTransformFeedback: gl.end_transform_feedback(); break;
    case op_lose: gl.lose(); break;
    case op_restore: gl.restore(); break;
    case op_attributes: {
      JsObject* o = vm.new_plain();
      const webgl::GlContext::Attributes& at = gl.attributes();
      vm.define_named(o, "alpha", bool_value(at.alpha));
      vm.define_named(o, "depth", bool_value(at.depth));
      vm.define_named(o, "stencil", bool_value(at.stencil));
      vm.define_named(o, "antialias", bool_value(gl.antialiased()));
      vm.define_named(o, "premultipliedAlpha", bool_value(at.premultiplied));
      vm.define_named(o, "preserveDrawingBuffer", bool_value(at.preserve));
      *result = Value::make_object(o);
      break;
    }
    default: break;
  }
  if (touches_screen && !gl.lost()) {
    // (presented lazily by the script side after the current task; the pixels are also pulled when the canvas is read)
  }
  return true;
}

}  // namespace

void flush_gl(DomBindings::Impl& c, uint32_t serial) {
  if (!c.gl) return;
  for (uint64_t i = 0; i < c.gl->items.count(); ++i) {
    GlEntry& e = *c.gl->items[i];
    if (e.serial == serial) present_entry(c.vm, e);
  }
}

bool install_gl(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__gl_create", gl_create, 3);
  vm.define_native(g, "__gl_ops", gl_ops, 0);
  vm.define_native(g, "__gl", gl_call, 12);
  vm.define_native(g, "__gl_present", gl_present, 1);
  vm.define_native(g, "__gl_resize", gl_resize, 3);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
