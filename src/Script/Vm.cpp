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

#include "Script/Vm.hpp"
#ifdef TCOS_ERROR_LINES
#include <stdio.h>
#endif
#include "Script/RegExp.hpp"
#include "Script/TypedArray.hpp"
#include <initializer_list>
#include <new>
#include "Math/FloatMath.hpp"
#include "Script/Number.hpp"

namespace script {

namespace {

uint32_t hash_text(StringView s) {
  uint32_t h = 2166136261u;
  for (uint64_t i = 0; i < s.size(); ++i) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
  return h;
}

const char* const kErrorNames[5] = {"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError"};

}  // namespace

// An async function that is waiting at an `await`: what its frame looked like then.
struct Vm::Coroutine {
  explicit Coroutine(allocators::IAllocator& allocator) : stack(allocator), handlers(allocator) {}
  FunctionCode* code = nullptr;
  uint32_t pc = 0;
  Env* env = nullptr;
  Value this_value;
  JsObject* callee = nullptr;
  JsObject* promise = nullptr;   // the promise the call returned
  List<Value> stack;             // the values the frame had on the stack
  List<Handler> handlers;        // its try handlers (their `stack` counts from the frame's base)
  bool running = false;
  bool generator = false, done = false;   // a generator object (it is the coroutine itself), and whether it has finished
};

// --- Construction ------------------------------------------------------------------------------------------------

Vm::Vm(allocators::IAllocator& allocator, IHost* host)
    : allocator_(allocator), host_(host), gray_(allocator), codes_(allocator), halt_reason_(allocator), frames_(allocator), handlers_(allocator) {
  frames_.reserve(kMaxFrames + 8);   // (the interpreter keeps pointers to frames: the list must never move them)
  stack_ = static_cast<Value*>(allocator_.allocate(sizeof(Value) * kStackValues));
  interned_cap_ = 1024;
  interned_ = static_cast<JsString**>(allocator_.allocate(sizeof(JsString*) * interned_cap_));
  if (!stack_ || !interned_) return;
  for (uint32_t i = 0; i < interned_cap_; ++i) interned_[i] = nullptr;
  empty_ = intern("");
  length_ = intern("length");
  prototype_ = intern("prototype");
  name_ = intern("name");
  message_ = intern("message");
  constructor_ = intern("constructor");
  to_string_ = intern("toString");
  value_of_ = intern("valueOf");
  undefined_text_ = intern("undefined");
  null_text_ = intern("null");
  true_text_ = intern("true");
  false_text_ = intern("false");
  object_proto_ = new_object(nullptr);
  function_proto_ = new_object(object_proto_);
  array_proto_ = new_object(object_proto_);
  string_proto_ = new_object(object_proto_);
  number_proto_ = new_object(object_proto_);
  boolean_proto_ = new_object(object_proto_);
  error_proto_ = new_object(object_proto_);
  iterator_proto_ = new_object(object_proto_);
  sentinel_ = new_object(object_proto_);
  global_ = new_object(object_proto_);
  promise_proto_ = new_object(object_proto_);
  micro_ = new_array();
  rejections_ = new_array();
  error_protos_[0] = error_proto_;
  for (int i = 1; i < 5; ++i) error_protos_[i] = new_object(error_proto_);
  ok_ = empty_ && global_ && error_protos_[4] && install_builtins(*this);
  if (!ok_) halted_ = true;
}

Vm::~Vm() {
  Cell* c = cells_;
  while (c) {
    Cell* next = c->next;
    free_cell(c);
    c = next;
  }
  for (uint64_t i = 0; i < codes_.count(); ++i) {
    codes_[i]->~FunctionCode();
    allocator_.deallocate(codes_[i]);
  }
  allocator_.deallocate(interned_);
  allocator_.deallocate(stack_);
}

FunctionCode* Vm::new_code() {
  void* mem = allocator_.allocate(sizeof(FunctionCode));
  if (!mem) return nullptr;
  FunctionCode* code = new (mem) FunctionCode(allocator_);
  if (!codes_.push_back(code)) {
    code->~FunctionCode();
    allocator_.deallocate(mem);
    return nullptr;
  }
  return code;
}

void Vm::recover() {
  if (native_depth_ == 0) {
    frames_.clear();
    handlers_.clear();
    sp_ = 0;
  }
  if (halted_ && ok_) {
    if (micro_) micro_->length = 0;
    if (rejections_) rejections_->length = 0;
    micro_head_ = 0;
    draining_ = false;
    suspended_ = false;
    async_depth_ = 0;
    halted_ = false;
    halt_reason_.clear();
    has_exception_ = false;
    exception_ = Value();
    gc_wanted_ = true;
    if (native_depth_ == 0 && gc_hold_ == 0) collect();
  }
}

void Vm::halt(StringView reason) {
  if (halted_) return;
  halted_ = true;
  halt_reason_.assign(reason);
}

// --- The heap ----------------------------------------------------------------------------------------------------

void Vm::link(Cell* cell, uint64_t size) {
  cell->next = cells_;
  cells_ = cell;
  heap_bytes_ += size;
  if (heap_bytes_ > gc_threshold_) gc_wanted_ = true;
  if (heap_bytes_ > memory_limit_ && !halted_) {
    // Try to free something first; only stop the script if that does not help.
    gc_wanted_ = true;
    if (heap_bytes_ > memory_limit_ + (memory_limit_ >> 2)) halt("out of memory");
  }
}

void Vm::free_cell(Cell* cell) {
  switch (cell->kind) {
    case CellKind::String: {
      allocator_.deallocate(static_cast<JsString*>(cell)->wide);
      allocator_.deallocate(cell);
      break;
    }
    case CellKind::Env: allocator_.deallocate(cell); break;
    case CellKind::Object: {
      JsObject* o = static_cast<JsObject*>(cell);
      if (o->okind == ObjKind::Coroutine && o->host_data) {
        Coroutine* co = static_cast<Coroutine*>(o->host_data);
        co->~Coroutine();
        allocator_.deallocate(co);
      }
      if (o->okind == ObjKind::RegExp && o->host_data) regexp_release(allocator_, o->host_data);
      allocator_.deallocate(o->props);
      allocator_.deallocate(o->index);
      allocator_.deallocate(o->items);
      allocator_.deallocate(o);
      break;
    }
  }
}

uint8_t Vm::string_kind(JsString* s) {
  if (s->kind) return s->kind;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s->data());
  const uint32_t n = s->length;
  if (s->binary) {
    s->kind = 3;
    s->units = n;
    return 3;
  }
  bool ascii = true;
  uint32_t i = 0, units = 0;
  bool valid = true;
  while (i < n && valid) {
    const uint8_t c = p[i];
    if (c < 0x80) {
      ++i;
      ++units;
      continue;
    }
    ascii = false;
    uint32_t extra;
    if (c >= 0xC2 && c <= 0xDF) extra = 1;
    else if (c >= 0xE0 && c <= 0xEF) extra = 2;
    else if (c >= 0xF0 && c <= 0xF4) extra = 3;
    else { valid = false; break; }
    if (i + extra >= n) { valid = false; break; }
    for (uint32_t k = 1; k <= extra; ++k)
      if ((p[i + k] & 0xC0) != 0x80) valid = false;
    if (!valid) break;
    if (c == 0xE0 && p[i + 1] < 0xA0) valid = false;   // (overlong)
    if (c == 0xF0 && p[i + 1] < 0x90) valid = false;
    if (c == 0xF4 && p[i + 1] > 0x8F) valid = false;
    if (!valid) break;
    i += extra + 1;
    units += extra == 3 ? 2 : 1;
  }
  if (!valid) {
    s->kind = 3;
    s->units = n;
  } else if (ascii) {
    s->kind = 1;
    s->units = n;
  } else {
    s->kind = 2;
    s->units = units;
  }
  return s->kind;
}

namespace {
// A code point (a lone surrogate included) as UTF-8 bytes appended to `out`.
void append_wtf8(String* out, uint32_t cp) {
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
}  // namespace

const uint16_t* Vm::string_wide(JsString* s) {
  if (string_kind(s) != 2) return nullptr;
  if (s->wide) return s->wide;
  uint16_t* w = static_cast<uint16_t*>(allocator_.allocate(sizeof(uint16_t) * (uint64_t{s->units} + 1)));
  if (!w) {
    halt("out of memory");
    return nullptr;
  }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s->data());
  uint32_t i = 0, k = 0;
  while (i < s->length && k < s->units) {
    const uint8_t c = p[i];
    uint32_t cp, step;
    if (c < 0x80) { cp = c; step = 1; }
    else if (c < 0xE0) { cp = ((c & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu); step = 2; }
    else if (c < 0xF0) { cp = ((c & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu); step = 3; }
    else { cp = ((c & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) | ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu); step = 4; }
    i += step;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      w[k++] = static_cast<uint16_t>(0xD800 + (cp >> 10));
      if (k < s->units) w[k++] = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
    } else {
      w[k++] = static_cast<uint16_t>(cp);
    }
  }
  s->wide = w;
  heap_bytes_ += sizeof(uint16_t) * (uint64_t{s->units} + 1);
  return w;
}

JsString* Vm::string_from_units(const uint16_t* units, uint32_t count) {
  String out(allocator_);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t u = units[i];
    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {   // (a pair is one character)
      u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    }
    append_wtf8(&out, u);
  }
  return new_string(out.view());
}

uint64_t Vm::string_byte_of_unit(JsString* s, uint64_t unit) {
  if (string_kind(s) != 2) return unit;
  if (unit >= s->units) return s->length;
  uint32_t u = 0, b = 0;
  if (s->map_unit <= unit) { u = s->map_unit; b = s->map_byte; }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s->data());
  while (u < unit && b < s->length) {
    const uint8_t c = p[b];
    const uint32_t step = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    u += c >= 0xF0 ? 2 : 1;
    b += step;
  }
  s->map_unit = u;   // (u may be one past `unit` when it fell inside a surrogate pair: the pair's end)
  s->map_byte = b;
  return b;
}

uint64_t Vm::string_unit_of_byte(JsString* s, uint64_t byte) {
  if (string_kind(s) != 2) return byte;
  if (byte >= s->length) return s->units;
  uint32_t u = 0, b = 0;
  if (s->map_byte <= byte) { u = s->map_unit; b = s->map_byte; }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s->data());
  while (b < byte && b < s->length) {
    const uint8_t c = p[b];
    const uint32_t step = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    u += c >= 0xF0 ? 2 : 1;
    b += step;
  }
  s->map_unit = u;
  s->map_byte = b;
  return u;
}

JsString* Vm::new_binary_string(StringView bytes) {
  JsString* s = new_string(bytes);
  if (s != empty_ && !s->interned) s->binary = true;
  return s;
}

JsString* Vm::string_part(JsString* s, uint32_t from, uint32_t to) {
  const uint8_t kind = string_kind(s);
  if (to > s->units) to = s->units;
  if (from >= to) return empty_;
  if (kind == 2) {
    const uint16_t* w = string_wide(s);
    if (!w) return empty_;
    return string_from_units(w + from, to - from);
  }
  if (from == 0 && to == s->length) return s;
  JsString* part = new_string(StringView(s->data() + from, to - from));
  if (kind == 3 && part != empty_ && !part->interned) part->binary = true;
  return part;
}

uint32_t Vm::string_unit_at(JsString* s, uint32_t index) {
  if (string_kind(s) == 2) {
    const uint16_t* w = string_wide(s);
    return w && index < s->units ? w[index] : 0;
  }
  return index < s->length ? static_cast<uint8_t>(s->data()[index]) : 0;
}

JsString* Vm::new_string(StringView text) {
  if (text.empty() && empty_) return empty_;
  void* mem = allocator_.allocate(sizeof(JsString) + text.size() + 1);
  if (!mem) {
    halt("out of memory");
    return empty_;
  }
  JsString* s = new (mem) JsString();
  s->length = static_cast<uint32_t>(text.size());
  char* d = s->data();
  for (uint64_t i = 0; i < text.size(); ++i) d[i] = text[i];
  d[text.size()] = 0;
  s->hash = hash_text(text);
  link(s, sizeof(JsString) + text.size() + 1);
  return s;
}

JsString* Vm::new_bytes(uint32_t length) {
  if (length == 0) return empty_;
  void* mem = allocator_.allocate(sizeof(JsString) + uint64_t{length} + 1);
  if (!mem) {
    halt("out of memory");
    return empty_;
  }
  JsString* s = new (mem) JsString();
  s->length = length;
  char* d = s->data();
  for (uint32_t i = 0; i <= length; ++i) d[i] = 0;
  s->hash = 0;
  link(s, sizeof(JsString) + uint64_t{length} + 1);
  return s;
}

JsString* Vm::concat(JsString* a, JsString* b) {
  if (a->length == 0) return b;
  if (b->length == 0) return a;
  if (a->length >= 3 && b->length >= 3) {   // a lone high surrogate followed by a lone low one is one character (4 bytes, not 2 x 3)
    const uint8_t* ae = reinterpret_cast<const uint8_t*>(a->data()) + a->length - 3;
    const uint8_t* bs = reinterpret_cast<const uint8_t*>(b->data());
    if (ae[0] == 0xED && ae[1] >= 0xA0 && ae[1] <= 0xAF && bs[0] == 0xED && bs[1] >= 0xB0 && bs[1] <= 0xBF) {
      const uint32_t high = 0xD000u + ((ae[1] & 0x3Fu) << 6) + (ae[2] & 0x3Fu), low = 0xD000u + ((bs[1] & 0x3Fu) << 6) + (bs[2] & 0x3Fu);
      const uint32_t cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
      String merged(allocator_);
      merged.append(StringView(a->data(), a->length - 3));
      append_wtf8(&merged, cp);
      merged.append(StringView(b->data() + 3, b->length - 3));
      return new_string(merged.view());
    }
  }
  const uint64_t total = uint64_t{a->length} + b->length;
  if (total > (64u << 20)) {
    halt("string too long");
    return empty_;
  }
  void* mem = allocator_.allocate(sizeof(JsString) + total + 1);
  if (!mem) {
    halt("out of memory");
    return empty_;
  }
  JsString* s = new (mem) JsString();
  s->length = static_cast<uint32_t>(total);
  char* d = s->data();
  for (uint32_t i = 0; i < a->length; ++i) d[i] = a->data()[i];
  for (uint32_t i = 0; i < b->length; ++i) d[a->length + i] = b->data()[i];
  d[total] = 0;
  s->hash = hash_text(StringView(d, total));
  s->binary = a->binary || b->binary;   // (raw bytes stay raw)
  link(s, sizeof(JsString) + total + 1);
  return s;
}

JsString* Vm::intern(StringView text) {
  const uint32_t h = hash_text(text);
  const uint32_t mask = interned_cap_ - 1;
  for (uint32_t i = h & mask;; i = (i + 1) & mask) {
    JsString* s = interned_[i];
    if (!s) break;
    if (s->hash == h && s->length == text.size() && s->view().equals(text)) return s;
  }
  JsString* s = new_string(text);
  if (s == empty_ && !text.empty()) return s;
  if (s->interned) return s;   // (the empty string)
  s->interned = true;
  if ((interned_count_ + 1) * 2 > interned_cap_) {
    // grow the table
    const uint32_t cap = interned_cap_ * 2;
    JsString** fresh = static_cast<JsString**>(allocator_.allocate(sizeof(JsString*) * cap));
    if (!fresh) {
      halt("out of memory");
      return s;
    }
    for (uint32_t i = 0; i < cap; ++i) fresh[i] = nullptr;
    for (uint32_t i = 0; i < interned_cap_; ++i) {
      JsString* e = interned_[i];
      if (!e) continue;
      uint32_t k = e->hash & (cap - 1);
      while (fresh[k]) k = (k + 1) & (cap - 1);
      fresh[k] = e;
    }
    allocator_.deallocate(interned_);
    interned_ = fresh;
    interned_cap_ = cap;
  }
  uint32_t k = s->hash & (interned_cap_ - 1);
  while (interned_[k]) k = (k + 1) & (interned_cap_ - 1);
  interned_[k] = s;
  ++interned_count_;
  return s;
}

void Vm::rebuild_intern_table() {
  for (uint32_t i = 0; i < interned_cap_; ++i) interned_[i] = nullptr;
  interned_count_ = 0;
  for (Cell* c = cells_; c; c = c->next) {
    if (c->kind != CellKind::String) continue;
    JsString* s = static_cast<JsString*>(c);
    if (!s->interned) continue;
    uint32_t k = s->hash & (interned_cap_ - 1);
    while (interned_[k]) k = (k + 1) & (interned_cap_ - 1);
    interned_[k] = s;
    ++interned_count_;
  }
}

JsObject* Vm::new_object(JsObject* proto) {
  void* mem = allocator_.allocate(sizeof(JsObject));
  if (!mem) {
    halt("out of memory");
    return sentinel_;
  }
  JsObject* o = new (mem) JsObject();
  o->proto = proto;
  link(o, sizeof(JsObject));
  return o;
}

// --- BigInt --------------------------------------------------------------------------------------------------------------

JsObject* Vm::new_bigint(bool negative, StringView digits) {
  Value proto;
  get_plain(global_, intern("__bigint_proto"), &proto);
  JsObject* b = new_object(proto.is_object() ? proto.as_object() : object_proto_);
  b->okind = ObjKind::BigInt;
  uint64_t skip = 0;
  while (skip + 1 < digits.size() && digits[skip] == '0') ++skip;
  const StringView clean = digits.substr(skip);
  const bool zero = clean.empty() || clean.equals("0");
  define_named(b, "__neg", Value::make_bool(negative && !zero), 0);
  define_named(b, "__mag", Value::make_string(new_string(zero ? StringView("0") : clean)), 0);
  return b;
}

bool Vm::bigint_parts(const Value& v, bool* negative, StringView* digits) {
  Value neg, mag;
  if (!is_bigint(v) || !get_plain(v.as_object(), intern("__neg"), &neg) || !get_plain(v.as_object(), intern("__mag"), &mag) || !mag.is_string()) return false;
  *negative = neg.is_bool() && neg.u.boolean;
  *digits = mag.as_string()->view();
  return true;
}

// An operator with a BigInt in it: the library in script (Stdlib.cpp) does the work.
bool Vm::bigint_operation(const char* op, const Value& a, const Value& b, Value* out) {
  Value fn;
  if (!get_named(Value::make_object(global_), "__bigint_op", &fn) || !is_callable(fn)) return throw_error(ErrorKind::TypeError, "BigInt is not available");
  const Value call_args[3] = {Value::make_string(intern(op)), a, b};
  return call(fn, Value(), call_args, 3, out);
}

// --- Proxy ---------------------------------------------------------------------------------------------------------------

JsObject* Vm::new_proxy(JsObject* target, const Value& handler) {
  JsObject* p = new_object(target->proto);
  p->okind = ObjKind::Proxy;
  p->target = target;
  p->bound_this = handler;
  return p;
}

bool Vm::proxy_parts(JsObject* proxy, const char* operation, JsObject** target, Value* handler) {
  if (!proxy->target) {
    String message(allocator_);
    message.append("Cannot perform '");
    message.append(operation);
    message.append("' on a proxy that has been revoked");
    return throw_error(ErrorKind::TypeError, message.view());
  }
  *target = proxy->target;
  *handler = proxy->bound_this;
  return true;
}

// The trap called `name` of the proxy (undefined: there is none, the target answers).
bool Vm::proxy_trap(JsObject* proxy, const char* name, JsObject** target, Value* handler, Value* trap) {
  if (!proxy_parts(proxy, name, target, handler)) return false;
  *trap = Value();
  if (!get(*handler, intern(name), trap)) return false;
  if (trap->is_nullish()) {
    *trap = Value();
    return true;
  }
  if (!is_callable(*trap)) {
    String message(allocator_);
    message.append("proxy trap '");
    message.append(name);
    message.append("' is not a function");
    return throw_error(ErrorKind::TypeError, message.view());
  }
  return true;
}

bool Vm::proxy_get(JsObject* proxy, JsString* key, const Value& receiver, Value* out) {
  JsObject* target;
  Value handler, trap;
  if (!proxy_trap(proxy, "get", &target, &handler, &trap)) return false;
  if (trap.is_undefined()) return get(Value::make_object(target), key, out);
  const Value call_args[3] = {Value::make_object(target), Value::make_string(key), receiver};
  return call(trap, handler, call_args, 3, out);
}

bool Vm::proxy_set(JsObject* proxy, JsString* key, const Value& value, const Value& receiver) {
  JsObject* target;
  Value handler, trap;
  if (!proxy_trap(proxy, "set", &target, &handler, &trap)) return false;
  if (trap.is_undefined()) return put(Value::make_object(target), key, value);
  const Value call_args[4] = {Value::make_object(target), Value::make_string(key), value, receiver};
  Value ignored;
  return call(trap, handler, call_args, 4, &ignored);
}

bool Vm::proxy_has(JsObject* proxy, JsString* key, bool* out) {
  JsObject* target;
  Value handler, trap;
  if (!proxy_trap(proxy, "has", &target, &handler, &trap)) return false;
  if (trap.is_undefined()) {
    *out = has_property(target, key);
    return true;
  }
  const Value call_args[2] = {Value::make_object(target), Value::make_string(key)};
  Value r;
  if (!call(trap, handler, call_args, 2, &r)) return false;
  *out = to_boolean(r);
  return true;
}

bool Vm::proxy_delete(JsObject* proxy, JsString* key, bool* out) {
  JsObject* target;
  Value handler, trap;
  if (!proxy_trap(proxy, "deleteProperty", &target, &handler, &trap)) return false;
  if (trap.is_undefined()) {
    *out = delete_property(target, key);
    return true;
  }
  const Value call_args[2] = {Value::make_object(target), Value::make_string(key)};
  Value r;
  if (!call(trap, handler, call_args, 2, &r)) return false;
  *out = to_boolean(r);
  return true;
}

JsObject* Vm::proxy_keys(JsObject* proxy, bool all) {
  JsObject* target;
  Value handler, trap;
  if (!proxy_trap(proxy, "ownKeys", &target, &handler, &trap)) return new_array();
  if (trap.is_undefined()) return all ? own_names(target) : own_keys(target);
  const Value call_args[1] = {Value::make_object(target)};
  Value r;
  JsObject* keys = new_array();
  if (!call(trap, handler, call_args, 1, &r) || !r.is_object()) return keys;
  for (uint32_t i = 0; i < r.as_object()->length; ++i) {
    JsString* name;
    if (!to_string(r.as_object()->items[i], &name)) return keys;
    if (all) {
      array_push(keys, Value::make_string(name));
      continue;
    }
    // for Object.keys only the enumerable ones count: those the target has (or that it does not know of at all)
    Value ignored;
    uint8_t flags = kDefaultFlags;
    if (!own_property(target, name, &ignored, &flags) || (flags & kEnumerable)) array_push(keys, Value::make_string(name));
  }
  return keys;
}

bool Vm::proxy_call(JsObject* proxy, const Value& this_value, const Value* args, uint32_t argc, Value* result, bool construct) {
  JsObject* target;
  Value handler, trap;
  if (!proxy_trap(proxy, construct ? "construct" : "apply", &target, &handler, &trap)) return false;
  if (trap.is_undefined()) return construct ? call_value(Value::make_object(target), Value(), args, argc, result, true) : call(Value::make_object(target), this_value, args, argc, result);
  JsObject* list = new_array(argc);
  for (uint32_t i = 0; i < argc; ++i) array_push(list, args[i]);
  if (construct) {
    const Value call_args[3] = {Value::make_object(target), Value::make_object(list), Value::make_object(proxy)};
    if (!call(trap, handler, call_args, 3, result)) return false;
    if (!result->is_object()) return throw_error(ErrorKind::TypeError, "proxy [[Construct]] must return an object");
    return true;
  }
  const Value call_args[3] = {Value::make_object(target), this_value, Value::make_object(list)};
  return call(trap, handler, call_args, 3, result);
}

void Vm::run_epilogue() {
  if (!epilogue_ || in_epilogue_ || halted_) {
    epilogue_pending_ = false;
    return;
  }
  epilogue_pending_ = false;
  in_epilogue_ = true;
  epilogue_(epilogue_user_);
  in_epilogue_ = false;
}

JsObject* Vm::new_instance(JsObject* proto) {
  if (construct_hook_) {
    if (JsObject* made = construct_hook_(construct_user_, proto)) return made;
  }
  return new_object(proto);
}

JsObject* Vm::new_array(uint32_t capacity) {
  JsObject* a = new_object(array_proto_);
  if (a == sentinel_) return a;
  a->okind = ObjKind::Array;
  if (capacity) ensure_items(a, capacity);
  return a;
}

JsObject* Vm::new_native(NativeFn fn, StringView name, uint32_t arity) {
  JsObject* f = new_object(function_proto_);
  if (f == sentinel_) return f;
  f->okind = ObjKind::Native;
  f->native = fn;
  f->position = arity;
  f->bound_this = Value::make_string(intern(name));
  return f;
}

JsObject* Vm::new_function(FunctionCode* code, Env* env, const Value& this_value) {
  JsObject* f = new_object(function_proto_);
  if (f == sentinel_) return f;
  f->okind = ObjKind::Function;
  f->code = code;
  f->env = env;
  f->bound_this = this_value;
  return f;
}

Env* Vm::new_env(uint32_t slots, Env* parent) {
  const uint64_t size = sizeof(Env) + sizeof(Value) * slots;
  void* mem = allocator_.allocate(size);
  if (!mem) {
    halt("out of memory");
    return nullptr;
  }
  Env* e = new (mem) Env();
  e->parent = parent;
  e->count = slots;
  Value* s = e->slots();
  for (uint32_t i = 0; i < slots; ++i) new (&s[i]) Value();
  link(e, size);
  return e;
}

JsObject* Vm::new_host_object(JsObject* proto, const HostClass* host, void* data) {
  JsObject* o = new_object(proto);
  if (o == sentinel_) return o;
  o->host = host;
  o->host_data = data;
  return o;
}

JsObject* Vm::new_error(ErrorKind kind, StringView message) {
  JsObject* e = new_object(error_protos_[static_cast<int>(kind)]);
  e->okind = ObjKind::Error;
  define(e, message_, Value::make_string(new_string(message)), kWritable);
  {   // error.stack: pages (and their error reporters) read it; there is no call stack to give, only the first line
    static const char* const kNames[5] = {"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError"};
    String stack(allocator_);
    stack.append(kNames[static_cast<int>(kind)]);
    if (!message.empty()) {
      stack.append(": ");
      stack.append(message);
    }
    stack.append("\n    at <anonymous>");
    define(e, intern("stack"), Value::make_string(new_string(stack.view())), kWritable);
  }
  return e;
}

// Runs `run` (which may run script) while `*v` is kept alive. The value goes on the value stack: the script may call natives that do the same, so a single
// shared slot would be overwritten by the inner call and the outer one would get back `undefined`.
template <class F>
void Vm::keeping(Value* v, F&& run) {
  if (sp_ >= kStackValues) {
    run();
    return;
  }
  const uint32_t at = sp_;
  stack_[sp_++] = *v;
  run();
  if (sp_ > at) {
    *v = stack_[at];
    sp_ = at;
  }
}

bool Vm::throw_error(ErrorKind kind, StringView message) {
  JsObject* e = new_error(kind, message);
  return throw_value(Value::make_object(e));
}

bool Vm::throw_value(const Value& value) {
  exception_ = value;
  has_exception_ = true;
  return false;
}

// --- Collector ---------------------------------------------------------------------------------------------------

void Vm::mark(Cell* cell) {
  if (!cell || cell->marked) return;
  cell->marked = true;
  if (cell->kind != CellKind::String) gray_.push_back(cell);
}

void Vm::mark_value(const Value& v) {
  if (v.tag == Tag::String || v.tag == Tag::Object) mark(v.u.cell);
}

void Vm::mark_object(JsObject* o) { mark(o); }

void Vm::trace() {
  while (gray_.count() > 0) {
    Cell* c = gray_[gray_.count() - 1];
    gray_.remove_at(gray_.count() - 1);
    if (c->kind == CellKind::Env) {
      Env* e = static_cast<Env*>(c);
      mark(e->parent);
      for (uint32_t i = 0; i < e->count; ++i) mark_value(e->slots()[i]);
    } else if (c->kind == CellKind::Object) {
      JsObject* o = static_cast<JsObject*>(c);
      mark(o->proto);
      for (uint32_t i = 0; i < o->prop_count; ++i) {
        mark(o->props[i].key);
        mark_value(o->props[i].value);
      }
      for (uint32_t i = 0; i < o->length && o->items; ++i) mark_value(o->items[i]);
      mark(o->env);
      mark(o->target);
      mark_value(o->bound_this);
      if (o->okind == ObjKind::Coroutine && o->host_data) {
        Coroutine* co = static_cast<Coroutine*>(o->host_data);
        mark(co->env);
        mark_value(co->this_value);
        mark(co->callee);
        mark(co->promise);
        for (uint64_t i = 0; i < co->stack.count(); ++i) mark_value(co->stack[i]);
        for (uint64_t i = 0; i < co->handlers.count(); ++i) mark(co->handlers[i].env);
      }
    }
  }
}

void Vm::sweep() {
  Cell** link_to = &cells_;
  uint64_t bytes = 0;
  while (*link_to) {
    Cell* c = *link_to;
    if (c->marked) {
      c->marked = false;
      *link_to = c;
      link_to = &c->next;
      switch (c->kind) {
        case CellKind::String: bytes += sizeof(JsString) + static_cast<JsString*>(c)->length + 1; break;
        case CellKind::Env: bytes += sizeof(Env) + sizeof(Value) * static_cast<Env*>(c)->count; break;
        case CellKind::Object: {
          JsObject* o = static_cast<JsObject*>(c);
          bytes += sizeof(JsObject) + sizeof(Prop) * o->prop_cap + sizeof(Value) * o->capacity + sizeof(uint32_t) * o->index_cap;
          break;
        }
      }
    } else {
      *link_to = c->next;
      free_cell(c);
    }
  }
  heap_bytes_ = bytes;
}

void Vm::collect() {
  gc_wanted_ = false;
  for (uint32_t i = 0; i < sp_; ++i) mark_value(stack_[i]);
  for (uint64_t i = 0; i < frames_.count(); ++i) {
    const Frame& f = frames_[i];
    mark(f.env);
    mark_value(f.this_value);
    mark(f.callee);
    mark_value(f.construct_this);
    mark(f.coroutine);
  }
  for (uint64_t i = 0; i < handlers_.count(); ++i) mark(handlers_[i].env);
  mark_value(exception_);
  mark_value(last_result_);
  mark_value(iter_return_);
  mark_value(await_value_);
  mark(promise_proto_);
  mark(micro_);
  mark(rejections_);
  for (JsObject* o : {global_, object_proto_, function_proto_, array_proto_, string_proto_, number_proto_, boolean_proto_, error_proto_, iterator_proto_, sentinel_}) mark(o);
  for (int i = 0; i < 5; ++i) mark(error_protos_[i]);
  for (JsString* s : {empty_, length_, prototype_, name_, message_, constructor_, to_string_, value_of_, undefined_text_, null_text_, true_text_, false_text_}) mark(s);
  for (uint64_t i = 0; i < codes_.count(); ++i) {
    FunctionCode* code = codes_[i];
    for (uint64_t k = 0; k < code->constants.count(); ++k) mark_value(code->constants[k]);
    mark(code->name);
  }
  trace();
  sweep();
  rebuild_intern_table();
  gc_threshold_ = heap_bytes_ * 2 > (4u << 20) ? heap_bytes_ * 2 : (4u << 20);
  if (gc_threshold_ > memory_limit_) gc_threshold_ = memory_limit_ - (memory_limit_ >> 3);
}

// --- Properties --------------------------------------------------------------------------------------------------

int64_t Vm::find_prop(JsObject* o, JsString* key) const {
  if (o->index) {
    const uint32_t mask = o->index_cap - 1;
    for (uint32_t i = key->hash & mask;; i = (i + 1) & mask) {
      const uint32_t slot = o->index[i];
      if (slot == 0) return -1;
      if (o->props[slot - 1].key == key) return static_cast<int64_t>(slot - 1);
    }
  }
  for (uint32_t i = 0; i < o->prop_count; ++i)
    if (o->props[i].key == key) return i;
  return -1;
}

void Vm::index_rebuild(JsObject* o) {
  allocator_.deallocate(o->index);
  o->index = nullptr;
  o->index_cap = 0;
  if (o->prop_count <= 8) return;
  uint32_t cap = 16;
  while (cap < o->prop_count * 2 + 2) cap *= 2;
  uint32_t* index = static_cast<uint32_t*>(allocator_.allocate(sizeof(uint32_t) * cap));
  if (!index) return;   // (lookups just stay linear)
  for (uint32_t i = 0; i < cap; ++i) index[i] = 0;
  for (uint32_t p = 0; p < o->prop_count; ++p) {
    uint32_t k = o->props[p].key->hash & (cap - 1);
    while (index[k]) k = (k + 1) & (cap - 1);
    index[k] = p + 1;
  }
  o->index = index;
  o->index_cap = cap;
  heap_bytes_ += sizeof(uint32_t) * cap;
}

void Vm::grow_props(JsObject* o) {
  const uint32_t cap = o->prop_cap ? o->prop_cap * 2 : 4;
  Prop* fresh = static_cast<Prop*>(allocator_.allocate(sizeof(Prop) * cap));
  if (!fresh) {
    halt("out of memory");
    return;
  }
  for (uint32_t i = 0; i < o->prop_count; ++i) fresh[i] = o->props[i];
  allocator_.deallocate(o->props);
  o->props = fresh;
  o->prop_cap = cap;
  heap_bytes_ += sizeof(Prop) * cap;
  if (heap_bytes_ > gc_threshold_) gc_wanted_ = true;
}

bool Vm::ensure_items(JsObject* o, uint32_t capacity) {
  if (capacity <= o->capacity) return true;
  uint32_t cap = o->capacity ? o->capacity : 4;
  while (cap < capacity) cap *= 2;
  if (cap > (32u << 20)) {
    halt("array too large");
    return false;
  }
  Value* fresh = static_cast<Value*>(allocator_.allocate(sizeof(Value) * cap));
  if (!fresh) {
    halt("out of memory");
    return false;
  }
  for (uint32_t i = 0; i < o->length; ++i) fresh[i] = o->items[i];
  for (uint32_t i = o->length; i < cap; ++i) new (&fresh[i]) Value();
  allocator_.deallocate(o->items);
  o->items = fresh;
  o->capacity = cap;
  heap_bytes_ += sizeof(Value) * cap;
  if (heap_bytes_ > gc_threshold_) gc_wanted_ = true;
  return true;
}

void Vm::define(JsObject* o, JsString* key, const Value& value, uint8_t flags) {
  if (o == sentinel_ || halted_) return;
  if (o->okind == ObjKind::Array) {
    uint32_t i;
    if (key == length_) {
      double d = value.is_number() ? value.u.number : 0;
      array_set_length(o, d >= 0 && d < 4294967295.0 ? static_cast<uint32_t>(d) : 0);
      return;
    }
    if (canonical_index(key->view(), &i)) {
      if (i > o->length + (1u << 20)) return;
      if (!ensure_items(o, i + 1)) return;
      for (uint32_t k = o->length; k < i; ++k) o->items[k] = Value();
      o->items[i] = value;
      if (i >= o->length) o->length = i + 1;
      return;
    }
  }
  const int64_t at = find_prop(o, key);
  if (at >= 0) {
    o->props[at].value = value;
    o->props[at].flags = flags;
    return;
  }
  if (o->prop_count == o->prop_cap) grow_props(o);
  if (o->prop_count == o->prop_cap) return;
  o->props[o->prop_count++] = Prop{key, value, flags};
  if (o->prop_count == 9 || (o->index && o->prop_count * 2 + 2 > o->index_cap)) index_rebuild(o);
  else if (o->index) {
    uint32_t k = key->hash & (o->index_cap - 1);
    while (o->index[k]) k = (k + 1) & (o->index_cap - 1);
    o->index[k] = o->prop_count;
  }
}

void Vm::define_accessor(JsObject* o, JsString* key, const Value& getter, const Value& setter, uint8_t flags) {
  if (o == sentinel_ || halted_) return;
  const int64_t at = find_prop(o, key);
  JsObject* pair = nullptr;
  if (at >= 0 && (o->props[at].flags & kAccessor) && o->props[at].value.is_object()) pair = o->props[at].value.as_object();
  if (!pair) {
    pair = new_array(2);
    array_push(pair, Value());
    array_push(pair, Value());
  }
  if (!getter.is_undefined()) pair->items[0] = getter;
  if (!setter.is_undefined()) pair->items[1] = setter;
  define(o, key, Value::make_object(pair), static_cast<uint8_t>((flags & kEnumerable) | kAccessor));
}

void Vm::define_native(JsObject* o, StringView name, NativeFn fn, uint32_t arity) {
  JsObject* f = new_native(fn, name, arity);
  define(o, intern(name), Value::make_object(f), kWritable);   // (not enumerable)
}

bool Vm::canonical_index(StringView s, uint32_t* index) {
  if (s.empty() || s.size() > 10) return false;
  if (s[0] == '0' && s.size() > 1) return false;
  uint64_t v = 0;
  for (uint64_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  if (v >= 4294967295ull) return false;
  *index = static_cast<uint32_t>(v);
  return true;
}

bool Vm::array_index(const Value& key, uint32_t* index) const {
  if (key.is_number()) {
    const double d = key.u.number;
    if (d >= 0 && d < 4294967295.0 && d == static_cast<double>(static_cast<uint32_t>(d))) {
      *index = static_cast<uint32_t>(d);
      return true;
    }
    return false;
  }
  if (key.is_string()) return canonical_index(key.as_string()->view(), index);
  return false;
}

bool Vm::array_push(JsObject* a, const Value& v) {
  if (a->okind != ObjKind::Array) return false;
  if (!ensure_items(a, a->length + 1)) return false;
  a->items[a->length++] = v;
  return true;
}

bool Vm::array_push_bound(JsObject* b, const Value& v) {
  if (!ensure_items(b, b->length + 1)) return false;
  b->items[b->length++] = v;
  return true;
}

bool Vm::array_set_length(JsObject* a, uint32_t length) {
  if (length > a->length) {
    if (length > a->length + (1u << 24)) return false;
    if (!ensure_items(a, length)) return false;
    for (uint32_t i = a->length; i < length; ++i) a->items[i] = Value();
  }
  a->length = length;
  return true;
}

bool Vm::get_own(JsObject* o, JsString* key, Value* out) {
  if (o->host && o->host->get && o->host->get(*this, o, key, out)) return true;
  if (o->okind == ObjKind::Array) {
    if (key == length_) {
      *out = Value::make_number(o->length);
      return true;
    }
    uint32_t i;
    if (canonical_index(key->view(), &i)) {
      if (i < o->length) {
        *out = o->items[i];
        return true;
      }
      return false;
    }
  }
  const int64_t at = find_prop(o, key);
  if (at >= 0) {
    if (o->props[at].flags & kAccessor) return accessor_value(o->props[at].value, out);
    *out = o->props[at].value;
    return true;
  }
  if (o->okind == ObjKind::Function || o->okind == ObjKind::Native || o->okind == ObjKind::Bound) {
    if (o->okind == ObjKind::Bound && o->target && (key == name_ || key == length_)) {   // bind(): "bound f", and the parameters left after the bound arguments
      Value inner;
      get_own(o->target, key, &inner);
      if (key == name_) {
        String text(allocator_);
        text.append("bound ");
        if (inner.is_string()) text.append(inner.as_string()->view());
        *out = Value::make_string(new_string(text.view()));
      } else {
        double n = inner.is_number() ? inner.u.number - o->length : 0;
        *out = Value::make_number(n < 0 ? 0 : n);
      }
      return true;
    }
    if (key == name_) {
      *out = o->okind == ObjKind::Function ? Value::make_string(o->code->name ? o->code->name : empty_) : o->okind == ObjKind::Native ? o->bound_this : Value::make_string(empty_);
      return true;
    }
    if (key == length_) {
      *out = Value::make_number(o->okind == ObjKind::Function ? o->code->param_count - (o->code->has_rest ? 1 : 0) : o->okind == ObjKind::Native ? o->position : 0);
      return true;
    }
    if (key == prototype_ && o->okind == ObjKind::Function && !o->code->is_arrow) {   // made when first looked at
      JsObject* proto = new_plain();
      if (o->code->is_generator) {   // the objects a generator function makes inherit from the generator prototype
        Value generator_proto;
        if (get_plain(global_, intern("__generator_proto"), &generator_proto) && generator_proto.is_object()) proto->proto = generator_proto.as_object();
      } else {
        define(proto, constructor_, Value::make_object(o), kWritable);
      }
      define(o, prototype_, Value::make_object(proto), kWritable);
      *out = Value::make_object(proto);
      return true;
    }
  }
  return false;
}

// The value of an accessor property: what its getter returns for the object being read (nothing is called when no read is going on: hasOwnProperty ...).
bool Vm::accessor_value(const Value& pair, Value* out) {
  *out = Value();
  if (!has_receiver_ || !pair.is_object() || pair.as_object()->length < 2) return true;
  const Value getter = pair.as_object()->items[0];
  if (!is_callable(getter)) return true;
  const Value receiver = receiver_;
  has_receiver_ = false;   // (what the getter reads is its own business)
  const bool ok = call(getter, receiver, nullptr, 0, out);
  has_receiver_ = true;
  receiver_ = receiver;
  if (!ok) getter_failed_ = true;
  return true;
}

bool Vm::call_setter(const Value& pair, const Value& receiver, const Value& value) {
  if (!pair.is_object() || pair.as_object()->length < 2) return true;
  const Value setter = pair.as_object()->items[1];
  if (!is_callable(setter)) return true;   // (a property with only a getter ignores a write, as in sloppy mode)
  Value ignored;
  const bool saved_has = has_receiver_;
  const Value saved = receiver_;
  has_receiver_ = false;
  const bool ok = call(setter, receiver, &value, 1, &ignored);
  has_receiver_ = saved_has;
  receiver_ = saved;
  return ok;
}

bool Vm::get_plain(JsObject* o, JsString* key, Value* out) {
  const int64_t at = find_prop(o, key);
  if (at < 0) return false;
  *out = o->props[at].value;
  return true;
}

bool Vm::has_property(JsObject* o, JsString* key) {
  Value ignored;
  for (JsObject* p = o; p; p = p->proto) {
    if (p->okind == ObjKind::Proxy) {
      bool found = false;
      return proxy_has(p, key, &found) && found;
    }
    if (get_own(p, key, &ignored)) return true;
  }
  return false;
}

bool Vm::get(const Value& base, JsString* key, Value* out) {
  JsObject* o = nullptr;
  switch (base.tag) {
    case Tag::Undefined:
    case Tag::Null: {
      String message(allocator_);
      message.append("Cannot read properties of ");
      message.append(base.is_null() ? "null" : "undefined");
      message.append(" (reading '");
      message.append(key->view());
      message.append("')");
      return throw_error(ErrorKind::TypeError, message.view());
    }
    case Tag::String: {
      JsString* s = base.as_string();
      if (key == length_) {
        *out = Value::make_number(string_length(s));
        return true;
      }
      uint32_t i;
      if (canonical_index(key->view(), &i)) {
        *out = i < string_length(s) ? Value::make_string(string_part(s, i, i + 1)) : Value();
        return true;
      }
      o = string_proto_;
      break;
    }
    case Tag::Number: o = number_proto_; break;
    case Tag::Boolean: o = boolean_proto_; break;
    case Tag::Object: o = base.as_object(); break;
  }
  if (o && o->okind == ObjKind::Proxy) return proxy_get(o, key, base, out);
  const Value saved_receiver = receiver_;
  const bool saved_has = has_receiver_;
  receiver_ = base;
  has_receiver_ = true;
  for (; o; o = o->proto) {
    if (get_own(o, key, out)) {
      receiver_ = saved_receiver;
      has_receiver_ = saved_has;
      if (getter_failed_) {
        getter_failed_ = false;
        return false;
      }
      return true;
    }
  }
  receiver_ = saved_receiver;
  has_receiver_ = saved_has;
  *out = Value();
  return true;
}

bool Vm::to_property_key(const Value& v, JsString** out) {
  if (v.is_string()) {
    JsString* s = v.as_string();
    *out = s->interned ? s : intern(s->view());
    return true;
  }
  if (v.is_number() && v.u.number >= 0 && v.u.number < 1e9 && v.u.number == static_cast<double>(static_cast<uint32_t>(v.u.number))) {
    char buffer[16];
    uint32_t n = static_cast<uint32_t>(v.u.number);
    int length = 0;
    if (n == 0) buffer[length++] = '0';
    while (n > 0) {
      buffer[length++] = static_cast<char>('0' + n % 10);
      n /= 10;
    }
    char text[16];
    for (int i = 0; i < length; ++i) text[i] = buffer[length - 1 - i];
    *out = intern(StringView(text, static_cast<uint64_t>(length)));
    return true;
  }
  JsString* s;
  if (!to_string(v, &s)) return false;
  *out = intern(s->view());
  return true;
}

bool Vm::get_member(const Value& base, const Value& key, Value* out) {
  uint32_t i;
  if (base.is_object() && array_index(key, &i)) {
    JsObject* o = base.as_object();
    if (o->okind == ObjKind::Array) {
      *out = i < o->length ? o->items[i] : Value();
      return true;
    }
    if (o->okind == ObjKind::TypedArray) return typed_array_get_value(*this, o, i, out);
  } else if (base.is_string() && key.is_number() && array_index(key, &i)) {
    JsString* s = base.as_string();
    *out = i < string_length(s) ? Value::make_string(string_part(s, i, i + 1)) : Value();
    return true;
  }
  if (base.is_nullish()) {
    JsString* k = nullptr;
    String message(allocator_);
    message.append("Cannot read properties of ");
    message.append(base.is_null() ? "null" : "undefined");
    if (to_property_key(key, &k)) {
      message.append(" (reading '");
      message.append(k->view());
      message.append("')");
    }
    return throw_error(ErrorKind::TypeError, message.view());
  }
  JsString* k;
  if (!to_property_key(key, &k)) return false;
  return get(base, k, out);
}

bool Vm::put_ex(const Value& base, JsString* key, const Value& value, bool strict) {
  if (base.is_nullish()) {
    String message(allocator_);
    message.append("Cannot set properties of ");
    message.append(base.is_null() ? "null" : "undefined");
    message.append(" (setting '");
    message.append(key->view());
    message.append("')");
    return throw_error(ErrorKind::TypeError, message.view());
  }
  // a write that does not happen: nothing in sloppy mode, a TypeError in strict mode
  auto refuse = [&](const char* before, const char* after) {
    if (!strict) return true;
    String message(allocator_);
    message.append(before);
    message.append(key->view());
    message.append(after);
    return throw_error(ErrorKind::TypeError, message.view());
  };
  if (!base.is_object()) return refuse("Cannot create property '", "' on a primitive value");
  JsObject* o = base.as_object();
  if (o->okind == ObjKind::Proxy) return proxy_set(o, key, value, base);
  if (o->host && o->host->set) {
    const HostClass::Set r = o->host->set(*this, o, key, value);
    if (r == HostClass::Set::Threw) return false;
    if (r == HostClass::Set::Handled) {
      if (epilogue_pending_) run_epilogue();
      return true;
    }
  }
  if (o->okind == ObjKind::Array && key == length_) {   // a length must be a whole number below 2^32
    double d = 0;
    if (!to_number(value, &d)) return false;
    if (!(d >= 0 && d < 4294967296.0 && d == static_cast<double>(static_cast<uint32_t>(d)))) return throw_error(ErrorKind::RangeError, "Invalid array length");
  }
  if (o->frozen) return refuse("Cannot assign to read only property '", "' of object");
  const int64_t at = find_prop(o, key);
  if (at >= 0) {
    if (o->props[at].flags & kAccessor) {
      const Value pair = o->props[at].value;
      if (strict && !(pair.is_object() && pair.as_object()->length >= 2 && is_callable(pair.as_object()->items[1]))) return refuse("Cannot set property ", " of #<Object> which has only a getter");
      return call_setter(pair, base, value);
    }
    if (o->props[at].flags & kWritable) o->props[at].value = value;
    else return refuse("Cannot assign to read only property '", "' of object");
    return true;
  }
  for (JsObject* p = o->proto; p; p = p->proto) {   // a setter further up the chain takes the write; a read-only property there stops it
    const int64_t up = find_prop(p, key);
    if (up < 0) continue;
    if (p->props[up].flags & kAccessor) {
      const Value pair = p->props[up].value;
      if (strict && !(pair.is_object() && pair.as_object()->length >= 2 && is_callable(pair.as_object()->items[1]))) return refuse("Cannot set property ", " of #<Object> which has only a getter");
      return call_setter(pair, base, value);
    }
    if (!(p->props[up].flags & kWritable) && !p->host) return refuse("Cannot assign to read only property '", "' of object");
    break;
  }
  if (o->non_extensible || o->sealed) return refuse("Cannot add property ", ", object is not extensible");
  define(o, key, value);
  return true;
}

bool Vm::put_member_ex(const Value& base, const Value& key, const Value& value, bool strict) {
  uint32_t i;
  if (base.is_object() && array_index(key, &i)) {
    JsObject* o = base.as_object();
    if (o->okind == ObjKind::TypedArray) return typed_array_put(*this, o, i, value);
    if (o->okind == ObjKind::Array && !o->frozen) {
      if (i < o->length) {
        o->items[i] = value;
        return true;
      }
      if (o->non_extensible || o->sealed) {
        JsString* k;
        if (!to_property_key(key, &k)) return false;
        return put_ex(base, k, value, strict);
      }
      if (i <= o->length + (1u << 20) && ensure_items(o, i + 1)) {
        for (uint32_t k = o->length; k < i; ++k) o->items[k] = Value();
        o->items[i] = value;
        o->length = i + 1;
      }
      return true;
    }
  }
  JsString* k;
  if (base.is_nullish()) {
    if (!to_property_key(key, &k)) return false;
    return put_ex(base, key.is_string() ? key.as_string() : k, value, strict);
  }
  if (!to_property_key(key, &k)) return false;
  return put_ex(base, k, value, strict);
}

void Vm::set_property_flags(JsObject* o, JsString* key, uint8_t bits, uint8_t mask) {
  const int64_t at = find_prop(o, key);
  if (at >= 0) o->props[at].flags = static_cast<uint8_t>((o->props[at].flags & ~mask) | (bits & mask));
}

bool Vm::delete_property(JsObject* o, JsString* key) {
  if (o->okind == ObjKind::Proxy) {
    bool deleted = false;
    return proxy_delete(o, key, &deleted) && deleted;
  }
  if (o->host && o->host->remove) {
    bool deleted = true;
    if (o->host->remove(*this, o, key, &deleted)) return deleted;
  }
  if (o->frozen || o->sealed) {
    Value ignored;
    uint8_t flags;
    return !own_property(o, key, &ignored, &flags);   // (deleting what is not there succeeds)
  }
  if (o->okind == ObjKind::Array) {
    uint32_t i;
    if (canonical_index(key->view(), &i)) {
      if (i < o->length) o->items[i] = Value();
      return true;
    }
  }
  const int64_t at = find_prop(o, key);
  if (at < 0) return true;
  if (o->props[at].flags & kNonConfigurable) return false;
  for (uint32_t i = static_cast<uint32_t>(at); i + 1 < o->prop_count; ++i) o->props[i] = o->props[i + 1];
  --o->prop_count;
  index_rebuild(o);
  return true;
}

JsObject* Vm::own_keys(JsObject* o) {
  if (o->okind == ObjKind::Proxy) return proxy_keys(o, false);
  JsObject* keys = new_array(o->prop_count + (o->okind == ObjKind::Array ? o->length : 0));
  if (o->okind == ObjKind::Array) {
    for (uint32_t i = 0; i < o->length; ++i) {
      char buffer[16];
      int length = 0;
      uint32_t n = i;
      if (n == 0) buffer[length++] = '0';
      while (n > 0) {
        buffer[length++] = static_cast<char>('0' + n % 10);
        n /= 10;
      }
      char text[16];
      for (int k = 0; k < length; ++k) text[k] = buffer[length - 1 - k];
      array_push(keys, Value::make_string(new_string(StringView(text, static_cast<uint64_t>(length)))));
    }
  }
  if (o->host && o->host->keys) o->host->keys(*this, o, keys);
  for (uint32_t i = 0; i < o->prop_count; ++i)
    if (o->props[i].flags & kEnumerable) array_push(keys, Value::make_string(o->props[i].key));
  return keys;
}

JsObject* Vm::own_names(JsObject* o) {
  if (o->okind == ObjKind::Proxy) return proxy_keys(o, true);
  JsObject* names = own_keys(o);
  if (o->okind == ObjKind::Array) array_push(names, Value::make_string(length_));
  for (uint32_t i = 0; i < o->prop_count; ++i)
    if (!(o->props[i].flags & kEnumerable)) array_push(names, Value::make_string(o->props[i].key));
  return names;
}

bool Vm::own_property(JsObject* o, JsString* key, Value* value, uint8_t* flags) {
  const int64_t at = find_prop(o, key);
  if (at >= 0) {
    *value = o->props[at].value;
    *flags = o->props[at].flags;
    return true;
  }
  const bool saved = has_receiver_;
  has_receiver_ = false;
  const bool found = get_own(o, key, value);
  has_receiver_ = saved;
  *flags = key == length_ ? kWritable : kDefaultFlags;
  return found;
}

// --- Conversions -------------------------------------------------------------------------------------------------

bool Vm::to_boolean(const Value& v) {
  switch (v.tag) {
    case Tag::Undefined:
    case Tag::Null: return false;
    case Tag::Boolean: return v.u.boolean;
    case Tag::Number: return v.u.number == v.u.number && v.u.number != 0;
    case Tag::String: return v.as_string()->length > 0;
    default: {
      if (is_bigint(v)) {   // 0n is false
        const JsObject* o = v.as_object();
        for (uint32_t i = 0; i < o->prop_count; ++i)
          if (o->props[i].key->view().equals("__mag") && o->props[i].value.is_string()) return !o->props[i].value.as_string()->view().equals("0");
      }
      return true;
    }
  }
}

bool Vm::to_primitive(const Value& v, bool prefer_string, Value* out) { return to_primitive_hint(v, prefer_string ? 1 : 2, out); }

bool Vm::to_primitive_hint(const Value& v, int hint, Value* out) {
  if (!v.is_object()) {
    *out = v;
    return true;
  }
  Value exotic;
  if (!get(v, intern("@@toPrimitive"), &exotic)) return false;
  if (is_callable(exotic)) {
    const Value hint_text = Value::make_string(new_string(hint == 0 ? "default" : hint == 1 ? "string" : "number"));
    Value result;
    if (!call(exotic, v, &hint_text, 1, &result)) return false;
    if (result.is_object()) return throw_error(ErrorKind::TypeError, "Cannot convert object to primitive value");
    *out = result;
    return true;
  }
  const bool prefer_string = hint == 1;
  JsString* order[2] = {prefer_string ? to_string_ : value_of_, prefer_string ? value_of_ : to_string_};
  for (JsString* name : order) {
    Value method;
    if (!get(v, name, &method)) return false;
    if (!is_callable(method)) continue;
    Value result;
    if (!call(method, v, nullptr, 0, &result)) return false;
    if (!result.is_object()) {
      *out = result;
      return true;
    }
  }
  return throw_error(ErrorKind::TypeError, "Cannot convert object to primitive value");
}

bool Vm::to_string(const Value& v, JsString** out) {
  switch (v.tag) {
    case Tag::Undefined: *out = undefined_text_; return true;
    case Tag::Null: *out = null_text_; return true;
    case Tag::Boolean: *out = v.u.boolean ? true_text_ : false_text_; return true;
    case Tag::String: *out = v.as_string(); return true;
    case Tag::Number: {
      String text(allocator_);
      number_to_string(v.u.number, &text);
      *out = new_string(text.view());
      return true;
    }
    default: {
      bool negative;
      StringView digits;
      if (is_bigint(v) && bigint_parts(v, &negative, &digits)) {   // String(10n)
        String text(allocator_);
        if (negative) text.push_back('-');
        text.append(digits);
        *out = new_string(text.view());
        return true;
      }
      Value prim;
      if (!to_primitive(v, true, &prim)) return false;
      return to_string(prim, out);
    }
  }
}

bool Vm::to_number(const Value& v, double* out) {
  switch (v.tag) {
    case Tag::Undefined: *out = __builtin_nan(""); return true;
    case Tag::Null: *out = 0; return true;
    case Tag::Boolean: *out = v.u.boolean ? 1 : 0; return true;
    case Tag::Number: *out = v.u.number; return true;
    case Tag::String: {
      double d;
      if (!string_to_number(v.as_string()->view(), &d)) d = __builtin_nan("");
      *out = d;
      return true;
    }
    default: {
      bool negative;
      StringView digits;
      if (is_bigint(v) && bigint_parts(v, &negative, &digits)) {   // Number(10n)
        double d = 0;
        string_to_number(digits, &d);
        *out = negative ? -d : d;
        return true;
      }
      Value prim;
      if (!to_primitive(v, false, &prim)) return false;
      return to_number(prim, out);
    }
  }
}

bool Vm::to_int32(const Value& v, int32_t* out) {
  double d;
  if (!to_number(v, &d)) return false;
  if (d != d || d > 1.7e308 || d < -1.7e308) {
    *out = 0;
    return true;
  }
  if (d >= -9.2e18 && d <= 9.2e18) {
    *out = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(d)));
    return true;
  }
  double m = d - 4294967296.0 * static_cast<double>(static_cast<int64_t>(d / 4294967296.0));
  *out = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(m)));
  return true;
}

StringView Vm::type_of(const Value& v) {
  switch (v.tag) {
    case Tag::Undefined: return "undefined";
    case Tag::Null: return "object";
    case Tag::Boolean: return "boolean";
    case Tag::Number: return "number";
    case Tag::String: return "string";
    default: return is_bigint(v) ? "bigint" : is_callable(v) ? "function" : "object";
  }
}

bool Vm::strict_equals(const Value& a, const Value& b) {
  if (a.tag != b.tag) return false;
  switch (a.tag) {
    case Tag::Undefined:
    case Tag::Null: return true;
    case Tag::Boolean: return a.u.boolean == b.u.boolean;
    case Tag::Number: return a.u.number == b.u.number;
    case Tag::String: return a.as_string() == b.as_string() || (a.as_string()->length == b.as_string()->length && a.as_string()->view().equals(b.as_string()->view()));
    default: {
      if (a.u.cell == b.u.cell) return true;
      bool na, nb;
      StringView da, db;
      return is_bigint(a) && is_bigint(b) && bigint_parts(a, &na, &da) && bigint_parts(b, &nb, &db) && na == nb && da.equals(db);   // (BigInts of the same value are equal)
    }
  }
}

bool Vm::loose_equals(const Value& a, const Value& b) {
  if (is_bigint(a) != is_bigint(b)) {
    Value r;
    if (!bigint_operation("==", a, b, &r)) return false;
    return to_boolean(r);
  }
  if (a.tag == b.tag) return strict_equals(a, b);
  if (a.is_nullish() && b.is_nullish()) return true;
  if (a.is_nullish() || b.is_nullish()) return false;
  if (a.is_object() && !b.is_object()) {
    Value p;
    if (!to_primitive_hint(a, 0, &p)) return false;
    return loose_equals(p, b);
  }
  if (b.is_object() && !a.is_object()) {
    Value p;
    if (!to_primitive_hint(b, 0, &p)) return false;
    return loose_equals(a, p);
  }
  double x, y;
  if (!to_number(a, &x) || !to_number(b, &y)) return false;
  return x == y;
}

void Vm::describe(const Value& v, String* out, uint32_t depth) {
  switch (v.tag) {
    case Tag::String:
      if (depth == 0) {
        out->append(v.as_string()->view());
      } else {
        out->push_back('\'');
        out->append(v.as_string()->view());
        out->push_back('\'');
      }
      return;
    case Tag::Number: {
      String text(allocator_);
      number_to_string(v.u.number, &text);
      out->append(text.view());
      return;
    }
    case Tag::Object: break;
    case Tag::Undefined: out->append("undefined"); return;
    case Tag::Null: out->append("null"); return;
    case Tag::Boolean: out->append(v.u.boolean ? "true" : "false"); return;
  }
  JsObject* o = v.as_object();
  if (o->okind == ObjKind::BigInt) {
    bool negative;
    StringView digits;
    if (bigint_parts(v, &negative, &digits)) {
      if (negative) out->push_back('-');
      out->append(digits);
      out->push_back('n');
      return;
    }
  }
  if (o->okind == ObjKind::TypedArray) {
    typed_array_describe(*this, o, out, depth);
    return;
  }
  if (is_callable(v)) {
    Value name;
    get_own(o, name_, &name);
    out->append("function ");
    if (name.is_string()) out->append(name.as_string()->view());
    out->append("() { [code] }");
    return;
  }
  if (o->okind == ObjKind::Error) {
    Value name, message;
    get(v, name_, &name);
    get(v, message_, &message);
    if (name.is_string()) out->append(name.as_string()->view());
    else out->append("Error");
    if (message.is_string() && message.as_string()->length) {
      out->append(": ");
      out->append(message.as_string()->view());
    }
    return;
  }
  if (depth > 2) {
    out->append(o->okind == ObjKind::Array ? "[Array]" : "[Object]");
    return;
  }
  if (o->okind == ObjKind::Array) {
    out->push_back('[');
    for (uint32_t i = 0; i < o->length && i < 100; ++i) {
      if (i) out->append(", ");
      describe(o->items[i], out, depth + 1);
    }
    if (o->length > 100) out->append(", ...");
    out->push_back(']');
    return;
  }
  out->push_back('{');
  bool first = true;
  for (uint32_t i = 0; i < o->prop_count && i < 50; ++i) {
    if (!(o->props[i].flags & kEnumerable)) continue;
    if (!first) out->append(", ");
    first = false;
    out->append(o->props[i].key->view());
    out->append(": ");
    if (o->props[i].flags & kAccessor) out->append("[Getter/Setter]");
    else describe(o->props[i].value, out, depth + 1);
  }
  out->push_back('}');
}

void Vm::take_exception(String* text) {
  text->clear();
  if (halted_ && !halt_reason_.empty()) {
    text->append("Error: ");
    text->append(halt_reason_.view());
  } else {
    describe(exception_, text, 0);
  }
#ifdef TCOS_ERROR_LINES
  fprintf(stderr, "[js error at line %u] %s\n", throw_line_, text->c_str());
#endif
  throw_line_ = 0;
  has_exception_ = false;
  exception_ = Value();
}

uint32_t Vm::random_next() {
  random_state_ ^= random_state_ << 13;
  random_state_ ^= random_state_ >> 7;
  random_state_ ^= random_state_ << 17;
  return static_cast<uint32_t>(random_state_ >> 16);
}

// --- Operators ---------------------------------------------------------------------------------------------------

namespace {

constexpr double kInfinity = __builtin_inf();

double js_pow(double a, double b) {
  if (b != b) return __builtin_nan("");
  if (b == 0) return 1;
  if ((a == 1 || a == -1) && (b == kInfinity || b == -kInfinity)) return __builtin_nan("");
  if (b == static_cast<double>(static_cast<int64_t>(b)) && b > -1100 && b < 1100) {
    int64_t n = static_cast<int64_t>(b);
    const bool negative = n < 0;
    if (negative) n = -n;
    double result = 1, base = a;
    while (n > 0) {
      if (n & 1) result *= base;
      base *= base;
      n >>= 1;
    }
    if (negative && (result == kInfinity || result == -kInfinity) && a != 0) {   // (2 ** -1074: the positive power overflows, the denormal result does not)
      int64_t m = -static_cast<int64_t>(b);
      result = 1;
      base = 1 / a;
      while (m > 0) {
        if (m & 1) result *= base;
        base *= base;
        m >>= 1;
      }
      return result;
    }
    return negative ? 1 / result : result;
  }
  if (a > 0 && a < kInfinity) return fmath::pow(a, b);
  if (a == 0) return b > 0 ? 0 : kInfinity;
  if (a == kInfinity) return b > 0 ? kInfinity : 0;
  return __builtin_nan("");
}

double js_mod(double a, double b) {
  if (a != a || b != b || b == 0 || a == kInfinity || a == -kInfinity) return __builtin_nan("");
  if (b == kInfinity || b == -kInfinity) return a;
  if (a == 0) return a;
  const double aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
  if (aa < 9e15 && bb < 9e15 && aa == static_cast<double>(static_cast<int64_t>(aa)) && bb == static_cast<double>(static_cast<int64_t>(bb))) {
    const int64_t r = static_cast<int64_t>(aa) % static_cast<int64_t>(bb);
    return a < 0 ? -static_cast<double>(r) : static_cast<double>(r);
  }
  const double q = aa / bb;
  const double whole = q < 9e15 ? static_cast<double>(static_cast<int64_t>(q)) : q;
  double r = aa - whole * bb;
  if (r < 0) r = 0;
  return a < 0 ? -r : r;
}

}  // namespace

bool Vm::compare(const Value& a, const Value& b, bool* less, bool* undefined) {
  Value pa, pb;
  if (!to_primitive(a, false, &pa) || !to_primitive(b, false, &pb)) return false;
  *undefined = false;
  if (pa.is_string() && pb.is_string()) {
    *less = pa.as_string()->view().compare(pb.as_string()->view()) < 0;
    return true;
  }
  double x, y;
  if (!to_number(pa, &x) || !to_number(pb, &y)) return false;
  if (x != x || y != y) {
    *undefined = true;
    *less = false;
    return true;
  }
  *less = x < y;
  return true;
}

bool Vm::instance_of(const Value& v, const Value& ctor, bool* out) {
  if (ctor.is_object()) {   // a [Symbol.hasInstance] method decides
    Value method;
    if (!get_named(ctor, "@@hasInstance", &method)) return false;
    if (is_callable(method)) {
      Value answer;
      if (!call_value(method, ctor, &v, 1, &answer, false)) return false;
      *out = to_boolean(answer);
      return true;
    }
  }
  if (!is_callable(ctor)) return throw_error(ErrorKind::TypeError, "Right-hand side of 'instanceof' is not callable");
  JsObject* c = ctor.as_object();
  while (c->okind == ObjKind::Bound && c->target) c = c->target;
  Value proto;
  if (!get(Value::make_object(c), prototype_, &proto)) return false;
  *out = false;
  if (!v.is_object() || !proto.is_object()) return true;
  for (JsObject* p = v.as_object()->proto; p; p = p->proto) {
    if (p == proto.as_object()) {
      *out = true;
      return true;
    }
  }
  return true;
}

bool Vm::has_in(const Value& key, const Value& object, bool* out) {
  if (!object.is_object()) return throw_error(ErrorKind::TypeError, "Cannot use 'in' operator to search for a key in a non-object");
  JsString* k;
  if (!to_property_key(key, &k)) return false;
  *out = has_property(object.as_object(), k);
  return true;
}

bool Vm::binary_op(Opc op, const Value& a, const Value& b, Value* out) {
  if (is_bigint(a) || is_bigint(b)) {
    const char* name = nullptr;
    switch (op) {
      case Opc::Add: name = "+"; break;
      case Opc::Sub: name = "-"; break;
      case Opc::Mul: name = "*"; break;
      case Opc::Div: name = "/"; break;
      case Opc::Mod: name = "%"; break;
      case Opc::Pow: name = "**"; break;
      case Opc::BitAnd: name = "&"; break;
      case Opc::BitOr: name = "|"; break;
      case Opc::BitXor: name = "^"; break;
      case Opc::Shl: name = "<<"; break;
      case Opc::Shr: name = ">>"; break;
      case Opc::UShr: name = ">>>"; break;
      case Opc::Lt: name = "<"; break;
      case Opc::Gt: name = ">"; break;
      case Opc::Le: name = "<="; break;
      case Opc::Ge: name = ">="; break;
      case Opc::Eq: name = "=="; break;
      case Opc::Ne: name = "!="; break;
      case Opc::StrictEq: name = "==="; break;
      case Opc::StrictNe: name = "!=="; break;
      default: break;
    }
    if (name) return bigint_operation(name, a, b, out);
  }
  if (a.is_number() && b.is_number()) {
    const double x = a.u.number, y = b.u.number;
    switch (op) {
      case Opc::Add: *out = Value::make_number(x + y); return true;
      case Opc::Sub: *out = Value::make_number(x - y); return true;
      case Opc::Mul: *out = Value::make_number(x * y); return true;
      case Opc::Div: *out = Value::make_number(x / y); return true;
      case Opc::Lt: *out = Value::make_bool(x < y); return true;
      case Opc::Gt: *out = Value::make_bool(x > y); return true;
      case Opc::Le: *out = Value::make_bool(x <= y); return true;
      case Opc::Ge: *out = Value::make_bool(x >= y); return true;
      default: break;
    }
  }
  switch (op) {
    case Opc::Add: {
      Value pa, pb;
      if (!to_primitive_hint(a, 0, &pa) || !to_primitive_hint(b, 0, &pb)) return false;
      if (pa.is_string() || pb.is_string()) {
        JsString *sa, *sb;
        if (!to_string(pa, &sa) || !to_string(pb, &sb)) return false;
        *out = Value::make_string(concat(sa, sb));
        return true;
      }
      double x, y;
      if (!to_number(pa, &x) || !to_number(pb, &y)) return false;
      *out = Value::make_number(x + y);
      return true;
    }
    case Opc::Sub:
    case Opc::Mul:
    case Opc::Div:
    case Opc::Mod:
    case Opc::Pow: {
      double x, y;
      if (!to_number(a, &x) || !to_number(b, &y)) return false;
      double r = op == Opc::Sub ? x - y : op == Opc::Mul ? x * y : op == Opc::Div ? x / y : op == Opc::Mod ? js_mod(x, y) : js_pow(x, y);
      *out = Value::make_number(r);
      return true;
    }
    case Opc::BitAnd:
    case Opc::BitOr:
    case Opc::BitXor:
    case Opc::Shl:
    case Opc::Shr:
    case Opc::UShr: {
      int32_t x, y;
      if (!to_int32(a, &x) || !to_int32(b, &y)) return false;
      const uint32_t shift = static_cast<uint32_t>(y) & 31;
      double r;
      switch (op) {
        case Opc::BitAnd: r = x & y; break;
        case Opc::BitOr: r = x | y; break;
        case Opc::BitXor: r = x ^ y; break;
        case Opc::Shl: r = static_cast<int32_t>(static_cast<uint32_t>(x) << shift); break;
        case Opc::Shr: r = x >> shift; break;
        default: r = static_cast<double>(static_cast<uint32_t>(x) >> shift); break;
      }
      *out = Value::make_number(r);
      return true;
    }
    case Opc::Eq: *out = Value::make_bool(loose_equals(a, b)); return !has_exception_;
    case Opc::Ne: *out = Value::make_bool(!loose_equals(a, b)); return !has_exception_;
    case Opc::StrictEq: *out = Value::make_bool(strict_equals(a, b)); return true;
    case Opc::StrictNe: *out = Value::make_bool(!strict_equals(a, b)); return true;
    case Opc::Lt:
    case Opc::Gt:
    case Opc::Le:
    case Opc::Ge: {
      bool less, undefined;
      if (op == Opc::Lt || op == Opc::Ge) {
        if (!compare(a, b, &less, &undefined)) return false;
        *out = Value::make_bool(op == Opc::Lt ? less : (!undefined && !less));
      } else {
        if (!compare(b, a, &less, &undefined)) return false;
        *out = Value::make_bool(op == Opc::Gt ? less : (!undefined && !less));
      }
      return true;
    }
    case Opc::In: {
      bool r;
      if (!has_in(a, b, &r)) return false;
      *out = Value::make_bool(r);
      return true;
    }
    case Opc::InstanceOf: {
      bool r;
      if (!instance_of(a, b, &r)) return false;
      *out = Value::make_bool(r);
      return true;
    }
    default: return throw_error(ErrorKind::Error, "bad operator");
  }
}

// --- Iteration ---------------------------------------------------------------------------------------------------

bool Vm::make_iterator(const Value& v, bool for_in, Value* out) {
  JsObject* it = new_object(iterator_proto_);
  it->okind = ObjKind::Iterator;
  it->bound_this = v;
  it->position = 0;
  if (for_in) {
    if (v.is_object()) {
      JsObject* o = v.as_object();
      JsObject* keys = own_keys(o);
      for (JsObject* p = o->proto; p; p = p->proto) {   // inherited enumerable properties follow
        JsObject* inherited = own_keys(p);
        for (uint32_t i = 0; i < inherited->length; ++i) {
          bool seen = false;
          for (uint32_t k = 0; k < keys->length && !seen; ++k) seen = strict_equals(keys->items[k], inherited->items[i]);
          if (!seen) array_push(keys, inherited->items[i]);
        }
      }
      it->target = keys;   // (the list of keys to visit; kept alive through the iterator)
    } else if (v.is_string()) {
      JsObject* keys = new_array(string_length(v.as_string()));
      for (uint32_t i = 0; i < string_length(v.as_string()); ++i) {
        String n(allocator_);
        n.append_uint(i);
        array_push(keys, Value::make_string(new_string(n.view())));
      }
      it->target = keys;
    }
    it->proto = iterator_proto_;
    *out = Value::make_object(it);
    return true;
  }
  if (v.is_object() && v.as_object()->okind != ObjKind::Array) {
    // an object with an [Symbol.iterator]() method (the key is "@@iterator"): arrays and strings it returns are walked directly, anything else is an
    // iterator whose next() is called for each value
    Value method;
    if (!get(v, intern("@@iterator"), &method)) return false;
    if (is_callable(method)) {
      Value made;
      if (!call_value(method, v, nullptr, 0, &made, false)) return false;
      if (made.is_string() || (made.is_object() && made.as_object()->okind == ObjKind::Array)) {
        it->bound_this = made;
      } else if (made.is_object()) {
        it->bound_this = made;
        it->frozen = true;   // (an iterator object whose next() is called)
      } else {
        return throw_error(ErrorKind::TypeError, "Result of the Symbol.iterator method is not an object");
      }
      *out = Value::make_object(it);
      return true;
    }
  }
  if (!(it->bound_this.is_string() || (it->bound_this.is_object() && it->bound_this.as_object()->okind == ObjKind::Array))) {
    String message(allocator_);
    describe(v, &message, 1);
    message.append(" is not iterable");
    return throw_error(ErrorKind::TypeError, message.view());
  }
  it->items = nullptr;
  it->length = 0;
  *out = Value::make_object(it);
  return true;
}

bool Vm::iterator_next(JsObject* it, Value* out) {
  const Value& src = it->bound_this;
  iter_return_ = Value();
  if (it->frozen) {   // an iterator object: call its next()
    Value next, step, done;
    if (!get_named(src, "next", &next)) return false;
    if (!call_value(next, src, nullptr, 0, &step, false)) return false;
    if (!step.is_object()) {
      throw_error(ErrorKind::TypeError, "Iterator result is not an object");
      return false;
    }
    if (!get_named(step, "done", &done)) return false;
    if (to_boolean(done)) {
      iter_return_ = Value();
      if (!get_named(step, "value", &iter_return_)) return false;
      return false;
    }
    return get_named(step, "value", out);
  }
  if (it->target) {   // for-in: a list of keys
    JsObject* keys = it->target;
    if (it->position < keys->length) {
      *out = keys->items[it->position++];
      return true;
    }
    return false;
  }
  if (src.is_string()) {
    JsString* s = src.as_string();
    const uint32_t length = string_length(s);
    if (it->position < length) {   // (a character at a time: a surrogate pair is one)
      uint32_t step = 1;
      if (string_kind(s) == 2) {
        const uint16_t* w = string_wide(s);
        if (w && w[it->position] >= 0xD800 && w[it->position] <= 0xDBFF && it->position + 1 < length && w[it->position + 1] >= 0xDC00 && w[it->position + 1] <= 0xDFFF) step = 2;
      }
      *out = Value::make_string(string_part(s, it->position, it->position + step));
      it->position += step;
      return true;
    }
    return false;
  }
  if (src.is_object() && src.as_object()->okind == ObjKind::Array) {
    JsObject* a = src.as_object();
    if (it->position < a->length) {
      *out = a->items[it->position++];
      return true;
    }
  }
  return false;
}

bool Vm::throw_uninitialized(FunctionCode* code, uint16_t mark) {
  String message(allocator_);
  const uint32_t name = mark - kChecked;
  if (name < code->constants.count() && code->constants[name].is_string()) {
    JsString* s = code->constants[name].as_string();
    message.append("Cannot access '");
    message.append(StringView(s->data(), s->length));
    message.append("' before initialization");
  } else {
    message.append("Cannot access a variable before initialization");
  }
  return throw_error(ErrorKind::ReferenceError, message.view());
}

bool Vm::iterator_close(JsObject* it, bool silent) {
  if (!it->frozen) return true;   // (arrays, strings and key lists have nothing to close)
  const Value src = it->bound_this;
  Value method, result;
  bool ok = get_named(src, "return", &method);
  if (ok && is_callable(method)) {
    ok = call_value(method, src, nullptr, 0, &result, false);
    if (ok && !silent && !result.is_object()) ok = throw_error(ErrorKind::TypeError, "Iterator result is not an object");
  }
  if (!ok && silent) {
    has_exception_ = false;
    exception_ = Value();
    return true;
  }
  return ok;
}

bool Vm::iterate_into(JsObject* array, const Value& iterable, int64_t max) {
  if (max == 0) return true;
  Value it_value;
  if (!make_iterator(iterable, false, &it_value)) return false;
  JsObject* it = it_value.as_object();
  Value item;
  int64_t taken = 0;
  while (iterator_next(it, &item)) {
    if (!array_push(array, item)) break;
    if (max > 0 && ++taken >= max) break;
  }
  return !has_exception_;
}

bool Vm::spread_into(JsObject* array, const Value& iterable) {
  Value it_value;
  if (!make_iterator(iterable, false, &it_value)) return false;
  JsObject* it = it_value.as_object();
  Value item;
  while (iterator_next(it, &item)) {
    if (!array_push(array, item)) break;
  }
  return !has_exception_;
}

// --- Calls and the interpreter -----------------------------------------------------------------------------------

bool Vm::push(const Value& v) {
  if (sp_ >= kStackValues) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  stack_[sp_++] = v;
  return true;
}

// Is there a global called `name`? (Without calling a getter that defines it: get_own() gives undefined for an accessor when no receiver is set.)
bool Vm::has_global(JsString* name) {
  const bool saved_has = has_receiver_;
  has_receiver_ = false;
  bool found = false;
  Value ignored;
  for (JsObject* o = global_; o && !found; o = o->proto) found = get_own(o, name, &ignored);
  has_receiver_ = saved_has;
  return found;
}

bool Vm::lookup_global(JsString* name, Value* out) {
  if (has_global(name)) return get(Value::make_object(global_), name, out);   // (read the way `window.name` is: a getter on the window is called)
  String message(allocator_);
  message.append(name->view());
  message.append(" is not defined");
  return throw_error(ErrorKind::ReferenceError, message.view());
}

bool Vm::push_frame(JsObject* fn, const Value& this_value, const Value* args, uint32_t argc, uint32_t base, bool construct, const Value& construct_this) {
  if (frames_.count() >= kMaxFrames) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  FunctionCode* code = fn->code;
  Env* env = new_env(code->slot_count, fn->env);
  if (!env) return throw_error(ErrorKind::RangeError, "out of memory");
  Value* slots = env->slots();
  for (uint32_t i = 0; i < code->param_count; ++i) {
    if (code->has_rest && i + 1 == code->param_count) {
      JsObject* rest = new_array(argc > i ? argc - i : 0);
      for (uint32_t k = i; k < argc; ++k) array_push(rest, args[k]);
      slots[i] = Value::make_object(rest);
    } else {
      slots[i] = i < argc ? args[i] : Value();
    }
  }
  if (code->arguments_slot >= 0) {
    JsObject* list = new_array(argc);
    for (uint32_t k = 0; k < argc; ++k) array_push(list, args[k]);
    list->position = kArgumentsMark;   // (an array that stands for an arguments object: Object.prototype.toString says so)
    slots[code->arguments_slot] = Value::make_object(list);
  }
  if (code->self_slot >= 0) slots[code->self_slot] = Value::make_object(fn);
  Frame f;
  f.code = code;
  f.pc = 0;
  f.env = env;
  f.this_value = code->is_arrow ? fn->bound_this : (this_value.is_nullish() && !construct && !code->strict ? Value::make_object(global_) : this_value);   // (strict code keeps an undefined `this`)
  f.callee = fn;
  f.base = base;
  f.handlers = static_cast<uint32_t>(handlers_.count());
  f.construct = construct;
  f.via_super = super_pending_;
  super_pending_ = false;
  f.construct_this = construct_this;
  f.coroutine = nullptr;
  if (!frames_.push_back(f)) return throw_error(ErrorKind::RangeError, "out of memory");
  return true;
}

bool Vm::unwind(uint32_t entry_depth) {
#ifdef TCOS_ERROR_LINES
  if (throw_line_ == 0 && frames_.count() > 0) {
    const Frame& top = frames_[frames_.count() - 1];
    const uint32_t* line = top.pc > 0 ? top.code->lines.get(top.pc - 1) : nullptr;
    if (line) {
      throw_line_ = *line;
      for (uint64_t i = frames_.count(); i > 0; --i) {   // (a call stack, innermost first)
        const Frame& f = frames_[i - 1];
        const uint32_t* l = f.pc > 0 ? f.code->lines.get(f.pc - 1) : nullptr;
        fprintf(stderr, "    at %.*s (line %u)\n", f.code->name ? (int)f.code->name->length : 1, f.code->name ? f.code->name->data() : "?", l ? *l : 0);
      }
    }
  }
#endif
  while (handlers_.count() > 0 && handlers_[handlers_.count() - 1].frame >= entry_depth) {
    const Handler h = handlers_[handlers_.count() - 1];
    handlers_.remove_at(handlers_.count() - 1);
    while (frames_.count() > h.frame + 1) frames_.remove_at(frames_.count() - 1);
    Frame& f = frames_[h.frame];
    f.env = h.env;
    f.pc = h.pc;
    sp_ = h.stack;
    stack_[sp_++] = exception_;
    has_exception_ = false;
    exception_ = Value();
    throw_line_ = 0;
    return true;
  }
  if (frames_.count() > entry_depth) {
    sp_ = frames_[entry_depth].base;
    while (frames_.count() > entry_depth) frames_.remove_at(frames_.count() - 1);
  }
  return false;
}

bool Vm::call_value(const Value& function, const Value& this_value, const Value* args, uint32_t argc, Value* result, bool construct) {
  if (halted_) return false;
  Value fn = function;
  Value self = this_value;
  List<Value> bound_args(allocator_);
  uint32_t total = argc;
  const Value* all = args;
  // Bound functions: their function with their `this` and the bound arguments first.
  while (fn.is_object() && fn.as_object()->okind == ObjKind::Bound) {
    JsObject* b = fn.as_object();
    List<Value> merged(allocator_);
    for (uint32_t i = 0; i < b->length; ++i) merged.push_back(b->items[i]);
    for (uint32_t i = 0; i < total; ++i) merged.push_back(all[i]);
    bound_args.clear();
    for (uint64_t i = 0; i < merged.count(); ++i) bound_args.push_back(merged[i]);
    all = bound_args.count() ? &bound_args[0] : nullptr;
    total = static_cast<uint32_t>(bound_args.count());
    self = b->bound_this;
    fn = Value::make_object(b->target);
  }
  if (fn.is_object() && fn.as_object()->okind == ObjKind::Proxy && is_callable(fn)) {
    if (total == 0) return proxy_call(fn.as_object(), self, nullptr, 0, result, construct);
    return proxy_call(fn.as_object(), self, all, total, result, construct);
  }
  if (!is_callable(fn)) {
    String message(allocator_);
    describe(fn, &message, 1);
    message.append(construct ? " is not a constructor" : " is not a function");
    return throw_error(ErrorKind::TypeError, message.view());
  }
  JsObject* f = fn.as_object();
  const uint32_t base = sp_;
  // The function, `this` and arguments go on the stack: that is where the collector finds them.
  if (sp_ + total + 3 >= kStackValues) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  stack_[sp_++] = fn;
  stack_[sp_++] = self;
  for (uint32_t i = 0; i < total; ++i) stack_[sp_++] = all[i];
  const Value* a = &stack_[base + 2];
  Value this_for_call = self;
  Value created;
  if (construct) {
    Value proto;
    if (!get(fn, prototype_, &proto)) {
      sp_ = base;
      return false;
    }
    created = Value::make_object(new_instance(proto.is_object() ? proto.as_object() : object_proto_));
    stack_[base + 1] = created;
    this_for_call = created;
  }
  if (f->okind == ObjKind::Native) {
    if (native_depth_ > 150) {
      sp_ = base;
      return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
    }
    ++native_depth_;
    Value r;
    current_native_ = f;
    const bool ok = f->native(*this, this_for_call, a, total, &r);
    --native_depth_;
    sp_ = base;
    if (!ok) return false;
    if (epilogue_pending_) {
      keeping(&r, [this] { run_epilogue(); });   // (the result is kept while the epilogue runs script)
    }
    *result = construct && !r.is_object() ? created : r;
    return true;
  }
  if (construct && (f->code->is_arrow || f->code->is_async || f->code->is_generator)) {
    sp_ = base;
    return throw_error(ErrorKind::TypeError, "function is not a constructor");
  }
  if (f->code->is_async) {
    const bool started = start_async(f, this_for_call, a, total, base, result);
    sp_ = base;
    return started;
  }
  if (f->code->is_generator) {
    const bool started = start_generator(f, this_for_call, a, total, result);
    sp_ = base;
    return started;
  }
  const uint32_t entry = static_cast<uint32_t>(frames_.count());
  if (!push_frame(f, this_for_call, a, total, base, construct, created)) {
    sp_ = base;
    return false;
  }
  sp_ = base;
  const bool ok = execute(entry);
  if (!ok) return false;
  *result = last_result_;
  last_result_ = Value();
  return true;
}

bool Vm::call(const Value& function, const Value& this_value, const Value* args, uint32_t argc, Value* result) {
  const bool ok = call_value(function, this_value, args, argc, result, false);
  if (ok) {
    keeping(result, [this] { checkpoint(); });
  }
  return ok;
}
bool Vm::construct(const Value& function, const Value* args, uint32_t argc, Value* result) {
  const bool ok = call_value(function, Value(), args, argc, result, true);
  if (ok) {
    keeping(result, [this] { checkpoint(); });
  }
  return ok;
}

bool Vm::run(FunctionCode* code, Value* result) {
  if (!ok_ || halted_) return false;
  instructions_ = 0;
  JsObject* fn = new_function(code, nullptr, Value());
  Value r;
  const bool ok = call_value(Value::make_object(fn), Value::make_object(global_), nullptr, 0, &r, false);
  if (ok) {
    keeping(&r, [this] { checkpoint(); });
    *result = r;
  }
  return ok;
}

bool Vm::execute(uint32_t entry_depth) {
#define THROW_PENDING() \
  do { fp->pc = pc; if (!unwind(entry_depth)) return false; goto reload; } while (0)
#define CHECK(expr) do { if (!(expr)) THROW_PENDING(); } while (0)
#define PUSH(v) do { const Value pushed_ = (v); if (sp_ >= kStackValues) { throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded"); THROW_PENDING(); } stack_[sp_] = pushed_; ++sp_; } while (0)
  FunctionCode* code;
  const Instr* instrs;
  uint32_t pc;
  Frame* fp;
reload:
  fp = &frames_[frames_.count() - 1];
  code = fp->code;
  instrs = &code->code[0];
  pc = fp->pc;
  for (;;) {
    if (halted_) {
      has_exception_ = true;
      exception_ = Value();
      while (handlers_.count() > 0 && handlers_[handlers_.count() - 1].frame >= entry_depth) handlers_.remove_at(handlers_.count() - 1);
      if (frames_.count() > entry_depth) {
        sp_ = frames_[entry_depth].base;
        while (frames_.count() > entry_depth) frames_.remove_at(frames_.count() - 1);
      }
      return false;
    }
    if (budget_ && ++instructions_ > budget_) {
      halt("the script ran too long");
      continue;
    }
    if (gc_wanted_ && native_depth_ == 0 && gc_hold_ == 0) {
      fp->pc = pc;
      collect();
    }
    const Instr in = instrs[pc++];
    fp->pc = pc;
    switch (in.op) {
      case Opc::PushUndef: PUSH(Value()); break;
      case Opc::PushNull: PUSH(Value::make_null()); break;
      case Opc::PushTrue: PUSH(Value::make_bool(true)); break;
      case Opc::PushFalse: PUSH(Value::make_bool(false)); break;
      case Opc::PushNum: PUSH(code->constants[static_cast<uint32_t>(in.a)]); break;
      case Opc::PushStr: PUSH(code->constants[static_cast<uint32_t>(in.a)]); break;
      case Opc::PushInt: PUSH(Value::make_number(in.a)); break;
      case Opc::PushThis: PUSH(fp->this_value); break;
      case Opc::NewTarget: {
        Value target;
        if (fp->construct && fp->callee) {
          target = Value::make_object(fp->callee);
        } else if (fp->via_super && fp->this_value.is_object()) {   // (a parent constructor run by super(...): the class that `new` named)
          get(fp->this_value, intern("constructor"), &target);
          if (!is_callable(target)) target = Value();
        }
        PUSH(target);
        break;
      }
      case Opc::Pop: --sp_; break;
      case Opc::Dup: PUSH(stack_[sp_ - 1]); break;
      case Opc::Dup2: {
        const Value a = stack_[sp_ - 2], b = stack_[sp_ - 1];
        PUSH(a);
        PUSH(b);
        break;
      }
      case Opc::DupX1: {   // [a, b] -> [b, a, b]
        const Value a = stack_[sp_ - 2], b = stack_[sp_ - 1];
        stack_[sp_ - 2] = b;
        stack_[sp_ - 1] = a;
        PUSH(b);
        break;
      }
      case Opc::DupX2: {   // [a, b, c] -> [c, a, b, c]
        const Value a = stack_[sp_ - 3], b = stack_[sp_ - 2], c = stack_[sp_ - 1];
        stack_[sp_ - 3] = c;
        stack_[sp_ - 2] = a;
        stack_[sp_ - 1] = b;
        PUSH(c);
        break;
      }
      case Opc::Swap: {
        const Value t = stack_[sp_ - 1];
        stack_[sp_ - 1] = stack_[sp_ - 2];
        stack_[sp_ - 2] = t;
        break;
      }
      case Opc::GetVar: {
        Env* e = fp->env;
        for (uint8_t d = in.b; d > 0; --d) e = e->parent;
        if (in.unused >= kChecked && e->slots()[in.a].is_uninitialized()) {
          CHECK(throw_uninitialized(code, in.unused));
        }
        PUSH(e->slots()[in.a]);
        break;
      }
      case Opc::SetVar: {
        Env* e = fp->env;
        for (uint8_t d = in.b; d > 0; --d) e = e->parent;
        if (in.unused != 0) {
          if (in.unused == kConstAssign) {
            throw_error(ErrorKind::TypeError, "Assignment to constant variable.");
            THROW_PENDING();
          }
          if (e->slots()[in.a].is_uninitialized()) CHECK(throw_uninitialized(code, in.unused));
        }
        e->slots()[in.a] = stack_[sp_ - 1];
        break;
      }
      case Opc::TdzMark: {
        Env* e = fp->env;
        for (uint8_t d = in.b; d > 0; --d) e = e->parent;
        e->slots()[in.a] = Value::make_uninitialized();
        break;
      }
      case Opc::PushEnv: {
        Env* e = new_env(static_cast<uint32_t>(in.a), fp->env);
        if (!e) {
          throw_error(ErrorKind::RangeError, "out of memory");
          THROW_PENDING();
        }
        fp->env = e;
        break;
      }
      case Opc::IterReturn: PUSH(iter_return_); break;
      case Opc::PopEnv: fp->env = fp->env->parent; break;
      case Opc::CopyEnv: {
        Env* old = fp->env;
        Env* e = new_env(old->count, old->parent);
        if (!e) {
          throw_error(ErrorKind::RangeError, "out of memory");
          THROW_PENDING();
        }
        for (uint32_t i = 0; i < old->count; ++i) e->slots()[i] = old->slots()[i];
        fp->env = e;
        break;
      }
      case Opc::GetGlobal: {
        Value v;
        CHECK(lookup_global(code->constants[static_cast<uint32_t>(in.a)].as_string(), &v));
        PUSH(v);
        break;
      }
      case Opc::SetGlobal: {
        JsString* name = code->constants[static_cast<uint32_t>(in.a)].as_string();
        if (global_->host && global_->host->set) {
          const HostClass::Set r = global_->host->set(*this, global_, name, stack_[sp_ - 1]);
          if (r == HostClass::Set::Threw) THROW_PENDING();
          if (r == HostClass::Set::Handled) break;
        }
        if (code->strict && !has_global(name)) {   // (assigning to a name that was never declared)
          String message(allocator_);
          message.append(name->view());
          message.append(" is not defined");
          throw_error(ErrorKind::ReferenceError, message.view());
          THROW_PENDING();
        }
        define(global_, name, stack_[sp_ - 1], kDefaultFlags);
        break;
      }
      case Opc::TypeofGlobal: {
        JsString* name = code->constants[static_cast<uint32_t>(in.a)].as_string();
        Value v;
        const bool found = has_global(name);
        if (found) CHECK(get(Value::make_object(global_), name, &v));
        PUSH(Value::make_string(intern(found ? type_of(v) : StringView("undefined"))));
        break;
      }
      case Opc::DeclareGlobal: {
        JsString* name = code->constants[static_cast<uint32_t>(in.a)].as_string();
        Value ignored;
        if (!get_own(global_, name, &ignored)) define(global_, name, Value(), kDefaultFlags);
        break;
      }
      case Opc::GetProp: {
        Value v;
        const Value base = stack_[sp_ - 1];
        CHECK(get(base, code->constants[static_cast<uint32_t>(in.a)].as_string(), &v));
        stack_[sp_ - 1] = v;
        break;
      }
      case Opc::SetProp: {   // [obj, value] -> [value]
        const Value value = stack_[sp_ - 1], base = stack_[sp_ - 2];
        CHECK(put_ex(base, code->constants[static_cast<uint32_t>(in.a)].as_string(), value, code->strict));
        sp_ -= 2;
        stack_[sp_++] = value;
        break;
      }
      case Opc::GetElem: {   // [obj, key] -> [value]
        Value v;
        CHECK(get_member(stack_[sp_ - 2], stack_[sp_ - 1], &v));
        --sp_;
        stack_[sp_ - 1] = v;
        break;
      }
      case Opc::SetElem: {   // [obj, key, value] -> [value]
        const Value value = stack_[sp_ - 1], key = stack_[sp_ - 2], base = stack_[sp_ - 3];
        CHECK(put_member_ex(base, key, value, code->strict));
        sp_ -= 3;
        stack_[sp_++] = value;
        break;
      }
      case Opc::DeleteProp:
      case Opc::DeleteElem: {
        Value key;
        Value base;
        JsString* k;
        if (in.op == Opc::DeleteProp) {
          base = stack_[sp_ - 1];
          k = code->constants[static_cast<uint32_t>(in.a)].as_string();
          --sp_;
        } else {
          base = stack_[sp_ - 2];
          key = stack_[sp_ - 1];
          sp_ -= 2;
          CHECK(to_property_key(key, &k));
        }
        bool r = true;
        if (base.is_object()) r = delete_property(base.as_object(), k);
        if (!r && code->strict) {
          String message(allocator_);
          message.append("Cannot delete property '");
          message.append(k->view());
          message.append("' of object");
          throw_error(ErrorKind::TypeError, message.view());
          THROW_PENDING();
        }
        PUSH(Value::make_bool(r));
        break;
      }
      case Opc::GetMethod: {   // [obj] -> [fn, obj]
        Value fn;
        const Value base = stack_[sp_ - 1];
        CHECK(get(base, code->constants[static_cast<uint32_t>(in.a)].as_string(), &fn));
        stack_[sp_ - 1] = fn;
        PUSH(base);
        break;
      }
      case Opc::GetMethodElem: {   // [obj, key] -> [fn, obj]
        Value fn;
        const Value base = stack_[sp_ - 2];
        CHECK(get_member(base, stack_[sp_ - 1], &fn));
        stack_[sp_ - 2] = fn;
        stack_[sp_ - 1] = base;
        break;
      }
      case Opc::Call:
      case Opc::New:
      case Opc::CallSpread:
      case Opc::NewSpread: {
        const bool is_new = in.op == Opc::New || in.op == Opc::NewSpread;
        const bool spread = in.op == Opc::CallSpread || in.op == Opc::NewSpread;
        uint32_t argc = spread ? 0 : static_cast<uint32_t>(in.a);
        const uint32_t base = spread ? sp_ - 3 : sp_ - argc - 2;
        const Value callee = stack_[base];
        const Value self = stack_[base + 1];
        const Value* args = &stack_[base + 2];
        JsObject* arg_array = nullptr;
        if (spread) {
          arg_array = stack_[base + 2].as_object();
          args = arg_array->items;
          argc = arg_array->length;
        }
        if (callee.is_object() && callee.as_object()->okind == ObjKind::Function && !callee.as_object()->code->is_async && !callee.as_object()->code->is_generator && !(is_new && callee.as_object()->code->is_arrow)) {
          JsObject* f = callee.as_object();
          Value this_for_call = self, created;
          if (is_new) {
            Value proto;
            CHECK(get(callee, prototype_, &proto));
            created = Value::make_object(new_instance(proto.is_object() ? proto.as_object() : object_proto_));
            this_for_call = created;
            stack_[base + 1] = created;   // (kept reachable)
          }
          CHECK(push_frame(f, this_for_call, args, argc, base, is_new, created));
          sp_ = base;
          goto reload;
        }
        Value result;
        if (callee.is_object() && callee.as_object()->okind == ObjKind::Native) {
          Value this_for_call = self;
          Value created;
          if (is_new) {
            Value proto;
            CHECK(get(callee, prototype_, &proto));
            created = Value::make_object(new_instance(proto.is_object() ? proto.as_object() : object_proto_));
            this_for_call = created;
            stack_[base + 1] = created;
          }
          if (native_depth_ > 150) {
            throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
            THROW_PENDING();
          }
          ++native_depth_;
          current_native_ = callee.as_object();
          const bool ok = callee.as_object()->native(*this, this_for_call, args, argc, &result);
          --native_depth_;
          CHECK(ok);
          if (epilogue_pending_) {
            keeping(&result, [this] { run_epilogue(); });
          }
          if (is_new && !result.is_object()) result = created;
        } else {
          // bound functions and things that are not functions: the general path
          sp_ = base + 2 + (spread ? 1 : argc);
          CHECK(call_value(callee, self, args, argc, &result, is_new));
        }
        sp_ = base;
        PUSH(result);
        break;
      }
      case Opc::Add: case Opc::Sub: case Opc::Mul: case Opc::Div: case Opc::Mod: case Opc::Pow: case Opc::BitAnd: case Opc::BitOr: case Opc::BitXor:
      case Opc::Shl: case Opc::Shr: case Opc::UShr: case Opc::Eq: case Opc::Ne: case Opc::StrictEq: case Opc::StrictNe: case Opc::Lt: case Opc::Gt:
      case Opc::Le: case Opc::Ge: case Opc::In: case Opc::InstanceOf: {
        Value r;
        const Value a = stack_[sp_ - 2], b = stack_[sp_ - 1];
        if (!binary_op(in.op, a, b, &r)) {
          if (!has_exception_) throw_error(ErrorKind::Error, "operation failed");
          THROW_PENDING();
        }
        --sp_;
        stack_[sp_ - 1] = r;
        break;
      }
      case Opc::Not: stack_[sp_ - 1] = Value::make_bool(!to_boolean(stack_[sp_ - 1])); break;
      case Opc::Neg: {
        if (is_bigint(stack_[sp_ - 1])) {
          Value r;
          CHECK(bigint_operation("neg", stack_[sp_ - 1], Value(), &r));
          stack_[sp_ - 1] = r;
          break;
        }
        double d;
        CHECK(to_number(stack_[sp_ - 1], &d));
        stack_[sp_ - 1] = Value::make_number(-d);
        break;
      }
      case Opc::Plus:
      case Opc::ToNumber: {
        if (is_bigint(stack_[sp_ - 1])) {
          if (in.op == Opc::Plus) CHECK(throw_error(ErrorKind::TypeError, "Cannot convert a BigInt value to a number"));   // +10n
          break;   // (x++ keeps the BigInt for the old value; the update itself is done by Inc)
        }
        double d;
        CHECK(to_number(stack_[sp_ - 1], &d));
        stack_[sp_ - 1] = Value::make_number(d);
        break;
      }
      case Opc::BitNot: {
        if (is_bigint(stack_[sp_ - 1])) {
          Value r;
          CHECK(bigint_operation("~", stack_[sp_ - 1], Value(), &r));
          stack_[sp_ - 1] = r;
          break;
        }
        int32_t i;
        CHECK(to_int32(stack_[sp_ - 1], &i));
        stack_[sp_ - 1] = Value::make_number(~i);
        break;
      }
      case Opc::Inc:
      case Opc::Dec: {
        if (is_bigint(stack_[sp_ - 1])) {
          Value r;
          CHECK(bigint_operation(in.op == Opc::Inc ? "inc" : "dec", stack_[sp_ - 1], Value(), &r));
          stack_[sp_ - 1] = r;
          break;
        }
        double d;
        CHECK(to_number(stack_[sp_ - 1], &d));
        stack_[sp_ - 1] = Value::make_number(in.op == Opc::Inc ? d + 1 : d - 1);
        break;
      }
      case Opc::ToStr: {
        JsString* s;
        CHECK(to_string(stack_[sp_ - 1], &s));
        stack_[sp_ - 1] = Value::make_string(s);
        break;
      }
      case Opc::Typeof: stack_[sp_ - 1] = Value::make_string(intern(type_of(stack_[sp_ - 1]))); break;
      case Opc::Void: stack_[sp_ - 1] = Value(); break;
      case Opc::Jump: pc = static_cast<uint32_t>(in.a); break;
      case Opc::JumpIfFalse:
        if (!to_boolean(stack_[--sp_])) pc = static_cast<uint32_t>(in.a);
        break;
      case Opc::JumpIfTrue:
        if (to_boolean(stack_[--sp_])) pc = static_cast<uint32_t>(in.a);
        break;
      case Opc::CheckReturn:
        if (is_generator_return(stack_[sp_ - 1])) pc = static_cast<uint32_t>(in.a);
        break;
      case Opc::JumpIfFalseKeep:
        if (!to_boolean(stack_[sp_ - 1])) pc = static_cast<uint32_t>(in.a);
        else --sp_;
        break;
      case Opc::JumpIfTrueKeep:
        if (to_boolean(stack_[sp_ - 1])) pc = static_cast<uint32_t>(in.a);
        else --sp_;
        break;
      case Opc::JumpIfNotNullishKeep:
        if (!stack_[sp_ - 1].is_nullish()) pc = static_cast<uint32_t>(in.a);
        else --sp_;
        break;
      case Opc::Closure: {
        JsObject* f = new_function(code->children[static_cast<uint32_t>(in.a)], fp->env, fp->this_value);
        PUSH(Value::make_object(f));
        break;
      }
      case Opc::Return: {
        Value result = stack_[sp_ - 1];
        const Frame done = *fp;
        frames_.remove_at(frames_.count() - 1);
        while (handlers_.count() > done.handlers) handlers_.remove_at(handlers_.count() - 1);
        sp_ = done.base;
        if (done.construct && !result.is_object()) result = done.construct_this;
        if (frames_.count() <= entry_depth) {
          last_result_ = result;
          return true;
        }
        PUSH(result);
        goto reload;
      }
      case Opc::Throw:
        throw_value(stack_[sp_ - 1]);
        THROW_PENDING();
      case Opc::NewObject: PUSH(Value::make_object(new_plain())); break;
      case Opc::DefineProp: {   // [obj, value] -> [obj]
        define(stack_[sp_ - 2].as_object(), code->constants[static_cast<uint32_t>(in.a)].as_string(), stack_[sp_ - 1]);
        --sp_;
        break;
      }
      case Opc::DefineElem: {   // [obj, key, value] -> [obj]
        JsString* k;
        CHECK(to_property_key(stack_[sp_ - 2], &k));
        define(stack_[sp_ - 3].as_object(), k, stack_[sp_ - 1]);
        sp_ -= 2;
        break;
      }
      case Opc::DefineAccessor: {   // [obj, fn] -> [obj]   (b: 1 getter, 2 setter)
        JsString* k = code->constants[static_cast<uint32_t>(in.a)].as_string();
        define_accessor(stack_[sp_ - 2].as_object(), k, in.b == 1 ? stack_[sp_ - 1] : Value(), in.b == 2 ? stack_[sp_ - 1] : Value());
        --sp_;
        break;
      }
      case Opc::DefineAccessorElem: {   // [obj, key, fn] -> [obj]
        JsString* k;
        CHECK(to_property_key(stack_[sp_ - 2], &k));
        define_accessor(stack_[sp_ - 3].as_object(), k, in.b == 1 ? stack_[sp_ - 1] : Value(), in.b == 2 ? stack_[sp_ - 1] : Value());
        sp_ -= 2;
        break;
      }
      case Opc::NewArray: PUSH(Value::make_object(new_array())); break;
      case Opc::ArrayPush:
        array_push(stack_[sp_ - 2].as_object(), stack_[sp_ - 1]);
        --sp_;
        break;
      case Opc::ArraySpread:
        CHECK(spread_into(stack_[sp_ - 2].as_object(), stack_[sp_ - 1]));
        --sp_;
        break;
      case Opc::ObjectSpread: {
        const Value src = stack_[sp_ - 1];
        JsObject* target = stack_[sp_ - 2].as_object();
        if (src.is_object()) {
          JsObject* keys = own_keys(src.as_object());
          for (uint32_t i = 0; i < keys->length; ++i) {
            Value v;
            CHECK(get(src, keys->items[i].as_string(), &v));
            define(target, keys->items[i].as_string(), v);
          }
        }
        --sp_;
        break;
      }
      case Opc::ForInStart:
      case Opc::ForOfStart: {
        Value it;
        CHECK(make_iterator(stack_[sp_ - 1], in.op == Opc::ForInStart, &it));
        stack_[sp_ - 1] = it;
        break;
      }
      case Opc::IterNext: {
        JsObject* it = stack_[sp_ - 1].as_object();
        Value v;
        if (iterator_next(it, &v)) stack_[sp_ - 1] = v;
        else {
          if (has_exception_) THROW_PENDING();
          --sp_;
          pc = static_cast<uint32_t>(in.a);
        }
        break;
      }
      case Opc::IterClose: {
        JsObject* it = stack_[sp_ - 1].as_object();
        --sp_;
        CHECK(iterator_close(it, in.b != 0));
        break;
      }
      case Opc::TryBegin: {
        Handler h{static_cast<uint32_t>(in.a), sp_, static_cast<uint32_t>(frames_.count() - 1), fp->env};
        handlers_.push_back(h);
        break;
      }
      case Opc::TryEnd:
        if (handlers_.count() > 0) handlers_.remove_at(handlers_.count() - 1);
        break;
      case Opc::Await:
      case Opc::Yield: {
        // The async function stops here: its frame, the values it has on the stack and its handlers are saved in its coroutine, and the VM goes back to
        // whoever started or resumed it (run_coroutine), which waits for the value to settle.
        Coroutine* saved = fp->coroutine ? coroutine_of(fp->coroutine) : nullptr;
        if (!saved || frames_.count() != entry_depth + 1) {
          throw_error(ErrorKind::SyntaxError, "await is only valid in async functions");
          THROW_PENDING();
        }
        const Value awaited = stack_[sp_ - 1];
        --sp_;
        saved->code = fp->code;
        saved->pc = pc;
        saved->env = fp->env;
        saved->this_value = fp->this_value;
        saved->callee = fp->callee;
        saved->stack.clear();
        saved->handlers.clear();
        bool fits = true;
        for (uint32_t i = fp->base; i < sp_ && fits; ++i) fits = saved->stack.push_back(stack_[i]);
        for (uint64_t i = fp->handlers; i < handlers_.count() && fits; ++i) fits = saved->handlers.push_back(Handler{handlers_[i].pc, handlers_[i].stack - fp->base, 0, handlers_[i].env});
        if (!fits) {
          halt("out of memory");
          continue;
        }
        while (handlers_.count() > fp->handlers) handlers_.remove_at(handlers_.count() - 1);
        sp_ = fp->base;
        frames_.remove_at(frames_.count() - 1);
        await_value_ = awaited;
        suspended_ = true;
        return true;
      }
    }
  }
#undef THROW_PENDING
#undef CHECK
#undef PUSH
}

// --- Promises, microtasks and async functions ---------------------------------------------------------------------------

namespace {

// The functions a promise is resolved or rejected with (`resolve` and `reject` of the executor). Their data: [promise, shared flag object].
bool resolving_native(Vm& vm, const Value&, const Value* args, uint32_t argc, Value* result, bool reject) {
  JsObject* self = vm.current_native();
  JsObject* promise = self->items[0].as_object();
  JsObject* shared = self->items[1].as_object();
  *result = Value();
  if (shared->position) return true;
  shared->position = 1;
  const Value v = argc > 0 ? args[0] : Value();
  if (reject) vm.promise_reject(promise, v);
  else vm.promise_resolve(promise, v);
  return true;
}
bool resolve_function(Vm& vm, const Value& t, const Value* args, uint32_t argc, Value* result) { return resolving_native(vm, t, args, argc, result, false); }
bool reject_function(Vm& vm, const Value& t, const Value* args, uint32_t argc, Value* result) { return resolving_native(vm, t, args, argc, result, true); }

}  // namespace

JsObject* Vm::new_native_with(NativeFn fn, StringView name, uint32_t arity, const Value* data, uint32_t count) {
  JsObject* f = new_native(fn, name, arity);
  if (f == sentinel_) return f;
  if (count && ensure_items(f, count)) {
    for (uint32_t i = 0; i < count; ++i) f->items[i] = data[i];
    f->length = count;
  }
  return f;
}

JsObject* Vm::new_promise() {
  JsObject* p = new_object(promise_proto_);
  p->okind = ObjKind::Promise;
  return p;
}

JsObject* Vm::resolving_function(JsObject* promise, JsObject* shared, bool reject) {
  const Value data[2] = {Value::make_object(promise), Value::make_object(shared)};
  return new_native_with(reject ? reject_function : resolve_function, "", 1, data, 2);
}

void Vm::promise_settle(JsObject* promise, uint32_t state, const Value& value) {
  if (promise_state(promise) != kPromisePending) return;
  promise->position = (promise->position & ~kPromiseState) | state;
  promise->bound_this = value;
  // the reactions that were waiting
  const uint32_t count = promise->length;
  Value* items = promise->items;
  List<Value> copy(allocator_);
  for (uint32_t i = 0; i < count; ++i) copy.push_back(items[i]);
  promise->length = 0;
  for (uint32_t i = 0; i + 2 < count; i += 3) {
    enqueue_job(state == kPromiseFulfilled ? copy[i] : copy[i + 1], value, copy[i + 2], state == kPromiseFulfilled ? 0 : 1);
  }
  if (state == kPromiseRejected && !(promise->position & kPromiseHandled) && rejections_) array_push(rejections_, Value::make_object(promise));
}

void Vm::promise_fulfill(JsObject* promise, const Value& value) { promise_settle(promise, kPromiseFulfilled, value); }
void Vm::promise_reject(JsObject* promise, const Value& reason) { promise_settle(promise, kPromiseRejected, reason); }

void Vm::promise_resolve(JsObject* promise, const Value& value) {
  if (promise_state(promise) != kPromisePending) return;
  if (value.is_object() && value.as_object() == promise) {
    promise_reject(promise, Value::make_object(new_error(ErrorKind::TypeError, "Chaining cycle detected for promise")));
    return;
  }
  if (!value.is_object()) {
    promise_fulfill(promise, value);
    return;
  }
  Value then;
  if (!get_named(value, "then", &then)) {
    const Value reason = exception_;
    has_exception_ = false;
    exception_ = Value();
    promise_reject(promise, reason);
    return;
  }
  if (!is_callable(then)) {
    promise_fulfill(promise, value);
    return;
  }
  enqueue_job(then, value, Value::make_object(promise), 3);
}

void Vm::promise_then(JsObject* promise, const Value& on_fulfilled, const Value& on_rejected, JsObject* derived) {
  const Value derived_value = derived ? Value::make_object(derived) : Value();
  const uint32_t state = promise_state(promise);
  promise->position |= kPromiseHandled;
  if (state == kPromisePending) {
    if (!ensure_items(promise, promise->length + 3)) return;
    promise->items[promise->length++] = on_fulfilled;
    promise->items[promise->length++] = on_rejected;
    promise->items[promise->length++] = derived_value;
    return;
  }
  enqueue_job(state == kPromiseFulfilled ? on_fulfilled : on_rejected, promise->bound_this, derived_value, state == kPromiseFulfilled ? 0 : 1);
}

JsObject* Vm::promise_resolved(const Value& value) {
  if (is_promise(value)) return value.as_object();
  JsObject* p = new_promise();
  promise_resolve(p, value);
  return p;
}

void Vm::enqueue_job(const Value& handler, const Value& argument, const Value& derived, uint32_t kind) {
  if (!micro_ || !ensure_items(micro_, micro_->length + 4)) return;
  micro_->items[micro_->length++] = handler;
  micro_->items[micro_->length++] = argument;
  micro_->items[micro_->length++] = derived;
  micro_->items[micro_->length++] = Value::make_number(kind);
  if (micro_->length > (1u << 22)) halt("too many promise callbacks");
}

void Vm::enqueue_microtask(const Value& function) { enqueue_job(function, Value(), Value(), 2); }

void Vm::promise_job(const Value& handler, const Value& argument, const Value& derived, uint32_t kind) {
  JsObject* target = derived.is_object() ? derived.as_object() : nullptr;
  if (kind == 2) {   // queueMicrotask
    Value ignored;
    if (!call_value(handler, Value(), nullptr, 0, &ignored, false) && !halted_) {
      String text(allocator_);
      take_exception(&text);
      if (host_) {
        String line(allocator_);
        line.append("Uncaught ");
        line.append(text.view());
        host_->console(3, line.view());
      }
    }
    return;
  }
  if (kind == 3) {   // a promise that was resolved with a thenable follows it
    JsObject* shared = new_plain();
    JsObject* resolve = resolving_function(target, shared, false);
    JsObject* reject = resolving_function(target, shared, true);
    const Value fn_args[2] = {Value::make_object(resolve), Value::make_object(reject)};
    Value ignored;
    if (!call_value(handler, argument, fn_args, 2, &ignored, false) && !halted_) {
      const Value reason = exception_;
      has_exception_ = false;
      exception_ = Value();
      if (!shared->position) {
        shared->position = 1;
        promise_reject(target, reason);
      }
    }
    return;
  }
  const bool rejected = kind == 1;
  if (handler.is_object() && handler.as_object()->okind == ObjKind::Coroutine) {
    run_coroutine(handler.as_object(), false, rejected, argument);
    return;
  }
  if (!is_callable(handler)) {   // no callback: the result passes through
    if (target) {
      if (rejected) promise_reject(target, argument);
      else promise_resolve(target, argument);
    }
    return;
  }
  Value r;
  if (call_value(handler, Value(), &argument, 1, &r, false)) {
    if (target) promise_resolve(target, r);
  } else if (!halted_) {
    const Value reason = exception_;
    has_exception_ = false;
    exception_ = Value();
    if (target) promise_reject(target, reason);
  }
}

void Vm::report_rejections() {
  if (!rejections_) return;
  for (uint32_t i = 0; i < rejections_->length; ++i) {
    if (!rejections_->items[i].is_object()) continue;
    JsObject* p = rejections_->items[i].as_object();
    if (p->position & kPromiseHandled) continue;
    p->position |= kPromiseHandled;
    Value hook;
    if (get_named(Value::make_object(global()), "__unhandled_rejection", &hook) && is_callable(hook)) {   // the page may handle (and prevent) it: window "unhandledrejection"
      const Value hook_args[2] = {Value::make_object(p), p->bound_this};
      Value prevented;
      if (call(hook, Value(), hook_args, 2, &prevented)) {
        if (to_boolean(prevented)) continue;
      } else {
        Value ignored;
        take_exception_value(&ignored);
      }
    }
    if (!host_) continue;
    String line(allocator_);
    line.append("Uncaught (in promise) ");
    describe(p->bound_this, &line, 0);
    host_->console(3, line.view());
  }
  rejections_->length = 0;
}

void Vm::run_microtasks() {
  if (draining_ || !micro_) return;
  draining_ = true;
  while (micro_head_ + 4 <= micro_->length && !halted_) {
    // (the job stays in the queue, where the collector sees it, until it has run)
    const Value handler = micro_->items[micro_head_], argument = micro_->items[micro_head_ + 1], derived = micro_->items[micro_head_ + 2];
    const uint32_t kind = micro_->items[micro_head_ + 3].is_number() ? static_cast<uint32_t>(micro_->items[micro_head_ + 3].u.number) : 2;
    promise_job(handler, argument, derived, kind);
    micro_head_ += 4;
    if (micro_head_ >= micro_->length) {
      micro_->length = 0;
      micro_head_ = 0;
    } else if (micro_head_ >= 4096 && micro_head_ * 2 >= micro_->length) {
      const uint32_t rest = micro_->length - micro_head_;
      for (uint32_t i = 0; i < rest; ++i) micro_->items[i] = micro_->items[micro_head_ + i];
      micro_->length = rest;
      micro_head_ = 0;
    }
  }
  if (halted_) {
    micro_->length = 0;
    micro_head_ = 0;
  }
  draining_ = false;
  report_rejections();
}

void Vm::checkpoint() {
  if (frames_.count() == 0 && native_depth_ == 0 && !draining_ && micro_ && micro_head_ < micro_->length) run_microtasks();
  else if (frames_.count() == 0 && native_depth_ == 0 && !draining_ && rejections_ && rejections_->length) report_rejections();
}

Vm::Coroutine* Vm::coroutine_of(JsObject* co) const { return co && co->okind == ObjKind::Coroutine ? static_cast<Coroutine*>(co->host_data) : nullptr; }

bool Vm::start_async(JsObject* function, const Value& this_value, const Value* args, uint32_t argc, uint32_t base, Value* result) {
  (void)base;
  if (sp_ + 4 >= kStackValues) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  JsObject* promise = new_promise();
  stack_[sp_++] = Value::make_object(promise);   // (kept reachable)
  void* mem = allocator_.allocate(sizeof(Coroutine));
  JsObject* co = new_object(nullptr);
  if (!mem || co == sentinel_) {
    if (mem) allocator_.deallocate(mem);
    return throw_error(ErrorKind::RangeError, "out of memory");
  }
  co->okind = ObjKind::Coroutine;
  Coroutine* state = new (mem) Coroutine(allocator_);
  state->promise = promise;
  co->host_data = state;
  stack_[sp_++] = Value::make_object(co);
  if (async_depth_ >= 100) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  if (!push_frame(function, this_value, args, argc, sp_, false, Value())) return false;
  frames_[frames_.count() - 1].coroutine = co;
  if (!run_coroutine(co, true, false, Value())) return false;
  *result = Value::make_object(promise);
  return true;
}

bool Vm::run_coroutine(JsObject* co, bool start, bool throwing, const Value& input) {
  Coroutine* s = coroutine_of(co);
  if (!s || s->running) return true;
  JsObject* promise = s->promise;
  if (!start) {
    if (frames_.count() >= kMaxFrames || sp_ + s->stack.count() + 4 >= kStackValues || async_depth_ >= 100) {
      promise_reject(promise, Value::make_object(new_error(ErrorKind::RangeError, "Maximum call stack size exceeded")));
      return true;
    }
    const uint32_t base = sp_;
    Frame f;
    f.code = s->code;
    f.pc = s->pc;
    f.env = s->env;
    f.this_value = s->this_value;
    f.callee = s->callee;
    f.base = base;
    f.handlers = static_cast<uint32_t>(handlers_.count());
    f.construct = false;
    f.via_super = false;
    f.coroutine = co;
    for (uint64_t i = 0; i < s->stack.count(); ++i) stack_[sp_++] = s->stack[i];
    const uint32_t index = static_cast<uint32_t>(frames_.count());
    for (uint64_t i = 0; i < s->handlers.count(); ++i) handlers_.push_back(Handler{s->handlers[i].pc, s->handlers[i].stack + base, index, s->handlers[i].env});
    frames_.push_back(f);
    if (!throwing) stack_[sp_++] = input;
    s->stack.clear();
    s->handlers.clear();
  }
  const uint32_t entry = static_cast<uint32_t>(frames_.count() - 1);
  const uint32_t resume_sp = frames_[entry].base;
  s->running = true;
  ++async_depth_;
  suspended_ = false;
  bool ok;
  if (!start && throwing) {
    throw_value(input);
    ok = unwind(entry) && execute(entry);
  } else {
    ok = execute(entry);
  }
  --async_depth_;
  s->running = false;
  sp_ = resume_sp;
  if (ok) {
    if (suspended_) {
      suspended_ = false;
      const Value awaited = await_value_;
      await_value_ = Value();
      async_wait(co, awaited);
    } else {
      const Value r = last_result_;
      last_result_ = Value();
      promise_resolve(promise, r);
    }
    return true;
  }
  if (halted_) return false;
  const Value reason = exception_;
  has_exception_ = false;
  exception_ = Value();
  promise_reject(promise, reason);
  return true;
}

bool Vm::is_generator_object(const Value& v) {
  if (!v.is_object() || v.as_object()->okind != ObjKind::Coroutine || !v.as_object()->host_data) return false;
  return static_cast<Coroutine*>(v.as_object()->host_data)->generator;
}

// Calling a generator function: the frame is made and run up to the first thing it does (see the start of a generator in the compiler), then the generator
// object - the coroutine itself - is the result.
bool Vm::start_generator(JsObject* function, const Value& this_value, const Value* args, uint32_t argc, Value* result) {
  if (sp_ + 4 >= kStackValues) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  void* mem = allocator_.allocate(sizeof(Coroutine));
  JsObject* co = new_object(nullptr);
  if (!mem || co == sentinel_) {
    if (mem) allocator_.deallocate(mem);
    return throw_error(ErrorKind::RangeError, "out of memory");
  }
  co->okind = ObjKind::Coroutine;
  Coroutine* state = new (mem) Coroutine(allocator_);
  state->generator = true;
  co->host_data = state;
  stack_[sp_++] = Value::make_object(co);   // (kept reachable)
  Value proto;
  if (get(Value::make_object(function), prototype_, &proto) && proto.is_object()) co->proto = proto.as_object();
  if (async_depth_ >= 100) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
  if (!push_frame(function, this_value, args, argc, sp_, false, Value())) return false;
  frames_[frames_.count() - 1].coroutine = co;
  Value ignored;
  bool done = false;
  if (!run_generator(co, true, 0, Value(), &ignored, &done)) return false;
  *result = Value::make_object(co);
  return true;
}

bool Vm::run_generator(JsObject* co, bool start, int mode, const Value& input, Value* value, bool* done) {
  Coroutine* s = coroutine_of(co);
  *done = false;
  *value = Value();
  if (!s) return true;
  if (!start) {
    if (frames_.count() >= kMaxFrames || sp_ + s->stack.count() + 4 >= kStackValues || async_depth_ >= 100) return throw_error(ErrorKind::RangeError, "Maximum call stack size exceeded");
    const uint32_t base = sp_;
    Frame f;
    f.code = s->code;
    f.pc = s->pc;
    f.env = s->env;
    f.this_value = s->this_value;
    f.callee = s->callee;
    f.base = base;
    f.handlers = static_cast<uint32_t>(handlers_.count());
    f.construct = false;
    f.via_super = false;
    f.coroutine = co;
    for (uint64_t i = 0; i < s->stack.count(); ++i) stack_[sp_++] = s->stack[i];
    const uint32_t index = static_cast<uint32_t>(frames_.count());
    for (uint64_t i = 0; i < s->handlers.count(); ++i) handlers_.push_back(Handler{s->handlers[i].pc, s->handlers[i].stack + base, index, s->handlers[i].env});
    frames_.push_back(f);
    if (mode == 0) stack_[sp_++] = input;
    s->stack.clear();
    s->handlers.clear();
  }
  const uint32_t entry = static_cast<uint32_t>(frames_.count() - 1);
  const uint32_t resume_sp = frames_[entry].base;
  s->running = true;
  ++async_depth_;
  suspended_ = false;
  bool ok;
  if (!start && mode == 1) {
    throw_value(input);
    ok = unwind(entry) && execute(entry);
  } else if (!start && mode == 2) {   // return(value): finally blocks run, catch clauses do not
    JsObject* completion = new_plain();
    define_named(completion, "__generator_return", Value::make_bool(true), 0);
    define_named(completion, "value", input, 0);
    throw_value(Value::make_object(completion));
    ok = unwind(entry) && execute(entry);
  } else {
    ok = execute(entry);
  }
  --async_depth_;
  s->running = false;
  sp_ = resume_sp;
  if (ok) {
    if (suspended_) {   // a yield
      suspended_ = false;
      *value = await_value_;
      await_value_ = Value();
      return true;
    }
    *value = last_result_;
    last_result_ = Value();
    *done = true;
    s->done = true;
    return true;
  }
  s->done = true;
  return false;   // (it threw: the exception is pending)
}

bool Vm::is_generator_return(const Value& v) {
  Value flag;
  return v.is_object() && v.as_object()->okind == ObjKind::Plain && get_plain(v.as_object(), intern("__generator_return"), &flag) && flag.is_bool();
}

bool Vm::generator_resume(const Value& generator, int mode, const Value& input, Value* result) {
  if (!is_generator_object(generator)) return throw_error(ErrorKind::TypeError, "next method called on incompatible receiver");
  JsObject* co = generator.as_object();
  Coroutine* s = coroutine_of(co);
  Value value;
  bool done = false;
  if (s->running) return throw_error(ErrorKind::TypeError, "Generator is already running");
  const bool started = s->code != nullptr && !s->done;
  if (s->done || (mode == 2 && !started)) {
    s->done = true;
    if (mode == 1) {
      throw_value(input);
      return false;
    }
    value = mode == 2 ? input : Value();
    done = true;
  } else if (!run_generator(co, false, mode, input, &value, &done)) {
    if (mode == 2 && has_exception_ && is_generator_return(exception_)) {   // the return() went all the way out
      Value returned;
      get_plain(exception_.as_object(), intern("value"), &returned);
      has_exception_ = false;
      exception_ = Value();
      value = returned;
      done = true;
    } else {
      return false;
    }
  }
  JsObject* r = new_plain();
  define_named(r, "value", value);
  define_named(r, "done", Value::make_bool(done));
  *result = Value::make_object(r);
  return true;
}

void Vm::async_wait(JsObject* co, const Value& awaited) {
  JsObject* p = promise_resolved(awaited);
  promise_then(p, Value::make_object(co), Value::make_object(co), nullptr);
}


double Vm::add_timer(const Value& function, double delay_ms, double interval_ms, const Value* args, uint32_t argc) {
  Value list;
  if (!get_named(Value::make_object(global()), "__timers", &list) || !list.is_object()) return 0;
  Value counter;
  get_named(Value::make_object(global()), "__timer_id", &counter);
  const double id = (counter.is_number() ? counter.u.number : 0) + 1;
  define_named(global(), "__timer_id", Value::make_number(id), kWritable);
  if (delay_ms != delay_ms || delay_ms < 0) delay_ms = 0;
  Value level_value;
  get_named(Value::make_object(global()), "__timer_level", &level_value);
  const double level = (level_value.is_number() ? level_value.u.number : 0) + 1;   // timers started by timers: from the sixth level on, no sooner than 4 ms
  if (level > 5) {
    if (delay_ms < 4) delay_ms = 4;
    if (interval_ms == interval_ms && interval_ms >= 0 && interval_ms < 4) interval_ms = 4;
  }
  if (interval_ms == interval_ms && interval_ms >= 0 && interval_ms < 1) interval_ms = 1;
  JsObject* entry = new_array(4 + argc);
  array_push(entry, function);
  array_push(entry, Value::make_number(clock_ms() + (delay_ms < 1 && interval_ms >= 0 ? 1 : delay_ms)));
  array_push(entry, Value::make_number(id));
  array_push(entry, Value::make_number(interval_ms == interval_ms ? interval_ms : -1));
  for (uint32_t i = 0; i < argc; ++i) array_push(entry, args[i]);
  define_named(entry, "n", Value::make_number(level), 0);
  array_push(list.as_object(), Value::make_object(entry));
  return id;
}

void Vm::clear_timer(double id) {
  Value list;
  if (!get_named(Value::make_object(global()), "__timers", &list) || !list.is_object()) return;
  JsObject* l = list.as_object();
  for (uint32_t i = 0; i < l->length; ++i) {
    if (l->items[i].is_object() && l->items[i].as_object()->length >= 3 && l->items[i].as_object()->items[2].is_number() && l->items[i].as_object()->items[2].u.number == id) l->items[i] = Value();
  }
}

}  // namespace script
