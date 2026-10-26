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
#include "Hardware/VideoCodec/AMD/AmdUvd6Messages.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// An H.264 decoding session on the UVD 6 decode engine (Radeon RX Vega M). The same session as AmdVcnAvcDecoder - packets are access units (Annex B, or length-prefixed
// after an avcC record); the parser reads their syntax, and for every picture the engine is sent what Mesa's radeon_uvd.c sends it: a message with the parameter sets and
// the reference pictures' frame numbers and order counts (AmdUvd6Messages), the slices behind start codes, and the buffers it works in - on the UVD ring (AmdUvd6Ring).
// Unlike VCN's, the firmware's feedback area is not read back (nothing is known of its layout beyond the size word Mesa writes); a picture counts as decoded when its
// job's fence comes. The engine keeps the decoded picture buffer itself and writes the decoded picture, as linear NV12, into a
// target surface the CPU reads back. The order counts, reference marking and output order are decided here (the same decoded picture buffer rules the CPU decoder
// uses), so pictures come out in display order whatever the stream's reordering.
//
// One session owns the engine's work memory at a time. A job that does not finish wedges the ring for good: every later packet is Invalid; nothing falls back to the
// CPU in the middle of a stream, since the CPU decoder would not have the reference pictures.
class AmdUvd6AvcDecoder final : public IVideoDecoder {
 public:
  struct Engine {
    AmdUvd6Ring* ring = nullptr;
    AmdVcnWorkMemory* memory = nullptr;
    uint32_t stream_handle = 1;        // names the session to the firmware; every open session needs its own
    uint32_t job_timeout_ms = 2000;    // a picture takes milliseconds; two seconds is a dead engine
  };

  AmdUvd6AvcDecoder(allocators::IAllocator& allocator, const Engine& engine, const DecoderConfig& config);
  ~AmdUvd6AvcDecoder();
  AmdUvd6AvcDecoder(const AmdUvd6AvcDecoder&) = delete;
  AmdUvd6AvcDecoder& operator=(const AmdUvd6AvcDecoder&) = delete;

  // Reads the avcC record, and when it carries a usable sequence parameter set opens the engine's session for it right away (so a stream the engine cannot take is
  // refused here, and the caller falls back to the CPU). False when refused. An Annex B stream opens its session at the first picture.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::H264; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { dpb_.bump_output(true); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override { return stats_; }

  bool session_open() const { return open_; }

 private:
  bool configure_stream(const h264::Sps& sps);
  bool allocate_buffers();
  void close_session();
  bool send_session_message(bool create);
  bool decode_picture(ConstByteView packet, ArrayView<const H264Slice> slices, int64_t pts);
  bool gather_bitstream(ConstByteView packet, ArrayView<const H264Slice> slices);
  bool read_back(h264::PictureInfo* picture);
  bool ensure_frame(int slot);
  void paint_grey(int slot);

  allocators::IAllocator& allocator_;
  Engine engine_;
  DecoderConfig config_;
  H264StreamParser parser_;
  List<H264Slice> slices_;
  h264::Dpb dpb_;
  h264::PictureInfo infos_[h264::kMaxPictures];
  UniquePointer<VideoFrameBuffer> frames_[h264::kMaxPictures];   // decoded pictures, NV12, cropped to what is shown
  const h264::Sps* sps_ = nullptr;
  int mb_w_ = 0, mb_h_ = 0;
  uint32_t crop_x_ = 0, crop_y_ = 0, shown_w_ = 0, shown_h_ = 0;
  bool configured_ = false;
  bool open_ = false;      // the engine has a session for this stream
  bool leased_ = false;    // this session holds the work memory's lease
  bool waiting_for_idr_ = true;

  amd_uvd6::AvcBufferSizes sizes_;
  amd_vcn::TargetLayout layout_;
  AmdVcnWorkMemory::Piece message_, session_context_, context_, dpb_buffer_, bitstream_, target_;
  ByteBuffer host_bits_, host_message_;
  uint32_t picture_number_ = 0;
  VideoFrameBuffer i420_, out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
