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

// The natives under text tracks: TextTrack, TextTrackList, TextTrackCue, VTTCue, VTTRegion and <track> are written in script (WebPrelude.cpp: the events,
// the modes, loading the file of a <track>); what these functions do is the WebVTT parser and the cue drawing (Media/WebVtt.hpp, Objects.hpp).
// A cue crosses between script and C++ as a plain object: start, end, id, text, vertical (0 horizontal, 1 "rl", 2 "lr"), snap, line (NaN: auto), lineAlign
// (0 start, 1 center, 2 end), position (NaN: auto), positionAlign (0 line-left, 1 center, 2 line-right, 3 auto), size, align (0 start, 1 center, 2 end, 3 left,
// 4 right), pause, region (an id) - and for publishing also show and region as an object (id, width, lines, ax, ay, vx, vy).
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Objects.hpp"
#include "Media/WebVtt.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

PageObjects& objs(Vm& vm) { return impl(vm).host.objects(); }

double nan_value() {
  const uint64_t bits = 0x7FF8000000000000ull;
  double d;
  __builtin_memcpy(&d, &bits, sizeof d);
  return d;
}

bool field(Vm& vm, const Value& o, const char* name, Value* out) { return vm.get_named(o, name, out); }

bool number_field(Vm& vm, const Value& o, const char* name, double* out) {
  Value v;
  return field(vm, o, name, &v) && vm.to_number(v, out);
}

bool text_field(Vm& vm, const Value& o, const char* name, String* out) {
  Value v;
  return field(vm, o, name, &v) && arg_text(vm, v, out);
}

bool flag_field(Vm& vm, const Value& o, const char* name, bool* out) {
  Value v;
  if (!field(vm, o, name, &v)) return false;
  *out = Vm::to_boolean(v);
  return true;
}

// A cue as the plain object script reads.
JsObject* cue_object(Vm& vm, const media::VttCue& cue) {
  JsObject* o = vm.new_plain();
  if (!o) return nullptr;
  vm.define_named(o, "id", str_value(vm, cue.id.view()));
  vm.define_named(o, "start", num_value(cue.start));
  vm.define_named(o, "end", num_value(cue.end));
  vm.define_named(o, "text", str_value(vm, cue.text.view()));
  vm.define_named(o, "region", str_value(vm, cue.region_id.view()));
  vm.define_named(o, "pause", bool_value(cue.pause_on_exit));
  vm.define_named(o, "vertical", num_value(static_cast<double>(cue.vertical)));
  vm.define_named(o, "snap", bool_value(cue.snap_to_lines));
  vm.define_named(o, "line", num_value(cue.line_auto ? nan_value() : cue.line));
  vm.define_named(o, "lineAlign", num_value(static_cast<double>(cue.line_align)));
  vm.define_named(o, "position", num_value(cue.position_auto ? nan_value() : cue.position));
  vm.define_named(o, "positionAlign", num_value(static_cast<double>(cue.position_align)));
  vm.define_named(o, "size", num_value(cue.size));
  vm.define_named(o, "align", num_value(static_cast<double>(cue.align)));
  return o;
}

// __vtt_parse(text): { cues: [...], regions: [{ id, width, lines, ax, ay, vx, vy, scroll }], styles: [text] } - or null if the text is not a WebVTT file.
NATIVE(vtt_parse) {
  (void)self;
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  media::VttFile file(vm.allocator());
  if (!media::parse_webvtt(vm.allocator(), text.view(), &file)) {
    *result = Value::make_null();
    return true;
  }
  JsObject* out = vm.new_plain();
  JsObject* cues = vm.new_array();
  JsObject* regions = vm.new_array();
  JsObject* styles = vm.new_array();
  if (!out || !cues || !regions || !styles) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  for (const auto& c : file.cues.view()) {
    JsObject* o = cue_object(vm, *c);
    if (!o || !vm.array_push(cues, Value::make_object(o))) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  }
  for (const auto& r : file.regions.view()) {
    JsObject* o = vm.new_plain();
    if (!o) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
    vm.define_named(o, "id", str_value(vm, r->id.view()));
    vm.define_named(o, "width", num_value(r->width));
    vm.define_named(o, "lines", num_value(r->lines));
    vm.define_named(o, "ax", num_value(r->anchor_x));
    vm.define_named(o, "ay", num_value(r->anchor_y));
    vm.define_named(o, "vx", num_value(r->viewport_x));
    vm.define_named(o, "vy", num_value(r->viewport_y));
    vm.define_named(o, "scroll", bool_value(r->scroll_up));
    if (!vm.array_push(regions, Value::make_object(o))) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  }
  for (const auto& s : file.styles.view())
    if (!vm.array_push(styles, str_value(vm, s.view()))) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  vm.define_named(out, "cues", Value::make_object(cues));
  vm.define_named(out, "regions", Value::make_object(regions));
  vm.define_named(out, "styles", Value::make_object(styles));
  *result = Value::make_object(out);
  return true;
}

// __vtt_html(cueText): the cue text as HTML (getCueAsHTML()).
NATIVE(vtt_html) {
  (void)self;
  String text(vm.allocator()), html(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  if (!media::vtt_cue_text_to_html(text.view(), &html)) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  *result = str_value(vm, html.view());
  return true;
}

// __vtt_settings(settingsText): a cue object with the settings ("line:10% align:start") applied to a default cue, as WebVTT parsing would.
NATIVE(vtt_settings) {
  (void)self;
  String settings(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &settings)) return false;
  media::VttCue cue(vm.allocator());
  media::apply_vtt_settings(settings.view(), &cue);
  JsObject* o = cue_object(vm, cue);
  if (!o) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  *result = Value::make_object(o);
  return true;
}

// __vtt_time(seconds): "hh:mm:ss.ttt" for a time.
NATIVE(vtt_time) {
  (void)self;
  double seconds;
  if (!vm.to_number(arg_at(args, argc, 0), &seconds)) return false;
  String out(vm.allocator());
  media::format_vtt_timestamp(seconds, &out);
  *result = str_value(vm, out.view());
  return true;
}

// __node_by_serial(serial): the element of the page with that serial number (null if it is gone).
NATIVE(node_by_serial) {
  (void)self;
  double d;
  if (!vm.to_number(arg_at(args, argc, 0), &d)) return false;
  dom::Node* n = d >= 0 && d < 4294967295.0 ? impl(vm).doc.by_serial(static_cast<uint32_t>(d)) : nullptr;
  *result = node_value(impl(vm), n);
  return true;
}

bool read_region(Vm& vm, const Value& o, media::VttRegion* r) {
  double v;
  if (!text_field(vm, o, "id", &r->id)) return false;
  if (!number_field(vm, o, "width", &v)) return false;
  r->width = v;
  if (!number_field(vm, o, "lines", &v)) return false;
  r->lines = v >= 0 && v < 1000 ? static_cast<uint32_t>(v) : 3;
  if (!number_field(vm, o, "ax", &r->anchor_x) || !number_field(vm, o, "ay", &r->anchor_y) || !number_field(vm, o, "vx", &r->viewport_x) ||
      !number_field(vm, o, "vy", &r->viewport_y))
    return false;
  return flag_field(vm, o, "scroll", &r->scroll_up);
}

// __tt_publish(element, cues): the cues of the text tracks that are showing or hidden, in drawing order, as plain objects with `show` and `region` (an object or null).
NATIVE(tt_publish) {
  (void)self;
  dom::Node* element = node_of(arg_at(args, argc, 0));
  if (!element) return vm.throw_error(ErrorKind::TypeError, "The first argument is not an element.");
  const Value list = arg_at(args, argc, 1);
  double length = 0;
  Value lv;
  if (!list.is_object() || !vm.get_named(list, "length", &lv) || !vm.to_number(lv, &length)) return vm.throw_error(ErrorKind::TypeError, "The second argument is not an array.");
  if (!(length >= 0)) length = 0;
  if (length > 20000) length = 20000;
  List<UniquePointer<PageObjects::TrackCue>> cues(vm.allocator());
  for (uint32_t i = 0; i < static_cast<uint32_t>(length); ++i) {
    String name(vm.allocator());
    name.append_uint(i);
    Value o;
    if (!vm.get_named(list, name.view(), &o)) return false;
    auto tc = UniquePointer<PageObjects::TrackCue>::make(vm.allocator(), vm.allocator());
    if (!tc) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
    media::VttCue& c = tc->cue;
    double v;
    if (!flag_field(vm, o, "show", &tc->showing) || !number_field(vm, o, "start", &c.start) || !number_field(vm, o, "end", &c.end) || !text_field(vm, o, "text", &c.text) ||
        !text_field(vm, o, "id", &c.id) || !flag_field(vm, o, "snap", &c.snap_to_lines) || !number_field(vm, o, "size", &c.size))
      return false;
    if (!number_field(vm, o, "vertical", &v)) return false;
    c.vertical = v == 1 ? media::VttVertical::GrowingLeft : v == 2 ? media::VttVertical::GrowingRight : media::VttVertical::Horizontal;
    if (!number_field(vm, o, "line", &c.line)) return false;
    c.line_auto = !(c.line == c.line);
    if (!number_field(vm, o, "lineAlign", &v)) return false;
    c.line_align = v == 1 ? media::VttLineAlign::Center : v == 2 ? media::VttLineAlign::End : media::VttLineAlign::Start;
    if (!number_field(vm, o, "position", &c.position)) return false;
    c.position_auto = !(c.position == c.position);
    if (!number_field(vm, o, "positionAlign", &v)) return false;
    c.position_align = v == 0 ? media::VttPositionAlign::LineLeft : v == 1 ? media::VttPositionAlign::Center : v == 2 ? media::VttPositionAlign::LineRight : media::VttPositionAlign::Auto;
    if (!number_field(vm, o, "align", &v)) return false;
    c.align = v == 0 ? media::VttAlign::Start : v == 2 ? media::VttAlign::End : v == 3 ? media::VttAlign::Left : v == 4 ? media::VttAlign::Right : media::VttAlign::Center;
    Value region;
    if (!vm.get_named(o, "region", &region)) return false;
    if (region.is_object()) {
      if (!read_region(vm, region, &c.region)) return false;
      c.has_region = true;
    }
    if (!cues.push_back(util::move(tc))) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  }
  PageObjects::Lock lock(objs(vm));
  objs(vm).video_set_cues(element->serial, util::move(cues));
  *result = Value();
  return true;
}

}  // namespace

bool install_tracks(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__vtt_parse", vtt_parse, 1);
  vm.define_native(g, "__vtt_html", vtt_html, 1);
  vm.define_native(g, "__vtt_settings", vtt_settings, 2);
  vm.define_native(g, "__vtt_time", vtt_time, 1);
  vm.define_native(g, "__node_by_serial", node_by_serial, 1);
  vm.define_native(g, "__tt_publish", tt_publish, 2);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
