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
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Intel/IntelEngineBuffers.hpp"
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An H.265 decoding session on the video engine (the HCP pipeline) of a Gen9.5 Intel GPU. The CPU decoder (H265Decoder) reads the parameter sets and every slice
// header, orders the pictures, works out the reference picture sets and the final reference lists, and keeps the output queue and the picture hash check; what it
// hands to this class, through its offload interface, is each picture's parameter sets, its slice segments (header and NAL unit) and the reference pictures as pool
// slots. For every picture a command batch is built the way the reference driver (gen9_mfd.c) builds it - pipe mode, surface, buffer addresses, scaling lists, picture
// state, tile state, then for each slice segment its state, reference index lists, prediction weights and a bitstream object - and run on the engine. The engine decodes
// into a tiled NV12 surface of its own (one per picture of the CPU decoder's pool, which is also what it predicts from) and keeps each picture's collocated motion
// vectors in a buffer of its own; the surface is read back into the picture of the CPU decoder.
//
// 8-bit 4:2:0 streams whose pictures one slice's references fit the engine's frame store of eight; a picture or sequence the engine cannot take is decoded on the CPU.
// A picture with more slice segments than a batch holds is sent in several batches. One session owns the engine at a time. A job that does not finish wedges the
// engine: every later picture is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class IntelHevcDecoder final : public IVideoDecoder, private h265::IOffload {
 public:
  IntelHevcDecoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config);
  ~IntelHevcDecoder();
  IntelHevcDecoder(const IntelHevcDecoder&) = delete;
  IntelHevcDecoder& operator=(const IntelHevcDecoder&) = delete;

  // Takes the engine's lease and reads the codec configuration (an hvcC record) if there is one. False when the engine is not usable or busy with another session,
  // or the configuration is not readable.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Hevc; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override { cpu_.reset(); }
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;
  uint64_t pictures_decoded() const { return decoded_; }
  // The stream's picture hashes (SEI), checked by the CPU decoder against the pictures this session makes.
  void set_plane_hasher(IPlaneHasher* hasher) { cpu_.set_plane_hasher(hasher); }
  H265Decoder::HashStatistics hash_statistics() const { return cpu_.hash_statistics(); }
  // Keeps a batch to at most `words` dwords (for tests: a picture with many slices goes in several batches).
  void limit_batch_words(uint64_t words) { batch_limit_ = words; }

 private:
  // h265::IOffload
  bool accepts(const h265::Sps& sps, const h265::Pps& pps) override;
  bool decode_picture(const h265::OffloadPicture& picture) override;

  // Which of the picture's reference pictures (indexes into OffloadPicture::refs) are in which frame store of the engine.
  struct FrameStore {
    int8_t of[h265::kMaxRefs];
    uint8_t ref[intel_mfx::kMaxHevcReferenceFrames];   // the reference picture in each frame store
    uint32_t count = 0;
  };
  bool build_frame_store(const h265::OffloadPicture& picture, FrameStore* store) const;
  bool build_slices(const h265::OffloadPicture& picture, const FrameStore& store);
  bool fail(const h265::OffloadPicture& picture, bool grey);

  allocators::IAllocator& allocator_;
  DecoderConfig config_;
  IntelEngineBuffers buffers_;
  H265Decoder cpu_;
  ByteBuffer host_bits_;
  List<intel_mfx::HevcSlice> jobs_;
  uint64_t decoded_ = 0, failed_ = 0, batch_limit_ = UINT64_MAX;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
