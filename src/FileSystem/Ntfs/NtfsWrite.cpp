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

// The writing half of NtfsFileSystem: cluster and file record allocation, growing and cutting streams, changing the folders' B+trees,
// and creating, removing and renaming. Nothing here is journaled (see the class comment); every change builds the new state of a record
// in memory and writes it once.
#include "FileSystem/Ntfs/NtfsFileSystem.hpp"
#include "FileSystem/Ntfs/NtfsFormat.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

using namespace ntfs;
using util::move;

namespace {

// Where the entries of an index block start (from the node header at byte 0x18), as Windows lays them out.
uint32_t block_entries_offset(uint32_t block_size) { return align8(0x28 + 2 * (block_size / kFixupUnit + 1)) - 0x18; }

// Replaces `remove` bytes at `at` with `insert`.
bool splice(ByteBuffer* buffer, uint32_t at, uint32_t remove, ConstByteView insert, allocators::IAllocator& allocator) {
  ConstByteView view = buffer->view();
  if (at > view.size() || remove > view.size() - at) return false;
  ByteBuffer next(allocator);
  if (!next.append(view.first(at)) || !next.append(insert) || !next.append(view.subview(at + remove))) return false;
  buffer->clear();
  return buffer->append(next.view());
}

// An index entry: file reference, key, optional sub-node pointer.
bool make_entry(ByteBuffer* out, uint64_t file_ref, ConstByteView key, bool has_child, uint64_t child, bool last) {
  const uint32_t length = align8(16 + static_cast<uint32_t>(key.size()) + (has_child ? 8 : 0));
  out->clear();
  if (!out->put_zeros(length)) return false;
  ByteView e = out->writable_view();
  wr64(e, 0, file_ref);
  wr16(e, 8, length);
  wr16(e, 10, static_cast<uint32_t>(key.size()));
  wr16(e, 12, (has_child ? kEntryChild : 0) | (last ? kEntryLast : 0));
  if (!key.empty()) e.subview(16, key.size()).copy_from(key);
  if (has_child) wr64(e, length - 8, child);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Clean and dirty

Status NtfsFileSystem::touch_volume_flag(bool dirty) {
  Record record(allocator_);
  Status s = read_record(kRecVolume, &record);
  if (s != Status::Ok) return s;
  Attr a;
  if (attr_find(record, kAttrVolumeInformation, "", &a) != Status::Ok || a.nonresident || a.value_length < 12) return Status::Corrupt;
  const uint32_t flags = rd16(record.bytes(), a.value_offset + 10);
  wr16(record.bytes(), a.value_offset + 10, dirty ? (flags | kVolumeDirty) : (flags & ~kVolumeDirty));
  return write_record(record);
}

// Called before anything changes: the volume is marked dirty until flush().
Status NtfsFileSystem::begin_write() {
  if (!writable_) return Status::ReadOnly;
  if (!dirty_set_) {
    const Status s = touch_volume_flag(true);
    if (s != Status::Ok) return s;
    dirty_set_ = true;
  }
  return Status::Ok;
}

Status NtfsFileSystem::finish_write() {
  if (!dirty_set_) return Status::Ok;
  const Status s = touch_volume_flag(false);
  if (s == Status::Ok) dirty_set_ = false;
  return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Clusters

// The first bit in [from, to) of the cluster bitmap that is set (`want_set`) or clear; `to` if there is none.
Status NtfsFileSystem::bitmap_scan(uint64_t from, uint64_t to, bool want_set, uint64_t* found) {
  uint8_t storage[4096];
  ByteView buffer(storage);
  uint64_t bit = from;
  while (bit < to) {
    const uint64_t first_byte = bit / 8, last_byte = (to + 7) / 8;
    uint64_t n = last_byte - first_byte;
    if (n > buffer.size()) n = buffer.size();
    if (first_byte + n > bitmap_size_) return Status::Corrupt;
    const Status s = read_runs(bitmap_runs_, first_byte, buffer.first(n));
    if (s != Status::Ok) return s;
    for (uint64_t i = 0; i < n; ++i) {
      const uint8_t b = buffer[i];
      const uint64_t base = (first_byte + i) * 8;
      if (want_set ? b == 0 : b == 0xFF) {
        bit = base + 8;
        continue;
      }
      for (uint64_t pos = bit > base ? bit : base; pos < base + 8; ++pos) {
        if (pos >= to) {
          *found = to;
          return Status::Ok;
        }
        if (((b >> (pos - base)) & 1) == (want_set ? 1u : 0u)) {
          *found = pos;
          return Status::Ok;
        }
      }
      bit = base + 8;
    }
  }
  *found = to;
  return Status::Ok;
}

// Marks clusters used or free. Changing a bit that already has the value (a double allocation or free) is Corrupt and changes nothing more.
Status NtfsFileSystem::bitmap_update(uint64_t first, uint64_t count, bool used) {
  if (count == 0) return Status::Ok;
  if (first >= cluster_count_ || count > cluster_count_ - first) return Status::Corrupt;
  uint8_t storage[4096];
  ByteView buffer(storage);
  uint64_t pos = first;
  const uint64_t end = first + count;
  while (pos < end) {
    const uint64_t byte0 = pos / 8;
    uint64_t chunk_end = (byte0 + buffer.size()) * 8;
    if (chunk_end > end) chunk_end = end;
    const uint64_t n = (chunk_end + 7) / 8 - byte0;
    if (byte0 + n > bitmap_size_) return Status::Corrupt;
    Status s = read_runs(bitmap_runs_, byte0, buffer.first(n));
    if (s != Status::Ok) return s;
    for (uint64_t b = pos; b < chunk_end; ++b) {
      uint8_t& cell = buffer[b / 8 - byte0];
      const uint8_t mask = static_cast<uint8_t>(1u << (b % 8));
      if (((cell & mask) != 0) == used) return Status::Corrupt;
      cell ^= mask;
    }
    s = write_runs(bitmap_runs_, byte0, ConstByteView(buffer).first(n));
    if (s != Status::Ok) return s;
    pos = chunk_end;
  }
  if (free_known_) free_clusters_ = used ? free_clusters_ - count : free_clusters_ + count;
  return Status::Ok;
}

// Takes the first free run at or after `hint` (wrapping around once), at most `want` clusters long.
Status NtfsFileSystem::allocate_run(uint64_t hint, uint64_t want, uint64_t* lcn, uint64_t* got) {
  if (want == 0) return Status::InvalidArgument;
  if (hint >= cluster_count_) hint = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const uint64_t from = pass == 0 ? hint : 0, to = pass == 0 ? cluster_count_ : hint;
    if (from >= to) continue;
    uint64_t first = 0;
    Status s = bitmap_scan(from, to, false, &first);
    if (s != Status::Ok) return s;
    if (first == to) continue;
    uint64_t stop = first + want < to ? first + want : to, end = 0;
    s = bitmap_scan(first, stop, true, &end);
    if (s != Status::Ok) return s;
    s = bitmap_update(first, end - first, true);
    if (s != Status::Ok) return s;
    *lcn = first;
    *got = end - first;
    alloc_hint_ = end;
    return Status::Ok;
  }
  return Status::NoSpace;
}

uint64_t NtfsFileSystem::free_cluster_count() {
  if (!free_known_) {
    if (cluster_count_ > (uint64_t{1} << 26)) return 0;  // scanning a huge bitmap would stall a status line: "unknown"
    uint64_t free = 0;
    uint8_t storage[4096];
    ByteView buffer(storage);
    for (uint64_t byte = 0; byte * 8 < cluster_count_;) {
      uint64_t n = (cluster_count_ + 7) / 8 - byte;
      if (n > buffer.size()) n = buffer.size();
      if (byte + n > bitmap_size_ || read_runs(bitmap_runs_, byte, buffer.first(n)) != Status::Ok) return 0;
      for (uint64_t i = 0; i < n; ++i)
        for (uint32_t k = 0; k < 8; ++k)
          if ((byte + i) * 8 + k < cluster_count_ && !((buffer[i] >> k) & 1)) ++free;
      byte += n;
    }
    free_clusters_ = free;
    free_known_ = true;
  }
  return free_clusters_;
}

Status NtfsFileSystem::free_runs(const RunList& runs) {
  Status result = Status::Ok;
  for (const Run& run : runs.view()) {
    if (run.lcn == kHole) continue;
    const Status s = bitmap_update(run.lcn, run.length, false);
    if (s != Status::Ok) result = s;
  }
  return result;
}

// Appends `extra` clusters to the end of `runs` (next to the last run if it can). The runs allocated are also listed in `fresh`.
Status NtfsFileSystem::extend_runs(RunList* runs, uint64_t extra, RunList* fresh) {
  uint64_t vcn = runs_end(*runs), hint = alloc_hint_;
  if (!runs->view().empty()) {
    const Run& last = runs->view()[runs->count() - 1];
    if (last.lcn != kHole) hint = last.lcn + last.length;
  }
  while (extra) {
    uint64_t lcn = 0, got = 0;
    Status s = allocate_run(hint, extra, &lcn, &got);
    if (s != Status::Ok) {
      free_runs(*fresh);
      return s;
    }
    Run run;
    run.vcn = vcn;
    run.length = got;
    run.lcn = lcn;
    if (!fresh->push_back(run)) {
      bitmap_update(lcn, got, false);
      free_runs(*fresh);
      return Status::OutOfMemory;
    }
    bool merged = false;
    if (!runs->view().empty()) {
      Run& last = runs->operator[](runs->count() - 1);
      if (last.lcn != kHole && last.lcn + last.length == lcn) {
        last.length += got;
        merged = true;
      }
    }
    if (!merged && !runs->push_back(run)) {
      free_runs(*fresh);
      return Status::OutOfMemory;
    }
    vcn += got;
    extra -= got;
    hint = lcn + got;
  }
  return Status::Ok;
}

// Cuts `runs` to the first `keep` clusters; what was cut off goes to `freed` (not yet marked free).
Status NtfsFileSystem::cut_runs(RunList* runs, uint64_t keep, RunList* freed) {
  while (runs->count() > 0) {
    Run& last = runs->operator[](runs->count() - 1);
    if (last.vcn + last.length <= keep) break;
    if (last.vcn >= keep) {
      if (last.lcn != kHole && !freed->push_back(last)) return Status::OutOfMemory;
      runs->remove_at(runs->count() - 1);
      continue;
    }
    const uint64_t kept = keep - last.vcn;
    if (last.lcn != kHole) {
      Run cut;
      cut.vcn = keep;
      cut.length = last.length - kept;
      cut.lcn = last.lcn + kept;
      if (!freed->push_back(cut)) return Status::OutOfMemory;
    }
    last.length = kept;
    break;
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Streams

// Writes a non-resident stream's sizes and runs back to its attribute in the base record.
Status NtfsFileSystem::stream_store(Stream& stream) {
  Record record(allocator_);
  Status s = read_record(stream.record, &record);
  if (s != Status::Ok) return s;
  Attr a;
  s = attr_find(record, stream.type, stream.name, &a);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  if (!a.nonresident) return Status::Corrupt;
  const uint32_t header = a.runs_offset - a.offset;
  ByteBuffer pairs(allocator_);
  if (!encode_runs(stream.runs, &pairs)) return Status::OutOfMemory;
  const uint32_t length = align8(header + static_cast<uint32_t>(pairs.size()));
  ByteBuffer next(allocator_);
  if (!next.append(record.bytes().subview(a.offset, header)) || !next.append(pairs.view()) || !next.put_zeros(length - header - pairs.size()))
    return Status::OutOfMemory;
  ByteView n = next.writable_view();
  const uint64_t clusters = runs_end(stream.runs);
  wr32(n, 4, length);
  wr64(n, 0x18, clusters - 1);  // (all ones for an empty stream)
  wr64(n, 0x28, clusters * cluster_bytes_);
  wr64(n, 0x30, stream.size);
  wr64(n, 0x38, stream.initialized);
  if (header >= 0x48 && (a.flags & (kStreamCompressed | kStreamSparse))) {  // (only compressed and sparse streams have the stored size)
    uint64_t stored = 0;
    for (const Run& run : stream.runs.view())
      if (run.lcn != kHole) stored += run.length;
    wr64(n, 0x40, stored * cluster_bytes_);
  }
  s = attr_replace(record, a, next.view());
  if (s != Status::Ok) return s;
  return write_record(record);
}

// Turns a resident stream into a non-resident one that holds the same bytes.
Status NtfsFileSystem::stream_convert(Stream& stream) {
  Record record(allocator_);
  Status s = read_record(stream.record, &record);
  if (s != Status::Ok) return s;
  Attr a;
  s = attr_find(record, stream.type, stream.name, &a);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  if (a.nonresident) return Status::Corrupt;
  const uint64_t clusters = (a.value_length + cluster_bytes_ - 1) / cluster_bytes_;
  RunList runs(allocator_), fresh(allocator_);
  if (clusters) {
    s = extend_runs(&runs, clusters, &fresh);
    if (s != Status::Ok) return s;
    ByteBuffer padded(allocator_);
    if (!padded.append(record.bytes().subview(a.value_offset, a.value_length)) || !padded.put_zeros(clusters * cluster_bytes_ - a.value_length)) {
      free_runs(fresh);
      return Status::OutOfMemory;
    }
    s = write_runs(runs, 0, padded.view());
    if (s != Status::Ok) {
      free_runs(fresh);
      return s;
    }
  }
  ByteBuffer pairs(allocator_);
  uint32_t name_length = a.name_length;
  if (!encode_runs(runs, &pairs)) {
    free_runs(fresh);
    return Status::OutOfMemory;
  }
  const uint32_t header = align8(0x40 + 2 * name_length);
  const uint32_t length = align8(header + static_cast<uint32_t>(pairs.size()));
  ByteBuffer next(allocator_);
  if (!next.put_zeros(header) || !next.append(pairs.view()) || !next.put_zeros(length - header - pairs.size())) {
    free_runs(fresh);
    return Status::OutOfMemory;
  }
  ByteView n = next.writable_view();
  wr32(n, 0, a.type);
  wr32(n, 4, length);
  n[8] = 1;
  n[9] = static_cast<uint8_t>(name_length);
  wr16(n, 10, name_length ? 0x40 : 0);
  wr16(n, 12, a.flags);
  wr16(n, 14, a.id);
  wr64(n, 0x10, 0);
  wr64(n, 0x18, clusters - 1);
  wr16(n, 0x20, header);
  wr64(n, 0x28, clusters * cluster_bytes_);
  wr64(n, 0x30, a.value_length);
  wr64(n, 0x38, a.value_length);
  for (uint32_t i = 0; i < name_length; ++i) wr16(n, 0x40 + 2 * i, rd16(record.bytes(), a.name_offset + 2 * i));
  s = attr_replace(record, a, next.view());
  if (s == Status::Ok) s = write_record(record);
  if (s != Status::Ok) {
    free_runs(fresh);
    return s;
  }
  stream.resident = false;
  stream.size = stream.initialized = a.value_length;
  stream.allocated = clusters * cluster_bytes_;
  return copy_runs(runs, &stream.runs);
}

// Sets the length of a stream: cutting it, or adding zeros (cheaply: past the initialized size reads as zeros; `initialize` writes them
// out, which the metadata streams need).
Status NtfsFileSystem::stream_resize(Stream& stream, uint64_t new_size, bool initialize) {
  if (stream.flags & (kStreamCompressed | kStreamEncrypted)) return Status::Unsupported;
  if (!stream.in_base) return Status::Unsupported;
  if (stream.resident) {
    if (new_size == stream.size) return Status::Ok;
    Record record(allocator_);
    Status s = read_record(stream.record, &record);
    if (s != Status::Ok) return s;
    Attr a;
    s = attr_find(record, stream.type, stream.name, &a);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (a.nonresident) return Status::Corrupt;
    s = new_size > 0xFFFFFF ? Status::NoSpace : resident_resize(record, a, static_cast<uint32_t>(new_size));
    if (s == Status::Ok) {
      s = write_record(record);
      if (s == Status::Ok) stream.size = stream.initialized = new_size;
      return s;
    }
    if (s != Status::NoSpace || (stream.type != kAttrData && stream.type != kAttrBitmap)) return s;
    s = stream_convert(stream);
    if (s != Status::Ok) return s;
  }
  if (new_size == stream.size && !initialize) return Status::Ok;
  RunList saved(allocator_), fresh(allocator_), freed(allocator_);
  Status s = copy_runs(stream.runs, &saved);
  if (s != Status::Ok) return s;
  const uint64_t old_size = stream.size, old_initialized = stream.initialized, old_allocated = stream.allocated;
  const uint64_t have = runs_end(stream.runs), need = (new_size + cluster_bytes_ - 1) / cluster_bytes_;
  if (need > have) s = extend_runs(&stream.runs, need - have, &fresh);
  else if (need < have) s = cut_runs(&stream.runs, need, &freed);
  if (s == Status::Ok && initialize && new_size > stream.initialized) s = zero_runs(stream.runs, stream.initialized, new_size);
  if (s == Status::Ok) {
    stream.size = new_size;
    stream.allocated = need * cluster_bytes_;
    stream.initialized = initialize ? new_size : (stream.initialized < new_size ? stream.initialized : new_size);
    s = stream_store(stream);
  }
  if (s != Status::Ok) {
    if (!fresh.view().empty()) free_runs(fresh);
    copy_runs(saved, &stream.runs);
    stream.size = old_size;
    stream.initialized = old_initialized;
    stream.allocated = old_allocated;
    return s;
  }
  return free_runs(freed);
}

// Gives clusters to the holes in [first_vcn, last_vcn] of a sparse stream.
Status NtfsFileSystem::fill_holes(Stream& stream, uint64_t first_vcn, uint64_t last_vcn) {
  RunList saved(allocator_), fresh(allocator_);
  Status s = copy_runs(stream.runs, &saved);
  if (s != Status::Ok) return s;
  uint64_t vcn = first_vcn;
  while (s == Status::Ok && vcn <= last_vcn) {
    const Run* run = run_for(stream.runs, vcn);
    if (!run) {
      s = Status::Corrupt;
      break;
    }
    const uint64_t run_end = run->vcn + run->length;
    if (run->lcn != kHole) {
      vcn = run_end;
      continue;
    }
    uint64_t want = (run_end < last_vcn + 1 ? run_end : last_vcn + 1) - vcn;
    uint64_t lcn = 0, got = 0;
    s = allocate_run(alloc_hint_, want, &lcn, &got);
    if (s != Status::Ok) break;
    Run mapped;
    mapped.vcn = vcn;
    mapped.length = got;
    mapped.lcn = lcn;
    if (!fresh.push_back(mapped)) {
      bitmap_update(lcn, got, false);
      s = Status::OutOfMemory;
      break;
    }
    // Rebuild the list with the hole split around the new run.
    RunList next(allocator_);
    for (const Run& r : stream.runs.view()) {
      if (r.vcn != run->vcn) {
        if (!next.push_back(r)) s = Status::OutOfMemory;
        continue;
      }
      if (vcn > r.vcn) {
        Run before = r;
        before.length = vcn - r.vcn;
        if (!next.push_back(before)) s = Status::OutOfMemory;
      }
      if (!next.push_back(mapped)) s = Status::OutOfMemory;
      if (vcn + got < r.vcn + r.length) {
        Run after = r;
        after.vcn = vcn + got;
        after.length = r.vcn + r.length - (vcn + got);
        if (!next.push_back(after)) s = Status::OutOfMemory;
      }
    }
    if (s != Status::Ok) break;
    s = copy_runs(next, &stream.runs);
    if (s != Status::Ok) break;
    uint8_t zero_storage[4096] = {};  // the new clusters must not show what was there before
    ConstByteView zeros(zero_storage);
    for (uint64_t at = 0; at < got * cluster_bytes_ && s == Status::Ok;) {
      const uint64_t n = got * cluster_bytes_ - at < zeros.size() ? got * cluster_bytes_ - at : zeros.size();
      s = write_bytes(lcn * cluster_bytes_ + at, zeros.first(n));
      at += n;
    }
    vcn += got;
  }
  if (s == Status::Ok) {
    // Neighbouring runs of one kind are merged so that the runs take the least room.
    RunList merged(allocator_);
    for (const Run& r : stream.runs.view()) {
      if (!merged.view().empty()) {
        Run& last = merged[merged.count() - 1];
        if ((last.lcn == kHole && r.lcn == kHole) || (last.lcn != kHole && r.lcn != kHole && last.lcn + last.length == r.lcn)) {
          last.length += r.length;
          continue;
        }
      }
      if (!merged.push_back(r)) {
        s = Status::OutOfMemory;
        break;
      }
    }
    if (s == Status::Ok) s = copy_runs(merged, &stream.runs);
  }
  if (s == Status::Ok) s = stream_store(stream);
  if (s != Status::Ok) {
    free_runs(fresh);
    copy_runs(saved, &stream.runs);
  }
  return s;
}

// Writes inside the stream (callers grow it first).
Status NtfsFileSystem::stream_write(Stream& stream, uint64_t offset, ConstByteView data) {
  if (offset > stream.size || data.size() > stream.size - offset) return Status::InvalidArgument;
  if (stream.flags & (kStreamCompressed | kStreamEncrypted)) return Status::Unsupported;
  if (data.empty()) return Status::Ok;
  if (stream.resident) {
    Record record(allocator_);
    Status s = read_record(stream.record, &record);
    if (s != Status::Ok) return s;
    Attr a;
    s = attr_find(record, stream.type, stream.name, &a);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (a.nonresident || a.value_length != stream.size) return Status::Corrupt;
    record.bytes().subview(a.value_offset + offset, data.size()).copy_from(data);
    return write_record(record);
  }
  const uint64_t end = offset + data.size();
  Status s = Status::Ok;
  bool holes = false;
  for (uint64_t vcn = offset / cluster_bytes_; vcn <= (end - 1) / cluster_bytes_;) {
    const Run* run = run_for(stream.runs, vcn);
    if (!run) return Status::Corrupt;
    if (run->lcn == kHole) holes = true;
    vcn = run->vcn + run->length;
  }
  if (holes) {
    s = fill_holes(stream, offset / cluster_bytes_, (end - 1) / cluster_bytes_);
    if (s != Status::Ok) return s;
  }
  if (offset > stream.initialized) {  // what lies between the old end of the data and the new must read as zeros
    s = zero_runs(stream.runs, stream.initialized, offset);
    if (s != Status::Ok) return s;
  }
  s = write_runs(stream.runs, offset, data);
  if (s != Status::Ok) return s;
  if (end > stream.initialized) {
    stream.initialized = end;
    s = stream_store(stream);
  }
  return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// File records

Status NtfsFileSystem::grow_mft() {
  Record mft(allocator_);
  Status s = read_record(kRecMft, &mft);
  if (s != Status::Ok) return s;
  Stream data(allocator_);
  s = stream_open(mft, kAttrData, "", &data);
  if (s != Status::Ok) return s;
  uint64_t extra = uint64_t{64} * record_size_;
  extra = (extra + cluster_bytes_ - 1) / cluster_bytes_ * cluster_bytes_;
  if (data.size > (uint64_t{1} << 32) * record_size_ - extra) return Status::NoSpace;
  s = stream_resize(data, data.size + extra, true);
  if (s != Status::Ok) return s;
  s = copy_runs(data.runs, &mft_runs_);
  if (s != Status::Ok) return s;
  mft_size_ = data.size;
  Stream bitmap(allocator_);
  s = stream_open_number(kRecMft, kAttrBitmap, "", &bitmap);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  const uint64_t needed = align8(static_cast<uint32_t>((mft_size_ / record_size_ + 7) / 8));
  if (bitmap.size < needed) {
    s = stream_resize(bitmap, needed, true);
    if (s != Status::Ok) return s;
  }
  epoch_++;
  return Status::Ok;
}

// Takes a free file record (growing the MFT if all are used) and starts it as an empty, in-use record.
Status NtfsFileSystem::allocate_record(Record* out) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    Stream bitmap(allocator_);
    Status s = stream_open_number(kRecMft, kAttrBitmap, "", &bitmap);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    const uint64_t records = mft_size_ / record_size_;
    const uint64_t limit = records < bitmap.size * 8 ? records : bitmap.size * 8;
    uint8_t storage[512];
    ByteView buffer(storage);
    uint64_t buffer_start = ~uint64_t{0}, buffer_size = 0;
    for (uint64_t number = kFirstUserRecord; number < limit; ++number) {
      const uint64_t byte = number / 8;
      if (buffer_start == ~uint64_t{0} || byte < buffer_start || byte >= buffer_start + buffer_size) {
        buffer_start = byte;
        buffer_size = (limit + 7) / 8 - byte < buffer.size() ? (limit + 7) / 8 - byte : buffer.size();
        uint64_t got = 0;
        s = stream_read(bitmap, byte, buffer.first(buffer_size), &got);
        if (s != Status::Ok) return s;
        if (got != buffer_size) return Status::Corrupt;
      }
      uint8_t& cell = buffer[byte - buffer_start];
      const uint8_t mask = static_cast<uint8_t>(1u << (number % 8));
      if (cell & mask) continue;
      cell |= mask;
      s = stream_write(bitmap, byte, ConstByteView(&cell, 1));
      if (s != Status::Ok) return s;
      uint16_t sequence = 1;
      Record old(allocator_);
      if (read_record(number, &old, false) == Status::Ok && rd16(old.bytes(), 0x10) != 0) sequence = rd16(old.bytes(), 0x10);
      return record_init(*out, number, sequence, kRecordInUse, record_size_);
    }
    s = grow_mft();
    if (s != Status::Ok) return s;
  }
  return Status::NoSpace;
}

// Marks a file record free (its clusters are the caller's business).
Status NtfsFileSystem::release_record(uint64_t number) {
  Record record(allocator_);
  Status s = read_record(number, &record, false);
  if (s != Status::Ok) return s;
  ByteView data = record.bytes();
  uint16_t sequence = static_cast<uint16_t>(rd16(data, 0x10) + 1);
  if (sequence == 0) sequence = 1;
  wr16(data, 0x10, sequence);
  wr16(data, 0x12, 0);
  wr16(data, 0x16, rd16(data, 0x16) & ~(kRecordInUse | kRecordDirectory));
  s = write_record(record);
  if (s != Status::Ok) return s;
  Stream bitmap(allocator_);
  s = stream_open_number(kRecMft, kAttrBitmap, "", &bitmap);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  uint8_t cell = 0;
  uint64_t got = 0;
  s = stream_read(bitmap, number / 8, ByteView(&cell, 1), &got);
  if (s != Status::Ok) return s;
  if (got != 1) return Status::Corrupt;
  cell &= static_cast<uint8_t>(~(1u << (number % 8)));
  return stream_write(bitmap, number / 8, ConstByteView(&cell, 1));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Folder trees: changing

Status NtfsFileSystem::index_store(IndexCtx& ctx, IndexNode& node) {
  if (node.root) {
    Record record(allocator_);
    Status s = read_record(ctx.dir, &record);
    if (s != Status::Ok) return s;
    Attr a;
    s = attr_find(record, kAttrIndexRoot, "$I30", &a);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (a.nonresident || a.value_length < 0x20) return Status::Corrupt;
    const uint32_t length = 0x20 + static_cast<uint32_t>(node.entries.size());
    s = resident_resize(record, a, length);
    if (s != Status::Ok) return s;
    ByteView value = record.bytes().subview(a.value_offset, length);
    wr32(value, 0x10, 0x10);
    wr32(value, 0x14, 0x10 + static_cast<uint32_t>(node.entries.size()));
    wr32(value, 0x18, 0x10 + static_cast<uint32_t>(node.entries.size()));
    wr32(value, 0x1C, node.large ? 1 : 0);
    value.subview(0x20, node.entries.size()).copy_from(node.entries.view());
    return write_record(record);
  }
  ByteBuffer raw(allocator_);
  const Status s = build_block(ctx, node, &raw);
  if (s != Status::Ok) return s;
  return stream_write(ctx.alloc, node.vcn * ctx.vcn_unit, raw.view());
}

// The index block as it goes on the medium (entries, header, multi sector protection).
Status NtfsFileSystem::build_block(IndexCtx& ctx, IndexNode& node, ByteBuffer* raw_out) {
  const uint32_t eo = block_entries_offset(ctx.block_size);
  if (node.entries.size() > ctx.block_size - 0x18 - eo) return Status::NoSpace;
  ByteBuffer& raw = *raw_out;
  raw.clear();
  if (!raw.put_zeros(ctx.block_size)) return Status::OutOfMemory;
  ByteView b = raw.writable_view();
  const uint32_t usa_count = ctx.block_size / kFixupUnit + 1;
  b[0] = 'I';
  b[1] = 'N';
  b[2] = 'D';
  b[3] = 'X';
  wr16(b, 4, 0x28);
  wr16(b, 6, usa_count);
  wr64(b, 0x10, node.vcn);
  wr32(b, 0x18, eo);
  wr32(b, 0x1C, eo + static_cast<uint32_t>(node.entries.size()));
  wr32(b, 0x20, ctx.block_size - 0x18);
  wr32(b, 0x24, node.large ? 1 : 0);
  b.subview(0x18 + eo, node.entries.size()).copy_from(node.entries.view());
  uint16_t usn = static_cast<uint16_t>(node.usn + 1);
  if (usn == 0 || usn == 0xFFFF) usn = 1;
  node.usn = usn;
  wr16(b, 0x28, usn);
  for (uint32_t i = 1; i < usa_count; ++i) {
    const uint32_t end = i * kFixupUnit - 2;
    wr16(b, 0x28 + 2 * i, rd16(b, end));
    wr16(b, end, usn);
  }
  return Status::Ok;
}

// Takes an index block (reusing a free one, else growing the allocation); its VCN is returned and its content is the caller's to write.
Status NtfsFileSystem::index_new_block(IndexCtx& ctx, uint64_t* vcn) {
  if (!ctx.has_alloc || !ctx.has_bitmap) return Status::Corrupt;
  Status s = Status::Ok;
  const uint64_t blocks = ctx.alloc.size / ctx.block_size;
  ByteBuffer bits(allocator_);
  if (!bits.put_zeros(ctx.bitmap.size)) return Status::OutOfMemory;
  uint64_t got = 0;
  s = stream_read(ctx.bitmap, 0, bits.writable_view(), &got);
  if (s != Status::Ok) return s;
  uint64_t index = blocks;
  for (uint64_t i = 0; i < blocks && i / 8 < bits.size(); ++i) {
    if (!((bits.view()[i / 8] >> (i % 8)) & 1)) {
      index = i;
      break;
    }
  }
  if (index == blocks) {
    s = stream_resize(ctx.alloc, ctx.alloc.size + ctx.block_size, true);
    if (s != Status::Ok) return s;
  }
  if (index / 8 >= ctx.bitmap.size) {
    s = stream_resize(ctx.bitmap, align8(static_cast<uint32_t>(index / 8 + 1)), true);
    if (s != Status::Ok) return s;
    if (!bits.put_zeros(ctx.bitmap.size - bits.size())) return Status::OutOfMemory;
  }
  uint8_t cell = index / 8 < bits.size() ? bits.view()[index / 8] : 0;
  cell |= static_cast<uint8_t>(1u << (index % 8));
  s = stream_write(ctx.bitmap, index / 8, ConstByteView(&cell, 1));
  if (s != Status::Ok) return s;
  *vcn = index * ctx.block_size / ctx.vcn_unit;
  return Status::Ok;
}

Status NtfsFileSystem::index_free_block(IndexCtx& ctx, uint64_t vcn) {
  const uint64_t index = vcn * ctx.vcn_unit / ctx.block_size;
  if (!ctx.has_bitmap || index / 8 >= ctx.bitmap.size) return Status::Corrupt;
  uint8_t cell = 0;
  uint64_t got = 0;
  Status s = stream_read(ctx.bitmap, index / 8, ByteView(&cell, 1), &got);
  if (s != Status::Ok) return s;
  cell &= static_cast<uint8_t>(~(1u << (index % 8)));
  return stream_write(ctx.bitmap, index / 8, ConstByteView(&cell, 1));
}

// Frees a block and everything below it.
Status NtfsFileSystem::index_free_subtree(IndexCtx& ctx, uint64_t vcn) {
  IndexNode node(allocator_);
  Status s = index_load(ctx, false, vcn, &node);
  if (s != Status::Ok) return s;
  ConstByteView entries = node.entries.view();
  for (uint32_t at = 0;;) {
    Entry e;
    if (!entry_at(entries, at, &e)) return Status::Corrupt;
    if (e.has_child()) {
      s = index_free_subtree(ctx, e.child);
      if (s != Status::Ok) return s;
    }
    if (e.last()) break;
    at += e.length;
  }
  return index_free_block(ctx, vcn);
}

// The first index block of a folder: its clusters, the $INDEX_ALLOCATION and $BITMAP attributes and the shrunken root all go into the
// folder's record in one write (the record is full, so the root has to shrink before anything can be added).
Status NtfsFileSystem::push_down_first(IndexCtx& ctx, IndexNode& root) {
  IndexNode child(allocator_);
  child.root = false;
  child.vcn = 0;
  child.large = root.large;
  if (!child.entries.append(root.entries.view())) return Status::OutOfMemory;
  ByteBuffer image(allocator_);
  Status s = build_block(ctx, child, &image);
  if (s != Status::Ok) return s;
  const uint64_t clusters = (ctx.block_size + cluster_bytes_ - 1) / cluster_bytes_;
  RunList runs(allocator_), fresh(allocator_);
  s = extend_runs(&runs, clusters, &fresh);
  if (s != Status::Ok) return s;
  s = write_runs(runs, 0, image.view());
  Record record(allocator_);
  if (s == Status::Ok) s = read_record(ctx.dir, &record);
  Attr a;
  if (s == Status::Ok) s = attr_find(record, kAttrIndexRoot, "$I30", &a);
  ByteBuffer end(allocator_), pairs(allocator_), attr(allocator_), bits(allocator_);
  if (s == Status::Ok && (!make_entry(&end, 0, ConstByteView(), true, 0, true) || !encode_runs(runs, &pairs))) s = Status::OutOfMemory;
  if (s == Status::Ok) s = resident_resize(record, a, 0x20 + static_cast<uint32_t>(end.size()));
  if (s == Status::Ok) {
    ByteView value = record.bytes().subview(a.value_offset, 0x20 + end.size());
    wr32(value, 0x10, 0x10);
    wr32(value, 0x14, 0x10 + static_cast<uint32_t>(end.size()));
    wr32(value, 0x18, 0x10 + static_cast<uint32_t>(end.size()));
    wr32(value, 0x1C, 1);
    value.subview(0x20, end.size()).copy_from(end.view());
    // $INDEX_ALLOCATION: non-resident, named "$I30"
    const uint32_t header = 0x48, length = align8(header + static_cast<uint32_t>(pairs.size()));
    if (!attr.put_zeros(length)) s = Status::OutOfMemory;
    if (s == Status::Ok) {
      ByteView n = attr.writable_view();
      wr32(n, 0, kAttrIndexAllocation);
      wr32(n, 4, length);
      n[8] = 1;
      n[9] = 4;
      wr16(n, 10, 0x40);
      wr16(n, 14, take_attr_id(record));
      wr64(n, 0x18, clusters - 1);
      wr16(n, 0x20, header);
      wr64(n, 0x28, clusters * cluster_bytes_);
      wr64(n, 0x30, ctx.block_size);
      wr64(n, 0x38, ctx.block_size);
      const char* name = "$I30";
      for (uint32_t i = 0; i < 4; ++i) wr16(n, 0x40 + 2 * i, static_cast<uint8_t>(name[i]));
      n.subview(header, pairs.size()).copy_from(pairs.view());
      s = attr_insert(record, attr_insert_point(record, kAttrIndexAllocation), attr.view());
    }
  }
  if (s == Status::Ok) {  // $BITMAP: block 0 is in use
    uint8_t first[8] = {1};
    if (!build_resident(kAttrBitmap, "$I30", 0, take_attr_id(record), ConstByteView(first, 8), false, &bits)) s = Status::OutOfMemory;
    if (s == Status::Ok) s = attr_insert(record, attr_insert_point(record, kAttrBitmap), bits.view());
  }
  if (s == Status::Ok) s = write_record(record);
  if (s != Status::Ok) free_runs(fresh);
  return s;
}

// The root has outgrown the file record: everything in it moves to a new block and the root keeps only a pointer to that.
Status NtfsFileSystem::push_down_root(IndexCtx& ctx, IndexNode& root) {
  if (!ctx.has_alloc) return push_down_first(ctx, root);
  Status s = index_new_block(ctx, &root.vcn);
  if (s != Status::Ok) return s;
  IndexNode child(allocator_), top(allocator_);
  child.root = false;
  child.vcn = root.vcn;
  child.large = root.large;
  child.capacity = ctx.block_size - 0x18 - block_entries_offset(ctx.block_size);
  if (!child.entries.append(root.entries.view())) return Status::OutOfMemory;
  ByteBuffer end(allocator_);
  if (!make_entry(&end, 0, ConstByteView(), true, child.vcn, true) || !top.entries.append(end.view())) return Status::OutOfMemory;
  top.root = true;
  top.large = true;
  if (child.entries.size() <= child.capacity) {
    s = index_store(ctx, child);
    if (s == Status::Ok) s = index_store(ctx, top);
    return s;
  }
  s = index_store(ctx, top);  // a block too small for what the root held: it is split under the new root
  if (s != Status::Ok) return s;
  Step step_storage[2];
  ArrayView<Step> steps(step_storage);
  steps[0].root = true;
  steps[0].ordinal = 0;
  steps[1].root = false;
  steps[1].vcn = child.vcn;
  return split_node(ctx, steps, 1, child);
}

// A block is full: the middle entry moves up into its parent and the entries before it go to a new block.
Status NtfsFileSystem::split_node(IndexCtx& ctx, ArrayView<Step> steps, uint32_t level, IndexNode& node) {
  if (level == 0 || node.root) return Status::Corrupt;
  ConstByteView entries = node.entries.view();
  const uint32_t real = node_count(node) - 1;
  if (real < 2) return Status::NoSpace;
  uint32_t terminator = 0;
  for (uint32_t i = 0, at = 0; i < real; ++i) {
    Entry e;
    if (!entry_at(entries, at, &e)) return Status::Corrupt;
    at += e.length;
    terminator = at;
  }
  uint32_t m = 0, m_offset = 0;
  for (uint32_t i = 0, at = 0; i < real; ++i) {
    Entry e;
    if (!entry_at(entries, at, &e)) return Status::Corrupt;
    if (i >= 1 && at + e.length / 2 >= terminator / 2) {
      m = i;
      m_offset = at;
      break;
    }
    m = i;
    m_offset = at;
    at += e.length;
  }
  if (m == 0) {  // keep something on both sides
    Entry first;
    entry_at(entries, 0, &first);
    m = 1;
    m_offset = first.length;
  }
  Entry middle;
  if (!entry_at(entries, m_offset, &middle) || middle.last()) return Status::Corrupt;
  ByteBuffer left(allocator_), end(allocator_), right(allocator_), promoted(allocator_);
  if (!left.append(entries.first(m_offset)) || !right.append(entries.subview(m_offset + middle.length))) return Status::OutOfMemory;
  if (!make_entry(&end, 0, ConstByteView(), middle.has_child(), middle.child, true) || !left.append(end.view())) return Status::OutOfMemory;
  uint64_t left_vcn = 0;
  Status s = index_new_block(ctx, &left_vcn);
  if (s != Status::Ok) return s;
  if (!make_entry(&promoted, middle.file_ref, entries.subview(middle.offset + 16, middle.key_length), true, left_vcn, false)) return Status::OutOfMemory;
  IndexNode low(allocator_);
  low.root = false;
  low.vcn = left_vcn;
  low.large = node.large;
  if (!low.entries.append(left.view())) return Status::OutOfMemory;
  node.entries.clear();
  if (!node.entries.append(right.view())) return Status::OutOfMemory;
  s = index_store(ctx, low);
  if (s == Status::Ok) s = index_store(ctx, node);
  if (s != Status::Ok) return s;
  return insert_at(ctx, steps, level - 1, promoted.view());
}

// `node` was changed in memory: store it, or make room if it no longer fits.
Status NtfsFileSystem::place(IndexCtx& ctx, ArrayView<Step> steps, uint32_t level, IndexNode& node) {
  if (node.root) {
    const Status s = index_store(ctx, node);
    if (s != Status::NoSpace) return s;
    return push_down_root(ctx, node);
  }
  if (node.entries.size() <= ctx.block_size - 0x18 - block_entries_offset(ctx.block_size)) return index_store(ctx, node);
  return split_node(ctx, steps, level, node);
}

// Puts an entry in the node at `level` of the path, before the entry that was followed there.
Status NtfsFileSystem::insert_at(IndexCtx& ctx, ArrayView<Step> steps, uint32_t level, ConstByteView entry) {
  IndexNode node(allocator_);
  Status s = index_load(ctx, steps[level].root, steps[level].vcn, &node);
  if (s != Status::Ok) return s;
  uint32_t offset = 0;
  Entry e;
  if (!node_offset(node, steps[level].ordinal, &offset, &e)) return Status::Corrupt;
  if (!splice(&node.entries, offset, 0, entry, allocator_)) return Status::OutOfMemory;
  return place(ctx, steps, level, node);
}

// Adds a name (the $FILE_NAME value `key`) for the file `file_ref` to folder `dir`.
Status NtfsFileSystem::index_insert(uint64_t dir, ConstByteView key, uint64_t file_ref) {
  IndexCtx ctx(allocator_);
  Status s = index_open(dir, &ctx);
  if (s != Status::Ok) return s;
  uint16_t buffer[255];
  ArrayView<uint16_t> units(buffer);
  const uint32_t count = key[0x40];
  for (uint32_t i = 0; i < count; ++i) units[i] = rd16(key, 0x42 + 2 * i);
  Step step_storage[kMaxDepth];
  ArrayView<Step> steps(step_storage);
  uint32_t depth = 0;
  bool found = false;
  s = index_search(ctx, Units(units).first(count), steps, &depth, &found);
  if (s != Status::Ok) return s;
  if (found) return Status::AlreadyExists;
  ByteBuffer entry(allocator_);
  if (!make_entry(&entry, file_ref, key, false, 0, false)) return Status::OutOfMemory;
  return insert_at(ctx, steps, depth - 1, entry.view());
}

// Removes and returns (as an entry without sub-node) the greatest entry of the subtree below `vcn`, if it has one.
Status NtfsFileSystem::take_max(IndexCtx& ctx, bool root, uint64_t vcn, ByteBuffer* taken, bool* found) {
  *found = false;
  IndexNode node(allocator_);
  Status s = index_load(ctx, root, vcn, &node);
  if (s != Status::Ok) return s;
  const uint32_t count = node_count(node);
  uint32_t end_offset = 0, last_offset = 0;
  Entry end_entry, last_entry;
  if (!node_offset(node, count - 1, &end_offset, &end_entry)) return Status::Corrupt;
  if (end_entry.has_child()) {
    s = take_max(ctx, false, end_entry.child, taken, found);
    if (s != Status::Ok || *found) return s;
  }
  if (count < 2) return Status::Ok;  // nothing in this node
  if (!node_offset(node, count - 2, &last_offset, &last_entry)) return Status::Corrupt;
  ConstByteView entries = node.entries.view();
  if (!make_entry(taken, last_entry.file_ref, entries.subview(last_entry.offset + 16, last_entry.key_length), false, 0, false)) return Status::OutOfMemory;
  ByteBuffer replacement(allocator_);
  const bool freed_tail = end_entry.has_child() && last_entry.has_child();
  if (end_entry.has_child() && !last_entry.has_child()) return Status::Corrupt;
  if (freed_tail) {  // the empty subtree at the end is dropped; the removed entry's own subtree takes its place
    if (!make_entry(&replacement, 0, ConstByteView(), true, last_entry.child, true)) return Status::OutOfMemory;
  }
  const uint32_t old_child = end_entry.child;
  const bool had_tail = end_entry.has_child();
  if (!splice(&node.entries, last_offset, last_entry.length + (freed_tail ? end_entry.length : 0), replacement.view(), allocator_)) return Status::OutOfMemory;
  s = index_store(ctx, node);
  if (s != Status::Ok) return s;
  if (had_tail && freed_tail) {
    s = index_free_subtree(ctx, old_child);
    if (s != Status::Ok) return s;
  }
  *found = true;
  return Status::Ok;
}

// Removes the name from folder `dir`.
Status NtfsFileSystem::index_remove(uint64_t dir, Units name) {
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
  if (!entry.has_child()) {
    if (!splice(&node.entries, offset, entry.length, ConstByteView(), allocator_)) return Status::OutOfMemory;
    return index_store(ctx, node);
  }
  // An entry with a sub-node is replaced by the greatest entry of that sub-node.
  ByteBuffer taken(allocator_), replacement(allocator_);
  bool got = false;
  s = take_max(ctx, false, entry.child, &taken, &got);
  if (s != Status::Ok) return s;
  if (!got) {  // nothing below: the entry and its empty subtree go
    if (!splice(&node.entries, offset, entry.length, ConstByteView(), allocator_)) return Status::OutOfMemory;
    s = index_store(ctx, node);
    if (s != Status::Ok) return s;
    return index_free_subtree(ctx, entry.child);
  }
  Entry bare;
  if (!entry_at(taken.view(), 0, &bare)) return Status::Corrupt;
  if (!make_entry(&replacement, bare.file_ref, taken.view().subview(16, bare.key_length), true, entry.child, false)) return Status::OutOfMemory;
  if (!splice(&node.entries, offset, entry.length, replacement.view(), allocator_)) return Status::OutOfMemory;
  return place(ctx, steps, depth - 1, node);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Namespace

bool NtfsFileSystem::valid_new_name(StringView name) {
  if (!valid_name(name)) return false;
  for (uint64_t i = 0; i < name.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(name[i]);
    if (c < 0x20 || c == 0x7F) return false;
    for (const char* p = "\"*/:<>?\\|"; *p; ++p)
      if (c == static_cast<uint8_t>(*p)) return false;
  }
  const char last = name[name.size() - 1];
  return last != ' ' && last != '.';  // (Windows cannot open such names)
}

// The value of a $FILE_NAME attribute for a new name in `parent`.
Status NtfsFileSystem::build_file_name(const Record& parent, Units name, uint8_t ns, uint32_t flags, ByteBuffer* value) {
  const uint32_t units = static_cast<uint32_t>(name.size());
  value->clear();
  if (!value->put_zeros(0x42 + 2 * units)) return Status::OutOfMemory;
  ByteView v = value->writable_view();
  wr64(v, 0, parent.number | (uint64_t{rd16(parent.bytes(), 0x10)} << 48));
  for (uint32_t i = 0; i < 4; ++i) wr64(v, 8 + 8 * i, kTimeStamp);
  wr32(v, 0x38, flags);
  v[0x40] = static_cast<uint8_t>(units);
  v[0x41] = ns;
  for (uint32_t i = 0; i < units; ++i) wr16(v, 0x42 + 2 * i, name[i]);
  return Status::Ok;
}

Status NtfsFileSystem::create_node(StringView path, bool directory, uint64_t* number) {
  Resolved parent;
  StringView leaf;
  Status s = split(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  if (!valid_new_name(leaf)) return Status::InvalidPath;
  uint16_t buffer[kMaxNameLength];
  Units units;
  if (!to_units(leaf, ArrayView<uint16_t>(buffer), &units)) return Status::InvalidPath;
  uint64_t existing = 0;
  s = lookup(parent.number, units, &existing, nullptr);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  s = begin_write();
  if (s != Status::Ok) return s;
  Record folder(allocator_);
  s = read_record(parent.number, &folder);
  if (s != Status::Ok) return s;
  uint32_t security = 0;
  Attr a;
  if (attr_find(folder, kAttrStandardInformation, "", &a) == Status::Ok && !a.nonresident && a.value_length >= 0x38)
    security = rd32(folder.bytes(), a.value_offset + 0x34);

  Record record(allocator_);
  s = allocate_record(&record);
  if (s != Status::Ok) return s;
  epoch_++;
  ByteBuffer attr(allocator_), value(allocator_);
  uint32_t at = rd16(record.bytes(), 0x14);
  // $STANDARD_INFORMATION
  if (!value.put_zeros(0x48)) return Status::OutOfMemory;
  for (uint32_t i = 0; i < 4; ++i) wr64(value.writable_view(), 8 * i, kTimeStamp);
  wr32(value.writable_view(), 0x20, directory ? 0 : kFileArchive);
  wr32(value.writable_view(), 0x34, security);
  if (!build_resident(kAttrStandardInformation, "", 0, take_attr_id(record), value.view(), false, &attr)) return Status::OutOfMemory;
  s = attr_insert(record, at, attr.view());
  at += static_cast<uint32_t>(attr.size());
  // $FILE_NAME
  if (s == Status::Ok) s = build_file_name(folder, units, kNameWin32, directory ? kFileIndexPresent : kFileArchive, &value);
  ByteBuffer key(allocator_);
  if (s == Status::Ok && !key.append(value.view())) s = Status::OutOfMemory;
  if (s == Status::Ok && !build_resident(kAttrFileName, "", 0, take_attr_id(record), value.view(), true, &attr)) s = Status::OutOfMemory;
  if (s == Status::Ok) s = attr_insert(record, at, attr.view());
  at += static_cast<uint32_t>(attr.size());
  // $DATA, or the empty index root
  if (s == Status::Ok) {
    if (directory) {
      value.clear();
      if (!value.put_zeros(0x30)) s = Status::OutOfMemory;
      if (s == Status::Ok) {
        ByteView v = value.writable_view();
        wr32(v, 0, kAttrFileName);
        wr32(v, 4, 1);
        wr32(v, 8, index_block_size_);
        v[12] = static_cast<uint8_t>(index_block_size_ >= cluster_bytes_ ? index_block_size_ / cluster_bytes_ : index_block_size_ / kFixupUnit);
        wr32(v, 0x10, 0x10);
        wr32(v, 0x14, 0x20);
        wr32(v, 0x18, 0x20);
        wr32(v, 0x1C, 0);
        wr16(v, 0x28, 0x10);
        wr16(v, 0x2C, kEntryLast);
        wr16(v, 0x28 + 0, 0x10);
      }
      if (s == Status::Ok && !build_resident(kAttrIndexRoot, "$I30", 0, take_attr_id(record), value.view(), false, &attr)) s = Status::OutOfMemory;
    } else {
      if (!build_resident(kAttrData, "", 0, take_attr_id(record), ConstByteView(), false, &attr)) s = Status::OutOfMemory;
    }
  }
  if (s == Status::Ok) s = attr_insert(record, at, attr.view());
  if (s == Status::Ok) {
    wr16(record.bytes(), 0x12, 1);
    wr16(record.bytes(), 0x16, kRecordInUse | (directory ? kRecordDirectory : 0));
    s = write_record(record);
  }
  if (s == Status::Ok) s = index_insert(parent.number, key.view(), record.number | (uint64_t{rd16(record.bytes(), 0x10)} << 48));
  if (s != Status::Ok) {
    release_record(record.number);
    return s;
  }
  *number = record.number;
  return Status::Ok;
}

Status NtfsFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (!writable_) return Status::ReadOnly;
  uint64_t number = 0;
  return create_node(path, true, &number);
}

// Everything a file record owns on the volume: the runs of all its non-resident attributes and the extension records its attribute list names.
Status NtfsFileSystem::delete_file_record(uint64_t number) {
  Record record(allocator_);
  Status s = read_record(number, &record);
  if (s != Status::Ok) return s;
  RunList doomed(allocator_);
  List<uint64_t> extensions(allocator_);
  struct Collect {
    NtfsFileSystem& fs;
    RunList* out;
    Status operator()(const Record& r) {
      Attr a;
      Status st = attr_first(r, &a);
      while (st == Status::Ok) {
        if (a.nonresident) {
          RunList part(fs.allocator_);
          const Status d = fs.decode_runs(r.bytes().subview(a.runs_offset, a.length - (a.runs_offset - a.offset)), 0, &part);
          if (d != Status::Ok) return d;
          for (const Run& run : part.view())
            if (run.lcn != kHole && !out->push_back(run)) return Status::OutOfMemory;
        }
        st = attr_next(r, &a);
      }
      return st == Status::EndOfDirectory ? Status::Ok : st;
    }
  } collect{*this, &doomed};
  s = collect(record);
  if (s != Status::Ok) return s;
  ByteBuffer list(allocator_);
  if (load_attribute_list(record, &list) == Status::Ok) {
    ConstByteView data = list.view();
    for (uint32_t at = 0; data.size() - at >= 0x1A;) {
      const uint32_t length = rd16(data, at + 4);
      if (length < 0x1A || (length & 7) != 0 || length > data.size() - at) return Status::Corrupt;
      const uint64_t ref = rd64(data, at + 16) & kRefMask;
      bool known = ref == number;
      for (const uint64_t e : extensions.view()) known = known || e == ref;
      if (!known) {
        if (!extensions.push_back(ref)) return Status::OutOfMemory;
        Record extension(allocator_);
        s = read_record(ref, &extension);
        if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
        s = collect(extension);
        if (s != Status::Ok) return s;
      }
      at += length;
    }
  }
  s = release_record(number);  // first the record, then what it owned: a crash in between leaks clusters, never shares them
  if (s != Status::Ok) return s;
  for (const uint64_t e : extensions.view()) {
    s = release_record(e);
    if (s != Status::Ok) return s;
  }
  return free_runs(doomed);
}

Status NtfsFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (!writable_) return Status::ReadOnly;
  Resolved r;
  Status s = resolve(path, &r);
  if (s != Status::Ok) return s;
  if (r.number == kRecRoot) return Status::InvalidPath;
  if (is_open(r.number)) return Status::Busy;
  Record record(allocator_);
  s = read_record(r.number, &record);
  if (s != Status::Ok) return s;
  Attr a;
  if (attr_find(record, kAttrStandardInformation, "", &a) == Status::Ok && !a.nonresident && a.value_length >= 0x24 &&
      (rd32(record.bytes(), a.value_offset + 0x20) & kFileReadOnly))
    return Status::ReadOnly;
  if (r.directory) {
    bool empty = false;
    s = directory_empty(r.number, &empty);
    if (s != Status::Ok) return s;
    if (!empty) return Status::NotEmpty;
  }
  s = begin_write();
  if (s != Status::Ok) return s;
  epoch_++;

  // The names of this link in the folder: the one that matched and its 8.3 or long counterpart.
  struct Name {
    uint32_t offset;
    NameInfo info;
  };
  List<Name> mine(allocator_);
  uint32_t others = 0;  // names (not 8.3 aliases) that stay
  Attr n;
  Status it = attr_first(record, &n);
  while (it == Status::Ok) {
    if (n.type == kAttrFileName && !n.nonresident && n.value_length >= 0x42) {
      ConstByteView v = record.bytes().subview(n.value_offset, n.value_length);
      NameInfo info;
      info.parent = rd64(v, 0) & kRefMask;
      info.ns = v[0x41];
      info.units = v[0x40];
      if (0x42u + 2u * info.units > n.value_length) return Status::Corrupt;
      ArrayView<uint16_t> chars(info.chars);
      for (uint32_t i = 0; i < info.units; ++i) chars[i] = rd16(v, 0x42 + 2 * i);
      const bool same = info.ns == r.name.ns && info.view().equals(r.name.view());
      const bool twin = info.parent == r.parent && ((r.name.ns == kNameWin32 && info.ns == kNameDos) || (r.name.ns == kNameDos && info.ns == kNameWin32));
      if (info.parent == r.parent && (same || twin)) {
        Name entry;
        entry.offset = n.offset;
        entry.info = info;
        if (!mine.push_back(entry)) return Status::OutOfMemory;
      } else if (info.ns != kNameDos) {
        ++others;
      }
    }
    it = attr_next(record, &n);
  }
  if (it != Status::EndOfDirectory) return it;
  if (others != 0 && !r.directory) {  // another name remains (a hard link): only this link's names go, which needs a plain record
    ByteBuffer list(allocator_);
    if (load_attribute_list(record, &list) == Status::Ok) return Status::Unsupported;
  }
  for (const Name& name : mine.view()) {
    s = index_remove(r.parent, name.info.view());
    if (s != Status::Ok && s != Status::NotFound) return s;
  }
  if (others == 0 || r.directory) return delete_file_record(r.number);
  for (uint32_t i = mine.count(); i-- > 0;) {  // from the back, so that the offsets stay right
    Attr victim;
    s = attr_at(record, mine[i].offset, &victim);
    if (s != Status::Ok) return s;
    attr_remove(record, victim);
  }
  wr16(record.bytes(), 0x12, others);
  return write_record(record);
}

Status NtfsFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (!writable_) return Status::ReadOnly;
  Resolved src;
  Status s = resolve(from, &src);
  if (s != Status::Ok) return s;
  if (src.number == kRecRoot) return Status::InvalidPath;
  if (is_open(src.number)) return Status::Busy;
  Resolved target_parent;
  StringView leaf;
  s = split(to, &target_parent, &leaf);
  if (s != Status::Ok) return s;
  if (!valid_new_name(leaf)) return Status::InvalidPath;
  uint16_t buffer[kMaxNameLength];
  Units units;
  if (!to_units(leaf, ArrayView<uint16_t>(buffer), &units)) return Status::InvalidPath;
  if (src.directory) {  // not into itself
    uint64_t up = target_parent.number;
    for (uint32_t hops = 0; hops < 4096; ++hops) {
      if (up == src.number) return Status::InvalidArgument;
      if (up == kRecRoot) break;
      Record record(allocator_);
      s = read_record(up, &record);
      if (s != Status::Ok) return s;
      Attr n;
      if (attr_find(record, kAttrFileName, "", &n) != Status::Ok || n.nonresident || n.value_length < 8) return Status::Corrupt;
      up = rd64(record.bytes(), n.value_offset) & kRefMask;
    }
  }
  uint64_t clash = 0;
  s = lookup(target_parent.number, units, &clash, nullptr);
  if (s == Status::Ok) {
    if ((clash & kRefMask) != src.number || target_parent.number != src.parent) return Status::AlreadyExists;  // (the same file: the case changes)
  } else if (s != Status::NotFound) {
    return s;
  }
  Record record(allocator_);
  s = read_record(src.number, &record);
  if (s != Status::Ok) return s;
  ByteBuffer list(allocator_);
  if (load_attribute_list(record, &list) == Status::Ok) return Status::Unsupported;
  s = begin_write();
  if (s != Status::Ok) return s;
  epoch_++;

  // This link's names: the one that matched and its counterpart.
  List<uint32_t> offsets(allocator_);
  ByteBuffer template_value(allocator_);
  uint32_t others = 0;
  Attr n;
  Status it = attr_first(record, &n);
  while (it == Status::Ok) {
    if (n.type == kAttrFileName && !n.nonresident && n.value_length >= 0x42) {
      ConstByteView v = record.bytes().subview(n.value_offset, n.value_length);
      const uint32_t name_units = v[0x40];
      if (0x42u + 2u * name_units > n.value_length) return Status::Corrupt;
      const uint64_t parent = rd64(v, 0) & kRefMask;
      const uint8_t ns = v[0x41];
      bool same = name_units == src.name.units && ns == src.name.ns;
      for (uint32_t i = 0; same && i < name_units; ++i) same = rd16(v, 0x42 + 2 * i) == src.name.view()[i];
      const bool twin = parent == src.parent && ((src.name.ns == kNameWin32 && ns == kNameDos) || (src.name.ns == kNameDos && ns == kNameWin32));
      if (parent == src.parent && (same || twin)) {
        if (!offsets.push_back(n.offset)) return Status::OutOfMemory;
        if (template_value.size() == 0 && !template_value.append(v.first(n.value_length))) return Status::OutOfMemory;
      } else if (ns != kNameDos) {
        ++others;
      }
    }
    it = attr_next(record, &n);
  }
  if (it != Status::EndOfDirectory) return it;
  if (offsets.count() == 0) return Status::Corrupt;

  // The old names leave the folder first; if the new one cannot go in, they are put back.
  const uint64_t reference = src.number | (uint64_t{rd16(record.bytes(), 0x10)} << 48);
  ByteBuffer old_keys(allocator_);  // [length][value] ...
  for (const uint32_t offset : offsets.view()) {
    Attr victim;
    s = attr_at(record, offset, &victim);
    if (s != Status::Ok) return s;
    ConstByteView v = record.bytes().subview(victim.value_offset, victim.value_length);
    uint16_t old_buffer[255];
    ArrayView<uint16_t> chars(old_buffer);
    const uint32_t old_units = v[0x40];
    for (uint32_t i = 0; i < old_units; ++i) chars[i] = rd16(v, 0x42 + 2 * i);
    if (!old_keys.put32(victim.value_length) || !old_keys.append(v)) return Status::OutOfMemory;
    s = index_remove(src.parent, Units(chars).first(old_units));
    if (s != Status::Ok && s != Status::NotFound) return s;
  }
  auto restore = [&]() {
    ConstByteView all = old_keys.view();
    for (uint64_t at = 0; at + 4 <= all.size();) {
      const uint32_t length = rd32(all, at);
      index_insert(src.parent, all.subview(at + 4, length), reference);
      at += 4 + length;
    }
  };
  Record new_folder(allocator_);
  s = read_record(target_parent.number, &new_folder);
  ByteBuffer value(allocator_);
  if (s == Status::Ok) s = build_file_name(new_folder, units, kNameWin32, 0, &value);
  if (s != Status::Ok) {
    restore();
    return s;
  }
  value.writable_view().subview(8, 0x38).copy_from(template_value.view().subview(8, 0x38));  // times, sizes and flags stay
  s = index_insert(target_parent.number, value.view(), reference);
  if (s != Status::Ok) {
    restore();
    return s;
  }
  for (uint32_t i = offsets.count(); i-- > 0;) {  // from the back, so that the offsets stay right
    Attr victim;
    s = attr_at(record, offsets[i], &victim);
    if (s != Status::Ok) break;
    attr_remove(record, victim);
  }
  ByteBuffer attr(allocator_);
  if (s == Status::Ok && !build_resident(kAttrFileName, "", 0, take_attr_id(record), value.view(), true, &attr)) s = Status::OutOfMemory;
  if (s == Status::Ok) s = attr_insert(record, attr_insert_point(record, kAttrFileName), attr.view());
  if (s == Status::Ok) {
    wr16(record.bytes(), 0x12, others + 1);
    s = write_record(record);
  }
  if (s != Status::Ok) {  // the record on the medium is unchanged: undo the folder entries
    index_remove(target_parent.number, units);
    restore();
  }
  return s;
}

// Brings the copies of a file's sizes and attributes in its $FILE_NAME attributes and in the folders' index entries up to date (Windows
// does that when a file is closed; the folders only show these copies to programs that do not open the file).
Status NtfsFileSystem::sync_names(uint64_t number) {
  if (!writable_) return Status::Ok;
  Record record(allocator_);
  Status s = read_record(number, &record);
  if (s != Status::Ok) return s;
  if (rd16(record.bytes(), 0x16) & kRecordDirectory) return Status::Ok;
  uint32_t attributes = kFileArchive;
  Attr a;
  if (attr_find(record, kAttrStandardInformation, "", &a) == Status::Ok && !a.nonresident && a.value_length >= 0x24)
    attributes = rd32(record.bytes(), a.value_offset + 0x20);
  Stream data(allocator_);
  s = stream_open(record, kAttrData, "", &data);
  if (s == Status::NotFound) return Status::Ok;
  if (s != Status::Ok) return s;
  uint8_t copy[0x18];  // allocated size, real size, flags
  wr64(ByteView(copy), 0, data.resident ? 0 : data.allocated);
  wr64(ByteView(copy), 8, data.size);
  wr32(ByteView(copy), 0x10, attributes);
  wr32(ByteView(copy), 0x14, 0);
  struct Name {
    uint64_t parent;
    uint32_t units;
    uint16_t chars[255];
  };
  List<Name> names(allocator_);
  bool changed = false;
  Attr n;
  Status it = attr_first(record, &n);
  while (it == Status::Ok) {
    if (n.type == kAttrFileName && !n.nonresident && n.value_length >= 0x42) {
      ByteView v = record.bytes().subview(n.value_offset, n.value_length);
      if (0x42u + 2u * v[0x40] > n.value_length) return Status::Corrupt;
      Name name;
      name.parent = rd64(v, 0) & kRefMask;
      name.units = v[0x40];
      ArrayView<uint16_t> name_chars(name.chars);
      for (uint32_t i = 0; i < name.units; ++i) name_chars[i] = rd16(v, 0x42 + 2 * i);
      if (!names.push_back(name)) return Status::OutOfMemory;
      if (!v.subview(0x28, 0x10).equals(ConstByteView(copy, 0x10)) || rd32(v, 0x38) != attributes) {
        v.subview(0x28, 0x10).copy_from(ConstByteView(copy, 0x10));
        wr32(v, 0x38, attributes);
        changed = true;
      }
    }
    it = attr_next(record, &n);
  }
  if (it != Status::EndOfDirectory) return it;
  if (changed) {
    s = write_record(record);
    if (s != Status::Ok) return s;
  }
  for (const Name& name : names.view()) {
    IndexCtx ctx(allocator_);
    s = index_open(name.parent, &ctx);
    if (s != Status::Ok) return s;
    Step step_storage[kMaxDepth];
  ArrayView<Step> steps(step_storage);
    uint32_t depth = 0;
    bool found = false;
    s = index_search(ctx, Units(name.chars, name.units), steps, &depth, &found);
    if (s != Status::Ok) return s;
    if (!found) continue;
    IndexNode node(allocator_);
    s = index_load(ctx, steps[depth - 1].root, steps[depth - 1].vcn, &node);
    if (s != Status::Ok) return s;
    uint32_t offset = 0;
    Entry entry;
    if (!node_offset(node, steps[depth - 1].ordinal, &offset, &entry)) return Status::Corrupt;
    ByteView key = node.entries.writable_view().subview(entry.offset + 16, entry.key_length);
    if (key.subview(0x28, 0x10).equals(ConstByteView(copy, 0x10)) && rd32(key, 0x38) == attributes) continue;
    key.subview(0x28, 0x10).copy_from(ConstByteView(copy, 0x10));
    wr32(key, 0x38, attributes);
    s = index_store(ctx, node);
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

}  // namespace filesystem
