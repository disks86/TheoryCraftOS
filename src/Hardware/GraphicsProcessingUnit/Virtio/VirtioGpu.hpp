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
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpuProtocol.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

// QEMU's virtio-gpu (virtio-gpu-pci, virtio-vga and their -gl variants). As a
// display it keeps the screen in a resource backed by guest memory: drawing is
// the software FramebufferGpu code, present() copies the changed part to the host
// and flushes it. When the host offers virgl (virtio-gpu-gl*: QEMU renders with
// the host's OpenGL), it also creates a 3D context; VirglDevice
// (Graphics/Graphics3D/Virgl) uses that through execute() for GPU-accelerated 3D.
class VirtioGpu : public FramebufferGpu {
 public:
  static constexpr uint16_t kPciVendor = kVirtioPciVendor;
  static constexpr uint16_t kPciDeviceId = virtio_gpu::kPciDevice;

  explicit VirtioGpu(const GpuContext& context)
      : st_(context.st), pci_(context.pci), device_(context.device), allocator_(context.allocator) {}

  const char* name() const override { return has_3d_ ? "virtio-gpu (virgl 3D)" : "virtio-gpu"; }
  bool initialize() override;
  void shutdown() override;

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;
  void present() override;

  // --- For the 3D device ------------------------------------------------------
  // True if the host renders 3D for us (virgl negotiated, capset present, context created).
  bool has_3d() const { return has_3d_; }
  uint32_t context_id() const { return kContextId; }
  uint32_t allocate_resource_id() { return next_resource_++; }
  // Sends one request and waits for the response. `response` (optional) receives the reply
  // (as much as fits). Returns the response type (virtio_gpu::kRespOk... or an error), 0 on failure.
  // The host may stop answering (QEMU's GTK/GL display holds the queue while its window is hidden or
  // busy): a command waits at most kCommandTimeoutUs, and while one is still outstanding the buffers
  // are not reused and further calls fail at once instead of piling up. The wait never masks interrupts,
  // so the rest of the system keeps running.
  uint32_t execute(ConstByteView request, ByteView response = ByteView());
  // execute() for commands that answer with "OK, no data".
  bool command(ConstByteView request) { return execute(request) == virtio_gpu::kRespOkNoData; }
  // Shows resource `id` (size w x h) on the screen and flushes it. The 2D screen comes back
  // on the next present() of this class.
  bool show_resource(uint32_t id, uint32_t width, uint32_t height);
  allocators::IAllocator& allocator() { return *allocator_; }
  // How many commands timed out so far (the host was not answering).
  uint32_t timeouts() const { return __atomic_load_n(&timeouts_, __ATOMIC_ACQUIRE); }
  static constexpr uint64_t kCommandTimeoutUs = 250000;

 private:
  static constexpr uint32_t kContextId = 1;
  static constexpr uint64_t kRequestBytes = 256 * 1024;
  static constexpr uint64_t kResponseBytes = 4096;

  bool setup_3d();
  bool create_screen(uint32_t width, uint32_t height);
  void release_screen();

  efi::SystemTable* st_;
  const IPciBus* pci_;
  PciDeviceInfo device_;
  allocators::IAllocator* allocator_;

  VirtioPciDevice* transport_ = nullptr;
  alignas(8) unsigned char transport_storage_[sizeof(VirtioPciDevice)];
  Virtqueue control_;
  ByteView request_, response_;
  kernel::Mutex lock_;
  uint64_t fence_ = 0;
  bool in_flight_ = false;  // a command timed out and the device still holds its buffers
  uint32_t timeouts_ = 0;

  bool has_3d_ = false;
  uint32_t next_resource_ = 1;
  uint32_t screen_resource_ = 0;  // the 2D framebuffer resource
  uint32_t shown_resource_ = 0;   // what the scanout currently shows
  ByteView backing_;
  uint32_t current_ = 0;
  bool ready_ = false;
};

}  // namespace hardware
