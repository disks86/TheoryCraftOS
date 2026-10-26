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
#include "Hardware/VideoCodec/AMD/AmdUvd6AvcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Messages.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An H.265 decoding session on the UVD 6 decode engine (Radeon RX Vega M). The CPU decoder (H265Decoder) reads every parameter set and slice header, orders the pictures, keeps the
// reference picture set and the output order and checks the pictures' hashes; what it hands to this class, through its offload interface, is the slice data. For
// every picture the engine is sent what Mesa's radeon_uvd.c sends it (RUVD_CODEC_H265): a message with the parameter sets, the order counts of the reference pictures and the buffers each of them
// lives in, the slices behind start codes, and the buffers it works in. The engine keeps the decoded picture buffer itself and writes the decoded picture, as
// linear NV12, into a target surface the CPU reads back into the picture buffer of the CPU decoder (so the picture comes out, cropped and converted, like any other).
//
// A sequence the engine cannot take (a size beyond the limits, more reference pictures than its buffers hold) is decoded on the CPU, whole sequences at a time.
// One session owns the engine's work memory at a time. Unlike VCN's, the firmware's feedback area is not read back. A job that does not finish wedges the ring for good: every later picture is damaged (grey); nothing falls
// back to the CPU in the middle of a stream, since the CPU decoder would not have the reference pictures.
class AmdUvd6HevcDecoder final : public IVideoDecoder, private h265::IOffload {
 public:
  using Engine = AmdUvd6AvcDecoder::Engine;

  AmdUvd6HevcDecoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdUvd6HevcDecoder();
  AmdUvd6HevcDecoder(const AmdUvd6HevcDecoder&) = delete;
  AmdUvd6HevcDecoder& operator=(const AmdUvd6HevcDecoder&) = delete;

  // Takes the engine's lease and reads the hvcC record, if the config has one. False when the engine is busy or the record is not usable. The session with the
  // engine opens at the first picture, once its size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Hevc; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override { cpu_.reset(); }
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;

  bool session_open() const { return open_; }
  // Pictures the engine decoded, and how many of them the stream's own picture hashes confirmed or contradicted.
  uint64_t pictures_decoded() const { return decoded_; }
  H265Decoder::HashStatistics hash_statistics() const { return cpu_.hash_statistics(); }
  // Lets the CPU decoder check the engine's pictures against the stream's MD5 hashes (see H265Decoder::set_plane_hasher).
  void set_plane_hasher(IPlaneHasher* hasher) { cpu_.set_plane_hasher(hasher); }

 private:
  // h265::IOffload
  bool accepts(const h265::Sps& sps, const h265::Pps& pps) override;
  bool decode_picture(const h265::OffloadPicture& picture) override;

  bool open_session(const h265::Sps& sps);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool gather_bitstream(ArrayView<const ConstByteView> slices);
  void read_back(const h265::OffloadPicture& picture);

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  H265Decoder cpu_;
  bool leased_ = false;    // this session holds the work memory's lease
  bool open_ = false;      // the engine has a session
  uint32_t width_ = 0, height_ = 0, references_needed_ = 0;

  amd_uvd6::HevcBufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  uint32_t picture_number_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
