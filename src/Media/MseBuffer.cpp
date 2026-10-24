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

// A SourceBuffer's frames: the coded frame processing of the Media Source Extensions, the buffered ranges, and the files written for the players.
#include "Media/OggWriter.hpp"
#include "Media/Mse.hpp"
#include "Crypto/Crypto.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint32_t fourcc(const char* s) { return (uint32_t{static_cast<uint8_t>(s[0])} << 24) | (uint32_t{static_cast<uint8_t>(s[1])} << 16) | (uint32_t{static_cast<uint8_t>(s[2])} << 8) | static_cast<uint8_t>(s[3]); }

constexpr int64_t kMaxTime = int64_t{1} << 60;
constexpr int64_t kReorder = 10000000;   // how far a frame's decode time can be from its presentation time (10 s), for finding the frames near a time

// Microseconds as ticks of 1/timescale seconds, rounded to the nearest.
int64_t us_to_ticks(int64_t us, uint32_t timescale) {
  const bool negative = us < 0;
  const uint64_t m = negative ? uint64_t{0} - static_cast<uint64_t>(us) : static_cast<uint64_t>(us);
  const uint64_t ticks = (m / 1000000u) * timescale + ((m % 1000000u) * timescale + 500000u) / 1000000u;
  return negative ? -static_cast<int64_t>(ticks) : static_cast<int64_t>(ticks);
}

// Writes the boxes of a file into a ByteBuffer; a failure to get memory is remembered.
struct Out {
  explicit Out(ByteBuffer& buffer) : b(buffer) {}
  ByteBuffer& b;
  bool ok = true;
  void u8(uint32_t v) { ok = b.put8(static_cast<uint8_t>(v)) && ok; }
  void u16(uint32_t v) { u8(v >> 8); u8(v); }
  void u32(uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
  void tag(const char* t) { u32(fourcc(t)); }
  void zeros(uint64_t n) { ok = b.put_zeros(n) && ok; }
  void bytes(ConstByteView v) { ok = b.append(v) && ok; }
  uint64_t begin(const char* type) {
    const uint64_t at = b.size();
    u32(0);
    tag(type);
    return at;
  }
  void full(const char* type, uint32_t version_flags) {   // (the caller ends it)
    tag(type);
    u32(version_flags);
  }
  uint64_t begin_full(const char* type, uint32_t version_flags) {
    const uint64_t at = begin(type);
    u32(version_flags);
    return at;
  }
  void end(uint64_t at) {
    const uint64_t size = b.size() - at;
    const bool patched = b.patch8(at, static_cast<uint8_t>(size >> 24)) && b.patch8(at + 1, static_cast<uint8_t>(size >> 16)) && b.patch8(at + 2, static_cast<uint8_t>(size >> 8)) &&
                         b.patch8(at + 3, static_cast<uint8_t>(size));
    ok = ok && patched;
  }
  void patch32(uint64_t at, uint32_t v) {
    const bool patched = b.patch8(at, static_cast<uint8_t>(v >> 24)) && b.patch8(at + 1, static_cast<uint8_t>(v >> 16)) && b.patch8(at + 2, static_cast<uint8_t>(v >> 8)) &&
                         b.patch8(at + 3, static_cast<uint8_t>(v));
    ok = ok && patched;
  }
};

// Heap sort of ranges by start (no recursion, and no worst case worse than n log n whatever order frames came in).
void sort_ranges(ArrayView<MseBuffer::Range> a) {
  const uint64_t n = a.size();
  auto less = [&](uint64_t x, uint64_t y) { return a[x].start < a[y].start; };
  auto sift = [&](uint64_t root, uint64_t end) {
    for (;;) {
      uint64_t child = root * 2 + 1;
      if (child >= end) return;
      if (child + 1 < end && less(child, child + 1)) ++child;
      if (!less(root, child)) return;
      const MseBuffer::Range t = a[root];
      a[root] = a[child];
      a[child] = t;
      root = child;
    }
  };
  for (uint64_t i = n / 2; i > 0; --i) sift(i - 1, n);
  for (uint64_t end = n; end > 1; --end) {
    const MseBuffer::Range t = a[0];
    a[0] = a[end - 1];
    a[end - 1] = t;
    sift(0, end - 1);
  }
}

}  // namespace

// The protection record of an encrypted frame (kept beside the frame's bytes): scheme, key identifier, IV, pattern, subsamples.
constexpr uint32_t kAuxHeader = 40;

bool put_aux(ByteBuffer* b, const MseSampleCrypto& c) {
  uint8_t head[kAuxHeader] = {};
  head[0] = static_cast<uint8_t>(c.scheme);
  for (int i = 0; i < 16; ++i) {
    head[1 + i] = c.kid[i];
    head[17 + i] = c.iv[i];
  }
  head[33] = c.crypt_blocks;
  head[34] = c.skip_blocks;
  head[36] = static_cast<uint8_t>(c.subsample_count >> 24);
  head[37] = static_cast<uint8_t>(c.subsample_count >> 16);
  head[38] = static_cast<uint8_t>(c.subsample_count >> 8);
  head[39] = static_cast<uint8_t>(c.subsample_count);
  return b->append(ConstByteView(head, kAuxHeader)) && b->append(c.subsamples);
}

struct CryptoRecord {
  MseScheme scheme = MseScheme::None;
  uint8_t kid[16] = {};
  uint8_t iv[16] = {};
  uint8_t crypt = 0, skip = 0;
  uint32_t count = 0;
  ConstByteView subsamples;
  uint64_t size = 0;   // bytes the record takes
};

bool get_aux(ConstByteView all, uint32_t aux, CryptoRecord* r) {
  if (aux == 0) return false;
  ConstByteView rec;
  if (!all.try_subview(aux - 1, kAuxHeader, &rec)) return false;
  r->scheme = static_cast<MseScheme>(rec[0]);
  for (int i = 0; i < 16; ++i) {
    r->kid[i] = rec[1 + i];
    r->iv[i] = rec[17 + i];
  }
  r->crypt = rec[33];
  r->skip = rec[34];
  r->count = (uint32_t{rec[36]} << 24) | (uint32_t{rec[37]} << 16) | (uint32_t{rec[38]} << 8) | rec[39];
  if (r->count > 0xFFFF || !all.try_subview(uint64_t{aux} - 1 + kAuxHeader, uint64_t{r->count} * 6, &r->subsamples)) return false;
  r->size = kAuxHeader + uint64_t{r->count} * 6;
  return true;
}

void increment_counter(uint8_t counter[16]) {
  for (int i = 15; i >= 0; --i)
    if (++counter[i] != 0) break;
}

// Decrypts one sample in place, as its scheme says (ISO/IEC 23001-7): the byte ranges of the subsamples that are protected, and in them
//   cenc  AES-CTR over every byte, the keystream running on from one range to the next
//   cens  AES-CTR over the blocks the pattern (crypt, skip) picks, a partial block at the end of a range left alone
//   cbc1  AES-CBC over the whole blocks, the chain running on from one range to the next
//   cbcs  AES-CBC over the blocks the pattern picks, the chain starting again from the IV at every range
void decrypt_sample(const crypto::Aes& aes, const CryptoRecord& r, ByteView data) {
  struct Range {
    uint64_t start, length;
  };
  // the protected ranges
  uint8_t counter[16];
  for (int i = 0; i < 16; ++i) counter[i] = r.iv[i];
  uint8_t stream[16];
  uint32_t used = 16;   // bytes of `stream` taken
  uint8_t chain[16];
  for (int i = 0; i < 16; ++i) chain[i] = r.iv[i];
  const auto each_range = [&](auto&& fn) {
    if (r.count == 0) {
      fn(Range{0, data.size()});
      return;
    }
    ByteReader u(r.subsamples);
    uint64_t at = 0;
    for (uint32_t k = 0; k < r.count; ++k) {
      const uint64_t clear = u.be16();
      const uint64_t protected_bytes = u.be32();
      at += clear;
      if (!u.ok() || at > data.size() || protected_bytes > data.size() - at) return;
      if (protected_bytes) fn(Range{at, protected_bytes});
      at += protected_bytes;
    }
  };
  // the runs of whole blocks a pattern picks inside a range: calls fn(start, blocks)
  const auto each_run = [&](const Range& range, auto&& fn) {
    const uint64_t blocks = range.length / 16;
    if (r.crypt == 0 && r.skip == 0) {
      if (blocks) fn(range.start, blocks);
      return;
    }
    if (r.crypt == 0) return;
    for (uint64_t b = 0; b < blocks; b += uint64_t{r.crypt} + r.skip) {
      const uint64_t n = blocks - b < r.crypt ? blocks - b : r.crypt;
      fn(range.start + b * 16, n);
    }
  };
  switch (r.scheme) {
    case MseScheme::Cenc:
      each_range([&](const Range& range) {
        for (uint64_t i = 0; i < range.length; ++i) {
          if (used == 16) {
            aes.encrypt_block(counter, stream);
            increment_counter(counter);
            used = 0;
          }
          data[range.start + i] ^= stream[used++];
        }
      });
      break;
    case MseScheme::Cens:
      each_range([&](const Range& range) {
        each_run(range, [&](uint64_t start, uint64_t blocks) {
          for (uint64_t b = 0; b < blocks; ++b) {
            aes.encrypt_block(counter, stream);
            increment_counter(counter);
            for (int i = 0; i < 16; ++i) data[start + b * 16 + i] ^= stream[i];
          }
        });
      });
      break;
    case MseScheme::Cbc1:
      each_range([&](const Range& range) {
        const uint64_t whole = range.length & ~uint64_t{15};
        if (!whole) return;
        uint8_t last[16];
        for (int i = 0; i < 16; ++i) last[i] = data[range.start + whole - 16 + i];
        aes.cbc(chain, data.subview(range.start, whole), true);
        for (int i = 0; i < 16; ++i) chain[i] = last[i];
      });
      break;
    case MseScheme::Cbcs:
      each_range([&](const Range& range) {
        for (int i = 0; i < 16; ++i) chain[i] = r.iv[i];
        each_run(range, [&](uint64_t start, uint64_t blocks) {
          uint8_t last[16];
          for (int i = 0; i < 16; ++i) last[i] = data[start + blocks * 16 - 16 + i];
          aes.cbc(chain, data.subview(start, blocks * 16), true);
          for (int i = 0; i < 16; ++i) chain[i] = last[i];
        });
      });
      break;
    default: break;
  }
}

// A key identifier as base64url text without padding (what the "keyids" init data of EME uses).
void base64url(const uint8_t* bytes, uint32_t n, ByteBuffer* out) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (uint32_t i = 0; i < n; i += 3) {
    const uint32_t b0 = bytes[i], b1 = i + 1 < n ? bytes[i + 1] : 0, b2 = i + 2 < n ? bytes[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out->put8(static_cast<uint8_t>(kAlphabet[(v >> 18) & 63]));
    out->put8(static_cast<uint8_t>(kAlphabet[(v >> 12) & 63]));
    if (i + 1 < n) out->put8(static_cast<uint8_t>(kAlphabet[(v >> 6) & 63]));
    if (i + 2 < n) out->put8(static_cast<uint8_t>(kAlphabet[v & 63]));
  }
}

// --- MseInit ------------------------------------------------------------------------------------------------------------------------------

ConstByteView MseInit::entry(const MseTrackInfo& track) const {
  ConstByteView all = entries.view();
  ConstByteView out;
  return all.try_subview(track.entry_offset, track.entry_size, &out) ? out : ConstByteView();
}

bool MseInit::same_as(const MseInit& other) const {
  if (tracks.count() != other.tracks.count()) return false;
  for (uint64_t i = 0; i < tracks.count(); ++i) {
    const MseTrackInfo& a = tracks[i];
    const MseTrackInfo& b = other.tracks[i];
    if (a.id != b.id || a.kind != b.kind || a.codec != b.codec || a.timescale != b.timescale || a.width != b.width || a.height != b.height || a.channels != b.channels ||
        a.sample_rate != b.sample_rate)
      return false;
    const ConstByteView ea = entry(a), eb = other.entry(b);
    if (ea.size() != eb.size() || (ea.size() && __builtin_memcmp(ea.data(), eb.data(), ea.size()) != 0)) return false;
    if (__builtin_memcmp(&a.protection, &b.protection, sizeof(MseProtection)) != 0) return false;
  }
  const ConstByteView pa = pssh.view(), pb = other.pssh.view();
  return pa.size() == pb.size() && (pa.size() == 0 || __builtin_memcmp(pa.data(), pb.data(), pa.size()) == 0);
}

const MseTrackInfo* MseInit::first_of(MseTrackKind kind) const {
  for (uint64_t i = 0; i < tracks.count(); ++i)
    if (tracks[i].kind == kind) return &tracks[i];
  return nullptr;
}

// --- The frames of a track, in decode order ----------------------------------------------------------------------------------------------------

class MseBuffer::FrameList {
 public:
  explicit FrameList(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~FrameList() { release_view(allocator_, storage_); }
  FrameList(const FrameList&) = delete;
  FrameList& operator=(const FrameList&) = delete;

  uint64_t size() const { return size_; }
  Frame& operator[](uint64_t i) { return storage_.first(size_)[i]; }
  const Frame& operator[](uint64_t i) const { return storage_.first(size_)[i]; }
  ArrayView<const Frame> view() const { return ArrayView<const Frame>(storage_).first(size_); }

  bool insert(uint64_t index, const Frame& f) {
    if (index > size_) return false;
    if (size_ == storage_.size()) {
      const uint64_t capacity = storage_.empty() ? 256 : storage_.size() * 2;
      ArrayView<Frame> bigger = allocate_view<Frame>(allocator_, capacity);
      if (bigger.empty()) return false;
      if (size_) bigger.first(size_).copy_from(ArrayView<const Frame>(storage_).first(size_));
      release_view(allocator_, storage_);
      storage_ = bigger;
    }
    if (index < size_) storage_.subview(index + 1, size_ - index).copy_from(ArrayView<const Frame>(storage_).subview(index, size_ - index));
    storage_[index] = f;
    ++size_;
    return true;
  }
  void erase(uint64_t first, uint64_t last) {
    if (first >= last || last > size_) return;
    if (last < size_) storage_.subview(first, size_ - last).copy_from(ArrayView<const Frame>(storage_).subview(last, size_ - last));
    size_ -= last - first;
  }
  void shrink_to(uint64_t n) {
    if (n < size_) size_ = n;
  }
  // The first index whose decode time is above `dts`.
  uint64_t upper_bound_dts(int64_t dts) const {
    uint64_t lo = 0, hi = size_;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      if (storage_[mid].dts <= dts) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }
  uint64_t lower_bound_dts(int64_t dts) const {
    uint64_t lo = 0, hi = size_;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      if (storage_[mid].dts < dts) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<Frame> storage_;
  uint64_t size_ = 0;
};

struct MseBuffer::Slot {
  Slot(allocators::IAllocator& allocator, const MseTrackInfo& info) : frames(allocator), kind(info.kind), id(info.id) {}
  FrameList frames;
  MseTrackKind kind;
  uint32_t id;
  bool has_last = false, need_ra = true, max_dirty = false;
  int64_t last_dts = 0, last_dur = 0;
  int64_t max_end = INT64_MIN;   // the end of the frame that ends last (INT64_MIN: none)
};

// --- The buffer ---------------------------------------------------------------------------------------------------------------------------

MseBuffer::MseBuffer(allocators::IAllocator& allocator, MseFormat format, uint32_t id)
    : allocator_(allocator), format_(format), id_(id), parser_(make_mse_parser(allocator, format)), inits_(allocator), arena_(UniquePointer<ByteBuffer>::make(allocator, allocator)), aux_(UniquePointer<ByteBuffer>::make(allocator, allocator)), encrypted_(allocator), sent_(allocator),
      range_cache_(allocator) {
  for (int8_t& m : slot_map_) m = -1;
  if (format == MseFormat::Mpeg || format == MseFormat::Adts) mode_ = Mode::Sequence;   // (the formats without timestamps of their own are always in sequence mode)
}

MseBuffer::~MseBuffer() = default;

bool MseBuffer::set_mode(Mode mode) {
  if ((format_ == MseFormat::Mpeg || format_ == MseFormat::Adts) && mode == Mode::Segments) return false;
  if (mode == Mode::Sequence && mode_ != Mode::Sequence) group_start_pending_ = true;
  mode_ = mode;
  return true;
}

void MseBuffer::set_timestamp_offset(int64_t offset_us) {
  timestamp_offset_ = offset_us;
  if (mode_ == Mode::Sequence) {
    group_end_ = offset_us;
    group_start_pending_ = true;
  }
}

bool MseBuffer::change_format(MseFormat format) {
  if (format == format_) return true;
  // Another byte stream format: the parser changes, the frames and the init segments stay.
  const auto has_init = [](MseFormat f) { return f == MseFormat::Mp4 || f == MseFormat::Webm; };
  if (has_init(format) != has_init(format_)) return false;
  UniquePointer<IMseParser> parser = make_mse_parser(allocator_, format);
  if (!parser) return false;
  parser_ = util::move(parser);
  format_ = format;
  reset_group();
  return true;
}

uint32_t MseBuffer::width() const {
  if (!have_init_ || inits_.count() == 0) return 0;
  const MseTrackInfo* t = inits_[current_generation_]->first_of(MseTrackKind::Video);
  return t ? t->width : 0;
}
uint32_t MseBuffer::height() const {
  if (!have_init_ || inits_.count() == 0) return 0;
  const MseTrackInfo* t = inits_[current_generation_]->first_of(MseTrackKind::Video);
  return t ? t->height : 0;
}
bool MseBuffer::has_video() const {
  if (!have_init_ || inits_.count() == 0) return false;
  return inits_[current_generation_]->first_of(MseTrackKind::Video) != nullptr;
}
bool MseBuffer::split_parts() const {
  if (!have_init_ || inits_.count() == 0) return false;
  bool video = false, other_sound = false;
  for (const MseTrackInfo& t : inits_[current_generation_]->tracks.view()) {
    if (t.kind == MseTrackKind::Video) video = true;
    else if (t.container == kContainerOpus || t.container == kContainerVorbis) other_sound = true;
  }
  return video && other_sound;
}

bool MseBuffer::track_info(uint32_t index, MseTrackKind* kind, uint32_t* id) const {
  if (index >= slot_count_) return false;
  *kind = slots_[index]->kind;
  *id = slots_[index]->id;
  return true;
}

void MseBuffer::reset_group() {
  for (uint32_t i = 0; i < slot_count_; ++i) {
    slots_[i]->has_last = false;
    slots_[i]->need_ra = true;
  }
}

void MseBuffer::abort() {
  parser_->reset();
  window_start_ = 0;
  window_end_ = INT64_MAX;
  reset_group();
  if (mode_ == Mode::Sequence) group_start_pending_ = true;
}

// --- Initialization segments -----------------------------------------------------------------------------------------------------------------

bool MseBuffer::init_segment(UniquePointer<MseInit> init) {
  if (!init || init->tracks.count() == 0 || init->tracks.count() > 8) return false;
  if (!have_init_) {
    for (uint64_t i = 0; i < init->tracks.count() && slot_count_ < kMaxSlots; ++i) {
      slots_[slot_count_] = UniquePointer<Slot>::make(allocator_, allocator_, init->tracks[i]);
      if (!slots_[slot_count_]) return false;
      ++slot_count_;
    }
  }
  // which slot each track of this segment feeds: the one with its id, else a free one of its kind
  bool taken[kMaxSlots] = {};
  slot_map_count_ = static_cast<uint32_t>(init->tracks.count());
  bool any = false;
  for (uint32_t i = 0; i < slot_map_count_; ++i) {
    slot_map_[i] = -1;
    for (uint32_t s = 0; s < slot_count_; ++s) {
      if (!taken[s] && slots_[s]->id == init->tracks[i].id && slots_[s]->kind == init->tracks[i].kind) {
        slot_map_[i] = static_cast<int8_t>(s);
        taken[s] = true;
        break;
      }
    }
  }
  for (uint32_t i = 0; i < slot_map_count_; ++i) {
    if (slot_map_[i] >= 0) continue;
    for (uint32_t s = 0; s < slot_count_; ++s) {
      if (!taken[s] && slots_[s]->kind == init->tracks[i].kind) {
        slot_map_[i] = static_cast<int8_t>(s);
        taken[s] = true;
        break;
      }
    }
  }
  for (uint32_t i = 0; i < slot_map_count_; ++i) any = any || slot_map_[i] >= 0;
  if (!any) return false;
  // the generation: an earlier init segment that says the same, or a new one
  uint64_t generation = inits_.count();
  for (uint64_t g = 0; g < inits_.count(); ++g) {
    if (inits_[g]->same_as(*init)) {
      generation = g;
      break;
    }
  }
  if (init->duration_us >= 0) init_duration_us_ = init->duration_us;
  if (generation == inits_.count()) {
    // protected content: the init data for an "encrypted" event is the 'pssh' boxes, or without them the key identifiers
    bool any_protected = false;
    for (const MseTrackInfo& t : init->tracks.view()) any_protected = any_protected || t.protection.is_protected;
    if (init->pssh.size() > 0) {
      queue_encrypted(0, init->pssh.view());
    } else if (any_protected && format_ == MseFormat::Webm) {   // a WebM stream names its keys: one event for each
      for (uint64_t ti = 0; ti < init->tracks.count(); ++ti) {
        const MseTrackInfo& t = init->tracks[ti];
        if (t.protection.is_protected) queue_encrypted(2, ConstByteView(t.protection.kid, 16));
      }
    } else if (any_protected) {
      ByteBuffer json(allocator_);
      json.append(ConstByteView(reinterpret_cast<const uint8_t*>("{\"kids\":["), 9));
      bool first = true;
      for (uint64_t ti = 0; ti < init->tracks.count(); ++ti) {
        const MseTrackInfo& t = init->tracks[ti];
        if (!t.protection.is_protected) continue;
        bool seen = false;   // (tracks that share a key announce it once)
        for (uint64_t earlier = 0; earlier < ti; ++earlier)
          if (init->tracks[earlier].protection.is_protected && __builtin_memcmp(init->tracks[earlier].protection.kid, t.protection.kid, 16) == 0) seen = true;
        if (seen) continue;
        if (!first) json.put8(',');
        first = false;
        json.put8('"');
        base64url(t.protection.kid, 16, &json);
        json.put8('"');
      }
      json.put8(']');
      json.put8('}');
      queue_encrypted(1, json.view());
    }
    if (inits_.count() >= 256) return false;
    if (!inits_.push_back(util::move(init))) return false;
  }
  current_generation_ = static_cast<uint32_t>(generation);
  have_init_ = true;
  // the frames that follow start a new coded frame group
  reset_group();
  if (mode_ == Mode::Sequence) group_start_pending_ = true;
  return true;
}

// --- Coded frame processing ---------------------------------------------------------------------------------------------------------------

bool MseBuffer::frame(uint32_t track, int64_t pts_us, int64_t dts_us, int64_t dur_us, bool sync, ConstByteView data, const MseSampleCrypto* crypto) {
  if (track >= slot_map_count_ || slot_map_[track] < 0) return true;   // (a track that is not kept)
  const uint32_t si = static_cast<uint32_t>(slot_map_[track]);
  Slot& slot = *slots_[si];
  if (dur_us < 0) dur_us = 0;
  if (pts_us > kMaxTime || pts_us < -kMaxTime || dts_us > kMaxTime || dts_us < -kMaxTime || dur_us > kMaxTime) return false;
  if (slot.kind == MseTrackKind::Audio) sync = true;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (mode_ == Mode::Sequence && group_start_pending_) {
      timestamp_offset_ = group_end_ - pts_us;
      group_start_pending_ = false;
    }
    if (timestamp_offset_ > kMaxTime || timestamp_offset_ < -kMaxTime) return false;
    const int64_t pts = pts_us + timestamp_offset_, dts = dts_us + timestamp_offset_;
    if (slot.has_last && (dts < slot.last_dts || dts - slot.last_dts > 2 * slot.last_dur + 1000)) {
      // a discontinuity: this is where a new group of frames starts
      if (mode_ == Mode::Segments) group_end_ = pts;
      else group_start_pending_ = true;
      reset_group();
      continue;
    }
    if (pts < window_start_ || pts + dur_us > window_end_) {   // outside the append window: dropped, and the next one has to be a random access point
      slot.need_ra = true;
      return true;
    }
    if (slot.need_ra) {
      if (!sync) return true;
      slot.need_ra = false;
    }
    if (!add_frame(si, pts, dts, dur_us, sync, data, crypto)) return false;
    slot.has_last = true;
    slot.last_dts = dts;
    slot.last_dur = dur_us;
    if (pts + dur_us > group_end_) group_end_ = pts + dur_us;
    if (pts + dur_us > highest_added_) highest_added_ = pts + dur_us;
    return true;
  }
  return true;
}

bool MseBuffer::add_frame(uint32_t si, int64_t pts, int64_t dts, int64_t duration, bool sync, ConstByteView data, const MseSampleCrypto* crypto) {
  Slot& slot = *slots_[si];
  if (data.size() > UINT32_MAX) return false;
  remove_overlap(si, pts, duration);
  const uint64_t offset = arena_->size();
  if (!arena_->append(data)) {
    memory_error_ = true;
    return false;
  }
  arena_used_ += data.size();
  Frame f;
  f.aux = 0;
  if (crypto) {
    const uint64_t at_aux = aux_->size();
    if (at_aux >= 0xFFFFFFF0u || !put_aux(aux_.get(), *crypto)) {
      memory_error_ = true;
      return false;
    }
    f.aux = static_cast<uint32_t>(at_aux + 1);
    ++protected_frames_;
  }
  f.pts = pts;
  f.dts = dts;
  f.duration = duration;
  f.offset = offset;
  f.size = static_cast<uint32_t>(data.size());
  f.generation = current_generation_;
  f.slot = static_cast<uint8_t>(si);
  f.sync = sync;
  const uint64_t at = slot.frames.upper_bound_dts(dts);
  if (!slot.frames.insert(at, f)) {
    memory_error_ = true;
    forget_aux(f);
    return false;
  }
  if (!slot.max_dirty && pts + duration > slot.max_end) slot.max_end = pts + duration;
  ++appended_;
  range_cache_valid_ = false;
  return true;
}

// Frames that start inside the new frame's time are replaced, and so are the ones after them that cannot be decoded without them.
void MseBuffer::remove_overlap(uint32_t si, int64_t pts, int64_t duration) {
  Slot& slot = *slots_[si];
  if (slot.frames.size() == 0) return;
  if (slot.max_dirty) {
    slot.max_end = INT64_MIN;
    for (const Frame& f : slot.frames.view())
      if (f.pts + f.duration > slot.max_end) slot.max_end = f.pts + f.duration;
    slot.max_dirty = false;
  }
  if (pts >= slot.max_end) return;   // nothing is at or after this time
  const int64_t end = pts + (duration > 0 ? duration : 1);
  uint64_t first = slot.frames.size();
  for (uint64_t i = slot.frames.lower_bound_dts(pts - kReorder); i < slot.frames.size(); ++i) {
    const Frame& f = slot.frames[i];
    if (f.dts >= end) break;
    if (f.pts >= pts && f.pts < end) {
      first = i;
      break;
    }
  }
  if (first == slot.frames.size()) return;
  uint64_t last = first;
  while (last < slot.frames.size()) {
    const Frame& f = slot.frames[last];
    const bool in_range = f.pts >= pts && f.pts < end;
    if (!in_range && f.sync) break;
    if (!in_range && last == first) break;
    ++last;
  }
  erase_frames(si, first, last);
}

void MseBuffer::erase_frames(uint32_t si, uint64_t first, uint64_t last) {
  Slot& slot = *slots_[si];
  int64_t from = INT64_MAX, to = INT64_MIN;
  for (uint64_t i = first; i < last; ++i) {
    const Frame& f = slot.frames[i];
    garbage_ += f.size;
    forget_aux(f);
    if (f.pts < from) from = f.pts;
    if (f.pts + f.duration > to) to = f.pts + f.duration;
  }
  slot.frames.erase(first, last);
  slot.max_dirty = true;
  range_cache_valid_ = false;
  if (from <= to) note_removed(from, to);
}

void MseBuffer::note_removed(int64_t from, int64_t to) {
  ++epoch_;
  if (change_count_ == 32) dropped_epoch_ = changes_[change_next_].epoch;
  changes_[change_next_] = Change{epoch_, from, to};
  change_next_ = (change_next_ + 1) % 32;
  if (change_count_ < 32) ++change_count_;
}

bool MseBuffer::changed_since(uint64_t epoch, int64_t from_us, int64_t to_us) const {
  if (epoch >= epoch_) return false;
  if (dropped_epoch_ > epoch) return true;   // (the record of it is gone: assume the worst)
  for (uint32_t i = 0; i < change_count_; ++i) {
    const Change& c = changes_[i];
    if (c.epoch > epoch && c.from < to_us && c.to > from_us) return true;
  }
  return false;
}

MseAppend MseBuffer::append(ConstByteView bytes, int64_t playhead_us) {
  if (!make_room(bytes.size(), playhead_us)) return MseAppend::Quota;
  memory_error_ = false;
  if (!parser_->append(bytes, *this)) {
    reset_group();
    if (mode_ == Mode::Sequence) group_start_pending_ = true;
    return memory_error_ ? MseAppend::Memory : MseAppend::Decode;
  }
  if ((garbage_ > (4u << 20) && garbage_ > arena_used_ / 2) || aux_garbage_ > (2u << 20)) compact();
  return MseAppend::Ok;
}

void MseBuffer::compact() {
  auto fresh = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  if (!fresh) return;
  // (the order frames were stored in does not matter: every frame knows where its bytes go)
  for (uint32_t s = 0; s < slot_count_; ++s) {
    FrameList& list = slots_[s]->frames;
    for (uint64_t i = 0; i < list.size(); ++i) {
      Frame& f = list[i];
      if (!fresh->append(arena_->view().subview(f.offset, f.size))) return;   // (nothing is changed until all have moved: offsets are patched below)
    }
  }
  // second pass: the offsets (the copy above kept the frames in the same order)
  uint64_t at = 0;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    FrameList& list = slots_[s]->frames;
    for (uint64_t i = 0; i < list.size(); ++i) {
      list[i].offset = at;
      at += list[i].size;
    }
  }
  arena_ = util::move(fresh);
  arena_used_ = at;
  garbage_ = 0;
  // the protection records of the frames that are left
  if (aux_garbage_ > 0 || protected_frames_ == 0) {
    auto records = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
    if (!records) return;
    for (uint32_t s = 0; s < slot_count_; ++s) {
      FrameList& list = slots_[s]->frames;
      for (uint64_t i = 0; i < list.size(); ++i) {
        CryptoRecord rec;
        if (list[i].aux == 0 || !get_aux(aux_->view(), list[i].aux, &rec)) continue;
        const uint64_t where = records->size();
        if (!records->append(aux_->view().subview(list[i].aux - 1, rec.size))) return;
        list[i].aux = static_cast<uint32_t>(where + 1);
      }
    }
    aux_ = util::move(records);
    aux_garbage_ = 0;
  }
}

void MseBuffer::forget_aux(const Frame& f) {
  CryptoRecord rec;
  if (f.aux == 0 || !get_aux(aux_->view(), f.aux, &rec)) return;
  aux_garbage_ += rec.size;
  if (protected_frames_ > 0) --protected_frames_;
}

void MseBuffer::queue_encrypted(uint8_t kind, ConstByteView data) {
  for (const UniquePointer<ByteBuffer>& earlier : sent_.view()) {
    const ConstByteView e = earlier->view();
    if (e.size() == data.size() + 1 && e[0] == kind && __builtin_memcmp(e.data() + 1, data.data(), data.size()) == 0) return;   // (announced already)
  }
  auto note = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  auto event = UniquePointer<EncryptedEvent>::make(allocator_, allocator_);
  if (!note || !event || !note->put8(kind) || !note->append(data) || !event->data.append(data)) return;
  event->keyids = kind == 1;
  event->kind = kind;
  if (sent_.count() >= 16) sent_.remove_at(0);
  sent_.push_back(util::move(note));
  if (encrypted_.count() < 32) encrypted_.push_back(util::move(event));
}

void MseBuffer::protection_data(ConstByteView pssh_boxes) { queue_encrypted(0, pssh_boxes); }

bool MseBuffer::take_encrypted(UniquePointer<EncryptedEvent>* out) {
  if (encrypted_.count() == 0) return false;
  *out = util::move(encrypted_[0]);
  encrypted_.remove_at(0);
  return true;
}

// --- Removing, evicting -----------------------------------------------------------------------------------------------------------------------

void MseBuffer::remove(int64_t start_us, int64_t end_us) {
  if (end_us <= start_us) return;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    Slot& slot = *slots_[s];
    FrameList& list = slot.frames;
    // the removal reaches on to the next random access point
    int64_t end = INT64_MAX;   // (with none, everything after it depended on what is removed)
    bool found = false;
    for (const Frame& f : list.view()) {
      if (f.sync && f.pts >= end_us && (!found || f.pts < end)) {
        end = f.pts;
        found = true;
      }
    }
    uint64_t kept = 0;
    int64_t from = INT64_MAX, to = INT64_MIN;
    const uint64_t n = list.size();
    for (uint64_t i = 0; i < n; ++i) {
      const Frame f = list[i];
      if (f.pts >= start_us && f.pts < end) {
        garbage_ += f.size;
        forget_aux(f);
        if (f.pts < from) from = f.pts;
        if (f.pts + f.duration > to) to = f.pts + f.duration;
      } else {
        if (kept != i) list[kept] = f;
        ++kept;
      }
    }
    if (kept != n) {
      list.shrink_to(kept);
      slot.max_dirty = true;
      range_cache_valid_ = false;
      note_removed(from, to);
    }
  }
  if (garbage_ > (4u << 20) && garbage_ > arena_used_ / 2) compact();
}

bool MseBuffer::make_room(uint64_t needed, int64_t playhead_us) {
  if (needed > kMaxBytes) return false;
  if (bytes() + needed <= kMaxBytes) return true;
  const uint64_t want = needed + kMaxBytes / 8;
  // The lead track decides where to cut: the first random access point after enough bytes, and never closer to the playhead than half a second.
  const int64_t limit = playhead_us - 500000;
  for (int round = 0; round < 8 && bytes() + needed > kMaxBytes; ++round) {
    const Slot* lead = nullptr;
    for (uint32_t s = 0; s < slot_count_; ++s) {
      if (slots_[s]->frames.size() == 0) continue;
      if (!lead || (slots_[s]->kind == MseTrackKind::Video && lead->kind != MseTrackKind::Video)) lead = slots_[s].get();
    }
    if (!lead) return false;
    int64_t earliest = INT64_MAX;
    for (const Frame& f : lead->frames.view())
      if (f.pts < earliest) earliest = f.pts;
    uint64_t sum = 0;
    int64_t cut = INT64_MIN;
    for (const Frame& f : lead->frames.view()) {
      sum += f.size;
      if (!f.sync || f.pts <= earliest || f.pts > limit) continue;
      cut = f.pts;
      if (sum >= want) break;
    }
    if (cut == INT64_MIN) return false;
    const uint64_t before = bytes();
    remove(earliest, cut);
    if (bytes() >= before) return false;
  }
  return bytes() + needed <= kMaxBytes;
}

// --- Ranges --------------------------------------------------------------------------------------------------------------------------------

void MseBuffer::slot_ranges(uint32_t s, List<Range>* out) const {
  out->clear();
  const FrameList& list = slots_[s]->frames;
  if (list.size() == 0) return;
  ArrayView<Range> raw = allocate_view<Range>(allocator_, list.size());
  if (raw.empty()) return;
  for (uint64_t i = 0; i < list.size(); ++i) raw[i] = Range{list[i].pts, list[i].pts + list[i].duration};
  sort_ranges(raw);
  Range current = raw[0];
  int64_t last_duration = raw[0].end - raw[0].start;
  for (uint64_t i = 1; i < raw.size(); ++i) {
    const int64_t duration = raw[i].end - raw[i].start;
    const int64_t tolerance = (duration > last_duration ? duration : last_duration) / 2 + 1000;
    if (raw[i].start <= current.end + tolerance) {
      if (raw[i].end > current.end) current.end = raw[i].end;
    } else {
      out->push_back(current);
      current = raw[i];
    }
    last_duration = duration;
  }
  out->push_back(current);
  release_view(allocator_, raw);
}

int64_t MseBuffer::highest_end() const {
  int64_t best = -1;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    const FrameList& list = slots_[s]->frames;
    for (const Frame& f : list.view())
      if (f.pts + f.duration > best) best = f.pts + f.duration;
  }
  return best;
}

void MseBuffer::buffered(List<Range>* out, int64_t reach) const {
  out->clear();
  const bool ended = reach >= 0;
  if (!ended && range_cache_valid_) {
    for (const Range& r : range_cache_.view()) out->push_back(r);
    return;
  }
  List<Range> acc(allocator_), mine(allocator_), merged(allocator_);
  bool first = true;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    if (slots_[s]->frames.size() == 0) return;   // a track without a frame: nothing can be played yet
    slot_ranges(s, &mine);
    if (ended && mine.count() > 0 && mine[mine.count() - 1].end < reach) mine[mine.count() - 1].end = reach;
    if (first) {
      acc.clear();
      for (const Range& r : mine.view()) acc.push_back(r);
      first = false;
      continue;
    }
    merged.clear();
    uint64_t a = 0, b = 0;
    while (a < acc.count() && b < mine.count()) {
      const int64_t lo = acc[a].start > mine[b].start ? acc[a].start : mine[b].start;
      const int64_t hi = acc[a].end < mine[b].end ? acc[a].end : mine[b].end;
      if (lo < hi) merged.push_back(Range{lo, hi});
      if (acc[a].end < mine[b].end) ++a;
      else ++b;
    }
    acc.clear();
    for (const Range& r : merged.view()) acc.push_back(r);
  }
  for (const Range& r : acc.view()) out->push_back(r);
  if (!ended) {
    range_cache_.clear();
    for (const Range& r : acc.view()) range_cache_.push_back(r);
    range_cache_valid_ = true;
  }
}

// --- Snapshots ------------------------------------------------------------------------------------------------------------------------------

bool MseBuffer::plan(int64_t t, MseSnapshotPlan* out) const { return plan_for(t, out, nullptr, nullptr) == MsePlan::Ok; }

MsePlan MseBuffer::plan_for(int64_t t, MseSnapshotPlan* out, const IMseKeys* keys, uint8_t* missing_kid) const {
  List<Range> ranges(allocator_);
  buffered(&ranges);
  const Range* found = nullptr;
  for (const Range& r : ranges.view()) {
    if (t >= r.start - 100000 && t < r.end) {
      found = &r;
      break;
    }
  }
  if (!found) return MsePlan::NoData;
  const Slot* lead = nullptr;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    if (slots_[s]->frames.size() == 0) continue;
    if (!lead || (slots_[s]->kind == MseTrackKind::Video && lead->kind != MseTrackKind::Video)) lead = slots_[s].get();
  }
  if (!lead) return MsePlan::NoData;
  const int64_t at = t < found->start ? found->start : t;
  // the generation of the picture at `at`: the frame that covers it, else the first one after
  uint32_t generation = 0;
  bool have_generation = false;
  int64_t next_pts = INT64_MAX;
  for (const Frame& f : lead->frames.view()) {
    if (f.pts <= at && at < f.pts + f.duration) {
      generation = f.generation;
      have_generation = true;
      break;
    }
    if (f.pts >= at && f.pts < next_pts) {
      next_pts = f.pts;
      generation = f.generation;
    }
  }
  if (!have_generation && next_pts == INT64_MAX) return MsePlan::NoData;
  int64_t start = found->start, end = found->end;
  if (inits_.count() > 1) {
    for (const Frame& f : lead->frames.view()) {
      if (f.generation == generation) continue;
      if (f.pts + f.duration <= at) {
        if (f.pts + f.duration > start) start = f.pts + f.duration;
      } else if (f.pts >= at) {
        if (f.pts < end) end = f.pts;
      }
    }
  }
  if (end <= start) return MsePlan::NoData;
  // a picture can be decoded from a key frame: start at the one the playhead is in (what comes before it is not needed, and may need keys that are not there)
  if (lead->kind == MseTrackKind::Video) {
    int64_t gop = INT64_MIN;
    for (const Frame& f : lead->frames.view())
      if (f.sync && f.generation == generation && f.pts <= at && f.pts >= start && f.pts > gop) gop = f.pts;
    if (gop != INT64_MIN) start = gop;
  }
  // encrypted frames: the plan reaches as far as there are keys
  if (protected_frames_ > 0) {
    int64_t cut = end;
    uint8_t first_missing[16] = {};
    bool any_missing = false;
    for (uint32_t s = 0; s < slot_count_; ++s) {
      for (const Frame& f : slots_[s]->frames.view()) {
        if (f.aux == 0 || f.pts < start || f.pts >= end || f.pts >= cut) continue;
        CryptoRecord rec;
        uint8_t key[16];
        if (!get_aux(aux_->view(), f.aux, &rec)) return MsePlan::NoData;
        if (keys && keys->key_for(rec.kid, key)) continue;
        cut = f.pts;
        any_missing = true;
        for (int k = 0; k < 16; ++k) first_missing[k] = rec.kid[k];
      }
    }
    if (any_missing) {
      if (missing_kid)
        for (int k = 0; k < 16; ++k) missing_kid[k] = first_missing[k];
      if (cut - start < 200000) return MsePlan::NeedKey;   // (the key for what is at the playhead is not there)
      end = cut;
    }
  }
  out->start_us = start;
  out->end_us = end;
  out->generation = generation;
  out->base_us = start;
  out->has_video = lead->kind == MseTrackKind::Video;
  out->width = out->height = 0;
  if (generation < inits_.count()) {
    const MseTrackInfo* v = inits_[generation]->first_of(MseTrackKind::Video);
    if (v) {
      out->width = v->width;
      out->height = v->height;
    }
  }
  return MsePlan::Ok;
}

MseDecrypt MseBuffer::decrypt(const IMseKeys& keys, const MseSnapshotPlan& plan, uint8_t* missing_kid) {
  if (protected_frames_ == 0) return MseDecrypt::Ok;
  crypto::Aes aes;
  uint8_t current_key[16] = {};
  bool have_key = false;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    FrameList& list = slots_[s]->frames;
    for (uint64_t i = 0; i < list.size(); ++i) {
      Frame& f = list[i];
      if (f.aux == 0 || f.pts < plan.start_us || f.pts >= plan.end_us) continue;
      CryptoRecord rec;
      uint8_t key[16];
      if (!get_aux(aux_->view(), f.aux, &rec)) return MseDecrypt::Failed;
      if (!keys.key_for(rec.kid, key)) {
        if (missing_kid)
          for (int k = 0; k < 16; ++k) missing_kid[k] = rec.kid[k];
        return MseDecrypt::NeedKey;
      }
      if (!have_key || __builtin_memcmp(current_key, key, 16) != 0) {
        if (!aes.set_key(ConstByteView(key, 16))) return MseDecrypt::Failed;
        for (int k = 0; k < 16; ++k) current_key[k] = key[k];
        have_key = true;
      }
      ByteView bytes = arena_->writable_view();
      if (f.offset > bytes.size() || f.size > bytes.size() - f.offset) return MseDecrypt::Failed;
      decrypt_sample(aes, rec, bytes.subview(f.offset, f.size));
      aux_garbage_ += rec.size;
      f.aux = 0;   // (in the clear from now on)
      --protected_frames_;
    }
  }
  return MseDecrypt::Ok;
}

// --- Ogg: the Opus and Vorbis sound of a WebM stream, in the form the audio player reads -----------------------------------------------------

namespace {

bool ogg_page(ByteBuffer* out, uint8_t type, int64_t granule, uint32_t serial, uint32_t sequence, const ConstByteView* packets, uint32_t count) {
  return ogg::page(out, type, granule, serial, sequence, packets, count);
}
void put_le(ByteBuffer* b, uint64_t v, int bytes) { ogg::put_le(b, v, bytes); }

}  // namespace

bool MseBuffer::write_ogg(const MseSnapshotPlan& plan, MseContainer container, ByteBuffer* out, int64_t* base_us) const {
  const MseInit& init = *inits_[plan.generation];
  const MseTrackInfo* info = nullptr;
  for (uint64_t k = 0; k < init.tracks.count(); ++k)
    if (init.tracks[k].kind == MseTrackKind::Audio && init.tracks[k].container == container) info = &init.tracks[k];
  if (!info) return false;
  const Slot* slot = nullptr;
  for (uint32_t s = 0; s < slot_count_; ++s)
    if (slots_[s]->kind == MseTrackKind::Audio && slots_[s]->id == info->id) slot = slots_[s].get();
  if (!slot) return false;
  const ConstByteView priv = init.entry(*info);
  constexpr uint32_t kSerial = 0x5443534F;
  uint32_t sequence = 0;
  // the headers
  ByteBuffer tags(allocator_);
  uint32_t pre_skip = 0;
  if (container == kContainerOpus) {
    if (priv.size() < 19) return false;
    pre_skip = priv[10] | (uint32_t{priv[11]} << 8);
    static const char kVendor[] = "TheoryCraftOS";
    tags.append(ConstByteView(reinterpret_cast<const uint8_t*>("OpusTags"), 8));
    put_le(&tags, sizeof kVendor - 1, 4);
    tags.append(ConstByteView(reinterpret_cast<const uint8_t*>(kVendor), sizeof kVendor - 1));
    put_le(&tags, 0, 4);
    if (!ogg_page(out, 0x02, 0, kSerial, sequence++, &priv, 1)) return false;
    const ConstByteView t = tags.view();
    if (!ogg_page(out, 0, 0, kSerial, sequence++, &t, 1)) return false;
  } else {
    // CodecPrivate of Vorbis: the count of packets less one, the sizes of all but the last (runs of 255), the three header packets
    ByteReader r(priv);
    if (r.u8() != 2) return false;
    uint64_t sizes[2];
    for (uint64_t& size : sizes) {
      size = 0;
      uint8_t b;
      do {
        b = r.u8();
        size += b;
      } while (b == 255 && r.ok());
    }
    if (!r.ok()) return false;
    const ConstByteView identification = r.take(sizes[0]), comment = r.take(sizes[1]), setup = r.rest();
    if (!r.ok() || setup.empty()) return false;
    if (!ogg_page(out, 0x02, 0, kSerial, sequence++, &identification, 1)) return false;
    const ConstByteView second[2] = {comment, setup};
    if (!ogg_page(out, 0, 0, kSerial, sequence++, second, 2)) return false;
  }
  // the packets: pages of a few at a time, each with the position in samples of the end of its last packet
  const uint64_t rate = container == kContainerOpus ? 48000 : info->sample_rate;
  int64_t first = INT64_MAX;
  for (const Frame& f : slot->frames.view())
    if (f.generation == plan.generation && f.pts >= plan.start_us && f.pts < plan.end_us && f.pts < first) first = f.pts;
  if (first == INT64_MAX) return false;
  ConstByteView group[64];
  uint32_t in_group = 0;
  uint64_t group_bytes = 0;
  int64_t group_end = 0;
  bool wrote = false;
  const auto flush = [&](bool last) {
    if (in_group == 0) return true;
    const int64_t granule = static_cast<int64_t>(pre_skip) + group_end * static_cast<int64_t>(rate) / 1000000;
    const bool ok = ogg_page(out, last ? 0x04 : 0, granule, kSerial, sequence++, group, in_group);
    in_group = 0;
    group_bytes = 0;
    wrote = wrote || ok;
    return ok;
  };
  // (frames are in decode order, which for sound is the order they play in)
  uint64_t remaining = 0;
  for (const Frame& f : slot->frames.view())
    if (f.generation == plan.generation && f.pts >= plan.start_us && f.pts < plan.end_us) ++remaining;
  for (const Frame& f : slot->frames.view()) {
    if (f.generation != plan.generation || f.pts < plan.start_us || f.pts >= plan.end_us) continue;
    --remaining;
    const ConstByteView data = arena_->view().subview(f.offset, f.size);
    if (in_group == 64 || group_bytes + data.size() > 6000) {
      if (!flush(false)) return false;
    }
    group[in_group++] = data;
    group_bytes += data.size();
    group_end = f.pts + f.duration - first;
    if (remaining == 0 && !flush(true)) return false;
  }
  *base_us = first;
  return wrote;
}

bool MseBuffer::snapshot(MseSnapshotPlan* plan, ByteBuffer* out) const {
  out->clear();
  if (slot_count_ == 0) return false;
  if (protected_frames_ > 0) {   // (nothing encrypted goes into a file: decrypt() first)
    for (uint32_t s = 0; s < slot_count_; ++s)
      for (const Frame& f : slots_[s]->frames.view())
        if (f.aux != 0 && f.pts >= plan->start_us && f.pts < plan->end_us) return false;
  }
  // which kind of file, by the first track of the kind the plan is for
  if (plan->generation >= inits_.count()) return false;
  const MseInit& init = *inits_[plan->generation];
  const bool want_video = plan->part == 1 || (plan->part == 0 && plan->has_video);
  const MseTrackInfo* lead = init.first_of(want_video ? MseTrackKind::Video : MseTrackKind::Audio);
  if (!lead) return false;
  plan->has_video = lead->kind == MseTrackKind::Video;
  if (lead->container == kContainerOpus || lead->container == kContainerVorbis) return write_ogg(*plan, static_cast<MseContainer>(lead->container), out, &plan->base_us);
  if (lead->container == kContainerRaw) {
    // the frames of the range, one after the other, are the file
    const FrameList& list = slots_[0]->frames;
    int64_t first = INT64_MAX;
    for (const Frame& f : list.view()) {
      if (f.generation != plan->generation || f.pts < plan->start_us || f.pts >= plan->end_us) continue;
      if (f.pts < first) first = f.pts;
      if (!out->append(arena_->view().subview(f.offset, f.size))) return false;
    }
    plan->base_us = first == INT64_MAX ? plan->start_us : first;
    return out->size() > 0;
  }
  return write_mp4(*plan, out, &plan->base_us);
}

bool MseBuffer::write_mp4(const MseSnapshotPlan& plan, ByteBuffer* out, int64_t* base_us) const {
  struct TrackOut {
    ArrayView<uint32_t> indexes;   // the frames, in decode order
    uint64_t count = 0, bytes = 0, data_offset = 0, stco_at = 0;
    uint32_t generation = 0, slot = 0;
    int64_t first_dts = 0, duration_ticks = 0;
    const MseTrackInfo* info = nullptr;
  };
  TrackOut tracks[kMaxSlots];
  uint32_t track_count = 0;
  int64_t base = INT64_MAX;
  bool failed = false;
  for (uint32_t s = 0; s < slot_count_ && !failed; ++s) {
    const FrameList& list = slots_[s]->frames;
    tracks[track_count] = TrackOut();   // (a slot that was skipped left its numbers behind)
    TrackOut& t = tracks[track_count];
    t.slot = s;
    t.indexes = allocate_view<uint32_t>(allocator_, list.size() ? list.size() : 1);
    if (t.indexes.empty()) {
      failed = true;
      break;
    }
    bool have_generation = false;
    for (uint64_t i = 0; i < list.size(); ++i) {
      const Frame& f = list[i];
      if (f.pts < plan.start_us || f.pts >= plan.end_us) continue;
      if (!have_generation) {
        t.generation = f.generation;
        have_generation = true;
      }
      if (f.generation != t.generation) continue;
      if (t.count == 0) t.first_dts = f.dts;
      t.indexes[t.count++] = static_cast<uint32_t>(i);
      t.bytes += f.size;
    }
    if (t.count == 0) {
      release_view(allocator_, t.indexes);
      continue;
    }
    // the track's description, from the init segment its frames belong to
    const MseInit& init = *inits_[t.generation];
    for (uint64_t k = 0; k < init.tracks.count(); ++k) {
      if (init.tracks[k].id == slots_[s]->id && init.tracks[k].kind == slots_[s]->kind) t.info = &init.tracks[k];
    }
    // only the tracks this file is for: MP4 can carry H.264, VP9, AV1 and AAC / MP3; the part says picture or sound
    if (!t.info || t.info->container != kContainerMp4 || (plan.part == 1 && t.info->kind != MseTrackKind::Video) || (plan.part == 2 && t.info->kind != MseTrackKind::Audio)) {
      release_view(allocator_, t.indexes);
      continue;
    }
    if (t.first_dts < base) base = t.first_dts;
    ++track_count;
  }
  bool ok = !failed && track_count > 0;
  if (ok) {
    Out w(*out);
    uint64_t at = w.begin("ftyp");
    w.tag("isom");
    w.u32(512);
    w.tag("isom");
    w.tag("iso2");
    w.tag("avc1");
    w.tag("mp41");
    w.end(at);
    int64_t movie_ms = 0;
    for (uint32_t i = 0; i < track_count; ++i) {
      TrackOut& t = tracks[i];
      const uint32_t scale = t.info->timescale;
      const FrameList& frames = slots_[t.slot]->frames;
      const Frame& last = frames[t.indexes[t.count - 1]];
      t.duration_ticks = us_to_ticks(last.dts + last.duration - t.first_dts, scale);
      const int64_t ms = (last.dts + last.duration - t.first_dts) / 1000;
      if (ms > movie_ms) movie_ms = ms;
    }
    const uint64_t moov = w.begin("moov");
    {
      const uint64_t mvhd = w.begin_full("mvhd", 0);
      w.u32(0);
      w.u32(0);
      w.u32(1000);
      w.u32(static_cast<uint32_t>(movie_ms));
      w.u32(0x00010000);
      w.u16(0x0100);
      w.zeros(10);
      const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
      for (uint32_t m : matrix) w.u32(m);
      w.zeros(24);
      w.u32(track_count + 1);
      w.end(mvhd);
    }
    for (uint32_t i = 0; i < track_count; ++i) {
      TrackOut& t = tracks[i];
      const bool video = t.info->kind == MseTrackKind::Video;
      const uint32_t scale = t.info->timescale;
      const FrameList& frames = slots_[t.slot]->frames;
      const uint64_t trak = w.begin("trak");
      {
        const uint64_t tkhd = w.begin_full("tkhd", 3);
        w.u32(0);
        w.u32(0);
        w.u32(i + 1);
        w.u32(0);
        w.u32(static_cast<uint32_t>((t.duration_ticks * 1000) / (scale ? scale : 1)));
        w.zeros(8);
        w.u16(0);
        w.u16(0);
        w.u16(video ? 0 : 0x0100);
        w.u16(0);
        const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
        for (uint32_t m : matrix) w.u32(m);
        w.u32(video ? t.info->width << 16 : 0);
        w.u32(video ? t.info->height << 16 : 0);
        w.end(tkhd);
      }
      const uint64_t mdia = w.begin("mdia");
      {
        const uint64_t mdhd = w.begin_full("mdhd", 0);
        w.u32(0);
        w.u32(0);
        w.u32(scale);
        w.u32(static_cast<uint32_t>(t.duration_ticks));
        w.u16(0x55C4);
        w.u16(0);
        w.end(mdhd);
        const uint64_t hdlr = w.begin_full("hdlr", 0);
        w.u32(0);
        w.tag(video ? "vide" : "soun");
        w.zeros(12);
        const char* name = video ? "VideoHandler" : "SoundHandler";
        for (const char* c = name; *c; ++c) w.u8(static_cast<uint8_t>(*c));
        w.u8(0);
        w.end(hdlr);
      }
      const uint64_t minf = w.begin("minf");
      if (video) {
        const uint64_t vmhd = w.begin_full("vmhd", 1);
        w.zeros(8);
        w.end(vmhd);
      } else {
        const uint64_t smhd = w.begin_full("smhd", 0);
        w.zeros(4);
        w.end(smhd);
      }
      {
        const uint64_t dinf = w.begin("dinf");
        const uint64_t dref = w.begin_full("dref", 0);
        w.u32(1);
        const uint64_t url = w.begin_full("url ", 1);
        w.end(url);
        w.end(dref);
        w.end(dinf);
      }
      const uint64_t stbl = w.begin("stbl");
      w.bytes(inits_[t.generation]->entry(*t.info));
      {   // stts: runs of equal durations
        const uint64_t stts = w.begin_full("stts", 0);
        const uint64_t count_at = out->size();
        w.u32(0);
        uint32_t entries = 0;
        uint32_t run = 0, run_delta = 0;
        for (uint64_t k = 0; k < t.count; ++k) {
          const Frame& f = frames[t.indexes[k]];
          const int64_t this_ticks = us_to_ticks(f.dts - t.first_dts, scale);
          const int64_t next_ticks = k + 1 < t.count ? us_to_ticks(frames[t.indexes[k + 1]].dts - t.first_dts, scale) : us_to_ticks(f.dts + f.duration - t.first_dts, scale);
          int64_t delta = next_ticks - this_ticks;
          if (delta < 0) delta = 0;
          const uint32_t d = static_cast<uint32_t>(delta);
          if (run > 0 && d == run_delta) {
            ++run;
          } else {
            if (run > 0) {
              w.u32(run);
              w.u32(run_delta);
              ++entries;
            }
            run = 1;
            run_delta = d;
          }
        }
        if (run > 0) {
          w.u32(run);
          w.u32(run_delta);
          ++entries;
        }
        w.patch32(count_at, entries);
        w.end(stts);
      }
      {   // ctts, if any frame is shown at another time than it is decoded
        bool any = false, negative = false;
        for (uint64_t k = 0; k < t.count; ++k) {
          const Frame& f = frames[t.indexes[k]];
          const int64_t cto = us_to_ticks(f.pts - t.first_dts, scale) - us_to_ticks(f.dts - t.first_dts, scale);
          if (cto != 0) any = true;
          if (cto < 0) negative = true;
        }
        if (any) {
          const uint64_t ctts = w.begin_full("ctts", negative ? 0x01000000u : 0);
          const uint64_t count_at = out->size();
          w.u32(0);
          uint32_t entries = 0, run = 0;
          int64_t run_value = 0;
          for (uint64_t k = 0; k < t.count; ++k) {
            const Frame& f = frames[t.indexes[k]];
            const int64_t cto = us_to_ticks(f.pts - t.first_dts, scale) - us_to_ticks(f.dts - t.first_dts, scale);
            if (run > 0 && cto == run_value) {
              ++run;
            } else {
              if (run > 0) {
                w.u32(run);
                w.u32(static_cast<uint32_t>(static_cast<int32_t>(run_value)));
                ++entries;
              }
              run = 1;
              run_value = cto;
            }
          }
          if (run > 0) {
            w.u32(run);
            w.u32(static_cast<uint32_t>(static_cast<int32_t>(run_value)));
            ++entries;
          }
          w.patch32(count_at, entries);
          w.end(ctts);
        }
      }
      {   // stsc: all the samples in one chunk
        const uint64_t stsc = w.begin_full("stsc", 0);
        w.u32(1);
        w.u32(1);
        w.u32(static_cast<uint32_t>(t.count));
        w.u32(1);
        w.end(stsc);
      }
      {
        const uint64_t stsz = w.begin_full("stsz", 0);
        w.u32(0);
        w.u32(static_cast<uint32_t>(t.count));
        for (uint64_t k = 0; k < t.count; ++k) w.u32(frames[t.indexes[k]].size);
        w.end(stsz);
      }
      {
        const uint64_t stco = w.begin_full("stco", 0);
        w.u32(1);
        t.stco_at = out->size();
        w.u32(0);
        w.end(stco);
      }
      {
        bool all_sync = true;
        for (uint64_t k = 0; k < t.count; ++k)
          if (!frames[t.indexes[k]].sync) all_sync = false;
        if (!all_sync) {
          const uint64_t stss = w.begin_full("stss", 0);
          const uint64_t count_at = out->size();
          w.u32(0);
          uint32_t entries = 0;
          for (uint64_t k = 0; k < t.count; ++k) {
            if (!frames[t.indexes[k]].sync) continue;
            w.u32(static_cast<uint32_t>(k + 1));
            ++entries;
          }
          w.patch32(count_at, entries);
          w.end(stss);
        }
      }
      w.end(stbl);
      w.end(minf);
      w.end(mdia);
      w.end(trak);
    }
    w.end(moov);
    // the media data: one track after the other
    uint64_t total = 0;
    for (uint32_t i = 0; i < track_count; ++i) {
      tracks[i].data_offset = total;
      total += tracks[i].bytes;
    }
    if (total > 0xFFFFFFF0u - out->size()) ok = false;
    const uint64_t payload = out->size() + 8;
    for (uint32_t i = 0; i < track_count && ok; ++i) w.patch32(tracks[i].stco_at, static_cast<uint32_t>(payload + tracks[i].data_offset));
    const uint64_t mdat = w.begin("mdat");
    for (uint32_t i = 0; i < track_count && ok; ++i) {
      const FrameList& frames = slots_[tracks[i].slot]->frames;
      for (uint64_t k = 0; k < tracks[i].count; ++k) {
        const Frame& f = frames[tracks[i].indexes[k]];
        w.bytes(arena_->view().subview(f.offset, f.size));
      }
    }
    w.end(mdat);
    ok = ok && w.ok;
    *base_us = base;
  }
  for (uint32_t i = 0; i < kMaxSlots; ++i) release_view(allocator_, tracks[i].indexes);
  return ok;
}

}  // namespace media
