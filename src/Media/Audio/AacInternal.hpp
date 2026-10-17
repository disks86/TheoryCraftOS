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
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

namespace aac {
class Decoder;
}

// An AAC-LC decoder for raw_data_blocks (the payload of an ADTS frame or an MP4 sample). See Aac.cpp for what it covers.
class AacDecoder {
 public:
  explicit AacDecoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~AacDecoder();
  AacDecoder(const AacDecoder&) = delete;
  AacDecoder& operator=(const AacDecoder&) = delete;

  // `sampling_index` as in ADTS and AudioSpecificConfig (0 = 96 kHz ... 11 = 8 kHz).
  bool init(uint32_t sampling_index);
  uint32_t sample_rate() const;
  // Output channels of the last decoded block: 1 or 2 (the front pair of a larger layout).
  uint32_t channels() const;
  // Decodes one block into `pcm`: 1024 frames of channels() interleaved 16-bit samples (room for 2048 values).
  bool decode_block(ConstByteView block, int16_t* pcm);

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<aac::Decoder> impl_;
};

// Reads an MPEG-4 AudioSpecificConfig. False if it is not one this decoder can use.
bool parse_audio_specific_config(ConstByteView config, uint32_t* object_type, uint32_t* sampling_index, uint32_t* channel_config);

}  // namespace media
