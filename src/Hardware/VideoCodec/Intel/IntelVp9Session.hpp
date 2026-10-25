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
#include "Hardware/VideoCodec/Intel/IntelVp9Probs.hpp"
#include "Hardware/VideoCodec/Software/Vp9.hpp"
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// A VP9 decoding session on the video engine (the HCP pipeline) of a Gen9.5 Intel GPU. The CPU decoder (Vp9Decoder) reads every uncompressed header (sizes, loop filter,
// quantizer and segmentation parameters, reference slots), keeps the reference slots and the output; what it hands to this class, through its offload interface, is the
// frame: those parameters and the frame's bytes. For every frame a command batch is built the way the reference driver (gen9_mfd.c) builds it - pipe mode, the surface and
// its three reference pictures, buffer addresses, a state for each segment, the frame state, then a bitstream object for the compressed header and the tiles - and run on
// the engine. The engine parses the compressed header and decodes the tiles itself, into a tiled NV12 surface of its own; the pictures its reference slots hold are such
// surfaces too. Like the reference driver this class keeps the four saved probability contexts: the engine reads the probabilities a frame starts from out of a buffer
// built from them and leaves the adapted ones in it, and the driver saves those for the frames that use that context. The decoded surface is read back into the CPU
// decoder's picture (so it comes out, converted, like any other, and a show_existing_frame needs nothing from the engine).
//
// 8-bit 4:2:0 streams (profile 0) up to 4096 x 4096; anything else is decoded by the CPU, the whole stream. The session is made for the size of the key frame that starts
// the stream: a later frame of another size (reference scaling) cannot be decoded and is shown grey. One session owns the engine at a time. A job that does not finish
// wedges the engine: every later frame is damaged (grey); nothing falls back to the CPU in the middle of a stream.
class IntelVp9Decoder final : public IVideoDecoder, private vp9::IOffload {
 public:
  IntelVp9Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config);
  ~IntelVp9Decoder();
  IntelVp9Decoder(const IntelVp9Decoder&) = delete;
  IntelVp9Decoder& operator=(const IntelVp9Decoder&) = delete;

  // Takes the engine's lease. False when the engine is not usable or busy with another session.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Vp9; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;
  uint64_t frames_decoded() const { return decoded_; }

 private:
  // vp9::IOffload
  bool accepts(const vp9::FrameHeaderInfo& info, int ss_x, int ss_y) override;
  bool decode_frame(const vp9::OffloadFrame& frame) override;

  bool run_frame(const vp9::OffloadFrame& frame, int current);
  void read_back(const vp9::OffloadFrame& frame, int surface);
  int pick_surface() const;
  void forget_references();

  allocators::IAllocator& allocator_;
  DecoderConfig config_;
  IntelEngineBuffers buffers_;
  Vp9Decoder cpu_;
  ByteBuffer host_bits_, host_planes_;
  intel_vp9::ProbabilityContexts contexts_;
  int ref_surface_[8];                       // the engine surface each of the stream's eight reference slots holds (-1: none)
  uint32_t surface_width_[IntelEngineBuffers::kMaxSurfaces] = {}, surface_height_[IntelEngineBuffers::kMaxSurfaces] = {};   // of the frame each holds, as coded
  uint32_t mv_current_ = 10, mv_last_ = 11;  // the scratch buffers of this frame's collocated motion vectors and of the frame before's
  // The frame before: what the engine's flags for this one depend on.
  bool have_last_ = false, last_show_ = false, last_intra_only_ = false;
  uint32_t last_width_ = 0, last_height_ = 0, last_frame_type_ = 0;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
