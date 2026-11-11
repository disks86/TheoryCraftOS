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

/* libvulkan for Linux programs on TheoryCraftOS: the same Vulkan driver as vulkan-1.dll (src/Vulkan, docs/Vulkan.md) over the kernel-mode Direct3D layer, which a Linux process
 * reaches through /dev/dxg (src/Process/DxgDevice.h) instead of the NtGdiDdDDI* system calls. Every adapter of the kernel is a VkPhysicalDevice, as for a Windows program.
 *
 * This file is the Linux layer of the driver (vk_os.h): raw Linux system calls (x86-64, AArch64, RISC-V 64 and LoongArch 64: a program of another CPU runs on the kernel's
 * emulator with the same calls), memory from mmap, diagnostics to standard error when VULKAN_1_DEBUG is set, CLOCK_MONOTONIC, the ioctl of /dev/dxg, images as
 * dmabufs (/dev/dxg's export and import: VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf, what a Wayland client passes the compositor) and
 * VK_EXT_headless_surface (a surface without a window: its swapchain is shown on the display).
 *
 * It needs no C library: it is built freestanding into libvulkan.a (Linux programs here are linked statically: the system has no dynamic loader yet), and memcpy, memmove,
 * memset and memcmp are weak, so a C library's own win when the program has one. The program calls the vk* functions directly or through vkGetInstanceProcAddr. */
#include "../../Vulkan/khronos/vulkan/vulkan_core.h"

#define VK_OS_NAME "libvulkan"
#define VK_OS_EXPORT __attribute__((visibility("default")))
#define VK_OS_INSTANCE_EXTENSIONS {VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_SPEC_VERSION},
#define VK_OS_PROCS(X) X(vkCreateHeadlessSurfaceEXT) X(vkGetMemoryFdKHR) X(vkGetMemoryFdPropertiesKHR)
#define VK_OS_EXTERNAL_MEMORY_FD 1   /* images as dmabufs of /dev/dxg (vk_external.inc) */
#include "../../Vulkan/vk_driver.inc"

/* ---- Linux system calls ------------------------------------------------------------------------------------------------------------------------------------------------ */
#if defined(__x86_64__)
enum { SYS_WRITE = 1, SYS_MMAP = 9, SYS_MUNMAP = 11, SYS_IOCTL = 16, SYS_CLOSE = 3, SYS_CLOCK_GETTIME = 228, SYS_OPENAT = 257 };
static long linux_call(long n, long a, long b, long c, long d, long e, long f) {
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  long result;
  __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return result;
}
#elif defined(__aarch64__) || defined(__riscv) || defined(__loongarch64)
/* the "generic" table */
enum { SYS_WRITE = 64, SYS_MMAP = 222, SYS_MUNMAP = 215, SYS_IOCTL = 29, SYS_CLOSE = 57, SYS_CLOCK_GETTIME = 113, SYS_OPENAT = 56 };
static long linux_call(long n, long a, long b, long c, long d, long e, long f) {
#if defined(__aarch64__)
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
#else
  register long a7 __asm__("a7") = n;
  register long a0 __asm__("a0") = a;
  register long a1 __asm__("a1") = b;
  register long a2 __asm__("a2") = c;
  register long a3 __asm__("a3") = d;
  register long a4 __asm__("a4") = e;
  register long a5 __asm__("a5") = f;
#if defined(__riscv)
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
#else
  __asm__ volatile("syscall 0" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory", "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8");
#endif
  return a0;
#endif
}
#else
#error "libvulkan: no system call interface for this CPU"
#endif

enum { AT_FDCWD = -100, O_RDWR = 2, O_CLOEXEC = 0x80000, PROT_READ = 1, PROT_WRITE = 2, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20, CLOCK_MONOTONIC = 1 };
#define PAGE_BYTES 4096u

static void *map_pages(size_t bytes) {
  long r = linux_call(SYS_MMAP, 0, (long)bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return r < 0 && r > -4096 ? NULL : (void *)r;      /* (-errno is a failure, anything else an address) */
}

/* ---- memory (weak: a C library's own are better and take their place) ------------------------------------------------------------------------------------------------ */
__attribute__((weak)) void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *p = d;
  const uint8_t *q = s;
  while (n--) *p++ = *q++;
  return d;
}
__attribute__((weak)) void *memmove(void *d, const void *s, size_t n) {
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
__attribute__((weak)) void *memset(void *d, int c, size_t n) {
  uint8_t *p = d;
  while (n--) *p++ = (uint8_t)c;
  return d;
}
__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *p = a, *q = b;
  for (; n--; ++p, ++q)
    if (*p != *q) return *p < *q ? -1 : 1;
  return 0;
}

/* The heap: blocks of 32 .. 4096 bytes (powers of two, a free list each) cut from 64 KiB pieces of mmap, anything bigger mapped on its own. Every block starts with a 16-byte
 * header (keeps the user's part 16-byte aligned) that says which it is. One spin lock guards the lists: the driver's allocations are short and few per call. Freed small blocks
 * are reused, never given back to the system. */
enum { HEAP_CLASSES = 8, HEAP_MIN_SHIFT = 5, HEAP_HEADER = 16, HEAP_PIECE = 64 * 1024 };
#define HEAP_LARGE (1ull << 63)
typedef struct FreeBlock {
  struct FreeBlock *next;
} FreeBlock;
static FreeBlock *g_free[HEAP_CLASSES];
static uint8_t *g_piece;
static size_t g_piece_left;
static int g_heap_lock;

static void heap_lock(void) {
  while (__atomic_exchange_n(&g_heap_lock, 1, __ATOMIC_ACQUIRE))
    while (__atomic_load_n(&g_heap_lock, __ATOMIC_RELAXED)) {
    }
}
static void heap_unlock(void) { __atomic_store_n(&g_heap_lock, 0, __ATOMIC_RELEASE); }

static void *vk_alloc(size_t bytes) {
  if (bytes == 0 || bytes > SIZE_MAX - HEAP_HEADER - PAGE_BYTES) return NULL;
  const size_t total = bytes + HEAP_HEADER;
  uint8_t *block = NULL;
  uint64_t tag;
  size_t usable;
  if (total <= ((size_t)1 << (HEAP_MIN_SHIFT + HEAP_CLASSES - 1))) {
    unsigned c = 0;
    while (((size_t)1 << (HEAP_MIN_SHIFT + c)) < total) ++c;
    const size_t size = (size_t)1 << (HEAP_MIN_SHIFT + c);
    heap_lock();
    if (g_free[c]) {
      block = (uint8_t *)g_free[c];
      g_free[c] = g_free[c]->next;
    } else {
      if (g_piece_left < size) {
        uint8_t *piece = map_pages(HEAP_PIECE);      /* (what was left of the old piece stays unused: less than one block of this class) */
        if (piece) g_piece = piece, g_piece_left = HEAP_PIECE;
      }
      if (g_piece_left >= size) block = g_piece, g_piece += size, g_piece_left -= size;
    }
    heap_unlock();
    tag = c, usable = size - HEAP_HEADER;
  } else {
    const size_t mapped = (total + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;      /* (no wrap: bytes was checked against SIZE_MAX above) */
    block = map_pages(mapped);
    tag = HEAP_LARGE | mapped, usable = mapped - HEAP_HEADER;
  }
  if (!block) return NULL;
  memcpy(block, &tag, sizeof tag);
  memset(block + HEAP_HEADER, 0, usable < bytes ? usable : bytes);      /* (a large mapping is zero already past `bytes`; a reused small block is cleared here) */
  return block + HEAP_HEADER;
}
static void vk_free(void *p) {
  if (!p) return;
  uint8_t *block = (uint8_t *)p - HEAP_HEADER;
  uint64_t tag;
  memcpy(&tag, block, sizeof tag);
  if (tag & HEAP_LARGE) {
    linux_call(SYS_MUNMAP, (long)block, (long)(tag & ~HEAP_LARGE), 0, 0, 0, 0);
    return;
  }
  if (tag >= HEAP_CLASSES) return;      /* (not a block of this heap: leak it rather than corrupt a list) */
  FreeBlock *f = (FreeBlock *)block;
  heap_lock();
  f->next = g_free[tag];
  g_free[tag] = f;
  heap_unlock();
}

/* ---- diagnostics and the clock ------------------------------------------------------------------------------------------------------------------------------------------ */
extern char **environ __attribute__((weak));      /* (the C library's, when the program has one; without one there is no environment to read) */

static bool debug_wanted(void) {
  static int known;      /* 0: not looked yet, 1: no, 2: yes */
  int k = __atomic_load_n(&known, __ATOMIC_RELAXED);
  if (k) return k == 2;
  k = 1;
  if (&environ && environ)
    for (char **e = environ; *e; ++e) {
      const char *v = *e, *want = "VULKAN_1_DEBUG=";
      while (*want && *v == *want) ++v, ++want;
      if (!*want) k = 2;
    }
  __atomic_store_n(&known, k, __ATOMIC_RELAXED);
  return k == 2;
}

static void vk_os_trace(const char *text) {
  if (debug_wanted()) linux_call(SYS_WRITE, 2, (long)text, (long)cstrlen(text), 0, 0, 0);
}

static uint64_t vk_os_now_ns(void) {
  struct {
    int64_t seconds, nanoseconds;
  } t = {0, 0};
  if (linux_call(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t, 0, 0, 0, 0) != 0) return 0;
  return (uint64_t)t.seconds * 1000000000ull + (uint64_t)t.nanoseconds;
}

/* ---- the D3DKMT services: ioctl on /dev/dxg ---------------------------------------------------------------------------------------------------------------------------- */
#define STATUS_NO_SUCH_DEVICE_KMT ((KmtStatus)0xC000000Eu)
#define STATUS_INVALID_PARAMETER_KMT ((KmtStatus)0xC000000Du)
static int g_dxg = -1;      /* the descriptor, opened at the first call and kept for the life of the process */

static int dxg_descriptor(void) {
  int fd = __atomic_load_n(&g_dxg, __ATOMIC_ACQUIRE);
  if (fd >= 0) return fd;
  long opened = linux_call(SYS_OPENAT, AT_FDCWD, (long)TCOS_DXG_DEVICE_PATH, O_RDWR | O_CLOEXEC, 0, 0, 0);
  if (opened < 0) return -1;
  int expected = -1;
  if (!__atomic_compare_exchange_n(&g_dxg, &expected, (int)opened, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    linux_call(SYS_CLOSE, opened, 0, 0, 0, 0, 0);      /* (another thread opened it first) */
    return expected;
  }
  return (int)opened;
}

static KmtStatus kmt(uint32_t service, void *params) {
  const int fd = dxg_descriptor();
  if (fd < 0) return STATUS_NO_SUCH_DEVICE_KMT;
  TcosDxgCall call = {service, 0, (uint64_t)(uintptr_t)params};
  const long r = linux_call(SYS_IOCTL, fd, (long)TCOS_DXG_IOCTL_CALL, (long)&call, 0, 0, 0);
  if (r != 0) return r == -19 ? STATUS_NO_SUCH_DEVICE_KMT : STATUS_INVALID_PARAMETER_KMT;      /* (-ENODEV: the system has no Direct3D layer) */
  return call.status;
}

/* ---- images as dmabufs (TCOS_DXG_IOCTL_EXPORT / IMPORT) --------------------------------------------------------------------------------------------------------------- */
static bool vk_os_export_fd(uint32_t allocation, int *fd) {
  const int dxg = dxg_descriptor();
  if (dxg < 0) return false;
  TcosDxgExport e = {allocation, TCOS_DXG_EXPORT_CLOEXEC, -1, 0};
  if (linux_call(SYS_IOCTL, dxg, (long)TCOS_DXG_IOCTL_EXPORT, (long)&e, 0, 0, 0) != 0 || !STATUS_OK(e.status) || e.fd < 0) {
    vk_os_trace("vkGetMemoryFdKHR: /dev/dxg would not export the image\n");
    return false;
  }
  *fd = e.fd;
  return true;
}

static bool vk_os_import_fd(int fd, uint32_t device, uint32_t *allocation, uint32_t *width, uint32_t *height, uint32_t *format, uint32_t *pitch) {
  const int dxg = dxg_descriptor();
  if (dxg < 0) return false;
  TcosDxgImport i;
  memset(&i, 0, sizeof i);
  i.fd = fd, i.device = device;
  if (linux_call(SYS_IOCTL, dxg, (long)TCOS_DXG_IOCTL_IMPORT, (long)&i, 0, 0, 0) != 0 || !STATUS_OK(i.status) || i.allocation == 0) {
    vk_os_trace("vkAllocateMemory: /dev/dxg would not import the descriptor\n");
    return false;
  }
  *allocation = i.allocation, *width = i.width, *height = i.height, *format = i.format, *pitch = i.pitch;
  return true;
}

static void vk_os_close_fd(int fd) { linux_call(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }

/* ---- VK_EXT_headless_surface -------------------------------------------------------------------------------------------------------------------------------------------- */
enum { SURFACE_HEADLESS = 1 };

VKAPI VkResult VKAPI_CALL vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)instance, (void)ci, (void)alloc;
  return make_surface(0, SURFACE_HEADLESS, out);      /* (no window: a present flips the image onto the display) */
}

static bool vk_os_surface_extent(const Surface *s, uint32_t *w, uint32_t *h) {
  (void)s, (void)w, (void)h;
  return false;      /* (the program picks the swapchain's size) */
}
