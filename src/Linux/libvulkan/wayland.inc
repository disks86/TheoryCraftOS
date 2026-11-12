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

/* VK_KHR_wayland_surface for libvulkan (included by libvulkan.c): a swapchain whose images a Wayland compositor shows. Each image is a shareable allocation of the
 * Direct3D layer, exported once as a dmabuf of /dev/dxg and made a wl_buffer with zwp_linux_buffer_params_v1.create_immed (linear, plane 0 at offset 0, the
 * allocation's own row pitch; XRGB8888, or ARGB8888 for a premultiplied swapchain). A present attaches the image's buffer, damages all of it and commits; the
 * compositor draws the image where it is (the kernel's compositor samples it on the device). An image is free again when its buffer is released.
 *
 * The protocol goes through the program's own libwayland-client (the wl_display it passes is one of that library's): the functions are looked up with dlsym in
 * the copy the program loaded, so libvulkan does not link against it and a program that never makes a Wayland surface does not need it. Every object the
 * swapchain makes lives on an event queue of its own, so the program's dispatching never runs the swapchain's handlers and the other way round.
 *
 * Present modes: FIFO waits for the frame callback of the previous present before it attaches (one frame a refresh of the compositor); MAILBOX and IMMEDIATE
 * attach at once. A finite timeout of vkAcquireNextImageKHR is not kept: it waits until the compositor gives an image back (0 still answers VK_NOT_READY). */

/* ---- libwayland-client, as wayland-util.h / wayland-client-core.h describe it --------------------------------------------------------------------------------------- */
struct wl_display;
struct wl_surface;
struct wl_proxy;
struct wl_event_queue;
struct wl_interface;
struct wl_message {
  const char *name;
  const char *signature;
  const struct wl_interface **types;
};
struct wl_interface {
  const char *name;
  int version;
  int method_count;
  const struct wl_message *methods;
  int event_count;
  const struct wl_message *events;
};
#include "../../Vulkan/khronos/vulkan/vulkan_wayland.h"

enum { WL_MARSHAL_FLAG_DESTROY = 1, RTLD_NOW = 2, RTLD_NOLOAD = 4 };
enum { WL_DISPLAY_GET_REGISTRY = 1, WL_REGISTRY_BIND = 0, WL_SURFACE_ATTACH = 1, WL_SURFACE_DAMAGE = 2, WL_SURFACE_FRAME = 3, WL_SURFACE_COMMIT = 6,
       WL_SURFACE_DAMAGE_BUFFER = 9, WL_BUFFER_DESTROY = 0, DMABUF_DESTROY = 0, DMABUF_CREATE_PARAMS = 1, PARAMS_DESTROY = 0, PARAMS_ADD = 1,
       PARAMS_CREATE_IMMED = 3 };
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ARGB8888 0x34325241u
#define WL_FULL_DAMAGE 0x7FFFFFFF

/* The C library's dynamic loader (weak: a program without one has no libwayland-client either, and gets VK_ERROR_INITIALIZATION_FAILED). */
extern void *dlopen(const char *file, int mode) __attribute__((weak));
extern void *dlsym(void *handle, const char *name) __attribute__((weak));

typedef struct WaylandApi {
  struct wl_proxy *(*marshal_flags)(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, uint32_t flags, ...);
  int (*add_listener)(struct wl_proxy *proxy, void (**implementation)(void), void *data);
  void (*proxy_destroy)(struct wl_proxy *proxy);
  uint32_t (*get_version)(struct wl_proxy *proxy);
  void *(*create_wrapper)(void *proxy);
  void (*wrapper_destroy)(void *wrapper);
  void (*set_queue)(struct wl_proxy *proxy, struct wl_event_queue *queue);
  struct wl_event_queue *(*create_queue)(struct wl_display *display);
  void (*queue_destroy)(struct wl_event_queue *queue);
  int (*roundtrip_queue)(struct wl_display *display, struct wl_event_queue *queue);
  int (*dispatch_queue)(struct wl_display *display, struct wl_event_queue *queue);
  int (*dispatch_queue_pending)(struct wl_display *display, struct wl_event_queue *queue);
  int (*flush)(struct wl_display *display);
  int (*get_error)(struct wl_display *display);
  const struct wl_interface *registry, *buffer, *callback;
} WaylandApi;

static WaylandApi g_wl;
static const struct wl_interface *kImmedTypes[] = {NULL, NULL, NULL, NULL, NULL};      /* (create_immed's wl_buffer: libwayland's interface, set when it is found) */
static int g_wl_state;      /* 0: not looked yet, 1: looking, 2: usable, 3: not there */

static bool wayland_load(void) {
  int state = __atomic_load_n(&g_wl_state, __ATOMIC_ACQUIRE);
  while (state < 2) {
    int expected = 0;
    if (state == 0 && __atomic_compare_exchange_n(&g_wl_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      bool ok = false;
      if (dlopen && dlsym) {
        void *lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);      /* (the program's copy: its wl_display is one of that library's) */
        if (!lib) lib = dlopen("libwayland-client.so.0", RTLD_NOW);
        if (lib) {
          ok = true;
#define WL_FIND(field, symbol)                         \
  do {                                                 \
    void *f = dlsym(lib, symbol);                      \
    if (!f) ok = false;                                \
    __builtin_memcpy(&g_wl.field, &f, sizeof f);       \
  } while (0)
          WL_FIND(marshal_flags, "wl_proxy_marshal_flags");      /* (libwayland 1.20 and later) */
          WL_FIND(add_listener, "wl_proxy_add_listener");
          WL_FIND(proxy_destroy, "wl_proxy_destroy");
          WL_FIND(get_version, "wl_proxy_get_version");
          WL_FIND(create_wrapper, "wl_proxy_create_wrapper");
          WL_FIND(wrapper_destroy, "wl_proxy_wrapper_destroy");
          WL_FIND(set_queue, "wl_proxy_set_queue");
          WL_FIND(create_queue, "wl_display_create_queue");
          WL_FIND(queue_destroy, "wl_event_queue_destroy");
          WL_FIND(roundtrip_queue, "wl_display_roundtrip_queue");
          WL_FIND(dispatch_queue, "wl_display_dispatch_queue");
          WL_FIND(dispatch_queue_pending, "wl_display_dispatch_queue_pending");
          WL_FIND(flush, "wl_display_flush");
          WL_FIND(get_error, "wl_display_get_error");
          WL_FIND(registry, "wl_registry_interface");
          WL_FIND(buffer, "wl_buffer_interface");
          WL_FIND(callback, "wl_callback_interface");
#undef WL_FIND
          kImmedTypes[0] = g_wl.buffer;      /* (create_immed's and created's new wl_buffer) */
        }
      }
      if (!ok) vk_os_trace("vkCreateWaylandSurfaceKHR: libwayland-client.so.0 (1.20 or later) is not in the program\n");
      __atomic_store_n(&g_wl_state, ok ? 2 : 3, __ATOMIC_RELEASE);
      return ok;
    }
    state = __atomic_load_n(&g_wl_state, __ATOMIC_ACQUIRE);      /* (another thread is looking) */
  }
  return state == 2;
}

/* zwp_linux_dmabuf_v1 and zwp_linux_buffer_params_v1 (linux-dmabuf-unstable-v1.xml): the messages the swapchain sends, and every event the compositor may send
 * on them (libwayland reads an event by its signature even when nobody listens). */
static const struct wl_interface kDmabufInterface, kParamsInterface;
static const struct wl_interface *kParamsTypes[] = {&kParamsInterface};
static const struct wl_interface *kNoTypes[] = {NULL, NULL, NULL, NULL, NULL, NULL};
static const struct wl_message kDmabufRequests[] = {{"destroy", "", kNoTypes}, {"create_params", "n", kParamsTypes}};
static const struct wl_message kDmabufEvents[] = {{"format", "u", kNoTypes}, {"modifier", "3uuu", kNoTypes}};
static const struct wl_interface kDmabufInterface = {"zwp_linux_dmabuf_v1", 4, 2, kDmabufRequests, 2, kDmabufEvents};
static const struct wl_message kParamsRequests[] = {
    {"destroy", "", kNoTypes}, {"add", "huuuuu", kNoTypes}, {"create", "iiuu", kNoTypes}, {"create_immed", "2niiuu", kImmedTypes}};
static const struct wl_message kParamsEvents[] = {{"created", "n", kImmedTypes}, {"failed", "", kNoTypes}};
static const struct wl_interface kParamsInterface = {"zwp_linux_buffer_params_v1", 4, 4, kParamsRequests, 2, kParamsEvents};

/* The state of a Wayland surface (Surface.os) and of a swapchain of one (Swapchain.os). */
enum { SURFACE_WAYLAND = 2 };
typedef struct WlSurface {
  struct wl_display *display;
  struct wl_surface *surface;
} WlSurface;

typedef struct WlSwapchain WlSwapchain;
typedef struct WlSlot {
  WlSwapchain *owner;
  struct wl_proxy *buffer;
  bool busy;      /* the compositor has it (attached, not released yet) */
  bool held;      /* the program has it (acquired, not presented yet) */
} WlSlot;
struct WlSwapchain {
  struct wl_display *display;
  struct wl_proxy *surface;            /* the program's wl_surface (attach, damage, commit) */
  struct wl_proxy *surface_wrapper;    /* the same on our queue (the frame callback) */
  struct wl_event_queue *queue;
  struct wl_proxy *dmabuf;
  struct wl_proxy *frame;              /* the frame callback of the last FIFO present, until done */
  uint32_t dmabuf_name, dmabuf_version, surface_version;
  bool lost;
  WlSlot slot[8];
};

/* ---- event handlers (they run inside wl_display_*_queue of the swapchain's own queue) ------------------------------------------------------------------------------- */
static void wl_global(void *data, struct wl_proxy *registry, uint32_t name, const char *interface, uint32_t version) {
  (void)registry;
  WlSwapchain *w = data;
  if (streq(interface, kDmabufInterface.name) && version >= 2) w->dmabuf_name = name, w->dmabuf_version = version < 4 ? version : 4;      /* (create_immed is version 2) */
}
static void wl_global_remove(void *data, struct wl_proxy *registry, uint32_t name) { (void)data, (void)registry, (void)name; }
static void (*const kRegistryListener[2])(void) = {(void (*)(void))wl_global, (void (*)(void))wl_global_remove};

static void wl_buffer_release(void *data, struct wl_proxy *buffer) {
  (void)buffer;
  ((WlSlot *)data)->busy = false;
}
static void (*const kBufferListener[1])(void) = {(void (*)(void))wl_buffer_release};

static void wl_frame_done(void *data, struct wl_proxy *callback, uint32_t time) {
  (void)time;
  WlSwapchain *w = data;
  if (w->frame == callback) w->frame = NULL;
  g_wl.proxy_destroy(callback);
}
static void (*const kFrameListener[1])(void) = {(void (*)(void))wl_frame_done};

/* ---- the surface ------------------------------------------------------------------------------------------------------------------------------------------------------ */
VKAPI VkResult VKAPI_CALL vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)instance, (void)alloc;
  if (!ci || !ci->display || !ci->surface) return VK_ERROR_INITIALIZATION_FAILED;
  if (!wayland_load()) return VK_ERROR_INITIALIZATION_FAILED;
  WlSurface *ws = vk_alloc(sizeof(WlSurface));
  if (!ws) return VK_ERROR_OUT_OF_HOST_MEMORY;
  ws->display = ci->display, ws->surface = ci->surface;
  VkResult r = make_surface(0, SURFACE_WAYLAND, out);
  if (r != VK_SUCCESS) {
    vk_free(ws);
    return r;
  }
  ((Surface *)*out)->os = ws;
  return VK_SUCCESS;
}

VKAPI VkBool32 VKAPI_CALL vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice pd, uint32_t family, struct wl_display *display) {
  (void)pd;
  return family == 0 && display != NULL && wayland_load();
}

/* ---- the swapchain ---------------------------------------------------------------------------------------------------------------------------------------------------- */
static void wl_swapchain_free(WlSwapchain *w) {
  for (uint32_t i = 0; i < 8; ++i)
    if (w->slot[i].buffer) g_wl.marshal_flags(w->slot[i].buffer, WL_BUFFER_DESTROY, NULL, g_wl.get_version(w->slot[i].buffer), WL_MARSHAL_FLAG_DESTROY);
  if (w->frame) g_wl.proxy_destroy(w->frame);      /* (wl_callback has no destroy request) */
  if (w->dmabuf) g_wl.marshal_flags(w->dmabuf, DMABUF_DESTROY, NULL, w->dmabuf_version, WL_MARSHAL_FLAG_DESTROY);
  if (w->surface_wrapper) g_wl.wrapper_destroy(w->surface_wrapper);
  g_wl.flush(w->display);
  if (w->queue) g_wl.queue_destroy(w->queue);
  vk_free(w);
}

static VkResult vk_os_swapchain_made(Swapchain *sc) {
  const WlSurface *ws = sc->surface->os;
  if (sc->n > 8) return VK_ERROR_INITIALIZATION_FAILED;
  WlSwapchain *w = vk_alloc(sizeof(WlSwapchain));
  if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
  w->display = ws->display, w->surface = (struct wl_proxy *)ws->surface;
  w->surface_version = g_wl.get_version(w->surface);
  w->queue = g_wl.create_queue(w->display);
  struct wl_proxy *display = g_wl.create_wrapper(w->display);
  w->surface_wrapper = g_wl.create_wrapper(w->surface);
  if (!w->queue || !display || !w->surface_wrapper) {
    if (display) g_wl.wrapper_destroy(display);
    wl_swapchain_free(w);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  g_wl.set_queue(display, w->queue);
  g_wl.set_queue(w->surface_wrapper, w->queue);
  /* the dmabuf global, through a registry of our own */
  struct wl_proxy *registry = g_wl.marshal_flags(display, WL_DISPLAY_GET_REGISTRY, g_wl.registry, g_wl.get_version(display), 0, NULL);
  g_wl.wrapper_destroy(display);
  if (!registry) {
    wl_swapchain_free(w);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  g_wl.add_listener(registry, (void (**)(void))kRegistryListener, w);
  if (g_wl.roundtrip_queue(w->display, w->queue) < 0 || !w->dmabuf_name) {
    vk_os_trace(w->dmabuf_name ? "vkCreateSwapchainKHR: the Wayland connection failed\n" : "vkCreateSwapchainKHR: the compositor has no zwp_linux_dmabuf_v1 (version 2 or later)\n");
    g_wl.proxy_destroy(registry);
    wl_swapchain_free(w);
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  w->dmabuf = g_wl.marshal_flags(registry, WL_REGISTRY_BIND, &kDmabufInterface, w->dmabuf_version, 0, w->dmabuf_name, kDmabufInterface.name, w->dmabuf_version, NULL);
  g_wl.proxy_destroy(registry);      /* (wl_registry has no destroy request) */
  if (!w->dmabuf) {
    wl_swapchain_free(w);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  /* a wl_buffer for each image */
  const uint32_t format = sc->premultiplied ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
  for (uint32_t i = 0; i < sc->n; ++i) {
    WlSlot *slot = &w->slot[i];
    slot->owner = w;
    int fd = -1;
    if (!vk_os_export_fd(sc->img[i]->alloc, &fd)) {
      wl_swapchain_free(w);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    struct wl_proxy *params = g_wl.marshal_flags(w->dmabuf, DMABUF_CREATE_PARAMS, &kParamsInterface, w->dmabuf_version, 0, NULL);
    if (params) {
      g_wl.marshal_flags(params, PARAMS_ADD, NULL, w->dmabuf_version, 0, fd, 0u, 0u, sc->img[i]->pitch, 0u, 0u);      /* plane 0, offset 0, linear */
      slot->buffer = g_wl.marshal_flags(params, PARAMS_CREATE_IMMED, g_wl.buffer, w->dmabuf_version, 0, NULL, (int32_t)sc->w, (int32_t)sc->h, format, 0u);
      g_wl.marshal_flags(params, PARAMS_DESTROY, NULL, w->dmabuf_version, WL_MARSHAL_FLAG_DESTROY);
    }
    vk_os_close_fd(fd);      /* (libwayland sent a copy) */
    if (!slot->buffer) {
      wl_swapchain_free(w);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    g_wl.add_listener(slot->buffer, (void (**)(void))kBufferListener, slot);
  }
  if (g_wl.roundtrip_queue(w->display, w->queue) < 0 || g_wl.get_error(w->display)) {      /* (a buffer the compositor cannot take ends the connection) */
    vk_os_trace("vkCreateSwapchainKHR: the compositor refused the swapchain's buffers\n");
    wl_swapchain_free(w);
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  sc->os = w;
  return VK_SUCCESS;
}

static void vk_os_swapchain_gone(Swapchain *sc) {
  if (sc->os) wl_swapchain_free(sc->os);
  sc->os = NULL;
}

static VkResult vk_os_acquire(Swapchain *sc, uint64_t timeout, uint32_t *index) {
  WlSwapchain *w = sc->os;
  if (g_wl.dispatch_queue_pending(w->display, w->queue) < 0) w->lost = true;      /* (releases that arrived since) */
  for (;;) {
    if (w->lost) return VK_ERROR_SURFACE_LOST_KHR;
    for (uint32_t k = 0; k < sc->n; ++k) {
      const uint32_t i = (sc->next + k) % sc->n;      /* (the oldest first) */
      if (!w->slot[i].busy && !w->slot[i].held) {
        w->slot[i].held = true;
        sc->next = (i + 1) % sc->n;
        *index = i;
        return VK_SUCCESS;
      }
    }
    if (timeout == 0) return VK_NOT_READY;
    if (g_wl.dispatch_queue(w->display, w->queue) < 0) w->lost = true;      /* (waits for the compositor to give one back) */
  }
}

static VkResult vk_os_present(Swapchain *sc, uint32_t index) {
  WlSwapchain *w = sc->os;
  WlSlot *slot = &w->slot[index];
  while (!w->lost && w->frame)      /* FIFO: the previous frame has been shown first */
    if (g_wl.dispatch_queue(w->display, w->queue) < 0) w->lost = true;
  if (w->lost) return VK_ERROR_SURFACE_LOST_KHR;
  if (sc->mode == VK_PRESENT_MODE_FIFO_KHR || sc->mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
    w->frame = g_wl.marshal_flags(w->surface_wrapper, WL_SURFACE_FRAME, g_wl.callback, w->surface_version, 0, NULL);
    if (w->frame) g_wl.add_listener(w->frame, (void (**)(void))kFrameListener, w);
  }
  g_wl.marshal_flags(w->surface, WL_SURFACE_ATTACH, NULL, w->surface_version, 0, slot->buffer, 0, 0);
  if (w->surface_version >= 4) g_wl.marshal_flags(w->surface, WL_SURFACE_DAMAGE_BUFFER, NULL, w->surface_version, 0, 0, 0, WL_FULL_DAMAGE, WL_FULL_DAMAGE);
  else g_wl.marshal_flags(w->surface, WL_SURFACE_DAMAGE, NULL, w->surface_version, 0, 0, 0, WL_FULL_DAMAGE, WL_FULL_DAMAGE);
  g_wl.marshal_flags(w->surface, WL_SURFACE_COMMIT, NULL, w->surface_version, 0);
  slot->busy = true, slot->held = false;
  if (g_wl.flush(w->display) < 0 && g_wl.get_error(w->display)) {
    w->lost = true;
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  return VK_SUCCESS;
}
