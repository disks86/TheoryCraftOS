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
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An MPEG-2 decoding session on the UVD 6 decode engine (Radeon RX Vega M). The CPU decoder (Mpeg2Decoder) reads the sequence and picture headers, orders the pictures and keeps the
// output queue; what it hands to this class, through its offload interface, is each picture's headers and slices. For every picture the engine is sent what Mesa
// sends it (radeon_uvd.c, RUVD_CODEC_MPEG2): a message with the picture coding parameters, both quantiser matrices and the job numbers of the picture and of the pictures it
// predicts from, the slices behind it with their start codes, and the buffers it works in. The engine keeps the last six pictures it decoded, and a picture is named by the number
// of its job, so this class remembers the job number of each picture the CPU decoder still holds and refuses a reference more than six jobs back (that picture is grey). It writes the decoded picture, as linear NV12, into a target surface the CPU reads back into the picture of the CPU decoder (so it comes out, cropped and
// converted, like any other).
//
// For a field picture only the lines of its field are read back. Unlike VCN's, the firmware's feedback area is not read back.
//
// A sequence the engine cannot take (a size beyond the limits) is decoded on the CPU, whole sequences at a time. One session owns the engine's work memory at a time.
// A job that does not finish wedges the ring for good: every later picture is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class AmdUvd6Mpeg2Decoder final : public IVideoDecoder, private mpeg2::IOffload {
 public:
  using Engine = AmdUvd6AvcDecoder::Engine;

  AmdUvd6Mpeg2Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdUvd6Mpeg2Decoder();
  AmdUvd6Mpeg2Decoder(const AmdUvd6Mpeg2Decoder&) = delete;
  AmdUvd6Mpeg2Decoder& operator=(const AmdUvd6Mpeg2Decoder&) = delete;

  // Takes the engine's lease. False when the engine is busy. The session with the engine opens at the first picture, once its size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Mpeg2; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;

  bool session_open() const { return open_; }
  uint64_t pictures_decoded() const { return decoded_; }

 private:
  // mpeg2::IOffload
  bool accepts(const mpeg2::SequenceHeader& seq) override;
  bool decode_picture(const mpeg2::OffloadPicture& picture) override;

  bool open_session(const mpeg2::SequenceHeader& seq);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool gather_bitstream(ArrayView<const ConstByteView> slices);
  // The job numbers of the picture and its references; false when a reference is no longer in the engine's picture buffer.
  bool job_numbers(const mpeg2::OffloadPicture& picture, uint32_t number, amd_uvd6::Mpeg2Pictures* pictures) const;
  void read_back(const mpeg2::OffloadPicture& picture);

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  Mpeg2Decoder cpu_;
  bool leased_ = false;    // this session holds the work memory's lease
  bool open_ = false;      // the engine has a session
  uint32_t width_ = 0, height_ = 0;

  amd_uvd6::Mpeg2BufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  uint32_t number_of_[mpeg2::kPoolSize];                    // the job that decoded the picture in a pool entry of the CPU decoder (0: none)
  uint32_t picture_number_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
