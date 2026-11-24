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
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace h264 {
class Core;
}

// An H.264 / AVC decoder on the CPU: Baseline, Main and High profile video (8-bit 4:2:0 or monochrome, CAVLC or CABAC, I/P/B
// slices, multiple reference frames, weighted prediction, 8x8 transform and scaling matrices, multiple slices, deblocking),
// progressive or interlaced: field pictures (alone or as complementary field pairs in one frame) and MBAFF frames (frame and
// field macroblock pairs), with field reference lists, field order counts, direct modes and the loop filter of each, and gaps in
// frame_num (8.2.5.2: "non-existing" frames, the samples of the reference before them, which a lost picture is concealed with). Not
// covered: flexible macroblock ordering, data partitioning, SP/SI slices, high bit depths and 4:2:2 / 4:4:4. Pictures come
// out in display order, as frames (a field pair in one frame; a field without its pair with the other field's lines grey).
//
// Input is either Annex B (start codes, with SPS and PPS in the stream) or - after configure() with an avcC record,
// as stored in MP4 and AVI files - length-prefixed NAL units.
class H264Decoder {
 public:
  explicit H264Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  // Takes the parameter sets out of an avcC record (ISO 14496-15) and switches the input to length-prefixed NAL units.
  bool configure(ConstByteView avcc);
  // Decodes one access unit (all NAL units of a picture). False if the data holds nothing decodable.
  bool decode(ConstByteView access_unit, int64_t pts_us);
  // The next picture in display order, as I420 (cropped). The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  // The stream ends: pictures held for reordering become available.
  void flush();
  // Drops everything and waits for an IDR picture (after a seek).
  void reset();
  // The size of the pictures; zero until a sequence parameter set was seen.
  VideoFormat output_format() const;
  bool ready() const;  // a picture can be received

  // For tests and tools: told about every slice once its reference lists are built, with the order count of each entry of the lists.
  struct SliceRefs {
    int slice_type = 0;          // 0 P, 1 B, 2 I
    int first_mb = 0;
    int current_poc = 0;
    int count[2] = {0, 0};
    int poc[2][32] = {};
    bool long_term[2][32] = {};
  };
  using SliceObserver = void (*)(void* context, const SliceRefs& refs);
  void set_slice_observer(SliceObserver observer, void* context);

  // Decoding as a DXVA driver does it (src/Hardware/VideoCodec/Dxva/DxvaH264.cpp): the program keeps the decoded picture buffer. For each picture it names the surface the
  // picture goes into (a key) and the references with their order counts, frame numbers and long-term status; the decoder keeps each decoded picture under its key and
  // builds the slices' reference lists from those references only (its own marking and order counting are not used), and the picture comes back at once, uncropped.
  struct ExternalReference {
    uint32_t key = 0;           // the surface the reference was decoded into
    bool long_term = false;
    bool non_existing = false;  // a frame the program inferred for a gap in frame_num (never decoded: mid-grey, not reported missing)
    int frame_num = 0;          // FrameNum, or LongTermFrameIdx of a long-term reference
    int top_poc = 0, bottom_poc = 0;
    uint8_t fields = 3;         // the fields used for reference: bit 0 the top field, bit 1 the bottom field (a frame picture uses a reference with both)
  };
  struct ExternalPicture {
    uint32_t key = 0;           // where the picture is kept (a reference to this key reads the picture that was there before)
    int top_poc = 0, bottom_poc = 0;   // (a field picture: only its own field's count is used)
    // (A field picture - its slices say which - is decoded into the frame of the surface: the other field's lines stay what the surface held, so the second
    // field of a pair goes in beside the first.)
    ArrayView<const ExternalReference> references;   // at most 16
  };
  enum class ExternalStatus : uint8_t {
    Decoded,
    MissingReference,   // decoded, but a reference's key holds no picture (mid-grey was used for it)
    NotDecoded,         // the data holds no decodable slice of a picture
    NoMemory,
  };
  // Decodes the one picture `access_unit` holds (Annex B: any parameter sets, then its slices; slices of a second picture are ignored) and gives it in `decoded` (I420,
  // the whole coded size of the frame - for a field picture the frame it is in; valid until the next call). Pictures that are not IDR are decoded even without an IDR
  // before them. `structure` (when not null) gets the picture's structure (3 frame, 1 top field, 2 bottom field).
  [[nodiscard]] ExternalStatus decode_external(ConstByteView access_unit, const ExternalPicture& picture, VideoFrame* decoded, int* structure = nullptr);

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<h264::Core> core_;
};

}  // namespace hardware
