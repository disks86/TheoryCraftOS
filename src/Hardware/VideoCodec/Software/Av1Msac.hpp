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
#include "Utility/Bounds.hpp"

// The symbol-adaptive range decoder of AV1 (the "msac"), a port of dav1d's src/msac.c and src/msac.h (BSD-2-Clause, Copyright (c)
// 2018 VideoLAN and dav1d authors, Two Orioles LLC), which the output is checked against bit for bit.
//
// CDFs are passed as ArrayView<uint16_t> over the innermost array of the CDF context, e.g. msac.symbol_adapt(ctx.m.txsz[a][b], 2).
// `n_symbols` is dav1d's: the number of symbols minus one, so an array of n_symbols + 1 entries (the last is the adaptation counter).
// A view that is too short stops the OS (checked access), it never reads or writes outside the CDF.
// The data is read through a ConstByteView; past its end the decoder reads one bits, exactly like dav1d.

namespace hardware {
namespace av1 {

class Msac {
 public:
  Msac() = default;
  // `data` must stay alive and unchanged while the decoder is used.
  Msac(ConstByteView data, bool disable_cdf_update) { init(data, disable_cdf_update); }

  void init(ConstByteView data, bool disable_cdf_update) {
    data_ = data;
    pos_ = 0;
    dif_ = 0;
    rng_ = 0x8000;
    cnt_ = -15;
    allow_update_cdf_ = !disable_cdf_update;
    refill();
  }

  // Decodes a symbol from an adaptive CDF (dav1d_msac_decode_symbol_adapt4/8/16/_c). Supported n_symbols: adapt4 1-3, adapt8 1-7,
  // adapt16 3-15 (the three names are kept so call sites read like dav1d; they are all the same code here).
  unsigned symbol_adapt(ArrayView<uint16_t> cdf, unsigned n_symbols) {
    const uint64_t c = dif_ >> (kWinSize - 16);
    const unsigned r = rng_ >> 8;
    unsigned u = rng_, v = rng_, val = 0;
    // cdf[n_symbols] is the counter; reading it first checks that the view is long enough for everything below.
    const unsigned count = cdf[n_symbols];
    for (;; ++val) {
      u = v;
      if (val >= n_symbols) {  // dav1d reaches val == n_symbols with v == 0 for a valid CDF (counter <= 32): stop there
        v = 0;
        break;
      }
      v = ((r * (static_cast<unsigned>(cdf[val]) >> kProbShift)) >> (7 - kProbShift)) + kMinProb * (n_symbols - val);
      if (c >= v) break;
    }
    norm(dif_ - (static_cast<uint64_t>(v) << (kWinSize - 16)), u - v);
    if (allow_update_cdf_) {
      const unsigned rate = 4 + (count >> 4) + (n_symbols > 2);
      unsigned i = 0;
      for (; i < val; ++i) cdf[i] = static_cast<uint16_t>(cdf[i] + ((32768u - cdf[i]) >> rate));
      for (; i < n_symbols; ++i) cdf[i] = static_cast<uint16_t>(cdf[i] - (cdf[i] >> rate));
      cdf[n_symbols] = static_cast<uint16_t>(count + (count < 32));
    }
    return val;
  }
  unsigned symbol_adapt4(ArrayView<uint16_t> cdf, unsigned n_symbols) { return symbol_adapt(cdf, n_symbols); }
  unsigned symbol_adapt8(ArrayView<uint16_t> cdf, unsigned n_symbols) { return symbol_adapt(cdf, n_symbols); }
  unsigned symbol_adapt16(ArrayView<uint16_t> cdf, unsigned n_symbols) { return symbol_adapt(cdf, n_symbols); }

  // A binary value with an adaptive probability: cdf[0] = probability, cdf[1] = counter (dav1d_msac_decode_bool_adapt).
  unsigned bool_adapt(ArrayView<uint16_t> cdf) {
    const unsigned bit = decode_bool(cdf[0]);
    if (allow_update_cdf_) {
      const unsigned count = cdf[1];
      const unsigned rate = 4 + (count >> 4);
      if (bit)
        cdf[0] = static_cast<uint16_t>(cdf[0] + ((32768u - cdf[0]) >> rate));
      else
        cdf[0] = static_cast<uint16_t>(cdf[0] - (cdf[0] >> rate));
      cdf[1] = static_cast<uint16_t>(count + (count < 32));
    }
    return bit;
  }

  // A value with probability one half (dav1d_msac_decode_bool_equi).
  unsigned bool_equi() {
    const unsigned r = rng_;
    uint64_t dif = dif_;
    unsigned v = ((r >> 8) << 7) + kMinProb;
    const uint64_t vw = static_cast<uint64_t>(v) << (kWinSize - 16);
    const unsigned ret = dif >= vw;
    dif -= ret * vw;
    v += ret * (r - 2 * v);
    norm(dif, v);
    return !ret;
  }

  // A value whose probability of being one is f / 32768 (dav1d_msac_decode_bool).
  unsigned decode_bool(unsigned f) {
    const unsigned r = rng_;
    uint64_t dif = dif_;
    unsigned v = ((r >> 8) * (f >> kProbShift) >> (7 - kProbShift)) + kMinProb;
    const uint64_t vw = static_cast<uint64_t>(v) << (kWinSize - 16);
    const unsigned ret = dif >= vw;
    dif -= ret * vw;
    v += ret * (r - 2 * v);
    norm(dif, v);
    return !ret;
  }

  // The high coefficient token (br_tok CDF, 4 entries): 3..14 (dav1d_msac_decode_hi_tok).
  unsigned hi_tok(ArrayView<uint16_t> cdf) {
    unsigned tok_br = symbol_adapt(cdf, 3);
    unsigned tok = 3 + tok_br;
    if (tok_br == 3) {
      tok_br = symbol_adapt(cdf, 3);
      tok = 6 + tok_br;
      if (tok_br == 3) {
        tok_br = symbol_adapt(cdf, 3);
        tok = 9 + tok_br;
        if (tok_br == 3) tok = 12 + symbol_adapt(cdf, 3);
      }
    }
    return tok;
  }

  // n equiprobable bits, most significant first (dav1d_msac_decode_bools).
  unsigned bools(unsigned n) {
    unsigned v = 0;
    while (n--) v = (v << 1) | bool_equi();
    return v;
  }

  // A value in [0, n), n > 0 (dav1d_msac_decode_uniform).
  int uniform(unsigned n) {
    if (n == 0) return 0;  // dav1d asserts n > 0
    const int l = (31 ^ __builtin_clz(n)) + 1;
    if (l <= 1) return 0;  // n == 1: dav1d asserts l > 1 (never happens in a legal stream)
    const unsigned m = (1u << l) - n;
    const unsigned v = bools(static_cast<unsigned>(l - 1));
    return static_cast<int>(v < m ? v : (v << 1) - m + bool_equi());
  }

  // Sub-exponential value in [0, n) around `ref` (dav1d_msac_decode_subexp); n >> k must be 8 in a legal stream.
  int subexp(int ref, int n, unsigned k) {
    unsigned a = 0;
    if (bool_equi()) {
      if (bool_equi()) k += bool_equi() + 1;
      a = 1u << k;
    }
    const unsigned v = bools(k) + a;
    return ref * 2 <= n ? static_cast<int>(inv_recenter(static_cast<unsigned>(ref), v))
                        : n - 1 - static_cast<int>(inv_recenter(static_cast<unsigned>(n - 1 - ref), v));
  }

  // Exp-Golomb value (dav1d's read_golomb in recon_tmpl.c).
  unsigned golomb() {
    int len = 0;
    unsigned val = 1;
    while (!bool_equi() && len < 32) ++len;
    while (len--) val = (val << 1) + bool_equi();
    return val - 1;
  }

  // State, for tests and debugging.
  unsigned rng() const { return rng_; }
  uint64_t dif() const { return dif_; }
  int cnt() const { return cnt_; }
  uint64_t position() const { return pos_; }

 private:
  static constexpr unsigned kProbShift = 6;
  static constexpr unsigned kMinProb = 4;  // must be <= (1 << kProbShift) / 16
  static constexpr int kWinSize = 64;

  static unsigned inv_recenter(unsigned r, unsigned v) {
    if (v > (r << 1)) return v;
    if ((v & 1) == 0) return (v >> 1) + r;
    return r - ((v + 1) >> 1);
  }

  // Fills dif_ with whole bytes from the stream; past the end the remaining bits become ones (inverted zero bytes).
  void refill() {
    int c = kWinSize - cnt_ - 24;
    uint64_t dif = dif_;
    do {
      uint8_t byte = 0;
      if (!data_.try_get(pos_, &byte)) {
        dif |= ~(~static_cast<uint64_t>(0xff) << c);  // c is in 0..55 here
        break;
      }
      ++pos_;
      dif |= static_cast<uint64_t>(byte ^ 0xff) << c;
      c -= 8;
    } while (c >= 0);
    dif_ = dif;
    cnt_ = kWinSize - c - 24;
  }

  // Renormalises so that 32768 <= rng < 65536, reading more bytes when needed.
  void norm(uint64_t dif, unsigned rng) {
    const int d = 15 ^ (31 ^ __builtin_clz(rng ? rng : 1u));
    const int cnt = cnt_;
    dif_ = dif << d;
    rng_ = rng << d;
    cnt_ = cnt - d;
    if (static_cast<unsigned>(cnt) < static_cast<unsigned>(d)) refill();  // unsigned compare avoids redundant refills at the end
  }

  ConstByteView data_;
  uint64_t pos_ = 0;
  uint64_t dif_ = 0;
  unsigned rng_ = 0;
  int cnt_ = 0;
  bool allow_update_cdf_ = false;
};

}  // namespace av1
}  // namespace hardware
