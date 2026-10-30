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

// Video memory management (WDDM's VidMm): what an allocation is, where its contents live, and when it is on the GPU.
//
// Every allocation has a backing in system memory (the "paging file" of this design) that is always valid unless the GPU holds newer contents; the device object
// (graphics::IImage / IBuffer - the allocation's place in video memory) exists only while the allocation is resident. The manager keeps the resident bytes within the
// adapter's budget by evicting the least recently used allocations that are not in use (writing a colour image back first), and brings an allocation in again, uploading
// the backing, when a command stream needs it. A CPU lock takes the contents back to the system-memory backing; the CPU writes there and the upload happens at the next use.
//
// Depth/stencil images have no backing: their contents are lost when they are evicted (D3D allows it for a depth buffer).
// All calls are made with the Kernel's lock held.
#pragma once
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"

namespace dxgk {

class VidMm;

class Allocation {
 public:
  Allocation(VidMm& manager, allocators::IAllocator& allocator, const AllocationDesc& desc, uint64_t id);
  ~Allocation();
  Allocation(const Allocation&) = delete;
  Allocation& operator=(const Allocation&) = delete;

  uint64_t id() const { return id_; }
  VidMm& manager() const { return manager_; }
  // What being resident costs against the adapter's budget.
  uint64_t device_bytes() const { return device_bytes_; }
  const AllocationDesc& desc() const { return desc_; }
  bool is_buffer() const { return (desc_.flags & (kAllocVertexBuffer | kAllocIndexBuffer | kAllocQueryResult | kAllocUnorderedAccess)) != 0; }
  bool is_query_result() const { return (desc_.flags & kAllocQueryResult) != 0; }
  bool is_image() const { return !is_buffer(); }
  bool is_depth() const { return dxgk::is_depth(desc_.format); }
  bool is_staging() const { return (desc_.flags & kAllocStaging) != 0; }
  bool can_be_resident() const { return !is_staging() && !is_query_result(); }
  bool resident() const { return image_ != nullptr || buffer_ != nullptr; }
  uint32_t mip_levels() const { return desc_.mip_levels; }
  // The 2D images of the allocation (1 for a plain texture); they follow each other in the backing, each width * height pixels.
  uint32_t layers() const { return desc_.kind == graphics::TextureKind::Texture2D ? 1 : desc_.layers; }
  graphics::TextureKind texture_kind() const { return desc_.kind; }
  uint32_t pitch() const { return is_image() ? desc_.width * pixel_bytes(desc_.format) : 0; }

  // The system-memory backing (empty for depth images). Little-endian words in desc().format's layout; the levels of a mipmapped texture follow each other.
  ConstByteView backing() const { return cpu_; }
  ByteView writable_backing() { return cpu_; }
  // Where level `level` starts in the backing and how many bytes it has; false for a level the allocation does not have.
  bool level_range(uint32_t level, uint64_t* offset, uint64_t* bytes) const;

  graphics::IImage* image() const { return image_; }
  graphics::IBuffer* buffer() const { return buffer_; }
  uint32_t lock_count() const { return lock_count_; }

 private:
  friend class VidMm;
  VidMm& manager_;
  allocators::IAllocator& allocator_;
  AllocationDesc desc_;
  uint64_t id_;
  ByteView cpu_;
  graphics::IImage* image_ = nullptr;
  graphics::IBuffer* buffer_ = nullptr;
  uint64_t device_bytes_ = 0;    // what residency costs
  bool device_stale_ = true;     // the device object (if any) lacks the backing's current contents
  bool cpu_written_ = false;     // something other than the GPU wrote the backing since creation (a device that cannot take pixels may skip the upload of an allocation nobody wrote)
  bool gpu_newer_ = false;       // the device object holds contents the backing lacks
  uint32_t lock_count_ = 0;
  uint32_t pin_count_ = 0;
  uint64_t last_use_ = 0;
};

class VidMm {
 public:
  // `local_budget`: bytes that may be resident at once; `system_limit`: bytes of backing that may exist.
  VidMm(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, uint64_t local_budget, uint64_t system_limit);
  ~VidMm();
  VidMm(const VidMm&) = delete;
  VidMm& operator=(const VidMm&) = delete;

  graphics::IGraphicsDevice& device() { return device_; }
  allocators::IAllocator& allocator() { return allocator_; }
  uint64_t budget() const { return budget_; }
  uint64_t resident_bytes() const { return resident_bytes_; }
  uint64_t system_bytes() const { return system_bytes_; }
  uint32_t resident_count() const { return static_cast<uint32_t>(resident_.count()); }

  // Checks a description and makes the allocation (zero-filled, not resident). kInvalidParameter for a bad description, kGraphicsNoVideoMemory when the system-memory
  // limit is reached, kNoMemory when memory ran out.
  NtStatus create(const AllocationDesc& desc, SharedPointer<Allocation>* out);

  // Puts the allocation on the GPU with its current contents, evicting others if the budget needs it: kGraphicsNoVideoMemory when it cannot be made to fit.
  NtStatus make_resident(Allocation& a);
  // Takes it off the GPU (the contents are written back first); a pinned allocation is not evicted.
  bool evict(Allocation& a);
  // Evicts everything that can be.
  void trim();
  // Makes the backing hold the current contents (reads the device object back if the GPU wrote it). False if the read-back failed.
  bool read_back(Allocation& a);
  // The CPU changed the backing: the device object is stale. / The GPU changed the device object: the backing is stale.
  void note_cpu_write(Allocation& a);
  void note_gpu_write(Allocation& a);
  // The backing has been thrown away by the caller (a discard lock): the GPU's contents are no longer wanted.
  void discard_gpu_contents(Allocation& a) { a.gpu_newer_ = false; }

  // A pinned allocation is not evicted: the working set of a command stream being run.
  void pin(Allocation& a) { ++a.pin_count_; }
  void unpin(Allocation& a) { if (a.pin_count_) --a.pin_count_; }
  void lock_taken(Allocation& a) { ++a.lock_count_; }
  void lock_released(Allocation& a) { if (a.lock_count_) --a.lock_count_; }

  // The uploads and read-backs go through the logical 0xAARRGGBB the devices use; these convert a whole level between that and the backing's layout.
  static uint32_t to_logical(Format f, uint32_t word);
  static uint32_t from_logical(Format f, uint32_t pixel);

 private:
  friend class Allocation;
  void forget(Allocation& a);   // called by ~Allocation
  bool upload(Allocation& a);
  void release_device_object(Allocation& a);
  bool evict_for(uint64_t needed, const Allocation* keep);
  bool create_device_object(Allocation& a);

  allocators::IAllocator& allocator_;
  graphics::IGraphicsDevice& device_;
  uint64_t budget_, system_limit_;
  uint64_t resident_bytes_ = 0, system_bytes_ = 0;
  uint64_t clock_ = 0, next_id_ = 1;
  List<Allocation*> resident_;
  ByteBuffer scratch_;   // the converted pixels of an upload or read-back
};

}  // namespace dxgk
