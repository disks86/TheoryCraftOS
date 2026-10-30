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

#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace dxgk {

namespace {
Kernel* g_system_kernel = nullptr;
constexpr uint32_t kMaxAllocationList = 1024;
}

// What a process's handles name. Each record says which device it was made on, so that destroying the device takes its objects with it.
struct AdapterRec {
  Adapter* adapter = nullptr;
};
struct AllocRec {
  SharedPointer<Allocation> allocation;
  Handle device = 0;
  bool locked = false, read_only = false;
  bool acquired = false;   // lent out by acquire_image (and pinned)
};
struct ContextRec {
  SharedPointer<ContextObject> context;
  Handle device = 0;
};
struct SyncRec {
  SharedPointer<SyncObject> sync;
  Handle device = 0;
};
struct ShaderRec {
  SharedPointer<ShaderObject> shader;
  Handle device = 0;
};
struct PipelineRec {
  SharedPointer<PipelineObject> pipeline;
  Handle device = 0;
};

struct Kernel::ShareToken {
  uint32_t token = 0;
  ProcessId owner = 0;
  SharedPointer<Allocation> allocation;
};

struct Kernel::Process {
  explicit Process(allocators::IAllocator& allocator, ProcessId pid)
      : pid(pid), adapters(allocator, 1), devices(allocator, 2), contexts(allocator, 3), allocations(allocator, 4), syncs(allocator, 5), shaders(allocator, 6), pipelines(allocator, 7) {}
  ProcessId pid;
  HandleTable<AdapterRec> adapters;
  HandleTable<DeviceObject> devices;
  HandleTable<ContextRec> contexts;
  HandleTable<AllocRec> allocations;
  HandleTable<SyncRec> syncs;
  HandleTable<ShaderRec> shaders;
  HandleTable<PipelineRec> pipelines;
};

namespace {

// The pipelines a stream may bind: this process's, made on the context's device.
class DeviceLookup final : public IPipelineLookup {
 public:
  DeviceLookup(const HandleTable<PipelineRec>& table, Handle device) : table_(table), device_(device) {}
  SharedPointer<PipelineObject> find_pipeline(Handle handle) override {
    SharedPointer<PipelineRec> rec = table_.get(handle);
    return rec && rec->device == device_ ? rec->pipeline : SharedPointer<PipelineObject>();
  }

 private:
  const HandleTable<PipelineRec>& table_;
  Handle device_;
};

// The handles in `table` whose record was made on `device`.
template <class Table>
bool handles_of_device(const Table& table, Handle device, List<Handle>& out) {
  bool ok = true;
  table.for_each([&](Handle h, const auto& rec) {
    if (rec->device == device) ok &= out.push_back(h);
  });
  return ok;
}

}  // namespace

Kernel::Kernel(allocators::IAllocator& allocator) : allocator_(allocator), adapters_(allocator), tokens_(allocator), processes_(allocator) {}

Kernel::~Kernel() {
  kernel::ScopedMutex g(lock_);
  while (processes_.count()) close_process_locked(processes_[0]->pid);
}

void Kernel::set_wait_hook(IWaitHook* hook) {
  kernel::ScopedMutex g(lock_);
  hook_ = hook;
}

uint32_t Kernel::add_adapter(graphics::IGraphicsDevice& device, const AdapterConfig& config) {
  kernel::ScopedMutex g(lock_);
  if (adapters_.count() >= kMaxAdapters) return kInvalidIndex;
  const uint32_t index = static_cast<uint32_t>(adapters_.count());
  Luid luid = config.luid;
  if (luid.low == 0 && luid.high == 0) luid.low = next_luid_++;
  UniquePointer<Adapter> a = UniquePointer<Adapter>::make(allocator_, allocator_, device, config, index, luid);
  if (!a || !adapters_.push_back(util::move(a))) return kInvalidIndex;
  return index;
}

uint32_t Kernel::adapter_count() {
  kernel::ScopedMutex g(lock_);
  return static_cast<uint32_t>(adapters_.count());
}

Adapter* Kernel::adapter(uint32_t index) {
  kernel::ScopedMutex g(lock_);
  return index < adapters_.count() ? adapters_[index].get() : nullptr;
}

// --- Processes -----------------------------------------------------------------------------------------------------------------------------------------------

Kernel::Process* Kernel::find_process(ProcessId pid) {
  for (UniquePointer<Process>& p : processes_.view())
    if (p->pid == pid) return p.get();
  return nullptr;
}

NtStatus Kernel::open_process(ProcessId pid) {
  kernel::ScopedMutex g(lock_);
  if (find_process(pid)) return status::kSuccess;
  if (processes_.count() >= kMaxProcesses) return status::kInsufficientResources;
  UniquePointer<Process> p = UniquePointer<Process>::make(allocator_, allocator_, pid);
  if (!p || !processes_.push_back(util::move(p))) return status::kNoMemory;
  return status::kSuccess;
}

void Kernel::close_process(ProcessId pid) {
  kernel::ScopedMutex g(lock_);
  close_process_locked(pid);
}

void Kernel::close_process_locked(ProcessId pid) {
  Process* p = find_process(pid);
  if (!p) return;
  // The tokens the process made for others go; what others already opened stays alive in their tables.
  for (uint64_t i = tokens_.count(); i-- > 0;)
    if (tokens_[i]->owner == pid) tokens_.remove_at(i);
  for (;;) {
    Handle first = kNullHandle;
    p->devices.for_each([&](Handle h, const SharedPointer<DeviceObject>&) {
      if (!first) first = h;
    });
    if (!first) break;
    destroy_device_locked(*p, first);
  }
  for (uint64_t i = 0; i < processes_.count(); ++i)
    if (processes_[i].get() == p) {
      processes_.remove_at(i);
      break;
    }
  pump_locked();   // (fence signals the process owed, work other processes waited on it for)
}

bool Kernel::pump_locked() {
  bool any = false;
  for (UniquePointer<Adapter>& a : adapters_.view()) any |= a->pump();
  if (any && hook_) hook_->wake();
  return any;
}

bool Kernel::pump() {
  kernel::ScopedMutex g(lock_);
  return pump_locked();
}

// --- Adapters ------------------------------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::enum_adapters(ProcessId pid, ArrayView<AdapterEntry> out, uint32_t* total) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p) return status::kInvalidParameter;
  *total = static_cast<uint32_t>(adapters_.count());
  uint32_t filled = 0;
  for (UniquePointer<Adapter>& a : adapters_.view()) {
    if (filled >= out.size()) return status::kBufferTooSmall;
    SharedPointer<AdapterRec> rec = SharedPointer<AdapterRec>::make(allocator_);
    if (!rec) return status::kNoMemory;
    rec->adapter = a.get();
    const Handle h = p->adapters.insert(rec);
    if (!h) return status::kInsufficientResources;
    out[filled++] = {h, a->luid(), a->index()};
  }
  return status::kSuccess;
}

NtStatus Kernel::open_adapter_from_index(ProcessId pid, uint32_t index, Handle* adapter) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p) return status::kInvalidParameter;
  if (index >= adapters_.count()) return status::kNotFound;
  SharedPointer<AdapterRec> rec = SharedPointer<AdapterRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->adapter = adapters_[index].get();
  *adapter = p->adapters.insert(rec);
  return *adapter ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::open_adapter_from_luid(ProcessId pid, Luid luid, Handle* adapter) {
  uint32_t index = kInvalidIndex;
  {
    kernel::ScopedMutex g(lock_);
    for (UniquePointer<Adapter>& a : adapters_.view())
      if (a->luid() == luid) index = a->index();
  }
  if (index == kInvalidIndex) return status::kNotFound;
  return open_adapter_from_index(pid, index, adapter);
}

NtStatus Kernel::close_adapter(ProcessId pid, Handle adapter) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->adapters.remove(adapter) ? status::kSuccess : status::kInvalidHandle;
}

NtStatus Kernel::query_adapter_caps(ProcessId pid, Handle adapter, AdapterCaps* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  *out = rec->adapter->caps();
  return status::kSuccess;
}

NtStatus Kernel::query_video_memory_info(ProcessId pid, Handle adapter, MemorySegmentGroup group, VideoMemoryInfo* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  VidMm& mm = rec->adapter->vidmm();
  *out = VideoMemoryInfo();
  if (group == MemorySegmentGroup::NonLocal) {
    // (system memory: what the backings may use)
    out->budget = 0;
    return status::kSuccess;
  }
  uint64_t mine = 0;
  p->allocations.for_each([&](Handle, const SharedPointer<AllocRec>& r) {
    if (&r->allocation->manager() == &mm && r->allocation->resident()) mine += r->allocation->device_bytes();
  });
  out->budget = mm.budget();
  out->current_usage = mine;
  out->available_for_reservation = mm.budget() - mm.resident_bytes() < mm.budget() ? mm.budget() - mm.resident_bytes() : 0;
  return status::kSuccess;
}

NtStatus Kernel::display_mode_count(ProcessId pid, Handle adapter, uint32_t* count) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!rec->adapter->display()) return status::kNotSupported;
  *count = rec->adapter->display()->mode_count();
  return status::kSuccess;
}

NtStatus Kernel::display_mode(ProcessId pid, Handle adapter, uint32_t index, DisplayModeInfo* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!rec->adapter->display()) return status::kNotSupported;
  hardware::DisplayMode m;
  if (!rec->adapter->display()->mode_info(index, &m)) return status::kInvalidParameter;
  *out = {m.width, m.height};
  return status::kSuccess;
}

NtStatus Kernel::current_display_mode(ProcessId pid, Handle adapter, uint32_t* index) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!rec->adapter->display()) return status::kNotSupported;
  *index = rec->adapter->display()->current_mode();
  return status::kSuccess;
}

NtStatus Kernel::set_display_mode(ProcessId pid, Handle adapter, uint32_t index) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!rec->adapter->display()) return status::kNotSupported;
  if (index >= rec->adapter->display()->mode_count()) return status::kInvalidParameter;
  return rec->adapter->display()->set_mode(index) ? status::kSuccess : status::kNotSupported;
}

// --- Devices and contexts --------------------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::create_device(ProcessId pid, Handle adapter, Handle* device) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AdapterRec* rec = p ? p->adapters.raw(adapter) : nullptr;
  if (!rec) return status::kInvalidHandle;
  SharedPointer<DeviceObject> d = SharedPointer<DeviceObject>::make(allocator_, *rec->adapter);
  if (!d) return status::kNoMemory;
  *device = p->devices.insert(d);
  return *device ? status::kSuccess : status::kInsufficientResources;
}

// Destroys a device and everything made on it, in the order the pieces depend on each other.
void Kernel::destroy_device_locked(Process& p, Handle device) {
  List<Handle> doomed(allocator_);
  if (handles_of_device(p.contexts, device, doomed))
    for (Handle h : doomed.view()) {
      SharedPointer<ContextRec> rec = p.contexts.get(h);
      rec->context->device->adapter().destroy_context(rec->context);
      p.contexts.remove(h);
    }
  doomed.clear();
  if (handles_of_device(p.pipelines, device, doomed))
    for (Handle h : doomed.view()) p.pipelines.remove(h);
  doomed.clear();
  if (handles_of_device(p.shaders, device, doomed))
    for (Handle h : doomed.view()) p.shaders.remove(h);
  doomed.clear();
  if (handles_of_device(p.allocations, device, doomed))
    for (Handle h : doomed.view()) {
      SharedPointer<AllocRec> rec = p.allocations.get(h);
      if (rec->locked) rec->allocation->manager().lock_released(*rec->allocation);
      p.allocations.remove(h);
    }
  doomed.clear();
  if (handles_of_device(p.syncs, device, doomed))
    for (Handle h : doomed.view()) p.syncs.remove(h);
  p.devices.remove(device);
}

NtStatus Kernel::destroy_device(ProcessId pid, Handle device) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p || !p->devices.raw(device)) return status::kInvalidHandle;
  destroy_device_locked(*p, device);
  pump_locked();
  return status::kSuccess;
}

NtStatus Kernel::get_device_state(ProcessId pid, Handle device, DeviceState* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  *out = d->state;
  return status::kSuccess;
}

NtStatus Kernel::create_context(ProcessId pid, Handle device, ContextPriority priority, Handle* context) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<DeviceObject> d = p ? p->devices.get(device) : SharedPointer<DeviceObject>();
  if (!d) return status::kInvalidHandle;
  SharedPointer<ContextObject> c;
  NtStatus s = d->adapter().create_context(d, priority, &c);
  if (s != status::kSuccess) return s;
  SharedPointer<ContextRec> rec = SharedPointer<ContextRec>::make(allocator_);
  if (!rec) {
    d->adapter().destroy_context(c);
    return status::kNoMemory;
  }
  rec->context = c;
  rec->device = device;
  *context = p->contexts.insert(rec);
  if (!*context) {
    d->adapter().destroy_context(c);
    return status::kInsufficientResources;
  }
  return status::kSuccess;
}

NtStatus Kernel::destroy_context(ProcessId pid, Handle context) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<ContextRec> rec = p ? p->contexts.get(context) : SharedPointer<ContextRec>();
  if (!rec) return status::kInvalidHandle;
  rec->context->device->adapter().destroy_context(rec->context);
  p->contexts.remove(context);
  pump_locked();
  return status::kSuccess;
}

// --- Allocations ---------------------------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::create_allocation(ProcessId pid, Handle device, const AllocationDesc& desc, Handle* allocation) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  SharedPointer<Allocation> a;
  NtStatus s = d->adapter().vidmm().create(desc, &a);
  if (s != status::kSuccess) return s;
  SharedPointer<AllocRec> rec = SharedPointer<AllocRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->allocation = a;
  rec->device = device;
  *allocation = p->allocations.insert(rec);
  return *allocation ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::destroy_allocation(ProcessId pid, Handle allocation) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<AllocRec> rec = p ? p->allocations.get(allocation) : SharedPointer<AllocRec>();
  if (!rec) return status::kInvalidHandle;
  if (rec->locked) rec->allocation->manager().lock_released(*rec->allocation);
  p->allocations.remove(allocation);
  return status::kSuccess;
}

NtStatus Kernel::query_allocation(ProcessId pid, Handle allocation, AllocationInfo* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  const Allocation& a = *rec->allocation;
  out->desc = a.desc();
  out->bytes = a.backing().size();
  out->pitch = a.pitch();
  out->resident = a.resident();
  out->shared = (a.desc().flags & kAllocShared) != 0;
  return status::kSuccess;
}

NtStatus Kernel::share_allocation(ProcessId pid, Handle allocation, uint32_t* token) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!(rec->allocation->desc().flags & kAllocShared)) return status::kAccessDenied;
  UniquePointer<ShareToken> t = UniquePointer<ShareToken>::make(allocator_);
  if (!t) return status::kNoMemory;
  t->token = next_token_++;
  t->owner = pid;
  t->allocation = rec->allocation;
  *token = t->token;
  return tokens_.push_back(util::move(t)) ? status::kSuccess : status::kNoMemory;
}

NtStatus Kernel::open_shared_allocation(ProcessId pid, Handle device, uint32_t token, Handle* allocation, AllocationDesc* desc) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  for (UniquePointer<ShareToken>& t : tokens_.view()) {
    if (t->token != token) continue;
    if (&t->allocation->manager() != &d->adapter().vidmm()) return status::kInvalidParameter;   // (another adapter's memory)
    SharedPointer<AllocRec> rec = SharedPointer<AllocRec>::make(allocator_);
    if (!rec) return status::kNoMemory;
    rec->allocation = t->allocation;
    rec->device = device;
    *allocation = p->allocations.insert(rec);
    if (desc) *desc = t->allocation->desc();
    return *allocation ? status::kSuccess : status::kInsufficientResources;
  }
  return status::kNotFound;
}

NtStatus Kernel::close_share(ProcessId pid, uint32_t token) {
  kernel::ScopedMutex g(lock_);
  for (uint64_t i = 0; i < tokens_.count(); ++i)
    if (tokens_[i]->token == token) {
      if (tokens_[i]->owner != pid) return status::kAccessDenied;
      tokens_.remove_at(i);
      return status::kSuccess;
    }
  return status::kNotFound;
}

NtStatus Kernel::lock(ProcessId pid, Handle allocation, uint32_t flags, uint32_t subresource, LockResult* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  Allocation& a = *rec->allocation;
  if (flags & ~(kLockReadOnly | kLockDoNotWait | kLockDiscard)) return status::kInvalidParameter;
  if (rec->locked) return status::kInvalidDeviceState;
  if (a.is_depth()) return status::kNotSupported;
  uint64_t offset = 0, bytes = 0;
  if (!a.level_range(subresource, &offset, &bytes)) return status::kInvalidParameter;
  // Queued work that uses the allocation has to be done before the CPU may touch it. The queues run on their own as soon as they can, so what is still queued is waiting for a fence.
  Adapter* adapter = nullptr;
  for (UniquePointer<Adapter>& ad : adapters_.view())
    if (&ad->vidmm() == &a.manager()) adapter = ad.get();
  if (adapter) {
    adapter->pump();
    if (adapter->allocation_in_use(a)) return status::kGraphicsAllocationBusy;
  }
  if (flags & kLockDiscard) a.manager().discard_gpu_contents(a);
  else if (!a.manager().read_back(a)) return status::kInvalidDeviceState;
  ByteView data;
  if (!a.writable_backing().try_subview(offset, bytes, &data)) return status::kInvalidDeviceState;
  a.manager().lock_taken(a);
  rec->locked = true;
  rec->read_only = (flags & kLockReadOnly) != 0;
  out->data = data;
  out->pitch = a.is_image() ? graphics::mip_extent(a.desc().width, subresource) * pixel_bytes(a.desc().format) : 0;
  return status::kSuccess;
}

NtStatus Kernel::unlock(ProcessId pid, Handle allocation) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!rec->locked) return status::kInvalidDeviceState;
  if (!rec->read_only) rec->allocation->manager().note_cpu_write(*rec->allocation);
  rec->allocation->manager().lock_released(*rec->allocation);
  rec->locked = false;
  return status::kSuccess;
}

NtStatus Kernel::acquire_image(ProcessId pid, Handle allocation, AcquiredImage* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec || !out) return status::kInvalidHandle;
  Allocation& a = *rec->allocation;
  if (!a.is_image() || a.is_depth() || a.is_staging() || !(a.desc().flags & kAllocTexture) || !(a.desc().flags & kAllocRenderTarget)) return status::kNotSupported;
  if (rec->locked || rec->acquired) return status::kInvalidDeviceState;
  Adapter* adapter = nullptr;
  for (UniquePointer<Adapter>& ad : adapters_.view())
    if (&ad->vidmm() == &a.manager()) adapter = ad.get();
  if (!adapter) return status::kInvalidDeviceState;
  adapter->pump();
  if (adapter->allocation_in_use(a)) return status::kGraphicsAllocationBusy;
  const NtStatus s = a.manager().make_resident(a);
  if (s != status::kSuccess) return s;
  if (!a.image()) return status::kInvalidDeviceState;
  a.manager().pin(a);
  rec->acquired = true;
  out->device = &adapter->device();
  out->image = a.image();
  return status::kSuccess;
}

NtStatus Kernel::release_image(ProcessId pid, Handle allocation) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!rec->acquired) return status::kInvalidDeviceState;
  rec->allocation->manager().unpin(*rec->allocation);
  rec->acquired = false;
  return status::kSuccess;
}

NtStatus Kernel::make_resident(ProcessId pid, Handle device, ArrayView<const Handle> allocations) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  if (allocations.size() > kMaxAllocationList) return status::kInvalidParameter;
  List<SharedPointer<Allocation>> list(allocator_);
  for (Handle h : allocations) {
    AllocRec* rec = p->allocations.raw(h);
    if (!rec || &rec->allocation->manager() != &d->adapter().vidmm()) return status::kInvalidHandle;
    if (!list.push_back(rec->allocation)) return status::kNoMemory;
  }
  VidMm& mm = d->adapter().vidmm();
  for (SharedPointer<Allocation>& a : list.view()) mm.pin(*a);   // (so that making the later ones resident does not push the earlier ones out)
  NtStatus s = status::kSuccess;
  for (SharedPointer<Allocation>& a : list.view()) {
    s = mm.make_resident(*a);
    if (s != status::kSuccess) break;
  }
  for (SharedPointer<Allocation>& a : list.view()) mm.unpin(*a);
  return s;
}

NtStatus Kernel::evict(ProcessId pid, Handle device, ArrayView<const Handle> allocations) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  for (Handle h : allocations) {
    AllocRec* rec = p->allocations.raw(h);
    if (!rec || &rec->allocation->manager() != &d->adapter().vidmm()) return status::kInvalidHandle;
    d->adapter().vidmm().evict(*rec->allocation);
  }
  return status::kSuccess;
}

NtStatus Kernel::trim(ProcessId pid, Handle device) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  p->allocations.for_each([&](Handle, const SharedPointer<AllocRec>& rec) {
    if (rec->device == device) d->adapter().vidmm().evict(*rec->allocation);
  });
  return status::kSuccess;
}

// --- Synchronization -------------------------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::create_sync(ProcessId pid, Handle device, SyncType type, uint64_t initial, uint64_t limit, Handle* sync) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p || !p->devices.raw(device)) return status::kInvalidHandle;
  if (type != SyncType::Fence && type != SyncType::Semaphore) return status::kInvalidParameter;
  if (type == SyncType::Semaphore && (limit == 0 || initial > limit)) return status::kInvalidParameter;
  SharedPointer<SyncRec> rec = SharedPointer<SyncRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->sync = SharedPointer<SyncObject>::make(allocator_, type, initial, limit);
  if (!rec->sync) return status::kNoMemory;
  rec->device = device;
  *sync = p->syncs.insert(rec);
  return *sync ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::destroy_sync(ProcessId pid, Handle sync) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->syncs.remove(sync) ? status::kSuccess : status::kInvalidHandle;
}

NtStatus Kernel::query_sync(ProcessId pid, Handle sync, uint64_t* value) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SyncRec* rec = p ? p->syncs.raw(sync) : nullptr;
  if (!rec) return status::kInvalidHandle;
  *value = rec->sync->value();
  return status::kSuccess;
}

NtStatus Kernel::signal_sync_cpu(ProcessId pid, ArrayView<const Handle> syncs, ArrayView<const uint64_t> values) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p) return status::kInvalidParameter;
  if (syncs.size() != values.size() || syncs.empty() || syncs.size() > kMaxSyncWait) return status::kInvalidParameter;
  SharedPointer<SyncObject> objects[kMaxSyncWait];
  for (uint64_t i = 0; i < syncs.size(); ++i) {
    SyncRec* rec = p->syncs.raw(syncs[i]);
    if (!rec) return status::kInvalidHandle;
    objects[i] = rec->sync;
  }
  // Everything or nothing: a fence cannot go back, a semaphore cannot go past its limit.
  for (uint64_t i = 0; i < syncs.size(); ++i) {
    SyncObject probe = *objects[i];
    if (!probe.signal(values[i])) return status::kInvalidParameter;
  }
  for (uint64_t i = 0; i < syncs.size(); ++i) objects[i]->signal(values[i]);
  pump_locked();
  if (hook_) hook_->wake();
  return status::kSuccess;
}

NtStatus Kernel::wait_sync_cpu(ProcessId pid, ArrayView<const Handle> syncs, ArrayView<const uint64_t> values, bool wait_all, uint64_t timeout_ns) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p) return status::kInvalidParameter;
  if (syncs.size() != values.size() || syncs.empty() || syncs.size() > kMaxSyncWait) return status::kInvalidParameter;
  SharedPointer<SyncObject> objects[kMaxSyncWait];
  const uint64_t n = syncs.size();
  for (uint64_t i = 0; i < n; ++i) {
    SyncRec* rec = p->syncs.raw(syncs[i]);
    if (!rec) return status::kInvalidHandle;
    objects[i] = rec->sync;
  }
  const uint64_t start = hook_ ? hook_->now_ns() : 0;
  for (;;) {
    pump_locked();
    uint64_t ready = 0;
    uint64_t first = n;
    for (uint64_t i = 0; i < n; ++i)
      if (objects[i]->satisfied(values[i])) {
        ++ready;
        if (first == n) first = i;
      }
    if (wait_all ? ready == n : ready > 0) {
      if (wait_all) {
        for (uint64_t i = 0; i < n; ++i) objects[i]->consume();
      } else {
        objects[first]->consume();
      }
      return status::kSuccess;
    }
    if (timeout_ns == 0 || !hook_) return status::kTimeout;
    uint64_t remaining = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t elapsed = hook_->now_ns() - start;
      if (elapsed >= timeout_ns) return status::kTimeout;
      remaining = timeout_ns - elapsed;
    }
    lock_.unlock();
    hook_->wait(remaining);
    lock_.lock();
  }
}

NtStatus Kernel::signal_sync_gpu(ProcessId pid, Handle context, Handle sync, uint64_t value) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<ContextRec> c = p ? p->contexts.get(context) : SharedPointer<ContextRec>();
  SharedPointer<SyncRec> s = p ? p->syncs.get(sync) : SharedPointer<SyncRec>();
  if (!c || !s) return status::kInvalidHandle;
  const NtStatus st = c->context->device->adapter().queue_signal(*c->context, s->sync, value);
  if (st == status::kSuccess) pump_locked();
  return st;
}

NtStatus Kernel::wait_sync_gpu(ProcessId pid, Handle context, Handle sync, uint64_t value) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<ContextRec> c = p ? p->contexts.get(context) : SharedPointer<ContextRec>();
  SharedPointer<SyncRec> s = p ? p->syncs.get(sync) : SharedPointer<SyncRec>();
  if (!c || !s) return status::kInvalidHandle;
  const NtStatus st = c->context->device->adapter().queue_wait(*c->context, s->sync, value);
  if (st == status::kSuccess) pump_locked();
  return st;
}

// --- Work -------------------------------------------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::render(ProcessId pid, Handle context, ConstByteView commands, ArrayView<const Handle> allocation_list) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<ContextRec> c = p ? p->contexts.get(context) : SharedPointer<ContextRec>();
  if (!c) return status::kInvalidHandle;
  if (allocation_list.size() > kMaxAllocationList) return status::kInvalidParameter;
  Adapter& adapter = c->context->device->adapter();
  List<SharedPointer<Allocation>> list(allocator_);
  for (Handle h : allocation_list) {
    AllocRec* rec = p->allocations.raw(h);
    if (!rec) return status::kInvalidHandle;
    if (&rec->allocation->manager() != &adapter.vidmm()) return status::kInvalidParameter;
    if (!list.push_back(rec->allocation)) return status::kNoMemory;
  }
  DeviceLookup lookup(p->pipelines, c->device);
  const NtStatus st = adapter.queue_render(*c->context, commands, ArrayView<const SharedPointer<Allocation>>(list.view().data(), list.count()), lookup);
  if (st == status::kSuccess) pump_locked();
  return st;
}

NtStatus Kernel::present(ProcessId pid, Handle context, const PresentArgs& args) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<ContextRec> c = p ? p->contexts.get(context) : SharedPointer<ContextRec>();
  if (!c) return status::kInvalidHandle;
  Adapter& adapter = c->context->device->adapter();
  SharedPointer<AllocRec> source = p->allocations.get(args.source);
  SharedPointer<AllocRec> destination = args.destination ? p->allocations.get(args.destination) : SharedPointer<AllocRec>();
  if (!source || (args.destination && !destination)) return status::kInvalidHandle;
  if (&source->allocation->manager() != &adapter.vidmm() || (destination && &destination->allocation->manager() != &adapter.vidmm())) return status::kInvalidParameter;
  const NtStatus st = adapter.queue_present(*c->context, args, source->allocation, destination ? destination->allocation : SharedPointer<Allocation>());
  if (st == status::kSuccess) pump_locked();
  return st;
}

NtStatus Kernel::context_idle(ProcessId pid, Handle context, bool* idle) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<ContextRec> c = p ? p->contexts.get(context) : SharedPointer<ContextRec>();
  if (!c) return status::kInvalidHandle;
  pump_locked();
  *idle = c->context->device->adapter().context_idle(*c->context);
  return status::kSuccess;
}

// --- Shaders, pipelines and the escape ----------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::create_shader_locked(Process& p, Handle device, const ShaderDesc& desc, Handle* shader) {
  DeviceObject* d = p.devices.raw(device);
  if (!d) return status::kInvalidHandle;
  SharedPointer<ShaderObject> s;
  NtStatus st = d->adapter().create_shader(desc, &s);
  if (st != status::kSuccess) return st;
  SharedPointer<ShaderRec> rec = SharedPointer<ShaderRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->shader = s;
  rec->device = device;
  *shader = p.shaders.insert(rec);
  return *shader ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::create_pipeline_locked(Process& p, Handle device, const PipelineDesc& desc, Handle* pipeline) {
  DeviceObject* d = p.devices.raw(device);
  if (!d) return status::kInvalidHandle;
  if (desc.compute_shader) {
    SharedPointer<ShaderRec> cs = p.shaders.get(desc.compute_shader);
    if (!cs || cs->device != device) return status::kInvalidHandle;
    SharedPointer<PipelineObject> compute;
    const NtStatus ct = d->adapter().create_compute_pipeline(cs->shader, &compute);
    if (ct != status::kSuccess) return ct;
    SharedPointer<PipelineRec> crec = SharedPointer<PipelineRec>::make(allocator_);
    if (!crec) return status::kNoMemory;
    crec->pipeline = compute;
    crec->device = device;
    *pipeline = p.pipelines.insert(crec);
    return *pipeline ? status::kSuccess : status::kInsufficientResources;
  }
  SharedPointer<ShaderRec> vs = p.shaders.get(desc.vertex_shader), fs = p.shaders.get(desc.fragment_shader);
  if (!vs || !fs || vs->device != device || fs->device != device) return status::kInvalidHandle;
  SharedPointer<PipelineObject> pipe;
  NtStatus st = d->adapter().create_pipeline(desc, vs->shader, fs->shader, &pipe);
  if (st != status::kSuccess) return st;
  SharedPointer<PipelineRec> rec = SharedPointer<PipelineRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->pipeline = pipe;
  rec->device = device;
  *pipeline = p.pipelines.insert(rec);
  return *pipeline ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::create_shader(ProcessId pid, Handle device, const ShaderDesc& desc, Handle* shader) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p ? create_shader_locked(*p, device, desc, shader) : status::kInvalidParameter;
}

NtStatus Kernel::create_pipeline(ProcessId pid, Handle device, const PipelineDesc& desc, Handle* pipeline) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p ? create_pipeline_locked(*p, device, desc, pipeline) : status::kInvalidParameter;
}

NtStatus Kernel::destroy_shader(ProcessId pid, Handle shader) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->shaders.remove(shader) ? status::kSuccess : status::kInvalidHandle;
}

NtStatus Kernel::destroy_pipeline(ProcessId pid, Handle pipeline) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->pipelines.remove(pipeline) ? status::kSuccess : status::kInvalidHandle;
}

NtStatus Kernel::escape(ProcessId pid, Handle device, ConstByteView request, ByteView reply, uint32_t* reply_bytes) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  *reply_bytes = 0;
  ByteReader in(request);
  ByteWriter out(reply);
  const uint32_t op = in.le32();
  if (!in.ok()) return status::kInvalidParameter;
  switch (static_cast<EscapeOp>(op)) {
    case EscapeOp::CreateShader: {
      ShaderDesc desc;
      const uint32_t kind = in.le32(), stage = in.le32(), bytes = in.le32();
      if (!in.ok() || kind > static_cast<uint32_t>(ShaderKind::Dxbc) || stage > 2) return status::kInvalidParameter;
      desc.kind = static_cast<ShaderKind>(kind);
      desc.stage = static_cast<graphics::ShaderStage>(stage);
      desc.code = in.take(bytes);
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      Handle h = 0;
      const NtStatus st = create_shader_locked(*p, device, desc, &h);
      if (st != status::kSuccess) return st;
      out.le32(h);
      break;
    }
    case EscapeOp::CreatePipeline: {
      PipelineDesc desc;
      desc.vertex_shader = in.le32();
      desc.fragment_shader = in.le32();
      desc.vertex_stride = in.le32();
      desc.attribute_count = in.le32();
      desc.varying_count = in.le32();
      desc.texture_count = in.le32();
      const uint32_t cull = in.le32(), front = in.le32(), flags = in.le32();
      for (graphics::VertexAttribute& a : desc.attributes) {
        a.location = in.le32();
        const uint32_t format = in.le32();
        a.offset = in.le32();
        if (format > static_cast<uint32_t>(graphics::VertexFormat::Float4)) return status::kInvalidParameter;
        a.format = static_cast<graphics::VertexFormat>(format);
      }
      const uint32_t enable = in.le32(), compare = in.le32(), fail = in.le32(), depth_fail = in.le32(), pass = in.le32();
      const uint32_t compare_mask = in.le32(), write_mask = in.le32(), reference = in.le32();
      // Optional trailing words (the state of Direct3D 10): depth_compare, then blend custom, enable, the four factors, the two ops, the write mask, and a bit per attribute for
      // per_instance (then the number of colour outputs, then independent blending). Their absence is the base state.
      if (!in.ok()) return status::kInvalidParameter;
      if (!in.at_end()) {
        const uint32_t depth_compare = in.le32(), custom = in.le32(), blend_enable = in.le32(), sc = in.le32(), dc = in.le32(), sa = in.le32(), da = in.le32(), cop = in.le32(), aop = in.le32(),
                       mask = in.le32(), instance_bits = in.le32();
        if (!in.ok() || depth_compare > 7 || custom > 1 || blend_enable > 1 || sc > 10 || dc > 10 || sa > 10 || da > 10 || cop > 4 || aop > 4 || mask > 15 || instance_bits > 15) return status::kInvalidParameter;
        desc.depth_compare = static_cast<graphics::CompareOp>(depth_compare);
        desc.blend.custom = custom != 0, desc.blend.enable = blend_enable != 0;
        desc.blend.src_color = static_cast<graphics::BlendFactor>(sc), desc.blend.dst_color = static_cast<graphics::BlendFactor>(dc);
        desc.blend.src_alpha = static_cast<graphics::BlendFactor>(sa), desc.blend.dst_alpha = static_cast<graphics::BlendFactor>(da);
        desc.blend.color_op = static_cast<graphics::BlendOp>(cop), desc.blend.alpha_op = static_cast<graphics::BlendOp>(aop);
        desc.blend.write_mask = static_cast<uint8_t>(mask);
        for (uint32_t i = 0; i < graphics::kMaxVertexAttributes; ++i) desc.attributes[i].per_instance = (instance_bits >> i) & 1;
        if (!in.at_end()) {   // (and, after that, the number of colour outputs)
          desc.color_target_count = in.le32();
          if (!in.ok() || desc.color_target_count == 0 || desc.color_target_count > graphics::kMaxColorTargets) return status::kInvalidParameter;
          if (!in.at_end()) {   // (and, after that, independent blending: a flag, then for targets 1 .. 3 the nine words of the blend state)
            const uint32_t independent = in.le32();
            if (!in.ok() || independent > 1) return status::kInvalidParameter;
            desc.independent_blend = independent != 0;
            for (graphics::BlendState& t : desc.target_blend) {
              const uint32_t tc = in.le32(), te = in.le32(), tsc = in.le32(), tdc = in.le32(), tsa = in.le32(), tda = in.le32(), tcop = in.le32(), taop = in.le32(), tmask = in.le32();
              if (!in.ok() || tc > 1 || te > 1 || tsc > 10 || tdc > 10 || tsa > 10 || tda > 10 || tcop > 4 || taop > 4 || tmask > 15) return status::kInvalidParameter;
              t.custom = tc != 0, t.enable = te != 0;
              t.src_color = static_cast<graphics::BlendFactor>(tsc), t.dst_color = static_cast<graphics::BlendFactor>(tdc);
              t.src_alpha = static_cast<graphics::BlendFactor>(tsa), t.dst_alpha = static_cast<graphics::BlendFactor>(tda);
              t.color_op = static_cast<graphics::BlendOp>(tcop), t.alpha_op = static_cast<graphics::BlendOp>(taop);
              t.write_mask = static_cast<uint8_t>(tmask);
            }
          }
        }
      }
      if (!in.ok() || !in.at_end() || cull > 2 || front > 1 || compare > 7 || fail > 7 || depth_fail > 7 || pass > 7 || compare_mask > 255 || write_mask > 255 || reference > 255 || enable > 1)
        return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      desc.cull_mode = static_cast<graphics::CullMode>(cull);
      desc.front_face = static_cast<graphics::FrontFace>(front);
      desc.depth_test = (flags & 1) != 0;
      desc.depth_write = (flags & 2) != 0;
      desc.alpha_blend = (flags & 4) != 0;
      desc.stencil.enable = enable != 0;
      desc.stencil.compare = static_cast<graphics::CompareOp>(compare);
      desc.stencil.fail_op = static_cast<graphics::StencilOp>(fail);
      desc.stencil.depth_fail_op = static_cast<graphics::StencilOp>(depth_fail);
      desc.stencil.pass_op = static_cast<graphics::StencilOp>(pass);
      desc.stencil.compare_mask = static_cast<uint8_t>(compare_mask);
      desc.stencil.write_mask = static_cast<uint8_t>(write_mask);
      desc.stencil.reference = static_cast<uint8_t>(reference);
      Handle h = 0;
      const NtStatus st = create_pipeline_locked(*p, device, desc, &h);
      if (st != status::kSuccess) return st;
      out.le32(h);
      break;
    }
    case EscapeOp::CreateComputePipeline: {
      PipelineDesc desc;
      desc.compute_shader = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      Handle h = 0;
      const NtStatus st = create_pipeline_locked(*p, device, desc, &h);
      if (st != status::kSuccess) return st;
      out.le32(h);
      break;
    }
    case EscapeOp::DestroyShader:
    case EscapeOp::DestroyPipeline: {
      const Handle h = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      const bool removed = static_cast<EscapeOp>(op) == EscapeOp::DestroyShader ? p->shaders.remove(h) : p->pipelines.remove(h);
      return removed ? status::kSuccess : status::kInvalidHandle;
    }
    default: return status::kNotSupported;
  }
  if (!out.ok()) return status::kBufferTooSmall;
  *reply_bytes = static_cast<uint32_t>(out.position());
  return status::kSuccess;
}

void set_system_kernel(Kernel* kernel) { g_system_kernel = kernel; }
Kernel* system_kernel() { return g_system_kernel; }

}  // namespace dxgk
