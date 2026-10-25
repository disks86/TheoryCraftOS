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

// The natives under RTCPeerConnection (the script side is in WebPrelude.cpp):
//   __rtc_available() -> bool
//   __rtc_create(iceServers) -> id (0: not made, the reason in __rtc_error)
//   __rtc_op(id, name, a, b, bytes, n1, n2) -> "ok:<result>" or "err:<Name: message>"
//   __rtc_release(id)
#include "Browser/DomBindingsInternal.hpp"
#include "Script/TypedArray.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

NATIVE(rtc_available) {
  (void)self; (void)args; (void)argc;
  *result = bool_value(impl(vm).host.rtc_available());
  return true;
}

NATIVE(rtc_create) {
  (void)self;
  String servers(vm.allocator()), error(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &servers)) return false;
  const uint32_t id = impl(vm).host.rtc_create(servers.view(), &error);
  if (id == 0) {
    Value text = str_value(vm, error.view());
    vm.define_named(vm.global(), "__rtc_error", text, 0);
  }
  *result = num_value(id);
  return true;
}

NATIVE(rtc_op) {
  (void)self;
  double id, n1 = 0, n2 = 0;
  String name(vm.allocator()), a(vm.allocator()), b(vm.allocator()), out(vm.allocator()), reply(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &id) || !arg_text(vm, arg_at(args, argc, 1), &name) || !arg_text(vm, arg_at(args, argc, 2), &a) || !arg_text(vm, arg_at(args, argc, 3), &b)) return false;
  ConstByteView bytes;
  const Value data = arg_at(args, argc, 4);
  if (!data.is_nullish()) {
    ArrayView<uint8_t> view;
    if (!script::typed_array_bytes(data, &view)) return vm.throw_error(ErrorKind::TypeError, "The data of an RTC operation must be a Uint8Array");
    bytes = ConstByteView(view.data(), view.size());
  }
  if (!vm.to_number(arg_at(args, argc, 5), &n1) || !vm.to_number(arg_at(args, argc, 6), &n2)) return false;
  const bool ok = impl(vm).host.rtc_op(static_cast<uint32_t>(id), name.view(), a.view(), b.view(), bytes, static_cast<int64_t>(n1), static_cast<int64_t>(n2), &out);
  reply.assign(ok ? "ok:" : "err:");
  reply.append(out.view());
  *result = str_value(vm, reply.view());
  return true;
}

NATIVE(rtc_release) {
  (void)self;
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  impl(vm).host.rtc_release(static_cast<uint32_t>(id));
  *result = Value();
  return true;
}

}  // namespace

bool install_rtc(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__rtc_available", rtc_available, 0);
  vm.define_native(g, "__rtc_create", rtc_create, 1);
  vm.define_native(g, "__rtc_op", rtc_op, 7);
  vm.define_native(g, "__rtc_release", rtc_release, 1);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
