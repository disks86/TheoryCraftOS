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

// One graphics adapter as the kernel sees it (WDDM's DXGADAPTER with its VidMm and VidSch): the device below it, its video memory manager, its contexts and the
// scheduler that runs their queues.
//
// Scheduling: each context has an in-order queue of submissions (command streams, fence signals and waits, presents). pump() runs queue heads - the highest priority first, the
// contexts of a priority in turn - until every queue is empty or its head waits for a fence nobody has signalled. A submission that fails (the device refuses it, memory
// runs out) loses its context: the rest of its queue is dropped, except fence signals, which still happen so that nobody waits for them forever.
#pragma once
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Kernel/Dxgkrnl/DxgObjects.hpp"

namespace dxgk {

struct AdapterConfig {
  const char* name = nullptr;                 // default: the device's
  Luid luid;                                  // zero: the kernel gives one
  uint64_t local_memory_budget = 256ull << 20;   // bytes of device objects that may be resident
  uint64_t system_memory_limit = 1ull << 30;     // bytes of system-memory backing
  hardware::IGraphicsProcessingUnit* display = nullptr;   // the screen this adapter drives (mode setting); optional
};

class Adapter {
 public:
  static constexpr uint32_t kMaxQueuedSubmissions = 4096;
  static constexpr uint64_t kMaxQueuedBytes = 64ull << 20;

  Adapter(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, const AdapterConfig& config, uint32_t index, Luid luid);
  ~Adapter();
  Adapter(const Adapter&) = delete;
  Adapter& operator=(const Adapter&) = delete;

  uint32_t index() const { return index_; }
  Luid luid() const { return luid_; }
  const AdapterCaps& caps() const { return caps_; }
  VidMm& vidmm() { return vidmm_; }
  graphics::IGraphicsDevice& device() { return device_; }
  // The GPU's timestamp clock was interrupted (reset, power transition, frequency change): the next TimestampDisjoint query on this adapter says 1.
  void interrupt_clock() { device_.interrupt_clock(); }
  hardware::IGraphicsProcessingUnit* display() { return display_; }
  allocators::IAllocator& allocator() { return allocator_; }

  // --- Shaders and pipelines. kNotSupported when the device has no way to run the shader; kInvalidParameter for bad input.
  NtStatus create_shader(const ShaderDesc& desc, SharedPointer<ShaderObject>* out);
  NtStatus create_compute_pipeline(const SharedPointer<ShaderObject>& shader, SharedPointer<PipelineObject>* out);
  NtStatus create_pipeline(const PipelineDesc& desc, const SharedPointer<ShaderObject>& vertex, const SharedPointer<ShaderObject>& fragment, SharedPointer<PipelineObject>* out,
                         const SharedPointer<ShaderObject>& geometry = SharedPointer<ShaderObject>(), const SharedPointer<ShaderObject>& hull = SharedPointer<ShaderObject>(),
                         const SharedPointer<ShaderObject>& domain = SharedPointer<ShaderObject>());

  // --- Contexts and their queues.
  NtStatus create_context(const SharedPointer<DeviceObject>& device, ContextPriority priority, SharedPointer<ContextObject>* out);
  // Drops the context from the scheduler. What it has queued is dropped (fence signals are still carried out).
  void destroy_context(const SharedPointer<ContextObject>& context);

  // Checks a command stream against its allocation list and pipelines and queues it. Nothing is queued when it is not valid (kInvalidParameter).
  NtStatus queue_render(ContextObject& context, ConstByteView commands, ArrayView<const SharedPointer<Allocation>> allocations, IPipelineLookup& pipelines);
  NtStatus queue_signal(ContextObject& context, const SharedPointer<SyncObject>& sync, uint64_t value);
  NtStatus queue_wait(ContextObject& context, const SharedPointer<SyncObject>& sync, uint64_t value);
  NtStatus queue_present(ContextObject& context, const PresentArgs& args, const SharedPointer<Allocation>& source, const SharedPointer<Allocation>& destination);

  // Runs what can run. True if anything did.
  bool pump();
  // Is the allocation named by a submission waiting in some queue?
  bool allocation_in_use(const Allocation& a) const;
  bool context_idle(const ContextObject& context) const { return context.queue.count() == 0; }

 private:
  class StreamRunner;
  friend class StreamRunner;
  NtStatus queue(ContextObject& context, UniquePointer<Submission> submission);
  bool runnable(const ContextObject& context) const;
  void run_head(ContextObject& context);
  void fault(ContextObject& context, NtStatus why);
  NtStatus execute_render(Submission& s);
  NtStatus execute_present(Submission& s);

  allocators::IAllocator& allocator_;
  graphics::IGraphicsDevice& device_;
  hardware::IGraphicsProcessingUnit* display_;
  uint32_t index_;
  Luid luid_;
  AdapterCaps caps_;
  VidMm vidmm_;                                      // (declared before the contexts: the allocations the queues hold go first)
  List<SharedPointer<ContextObject>> contexts_;
  uint32_t next_[3] = {0, 0, 0};                     // per priority: where the round robin goes on
};

}  // namespace dxgk
