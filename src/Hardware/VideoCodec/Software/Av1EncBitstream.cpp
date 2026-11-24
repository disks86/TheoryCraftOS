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

// The symbol encoder of AV1 (Av1EncBitstream.hpp): libaom's od_ec_enc (aom_dsp/entenc.c, BSD-2-Clause, Copyright (c) 2001-2016 Xiph.Org Foundation
// and contributors, Alliance for Open Media) in the 32-bit window form, with the CDF adaptation of av1::Msac.
#include "Hardware/VideoCodec/Software/Av1EncBitstream.hpp"

namespace hardware {
namespace av1 {

namespace {
constexpr unsigned kProbShift = 6;   // EC_PROB_SHIFT
constexpr unsigned kMinProb = 4;     // EC_MIN_PROB
inline int ilog_nz(unsigned v) { return 32 - __builtin_clz(v); }
}  // namespace

bool append_obu(ByteBuffer* out, unsigned type, ConstByteView payload) {
  bool ok = out->put8(static_cast<uint8_t>((type << 3) | 2));
  uint64_t size = payload.size();
  do {
    uint8_t b = static_cast<uint8_t>(size & 0x7F);
    size >>= 7;
    if (size) b |= 0x80;
    ok = out->put8(b) && ok;
  } while (size);
  return out->append(payload) && ok;
}

void SymbolWriter::normalize(uint32_t low, unsigned rng) {
  int c = cnt_;
  const int d = 16 - ilog_nz(rng);
  int s = c + d;
  if (s >= 0) {
    c += 16;
    uint32_t m = (1u << c) - 1;
    if (s >= 8) {
      ok_ = precarry_.push_back(static_cast<uint16_t>(low >> c)) && ok_;
      low &= m;
      c -= 8;
      m >>= 8;
    }
    ok_ = precarry_.push_back(static_cast<uint16_t>(low >> c)) && ok_;
    s = c + d - 24;
    low &= m;
  }
  low_ = low << d;
  rng_ = rng << d;
  cnt_ = s;
}

void SymbolWriter::encode_q15(unsigned fl, unsigned fh, int s, int n) {
  uint32_t l = low_;
  unsigned r = rng_;
  if (fl < 32768u) {
    const unsigned u = (((r >> 8) * (fl >> kProbShift)) >> (7 - kProbShift)) + kMinProb * static_cast<unsigned>(n - (s - 1));
    const unsigned v = (((r >> 8) * (fh >> kProbShift)) >> (7 - kProbShift)) + kMinProb * static_cast<unsigned>(n - s);
    l += r - u;
    r = u - v;
  } else {
    r -= (((r >> 8) * (fh >> kProbShift)) >> (7 - kProbShift)) + kMinProb * static_cast<unsigned>(n - s);
  }
  normalize(l, r);
}

void SymbolWriter::symbol(ArrayView<uint16_t> cdf, unsigned n_symbols, unsigned s, bool adapt) {
  if (s > n_symbols) {
    ok_ = false;
    return;
  }
  const unsigned count = cdf[n_symbols];   // (the counter; reading it checks the view is long enough)
  const unsigned fl = s > 0 ? cdf[s - 1] : 32768u;
  const unsigned fh = s < n_symbols ? cdf[s] : 0u;
  encode_q15(fl, fh, static_cast<int>(s), static_cast<int>(n_symbols));
  if (adapt) {
    const unsigned rate = 4 + (count >> 4) + (n_symbols > 2);
    unsigned i = 0;
    for (; i < s; ++i) cdf[i] = static_cast<uint16_t>(cdf[i] + ((32768u - cdf[i]) >> rate));
    for (; i < n_symbols; ++i) cdf[i] = static_cast<uint16_t>(cdf[i] - (cdf[i] >> rate));
    cdf[n_symbols] = static_cast<uint16_t>(count + (count < 32));
  }
}

void SymbolWriter::bool_prob(unsigned f, unsigned bit) {
  // (a two-symbol CDF whose first entry is f: Msac::decode_bool's v is the symbol coder's v of symbol 0)
  const unsigned b = bit ? 1u : 0u;
  encode_q15(b ? f : 32768u, b ? 0u : f, static_cast<int>(b), 1);
}

bool SymbolWriter::finish(ByteBuffer* out) {
  uint32_t l = low_;
  int c = cnt_;
  int s = 10;
  const uint32_t m = 0x3FFF;
  uint32_t e = ((l + m) & ~m) | (m + 1);
  s += c;
  if (s > 0) {
    uint32_t n = (1u << (c + 16)) - 1;
    do {
      ok_ = precarry_.push_back(static_cast<uint16_t>(e >> (c + 16))) && ok_;
      e &= n;
      s -= 8;
      c -= 8;
      n >>= 8;
    } while (s > 0);
  }
  if (!ok_) return false;
  const uint64_t count = precarry_.count();
  const uint64_t start = out->size();
  if (!out->put_zeros(count)) return false;
  ByteView bytes = out->writable_view().subview(start, count);
  unsigned carry = 0;
  for (uint64_t i = count; i-- > 0;) {
    carry = precarry_[i] + carry;
    bytes[i] = static_cast<uint8_t>(carry);
    carry >>= 8;
  }
  return true;
}

}  // namespace av1
}  // namespace hardware
