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

// Synchronization objects: NtGdiDdDDICreateSynchronizationObject(2), DestroySynchronizationObject, SignalSynchronizationObject, WaitForSynchronizationObject,
// SignalSynchronizationObjectFromCpu / FromGpu, WaitForSynchronizationObjectFromCpu / FromGpu.
//
// dxgk::Kernel has two kinds of object: a fence (a 64-bit counter that only goes up) and a semaphore (signals up to a limit; a wait takes one). D3DKMT's mutex is a semaphore with
// a limit of one. CPU notification objects (they need an NT event) and monitored fences (they need the fence value mapped into the process, and a GPU address) are not
// available: the kernel says STATUS_NOT_SUPPORTED, as an adapter of WDDM 1.3 does.
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

constexpr uint32_t kTypeMutex = 1, kTypeSemaphore = 2, kTypeFence = 3, kTypeCpuNotification = 4, kTypeMonitoredFence = 5, kTypePeriodicMonitoredFence = 6;
constexpr uint32_t kFlagShared = 1u << 0, kFlagNtSecuritySharing = 1u << 1, kFlagCrossAdapter = 1u << 2;

// Makes the object and notes it; writes the handle at `handle_offset`.
uint32_t create_sync(Session& s, Call& call, uint64_t arg, dxgk::Handle device, dxgk::SyncType type, uint64_t initial, uint64_t limit, uint64_t handle_offset) {
  Locked g(s);
  ProcessState& ps = s.state();
  if (!ps.find_device(device)) return status::kInvalidHandle;
  dxgk::Handle sync = 0;
  uint32_t st = s.kernel().create_sync(s.pid(), device, type, initial, limit, &sync);
  if (st != status::kSuccess) return st;
  SyncRecord rec;
  rec.handle = sync;
  rec.device = device;
  rec.type = type;
  if (!ps.syncs.push_back(rec)) {
    s.kernel().destroy_sync(s.pid(), sync);
    return status::kNoMemory;
  }
  if (!put32(call, arg, handle_offset, sync)) {
    s.kernel().destroy_sync(s.pid(), sync);
    ps.syncs.remove_at(ps.syncs.count() - 1);
    return kStatusAccessViolation;
  }
  return status::kSuccess;
}

uint32_t create_synchronization_object(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_create_sync)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t type = b.u32(4);
  if (device == 0) return kStatusInvalidParameter;
  switch (type) {
    case kTypeMutex:
      return create_sync(s, call, arg, device, dxgk::SyncType::Semaphore, b.u32(8) ? 1 : 0, 1, 72);
    case kTypeSemaphore: {
      const uint32_t max = b.u32(8), initial = b.u32(12);
      if (max == 0 || initial > max) return kStatusInvalidParameter;
      return create_sync(s, call, arg, device, dxgk::SyncType::Semaphore, initial, max, 72);
    }
    case kTypeFence:
      return create_sync(s, call, arg, device, dxgk::SyncType::Fence, 0, 0, 72);
    case kTypeCpuNotification:
      return kStatusNotSupported;
    default:
      return kStatusInvalidParameter;
  }
}

uint32_t create_synchronization_object2(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_create_sync2)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t type = b.u32(8);
  const uint32_t flags = b.u32(12);
  if (device == 0) return kStatusInvalidParameter;
  if (type == kTypeCpuNotification || type == kTypeMonitoredFence || type == kTypePeriodicMonitoredFence) return kStatusNotSupported;
  if (flags & (kFlagShared | kFlagNtSecuritySharing | kFlagCrossAdapter)) return kStatusNotSupported;      // (no sharing of synchronization objects between processes)
  switch (type) {
    case kTypeMutex:
      return create_sync(s, call, arg, device, dxgk::SyncType::Semaphore, b.u32(16) ? 1 : 0, 1, 88);
    case kTypeSemaphore: {
      const uint32_t max = b.u32(16), initial = b.u32(20);
      if (max == 0 || initial > max) return kStatusInvalidParameter;
      return create_sync(s, call, arg, device, dxgk::SyncType::Semaphore, initial, max, 88);
    }
    case kTypeFence:
      return create_sync(s, call, arg, device, dxgk::SyncType::Fence, b.u64(16), 0, 88);
    default:
      return kStatusInvalidParameter;
  }
}

uint32_t destroy_synchronization_object(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_handle)) return load_failure(arg);
  const dxgk::Handle sync = b.u32(0);
  if (sync == 0) return kStatusInvalidParameter;
  Locked g(s);
  const uint32_t st = s.kernel().destroy_sync(s.pid(), sync);
  if (st == status::kSuccess)
    for (uint64_t i = 0; i < s.state().syncs.count(); ++i)
      if (s.state().syncs[i].handle == sync) {
        s.state().syncs.remove_at(i);
        break;
      }
  return st;
}

// --- From the CPU ---------------------------------------------------------------------------------------------------------------------------------------------

bool read_handles_and_values(Session& s, Call& call, uint64_t handles_ptr, uint64_t values_ptr, uint32_t count, List<dxgk::Handle>* handles, List<uint64_t>* values) {
  (void)s;
  return read_u32_array(call, handles_ptr, count, handles) && read_u64_array(call, values_ptr, count, values);
}

uint32_t signal_from_cpu(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_signal_from_cpu)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t count = b.u32(4);
  const uint64_t handles_ptr = b.u64(8), values_ptr = b.u64(16);
  if (device == 0 || count == 0 || count > kMaxObjectsPerSync || handles_ptr == 0 || values_ptr == 0) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  List<uint64_t> values(s.allocator());
  if (!read_handles_and_values(s, call, handles_ptr, values_ptr, count, &handles, &values)) return kStatusAccessViolation;
  return s.kernel().signal_sync_cpu(s.pid(), ArrayView<const dxgk::Handle>(handles.view()), ArrayView<const uint64_t>(values.view()));
}

uint32_t wait_from_cpu(Session& s, Call& call, uint64_t arg) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_wait_from_cpu)) return load_failure(arg);
  const dxgk::Handle device = b.u32(0);
  const uint32_t count = b.u32(4);
  const uint64_t handles_ptr = b.u64(8), values_ptr = b.u64(16), event = b.u64(24);
  const uint32_t flags = b.u32(32);
  if (device == 0 || count == 0 || count > kMaxObjectsPerSync || handles_ptr == 0 || values_ptr == 0) return kStatusInvalidParameter;
  if (event != 0) return kStatusNotSupported;      // (completion through an NT event: the event object is not reachable from here)
  List<dxgk::Handle> handles(s.allocator());
  List<uint64_t> values(s.allocator());
  if (!read_handles_and_values(s, call, handles_ptr, values_ptr, count, &handles, &values)) return kStatusAccessViolation;
  // Without the process lock: other threads of the process may be the ones that signal.
  return wait_for_syncs(s, call, ArrayView<const dxgk::Handle>(handles.view()), ArrayView<const uint64_t>(values.view()), (flags & 1) == 0, ~uint64_t{0});
}

// --- On the GPU timeline ------------------------------------------------------------------------------------------------------------------------------

// The type of a sync object of the process; false if it is not one.
bool sync_type(Session& s, dxgk::Handle handle, dxgk::SyncType* type) {
  Locked g(s);
  SyncRecord* rec = s.state().find_sync(handle);
  if (!rec) return false;
  *type = rec->type;
  return true;
}

// Queues a signal (or wait) of every object on the context. `values` may be null: the objects must then be semaphores (no value) - the legacy interface. All the handles are
// checked before anything is queued.
uint32_t queue_objects(Session& s, dxgk::Handle context, const List<dxgk::Handle>& handles, const List<uint64_t>* values, uint64_t single_value, bool use_single, bool signal) {
  for (dxgk::Handle h : handles.view()) {
    dxgk::SyncType type;
    if (!sync_type(s, h, &type)) return status::kInvalidHandle;
    if (!values && !use_single && type == dxgk::SyncType::Fence) return kStatusInvalidParameter;      // (a fence needs the value the legacy interface has no field for)
  }
  uint32_t st = status::kSuccess;
  for (uint64_t i = 0; i < handles.count() && st == status::kSuccess; ++i) {
    const uint64_t value = values ? (*values)[i] : use_single ? single_value : 0;
    st = signal ? s.kernel().signal_sync_gpu(s.pid(), context, handles[i], value) : s.kernel().wait_sync_gpu(s.pid(), context, handles[i], value);
  }
  return st;
}

uint32_t gpu_inline(Session& s, Call& call, uint64_t arg, bool signal, bool with_value, uint64_t value_offset, uint64_t size) {
  UserBlock b;
  if (!b.load(call, arg, size)) return load_failure(arg);
  const dxgk::Handle context = b.u32(0);
  const uint32_t count = b.u32(4);
  if (context == 0 || count == 0 || count > kMaxObjectsPerSync) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  if (!read_u32_array(call, element_address(arg, 8, 1), count, &handles)) return kStatusAccessViolation;
  return queue_objects(s, context, handles, nullptr, with_value ? b.u64(value_offset) : 0, with_value, signal);
}

uint32_t signal_object(Session& s, Call& call, uint64_t arg) { return gpu_inline(s, call, arg, true, false, 0, sizeof_signal_sync); }
uint32_t wait_object(Session& s, Call& call, uint64_t arg) { return gpu_inline(s, call, arg, false, false, 0, sizeof_wait_sync); }

// FromGpu: the handle array and the array of fence values are in the caller's memory.
uint32_t gpu_arrays(Session& s, Call& call, uint64_t arg, bool signal) {
  UserBlock b;
  if (!b.load(call, arg, sizeof_signal_from_gpu)) return load_failure(arg);
  const dxgk::Handle context = b.u32(0);
  const uint32_t count = b.u32(4);
  const uint64_t handles_ptr = b.u64(8), values_ptr = b.u64(16);
  if (context == 0 || count == 0 || count > kMaxObjectsPerSync || handles_ptr == 0 || values_ptr == 0) return kStatusInvalidParameter;
  List<dxgk::Handle> handles(s.allocator());
  List<uint64_t> values(s.allocator());
  if (!read_handles_and_values(s, call, handles_ptr, values_ptr, count, &handles, &values)) return kStatusAccessViolation;
  return queue_objects(s, context, handles, &values, 0, false, signal);
}

uint32_t signal_from_gpu(Session& s, Call& call, uint64_t arg) { return gpu_arrays(s, call, arg, true); }
uint32_t wait_from_gpu(Session& s, Call& call, uint64_t arg) { return gpu_arrays(s, call, arg, false); }

}  // namespace

void register_sync() {
  using N = WindowsSyscallNumber;
  add<create_synchronization_object>(N::NtGdiDdDDICreateSynchronizationObject);
  add<create_synchronization_object2>(N::NtGdiDdDDICreateSynchronizationObject2);
  add<destroy_synchronization_object>(N::NtGdiDdDDIDestroySynchronizationObject);
  add<signal_from_cpu>(N::NtGdiDdDDISignalSynchronizationObjectFromCpu);
  add<wait_from_cpu>(N::NtGdiDdDDIWaitForSynchronizationObjectFromCpu);
  add<signal_object>(N::NtGdiDdDDISignalSynchronizationObject);
  add<wait_object>(N::NtGdiDdDDIWaitForSynchronizationObject);
  add<signal_from_gpu>(N::NtGdiDdDDISignalSynchronizationObjectFromGpu);
  add<wait_from_gpu>(N::NtGdiDdDDIWaitForSynchronizationObjectFromGpu);
}

}  // namespace process::win32k::dxg
