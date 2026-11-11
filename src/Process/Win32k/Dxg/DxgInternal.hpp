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
#include <stdint.h>
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Process/Win32k/Win32k.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

// The Direct3D services of win32u.dll (NtGdiDdDDI*, NtDxgk*): the thunks of the D3DKMT interface of gdi32.dll / d3d9 / dxgi. Each handler reads the user-mode structure of
// the call (the Windows x64 layout, see DxgLayouts.hpp), maps it onto one or more calls of the kernel-mode driver (dxgk::Kernel, one process id per caller) and writes the
// results back. docs/win32k/Dxg.md lists what is implemented and how the pieces that dxgk::Kernel has no counterpart for (command buffers, CPU views of locked
// allocations, resources and global shares) are made.
namespace process::win32k::dxg {

// --- Status values beyond win32k's.
namespace nt {
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kThreadIsTerminating = 0xC000004Bu;
constexpr uint32_t kNotSupported = 0xC00000BBu;
constexpr uint32_t kNoSuchDevice = 0xC000000Eu;
}  // namespace nt

// --- Limits.
constexpr uint32_t kMaxPrivateData = 64 * 1024;       // each blob of private driver / runtime data a process hands the kernel
constexpr uint32_t kMaxEscapeData = 4u << 20;         // ... but the data of a driver's escape: a shader's code (dxgk::D3d9Program::kMaxDxbcBytes)
constexpr uint32_t kMaxAllocationsPerCall = 256;      // NumAllocations of one CreateAllocation / list of one DestroyAllocation
constexpr uint32_t kMaxObjectsPerSync = 32;           // D3DDDI_MAX_OBJECT_SIGNALED / _WAITED_ON
constexpr uint32_t kMaxAdaptersPerCall = 16;          // MAX_ENUM_ADAPTERS
constexpr uint64_t kCommandBufferBytes = 64 * 1024;   // per context (see Contexts below)
constexpr uint32_t kAllocationListEntries = 1024;     // D3DDDI_ALLOCATIONLIST entries (8 bytes)
constexpr uint32_t kPatchListEntries = 1024;          // D3DDDI_PATCHLOCATIONLIST entries (24 bytes)
constexpr uint32_t kMaxResourcesPerProcess = 4096;

// A copy of the head of a user-mode structure, read at once (the caller may change its memory while the call runs: every field is read exactly once, from here).
class UserBlock {
 public:
  static constexpr uint64_t kCapacity = 512;
  // False if `size` is over the capacity or the memory is not readable.
  bool load(Call& call, uint64_t address, uint64_t size) {
    size_ = 0;
    if (size > kCapacity) return false;
    if (!call.read(address, ByteView(bytes_, size))) return false;
    size_ = size;
    return true;
  }
  uint32_t u32(uint64_t offset) const { return static_cast<uint32_t>(read(offset, 4)); }
  uint64_t u64(uint64_t offset) const { return read(offset, 8); }
  int32_t i32(uint64_t offset) const { return static_cast<int32_t>(u32(offset)); }

 private:
  uint64_t read(uint64_t offset, uint64_t width) const {
    if (offset > size_ || size_ - offset < width) return 0;
    uint64_t v = 0;
    for (uint64_t i = 0; i < width; ++i) v |= uint64_t{bytes_[offset + i]} << (8 * i);
    return v;
  }
  uint8_t bytes_[kCapacity] = {};
  uint64_t size_ = 0;
};

// The address of element `index` of an array of the caller's (0 if the arithmetic wraps: every access to address 0 fails).
inline uint64_t element_address(uint64_t base, uint64_t index, uint64_t stride) {
  if (stride != 0 && index > ~uint64_t{0} / stride) return 0;
  const uint64_t offset = index * stride;
  return offset > ~uint64_t{0} - base ? 0 : base + offset;
}

// Writes one field of a structure of the caller's (address + offset must not wrap).
bool put32(Call& call, uint64_t address, uint64_t offset, uint32_t value);
bool put64(Call& call, uint64_t address, uint64_t offset, uint64_t value);
// `count` 32-bit / 64-bit values of an array of the caller's, copied in (count is checked by the caller against its limit). False if unreadable.
bool read_u32_array(Call& call, uint64_t address, uint32_t count, List<uint32_t>* out);
bool read_u64_array(Call& call, uint64_t address, uint32_t count, List<uint64_t>* out);
// `size` bytes of the caller's memory (at most `limit`) appended to the buffer.
bool read_blob(Call& call, uint64_t address, uint32_t size, ByteBuffer* out, uint32_t limit = kMaxPrivateData);

// --- What a process has made through this layer that dxgk::Kernel has no counterpart for.

// One allocation of the process (dxgk's handle) and where it came from.
struct AllocationRecord {
  dxgk::Handle handle = 0;
  dxgk::Handle device = 0;
  uint32_t resource = 0;      // the resource it belongs to (0: none)
};

// A CPU view of a locked allocation: the kernel's backing memory is not in the process's address space, so Lock maps a region of the process's own memory with a copy, and
// Unlock copies it back (unless the lock was read-only) before the kernel unlocks.
struct LockRecord {
  dxgk::Handle allocation = 0;
  dxgk::Handle device = 0;
  uint64_t user_base = 0, user_size = 0;
  bool read_only = false;
  ByteView data;              // the kernel's view, valid until the kernel's unlock
};

// A context's command buffer, allocation list and patch location list: mapped in the process at CreateContext (WDDM 1.x: the user-mode driver fills them and Render names the
// part to run).
struct ContextRecord {
  dxgk::Handle context = 0;
  dxgk::Handle device = 0;
  uint64_t region_base = 0, region_size = 0;      // 0: no buffers (virtual contexts)
  int32_t priority = 0, in_process_priority = 0;
};

// A synchronization object of the process (dxgk's handle) and what kind it is.
struct SyncRecord {
  dxgk::Handle handle = 0;
  dxgk::Handle device = 0;
  dxgk::SyncType type = dxgk::SyncType::Fence;
};

struct ResourceRecord {
  explicit ResourceRecord(allocators::IAllocator& allocator) : allocations(allocator), runtime_data(allocator), driver_data(allocator), allocation_data(allocator) {}
  uint32_t id = 0;
  dxgk::Handle device = 0;
  uint32_t share = 0;                         // the global share handle (0: not shared)
  List<dxgk::Handle> allocations;
  ByteBuffer runtime_data, driver_data;       // PrivateRuntimeData / PrivateDriverData of the creation
  List<UniquePointer<ByteBuffer>> allocation_data;   // per allocation, in the order of `allocations`
};

// An adapter handle of the process and the adapter's number (the handle may be closed while devices made from it live on).
struct AdapterRecord {
  dxgk::Handle handle = 0;
  uint32_t index = 0;
};

struct DeviceRecord {
  dxgk::Handle handle = 0;
  uint32_t adapter_index = 0;
  uint32_t queued_present_limit = 3;
};

class ProcessState {
 public:
  ProcessState(allocators::IAllocator& allocator, uint64_t pid, dxgk::Kernel* kernel)
      : adapters(allocator), allocations(allocator), locks(allocator), contexts(allocator), resources(allocator), devices(allocator), syncs(allocator), pid_(pid), kernel_(kernel), allocator_(allocator) {}
  uint64_t pid() const { return pid_; }
  dxgk::Kernel* kernel() const { return kernel_; }
  allocators::IAllocator& allocator() { return allocator_; }

  kernel::Mutex lock;       // guards everything below
  List<AdapterRecord> adapters;
  List<AllocationRecord> allocations;
  List<UniquePointer<LockRecord>> locks;
  List<ContextRecord> contexts;
  List<UniquePointer<ResourceRecord>> resources;
  List<DeviceRecord> devices;
  List<SyncRecord> syncs;
  uint32_t next_resource = 1;

  AdapterRecord* find_adapter(dxgk::Handle handle);
  AllocationRecord* find_allocation(dxgk::Handle handle);
  ContextRecord* find_context(dxgk::Handle handle);
  ResourceRecord* find_resource(uint32_t id);
  DeviceRecord* find_device(dxgk::Handle handle);
  SyncRecord* find_sync(dxgk::Handle handle);
  LockRecord* find_lock(dxgk::Handle allocation);
  void remove_allocation(dxgk::Handle handle);

 private:
  uint64_t pid_;
  dxgk::Kernel* kernel_;
  allocators::IAllocator& allocator_;
};

// A resource that other processes can open (the global share handle of D3DKMT_CREATEALLOCATION.hGlobalShare). The kernel's share tokens are per allocation.
struct ShareRecord {
  explicit ShareRecord(allocators::IAllocator& allocator) : tokens(allocator), runtime_data(allocator), driver_data(allocator), allocation_data(allocator) {}
  uint32_t id = 0;
  uint64_t owner = 0;
  List<uint32_t> tokens;
  ByteBuffer runtime_data, driver_data;
  List<UniquePointer<ByteBuffer>> allocation_data;
};

// Who owns the video present source of an adapter (D3DKMTSetVidPnSourceOwner). The display has one source per adapter.
enum class SourceOwner : uint32_t { None, Exclusive, Emulated };
// D3DKMT_VIDPNSOURCEOWNER_TYPE
constexpr uint32_t kOwnerUnowned = 0, kOwnerShared = 1, kOwnerExclusive = 2, kOwnerExclusiveGdi = 3, kOwnerEmulated = 4;
constexpr uint32_t kStatusVidPnSourceInUse = 0xC01E0342u, kStatusPresentOccluded = 0xC01E0006u;
// Sets the owner of the source of adapter `adapter` for a device of a process (statuses as Windows has them, see docs/win32k/Dxg.md).
uint32_t set_source_owner(uint32_t adapter, uint64_t pid, dxgk::Handle device, uint32_t type);
// Releases what a device (0: every device) of a process owns.
void release_source_owner(uint64_t pid, dxgk::Handle device);
bool source_is_exclusive(uint32_t adapter);

// One call: the driver, the caller's state, and the failure to answer with when there is none.
class Session {
 public:
  explicit Session(Call& call);
  bool ok() const { return failure_ == 0; }
  uint32_t failure() const { return failure_; }
  dxgk::Kernel& kernel() { return *kernel_; }
  ProcessState& state() { return *state_; }
  uint64_t pid() const { return pid_; }
  allocators::IAllocator& allocator() { return state_->allocator(); }

 private:
  uint32_t failure_ = 0;
  dxgk::Kernel* kernel_ = nullptr;
  SharedPointer<ProcessState> state_;
  uint64_t pid_ = 0;
};

// The registry of the layer: processes that used the driver and the global shares.
SharedPointer<ProcessState> state_for(Call& call, dxgk::Kernel* kernel, uint32_t* status);
// Drops the registry (host tests: the allocator it was made from goes away with the test's system).
void shutdown_for_tests();
SharedPointer<ProcessState> find_state(uint64_t pid);
void process_exited(uint64_t pid);
// Shares. `record` is adopted. open_share copies the data of share `id` out under the registry's lock.
uint32_t add_share(UniquePointer<ShareRecord> record);       // returns the id (0: out of memory / ids)
bool with_share(uint32_t id, void (*use)(void* context, ShareRecord& share), void* context);
void remove_share(dxgk::Kernel& kernel, uint64_t owner, uint32_t id);

// Waiting in the calling thread without the system lock: polls the driver (timeout 0) and sleeps in the host between tries. `timeout_ns` ~0: forever.
uint32_t wait_for_syncs(Session& s, Call& call, ArrayView<const dxgk::Handle> handles, ArrayView<const uint64_t> values, bool wait_all, uint64_t timeout_ns);

// The memory of the process's address space for buffers made here (null: the process has none attached).
bool map_user_region(Call& call, uint64_t size, uint64_t* base);
void unmap_user_region(Call& call, uint64_t base, uint64_t size);

// Drops the layer's records of `device` and unmaps what was mapped for them (before the kernel destroys the device).
void forget_device(Session& s, Call& call, dxgk::Handle device);
// Unlocks (with write-back unless `discard`) and forgets one lock record.
void release_lock(Session& s, Call& call, dxgk::Handle allocation, bool write_back);

// --- Registration (one function per file).
void register_adapters();
void register_allocations();
void register_sync();
void register_work();
void register_unsupported();

// The D3DKMT thunks all take one pointer: this wraps a function of (Session, Call, address) in a handler that answers STATUS_NO_SUCH_DEVICE without a kernel.
using ThunkFn = uint32_t (*)(Session&, Call&, uint64_t arg0);
template <ThunkFn F>
uint64_t thunk(Call& call) {
  Session s(call);
  if (!s.ok()) return s.failure();
  return F(s, call, call.arg(0));
}
// Registers a service of this layer (unlocked: the Direct3D kernel has its own locks) and notes it as one of the D3DKMT services, which a Linux process may call through
// /dev/dxg (src/Process/DxgDevice.h).
void register_service(WindowsSyscallNumber number, Handler handler);
// True if `service` is a service registered with register_service.
bool is_service(uint32_t service);
template <ThunkFn F>
inline void add(WindowsSyscallNumber number) {
  register_service(number, &thunk<F>);
}

// A user pointer that is null is STATUS_INVALID_PARAMETER, one that cannot be read STATUS_ACCESS_VIOLATION.
inline uint32_t load_failure(uint64_t address) { return address == 0 ? kStatusInvalidParameter : kStatusAccessViolation; }

}  // namespace process::win32k::dxg
