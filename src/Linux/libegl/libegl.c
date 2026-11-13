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

/* libEGL for Linux programs on TheoryCraftOS: EGL 1.5, OpenGL ES 2.0/3.0 and OpenGL 3.3 over the kernel's OpenGL (src/OpenGl, docs/OpenGL.md), the same GL that
 * opengl32.dll gives Windows programs. Every GL command is encoded into the calling thread's command buffer by the generated entry points
 * (src/OpenGl/generated/GlEntries.inc) and runs in the kernel, reached with /dev/dxg's TCOS_DXG_IOCTL_GL (src/Process/DxgDevice.h) instead of NtTcosOpenGl.
 *
 * Displays: the Wayland platform (EGL_KHR_platform_wayland; also eglGetDisplay of a wl_display), the GBM one (EGL_KHR_platform_gbm, EGL_MESA_platform_gbm;
 * also eglGetDisplay of a gbm_device of libgbm.so.1, src/Linux/libgbm) and the surfaceless one (EGL_MESA_platform_surfaceless, and EGL_DEFAULT_DISPLAY):
 * pbuffers and no surface at all.
 *
 * A Wayland window surface is a wl_egl_window (libwayland-egl): eglSwapBuffers swaps the kernel's images and copies the new front image into a buffer that it
 * attaches, damages and commits; with a swap interval of 1 it first waits for the frame callback of the frame before. When the compositor has
 * zwp_linux_dmabuf_v1 the buffers are GPU images (gbm buffer objects of /dev/dxg made wl_buffers with create_immed): the kernel copies the front image into
 * the image itself (FRONT_TO_BUFFER, src/Process/WindowsOpenGl.hpp) and the compositor draws it in place. Otherwise (or with TCOS_EGL_SHM=1 in the environment)
 * they are wl_shm buffers the front image is read straight into. Each window surface keeps up to three buffers and its own event queue, so the program's own
 * queue and other threads never see its events. The wl_egl_window's resize takes effect at once when the surface is current on the calling thread, else at
 * the next swap.
 *
 * A GBM window surface is a gbm_surface: each swap copies the front image into one of its buffers nobody holds (in the kernel, as above), which
 * gbm_surface_lock_front_buffer then gives the program. With no free buffer the swap is EGL_BAD_ALLOC (the program must release one first).
 *
 * Images: EGL_KHR_image_base with EGL_EXT_image_dma_buf_import (one plane, linear, ARGB8888 / XRGB8888 / ABGR8888 / XBGR8888, a dmabuf of /dev/dxg or any
 * mappable file) and glEGLImageTargetTexture2DOES (GL_OES_EGL_image). The texture takes the image's pixels when glEGLImageTargetTexture2DOES is called (a copy
 * through the dmabuf's mapping): it does not follow later changes of the image, as a texture sharing its memory would.
 *
 * The library is installed as /usr/lib/libEGL.so.1; libGLESv2.so.2, libGL.so.1 and libOpenGL.so.0 are empty libraries that need it (build.sh), so a program
 * linked against any of them, or one that opens them with dlopen and looks commands up with dlsym, reaches the one copy of the state here. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl-backend.h>
#include <gbm.h>
#include "../libgbm/tcos_gbm.h"

#define GLAPI __attribute__((visibility("default")))
#define APIENTRY
#include "../../OpenGl/GlClient.h"
#include "../../Process/DxgDevice.h"

#define EGLAPI __attribute__((visibility("default")))

/* ---- EGL's types and numbers (the ones this library uses, as Khronos' egl.h and eglext.h define them) */
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLClientBuffer;
typedef void *EGLSync;
typedef void *EGLImage;
typedef uint64_t EGLTime;
typedef void (*EGLProc)(void);

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_DONT_CARE ((EGLint)-1)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_SYNC ((EGLSync)0)
#define EGL_NO_IMAGE ((EGLImage)0)
#define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_FOREVER 0xFFFFFFFFFFFFFFFFull
enum {
  EGL_SUCCESS = 0x3000, EGL_NOT_INITIALIZED, EGL_BAD_ACCESS, EGL_BAD_ALLOC, EGL_BAD_ATTRIBUTE, EGL_BAD_CONFIG, EGL_BAD_CONTEXT, EGL_BAD_CURRENT_SURFACE, EGL_BAD_DISPLAY,
  EGL_BAD_MATCH, EGL_BAD_NATIVE_PIXMAP, EGL_BAD_NATIVE_WINDOW, EGL_BAD_PARAMETER, EGL_BAD_SURFACE, EGL_CONTEXT_LOST,
  EGL_BUFFER_SIZE = 0x3020, EGL_ALPHA_SIZE, EGL_BLUE_SIZE, EGL_GREEN_SIZE, EGL_RED_SIZE, EGL_DEPTH_SIZE, EGL_STENCIL_SIZE, EGL_CONFIG_CAVEAT, EGL_CONFIG_ID, EGL_LEVEL,
  EGL_MAX_PBUFFER_HEIGHT, EGL_MAX_PBUFFER_PIXELS, EGL_MAX_PBUFFER_WIDTH, EGL_NATIVE_RENDERABLE, EGL_NATIVE_VISUAL_ID, EGL_NATIVE_VISUAL_TYPE, EGL_SAMPLES = 0x3031,
  EGL_SAMPLE_BUFFERS, EGL_SURFACE_TYPE, EGL_TRANSPARENT_TYPE, EGL_TRANSPARENT_BLUE_VALUE, EGL_TRANSPARENT_GREEN_VALUE, EGL_TRANSPARENT_RED_VALUE, EGL_NONE,
  EGL_BIND_TO_TEXTURE_RGB, EGL_BIND_TO_TEXTURE_RGBA, EGL_MIN_SWAP_INTERVAL, EGL_MAX_SWAP_INTERVAL, EGL_LUMINANCE_SIZE, EGL_ALPHA_MASK_SIZE, EGL_COLOR_BUFFER_TYPE,
  EGL_RENDERABLE_TYPE, EGL_MATCH_NATIVE_PIXMAP, EGL_CONFORMANT,
  EGL_SLOW_CONFIG = 0x3050, EGL_NON_CONFORMANT_CONFIG, EGL_TRANSPARENT_RGB, EGL_VENDOR, EGL_VERSION, EGL_EXTENSIONS,
  EGL_HEIGHT = 0x3056, EGL_WIDTH, EGL_LARGEST_PBUFFER, EGL_DRAW, EGL_READ, EGL_CORE_NATIVE_ENGINE, EGL_NO_TEXTURE, EGL_TEXTURE_RGB, EGL_TEXTURE_RGBA, EGL_TEXTURE_2D,
  EGL_TEXTURE_FORMAT = 0x3080, EGL_TEXTURE_TARGET, EGL_MIPMAP_TEXTURE, EGL_MIPMAP_LEVEL, EGL_BACK_BUFFER, EGL_SINGLE_BUFFER, EGL_RENDER_BUFFER, EGL_VG_COLORSPACE,
  EGL_VG_ALPHA_FORMAT, EGL_COLORSPACE_SRGB, EGL_COLORSPACE_LINEAR, EGL_VG_ALPHA_FORMAT_NONPRE, EGL_VG_ALPHA_FORMAT_PRE, EGL_CLIENT_APIS, EGL_RGB_BUFFER,
  EGL_LUMINANCE_BUFFER, EGL_HORIZONTAL_RESOLUTION, EGL_VERTICAL_RESOLUTION, EGL_PIXEL_ASPECT_RATIO, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED, EGL_BUFFER_DESTROYED,
  EGL_OPENVG_IMAGE, EGL_CONTEXT_CLIENT_TYPE, EGL_CONTEXT_CLIENT_VERSION, EGL_MULTISAMPLE_RESOLVE, EGL_MULTISAMPLE_RESOLVE_DEFAULT, EGL_MULTISAMPLE_RESOLVE_BOX,
  EGL_OPENGL_ES_API = 0x30A0, EGL_OPENVG_API, EGL_OPENGL_API,
  EGL_CONTEXT_MINOR_VERSION = 0x30FB, EGL_CONTEXT_FLAGS_KHR = 0x30FC, EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD, EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY = 0x31BD,
  EGL_CONTEXT_OPENGL_DEBUG = 0x31B0, EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE = 0x31B1, EGL_CONTEXT_OPENGL_ROBUST_ACCESS = 0x31B2, EGL_NO_RESET_NOTIFICATION = 0x31BE,
  EGL_LOSE_CONTEXT_ON_RESET = 0x31BF, EGL_GL_COLORSPACE = 0x309D,
  EGL_SYNC_PRIOR_COMMANDS_COMPLETE = 0x30F0, EGL_SYNC_STATUS = 0x30F1, EGL_SIGNALED = 0x30F2, EGL_UNSIGNALED = 0x30F3, EGL_TIMEOUT_EXPIRED = 0x30F5,
  EGL_CONDITION_SATISFIED = 0x30F6, EGL_SYNC_TYPE = 0x30F7, EGL_SYNC_CONDITION = 0x30F8, EGL_SYNC_FENCE = 0x30F9,
  EGL_PLATFORM_GBM_KHR = 0x31D7, EGL_PLATFORM_WAYLAND_KHR = 0x31D8, EGL_PLATFORM_SURFACELESS_MESA = 0x31DD,
  EGL_LINUX_DMA_BUF_EXT = 0x3270, EGL_LINUX_DRM_FOURCC_EXT = 0x3271, EGL_DMA_BUF_PLANE0_FD_EXT = 0x3272, EGL_DMA_BUF_PLANE0_OFFSET_EXT = 0x3273,
  EGL_DMA_BUF_PLANE0_PITCH_EXT = 0x3274, EGL_DMA_BUF_PLANE1_FD_EXT = 0x3275, EGL_YUV_COLOR_SPACE_HINT_EXT = 0x327B, EGL_SAMPLE_RANGE_HINT_EXT = 0x327C,
  EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT = 0x327D, EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT = 0x327E, EGL_IMAGE_PRESERVED_KHR = 0x30D2,
  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT = 0x3443, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT = 0x3444,
};
enum { EGL_PBUFFER_BIT = 1, EGL_WINDOW_BIT = 4, EGL_OPENGL_ES_BIT = 1, EGL_OPENGL_ES2_BIT = 4, EGL_OPENGL_BIT = 8, EGL_OPENGL_ES3_BIT = 0x40 };
enum { EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 1, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT = 2 };
enum { EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR = 1, EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR = 2, EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR = 4 };
enum { EGL_SYNC_FLUSH_COMMANDS_BIT = 1 };

/* ---- The kernel's OpenGL: the 64-byte request of src/Process/WindowsOpenGl.hpp, sent with /dev/dxg's TCOS_DXG_IOCTL_GL */
typedef struct TcosGlRequest {
  uint32_t size, op, context, share;
  uint64_t window, data;
  uint32_t length, width, height, flags;
  uint64_t result;
  uint32_t major, minor;
} TcosGlRequest;
typedef char TcosGlRequestSize[sizeof(TcosGlRequest) == 64 ? 1 : -1];
enum { OP_CREATE = 1, OP_DELETE, OP_MAKE_CURRENT, OP_EXECUTE, OP_SWAP, OP_READ_FRONT, OP_SHARE_LISTS, OP_FLUSHED, OP_DESTROY_SURFACE, OP_FRONT_TO_BUFFER };
enum { SURFACE_DOUBLE = 1, SURFACE_DEPTH = 2, SURFACE_STENCIL = 4, SURFACE_ALPHA = 16 };
enum { CREATE_CORE = 1, CREATE_FORWARD = 2, CREATE_DEBUG = 4, CREATE_ES = 8 };
#define STATUS_RESOURCE_IN_USE ((int32_t)0xC0000708)

static int g_dxg = -1;
static pthread_once_t g_dxg_once = PTHREAD_ONCE_INIT;
static void open_dxg(void) { g_dxg = open(TCOS_DXG_DEVICE_PATH, O_RDWR | O_CLOEXEC); }

/* The request's NTSTATUS (negative: it failed); -1 (an NTSTATUS failure too) when the kernel has no OpenGL or /dev/dxg cannot be opened. */
static int32_t gl_call(TcosGlRequest *r) {
  pthread_once(&g_dxg_once, open_dxg);
  if (g_dxg < 0) return -1;
  r->size = sizeof(*r);
  TcosDxgGl call;
  memset(&call, 0, sizeof(call));
  call.request = (uint64_t)(uintptr_t)r;
  if (ioctl(g_dxg, TCOS_DXG_IOCTL_GL, &call) != 0) return -1;
  return call.status;
}

/* ---- Objects. Every handle given out is a pointer to one of these; a handle is checked against its display's lists before it is used, so a stale or made-up
 * handle is an EGL error, never a wild read. Objects are freed when destroyed and no thread has them current (EGL's "marked for deletion"). */
typedef struct EglDisplay EglDisplay;
typedef struct EglSurface EglSurface;
typedef struct EglContext EglContext;

typedef struct EglConfigInfo {
  EGLint id, alpha, depth, stencil;
  uint32_t fourcc;      /* EGL_NATIVE_VISUAL_ID: the DRM format of the window's buffers ('AR24' / 'XR24') */
} EglConfigInfo;
static const EglConfigInfo kConfigs[] = {
    {1, 8, 24, 8, 0x34325241u}, {2, 0, 24, 8, 0x34325258u}, {3, 8, 0, 0, 0x34325241u}, {4, 0, 0, 0, 0x34325258u}, {5, 8, 24, 0, 0x34325241u}, {6, 0, 24, 0, 0x34325258u},
};
#define CONFIG_COUNT ((EGLint)(sizeof(kConfigs) / sizeof(kConfigs[0])))
#define RENDERABLE_ALL (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT)

enum { BUFFER_SLOTS = 3 };
typedef struct ShmBuffer {
  struct wl_buffer *buffer;
  void *data;              /* a wl_shm buffer's mapping (null for a GPU buffer) */
  size_t bytes;
  struct gbm_bo *bo;       /* a GPU buffer: its image */
  int fd;                  /* ... and its dmabuf (-1: none) */
  uint32_t width, height;
  int busy;      /* attached and not yet released by the compositor */
} ShmBuffer;

struct EglSurface {
  EglSurface *next;
  EglDisplay *display;
  const EglConfigInfo *config;
  int window_surface;      /* else a pbuffer */
  int destroyed, bound;    /* destroyed: by eglDestroySurface (freed when `bound` drops to 0) */
  uint64_t kernel_window;  /* the kernel's name for its images */
  EGLint width, height;    /* a pbuffer's size; a window's last size given to the kernel */
  EGLint swap_interval;
  /* a window surface */
  struct wl_egl_window *native;
  struct wl_event_queue *queue;
  struct wl_surface *surface;      /* a wrapper of the window's wl_surface on `queue` */
  struct wl_shm *shm;              /* a wrapper of the display's wl_shm on `queue` */
  struct wl_callback *frame;       /* the frame callback of the last swap, until it is done */
  struct wl_proxy *dmabuf;         /* a wrapper of the display's zwp_linux_dmabuf_v1 on `queue` (null: wl_shm buffers) */
  int dx, dy;                      /* to move the next buffer by (wl_egl_window_resize) */
  ShmBuffer slots[BUFFER_SLOTS];
  /* a GBM window surface */
  struct gbm_surface *gbm;
};

struct EglContext {
  EglContext *next;
  EglDisplay *display;
  const EglConfigInfo *config;      /* null: EGL_KHR_no_config_context */
  EGLenum api;
  EGLint major, minor;
  uint32_t kernel;      /* the kernel's context number */
  int destroyed;
  pthread_t thread;
  int current;
};

struct EglDisplay {
  EglDisplay *next;
  EGLenum platform;
  void *native;              /* what the program gave (a wl_display, a gbm_device or null) */
  struct wl_display *wl;
  pthread_mutex_t lock;      /* the lists and the Wayland objects below */
  int initialized;
  struct wl_event_queue *queue;
  struct wl_registry *registry;
  struct wl_shm *shm;
  int has_argb, has_xrgb;
  struct wl_proxy *dmabuf;              /* zwp_linux_dmabuf_v1 (version 2 or 3), when the compositor has it and the GPU buffers work */
  uint32_t dmabuf_name, dmabuf_version;
  struct gbm_device *buffers;           /* the device the GPU buffers of window surfaces are made on (a Wayland display's own) */
  struct gbm_device *gbm;               /* the GBM platform's native display */
  EglSurface *surfaces;
  EglContext *contexts;
};

static pthread_mutex_t g_displays_lock = PTHREAD_MUTEX_INITIALIZER;
static EglDisplay *g_displays;      /* never freed: an EGLDisplay stays valid for the program's life, as EGL says */

/* ---- Per thread: the command buffer, what is current and the last error */
typedef struct ThreadGl {
  GlStream stream;
  EglContext *context;
  EglSurface *draw, *read;
  EglDisplay *display;
  EGLint error;
  EGLenum api;
} ThreadGl;

static pthread_key_t g_thread_key;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static __thread ThreadGl *t_gl;      /* (a pointer only: the static TLS of a library that a program opens with dlopen is small) */
static ThreadGl g_fallback;          /* when a thread's own cannot be made: commands then go nowhere */

static void release_current(ThreadGl *t);
static void thread_exit(void *p) {
  ThreadGl *t = (ThreadGl *)p;
  if (!t) return;
  t_gl = t;
  release_current(t);
  t_gl = NULL;
  free(t);
}
static void make_thread_key(void) { pthread_key_create(&g_thread_key, thread_exit); }

static ThreadGl *thread_gl(void) {
  if (t_gl) return t_gl;
  pthread_once(&g_thread_once, make_thread_key);
  ThreadGl *t = (ThreadGl *)calloc(1, sizeof(ThreadGl));
  if (!t) return &g_fallback;
  t->error = EGL_SUCCESS;
  t->api = EGL_OPENGL_ES_API;
  pthread_setspecific(g_thread_key, t);
  t_gl = t;
  return t;
}

GlStream *gl_stream_current(void) { return &thread_gl()->stream; }

uint64_t gl_stream_submit(const uint32_t *words, uint32_t count) {
  ThreadGl *t = thread_gl();
  if (!t->context || t == &g_fallback || count == 0) return 0;
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_EXECUTE;
  r.data = (uint64_t)(uintptr_t)words;
  r.length = count * 4;
  if (gl_call(&r) < 0) return 0;
  return r.result;
}

#include "../../OpenGl/generated/GlEntries.inc"

GLAPI GLenum APIENTRY glGetError(void) { return tcos_glGetError(); }
GLAPI void APIENTRY glFlush(void) { tcos_glFlush(); }
GLAPI void APIENTRY glFinish(void) { tcos_glFinish(); }

static EGLBoolean fail(EGLint error) {
  thread_gl()->error = error;
  return EGL_FALSE;
}
static EGLBoolean ok(void) {
  thread_gl()->error = EGL_SUCCESS;
  return EGL_TRUE;
}

/* ---- Displays */
static EglDisplay *find_display(EGLDisplay handle) {
  pthread_mutex_lock(&g_displays_lock);
  EglDisplay *d = g_displays;
  while (d && d != (EglDisplay *)handle) d = d->next;
  pthread_mutex_unlock(&g_displays_lock);
  return d;
}

/* The display, initialized; else null with the error set. Takes nothing: the caller locks it for the lists. */
static EglDisplay *ready_display(EGLDisplay handle) {
  EglDisplay *d = find_display(handle);
  if (!d) { fail(EGL_BAD_DISPLAY); return NULL; }
  pthread_mutex_lock(&d->lock);
  const int initialized = d->initialized;
  pthread_mutex_unlock(&d->lock);
  if (!initialized) { fail(EGL_NOT_INITIALIZED); return NULL; }
  return d;
}

static EGLDisplay get_display(EGLenum platform, void *native) {
  pthread_mutex_lock(&g_displays_lock);
  EglDisplay *d = g_displays;
  while (d && !(d->platform == platform && d->native == native)) d = d->next;
  if (!d) {
    d = (EglDisplay *)calloc(1, sizeof(EglDisplay));
    if (d) {
      d->platform = platform;
      d->native = native;
      if (platform == EGL_PLATFORM_WAYLAND_KHR) d->wl = (struct wl_display *)native;
      if (platform == EGL_PLATFORM_GBM_KHR) d->gbm = (struct gbm_device *)native;
      pthread_mutex_init(&d->lock, NULL);
      d->next = g_displays;
      g_displays = d;
    }
  }
  pthread_mutex_unlock(&g_displays_lock);
  if (!d) { fail(EGL_BAD_ALLOC); return EGL_NO_DISPLAY; }
  ok();
  return d;
}

/* A wl_display is a proxy whose first word is its interface: what Mesa looks at too when eglGetDisplay is given a native display without a platform. */
static int is_wayland_display(void *native) { return native && *(const struct wl_interface *const *)native == &wl_display_interface; }

EGLAPI EGLDisplay eglGetDisplay(void *native) {
  if (native == EGL_DEFAULT_DISPLAY) return get_display(EGL_PLATFORM_SURFACELESS_MESA, NULL);
  if (tcos_gbm_is_device(native)) return get_display(EGL_PLATFORM_GBM_KHR, native);
  if (is_wayland_display(native)) return get_display(EGL_PLATFORM_WAYLAND_KHR, native);
  fail(EGL_BAD_PARAMETER);
  return EGL_NO_DISPLAY;
}

static EGLDisplay platform_display(EGLenum platform, void *native) {
  if (platform == EGL_PLATFORM_WAYLAND_KHR) {
    if (!native || !is_wayland_display(native)) { fail(EGL_BAD_PARAMETER); return EGL_NO_DISPLAY; }
    return get_display(platform, native);
  }
  if (platform == EGL_PLATFORM_SURFACELESS_MESA) {
    if (native) { fail(EGL_BAD_PARAMETER); return EGL_NO_DISPLAY; }
    return get_display(platform, NULL);
  }
  if (platform == EGL_PLATFORM_GBM_KHR) {      /* (EGL_PLATFORM_GBM_MESA is the same number) */
    if (!tcos_gbm_is_device(native)) { fail(EGL_BAD_PARAMETER); return EGL_NO_DISPLAY; }
    return get_display(platform, native);
  }
  fail(EGL_BAD_PARAMETER);
  return EGL_NO_DISPLAY;
}
EGLAPI EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs) {
  if (attribs && attribs[0] != EGL_NONE) { fail(EGL_BAD_ATTRIBUTE); return EGL_NO_DISPLAY; }
  return platform_display(platform, native);
}
EGLAPI EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attribs) {
  if (attribs && attribs[0] != EGL_NONE) { fail(EGL_BAD_ATTRIBUTE); return EGL_NO_DISPLAY; }
  return platform_display(platform, native);
}

/* zwp_linux_dmabuf_v1 and zwp_linux_buffer_params_v1 (linux-dmabuf-unstable-v1.xml, as src/Linux/libvulkan/wayland.inc has them): the requests sent, and every
 * event of versions 2 and 3 (libwayland reads an event by its signature even when nobody listens). */
static const struct wl_interface kDmabufInterface, kParamsInterface;
static const struct wl_interface *kParamsTypes[] = {&kParamsInterface};
static const struct wl_interface *kNoTypes[] = {NULL, NULL, NULL, NULL, NULL, NULL};
static const struct wl_interface *kImmedTypes[] = {&wl_buffer_interface, NULL, NULL, NULL, NULL};
static const struct wl_message kDmabufRequests[] = {{"destroy", "", kNoTypes}, {"create_params", "n", kParamsTypes}};
static const struct wl_message kDmabufEvents[] = {{"format", "u", kNoTypes}, {"modifier", "3uuu", kNoTypes}};
static const struct wl_interface kDmabufInterface = {"zwp_linux_dmabuf_v1", 3, 2, kDmabufRequests, 2, kDmabufEvents};
static const struct wl_message kParamsRequests[] = {
    {"destroy", "", kNoTypes}, {"add", "huuuuu", kNoTypes}, {"create", "iiuu", kNoTypes}, {"create_immed", "2niiuu", kImmedTypes}};
static const struct wl_message kParamsEvents[] = {{"created", "n", kImmedTypes}, {"failed", "", kNoTypes}};
static const struct wl_interface kParamsInterface = {"zwp_linux_buffer_params_v1", 3, 4, kParamsRequests, 2, kParamsEvents};
enum { DMABUF_DESTROY = 0, DMABUF_CREATE_PARAMS = 1, PARAMS_DESTROY = 0, PARAMS_ADD = 1, PARAMS_CREATE_IMMED = 3 };

static int env_set(const char *name) {
  const char *v = getenv(name);
  return v && v[0] && strcmp(v, "0") != 0;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  EglDisplay *d = (EglDisplay *)data;
  if (strcmp(interface, "wl_shm") == 0 && !d->shm) d->shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, 1);
  if (strcmp(interface, kDmabufInterface.name) == 0 && version >= 2 && !d->dmabuf_name) d->dmabuf_name = name, d->dmabuf_version = version < 3 ? version : 3;      /* (create_immed is version 2) */
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener kRegistryListener = {registry_global, registry_global_remove};
static void shm_format(void *data, struct wl_shm *shm, uint32_t format) {
  EglDisplay *d = (EglDisplay *)data;
  if (format == WL_SHM_FORMAT_ARGB8888) d->has_argb = 1;
  if (format == WL_SHM_FORMAT_XRGB8888) d->has_xrgb = 1;
}
static const struct wl_shm_listener kShmListener = {shm_format};

/* The display's wl_shm, found on a registry of its own (on a queue of its own: the program's queues never see these events). */
static int connect_wayland(EglDisplay *d) {
  d->queue = wl_display_create_queue(d->wl);
  if (!d->queue) return 0;
  struct wl_display *wrapper = (struct wl_display *)wl_proxy_create_wrapper(d->wl);
  if (!wrapper) return 0;
  wl_proxy_set_queue((struct wl_proxy *)wrapper, d->queue);
  d->registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  if (!d->registry) return 0;
  wl_registry_add_listener(d->registry, &kRegistryListener, d);
  if (wl_display_roundtrip_queue(d->wl, d->queue) < 0 || !d->shm) return 0;
  wl_shm_add_listener(d->shm, &kShmListener, d);
  /* GPU buffers: the compositor's zwp_linux_dmabuf_v1 and a GBM device of /dev/dxg to make them on (else every window has wl_shm buffers) */
  if (d->dmabuf_name && !env_set("TCOS_EGL_SHM") && g_dxg >= 0) {
    d->buffers = gbm_create_device(g_dxg);
    if (d->buffers) d->dmabuf = (struct wl_proxy *)wl_registry_bind(d->registry, d->dmabuf_name, &kDmabufInterface, d->dmabuf_version);
  }
  if (wl_display_roundtrip_queue(d->wl, d->queue) < 0) return 0;
  return d->has_xrgb || d->has_argb;      /* (both are always there: wl_shm must offer them) */
}

static void disconnect_wayland(EglDisplay *d) {
  if (d->dmabuf) wl_proxy_marshal_flags(d->dmabuf, DMABUF_DESTROY, NULL, d->dmabuf_version, WL_MARSHAL_FLAG_DESTROY);
  if (d->buffers) gbm_device_destroy(d->buffers);
  d->dmabuf = NULL;
  d->buffers = NULL;
  d->dmabuf_name = 0;
  if (d->shm) wl_shm_destroy(d->shm);
  if (d->registry) wl_registry_destroy(d->registry);
  if (d->queue) wl_event_queue_destroy(d->queue);
  d->shm = NULL;
  d->registry = NULL;
  d->queue = NULL;
  d->has_argb = d->has_xrgb = 0;
}

EGLAPI EGLBoolean eglInitialize(EGLDisplay handle, EGLint *major, EGLint *minor) {
  EglDisplay *d = find_display(handle);
  if (!d) return fail(EGL_BAD_DISPLAY);
  /* the kernel must have an OpenGL: a request it cannot run (a context of no number) answers, one with no /dev/dxg or no GL does not */
  TcosGlRequest probe;
  memset(&probe, 0, sizeof(probe));
  probe.op = OP_DELETE;
  pthread_once(&g_dxg_once, open_dxg);
  if (g_dxg < 0) return fail(EGL_NOT_INITIALIZED);
  TcosDxgGl call;
  memset(&call, 0, sizeof(call));
  probe.size = sizeof(probe);
  call.request = (uint64_t)(uintptr_t)&probe;
  if (ioctl(g_dxg, TCOS_DXG_IOCTL_GL, &call) != 0) return fail(EGL_NOT_INITIALIZED);
  pthread_mutex_lock(&d->lock);
  int good = 1;
  if (!d->initialized && d->platform == EGL_PLATFORM_WAYLAND_KHR) {
    good = connect_wayland(d);
    if (!good) disconnect_wayland(d);
  }
  if (good) d->initialized = 1;
  pthread_mutex_unlock(&d->lock);
  if (!good) return fail(EGL_NOT_INITIALIZED);
  if (major) *major = 1;
  if (minor) *minor = 5;
  return ok();
}

static void free_surface(EglSurface *s);
static void free_context(EglContext *c);

/* Takes `s` out of its display's list and frees it when nothing has it current. Display locked. */
static void drop_surface(EglSurface *s) {
  if (s->bound) return;
  EglSurface **at = &s->display->surfaces;
  while (*at && *at != s) at = &(*at)->next;
  if (*at) *at = s->next;
  free_surface(s);
}
static void drop_context(EglContext *c) {
  if (c->current) return;
  EglContext **at = &c->display->contexts;
  while (*at && *at != c) at = &(*at)->next;
  if (*at) *at = c->next;
  free_context(c);
}

EGLAPI EGLBoolean eglTerminate(EGLDisplay handle) {
  EglDisplay *d = find_display(handle);
  if (!d) return fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&d->lock);
  if (d->initialized) {
    /* everything is marked for deletion; what a thread has current goes when it is released */
    for (EglSurface *s = d->surfaces, *next; s; s = next) { next = s->next; s->destroyed = 1; drop_surface(s); }
    for (EglContext *c = d->contexts, *next; c; c = next) { next = c->next; c->destroyed = 1; drop_context(c); }
    if (!d->surfaces && !d->contexts) disconnect_wayland(d);
    d->initialized = 0;
  }
  pthread_mutex_unlock(&d->lock);
  return ok();
}

static const char kClientExtensions[] = "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_wayland EGL_EXT_platform_wayland EGL_KHR_platform_gbm "
                                        "EGL_MESA_platform_gbm EGL_MESA_platform_surfaceless EGL_KHR_client_get_all_proc_addresses";
static const char kDisplayExtensions[] = "EGL_KHR_create_context EGL_KHR_create_context_no_error EGL_KHR_surfaceless_context EGL_KHR_no_config_context "
                                         "EGL_KHR_get_all_proc_addresses EGL_KHR_swap_buffers_with_damage EGL_EXT_swap_buffers_with_damage EGL_KHR_fence_sync "
                                         "EGL_KHR_image_base EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers";

EGLAPI const char *eglQueryString(EGLDisplay handle, EGLint name) {
  if (handle == EGL_NO_DISPLAY) {
    if (name == EGL_EXTENSIONS) { ok(); return kClientExtensions; }
    if (name == EGL_VERSION) { ok(); return "1.5"; }
    fail(EGL_BAD_DISPLAY);
    return NULL;
  }
  if (!ready_display(handle)) return NULL;
  switch (name) {
    case EGL_VENDOR: ok(); return "TheoryCraftOS";
    case EGL_VERSION: ok(); return "1.5 TheoryCraftOS";
    case EGL_CLIENT_APIS: ok(); return "OpenGL OpenGL_ES";
    case EGL_EXTENSIONS: ok(); return kDisplayExtensions;
    default: fail(EGL_BAD_PARAMETER); return NULL;
  }
}

/* ---- Configs */
static const EglConfigInfo *config_of(EGLConfig handle) {
  const uintptr_t id = (uintptr_t)handle;
  return id >= 1 && id <= (uintptr_t)CONFIG_COUNT ? &kConfigs[id - 1] : NULL;
}
static EGLConfig handle_of(const EglConfigInfo *c) { return (EGLConfig)(uintptr_t)c->id; }

static int config_value(const EglConfigInfo *c, EGLint name, EGLint *value) {
  switch (name) {
    case EGL_BUFFER_SIZE: *value = 24 + c->alpha; return 1;
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: *value = 8; return 1;
    case EGL_ALPHA_SIZE: *value = c->alpha; return 1;
    case EGL_DEPTH_SIZE: *value = c->depth; return 1;
    case EGL_STENCIL_SIZE: *value = c->stencil; return 1;
    case EGL_CONFIG_CAVEAT: *value = EGL_NONE; return 1;
    case EGL_CONFIG_ID: *value = c->id; return 1;
    case EGL_LEVEL: *value = 0; return 1;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: *value = 16384; return 1;
    case EGL_MAX_PBUFFER_PIXELS: *value = 16384 * 16384; return 1;
    case EGL_NATIVE_RENDERABLE: *value = EGL_FALSE; return 1;
    case EGL_NATIVE_VISUAL_ID: *value = (EGLint)c->fourcc; return 1;
    case EGL_NATIVE_VISUAL_TYPE: *value = 0; return 1;
    case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: *value = 0; return 1;
    case EGL_SURFACE_TYPE: *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; return 1;
    case EGL_TRANSPARENT_TYPE: *value = EGL_NONE; return 1;
    case EGL_TRANSPARENT_RED_VALUE: case EGL_TRANSPARENT_GREEN_VALUE: case EGL_TRANSPARENT_BLUE_VALUE: *value = 0; return 1;
    case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: *value = EGL_FALSE; return 1;
    case EGL_MIN_SWAP_INTERVAL: *value = 0; return 1;
    case EGL_MAX_SWAP_INTERVAL: *value = 1; return 1;
    case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: *value = 0; return 1;
    case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; return 1;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: *value = RENDERABLE_ALL; return 1;
    default: return 0;
  }
}

EGLAPI EGLBoolean eglGetConfigAttrib(EGLDisplay handle, EGLConfig config, EGLint name, EGLint *value) {
  if (!ready_display(handle)) return EGL_FALSE;
  const EglConfigInfo *c = config_of(config);
  if (!c) return fail(EGL_BAD_CONFIG);
  if (!value) return fail(EGL_BAD_PARAMETER);
  if (!config_value(c, name, value)) return fail(EGL_BAD_ATTRIBUTE);
  return ok();
}

EGLAPI EGLBoolean eglGetConfigs(EGLDisplay handle, EGLConfig *configs, EGLint size, EGLint *count) {
  if (!ready_display(handle)) return EGL_FALSE;
  if (!count) return fail(EGL_BAD_PARAMETER);
  if (!configs) { *count = CONFIG_COUNT; return ok(); }
  EGLint n = 0;
  for (EGLint i = 0; i < CONFIG_COUNT && n < size; ++i) configs[n++] = handle_of(&kConfigs[i]);
  *count = n;
  return ok();
}

/* What eglChooseConfig asked for, attribute by attribute (EGL 1.5, table 3.4: the defaults and how each is matched). */
typedef struct ConfigQuery {
  EGLint sizes[7];      /* buffer, red, green, blue, alpha, depth, stencil: at least (0 and EGL_DONT_CARE: anything) */
  EGLint config_id, surface_type, renderable, conformant, caveat, native_renderable, level, samples, sample_buffers, transparent, color_type;
  EGLint native_visual_type, min_swap, max_swap, bind_rgb, bind_rgba;
} ConfigQuery;
static const EGLint kSizeNames[7] = {EGL_BUFFER_SIZE, EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE, EGL_DEPTH_SIZE, EGL_STENCIL_SIZE};

static int parse_query(const EGLint *attribs, ConfigQuery *q) {
  memset(q, 0, sizeof(*q));
  q->config_id = q->caveat = q->native_renderable = q->native_visual_type = q->min_swap = q->max_swap = q->bind_rgb = q->bind_rgba = EGL_DONT_CARE;
  q->surface_type = EGL_WINDOW_BIT;
  /* EGL's default is EGL_OPENGL_ES_BIT, OpenGL ES 1.x, which this library does not have: a program that does not say (and then makes an ES 2.0 context, as
   * most do) would find no config. Unsaid, any renderable type matches; asked for, ES 1.x matches nothing. */
  q->renderable = 0;
  q->transparent = EGL_NONE;
  q->color_type = EGL_RGB_BUFFER;
  for (; attribs && attribs[0] != EGL_NONE; attribs += 2) {
    const EGLint name = attribs[0], value = attribs[1];
    int size = -1;
    for (int i = 0; i < 7; ++i) if (kSizeNames[i] == name) size = i;
    if (size >= 0) { q->sizes[size] = value; continue; }
    switch (name) {
      case EGL_CONFIG_ID: q->config_id = value; break;
      case EGL_SURFACE_TYPE: q->surface_type = value; break;
      case EGL_RENDERABLE_TYPE: q->renderable = value; break;
      case EGL_CONFORMANT: q->conformant = value; break;
      case EGL_CONFIG_CAVEAT: q->caveat = value; break;
      case EGL_NATIVE_RENDERABLE: q->native_renderable = value; break;
      case EGL_LEVEL: q->level = value; break;
      case EGL_SAMPLES: q->samples = value; break;
      case EGL_SAMPLE_BUFFERS: q->sample_buffers = value; break;
      case EGL_TRANSPARENT_TYPE: q->transparent = value; break;
      case EGL_COLOR_BUFFER_TYPE: q->color_type = value; break;
      case EGL_NATIVE_VISUAL_TYPE: q->native_visual_type = value; break;
      case EGL_MIN_SWAP_INTERVAL: q->min_swap = value; break;
      case EGL_MAX_SWAP_INTERVAL: q->max_swap = value; break;
      case EGL_BIND_TO_TEXTURE_RGB: q->bind_rgb = value; break;
      case EGL_BIND_TO_TEXTURE_RGBA: q->bind_rgba = value; break;
      case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: if (value > 0 && value != EGL_DONT_CARE) return -1; break;      /* (none has any) */
      case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: case EGL_MAX_PBUFFER_PIXELS: case EGL_NATIVE_VISUAL_ID:
      case EGL_TRANSPARENT_RED_VALUE: case EGL_TRANSPARENT_GREEN_VALUE: case EGL_TRANSPARENT_BLUE_VALUE: case EGL_MATCH_NATIVE_PIXMAP: break;      /* (ignored, as EGL says) */
      default: return 0;
    }
  }
  return 1;
}

static int exact(EGLint want, EGLint have) { return want == EGL_DONT_CARE || want == have; }
static int matches(const ConfigQuery *q, const EglConfigInfo *c) {
  if (q->config_id != EGL_DONT_CARE) return q->config_id == c->id;      /* (every other attribute is ignored then) */
  for (int i = 0; i < 7; ++i) {
    EGLint have = 0;
    config_value(c, kSizeNames[i], &have);
    if (q->sizes[i] != EGL_DONT_CARE && q->sizes[i] > have) return 0;
  }
  EGLint surface = 0, renderable = 0;
  config_value(c, EGL_SURFACE_TYPE, &surface);
  config_value(c, EGL_RENDERABLE_TYPE, &renderable);
  if (q->surface_type != EGL_DONT_CARE && (q->surface_type & ~surface)) return 0;
  if (q->renderable != EGL_DONT_CARE && (q->renderable & ~renderable)) return 0;
  if (q->conformant != EGL_DONT_CARE && (q->conformant & ~renderable)) return 0;
  if (!exact(q->caveat, EGL_NONE) || !exact(q->native_renderable, EGL_FALSE) || !exact(q->level, 0) || !exact(q->transparent, EGL_NONE) ||
      !exact(q->color_type, EGL_RGB_BUFFER) || !exact(q->native_visual_type, 0) || !exact(q->bind_rgb, EGL_FALSE) || !exact(q->bind_rgba, EGL_FALSE))
    return 0;
  if (q->min_swap != EGL_DONT_CARE && q->min_swap != 0) return 0;
  if (q->max_swap != EGL_DONT_CARE && q->max_swap != 1) return 0;
  if (q->samples != EGL_DONT_CARE && q->samples > 0) return 0;
  if (q->sample_buffers != EGL_DONT_CARE && q->sample_buffers > 0) return 0;
  return 1;
}

/* EGL's order of the matches: more colour bits of the components that were asked for first, then the smaller buffer, depth and stencil, then the id. */
static const ConfigQuery *g_sort_query;
static int config_order(const void *pa, const void *pb) {
  const EglConfigInfo *a = config_of(*(EGLConfig const *)pa), *b = config_of(*(EGLConfig const *)pb);
  const ConfigQuery *q = g_sort_query;
  EGLint bits_a = 0, bits_b = 0;
  for (int i = 1; i <= 4; ++i) {
    if (q->sizes[i] == 0 || q->sizes[i] == EGL_DONT_CARE) continue;
    EGLint va = 0, vb = 0;
    config_value(a, kSizeNames[i], &va);
    config_value(b, kSizeNames[i], &vb);
    bits_a += va;
    bits_b += vb;
  }
  if (bits_a != bits_b) return bits_b - bits_a;
  if (a->alpha != b->alpha) return a->alpha - b->alpha;      /* (BUFFER_SIZE: smaller first) */
  if (a->depth != b->depth) return a->depth - b->depth;
  if (a->stencil != b->stencil) return a->stencil - b->stencil;
  return a->id - b->id;
}

EGLAPI EGLBoolean eglChooseConfig(EGLDisplay handle, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *count) {
  if (!ready_display(handle)) return EGL_FALSE;
  if (!count) return fail(EGL_BAD_PARAMETER);
  ConfigQuery q;
  const int parsed = parse_query(attribs, &q);
  if (parsed == 0) return fail(EGL_BAD_ATTRIBUTE);
  EGLConfig found[CONFIG_COUNT];
  EGLint n = 0;
  if (parsed > 0)
    for (EGLint i = 0; i < CONFIG_COUNT; ++i)
      if (matches(&q, &kConfigs[i])) found[n++] = handle_of(&kConfigs[i]);
  static pthread_mutex_t sort_lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&sort_lock);
  g_sort_query = &q;
  qsort(found, (size_t)n, sizeof(found[0]), config_order);
  pthread_mutex_unlock(&sort_lock);
  if (!configs) { *count = n; return ok(); }
  EGLint out = 0;
  for (; out < n && out < size; ++out) configs[out] = found[out];
  *count = out;
  return ok();
}

/* ---- Contexts */
static EglContext *find_context(EglDisplay *d, EGLContext handle) {
  for (EglContext *c = d->contexts; c; c = c->next)
    if (c == (EglContext *)handle && !c->destroyed) return c;
  return NULL;
}
static void free_context(EglContext *c) {
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_DELETE;
  r.context = c->kernel;
  gl_call(&r);
  free(c);
}

EGLAPI EGLBoolean eglBindAPI(EGLenum api) {
  if (api != EGL_OPENGL_ES_API && api != EGL_OPENGL_API) return fail(EGL_BAD_PARAMETER);
  ThreadGl *t = thread_gl();
  t->api = api;
  return ok();
}
EGLAPI EGLenum eglQueryAPI(void) { return thread_gl()->api; }

EGLAPI EGLContext eglCreateContext(EGLDisplay handle, EGLConfig config, EGLContext share, const EGLint *attribs) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_NO_CONTEXT;
  const EglConfigInfo *cfg = config_of(config);
  if (config != EGL_NO_CONFIG_KHR && !cfg) { fail(EGL_BAD_CONFIG); return EGL_NO_CONTEXT; }
  const EGLenum api = thread_gl()->api;
  EGLint major = 1, minor = 0, profile = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, flags = 0;
  int profile_given = 0;
  for (; attribs && attribs[0] != EGL_NONE; attribs += 2) {
    const EGLint value = attribs[1];
    switch (attribs[0]) {
      case EGL_CONTEXT_CLIENT_VERSION: major = value; break;      /* (= EGL_CONTEXT_MAJOR_VERSION) */
      case EGL_CONTEXT_MINOR_VERSION: minor = value; break;
      case EGL_CONTEXT_OPENGL_PROFILE_MASK: profile = value; profile_given = 1; break;
      case EGL_CONTEXT_FLAGS_KHR: flags = value; break;
      case EGL_CONTEXT_OPENGL_DEBUG: if (value) flags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR; break;
      case EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE: if (value) flags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR; break;
      case EGL_CONTEXT_OPENGL_ROBUST_ACCESS: if (value) { fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; } break;      /* (no robust access) */
      case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY:
        if (value != EGL_NO_RESET_NOTIFICATION && value != EGL_LOSE_CONTEXT_ON_RESET) { fail(EGL_BAD_ATTRIBUTE); return EGL_NO_CONTEXT; }
        break;
      case 0x31B3:      /* EGL_CONTEXT_OPENGL_NO_ERROR_KHR: errors are still checked, which no-error contexts may do */
        break;
      default: fail(EGL_BAD_ATTRIBUTE); return EGL_NO_CONTEXT;
    }
  }
  if (flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR) { fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
  uint32_t kflags = 0;
  if (api == EGL_OPENGL_ES_API) {
    /* OpenGL ES 2.0 and 3.0 (a 3.0 context serves both); not 1.x, not 3.1 and later */
    if (major == 2) minor = 0;
    if (!((major == 2 && minor == 0) || (major == 3 && minor == 0))) { fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
    if (cfg && !(RENDERABLE_ALL & (major == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT))) { fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
    kflags = CREATE_ES;
    if (flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR) kflags |= CREATE_DEBUG;
  } else {
    /* OpenGL up to 3.3: the core profile from 3.2 when asked for (the default is core, as EGL says), else compatibility */
    if (major < 1 || major > 3 || (major == 3 && minor > 3)) { fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
    if (profile & ~(EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT | EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT) || !profile) { fail(EGL_BAD_MATCH); return EGL_NO_CONTEXT; }
    const int core = (major > 3 || (major == 3 && minor >= 2)) && (profile & EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT) &&
                     !(profile_given && (profile & EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT));
    if (core) kflags |= CREATE_CORE;
    if (flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR) kflags |= CREATE_FORWARD;
    if (flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR) kflags |= CREATE_DEBUG;
    if (major < 3 || (major == 3 && minor < 2)) { major = 3; minor = 3; }      /* (an older version asked for: the compatibility context, which has it all) */
  }
  pthread_mutex_lock(&d->lock);
  EglContext *with = NULL;
  if (share != EGL_NO_CONTEXT) {
    with = find_context(d, share);
    if (!with || with->api != api) {
      pthread_mutex_unlock(&d->lock);
      fail(EGL_BAD_CONTEXT);
      return EGL_NO_CONTEXT;
    }
  }
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_CREATE;
  r.share = with ? with->kernel : 0;
  r.major = (uint32_t)major;
  r.minor = (uint32_t)minor;
  r.flags = kflags;
  EglContext *c = NULL;
  if (gl_call(&r) >= 0 && r.result) c = (EglContext *)calloc(1, sizeof(EglContext));
  if (c) {
    c->display = d;
    c->config = cfg;
    c->api = api;
    c->major = api == EGL_OPENGL_ES_API ? 3 : major;
    c->minor = api == EGL_OPENGL_ES_API ? 0 : minor;
    c->kernel = (uint32_t)r.result;
    c->next = d->contexts;
    d->contexts = c;
  } else if (r.result) {
    TcosGlRequest del;
    memset(&del, 0, sizeof(del));
    del.op = OP_DELETE;
    del.context = (uint32_t)r.result;
    gl_call(&del);
  }
  pthread_mutex_unlock(&d->lock);
  if (!c) { fail(EGL_BAD_ALLOC); return EGL_NO_CONTEXT; }
  ok();
  return c;
}

EGLAPI EGLBoolean eglDestroyContext(EGLDisplay handle, EGLContext context) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&d->lock);
  EglContext *c = find_context(d, context);
  if (c) {
    c->destroyed = 1;
    drop_context(c);
  }
  pthread_mutex_unlock(&d->lock);
  return c ? ok() : fail(EGL_BAD_CONTEXT);
}

EGLAPI EGLBoolean eglQueryContext(EGLDisplay handle, EGLContext context, EGLint name, EGLint *value) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&d->lock);
  EglContext *c = find_context(d, context);
  EGLint v = 0;
  int known = 1;
  if (c) {
    switch (name) {
      case EGL_CONFIG_ID: v = c->config ? c->config->id : 0; break;
      case EGL_CONTEXT_CLIENT_TYPE: v = (EGLint)c->api; break;
      case EGL_CONTEXT_CLIENT_VERSION: v = c->major; break;
      case EGL_CONTEXT_MINOR_VERSION: v = c->minor; break;
      case EGL_RENDER_BUFFER: {      /* (every surface is drawn through its back buffer; none without one bound) */
        ThreadGl *t = thread_gl();
        v = t->context == c && t->draw ? EGL_BACK_BUFFER : EGL_NONE;
        break;
      }
      default: known = 0;
    }
  }
  pthread_mutex_unlock(&d->lock);
  if (!c) return fail(EGL_BAD_CONTEXT);
  if (!known) return fail(EGL_BAD_ATTRIBUTE);
  if (value) *value = v;
  return ok();
}

/* ---- Surfaces */
static uint64_t g_next_window = 1;
static uint64_t next_kernel_window(void) { return __atomic_fetch_add(&g_next_window, 1, __ATOMIC_RELAXED); }

static EglSurface *find_surface(EglDisplay *d, EGLSurface handle) {
  for (EglSurface *s = d->surfaces; s; s = s->next)
    if (s == (EglSurface *)handle && !s->destroyed) return s;
  return NULL;
}

static void free_slot(ShmBuffer *b) {
  if (b->buffer) wl_buffer_destroy(b->buffer);
  if (b->data) munmap(b->data, b->bytes);
  if (b->bo) {
    if (b->fd >= 0) close(b->fd);
    gbm_bo_destroy(b->bo);
  }
  memset(b, 0, sizeof(*b));
}

static void free_surface(EglSurface *s) {
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_DESTROY_SURFACE;
  r.window = s->kernel_window;
  gl_call(&r);
  if (s->window_surface) {
    for (int i = 0; i < BUFFER_SLOTS; ++i) free_slot(&s->slots[i]);
    if (s->frame) wl_callback_destroy(s->frame);
    if (s->dmabuf) wl_proxy_wrapper_destroy(s->dmabuf);
    if (s->surface) wl_proxy_wrapper_destroy(s->surface);
    if (s->shm) wl_proxy_wrapper_destroy(s->shm);
    if (s->queue) wl_event_queue_destroy(s->queue);
    if (s->native) {
      s->native->driver_private = NULL;
      s->native->resize_callback = NULL;
      if (s->native->version >= 3) s->native->destroy_window_callback = NULL;
    }
  }
  free(s);
}

static int make_current_kernel(EglContext *c, EglSurface *draw);

/* wl_egl_window_resize: the size takes effect at once on the thread that has the surface current, else when it is next made current or swapped. */
static void window_resized(struct wl_egl_window *window, void *data) {
  EglSurface *s = (EglSurface *)data;
  if (!s || s->native != window) return;
  s->dx += window->dx;
  s->dy += window->dy;
  ThreadGl *t = thread_gl();
  if (t->draw == s && t->context && (window->width != s->width || window->height != s->height)) {
    gl_stream_flush();
    make_current_kernel(t->context, s);
  }
}
/* wl_egl_window_destroy before eglDestroySurface: the surface has no window any more (a swap is then EGL_BAD_NATIVE_WINDOW). */
static void window_destroyed(void *data) {
  EglSurface *s = (EglSurface *)data;
  if (!s) return;
  s->native = NULL;
}

static EGLint surface_flags(const EglSurface *s) {
  EGLint f = s->window_surface ? SURFACE_DOUBLE : 0;
  if (s->config->depth) f |= SURFACE_DEPTH;
  if (s->config->stencil) f |= SURFACE_STENCIL;
  if (s->config->alpha) f |= SURFACE_ALPHA;
  return f;
}

/* Makes `c` current on the calling thread with `draw` (null: no drawable), the size of the window as the program last set it. */
static int make_current_kernel(EglContext *c, EglSurface *draw) {
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_MAKE_CURRENT;
  r.context = c ? c->kernel : 0;
  if (c && draw) {
    if (draw->window_surface && draw->native) {
      draw->width = draw->native->width > 0 ? draw->native->width : 1;
      draw->height = draw->native->height > 0 ? draw->native->height : 1;
    }
    r.window = draw->kernel_window;
    r.width = (uint32_t)draw->width;
    r.height = (uint32_t)draw->height;
    r.flags = (uint32_t)surface_flags(draw);
  }
  const int32_t st = gl_call(&r);
  return st >= 0 ? 0 : st == STATUS_RESOURCE_IN_USE ? EGL_BAD_ACCESS : EGL_BAD_ALLOC;
}

static EglSurface *new_surface(EglDisplay *d, const EglConfigInfo *cfg, int window) {
  EglSurface *s = (EglSurface *)calloc(1, sizeof(EglSurface));
  if (!s) return NULL;
  s->display = d;
  s->config = cfg;
  s->window_surface = window;
  s->kernel_window = next_kernel_window();
  s->swap_interval = 1;
  return s;
}

/* The attributes of a window surface: only what has one meaning here. */
static int window_attribs_ok(EGLint name, EGLint value) {
  switch (name) {
    case EGL_RENDER_BUFFER: return value == EGL_BACK_BUFFER ? 1 : value == EGL_SINGLE_BUFFER ? 1 : 0;      /* (single buffering is given as double, which EGL allows) */
    case EGL_GL_COLORSPACE: return value == EGL_COLORSPACE_LINEAR ? 1 : -1;      /* (no sRGB window buffers: EGL_BAD_MATCH) */
    case EGL_VG_ALPHA_FORMAT: case EGL_VG_COLORSPACE: return 1;
    default: return 0;
  }
}

/* A window surface of a gbm_surface (the GBM platform): its size is the gbm_surface's, fixed. Display not locked. */
static EGLSurface create_gbm_surface(EglDisplay *d, const EglConfigInfo *cfg, void *native) {
  uint32_t width = 0, height = 0, format = 0;
  if (!tcos_gbm_surface_info(native, &width, &height, &format)) { fail(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
  pthread_mutex_lock(&d->lock);
  for (EglSurface *o = d->surfaces; o; o = o->next)
    if (o->gbm == (struct gbm_surface *)native && !o->destroyed) {      /* (a gbm_surface has one EGL surface at a time) */
      pthread_mutex_unlock(&d->lock);
      fail(EGL_BAD_ALLOC);
      return EGL_NO_SURFACE;
    }
  EglSurface *s = new_surface(d, cfg, 1);
  if (s) {
    s->gbm = (struct gbm_surface *)native;
    s->width = (EGLint)width;
    s->height = (EGLint)height;
    s->next = d->surfaces;
    d->surfaces = s;
  }
  pthread_mutex_unlock(&d->lock);
  if (!s) { fail(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
  ok();
  return s;
}

static EGLSurface create_window_surface(EGLDisplay handle, EGLConfig config, void *native, const EGLint *ints, const EGLAttrib *attribs) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_NO_SURFACE;
  const EglConfigInfo *cfg = config_of(config);
  if (!cfg) { fail(EGL_BAD_CONFIG); return EGL_NO_SURFACE; }
  if (d->platform != EGL_PLATFORM_WAYLAND_KHR && d->platform != EGL_PLATFORM_GBM_KHR) { fail(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
  for (int i = 0;; i += 2) {
    const EGLint name = ints ? ints[i] : attribs ? (EGLint)attribs[i] : EGL_NONE;
    if (name == EGL_NONE) break;
    const EGLint value = ints ? ints[i + 1] : (EGLint)attribs[i + 1];
    const int good = window_attribs_ok(name, value);
    if (good <= 0) { fail(good < 0 ? EGL_BAD_MATCH : EGL_BAD_ATTRIBUTE); return EGL_NO_SURFACE; }
  }
  if (d->platform == EGL_PLATFORM_GBM_KHR) return create_gbm_surface(d, cfg, native);
  struct wl_egl_window *window = (struct wl_egl_window *)native;
  if (!window || !window->surface) { fail(EGL_BAD_NATIVE_WINDOW); return EGL_NO_SURFACE; }
  if (window->driver_private) { fail(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }      /* (a window has one surface at a time) */
  pthread_mutex_lock(&d->lock);
  EglSurface *s = new_surface(d, cfg, 1);
  int good = s != NULL;
  if (good) {
    s->native = window;
    s->queue = wl_display_create_queue(d->wl);
    s->surface = s->queue ? (struct wl_surface *)wl_proxy_create_wrapper(window->surface) : NULL;
    s->shm = s->surface ? (struct wl_shm *)wl_proxy_create_wrapper(d->shm) : NULL;
    s->dmabuf = s->shm && d->dmabuf ? (struct wl_proxy *)wl_proxy_create_wrapper(d->dmabuf) : NULL;
    good = s->shm != NULL && (!d->dmabuf || s->dmabuf);
    if (good) {
      wl_proxy_set_queue((struct wl_proxy *)s->surface, s->queue);
      wl_proxy_set_queue((struct wl_proxy *)s->shm, s->queue);
      if (s->dmabuf) wl_proxy_set_queue(s->dmabuf, s->queue);
      s->width = window->width > 0 ? window->width : 1;
      s->height = window->height > 0 ? window->height : 1;
      window->driver_private = s;
      window->resize_callback = window_resized;
      if (window->version >= 3) window->destroy_window_callback = window_destroyed;
      s->next = d->surfaces;
      d->surfaces = s;
    } else {
      s->native = NULL;
      free_surface(s);
      s = NULL;
    }
  }
  pthread_mutex_unlock(&d->lock);
  if (!good) { fail(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
  ok();
  return s;
}

EGLAPI EGLSurface eglCreateWindowSurface(EGLDisplay handle, EGLConfig config, void *native, const EGLint *attribs) {
  static const EGLint none[] = {EGL_NONE};
  return create_window_surface(handle, config, native, attribs ? attribs : none, NULL);
}
EGLAPI EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay handle, EGLConfig config, void *native, const EGLint *attribs) {
  static const EGLint none[] = {EGL_NONE};
  return create_window_surface(handle, config, native, attribs ? attribs : none, NULL);
}
EGLAPI EGLSurface eglCreatePlatformWindowSurface(EGLDisplay handle, EGLConfig config, void *native, const EGLAttrib *attribs) {
  static const EGLAttrib none[] = {EGL_NONE};
  return create_window_surface(handle, config, native, NULL, attribs ? attribs : none);
}

EGLAPI EGLSurface eglCreatePbufferSurface(EGLDisplay handle, EGLConfig config, const EGLint *attribs) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_NO_SURFACE;
  const EglConfigInfo *cfg = config_of(config);
  if (!cfg) { fail(EGL_BAD_CONFIG); return EGL_NO_SURFACE; }
  EGLint width = 0, height = 0;
  for (; attribs && attribs[0] != EGL_NONE; attribs += 2) {
    switch (attribs[0]) {
      case EGL_WIDTH: width = attribs[1]; break;
      case EGL_HEIGHT: height = attribs[1]; break;
      case EGL_LARGEST_PBUFFER: break;
      case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET:
        if (attribs[1] != EGL_NO_TEXTURE) { fail(EGL_BAD_MATCH); return EGL_NO_SURFACE; }      /* (no binding to textures) */
        break;
      case EGL_MIPMAP_TEXTURE: case EGL_GL_COLORSPACE: case EGL_VG_ALPHA_FORMAT: case EGL_VG_COLORSPACE: break;
      default: fail(EGL_BAD_ATTRIBUTE); return EGL_NO_SURFACE;
    }
  }
  if (width < 0 || height < 0) { fail(EGL_BAD_PARAMETER); return EGL_NO_SURFACE; }
  if (width > 16384 || height > 16384) { fail(EGL_BAD_MATCH); return EGL_NO_SURFACE; }
  pthread_mutex_lock(&d->lock);
  EglSurface *s = new_surface(d, cfg, 0);
  if (s) {
    s->width = width ? width : 1;      /* (a 0 x 0 pbuffer has one pixel of images in the kernel; it reports its own size) */
    s->height = height ? height : 1;
    s->next = d->surfaces;
    d->surfaces = s;
  }
  pthread_mutex_unlock(&d->lock);
  if (!s) { fail(EGL_BAD_ALLOC); return EGL_NO_SURFACE; }
  ok();
  return s;
}

EGLAPI EGLSurface eglCreatePixmapSurface(EGLDisplay handle, EGLConfig config, void *pixmap, const EGLint *attribs) {
  if (!ready_display(handle)) return EGL_NO_SURFACE;
  fail(config_of(config) ? EGL_BAD_NATIVE_PIXMAP : EGL_BAD_CONFIG);      /* (no platform here has pixmaps) */
  return EGL_NO_SURFACE;
}
EGLAPI EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay handle, EGLConfig config, void *pixmap, const EGLAttrib *attribs) {
  return eglCreatePixmapSurface(handle, config, pixmap, NULL);
}
EGLAPI EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay handle, EGLenum type, EGLClientBuffer buffer, EGLConfig config, const EGLint *attribs) {
  if (!ready_display(handle)) return EGL_NO_SURFACE;
  fail(EGL_BAD_PARAMETER);
  return EGL_NO_SURFACE;
}

EGLAPI EGLBoolean eglDestroySurface(EGLDisplay handle, EGLSurface surface) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&d->lock);
  EglSurface *s = find_surface(d, surface);
  if (s) {
    s->destroyed = 1;
    drop_surface(s);
  }
  pthread_mutex_unlock(&d->lock);
  return s ? ok() : fail(EGL_BAD_SURFACE);
}

EGLAPI EGLBoolean eglQuerySurface(EGLDisplay handle, EGLSurface surface, EGLint name, EGLint *value) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&d->lock);
  EglSurface *s = find_surface(d, surface);
  EGLint v = 0;
  int known = 1;
  if (s) {
    switch (name) {
      case EGL_WIDTH: v = s->window_surface && s->native ? s->native->width : s->width; break;
      case EGL_HEIGHT: v = s->window_surface && s->native ? s->native->height : s->height; break;
      case EGL_CONFIG_ID: v = s->config->id; break;
      case EGL_RENDER_BUFFER: v = EGL_BACK_BUFFER; break;
      case EGL_SWAP_BEHAVIOR: v = EGL_BUFFER_DESTROYED; break;
      case EGL_MULTISAMPLE_RESOLVE: v = EGL_MULTISAMPLE_RESOLVE_DEFAULT; break;
      case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO: v = EGL_DONT_CARE; break;      /* (EGL_UNKNOWN) */
      case EGL_LARGEST_PBUFFER: case EGL_MIPMAP_TEXTURE: case EGL_MIPMAP_LEVEL: v = 0; break;
      case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET: v = EGL_NO_TEXTURE; break;
      case EGL_GL_COLORSPACE: v = EGL_COLORSPACE_LINEAR; break;
      case EGL_VG_ALPHA_FORMAT: v = EGL_VG_ALPHA_FORMAT_NONPRE; break;
      case EGL_VG_COLORSPACE: v = EGL_COLORSPACE_SRGB; break;
      default: known = 0;
    }
  }
  pthread_mutex_unlock(&d->lock);
  if (!s) return fail(EGL_BAD_SURFACE);
  if (!known) return fail(EGL_BAD_ATTRIBUTE);
  if (value) *value = v;
  return ok();
}

EGLAPI EGLBoolean eglSurfaceAttrib(EGLDisplay handle, EGLSurface surface, EGLint name, EGLint value) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&d->lock);
  EglSurface *s = find_surface(d, surface);
  pthread_mutex_unlock(&d->lock);
  if (!s) return fail(EGL_BAD_SURFACE);
  switch (name) {
    case EGL_SWAP_BEHAVIOR: return value == EGL_BUFFER_DESTROYED ? ok() : value == EGL_BUFFER_PRESERVED ? fail(EGL_BAD_MATCH) : fail(EGL_BAD_PARAMETER);
    case EGL_MULTISAMPLE_RESOLVE: return value == EGL_MULTISAMPLE_RESOLVE_DEFAULT ? ok() : fail(EGL_BAD_MATCH);
    case EGL_MIPMAP_LEVEL: return ok();
    default: return fail(EGL_BAD_ATTRIBUTE);
  }
}

EGLAPI EGLBoolean eglBindTexImage(EGLDisplay handle, EGLSurface surface, EGLint buffer) { return ready_display(handle) ? fail(EGL_BAD_MATCH) : EGL_FALSE; }
EGLAPI EGLBoolean eglReleaseTexImage(EGLDisplay handle, EGLSurface surface, EGLint buffer) { return ready_display(handle) ? fail(EGL_BAD_MATCH) : EGL_FALSE; }
EGLAPI EGLBoolean eglCopyBuffers(EGLDisplay handle, EGLSurface surface, void *pixmap) { return ready_display(handle) ? fail(EGL_BAD_NATIVE_PIXMAP) : EGL_FALSE; }

/* ---- Current */
/* Lets go of what `t` has current (its surfaces and context may then be freed). */
static void release_current(ThreadGl *t) {
  if (!t->context) return;
  gl_stream_flush();
  make_current_kernel(NULL, NULL);
  EglDisplay *d = t->display;
  pthread_mutex_lock(&d->lock);
  EglContext *c = t->context;
  EglSurface *draw = t->draw, *read = t->read;
  c->current = 0;
  if (draw) --draw->bound;
  if (read) --read->bound;
  if (c->destroyed) drop_context(c);
  if (draw && draw->destroyed) drop_surface(draw);
  if (read && read != draw && read->destroyed) drop_surface(read);
  if (!d->initialized && !d->surfaces && !d->contexts) disconnect_wayland(d);
  pthread_mutex_unlock(&d->lock);
  t->context = NULL;
  t->draw = t->read = NULL;
  t->display = NULL;
}

EGLAPI EGLBoolean eglMakeCurrent(EGLDisplay handle, EGLSurface draw, EGLSurface read, EGLContext context) {
  ThreadGl *t = thread_gl();
  if (t == &g_fallback) return fail(EGL_BAD_ALLOC);
  EglDisplay *d = find_display(handle);
  if (!d) return fail(EGL_BAD_DISPLAY);
  if (context == EGL_NO_CONTEXT) {
    if (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE) return fail(EGL_BAD_MATCH);
    release_current(t);
    return ok();
  }
  if (!ready_display(handle)) return EGL_FALSE;
  if ((draw == EGL_NO_SURFACE) != (read == EGL_NO_SURFACE)) return fail(EGL_BAD_MATCH);
  pthread_mutex_lock(&d->lock);
  EglContext *c = find_context(d, context);
  EglSurface *ds = draw ? find_surface(d, draw) : NULL, *rs = read ? find_surface(d, read) : NULL;
  EGLint error = 0;
  if (!c) error = EGL_BAD_CONTEXT;
  else if ((draw && !ds) || (read && !rs)) error = EGL_BAD_SURFACE;
  else if (c->current && !pthread_equal(c->thread, pthread_self())) error = EGL_BAD_ACCESS;
  else if (ds && ds->window_surface && !ds->native && !ds->gbm) error = EGL_BAD_NATIVE_WINDOW;
  else if (c->config && ds && c->config != ds->config) error = EGL_BAD_MATCH;
  pthread_mutex_unlock(&d->lock);
  if (error) return fail(error);
  if (t->context == c && t->draw == ds && t->read == rs) return ok();
  gl_stream_flush();
  /* the reading surface is the drawing one: the kernel's contexts read where they draw (a different read surface is taken as the draw surface) */
  error = make_current_kernel(c, ds);
  if (error) return fail(error);
  /* the old one goes (its counts and deferred deletions), the new one is counted */
  EglContext *old_context = t->context;
  EglSurface *old_draw = t->draw, *old_read = t->read;
  EglDisplay *old_display = t->display;
  pthread_mutex_lock(&d->lock);
  c->current = 1;
  c->thread = pthread_self();
  if (ds) ++ds->bound;
  if (rs) ++rs->bound;
  pthread_mutex_unlock(&d->lock);
  if (old_context) {
    pthread_mutex_lock(&old_display->lock);
    if (old_context != c) old_context->current = 0;
    if (old_draw) --old_draw->bound;
    if (old_read) --old_read->bound;
    if (old_context != c && old_context->destroyed) drop_context(old_context);
    if (old_draw && old_draw->destroyed) drop_surface(old_draw);
    if (old_read && old_read != old_draw && old_read->destroyed) drop_surface(old_read);
    pthread_mutex_unlock(&old_display->lock);
  }
  t->context = c;
  t->draw = ds;
  t->read = rs;
  t->display = d;
  return ok();
}

EGLAPI EGLContext eglGetCurrentContext(void) { return thread_gl()->context; }
EGLAPI EGLDisplay eglGetCurrentDisplay(void) { return thread_gl()->display; }
EGLAPI EGLSurface eglGetCurrentSurface(EGLint which) {
  ThreadGl *t = thread_gl();
  if (which == EGL_DRAW) return t->draw;
  if (which == EGL_READ) return t->read;
  fail(EGL_BAD_PARAMETER);
  return EGL_NO_SURFACE;
}
EGLAPI EGLBoolean eglReleaseThread(void) {
  ThreadGl *t = thread_gl();
  release_current(t);
  t->api = EGL_OPENGL_ES_API;
  return ok();
}
EGLAPI EGLint eglGetError(void) {
  ThreadGl *t = thread_gl();
  const EGLint e = t->error;
  t->error = EGL_SUCCESS;
  return e;
}
EGLAPI EGLBoolean eglWaitClient(void) {
  if (thread_gl()->context) glFinish();
  return ok();
}
EGLAPI EGLBoolean eglWaitGL(void) { return eglWaitClient(); }
EGLAPI EGLBoolean eglWaitNative(EGLint engine) { return ok(); }

/* ---- Swapping */
static void buffer_released(void *data, struct wl_buffer *buffer) {
  ShmBuffer *b = (ShmBuffer *)data;
  b->busy = 0;
}
static const struct wl_buffer_listener kBufferListener = {buffer_released};
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
  EglSurface *s = (EglSurface *)data;
  if (s->frame == callback) s->frame = NULL;
  wl_callback_destroy(callback);
}
static const struct wl_callback_listener kFrameListener = {frame_done};

/* A GPU buffer in slot `b`: an image of /dev/dxg (a gbm buffer object) made a wl_buffer with create_immed (one plane, offset 0, linear). 0 when the image could
 * not be made (the surface then uses wl_shm buffers). */
static int make_gpu_buffer(EglSurface *s, ShmBuffer *b, uint32_t width, uint32_t height) {
  struct gbm_bo *bo = gbm_bo_create(s->display->buffers, width, height, s->config->fourcc, GBM_BO_USE_RENDERING);
  if (!bo) return 0;
  const int fd = gbm_bo_get_fd(bo);
  if (fd < 0) {
    gbm_bo_destroy(bo);
    return 0;
  }
  const uint32_t version = wl_proxy_get_version(s->dmabuf);
  struct wl_proxy *params = wl_proxy_marshal_flags(s->dmabuf, DMABUF_CREATE_PARAMS, &kParamsInterface, version, 0, NULL);
  struct wl_buffer *buffer = NULL;
  if (params) {
    wl_proxy_marshal_flags(params, PARAMS_ADD, NULL, version, 0, fd, 0u, 0u, gbm_bo_get_stride(bo), 0u, 0u);
    buffer = (struct wl_buffer *)wl_proxy_marshal_flags(params, PARAMS_CREATE_IMMED, &wl_buffer_interface, version, 0, NULL, (int32_t)width, (int32_t)height,
                                                        s->config->fourcc, 0u);
    wl_proxy_marshal_flags(params, PARAMS_DESTROY, NULL, version, WL_MARSHAL_FLAG_DESTROY);
  }
  if (!buffer) {
    close(fd);
    gbm_bo_destroy(bo);
    return 0;
  }
  wl_buffer_add_listener(buffer, &kBufferListener, b);
  b->buffer = buffer;
  b->bo = bo;
  b->fd = fd;
  b->width = width;
  b->height = height;
  return 1;
}

/* A wl_shm buffer in slot `b`. */
static int make_shm_buffer(EglSurface *s, ShmBuffer *b, uint32_t width, uint32_t height) {
  const size_t bytes = (size_t)width * height * 4;
  const int fd = memfd_create("egl-shm", MFD_CLOEXEC);
  if (fd < 0) return 0;
  if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return 0; }
  void *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) { close(fd); return 0; }
  struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, (int32_t)bytes);
  close(fd);
  if (!pool) { munmap(data, bytes); return 0; }
  const uint32_t format = s->config->alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
  b->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, (int32_t)(width * 4), format);
  wl_shm_pool_destroy(pool);
  if (!b->buffer) { munmap(data, bytes); return 0; }
  wl_buffer_add_listener(b->buffer, &kBufferListener, b);
  b->data = data;
  b->bytes = bytes;
  b->width = width;
  b->height = height;
  return 1;
}

/* A buffer of `width` x `height` that the compositor does not hold, made when there is none (waiting for one to be released when all three are held); null when the
 * connection failed or there is no memory. A GPU buffer when the surface has them; when one cannot be made, the surface goes over to wl_shm buffers. */
static ShmBuffer *free_buffer(EglSurface *s, uint32_t width, uint32_t height) {
  for (;;) {
    ShmBuffer *same = NULL, *other = NULL, *empty = NULL;
    for (int i = 0; i < BUFFER_SLOTS; ++i) {
      ShmBuffer *b = &s->slots[i];
      if (!b->buffer) { if (!empty) empty = b; continue; }
      if (b->busy) continue;
      if (b->width == width && b->height == height) { if (!same) same = b; }
      else if (!other) other = b;
    }
    if (same && (!s->dmabuf || same->bo)) return same;
    ShmBuffer *b = same ? same : empty ? empty : other;
    if (b) {
      free_slot(b);
      if (width > 16384 || height > 16384) return NULL;
      if (s->dmabuf && make_gpu_buffer(s, b, width, height)) return b;
      if (s->dmabuf) {      /* (no GPU image of this size or format: wl_shm from now on) */
        wl_proxy_wrapper_destroy(s->dmabuf);
        s->dmabuf = NULL;
      }
      return make_shm_buffer(s, b, width, height) ? b : NULL;
    }
    /* all held: wait for a release */
    if (wl_display_dispatch_queue(s->display->wl, s->queue) < 0) return NULL;
  }
}

/* The kernel copies the front image of the surface into the GPU image of dmabuf `fd` (FRONT_TO_BUFFER); 0 when it did. While the GPU still works on the image
 * it is tried again for a while. */
static int front_to_buffer(const EglSurface *s, int fd) {
  for (int tries = 0; tries < 1000; ++tries) {
    TcosGlRequest r;
    memset(&r, 0, sizeof(r));
    r.op = OP_FRONT_TO_BUFFER;
    r.window = s->kernel_window;
    r.data = (uint64_t)fd;
    const int32_t st = gl_call(&r);
    if (st >= 0) return 0;
    if (st != STATUS_RESOURCE_IN_USE) return -1;
    sched_yield();
  }
  return -1;
}

/* A swap of a GBM window surface: the new front image into a buffer of the gbm_surface nobody holds. */
static EGLBoolean swap_gbm(EglSurface *s) {
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_SWAP;
  r.window = s->kernel_window;
  r.width = (uint32_t)s->width;
  r.height = (uint32_t)s->height;
  if (gl_call(&r) < 0) return fail(EGL_BAD_SURFACE);
  struct gbm_bo *bo = tcos_gbm_surface_take(s->gbm);
  if (!bo) return fail(tcos_gbm_surface_info(s->gbm, NULL, NULL, NULL) ? EGL_BAD_ALLOC : EGL_BAD_NATIVE_WINDOW);      /* (every buffer held, or the gbm_surface is gone) */
  const int fd = gbm_bo_get_fd(bo);
  const int bad = fd < 0 || front_to_buffer(s, fd) != 0;
  if (fd >= 0) close(fd);
  if (bad) {
    tcos_gbm_surface_untake(s->gbm, bo);
    return fail(EGL_BAD_SURFACE);
  }
  tcos_gbm_surface_queue(s->gbm, bo);
  return ok();
}

static EGLBoolean swap(EGLDisplay handle, EGLSurface surface, const EGLint *rects, EGLint count) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  ThreadGl *t = thread_gl();
  pthread_mutex_lock(&d->lock);
  EglSurface *s = find_surface(d, surface);
  pthread_mutex_unlock(&d->lock);
  if (!s) return fail(EGL_BAD_SURFACE);
  if (t->draw != s || !t->context) return fail(EGL_BAD_SURFACE);      /* (only the calling thread's drawing surface) */
  if (count < 0 || (count > 0 && !rects)) return fail(EGL_BAD_PARAMETER);
  gl_stream_flush();
  if (!s->window_surface) return ok();      /* (a pbuffer has no back buffer to show) */
  if (s->gbm) return swap_gbm(s);
  struct wl_egl_window *window = s->native;
  if (!window) return fail(EGL_BAD_NATIVE_WINDOW);
  /* the frame before must have been shown first (swap interval 1) */
  while (s->swap_interval > 0 && s->frame)
    if (wl_display_dispatch_queue(d->wl, s->queue) < 0) return fail(EGL_BAD_NATIVE_WINDOW);
  wl_display_dispatch_queue_pending(d->wl, s->queue);      /* (releases that came in meanwhile) */
  /* the kernel swaps its images, and the window's new size (if it changed) takes effect for the next frame */
  const EGLint new_width = window->width > 0 ? window->width : 1, new_height = window->height > 0 ? window->height : 1;
  TcosGlRequest r;
  memset(&r, 0, sizeof(r));
  r.op = OP_SWAP;
  r.window = s->kernel_window;
  r.width = (uint32_t)new_width;
  r.height = (uint32_t)new_height;
  if (gl_call(&r) < 0) return fail(EGL_BAD_SURFACE);
  memset(&r, 0, sizeof(r));
  r.op = OP_READ_FRONT;
  r.window = s->kernel_window;
  if (gl_call(&r) < 0 || !r.width || !r.height) return fail(EGL_BAD_SURFACE);
  const uint32_t width = r.width, height = r.height;
  s->width = new_width;
  s->height = new_height;
  ShmBuffer *b = free_buffer(s, width, height);
  if (!b) return fail(EGL_BAD_ALLOC);
  if (b->bo) {
    /* a GPU buffer: the kernel copies the picture into its image */
    if (front_to_buffer(s, b->fd) != 0) return fail(EGL_BAD_SURFACE);
  } else {
    /* the picture, top row first, B G R A bytes: ARGB8888 / XRGB8888 as wl_shm has them */
    r.data = (uint64_t)(uintptr_t)b->data;
    r.length = (uint32_t)b->bytes;
    if (gl_call(&r) < 0) return fail(EGL_BAD_SURFACE);
  }
  const uint32_t version = wl_proxy_get_version((struct wl_proxy *)s->surface);
  if (version >= 5) {
    wl_surface_attach(s->surface, b->buffer, 0, 0);
    if (s->dx || s->dy) wl_surface_offset(s->surface, s->dx, s->dy);
  } else {
    wl_surface_attach(s->surface, b->buffer, s->dx, s->dy);
  }
  s->dx = s->dy = 0;
  if (count == 0 || version < 4) {
    if (version >= 4) wl_surface_damage_buffer(s->surface, 0, 0, INT32_MAX, INT32_MAX);
    else wl_surface_damage(s->surface, 0, 0, INT32_MAX, INT32_MAX);
  } else {
    /* EGL's rectangles count from the bottom row, the buffer's from the top */
    for (EGLint i = 0; i < count; ++i) {
      const EGLint *q = rects + 4 * i;
      wl_surface_damage_buffer(s->surface, q[0], (int32_t)height - q[1] - q[3], q[2], q[3]);
    }
  }
  if (s->swap_interval > 0) {
    s->frame = wl_surface_frame(s->surface);
    if (s->frame) wl_callback_add_listener(s->frame, &kFrameListener, s);
  }
  wl_surface_commit(s->surface);
  b->busy = 1;
  window->attached_width = (int)width;
  window->attached_height = (int)height;
  if (wl_display_flush(d->wl) < 0 && errno != EAGAIN) return fail(EGL_BAD_NATIVE_WINDOW);
  return ok();
}

EGLAPI EGLBoolean eglSwapBuffers(EGLDisplay handle, EGLSurface surface) { return swap(handle, surface, NULL, 0); }
EGLAPI EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay handle, EGLSurface surface, const EGLint *rects, EGLint count) { return swap(handle, surface, rects, count); }
EGLAPI EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay handle, EGLSurface surface, const EGLint *rects, EGLint count) { return swap(handle, surface, rects, count); }

EGLAPI EGLBoolean eglSwapInterval(EGLDisplay handle, EGLint interval) {
  if (!ready_display(handle)) return EGL_FALSE;
  ThreadGl *t = thread_gl();
  if (!t->context) return fail(EGL_BAD_CONTEXT);
  if (!t->draw) return fail(EGL_BAD_SURFACE);
  t->draw->swap_interval = interval < 0 ? 0 : interval > 1 ? 1 : interval;
  return ok();
}

/* ---- Fences (EGL 1.5, EGL_KHR_fence_sync). The kernel runs every command before the call that sent it returns, so a fence is signalled once what was queued
 * before it is sent: it is made signalled. */
typedef struct EglSync {
  struct EglSync *next;
  EglDisplay *display;
} EglSync;
static pthread_mutex_t g_syncs_lock = PTHREAD_MUTEX_INITIALIZER;
static EglSync *g_syncs;

static EglSync *find_sync(EglDisplay *d, EGLSync handle) {
  for (EglSync *s = g_syncs; s; s = s->next)
    if (s == (EglSync *)handle && s->display == d) return s;
  return NULL;
}

static EGLSync create_sync(EGLDisplay handle, EGLenum type, int has_attribs) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_NO_SYNC;
  if (type != EGL_SYNC_FENCE) { fail(EGL_BAD_PARAMETER); return EGL_NO_SYNC; }
  if (has_attribs) { fail(EGL_BAD_ATTRIBUTE); return EGL_NO_SYNC; }
  ThreadGl *t = thread_gl();
  if (!t->context || t->display != d) { fail(EGL_BAD_MATCH); return EGL_NO_SYNC; }
  gl_stream_flush();
  EglSync *s = (EglSync *)calloc(1, sizeof(EglSync));
  if (!s) { fail(EGL_BAD_ALLOC); return EGL_NO_SYNC; }
  s->display = d;
  pthread_mutex_lock(&g_syncs_lock);
  s->next = g_syncs;
  g_syncs = s;
  pthread_mutex_unlock(&g_syncs_lock);
  ok();
  return s;
}
EGLAPI EGLSync eglCreateSync(EGLDisplay handle, EGLenum type, const EGLAttrib *attribs) { return create_sync(handle, type, attribs && attribs[0] != EGL_NONE); }
EGLAPI EGLSync eglCreateSyncKHR(EGLDisplay handle, EGLenum type, const EGLint *attribs) { return create_sync(handle, type, attribs && attribs[0] != EGL_NONE); }

EGLAPI EGLBoolean eglDestroySync(EGLDisplay handle, EGLSync sync) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&g_syncs_lock);
  EglSync **at = &g_syncs;
  while (*at && !(*at == (EglSync *)sync && (*at)->display == d)) at = &(*at)->next;
  EglSync *s = *at;
  if (s) *at = s->next;
  pthread_mutex_unlock(&g_syncs_lock);
  if (!s) return fail(EGL_BAD_PARAMETER);
  free(s);
  return ok();
}
EGLAPI EGLBoolean eglDestroySyncKHR(EGLDisplay handle, EGLSync sync) { return eglDestroySync(handle, sync); }

EGLAPI EGLint eglClientWaitSync(EGLDisplay handle, EGLSync sync, EGLint flags, EGLTime timeout) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&g_syncs_lock);
  EglSync *s = find_sync(d, sync);
  pthread_mutex_unlock(&g_syncs_lock);
  if (!s) return fail(EGL_BAD_PARAMETER);
  ok();
  return EGL_CONDITION_SATISFIED;
}
EGLAPI EGLint eglClientWaitSyncKHR(EGLDisplay handle, EGLSync sync, EGLint flags, EGLTime timeout) { return eglClientWaitSync(handle, sync, flags, timeout); }
EGLAPI EGLBoolean eglWaitSync(EGLDisplay handle, EGLSync sync, EGLint flags) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&g_syncs_lock);
  EglSync *s = find_sync(d, sync);
  pthread_mutex_unlock(&g_syncs_lock);
  if (!s) return fail(EGL_BAD_PARAMETER);
  return flags == 0 ? ok() : fail(EGL_BAD_PARAMETER);
}

static EGLBoolean sync_attrib(EGLDisplay handle, EGLSync sync, EGLint name, EGLAttrib *value) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&g_syncs_lock);
  EglSync *s = find_sync(d, sync);
  pthread_mutex_unlock(&g_syncs_lock);
  if (!s) return fail(EGL_BAD_PARAMETER);
  if (!value) return fail(EGL_BAD_PARAMETER);
  switch (name) {
    case EGL_SYNC_TYPE: *value = EGL_SYNC_FENCE; return ok();
    case EGL_SYNC_STATUS: *value = EGL_SIGNALED; return ok();
    case EGL_SYNC_CONDITION: *value = EGL_SYNC_PRIOR_COMMANDS_COMPLETE; return ok();
    default: return fail(EGL_BAD_ATTRIBUTE);
  }
}
EGLAPI EGLBoolean eglGetSyncAttrib(EGLDisplay handle, EGLSync sync, EGLint name, EGLAttrib *value) { return sync_attrib(handle, sync, name, value); }
EGLAPI EGLBoolean eglGetSyncAttribKHR(EGLDisplay handle, EGLSync sync, EGLint name, EGLint *value) {
  EGLAttrib v = 0;
  if (!value) return fail(EGL_BAD_PARAMETER);
  const EGLBoolean r = sync_attrib(handle, sync, name, &v);
  if (r) *value = (EGLint)v;
  return r;
}

/* ---- Images (EGL 1.5, EGL_KHR_image_base): dmabufs (EGL_EXT_image_dma_buf_import) of one plane in an 8-bit RGB format, linear. An image holds a duplicate of
 * the descriptor; glEGLImageTargetTexture2DOES copies its pixels into the bound texture through the dmabuf's mapping (between DMA_BUF_IOCTL_SYNC start and end). */
#define DRM_FORMAT(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static const uint32_t kImageFormats[] = {DRM_FORMAT('A', 'R', '2', '4'), DRM_FORMAT('X', 'R', '2', '4'), DRM_FORMAT('A', 'B', '2', '4'), DRM_FORMAT('X', 'B', '2', '4')};
#define DRM_MOD_LINEAR 0ull
#define DRM_MOD_INVALID 0x00ffffffffffffffull

typedef struct EglImage {
  struct EglImage *next;
  EglDisplay *display;
  int fd;
  uint32_t width, height, format, offset, pitch;
} EglImage;
static pthread_mutex_t g_images_lock = PTHREAD_MUTEX_INITIALIZER;
static EglImage *g_images;

static int image_format_ok(uint32_t format) {
  for (size_t i = 0; i < sizeof(kImageFormats) / sizeof(kImageFormats[0]); ++i)
    if (kImageFormats[i] == format) return 1;
  return 0;
}

static EGLImage create_image(EGLDisplay handle, EGLContext context, EGLenum target, EGLClientBuffer buffer, const EGLint *ints, const EGLAttrib *attribs) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_NO_IMAGE;
  if (target != EGL_LINUX_DMA_BUF_EXT) { fail(EGL_BAD_PARAMETER); return EGL_NO_IMAGE; }
  if (context != EGL_NO_CONTEXT || buffer) { fail(EGL_BAD_PARAMETER); return EGL_NO_IMAGE; }      /* (EGL_EXT_image_dma_buf_import: no context, no client buffer) */
  int64_t width = -1, height = -1, format = -1, fd = -1, offset = 0, pitch = -1;
  uint64_t modifier_lo = 0, modifier_hi = 0;
  int has_modifier = 0;
  for (int i = 0;; i += 2) {
    const EGLint name = ints ? ints[i] : attribs ? (EGLint)attribs[i] : EGL_NONE;
    if (name == EGL_NONE) break;
    const int64_t value = ints ? ints[i + 1] : (int64_t)attribs[i + 1];
    switch (name) {
      case EGL_WIDTH: width = value; break;
      case EGL_HEIGHT: height = value; break;
      case EGL_LINUX_DRM_FOURCC_EXT: format = value; break;
      case EGL_DMA_BUF_PLANE0_FD_EXT: fd = value; break;
      case EGL_DMA_BUF_PLANE0_OFFSET_EXT: offset = value; break;
      case EGL_DMA_BUF_PLANE0_PITCH_EXT: pitch = value; break;
      case EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT: modifier_lo = (uint32_t)value; has_modifier = 1; break;
      case EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT: modifier_hi = (uint32_t)value; has_modifier = 1; break;
      case EGL_IMAGE_PRESERVED_KHR: case EGL_YUV_COLOR_SPACE_HINT_EXT: case EGL_SAMPLE_RANGE_HINT_EXT: case EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT:
      case EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT: break;
      default:
        if (name >= EGL_DMA_BUF_PLANE1_FD_EXT && name <= EGL_DMA_BUF_PLANE1_FD_EXT + 5) { fail(EGL_BAD_MATCH); return EGL_NO_IMAGE; }      /* (planes 1 and 2) */
        fail(EGL_BAD_ATTRIBUTE);
        return EGL_NO_IMAGE;
    }
  }
  if (width < 0 || height < 0 || format < 0 || fd < 0 || pitch < 0) { fail(EGL_BAD_PARAMETER); return EGL_NO_IMAGE; }      /* (a required attribute is missing) */
  const uint64_t modifier = modifier_hi << 32 | modifier_lo;
  if (!image_format_ok((uint32_t)format) || (has_modifier && modifier != DRM_MOD_LINEAR && modifier != DRM_MOD_INVALID)) { fail(EGL_BAD_MATCH); return EGL_NO_IMAGE; }
  if (width == 0 || height == 0 || width > 16384 || height > 16384 || offset < 0 || offset > 0xFFFFFFFFll || pitch > 0xFFFFFFFFll || pitch < width * 4) {
    fail(EGL_BAD_ACCESS);
    return EGL_NO_IMAGE;
  }
  /* the plane must be inside the descriptor's bytes */
  const off_t end = lseek((int)fd, 0, SEEK_END);
  if (end < 0 || (uint64_t)offset > (uint64_t)end || (uint64_t)pitch * (uint64_t)(height - 1) + (uint64_t)width * 4 > (uint64_t)end - (uint64_t)offset) {
    fail(EGL_BAD_ACCESS);
    return EGL_NO_IMAGE;
  }
  EglImage *image = (EglImage *)calloc(1, sizeof(EglImage));
  const int dup_fd = image ? fcntl((int)fd, F_DUPFD_CLOEXEC, 0) : -1;
  if (!image || dup_fd < 0) {
    free(image);
    fail(EGL_BAD_ALLOC);
    return EGL_NO_IMAGE;
  }
  image->display = d;
  image->fd = dup_fd;
  image->width = (uint32_t)width;
  image->height = (uint32_t)height;
  image->format = (uint32_t)format;
  image->offset = (uint32_t)offset;
  image->pitch = (uint32_t)pitch;
  pthread_mutex_lock(&g_images_lock);
  image->next = g_images;
  g_images = image;
  pthread_mutex_unlock(&g_images_lock);
  ok();
  return image;
}

EGLAPI EGLImage eglCreateImage(EGLDisplay handle, EGLContext context, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attribs) {
  static const EGLAttrib none[] = {EGL_NONE};
  return create_image(handle, context, target, buffer, NULL, attribs ? attribs : none);
}
EGLAPI EGLImage eglCreateImageKHR(EGLDisplay handle, EGLContext context, EGLenum target, EGLClientBuffer buffer, const EGLint *attribs) {
  static const EGLint none[] = {EGL_NONE};
  return create_image(handle, context, target, buffer, attribs ? attribs : none, NULL);
}

EGLAPI EGLBoolean eglDestroyImage(EGLDisplay handle, EGLImage image) {
  EglDisplay *d = ready_display(handle);
  if (!d) return EGL_FALSE;
  pthread_mutex_lock(&g_images_lock);
  EglImage **at = &g_images;
  while (*at && !(*at == (EglImage *)image && (*at)->display == d)) at = &(*at)->next;
  EglImage *found = *at;
  if (found) *at = found->next;
  pthread_mutex_unlock(&g_images_lock);
  if (!found) return fail(EGL_BAD_PARAMETER);
  close(found->fd);
  free(found);
  return ok();
}
EGLAPI EGLBoolean eglDestroyImageKHR(EGLDisplay handle, EGLImage image) { return eglDestroyImage(handle, image); }

/* EGL_EXT_image_dma_buf_import_modifiers: the formats and the one layout (linear) */
EGLAPI EGLBoolean eglQueryDmaBufFormatsEXT(EGLDisplay handle, EGLint max, EGLint *formats, EGLint *count) {
  if (!ready_display(handle)) return EGL_FALSE;
  const EGLint n = (EGLint)(sizeof(kImageFormats) / sizeof(kImageFormats[0]));
  if (max < 0 || (max > 0 && !formats) || !count) return fail(EGL_BAD_PARAMETER);
  if (max == 0) { *count = n; return ok(); }
  EGLint i = 0;
  for (; i < n && i < max; ++i) formats[i] = (EGLint)kImageFormats[i];
  *count = i;
  return ok();
}
EGLAPI EGLBoolean eglQueryDmaBufModifiersEXT(EGLDisplay handle, EGLint format, EGLint max, uint64_t *modifiers, EGLBoolean *external, EGLint *count) {
  if (!ready_display(handle)) return EGL_FALSE;
  if (!image_format_ok((uint32_t)format)) return fail(EGL_BAD_PARAMETER);
  if (max < 0 || (max > 0 && !modifiers) || !count) return fail(EGL_BAD_PARAMETER);
  if (max == 0) { *count = 1; return ok(); }
  modifiers[0] = DRM_MOD_LINEAR;
  if (external) external[0] = EGL_FALSE;
  *count = 1;
  return ok();
}

/* GL_OES_EGL_image: the image's pixels into the texture bound to `target` (level 0, RGBA); GL_INVALID_OPERATION (as the extension says) when it cannot be read. */
enum { GL_TEXTURE_2D_ = 0x0DE1, GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401 };
static int read_image(const EglImage *image, uint8_t *rgba) {
  const off_t end = lseek(image->fd, 0, SEEK_END);
  if (end <= 0) return 0;
  uint8_t *map = (uint8_t *)mmap(NULL, (size_t)end, PROT_READ, MAP_SHARED, image->fd, 0);
  if (map == MAP_FAILED) return 0;
  uint64_t sync = 1;      /* DMA_BUF_SYNC_READ | START */
  ioctl(image->fd, 0x40086200ul, &sync);      /* (DMA_BUF_IOCTL_SYNC; a file in memory has none, and needs none) */
  const uint32_t f = image->format;
  const int swap_rb = f == DRM_FORMAT('A', 'R', '2', '4') || f == DRM_FORMAT('X', 'R', '2', '4');      /* (B, G, R, A bytes) */
  const int opaque = f == DRM_FORMAT('X', 'R', '2', '4') || f == DRM_FORMAT('X', 'B', '2', '4');
  for (uint32_t y = 0; y < image->height; ++y) {
    const uint8_t *src = map + image->offset + (size_t)y * image->pitch;
    uint8_t *dst = rgba + (size_t)y * image->width * 4;
    for (uint32_t x = 0; x < image->width; ++x, src += 4, dst += 4) {
      dst[0] = swap_rb ? src[2] : src[0];
      dst[1] = src[1];
      dst[2] = swap_rb ? src[0] : src[2];
      dst[3] = opaque ? 255 : src[3];
    }
  }
  sync = 1 | 4;      /* READ | END */
  ioctl(image->fd, 0x40086200ul, &sync);
  munmap(map, (size_t)end);
  return 1;
}

/* GL_INVALID_OPERATION in the current context: a query begun with name 0 is that error, and changes nothing else */
static void gl_invalid_operation(void) { glBeginQuery(0x8C2F /* GL_ANY_SAMPLES_PASSED */, 0); }

GLAPI void APIENTRY glEGLImageTargetTexture2DOES(GLenum target, void *handle) {
  ThreadGl *t = thread_gl();
  if (!t->context) return;
  pthread_mutex_lock(&g_images_lock);
  EglImage *image = g_images;
  while (image && image != (EglImage *)handle) image = image->next;
  EglImage copy;
  int found = image != NULL;
  int fd = -1;
  if (found) {
    copy = *image;
    fd = fcntl(image->fd, F_DUPFD_CLOEXEC, 0);      /* (the image may be destroyed by another thread meanwhile) */
  }
  pthread_mutex_unlock(&g_images_lock);
  if (!found || fd < 0 || target != GL_TEXTURE_2D_) {
    if (fd >= 0) close(fd);
    gl_invalid_operation();
    return;
  }
  copy.fd = fd;
  uint8_t *rgba = (uint8_t *)malloc((size_t)copy.width * copy.height * 4);
  const int good = rgba && read_image(&copy, rgba);
  close(fd);
  if (!good) {
    free(rgba);
    gl_invalid_operation();
    return;
  }
  glTexImage2D(target, 0, GL_RGBA_, (GLsizei)copy.width, (GLsizei)copy.height, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);
  gl_stream_flush();      /* (the kernel reads the pixels before they are freed) */
  free(rgba);
}
/* (no renderbuffers from images) */
GLAPI void APIENTRY glEGLImageTargetRenderbufferStorageOES(GLenum target, void *handle) { gl_invalid_operation(); }

/* ---- eglGetProcAddress: every command of the kernel's GL and every EGL function, by name (EGL_KHR_get_all_proc_addresses). */
typedef struct { const char *name; EGLProc proc; } ProcEntry;
static const ProcEntry g_gl_procs[] = {
#define GL_PROC(name) {#name, (EGLProc)(void *)name},
#include "../../OpenGl/generated/GlProcs.inc"
#undef GL_PROC
};
#define EGL_PROC(name) {#name, (EGLProc)(void *)name}
static const ProcEntry g_egl_procs[] = {
    EGL_PROC(eglBindAPI), EGL_PROC(eglBindTexImage), EGL_PROC(eglChooseConfig), EGL_PROC(eglClientWaitSync), EGL_PROC(eglClientWaitSyncKHR), EGL_PROC(eglCopyBuffers),
    EGL_PROC(eglCreateContext), EGL_PROC(eglCreateImage), EGL_PROC(eglCreatePbufferFromClientBuffer), EGL_PROC(eglCreatePbufferSurface), EGL_PROC(eglCreatePixmapSurface),
    EGL_PROC(eglCreatePlatformPixmapSurface), EGL_PROC(eglCreatePlatformWindowSurface), EGL_PROC(eglCreatePlatformWindowSurfaceEXT), EGL_PROC(eglCreateSync),
    EGL_PROC(eglCreateSyncKHR), EGL_PROC(eglCreateWindowSurface), EGL_PROC(eglDestroyContext), EGL_PROC(eglDestroyImage), EGL_PROC(eglDestroySurface),
    EGL_PROC(eglDestroySync), EGL_PROC(eglDestroySyncKHR), EGL_PROC(eglGetConfigAttrib), EGL_PROC(eglGetConfigs), EGL_PROC(eglGetCurrentContext),
    EGL_PROC(eglGetCurrentDisplay), EGL_PROC(eglGetCurrentSurface), EGL_PROC(eglGetDisplay), EGL_PROC(eglGetError), EGL_PROC(eglGetPlatformDisplay),
    EGL_PROC(eglGetPlatformDisplayEXT), EGL_PROC(eglGetSyncAttrib), EGL_PROC(eglGetSyncAttribKHR), EGL_PROC(eglInitialize), EGL_PROC(eglMakeCurrent),
    EGL_PROC(eglQueryAPI), EGL_PROC(eglQueryContext), EGL_PROC(eglQueryString), EGL_PROC(eglQuerySurface), EGL_PROC(eglReleaseTexImage), EGL_PROC(eglReleaseThread),
    EGL_PROC(eglSurfaceAttrib), EGL_PROC(eglSwapBuffers), EGL_PROC(eglSwapBuffersWithDamageEXT), EGL_PROC(eglSwapBuffersWithDamageKHR), EGL_PROC(eglSwapInterval),
    EGL_PROC(eglTerminate), EGL_PROC(eglWaitClient), EGL_PROC(eglWaitGL), EGL_PROC(eglWaitNative), EGL_PROC(eglWaitSync),
    EGL_PROC(eglCreateImageKHR), EGL_PROC(eglDestroyImageKHR), EGL_PROC(eglQueryDmaBufFormatsEXT), EGL_PROC(eglQueryDmaBufModifiersEXT),
    EGL_PROC(glEGLImageTargetTexture2DOES), EGL_PROC(glEGLImageTargetRenderbufferStorageOES),
};
#undef EGL_PROC

EGLAPI EGLProc eglGetProcAddress(const char *name) {
  if (!name) return NULL;
  size_t lo = 0, hi = sizeof(g_gl_procs) / sizeof(g_gl_procs[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(g_gl_procs[mid].name, name);
    if (c == 0) return g_gl_procs[mid].proc;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  for (size_t i = 0; i < sizeof(g_egl_procs) / sizeof(g_egl_procs[0]); ++i)
    if (strcmp(g_egl_procs[i].name, name) == 0) return g_egl_procs[i].proc;
  return NULL;
}
