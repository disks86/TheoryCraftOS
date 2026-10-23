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

// Pictures, videos and canvases for scripts: the properties and methods of <img>, <video> and <canvas> elements, the canvas "2d" context (state, paths,
// gradients, patterns, text, images, pixels), ImageData, Path2D, OffscreenCanvas and the byte arrays that ImageData uses. The drawing itself is in
// Canvas.cpp, the pictures and videos behind the elements in Objects.cpp.
#include <initializer_list>
#include "Browser/Css.hpp"
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Objects.hpp"
#include "Browser/Url.hpp"
#include "Media/ImageCodecs.hpp"
#include "Script/TypedArray.hpp"
#include "Utility/ByteText.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)
#define IS(text) k.equals(text)

constexpr double kPi = 3.14159265358979323846;

PageObjects& objs(Vm& vm) { return impl(vm).host.objects(); }

double nan_value() {
  const uint64_t bits = 0x7FF8000000000000ull;
  double d;
  __builtin_memcpy(&d, &bits, sizeof d);
  return d;
}
bool finite(double d) { return d == d && d - d == 0; }

bool arg_number(Vm& vm, const Value* args, uint32_t argc, uint32_t i, double* out) { return vm.to_number(arg_at(args, argc, i), out); }

bool need_args(Vm& vm, uint32_t argc, uint32_t n, const char* function) {
  if (argc >= n) return true;
  String m(vm.allocator());
  m.append("Failed to execute '");
  m.append(function);
  m.append("': ");
  m.append_uint(n);
  m.append(n == 1 ? " argument required, but only " : " arguments required, but only ");
  m.append_uint(argc);
  m.append(" present.");
  return vm.throw_error(ErrorKind::TypeError, m.view());
}

JsString* key_of(Vm& vm, const char* name) { return vm.intern(name); }

// --- Byte arrays: the script engine's Uint8ClampedArray and Uint8Array (Script/TypedArray.hpp); these helpers make and read them.

bool is_bytes(const Value& v) { return is_typed_array(v); }

// A byte array of `length` zeros (or of `bytes`).
JsObject* make_bytes(DomBindings::Impl& c, StringView bytes, bool clamped) { return new_typed_array(c.vm, clamped ? ArrayKind::Uint8Clamped : ArrayKind::Uint8, bytes); }

JsObject* make_zero_bytes(DomBindings::Impl& c, uint64_t length, bool clamped) { return new_zero_typed_array(c.vm, clamped ? ArrayKind::Uint8Clamped : ArrayKind::Uint8, length); }

// --- ImageData: { width, height, data, colorSpace }

NATIVE(image_data_constructor) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  JsObject* data = nullptr;
  double w = 0, h = 0;
  if (is_bytes(arg_at(args, argc, 0))) {
    if (!arg_number(vm, args, argc, 1, &w)) return false;
    ArrayView<uint8_t> view;
    typed_array_bytes(args[0], &view);
    const uint64_t byte_count = view.size();
    if (!(w >= 1) || byte_count % 4 != 0 || (byte_count / 4) % static_cast<uint32_t>(w) != 0)
      return throw_dom(vm, "InvalidStateError", "The input data length is not a multiple of (4 * width).");
    h = static_cast<double>(byte_count / 4) / w;
    if (argc > 2 && !args[2].is_undefined() && !vm.to_number(args[2], &h)) return false;
    data = args[0].as_object();
  } else {
    if (!need_args(vm, argc, 2, "ImageData") || !arg_number(vm, args, argc, 0, &w) || !arg_number(vm, args, argc, 1, &h)) return false;
    if (!(w >= 1) || !(h >= 1) || w * h > 16.0 * 1024 * 1024) return throw_dom(vm, "IndexSizeError", "The source width or height is zero or too large.");
    data = make_zero_bytes(c, static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4, true);
    if (!data) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  }
  JsObject* o = vm.new_object(c.image_data_proto);
  vm.define_named(o, "width", num_value(w));
  vm.define_named(o, "height", num_value(h));
  vm.define_named(o, "data", Value::make_object(data));
  vm.define_named(o, "colorSpace", str_value(vm, "srgb"));
  *result = Value::make_object(o);
  return true;
}

// The pixels of an ImageData as 0xAARRGGBB; false (with an error thrown) if it is not one.
bool read_image_data(Vm& vm, const Value& v, uint32_t* w, uint32_t* h, List<uint32_t>* pixels) {
  if (!v.is_object()) return vm.throw_error(ErrorKind::TypeError, "parameter 1 is not of type 'ImageData'.");
  Value wv, hv, dv;
  if (!vm.get_named(v, "width", &wv) || !vm.get_named(v, "height", &hv) || !vm.get_named(v, "data", &dv)) return false;
  double dw, dh;
  if (!vm.to_number(wv, &dw) || !vm.to_number(hv, &dh)) return false;
  if (!(dw >= 1) || !(dh >= 1) || !is_bytes(dv) || dw * dh > 16.0 * 1024 * 1024) return vm.throw_error(ErrorKind::TypeError, "parameter 1 is not of type 'ImageData'.");
  *w = static_cast<uint32_t>(dw);
  *h = static_cast<uint32_t>(dh);
  ArrayView<uint8_t> d;
  typed_array_bytes(dv, &d);
  if (d.size() < uint64_t{*w} * *h * 4) return vm.throw_error(ErrorKind::TypeError, "parameter 1 is not of type 'ImageData'.");
  if (!pixels->reserve(uint64_t{*w} * *h)) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  for (uint64_t i = 0; i < uint64_t{*w} * *h; ++i)
    pixels->push_back((uint32_t{d[i * 4 + 3]} << 24) | (uint32_t{d[i * 4]} << 16) | (uint32_t{d[i * 4 + 1]} << 8) | d[i * 4 + 2]);
  return true;
}

// --- Base64 and the picture formats toDataURL writes --------------------------------------------------------------------------------

void base64_encode(ConstByteView bytes, String* out) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (uint64_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t b0 = bytes[i], b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0, b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out->push_back(kAlphabet[(v >> 18) & 63]);
    out->push_back(kAlphabet[(v >> 12) & 63]);
    out->push_back(i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=');
    out->push_back(i + 2 < bytes.size() ? kAlphabet[v & 63] : '=');
  }
}

// --- Thenables: what canvas.toBlob() and img.decode() hand back, something that .then() and .catch() work on -------------------------

NATIVE(thenable_then) {
  const Value fn = arg_at(args, argc, 0);
  if (Vm::is_callable(fn)) {
    Value ignored;
    const Value undefined;
    if (!vm.call(fn, Value(), &undefined, 1, &ignored)) return false;
  }
  *result = self;
  return true;
}
NATIVE(thenable_catch) {
  (void)vm;
  (void)args;
  (void)argc;
  *result = self;
  return true;
}
NATIVE(thenable_finally) {
  const Value fn = arg_at(args, argc, 0);
  if (Vm::is_callable(fn)) {
    Value ignored;
    if (!vm.call(fn, Value(), nullptr, 0, &ignored)) return false;
  }
  *result = self;
  return true;
}

Value make_thenable(DomBindings::Impl& c) { return Value::make_object(c.vm.new_object(c.thenable_proto)); }

// A TimeRanges: one range, 0 to `end` (none when `end` is 0).
bool range_index(Vm& vm, const Value& self, uint32_t* index_out, const Value* args, uint32_t argc, const char* function) {
  if (!need_args(vm, argc, 1, function)) return false;
  double index, length;
  Value l;
  if (!arg_number(vm, args, argc, 0, &index) || !vm.get_named(self, "length", &l) || !vm.to_number(l, &length)) return false;
  if (index_out) *index_out = index >= 0 && index < 4294967295.0 ? static_cast<uint32_t>(index) : 0;
  if (!(index >= 0 && index < length)) {
    String m(vm.allocator());
    m.append("Failed to execute '");
    m.append(function);
    m.append("' on 'TimeRanges': The index provided (");
    m.append_uint(index >= 0 && index < 4294967295.0 ? static_cast<uint64_t>(index) : 0);
    m.append(") is greater than or equal to the maximum bound (");
    m.append_uint(static_cast<uint64_t>(length));
    m.append(").");
    return throw_dom(vm, "IndexSizeError", m.view());
  }
  return true;
}
// (the boundaries are properties of their own, __s0, __e0, __s1 ...)
bool range_edge(Vm& vm, const Value& self, const Value* args, uint32_t argc, const char* function, char which, Value* result) {
  uint32_t index = 0;
  if (!range_index(vm, self, &index, args, argc, function)) return false;
  String name(vm.allocator());
  name.append("__");
  name.push_back(which);
  name.append_uint(index);
  return vm.get_named(self, name.view(), result);
}
NATIVE(ranges_start) { return range_edge(vm, self, args, argc, "start", 's', result); }
NATIVE(ranges_end) { return range_edge(vm, self, args, argc, "end", 'e', result); }

// element.error: a MediaError (the code is what MediaError.MEDIA_ERR_* says).
Value make_media_error(DomBindings::Impl& c, uint32_t code) {
  Vm& vm = c.vm;
  JsObject* o = vm.new_object(c.media_error_proto);
  vm.define_named(o, "code", num_value(code));
  vm.define_named(o, "message", str_value(vm, code == 4 ? "MEDIA_ELEMENT_ERROR: Format error" : "MEDIA_ELEMENT_ERROR: Media error"));
  return Value::make_object(o);
}

}  // namespace

Value make_time_ranges(DomBindings::Impl& c, const double* pairs, uint32_t count) {
  Vm& vm = c.vm;
  JsObject* o = vm.new_object(c.time_ranges_proto);
  vm.define_named(o, "length", num_value(count));
  for (uint32_t i = 0; i < count; ++i) {
    String start(vm.allocator()), end(vm.allocator());
    start.append("__s");
    start.append_uint(i);
    end.append("__e");
    end.append_uint(i);
    vm.define(o, vm.intern(start.view()), num_value(pairs[i * 2]), 0);
    vm.define(o, vm.intern(end.view()), num_value(pairs[i * 2 + 1]), 0);
  }
  return Value::make_object(o);
}

namespace {

Value make_ranges(DomBindings::Impl& c, double end) {
  const double pair[2] = {0, end};
  return make_time_ranges(c, pair, end > 0 ? 1 : 0);
}

// --- Element properties --------------------------------------------------------------------------------------------------------------

uint32_t attribute_size(dom::Node* n, const char* name, uint32_t fallback) {
  StringView v;
  uint64_t value;
  if (dom::get_attribute(n, name, &v) && v.trimmed().parse_uint(&value) && value > 0 && value <= CanvasBitmap::kMaxDimension) return static_cast<uint32_t>(value);
  return fallback;
}

}  // namespace

bool media_get(DomBindings::Impl& c, dom::Node* n, StringView k, Value* out) {
  Vm& vm = c.vm;
  if (n->is_tag("img")) {
    if (IS("naturalWidth") || IS("naturalHeight") || IS("complete") || IS("currentSrc") || (IS("width") && !dom::has_attribute(n, "width")) ||
        (IS("height") && !dom::has_attribute(n, "height")) || IS("x") || IS("y")) {
      PageObjects::ImageState st(vm.allocator());
      objs(vm).image_state(n->serial, &st);
      if (IS("naturalWidth") || IS("width")) *out = num_value(st.width);
      else if (IS("naturalHeight") || IS("height")) *out = num_value(st.height);
      else if (IS("complete")) *out = bool_value(st.known ? st.complete : !(dom::has_attribute(n, "src") || dom::has_attribute(n, "srcset")));
      else if (IS("currentSrc")) *out = str_value(vm, st.current_source.view());
      else *out = num_value(0);
      return true;
    }
    return false;
  }
  if (n->is_tag("video") || n->is_tag("audio")) {
    if (IS("loop") || IS("autoplay") || IS("controls") || IS("defaultMuted") || IS("playsInline") || IS("disablePictureInPicture")) {
      const char* attribute = IS("defaultMuted") ? "muted" : IS("playsInline") ? "playsinline" : IS("disablePictureInPicture") ? "disablepictureinpicture" : IS("loop") ? "loop" : IS("autoplay") ? "autoplay" : "controls";
      *out = bool_value(dom::has_attribute(n, attribute));
      return true;
    }
    PageObjects::VideoState st;
    objs(vm).video_state(n->serial, &st);
    if (IS("currentTime")) { *out = num_value(st.current_time); return true; }
    if (IS("duration")) { *out = num_value(st.ready && st.duration >= 0 ? st.duration : nan_value()); return true; }
    if (IS("paused")) { *out = bool_value(st.paused); return true; }
    if (IS("ended")) { *out = bool_value(st.ended); return true; }
    if (IS("seeking")) { *out = bool_value(st.seeking); return true; }
    if (IS("volume")) { *out = num_value(st.volume); return true; }
    if (IS("muted")) { *out = bool_value(st.muted); return true; }
    if (IS("readyState")) { *out = num_value(st.ready_state); return true; }
    if (IS("networkState")) { *out = num_value(st.network_state); return true; }
    if (IS("videoWidth")) { *out = num_value(st.width); return true; }
    if (IS("videoHeight")) { *out = num_value(st.height); return true; }
    if (IS("playbackRate")) { *out = num_value(st.rate); return true; }
    if (IS("defaultPlaybackRate")) { *out = num_value(st.default_rate); return true; }
    if (IS("error")) { *out = st.error_code ? make_media_error(c, st.error_code) : Value::make_null(); return true; }
    if (IS("srcObject")) { *out = Value::make_null(); return true; }
    if (IS("preload")) {
      StringView p;
      const bool given = dom::get_attribute(n, "preload", &p);
      p = p.trimmed();
      *out = str_value(vm, !given ? "metadata" : p.equals_ignore_case("none") ? "none" : p.equals_ignore_case("metadata") ? "metadata" : "auto");
      return true;
    }
    if (IS("preservesPitch")) { *out = bool_value(true); return true; }
    if (st.dynamic && (IS("buffered") || IS("seekable"))) {
      *out = IS("buffered") ? make_time_ranges(c, st.buffered_ranges, st.buffered_count) : make_time_ranges(c, st.seekable_ranges, st.seekable_count);
      return true;
    }
    if (IS("buffered") || IS("seekable")) { *out = make_ranges(c, st.buffered_end); return true; }
    if (IS("played")) { *out = make_ranges(c, st.played_end); return true; }
    if (IS("currentSrc")) {
      StringView src;
      String abs(vm.allocator());
      if (dom::get_attribute(n, "src", &src) && resolve_reference(vm.allocator(), c.host.address(), src, &abs)) *out = str_value(vm, abs.view());
      else *out = str_value(vm, "");
      return true;
    }
    if (IS("videoTracks") || IS("audioTracks") || IS("textTracks")) { *out = Value::make_object(vm.new_array()); return true; }
    return false;
  }
  if (n->is_tag("canvas")) {
    if (IS("width")) { *out = num_value(attribute_size(n, "width", 300)); return true; }
    if (IS("height")) { *out = num_value(attribute_size(n, "height", 150)); return true; }
    return false;
  }
  return false;
}

HostClass::Set media_set(DomBindings::Impl& c, dom::Node* n, StringView k, const Value& value) {
  Vm& vm = c.vm;
  if (n->is_tag("video") || n->is_tag("audio")) {
    if (IS("currentTime")) {
      double d;
      if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
      if (!finite(d)) {
        vm.throw_error(ErrorKind::TypeError, "Failed to set the 'currentTime' property on 'HTMLMediaElement': The provided double value is non-finite.");
        return HostClass::Set::Threw;
      }
      objs(vm).video_seek(n->serial, d);
      return HostClass::Set::Handled;
    }
    if (IS("playbackRate") || IS("defaultPlaybackRate")) {
      double d;
      if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
      if (!finite(d)) {
        vm.throw_error(ErrorKind::TypeError, "The provided double value is non-finite.");
        return HostClass::Set::Threw;
      }
      if (d < 0 || d > 16 || (d > 0 && d < 0.0625)) {   // (what the player can be made to do: 1/16 to 16 times the speed, or stopped)
        throw_dom(vm, "NotSupportedError", "The provided playback rate is not in the supported playback range.");
        return HostClass::Set::Threw;
      }
      if (IS("playbackRate")) objs(vm).video_set_rate(n->serial, d);
      else objs(vm).video_set_default_rate(n->serial, d);
      return HostClass::Set::Handled;
    }
    if (IS("preload")) {
      String text(vm.allocator());
      if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
      dom::set_attribute(n, "preload", text.view());
      return HostClass::Set::Handled;
    }
    if (IS("volume")) {
      double d;
      if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
      if (!(d >= 0 && d <= 1)) {
        throw_dom(vm, "IndexSizeError", "The volume provided is outside the range [0, 1].");
        return HostClass::Set::Threw;
      }
      objs(vm).video_set_volume(n->serial, d);
      return HostClass::Set::Handled;
    }
    if (IS("muted")) {
      objs(vm).video_set_muted(n->serial, Vm::to_boolean(value));
      return HostClass::Set::Handled;
    }
    if (IS("loop") || IS("autoplay") || IS("controls") || IS("defaultMuted") || IS("playsInline")) {
      const char* attribute = IS("defaultMuted") ? "muted" : IS("playsInline") ? "playsinline" : IS("loop") ? "loop" : IS("autoplay") ? "autoplay" : "controls";
      const bool on = Vm::to_boolean(value);
      if (on) dom::set_attribute(n, attribute, "");
      else dom::remove_attribute(n, attribute);
      if (IS("loop")) objs(vm).video_set_loop(n->serial, on);
      return HostClass::Set::Handled;
    }
    if (IS("preservesPitch") || IS("currentSrc") || IS("duration") || IS("paused") || IS("ended") || IS("readyState") || IS("networkState") || IS("seeking") ||
        IS("error") || IS("buffered") || IS("seekable") || IS("played") || IS("videoWidth") || IS("videoHeight"))
      return HostClass::Set::Handled;   // (read-only)
    return HostClass::Set::NotHandled;
  }
  if (n->is_tag("canvas") && (IS("width") || IS("height"))) {
    double d;
    if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
    if (!(d >= 0) || d > CanvasBitmap::kMaxDimension) d = IS("width") ? 300 : 150;
    String text(vm.allocator());
    text.append_uint(static_cast<uint64_t>(d));
    dom::set_attribute(n, IS("width") ? "width" : "height", text.view());
    // a new size is a new, blank canvas
    PageObjects& o = objs(vm);
    if (o.existing_canvas(n->serial)) o.canvas(n->serial, attribute_size(n, "width", 300), attribute_size(n, "height", 150));
    return HostClass::Set::Handled;
  }
  if (n->is_tag("img") && (IS("naturalWidth") || IS("naturalHeight") || IS("complete") || IS("currentSrc"))) return HostClass::Set::Handled;
  return HostClass::Set::NotHandled;
}

namespace {

// --- Element methods --------------------------------------------------------------------------------------------------------------

StringView attr_text(const dom::Node* n, const char* name) {
  StringView v;
  return dom::get_attribute(n, name, &v) ? v.trimmed() : StringView();
}

NATIVE(el_play) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("video") && !n->is_tag("audio")) return vm.throw_error(ErrorKind::TypeError, "play is not a function");
  // The promise is settled at once: fulfilled when the element can play (or will, when its file is there), rejected when it has nothing it could play.
  JsObject* promise = vm.new_promise();
  if (!promise) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  bool has_source = !attr_text(n, "src").empty();
  for (const dom::Node* child = n->first_child; child && !has_source; child = child->next)
    if (child->is_tag("source") && !attr_text(child, "src").empty()) has_source = true;
  if (objs(vm).video_play(n->serial, n->is_tag("audio"), has_source)) {
    vm.promise_fulfill(promise, Value());
  } else {
    throw_dom(vm, "NotSupportedError", "Failed to load because no supported source was found.");
    Value reason;
    vm.take_exception_value(&reason);
    vm.promise_reject(promise, reason);
  }
  *result = Value::make_object(promise);
  return true;
}

NATIVE(el_fast_seek) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  double d;
  if (!need_args(vm, argc, 1, "fastSeek") || !arg_number(vm, args, argc, 0, &d)) return false;
  if (!finite(d)) return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'fastSeek' on 'HTMLMediaElement': The provided double value is non-finite.");
  if (n->is_tag("video") || n->is_tag("audio")) objs(vm).video_seek(n->serial, d);
  return true;
}

NATIVE(el_pause) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (n->is_tag("video") || n->is_tag("audio")) objs(vm).video_pause(n->serial);
  return true;
}

NATIVE(el_load) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (n->is_tag("video") || n->is_tag("audio")) {
    objs(vm).video_load(n->serial);
    n->owner->touch();   // (the page is drawn again, which fetches it)
  }
  return true;
}

NATIVE(el_can_play_type) {
  String type(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &type)) return false;
  const StringView t = type.view();
  dom::Node* n = this_node(vm, self);
  bool maybe = t.starts_with("video/mp4") || t.starts_with("video/quicktime") || t.starts_with("video/x-msvideo") || t.starts_with("image/gif");
  if (n && n->is_tag("audio")) {
    maybe = false;
    for (const char* ok : {"audio/mpeg", "audio/mp3", "audio/mp4", "audio/x-m4a", "audio/aac", "audio/ogg", "audio/flac", "audio/wav", "audio/wave", "audio/x-wav", "audio/aiff",
                           "audio/x-aiff", "audio/basic", "audio/midi", "audio/x-midi"})
      if (t.starts_with(ok)) maybe = true;
  }
  *result = str_value(vm, maybe ? (t.contains("codecs") ? "probably" : "maybe") : "");
  return true;
}

NATIVE(el_decode) {   // img.decode(): the picture is fetched when the page is drawn; nothing to wait for here
  (void)args;
  (void)argc;
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  *result = make_thenable(impl(vm));
  return true;
}

NATIVE(el_capture_stream) { return vm.throw_error(ErrorKind::TypeError, "captureStream is not supported"); }

// --- The 2D context -----------------------------------------------------------------------------------------------------------------

extern const HostClass kContextClass;

CanvasBitmap* context_bitmap(PageObjects& objects, const Value& self) {
  if (!self.is_object() || self.as_object()->host != &kContextClass) return nullptr;
  return objects.existing_canvas(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self.as_object()->host_data)));
}

#define CTX()                                              \
  PageObjects& objects = objs(vm);                         \
  PageObjects::Lock lock(objects);                         \
  CanvasBitmap* bm = context_bitmap(objects, self);        \
  if (!bm) return true;                                    \
  (void)bm;

#define NUM(i, name)                                        \
  double name;                                              \
  if (!arg_number(vm, args, argc, i, &name)) return false;

void format_color(uint32_t argb, String* out) {
  const uint32_t a = argb >> 24;
  static const char kHex[] = "0123456789abcdef";
  if (a == 255) {
    out->push_back('#');
    for (int shift = 20; shift >= 0; shift -= 4) out->push_back(kHex[(argb >> shift) & 15]);
    return;
  }
  out->append("rgba(");
  out->append_uint((argb >> 16) & 0xFF);
  out->append(", ");
  out->append_uint((argb >> 8) & 0xFF);
  out->append(", ");
  out->append_uint(argb & 0xFF);
  out->append(", ");
  const uint32_t m = (a * 1000 + 127) / 255;
  out->append_uint(m / 1000);
  if (m % 1000) {
    out->push_back('.');
    char digits[3] = {static_cast<char>('0' + (m / 100) % 10), static_cast<char>('0' + (m / 10) % 10), static_cast<char>('0' + m % 10)};
    uint32_t n = 3;
    while (n > 0 && digits[n - 1] == '0') --n;
    out->append(StringView(digits, n));
  }
  out->push_back(')');
}

bool parse_color(StringView text, uint32_t* out) {
  text = text.trimmed();
  if (text.equals_ignore_case("currentcolor")) {
    *out = 0xFF000000u;
    return true;
  }
  return parse_css_color_alpha(text, out);
}

// An ImageSource for an <img>, <canvas> or <video> element value.
bool source_pixels(Vm& vm, const Value& v, ImageSource* src) {
  dom::Node* n = node_of(v);
  if (!n || !n->is_element()) return false;
  PageObjects& o = objs(vm);
  if (n->is_tag("img")) return o.image_pixels(n->serial, src);
  if (n->is_tag("video")) return o.video_pixels(n->serial, src);
  if (n->is_tag("canvas")) {
    CanvasBitmap* b = o.existing_canvas(n->serial);
    if (!b) return false;
    *src = b->snapshot();
    return src->valid();
  }
  return false;
}

bool is_source_element(const Value& v) {
  dom::Node* n = node_of(v);
  return n && (n->is_tag("img") || n->is_tag("video") || n->is_tag("canvas"));
}

// Reads a fill or stroke style value into a paint. `*valid` is false when it is not one (the assignment is ignored).
bool read_style(Vm& vm, CanvasBitmap* bm, const Value& v, CanvasPaint* paint, String* normalized, bool* valid) {
  *valid = false;
  DomBindings::Impl& c = impl(vm);
  if (v.is_object()) {
    JsObject* o = v.as_object();
    Value kind;
    if (vm.get_plain(o, key_of(vm, "__gk"), &kind)) {   // a gradient
      Value values, stops;
      if (!vm.get_plain(o, key_of(vm, "__gv"), &values) || !vm.get_plain(o, key_of(vm, "__gs"), &stops) || !values.is_object() || !stops.is_object()) return true;
      CanvasPaint p;
      p.kind = kind.u.number == 0 ? CanvasPaint::Kind::Linear : kind.u.number == 1 ? CanvasPaint::Kind::Radial : CanvasPaint::Kind::Conic;
      double nums[6] = {};
      JsObject* vals = values.as_object();
      for (uint32_t i = 0; i < vals->length && i < 6; ++i) nums[i] = vals->items[i].is_number() ? vals->items[i].u.number : 0;
      if (p.kind == CanvasPaint::Kind::Linear) {
        p.x0 = nums[0]; p.y0 = nums[1]; p.x1 = nums[2]; p.y1 = nums[3];
      } else if (p.kind == CanvasPaint::Kind::Radial) {
        p.x0 = nums[0]; p.y0 = nums[1]; p.r0 = nums[2]; p.x1 = nums[3]; p.y1 = nums[4]; p.r1 = nums[5];
      } else {
        p.r0 = nums[0]; p.x0 = nums[1]; p.y0 = nums[2];
      }
      JsObject* list = stops.as_object();
      for (uint32_t i = 0; i < list->length; ++i) {
        if (!list->items[i].is_object()) continue;
        JsObject* pair = list->items[i].as_object();
        if (pair->length >= 2) p.add_stop(pair->items[0].u.number, static_cast<uint32_t>(pair->items[1].u.number));
      }
      *paint = p;
      *valid = true;
      return true;
    }
    Value source;
    if (vm.get_plain(o, key_of(vm, "__pat"), &source)) {   // a pattern
      ImageSource src;
      if (!source_pixels(vm, source, &src)) return true;
      Value rx, ry;
      vm.get_plain(o, key_of(vm, "__rx"), &rx);
      vm.get_plain(o, key_of(vm, "__ry"), &ry);
      CanvasPaint p;
      p.kind = CanvasPaint::Kind::Pattern;
      p.pattern = bm->add_pattern(src);
      p.repeat_x = Vm::to_boolean(rx);
      p.repeat_y = Vm::to_boolean(ry);
      if (p.pattern == 0) return true;
      *paint = p;
      *valid = true;
      return true;
    }
    (void)c;
  }
  String text(vm.allocator());
  if (!arg_text(vm, v, &text)) return false;
  uint32_t color;
  if (!parse_color(text.view(), &color)) return true;
  CanvasPaint p;
  p.kind = CanvasPaint::Kind::Solid;
  p.color = color;
  *paint = p;
  normalized->clear();
  format_color(color, normalized);
  *valid = true;
  return true;
}

bool is_state_property(StringView k) {
  static const char* const kNames[] = {"fillStyle", "strokeStyle", "lineWidth", "lineCap", "lineJoin", "miterLimit", "lineDashOffset", "globalAlpha", "globalCompositeOperation",
                                       "imageSmoothingEnabled", "textAlign", "textBaseline", "shadowBlur", "shadowOffsetX", "shadowOffsetY", "shadowColor", "font"};
  for (const char* n : kNames)
    if (k.equals(n)) return true;
  return false;
}

bool ctx_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  const StringView k = key->view();
  if (!is_state_property(k)) return false;
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = objects.existing_canvas(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o->host_data)));
  if (!bm) return false;
  const CanvasState& s = bm->state();
  if (IS("fillStyle") || IS("strokeStyle")) {
    const CanvasPaint& paint = IS("fillStyle") ? s.fill : s.stroke;
    Value v;
    if (paint.kind != CanvasPaint::Kind::Solid && vm.get_plain(o, key_of(vm, IS("fillStyle") ? "__fs" : "__ss"), &v)) {
      *out = v;
    } else {
      String text(vm.allocator());
      format_color(paint.color, &text);
      *out = str_value(vm, text.view());
    }
    return true;
  }
  if (IS("lineWidth")) { *out = num_value(s.line_width); return true; }
  if (IS("lineCap")) { *out = str_value(vm, s.line_cap == 1 ? "round" : s.line_cap == 2 ? "square" : "butt"); return true; }
  if (IS("lineJoin")) { *out = str_value(vm, s.line_join == 1 ? "round" : s.line_join == 2 ? "bevel" : "miter"); return true; }
  if (IS("miterLimit")) { *out = num_value(s.miter_limit); return true; }
  if (IS("lineDashOffset")) { *out = num_value(s.dash_offset); return true; }
  if (IS("globalAlpha")) { *out = num_value(s.global_alpha); return true; }
  if (IS("globalCompositeOperation")) { *out = str_value(vm, composite_name(s.composite)); return true; }
  if (IS("imageSmoothingEnabled")) { *out = bool_value(s.smoothing); return true; }
  if (IS("textAlign")) {
    static const char* const kNames[] = {"start", "end", "left", "right", "center"};
    *out = str_value(vm, kNames[s.text_align < 5 ? s.text_align : 0]);
    return true;
  }
  if (IS("textBaseline")) {
    static const char* const kNames[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
    *out = str_value(vm, kNames[s.text_baseline < 6 ? s.text_baseline : 0]);
    return true;
  }
  if (IS("shadowBlur")) { *out = num_value(s.shadow_blur); return true; }
  if (IS("shadowOffsetX")) { *out = num_value(s.shadow_x); return true; }
  if (IS("shadowOffsetY")) { *out = num_value(s.shadow_y); return true; }
  if (IS("shadowColor")) {
    String text(vm.allocator());
    format_color(s.shadow_color, &text);
    if ((s.shadow_color >> 24) == 0) text.assign("rgba(0, 0, 0, 0)");
    *out = str_value(vm, text.view());
    return true;
  }
  if (IS("font")) {
    *out = s.font_text_length ? str_value(vm, StringView(s.font_text, s.font_text_length)) : str_value(vm, "10px sans-serif");
    return true;
  }
  return false;
}

HostClass::Set ctx_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  const StringView k = key->view();
  if (!is_state_property(k) && !k.equals("canvas")) return HostClass::Set::NotHandled;
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = objects.existing_canvas(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o->host_data)));
  if (!bm) return HostClass::Set::NotHandled;
  CanvasState& s = bm->state();
  double d = 0;
  String text(vm.allocator());
  auto number = [&](double* into, bool (*ok)(double)) {
    if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
    if (finite(d) && ok(d)) *into = d;
    return HostClass::Set::Handled;
  };
  if (IS("fillStyle") || IS("strokeStyle")) {
    CanvasPaint p;
    bool valid;
    String normalized(vm.allocator());
    if (!read_style(vm, bm, value, &p, &normalized, &valid)) return HostClass::Set::Threw;
    if (valid) {
      (IS("fillStyle") ? s.fill : s.stroke) = p;
      if (p.kind != CanvasPaint::Kind::Solid) vm.define(o, key_of(vm, IS("fillStyle") ? "__fs" : "__ss"), value, 0);
    }
    return HostClass::Set::Handled;
  }
  if (IS("lineWidth")) return number(&s.line_width, [](double v) { return v > 0; });
  if (IS("miterLimit")) return number(&s.miter_limit, [](double v) { return v > 0; });
  if (IS("lineDashOffset")) return number(&s.dash_offset, [](double) { return true; });
  if (IS("globalAlpha")) return number(&s.global_alpha, [](double v) { return v >= 0 && v <= 1; });
  if (IS("shadowBlur")) return number(&s.shadow_blur, [](double v) { return v >= 0; });
  if (IS("shadowOffsetX")) return number(&s.shadow_x, [](double) { return true; });
  if (IS("shadowOffsetY")) return number(&s.shadow_y, [](double) { return true; });
  if (IS("lineCap") || IS("lineJoin") || IS("globalCompositeOperation") || IS("textAlign") || IS("textBaseline") || IS("shadowColor") || IS("font")) {
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    const StringView t = text.view();
    if (IS("lineCap")) {
      if (t.equals("butt")) s.line_cap = 0;
      else if (t.equals("round")) s.line_cap = 1;
      else if (t.equals("square")) s.line_cap = 2;
    } else if (IS("lineJoin")) {
      if (t.equals("miter")) s.line_join = 0;
      else if (t.equals("round")) s.line_join = 1;
      else if (t.equals("bevel")) s.line_join = 2;
    } else if (IS("globalCompositeOperation")) {
      Composite op;
      if (parse_composite(t, &op)) s.composite = op;
    } else if (IS("textAlign")) {
      static const char* const kNames[] = {"start", "end", "left", "right", "center"};
      for (uint8_t i = 0; i < 5; ++i)
        if (t.equals(kNames[i])) s.text_align = i;
    } else if (IS("textBaseline")) {
      static const char* const kNames[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
      for (uint8_t i = 0; i < 6; ++i)
        if (t.equals(kNames[i])) s.text_baseline = i;
    } else if (IS("shadowColor")) {
      uint32_t color;
      if (parse_color(t, &color)) s.shadow_color = color;
    } else {   // font: "italic bold 16px/20px Arial, sans-serif"
      double size = 0;
      bool bold = false, have_size = false;
      uint64_t i = 0;
      while (i < t.size()) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
        const uint64_t start = i;
        while (i < t.size() && t[i] != ' ' && t[i] != '\t') ++i;
        const StringView word = t.substr(start, i - start);
        if (word.empty()) break;
        if (word.equals_ignore_case("bold") || word.equals_ignore_case("bolder") || word.equals("600") || word.equals("700") || word.equals("800") || word.equals("900")) bold = true;
        else if (!have_size) {
          StringView num = word;
          const uint64_t slash = num.find('/');
          if (slash != StringView::npos) num = num.first(slash);
          double v = 0;
          uint64_t j = 0;
          bool any = false;
          while (j < num.size() && num[j] >= '0' && num[j] <= '9') { v = v * 10 + (num[j++] - '0'); any = true; }
          if (j < num.size() && num[j] == '.') {
            ++j;
            double scale = 0.1;
            while (j < num.size() && num[j] >= '0' && num[j] <= '9') { v += (num[j++] - '0') * scale; scale /= 10; any = true; }
          }
          const StringView unit = num.substr(j);
          if (any && (unit.equals("px") || unit.equals("pt") || unit.equals("em") || unit.equals("rem") || unit.equals("%") || unit.empty() == false)) {
            size = unit.equals("pt") ? v * 4 / 3 : unit.equals("em") || unit.equals("rem") ? v * 16 : unit.equals("%") ? v * 16 / 100 : v;
            have_size = true;
          }
        }
      }
      if (have_size && size > 0) {
        s.font_size = size;
        s.font_bold = bold;
        const uint64_t n = t.size() < sizeof s.font_text ? t.size() : sizeof s.font_text;
        for (uint64_t k = 0; k < n; ++k) s.font_text[k] = t[k];
        s.font_text_length = static_cast<uint8_t>(n);
      }
    }
    return HostClass::Set::Handled;
  }
  if (IS("imageSmoothingEnabled")) {
    s.smoothing = Vm::to_boolean(value);
    return HostClass::Set::Handled;
  }
  if (IS("canvas")) return HostClass::Set::Handled;
  return HostClass::Set::NotHandled;
}

const HostClass kContextClass = {"CanvasRenderingContext2D", ctx_get, ctx_set, nullptr, nullptr};

// --- State and transforms

NATIVE(ctx_save) { CTX(); bm->save(); return true; }
NATIVE(ctx_restore) { CTX(); bm->restore(); return true; }
NATIVE(ctx_reset) { CTX(); bm->reset(); objects.mark_dirty(); return true; }
NATIVE(ctx_is_lost) { (void)self; (void)args; (void)argc; *result = bool_value(false); (void)vm; return true; }

NATIVE(ctx_scale) {
  CTX();
  if (!need_args(vm, argc, 2, "scale")) return false;
  NUM(0, x); NUM(1, y);
  if (finite(x) && finite(y)) bm->transform_by(make_transform(x, 0, 0, y, 0, 0));
  return true;
}
NATIVE(ctx_translate) {
  CTX();
  if (!need_args(vm, argc, 2, "translate")) return false;
  NUM(0, x); NUM(1, y);
  if (finite(x) && finite(y)) bm->transform_by(make_transform(1, 0, 0, 1, x, y));
  return true;
}
NATIVE(ctx_rotate) {
  CTX();
  if (!need_args(vm, argc, 1, "rotate")) return false;
  NUM(0, a);
  if (finite(a)) {
    const double cs = fmath::cos(a), sn = fmath::sin(a);
    bm->transform_by(make_transform(cs, sn, -sn, cs, 0, 0));
  }
  return true;
}
NATIVE(ctx_transform) {
  CTX();
  if (!need_args(vm, argc, 6, "transform")) return false;
  NUM(0, a); NUM(1, b); NUM(2, cc); NUM(3, d); NUM(4, e); NUM(5, f);
  if (finite(a) && finite(b) && finite(cc) && finite(d) && finite(e) && finite(f)) bm->transform_by(make_transform(a, b, cc, d, e, f));
  return true;
}

bool read_matrix(Vm& vm, const Value* args, uint32_t argc, Transform* m) {
  if (argc >= 6) {
    double v[6];
    for (int i = 0; i < 6; ++i)
      if (!vm.to_number(args[i], &v[i])) return false;
    *m = make_transform(v[0], v[1], v[2], v[3], v[4], v[5]);
    return true;
  }
  if (argc >= 1 && args[0].is_object()) {
    double v[6];
    static const char* const kNames[] = {"a", "b", "c", "d", "e", "f"};
    for (int i = 0; i < 6; ++i) {
      Value item;
      if (!vm.get_named(args[0], kNames[i], &item) || !vm.to_number(item, &v[i])) return false;
    }
    *m = make_transform(v[0], v[1], v[2], v[3], v[4], v[5]);
    return true;
  }
  *m = Transform::Identity();
  return true;
}

NATIVE(ctx_set_transform) {
  CTX();
  Transform m = Transform::Identity();
  if (!read_matrix(vm, args, argc, &m)) return false;
  bm->set_transform(m);
  return true;
}
NATIVE(ctx_reset_transform) { CTX(); bm->set_transform(Transform::Identity()); return true; }
NATIVE(ctx_get_transform) {
  CTX();
  const Transform& m = bm->state().ctm;
  JsObject* o = vm.new_plain();
  const double v[6] = {tr_a(m), tr_b(m), tr_c(m), tr_d(m), tr_e(m), tr_f(m)};
  static const char* const kNames[] = {"a", "b", "c", "d", "e", "f"};
  static const char* const kAlt[] = {"m11", "m12", "m21", "m22", "m41", "m42"};
  for (int i = 0; i < 6; ++i) {
    vm.define_named(o, kNames[i], num_value(v[i]));
    vm.define_named(o, kAlt[i], num_value(v[i]));
  }
  vm.define_named(o, "is2D", bool_value(true));
  vm.define_named(o, "isIdentity", bool_value(tr_a(m) == 1 && tr_b(m) == 0 && tr_c(m) == 0 && tr_d(m) == 1 && tr_e(m) == 0 && tr_f(m) == 0));
  *result = Value::make_object(o);
  return true;
}

// --- Gradients and patterns

bool make_gradient(Vm& vm, int kind, std::initializer_list<double> values, Value* result) {
  DomBindings::Impl& c = impl(vm);
  JsObject* g = vm.new_object(c.gradient_proto);
  JsObject* vals = vm.new_array();
  for (double v : values) vm.array_push(vals, num_value(v));
  vm.define(g, key_of(vm, "__gk"), num_value(kind), 0);
  vm.define(g, key_of(vm, "__gv"), Value::make_object(vals), 0);
  vm.define(g, key_of(vm, "__gs"), Value::make_object(vm.new_array()), 0);
  *result = Value::make_object(g);
  return true;
}

NATIVE(ctx_create_linear_gradient) {
  (void)self;
  if (!need_args(vm, argc, 4, "createLinearGradient")) return false;
  double v[4];
  for (int i = 0; i < 4; ++i) {
    if (!arg_number(vm, args, argc, static_cast<uint32_t>(i), &v[i])) return false;
    if (!finite(v[i])) return vm.throw_error(ErrorKind::TypeError, "The provided double value is non-finite.");
  }
  return make_gradient(vm, 0, {v[0], v[1], v[2], v[3]}, result);
}

NATIVE(ctx_create_radial_gradient) {
  (void)self;
  if (!need_args(vm, argc, 6, "createRadialGradient")) return false;
  double v[6];
  for (int i = 0; i < 6; ++i) {
    if (!arg_number(vm, args, argc, static_cast<uint32_t>(i), &v[i])) return false;
    if (!finite(v[i])) return vm.throw_error(ErrorKind::TypeError, "The provided double value is non-finite.");
  }
  if (v[2] < 0 || v[5] < 0) return throw_dom(vm, "IndexSizeError", "The radius provided is negative.");
  return make_gradient(vm, 1, {v[0], v[1], v[2], v[3], v[4], v[5]}, result);
}

NATIVE(ctx_create_conic_gradient) {
  (void)self;
  if (!need_args(vm, argc, 3, "createConicGradient")) return false;
  double v[3];
  for (int i = 0; i < 3; ++i) {
    if (!arg_number(vm, args, argc, static_cast<uint32_t>(i), &v[i])) return false;
    if (!finite(v[i])) return vm.throw_error(ErrorKind::TypeError, "The provided double value is non-finite.");
  }
  return make_gradient(vm, 2, {v[0], v[1], v[2]}, result);
}

NATIVE(gradient_add_color_stop) {
  if (!need_args(vm, argc, 2, "addColorStop")) return false;
  NUM(0, offset);
  if (!(offset >= 0 && offset <= 1)) return throw_dom(vm, "IndexSizeError", "The provided value is outside the range (0.0, 1.0).");
  String text(vm.allocator());
  if (!arg_text(vm, args[1], &text)) return false;
  uint32_t color;
  if (!parse_color(text.view(), &color)) return throw_dom(vm, "SyntaxError", "The value provided could not be parsed as a color.");
  Value stops;
  if (!self.is_object() || !vm.get_plain(self.as_object(), key_of(vm, "__gs"), &stops) || !stops.is_object()) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* pair = vm.new_array();
  vm.array_push(pair, num_value(offset));
  vm.array_push(pair, num_value(color));
  vm.array_push(stops.as_object(), Value::make_object(pair));
  return true;
}

NATIVE(ctx_create_pattern) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  if (!need_args(vm, argc, 1, "createPattern")) return false;
  if (!is_source_element(args[0])) return vm.throw_error(ErrorKind::TypeError, "The provided value is not of type '(CSSImageValue or HTMLImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap)'");
  String rep(vm.allocator());
  if (argc > 1 && !args[1].is_nullish() && !arg_text(vm, args[1], &rep)) return false;
  bool rx = true, ry = true;
  if (rep.empty() || rep.view().equals("repeat")) {
  } else if (rep.view().equals("repeat-x")) {
    ry = false;
  } else if (rep.view().equals("repeat-y")) {
    rx = false;
  } else if (rep.view().equals("no-repeat")) {
    rx = ry = false;
  } else {
    return throw_dom(vm, "SyntaxError", "The repetition mode is not valid.");
  }
  JsObject* p = vm.new_object(c.pattern_proto);
  vm.define(p, key_of(vm, "__pat"), args[0], 0);
  vm.define(p, key_of(vm, "__rx"), bool_value(rx), 0);
  vm.define(p, key_of(vm, "__ry"), bool_value(ry), 0);
  *result = Value::make_object(p);
  return true;
}

// --- Paths --------------------------------------------------------------------------------------------------------------------------
//
// A Path2D keeps what was drawn into it as a list of operations in a property scripts do not see, [[code, numbers...], ...]; drawing with one builds a
// Path from the list.

enum PathOp : int { kOpMove, kOpLine, kOpBezier, kOpQuad, kOpArc, kOpArcTo, kOpEllipse, kOpRect, kOpClose, kOpRoundRect, kOpAdd };

bool is_path2d(Vm& vm, const Value& v) {
  Value ops;
  return v.is_object() && vm.get_plain(v.as_object(), key_of(vm, "__ops"), &ops) && ops.is_object();
}

bool op_numbers(Vm& vm, const Value* args, uint32_t argc, uint32_t n, JsObject* op) {
  for (uint32_t i = 0; i < n; ++i) {
    double d;
    if (!arg_number(vm, args, argc, i, &d)) return false;
    vm.array_push(op, num_value(d));
  }
  return true;
}

// Runs the operations of a Path2D on `path` (an add_path of another Path2D is expanded).
bool build_path(Vm& vm, const Value& path2d, Path* path, uint32_t depth = 0) {
  Value ops;
  if (depth > 8 || !vm.get_plain(path2d.as_object(), key_of(vm, "__ops"), &ops) || !ops.is_object()) return true;
  JsObject* list = ops.as_object();
  for (uint32_t i = 0; i < list->length; ++i) {
    if (!list->items[i].is_object()) continue;
    JsObject* op = list->items[i].as_object();
    if (op->length == 0) continue;
    double v[10] = {};
    for (uint32_t k = 1; k < op->length && k <= 10; ++k) v[k - 1] = op->items[k].is_number() ? op->items[k].u.number : 0;
    switch (static_cast<int>(op->items[0].u.number)) {
      case kOpMove: path->move_to(v[0], v[1]); break;
      case kOpLine: path->line_to(v[0], v[1]); break;
      case kOpBezier: path->bezier_to(v[0], v[1], v[2], v[3], v[4], v[5]); break;
      case kOpQuad: path->quad_to(v[0], v[1], v[2], v[3]); break;
      case kOpArc: path->arc(v[0], v[1], v[2], v[3], v[4], v[5] != 0); break;
      case kOpArcTo: path->arc_to(v[0], v[1], v[2], v[3], v[4]); break;
      case kOpEllipse: path->ellipse(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0); break;
      case kOpRect: path->rect(v[0], v[1], v[2], v[3]); break;
      case kOpRoundRect: path->round_rect(v[0], v[1], v[2], v[3], v[4]); break;
      case kOpClose: path->close(); break;
      case kOpAdd: {   // [kOpAdd, a, b, c, d, e, f, <path2d at the end>]
        const Value inner = op->length > 7 ? op->items[7] : Value();
        if (is_path2d(vm, inner)) {
          Path sub(vm.allocator());
          if (!build_path(vm, inner, &sub, depth + 1)) return false;
          path->add_path(sub, make_transform(v[0], v[1], v[2], v[3], v[4], v[5]));
        }
        break;
      }
      default: break;
    }
  }
  return true;
}

bool path2d_add(Vm& vm, const Value& self, int code, uint32_t numbers, const Value* args, uint32_t argc, const char* name) {
  if (!need_args(vm, argc, numbers, name)) return false;
  Value ops;
  if (!self.is_object() || !vm.get_plain(self.as_object(), key_of(vm, "__ops"), &ops) || !ops.is_object()) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* op = vm.new_array();
  vm.array_push(op, num_value(code));
  if (!op_numbers(vm, args, argc, numbers, op)) return false;
  vm.array_push(ops.as_object(), Value::make_object(op));
  return true;
}

NATIVE(path_move_to) { (void)result; return path2d_add(vm, self, kOpMove, 2, args, argc, "moveTo"); }
NATIVE(path_line_to) { (void)result; return path2d_add(vm, self, kOpLine, 2, args, argc, "lineTo"); }
NATIVE(path_bezier) { (void)result; return path2d_add(vm, self, kOpBezier, 6, args, argc, "bezierCurveTo"); }
NATIVE(path_quad) { (void)result; return path2d_add(vm, self, kOpQuad, 4, args, argc, "quadraticCurveTo"); }
NATIVE(path_arc_to) { (void)result; return path2d_add(vm, self, kOpArcTo, 5, args, argc, "arcTo"); }
NATIVE(path_rect) { (void)result; return path2d_add(vm, self, kOpRect, 4, args, argc, "rect"); }
NATIVE(path_close) { (void)result; return path2d_add(vm, self, kOpClose, 0, args, argc, "closePath"); }

double radius_of(Vm& vm, const Value& v, bool* ok) {
  *ok = true;
  Value first = v;
  if (v.is_object() && v.as_object()->okind == ObjKind::Array) first = v.as_object()->length > 0 ? v.as_object()->items[0] : Value();
  double d = 0;
  if (!vm.to_number(first, &d)) *ok = false;
  return d;
}

NATIVE(path_round_rect) {
  (void)result;
  if (!need_args(vm, argc, 4, "roundRect")) return false;
  Value ops;
  if (!self.is_object() || !vm.get_plain(self.as_object(), key_of(vm, "__ops"), &ops)) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* op = vm.new_array();
  vm.array_push(op, num_value(kOpRoundRect));
  if (!op_numbers(vm, args, argc, 4, op)) return false;
  bool ok = true;
  const double r = argc > 4 ? radius_of(vm, args[4], &ok) : 0;
  if (!ok) return false;
  vm.array_push(op, num_value(r));
  vm.array_push(ops.as_object(), Value::make_object(op));
  return true;
}

NATIVE(path_arc) {
  (void)result;
  if (!need_args(vm, argc, 5, "arc")) return false;
  Value ops;
  if (!self.is_object() || !vm.get_plain(self.as_object(), key_of(vm, "__ops"), &ops)) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* op = vm.new_array();
  vm.array_push(op, num_value(kOpArc));
  if (!op_numbers(vm, args, argc, 5, op)) return false;
  vm.array_push(op, num_value(argc > 5 && Vm::to_boolean(args[5]) ? 1 : 0));
  if (args[2].is_number() && args[2].u.number < 0) return throw_dom(vm, "IndexSizeError", "The radius provided is negative.");
  vm.array_push(ops.as_object(), Value::make_object(op));
  return true;
}

NATIVE(path_ellipse) {
  (void)result;
  if (!need_args(vm, argc, 7, "ellipse")) return false;
  Value ops;
  if (!self.is_object() || !vm.get_plain(self.as_object(), key_of(vm, "__ops"), &ops)) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* op = vm.new_array();
  vm.array_push(op, num_value(kOpEllipse));
  if (!op_numbers(vm, args, argc, 7, op)) return false;
  vm.array_push(op, num_value(argc > 7 && Vm::to_boolean(args[7]) ? 1 : 0));
  vm.array_push(ops.as_object(), Value::make_object(op));
  return true;
}

NATIVE(path_add_path) {
  (void)result;
  if (!need_args(vm, argc, 1, "addPath") || !is_path2d(vm, args[0])) return vm.throw_error(ErrorKind::TypeError, "parameter 1 is not of type 'Path2D'.");
  Value ops;
  if (!self.is_object() || !vm.get_plain(self.as_object(), key_of(vm, "__ops"), &ops)) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  Transform m = Transform::Identity();
  if (argc > 1 && !read_matrix(vm, args + 1, argc - 1, &m)) return false;
  JsObject* op = vm.new_array();
  vm.array_push(op, num_value(kOpAdd));
  const double v[6] = {tr_a(m), tr_b(m), tr_c(m), tr_d(m), tr_e(m), tr_f(m)};
  for (double d : v) vm.array_push(op, num_value(d));
  vm.array_push(op, args[0]);
  vm.array_push(ops.as_object(), Value::make_object(op));
  return true;
}

// --- SVG path data ("M 10 10 L 20 20 C ... Z") for new Path2D(string)

struct SvgReader {
  StringView s;
  uint64_t i = 0;
  void skip() {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool number(double* out) {
    skip();
    const uint64_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    double whole = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { whole = whole * 10 + (s[i++] - '0'); any = true; }
    if (i < s.size() && s[i] == '.') {
      ++i;
      double scale = 0.1;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') { whole += (s[i++] - '0') * scale; scale /= 10; any = true; }
    }
    if (!any) {
      i = start;
      return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      uint64_t j = i + 1;
      bool neg = false;
      if (j < s.size() && (s[j] == '-' || s[j] == '+')) neg = s[j++] == '-';
      int e = 0;
      bool digits = false;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9' && e < 400) { e = e * 10 + (s[j++] - '0'); digits = true; }
      if (digits) {
        i = j;
        double f = 1;
        for (int k = 0; k < e; ++k) f *= 10;
        whole = neg ? whole / f : whole * f;
      }
    }
    *out = (s[start] == '-') ? -whole : whole;
    return true;
  }
  bool flag(double* out) {
    skip();
    if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
      *out = s[i++] - '0';
      return true;
    }
    return false;
  }
};

void svg_op(Vm& vm, JsObject* ops, int code, ArrayView<const double> v) {
  JsObject* op = vm.new_array();
  vm.array_push(op, num_value(code));
  for (uint64_t i = 0; i < v.size(); ++i) vm.array_push(op, num_value(v[i]));
  vm.array_push(ops, Value::make_object(op));
}

// An SVG arc from (x0,y0) to (x1,y1) as an ellipse operation.
void svg_arc(Vm& vm, JsObject* ops, double x0, double y0, double rx, double ry, double phi_deg, double large, double sweep, double x1, double y1) {
  if (rx < 0) rx = -rx;
  if (ry < 0) ry = -ry;
  if (rx == 0 || ry == 0 || (x0 == x1 && y0 == y1)) {
    const double p[2] = {x1, y1};
    svg_op(vm, ops, kOpLine, ArrayView<const double>(p, 2));
    return;
  }
  const double phi = phi_deg * kPi / 180;
  const double cp = fmath::cos(phi), sp = fmath::sin(phi);
  const double dx = (x0 - x1) / 2, dy = (y0 - y1) / 2;
  const double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
  double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1) {
    const double s = fmath::sqrt(lambda);
    rx *= s;
    ry *= s;
  }
  const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
  const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
  double coef = den == 0 ? 0 : fmath::sqrt(num / den > 0 ? num / den : 0);
  if ((large != 0) == (sweep != 0)) coef = -coef;
  const double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
  const double cx = cp * cxp - sp * cyp + (x0 + x1) / 2, cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
  const double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry, vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
  const double theta = canvas_atan2(uy, ux);
  double delta = canvas_atan2(ux * vy - uy * vx, ux * vx + uy * vy);
  if (sweep == 0 && delta > 0) delta -= 2 * kPi;
  if (sweep != 0 && delta < 0) delta += 2 * kPi;
  const double e[8] = {cx, cy, rx, ry, phi, theta, theta + delta, delta < 0 ? 1.0 : 0.0};
  svg_op(vm, ops, kOpEllipse, ArrayView<const double>(e, 8));
}

void parse_svg_path(Vm& vm, StringView data, JsObject* ops) {
  SvgReader r{data};
  double cx = 0, cy = 0, sx = 0, sy = 0, last_cx = 0, last_cy = 0;   // current point, subpath start, last control point
  char command = 0, previous = 0;
  uint32_t guard = 0;
  while (guard++ < 100000) {
    r.skip();
    if (r.i >= data.size()) break;
    const char ch = data[r.i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
      command = ch;
      ++r.i;
      if (command == 'Z' || command == 'z') {
        svg_op(vm, ops, kOpClose, ArrayView<const double>());
        cx = sx;
        cy = sy;
        previous = 'z';
        continue;
      }
    } else if (command == 0 || command == 'Z' || command == 'z') {
      break;
    }
    const bool rel = command >= 'a';
    const char up = static_cast<char>(rel ? command - 32 : command);
    double v[7] = {};
    auto read = [&](uint32_t n) {
      for (uint32_t k = 0; k < n; ++k)
        if (!r.number(&v[k])) return false;
      return true;
    };
    switch (up) {
      case 'M': {
        if (!read(2)) return;
        if (rel) { v[0] += cx; v[1] += cy; }
        svg_op(vm, ops, kOpMove, ArrayView<const double>(v, 2));
        cx = sx = v[0]; cy = sy = v[1];
        command = rel ? 'l' : 'L';
        break;
      }
      case 'L': {
        if (!read(2)) return;
        if (rel) { v[0] += cx; v[1] += cy; }
        svg_op(vm, ops, kOpLine, ArrayView<const double>(v, 2));
        cx = v[0]; cy = v[1];
        break;
      }
      case 'H': {
        if (!read(1)) return;
        v[1] = cy;
        if (rel) v[0] += cx;
        svg_op(vm, ops, kOpLine, ArrayView<const double>(v, 2));
        cx = v[0];
        break;
      }
      case 'V': {
        if (!read(1)) return;
        v[1] = v[0] + (rel ? cy : 0);
        v[0] = cx;
        svg_op(vm, ops, kOpLine, ArrayView<const double>(v, 2));
        cy = v[1];
        break;
      }
      case 'C': {
        if (!read(6)) return;
        if (rel) for (int k = 0; k < 6; k += 2) { v[k] += cx; v[k + 1] += cy; }
        svg_op(vm, ops, kOpBezier, ArrayView<const double>(v, 6));
        last_cx = v[2]; last_cy = v[3];
        cx = v[4]; cy = v[5];
        break;
      }
      case 'S': {
        if (!read(4)) return;
        if (rel) for (int k = 0; k < 4; k += 2) { v[k] += cx; v[k + 1] += cy; }
        const bool smooth = previous == 'C' || previous == 'S';
        const double c1x = smooth ? 2 * cx - last_cx : cx, c1y = smooth ? 2 * cy - last_cy : cy;
        const double b[6] = {c1x, c1y, v[0], v[1], v[2], v[3]};
        svg_op(vm, ops, kOpBezier, ArrayView<const double>(b, 6));
        last_cx = v[0]; last_cy = v[1];
        cx = v[2]; cy = v[3];
        break;
      }
      case 'Q': {
        if (!read(4)) return;
        if (rel) for (int k = 0; k < 4; k += 2) { v[k] += cx; v[k + 1] += cy; }
        svg_op(vm, ops, kOpQuad, ArrayView<const double>(v, 4));
        last_cx = v[0]; last_cy = v[1];
        cx = v[2]; cy = v[3];
        break;
      }
      case 'T': {
        if (!read(2)) return;
        if (rel) { v[0] += cx; v[1] += cy; }
        const bool smooth = previous == 'Q' || previous == 'T';
        const double qx = smooth ? 2 * cx - last_cx : cx, qy = smooth ? 2 * cy - last_cy : cy;
        const double b[4] = {qx, qy, v[0], v[1]};
        svg_op(vm, ops, kOpQuad, ArrayView<const double>(b, 4));
        last_cx = qx; last_cy = qy;
        cx = v[0]; cy = v[1];
        break;
      }
      case 'A': {
        if (!read(3) || !r.flag(&v[3]) || !r.flag(&v[4]) || !r.number(&v[5]) || !r.number(&v[6])) return;
        if (rel) { v[5] += cx; v[6] += cy; }
        svg_arc(vm, ops, cx, cy, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
        cx = v[5]; cy = v[6];
        break;
      }
      default: return;
    }
    previous = up;
  }
}

NATIVE(path2d_constructor) {
  DomBindings::Impl& c = impl(vm);
  (void)self;
  JsObject* o = vm.new_object(c.path2d_proto);
  JsObject* ops = vm.new_array();
  vm.define(o, key_of(vm, "__ops"), Value::make_object(ops), 0);
  const Value a = arg_at(args, argc, 0);
  if (is_path2d(vm, a)) {
    Value other;
    vm.get_plain(a.as_object(), key_of(vm, "__ops"), &other);
    for (uint32_t i = 0; i < other.as_object()->length; ++i) vm.array_push(ops, other.as_object()->items[i]);
  } else if (!a.is_nullish()) {
    String data(vm.allocator());
    if (!arg_text(vm, a, &data)) return false;
    parse_svg_path(vm, data.view(), ops);
  }
  *result = Value::make_object(o);
  return true;
}

// The Path a drawing method works on: a Path2D argument, or the context's current path.
struct PathArg {
  Path* path = nullptr;       // nullptr: the current path
  uint32_t used = 0;          // arguments taken
};

bool read_rule(Vm& vm, const Value& v, bool* even_odd) {
  String rule(vm.allocator());
  if (!arg_text(vm, v, &rule)) return false;
  if (rule.view().equals("evenodd")) *even_odd = true;
  else if (rule.view().equals("nonzero")) *even_odd = false;
  else return vm.throw_error(ErrorKind::TypeError, "The provided value is not a valid enum value of type CanvasFillRule.");
  return true;
}

NATIVE(ctx_begin_path) { CTX(); bm->begin_path(); return true; }
NATIVE(ctx_close_path) { CTX(); bm->path().close(); return true; }
NATIVE(ctx_move_to) {
  CTX();
  if (!need_args(vm, argc, 2, "moveTo")) return false;
  NUM(0, x); NUM(1, y);
  bm->path().move_to(x, y);
  return true;
}
NATIVE(ctx_line_to) {
  CTX();
  if (!need_args(vm, argc, 2, "lineTo")) return false;
  NUM(0, x); NUM(1, y);
  bm->path().line_to(x, y);
  return true;
}
NATIVE(ctx_bezier) {
  CTX();
  if (!need_args(vm, argc, 6, "bezierCurveTo")) return false;
  NUM(0, a); NUM(1, b); NUM(2, cc); NUM(3, d); NUM(4, e); NUM(5, f);
  bm->path().bezier_to(a, b, cc, d, e, f);
  return true;
}
NATIVE(ctx_quad) {
  CTX();
  if (!need_args(vm, argc, 4, "quadraticCurveTo")) return false;
  NUM(0, a); NUM(1, b); NUM(2, cc); NUM(3, d);
  bm->path().quad_to(a, b, cc, d);
  return true;
}
NATIVE(ctx_arc) {
  CTX();
  if (!need_args(vm, argc, 5, "arc")) return false;
  NUM(0, x); NUM(1, y); NUM(2, r); NUM(3, a0); NUM(4, a1);
  if (!bm->path().arc(x, y, r, a0, a1, argc > 5 && Vm::to_boolean(args[5]))) return throw_dom(vm, "IndexSizeError", "The radius provided is negative.");
  return true;
}
NATIVE(ctx_arc_to) {
  CTX();
  if (!need_args(vm, argc, 5, "arcTo")) return false;
  NUM(0, x1); NUM(1, y1); NUM(2, x2); NUM(3, y2); NUM(4, r);
  if (!bm->path().arc_to(x1, y1, x2, y2, r)) return throw_dom(vm, "IndexSizeError", "The radius provided is negative.");
  return true;
}
NATIVE(ctx_ellipse) {
  CTX();
  if (!need_args(vm, argc, 7, "ellipse")) return false;
  NUM(0, x); NUM(1, y); NUM(2, rx); NUM(3, ry); NUM(4, rot); NUM(5, a0); NUM(6, a1);
  if (!bm->path().ellipse(x, y, rx, ry, rot, a0, a1, argc > 7 && Vm::to_boolean(args[7]))) return throw_dom(vm, "IndexSizeError", "The radius provided is negative.");
  return true;
}
NATIVE(ctx_rect) {
  CTX();
  if (!need_args(vm, argc, 4, "rect")) return false;
  NUM(0, x); NUM(1, y); NUM(2, w); NUM(3, h);
  bm->path().rect(x, y, w, h);
  return true;
}
NATIVE(ctx_round_rect) {
  CTX();
  if (!need_args(vm, argc, 4, "roundRect")) return false;
  NUM(0, x); NUM(1, y); NUM(2, w); NUM(3, h);
  bool ok = true;
  const double r = argc > 4 ? radius_of(vm, args[4], &ok) : 0;
  if (!ok) return false;
  if (r < 0) return vm.throw_error(ErrorKind::RangeError, "Radius value must be non-negative.");
  bm->path().round_rect(x, y, w, h, r);
  return true;
}

// fill() / fill(rule) / fill(path) / fill(path, rule); the same for clip()
bool path_and_rule(Vm& vm, const Value* args, uint32_t argc, Path* storage, bool use_ctm_path, const Path** path, bool* even_odd) {
  *path = nullptr;
  *even_odd = false;
  (void)use_ctm_path;
  uint32_t i = 0;
  if (argc > 0 && is_path2d(vm, args[0])) {
    if (!build_path(vm, args[0], storage)) return false;
    *path = storage;
    i = 1;
  }
  if (argc > i && !args[i].is_undefined()) return read_rule(vm, args[i], even_odd);
  return true;
}

NATIVE(ctx_fill) {
  CTX();
  Path storage(vm.allocator());
  const Path* path;
  bool even_odd;
  if (!path_and_rule(vm, args, argc, &storage, false, &path, &even_odd)) return false;
  bm->fill_path(path, even_odd);
  objects.mark_dirty();
  return true;
}
NATIVE(ctx_stroke) {
  CTX();
  Path storage(vm.allocator());
  const Path* path = nullptr;
  if (argc > 0 && is_path2d(vm, args[0])) {
    if (!build_path(vm, args[0], &storage)) return false;
    path = &storage;
  }
  bm->stroke_path(path);
  objects.mark_dirty();
  return true;
}
NATIVE(ctx_clip) {
  CTX();
  Path storage(vm.allocator());
  const Path* path;
  bool even_odd;
  if (!path_and_rule(vm, args, argc, &storage, false, &path, &even_odd)) return false;
  bm->clip_path(path, even_odd);
  return true;
}
NATIVE(ctx_is_point_in_path) {
  CTX();
  Path storage(vm.allocator());
  const Path* path = nullptr;
  uint32_t i = 0;
  if (argc > 0 && is_path2d(vm, args[0])) {
    if (!build_path(vm, args[0], &storage)) return false;
    path = &storage;
    i = 1;
  }
  if (!need_args(vm, argc, i + 2, "isPointInPath")) return false;
  double x, y;
  if (!arg_number(vm, args, argc, i, &x) || !arg_number(vm, args, argc, i + 1, &y)) return false;
  bool even_odd = false;
  if (argc > i + 2 && !args[i + 2].is_undefined() && !read_rule(vm, args[i + 2], &even_odd)) return false;
  *result = bool_value(finite(x) && finite(y) && bm->in_path(path, x, y, even_odd));
  return true;
}
NATIVE(ctx_is_point_in_stroke) {
  CTX();
  Path storage(vm.allocator());
  const Path* path = nullptr;
  uint32_t i = 0;
  if (argc > 0 && is_path2d(vm, args[0])) {
    if (!build_path(vm, args[0], &storage)) return false;
    path = &storage;
    i = 1;
  }
  if (!need_args(vm, argc, i + 2, "isPointInStroke")) return false;
  double x, y;
  if (!arg_number(vm, args, argc, i, &x) || !arg_number(vm, args, argc, i + 1, &y)) return false;
  *result = bool_value(finite(x) && finite(y) && bm->in_stroke(path, x, y));
  return true;
}

// --- Rectangles and text

NATIVE(ctx_fill_rect) {
  CTX();
  if (!need_args(vm, argc, 4, "fillRect")) return false;
  NUM(0, x); NUM(1, y); NUM(2, w); NUM(3, h);
  bm->fill_rect(x, y, w, h);
  objects.mark_dirty();
  return true;
}
NATIVE(ctx_stroke_rect) {
  CTX();
  if (!need_args(vm, argc, 4, "strokeRect")) return false;
  NUM(0, x); NUM(1, y); NUM(2, w); NUM(3, h);
  bm->stroke_rect(x, y, w, h);
  objects.mark_dirty();
  return true;
}
NATIVE(ctx_clear_rect) {
  CTX();
  if (!need_args(vm, argc, 4, "clearRect")) return false;
  NUM(0, x); NUM(1, y); NUM(2, w); NUM(3, h);
  bm->clear_rect(x, y, w, h);
  objects.mark_dirty();
  return true;
}

bool text_call(Vm& vm, const Value& self, const Value* args, uint32_t argc, bool stroke, const char* name) {
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = context_bitmap(objects, self);
  if (!bm) return true;
  if (!need_args(vm, argc, 3, name)) return false;
  String text(vm.allocator());
  if (!arg_text(vm, args[0], &text)) return false;
  NUM(1, x); NUM(2, y);
  double max_width = 1e300;
  if (argc > 3 && !args[3].is_undefined() && !vm.to_number(args[3], &max_width)) return false;
  if (!finite(x) || !finite(y)) return true;
  if (stroke) bm->stroke_text(text.view(), x, y, max_width);
  else bm->fill_text(text.view(), x, y, max_width);
  objects.mark_dirty();
  return true;
}
NATIVE(ctx_fill_text) { (void)result; return text_call(vm, self, args, argc, false, "fillText"); }
NATIVE(ctx_stroke_text) { (void)result; return text_call(vm, self, args, argc, true, "strokeText"); }

NATIVE(ctx_measure_text) {
  CTX();
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  const double w = bm->measure_text(text.view());
  const double fs = bm->state().font_size;
  JsObject* m = vm.new_object(impl(vm).text_metrics_proto);
  vm.define_named(m, "width", num_value(w));
  vm.define_named(m, "actualBoundingBoxLeft", num_value(0));
  vm.define_named(m, "actualBoundingBoxRight", num_value(w));
  vm.define_named(m, "actualBoundingBoxAscent", num_value(fs * 12.0 / 16.0));
  vm.define_named(m, "actualBoundingBoxDescent", num_value(fs * 4.0 / 16.0));
  vm.define_named(m, "fontBoundingBoxAscent", num_value(fs * 12.0 / 16.0));
  vm.define_named(m, "fontBoundingBoxDescent", num_value(fs * 4.0 / 16.0));
  vm.define_named(m, "emHeightAscent", num_value(fs * 12.0 / 16.0));
  vm.define_named(m, "emHeightDescent", num_value(fs * 4.0 / 16.0));
  vm.define_named(m, "alphabeticBaseline", num_value(0));
  *result = Value::make_object(m);
  return true;
}

NATIVE(ctx_set_line_dash) {
  CTX();
  if (!need_args(vm, argc, 1, "setLineDash")) return false;
  CanvasState& s = bm->state();
  if (!args[0].is_object()) return vm.throw_error(ErrorKind::TypeError, "The provided value cannot be converted to a sequence.");
  Value len;
  if (!vm.get(args[0], vm.length_name(), &len)) return false;
  double n;
  if (!vm.to_number(len, &n)) return false;
  double values[CanvasState::kMaxDashes];
  uint32_t count = 0;
  for (uint32_t i = 0; i < static_cast<uint32_t>(n) && i < CanvasState::kMaxDashes; ++i) {
    Value item;
    double d;
    if (!vm.get_member(args[0], num_value(i), &item) || !vm.to_number(item, &d)) return false;
    if (!finite(d) || d < 0) return true;   // (a bad pattern is ignored)
    values[count++] = d;
  }
  for (uint32_t i = 0; i < count; ++i) s.dash[i] = values[i];
  s.dash_count = count;
  return true;
}
NATIVE(ctx_get_line_dash) {
  CTX();
  const CanvasState& s = bm->state();
  JsObject* a = vm.new_array();
  const uint32_t reps = s.dash_count % 2 ? 2 : 1;
  for (uint32_t r = 0; r < reps; ++r)
    for (uint32_t i = 0; i < s.dash_count; ++i) vm.array_push(a, num_value(s.dash[i]));
  *result = Value::make_object(a);
  return true;
}

// --- Images and pixels

NATIVE(ctx_draw_image) {
  CTX();
  if (!need_args(vm, argc, 3, "drawImage")) return false;
  const Value source = args[0];
  if (!is_source_element(source)) return vm.throw_error(ErrorKind::TypeError, "The provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or VideoFrame)'");
  if (argc != 3 && argc != 5 && argc != 9) return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'drawImage': Valid arities are: [3, 5, 9], but 4 arguments provided.");
  double v[8];
  for (uint32_t i = 1; i < argc; ++i) {
    if (!vm.to_number(args[i], &v[i - 1])) return false;
    if (!finite(v[i - 1])) return true;
  }
  ImageSource src;
  if (!source_pixels(vm, source, &src)) {
    dom::Node* n = node_of(source);
    if (n && n->is_tag("img")) {
      PageObjects::ImageState st(vm.allocator());
      objects.image_state(n->serial, &st);
      if (st.broken) return throw_dom(vm, "InvalidStateError", "The HTMLImageElement provided is in the 'broken' state.");
    }
    return true;   // (not there yet: nothing to draw)
  }
  const double iw = src.width, ih = src.height;
  if (argc == 3) bm->draw_image(src, 0, 0, iw, ih, v[0], v[1], iw, ih);
  else if (argc == 5) bm->draw_image(src, 0, 0, iw, ih, v[0], v[1], v[2], v[3]);
  else bm->draw_image(src, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
  objects.mark_dirty();
  return true;
}

JsObject* make_image_data(DomBindings::Impl& c, uint32_t w, uint32_t h, ArrayView<const uint32_t> pixels) {
  Vm& vm = c.vm;
  String bytes(vm.allocator());
  const uint64_t count = uint64_t{w} * h;
  for (uint64_t i = 0; i < count; ++i) {
    const uint32_t p = pixels.empty() ? 0 : pixels[i];
    bytes.push_back(static_cast<char>((p >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((p >> 8) & 0xFF));
    bytes.push_back(static_cast<char>(p & 0xFF));
    bytes.push_back(static_cast<char>(p >> 24));
  }
  JsObject* data = make_bytes(c, bytes.view(), true);
  JsObject* o = vm.new_object(c.image_data_proto);
  vm.define_named(o, "width", num_value(w));
  vm.define_named(o, "height", num_value(h));
  vm.define_named(o, "data", Value::make_object(data));
  vm.define_named(o, "colorSpace", str_value(vm, "srgb"));
  return o;
}

NATIVE(ctx_create_image_data) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  double w, h;
  if (argc == 1 && args[0].is_object()) {
    Value wv, hv;
    if (!vm.get_named(args[0], "width", &wv) || !vm.get_named(args[0], "height", &hv) || !vm.to_number(wv, &w) || !vm.to_number(hv, &h)) return false;
  } else {
    if (!need_args(vm, argc, 2, "createImageData") || !arg_number(vm, args, argc, 0, &w) || !arg_number(vm, args, argc, 1, &h)) return false;
  }
  w = w < 0 ? -w : w;
  h = h < 0 ? -h : h;
  if (!(w >= 1) || !(h >= 1)) return throw_dom(vm, "IndexSizeError", "The source width or height is zero.");
  if (w * h > 8.0 * 1024 * 1024) return vm.throw_error(ErrorKind::RangeError, "The image data is too large.");
  *result = Value::make_object(make_image_data(c, static_cast<uint32_t>(w), static_cast<uint32_t>(h), ArrayView<const uint32_t>()));
  return true;
}

NATIVE(ctx_get_image_data) {
  CTX();
  if (!need_args(vm, argc, 4, "getImageData")) return false;
  NUM(0, x); NUM(1, y); NUM(2, w); NUM(3, h);
  if (!finite(x) || !finite(y) || !finite(w) || !finite(h)) return vm.throw_error(ErrorKind::TypeError, "The provided double value is non-finite.");
  if (w < 0) { x += w; w = -w; }
  if (h < 0) { y += h; h = -h; }
  if (w < 1 || h < 1) return throw_dom(vm, "IndexSizeError", "The source width or height is zero.");
  if (w * h > 8.0 * 1024 * 1024) return vm.throw_error(ErrorKind::RangeError, "The image data is too large.");
  const uint32_t iw = static_cast<uint32_t>(w), ih = static_cast<uint32_t>(h);
  List<uint32_t> pixels(vm.allocator());
  if (!pixels.reserve(uint64_t{iw} * ih)) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  for (uint64_t i = 0; i < uint64_t{iw} * ih; ++i) pixels.push_back(0);
  if (!bm->get_image_data(static_cast<int32_t>(x), static_cast<int32_t>(y), iw, ih, pixels.view())) return true;
  *result = Value::make_object(make_image_data(impl(vm), iw, ih, pixels.view()));
  return true;
}

NATIVE(ctx_put_image_data) {
  CTX();
  if (!need_args(vm, argc, 3, "putImageData")) return false;
  uint32_t w, h;
  List<uint32_t> pixels(vm.allocator());
  if (!read_image_data(vm, args[0], &w, &h, &pixels)) return false;
  NUM(1, dx); NUM(2, dy);
  double dirty[4] = {0, 0, static_cast<double>(w), static_cast<double>(h)};
  if (argc >= 7) {
    for (int i = 0; i < 4; ++i)
      if (!vm.to_number(args[3 + i], &dirty[i])) return false;
  }
  if (!finite(dx) || !finite(dy)) return true;
  ImageSource src;
  src.pixels = pixels.view();
  src.width = w;
  src.height = h;
  src.pitch = w;
  bm->put_image_data(src, static_cast<int32_t>(dx), static_cast<int32_t>(dy), static_cast<int32_t>(dirty[0]), static_cast<int32_t>(dirty[1]), static_cast<int32_t>(dirty[2]),
                     static_cast<int32_t>(dirty[3]));
  objects.mark_dirty();
  return true;
}

// --- <canvas>

NATIVE(canvas_get_context) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  DomBindings::Impl& c = impl(vm);
  if (!n->is_tag("canvas")) return vm.throw_error(ErrorKind::TypeError, "getContext is not a function");
  String kind(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &kind)) return false;
  if (!kind.view().equals("2d")) {   // (no WebGL, no bitmap renderer)
    *result = Value::make_null();
    return true;
  }
  Value existing;
  if (vm.get_plain(self.as_object(), key_of(vm, "__ctx"), &existing) && existing.is_object()) {
    *result = existing;
    return true;
  }
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = objects.canvas(n->serial, attribute_size(n, "width", 300), attribute_size(n, "height", 150));
  if (!bm) {
    *result = Value::make_null();
    return true;
  }
  // attributes: { willReadFrequently, alpha, desynchronized } change nothing here
  JsObject* ctx = vm.new_host_object(c.canvas_context_proto, &kContextClass, reinterpret_cast<void*>(static_cast<uintptr_t>(n->serial)));
  vm.define_named(ctx, "canvas", self, kWritable);
  vm.define(self.as_object(), key_of(vm, "__ctx"), Value::make_object(ctx), 0);
  *result = Value::make_object(ctx);
  return true;
}

bool canvas_data_url(Vm& vm, dom::Node* n, StringView type, double quality, String* out) {
  PageObjects& objects = objs(vm);
  PageObjects::Lock lock(objects);
  CanvasBitmap* bm = objects.canvas(n->serial, attribute_size(n, "width", 300), attribute_size(n, "height", 150));
  if (!bm) return false;
  const ImageSource src = bm->snapshot();
  media::Image image(vm.allocator());
  if (!image.create(src.width, src.height)) return false;
  const ArrayView<uint32_t> frame = image.frame(0);
  const bool jpeg = type.equals_ignore_case("image/jpeg") || type.equals_ignore_case("image/jpg");
  for (uint32_t y = 0; y < src.height; ++y)
    for (uint32_t x = 0; x < src.width; ++x) {
      uint32_t p = src.pixels[uint64_t{y} * src.pitch + x];
      if (jpeg) {   // (no alpha: over black)
        const uint32_t a = p >> 24;
        p = 0xFF000000u | (((((p >> 16) & 0xFF) * a + 127) / 255) << 16) | (((((p >> 8) & 0xFF) * a + 127) / 255) << 8) | (((p & 0xFF) * a + 127) / 255);
      }
      frame[uint64_t{y} * src.width + x] = p;
    }
  image.set_has_alpha(!jpeg);
  ByteBuffer bytes(vm.allocator());
  const char* mime = "image/png";
  bool ok;
  if (jpeg) {
    mime = "image/jpeg";
    const uint32_t q = quality >= 0 && quality <= 1 ? static_cast<uint32_t>(quality * 100) : 92;
    ok = media::encode_jpeg(image, q < 1 ? 1 : q, &bytes);
  } else if (type.equals_ignore_case("image/webp")) {
    mime = "image/webp";
    ok = media::encode_webp(image, &bytes);
  } else if (type.equals_ignore_case("image/bmp")) {
    mime = "image/bmp";
    ok = media::encode_bmp(image, &bytes);
  } else {
    ok = media::encode_png(image, 6, &bytes);
  }
  if (!ok) return false;
  out->assign("data:");
  out->append(mime);
  out->append(";base64,");
  base64_encode(bytes.view(), out);
  return true;
}

NATIVE(canvas_to_data_url) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("canvas")) return vm.throw_error(ErrorKind::TypeError, "toDataURL is not a function");
  String type(vm.allocator());
  if (argc > 0 && !args[0].is_nullish() && !arg_text(vm, args[0], &type)) return false;
  double quality = -1;
  if (argc > 1 && args[1].is_number()) quality = args[1].u.number;
  String url(vm.allocator());
  if (!canvas_data_url(vm, n, type.view(), quality, &url)) {
    *result = str_value(vm, "data:,");
    return true;
  }
  *result = str_value(vm, url.view());
  return true;
}

NATIVE(canvas_to_blob) {   // there are no Blobs: the callback gets an object that says how big the picture is and what it is
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  const Value callback = arg_at(args, argc, 0);
  if (!Vm::is_callable(callback)) return vm.throw_error(ErrorKind::TypeError, "The callback provided as parameter 1 is not a function.");
  String type(vm.allocator());
  if (argc > 1 && !args[1].is_nullish() && !arg_text(vm, args[1], &type)) return false;
  String url(vm.allocator());
  Value blob = Value::make_null();
  if (canvas_data_url(vm, n, type.view(), argc > 2 && args[2].is_number() ? args[2].u.number : -1, &url)) {
    JsObject* b = vm.new_plain();
    const uint64_t semi = url.view().find(';');
    vm.define_named(b, "type", str_value(vm, url.view().substr(5, semi - 5)));
    vm.define_named(b, "size", num_value(static_cast<double>((url.size() - semi - 8) / 4 * 3)));
    vm.define_named(b, "dataURL", str_value(vm, url.view()));
    blob = Value::make_object(b);
  }
  Value ignored;
  (void)result;
  return vm.call(callback, Value(), &blob, 1, &ignored);
}

NATIVE(offscreen_canvas_constructor) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  if (!need_args(vm, argc, 2, "OffscreenCanvas")) return false;
  NUM(0, w); NUM(1, h);
  dom::Node* canvas = c.doc.create_element("canvas");
  if (!canvas) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  String text(vm.allocator());
  text.append_uint(static_cast<uint64_t>(w > 0 ? w : 0));
  dom::set_attribute(canvas, "width", text.view());
  text.clear();
  text.append_uint(static_cast<uint64_t>(h > 0 ? h : 0));
  dom::set_attribute(canvas, "height", text.view());
  *result = node_value(c, canvas);
  return true;
}

}  // namespace

bool install_media(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  JsObject* ep = c.element_proto;
  vm.define_native(ep, "play", el_play, 0);
  vm.define_native(ep, "pause", el_pause, 0);
  vm.define_native(ep, "load", el_load, 0);
  vm.define_native(ep, "fastSeek", el_fast_seek, 1);
  vm.define_native(ep, "canPlayType", el_can_play_type, 1);
  vm.define_native(ep, "decode", el_decode, 0);
  vm.define_native(ep, "captureStream", el_capture_stream, 0);
  vm.define_native(ep, "getContext", canvas_get_context, 1);
  vm.define_native(ep, "toDataURL", canvas_to_data_url, 0);
  vm.define_native(ep, "toBlob", canvas_to_blob, 1);

  c.thenable_proto = vm.new_plain();
  vm.define_native(c.thenable_proto, "then", thenable_then, 1);
  vm.define_native(c.thenable_proto, "catch", thenable_catch, 1);
  vm.define_native(c.thenable_proto, "finally", thenable_finally, 1);
  c.media_error_proto = vm.new_plain();
  {
    JsObject* f = vm.new_native([](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, "MediaError", 0);
    vm.define(f, vm.prototype_name(), Value::make_object(c.media_error_proto), 0);
    vm.define(c.media_error_proto, vm.intern("constructor"), Value::make_object(f), kWritable);
    vm.define_named(g, "MediaError", Value::make_object(f), kWritable);
    const struct { const char* name; uint32_t value; } kCodes[] = {{"MEDIA_ERR_ABORTED", 1}, {"MEDIA_ERR_NETWORK", 2}, {"MEDIA_ERR_DECODE", 3}, {"MEDIA_ERR_SRC_NOT_SUPPORTED", 4}};
    for (const auto& code : kCodes) {
      vm.define_named(f, code.name, num_value(code.value), 0);
      vm.define_named(c.media_error_proto, code.name, num_value(code.value), 0);
    }
  }
  c.time_ranges_proto = vm.new_plain();
  vm.define_native(c.time_ranges_proto, "start", ranges_start, 1);
  vm.define_native(c.time_ranges_proto, "end", ranges_end, 1);

  auto make_class = [&](const char* name, NativeFn ctor, JsObject* proto, uint32_t arity) {
    JsObject* f = vm.new_native(ctor, name, arity);
    vm.define(f, vm.prototype_name(), Value::make_object(proto), 0);
    vm.define(proto, vm.intern("constructor"), Value::make_object(f), kWritable);
    vm.define_named(g, name, Value::make_object(f), kWritable);
    return f;
  };

  c.typed_array_proto = vm.new_plain();   // (the byte arrays themselves are the engine's)

  c.image_data_proto = vm.new_plain();
  make_class("ImageData", image_data_constructor, c.image_data_proto, 2);
  make_class("TimeRanges", [](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, c.time_ranges_proto, 0);
  c.text_metrics_proto = vm.new_plain();
  make_class("TextMetrics", [](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, c.text_metrics_proto, 0);

  // gradients, patterns, paths
  c.gradient_proto = vm.new_plain();
  vm.define_native(c.gradient_proto, "addColorStop", gradient_add_color_stop, 2);
  make_class("CanvasGradient", [](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, c.gradient_proto, 0);
  c.pattern_proto = vm.new_plain();
  make_class("CanvasPattern", [](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, c.pattern_proto, 0);
  c.path2d_proto = vm.new_plain();
  vm.define_native(c.path2d_proto, "moveTo", path_move_to, 2);
  vm.define_native(c.path2d_proto, "lineTo", path_line_to, 2);
  vm.define_native(c.path2d_proto, "bezierCurveTo", path_bezier, 6);
  vm.define_native(c.path2d_proto, "quadraticCurveTo", path_quad, 4);
  vm.define_native(c.path2d_proto, "arc", path_arc, 5);
  vm.define_native(c.path2d_proto, "arcTo", path_arc_to, 5);
  vm.define_native(c.path2d_proto, "ellipse", path_ellipse, 7);
  vm.define_native(c.path2d_proto, "rect", path_rect, 4);
  vm.define_native(c.path2d_proto, "roundRect", path_round_rect, 5);
  vm.define_native(c.path2d_proto, "closePath", path_close, 0);
  vm.define_native(c.path2d_proto, "addPath", path_add_path, 1);
  make_class("Path2D", path2d_constructor, c.path2d_proto, 1);

  // the 2D context
  JsObject* p = c.canvas_context_proto = vm.new_plain();
  struct Method {
    const char* name;
    NativeFn fn;
    uint32_t arity;
  };
  static const Method kMethods[] = {
      {"save", ctx_save, 0}, {"restore", ctx_restore, 0}, {"reset", ctx_reset, 0}, {"isContextLost", ctx_is_lost, 0},
      {"scale", ctx_scale, 2}, {"translate", ctx_translate, 2}, {"rotate", ctx_rotate, 1}, {"transform", ctx_transform, 6}, {"setTransform", ctx_set_transform, 6},
      {"resetTransform", ctx_reset_transform, 0}, {"getTransform", ctx_get_transform, 0},
      {"createLinearGradient", ctx_create_linear_gradient, 4}, {"createRadialGradient", ctx_create_radial_gradient, 6}, {"createConicGradient", ctx_create_conic_gradient, 3},
      {"createPattern", ctx_create_pattern, 2},
      {"beginPath", ctx_begin_path, 0}, {"closePath", ctx_close_path, 0}, {"moveTo", ctx_move_to, 2}, {"lineTo", ctx_line_to, 2}, {"bezierCurveTo", ctx_bezier, 6},
      {"quadraticCurveTo", ctx_quad, 4}, {"arc", ctx_arc, 5}, {"arcTo", ctx_arc_to, 5}, {"ellipse", ctx_ellipse, 7}, {"rect", ctx_rect, 4}, {"roundRect", ctx_round_rect, 5},
      {"fill", ctx_fill, 0}, {"stroke", ctx_stroke, 0}, {"clip", ctx_clip, 0}, {"isPointInPath", ctx_is_point_in_path, 2}, {"isPointInStroke", ctx_is_point_in_stroke, 2},
      {"fillRect", ctx_fill_rect, 4}, {"strokeRect", ctx_stroke_rect, 4}, {"clearRect", ctx_clear_rect, 4},
      {"fillText", ctx_fill_text, 3}, {"strokeText", ctx_stroke_text, 3}, {"measureText", ctx_measure_text, 1},
      {"setLineDash", ctx_set_line_dash, 1}, {"getLineDash", ctx_get_line_dash, 0},
      {"drawImage", ctx_draw_image, 3}, {"createImageData", ctx_create_image_data, 2}, {"getImageData", ctx_get_image_data, 4}, {"putImageData", ctx_put_image_data, 3},
  };
  for (const Method& m : kMethods) vm.define_native(p, m.name, m.fn, m.arity);
  make_class("CanvasRenderingContext2D", [](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, p, 0);
  make_class("OffscreenCanvas", offscreen_canvas_constructor, vm.new_plain(), 2);
  return !vm.halted();
}

JsObject* make_byte_array(DomBindings::Impl& c, StringView bytes) { return make_bytes(c, bytes, false); }

bool byte_array_bytes(const Value& v, StringView* out) {
  ArrayView<uint8_t> bytes;
  if (!is_bytes(v) || !typed_array_bytes(v, &bytes)) return false;
  *out = bytes_as_text(ConstByteView(bytes.data(), bytes.size()));
  return true;
}

}  // namespace bind
}  // namespace browser
