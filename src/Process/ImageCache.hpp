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
#include "Executable/PeFile.hpp"
#include "Kernel/Sync.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Utility/ArrayView.hpp"

namespace process {

// The images of DLLs laid out as they are in memory at a base (headers and sections where their RVAs put them, relocated for that base), shared by every process that maps
// the same file there: a 300 MB browser DLL is in memory once however many of its processes run. The pages are mapped as an image (copy-on-write):
// a page that the loader (relocations, the import table, the security cookie) or the program writes gets a copy of its own in that process (AddressSpace::privatize).
//
// An image is known by its file: the size, the first page (the headers, with their time stamp and checksum) and a hash of all of it; an image nobody maps any more
// is freed. The cache is safe to use from several cores at once. The memory of an image is one block (the pages of a section view are, too).
class ImageCache {
 public:
  explicit ImageCache(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~ImageCache();
  ImageCache(const ImageCache&) = delete;
  ImageCache& operator=(const ImageCache&) = delete;

  class Image;
  // The laid-out image of `file` (whose parsed headers are `pe`) for loading at `base`, made now if it is not there yet: at another base than its preferred one the
  // relocations are applied (and the headers' ImageBase changed) once, in the shared copy, so a library that every process has to move to the same place is shared too.
  // The caller holds one reference (Image::release when done; each mapping holds its own). nullptr if the layout is not one that can be shared (old 512-byte sections,
  // a damaged file or relocations) or memory ran out.
  Image* acquire(const exe::PeFile& pe, ConstByteView file, uint64_t base);
  uint32_t count();

  // Maps the image at the base it was made for into `space`: the headers read-only, each section with the permissions of its characteristics
  // (the Windows layer marks them copy-on-write). False if a piece does not fit; nothing stays mapped then.
  static bool map(Image& image, AddressSpace& space);

 private:
  friend class Image;

  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;
  Image* images_ = nullptr;
};

class ImageCache::Image final : public SharedMemory {
 public:
  void retain() override { __atomic_add_fetch(&refs_, 1, __ATOMIC_ACQ_REL); }
  void release() override;
  uint8_t* storage() const { return storage_; }
  uint64_t size() const { return size_; }

 private:
  friend class ImageCache;
  static constexpr uint32_t kMaxSections = 96;
  explicit Image(ImageCache& cache) : cache_(cache) {}
  ImageCache& cache_;
  uint8_t* storage_ = nullptr;
  uint64_t size_ = 0, header_bytes_ = 0;
  uint64_t file_size_ = 0, sample_ = 0, base_ = 0;
  uint8_t headers_[4096] = {};
  PeSectionPlacement sections_[kMaxSections];
  uint32_t section_count_ = 0;
  uint32_t refs_ = 0;      // changes to and from zero only under the cache's lock
  Image* next_ = nullptr;
};

// The cache the kernel's loader uses (nullptr: images are copied into each process, as in the host tests).
ImageCache* image_cache();
void set_image_cache(ImageCache* cache);

}  // namespace process
