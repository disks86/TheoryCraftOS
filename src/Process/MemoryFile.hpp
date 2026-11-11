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

// A file that lives in memory: what Linux's memfd_create() makes. It is an ordinary IFile (read, write, lseek, ftruncate and fstat work on it as on any
// file), and its bytes can also be mapped into processes (mmap MAP_SHARED, or MAP_PRIVATE as a copy-on-write view) and read by the kernel itself, which is
// how a Wayland client hands the kernel's compositor its pictures with no copy (src/Wayland: a wl_shm pool is such a file passed with SCM_RIGHTS).
//
// The bytes are one MemoryBlock: page-aligned memory shared by every view of the file and every kernel reader holding it. Writes and shrinking keep the
// block; growing past its capacity moves the contents to a bigger block (views made before keep showing the old one, which stays alive as long as they or a
// reader hold it: memory stays valid, only the sharing ends, so a program that grows a file maps it again, as Wayland clients do after wl_shm_pool.resize).
//
// Locking: a mutex of the file's own guards which block is current and the size; a block's bytes are not guarded (shared memory: the processes and the
// compositor agree on who writes when).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFile.hpp"
#include "Kernel/Sync.hpp"
#include "Process/AddressSpace.hpp"
#include "Utility/ArrayView.hpp"

namespace process {

// Page-aligned memory with a reference count: each mapping (AddressSpace::map_shared retains it) and each BlockRef holds one use; the last one frees it.
class MemoryBlock final : public SharedMemory {
 public:
  static constexpr uint64_t kPage = 4096;
  // A zero-filled block of `capacity` bytes (a multiple of kPage), with one use for the caller; nullptr without memory.
  static MemoryBlock* make(allocators::IAllocator& allocator, uint64_t capacity);
  void retain() override { __atomic_add_fetch(&references_, 1, __ATOMIC_ACQ_REL); }
  void release() override;
  ByteView bytes() const { return ByteView(storage_, capacity_); }
  uint8_t* storage() const { return storage_; }      // (for AddressSpace::map_shared)
  uint64_t capacity() const { return capacity_; }

 private:
  MemoryBlock(allocators::IAllocator& allocator, uint8_t* storage, uint64_t capacity) : allocator_(allocator), storage_(storage), capacity_(capacity) {}
  ~MemoryBlock() = default;
  allocators::IAllocator& allocator_;
  uint8_t* storage_;
  uint64_t capacity_;
  uint32_t references_ = 1;
};

// One use of a block, let go when it ends (move-only).
class BlockRef {
 public:
  BlockRef() = default;
  explicit BlockRef(MemoryBlock* adopted) : block_(adopted) {}      // takes over a use the caller had
  BlockRef(BlockRef&& other) : block_(other.block_) { other.block_ = nullptr; }
  BlockRef& operator=(BlockRef&& other) {
    if (this != &other) {
      reset();
      block_ = other.block_;
      other.block_ = nullptr;
    }
    return *this;
  }
  BlockRef(const BlockRef&) = delete;
  BlockRef& operator=(const BlockRef&) = delete;
  ~BlockRef() { reset(); }
  void reset() {
    if (block_) block_->release();
    block_ = nullptr;
  }
  MemoryBlock* get() const { return block_; }
  explicit operator bool() const { return block_ != nullptr; }

 private:
  MemoryBlock* block_ = nullptr;
};

class MemoryFile final : public filesystem::IFile {
 public:
  static constexpr uint64_t kMaxSize = 512ull << 20;      // one file (a 4K picture is 32 MiB; a toolkit's pool a few of those)
  // fcntl(F_ADD_SEALS) bits
  static constexpr uint32_t kSealSeal = 1, kSealShrink = 2, kSealGrow = 4, kSealWrite = 8, kSealFutureWrite = 16;

  // `sealing`: MFD_ALLOW_SEALING (without it the file is sealed against more seals, as on Linux).
  MemoryFile(allocators::IAllocator& allocator, bool sealing);
  ~MemoryFile();
  MemoryFile(const MemoryFile&) = delete;
  MemoryFile& operator=(const MemoryFile&) = delete;

  // --- IFile
  uint64_t size() const override;
  filesystem::Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override;
  filesystem::Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override;
  filesystem::Status resize(uint64_t new_size) override;
  filesystem::Status flush() override { return filesystem::Status::Ok; }

  // The block that holds the file now (its first size() bytes are the file; the rest of its capacity reads as zeros), with a use for the caller. Empty while
  // the file is empty. *size: the file's size at that moment.
  BlockRef block(uint64_t* size);
  // posix_fallocate(): the file is at least offset + length bytes.
  filesystem::Status allocate(uint64_t offset, uint64_t length);
  // Seals (F_ADD_SEALS / F_GET_SEALS). add_seals is false when sealing is not allowed (EPERM).
  bool add_seals(uint32_t seals);
  uint32_t seals() const;

 private:
  filesystem::Status grow_locked(uint64_t new_size);

  allocators::IAllocator& allocator_;
  mutable kernel::Mutex lock_;
  MemoryBlock* block_ = nullptr;      // (the file's own use of it; nullptr while it never had bytes)
  uint64_t size_ = 0;
  uint32_t seals_ = 0;
};

}  // namespace process
