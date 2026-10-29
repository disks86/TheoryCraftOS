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

// The services of the kernel-mode Direct3D driver (dxgkrnl): what the D3DKMT* thunks of gdi32.dll / win32u.dll reach through system calls (NtGdiDdDDI*). Each method is the
// counterpart of the D3DKMT function it is named after, with the calling process passed explicitly (the system-call layer knows who is calling), and returns an NTSTATUS.
//
//   D3DKMTEnumAdapters2 / OpenAdapterFromLuid     enum_adapters / open_adapter_from_luid, open_adapter_from_index
//   D3DKMTQueryAdapterInfo                        query_adapter_caps, query_video_memory_info
//   D3DKMTCreateDevice / DestroyDevice            create_device / destroy_device;  D3DKMTGetDeviceState: get_device_state
//   D3DKMTCreateContext / DestroyContext          create_context / destroy_context
//   D3DKMTCreateAllocation / DestroyAllocation    create_allocation / destroy_allocation, query_allocation
//   D3DKMTShareObjects / OpenResource             share_allocation / open_shared_allocation / close_share
//   D3DKMTLock / Unlock                           lock / unlock
//   D3DKMTMakeResident / Evict / Trim             make_resident / evict / trim
//   D3DKMTCreateSynchronizationObject2 and co.    create_sync / destroy_sync, signal_sync_cpu, wait_sync_cpu, signal_sync_gpu, wait_sync_gpu
//   D3DKMTRender / Present                        render / present
//   D3DKMTEscape                                  escape (the private driver data: shaders and pipelines)
//   D3DKMTGetDisplayModeList / SetDisplayMode     display_mode_count / display_mode / set_display_mode
//
// All calls are safe from any thread (one lock). Nothing blocks except wait_sync_cpu with a timeout, and that only when a wait hook is set.
#pragma once
#include "Kernel/Dxgkrnl/Adapter.hpp"
#include "Kernel/Dxgkrnl/HandleTable.hpp"
#include "Kernel/Sync.hpp"

namespace dxgk {

// How wait_sync_cpu sleeps. wait() is called with the kernel lock released and returns when wake() was called or the time (nanoseconds) has passed - it may return early.
class IWaitHook {
 public:
  virtual void wait(uint64_t timeout_ns) = 0;
  virtual void wake() = 0;
  virtual uint64_t now_ns() = 0;

 protected:
  IWaitHook() = default;
  ~IWaitHook() = default;
};

// The private driver data of D3DKMTEscape, all little-endian 32-bit words (the user-mode driver and the kernel agree on it; the kernel checks everything):
//   request:  op, then the operation's words        reply (to the buffer given): the words listed
//   CreateShader     kind, stage, code_bytes, code (padded to a word)              reply: handle
//   CreatePipeline   vertex_shader, fragment_shader, vertex_stride, attribute_count, varying_count, texture_count, cull, front_face, flags (1 depth test, 2 depth write,
//                    4 blend), then per attribute (4 of them): location, format, offset; then stencil: enable, compare, fail_op, depth_fail_op, pass_op, masks (compare,
//                    write, reference: one word each)                                                                                       reply: handle
//                    (optionally, after that: depth_compare, blend custom, enable, source colour, destination colour, source alpha, destination alpha factors, colour op, alpha op,
//                    write mask, per-instance bits of the attributes, then the number of colour outputs, then for independent blending a flag and the nine words
//                    (custom, enable, four factors, two ops, write mask) of the blend state of each of targets 1 .. 3 - one word each)
//   DestroyShader / DestroyPipeline   handle
enum class EscapeOp : uint32_t { CreateShader = 1, CreatePipeline = 2, DestroyShader = 3, DestroyPipeline = 4 };

class Kernel {
 public:
  static constexpr uint32_t kMaxAdapters = 8;
  static constexpr uint32_t kMaxProcesses = 256;
  static constexpr uint32_t kMaxSyncWait = 64;

  explicit Kernel(allocators::IAllocator& allocator);
  ~Kernel();
  Kernel(const Kernel&) = delete;
  Kernel& operator=(const Kernel&) = delete;

  void set_wait_hook(IWaitHook* hook);

  // --- The machine's adapters. Returns the adapter number (kInvalidIndex if there is no room). `device` must outlive the kernel; give the locked wrapper when others share it.
  uint32_t add_adapter(graphics::IGraphicsDevice& device, const AdapterConfig& config);
  uint32_t adapter_count();
  Adapter* adapter(uint32_t index);   // (diagnostics and tests)

  // --- Processes. A process is opened before its first call; closing it destroys everything it still holds.
  NtStatus open_process(ProcessId pid);
  void close_process(ProcessId pid);

  NtStatus enum_adapters(ProcessId pid, ArrayView<AdapterEntry> out, uint32_t* total);
  NtStatus open_adapter_from_luid(ProcessId pid, Luid luid, Handle* adapter);
  NtStatus open_adapter_from_index(ProcessId pid, uint32_t index, Handle* adapter);
  NtStatus close_adapter(ProcessId pid, Handle adapter);
  NtStatus query_adapter_caps(ProcessId pid, Handle adapter, AdapterCaps* out);
  NtStatus query_video_memory_info(ProcessId pid, Handle adapter, MemorySegmentGroup group, VideoMemoryInfo* out);

  NtStatus display_mode_count(ProcessId pid, Handle adapter, uint32_t* count);
  NtStatus display_mode(ProcessId pid, Handle adapter, uint32_t index, DisplayModeInfo* out);
  NtStatus current_display_mode(ProcessId pid, Handle adapter, uint32_t* index);
  NtStatus set_display_mode(ProcessId pid, Handle adapter, uint32_t index);

  NtStatus create_device(ProcessId pid, Handle adapter, Handle* device);
  NtStatus destroy_device(ProcessId pid, Handle device);
  NtStatus get_device_state(ProcessId pid, Handle device, DeviceState* out);

  NtStatus create_context(ProcessId pid, Handle device, ContextPriority priority, Handle* context);
  NtStatus destroy_context(ProcessId pid, Handle context);

  // --- Allocations.
  NtStatus create_allocation(ProcessId pid, Handle device, const AllocationDesc& desc, Handle* allocation);
  NtStatus destroy_allocation(ProcessId pid, Handle allocation);
  NtStatus query_allocation(ProcessId pid, Handle allocation, AllocationInfo* out);
  // Makes a token another process can open the allocation with (the allocation must have been created kAllocShared). The token lives until the owner closes it or exits.
  NtStatus share_allocation(ProcessId pid, Handle allocation, uint32_t* token);
  NtStatus open_shared_allocation(ProcessId pid, Handle device, uint32_t token, Handle* allocation, AllocationDesc* desc);
  NtStatus close_share(ProcessId pid, uint32_t token);
  // The CPU's view of one subresource (a mip level; 0 for a buffer). The view is the allocation's system-memory backing and stays valid until unlock().
  NtStatus lock(ProcessId pid, Handle allocation, uint32_t flags, uint32_t subresource, LockResult* out);
  NtStatus unlock(ProcessId pid, Handle allocation);
  // Residency of a list of allocations. make_resident reports kGraphicsNoVideoMemory if they cannot all be on the GPU at once.
  NtStatus make_resident(ProcessId pid, Handle device, ArrayView<const Handle> allocations);
  NtStatus evict(ProcessId pid, Handle device, ArrayView<const Handle> allocations);
  NtStatus trim(ProcessId pid, Handle device);

  // --- Synchronization. A fence's `initial` is its starting value; a semaphore's `limit` is how many signals it can hold.
  NtStatus create_sync(ProcessId pid, Handle device, SyncType type, uint64_t initial, uint64_t limit, Handle* sync);
  NtStatus destroy_sync(ProcessId pid, Handle sync);
  NtStatus query_sync(ProcessId pid, Handle sync, uint64_t* value);
  NtStatus signal_sync_cpu(ProcessId pid, ArrayView<const Handle> syncs, ArrayView<const uint64_t> values);
  // Waits until all (or any) of the objects reach their values. kTimeout when they do not within `timeout_ns` (0: just look; ~0: forever). Without a wait hook it never sleeps.
  NtStatus wait_sync_cpu(ProcessId pid, ArrayView<const Handle> syncs, ArrayView<const uint64_t> values, bool wait_all, uint64_t timeout_ns);
  // The same, ordered with a context's work: the signal happens after everything queued before it, the wait holds back everything queued after it.
  NtStatus signal_sync_gpu(ProcessId pid, Handle context, Handle sync, uint64_t value);
  NtStatus wait_sync_gpu(ProcessId pid, Handle context, Handle sync, uint64_t value);

  // --- Work. `allocation_list` names the allocations the stream's indices refer to. The stream is checked at once (kInvalidParameter and nothing is queued if it is wrong); it
  // runs as soon as the context's earlier work and the fences it waits for allow, which is before this call returns when nothing holds it back.
  NtStatus render(ProcessId pid, Handle context, ConstByteView commands, ArrayView<const Handle> allocation_list);
  NtStatus present(ProcessId pid, Handle context, const PresentArgs& args);
  // Is everything queued on the context done?
  NtStatus context_idle(ProcessId pid, Handle context, bool* idle);

  // --- Shaders and pipelines directly (escape() is the same through the private-data ABI).
  NtStatus create_shader(ProcessId pid, Handle device, const ShaderDesc& desc, Handle* shader);
  NtStatus create_pipeline(ProcessId pid, Handle device, const PipelineDesc& desc, Handle* pipeline);
  NtStatus destroy_shader(ProcessId pid, Handle shader);
  NtStatus destroy_pipeline(ProcessId pid, Handle pipeline);
  NtStatus escape(ProcessId pid, Handle device, ConstByteView request, ByteView reply, uint32_t* reply_bytes);

  // Runs every adapter's queues. Called by every call that can unblock work; a kernel thread may call it too.
  bool pump();

 private:
  struct Process;
  struct ShareToken;
  Process* find_process(ProcessId pid);
  bool pump_locked();
  void destroy_device_locked(Process& p, Handle device);
  void close_process_locked(ProcessId pid);
  NtStatus create_shader_locked(Process& p, Handle device, const ShaderDesc& desc, Handle* shader);
  NtStatus create_pipeline_locked(Process& p, Handle device, const PipelineDesc& desc, Handle* pipeline);

  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;
  IWaitHook* hook_ = nullptr;
  uint32_t next_luid_ = 1, next_token_ = 0x1000;
  // (declared in the order the parts must be destroyed in, last to first: the processes and tokens hold references into the adapters)
  List<UniquePointer<Adapter>> adapters_;
  List<UniquePointer<ShareToken>> tokens_;
  List<UniquePointer<Process>> processes_;
};

// The system's one kernel-mode Direct3D driver (set by main once the GPUs are known); null before that.
void set_system_kernel(Kernel* kernel);
Kernel* system_kernel();

}  // namespace dxgk
