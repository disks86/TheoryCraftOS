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

// SPA PODs ("plain old data"), the values PipeWire's native protocol carries: every value is a header (u32 body size, u32 type) and a body, padded to 8
// bytes, little endian. Containers hold other PODs: a Struct a run of them, an Object typed properties (key, flags, value), an Array values of one type
// packed after one child header, a Choice the same with a choice kind in front.
//
// The Builder appends to a ByteBuffer (containers are begun and ended; the size is patched in at the end). The Parser walks a run of PODs from untrusted
// bytes: every size is checked against what is there, a String must hold its NUL, and nothing is read past the view it was given.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace pipewire::pod {

enum Type : uint32_t {
  kNone = 1, kBool = 2, kId = 3, kInt = 4, kLong = 5, kFloat = 6, kDouble = 7, kString = 8, kBytes = 9, kRectangle = 10, kFraction = 11, kBitmap = 12,
  kArray = 13, kStruct = 14, kObject = 15, kSequence = 16, kPointer = 17, kFd = 18, kChoice = 19, kPod = 20,
};

enum ChoiceType : uint32_t { kChoiceNone = 0, kChoiceRange = 1, kChoiceStep = 2, kChoiceEnum = 3, kChoiceFlags = 4 };

// How deep containers may nest in what is parsed (a hostile message cannot make the parser recurse without end).
constexpr uint32_t kMaxDepth = 16;

class Builder {
 public:
  explicit Builder(ByteBuffer& out) : out_(out) {}

  void none() { header(0, kNone); }
  void boolean(bool v) { scalar32(kBool, v ? 1 : 0); }
  void id(uint32_t v) { scalar32(kId, v); }
  void int32(int32_t v) { scalar32(kInt, static_cast<uint32_t>(v)); }
  void int64(int64_t v) { scalar64(kLong, static_cast<uint64_t>(v)); }
  void float32(float v);
  void float64(double v);
  void fd(int64_t index) { scalar64(kFd, static_cast<uint64_t>(index)); }
  void fraction(uint32_t num, uint32_t denom);
  void rectangle(uint32_t width, uint32_t height);
  // The text and a NUL.
  void string(StringView text);
  // A string, or None when `text` is null (how PipeWire writes a missing string).
  void string_or_none(const char* text);
  void bytes(ConstByteView data);
  // An Array of Ids / Floats / Ints.
  void array_ids(ArrayView<const uint32_t> values);
  void array_floats(ArrayView<const float> values);
  // A whole POD that was built or parsed elsewhere, copied as it is (padded).
  void raw(ConstByteView pod);

  // Containers: begin_*() returns a frame for end(). Values written in between are its children; for an Object, write property() before each value.
  uint64_t begin_struct() { return begin(kStruct); }
  uint64_t begin_object(uint32_t object_type, uint32_t object_id);
  void property(uint32_t key, uint32_t flags = 0) { put32(key); put32(flags); }
  // A Choice of `kind` whose values are all of `child_type` (4-byte bodies: Int, Id, Bool, Float); write them with choice_value32().
  uint64_t begin_choice(uint32_t kind, uint32_t child_type);
  void choice_value32(uint32_t bits) { put32(bits); }
  void end(uint64_t frame);

  bool ok() const { return ok_; }

 private:
  uint64_t begin(uint32_t type);
  void header(uint32_t size, uint32_t type) { put32(size); put32(type); }
  void scalar32(uint32_t type, uint32_t v) { header(4, type); put32(v); put32(0); }
  void scalar64(uint32_t type, uint64_t v) { header(8, type); put32(static_cast<uint32_t>(v)); put32(static_cast<uint32_t>(v >> 32)); }
  void put32(uint32_t v) { ok_ = out_.put32(v) && ok_; }
  void pad();

  ByteBuffer& out_;
  bool ok_ = true;
};

// One POD as found: its type and its body (without the padding).
struct Value {
  uint32_t type = 0;
  ConstByteView body;
  // The whole POD: header and body (for passing it on as it is).
  ConstByteView whole;

  bool is_none() const { return type == kNone; }
  bool get_bool(bool* out) const;
  bool get_id(uint32_t* out) const;
  bool get_int(int32_t* out) const;
  bool get_long(int64_t* out) const;
  bool get_float(float* out) const;
  bool get_double(double* out) const;
  bool get_fd(int64_t* out) const;
  // A String (the NUL is not part of the view). With `allow_none`, None gives an empty view and *was_none = true.
  bool get_string(StringView* out, bool allow_none = false, bool* was_none = nullptr) const;
  bool get_bytes(ConstByteView* out) const;
};

// A run of PODs one after another (the body of a Struct, a message's payload).
class Parser {
 public:
  Parser() = default;
  explicit Parser(ConstByteView data, uint32_t depth = 0) : data_(data), depth_(depth) {}

  bool at_end() const { return position_ >= data_.size(); }
  // The next POD; false at the end or when it does not fit (the parser then fails for good).
  bool next(Value* out);
  bool ok() const { return ok_; }

  // Typed helpers: each reads the next value and fails (ok() false) if it is not of that type.
  bool int32(int32_t* out);
  bool uint32(uint32_t* out);
  bool id(uint32_t* out);
  bool int64(int64_t* out);
  bool boolean(bool* out);
  bool string(StringView* out, bool allow_none = true);
  bool pod(Value* out) { return next(out) || fail(); }
  // The next value must be a Struct: `inner` walks its children.
  bool structure(Parser* inner);
  uint32_t depth() const { return depth_; }

 private:
  bool fail() { ok_ = false; return false; }

  ConstByteView data_;
  uint64_t position_ = 0;
  uint32_t depth_ = 0;
  bool ok_ = true;
};

// The properties of an Object.
class ObjectReader {
 public:
  // False if `value` is not an Object.
  bool open(const Value& value);
  uint32_t object_type() const { return type_; }
  uint32_t object_id() const { return id_; }
  // The next property; false at the end or when it does not fit.
  bool next(uint32_t* key, uint32_t* flags, Value* value);
  bool ok() const { return ok_; }

 private:
  ConstByteView body_;
  uint64_t position_ = 0;
  uint32_t type_ = 0, id_ = 0;
  bool ok_ = true;
};

// The elements of an Array (or the values of a Choice): `child_type`, each `child_size` bytes.
class ArrayReader {
 public:
  bool open(const Value& value);
  uint32_t child_type() const { return child_type_; }
  uint32_t choice_type() const { return choice_type_; }
  uint64_t count() const { return child_size_ ? values_.size() / child_size_ : 0; }
  // Element `index` as 32 bits (Id, Int, Bool, Float bodies); false if out of range or not 4 bytes wide.
  bool get32(uint64_t index, uint32_t* out) const;

 private:
  ConstByteView values_;
  uint32_t child_type_ = 0, child_size_ = 0, choice_type_ = kChoiceNone;
};

}  // namespace pipewire::pod
