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
#include "Process/Win32k/Dxg/DxgInternal.hpp"
#include "Process/Win32k/Dxg/DxgLayouts.hpp"

// Allocations and resources: NtGdiDdDDICreateAllocation(2), DestroyAllocation(2), Lock(2), Unlock(2), QueryAllocationResidency, QueryResourceInfo, OpenResource(2).
//
// A D3DKMT allocation carries the description of what it is in the private driver data the user-mode driver and the miniport agree on; here the miniport is dxgk::Kernel, whose
// description is dxgk::AllocationDesc, so that is what the private data holds (DxgLayouts.hpp, "allocation private data"). A resource is a group of allocations made by one call
// (CreateResource) with its own private data; dxgk has no such object, so the layer keeps them, together with the data a process that opens the resource through the global
// share handle has to receive.
namespace process::win32k::dxg {

namespace {

using namespace layout;
namespace status = dxgk::status;

class Locked {
 public:
  explicit Locked(Session& s) : mutex_(s.state().lock) { mutex_.lock(); }
  ~Locked() { mutex_.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  kernel::Mutex& mutex_;
};

dxgk::Handle resource_handle(uint32_t id) { return kResourceHandleBase | id; }
bool resource_id(dxgk::Handle handle, uint32_t* id) {
  if ((handle & 0xF0000000u) != kResourceHandleBase) return false;
  *id = handle & 0x0FFFFFFFu;
  return *id != 0;
}

uint32_t word(const ByteBuffer& data, uint32_t index) {
  const ConstByteView v = data.view();
  uint32_t out = 0;
  for (uint32_t i = 0; i < 4; ++i) out |= uint32_t{v[uint64_t{index} * 4 + i]} << (8 * i);
  return out;
}

void set_word(ByteBuffer& data, uint32_t index, uint32_t value) {
  data.patch32(uint64_t{index} * 4, value);
}

// Reads the private data of one allocation and turns it into the kernel's description. kInvalidParameter for a description that is not one.
uint32_t parse_private_data(const ByteBuffer& data, Session& s, dxgk::Handle device, bool primary, bool shared, dxgk::AllocationDesc* desc) {
  if (data.size() < kAllocPrivateBytes) return kStatusInvalidParameter;
  if (word(data, kAwVersion) != kAllocPrivateVersion) return kStatusInvalidParameter;
  desc->flags = word(data, kAwFlags);
  const uint32_t format = word(data, kAwFormat);
  if (format > static_cast<uint32_t>(dxgk::Format::A2B10G10R10)) return kStatusInvalidParameter;
  desc->format = static_cast<dxgk::Format>(format);
  desc->width = word(data, kAwWidth);
  desc->height = word(data, kAwHeight);
  desc->mip_levels = word(data, kAwMips);
  desc->size = uint64_t{word(data, kAwSizeLo)} | uint64_t{word(data, kAwSizeHi)} << 32;
  const uint32_t kind = word(data, kAwKind);
  if (kind > static_cast<uint32_t>(graphics::TextureKind::Texture2DMS)) return kStatusInvalidParameter;
  desc->kind = static_cast<graphics::TextureKind>(kind);
  desc->samples = word(data, kAwSamples);
  desc->layers = word(data, kAwLayers);
  desc->initial_state = word(data, kAwInitialState);
  desc->heap = word(data, kAwHeap);
  desc->heap_offset = uint64_t{word(data, kAwHeapOffsetLo)} | uint64_t{word(data, kAwHeapOffsetHi)} << 32;
  if (desc->heap != 0) {
    AllocationRecord* heap = s.state().find_allocation(desc->heap);
    if (!heap || heap->device != device) return status::kInvalidHandle;
  }
  if (primary) desc->flags |= dxgk::kAllocPresent;
  if (shared) desc->flags |= dxgk::kAllocShared;
  return status::kSuccess;
}

// Drops a resource record, closing the global share it made. With the process lock held.
void drop_resource(Session& s, uint32_t id) {
  ProcessState& ps = s.state();
  for (uint64_t i = 0; i < ps.resources.count(); ++i)
    if (ps.resources[i]->id == id) {
      if (ps.resources[i]->share) remove_share(s.kernel(), s.pid(), ps.resources[i]->share);
      ps.resources.remove_at(i);
      return;
    }
}

// Destroys one allocation of the process (unlocking it first) and forgets it, also in its resource. With the process lock held.
uint32_t destroy_one(Session& s, Call& call, dxgk::Handle allocation) {
  ProcessState& ps = s.state();
  AllocationRecord* rec = ps.find_allocation(allocation);
  if (!rec) return status::kInvalidHandle;
  const uint32_t resource = rec->resource;
  release_lock(s, call, allocation, false);
  const uint32_t st = s.kernel().destroy_allocation(s.pid(), allocation);
  if (st != status::kSuccess) return st;
  ps.remove_allocation(allocation);
  if (resource) {
    ResourceRecord* r = ps.find_resource(resource);
    if (r) {
      for (uint64_t i = 0; i < r->allocations.count(); ++i)
        if (r->allocations[i] == allocation) {
          r->allocations.remove_at(i);
          if (i < r->allocation_data.count()) r->allocation_data.remove_at(i);
          break;
        }
      if (r->allocations.count() == 0) drop_resource(s, resource);
    }
  }
  return status::kSuccess;
}

// --- Creating ------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t create_allocation_common(Session& s, Call& call, uint64_t arg, uint64_t info_stride) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_create_allocation)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint64_t runtime_ptr = b.u64(16);
  const uint32_t runtime_size = b.u32(24);
  const uint64_t driver_ptr = b.u64(32);
  const uint32_t driver_size = b.u32(40);
  const uint32_t count = b.u32(44);
  const uint64_t infos = b.u64(48);
  const uint32_t flags = b.u32(56);
  const bool create_resource = (flags & kCaCreateResource) != 0, shared = (flags & kCaCreateShared) != 0;
  if (device == 0 || count == 0 || count > kMaxAllocationsPerCall || infos == 0) return kStatusInvalidParameter;
  if (shared && !create_resource) return kStatusInvalidParameter;
  if (flags & (kCaStandardAllocation | kCaExistingSysMem | kCaExistingSection | kCaCrossAdapter | kCaOpenCrossAdapter | kCaCreateProtected | kCaNtSecuritySharing)) return kStatusNotSupported;

  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  if (create_resource && ps.resources.count() >= kMaxResourcesPerProcess) return status::kInsufficientResources;

  UniquePointer<ResourceRecord> resource;
  if (create_resource) {
    resource = UniquePointer<ResourceRecord>::make(s.allocator(), s.allocator());
    if (!resource) return status::kNoMemory;
    if (!read_blob(call, runtime_ptr, runtime_size, &resource->runtime_data) || !read_blob(call, driver_ptr, driver_size, &resource->driver_data))
      return runtime_size > kMaxPrivateData || driver_size > kMaxPrivateData ? kStatusInvalidParameter : kStatusAccessViolation;
    resource->device = device;
  }

  // Everything made so far is undone on any failure.
  List<dxgk::Handle> created(s.allocator());
  List<UniquePointer<ByteBuffer>> datas(s.allocator());
  List<uint64_t> private_addresses(s.allocator());
  uint32_t st = status::kSuccess;
  for (uint32_t i = 0; i < count && st == status::kSuccess; ++i) {
    UserBlock e;
    if (!e.load(call, element_address(infos, i, info_stride), 40)) {
      st = kStatusAccessViolation;
      break;
    }
    const uint64_t data_ptr = e.u64(16);
    const uint32_t data_size = e.u32(24);
    UniquePointer<ByteBuffer> data = UniquePointer<ByteBuffer>::make(s.allocator(), s.allocator());
    if (!data) {
      st = status::kNoMemory;
      break;
    }
    if (data_size < kAllocPrivateBytes || data_size > kMaxPrivateData) {
      st = kStatusInvalidParameter;
      break;
    }
    if (!read_blob(call, data_ptr, data_size, data.get())) {
      st = kStatusAccessViolation;
      break;
    }
    dxgk::AllocationDesc desc;
    st = parse_private_data(*data, s, device, (e.u32(32) & 1) != 0, shared, &desc);
    if (st != status::kSuccess) break;
    dxgk::Handle handle = 0;
    st = s.kernel().create_allocation(s.pid(), device, desc, &handle);
    if (st != status::kSuccess) break;
    if (!created.push_back(handle)) {
      s.kernel().destroy_allocation(s.pid(), handle);
      st = status::kNoMemory;
      break;
    }
    AllocationRecord rec;
    rec.handle = handle;
    rec.device = device;
    if (!ps.allocations.push_back(rec)) {
      st = status::kNoMemory;
      break;
    }
    // What the miniport tells the user-mode driver back: the pitch and the size of the backing.
    dxgk::AllocationInfo info;
    if (s.kernel().query_allocation(s.pid(), handle, &info) == status::kSuccess) {
      set_word(*data, kAwPitch, info.pitch);
      set_word(*data, kAwBytesLo, static_cast<uint32_t>(info.bytes));
      set_word(*data, kAwBytesHi, static_cast<uint32_t>(info.bytes >> 32));
    }
    if (!datas.push_back(util::move(data)) || !private_addresses.push_back(data_ptr)) st = status::kNoMemory;
  }

  uint32_t share_id = 0;
  if (st == status::kSuccess && shared) {
    UniquePointer<ShareRecord> share = UniquePointer<ShareRecord>::make(s.allocator(), s.allocator());
    if (!share) st = status::kNoMemory;
    for (uint32_t i = 0; st == status::kSuccess && i < created.count(); ++i) {
      uint32_t token = 0;
      st = s.kernel().share_allocation(s.pid(), created[i], &token);
      if (st == status::kSuccess && !share->tokens.push_back(token)) {
        s.kernel().close_share(s.pid(), token);
        st = status::kNoMemory;
      }
    }
    if (st == status::kSuccess) {
      share->owner = s.pid();
      if (!share->runtime_data.append(resource->runtime_data.view()) || !share->driver_data.append(resource->driver_data.view())) st = status::kNoMemory;
      for (uint64_t i = 0; st == status::kSuccess && i < datas.count(); ++i) {
        UniquePointer<ByteBuffer> copy = UniquePointer<ByteBuffer>::make(s.allocator(), s.allocator());
        if (!copy || !copy->append(datas[i]->view()) || !share->allocation_data.push_back(util::move(copy))) st = status::kNoMemory;
      }
    }
    if (st == status::kSuccess) {
      // (the tokens are the kernel's: if the share cannot be recorded they are closed again below)
      List<uint32_t> tokens(s.allocator());
      for (uint32_t t : share->tokens.view()) tokens.push_back(t);
      share_id = add_share(util::move(share));
      if (share_id == 0) {
        for (uint32_t t : tokens.view()) s.kernel().close_share(s.pid(), t);
        st = status::kNoMemory;
      }
    }
  }

  // The results: the allocation handles and the private data the miniport filled in, then the resource and the share.
  uint32_t id = 0;
  if (st == status::kSuccess && create_resource) {
    id = ps.next_resource++;
    resource->id = id;
    resource->share = share_id;
    for (uint32_t i = 0; i < created.count(); ++i) {
      ps.find_allocation(created[i])->resource = id;
      resource->allocations.push_back(created[i]);
    }
    for (uint64_t i = 0; i < datas.count(); ++i) {
      UniquePointer<ByteBuffer> copy = UniquePointer<ByteBuffer>::make(s.allocator(), s.allocator());
      if (copy && copy->append(datas[i]->view())) resource->allocation_data.push_back(util::move(copy));
    }
    if (!ps.resources.push_back(util::move(resource))) {
      st = status::kNoMemory;
      id = 0;
    }
  }
  if (st == status::kSuccess) {
    bool wrote = true;
    for (uint32_t i = 0; wrote && i < created.count(); ++i) {
      const uint64_t entry = element_address(infos, i, info_stride);
      wrote = put32(call, entry, 0, created[i]) && call.write(private_addresses[i], datas[i]->view());
    }
    if (wrote && create_resource) wrote = put32(call, arg, 4, resource_handle(id));
    if (wrote && shared) wrote = put32(call, arg, 8, share_id);
    if (!wrote) {
      st = kStatusAccessViolation;
      if (create_resource) drop_resource(s, id);
    }
  } else if (share_id) {
    remove_share(s.kernel(), s.pid(), share_id);
  }
  if (st != status::kSuccess) {
    for (dxgk::Handle h : created.view()) {
      s.kernel().destroy_allocation(s.pid(), h);
      ps.remove_allocation(h);
    }
  }
  return st;
}

uint32_t create_allocation(Session& s, Call& call, uint64_t arg) { return create_allocation_common(s, call, arg, kAllocationInfoStride); }
uint32_t create_allocation2(Session& s, Call& call, uint64_t arg) { return create_allocation_common(s, call, arg, kAllocationInfo2Stride); }

// --- Destroying ------------------------------------------------------------------------------------------------------------------------------------------

uint32_t destroy_allocation(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_destroy_allocation)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const dxgk::Handle resource = b.u32(4);
  const uint64_t list = b.u64(8);
  const uint32_t count = b.u32(16);
  if (device == 0 || (resource != 0) == (count != 0) || count > kMaxAllocationsPerCall) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  if (resource == 0 && (list == 0 || !read_u32_array(call, list, count, &handles))) return load_failure(list);
  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  if (resource != 0) {
    uint32_t id = 0;
    if (!resource_id(resource, &id)) return status::kInvalidHandle;
    ResourceRecord* r = ps.find_resource(id);
    if (!r || r->device != device) return status::kInvalidHandle;
    for (uint64_t i = 0; i < r->allocations.count(); ++i) handles.push_back(r->allocations[i]);
  }
  // Everything named has to be this device's before anything is destroyed.
  for (dxgk::Handle h : handles.view()) {
    AllocationRecord* rec = ps.find_allocation(h);
    if (!rec || rec->device != device) return status::kInvalidHandle;
  }
  uint32_t st = status::kSuccess;
  for (dxgk::Handle h : handles.view()) {
    const uint32_t one = destroy_one(s, call, h);
    if (one != status::kSuccess) st = one;
  }
  return st;
}

// --- Locking -------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t lock_allocation(Session& s, Call& call, uint64_t arg, dxgk::Handle device, dxgk::Handle allocation, uint32_t lock_flags, uint32_t subresource, uint64_t data_offset) {
  if (device == 0 || allocation == 0) return kStatusInvalidParameter;
  Locked g(s);
  ProcessState& ps = s.state();
  AllocationRecord* rec = ps.find_allocation(allocation);
  if (!rec) return status::kInvalidHandle;
  if (rec->device != device) return kStatusInvalidParameter;
  if (!call.process().address_space()) return kStatusNotSupported;      // (no way to show the contents to a process without an address space of its own)
  uint32_t kernel_flags = 0;
  if (lock_flags & kLockReadOnly) kernel_flags |= dxgk::kLockReadOnly;
  if (lock_flags & kLockDoNotWait) kernel_flags |= dxgk::kLockDoNotWait;
  if (lock_flags & kLockDiscard) kernel_flags |= dxgk::kLockDiscard;
  dxgk::LockResult result;
  uint32_t st = s.kernel().lock(s.pid(), allocation, kernel_flags, subresource, &result);
  if (st != status::kSuccess) return st;
  UniquePointer<LockRecord> lock = UniquePointer<LockRecord>::make(s.allocator());
  uint64_t base = 0;
  const uint64_t size = result.data.size();
  bool ok = lock && map_user_region(call, size, &base);
  if (ok) {
    lock->allocation = allocation;
    lock->device = device;
    lock->user_base = base;
    lock->user_size = size;
    lock->read_only = (lock_flags & kLockReadOnly) != 0;
    lock->data = result.data;
    // (a discard lock starts with nothing the caller can rely on: the region is zero)
    if (!(lock_flags & kLockDiscard)) ok = call.write(base, ConstByteView(result.data));
    ok = ok && put64(call, arg, data_offset, base);
  }
  if (ok && !ps.locks.push_back(util::move(lock))) ok = false;
  if (!ok) {
    unmap_user_region(call, base, size);
    s.kernel().unlock(s.pid(), allocation);
    return base == 0 ? status::kNoMemory : kStatusAccessViolation;
  }
  return status::kSuccess;
}

uint32_t lock(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_lock)) return load_failure(arg);
  // (PrivateDriverData is the subresource: the mip level of an image, 0 for a buffer.)
  return lock_allocation(s, call, arg, b.u32(0), b.u32(4), b.u32(32), b.u32(8), 24);
}

uint32_t lock2(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_lock2)) return load_failure(arg);
  return lock_allocation(s, call, arg, b.u32(0), b.u32(4), b.u32(8), 0, 16);
}

uint32_t unlock_one(Session& s, Call& call, dxgk::Handle device, dxgk::Handle allocation) {
  ProcessState& ps = s.state();
  AllocationRecord* rec = ps.find_allocation(allocation);
  if (!rec) return status::kInvalidHandle;
  if (rec->device != device) return kStatusInvalidParameter;
  if (!ps.find_lock(allocation)) return status::kInvalidDeviceState;
  release_lock(s, call, allocation, true);
  return status::kSuccess;
}

uint32_t unlock(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_unlock)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t count = b.u32(4);
  const uint64_t list = b.u64(8);
  if (device == 0 || count == 0 || count > kMaxAllocationsPerCall || list == 0) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  if (!read_u32_array(call, list, count, &handles)) return kStatusAccessViolation;
  Locked g(s);
  // All of them have to be locked by this device before any is unlocked.
  for (dxgk::Handle h : handles.view()) {
    AllocationRecord* rec = s.state().find_allocation(h);
    if (!rec) return status::kInvalidHandle;
    if (rec->device != device) return kStatusInvalidParameter;
    if (!s.state().find_lock(h)) return status::kInvalidDeviceState;
  }
  for (dxgk::Handle h : handles.view()) unlock_one(s, call, device, h);
  return status::kSuccess;
}

uint32_t unlock2(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_unlock2)) return load_failure(arg);
  if (b.u32(0) == 0 || b.u32(4) == 0) return kStatusInvalidParameter;
  Locked g(s);
  return unlock_one(s, call, b.u32(0), b.u32(4));
}

// --- Residency -------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t query_allocation_residency(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_query_residency)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const dxgk::Handle resource = b.u32(4);
  const uint64_t list = b.u64(8);
  const uint32_t count = b.u32(16);
  const uint64_t out = b.u64(24);
  if (device == 0 || out == 0 || count > kMaxAllocationsPerCall || (resource == 0 && count == 0)) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  if (count != 0) {
    if (list == 0 || !read_u32_array(call, list, count, &handles)) return load_failure(list);
  } else {
    uint32_t id = 0;
    if (!resource_id(resource, &id)) return status::kInvalidHandle;
    ResourceRecord* r = ps.find_resource(id);
    if (!r || r->device != device) return status::kInvalidHandle;
    for (uint64_t i = 0; i < r->allocations.count(); ++i) handles.push_back(r->allocations[i]);
  }
  List<uint32_t> states(s.allocator());
  for (dxgk::Handle h : handles.view()) {
    AllocationRecord* rec = ps.find_allocation(h);
    if (!rec || rec->device != device) return status::kInvalidHandle;
    dxgk::AllocationInfo info;
    const uint32_t st = s.kernel().query_allocation(s.pid(), h, &info);
    if (st != status::kSuccess) return st;
    // D3DDDI_ALLOCATIONRESIDENCYSTATUS: 1 resident in GPU memory, 2 resident in shared memory, 3 not resident. System-memory-only kinds live in shared memory.
    const uint32_t f = info.desc.flags;
    const bool system_only = (f & (dxgk::kAllocStaging | dxgk::kAllocConstantBuffer | dxgk::kAllocHeap | dxgk::kAllocQueryResult)) != 0;
    states.push_back(info.resident ? 1u : system_only ? 2u : 3u);
  }
  for (uint64_t i = 0; i < states.count(); ++i)
    if (!put32(call, element_address(out, i, 4), 0, states[i])) return kStatusAccessViolation;
  return status::kSuccess;
}

// --- Shared resources -------------------------------------------------------------------------------------------------------------------------------------

// What the share holds, copied out under the registry's lock.
struct ShareSnapshot {
  explicit ShareSnapshot(allocators::IAllocator& a) : allocator(a), tokens(a), runtime_data(a), driver_data(a), allocation_data(a) {}
  allocators::IAllocator& allocator;
  List<uint32_t> tokens;
  ByteBuffer runtime_data, driver_data;
  List<UniquePointer<ByteBuffer>> allocation_data;
  bool ok = true;
};

void snapshot_share(void* context, ShareRecord& share) {
  ShareSnapshot& snap = *static_cast<ShareSnapshot*>(context);
  for (uint32_t t : share.tokens.view()) snap.ok = snap.ok && snap.tokens.push_back(t);
  snap.ok = snap.ok && snap.runtime_data.append(share.runtime_data.view()) && snap.driver_data.append(share.driver_data.view());
  for (UniquePointer<ByteBuffer>& d : share.allocation_data.view()) {
    UniquePointer<ByteBuffer> copy = UniquePointer<ByteBuffer>::make(snap.allocator, snap.allocator);
    snap.ok = snap.ok && copy && copy->append(d->view()) && snap.allocation_data.push_back(util::move(copy));
  }
}

uint64_t total_driver_bytes(const ShareSnapshot& snap) {
  uint64_t total = snap.driver_data.size();
  for (const UniquePointer<ByteBuffer>& d : snap.allocation_data.view()) total += d->size();
  return total;
}

uint32_t query_resource_info(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_query_resource_info)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t global = b.u32(4);
  const uint64_t runtime_ptr = b.u64(8);
  const uint32_t runtime_size = b.u32(16);
  if (device == 0 || global == 0) return kStatusInvalidParameter;
  {
    Locked g(s);
    if (!s.state().find_device(device)) return status::kInvalidHandle;
  }
  ShareSnapshot snap(s.allocator());
  if (!with_share(global, snapshot_share, &snap)) return status::kInvalidHandle;
  if (!snap.ok) return status::kNoMemory;
  const uint64_t total = total_driver_bytes(snap);
  // Sizes first, as D3DKMTQueryResourceInfo does: PrivateRuntimeDataSize comes back as what the share holds even when the buffer was too small.
  const bool wrote = put32(call, arg, 16, static_cast<uint32_t>(snap.runtime_data.size())) && put32(call, arg, 20, static_cast<uint32_t>(total)) &&
                     put32(call, arg, 24, static_cast<uint32_t>(snap.driver_data.size())) && put32(call, arg, 28, static_cast<uint32_t>(snap.tokens.count()));
  if (!wrote) return kStatusAccessViolation;
  if (runtime_size < snap.runtime_data.size()) return status::kBufferTooSmall;
  if (snap.runtime_data.size() != 0 && !call.write(runtime_ptr, snap.runtime_data.view())) return kStatusAccessViolation;
  return status::kSuccess;
}

uint32_t open_resource_common(Session& s, Call& call, uint64_t arg, uint64_t info_stride) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_open_resource)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t global = b.u32(4);
  const uint32_t count = b.u32(8);
  const uint64_t infos = b.u64(16);
  const uint64_t runtime_ptr = b.u64(24);
  const uint32_t runtime_size = b.u32(32);
  const uint64_t total_ptr = b.u64(56);
  const uint32_t total_size = b.u32(64);
  if (device == 0 || global == 0 || count == 0 || count > kMaxAllocationsPerCall || infos == 0) return kStatusInvalidParameter;
  ShareSnapshot snap(s.allocator());
  if (!with_share(global, snapshot_share, &snap)) return status::kInvalidHandle;
  if (!snap.ok) return status::kNoMemory;
  if (count != snap.tokens.count() || snap.allocation_data.count() != snap.tokens.count()) return kStatusInvalidParameter;
  if (runtime_size < snap.runtime_data.size() || total_size < total_driver_bytes(snap)) return status::kBufferTooSmall;
  if ((snap.runtime_data.size() != 0 && runtime_ptr == 0) || total_ptr == 0) return kStatusInvalidParameter;

  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  if (ps.resources.count() >= kMaxResourcesPerProcess) return status::kInsufficientResources;
  UniquePointer<ResourceRecord> resource = UniquePointer<ResourceRecord>::make(s.allocator(), s.allocator());
  if (!resource) return status::kNoMemory;
  resource->device = device;
  if (!resource->runtime_data.append(snap.runtime_data.view()) || !resource->driver_data.append(snap.driver_data.view())) return status::kNoMemory;

  List<dxgk::Handle> opened(s.allocator());
  uint32_t st = status::kSuccess;
  for (uint32_t i = 0; i < count && st == status::kSuccess; ++i) {
    dxgk::Handle handle = 0;
    dxgk::AllocationDesc desc;
    st = s.kernel().open_shared_allocation(s.pid(), device, snap.tokens[i], &handle, &desc);
    if (st != status::kSuccess) break;
    AllocationRecord rec;
    rec.handle = handle;
    rec.device = device;
    if (!opened.push_back(handle) || !ps.allocations.push_back(rec)) {
      if (opened.count() == 0 || opened[opened.count() - 1] != handle) s.kernel().destroy_allocation(s.pid(), handle);
      st = status::kNoMemory;
    }
  }
  // The private data, laid out as the caller's buffer wants it: the resource's first, then each allocation's.
  ByteBuffer total(s.allocator());
  List<uint64_t> offsets(s.allocator());
  if (st == status::kSuccess) {
    if (!total.append(snap.driver_data.view())) st = status::kNoMemory;
    for (uint32_t i = 0; st == status::kSuccess && i < count; ++i) {
      if (!offsets.push_back(total.size()) || !total.append(snap.allocation_data[i]->view())) st = status::kNoMemory;
    }
  }
  uint32_t id = 0;
  if (st == status::kSuccess) {
    id = ps.next_resource++;
    resource->id = id;
    for (uint32_t i = 0; i < count; ++i) {
      ps.find_allocation(opened[i])->resource = id;
      resource->allocations.push_back(opened[i]);
      UniquePointer<ByteBuffer> copy = UniquePointer<ByteBuffer>::make(s.allocator(), s.allocator());
      if (copy && copy->append(snap.allocation_data[i]->view())) resource->allocation_data.push_back(util::move(copy));
    }
    if (!ps.resources.push_back(util::move(resource))) st = status::kNoMemory;
  }
  if (st == status::kSuccess) {
    bool wrote = (snap.runtime_data.size() == 0 || call.write(runtime_ptr, snap.runtime_data.view())) && (total.size() == 0 || call.write(total_ptr, total.view()));
    wrote = wrote && put64(call, arg, 40, snap.driver_data.size() ? total_ptr : 0) && put32(call, arg, 48, static_cast<uint32_t>(snap.driver_data.size())) &&
            put32(call, arg, 68, resource_handle(id)) && put32(call, arg, 32, static_cast<uint32_t>(snap.runtime_data.size()));
    for (uint32_t i = 0; wrote && i < count; ++i) {
      const uint64_t entry = element_address(infos, i, info_stride);
      wrote = put32(call, entry, 0, opened[i]) && put64(call, entry, 8, element_address(total_ptr, offsets[i], 1)) && put32(call, entry, 16, static_cast<uint32_t>(snap.allocation_data[i]->size()));
    }
    if (!wrote) {
      drop_resource(s, id);
      st = kStatusAccessViolation;
    }
  }
  if (st != status::kSuccess) {
    for (dxgk::Handle h : opened.view()) {
      s.kernel().destroy_allocation(s.pid(), h);
      ps.remove_allocation(h);
    }
  }
  return st;
}

uint32_t open_resource(Session& s, Call& call, uint64_t arg) { return open_resource_common(s, call, arg, kOpenAllocationInfoStride); }
uint32_t open_resource2(Session& s, Call& call, uint64_t arg) { return open_resource_common(s, call, arg, kOpenAllocationInfo2Stride); }

}  // namespace

void register_allocations() {
  using N = WindowsSyscallNumber;
  add<create_allocation>(N::NtGdiDdDDICreateAllocation);
  add<create_allocation2>(N::NtGdiDdDDICreateAllocation2);
  add<destroy_allocation>(N::NtGdiDdDDIDestroyAllocation);
  add<destroy_allocation>(N::NtGdiDdDDIDestroyAllocation2);      // (the same leading fields; the flags of the second only say how Windows may defer the destruction)
  add<lock>(N::NtGdiDdDDILock);
  add<lock2>(N::NtGdiDdDDILock2);
  add<unlock>(N::NtGdiDdDDIUnlock);
  add<unlock2>(N::NtGdiDdDDIUnlock2);
  add<query_allocation_residency>(N::NtGdiDdDDIQueryAllocationResidency);
  add<query_resource_info>(N::NtGdiDdDDIQueryResourceInfo);
  add<open_resource>(N::NtGdiDdDDIOpenResource);
  add<open_resource2>(N::NtGdiDdDDIOpenResource2);
}

}  // namespace process::win32k::dxg
