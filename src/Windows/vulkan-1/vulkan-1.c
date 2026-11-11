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
 * So the driver is written once, over that layer, and works for all of them - and for Linux programs too: the driver is src/Vulkan (docs/Vulkan.md), shared with the Linux
 * libvulkan (src/Linux/libvulkan), which reaches the same services through /dev/dxg.
 *
 * This file is the Windows layer of it (vk_os.h): memory from the process heap, diagnostics to the debugger, the clock of QueryPerformanceCounter, the D3DKMT functions of
 * gdi32.dll, and VK_KHR_win32_surface (a window's client size from user32.dll). vkGetInstanceProcAddr answers NULL for what is not implemented and the exports that exist for the
 * sake of linking answer VK_ERROR_FEATURE_NOT_PRESENT (src/Windows/stubs/vulkan-1.inc). Built freestanding over kernel32.dll, gdi32.dll and user32.dll with the OS's mingw compiler. */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include "../../Vulkan/khronos/vulkan/vulkan_core.h"
#include "../../Vulkan/khronos/vulkan/vulkan_win32.h"

#define VK_OS_NAME "vulkan-1"
#define VK_OS_EXPORT __declspec(dllexport)
#define VK_OS_INSTANCE_EXTENSIONS {VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION},
#define VK_OS_PROCS(X) X(vkCreateWin32SurfaceKHR) X(vkGetPhysicalDeviceWin32PresentationSupportKHR)
#include "../../Vulkan/vk_driver.inc"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* ---- memory (the DLL is freestanding: these are the C library's for the compiler and the driver) ---------------------------------------------------------------------- */
void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *p = d;
  const uint8_t *q = s;
  while (n--) *p++ = *q++;
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  uint8_t *p = d;
  const uint8_t *q = s;
  if (p < q) {
    while (n--) *p++ = *q++;
  } else {
    p += n;
    q += n;
    while (n--) *--p = *--q;
  }
  return d;
}
void *memset(void *d, int c, size_t n) {
  uint8_t *p = d;
  while (n--) *p++ = (uint8_t)c;
  return d;
}

static void *vk_alloc(size_t bytes) { return bytes ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes) : NULL; }
static void vk_free(void *p) {
  if (p) HeapFree(GetProcessHeap(), 0, p);
}

/* ---- diagnostics and the clock ------------------------------------------------------------------------------------------------------------------------------------------ */
static void vk_os_trace(const char *text) {
  char name[8];
  OutputDebugStringA(text);
  if (GetEnvironmentVariableA("VULKAN_1_DEBUG", name, sizeof name)) {
    DWORD written;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, text, (DWORD)cstrlen(text), &written, NULL);
  }
}

static uint64_t vk_os_now_ns(void) {
  static LARGE_INTEGER frequency;
  LARGE_INTEGER t;
  if (!frequency.QuadPart && !QueryPerformanceFrequency(&frequency)) return 0;
  QueryPerformanceCounter(&t);
  return (uint64_t)(t.QuadPart / frequency.QuadPart) * 1000000000ull + (uint64_t)(t.QuadPart % frequency.QuadPart) * 1000000000ull / (uint64_t)frequency.QuadPart;
}

/* ---- the D3DKMT services of gdi32.dll ----------------------------------------------------------------------------------------------------------------------------------- */
#define KMT_DECL(name) __declspec(dllimport) LONG WINAPI name(void *params)
KMT_DECL(D3DKMTEnumAdapters2);
KMT_DECL(D3DKMTOpenAdapterFromLuid);
KMT_DECL(D3DKMTCloseAdapter);
KMT_DECL(D3DKMTQueryAdapterInfo);
KMT_DECL(D3DKMTCreateDevice);
KMT_DECL(D3DKMTDestroyDevice);
KMT_DECL(D3DKMTCreateContext);
KMT_DECL(D3DKMTDestroyContext);
KMT_DECL(D3DKMTCreateAllocation);
KMT_DECL(D3DKMTDestroyAllocation);
KMT_DECL(D3DKMTLock);
KMT_DECL(D3DKMTUnlock);
KMT_DECL(D3DKMTRender);
KMT_DECL(D3DKMTPresent);
KMT_DECL(D3DKMTEscape);
KMT_DECL(D3DKMTGetDisplayModeList);
KMT_DECL(D3DKMTGetDeviceState);

#define STATUS_NOT_IMPLEMENTED_KMT ((KmtStatus)0xC0000002u)
static KmtStatus kmt(uint32_t service, void *params) {
  switch (service) {
    case TCOS_DXG_ENUM_ADAPTERS2: return D3DKMTEnumAdapters2(params);
    case TCOS_DXG_OPEN_ADAPTER_FROM_LUID: return D3DKMTOpenAdapterFromLuid(params);
    case TCOS_DXG_CLOSE_ADAPTER: return D3DKMTCloseAdapter(params);
    case TCOS_DXG_QUERY_ADAPTER_INFO: return D3DKMTQueryAdapterInfo(params);
    case TCOS_DXG_CREATE_DEVICE: return D3DKMTCreateDevice(params);
    case TCOS_DXG_DESTROY_DEVICE: return D3DKMTDestroyDevice(params);
    case TCOS_DXG_CREATE_CONTEXT: return D3DKMTCreateContext(params);
    case TCOS_DXG_DESTROY_CONTEXT: return D3DKMTDestroyContext(params);
    case TCOS_DXG_CREATE_ALLOCATION: return D3DKMTCreateAllocation(params);
    case TCOS_DXG_DESTROY_ALLOCATION: return D3DKMTDestroyAllocation(params);
    case TCOS_DXG_LOCK: return D3DKMTLock(params);
    case TCOS_DXG_UNLOCK: return D3DKMTUnlock(params);
    case TCOS_DXG_RENDER: return D3DKMTRender(params);
    case TCOS_DXG_PRESENT: return D3DKMTPresent(params);
    case TCOS_DXG_ESCAPE: return D3DKMTEscape(params);
    case TCOS_DXG_GET_DISPLAY_MODE_LIST: return D3DKMTGetDisplayModeList(params);
    case TCOS_DXG_GET_DEVICE_STATE: return D3DKMTGetDeviceState(params);
    default: return STATUS_NOT_IMPLEMENTED_KMT;
  }
}

/* ---- VK_KHR_win32_surface ----------------------------------------------------------------------------------------------------------------------------------------------- */
enum { SURFACE_WIN32 = 1 };

VKAPI VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)instance, (void)alloc;
  return make_surface((uint64_t)(ULONG_PTR)ci->hwnd, SURFACE_WIN32, out);
}
VKAPI VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice pd, uint32_t family) {
  (void)pd;
  return family == 0;
}

static bool vk_os_surface_extent(const Surface *s, uint32_t *w, uint32_t *h) {
  RECT r;
  if (!s->window || !GetClientRect((HWND)(ULONG_PTR)s->window, &r)) return false;
  *w = (uint32_t)(r.right - r.left), *h = (uint32_t)(r.bottom - r.top);
  return true;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module, (void)reason, (void)reserved;
  return TRUE;
}
