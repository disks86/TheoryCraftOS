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

// AAC-LC decoder (MPEG-2/4 Advanced Audio Coding, Low Complexity profile) and an ADTS reader around it.
//
// Implemented: all the LC coding tools - spectral Huffman codebooks 1-11 with escapes, scalefactors, section and window-group
// handling, short-block sequences with sine and Kaiser-Bessel-derived windows, pulse data, temporal noise shaping, mid/side
// and intensity stereo, perceptual noise substitution - through to PCM. Channels beyond the front pair are decoded but not
// output. The Huffman code lengths and codewords and the scalefactor-band widths are the ones in ISO/IEC 14496-3, here taken
// from libxaac (Ittiam Systems, Apache License 2.0); everything else is computed.
// Not handled: Main/LTP prediction, scalable and SSR profiles, coupling channels, gain control, SBR/PS (an HE-AAC stream
// decodes as its AAC-LC core at the core sampling rate), error-resilient and other non-LC object types.
#include "Math/FloatMath.hpp"
#include "Media/Audio/AacInternal.hpp"
#include "Media/Audio/AacTables.hpp"
#include "Media/Audio/Mdct.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"

namespace media {
namespace aac {

namespace {
constexpr uint8_t kTnsMaxBandsLong[13] = {31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39};
constexpr uint8_t kTnsMaxBandsShort[13] = {9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14};

enum WindowSequence { OnlyLong = 0, LongStart = 1, EightShort = 2, LongStop = 3 };
enum BandType { ZeroBand = 0, NoiseBand = 13, IntensityBand2 = 14, IntensityBand = 15 };


struct Bits {
  ConstByteView d;
  uint64_t pos = 0;
  bool overrun = false;
  uint32_t get1() {
    const uint64_t byte = pos >> 3;
    if (byte >= d.size()) { overrun = true; return 0; }
    const uint32_t bit = (d[byte] >> (7 - (pos & 7))) & 1u;
    ++pos;
    return bit;
  }
  uint32_t get(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) v = (v << 1) | get1();
    return v;
  }
  void skip(uint64_t n) { pos += n; if (pos > d.size() * 8) overrun = true; }
  void align() { pos = (pos + 7) & ~uint64_t{7}; }
};

struct Tns {
  bool present = false;
  uint8_t n_filt[8] = {};
  uint8_t length[8][3] = {}, order[8][3] = {};
  bool direction[8][3] = {};
  float lpc[8][3][12] = {};
};

struct Pulse {
  bool present = false;
  uint8_t count = 0, start_sfb = 0, offset[4] = {}, amp[4] = {};
};

struct IcsInfo {
  uint8_t window_sequence = 0, window_shape = 0, max_sfb = 0, num_window_groups = 1, group_len[8] = {1};
  uint8_t num_windows = 1, num_swb = 0, tns_max_bands = 0;
  const uint8_t* widths = nullptr;  // band widths for this window type
  uint16_t swb_offset[53] = {};
  bool is_short() const { return window_sequence == EightShort; }
};

struct Channel {
  IcsInfo ics;
  uint8_t global_gain = 0;
  uint8_t band_type[8 * 51] = {};
  int sf[8 * 51] = {};          // scalefactor, noise energy, or intensity position, by band
  Tns tns;
  Pulse pulse;
  int16_t q[1024];
  float coef[1024];
};

struct ChannelState {
  float overlap[1024] = {};
  uint8_t prev_shape = 0;
};

float bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 60; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return static_cast<float>(sum);
}

}  // namespace

class Decoder {
 public:
  explicit Decoder(allocators::IAllocator& allocator) : allocator_(allocator), mdct_long_(allocator), mdct_short_(allocator) {}
  ~Decoder() { allocator_.deallocate(trees_); allocator_.deallocate(pow43_); }
  bool init(uint32_t sampling_index);
  uint32_t sample_rate() const { return kSampleRates[sampling_index_]; }
  // Decodes one raw_data_block into `pcm` (interleaved int16, 1024 frames) for `channels()` output channels. False if the block is
  // damaged or uses a tool that is not supported.
  bool decode_block(ConstByteView data, int16_t* pcm);
  uint32_t channels() const { return out_channels_; }
  void reset_state() {
    for (ChannelState& st : states_) st = ChannelState();
    random_ = 0x1F2E3D4C;
  }

 private:
  bool parse_ics_info(Bits& b, IcsInfo* ics);
  bool parse_channel(Bits& b, Channel* ch, const IcsInfo* common);
  bool parse_section_data(Bits& b, Channel* ch);
  bool parse_scalefactors(Bits& b, Channel* ch);
  bool parse_pulse(Bits& b, Channel* ch);
  bool parse_tns(Bits& b, Channel* ch);
  bool parse_spectrum(Bits& b, Channel* ch);
  int huff(Bits& b, int book);
  void dequantize(Channel* ch);
  void apply_tns(Channel* ch);
  void synthesize(Channel* ch, ChannelState* st, float* out);

  allocators::IAllocator& allocator_;
  Mdct mdct_long_, mdct_short_;
  uint32_t sampling_index_ = 4;
  int16_t* trees_ = nullptr;   // 12 binary trees, node pairs
  uint32_t tree_base_[13] = {};
  float* pow43_ = nullptr;
  float long_slope_[2][1024];
  float short_slope_[2][128];
  uint32_t random_ = 0x1F2E3D4C;
  uint32_t out_channels_ = 0;
  Channel ch_[2];
  ChannelState states_[16];
  int chosen_type_ = -1, chosen_tag_ = -1;
  bool ms_used_[8 * 51];
};

bool Decoder::init(uint32_t sampling_index) {
  if (sampling_index >= 13) return false;
  sampling_index_ = sampling_index;
  // Binary trees for the Huffman codes: node i has children at [2i], [2i+1]; a negative entry ~s is the symbol s, 0 is invalid.
  uint32_t total = 0;
  for (int i = 0; i < 12; ++i) { tree_base_[i] = total; total += kBooks[i].count * 2; }
  tree_base_[12] = total;
  trees_ = static_cast<int16_t*>(allocator_.allocate(sizeof(int16_t) * total));
  if (!trees_) return false;
  for (uint32_t i = 0; i < total; ++i) trees_[i] = 0;
  for (int bk = 0; bk < 12; ++bk) {
    int16_t* tree = trees_ + tree_base_[bk];
    uint32_t nodes = 1;
    for (uint32_t s = 0; s < kBooks[bk].count; ++s) {
      const uint32_t len = kBooks[bk].lens[s], code = kBooks[bk].codes[s];
      uint32_t node = 0;
      for (uint32_t k = 0; k < len; ++k) {
        const uint32_t bit = (code >> (len - 1 - k)) & 1;
        int16_t& child = tree[node * 2 + bit];
        if (k + 1 == len) {
          child = static_cast<int16_t>(~s);
        } else {
          if (child == 0) child = static_cast<int16_t>(nodes++);
          node = static_cast<uint32_t>(child);
        }
      }
    }
  }
  // x^(4/3) for the quantised values that can be looked up; larger ones (escapes) are computed.
  pow43_ = static_cast<float*>(allocator_.allocate(sizeof(float) * 8192));
  if (!pow43_) return false;
  for (int i = 0; i < 8192; ++i) pow43_[i] = static_cast<float>(fmath::pow(i, 4.0 / 3.0));
  // Window slopes: sine and Kaiser-Bessel-derived (alpha 4 for long, 6 for short windows).
  const double pi = 3.14159265358979323846;
  for (int i = 0; i < 1024; ++i) long_slope_[0][i] = static_cast<float>(fmath::sin(pi * (i + 0.5) / 2048.0));
  for (int i = 0; i < 128; ++i) short_slope_[0][i] = static_cast<float>(fmath::sin(pi * (i + 0.5) / 256.0));
  for (int shape = 1; shape < 2; ++shape) {
    for (int pass = 0; pass < 2; ++pass) {
      const int half = pass == 0 ? 1024 : 128;
      const double alpha = pass == 0 ? 4.0 : 6.0;
      float kaiser[1025];
      double total_sum = 0;
      for (int j = 0; j <= half; ++j) {
        const double r = 2.0 * j / half - 1.0;
        kaiser[j] = bessel_i0(pi * alpha * fmath::sqrt(1.0 - r * r));
        total_sum += kaiser[j];
      }
      double run = 0;
      for (int n = 0; n < half; ++n) {
        run += kaiser[n];
        const float w = static_cast<float>(fmath::sqrt(run / total_sum));
        if (pass == 0) long_slope_[1][n] = w; else short_slope_[1][n] = w;
      }
    }
  }
  return mdct_long_.init(2048) && mdct_short_.init(256);
}

int Decoder::huff(Bits& b, int book) {
  const int16_t* tree = trees_ + tree_base_[book];
  uint32_t node = 0;
  for (int depth = 0; depth < 20; ++depth) {
    const int16_t next = tree[node * 2 + b.get1()];
    if (b.overrun) return -1;
    if (next < 0) return ~next;
    if (next == 0) return -1;
    node = static_cast<uint32_t>(next);
  }
  return -1;
}

bool Decoder::parse_ics_info(Bits& b, IcsInfo* ics) {
  b.get1();  // reserved
  ics->window_sequence = static_cast<uint8_t>(b.get(2));
  ics->window_shape = static_cast<uint8_t>(b.get1());
  const bool is_short = ics->window_sequence == EightShort;
  const int rate = static_cast<int>(sampling_index_);
  static const uint8_t* const kLongWidths[13] = {kWidthsL96, kWidthsL96, kWidthsL64, kWidthsL48, kWidthsL48, kWidthsL32, kWidthsL24, kWidthsL24, kWidthsL16, kWidthsL16, kWidthsL16, kWidthsL8, kWidthsL8};
  static const uint8_t kLongCount[13] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40, 40};
  static const uint8_t* const kShortWidths[13] = {kWidthsS96, kWidthsS96, kWidthsS96, kWidthsS48, kWidthsS48, kWidthsS48, kWidthsS24, kWidthsS24, kWidthsS16, kWidthsS16, kWidthsS16, kWidthsS8, kWidthsS8};
  static const uint8_t kShortCount[13] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15};
  ics->widths = is_short ? kShortWidths[rate] : kLongWidths[rate];
  ics->num_swb = is_short ? kShortCount[rate] : kLongCount[rate];
  ics->tns_max_bands = is_short ? kTnsMaxBandsShort[rate] : kTnsMaxBandsLong[rate];
  uint16_t at = 0;
  for (int i = 0; i < ics->num_swb; ++i) { ics->swb_offset[i] = at; at = static_cast<uint16_t>(at + ics->widths[i]); }
  ics->swb_offset[ics->num_swb] = at;
  if (is_short) {
    ics->max_sfb = static_cast<uint8_t>(b.get(4));
    const uint32_t grouping = b.get(7);
    ics->num_windows = 8;
    ics->num_window_groups = 1;
    ics->group_len[0] = 1;
    for (int i = 0; i < 7; ++i) {
      if ((grouping >> (6 - i)) & 1) ++ics->group_len[ics->num_window_groups - 1];
      else ics->group_len[ics->num_window_groups++] = 1;
    }
  } else {
    ics->max_sfb = static_cast<uint8_t>(b.get(6));
    if (b.get1()) return false;  // predictor_data_present: Main profile prediction is not supported
    ics->num_windows = 1;
    ics->num_window_groups = 1;
    ics->group_len[0] = 1;
  }
  return !b.overrun && ics->max_sfb <= ics->num_swb;
}

bool Decoder::parse_section_data(Bits& b, Channel* ch) {
  const IcsInfo& ics = ch->ics;
  const uint32_t esc_bits = ics.is_short() ? 3 : 5, esc = (1u << esc_bits) - 1;
  for (int g = 0; g < ics.num_window_groups; ++g) {
    int k = 0;
    while (k < ics.max_sfb) {
      const uint32_t cb = b.get(4);
      if (cb == 12) return false;
      uint32_t len = 0, incr;
      do { incr = b.get(esc_bits); len += incr; } while (incr == esc && !b.overrun);
      if (b.overrun || k + len > ics.max_sfb) return false;
      for (uint32_t i = 0; i < len; ++i) ch->band_type[g * ics.max_sfb + k + i] = static_cast<uint8_t>(cb);
      k += static_cast<int>(len);
    }
  }
  return !b.overrun;
}

bool Decoder::parse_scalefactors(Bits& b, Channel* ch) {
  const IcsInfo& ics = ch->ics;
  int scale = ch->global_gain, noise = ch->global_gain - 90, intensity = 0;
  bool first_noise = true;
  for (int g = 0; g < ics.num_window_groups; ++g)
    for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
      const int idx = g * ics.max_sfb + sfb;
      const int cb = ch->band_type[idx];
      if (cb == ZeroBand) {
        ch->sf[idx] = 0;
      } else if (cb == IntensityBand || cb == IntensityBand2) {
        const int d = huff(b, 11);
        if (d < 0) return false;
        intensity += d - 60;
        ch->sf[idx] = intensity < -155 ? -155 : intensity > 100 ? 100 : intensity;
      } else if (cb == NoiseBand) {
        if (first_noise) { noise += static_cast<int>(b.get(9)) - 256; first_noise = false; }
        else {
          const int d = huff(b, 11);
          if (d < 0) return false;
          noise += d - 60;
        }
        ch->sf[idx] = noise < -100 ? -100 : noise > 155 ? 155 : noise;
      } else {
        const int d = huff(b, 11);
        if (d < 0) return false;
        scale += d - 60;
        if (scale < 0 || scale > 255) return false;
        ch->sf[idx] = scale;
      }
    }
  return !b.overrun;
}

bool Decoder::parse_pulse(Bits& b, Channel* ch) {
  Pulse& p = ch->pulse;
  p.count = static_cast<uint8_t>(b.get(2) + 1);
  p.start_sfb = static_cast<uint8_t>(b.get(6));
  if (ch->ics.is_short() || p.start_sfb >= ch->ics.num_swb) return false;
  for (int i = 0; i < p.count; ++i) {
    p.offset[i] = static_cast<uint8_t>(b.get(5));
    p.amp[i] = static_cast<uint8_t>(b.get(4));
  }
  return !b.overrun;
}

bool Decoder::parse_tns(Bits& b, Channel* ch) {
  Tns& t = ch->tns;
  const bool is8 = ch->ics.is_short();
  const int max_order = is8 ? 7 : 12;
  for (int w = 0; w < ch->ics.num_windows; ++w) {
    t.n_filt[w] = static_cast<uint8_t>(b.get(2 - (is8 ? 1 : 0)));
    if (t.n_filt[w] > 3) return false;
    if (!t.n_filt[w]) continue;
    const uint32_t coef_res = b.get1();
    for (int f = 0; f < t.n_filt[w]; ++f) {
      t.length[w][f] = static_cast<uint8_t>(b.get(6 - 2 * (is8 ? 1 : 0)));
      t.order[w][f] = static_cast<uint8_t>(b.get(5 - 2 * (is8 ? 1 : 0)));
      if (t.order[w][f] > max_order) return false;
      if (!t.order[w][f]) continue;
      t.direction[w][f] = b.get1();
      const uint32_t compress = b.get1();
      const uint32_t len = coef_res + 3 - compress, res_bits = coef_res + 3;
      float r[12];
      const double pi = 3.14159265358979323846;
      for (int i = 0; i < t.order[w][f]; ++i) {
        int q = static_cast<int>(b.get(len));
        if (q >= (1 << (len - 1))) q -= 1 << len;  // two's complement
        const double iqfac = ((1 << (res_bits - 1)) - 0.5) / (pi / 2), iqfac_m = ((1 << (res_bits - 1)) + 0.5) / (pi / 2);
        r[i] = static_cast<float>(fmath::sin(q / (q >= 0 ? iqfac : iqfac_m)));
      }
      // Reflection coefficients to direct-form prediction coefficients.
      float lpc[12] = {};
      for (int i = 0; i < t.order[w][f]; ++i) {
        const float rr = r[i];
        lpc[i] = rr;
        for (int j = 0; j < (i + 1) / 2; ++j) {
          const float a = lpc[j], c = lpc[i - 1 - j];
          lpc[j] = a + rr * c;
          lpc[i - 1 - j] = c + rr * a;
        }
      }
      for (int i = 0; i < t.order[w][f]; ++i) t.lpc[w][f][i] = lpc[i];
    }
  }
  return !b.overrun;
}

bool Decoder::parse_spectrum(Bits& b, Channel* ch) {
  const IcsInfo& ics = ch->ics;
  for (int16_t& v : ch->q) v = 0;
  int window = 0;
  for (int g = 0; g < ics.num_window_groups; ++g) {
    for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
      const int cb = ch->band_type[g * ics.max_sfb + sfb];
      if (cb == ZeroBand || cb >= NoiseBand) continue;
      const int start = ics.swb_offset[sfb], end = ics.swb_offset[sfb + 1];
      for (int w = 0; w < ics.group_len[g]; ++w) {
        int16_t* q = ch->q + (window + w) * 128 * (ics.is_short() ? 1 : 0) + start;
        if (cb <= 4) {
          for (int k = start; k < end; k += 4, q += 4) {
            const int idx = huff(b, cb - 1);
            if (idx < 0) return false;
            int v[4] = {idx / 27, idx / 9 % 3, idx / 3 % 3, idx % 3};
            if (cb <= 2) {
              for (int i = 0; i < 4; ++i) q[i] = static_cast<int16_t>(v[i] - 1);
            } else {
              for (int i = 0; i < 4; ++i) {
                q[i] = static_cast<int16_t>(v[i]);
                if (v[i] && b.get1()) q[i] = static_cast<int16_t>(-v[i]);
              }
            }
          }
        } else {
          const int modulus = cb <= 6 ? 9 : cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
          for (int k = start; k < end; k += 2, q += 2) {
            const int idx = huff(b, cb - 1);
            if (idx < 0) return false;
            int v0 = idx / modulus, v1 = idx % modulus;
            if (cb <= 6) {
              q[0] = static_cast<int16_t>(v0 - 4);
              q[1] = static_cast<int16_t>(v1 - 4);
            } else {
              const bool n0 = v0 && b.get1(), n1 = v1 && b.get1();
              if (cb == 11) {
                auto escape = [&](int v) {
                  if (v != 16) return v;
                  int ones = 0;
                  while (b.get1() && ones < 12 && !b.overrun) ++ones;
                  const uint32_t n = static_cast<uint32_t>(ones) + 4;
                  return static_cast<int>((1u << n) + b.get(n));
                };
                v0 = escape(v0);
                v1 = escape(v1);
              }
              if (v0 > 32767 || v1 > 32767) return false;
              q[0] = static_cast<int16_t>(n0 ? -v0 : v0);
              q[1] = static_cast<int16_t>(n1 ? -v1 : v1);
            }
          }
        }
        if (b.overrun) return false;
      }
    }
    window += ics.group_len[g];
  }
  return !b.overrun;
}

// ---- reconstruction ----
void Decoder::dequantize(Channel* ch) {
  const IcsInfo& ics = ch->ics;
  for (float& c : ch->coef) c = 0;
  if (ch->pulse.present) {
    int pos = ics.swb_offset[ch->pulse.start_sfb];
    for (int i = 0; i < ch->pulse.count; ++i) {
      pos += ch->pulse.offset[i];
      if (pos >= 1024) break;
      ch->q[pos] = static_cast<int16_t>(ch->q[pos] + (ch->q[pos] > 0 ? ch->pulse.amp[i] : -ch->pulse.amp[i]));
    }
  }
  int window = 0;
  for (int g = 0; g < ics.num_window_groups; ++g) {
    for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
      const int idx = g * ics.max_sfb + sfb;
      const int cb = ch->band_type[idx];
      if (cb == ZeroBand || cb == IntensityBand || cb == IntensityBand2) continue;
      const int start = ics.swb_offset[sfb], end = ics.swb_offset[sfb + 1];
      for (int w = 0; w < ics.group_len[g]; ++w) {
        const int base = ics.is_short() ? (window + w) * 128 : 0;
        float* c = ch->coef + base;
        if (cb == NoiseBand) {
          float energy = 0;
          for (int k = start; k < end; ++k) {
            random_ = random_ * 1664525u + 1013904223u;
            c[k] = static_cast<float>(static_cast<int32_t>(random_));
            energy += c[k] * c[k];
          }
          const float scale = energy > 0 ? static_cast<float>(fmath::pow(2.0, ch->sf[idx] * 0.25) / fmath::sqrt(energy)) : 0.0f;
          for (int k = start; k < end; ++k) c[k] *= scale;
        } else {
          const float gain = static_cast<float>(fmath::pow(2.0, (ch->sf[idx] - 100) * 0.25));
          const int16_t* q = ch->q + base;
          for (int k = start; k < end; ++k) {
            const int v = q[k] < 0 ? -q[k] : q[k];
            const float m = v < 8192 ? pow43_[v] : static_cast<float>(fmath::pow(v, 4.0 / 3.0));
            c[k] = (q[k] < 0 ? -m : m) * gain;
          }
        }
      }
    }
    window += ics.group_len[g];
  }
}

void Decoder::apply_tns(Channel* ch) {
  const IcsInfo& ics = ch->ics;
  const int mmm = ics.tns_max_bands < ics.max_sfb ? ics.tns_max_bands : ics.max_sfb;
  if (!mmm) return;
  for (int w = 0; w < ics.num_windows; ++w) {
    int bottom = ics.num_swb;
    for (int f = 0; f < ch->tns.n_filt[w]; ++f) {
      const int top = bottom;
      bottom = top - ch->tns.length[w][f] > 0 ? top - ch->tns.length[w][f] : 0;
      const int order = ch->tns.order[w][f];
      if (order == 0) continue;
      int start = ics.swb_offset[bottom < mmm ? bottom : mmm];
      const int end = ics.swb_offset[top < mmm ? top : mmm];
      const int size = end - start;
      if (size <= 0) continue;
      int inc = 1;
      if (ch->tns.direction[w][f]) {
        inc = -1;
        start = end - 1;
      }
      start += w * 128;
      float* coef = ch->coef;
      for (int m = 0; m < size; ++m, start += inc)
        for (int i = 1; i <= (m < order ? m : order); ++i) coef[start] -= coef[start - i * inc] * ch->tns.lpc[w][f][i - 1];
    }
  }
}

constexpr float kLongScale = 1.0f / 1024.0f, kShortScale = 1.0f / 128.0f;

void Decoder::synthesize(Channel* ch, ChannelState* st, float* out) {
  const IcsInfo& ics = ch->ics;
  const int prev_shape = st->prev_shape, shape = ics.window_shape;
  float z[2048];
  if (ics.window_sequence == EightShort) {
    for (float& f : z) f = 0;
    float y[256];
    for (int w = 0; w < 8; ++w) {
      mdct_short_.inverse(ch->coef + w * 128, y);
      const float* rise = short_slope_[w == 0 ? prev_shape : shape];
      const float* fall = short_slope_[shape];
      for (int i = 0; i < 128; ++i) z[448 + w * 128 + i] += y[i] * kShortScale * rise[i];
      for (int i = 0; i < 128; ++i) z[448 + w * 128 + 128 + i] += y[128 + i] * kShortScale * fall[127 - i];
    }
  } else {
    float y[2048];
    mdct_long_.inverse(ch->coef, y);
    const float* rise = long_slope_[prev_shape];
    const float* fall = long_slope_[shape];
    const float* srise = short_slope_[prev_shape];
    const float* sfall = short_slope_[shape];
    for (int i = 0; i < 1024; ++i) {
      float w;
      if (ics.window_sequence == LongStop) w = i < 448 ? 0.0f : i < 576 ? srise[i - 448] : 1.0f;
      else w = rise[i];
      z[i] = y[i] * kLongScale * w;
    }
    for (int i = 0; i < 1024; ++i) {
      float w;
      if (ics.window_sequence == LongStart) w = i < 448 ? 1.0f : i < 576 ? sfall[127 - (i - 448)] : 0.0f;
      else w = fall[1023 - i];
      z[1024 + i] = y[1024 + i] * kLongScale * w;
    }
  }
  for (int i = 0; i < 1024; ++i) {
    out[i] = st->overlap[i] + z[i];
    st->overlap[i] = z[1024 + i];
  }
  st->prev_shape = static_cast<uint8_t>(shape);
}

bool Decoder::parse_channel(Bits& b, Channel* ch, const IcsInfo* common) {
  ch->global_gain = static_cast<uint8_t>(b.get(8));
  if (common) ch->ics = *common;
  else if (!parse_ics_info(b, &ch->ics)) return false;
  for (uint8_t& t : ch->band_type) t = 0;
  ch->pulse = Pulse();
  ch->tns = Tns();
  if (!parse_section_data(b, ch) || !parse_scalefactors(b, ch)) return false;
  if (b.get1()) {
    ch->pulse.present = true;
    if (!parse_pulse(b, ch)) return false;
  }
  if (b.get1()) {
    ch->tns.present = true;
    if (!parse_tns(b, ch)) return false;
  }
  if (b.get1()) return false;  // gain control (SSR profile)
  if (!parse_spectrum(b, ch)) return false;
  dequantize(ch);
  return true;
}

bool Decoder::decode_block(ConstByteView data, int16_t* pcm) {
  Bits b{data};
  float out[2][1024];
  int chosen_channels = 0;  // 0: nothing chosen yet, 1: a single channel, 2: a pair
  int channel_index = 0;
  for (;;) {
    const uint32_t id = b.get(3);
    if (b.overrun) return false;
    if (id == 7) break;  // END
    if (id == 0 || id == 3) {  // SCE, LFE
      b.get(4);
      if (channel_index >= 16) return false;
      Channel& c = ch_[0];
      if (!parse_channel(b, &c, nullptr)) return false;
      if (c.tns.present) apply_tns(&c);
      float tmp[1024];
      synthesize(&c, &states_[channel_index], chosen_channels == 0 ? out[0] : tmp);
      if (chosen_channels == 0) chosen_channels = 1;
      ++channel_index;
    } else if (id == 1) {  // CPE
      b.get(4);
      if (channel_index + 1 >= 16) return false;
      const bool common = b.get1();
      int ms_present = 0;
      IcsInfo shared;
      if (common) {
        if (!parse_ics_info(b, &shared)) return false;
        ms_present = static_cast<int>(b.get(2));
        if (ms_present == 3) return false;
        const int bands = shared.num_window_groups * shared.max_sfb;
        for (int i = 0; i < bands; ++i) ms_used_[i] = ms_present == 2;
        if (ms_present == 1)
          for (int i = 0; i < bands; ++i) ms_used_[i] = b.get1();
      }
      if (!parse_channel(b, &ch_[0], common ? &shared : nullptr) || !parse_channel(b, &ch_[1], common ? &shared : nullptr)) return false;
      if (common) {
        const IcsInfo& ics = ch_[0].ics;
        int window = 0;
        for (int g = 0; g < ics.num_window_groups; ++g) {
          for (int sfb = 0; sfb < ics.max_sfb; ++sfb) {
            const int idx = g * ics.max_sfb + sfb;
            const int start = ics.swb_offset[sfb], end = ics.swb_offset[sfb + 1];
            const int b0 = ch_[0].band_type[idx], b1 = ch_[1].band_type[idx];
            for (int w = 0; w < ics.group_len[g]; ++w) {
              float* l = ch_[0].coef + (ics.is_short() ? (window + w) * 128 : 0);
              float* r = ch_[1].coef + (ics.is_short() ? (window + w) * 128 : 0);
              if (ms_used_[idx] && b0 < NoiseBand && b1 < NoiseBand) {
                for (int k = start; k < end; ++k) {
                  const float m = l[k], s = r[k];
                  l[k] = m + s;
                  r[k] = m - s;
                }
              }
              if (b1 == IntensityBand || b1 == IntensityBand2) {  // the right channel is the left one, scaled
                float scale = (b1 == IntensityBand ? 1.0f : -1.0f) * static_cast<float>(fmath::pow(0.5, ch_[1].sf[idx] * 0.25));
                if (ms_present && ms_used_[idx]) scale = -scale;
                for (int k = start; k < end; ++k) r[k] = l[k] * scale;
              }
            }
          }
          window += ics.group_len[g];
        }
      }
      for (int c = 0; c < 2; ++c) {
        if (ch_[c].tns.present) apply_tns(&ch_[c]);
      }
      float tmp[1024];
      if (chosen_channels < 2) {
        synthesize(&ch_[0], &states_[channel_index], out[0]);
        synthesize(&ch_[1], &states_[channel_index + 1], out[1]);
        chosen_channels = 2;
      } else {
        synthesize(&ch_[0], &states_[channel_index], tmp);
        synthesize(&ch_[1], &states_[channel_index + 1], tmp);
      }
      channel_index += 2;
    } else if (id == 4) {  // DSE
      b.get(4);
      const bool align = b.get1();
      uint32_t count = b.get(8);
      if (count == 255) count += b.get(8);
      if (align) b.align();
      b.skip(uint64_t{count} * 8);
    } else if (id == 6) {  // FIL
      uint32_t count = b.get(4);
      if (count == 15) count += b.get(8) - 1;
      b.skip(uint64_t{count} * 8);
    } else if (id == 5) {  // PCE
      b.get(4);
      b.get(2);
      b.get(4);
      const uint32_t front = b.get(4), side = b.get(4), back = b.get(4), lfe = b.get(2), assoc = b.get(3), cc = b.get(4);
      if (b.get1()) b.get(4);
      if (b.get1()) b.get(4);
      if (b.get1()) b.get(3);
      b.skip(uint64_t{(front + side + back + cc)} * 5 + uint64_t{lfe} * 4 + uint64_t{assoc} * 4);
      b.align();
      const uint32_t comment = b.get(8);
      b.skip(uint64_t{comment} * 8);
    } else {
      return false;  // CCE (coupling) and anything else
    }
    if (b.overrun) return false;
  }
  if (chosen_channels == 0) return false;
  out_channels_ = static_cast<uint32_t>(chosen_channels);
  for (int i = 0; i < 1024; ++i)
    for (int c = 0; c < chosen_channels; ++c) {
      float v = out[c][i];
      v = v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v;
      pcm[i * chosen_channels + c] = static_cast<int16_t>(v >= 0 ? v + 0.5f : v - 0.5f);
    }
  return true;
}

}  // namespace aac

// ---- public wrapper ----

AacDecoder::~AacDecoder() {}

bool AacDecoder::init(uint32_t sampling_index) {
  impl_ = UniquePointer<aac::Decoder>::make(allocator_, allocator_);
  return impl_ && impl_->init(sampling_index);
}
void AacDecoder::reset() {
  if (impl_) impl_->reset_state();
}
uint32_t AacDecoder::sample_rate() const { return impl_ ? impl_->sample_rate() : 0; }
uint32_t AacDecoder::channels() const { return impl_ ? impl_->channels() : 0; }
bool AacDecoder::decode_block(ConstByteView block, int16_t* pcm) { return impl_ && impl_->decode_block(block, pcm); }

bool parse_audio_specific_config(ConstByteView config, uint32_t* object_type, uint32_t* sampling_index, uint32_t* channel_config) {
  aac::Bits b{config};
  uint32_t aot = b.get(5);
  if (aot == 31) aot = 32 + b.get(6);
  uint32_t index = b.get(4);
  if (index == 15) {  // explicit frequency: match it against the table
    const uint32_t hz = b.get(24);
    index = 13;
    for (uint32_t i = 0; i < 13; ++i)
      if (aac::kSampleRates[i] == hz) index = i;
  }
  const uint32_t channels = b.get(4);
  if (b.overrun || index >= 13) return false;
  *object_type = aot;
  *sampling_index = index;
  *channel_config = channels;
  return aot == 2;  // AAC-LC
}

namespace {
// ADTS: 12 sync bits, layer 0, a valid sampling index, and a frame length that fits.
bool adts_header(ConstByteView d, uint64_t at, uint32_t* header_size, uint32_t* frame_length, uint32_t* sampling_index, uint32_t* profile) {
  if (at + 7 > d.size() || d[at] != 0xFF || (d[at + 1] & 0xF6) != 0xF0) return false;
  const uint32_t protection_absent = d[at + 1] & 1;
  *profile = d[at + 2] >> 6;
  *sampling_index = (d[at + 2] >> 2) & 15;
  *frame_length = ((d[at + 3] & 3u) << 11) | (uint32_t{d[at + 4]} << 3) | (d[at + 5] >> 5);
  *header_size = protection_absent ? 7 : 9;
  return *sampling_index < 13 && *frame_length >= *header_size && ((d[at + 6] & 3) == 0);
}
}  // namespace

bool looks_like_adts(ConstByteView d) {
  uint32_t header, length, rate, profile;
  if (!adts_header(d, 0, &header, &length, &rate, &profile)) return false;
  return length == d.size() || (length + 2 <= d.size() && adts_header(d, length, &header, &length, &rate, &profile));
}

bool decode_aac(ConstByteView data, AudioData* out) {
  out->clear();
  if (!looks_like_adts(data)) return false;
  allocators::IAllocator& allocator = out->allocator();
  AacDecoder decoder(allocator);
  ByteBuffer pcm(allocator);
  uint32_t rate = 0, channels = 0;
  int16_t block[2048];
  bool ok = true;
  for (uint64_t at = 0; ok && at + 7 <= data.size();) {
    uint32_t header, length, index, profile;
    if (!adts_header(data, at, &header, &length, &index, &profile) || at + length > data.size()) {
      ++at;  // lost sync: look for the next frame
      continue;
    }
    if (profile != 1) {  // not AAC-LC
      at += length;
      continue;
    }
    if (rate == 0) {
      if (!decoder.init(index)) return false;
      rate = decoder.sample_rate();
    } else if (aac::kSampleRates[index] != rate) {
      at += length;
      continue;
    }
    // (Several raw data blocks in one frame carry a table of their positions: only the single-block form is handled.)
    if ((data[at + 6] & 3) == 0 && decoder.decode_block(data.subview(at + header, length - header), block)) {
      if (channels == 0) channels = decoder.channels();
      if (decoder.channels() == channels) ok = pcm.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels * 2));
    }
    at += length;
  }
  if (!ok || rate == 0 || channels == 0 || pcm.size() == 0) return false;
  audio::StreamFormat format;
  format.sample_rate = rate;
  format.channels = static_cast<uint8_t>(channels);
  const uint64_t frames = pcm.size() / (channels * 2);
  if (!out->create(format, frames)) return false;
  __builtin_memcpy(out->samples().data(), pcm.view().data(), frames * channels * 2);
  return true;
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// Streaming: ADTS frames are found through a window of the file and decoded one at a time.

namespace {

class AacStream final : public StreamingPcmSource {
 public:
  static constexpr uint32_t kIndexEvery = 32, kPrimeFrames = 2;

  AacStream(IByteSource& source, allocators::IAllocator& allocator) : source_(source), allocator_(allocator), window_(source, allocator), decoder_(allocator), pcm_(allocator), index_(allocator) {}

  bool init() {
    uint64_t counted = 0, at = 0, frame_at = 0;
    uint32_t header = 0, length = 0, index = 0;
    while (next_frame(at, &frame_at, &header, &length, &index)) {
      at = frame_at + length;
      if (rate_ == 0) {
        if (!decoder_.init(index)) return false;
        rate_ = decoder_.sample_rate();
        sampling_index_ = index;
      } else if (kSampleRates()[index] != rate_) {
        continue;
      }
      if (counted % kIndexEvery == 0 && !index_.push_back(frame_at)) return false;
      ++counted;
    }
    if (rate_ == 0 || counted == 0) return false;
    // The channel count is what the first block decodes to.
    cur_at_ = index_[0];
    cur_frame_ = 0;
    int16_t block[2048];
    if (!decode_next(block)) return false;
    channels_ = decoder_.channels();
    counted_ = counted;
    frames_ = counted * 1024;
    format_.sample_rate = rate_;
    format_.channels = static_cast<uint8_t>(channels_);
    format_.type = audio::SampleType::S16;
    restart(0);
    return channels_ >= 1 && channels_ <= 2 && format_.valid();
  }

 protected:
  bool refill(uint64_t frame) override {
    const uint64_t k = frame / 1024;
    if (k < cur_frame_ || k - cur_frame_ > 256) restart(k > kPrimeFrames ? k - kPrimeFrames : 0);
    int16_t block[2048];
    while (cur_frame_ < k)
      if (!decode_next(block)) return false;
    pcm_.clear();
    uint32_t got = 0;
    for (; got < 4 && cur_frame_ < counted_; ++got) {
      if (!decode_next(block)) break;
      if (!pcm_.append(ConstByteView(reinterpret_cast<const uint8_t*>(block), uint64_t{1024} * channels_ * 2))) return false;
    }
    if (got == 0) return false;
    set_window(k * 1024, pcm_.view());
    return true;
  }

 private:
  static const uint32_t* kSampleRates() { return aac::kSampleRates; }

  // Goes back to the remembered place at or before frame `target` with the decoder's memory cleared.
  void restart(uint64_t target) {
    const uint64_t slot = target / kIndexEvery;
    decoder_.reset();
    cur_frame_ = slot * kIndexEvery;
    cur_at_ = index_[slot];
  }

  // The next ADTS frame at or after `pos`: one the decoder can use (AAC-LC, one raw data block).
  bool next_frame(uint64_t pos, uint64_t* at, uint32_t* header, uint32_t* length, uint32_t* index) {
    for (;;) {
      const ConstByteView v = window_.from(pos);
      if (v.size() < 7) return false;
      for (uint64_t i = 0; i + 7 <= v.size(); ++i) {
        uint32_t h, len, idx, profile;
        if (!adts_header(v, i, &h, &len, &idx, &profile) || i + len > v.size()) continue;
        if (profile != 1 || (v[i + 6] & 3) != 0) {
          i += len - 1;   // (not AAC-LC, or several blocks in one frame: not played)
          continue;
        }
        *at = pos + i;
        *header = h;
        *length = len;
        *index = idx;
        return true;
      }
      if (v.size() < 131072) return false;
      pos += v.size() - 8192;
    }
  }

  // Decodes the frame at cur_at_ (of the stream's sampling rate); a frame that cannot be decoded is silence.
  bool decode_next(int16_t* block) {
    for (;;) {
      uint64_t at = 0;
      uint32_t header = 0, length = 0, index = 0;
      if (!next_frame(cur_at_, &at, &header, &length, &index)) return false;
      cur_at_ = at + length;
      if (aac::kSampleRates[index] != rate_) continue;
      const ConstByteView v = window_.from(at);
      if (v.size() < length) return false;
      if (!decoder_.decode_block(v.subview(header, length - header), block))
        for (int i = 0; i < 2048; ++i) block[i] = 0;
      ++cur_frame_;
      return true;
    }
  }

  IByteSource& source_;
  allocators::IAllocator& allocator_;
  SourceWindow window_;
  AacDecoder decoder_;
  ByteBuffer pcm_;
  List<uint64_t> index_;
  uint32_t rate_ = 0, channels_ = 0, sampling_index_ = 0;
  uint64_t counted_ = 0, cur_frame_ = 0, cur_at_ = 0;
};

}  // namespace

UniquePointer<audio::IPcmSource> open_aac_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<AacStream> stream = UniquePointer<AacStream>::make(allocator, source, allocator);
  if (!stream || !stream->init()) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

}  // namespace media
