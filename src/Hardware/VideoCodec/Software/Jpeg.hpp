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
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"

namespace hardware {

// A JPEG decoder (ITU T.81, Huffman coded, 8-bit samples) with integer arithmetic: sequential (baseline) and progressive
// files, one component (grey) or three (YCbCr) with any sampling up to 2x2 per component, restart markers, any number
// of scans. Arithmetic-coded, 12-bit, lossless, hierarchical and four-component (CMYK) files are refused.
class JpegDecoder {
 public:
  explicit JpegDecoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~JpegDecoder() { release(); }
  JpegDecoder(const JpegDecoder&) = delete;
  JpegDecoder& operator=(const JpegDecoder&) = delete;

  // Decodes a whole JFIF/JPEG file. False for anything it cannot decode (the picture is then not available).
  bool decode(ConstByteView data);
  // The size of the last picture decoded.
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  bool decoded() const { return decoded_; }
  // Writes the last decoded picture into `out` (allocated for the picture's width and height in any pixel format).
  bool output(const VideoFrameBuffer& out) const;
  // The same into a Bgra8888 picture held elsewhere (`pitch` in pixels, at least the width).
  bool output_bgra(uint32_t* dest, uint32_t pitch) const;

 private:
  struct Component {
    uint8_t id = 0, h = 1, v = 1, quant = 0;
    uint32_t blocks_x = 0, blocks_y = 0;  // the plane in whole blocks (padded to the MCU grid)
    ByteView plane;
    ByteView full;                    // after decoding: the plane brought up to the picture's resolution (empty when it already is)
    ArrayView<int16_t> coefficients;  // progressive files: every coefficient of the component until the last scan is in
    int32_t dc_prediction = 0;
    uint8_t dc_table = 0, ac_table = 0;
  };
  struct Huffman {
    bool defined = false;
    uint8_t symbols[256];
    int32_t min_code[17], max_code[18], first_index[17];
  };
  struct Bits;
  struct ScanParams {
    uint32_t spectral_start = 0, spectral_end = 63, approximation_high = 0, approximation_low = 0;
  };

  void release();
  bool parse_frame_header(ConstByteView segment);
  bool parse_huffman(ConstByteView segment);
  bool parse_quant(ConstByteView segment);
  bool decode_scan(ConstByteView data, uint64_t* position, const uint8_t* scan_components, uint32_t scan_count, const ScanParams& params);
  bool decode_block_progressive(Bits& bits, Component& c, int16_t* block, const ScanParams& params, uint32_t* eob_run);
  void finish_progressive();
  void upsample_components();
  bool decode_block(Bits& bits, Component& c, uint8_t* destination, uint32_t stride);
  uint32_t sample(uint32_t component, uint32_t x, uint32_t y) const;

  allocators::IAllocator& allocator_;
  uint16_t quant_[4][64];
  bool quant_defined_[4] = {false, false, false, false};
  Huffman dc_[4], ac_[4];
  Component components_[3];
  uint32_t component_count_ = 0;
  uint32_t width_ = 0, height_ = 0;
  uint32_t max_h_ = 1, max_v_ = 1;
  uint32_t restart_interval_ = 0;
  bool decoded_ = false;
  bool frame_seen_ = false;
  bool progressive_ = false;
};

// A baseline JPEG encoder: 4:2:0 YCbCr (or one grey component never - always colour), quality 1-100 (libjpeg's
// scale), Huffman tables optimised for each picture. Integer arithmetic throughout.
class JpegEncoder {
 public:
  explicit JpegEncoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~JpegEncoder() { release_view(allocator_, buffer_); }
  JpegEncoder(const JpegEncoder&) = delete;
  JpegEncoder& operator=(const JpegEncoder&) = delete;

  // Encodes `frame` (any pixel format; converted as needed). The file is then in output(). False if the frame is
  // invalid or memory ran out.
  bool encode(const VideoFrame& frame, uint32_t quality);
  ConstByteView output() const { return ConstByteView(buffer_).first(length_); }

 private:
  struct Tables;
  struct Writer;
  allocators::IAllocator& allocator_;
  ByteView buffer_;  // grown as the file is written
  uint64_t length_ = 0;
};

}  // namespace hardware
