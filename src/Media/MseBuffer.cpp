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
#include "Media/Mse.hpp"
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
  }
  return true;
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
    : allocator_(allocator), format_(format), id_(id), parser_(make_mse_parser(allocator, format)), inits_(allocator), arena_(UniquePointer<ByteBuffer>::make(allocator, allocator)), range_cache_(allocator) {
  for (int8_t& m : slot_map_) m = -1;
  if (format != MseFormat::Mp4) mode_ = Mode::Sequence;   // (the formats without timestamps of their own are always in sequence mode)
}

MseBuffer::~MseBuffer() = default;

bool MseBuffer::set_mode(Mode mode) {
  if (format_ != MseFormat::Mp4 && mode == Mode::Segments) return false;
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
  if ((format == MseFormat::Mp4) != (format_ == MseFormat::Mp4)) return false;
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

bool MseBuffer::frame(uint32_t track, int64_t pts_us, int64_t dts_us, int64_t dur_us, bool sync, ConstByteView data) {
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
    if (!add_frame(si, pts, dts, dur_us, sync, data)) return false;
    slot.has_last = true;
    slot.last_dts = dts;
    slot.last_dur = dur_us;
    if (pts + dur_us > group_end_) group_end_ = pts + dur_us;
    if (pts + dur_us > highest_added_) highest_added_ = pts + dur_us;
    return true;
  }
  return true;
}

bool MseBuffer::add_frame(uint32_t si, int64_t pts, int64_t dts, int64_t duration, bool sync, ConstByteView data) {
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
  if (garbage_ > (4u << 20) && garbage_ > arena_used_ / 2) compact();
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

bool MseBuffer::plan(int64_t t, MseSnapshotPlan* out) const {
  List<Range> ranges(allocator_);
  buffered(&ranges);
  const Range* found = nullptr;
  for (const Range& r : ranges.view()) {
    if (t >= r.start - 100000 && t < r.end) {
      found = &r;
      break;
    }
  }
  if (!found) return false;
  const Slot* lead = nullptr;
  for (uint32_t s = 0; s < slot_count_; ++s) {
    if (slots_[s]->frames.size() == 0) continue;
    if (!lead || (slots_[s]->kind == MseTrackKind::Video && lead->kind != MseTrackKind::Video)) lead = slots_[s].get();
  }
  if (!lead) return false;
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
  if (!have_generation && next_pts == INT64_MAX) return false;
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
  if (end <= start) return false;
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
  return true;
}

bool MseBuffer::snapshot(MseSnapshotPlan* plan, ByteBuffer* out) const {
  out->clear();
  if (format_ != MseFormat::Mp4) {
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
    if (!t.info) {
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
