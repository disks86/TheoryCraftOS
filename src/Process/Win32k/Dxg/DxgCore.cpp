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
#include "Process/AddressSpace.hpp"
#include "Process/Win32k/Dxg/DxgInternal.hpp"

// The shared parts of the Direct3D services: the registry of processes and global shares, the Session of one call, the helpers that reach the caller's memory, the polling wait
// and the CPU views of locked allocations.
namespace process::win32k::dxg {

namespace {

struct OwnerRecord {
  uint32_t adapter = 0;
  uint64_t pid = 0;
  dxgk::Handle device = 0;
  SourceOwner kind = SourceOwner::None;
};

struct Registry {
  explicit Registry(allocators::IAllocator& a) : allocator(a), states(a), shares(a), owners(a) {}
  allocators::IAllocator& allocator;
  List<SharedPointer<ProcessState>> states;
  List<UniquePointer<ShareRecord>> shares;
  List<OwnerRecord> owners;
  uint32_t next_share = 0x40000001u;
};

kernel::Mutex g_lock;
UniquePointer<Registry> g_registry;

// With g_lock held.
Registry* registry_locked() {
  if (!g_registry) {
    allocators::IAllocator* allocator = system_allocator();
    if (!allocator) return nullptr;
    g_registry = UniquePointer<Registry>::make(*allocator, *allocator);
  }
  return g_registry.get();
}

uint64_t field_address(uint64_t address, uint64_t offset) {
  return address == 0 || offset > ~uint64_t{0} - address ? 0 : address + offset;
}

}  // namespace

// --- Writing to the caller ----------------------------------------------------------------------------------------------------------------------------------

bool put32(Call& call, uint64_t address, uint64_t offset, uint32_t value) {
  const uint64_t at = field_address(address, offset);
  return at != 0 && call.write_u32(at, value);
}

bool put64(Call& call, uint64_t address, uint64_t offset, uint64_t value) {
  const uint64_t at = field_address(address, offset);
  return at != 0 && call.write_u64(at, value);
}

bool read_u32_array(Call& call, uint64_t address, uint32_t count, List<uint32_t>* out) {
  if (!out->reserve(count)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    const uint64_t at = field_address(address, uint64_t{i} * 4);
    if (at == 0 || !call.read_u32(at, &v) || !out->push_back(v)) return false;
  }
  return true;
}

bool read_u64_array(Call& call, uint64_t address, uint32_t count, List<uint64_t>* out) {
  if (!out->reserve(count)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t v = 0;
    const uint64_t at = field_address(address, uint64_t{i} * 8);
    if (at == 0 || !call.read_u64(at, &v) || !out->push_back(v)) return false;
  }
  return true;
}

bool read_blob(Call& call, uint64_t address, uint32_t size, ByteBuffer* out, uint32_t limit) {
  if (size > limit) return false;
  out->clear();
  if (size == 0) return true;
  if (!out->put_zeros(size)) return false;
  return call.read(address, out->writable_view());
}

// --- The process state ------------------------------------------------------------------------------------------------------------------------------------

AdapterRecord* ProcessState::find_adapter(dxgk::Handle handle) {
  for (AdapterRecord& r : adapters.view())
    if (r.handle == handle) return &r;
  return nullptr;
}

AllocationRecord* ProcessState::find_allocation(dxgk::Handle handle) {
  for (AllocationRecord& r : allocations.view())
    if (r.handle == handle) return &r;
  return nullptr;
}

ContextRecord* ProcessState::find_context(dxgk::Handle handle) {
  for (ContextRecord& r : contexts.view())
    if (r.context == handle) return &r;
  return nullptr;
}

ResourceRecord* ProcessState::find_resource(uint32_t id) {
  for (UniquePointer<ResourceRecord>& r : resources.view())
    if (r->id == id) return r.get();
  return nullptr;
}

DeviceRecord* ProcessState::find_device(dxgk::Handle handle) {
  for (DeviceRecord& r : devices.view())
    if (r.handle == handle) return &r;
  return nullptr;
}

SyncRecord* ProcessState::find_sync(dxgk::Handle handle) {
  for (SyncRecord& r : syncs.view())
    if (r.handle == handle) return &r;
  return nullptr;
}

LockRecord* ProcessState::find_lock(dxgk::Handle allocation) {
  for (UniquePointer<LockRecord>& r : locks.view())
    if (r->allocation == allocation) return r.get();
  return nullptr;
}

void ProcessState::remove_allocation(dxgk::Handle handle) {
  for (uint64_t i = 0; i < allocations.count(); ++i)
    if (allocations[i].handle == handle) {
      allocations.remove_at(i);
      return;
    }
}

// --- The registry ---------------------------------------------------------------------------------------------------------------------------------------------

SharedPointer<ProcessState> find_state(uint64_t pid) {
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r) return SharedPointer<ProcessState>();
  for (SharedPointer<ProcessState>& s : r->states.view())
    if (s->pid() == pid) return s;
  return SharedPointer<ProcessState>();
}

// The state of the calling process, made (and the process opened in the driver) on its first call. A state made for another driver (a test replaced the kernel) is dropped.
SharedPointer<ProcessState> state_for(Call& call, dxgk::Kernel* kernel, uint32_t* status) {
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r) {
    *status = kStatusNoMemory;
    return SharedPointer<ProcessState>();
  }
  for (uint64_t i = 0; i < r->states.count(); ++i) {
    if (r->states[i]->pid() != call.pid()) continue;
    if (r->states[i]->kernel() == kernel) return r->states[i];
    r->states.remove_at(i);
    break;
  }
  const uint32_t s = kernel->open_process(call.pid());
  if (s != dxgk::status::kSuccess) {
    *status = s;
    return SharedPointer<ProcessState>();
  }
  SharedPointer<ProcessState> state = SharedPointer<ProcessState>::make(r->allocator, r->allocator, call.pid(), kernel);
  if (!state || !r->states.push_back(state)) {
    kernel->close_process(call.pid());
    *status = kStatusNoMemory;
    return SharedPointer<ProcessState>();
  }
  return state;
}

Session::Session(Call& call) : pid_(call.pid()) {
  kernel_ = dxgk::system_kernel();
  if (!kernel_) {
    failure_ = nt::kNoSuchDevice;
    return;
  }
  uint32_t status = 0;
  state_ = state_for(call, kernel_, &status);
  if (!state_) failure_ = status ? status : kStatusNoMemory;
}

void process_exited(uint64_t pid) {
  SharedPointer<ProcessState> state;
  dxgk::Kernel* kernel = nullptr;
  {
    kernel::ScopedMutex g(g_lock);
    Registry* r = registry_locked();
    if (!r) return;
    for (uint64_t i = 0; i < r->states.count(); ++i)
      if (r->states[i]->pid() == pid) {
        state = r->states[i];
        r->states.remove_at(i);
        break;
      }
    for (uint64_t i = r->shares.count(); i-- > 0;)
      if (r->shares[i]->owner == pid) r->shares.remove_at(i);
    for (uint64_t i = r->owners.count(); i-- > 0;)
      if (r->owners[i].pid == pid) r->owners.remove_at(i);
  }
  // (the kernel destroys the process's devices, allocations and the share tokens it made; the layer's records go with `state`)
  if (state) kernel = state->kernel();
  if (kernel && kernel == dxgk::system_kernel()) {
    // (with the process lock: a thread of the process that is still inside a call is using views of memory the kernel frees here)
    kernel::ScopedMutex g(state->lock);
    kernel->close_process(pid);
  }
}

// --- Video present source ownership -----------------------------------------------------------------------------------------------------------------------

uint32_t set_source_owner(uint32_t adapter, uint64_t pid, dxgk::Handle device, uint32_t type) {
  if (type > kOwnerEmulated || type == kOwnerExclusiveGdi) return kStatusInvalidParameter;
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r) return kStatusNoMemory;
  OwnerRecord* owner = nullptr;
  uint64_t index = 0;
  for (uint64_t i = 0; i < r->owners.count(); ++i)
    if (r->owners[i].adapter == adapter) {
      owner = &r->owners[i];
      index = i;
    }
  const bool mine = owner && owner->pid == pid && owner->device == device;
  if (type == kOwnerUnowned) {
    if (mine) r->owners.remove_at(index);
    return dxgk::status::kSuccess;
  }
  if (type == kOwnerShared) return mine ? kStatusInvalidParameter : kStatusVidPnSourceInUse;      // (the shared owner is the desktop's, never a program's)
  const SourceOwner want = type == kOwnerExclusive ? SourceOwner::Exclusive : SourceOwner::Emulated;
  if (!owner) {
    OwnerRecord rec;
    rec.adapter = adapter, rec.pid = pid, rec.device = device, rec.kind = want;
    return r->owners.push_back(rec) ? dxgk::status::kSuccess : kStatusNoMemory;
  }
  if (!mine) return kStatusVidPnSourceInUse;
  return owner->kind == want ? dxgk::status::kSuccess : kStatusInvalidParameter;
}

void release_source_owner(uint64_t pid, dxgk::Handle device) {
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r) return;
  for (uint64_t i = r->owners.count(); i-- > 0;)
    if (r->owners[i].pid == pid && (device == 0 || r->owners[i].device == device)) r->owners.remove_at(i);
}

bool source_is_exclusive(uint32_t adapter) {
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r) return false;
  for (const OwnerRecord& o : r->owners.view())
    if (o.adapter == adapter && o.kind == SourceOwner::Exclusive) return true;
  return false;
}

void shutdown_for_tests() {
  kernel::ScopedMutex g(g_lock);
  g_registry.reset();
}

// --- Global shares ------------------------------------------------------------------------------------------------------------------------------------------

uint32_t add_share(UniquePointer<ShareRecord> record) {
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r || !record || r->next_share >= 0x7FFFFFFFu) return 0;
  record->id = r->next_share++;
  const uint32_t id = record->id;
  return r->shares.push_back(util::move(record)) ? id : 0;
}

bool with_share(uint32_t id, void (*use)(void* context, ShareRecord& share), void* context) {
  kernel::ScopedMutex g(g_lock);
  Registry* r = registry_locked();
  if (!r) return false;
  for (UniquePointer<ShareRecord>& s : r->shares.view())
    if (s->id == id) {
      use(context, *s);
      return true;
    }
  return false;
}

void remove_share(dxgk::Kernel& kernel, uint64_t owner, uint32_t id) {
  UniquePointer<ShareRecord> gone;
  {
    kernel::ScopedMutex g(g_lock);
    Registry* r = registry_locked();
    if (!r) return;
    for (uint64_t i = 0; i < r->shares.count(); ++i)
      if (r->shares[i]->id == id && r->shares[i]->owner == owner) {
        gone = util::move(r->shares[i]);
        r->shares.remove_at(i);
        break;
      }
  }
  if (gone)
    for (uint32_t token : gone->tokens.view()) kernel.close_share(owner, token);
}

// --- Waiting -----------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t wait_for_syncs(Session& s, Call& call, ArrayView<const dxgk::Handle> handles, ArrayView<const uint64_t> values, bool wait_all, uint64_t timeout_ns) {
  Process& p = call.process();
  const uint64_t started = p.host().monotonic_ns();
  uint64_t nap = 20000;      // 20 us, doubling up to 1 ms
  for (;;) {
    const uint32_t st = s.kernel().wait_sync_cpu(s.pid(), handles, values, wait_all, 0);
    if (st != dxgk::status::kTimeout) return st;
    if (timeout_ns == 0) return st;
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) return dxgk::status::kTimeout;
      left = timeout_ns - used;
    }
    IProcessRuntime* runtime = p.runtime();
    if (p.exited() || (runtime && runtime->interrupted())) return nt::kThreadIsTerminating;
    p.host().sleep_ns(nap < left ? nap : left);
    if (nap < 1000000) nap *= 2;
  }
}

// --- Memory of the process ---------------------------------------------------------------------------------------------------------------------------------

bool map_user_region(Call& call, uint64_t size, uint64_t* base) {
  AddressSpace* space = call.process().address_space();
  uint64_t rounded = 0;
  if (!space || size == 0 || !AddressSpace::round_up(size, &rounded)) return false;
  uint64_t at = 0;
  if (!space->find_free(rounded, call.process().mmap_cursor(), &at)) return false;
  if (!space->map(at, rounded, kProtRead | kProtWrite)) return false;
  *base = at;
  return true;
}

void unmap_user_region(Call& call, uint64_t base, uint64_t size) {
  AddressSpace* space = call.process().address_space();
  if (space && base != 0 && size != 0) space->unmap(base, size);
}

// --- CPU views ----------------------------------------------------------------------------------------------------------------------------------------------------

void release_lock(Session& s, Call& call, dxgk::Handle allocation, bool write_back) {
  ProcessState& ps = s.state();
  for (uint64_t i = 0; i < ps.locks.count(); ++i) {
    LockRecord& rec = *ps.locks[i];
    if (rec.allocation != allocation) continue;
    if (write_back && !rec.read_only) call.read(rec.user_base, rec.data);      // (an unreadable view leaves the old contents)
    s.kernel().unlock(s.pid(), allocation);
    unmap_user_region(call, rec.user_base, rec.user_size);
    ps.locks.remove_at(i);
    return;
  }
}

void forget_device(Session& s, Call& call, dxgk::Handle device) {
  ProcessState& ps = s.state();
  release_source_owner(s.pid(), device);
  // CPU views first: they point into memory the kernel frees with the device.
  for (uint64_t i = ps.locks.count(); i-- > 0;)
    if (ps.locks[i]->device == device) release_lock(s, call, ps.locks[i]->allocation, false);
  for (uint64_t i = ps.contexts.count(); i-- > 0;)
    if (ps.contexts[i].device == device) {
      unmap_user_region(call, ps.contexts[i].region_base, ps.contexts[i].region_size);
      ps.contexts.remove_at(i);
    }
  for (uint64_t i = ps.allocations.count(); i-- > 0;)
    if (ps.allocations[i].device == device) ps.allocations.remove_at(i);
  for (uint64_t i = ps.resources.count(); i-- > 0;)
    if (ps.resources[i]->device == device) {
      if (ps.resources[i]->share) remove_share(s.kernel(), s.pid(), ps.resources[i]->share);
      ps.resources.remove_at(i);
    }
  for (uint64_t i = ps.syncs.count(); i-- > 0;)
    if (ps.syncs[i].device == device) ps.syncs.remove_at(i);
  for (uint64_t i = 0; i < ps.devices.count(); ++i)
    if (ps.devices[i].handle == device) {
      ps.devices.remove_at(i);
      break;
    }
}

}  // namespace process::win32k::dxg
