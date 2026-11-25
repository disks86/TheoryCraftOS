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

// What the kernel's Vulkan (KernelVulkan.c) needs from the kernel: memory, a trace line, a clock, sleeping, and render-node sessions of its own. A session
// is the same object a program gets from /dev/dri/renderD128 (graphics::virtgpu::Session), on the system's virtio-gpu; its ioctls take kernel pointers
// (KernelMemory) and its buffer objects are mapped where the kernel already sees them (host blobs: the host-visible window, which the kernel maps 1:1).
#include "Graphics/Graphics3D/Venus/KernelVulkanOs.hpp"
#include "BootLog.hpp"
#include "Graphics/Graphics3D/Venus/KernelVulkan.h"
#include "Graphics/VirtGpu/VirtGpuSession.hpp"
#include "Graphics/VirtGpu/VirtioVirtGpuHost.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace graphics::venus {
namespace {

// A session's ioctl argument structures are in kernel memory: an "address" is a kernel pointer.
class KernelMemory final : public process::IUserMemory {
 public:
  bool read(uint64_t address, ByteView out) override {
    if (!address && out.size()) return false;
    return out.copy_from(ConstByteView(as_bytes(address), out.size()));
  }
  bool write(uint64_t address, ConstByteView data) override {
    if (!address && data.size()) return false;
    return ByteView(as_bytes(address), data.size()).copy_from(data);
  }

 private:
  // (the only cast of an address to memory: the generated Venus code hands the session pointers to its own structures, which are kernel memory)
  static uint8_t* as_bytes(uint64_t address) { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address)); }
};

struct Mapping {
  uint8_t* address = nullptr;
  virtgpu::BufferRef buffer;   // (holds the buffer object while it is mapped, as a program's mapping does)
};

struct Node {
  UniquePointer<virtgpu::Session> session;
  List<Mapping> mappings;
  explicit Node(allocators::IAllocator& allocator) : mappings(allocator) {}
};

constexpr uint32_t kMaxNodes = 16;   // (each kernel Vulkan instance has one, and a probe has one for a moment)

struct State {
  allocators::IAllocator* allocator = nullptr;
  kernel::Mutex lock;   // the node table (not a session's calls: each session has its own lock)
  UniquePointer<Node> nodes[kMaxNodes];
};
State g_state;

Node* node_of(int64_t handle) {
  if (handle < 0 || handle >= static_cast<int64_t>(kMaxNodes)) return nullptr;
  g_state.lock.lock();
  Node* n = g_state.nodes[handle].get();
  g_state.lock.unlock();
  return n;
}

}  // namespace

void set_kernel_vulkan_allocator(allocators::IAllocator& allocator) { __atomic_store_n(&g_state.allocator, &allocator, __ATOMIC_RELEASE); }

}  // namespace graphics::venus

using graphics::venus::g_state;

extern "C" {

void* tcos_kvk_alloc(size_t bytes) {
  allocators::IAllocator* allocator = __atomic_load_n(&g_state.allocator, __ATOMIC_ACQUIRE);
  void* p = allocator && bytes ? allocator->allocate_aligned(bytes, 16) : nullptr;
  if (p) ByteView(static_cast<uint8_t*>(p), bytes).fill(0);
  return p;
}

void tcos_kvk_free(void* p) {
  allocators::IAllocator* allocator = __atomic_load_n(&g_state.allocator, __ATOMIC_ACQUIRE);
  if (p && allocator) allocator->deallocate(p);
}

void tcos_kvk_trace(const char* text) {
  if (!text) return;
  size_t n = 0;
  while (text[n]) ++n;
  while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) --n;
  bootlog::Entry(bootlog::Level::Warning, "Venus").text(StringView(text, n));
}

uint64_t tcos_kvk_now_ns(void) { return hardware::uptime_us() * 1000; }

void tcos_kvk_sleep_ns(uint64_t ns) {
  if (ns < 1000) kernel::yield();
  else kernel::sleep_us(ns / 1000);
}

int64_t tcos_kvk_virtgpu_open(void) {
  graphics::virtgpu::IVirtGpuHost* host = graphics::virtgpu::system_host();
  allocators::IAllocator* allocator = __atomic_load_n(&g_state.allocator, __ATOMIC_ACQUIRE);
  if (!host || !allocator) return -1;
  UniquePointer<graphics::venus::Node> node = UniquePointer<graphics::venus::Node>::make(*allocator, *allocator);
  if (!node) return -1;
  node->session = UniquePointer<graphics::virtgpu::Session>::make(*allocator, *host, *allocator);
  if (!node->session) return -1;
  int64_t handle = -1;
  g_state.lock.lock();
  for (uint32_t i = 0; i < graphics::venus::kMaxNodes; ++i)
    if (!g_state.nodes[i]) {
      g_state.nodes[i] = static_cast<UniquePointer<graphics::venus::Node>&&>(node);
      handle = i;
      break;
    }
  g_state.lock.unlock();
  return handle;   // (-1 with every slot taken: the node goes with `node`)
}

int tcos_kvk_virtgpu_ioctl(int64_t handle, uint32_t request, void* argument) {
  graphics::venus::Node* node = graphics::venus::node_of(handle);
  if (!node) return graphics::virtgpu::kEbadf;
  graphics::venus::KernelMemory memory;
  const int32_t answer = node->session->ioctl(memory, request, reinterpret_cast<uintptr_t>(argument), nullptr);
  return answer < 0 ? -answer : 0;
}

void* tcos_kvk_virtgpu_map(int64_t handle, uint64_t offset, uint64_t size) {
  graphics::venus::Node* node = graphics::venus::node_of(handle);
  if (!node || size == 0 || size > ~uint64_t{0} - 4095) return nullptr;
  const uint64_t rounded = (size + 4095) & ~uint64_t{4095};
  graphics::venus::Mapping m;
  m.buffer = node->session->buffer_at(offset, rounded);
  if (!m.buffer || !m.buffer->mappable()) return nullptr;
  m.address = m.buffer->storage();
  g_state.lock.lock();
  const bool kept = node->mappings.push_back(m);
  g_state.lock.unlock();
  return kept ? m.address : nullptr;
}

void tcos_kvk_virtgpu_unmap(int64_t handle, void* address, uint64_t size) {
  (void)size;
  graphics::venus::Node* node = graphics::venus::node_of(handle);
  if (!node) return;
  graphics::virtgpu::BufferRef dropped;   // (let go after the lock)
  g_state.lock.lock();
  for (uint64_t i = 0; i < node->mappings.count(); ++i)
    if (node->mappings[i].address == address) {
      dropped = node->mappings[i].buffer;
      node->mappings.remove_at(i);
      break;
    }
  g_state.lock.unlock();
}

void tcos_kvk_virtgpu_close(int64_t handle) {
  if (handle < 0 || handle >= static_cast<int64_t>(graphics::venus::kMaxNodes)) return;
  UniquePointer<graphics::venus::Node> dropped;   // (the session talks to the device as it goes: after the lock)
  g_state.lock.lock();
  dropped = static_cast<UniquePointer<graphics::venus::Node>&&>(g_state.nodes[handle]);
  g_state.lock.unlock();
}

}  // extern "C"
