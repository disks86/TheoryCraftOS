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

// The natives under the Cache API and service workers. The script side (ServiceWorkerPrelude.cpp) does the matching and the events; these reach the caches of the
// browser (CacheStore) and its service worker hub (ServiceWorker.hpp):
//   __cache_*  the caches of the page's origin (names, entries, writes);
//   __sw_*     what navigator.serviceWorker needs (register, get, update, unregister, postMessage, the controller);
//   __sws_*    what the global scope of a service worker needs (skipWaiting, clients, the answers to the events the hub dispatches).
#include "Browser/CacheStore.hpp"
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/ServiceWorker.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

bool prop_text(Vm& vm, const Value& object, const char* name, String* out) {
  Value v;
  out->clear();
  if (!vm.get_named(object, name, &v)) return false;
  return arg_text(vm, v, out);
}

bool prop_number(Vm& vm, const Value& object, const char* name, double* out) {
  Value v;
  *out = 0;
  if (!vm.get_named(object, name, &v) || v.is_nullish()) return true;
  return vm.to_number(v, out);
}

String origin_of(Vm& vm) {
  String origin(vm.allocator());
  impl(vm).host.page_origin(&origin);
  return origin;
}

// --- Caches ---------------------------------------------------------------------------------------------------------------------

// __cache_names() -> [name ...]
NATIVE(cache_names) {
  (void)self; (void)args; (void)argc;
  List<String> names(vm.allocator());
  const String origin = origin_of(vm);
  impl(vm).host.cache_store().names(origin.view(), &names);
  JsObject* out = vm.new_array();
  for (const String& name : names) vm.array_push(out, str_value(vm, name.view()));
  *result = Value::make_object(out);
  return true;
}
// __cache_has(name), __cache_open(name), __cache_delete(name) -> bool
NATIVE(cache_has) {
  (void)self;
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  const String origin = origin_of(vm);
  *result = bool_value(impl(vm).host.cache_store().has(origin.view(), name.view()));
  return true;
}
NATIVE(cache_open) {
  (void)self;
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  const String origin = origin_of(vm);
  *result = bool_value(impl(vm).host.cache_store().open(origin.view(), name.view()));
  return true;
}
NATIVE(cache_delete) {
  (void)self;
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  const String origin = origin_of(vm);
  *result = bool_value(impl(vm).host.cache_store().remove(origin.view(), name.view()));
  return true;
}
// __cache_count(name) -> number of entries (-1: no such cache)
NATIVE(cache_count) {
  (void)self;
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  const String origin = origin_of(vm);
  CacheStore& store = impl(vm).host.cache_store();
  *result = num_value(store.has(origin.view(), name.view()) ? static_cast<double>(store.count(origin.view(), name.view())) : -1);
  return true;
}
// __cache_entry(name, index, withBody) -> { method, url, requestHeaders, status, statusText, headers, body, responseUrl, responseType, redirected } | null
NATIVE(cache_entry) {
  (void)self;
  String name(vm.allocator());
  double index;
  if (!arg_text(vm, arg_at(args, argc, 0), &name) || !vm.to_number(arg_at(args, argc, 1), &index)) return false;
  const String origin = origin_of(vm);
  CacheStore::Entry e(vm.allocator());
  *result = Value::make_null();
  if (index < 0 || !impl(vm).host.cache_store().read(origin.view(), name.view(), static_cast<uint32_t>(index), &e, Vm::to_boolean(arg_at(args, argc, 2)))) return true;
  JsObject* o = vm.new_plain();
  vm.define_named(o, "method", str_value(vm, e.method.view()));
  vm.define_named(o, "url", str_value(vm, e.url.view()));
  vm.define_named(o, "requestHeaders", str_value(vm, e.request_headers.view()));
  vm.define_named(o, "status", num_value(e.status));
  vm.define_named(o, "statusText", str_value(vm, e.status_text.view()));
  vm.define_named(o, "headers", str_value(vm, e.headers.view()));
  vm.define_named(o, "body", str_value(vm, e.body.view()));
  vm.define_named(o, "responseUrl", str_value(vm, e.response_url.view()));
  vm.define_named(o, "responseType", str_value(vm, e.response_type.view()));
  vm.define_named(o, "redirected", bool_value(e.redirected));
  *result = Value::make_object(o);
  return true;
}
// __cache_write(name, index, entry) -> 0 done, 1 no such cache, 2 over the quota (index -1 adds)
NATIVE(cache_write) {
  (void)self;
  String name(vm.allocator());
  double index;
  const Value entry = arg_at(args, argc, 2);
  if (!arg_text(vm, arg_at(args, argc, 0), &name) || !vm.to_number(arg_at(args, argc, 1), &index)) return false;
  if (!entry.is_object()) return vm.throw_error(ErrorKind::TypeError, "A cache entry is needed");
  CacheStore::Entry e(vm.allocator());
  double status = 200;
  Value redirected;
  if (!prop_text(vm, entry, "method", &e.method) || !prop_text(vm, entry, "url", &e.url) || !prop_text(vm, entry, "requestHeaders", &e.request_headers) ||
      !prop_number(vm, entry, "status", &status) || !prop_text(vm, entry, "statusText", &e.status_text) || !prop_text(vm, entry, "headers", &e.headers) ||
      !prop_text(vm, entry, "body", &e.body) || !prop_text(vm, entry, "responseUrl", &e.response_url) || !prop_text(vm, entry, "responseType", &e.response_type))
    return false;
  e.status = static_cast<uint32_t>(status);
  e.redirected = vm.get_named(entry, "redirected", &redirected) && Vm::to_boolean(redirected);
  const String origin = origin_of(vm);
  bool over = false;
  CacheStore& store = impl(vm).host.cache_store();
  if (!store.has(origin.view(), name.view())) {
    *result = num_value(1);
    return true;
  }
  *result = num_value(store.write(origin.view(), name.view(), static_cast<int64_t>(index), util::move(e), &over) ? 0 : (over ? 2 : 1));
  return true;
}
// __cache_erase(name, index) -> bool
NATIVE(cache_erase) {
  (void)self;
  String name(vm.allocator());
  double index;
  if (!arg_text(vm, arg_at(args, argc, 0), &name) || !vm.to_number(arg_at(args, argc, 1), &index)) return false;
  const String origin = origin_of(vm);
  *result = bool_value(index >= 0 && impl(vm).host.cache_store().erase(origin.view(), name.view(), static_cast<uint32_t>(index)));
  return true;
}
// __cache_usage() -> bytes the origin keeps in its caches
NATIVE(cache_usage) {
  (void)self; (void)args; (void)argc;
  const String origin = origin_of(vm);
  *result = num_value(static_cast<double>(impl(vm).host.cache_store().usage(origin.view())));
  return true;
}

// --- Service workers: the document's side --------------------------------------------------------------------------------------

ServiceWorkerHub* hub_of(Vm& vm) { return impl(vm).host.service_workers(); }

bool unavailable(Vm& vm, Value* result) {
  *result = str_value(vm, "{\"ok\":false,\"name\":\"InvalidStateError\",\"message\":\"Service workers are not available in this document.\"}");
  return true;
}

NATIVE(sw_available) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub = hub_of(vm);
  *result = bool_value(hub && hub->available() && impl(vm).host.service_worker_client() != 0);
  return true;
}
// __sw_register(scriptURL, scope, type, updateViaCache) -> JSON
NATIVE(sw_register) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  if (!hub) return unavailable(vm, result);
  String script(vm.allocator()), scope(vm.allocator()), type(vm.allocator()), via(vm.allocator()), json(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &script) || !arg_text(vm, arg_at(args, argc, 1), &scope) || !arg_text(vm, arg_at(args, argc, 2), &type) || !arg_text(vm, arg_at(args, argc, 3), &via))
    return false;
  hub->api_register(impl(vm).host.service_worker_client(), script.view(), scope.view(), type.view(), via.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_get_registration) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  if (!hub) return unavailable(vm, result);
  String url(vm.allocator()), json(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &url)) return false;
  hub->api_get_registration(impl(vm).host.service_worker_client(), url.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_get_registrations) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub = hub_of(vm);
  String json(vm.allocator());
  if (hub) hub->api_get_registrations(impl(vm).host.service_worker_client(), &json);
  else json.assign("[]");
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_update) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  if (!hub) return unavailable(vm, result);
  double reg;
  String json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  hub->api_update(static_cast<uint32_t>(reg), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_unregister) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  *result = bool_value(hub && hub->api_unregister(static_cast<uint32_t>(reg)));
  return true;
}
NATIVE(sw_registration) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  if (hub) hub->api_registration(static_cast<uint32_t>(reg), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_controller) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub = hub_of(vm);
  String json(vm.allocator());
  if (hub) hub->client_controller(impl(vm).host.service_worker_client(), &json);
  *result = str_value(vm, json.view());
  return true;
}
// __sw_take_event() -> the next thing the hub has to tell this document (JSON), or ""
NATIVE(sw_take_event) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub = hub_of(vm);
  String json(vm.allocator());
  if (hub) hub->client_take_event(impl(vm).host.service_worker_client(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_post) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double worker;
  String wire(vm.allocator()), ports(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &worker) || !arg_text(vm, arg_at(args, argc, 1), &wire) || !arg_text(vm, arg_at(args, argc, 2), &ports)) return false;
  *result = bool_value(hub && hub->api_post(impl(vm).host.service_worker_client(), static_cast<uint32_t>(worker), wire.view(), ports.view()));
  return true;
}
NATIVE(sw_preload) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String op(vm.allocator()), value(vm.allocator()), json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg) || !arg_text(vm, arg_at(args, argc, 1), &op) || !arg_text(vm, arg_at(args, argc, 2), &value)) return false;
  if (hub) hub->api_preload(static_cast<uint32_t>(reg), op.view(), value.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sw_sync_register) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String tag(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg) || !arg_text(vm, arg_at(args, argc, 1), &tag)) return false;
  *result = bool_value(hub && hub->api_sync_register(static_cast<uint32_t>(reg), tag.view()));
  return true;
}
NATIVE(sw_sync_tags) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  if (hub) hub->api_sync_tags(static_cast<uint32_t>(reg), &json);
  else json.assign("[]");
  *result = str_value(vm, json.view());
  return true;
}

// --- Notifications and Push ------------------------------------------------------------------------------------------------------

// __nt_permission() / __nt_request() -> "default", "granted", "denied" ("pending": the user has not answered yet)
NATIVE(nt_permission) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub = hub_of(vm);
  String state(vm.allocator());
  if (hub) hub->notify_permission(impl(vm).host.service_worker_client(), impl(vm).host.service_worker_id(), &state);
  else state.assign("denied");
  *result = str_value(vm, state.view());
  return true;
}
NATIVE(nt_request) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub = hub_of(vm);
  String state(vm.allocator());
  if (hub) hub->notify_request(impl(vm).host.service_worker_client(), impl(vm).host.service_worker_id(), &state);
  else state.assign("denied");
  *result = str_value(vm, state.view());
  return true;
}
// __nt_show(reg, title, body, tag, sticky, renotify, options, actionIds, actionTitles) -> JSON
NATIVE(nt_show) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String title(vm.allocator()), body(vm.allocator()), tag(vm.allocator()), options(vm.allocator()), ids(vm.allocator()), titles(vm.allocator()), json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg) || !arg_text(vm, arg_at(args, argc, 1), &title) || !arg_text(vm, arg_at(args, argc, 2), &body) ||
      !arg_text(vm, arg_at(args, argc, 3), &tag) || !arg_text(vm, arg_at(args, argc, 6), &options) || !arg_text(vm, arg_at(args, argc, 7), &ids) ||
      !arg_text(vm, arg_at(args, argc, 8), &titles))
    return false;
  if (hub) hub->notify_show(impl(vm).host.service_worker_client(), static_cast<uint32_t>(reg), title.view(), body.view(), tag.view(), Vm::to_boolean(arg_at(args, argc, 4)),
                            Vm::to_boolean(arg_at(args, argc, 5)), options.view(), ids.view(), titles.view(), &json);
  else json.assign("{\"ok\":false,\"name\":\"NotAllowedError\",\"message\":\"No notification permission has been granted for this origin.\"}");
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(nt_close) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  *result = bool_value(hub && hub->notify_close(impl(vm).host.service_worker_client(), static_cast<uint32_t>(id)));
  return true;
}
NATIVE(nt_list) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String tag(vm.allocator()), json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg) || !arg_text(vm, arg_at(args, argc, 1), &tag)) return false;
  if (hub) hub->notify_list(static_cast<uint32_t>(reg), tag.view(), &json);
  else json.assign("[]");
  *result = str_value(vm, json.view());
  return true;
}
// __pu_subscribe(reg, userVisibleOnly, keyHex) -> JSON
NATIVE(pu_subscribe) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String key(vm.allocator()), json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg) || !arg_text(vm, arg_at(args, argc, 2), &key)) return false;
  if (hub) hub->push_subscribe(static_cast<uint32_t>(reg), Vm::to_boolean(arg_at(args, argc, 1)), key.view(), &json);
  else json.assign("{\"ok\":false,\"name\":\"NotAllowedError\",\"message\":\"Registration failed - permission denied\"}");
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(pu_get) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String json(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  if (hub) hub->push_get(static_cast<uint32_t>(reg), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(pu_unsubscribe) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  *result = bool_value(hub && hub->push_unsubscribe(static_cast<uint32_t>(reg)));
  return true;
}
NATIVE(pu_state) {
  (void)self;
  ServiceWorkerHub* hub = hub_of(vm);
  double reg;
  String state(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &reg)) return false;
  if (hub) hub->push_state(static_cast<uint32_t>(reg), &state);
  else state.assign("denied");
  *result = str_value(vm, state.view());
  return true;
}

// --- Service workers: the global scope's side ------------------------------------------------------------------------------------

// The worker this page is the scope of, and the hub; false (a TypeError thrown) in a page that is not one.
bool scope_of(Vm& vm, ServiceWorkerHub** hub, uint32_t* worker) {
  *worker = impl(vm).host.service_worker_id();
  *hub = impl(vm).host.service_workers();
  if (*worker == 0 || !*hub) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  return true;
}

NATIVE(sws_id) {
  (void)self; (void)args; (void)argc;
  *result = num_value(impl(vm).host.service_worker_id());
  return true;
}
NATIVE(sws_skip_waiting) {
  (void)self; (void)args; (void)argc; (void)result;
  ServiceWorkerHub* hub;
  uint32_t worker;
  if (!scope_of(vm, &hub, &worker)) return false;
  hub->worker_skip_waiting(worker);
  return true;
}
NATIVE(sws_claim) {
  (void)self; (void)args; (void)argc; (void)result;
  ServiceWorkerHub* hub;
  uint32_t worker;
  if (!scope_of(vm, &hub, &worker)) return false;
  hub->worker_claim(worker);
  return true;
}
// __sws_clients(includeUncontrolled, type) -> JSON array
NATIVE(sws_clients) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String type(vm.allocator()), json(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 1), &type)) return false;
  hub->worker_clients(worker, Vm::to_boolean(arg_at(args, argc, 0)), type.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sws_client) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String id(vm.allocator()), json(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &id)) return false;
  hub->worker_client(worker, id.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sws_client_post) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String id(vm.allocator()), wire(vm.allocator()), ports(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &id) || !arg_text(vm, arg_at(args, argc, 1), &wire) || !arg_text(vm, arg_at(args, argc, 2), &ports)) return false;
  *result = bool_value(hub->worker_client_post(worker, id.view(), wire.view(), ports.view()));
  return true;
}
NATIVE(sws_client_navigate) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String id(vm.allocator()), url(vm.allocator()), json(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &id) || !arg_text(vm, arg_at(args, argc, 1), &url)) return false;
  hub->worker_client_navigate(worker, id.view(), url.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sws_open_window) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String url(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &url)) return false;
  *result = bool_value(hub->worker_open_window(worker, url.view()));
  return true;
}
// __sws_done(token, ok, message): the lifecycle event (or message, sync ...) dispatched under `token` has settled
NATIVE(sws_done) {
  (void)self; (void)result;
  ServiceWorkerHub* hub;
  uint32_t worker;
  double token;
  String message(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !vm.to_number(arg_at(args, argc, 0), &token) || !arg_text(vm, arg_at(args, argc, 2), &message)) return false;
  hub->worker_done(worker, static_cast<uint32_t>(token), Vm::to_boolean(arg_at(args, argc, 1)), message.view());
  return true;
}
// __sws_answer(token, kind, status, statusText, headers, body, url, type, redirected): the answer to a fetch event
NATIVE(sws_answer) {
  (void)self; (void)result;
  ServiceWorkerHub* hub;
  uint32_t worker;
  double token, kind, status;
  if (!scope_of(vm, &hub, &worker)) return false;
  ServiceWorkerHub::FetchAnswer answer(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &token) || !vm.to_number(arg_at(args, argc, 1), &kind) || !vm.to_number(arg_at(args, argc, 2), &status) ||
      !arg_text(vm, arg_at(args, argc, 3), &answer.status_text) || !arg_text(vm, arg_at(args, argc, 4), &answer.headers) || !arg_text(vm, arg_at(args, argc, 5), &answer.body) ||
      !arg_text(vm, arg_at(args, argc, 6), &answer.url) || !arg_text(vm, arg_at(args, argc, 7), &answer.type))
    return false;
  answer.kind = static_cast<uint32_t>(kind);
  answer.status = static_cast<uint32_t>(status);
  answer.redirected = Vm::to_boolean(arg_at(args, argc, 8));
  hub->worker_answer(worker, static_cast<uint32_t>(token), util::move(answer));
  return true;
}
// __sws_import(url) -> the source, or null
NATIVE(sws_import) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String url(vm.allocator()), source(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &url)) return false;
  *result = hub->worker_import(worker, url.view(), &source) ? str_value(vm, source.view()) : Value::make_null();
  return true;
}
NATIVE(sws_registration) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String json(vm.allocator());
  if (!scope_of(vm, &hub, &worker)) return false;
  hub->worker_registration(worker, &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sws_update) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String json(vm.allocator());
  if (!scope_of(vm, &hub, &worker)) return false;
  hub->worker_update(worker, &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sws_unregister) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub;
  uint32_t worker;
  if (!scope_of(vm, &hub, &worker)) return false;
  *result = bool_value(hub->worker_unregister(worker));
  return true;
}
NATIVE(sws_sync_register) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String tag(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &tag)) return false;
  *result = bool_value(hub->worker_sync_register(worker, tag.view()));
  return true;
}
NATIVE(sws_sync_tags) {
  (void)self; (void)args; (void)argc;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String json(vm.allocator());
  if (!scope_of(vm, &hub, &worker)) return false;
  hub->worker_sync_tags(worker, &json);
  *result = str_value(vm, json.view());
  return true;
}
NATIVE(sws_preload) {
  (void)self;
  ServiceWorkerHub* hub;
  uint32_t worker;
  String op(vm.allocator()), value(vm.allocator()), json(vm.allocator());
  if (!scope_of(vm, &hub, &worker) || !arg_text(vm, arg_at(args, argc, 0), &op) || !arg_text(vm, arg_at(args, argc, 1), &value)) return false;
  hub->worker_preload(worker, op.view(), value.view(), &json);
  *result = str_value(vm, json.view());
  return true;
}

}  // namespace

bool install_service_worker(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__cache_names", cache_names, 0);
  vm.define_native(g, "__cache_has", cache_has, 1);
  vm.define_native(g, "__cache_open", cache_open, 1);
  vm.define_native(g, "__cache_delete", cache_delete, 1);
  vm.define_native(g, "__cache_count", cache_count, 1);
  vm.define_native(g, "__cache_entry", cache_entry, 3);
  vm.define_native(g, "__cache_write", cache_write, 3);
  vm.define_native(g, "__cache_erase", cache_erase, 2);
  vm.define_native(g, "__cache_usage", cache_usage, 0);
  vm.define_native(g, "__sw_available", sw_available, 0);
  vm.define_native(g, "__sw_register", sw_register, 4);
  vm.define_native(g, "__sw_get_registration", sw_get_registration, 1);
  vm.define_native(g, "__sw_get_registrations", sw_get_registrations, 0);
  vm.define_native(g, "__sw_update", sw_update, 1);
  vm.define_native(g, "__sw_unregister", sw_unregister, 1);
  vm.define_native(g, "__sw_registration", sw_registration, 1);
  vm.define_native(g, "__sw_controller", sw_controller, 0);
  vm.define_native(g, "__sw_post", sw_post, 3);
  vm.define_native(g, "__sw_take_event", sw_take_event, 0);
  vm.define_native(g, "__sw_preload", sw_preload, 3);
  vm.define_native(g, "__sw_sync_register", sw_sync_register, 2);
  vm.define_native(g, "__sw_sync_tags", sw_sync_tags, 1);
  vm.define_native(g, "__nt_permission", nt_permission, 0);
  vm.define_native(g, "__nt_request", nt_request, 0);
  vm.define_native(g, "__nt_show", nt_show, 9);
  vm.define_native(g, "__nt_close", nt_close, 1);
  vm.define_native(g, "__nt_list", nt_list, 2);
  vm.define_native(g, "__pu_subscribe", pu_subscribe, 3);
  vm.define_native(g, "__pu_get", pu_get, 1);
  vm.define_native(g, "__pu_unsubscribe", pu_unsubscribe, 1);
  vm.define_native(g, "__pu_state", pu_state, 1);
  vm.define_native(g, "__sws_id", sws_id, 0);
  vm.define_native(g, "__sws_skip_waiting", sws_skip_waiting, 0);
  vm.define_native(g, "__sws_claim", sws_claim, 0);
  vm.define_native(g, "__sws_clients", sws_clients, 2);
  vm.define_native(g, "__sws_client", sws_client, 1);
  vm.define_native(g, "__sws_client_post", sws_client_post, 3);
  vm.define_native(g, "__sws_client_navigate", sws_client_navigate, 2);
  vm.define_native(g, "__sws_open_window", sws_open_window, 1);
  vm.define_native(g, "__sws_done", sws_done, 3);
  vm.define_native(g, "__sws_answer", sws_answer, 9);
  vm.define_native(g, "__sws_import", sws_import, 1);
  vm.define_native(g, "__sws_registration", sws_registration, 0);
  vm.define_native(g, "__sws_update", sws_update, 0);
  vm.define_native(g, "__sws_unregister", sws_unregister, 0);
  vm.define_native(g, "__sws_sync_register", sws_sync_register, 1);
  vm.define_native(g, "__sws_sync_tags", sws_sync_tags, 0);
  vm.define_native(g, "__sws_preload", sws_preload, 2);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
