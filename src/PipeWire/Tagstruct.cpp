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

#include "PipeWire/Tagstruct.hpp"

namespace pipewire::pulse {

namespace {
constexpr uint64_t kMaxPropListItems = 256;
constexpr uint32_t kMaxPropValue = 64 * 1024;
}  // namespace

uint32_t SampleSpec::sample_size() const {
  switch (format) {
    case kU8: case kAlaw: case kUlaw: return 1;
    case kS16Le: case kS16Be: return 2;
    case kS24Le: case kS24Be: return 3;
    default: return 4;
  }
}

uint32_t Cvolume::max() const {
  uint32_t m = 0;
  for (uint32_t i = 0; i < channels && i < kChannelsMax; ++i)
    if (values[i] > m) m = values[i];
  return m;
}

// ---- TagWriter -------------------------------------------------------------------------------------------------------------------------------------

void TagWriter::be32(uint32_t v) {
  const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  ok_ = out_.append(ConstByteView(b, 4)) && ok_;
}

void TagWriter::be64(uint64_t v) {
  be32(static_cast<uint32_t>(v >> 32));
  be32(static_cast<uint32_t>(v));
}

void TagWriter::string(const char* text) {
  if (!text) string_null();
  else string(StringView(text));
}

void TagWriter::string(StringView text) {
  tag(kTagString);
  // (a NUL inside would end the string early for the reader: cut it there)
  uint64_t n = 0;
  while (n < text.size() && text[n] != 0) ++n;
  ok_ = out_.append(text.first(n).bytes()) && out_.put8(0) && ok_;
}

void TagWriter::arbitrary(ConstByteView data) {
  if (data.size() > UINT32_MAX) { ok_ = false; return; }
  tag(kTagArbitrary);
  be32(static_cast<uint32_t>(data.size()));
  ok_ = out_.append(data) && ok_;
}

void TagWriter::sample_spec(const SampleSpec& spec) {
  tag(kTagSampleSpec);
  byte(spec.format);
  byte(spec.channels);
  be32(spec.rate);
}

void TagWriter::channel_map(const ChannelMap& map) {
  tag(kTagChannelMap);
  const uint8_t n = map.channels <= kChannelsMax ? map.channels : kChannelsMax;
  byte(n);
  for (uint8_t i = 0; i < n; ++i) byte(map.positions[i]);
}

void TagWriter::cvolume(const Cvolume& volume) {
  tag(kTagCvolume);
  const uint8_t n = volume.channels <= kChannelsMax ? volume.channels : kChannelsMax;
  byte(n);
  for (uint8_t i = 0; i < n; ++i) be32(volume.values[i]);
}

void TagWriter::proplist(const PropList& list) {
  tag(kTagPropList);
  for (uint64_t i = 0; i < list.count(); ++i) {
    const Property& p = list.at(i);
    string(p.key.view());
    // text values go with their NUL (pa_proplist_gets wants it)
    const uint64_t n = p.value.size() + 1;
    if (n > UINT32_MAX) { ok_ = false; return; }
    u32(static_cast<uint32_t>(n));
    tag(kTagArbitrary);
    be32(static_cast<uint32_t>(n));
    ok_ = out_.append(p.value.view().bytes()) && out_.put8(0) && ok_;
  }
  string_null();
}

void TagWriter::format_info(uint8_t encoding, const PropList& list) {
  tag(kTagFormatInfo);
  u8(encoding);
  proplist(list);
}

// ---- TagReader -------------------------------------------------------------------------------------------------------------------------------------

bool TagReader::take(uint64_t n, ConstByteView* out) {
  if (!ok_ || n > data_.size() - position_) return fail();
  *out = data_.subview(position_, n);
  position_ += n;
  return true;
}

bool TagReader::expect(uint8_t tag) {
  ConstByteView b;
  if (!take(1, &b)) return false;
  return b[0] == tag || fail();
}

bool TagReader::be32(uint32_t* out) {
  ConstByteView b;
  if (!take(4, &b)) return false;
  *out = uint32_t{b[0]} << 24 | uint32_t{b[1]} << 16 | uint32_t{b[2]} << 8 | b[3];
  return true;
}

bool TagReader::be64(uint64_t* out) {
  uint32_t hi = 0, lo = 0;
  if (!be32(&hi) || !be32(&lo)) return false;
  *out = uint64_t{hi} << 32 | lo;
  return true;
}

bool TagReader::u32(uint32_t* out) { return expect(kTagU32) && be32(out); }
bool TagReader::u64(uint64_t* out) { return expect(kTagU64) && be64(out); }
bool TagReader::usec(uint64_t* out) { return expect(kTagUsec) && be64(out); }
bool TagReader::volume(uint32_t* out) { return expect(kTagVolume) && be32(out); }

bool TagReader::s64(int64_t* out) {
  uint64_t v = 0;
  if (!expect(kTagS64) || !be64(&v)) return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool TagReader::u8(uint8_t* out) {
  ConstByteView b;
  if (!expect(kTagU8) || !take(1, &b)) return false;
  *out = b[0];
  return true;
}

bool TagReader::boolean(bool* out) {
  ConstByteView b;
  if (!take(1, &b)) return false;
  if (b[0] == kTagBooleanTrue) *out = true;
  else if (b[0] == kTagBooleanFalse) *out = false;
  else return fail();
  return true;
}

bool TagReader::string(StringView* out, bool* is_null) {
  ConstByteView b;
  if (is_null) *is_null = false;
  if (!take(1, &b)) return false;
  if (b[0] == kTagStringNull) {
    *out = StringView();
    if (is_null) *is_null = true;
    return true;
  }
  if (b[0] != kTagString) return fail();
  for (uint64_t i = position_; i < data_.size(); ++i)
    if (data_[i] == 0) {
      *out = StringView(data_.subview(position_, i - position_));
      position_ = i + 1;
      return true;
    }
  return fail();
}

bool TagReader::arbitrary(ConstByteView* out) {
  uint32_t n = 0;
  return expect(kTagArbitrary) && be32(&n) && take(n, out);
}

bool TagReader::sample_spec(SampleSpec* out) {
  ConstByteView b;
  if (!expect(kTagSampleSpec) || !take(6, &b)) return false;
  out->format = b[0];
  out->channels = b[1];
  out->rate = uint32_t{b[2]} << 24 | uint32_t{b[3]} << 16 | uint32_t{b[4]} << 8 | b[5];
  return true;
}

bool TagReader::channel_map(ChannelMap* out) {
  ConstByteView b;
  if (!expect(kTagChannelMap) || !take(1, &b)) return false;
  const uint8_t n = b[0];
  if (n > kChannelsMax) return fail();
  ConstByteView positions;
  if (!take(n, &positions)) return false;
  out->channels = n;
  for (uint8_t i = 0; i < n; ++i) out->positions[i] = positions[i];
  return true;
}

bool TagReader::cvolume(Cvolume* out) {
  ConstByteView b;
  if (!expect(kTagCvolume) || !take(1, &b)) return false;
  const uint8_t n = b[0];
  if (n > kChannelsMax) return fail();
  out->channels = n;
  for (uint8_t i = 0; i < n; ++i)
    if (!be32(&out->values[i])) return false;
  return true;
}

bool TagReader::timeval(uint32_t* seconds, uint32_t* microseconds) { return expect(kTagTimeval) && be32(seconds) && be32(microseconds); }

bool TagReader::proplist(PropList* out) {
  if (!expect(kTagPropList)) return false;
  out->clear();
  for (uint64_t items = 0;; ++items) {
    StringView key;
    bool is_null = false;
    if (!string(&key, &is_null)) return false;
    if (is_null) return true;
    if (items >= kMaxPropListItems || key.empty()) return fail();
    uint32_t length = 0;
    ConstByteView value;
    if (!u32(&length) || !arbitrary(&value)) return false;
    if (length != value.size() || length > kMaxPropValue) return fail();
    // a text value carries its NUL; it is not kept
    if (!value.empty() && value[value.size() - 1] == 0) value = value.first(value.size() - 1);
    if (!out->set(key, StringView(value))) return fail();
  }
}

bool TagReader::format_info(uint8_t* encoding, PropList* out) { return expect(kTagFormatInfo) && u8(encoding) && proplist(out); }

}  // namespace pipewire::pulse
