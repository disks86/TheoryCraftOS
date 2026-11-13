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

/* XInput 2.2 touches through real libXi against the kernel's X server (tests/x11_interop.sh, with tests/x11/X11HostServer.cpp run as TCOS_XI2=1, which
 * plays the desktop's touchscreen: one finger down, moved and lifted on the window named "xi2-target"). Prints each check and a summary; exit status 0
 * when all passed. */

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
  Display* d = XOpenDisplay(NULL);
  CHECK(d != NULL);
  if (!d) return 1;
  int opcode = 0, event = 0, error = 0;
  CHECK(XQueryExtension(d, "XInputExtension", &opcode, &event, &error));
  int major = 2, minor = 2;
  CHECK(XIQueryVersion(d, &major, &minor) == Success && major == 2 && minor == 2);
  /* the devices: a touchscreen with a direct touch class */
  int n = 0, touchscreen = 0, touches = 0;
  XIDeviceInfo* devices = XIQueryDevice(d, XIAllDevices, &n);
  CHECK(devices != NULL && n == 3);
  for (int i = 0; devices && i < n; ++i) {
    printf("device %d \"%s\" use %d classes %d\n", devices[i].deviceid, devices[i].name, devices[i].use, devices[i].num_classes);
    for (int k = 0; k < devices[i].num_classes; ++k)
      if (devices[i].classes[k]->type == XITouchClass) {
        XITouchClassInfo* t = (XITouchClassInfo*)devices[i].classes[k];
        if (t->mode == XIDirectTouch) touchscreen = devices[i].deviceid, touches = t->num_touches;
      }
  }
  if (devices) XIFreeDeviceInfo(devices);
  CHECK(touchscreen == 4 && touches == 10);
  /* a window that takes touches */
  Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 100, 100, 300, 200, 0, 0, 0xFFFFFF);
  XStoreName(d, w, "xi2-target");
  unsigned char bits[XIMaskLen(XI_LASTEVENT)];
  memset(bits, 0, sizeof bits);
  XISetMask(bits, XI_TouchBegin);
  XISetMask(bits, XI_TouchUpdate);
  XISetMask(bits, XI_TouchEnd);
  XIEventMask mask = {XIAllMasterDevices, sizeof bits, bits};
  CHECK(XISelectEvents(d, w, &mask, 1) == Success);
  int got = 0;
  XIEventMask* selected = XIGetSelectedEvents(d, w, &got);
  CHECK(selected && got == 1 && XIMaskIsSet(selected[0].mask, XI_TouchBegin));
  if (selected) XFree(selected);
  XMapWindow(d, w);
  XFlush(d);
  /* the host server's finger: begin, update(s), end, one touch id, in the window's coordinates */
  int begins = 0, updates = 0, ends = 0, same_id = 1;
  unsigned int id = 0;
  double first_x = -1, first_y = -1;
  for (int i = 0; i < 1000 && ends == 0; ++i) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type != GenericEvent || e.xcookie.extension != opcode || !XGetEventData(d, &e.xcookie)) continue;
      XIDeviceEvent* t = (XIDeviceEvent*)e.xcookie.data;
      if (t->evtype == XI_TouchBegin) { ++begins; id = (unsigned)t->detail; first_x = t->event_x; first_y = t->event_y; CHECK(t->event == w && t->sourceid == 4); }
      else if (t->evtype == XI_TouchUpdate) { ++updates; same_id &= (unsigned)t->detail == id; }
      else if (t->evtype == XI_TouchEnd) { ++ends; same_id &= (unsigned)t->detail == id; }
      XFreeEventData(d, &e.xcookie);
    }
    usleep(5000);
  }
  CHECK(begins == 1 && updates >= 1 && ends == 1 && same_id);
  CHECK(first_x == 30.0 && first_y == 40.0);
  printf("touch %u: begin at %.0f,%.0f, %d updates, end\n", id, first_x, first_y, updates);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  printf("xi2_client: %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
