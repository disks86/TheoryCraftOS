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

// Replaying the journal (jbd2) of an ext3/ext4 volume that was not unmounted cleanly.
#include "FileSystem/Ext/ExtFileSystem.hpp"
#include "ObjectContainer/List.hpp"

namespace filesystem {

using namespace ext;

namespace {
// The journal is big endian.
constexpr uint32_t kJournalMagic = 0xC03B3998;
constexpr uint32_t kBlockDescriptor = 1, kBlockCommit = 2, kBlockSuperV1 = 3, kBlockSuperV2 = 4, kBlockRevoke = 5;
constexpr uint32_t kJournalIncompatRevoke = 0x1, kJournalIncompat64Bit = 0x2, kJournalIncompatAsyncCommit = 0x4, kJournalIncompatCsumV2 = 0x8,
                   kJournalIncompatCsumV3 = 0x10;
constexpr uint32_t kTagEscape = 0x1, kTagSameUuid = 0x2, kTagLast = 0x8;

uint32_t be32(ConstByteView v, uint64_t o) {
  ConstByteView p = v.subview(o, 4);
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}
uint16_t be16(ConstByteView v, uint64_t o) {
  ConstByteView p = v.subview(o, 2);
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
void put_be32(ByteView v, uint64_t o, uint32_t x) {
  ByteView p = v.subview(o, 4);
  p[0] = static_cast<uint8_t>(x >> 24);
  p[1] = static_cast<uint8_t>(x >> 16);
  p[2] = static_cast<uint8_t>(x >> 8);
  p[3] = static_cast<uint8_t>(x);
}

// A block the journal says no longer to replay, up to (and including) the transaction that revoked it.
struct Revoke {
  uint64_t block = 0;
  uint32_t sequence = 0;
};
}  // namespace

// Replays the journal and leaves it empty. Unsupported (nothing changed) if the journal is not one this code can replay.
Status ExtFileSystem::replay_journal() {
  const uint32_t number = sb32(kSbJournalInum);
  if (number == 0 || number > inodes_count_) return Status::Unsupported;  // a journal on a device of its own
  Inode journal;
  Status s = read_inode(number, &journal);
  if (s != Status::Ok) return s;
  if (!journal.is_regular()) return Status::Corrupt;

  ByteBuffer block(allocator_), data(allocator_);
  if (!block.put_zeros(block_size_) || !data.put_zeros(block_size_)) return Status::OutOfMemory;
  auto disk_block = [&](uint64_t index, uint64_t* disk) {
    uint64_t run = 0;
    const Status m = map_run(journal, index, 1, disk, &run);
    if (m != Status::Ok) return m;
    return *disk == 0 ? Status::Corrupt : Status::Ok;
  };
  auto load = [&](uint64_t index, ByteBuffer& into) {
    uint64_t disk = 0;
    const Status m = disk_block(index, &disk);
    return m != Status::Ok ? m : read_block(disk, into.writable_view());
  };

  s = load(0, block);
  if (s != Status::Ok) return s;
  if (be32(block.view(), 0) != kJournalMagic) return Status::Corrupt;
  const uint32_t super_type = be32(block.view(), 4);
  if (super_type != kBlockSuperV1 && super_type != kBlockSuperV2) return Status::Corrupt;
  if (be32(block.view(), 0x0C) != block_size_) return Status::Corrupt;
  const uint32_t length = be32(block.view(), 0x10), first = be32(block.view(), 0x14), sequence = be32(block.view(), 0x18),
                 start = be32(block.view(), 0x1C);
  const uint32_t features = super_type == kBlockSuperV2 ? be32(block.view(), 0x28) : 0;
  if (features & ~(kJournalIncompatRevoke | kJournalIncompat64Bit | kJournalIncompatAsyncCommit | kJournalIncompatCsumV2 | kJournalIncompatCsumV3))
    return Status::Unsupported;
  if (length < 2 || uint64_t{length} * block_size_ > journal.size() || first < 1 || first >= length || start >= length) return Status::Corrupt;
  if (start == 0) return Status::Ok;  // nothing in the journal

  const bool wide = (features & kJournalIncompat64Bit) != 0;
  const bool csum3 = (features & kJournalIncompatCsumV3) != 0;
  const bool csum2 = (features & kJournalIncompatCsumV2) != 0;
  const uint32_t tag_bytes = csum3 ? 16 : (wide ? 12 : 8) + (csum2 ? 2 : 0);
  const uint32_t tag_limit = block_size_ - ((csum2 || csum3) ? 4 : 0);  // (the checksum at the end of descriptor and revoke blocks)
  auto advance = [&](uint32_t position) { return position + 1 >= length ? first : position + 1; };

  // Three walks over the log: find where it ends (the last transaction with a commit block), collect the revoked blocks, copy the rest.
  List<Revoke> revoked(allocator_);
  uint32_t end_transaction = 0;
  for (uint32_t pass = 0; pass < 3; ++pass) {
    uint32_t next = sequence, position = start;
    for (uint32_t visited = 0; visited <= length; ++visited) {
      if (pass != 0 && next == end_transaction) break;
      s = load(position, block);
      if (s != Status::Ok) return s;
      if (be32(block.view(), 0) != kJournalMagic || be32(block.view(), 8) != next) break;
      const uint32_t type = be32(block.view(), 4);
      if (type == kBlockDescriptor) {
        uint32_t data_position = advance(position);
        for (uint32_t offset = 12; offset + tag_bytes <= tag_limit;) {
          const uint32_t flags = csum3 ? be32(block.view(), offset + 4) : be16(block.view(), offset + 6);
          if (pass == 2) {
            uint64_t target = be32(block.view(), offset);
            if (wide) target |= uint64_t{be32(block.view(), offset + 8)} << 32;
            bool skip = false;
            uint64_t low = 0, high = revoked.count();
            while (low < high) {  // the revoke list is sorted by block
              const uint64_t mid = (low + high) / 2;
              if (revoked[mid].block < target) low = mid + 1;
              else high = mid;
            }
            if (low < revoked.count() && revoked[low].block == target && revoked[low].sequence >= next) skip = true;
            if (!skip) {
              if (target >= blocks_count_) return Status::Corrupt;
              s = load(data_position, data);
              if (s != Status::Ok) return s;
              if (flags & kTagEscape) put_be32(data.writable_view(), 0, kJournalMagic);  // (the journal stored it zeroed so as not to look like its own block)
              s = write_block(target, data.view());
              if (s != Status::Ok) return s;
            }
          }
          data_position = advance(data_position);
          offset += tag_bytes + ((flags & kTagSameUuid) ? 0 : 16);
          if (flags & kTagLast) break;
        }
        position = data_position;
      } else if (type == kBlockCommit) {
        position = advance(position);
        ++next;
      } else if (type == kBlockRevoke) {
        if (pass == 1) {
          const uint32_t used = be32(block.view(), 12);
          const uint32_t size = wide ? 8 : 4;
          if (used > block_size_) return Status::Corrupt;
          for (uint32_t offset = 16; offset + size <= used; offset += size) {
            uint64_t target = be32(block.view(), offset);
            if (wide) target = (target << 32) | be32(block.view(), offset + 4);
            uint64_t low = 0, high = revoked.count();
            while (low < high) {
              const uint64_t mid = (low + high) / 2;
              if (revoked[mid].block < target) low = mid + 1;
              else high = mid;
            }
            if (low < revoked.count() && revoked[low].block == target) {
              if (revoked[low].sequence < next) revoked[low].sequence = next;
            } else {
              Revoke record;
              record.block = target;
              record.sequence = next;
              if (!revoked.insert(low, record)) return Status::OutOfMemory;
            }
          }
        }
        position = advance(position);
      } else {
        break;
      }
    }
    if (pass == 0) end_transaction = next;
  }

  // The journal is empty from here on: its next transaction is the one after the last that was replayed.
  uint64_t disk = 0;
  s = load(0, block);
  if (s == Status::Ok) s = disk_block(0, &disk);
  if (s != Status::Ok) return s;
  put_be32(block.writable_view(), 0x1C, 0);
  put_be32(block.writable_view(), 0x18, end_transaction + 1);
  return write_block(disk, block.view());
}

// Replays the journal, then reads the volume again (the replay rewrote blocks of it, the superblock and group descriptors among them) and
// marks it as needing no recovery.
Status ExtFileSystem::recover() {
  if (!(compat_ & kCompatHasJournal)) return Status::Unsupported;
  writable_ = true;  // (the replay writes to a volume that is not clean)
  Status s = replay_journal();
  if (s != Status::Ok) {
    writable_ = false;
    return s;
  }
  s = load_volume();
  if (s != Status::Ok) return s;
  writable_ = true;
  incompat_ &= ~kIncompatRecover;
  wr32(sb_.writable_view(), kSbFeatureIncompat, incompat_);
  s = write_super();
  if (s != Status::Ok) {
    writable_ = false;
    return s;
  }
  if (!device_.flush()) return Status::IoError;
  decide_writable();
  return Status::Ok;
}

}  // namespace filesystem
