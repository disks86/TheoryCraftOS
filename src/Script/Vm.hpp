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
  // Counts instructions run by native code (a WebAssembly interpreter) against the budget. False, with the script stopped, when it is spent.
  bool charge(uint64_t instructions) {
    if (!budget_) return true;
    instructions_ += instructions;
    if (instructions_ > budget_) {
      halt("the script ran too long");
      return false;
    }
    return true;
  }
  uint64_t heap_bytes() const { return heap_bytes_; }
  uint64_t memory_limit() const { return memory_limit_; }

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
  // Strings as scripts see them: text in UTF-16 code units, raw bytes as bytes (see JsString::kind).
  uint8_t string_kind(JsString* s);
  uint32_t string_length(JsString* s) { return string_kind(s) ? s->units : 0; }
  const uint16_t* string_wide(JsString* s);   // the code units of a text string (null for ascii and binary, where the bytes are the characters)
  JsString* string_from_units(const uint16_t* units, uint32_t count);
  JsString* string_part(JsString* s, uint32_t from, uint32_t to);   // the characters from..to (clamped) as a string
  uint32_t string_unit_at(JsString* s, uint32_t index);
  uint64_t string_byte_of_unit(JsString* s, uint64_t unit);       // the byte offset of a character offset (for ascii and binary strings the same number)
  uint64_t string_unit_of_byte(JsString* s, uint64_t byte);       // and the other way round             // the character at index (< length)
  JsString* new_binary_string(StringView bytes);                    // raw bytes that scripts see as characters 0..255
  // A string of `length` zero bytes (the storage of an ArrayBuffer).
  JsString* new_bytes(uint32_t length);
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
  bool put(const Value& base, JsString* key, const Value& value) { return put_ex(base, key, value, false); }
  bool put_member(const Value& base, const Value& key, const Value& value) { return put_member_ex(base, key, value, false); }
  // The same; in strict mode code a write that does not happen (read-only, no setter, not extensible) throws a TypeError.
  bool put_ex(const Value& base, JsString* key, const Value& value, bool strict);
  bool put_member_ex(const Value& base, const Value& key, const Value& value, bool strict);
  // True if the script function that is running (the one that called the native) is the host's own.
  bool in_library_code() const { return frames_.count() > 0 && frames_[frames_.count() - 1].code->library; }
  bool get_named(const Value& base, StringView name, Value* out) { return get(base, intern(name), out); }
  // Property of an object itself (no prototype chain): false if it has none.
  bool get_own(JsObject* object, JsString* key, Value* out);
  bool has_property(JsObject* object, JsString* key);
  bool delete_property(JsObject* object, JsString* key);
  // Changes the flag bits in `mask` of an own property to those in `bits` (the property keeps its other flags).
  void set_property_flags(JsObject* object, JsString* key, uint8_t bits, uint8_t mask);
  // Sets a property directly (replacing one that is there), with the given flags.
  void define(JsObject* object, JsString* key, const Value& value, uint8_t flags = kDefaultFlags);
  // A property with a getter and / or a setter (undefined: leave that part as it is). `flags` may carry kEnumerable.
  void define_accessor(JsObject* object, JsString* key, const Value& getter, const Value& setter, uint8_t flags = kEnumerable);
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
  // Appends the values an iterable gives to `array`, at most `max` of them (negative: all). False if it is not iterable or an iterator threw.
  bool iterate_into(JsObject* array, const Value& iterable, int64_t max);
  // Every own property name (also the ones that are not enumerable), and the value and attributes of one own property (a getter / setter is not called).
  JsObject* own_names(JsObject* object);
  bool own_property(JsObject* object, JsString* key, Value* value, uint8_t* flags);
  static bool callable_object(const JsObject* o) {
    for (int depth = 0; o && depth < 16; ++depth) {
      if (o->okind == ObjKind::Function || o->okind == ObjKind::Native || o->okind == ObjKind::Bound) return true;
      if (o->okind != ObjKind::Proxy) return false;
      o = o->target;   // (a proxy can be called when what it stands for can)
    }
    return false;
  }
  static bool is_callable(const Value& v) { return v.is_object() && callable_object(v.as_object()); }
  // Proxy: new Proxy(target, handler). The traps (get, set, has, deleteProperty, ownKeys, apply, construct) are asked; what has none goes to the target.
  JsObject* new_proxy(JsObject* target, const Value& handler);
  // BigInt: arbitrary precision integers, objects of kind BigInt (see Stdlib.cpp for the arithmetic; the operators call it).
  static bool is_bigint(const Value& v) { return v.is_object() && v.as_object()->okind == ObjKind::BigInt; }
  JsObject* new_bigint(bool negative, StringView digits);
  bool bigint_parts(const Value& v, bool* negative, StringView* digits);
  static void revoke_proxy(JsObject* proxy) { proxy->target = nullptr; }

  // --- Conversions
  bool to_primitive(const Value& v, bool prefer_string, Value* out);
  // hint: 0 default (the + operator, ==), 1 string, 2 number; an object's [Symbol.toPrimitive] is asked first
  bool to_primitive_hint(const Value& v, int hint, Value* out);
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

  // --- Promises and microtasks. A promise settles once; what is attached to it with promise_then() runs as a microtask. Microtasks run when the outermost
  // call into the VM (run(), call()) has finished, before control goes back to the host - that is when `await` continues and `then` callbacks run.
  JsObject* promise_proto() { return promise_proto_; }
  JsObject* new_promise();
  static bool is_promise(const Value& v) { return v.is_object() && v.as_object()->okind == ObjKind::Promise; }
  // The state of a promise: 0 pending, 1 fulfilled, 2 rejected; `value` gets the result or the reason.
  static uint32_t promise_state(const JsObject* p) { return p->position & kPromiseState; }
  static Value promise_value(const JsObject* p) { return p->bound_this; }
  // Resolves it with a value (a promise or a thenable is followed), fulfils it or rejects it. Does nothing if it has settled.
  void promise_resolve(JsObject* promise, const Value& value);
  void promise_fulfill(JsObject* promise, const Value& value);
  void promise_reject(JsObject* promise, const Value& reason);
  // Attaches callbacks (functions, or undefined to pass the result on); `derived` (may be null) is settled with what they return.
  void promise_then(JsObject* promise, const Value& on_fulfilled, const Value& on_rejected, JsObject* derived);
  // A promise already resolved with `value` (`value` itself if it is a promise).
  JsObject* promise_resolved(const Value& value);
  // The `resolve` / `reject` function of a promise; `shared` is a plain object that the pair of them share (once one has been called, the other does nothing).
  JsObject* resolving_function(JsObject* promise, JsObject* shared, bool reject);
  // Takes the pending exception as a value (and clears it).
  void take_exception_value(Value* out) {
    *out = exception_;
    has_exception_ = false;
    exception_ = Value();
  }
  // Runs `function` (no arguments) after the current script.
  void enqueue_microtask(const Value& function);
  // Runs the queued microtasks now (what the outermost call does by itself).
  void run_microtasks();
  bool microtasks_pending() const { return micro_ && micro_head_ < micro_->length; }
  // A native function that keeps values of its own: they are in current_native()->items while it runs (read them before calling anything else).
  JsObject* new_native_with(NativeFn fn, StringView name, uint32_t arity, const Value* data, uint32_t count);
  JsObject* current_native() { return current_native_; }

  // --- For the host that installs objects: a pointer of its own, an own property (no host callbacks, no prototype chain) and a hold on the collector (the
  // collector does not run while anything holds it: for C++ code that keeps script values in its own variables across calls into scripts).
  void set_user(void* user) { user_ = user; }
  void* user() const { return user_; }
  // A second pointer for a part of the host that has state of its own (the WebAssembly store) next to the one `user` holds.
  void set_extension(void* extension) { extension_ = extension; }
  void* extension() const { return extension_; }
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
  // The next function frame is a parent constructor called by super(...) (see new.target).
  void mark_super_call() { super_pending_ = true; }
  // The host can ask for a call of `hook` as soon as the native function (or host property write) that is running has returned: request_epilogue().
  void set_epilogue(void (*hook)(void*), void* user) {
    epilogue_ = hook;
    epilogue_user_ = user;
  }
  void request_epilogue() { epilogue_pending_ = true; }
  // Lets the host make the object `new` starts with (a custom element class makes a document node): given the prototype, an object or nullptr for the usual one.
  void set_construct_hook(JsObject* (*hook)(void* user, JsObject* proto), void* user) {
    construct_hook_ = hook;
    construct_user_ = user;
  }
  // generator.next(value) / .throw(error) / .return(value): the object { value, done }. False if the generator threw.
  bool generator_resume(const Value& generator, int mode, const Value& input, Value* result);
  static bool is_generator_object(const Value& v);
  // The exception that carries a generator's return() out through its finally blocks.
  bool is_generator_return(const Value& v);
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
    bool via_super;          // the frame is a parent class constructor that super(...) called: new.target is the class of `this`
    Value construct_this;
    JsObject* coroutine;     // the async function this frame is running (its state is saved here at an `await`)
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
  // Leaves an iteration early: calls the iterator's return() if it has one. `silent`: an exception from it is dropped (another one is already on its way).
  bool iterator_close(JsObject* it, bool silent);
  bool throw_uninitialized(FunctionCode* code, uint16_t mark);   // ReferenceError for a let / const read or written in its temporal dead zone
  bool spread_into(JsObject* array, const Value& iterable);
  bool has_global(JsString* name);
  bool lookup_global(JsString* name, Value* out);
  bool push(const Value& v);
  // async functions
  struct Coroutine;
  Coroutine* coroutine_of(JsObject* co) const;
  bool start_async(JsObject* function, const Value& this_value, const Value* args, uint32_t argc, uint32_t base, Value* result);
  bool run_coroutine(JsObject* co, bool start, bool throwing, const Value& input);
  bool start_generator(JsObject* function, const Value& this_value, const Value* args, uint32_t argc, Value* result);
  // Runs a generator up to its next yield (mode 0 next, 1 throw; 2 return ends it): false if it threw.
  bool run_generator(JsObject* co, bool start, int mode, const Value& input, Value* value, bool* done);
  void async_wait(JsObject* co, const Value& awaited);
  // promises
  void promise_settle(JsObject* promise, uint32_t state, const Value& value);
  void promise_job(const Value& handler, const Value& argument, const Value& derived, uint32_t kind);
  void enqueue_job(const Value& handler, const Value& argument, const Value& derived, uint32_t kind);
  void report_rejections();
  void checkpoint();

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
  Value receiver_;                       // the object a property is being read from, while get() looks for it (what a getter sees as `this`)
  bool has_receiver_ = false, getter_failed_ = false;
  bool super_pending_ = false;
  void (*epilogue_)(void*) = nullptr;
  void* epilogue_user_ = nullptr;
  bool epilogue_pending_ = false, in_epilogue_ = false;
  void run_epilogue();
  template <class F>
  void keeping(Value* v, F&& run);
  JsObject* (*construct_hook_)(void*, JsObject*) = nullptr;
  void* construct_user_ = nullptr;
  JsObject* new_instance(JsObject* proto);
  bool accessor_value(const Value& pair, Value* out);
  bool bigint_operation(const char* op, const Value& a, const Value& b, Value* out);
  bool proxy_parts(JsObject* proxy, const char* operation, JsObject** target, Value* handler);
  bool proxy_trap(JsObject* proxy, const char* name, JsObject** target, Value* handler, Value* trap);
  bool proxy_get(JsObject* proxy, JsString* key, const Value& receiver, Value* out);
  bool proxy_set(JsObject* proxy, JsString* key, const Value& value, const Value& receiver);
  bool proxy_has(JsObject* proxy, JsString* key, bool* out);
  bool proxy_delete(JsObject* proxy, JsString* key, bool* out);
  JsObject* proxy_keys(JsObject* proxy, bool all);
  bool proxy_call(JsObject* proxy, const Value& this_value, const Value* args, uint32_t argc, Value* result, bool construct);
  bool call_setter(const Value& pair, const Value& receiver, const Value& value);
  String halt_reason_;
  // roots
  Value* stack_ = nullptr;
  uint32_t sp_ = 0;
  List<Frame> frames_;
  List<Handler> handlers_;
  Value exception_;
  bool has_exception_ = false;
  uint32_t throw_line_ = 0;   // the source line an exception was thrown from (kept for debugging builds: TCOS_ERROR_LINES prints it)
  Value last_result_;
  Value iter_return_;   // the value an iterator finished with (done: true, value: ...): what `yield*` evaluates to
  JsObject *global_ = nullptr, *object_proto_ = nullptr, *array_proto_ = nullptr, *function_proto_ = nullptr, *string_proto_ = nullptr, *number_proto_ = nullptr,
           *boolean_proto_ = nullptr, *error_proto_ = nullptr, *iterator_proto_ = nullptr, *sentinel_ = nullptr;
  JsObject* error_protos_[5] = {};
  JsObject *promise_proto_ = nullptr, *micro_ = nullptr, *rejections_ = nullptr, *current_native_ = nullptr;
  uint32_t micro_head_ = 0;
  bool draining_ = false, suspended_ = false;
  uint32_t async_depth_ = 0;
  Value await_value_;
  JsString *empty_ = nullptr, *length_ = nullptr, *prototype_ = nullptr, *name_ = nullptr, *message_ = nullptr, *constructor_ = nullptr, *to_string_ = nullptr,
           *value_of_ = nullptr, *undefined_text_ = nullptr, *null_text_ = nullptr, *true_text_ = nullptr, *false_text_ = nullptr;
  // limits
  uint64_t budget_ = 0, instructions_ = 0;
  uint32_t native_depth_ = 0;
  uint64_t random_state_ = 88172645463325252ull;
  double skew_ms_ = 0;
  void* user_ = nullptr;
  void* extension_ = nullptr;
  uint32_t gc_hold_ = 0;
};

// Defines the global functions and objects (Object, Array, String, Math, JSON, console, ...). Called by the Vm constructor.
bool install_builtins(Vm& vm);

}  // namespace script
