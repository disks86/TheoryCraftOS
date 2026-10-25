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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "FileSystem/Ntfs/NtfsFormat.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem {

// NTFS, the file system of Windows NT and later (volumes of versions 1.x to 3.1).
//
// Reading: directories are read through their B+trees (index root and index allocation blocks) and found case insensitively through the
// volume's $UpCase table, as Windows does; short 8.3 aliases resolve but are not listed. File data is read from resident attributes, from
// runs of clusters (holes read as zeros, the part past the initialized size too), through attribute lists that spread a file over
// extension records, and from LZNT1 compressed files. The system files (records 0 to 23) are not visible. Names are shown as UTF-8 (names
// that would not fit in 255 bytes of it are left out of listings). Only the unnamed data stream is reachable; alternate streams, security
// descriptors, quotas, the USN journal and time stamps are left alone. Reparse points (symbolic links, junctions) are shown as the file or
// folder they are stored as, not followed. Encrypted (EFS) files are Unsupported. Files with several names are listed under each.
//
// Writing: files and folders can be created, written, grown, cut, renamed (into other folders too) and removed. Clusters come from the
// $Bitmap, file records from the MFT bitmap (the MFT grows when it is full), names go into the folders' B+trees (index blocks are added
// and split as needed). Data stays resident while it fits in the file record. Like ntfs-3g this does not write $LogFile records: a volume
// is mounted writable only if its device is, Windows left it clean (no dirty flag, no hibernation file, an empty or cleanly shut down
// log) and it is version 3.x; any other volume is read only (writes return ReadOnly). The dirty flag is set before the first change and
// cleared by flush() (and when the filesystem is destroyed), so a crash in between leaves the volume for chkdsk. New files are created
// with a Win32 name only (no 8.3 alias), names with characters Windows does not allow (\ / : * ? " < > | and controls) are InvalidPath,
// time stamps are not kept up to date (no clock here: new entries get 1980-01-01) and a new file takes the security of its folder.
// Not written: compressed and encrypted files (Unsupported; they can be read and removed), and anything that would need a new attribute
// list or extension record: a file record that is full (a very fragmented file, a folder whose first index block does not fit) answers
// NoSpace, and a file that already has an attribute list can be read, overwritten in place and removed, but not grown, cut or renamed
// (Unsupported). Writing into the holes of a sparse file gives them clusters.
//
// The volume starts at block `first_block` of the device (0 for a bare filesystem, the partition's first block otherwise) and mounting
// reads and validates its boot sector and the system files. The device must outlive the filesystem.
//
//   UniquePointer<IFileSystem> fs;
//   if (NtfsFileSystem::mount(allocator, device, partition_lba, &fs) == Status::Ok) ...
class NtfsFileSystem final : public IFileSystem {
 public:
  static Status mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                      UniquePointer<IFileSystem>* result);

  // Use mount(): a constructed filesystem is not usable until it has read the volume.
  NtfsFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block);
  ~NtfsFileSystem();
  NtfsFileSystem(const NtfsFileSystem&) = delete;
  NtfsFileSystem& operator=(const NtfsFileSystem&) = delete;

  FileSystemInfo info() const override;
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override;
  Status open_directory(StringView path, UniquePointer<IDirectory>* directory) override;
  Status stat(StringView path, DirectoryEntry* entry) override;
  Status create_directory(StringView path) override;
  Status remove(StringView path) override;
  Status rename(StringView from, StringView to) override;
  Status flush() override;

 private:
  class File;
  class Directory;

  // A run of clusters of a stream: virtual cluster number, length, and where it is on the volume (kHole: not stored, reads as zeros).
  struct Run {
    uint64_t vcn = 0, length = 0, lcn = 0;
  };
  using RunList = List<Run>;

  // A file record in memory with the multi sector protection removed (so `bytes` is the record as it logically reads).
  struct Record {
    explicit Record(allocators::IAllocator& allocator) : buffer(allocator) {}
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
    ByteBuffer buffer;
    uint64_t number = 0;
    ByteView bytes() { return buffer.writable_view(); }
    ConstByteView bytes() const { return buffer.view(); }
  };
  // One attribute of a record, as parsed: offsets are from the start of the record.
  struct Attr {
    uint32_t offset = 0, length = 0, type = 0;
    bool nonresident = false;
    uint8_t name_length = 0;  // in UTF-16 units
    uint32_t name_offset = 0;
    uint16_t flags = 0, id = 0;
    uint32_t value_offset = 0, value_length = 0;  // resident
    uint64_t lowest_vcn = 0, highest_vcn = 0, allocated = 0, size = 0, initialized = 0;  // non-resident
    uint32_t runs_offset = 0;
    uint8_t compression_shift = 0;
  };
  // An attribute's data: resident (read from its record) or runs of clusters. `in_base` says that all of it is in the base record.
  struct Stream {
    explicit Stream(allocators::IAllocator& allocator) : runs(allocator) {}
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    uint64_t record = 0;
    uint32_t type = 0;
    char name[8] = {};  // ASCII, "" for the unnamed one
    bool resident = true, in_base = true;
    uint16_t flags = 0;
    uint8_t compression_shift = 0;
    uint64_t size = 0, allocated = 0, initialized = 0;
    RunList runs;
  };
  // A directory entry of an index node, parsed in place.
  struct Entry {
    uint32_t offset = 0, length = 0, key_length = 0;
    uint16_t flags = 0;
    uint64_t file_ref = 0, child = 0;
    bool last() const { return (flags & ntfs::kEntryLast) != 0; }
    bool has_child() const { return (flags & ntfs::kEntryChild) != 0; }
  };
  // The entries of one index node, packed (the end entry included), loaded for reading or changing.
  struct IndexNode {
    explicit IndexNode(allocators::IAllocator& allocator) : entries(allocator) {}
    IndexNode(const IndexNode&) = delete;
    IndexNode& operator=(const IndexNode&) = delete;
    ByteBuffer entries;
    bool root = false;
    uint64_t vcn = 0;      // blocks only
    bool large = false;    // has sub-nodes
    uint32_t capacity = 0; // blocks: bytes the entries may take
    uint16_t usn = 0;
  };
  // A folder's index as one operation sees it.
  struct IndexCtx {
    explicit IndexCtx(allocators::IAllocator& allocator) : alloc(allocator), bitmap(allocator) {}
    uint64_t dir = 0;
    uint32_t block_size = 0, vcn_unit = 0;
    bool has_alloc = false, has_bitmap = false;
    Stream alloc, bitmap;
  };
  // One step down a tree: the node (the root, or a block by VCN) and the entry in it that was followed or where a new one goes.
  struct Step {
    bool root = true;
    uint64_t vcn = 0;
    uint32_t ordinal = 0;
  };
  static constexpr uint32_t kMaxDepth = 16;
  // Where a directory listing is up to: for each level the block, the entry and whether its sub-node was done.
  struct Cursor {
    struct Frame {
      bool root = true;
      uint64_t vcn = 0;
      uint32_t ordinal = 0;
      bool child_done = false;
    };
    Frame frame_storage[kMaxDepth];
    ArrayView<Frame> frames() { return ArrayView<Frame>(frame_storage); }
    uint32_t depth = 0;
    bool started = false, ended = false;
  };
  // A name as UTF-16 code units.
  using Units = ArrayView<const uint16_t>;
  // A name taken from an index entry or a $FILE_NAME attribute.
  struct NameInfo {
    uint64_t parent = 0;
    uint32_t file_flags = 0;
    uint8_t ns = 0;
    uint32_t units = 0;
    uint16_t chars[255] = {};
    Units view() const { return Units(chars, units); }
  };
  // A path resolved to a file record.
  struct Resolved {
    uint64_t number = 0;
    uint64_t parent = 0;          // the folder it was found in (the root: itself)
    NameInfo name;                // the entry that matched
    bool directory = false;
  };

  // Mounting and device access.
  Status init();
  Status check_writable(uint32_t* volume_flags);
  Status load_label();
  Status read_bytes(uint64_t offset, ByteView out);  // volume-relative byte offset
  Status write_bytes(uint64_t offset, ConstByteView data);
  Status read_runs(const RunList& runs, uint64_t offset, ByteView out);
  Status write_runs(const RunList& runs, uint64_t offset, ConstByteView data);
  Status zero_runs(const RunList& runs, uint64_t from, uint64_t to);
  const Run* run_for(const RunList& runs, uint64_t vcn) const;
  Status decode_runs(ConstByteView pairs, uint64_t start_vcn, RunList* out) const;
  bool encode_runs(const RunList& runs, ByteBuffer* out) const;
  static Status copy_runs(const RunList& from, RunList* to);
  static uint64_t runs_end(const RunList& runs);  // clusters covered

  // Records and attributes.
  Status read_record(uint64_t number, Record* record, bool need_in_use = true);
  Status write_record(Record& record);
  static Status attr_at(const Record& record, uint32_t offset, Attr* attr);
  static Status attr_first(const Record& record, Attr* attr);
  static Status attr_next(const Record& record, Attr* attr);
  static bool attr_named(const Record& record, const Attr& attr, const char* name);
  static Status attr_find(const Record& record, uint32_t type, const char* name, Attr* attr);
  static Status attr_find_vcn(const Record& record, uint32_t type, const char* name, uint64_t lowest_vcn, Attr* attr);
  static uint32_t attr_insert_point(const Record& record, uint32_t type);
  static Status attr_insert(Record& record, uint32_t at, ConstByteView attr);
  static void attr_remove(Record& record, const Attr& attr);
  static Status attr_replace(Record& record, const Attr& old_attr, ConstByteView replacement);
  static uint16_t take_attr_id(Record& record);
  static bool build_resident(uint32_t type, const char* name, uint16_t flags, uint16_t id, ConstByteView value, bool indexed, ByteBuffer* out);
  static Status resident_resize(Record& record, const Attr& attr, uint32_t new_length);
  static Status record_init(Record& record, uint64_t number, uint16_t sequence, uint16_t flags, uint32_t record_size);

  // Streams.
  Status stream_open(const Record& base, uint32_t type, const char* name, Stream* stream);
  Status stream_open_number(uint64_t record, uint32_t type, const char* name, Stream* stream);
  Status load_attribute_list(const Record& base, ByteBuffer* list);
  Status stream_read(Stream& stream, uint64_t offset, ByteView out, uint64_t* transferred);
  Status read_plain(const Stream& stream, uint64_t offset, ByteView out);
  Status read_compressed(Stream& stream, uint64_t offset, ByteView out);
  Status read_unit(const Stream& stream, uint64_t unit, ByteView out);
  static bool lznt1_decompress(ConstByteView in, ByteView out, uint64_t* produced);
  Status stream_write(Stream& stream, uint64_t offset, ConstByteView data);
  Status stream_resize(Stream& stream, uint64_t new_size, bool initialize);
  Status stream_store(Stream& stream);
  Status stream_convert(Stream& stream);
  Status fill_holes(Stream& stream, uint64_t first_vcn, uint64_t last_vcn);
  Status extend_runs(RunList* runs, uint64_t extra, RunList* fresh);
  Status cut_runs(RunList* runs, uint64_t keep, RunList* freed);
  Status free_runs(const RunList& runs);

  // Clusters and file records.
  Status bitmap_scan(uint64_t from, uint64_t to, bool want_set, uint64_t* found);
  Status bitmap_update(uint64_t first, uint64_t count, bool used);
  Status allocate_run(uint64_t hint, uint64_t want, uint64_t* lcn, uint64_t* got);
  uint64_t free_cluster_count();
  Status allocate_record(Record* record);
  Status release_record(uint64_t number);
  Status grow_mft();
  Status begin_write();
  Status finish_write();

  // Folders: names, lookup, listing.
  uint32_t upcase(uint32_t unit) const {
    if (upcase_.empty()) return unit >= 'a' && unit <= 'z' ? unit - 'a' + 'A' : unit;  // (no table: ASCII only)
    return unit < upcase_.size() ? upcase_[unit] : unit;
  }
  int compare_names(Units a, ConstByteView key_name) const;  // key_name: UTF-16LE bytes
  static bool entry_at(ConstByteView entries, uint32_t offset, Entry* entry);
  static bool entry_name(ConstByteView entries, const Entry& entry, NameInfo* name);
  static bool to_units(StringView text, ArrayView<uint16_t> buffer, Units* units);
  static bool units_to_name(Units units, FixedString<kMaxNameLength + 1>* out);
  Status index_open(uint64_t dir, IndexCtx* ctx);
  Status index_load(IndexCtx& ctx, bool root, uint64_t vcn, IndexNode* node);
  Status index_search(IndexCtx& ctx, Units name, ArrayView<Step> steps, uint32_t* depth, bool* found);
  Status lookup(uint64_t dir, Units name, uint64_t* file_ref, NameInfo* key);
  Status dir_next(uint64_t dir, Cursor* cursor, uint64_t* number, NameInfo* name);
  Status directory_empty(uint64_t dir, bool* empty);
  Status describe(uint64_t number, const NameInfo& name, DirectoryEntry* entry);
  Status resolve(StringView path, Resolved* result);
  Status split(StringView path, Resolved* parent, StringView* leaf);
  bool is_open(uint64_t number) const;

  // Changing folders.
  Status index_store(IndexCtx& ctx, IndexNode& node);
  Status build_block(IndexCtx& ctx, IndexNode& node, ByteBuffer* raw);
  Status push_down_first(IndexCtx& ctx, IndexNode& root);
  Status index_new_block(IndexCtx& ctx, uint64_t* vcn);
  Status index_free_block(IndexCtx& ctx, uint64_t vcn);
  Status index_free_subtree(IndexCtx& ctx, uint64_t vcn);
  static bool node_offset(const IndexNode& node, uint32_t ordinal, uint32_t* offset, Entry* entry);
  static uint32_t node_count(const IndexNode& node);
  Status insert_at(IndexCtx& ctx, ArrayView<Step> steps, uint32_t depth, ConstByteView entry);
  Status index_insert(uint64_t dir, ConstByteView file_name_key, uint64_t file_ref);
  Status index_remove(uint64_t dir, Units name);
  Status take_max(IndexCtx& ctx, bool root, uint64_t vcn, ByteBuffer* taken, bool* found);
  Status place(IndexCtx& ctx, ArrayView<Step> steps, uint32_t level, IndexNode& node);
  Status push_down_root(IndexCtx& ctx, IndexNode& root);
  Status split_node(IndexCtx& ctx, ArrayView<Step> steps, uint32_t level, IndexNode& node);

  // Namespace.
  Status create_node(StringView path, bool directory, uint64_t* number);
  Status build_file_name(const Record& parent, Units name, uint8_t ns, uint32_t flags, ByteBuffer* value);
  Status delete_file_record(uint64_t number);
  Status drop_links(Record& record, uint64_t parent, const NameInfo& matched, ByteBuffer* removed_keys, uint32_t* remaining);
  Status sync_names(uint64_t number);
  Status touch_volume_flag(bool dirty);
  static bool valid_new_name(StringView name);

  allocators::IAllocator& allocator_;
  hardware::IStorageDevice& device_;
  const uint64_t first_block_;
  mutable kernel::RecursiveMutex mutex_;
  struct OpenHandle {
    uint64_t number = 0;
    OpenHandle* next = nullptr;
  };
  OpenHandle* open_ = nullptr;
  uint64_t epoch_ = 1;  // changes whenever anything is written, so handles drop what they cached

  ByteView block_;  // one device block, cached
  uint64_t block_number_ = ~uint64_t{0};
  uint32_t block_size_ = 0;
  ArrayView<uint16_t> upcase_;  // the volume's $UpCase table (empty: ASCII only)

  bool writable_ = false, dirty_set_ = false;
  uint32_t bytes_per_sector_ = 0, cluster_bytes_ = 0, record_size_ = 0, index_block_size_ = 0;
  uint64_t cluster_count_ = 0;
  uint64_t mft_lcn_ = 0, mirror_lcn_ = 0;
  uint32_t major_version_ = 0;
  RunList mft_runs_;
  uint64_t mft_size_ = 0;
  RunList bitmap_runs_;
  uint64_t bitmap_size_ = 0;
  uint64_t alloc_hint_ = 0;
  uint64_t free_clusters_ = 0;
  bool free_known_ = false;
  FixedString<32> label_;
  // The last decompressed unit of a compressed file.
  ByteBuffer unit_cache_;
  uint64_t unit_cache_record_ = 0, unit_cache_unit_ = ~uint64_t{0}, unit_cache_epoch_ = 0;
};

}  // namespace filesystem
