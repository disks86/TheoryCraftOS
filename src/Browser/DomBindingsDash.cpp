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

// The natives under the dashjs-style player of script (WebPrelude.cpp): what the DASH client of an element knows (the qualities there are, which one is
// playing, the buffer, the throughput), and the choices script can make (a held quality, automatic switching).
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Objects.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

PageObjects& objs(Vm& vm) { return impl(vm).host.objects(); }

bool oom(Vm& vm) { return vm.throw_error(ErrorKind::RangeError, "Out of memory"); }

// __dash_info(element): null if the element does not play a DASH manifest, else
// { dynamic, duration, ended, throughput, fetched, switches, streams: [{ kind, auto, quality, buffer, qualities: [{ id, bandwidth, width, height, codecs }] }] }
NATIVE(dash_info) {
  (void)self;
  dom::Node* element = node_of(arg_at(args, argc, 0));
  *result = Value::make_null();
  if (!element) return true;
  PageObjects::Lock lock(objs(vm));
  int64_t playhead = 0;
  media::DashClient* client = objs(vm).video_dash(element->serial, &playhead);
  if (!client || !client->manifest()) return true;
  JsObject* o = vm.new_plain();
  JsObject* streams = vm.new_array();
  if (!o || !streams) return oom(vm);
  const media::DashManifest& m = *client->manifest();
  vm.define_named(o, "dynamic", bool_value(m.dynamic));
  vm.define_named(o, "duration", num_value(m.duration_us >= 0 ? static_cast<double>(m.duration_us) / 1e6 : -1));
  vm.define_named(o, "minBufferTime", num_value(static_cast<double>(m.min_buffer_us) / 1e6));
  vm.define_named(o, "periods", num_value(static_cast<double>(m.periods.count())));
  vm.define_named(o, "ended", bool_value(client->ended()));
  vm.define_named(o, "error", num_value(client->error()));
  vm.define_named(o, "throughput", num_value(static_cast<double>(client->throughput_bps())));
  vm.define_named(o, "fetched", num_value(client->segments_fetched()));
  vm.define_named(o, "switches", num_value(client->switches()));
  for (uint32_t s = 0; s < client->stream_count(); ++s) {
    JsObject* st = vm.new_plain();
    JsObject* qualities = vm.new_array();
    if (!st || !qualities) return oom(vm);
    vm.define_named(st, "kind", str_value(vm, client->stream_kind(s)));
    vm.define_named(st, "auto", bool_value(client->auto_switch(s)));
    vm.define_named(st, "quality", num_value(client->current_quality(s)));
    vm.define_named(st, "buffer", num_value(client->buffer_level(s, playhead)));
    for (uint32_t q = 0; q < client->quality_count(s); ++q) {
      media::DashClient::QualityInfo info(vm.allocator());
      if (!client->quality_info(s, q, &info)) continue;
      JsObject* qo = vm.new_plain();
      if (!qo) return oom(vm);
      vm.define_named(qo, "id", str_value(vm, info.id.view()));
      vm.define_named(qo, "bandwidth", num_value(static_cast<double>(info.bandwidth)));
      vm.define_named(qo, "width", num_value(info.width));
      vm.define_named(qo, "height", num_value(info.height));
      vm.define_named(qo, "codecs", str_value(vm, info.codecs.view()));
      if (!vm.array_push(qualities, Value::make_object(qo))) return oom(vm);
    }
    vm.define_named(st, "qualities", Value::make_object(qualities));
    if (!vm.array_push(streams, Value::make_object(st))) return oom(vm);
  }
  vm.define_named(o, "streams", Value::make_object(streams));
  *result = Value::make_object(o);
  return true;
}

// __dash_set(element, stream, what, value): what 0 holds quality `value`, 1 turns automatic switching on or off. True if it was done.
NATIVE(dash_set) {
  (void)self;
  dom::Node* element = node_of(arg_at(args, argc, 0));
  double stream = 0, what = 0, value = 0;
  if (!vm.to_number(arg_at(args, argc, 1), &stream) || !vm.to_number(arg_at(args, argc, 2), &what)) return false;
  if (what == 0 && !vm.to_number(arg_at(args, argc, 3), &value)) return false;
  *result = bool_value(false);
  if (!element || !(stream >= 0) || stream > 8) return true;
  PageObjects::Lock lock(objs(vm));
  int64_t playhead = 0;
  media::DashClient* client = objs(vm).video_dash(element->serial, &playhead);
  if (!client || static_cast<uint32_t>(stream) >= client->stream_count()) return true;
  if (what == 0) client->set_quality(static_cast<uint32_t>(stream), value >= 0 && value < 1000 ? static_cast<uint32_t>(value) : 0);
  else client->set_auto_switch(static_cast<uint32_t>(stream), Vm::to_boolean(arg_at(args, argc, 3)));
  *result = bool_value(true);
  return true;
}

}  // namespace

bool install_dash(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__dash_info", dash_info, 1);
  vm.define_native(g, "__dash_set", dash_set, 4);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
