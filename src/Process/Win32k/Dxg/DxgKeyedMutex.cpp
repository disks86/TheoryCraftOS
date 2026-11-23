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

// Keyed mutexes: NtGdiDdDDICreateKeyedMutex(2), DestroyKeyedMutex, OpenKeyedMutex(2), OpenKeyedMutexFromNtHandle (DxgShares.cpp, with the resource it is shared with),
// AcquireKeyedMutex(2) and ReleaseKeyedMutex(2).
//
// A keyed mutex orders the use of a shared resource between devices and processes: whoever holds it uses the resource, and releases it with a key that says who may
// have it next (DXGI's IDXGIKeyedMutex::AcquireSync / ReleaseSync). It is a CPU object of this layer's (dxgk has none): a KeyedMutexState every handle of it refers to,
// in any process. A handle comes from its creation, from the global share handle it was made with (D3DKMTCreateKeyedMutex, or ...2 without NtSecuritySharing), or
// from the NT handle of the resource it is shared with (D3DKMTShareObjects of a resource and the mutex). Acquire waits - with the process's lock let go, as a fence's
// wait does - until the mutex is free and its key is the one asked for, or the time runs out (STATUS_TIMEOUT). A holder that goes - its process exits or it destroys its
// handle - abandons the mutex: every later Acquire answers STATUS_ABANDONED (WAIT_ABANDONED: the resource's contents are not known). The fence values and the private
// data of Release2 are passed on to the next Acquire (the queues of the devices here run their work when it is submitted, so a released resource has its contents).
namespace process::win32k::dxg {

namespace {

using namespace layout;
namespace status = dxgk::status;

constexpr uint32_t kStatusTimeout = 0x102u, kStatusAbandoned = 0x80u, kStatusInvalidDeviceState = 0xC0000184u, kStatusBufferTooSmall = 0xC0000023u;
constexpr uint32_t kMaxKeyedMutexes = 4096;

class Locked {
 public:
  explicit Locked(Session& s) : mutex_(s.state().lock) { mutex_.lock(); }
  ~Locked() { mutex_.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  kernel::Mutex& mutex_;
};

allocators::IAllocator& object_allocator(Session& s, Call& call) {
  allocators::IAllocator* system = call.allocator();
  return system ? *system : s.allocator();
}

SharedPointer<KeyedMutexState> make_state(Session& s, Call& call, uint64_t initial) {
  allocators::IAllocator& a = object_allocator(s, call);
  SharedPointer<KeyedMutexState> state = SharedPointer<KeyedMutexState>::make(a, a);
  if (state) state->key = initial;
  return state;
}

// The holder (pid, handle) lets go without a Release: the mutex is abandoned.
void abandon_if_held(KeyedMutexState& m, uint64_t pid, dxgk::Handle handle) {
  kernel::ScopedMutex g(m.lock);
  if (m.held && m.owner_pid == pid && m.owner_handle == handle) {
    m.held = false;
    m.abandoned = true;
  }
}

// --- Making and opening -----------------------------------------------------------------------------------------------------------------------------------

// A new handle for `state` written at arg + handle_offset (and the global share handle at arg + global_offset when `global`). With the process lock held.
uint32_t give(Session& s, Call& call, SharedPointer<KeyedMutexState> state, uint64_t arg, uint64_t handle_offset, bool global, uint64_t global_offset) {
  ProcessState& ps = s.state();
  const dxgk::Handle handle = add_keyed_mutex(ps, state);
  if (!handle) return status::kInsufficientResources;
  uint32_t id = 0;
  if (global) {
    id = add_global_keyed_mutex(s.pid(), state);
    if (!id) {
      for (uint64_t i = 0; i < ps.keyed_mutexes.count(); ++i)
        if (ps.keyed_mutexes[i].handle == handle) { ps.keyed_mutexes.remove_at(i); break; }
      return status::kNoMemory;
    }
    for (KeyedMutexRecord& r : ps.keyed_mutexes.view())
      if (r.handle == handle) r.global = id;
  }
  if (!put32(call, arg, handle_offset, handle) || (global && !put32(call, arg, global_offset, id))) {
    for (uint64_t i = 0; i < ps.keyed_mutexes.count(); ++i)
      if (ps.keyed_mutexes[i].handle == handle) { ps.keyed_mutexes.remove_at(i); break; }
    if (id) remove_global_keyed_mutex(id);
    return kStatusAccessViolation;
  }
  return status::kSuccess;
}

// D3DKMT_CREATEKEYEDMUTEX { InitialValue @0; hSharedHandle @8 (out); hKeyedMutex @12 (out) }
uint32_t create_keyed_mutex(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 16)) return load_failure(arg);
  SharedPointer<KeyedMutexState> state = make_state(s, call, b.u64(0));
  if (!state) return status::kNoMemory;
  Locked g(s);
  return give(s, call, util::move(state), arg, 12, true, 8);
}

// D3DKMT_CREATEKEYEDMUTEX2 { InitialValue @0; hSharedHandle @8 (out); hKeyedMutex @12 (out); pPrivateRuntimeData @16; PrivateRuntimeDataSize @24; Flags @28
// (bit 0 NtSecuritySharing: shared with its resource's NT handle, no global share handle) }
uint32_t create_keyed_mutex2(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 32)) return load_failure(arg);
  const uint64_t data = b.u64(16);
  const uint32_t size = b.u32(24), flags = b.u32(28);
  if (flags & ~uint32_t{1}) return kStatusInvalidParameter;
  if (size > kMaxPrivateData || (size && !data)) return kStatusInvalidParameter;
  SharedPointer<KeyedMutexState> state = make_state(s, call, b.u64(0));
  if (!state) return status::kNoMemory;
  if (!read_blob(call, data, size, &state->runtime_data)) return kStatusAccessViolation;
  state->nt_shareable = (flags & 1) != 0;
  Locked g(s);
  if (state->nt_shareable && !put32(call, arg, 8, 0)) return kStatusAccessViolation;
  return give(s, call, util::move(state), arg, 12, !(flags & 1), 8);
}

// D3DKMT_DESTROYKEYEDMUTEX { hKeyedMutex @0 }. A holder that destroys its handle abandons the mutex.
uint32_t destroy_keyed_mutex(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, 4)) return load_failure(arg);
  const dxgk::Handle handle = b.u32(0);
  SharedPointer<KeyedMutexState> gone;
  uint32_t global = 0;
  {
    Locked g(s);
    ProcessState& ps = s.state();
    for (uint64_t i = 0; i < ps.keyed_mutexes.count(); ++i)
      if (ps.keyed_mutexes[i].handle == handle) {
        gone = util::move(ps.keyed_mutexes[i].state);
        global = ps.keyed_mutexes[i].global;
        ps.keyed_mutexes.remove_at(i);
        break;
      }
  }
  if (!gone) return status::kInvalidHandle;
  abandon_if_held(*gone, s.pid(), handle);
  if (global) remove_global_keyed_mutex(global);
  return status::kSuccess;
}

// The private runtime data of the mutex into the caller's buffer at `data` (`size` bytes there); its size written at arg + size_offset when given.
uint32_t give_runtime_data(Call& call, const KeyedMutexState& m, uint64_t data, uint32_t size) {
  if (m.runtime_data.size() == 0) return status::kSuccess;
  if (size < m.runtime_data.size()) return kStatusBufferTooSmall;
  if (!data) return kStatusInvalidParameter;
  return call.write(data, m.runtime_data.view()) ? status::kSuccess : kStatusAccessViolation;
}

// D3DKMT_OPENKEYEDMUTEX { hSharedHandle @0; hKeyedMutex @4 (out) } and D3DKMT_OPENKEYEDMUTEX2 { ... pPrivateRuntimeData @8; PrivateRuntimeDataSize @16 }
uint32_t open_global(Session& s, Call& call, uint64_t arg, bool two) {
  UserBlock b;
  if (!b.load(call, arg, two ? 24 : 8)) return load_failure(arg);
  const uint32_t id = b.u32(0);
  if (id == 0) return kStatusInvalidParameter;
  SharedPointer<KeyedMutexState> state = find_global_keyed_mutex(id);
  if (!state) return status::kInvalidHandle;
  if (two) {
    if (const uint32_t st = give_runtime_data(call, *state, b.u64(8), b.u32(16))) return st;
    if (!put32(call, arg, 16, static_cast<uint32_t>(state->runtime_data.size()))) return kStatusAccessViolation;
  }
  Locked g(s);
  return give(s, call, util::move(state), arg, 4, false, 0);
}
uint32_t open_keyed_mutex(Session& s, Call& call, uint64_t arg) { return open_global(s, call, arg, false); }
uint32_t open_keyed_mutex2(Session& s, Call& call, uint64_t arg) { return open_global(s, call, arg, true); }

// --- Acquiring and releasing ----------------------------------------------------------------------------------------------------------------------------------

// The wait of an Acquire: *pTimeout in 100 ns units, negative from now, positive an absolute time (of the system's clock, which starts at boot here: taken from
// now as well), NULL forever.
uint32_t timeout_of(Call& call, uint64_t pointer, uint64_t* out_ns) {
  if (pointer == 0) { *out_ns = ~uint64_t{0}; return status::kSuccess; }
  uint64_t raw = 0;
  if (!call.read_u64(pointer, &raw)) return kStatusAccessViolation;
  const int64_t v = static_cast<int64_t>(raw);
  const uint64_t ticks = v < 0 ? uint64_t(0) - raw : raw;
  *out_ns = ticks > ~uint64_t{0} / 100 ? ~uint64_t{0} - 1 : ticks * 100;
  return status::kSuccess;
}

// D3DKMT_ACQUIREKEYEDMUTEX { hKeyedMutex @0; Key @8; pTimeout @16; FenceValue @24 (out) } and ...2 { ... pPrivateRuntimeData @32; PrivateRuntimeDataSize @40 }
uint32_t acquire(Session& s, Call& call, uint64_t arg, bool two) {
  UserBlock b;
  if (!b.load(call, arg, two ? 48 : 32)) return load_failure(arg);
  const dxgk::Handle handle = b.u32(0);
  const uint64_t key = b.u64(8);
  const uint64_t data = two ? b.u64(32) : 0;
  const uint32_t size = two ? b.u32(40) : 0;
  uint64_t timeout_ns = 0;
  if (const uint32_t st = timeout_of(call, b.u64(16), &timeout_ns)) return st;
  SharedPointer<KeyedMutexState> m;
  {
    Locked g(s);
    m = keyed_mutex_of(s.state(), handle);
  }
  if (!m) return status::kInvalidHandle;
  Process& p = call.process();
  const uint64_t started = p.host().monotonic_ns();
  uint64_t nap = 20000;      // 20 us, doubling up to 1 ms
  for (;;) {
    uint64_t fence = 0;
    ByteBuffer released(s.allocator());
    uint32_t outcome = kStatusTimeout;
    {
      kernel::ScopedMutex g(m->lock);
      if (m->abandoned) outcome = kStatusAbandoned;
      else if (m->held && m->owner_pid == s.pid() && m->owner_handle == handle) outcome = kStatusInvalidDeviceState;      // (held by this handle already)
      else if (!m->held && m->key == key) {
        m->held = true;
        m->owner_pid = s.pid();
        m->owner_handle = handle;
        fence = m->fence_value;
        if (two && !released.append(m->release_data.view())) { m->held = false; outcome = status::kNoMemory; }
        else outcome = status::kSuccess;
      }
    }
    if (outcome == status::kSuccess) {
      bool wrote = put64(call, arg, 24, fence);
      if (wrote && two && size && data) {
        // (what the last Release2 gave, as much as the buffer takes; zeros if there was none)
        ByteBuffer out(s.allocator());
        wrote = out.put_zeros(size);
        wrote = wrote && out.writable_view().copy_from(released.view().first(released.size() < size ? released.size() : size));
        wrote = wrote && call.write(data, out.view());
      }
      if (!wrote) {      // (not delivered: it is let go again)
        kernel::ScopedMutex g(m->lock);
        m->held = false;
        return kStatusAccessViolation;
      }
      return status::kSuccess;
    }
    if (outcome != kStatusTimeout) return outcome;
    if (timeout_ns == 0) return kStatusTimeout;
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) return kStatusTimeout;
      left = timeout_ns - used;
    }
    IProcessRuntime* runtime = p.runtime();
    if (p.exited() || (runtime && runtime->interrupted())) return nt::kThreadIsTerminating;
    if (runtime) runtime->unlock();      // (the holder may be another thread of this process: it must get into its Release)
    p.host().sleep_ns(nap < left ? nap : left);
    if (runtime) runtime->lock();
    if (nap < 1000000) nap *= 2;
  }
}
uint32_t acquire_keyed_mutex(Session& s, Call& call, uint64_t arg) { return acquire(s, call, arg, false); }
uint32_t acquire_keyed_mutex2(Session& s, Call& call, uint64_t arg) { return acquire(s, call, arg, true); }

// D3DKMT_RELEASEKEYEDMUTEX { hKeyedMutex @0; Key @8; FenceValue @16 } and ...2 { ... pPrivateRuntimeData @24; PrivateRuntimeDataSize @32 }. Only its holder releases it.
uint32_t release(Session& s, Call& call, uint64_t arg, bool two) {
  UserBlock b;
  if (!b.load(call, arg, two ? 40 : 24)) return load_failure(arg);
  const dxgk::Handle handle = b.u32(0);
  const uint64_t key = b.u64(8), fence = b.u64(16);
  const uint64_t data = two ? b.u64(24) : 0;
  const uint32_t size = two ? b.u32(32) : 0;
  if (size > kMaxPrivateData || (size && !data)) return kStatusInvalidParameter;
  ByteBuffer given(s.allocator());
  if (size && !read_blob(call, data, size, &given)) return kStatusAccessViolation;
  SharedPointer<KeyedMutexState> m;
  {
    Locked g(s);
    m = keyed_mutex_of(s.state(), handle);
  }
  if (!m) return status::kInvalidHandle;
  kernel::ScopedMutex g(m->lock);
  if (!m->held || m->owner_pid != s.pid() || m->owner_handle != handle) return kStatusInvalidDeviceState;
  if (two) {
    m->release_data.clear();
    if (!m->release_data.append(given.view())) return status::kNoMemory;
  }
  m->key = key;
  m->fence_value = fence;
  m->held = false;
  m->owner_pid = 0;
  m->owner_handle = 0;
  return status::kSuccess;
}
uint32_t release_keyed_mutex(Session& s, Call& call, uint64_t arg) { return release(s, call, arg, false); }
uint32_t release_keyed_mutex2(Session& s, Call& call, uint64_t arg) { return release(s, call, arg, true); }

}  // namespace

dxgk::Handle add_keyed_mutex(ProcessState& ps, SharedPointer<KeyedMutexState> state) {
  if (!state || ps.keyed_mutexes.count() >= kMaxKeyedMutexes || ps.next_keyed_mutex >= 0x0FFFFFFFu) return 0;
  KeyedMutexRecord r;
  r.handle = kKeyedMutexHandleBase | ps.next_keyed_mutex++;
  r.state = util::move(state);
  const dxgk::Handle handle = r.handle;
  return ps.keyed_mutexes.push_back(util::move(r)) ? handle : 0;
}

SharedPointer<KeyedMutexState> keyed_mutex_of(ProcessState& ps, dxgk::Handle handle) {
  for (KeyedMutexRecord& r : ps.keyed_mutexes.view())
    if (r.handle == handle) return r.state;
  return SharedPointer<KeyedMutexState>();
}

void keyed_mutexes_process_exited(ProcessState& ps, uint64_t pid) {
  for (KeyedMutexRecord& r : ps.keyed_mutexes.view())
    if (r.state) abandon_if_held(*r.state, pid, r.handle);
}

void register_keyed_mutexes() {
  using N = WindowsSyscallNumber;
  add<create_keyed_mutex>(N::NtGdiDdDDICreateKeyedMutex);
  add<create_keyed_mutex2>(N::NtGdiDdDDICreateKeyedMutex2);
  add<destroy_keyed_mutex>(N::NtGdiDdDDIDestroyKeyedMutex);
  add<open_keyed_mutex>(N::NtGdiDdDDIOpenKeyedMutex);
  add<open_keyed_mutex2>(N::NtGdiDdDDIOpenKeyedMutex2);
  add<acquire_keyed_mutex>(N::NtGdiDdDDIAcquireKeyedMutex);
  add<acquire_keyed_mutex2>(N::NtGdiDdDDIAcquireKeyedMutex2);
  add<release_keyed_mutex>(N::NtGdiDdDDIReleaseKeyedMutex);
  add<release_keyed_mutex2>(N::NtGdiDdDDIReleaseKeyedMutex2);
}

}  // namespace process::win32k::dxg
