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

/* A Wayland client that reads touches: glibc, libwayland-client, a fullscreen xdg-shell toplevel and the seat's wl_touch. It prints each touch event
 * ("wl-touch: down id x y", "motion", "up", "cancel") and ends when two fingers have each gone down and up (or after `seconds`, default 60), with
 * "wl-touch: N downs, M ups, K motions". For the QEMU test of touchscreens (virtio-multitouch-pci; touches sent with QMP input-send-event). */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static struct wl_touch *touch;
static int width = 640, height = 480, configured, downs, ups, motions, cancels;
static uint32_t seat_caps;

static void touch_down(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y) {
  ++downs;
  printf("wl-touch: down %d %d %d\n", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
  fflush(stdout);
}
static void touch_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
  ++ups;
  printf("wl-touch: up %d\n", id);
  fflush(stdout);
}
static void touch_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
  ++motions;
  printf("wl-touch: motion %d %d %d\n", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
  fflush(stdout);
}
static void touch_frame(void *d, struct wl_touch *t) {}
static void touch_cancel(void *d, struct wl_touch *t) { ++cancels; printf("wl-touch: cancel\n"); }
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t major, wl_fixed_t minor) {}
static void touch_orientation(void *d, struct wl_touch *t, int32_t id, wl_fixed_t orientation) {}
static const struct wl_touch_listener touch_listener = {touch_down, touch_up, touch_motion, touch_frame, touch_cancel, touch_shape, touch_orientation};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
  seat_caps = caps;
  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
    touch = wl_seat_get_touch(s);
    wl_touch_add_listener(touch, &touch_listener, NULL);
  }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {}
static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
  else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};
static void surface_configure(void *d, struct xdg_surface *s, uint32_t serial) {
  xdg_surface_ack_configure(s, serial);
  configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {surface_configure};
static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
  if (w > 0 && h > 0) width = w, height = h;
}
static void toplevel_close(void *d, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};

static struct wl_buffer *make_buffer(void) {
  const int stride = width * 4, size = stride * height;
  const int fd = memfd_create("wltouch", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) != 0) return NULL;
  uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) return NULL;
  for (int i = 0; i < width * height; ++i) px[i] = 0xFF206040u;
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return b;
}

int main(int argc, char **argv) {
  const int seconds = argc > 1 ? atoi(argv[1]) : 60;
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) { printf("wl-touch: no display\n"); return 1; }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !shm || !seat || !wm) { printf("wl-touch: globals missing\n"); return 1; }
  xdg_wm_base_add_listener(wm, &wm_listener, NULL);
  wl_seat_add_listener(seat, &seat_listener, NULL);
  wl_display_roundtrip(display);
  printf("wl-touch: seat capabilities %u%s\n", seat_caps, touch ? " (touch)" : "");
  struct wl_surface *surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surface);
  xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
  struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
  xdg_toplevel_set_title(top, "Linux touch test");
  xdg_toplevel_set_fullscreen(top, NULL);
  wl_surface_commit(surface);
  while (!configured && wl_display_dispatch(display) >= 0) {
  }
  struct wl_buffer *buffer = make_buffer();
  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_damage(surface, 0, 0, width, height);
  wl_surface_commit(surface);
  wl_display_flush(display);
  printf("wl-touch: shown %d x %d\n", width, height);
  fflush(stdout);
  const time_t end = time(NULL) + seconds;
  while (!(downs >= 2 && ups >= 2) && time(NULL) < end) {
    wl_display_flush(display);
    struct pollfd p = {wl_display_get_fd(display), POLLIN, 0};
    if (poll(&p, 1, 500) > 0 && wl_display_dispatch(display) < 0) break;
    wl_display_dispatch_pending(display);
  }
  printf("wl-touch: %d downs, %d ups, %d motions, %d cancels\n", downs, ups, motions, cancels);
  fflush(stdout);
  wl_display_disconnect(display);
  return downs >= 2 && ups >= 2 ? 0 : 1;
}
