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
#include "Utility/RangeAllocator.hpp"

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
  // The core the last command completion interrupted.
  uint32_t last_interrupt_core() const { return __atomic_load_n(&last_interrupt_core_, __ATOMIC_ACQUIRE); }
  // Sends the completion interrupt to `core` from now on (its MSI-X entry rewritten, as the scheduler's steering does) and tells the
  // scheduler. False without interrupts or for a core that is not online.
  bool move_interrupt(uint32_t core);

  uint32_t mode_count() const override;
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;
  void present() override;
  void take_over_for_panic() override;
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
  // The colour targets the host blends with two sources (dual-source blending; 0: none), and how many viewports a draw has (GL_MAX_VIEWPORTS; 0 when it did not say).
  uint32_t virgl_max_dual_source_targets() const { return has_3d_ ? virgl_max_dual_source_ : 0; }
  uint32_t virgl_max_viewports() const { return has_3d_ ? virgl_max_viewports_ : 0; }
  // Geometry shaders are core in OpenGL 3.2 / GLSL 1.50.
  // Tessellation is core in OpenGL 4.0 / GLSL 4.00.
  bool virgl_has_tessellation() const { return has_3d_ && virgl_glsl_level_ >= 400; }
  bool virgl_has_geometry() const { return has_3d_ && virgl_glsl_level_ >= 150; }
  bool virgl_has_compute() const { return has_3d_ && virgl_glsl_level_ >= 430; }
  uint32_t context_id() const { return kContextId; }
  uint32_t allocate_resource_id() { return __atomic_fetch_add(&next_resource_, 1u, __ATOMIC_RELAXED); }   // (the 3D device makes resources from several threads)
  // The 3D work the host has been given and finished. Every command of the virgl context (one with a type from kCmdCtxCreate on) takes the next number of
  // work_sent(); once a fenced one is answered, all of them up to its number are finished on the host's GPU and work_done() says so. Work numbered above
  // work_done() may still be running: the host takes buffer uploads without waiting for it (see VirglBuffer::unmap).
  uint64_t work_sent() const { return __atomic_load_n(&work_sent_, __ATOMIC_ACQUIRE); }
  uint64_t work_done() const { return __atomic_load_n(&work_done_, __ATOMIC_ACQUIRE); }
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
  // A 3D command (a virgl context's) has a fence when `fence` is set: the host answers it only when its GPU has finished the work (and its fence poll has noticed). Without one the
  // host answers as soon as it has taken the command in, and the GPU works on while the guest goes on: the host runs the commands of the queue in order, and what reads back
  // (a transfer from the host) waits for the work before it, so only a caller that needs the GPU to be done - or the host to poll - asks for the fence.
  // `context` is the 3D context the command is for (a command from kCmdCtxCreate on is stamped with it; resource creation of a blob carries it in
  // its header itself): the virgl context by default, or one made by create_context().
  uint32_t execute(ConstByteView request, ByteView response = ByteView(), bool fence = true, uint32_t context = kContextId);
  // execute() for commands that answer with "OK, no data".
  bool command(ConstByteView request, bool fence = true) { return execute(request, ByteView(), fence) == virtio_gpu::kRespOkNoData; }
  // Shows resource `id` (size w x h) on the screen and flushes it. The 2D screen comes back
  // on the next present() of this class.
  bool show_resource(uint32_t id, uint32_t width, uint32_t height);
  allocators::IAllocator& allocator() { return *allocator_; }
  // The PCI function it is (what the render node's /sys entries describe) and its configuration space.
  const PciDeviceInfo& pci_device() const { return device_; }
  uint32_t read_pci_config32(uint16_t offset) const { return pci_ ? pci_->read32(device_.address, offset) : 0xFFFFFFFFu; }

  // --- Contexts of their own type (Venus: the host's Vulkan driver) --------------------------------------------------------------------------------------
  // A host with VIRTIO_GPU_F_CONTEXT_INIT makes contexts of a type the guest names (a capset id: kCapsetVenus, kCapsetDrm, ...), each with its own
  // objects, and with VIRTIO_GPU_F_RESOURCE_BLOB resources that are memory of such a context (RESOURCE_CREATE_BLOB), which the guest sees by
  // mapping them into the device's host-visible window (MAP_BLOB: a BAR range of QEMU's `hostmem`). The kernel's own 3D (virgl, kContextId) is not
  // one of them. All of these are safe from any thread.
  bool has_context_init() const { return ready_ && (transport_->accepted_features() & virtio_gpu::kFeatureContextInit); }
  bool has_blob() const { return ready_ && (transport_->accepted_features() & virtio_gpu::kFeatureResourceBlob); }
  // The capability sets the host has: bit n for capset id n (ids up to 63).
  uint64_t capsets() const { return capsets_; }
  bool has_capset(uint32_t id) const { return id < 64 && (capsets_ >> id) & 1; }
  // The newest version of capset `id` and its size in bytes; false if the host does not have it.
  bool capset_info(uint32_t id, uint32_t* max_version, uint32_t* max_size) const;
  // Reads version `version` of capset `id` into `out` (at most its size); the bytes written, 0 on failure.
  uint32_t read_capset(uint32_t id, uint32_t version, ByteView out);
  // The window host memory is mapped into: its physical address and size (0 when the device has none: QEMU's hostmem=0). Pages of it are what
  // map_blob() hands out.
  uint64_t host_visible_base() const { return host_window_.address; }
  uint64_t host_visible_size() const { return host_window_.length; }
  // Makes a context of type `capset_id` (named `name` in the host's logs, up to 63 bytes). Its id, 0 if the host refused or cannot.
  uint32_t create_context(uint32_t capset_id, ConstByteView name);
  void destroy_context(uint32_t context);
  // Creates a blob resource of `context`: `blob_mem` (kBlobMem*), `flags` (kBlobFlag*), `blob_id` (the context's name for the memory: a Venus
  // VkDeviceMemory id, or 0 for shared memory the host allocates), `size` bytes; a guest blob is backed by `pages` (physical runs). The resource is
  // attached to the context. Its id, 0 on failure.
  uint32_t create_blob(uint32_t context, uint32_t blob_mem, uint32_t flags, uint64_t blob_id, uint64_t size, ArrayView<const virtio_gpu::MemEntry> pages = {});
  // Maps a mappable host blob of `size` bytes into the window: `physical` is where the guest finds it, `map_info` how it is to be cached
  // (kMapCache*). False if the window is full or the host refused.
  bool map_blob(uint32_t context, uint32_t resource, uint64_t size, uint64_t* physical, uint32_t* map_info);
  // Takes a mapping away again (no CPU may touch it any more: the window range is handed out anew).
  bool unmap_blob(uint32_t context, uint32_t resource, uint64_t physical);
  // Drops a resource (of any context). Its mapping, if it has one, must be gone first.
  bool unref_resource(uint32_t resource);
  bool attach_resource(uint32_t context, uint32_t resource);
  bool detach_resource(uint32_t context, uint32_t resource);
  // A 3D (virgl) resource of `context` described by `description` (its header and id are filled in) with guest backing `pages`, attached to the context.
  // Its id, 0 on failure.
  uint32_t create_resource_3d(uint32_t context, const virtio_gpu::ResourceCreate3d& description, ArrayView<const virtio_gpu::MemEntry> pages);
  // A 3D transfer between a resource's backing and the host's copy, waited for. A fence number that is signaled (fence_signaled() is true for it), 0 on
  // failure.
  uint64_t transfer_3d(uint32_t context, uint32_t resource, bool to_host, const virtio_gpu::Box& box, uint32_t level, uint64_t offset, uint32_t stride, uint32_t layer_stride);
  // Sends `commands` (a context's own command stream: venus commands for a Venus context) to `context`. With a fence the host answers once the work
  // the commands made on ring `ring` (with kFeatureContextInit; ~0u: the context's shared timeline) is finished; without one, once it has taken them in.
  // The response type (kRespOkNoData when it went well), 0 when the host did not answer.
  static constexpr uint32_t kNoRing = ~0u;
  uint32_t submit(uint32_t context, ConstByteView commands, bool fence = false, uint32_t ring = kNoRing);
  // The same with a fence, without waiting for it: the command goes to the host and the call returns at once with the fence's number (0 if it could not
  // be sent: no slot free, the host not answering, ...). fence_signaled() / wait_fence() tell when the host has finished the work. Up to kAsyncSlots
  // such commands may be out at once.
  uint64_t submit_async(uint32_t context, ConstByteView commands, uint32_t ring = kNoRing);
  // Whether the host has answered the fenced command numbered `fence` (any number up to the last handed out that is no longer outstanding).
  bool fence_signaled(uint64_t fence);
  // Waits until it has, at most `timeout_us` (~0: for ever, as long as the host answers). False on a timeout.
  bool wait_fence(uint64_t fence, uint64_t timeout_us);
  static constexpr uint32_t kAsyncSlots = 32;
  // The largest command stream submit() takes at once.
  static constexpr uint64_t max_submit_bytes() { return kRequestBytes - sizeof(virtio_gpu::CmdSubmit); }
  // How many commands timed out so far (the host was not answering).
  uint32_t timeouts() const { return __atomic_load_n(&timeouts_, __ATOMIC_ACQUIRE); }
  static constexpr uint64_t kCommandTimeoutUs = 250000;
  // ... but a 3D command (a submission of virgl commands) waits longer: the host may be compiling a big shader for it, which takes seconds when its shader cache does not have it.
  static constexpr uint64_t k3dCommandTimeoutUs = 20000000;
  static constexpr uint64_t kPanicAnswerWaitUs = 1000000;   // how long a panic waits for the answer to a command a stopped thread had out
  static constexpr uint64_t kMaskedLockWaitUs = 2000;   // how long a caller with interrupts masked looks for the mutex before it gives up
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
  bool alone_ = false;   // take_over_for_panic() was called: nobody else is left to take lock_ or to give it back
  kernel::Event completion_{true};   // set by the interrupt handler: a command finished
  bool interrupts_ = false;
  int32_t io_device_ = -1;           // kernel::register_io_device: what the threads waiting for completion_ follow
  uint32_t interrupt_count_ = 0;
  uint32_t last_interrupt_core_ = 0;
  void drop_interrupt_handler();
  static bool retarget_interrupt(void* self, uint32_t core, uint64_t hardware_id);
  static kernel::Frame* on_interrupt(kernel::Frame* frame, void* self);
  uint64_t fence_ = 0;
  // Commands posted by submit_async() that the host has not answered yet: each has descriptors 2 + 2 * slot and 2 + 2 * slot + 1 and a buffer of
  // its own (the request, then room for the answer).
  struct AsyncSlot {
    bool busy = false;
    uint64_t fence = 0;
    ByteView buffer;
    uint32_t request_bytes = 0;
  };
  AsyncSlot async_[kAsyncSlots];
  static constexpr uint64_t kAsyncBufferBytes = 64 * 1024;
  // Takes every answer the device has given (lock_ held): the synchronous command's (descriptor 0, `sync_done` set) and the asynchronous ones'.
  void reap_locked(bool* sync_done);
  bool lock_for_command();
  uint64_t work_sent_ = 0;   // (written under lock_)
  uint64_t work_done_ = 0;
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

  uint64_t capsets_ = 0;                 // bit n: the host has capset n
  struct CapsetInfo {
    uint32_t id = 0, max_version = 0, max_size = 0;
  };
  static constexpr uint32_t kMaxCapsets = 16;
  CapsetInfo capset_infos_[kMaxCapsets];
  uint32_t capset_count_ = 0;
  uint32_t next_context_ = kContextId + 1;
  VirtioPciDevice::SharedMemoryRegion host_window_;   // the host-visible window (shm id 1)
  kernel::Mutex window_lock_;
  RangeAllocator<1024> window_;                        // which pages of it are mapped (offsets from the start of the window)
  void read_capsets();

  bool has_3d_ = false;
  uint32_t virgl_bset_ = 0, virgl_glsl_level_ = 0, virgl_max_samples_ = 0, virgl_max_dual_source_ = 0, virgl_max_viewports_ = 0;
  uint32_t next_resource_ = 1;
  uint64_t resource_bytes_ = 0;   // live 3D resources, see add_resource_bytes()
  uint32_t screen_resource_ = 0;  // the 2D framebuffer resource
  uint32_t shown_resource_ = 0;   // what the scanout currently shows
  ByteView backing_;
  uint32_t current_ = 0;
  bool ready_ = false;
};

}  // namespace hardware
