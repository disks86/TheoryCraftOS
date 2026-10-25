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

// What the interpreter's files share (ShaderMachine.cpp runs the tree, ShaderBuiltins.cpp has the built-in functions).
#pragma once
#include "WebGl/Shader.hpp"

namespace shader {

inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }
inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline int32_t u2i(uint32_t u) { return static_cast<int32_t>(u); }

// A value being read: slots and their type (the slots may be a variable or a temporary).
struct Ref {
  const uint32_t* p = nullptr;
  Type t;
};
// A place being written: a whole value, or (n > 0) the components swz[0..n) of the vector at p.
struct LRef {
  uint32_t* p = nullptr;
  Type t;
  uint8_t swz[4] = {0, 0, 0, 0};
  uint8_t n = 0;
};

inline uint32_t count_of(const Type& t) { return static_cast<uint32_t>(t.cols) * t.rows; }

uint32_t convert_scalar(uint32_t v, Base from, Base to);

struct Interp {
  Machine& m;
  const Module& mod;
  uint32_t* wr[12] = {};   // the writable place of a pointer argument of a built-in (atomics, modf ...)
  explicit Interp(Machine& machine) : m(machine), mod(machine.module_) {}

  bool fail(const char* why);
  uint32_t* alloc(uint32_t slots);
  bool new_value(const Type& t, Ref* out, uint32_t** writable);

  // expressions
  bool eval(int32_t node, Ref* out);
  bool lvalue(int32_t node, LRef* out);
  bool store(const LRef& dst, const Ref& src);
  bool binary(Op op, const Ref& a, const Ref& b, Ref* out);
  bool unary(Op op, const Ref& a, Ref* out);
  bool construct(const Type& t, const Ref* args, uint32_t n, Ref* out);
  bool convert_to(const Ref& src, Base to, Ref* out);
  bool field_of(const Ref& base, const Node& node, Ref* out);
  bool call_user(int32_t fn, const Node& node, Ref* out);
  bool tex_call(const Node& node, Ref* out);
  bool call_builtin(const Node& node, Ref* out);
  bool builtin(BuiltinFn fn, const Ref* args, uint32_t n, Ref* out, const Node& node);

  // statements
  Machine::Status run(uint32_t base);
  Machine::Status exec_step(uint32_t base);
  bool push_frame(int32_t node);
  bool unwind(bool is_continue);
  Machine::Status begin(int32_t fn, ArrayView<const uint32_t* const> args);

  bool read_swizzle(const Node& node, uint32_t* idx, uint32_t* n) const;
  uint32_t slots(const Type& t) const { return mod.slots_of(t); }
};

}  // namespace shader
