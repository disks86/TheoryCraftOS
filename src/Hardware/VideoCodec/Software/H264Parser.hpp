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
#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// One slice of an access unit, as a decode engine in hardware needs it: the parsed header and where the slice's bits are.
struct H264Slice {
  uint64_t nal_offset = 0;       // of the NAL unit's first byte (its header) in the access unit passed to parse()
  uint64_t nal_size = 0;         // bytes, header byte and emulation prevention bytes included - what an engine reads
  uint64_t data_bit_offset = 0;  // where slice_data() starts, in bits from nal_offset, counted in the bytes as stored (header byte and
                                 // emulation prevention bytes included)
  bool starts_picture = false;   // the first slice of a new picture
  h264::SliceHeader header;
};

// Reads the syntax of an H.264 stream without decoding any picture: parameter sets are kept, and every slice comes out with its parsed header
// and position. This is the half of a decoder that does not depend on how the pixels are made, so a hardware engine (which is programmed from
// the parameter sets and slice headers, and reads the slice data itself) and the CPU decoder see the same stream the same way. Supported:
// what H264Decoder decodes (progressive 8-bit 4:2:0 Baseline/Main/High); anything else is refused at the parameter set or the slice.
class H264StreamParser {
 public:
  explicit H264StreamParser(allocators::IAllocator& allocator) : allocator_(allocator) {}
  H264StreamParser(const H264StreamParser&) = delete;
  H264StreamParser& operator=(const H264StreamParser&) = delete;

  // Takes the parameter sets out of an avcC record and switches the input to length-prefixed NAL units (as H264Decoder::configure).
  bool configure(ConstByteView avcc);
  // Parses one access unit (Annex B, or length-prefixed after configure()): parameter sets are stored, and the slices are appended to `slices`
  // (which is cleared first). False if nothing in the data parsed. A slice that does not parse is skipped, as the decoder does.
  bool parse(ConstByteView access_unit, List<H264Slice>* slices);
  // Forgets the previous slice (after a seek): the next slice starts a picture.
  void reset() { have_prev_ = false; }

  // The parameter sets a slice refers to; nullptr if there are none (cannot happen for a slice parse() returned).
  const h264::Sps* sps_for(const h264::SliceHeader& header) const;
  const h264::Pps* pps_for(const h264::SliceHeader& header) const;

  // The position, counted in the bytes as stored, of bit `rbsp_bits` of a NAL unit's payload (the bytes after its header byte) once the emulation
  // prevention bytes are taken out. `nal` is the whole NAL unit, header byte included.
  static uint64_t stored_bit_offset(const uint8_t* nal, uint64_t size, uint64_t rbsp_bits);

 private:
  bool ensure_state();

  allocators::IAllocator& allocator_;
  UniquePointer<h264::ParameterSets> sets_;
  h264::Mem<uint8_t> rbsp_;
  int nal_length_size_ = 0;
  bool have_prev_ = false;
  h264::SliceHeader prev_;
};

}  // namespace hardware
