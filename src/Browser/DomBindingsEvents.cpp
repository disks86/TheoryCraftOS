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

// Events: the Event objects and their constructors, addEventListener and friends, dispatching with a capture phase, a target phase and a bubble phase,
// on* handlers (properties and attributes), and what a click does when no listener stopped it.
#include <initializer_list>
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Url.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

void fire_error_event(DomBindings::Impl& c, StringView text) {
  if (c.in_error_event) return;
  Vm& vm = c.vm;
  Value hook;
  if (!vm.get_named(Value::make_object(vm.global()), "__window_error", &hook) || !Vm::is_callable(hook)) return;
  c.in_error_event = true;
  const Value arg = Value::make_string(vm.new_string(text));
  Value ignored;
  if (!vm.call(hook, Value(), &arg, 1, &ignored)) {
    String dropped(vm.allocator());
    vm.take_exception(&dropped);
  }
  c.in_error_event = false;
}

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

JsString* hidden(Vm& vm, const char* name) { return vm.intern(name); }

// The listeners of an object: { type: [[function, capture, once], ...] }, kept in a property of its own that scripts do not see.
JsObject* listeners_of(Vm& vm, JsObject* target, bool create) {
  Value v;
  JsString* key = hidden(vm, "__l");
  if (vm.get_plain(target, key, &v) && v.is_object()) return v.as_object();
  if (!create) return nullptr;
  JsObject* l = vm.new_plain();
  vm.define(target, key, Value::make_object(l), 0);
  return l;
}

bool read_options(Vm& vm, const Value& options, bool* capture, bool* once) {
  *capture = *once = false;
  if (options.is_object()) {
    Value v;
    if (!vm.get_named(options, "capture", &v)) return false;
    *capture = Vm::to_boolean(v);
    if (!vm.get_named(options, "once", &v)) return false;
    *once = Vm::to_boolean(v);
  } else {
    *capture = Vm::to_boolean(options);
  }
  return true;
}

JsObject* event_target_object(Vm& vm, const Value& self) {
  if (self.is_object()) return self.as_object();
  if (self.is_nullish()) return vm.global();   // (addEventListener(...) called on its own)
  vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  return nullptr;
}

NATIVE(add_event_listener) {
  JsObject* target = event_target_object(vm, self);
  String type(vm.allocator());
  if (!target || !arg_text(vm, arg_at(args, argc, 0), &type)) return false;
  const Value fn = arg_at(args, argc, 1);
  if (!Vm::is_callable(fn) && !fn.is_object()) return true;   // (null: nothing to add)
  bool capture, once;
  if (!read_options(vm, arg_at(args, argc, 2), &capture, &once)) return false;
  JsObject* l = listeners_of(vm, target, true);
  JsString* key = vm.intern(type.view());
  Value list_value;
  JsObject* list;
  if (vm.get_plain(l, key, &list_value) && list_value.is_object()) {
    list = list_value.as_object();
  } else {
    list = vm.new_array();
    vm.define(l, key, Value::make_object(list), kWritable);
  }
  for (uint32_t i = 0; i < list->length; ++i) {   // (the same function for the same phase is added once)
    JsObject* e = list->items[i].as_object();
    if (vm.strict_equals(e->items[0], fn) && Vm::to_boolean(e->items[1]) == capture) return true;
  }
  JsObject* entry = vm.new_array(3);
  vm.array_push(entry, fn);
  vm.array_push(entry, bool_value(capture));
  vm.array_push(entry, bool_value(once));
  vm.array_push(list, Value::make_object(entry));
  return true;
}

NATIVE(remove_event_listener) {
  JsObject* target = event_target_object(vm, self);
  String type(vm.allocator());
  if (!target || !arg_text(vm, arg_at(args, argc, 0), &type)) return false;
  bool capture, once;
  if (!read_options(vm, arg_at(args, argc, 2), &capture, &once)) return false;
  JsObject* l = listeners_of(vm, target, false);
  Value list_value;
  if (!l || !vm.get_plain(l, vm.intern(type.view()), &list_value) || !list_value.is_object()) return true;
  JsObject* list = list_value.as_object();
  for (uint32_t i = 0; i < list->length; ++i) {
    JsObject* e = list->items[i].as_object();
    if (vm.strict_equals(e->items[0], arg_at(args, argc, 1)) && Vm::to_boolean(e->items[1]) == capture) {
      for (uint32_t k = i + 1; k < list->length; ++k) list->items[k - 1] = list->items[k];
      --list->length;
      break;
    }
  }
  return true;
}

NATIVE(dispatch_event_native) {
  DomBindings::Impl& c = impl(vm);
  const Value event = arg_at(args, argc, 0);
  if (!event.is_object()) return vm.throw_error(ErrorKind::TypeError, "parameter 1 is not of type 'Event'.");
  dom::Node* node = node_of(self);
  const bool window = !node && (self.is_nullish() || (self.is_object() && self.as_object() == vm.global()));
  if (!node && !window && !self.is_object()) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  bool prevented = false;
  vm.hold_collector();
  const bool ok = dispatch_event(c, node, window, event, &prevented, !node && !window ? self.as_object() : nullptr);
  vm.release_collector();
  *result = bool_value(!prevented);
  return ok;
}

// --- Event objects -------------------------------------------------------------------------------------------------------------

bool event_flag(Vm& vm, JsObject* e, const char* name) {
  Value v;
  return vm.get_plain(e, vm.intern(name), &v) && Vm::to_boolean(v);
}

NATIVE(event_prevent_default) {
  if (!self.is_object()) return true;
  JsObject* e = self.as_object();
  Value cancelable;
  if (vm.get_plain(e, vm.intern("cancelable"), &cancelable) && Vm::to_boolean(cancelable)) vm.define_named(e, "defaultPrevented", bool_value(true));
  return true;
}

NATIVE(event_stop_propagation) {
  if (self.is_object()) vm.define(self.as_object(), hidden(vm, "__stop"), bool_value(true), 0);
  return true;
}

NATIVE(event_stop_immediate) {
  if (self.is_object()) {
    vm.define(self.as_object(), hidden(vm, "__stop"), bool_value(true), 0);
    vm.define(self.as_object(), hidden(vm, "__stopi"), bool_value(true), 0);
  }
  return true;
}

NATIVE(event_composed_path) {
  Value path;
  if (self.is_object() && vm.get_plain(self.as_object(), hidden(vm, "__path"), &path) && path.is_object()) {
    JsObject* copy = vm.new_array(path.as_object()->length);
    for (uint32_t i = 0; i < path.as_object()->length; ++i) vm.array_push(copy, path.as_object()->items[i]);
    *result = Value::make_object(copy);
  } else {
    *result = Value::make_object(vm.new_array());
  }
  return true;
}

NATIVE(event_init) {   // initEvent(type, bubbles, cancelable) and initCustomEvent(type, bubbles, cancelable, detail)
  if (!self.is_object()) return true;
  JsObject* e = self.as_object();
  vm.define_named(e, "type", arg_at(args, argc, 0));
  vm.define_named(e, "bubbles", bool_value(Vm::to_boolean(arg_at(args, argc, 1))));
  vm.define_named(e, "cancelable", bool_value(Vm::to_boolean(arg_at(args, argc, 2))));
  if (argc > 3) vm.define_named(e, "detail", args[3]);
  return true;
}

void fill_event(DomBindings::Impl& c, JsObject* e, StringView type, const EventInit& init, bool trusted) {
  Vm& vm = c.vm;
  vm.define_named(e, "type", str_value(vm, type));
  vm.define_named(e, "target", Value::make_null());
  vm.define_named(e, "currentTarget", Value::make_null());
  vm.define_named(e, "srcElement", Value::make_null());
  vm.define_named(e, "bubbles", bool_value(init.bubbles));
  vm.define_named(e, "cancelable", bool_value(init.cancelable));
  vm.define_named(e, "composed", bool_value(false));
  vm.define_named(e, "defaultPrevented", bool_value(false));
  vm.define_named(e, "eventPhase", num_value(0));
  vm.define_named(e, "isTrusted", bool_value(trusted));
  vm.define_named(e, "timeStamp", num_value(vm.clock_ms()));
  vm.define_named(e, "detail", num_value(init.detail));
  vm.define_named(e, "clientX", num_value(init.client_x));
  vm.define_named(e, "clientY", num_value(init.client_y));
  vm.define_named(e, "pageX", num_value(init.client_x));
  vm.define_named(e, "pageY", num_value(init.client_y));
  vm.define_named(e, "screenX", num_value(init.client_x));
  vm.define_named(e, "screenY", num_value(init.client_y));
  vm.define_named(e, "offsetX", num_value(init.client_x));
  vm.define_named(e, "offsetY", num_value(init.client_y));
  vm.define_named(e, "button", num_value(init.button));
  vm.define_named(e, "buttons", num_value(0));
  vm.define_named(e, "which", num_value(init.button + 1));
  vm.define_named(e, "ctrlKey", bool_value(init.ctrl));
  vm.define_named(e, "shiftKey", bool_value(init.shift));
  vm.define_named(e, "altKey", bool_value(init.alt));
  vm.define_named(e, "metaKey", bool_value(false));
  if (init.key[0]) {
    vm.define_named(e, "key", str_value(vm, init.key));
    vm.define_named(e, "code", str_value(vm, init.key));
    vm.define_named(e, "keyCode", num_value(init.key_code));
    vm.define_named(e, "which", num_value(init.key_code));
    vm.define_named(e, "charCode", num_value(0));
    vm.define_named(e, "repeat", bool_value(false));
  }
  if (type.equals("message")) {
    vm.define_named(e, "data", str_value(vm, init.message_data));
    vm.define_named(e, "origin", str_value(vm, init.message_origin));
    vm.define_named(e, "lastEventId", str_value(vm, ""));
    vm.define_named(e, "source", Value::make_null());
    vm.define_named(e, "ports", Value::make_object(vm.new_array()));
  }
  if (type.equals("submit")) vm.define_named(e, "submitter", node_value(c, init.submitter));
}

// new Event(type, init) and the like: `this` is the object the constructor call made.
NATIVE(event_constructor) {
  DomBindings::Impl& c = impl(vm);
  if (!self.is_object()) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'Event': Please use the 'new' operator.");
  JsObject* e = self.as_object();
  String type(vm.allocator());
  if (argc == 0) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'Event': 1 argument required, but only 0 present.");
  if (!arg_text(vm, args[0], &type)) return false;
  EventInit init;
  init.bubbles = false;
  init.cancelable = false;
  fill_event(c, e, type.view(), init, false);
  const Value options = arg_at(args, argc, 1);
  if (options.is_object()) {   // the options become properties of the event (bubbles, cancelable, detail, clientX, key ...)
    JsObject* keys = vm.own_keys(options.as_object());
    for (uint32_t i = 0; i < keys->length; ++i) {
      Value v;
      if (!vm.get(options, keys->items[i].as_string(), &v)) return false;
      vm.define(e, keys->items[i].as_string(), v);
    }
  }
  *result = self;
  return true;
}

NATIVE(do_nothing) { return true; }

// --- Dispatching -----------------------------------------------------------------------------------------------------------------

void report_error(DomBindings::Impl& c, StringView what) {
  Vm& vm = c.vm;
  String text(vm.allocator());
  vm.take_exception(&text);
  if (c.error_count++ == 0) c.first_error.assign(text.view());
  fire_error_event(c, text.view());
  if (vm.host()) {
    String line(vm.allocator());
    line.append("Uncaught ");
    line.append(text.view());
    line.append(" (in ");
    line.append(what);
    line.append(")");
    vm.host()->console(3, line.view());
  }
}

// Calls one function (or object with handleEvent) with the event.
void call_listener(DomBindings::Impl& c, const Value& listener, JsObject* current, const Value& event, StringView type) {
  Vm& vm = c.vm;
  Value fn = listener, self = Value::make_object(current);
  if (!Vm::is_callable(listener)) {
    Value handle;
    if (!vm.get_named(listener, "handleEvent", &handle) || !Vm::is_callable(handle)) return;
    fn = handle;
    self = listener;
  }
  Value ignored;
  if (!vm.call(fn, self, &event, 1, &ignored)) report_error(c, type);
}

// The on<type> handler of a node or the window, if it has one: the property, else the attribute (compiled once).
bool inline_handler(DomBindings::Impl& c, dom::Node* node, JsObject* wrapper, StringView type, Value* out) {
  Vm& vm = c.vm;
  String name(vm.allocator());
  name.append("on");
  name.append(type);
  Value v;
  if (vm.get_plain(wrapper, vm.intern(name.view()), &v) && Vm::is_callable(v)) {
    *out = v;
    return true;
  }
  dom::Node* holder = node;
  if (!holder && wrapper == vm.global()) {   // the window's handlers can be attributes of <body> (onload="...")
    holder = c.doc.body();
    if (!(type == StringView("load") || type == StringView("unload") || type == StringView("resize") || type == StringView("scroll") || type == StringView("hashchange") ||
          type == StringView("popstate") || type == StringView("beforeunload") || type == StringView("pageshow") || type == StringView("pagehide")))
      holder = nullptr;
  }
  StringView code;
  if (!holder || !holder->is_element() || !dom::get_attribute(holder, name.view(), &code) || code.empty()) return false;
  JsObject* store = holder->script ? static_cast<JsObject*>(holder->script) : wrapper;
  String cache_name(vm.allocator());
  cache_name.append("__attr_");
  cache_name.append(name.view());
  JsString* cache_key = vm.intern(cache_name.view());
  if (vm.get_plain(store, cache_key, &v)) {
    if (Vm::is_callable(v)) {
      *out = v;
      return true;
    }
    return false;   // (it did not compile: said once)
  }
  Value fn;
  if (c.host.compile_handler(code, &fn) && Vm::is_callable(fn)) {
    vm.define(store, cache_key, fn, 0);
    *out = fn;
    return true;
  }
  vm.define(store, cache_key, bool_value(false), 0);
  return false;
}

enum class Phase { Capture, TargetCapture, Target, Bubble };

// Runs the listeners of `current` for this phase. Returns false when stopImmediatePropagation() was called.
bool invoke(DomBindings::Impl& c, dom::Node* node, JsObject* current, StringView type, Phase phase, const Value& event) {
  Vm& vm = c.vm;
  JsObject* e = event.as_object();
  vm.define_named(e, "currentTarget", Value::make_object(current));
  vm.define_named(e, "eventPhase", num_value(phase == Phase::Capture ? 1 : phase == Phase::Bubble ? 3 : 2));
  // the handler property / attribute runs with the non-capturing listeners
  if (phase == Phase::Target || phase == Phase::Bubble) {
    Value handler;
    if (inline_handler(c, node, current, type, &handler)) {
      Value ignored;
      if (!vm.call(handler, Value::make_object(current), &event, 1, &ignored)) report_error(c, type);
      if (event_flag(vm, e, "__stopi")) return false;
    }
  }
  JsObject* l = listeners_of(vm, current, false);
  Value list_value;
  if (!l || !vm.get_plain(l, vm.intern(type), &list_value) || !list_value.is_object()) return true;
  JsObject* list = list_value.as_object();
  // (a copy: listeners may add and remove listeners while this runs)
  List<Value> entries(vm.allocator());
  for (uint32_t i = 0; i < list->length; ++i) entries.push_back(list->items[i]);
  for (uint64_t i = 0; i < entries.count(); ++i) {
    JsObject* entry = entries[i].as_object();
    const bool capture = Vm::to_boolean(entry->items[1]);
    if ((phase == Phase::Capture || phase == Phase::TargetCapture) != capture) continue;
    bool still = false;
    for (uint32_t k = 0; k < list->length && !still; ++k) still = list->items[k].as_object() == entry;
    if (!still) continue;   // (removed meanwhile)
    if (Vm::to_boolean(entry->items[2])) {   // once: gone before it runs
      for (uint32_t k = 0; k < list->length; ++k) {
        if (list->items[k].as_object() == entry) {
          for (uint32_t m = k + 1; m < list->length; ++m) list->items[m - 1] = list->items[m];
          --list->length;
          break;
        }
      }
    }
    call_listener(c, entry->items[0], current, event, type);
    if (event_flag(vm, e, "__stopi")) return false;
  }
  return true;
}

}  // namespace

Value make_event(DomBindings::Impl& c, StringView type, const EventInit& init, JsObject* proto) {
  Vm& vm = c.vm;
  JsObject* e = vm.new_object(proto ? proto : c.event_proto);
  fill_event(c, e, type, init, true);
  return Value::make_object(e);
}

bool dispatch_event(DomBindings::Impl& c, dom::Node* target, bool target_is_window, const Value& event, bool* prevented, JsObject* object_target) {
  Vm& vm = c.vm;
  JsObject* e = event.as_object();
  String type(vm.allocator());
  Value type_value;
  vm.get_named(event, "type", &type_value);
  if (!arg_text(vm, type_value, &type)) {
    report_error(c, "dispatchEvent");
    return true;
  }
  // the path from the target out to the window
  List<dom::Node*> nodes(vm.allocator());
  if (!target_is_window)
    for (dom::Node* n = target; n && nodes.count() < 4096; n = dom::event_parent(n)) nodes.push_back(n);
  JsObject* path = vm.new_array(static_cast<uint32_t>(nodes.count()) + 1);
  if (object_target) vm.array_push(path, Value::make_object(object_target));   // (a script object that is an event target: it is the whole path)
  for (uint64_t i = 0; i < nodes.count(); ++i) vm.array_push(path, node_value(c, nodes[i]));
  // (the window is on the path of events that go through the document, but not of load, which is the window's)
  const bool to_window = target_is_window || (nodes.count() && nodes[nodes.count() - 1]->type == dom::NodeType::Document);
  if (to_window) vm.array_push(path, Value::make_object(vm.global()));
  vm.define(e, vm.intern("__path"), Value::make_object(path), 0);
  vm.define(e, vm.intern("__stop"), bool_value(false), 0);
  vm.define(e, vm.intern("__stopi"), bool_value(false), 0);
  vm.define_named(e, "target", path->length ? path->items[0] : Value::make_null());
  vm.define_named(e, "srcElement", path->length ? path->items[0] : Value::make_null());
  Value bubbles_value;
  vm.get_named(event, "bubbles", &bubbles_value);
  const bool bubbles = Vm::to_boolean(bubbles_value);
  auto node_at = [&](uint32_t i) -> dom::Node* { return i < nodes.count() ? nodes[i] : nullptr; };
  bool stopped = false;
  for (uint32_t i = path->length; i > 1 && !stopped; --i) {   // capture: from the outside in
    JsObject* current = path->items[i - 1].as_object();
    invoke(c, node_at(i - 1), current, type.view(), Phase::Capture, event);
    stopped = event_flag(vm, e, "__stop");
  }
  if (!stopped && path->length > 0) {
    JsObject* current = path->items[0].as_object();
    if (invoke(c, node_at(0), current, type.view(), Phase::TargetCapture, event)) {   // (capturing listeners of the target, then the others)
      invoke(c, node_at(0), current, type.view(), Phase::Target, event);
    }
    stopped = event_flag(vm, e, "__stop");
  }
  for (uint32_t i = 1; bubbles && i < path->length && !stopped; ++i) {   // bubble: from the inside out
    JsObject* current = path->items[i].as_object();
    invoke(c, node_at(i), current, type.view(), Phase::Bubble, event);
    stopped = event_flag(vm, e, "__stop");
  }
  vm.define_named(e, "currentTarget", Value::make_null());
  vm.define_named(e, "eventPhase", num_value(0));
  Value dp;
  vm.get_named(event, "defaultPrevented", &dp);
  *prevented = Vm::to_boolean(dp);
  return true;
}

NATIVE(el_click) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* node = node_of(self);
  if (!node) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  EventInit init;
  if (c.owner.dispatch(node, "click", init)) c.owner.activate(node);
  return true;
}

NATIVE(doc_create_event) {
  DomBindings::Impl& c = impl(vm);
  EventInit init;
  init.bubbles = init.cancelable = false;
  *result = make_event(c, "", init);
  return true;
}

bool install_events(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  Value target_proto;
  vm.get_named(Value::make_object(g), "__event_target_proto", &target_proto);
  JsObject* tp = target_proto.as_object();
  vm.define_native(tp, "addEventListener", add_event_listener, 2);
  vm.define_native(tp, "removeEventListener", remove_event_listener, 2);
  vm.define_native(tp, "dispatchEvent", dispatch_event_native, 1);
  vm.define_native(c.element_proto, "click", el_click, 0);
  vm.define_native(c.document_proto, "createEvent", doc_create_event, 1);
  // the window is an event target too: the global functions
  vm.define_native(g, "addEventListener", add_event_listener, 2);
  vm.define_native(g, "removeEventListener", remove_event_listener, 2);
  vm.define_native(g, "dispatchEvent", dispatch_event_native, 1);

  c.event_proto = vm.new_plain();
  vm.define_native(c.event_proto, "preventDefault", event_prevent_default, 0);
  vm.define_native(c.event_proto, "stopPropagation", event_stop_propagation, 0);
  vm.define_native(c.event_proto, "stopImmediatePropagation", event_stop_immediate, 0);
  vm.define_native(c.event_proto, "composedPath", event_composed_path, 0);
  vm.define_native(c.event_proto, "initEvent", event_init, 3);
  vm.define_native(c.event_proto, "initCustomEvent", event_init, 4);
  static const char* const kEventClasses[] = {"Event", "CustomEvent", "UIEvent", "MouseEvent", "KeyboardEvent", "FocusEvent", "InputEvent", "PointerEvent", "WheelEvent", "TouchEvent",
                                              "MessageEvent", "ErrorEvent", "HashChangeEvent", "PopStateEvent", "StorageEvent", "ProgressEvent", "AnimationEvent", "TransitionEvent"};
  for (const char* name : kEventClasses) {
    JsObject* f = vm.new_native(event_constructor, name, 1);
    JsObject* proto = StringView(name).equals("Event") ? c.event_proto : vm.new_object(c.event_proto);
    vm.define(f, vm.prototype_name(), Value::make_object(proto), 0);
    vm.define(proto, vm.intern("constructor"), Value::make_object(f), kWritable);
    vm.define_named(g, name, Value::make_object(f), kWritable);
  }
  return !vm.halted();
}

}  // namespace bind

// --- What the page offers -------------------------------------------------------------------------------------------------------

void DomBindings::activate(dom::Node* node) {
  Impl& c = *impl_;
  using namespace bind;
  if (node && c.host.activate_control(node)) return;   // (a form control, a label, an option)
  // the nearest link
  for (dom::Node* n = node; n && n->is_element(); n = n->parent) {
    StringView href;
    if ((n->is_tag("a") || n->is_tag("area")) && dom::get_attribute(n, "href", &href) && !href.empty()) {
      if (href.starts_with("javascript:")) {
        Value fn;
        vm_.hold_collector();
        if (c.host.compile_handler(href.substr(11), &fn)) {
          Value ignored;
          if (!vm_.call(fn, Value::make_object(vm_.global()), nullptr, 0, &ignored)) {
            String text(vm_.allocator());
            vm_.take_exception(&text);
            if (c.error_count++ == 0) c.first_error.assign(text.view());
            fire_error_event(c, text.view());
          }
        }
        vm_.release_collector();
        return;
      }
      String target(vm_.allocator());
      if (href.starts_with('#')) {
        c.host.set_hash(href);
      } else if (resolve_reference(vm_.allocator(), c.host.address(), href, &target)) {
        c.host.navigate(target.view(), false);
      }
      return;
    }
  }
}

}  // namespace browser
