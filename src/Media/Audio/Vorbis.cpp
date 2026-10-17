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

// Ogg Vorbis I decoder: Ogg page parsing, the setup header (codebooks, floor type 1, residue types 0-2, channel coupling,
// mappings, modes), and the audio packets through to windowed, overlapped PCM. Written from the Vorbis I specification.
// Not handled: floor type 0 (obsolete; no current encoder writes it), chained or multiplexed streams (the first logical
// stream is read, the rest of the file is ignored), Ogg page checksums (not verified).
#include "Math/FloatMath.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/Audio/Mdct.hpp"

namespace media {

namespace {
constexpr uint32_t kMaxChannels = 255;
constexpr uint32_t kMaxEntries = 1u << 20;
constexpr uint64_t kMaxVectorFloats = 1u << 24;

uint32_t ilog(uint32_t v) {
  uint32_t n = 0;
  while (v) { ++n; v >>= 1; }
  return n;
}

// Everything the setup header builds lives here and goes away together.
class Arena {
 public:
  explicit Arena(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Arena() {
    Block* b = head_;
    while (b) {
      Block* next = b->next;
      allocator_.deallocate(b);
      b = next;
    }
  }
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  // Zeroed, 16-byte aligned memory for `count` objects of T (null if it ran out).
  template <class T>
  T* make(uint64_t count) {
    const uint64_t bytes = (count * sizeof(T) + 15) & ~uint64_t{15};
    if (count > (uint64_t{1} << 32) || bytes + sizeof(Block) < bytes) return nullptr;
    Block* b = static_cast<Block*>(allocator_.allocate(sizeof(Block) + bytes));
    if (!b) return nullptr;
    b->next = head_;
    head_ = b;
    uint8_t* mem = reinterpret_cast<uint8_t*>(b) + sizeof(Block);
    for (uint64_t i = 0; i < bytes; ++i) mem[i] = 0;
    return reinterpret_cast<T*>(mem);
  }

 private:
  struct alignas(16) Block {
    Block* next;
  };
  allocators::IAllocator& allocator_;
  Block* head_ = nullptr;
};

struct Bits {
  ConstByteView d;
  uint64_t pos = 0;
  bool eop = false;  // read past the end of the packet
  uint32_t read(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t byte = pos >> 3;
      if (byte >= d.size()) { eop = true; return v; }
      v |= uint32_t{(d[byte] >> (pos & 7)) & 1u} << i;
      ++pos;
    }
    return v;
  }
};

struct Codebook {
  uint32_t dims = 0, entries = 0;
  int32_t* tree = nullptr;  // two children per node: 0 unset, > 0 an internal node, < 0 a leaf for entry -(v + 1)
  float* vq = nullptr;      // entries x dims, or null when the book has no vector lookup
};

float unpack_float32(uint32_t x) {
  double mantissa = static_cast<double>(x & 0x1FFFFF);
  const int32_t exponent = static_cast<int32_t>((x & 0x7FE00000) >> 21) - 788;
  if (x & 0x80000000u) mantissa = -mantissa;
  double scale = 1.0;
  for (int32_t e = exponent; e > 0; --e) scale *= 2.0;
  for (int32_t e = exponent; e < 0; ++e) scale *= 0.5;
  return static_cast<float>(mantissa * scale);
}

// A scalar from the book: the entry whose codeword comes next in the stream, or -1.
int32_t decode_scalar(Bits& b, const Codebook& cb) {
  int32_t node = 0;
  for (uint32_t depth = 0; depth < 33; ++depth) {
    const uint32_t bit = b.read(1);
    if (b.eop) return -1;
    const int32_t next = cb.tree[node * 2 + bit];
    if (next < 0) return -next - 1;
    if (next == 0) return -1;
    node = next;
  }
  return -1;
}

bool read_codebook(Bits& b, Arena& arena, Codebook* cb) {
  if (b.read(24) != 0x564342) return false;
  cb->dims = b.read(16);
  cb->entries = b.read(24);
  if (b.eop || cb->dims == 0 || cb->entries == 0 || cb->entries > kMaxEntries) return false;
  uint8_t* lengths = arena.make<uint8_t>(cb->entries);
  if (!lengths) return false;
  const uint32_t entries = cb->entries;
  if (b.read(1)) {  // ordered
    uint32_t entry = 0, length = b.read(5) + 1;
    while (entry < entries) {
      const uint32_t number = b.read(ilog(entries - entry));
      if (b.eop || entry + number > entries || length > 32) return false;
      for (uint32_t i = 0; i < number; ++i) lengths[entry + i] = static_cast<uint8_t>(length);
      entry += number;
      ++length;
    }
  } else {
    const bool sparse = b.read(1);
    for (uint32_t i = 0; i < entries; ++i) {
      if (sparse && !b.read(1)) continue;
      lengths[i] = static_cast<uint8_t>(b.read(5) + 1);
    }
  }
  if (b.eop) return false;

  // Codewords: each entry in order takes the lowest unused code of its length.
  uint32_t used = 0;
  for (uint32_t i = 0; i < entries; ++i) used += lengths[i] != 0;
  if (used == 0) return false;
  cb->tree = arena.make<int32_t>((uint64_t{used} + 40) * 2);
  if (!cb->tree) return false;
  uint32_t nodes = 1;
  uint32_t available[33] = {};
  bool first = true;
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t len = lengths[i];
    if (!len) continue;
    uint32_t code;
    if (first) {
      first = false;
      code = 0;
      for (uint32_t y = 1; y <= len; ++y) available[y] = 1u << (32 - y);
    } else {
      uint32_t z = len;
      while (z > 0 && !available[z]) --z;
      if (z == 0) return false;  // more codes than the tree has room for
      const uint32_t res = available[z];
      available[z] = 0;
      code = res >> (32 - len);
      for (uint32_t y = len; y > z; --y) available[y] = res + (1u << (32 - y));
    }
    int32_t node = 0;
    for (uint32_t k = 0; k < len; ++k) {
      const uint32_t bit = (code >> (len - 1 - k)) & 1;
      int32_t& child = cb->tree[node * 2 + bit];
      if (k + 1 == len) {
        if (child != 0) return false;
        child = -static_cast<int32_t>(i) - 1;
      } else {
        if (child < 0) return false;
        if (child == 0) {
          if (nodes >= used + 40) return false;
          child = static_cast<int32_t>(nodes++);
        }
        node = child;
      }
    }
  }
  if (used > 1)
    for (uint32_t y = 1; y <= 32; ++y)
      if (available[y]) return false;  // an incomplete tree (only a one-entry book may be)

  const uint32_t lookup_type = b.read(4);
  if (lookup_type > 2) return false;
  if (lookup_type == 0) return !b.eop;
  const float minimum = unpack_float32(b.read(32)), delta = unpack_float32(b.read(32));
  const uint32_t value_bits = b.read(4) + 1;
  const bool sequence = b.read(1);
  uint64_t lookup_values;
  if (lookup_type == 1) {
    lookup_values = 1;
    while (true) {  // the largest r with r^dims <= entries
      uint64_t p = 1;
      bool over = false;
      for (uint32_t d = 0; d < cb->dims && !over; ++d) {
        p *= lookup_values + 1;
        over = p > entries;
      }
      if (over) break;
      ++lookup_values;
    }
  } else {
    lookup_values = uint64_t{entries} * cb->dims;
  }
  if (lookup_values > kMaxVectorFloats || uint64_t{entries} * cb->dims > kMaxVectorFloats) return false;
  uint32_t* multiplicands = arena.make<uint32_t>(lookup_values);
  cb->vq = arena.make<float>(uint64_t{entries} * cb->dims);
  if (!multiplicands || !cb->vq) return false;
  for (uint64_t i = 0; i < lookup_values; ++i) multiplicands[i] = b.read(value_bits);
  if (b.eop) return false;
  for (uint32_t e = 0; e < entries; ++e) {
    float last = 0;
    uint64_t divisor = 1;
    for (uint32_t d = 0; d < cb->dims; ++d) {
      const uint64_t index = lookup_type == 1 ? (e / divisor) % lookup_values : uint64_t{e} * cb->dims + d;
      const float v = static_cast<float>(multiplicands[index]) * delta + minimum + last;
      cb->vq[uint64_t{e} * cb->dims + d] = v;
      if (sequence) last = v;
      if (lookup_type == 1) divisor *= lookup_values;
    }
  }
  return true;
}

struct Floor1 {
  uint32_t partitions = 0;
  uint8_t part_class[32] = {};
  uint8_t class_dim[16] = {}, class_sub[16] = {}, class_master[16] = {};
  int16_t class_books[16][8] = {};
  uint32_t multiplier = 1, rangebits = 0, count = 0;
  uint16_t x[256] = {};
  uint16_t sorted[256] = {};
  uint16_t low[256] = {}, high[256] = {};
};

struct Residue {
  uint32_t type = 0, begin = 0, end = 0, partition_size = 0, classifications = 0, classbook = 0;
  int16_t books[64][8] = {};
};

struct Mapping {
  uint32_t submaps = 1, coupling_steps = 0;
  uint8_t magnitude[256] = {}, angle[256] = {}, mux[kMaxChannels] = {};
  uint8_t floor[16] = {}, residue[16] = {};
};

struct Mode {
  bool blockflag = false;
  uint32_t mapping = 0;
};

// Vorbis floor values: 10^((i - 255) * 7 / 256) for i in 0..255 (the inverse-dB table, which is exactly this series).
float g_inverse_db[256];
bool g_db_ready = false;
void init_db_table() {
  if (g_db_ready) return;
  for (int i = 0; i < 256; ++i) g_inverse_db[i] = static_cast<float>(fmath::pow(10.0, (i - 255) * 7.0 / 256.0));
  g_db_ready = true;
}

// The rising half of the power-sine window of a block of n samples.
struct MdctPlan {
  float* window_slope = nullptr;
};

bool make_plan(Arena& arena, uint32_t n, MdctPlan* p) {
  const uint32_t m = n / 2;
  p->window_slope = arena.make<float>(m);
  if (!p->window_slope) return false;
  const double pi = 3.14159265358979323846;
  for (uint32_t i = 0; i < m; ++i) {
    const double s = fmath::sin(pi / 2.0 * (i + 0.5) / m);
    p->window_slope[i] = static_cast<float>(fmath::sin(pi / 2.0 * s * s));
  }
  return true;
}

constexpr float kScale = 32768.0f;

class Decoder {
 public:
  explicit Decoder(allocators::IAllocator& allocator) : allocator_(allocator), arena_(allocator), pcm_(allocator), mdct0_(allocator), mdct1_(allocator) {}
  ~Decoder() {
    allocator_.deallocate(work_);
    allocator_.deallocate(classes_);
  }
  bool decode(ConstByteView data, AudioData* out);

 private:
  bool on_packet(ConstByteView packet);
  bool read_identification(Bits& b);
  bool read_setup(Bits& b);
  bool read_floor(Bits& b, Floor1* f);
  bool read_residue(Bits& b, Residue* r);
  bool read_mapping(Bits& b, Mapping* m);
  bool decode_floor(Bits& b, const Floor1& f, float* out, uint32_t half);
  void decode_residue(Bits& b, const Residue& r, uint32_t size, uint32_t nch, float** vec, const bool* skip);
  bool audio_packet(Bits& b);

  allocators::IAllocator& allocator_;
  Arena arena_;
  ByteBuffer pcm_;  // int16 samples, interleaved over out_channels_
  uint32_t state_ = 0;  // 0 identification, 1 comment, 2 setup, 3 audio, 4 failed
  uint32_t channels_ = 0, rate_ = 0, out_channels_ = 0, front_[2] = {0, 1};
  uint32_t blocksize_[2] = {0, 0};
  Codebook* books_ = nullptr;
  uint32_t book_count_ = 0;
  Floor1* floors_ = nullptr;
  uint32_t floor_count_ = 0;
  Residue* residues_ = nullptr;
  uint32_t residue_count_ = 0;
  Mapping* mappings_ = nullptr;
  uint32_t mapping_count_ = 0;
  Mode modes_[64];
  uint32_t mode_count_ = 0;
  MdctPlan plans_[2];
  Mdct mdct0_, mdct1_;
  // Per-packet working memory, one block of allocation carved up in prepare().
  uint8_t* work_ = nullptr;
  float* floor_[kMaxChannels] = {};
  float* spectrum_[kMaxChannels] = {};
  float* overlap_[kMaxChannels] = {};
  float* time_ = nullptr;
  float* window_ = nullptr;
  uint8_t* classes_ = nullptr;
  uint64_t classes_size_ = 0;
  bool have_previous_ = false;
  uint32_t previous_n_ = 0;
  uint64_t frames_out_ = 0;
  bool prepared_ = false;
  bool prepare();
};

bool Decoder::read_identification(Bits& b) {
  if (b.read(32) != 0) return false;  // version
  channels_ = b.read(8);
  rate_ = b.read(32);
  b.read(32);
  b.read(32);
  b.read(32);
  const uint32_t b0 = b.read(4), b1 = b.read(4);
  if (b.eop || channels_ == 0 || rate_ < audio::StreamFormat::kMinRate || rate_ > audio::StreamFormat::kMaxRate) return false;
  if (b0 < 6 || b0 > 13 || b1 < 6 || b1 > 13 || b0 > b1 || !b.read(1)) return false;
  blocksize_[0] = 1u << b0;
  blocksize_[1] = 1u << b1;
  out_channels_ = channels_ > 2 ? 2 : channels_;
  // Which coded channels are the front left and right: Vorbis puts the centre between them for 3, 5, 6, 7 and 8 channels.
  front_[0] = 0;
  front_[1] = channels_ == 3 || channels_ >= 5 ? 2 : 1;
  return true;
}

bool Decoder::read_floor(Bits& b, Floor1* f) {
  f->partitions = b.read(5);
  int32_t max_class = -1;  // no partitions, no classes
  for (uint32_t i = 0; i < f->partitions; ++i) {
    f->part_class[i] = static_cast<uint8_t>(b.read(4));
    if (f->part_class[i] > max_class) max_class = f->part_class[i];
  }
  for (int32_t c = 0; c <= max_class; ++c) {
    f->class_dim[c] = static_cast<uint8_t>(b.read(3) + 1);
    f->class_sub[c] = static_cast<uint8_t>(b.read(2));
    if (f->class_sub[c]) {
      f->class_master[c] = static_cast<uint8_t>(b.read(8));
      if (f->class_master[c] >= book_count_) return false;
    }
    for (uint32_t k = 0; k < (1u << f->class_sub[c]); ++k) {
      f->class_books[c][k] = static_cast<int16_t>(static_cast<int32_t>(b.read(8)) - 1);
      if (f->class_books[c][k] >= static_cast<int32_t>(book_count_)) return false;
    }
  }
  f->multiplier = b.read(2) + 1;
  f->rangebits = b.read(4);
  f->x[0] = 0;
  f->x[1] = static_cast<uint16_t>(1u << f->rangebits);
  uint32_t n = 2;
  for (uint32_t i = 0; i < f->partitions; ++i)
    for (uint32_t j = 0; j < f->class_dim[f->part_class[i]]; ++j) f->x[n++] = static_cast<uint16_t>(b.read(f->rangebits));
  f->count = n;
  if (b.eop) return false;
  // Order the posts by position and find each post's neighbours among the posts before it.
  for (uint32_t i = 0; i < n; ++i) f->sorted[i] = static_cast<uint16_t>(i);
  for (uint32_t i = 1; i < n; ++i) {  // insertion sort by x
    const uint16_t v = f->sorted[i];
    uint32_t j = i;
    while (j > 0 && f->x[f->sorted[j - 1]] > f->x[v]) { f->sorted[j] = f->sorted[j - 1]; --j; }
    f->sorted[j] = v;
  }
  for (uint32_t i = 1; i < n; ++i)
    if (f->x[f->sorted[i]] == f->x[f->sorted[i - 1]]) return false;  // positions must be distinct
  for (uint32_t i = 2; i < n; ++i) {
    int32_t lo = -1, hi = -1;
    for (uint32_t j = 0; j < i; ++j) {
      if (f->x[j] < f->x[i] && (lo < 0 || f->x[j] > f->x[lo])) lo = static_cast<int32_t>(j);
      if (f->x[j] > f->x[i] && (hi < 0 || f->x[j] < f->x[hi])) hi = static_cast<int32_t>(j);
    }
    if (lo < 0 || hi < 0) return false;
    f->low[i] = static_cast<uint16_t>(lo);
    f->high[i] = static_cast<uint16_t>(hi);
  }
  return true;
}

bool Decoder::read_residue(Bits& b, Residue* r) {
  r->begin = b.read(24);
  r->end = b.read(24);
  r->partition_size = b.read(24) + 1;
  r->classifications = b.read(6) + 1;
  r->classbook = b.read(8);
  if (r->classbook >= book_count_ || r->end < r->begin) return false;
  uint8_t cascade[64];
  for (uint32_t i = 0; i < r->classifications; ++i) {
    const uint32_t low = b.read(3);
    const uint32_t high = b.read(1) ? b.read(5) : 0;
    cascade[i] = static_cast<uint8_t>(high * 8 + low);
  }
  for (uint32_t i = 0; i < r->classifications; ++i)
    for (uint32_t j = 0; j < 8; ++j) {
      if (cascade[i] & (1u << j)) {
        const uint32_t book = b.read(8);
        if (book >= book_count_ || !books_[book].vq) return false;
        r->books[i][j] = static_cast<int16_t>(book);
      } else {
        r->books[i][j] = -1;
      }
    }
  return !b.eop;
}

bool Decoder::read_mapping(Bits& b, Mapping* m) {
  if (b.read(16) != 0) return false;
  m->submaps = b.read(1) ? b.read(4) + 1 : 1;
  m->coupling_steps = 0;
  if (b.read(1)) {
    m->coupling_steps = b.read(8) + 1;
    for (uint32_t i = 0; i < m->coupling_steps; ++i) {
      const uint32_t mag = b.read(ilog(channels_ - 1)), ang = b.read(ilog(channels_ - 1));
      if (mag == ang || mag >= channels_ || ang >= channels_) return false;
      m->magnitude[i] = static_cast<uint8_t>(mag);
      m->angle[i] = static_cast<uint8_t>(ang);
    }
  }
  if (b.read(2) != 0) return false;
  if (m->submaps > 1)
    for (uint32_t c = 0; c < channels_; ++c) {
      m->mux[c] = static_cast<uint8_t>(b.read(4));
      if (m->mux[c] >= m->submaps) return false;
    }
  for (uint32_t s = 0; s < m->submaps; ++s) {
    b.read(8);
    m->floor[s] = static_cast<uint8_t>(b.read(8));
    m->residue[s] = static_cast<uint8_t>(b.read(8));
    if (m->floor[s] >= floor_count_ || m->residue[s] >= residue_count_) return false;
  }
  return !b.eop;
}

bool Decoder::read_setup(Bits& b) {
  book_count_ = b.read(8) + 1;
  books_ = arena_.make<Codebook>(book_count_);
  if (!books_) return false;
  for (uint32_t i = 0; i < book_count_; ++i)
    if (!read_codebook(b, arena_, &books_[i])) return false;
  const uint32_t times = b.read(6) + 1;
  for (uint32_t i = 0; i < times; ++i)
    if (b.read(16) != 0) return false;
  floor_count_ = b.read(6) + 1;
  floors_ = arena_.make<Floor1>(floor_count_);
  if (!floors_) return false;
  for (uint32_t i = 0; i < floor_count_; ++i) {
    if (b.read(16) != 1) return false;  // floor type 0 is not supported
    if (!read_floor(b, &floors_[i])) return false;
  }
  residue_count_ = b.read(6) + 1;
  residues_ = arena_.make<Residue>(residue_count_);
  if (!residues_) return false;
  for (uint32_t i = 0; i < residue_count_; ++i) {
    residues_[i].type = b.read(16);
    if (residues_[i].type > 2 || !read_residue(b, &residues_[i])) return false;
  }
  mapping_count_ = b.read(6) + 1;
  mappings_ = arena_.make<Mapping>(mapping_count_);
  if (!mappings_) return false;
  for (uint32_t i = 0; i < mapping_count_; ++i)
    if (!read_mapping(b, &mappings_[i])) return false;
  mode_count_ = b.read(6) + 1;
  for (uint32_t i = 0; i < mode_count_; ++i) {
    modes_[i].blockflag = b.read(1);
    if (b.read(16) != 0 || b.read(16) != 0) return false;
    modes_[i].mapping = b.read(8);
    if (modes_[i].mapping >= mapping_count_) return false;
  }
  return !b.eop && b.read(1) == 1 && prepare();
}

// Plans and working memory for both block sizes.
bool Decoder::prepare() {
  init_db_table();
  if (!make_plan(arena_, blocksize_[0], &plans_[0]) || !make_plan(arena_, blocksize_[1], &plans_[1]) || !mdct0_.init(blocksize_[0]) || !mdct1_.init(blocksize_[1])) return false;
  const uint32_t max_n = blocksize_[1], half = max_n / 2;
  const uint64_t floats = uint64_t{channels_} * (2 * half + max_n) + max_n + max_n;
  work_ = static_cast<uint8_t*>(allocator_.allocate(floats * sizeof(float) + 64));
  if (!work_) return false;
  float* at = reinterpret_cast<float*>((reinterpret_cast<uintptr_t>(work_) + 15) & ~uintptr_t{15});
  for (uint32_t i = 0; i < floats; ++i) at[i] = 0;
  for (uint32_t c = 0; c < channels_; ++c) {
    floor_[c] = at; at += half;
    spectrum_[c] = at; at += half;
    overlap_[c] = at; at += max_n;
  }
  time_ = at; at += max_n;
  window_ = at; at += max_n;
  prepared_ = true;
  return true;
}

bool Decoder::decode_floor(Bits& b, const Floor1& f, float* out, uint32_t half) {
  static const uint32_t kRanges[4] = {256, 128, 86, 64};
  const uint32_t range = kRanges[f.multiplier - 1];
  if (!b.read(1)) return false;
  int32_t y[256];
  y[0] = static_cast<int32_t>(b.read(ilog(range - 1)));
  y[1] = static_cast<int32_t>(b.read(ilog(range - 1)));
  uint32_t offset = 2;
  for (uint32_t i = 0; i < f.partitions; ++i) {
    const uint32_t cls = f.part_class[i], dim = f.class_dim[cls], cbits = f.class_sub[cls], mask = (1u << cbits) - 1;
    int32_t cval = 0;
    if (cbits) {
      cval = decode_scalar(b, books_[f.class_master[cls]]);
      if (cval < 0) return false;
    }
    for (uint32_t j = 0; j < dim; ++j) {
      const int32_t book = f.class_books[cls][cval & mask];
      cval >>= cbits;
      if (book >= 0) {
        const int32_t v = decode_scalar(b, books_[book]);
        if (v < 0) return false;
        y[offset + j] = v;
      } else {
        y[offset + j] = 0;
      }
    }
    offset += dim;
  }
  if (b.eop) return false;
  // Reconstruct the posts: each is a prediction from its two neighbours corrected by the coded value.
  int32_t final_y[256];
  bool step2[256];
  final_y[0] = y[0];
  final_y[1] = y[1];
  step2[0] = step2[1] = true;
  for (uint32_t i = 2; i < f.count; ++i) {
    const int32_t lo = f.low[i], hi = f.high[i];
    const int32_t dy = final_y[hi] - final_y[lo], adx = f.x[hi] - f.x[lo], ady = dy < 0 ? -dy : dy;
    const int32_t off = (ady * (f.x[i] - f.x[lo])) / adx;
    const int32_t predicted = dy < 0 ? final_y[lo] - off : final_y[lo] + off;
    const int32_t val = y[i], highroom = static_cast<int32_t>(range) - predicted, lowroom = predicted;
    const int32_t room = (highroom < lowroom ? highroom : lowroom) * 2;
    if (val) {
      step2[lo] = step2[hi] = step2[i] = true;
      if (val >= room) final_y[i] = highroom > lowroom ? val - lowroom + predicted : predicted - val + highroom - 1;
      else final_y[i] = (val & 1) ? predicted - ((val + 1) >> 1) : predicted + (val >> 1);
    } else {
      step2[i] = false;
      final_y[i] = predicted;
    }
  }
  // Draw straight lines between the posts that matter.
  auto render_line = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const int32_t dy = y1 - y0, adx = x1 - x0, base = dy / adx;
    int32_t ady = (dy < 0 ? -dy : dy) - (base < 0 ? -base : base) * adx;
    const int32_t sy = dy < 0 ? base - 1 : base + 1;
    int32_t yy = y0, err = 0;
    auto put = [&](int32_t x, int32_t v) {
      if (x >= 0 && static_cast<uint32_t>(x) < half) {
        int32_t i = v < 0 ? 0 : v > 255 ? 255 : v;
        out[x] = g_inverse_db[i];
      }
    };
    put(x0, yy);
    for (int32_t x = x0 + 1; x < x1; ++x) {
      err += ady;
      if (err >= adx) { err -= adx; yy += sy; } else { yy += base; }
      put(x, yy);
    }
  };
  int32_t hx = 0, lx = 0;
  int32_t ly = final_y[f.sorted[0]] * static_cast<int32_t>(f.multiplier), hy = ly;
  for (uint32_t i = 1; i < f.count; ++i) {
    const uint32_t idx = f.sorted[i];
    if (!step2[idx]) continue;
    hy = final_y[idx] * static_cast<int32_t>(f.multiplier);
    hx = f.x[idx];
    render_line(lx, ly, hx, hy);
    lx = hx;
    ly = hy;
  }
  for (int32_t x = hx; x >= 0 && static_cast<uint32_t>(x) < half; ++x) {
    const int32_t v = hy < 0 ? 0 : hy > 255 ? 255 : hy;
    out[x] = g_inverse_db[v];
  }
  return true;
}

// Decodes the residue vectors of the channels in `vec` (`nch` of them, `size` samples each) by adding to them; `skip[c]` marks
// channels with nothing to decode. Running out of packet simply leaves the rest as it is.
void Decoder::decode_residue(Bits& b, const Residue& r, uint32_t size, uint32_t nch, float** vec, const bool* skip) {
  const uint32_t begin = r.begin < size ? r.begin : size, end = r.end < size ? r.end : size;
  if (end <= begin) return;
  const uint32_t to_read = (end - begin) / r.partition_size;
  if (to_read == 0) return;
  const Codebook& classbook = books_[r.classbook];
  const uint32_t per_word = classbook.dims;
  const uint64_t need = uint64_t{nch} * (to_read + per_word);
  if (need > classes_size_) {
    allocator_.deallocate(classes_);
    classes_ = static_cast<uint8_t*>(allocator_.allocate(need));
    classes_size_ = classes_ ? need : 0;
    if (!classes_) return;
  }
  const uint64_t stride = to_read + per_word;
  for (uint32_t pass = 0; pass < 8; ++pass) {
    uint32_t part = 0;
    while (part < to_read) {
      if (pass == 0)
        for (uint32_t c = 0; c < nch; ++c) {
          if (skip[c]) continue;
          int32_t temp = decode_scalar(b, classbook);
          if (temp < 0) return;
          for (int32_t i = static_cast<int32_t>(per_word) - 1; i >= 0; --i) {
            classes_[c * stride + part + static_cast<uint32_t>(i)] = static_cast<uint8_t>(temp % static_cast<int32_t>(r.classifications));
            temp /= static_cast<int32_t>(r.classifications);
          }
        }
      for (uint32_t i = 0; i < per_word && part < to_read; ++i, ++part) {
        for (uint32_t c = 0; c < nch; ++c) {
          if (skip[c]) continue;
          const int32_t book = r.books[classes_[c * stride + part]][pass];
          if (book < 0) continue;
          const Codebook& cb = books_[book];
          const uint32_t offset = begin + part * r.partition_size;
          float* v = vec[c];
          if (r.type == 0) {
            const uint32_t step = r.partition_size / cb.dims;
            for (uint32_t k = 0; k < step; ++k) {
              const int32_t e = decode_scalar(b, cb);
              if (e < 0) return;
              const float* q = cb.vq + uint64_t{static_cast<uint32_t>(e)} * cb.dims;
              for (uint32_t j = 0; j < cb.dims; ++j) {
                const uint64_t at = uint64_t{offset} + k + uint64_t{j} * step;
                if (at < size) v[at] += q[j];
              }
            }
          } else {
            for (uint32_t k = 0; k < r.partition_size; k += cb.dims) {
              const int32_t e = decode_scalar(b, cb);
              if (e < 0) return;
              const float* q = cb.vq + uint64_t{static_cast<uint32_t>(e)} * cb.dims;
              for (uint32_t j = 0; j < cb.dims; ++j) {
                const uint64_t at = uint64_t{offset} + k + j;
                if (at < size) v[at] += q[j];
              }
            }
          }
        }
      }
    }
  }
}

bool Decoder::audio_packet(Bits& b) {
  if (b.read(1) != 0) return true;  // not an audio packet: ignore
  const uint32_t mode_index = b.read(ilog(mode_count_ - 1));
  if (b.eop || mode_index >= mode_count_) return true;
  const Mode& mode = modes_[mode_index];
  const uint32_t n = blocksize_[mode.blockflag], half = n / 2;
  bool prev_long = false, next_long = false;
  if (mode.blockflag) {
    prev_long = b.read(1);
    next_long = b.read(1);
  }
  const Mapping& map = mappings_[mode.mapping];
  bool unused[kMaxChannels];
  for (uint32_t c = 0; c < channels_; ++c) {
    for (uint32_t i = 0; i < half; ++i) { floor_[c][i] = 0; spectrum_[c][i] = 0; }
    const Floor1& f = floors_[map.floor[map.mux[c]]];
    unused[c] = !decode_floor(b, f, floor_[c], half);
  }
  for (uint32_t s = 0; s < map.coupling_steps; ++s)
    if (!unused[map.magnitude[s]] || !unused[map.angle[s]]) unused[map.magnitude[s]] = unused[map.angle[s]] = false;
  // Residues, submap by submap.
  for (uint32_t s = 0; s < map.submaps; ++s) {
    float* vec[kMaxChannels];
    bool skip[kMaxChannels];
    uint32_t count = 0;
    for (uint32_t c = 0; c < channels_; ++c)
      if (map.mux[c] == s) { vec[count] = spectrum_[c]; skip[count] = unused[c]; ++count; }
    if (count == 0) continue;
    const Residue& r = residues_[map.residue[s]];
    if (r.type == 2) {
      bool any = false;
      for (uint32_t i = 0; i < count; ++i) any = any || !skip[i];
      if (!any) continue;
      // One long vector with the channels interleaved sample by sample.
      float* joint = time_;  // scratch of max_n floats; count * half may be larger, so use the heap block when needed
      const uint64_t total = uint64_t{count} * half;
      float* heap = nullptr;
      if (total > blocksize_[1]) {
        heap = static_cast<float*>(allocator_.allocate(total * sizeof(float)));
        if (!heap) return false;
        joint = heap;
      }
      for (uint64_t i = 0; i < total; ++i) joint[i] = 0;
      bool no_skip = false;
      float* one[1] = {joint};
      decode_residue(b, r, static_cast<uint32_t>(total), 1, one, &no_skip);
      for (uint32_t i = 0; i < count; ++i)
        for (uint32_t k = 0; k < half; ++k) vec[i][k] = joint[uint64_t{k} * count + i];
      allocator_.deallocate(heap);
    } else {
      decode_residue(b, r, half, count, vec, skip);
    }
  }
  // Undo the channel coupling, last step first.
  for (uint32_t s = map.coupling_steps; s-- > 0;) {
    float* m = spectrum_[map.magnitude[s]];
    float* a = spectrum_[map.angle[s]];
    for (uint32_t i = 0; i < half; ++i) {
      const float M = m[i], A = a[i];
      if (M > 0) {
        if (A > 0) { a[i] = M - A; } else { a[i] = M; m[i] = M + A; }
      } else {
        if (A > 0) { a[i] = M + A; } else { a[i] = M; m[i] = M - A; }
      }
    }
  }
  // Window shape for this block given its neighbours.
  const uint32_t short_half = blocksize_[0] / 2;
  uint32_t left_start = 0, left_len = half, right_start = half, right_len = half;
  if (mode.blockflag && !prev_long) { left_start = n / 4 - blocksize_[0] / 4; left_len = short_half; }
  if (mode.blockflag && !next_long) { right_start = n * 3 / 4 - blocksize_[0] / 4; right_len = short_half; }
  const MdctPlan& plan = plans_[mode.blockflag];
  const float* left_slope = (left_len == half ? plan : plans_[0]).window_slope;
  const float* right_slope = (right_len == half ? plan : plans_[0]).window_slope;
  for (uint32_t i = 0; i < n; ++i) {
    float w;
    if (i < left_start) w = 0;
    else if (i < left_start + left_len) w = left_slope[i - left_start];
    else if (i < right_start) w = 1;
    else if (i < right_start + right_len) w = right_slope[right_len - 1 - (i - right_start)];
    else w = 0;
    window_[i] = w;
  }
  // Overlap-add, then give out the samples that are now complete.
  const uint32_t emit = have_previous_ ? previous_n_ / 4 + n / 4 : 0;
  const int32_t start = have_previous_ ? static_cast<int32_t>(previous_n_ / 4) - static_cast<int32_t>(n / 4) : 0;
  const uint64_t pcm_mark = pcm_.size();
  if (emit && !pcm_.put_zeros(uint64_t{emit} * out_channels_ * 2)) return false;
  for (uint32_t c = 0; c < channels_; ++c) {
    float* acc = overlap_[c];
    if (!unused[c]) {
      for (uint32_t i = 0; i < half; ++i) spectrum_[c][i] *= floor_[c][i];
      (mode.blockflag ? mdct1_ : mdct0_).inverse(spectrum_[c], time_);
      if (!have_previous_) {
        for (uint32_t i = 0; i < half; ++i) acc[i] = time_[half + i] * window_[half + i];
      } else {
        for (uint32_t i = 0; i < n; ++i) {
          const int32_t rel = start + static_cast<int32_t>(i);
          if (rel >= 0) acc[rel] += time_[i] * window_[i];
        }
      }
    } else if (!have_previous_) {
      for (uint32_t i = 0; i < half; ++i) acc[i] = 0;
    }
    if (emit) {
      for (uint32_t o = 0; o < out_channels_; ++o) {
        if (c != front_[o]) continue;
        int16_t* dst = reinterpret_cast<int16_t*>(const_cast<uint8_t*>(pcm_.view().data()) + pcm_mark);
        for (uint32_t i = 0; i < emit; ++i) {
          float v = acc[i] * kScale;
          v = v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v;
          const int32_t iv = static_cast<int32_t>(v >= 0 ? v + 0.5f : v - 0.5f);
          dst[uint64_t{i} * out_channels_ + o] = static_cast<int16_t>(iv);
        }
      }
      // What is left (the right half of this block) moves to the front; everything behind it is cleared.
      const uint32_t extent = static_cast<uint32_t>(start + static_cast<int32_t>(n));
      for (uint32_t i = 0; i < half; ++i) acc[i] = acc[i + emit];
      for (uint32_t i = half; i < extent; ++i) acc[i] = 0;
    }
  }
  if (emit) frames_out_ += emit;
  have_previous_ = true;
  previous_n_ = n;
  return true;
}

bool Decoder::on_packet(ConstByteView packet) {
  Bits b{packet};
  switch (state_) {
    case 0:
      if (packet.size() < 7 || b.read(8) != 1 || b.read(8) != 'v') return false;
      for (const char* p = "orbis"; *p; ++p)
        if (b.read(8) != static_cast<uint8_t>(*p)) return false;
      if (!read_identification(b)) return false;
      state_ = 1;
      return true;
    case 1:
      if (packet.size() < 7 || packet[0] != 3) return false;
      state_ = 2;
      return true;
    case 2:
      if (packet.size() < 7 || b.read(8) != 5) return false;
      for (const char* p = "vorbis"; *p; ++p)
        if (b.read(8) != static_cast<uint8_t>(*p)) return false;
      if (!read_setup(b)) return false;
      state_ = 3;
      return true;
    default:
      return audio_packet(b);
  }
}

uint32_t le32(ConstByteView d, uint64_t at) { return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (uint32_t{d[at + 3]} << 24); }

bool Decoder::decode(ConstByteView data, AudioData* out) {
  uint64_t at = 0;
  uint32_t serial = 0;
  bool have_serial = false;
  int64_t last_granule = -1;
  ByteBuffer packet(allocator_);
  while (at + 27 <= data.size()) {
    if (data[at] != 'O' || data[at + 1] != 'g' || data[at + 2] != 'g' || data[at + 3] != 'S') {
      // Lost sync: look for the next page.
      ++at;
      continue;
    }
    const uint32_t segments = data[at + 26];
    if (at + 27 + segments > data.size()) break;
    const uint32_t page_serial = le32(data, at + 14);
    uint64_t body = at + 27 + segments, total = 0;
    for (uint32_t s = 0; s < segments; ++s) total += data[at + 27 + s];
    if (body + total > data.size()) break;
    if (!have_serial) { serial = page_serial; have_serial = true; }
    if (page_serial == serial) {
      uint64_t granule = le32(data, at + 6) | (uint64_t{le32(data, at + 10)} << 32);
      if (granule != 0xFFFFFFFFFFFFFFFFull) last_granule = static_cast<int64_t>(granule);
      uint64_t p = body;
      for (uint32_t s = 0; s < segments; ++s) {
        const uint32_t len = data[at + 27 + s];
        if (!packet.append(data.subview(p, len))) return false;
        p += len;
        if (len < 255) {
          if (!on_packet(packet.view())) return false;
          packet.clear();
        }
      }
      if (data[at + 5] & 4) break;  // end of stream
    }
    at = body + total;
  }
  if (state_ != 3 || frames_out_ == 0) return false;
  uint64_t frames = frames_out_;
  if (last_granule > 0 && static_cast<uint64_t>(last_granule) < frames) frames = static_cast<uint64_t>(last_granule);
  audio::StreamFormat format;
  format.sample_rate = rate_;
  format.channels = static_cast<uint8_t>(out_channels_);
  if (!out->create(format, frames)) return false;
  __builtin_memcpy(out->samples().data(), pcm_.view().data(), frames * out_channels_ * 2);
  return true;
}
}  // namespace

bool decode_vorbis(ConstByteView data, AudioData* out) {
  out->clear();
  if (detect_audio_format(data) != AudioFileFormat::Vorbis) return false;
  Decoder decoder(out->allocator());
  if (decoder.decode(data, out)) return true;
  out->clear();
  return false;
}

}  // namespace media
