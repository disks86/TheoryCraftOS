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

/* The kernel's Vulkan (KernelVulkan.h): the Venus backend of the shared driver, as one C translation unit of the kernel. What vk_os.h asks a platform for is
 * given here by the kernel's hooks (KernelVulkanOs.cpp); presentation is left out (VN_NO_PRESENT); there is no environment, so nothing asks for the other backend. */
#include "Graphics/Graphics3D/Venus/KernelVulkan.h"
#include "Process/VirtGpuDevice.h"

#define VK_OS_VIRTGPU 1
#define VN_NO_PRESENT 1
#define VK_OS_INSTANCE_EXTENSIONS /* (no surface extensions: the kernel presents its images itself) */

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* ---- what the platform gives (vk_os.h) ---- */
static void *vk_alloc(size_t bytes) { return bytes ? tcos_kvk_alloc(bytes) : NULL; }
static void vk_free(void *p) {
  if (p) tcos_kvk_free(p);
}
static void vk_os_trace(const char *text) { tcos_kvk_trace(text); }
static uint64_t vk_os_now_ns(void) { return tcos_kvk_now_ns(); }
static void vk_os_sleep_ns(uint64_t ns) { tcos_kvk_sleep_ns(ns); }
static bool vk_os_getenv(const char *name, char *out, size_t cap) {
  (void)name, (void)out, (void)cap;
  return false;   /* (the kernel has no environment) */
}
static int64_t vk_os_virtgpu_open(void) { return tcos_kvk_virtgpu_open(); }
static int vk_os_virtgpu_ioctl(int64_t node, uint32_t request, void *argument) { return tcos_kvk_virtgpu_ioctl(node, request, argument); }
static void *vk_os_virtgpu_map(int64_t node, uint64_t offset, uint64_t size) { return tcos_kvk_virtgpu_map(node, offset, size); }
static void vk_os_virtgpu_unmap(int64_t node, void *address, uint64_t size) { tcos_kvk_virtgpu_unmap(node, address, size); }
static void vk_os_virtgpu_close(int64_t node) { tcos_kvk_virtgpu_close(node); }

#include "Vulkan/vk_strings.inc"
#define ICD_LOADER_MAGIC ((void *)(uintptr_t)0x01CDC0DEu)

#include "Vulkan/vn_tables.inc"
#include "Vulkan/vk_venus.inc"

static const TcosKernelVulkan kKernelVulkan = {
#define TCOS_KVK_ENTRY(name) vn_vk##name,
    TCOS_KVK_PROCS(TCOS_KVK_ENTRY)
#undef TCOS_KVK_ENTRY
};

const TcosKernelVulkan *tcos_kernel_vulkan(void) { return &kKernelVulkan; }

bool tcos_kernel_vulkan_available(void) {
  VnRenderer probe;
  if (!vn_renderer_open(&probe)) return false;
  vn_renderer_close(&probe);
  return true;
}
