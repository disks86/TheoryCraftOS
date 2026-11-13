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

/* XDND through real libX11 against the kernel's X server (tests/x11_interop.sh, with tests/x11/X11HostServer.cpp run as TCOS_XDND=1, which plays the
 * desktop): first a drop target ("xdnd-target" with XdndAware) that the desktop drags its file onto, then a drag source that lets a file go over
 * the desktop (the root has XdndAware). Prints each check and a summary; exit status 0 when all passed. */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

static Atom A(Display* d, const char* name) { return XInternAtom(d, name, False); }

static void message(Display* d, Window to, Window window, Atom type, long l0, long l1, long l2, long l3, long l4) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = window;
  e.xclient.message_type = type;
  e.xclient.format = 32;
  e.xclient.data.l[0] = l0;
  e.xclient.data.l[1] = l1;
  e.xclient.data.l[2] = l2;
  e.xclient.data.l[3] = l3;
  e.xclient.data.l[4] = l4;
  XSendEvent(d, to, False, NoEventMask, &e);
  XFlush(d);
}

int main(void) {
  Display* d = XOpenDisplay(NULL);
  CHECK(d != NULL);
  if (!d) return 1;
  const Window root = DefaultRootWindow(d);
  const Atom aware = A(d, "XdndAware"), enter = A(d, "XdndEnter"), position = A(d, "XdndPosition"), status = A(d, "XdndStatus");
  const Atom drop = A(d, "XdndDrop"), finished = A(d, "XdndFinished"), selection = A(d, "XdndSelection"), copy = A(d, "XdndActionCopy");
  const Atom type_list = A(d, "XdndTypeList"), uri_list = A(d, "text/uri-list"), prop = A(d, "XDND_TEST_DATA");

  /* the root takes drops (the desktop) */
  {
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char* data = NULL;
    CHECK(XGetWindowProperty(d, root, aware, 0, 1, False, XA_ATOM, &type, &format, &n, &after, &data) == Success && n == 1 && format == 32 &&
          data && *(unsigned long*)data >= 3);
    if (data) XFree(data);
  }

  /* 1. a drop target: the desktop drags its file onto it */
  Window w = XCreateSimpleWindow(d, root, 100, 100, 300, 200, 0, 0, 0xFFFFFF);
  XStoreName(d, w, "xdnd-target");
  const long version = 5;
  XChangeProperty(d, w, aware, XA_ATOM, 32, PropModeReplace, (const unsigned char*)&version, 1);
  XSelectInput(d, w, PropertyChangeMask);
  XMapWindow(d, w);
  XFlush(d);
  int entered = 0, positions = 0, dropped = 0, got_data = 0, uri_offered = 0;
  Window source = None;
  char text[512] = "";
  for (int i = 0; i < 1000 && !got_data; ++i) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type == ClientMessage && e.xclient.message_type == enter) {
        entered = 1;
        source = (Window)e.xclient.data.l[0];
        if (e.xclient.data.l[1] & 1) {      /* more than three types: XdndTypeList */
          Atom type;
          int format;
          unsigned long n, after;
          unsigned char* data = NULL;
          if (XGetWindowProperty(d, source, type_list, 0, 64, False, XA_ATOM, &type, &format, &n, &after, &data) == Success && data) {
            for (unsigned long k = 0; k < n; ++k) uri_offered |= ((unsigned long*)data)[k] == uri_list;
            XFree(data);
          }
        } else {
          for (int k = 2; k < 5; ++k) uri_offered |= (Atom)e.xclient.data.l[k] == uri_list;
        }
      } else if (e.type == ClientMessage && e.xclient.message_type == position) {
        ++positions;
        message(d, source, source, status, (long)w, 1, 0, 0, (long)copy);
      } else if (e.type == ClientMessage && e.xclient.message_type == drop) {
        dropped = 1;
        XConvertSelection(d, selection, uri_list, prop, w, (Time)e.xclient.data.l[2]);
        XFlush(d);
      } else if (e.type == SelectionNotify && e.xselection.property == prop) {
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char* data = NULL;
        if (XGetWindowProperty(d, w, prop, 0, 1024, True, AnyPropertyType, &type, &format, &n, &after, &data) == Success && data) {
          snprintf(text, sizeof text, "%.*s", (int)n, (const char*)data);
          XFree(data);
        }
        got_data = 1;
        message(d, source, source, finished, (long)w, 1, (long)copy, 0, 0);
      }
    }
    usleep(5000);
  }
  CHECK(entered && uri_offered && positions > 0 && dropped && got_data);
  CHECK(strcmp(text, "file:///mnt/desktop/Documents/a%20b.txt\r\n") == 0);
  printf("dropped on the target: %s", text);

  /* 2. a drag source: its file let go over the desktop (sent to the root, the top-level under the pointer there) */
  Window s = XCreateSimpleWindow(d, root, 500, 100, 100, 100, 0, 0, 0xFFFFFF);
  XMapWindow(d, s);
  XSetSelectionOwner(d, selection, s, CurrentTime);
  CHECK(XGetSelectionOwner(d, selection) == s);
  message(d, root, root, enter, (long)s, 5L << 24, (long)uri_list, 0, 0);
  message(d, root, root, position, (long)s, 0, (700L << 16) | 450L, CurrentTime, (long)copy);
  const char* data = "file:///mnt/desktop/Documents/b.txt\r\n";
  int served = 0, accepted = 0, done = 0, taken = 0;
  for (int i = 0; i < 1000 && !done; ++i) {
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      if (e.type == SelectionRequest) {
        XSelectionRequestEvent* r = &e.xselectionrequest;
        XChangeProperty(d, r->requestor, r->property, r->target, 8, PropModeReplace, (const unsigned char*)data, (int)strlen(data));
        XEvent n;
        memset(&n, 0, sizeof n);
        n.xselection.type = SelectionNotify;
        n.xselection.requestor = r->requestor;
        n.xselection.selection = r->selection;
        n.xselection.target = r->target;
        n.xselection.property = r->property;
        n.xselection.time = r->time;
        XSendEvent(d, r->requestor, False, NoEventMask, &n);
        XFlush(d);
        served = 1;
      } else if (e.type == ClientMessage && e.xclient.message_type == status) {
        if ((e.xclient.data.l[1] & 1) && !accepted) {
          accepted = 1;
          message(d, root, root, drop, (long)s, 0, CurrentTime, 0, 0);
        }
      } else if (e.type == ClientMessage && e.xclient.message_type == finished) {
        done = 1;
        taken = (e.xclient.data.l[1] & 1) != 0;
      }
    }
    if (!accepted && i % 20 == 19) message(d, root, root, position, (long)s, 0, (700L << 16) | 450L, CurrentTime, (long)copy);
    usleep(5000);
  }
  CHECK(served && accepted && done && taken);
  XDestroyWindow(d, s);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  printf("xdnd_client: %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
