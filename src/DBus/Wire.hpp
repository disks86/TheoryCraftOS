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

// The D-Bus wire format (the "D-Bus Specification", version 0.43): the names and signatures it allows, and the marshalling of values.
//
// Values are aligned to their own size from the start of the message (the body starts on an 8-byte boundary, so a body read or written on its own aligns
// the same way). Both byte orders are read; the bus writes its own messages little-endian and the headers of forwarded ones in the order they came in.
// Everything that comes from a connection is checked before it is used: names, signatures (at most 255 bytes, 32 nested arrays and 32 nested structs),
// strings (UTF-8 without NUL), booleans (0 or 1), padding (zero), array lengths (at most 64 MiB, a whole number of elements) and the nesting of variants
// (at most 64 containers in all).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace dbus {

inline constexpr uint32_t kMaxArrayBytes = 1u << 26;        // 64 MiB
inline constexpr uint32_t kMaxMessageBytes = 1u << 27;      // 128 MiB, header and body
inline constexpr uint32_t kMaxNameBytes = 255;
inline constexpr uint32_t kMaxSignatureBytes = 255;
inline constexpr uint32_t kMaxSignatureDepth = 32;          // arrays, and structs, each
inline constexpr uint32_t kMaxValueDepth = 64;              // containers in a value, variants included

// --- names
bool valid_object_path(StringView path);
bool valid_interface_name(StringView name);       // also an error name
bool valid_member_name(StringView name);
bool valid_bus_name(StringView name);             // unique (":1.5") or well-known ("org.example.App")
inline bool is_unique_name(StringView name) { return name.starts_with(':'); }
bool valid_utf8(ConstByteView bytes);

// --- signatures
// One complete type starting at `at`: *end is the position after it. False if there is none or it is malformed.
bool complete_type(StringView signature, uint64_t at, uint64_t* end);
bool valid_signature(StringView signature);                  // a run of complete types (empty is valid)
bool single_complete_type(StringView signature);             // exactly one (a variant's)
uint32_t alignment_of(char type_code);                       // 1, 2, 4 or 8 (1 for an unknown code)
bool is_basic_type(char type_code);

// Reads values of one byte order, aligned from the start of `data` (`data` is the whole message, or the body). Every call checks what it reads; once one
// has failed the reader stays failed.
class Reader {
 public:
  Reader(ConstByteView data, bool big_endian) : data_(data), big_endian_(big_endian) {}
  uint64_t position() const { return position_; }
  bool at_end() const { return position_ == data_.size(); }
  bool failed() const { return failed_; }
  // Skips to the next multiple of `alignment`; the padding must be zero.
  bool align(uint32_t alignment);
  bool u8(uint8_t* out);
  bool u16(uint16_t* out);
  bool u32(uint32_t* out);
  bool u64(uint64_t* out);
  bool boolean(bool* out);
  bool string(StringView* out);            // 's': u32 length, UTF-8 bytes, NUL
  bool object_path(StringView* out);       // 'o'
  bool signature(StringView* out);         // 'g': u8 length, bytes, NUL
  // An array's header: its length (checked against kMaxArrayBytes and what is left) and the padding to its first element. *end: where it ends.
  bool begin_array(uint32_t element_alignment, uint64_t* end);
  bool skip(uint64_t count);

 private:
  bool fail() { failed_ = true; return false; }
  bool take(uint64_t count, ConstByteView* out);
  ConstByteView data_;
  bool big_endian_;
  uint64_t position_ = 0;
  bool failed_ = false;
};

// Writes values of one byte order onto the end of a buffer, aligned from where the buffer ended when the writer was made. Failure (memory, an array too long) sticks: check ok()
// when done.
class Writer {
 public:
  explicit Writer(ByteBuffer& out, bool big_endian = false) : out_(out), big_endian_(big_endian), base_(out.size()) {}
  bool ok() const { return ok_; }
  void pad(uint32_t alignment);
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void boolean(bool v) { u32(v ? 1 : 0); }
  void string(StringView s);
  void object_path(StringView s) { string(s); }
  void signature(StringView s);
  void raw(ConstByteView bytes);
  // An array: begin_array() writes the length's place and the padding before the first element; end_array() fills the length in.
  struct ArrayMark {
    uint64_t length_at = 0, start = 0;
  };
  ArrayMark begin_array(uint32_t element_alignment);
  void end_array(const ArrayMark& mark);
  void begin_struct() { pad(8); }
  // A variant: its signature, then the caller writes the one value.
  void variant(StringView signature) { this->signature(signature); }
  uint64_t size() const { return out_.size() - base_; }

 private:
  void put(uint64_t v, uint32_t bytes);
  ByteBuffer& out_;
  bool big_endian_;
  uint64_t base_;
  bool ok_ = true;
};

// Checks (and steps over) one value of the single complete type `type`.
bool read_value(Reader& r, StringView type, uint32_t unix_fds);

// Checks that `body` holds exactly the values of `signature`, in that byte order: every rule above, and 'h' indices below `unix_fds`.
bool validate_body(ConstByteView body, StringView signature, bool big_endian, uint32_t unix_fds);

// The top-level arguments of a body that are strings or object paths ('s', 'o'), as the match rules' argN and argNpath look at them: (*out)[i] is the
// i-th argument when it is one, else empty with (*kinds)[i] = 0. At most `count` arguments are looked at. The body must have been validated.
struct StringArgs {
  static constexpr uint32_t kMax = 64;
  StringView value[kMax];
  char kind[kMax] = {};      // 's', 'o' or 0
  uint32_t count = 0;
};
void string_args(ConstByteView body, StringView signature, bool big_endian, uint32_t wanted, StringArgs* out);

}  // namespace dbus
