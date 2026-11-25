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
#include "Kernel/Dxgkrnl/CommandStream.hpp"
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

struct SignatureRec {
  SharedPointer<RootSignatureObject> signature;
  Handle device = 0;
};
struct HeapRec {
  SharedPointer<DescriptorHeapObject> heap;
  Handle device = 0;
};
struct VideoDecoderRec {
  SharedPointer<VideoDecoderObject> decoder;
  Handle device = 0;
};

struct Kernel::ShareToken {
  uint32_t token = 0;
  ProcessId owner = 0;
  SharedPointer<Allocation> allocation;
};

struct Kernel::Process {
  explicit Process(allocators::IAllocator& allocator, ProcessId pid)
      : pid(pid), adapters(allocator, 1), devices(allocator, 2), contexts(allocator, 3), allocations(allocator, 4), syncs(allocator, 5), shaders(allocator, 6), pipelines(allocator, 7), signatures(allocator, 8), heaps(allocator, 9),
        video_decoders(allocator, 10) {}
  ProcessId pid;
  HandleTable<AdapterRec> adapters;
  HandleTable<DeviceObject> devices;
  HandleTable<ContextRec> contexts;
  HandleTable<AllocRec> allocations;
  HandleTable<SyncRec> syncs;
  HandleTable<ShaderRec> shaders;
  HandleTable<PipelineRec> pipelines;
  HandleTable<SignatureRec> signatures;
  HandleTable<HeapRec> heaps;
  HandleTable<VideoDecoderRec> video_decoders;
};

namespace {

// The pipelines a stream may bind: this process's, made on the context's device.
class DeviceLookup final : public IPipelineLookup {
 public:
  DeviceLookup(const HandleTable<PipelineRec>& table, const HandleTable<SignatureRec>& signatures, const HandleTable<HeapRec>& heaps, const HandleTable<VideoDecoderRec>& decoders, Handle device)
      : table_(table), signatures_(signatures), heaps_(heaps), decoders_(decoders), device_(device) {}
  SharedPointer<PipelineObject> find_pipeline(Handle handle) override {
    SharedPointer<PipelineRec> rec = table_.get(handle);
    return rec && rec->device == device_ ? rec->pipeline : SharedPointer<PipelineObject>();
  }
  SharedPointer<RootSignatureObject> find_root_signature(Handle handle) override {
    SharedPointer<SignatureRec> rec = signatures_.get(handle);
    return rec && rec->device == device_ ? rec->signature : SharedPointer<RootSignatureObject>();
  }
  SharedPointer<DescriptorHeapObject> find_descriptor_heap(Handle handle) override {
    SharedPointer<HeapRec> rec = heaps_.get(handle);
    return rec && rec->device == device_ ? rec->heap : SharedPointer<DescriptorHeapObject>();
  }
  SharedPointer<VideoDecoderObject> find_video_decoder(Handle handle) override {
    SharedPointer<VideoDecoderRec> rec = decoders_.get(handle);
    return rec && rec->device == device_ ? rec->decoder : SharedPointer<VideoDecoderObject>();
  }

 private:
  const HandleTable<PipelineRec>& table_;
  const HandleTable<SignatureRec>& signatures_;
  const HandleTable<HeapRec>& heaps_;
  const HandleTable<VideoDecoderRec>& decoders_;
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
  if (a) a->set_present_tap(present_tap_);
  if (!a || !adapters_.push_back(util::move(a))) return kInvalidIndex;
  return index;
}

void Kernel::set_present_tap(IPresentTap* tap) {
  kernel::ScopedMutex g(lock_);
  present_tap_ = tap;
  for (UniquePointer<Adapter>& a : adapters_.view()) a->set_present_tap(tap);
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
  if (handles_of_device(p.signatures, device, doomed))
    for (Handle h : doomed.view()) p.signatures.remove(h);
  doomed.clear();
  if (handles_of_device(p.heaps, device, doomed))
    for (Handle h : doomed.view()) p.heaps.remove(h);
  doomed.clear();
  if (handles_of_device(p.video_decoders, device, doomed))
    for (Handle h : doomed.view()) p.video_decoders.remove(h);
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
  SharedPointer<Allocation> a, heap;
  if (desc.heap) {
    SharedPointer<AllocRec> h = p->allocations.get(desc.heap);
    if (!h || h->device != device) return status::kInvalidHandle;
    heap = h->allocation;
  }
  NtStatus s = d->adapter().vidmm().create(desc, &a, heap);
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

NtStatus Kernel::query_stream_output(ProcessId pid, Handle allocation, uint64_t* offset) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  if (!(rec->allocation->desc().flags & kAllocStreamOutput)) return status::kInvalidParameter;
  *offset = rec->allocation->stream_output_filled();
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

// --- Exports (Linux dmabufs) -----------------------------------------------------------------------------------------------------------------------------------

class ExportedAllocation {
 public:
  ExportedAllocation(Kernel& kernel, SharedPointer<Allocation> allocation, Adapter& adapter, const ExportInfo& info)
      : kernel_(kernel), allocation_(util::move(allocation)), adapter_(adapter), info_(info) {}
  // What the kernel's readers still hold is given back, and the allocation let go, under the kernel's lock (the video memory manager is not safe without it).
  ~ExportedAllocation() {
    kernel::ScopedMutex g(kernel_.lock_);
    for (; reads_; --reads_) allocation_->manager().lock_released(*allocation_);
    for (; acquires_; --acquires_) allocation_->manager().unpin(*allocation_);
    allocation_.reset();
  }
  ExportedAllocation(const ExportedAllocation&) = delete;
  ExportedAllocation& operator=(const ExportedAllocation&) = delete;

 private:
  friend class Kernel;
  friend const ExportInfo& export_info(const ExportedAllocation& exported);
  friend bool copy_export(ExportedAllocation& exported, ByteView bytes, bool to_image);
  friend bool export_idle(ExportedAllocation& exported);
  Kernel& kernel_;
  SharedPointer<Allocation> allocation_;
  Adapter& adapter_;
  const ExportInfo info_;
  uint32_t reads_ = 0, acquires_ = 0;      // (under the kernel's lock)
};

const ExportInfo& export_info(const ExportedAllocation& exported) { return exported.info_; }

bool export_idle(ExportedAllocation& exported) {
  kernel::ScopedMutex g(exported.kernel_.lock_);
  exported.adapter_.pump();
  return !exported.adapter_.allocation_in_use(*exported.allocation_);
}

bool copy_export(ExportedAllocation& exported, ByteView bytes, bool to_image) {
  if (!exported.info_.image) return false;      // (an NT handle's export: not a dmabuf's image)
  kernel::ScopedMutex g(exported.kernel_.lock_);
  Allocation& a = *exported.allocation_;
  exported.adapter_.pump();
  if (exported.adapter_.allocation_in_use(a)) return false;
  uint64_t offset = 0, level = 0;
  ByteView image;
  if (!a.level_range(0, &offset, &level) || !a.writable_backing().try_subview(offset, level, &image)) return false;
  const uint64_t n = image.size() < bytes.size() ? image.size() : bytes.size();
  if (to_image) {
    if (n < image.size()) {
      if (!a.manager().read_back(a)) return false;      // (the part not given stays what the GPU has)
    } else {
      a.manager().discard_gpu_contents(a);
    }
    image.first(n).copy_from(ConstByteView(bytes.first(n)));
    a.manager().note_cpu_write(a);
    return true;
  }
  if (!a.manager().read_back(a)) return false;
  bytes.first(n).copy_from(ConstByteView(image.first(n)));
  return true;
}

Adapter* Kernel::adapter_of_locked(const Allocation& a) {
  for (UniquePointer<Adapter>& ad : adapters_.view())
    if (&ad->vidmm() == &a.manager()) return ad.get();
  return nullptr;
}

namespace {
// The formats a compositor takes: 8 bits a channel, four channels.
bool exportable_format(Format f) {
  return f == Format::A8R8G8B8 || f == Format::X8R8G8B8 || f == Format::A8B8G8R8 || f == Format::X8B8G8R8 || f == Format::A8R8G8B8Srgb || f == Format::A8B8G8R8Srgb;
}
}  // namespace

class ExportedSync {
 public:
  ExportedSync(Kernel& kernel, SharedPointer<SyncObject> sync) : kernel_(kernel), sync_(util::move(sync)) {}
  ~ExportedSync() {
    kernel::ScopedMutex g(kernel_.lock_);
    sync_.reset();
  }
  ExportedSync(const ExportedSync&) = delete;
  ExportedSync& operator=(const ExportedSync&) = delete;

 private:
  friend class Kernel;
  friend uint64_t exported_sync_value(ExportedSync& exported);
  friend void signal_exported_sync(ExportedSync& exported, uint64_t point);
  Kernel& kernel_;
  SharedPointer<SyncObject> sync_;
};

uint64_t exported_sync_value(ExportedSync& exported) {
  kernel::ScopedMutex g(exported.kernel_.lock_);
  return exported.sync_->value();
}

void signal_exported_sync(ExportedSync& exported, uint64_t point) {
  Kernel& k = exported.kernel_;
  kernel::ScopedMutex g(k.lock_);
  if (exported.sync_->value() >= point) return;
  exported.sync_->signal(point);
  k.pump_locked();
  if (k.hook_) k.hook_->wake();
}

NtStatus Kernel::export_sync(ProcessId pid, Handle sync, SharedPointer<ExportedSync>* out) {
  SharedPointer<ExportedSync> made;
  {
    kernel::ScopedMutex g(lock_);
    Process* p = find_process(pid);
    SyncRec* rec = p ? p->syncs.raw(sync) : nullptr;
    if (!rec) return status::kInvalidHandle;
    if (rec->sync->type() != SyncType::Fence) return status::kNotSupported;
    made = SharedPointer<ExportedSync>::make(allocator_, *this, rec->sync);
    if (!made) return status::kNoMemory;
  }
  *out = util::move(made);      // (with the lock let go, as export_allocation)
  return status::kSuccess;
}

NtStatus Kernel::import_sync(ProcessId pid, Handle device, const ExportedSync& exported, Handle* sync) {
  if (&exported.kernel_ != this) return status::kInvalidParameter;
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p || !p->devices.raw(device)) return status::kInvalidHandle;
  SharedPointer<SyncRec> rec = SharedPointer<SyncRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->sync = exported.sync_;
  rec->device = device;
  *sync = p->syncs.insert(rec);
  return *sync ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::export_allocation(ProcessId pid, Handle allocation, SharedPointer<ExportedAllocation>* out) { return export_locked_common(pid, allocation, true, out); }
NtStatus Kernel::export_shared_allocation(ProcessId pid, Handle allocation, SharedPointer<ExportedAllocation>* out) { return export_locked_common(pid, allocation, false, out); }

NtStatus Kernel::export_locked_common(ProcessId pid, Handle allocation, bool image_only, SharedPointer<ExportedAllocation>* out) {
  SharedPointer<ExportedAllocation> made;
  {
    kernel::ScopedMutex g(lock_);
    Process* p = find_process(pid);
    AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
    if (!rec) return status::kInvalidHandle;
    const Allocation& a = *rec->allocation;
    const AllocationDesc& d = a.desc();
    if (!(d.flags & kAllocShared)) return status::kAccessDenied;
    if (image_only && (!a.is_image() || a.is_depth() || a.is_staging() || d.samples != 1 || d.mip_levels != 1 || d.kind != graphics::TextureKind::Texture2D || d.layers > 1 ||
                       d.heap != 0 || !exportable_format(d.format)))
      return status::kNotSupported;
    Adapter* adapter = adapter_of_locked(a);
    if (!adapter) return status::kInvalidDeviceState;
    ExportInfo info;
    info.desc = d;
    info.pitch = a.pitch();
    info.bytes = a.backing().size();
    info.adapter = adapter->index();
    info.image = image_only;
    made = SharedPointer<ExportedAllocation>::make(allocator_, *this, rec->allocation, *adapter, info);
    if (!made) return status::kNoMemory;
  }
  *out = util::move(made);      // (with the lock let go: what *out held before may have been the last use of another export)
  return status::kSuccess;
}

NtStatus Kernel::import_allocation(ProcessId pid, Handle device, const ExportedAllocation& exported, Handle* allocation, AllocationDesc* desc) {
  if (&exported.kernel_ != this) return status::kInvalidParameter;
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  DeviceObject* d = p ? p->devices.raw(device) : nullptr;
  if (!d) return status::kInvalidHandle;
  if (&exported.allocation_->manager() != &d->adapter().vidmm()) return status::kInvalidParameter;   // (another adapter's memory)
  SharedPointer<AllocRec> rec = SharedPointer<AllocRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->allocation = exported.allocation_;
  rec->device = device;
  *allocation = p->allocations.insert(rec);
  if (desc) *desc = exported.info_.desc;
  return *allocation ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::read_export(ExportedAllocation& exported, LockResult* out) {
  if (&exported.kernel_ != this) return status::kInvalidParameter;
  if (!exported.info_.image) return status::kNotSupported;
  kernel::ScopedMutex g(lock_);
  Allocation& a = *exported.allocation_;
  exported.adapter_.pump();
  if (exported.adapter_.allocation_in_use(a)) return status::kGraphicsAllocationBusy;
  if (!a.manager().read_back(a)) return status::kInvalidDeviceState;
  uint64_t offset = 0, bytes = 0;
  ByteView data;
  if (!a.level_range(0, &offset, &bytes) || !a.writable_backing().try_subview(offset, bytes, &data)) return status::kInvalidDeviceState;
  a.manager().lock_taken(a);
  ++exported.reads_;
  out->data = data;
  out->pitch = exported.info_.pitch;
  return status::kSuccess;
}

NtStatus Kernel::end_read_export(ExportedAllocation& exported) {
  if (&exported.kernel_ != this) return status::kInvalidParameter;
  kernel::ScopedMutex g(lock_);
  if (!exported.reads_) return status::kInvalidDeviceState;
  exported.allocation_->manager().lock_released(*exported.allocation_);
  --exported.reads_;
  return status::kSuccess;
}

NtStatus Kernel::acquire_export_image(ExportedAllocation& exported, AcquiredImage* out) {
  if (&exported.kernel_ != this || !out) return status::kInvalidParameter;
  if (!exported.info_.image) return status::kNotSupported;
  kernel::ScopedMutex g(lock_);
  Allocation& a = *exported.allocation_;
  if (!(a.desc().flags & kAllocTexture) || !(a.desc().flags & kAllocRenderTarget)) return status::kNotSupported;
  if (a.lock_count() != exported.reads_) return status::kInvalidDeviceState;      // (the CPU of a process has it locked: its picture may be half written)
  exported.adapter_.pump();
  if (exported.adapter_.allocation_in_use(a)) return status::kGraphicsAllocationBusy;
  const NtStatus s = a.manager().make_resident(a);
  if (s != status::kSuccess) return s;
  if (!a.image()) return status::kInvalidDeviceState;
  a.manager().pin(a);
  ++exported.acquires_;
  out->device = &exported.adapter_.device();
  out->image = a.image();
  return status::kSuccess;
}

NtStatus Kernel::release_export_image(ExportedAllocation& exported) {
  if (&exported.kernel_ != this) return status::kInvalidParameter;
  kernel::ScopedMutex g(lock_);
  if (!exported.acquires_) return status::kInvalidDeviceState;
  exported.allocation_->manager().unpin(*exported.allocation_);
  --exported.acquires_;
  return status::kSuccess;
}

NtStatus Kernel::lock(ProcessId pid, Handle allocation, uint32_t flags, uint32_t subresource, LockResult* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  AllocRec* rec = p ? p->allocations.raw(allocation) : nullptr;
  if (!rec) return status::kInvalidHandle;
  Allocation& a = *rec->allocation;
  if (flags & ~(kLockReadOnly | kLockDoNotWait | kLockDiscard)) return status::kInvalidParameter;
  if (rec->locked) return status::kInvalidDeviceState;
  if (a.is_depth() || a.desc().samples != 1) return status::kNotSupported;
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
  out->pitch = a.is_image() ? static_cast<uint32_t>(image_row_bytes(a.desc().format, graphics::mip_extent(a.desc().width, subresource))) : 0;   // (a compressed one's: a row of blocks)
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
  DeviceLookup lookup(p->pipelines, p->signatures, p->heaps, p->video_decoders, c->device);
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
  SharedPointer<ShaderRec> gs = desc.geometry_shader ? p.shaders.get(desc.geometry_shader) : SharedPointer<ShaderRec>();
  if (desc.geometry_shader && (!gs || gs->device != device)) return status::kInvalidHandle;
  SharedPointer<ShaderRec> hs = desc.hull_shader ? p.shaders.get(desc.hull_shader) : SharedPointer<ShaderRec>(), ds = desc.domain_shader ? p.shaders.get(desc.domain_shader) : SharedPointer<ShaderRec>();
  if ((desc.hull_shader && (!hs || hs->device != device)) || (desc.domain_shader && (!ds || ds->device != device))) return status::kInvalidHandle;
  SharedPointer<PipelineObject> pipe;
  NtStatus st = d->adapter().create_pipeline(desc, vs->shader, fs->shader, &pipe, gs ? gs->shader : SharedPointer<ShaderObject>(), hs ? hs->shader : SharedPointer<ShaderObject>(),
                                             ds ? ds->shader : SharedPointer<ShaderObject>());
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

// --- Direct3D 12 binding objects ------------------------------------------------------------------------------------------------------------------------------------

NtStatus Kernel::create_root_signature(ProcessId pid, Handle device, ConstByteView blob, Handle* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p || !p->devices.raw(device)) return status::kInvalidHandle;
  SharedPointer<SignatureRec> rec = SharedPointer<SignatureRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->signature = SharedPointer<RootSignatureObject>::make(allocator_);
  if (!rec->signature) return status::kNoMemory;
  const NtStatus st = parse_root_signature(blob, &rec->signature->desc);
  if (st != status::kSuccess) return st;
  rec->device = device;
  *out = p->signatures.insert(rec);
  return *out ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::destroy_root_signature(ProcessId pid, Handle signature) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->signatures.remove(signature) ? status::kSuccess : status::kInvalidHandle;
}

NtStatus Kernel::create_descriptor_heap(ProcessId pid, Handle device, DescriptorHeapType type, uint32_t count, Handle* out) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p || !p->devices.raw(device)) return status::kInvalidHandle;
  if (count == 0 || count > kMaxHeapDescriptors) return status::kInvalidParameter;
  SharedPointer<HeapRec> rec = SharedPointer<HeapRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->heap = SharedPointer<DescriptorHeapObject>::make(allocator_, allocator_, type);
  if (!rec->heap || !rec->heap->resize(count)) return status::kNoMemory;
  rec->device = device;
  *out = p->heaps.insert(rec);
  return *out ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::destroy_descriptor_heap(ProcessId pid, Handle heap) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->heaps.remove(heap) ? status::kSuccess : status::kInvalidHandle;
}

NtStatus Kernel::write_descriptor(ProcessId pid, Handle heap, uint32_t index, const DescriptorDesc& desc) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<HeapRec> h = p ? p->heaps.get(heap) : SharedPointer<HeapRec>();
  if (!h) return status::kInvalidHandle;
  if (index >= h->heap->count()) return status::kInvalidParameter;
  Descriptor d;
  d.kind = desc.kind;
  d.sampler = desc.sampler;
  if (desc.kind == DescriptorKind::None) {
    h->heap->at(index) = d;
    return status::kSuccess;
  }
  if ((desc.kind == DescriptorKind::Sampler) != (h->heap->type() == DescriptorHeapType::Sampler)) return status::kInvalidParameter;
  if (desc.kind != DescriptorKind::Sampler) {
    SharedPointer<AllocRec> a = p->allocations.get(desc.allocation);
    if (!a || a->device != h->device) return status::kInvalidHandle;
    const Allocation& alloc = *a->allocation;
    const bool buffer_view = desc.kind == DescriptorKind::Srv && alloc.is_buffer();
    bool stencil = desc.stencil;
    if (desc.kind == DescriptorKind::UavTexture) {
      // (a level of the image and layers it has; the image a storage image)
      const graphics::StorageImageView& v = desc.image;
      graphics::Format view = graphics::Format::Undefined;
      if (!alloc.is_image() || !(alloc.desc().flags & kAllocStorageImage) || v.level >= alloc.mip_levels() || v.layer_count == 0 || v.first_layer >= alloc.level_layers(v.level) ||
          v.layer_count > alloc.level_layers(v.level) - v.first_layer || !is_known_format(desc.view_format) ||
          !storage_view_format(alloc.desc().format, static_cast<Format>(desc.view_format), &view))
        return status::kInvalidParameter;
      d.allocation = a->allocation;
      d.image = v;
      d.image.format = view;
      d.view_format = desc.view_format;
      h->heap->at(index) = d;
      return status::kSuccess;
    }
    if (desc.kind == DescriptorKind::Srv ? (buffer_view ? !(alloc.desc().flags & kAllocUnorderedAccess) : (!alloc.is_image() || !(alloc.desc().flags & kAllocTexture)))
        : desc.kind == DescriptorKind::Uav ? !(alloc.desc().flags & kAllocUnorderedAccess)
                                           : !alloc.is_buffer() || alloc.is_query_result())
      return status::kInvalidParameter;
    if (desc.kind != DescriptorKind::Srv || buffer_view) {
      // (a typed view: whole elements of its format - an unordered access view's a format that can be stored -; the others whole words)
      const graphics::TexelFormat texel = static_cast<graphics::TexelFormat>(desc.texel_format);
      const bool typed = desc.texel_format != 0 && desc.kind != DescriptorKind::Cbv;
      if (typed && (!graphics::texel_format_valid(texel) || (desc.kind == DescriptorKind::Uav && !graphics::texel_format_storable(texel)))) return status::kInvalidParameter;
      const uint64_t unit = typed ? graphics::texel_bytes(texel) : 4;
      if (desc.offset > alloc.desc().size || desc.offset % unit || desc.size > alloc.desc().size - desc.offset || desc.size % unit) return status::kInvalidParameter;
      d.offset = desc.offset;
      d.size = desc.size ? desc.size : (alloc.desc().size - desc.offset) / unit * unit;
      if (typed) d.texel_format = desc.texel_format;
      if (desc.counter) {
        // (an unordered access view's counter: a word of a buffer of the same device that shaders write)
        SharedPointer<AllocRec> c = p->allocations.get(desc.counter);
        if (!c || c->device != h->device) return status::kInvalidHandle;
        const Allocation& ca = *c->allocation;
        if (desc.kind != DescriptorKind::Uav || !ca.is_buffer() || !(ca.desc().flags & kAllocUnorderedAccess) || desc.counter_offset % 4 || desc.counter_offset > ca.desc().size ||
            ca.desc().size - desc.counter_offset < 4)
          return status::kInvalidParameter;
        d.counter = c->allocation;
        d.counter_offset = desc.counter_offset;
      }
    } else {
      // (a texture's view: its levels and layers from the offset words, each a level or layer the image has; a volume's all its slices)
      graphics::TextureView v;
      v.base_level = static_cast<uint32_t>(desc.offset & 0xFFFF), v.level_count = static_cast<uint32_t>(desc.offset >> 16 & 0xFFFF);
      v.first_layer = static_cast<uint32_t>(desc.offset >> 32 & 0xFFFF), v.layer_count = static_cast<uint32_t>(desc.offset >> 48);
      if (v.level_count == 0 && v.base_level < alloc.mip_levels()) v.level_count = alloc.mip_levels() - v.base_level;
      if (v.layer_count == 0 && v.first_layer < alloc.layers()) v.layer_count = alloc.layers() - v.first_layer;
      if (v.base_level >= alloc.mip_levels() || v.level_count > alloc.mip_levels() - v.base_level || v.first_layer >= alloc.layers() || v.layer_count > alloc.layers() - v.first_layer ||
          (alloc.texture_kind() == graphics::TextureKind::Texture3D && v.layer_count != alloc.layers()))
        return status::kInvalidParameter;
      v.kind = alloc.texture_kind();
      d.view = v;
      // (the texture view's plane of a depth buffer - the escape's texel-format word, which a texture's view has no other use for: 1 the stencil plane; DescriptorDesc::stencil)
      if (desc.texel_format > 1) return status::kInvalidParameter;
      if (desc.texel_format == 1) stencil = true;
      // (the most detailed level the view's lookups take - Direct3D's ResourceMinLODClamp: 0 or more, of the image's levels, a level past the view's last reading its last)
      if (!(desc.min_lod_clamp >= 0.0f && desc.min_lod_clamp <= 1000.0f)) return status::kInvalidParameter;
      d.min_lod_clamp = desc.min_lod_clamp;
    }
    d.allocation = a->allocation;
    if (stencil) {
      // (a view of the stencil plane: of a depth buffer read as a texture that has one)
      if (desc.kind != DescriptorKind::Srv || !alloc.is_depth() || alloc.desc().format != Format::D24S8) return status::kInvalidParameter;
      d.stencil = true;
    }
    if (desc.alternate) {
      // (an image's view only: the alternate is a 2D array of the same device, sampled)
      SharedPointer<AllocRec> alt = p->allocations.get(desc.alternate);
      if (!alt || alt->device != h->device) return status::kInvalidHandle;
      if (desc.kind != DescriptorKind::Srv || !alloc.is_image() || !alt->allocation->is_image() || !(alt->allocation->desc().flags & kAllocTexture) ||
          alt->allocation->texture_kind() != graphics::TextureKind::Texture2DArray)
        return status::kInvalidParameter;
      d.alternate = alt->allocation;
    }
  }
  h->heap->at(index) = d;
  return status::kSuccess;
}

NtStatus Kernel::copy_descriptors(ProcessId pid, Handle destination, uint32_t destination_index, Handle source, uint32_t source_index, uint32_t count) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<HeapRec> to = p ? p->heaps.get(destination) : SharedPointer<HeapRec>(), from = p ? p->heaps.get(source) : SharedPointer<HeapRec>();
  if (!to || !from) return status::kInvalidHandle;
  if (to->heap->type() != from->heap->type() || count == 0 || destination_index > to->heap->count() || count > to->heap->count() - destination_index || source_index > from->heap->count() ||
      count > from->heap->count() - source_index)
    return status::kInvalidParameter;
  // (an overlap within one heap: copied the way memmove would)
  if (destination_index <= source_index) {
    for (uint32_t i = 0; i < count; ++i) to->heap->at(destination_index + i) = from->heap->at(source_index + i);
  } else {
    for (uint32_t i = count; i-- > 0;) to->heap->at(destination_index + i) = from->heap->at(source_index + i);
  }
  return status::kSuccess;
}

NtStatus Kernel::update_tile_mappings(ProcessId pid, Handle reserved, uint32_t first_tile, Handle heap, uint64_t heap_offset, uint32_t tile_count) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<AllocRec> r = p ? p->allocations.get(reserved) : SharedPointer<AllocRec>();
  if (!r) return status::kInvalidHandle;
  SharedPointer<Allocation> heap_allocation;
  if (heap) {
    SharedPointer<AllocRec> h = p->allocations.get(heap);
    if (!h || h->device != r->device) return status::kInvalidHandle;
    heap_allocation = h->allocation;
  }
  return r->allocation->manager().map_tiles(*r->allocation, first_tile, heap_allocation, heap_offset, tile_count);
}

NtStatus Kernel::set_mirror(ProcessId pid, Handle mirror, uint32_t mode, uint32_t channels, ArrayView<const Handle> sources, ArrayView<const uint32_t> subresources) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  SharedPointer<AllocRec> m = p ? p->allocations.get(mirror) : SharedPointer<AllocRec>();
  if (!m) return status::kInvalidHandle;
  if (sources.size() > graphics::kMaxTextureLayers) return status::kInvalidParameter;
  List<SharedPointer<Allocation>> found(allocator_);
  for (const Handle h : sources) {
    SharedPointer<AllocRec> s = p->allocations.get(h);
    if (!s || s->device != m->device) return status::kInvalidHandle;
    if (!found.push_back(s->allocation)) return status::kNoMemory;
  }
  return m->allocation->manager().set_mirror(*m->allocation, ArrayView<const SharedPointer<Allocation>>(found.view().data(), found.count()), mode, channels, subresources);
}

// --- Video decoders -----------------------------------------------------------------------------------------------------------------------------------------------------

bool Kernel::video_decode_supported(uint32_t profile, uint32_t surface_format, uint32_t width, uint32_t height) const {
  // (the planes are 2D images: no bigger than the kernel's textures)
  if (profile >= hardware::kDxvaProfileCount || surface_format >= hardware::kDxvaSurfaceFormatCount || width > kMinTextureDimension || height > kMinTextureDimension) return false;
  return hardware::dxva_profile_supports(static_cast<hardware::DxvaProfile>(profile), static_cast<hardware::DxvaSurfaceFormat>(surface_format), width, height);
}

NtStatus Kernel::create_video_decoder(ProcessId pid, Handle device, const VideoDecoderDesc& desc, Handle* out) {
  *out = 0;
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  if (!p || !p->devices.raw(device)) return status::kInvalidHandle;
  if (desc.surfaces == 0 || desc.surfaces > kMaxVideoDecodeSurfaces || desc.surface_format >= hardware::kDxvaSurfaceFormatCount) return status::kInvalidParameter;
  if (!video_decode_supported(desc.profile, desc.surface_format, desc.width, desc.height)) return status::kNotSupported;
  hardware::DxvaDecoderConfig config;
  config.profile = static_cast<hardware::DxvaProfile>(desc.profile);
  config.width = desc.width;
  config.height = desc.height;
  config.surfaces = desc.surfaces;
  config.surface = static_cast<hardware::DxvaSurfaceFormat>(desc.surface_format);
  UniquePointer<hardware::IDxvaDecoder> decoder = hardware::create_dxva_decoder(allocator_, config);
  if (!decoder) return status::kNoMemory;
  SharedPointer<VideoDecoderRec> rec = SharedPointer<VideoDecoderRec>::make(allocator_);
  if (!rec) return status::kNoMemory;
  rec->decoder = SharedPointer<VideoDecoderObject>::make(allocator_, util::move(decoder), desc);
  if (!rec->decoder) return status::kNoMemory;
  rec->device = device;
  *out = p->video_decoders.insert(rec);
  return *out ? status::kSuccess : status::kInsufficientResources;
}

NtStatus Kernel::destroy_video_decoder(ProcessId pid, Handle decoder) {
  kernel::ScopedMutex g(lock_);
  Process* p = find_process(pid);
  return p && p->video_decoders.remove(decoder) ? status::kSuccess : status::kInvalidHandle;
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

NtStatus Kernel::escape_d3d12(ProcessId pid, Handle device, uint32_t op, ByteReader& in, ByteWriter& out, ByteView reply) {
  switch (static_cast<EscapeOp>(op)) {
    case EscapeOp::CreateRootSignature: {
      const uint32_t bytes = in.le32();
      ConstByteView blob = in.take(bytes);
      if (!in.ok()) return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      Handle h = 0;
      const NtStatus st = create_root_signature(pid, device, blob, &h);
      if (st == status::kSuccess) out.le32(h);
      return st;
    }
    case EscapeOp::CreateVideoDecoder: {
      VideoDecoderDesc desc;
      desc.profile = in.le32();
      desc.width = in.le32();
      desc.height = in.le32();
      desc.surfaces = in.le32();
      desc.surface_format = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      Handle h = 0;
      const NtStatus st = create_video_decoder(pid, device, desc, &h);
      if (st == status::kSuccess) out.le32(h);
      return st;
    }
    case EscapeOp::DestroyVideoDecoder: {
      const Handle h = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      return destroy_video_decoder(pid, h);
    }
    case EscapeOp::QueryVideoDecodeSupport: {
      const uint32_t profile = in.le32(), surface_format = in.le32(), width = in.le32(), height = in.le32();
      if (!in.ok() || !in.at_end() || surface_format >= hardware::kDxvaSurfaceFormatCount) return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      out.le32(video_decode_supported(profile, surface_format, width, height) ? 1u : 0u);
      return status::kSuccess;
    }
    case EscapeOp::DestroyRootSignature:
    case EscapeOp::DestroyDescriptorHeap: {
      const Handle h = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      return static_cast<EscapeOp>(op) == EscapeOp::DestroyRootSignature ? destroy_root_signature(pid, h) : destroy_descriptor_heap(pid, h);
    }
    case EscapeOp::CreateDescriptorHeap: {
      const uint32_t type = in.le32(), count = in.le32();
      if (!in.ok() || !in.at_end() || type > 1) return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      Handle h = 0;
      const NtStatus st = create_descriptor_heap(pid, device, static_cast<DescriptorHeapType>(type), count, &h);
      if (st == status::kSuccess) out.le32(h);
      return st;
    }
    case EscapeOp::WriteDescriptors: {
      const Handle heap = in.le32();
      const uint32_t count = in.le32();
      if (!in.ok() || count > 4096) return status::kInvalidParameter;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = in.le32(), kind_word = in.le32(), allocation = in.le32(), off_lo = in.le32(), off_hi = in.le32(), size_lo = in.le32(), size_hi = in.le32();
        const uint32_t filter = in.le32(), au = in.le32(), av = in.le32(), mip = in.le32(), bias = in.le32(), min_lod = in.le32(), max_lod = in.le32();
        DescriptorDesc d;
        // (kDescriptorSamplerExtras in the kind word: six more words follow, a sampler's anisotropy, whether its border colour is its own, and the colour's r, g, b, a)
        const uint32_t kind = kind_word & ~kDescriptorSamplerExtras;
        if (kind_word & kDescriptorSamplerExtras) {
          const uint32_t anisotropy = in.le32(), custom = in.le32();
          for (float& c : d.sampler.border_rgba) c = bits_float(in.le32());
          if (!in.ok() || anisotropy < 1 || anisotropy > graphics::kMaxAnisotropy || custom > 1) return status::kInvalidParameter;
          d.sampler.max_anisotropy = static_cast<uint8_t>(anisotropy);
          d.sampler.custom_border = custom != 0;
        }
        graphics::BorderColor no_border;
        if (!in.ok() || kind > static_cast<uint32_t>(DescriptorKind::UavTexture) || !decode_filter_word(filter, &d.sampler) || !decode_address_word(au, &d.sampler.address_u, &d.sampler.border) ||
            !decode_address_word(av, &d.sampler.address_v, &no_border) || mip > 2)
          return status::kInvalidParameter;
        d.kind = static_cast<DescriptorKind>(kind);
        d.allocation = allocation;
        d.offset = off_lo | (uint64_t{off_hi} << 32);
        d.size = size_lo | (uint64_t{size_hi} << 32);
        d.sampler.mip_filter = static_cast<graphics::MipFilter>(mip);
        d.sampler.lod_bias = bits_float(bias);
        d.sampler.min_lod = bits_float(min_lod);
        d.sampler.max_lod = bits_float(max_lod);
        // (a texture's view has no range: its size word names the array image a shader that declares one reads instead - Descriptor::alternate)
        if (d.kind == DescriptorKind::Srv && size_hi == 0xFFFFFFFFu) {
          d.alternate = size_lo;
          d.size = 0;
        }
        // (a sampler: the offset's low word is its comparison, a graphics::CompareOp - SampleCmp, GatherCmp -; nothing for the others, whose offsets are buffers' ranges - or a
        // texture's view: its levels and layers, DescriptorDesc::view)
        if (d.kind == DescriptorKind::Sampler) {
          if (off_lo > static_cast<uint32_t>(graphics::CompareOp::Always) || off_hi) return status::kInvalidParameter;
          d.sampler.compare = static_cast<graphics::CompareOp>(off_lo);
          d.offset = 0;
        }
        // (a view of a buffer has no sampler: those words are the view's element format - typed views - and an unordered access view's counter, its buffer and offset; a texture's
        // view has no sampler either: its element-format word is the plane of a depth buffer it reads, 1 the stencil - write_descriptor)
        if (d.kind == DescriptorKind::Srv || d.kind == DescriptorKind::Uav) {
          d.texel_format = bias;
          d.counter = min_lod;
          d.counter_offset = max_lod;
          if (d.kind == DescriptorKind::Srv) d.min_lod_clamp = bits_float(min_lod);   // (a texture's view: its min_lod word is the clamp; a buffer's is 0)
          d.sampler.lod_bias = d.sampler.min_lod = 0.0f;
          d.sampler.max_lod = 1000.0f;
        }
        // (a texture's unordered access view: the level, the first layer and the layer count)
        if (d.kind == DescriptorKind::UavTexture) {
          d.image.level = off_lo, d.image.first_layer = off_hi, d.image.layer_count = size_lo;
          d.view_format = size_hi;   // (the view's format: 0, the allocation's own)
          d.offset = d.size = 0;
        }
        const NtStatus st = write_descriptor(pid, heap, index, d);
        if (st != status::kSuccess) return st;
      }
      return in.at_end() ? status::kSuccess : status::kInvalidParameter;
    }
    case EscapeOp::CopyDescriptors: {
      const Handle dst = in.le32();
      const uint32_t dst_index = in.le32();
      const Handle src = in.le32();
      const uint32_t src_index = in.le32(), count = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      return copy_descriptors(pid, dst, dst_index, src, src_index, count);
    }
    case EscapeOp::UpdateTileMappings: {
      const Handle reserved = in.le32();
      const uint32_t first = in.le32();
      const Handle heap = in.le32();
      const uint32_t lo = in.le32(), hi = in.le32(), count = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      return update_tile_mappings(pid, reserved, first, heap, lo | (uint64_t{hi} << 32), count);
    }
    case EscapeOp::SetMirror: {
      const Handle mirror = in.le32();
      const uint32_t mode = in.le32(), channels = in.le32(), count = in.le32();
      if (!in.ok() || count > graphics::kMaxTextureLayers) return status::kInvalidParameter;
      List<Handle> sources(allocator_);
      List<uint32_t> subresources(allocator_);
      for (uint32_t i = 0; i < count; ++i)
        if (!sources.push_back(in.le32())) return status::kNoMemory;
      // (optionally the subresource of each source)
      if (in.ok() && !in.at_end())
        for (uint32_t i = 0; i < count; ++i)
          if (!subresources.push_back(in.le32())) return status::kNoMemory;
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      return set_mirror(pid, mirror, mode, channels, ArrayView<const Handle>(sources.view().data(), sources.count()),
                        ArrayView<const uint32_t>(subresources.view().data(), subresources.count()));
    }
    case EscapeOp::QueryStreamOutput: {
      const Handle allocation = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      if (reply.size() < 8) return status::kBufferTooSmall;
      uint64_t offset = 0;
      const NtStatus st = query_stream_output(pid, allocation, &offset);
      if (st == status::kSuccess) {
        out.le32(static_cast<uint32_t>(offset));
        out.le32(static_cast<uint32_t>(offset >> 32));
      }
      return st;
    }
    case EscapeOp::QuerySync: {
      const Handle sync = in.le32();
      if (!in.ok() || !in.at_end()) return status::kInvalidParameter;
      if (reply.size() < 8) return status::kBufferTooSmall;
      uint64_t value = 0;
      const NtStatus st = query_sync(pid, sync, &value);
      if (st == status::kSuccess) {
        out.le32(static_cast<uint32_t>(value));
        out.le32(static_cast<uint32_t>(value >> 32));
      }
      return st;
    }
    default: return status::kNotSupported;
  }
}

NtStatus Kernel::escape(ProcessId pid, Handle device, ConstByteView request, ByteView reply, uint32_t* reply_bytes) {
  {
    // (the Direct3D 12 operations call the public methods, which take the lock themselves)
    ByteReader peek(request);
    const uint32_t first = peek.le32();
    if (peek.ok() && first >= static_cast<uint32_t>(EscapeOp::CreateRootSignature)) {
      *reply_bytes = 0;
      ByteWriter w(reply);
      const NtStatus st = escape_d3d12(pid, device, first, peek, w, reply);
      if (st != status::kSuccess) return st;
      if (!w.ok()) return status::kBufferTooSmall;
      *reply_bytes = static_cast<uint32_t>(w.position());
      return status::kSuccess;
    }
  }
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
      // (the kind is the low byte; above it, for Dxbc, the register its constant buffers begin at: ShaderDesc::constant_base)
      const uint32_t kind_word = in.le32(), stage = in.le32(), bytes = in.le32();
      const uint32_t kind = kind_word & 0xFF;
      if (!in.ok() || kind > static_cast<uint32_t>(ShaderKind::Dxbc) || stage > 5) return status::kInvalidParameter;
      if ((kind_word >> 8) != 0 && kind != static_cast<uint32_t>(ShaderKind::Dxbc)) return status::kInvalidParameter;
      desc.kind = static_cast<ShaderKind>(kind);
      desc.constant_base = kind_word >> 8;
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
      for (uint32_t ai = 0; ai < 4; ++ai) {      // (the first four attributes are in the base layout; more come in the last, optional part)
        graphics::VertexAttribute& a = desc.attributes[ai];
        a.location = in.le32();
        const uint32_t format = in.le32();
        a.offset = in.le32();
        if (format >= graphics::kVertexFormatCount) return status::kInvalidParameter;
        a.format = static_cast<graphics::VertexFormat>(format);
      }
      const uint32_t enable = in.le32(), compare = in.le32(), fail = in.le32(), depth_fail = in.le32(), pass = in.le32();
      const uint32_t compare_mask = in.le32(), write_mask = in.le32(), reference = in.le32();
      constexpr uint32_t kLastFactor = graphics::kBlendFactorCount - 1;
      // Optional trailing words (the state of Direct3D 10): depth_compare, then blend custom, enable, the four factors, the two ops, the write mask, and a bit per attribute for
      // per_instance (then the number of colour outputs, then independent blending). Their absence is the base state.
      if (!in.ok()) return status::kInvalidParameter;
      if (!in.at_end()) {
        const uint32_t depth_compare = in.le32(), custom = in.le32(), blend_enable = in.le32(), sc = in.le32(), dc = in.le32(), sa = in.le32(), da = in.le32(), cop = in.le32(), aop = in.le32(),
                       mask = in.le32(), instance_bits = in.le32();
        if (!in.ok() || depth_compare > 7 || custom > 1 || blend_enable > 1 || sc > kLastFactor || dc > kLastFactor || sa > kLastFactor || da > kLastFactor || cop > 4 || aop > 4 || mask > 15 || instance_bits > 15) return status::kInvalidParameter;
        desc.depth_compare = static_cast<graphics::CompareOp>(depth_compare);
        desc.blend.custom = custom != 0, desc.blend.enable = blend_enable != 0;
        desc.blend.src_color = static_cast<graphics::BlendFactor>(sc), desc.blend.dst_color = static_cast<graphics::BlendFactor>(dc);
        desc.blend.src_alpha = static_cast<graphics::BlendFactor>(sa), desc.blend.dst_alpha = static_cast<graphics::BlendFactor>(da);
        desc.blend.color_op = static_cast<graphics::BlendOp>(cop), desc.blend.alpha_op = static_cast<graphics::BlendOp>(aop);
        desc.blend.write_mask = static_cast<uint8_t>(mask);
        for (uint32_t i = 0; i < 4; ++i) desc.attributes[i].per_instance = (instance_bits >> i) & 1;
        if (!in.at_end()) {   // (and, after that, the number of colour outputs)
          desc.color_target_count = in.le32();
          if (!in.ok() || desc.color_target_count == 0 || desc.color_target_count > graphics::kMaxColorTargets) return status::kInvalidParameter;
          if (!in.at_end()) {   // (and, after that, independent blending: a flag, then for targets 1 .. 3 the nine words of the blend state)
            const uint32_t independent = in.le32();
            if (!in.ok() || independent > 1) return status::kInvalidParameter;
            desc.independent_blend = independent != 0;
            for (uint32_t ti = 0; ti < 3; ++ti) {
              graphics::BlendState& t = desc.target_blend[ti];
              const uint32_t tc = in.le32(), te = in.le32(), tsc = in.le32(), tdc = in.le32(), tsa = in.le32(), tda = in.le32(), tcop = in.le32(), taop = in.le32(), tmask = in.le32();
              if (!in.ok() || tc > 1 || te > 1 || tsc > kLastFactor || tdc > kLastFactor || tsa > kLastFactor || tda > kLastFactor || tcop > 4 || taop > 4 || tmask > 15) return status::kInvalidParameter;
              t.custom = tc != 0, t.enable = te != 0;
              t.src_color = static_cast<graphics::BlendFactor>(tsc), t.dst_color = static_cast<graphics::BlendFactor>(tdc);
              t.src_alpha = static_cast<graphics::BlendFactor>(tsa), t.dst_alpha = static_cast<graphics::BlendFactor>(tda);
              t.color_op = static_cast<graphics::BlendOp>(tcop), t.alpha_op = static_cast<graphics::BlendOp>(taop);
              t.write_mask = static_cast<uint8_t>(tmask);
            }
            if (!in.at_end()) {   // (and, after that, whether coverage is per sample)
              const uint32_t multisample = in.le32();
              if (!in.ok() || multisample > 3) return status::kInvalidParameter;   // (bit 0: coverage per sample, bit 1: alpha to coverage)
              desc.multisample = (multisample & 1) != 0;
              desc.alpha_to_coverage = (multisample & 2) != 0;
              if (!in.at_end()) {   // (and, after that, a geometry shader, then a hull and a domain shader)
                desc.geometry_shader = in.le32();
                if (!in.ok()) return status::kInvalidParameter;
                if (!in.at_end()) {
                  desc.hull_shader = in.le32();
                  desc.domain_shader = in.le32();
                  if (!in.ok()) return status::kInvalidParameter;
                  if (!in.at_end()) {   // (and, after that, stream output: the entry count, and if there are entries the rasterized stream (-1: none), the strides, then the entries)
                    const uint32_t entries = in.le32();
                    if (!in.ok() || entries > graphics::kMaxStreamOutputEntries) return status::kInvalidParameter;
                    if (entries) {
                      desc.rasterized_stream = static_cast<int32_t>(in.le32());
                      for (uint32_t& stride : desc.stream_output_stride) stride = in.le32();
                      for (uint32_t e = 0; e < entries; ++e) {
                        const uint32_t packed = in.le32(), buffer = in.le32();
                        if (!in.ok() || buffer >= graphics::kMaxStreamOutputBuffers) return status::kInvalidParameter;
                        graphics::StreamOutputEntry& entry = desc.stream_output[e];
                        entry.stream = static_cast<uint8_t>(packed), entry.output = static_cast<uint8_t>(packed >> 8), entry.start_component = static_cast<uint8_t>(packed >> 16);
                        entry.component_count = static_cast<uint8_t>(packed >> 24), entry.buffer = static_cast<uint8_t>(buffer);
                      }
                    }
                    if (!in.ok()) return status::kInvalidParameter;
                    desc.stream_output_count = entries;
                    if (!in.at_end()) {   // (and, after that, the vertex layout beyond four attributes and one vertex buffer, and the blend state of targets 4 .. 7)
                      const uint32_t extra = in.le32();
                      if (!in.ok() || extra > graphics::kMaxVertexAttributes - 4) return status::kInvalidParameter;
                      for (uint32_t k = 0; k < extra; ++k) {
                        graphics::VertexAttribute& a = desc.attributes[4 + k];
                        a.location = in.le32();
                        const uint32_t packed = in.le32();      // format | slot << 8
                        a.offset = in.le32();
                        if ((packed & 0xFF) >= graphics::kVertexFormatCount || (packed >> 8) >= graphics::kMaxVertexBuffers) return status::kInvalidParameter;
                        a.format = static_cast<graphics::VertexFormat>(packed & 0xFF);
                        a.slot = static_cast<uint8_t>(packed >> 8);
                      }
                      const uint32_t instance_high = in.le32(), slots_low = in.le32(), strides = in.le32();      // (per_instance of the extra attributes, the slots of the first four: 4 bits each)
                      if (!in.ok() || strides >= graphics::kMaxVertexBuffers) return status::kInvalidParameter;
                      for (uint32_t k = 0; k < extra; ++k) desc.attributes[4 + k].per_instance = (instance_high >> k) & 1;
                      for (uint32_t k = 0; k < 4; ++k) {
                        desc.attributes[k].slot = static_cast<uint8_t>((slots_low >> (4 * k)) & 0xF);
                        if (desc.attributes[k].slot >= graphics::kMaxVertexBuffers) return status::kInvalidParameter;
                      }
                      for (uint32_t k = 1; k <= strides; ++k) desc.slot_strides[k] = in.le32();
                      const uint32_t more_blends = in.le32();
                      if (!in.ok() || more_blends > graphics::kMaxColorTargets - 4) return status::kInvalidParameter;
                      for (uint32_t k = 0; k < more_blends; ++k) {
                        graphics::BlendState& t = desc.target_blend[3 + k];
                        const uint32_t tc = in.le32(), te = in.le32(), tsc = in.le32(), tdc = in.le32(), tsa = in.le32(), tda = in.le32(), tcop = in.le32(), taop = in.le32(), tmask = in.le32();
                        if (!in.ok() || tc > 1 || te > 1 || tsc > kLastFactor || tdc > kLastFactor || tsa > kLastFactor || tda > kLastFactor || tcop > 4 || taop > 4 || tmask > 15) return status::kInvalidParameter;
                        t.custom = tc != 0, t.enable = te != 0;
                        t.src_color = static_cast<graphics::BlendFactor>(tsc), t.dst_color = static_cast<graphics::BlendFactor>(tdc);
                        t.src_alpha = static_cast<graphics::BlendFactor>(tsa), t.dst_alpha = static_cast<graphics::BlendFactor>(tda);
                        t.color_op = static_cast<graphics::BlendOp>(tcop), t.alpha_op = static_cast<graphics::BlendOp>(taop);
                        t.write_mask = static_cast<uint8_t>(tmask);
                      }
                      if (!in.at_end()) {   // (and, after that, the colour of the constant blend factors: red, green, blue, alpha as floats; every target's)
                        float constant[4];
                        for (float& c : constant) {
                          const uint32_t bits = in.le32();
                          __builtin_memcpy(&c, &bits, 4);
                          if (c != c) return status::kInvalidParameter;      // (NaN)
                        }
                        if (!in.ok()) return status::kInvalidParameter;
                        for (uint32_t c = 0; c < 4; ++c) {
                          desc.blend.constant[c] = constant[c];
                          for (graphics::BlendState& t : desc.target_blend) t.constant[c] = constant[c];
                        }
                        // (and, after that, tagged parts in any order, each at most once: a tag, the count of the words that follow, the words)
                        bool seen_raster = false, seen_step_rates = false, seen_lines = false;
                        while (!in.at_end()) {
                          const uint32_t tag = in.le32(), words = in.le32();
                          if (!in.ok()) return status::kInvalidParameter;
                          if (tag == kPipelineLineTag) {
                            // kPipelineLineTag: how the lines are drawn (graphics::LineMode)
                            const uint32_t mode = in.le32();
                            if (seen_lines || words != kPipelineLineWords || !in.ok() || mode > static_cast<uint32_t>(graphics::LineMode::QuadrilateralNarrow)) return status::kInvalidParameter;
                            seen_lines = true;
                            desc.line_mode = static_cast<graphics::LineMode>(mode);
                            continue;
                          }
                          if (tag == kPipelineStepRateTag) {
                            // kPipelineStepRateTag: the step rates of the per-instance attributes, a word for each attribute from the first (a per-vertex one's is 0)
                            if (seen_step_rates || words > desc.attribute_count || words > graphics::kMaxVertexAttributes) return status::kInvalidParameter;
                            seen_step_rates = true;
                            for (uint32_t k = 0; k < words; ++k) {
                              const uint32_t rate = in.le32();
                              if (!in.ok() || (!desc.attributes[k].per_instance && rate != 0)) return status::kInvalidParameter;
                              if (desc.attributes[k].per_instance) desc.attributes[k].step_rate = rate;
                            }
                            continue;
                          }
                          if (tag != kPipelineRasterTag || words != kPipelineRasterWords || seen_raster) return status::kInvalidParameter;
                          seen_raster = true;
                          // kPipelineRasterTag: fill mode, depth clip, depth bias (constant, slope and clamp as float bits, the format's bits), the back faces' stencil state (separate, compare,
                          // fail, depth fail, pass, compare mask, write mask), logic operation (enable, op), the back faces' stencil reference (with separate)
                          const uint32_t fill = in.le32(), clip = in.le32(), bias = in.le32(), slope = in.le32(), clamp = in.le32(), bits = in.le32();
                          const uint32_t separate = in.le32(), bcompare = in.le32(), bfail = in.le32(), bdepth_fail = in.le32(), bpass = in.le32(), bcompare_mask = in.le32(), bwrite_mask = in.le32();
                          const uint32_t logic = in.le32(), logic_op = in.le32(), breference = in.le32();
                          if (!in.ok() || fill > 1 || clip > 1 || (bits != 0 && bits != 16 && bits != 24) || separate > 1 || bcompare > 7 || bfail > 7 || bdepth_fail > 7 || bpass > 7 || bcompare_mask > 255 ||
                              bwrite_mask > 255 || logic > 1 || logic_op >= graphics::kLogicOpCount || breference > 255)
                            return status::kInvalidParameter;
                          desc.fill_mode = static_cast<graphics::FillMode>(fill);
                          desc.depth_clip = clip != 0;
                          __builtin_memcpy(&desc.depth_bias.constant, &bias, 4);   // (a float: Direct3D 12's RASTERIZER1/2 bias, or the integer one of the others as a float)
                          __builtin_memcpy(&desc.depth_bias.slope, &slope, 4);
                          __builtin_memcpy(&desc.depth_bias.clamp, &clamp, 4);
                          desc.depth_bias.unorm_bits = static_cast<uint8_t>(bits);
                          desc.separate_back_stencil = separate != 0;
                          desc.back_stencil.compare = static_cast<graphics::CompareOp>(bcompare);
                          desc.back_stencil.fail_op = static_cast<graphics::StencilOp>(bfail);
                          desc.back_stencil.depth_fail_op = static_cast<graphics::StencilOp>(bdepth_fail);
                          desc.back_stencil.pass_op = static_cast<graphics::StencilOp>(bpass);
                          desc.back_stencil.compare_mask = static_cast<uint8_t>(bcompare_mask);
                          desc.back_stencil.write_mask = static_cast<uint8_t>(bwrite_mask);
                          desc.back_stencil.reference = static_cast<uint8_t>(breference);
                          desc.logic_op_enable = logic != 0;
                          desc.logic_op = static_cast<graphics::LogicOp>(logic_op);
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
      if (!in.ok() || !in.at_end() || (cull & ~kCullConservative) > 2 || front > 1 || compare > 7 || fail > 7 || depth_fail > 7 || pass > 7 || compare_mask > 255 || write_mask > 255 || reference > 255 || enable > 1)
        return status::kInvalidParameter;
      if (reply.size() < 4) return status::kBufferTooSmall;
      desc.cull_mode = static_cast<graphics::CullMode>(cull & ~kCullConservative);
      desc.conservative = (cull & kCullConservative) != 0;
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
