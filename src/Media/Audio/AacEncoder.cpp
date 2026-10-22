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

// AAC-LC encoder: long blocks with the sine window, a forward MDCT, scalefactors from a band-energy noise model, Huffman coding with
// the standard's codebooks (the decoder's tables, see AacTables.hpp), mid/side stereo per band and a bit reservoir so that the stream
// averages the requested bitrate. Output is raw_data_blocks (wrapped in ADTS by encode_aac()).
//
// Not done: block switching (short windows), temporal noise shaping, intensity stereo, a psychoacoustic model beyond the energy
// shaping below. Transients therefore spread their quantisation noise over the whole 2048-sample window.
#include "Math/FloatMath.hpp"
#include "Media/Audio/AacInternal.hpp"
#include "Media/Audio/AacTables.hpp"
#include "Media/Audio/Mdct.hpp"
#include "Media/AudioCodecs.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/Move.hpp"

namespace media {
namespace aac {

namespace {

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline int clampi(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }

constexpr int kFrame = 1024;
constexpr int kMaxBands = 51;
constexpr int kMaxFrameBitsPerChannel = 6144;  // the standard's limit on one channel's share of a block
constexpr double kBeta = 0.5;                  // how much the step follows the band's level: 0 = one step everywhere, 1 = constant SNR

// Scalefactor-band widths for the long window at each sampling index (the same grouping as the decoder uses).
const uint8_t* long_widths(uint32_t index, int* count) {
  static const uint8_t* const kWidths[13] = {kWidthsL96, kWidthsL96, kWidthsL64, kWidthsL48, kWidthsL48, kWidthsL32, kWidthsL24,
                                             kWidthsL24, kWidthsL16, kWidthsL16, kWidthsL16, kWidthsL8,  kWidthsL8};
  static const uint8_t kCounts[13] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40, 40};
  *count = kCounts[index];
  return kWidths[index];
}

// Writes bits MSB first; with no buffer it only counts (the rate loop prices a coding before committing to it).
class BitSink {
 public:
  explicit BitSink(ByteBuffer* out) : out_(out) {}
  void put(uint32_t value, int n) {
    count_ += static_cast<uint64_t>(n);
    if (!out_) return;
    for (int i = n - 1; i >= 0; --i) {
      acc_ = static_cast<uint8_t>((acc_ << 1) | ((value >> i) & 1));
      if (++fill_ == 8) {
        ok_ = out_->put8(acc_) && ok_;
        acc_ = 0;
        fill_ = 0;
      }
    }
  }
  void align() {
    if (fill_ != 0) put(0, 8 - fill_);
  }
  uint64_t count() const { return count_; }
  bool ok() const { return ok_; }

 private:
  ByteBuffer* out_;
  uint64_t count_ = 0;
  uint8_t acc_ = 0;
  int fill_ = 0;
  bool ok_ = true;
};

void put_code(BitSink& b, int book, uint32_t symbol) { b.put(kBooks[book].codes[symbol], kBooks[book].lens[symbol]); }

// The largest magnitude codebook `cb` (1 .. 11) can carry; cb 11 takes anything through its escapes.
constexpr int kMaxMagnitude[12] = {0, 1, 1, 2, 2, 4, 4, 7, 7, 12, 12, 8191};

// Codes (or, on a counting sink, prices) the quantised values q[start, end) with codebook cb in 1 .. 11. The inverse of the decoder's
// parse_spectrum().
void put_band(BitSink& b, const int16_t* q, int start, int end, int cb) {
  if (cb <= 4) {
    for (int k = start; k < end; k += 4) {
      int idx = 0;
      for (int i = 0; i < 4; ++i) idx = idx * 3 + (cb <= 2 ? q[k + i] + 1 : iabs(q[k + i]));
      put_code(b, cb - 1, static_cast<uint32_t>(idx));
      if (cb >= 3)
        for (int i = 0; i < 4; ++i)
          if (q[k + i] != 0) b.put(q[k + i] < 0, 1);
    }
    return;
  }
  const int modulus = cb <= 6 ? 9 : cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
  for (int k = start; k < end; k += 2) {
    const int a = q[k], c = q[k + 1];
    if (cb <= 6) {
      put_code(b, cb - 1, static_cast<uint32_t>((a + 4) * 9 + (c + 4)));
      continue;
    }
    const int ma = imin(iabs(a), modulus - 1), mc = imin(iabs(c), modulus - 1);
    put_code(b, cb - 1, static_cast<uint32_t>(ma * modulus + mc));
    if (ma) b.put(a < 0, 1);
    if (mc) b.put(c < 0, 1);
    if (cb == 11) {
      const int values[2] = {iabs(a), iabs(c)};
      for (const int v : values) {
        if (v < 16) continue;
        int n = 4;
        while ((v >> (n + 1)) != 0) ++n;
        for (int i = 4; i < n; ++i) b.put(1, 1);
        b.put(0, 1);
        b.put(static_cast<uint32_t>(v - (1 << n)), n);
      }
    }
  }
}

int largest(const int16_t* q, int start, int end) {
  int m = 0;
  for (int k = start; k < end; ++k) m = imax(m, iabs(q[k]));
  return m;
}

// One channel's quantised spectrum with everything needed to write it.
struct Coded {
  int16_t q[kFrame];
  int sf[kMaxBands];
  uint8_t cb[kMaxBands];
  int max_sfb = 0;
  int global_gain = 0;
  uint64_t bits = 0;  // the individual_channel_stream without ics_info
};

struct Section {
  int cb, length;
};

int build_sections(const Coded& c, Section* out) {
  int n = 0;
  for (int b = 0; b < c.max_sfb; ++b) {
    if (n > 0 && out[n - 1].cb == c.cb[b]) ++out[n - 1].length;
    else out[n++] = {c.cb[b], 1};
  }
  return n;
}

// ics_info for a long window with the sine window shape and no prediction.
void put_ics_info(BitSink& b, int max_sfb) {
  b.put(0, 1);  // ics_reserved_bit
  b.put(0, 2);  // window_sequence: ONLY_LONG_SEQUENCE
  b.put(0, 1);  // window_shape: sine
  b.put(static_cast<uint32_t>(max_sfb), 6);
  b.put(0, 1);  // predictor_data_present
}

// 4.4.2 individual_channel_stream: gain, ics_info (unless the pair shares one), sections, scalefactors, no pulse / TNS / gain control,
// spectrum. Returns its size in bits.
uint64_t put_channel(BitSink& b, const Coded& c, const uint16_t* offsets, bool with_ics_info) {
  const uint64_t begin = b.count();
  b.put(static_cast<uint32_t>(c.global_gain), 8);
  if (with_ics_info) put_ics_info(b, c.max_sfb);
  Section sections[kMaxBands];
  const int ns = build_sections(c, sections);
  for (int s = 0; s < ns; ++s) {
    b.put(static_cast<uint32_t>(sections[s].cb), 4);
    int len = sections[s].length;
    for (; len >= 31; len -= 31) b.put(31, 5);
    b.put(static_cast<uint32_t>(len), 5);
  }
  int prev = c.global_gain;
  for (int band = 0; band < c.max_sfb; ++band) {
    if (c.cb[band] == 0) continue;
    put_code(b, 11, static_cast<uint32_t>(c.sf[band] - prev + 60));
    prev = c.sf[band];
  }
  b.put(0, 3);  // pulse_data_present, tns_data_present, gain_control_data_present
  for (int band = 0; band < c.max_sfb; ++band)
    if (c.cb[band] != 0) put_band(b, c.q, offsets[band], offsets[band + 1], c.cb[band]);
  return b.count() - begin;
}

// x^(3/4)
inline double pow075(double x) {
  const double s = fmath::sqrt(x);
  return s * fmath::sqrt(s);
}

inline double log2d(double x) { return fmath::ln(x) * 1.4426950408889634; }

// Quantises one band with scalefactor `sf`: q = nint(|x / step|^(3/4) - 0.0946), step = 2^((sf - 100) / 4).
bool quantize_band(const float* x, int start, int end, int sf, int16_t* q) {
  const double inv = fmath::pow(2.0, -(sf - 100) * 0.1875);  // step^(-3/4)
  bool any = false;
  for (int k = start; k < end; ++k) {
    const double a = x[k] < 0 ? -static_cast<double>(x[k]) : static_cast<double>(x[k]);
    int v = static_cast<int>(pow075(a) * inv + 0.4054);
    if (v > 8191) v = 8191;
    q[k] = static_cast<int16_t>(x[k] < 0 ? -v : v);
    any = any || v != 0;
  }
  return any;
}

}  // namespace

class Encoder {
 public:
  explicit Encoder(allocators::IAllocator& allocator) : allocator_(allocator), mdct_(allocator) {}

  bool init(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps);
  bool encode_block(ArrayView<const int16_t> pcm, ByteBuffer* out);
  uint32_t sampling_index() const { return sr_index_; }
  uint32_t channels() const { return channels_; }

 private:
  void analyse(int ch, ArrayView<const int16_t> pcm);
  void quantize_channel(const float* x, const int* lg, int gain, Coded* c) const;
  void finish_channel(Coded* c) const;
  uint64_t price(const float* const* spectra, const int (*lg)[kMaxBands], int gain, Coded* coded) const;

  allocators::IAllocator& allocator_;
  Mdct mdct_;
  uint32_t sr_index_ = 4, channels_ = 1;
  int nswb_ = 0;
  uint16_t offsets_[kMaxBands + 1] = {};
  uint64_t target_bits_ = 0;
  int64_t reservoir_ = 0;
  float window_[2 * kFrame];
  float history_[2][kFrame] = {};
  float spectrum_[2][kFrame];
  float coded_spectrum_[2][kFrame];  // L/R or M/S per band
  bool ms_[kMaxBands] = {};
};

bool Encoder::init(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps) {
  if (channels < 1 || channels > 2) return false;
  sr_index_ = 13;
  for (uint32_t i = 0; i < 13; ++i)
    if (kSampleRates[i] == sample_rate) sr_index_ = i;
  if (sr_index_ == 13) return false;
  channels_ = channels;
  const uint8_t* widths = long_widths(sr_index_, &nswb_);
  uint16_t at = 0;
  for (int i = 0; i < nswb_; ++i) {
    offsets_[i] = at;
    at = static_cast<uint16_t>(at + widths[i]);
  }
  offsets_[nswb_] = at;
  if (at != kFrame) return false;
  if (bitrate_bps == 0) bitrate_bps = imin(48000 * static_cast<int>(channels) * imax(1, static_cast<int>(sample_rate / 44100)), 320000);
  const uint64_t per_frame = uint64_t{bitrate_bps} * kFrame / sample_rate;
  const uint64_t cap = uint64_t{kMaxFrameBitsPerChannel} * channels - 128;
  target_bits_ = per_frame > cap ? cap : per_frame < 200 ? 200 : per_frame;
  reservoir_ = 0;
  const double pi = 3.14159265358979323846;
  for (int i = 0; i < 2 * kFrame; ++i) window_[i] = static_cast<float>(fmath::sin(pi * (i + 0.5) / (2.0 * kFrame)));
  for (auto& h : history_)
    for (float& f : h) f = 0;
  return mdct_.init(2 * kFrame);
}

// The forward MDCT (X[k] = 2 sum x[n] cos(2pi/N (n + n0)(k + 1/2)), the standard's definition) of the previous and the new block of
// channel `ch`, windowed. It folds the 2048 samples into 1024 and runs a DCT-IV, which the inverse transform of Mdct produces.
void Encoder::analyse(int ch, ArrayView<const int16_t> pcm) {
  float xw[2 * kFrame];
  for (int i = 0; i < kFrame; ++i) {
    xw[i] = history_[ch][i] * window_[i];
    const float cur = static_cast<float>(pcm[static_cast<uint64_t>(i) * channels_ + static_cast<uint64_t>(ch)]);
    xw[kFrame + i] = cur * window_[kFrame + i];
    history_[ch][i] = cur;
  }
  constexpr int N = kFrame, H = kFrame / 2;
  float u[kFrame], y[2 * kFrame];
  for (int j = 0; j < H; ++j) u[j] = -xw[3 * H - 1 - j] - xw[3 * H + j];
  for (int j = H; j < N; ++j) u[j] = xw[j - H] - xw[3 * H - 1 - j];
  mdct_.inverse(u, y);
  for (int k = 0; k < N; ++k) spectrum_[ch][k] = -2.0f * y[3 * H - 1 - k];
}

// Scalefactors from the band levels and the global offset `gain`, quantisation, codebooks and the size of the result.
void Encoder::quantize_channel(const float* x, const int* lg, int gain, Coded* c) const {
  for (int b = 0; b < nswb_; ++b) c->sf[b] = clampi(0, 255, gain + lg[b]);
  bool nonzero[kMaxBands];
  for (int b = 0; b < nswb_; ++b) nonzero[b] = quantize_band(x, offsets_[b], offsets_[b + 1], c->sf[b], c->q);
  // The scalefactor deltas between coded bands are limited to +-60: a band that would break this gets the closest allowed step.
  for (int pass = 0; pass < 3; ++pass) {
    bool changed = false;
    int prev = -1;
    for (int b = 0; b < nswb_; ++b) {
      if (!nonzero[b]) continue;
      if (prev >= 0 && iabs(c->sf[b] - prev) > 60) {
        c->sf[b] = clampi(prev - 60, prev + 60, c->sf[b]);
        nonzero[b] = quantize_band(x, offsets_[b], offsets_[b + 1], c->sf[b], c->q);
        changed = true;
      }
      if (nonzero[b]) prev = c->sf[b];
    }
    if (!changed) break;
  }
  {  // if that did not settle, one scalefactor for all coded bands (the coarsest wanted)
    int prev = -1, top = 0;
    bool bad = false;
    for (int b = 0; b < nswb_; ++b) {
      if (!nonzero[b]) continue;
      if (prev >= 0 && iabs(c->sf[b] - prev) > 60) bad = true;
      prev = c->sf[b];
      top = imax(top, c->sf[b]);
    }
    if (bad)
      for (int b = 0; b < nswb_; ++b)
        if (nonzero[b]) {
          c->sf[b] = top;
          nonzero[b] = quantize_band(x, offsets_[b], offsets_[b + 1], top, c->q);
        }
  }
  c->max_sfb = 0;
  for (int b = 0; b < nswb_; ++b)
    if (nonzero[b]) c->max_sfb = b + 1;
  for (int b = 0; b < c->max_sfb; ++b) {
    c->cb[b] = 0;
    if (!nonzero[b]) continue;
    const int m = largest(c->q, offsets_[b], offsets_[b + 1]);
    uint64_t best = ~uint64_t{0};
    for (int cb = 1; cb <= 11; ++cb) {
      if (kMaxMagnitude[cb] < m) continue;
      BitSink counter(nullptr);
      put_band(counter, c->q, offsets_[b], offsets_[b + 1], cb);
      if (counter.count() < best) {
        best = counter.count();
        c->cb[b] = static_cast<uint8_t>(cb);
      }
    }
  }
  c->global_gain = 0;
  for (int b = 0; b < c->max_sfb; ++b)
    if (c->cb[b] != 0) {
      c->global_gain = c->sf[b];
      break;
    }
  finish_channel(c);
}

void Encoder::finish_channel(Coded* c) const {
  BitSink counter(nullptr);
  c->bits = put_channel(counter, *c, offsets_, channels_ == 1);
}

bool Encoder::encode_block(ArrayView<const int16_t> pcm, ByteBuffer* out) {
  if (pcm.size() != uint64_t{kFrame} * channels_) return false;
  for (uint32_t ch = 0; ch < channels_; ++ch) analyse(static_cast<int>(ch), pcm);

  // Mid/side per band where the pair is correlated enough that the orthonormal M/S energies have the smaller product.
  for (int b = 0; b < kMaxBands; ++b) ms_[b] = false;
  for (uint32_t ch = 0; ch < channels_; ++ch)
    for (int k = 0; k < kFrame; ++k) coded_spectrum_[ch][k] = spectrum_[ch][k];
  if (channels_ == 2) {
    for (int b = 0; b < nswb_; ++b) {
      double el = 0, er = 0, c = 0;
      for (int k = offsets_[b]; k < offsets_[b + 1]; ++k) {
        const double l = spectrum_[0][k], r = spectrum_[1][k];
        el += l * l;
        er += r * r;
        c += l * r;
      }
      const double em = (el + er) * 0.5 + c, es = (el + er) * 0.5 - c;
      if (el > 0 && er > 0 && em * es < 0.8 * el * er) {
        ms_[b] = true;
        for (int k = offsets_[b]; k < offsets_[b + 1]; ++k) {
          coded_spectrum_[0][k] = (spectrum_[0][k] + spectrum_[1][k]) * 0.5f;
          coded_spectrum_[1][k] = (spectrum_[0][k] - spectrum_[1][k]) * 0.5f;
        }
      }
    }
  }

  // Per band: 4 * kBeta * log2(rms), the part of the scalefactor that follows the signal.
  int lg[2][kMaxBands];
  for (uint32_t ch = 0; ch < channels_; ++ch)
    for (int b = 0; b < nswb_; ++b) {
      double e = 0;
      for (int k = offsets_[b]; k < offsets_[b + 1]; ++k) e += static_cast<double>(coded_spectrum_[ch][k]) * coded_spectrum_[ch][k];
      const double rms = fmath::sqrt(e / (offsets_[b + 1] - offsets_[b]));
      lg[ch][b] = rms < 1e-3 ? -40 : static_cast<int>(4.0 * kBeta * log2d(rms) + (rms >= 1 ? 0.5 : -0.5));
    }

  // The bit budget of this block, then the coarsest global step that fits it.
  const int64_t cap = static_cast<int64_t>(kMaxFrameBitsPerChannel) * channels_ - 128;
  int64_t budget = static_cast<int64_t>(target_bits_) + reservoir_ / 4;
  budget = budget < static_cast<int64_t>(target_bits_ / 2) ? static_cast<int64_t>(target_bits_ / 2) : budget > 3 * static_cast<int64_t>(target_bits_) ? 3 * static_cast<int64_t>(target_bits_) : budget;
  if (budget > cap) budget = cap;
  const int64_t overhead = 32 + (channels_ == 2 ? nswb_ : 0);
  Coded coded[2];
  int lo = -100, hi = 300;
  while (lo < hi) {
    const int mid = lo + (hi - lo) / 2;
    int64_t total = overhead;
    for (uint32_t ch = 0; ch < channels_; ++ch) {
      quantize_channel(coded_spectrum_[ch], lg[ch], mid, &coded[ch]);
      total += static_cast<int64_t>(coded[ch].bits);
    }
    if (total <= budget) hi = mid;
    else lo = mid + 1;
  }
  for (uint32_t ch = 0; ch < channels_; ++ch) quantize_channel(coded_spectrum_[ch], lg[ch], lo, &coded[ch]);

  // The shared band limit of a pair; the channel that stops earlier codes zero bands up to it.
  int max_sfb = coded[0].max_sfb;
  if (channels_ == 2) {
    max_sfb = imax(max_sfb, coded[1].max_sfb);
    for (int ch = 0; ch < 2; ++ch) {
      for (int b = coded[ch].max_sfb; b < max_sfb; ++b) coded[ch].cb[b] = 0;
      coded[ch].max_sfb = max_sfb;
      finish_channel(&coded[ch]);
    }
  }

  const uint64_t mark = out->size();
  BitSink w(out);
  w.put(channels_ == 2 ? 1 : 0, 3);  // id_syn_ele: CPE / SCE
  w.put(0, 4);                       // element_instance_tag
  if (channels_ == 2) {
    w.put(1, 1);  // common_window
    put_ics_info(w, max_sfb);
    bool any_ms = false;
    for (int b = 0; b < max_sfb; ++b) any_ms = any_ms || ms_[b];
    w.put(any_ms ? 1 : 0, 2);  // ms_mask_present
    if (any_ms)
      for (int b = 0; b < max_sfb; ++b) w.put(ms_[b] ? 1 : 0, 1);
    put_channel(w, coded[0], offsets_, false);
    put_channel(w, coded[1], offsets_, false);
  } else {
    put_channel(w, coded[0], offsets_, true);
  }
  w.put(7, 3);  // ID_END
  w.align();
  if (!w.ok()) {
    out->truncate(mark);
    return false;
  }
  const int64_t used = static_cast<int64_t>(w.count());
  reservoir_ += static_cast<int64_t>(target_bits_) - used;
  const int64_t t = static_cast<int64_t>(target_bits_);
  reservoir_ = reservoir_ < -4 * t ? -4 * t : reservoir_ > 8 * t ? 8 * t : reservoir_;
  return true;
}

}  // namespace aac

// ---- public wrapper ----

AacEncoder::~AacEncoder() {}

bool AacEncoder::init(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps) {
  impl_ = UniquePointer<aac::Encoder>::make(allocator_, allocator_);
  return impl_ && impl_->init(sample_rate, channels, bitrate_bps);
}

uint32_t AacEncoder::sampling_index() const { return impl_ ? impl_->sampling_index() : 0; }
uint32_t AacEncoder::channels() const { return impl_ ? impl_->channels() : 0; }

bool AacEncoder::encode_block(ArrayView<const int16_t> pcm, ByteBuffer* out) { return impl_ && impl_->encode_block(pcm, out); }

bool AacEncoder::audio_specific_config(ByteBuffer* out) const {
  if (!impl_) return false;
  const uint32_t config = (2u << 11) | (impl_->sampling_index() << 7) | (impl_->channels() << 3);  // AAC LC, 1024-sample frames
  return out->put8(static_cast<uint8_t>(config >> 8)) && out->put8(static_cast<uint8_t>(config));
}

// An ADTS header for a frame of `frame_length` bytes (header included): AAC-LC, no CRC, variable bitrate.
static bool put_adts_header(ByteBuffer* out, uint32_t sampling_index, uint32_t channels, uint32_t frame_length) {
  if (frame_length > 8191) return false;
  const uint8_t h[7] = {0xFF, 0xF1, static_cast<uint8_t>((1u << 6) | (sampling_index << 2) | (channels >> 2)),
                        static_cast<uint8_t>(((channels & 3u) << 6) | (frame_length >> 11)), static_cast<uint8_t>(frame_length >> 3),
                        static_cast<uint8_t>(((frame_length & 7u) << 5) | 0x1F), 0xFC};
  return out->append(ConstByteView(h, 7));
}

bool encode_aac(const AudioData& audio, uint32_t bitrate_bps, ByteBuffer* out) {
  if (audio.empty() || audio.format().channels < 1 || audio.format().channels > 2) return false;
  allocators::IAllocator& allocator = audio.allocator();
  const uint32_t channels = audio.format().channels;
  AacEncoder encoder(allocator);
  const uint32_t rate = audio.format().sample_rate;
  if (bitrate_bps == 0) {
    const uint32_t scale = rate / 44100 > 1 ? rate / 44100 : 1;
    bitrate_bps = 48000 * channels * scale > 320000 ? 320000 : 48000 * channels * scale;
  }
  // The ADTS headers (7 bytes per block) count against the bitrate.
  const uint64_t header_rate = uint64_t{7} * 8 * rate / 1024;
  if (bitrate_bps > 2 * header_rate) bitrate_bps -= static_cast<uint32_t>(header_rate);
  if (!encoder.init(rate, channels, bitrate_bps)) return false;
  const uint64_t frames = audio.frame_count();
  // One block more than the audio fills, to carry the tail of the last overlap.
  const uint64_t blocks = (frames + 1024u - 1) / 1024u + 1;
  ArrayView<int16_t> block = allocate_view<int16_t>(allocator, uint64_t{1024u} * channels);
  if (block.empty()) return false;
  ByteBuffer raw(allocator);
  const uint64_t mark = out->size();
  bool ok = true;
  for (uint64_t i = 0; ok && i < blocks; ++i) {
    block.fill(0);
    const uint64_t first = i * 1024u;
    if (first < frames) {
      const uint64_t n = frames - first < 1024u ? frames - first : 1024u;
      block.first(n * channels).copy_from(audio.samples().subview(first * channels, n * channels));
    }
    raw.clear();
    ok = encoder.encode_block(block, &raw) && put_adts_header(out, encoder.sampling_index(), channels, static_cast<uint32_t>(raw.size() + 7)) &&
         out->append(raw.view());
  }
  release_view(allocator, block);
  if (!ok) out->truncate(mark);
  return ok;
}

}  // namespace media
