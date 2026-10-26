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


// ArrayBuffer, the typed arrays (Int8Array ... Float64Array, with Uint8ClampedArray) and DataView. The bytes of a buffer live in a string cell (so the collector
// owns them); a typed array is a view of them - an element type, a byte offset and a length - and answers index reads and writes itself. The methods that
// walk the elements (map, filter, sort ...) are written in script (Engine.cpp); what needs the bytes is here.
#pragma once
#include "Script/Vm.hpp"
#include "Utility/ArrayView.hpp"

namespace script {

enum class ArrayKind : uint8_t { Int8, Uint8, Uint8Clamped, Int16, Uint16, Int32, Uint32, Float32, Float64, BigInt64, BigUint64 };

bool install_typed_arrays(Vm& vm);

// A new typed array over a new buffer holding `bytes` (its length must be a multiple of the element size), or `length` zero elements.
JsObject* new_typed_array(Vm& vm, ArrayKind kind, StringView bytes);
JsObject* new_zero_typed_array(Vm& vm, ArrayKind kind, uint64_t length);
bool is_typed_array(const Value& v);
// The bytes a typed array, an ArrayBuffer or a DataView covers.
bool typed_array_bytes(const Value& v, ArrayView<uint8_t>* bytes);

// An ArrayBuffer over a cell made by Vm::new_bytes (the memory of a WebAssembly instance is one), and the end of its life: a detached buffer has no bytes (its
// byteLength is 0, and so is the length of every view of it).
JsObject* new_array_buffer(Vm& vm, JsString* cell);
void detach_array_buffer(JsObject* buffer);

// What the interpreter does with `array[index]`.
bool typed_array_get(JsObject* array, uint32_t index, Value* out);
// The same for any kind (a BigInt is made for BigInt64Array / BigUint64Array); false only with an exception pending.
bool typed_array_get_value(Vm& vm, JsObject* array, uint32_t index, Value* out);
bool typed_array_put(Vm& vm, JsObject* array, uint32_t index, const Value& value);
void typed_array_describe(Vm& vm, JsObject* array, String* out, uint32_t depth);

}  // namespace script
