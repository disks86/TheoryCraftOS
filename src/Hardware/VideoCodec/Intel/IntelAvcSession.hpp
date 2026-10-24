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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/Intel/IntelAvcDecode.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

// An H.264 decoding session on the video engine of a Gen9 Intel GPU. Packets are access units (Annex B, or length-prefixed after an avcC record); the parser reads
// their syntax, the decoded picture buffer (the same one the CPU decoder uses) decides references, order counts and output order, and for every picture a command
// batch is built from the slices and run on the engine (VcsDriver). Pictures come out in display order, converted from the engine's tiled NV12 to what the session
// was configured for.
//
// Takes the driver's lease: while one session exists no other can use the engine. A failure of the engine (a job that times out wedges it for good) makes every
// later packet Invalid; nothing falls back to the CPU in the middle of a stream, because the CPU decoder would not have the reference pictures.
class IntelAvcDecoder final : public IVideoDecoder {
 public:
  IntelAvcDecoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config);
  ~IntelAvcDecoder();
  IntelAvcDecoder(const IntelAvcDecoder&) = delete;
  IntelAvcDecoder& operator=(const IntelAvcDecoder&) = delete;

  // Takes the lease and reads the avcC record. False when the engine is not usable or busy with another session, or the record is bad.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::H264; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { dpb_.bump_output(true); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override { return stats_; }

  // Largest picture the engine path takes, in macroblocks per side (4096 pixels).
  static constexpr uint32_t kMaxMbsPerSide = 256;

 private:
  struct Slot {
    intel_gt::GpuBuffer surface, direct_mv;
    uint32_t surface_address = 0, direct_mv_address = 0;
  };
  struct RefRow {
    uint8_t entry[intel_mfx::kMaxAvcRefIdx];
  };
  static_assert(sizeof(RefRow) == intel_mfx::kMaxAvcRefIdx, "rows of reference indices are packed");

  bool place(intel_gt::GpuBuffer* buffer, uint64_t bytes, uint32_t* address);
  void drop(intel_gt::GpuBuffer* buffer, uint32_t address);
  void release_resources();
  bool configure_stream(const h264::Sps& sps);
  bool ensure_slot(int slot);
  bool ensure_bitstream(uint64_t bytes);
  bool decode_picture(ConstByteView packet, ArrayView<const H264Slice> slices, int64_t pts);

  allocators::IAllocator& allocator_;
  intel_gt::VcsDriver& driver_;
  DecoderConfig config_;
  bool leased_ = false;
  H264StreamParser parser_;
  List<H264Slice> slices_;
  List<RefRow> rows0_, rows1_;
  h264::Dpb dpb_;
  h264::PictureInfo infos_[h264::kMaxPictures];
  Slot slots_[h264::kMaxPictures];
  const h264::Sps* sps_ = nullptr;
  int mb_w_ = 0, mb_h_ = 0;
  bool configured_ = false;
  bool waiting_for_idr_ = true;
  intel_mfx::Nv12TiledLayout layout_;
  intel_gt::GpuBuffer intra_row_, deblock_row_, bsd_mpc_, mpr_, bitstream_;
  uint32_t intra_row_address_ = 0, deblock_row_address_ = 0, bsd_mpc_address_ = 0, mpr_address_ = 0, bitstream_address_ = 0;
  uint64_t next_address_ = intel_gt::VcsDriver::kFirstBufferGpuAddress;
  ArrayView<uint32_t> batch_words_;
  VideoFrameBuffer i420_, out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
