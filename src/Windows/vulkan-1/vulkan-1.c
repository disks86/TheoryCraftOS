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

/* vulkan-1.dll for TheoryCraftOS: the Vulkan loader and the one driver behind it. A real vulkan-1.dll finds the drivers of the installed GPUs and forwards the calls to them; this OS has
 * one graphics architecture for every GPU it drives (docs/Direct3DKernelMode.md): the kernel-mode Direct3D layer (dxgkrnl, reached with the D3DKMT* services of gdi32.dll) owns the
 * adapters, video memory and command queues, and replays a device-independent command stream on whatever the adapter is - the CPU renderer, virgl, SVGA3D, Intel Gen9, V3D, Radeon Vega.
 * So the driver here is written once, over that layer, and works for all of them:
 *
 *   - the adapters are the kernel's (vkEnumeratePhysicalDevices); the limits, features and formats are what the adapter reports (QueryAdapterInfo: texture units, push constant bytes,
 *     the DeviceFeature bits), not what a desktop GPU has - a draw that needs more than the adapter has is refused when the pipeline is made;
 *   - shaders are SPIR-V, translated to DXBC (spirv.inc) and handed to the kernel as DXBC through its private escape;
 *   - a command buffer is recorded and run at vkQueueSubmit: draws become the kernel's command stream (one stream per render pass), copies and clears that the CPU can do are done in
 *     order with it; every submission has run when vkQueueSubmit returns, so fences, semaphores and events only keep a state;
 *   - memory is host memory; a buffer or image that the GPU uses is copied to its kernel allocation when the program has written the memory since it was last copied;
 *   - presentation flips a swapchain image onto the display.
 * Not here: compute, geometry and tessellation shaders, storage buffers and images, texel buffers, queries, indirect draws, sparse resources, a second queue, Vulkan 1.1 and later (the
 * driver is a 1.0 driver). vkGetInstanceProcAddr answers NULL for what is not here and the exports that exist for the sake of linking answer VK_ERROR_FEATURE_NOT_PRESENT
 * (src/Windows/stubs/vulkan-1.inc). Built freestanding over kernel32.dll, gdi32.dll and user32.dll with the OS's mingw compiler. */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES_DISABLED 0
#include <windows.h>
#include "khronos/vulkan/vulkan_core.h"
#include "khronos/vulkan/vulkan_win32.h"
#include "spirv.h"
#include "spirv.inc"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "vk_base.inc"
#include "vk_instance.inc"
#include "vk_resources.inc"
#include "vk_pipeline.inc"
#include "vk_commands.inc"
#include "vk_wsi.inc"
#include "vk_entry.inc"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module, (void)reason, (void)reserved;
  return TRUE;
}
