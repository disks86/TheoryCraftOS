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
#include "Hardware/VideoCodec/Intel/IntelAvpCommands.hpp"
#include "Hardware/VideoCodec/Intel/IntelEngineBuffers.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// An AV1 decoding session on the video engine (the AVP pipeline) of a Gen12 (Xe-LP) Intel GPU. The CPU decoder (Av1Decoder) parses every OBU, checks the headers, keeps the
// reference slots and the output; what it hands to this class, through its offload interface, is the frame: the headers and the tile data. For every frame a command batch is
// built the way the reference driver (iHD) builds it - pipe mode, the surface and its reference pictures, buffer addresses, a state for each segment, the frame state, the
// loop filter state, the inter prediction state, then a tile coding state and a bitstream object for every tile - and run on the engine. The engine decodes the tiles into a
// tiled NV12 surface of its own; the pictures its reference slots hold are such surfaces too. The decoded surface is read back into the CPU decoder's picture (so the picture
// comes out, converted, like any other, and a show_existing_frame needs nothing from the engine).
//
// What the engine keeps for a frame and the next frames find: the entropy contexts (the engine reads the tables a frame starts from out of a buffer of its own, which
// is filled with the defaults for the frame's quantizer or with the tables the primary reference frame ended with, and leaves the adapted tables of the frame in a second
// one), the segmentation map, and the collocated motion vectors. They live in a buffer tied to the picture's surface.
//
// 8-bit 4:2:0 streams without film grain, super-resolution or spatial layers, up to 4096 x 4096; anything else is decoded by the CPU, the whole stream. Reference scaling
// (frames of another size than the reference's) is left to the engine. One session owns the engine at a time. A job that does not finish wedges the engine: every later
// frame is damaged (grey); nothing falls back to the CPU in the middle of a stream.
//
// Not run on a real engine: the command order (the reference driver's frame level function is not at hand), the quantizer matrix levels per segment and the memory-object
// control index are inferred.
class IntelAv1Decoder final : public IVideoDecoder, private av1::IOffload {
 public:
  IntelAv1Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config);
  ~IntelAv1Decoder();
  IntelAv1Decoder(const IntelAv1Decoder&) = delete;
  IntelAv1Decoder& operator=(const IntelAv1Decoder&) = delete;

  // Takes the engine's lease and reads the av1C record, if the config has one. False when the engine is not usable, busy with another session, or the record is damaged.
  bool prepare();

  VideoCodec codec() const override { return VideoCodec::Av1; }
  SubmitResult submit(const VideoPacket& packet) override;
  bool receive(VideoFrame* frame) override;
  void flush() override { cpu_.flush(); }
  void reset() override;
  VideoFormat output_format() const override;
  VideoCodecStatistics statistics() const override;
  uint64_t frames_decoded() const { return decoded_; }

 private:
  // av1::IOffload
  bool accepts(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr) override;
  bool decode_frame(const av1::OffloadFrame& frame) override;
  void show_existing(unsigned idx, bool refresh_all) override;
  void new_sequence() override { forget_references(); }

  // What the session keeps about the frame each engine surface holds.
  struct SlotState {
    bool valid = false;                    // decoded by the engine
    uint32_t frame_type = 0, order_hint = 0;
    uint32_t width = 0, height = 0, mi_cols = 0, mi_rows = 0;
    uint32_t ref_order_hint[7] = {};       // the order hints of the references the frame was decoded with
    bool cdf_valid = false, frame_end_update = false;   // the tables it ended with are in the backward adaptation buffer (else they are the ones it started from)
    bool seg_valid = false, mv_valid = false;
  };
  struct AuxLayout {
    uint64_t cdf_init = 0, cdf_backward = 0, segment_id = 0, motion_vector = 0, bytes = 0;
    uint64_t segment_id_bytes = 0;
  };

  bool run_frame(const av1::OffloadFrame& frame, int current);
  bool open_for(const av1::SequenceHeader& seq);
  void read_back(const av1::OffloadFrame& frame, int surface);
  int pick_surface() const;
  void forget_references();
  uint32_t motion_field_mask(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr, uint32_t mi_cols, uint32_t mi_rows) const;

  allocators::IAllocator& allocator_;
  DecoderConfig config_;
  IntelEngineBuffers buffers_;
  Av1Decoder cpu_;
  ByteBuffer host_bits_, host_planes_, host_cdf_;
  int ref_surface_[8];                     // the engine surface each of the stream's eight reference slots holds (-1: none)
  SlotState state_[IntelEngineBuffers::kMaxSurfaces];
  AuxLayout aux_;
  uint32_t open_width_ = 0, open_height_ = 0;   // what the buffers were made for (the sequence's biggest picture, rounded up to 8)
  bool open_sb128_ = false;
  uint64_t decoded_ = 0, failed_ = 0;
  VideoFrameBuffer out_;
  VideoCodecStatistics stats_;
};

}  // namespace hardware
