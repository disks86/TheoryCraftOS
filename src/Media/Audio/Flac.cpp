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

#include "Media/Audio/Pcm.hpp"
#include "Media/AudioStreaming.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {
uint8_t crc8(ConstByteView data) {
  uint8_t crc = 0;
  for (uint8_t byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
  }
  return crc;
}

uint16_t crc16(ConstByteView data) {
  uint16_t crc = 0;
  for (uint8_t byte : data) {
    crc ^= static_cast<uint16_t>(byte << 8);
    for (int i = 0; i < 8; ++i) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
  }
  return crc;
}

// Reads bits from a byte string, most significant first. Reading past the end sets `failed` and returns zeros.
struct BitReader {
  ConstByteView data;
  uint64_t bit_position = 0;
  bool failed = false;

  uint64_t bits_left() const { return data.size() * 8 - bit_position; }
  uint32_t read(uint32_t n) {  // n <= 32
    if (n == 0) return 0;
    if (n > bits_left()) {
      failed = true;
      bit_position = data.size() * 8;
      return 0;
    }
    uint64_t v = 0;
    uint32_t got = 0;
    while (got < n) {
      const uint64_t byte = bit_position / 8, offset = bit_position % 8;
      const uint32_t take = 8 - static_cast<uint32_t>(offset) < n - got ? 8 - static_cast<uint32_t>(offset) : n - got;
      v = (v << take) | ((data[byte] >> (8 - offset - take)) & ((1u << take) - 1));
      bit_position += take;
      got += take;
    }
    return static_cast<uint32_t>(v);
  }
  int64_t read_signed(uint32_t n) {  // n <= 32
    if (n == 0) return 0;
    const int64_t v = read(n);
    return v >= (int64_t{1} << (n - 1)) ? v - (int64_t{1} << n) : v;
  }
  uint32_t unary() {  // zeros before the first one
    uint32_t n = 0;
    while (!failed && read(1) == 0) {
      if (++n > (1u << 24)) {
        failed = true;
        break;
      }
    }
    return n;
  }
  void align() { bit_position = (bit_position + 7) & ~uint64_t{7}; }
};

struct StreamInfo {
  uint32_t min_block = 0, max_block = 0, rate = 0, channels = 0, bits = 0;
  uint64_t total = 0;
};

// A frame header's UTF-8-style number (the frame or sample number): up to 7 bytes.
bool read_utf8_number(BitReader& r, uint64_t* value) {
  const uint32_t first = r.read(8);
  if (r.failed) return false;
  uint32_t extra = 0;
  uint64_t v = 0;
  if ((first & 0x80) == 0) {
    v = first;
  } else {
    uint32_t mask = 0x80;
    while (first & mask) {
      ++extra;
      mask >>= 1;
    }
    if (extra < 2 || extra > 7) return false;
    v = first & (mask - 1);
    --extra;
    for (uint32_t i = 0; i < extra; ++i) {
      const uint32_t b = r.read(8);
      if ((b & 0xC0) != 0x80) return false;
      v = (v << 6) | (b & 0x3F);
    }
  }
  *value = v;
  return !r.failed;
}

// Residual coding (partitioned Rice): `count` values into out[order ...].
bool decode_residual(BitReader& r, uint32_t block, uint32_t order, int64_t* out) {
  const uint32_t method = r.read(2);
  if (method > 1) return false;
  const uint32_t partition_order = r.read(4);
  const uint32_t parameter_bits = method == 0 ? 4 : 5, escape = method == 0 ? 15 : 31;
  const uint32_t partitions = 1u << partition_order;
  if ((block >> partition_order) < order || (block & (partitions - 1)) != 0) return false;
  uint32_t at = order;
  for (uint32_t p = 0; p < partitions; ++p) {
    const uint32_t n = (block >> partition_order) - (p == 0 ? order : 0);
    const uint32_t parameter = r.read(parameter_bits);
    if (r.failed) return false;
    if (parameter == escape) {
      const uint32_t bits = r.read(5);
      for (uint32_t i = 0; i < n; ++i) out[at++] = r.read_signed(bits);
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t q = r.unary();
        const uint64_t v = (q << parameter) | r.read(parameter);
        out[at++] = static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
      }
    }
    if (r.failed) return false;
  }
  return true;
}

bool decode_subframe(BitReader& r, uint32_t block, uint32_t bits, int64_t* out) {
  if (r.read(1) != 0) return false;
  const uint32_t type = r.read(6);
  uint32_t wasted = 0;
  if (r.read(1)) wasted = r.unary() + 1;
  if (r.failed || wasted >= bits) return false;
  const uint32_t depth = bits - wasted;
  bool ok = true;
  if (type == 0) {
    const int64_t v = r.read_signed(depth);
    for (uint32_t i = 0; i < block; ++i) out[i] = v;
  } else if (type == 1) {
    for (uint32_t i = 0; i < block; ++i) out[i] = r.read_signed(depth);
  } else if (type >= 8 && type <= 12) {
    const uint32_t order = type - 8;
    if (order > block) return false;
    for (uint32_t i = 0; i < order; ++i) out[i] = r.read_signed(depth);
    ok = decode_residual(r, block, order, out);
    for (uint32_t i = order; ok && i < block; ++i) {
      // Damaged data can make the predictions grow without bound: do the arithmetic modulo 2^64, where that is defined.
      auto u = [&](uint32_t back) { return static_cast<uint64_t>(out[i - back]); };
      uint64_t p;
      switch (order) {
        case 0: p = 0; break;
        case 1: p = u(1); break;
        case 2: p = 2 * u(1) - u(2); break;
        case 3: p = 3 * u(1) - 3 * u(2) + u(3); break;
        default: p = 4 * u(1) - 6 * u(2) + 4 * u(3) - u(4); break;
      }
      out[i] = static_cast<int64_t>(static_cast<uint64_t>(out[i]) + p);
    }
  } else if (type >= 32) {
    const uint32_t order = type - 31;
    if (order > block) return false;
    for (uint32_t i = 0; i < order; ++i) out[i] = r.read_signed(depth);
    const uint32_t precision_code = r.read(4);
    if (precision_code == 15) return false;
    const uint32_t precision = precision_code + 1;
    const int32_t shift = static_cast<int32_t>(r.read_signed(5));
    if (shift < 0) return false;
    int64_t coefficients[32];
    for (uint32_t i = 0; i < order; ++i) coefficients[i] = r.read_signed(precision);
    ok = decode_residual(r, block, order, out);
    for (uint32_t i = order; ok && i < block; ++i) {
      uint64_t sum = 0;
      for (uint32_t j = 0; j < order; ++j) sum += static_cast<uint64_t>(coefficients[j]) * static_cast<uint64_t>(out[i - 1 - j]);
      out[i] = static_cast<int64_t>(static_cast<uint64_t>(out[i]) + static_cast<uint64_t>(static_cast<int64_t>(sum) >> shift));
    }
  } else {
    return false;
  }
  if (!ok || r.failed) return false;
  if (wasted)
    for (uint32_t i = 0; i < block; ++i) out[i] = static_cast<int64_t>(static_cast<uint64_t>(out[i]) << wasted);
  return true;
}
}  // namespace

// A frame header, checked against its CRC-8.
struct FrameHeader {
  uint32_t block = 0, channel_code = 0, bits = 0;
  uint64_t number = 0;       // the frame number (fixed block size) or the first sample's number (variable)
  bool variable = false;
  uint64_t header_bytes = 0;
};

bool parse_frame_header(ConstByteView data, const StreamInfo& info, uint32_t max_block, FrameHeader* h) {
  BitReader r{data, 0};
  if (r.read(14) != 0x3FFE) return false;
  r.read(1);
  h->variable = r.read(1) != 0;
  const uint32_t block_code = r.read(4), rate_code = r.read(4), channel_code = r.read(4), size_code = r.read(3);
  r.read(1);
  if (!read_utf8_number(r, &h->number)) return false;
  uint32_t block = 0;
  if (block_code == 1) block = 192;
  else if (block_code >= 2 && block_code <= 5) block = 576u << (block_code - 2);
  else if (block_code == 6) block = r.read(8) + 1;
  else if (block_code == 7) block = r.read(16) + 1;
  else if (block_code >= 8) block = 256u << (block_code - 8);
  if (rate_code == 12) r.read(8);
  else if (rate_code == 13 || rate_code == 14) r.read(16);
  else if (rate_code == 15) return false;
  static const uint32_t kSizes[8] = {0, 8, 12, 0, 16, 20, 24, 32};
  const uint32_t bits = size_code == 0 ? info.bits : kSizes[size_code];
  if (bits == 0 || block == 0 || block > max_block || channel_code > 10) return false;
  h->header_bytes = r.bit_position / 8;
  const uint32_t stored_crc = r.read(8);
  if (r.failed || crc8(data.subview(0, h->header_bytes)) != stored_crc) return false;
  h->header_bytes = r.bit_position / 8;
  h->block = block;
  h->channel_code = channel_code;
  h->bits = bits;
  return true;
}

// Decodes the frame that starts at data[0] into `pcm` (appended as 16-bit samples, two channels at most). `store` holds
// max_block * info.channels values. *bytes is the length of the frame.
bool decode_frame(ConstByteView data, const StreamInfo& info, int64_t* store, uint32_t max_block, ByteBuffer* pcm, FrameHeader* h, uint64_t* bytes) {
  if (!parse_frame_header(data, info, max_block, h)) return false;
  const uint32_t block = h->block, channel_code = h->channel_code, bits = h->bits;
  const uint32_t out_channels = info.channels > 1 ? 2 : 1;
  const uint32_t channels = channel_code < 8 ? channel_code + 1 : 2;
  if (channels != info.channels) return false;
  ArrayView<int64_t> channel_data[8];
  for (uint32_t c = 0; c < channels; ++c) channel_data[c] = ArrayView<int64_t>(store + uint64_t{c} * max_block, block);
  BitReader r{data, h->header_bytes * 8};
  bool ok = true;
  for (uint32_t c = 0; c < channels && ok; ++c) {
    uint32_t channel_bits = bits;
    if ((channel_code == 8 && c == 1) || (channel_code == 9 && c == 0) || (channel_code == 10 && c == 1)) ++channel_bits;  // the side channel
    ok = decode_subframe(r, block, channel_bits, channel_data[c].data());
  }
  if (!ok) return false;
  r.align();
  const uint64_t frame_bytes = r.bit_position / 8;
  const uint32_t crc = r.read(16);
  if (r.failed || crc16(data.subview(0, frame_bytes)) != crc) return false;
  *bytes = frame_bytes + 2;

  // Undo the stereo decorrelation.
  if (channel_code == 8) {
    for (uint32_t i = 0; i < block; ++i) channel_data[1][i] = channel_data[0][i] - channel_data[1][i];
  } else if (channel_code == 9) {
    for (uint32_t i = 0; i < block; ++i) channel_data[0][i] += channel_data[1][i];
  } else if (channel_code == 10) {
    for (uint32_t i = 0; i < block; ++i) {
      const int64_t mid = channel_data[0][i] * 2 + (channel_data[1][i] & 1), side = channel_data[1][i];
      channel_data[0][i] = (mid + side) >> 1;
      channel_data[1][i] = (mid - side) >> 1;
    }
  }
  for (uint32_t i = 0; i < block && ok; ++i)
    for (uint32_t c = 0; c < out_channels; ++c) {
      const int64_t v = channel_data[c][i];
      const int64_t scaled = bits > 16 ? v >> (bits - 16) : v * (int64_t{1} << (16 - bits));
      ok = pcm->put16(static_cast<uint16_t>(static_cast<int16_t>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled)));
    }
  return ok;
}

bool decode_flac(ConstByteView data, AudioData* out) {
  out->clear();
  if (data.size() < 8 + 34 || detect_audio_format(data) != AudioFileFormat::Flac) return false;
  allocators::IAllocator& allocator = out->allocator();
  StreamInfo info;
  uint64_t at = 4;
  bool have_info = false, last = false;
  while (!last) {
    if (at + 4 > data.size()) return false;
    last = data[at] & 0x80;
    const uint32_t type = data[at] & 0x7F, length = (uint32_t{data[at + 1]} << 16) | (uint32_t{data[at + 2]} << 8) | data[at + 3];
    at += 4;
    if (at + length > data.size()) return false;
    if (type == 0) {
      if (length < 34) return false;
      BitReader r{data.subview(at, length)};
      info.min_block = r.read(16);
      info.max_block = r.read(16);
      r.read(24);
      r.read(24);
      info.rate = r.read(20);
      info.channels = r.read(3) + 1;
      info.bits = r.read(5) + 1;
      info.total = (uint64_t{r.read(4)} << 32) | r.read(32);
      have_info = true;
    }
    at += length;
  }
  if (!have_info || info.rate == 0 || info.bits < 4 || info.max_block < 16) return false;

  // The frames.
  ByteBuffer pcm(allocator);
  const uint32_t out_channels = info.channels > 1 ? 2 : 1;
  const uint32_t max_block = 65536;
  ArrayView<int64_t> store = allocate_view<int64_t>(allocator, uint64_t{max_block} * info.channels);
  if (store.empty()) return false;
  bool ok = true;
  uint64_t frames_done = 0;
  while (ok && at + 6 < data.size()) {
    FrameHeader header;
    uint64_t bytes = 0;
    if (!decode_frame(data.subview(at), info, store.data(), max_block, &pcm, &header, &bytes)) { ok = false; break; }
    at += bytes;
    frames_done += header.block;
  }
  release_view(allocator, store);
  if (!ok || frames_done == 0) return false;
  audio::StreamFormat format;
  format.sample_rate = info.rate;
  format.channels = static_cast<uint8_t>(out_channels);
  format.type = audio::SampleType::S16;
  if (!out->create(format, frames_done)) return false;
  __builtin_memcpy(out->samples().data(), pcm.view().data(), frames_done * out_channels * 2);
  return true;
}

// ---- Encoding -------------------------------------------------------------------------------

namespace {
struct BitWriter {
  ByteBuffer* out;
  uint64_t buffer = 0;
  uint32_t count = 0;
  bool ok = true;
  void write(uint64_t value, uint32_t n) {  // n <= 32
    buffer = (buffer << n) | (value & ((n == 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1)));
    count += n;
    while (count >= 8) {
      ok = ok && out->put8(static_cast<uint8_t>(buffer >> (count - 8)));
      count -= 8;
    }
    buffer &= (uint64_t{1} << count) - 1;
  }
  void write_signed(int64_t v, uint32_t n) { write(static_cast<uint64_t>(v), n); }
  void unary(uint32_t zeros) {
    while (zeros >= 32) {
      write(0, 32);
      zeros -= 32;
    }
    write(1, zeros + 1);
  }
  void align() {
    if (count) write(0, 8 - count);
  }
};

uint64_t zigzag(int64_t v) { return v >= 0 ? static_cast<uint64_t>(v) << 1 : (static_cast<uint64_t>(-(v + 1)) << 1) | 1; }

// The residual of a fixed predictor of `order` over `x`.
void fixed_residual(const int64_t* x, uint32_t n, uint32_t order, int64_t* residual) {
  for (uint32_t i = 0; i < n; ++i) {
    int64_t p = 0;
    if (i >= order) {
      switch (order) {
        case 1: p = x[i - 1]; break;
        case 2: p = 2 * x[i - 1] - x[i - 2]; break;
        case 3: p = 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; break;
        case 4: p = 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; break;
        default: break;
      }
    }
    residual[i] = x[i] - p;
  }
}

// The bits the Rice-coded values residual[from, to) take with parameter `k`.
uint64_t rice_bits(const int64_t* residual, uint32_t from, uint32_t to, uint32_t k) {
  uint64_t bits = 0;
  for (uint32_t i = from; i < to; ++i) bits += (zigzag(residual[i]) >> k) + 1 + k;
  return bits;
}

// Picks the partition order and parameters for `residual` (samples order.. block). Fills `parameters`, returns the order.
uint32_t plan_partitions(const int64_t* residual, uint32_t block, uint32_t order, uint8_t* parameters, uint64_t* total_bits) {
  uint32_t best_order = 0;
  uint64_t best = ~uint64_t{0};
  for (uint32_t po = 0; po <= 8; ++po) {
    const uint32_t partitions = 1u << po;
    if ((block & (partitions - 1)) != 0 || (block >> po) <= order) break;
    uint64_t bits = 0;
    uint8_t chosen[256];
    for (uint32_t p = 0; p < partitions; ++p) {
      const uint32_t from = p == 0 ? order : (block >> po) * p, to = (block >> po) * (p + 1);
      uint64_t sum = 0;
      for (uint32_t i = from; i < to; ++i) sum += zigzag(residual[i]);
      const uint32_t n = to - from;
      uint32_t k = 0;
      while (k < 14 && (n << (k + 1)) <= sum) ++k;  // about log2 of the mean
      uint64_t cost = rice_bits(residual, from, to, k);
      for (uint32_t alt = (k > 0 ? k - 1 : 0); alt <= (k < 14 ? k + 1 : 14); ++alt) {
        const uint64_t c = rice_bits(residual, from, to, alt);
        if (c < cost) {
          cost = c;
          k = alt;
        }
      }
      chosen[p] = static_cast<uint8_t>(k);
      bits += cost + 4;
    }
    if (bits < best) {
      best = bits;
      best_order = po;
      for (uint32_t p = 0; p < partitions; ++p) parameters[p] = chosen[p];
    }
  }
  *total_bits = best;
  return best_order;
}

// Writes one subframe: constant, or the best fixed predictor of orders 0-4 with Rice coding.
void write_subframe(BitWriter& w, const int64_t* x, uint32_t block, uint32_t bits, int64_t* scratch) {
  bool constant = true;
  for (uint32_t i = 1; i < block; ++i)
    if (x[i] != x[0]) { constant = false; break; }
  if (constant) {
    w.write(0, 1);
    w.write(0, 6);
    w.write(0, 1);
    w.write_signed(x[0], bits);
    return;
  }
  uint32_t best_order = 0;
  uint64_t best_bits = ~uint64_t{0};
  uint8_t parameters[256], best_parameters[256];
  uint32_t best_partition_order = 0;
  for (uint32_t order = 0; order <= 4 && order < block; ++order) {
    fixed_residual(x, block, order, scratch);
    uint64_t rice;
    const uint32_t po = plan_partitions(scratch, block, order, parameters, &rice);
    const uint64_t total = rice + uint64_t{order} * bits;
    if (total < best_bits) {
      best_bits = total;
      best_order = order;
      best_partition_order = po;
      for (uint32_t p = 0; p < (1u << po); ++p) best_parameters[p] = parameters[p];
    }
  }
  if (best_bits > uint64_t{block} * bits) {  // no better than verbatim
    w.write(0, 1);
    w.write(1, 6);
    w.write(0, 1);
    for (uint32_t i = 0; i < block; ++i) w.write_signed(x[i], bits);
    return;
  }
  fixed_residual(x, block, best_order, scratch);
  w.write(0, 1);
  w.write(8 + best_order, 6);
  w.write(0, 1);
  for (uint32_t i = 0; i < best_order; ++i) w.write_signed(x[i], bits);
  w.write(0, 2);  // 4-bit Rice parameters
  w.write(best_partition_order, 4);
  const uint32_t partitions = 1u << best_partition_order;
  for (uint32_t p = 0; p < partitions; ++p) {
    const uint32_t from = p == 0 ? best_order : (block >> best_partition_order) * p, to = (block >> best_partition_order) * (p + 1);
    const uint32_t k = best_parameters[p];
    w.write(k, 4);
    for (uint32_t i = from; i < to; ++i) {
      const uint64_t v = zigzag(scratch[i]);
      w.unary(static_cast<uint32_t>(v >> k));
      w.write(v & ((uint64_t{1} << k) - 1), k);
    }
  }
}

uint64_t estimate(const int64_t* x, uint32_t block, int64_t* scratch) {
  uint64_t best = ~uint64_t{0};
  for (uint32_t order = 0; order <= 2 && order < block; ++order) {
    fixed_residual(x, block, order, scratch);
    uint64_t sum = 0;
    for (uint32_t i = order; i < block; ++i) sum += zigzag(scratch[i]);
    if (sum < best) best = sum;
  }
  return best;
}
}  // namespace

bool encode_flac(const AudioData& audio, ByteBuffer* out) {
  if (audio.empty()) return false;
  allocators::IAllocator& allocator = audio.allocator();
  const uint32_t channels = audio.format().channels, rate = audio.format().sample_rate;
  const uint64_t frames = audio.frame_count();
  constexpr uint32_t kBlock = 4096;
  ArrayView<int64_t> work = allocate_view<int64_t>(allocator, uint64_t{kBlock} * 5);
  if (work.empty()) return false;
  int64_t* ch[4] = {work.data(), work.data() + kBlock, work.data() + 2 * kBlock, work.data() + 3 * kBlock};  // left, right, mid, side
  int64_t* scratch = work.data() + 4 * kBlock;
  ByteBuffer body(allocator);
  uint32_t min_frame = 0xFFFFFFFF, max_frame = 0;
  bool ok = true;
  uint64_t frame_number = 0;
  for (uint64_t start = 0; start < frames && ok; start += kBlock, ++frame_number) {
    const uint32_t block = static_cast<uint32_t>(frames - start < kBlock ? frames - start : kBlock);
    for (uint32_t i = 0; i < block; ++i) {
      ch[0][i] = audio.samples()[(start + i) * channels];
      if (channels == 2) {
        ch[1][i] = audio.samples()[(start + i) * channels + 1];
        ch[2][i] = (ch[0][i] + ch[1][i]) >> 1;
        ch[3][i] = ch[0][i] - ch[1][i];
      }
    }
    // The channel arrangement: independent, left+side, side+right or mid+side, whichever looks smallest.
    uint32_t mode = 1;  // 1 = independent (channels - 1 = 0 or 1)
    const int64_t* subs[2] = {ch[0], channels == 2 ? ch[1] : nullptr};
    uint32_t assignment = channels - 1;
    uint32_t widths[2] = {16, 16};
    if (channels == 2) {
      const uint64_t el = estimate(ch[0], block, scratch), er = estimate(ch[1], block, scratch), em = estimate(ch[2], block, scratch), es = estimate(ch[3], block, scratch);
      uint64_t best = el + er;
      if (el + es < best) { best = el + es; mode = 8; }
      if (es + er < best) { best = es + er; mode = 9; }
      if (em + es < best) { best = em + es; mode = 10; }
      if (mode == 8) { subs[0] = ch[0]; subs[1] = ch[3]; assignment = 8; widths[1] = 17; }
      else if (mode == 9) { subs[0] = ch[3]; subs[1] = ch[1]; assignment = 9; widths[0] = 17; }
      else if (mode == 10) { subs[0] = ch[2]; subs[1] = ch[3]; assignment = 10; widths[1] = 17; }
    }
    ByteBuffer frame(allocator);
    BitWriter w{&frame};
    w.write(0x3FFE, 14);
    w.write(0, 1);
    w.write(0, 1);  // fixed block size
    const uint32_t block_code = block == kBlock ? 12 : block <= 256 ? 6 : 7;
    w.write(block_code, 4);
    w.write(0, 4);  // the sample rate is in the stream info
    w.write(assignment, 4);
    w.write(4, 3);  // 16 bits
    w.write(0, 1);
    // The frame number as UTF-8.
    if (frame_number < 0x80) {
      w.write(frame_number, 8);
    } else if (frame_number < 0x800) {
      w.write(0xC0 | (frame_number >> 6), 8);
      w.write(0x80 | (frame_number & 0x3F), 8);
    } else if (frame_number < 0x10000) {
      w.write(0xE0 | (frame_number >> 12), 8);
      w.write(0x80 | ((frame_number >> 6) & 0x3F), 8);
      w.write(0x80 | (frame_number & 0x3F), 8);
    } else if (frame_number < 0x200000) {
      w.write(0xF0 | (frame_number >> 18), 8);
      w.write(0x80 | ((frame_number >> 12) & 0x3F), 8);
      w.write(0x80 | ((frame_number >> 6) & 0x3F), 8);
      w.write(0x80 | (frame_number & 0x3F), 8);
    } else {
      ok = false;
      break;
    }
    if (block_code == 6) w.write(block - 1, 8);
    else if (block_code == 7) w.write(block - 1, 16);
    ok = ok && w.ok;
    if (!ok) break;
    // The header's CRC-8 covers the bytes so far (they are whole bytes here).
    const uint8_t header_crc = crc8(frame.view());
    w.write(header_crc, 8);
    for (uint32_t c = 0; c < channels; ++c) write_subframe(w, subs[c], block, widths[c], scratch);
    w.align();
    const uint16_t frame_crc = crc16(frame.view());
    w.write(frame_crc, 16);
    ok = ok && w.ok;
    if (frame.size() < min_frame) min_frame = static_cast<uint32_t>(frame.size());
    if (frame.size() > max_frame) max_frame = static_cast<uint32_t>(frame.size());
    ok = ok && body.append(frame.view());
  }
  release_view(allocator, work);
  if (!ok) return false;
  // The header: "fLaC", then the stream info as the only (last) metadata block.
  ByteBuffer header(allocator);
  BitWriter h{&header};
  h.write(kBlock > frames ? static_cast<uint32_t>(frames < 16 ? 16 : frames) : kBlock, 16);
  h.write(kBlock, 16);
  h.write(min_frame, 24);
  h.write(max_frame, 24);
  h.write(rate, 20);
  h.write(channels - 1, 3);
  h.write(15, 5);  // 16 bits
  h.write(frames >> 32, 4);
  h.write(frames & 0xFFFFFFFFu, 32);
  for (int i = 0; i < 4; ++i) h.write(0, 32);  // no MD5 signature
  if (!h.ok) return false;
  return out->put_tag("fLaC") && out->put8(0x80) && out->put8(0) && out->put8(0) && out->put8(34) && out->append(header.view()) && out->append(body.view());
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// Streaming: the file is read through a window and decoded a frame at a time.

namespace {

class FlacStream final : public StreamingPcmSource {
 public:
  FlacStream(IByteSource& source, allocators::IAllocator& allocator) : source_(source), allocator_(allocator), pcm_(allocator), seek_table_(allocator) {}
  ~FlacStream() {
    release_view(allocator_, store_);
    release_view(allocator_, window_);
  }

  bool init() {
    uint8_t magic[4];
    if (!read_exact(source_, 0, ByteView(magic)) || __builtin_memcmp(magic, "fLaC", 4) != 0) return false;
    uint64_t at = 4;
    bool have_info = false, last = false;
    while (!last) {
      uint8_t h[4];
      if (!read_exact(source_, at, ByteView(h))) return false;
      last = h[0] & 0x80;
      const uint32_t type = h[0] & 0x7F, length = (uint32_t{h[1]} << 16) | (uint32_t{h[2]} << 8) | h[3];
      at += 4;
      if (at + length > source_.size()) return false;
      if (type == 0) {
        if (length < 34) return false;
        uint8_t body[34];
        if (!read_exact(source_, at, ByteView(body))) return false;
        BitReader r{ConstByteView(body, 34), 0};
        info_.min_block = r.read(16);
        info_.max_block = r.read(16);
        r.read(24);
        r.read(24);
        info_.rate = r.read(20);
        info_.channels = r.read(3) + 1;
        info_.bits = r.read(5) + 1;
        info_.total = (uint64_t{r.read(4)} << 32) | r.read(32);
        have_info = true;
      } else if (type == 3 && length <= (1u << 20) && length % 18 == 0) {   // SEEKTABLE: 18 bytes a point
        if (seek_table_.put_zeros(length) && !read_exact(source_, at, ByteView(const_cast<uint8_t*>(seek_table_.view().data()), length))) return false;
      }
      at += length;
    }
    if (!have_info || info_.rate == 0 || info_.bits < 4 || info_.max_block < 16) return false;
    frames_start_ = at;
    max_block_ = info_.max_block;
    store_ = allocate_view<int64_t>(allocator_, uint64_t{max_block_} * info_.channels);
    window_ = allocate_view<uint8_t>(allocator_, kWindowBytes);
    if (store_.empty() || window_.empty()) return false;
    format_.sample_rate = info_.rate;
    format_.channels = static_cast<uint8_t>(info_.channels > 1 ? 2 : 1);
    format_.type = audio::SampleType::S16;
    frames_ = info_.total ? info_.total : find_total();
    cur_ = frames_start_;
    next_sample_ = 0;
    return frames_ > 0 && format_.valid();
  }

 protected:
  bool refill(uint64_t frame) override {
    if (frame < next_sample_ || frame - next_sample_ > (uint64_t{1} << 20)) {
      if (!seek(frame)) return false;
    }
    uint64_t window_start = 0;
    bool have = false;
    for (int attempt = 0; attempt < 2 && !have; ++attempt) {
      while (next_sample_ <= frame || !have) {
        if (next_sample_ >= frames_ && frame >= frames_) return false;
        const uint64_t start = next_sample_;
        pcm_.clear();
        uint32_t block = 0;
        if (!decode_frame_here(&block)) break;
        if (start + block > frame) {
          window_start = start;
          have = true;
          break;
        }
      }
      if (!have && attempt == 0) {   // the place found by searching was wrong: start again from the beginning and decode forward
        cur_ = frames_start_;
        next_sample_ = 0;
      }
    }
    if (!have) return false;
    while (pcm_.size() / format_.frame_size() < 4096 && next_sample_ < frames_) {   // a little more, so that playing on does not refill every frame
      uint32_t block = 0;
      if (!decode_frame_here(&block)) break;
    }
    set_window(window_start, pcm_.view());
    return true;
  }

 private:
  static constexpr uint32_t kWindowBytes = 262144, kNeedBytes = 131072;

  // The bytes from `at` on, at least kNeedBytes of them unless the file ends sooner.
  ConstByteView bytes_from(uint64_t at) {
    if (at >= source_.size()) return ConstByteView();
    if (!(at >= window_pos_ && at < window_pos_ + window_len_ && (at + kNeedBytes <= window_pos_ + window_len_ || window_pos_ + window_len_ >= source_.size()))) {
      window_pos_ = at;
      window_len_ = source_.read(at, window_);
    }
    if (at < window_pos_ || at >= window_pos_ + window_len_) return ConstByteView();
    return ConstByteView(window_).subview(at - window_pos_, window_pos_ + window_len_ - at);
  }

  // Decodes the frame at cur_ and appends it to pcm_.
  bool decode_frame_here(uint32_t* block) {
    const ConstByteView v = bytes_from(cur_);
    if (v.size() < 6) return false;
    FrameHeader h;
    uint64_t bytes = 0;
    if (!decode_frame(v, info_, store_.data(), max_block_, &pcm_, &h, &bytes)) return false;
    cur_ += bytes;
    next_sample_ += h.block;
    *block = h.block;
    return true;
  }

  // The first frame header at or after `from` (before `limit`).
  bool find_header(uint64_t from, uint64_t limit, uint64_t* at, FrameHeader* h) {
    uint64_t pos = from;
    while (pos < limit && pos + 16 <= source_.size()) {
      const ConstByteView v = bytes_from(pos);
      if (v.size() < 16) return false;
      const uint64_t scan = v.size() - 15 < limit - pos ? v.size() - 15 : limit - pos;
      for (uint64_t i = 0; i < scan; ++i) {
        if (v[i] == 0xFF && (v[i + 1] & 0xFE) == 0xF8 && parse_frame_header(v.subview(i), info_, max_block_, h)) {
          *at = pos + i;
          return true;
        }
      }
      pos += scan;
    }
    return false;
  }

  uint64_t sample_of(const FrameHeader& h) const { return h.variable ? h.number : h.number * info_.min_block; }

  bool seek(uint64_t frame) {
    cur_ = frames_start_;
    next_sample_ = 0;
    if (seek_table_.size() >= 18) {   // the last point at or before the frame
      const uint8_t* t = seek_table_.view().data();
      for (uint64_t i = 0; i + 18 <= seek_table_.size(); i += 18) {
        uint64_t sample = 0, offset = 0;
        for (int k = 0; k < 8; ++k) sample = (sample << 8) | t[i + k], offset = (offset << 8) | t[i + 8 + k];
        if (sample == 0xFFFFFFFFFFFFFFFFull) break;   // a placeholder
        if (sample > frame) break;
        if (frames_start_ + offset < source_.size()) {
          cur_ = frames_start_ + offset;
          next_sample_ = sample;
        }
      }
      return true;
    }
    // No seek table: look for frames by halving the part of the file that holds the place.
    uint64_t lo = frames_start_, lo_sample = 0, hi = source_.size();
    while (hi - lo > 65536) {
      const uint64_t mid = lo + (hi - lo) / 2;
      FrameHeader h;
      uint64_t pos = 0;
      if (!find_header(mid, hi, &pos, &h)) {
        hi = mid;
        continue;
      }
      const uint64_t sample = sample_of(h);
      if (sample <= frame) {
        lo = pos;
        lo_sample = sample;
      } else {
        hi = mid;
      }
    }
    cur_ = lo;
    next_sample_ = lo_sample;
    return true;
  }

  // The length of a file whose header does not say: the last frame, found from the end.
  uint64_t find_total() {
    const uint64_t size = source_.size();
    uint64_t from = size > 262144 ? size - 262144 : frames_start_;
    FrameHeader h;
    uint64_t pos = 0, best = 0;
    while (from < size && find_header(from, size, &pos, &h)) {
      cur_ = pos;
      pcm_.clear();
      FrameHeader d;
      uint64_t bytes = 0;
      const ConstByteView v = bytes_from(pos);
      if (decode_frame(v, info_, store_.data(), max_block_, &pcm_, &d, &bytes) && pos + bytes + 6 >= size) best = sample_of(d) + d.block;
      from = pos + 1;
    }
    pcm_.clear();
    return best;
  }

  IByteSource& source_;
  allocators::IAllocator& allocator_;
  StreamInfo info_;
  ByteBuffer pcm_, seek_table_;
  ArrayView<int64_t> store_;
  ByteView window_;
  uint64_t window_pos_ = 0, window_len_ = 0;
  uint64_t frames_start_ = 0, cur_ = 0, next_sample_ = 0;
  uint32_t max_block_ = 0;
};

}  // namespace

UniquePointer<audio::IPcmSource> open_flac_stream(IByteSource& source, allocators::IAllocator& allocator) {
  UniquePointer<FlacStream> stream = UniquePointer<FlacStream>::make(allocator, source, allocator);
  if (!stream || !stream->init()) return UniquePointer<audio::IPcmSource>();
  return UniquePointer<audio::IPcmSource>(util::move(stream));
}

}  // namespace media
