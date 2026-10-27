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

// The window: location, history, storage, matchMedia, getComputedStyle, animation frames, scrolling, the dialogs, navigator, screen, performance, base64 and URI
// coding, console extras, and the observers pages create (which never report).
#include <initializer_list>
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Url.hpp"
#include "Browser/WebUrl.hpp"
#include "Script/TypedArray.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)
#define IS(text) k.equals(text)

bool resolve_url(Vm& vm, DomBindings::Impl& c, StringView url, String* out) {
  if (resolve_reference(vm.allocator(), c.host.address(), url, out)) return true;
  out->assign(url);
  return true;
}

// --- location --------------------------------------------------------------------------------------------------------------------

bool location_get(Vm& vm, JsObject*, JsString* key, Value* out) {
  DomBindings::Impl& c = impl(vm);
  const StringView k = key->view();
  if (!(IS("href") || IS("protocol") || IS("host") || IS("hostname") || IS("port") || IS("pathname") || IS("search") || IS("hash") || IS("origin"))) return false;
  AddressParts p(vm.allocator());
  split_address(c.host.address(), &p);
  if (IS("href")) *out = str_value(vm, c.host.address());
  else if (IS("protocol")) *out = str_value(vm, p.protocol.view());
  else if (IS("host")) *out = str_value(vm, p.host.view());
  else if (IS("hostname")) *out = str_value(vm, p.hostname.view());
  else if (IS("port")) *out = str_value(vm, p.port.view());
  else if (IS("pathname")) *out = str_value(vm, p.pathname.view());
  else if (IS("search")) *out = str_value(vm, p.search.view());
  else if (IS("hash")) *out = str_value(vm, p.hash.view());
  else *out = str_value(vm, p.origin.view());
  return true;
}

HostClass::Set location_set(Vm& vm, JsObject*, JsString* key, const Value& value) {
  DomBindings::Impl& c = impl(vm);
  const StringView k = key->view();
  if (!(IS("href") || IS("protocol") || IS("host") || IS("hostname") || IS("port") || IS("pathname") || IS("search") || IS("hash"))) return HostClass::Set::NotHandled;
  String text(vm.allocator());
  if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
  if (IS("href")) {
    String url(vm.allocator());
    resolve_url(vm, c, text.view(), &url);
    c.host.navigate(url.view(), false);
    return HostClass::Set::Handled;
  }
  if (IS("hash")) {
    String hash(vm.allocator());
    if (!text.empty() && !text.view().starts_with('#')) hash.push_back('#');
    hash.append(text.view());
    c.host.set_hash(hash.view());
    return HostClass::Set::Handled;
  }
  AddressParts p(vm.allocator());
  split_address(c.host.address(), &p);
  String url(vm.allocator());
  if (IS("pathname")) {
    url.append(p.origin.view());
    if (!text.view().starts_with('/')) url.push_back('/');
    url.append(text.view());
    url.append(p.search.view());
  } else if (IS("search")) {
    url.append(p.origin.view());
    url.append(p.pathname.view());
    if (!text.empty() && !text.view().starts_with('?')) url.push_back('?');
    url.append(text.view());
  } else {
    // protocol, host, hostname, port: rebuilt from the parts
    String protocol(vm.allocator()), host(vm.allocator());
    protocol.assign(p.protocol.view());
    host.assign(p.host.view());
    if (IS("protocol")) {
      protocol.assign(text.view());
      if (!protocol.view().ends_with(':')) protocol.push_back(':');
    } else if (IS("host")) {
      host.assign(text.view());
    } else if (IS("hostname")) {
      host.assign(text.view());
      if (!p.port.empty()) {
        host.push_back(':');
        host.append(p.port.view());
      }
    } else {
      host.assign(p.hostname.view());
      if (!text.empty()) {
        host.push_back(':');
        host.append(text.view());
      }
    }
    url.append(protocol.view());
    url.append("//");
    url.append(host.view());
    url.append(p.pathname.view());
    url.append(p.search.view());
  }
  c.host.navigate(url.view(), false);
  return HostClass::Set::Handled;
}

const HostClass kLocationClass = {"Location", location_get, location_set, nullptr, nullptr};

NATIVE(location_assign) {
  DomBindings::Impl& c = impl(vm);
  String text(vm.allocator()), url(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  resolve_url(vm, c, text.view(), &url);
  c.host.navigate(url.view(), false);
  return true;
}

NATIVE(location_replace) {
  DomBindings::Impl& c = impl(vm);
  String text(vm.allocator()), url(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  resolve_url(vm, c, text.view(), &url);
  c.host.navigate(url.view(), true);
  return true;
}

NATIVE(location_reload) {
  impl(vm).host.reload();
  return true;
}

NATIVE(location_to_string) {
  *result = str_value(vm, impl(vm).host.address());
  return true;
}

// --- history ---------------------------------------------------------------------------------------------------------------------

// history.state: the state of the entry the page is on, parsed from its JSON text (the same object each time until the entry changes).
bool history_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  if (key->view().equals("length")) {
    *out = num_value(impl(vm).host.history_length());
    return true;
  }
  if (!key->view().equals("state")) return false;
  String json(vm.allocator());
  if (!impl(vm).host.history_state(&json)) {
    *out = Value::make_null();
    return true;
  }
  Value cached_text, cached_value;
  if (vm.get_plain(o, vm.intern("__sj"), &cached_text) && cached_text.is_string() && cached_text.as_string()->view().equals(json.view()) && vm.get_plain(o, vm.intern("__sv"), &cached_value)) {
    *out = cached_value;
    return true;
  }
  Value parse, parsed;
  const Value text = str_value(vm, json.view());
  if (!vm.get_named(Value::make_object(vm.global()), "__deserialize", &parse) || !Vm::is_callable(parse) || !vm.call(parse, Value(), &text, 1, &parsed)) {
    if (vm.has_exception()) {
      String ignored(vm.allocator());
      vm.take_exception(&ignored);
    }
    parsed = Value::make_null();
  }
  vm.define(o, vm.intern("__sj"), text, kWritable);
  vm.define(o, vm.intern("__sv"), parsed, kWritable);
  *out = parsed;
  return true;
}

const HostClass kHistoryClass = {"History", history_get, nullptr, nullptr, nullptr};

NATIVE(history_back) {
  impl(vm).host.history_go(-1);
  return true;
}
NATIVE(history_forward) {
  impl(vm).host.history_go(1);
  return true;
}
NATIVE(history_go) {
  double d = 0;
  if (argc > 0 && !vm.to_number(args[0], &d)) return false;
  if (d != d) d = 0;
  if (d == 0) impl(vm).host.reload();
  else impl(vm).host.history_go(static_cast<int32_t>(d));
  return true;
}

// pushState(state, title, url) and replaceState: the state is serialized (to text, so that the entry can outlive the page and the state can hold dates, nested
// objects ...), the address must be of the same origin as the page.
bool state_impl(Vm& vm, const Value& self, const Value* args, uint32_t argc, bool replace) {
  DomBindings::Impl& c = impl(vm);
  if (!self.is_object()) return true;
  Value serialize, text;
  const Value state = arg_at(args, argc, 0);
  bool has_state = !state.is_undefined();
  String json(vm.allocator());
  if (has_state) {
    if (!vm.get_named(Value::make_object(vm.global()), "__serialize", &serialize) || !Vm::is_callable(serialize)) return vm.throw_error(ErrorKind::TypeError, "history state cannot be saved");
    if (!vm.call(serialize, Value(), &state, 1, &text)) return false;
    if (!arg_text(vm, text, &json)) return false;
    if (json.size() > (2u << 20)) return throw_dom(vm, "DataCloneError", "Failed to execute 'pushState' on 'History': The state object is too large.");
  }
  String resolved(vm.allocator());
  const Value url = arg_at(args, argc, 2);
  if (!url.is_nullish()) {
    String target(vm.allocator());
    if (!arg_text(vm, url, &target)) return false;
    WebUrl base(vm.allocator()), next(vm.allocator());
    if (WebUrl::parse(vm.allocator(), c.host.address(), nullptr, &base)) {
      if (!WebUrl::parse(vm.allocator(), target.view(), &base, &next)) return throw_dom(vm, "SecurityError", "Failed to execute 'pushState' on 'History': A history state object with URL cannot be created.");
      const bool same_origin = base.is_file() ? (next.is_file() && base.path.equals(next.path.view())) : base.same_origin(next);
      if (!same_origin || !next.username.equals(base.username.view()) || !next.password.equals(base.password.view()))
        return throw_dom(vm, "SecurityError", "Failed to execute 'pushState' on 'History': A history state object with a URL of another origin cannot be created.");
      next.serialize(&resolved);
    } else {
      resolve_url(vm, c, target.view(), &resolved);
    }
  } else {
    resolved.assign(c.host.address());
  }
  c.host.push_state(resolved.view(), json.view(), has_state, replace);
  return true;
}
NATIVE(history_push_state) { return state_impl(vm, self, args, argc, false); }
NATIVE(history_replace_state) { return state_impl(vm, self, args, argc, true); }

// --- storage ---------------------------------------------------------------------------------------------------------------------

StorageArea* area_of(JsObject* o) { return static_cast<StorageArea*>(o->host_data); }

bool storage_method(StringView k) { return IS("getItem") || IS("setItem") || IS("removeItem") || IS("clear") || IS("key") || IS("constructor") || IS("toString"); }

// Tells the other pages of the origin that a key changed (key null: the whole area was cleared).
void storage_notify(Vm& vm, JsObject* o, const String* key, const String* old_value, const String* new_value) {
  DomBindings::Impl& c = impl(vm);
  c.host.storage_changed(o == c.session_storage, key, old_value, new_value);
}

// Sets a value and reports the change, if it is one. False after throwing (the quota).
bool storage_put(Vm& vm, JsObject* o, StringView key, StringView value) {
  StorageArea* a = area_of(o);
  StringView existing;
  String old_copy(vm.allocator()), key_copy(vm.allocator()), value_copy(vm.allocator());
  const bool had = a->get(key, &existing);
  if (had) old_copy.assign(existing);
  if (had && old_copy.view().equals(value)) return true;
  if (!a->set(key, value)) return throw_dom(vm, "QuotaExceededError", "Failed to execute 'setItem' on 'Storage': Setting the value exceeded the quota.");
  key_copy.assign(key);
  value_copy.assign(value);
  storage_notify(vm, o, &key_copy, had ? &old_copy : nullptr, &value_copy);
  return true;
}

bool storage_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  const StringView k = key->view();
  if (k.empty() || storage_method(k) || (k.size() > 1 && k[0] == '_' && k[1] == '_')) return false;
  if (IS("length")) {
    *out = num_value(area_of(o)->count());
    return true;
  }
  StringView v;
  if (area_of(o)->get(k, &v)) {
    *out = str_value(vm, v);
    return true;
  }
  return false;
}

HostClass::Set storage_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  const StringView k = key->view();
  if (k.empty() || storage_method(k) || IS("length") || (k.size() > 1 && k[0] == '_' && k[1] == '_')) return HostClass::Set::NotHandled;
  String text(vm.allocator());
  if (!arg_text(vm, value.is_undefined() ? str_value(vm, "undefined") : value, &text)) return HostClass::Set::Threw;
  return storage_put(vm, o, k, text.view()) ? HostClass::Set::Handled : HostClass::Set::Threw;
}

// Removes a key and reports it.
void storage_drop(Vm& vm, JsObject* o, StringView key) {
  StorageArea* a = area_of(o);
  StringView existing;
  if (!a->get(key, &existing)) return;
  String old_copy(vm.allocator()), key_copy(vm.allocator());
  old_copy.assign(existing);
  key_copy.assign(key);
  a->remove(key);
  storage_notify(vm, o, &key_copy, &old_copy, nullptr);
}

bool storage_remove(Vm& vm, JsObject* o, JsString* key, bool* deleted) {
  storage_drop(vm, o, key->view());
  *deleted = true;
  return true;
}

void storage_keys(Vm& vm, JsObject* o, JsObject* keys) {
  for (uint32_t i = 0; i < area_of(o)->count(); ++i) vm.array_push(keys, str_value(vm, area_of(o)->key(i)));
}

const HostClass kStorageClass = {"Storage", storage_get, storage_set, storage_remove, storage_keys};

JsObject* this_storage(Vm& vm, const Value& self) {
  if (self.is_object() && self.as_object()->host == &kStorageClass) return self.as_object();
  vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  return nullptr;
}

NATIVE(storage_get_item) {
  JsObject* o = this_storage(vm, self);
  String key(vm.allocator());
  if (!o || !arg_text(vm, arg_at(args, argc, 0), &key)) return false;
  StringView v;
  *result = area_of(o)->get(key.view(), &v) ? str_value(vm, v) : Value::make_null();
  return true;
}

NATIVE(storage_set_item) {
  JsObject* o = this_storage(vm, self);
  String key(vm.allocator()), value(vm.allocator());
  if (!o) return false;
  if (argc < 2) {
    String message(vm.allocator());
    message.append("Failed to execute 'setItem' on 'Storage': 2 arguments required, but only ");
    message.append_uint(argc);
    message.append(" present.");
    return vm.throw_error(ErrorKind::TypeError, message.view());
  }
  if (!arg_text(vm, args[0], &key)) return false;
  JsString* s;
  if (!vm.to_string(args[1], &s)) return false;
  value.assign(s->view());
  return storage_put(vm, o, key.view(), value.view());
}

NATIVE(storage_remove_item) {
  JsObject* o = this_storage(vm, self);
  String key(vm.allocator());
  if (!o || !arg_text(vm, arg_at(args, argc, 0), &key)) return false;
  storage_drop(vm, o, key.view());
  return true;
}

NATIVE(storage_clear) {
  JsObject* o = this_storage(vm, self);
  if (!o) return false;
  const bool had_any = area_of(o)->count() > 0;
  area_of(o)->clear();
  if (had_any) storage_notify(vm, o, nullptr, nullptr, nullptr);
  return true;
}

NATIVE(storage_key) {
  JsObject* o = this_storage(vm, self);
  double d;
  if (!o || !vm.to_number(arg_at(args, argc, 0), &d)) return false;
  *result = d >= 0 && d < area_of(o)->count() ? str_value(vm, area_of(o)->key(static_cast<uint32_t>(d))) : Value::make_null();
  return true;
}

// --- Window functions ----------------------------------------------------------------------------------------------------------

NATIVE(win_get_computed_style) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = node_of(arg_at(args, argc, 0));
  if (!n || !n->is_element()) return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
  uint32_t pseudo = 0;   // getComputedStyle(element, '::before')
  String which(vm.allocator());
  if (argc > 1 && !args[1].is_undefined() && !args[1].is_null() && arg_text(vm, args[1], &which)) {
    const StringView w = which.view();
    if (w.equals_ignore_case("::before") || w.equals_ignore_case(":before")) pseudo = static_cast<uint32_t>(PseudoElement::Before);
    else if (w.equals_ignore_case("::after") || w.equals_ignore_case(":after")) pseudo = static_cast<uint32_t>(PseudoElement::After);
  }
  *result = make_computed_style(c, n, pseudo);
  return true;
}

NATIVE(win_request_animation_frame) {
  if (!Vm::is_callable(arg_at(args, argc, 0))) return vm.throw_error(ErrorKind::TypeError, "requestAnimationFrame needs a function");
  const Value stamp = num_value(vm.clock_ms());
  *result = num_value(vm.add_timer(args[0], 16, -1, &stamp, 1));
  return true;
}

NATIVE(win_cancel_animation_frame) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  vm.clear_timer(id);
  return true;
}

NATIVE(win_queue_microtask) {
  if (!Vm::is_callable(arg_at(args, argc, 0))) return vm.throw_error(ErrorKind::TypeError, "queueMicrotask needs a function");
  vm.enqueue_microtask(args[0]);
  return true;
}

NATIVE(win_scroll_to) {
  DomBindings::Impl& c = impl(vm);
  double top = c.scroll_line;   // (pixels: the field keeps its old name)
  const Value a0 = arg_at(args, argc, 0);
  if (a0.is_object()) {
    Value v;
    vm.get_named(a0, "top", &v);
    if (v.is_number()) top = v.u.number;
  } else if (argc > 1) {
    if (!vm.to_number(args[1], &top)) return false;
  }
  if (top != top || top < 0) top = 0;
  c.scroll_line = top > 100000000 ? 100000000 : static_cast<int32_t>(top);
  c.host.scroll_to(c.scroll_line);
  update_metrics(c);
  return true;
}

NATIVE(win_scroll_by) {
  DomBindings::Impl& c = impl(vm);
  double dy = 0;
  const Value a0 = arg_at(args, argc, 0);
  if (a0.is_object()) {
    Value v;
    vm.get_named(a0, "top", &v);
    if (v.is_number()) dy = v.u.number;
  } else if (argc > 1) {
    if (!vm.to_number(args[1], &dy)) return false;
  }
  if (dy != dy) dy = 0;
  int64_t line = c.scroll_line + static_cast<int64_t>(dy);
  if (line < 0) line = 0;
  if (line > 100000000) line = 100000000;
  c.scroll_line = static_cast<int32_t>(line);
  c.host.scroll_to(c.scroll_line);
  update_metrics(c);
  return true;
}

NATIVE(win_confirm) {
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  *result = bool_value(impl(vm).host.confirm(text.view()));
  return true;
}

NATIVE(win_prompt) {
  *result = Value::make_null();
  return true;
}

NATIVE(win_do_nothing) { return true; }

// __rect(element): [x, y, width, height, content width, content height, padding left, padding top] of its box on the page (CSS pixels from the top left of the
// document), or null if it has none.
NATIVE(node_page_rect) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  double r[8];
  if (!n || !impl(vm).host.node_rect(n, r)) {
    *result = Value::make_null();
    return true;
  }
  JsObject* a = vm.new_array();
  for (double v : r) vm.array_push(a, num_value(v));
  *result = Value::make_object(a);
  return true;
}

NATIVE(layout_ready_native) {
  (void)self; (void)args; (void)argc;
  *result = bool_value(impl(vm).host.layout_ready());
  return true;
}

// --- navigation: what the Navigation API and window.open() need from the browser ----------------------------------------------------

NATIVE(set_hidden) {
  (void)self;
  impl(vm).page_hidden = argc > 0 && args[0].is_bool() && args[0].u.boolean;
  *result = Value();
  return true;
}

// __history_entries(): JSON text {"index":n,"entries":[{"key","id","url","state","nav"}]}, or null without a session history.
NATIVE(history_entries) {
  (void)self; (void)args; (void)argc;
  String json(vm.allocator());
  int64_t index = -1;
  if (!impl(vm).host.history_entries(&json, &index)) {
    *result = Value::make_null();
    return true;
  }
  String text(vm.allocator());
  text.append("{\"index\":");
  text.append_int(index);
  text.append(",\"entries\":");
  text.append(json.view());
  text.append("}");
  *result = str_value(vm, text.view());
  return true;
}

// __nav_state(text | null): the Navigation API's state of the current entry.
NATIVE(nav_state) {
  (void)self;
  String text(vm.allocator());
  const bool has = argc > 0 && !args[0].is_nullish();
  if (has && !arg_text(vm, args[0], &text)) return false;
  impl(vm).host.set_navigation_state(text.view(), has);
  *result = Value();
  return true;
}

NATIVE(nav_go_key) {
  (void)self;
  double key = 0;
  if (argc > 0 && !vm.to_number(args[0], &key)) return false;
  if (key >= 1) impl(vm).host.history_go_to_key(static_cast<uint64_t>(key));
  *result = Value();
  return true;
}

// __open_window(url, target, noopener): true if the browser opened a window.
NATIVE(open_window) {
  (void)self;
  String url(vm.allocator()), target(vm.allocator());
  if (argc > 0 && !arg_text(vm, args[0], &url)) return false;
  if (argc > 1 && !arg_text(vm, args[1], &target)) return false;
  const bool noopener = argc > 2 && args[2].is_bool() && args[2].u.boolean;
  *result = bool_value(impl(vm).host.open_window(url.view(), target.view(), noopener));
  return true;
}

NATIVE(close_window) {
  (void)self; (void)args; (void)argc;
  impl(vm).host.close_window();
  *result = Value();
  return true;
}


NATIVE(win_structured_clone) {
  Value json;
  if (!vm.get_named(Value::make_object(vm.global()), "JSON", &json)) return false;
  Value text;
  const Value v = arg_at(args, argc, 0);
  Value stringify, parse;
  if (!vm.get_named(json, "stringify", &stringify) || !vm.call(stringify, json, &v, 1, &text)) return false;
  if (text.is_undefined()) return throw_dom(vm, "DataCloneError", "The object could not be cloned.");
  if (!vm.get_named(json, "parse", &parse)) return false;
  return vm.call(parse, json, &text, 1, result);
}

// --- Base64 and URIs ---------------------------------------------------------------------------------------------------------------

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

NATIVE(win_btoa) {
  String text(vm.allocator()), out(vm.allocator());
  JsString* source;
  if (!vm.to_string(arg_at(args, argc, 0), &source)) return false;
  if (vm.string_kind(source) == 2) {   // text: each character is a byte (Latin-1); anything above 255 is an error
    const uint16_t* wide = vm.string_wide(source);
    for (uint32_t i = 0; wide && i < source->units; ++i) {
      if (wide[i] > 255) return throw_dom(vm, "InvalidCharacterError", "The string to be encoded contains characters outside of the Latin1 range.");
      text.push_back(static_cast<char>(wide[i]));
    }
  } else {
    text.assign(source->view());
  }
  const StringView s = text.view();
  for (uint64_t i = 0; i < s.size(); i += 3) {
    const uint32_t b0 = static_cast<uint8_t>(s[i]), b1 = i + 1 < s.size() ? static_cast<uint8_t>(s[i + 1]) : 0, b2 = i + 2 < s.size() ? static_cast<uint8_t>(s[i + 2]) : 0;
    const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kBase64[(n >> 18) & 63]);
    out.push_back(kBase64[(n >> 12) & 63]);
    out.push_back(i + 1 < s.size() ? kBase64[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < s.size() ? kBase64[n & 63] : '=');
  }
  *result = str_value(vm, out.view());
  return true;
}

NATIVE(win_atob) {
  String text(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  String clean(vm.allocator());   // (forgiving-base64 decode: white space goes, a length of 0 mod 4 may end in one or two '=', 1 mod 4 and any other '=' are errors)
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch != ' ' && ch != '\n' && ch != '\t' && ch != '\r' && ch != '\f') clean.push_back(ch);
  }
  if (clean.size() % 4 == 0) {
    for (int k = 0; k < 2 && clean.size() && clean[clean.size() - 1] == '='; ++k) clean.truncate(clean.size() - 1);
  }
  if (clean.size() % 4 == 1) return throw_dom(vm, "InvalidCharacterError", "The string to be decoded is not correctly encoded.");
  text.assign(clean.view());
  uint32_t buffer = 0;
  int bits = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    int v = -1;
    for (int k = 0; k < 64 && v < 0; ++k)
      if (kBase64[k] == ch) v = k;
    if (v < 0) return throw_dom(vm, "InvalidCharacterError", "The string to be decoded is not correctly encoded.");
    buffer = (buffer << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  *result = Value::make_string(vm.new_binary_string(out.view()));   // (bytes: every character is 0..255)
  return true;
}

// Throws a URIError (an Error with the message when that class is not there).
bool throw_uri_error(Vm& vm, const char* message) {
  Value ctor;
  if (vm.get_named(Value::make_object(vm.global()), "URIError", &ctor) && Vm::is_callable(ctor)) {
    Value text = Value::make_string(vm.new_string(message));
    Value error;
    if (vm.construct(ctor, &text, 1, &error)) return vm.throw_value(error);
    return false;
  }
  return vm.throw_error(ErrorKind::Error, message);
}

bool uri_encode(Vm& vm, const Value* args, uint32_t argc, Value* result, const char* keep) {
  String text(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  static const char kHex[] = "0123456789ABCDEF";
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (static_cast<uint8_t>(ch) == 0xED && i + 1 < text.size() && static_cast<uint8_t>(text[i + 1]) >= 0xA0) return throw_uri_error(vm, "URI malformed");   // (a lone surrogate)
    bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    for (const char* p = keep; *p && !plain; ++p) plain = *p == ch;
    if (plain) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[(static_cast<uint8_t>(ch) >> 4) & 15]);
      out.push_back(kHex[static_cast<uint8_t>(ch) & 15]);
    }
  }
  *result = str_value(vm, out.view());
  return true;
}
NATIVE(win_encode_uri_component) { return uri_encode(vm, args, argc, result, "-_.!~*'()"); }
NATIVE(win_encode_uri) { return uri_encode(vm, args, argc, result, "-_.!~*'();/?:@&=+$,#"); }

bool uri_decode(Vm& vm, const Value* args, uint32_t argc, Value* result, const char* keep_encoded) {
  String text(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  auto hex = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return throw_uri_error(vm, "URI malformed");
    const char decoded = static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
    bool keep = false;
    for (const char* p = keep_encoded; *p && !keep; ++p) keep = *p == decoded;
    if (keep) out.append(text.view().substr(i, 3));
    else out.push_back(decoded);
    i += 2;
  }
  *result = str_value(vm, out.view());
  return true;
}
NATIVE(win_decode_uri_component) { return uri_decode(vm, args, argc, result, ""); }
NATIVE(win_decode_uri) { return uri_decode(vm, args, argc, result, ";/?:@&=+$,#"); }

// --- performance -----------------------------------------------------------------------------------------------------------------

NATIVE(performance_now) {
  Value origin;
  vm.get_named(Value::make_object(vm.global()), "__time_origin", &origin);
  *result = num_value(vm.clock_ms() - (origin.is_number() ? origin.u.number : 0));
  return true;
}

// --- console ---------------------------------------------------------------------------------------------------------------------

void console_text(Vm& vm, const Value* args, uint32_t argc, uint32_t from, String* out) {
  for (uint32_t i = from; i < argc; ++i) {
    if (i > from) out->push_back(' ');
    vm.describe(args[i], out, 0);
  }
}

NATIVE(console_assert) {
  if (argc > 0 && Vm::to_boolean(args[0])) return true;
  String text(vm.allocator());
  text.append("Assertion failed");
  if (argc > 1) {
    text.append(": ");
    console_text(vm, args, argc, 1, &text);
  }
  if (vm.host()) vm.host()->console(3, text.view());
  return true;
}

NATIVE(console_log_like) {
  String text(vm.allocator());
  console_text(vm, args, argc, 0, &text);
  if (vm.host()) vm.host()->console(0, text.view());
  return true;
}

NATIVE(console_time) {
  String label(vm.allocator());
  arg_text(vm, argc ? args[0] : str_value(vm, "default"), &label);
  String key(vm.allocator());
  key.append("__time_");
  key.append(label.view());
  vm.define(vm.global(), vm.intern(key.view()), num_value(vm.clock_ms()), 0);
  return true;
}

bool console_time_report(Vm& vm, const Value* args, uint32_t argc, bool end) {
  String label(vm.allocator());
  arg_text(vm, argc ? args[0] : str_value(vm, "default"), &label);
  String key(vm.allocator());
  key.append("__time_");
  key.append(label.view());
  Value start;
  if (!vm.get_plain(vm.global(), vm.intern(key.view()), &start) || !start.is_number()) return true;
  String text(vm.allocator());
  text.append(label.view());
  text.append(": ");
  text.append_uint(static_cast<uint64_t>(vm.clock_ms() - start.u.number));
  text.append("ms");
  if (vm.host()) vm.host()->console(0, text.view());
  if (end) vm.delete_property(vm.global(), vm.intern(key.view()));
  return true;
}
NATIVE(console_time_end) { return console_time_report(vm, args, argc, true); }
NATIVE(console_time_log) { return console_time_report(vm, args, argc, false); }

NATIVE(console_count) {
  String label(vm.allocator());
  arg_text(vm, argc ? args[0] : str_value(vm, "default"), &label);
  String key(vm.allocator());
  key.append("__count_");
  key.append(label.view());
  Value n;
  const double next = (vm.get_plain(vm.global(), vm.intern(key.view()), &n) && n.is_number() ? n.u.number : 0) + 1;
  vm.define(vm.global(), vm.intern(key.view()), num_value(next), 0);
  String text(vm.allocator());
  text.append(label.view());
  text.append(": ");
  text.append_uint(static_cast<uint64_t>(next));
  if (vm.host()) vm.host()->console(0, text.view());
  return true;
}

// --- Observers and other things that exist but report nothing ---------------------------------------------------------------------

NATIVE(observer_constructor) {
  if (self.is_object() && argc > 0) vm.define(self.as_object(), vm.intern("__callback"), args[0], 0);
  *result = self;
  return true;
}

NATIVE(empty_array_native) {
  *result = Value::make_object(vm.new_array());
  return true;
}

// --- matchMedia ---------------------------------------------------------------------------------------------------------------------

NATIVE(win_match_media) {
  DomBindings::Impl& c = impl(vm);
  String query(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &query)) return false;
  JsObject* mql = vm.new_object(c.media_query_proto);
  const bool matches = media_matches(query.view(), c.host.media());
  vm.define_named(mql, "media", str_value(vm, media_valid(query.view()) ? query.view() : StringView("not all")));
  vm.define_named(mql, "matches", bool_value(matches));
  vm.define_named(mql, "onchange", Value::make_null());
  vm.define(mql, vm.intern("__q"), str_value(vm, query.view()), 0);
  vm.array_push(c.media_lists, Value::make_object(mql));
  *result = Value::make_object(mql);
  return true;
}

NATIVE(mql_add_listener) {   // addListener(fn) / addEventListener("change", fn)
  if (!self.is_object()) return true;
  JsObject* mql = self.as_object();
  Value fn = argc > 1 && arg_at(args, argc, 0).is_string() ? args[1] : arg_at(args, argc, 0);
  if (argc > 1 && arg_at(args, argc, 0).is_string() && !args[0].as_string()->view().equals("change")) return true;
  if (!Vm::is_callable(fn) && !fn.is_object()) return true;
  JsString* key = vm.intern("__fns");
  Value list;
  if (!vm.get_plain(mql, key, &list) || !list.is_object()) {
    list = Value::make_object(vm.new_array());
    vm.define(mql, key, list, 0);
  }
  for (uint32_t i = 0; i < list.as_object()->length; ++i)
    if (vm.strict_equals(list.as_object()->items[i], fn)) return true;
  vm.array_push(list.as_object(), fn);
  return true;
}

NATIVE(mql_remove_listener) {
  if (!self.is_object()) return true;
  Value fn = argc > 1 && arg_at(args, argc, 0).is_string() ? args[1] : arg_at(args, argc, 0);
  Value list;
  if (!vm.get_plain(self.as_object(), vm.intern("__fns"), &list) || !list.is_object()) return true;
  JsObject* l = list.as_object();
  for (uint32_t i = 0; i < l->length; ++i) {
    if (vm.strict_equals(l->items[i], fn)) {
      for (uint32_t k = i + 1; k < l->length; ++k) l->items[k - 1] = l->items[k];
      --l->length;
      break;
    }
  }
  return true;
}

NATIVE(mql_dispatch) {
  *result = bool_value(true);
  return true;
}

bool window_set(Vm& vm, JsObject*, JsString* key, const Value& value, HostClass::Set* out) {
  (void)vm;
  (void)key;
  (void)value;
  *out = HostClass::Set::NotHandled;
  return true;
}

HostClass::Set window_set_property(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  HostClass::Set r;
  window_set(vm, o, key, value, &r);
  if (key->view().equals("location")) {
    DomBindings::Impl& c = impl(vm);
    String text(vm.allocator()), url(vm.allocator());
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    resolve_url(vm, c, text.view(), &url);
    c.host.navigate(url.view(), false);
    return HostClass::Set::Handled;
  }
  return r;
}

const HostClass kWindowClass = {"Window", nullptr, window_set_property, nullptr, nullptr};

void define_function_global(Vm& vm, const char* name, NativeFn fn, uint32_t arity) { vm.define_native(vm.global(), name, fn, arity); }

// --- Fullscreen: the script side (WebPrelude.cpp) keeps the promises and events; these ask the page and the window.
NATIVE(fs_supported) {
  (void)self; (void)args; (void)argc;
  *result = bool_value(impl(vm).host.fullscreen_supported());
  return true;
}
// __fs_request(element): true if the window went fullscreen with that element.
NATIVE(fs_request) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  *result = bool_value(n && impl(vm).host.fullscreen_request(n));
  return true;
}
// __make_node(document, kind, nameOrTarget, data): kind 7 a processing instruction, 4 a CDATA section.
NATIVE(make_node) {
  (void)self;
  dom::Node* d = node_of(arg_at(args, argc, 0));
  double kind = 0;
  String name(vm.allocator()), data(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 1), &kind) || !arg_text(vm, arg_at(args, argc, 2), &name) || !arg_text(vm, arg_at(args, argc, 3), &data)) return false;
  *result = Value::make_null();
  if (!d) return true;
  dom::Node* n = kind == 7 ? d->owner->create_comment(data.view()) : d->owner->create_text(data.view());
  if (!n) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  n->dom_type = kind == 7 ? 7 : 4;
  n->name.assign(kind == 7 ? name.view() : StringView("#cdata-section"));
  *result = node_value(impl(vm), n);
  return true;
}

// __set_attr_ns(element, namespace, qualifiedName, value): an attribute with a namespace. __attr_ns(element, qualifiedName): its namespace (null if it has none, undefined if
// there is no such attribute). __attr_names(element): the qualified names, in order.
NATIVE(set_attr_ns) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  String ns(vm.allocator()), name(vm.allocator()), value(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 1), &ns) || !arg_text(vm, arg_at(args, argc, 2), &name) || !arg_text(vm, arg_at(args, argc, 3), &value)) return false;
  *result = Value();
  if (!n || !n->is_element()) return true;
  const dom::DomError e = dom::set_attribute(n, name.view(), value.view());
  if (e != dom::DomError::None) return throw_dom_error(vm, e);
  for (uint64_t i = 0; i < n->attrs.count(); ++i) {
    String lowered(vm.allocator());
    for (char c : name.view()) lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
    if (n->attrs[i].name.view().equals(lowered.view())) n->attrs[i].ns.assign(ns.view());
  }
  return true;
}

NATIVE(attr_ns) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 1), &name)) return false;
  *result = Value();
  if (!n || !n->is_element()) return true;
  for (uint64_t i = 0; i < n->attrs.count(); ++i) {
    String lowered(vm.allocator());
    for (char c : name.view()) lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
    if (n->attrs[i].name.view().equals(lowered.view())) {
      *result = n->attrs[i].ns.empty() ? Value::make_null() : str_value(vm, n->attrs[i].ns.view());
      return true;
    }
  }
  return true;
}

// __detach_buffer(arrayBuffer): the buffer is transferred away (structuredClone with `transfer`): it has no bytes any more.
NATIVE(detach_buffer) {
  (void)self;
  (void)result;
  const Value b = arg_at(args, argc, 0);
  if (b.is_object()) script::detach_array_buffer(b.as_object());   // (does nothing for an object that is no ArrayBuffer)
  return true;
}

// __set_content_type(document, type): the document is XML of that type (DOMParser, createDocument).
NATIVE(set_content_type) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  String type(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 1), &type)) return false;
  if (n && n->type == dom::NodeType::Document) n->owner->content_type.assign(type.view());
  *result = Value();
  return true;
}

// __cookie_details(url): the cookies a script can see, as text (see IDomHost::cookie_details). __cookie_write(setCookie): true if the cookie was kept.
NATIVE(cookie_details) {
  (void)self;
  String url(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &url)) return false;
  impl(vm).host.cookie_details(url.view(), &out);
  *result = str_value(vm, out.view());
  return true;
}
NATIVE(cookie_write) {
  (void)self;
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  *result = bool_value(impl(vm).host.cookie_write(text.view()));
  return true;
}

NATIVE(clipboard_get) {
  (void)self; (void)args; (void)argc;
  String text(vm.allocator());
  *result = impl(vm).host.clipboard_read(&text) ? str_value(vm, text.view()) : Value::make_null();
  return true;
}

// __clipboard_formats(): the MIME types on the clipboard, comma separated ("" when it is empty).
NATIVE(clipboard_formats) {
  (void)self; (void)args; (void)argc;
  String csv(vm.allocator());
  impl(vm).host.clipboard_formats(&csv);
  *result = str_value(vm, csv.view());
  return true;
}

// __clipboard_read(format): the bytes of that format as a string, or null.
NATIVE(clipboard_read_format) {
  (void)self;
  String format(vm.allocator()), bytes(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &format)) return false;
  *result = impl(vm).host.clipboard_read_format(format.view(), &bytes) ? str_value(vm, bytes.view()) : Value::make_null();
  return true;
}

// __clipboard_write(text, html, png): each a string or null; false if the browser could not.
NATIVE(clipboard_write_items) {
  (void)self;
  String text(vm.allocator()), html(vm.allocator()), image(vm.allocator());
  ClipboardData data;
  const Value t = arg_at(args, argc, 0), h = arg_at(args, argc, 1), i = arg_at(args, argc, 2);
  if (!t.is_nullish()) {
    if (!arg_text(vm, t, &text)) return false;
    data.has_text = true;
  }
  if (!h.is_nullish()) {
    if (!arg_text(vm, h, &html)) return false;
    data.has_html = true;
  }
  if (!i.is_nullish()) {
    if (!arg_text(vm, i, &image)) return false;
    data.has_image = true;
  }
  data.text = text.view();
  data.html = html.view();
  data.image_png = image.view();
  *result = bool_value(impl(vm).host.clipboard_write_items(data));
  return true;
}

NATIVE(clipboard_set) {
  (void)self;
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  impl(vm).host.clipboard_write(text.view());
  *result = Value();
  return true;
}

NATIVE(pip_supported) {
  (void)self; (void)args; (void)argc;
  *result = bool_value(impl(vm).host.pip_supported());
  return true;
}
// __pip_request(video): [code, width, height]; the code is 0 when the video is in the window now.
NATIVE(pip_request) {
  (void)self;
  uint32_t w = 0, h = 0;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  const int code = impl(vm).host.pip_request(n, &w, &h);
  JsObject* out = vm.new_array();
  if (!out) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  vm.array_push(out, num_value(code));
  vm.array_push(out, num_value(w));
  vm.array_push(out, num_value(h));
  *result = Value::make_object(out);
  return true;
}
NATIVE(pip_exit) {
  (void)self; (void)args; (void)argc;
  impl(vm).host.pip_exit();
  *result = Value();
  return true;
}
NATIVE(pip_element) {
  (void)self; (void)args; (void)argc;
  *result = node_value(impl(vm), impl(vm).host.pip_element());
  return true;
}

NATIVE(fs_exit) {
  (void)self; (void)args; (void)argc;
  impl(vm).host.fullscreen_exit();
  *result = Value();
  return true;
}
NATIVE(fs_element) {
  (void)self; (void)args; (void)argc;
  *result = node_value(impl(vm), impl(vm).host.fullscreen_element());
  return true;
}

}  // namespace

void update_metrics(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  const MediaEnvironment& env = c.host.media();
  vm.define_named(g, "innerWidth", num_value(env.width));
  vm.define_named(g, "innerHeight", num_value(env.height));
  vm.define_named(g, "outerWidth", num_value(env.width));
  vm.define_named(g, "outerHeight", num_value(env.height));
  vm.define_named(g, "scrollX", num_value(0));
  vm.define_named(g, "pageXOffset", num_value(0));
  vm.define_named(g, "scrollY", num_value(c.scroll_line));
  vm.define_named(g, "pageYOffset", num_value(c.scroll_line));
  vm.define_named(g, "devicePixelRatio", num_value(env.resolution_dpi / 96.0));
  Value screen;
  if (vm.get_named(Value::make_object(g), "screen", &screen) && screen.is_object()) {
    const int sw = env.screen_width > 0 ? env.screen_width : env.width, sh = env.screen_height > 0 ? env.screen_height : env.height;
    vm.define_named(screen.as_object(), "width", num_value(sw));
    vm.define_named(screen.as_object(), "height", num_value(sh));
    vm.define_named(screen.as_object(), "availWidth", num_value(sw));
    vm.define_named(screen.as_object(), "availHeight", num_value(sh));
  }
}

uint32_t media_changed_impl(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  uint32_t changed = 0;
  vm.hold_collector();
  for (uint32_t i = 0; i < c.media_lists->length; ++i) {
    JsObject* mql = c.media_lists->items[i].as_object();
    Value q, was;
    if (!vm.get_plain(mql, vm.intern("__q"), &q) || !q.is_string() || !vm.get_plain(mql, vm.intern("matches"), &was)) continue;
    const bool now = media_matches(q.as_string()->view(), c.host.media());
    if (now == Vm::to_boolean(was)) continue;
    ++changed;
    vm.define_named(mql, "matches", bool_value(now));
    // the change event: { type: "change", matches, media }
    JsObject* e = vm.new_plain();
    vm.define_named(e, "type", str_value(vm, "change"));
    vm.define_named(e, "matches", bool_value(now));
    Value media;
    vm.get_plain(mql, vm.intern("media"), &media);
    vm.define_named(e, "media", media);
    vm.define_named(e, "target", Value::make_object(mql));
    vm.define_named(e, "currentTarget", Value::make_object(mql));
    const Value event = Value::make_object(e);
    Value handler;
    if (vm.get_plain(mql, vm.intern("onchange"), &handler) && Vm::is_callable(handler)) {
      Value ignored;
      if (!vm.call(handler, Value::make_object(mql), &event, 1, &ignored)) {
        String text(vm.allocator());
        vm.take_exception(&text);
        if (c.error_count++ == 0) c.first_error.assign(text.view());
        fire_error_event(c, text.view());
      }
    }
    Value list;
    if (vm.get_plain(mql, vm.intern("__fns"), &list) && list.is_object()) {
      List<Value> copy(vm.allocator());
      for (uint32_t k = 0; k < list.as_object()->length; ++k) copy.push_back(list.as_object()->items[k]);
      for (uint64_t k = 0; k < copy.count(); ++k) {
        Value fn = copy[k], self = Value::make_object(mql);
        if (!Vm::is_callable(fn)) {
          Value handle;
          if (!vm.get_named(fn, "handleEvent", &handle) || !Vm::is_callable(handle)) continue;
          self = fn;
          fn = handle;
        }
        Value ignored;
        if (!vm.call(fn, self, &event, 1, &ignored)) {
          String text(vm.allocator());
          vm.take_exception(&text);
          if (c.error_count++ == 0) c.first_error.assign(text.view());
        fire_error_event(c, text.view());
        }
      }
    }
  }
  vm.release_collector();
  return changed;
}

bool install_window(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  g->host = &kWindowClass;
  vm.define_named(g, "__time_origin", num_value(vm.clock_ms()), 0);
  vm.define_native(g, "__set_content_type", set_content_type, 2);
  vm.define_native(g, "__detach_buffer", detach_buffer, 1);
  vm.define_native(g, "__set_attr_ns", set_attr_ns, 4);
  vm.define_native(g, "__make_node", make_node, 4);
  vm.define_native(g, "__attr_ns", attr_ns, 2);
  vm.define_native(g, "__cookie_details", cookie_details, 1);
  vm.define_native(g, "__cookie_write", cookie_write, 1);
  vm.define_native(g, "__clipboard_get", clipboard_get, 0);
  vm.define_native(g, "__clipboard_set", clipboard_set, 1);
  vm.define_native(g, "__clipboard_formats", clipboard_formats, 0);
  vm.define_native(g, "__clipboard_read", clipboard_read_format, 1);
  vm.define_native(g, "__clipboard_write", clipboard_write_items, 3);
  vm.define_native(g, "__pip_supported", pip_supported, 0);
  vm.define_native(g, "__pip_request", pip_request, 1);
  vm.define_native(g, "__pip_exit", pip_exit, 0);
  vm.define_native(g, "__pip_element", pip_element, 0);
  vm.define_native(g, "__set_hidden", set_hidden, 1);
  vm.define_native(g, "__rect", node_page_rect, 1);
  vm.define_native(g, "__layout_ready", layout_ready_native, 0);
  vm.define_native(g, "__history_entries", history_entries, 0);
  vm.define_native(g, "__nav_state", nav_state, 1);
  vm.define_native(g, "__nav_go_key", nav_go_key, 1);
  vm.define_native(g, "__open_window", open_window, 3);
  vm.define_native(g, "__close_window", close_window, 0);
  vm.define_native(g, "__fs_supported", fs_supported, 0);
  vm.define_native(g, "__fs_request", fs_request, 1);
  vm.define_native(g, "__fs_exit", fs_exit, 0);
  vm.define_native(g, "__fs_element", fs_element, 0);

  // location and history
  c.location_proto = nullptr;
  c.location_proto = vm.new_plain();
  vm.define_native(c.location_proto, "assign", location_assign, 1);
  vm.define_native(c.location_proto, "replace", location_replace, 1);
  vm.define_native(c.location_proto, "reload", location_reload, 0);
  vm.define_native(c.location_proto, "toString", location_to_string, 0);
  c.location = vm.new_host_object(c.location_proto, &kLocationClass, &c);
  vm.define_named(g, "location", Value::make_object(c.location), kWritable);
  c.history_proto = vm.new_plain();
  vm.define_native(c.history_proto, "back", history_back, 0);
  vm.define_native(c.history_proto, "forward", history_forward, 0);
  vm.define_native(c.history_proto, "go", history_go, 1);
  vm.define_native(c.history_proto, "pushState", history_push_state, 3);
  vm.define_native(c.history_proto, "replaceState", history_replace_state, 3);
  JsObject* history = vm.new_host_object(c.history_proto, &kHistoryClass, &c);
  vm.define_named(history, "scrollRestoration", str_value(vm, "auto"), kWritable);
  vm.define_named(g, "history", Value::make_object(history), kWritable);

  // storage
  c.storage_proto = vm.new_plain();
  vm.define_native(c.storage_proto, "getItem", storage_get_item, 1);
  vm.define_native(c.storage_proto, "setItem", storage_set_item, 2);
  vm.define_native(c.storage_proto, "removeItem", storage_remove_item, 1);
  vm.define_native(c.storage_proto, "clear", storage_clear, 0);
  vm.define_native(c.storage_proto, "key", storage_key, 1);
  {
    JsObject* storage_ctor = vm.new_native([](Vm& v, const Value&, const Value*, uint32_t, Value*) { return v.throw_error(ErrorKind::TypeError, "Illegal constructor"); }, "Storage", 0);
    vm.define(storage_ctor, vm.prototype_name(), Value::make_object(c.storage_proto), 0);
    vm.define(c.storage_proto, vm.intern("constructor"), Value::make_object(storage_ctor), kWritable);
    vm.define_named(g, "Storage", Value::make_object(storage_ctor), kWritable);
  }
  c.local_storage = vm.new_host_object(c.storage_proto, &kStorageClass, &c.host.local_storage());
  c.session_storage = vm.new_host_object(c.storage_proto, &kStorageClass, &c.host.session_storage());
  vm.define_named(g, "localStorage", Value::make_object(c.local_storage), kWritable);
  vm.define_named(g, "sessionStorage", Value::make_object(c.session_storage), kWritable);

  // media queries
  c.media_query_proto = vm.new_plain();
  vm.define_native(c.media_query_proto, "addListener", mql_add_listener, 1);
  vm.define_native(c.media_query_proto, "removeListener", mql_remove_listener, 1);
  vm.define_native(c.media_query_proto, "addEventListener", mql_add_listener, 2);
  vm.define_native(c.media_query_proto, "removeEventListener", mql_remove_listener, 2);
  vm.define_native(c.media_query_proto, "dispatchEvent", mql_dispatch, 1);
  define_function_global(vm, "matchMedia", win_match_media, 1);

  // functions of the window
  define_function_global(vm, "getComputedStyle", win_get_computed_style, 2);
  define_function_global(vm, "requestAnimationFrame", win_request_animation_frame, 1);
  define_function_global(vm, "cancelAnimationFrame", win_cancel_animation_frame, 1);
  define_function_global(vm, "queueMicrotask", win_queue_microtask, 1);
  define_function_global(vm, "scrollTo", win_scroll_to, 2);
  define_function_global(vm, "scroll", win_scroll_to, 2);
  define_function_global(vm, "scrollBy", win_scroll_by, 2);
  define_function_global(vm, "confirm", win_confirm, 1);
  define_function_global(vm, "prompt", win_prompt, 2);
  for (const char* name : {"focus", "blur", "print", "stop", "moveTo", "resizeTo"}) define_function_global(vm, name, win_do_nothing, 0);
  define_function_global(vm, "structuredClone", win_structured_clone, 1);
  define_function_global(vm, "btoa", win_btoa, 1);
  define_function_global(vm, "atob", win_atob, 1);
  define_function_global(vm, "encodeURIComponent", win_encode_uri_component, 1);
  define_function_global(vm, "encodeURI", win_encode_uri, 1);
  define_function_global(vm, "decodeURIComponent", win_decode_uri_component, 1);
  define_function_global(vm, "decodeURI", win_decode_uri, 1);
  vm.define_named(g, "name", str_value(vm, ""), kWritable);
  vm.define_named(g, "status", str_value(vm, ""), kWritable);
  vm.define_named(g, "closed", bool_value(false), kWritable);
  vm.define_named(g, "length", num_value(0), kWritable);
  vm.define_named(g, "opener", Value::make_null(), kWritable);
  vm.define_named(g, "top", Value::make_object(g), kWritable);
  vm.define_named(g, "parent", Value::make_object(g), kWritable);
  vm.define_named(g, "frames", Value::make_object(g), kWritable);
  vm.define_named(g, "frameElement", Value::make_null(), kWritable);
  vm.define_named(g, "isSecureContext", bool_value(false), kWritable);
  vm.define_named(g, "onload", Value::make_null(), kWritable);

  // screen, navigator, performance
  JsObject* screen = vm.new_plain();
  vm.define_named(screen, "colorDepth", num_value(24));
  vm.define_named(screen, "pixelDepth", num_value(24));
  JsObject* orientation = vm.new_plain();
  vm.define_named(orientation, "type", str_value(vm, "landscape-primary"));
  vm.define_named(orientation, "angle", num_value(0));
  vm.define_named(screen, "orientation", Value::make_object(orientation));
  vm.define_named(g, "screen", Value::make_object(screen), kWritable);
  Value navigator;
  if (vm.get_named(Value::make_object(g), "navigator", &navigator) && navigator.is_object()) {
    JsObject* n = navigator.as_object();
    vm.define_named(n, "platform", str_value(vm, "TheoryCraftOS"));
    vm.define_named(n, "vendor", str_value(vm, ""));
    vm.define_named(n, "onLine", bool_value(true));
    vm.define_named(n, "cookieEnabled", bool_value(true));
    vm.define_named(n, "hardwareConcurrency", num_value(1));
    vm.define_named(n, "doNotTrack", Value::make_null());
    JsObject* languages = vm.new_array();
    vm.array_push(languages, str_value(vm, "en-US"));
    vm.define_named(n, "languages", Value::make_object(languages));
  }
  JsObject* performance = vm.new_plain();
  vm.define_native(performance, "now", performance_now, 0);
  vm.define_native(performance, "mark", win_do_nothing, 1);
  vm.define_native(performance, "measure", win_do_nothing, 1);
  vm.define_native(performance, "getEntriesByName", empty_array_native, 1);
  vm.define_native(performance, "getEntriesByType", empty_array_native, 1);
  vm.define_named(performance, "timeOrigin", num_value(vm.clock_ms()));
  {   // performance.timing (the old Navigation Timing API that pages still read): every moment is when the page started
    JsObject* timing = vm.new_plain();
    for (const char* name : {"navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd", "fetchStart", "domainLookupStart", "domainLookupEnd",
                             "connectStart", "connectEnd", "secureConnectionStart", "requestStart", "responseStart", "responseEnd", "domLoading", "domInteractive",
                             "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete", "loadEventStart", "loadEventEnd"})
      vm.define_named(timing, name, num_value(vm.clock_ms()));
    vm.define_named(performance, "timing", Value::make_object(timing));
    JsObject* navigation = vm.new_plain();
    vm.define_named(navigation, "type", num_value(0));
    vm.define_named(navigation, "redirectCount", num_value(0));
    vm.define_named(performance, "navigation", Value::make_object(navigation));
  }
  vm.define_named(g, "performance", Value::make_object(performance), kWritable);

  // console extras
  Value console;
  if (vm.get_named(Value::make_object(g), "console", &console) && console.is_object()) {
    JsObject* o = console.as_object();
    vm.define_native(o, "assert", console_assert, 2);
    vm.define_native(o, "dir", console_log_like, 1);
    vm.define_native(o, "table", console_log_like, 1);
    vm.define_native(o, "group", console_log_like, 1);
    vm.define_native(o, "groupCollapsed", console_log_like, 1);
    vm.define_native(o, "groupEnd", win_do_nothing, 0);
    vm.define_native(o, "trace", console_log_like, 1);
    vm.define_native(o, "clear", win_do_nothing, 0);
    vm.define_native(o, "time", console_time, 1);
    vm.define_native(o, "timeEnd", console_time_end, 1);
    vm.define_native(o, "timeLog", console_time_log, 1);
    vm.define_native(o, "count", console_count, 1);
  }

  // observers and the like exist but never report
  for (const char* name : {"MutationObserver", "PerformanceObserver"}) {
    JsObject* proto = vm.new_plain();
    vm.define_native(proto, "observe", win_do_nothing, 1);
    vm.define_native(proto, "unobserve", win_do_nothing, 1);
    vm.define_native(proto, "disconnect", win_do_nothing, 0);
    vm.define_native(proto, "takeRecords", empty_array_native, 0);
    JsObject* f = vm.new_native(observer_constructor, name, 1);
    vm.define(f, vm.prototype_name(), Value::make_object(proto), 0);
    vm.define(proto, vm.intern("constructor"), Value::make_object(f), kWritable);
    vm.define_named(g, name, Value::make_object(f), kWritable);
  }
  update_metrics(c);
  return !vm.halted();
}

}  // namespace bind

void DomBindings::set_ready_state(StringView state) {
  Impl& c = *impl_;
  vm_.define_named(wrap(c.doc.root()), "readyState", bind::str_value(vm_, state), script::kWritable);
}

void DomBindings::set_hidden(bool hidden) { impl_->page_hidden = hidden; }

void DomBindings::set_scroll(int32_t y) {
  impl_->scroll_line = y < 0 ? 0 : y;
  bind::update_metrics(*impl_);
}

void DomBindings::update_window_metrics() { bind::update_metrics(*impl_); }

uint32_t DomBindings::media_changed() { return bind::media_changed_impl(*impl_); }

}  // namespace browser
