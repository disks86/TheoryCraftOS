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

/* What the shared Vulkan driver (src/Vulkan, docs/Vulkan.md) needs from the system it runs on. The driver is one translation unit: a platform's source file
 *
 *   1. includes its own headers and the Khronos headers of its surface extension (vulkan_win32.h, ...), defines VK_OS_NAME, VK_OS_INSTANCE_EXTENSIONS and VK_OS_PROCS,
 *   2. includes "vk_driver.inc" (this header, the translator and the driver),
 *   3. defines the functions declared below and its surface functions (each one makes a Surface with make_surface()).
 *
 * The platforms: src/Windows/vulkan-1/vulkan-1.c (vulkan-1.dll over gdi32's D3DKMT thunks, VK_KHR_win32_surface) and src/Linux/libvulkan/libvulkan.c (libvulkan.a over /dev/dxg,
 * VK_EXT_headless_surface). Everything else - the objects, the formats, memory, pipelines, the SPIR-V translator, command buffers, swapchains - is shared. */
#ifndef TCOS_VK_OS_H
#define TCOS_VK_OS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "khronos/vulkan/vulkan_core.h"
#include "../Process/DxgDevice.h"   /* the TCOS_DXG_* service numbers */

#ifndef VK_OS_NAME
#error "the platform defines VK_OS_NAME (the prefix of the driver's trace lines)"
#endif
#ifndef VK_OS_INSTANCE_EXTENSIONS
#error "the platform defines VK_OS_INSTANCE_EXTENSIONS (its surface extensions, entries of a VkExtensionProperties table)"
#endif
#ifndef VK_OS_PROCS
#error "the platform defines VK_OS_PROCS(X) (X(name) for each Vulkan function of its own)"
#endif
#ifndef VK_OS_EXPORT
#error "the platform defines VK_OS_EXPORT (how a Vulkan entry point is made visible to programs)"
#endif
#define VKAPI VK_OS_EXPORT VKAPI_ATTR

/* The status of a D3DKMT service (an NTSTATUS: negative is a failure). */
typedef int32_t KmtStatus;

/* Runs D3DKMT service `service` (a TCOS_DXG_* number) with its parameter block. */
static KmtStatus kmt(uint32_t service, void *params);
/* Zeroed memory (NULL for 0 bytes or when it ran out) and its release (NULL is fine). Safe from any thread. */
static void *vk_alloc(size_t bytes);
static void vk_free(void *p);
/* Writes a line of diagnostics (why a call failed) where a developer of the program looks: the debugger, and standard error when VULKAN_1_DEBUG is set. */
static void vk_os_trace(const char *text);
/* A monotonic clock in nanoseconds (timestamp queries). */
static uint64_t vk_os_now_ns(void);
/* The size of the window a surface shows into; false when it has none or the platform cannot say (the driver then answers 640 x 480). */
typedef struct Surface Surface;
static bool vk_os_surface_extent(const Surface *surface, uint32_t *width, uint32_t *height);

/* GPU images as descriptors of the platform (VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf: Linux's dmabufs, made by /dev/dxg). A platform that has them
 * defines VK_OS_EXTERNAL_MEMORY_FD to 1 and these three; the driver then offers the extensions and vkGetMemoryFdKHR (in its VK_OS_PROCS). An export is of an allocation the
 * driver made shareable; an import gives a handle on `device` for a descriptor and says what the image is (the kernel's format number, AW_FORMAT); the descriptor stays the
 * caller's either way. */
#if VK_OS_EXTERNAL_MEMORY_FD
static bool vk_os_export_fd(uint32_t allocation, int *fd);
static bool vk_os_import_fd(int fd, uint32_t device, uint32_t *allocation, uint32_t *width, uint32_t *height, uint32_t *format, uint32_t *pitch);
static void vk_os_close_fd(int fd);
#endif

/* Surfaces a compositor shows (VK_KHR_wayland_surface): a platform that has them defines VK_OS_COMPOSITOR_SURFACES to 1 and these four (vk_wsi.inc). Its surface has `os` set
 * (freed with vk_free when the surface goes); a swapchain of it has images whose allocations are shareable, and
 *   vk_os_swapchain_made   hands them to the compositor (a buffer each), after they were made; a failure undoes the swapchain
 *   vk_os_swapchain_gone   takes them back, before they are freed
 *   vk_os_acquire          picks an image the compositor is done with and the program does not hold (waits up to `timeout` ns for one: VK_TIMEOUT / VK_NOT_READY)
 *   vk_os_present          shows image `index`; every submission before it has run, so the image holds the frame */

/* (the driver also uses memcpy, memmove, memset and memcmp: the platform has them, from its C library or its own) */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif
