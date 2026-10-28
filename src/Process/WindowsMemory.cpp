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
#include "Process/WindowsMemory.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"
#include "Process/WindowsThreads.hpp"
#include "Process/WindowsObjects.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kStatusSuccess = 0, kStatusInvalidHandle = 0xC0000008u, kStatusInvalidParameter = 0xC000000Du, kStatusAccessViolation = 0xC0000005u,
                   kStatusNoMemory = 0xC0000017u, kStatusConflictingAddresses = 0xC0000018u, kStatusUnableToFreeVm = 0xC000001Au, kStatusInvalidPageProtection = 0xC0000045u,
                   kStatusPartialCopy = 0x8000000Du, kStatusInvalidCid = 0xC000000Bu, kStatusInfoLengthMismatch = 0xC0000004u, kStatusInvalidInfoClass = 0xC0000003u,
                   kStatusProcessIsTerminating = 0xC000010Au, kStatusNotImplemented = 0xC0000002u;
constexpr uint64_t kCurrentProcess = ~uint64_t{0};      // NtCurrentProcess()
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000;
constexpr uint32_t kPageNoAccess = 0x01, kPageReadOnly = 0x02, kPageReadWrite = 0x04, kPageWriteCopy = 0x08, kPageExecute = 0x10, kPageExecuteRead = 0x20,
                   kPageExecuteReadWrite = 0x40, kPageExecuteWriteCopy = 0x80;
constexpr uint64_t kAllocationGranularity = 0x10000;
constexpr uint64_t kChunk = 2048;      // how much of another process's memory is moved at a time

// PAGE_* -> our permission bits. Modifier flags (guard, no-cache, write-combine) are not supported.
bool page_protection(uint32_t page, uint8_t* out) {
  if (page & ~uint32_t{0x7FF}) return false;
  switch (page & 0xFF) {
    case kPageNoAccess: *out = 0; return true;
    case kPageReadOnly: *out = kProtRead; return true;
    case kPageReadWrite: *out = kProtRead | kProtWrite; return true;
    case kPageWriteCopy: *out = kProtRead; return true;      // (the first write copies the page: AddressSpace::break_copy_on_write)
    case kPageExecute: *out = kProtExecute; return true;
    case kPageExecuteRead: *out = kProtRead | kProtExecute; return true;
    case kPageExecuteReadWrite: *out = kProtRead | kProtWrite | kProtExecute; return true;
    case kPageExecuteWriteCopy: *out = kProtRead | kProtExecute; return true;
    default: return false;
  }
}
uint32_t page_constant(uint8_t protection) {
  switch (protection) {
    case 0: return kPageNoAccess;
    case kProtRead: return kPageReadOnly;
    case kProtRead | kProtWrite: return kPageReadWrite;
    case kProtWrite: return kPageReadWrite;
    case kProtExecute: return kPageExecute;
    case kProtRead | kProtExecute: return kPageExecuteRead;
    default: return kPageExecuteReadWrite;
  }
}

// The PAGE_* constant of the page, as the program asked for it (copy-on-write and the like); pages the loader mapped have only their permissions to go by.
uint32_t effective_protect(const AddressSpace& space, uint64_t address) {
  uint8_t prot = 0;
  PageAttributes attributes;
  if (!space.protection_at(address, &prot)) return 0;
  if (space.attributes_at(address, &attributes) && attributes.protect != 0) return attributes.protect;
  return page_constant(prot);
}

constexpr uint32_t kStatusAccessDenied = 0xC0000022u;

// ---- the operations, on one process

void set_private_attributes(AddressSpace& space, uint64_t base, uint64_t size, uint32_t protect, uint32_t type) {
  PageAttributes attributes;
  attributes.allocation_protect = protect;
  attributes.reserved = !(type & kMemCommit);
  attributes.protect = attributes.reserved ? kPageNoAccess : protect;
  space.set_attributes(base, size, attributes);
}

uint32_t vm_allocate(Process& target, uint64_t* base_io, uint64_t* size_io, uint32_t type, uint32_t protect) {
  if (type == (kMemReserve | 0x400000)) type = kMemReserve;      // MEM_PHYSICAL (address windowing): a plain reservation
  AddressSpace* space = target.address_space();
  uint64_t base = *base_io, size = *size_io;
  uint8_t prot = 0;
  constexpr uint32_t kMemReset = 0x80000, kMemResetUndo = 0x1000000;
  if (type & (kMemReset | kMemResetUndo)) {      // the contents of the pages may be dropped; nothing here keeps them, so the pages only have to exist
    if (!space || base == 0 || size == 0 || (type & ~uint32_t{kMemReset | kMemResetUndo})) return kStatusInvalidParameter;
    uint64_t reset_size = 0;
    const uint64_t reset_base = AddressSpace::round_down(base);
    if (!AddressSpace::round_up(size + (base - reset_base), &reset_size)) return kStatusInvalidParameter;
    if (!space->check(reset_base, reset_size, 0)) return kStatusConflictingAddresses;
    *base_io = reset_base;
    *size_io = reset_size;
    return kStatusSuccess;
  }
  if (!(type & (kMemCommit | kMemReserve)) || (type & ~uint32_t{kMemCommit | kMemReserve | 0x100000 /*MEM_TOP_DOWN: the search direction is ours to choose*/})) return kStatusInvalidParameter;
  if (!page_protection(protect, &prot)) return kStatusInvalidPageProtection;
  if (!space) return kStatusNoMemory;
  if (size == 0) return kStatusInvalidParameter;
  if ((protect & 0xFF) == kPageWriteCopy || (protect & 0xFF) == kPageExecuteWriteCopy) {      // private memory is not copy-on-write; only a view can be (committing into a view is checked below)
    PageAttributes existing;
    if (base == 0 || !space->attributes_at(AddressSpace::round_down(base), &existing) || (existing.type != PageAttributes::Mapped && existing.type != PageAttributes::Image)) return kStatusInvalidPageProtection;
  }
  if (!(type & kMemCommit)) prot = 0;      // reserved memory cannot be touched until it is committed
  uint64_t rounded = 0;
  if (!AddressSpace::round_up(size, &rounded)) return kStatusInvalidParameter;
  if (base != 0) {
    base = AddressSpace::round_down(base);
    if (space->check(base, rounded, 0)) {
      // Committing pages that were reserved earlier.
      if (!(type & kMemCommit)) return kStatusConflictingAddresses;
      PageAttributes existing;
      if (space->attributes_at(base, &existing) && existing.type == PageAttributes::Image) return kStatusAccessDenied;      // the pages of an image are never committed again
      if (space->attributes_at(base, &existing) && existing.type == PageAttributes::Mapped) {      // a view: a file's pages are already there; plain memory takes what the view allows
        if (existing.file_view) return kStatusAccessDenied;
        if (page_access(protect) & ~page_access(existing.allocation_protect)) return kStatusInvalidParameter;
      }
      if (!space->protect(base, rounded, prot) || !space->set_page_protect(base, rounded, protect, false)) return kStatusConflictingAddresses;
    } else if (!space->map(base, rounded, prot)) {
      return kStatusConflictingAddresses;
    } else {
      set_private_attributes(*space, base, rounded, protect, type);
    }
  } else {
    uint64_t at = 0;
    const uint64_t hint = (target.mmap_cursor() + kAllocationGranularity - 1) & ~(kAllocationGranularity - 1);
    if (!space->find_free(rounded, hint, &at)) return kStatusNoMemory;
    at = (at + kAllocationGranularity - 1) & ~(kAllocationGranularity - 1);      // reservations begin on the allocation granularity
    if (!space->map(at, rounded, prot)) return kStatusNoMemory;
    set_private_attributes(*space, at, rounded, protect, type);
    target.set_mmap_cursor(at + rounded);
    base = at;
  }
  *base_io = base;
  *size_io = rounded;
  return kStatusSuccess;
}

uint32_t vm_free(Process& target, uint64_t* base_io, uint64_t* size_io, uint32_t type) {
  AddressSpace* space = target.address_space();
  uint64_t base = *base_io, size = *size_io;
  if (!space) return kStatusNoMemory;
  if (type == kMemRelease) {
    // Releases the whole allocation `base` is the start of (as NtAllocateVirtualMemory returned it); the size must be 0.
    uint64_t region_base = 0, region_size = 0;
    if (size != 0) return kStatusInvalidParameter;
    if (!space->allocation_extent(base, &region_base, &region_size) || region_base != base) return kStatusUnableToFreeVm;
    if (!space->unmap(region_base, region_size)) return kStatusUnableToFreeVm;
    *base_io = region_base;
    *size_io = region_size;
    return kStatusSuccess;
  }
  if (type == kMemDecommit) {
    uint64_t rounded = 0;
    base = AddressSpace::round_down(base);
    if (size == 0 || !AddressSpace::round_up(size, &rounded)) return kStatusInvalidParameter;
    PageAttributes existing;
    if (space->attributes_at(base, &existing) && existing.type != PageAttributes::Private) return kStatusInvalidParameter;      // the pages of a view stay committed
    if (!space->protect(base, rounded, 0)) return kStatusUnableToFreeVm;      // decommitted pages become inaccessible (their contents are not cleared)
    space->set_page_protect(base, rounded, kPageNoAccess, true);
    *base_io = base;
    *size_io = rounded;
    return kStatusSuccess;
  }
  return kStatusInvalidParameter;
}

uint32_t vm_protect(Process& target, uint64_t* base_io, uint64_t* size_io, uint32_t protect, uint32_t* old_protect) {
  AddressSpace* space = target.address_space();
  uint8_t prot = 0;
  if (!page_protection(protect, &prot)) return kStatusInvalidPageProtection;
  if (!space) return kStatusNoMemory;
  uint64_t rounded = 0;
  const uint64_t base = AddressSpace::round_down(*base_io);
  if ((protect & 0xFF) == kPageWriteCopy || (protect & 0xFF) == kPageExecuteWriteCopy) {      // only a view can be copy-on-write
    PageAttributes existing;
    if (!space->attributes_at(base, &existing) || existing.type == PageAttributes::Private) return kStatusInvalidPageProtection;
  }
  if (*size_io == 0 || !AddressSpace::round_up(*size_io, &rounded)) return kStatusInvalidParameter;
  uint8_t old = 0;
  if (!space->protection_at(base, &old)) return kStatusConflictingAddresses;
  {      // every page of the range has to be committed
    AddressSpace::RegionInfo region;
    for (uint64_t i = 0; space->region_at(i, &region); ++i) {
      if (region.base + region.size <= base) continue;
      if (region.base >= base + rounded) break;
      if (region.attributes.protect != 0 && region.attributes.reserved) return kStatusConflictingAddresses;
    }
  }
  *old_protect = effective_protect(*space, base);
  PageAttributes attributes;
  if (space->attributes_at(base, &attributes) && attributes.type == PageAttributes::Mapped && (page_access(protect) & ~page_access(attributes.allocation_protect))) return kStatusInvalidParameter;
  if (attributes.type == PageAttributes::Mapped && !space->is_shared(base)) {      // a page of a view that already has a copy of its own is written to freely
    if (protect == kPageWriteCopy) { protect = kPageReadWrite; prot = kProtRead | kProtWrite; }
    else if (protect == kPageExecuteWriteCopy) { protect = kPageExecuteReadWrite; prot = kProtRead | kProtWrite | kProtExecute; }
  }
  if (attributes.type == PageAttributes::Image && attributes.copied) {      // a page of an image that was written to has a copy of its own
    if (protect == kPageWriteCopy) { protect = kPageReadWrite; prot = kProtRead | kProtWrite; }
    else if (protect == kPageExecuteWriteCopy) { protect = kPageExecuteReadWrite; prot = kProtRead | kProtWrite | kProtExecute; }
  } else if (attributes.type == PageAttributes::Image) {      // an image is never written through: asking for write gives copy-on-write
    if (protect == kPageReadWrite) { protect = kPageWriteCopy; prot = kProtRead; }
    else if (protect == kPageExecuteReadWrite) { protect = kPageExecuteWriteCopy; prot = kProtRead | kProtExecute; }
  }
  if (!space->protect(base, rounded, prot) || !space->set_page_protect(base, rounded, protect, false)) return kStatusConflictingAddresses;
  *base_io = base;
  *size_io = rounded;
  return kStatusSuccess;
}

void vm_query(Process& target, uint64_t address, uint8_t* out) {
  for (uint32_t i = 0; i < 48; ++i) out[i] = 0;
  auto put64 = [&](uint32_t at, uint64_t v) { for (uint32_t i = 0; i < 8; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  auto put32 = [&](uint32_t at, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  AddressSpace* space = target.address_space();
  const uint64_t page = AddressSpace::round_down(address);
  uint64_t base = 0, size = 0;
  uint8_t protection = 0;
  if (space && space->region_extent(page, &base, &size) && space->protection_at(page, &protection)) {
    uint64_t allocation = 0, allocation_size = 0;
    if (!space->allocation_base(page, &allocation) && !space->allocation_extent(page, &allocation, &allocation_size)) allocation = base;
    PageAttributes attributes;
    const bool known = space->attributes_at(page, &attributes) && attributes.protect != 0;
    const uint32_t current = known ? attributes.protect : page_constant(protection);
    // The run of pages that look the same: neighbours of one allocation with the same permissions and attributes.
    uint64_t end = base + size, id = 0;
    AddressSpace::RegionInfo next;
    for (uint64_t i = 0; space->region_at(i, &next); ++i) {
      if (next.base == base) { id = next.allocation; continue; }
      if (next.base < end) continue;
      if (next.base != end || next.allocation != id || next.protection != protection || !(next.attributes == attributes)) break;
      end = next.base + next.size;
    }
    put64(0, page);
    put64(8, allocation);
    put32(16, known && attributes.allocation_protect ? attributes.allocation_protect : current);      // AllocationProtect
    put64(24, end - page);                     // RegionSize: to the end of this run of pages
    const bool reserved = known ? attributes.reserved : protection == 0;
    put32(32, reserved ? 0x2000 : 0x1000);     // State: MEM_RESERVE for pages nobody may touch, else MEM_COMMIT
    put32(36, reserved ? 0 : current);         // Protect
    put32(40, attributes.type == PageAttributes::Mapped ? 0x40000 : attributes.type == PageAttributes::Image ? 0x1000000 : 0x20000);      // Type: MEM_MAPPED, MEM_IMAGE, MEM_PRIVATE
  } else {
    // Free: up to the next mapped page.
    uint64_t next = AddressSpace::kUserTop;
    AddressSpace::RegionInfo region;
    for (uint64_t i = 0; space && space->region_at(i, &region); ++i) {
      if (region.base > page) { next = region.base; break; }
    }
    put64(0, page);
    put64(24, next - page);
    put32(32, 0x10000);      // MEM_FREE
    put32(36, 0x01);         // PAGE_NOACCESS
  }
}

// How many bytes from `address` on lie in committed pages (readable or not: another program's memory is reached without its permissions).
uint64_t committed_prefix(AddressSpace& space, uint64_t address, uint64_t size) {
  uint64_t done = 0;
  while (done < size) {
    const uint64_t at = address + done;
    if (at < address || at >= AddressSpace::kUserTop) break;
    uint8_t protection = 0;
    if (!space.protection_at(at, &protection) || protection == 0) break;
    const uint64_t to_page_end = AddressSpace::kPageSize - (at & (AddressSpace::kPageSize - 1));
    done += to_page_end < size - done ? to_page_end : size - done;
  }
  return done;
}

uint32_t vm_read(Process& target, uint64_t address, ByteView out, uint64_t* done) {
  AddressSpace* space = target.address_space();
  *done = 0;
  if (!space) return kStatusNoMemory;
  const uint64_t n = committed_prefix(*space, address, out.size());
  if (n > 0 && !space->read_privileged(address, out.first(n))) return kStatusAccessViolation;
  *done = n;
  return n == out.size() ? kStatusSuccess : kStatusPartialCopy;
}

// How many bytes from `address` on lie in pages that a write may change (read-write, copy-on-write, or the executable versions of those).
uint64_t writable_prefix(AddressSpace& space, uint64_t address, uint64_t size) {
  uint64_t done = 0;
  while (done < size) {
    const uint64_t at = address + done;
    if (at < address || at >= AddressSpace::kUserTop) break;
    PageAttributes attributes;
    if (!space.attributes_at(AddressSpace::round_down(at), &attributes)) break;
    if (attributes.protect != 0 && !attributes.reserved) {
      const uint32_t page = attributes.protect & 0xFF;
      if (page != kPageReadWrite && page != kPageWriteCopy && page != kPageExecuteReadWrite && page != kPageExecuteWriteCopy) break;
    } else {
      uint8_t protection = 0;
      if (!space.protection_at(at, &protection) || !(protection & kProtWrite)) break;
    }
    const uint64_t to_page_end = AddressSpace::kPageSize - (at & (AddressSpace::kPageSize - 1));
    done += to_page_end < size - done ? to_page_end : size - done;
  }
  return done;
}

uint32_t vm_write(Process& target, uint64_t address, ConstByteView in, uint64_t* done) {
  AddressSpace* space = target.address_space();
  *done = 0;
  if (!space) return kStatusNoMemory;
  const uint64_t n = writable_prefix(*space, address, in.size());
  if (n > 0 && !space->write_privileged(address, in.first(n))) return kStatusAccessViolation;
  *done = n;
  return n == in.size() ? kStatusSuccess : kStatusPartialCopy;
}

// The process a handle names for memory work: this one (*remote false), or another running program (its id in *pid).
uint32_t resolve(Process& p, uint64_t handle, bool* remote, uint64_t* pid) {
  *remote = false;
  if (handle == kCurrentProcess) return kStatusSuccess;
  bool exited = false;
  uint32_t code = 0;
  uint64_t id = 0;
  if (!process_handle_info(p, handle, &exited, &code, &id)) return kStatusInvalidHandle;
  if (id == p.id()) return kStatusSuccess;
  if (exited) return kStatusProcessIsTerminating;
  if (!p.runtime()) return kStatusNotImplemented;
  *remote = true;
  *pid = id;
  return kStatusSuccess;
}

}  // namespace

uint32_t run_remote(Process& target, IProcessRuntime::RemoteOp& op) {
  if (target.exited()) return kStatusProcessIsTerminating;
  switch (op.kind) {
    case IProcessRuntime::RemoteOp::Read: return vm_read(target, op.address, op.out, &op.done);
    case IProcessRuntime::RemoteOp::Write: return vm_write(target, op.address, op.in, &op.done);
    case IProcessRuntime::RemoteOp::Allocate: return vm_allocate(target, &op.address, &op.size, op.type, op.protect);
    case IProcessRuntime::RemoteOp::Free: return vm_free(target, &op.address, &op.size, op.type);
    case IProcessRuntime::RemoteOp::Protect: return vm_protect(target, &op.address, &op.size, op.protect, &op.old_protect);
    case IProcessRuntime::RemoteOp::Query: vm_query(target, op.address, op.info); return kStatusSuccess;
    case IProcessRuntime::RemoteOp::CreateThread: return create_thread_in(target, op.address, op.argument, op.suspended, op.stack_size, &op.thread_id);
    case IProcessRuntime::RemoteOp::ThreadStatus: return thread_status_in(target, op.thread_id, &op.size);
    case IProcessRuntime::RemoteOp::TerminateThread: return terminate_thread_in(target, op.thread_id, uint32_t(op.size));
    case IProcessRuntime::RemoteOp::ResumeThread: return resume_thread_in(target, op.thread_id, &op.size);
    case IProcessRuntime::RemoteOp::GetContext: return context_in(target, op.thread_id, false, op.context);
    case IProcessRuntime::RemoteOp::SetContext: return context_in(target, op.thread_id, true, op.context);
    case IProcessRuntime::RemoteOp::Unmap: return unmap_view_in(target, op.address);
    case IProcessRuntime::RemoteOp::MapView: return map_view_in(target, uint32_t(op.thread_id), op.argument, &op.address, &op.size, op.protect);
    case IProcessRuntime::RemoteOp::Duplicate: return give_object_handle_in(target, op.type, op.argument, op.protect, op.suspended, &op.address);
    case IProcessRuntime::RemoteOp::ProcessInfo: {
      op.address = 0;
      target.read_user_u64(target.gs_base() + 0x60, &op.address);      // TEB.ProcessEnvironmentBlock
      for (uint32_t i = 0; i < 64; ++i) op.image[i] = target.image_information()[i];
      return kStatusSuccess;
    }
  }
  return kStatusInvalidParameter;
}

// NtAllocateVirtualMemory(Process, PVOID* Base, ULONG_PTR ZeroBits, PSIZE_T Size, ULONG Type, ULONG Protect)
uint32_t nt_allocate_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t type = 0, protect = 0, base = 0, size = 0, pid = 0;
  bool remote = false;
  if (!argument(process, r, 4, &type) || !argument(process, r, 5, &protect)) return kStatusAccessViolation;
  const uint32_t resolved = resolve(process, r.args[0], &remote, &pid);
  if (resolved != kStatusSuccess) return resolved;
  if (!process.read_user_u64(r.args[1], &base) || !process.read_user_u64(r.args[3], &size)) return kStatusAccessViolation;
  uint32_t status;
  if (remote) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::Allocate;
    op.address = base; op.size = size; op.type = uint32_t(type); op.protect = uint32_t(protect);
    status = process.runtime()->remote_operation(pid, op);
    base = op.address; size = op.size;
  } else {
    status = vm_allocate(process, &base, &size, uint32_t(type), uint32_t(protect));
  }
  if (status != kStatusSuccess) return status;
  return process.write_user_u64(r.args[1], base) && process.write_user_u64(r.args[3], size) ? kStatusSuccess : kStatusAccessViolation;
}

// NtFreeVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG FreeType)
uint32_t nt_free_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t base = 0, size = 0, pid = 0;
  bool remote = false;
  const uint32_t resolved = resolve(process, r.args[0], &remote, &pid);
  if (resolved != kStatusSuccess) return resolved;
  if (!process.read_user_u64(r.args[1], &base) || !process.read_user_u64(r.args[2], &size)) return kStatusAccessViolation;
  uint32_t status;
  if (remote) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::Free;
    op.address = base; op.size = size; op.type = uint32_t(r.args[3]);
    status = process.runtime()->remote_operation(pid, op);
    base = op.address; size = op.size;
  } else {
    status = vm_free(process, &base, &size, uint32_t(r.args[3]));
  }
  if (status != kStatusSuccess) return status;
  return process.write_user_u64(r.args[1], base) && process.write_user_u64(r.args[2], size) ? kStatusSuccess : kStatusAccessViolation;
}

// NtProtectVirtualMemory(Process, PVOID* Base, PSIZE_T Size, ULONG NewProtect, PULONG OldProtect)
uint32_t nt_protect_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t old_pointer = 0, base = 0, size = 0, pid = 0;
  bool remote = false;
  if (!argument(process, r, 4, &old_pointer)) return kStatusAccessViolation;
  const uint32_t resolved = resolve(process, r.args[0], &remote, &pid);
  if (resolved != kStatusSuccess) return resolved;
  if (!process.read_user_u64(r.args[1], &base) || !process.read_user_u64(r.args[2], &size) || old_pointer == 0) return kStatusAccessViolation;
  uint32_t old = 0, status;
  if (remote) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::Protect;
    op.address = base; op.size = size; op.protect = uint32_t(r.args[3]);
    status = process.runtime()->remote_operation(pid, op);
    base = op.address; size = op.size; old = op.old_protect;
  } else {
    // (the old protection is written before the change, so that a bad pointer changes nothing)
    uint8_t probe = 0;
    AddressSpace* space = process.address_space();
    if (space && space->protection_at(AddressSpace::round_down(base), &probe)) {
      uint8_t bytes[4] = {};
      const uint32_t value = effective_protect(*space, AddressSpace::round_down(base));
      for (uint32_t i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
      if (!process.write_user(old_pointer, ConstByteView(bytes, 4))) return kStatusAccessViolation;
    }
    status = vm_protect(process, &base, &size, uint32_t(r.args[3]), &old);
  }
  if (status != kStatusSuccess) return status;
  uint8_t bytes[4] = {uint8_t(old), uint8_t(old >> 8), uint8_t(old >> 16), uint8_t(old >> 24)};
  if (!process.write_user(old_pointer, ConstByteView(bytes, 4))) return kStatusAccessViolation;
  return process.write_user_u64(r.args[1], base) && process.write_user_u64(r.args[2], size) ? kStatusSuccess : kStatusAccessViolation;
}

// NtQueryVirtualMemory(Process, BaseAddress, MemoryBasicInformation = 0, Buffer, Length, ReturnLength): MEMORY_BASIC_INFORMATION (48 bytes) for the page.
uint32_t nt_query_virtual_memory(Process& process, const SyscallRequest& r) {
  uint64_t length = 0, returned = 0, pid = 0;
  bool remote = false;
  if (!argument(process, r, 4, &length) || !argument(process, r, 5, &returned)) return kStatusAccessViolation;
  const uint32_t resolved = resolve(process, r.args[0], &remote, &pid);
  if (resolved != kStatusSuccess) return resolved;
  if ((r.args[2] & 0xFFFFFFFFu) != 0) return kStatusInvalidInfoClass;      // (a ULONG: mask the undefined upper half)
  if ((length & 0xFFFFFFFFu) < 48) return kStatusInfoLengthMismatch;
  if (AddressSpace::round_down(r.args[1]) >= AddressSpace::kUserTop) return kStatusInvalidParameter;
  uint8_t out[48];
  if (remote) {
    IProcessRuntime::RemoteOp op;
    op.kind = IProcessRuntime::RemoteOp::Query;
    op.address = r.args[1];
    const uint32_t status = process.runtime()->remote_operation(pid, op);
    if (status != kStatusSuccess) return status;
    for (uint32_t i = 0; i < 48; ++i) out[i] = op.info[i];
  } else {
    if (!process.address_space()) return kStatusNoMemory;
    vm_query(process, r.args[1], out);
  }
  if (!process.write_user(r.args[3], ConstByteView(out, 48))) return kStatusAccessViolation;
  if (returned != 0 && !process.write_user_u64(returned, 48)) return kStatusAccessViolation;
  return kStatusSuccess;
}

namespace {
// NtReadVirtualMemory / NtWriteVirtualMemory: the bytes move through a small buffer, a chunk at a time.
uint32_t move_memory(Process& process, const SyscallRequest& r, bool write) {
  uint64_t pid = 0, copied = 0;
  bool remote = false;
  const uint32_t resolved = resolve(process, r.args[0], &remote, &pid);
  if (resolved != kStatusSuccess) return resolved;
  uint64_t done_pointer = 0;
  if (!argument(process, r, 4, &done_pointer)) return kStatusAccessViolation;
  const uint64_t remote_address = r.args[1], local_address = r.args[2], size = r.args[3];
  uint32_t status = kStatusSuccess;
  uint8_t buffer[kChunk];
  if (!write && size > 0) {      // memory that cannot be read in full is not read at all
    uint64_t at = remote_address;
    const uint64_t end = remote_address + size;
    if (end < remote_address) status = kStatusPartialCopy;
    while (status == kStatusSuccess && at < end) {
      uint8_t info[48];
      if (remote) {
        IProcessRuntime::RemoteOp op;
        op.kind = IProcessRuntime::RemoteOp::Query;
        op.address = at;
        if (process.runtime()->remote_operation(pid, op) != kStatusSuccess) { status = kStatusPartialCopy; break; }
        for (uint32_t i = 0; i < 48; ++i) info[i] = op.info[i];
      } else {
        vm_query(process, at, info);
      }
      uint64_t region = 0, base = 0;
      uint32_t state = 0, protect = 0;
      for (uint32_t i = 0; i < 8; ++i) { region |= uint64_t{info[24 + i]} << (8 * i); base |= uint64_t{info[i]} << (8 * i); }
      for (uint32_t i = 0; i < 4; ++i) { state |= uint32_t{info[32 + i]} << (8 * i); protect |= uint32_t{info[36 + i]} << (8 * i); }
      if (state != 0x1000 || protect == 0 || (protect & 0x101) != 0 || region == 0) { status = kStatusPartialCopy; break; }
      at = base + region;
    }
  }
  if (write && status == kStatusSuccess) {      // a buffer that cannot be read in full is not written at all
    for (uint64_t at = 0; at < size; at += kChunk) {
      const uint64_t n = size - at < kChunk ? size - at : kChunk;
      if (!process.read_user(local_address + at, ByteView(buffer, n))) { status = kStatusPartialCopy; break; }
    }
  }
  while (status == kStatusSuccess && copied < size && status == kStatusSuccess) {
    const uint64_t n = size - copied < kChunk ? size - copied : kChunk;
    if (remote_address + copied < remote_address) { status = kStatusPartialCopy; break; }
    uint64_t done = 0;
    if (write) {
      if (!process.read_user(local_address + copied, ByteView(buffer, n))) { status = kStatusAccessViolation; break; }
      if (remote) {
        IProcessRuntime::RemoteOp op;
        op.kind = IProcessRuntime::RemoteOp::Write;
        op.address = remote_address + copied; op.in = ConstByteView(buffer, n);
        status = process.runtime()->remote_operation(pid, op);
        done = op.done;
      } else {
        status = vm_write(process, remote_address + copied, ConstByteView(buffer, n), &done);
      }
    } else {
      if (remote) {
        IProcessRuntime::RemoteOp op;
        op.kind = IProcessRuntime::RemoteOp::Read;
        op.address = remote_address + copied; op.out = ByteView(buffer, n);
        status = process.runtime()->remote_operation(pid, op);
        done = op.done;
      } else {
        status = vm_read(process, remote_address + copied, ByteView(buffer, n), &done);
      }
      if (done > 0 && !process.write_user(local_address + copied, ConstByteView(buffer, done))) {      // the local buffer ends in unwritable pages: what fits in the pages before them is delivered
        uint64_t fits = 0;
        while (fits < done) {
          const uint64_t at = local_address + copied + fits;
          const uint64_t to_page_end = AddressSpace::kPageSize - (at & (AddressSpace::kPageSize - 1));
          const uint64_t part = to_page_end < done - fits ? to_page_end : done - fits;
          if (!process.write_user(at, ConstByteView(buffer + fits, part))) break;
          fits += part;
        }
        done = fits;
        status = kStatusPartialCopy;
      }
    }
    copied += done;
    if (status == kStatusPartialCopy || done < n) break;
  }
  if (done_pointer != 0 && !process.write_user_u64(done_pointer, copied)) return kStatusAccessViolation;
  if (status == kStatusSuccess && copied < size) status = kStatusPartialCopy;
  return status;
}
}  // namespace

uint32_t nt_read_virtual_memory(Process& process, const SyscallRequest& r) { return move_memory(process, r, false); }
uint32_t nt_write_virtual_memory(Process& process, const SyscallRequest& r) { return move_memory(process, r, true); }

}  // namespace process::windowsabi
