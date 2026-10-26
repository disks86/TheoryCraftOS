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
#include "Hardware/VideoCodec/Software/Mpeg4Headers.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An MPEG-4 Part 2 decoding session (Simple and Advanced Simple profile; DivX, Xvid) on the UVD 6 decode engine (Radeon RX Vega M). There is no CPU decoder for this codec:
// the headers are read by Mpeg4Headers (the layer's parameters and each plane's coding type), the engine does all the decoding, and this class keeps what a decoder around
// the engine has to: the job numbers of the two anchor pictures (the engine names a picture by the number of the job that decoded it, and keeps the last six), and the output
// order. Pictures come out in display order: an anchor (I or P) is held back until the next anchor has been decoded, a B picture goes out at once, and a stream with
// no B pictures (low_delay) goes out as it is decoded; flush() releases the last anchor. Each packet carries one plane (with the layer header in front of it when it is
// repeated); the layer header may also come as the configuration record.
//
// Not taken: more than one plane in a packet (a packed bitstream), a plane that is not coded (a "repeat the last picture" marker), a change of picture size without reset(), a
// reference more than six jobs back (five B pictures in a row), the H.263 short header, and the tools Mpeg4Headers lists. One session owns the engine's work memory at a time. A job
// that does not finish wedges the ring for good: every later packet is refused.
class AmdUvd6Mpeg4Decoder final : public IVideoDecoder {
 public:
  using Engine = AmdUvd6AvcDecoder::Engine;

  AmdUvd6Mpeg4Decoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdUvd6Mpeg4Decoder();
  AmdUvd6Mpeg4Decoder(const AmdUvd6Mpeg4Decoder&) = delete;
  AmdUvd6Mpeg4Decoder& operator=(const AmdUvd6Mpeg4Decoder&) = delete;

  // Takes the engine's lease and reads the layer header out of the configuration record, if there is one. False when the engine is busy or the record is not usable. The
  // session with the engine opens at the first plane, once the layer's size is known.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Mpeg4; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override;
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;

  bool session_open() const { return open_; }
  uint64_t pictures_decoded() const { return decoded_; }

 private:
  struct Slot {
    explicit Slot(allocators::IAllocator& allocator) : buffer(allocator) {}
    VideoFrameBuffer buffer;
    int64_t pts_us = 0;
    bool keyframe = false;
  };

  bool accepts(const mpeg4::Vol& vol) const;
  bool open_session(const mpeg4::Vol& vol);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool decode_plane(const VideoPacket& packet, const mpeg4::Vop& vop, Slot* slot);
  void read_back(Slot* slot);
  void forget_pictures();

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  bool leased_ = false;    // this session holds the work memory's lease
  bool open_ = false;      // the engine has a session
  bool have_vol_ = false;
  mpeg4::Vol vol_;
  uint32_t width_ = 0, height_ = 0;   // the layer's size, as the session was opened for it

  amd_uvd6::Mpeg4BufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_, row_;
  uint32_t picture_number_ = 0;
  uint32_t newest_anchor_ = 0, older_anchor_ = 0;   // job numbers (0: none)
  uint64_t decoded_ = 0, failed_ = 0;
  Slot slots_[3];
  int held_ = -1, ready_ = -1;   // which slot holds the anchor waiting for the next one, and which the picture to hand out (-1: none)
  int free_slot() const { for (int i = 0; i < 3; ++i) if (i != held_ && i != ready_) return i; return 0; }
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
