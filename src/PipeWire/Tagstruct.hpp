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

// The PulseAudio native protocol's "tagstruct": the body of every command packet, a run of values each led by a one-byte tag ('L' u32, 't' string,
// 'P' property list, ...), big endian. pipewire-pulse speaks it so that programs built on libpulse (most Linux desktop programs) reach the PipeWire
// graph; this is the codec for that (PulseServer.hpp). The reader is for untrusted bytes: every length is checked, a string must end in its NUL.
#pragma once
#include <stdint.h>
#include "PipeWire/Properties.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace pipewire::pulse {

enum Tag : uint8_t {
  kTagString = 't', kTagStringNull = 'N', kTagU32 = 'L', kTagU8 = 'B', kTagU64 = 'R', kTagS64 = 'r', kTagSampleSpec = 'a', kTagArbitrary = 'x',
  kTagBooleanTrue = '1', kTagBooleanFalse = '0', kTagTimeval = 'T', kTagUsec = 'U', kTagChannelMap = 'm', kTagCvolume = 'v', kTagPropList = 'P',
  kTagVolume = 'V', kTagFormatInfo = 'f',
};

constexpr uint32_t kChannelsMax = 32;
constexpr uint32_t kVolumeNorm = 0x10000u;
constexpr uint32_t kVolumeMax = 0x7FFFFFFFu;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

// pa_sample_format_t
enum SampleFormat : uint8_t {
  kU8 = 0, kAlaw = 1, kUlaw = 2, kS16Le = 3, kS16Be = 4, kFloat32Le = 5, kFloat32Be = 6, kS32Le = 7, kS32Be = 8, kS24Le = 9, kS24Be = 10,
  kS24_32Le = 11, kS24_32Be = 12, kSampleFormatMax = 13,
};

struct SampleSpec {
  uint8_t format = kS16Le;
  uint8_t channels = 2;
  uint32_t rate = 48000;
  bool valid() const { return format < kSampleFormatMax && channels >= 1 && channels <= kChannelsMax && rate >= 1 && rate <= 384000; }
  uint32_t sample_size() const;
  uint32_t frame_size() const { return sample_size() * channels; }
};

struct ChannelMap {
  uint8_t channels = 0;
  uint8_t positions[kChannelsMax] = {};
};

struct Cvolume {
  uint8_t channels = 0;
  uint32_t values[kChannelsMax] = {};
  // The loudest channel (what a single volume stands for).
  uint32_t max() const;
};

// A property list ('P') is a Properties (PipeWire's dictionaries and PulseAudio's property lists are the same thing).
using PropList = Properties;

class TagWriter {
 public:
  explicit TagWriter(ByteBuffer& out) : out_(out) {}
  void u32(uint32_t v) { tag(kTagU32); be32(v); }
  void u8(uint8_t v) { tag(kTagU8); byte(v); }
  void u64(uint64_t v) { tag(kTagU64); be64(v); }
  void s64(int64_t v) { tag(kTagS64); be64(static_cast<uint64_t>(v)); }
  void usec(uint64_t v) { tag(kTagUsec); be64(v); }
  void boolean(bool v) { tag(v ? kTagBooleanTrue : kTagBooleanFalse); }
  // A string, or the null string when `text` is null.
  void string(const char* text);
  void string(StringView text);
  void string_null() { tag(kTagStringNull); }
  void arbitrary(ConstByteView data);
  void sample_spec(const SampleSpec& spec);
  void channel_map(const ChannelMap& map);
  void cvolume(const Cvolume& volume);
  void volume(uint32_t v) { tag(kTagVolume); be32(v); }
  void timeval(uint32_t seconds, uint32_t microseconds) { tag(kTagTimeval); be32(seconds); be32(microseconds); }
  void proplist(const PropList& list);
  // A pa_format_info: an encoding (PCM = 1) and its properties.
  void format_info(uint8_t encoding, const PropList& list);
  bool ok() const { return ok_; }

 private:
  void tag(uint8_t t) { byte(t); }
  void byte(uint8_t v) { ok_ = out_.put8(v) && ok_; }
  void be32(uint32_t v);
  void be64(uint64_t v);

  ByteBuffer& out_;
  bool ok_ = true;
};

class TagReader {
 public:
  explicit TagReader(ConstByteView data) : data_(data) {}
  bool at_end() const { return position_ >= data_.size(); }
  bool ok() const { return ok_; }

  bool u32(uint32_t* out);
  bool u8(uint8_t* out);
  bool u64(uint64_t* out);
  bool s64(int64_t* out);
  bool usec(uint64_t* out);
  bool boolean(bool* out);
  // A string or the null string (*is_null set, the view empty).
  bool string(StringView* out, bool* is_null = nullptr);
  bool arbitrary(ConstByteView* out);
  bool sample_spec(SampleSpec* out);
  bool channel_map(ChannelMap* out);
  bool cvolume(Cvolume* out);
  bool volume(uint32_t* out);
  bool timeval(uint32_t* seconds, uint32_t* microseconds);
  bool proplist(PropList* out);
  bool format_info(uint8_t* encoding, PropList* out);

 private:
  bool fail() { ok_ = false; return false; }
  bool expect(uint8_t tag);
  bool take(uint64_t n, ConstByteView* out);
  bool be32(uint32_t* out);
  bool be64(uint64_t* out);

  ConstByteView data_;
  uint64_t position_ = 0;
  bool ok_ = true;
};

}  // namespace pipewire::pulse
