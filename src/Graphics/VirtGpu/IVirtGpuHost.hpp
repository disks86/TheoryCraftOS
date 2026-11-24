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

// What a render node session (VirtGpuSession) needs of the virtio-gpu: the device's contexts, resources and fences. The kernel's is VirtioVirtGpuHost over
// hardware::VirtioGpu; the host tests have a pretend one. Every call is safe from any thread.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace graphics::virtgpu {

// One run of guest memory backing a resource (physical address, bytes).
struct GuestRun {
  uint64_t address = 0;
  uint32_t length = 0;
};

// RESOURCE_CREATE_3D's parameters (the virgl resource description).
struct Resource3d {
  uint32_t target = 0, format = 0, bind = 0, width = 0, height = 0, depth = 0, array_size = 0, last_level = 0, samples = 0, flags = 0;
};

struct Box {
  uint32_t x = 0, y = 0, z = 0, w = 0, h = 0, d = 0;
};

class IVirtGpuHost {
 public:
  // What the device has (the DRM GETPARAM answers): virgl 3D, blob resources, the host-visible window, context types, resource UUIDs (cross-device sharing).
  virtual bool has_3d() const = 0;
  virtual bool has_blob() const = 0;
  virtual bool has_host_visible() const = 0;
  virtual bool has_context_init() const = 0;
  virtual bool has_resource_uuid() const = 0;
  // Bit n: the host has capset n.
  virtual uint64_t capset_mask() const = 0;
  virtual bool capset_info(uint32_t id, uint32_t* max_version, uint32_t* max_size) const = 0;
  // The bytes written to `out`, 0 on failure.
  virtual uint32_t read_capset(uint32_t id, uint32_t version, ByteView out) = 0;

  // A context of type `capset_id` (0: the host's default, a virgl context). Its id, 0 on failure.
  virtual uint32_t create_context(uint32_t capset_id, ConstByteView name) = 0;
  virtual void destroy_context(uint32_t context) = 0;
  // A blob resource (VirtioGpu::create_blob); 0 on failure.
  virtual uint32_t create_blob(uint32_t context, uint32_t blob_mem, uint32_t flags, uint64_t blob_id, uint64_t size, ArrayView<const GuestRun> pages) = 0;
  virtual bool map_blob(uint32_t context, uint32_t resource, uint64_t size, uint64_t* physical, uint32_t* map_info) = 0;
  virtual bool unmap_blob(uint32_t context, uint32_t resource, uint64_t physical) = 0;
  // A 3D resource with guest backing `pages`, attached to `context`; 0 on failure.
  virtual uint32_t create_resource_3d(uint32_t context, const Resource3d& description, ArrayView<const GuestRun> pages) = 0;
  virtual bool unref_resource(uint32_t resource) = 0;
  virtual bool attach_resource(uint32_t context, uint32_t resource) = 0;
  // A transfer between a resource's guest backing and the host's copy (the box of `level`; `offset`, `stride`, `layer_stride` in the backing). Fenced:
  // the fence's number (0 if it could not be sent).
  virtual uint64_t transfer(uint32_t context, uint32_t resource, bool to_host, const Box& box, uint32_t level, uint64_t offset, uint32_t stride, uint32_t layer_stride) = 0;
  // Sends `commands` to `context` with a fence (on ring `ring`, ~0u: none); the fence's number, 0 if it could not be sent.
  virtual uint64_t submit(uint32_t context, ConstByteView commands, uint32_t ring) = 0;
  virtual bool fence_signaled(uint64_t fence) = 0;
  // False on a timeout (~0: wait as long as the host answers).
  virtual bool wait_fence(uint64_t fence, uint64_t timeout_us) = 0;
  // The largest command stream submit() takes.
  virtual uint64_t max_submit_bytes() const = 0;

 protected:
  IVirtGpuHost() = default;
  ~IVirtGpuHost() = default;
};

}  // namespace graphics::virtgpu
