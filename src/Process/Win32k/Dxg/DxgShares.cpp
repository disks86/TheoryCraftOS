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
#include "Process/Objects.hpp"
#include "Process/WindowsObjects.hpp"

// Resources and fences shared through NT handles: NtGdiDdDDIShareObjects, QueryResourceInfoFromNtHandle, OpenResourceFromNtHandle, OpenSyncObjectFromNtHandle(2),
// OpenNtHandleFromName and OpenSyncObjectNtHandleFromName.
//
// A resource made with NtSecuritySharing (DxgAllocations.cpp) or a fence made with Shared | NtSecuritySharing (DxgSync.cpp) is shared by making a kernel object of the
// object manager's (ObjectKind::DxgSharedResource, "DxgkSharedResource", or DxgSharedSync, "DxgkSharedSyncObject") whose payload holds the kernel-mode driver's exports
// (dxgk::Kernel::export_shared_allocation / export_sync) and, for a resource, the private data its creator gave. The handle is an ordinary NT handle: it is closed,
// duplicated into other processes and inherited as any is, and the object - with the allocations - lives while a handle (or an opened resource) does. A name in the
// OBJECT_ATTRIBUTES puts the object in the namespace (\BaseNamedObjects\... as an event's name is), where OpenNtHandleFromName finds it. Opening imports the
// allocations into the caller's device (the same allocations: what one process draws the other sees), on the adapter that made them.
namespace process::win32k::dxg {

namespace {

using namespace layout;
namespace status = dxgk::status;

constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024u, kStatusBufferTooSmall = 0xC0000023u;

class Locked {
 public:
  explicit Locked(Session& s) : mutex_(s.state().lock) { mutex_.lock(); }
  ~Locked() { mutex_.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  kernel::Mutex& mutex_;
};

bool resource_id(dxgk::Handle handle, uint32_t* id) {
  if ((handle & 0xF0000000u) != kResourceHandleBase) return false;
  *id = handle & 0x0FFFFFFFu;
  return *id != 0;
}

// What a shared resource's object holds: one export for each allocation (in the creator's order), and the private data the creator gave, which an opener receives.
class SharedResource final : public ObjectPayload {
 public:
  explicit SharedResource(allocators::IAllocator& a) : exports(a), runtime_data(a), driver_data(a), allocation_data(a), allocator(a) {}
  List<SharedPointer<dxgk::ExportedAllocation>> exports;
  ByteBuffer runtime_data, driver_data;
  List<UniquePointer<ByteBuffer>> allocation_data;
  allocators::IAllocator& allocator;
  SharedPointer<KeyedMutexState> keyed_mutex;          // shared with it (ShareObjects of the resource and the mutex: DxgKeyedMutex.cpp), or empty
  SharedPointer<dxgk::ExportedSync> sync;              // ... and the synchronization object shared with them, or empty
  uint64_t total_driver_bytes() const {
    uint64_t total = 0;
    for (const UniquePointer<ByteBuffer>& d : allocation_data.view()) total += d->size();
    return total;
  }
};
// A shared fence's object.
class SharedSync final : public ObjectPayload {
 public:
  SharedPointer<dxgk::ExportedSync> sync;
};

// The memory the objects come from: they outlive the process that made them, so the system's (the object manager's) when there is one.
allocators::IAllocator& object_allocator(Session& s, Call& call) {
  allocators::IAllocator* system = call.allocator();
  return system ? *system : s.allocator();
}

// The payload of an NT handle of the caller's, of the kind asked for.
template <class T>
uint32_t payload_of(Call& call, uint64_t handle, ObjectKind kind, SharedPointer<ObjectPayload>* holder, T** out) {
  const uint32_t st = windowsabi::payload_of_handle(call.process(), handle, kind, 0, holder);
  if (st != 0) return st;
  *out = static_cast<T*>(holder->get());      // (the kind says what the payload is: only this file makes them)
  return status::kSuccess;
}

// --- Sharing --------------------------------------------------------------------------------------------------------------------------------------------------

// NtGdiDdDDIShareObjects(UINT cObjects, const D3DKMT_HANDLE* hObjects, OBJECT_ATTRIBUTES* pObjectAttributes, DWORD dwDesiredAccess, HANDLE* phSharedNtHandle): a resource
// made with NtSecuritySharing - alone, with its keyed mutex (made with NtSecuritySharing), or with the mutex and a fence made shared - or a fence made shared, alone.
uint32_t share_objects(Session& s, Call& call, uint64_t) {
  const uint32_t count = call.arg32(0);
  const uint64_t handles = call.arg(1), attributes = call.arg(2), out = call.arg(4);
  const uint32_t access = call.arg32(3);
  if (call.bad_stack()) return kStatusAccessViolation;
  if (count == 0 || count > 3 || handles == 0 || out == 0) return kStatusInvalidParameter;
  uint32_t list[3] = {0, 0, 0};
  for (uint32_t i = 0; i < count; ++i)
    if (!call.read_u32(handles + 4 * uint64_t{i}, &list[i])) return kStatusAccessViolation;
  const uint32_t handle = list[0];
  allocators::IAllocator& allocator = object_allocator(s, call);
  SharedPointer<ObjectPayload> payload;
  ObjectKind kind = ObjectKind::None;
  uint32_t id = 0;
  if (resource_id(handle, &id)) {
    SharedPointer<SharedResource> shared = SharedPointer<SharedResource>::make(allocator, allocator);
    if (!shared) return status::kNoMemory;
    Locked g(s);
    ResourceRecord* r = s.state().find_resource(id);
    if (!r) return status::kInvalidHandle;
    if (!r->nt_shareable || r->allocations.count() == 0) return kStatusInvalidParameter;
    if (count >= 2) {      // (the keyed mutex that orders its use)
      shared->keyed_mutex = keyed_mutex_of(s.state(), list[1]);
      if (!shared->keyed_mutex) return status::kInvalidHandle;
      if (!shared->keyed_mutex->nt_shareable) return kStatusInvalidParameter;
    }
    if (count == 3) {      // (and a fence)
      SyncRecord* sync = s.state().find_sync(list[2]);
      if (!sync) return status::kInvalidHandle;
      if (!sync->nt_shareable) return kStatusInvalidParameter;
      const uint32_t st = s.kernel().export_sync(s.pid(), list[2], &shared->sync);
      if (st != status::kSuccess) return st;
    }
    if (!shared->runtime_data.append(r->runtime_data.view()) || !shared->driver_data.append(r->driver_data.view())) return status::kNoMemory;
    for (uint64_t i = 0; i < r->allocations.count(); ++i) {
      SharedPointer<dxgk::ExportedAllocation> exported;
      const uint32_t st = s.kernel().export_shared_allocation(s.pid(), r->allocations[i], &exported);
      if (st != status::kSuccess) return st;
      UniquePointer<ByteBuffer> data = UniquePointer<ByteBuffer>::make(allocator, allocator);
      if (!data || (i < r->allocation_data.count() && !data->append(r->allocation_data[i]->view()))) return status::kNoMemory;
      if (!shared->exports.push_back(util::move(exported)) || !shared->allocation_data.push_back(util::move(data))) return status::kNoMemory;
    }
    payload = util::move(shared);
    kind = ObjectKind::DxgSharedResource;
  } else {
    if (count != 1) return kStatusInvalidParameter;      // (a fence is shared alone)
    SharedPointer<SharedSync> shared = SharedPointer<SharedSync>::make(allocator);
    if (!shared) return status::kNoMemory;
    Locked g(s);
    SyncRecord* sync = s.state().find_sync(handle);
    if (!sync) return status::kInvalidHandle;
    if (!sync->nt_shareable) return kStatusInvalidParameter;
    const uint32_t st = s.kernel().export_sync(s.pid(), handle, &shared->sync);
    if (st != status::kSuccess) return st;
    payload = util::move(shared);
    kind = ObjectKind::DxgSharedSync;
  }
  return windowsabi::create_payload_object(call.process(), kind, attributes, access, util::move(payload), out);
}

// --- Resources ------------------------------------------------------------------------------------------------------------------------------------------------

// D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE { hDevice @0; hNtHandle @8; pPrivateRuntimeData @16; PrivateRuntimeDataSize @24 (in: the buffer's, out: the data's);
// TotalPrivateDriverDataSize @28; ResourcePrivateDriverDataSize @32; NumAllocations @36 }. With no buffer, the sizes alone.
uint32_t query_resource_info_from_nt_handle(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 40)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint64_t nt = b.u64(8), runtime_ptr = b.u64(16);
  const uint32_t runtime_size = b.u32(24);
  if (device == 0 || nt == 0) return kStatusInvalidParameter;
  {
    Locked g(s);
    if (!s.state().find_device(device)) return status::kInvalidHandle;
  }
  SharedPointer<ObjectPayload> holder;
  SharedResource* r = nullptr;
  if (const uint32_t st = payload_of(call, nt, ObjectKind::DxgSharedResource, &holder, &r)) return st;
  const bool wrote = put32(call, arg, 24, static_cast<uint32_t>(r->runtime_data.size())) && put32(call, arg, 28, static_cast<uint32_t>(r->total_driver_bytes())) &&
                     put32(call, arg, 32, static_cast<uint32_t>(r->driver_data.size())) && put32(call, arg, 36, static_cast<uint32_t>(r->exports.count()));
  if (!wrote) return kStatusAccessViolation;
  if (runtime_ptr == 0) return status::kSuccess;
  if (runtime_size < r->runtime_data.size()) return kStatusBufferTooSmall;
  if (r->runtime_data.size() != 0 && !call.write(runtime_ptr, r->runtime_data.view())) return kStatusAccessViolation;
  return status::kSuccess;
}

// D3DKMT_OPENRESOURCEFROMNTHANDLE { hDevice @0; hNtHandle @8; NumAllocations @16; pOpenAllocationInfo2 @24 (80 bytes each: hAllocation @0, pPrivateDriverData @8,
// PrivateDriverDataSize @16, GpuVirtualAddress @24); PrivateRuntimeDataSize @32; pPrivateRuntimeData @40; ResourcePrivateDriverDataSize @48;
// pResourcePrivateDriverData @56; TotalPrivateDriverDataBufferSize @64; pTotalPrivateDriverDataBuffer @72; hResource @80 (out); hKeyedMutex @84 (out);
// pKeyedMutexPrivateRuntimeData @88; KeyedMutexPrivateRuntimeDataSize @96; hSyncObject @100 (out) }. The allocations' private data goes into the total buffer one after
// the other, each entry pointing at its own; the resource's into its buffer. A keyed mutex shared with it gets a handle of the caller's (its private runtime data into
// pKeyedMutexPrivateRuntimeData), and so does a fence shared with them (on the device); 0 for what was not shared.
uint32_t open_resource_from_nt_handle(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 104)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint64_t nt = b.u64(8);
  const uint32_t count = b.u32(16);
  const uint64_t infos = b.u64(24);
  const uint32_t runtime_size = b.u32(32);
  const uint64_t runtime_ptr = b.u64(40);
  const uint32_t resource_size = b.u32(48);
  const uint64_t resource_ptr = b.u64(56);
  const uint32_t total_size = b.u32(64);
  const uint64_t total_ptr = b.u64(72);
  const uint64_t mutex_data = b.u64(88);
  const uint32_t mutex_size = b.u32(96);
  if (device == 0 || nt == 0 || count == 0 || count > kMaxAllocationsPerCall || infos == 0) return kStatusInvalidParameter;
  SharedPointer<ObjectPayload> holder;
  SharedResource* shared = nullptr;
  if (const uint32_t st = payload_of(call, nt, ObjectKind::DxgSharedResource, &holder, &shared)) return st;
  if (count != shared->exports.count()) return kStatusInvalidParameter;
  if (runtime_size < shared->runtime_data.size() || resource_size < shared->driver_data.size() || total_size < shared->total_driver_bytes()) return kStatusBufferTooSmall;
  if ((shared->runtime_data.size() && runtime_ptr == 0) || (shared->driver_data.size() && resource_ptr == 0) || (shared->total_driver_bytes() && total_ptr == 0))
    return kStatusInvalidParameter;
  if (shared->keyed_mutex && shared->keyed_mutex->runtime_data.size() && (mutex_size < shared->keyed_mutex->runtime_data.size() || mutex_data == 0))
    return mutex_size < shared->keyed_mutex->runtime_data.size() ? kStatusBufferTooSmall : kStatusInvalidParameter;

  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  if (ps.resources.count() >= kMaxResourcesPerProcess) return status::kInsufficientResources;
  UniquePointer<ResourceRecord> resource = UniquePointer<ResourceRecord>::make(s.allocator(), s.allocator());
  if (!resource) return status::kNoMemory;
  resource->device = device;
  resource->nt_shareable = true;      /* (an opened resource is shared on as its creator's is) */
  if (!resource->runtime_data.append(shared->runtime_data.view()) || !resource->driver_data.append(shared->driver_data.view())) return status::kNoMemory;
  List<dxgk::Handle> opened(s.allocator());
  uint32_t st = status::kSuccess;
  for (uint32_t i = 0; i < count && st == status::kSuccess; ++i) {
    dxgk::Handle handle = 0;
    st = s.kernel().import_allocation(s.pid(), device, *shared->exports[i], &handle, nullptr);
    if (st != status::kSuccess) break;
    AllocationRecord rec;
    rec.handle = handle;
    rec.device = device;
    if (!opened.push_back(handle)) { s.kernel().destroy_allocation(s.pid(), handle); st = status::kNoMemory; break; }
    if (!ps.allocations.push_back(rec)) st = status::kNoMemory;
  }
  uint32_t id = 0;
  if (st == status::kSuccess) {
    id = ps.next_resource++;
    resource->id = id;
    for (uint32_t i = 0; i < count && st == status::kSuccess; ++i) {
      ps.find_allocation(opened[i])->resource = id;
      UniquePointer<ByteBuffer> copy = UniquePointer<ByteBuffer>::make(s.allocator(), s.allocator());
      if (!copy || !copy->append(shared->allocation_data[i]->view()) || !resource->allocations.push_back(opened[i]) || !resource->allocation_data.push_back(util::move(copy)))
        st = status::kNoMemory;
    }
    if (st == status::kSuccess && !ps.resources.push_back(util::move(resource))) st = status::kNoMemory;
  }
  if (st == status::kSuccess) {
    bool wrote = (shared->runtime_data.size() == 0 || call.write(runtime_ptr, shared->runtime_data.view())) &&
                 (shared->driver_data.size() == 0 || call.write(resource_ptr, shared->driver_data.view()));
    uint64_t at = 0;
    for (uint32_t i = 0; wrote && i < count; ++i) {
      const ByteBuffer& data = *shared->allocation_data[i];
      const uint64_t entry = element_address(infos, i, kOpenAllocationInfo2Stride);
      wrote = (data.size() == 0 || call.write(total_ptr + at, data.view())) && put32(call, entry, 0, opened[i]) && put64(call, entry, 8, data.size() ? total_ptr + at : 0) &&
              put32(call, entry, 16, static_cast<uint32_t>(data.size())) && put64(call, entry, 24, 0);
      at += data.size();
    }
    // (the keyed mutex and the fence shared with it: handles of the caller's)
    dxgk::Handle mutex = 0, sync = 0;
    if (wrote && shared->keyed_mutex) {
      mutex = add_keyed_mutex(ps, shared->keyed_mutex);
      wrote = mutex != 0 && (shared->keyed_mutex->runtime_data.size() == 0 || call.write(mutex_data, shared->keyed_mutex->runtime_data.view())) &&
              put32(call, arg, 96, static_cast<uint32_t>(shared->keyed_mutex->runtime_data.size()));
    }
    if (wrote && shared->sync) {
      wrote = s.kernel().import_sync(s.pid(), device, *shared->sync, &sync) == status::kSuccess;
      SyncRecord rec;
      rec.handle = sync;
      rec.device = device;
      rec.type = dxgk::SyncType::Fence;
      rec.nt_shareable = true;
      if (wrote && !ps.syncs.push_back(rec)) { s.kernel().destroy_sync(s.pid(), sync); sync = 0; wrote = false; }
    }
    wrote = wrote && put32(call, arg, 32, static_cast<uint32_t>(shared->runtime_data.size())) && put32(call, arg, 48, static_cast<uint32_t>(shared->driver_data.size())) &&
            put32(call, arg, 64, static_cast<uint32_t>(shared->total_driver_bytes())) && put32(call, arg, 80, kResourceHandleBase | id) && put32(call, arg, 84, mutex) &&
            put32(call, arg, 100, sync);
    if (!wrote) {
      for (uint64_t i = 0; i < ps.resources.count(); ++i)
        if (ps.resources[i]->id == id) { ps.resources.remove_at(i); break; }
      for (uint64_t i = 0; mutex && i < ps.keyed_mutexes.count(); ++i)
        if (ps.keyed_mutexes[i].handle == mutex) { ps.keyed_mutexes.remove_at(i); break; }
      if (sync) {
        s.kernel().destroy_sync(s.pid(), sync);
        for (uint64_t i = 0; i < ps.syncs.count(); ++i)
          if (ps.syncs[i].handle == sync) { ps.syncs.remove_at(i); break; }
      }
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

// D3DKMT_OPENKEYEDMUTEXFROMNTHANDLE { hNtHandle @0; hKeyedMutex @8 (out); pPrivateRuntimeData @16; PrivateRuntimeDataSize @24 }: the keyed mutex shared with a resource
// (STATUS_INVALID_PARAMETER for a resource shared without one).
uint32_t open_keyed_mutex_from_nt_handle(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 32)) return load_failure(arg);
  const uint64_t nt = b.u64(0), data = b.u64(16);
  const uint32_t size = b.u32(24);
  if (nt == 0) return kStatusInvalidParameter;
  SharedPointer<ObjectPayload> holder;
  SharedResource* shared = nullptr;
  if (const uint32_t st = payload_of(call, nt, ObjectKind::DxgSharedResource, &holder, &shared)) return st;
  if (!shared->keyed_mutex) return kStatusInvalidParameter;
  const ByteBuffer& runtime = shared->keyed_mutex->runtime_data;
  if (runtime.size() && size < runtime.size()) return kStatusBufferTooSmall;
  if (runtime.size() && (!data || !call.write(data, runtime.view()))) return data ? kStatusAccessViolation : kStatusInvalidParameter;
  Locked g(s);
  const dxgk::Handle mutex = add_keyed_mutex(s.state(), shared->keyed_mutex);
  if (!mutex) return status::kInsufficientResources;
  if (put32(call, arg, 8, mutex) && put32(call, arg, 24, static_cast<uint32_t>(runtime.size()))) return status::kSuccess;
  for (uint64_t i = 0; i < s.state().keyed_mutexes.count(); ++i)
    if (s.state().keyed_mutexes[i].handle == mutex) { s.state().keyed_mutexes.remove_at(i); break; }
  return kStatusAccessViolation;
}

// --- Fences ------------------------------------------------------------------------------------------------------------------------------------------------

// A synchronization object of the caller's for the shared fence, on `device` (0: the caller's first device on the fence's kernel - D3DKMTOpenSyncObjectFromNtHandle names
// none). Writes the handle at arg + handle_offset.
uint32_t open_sync(Session& s, Call& call, uint64_t nt, dxgk::Handle device, uint64_t arg, uint64_t handle_offset) {
  SharedPointer<ObjectPayload> holder;
  SharedSync* shared = nullptr;
  if (const uint32_t st = payload_of(call, nt, ObjectKind::DxgSharedSync, &holder, &shared)) return st;
  Locked g(s);
  ProcessState& ps = s.state();
  if (device == 0) {
    if (ps.devices.count() == 0) return kStatusInvalidParameter;
    device = ps.devices[0].handle;
  }
  if (!ps.find_device(device)) return status::kInvalidHandle;
  dxgk::Handle sync = 0;
  const uint32_t st = s.kernel().import_sync(s.pid(), device, *shared->sync, &sync);
  if (st != status::kSuccess) return st;
  SyncRecord rec;
  rec.handle = sync;
  rec.device = device;
  rec.type = dxgk::SyncType::Fence;
  rec.nt_shareable = true;      /* (shared on as its creator's is) */
  if (!ps.syncs.push_back(rec)) { s.kernel().destroy_sync(s.pid(), sync); return status::kNoMemory; }
  if (!put32(call, arg, handle_offset, sync)) {
    s.kernel().destroy_sync(s.pid(), sync);
    ps.syncs.remove_at(ps.syncs.count() - 1);
    return kStatusAccessViolation;
  }
  return status::kSuccess;
}

// D3DKMT_OPENSYNCOBJECTFROMNTHANDLE { hNtHandle @0; hSyncObject @8 }
uint32_t open_sync_object_from_nt_handle(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 16)) return load_failure(arg);
  if (b.u64(0) == 0) return kStatusInvalidParameter;
  return open_sync(s, call, b.u64(0), 0, arg, 8);
}

// D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 { hNtHandle @0; hDevice @8; Flags @12; hSyncObject @16; MonitoredFence { FenceValueCPUVirtualAddress @24;
// FenceValueGPUVirtualAddress @32; EngineAffinity @40 } }. The fences here are not monitored fences with a value in memory: both addresses come back 0.
uint32_t open_sync_object_from_nt_handle2(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 88)) return load_failure(arg);
  const uint64_t nt = b.u64(0);
  const dxgk::Handle device = b.u32(8);
  if (nt == 0 || device == 0) return kStatusInvalidParameter;
  if (b.u32(12) & ~uint32_t{0x3u}) return kStatusNotSupported;      // (Shared and NtSecuritySharing say what it is; anything else asks for what this layer does not make)
  const uint32_t st = open_sync(s, call, nt, device, arg, 16);
  if (st != status::kSuccess) return st;
  return put64(call, arg, 24, 0) && put64(call, arg, 32, 0) ? status::kSuccess : kStatusAccessViolation;
}

// --- Names -----------------------------------------------------------------------------------------------------------------------------------------------------

// D3DKMT_OPENNTHANDLEFROMNAME / D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME { dwDesiredAccess @0; pObjAttrib @8; hNtHandle @16 }
uint32_t open_by_name(Call& call, uint64_t arg, ObjectKind kind) {
  UserBlock b;
  if (!b.load(call, arg, 24)) return load_failure(arg);
  if (b.u64(8) == 0) return kStatusInvalidParameter;
  return windowsabi::open_payload_object(call.process(), kind, b.u64(8), b.u32(0), arg + 16);
}
uint32_t open_nt_handle_from_name(Session&, Call& call, uint64_t arg) { return open_by_name(call, arg, ObjectKind::DxgSharedResource); }
uint32_t open_sync_object_nt_handle_from_name(Session&, Call& call, uint64_t arg) { return open_by_name(call, arg, ObjectKind::DxgSharedSync); }

}  // namespace

void register_shares() {
  using N = WindowsSyscallNumber;
  add<share_objects>(N::NtGdiDdDDIShareObjects);
  add<query_resource_info_from_nt_handle>(N::NtGdiDdDDIQueryResourceInfoFromNtHandle);
  add<open_resource_from_nt_handle>(N::NtGdiDdDDIOpenResourceFromNtHandle);
  add<open_sync_object_from_nt_handle>(N::NtGdiDdDDIOpenSyncObjectFromNtHandle);
  add<open_sync_object_from_nt_handle2>(N::NtGdiDdDDIOpenSyncObjectFromNtHandle2);
  add<open_nt_handle_from_name>(N::NtGdiDdDDIOpenNtHandleFromName);
  add<open_sync_object_nt_handle_from_name>(N::NtGdiDdDDIOpenSyncObjectNtHandleFromName);
  add<open_keyed_mutex_from_nt_handle>(N::NtGdiDdDDIOpenKeyedMutexFromNtHandle);
}

}  // namespace process::win32k::dxg
