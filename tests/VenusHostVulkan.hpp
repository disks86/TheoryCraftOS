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

// The kernel's Vulkan table (src/Graphics/Graphics3D/Venus/KernelVulkan.h) filled with the build host's own Vulkan loader, for host programs that run the Venus device
// (VenusHostTest, EmulationRun with TCOS_EMU_VENUS). Include it in one source file; call load_host_vulkan() before the device is made.
#pragma once
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include "Graphics/Graphics3D/Venus/KernelVulkan.h"

// ---- the host's Vulkan as the kernel's table -----------------------------------------------------------------------------------------------------------
namespace {
void* g_loader = nullptr;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
VkInstance g_instance = nullptr;
TcosKernelVulkan g_table;

PFN_vkVoidFunction proc(const char* name) {
  PFN_vkVoidFunction f = g_gipa(g_instance, name);
  if (!f) {
    fprintf(stderr, "the host's Vulkan has no %s\n", name);
    abort();
  }
  return f;
}

// (functions looked up once the instance exists: every one of the table goes through these)
#define TRAMPOLINE(name, ret, params, args)                                                     \
  ret VKAPI_CALL t_##name params {                                                             \
    static PFN_vk##name f = reinterpret_cast<PFN_vk##name>(proc("vk" #name));                  \
    return f args;                                                                             \
  }
VkResult VKAPI_CALL t_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
  auto f = reinterpret_cast<PFN_vkCreateInstance>(g_gipa(nullptr, "vkCreateInstance"));
  const VkResult r = f(ci, a, out);
  if (r == VK_SUCCESS) g_instance = *out;
  return r;
}
}  // namespace

#include "VenusHostVulkan.inc"

extern "C" {
const TcosKernelVulkan* tcos_kernel_vulkan(void) { return &g_table; }
bool tcos_kernel_vulkan_available(void) { return g_gipa != nullptr; }
void* tcos_kvk_alloc(size_t bytes) { return calloc(1, bytes ? bytes : 1); }
void tcos_kvk_free(void* p) { free(p); }
void tcos_kvk_trace(const char* text) { printf("  %s\n", text); }
}

static bool load_host_vulkan() {
  g_loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!g_loader) return false;
  g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
  if (!g_gipa) return false;
  fill_table(&g_table);
  // (an instance of the host's loader kept to the end, with its devices enumerated: the loader unloads the drivers when the last instance goes, and a driver's
  // one-time allocations - RADV's, made under pthread_once - are then reported by LeakSanitizer as leaks of an unknown module; kept loaded they stay reachable)
  static VkInstance keep = nullptr;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(g_gipa(nullptr, "vkCreateInstance"));
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "VenusHostTest", 1, nullptr, 0, VK_API_VERSION_1_3};
  VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
  if (create && create(&ci, nullptr, &keep) == VK_SUCCESS) {
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(g_gipa(keep, "vkEnumeratePhysicalDevices"));
    uint32_t count = 0;
    if (enumerate) (void)enumerate(keep, &count, nullptr);
  }
  return true;
}

