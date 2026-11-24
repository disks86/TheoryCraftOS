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

#include "Process/WindowsVirtGpu.hpp"
#include "Graphics/VirtGpu/VirtGpuSession.hpp"
#include "Graphics/VirtGpu/VirtioVirtGpuHost.hpp"
#include <new>
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/VirtGpuDevice.h"
#include "Utility/ByteStream.hpp"
#include "Utility/SharedPointer.hpp"

namespace process::windowsabi {

namespace {
constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNoMemory = 0xC0000017u,
                   kConflictingAddresses = 0xC0000018u, kNoSuchDevice = 0xC000000Eu, kTooManyOpened = 0xC000011Fu;
constexpr uint32_t kMaxSessionsPerProcess = 64;

struct Open {
  uint64_t pid = 0;
  uint64_t handle = 0;
  SharedPointer<graphics::virtgpu::Session> session;
};

struct State {
  kernel::Mutex lock;
  List<Open>* opens = nullptr;   // (made by install_virtgpu_service, from the kernel's shared allocator)
  uint64_t next_handle = 0x7600;
  allocators::IAllocator* allocator = nullptr;
};
State g_state;

SharedPointer<graphics::virtgpu::Session> find(uint64_t pid, uint64_t handle) {
  SharedPointer<graphics::virtgpu::Session> found;
  g_state.lock.lock();
  if (g_state.opens)
    for (const Open& o : *g_state.opens)
      if (o.pid == pid && o.handle == handle) {
        found = o.session;
        break;
      }
  g_state.lock.unlock();
  return found;
}

uint32_t open_session(Process& p, uint64_t* handle) {
  graphics::virtgpu::IVirtGpuHost* host = graphics::virtgpu::system_host();
  if (!host || !g_state.allocator || !g_state.opens) return kNoSuchDevice;
  allocators::IAllocator& allocator = *g_state.allocator;
  SharedPointer<graphics::virtgpu::Session> session = SharedPointer<graphics::virtgpu::Session>::make(allocator, *host, allocator);
  if (!session) return kNoMemory;
  g_state.lock.lock();
  uint32_t status = kSuccess;
  {
    uint32_t mine = 0;
    for (const Open& o : *g_state.opens) mine += o.pid == p.id();
    if (mine >= kMaxSessionsPerProcess) {
      status = kTooManyOpened;
    } else {
      Open o;
      o.pid = p.id();
      o.handle = g_state.next_handle += 4;   // (Windows handles are multiples of four)
      o.session = session;
      if (!g_state.opens->push_back(o)) status = kNoMemory;
      else *handle = o.handle;
    }
  }
  g_state.lock.unlock();
  return status;
}

bool close_session(uint64_t pid, uint64_t handle) {
  SharedPointer<graphics::virtgpu::Session> dropped;   // (destroyed after the lock is let go: it talks to the device)
  g_state.lock.lock();
  bool found = false;
  if (g_state.opens)
    for (uint64_t i = 0; i < g_state.opens->count(); ++i)
      if ((*g_state.opens)[i].pid == pid && (*g_state.opens)[i].handle == handle) {
        dropped = (*g_state.opens)[i].session;
        g_state.opens->remove_at(i);
        found = true;
        break;
      }
  g_state.lock.unlock();
  return found;
}

// A program's mapping of a buffer: the buffer's memory shared into its address space (the buffer is the region's owner, so it lives as long as the mapping).
uint32_t map(Process& p, graphics::virtgpu::Session& session, uint64_t offset, uint64_t size, uint64_t* address) {
  AddressSpace* space = p.address_space();
  uint64_t rounded = 0;
  if (!space || size == 0 || !AddressSpace::round_up(size, &rounded)) return kInvalidParameter;
  graphics::virtgpu::BufferRef buffer = session.buffer_at(offset, rounded);
  if (!buffer) return kInvalidParameter;
  uint64_t at = 0;
  if (!space->find_free(rounded, p.mmap_cursor(), &at)) return kNoMemory;
  if (!space->map_shared(at, rounded, kProtRead | kProtWrite, buffer->storage(), buffer.get(), buffer->cpu_cache())) return kConflictingAddresses;
  *address = at;
  return kSuccess;
}
}  // namespace

bool install_virtgpu_service(allocators::IAllocator& allocator) {
  if (g_state.opens) return true;
  void* memory = allocator.allocate_aligned(sizeof(List<Open>), alignof(List<Open>));
  if (!memory) return false;
  g_state.allocator = &allocator;
  g_state.opens = new (memory) List<Open>(allocator);
  return true;
}

uint32_t nt_tcos_virtgpu(Process& p, const SyscallRequest& r) {
  AddressSpace* space = p.address_space();
  if (!space) return kAccessViolation;
  uint64_t a[8];
  if (!space->read(r.args[0], ByteView(reinterpret_cast<uint8_t*>(a), sizeof(a)))) return kAccessViolation;
  uint64_t results[4] = {};
  uint32_t status = kSuccess;
  switch (a[0]) {
    case TCOS_VIRTGPU_OP_OPEN: status = open_session(p, &results[0]); break;
    case TCOS_VIRTGPU_OP_CLOSE: status = close_session(p.id(), a[1]) ? kSuccess : kInvalidHandle; break;
    case TCOS_VIRTGPU_OP_IOCTL: {
      SharedPointer<graphics::virtgpu::Session> session = find(p.id(), a[1]);
      if (!session) return kInvalidHandle;
      // (no descriptors on Windows: the fence and PRIME calls that need them answer EOPNOTSUPP)
      const int32_t answer = session->ioctl(*space, static_cast<uint32_t>(a[2]), a[3], nullptr);
      results[0] = static_cast<uint64_t>(answer < 0 ? -answer : 0);
      break;
    }
    case TCOS_VIRTGPU_OP_MAP: {
      SharedPointer<graphics::virtgpu::Session> session = find(p.id(), a[1]);
      if (!session) return kInvalidHandle;
      status = map(p, *session, a[2], a[3], &results[0]);
      break;
    }
    case TCOS_VIRTGPU_OP_UNMAP: {
      if (!find(p.id(), a[1])) return kInvalidHandle;
      uint64_t rounded = 0;
      if (a[3] == 0 || !AddressSpace::round_up(a[3], &rounded) || !space->is_shared(a[2])) return kInvalidParameter;
      status = space->unmap(a[2], rounded) ? kSuccess : kInvalidParameter;
      break;
    }
    default: return kInvalidParameter;
  }
  if (status == kSuccess && !space->write(r.args[0] + 4 * 8, ConstByteView(reinterpret_cast<const uint8_t*>(results), sizeof(results)))) return kAccessViolation;
  return status;
}

void virtgpu_process_exited(uint64_t pid) {
  for (;;) {
    SharedPointer<graphics::virtgpu::Session> dropped;
    g_state.lock.lock();
    bool any = false;
    if (g_state.opens)
      for (uint64_t i = 0; i < g_state.opens->count(); ++i)
        if ((*g_state.opens)[i].pid == pid) {
          dropped = (*g_state.opens)[i].session;
          g_state.opens->remove_at(i);
          any = true;
          break;
        }
    g_state.lock.unlock();
    if (!any) return;
  }
}

uint32_t virtgpu_session_count() {
  g_state.lock.lock();
  const uint32_t n = g_state.opens ? static_cast<uint32_t>(g_state.opens->count()) : 0;
  g_state.lock.unlock();
  return n;
}

}  // namespace process::windowsabi
