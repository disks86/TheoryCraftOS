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

/* VK_KHR_xcb_surface and VK_KHR_xlib_surface for libvulkan (included by libvulkan.c): a swapchain shown in an X window (the kernel's X server, src/X11, or
 * any other). A present maps the image for the CPU (D3DKMTLock, read-only, after every submission before it has run) and sends its rows with
 * xcb_put_image (ZPixmap, the window's depth: B, G, R, X bytes as a 24- or 32-bit visual of a little-endian server takes them), in bands that fit the
 * server's largest request (BIG-REQUESTS when the server has it). Images are never held by the server, so one is free again as soon as it was presented.
 * FIFO waits for a round trip (GetInputFocus) after the present, so a program cannot queue frames faster than the server takes them; MAILBOX and IMMEDIATE
 * do not wait.
 *
 * The protocol goes through the program's own libxcb (an Xlib program's: XGetXCBConnection of libX11-xcb gives the xcb_connection_t of its Display): the
 * functions are looked up with dlsym in the copy the program loaded, as for Wayland. The surface's current extent is the window's size (GetGeometry);
 * an image is drawn at the window's top left whatever the window's size is now (the server clips it), and a resize is not reported as VK_SUBOPTIMAL_KHR. */

/* ---- libxcb, as xcb.h / xproto.h describe it ------------------------------------------------------------------------------------------------------------------------ */
/* (xcb_connection_t, xcb_window_t, Display and the Khronos xcb / xlib headers: at the top of libvulkan.c, before the driver's function table) */
typedef struct { unsigned int sequence; } xcb_cookie_t;
typedef struct {
  uint8_t response_type, depth;
  uint16_t sequence;
  uint32_t length, root;
  int16_t x, y;
  uint16_t width, height, border_width;
  uint8_t pad0[2];
} xcb_get_geometry_reply_t;
typedef struct {
  uint8_t response_type, error_code;
  uint16_t sequence;
  uint32_t resource_id;
  uint16_t minor_code;
  uint8_t major_code, pad0;
  uint32_t pad[5], full_sequence;
} xcb_generic_error_t;
enum { XCB_IMAGE_FORMAT_Z_PIXMAP = 2, X11_PUT_IMAGE_HEADER = 24, X11_BIG_HEADER = 28 };

typedef struct XcbApi {
  xcb_cookie_t (*get_geometry)(xcb_connection_t *c, uint32_t drawable);
  xcb_get_geometry_reply_t *(*get_geometry_reply)(xcb_connection_t *c, xcb_cookie_t cookie, xcb_generic_error_t **e);
  uint32_t (*generate_id)(xcb_connection_t *c);
  xcb_cookie_t (*create_gc)(xcb_connection_t *c, uint32_t gc, uint32_t drawable, uint32_t mask, const void *values);
  xcb_cookie_t (*free_gc)(xcb_connection_t *c, uint32_t gc);
  xcb_cookie_t (*put_image)(xcb_connection_t *c, uint8_t format, uint32_t drawable, uint32_t gc, uint16_t width, uint16_t height, int16_t x, int16_t y,
                            uint8_t left_pad, uint8_t depth, uint32_t length, const uint8_t *data);
  xcb_cookie_t (*get_input_focus)(xcb_connection_t *c);
  void *(*get_input_focus_reply)(xcb_connection_t *c, xcb_cookie_t cookie, xcb_generic_error_t **e);
  int (*flush)(xcb_connection_t *c);
  int (*has_error)(xcb_connection_t *c);
  uint32_t (*max_request_length)(xcb_connection_t *c);
  void (*free)(void *p);      /* (the C library's: xcb's replies are malloc'd) */
  xcb_connection_t *(*xlib_connection)(Display *display);      /* XGetXCBConnection (libX11-xcb), when there is one */
} XcbApi;

static XcbApi g_xcb;
static int g_xcb_state;      /* 0: not looked yet, 1: looking, 2: usable, 3: not there */

static void *x11_find(void *lib, const char *symbol, bool *ok) {
  void *f = dlsym(lib, symbol);
  if (!f) *ok = false;
  return f;
}

static bool xcb_load(void) {
  int state = __atomic_load_n(&g_xcb_state, __ATOMIC_ACQUIRE);
  while (state < 2) {
    int expected = 0;
    if (state == 0 && __atomic_compare_exchange_n(&g_xcb_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      bool ok = false;
      if (dlopen && dlsym) {
        void *lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_NOLOAD);      /* (the program's copy: its connection is one of that library's) */
        if (!lib) lib = dlopen("libxcb.so.1", RTLD_NOW);
        if (lib) {
          ok = true;
#define XCB_FIND(field, symbol)                         \
  do {                                                  \
    void *f = x11_find(lib, symbol, &ok);               \
    __builtin_memcpy(&g_xcb.field, &f, sizeof f);       \
  } while (0)
          XCB_FIND(get_geometry, "xcb_get_geometry");
          XCB_FIND(get_geometry_reply, "xcb_get_geometry_reply");
          XCB_FIND(generate_id, "xcb_generate_id");
          XCB_FIND(create_gc, "xcb_create_gc");
          XCB_FIND(free_gc, "xcb_free_gc");
          XCB_FIND(put_image, "xcb_put_image");
          XCB_FIND(get_input_focus, "xcb_get_input_focus");
          XCB_FIND(get_input_focus_reply, "xcb_get_input_focus_reply");
          XCB_FIND(flush, "xcb_flush");
          XCB_FIND(has_error, "xcb_connection_has_error");
          XCB_FIND(max_request_length, "xcb_get_maximum_request_length");
          XCB_FIND(free, "free");      /* (found through libxcb's own dependency: the C library that allocated its replies) */
          void *x11xcb = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_NOLOAD);
          if (!x11xcb) x11xcb = dlopen("libX11-xcb.so.1", RTLD_NOW);
          if (x11xcb) {
            void *f = dlsym(x11xcb, "XGetXCBConnection");
            __builtin_memcpy(&g_xcb.xlib_connection, &f, sizeof f);
          }
#undef XCB_FIND
        }
      }
      if (!ok) vk_os_trace("vkCreateXcbSurfaceKHR: libxcb.so.1 is not in the program\n");
      __atomic_store_n(&g_xcb_state, ok ? 2 : 3, __ATOMIC_RELEASE);
      return ok;
    }
    state = __atomic_load_n(&g_xcb_state, __ATOMIC_ACQUIRE);      /* (another thread is looking) */
  }
  return state == 2;
}

/* The state of an X surface (Surface.os) and of a swapchain of one (Swapchain.os). */
enum { SURFACE_XCB = 3, SURFACE_XLIB = 4 };
typedef struct X11Surface {
  xcb_connection_t *connection;
  xcb_window_t window;
} X11Surface;
typedef struct X11Swapchain {
  xcb_connection_t *connection;
  xcb_window_t window;
  uint32_t gc;
  uint8_t depth;
  uint32_t max_bytes;       /* the largest request, in bytes */
  uint8_t *rows;            /* a band of rows without padding, when the image's pitch has some (NULL: sent from the mapping) */
  uint64_t rows_bytes;
  bool held[8];
  bool lost;
} X11Swapchain;

static bool x11_surface(const Surface *s) { return s && (s->kind == SURFACE_XCB || s->kind == SURFACE_XLIB); }

/* The window's size and depth (a round trip); false when it is gone or the connection broke. */
static bool x11_geometry(xcb_connection_t *c, xcb_window_t window, uint32_t *width, uint32_t *height, uint8_t *depth) {
  if (g_xcb.has_error(c)) return false;
  xcb_generic_error_t *error = NULL;
  xcb_get_geometry_reply_t *g = g_xcb.get_geometry_reply(c, g_xcb.get_geometry(c, window), &error);
  if (error) g_xcb.free(error);
  if (!g) return false;
  if (width) *width = g->width;
  if (height) *height = g->height;
  if (depth) *depth = g->depth;
  g_xcb.free(g);
  return true;
}

static VkResult x11_make_surface(xcb_connection_t *c, xcb_window_t window, uint32_t kind, VkSurfaceKHR *out) {
  X11Surface *xs = vk_alloc(sizeof(X11Surface));
  if (!xs) return VK_ERROR_OUT_OF_HOST_MEMORY;
  xs->connection = c, xs->window = window;
  VkResult r = make_surface(0, kind, out);
  if (r != VK_SUCCESS) {
    vk_free(xs);
    return r;
  }
  ((Surface *)*out)->os = xs;
  return VK_SUCCESS;
}

/* ---- the surfaces ----------------------------------------------------------------------------------------------------------------------------------------------------- */
VKAPI VkResult VKAPI_CALL vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)instance, (void)alloc;
  if (!ci || !ci->connection || !ci->window || !xcb_load()) return VK_ERROR_INITIALIZATION_FAILED;
  return x11_make_surface(ci->connection, ci->window, SURFACE_XCB, out);
}
VKAPI VkResult VKAPI_CALL vkCreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)instance, (void)alloc;
  if (!ci || !ci->dpy || !ci->window || !xcb_load()) return VK_ERROR_INITIALIZATION_FAILED;
  if (!g_xcb.xlib_connection) {
    vk_os_trace("vkCreateXlibSurfaceKHR: libX11-xcb.so.1 (XGetXCBConnection) is not there\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  xcb_connection_t *c = g_xcb.xlib_connection(ci->dpy);
  if (!c) return VK_ERROR_INITIALIZATION_FAILED;
  return x11_make_surface(c, (xcb_window_t)ci->window, SURFACE_XLIB, out);
}
VKAPI VkBool32 VKAPI_CALL vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice pd, uint32_t family, xcb_connection_t *connection, xcb_visualid_t visual) {
  (void)pd, (void)visual;
  return family == 0 && connection != NULL && xcb_load();
}
VKAPI VkBool32 VKAPI_CALL vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice pd, uint32_t family, Display *display, VisualID visual) {
  (void)pd, (void)visual;
  return family == 0 && display != NULL && xcb_load() && g_xcb.xlib_connection;
}

static bool x11_surface_extent(const Surface *s, uint32_t *width, uint32_t *height) {
  const X11Surface *xs = s->os;
  return x11_geometry(xs->connection, xs->window, width, height, NULL);
}

/* ---- the swapchain ---------------------------------------------------------------------------------------------------------------------------------------------------- */
static void x11_swapchain_free(X11Swapchain *x) {
  if (x->gc && !g_xcb.has_error(x->connection)) {
    g_xcb.free_gc(x->connection, x->gc);
    g_xcb.flush(x->connection);
  }
  vk_free(x->rows);
  vk_free(x);
}

static VkResult x11_swapchain_made(Swapchain *sc) {
  const X11Surface *xs = sc->surface->os;
  if (sc->n > 8) return VK_ERROR_INITIALIZATION_FAILED;
  X11Swapchain *x = vk_alloc(sizeof(X11Swapchain));
  if (!x) return VK_ERROR_OUT_OF_HOST_MEMORY;
  memset(x, 0, sizeof *x);
  x->connection = xs->connection, x->window = xs->window;
  if (!x11_geometry(x->connection, x->window, NULL, NULL, &x->depth) || (x->depth != 24 && x->depth != 32)) {
    vk_os_trace("vkCreateSwapchainKHR: the X window is gone, or not of depth 24 or 32\n");
    x11_swapchain_free(x);
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  const uint64_t max_units = g_xcb.max_request_length(x->connection);      /* (4-byte units; it turns BIG-REQUESTS on when the server has it) */
  x->max_bytes = max_units > (1u << 28) ? 1u << 30 : (uint32_t)(max_units * 4);
  const uint64_t row = (uint64_t)sc->w * 4;
  if (x->max_bytes < X11_BIG_HEADER + row) {
    vk_os_trace("vkCreateSwapchainKHR: a row of the swapchain does not fit in one X request\n");
    x11_swapchain_free(x);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  /* the rows of one band in a buffer of their own when the images' rows are padded (X wants them without) */
  for (uint32_t i = 0; i < sc->n; ++i)
    if (sc->img[i]->pitch != row) {
      const uint64_t band = (x->max_bytes - X11_BIG_HEADER) / row;
      x->rows_bytes = MIN(band, (uint64_t)sc->h) * row;
      x->rows = vk_alloc(x->rows_bytes);
      if (!x->rows) {
        x11_swapchain_free(x);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      break;
    }
  x->gc = g_xcb.generate_id(x->connection);
  g_xcb.create_gc(x->connection, x->gc, x->window, 0, NULL);
  if (g_xcb.flush(x->connection) <= 0 || g_xcb.has_error(x->connection)) {
    x->gc = 0;
    x11_swapchain_free(x);
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  sc->os = x;
  return VK_SUCCESS;
}

static void x11_swapchain_gone(Swapchain *sc) {
  if (sc->os) x11_swapchain_free(sc->os);
  sc->os = NULL;
}

static VkResult x11_acquire(Swapchain *sc, uint64_t timeout, uint32_t *index) {
  X11Swapchain *x = sc->os;
  (void)timeout;
  if (x->lost || g_xcb.has_error(x->connection)) return VK_ERROR_SURFACE_LOST_KHR;
  for (uint32_t k = 0; k < sc->n; ++k) {
    const uint32_t i = (sc->next + k) % sc->n;
    if (!x->held[i]) {
      x->held[i] = true;
      sc->next = (i + 1) % sc->n;
      *index = i;
      return VK_SUCCESS;
    }
  }
  return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;      /* (the program holds every image: none comes back by waiting) */
}

static VkResult x11_present(Swapchain *sc, uint32_t index) {
  X11Swapchain *x = sc->os;
  Img *img = sc->img[index];
  x->held[index] = false;
  if (x->lost || g_xcb.has_error(x->connection)) {
    x->lost = true;
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  const uint8_t *pixels = kmt_lock(sc->dev, img->alloc, 0, LOCK_READ_ONLY);
  if (!pixels) return VK_ERROR_DEVICE_LOST;
  const uint64_t row = (uint64_t)sc->w * 4;
  const uint32_t band = (uint32_t)MIN((x->max_bytes - X11_BIG_HEADER) / row, (uint64_t)sc->h);
  for (uint32_t y = 0; y < sc->h; y += band) {
    const uint32_t rows = MIN(band, sc->h - y);
    const uint8_t *data = pixels + (uint64_t)y * img->pitch;
    if (x->rows) {      /* (padded rows: packed into the band buffer first) */
      for (uint32_t r = 0; r < rows; ++r) memcpy(x->rows + (uint64_t)r * row, pixels + (uint64_t)(y + r) * img->pitch, row);
      data = x->rows;
    }
    g_xcb.put_image(x->connection, XCB_IMAGE_FORMAT_Z_PIXMAP, x->window, x->gc, (uint16_t)sc->w, (uint16_t)rows, 0, (int16_t)y, 0, x->depth, (uint32_t)(row * rows), data);
  }
  kmt_unlock(sc->dev, img->alloc);
  if (sc->mode == VK_PRESENT_MODE_FIFO_KHR || sc->mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
    /* (a round trip: the server has taken the frame before the next one is drawn) */
    xcb_generic_error_t *error = NULL;
    void *reply = g_xcb.get_input_focus_reply(x->connection, g_xcb.get_input_focus(x->connection), &error);
    if (error) g_xcb.free(error);
    if (reply) g_xcb.free(reply);
    else x->lost = true;
  } else if (g_xcb.flush(x->connection) <= 0) {
    x->lost = true;
  }
  return x->lost ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
}
