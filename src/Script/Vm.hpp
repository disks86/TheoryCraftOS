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

// The virtual machine: the heap (strings, objects, environments, with a mark-and-sweep collector), the interpreter loop and the operations the
// built-in functions are written with. Errors are thrown as script values: functions that can throw return false and leave the exception in
// the VM (take_exception()); there are no C++ exceptions. A script is limited in the instructions it may run and the memory it may use, and stops
// with an error when it passes them (a page cannot hang or exhaust the browser).
#pragma once
#include "Script/Bytecode.hpp"
#include "Utility/String.hpp"

namespace script {

enum class ErrorKind : uint8_t { Error, TypeError, RangeError, ReferenceError, SyntaxError };

// What a script can reach in the outside world.
class IHost {
 public:
  virtual void console(int level, StringView text) = 0;   // 0 log, 1 info, 2 warn, 3 error
  virtual void alert(StringView text) = 0;
  virtual void document_write(StringView text, bool newline) = 0;
  virtual uint64_t now_ms() = 0;                          // milliseconds since 1970 (0 if there is no clock)
  virtual StringView location() = 0;                      // the address of the page
  virtual StringView title() = 0;
  virtual void set_title(StringView title) = 0;
  virtual StringView user_agent() = 0;

 protected:
  IHost() = default;
  ~IHost() = default;
};

class Vm {
 public:
  static constexpr uint32_t kStackValues = 16384, kMaxFrames = 256;

  Vm(allocators::IAllocator& allocator, IHost* host);
  ~Vm();
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  bool ok() const { return ok_; }
  allocators::IAllocator& allocator() { return allocator_; }
  IHost* host() { return host_; }

  // --- Limits
  void set_instruction_budget(uint64_t instructions) { budget_ = instructions; }
  void set_memory_limit(uint64_t bytes) { memory_limit_ = bytes; }
  uint64_t instructions_run() const { return instructions_; }
  uint64_t heap_bytes() const { return heap_bytes_; }

  // --- Running
  // Runs top-level code (a FunctionCode from the compiler) with `this` the global object. False if it threw (see take_exception()) or hit a limit.
  bool run(FunctionCode* code, Value* result);
  // Calls a function value.
  bool call(const Value& function, const Value& this_value, const Value* args, uint32_t argc, Value* result);
  bool construct(const Value& function, const Value* args, uint32_t argc, Value* result);
  // The pending exception as text ("TypeError: x is not a function"), and it is cleared.
  void take_exception(String* text);
  bool has_exception() const { return has_exception_; }
  // Throws: returns false, for natives to return.
  bool throw_error(ErrorKind kind, StringView message);
  bool throw_value(const Value& value);

  // --- Code the compiler made is kept here (its constants are roots of the collector).
  FunctionCode* new_code();

  // --- Strings and objects
  JsString* new_string(StringView text);
  JsString* intern(StringView text);
  JsString* concat(JsString* a, JsString* b);
  JsObject* new_object(JsObject* proto);
  JsObject* new_plain() { return new_object(object_proto_); }
  JsObject* new_array(uint32_t capacity = 0);
  JsObject* new_native(NativeFn fn, StringView name, uint32_t arity);
  JsObject* new_function(FunctionCode* code, Env* env, const Value& this_value);
  JsObject* new_error(ErrorKind kind, StringView message);
  // An object whose properties `host` answers for; `data` is its (the host's) own pointer.
  JsObject* new_host_object(JsObject* proto, const HostClass* host, void* data);
  Env* new_env(uint32_t slots, Env* parent);

  // --- Properties. get() and friends accept any base value (primitives use their prototypes); null and undefined throw.
  bool get(const Value& base, JsString* key, Value* out);
  bool get_member(const Value& base, const Value& key, Value* out);
  bool put(const Value& base, JsString* key, const Value& value);
  bool put_member(const Value& base, const Value& key, const Value& value);
  bool get_named(const Value& base, StringView name, Value* out) { return get(base, intern(name), out); }
  // Property of an object itself (no prototype chain): false if it has none.
  bool get_own(JsObject* object, JsString* key, Value* out);
  bool has_property(JsObject* object, JsString* key);
  bool delete_property(JsObject* object, JsString* key);
  // Sets a property directly (replacing one that is there), with the given flags.
  void define(JsObject* object, JsString* key, const Value& value, uint8_t flags = kDefaultFlags);
  void define_named(JsObject* object, StringView name, const Value& value, uint8_t flags = kDefaultFlags) { define(object, intern(name), value, flags); }
  void define_native(JsObject* object, StringView name, NativeFn fn, uint32_t arity);
  bool array_push(JsObject* array, const Value& value);
  // A bound function's arguments are kept in its items.
  bool array_push_bound(JsObject* bound, const Value& value);
  // The operators the built-ins share with the interpreter (Opc::Add ... Opc::InstanceOf).
  bool binary_op_public(Opc op, const Value& a, const Value& b, Value* out) { return binary_op(op, a, b, out); }
  JsObject* error_proto_of(int kind) { return error_protos_[kind]; }
  bool array_set_length(JsObject* array, uint32_t length);
  // The own enumerable property names of an object (array indices first), as strings in a new array.
  JsObject* own_keys(JsObject* object);
  static bool is_callable(const Value& v) { return v.is_object() && (v.as_object()->okind == ObjKind::Function || v.as_object()->okind == ObjKind::Native || v.as_object()->okind == ObjKind::Bound); }

  // --- Conversions
  bool to_primitive(const Value& v, bool prefer_string, Value* out);
  bool to_string(const Value& v, JsString** out);
  bool to_number(const Value& v, double* out);
  static bool to_boolean(const Value& v);
  bool to_int32(const Value& v, int32_t* out);
  bool to_property_key(const Value& v, JsString** out);
  bool strict_equals(const Value& a, const Value& b);
  bool loose_equals(const Value& a, const Value& b);
  StringView type_of(const Value& v);
  // The text of a value for messages and console.log: never throws (objects show as [object Object], arrays as their elements).
  void describe(const Value& v, String* out, uint32_t depth = 0);

  // --- Well-known objects
  JsObject* global() { return global_; }
  JsObject* object_proto() { return object_proto_; }
  JsObject* array_proto() { return array_proto_; }
  JsObject* function_proto() { return function_proto_; }
  JsObject* string_proto() { return string_proto_; }
  JsObject* number_proto() { return number_proto_; }
  JsObject* boolean_proto() { return boolean_proto_; }
  JsObject* error_proto() { return error_proto_; }
  JsString* empty_string() { return empty_; }
  JsString* length_name() { return length_; }
  JsString* prototype_name() { return prototype_; }
  JsString* name_name() { return name_; }
  JsString* message_name() { return message_; }

  // --- Timers (setTimeout, setInterval, requestAnimationFrame): each is an entry [function, due time, id, interval or -1, arguments...] in an array on the
  // global object, which Engine runs. Times are in milliseconds of clock_ms(). Returns the id (0 if it could not be made).
  double add_timer(const Value& function, double delay_ms, double interval_ms, const Value* args, uint32_t argc);
  void clear_timer(double id);
  // The time scripts see: the host's clock plus what the engine has skipped over while running timers ahead of their time.
  double clock_ms() { return (host_ ? static_cast<double>(host_->now_ms()) : 0.0) + skew_ms_; }
  void set_skew(double skew_ms) { skew_ms_ = skew_ms; }
  double skew() const { return skew_ms_; }

  // --- For the host that installs objects: a pointer of its own, an own property (no host callbacks, no prototype chain) and a hold on the collector (the
  // collector does not run while anything holds it: for C++ code that keeps script values in its own variables across calls into scripts).
  void set_user(void* user) { user_ = user; }
  void* user() const { return user_; }
  bool get_plain(JsObject* object, JsString* key, Value* out);
  void hold_collector() { ++gc_hold_; }
  void release_collector() { if (gc_hold_ > 0) --gc_hold_; }

  // --- Called by the built-ins
  void collect();   // (only safe at the top level of the interpreter: the VM does this itself)
  uint32_t random_next();
  // Stops the script: the next instruction ends it with this message.
  void halt(StringView reason);
  // After a script was stopped (limits) or threw: clears the stack so that the next script can run.
  void recover();
  bool halted() const { return halted_; }

 private:
  friend bool install_builtins(Vm& vm);
  struct Frame {
    FunctionCode* code;
    uint32_t pc;
    Env* env;
    Value this_value;
    JsObject* callee;
    uint32_t base;           // the stack slot the callee was in; the result replaces everything from here
    uint32_t handlers;       // how many handlers there were when the call started
    bool construct;
    Value construct_this;
  };
  struct Handler {
    uint32_t pc;
    uint32_t stack;
    uint32_t frame;
    Env* env;
  };

  // heap
  void* alloc_cell(uint64_t size);
  void link(Cell* cell, uint64_t size);
  void mark(Cell* cell);
  void mark_value(const Value& v);
  void mark_object(JsObject* o);
  void trace();
  void sweep();
  void free_cell(Cell* cell);
  void rebuild_intern_table();
  void grow_props(JsObject* o);
  void index_rebuild(JsObject* o);
  int64_t find_prop(JsObject* o, JsString* key) const;
  bool array_index(const Value& key, uint32_t* index) const;
  static bool canonical_index(StringView s, uint32_t* index);
  bool ensure_items(JsObject* o, uint32_t capacity);

  // running
  bool execute(uint32_t entry_depth);
  bool call_value(const Value& function, const Value& this_value, const Value* args, uint32_t argc, Value* result, bool construct);
  bool push_frame(JsObject* function, const Value& this_value, const Value* args, uint32_t argc, uint32_t base, bool construct, const Value& construct_this);
  bool unwind(uint32_t entry_depth);
  bool binary_op(Opc op, const Value& a, const Value& b, Value* out);
  bool compare(const Value& a, const Value& b, bool* less, bool* undefined);
  bool instance_of(const Value& v, const Value& ctor, bool* out);
  bool has_in(const Value& key, const Value& object, bool* out);
  bool make_iterator(const Value& v, bool for_in, Value* out);
  bool iterator_next(JsObject* it, Value* out);
  bool spread_into(JsObject* array, const Value& iterable);
  bool lookup_global(JsString* name, Value* out);
  bool push(const Value& v);

  allocators::IAllocator& allocator_;
  IHost* host_;
  bool ok_ = false;
  // heap
  Cell* cells_ = nullptr;
  uint64_t heap_bytes_ = 0, gc_threshold_ = 4u << 20, memory_limit_ = 96u << 20;
  JsString** interned_ = nullptr;
  uint32_t interned_cap_ = 0, interned_count_ = 0;
  List<Cell*> gray_;
  List<FunctionCode*> codes_;
  bool halted_ = false, gc_wanted_ = false;
  String halt_reason_;
  // roots
  Value* stack_ = nullptr;
  uint32_t sp_ = 0;
  List<Frame> frames_;
  List<Handler> handlers_;
  Value exception_;
  bool has_exception_ = false;
  Value last_result_;
  JsObject *global_ = nullptr, *object_proto_ = nullptr, *array_proto_ = nullptr, *function_proto_ = nullptr, *string_proto_ = nullptr, *number_proto_ = nullptr,
           *boolean_proto_ = nullptr, *error_proto_ = nullptr, *iterator_proto_ = nullptr, *sentinel_ = nullptr;
  JsObject* error_protos_[5] = {};
  JsString *empty_ = nullptr, *length_ = nullptr, *prototype_ = nullptr, *name_ = nullptr, *message_ = nullptr, *constructor_ = nullptr, *to_string_ = nullptr,
           *value_of_ = nullptr, *undefined_text_ = nullptr, *null_text_ = nullptr, *true_text_ = nullptr, *false_text_ = nullptr;
  // limits
  uint64_t budget_ = 0, instructions_ = 0;
  uint32_t native_depth_ = 0;
  uint64_t random_state_ = 88172645463325252ull;
  double skew_ms_ = 0;
  void* user_ = nullptr;
  uint32_t gc_hold_ = 0;
};

// Defines the global functions and objects (Object, Array, String, Math, JSON, console, ...). Called by the Vm constructor.
bool install_builtins(Vm& vm);

}  // namespace script
