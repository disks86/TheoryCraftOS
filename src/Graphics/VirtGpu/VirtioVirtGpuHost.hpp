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

// The render node's device: QEMU's virtio-gpu (hardware::VirtioGpu) as an IVirtGpuHost, and the one the system has (set at start-up, null when there is
// no virtio-gpu with 3D: then there is no render node).
#pragma once
#include "Graphics/VirtGpu/IVirtGpuHost.hpp"
#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.hpp"
#include "Kernel/Delay.hpp"

namespace graphics::virtgpu {

class VirtioVirtGpuHost final : public IVirtGpuHost {
 public:
  explicit VirtioVirtGpuHost(hardware::VirtioGpu& gpu) : gpu_(gpu) {}

  bool has_3d() const override { return gpu_.has_3d(); }
  bool has_blob() const override { return gpu_.has_blob(); }
  bool has_host_visible() const override { return gpu_.host_visible_size() != 0; }
  bool has_context_init() const override { return gpu_.has_context_init(); }
  bool has_resource_uuid() const override { return false; }   // (RESOURCE_ASSIGN_UUID is not negotiated: no other virtio device shares resources here)
  uint64_t capset_mask() const override { return gpu_.capsets(); }
  bool capset_info(uint32_t id, uint32_t* max_version, uint32_t* max_size) const override { return gpu_.capset_info(id, max_version, max_size); }
  uint32_t read_capset(uint32_t id, uint32_t version, ByteView out) override { return gpu_.read_capset(id, version, out); }
  uint32_t create_context(uint32_t capset_id, ConstByteView name) override { return gpu_.create_context(capset_id, name); }
  void destroy_context(uint32_t context) override { gpu_.destroy_context(context); }
  uint32_t create_blob(uint32_t context, uint32_t blob_mem, uint32_t flags, uint64_t blob_id, uint64_t size, ArrayView<const GuestRun> pages) override {
    hardware::virtio_gpu::MemEntry entries[1];
    if (pages.size() > 1) return 0;   // (the session's guest memory is one run)
    for (const GuestRun& run : pages) entries[0] = {run.address, run.length, 0};
    return gpu_.create_blob(context, blob_mem, flags, blob_id, size, ArrayView<const hardware::virtio_gpu::MemEntry>(entries, pages.size()));
  }
  bool map_blob(uint32_t context, uint32_t resource, uint64_t size, uint64_t* physical, uint32_t* map_info) override {
    return gpu_.map_blob(context, resource, size, physical, map_info);
  }
  bool unmap_blob(uint32_t context, uint32_t resource, uint64_t physical) override { return gpu_.unmap_blob(context, resource, physical); }
  uint32_t create_resource_3d(uint32_t context, const Resource3d& d, ArrayView<const GuestRun> pages) override {
    hardware::virtio_gpu::ResourceCreate3d create{};
    create.target = d.target, create.format = d.format, create.bind = d.bind, create.width = d.width, create.height = d.height, create.depth = d.depth;
    create.array_size = d.array_size, create.last_level = d.last_level, create.sample_count = d.samples, create.flags = d.flags;
    hardware::virtio_gpu::MemEntry entries[1];
    if (pages.size() > 1) return 0;
    for (const GuestRun& run : pages) entries[0] = {run.address, run.length, 0};
    return gpu_.create_resource_3d(context, create, ArrayView<const hardware::virtio_gpu::MemEntry>(entries, pages.size()));
  }
  bool unref_resource(uint32_t resource) override { return gpu_.unref_resource(resource); }
  bool attach_resource(uint32_t context, uint32_t resource) override { return gpu_.attach_resource(context, resource); }
  uint64_t transfer(uint32_t context, uint32_t resource, bool to_host, const Box& b, uint32_t level, uint64_t offset, uint32_t stride, uint32_t layer_stride) override {
    const hardware::virtio_gpu::Box box{b.x, b.y, b.z, b.w, b.h, b.d};
    return gpu_.transfer_3d(context, resource, to_host, box, level, offset, stride, layer_stride);
  }
  uint64_t submit(uint32_t context, ConstByteView commands, uint32_t ring) override {
    // All slots busy: the host has kAsyncSlots submissions to work through; wait for one of them to finish and try again.
    for (uint32_t attempt = 0; attempt < 20000; ++attempt) {
      if (const uint64_t fence = gpu_.submit_async(context, commands, ring)) return fence;
      if (commands.size() > gpu_.max_submit_bytes() || gpu_.timeouts() > timeouts_at_start_ + 64) return 0;
      hardware::delay_us(250);
    }
    return 0;
  }
  bool fence_signaled(uint64_t fence) override { return gpu_.fence_signaled(fence); }
  bool wait_fence(uint64_t fence, uint64_t timeout_us) override { return gpu_.wait_fence(fence, timeout_us); }
  uint64_t max_submit_bytes() const override { return hardware::VirtioGpu::max_submit_bytes(); }

  hardware::VirtioGpu& gpu() { return gpu_; }

 private:
  hardware::VirtioGpu& gpu_;
  uint32_t timeouts_at_start_ = gpu_.timeouts();
};

// The system's render node device (null: none). Set once at start-up, before any process runs.
void set_system_host(IVirtGpuHost* host);
IVirtGpuHost* system_host();

}  // namespace graphics::virtgpu
