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

// The bit-level writers of the AV1 picture encoder (Av1PictureEncoder.hpp): the f(n) / su(n) / ns(n) / leb128 writer of the OBU headers and the symbol
// coder of the tile data, the inverse of av1::Msac (libaom's od_ec encoder: the same CDF arithmetic, the same carry handling and the same final flush).
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/Software/Av1Cdf.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace av1 {

// Bits MSB first (the OBU syntax). A write that cannot get memory makes ok() false.
class HeaderBits {
 public:
  explicit HeaderBits(allocators::IAllocator& allocator) : buf_(allocator) {}
  void bit(unsigned b) {
    acc_ = static_cast<uint8_t>((acc_ << 1) | (b & 1));
    if (++count_ == 8) {
      ok_ = buf_.put8(acc_) && ok_;
      acc_ = 0;
      count_ = 0;
    }
  }
  void put(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) bit((value >> i) & 1);
  }
  // su(1 + bits): two's complement.
  void su(int value, int n) { put(static_cast<uint32_t>(value) & ((n >= 32) ? ~0u : ((1u << n) - 1)), n); }
  // ns(n) (av1 BitReader::uniform's inverse): v in [0, n).
  void ns(unsigned n, unsigned v) {
    if (n <= 1) return;
    int l = 0;
    while ((n >> l) > 1) ++l;
    ++l;
    const unsigned m = (1u << l) - n;
    if (v < m) {
      put(v, l - 1);
    } else {
      put((v + m) >> 1, l - 1);
      bit((v + m) & 1);
    }
  }
  void byte_align() {
    while (count_ != 0) bit(0);
  }
  void trailing() {
    bit(1);
    byte_align();
  }
  uint64_t bits() const { return buf_.size() * 8 + static_cast<uint64_t>(count_); }
  bool ok() const { return ok_; }
  ConstByteView bytes() const { return buf_.view(); }   // (whole bytes only: byte_align first)
  void clear() {
    buf_.clear();
    acc_ = 0;
    count_ = 0;
    ok_ = true;
  }

 private:
  ByteBuffer buf_;
  uint8_t acc_ = 0;
  int count_ = 0;
  bool ok_ = true;
};

// One OBU with obu_has_size_field 1 (no extension) around `payload`.
[[nodiscard]] bool append_obu(ByteBuffer* out, unsigned type, ConstByteView payload);

// The symbol encoder (od_ec_enc with a 32-bit window). The CDFs are the decoder's (inverse CDFs with the adaptation counter after the
// n_symbols entries, see Msac::symbol_adapt): encode() adapts them exactly as the decoder does when `adapt` is set.
class SymbolWriter {
 public:
  explicit SymbolWriter(allocators::IAllocator& allocator) : precarry_(allocator) {}
  void reset() {
    precarry_.clear();
    low_ = 0;
    rng_ = 0x8000;
    cnt_ = -9;
    ok_ = true;
  }
  // Symbol `s` of an adaptive CDF of n_symbols + 1 symbols (dav1d's n_symbols).
  void symbol(ArrayView<uint16_t> cdf, unsigned n_symbols, unsigned s, bool adapt);
  // A bit with the probability f / 32768 of being... as Msac::decode_bool reads it (non-adaptive).
  void bool_prob(unsigned f, unsigned bit);
  void bool_equi(unsigned bit) { bool_prob(16384, bit); }
  // Flushes the coder and appends the bytes (libaom's od_ec_enc_done with carry propagation).
  [[nodiscard]] bool finish(ByteBuffer* out);
  bool ok() const { return ok_; }

 private:
  void encode_q15(unsigned fl, unsigned fh, int s, int n);
  void normalize(uint32_t low, unsigned rng);
  List<uint16_t> precarry_;
  uint32_t low_ = 0;
  unsigned rng_ = 0x8000;
  int cnt_ = -9;
  bool ok_ = true;
};

}  // namespace av1
}  // namespace hardware
