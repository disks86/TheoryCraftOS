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

// What the compiler produces and the VM runs: a stack machine. Each function is a FunctionCode - a list of instructions with their constants, its
// nested functions, and what the VM needs to set up a call (how many variable slots, which hold the parameters).
#pragma once
#include "ObjectContainer/List.hpp"
#include "Script/Value.hpp"

namespace script {

enum class Opc : uint8_t {
  // constants
  PushUndef, PushNull, PushTrue, PushFalse, PushNum, PushStr, PushInt, PushThis, NewTarget,
  // the stack
  Pop, Dup, Dup2, DupX1, DupX2, Swap,
  // variables: GetVar/SetVar b = how many functions out, a = slot (SetVar leaves the value on the stack); globals by name constant a
  GetVar, SetVar, GetGlobal, SetGlobal, TypeofGlobal, DeclareGlobal,
  // properties: [obj] -> ..., by name constant a or by a key on the stack
  GetProp, SetProp, GetElem, SetElem, DeleteProp, DeleteElem, GetMethod, GetMethodElem,
  // calls: [fn, this, args...] a = argument count; the Spread forms take [fn, this, argsArray]; New takes [fn, placeholder, args...]
  Call, CallSpread, New, NewSpread,
  // operators
  Add, Sub, Mul, Div, Mod, Pow, BitAnd, BitOr, BitXor, Shl, Shr, UShr, Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge, In, InstanceOf,
  Not, Neg, Plus, BitNot, Typeof, Void, Inc, Dec, ToNumber, ToStr,
  // jumps (a = target); the Keep forms leave the value when they jump and drop it when they do not
  Jump, JumpIfFalse, JumpIfTrue, JumpIfFalseKeep, JumpIfTrueKeep, JumpIfNotNullishKeep,
  // functions and objects
  Closure, Return, Throw, NewObject, DefineProp, SetLiteralProto, DefineElem, DefineAccessor, DefineAccessorElem, NewArray, ArrayPush, ArraySpread, ObjectSpread,
  // loops over keys / values: Start turns [value] into [iterator]; IterNext (a = where to go at the end) takes [iterator] and gives [value]
  ForInStart, ForOfStart, IterNext,
  // exceptions: TryBegin (a = the handler) starts catching; TryEnd stops; the handler is entered with the exception on the stack
  TryBegin, TryEnd,
  // at the start of a catch clause: if the exception on the stack is the completion of a generator's return(), jump to a (the finally block, then the throw goes on)
  CheckReturn,
  // async functions: [value] -> (the function is suspended until the value is settled; then the result, or a throw, takes the place of the value)
  Await,
  // generators: [value] -> (the generator is suspended; the value goes to whoever called next(); the argument of the next next() takes its place)
  Yield,
  // [iterator] -> (): calls the return() of an iterator that is left early (b = 1: an exception is on its way, so one thrown by return() is dropped)
  IterClose,
  // a = slot, b = how many Envs out: the let / const / class variable there is in its temporal dead zone (until it is initialized)
  TdzMark,
  // block scopes whose variables closures capture: PushEnv (a = variables) makes a new Env inside the current one, PopEnv leaves it, CopyEnv replaces the
  // current one with a copy (a `for (let ...)` loop gives each iteration its own variables)
  PushEnv, PopEnv, CopyEnv,
  // [] -> [value]: what the iterator that just finished returned (the value of `yield*`)
  IterReturn,
};

// What the 16 spare bits of GetVar / SetVar say: 0 plain; kConstAssign (SetVar of a const: a TypeError); n >= kChecked: the variable is let / const / class and
// may be in its temporal dead zone (n - kChecked is the constant holding its name, for the message).
constexpr uint16_t kConstAssign = 1, kChecked = 2;

struct Instr {
  Opc op;
  uint8_t b;
  uint16_t unused;
  int32_t a;
};

struct FunctionCode {
  explicit FunctionCode(allocators::IAllocator& allocator) : code(allocator), lines(allocator), constants(allocator), children(allocator) {}
  List<Instr> code;
  List<uint32_t> lines;                 // the source line of each instruction
  List<Value> constants;                // numbers and (interned) strings
  List<FunctionCode*> children;         // the functions made inside this one
  uint32_t slot_count = 0;              // variables (parameters first)
  uint32_t param_count = 0;
  uint32_t expected_args = 0;           // the function's length: the parameters before the first default value or rest parameter
  int32_t arguments_slot = -1;          // where `arguments` goes, if the function uses it
  int32_t self_slot = -1;               // where a named function expression finds itself
  bool is_arrow = false, has_rest = false, is_async = false, is_generator = false;
  bool strict = false;                  // strict mode code: `this` is not made the global object, failed writes and deletes throw, assigning to an undeclared name is an error
  bool library = false;                 // the host's own scripts (the preludes): Object.defineProperty leaves a property configurable unless told otherwise
  JsString* name = nullptr;
};

}  // namespace script
