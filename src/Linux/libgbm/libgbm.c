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

/* libgbm for Linux programs on TheoryCraftOS: the Generic Buffer Manager (Mesa's gbm.h, the same ABI) over the kernel's Direct3D layer, reached through /dev/dxg
 * (src/Process/DxgDevice.h). Installed as /usr/lib/libgbm.so.1.
 *
 * A device is a D3DKMT device on the first adapter, made on the descriptor the program passes (/dev/dxg; the system has no /dev/dri). A buffer object is an
 * allocation of that device made shareable and exported once as a dmabuf (TCOS_DXG_IOCTL_EXPORT): a 2D image, linear, one plane at offset 0, in one of the four
 * 8-bit RGB formats (ARGB8888, XRGB8888, ABGR8888, XBGR8888). gbm_bo_get_fd gives a duplicate of that dmabuf, which the Wayland compositor shows in place
 * (zwp_linux_dmabuf_v1) and which another process can import. gbm_bo_import takes a dmabuf of /dev/dxg (GBM_BO_IMPORT_FD, or _FD_MODIFIER with the linear
 * modifier and one plane) and checks its size and format against the image's. gbm_bo_map maps the dmabuf (the kernel's CPU copy of the image) between
 * DMA_BUF_IOCTL_SYNC start and end, so what the CPU writes reaches the image at unmap and what the GPU drew is there at map.
 *
 * A gbm_surface is up to three buffers that an EGL window surface (libEGL.so.1 of src/Linux/libegl, EGL_KHR_platform_gbm) draws its frames into: each swap
 * copies the frame into a buffer nobody holds, and gbm_surface_lock_front_buffer gives the newest frame to the program until gbm_surface_release_buffer. The
 * private functions of tcos_gbm.h are how libEGL reaches them. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <gbm.h>

#include "../../Process/DxgDevice.h"
#include "tcos_gbm.h"

#define GBM_EXPORT __attribute__((visibility("default")))

/* DRM formats (drm_fourcc.h) and the linear modifier */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_ARGB8888 FOURCC('A', 'R', '2', '4')
#define FMT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define FMT_ABGR8888 FOURCC('A', 'B', '2', '4')
#define FMT_XBGR8888 FOURCC('X', 'B', '2', '4')
#define MOD_LINEAR 0ull
#define MOD_INVALID 0x00ffffffffffffffull

/* dxgk::Format and dxgk::AllocationFlags (src/Kernel/Dxgkrnl/DxgTypes.hpp), the words of an allocation's private data (src/Vulkan/vk_instance.inc) */
enum { DF_A8R8G8B8 = 1, DF_X8R8G8B8, DF_A8B8G8R8, DF_X8B8G8R8 };
enum { A_TEXTURE = 1u << 2, A_TARGET = 1u << 3, A_SHARED = 1u << 7 };
enum { AW_VERSION = 0, AW_FLAGS, AW_FORMAT, AW_WIDTH, AW_HEIGHT, AW_MIPS, AW_SIZE_LO, AW_SIZE_HI, AW_KIND, AW_SAMPLES, AW_LAYERS, AW_STATE, AW_HEAP, AW_HEAP_LO, AW_HEAP_HI,
       AW_PITCH, AW_BYTES_LO, AW_BYTES_HI, AW_WORDS };
enum { MAX_DIMENSION = 4096, SURFACE_BUFFERS = 3 };

/* DMA_BUF_IOCTL_SYNC (linux/dma-buf.h) */
#define DMA_BUF_SYNC_READ 1ull
#define DMA_BUF_SYNC_WRITE 2ull
#define DMA_BUF_SYNC_END 4ull
#define DMA_BUF_IOCTL_SYNC 0x40086200ul

struct gbm_device {
  struct gbm_device *(*create)(int fd);      /* gbm_create_device, as Mesa's device starts (what some EGLs look at to tell a gbm_device) */
  struct gbm_device *next;
  int fd;
  uint32_t adapter, device;
};

struct gbm_bo {
  struct gbm_device *gbm;
  uint32_t width, height, format, stride, flags;
  uint32_t allocation;      /* the device's allocation handle (gbm_bo_get_handle) */
  int fd;                   /* the dmabuf */
  size_t bytes;             /* the dmabuf's size (its CPU copy) */
  void *user_data;
  void (*destroy_user_data)(struct gbm_bo *, void *);
  struct gbm_surface *surface;      /* a buffer of a surface: freed with it */
};

enum { SLOT_FREE = 0, SLOT_DRAWING, SLOT_QUEUED, SLOT_LOCKED };
struct gbm_surface {
  struct gbm_surface *next;
  struct gbm_device *gbm;
  uint32_t width, height, format, flags;
  pthread_mutex_t lock;
  struct gbm_bo *bo[SURFACE_BUFFERS];
  int state[SURFACE_BUFFERS];
  uint64_t queued_at[SURFACE_BUFFERS];      /* the order frames were queued in: the newest is the front */
  uint64_t frames;
};

/* What is live, so that a stale or foreign pointer is never followed (tcos_gbm.h, and the checks below). */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gbm_device *g_devices;
static struct gbm_surface *g_surfaces;

static void put32(uint8_t *p, uint32_t at, uint32_t v) { memcpy(p + at, &v, 4); }
static void put64(uint8_t *p, uint32_t at, uint64_t v) { memcpy(p + at, &v, 8); }
static uint32_t get32(const uint8_t *p, uint32_t at) {
  uint32_t v;
  memcpy(&v, p + at, 4);
  return v;
}

/* A D3DKMT service through /dev/dxg: its NTSTATUS, or -1 when the ioctl failed. */
static int32_t kmt(int fd, uint32_t service, void *params) {
  TcosDxgCall call;
  memset(&call, 0, sizeof call);
  call.service = service;
  call.params = (uint64_t)(uintptr_t)params;
  if (ioctl(fd, TCOS_DXG_IOCTL_CALL, &call) != 0) return -1;
  return call.status;
}
#define STATUS_OK(s) ((s) >= 0)

static uint32_t dxgk_format(uint32_t format) {
  switch (format) {
    case FMT_ARGB8888: case GBM_BO_FORMAT_ARGB8888: return DF_A8R8G8B8;
    case FMT_XRGB8888: case GBM_BO_FORMAT_XRGB8888: return DF_X8R8G8B8;
    case FMT_ABGR8888: return DF_A8B8G8R8;
    case FMT_XBGR8888: return DF_X8B8G8R8;
    default: return 0;
  }
}
/* The DRM format a program asked for (the two old GBM_BO_FORMAT_* numbers are the same as ARGB8888 / XRGB8888). */
static uint32_t drm_format(uint32_t format) {
  if (format == GBM_BO_FORMAT_ARGB8888) return FMT_ARGB8888;
  if (format == GBM_BO_FORMAT_XRGB8888) return FMT_XRGB8888;
  return format;
}
/* The kernel's format numbers that are the same picture as a DRM format: an alpha channel the image has but the format ignores is fine. */
static int same_layout(uint32_t drm, uint32_t kernel) {
  switch (drm) {
    case FMT_ARGB8888: return kernel == DF_A8R8G8B8;
    case FMT_XRGB8888: return kernel == DF_A8R8G8B8 || kernel == DF_X8R8G8B8;
    case FMT_ABGR8888: return kernel == DF_A8B8G8R8;
    case FMT_XBGR8888: return kernel == DF_A8B8G8R8 || kernel == DF_X8B8G8R8;
    default: return 0;
  }
}

static int live_device(const struct gbm_device *gbm) {
  pthread_mutex_lock(&g_lock);
  const struct gbm_device *d = g_devices;
  while (d && d != gbm) d = d->next;
  pthread_mutex_unlock(&g_lock);
  return d != NULL;
}

/* ---- devices */
GBM_EXPORT struct gbm_device *gbm_create_device(int fd) {
  if (fd < 0) { errno = EINVAL; return NULL; }
  uint8_t args[16];
  memset(args, 0, sizeof args);
  if (!STATUS_OK(kmt(fd, TCOS_DXG_ENUM_ADAPTERS2, args))) { errno = ENODEV; return NULL; }      /* (not /dev/dxg, or no Direct3D layer) */
  uint32_t n = get32(args, 0);
  if (n == 0 || n > 16) { errno = ENODEV; return NULL; }
  uint8_t infos[16 * 20];
  memset(infos, 0, sizeof infos);
  put64(args, 8, (uint64_t)(uintptr_t)infos);
  if (!STATUS_OK(kmt(fd, TCOS_DXG_ENUM_ADAPTERS2, args)) || get32(args, 0) == 0) { errno = ENODEV; return NULL; }
  struct gbm_device *gbm = (struct gbm_device *)calloc(1, sizeof *gbm);
  if (!gbm) { errno = ENOMEM; return NULL; }
  gbm->create = gbm_create_device;
  gbm->fd = fd;
  gbm->adapter = get32(infos, 0);
  uint8_t dev[64];
  memset(dev, 0, sizeof dev);
  put32(dev, 0, gbm->adapter);
  if (!STATUS_OK(kmt(fd, TCOS_DXG_CREATE_DEVICE, dev))) {
    free(gbm);
    errno = ENODEV;
    return NULL;
  }
  gbm->device = get32(dev, 12);
  pthread_mutex_lock(&g_lock);
  gbm->next = g_devices;
  g_devices = gbm;
  pthread_mutex_unlock(&g_lock);
  return gbm;
}

GBM_EXPORT void gbm_device_destroy(struct gbm_device *gbm) {
  if (!gbm) return;
  pthread_mutex_lock(&g_lock);
  struct gbm_device **at = &g_devices;
  while (*at && *at != gbm) at = &(*at)->next;
  const int found = *at != NULL;
  if (found) *at = gbm->next;
  pthread_mutex_unlock(&g_lock);
  if (!found) return;
  uint8_t a[4];
  put32(a, 0, gbm->device);
  kmt(gbm->fd, TCOS_DXG_DESTROY_DEVICE, a);      /* (its allocations go with it; the buffers' dmabufs hold their images on their own) */
  put32(a, 0, gbm->adapter);
  kmt(gbm->fd, TCOS_DXG_CLOSE_ADAPTER, a);
  free(gbm);
}

GBM_EXPORT int gbm_device_get_fd(struct gbm_device *gbm) { return gbm ? gbm->fd : -1; }
GBM_EXPORT const char *gbm_device_get_backend_name(struct gbm_device *gbm) { return "tcos"; }

GBM_EXPORT int gbm_device_is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t flags) {
  if (!gbm || !dxgk_format(format)) return 0;
  /* no cursor planes and no protected memory; everything else is the same image */
  return (flags & (GBM_BO_USE_CURSOR | GBM_BO_USE_PROTECTED)) == 0;
}

GBM_EXPORT int gbm_device_get_format_modifier_plane_count(struct gbm_device *gbm, uint32_t format, uint64_t modifier) {
  if (!gbm || !dxgk_format(format) || (modifier != MOD_LINEAR && modifier != MOD_INVALID)) return -1;
  return 1;
}

/* ---- buffer objects */
static void destroy_allocation(struct gbm_device *gbm, uint32_t allocation) {
  if (!allocation) return;
  uint8_t args[32];
  uint32_t list[1] = {allocation};
  memset(args, 0, sizeof args);
  put32(args, 0, gbm->device);
  put64(args, 8, (uint64_t)(uintptr_t)list);
  put32(args, 16, 1);
  kmt(gbm->fd, TCOS_DXG_DESTROY_ALLOCATION, args);
}

static size_t dmabuf_size(int fd) {
  const off_t end = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  return end > 0 ? (size_t)end : 0;
}

static struct gbm_bo *make_bo(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, uint32_t flags) {
  if (!gbm || !live_device(gbm)) { errno = EINVAL; return NULL; }
  format = drm_format(format);
  const uint32_t kernel_format = dxgk_format(format);
  if (!kernel_format || width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION) { errno = EINVAL; return NULL; }
  if (flags & (GBM_BO_USE_CURSOR | GBM_BO_USE_PROTECTED)) { errno = EINVAL; return NULL; }
  uint32_t w[AW_WORDS];
  memset(w, 0, sizeof w);
  w[AW_VERSION] = 1;
  w[AW_FLAGS] = A_TEXTURE | A_TARGET | A_SHARED;      /* (drawn into, sampled by the compositor or another process, exported) */
  w[AW_FORMAT] = kernel_format;
  w[AW_WIDTH] = width;
  w[AW_HEIGHT] = height;
  w[AW_MIPS] = 1;
  w[AW_SAMPLES] = 1;
  w[AW_LAYERS] = 1;
  uint8_t info[40], args[72];
  memset(info, 0, sizeof info);
  memset(args, 0, sizeof args);
  put64(info, 16, (uint64_t)(uintptr_t)w);
  put32(info, 24, sizeof w);
  put32(args, 0, gbm->device);
  put32(args, 44, 1);
  put64(args, 48, (uint64_t)(uintptr_t)info);
  if (!STATUS_OK(kmt(gbm->fd, TCOS_DXG_CREATE_ALLOCATION, args)) || get32(info, 0) == 0) { errno = ENOMEM; return NULL; }
  const uint32_t allocation = get32(info, 0);
  TcosDxgExport ex;
  memset(&ex, 0, sizeof ex);
  ex.allocation = allocation;
  ex.flags = TCOS_DXG_EXPORT_CLOEXEC;
  if (ioctl(gbm->fd, TCOS_DXG_IOCTL_EXPORT, &ex) != 0 || !STATUS_OK(ex.status) || ex.fd < 0) {
    destroy_allocation(gbm, allocation);
    errno = ENOMEM;
    return NULL;
  }
  struct gbm_bo *bo = (struct gbm_bo *)calloc(1, sizeof *bo);
  if (!bo) {
    close(ex.fd);
    destroy_allocation(gbm, allocation);
    errno = ENOMEM;
    return NULL;
  }
  bo->gbm = gbm;
  bo->width = width;
  bo->height = height;
  bo->format = format;
  bo->flags = flags;
  bo->stride = w[AW_PITCH] ? w[AW_PITCH] : width * 4;
  bo->allocation = allocation;
  bo->fd = ex.fd;
  bo->bytes = dmabuf_size(ex.fd);
  return bo;
}

GBM_EXPORT struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, uint32_t flags) {
  return make_bo(gbm, width, height, format, flags);
}

/* Only the linear layout: a list without it (or with only the "no modifier" value) is ENOSYS / EINVAL as in Mesa when no modifier fits. */
static int has_linear(const uint64_t *modifiers, unsigned count) {
  for (unsigned i = 0; modifiers && i < count; ++i)
    if (modifiers[i] == MOD_LINEAR || modifiers[i] == MOD_INVALID) return 1;
  return 0;
}

GBM_EXPORT struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers,
                                                        const unsigned int count, uint32_t flags) {
  if (count > 0 && !has_linear(modifiers, count)) { errno = EINVAL; return NULL; }
  return make_bo(gbm, width, height, format, flags);
}
GBM_EXPORT struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers,
                                                       const unsigned int count) {
  return gbm_bo_create_with_modifiers2(gbm, width, height, format, modifiers, count, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
}

static struct gbm_bo *import_fd(struct gbm_device *gbm, int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t format, uint32_t flags) {
  format = drm_format(format);
  if (fd < 0 || !dxgk_format(format) || width == 0 || height == 0) { errno = EINVAL; return NULL; }
  TcosDxgImport im;
  memset(&im, 0, sizeof im);
  im.fd = fd;
  im.device = gbm->device;
  if (ioctl(gbm->fd, TCOS_DXG_IOCTL_IMPORT, &im) != 0 || !STATUS_OK(im.status) || !im.allocation) { errno = EINVAL; return NULL; }      /* (not a dmabuf of /dev/dxg) */
  /* the description must be the image's: the same size, format and rows */
  if (im.width != width || im.height != height || !same_layout(format, im.format) || (stride && stride != im.pitch)) {
    destroy_allocation(gbm, im.allocation);
    errno = EINVAL;
    return NULL;
  }
  struct gbm_bo *bo = (struct gbm_bo *)calloc(1, sizeof *bo);
  const int dup_fd = bo ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
  if (!bo || dup_fd < 0) {
    free(bo);
    destroy_allocation(gbm, im.allocation);
    errno = ENOMEM;
    return NULL;
  }
  bo->gbm = gbm;
  bo->width = width;
  bo->height = height;
  bo->format = format;
  bo->flags = flags;
  bo->stride = im.pitch;
  bo->allocation = im.allocation;
  bo->fd = dup_fd;
  bo->bytes = dmabuf_size(dup_fd);
  return bo;
}

GBM_EXPORT struct gbm_bo *gbm_bo_import(struct gbm_device *gbm, uint32_t type, void *buffer, uint32_t flags) {
  if (!gbm || !buffer || !live_device(gbm)) { errno = EINVAL; return NULL; }
  if (type == GBM_BO_IMPORT_FD) {
    const struct gbm_import_fd_data *d = (const struct gbm_import_fd_data *)buffer;
    return import_fd(gbm, d->fd, d->width, d->height, d->stride, d->format, flags);
  }
  if (type == GBM_BO_IMPORT_FD_MODIFIER) {
    const struct gbm_import_fd_modifier_data *d = (const struct gbm_import_fd_modifier_data *)buffer;
    if (d->num_fds != 1 || d->offsets[0] != 0 || d->strides[0] <= 0 || (d->modifier != MOD_LINEAR && d->modifier != MOD_INVALID)) { errno = EINVAL; return NULL; }
    return import_fd(gbm, d->fds[0], d->width, d->height, (uint32_t)d->strides[0], d->format, flags);
  }
  errno = ENOSYS;      /* (no wl_buffer or EGLImage imports) */
  return NULL;
}

static void free_bo(struct gbm_bo *bo) {
  if (bo->destroy_user_data) bo->destroy_user_data(bo, bo->user_data);
  if (live_device(bo->gbm)) destroy_allocation(bo->gbm, bo->allocation);
  if (bo->fd >= 0) close(bo->fd);
  free(bo);
}

GBM_EXPORT void gbm_bo_destroy(struct gbm_bo *bo) {
  if (!bo || bo->surface) return;      /* (a surface's buffers go with the surface) */
  free_bo(bo);
}

GBM_EXPORT uint32_t gbm_bo_get_width(struct gbm_bo *bo) { return bo ? bo->width : 0; }
GBM_EXPORT uint32_t gbm_bo_get_height(struct gbm_bo *bo) { return bo ? bo->height : 0; }
GBM_EXPORT uint32_t gbm_bo_get_stride(struct gbm_bo *bo) { return bo ? bo->stride : 0; }
GBM_EXPORT uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) { return bo && plane == 0 ? bo->stride : 0; }
GBM_EXPORT uint32_t gbm_bo_get_format(struct gbm_bo *bo) { return bo ? bo->format : 0; }
GBM_EXPORT uint32_t gbm_bo_get_bpp(struct gbm_bo *bo) { return bo ? 32 : 0; }
GBM_EXPORT uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) { return 0; }
GBM_EXPORT struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo) { return bo ? bo->gbm : NULL; }
GBM_EXPORT uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) { return MOD_LINEAR; }
GBM_EXPORT int gbm_bo_get_plane_count(struct gbm_bo *bo) { return bo ? 1 : 0; }
GBM_EXPORT union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
  union gbm_bo_handle h;
  memset(&h, 0, sizeof h);
  if (bo) h.u32 = bo->allocation;
  return h;
}
GBM_EXPORT union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane) {
  union gbm_bo_handle h;
  memset(&h, 0, sizeof h);
  if (bo && plane == 0) h.u32 = bo->allocation;
  else h.s32 = -1;
  return h;
}
GBM_EXPORT int gbm_bo_get_fd(struct gbm_bo *bo) { return bo ? fcntl(bo->fd, F_DUPFD_CLOEXEC, 0) : -1; }
GBM_EXPORT int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) { return plane == 0 ? gbm_bo_get_fd(bo) : -1; }

GBM_EXPORT void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy_user_data)(struct gbm_bo *, void *)) {
  if (!bo) return;
  bo->user_data = data;
  bo->destroy_user_data = destroy_user_data;
}
GBM_EXPORT void *gbm_bo_get_user_data(struct gbm_bo *bo) { return bo ? bo->user_data : NULL; }

/* A mapping: the whole dmabuf mapped, synced for what the program said it does. */
typedef struct MapData {
  void *base;
  size_t bytes;
  uint64_t sync;
  int fd;
} MapData;

static int dma_sync(int fd, uint64_t flags) {
  uint64_t f = flags;
  int r;
  do r = ioctl(fd, DMA_BUF_IOCTL_SYNC, &f);
  while (r != 0 && (errno == EAGAIN || errno == EINTR));      /* (busy: the GPU still works on the image) */
  return r;
}

GBM_EXPORT void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t flags, uint32_t *stride, void **map_data) {
  if (!bo || !map_data || !stride || width == 0 || height == 0 || x > bo->width || width > bo->width - x || y > bo->height || height > bo->height - y ||
      !(flags & GBM_BO_TRANSFER_READ_WRITE)) {
    errno = EINVAL;
    return NULL;
  }
  if (bo->bytes < (size_t)bo->stride * bo->height) { errno = EINVAL; return NULL; }
  MapData *m = (MapData *)calloc(1, sizeof *m);
  if (!m) { errno = ENOMEM; return NULL; }
  m->bytes = bo->bytes;
  m->fd = bo->fd;
  m->sync = ((flags & GBM_BO_TRANSFER_READ) ? DMA_BUF_SYNC_READ : 0) | ((flags & GBM_BO_TRANSFER_WRITE) ? DMA_BUF_SYNC_WRITE : 0);
  m->base = mmap(NULL, m->bytes, PROT_READ | ((flags & GBM_BO_TRANSFER_WRITE) ? PROT_WRITE : 0), MAP_SHARED, bo->fd, 0);
  if (m->base == MAP_FAILED) {
    free(m);
    return NULL;
  }
  if (dma_sync(bo->fd, m->sync) != 0) {
    munmap(m->base, m->bytes);
    free(m);
    return NULL;
  }
  *stride = bo->stride;
  *map_data = m;
  return (uint8_t *)m->base + (size_t)y * bo->stride + (size_t)x * 4;
}

GBM_EXPORT void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
  MapData *m = (MapData *)map_data;
  if (!bo || !m) return;
  dma_sync(m->fd, m->sync | DMA_BUF_SYNC_END);      /* (with WRITE: the CPU's pixels go to the image) */
  munmap(m->base, m->bytes);
  free(m);
}

GBM_EXPORT int gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count) {
  if (!bo || (!buf && count)) { errno = EINVAL; return -1; }
  if (count > (size_t)bo->stride * bo->height) { errno = EINVAL; return -1; }
  uint32_t stride = 0;
  void *data = NULL;
  uint8_t *p = (uint8_t *)gbm_bo_map(bo, 0, 0, bo->width, bo->height, GBM_BO_TRANSFER_WRITE, &stride, &data);
  if (!p) return -1;
  memcpy(p, buf, count);
  gbm_bo_unmap(bo, data);
  return 0;
}

/* ---- surfaces */
static struct gbm_surface *live_surface(const void *native) {
  pthread_mutex_lock(&g_lock);
  struct gbm_surface *s = g_surfaces;
  while (s && s != (const struct gbm_surface *)native) s = s->next;
  pthread_mutex_unlock(&g_lock);
  return s;
}

GBM_EXPORT struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers,
                                                                  const unsigned int count, uint32_t flags) {
  if (!gbm || !live_device(gbm)) { errno = EINVAL; return NULL; }
  format = drm_format(format);
  if (!dxgk_format(format) || width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION || (count > 0 && !has_linear(modifiers, count))) {
    errno = EINVAL;
    return NULL;
  }
  struct gbm_surface *s = (struct gbm_surface *)calloc(1, sizeof *s);
  if (!s) { errno = ENOMEM; return NULL; }
  s->gbm = gbm;
  s->width = width;
  s->height = height;
  s->format = format;
  s->flags = flags;
  pthread_mutex_init(&s->lock, NULL);
  pthread_mutex_lock(&g_lock);
  s->next = g_surfaces;
  g_surfaces = s;
  pthread_mutex_unlock(&g_lock);
  return s;
}
GBM_EXPORT struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers,
                                                                 const unsigned int count) {
  return gbm_surface_create_with_modifiers2(gbm, width, height, format, modifiers, count, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
}
GBM_EXPORT struct gbm_surface *gbm_surface_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, uint32_t flags) {
  return gbm_surface_create_with_modifiers2(gbm, width, height, format, NULL, 0, flags);
}

GBM_EXPORT void gbm_surface_destroy(struct gbm_surface *surface) {
  pthread_mutex_lock(&g_lock);
  struct gbm_surface **at = &g_surfaces;
  while (*at && *at != surface) at = &(*at)->next;
  const int found = *at != NULL;
  if (found) *at = surface->next;
  pthread_mutex_unlock(&g_lock);
  if (!found) return;
  for (int i = 0; i < SURFACE_BUFFERS; ++i)
    if (surface->bo[i]) free_bo(surface->bo[i]);
  pthread_mutex_destroy(&surface->lock);
  free(surface);
}

GBM_EXPORT struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface) {
  if (!live_surface(surface)) return NULL;
  pthread_mutex_lock(&surface->lock);
  int newest = -1;
  for (int i = 0; i < SURFACE_BUFFERS; ++i)
    if (surface->state[i] == SLOT_QUEUED && (newest < 0 || surface->queued_at[i] > surface->queued_at[newest])) newest = i;
  struct gbm_bo *bo = NULL;
  if (newest >= 0) {
    surface->state[newest] = SLOT_LOCKED;
    bo = surface->bo[newest];
    for (int i = 0; i < SURFACE_BUFFERS; ++i)      /* (older frames that were never locked are let go) */
      if (surface->state[i] == SLOT_QUEUED) surface->state[i] = SLOT_FREE;
  }
  pthread_mutex_unlock(&surface->lock);
  return bo;
}

GBM_EXPORT void gbm_surface_release_buffer(struct gbm_surface *surface, struct gbm_bo *bo) {
  if (!bo || !live_surface(surface)) return;
  pthread_mutex_lock(&surface->lock);
  for (int i = 0; i < SURFACE_BUFFERS; ++i)
    if (surface->bo[i] == bo && surface->state[i] == SLOT_LOCKED) surface->state[i] = SLOT_FREE;
  pthread_mutex_unlock(&surface->lock);
}

GBM_EXPORT int gbm_surface_has_free_buffers(struct gbm_surface *surface) {
  if (!live_surface(surface)) return 0;
  pthread_mutex_lock(&surface->lock);
  int free_slots = 0;
  for (int i = 0; i < SURFACE_BUFFERS; ++i) free_slots += surface->state[i] == SLOT_FREE;
  pthread_mutex_unlock(&surface->lock);
  return free_slots > 0;
}

GBM_EXPORT char *gbm_format_get_name(uint32_t gbm_format, struct gbm_format_name_desc *desc) {
  if (!desc) return NULL;
  const uint32_t f = drm_format(gbm_format);
  for (int i = 0; i < 4; ++i) desc->name[i] = (char)((f >> (8 * i)) & 0xFF);
  desc->name[4] = 0;
  return desc->name;
}

/* ---- for libEGL (tcos_gbm.h) */
GBM_EXPORT int tcos_gbm_is_device(const void *native) { return native && live_device((const struct gbm_device *)native); }

GBM_EXPORT int tcos_gbm_surface_info(const void *native, uint32_t *width, uint32_t *height, uint32_t *format) {
  struct gbm_surface *s = native ? live_surface(native) : NULL;
  if (!s) return 0;
  if (width) *width = s->width;
  if (height) *height = s->height;
  if (format) *format = s->format;
  return 1;
}

GBM_EXPORT struct gbm_bo *tcos_gbm_surface_take(struct gbm_surface *surface) {
  if (!live_surface(surface)) return NULL;
  pthread_mutex_lock(&surface->lock);
  struct gbm_bo *bo = NULL;
  for (int i = 0; i < SURFACE_BUFFERS && !bo; ++i) {
    if (surface->state[i] != SLOT_FREE) continue;
    if (!surface->bo[i]) {
      surface->bo[i] = make_bo(surface->gbm, surface->width, surface->height, surface->format, surface->flags);
      if (!surface->bo[i]) break;
      surface->bo[i]->surface = surface;
    }
    surface->state[i] = SLOT_DRAWING;
    bo = surface->bo[i];
  }
  pthread_mutex_unlock(&surface->lock);
  return bo;
}

static void set_state(struct gbm_surface *surface, struct gbm_bo *bo, int from, int to) {
  if (!bo || !live_surface(surface)) return;
  pthread_mutex_lock(&surface->lock);
  for (int i = 0; i < SURFACE_BUFFERS; ++i)
    if (surface->bo[i] == bo && surface->state[i] == from) {
      surface->state[i] = to;
      if (to == SLOT_QUEUED) surface->queued_at[i] = ++surface->frames;
    }
  pthread_mutex_unlock(&surface->lock);
}
GBM_EXPORT void tcos_gbm_surface_queue(struct gbm_surface *surface, struct gbm_bo *bo) { set_state(surface, bo, SLOT_DRAWING, SLOT_QUEUED); }
GBM_EXPORT void tcos_gbm_surface_untake(struct gbm_surface *surface, struct gbm_bo *bo) { set_state(surface, bo, SLOT_DRAWING, SLOT_FREE); }
