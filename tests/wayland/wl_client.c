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

// A real Wayland client (libwayland-client, libxkbcommon, the xdg-shell and xdg-decoration protocols) against the kernel's compositor, served on the
// development machine by tests/wayland/WaylandHostServer.cpp: the globals, the keymap compiled by xkbcommon, a decorated toplevel drawn from a memfd pool,
// frame callbacks, a subsurface, a popup, the scripted pointer and keyboard input the host server gives it, and a maximize. Prints "<n> checks passed".
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #c); } } while (0)

static struct wl_compositor* compositor;
static struct wl_subcompositor* subcompositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct wl_output* output;
static struct wl_data_device_manager* ddm;
static struct xdg_wm_base* wm;
static struct zxdg_decoration_manager_v1* deco_manager;
static int shm_xrgb = 0, output_done = 0, output_w = 0, output_h = 0;
static struct xkb_context* xkb;
static struct xkb_keymap* keymap;
static struct xkb_state* xkb_state;
static int kb_enter = 0, pointer_enter = 0, clicked = 0, released = 0, axis = 0, frames = 0;
static char typed[16];
static int typed_n = 0;
static int configured = 0, cfg_w = -1, cfg_h = -1, cfg_max = 0, cfg_act = 0, deco_mode = 0;
static int popup_cfg = 0, popup_x = 0, popup_y = 0, popup_done = 0, popup_serial = 0;
static uint32_t xdg_serial = 0;

static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void shm_format(void* d, struct wl_shm* s, uint32_t format) { if (format == WL_SHM_FORMAT_XRGB8888) shm_xrgb = 1; }
static const struct wl_shm_listener shm_listener = {shm_format};

static void out_geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char* make, const char* model, int32_t t) {}
static void out_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) { output_w = w; output_h = h; }
static void out_done(void* d, struct wl_output* o) { output_done = 1; }
static void out_scale(void* d, struct wl_output* o, int32_t s) {}
static void out_name(void* d, struct wl_output* o, const char* n) {}
static void out_description(void* d, struct wl_output* o, const char* n) {}
static const struct wl_output_listener output_listener = {out_geometry, out_mode, out_done, out_scale, out_name, out_description};

static void wm_ping(void* d, struct xdg_wm_base* w, uint32_t serial) { xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

// --- keyboard
static void kb_keymap(void* d, struct wl_keyboard* k, uint32_t format, int fd, uint32_t size) {
  CHECK(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
  char* text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(text != MAP_FAILED);
  if (text != MAP_FAILED) {
    keymap = xkb_keymap_new_from_string(xkb, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
  }
  close(fd);
  CHECK(keymap != NULL);
  if (keymap) {
    xkb_state = xkb_state_new(keymap);
    const xkb_keysym_t* syms;
    CHECK(xkb_keymap_key_get_syms_by_level(keymap, 30 + 8, 0, 0, &syms) == 1 && syms[0] == XKB_KEY_a);
    CHECK(xkb_keymap_key_get_syms_by_level(keymap, 30 + 8, 0, 1, &syms) == 1 && syms[0] == XKB_KEY_A);
    CHECK(xkb_keymap_key_get_syms_by_level(keymap, 28 + 8, 0, 0, &syms) == 1 && syms[0] == XKB_KEY_Return);
    CHECK(xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT) == 0 && xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT) == 3);
  }
}
static void kb_enter_cb(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) { ++kb_enter; }
static void kb_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) {}
static void kb_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  if (!xkb_state || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
  const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
  char buf[8];
  if (xkb_keysym_to_utf8(sym, buf, sizeof buf) > 1 && typed_n < 15) typed[typed_n++] = buf[0];
}
static void kb_modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
  if (xkb_state) xkb_state_update_mask(xkb_state, dep, lat, lock, 0, 0, group);
}
static void kb_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { CHECK(rate > 0 && delay > 0); }
static const struct wl_keyboard_listener kb_listener = {kb_keymap, kb_enter_cb, kb_leave, kb_key, kb_modifiers, kb_repeat};

// --- pointer
static void p_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) { ++pointer_enter; }
static void p_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) {}
static void p_motion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void p_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
  if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_PRESSED) ++clicked;
  if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_RELEASED) ++released;
}
static void p_axis(void* d, struct wl_pointer* p, uint32_t time, uint32_t a, wl_fixed_t value) { if (a == 0 && value > 0) ++axis; }
static void p_frame(void* d, struct wl_pointer* p) { ++frames; }
static void p_axis_source(void* d, struct wl_pointer* p, uint32_t s) {}
static void p_axis_stop(void* d, struct wl_pointer* p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void* d, struct wl_pointer* p, uint32_t a, int32_t v) {}
static void p_axis_value120(void* d, struct wl_pointer* p, uint32_t a, int32_t v) {}
static void p_axis_rel(void* d, struct wl_pointer* p, uint32_t a, uint32_t dir) {}
static const struct wl_pointer_listener pointer_listener = {p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_axis_source, p_axis_stop, p_axis_discrete,
                                                            p_axis_value120, p_axis_rel};

static struct wl_pointer* pointer;
static struct wl_keyboard* keyboard;
static void seat_caps(void* d, struct wl_seat* s, uint32_t caps) {
  CHECK((caps & WL_SEAT_CAPABILITY_POINTER) && (caps & WL_SEAT_CAPABILITY_KEYBOARD));
  if (!pointer) { pointer = wl_seat_get_pointer(s); wl_pointer_add_listener(pointer, &pointer_listener, NULL); }
  if (!keyboard) { keyboard = wl_seat_get_keyboard(s); wl_keyboard_add_listener(keyboard, &kb_listener, NULL); }
}
static void seat_name(void* d, struct wl_seat* s, const char* name) { CHECK(strcmp(name, "seat0") == 0); }
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
  if (!strcmp(iface, wl_compositor_interface.name)) compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 5 ? version : 5);
  else if (!strcmp(iface, wl_subcompositor_interface.name)) subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
  else if (!strcmp(iface, wl_shm_interface.name)) { shm = wl_registry_bind(r, name, &wl_shm_interface, 1); wl_shm_add_listener(shm, &shm_listener, NULL); }
  else if (!strcmp(iface, wl_seat_interface.name)) { seat = wl_registry_bind(r, name, &wl_seat_interface, version < 8 ? version : 8); wl_seat_add_listener(seat, &seat_listener, NULL); }
  else if (!strcmp(iface, wl_output_interface.name)) { output = wl_registry_bind(r, name, &wl_output_interface, version < 4 ? version : 4); wl_output_add_listener(output, &output_listener, NULL); }
  else if (!strcmp(iface, wl_data_device_manager_interface.name)) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
  else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, version < 5 ? version : 5); xdg_wm_base_add_listener(wm, &wm_listener, NULL); }
  else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) deco_manager = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void global_remove(void* d, struct wl_registry* r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

// --- the shell
static void xs_configure(void* d, struct xdg_surface* s, uint32_t serial) { xdg_serial = serial; xdg_surface_ack_configure(s, serial); ++configured; }
static const struct xdg_surface_listener xs_listener = {xs_configure};
static void tl_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) {
  cfg_w = w;
  cfg_h = h;
  cfg_max = cfg_act = 0;
  uint32_t* st;
  wl_array_for_each(st, states) {
    if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) cfg_max = 1;
    if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) cfg_act = 1;
  }
}
static void tl_close(void* d, struct xdg_toplevel* t) {}
static void tl_bounds(void* d, struct xdg_toplevel* t, int32_t w, int32_t h) {}
static void tl_caps(void* d, struct xdg_toplevel* t, struct wl_array* caps) {}
static const struct xdg_toplevel_listener tl_listener = {tl_configure, tl_close, tl_bounds, tl_caps};
static void deco_configure(void* d, struct zxdg_toplevel_decoration_v1* z, uint32_t mode) { deco_mode = (int)mode; }
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {deco_configure};
static void pxs_configure(void* d, struct xdg_surface* s, uint32_t serial) { popup_serial = (int)serial; xdg_surface_ack_configure(s, serial); }
static const struct xdg_surface_listener pxs_listener = {pxs_configure};
static void pp_configure(void* d, struct xdg_popup* p, int32_t x, int32_t y, int32_t w, int32_t h) { popup_cfg = 1; popup_x = x; popup_y = y; }
static void pp_done(void* d, struct xdg_popup* p) { popup_done = 1; }
static void pp_repositioned(void* d, struct xdg_popup* p, uint32_t token) {}
static const struct xdg_popup_listener pp_listener = {pp_configure, pp_done, pp_repositioned};
static int frame_done = 0;
static void frame_cb(void* d, struct wl_callback* cb, uint32_t time) { ++frame_done; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_listener = {frame_cb};

static struct wl_buffer* make_buffer(int w, int h, uint32_t color) {
  const int stride = w * 4, size = stride * h;
  const int fd = memfd_create("wl_client", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0 || ftruncate(fd, size) != 0) return NULL;
  uint32_t* px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) return NULL;
  for (int i = 0; i < w * h; ++i) px[i] = color;
  munmap(px, size);
  struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return b;
}

// Dispatches until `cond` holds or `ms` pass.
static struct wl_display* display;
static int wait_for(int* cond, int want, int ms) {
  const uint64_t until = now_ms() + (uint64_t)ms;
  while (*cond < want && now_ms() < until) {
    wl_display_flush(display);
    struct pollfd p = {wl_display_get_fd(display), POLLIN, 0};
    if (wl_display_prepare_read(display) != 0) { wl_display_dispatch_pending(display); continue; }
    if (poll(&p, 1, 20) > 0) wl_display_read_events(display);
    else wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
  }
  return *cond >= want;
}

int main(int argc, char** argv) {
  // The same choices as arguments, for where no environment can be given (the kernel's autorun): --parallel, --screen=WxH or --screen=any
  const char* screen_choice = getenv("WL_CLIENT_SCREEN");
  int parallel_choice = getenv("WL_CLIENT_PARALLEL") != NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--parallel") == 0) parallel_choice = 1;
    else if (strncmp(argv[i], "--screen=", 9) == 0) screen_choice = argv[i] + 9;
  }
  display = wl_display_connect(NULL);
  CHECK(display != NULL);
  if (!display) return 1;
  xkb = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
  struct wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  CHECK(compositor && subcompositor && shm && seat && output && ddm && wm && deco_manager);
  wl_display_roundtrip(display);
  // WL_CLIENT_SCREEN=WxH[/work height]: the desktop's size and the height of its work area (the host server's is 1280x800 with 768 above its
  // taskbar; the kernel's emulated desktop in tests/linux_dynamic_emulated.sh is another)
  int screen_w = 1280, screen_h = 800, work_h = 768;
  const int screen_any = screen_choice && strcmp(screen_choice, "any") == 0;
  if (screen_any) screen_w = output_w, screen_h = work_h = output_h;      // (whatever the desktop is, as long as it says)
  else if (screen_choice && sscanf(screen_choice, "%dx%d/%d", &screen_w, &screen_h, &work_h) < 3) work_h = screen_h;
  CHECK(shm_xrgb && output_done && output_w > 0 && output_w == screen_w && output_h == screen_h);
  if (output_w != screen_w || output_h != screen_h) printf("the output is %dx%d\n", output_w, output_h);
  wl_display_roundtrip(display);      // (the keyboard was asked for when the seat said what it has: its keymap comes now)
  CHECK(keymap != NULL);
  // WL_CLIENT_PARALLEL: one of several at once; the host server's scripted input goes to one window only, so this one does without it
  const int parallel = parallel_choice;
  struct wl_data_device* data_device = wl_data_device_manager_get_data_device(ddm, seat);

  // a decorated toplevel
  struct wl_surface* surface = wl_compositor_create_surface(compositor);
  struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(wm, surface);
  xdg_surface_add_listener(xs, &xs_listener, NULL);
  struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(tl, &tl_listener, NULL);
  struct zxdg_toplevel_decoration_v1* deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_manager, tl);
  zxdg_toplevel_decoration_v1_add_listener(deco, &deco_listener, NULL);
  zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  xdg_toplevel_set_title(tl, parallel ? "wl_client (parallel)" : "wl_client");
  xdg_toplevel_set_app_id(tl, "tcos.wl_client");
  xdg_toplevel_set_min_size(tl, 100, 80);
  wl_surface_commit(surface);
  CHECK(wait_for(&configured, 1, 2000));
  CHECK(deco_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  CHECK(cfg_w == 0 && cfg_h == 0);
  struct wl_buffer* buffer = make_buffer(320, 240, 0xFF2266AA);
  CHECK(buffer != NULL);
  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, 320, 240);
  struct wl_callback* cb = wl_surface_frame(surface);
  wl_callback_add_listener(cb, &frame_listener, NULL);
  wl_surface_commit(surface);
  CHECK(wait_for(&frame_done, 1, 2000));      // (the host server drew it)
  // the host activated it: a configure with the activated state
  CHECK(wait_for(&configured, 2, 2000) && (cfg_act || parallel));      // (with several windows the activation moves on: the last state is any)
  // a subsurface, desynchronized
  struct wl_surface* child = wl_compositor_create_surface(compositor);
  struct wl_subsurface* sub = wl_subcompositor_get_subsurface(subcompositor, child, surface);
  wl_subsurface_set_position(sub, 10, 10);
  wl_subsurface_set_desync(sub);
  struct wl_buffer* child_buffer = make_buffer(50, 50, 0xFFFF0000);
  wl_surface_attach(child, child_buffer, 0, 0);
  wl_surface_commit(child);
  wl_surface_commit(surface);
  // a popup
  struct xdg_positioner* pos = xdg_wm_base_create_positioner(wm);
  xdg_positioner_set_size(pos, 100, 80);
  xdg_positioner_set_anchor_rect(pos, 20, 20, 10, 10);
  xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
  xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
  struct wl_surface* ps = wl_compositor_create_surface(compositor);
  struct xdg_surface* pxs = xdg_wm_base_get_xdg_surface(wm, ps);
  xdg_surface_add_listener(pxs, &pxs_listener, NULL);
  struct xdg_popup* popup = xdg_surface_get_popup(pxs, xs, pos);
  xdg_popup_add_listener(popup, &pp_listener, NULL);
  wl_surface_commit(ps);
  CHECK(wait_for(&popup_cfg, 1, 2000) && popup_x == 30 && popup_y == 30);
  struct wl_buffer* popup_buffer = make_buffer(100, 80, 0xFF00FF00);
  wl_surface_attach(ps, popup_buffer, 0, 0);
  wl_surface_commit(ps);
  if (!parallel) {
    // the scripted input: pointer in, a click (which closes the popup: it is outside it), the keys h and i, a wheel notch
    CHECK(wait_for(&pointer_enter, 1, 3000));
    CHECK(wait_for(&clicked, 1, 3000) && wait_for(&released, 1, 3000));
    CHECK(wait_for(&popup_done, 1, 2000));
    CHECK(wait_for(&kb_enter, 1, 2000));
    CHECK(wait_for(&typed_n, 2, 3000) && typed[0] == 'h' && typed[1] == 'i');
    CHECK(wait_for(&axis, 1, 3000));
    CHECK(frames > 0);
  }
  // maximized: the work area inside the frame
  const int before = configured;
  xdg_toplevel_set_maximized(tl);
  // (a desktop of unknown size: its frames and taskbar, which the client cannot know, take some of the screen; inside the screen, then)
  CHECK(wait_for(&configured, before + 1, 2000) && cfg_max &&
        (screen_any ? cfg_w > screen_w / 2 && cfg_h > screen_h / 2 && cfg_w <= screen_w && cfg_h < screen_h : cfg_w == screen_w - 8 && cfg_h == work_h - 32));
  if (screen_any) printf("maximized to %dx%d on %dx%d\n", cfg_w, cfg_h, screen_w, screen_h);
  struct wl_buffer* big = make_buffer(cfg_w, cfg_h, 0xFF444444);
  wl_surface_attach(surface, big, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, cfg_w, cfg_h);
  wl_surface_commit(surface);
  wl_display_roundtrip(display);
  // tear down
  xdg_popup_destroy(popup);
  xdg_surface_destroy(pxs);
  wl_surface_destroy(ps);
  xdg_positioner_destroy(pos);
  wl_subsurface_destroy(sub);
  wl_surface_destroy(child);
  zxdg_toplevel_decoration_v1_destroy(deco);
  xdg_toplevel_destroy(tl);
  xdg_surface_destroy(xs);
  wl_surface_destroy(surface);
  wl_buffer_destroy(buffer);
  wl_buffer_destroy(child_buffer);
  wl_buffer_destroy(popup_buffer);
  wl_buffer_destroy(big);
  wl_data_device_release(data_device);
  CHECK(wl_display_roundtrip(display) >= 0);
  CHECK(wl_display_get_error(display) == 0);
  wl_display_disconnect(display);
  printf("%d checks, %d failed\n", checks, failures);
  if (failures == 0) printf("%d checks passed\n", checks);
  return failures ? 1 : 0;
}
