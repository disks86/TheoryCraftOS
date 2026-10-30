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
#include "Hardware/Monitor/IMonitorSource.hpp"
#include "Kernel/Interrupts.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

// QEMU's virtio-gpu (virtio-gpu-pci, virtio-vga and their -gl variants). As a
// display it keeps the screen in a resource backed by guest memory: drawing is
// the software FramebufferGpu code, present() copies the changed part to the host
// and flushes it. When the host offers virgl (virtio-gpu-gl*: QEMU renders with
// the host's OpenGL), it also creates a 3D context; VirglDevice
// (Graphics/Graphics3D/Virgl) uses that through execute() for GPU-accelerated 3D.
//
// A virtio-gpu has several scanouts (QEMU's max_outputs), each a monitor. As a monitor source it reports them, reads their EDID when the host offers
// it (the EDID feature), and can show one big screen resource on all of them at once, a rectangle of it on each (apply_layout()).
class VirtioGpu : public FramebufferGpu, public IMonitorSource {
 public:
  static constexpr uint16_t kPciVendor = kVirtioPciVendor;
  static constexpr uint16_t kPciDeviceId = virtio_gpu::kPciDevice;

  explicit VirtioGpu(const GpuContext& context)
      : st_(context.st), pci_(context.pci), device_(context.device), allocator_(context.allocator) {}

  const char* name() const override { return has_3d_ ? "virtio-gpu (virgl 3D)" : "virtio-gpu"; }
  bool initialize() override;
  void shutdown() override;

  // Has the device announce finished commands with an interrupt (MSI-X) so a thread waiting for the host sleeps until then instead of looking again and
  // again. Call once the kernel runs (interrupts, the local APIC and the scheduler are needed); until then, and when the device or machine cannot, commands
  // are waited for by polling. The interrupt is tried out first: a vector that does not arrive is given up on. True if completions are interrupt driven now.
  bool enable_interrupts();
  // Back to polling: the device stops sending interrupts and the handler goes. Needed before the kernel stops (its vector means nothing to the firmware's
  // interrupt table, which comes back then) and harmless at any other time.
  void disable_interrupts();
  bool interrupts_enabled() const { return interrupts_; }
  // Interrupts taken so far (for tests and the boot log).
  uint32_t interrupt_count() const { return __atomic_load_n(&interrupt_count_, __ATOMIC_ACQUIRE); }

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;
  void present() override;
  IMonitorSource* monitor_source() override { return this; }

  // --- IMonitorSource
  uint32_t connector_count() const override;
  bool connector_info(uint32_t connector, ConnectorInfo* out) const override;
  uint32_t read_edid(uint32_t connector, ByteView out) override;
  bool refresh_connectors() override;
  bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const override;
  uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const override;
  bool supports_multiple_displays() const override { return ready_; }
  bool apply_layout(const ScanoutPlacement* placements, uint32_t count, uint32_t desktop_width, uint32_t desktop_height) override;

  // --- For the 3D device ------------------------------------------------------
  // True if the host renders 3D for us (virgl negotiated, capset present, context created).
  bool has_3d() const { return has_3d_; }
  // What the virgl host says it can do (virgl_caps_v1: the GLSL level; zero when the set could not be read). Compute shaders and the memory barrier they need are core in
  // OpenGL 4.3 / GLSL 4.30; a host below that (or one that did not say) gets none. (The capability bits `bset` were read as well, but their numbering is not relied on.)
  // Most samples per pixel the host's multisampled images have (0 when it did not say).
  uint32_t virgl_max_samples() const { return has_3d_ ? virgl_max_samples_ : 0; }
  // Geometry shaders are core in OpenGL 3.2 / GLSL 1.50.
  // Tessellation is core in OpenGL 4.0 / GLSL 4.00.
  bool virgl_has_tessellation() const { return has_3d_ && virgl_glsl_level_ >= 400; }
  bool virgl_has_geometry() const { return has_3d_ && virgl_glsl_level_ >= 150; }
  bool virgl_has_compute() const { return has_3d_ && virgl_glsl_level_ >= 430; }
  uint32_t context_id() const { return kContextId; }
  uint32_t allocate_resource_id() { return next_resource_++; }
  // Bookkeeping of the 3D resources (buffers and images) the host holds for us, in bytes; the 3D device calls these as it creates and
  // frees them. Safe from any thread.
  void add_resource_bytes(uint64_t bytes) { __atomic_fetch_add(&resource_bytes_, bytes, __ATOMIC_RELAXED); }
  void remove_resource_bytes(uint64_t bytes) { __atomic_fetch_sub(&resource_bytes_, bytes, __ATOMIC_RELAXED); }
  // Used = the screen plus those resources. The total is the host's, which a guest cannot ask for: reported as 0 (unknown).
  bool video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const override;
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
  static constexpr uint64_t kInterruptWaitUs = 2000;   // one sleep for the interrupt; short, so a lost one costs little before the next look
  static constexpr uint64_t kSpinWaitUs = 50;   // how long a command's answer is waited for by spinning before the thread sleeps between looks

 private:
  static constexpr uint32_t kContextId = 1;
  static constexpr uint64_t kRequestBytes = 256 * 1024;
  static constexpr uint64_t kResponseBytes = 4096;

  bool setup_3d();
  bool read_display_info();
  bool show_layout(uint32_t id);
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
  kernel::Event completion_{true};   // set by the interrupt handler: a command finished
  bool interrupts_ = false;
  uint32_t interrupt_count_ = 0;
  static kernel::Frame* on_interrupt(kernel::Frame* frame, void* self);
  uint64_t fence_ = 0;
  bool in_flight_ = false;  // a command timed out and the device still holds its buffers
  uint32_t timeouts_ = 0;

  struct Output {
    bool enabled = false;
    uint32_t width = 0, height = 0;
  };
  Output outputs_[IMonitorSource::kMaxConnectors];
  uint32_t output_count_ = 1;
  ScanoutPlacement layout_[IMonitorSource::kMaxConnectors];   // what the scanouts show, when the screen is shared out among several monitors
  uint32_t layout_count_ = 0;
  uint32_t used_scanouts_ = 0;                                // bit mask of the scanouts that were given a resource

  bool has_3d_ = false;
  uint32_t virgl_bset_ = 0, virgl_glsl_level_ = 0, virgl_max_samples_ = 0;
  uint32_t next_resource_ = 1;
  uint64_t resource_bytes_ = 0;   // live 3D resources, see add_resource_bytes()
  uint32_t screen_resource_ = 0;  // the 2D framebuffer resource
  uint32_t shown_resource_ = 0;   // what the scanout currently shows
  ByteView backing_;
  uint32_t current_ = 0;
  bool ready_ = false;
};

}  // namespace hardware
