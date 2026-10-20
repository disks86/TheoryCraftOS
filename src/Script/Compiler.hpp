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

// Turns the syntax tree of a program into bytecode (see Bytecode.hpp). Variables are resolved here: a function's variables (its parameters, its `var`s
// and the `let` / `const` of its blocks, each block variable with a slot of its own) live in one Env per call, and a name that is not found in any
// enclosing function is a global. Top-level `var`, functions, `let` and `const` are properties of the global object, so scripts share them.
#pragma once
#include "Script/Ast.hpp"
#include "Script/Vm.hpp"

namespace script {

// Compiles `program` (a Block node from parse_program). The code returns the value of the last expression statement. False with *error set when
// the program cannot be compiled (too many nested functions, ...).
bool compile_program(Vm& vm, Node* program, FunctionCode** out, String* error);

}  // namespace script
