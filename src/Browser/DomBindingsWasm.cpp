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

// The natives under the WebAssembly object (the classes and promises around them are JavaScript in WebPrelude.cpp): compiling and instantiating modules, exported
// functions, memories, tables and globals. The runtime itself is src/Wasm; this file turns script values into WebAssembly values and back and keeps the handles
// (small numbers) the script side holds for objects of the runtime.
//
// Everything a page makes lives until the page does (the runtime's Store is an arena), and what the script engine's collector must not free is kept in arrays of a
// hidden object, __wasm_roots: the script functions imported into modules (host_fns), the function objects made for exported functions (func_objs) and the current
// ArrayBuffer of every memory (buffers). The bytes of a memory are the bytes of that buffer, so that typed arrays over it and the code in the module see the same.
#include "Browser/WasmBindings.hpp"
#include "Script/Vm.hpp"
#include "Utility/String.hpp"
#include "Script/TypedArray.hpp"
#include "Utility/ByteText.hpp"
#include "Wasm/Wasm.hpp"

namespace browser {
namespace bind {

using namespace script;

struct WasmState final : wasm::IHost {
  explicit WasmState(script::Vm& v)
      : vm(v), store(v.allocator()), modules(v.allocator()), instances(v.allocator()), mems(v.allocator()), tables(v.allocator()), globals(v.allocator()), funcs(v.allocator()) {
    store.set_host(this);
    store.set_page_budget(1024);   // 64 MiB of linear memory in all
  }
  bool call(wasm::Store& s, wasm::FuncInst& function, const wasm::Slot* args, wasm::Slot* results) override;
  bool tick(uint64_t instructions) override { return vm.charge(instructions * 16); }

  script::Vm& vm;
  wasm::Store store;
  List<SharedPointer<wasm::Module>> modules;
  List<wasm::Instance*> instances;
  List<wasm::Memory*> mems;
  List<wasm::Table*> tables;
  List<wasm::Global*> globals;
  List<wasm::FuncInst*> funcs;
  JsObject *host_fns = nullptr, *func_objs = nullptr, *buffers = nullptr;   // arrays inside __wasm_roots
};

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

using wasm::Slot;
using wasm::ValType;

constexpr uint64_t kMaxMemoryBytes = 128u << 20;
constexpr uint64_t kMaxHandles = 1u << 20;

WasmState& state(Vm& vm) { return *static_cast<WasmState*>(vm.extension()); }

Value arg_at(const Value* args, uint32_t argc, uint32_t i) { return i < argc ? args[i] : Value(); }
Value str_value(Vm& vm, StringView text) { return Value::make_string(vm.new_string(text)); }
Value bool_value(bool b) { return Value::make_bool(b); }

// The text of a string-like argument ("" for undefined and null, ToString otherwise).
bool arg_text(Vm& vm, const Value& v, String* out) {
  if (v.is_nullish()) return out->assign(StringView());
  JsString* s;
  if (!vm.to_string(v, &s)) return false;
  return out->assign(s->view());
}

Value number(double d) { return Value::make_number(d); }

// Throws WebAssembly.<kind> with the message (an Error if that class is not there).
bool throw_wasm(Vm& vm, const char* kind, StringView message) {
  Value api, ctor;
  if (vm.get_named(Value::make_object(vm.global()), "WebAssembly", &api) && api.is_object() && vm.get_named(api, kind, &ctor) && Vm::is_callable(ctor)) {
    Value text = Value::make_string(vm.new_string(message));
    Value error;
    if (vm.construct(ctor, &text, 1, &error)) return vm.throw_value(error);
    return false;
  }
  return vm.throw_error(ErrorKind::Error, message);
}

bool throw_trap(Vm& vm, const char* message) {
  uint64_t length = 0;
  while (message[length]) ++length;
  const StringView m(message, length);
  if (m.equals("Maximum call stack size exceeded")) return vm.throw_error(ErrorKind::RangeError, m);
  if (m.equals("out of memory")) return vm.throw_error(ErrorKind::RangeError, "WebAssembly: out of memory");
  return throw_wasm(vm, "RuntimeError", m);
}

// A message made of an API name, a colon and the text.
void with_api(String* out, const Value& api, Vm& vm, StringView text) {
  JsString* s = nullptr;
  if (api.is_string()) s = api.as_string();
  if (s) {
    out->append(s->view());
    out->append(": ");
  }
  (void)vm;
  out->append(text);
}

bool bytes_of(Vm& vm, const Value& v, ArrayView<uint8_t>* out, const char* what) {
  if (!script::typed_array_bytes(v, out)) {
    String m(vm.allocator());
    m.append(what);
    m.append(" must be a BufferSource");
    return vm.throw_error(ErrorKind::TypeError, m.view());
  }
  return true;
}

// --- value conversions -----------------------------------------------------------------------------------------------------------------------

bool type_from_text(StringView t, ValType* out) {
  if (t.equals("i32")) *out = ValType::I32;
  else if (t.equals("i64")) *out = ValType::I64;
  else if (t.equals("f32")) *out = ValType::F32;
  else if (t.equals("f64")) *out = ValType::F64;
  else if (t.equals("v128")) *out = ValType::V128;
  else if (t.equals("anyfunc") || t.equals("funcref")) *out = ValType::FuncRef;
  else if (t.equals("externref")) *out = ValType::ExternRef;
  else return false;
  return true;
}

const char* type_text(ValType t) {
  switch (t) {
    case ValType::FuncRef: return "anyfunc";
    default: return wasm::val_type_name(t);
  }
}

NATIVE(wasm_function_call);

Value bigint_from(Vm& vm, int64_t v) {
  const bool neg = v < 0;
  uint64_t mag = neg ? uint64_t{0} - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
  char digits[24];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + mag % 10);
    mag /= 10;
  } while (mag);
  char text[24];
  for (uint32_t i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
  return Value::make_object(vm.new_bigint(neg, StringView(text, n)));
}

// ToBigInt64: a BigInt (reduced modulo 2^64), a boolean or a string of digits.
bool bigint_to(Vm& vm, const Value& v, int64_t* out) {
  Value value = v;
  bool negative = false;
  StringView digits;
  if (value.is_bool()) {
    *out = value.u.boolean ? 1 : 0;
    return true;
  }
  if (value.is_object() && !Vm::is_bigint(value)) {
    Value prim;
    if (!vm.to_primitive_hint(value, 2, &prim)) return false;
    value = prim;
  }
  JsString* text = nullptr;
  if (value.is_string()) {
    text = value.as_string();
    digits = text->view();
    if (!digits.empty() && digits[0] == '-') {
      negative = true;
      digits = digits.substr(1);
    }
    if (digits.empty()) {
      *out = 0;
      return true;
    }
  } else if (!Vm::is_bigint(value) || !vm.bigint_parts(value, &negative, &digits)) {
    String m(vm.allocator());
    String d(vm.allocator());
    vm.describe(v, &d);
    m.append("Cannot convert ");
    m.append(d.view());
    m.append(" to a BigInt");
    return vm.throw_error(ErrorKind::TypeError, m.view());
  }
  uint64_t acc = 0;
  for (uint64_t i = 0; i < digits.size(); ++i) {
    if (digits[i] < '0' || digits[i] > '9') {
      String m(vm.allocator());
      m.append("Cannot convert ");
      m.append(digits);
      m.append(" to a BigInt");
      return vm.throw_error(ErrorKind::SyntaxError, m.view());
    }
    acc = acc * 10 + static_cast<uint64_t>(digits[i] - '0');
  }
  *out = static_cast<int64_t>(negative ? uint64_t{0} - acc : acc);
  return true;
}

bool call_global(Vm& vm, const char* name, const Value& arg, Value* out) {
  Value fn;
  if (!vm.get_named(Value::make_object(vm.global()), name, &fn) || !Vm::is_callable(fn)) return vm.throw_error(ErrorKind::TypeError, "WebAssembly is not available");
  return vm.call(fn, Value(), &arg, 1, out);
}

Value function_value(WasmState& st, wasm::FuncInst* f);

bool is_wasm_function(const Value& v) { return v.is_object() && v.as_object()->okind == ObjKind::Native && v.as_object()->native == wasm_function_call; }

wasm::FuncInst* function_of(WasmState& st, const Value& v) {
  if (!is_wasm_function(v)) return nullptr;
  JsObject* o = v.as_object();
  if (o->length < 1 || !o->items[0].is_number()) return nullptr;
  const double h = o->items[0].u.number;
  if (!(h >= 1) || h > static_cast<double>(st.funcs.count())) return nullptr;
  return st.funcs[static_cast<uint64_t>(h) - 1];
}

// ToWebAssemblyValue
bool to_wasm(Vm& vm, WasmState& st, ValType t, const Value& v, Slot* out) {
  *out = Slot();
  if ((t == ValType::I32 || t == ValType::F32 || t == ValType::F64) && Vm::is_bigint(v)) return vm.throw_error(ErrorKind::TypeError, "Cannot convert a BigInt value to a number");
  switch (t) {
    case ValType::I32: {
      int32_t i;
      if (!vm.to_int32(v, &i)) return false;
      out->lo = static_cast<uint32_t>(i);
      return true;
    }
    case ValType::I64: {
      int64_t i = 0;
      if (!bigint_to(vm, v, &i)) return false;
      out->lo = static_cast<uint64_t>(i);
      return true;
    }
    case ValType::F32: {
      double d;
      if (!vm.to_number(v, &d)) return false;
      out->lo = __builtin_bit_cast(uint32_t, static_cast<float>(d));
      return true;
    }
    case ValType::F64: {
      double d;
      if (!vm.to_number(v, &d)) return false;
      out->lo = __builtin_bit_cast(uint64_t, d);
      return true;
    }
    case ValType::V128: return vm.throw_error(ErrorKind::TypeError, "type incompatibility when transforming from/to JS");
    case ValType::FuncRef: {
      if (v.is_null()) return true;
      wasm::FuncInst* f = function_of(st, v);
      if (!f) return vm.throw_error(ErrorKind::TypeError, "WebAssembly function reference must be null or an exported WebAssembly function");
      out->lo = wasm::func_ref(f);
      return true;
    }
    case ValType::ExternRef: {
      Value h;
      if (!call_global(vm, "__wasm_extern_in", v, &h)) return false;
      out->lo = h.is_number() ? static_cast<uint64_t>(h.u.number) : 0;
      return true;
    }
  }
  return false;
}

// ToJSValue
bool to_js(Vm& vm, WasmState& st, ValType t, const Slot& s, Value* out) {
  switch (t) {
    case ValType::I32: *out = number(static_cast<int32_t>(static_cast<uint32_t>(s.lo))); return true;
    case ValType::I64: *out = bigint_from(vm, static_cast<int64_t>(s.lo)); return true;
    case ValType::F32: *out = number(static_cast<double>(__builtin_bit_cast(float, static_cast<uint32_t>(s.lo)))); return true;
    case ValType::F64: *out = number(__builtin_bit_cast(double, s.lo)); return true;
    case ValType::V128: return vm.throw_error(ErrorKind::TypeError, "type incompatibility when transforming from/to JS");
    case ValType::FuncRef: *out = s.lo ? function_value(st, wasm::func_from_ref(s.lo)) : Value::make_null(); return true;
    case ValType::ExternRef: return call_global(vm, "__wasm_extern_out", number(static_cast<double>(s.lo)), out);
  }
  return false;
}

// --- the registries and the objects the script side holds --------------------------------------------------------------------------------------

bool array_set(Vm& vm, JsObject* array, uint64_t index, const Value& v) { return vm.put_member(Value::make_object(array), number(static_cast<double>(index)), v); }
bool array_get(Vm& vm, JsObject* array, uint64_t index, Value* out) { return vm.get_member(Value::make_object(array), number(static_cast<double>(index)), out); }

// The function object of an exported function (the same object every time).
Value function_value(WasmState& st, wasm::FuncInst* f) {
  Vm& vm = st.vm;
  if (f->handle == 0) {
    if (st.funcs.count() >= kMaxHandles || !st.funcs.push_back(f)) return Value::make_null();
    f->handle = static_cast<uint32_t>(st.funcs.count());
    String name(vm.allocator());
    name.append_uint(f->index);
    const Value id = number(f->handle);
    JsObject* fn = vm.new_native_with(wasm_function_call, name.view(), f->sig->nparams, &id, 1);
    array_set(vm, st.func_objs, f->handle - 1, Value::make_object(fn));
  }
  Value fn;
  array_get(vm, st.func_objs, f->handle - 1, &fn);
  return fn;
}

// The registry number of a memory, table or global (made when it first reaches the script side).
template <class T>
uint32_t handle_for(List<T*>& list, T* object) {
  if (object->host_id == 0) {
    if (list.count() >= kMaxHandles || !list.push_back(object)) return 0;
    object->host_id = static_cast<uint32_t>(list.count());
  }
  return object->host_id;
}

bool handle_arg(Vm& vm, const Value* args, uint32_t argc, uint32_t i, uint64_t limit, uint32_t* out) {
  double d;
  if (!vm.to_number(arg_at(args, argc, i), &d)) return false;
  if (!(d >= 1) || d > static_cast<double>(limit)) return vm.throw_error(ErrorKind::TypeError, "Not a WebAssembly object");
  *out = static_cast<uint32_t>(d);
  return true;
}

// A wrapper object's registry number: its hidden __wasm_id when it is of the given class.
bool wrapper_handle(Vm& vm, const Value& v, const char* kind, uint32_t* out) {
  if (!v.is_object()) return false;
  Value k, id;
  if (!vm.get_plain(v.as_object(), vm.intern("__wasm_kind"), &k) || !k.is_string() || !k.as_string()->view().equals(kind)) return false;
  if (!vm.get_plain(v.as_object(), vm.intern("__wasm_id"), &id) || !id.is_number()) return false;
  *out = static_cast<uint32_t>(id.u.number);
  return true;
}

// --- memory in the script engine's buffers ------------------------------------------------------------------------------------------------------

bool memory_resize(wasm::Memory& m, uint64_t new_size) {
  WasmState& st = *static_cast<WasmState*>(m.host);
  Vm& vm = st.vm;
  if (new_size > kMaxMemoryBytes) return false;
  // (the engine stops a script that goes far over its memory limit: a memory that cannot fit is refused instead, which is a RangeError or a -1 from memory.grow)
  const uint64_t limit = vm.memory_limit() + (vm.memory_limit() >> 2);
  if (vm.heap_bytes() + new_size + (1u << 20) > limit) return false;
  JsString* cell = vm.new_bytes(static_cast<uint32_t>(new_size));
  if (vm.halted()) return false;
  uint8_t* data = chars_as_bytes(cell->data());
  if (m.size) ArrayView<uint8_t>(data, new_size).first(m.size).copy_from(ConstByteView(m.data, m.size));
  JsObject* buffer = script::new_array_buffer(vm, cell);
  const uint32_t id = handle_for(st.mems, &m);
  if (id == 0) return false;
  Value old;
  if (array_get(vm, st.buffers, id - 1, &old) && old.is_object()) script::detach_array_buffer(old.as_object());
  array_set(vm, st.buffers, id - 1, Value::make_object(buffer));
  m.data = data;
  m.size = new_size;
  return true;
}

// --- calling out to script ------------------------------------------------------------------------------------------------------------------------

}  // namespace

bool WasmState::call(wasm::Store&, wasm::FuncInst& function, const Slot* args, Slot* results) {
  const wasm::FuncSig& sig = *function.sig;
  Value fn;
  if (!array_get(vm, host_fns, function.host_id - 1, &fn) || !Vm::is_callable(fn)) return vm.throw_error(ErrorKind::TypeError, "imported function is not callable");
  wasm::SmallArray<Value, 8> js_args(vm.allocator(), sig.nparams);
  if (!js_args.ok()) return vm.throw_error(ErrorKind::RangeError, "WebAssembly: out of memory");
  for (uint32_t i = 0; i < sig.nparams; ++i)
    if (!to_js(vm, *this, sig.params[i], args[i], &js_args[i])) return false;
  Value r;
  if (!vm.call(fn, Value(), js_args.data(), sig.nparams, &r)) return false;
  vm.hold_collector();
  bool ok = true;
  if (sig.nresults == 1) {
    ok = to_wasm(vm, *this, sig.results[0], r, &results[0]);
  } else if (sig.nresults > 1) {
    JsObject* list = vm.new_array();
    ok = vm.iterate_into(list, r, -1);
    if (ok && list->length != sig.nresults) ok = vm.throw_error(ErrorKind::TypeError, "multi-return length mismatch");
    for (uint32_t i = 0; ok && i < sig.nresults; ++i) {
      Value item;
      ok = array_get(vm, list, i, &item) && to_wasm(vm, *this, sig.results[i], item, &results[i]);
    }
  }
  vm.release_collector();
  return ok;
}

namespace {

// The exported function object: this native with the registry number of its function in items[0].
NATIVE(wasm_function_call) {
  (void)self;
  WasmState& st = state(vm);
  JsObject* me = vm.current_native();
  wasm::FuncInst* f = me && me->length >= 1 && me->items[0].is_number() ? st.funcs[static_cast<uint64_t>(me->items[0].u.number) - 1] : nullptr;
  if (!f) return vm.throw_error(ErrorKind::TypeError, "not a WebAssembly function");
  const wasm::FuncSig& sig = *f->sig;
  wasm::SmallArray<Slot, 8> in(vm.allocator(), sig.nparams), out(vm.allocator(), sig.nresults);
  if (!in.ok() || !out.ok()) return vm.throw_error(ErrorKind::RangeError, "WebAssembly: out of memory");
  for (uint32_t i = 0; i < sig.nparams; ++i)
    if (!to_wasm(vm, st, sig.params[i], arg_at(args, argc, i), &in[i])) return false;
  const char* trap = nullptr;
  const wasm::Status status = st.store.invoke(*f, in.data(), out.data(), &trap);
  if (status == wasm::Status::HostError) return false;
  if (status == wasm::Status::Trap) return throw_trap(vm, trap ? trap : "trap");
  if (sig.nresults == 0) {
    *result = Value();
    return true;
  }
  vm.hold_collector();
  bool ok = true;
  if (sig.nresults == 1) {
    ok = to_js(vm, st, sig.results[0], out[0], result);
  } else {
    JsObject* list = vm.new_array();
    for (uint32_t i = 0; ok && i < sig.nresults; ++i) {
      Value v;
      ok = to_js(vm, st, sig.results[i], out[i], &v) && vm.array_push(list, v);
    }
    *result = Value::make_object(list);
  }
  vm.release_collector();
  return ok;
}

// --- Module -----------------------------------------------------------------------------------------------------------------------------------------

// __wasm_validate(bytes)
NATIVE(wasm_validate) {
  (void)self;
  ArrayView<uint8_t> bytes;
  if (!bytes_of(vm, arg_at(args, argc, 0), &bytes, "WebAssembly.validate(): Argument 0")) return false;
  wasm::Module m(vm.allocator());
  String error(vm.allocator());
  *result = bool_value(m.parse(ConstByteView(bytes.data(), bytes.size()), &error));
  return true;
}

// __wasm_compile(bytes, api): the registry number of the module
NATIVE(wasm_compile) {
  (void)self;
  WasmState& st = state(vm);
  ArrayView<uint8_t> bytes;
  if (!bytes_of(vm, arg_at(args, argc, 0), &bytes, "WebAssembly.Module(): Argument 0")) return false;
  SharedPointer<wasm::Module> m = SharedPointer<wasm::Module>::make(vm.allocator(), vm.allocator());
  String error(vm.allocator());
  if (!m || !m->parse(ConstByteView(bytes.data(), bytes.size()), &error)) {
    String message(vm.allocator());
    with_api(&message, arg_at(args, argc, 1), vm, error.view());
    return throw_wasm(vm, "CompileError", message.view());
  }
  if (st.modules.count() >= kMaxHandles || !st.modules.push_back(m)) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Module(): out of memory");
  *result = number(static_cast<double>(st.modules.count()));
  return true;
}

bool module_arg(Vm& vm, WasmState& st, const Value* args, uint32_t argc, SharedPointer<wasm::Module>* out) {
  uint32_t h;
  if (!handle_arg(vm, args, argc, 0, st.modules.count(), &h)) return false;
  *out = st.modules[h - 1];
  return true;
}

const char* kind_text(wasm::ExternKind k) {
  switch (k) {
    case wasm::ExternKind::Func: return "function";
    case wasm::ExternKind::Table: return "table";
    case wasm::ExternKind::Memory: return "memory";
    case wasm::ExternKind::Global: return "global";
  }
  return "";
}

// __wasm_module_imports(handle): [[module, name, kind], ...]
NATIVE(wasm_module_imports) {
  (void)self;
  WasmState& st = state(vm);
  SharedPointer<wasm::Module> m;
  if (!module_arg(vm, st, args, argc, &m)) return false;
  JsObject* list = vm.new_array();
  *result = Value::make_object(list);
  for (uint64_t i = 0; i < m->imports.count(); ++i) {
    const wasm::Import& imp = *m->imports[i];
    JsObject* row = vm.new_array();
    vm.array_push(row, str_value(vm, imp.module.view()));
    vm.array_push(row, str_value(vm, imp.name.view()));
    vm.array_push(row, str_value(vm, kind_text(imp.kind)));
    vm.array_push(list, Value::make_object(row));
  }
  return true;
}

// __wasm_module_exports(handle): [[name, kind], ...]
NATIVE(wasm_module_exports) {
  (void)self;
  WasmState& st = state(vm);
  SharedPointer<wasm::Module> m;
  if (!module_arg(vm, st, args, argc, &m)) return false;
  JsObject* list = vm.new_array();
  *result = Value::make_object(list);
  for (uint64_t i = 0; i < m->exports.count(); ++i) {
    const wasm::Export& e = *m->exports[i];
    JsObject* row = vm.new_array();
    vm.array_push(row, str_value(vm, e.name.view()));
    vm.array_push(row, str_value(vm, kind_text(e.kind)));
    vm.array_push(list, Value::make_object(row));
  }
  return true;
}

// __wasm_module_custom(handle, name): the Uint8Arrays of the custom sections with that name
NATIVE(wasm_module_custom) {
  (void)self;
  WasmState& st = state(vm);
  SharedPointer<wasm::Module> m;
  if (!module_arg(vm, st, args, argc, &m)) return false;
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 1), &name)) return false;
  JsObject* list = vm.new_array();
  *result = Value::make_object(list);
  for (uint64_t i = 0; i < m->customs.count(); ++i) {
    const wasm::CustomSection& cs = *m->customs[i];
    if (cs.name.view().equals(name.view())) vm.array_push(list, Value::make_object(script::new_typed_array(vm, script::ArrayKind::Uint8, bytes_as_text(cs.bytes.view()))));
  }
  return true;
}

// --- Instance -----------------------------------------------------------------------------------------------------------------------------------------

// The value an import is given: the extern the module asks for, or a message that says why none can be made.
bool resolve_import(Vm& vm, WasmState& st, const wasm::Module& m, const wasm::Import& imp, const Value& v, wasm::ExternVal* out, const char** why) {
  *why = nullptr;
  out->kind = imp.kind;
  switch (imp.kind) {
    case wasm::ExternKind::Func: {
      if (!Vm::is_callable(v)) {
        *why = "function import requires a callable";
        return true;
      }
      if (wasm::FuncInst* f = function_of(st, v)) {
        out->ptr = f;
        return true;
      }
      if (st.host_fns->length >= kMaxHandles) {
        *why = "too many imported functions";
        return true;
      }
      const wasm::FuncSig* sig = st.store.intern_sig(*m.types[imp.type_index]);
      array_set(vm, st.host_fns, st.host_fns->length, v);
      wasm::FuncInst* f = sig ? st.store.new_host_function(sig, st.host_fns->length, nullptr) : nullptr;
      if (!f) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Instance(): out of memory");
      out->ptr = f;
      return true;
    }
    case wasm::ExternKind::Table: {
      uint32_t h;
      if (!wrapper_handle(vm, v, "table", &h) || h > st.tables.count()) {
        *why = "table import requires a WebAssembly.Table";
        return true;
      }
      out->ptr = st.tables[h - 1];
      return true;
    }
    case wasm::ExternKind::Memory: {
      uint32_t h;
      if (!wrapper_handle(vm, v, "memory", &h) || h > st.mems.count()) {
        *why = "memory import must be a WebAssembly.Memory object";
        return true;
      }
      out->ptr = st.mems[h - 1];
      return true;
    }
    case wasm::ExternKind::Global: {
      uint32_t h;
      if (wrapper_handle(vm, v, "global", &h) && h <= st.globals.count()) {
        out->ptr = st.globals[h - 1];
        return true;
      }
      if (imp.global.mut) {
        *why = "imported mutable global must be a WebAssembly.Global object";
        return true;
      }
      const ValType t = imp.global.type;
      const bool number_ok = (t == ValType::I32 || t == ValType::F32 || t == ValType::F64) && v.is_number();
      const bool bigint_ok = t == ValType::I64 && Vm::is_bigint(v);
      const bool ref_ok = t == ValType::ExternRef || (t == ValType::FuncRef && (v.is_null() || is_wasm_function(v)));
      if (!number_ok && !bigint_ok && !ref_ok) {
        *why = "global import must be a number, valid Wasm reference, or WebAssembly.Global object";
        return true;
      }
      Slot value;
      if (!to_wasm(vm, st, t, v, &value)) return false;
      wasm::Global* g = st.store.new_global(imp.global, value);
      if (!g) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Instance(): out of memory");
      out->ptr = g;
      return true;
    }
  }
  return true;
}

// __wasm_instantiate(module, imports, api): imports is an array with one value for each import of the module; gives the instance's registry number
NATIVE(wasm_instantiate) {
  (void)self;
  WasmState& st = state(vm);
  SharedPointer<wasm::Module> m;
  if (!module_arg(vm, st, args, argc, &m)) return false;
  const Value imports = arg_at(args, argc, 1);
  const Value api = arg_at(args, argc, 2);
  List<wasm::ExternVal> resolved(vm.allocator());
  for (uint64_t i = 0; i < m->imports.count(); ++i) {
    const wasm::Import& imp = *m->imports[i];
    Value v;
    if (!imports.is_object() || !array_get(vm, imports.as_object(), i, &v)) v = Value();
    wasm::ExternVal ev;
    const char* why;
    if (!resolve_import(vm, st, *m, imp, v, &ev, &why)) return false;
    if (why) {
      String message(vm.allocator());
      String text(vm.allocator());
      text.append("Import #");
      text.append_uint(i);
      text.append(" \"");
      text.append(imp.module.view());
      text.append("\" \"");
      text.append(imp.name.view());
      text.append("\": ");
      text.append(why);
      with_api(&message, api, vm, text.view());
      return throw_wasm(vm, "LinkError", message.view());
    }
    if (!resolved.push_back(ev)) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Instance(): out of memory");
  }
  wasm::Instance* inst = nullptr;
  String error(vm.allocator());
  bool link = false;
  wasm::Status status = wasm::Status::Ok;
  if (!st.store.instantiate(m, ArrayView<const wasm::ExternVal>(resolved.view().data(), resolved.count()), &inst, &error, &link, &status)) {
    if (status == wasm::Status::HostError) return false;
    String message(vm.allocator());
    with_api(&message, api, vm, error.view());
    if (link) return throw_wasm(vm, "LinkError", message.view());
    if (status == wasm::Status::Trap) return throw_trap(vm, error.c_str());
    return vm.throw_error(ErrorKind::RangeError, message.view());
  }
  if (!inst || st.instances.count() >= kMaxHandles || !st.instances.push_back(inst)) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Instance(): out of memory");
  *result = number(static_cast<double>(st.instances.count()));
  return true;
}

// __wasm_instance_exports(instance): [[name, kind, value], ...] - a function object for a function, the registry number for the rest
NATIVE(wasm_instance_exports) {
  (void)self;
  WasmState& st = state(vm);
  uint32_t h;
  if (!handle_arg(vm, args, argc, 0, st.instances.count(), &h)) return false;
  wasm::Instance& inst = *st.instances[h - 1];
  JsObject* list = vm.new_array();
  *result = Value::make_object(list);
  const wasm::Module& m = *inst.module;
  for (uint64_t i = 0; i < m.exports.count(); ++i) {
    const wasm::Export& e = *m.exports[i];
    const wasm::ExternVal ev = wasm::Store::export_value(inst, e);
    Value v;
    switch (e.kind) {
      case wasm::ExternKind::Func: v = function_value(st, static_cast<wasm::FuncInst*>(ev.ptr)); break;
      case wasm::ExternKind::Table: v = number(handle_for(st.tables, static_cast<wasm::Table*>(ev.ptr))); break;
      case wasm::ExternKind::Memory: v = number(handle_for(st.mems, static_cast<wasm::Memory*>(ev.ptr))); break;
      case wasm::ExternKind::Global: v = number(handle_for(st.globals, static_cast<wasm::Global*>(ev.ptr))); break;
    }
    JsObject* row = vm.new_array();
    vm.array_push(row, str_value(vm, e.name.view()));
    vm.array_push(row, str_value(vm, kind_text(e.kind)));
    vm.array_push(row, v);
    vm.array_push(list, Value::make_object(row));
  }
  return true;
}

// --- Memory ----------------------------------------------------------------------------------------------------------------------------------------------

bool pages_arg(Vm& vm, const Value& v, uint32_t* out) {
  double d;
  if (!vm.to_number(v, &d)) return false;
  if (!(d >= 0) || d > 65536 || d != static_cast<double>(static_cast<uint32_t>(d))) return vm.throw_error(ErrorKind::TypeError, "WebAssembly.Memory(): Property 'initial': value is out of range");
  *out = static_cast<uint32_t>(d);
  return true;
}

// __wasm_memory_new(initial, maximum (undefined for none))
NATIVE(wasm_memory_new) {
  (void)self;
  WasmState& st = state(vm);
  wasm::MemType type;
  if (!pages_arg(vm, arg_at(args, argc, 0), &type.limits.min)) return false;
  const Value max = arg_at(args, argc, 1);
  if (!max.is_undefined()) {
    type.limits.has_max = true;
    if (!pages_arg(vm, max, &type.limits.max)) return false;
    if (type.limits.max < type.limits.min) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Memory(): Property 'maximum': value is below the 'initial' value");
  }
  wasm::Memory* m = st.store.new_memory(type, memory_resize, &st);
  if (!m) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Memory(): could not allocate memory");
  *result = number(handle_for(st.mems, m));
  return true;
}

// __wasm_memory_buffer(handle)
NATIVE(wasm_memory_buffer) {
  (void)self;
  WasmState& st = state(vm);
  uint32_t h;
  if (!handle_arg(vm, args, argc, 0, st.mems.count(), &h)) return false;
  return array_get(vm, st.buffers, h - 1, result);
}

// __wasm_memory_grow(handle, delta): the old size in pages
NATIVE(wasm_memory_grow) {
  (void)self;
  WasmState& st = state(vm);
  uint32_t h, delta;
  if (!handle_arg(vm, args, argc, 0, st.mems.count(), &h)) return false;
  double d;
  if (!vm.to_number(arg_at(args, argc, 1), &d)) return false;
  if (!(d >= 0) || d > 65536 || d != static_cast<double>(static_cast<uint32_t>(d))) return vm.throw_error(ErrorKind::TypeError, "WebAssembly.Memory.grow(): Argument 0 must be non-negative");
  delta = static_cast<uint32_t>(d);
  wasm::Memory* m = st.mems[h - 1];
  const int64_t old = m->grow(delta);
  if (old < 0) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Memory.grow(): Maximum memory size exceeded");
  *result = number(static_cast<double>(old));
  return true;
}

// --- Table -----------------------------------------------------------------------------------------------------------------------------------------------

bool table_arg(Vm& vm, WasmState& st, const Value* args, uint32_t argc, wasm::Table** out) {
  uint32_t h;
  if (!handle_arg(vm, args, argc, 0, st.tables.count(), &h)) return false;
  *out = st.tables[h - 1];
  return true;
}

// __wasm_table_new(elementType, initial, maximum, init)
NATIVE(wasm_table_new) {
  (void)self;
  WasmState& st = state(vm);
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  wasm::TableType type;
  ValType t;
  if (!type_from_text(text.view(), &t) || !wasm::is_ref_type(t)) return vm.throw_error(ErrorKind::TypeError, "WebAssembly.Table(): Descriptor property 'element' must be a WebAssembly reference type");
  type.elem = t;
  double d;
  if (!vm.to_number(arg_at(args, argc, 1), &d)) return false;
  if (!(d >= 0) || d > wasm::kMaxTableSize) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Table(): Property 'initial': value is above the upper bound");
  type.limits.min = static_cast<uint32_t>(d);
  const Value max = arg_at(args, argc, 2);
  if (!max.is_undefined()) {
    if (!vm.to_number(max, &d)) return false;
    if (!(d >= type.limits.min) || d > 4294967295.0) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Table(): Property 'maximum': value is below the 'initial' value");
    type.limits.has_max = true;
    type.limits.max = static_cast<uint32_t>(d);
  }
  Slot init;   // (the script side gives null for a funcref table and undefined for an externref one when the page named no value)
  if (!to_wasm(vm, st, t, arg_at(args, argc, 3), &init)) return false;
  wasm::Table* table = st.store.new_table(type, init.lo);
  if (!table) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Table(): could not allocate memory");
  *result = number(handle_for(st.tables, table));
  return true;
}

NATIVE(wasm_table_length) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Table* t;
  if (!table_arg(vm, st, args, argc, &t)) return false;
  *result = number(static_cast<double>(t->items.count()));
  return true;
}

bool index_arg(Vm& vm, const Value& v, wasm::Table& t, uint32_t* out, const char* api) {
  double d;
  if (!vm.to_number(v, &d)) return false;
  if (!(d >= 0) || d != static_cast<double>(static_cast<uint32_t>(d)) || d >= static_cast<double>(t.items.count())) {
    String m(vm.allocator());
    m.append(api);
    m.append(": invalid address ");
    m.append_uint(d >= 0 && d < 4294967296.0 ? static_cast<uint64_t>(d) : 0);
    m.append(" in ");
    m.append(t.elem == ValType::FuncRef ? "funcref" : "externref");
    m.append(" table of size ");
    m.append_uint(t.items.count());
    return vm.throw_error(ErrorKind::RangeError, m.view());
  }
  *out = static_cast<uint32_t>(d);
  return true;
}

// __wasm_table_get(handle, index)
NATIVE(wasm_table_get) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Table* t;
  uint32_t i;
  if (!table_arg(vm, st, args, argc, &t) || !index_arg(vm, arg_at(args, argc, 1), *t, &i, "WebAssembly.Table.get()")) return false;
  Slot s;
  s.lo = t->items[i];
  return to_js(vm, st, t->elem, s, result);
}

// __wasm_table_set(handle, index, value)
NATIVE(wasm_table_set) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Table* t;
  uint32_t i;
  if (!table_arg(vm, st, args, argc, &t) || !index_arg(vm, arg_at(args, argc, 1), *t, &i, "WebAssembly.Table.set()")) return false;
  Slot s;
  if (!to_wasm(vm, st, t->elem, arg_at(args, argc, 2), &s)) return false;
  t->items[i] = s.lo;
  *result = Value();
  return true;
}

// __wasm_table_grow(handle, delta, init): the old length
NATIVE(wasm_table_grow) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Table* t;
  if (!table_arg(vm, st, args, argc, &t)) return false;
  double d;
  if (!vm.to_number(arg_at(args, argc, 1), &d)) return false;
  if (!(d >= 0) || d != static_cast<double>(static_cast<uint32_t>(d))) return vm.throw_error(ErrorKind::TypeError, "WebAssembly.Table.grow(): Argument 0 must be convertible to a valid number");
  Slot init;
  if (argc > 2 && !args[2].is_undefined() && !to_wasm(vm, st, t->elem, args[2], &init)) return false;
  const int64_t old = t->grow(static_cast<uint32_t>(d), init.lo);
  if (old < 0) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Table.grow(): failed to grow table");
  *result = number(static_cast<double>(old));
  return true;
}

// --- Global --------------------------------------------------------------------------------------------------------------------------------------------------

bool global_arg(Vm& vm, WasmState& st, const Value* args, uint32_t argc, wasm::Global** out) {
  uint32_t h;
  if (!handle_arg(vm, args, argc, 0, st.globals.count(), &h)) return false;
  *out = st.globals[h - 1];
  return true;
}

// __wasm_global_new(type, mutable, value)
NATIVE(wasm_global_new) {
  (void)self;
  WasmState& st = state(vm);
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  wasm::GlobalType type;
  if (!type_from_text(text.view(), &type.type) || type.type == ValType::V128) return vm.throw_error(ErrorKind::TypeError, "WebAssembly.Global(): Descriptor property 'value' must be a WebAssembly type");
  type.mut = Vm::to_boolean(arg_at(args, argc, 1));
  Slot value;
  if (!to_wasm(vm, st, type.type, arg_at(args, argc, 2), &value)) return false;
  wasm::Global* g = st.store.new_global(type, value);
  if (!g) return vm.throw_error(ErrorKind::RangeError, "WebAssembly.Global(): out of memory");
  *result = number(handle_for(st.globals, g));
  return true;
}

NATIVE(wasm_global_get) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Global* g;
  if (!global_arg(vm, st, args, argc, &g)) return false;
  return to_js(vm, st, g->type.type, g->value, result);
}

NATIVE(wasm_global_set) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Global* g;
  if (!global_arg(vm, st, args, argc, &g)) return false;
  if (!g->type.mut) return vm.throw_error(ErrorKind::TypeError, "WebAssembly.Global.value: Can't set the value of an immutable global.");
  Slot s;
  if (!to_wasm(vm, st, g->type.type, arg_at(args, argc, 1), &s)) return false;
  g->value = s;
  *result = Value();
  return true;
}

// __wasm_global_type(handle): [type, mutable]
NATIVE(wasm_global_type) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Global* g;
  if (!global_arg(vm, st, args, argc, &g)) return false;
  JsObject* row = vm.new_array();
  vm.array_push(row, str_value(vm, type_text(g->type.type)));
  vm.array_push(row, bool_value(g->type.mut));
  *result = Value::make_object(row);
  return true;
}

// __wasm_table_type(handle): [element type, initial, maximum or undefined]
NATIVE(wasm_table_type) {
  (void)self;
  WasmState& st = state(vm);
  wasm::Table* t;
  if (!table_arg(vm, st, args, argc, &t)) return false;
  JsObject* row = vm.new_array();
  vm.array_push(row, str_value(vm, type_text(t->elem)));
  vm.array_push(row, number(static_cast<double>(t->items.count())));
  vm.array_push(row, t->has_max ? number(t->max) : Value());
  *result = Value::make_object(row);
  return true;
}

// __wasm_is_function(value)
NATIVE(wasm_is_function) {
  (void)self;
  WasmState& st = state(vm);
  *result = bool_value(function_of(st, arg_at(args, argc, 0)) != nullptr);
  return true;
}

}  // namespace

UniquePointer<WasmState> install_wasm_natives(Vm& vm) {
  UniquePointer<WasmState> owned = UniquePointer<WasmState>::make(vm.allocator(), vm);
  if (!owned) return owned;
  vm.set_extension(owned.get());
  WasmState& st = *owned;
  JsObject* roots = vm.new_plain();
  st.store.set_memory_hook(memory_resize, &st);
  st.host_fns = vm.new_array();
  st.func_objs = vm.new_array();
  st.buffers = vm.new_array();
  vm.define_named(roots, "host_fns", Value::make_object(st.host_fns), 0);
  vm.define_named(roots, "func_objs", Value::make_object(st.func_objs), 0);
  vm.define_named(roots, "buffers", Value::make_object(st.buffers), 0);
  JsObject* g = vm.global();
  vm.define_named(g, "__wasm_roots", Value::make_object(roots), 0);
  vm.define_native(g, "__wasm_validate", wasm_validate, 1);
  vm.define_native(g, "__wasm_compile", wasm_compile, 2);
  vm.define_native(g, "__wasm_module_imports", wasm_module_imports, 1);
  vm.define_native(g, "__wasm_module_exports", wasm_module_exports, 1);
  vm.define_native(g, "__wasm_module_custom", wasm_module_custom, 2);
  vm.define_native(g, "__wasm_instantiate", wasm_instantiate, 3);
  vm.define_native(g, "__wasm_instance_exports", wasm_instance_exports, 1);
  vm.define_native(g, "__wasm_memory_new", wasm_memory_new, 2);
  vm.define_native(g, "__wasm_memory_buffer", wasm_memory_buffer, 1);
  vm.define_native(g, "__wasm_memory_grow", wasm_memory_grow, 2);
  vm.define_native(g, "__wasm_table_new", wasm_table_new, 4);
  vm.define_native(g, "__wasm_table_length", wasm_table_length, 1);
  vm.define_native(g, "__wasm_table_get", wasm_table_get, 2);
  vm.define_native(g, "__wasm_table_set", wasm_table_set, 3);
  vm.define_native(g, "__wasm_table_grow", wasm_table_grow, 3);
  vm.define_native(g, "__wasm_table_type", wasm_table_type, 1);
  vm.define_native(g, "__wasm_global_new", wasm_global_new, 3);
  vm.define_native(g, "__wasm_global_get", wasm_global_get, 1);
  vm.define_native(g, "__wasm_global_set", wasm_global_set, 2);
  vm.define_native(g, "__wasm_global_type", wasm_global_type, 1);
  vm.define_native(g, "__wasm_is_function", wasm_is_function, 1);
  if (vm.halted()) return UniquePointer<WasmState>();
  return owned;
}

}  // namespace bind
}  // namespace browser
