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


// The CABAC arithmetic decoder of H.265 (9.3.4.3) and the numbering of its context variables. The engine is the one of H.264 (the same probability tables); it reads
// the bits of the slice segment data one at a time through a BitReader, so after a terminating bin the reader is exactly at the end of the arithmetic code word, which is
// where the next substream or the PCM samples begin once aligned to a byte.
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h265 {

static constexpr uint8_t kCabacRangeLps[256] = {
128,176,208,240,128,167,197,227,128,158,187,216,123,150,178,205,
116,142,169,195,111,135,160,185,105,128,152,175,100,122,144,166,
95,116,137,158,90,110,130,150,85,104,123,142,81,99,117,135,
77,94,111,128,73,89,105,122,69,85,100,116,66,80,95,110,
62,76,90,104,59,72,86,99,56,69,81,94,53,65,77,89,
51,62,73,85,48,59,69,80,46,56,66,76,43,53,63,72,
41,50,59,69,39,48,56,65,37,45,54,62,35,43,51,59,
33,41,48,56,32,39,46,53,30,37,43,50,29,35,41,48,
27,33,39,45,26,31,37,43,24,30,35,41,23,28,33,39,
22,27,32,37,21,26,30,35,20,24,29,33,19,23,27,31,
18,22,26,30,17,21,25,28,16,20,23,27,15,19,22,25,
14,18,21,24,14,17,20,23,13,16,19,22,12,15,18,21,
12,14,17,20,11,14,16,19,11,13,15,18,10,12,15,17,
10,12,14,16,9,11,13,15,9,11,12,14,8,10,12,14,
8,9,11,13,7,9,11,12,7,9,10,12,7,8,10,11,
6,8,9,11,6,7,9,10,6,7,8,9,2,2,2,2};
static constexpr uint8_t kCabacStateNext[128] = {
0,1,0,2,1,3,2,4,2,5,4,6,4,7,5,8,
6,9,7,10,8,11,9,12,9,13,11,14,11,15,12,16,
13,17,13,18,15,19,15,20,16,21,16,22,18,23,18,24,
19,25,19,26,21,27,21,28,22,29,22,30,23,31,24,32,
24,33,25,34,26,35,26,36,27,37,27,38,28,39,29,40,
29,41,30,42,30,43,30,44,31,45,32,46,32,47,33,48,
33,49,33,50,34,51,34,52,35,53,35,54,35,55,36,56,
36,57,36,58,37,59,37,60,37,61,38,62,38,62,63,63};

// Context variables, in the order of H265CabacInit.inc. The contexts of an element are consecutive from its first.
enum Ctx : int {
  kCtxSaoMerge = 0, kCtxSaoType = 1, kCtxSplitCu = 2, kCtxTransquantBypass = 5, kCtxSkip = 6, kCtxCuQpDelta = 9, kCtxPredMode = 11, kCtxPartMode = 12,
  kCtxPrevIntraLuma = 16, kCtxChromaPred = 17, kCtxMergeFlag = 18, kCtxMergeIdx = 19, kCtxInterPredIdc = 20, kCtxRefIdx = 25, kCtxMvpFlag = 27, kCtxRqtRootCbf = 28,
  kCtxSplitTransform = 29, kCtxCbfLuma = 32, kCtxCbfChroma = 34, kCtxMvdGreater0 = 38, kCtxMvdGreater1 = 39, kCtxTransformSkip = 40, kCtxLastX = 42, kCtxLastY = 60,
  kCtxCodedSubBlock = 78, kCtxSigCoeff = 82, kCtxGreater1 = 124, kCtxGreater2 = 148, kContextCount = 154
};

class Cabac {
 public:
  void attach(h264::BitReader* reader) { reader_ = reader; }
  // 9.3.2.5: the arithmetic decoder starts at the reader's position.
  void init_engine() {
    range_ = 510;
    offset_ = reader_->u(9);
  }
  // 9.3.2.2: the context variables for a slice of this type (0: I, 1 or 2: P/B) and quantiser.
  void init_contexts(int init_type, int slice_qp);
  int decision(int ctx) {
    uint8_t& st = state_[ctx];
    int state = st >> 1, mps = st & 1;
    const uint32_t lps = kCabacRangeLps[state * 4 + ((range_ >> 6) & 3)];
    range_ -= lps;
    int bin;
    if (offset_ >= range_) {
      bin = !mps;
      offset_ -= range_;
      range_ = lps;
      if (state == 0) mps = 1 - mps;
      state = kCabacStateNext[state * 2];
    } else {
      bin = mps;
      state = kCabacStateNext[state * 2 + 1];
    }
    st = static_cast<uint8_t>((state << 1) | mps);
    renorm();
    return bin;
  }
  int bypass() {
    offset_ = (offset_ << 1) | reader_->bit();
    if (offset_ >= range_) {
      offset_ -= range_;
      return 1;
    }
    return 0;
  }
  int bypass_bits(int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | bypass();
    return v;
  }
  int terminate() {
    range_ -= 2;
    if (offset_ >= range_) return 1;
    renorm();
    return 0;
  }
  // Saves / restores the context variables (the synchronisation of wavefront rows).
  void save(uint8_t out[kContextCount]) const { for (int i = 0; i < kContextCount; ++i) out[i] = state_[i]; }
  void restore(const uint8_t in[kContextCount]) { for (int i = 0; i < kContextCount; ++i) state_[i] = in[i]; }
  h264::BitReader* reader() const { return reader_; }

 private:
  void renorm() {
    while (range_ < 256) {
      range_ <<= 1;
      offset_ = (offset_ << 1) | reader_->bit();
    }
  }
  h264::BitReader* reader_ = nullptr;
  uint32_t range_ = 510, offset_ = 0;
  uint8_t state_[kContextCount] = {};   // (probability state << 1) | most probable symbol
};

}  // namespace h265
}  // namespace hardware
