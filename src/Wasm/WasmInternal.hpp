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

// What the interpreter and the SIMD operations share.
#pragma once
#include "Wasm/Wasm.hpp"

namespace wasm {

// The messages of traps (the texts V8 uses, so that scripts written against it see the same).
constexpr const char* kTrapUnreachable = "unreachable";
constexpr const char* kTrapMemory = "memory access out of bounds";
constexpr const char* kTrapDivZero = "divide by zero";
constexpr const char* kTrapRemZero = "remainder by zero";
constexpr const char* kTrapDivOverflow = "divide result unrepresentable";
constexpr const char* kTrapConversion = "float unrepresentable in integer range";
constexpr const char* kTrapTable = "table index is out of bounds";
constexpr const char* kTrapNullFunction = "null function or function signature mismatch";
constexpr const char* kTrapSignature = "null function or function signature mismatch";
constexpr const char* kTrapStack = "Maximum call stack size exceeded";
constexpr const char* kTrapSegment = "out of bounds";
constexpr const char* kTrapDataSegment = "data segment out of bounds";
constexpr const char* kTrapElemSegment = "element segment out of bounds";
constexpr const char* kTrapOutOfMemory = "out of memory";

// Runs the SIMD operation `sub` (0xFD sub-opcode) on the stack whose top is `sp` (advanced or retreated as the operation pushes and pops). `*extra` is how many
// instructions after `in` it consumed (a 128 bit constant or shuffle has its upper half in the next one). Returns a trap message or null.
const char* simd_exec(uint32_t sub, const Instr* in, Slot*& sp, Instance& instance, uint32_t* extra);

}  // namespace wasm
