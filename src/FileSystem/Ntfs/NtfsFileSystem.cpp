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

// Mounting, reading and the namespace of NtfsFileSystem; the writing half is in NtfsWrite.cpp.
#include "FileSystem/Ntfs/NtfsFileSystem.hpp"
#include "FileSystem/Ntfs/NtfsFormat.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/Move.hpp"
#include "Utility/Utf8.hpp"

namespace filesystem {

using namespace ntfs;
using util::move;

namespace {
constexpr uint32_t kMaxRuns = 1u << 22;  // a stream with more runs than this is treated as damaged
}

// An open file or directory: registered with the filesystem, which refuses to remove or rename what is open.
class NtfsFileSystem::File final : public IFile {
 public:
  File(NtfsFileSystem& fs, uint64_t number, bool writable) : fs_(fs), number_(number), writable_(writable), stream_(fs.allocator_) {
    handle_.number = number;
    handle_.next = fs_.open_;
    fs_.open_ = &handle_;
  }
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (dirty_) fs_.sync_names(number_);
    for (OpenHandle** link = &fs_.open_; *link; link = &(*link)->next) {
      if (*link == &handle_) {
        *link = handle_.next;
        break;
      }
    }
  }

  uint64_t size() const override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    return refresh() == Status::Ok ? stream_.size : 0;
  }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    Status s = refresh();
    if (s != Status::Ok) return s;
    return fs_.stream_read(stream_, offset, buffer, transferred);
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.begin_write();
    if (s != Status::Ok) return s;
    s = refresh();
    if (s != Status::Ok) return s;
    if (data.empty()) return Status::Ok;
    const uint64_t end = offset + data.size();
    if (end < offset) return Status::NoSpace;
    if (end > stream_.size) {
      s = fs_.stream_resize(stream_, end, false);
      fs_.epoch_++;
      epoch_ = fs_.epoch_;
      if (s != Status::Ok) return s;
      dirty_ = true;
    }
    s = fs_.stream_write(stream_, offset, data);
    fs_.epoch_++;
    epoch_ = fs_.epoch_;
    if (s == Status::Ok) {
      *transferred = data.size();
      dirty_ = true;
    }
    return s;
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.begin_write();
    if (s != Status::Ok) return s;
    s = refresh();
    if (s != Status::Ok) return s;
    if (new_size == stream_.size) return Status::Ok;
    s = fs_.stream_resize(stream_, new_size, false);
    fs_.epoch_++;
    epoch_ = fs_.epoch_;
    if (s == Status::Ok) dirty_ = true;
    return s;
  }
  Status flush() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (dirty_) {
      const Status s = fs_.sync_names(number_);
      if (s != Status::Ok) return s;
      dirty_ = false;
    }
    return fs_.flush();
  }

 private:
  // Loads the data stream again if anything was written since.
  Status refresh() const {
    if (loaded_ && epoch_ == fs_.epoch_) return Status::Ok;
    Stream fresh(fs_.allocator_);
    const Status s = fs_.stream_open_number(number_, kAttrData, "", &fresh);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    stream_.record = fresh.record;
    stream_.type = fresh.type;
    stream_.resident = fresh.resident;
    stream_.in_base = fresh.in_base;
    stream_.flags = fresh.flags;
    stream_.compression_shift = fresh.compression_shift;
    stream_.size = fresh.size;
    stream_.allocated = fresh.allocated;
    stream_.initialized = fresh.initialized;
    stream_.runs.clear();
    if (copy_runs(fresh.runs, &stream_.runs) != Status::Ok) return Status::OutOfMemory;
    loaded_ = true;
    epoch_ = fs_.epoch_;
    return Status::Ok;
  }

  NtfsFileSystem& fs_;
  const uint64_t number_;
  const bool writable_;
  OpenHandle handle_;
  mutable Stream stream_;
  mutable bool loaded_ = false;
  bool dirty_ = false;
  mutable uint64_t epoch_ = 0;
};

class NtfsFileSystem::Directory final : public IDirectory {
 public:
  Directory(NtfsFileSystem& fs, uint64_t number) : fs_(fs), number_(number) {
    handle_.number = number;
    handle_.next = fs_.open_;
    fs_.open_ = &handle_;
  }
  ~Directory() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (OpenHandle** link = &fs_.open_; *link; link = &(*link)->next) {
      if (*link == &handle_) {
        *link = handle_.next;
        break;
      }
    }
  }
  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (;;) {
      uint64_t number = 0;
      NameInfo name;
      Status s = fs_.dir_next(number_, &cursor_, &number, &name);
      if (s != Status::Ok) return s;
      s = fs_.describe(number, name, entry);
      if (s == Status::NotFound) continue;  // an entry whose record is gone
      return s;
    }
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    cursor_ = Cursor();
  }

 private:
  NtfsFileSystem& fs_;
  const uint64_t number_;
  OpenHandle handle_;
  Cursor cursor_;
};

Status NtfsFileSystem::mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                             UniquePointer<IFileSystem>* result) {
  UniquePointer<IFileSystem> fs = UniquePointer<IFileSystem>::make_as<NtfsFileSystem>(allocator, allocator, device, first_block);
  if (!fs) return Status::OutOfMemory;
  const Status s = static_cast<NtfsFileSystem*>(fs.get())->init();
  if (s != Status::Ok) return s;
  *result = move(fs);
  return Status::Ok;
}

NtfsFileSystem::NtfsFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block)
    : allocator_(allocator), device_(device), first_block_(first_block), mft_runs_(allocator), bitmap_runs_(allocator), unit_cache_(allocator) {}

NtfsFileSystem::~NtfsFileSystem() {
  if (dirty_set_) finish_write();
  release_view(allocator_, block_);
  release_view(allocator_, upcase_);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Device access

Status NtfsFileSystem::read_bytes(uint64_t offset, ByteView out) {
  if (offset > UINT64_MAX - first_block_ * block_size_) return Status::Corrupt;
  uint64_t absolute = first_block_ * block_size_ + offset;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint64_t block = absolute / block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % block_size_);
    const uint64_t left = out.size() - done;
    if (within == 0 && left >= block_size_) {  // whole blocks straight into the caller's buffer
      const uint64_t bytes = left / block_size_ * block_size_;
      if (!device_.read(block, out.subview(done, bytes))) return Status::IoError;
      done += bytes;
      absolute += bytes;
      continue;
    }
    if (block_number_ != block) {
      block_number_ = ~uint64_t{0};
      if (!device_.read(block, block_)) return Status::IoError;
      block_number_ = block;
    }
    uint64_t take = block_size_ - within;
    if (take > left) take = left;
    out.subview(done, take).copy_from(ConstByteView(block_).subview(within, take));
    done += take;
    absolute += take;
  }
  return Status::Ok;
}

Status NtfsFileSystem::write_bytes(uint64_t offset, ConstByteView data) {
  if (!writable_) return Status::ReadOnly;
  if (offset > UINT64_MAX - first_block_ * block_size_) return Status::Corrupt;
  uint64_t absolute = first_block_ * block_size_ + offset;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t block = absolute / block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % block_size_);
    const uint64_t left = data.size() - done;
    if (within == 0 && left >= block_size_) {  // whole blocks straight from the caller's buffer
      const uint64_t bytes = left / block_size_ * block_size_;
      if (block_number_ != ~uint64_t{0} && block_number_ >= block && block_number_ < block + bytes / block_size_) block_number_ = ~uint64_t{0};
      if (!device_.write(block, data.subview(done, bytes))) return Status::IoError;
      done += bytes;
      absolute += bytes;
      continue;
    }
    if (block_number_ != block) {
      block_number_ = ~uint64_t{0};
      if (!device_.read(block, block_)) return Status::IoError;
      block_number_ = block;
    }
    uint64_t take = block_size_ - within;
    if (take > left) take = left;
    block_.subview(within, take).copy_from(data.subview(done, take));
    if (!device_.write(block, ConstByteView(block_))) {
      block_number_ = ~uint64_t{0};
      return Status::IoError;
    }
    done += take;
    absolute += take;
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Runs of clusters

const NtfsFileSystem::Run* NtfsFileSystem::run_for(const RunList& runs, uint64_t vcn) const {
  ArrayView<const Run> view = runs.view();
  uint64_t lo = 0, hi = view.size();
  while (lo < hi) {  // the last run that starts at or before vcn
    const uint64_t mid = lo + (hi - lo) / 2;
    if (view[mid].vcn <= vcn) lo = mid + 1;
    else hi = mid;
  }
  if (lo == 0) return nullptr;
  const Run& run = view[lo - 1];
  return vcn - run.vcn < run.length ? &run : nullptr;
}

uint64_t NtfsFileSystem::runs_end(const RunList& runs) {
  ArrayView<const Run> view = runs.view();
  return view.empty() ? 0 : view[view.size() - 1].vcn + view[view.size() - 1].length;
}

Status NtfsFileSystem::copy_runs(const RunList& from, RunList* to) {
  to->clear();
  if (!to->reserve(from.count())) return Status::OutOfMemory;
  for (const Run& run : from.view())
    if (!to->push_back(run)) return Status::OutOfMemory;
  return Status::Ok;
}

Status NtfsFileSystem::read_runs(const RunList& runs, uint64_t offset, ByteView out) {
  uint64_t done = 0;
  while (done < out.size()) {
    if (offset > UINT64_MAX - done) return Status::Corrupt;
    const uint64_t pos = offset + done;
    const Run* run = run_for(runs, pos / cluster_bytes_);
    if (!run) return Status::Corrupt;
    const uint64_t within = pos - run->vcn * cluster_bytes_;
    uint64_t take = run->length * cluster_bytes_ - within;
    if (take > out.size() - done) take = out.size() - done;
    if (run->lcn == kHole) {
      out.subview(done, take).fill(0);
    } else {
      const Status s = read_bytes(run->lcn * cluster_bytes_ + within, out.subview(done, take));
      if (s != Status::Ok) return s;
    }
    done += take;
  }
  return Status::Ok;
}

Status NtfsFileSystem::write_runs(const RunList& runs, uint64_t offset, ConstByteView data) {
  uint64_t done = 0;
  while (done < data.size()) {
    if (offset > UINT64_MAX - done) return Status::Corrupt;
    const uint64_t pos = offset + done;
    const Run* run = run_for(runs, pos / cluster_bytes_);
    if (!run || run->lcn == kHole) return Status::Corrupt;
    const uint64_t within = pos - run->vcn * cluster_bytes_;
    uint64_t take = run->length * cluster_bytes_ - within;
    if (take > data.size() - done) take = data.size() - done;
    const Status s = write_bytes(run->lcn * cluster_bytes_ + within, data.subview(done, take));
    if (s != Status::Ok) return s;
    done += take;
  }
  return Status::Ok;
}

// Zeros the bytes [from, to) of a stream that is stored in `runs` (holes are left as they are).
Status NtfsFileSystem::zero_runs(const RunList& runs, uint64_t from, uint64_t to) {
  uint8_t zero_storage[4096] = {};
  ConstByteView zeros(zero_storage);
  while (from < to) {
    const Run* run = run_for(runs, from / cluster_bytes_);
    if (!run) return Status::Corrupt;
    const uint64_t run_end = (run->vcn + run->length) * cluster_bytes_;
    uint64_t stop = to < run_end ? to : run_end;
    if (run->lcn != kHole) {
      for (uint64_t at = from; at < stop;) {
        const uint64_t n = stop - at < zeros.size() ? stop - at : zeros.size();
        const Status s = write_bytes(run->lcn * cluster_bytes_ + (at - run->vcn * cluster_bytes_), zeros.first(n));
        if (s != Status::Ok) return s;
        at += n;
      }
    }
    from = stop;
  }
  return Status::Ok;
}

// Decodes mapping pairs and appends the runs to `out`, which must end where this part starts.
Status NtfsFileSystem::decode_runs(ConstByteView pairs, uint64_t start_vcn, RunList* out) const {
  if (start_vcn != runs_end(*out)) return Status::Corrupt;
  uint64_t vcn = start_vcn, i = 0;
  int64_t lcn = 0;
  while (i < pairs.size()) {
    const uint8_t head = pairs[i++];
    if (head == 0) return Status::Ok;
    const uint32_t length_bytes = head & 0xF, offset_bytes = head >> 4;
    if (length_bytes == 0 || length_bytes > 8 || offset_bytes > 8) return Status::Corrupt;
    if (pairs.size() - i < uint64_t{length_bytes} + offset_bytes) return Status::Corrupt;
    uint64_t length = 0;
    for (uint32_t k = 0; k < length_bytes; ++k) length |= uint64_t{pairs[i + k]} << (8 * k);
    i += length_bytes;
    if (length == 0 || length > kMaxClusters || vcn > kMaxClusters) return Status::Corrupt;
    Run run;
    run.vcn = vcn;
    run.length = length;
    if (offset_bytes == 0) {
      run.lcn = kHole;
    } else {
      uint64_t raw = 0;
      for (uint32_t k = 0; k < offset_bytes; ++k) raw |= uint64_t{pairs[i + k]} << (8 * k);
      if (offset_bytes < 8 && (pairs[i + offset_bytes - 1] & 0x80)) raw |= ~uint64_t{0} << (8 * offset_bytes);
      i += offset_bytes;
      const int64_t delta = static_cast<int64_t>(raw);
      if ((delta > 0 && lcn > INT64_MAX - delta) || (delta < 0 && lcn < INT64_MIN - delta)) return Status::Corrupt;
      lcn += delta;
      if (lcn < 0 || static_cast<uint64_t>(lcn) >= cluster_count_ || length > cluster_count_ - static_cast<uint64_t>(lcn)) return Status::Corrupt;
      run.lcn = static_cast<uint64_t>(lcn);
    }
    if (out->count() >= kMaxRuns || !out->push_back(run)) return Status::OutOfMemory;
    vcn += length;
  }
  return Status::Ok;
}

// Appends the mapping pairs of `runs` (holes included) and the terminating zero.
bool NtfsFileSystem::encode_runs(const RunList& runs, ByteBuffer* out) const {
  int64_t previous = 0;
  for (const Run& run : runs.view()) {
    uint32_t length_bytes = 1;
    while (length_bytes < 8 && (run.length >> (8 * length_bytes - 1)) != 0) ++length_bytes;  // (lengths are signed: 128 takes two bytes)
    int64_t delta = 0;
    uint32_t offset_bytes = 0;
    if (run.lcn != kHole) {
      delta = static_cast<int64_t>(run.lcn) - previous;
      offset_bytes = 1;
      while (offset_bytes < 8) {  // the fewest bytes that hold delta as a signed number
        const int64_t limit = int64_t{1} << (8 * offset_bytes - 1);
        if (delta >= -limit && delta < limit) break;
        ++offset_bytes;
      }
      previous = static_cast<int64_t>(run.lcn);
    }
    if (!out->put8(static_cast<uint8_t>(length_bytes | (offset_bytes << 4)))) return false;
    for (uint32_t k = 0; k < length_bytes; ++k)
      if (!out->put8(static_cast<uint8_t>(run.length >> (8 * k)))) return false;
    for (uint32_t k = 0; k < offset_bytes; ++k)
      if (!out->put8(static_cast<uint8_t>(static_cast<uint64_t>(delta) >> (8 * k)))) return false;
  }
  return out->put8(0);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// File records and attributes

Status NtfsFileSystem::read_record(uint64_t number, Record* record, bool need_in_use) {
  record->number = number;
  record->buffer.clear();
  if (number > mft_size_ / record_size_ || number == mft_size_ / record_size_) return Status::NotFound;
  if (!record->buffer.put_zeros(record_size_)) return Status::OutOfMemory;
  ByteView data = record->bytes();
  Status s = read_runs(mft_runs_, number * record_size_, data);
  if (s != Status::Ok) return s;
  if (data[0] != 'F' || data[1] != 'I' || data[2] != 'L' || data[3] != 'E') return need_in_use ? Status::NotFound : Status::Corrupt;
  const uint32_t usa_offset = rd16(data, 4), usa_count = rd16(data, 6);
  if (usa_count != record_size_ / kFixupUnit + 1 || usa_offset < 0x28 || usa_offset + 2 * usa_count > kFixupUnit - 2) return Status::Corrupt;
  const uint16_t usn = rd16(data, usa_offset);
  for (uint32_t i = 1; i < usa_count; ++i) {
    const uint32_t end = i * kFixupUnit - 2;
    if (rd16(data, end) != usn) return Status::Corrupt;  // a write that was cut short
    wr16(data, end, rd16(data, usa_offset + 2 * i));
  }
  const uint32_t first = rd16(data, 0x14), used = rd32(data, 0x18), allocated = rd32(data, 0x1C);
  if (first < usa_offset + 2 * usa_count || (first & 7) != 0 || used < first + 8 || (used & 7) != 0 || allocated > record_size_ || used > allocated)
    return Status::Corrupt;
  if (need_in_use && !(rd16(data, 0x16) & kRecordInUse)) return Status::NotFound;
  return Status::Ok;
}

Status NtfsFileSystem::write_record(Record& record) {
  if (!writable_) return Status::ReadOnly;
  ByteBuffer raw(allocator_);
  if (!raw.append(record.bytes())) return Status::OutOfMemory;
  ByteView data = raw.writable_view();
  const uint32_t usa_offset = rd16(data, 4), usa_count = rd16(data, 6);
  uint16_t usn = static_cast<uint16_t>(rd16(data, usa_offset) + 1);
  if (usn == 0 || usn == 0xFFFF) usn = 1;
  wr16(data, usa_offset, usn);
  for (uint32_t i = 1; i < usa_count; ++i) {
    const uint32_t end = i * kFixupUnit - 2;
    wr16(data, usa_offset + 2 * i, rd16(data, end));
    wr16(data, end, usn);
  }
  if (record.number >= mft_size_ / record_size_) return Status::Corrupt;
  Status s = write_runs(mft_runs_, record.number * record_size_, data);
  if (s != Status::Ok) return s;
  if (record.number < 4 && mirror_lcn_ != 0) {  // the first records are mirrored
    const uint64_t at = mirror_lcn_ * cluster_bytes_ + record.number * record_size_;
    if (at / cluster_bytes_ < cluster_count_) s = write_bytes(at, data);
  }
  return s;
}

Status NtfsFileSystem::attr_at(const Record& record, uint32_t offset, Attr* attr) {
  ConstByteView data = record.bytes();
  const uint32_t used = rd32(data, 0x18);
  if ((offset & 7) != 0 || offset > used || used - offset < 8) return Status::Corrupt;
  const uint32_t type = rd32(data, offset);
  if (type == kAttrEnd) return Status::EndOfDirectory;
  if (used - offset < 0x18) return Status::Corrupt;
  Attr a;
  a.offset = offset;
  a.type = type;
  a.length = rd32(data, offset + 4);
  a.nonresident = data[offset + 8] != 0;
  a.name_length = data[offset + 9];
  a.flags = rd16(data, offset + 12);
  a.id = rd16(data, offset + 14);
  if (a.length < (a.nonresident ? 0x40u : 0x18u) || (a.length & 7) != 0 || a.length > used - offset) return Status::Corrupt;
  const uint32_t name_rel = rd16(data, offset + 10);
  if (a.name_length != 0 && (name_rel < (a.nonresident ? 0x40u : 0x18u) || name_rel > a.length || a.length - name_rel < 2u * a.name_length)) return Status::Corrupt;
  a.name_offset = offset + name_rel;
  if (!a.nonresident) {
    const uint32_t value_rel = rd16(data, offset + 20);
    a.value_length = rd32(data, offset + 16);
    if (value_rel > a.length || a.length - value_rel < a.value_length) return Status::Corrupt;
    a.value_offset = offset + value_rel;
  } else {
    a.lowest_vcn = rd64(data, offset + 16);
    a.highest_vcn = rd64(data, offset + 24);
    const uint32_t runs_rel = rd16(data, offset + 32);
    a.compression_shift = static_cast<uint8_t>(rd16(data, offset + 34));
    a.allocated = rd64(data, offset + 40);
    a.size = rd64(data, offset + 48);
    a.initialized = rd64(data, offset + 56);
    if (runs_rel < 0x40 || runs_rel > a.length || a.compression_shift > 16) return Status::Corrupt;
    if (a.lowest_vcn == 0 && (a.initialized > a.size || a.size > a.allocated)) return Status::Corrupt;
    a.runs_offset = offset + runs_rel;
  }
  *attr = a;
  return Status::Ok;
}

Status NtfsFileSystem::attr_first(const Record& record, Attr* attr) { return attr_at(record, rd16(record.bytes(), 0x14), attr); }

Status NtfsFileSystem::attr_next(const Record& record, Attr* attr) { return attr_at(record, attr->offset + attr->length, attr); }

bool NtfsFileSystem::attr_named(const Record& record, const Attr& attr, const char* name) {
  uint32_t length = 0;
  while (name && name[length]) ++length;
  if (attr.name_length != length) return false;
  ConstByteView data = record.bytes();
  for (uint32_t i = 0; i < length; ++i)
    if (rd16(data, attr.name_offset + 2 * i) != static_cast<uint8_t>(name[i])) return false;
  return true;
}

Status NtfsFileSystem::attr_find(const Record& record, uint32_t type, const char* name, Attr* attr) {
  Attr a;
  Status s = attr_first(record, &a);
  while (s == Status::Ok) {
    if (a.type == type && attr_named(record, a, name) && (!a.nonresident || a.lowest_vcn == 0)) {
      *attr = a;
      return Status::Ok;
    }
    if (a.type > type) break;  // attributes are sorted by type
    s = attr_next(record, &a);
  }
  return s == Status::EndOfDirectory || s == Status::Ok ? Status::NotFound : s;
}

Status NtfsFileSystem::attr_find_vcn(const Record& record, uint32_t type, const char* name, uint64_t lowest_vcn, Attr* attr) {
  Attr a;
  Status s = attr_first(record, &a);
  while (s == Status::Ok) {
    if (a.type == type && attr_named(record, a, name) && a.nonresident && a.lowest_vcn == lowest_vcn) {
      *attr = a;
      return Status::Ok;
    }
    s = attr_next(record, &a);
  }
  return s == Status::EndOfDirectory ? Status::NotFound : s;
}

// Where a new attribute of `type` goes: before the first attribute of a higher type (so after the ones of its own type).
uint32_t NtfsFileSystem::attr_insert_point(const Record& record, uint32_t type) {
  Attr a;
  Status s = attr_first(record, &a);
  while (s == Status::Ok) {
    if (a.type > type) return a.offset;
    s = attr_next(record, &a);
  }
  return rd32(record.bytes(), 0x18) - 8;  // the end marker
}

Status NtfsFileSystem::attr_insert(Record& record, uint32_t at, ConstByteView attr) {
  ByteView data = record.bytes();
  const uint32_t used = rd32(data, 0x18), allocated = rd32(data, 0x1C);
  if (attr.size() > allocated - used) return Status::NoSpace;
  const uint32_t n = static_cast<uint32_t>(attr.size());
  data.subview(at + n, used - at).copy_from(ConstByteView(data).subview(at, used - at));
  data.subview(at, n).copy_from(attr);
  wr32(data, 0x18, used + n);
  return Status::Ok;
}

void NtfsFileSystem::attr_remove(Record& record, const Attr& attr) {
  ByteView data = record.bytes();
  const uint32_t used = rd32(data, 0x18);
  const uint32_t end = attr.offset + attr.length;
  data.subview(attr.offset, used - end).copy_from(ConstByteView(data).subview(end, used - end));
  data.subview(used - attr.length, attr.length).fill(0);
  wr32(data, 0x18, used - attr.length);
}

Status NtfsFileSystem::attr_replace(Record& record, const Attr& old_attr, ConstByteView replacement) {
  ByteView data = record.bytes();
  const uint32_t used = rd32(data, 0x18), allocated = rd32(data, 0x1C);
  if (replacement.size() > old_attr.length && replacement.size() - old_attr.length > allocated - used) return Status::NoSpace;
  attr_remove(record, old_attr);
  return attr_insert(record, old_attr.offset, replacement);
}

uint16_t NtfsFileSystem::take_attr_id(Record& record) {
  ByteView data = record.bytes();
  const uint16_t id = rd16(data, 0x28);
  wr16(data, 0x28, id + 1);
  return id;
}

bool NtfsFileSystem::build_resident(uint32_t type, const char* name, uint16_t flags, uint16_t id, ConstByteView value, bool indexed, ByteBuffer* out) {
  uint32_t name_length = 0;
  while (name && name[name_length]) ++name_length;
  const uint32_t value_offset = align8(0x18 + 2 * name_length);
  const uint32_t length = align8(value_offset + static_cast<uint32_t>(value.size()));
  out->clear();
  if (!out->put_zeros(length)) return false;
  ByteView a = out->writable_view();
  wr32(a, 0, type);
  wr32(a, 4, length);
  a[8] = 0;
  a[9] = static_cast<uint8_t>(name_length);
  wr16(a, 10, name_length ? 0x18 : 0);
  wr16(a, 12, flags);
  wr16(a, 14, id);
  wr32(a, 16, static_cast<uint32_t>(value.size()));
  wr16(a, 20, value_offset);
  a[22] = indexed ? 1 : 0;
  for (uint32_t i = 0; i < name_length; ++i) wr16(a, 0x18 + 2 * i, static_cast<uint8_t>(name[i]));
  if (!value.empty()) a.subview(value_offset, value.size()).copy_from(value);
  return true;
}

// Makes a resident attribute's value `new_length` bytes long (new bytes are zeros), moving what follows it.
Status NtfsFileSystem::resident_resize(Record& record, const Attr& attr, uint32_t new_length) {
  ByteView data = record.bytes();
  const uint32_t used = rd32(data, 0x18), allocated = rd32(data, 0x1C);
  const uint32_t value_rel = attr.value_offset - attr.offset;
  if (new_length > 0xFFFFFF) return Status::NoSpace;
  const uint32_t new_total = align8(value_rel + new_length);
  if (new_total > attr.length && new_total - attr.length > allocated - used) return Status::NoSpace;
  const uint32_t end = attr.offset + attr.length;
  const uint32_t new_end = attr.offset + new_total;
  const uint32_t old_value_end = attr.value_offset + attr.value_length;
  if (new_end != end) {
    data.subview(new_end, used - end).copy_from(ConstByteView(data).subview(end, used - end));
    if (new_end < end) data.subview(used - (end - new_end), end - new_end).fill(0);
  }
  if (new_length > attr.value_length) data.subview(old_value_end, attr.offset + new_total - old_value_end).fill(0);
  else data.subview(attr.value_offset + new_length, attr.offset + new_total - (attr.value_offset + new_length)).fill(0);
  wr32(data, attr.offset + 4, new_total);
  wr32(data, attr.offset + 16, new_length);
  wr32(data, 0x18, used - attr.length + new_total);
  return Status::Ok;
}

Status NtfsFileSystem::record_init(Record& record, uint64_t number, uint16_t sequence, uint16_t flags, uint32_t record_size) {
  record.number = number;
  record.buffer.clear();
  if (!record.buffer.put_zeros(record_size)) return Status::OutOfMemory;
  ByteView data = record.bytes();
  const uint32_t usa_count = record_size / kFixupUnit + 1;
  const uint32_t first = align8(0x30 + 2 * usa_count);
  data[0] = 'F';
  data[1] = 'I';
  data[2] = 'L';
  data[3] = 'E';
  wr16(data, 4, 0x30);
  wr16(data, 6, usa_count);
  wr16(data, 0x10, sequence);
  wr16(data, 0x12, 0);
  wr16(data, 0x14, first);
  wr16(data, 0x16, flags);
  wr32(data, 0x18, first + 8);
  wr32(data, 0x1C, record_size);
  wr16(data, 0x28, 0);
  wr16(data, 0x2A, 0);
  wr32(data, 0x2C, static_cast<uint32_t>(number));
  wr16(data, 0x30, 1);  // the first update sequence number
  wr32(data, first, kAttrEnd);
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Streams

// The raw bytes of a record's $ATTRIBUTE_LIST.
Status NtfsFileSystem::load_attribute_list(const Record& base, ByteBuffer* list) {
  Attr a;
  Status s = attr_find(base, kAttrAttributeList, "", &a);
  if (s != Status::Ok) return s;
  list->clear();
  if (!a.nonresident) return list->append(base.bytes().subview(a.value_offset, a.value_length)) ? Status::Ok : Status::OutOfMemory;
  if (a.size > (uint64_t{1} << 24) || a.size == 0) return Status::Corrupt;
  Stream stream(allocator_);
  stream.record = base.number;
  stream.type = kAttrAttributeList;
  stream.resident = false;
  stream.size = stream.initialized = a.size;
  stream.allocated = a.allocated;
  s = decode_runs(base.bytes().subview(a.runs_offset, a.length - (a.runs_offset - a.offset)), 0, &stream.runs);
  if (s != Status::Ok) return s;
  if (!list->put_zeros(a.size)) return Status::OutOfMemory;
  return read_runs(stream.runs, 0, list->writable_view());
}

Status NtfsFileSystem::stream_open_number(uint64_t record, uint32_t type, const char* name, Stream* stream) {
  Record base(allocator_);
  const Status s = read_record(record, &base);
  if (s != Status::Ok) return s;
  return stream_open(base, type, name, stream);
}

// Opens the stream of `type` and `name` ("" for the unnamed one) of the file whose base record is `base`.
Status NtfsFileSystem::stream_open(const Record& base, uint32_t type, const char* name, Stream* stream) {
  stream->record = base.number;
  stream->type = type;
  stream->name[0] = 0;
  for (uint32_t i = 0; name && name[i]; ++i) {
    if (i + 1 >= sizeof stream->name) return Status::InvalidArgument;
    stream->name[i] = name[i];
    stream->name[i + 1] = 0;
  }
  stream->runs.clear();
  stream->in_base = true;
  Attr first;
  Status s = attr_find(base, type, name, &first);
  Attr list_attr;
  const bool has_list = attr_find(base, kAttrAttributeList, "", &list_attr) == Status::Ok;
  if (s != Status::Ok && !(s == Status::NotFound && has_list)) return s;
  if (s == Status::Ok && !first.nonresident) {
    stream->resident = true;
    stream->flags = first.flags;
    stream->size = stream->initialized = first.value_length;
    stream->allocated = 0;
    return Status::Ok;
  }
  // Non-resident: the first extent carries the sizes, later ones (possibly in other records) only runs.
  ByteBuffer list(allocator_);
  uint64_t next_vcn = 0;
  bool have_first = false;
  if (!has_list) {
    stream->resident = false;
    stream->flags = first.flags;
    stream->compression_shift = first.compression_shift;
    stream->size = first.size;
    stream->allocated = first.allocated;
    stream->initialized = first.initialized;
    return decode_runs(base.bytes().subview(first.runs_offset, first.length - (first.runs_offset - first.offset)), 0, &stream->runs);
  }
  s = load_attribute_list(base, &list);
  if (s != Status::Ok) return s;
  ConstByteView data = list.view();
  uint32_t at = 0;
  uint32_t visited = 0;
  while (at < data.size()) {
    if (data.size() - at < 0x1A) return Status::Corrupt;
    const uint32_t entry_type = rd32(data, at), length = rd16(data, at + 4), name_length = data[at + 6], name_at = data[at + 7];
    if (length < 0x1A || (length & 7) != 0 || length > data.size() - at) return Status::Corrupt;
    if (name_length && (name_at < 0x1A || name_at + 2u * name_length > length)) return Status::Corrupt;
    bool match = entry_type == type && name_length == [&] { uint32_t n = 0; while (name && name[n]) ++n; return n; }();
    for (uint32_t i = 0; match && i < name_length; ++i)
      if (rd16(data, at + name_at + 2 * i) != static_cast<uint8_t>(name[i])) match = false;
    if (match) {
      if (++visited > kMaxRuns) return Status::Corrupt;
      const uint64_t start_vcn = rd64(data, at + 8), ref = rd64(data, at + 16) & kRefMask;
      Record extension(allocator_);
      const Record* holder = &base;
      if (ref != base.number) {
        s = read_record(ref, &extension);
        if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
        holder = &extension;
        stream->in_base = false;
      }
      Attr part;
      s = attr_find_vcn(*holder, type, name, start_vcn, &part);
      if (s != Status::Ok) {
        if (s == Status::NotFound && start_vcn == 0 && ref == base.number && first.type == type && !first.nonresident) {
          stream->resident = true;
          stream->size = stream->initialized = first.value_length;
          return Status::Ok;
        }
        return s == Status::NotFound ? Status::Corrupt : s;
      }
      if (start_vcn == 0) {
        have_first = true;
        stream->resident = false;
        stream->flags = part.flags;
        stream->compression_shift = part.compression_shift;
        stream->size = part.size;
        stream->allocated = part.allocated;
        stream->initialized = part.initialized;
      }
      if (start_vcn != next_vcn) return Status::Corrupt;
      s = decode_runs(holder->bytes().subview(part.runs_offset, part.length - (part.runs_offset - part.offset)), start_vcn, &stream->runs);
      if (s != Status::Ok) return s;
      next_vcn = runs_end(stream->runs);
    }
    at += length;
  }
  if (!have_first) {
    if (s == Status::Ok && first.type == type && !first.nonresident) return Status::Ok;
    return Status::NotFound;
  }
  return Status::Ok;
}

// Reads the stream through its runs: the part past the initialized size and the holes read as zeros.
Status NtfsFileSystem::read_plain(const Stream& stream, uint64_t offset, ByteView out) {
  uint64_t valid = 0;  // bytes of `out` that come from the medium
  if (offset < stream.initialized) {
    valid = stream.initialized - offset;
    if (valid > out.size()) valid = out.size();
  }
  if (valid) {
    const Status s = read_runs(stream.runs, offset, out.first(valid));
    if (s != Status::Ok) return s;
  }
  if (valid < out.size()) out.subview(valid).fill(0);
  return Status::Ok;
}

Status NtfsFileSystem::stream_read(Stream& stream, uint64_t offset, ByteView out, uint64_t* transferred) {
  *transferred = 0;
  if (offset >= stream.size || out.empty()) return Status::Ok;
  uint64_t n = stream.size - offset;
  if (n > out.size()) n = out.size();
  out = out.first(n);
  if (stream.flags & kStreamEncrypted) return Status::Unsupported;
  Status s;
  if (stream.resident) {
    Record record(allocator_);
    s = read_record(stream.record, &record);
    if (s != Status::Ok) return s;
    Attr a;
    s = attr_find(record, stream.type, stream.name, &a);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (a.nonresident || offset > a.value_length || a.value_length - offset < n) return Status::Corrupt;
    out.copy_from(record.bytes().subview(a.value_offset + offset, n));
  } else if (stream.flags & kStreamCompressed) {
    s = read_compressed(stream, offset, out);
  } else {
    s = read_plain(stream, offset, out);
  }
  if (s == Status::Ok) *transferred = n;
  return s;
}

// Decompresses one LZNT1 compressed unit. `produced` is how many bytes came out (the rest of `out` is zeroed). False if the data is damaged.
bool NtfsFileSystem::lznt1_decompress(ConstByteView in, ByteView out, uint64_t* produced) {
  uint64_t src = 0, dst = 0;
  while (in.size() - src >= 2) {
    const uint32_t header = rd16(in, src);
    if (header == 0) break;
    const uint64_t payload = (header & 0xFFF) + 1;
    src += 2;
    if (payload > in.size() - src) return false;
    const uint64_t chunk_end = src + payload;
    const uint64_t chunk_start = dst;
    if (!(header & 0x8000)) {  // stored
      if (payload > out.size() - dst) return false;
      out.subview(dst, payload).copy_from(in.subview(src, payload));
      dst += payload;
      src = chunk_end;
      continue;
    }
    while (src < chunk_end) {
      const uint8_t flags = in[src++];
      for (uint32_t bit = 0; bit < 8 && src < chunk_end; ++bit) {
        if (!(flags & (1u << bit))) {
          if (dst >= out.size()) return false;
          out[dst++] = in[src++];
          continue;
        }
        if (chunk_end - src < 2) return false;
        const uint32_t token = rd16(in, src);
        src += 2;
        const uint64_t position = dst - chunk_start;
        uint32_t offset_bits = 4;
        while ((uint64_t{1} << offset_bits) < position) ++offset_bits;
        const uint32_t length_bits = 16 - offset_bits;
        const uint64_t length = (token & ((1u << length_bits) - 1)) + 3;
        const uint64_t distance = (token >> length_bits) + 1;
        if (distance > position || length > out.size() - dst) return false;
        for (uint64_t i = 0; i < length; ++i, ++dst) out[dst] = out[dst - distance];
      }
    }
  }
  *produced = dst;
  if (dst < out.size()) out.subview(dst).fill(0);
  return true;
}

// Reads compression unit number `unit` of `stream` (decompressed) into `out`, which is one unit long.
Status NtfsFileSystem::read_unit(const Stream& stream, uint64_t unit, ByteView out) {
  const uint64_t unit_clusters = uint64_t{1} << stream.compression_shift;
  const uint64_t first = unit * unit_clusters;
  uint64_t mapped = 0;  // clusters at the start of the unit that are stored
  bool tail_hole = false;
  for (uint64_t i = 0; i < unit_clusters; ++i) {
    const Run* run = run_for(stream.runs, first + i);
    const bool hole = !run || run->lcn == kHole;
    if (hole) tail_hole = true;
    else if (tail_hole) return Status::Corrupt;  // stored clusters after a hole
    else ++mapped;
  }
  if (mapped == 0 || first * cluster_bytes_ >= stream.initialized) {
    out.fill(0);
    return Status::Ok;
  }
  if (mapped == unit_clusters) return read_runs(stream.runs, first * cluster_bytes_, out);
  ByteBuffer packed(allocator_);
  if (!packed.put_zeros(mapped * cluster_bytes_)) return Status::OutOfMemory;
  const Status s = read_runs(stream.runs, first * cluster_bytes_, packed.writable_view());
  if (s != Status::Ok) return s;
  uint64_t produced = 0;
  return lznt1_decompress(packed.view(), out, &produced) ? Status::Ok : Status::Corrupt;
}

Status NtfsFileSystem::read_compressed(Stream& stream, uint64_t offset, ByteView out) {
  if (cluster_bytes_ > 4096 || stream.compression_shift == 0) return Status::Unsupported;
  const uint64_t unit_bytes = uint64_t{cluster_bytes_} << stream.compression_shift;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint64_t pos = offset + done, unit = pos / unit_bytes;
    if (unit_cache_unit_ != unit || unit_cache_record_ != stream.record || unit_cache_epoch_ != epoch_ || unit_cache_.size() != unit_bytes) {
      unit_cache_unit_ = ~uint64_t{0};
      unit_cache_.clear();
      if (!unit_cache_.put_zeros(unit_bytes)) return Status::OutOfMemory;
      const Status s = read_unit(stream, unit, unit_cache_.writable_view());
      if (s != Status::Ok) return s;
      unit_cache_unit_ = unit;
      unit_cache_record_ = stream.record;
      unit_cache_epoch_ = epoch_;
    }
    const uint64_t within = pos - unit * unit_bytes;
    uint64_t take = unit_bytes - within;
    if (take > out.size() - done) take = out.size() - done;
    out.subview(done, take).copy_from(unit_cache_.view().subview(within, take));
    done += take;
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Names

// The volume's collation: both names upper-cased through $UpCase, compared unit by unit, then by length.
int NtfsFileSystem::compare_names(Units a, ConstByteView key_name) const {
  const uint64_t a_units = a.size(), key_units = key_name.size() / 2;
  const uint64_t n = a_units < key_units ? a_units : key_units;
  for (uint64_t i = 0; i < n; ++i) {
    const uint32_t x = upcase(a[i]), y = upcase(rd16(key_name, 2 * i));
    if (x != y) return x < y ? -1 : 1;
  }
  return a_units == key_units ? 0 : a_units < key_units ? -1 : 1;
}

// Parses and validates the index entry at `offset` of `entries`.
bool NtfsFileSystem::entry_at(ConstByteView entries, uint32_t offset, Entry* entry) {
  if (offset > entries.size() || entries.size() - offset < 16) return false;
  Entry e;
  e.offset = offset;
  e.file_ref = rd64(entries, offset);
  e.length = rd16(entries, offset + 8);
  e.key_length = rd16(entries, offset + 10);
  e.flags = rd16(entries, offset + 12);
  if (e.length < 16 || (e.length & 7) != 0 || e.length > entries.size() - offset) return false;
  if (e.has_child()) {
    if (e.length < 24) return false;
    e.child = rd64(entries, offset + e.length - 8);
  }
  if (!e.last()) {
    const uint32_t room = e.length - 16 - (e.has_child() ? 8u : 0u);
    if (e.key_length < 0x42 || e.key_length > room) return false;
    if (0x42u + 2u * entries[offset + 16 + 0x40] > e.key_length) return false;
  } else if (offset + e.length != entries.size()) {
    return false;  // the end entry is the last
  }
  *entry = e;
  return true;
}

bool NtfsFileSystem::entry_name(ConstByteView entries, const Entry& entry, NameInfo* name) {
  if (entry.last()) return false;
  const uint32_t key = entry.offset + 16;
  name->parent = rd64(entries, key);
  name->file_flags = rd32(entries, key + 0x38);
  name->units = entries[key + 0x40];
  name->ns = entries[key + 0x41];
  ArrayView<uint16_t> chars(name->chars);
  for (uint32_t i = 0; i < name->units; ++i) chars[i] = rd16(entries, key + 0x42 + 2 * i);
  return true;
}

// UTF-8 to UTF-16 units; false for malformed UTF-8 or more than 255 units.
bool NtfsFileSystem::to_units(StringView text, ArrayView<uint16_t> buffer, Units* units) {
  uint64_t n = 0;
  uint64_t i = 0;
  while (i < text.size()) {
    const uint8_t b = static_cast<uint8_t>(text[i]);
    uint32_t cp, extra;
    if (b < 0x80) cp = b, extra = 0;
    else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1;
    else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2;
    else if ((b & 0xF8) == 0xF0) cp = b & 0x07, extra = 3;
    else return false;
    if (extra > text.size() - i - 1) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t c = static_cast<uint8_t>(text[i + k]);
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return false;
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;
    i += extra + 1;
    if (cp >= 0x10000) {
      if (n + 2 > kMaxNameLength || n + 2 > buffer.size()) return false;
      cp -= 0x10000;
      buffer[n++] = static_cast<uint16_t>(0xD800 + (cp >> 10));
      buffer[n++] = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
    } else {
      if (n + 1 > kMaxNameLength || n + 1 > buffer.size()) return false;
      buffer[n++] = static_cast<uint16_t>(cp);
    }
  }
  *units = Units(buffer).first(n);
  return n != 0;
}

// UTF-16 to UTF-8 (a lone surrogate becomes U+FFFD). False if it does not fit in a name.
bool NtfsFileSystem::units_to_name(Units units, FixedString<kMaxNameLength + 1>* out) {
  out->clear();
  const uint64_t count = units.size();
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t cp = units[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
    }
    if (!append_utf8(out, cp)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Folders: the index

Status NtfsFileSystem::index_open(uint64_t dir, IndexCtx* ctx) {
  Record record(allocator_);
  Status s = read_record(dir, &record);
  if (s != Status::Ok) return s;
  if (!(rd16(record.bytes(), 0x16) & kRecordDirectory)) return Status::NotADirectory;
  Attr root;
  s = attr_find(record, kAttrIndexRoot, "$I30", &root);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  if (root.nonresident || root.value_length < 0x20) return Status::Corrupt;
  ConstByteView value = record.bytes().subview(root.value_offset, root.value_length);
  ctx->dir = dir;
  ctx->block_size = rd32(value, 8);
  if (ctx->block_size < kFixupUnit || ctx->block_size > (1u << 20) || (ctx->block_size & (ctx->block_size - 1)) != 0) return Status::Corrupt;
  ctx->vcn_unit = cluster_bytes_ <= ctx->block_size ? cluster_bytes_ : kFixupUnit;
  s = stream_open(record, kAttrIndexAllocation, "$I30", &ctx->alloc);
  if (s == Status::Ok) {
    if (ctx->alloc.resident) return Status::Corrupt;
    ctx->has_alloc = true;
  } else if (s == Status::NotFound) {
    ctx->has_alloc = false;
  } else {
    return s;
  }
  s = stream_open(record, kAttrBitmap, "$I30", &ctx->bitmap);
  if (s == Status::Ok) ctx->has_bitmap = true;
  else if (s == Status::NotFound) ctx->has_bitmap = false;
  else return s;
  return Status::Ok;
}

// Loads the index root (`root`) or the block at `vcn`.
Status NtfsFileSystem::index_load(IndexCtx& ctx, bool root, uint64_t vcn, IndexNode* node) {
  node->entries.clear();
  node->root = root;
  node->vcn = vcn;
  if (root) {
    Record record(allocator_);
    Status s = read_record(ctx.dir, &record);
    if (s != Status::Ok) return s;
    Attr attr;
    s = attr_find(record, kAttrIndexRoot, "$I30", &attr);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (attr.nonresident || attr.value_length < 0x20) return Status::Corrupt;
    ConstByteView value = record.bytes().subview(attr.value_offset, attr.value_length);
    const uint32_t entries_offset = rd32(value, 0x10), total = rd32(value, 0x14), flags = rd32(value, 0x1C);
    if (entries_offset < 0x10 || (entries_offset & 7) != 0 || total < entries_offset || total > attr.value_length - 0x10) return Status::Corrupt;
    node->large = (flags & 1) != 0;
    node->capacity = 0xFFFFFFFFu;
    if (!node->entries.append(value.subview(0x10 + entries_offset, total - entries_offset))) return Status::OutOfMemory;
  } else {
    if (!ctx.has_alloc) return Status::Corrupt;
    if (vcn > UINT64_MAX / ctx.vcn_unit) return Status::Corrupt;
    const uint64_t offset = vcn * ctx.vcn_unit;
    if (offset > ctx.alloc.size || ctx.alloc.size - offset < ctx.block_size) return Status::Corrupt;
    ByteBuffer raw(allocator_);
    if (!raw.put_zeros(ctx.block_size)) return Status::OutOfMemory;
    uint64_t got = 0;
    Status s = stream_read(ctx.alloc, offset, raw.writable_view(), &got);
    if (s != Status::Ok) return s;
    ByteView data = raw.writable_view();
    if (data[0] != 'I' || data[1] != 'N' || data[2] != 'D' || data[3] != 'X') return Status::Corrupt;
    const uint32_t usa_offset = rd16(data, 4), usa_count = rd16(data, 6);
    if (usa_count != ctx.block_size / kFixupUnit + 1 || usa_offset < 0x28 || usa_offset + 2 * usa_count > ctx.block_size) return Status::Corrupt;
    const uint16_t usn = rd16(data, usa_offset);
    for (uint32_t i = 1; i < usa_count; ++i) {
      const uint32_t end = i * kFixupUnit - 2;
      if (rd16(data, end) != usn) return Status::Corrupt;
      wr16(data, end, rd16(data, usa_offset + 2 * i));
    }
    if (rd64(data, 0x10) != vcn) return Status::Corrupt;
    const uint32_t entries_offset = rd32(data, 0x18), total = rd32(data, 0x1C), allocated = rd32(data, 0x20), flags = rd32(data, 0x24);
    if (entries_offset < 0x10 || (entries_offset & 7) != 0 || total < entries_offset || allocated > ctx.block_size - 0x18 || total > allocated ||
        entries_offset + 0x18u < usa_offset + 2 * usa_count)
      return Status::Corrupt;
    node->large = (flags & 1) != 0;
    node->capacity = allocated - entries_offset;
    node->usn = usn;
    if (!node->entries.append(ConstByteView(data).subview(0x18 + entries_offset, total - entries_offset))) return Status::OutOfMemory;
  }
  // The entries must parse and end with the end entry.
  ConstByteView entries = node->entries.view();
  uint32_t at = 0;
  for (uint32_t guard = 0;; ++guard) {
    Entry entry;
    if (guard > (1u << 20) || !entry_at(entries, at, &entry)) return Status::Corrupt;
    if (entry.last()) break;
    at += entry.length;
  }
  return Status::Ok;
}

// Walks down from the root to the entry named `name`, or to where it would be.
Status NtfsFileSystem::index_search(IndexCtx& ctx, Units name, ArrayView<Step> steps, uint32_t* depth, bool* found) {
  IndexNode node(allocator_);
  bool root = true;
  uint64_t vcn = 0;
  *found = false;
  for (uint32_t level = 0; level < kMaxDepth; ++level) {
    Status s = index_load(ctx, root, vcn, &node);
    if (s != Status::Ok) return s;
    ConstByteView entries = node.entries.view();
    uint32_t at = 0, ordinal = 0;
    bool descend = false;
    uint64_t child = 0;
    for (;; ++ordinal) {
      Entry entry;
      if (!entry_at(entries, at, &entry)) return Status::Corrupt;
      int order = -1;  // the end entry is greater than every name
      if (!entry.last()) order = compare_names(name, entries.subview(entry.offset + 16 + 0x42, 2u * entries[entry.offset + 16 + 0x40]));
      if (order == 0) {
        steps[level].root = root;
        steps[level].vcn = vcn;
        steps[level].ordinal = ordinal;
        *depth = level + 1;
        *found = true;
        return Status::Ok;
      }
      if (order < 0) {
        descend = entry.has_child();
        child = entry.child;
        steps[level].root = root;
        steps[level].vcn = vcn;
        steps[level].ordinal = ordinal;
        break;
      }
      at += entry.length;
    }
    if (!descend) {
      *depth = level + 1;
      return Status::Ok;
    }
    root = false;
    vcn = child;
  }
  return Status::Corrupt;
}

// Finds `name` in folder `dir`: the file reference and the entry's name.
Status NtfsFileSystem::lookup(uint64_t dir, Units name, uint64_t* file_ref, NameInfo* key) {
  IndexCtx ctx(allocator_);
  Status s = index_open(dir, &ctx);
  if (s != Status::Ok) return s;
  Step step_storage[kMaxDepth];
  ArrayView<Step> steps(step_storage);
  uint32_t depth = 0;
  bool found = false;
  s = index_search(ctx, name, steps, &depth, &found);
  if (s != Status::Ok) return s;
  if (!found) return Status::NotFound;
  IndexNode node(allocator_);
  s = index_load(ctx, steps[depth - 1].root, steps[depth - 1].vcn, &node);
  if (s != Status::Ok) return s;
  uint32_t offset = 0;
  Entry entry;
  if (!node_offset(node, steps[depth - 1].ordinal, &offset, &entry)) return Status::Corrupt;
  *file_ref = entry.file_ref;
  if (key) entry_name(node.entries.view(), entry, key);
  return Status::Ok;
}

// The offset and parsed form of the entry with the given ordinal.
bool NtfsFileSystem::node_offset(const IndexNode& node, uint32_t ordinal, uint32_t* offset, Entry* entry) {
  ConstByteView entries = node.entries.view();
  uint32_t at = 0;
  for (uint32_t i = 0;; ++i) {
    Entry e;
    if (!entry_at(entries, at, &e)) return false;
    if (i == ordinal) {
      *offset = at;
      *entry = e;
      return true;
    }
    if (e.last()) return false;
    at += e.length;
  }
}

uint32_t NtfsFileSystem::node_count(const IndexNode& node) {
  ConstByteView entries = node.entries.view();
  uint32_t at = 0, n = 0;
  for (;; ++n) {
    Entry e;
    if (!entry_at(entries, at, &e)) return n;
    if (e.last()) return n + 1;
    at += e.length;
  }
}

// Steps through a folder's entries in name order (sub-nodes before the entry they precede). Entries that are not listed - 8.3 aliases and
// system files - are skipped.
Status NtfsFileSystem::dir_next(uint64_t dir, Cursor* cursor, uint64_t* number, NameInfo* name) {
  if (cursor->ended) return Status::EndOfDirectory;
  IndexCtx ctx(allocator_);
  Status s = index_open(dir, &ctx);
  if (s != Status::Ok) return s;
  if (!cursor->started) {
    cursor->started = true;
    cursor->depth = 1;
    cursor->frames()[0] = Cursor::Frame();
  }
  IndexNode node(allocator_);
  for (uint32_t guard = 0; guard < (1u << 24); ++guard) {
    if (cursor->depth == 0) {
      cursor->ended = true;
      return Status::EndOfDirectory;
    }
    Cursor::Frame& frame = cursor->frames()[cursor->depth - 1];
    s = index_load(ctx, frame.root, frame.vcn, &node);
    if (s != Status::Ok) return s;
    uint32_t offset = 0;
    Entry entry;
    if (!node_offset(node, frame.ordinal, &offset, &entry)) return Status::Corrupt;
    if (entry.has_child() && !frame.child_done) {
      frame.child_done = true;
      if (cursor->depth >= kMaxDepth) return Status::Corrupt;
      Cursor::Frame& child = cursor->frames()[cursor->depth++];
      child = Cursor::Frame();
      child.root = false;
      child.vcn = entry.child;
      continue;
    }
    if (entry.last()) {
      --cursor->depth;
      continue;
    }
    NameInfo info;
    entry_name(node.entries.view(), entry, &info);
    frame.ordinal++;
    frame.child_done = false;
    const uint64_t record = entry.file_ref & kRefMask;
    if (info.ns == kNameDos || record < kFirstUserRecord) continue;
    *number = record;
    *name = info;
    return Status::Ok;
  }
  return Status::Corrupt;
}

Status NtfsFileSystem::directory_empty(uint64_t dir, bool* empty) {
  Cursor cursor;
  uint64_t number = 0;
  NameInfo name;
  const Status s = dir_next(dir, &cursor, &number, &name);
  if (s == Status::EndOfDirectory) {
    *empty = true;
    return Status::Ok;
  }
  *empty = false;
  return s;
}

// Fills a DirectoryEntry from the file's record (the sizes in the folder's index can be out of date).
Status NtfsFileSystem::describe(uint64_t number, const NameInfo& name, DirectoryEntry* entry) {
  Record record(allocator_);
  Status s = read_record(number, &record);
  if (s != Status::Ok) return s;
  DirectoryEntry result;
  if (!units_to_name(name.view(), &result.name)) return Status::NotFound;  // (a name that does not fit is left out of listings)
  const bool directory = (rd16(record.bytes(), 0x16) & kRecordDirectory) != 0;
  result.type = directory ? NodeType::Directory : NodeType::File;
  uint32_t attributes = name.file_flags;
  Attr a;
  if (attr_find(record, kAttrStandardInformation, "", &a) == Status::Ok && !a.nonresident && a.value_length >= 0x24)
    attributes = rd32(record.bytes(), a.value_offset + 0x20);
  result.read_only = (attributes & kFileReadOnly) != 0;
  if (!directory) {
    Stream data(allocator_);
    s = stream_open(record, kAttrData, "", &data);
    if (s == Status::NotFound) result.size = 0;
    else if (s != Status::Ok) return s;
    else result.size = data.size;
  }
  *entry = result;
  return Status::Ok;
}

// Walks `path` from the root.
Status NtfsFileSystem::resolve(StringView path, Resolved* result) {
  if (block_.empty()) return Status::InvalidArgument;
  Resolved current;
  current.number = current.parent = kRecRoot;
  current.directory = true;
  StringView rest = path, component;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.size() > kMaxNameLength) return Status::InvalidPath;
    if (!current.directory) return Status::NotADirectory;
    uint16_t buffer[kMaxNameLength];
    Units units;
    if (!to_units(component, ArrayView<uint16_t>(buffer), &units)) return Status::InvalidPath;
    uint64_t ref = 0;
    NameInfo key;
    Status s = lookup(current.number, units, &ref, &key);
    if (s != Status::Ok) return s;
    const uint64_t number = ref & kRefMask;
    if (number < kFirstUserRecord) return Status::NotFound;  // the system files are not part of the tree
    Record record(allocator_);
    s = read_record(number, &record);
    if (s == Status::NotFound) return Status::NotFound;
    if (s != Status::Ok) return s;
    Resolved next;
    next.parent = current.number;
    next.number = number;
    next.name = key;
    next.directory = (rd16(record.bytes(), 0x16) & kRecordDirectory) != 0;
    current = next;
  }
  *result = current;
  return Status::Ok;
}

Status NtfsFileSystem::split(StringView path, Resolved* parent, StringView* leaf) {
  StringView parent_path;
  if (!split_parent(path, &parent_path, leaf)) return Status::InvalidPath;
  const Status s = resolve(parent_path, parent);
  if (s != Status::Ok) return s;
  return parent->directory ? Status::Ok : Status::NotADirectory;
}

bool NtfsFileSystem::is_open(uint64_t number) const {
  for (const OpenHandle* h = open_; h; h = h->next)
    if (h->number == number) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The filesystem interface

FileSystemInfo NtfsFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign("NTFS");
  result.label = label_;
  result.total_bytes = cluster_count_ * cluster_bytes_;
  result.read_only = !writable_;
  if (writable_) {
    kernel::ScopedRecursiveMutex lock(mutex_);
    result.free_bytes = const_cast<NtfsFileSystem*>(this)->free_cluster_count() * cluster_bytes_;
  }
  return result;
}

Status NtfsFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Resolved r;
  Status s = resolve(path, &r);
  if (s != Status::Ok) return s;
  if (r.number == kRecRoot && r.parent == kRecRoot && r.name.units == 0) {
    DirectoryEntry root;
    root.type = NodeType::Directory;
    *entry = root;
    return Status::Ok;
  }
  return describe(r.number, r.name, entry);
}

Status NtfsFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Resolved r;
  Status s = resolve(path, &r);
  if (s != Status::Ok) return s;
  if (!r.directory) return Status::NotADirectory;
  UniquePointer<IDirectory> handle = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, r.number);
  if (!handle) return Status::OutOfMemory;
  *directory = move(handle);
  return Status::Ok;
}

Status NtfsFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  const bool write = mode != OpenMode::Read;
  if (write && !writable_) return Status::ReadOnly;
  Resolved r;
  Status s = resolve(path, &r);
  uint64_t number = 0;
  if (s == Status::NotFound && (mode == OpenMode::CreateOrOpen || mode == OpenMode::CreateTruncate)) {
    s = create_node(path, false, &number);
    if (s != Status::Ok) return s;
  } else if (s != Status::Ok) {
    return s;
  } else {
    if (r.directory) return Status::IsADirectory;
    number = r.number;
    Record record(allocator_);
    s = read_record(number, &record);
    if (s != Status::Ok) return s;
    Attr a;
    if (attr_find(record, kAttrStandardInformation, "", &a) == Status::Ok && !a.nonresident && a.value_length >= 0x24 && write &&
        (rd32(record.bytes(), a.value_offset + 0x20) & kFileReadOnly))
      return Status::ReadOnly;
    Stream data(allocator_);
    s = stream_open(record, kAttrData, "", &data);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (data.flags & kStreamEncrypted) return Status::Unsupported;
    if (write && (data.flags & kStreamCompressed)) return Status::Unsupported;
  }
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, number, write);
  if (!handle) return Status::OutOfMemory;
  if (mode == OpenMode::CreateTruncate) {
    const Status t = handle->resize(0);
    if (t != Status::Ok) return t;
  }
  *file = move(handle);
  return Status::Ok;
}

Status NtfsFileSystem::flush() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (dirty_set_) {
    const Status s = finish_write();
    if (s != Status::Ok) return s;
  }
  return device_.flush() ? Status::Ok : Status::IoError;
}

}  // namespace filesystem
