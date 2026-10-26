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


// Synthetic VC-1 access units for interlaced sequences, shared by the decoder tests: an Advanced profile sequence header and entry point, and frames of any frame coding mode
// whose picture data is nothing (no byte is zero, so no start code or escape can appear in it). They are enough for everything that reads only the picture-level
// syntax - the Core when an engine decodes the pixels - and for nothing that decodes macroblocks.
#pragma once
#include <stdint.h>
#include <vector>

namespace vc1test {

struct BitWriter {
  std::vector<uint8_t> bytes;
  uint32_t used = 0;
  void put(uint32_t value, uint32_t n) {
    for (uint32_t i = n; i-- > 0;) {
      if (used % 8 == 0) bytes.push_back(0);
      bytes.back() |= static_cast<uint8_t>(((value >> i) & 1) << (7 - used % 8));
      ++used;
    }
  }
  void stop() {   // the stop bit, then zeros to the byte
    put(1, 1);
    while (used % 8) put(0, 1);
  }
};

inline void put_unit(std::vector<uint8_t>* out, uint8_t code, const std::vector<uint8_t>& payload) {
  const uint8_t start[] = {0, 0, 1, code};
  out->insert(out->end(), start, start + 4);
  out->insert(out->end(), payload.begin(), payload.end());
}

// An Advanced profile sequence header (interlaced: 1) and entry point, as gen_vc1_synthetic.py writes them, for 64 x 64 pictures.
inline std::vector<uint8_t> interlaced_headers(bool interlace, bool psf = false) {
  std::vector<uint8_t> out;
  BitWriter seq;
  seq.put(3, 2); seq.put(2, 3); seq.put(1, 2); seq.put(0, 3); seq.put(0, 5); seq.put(0, 1);
  seq.put(31, 12); seq.put(31, 12);   // (64 >> 1) - 1
  seq.put(0, 1); seq.put(interlace ? 1 : 0, 1); seq.put(0, 1); seq.put(0, 1); seq.put(0, 1); seq.put(psf ? 1 : 0, 1);   // broadcast, interlace, tfcntr, finterp, reserved, psf
  seq.put(0, 1); seq.put(0, 1);   // display extended info, hrd
  seq.stop();
  put_unit(&out, 0x0F, seq.bytes);
  BitWriter entry;
  entry.put(0, 1); entry.put(1, 1); entry.put(0, 1); entry.put(0, 1);   // broken link, closed entry, panscan, refdist
  entry.put(0, 1); entry.put(1, 1); entry.put(0, 1); entry.put(0, 2); entry.put(0, 1); entry.put(0, 1);   // loop filter, fastuvmc, extended mv, dquant, vstransform, overlap
  entry.put(0, 2);   // quantizer
  entry.put(0, 1);   // coded size flag
  entry.put(0, 1); entry.put(0, 1);   // range map flags
  entry.stop();
  put_unit(&out, 0x0E, entry.bytes);
  return out;
}

// A frame of an interlaced sequence: its coding mode and type, then bytes that are nothing (no byte is zero, so no start code or escape can appear).
inline std::vector<uint8_t> interlaced_frame(int fcm, int type_or_fptype, bool second_field_unit) {
  std::vector<uint8_t> out;
  BitWriter w;
  if (fcm == 0) w.put(0, 1);
  else if (fcm == 1) w.put(2, 2);
  else w.put(3, 2);
  if (fcm == 2) {
    w.put(static_cast<uint32_t>(type_or_fptype), 3);   // FPTYPE
  } else {
    // PTYPE: unary, P 0, B 10, I 110, BI 1110
    const int ones = type_or_fptype;   // 0 P, 1 B, 2 I, 3 BI
    for (int i = 0; i < ones; ++i) w.put(1, 1);
    w.put(0, 1);
  }
  w.put(1, 1);   // RNDCTRL
  w.put(1, 1);   // UVSAMP
  for (int i = 0; i < 6; ++i) w.put(0x55, 8);
  put_unit(&out, 0x0D, w.bytes);
  if (second_field_unit) put_unit(&out, 0x0C, std::vector<uint8_t>(8, 0x55));
  return out;
}

}  // namespace vc1test
