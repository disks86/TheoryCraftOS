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

// Mounting NtfsFileSystem: the boot sector, the MFT, the system files it needs and the checks that decide whether the volume is writable.
#include "FileSystem/Ntfs/NtfsFileSystem.hpp"
#include "FileSystem/Ntfs/NtfsFormat.hpp"
#include "Utility/AllocatedView.hpp"

namespace filesystem {

using namespace ntfs;

namespace {
// A boot sector field that is a size either in clusters (positive) or as a power of two (negative: 2^-n bytes).
bool record_field(int8_t field, uint32_t cluster_bytes, uint32_t* bytes) {
  if (field > 0) {
    if (static_cast<uint64_t>(field) * cluster_bytes > (1u << 20)) return false;
    *bytes = static_cast<uint32_t>(field) * cluster_bytes;
  } else {
    if (field < -20 || field == 0) return false;
    *bytes = 1u << -field;
  }
  return *bytes >= kFixupUnit && (*bytes & (*bytes - 1)) == 0;
}
}  // namespace

Status NtfsFileSystem::init() {
  const hardware::StorageInfo storage = device_.info();
  block_size_ = storage.block_size;
  if (block_size_ == 0 || (block_size_ & (block_size_ - 1)) != 0 || first_block_ >= storage.block_count) return Status::InvalidArgument;
  block_ = allocate_view<uint8_t>(allocator_, block_size_);
  if (block_.empty()) return Status::OutOfMemory;

  uint8_t boot_storage[512];
  Status s = read_bytes(0, ByteView(boot_storage));
  ConstByteView boot(boot_storage);
  if (s != Status::Ok) return s;
  if (boot[510] != 0x55 || boot[511] != 0xAA || !boot.subview(3, 8).equals(text_bytes("NTFS    "))) return Status::Corrupt;

  bytes_per_sector_ = rd16(boot, 0x0B);
  if (bytes_per_sector_ < 512 || bytes_per_sector_ > 4096 || (bytes_per_sector_ & (bytes_per_sector_ - 1)) != 0) return Status::Corrupt;
  uint32_t sectors_per_cluster = boot[0x0D];
  if (sectors_per_cluster > 0x80) {
    if (256 - sectors_per_cluster > 16) return Status::Corrupt;
    sectors_per_cluster = 1u << (256 - sectors_per_cluster);
  }
  if (sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) return Status::Corrupt;
  if (uint64_t{bytes_per_sector_} * sectors_per_cluster > (1u << 21)) return Status::Unsupported;
  cluster_bytes_ = bytes_per_sector_ * sectors_per_cluster;
  const uint64_t total_sectors = rd64(boot, 0x28);
  const uint64_t device_bytes = (storage.block_count - first_block_) * block_size_;
  if (total_sectors == 0 || total_sectors > device_bytes / bytes_per_sector_) return Status::Corrupt;
  cluster_count_ = total_sectors / sectors_per_cluster;
  if (cluster_count_ == 0) return Status::Corrupt;
  mft_lcn_ = rd64(boot, 0x30);
  mirror_lcn_ = rd64(boot, 0x38);
  if (mft_lcn_ >= cluster_count_ || mirror_lcn_ >= cluster_count_) return Status::Corrupt;
  if (!record_field(static_cast<int8_t>(boot[0x40]), cluster_bytes_, &record_size_) || record_size_ > (1u << 16)) return Status::Corrupt;
  if (!record_field(static_cast<int8_t>(boot[0x44]), cluster_bytes_, &index_block_size_) || index_block_size_ > (1u << 16)) return Status::Corrupt;
  alloc_hint_ = cluster_count_ / 8;

  // The MFT: its first record says where the rest is. Until then it is one record at the place the boot sector names.
  Run first_run;
  first_run.length = (record_size_ + cluster_bytes_ - 1) / cluster_bytes_;
  first_run.lcn = mft_lcn_;
  if (first_run.length > cluster_count_ - mft_lcn_) return Status::Corrupt;
  if (!mft_runs_.push_back(first_run)) return Status::OutOfMemory;
  mft_size_ = record_size_;
  Record mft(allocator_);
  s = read_record(kRecMft, &mft);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  if (rd32(mft.bytes(), 0x20) != 0 || (rd16(mft.bytes(), 0x16) & kRecordDirectory)) return Status::Corrupt;
  Attr data;
  s = attr_find(mft, kAttrData, "", &data);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  if (!data.nonresident || data.size < 16ull * record_size_ || data.size > (uint64_t{1} << 40)) return Status::Corrupt;
  {  // first only the runs in this record, enough to find the extension records of a MFT with an attribute list
    RunList head(allocator_);
    s = decode_runs(mft.bytes().subview(data.runs_offset, data.length - (data.runs_offset - data.offset)), 0, &head);
    if (s != Status::Ok) return s;
    s = copy_runs(head, &mft_runs_);
    if (s != Status::Ok) return s;
    mft_size_ = data.size;
  }
  {
    Stream full(allocator_);
    s = stream_open(mft, kAttrData, "", &full);
    if (s != Status::Ok) return s;
    if (full.resident || runs_end(full.runs) * cluster_bytes_ < full.size) return Status::Corrupt;
    s = copy_runs(full.runs, &mft_runs_);
    if (s != Status::Ok) return s;
    mft_size_ = full.size;
  }

  // $Bitmap: one bit per cluster.
  {
    Record record(allocator_);
    s = read_record(kRecBitmap, &record);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    Stream bitmap(allocator_);
    s = stream_open(record, kAttrData, "", &bitmap);
    if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
    if (bitmap.resident || bitmap.initialized < (cluster_count_ + 7) / 8 || runs_end(bitmap.runs) * cluster_bytes_ < bitmap.size) return Status::Corrupt;
    s = copy_runs(bitmap.runs, &bitmap_runs_);
    if (s != Status::Ok) return s;
    bitmap_size_ = bitmap.size;
  }

  // $UpCase: the table the folders are ordered by. Without a usable one names are folded as ASCII.
  {
    Record record(allocator_);
    Stream table(allocator_);
    if (read_record(kRecUpcase, &record) == Status::Ok && stream_open(record, kAttrData, "", &table) == Status::Ok && !table.resident &&
        table.size == 65536 * 2 && !(table.flags & (kStreamCompressed | kStreamEncrypted))) {
      ByteBuffer raw(allocator_);
      uint64_t got = 0;
      if (!raw.put_zeros(table.size)) return Status::OutOfMemory;
      if (stream_read(table, 0, raw.writable_view(), &got) == Status::Ok && got == table.size) {
        upcase_ = allocate_view<uint16_t>(allocator_, 65536);
        if (upcase_.empty()) return Status::OutOfMemory;
        for (uint32_t i = 0; i < 65536; ++i) upcase_[i] = rd16(raw.view(), 2ull * i);
      }
    }
  }

  s = load_label();
  if (s != Status::Ok) return s;
  uint32_t volume_flags = 0;
  writable_ = false;
  if (!storage.read_only && check_writable(&volume_flags) == Status::Ok) writable_ = true;
  return Status::Ok;
}

// The volume label ($VOLUME_NAME) and the version and flags in $VOLUME_INFORMATION.
Status NtfsFileSystem::load_label() {
  Record record(allocator_);
  Status s = read_record(kRecVolume, &record);
  if (s != Status::Ok) return s == Status::NotFound ? Status::Corrupt : s;
  Attr a;
  if (attr_find(record, kAttrVolumeInformation, "", &a) != Status::Ok || a.nonresident || a.value_length < 12) return Status::Corrupt;
  major_version_ = record.bytes()[a.value_offset + 8];
  if (major_version_ < 1 || major_version_ > 3) return Status::Unsupported;
  if (attr_find(record, kAttrVolumeName, "", &a) == Status::Ok && !a.nonresident) {
    uint16_t buffer[255];
    ArrayView<uint16_t> units(buffer);
    const uint32_t count = a.value_length / 2 < 255 ? a.value_length / 2 : 255;
    for (uint32_t i = 0; i < count; ++i) units[i] = rd16(record.bytes(), a.value_offset + 2 * i);
    FixedString<kMaxNameLength + 1> text;
    if (units_to_name(Units(units).first(count), &text)) {
      uint64_t n = text.size() < label_.capacity() ? text.size() : label_.capacity();
      while (n > 0 && n < text.size() && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;  // not in the middle of a character
      label_.assign(text.view().first(n));
    }
  }
  return Status::Ok;
}

// Decides whether it is safe to change the volume: only if Windows left it clean (see the class comment).
Status NtfsFileSystem::check_writable(uint32_t* volume_flags) {
  if (major_version_ != 3 || mirror_lcn_ == 0) return Status::ReadOnly;
  Record record(allocator_);
  Status s = read_record(kRecVolume, &record);
  if (s != Status::Ok) return s;
  Attr a;
  if (attr_find(record, kAttrVolumeInformation, "", &a) != Status::Ok || a.nonresident) return Status::ReadOnly;
  *volume_flags = rd16(record.bytes(), a.value_offset + 10);
  if (*volume_flags & 0x007D) return Status::ReadOnly;  // dirty, log resize, upgrade, mounted by NT4, USN delete or object id repair pending

  Stream log(allocator_);
  s = stream_open_number(kRecLogFile, kAttrData, "", &log);
  if (s != Status::Ok || log.resident || log.size < 8192 || (log.flags & (kStreamCompressed | kStreamEncrypted))) return Status::ReadOnly;
  uint8_t page_storage[512];
  ConstByteView page(page_storage);
  uint64_t got = 0;
  s = stream_read(log, 0, ByteView(page_storage), &got);
  if (s != Status::Ok || got != page.size()) return Status::ReadOnly;
  if (rd32(page, 0) != 0xFFFFFFFFu) {  // not a log that was just reset: the restart area must say it was shut down
    if (page[0] != 'R' || page[1] != 'S' || page[2] != 'T' || page[3] != 'R') return Status::ReadOnly;
    const uint32_t area = rd16(page, 0x18);
    if (area < 0x20 || area + 0x10 > page.size() - 2) return Status::ReadOnly;
    const uint32_t in_use = rd16(page, area + 0x0C), flags = rd16(page, area + 0x0E);
    if (in_use != 0xFFFF && !(flags & 0x0002)) return Status::ReadOnly;
  }

  // Windows hibernated or started fast on this volume: its state is on the hibernation file, not on the volume.
  uint16_t buffer[16];
  ArrayView<uint16_t> name(buffer);
  const char* hiber = "hiberfil.sys";
  uint32_t units = 0;
  for (; hiber[units]; ++units) name[units] = static_cast<uint8_t>(hiber[units]);
  uint64_t ref = 0;
  s = lookup(kRecRoot, Units(name).first(units), &ref, nullptr);
  if (s == Status::Ok) {
    Stream file(allocator_);
    uint8_t head[4] = {};
    got = 0;
    if (stream_open_number(ref & kRefMask, kAttrData, "", &file) != Status::Ok) return Status::ReadOnly;
    if (!file.resident && !(file.flags & (kStreamCompressed | kStreamEncrypted)) && file.size >= 4) {
      if (stream_read(file, 0, ByteView(head), &got) != Status::Ok) return Status::ReadOnly;
      static const char* const kMagic[] = {"hibr", "HIBR", "wake", "WAKE"};
      for (const char* magic : kMagic)
        if (__builtin_memcmp(head, magic, 4) == 0) return Status::ReadOnly;
    }
  } else if (s != Status::NotFound) {
    return Status::ReadOnly;
  }
  return Status::Ok;
}

}  // namespace filesystem
