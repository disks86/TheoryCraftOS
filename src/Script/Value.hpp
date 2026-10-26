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

// Values and heap cells of the script engine (a JavaScript subset; see Engine.hpp). A Value is a tagged 16-byte record: undefined, null, a boolean, a
// double, or a pointer to a heap cell (a string or an object). Cells live in the VM's heap and are freed by its mark-and-sweep collector; a Value that
// is not reachable from the VM's roots (stack, frames, globals) must not be kept across anything that can run script code.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/StringView.hpp"

namespace script {

struct Cell;
struct JsString;
struct JsObject;
struct Env;
struct FunctionCode;
class Vm;

enum class Tag : uint8_t { Undefined, Null, Boolean, Number, String, Object };

struct Value {
  Tag tag;
  union {
    bool boolean;
    double number;
    Cell* cell;
  } u;

  Value() : tag(Tag::Undefined) { u.number = 0; }
  static Value make_null() {
    Value v;
    v.tag = Tag::Null;
    return v;
  }
  static Value make_bool(bool b) {
    Value v;
    v.tag = Tag::Boolean;
    v.u.boolean = b;
    return v;
  }
  static Value make_number(double d) {
    Value v;
    v.tag = Tag::Number;
    v.u.number = d;
    return v;
  }
  static Value make_string(JsString* s) {
    Value v;
    v.tag = Tag::String;
    v.u.cell = reinterpret_cast<Cell*>(s);
    return v;
  }
  static Value make_object(JsObject* o) {
    Value v;
    v.tag = Tag::Object;
    v.u.cell = reinterpret_cast<Cell*>(o);
    return v;
  }

  bool is_undefined() const { return tag == Tag::Undefined; }
  // The value of a let / const / class binding that has not been initialized yet (its temporal dead zone): an undefined that carries a mark. It is only
  // ever in a variable slot, and only GetVar/SetVar of a lexical binding look at it.
  static Value make_uninitialized() {
    Value v;
    v.u.number = 1;
    return v;
  }
  bool is_uninitialized() const { return tag == Tag::Undefined && u.number == 1; }
  bool is_null() const { return tag == Tag::Null; }
  bool is_nullish() const { return tag == Tag::Undefined || tag == Tag::Null; }
  bool is_bool() const { return tag == Tag::Boolean; }
  bool is_number() const { return tag == Tag::Number; }
  bool is_string() const { return tag == Tag::String; }
  bool is_object() const { return tag == Tag::Object; }
  JsString* as_string() const { return reinterpret_cast<JsString*>(u.cell); }
  JsObject* as_object() const { return reinterpret_cast<JsObject*>(u.cell); }
};

enum class CellKind : uint8_t { String, Object, Env };

struct Cell {
  Cell* next = nullptr;   // the heap's list of every cell
  CellKind kind;
  bool marked = false;
  explicit Cell(CellKind k) : kind(k) {}
};

// An immutable string of bytes (the page's text is ASCII; other bytes are kept as they are), NUL-terminated after `length`.
struct JsString : Cell {
  uint32_t length = 0;
  uint32_t hash = 0;
  bool interned = false;   // property names are interned: two equal names are the same cell
  bool symbol = false;     // a Symbol: a string cell that scripts see as a symbol (typeof, no conversion to string or number, hidden from the string property lists); its text is "@@<description>#<n>"
  bool described = false;  // a symbol made with a description (Symbol('') is not Symbol())
  // What the bytes mean to a script (looked at when something asks for the length in characters or an index; see Vm::string_kind):
  //   ascii: every byte is a character; text: valid UTF-8 (a lone surrogate is kept as its three bytes), a character is a UTF-16 code unit;
  //   binary: raw bytes made by __bytes_to_string, atob and the like (or anything that is not valid UTF-8): every byte is a character, its value.
  uint8_t kind = 0;        // 0 not looked at yet, 1 ascii, 2 text, 3 binary
  bool binary = false;     // made from raw bytes: kind 3 whatever the bytes are
  uint32_t units = 0;      // the length in characters (UTF-16 code units for text), once kind is set
  uint32_t map_unit = 0, map_byte = 0;   // the last place a character offset was turned into a byte offset (text only): the next one starts from there
  uint16_t* wide = nullptr;   // the code units of a text string, made when a script indexes into it (freed with the string)
  JsString() : Cell(CellKind::String) {}
  char* data() { return reinterpret_cast<char*>(this + 1); }
  const char* data() const { return reinterpret_cast<const char*>(this + 1); }
  StringView view() const { return StringView(data(), length); }
};

// The variables of one call of a function (and of the blocks in it): the closures made in it keep the Env alive.
struct Env : Cell {
  Env* parent = nullptr;
  uint32_t count = 0;
  Env() : Cell(CellKind::Env) {}
  Value* slots() { return reinterpret_cast<Value*>(this + 1); }
};

// A Promise keeps its state in `position` (kPromiseState bits; kPromiseHandled once a reaction was attached), its value or reason in `bound_this` and its
// reactions in `items` (three values each: what to call if it is fulfilled, if it is rejected, and the promise that follows). A Coroutine is a suspended
// async function (host_data points to its saved frame, see Vm.cpp). A native function may keep values of its own in `items`.
enum class ObjKind : uint8_t { Plain, Array, Function, Native, Bound, Error, Iterator, Promise, Coroutine, RegExp, TypedArray, Proxy, BigInt };   // (a BigInt keeps its sign and its decimal digits in the hidden properties __neg and __mag; the arithmetic is in script, see Stdlib.cpp)   // (a Proxy keeps its target in `target` and its handler in `bound_this`; a revoked one has no target)

enum PromiseBits : uint32_t { kPromisePending = 0, kPromiseFulfilled = 1, kPromiseRejected = 2, kPromiseState = 3, kPromiseHandled = 4 };

enum PropFlag : uint8_t { kEnumerable = 1, kWritable = 2, kDefaultFlags = 3, kAccessor = 4, kNonConfigurable = 8 };   // (an accessor's value is a two element array: getter, setter)

struct Prop {
  JsString* key;
  Value value;
  uint8_t flags;
};

// Natives return false after throwing (Vm::throw_error); `args` may be shorter than the function asks for.
using NativeFn = bool (*)(Vm& vm, const Value& this_value, const Value* args, uint32_t argc, Value* result);

// Objects that stand for something outside the script engine (a document node, a style declaration) answer property reads and writes themselves. The
// callbacks see every key first; a read that returns true has produced the value, a write returns what it did.
struct HostClass {
  const char* name;
  // Reads a property. True if it is the object's own (then *out is the value); false lets the ordinary properties and the prototype chain answer.
  bool (*get)(Vm& vm, JsObject* object, JsString* key, Value* out);
  // Writes a property.
  enum class Set : uint8_t { NotHandled, Handled, Threw };
  Set (*set)(Vm& vm, JsObject* object, JsString* key, const Value& value);
  // Deletes a property; true if it was handled (the result of the delete is then *deleted).
  bool (*remove)(Vm& vm, JsObject* object, JsString* key, bool* deleted);
  // Appends the names of the enumerable properties it has (as strings) to `keys`.
  void (*keys)(Vm& vm, JsObject* object, JsObject* keys);
};

// JsObject::position of an array that is a function's `arguments` object.
constexpr uint32_t kArgumentsMark = 0xA46;

struct JsObject : Cell {
  ObjKind okind = ObjKind::Plain;
  bool frozen = false;         // Object.freeze (also set on an iterator object whose next() is called: see Vm.cpp)
  bool non_extensible = false; // Object.preventExtensions: no property can be added
  bool sealed = false;         // Object.seal: ... and none can be deleted or reconfigured
  bool constructs = false;     // a native that `new` accepts although it has no prototype property (Proxy); other natives are methods
  JsObject* proto = nullptr;
  Prop* props = nullptr;
  uint32_t prop_count = 0, prop_cap = 0;
  uint32_t* index = nullptr;   // a hash index over props when there are many of them (slots hold prop number + 1)
  uint32_t index_cap = 0;
  // Arrays (elements), bound functions (the bound arguments) and iterators (the keys to visit).
  Value* items = nullptr;
  uint32_t length = 0, capacity = 0;
  // Functions.
  FunctionCode* code = nullptr;
  Env* env = nullptr;
  Value bound_this;       // an arrow function's `this`, a bound function's `this`, an iterator's source
  JsObject* target = nullptr;   // a bound function's function
  NativeFn native = nullptr;
  uint32_t position = 0;  // an iterator's place
  const HostClass* host = nullptr;   // objects that stand for something outside the engine
  void* host_data = nullptr;
  JsObject() : Cell(CellKind::Object) {}
};

}  // namespace script
