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
#include "Process/ImageCache.hpp"
#include "Process/ImageLoader.hpp"

namespace process {

namespace {
ImageCache* g_cache = nullptr;
constexpr uint64_t kPage = AddressSpace::kPageSize;

// A hash of the whole file, eight bytes at a time: with the size and the headers it tells any two files apart (a library changed on disk is not mistaken for the one
// that is cached).
uint64_t hash_of(ConstByteView file) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ file.size();
  const uint64_t words = file.size() / 8;
  for (uint64_t w = 0; w < words; ++w) {
    uint64_t v = 0;
    for (uint32_t k = 0; k < 8; ++k) v |= uint64_t{file[w * 8 + k]} << (8 * k);
    h = (h ^ (v * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  for (uint64_t i = words * 8; i < file.size(); ++i) h = (h ^ file[i]) * 0x100000001B3ull;
  return h;
}

uint64_t header_bytes_of(const exe::PeFile& pe, ConstByteView file) { return pe.size_of_headers() < file.size() ? pe.size_of_headers() : file.size(); }
}  // namespace

ImageCache* image_cache() { return g_cache; }
void set_image_cache(ImageCache* cache) { g_cache = cache; }

ImageCache::~ImageCache() {
  while (images_) {      // (only images nobody maps can be left: a process that still had one would outlive the cache)
    Image* next = images_->next_;
    allocator_.deallocate(images_->storage_);
    images_->~Image();
    allocator_.deallocate(images_);
    images_ = next;
  }
}

uint32_t ImageCache::count() {
  kernel::ScopedMutex hold(lock_);
  uint32_t n = 0;
  for (Image* i = images_; i; i = i->next_) ++n;
  return n;
}

ImageCache::Image* ImageCache::acquire(const exe::PeFile& pe, ConstByteView file) {
  uint64_t image_pages = 0;
  if (!pe.is_pe32_plus() || pe.section_alignment() < kPage || !AddressSpace::round_up(pe.size_of_image(), &image_pages) || image_pages == 0) return nullptr;
  const uint64_t headers = header_bytes_of(pe, file);
  if (headers == 0 || headers > image_pages || file.size() < kPage) return nullptr;
  const uint64_t sample = hash_of(file);
  {
    kernel::ScopedMutex hold(lock_);
    for (Image* i = images_; i; i = i->next_) {
      if (i->file_size_ != file.size() || i->sample_ != sample || i->size_ != image_pages || !ConstByteView(i->headers_, kPage).equals(file.first(kPage))) continue;
      __atomic_add_fetch(&i->refs_, 1, __ATOMIC_ACQ_REL);
      return i;
    }
  }
  // Not there: laid out now, outside the lock (a big library takes a while; two processes doing it at once make two, which is only wasteful).
  void* memory = allocator_.allocate(sizeof(Image));
  if (!memory) return nullptr;
  Image* image = new (memory) Image(*this);
  if (!place_pe_sections(pe, file, image_pages, image->sections_, Image::kMaxSections, &image->section_count_) ||
      !(image->storage_ = static_cast<uint8_t*>(allocator_.allocate_aligned(image_pages, kPage)))) {
    image->~Image();
    allocator_.deallocate(image);
    return nullptr;
  }
  ByteView storage(image->storage_, image_pages);
  storage.fill(0);
  storage.first(headers).copy_from(file.first(headers));
  for (uint32_t i = 0; i < image->section_count_; ++i) {
    const PeSectionPlacement& p = image->sections_[i];
    const uint64_t n = p.copy < p.span ? p.copy : p.span;      // (place_pe_sections keeps both inside the image and the file)
    if (n) storage.subview(p.rva, n).copy_from(file.subview(p.source, n));
  }
  image->size_ = image_pages;
  image->header_bytes_ = headers;
  image->file_size_ = file.size();
  image->sample_ = sample;
  ByteView(image->headers_, kPage).copy_from(file.first(kPage));
  image->refs_ = 1;
  kernel::ScopedMutex hold(lock_);
  image->next_ = images_;
  images_ = image;
  return image;
}

void ImageCache::Image::release() {
  ImageCache& cache = cache_;
  {
    kernel::ScopedMutex hold(cache.lock_);
    if (__atomic_sub_fetch(&refs_, 1, __ATOMIC_ACQ_REL) != 0) return;
    for (Image** at = &cache.images_; *at; at = &(*at)->next_) {      // nobody maps it any more: it goes (and can no longer be found)
      if (*at != this) continue;
      *at = next_;
      break;
    }
  }
  cache.allocator_.deallocate(storage_);
  this->~Image();
  cache.allocator_.deallocate(this);
}

bool ImageCache::map(Image& image, AddressSpace& space, uint64_t base) {
  uint64_t header_pages = 0;
  if (!AddressSpace::round_up(image.header_bytes_, &header_pages) || !space.map_shared(base, header_pages, kProtRead, image.storage(), &image)) return false;
  for (uint32_t i = 0; i < image.section_count_; ++i) {
    const PeSectionPlacement& p = image.sections_[i];
    uint64_t span = 0;
    // (a writable section is mapped read-only: the Windows layer makes it copy-on-write, and the first write gives the page a copy of its own)
    if (!AddressSpace::round_up(p.span, &span) || !space.map_shared(base + p.rva, span, uint8_t(p.protection & ~kProtWrite), image.storage() + p.rva, &image)) {
      for (uint32_t k = 0; k < i; ++k) space.unmap(base + image.sections_[k].rva, image.sections_[k].span);
      space.unmap(base, header_pages);
      return false;
    }
  }
  return true;
}

}  // namespace process
