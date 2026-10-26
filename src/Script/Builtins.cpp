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

// The global functions and objects scripts find: Object, Function, Array, String, Number, Boolean, Math, JSON, Error and its kinds, Date (a little),
// console, document (write, title, a few stubs that find nothing), window / location / navigator, timers, and the parse and test functions.
// Promise has its core here (the constructor, resolve, reject, then, catch); all, allSettled, race, any and finally are written in script (see Engine.cpp).
// Regular expressions are in RegExp.cpp, typed arrays in TypedArray.cpp; Map, Set and the rest of the standard library are written in script (Engine.cpp,
// Stdlib.cpp). Not here: the DOM.
#include <initializer_list>
#include "Math/FloatMath.hpp"
#include "Script/Number.hpp"
#include "Script/Ast.hpp"
#include "Script/Compiler.hpp"
#include "Script/RegExp.hpp"
#include "Script/TypedArray.hpp"
#include "Script/Vm.hpp"

namespace script {

namespace {

constexpr double kInf = __builtin_inf();
const double kNaN = __builtin_nan("");

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

Value arg_at(const Value* args, uint32_t argc, uint32_t i) { return i < argc ? args[i] : Value(); }
Value str_value(Vm& vm, StringView text) { return Value::make_string(vm.new_string(text)); }
Value num_value(double d) { return Value::make_number(d); }

bool arg_string(Vm& vm, const Value* args, uint32_t argc, uint32_t i, JsString** out) { return vm.to_string(arg_at(args, argc, i), out); }
bool arg_number(Vm& vm, const Value* args, uint32_t argc, uint32_t i, double* out) { return vm.to_number(arg_at(args, argc, i), out); }

// An integer argument (NaN -> 0), clamped to what fits an int64.
bool arg_integer(Vm& vm, const Value* args, uint32_t argc, uint32_t i, int64_t fallback, int64_t* out) {
  if (i >= argc || args[i].is_undefined()) {
    *out = fallback;
    return true;
  }
  double d;
  if (!vm.to_number(args[i], &d)) return false;
  if (d != d) d = 0;
  *out = d > 9e18 ? INT64_MAX : d < -9e18 ? INT64_MIN : static_cast<int64_t>(d);
  return true;
}

// A relative index (negative counts from the end) clamped to 0..length.
int64_t relative(int64_t index, int64_t length) {
  if (index < 0) {
    index += length;
    return index < 0 ? 0 : index;
  }
  return index > length ? length : index;
}

bool this_string(Vm& vm, const Value& self, JsString** out) {
  if (self.is_nullish()) return vm.throw_error(ErrorKind::TypeError, "String.prototype method called on null or undefined");
  return vm.to_string(self, out);
}

// What the Array.prototype methods work on: an array, or an array-like object (a NodeList, a typed array, {length: n, 0: ...}) as a copy of its elements. (The
// methods that change the array change the copy then; they are meant for arrays.) A native runs without the collector, so the copy needs no root.
bool this_array(Vm& vm, const Value& self, JsObject** out) {
  if (self.is_object() && self.as_object()->okind == ObjKind::Array) {
    *out = self.as_object();
    return true;
  }
  Value length_value;
  double length = 0;
  if (!self.is_object() || !vm.get_named(self, "length", &length_value) || !length_value.is_number() || !(length_value.u.number >= 0))
    return vm.throw_error(ErrorKind::TypeError, "Array.prototype method called on a value that is not an array");
  length = length_value.u.number;
  if (length > 16777216.0) length = 16777216.0;
  const uint32_t count = static_cast<uint32_t>(length);
  JsObject* copy = vm.new_array(count);
  for (uint32_t i = 0; i < count; ++i) {
    String key(vm.allocator());
    key.append_uint(i);
    Value item;
    if (!vm.get(self, vm.intern(key.view()), &item)) return false;
    vm.array_push(copy, item);
  }
  *out = copy;
  return true;
}

bool need_function(Vm& vm, const Value& v) {
  if (Vm::is_callable(v)) return true;
  String message(vm.allocator());
  vm.describe(v, &message, 1);
  message.append(" is not a function");
  return vm.throw_error(ErrorKind::TypeError, message.view());
}

// --- Object -------------------------------------------------------------------------------------------------------

NATIVE(object_ctor) {
  const Value v = arg_at(args, argc, 0);
  *result = v.is_object() ? v : Value::make_object(vm.new_plain());
  return true;
}

NATIVE(object_keys) {
  const Value v = arg_at(args, argc, 0);
  if (!v.is_object()) {
    *result = Value::make_object(vm.new_array());
    return true;
  }
  *result = Value::make_object(vm.own_keys(v.as_object()));
  return true;
}

NATIVE(object_values) {
  const Value v = arg_at(args, argc, 0);
  JsObject* out = vm.new_array();
  if (v.is_object()) {
    JsObject* keys = vm.own_keys(v.as_object());
    for (uint32_t i = 0; i < keys->length; ++i) {
      Value item;
      if (!vm.get(v, keys->items[i].as_string(), &item)) return false;
      vm.array_push(out, item);
    }
  }
  *result = Value::make_object(out);
  return true;
}

NATIVE(object_entries) {
  const Value v = arg_at(args, argc, 0);
  JsObject* out = vm.new_array();
  if (v.is_object()) {
    JsObject* keys = vm.own_keys(v.as_object());
    for (uint32_t i = 0; i < keys->length; ++i) {
      Value item;
      if (!vm.get(v, keys->items[i].as_string(), &item)) return false;
      JsObject* pair = vm.new_array(2);
      vm.array_push(pair, keys->items[i]);
      vm.array_push(pair, item);
      vm.array_push(out, Value::make_object(pair));
    }
  }
  *result = Value::make_object(out);
  return true;
}

NATIVE(object_assign) {
  const Value target = arg_at(args, argc, 0);
  if (!target.is_object()) return vm.throw_error(ErrorKind::TypeError, "Cannot convert undefined or null to object");
  for (uint32_t a = 1; a < argc; ++a) {
    if (!args[a].is_object()) continue;
    JsObject* keys = vm.own_keys(args[a].as_object());
    for (uint32_t i = 0; i < keys->length; ++i) {
      Value item;
      if (!vm.get(args[a], keys->items[i].as_string(), &item) || !vm.put(target, keys->items[i].as_string(), item)) return false;
    }
  }
  *result = target;
  return true;
}

NATIVE(object_create) {
  const Value proto = arg_at(args, argc, 0);
  *result = Value::make_object(vm.new_object(proto.is_object() ? proto.as_object() : nullptr));
  return true;
}

NATIVE(object_freeze) {
  const Value v = arg_at(args, argc, 0);
  if (v.is_object()) v.as_object()->frozen = true;
  *result = v;
  return true;
}

NATIVE(object_is_frozen) {
  const Value v = arg_at(args, argc, 0);
  *result = Value::make_bool(!v.is_object() || v.as_object()->frozen);
  return true;
}

NATIVE(object_get_prototype_of) {
  const Value v = arg_at(args, argc, 0);
  *result = v.is_object() && v.as_object()->proto ? Value::make_object(v.as_object()->proto) : Value::make_null();
  return true;
}

NATIVE(object_define_property) {
  const Value target = arg_at(args, argc, 0), desc = arg_at(args, argc, 2);
  if (!target.is_object() || !desc.is_object()) return vm.throw_error(ErrorKind::TypeError, "Object.defineProperty called on non-object");
  JsString* key;
  if (!vm.to_property_key(arg_at(args, argc, 1), &key)) return false;
  Value value, enumerable, writable, getter, setter;
  if (!vm.get_named(desc, "value", &value) || !vm.get_named(desc, "enumerable", &enumerable) || !vm.get_named(desc, "writable", &writable) || !vm.get_named(desc, "get", &getter) ||
      !vm.get_named(desc, "set", &setter))
    return false;
  if (!getter.is_undefined() || !setter.is_undefined()) {
    if ((!getter.is_undefined() && !Vm::is_callable(getter)) || (!setter.is_undefined() && !Vm::is_callable(setter)))
      return vm.throw_error(ErrorKind::TypeError, "Getter must be a function");
    vm.define_accessor(target.as_object(), key, getter, setter, Vm::to_boolean(enumerable) ? kEnumerable : 0);
    *result = target;
    return true;
  }
  vm.define(target.as_object(), key, value, static_cast<uint8_t>((Vm::to_boolean(enumerable) ? kEnumerable : 0) | (Vm::to_boolean(writable) ? kWritable : 0)));
  *result = target;
  return true;
}

NATIVE(object_define_properties) {
  const Value target = arg_at(args, argc, 0), props = arg_at(args, argc, 1);
  if (!target.is_object() || !props.is_object()) return vm.throw_error(ErrorKind::TypeError, "Object.defineProperties called on non-object");
  JsObject* keys = vm.own_keys(props.as_object());
  for (uint32_t i = 0; i < keys->length; ++i) {
    Value desc;
    if (!vm.get(props, keys->items[i].as_string(), &desc)) return false;
    const Value call_args[3] = {target, keys->items[i], desc};
    Value ignored;
    if (!object_define_property(vm, Value(), call_args, 3, &ignored)) return false;
  }
  *result = target;
  return true;
}

NATIVE(object_get_own_property_descriptor) {
  const Value target = arg_at(args, argc, 0);
  if (!target.is_object()) {
    *result = Value();
    return true;
  }
  JsString* key;
  if (!vm.to_property_key(arg_at(args, argc, 1), &key)) return false;
  Value value;
  uint8_t flags;
  if (!vm.own_property(target.as_object(), key, &value, &flags)) {
    *result = Value();
    return true;
  }
  JsObject* d = vm.new_plain();
  if (flags & kAccessor) {
    const bool pair = value.is_object() && value.as_object()->length >= 2;
    vm.define_named(d, "get", pair ? value.as_object()->items[0] : Value());
    vm.define_named(d, "set", pair ? value.as_object()->items[1] : Value());
  } else {
    vm.define_named(d, "value", value);
    vm.define_named(d, "writable", Value::make_bool((flags & kWritable) != 0 && !target.as_object()->frozen));
  }
  vm.define_named(d, "enumerable", Value::make_bool((flags & kEnumerable) != 0));
  vm.define_named(d, "configurable", Value::make_bool(!target.as_object()->frozen));
  *result = Value::make_object(d);
  return true;
}

NATIVE(object_get_own_property_names) {
  const Value target = arg_at(args, argc, 0);
  if (!target.is_object()) {
    *result = Value::make_object(vm.new_array());
    return true;
  }
  *result = Value::make_object(vm.own_names(target.as_object()));
  return true;
}

NATIVE(object_set_prototype_of) {
  const Value target = arg_at(args, argc, 0), proto = arg_at(args, argc, 1);
  if (target.is_object() && (proto.is_object() || proto.is_null())) {
    for (JsObject* p = proto.is_object() ? proto.as_object() : nullptr; p; p = p->proto)
      if (p == target.as_object()) return vm.throw_error(ErrorKind::TypeError, "Cyclic __proto__ value");
    target.as_object()->proto = proto.is_object() ? proto.as_object() : nullptr;
  }
  *result = target;
  return true;
}

// --- Classes (the parser turns `class` into a function and calls of these) -----------------------------------------------

// __class(constructor, parent): the constructor's prototype, chained to the parent's (and the constructor to the parent, for statics). Returns the prototype.
NATIVE(class_setup) {
  const Value ctor = arg_at(args, argc, 0), parent = arg_at(args, argc, 1);
  if (!ctor.is_object()) return vm.throw_error(ErrorKind::TypeError, "Class constructor is not a function");
  Value proto;
  if (!vm.get_named(ctor, "prototype", &proto)) return false;
  if (!proto.is_object()) return vm.throw_error(ErrorKind::TypeError, "Class has no prototype");
  if (argc > 1 && !parent.is_undefined()) {
    if (parent.is_null()) {
      proto.as_object()->proto = nullptr;
    } else if (!Vm::is_callable(parent)) {
      String message(vm.allocator());
      message.append("Class extends value ");
      vm.describe(parent, &message, 0);
      message.append(" is not a constructor or null");
      return vm.throw_error(ErrorKind::TypeError, message.view());
    } else {
      Value parent_proto;
      if (!vm.get_named(parent, "prototype", &parent_proto)) return false;
      proto.as_object()->proto = parent_proto.is_object() ? parent_proto.as_object() : nullptr;
      ctor.as_object()->proto = parent.as_object();
    }
  }
  vm.define(proto.as_object(), vm.intern("constructor"), ctor, kWritable);
  *result = proto;
  return true;
}

// __def(target, key, value, kind): 0 method (not enumerable), 1 getter, 2 setter, 3 a hidden data property (private fields).
NATIVE(class_define) {
  const Value target = arg_at(args, argc, 0), value = arg_at(args, argc, 2);
  JsString* key;
  double kind = 0;
  if (!target.is_object() || !vm.to_property_key(arg_at(args, argc, 1), &key) || !vm.to_number(arg_at(args, argc, 3), &kind)) return false;
  JsObject* t = target.as_object();
  if (kind == 1) vm.define_accessor(t, key, value, Value(), 0);
  else if (kind == 2) vm.define_accessor(t, key, Value(), value, 0);
  else vm.define(t, key, value, kWritable);
  *result = Value();
  return true;
}

// __super_init(parent, this, ...args): super(...args). A parent that is a built-in (Error ...) answers with an object of its own: what it made is copied onto `this`.
NATIVE(class_super_init) {
  const Value parent = arg_at(args, argc, 0), self_value = arg_at(args, argc, 1);
  if (!Vm::is_callable(parent)) return vm.throw_error(ErrorKind::TypeError, "Super constructor is not a constructor");
  if (self_value.is_object() && self_value.as_object()->host && parent.as_object()->okind == ObjKind::Native) {   // a class that extends a built-in of the page (HTMLElement): the object is made already
    *result = Value();
    return true;
  }
  Value made;
  vm.mark_super_call();
  const bool called = vm.call(parent, self_value, argc > 2 ? args + 2 : nullptr, argc > 2 ? argc - 2 : 0, &made);
  if (!called) return false;
  if (made.is_object() && self_value.is_object() && made.as_object() != self_value.as_object() && made.as_object()->okind != ObjKind::Array) {
    JsObject* from = made.as_object();
    JsObject* names = vm.own_names(from);
    for (uint32_t i = 0; i < names->length; ++i) {
      Value value;
      uint8_t flags;
      JsString* name = names->items[i].as_string();
      if (!vm.own_property(from, name, &value, &flags) || (flags & kAccessor)) continue;
      vm.define(self_value.as_object(), name, value, flags);
    }
  }
  *result = Value();
  return true;
}

// --- Destructuring (the parser turns patterns into calls of these) -------------------------------------------------------

// __to_array(iterable, n): the first n values (all when n < 0); an array is used as it is.
NATIVE(destructure_to_array) {
  const Value v = arg_at(args, argc, 0);
  double n = -1;
  if (argc > 1 && !vm.to_number(args[1], &n)) return false;
  if (v.is_object() && v.as_object()->okind == ObjKind::Array) {
    *result = v;
    return true;
  }
  if (v.is_nullish()) {
    String message(vm.allocator());
    message.append(v.is_null() ? "null" : "undefined");
    message.append(" is not iterable");
    return vm.throw_error(ErrorKind::TypeError, message.view());
  }
  JsObject* out = vm.new_array();
  *result = Value::make_object(out);
  return vm.iterate_into(out, v, n < 0 ? -1 : static_cast<int64_t>(n));
}

NATIVE(destructure_require_object) {
  const Value v = arg_at(args, argc, 0);
  if (v.is_nullish()) {
    String message(vm.allocator());
    message.append("Cannot destructure '");
    message.append(v.is_null() ? "null" : "undefined");
    message.append("' as it is ");
    message.append(v.is_null() ? "null" : "undefined");
    message.append(".");
    return vm.throw_error(ErrorKind::TypeError, message.view());
  }
  *result = v;
  return true;
}

// __rest(object, keys): a new object with the own enumerable properties of `object` that are not in `keys`.
NATIVE(destructure_rest) {
  const Value source = arg_at(args, argc, 0), keys = arg_at(args, argc, 1);
  JsObject* out = vm.new_plain();
  *result = Value::make_object(out);
  if (!source.is_object()) return true;
  JsObject* names = vm.own_keys(source.as_object());
  for (uint32_t i = 0; i < names->length; ++i) {
    JsString* name = names->items[i].as_string();
    bool skip = false;
    if (keys.is_object())
      for (uint32_t k = 0; k < keys.as_object()->length && !skip; ++k) {
        JsString* other;
        if (!vm.to_string(keys.as_object()->items[k], &other)) return false;
        skip = other->view().equals(name->view());
      }
    if (skip) continue;
    Value value;
    if (!vm.get(source, name, &value)) return false;
    vm.define(out, name, value);
  }
  return true;
}

// __bigint_new(negative, digits): a BigInt object (the arithmetic is in script)
NATIVE(bigint_new) {
  JsString* digits;
  if (!vm.to_string(arg_at(args, argc, 1), &digits)) return false;
  *result = Value::make_object(vm.new_bigint(Vm::to_boolean(arg_at(args, argc, 0)), digits->view()));
  return true;
}

// --- Generators ----------------------------------------------------------------------------------------------------------

NATIVE(generator_next) { return vm.generator_resume(self, 0, arg_at(args, argc, 0), result); }
NATIVE(generator_throw) { return vm.generator_resume(self, 1, arg_at(args, argc, 0), result); }
NATIVE(generator_return) { return vm.generator_resume(self, 2, arg_at(args, argc, 0), result); }
NATIVE(return_this) {
  *result = self;
  return true;
}
NATIVE(generator_to_string) {
  *result = str_value(vm, "[object Generator]");
  return true;
}

NATIVE(object_has_own_property) {
  JsString* key;
  if (!vm.to_property_key(arg_at(args, argc, 0), &key)) return false;
  Value ignored;
  *result = Value::make_bool(self.is_object() && vm.get_own(self.as_object(), key, &ignored));
  return true;
}

// __template(cooked, raw): the strings array a tag function gets
NATIVE(template_object) {
  const Value cooked = arg_at(args, argc, 0), raw = arg_at(args, argc, 1);
  if (cooked.is_object() && raw.is_object()) vm.define_named(cooked.as_object(), "raw", raw, 0);
  *result = cooked;
  return true;
}

NATIVE(object_to_string) {
  if (self.is_object()) {   // [Symbol.toStringTag]
    Value tag;
    if (vm.get_named(self, "@@toStringTag", &tag) && tag.is_string()) {
      String text(vm.allocator());
      text.append("[object ");
      text.append(tag.as_string()->view());
      text.push_back(']');
      *result = str_value(vm, text.view());
      return true;
    }
  }
  const char* name = "[object Object]";
  if (self.is_undefined()) name = "[object Undefined]";
  else if (self.is_null()) name = "[object Null]";
  else if (self.is_number()) name = "[object Number]";
  else if (self.is_string()) name = "[object String]";
  else if (self.is_bool()) name = "[object Boolean]";
  else if (self.is_object()) {
    switch (self.as_object()->okind) {
      case ObjKind::Array: name = "[object Array]"; break;
      case ObjKind::Function: case ObjKind::Native: case ObjKind::Bound: name = "[object Function]"; break;
      case ObjKind::Error: name = "[object Error]"; break;
      case ObjKind::RegExp: name = "[object RegExp]"; break;
      case ObjKind::Promise: name = "[object Promise]"; break;
      default: break;
    }
  }
  *result = str_value(vm, name);
  return true;
}

NATIVE(object_value_of) {
  *result = self;
  return true;
}

// --- Function -----------------------------------------------------------------------------------------------------

NATIVE(function_call) {
  if (!need_function(vm, self)) return false;
  return vm.call(self, arg_at(args, argc, 0), argc > 1 ? args + 1 : nullptr, argc > 1 ? argc - 1 : 0, result);
}

NATIVE(function_apply) {
  if (!need_function(vm, self)) return false;
  const Value list = arg_at(args, argc, 1);
  if (list.is_object() && list.as_object()->okind == ObjKind::Array) return vm.call(self, arg_at(args, argc, 0), list.as_object()->items, list.as_object()->length, result);
  return vm.call(self, arg_at(args, argc, 0), nullptr, 0, result);
}

NATIVE(function_bind) {
  if (!need_function(vm, self)) return false;
  JsObject* b = vm.new_object(vm.function_proto());
  b->okind = ObjKind::Bound;
  b->target = self.as_object();
  b->bound_this = arg_at(args, argc, 0);
  for (uint32_t i = 1; i < argc; ++i) {
    if (!vm.array_push_bound(b, args[i])) break;
  }
  *result = Value::make_object(b);
  return true;
}

NATIVE(function_to_string) {
  String text(vm.allocator());
  vm.describe(self, &text, 0);
  *result = str_value(vm, text.view());
  return true;
}

// --- Array --------------------------------------------------------------------------------------------------------

NATIVE(array_ctor) {
  JsObject* a = vm.new_array();
  if (argc == 1 && args[0].is_number()) {
    const double d = args[0].u.number;
    if (d < 0 || d != static_cast<double>(static_cast<uint32_t>(d)) || d > 100000000) return vm.throw_error(ErrorKind::RangeError, "Invalid array length");
    vm.array_set_length(a, static_cast<uint32_t>(d));
  } else {
    for (uint32_t i = 0; i < argc; ++i) vm.array_push(a, args[i]);
  }
  *result = Value::make_object(a);
  return true;
}

NATIVE(array_is_array) {
  const Value v = arg_at(args, argc, 0);
  JsObject* o = v.is_object() ? v.as_object() : nullptr;
  for (int depth = 0; o && o->okind == ObjKind::Proxy && depth < 16; ++depth) o = o->target;   // (a proxy of an array is one)
  *result = Value::make_bool(o && o->okind == ObjKind::Array);
  return true;
}

// --- Proxy ---------------------------------------------------------------------------------------------------------------

NATIVE(proxy_constructor) {
  if (!self.is_object() || self.as_object()->okind != ObjKind::Plain) return vm.throw_error(ErrorKind::TypeError, "Constructor Proxy requires 'new'");
  const Value target = arg_at(args, argc, 0), handler = arg_at(args, argc, 1);
  if (!target.is_object() || !handler.is_object()) return vm.throw_error(ErrorKind::TypeError, "Cannot create proxy with a non-object as target or handler");
  *result = Value::make_object(vm.new_proxy(target.as_object(), handler));
  return true;
}

NATIVE(proxy_revoke_function) {
  const JsObject* me = vm.current_native();
  if (me->length > 0 && me->items[0].is_object()) Vm::revoke_proxy(me->items[0].as_object());
  return true;
}

NATIVE(proxy_revocable) {
  const Value target = arg_at(args, argc, 0), handler = arg_at(args, argc, 1);
  if (!target.is_object() || !handler.is_object()) return vm.throw_error(ErrorKind::TypeError, "Cannot create proxy with a non-object as target or handler");
  JsObject* proxy = vm.new_proxy(target.as_object(), handler);
  const Value proxy_value = Value::make_object(proxy);
  JsObject* revoke = vm.new_native_with(proxy_revoke_function, "", 0, &proxy_value, 1);
  JsObject* r = vm.new_plain();
  vm.define_named(r, "proxy", proxy_value);
  vm.define_named(r, "revoke", Value::make_object(revoke));
  *result = Value::make_object(r);
  return true;
}

NATIVE(array_push_m) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  for (uint32_t i = 0; i < argc; ++i) vm.array_push(a, args[i]);
  *result = num_value(a->length);
  return true;
}

NATIVE(array_pop) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  if (a->length == 0) {
    *result = Value();
    return true;
  }
  *result = a->items[--a->length];
  return true;
}

NATIVE(array_shift) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  if (a->length == 0) {
    *result = Value();
    return true;
  }
  *result = a->items[0];
  for (uint32_t i = 1; i < a->length; ++i) a->items[i - 1] = a->items[i];
  --a->length;
  return true;
}

NATIVE(array_unshift) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  const uint32_t old = a->length;
  if (!vm.array_set_length(a, old + argc)) return true;
  for (uint32_t i = old; i > 0; --i) a->items[i - 1 + argc] = a->items[i - 1];
  for (uint32_t i = 0; i < argc; ++i) a->items[i] = args[i];
  *result = num_value(a->length);
  return true;
}

NATIVE(array_slice) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  int64_t from, to;
  if (!arg_integer(vm, args, argc, 0, 0, &from) || !arg_integer(vm, args, argc, 1, a->length, &to)) return false;
  from = relative(from, a->length);
  to = relative(to, a->length);
  JsObject* out = vm.new_array(to > from ? static_cast<uint32_t>(to - from) : 0);
  for (int64_t i = from; i < to; ++i) vm.array_push(out, a->items[i]);
  *result = Value::make_object(out);
  return true;
}

NATIVE(array_splice) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  int64_t start, count;
  if (!arg_integer(vm, args, argc, 0, 0, &start)) return false;
  start = relative(start, a->length);
  if (argc < 2) count = a->length - start;
  else if (!arg_integer(vm, args, argc, 1, 0, &count)) return false;
  if (count < 0) count = 0;
  if (count > a->length - start) count = a->length - start;
  JsObject* removed = vm.new_array(static_cast<uint32_t>(count));
  for (int64_t i = 0; i < count; ++i) vm.array_push(removed, a->items[start + i]);
  const uint32_t inserted = argc > 2 ? argc - 2 : 0;
  const uint32_t old = a->length;
  const uint32_t new_length = old - static_cast<uint32_t>(count) + inserted;
  if (new_length > old) {
    if (!vm.array_set_length(a, new_length)) return true;
    for (uint32_t i = old; i > static_cast<uint32_t>(start + count); --i) a->items[i - 1 + inserted - static_cast<uint32_t>(count)] = a->items[i - 1];
  } else {
    for (uint32_t i = static_cast<uint32_t>(start + count); i < old; ++i) a->items[i + inserted - static_cast<uint32_t>(count)] = a->items[i];
    a->length = new_length;
  }
  for (uint32_t i = 0; i < inserted; ++i) a->items[static_cast<uint32_t>(start) + i] = args[2 + i];
  *result = Value::make_object(removed);
  return true;
}

NATIVE(array_concat) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  JsObject* out = vm.new_array(a->length);
  for (uint32_t i = 0; i < a->length; ++i) vm.array_push(out, a->items[i]);
  for (uint32_t k = 0; k < argc; ++k) {
    if (args[k].is_object() && args[k].as_object()->okind == ObjKind::Array) {
      JsObject* b = args[k].as_object();
      for (uint32_t i = 0; i < b->length; ++i) vm.array_push(out, b->items[i]);
    } else {
      vm.array_push(out, args[k]);
    }
  }
  *result = Value::make_object(out);
  return true;
}

NATIVE(array_join) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  JsString* sep = nullptr;
  if (argc > 0 && !args[0].is_undefined()) {
    if (!vm.to_string(args[0], &sep)) return false;
  }
  String text(vm.allocator());
  for (uint32_t i = 0; i < a->length; ++i) {
    if (i) text.append(sep ? sep->view() : StringView(","));
    if (a->items[i].is_nullish()) continue;
    JsString* s;
    if (!vm.to_string(a->items[i], &s)) return false;
    text.append(s->view());
    if (i >= a->length) break;
  }
  *result = str_value(vm, text.view());
  return true;
}

NATIVE(array_index_of) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  int64_t from;
  if (!arg_integer(vm, args, argc, 1, 0, &from)) return false;
  from = relative(from, a->length);
  for (int64_t i = from; i < a->length; ++i) {
    if (vm.strict_equals(a->items[i], arg_at(args, argc, 0))) {
      *result = num_value(static_cast<double>(i));
      return true;
    }
  }
  *result = num_value(-1);
  return true;
}

NATIVE(array_last_index_of) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  for (int64_t i = static_cast<int64_t>(a->length) - 1; i >= 0; --i) {
    if (vm.strict_equals(a->items[i], arg_at(args, argc, 0))) {
      *result = num_value(static_cast<double>(i));
      return true;
    }
  }
  *result = num_value(-1);
  return true;
}

NATIVE(array_includes) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  const Value needle = arg_at(args, argc, 0);
  for (uint32_t i = 0; i < a->length; ++i) {
    const Value& item = a->items[i];
    if (vm.strict_equals(item, needle) || (item.is_number() && needle.is_number() && item.u.number != item.u.number && needle.u.number != needle.u.number)) {
      *result = Value::make_bool(true);
      return true;
    }
  }
  *result = Value::make_bool(false);
  return true;
}

// [Symbol.iterator]() and values() for the array-likes that inherit them (a NodeList): the elements, as an array a for...of walks.
NATIVE(array_values) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  *result = Value::make_object(a);
  return true;
}

NATIVE(array_reverse) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  for (uint32_t i = 0, j = a->length; i + 1 < j; ++i, --j) {
    const Value t = a->items[i];
    a->items[i] = a->items[j - 1];
    a->items[j - 1] = t;
  }
  *result = self;
  return true;
}

NATIVE(array_fill) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  int64_t from, to;
  if (!arg_integer(vm, args, argc, 1, 0, &from) || !arg_integer(vm, args, argc, 2, a->length, &to)) return false;
  from = relative(from, a->length);
  to = relative(to, a->length);
  for (int64_t i = from; i < to; ++i) a->items[i] = arg_at(args, argc, 0);
  *result = self;
  return true;
}

// Calls callback(item, index, array) for the elements; the mode decides what is done with the answers.
enum class Each { ForEach, Map, Filter, Find, FindIndex, Some, Every };

bool array_each(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, Each mode) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  const Value fn = arg_at(args, argc, 0), this_arg = arg_at(args, argc, 1);
  if (!need_function(vm, fn)) return false;
  JsObject* out = (mode == Each::Map || mode == Each::Filter) ? vm.new_array(a->length) : nullptr;
  *result = out ? Value::make_object(out) : Value();
  if (mode == Each::FindIndex) *result = num_value(-1);
  if (mode == Each::Some) *result = Value::make_bool(false);
  if (mode == Each::Every) *result = Value::make_bool(true);
  for (uint32_t i = 0; i < a->length; ++i) {
    const Value item = a->items[i];
    Value call_args[3] = {item, num_value(i), self};
    Value r;
    if (!vm.call(fn, this_arg, call_args, 3, &r)) return false;
    switch (mode) {
      case Each::ForEach: break;
      case Each::Map: vm.array_push(out, r); break;
      case Each::Filter:
        if (Vm::to_boolean(r)) vm.array_push(out, item);
        break;
      case Each::Find:
        if (Vm::to_boolean(r)) {
          *result = item;
          return true;
        }
        break;
      case Each::FindIndex:
        if (Vm::to_boolean(r)) {
          *result = num_value(i);
          return true;
        }
        break;
      case Each::Some:
        if (Vm::to_boolean(r)) {
          *result = Value::make_bool(true);
          return true;
        }
        break;
      case Each::Every:
        if (!Vm::to_boolean(r)) {
          *result = Value::make_bool(false);
          return true;
        }
        break;
    }
  }
  return true;
}

NATIVE(array_for_each) { return array_each(vm, self, args, argc, result, Each::ForEach); }
NATIVE(array_map) { return array_each(vm, self, args, argc, result, Each::Map); }
NATIVE(array_filter) { return array_each(vm, self, args, argc, result, Each::Filter); }
NATIVE(array_find) { return array_each(vm, self, args, argc, result, Each::Find); }
NATIVE(array_find_index) { return array_each(vm, self, args, argc, result, Each::FindIndex); }
NATIVE(array_some) { return array_each(vm, self, args, argc, result, Each::Some); }
NATIVE(array_every) { return array_each(vm, self, args, argc, result, Each::Every); }

bool array_reduce_impl(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, bool right) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  const Value fn = arg_at(args, argc, 0);
  if (!need_function(vm, fn)) return false;
  int64_t i = right ? static_cast<int64_t>(a->length) - 1 : 0;
  const int64_t step = right ? -1 : 1;
  Value acc;
  if (argc >= 2) {
    acc = args[1];
  } else {
    if (a->length == 0) return vm.throw_error(ErrorKind::TypeError, "Reduce of empty array with no initial value");
    acc = a->items[i];
    i += step;
  }
  for (; i >= 0 && i < static_cast<int64_t>(a->length); i += step) {
    Value call_args[4] = {acc, a->items[i], num_value(static_cast<double>(i)), self};
    if (!vm.call(fn, Value(), call_args, 4, &acc)) return false;
  }
  *result = acc;
  return true;
}
NATIVE(array_reduce) { return array_reduce_impl(vm, self, args, argc, result, false); }
NATIVE(array_reduce_right) { return array_reduce_impl(vm, self, args, argc, result, true); }

// Sorting: a merge sort (stable, and a comparison that throws stops it).
bool sort_less(Vm& vm, const Value& fn, const Value& a, const Value& b, bool* less_or_equal) {
  if (a.is_undefined() || b.is_undefined()) {
    *less_or_equal = !b.is_undefined() || a.is_undefined();
    return true;
  }
  if (fn.is_undefined()) {
    JsString *x, *y;
    if (!vm.to_string(a, &x) || !vm.to_string(b, &y)) return false;
    *less_or_equal = x->view().compare(y->view()) <= 0;
    return true;
  }
  Value call_args[2] = {a, b};
  Value r;
  double d;
  if (!vm.call(fn, Value(), call_args, 2, &r) || !vm.to_number(r, &d)) return false;
  *less_or_equal = !(d > 0);
  return true;
}

bool merge_sort(Vm& vm, const Value& fn, Value* items, Value* scratch, uint32_t n) {
  if (n < 2) return true;
  const uint32_t mid = n / 2;
  if (!merge_sort(vm, fn, items, scratch, mid) || !merge_sort(vm, fn, items + mid, scratch, n - mid)) return false;
  uint32_t i = 0, j = mid, k = 0;
  while (i < mid && j < n) {
    bool le;
    if (!sort_less(vm, fn, items[i], items[j], &le)) return false;
    scratch[k++] = le ? items[i++] : items[j++];
  }
  while (i < mid) scratch[k++] = items[i++];
  while (j < n) scratch[k++] = items[j++];
  for (uint32_t t = 0; t < n; ++t) items[t] = scratch[t];
  return true;
}

NATIVE(array_sort) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  const Value fn = arg_at(args, argc, 0);
  if (!fn.is_undefined() && !need_function(vm, fn)) return false;
  if (a->length > 1) {
    Value* scratch = static_cast<Value*>(vm.allocator().allocate(sizeof(Value) * a->length));
    if (!scratch) return vm.throw_error(ErrorKind::RangeError, "out of memory");
    Value* copy = static_cast<Value*>(vm.allocator().allocate(sizeof(Value) * a->length));
    if (!copy) {
      vm.allocator().deallocate(scratch);
      return vm.throw_error(ErrorKind::RangeError, "out of memory");
    }
    for (uint32_t i = 0; i < a->length; ++i) copy[i] = a->items[i];
    const bool ok = merge_sort(vm, fn, copy, scratch, a->length);
    if (ok)
      for (uint32_t i = 0; i < a->length; ++i) a->items[i] = copy[i];
    vm.allocator().deallocate(copy);
    vm.allocator().deallocate(scratch);
    if (!ok) return false;
  }
  *result = self;
  return true;
}

NATIVE(array_to_string) { return array_join(vm, self, nullptr, 0, result); }

NATIVE(array_flat) {
  JsObject* a;
  if (!this_array(vm, self, &a)) return false;
  JsObject* out = vm.new_array();
  for (uint32_t i = 0; i < a->length; ++i) {
    if (a->items[i].is_object() && a->items[i].as_object()->okind == ObjKind::Array) {
      JsObject* inner = a->items[i].as_object();
      for (uint32_t k = 0; k < inner->length; ++k) vm.array_push(out, inner->items[k]);
    } else {
      vm.array_push(out, a->items[i]);
    }
  }
  *result = Value::make_object(out);
  return true;
}

// --- String -------------------------------------------------------------------------------------------------------

NATIVE(string_ctor) {
  if (argc == 0) {
    *result = Value::make_string(vm.empty_string());
    return true;
  }
  JsString* s;
  if (!vm.to_string(args[0], &s)) return false;
  *result = Value::make_string(s);
  return true;
}

NATIVE(string_from_char_code) {
  String text(vm.allocator());
  for (uint32_t i = 0; i < argc; ++i) {
    double d;
    if (!vm.to_number(args[i], &d)) return false;
    const uint32_t cp = d == d && d >= 0 ? static_cast<uint32_t>(d) & 0xFFFF : 0;
    if (cp < 0x80) text.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
      text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  *result = str_value(vm, text.view());
  return true;
}

NATIVE(string_from_code_point) {
  String text(vm.allocator());
  for (uint32_t i = 0; i < argc; ++i) {
    double d;
    if (!vm.to_number(args[i], &d)) return false;
    if (!(d >= 0 && d <= 0x10FFFF) || d != static_cast<double>(static_cast<uint32_t>(d))) {
      String message(vm.allocator());
      message.append("Invalid code point ");
      JsString* shown;
      if (vm.to_string(args[i], &shown)) message.append(shown->view());
      return vm.throw_error(ErrorKind::RangeError, message.view());
    }
    const uint32_t cp = static_cast<uint32_t>(d);
    if (cp < 0x80) {
      text.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  *result = str_value(vm, text.view());
  return true;
}

NATIVE(string_char_at) {
  JsString* s;
  int64_t i;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &i)) return false;
  *result = i >= 0 && i < s->length ? str_value(vm, StringView(s->data() + i, 1)) : Value::make_string(vm.empty_string());
  return true;
}

NATIVE(string_at) {
  JsString* s;
  int64_t i;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &i)) return false;
  if (i < 0) i += s->length;
  *result = i >= 0 && i < s->length ? str_value(vm, StringView(s->data() + i, 1)) : Value();
  return true;
}

NATIVE(string_char_code_at) {
  JsString* s;
  int64_t i;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &i)) return false;
  *result = i >= 0 && i < s->length ? num_value(static_cast<uint8_t>(s->data()[i])) : num_value(kNaN);
  return true;
}

NATIVE(string_index_of) {
  JsString *s, *needle;
  int64_t from;
  if (!this_string(vm, self, &s) || !arg_string(vm, args, argc, 0, &needle) || !arg_integer(vm, args, argc, 1, 0, &from)) return false;
  if (from < 0) from = 0;
  if (from > s->length) from = s->length;
  const uint64_t at = s->view().find(needle->view(), static_cast<uint64_t>(from));
  *result = num_value(at == StringView::npos ? -1 : static_cast<double>(at));
  return true;
}

NATIVE(string_last_index_of) {
  JsString *s, *needle;
  if (!this_string(vm, self, &s) || !arg_string(vm, args, argc, 0, &needle)) return false;
  int64_t found = -1;
  for (int64_t i = static_cast<int64_t>(s->length) - needle->length; i >= 0; --i) {
    if (s->view().substr(static_cast<uint64_t>(i), needle->length).equals(needle->view())) {
      found = i;
      break;
    }
  }
  *result = num_value(static_cast<double>(found));
  return true;
}

NATIVE(string_includes) {
  JsString *s, *needle;
  if (!this_string(vm, self, &s) || !arg_string(vm, args, argc, 0, &needle)) return false;
  *result = Value::make_bool(s->view().find(needle->view()) != StringView::npos);
  return true;
}

NATIVE(string_starts_with) {
  JsString *s, *needle;
  if (!this_string(vm, self, &s) || !arg_string(vm, args, argc, 0, &needle)) return false;
  *result = Value::make_bool(s->view().starts_with(needle->view()));
  return true;
}

NATIVE(string_ends_with) {
  JsString *s, *needle;
  if (!this_string(vm, self, &s) || !arg_string(vm, args, argc, 0, &needle)) return false;
  *result = Value::make_bool(s->view().ends_with(needle->view()));
  return true;
}

NATIVE(string_slice) {
  JsString* s;
  int64_t from, to;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &from) || !arg_integer(vm, args, argc, 1, s->length, &to)) return false;
  from = relative(from, s->length);
  to = relative(to, s->length);
  *result = to > from ? str_value(vm, s->view().substr(static_cast<uint64_t>(from), static_cast<uint64_t>(to - from))) : Value::make_string(vm.empty_string());
  return true;
}

NATIVE(string_substring) {
  JsString* s;
  int64_t from, to;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &from) || !arg_integer(vm, args, argc, 1, s->length, &to)) return false;
  if (from < 0) from = 0;
  if (to < 0) to = 0;
  if (from > s->length) from = s->length;
  if (to > s->length) to = s->length;
  if (from > to) {
    const int64_t t = from;
    from = to;
    to = t;
  }
  *result = str_value(vm, s->view().substr(static_cast<uint64_t>(from), static_cast<uint64_t>(to - from)));
  return true;
}

NATIVE(string_substr) {
  JsString* s;
  int64_t from, length;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &from) || !arg_integer(vm, args, argc, 1, s->length, &length)) return false;
  from = relative(from, s->length);
  if (length < 0) length = 0;
  if (length > s->length - from) length = s->length - from;
  *result = str_value(vm, s->view().substr(static_cast<uint64_t>(from), static_cast<uint64_t>(length)));
  return true;
}

NATIVE(string_upper) {
  JsString* s;
  if (!this_string(vm, self, &s)) return false;
  String out(vm.allocator());
  for (uint32_t i = 0; i < s->length; ++i) out.push_back(s->data()[i] >= 'a' && s->data()[i] <= 'z' ? static_cast<char>(s->data()[i] - 32) : s->data()[i]);
  *result = str_value(vm, out.view());
  return true;
}

NATIVE(string_lower) {
  JsString* s;
  if (!this_string(vm, self, &s)) return false;
  String out(vm.allocator());
  for (uint32_t i = 0; i < s->length; ++i) out.push_back(s->data()[i] >= 'A' && s->data()[i] <= 'Z' ? static_cast<char>(s->data()[i] + 32) : s->data()[i]);
  *result = str_value(vm, out.view());
  return true;
}

bool trim_impl(Vm& vm, const Value& self, Value* result, bool left, bool right) {
  JsString* s;
  if (!this_string(vm, self, &s)) return false;
  StringView v = s->view();
  uint64_t a = 0, b = v.size();
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  while (left && a < b && blank(v[a])) ++a;
  while (right && b > a && blank(v[b - 1])) --b;
  *result = str_value(vm, v.substr(a, b - a));
  return true;
}
NATIVE(string_trim) { return trim_impl(vm, self, result, true, true); }
NATIVE(string_trim_start) { return trim_impl(vm, self, result, true, false); }
NATIVE(string_trim_end) { return trim_impl(vm, self, result, false, true); }

NATIVE(string_split) {
  JsString* s;
  if (!this_string(vm, self, &s)) return false;
  const Value sep = arg_at(args, argc, 0);
  int64_t limit;
  if (!arg_integer(vm, args, argc, 1, 4294967295ll, &limit)) return false;
  JsObject* out = vm.new_array();
  *result = Value::make_object(out);
  if (sep.is_undefined()) {
    vm.array_push(out, Value::make_string(s));
    return true;
  }
  if (regexp_is(sep)) return regexp_split(vm, s, sep, limit, result);
  JsString* d;
  if (!vm.to_string(sep, &d)) return false;
  if (d->length == 0) {
    for (uint32_t i = 0; i < s->length && static_cast<int64_t>(out->length) < limit; ++i) vm.array_push(out, str_value(vm, StringView(s->data() + i, 1)));
    return true;
  }
  uint64_t pos = 0;
  for (;;) {
    const uint64_t at = s->view().find(d->view(), pos);
    if (static_cast<int64_t>(out->length) >= limit) break;
    if (at == StringView::npos) {
      vm.array_push(out, str_value(vm, s->view().substr(pos)));
      break;
    }
    vm.array_push(out, str_value(vm, s->view().substr(pos, at - pos)));
    pos = at + d->length;
  }
  return true;
}

bool replace_impl(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, bool all) {
  JsString *s, *pattern;
  if (!this_string(vm, self, &s)) return false;
  if (regexp_is(arg_at(args, argc, 0))) return regexp_replace(vm, s, args[0], arg_at(args, argc, 1), all, result);
  if (!arg_string(vm, args, argc, 0, &pattern)) return false;
  const Value replacement = arg_at(args, argc, 1);
  JsString* fixed = nullptr;
  if (!Vm::is_callable(replacement) && !vm.to_string(replacement, &fixed)) return false;
  String out(vm.allocator());
  uint64_t pos = 0;
  for (;;) {
    const uint64_t at = s->view().find(pattern->view(), pos);
    if (at == StringView::npos || (pattern->length == 0 && at >= s->length && pos > 0)) break;
    out.append(s->view().substr(pos, at - pos));
    if (fixed) {
      out.append(fixed->view());
    } else {
      Value call_args[3] = {Value::make_string(pattern), num_value(static_cast<double>(at)), Value::make_string(s)};
      Value r;
      JsString* rs;
      if (!vm.call(replacement, Value(), call_args, 3, &r) || !vm.to_string(r, &rs)) return false;
      out.append(rs->view());
    }
    pos = at + pattern->length;
    if (pattern->length == 0) {
      if (at < s->length) out.push_back(s->data()[at]);
      ++pos;
    }
    if (!all) break;
    if (pos > s->length) break;
  }
  if (pos < s->length) out.append(s->view().substr(pos));
  *result = str_value(vm, out.view());
  return true;
}
NATIVE(string_match) {
  JsString* s;
  Value re;
  if (!this_string(vm, self, &s) || !regexp_from_argument(vm, arg_at(args, argc, 0), "", &re)) return false;
  return regexp_match(vm, s, re, result);
}
NATIVE(string_match_all) {
  JsString* s;
  Value re;
  if (!this_string(vm, self, &s) || !regexp_from_argument(vm, arg_at(args, argc, 0), "g", &re)) return false;
  return regexp_match_all(vm, s, re, result);
}
NATIVE(string_search) {
  JsString* s;
  Value re;
  if (!this_string(vm, self, &s) || !regexp_from_argument(vm, arg_at(args, argc, 0), "", &re)) return false;
  return regexp_search(vm, s, re, result);
}
NATIVE(string_replace) { return replace_impl(vm, self, args, argc, result, false); }
NATIVE(string_replace_all) { return replace_impl(vm, self, args, argc, result, true); }

NATIVE(string_repeat) {
  JsString* s;
  int64_t n;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &n)) return false;
  if (n < 0 || static_cast<uint64_t>(n) * s->length > (16u << 20)) return vm.throw_error(ErrorKind::RangeError, "Invalid count value");
  String out(vm.allocator());
  for (int64_t i = 0; i < n; ++i) out.append(s->view());
  *result = str_value(vm, out.view());
  return true;
}

bool pad_impl(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, bool start) {
  JsString *s, *fill = nullptr;
  int64_t target;
  if (!this_string(vm, self, &s) || !arg_integer(vm, args, argc, 0, 0, &target)) return false;
  if (argc > 1 && !args[1].is_undefined()) {
    if (!vm.to_string(args[1], &fill)) return false;
  }
  StringView pad = fill ? fill->view() : StringView(" ");
  if (target <= s->length || pad.empty() || target > (16 << 20)) {
    *result = Value::make_string(s);
    return true;
  }
  String padding(vm.allocator());
  for (int64_t i = 0; i < target - s->length; ++i) padding.push_back(pad[static_cast<uint64_t>(i) % pad.size()]);
  String out(vm.allocator());
  if (start) out.append(padding.view());
  out.append(s->view());
  if (!start) out.append(padding.view());
  *result = str_value(vm, out.view());
  return true;
}
NATIVE(string_pad_start) { return pad_impl(vm, self, args, argc, result, true); }
NATIVE(string_pad_end) { return pad_impl(vm, self, args, argc, result, false); }

NATIVE(string_concat) {
  JsString* s;
  if (!this_string(vm, self, &s)) return false;
  String out(vm.allocator());
  out.append(s->view());
  for (uint32_t i = 0; i < argc; ++i) {
    JsString* t;
    if (!vm.to_string(args[i], &t)) return false;
    out.append(t->view());
  }
  *result = str_value(vm, out.view());
  return true;
}

NATIVE(string_locale_compare) {
  JsString *s, *t;
  if (!this_string(vm, self, &s) || !arg_string(vm, args, argc, 0, &t)) return false;
  const int c = s->view().compare(t->view());
  *result = num_value(c < 0 ? -1 : c > 0 ? 1 : 0);
  return true;
}

NATIVE(string_to_string) {
  if (!self.is_string()) return vm.throw_error(ErrorKind::TypeError, "String.prototype.toString requires a string");
  *result = self;
  return true;
}

// --- Number and Boolean ---------------------------------------------------------------------------------------------

NATIVE(number_ctor) {
  double d = 0;
  if (argc > 0 && !vm.to_number(args[0], &d)) return false;
  *result = num_value(d);
  return true;
}

bool this_number(Vm& vm, const Value& self, double* out) {
  if (!self.is_number()) return vm.throw_error(ErrorKind::TypeError, "Number.prototype method called on a value that is not a number");
  *out = self.u.number;
  return true;
}

NATIVE(number_to_string_m) {
  double d;
  int64_t radix;
  if (!this_number(vm, self, &d) || !arg_integer(vm, args, argc, 0, 10, &radix)) return false;
  if (radix < 2 || radix > 36) return vm.throw_error(ErrorKind::RangeError, "toString() radix must be between 2 and 36");
  String text(vm.allocator());
  number_to_radix(d, static_cast<int>(radix), &text);
  *result = str_value(vm, text.view());
  return true;
}

NATIVE(number_to_fixed_m) {
  double d;
  int64_t digits;
  if (!this_number(vm, self, &d) || !arg_integer(vm, args, argc, 0, 0, &digits)) return false;
  if (digits < 0 || digits > 100) return vm.throw_error(ErrorKind::RangeError, "toFixed() digits argument must be between 0 and 100");
  String text(vm.allocator());
  number_to_fixed(d, static_cast<int>(digits), &text);
  *result = str_value(vm, text.view());
  return true;
}

NATIVE(number_value_of) {
  double d;
  if (!this_number(vm, self, &d)) return false;
  *result = num_value(d);
  return true;
}

NATIVE(number_is_integer) {
  const Value v = arg_at(args, argc, 0);
  *result = Value::make_bool(v.is_number() && v.u.number == v.u.number && v.u.number > -kInf && v.u.number < kInf && v.u.number == static_cast<double>(static_cast<int64_t>(v.u.number)));
  return true;
}

NATIVE(number_is_finite) {
  const Value v = arg_at(args, argc, 0);
  *result = Value::make_bool(v.is_number() && v.u.number == v.u.number && v.u.number > -kInf && v.u.number < kInf);
  return true;
}

NATIVE(number_is_nan) {
  const Value v = arg_at(args, argc, 0);
  *result = Value::make_bool(v.is_number() && v.u.number != v.u.number);
  return true;
}

NATIVE(global_is_nan) {
  double d;
  if (!arg_number(vm, args, argc, 0, &d)) return false;
  *result = Value::make_bool(d != d);
  return true;
}

NATIVE(global_is_finite) {
  double d;
  if (!arg_number(vm, args, argc, 0, &d)) return false;
  *result = Value::make_bool(d == d && d > -kInf && d < kInf);
  return true;
}

NATIVE(global_parse_float) {
  JsString* s;
  if (!arg_string(vm, args, argc, 0, &s)) return false;
  StringView v = s->view().trimmed();
  uint64_t i = 0;
  if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
  if (v.substr(i).starts_with("Infinity")) {
    *result = num_value(v[0] == '-' ? -kInf : kInf);
    return true;
  }
  uint64_t end = i;
  bool digits = false, dot = false;
  while (end < v.size()) {
    const char c = v[end];
    if (c >= '0' && c <= '9') digits = true;
    else if (c == '.' && !dot) dot = true;
    else break;
    ++end;
  }
  if (!digits) {
    *result = num_value(kNaN);
    return true;
  }
  if (end < v.size() && (v[end] == 'e' || v[end] == 'E')) {
    uint64_t e = end + 1;
    if (e < v.size() && (v[e] == '+' || v[e] == '-')) ++e;
    if (e < v.size() && v[e] >= '0' && v[e] <= '9') {
      while (e < v.size() && v[e] >= '0' && v[e] <= '9') ++e;
      end = e;
    }
  }
  double d = 0;
  StringView body = v.substr(i, end - i);
  if (body.ends_with('.')) body = body.without_suffix(1);
  if (!parse_decimal(body, &d)) d = kNaN;
  *result = num_value(v[0] == '-' ? -d : d);
  return true;
}

NATIVE(global_parse_int) {
  JsString* s;
  int64_t radix;
  if (!arg_string(vm, args, argc, 0, &s) || !arg_integer(vm, args, argc, 1, 0, &radix)) return false;
  StringView v = s->view().trimmed();
  uint64_t i = 0;
  bool negative = false;
  if (i < v.size() && (v[i] == '+' || v[i] == '-')) negative = v[i++] == '-';
  if ((radix == 0 || radix == 16) && v.substr(i).size() > 1 && v[i] == '0' && (v[i + 1] == 'x' || v[i + 1] == 'X')) {
    radix = 16;
    i += 2;
  }
  if (radix == 0) radix = 10;
  if (radix < 2 || radix > 36) {
    *result = num_value(kNaN);
    return true;
  }
  double value = 0;
  bool any = false;
  for (; i < v.size(); ++i) {
    const char c = v[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    if (d >= radix) break;
    value = value * static_cast<double>(radix) + d;
    any = true;
  }
  *result = any ? num_value(negative ? -value : value) : num_value(kNaN);
  return true;
}

NATIVE(boolean_ctor) {
  *result = Value::make_bool(Vm::to_boolean(arg_at(args, argc, 0)));
  return true;
}

NATIVE(boolean_to_string) {
  *result = str_value(vm, self.is_bool() && self.u.boolean ? "true" : "false");
  return true;
}

NATIVE(boolean_value_of) {
  *result = self;
  return true;
}

// --- Math ---------------------------------------------------------------------------------------------------------

double floor_of(double x) {
  if (x != x || x > 4.5e15 || x < -4.5e15) return x;
  const double t = static_cast<double>(static_cast<int64_t>(x));
  return t > x ? t - 1 : t;
}

#define MATH1(name, expr) \
  NATIVE(name) { double x; if (!arg_number(vm, args, argc, 0, &x)) return false; *result = num_value(expr); return true; }

MATH1(math_abs, x < 0 ? -x : x)
MATH1(math_floor, floor_of(x))
MATH1(math_ceil, -floor_of(-x))
MATH1(math_round, floor_of(x + 0.5))
MATH1(math_trunc, x < 0 ? -floor_of(-x) : floor_of(x))
MATH1(math_sign, x != x ? x : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(math_sqrt, x < 0 || x != x ? kNaN : x == kInf ? kInf : fmath::sqrt(x))
MATH1(math_sin, x != x || x == kInf || x == -kInf ? kNaN : fmath::sin(x))
MATH1(math_cos, x != x || x == kInf || x == -kInf ? kNaN : fmath::cos(x))
MATH1(math_tan, x != x || x == kInf || x == -kInf ? kNaN : fmath::tan(x))
MATH1(math_exp, x != x ? x : x > 709 ? kInf : x < -745 ? 0 : fmath::exp(x))
MATH1(math_log, x != x || x < 0 ? kNaN : x == 0 ? -kInf : x == kInf ? kInf : fmath::ln(x))
MATH1(math_log10, x != x || x < 0 ? kNaN : x == 0 ? -kInf : x == kInf ? kInf : fmath::ln(x) / 2.302585092994046)
MATH1(math_log2, x != x || x < 0 ? kNaN : x == 0 ? -kInf : x == kInf ? kInf : fmath::ln(x) / 0.6931471805599453)
MATH1(math_cbrt, x == 0 || x != x ? x : (x < 0 ? -1 : 1) * fmath::pow(x < 0 ? -x : x, 1.0 / 3.0))

NATIVE(math_pow) {
  double a, b;
  if (!arg_number(vm, args, argc, 0, &a) || !arg_number(vm, args, argc, 1, &b)) return false;
  Value x = num_value(a), y = num_value(b);
  return vm.binary_op_public(Opc::Pow, x, y, result);
}

NATIVE(math_atan2) {
  double y, x;
  if (!arg_number(vm, args, argc, 0, &y) || !arg_number(vm, args, argc, 1, &x)) return false;
  // atan from the series through the library's sin / cos is not available: use a rational approximation
  if (x != x || y != y) {
    *result = num_value(kNaN);
    return true;
  }
  const double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  double a = ax > ay ? ay / ax : (ay == 0 ? 0 : ax / ay);
  // atan(a) for 0..1 by a minimax polynomial
  const double s = a * a;
  double r = a * (0.99997726 + s * (-0.33262347 + s * (0.19354346 + s * (-0.11643287 + s * (0.05265332 + s * -0.01172120)))));
  if (ay > ax) r = 1.5707963267948966 - r;
  if (x < 0) r = 3.141592653589793 - r;
  if (y < 0) r = -r;
  *result = num_value(r);
  return true;
}

NATIVE(math_atan) {
  double x;
  if (!arg_number(vm, args, argc, 0, &x)) return false;
  Value a[2] = {num_value(x), num_value(1)};
  return math_atan2(vm, self, a, 2, result);
}

NATIVE(math_min) {
  double best = kInf;
  for (uint32_t i = 0; i < argc; ++i) {
    double d;
    if (!vm.to_number(args[i], &d)) return false;
    if (d != d) {
      *result = num_value(d);
      return true;
    }
    if (d < best) best = d;
  }
  *result = num_value(best);
  return true;
}

NATIVE(math_max) {
  double best = -kInf;
  for (uint32_t i = 0; i < argc; ++i) {
    double d;
    if (!vm.to_number(args[i], &d)) return false;
    if (d != d) {
      *result = num_value(d);
      return true;
    }
    if (d > best) best = d;
  }
  *result = num_value(best);
  return true;
}

NATIVE(math_random) {
  *result = num_value(static_cast<double>(vm.random_next() & 0xFFFFFFFu) / 268435456.0);
  return true;
}

NATIVE(math_hypot) {
  double sum = 0;
  for (uint32_t i = 0; i < argc; ++i) {
    double d;
    if (!vm.to_number(args[i], &d)) return false;
    sum += d * d;
  }
  *result = num_value(fmath::sqrt(sum));
  return true;
}

// --- Errors ---------------------------------------------------------------------------------------------------------

template <int Kind>
bool error_ctor(Vm& vm, const Value&, const Value* args, uint32_t argc, Value* result) {
  JsString* message = vm.empty_string();
  if (argc > 0 && !args[0].is_undefined() && !vm.to_string(args[0], &message)) return false;
  JsObject* error = vm.new_error(static_cast<ErrorKind>(Kind), message->view());
  *result = Value::make_object(error);
  if (argc > 1 && args[1].is_object()) {   // new Error(message, { cause })
    Value cause;
    if (!vm.get_named(args[1], "cause", &cause)) return false;
    if (vm.has_property(args[1].as_object(), vm.intern("cause"))) vm.define_named(error, "cause", cause, kWritable);
  }
  return true;
}

NATIVE(error_to_string) {
  String text(vm.allocator());
  vm.describe(self, &text, 0);
  *result = str_value(vm, text.view());
  return true;
}

// --- Promise -------------------------------------------------------------------------------------------------------

NATIVE(promise_ctor) {
  if (!self.is_object() || self.as_object()->okind != ObjKind::Plain || self.as_object()->proto != vm.promise_proto()) return vm.throw_error(ErrorKind::TypeError, "Promise constructor cannot be invoked without 'new'");
  const Value executor = arg_at(args, argc, 0);
  if (!Vm::is_callable(executor)) return vm.throw_error(ErrorKind::TypeError, "Promise resolver is not a function");
  JsObject* promise = self.as_object();
  promise->okind = ObjKind::Promise;
  promise->position = 0;
  promise->bound_this = Value();
  JsObject* shared = vm.new_plain();   // (its `position` says that the promise has been resolved or rejected: only the first call counts)
  const Value fns[2] = {Value::make_object(vm.resolving_function(promise, shared, false)), Value::make_object(vm.resolving_function(promise, shared, true))};
  Value ignored;
  if (!vm.call(executor, Value(), fns, 2, &ignored)) {
    if (vm.halted()) return false;
    Value reason;
    vm.take_exception_value(&reason);
    if (!shared->position) {
      shared->position = 1;
      vm.promise_reject(promise, reason);
    }
  }
  *result = self;
  return true;
}

NATIVE(promise_static_resolve) {
  *result = Value::make_object(vm.promise_resolved(arg_at(args, argc, 0)));
  return true;
}

NATIVE(promise_static_reject) {
  JsObject* p = vm.new_promise();
  vm.promise_reject(p, arg_at(args, argc, 0));
  *result = Value::make_object(p);
  return true;
}

NATIVE(promise_then) {
  if (!Vm::is_promise(self)) return vm.throw_error(ErrorKind::TypeError, "Promise.prototype.then called on a value that is not a promise");
  JsObject* derived = vm.new_promise();
  vm.promise_then(self.as_object(), arg_at(args, argc, 0), arg_at(args, argc, 1), derived);
  *result = Value::make_object(derived);
  return true;
}

NATIVE(promise_catch) {
  Value then;
  if (!vm.get_named(self, "then", &then)) return false;
  const Value a[2] = {Value(), arg_at(args, argc, 0)};
  return vm.call(then, self, a, 2, result);
}

// --- Symbol (just enough for Symbol.iterator: a symbol is a string that begins "@@") ------------------------------------------

NATIVE(symbol_function) {
  static uint32_t counter = 0;
  String text(vm.allocator());
  text.append("@@");
  JsString* description;
  if (argc > 0 && !args[0].is_undefined()) {
    if (!vm.to_string(args[0], &description)) return false;
    text.append(description->view());
  }
  text.push_back('#');
  text.append_uint(++counter);
  *result = str_value(vm, text.view());
  return true;
}

// --- JSON -----------------------------------------------------------------------------------------------------------

void json_quote(StringView s, String* out) {
  static const char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (uint64_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      default:
        if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F) {
          out->append("\\u00");
          out->push_back(kHex[(c >> 4) & 15]);
          out->push_back(kHex[c & 15]);
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

// JSON.stringify's state: the gap, the replacer (a function, or a list of the names allowed) and the objects being written (to find a cycle).
struct JsonWriter {
  Vm& vm;
  String gap, indent;
  Value replacer_function;
  JsObject* allowed = nullptr;   // an array of names, or null
  List<JsObject*> stack;
  String out;
  explicit JsonWriter(Vm& v) : vm(v), gap(v.allocator()), indent(v.allocator()), stack(v.allocator()), out(v.allocator()) {}

  bool circular(JsObject* o) {
    for (uint64_t i = 0; i < stack.count(); ++i)
      if (stack[i] == o) return true;
    return false;
  }

  // Writes holder[key] (`value` is what it holds). False with *wrote == false when it has no JSON form; false with a pending exception on a throw.
  bool write(const Value& holder, JsString* key, Value value, bool* wrote) {
    *wrote = false;
    if (value.is_object()) {   // toJSON
      Value to_json;
      if (!vm.get_named(value, "toJSON", &to_json)) return false;
      if (Vm::is_callable(to_json)) {
        const Value k = Value::make_string(key);
        if (!vm.call(to_json, value, &k, 1, &value)) return false;
      }
    }
    if (Vm::is_callable(replacer_function)) {
      const Value call_args[2] = {Value::make_string(key), value};
      if (!vm.call(replacer_function, holder, call_args, 2, &value)) return false;
    }
    switch (value.tag) {
      case Tag::Undefined: return true;
      case Tag::Null: out.append("null"); *wrote = true; return true;
      case Tag::Boolean: out.append(value.u.boolean ? "true" : "false"); *wrote = true; return true;
      case Tag::Number:
        if (value.u.number != value.u.number || value.u.number == kInf || value.u.number == -kInf) {
          out.append("null");
        } else {
          String t(vm.allocator());
          number_to_string(value.u.number == 0 ? 0.0 : value.u.number, &t);
          out.append(t.view());
        }
        *wrote = true;
        return true;
      case Tag::String: json_quote(value.as_string()->view(), &out); *wrote = true; return true;
      case Tag::Object: break;
    }
    if (Vm::is_callable(value)) return true;
    JsObject* o = value.as_object();
    if (circular(o)) return vm.throw_error(ErrorKind::TypeError, "Converting circular structure to JSON");
    if (stack.count() > 512) return vm.throw_error(ErrorKind::RangeError, "JSON nesting is too deep");
    stack.push_back(o);
    const uint64_t saved_indent = indent.size();
    indent.append(gap.view());
    bool ok = o->okind == ObjKind::Array ? write_array(value) : write_object(value);
    indent.truncate(saved_indent);
    stack.remove_at(stack.count() - 1);
    *wrote = ok;
    return ok;
  }

  bool write_array(const Value& array) {
    JsObject* o = array.as_object();
    if (o->length == 0) {
      out.append("[]");
      return true;
    }
    out.push_back('[');
    for (uint32_t i = 0; i < o->length; ++i) {
      if (i) out.push_back(',');
      if (!gap.empty()) {
        out.push_back('\n');
        out.append(indent.view());
      }
      String name(vm.allocator());
      name.append_uint(i);
      bool wrote;
      if (!write(array, vm.intern(name.view()), i < o->length ? o->items[i] : Value(), &wrote)) return false;
      if (!wrote) out.append("null");
    }
    if (!gap.empty()) {
      out.push_back('\n');
      out.append(indent.view().first(indent.size() - gap.size()));
    }
    out.push_back(']');
    return true;
  }

  bool write_object(const Value& object) {
    JsObject* keys = allowed ? allowed : vm.own_keys(object.as_object());
    out.push_back('{');
    bool first = true;
    for (uint32_t i = 0; i < keys->length; ++i) {
      JsString* key = keys->items[i].as_string();
      Value item;
      if (!vm.get(object, key, &item)) return false;
      const uint64_t mark = out.size();
      if (!first) out.push_back(',');
      if (!gap.empty()) {
        out.push_back('\n');
        out.append(indent.view());
      }
      json_quote(key->view(), &out);
      out.push_back(':');
      if (!gap.empty()) out.push_back(' ');
      bool wrote;
      if (!write(object, key, item, &wrote)) return false;
      if (!wrote) out.truncate(mark);
      else first = false;
    }
    if (!first && !gap.empty()) {
      out.push_back('\n');
      out.append(indent.view().first(indent.size() - gap.size()));
    }
    out.push_back('}');
    return true;
  }
};

NATIVE(json_stringify) {
  JsonWriter w(vm);
  const Value replacer = arg_at(args, argc, 1);
  if (Vm::is_callable(replacer)) {
    w.replacer_function = replacer;
  } else if (replacer.is_object() && replacer.as_object()->okind == ObjKind::Array) {
    JsObject* names = vm.new_array();
    for (uint32_t i = 0; i < replacer.as_object()->length; ++i) {
      const Value item = replacer.as_object()->items[i];
      if (!item.is_string() && !item.is_number()) continue;
      JsString* name;
      if (!vm.to_string(item, &name)) return false;
      name = vm.intern(name->view());
      bool seen = false;
      for (uint32_t k = 0; k < names->length; ++k) seen = seen || names->items[k].as_string() == name;
      if (!seen) vm.array_push(names, Value::make_string(name));
    }
    w.allowed = names;
  }
  const Value g = arg_at(args, argc, 2);
  if (g.is_number()) {
    for (int i = 0; i < static_cast<int>(g.u.number) && i < 10; ++i) w.gap.push_back(' ');
  } else if (g.is_string()) {
    w.gap.assign(g.as_string()->view().first(g.as_string()->length < 10 ? g.as_string()->length : 10));
  }
  JsObject* holder = vm.new_plain();
  vm.define_named(holder, "", arg_at(args, argc, 0));
  bool wrote;
  if (!w.write(Value::make_object(holder), vm.empty_string(), arg_at(args, argc, 0), &wrote)) return false;
  *result = wrote ? str_value(vm, w.out.view()) : Value();
  return true;
}

class JsonReader {
 public:
  JsonReader(Vm& vm, StringView text) : vm_(vm), text_(text) {}
  bool parse(Value* out) {
    skip();
    if (!value(out, 0)) return false;
    skip();
    if (pos_ != text_.size()) return fail();
    return true;
  }

 private:
  // The messages are the ones browsers give: where the text stopped making sense, and what was there.
  bool fail() {
    String message(vm_.allocator());
    if (pos_ >= text_.size()) {
      message.append("Unexpected end of JSON input");
    } else {
      const char c = text_[pos_];
      if (c == '"') message.append("Unexpected string in JSON at position ");
      else if ((c >= '0' && c <= '9') || c == '-') message.append("Unexpected number in JSON at position ");
      else {
        message.append("Unexpected token ");
        message.push_back(c);
        message.append(" in JSON at position ");
      }
      message.append_uint(pos_);
    }
    return vm_.throw_error(ErrorKind::SyntaxError, message.view());
  }
  void skip() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_;
  }
  static void utf8(uint32_t cp, String* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  bool hex4(uint32_t* cp) {
    *cp = 0;
    for (int i = 0; i < 4; ++i) {
      if (pos_ >= text_.size()) return fail();
      const char h = text_[pos_];
      const int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
      if (d < 0) return fail();
      *cp = *cp * 16 + static_cast<uint32_t>(d);
      ++pos_;
    }
    return true;
  }
  bool string(String* out) {
    ++pos_;   // the opening quote
    for (;;) {
      if (pos_ >= text_.size()) return fail();
      char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (static_cast<uint8_t>(c) < 0x20) return fail();   // (a control character must be escaped)
      ++pos_;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) return fail();
      c = text_[pos_];
      switch (c) {
        case '"': case '\\': case '/': out->push_back(c); ++pos_; break;
        case 'n': out->push_back('\n'); ++pos_; break;
        case 't': out->push_back('\t'); ++pos_; break;
        case 'r': out->push_back('\r'); ++pos_; break;
        case 'b': out->push_back('\b'); ++pos_; break;
        case 'f': out->push_back('\f'); ++pos_; break;
        case 'u': {
          ++pos_;
          uint32_t cp;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {   // a surrogate pair is one character
            const uint64_t before = pos_;
            pos_ += 2;
            uint32_t low;
            if (!hex4(&low)) return false;
            if (low >= 0xDC00 && low < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            else pos_ = before;
          }
          utf8(cp, out);
          break;
        }
        default: return fail();
      }
    }
  }
  bool literal(const char* word, uint64_t length) {
    if (!text_.substr(pos_).starts_with(word)) return false;
    pos_ += length;
    return true;
  }
  // number = [-] int [. digits] [e [+-] digits], where int is 0 or a digit 1-9 and more digits.
  bool number(Value* out) {
    const uint64_t start = pos_;
    auto digit = [&](uint64_t at) { return at < text_.size() && text_[at] >= '0' && text_[at] <= '9'; };
    if (text_[pos_] == '-') ++pos_;
    if (!digit(pos_)) return fail();
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      while (digit(pos_)) ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (!digit(pos_)) return fail();
      while (digit(pos_)) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (!digit(pos_)) return fail();
      while (digit(pos_)) ++pos_;
    }
    double d;
    if (!string_to_number(text_.substr(start, pos_ - start), &d)) {
      pos_ = start;
      return fail();
    }
    *out = num_value(d);
    return true;
  }
  bool value(Value* out, uint32_t depth) {
    if (depth > 512 || pos_ >= text_.size()) return fail();
    const char c = text_[pos_];
    if (c == '{') {
      JsObject* o = vm_.new_plain();
      *out = Value::make_object(o);   // (no script runs while parsing: nothing can collect it)
      ++pos_;
      skip();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      for (;;) {
        skip();
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail();
        String key(vm_.allocator());
        if (!string(&key)) return false;
        skip();
        if (pos_ >= text_.size() || text_[pos_] != ':') return fail();
        ++pos_;
        skip();
        Value item;
        if (!value(&item, depth + 1)) return false;
        vm_.define(o, vm_.intern(key.view()), item);
        skip();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < text_.size() && text_[pos_] == '}') {
          ++pos_;
          return true;
        }
        return fail();
      }
    }
    if (c == '[') {
      JsObject* a = vm_.new_array();
      *out = Value::make_object(a);
      ++pos_;
      skip();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      for (;;) {
        skip();
        Value item;
        if (!value(&item, depth + 1)) return false;
        vm_.array_push(a, item);
        skip();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < text_.size() && text_[pos_] == ']') {
          ++pos_;
          return true;
        }
        return fail();
      }
    }
    if (c == '"') {
      String s(vm_.allocator());
      if (!string(&s)) return false;
      *out = str_value(vm_, s.view());
      return true;
    }
    if (c == 't' && literal("true", 4)) { *out = Value::make_bool(true); return true; }
    if (c == 'f' && literal("false", 5)) { *out = Value::make_bool(false); return true; }
    if (c == 'n' && literal("null", 4)) { *out = Value::make_null(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) return number(out);
    return fail();
  }

  Vm& vm_;
  StringView text_;
  uint64_t pos_ = 0;
};

// The reviver walk of JSON.parse: the children first, then holder[key] itself, each replaced by what the reviver returns (undefined removes it).
bool json_revive(Vm& vm, const Value& reviver, const Value& holder, JsString* key, Value* out, uint32_t depth) {
  Value value;
  if (!vm.get(holder, key, &value)) return false;
  if (value.is_object() && depth < 512) {
    JsObject* o = value.as_object();
    if (o->okind == ObjKind::Array) {
      for (uint32_t i = 0; i < o->length; ++i) {
        String name(vm.allocator());
        name.append_uint(i);
        JsString* k = vm.intern(name.view());
        Value revived;
        if (!json_revive(vm, reviver, value, k, &revived, depth + 1)) return false;
        if (i >= o->length) break;
        if (revived.is_undefined()) o->items[i] = Value();
        else o->items[i] = revived;
      }
    } else {
      JsObject* keys = vm.own_keys(o);
      for (uint32_t i = 0; i < keys->length; ++i) {
        JsString* k = keys->items[i].as_string();
        Value revived;
        if (!json_revive(vm, reviver, value, k, &revived, depth + 1)) return false;
        if (revived.is_undefined()) vm.delete_property(o, k);
        else vm.define(o, k, revived);
      }
    }
  }
  const Value call_args[2] = {Value::make_string(key), value};
  return vm.call(reviver, holder, call_args, 2, out);
}

NATIVE(json_parse) {
  JsString* s;
  if (!arg_string(vm, args, argc, 0, &s)) return false;
  JsonReader reader(vm, s->view());
  Value parsed;
  if (!reader.parse(&parsed)) return false;
  const Value reviver = arg_at(args, argc, 1);
  if (!Vm::is_callable(reviver)) {
    *result = parsed;
    return true;
  }
  JsObject* holder = vm.new_plain();
  vm.define_named(holder, "", parsed);
  return json_revive(vm, reviver, Value::make_object(holder), vm.empty_string(), result, 0);
}

// --- console, document, timers, Date ---------------------------------------------------------------------------------

bool console_write(Vm& vm, const Value* args, uint32_t argc, int level) {
  String text(vm.allocator());
  for (uint32_t i = 0; i < argc; ++i) {
    if (i) text.push_back(' ');
    vm.describe(args[i], &text, 0);
  }
  if (vm.host()) vm.host()->console(level, text.view());
  return true;
}
NATIVE(console_log) { return console_write(vm, args, argc, 0); }
NATIVE(console_info) { return console_write(vm, args, argc, 1); }
NATIVE(console_warn) { return console_write(vm, args, argc, 2); }
NATIVE(console_error) { return console_write(vm, args, argc, 3); }

NATIVE(global_alert) {
  JsString* s;
  if (!arg_string(vm, args, argc, 0, &s)) return false;
  if (vm.host()) vm.host()->alert(s->view());
  return true;
}

bool document_write_impl(Vm& vm, const Value* args, uint32_t argc, bool newline) {
  String text(vm.allocator());
  for (uint32_t i = 0; i < argc; ++i) {
    JsString* s;
    if (!vm.to_string(args[i], &s)) return false;
    text.append(s->view());
  }
  if (vm.host()) vm.host()->document_write(text.view(), newline);
  return true;
}
NATIVE(document_write) { return document_write_impl(vm, args, argc, false); }
NATIVE(document_writeln) { return document_write_impl(vm, args, argc, true); }

NATIVE(return_null) {
  *result = Value::make_null();
  return true;
}

NATIVE(return_empty_array) {
  *result = Value::make_object(vm.new_array());
  return true;
}

NATIVE(do_nothing) { return true; }

// Listeners and timers are kept in arrays on the global object, where the collector sees them and the page loader can run them.
NATIVE(add_event_listener) {
  Value list;
  if (!vm.get_named(Value::make_object(vm.global()), "__listeners", &list) || !list.is_object()) return true;
  if (!Vm::is_callable(arg_at(args, argc, 1))) return true;
  JsObject* entry = vm.new_array(2);
  vm.array_push(entry, arg_at(args, argc, 0));
  vm.array_push(entry, args[1]);
  vm.array_push(list.as_object(), Value::make_object(entry));
  return true;
}

// Source text compiled and run as a program of its own (in the global scope): what eval() and the Function constructor do.
bool run_source(Vm& vm, StringView source, Value* result) {
  Arena arena(vm.allocator());
  String message(vm.allocator());
  Node* program = parse_program(source, arena, &message);
  if (!program) return vm.throw_error(ErrorKind::SyntaxError, message.view());
  FunctionCode* code = nullptr;
  if (!compile_program(vm, program, &code, &message)) return vm.throw_error(ErrorKind::SyntaxError, message.view());
  JsObject* fn = vm.new_function(code, nullptr, Value());
  return vm.call(Value::make_object(fn), Value::make_object(vm.global()), nullptr, 0, result);
}

// __compile_module(source): [the module function, [the specifiers it asks for]]. The function is called with (dependency namespaces, the exports object,
// import.meta) and returns a promise (see parse_module). Throws a SyntaxError.
NATIVE(compile_module) {
  JsString* source;
  if (!vm.to_string(arg_at(args, argc, 0), &source)) return false;
  Arena arena(vm.allocator());
  String message(vm.allocator());
  ModuleRequests requests(vm.allocator());
  Node* program = parse_module(source->view(), arena, &message, &requests);
  if (!program) return vm.throw_error(ErrorKind::SyntaxError, message.view());
  FunctionCode* code = nullptr;
  if (!compile_program(vm, program, &code, &message)) return vm.throw_error(ErrorKind::SyntaxError, message.view());
  JsObject* top = vm.new_function(code, nullptr, Value());
  Value module_function;
  if (!vm.call(Value::make_object(top), Value::make_object(vm.global()), nullptr, 0, &module_function)) return false;
  JsObject* list = vm.new_array();
  for (uint64_t i = 0; i < requests.specifiers.count(); ++i) vm.array_push(list, str_value(vm, requests.specifiers[i].view()));
  auto strings = [&](const List<String>& names) {
    JsObject* array = vm.new_array();
    for (uint64_t i = 0; i < names.count(); ++i) vm.array_push(array, str_value(vm, names[i].view()));
    return Value::make_object(array);
  };
  JsObject* out = vm.new_array(5);
  vm.array_push(out, module_function);
  vm.array_push(out, Value::make_object(list));
  vm.array_push(out, strings(requests.imports));
  vm.array_push(out, strings(requests.exports));
  vm.array_push(out, strings(requests.stars));
  *result = Value::make_object(out);
  return true;
}

NATIVE(global_eval) {
  const Value code = arg_at(args, argc, 0);
  if (!code.is_string()) {
    *result = code;
    return true;
  }
  return run_source(vm, code.as_string()->view(), result);
}

// new Function("a", "b", "return a + b")
NATIVE(function_constructor) {
  String source(vm.allocator());
  source.append("(function anonymous(");
  for (uint32_t i = 0; i + 1 < argc; ++i) {
    JsString* p;
    if (!vm.to_string(args[i], &p)) return false;
    if (i > 0) source.push_back(',');
    source.append(p->view());
  }
  source.append("\n) {\n");
  if (argc > 0) {
    JsString* body;
    if (!vm.to_string(args[argc - 1], &body)) return false;
    source.append(body->view());
  }
  source.append("\n})");
  return run_source(vm, source.view(), result);
}

bool timer_native(Vm& vm, const Value* args, uint32_t argc, Value* result, bool repeat) {
  Value callback = arg_at(args, argc, 0);
  if (callback.is_string()) {   // setTimeout("code()", 10): the text is compiled once and run as a program each time
    String source(vm.allocator());
    source.append("(function () {\n");
    source.append(callback.as_string()->view());
    source.append("\n})");
    if (!run_source(vm, source.view(), &callback)) return false;
  }
  if (!Vm::is_callable(callback)) return vm.throw_error(ErrorKind::TypeError, repeat ? "setInterval needs a function" : "setTimeout needs a function");
  double delay = 0;
  if (argc > 1 && !vm.to_number(args[1], &delay)) return false;
  *result = num_value(vm.add_timer(callback, delay, repeat ? (delay == delay && delay >= 0 ? delay : 0) : -1, argc > 2 ? args + 2 : nullptr, argc > 2 ? argc - 2 : 0));
  return true;
}
NATIVE(set_timeout) { return timer_native(vm, args, argc, result, false); }
NATIVE(set_interval) { return timer_native(vm, args, argc, result, true); }

NATIVE(clear_timeout) {
  double id;
  if (!arg_number(vm, args, argc, 0, &id)) return false;
  vm.clear_timer(id);
  return true;
}

NATIVE(date_now) {
  *result = num_value(vm.clock_ms());
  return true;
}

NATIVE(date_ctor) {
  JsObject* d = vm.new_plain();
  double t = vm.clock_ms();
  if (argc > 0 && args[0].is_number()) t = args[0].u.number;
  vm.define_named(d, "__t", num_value(t), kWritable);
  vm.define(d, vm.intern("__date"), Value::make_bool(true), kWritable);
  JsString* proto_name = vm.intern("__date_proto");
  Value proto;
  vm.get(Value::make_object(vm.global()), proto_name, &proto);
  if (proto.is_object()) d->proto = proto.as_object();
  *result = Value::make_object(d);
  return true;
}

bool date_time(Vm& vm, const Value& self, double* t) {
  Value v;
  if (!self.is_object() || !vm.get_named(self, "__t", &v) || !v.is_number()) return vm.throw_error(ErrorKind::TypeError, "this is not a Date object");
  *t = v.u.number;
  return true;
}

// Civil date of a day number (days since 1970-01-01), from the days-from-civil algorithm.
void civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  *y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  *y += *m <= 2;
}

bool date_part(Vm& vm, const Value& self, Value* result, int which) {
  double t;
  if (!date_time(vm, self, &t)) return false;
  const int64_t ms = static_cast<int64_t>(t);
  int64_t days = ms / 86400000;
  int64_t rem = ms % 86400000;
  if (rem < 0) {
    rem += 86400000;
    --days;
  }
  int64_t y;
  int m, d;
  civil_from_days(days, &y, &m, &d);
  double r = 0;
  switch (which) {
    case 0: r = static_cast<double>(y); break;
    case 1: r = m - 1; break;
    case 2: r = d; break;
    case 3: r = static_cast<double>(rem / 3600000); break;
    case 4: r = static_cast<double>(rem / 60000 % 60); break;
    case 5: r = static_cast<double>(rem / 1000 % 60); break;
    case 6: r = static_cast<double>(rem % 1000); break;
    default: r = static_cast<double>(((days % 7) + 11) % 7); break;   // (1970-01-01 was a Thursday)
  }
  *result = num_value(r);
  return true;
}
NATIVE(date_get_time) {
  double t;
  if (!date_time(vm, self, &t)) return false;
  *result = num_value(t);
  return true;
}
NATIVE(date_full_year) { return date_part(vm, self, result, 0); }
NATIVE(date_month) { return date_part(vm, self, result, 1); }
NATIVE(date_date) { return date_part(vm, self, result, 2); }
NATIVE(date_hours) { return date_part(vm, self, result, 3); }
NATIVE(date_minutes) { return date_part(vm, self, result, 4); }
NATIVE(date_seconds) { return date_part(vm, self, result, 5); }
NATIVE(date_milliseconds) { return date_part(vm, self, result, 6); }
NATIVE(date_day) { return date_part(vm, self, result, 7); }

NATIVE(date_to_iso) {
  Value parts[7];
  for (int i = 0; i < 7; ++i)
    if (!date_part(vm, self, &parts[i], i)) return false;
  String text(vm.allocator());
  auto two = [&](double v, int width) {
    String n(vm.allocator());
    n.append_uint(static_cast<uint64_t>(v));
    for (uint64_t k = n.size(); k < static_cast<uint64_t>(width); ++k) text.push_back('0');
    text.append(n.view());
  };
  two(parts[0].u.number, 4);
  text.push_back('-');
  two(parts[1].u.number + 1, 2);
  text.push_back('-');
  two(parts[2].u.number, 2);
  text.push_back('T');
  two(parts[3].u.number, 2);
  text.push_back(':');
  two(parts[4].u.number, 2);
  text.push_back(':');
  two(parts[5].u.number, 2);
  text.push_back('.');
  two(parts[6].u.number, 3);
  text.push_back('Z');
  *result = str_value(vm, text.view());
  return true;
}

}  // namespace

// --- Installing ----------------------------------------------------------------------------------------------------

bool install_builtins(Vm& vm) {
  JsObject* g = vm.global();
  auto global_value = [&](StringView name, const Value& v) { vm.define_named(g, name, v, kWritable); };
  auto make_ctor = [&](StringView name, NativeFn fn, uint32_t arity, JsObject* proto) {
    JsObject* c = vm.new_native(fn, name, arity);
    vm.define(c, vm.prototype_name(), Value::make_object(proto), 0);
    vm.define(proto, vm.intern("constructor"), Value::make_object(c), kWritable);
    global_value(name, Value::make_object(c));
    return c;
  };

  // Object and Function
  JsObject* object = make_ctor("Object", object_ctor, 1, vm.object_proto());
  vm.define_native(object, "keys", object_keys, 1);
  vm.define_native(object, "values", object_values, 1);
  vm.define_native(object, "entries", object_entries, 1);
  vm.define_native(object, "assign", object_assign, 2);
  vm.define_native(object, "create", object_create, 1);
  vm.define_native(object, "freeze", object_freeze, 1);
  vm.define_native(object, "isFrozen", object_is_frozen, 1);
  vm.define_native(object, "getPrototypeOf", object_get_prototype_of, 1);
  vm.define_native(object, "defineProperty", object_define_property, 3);
  vm.define_native(object, "defineProperties", object_define_properties, 2);
  vm.define_native(object, "getOwnPropertyDescriptor", object_get_own_property_descriptor, 2);
  vm.define_native(object, "getOwnPropertyNames", object_get_own_property_names, 1);
  vm.define_native(object, "setPrototypeOf", object_set_prototype_of, 2);
  vm.define_native(vm.object_proto(), "hasOwnProperty", object_has_own_property, 1);
  JsObject* generator_proto = vm.new_plain();   // what the objects made by generator functions inherit from
  vm.define_native(generator_proto, "next", generator_next, 1);
  vm.define_native(generator_proto, "throw", generator_throw, 1);
  vm.define_native(generator_proto, "return", generator_return, 1);
  vm.define_native(generator_proto, "toString", generator_to_string, 0);
  vm.define(generator_proto, vm.intern("@@iterator"), Value::make_object(vm.new_native(return_this, "[Symbol.iterator]", 0)), kWritable);
  vm.define_named(g, "__generator_proto", Value::make_object(generator_proto), 0);
  vm.define_native(g, "__template", template_object, 2);
  vm.define_native(g, "__bigint_new", bigint_new, 2);
  vm.define_native(g, "__to_array", destructure_to_array, 2);
  vm.define_native(g, "__req_object", destructure_require_object, 1);
  vm.define_native(g, "__rest", destructure_rest, 2);
  vm.define_native(g, "__class", class_setup, 2);
  vm.define_native(g, "__def", class_define, 4);
  vm.define_native(g, "__super_init", class_super_init, 2);
  vm.define_native(vm.object_proto(), "toString", object_to_string, 0);
  vm.define_native(vm.object_proto(), "valueOf", object_value_of, 0);
  make_ctor("Function", function_constructor, 1, vm.function_proto());
  vm.define_native(vm.function_proto(), "call", function_call, 1);
  vm.define_native(vm.function_proto(), "apply", function_apply, 2);
  vm.define_native(vm.function_proto(), "bind", function_bind, 1);
  vm.define_native(vm.function_proto(), "toString", function_to_string, 0);

  // Array
  JsObject* array = make_ctor("Array", array_ctor, 1, vm.array_proto());
  vm.define_native(array, "isArray", array_is_array, 1);
  JsObject* ap = vm.array_proto();
  vm.define_native(ap, "push", array_push_m, 1);
  vm.define_native(ap, "pop", array_pop, 0);
  vm.define_native(ap, "shift", array_shift, 0);
  vm.define_native(ap, "unshift", array_unshift, 1);
  vm.define_native(ap, "slice", array_slice, 2);
  vm.define_native(ap, "splice", array_splice, 2);
  vm.define_native(ap, "concat", array_concat, 1);
  vm.define_native(ap, "join", array_join, 1);
  vm.define_native(ap, "indexOf", array_index_of, 1);
  vm.define_native(ap, "lastIndexOf", array_last_index_of, 1);
  vm.define_native(ap, "includes", array_includes, 1);
  vm.define_native(ap, "reverse", array_reverse, 0);
  vm.define_native(ap, "fill", array_fill, 1);
  vm.define_native(ap, "forEach", array_for_each, 1);
  vm.define_native(ap, "map", array_map, 1);
  vm.define_native(ap, "filter", array_filter, 1);
  vm.define_native(ap, "find", array_find, 1);
  vm.define_native(ap, "findIndex", array_find_index, 1);
  vm.define_native(ap, "some", array_some, 1);
  vm.define_native(ap, "every", array_every, 1);
  vm.define_native(ap, "reduce", array_reduce, 1);
  vm.define_native(ap, "reduceRight", array_reduce_right, 1);
  vm.define_native(ap, "sort", array_sort, 1);
  vm.define_native(ap, "flat", array_flat, 0);
  vm.define_native(ap, "toString", array_to_string, 0);
  vm.define_native(ap, "@@iterator", array_values, 0);
  vm.define_native(ap, "values", array_values, 0);

  // String
  JsObject* string = make_ctor("String", string_ctor, 1, vm.string_proto());
  vm.define_native(string, "fromCharCode", string_from_char_code, 1);
  vm.define_native(string, "fromCodePoint", string_from_code_point, 1);
  JsObject* sp = vm.string_proto();
  vm.define_native(sp, "charAt", string_char_at, 1);
  vm.define_native(sp, "at", string_at, 1);
  vm.define_native(sp, "charCodeAt", string_char_code_at, 1);
  vm.define_native(sp, "indexOf", string_index_of, 1);
  vm.define_native(sp, "lastIndexOf", string_last_index_of, 1);
  vm.define_native(sp, "includes", string_includes, 1);
  vm.define_native(sp, "startsWith", string_starts_with, 1);
  vm.define_native(sp, "endsWith", string_ends_with, 1);
  vm.define_native(sp, "slice", string_slice, 2);
  vm.define_native(sp, "substring", string_substring, 2);
  vm.define_native(sp, "substr", string_substr, 2);
  vm.define_native(sp, "toUpperCase", string_upper, 0);
  vm.define_native(sp, "toLowerCase", string_lower, 0);
  vm.define_native(sp, "trim", string_trim, 0);
  vm.define_native(sp, "trimStart", string_trim_start, 0);
  vm.define_native(sp, "trimEnd", string_trim_end, 0);
  vm.define_native(sp, "split", string_split, 2);
  vm.define_native(sp, "replace", string_replace, 2);
  vm.define_native(sp, "match", string_match, 1);
  vm.define_native(sp, "matchAll", string_match_all, 1);
  vm.define_native(sp, "search", string_search, 1);
  vm.define_native(sp, "replaceAll", string_replace_all, 2);
  vm.define_native(sp, "repeat", string_repeat, 1);
  vm.define_native(sp, "padStart", string_pad_start, 2);
  vm.define_native(sp, "padEnd", string_pad_end, 2);
  vm.define_native(sp, "concat", string_concat, 1);
  vm.define_native(sp, "localeCompare", string_locale_compare, 1);
  vm.define_native(sp, "toString", string_to_string, 0);
  vm.define_native(sp, "valueOf", string_to_string, 0);

  // Number and Boolean
  JsObject* number = make_ctor("Number", number_ctor, 1, vm.number_proto());
  vm.define_native(number, "isInteger", number_is_integer, 1);
  vm.define_native(number, "isFinite", number_is_finite, 1);
  vm.define_native(number, "isNaN", number_is_nan, 1);
  vm.define_native(number, "parseFloat", global_parse_float, 1);
  vm.define_native(number, "parseInt", global_parse_int, 2);
  vm.define_named(number, "MAX_SAFE_INTEGER", num_value(9007199254740991.0), 0);
  vm.define_named(number, "MIN_SAFE_INTEGER", num_value(-9007199254740991.0), 0);
  vm.define_named(number, "MAX_VALUE", num_value(1.7976931348623157e308), 0);
  vm.define_named(number, "MIN_VALUE", num_value(5e-324), 0);
  vm.define_named(number, "EPSILON", num_value(2.220446049250313e-16), 0);
  vm.define_named(number, "POSITIVE_INFINITY", num_value(kInf), 0);
  vm.define_named(number, "NEGATIVE_INFINITY", num_value(-kInf), 0);
  vm.define_named(number, "NaN", num_value(kNaN), 0);
  vm.define_native(vm.number_proto(), "toString", number_to_string_m, 1);
  vm.define_native(vm.number_proto(), "toFixed", number_to_fixed_m, 1);
  vm.define_native(vm.number_proto(), "valueOf", number_value_of, 0);
  make_ctor("Boolean", boolean_ctor, 1, vm.boolean_proto());
  vm.define_native(vm.boolean_proto(), "toString", boolean_to_string, 0);
  vm.define_native(vm.boolean_proto(), "valueOf", boolean_value_of, 0);

  // Globals
  global_value("NaN", num_value(kNaN));
  global_value("Infinity", num_value(kInf));
  global_value("undefined", Value());
  vm.define_native(g, "parseInt", global_parse_int, 2);
  vm.define_native(g, "parseFloat", global_parse_float, 1);
  vm.define_native(g, "isNaN", global_is_nan, 1);
  vm.define_native(g, "isFinite", global_is_finite, 1);
  vm.define_native(g, "alert", global_alert, 1);
  vm.define_native(g, "eval", global_eval, 1);
  vm.define_native(g, "__compile_module", compile_module, 1);
  vm.define_native(g, "setTimeout", set_timeout, 2);
  vm.define_native(g, "setInterval", set_interval, 2);
  vm.define_native(g, "clearTimeout", clear_timeout, 1);
  vm.define_native(g, "clearInterval", clear_timeout, 1);
  vm.define_native(g, "addEventListener", add_event_listener, 2);
  vm.define_native(g, "removeEventListener", do_nothing, 2);
  vm.define_named(g, "__timers", Value::make_object(vm.new_array()), 0);
  vm.define_named(g, "__listeners", Value::make_object(vm.new_array()), 0);
  global_value("window", Value::make_object(g));
  global_value("self", Value::make_object(g));
  global_value("globalThis", Value::make_object(g));

  // Math
  JsObject* math = vm.new_plain();
  global_value("Math", Value::make_object(math));
  vm.define_named(math, "@@toStringTag", str_value(vm, "Math"), 0);
  vm.define_named(math, "PI", num_value(3.141592653589793), 0);
  vm.define_named(math, "E", num_value(2.718281828459045), 0);
  vm.define_named(math, "LN2", num_value(0.6931471805599453), 0);
  vm.define_named(math, "LN10", num_value(2.302585092994046), 0);
  vm.define_named(math, "SQRT2", num_value(1.4142135623730951), 0);
  vm.define_native(math, "abs", math_abs, 1);
  vm.define_native(math, "floor", math_floor, 1);
  vm.define_native(math, "ceil", math_ceil, 1);
  vm.define_native(math, "round", math_round, 1);
  vm.define_native(math, "trunc", math_trunc, 1);
  vm.define_native(math, "sign", math_sign, 1);
  vm.define_native(math, "sqrt", math_sqrt, 1);
  vm.define_native(math, "cbrt", math_cbrt, 1);
  vm.define_native(math, "sin", math_sin, 1);
  vm.define_native(math, "cos", math_cos, 1);
  vm.define_native(math, "tan", math_tan, 1);
  vm.define_native(math, "atan", math_atan, 1);
  vm.define_native(math, "atan2", math_atan2, 2);
  vm.define_native(math, "exp", math_exp, 1);
  vm.define_native(math, "log", math_log, 1);
  vm.define_native(math, "log10", math_log10, 1);
  vm.define_native(math, "log2", math_log2, 1);
  vm.define_native(math, "pow", math_pow, 2);
  vm.define_native(math, "min", math_min, 2);
  vm.define_native(math, "max", math_max, 2);
  vm.define_native(math, "random", math_random, 0);
  vm.define_native(math, "hypot", math_hypot, 2);

  // Symbol
  JsObject* symbol = vm.new_native(symbol_function, "Symbol", 0);
  global_value("Symbol", Value::make_object(symbol));
  for (const char* name : {"iterator", "asyncIterator", "hasInstance", "toPrimitive", "toStringTag", "species"}) {
    String well_known(vm.allocator());
    well_known.append("@@");
    well_known.append(name);
    vm.define_named(symbol, name, str_value(vm, well_known.view()), 0);
  }

  // Promise
  JsObject* promise = make_ctor("Promise", promise_ctor, 1, vm.promise_proto());
  vm.define_native(promise, "resolve", promise_static_resolve, 1);
  vm.define_native(promise, "reject", promise_static_reject, 1);
  vm.define_native(vm.promise_proto(), "then", promise_then, 2);
  vm.define_native(vm.promise_proto(), "catch", promise_catch, 1);

  // JSON
  JsObject* json = vm.new_plain();
  global_value("JSON", Value::make_object(json));
  vm.define_named(json, "@@toStringTag", str_value(vm, "JSON"), 0);
  vm.define_native(json, "stringify", json_stringify, 3);
  vm.define_native(json, "parse", json_parse, 1);

  // Errors
  vm.define_native(vm.error_proto(), "toString", error_to_string, 0);
  struct ErrorSpec {
    const char* name;
    NativeFn fn;
  };
  const ErrorSpec specs[5] = {{"Error", error_ctor<0>}, {"TypeError", error_ctor<1>}, {"RangeError", error_ctor<2>}, {"ReferenceError", error_ctor<3>}, {"SyntaxError", error_ctor<4>}};
  for (int i = 0; i < 5; ++i) {
    JsObject* proto = vm.error_proto_of(i);
    make_ctor(specs[i].name, specs[i].fn, 1, proto);
    vm.define(proto, vm.name_name(), str_value(vm, specs[i].name), kWritable);
    vm.define(proto, vm.message_name(), Value::make_string(vm.empty_string()), kWritable);
  }

  // console
  JsObject* console = vm.new_plain();
  global_value("console", Value::make_object(console));
  vm.define_native(console, "log", console_log, 1);
  vm.define_native(console, "debug", console_log, 1);
  vm.define_native(console, "info", console_info, 1);
  vm.define_native(console, "warn", console_warn, 1);
  vm.define_native(console, "error", console_error, 1);

  // Date
  JsObject* date_proto = vm.new_plain();
  JsObject* date = make_ctor("Date", date_ctor, 1, date_proto);
  vm.define_native(date, "now", date_now, 0);
  vm.define_native(date_proto, "getTime", date_get_time, 0);
  vm.define_native(date_proto, "valueOf", date_get_time, 0);
  vm.define_native(date_proto, "getFullYear", date_full_year, 0);
  vm.define_native(date_proto, "getMonth", date_month, 0);
  vm.define_native(date_proto, "getDate", date_date, 0);
  vm.define_native(date_proto, "getDay", date_day, 0);
  vm.define_native(date_proto, "getHours", date_hours, 0);
  vm.define_native(date_proto, "getMinutes", date_minutes, 0);
  vm.define_native(date_proto, "getSeconds", date_seconds, 0);
  vm.define_native(date_proto, "getMilliseconds", date_milliseconds, 0);
  vm.define_native(date_proto, "toISOString", date_to_iso, 0);
  vm.define_native(date_proto, "toJSON", date_to_iso, 0);
  vm.define_native(date_proto, "toString", date_to_iso, 0);
  vm.define_named(g, "__date_proto", Value::make_object(date_proto), 0);

  // document, location, navigator
  JsObject* document = vm.new_plain();
  global_value("document", Value::make_object(document));
  vm.define_native(document, "write", document_write, 1);
  vm.define_native(document, "writeln", document_writeln, 1);
  vm.define_native(document, "getElementById", return_null, 1);
  vm.define_native(document, "querySelector", return_null, 1);
  vm.define_native(document, "querySelectorAll", return_empty_array, 1);
  vm.define_native(document, "getElementsByTagName", return_empty_array, 1);
  vm.define_native(document, "getElementsByClassName", return_empty_array, 1);
  vm.define_native(document, "addEventListener", add_event_listener, 2);
  vm.define_native(document, "removeEventListener", do_nothing, 2);
  vm.define_named(document, "title", str_value(vm, vm.host() ? vm.host()->title() : StringView()), kDefaultFlags);
  vm.define_named(document, "readyState", str_value(vm, "loading"), kDefaultFlags);
  vm.define_named(document, "referrer", str_value(vm, ""), kDefaultFlags);
  vm.define_named(document, "cookie", str_value(vm, ""), kDefaultFlags);
  JsObject* location = vm.new_plain();
  const StringView href = vm.host() ? vm.host()->location() : StringView();
  vm.define_named(location, "href", str_value(vm, href), kDefaultFlags);
  global_value("location", Value::make_object(location));
  vm.define_named(document, "location", Value::make_object(location), kDefaultFlags);
  JsObject* navigator = vm.new_plain();
  vm.define_named(navigator, "userAgent", str_value(vm, vm.host() ? vm.host()->user_agent() : StringView()), kDefaultFlags);
  vm.define_named(navigator, "language", str_value(vm, "en-US"), kDefaultFlags);
  global_value("navigator", Value::make_object(navigator));
  if (!install_regexp(vm) || !install_typed_arrays(vm)) return false;
  {
    JsObject* proxy = vm.new_native(proxy_constructor, "Proxy", 2);
    vm.define_native(proxy, "revocable", proxy_revocable, 2);
    vm.define_named(vm.global(), "Proxy", Value::make_object(proxy), kWritable);
  }
  return !vm.halted();
}

}  // namespace script
