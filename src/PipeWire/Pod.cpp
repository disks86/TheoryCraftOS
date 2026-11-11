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

#include "PipeWire/Pod.hpp"
#include "Utility/ByteStream.hpp"

namespace pipewire::pod {

namespace {

uint32_t le32_at(ConstByteView v, uint64_t at) {
  return uint32_t{v[at]} | uint32_t{v[at + 1]} << 8 | uint32_t{v[at + 2]} << 16 | uint32_t{v[at + 3]} << 24;
}

constexpr uint64_t padded(uint64_t size) { return (size + 7) & ~uint64_t{7}; }

}  // namespace

// ---- Builder ---------------------------------------------------------------------------------------------------------------------------------------

void Builder::pad() {
  const uint64_t over = out_.size() & 7;
  if (over) ok_ = out_.put_zeros(8 - over) && ok_;
}

void Builder::float32(float v) { scalar32(kFloat, __builtin_bit_cast(uint32_t, v)); }
void Builder::float64(double v) { scalar64(kDouble, __builtin_bit_cast(uint64_t, v)); }

void Builder::fraction(uint32_t num, uint32_t denom) {
  header(8, kFraction);
  put32(num);
  put32(denom);
}

void Builder::rectangle(uint32_t width, uint32_t height) {
  header(8, kRectangle);
  put32(width);
  put32(height);
}

void Builder::string(StringView text) {
  if (text.size() >= UINT32_MAX) { ok_ = false; return; }
  header(static_cast<uint32_t>(text.size() + 1), kString);
  ok_ = out_.append(text.bytes()) && out_.put8(0) && ok_;
  pad();
}

void Builder::string_or_none(const char* text) {
  if (!text) none();
  else string(StringView(text));
}

void Builder::bytes(ConstByteView data) {
  if (data.size() >= UINT32_MAX) { ok_ = false; return; }
  header(static_cast<uint32_t>(data.size()), kBytes);
  ok_ = out_.append(data) && ok_;
  pad();
}

void Builder::array_ids(ArrayView<const uint32_t> values) {
  if (values.size() > (UINT32_MAX - 8) / 4) { ok_ = false; return; }
  header(static_cast<uint32_t>(8 + values.size() * 4), kArray);
  header(4, kId);
  for (uint32_t v : values) put32(v);
  pad();
}

void Builder::array_floats(ArrayView<const float> values) {
  if (values.size() > (UINT32_MAX - 8) / 4) { ok_ = false; return; }
  header(static_cast<uint32_t>(8 + values.size() * 4), kArray);
  header(4, kFloat);
  for (float v : values) put32(__builtin_bit_cast(uint32_t, v));
  pad();
}

void Builder::raw(ConstByteView pod) {
  ok_ = out_.append(pod) && ok_;
  pad();
}

uint64_t Builder::begin(uint32_t type) {
  pad();
  const uint64_t frame = out_.size();
  header(0, type);
  return frame;
}

uint64_t Builder::begin_object(uint32_t object_type, uint32_t object_id) {
  const uint64_t frame = begin(kObject);
  put32(object_type);
  put32(object_id);
  return frame;
}

uint64_t Builder::begin_choice(uint32_t kind, uint32_t child_type) {
  const uint64_t frame = begin(kChoice);
  put32(kind);
  put32(0);
  header(4, child_type);
  return frame;
}

void Builder::end(uint64_t frame) {
  if (frame > out_.size() || out_.size() - frame < 8 || out_.size() - frame - 8 > UINT32_MAX) { ok_ = false; return; }
  const uint64_t size = out_.size() - frame - 8;
  ok_ = out_.patch32(frame, static_cast<uint32_t>(size)) && ok_;
  pad();
}

// ---- Value -----------------------------------------------------------------------------------------------------------------------------------------

namespace {
bool body32(const Value& v, uint32_t type, uint32_t* out) {
  if (v.type != type || v.body.size() < 4) return false;
  *out = le32_at(v.body, 0);
  return true;
}
bool body64(const Value& v, uint32_t type, uint64_t* out) {
  if (v.type != type || v.body.size() < 8) return false;
  *out = uint64_t{le32_at(v.body, 0)} | uint64_t{le32_at(v.body, 4)} << 32;
  return true;
}
}  // namespace

bool Value::get_bool(bool* out) const {
  uint32_t v = 0;
  if (!body32(*this, kBool, &v)) return false;
  *out = v != 0;
  return true;
}
bool Value::get_id(uint32_t* out) const { return body32(*this, kId, out); }
bool Value::get_int(int32_t* out) const {
  uint32_t v = 0;
  if (!body32(*this, kInt, &v)) return false;
  *out = static_cast<int32_t>(v);
  return true;
}
bool Value::get_long(int64_t* out) const {
  uint64_t v = 0;
  if (!body64(*this, kLong, &v)) return false;
  *out = static_cast<int64_t>(v);
  return true;
}
bool Value::get_float(float* out) const {
  uint32_t v = 0;
  if (!body32(*this, kFloat, &v)) return false;
  *out = __builtin_bit_cast(float, v);
  return true;
}
bool Value::get_double(double* out) const {
  uint64_t v = 0;
  if (!body64(*this, kDouble, &v)) return false;
  *out = __builtin_bit_cast(double, v);
  return true;
}
bool Value::get_fd(int64_t* out) const {
  uint64_t v = 0;
  if (!body64(*this, kFd, &v)) return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool Value::get_string(StringView* out, bool allow_none, bool* was_none) const {
  if (was_none) *was_none = false;
  if (type == kNone && allow_none) {
    *out = StringView();
    if (was_none) *was_none = true;
    return true;
  }
  if (type != kString || body.empty()) return false;
  // the text ends at its first NUL, which must be inside the body
  for (uint64_t i = 0; i < body.size(); ++i)
    if (body[i] == 0) {
      *out = StringView(body.first(i));
      return true;
    }
  return false;
}

bool Value::get_bytes(ConstByteView* out) const {
  if (type != kBytes) return false;
  *out = body;
  return true;
}

// ---- Parser ----------------------------------------------------------------------------------------------------------------------------------------

bool Parser::next(Value* out) {
  if (!ok_ || position_ >= data_.size()) return false;
  const uint64_t left = data_.size() - position_;
  if (left < 8) return fail();
  const uint32_t size = le32_at(data_, position_);
  const uint32_t type = le32_at(data_, position_ + 4);
  if (size > left - 8) return fail();
  out->type = type;
  out->body = data_.subview(position_ + 8, size);
  out->whole = data_.subview(position_, 8 + uint64_t{size});
  // (the padding after the last POD of a run may be missing)
  const uint64_t step = padded(8 + uint64_t{size});
  position_ = step > left ? data_.size() : position_ + step;
  return true;
}

bool Parser::int32(int32_t* out) {
  Value v;
  return (next(&v) && v.get_int(out)) || fail();
}
bool Parser::uint32(uint32_t* out) {
  int32_t v = 0;
  if (!int32(&v)) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}
bool Parser::id(uint32_t* out) {
  Value v;
  return (next(&v) && v.get_id(out)) || fail();
}
bool Parser::int64(int64_t* out) {
  Value v;
  return (next(&v) && v.get_long(out)) || fail();
}
bool Parser::boolean(bool* out) {
  Value v;
  return (next(&v) && v.get_bool(out)) || fail();
}
bool Parser::string(StringView* out, bool allow_none) {
  Value v;
  return (next(&v) && v.get_string(out, allow_none)) || fail();
}

bool Parser::structure(Parser* inner) {
  Value v;
  if (!next(&v) || v.type != kStruct || depth_ + 1 >= kMaxDepth) return fail();
  *inner = Parser(v.body, depth_ + 1);
  return true;
}

// ---- ObjectReader / ArrayReader --------------------------------------------------------------------------------------------------------------------

bool ObjectReader::open(const Value& value) {
  if (value.type != kObject || value.body.size() < 8) return false;
  type_ = le32_at(value.body, 0);
  id_ = le32_at(value.body, 4);
  body_ = value.body;
  position_ = 8;
  ok_ = true;
  return true;
}

bool ObjectReader::next(uint32_t* key, uint32_t* flags, Value* value) {
  if (!ok_ || position_ >= body_.size()) return false;
  const uint64_t left = body_.size() - position_;
  if (left < 16) { ok_ = false; return false; }
  *key = le32_at(body_, position_);
  *flags = le32_at(body_, position_ + 4);
  const uint32_t size = le32_at(body_, position_ + 8);
  const uint32_t type = le32_at(body_, position_ + 12);
  if (size > left - 16) { ok_ = false; return false; }
  value->type = type;
  value->body = body_.subview(position_ + 16, size);
  value->whole = body_.subview(position_ + 8, 8 + uint64_t{size});
  const uint64_t step = 8 + padded(8 + uint64_t{size});
  position_ = step > left ? body_.size() : position_ + step;
  return true;
}

bool ArrayReader::open(const Value& value) {
  ConstByteView body = value.body;
  choice_type_ = kChoiceNone;
  if (value.type == kChoice) {
    if (body.size() < 8) return false;
    choice_type_ = le32_at(body, 0);
    body = body.subview(8);
  } else if (value.type != kArray) {
    return false;
  }
  if (body.size() < 8) return false;
  child_size_ = le32_at(body, 0);
  child_type_ = le32_at(body, 4);
  values_ = body.subview(8);
  if (child_size_ == 0) values_ = ConstByteView();
  return true;
}

bool ArrayReader::get32(uint64_t index, uint32_t* out) const {
  if (child_size_ != 4 || index >= count()) return false;
  *out = le32_at(values_, index * 4);
  return true;
}

}  // namespace pipewire::pod
