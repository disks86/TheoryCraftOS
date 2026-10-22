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


// Text and bytes are the same memory seen two ways: StringView over char, ByteView over uint8_t. These are the only places that turn one into the other,
// so the rest of the code needs no cast. It is safe because char and uint8_t have the same size and no alignment need, the view keeps its length, and
// the memory is the same one (its lifetime is the one of what was passed in).
#pragma once
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

inline StringView bytes_as_text(ConstByteView bytes) { return StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
inline ConstByteView text_as_bytes(StringView text) { return ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }
// The characters of a string cell as writable bytes (typed arrays keep their bytes in one).
inline uint8_t* chars_as_bytes(char* chars) { return reinterpret_cast<uint8_t*>(chars); }
