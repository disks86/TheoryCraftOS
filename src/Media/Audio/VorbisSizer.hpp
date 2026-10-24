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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

// How many samples each audio packet of a Vorbis stream adds to the sound, found from the packets alone (their mode numbers and the block sizes
// of the headers) - what a container that does not store sample positions (WebM) needs to give an Ogg file its granule positions.
class VorbisSizer {
 public:
  explicit VorbisSizer(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~VorbisSizer();
  VorbisSizer(const VorbisSizer&) = delete;
  VorbisSizer& operator=(const VorbisSizer&) = delete;

  // The three header packets. False if they are not valid Vorbis headers.
  bool init(ConstByteView identification, ConstByteView comment, ConstByteView setup);
  // The samples (per channel) the next audio packet adds to the output: 0 for the first, then a quarter of each of the two blocks it overlaps.
  // -1 if it is not an audio packet.
  int64_t next(ConstByteView packet);
  uint32_t sample_rate() const { return rate_; }
  uint32_t channels() const { return channels_; }

 private:
  allocators::IAllocator& allocator_;
  struct Impl;
  UniquePointer<Impl> impl_;
  uint32_t rate_ = 0, channels_ = 0;
};

}  // namespace media
