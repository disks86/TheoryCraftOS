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
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace media {
namespace opus {

// The range decoder of RFC 6716 section 4.1, shared by SILK and CELT. Bytes past the end of the packet read as zero; error() says
// whether the data ran out or a value was out of range.
class RangeDecoder {
 public:
  static constexpr int kBitRes = 3;  // fractional bit resolution of tell_frac()

  void init(ConstByteView data) {
    buf_ = data;
    storage_ = static_cast<uint32_t>(data.size());
    end_offs_ = 0;
    end_window_ = 0;
    nend_bits_ = 0;
    nbits_total_ = kCodeBits + 1 - ((kCodeBits - kCodeExtra) / kSymBits) * kSymBits;
    offs_ = 0;
    rng_ = 1u << kCodeExtra;
    rem_ = read_byte();
    val_ = rng_ - 1 - (rem_ >> (kSymBits - kCodeExtra));
    error_ = false;
    normalize();
  }

  uint32_t storage() const { return storage_; }
  // The bit length of the data in the range decoder so far (rounded up).
  int tell() const { return nbits_total_ - ilog(rng_); }
  // The same in 1/8 bits.
  uint32_t tell_frac() const {
    static const unsigned correction[8] = {35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
    const uint32_t nbits = static_cast<uint32_t>(nbits_total_) << kBitRes;
    int l = ilog(rng_);
    const uint32_t r = rng_ >> (l - 16);
    unsigned b = (r >> 12) - 8;
    b += r > correction[b];
    l = (l << 3) + static_cast<int>(b);
    return nbits - static_cast<uint32_t>(l);
  }
  bool error() const { return error_; }
  // Removes the last `n` bytes from the data the decoder may read (redundancy frames sit there).
  void trim_end(uint32_t n) { storage_ = n < storage_ ? storage_ - n : 0; }
  void add_bits(int n) { nbits_total_ += n; }  // the CELT silence flag skips the rest of the frame
  uint32_t range() const { return rng_; }

  unsigned decode(unsigned ft) {
    ext_ = rng_ / ft;
    const unsigned s = val_ / ext_;
    return ft - (s + 1 < ft ? s + 1 : ft);
  }
  unsigned decode_bin(unsigned bits) {
    ext_ = rng_ >> bits;
    const unsigned s = val_ / ext_;
    return (1u << bits) - (s + 1u < (1u << bits) ? s + 1u : (1u << bits));
  }
  void update(unsigned fl, unsigned fh, unsigned ft) {
    const uint32_t s = ext_ * (ft - fh);
    val_ -= s;
    rng_ = fl > 0 ? ext_ * (fh - fl) : rng_ - s;
    normalize();
  }
  // A bit with probability 1/(1 << logp) of being one.
  int bit_logp(unsigned logp) {
    const uint32_t r = rng_, d = val_, s = r >> logp;
    const int ret = d < s;
    if (!ret) val_ = d - s;
    rng_ = ret ? s : r - s;
    normalize();
    return ret;
  }
  // A symbol from an inverse cumulative distribution table with total 1 << ftb.
  int icdf(const uint8_t* table, unsigned ftb) {
    uint32_t s = rng_, t;
    const uint32_t d = val_, r = s >> ftb;
    int ret = -1;
    do {
      t = s;
      s = r * table[++ret];
    } while (d < s);
    val_ = d - s;
    rng_ = t - s;
    normalize();
    return ret;
  }
  uint32_t uint_(uint32_t ft) {
    ft--;
    int ftb = ilog(ft);
    if (ftb > kUintBits) {
      ftb -= kUintBits;
      const unsigned top = static_cast<unsigned>(ft >> ftb) + 1;
      const unsigned s = decode(top);
      update(s, s + 1, top);
      const uint32_t t = static_cast<uint32_t>(s) << ftb | bits(static_cast<unsigned>(ftb));
      if (t <= ft) return t;
      error_ = true;
      return ft;
    }
    ft++;
    const unsigned s = decode(ft);
    update(s, s + 1, ft);
    return s;
  }
  // Raw bits from the end of the packet.
  uint32_t bits(unsigned n) {
    uint32_t window = end_window_;
    int available = nend_bits_;
    if (static_cast<unsigned>(available) < n) {
      do {
        window |= static_cast<uint32_t>(read_byte_from_end()) << available;
        available += kSymBits;
      } while (available <= 32 - kSymBits);
    }
    const uint32_t ret = window & ((uint32_t{1} << n) - 1u);
    window >>= n;
    available -= static_cast<int>(n);
    end_window_ = window;
    nend_bits_ = available;
    nbits_total_ += static_cast<int>(n);
    return ret;
  }
  // Laplace-distributed value for the energy coder (RFC 6716 section 4.3.2.1).
  int laplace(unsigned fs, int decay) {
    int val = 0;
    unsigned fl = 0;
    const unsigned fm = decode_bin(15);
    if (fm >= fs) {
      val++;
      fl = fs;
      fs = laplace_freq1(fs, decay) + 1;
      while (fs > 1 && fm >= fl + 2 * fs) {
        fs *= 2;
        fl += fs;
        fs = static_cast<unsigned>(((fs - 2) * static_cast<int32_t>(decay)) >> 15);
        fs += 1;
        val++;
      }
      if (fs <= 1) {
        const int di = static_cast<int>((fm - fl) >> 1);
        val += di;
        fl += 2 * static_cast<unsigned>(di);
      }
      if (fm < fl + fs) val = -val;
      else fl += fs;
    }
    update(fl, fl + fs < 32768 ? fl + fs : 32768, 32768);
    return val;
  }

  static int ilog(uint32_t v) {
    int r = 0;
    while (v) {
      ++r;
      v >>= 1;
    }
    return r;
  }

 private:
  static constexpr int kSymBits = 8, kCodeBits = 32, kCodeExtra = (kCodeBits - 2) % kSymBits + 1, kUintBits = 8;
  static constexpr uint32_t kCodeTop = uint32_t{1} << (kCodeBits - 1), kCodeBot = kCodeTop >> kSymBits;

  static unsigned laplace_freq1(unsigned fs0, int decay) {
    const unsigned ft = 32768 - 1 * (2 * 16) - fs0;
    return ft * static_cast<uint32_t>(16384 - decay) >> 15;
  }
  int read_byte() {
    if (offs_ >= storage_) return 0;
    uint8_t b = 0;
    buf_.try_get(offs_++, &b);
    return b;
  }
  int read_byte_from_end() {
    if (end_offs_ >= storage_) return 0;
    uint8_t b = 0;
    buf_.try_get(storage_ - ++end_offs_, &b);
    return b;
  }
  void normalize() {
    while (rng_ <= kCodeBot) {
      nbits_total_ += kSymBits;
      rng_ <<= kSymBits;
      int sym = rem_;
      rem_ = read_byte();
      sym = (sym << kSymBits | rem_) >> (kSymBits - kCodeExtra);
      val_ = ((val_ << kSymBits) + (static_cast<uint32_t>(0xFF) & ~static_cast<uint32_t>(sym))) & (kCodeTop - 1);
    }
  }

  ConstByteView buf_;
  uint32_t storage_ = 0, offs_ = 0, end_offs_ = 0, end_window_ = 0;
  int nend_bits_ = 0, nbits_total_ = 0;
  uint32_t rng_ = 0, val_ = 0, ext_ = 0;
  int rem_ = 0;
  bool error_ = false;
};

}  // namespace opus
}  // namespace media
