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

#pragma once
#include "Audio/StreamFormat.hpp"
#include "Utility/ArrayView.hpp"

namespace audio {

// Where an AudioPlayer gets its sound from: a seekable run of PCM frames in one StreamFormat.
class IPcmSource {
 public:
  virtual StreamFormat format() const = 0;
  // Length in frames.
  virtual uint64_t frame_count() const = 0;
  // Copies whole frames starting at `frame` into `out`; returns the bytes copied (0 at or past the end).
  virtual uint32_t read(uint64_t frame, ByteView out) = 0;
  // The player stopped or paused (it will not read for a while): a source that drives hardware silences it here. Called with the player locked.
  virtual void playback_paused() {}

 protected:
  constexpr IPcmSource() = default;
  ~IPcmSource() = default;
};

// PCM that is already in memory (a decoded sound, a generated tone, the data of a WAV file). The
// memory is borrowed and must outlive the source.
class MemoryPcmSource final : public IPcmSource {
 public:
  MemoryPcmSource(ConstByteView data, const StreamFormat& format) : data_(data), format_(format) {}

  StreamFormat format() const override { return format_; }
  uint64_t frame_count() const override { return format_.frame_size() ? data_.size() / format_.frame_size() : 0; }
  uint32_t read(uint64_t frame, ByteView out) override {
    const uint32_t size = format_.frame_size();
    if (size == 0 || frame >= frame_count()) return 0;
    uint64_t bytes = (frame_count() - frame) * size;
    if (bytes > out.size()) bytes = out.size();
    bytes -= bytes % size;
    out.first(bytes).copy_from(data_.subview(frame * size, bytes));
    return static_cast<uint32_t>(bytes);
  }

 private:
  ConstByteView data_;
  StreamFormat format_;
};

}  // namespace audio
