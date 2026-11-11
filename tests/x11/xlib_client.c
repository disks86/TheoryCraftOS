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

/* A real Xlib (libX11 over libxcb) client against the kernel's X server: what ordinary X programs do at start and while they run. Prints each failed
 * check and a summary; exit status 0 when all passed. Built and run by tests/x11_interop.sh against tests/x11/X11HostServer.cpp, and meant to be run
 * as a Linux program inside TheoryCraftOS too (DISPLAY=:0). */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0, checks = 0, x_errors = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

static int on_error(Display* d, XErrorEvent* e) {
  char text[128];
  XGetErrorText(d, e->error_code, text, sizeof text);
  printf("X error: %s (request %d.%d, resource 0x%lx)\n", text, e->request_code, e->minor_code, e->resourceid);
  ++x_errors;
  return 0;
}

/* Waits (up to two seconds) for an event of this type on the window. */
static int wait_for(Display* d, Window w, int type, XEvent* out) {
  for (int i = 0; i < 400; ++i) {
    while (XPending(d)) {
      XNextEvent(d, out);
      if (out->type == type && (w == None || out->xany.window == w)) return 1;
    }
    usleep(5000);
  }
  return 0;
}

/* What the window shows (drawn at the start and again whenever its picture was lost: the server keeps no backing store). */
static void draw_scene(Display* d, Window w, GC gc) {
  XSetForeground(d, gc, 0xFF0000);
  XFillRectangle(d, w, gc, 10, 10, 50, 40);
  XSetForeground(d, gc, 0x0000FF);
  XDrawLine(d, w, gc, 0, 100, 319, 100);
  XDrawRectangle(d, w, gc, 70, 10, 30, 30);
  XDrawArc(d, w, gc, 120, 10, 40, 40, 0, 360 * 64);
  XFillArc(d, w, gc, 170, 10, 40, 40, 0, 270 * 64);
  XPoint triangle[3] = {{220, 50}, {260, 10}, {300, 50}};
  XFillPolygon(d, w, gc, triangle, 3, Convex, CoordModeOrigin);
  XSetForeground(d, gc, 0x000000);
  XDrawString(d, w, gc, 10, 80, "Hello, X11", 10);
  XDrawImageString(d, w, gc, 10, 130, "Image text", 10);
}

int main(void) {
  XSetErrorHandler(on_error);
  Display* d = XOpenDisplay(NULL);
  CHECK(d != NULL);
  if (!d) return 1;
  const int screen = DefaultScreen(d);
  const Window root = RootWindow(d, screen);
  CHECK(ScreenCount(d) == 1);
  CHECK(DefaultDepth(d, screen) == 24);
  CHECK(DisplayWidth(d, screen) == 1280 && DisplayHeight(d, screen) == 800);
  CHECK(strcmp(ServerVendor(d), "TheoryCraftOS") == 0);
  XVisualInfo vinfo;
  CHECK(XMatchVisualInfo(d, screen, 32, TrueColor, &vinfo) != 0);

  /* the window manager is there (EWMH) */
  Atom net_check = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
  Atom type;
  int format;
  unsigned long count, after;
  unsigned char* data = NULL;
  CHECK(XGetWindowProperty(d, root, net_check, 0, 1, False, XA_WINDOW, &type, &format, &count, &after, &data) == Success && count == 1);
  if (data) XFree(data);

  /* a top-level window, as toolkits make them */
  const unsigned long white = WhitePixel(d, screen), black = BlackPixel(d, screen);
  Window w = XCreateSimpleWindow(d, root, 100, 80, 320, 240, 1, black, white);
  XStoreName(d, w, "Xlib test");
  Atom net_name = XInternAtom(d, "_NET_WM_NAME", False), utf8 = XInternAtom(d, "UTF8_STRING", False);
  const char* title = "Xlib test \xE2\x9C\x93";
  XChangeProperty(d, w, net_name, utf8, 8, PropModeReplace, (const unsigned char*)title, (int)strlen(title));
  Atom wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(d, w, &wm_delete, 1);
  XSizeHints* hints = XAllocSizeHints();
  hints->flags = PMinSize;
  hints->min_width = 100;
  hints->min_height = 80;
  XSetWMNormalHints(d, w, hints);
  XFree(hints);
  XClassHint* klass = XAllocClassHint();
  klass->res_name = (char*)"xlibtest";
  klass->res_class = (char*)"XlibTest";
  XSetClassHint(d, w, klass);
  XFree(klass);
  XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask);
  XMapWindow(d, w);
  XEvent e;
  CHECK(wait_for(d, w, MapNotify, &e));
  CHECK(wait_for(d, w, Expose, &e));

  /* where it is: the frame went where it was asked, the inside is below the title bar */
  int x = 0, y = 0;
  Window child;
  XTranslateCoordinates(d, w, root, 0, 0, &x, &y, &child);
  CHECK(x == 106 && y == 105);
  Window groot;
  unsigned int gw, gh, gb, gd;
  CHECK(XGetGeometry(d, w, &groot, &x, &y, &gw, &gh, &gb, &gd) && gw == 320 && gh == 240 && gd == 24 && groot == root);
  Atom extents = XInternAtom(d, "_NET_FRAME_EXTENTS", False);
  data = NULL;
  CHECK(XGetWindowProperty(d, w, extents, 0, 4, False, XA_CARDINAL, &type, &format, &count, &after, &data) == Success && count == 4);
  if (data && count == 4) {
    const long* v = (const long*)data;      /* (Xlib hands format-32 data as longs) */
    CHECK(v[0] == 6 && v[1] == 6 && v[2] == 25 && v[3] == 6);
  }
  if (data) XFree(data);

  /* drawing */
  GC gc = XCreateGC(d, w, 0, NULL);
  XFontStruct* font = XLoadQueryFont(d, "fixed");
  CHECK(font != NULL);
  if (font) {
    CHECK(font->ascent == 12 && font->descent == 4 && font->max_bounds.width == 8);
    XSetFont(d, gc, font->fid);
    CHECK(XTextWidth(font, "Hello", 5) == 40);
  }
  draw_scene(d, w, gc);
  XSync(d, False);
  XImage* img = XGetImage(d, w, 0, 0, 320, 240, AllPlanes, ZPixmap);
  CHECK(img != NULL);
  if (img) {
    CHECK((XGetPixel(img, 20, 20) & 0xFFFFFF) == 0xFF0000);
    CHECK((XGetPixel(img, 5, 100) & 0xFFFFFF) == 0x0000FF);
    CHECK((XGetPixel(img, 70, 20) & 0xFFFFFF) == 0x0000FF);
    CHECK((XGetPixel(img, 180, 20) & 0xFFFFFF) == 0x0000FF);      /* (the upper left quarter of the 270-degree pie) */
    CHECK((XGetPixel(img, 300, 200) & 0xFFFFFF) == 0xFFFFFF);
    int ink = 0;
    for (int yy = 68; yy < 84; ++yy) for (int xx = 10; xx < 90; ++xx) ink += (XGetPixel(img, xx, yy) & 0xFFFFFF) == 0;
    CHECK(ink > 40);
    XDestroyImage(img);
  }
  /* PutImage of a client-side image */
  XImage* put = XCreateImage(d, DefaultVisual(d, screen), 24, ZPixmap, 0, malloc(16 * 16 * 4), 16, 16, 32, 0);
  for (int yy = 0; yy < 16; ++yy) for (int xx = 0; xx < 16; ++xx) XPutPixel(put, xx, yy, 0x00FF00);
  XPutImage(d, w, gc, put, 0, 0, 200, 150, 16, 16);
  XDestroyImage(put);
  /* a pixmap copied into the window */
  Pixmap pm = XCreatePixmap(d, w, 20, 20, 24);
  GC pgc = XCreateGC(d, pm, 0, NULL);
  XSetForeground(d, pgc, 0x123456);
  XFillRectangle(d, pm, pgc, 0, 0, 20, 20);
  XCopyArea(d, pm, w, gc, 0, 0, 20, 20, 250, 150);
  XSync(d, False);
  img = XGetImage(d, w, 200, 150, 80, 20, AllPlanes, ZPixmap);
  if (img) {
    CHECK((XGetPixel(img, 5, 5) & 0xFFFFFF) == 0x00FF00);
    CHECK((XGetPixel(img, 55, 5) & 0xFFFFFF) == 0x123456);
    XDestroyImage(img);
  }
  /* a bitmap from data, as icons and stipples are made */
  static const unsigned char bits[] = {0x0F, 0xF0};
  Pixmap bm = XCreateBitmapFromData(d, w, (const char*)bits, 16, 1);
  CHECK(bm != None);

  /* colours */
  XColor exact, screen_color;
  CHECK(XAllocNamedColor(d, DefaultColormap(d, screen), "SkyBlue", &screen_color, &exact) && screen_color.pixel == 0x87CEEB);
  XColor c = {.red = 0x8000, .green = 0x4000, .blue = 0xFFFF};
  CHECK(XAllocColor(d, DefaultColormap(d, screen), &c) && c.pixel == 0x8040FF);
  CHECK(XParseColor(d, DefaultColormap(d, screen), "#102030", &exact) && exact.red == 0x1000 && exact.blue == 0x3000);
  Colormap argb_map = XCreateColormap(d, root, vinfo.visual, AllocNone);
  CHECK(argb_map != None);

  /* fonts */
  int n = 0;
  char** names = XListFonts(d, "*fixed*", 10, &n);
  CHECK(names != NULL && n >= 1);
  if (names) XFreeFontNames(names);

  /* cursors */
  Cursor cur = XCreateFontCursor(d, XC_xterm);
  XDefineCursor(d, w, cur);

  /* keyboard */
  CHECK(XKeysymToKeycode(d, XK_a) == 12);
  CHECK(XKeysymToKeycode(d, XK_Return) == 0x28 + 8);
  int min_kc, max_kc;
  XDisplayKeycodes(d, &min_kc, &max_kc);
  CHECK(min_kc == 8 && max_kc == 255);
  XModifierKeymap* mods = XGetModifierMapping(d);
  CHECK(mods && mods->max_keypermod == 2 && mods->modifiermap[0] == 0xE1 + 8);
  if (mods) XFreeModifiermap(mods);

  /* properties, the window manager's ones too */
  XTextProperty tp;
  CHECK(XGetWMName(d, w, &tp) && tp.value && strcmp((const char*)tp.value, "Xlib test") == 0);
  Atom wm_state = XInternAtom(d, "WM_STATE", False);
  data = NULL;
  CHECK(XGetWindowProperty(d, w, wm_state, 0, 2, False, wm_state, &type, &format, &count, &after, &data) == Success && count == 2 && data &&
        ((const long*)data)[0] == NormalState);
  if (data) XFree(data);

  /* selections */
  Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
  XSetSelectionOwner(d, clipboard, w, CurrentTime);
  CHECK(XGetSelectionOwner(d, clipboard) == w);

  /* a client message to ourselves */
  XEvent msg;
  memset(&msg, 0, sizeof msg);
  msg.xclient.type = ClientMessage;
  msg.xclient.window = w;
  msg.xclient.message_type = XInternAtom(d, "TEST_MESSAGE", False);
  msg.xclient.format = 32;
  msg.xclient.data.l[0] = 0x12345678;
  XSendEvent(d, w, False, NoEventMask, &msg);
  CHECK(wait_for(d, w, ClientMessage, &e) && e.xclient.data.l[0] == 0x12345678 && e.xclient.send_event);

  /* the window manager answers EWMH requests: maximize it */
  XEvent max;
  memset(&max, 0, sizeof max);
  max.xclient.type = ClientMessage;
  max.xclient.window = w;
  max.xclient.message_type = XInternAtom(d, "_NET_WM_STATE", False);
  max.xclient.format = 32;
  max.xclient.data.l[0] = 1;
  max.xclient.data.l[1] = (long)XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_VERT", False);
  max.xclient.data.l[2] = (long)XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
  XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &max);
  CHECK(wait_for(d, w, ConfigureNotify, &e) && e.xconfigure.width == 1280 - 12 && e.xconfigure.height == 768 - 31);
  max.xclient.data.l[0] = 0;
  XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &max);
  CHECK(wait_for(d, w, ConfigureNotify, &e) && e.xconfigure.width == 320 && e.xconfigure.height == 240);

  /* subwindows and stacking */
  Window sub = XCreateSimpleWindow(d, w, 20, 160, 60, 40, 2, black, 0x00FFFF);
  XMapRaised(d, sub);
  XSync(d, False);
  img = XGetImage(d, w, 20, 160, 64, 44, AllPlanes, ZPixmap);
  if (img) {
    CHECK((XGetPixel(img, 0, 0) & 0xFFFFFF) == 0x000000);        /* the border */
    CHECK((XGetPixel(img, 10, 10) & 0xFFFFFF) == 0x00FFFF);      /* the inside */
    XDestroyImage(img);
  }
  Window r, parent, *children = NULL;
  unsigned int nchildren = 0;
  CHECK(XQueryTree(d, w, &r, &parent, &children, &nchildren) && parent == root && nchildren == 1 && children[0] == sub);
  if (children) XFree(children);
  XDestroyWindow(d, sub);

  /* the pointer and the focus */
  int rx, ry, wx, wy;
  unsigned int mask;
  CHECK(XQueryPointer(d, w, &r, &child, &rx, &ry, &wx, &wy, &mask));
  Window focus;
  int revert;
  XGetInputFocus(d, &focus, &revert);
  CHECK(getenv("XLIB_CLIENT_SHARED") ? focus != None : focus == w);      /* (with other clients about, the last window mapped has it) */

  XFreeGC(d, pgc);
  XFreePixmap(d, pm);
  XFreePixmap(d, bm);
  if (font) XFreeFont(d, font);
  XFreeCursor(d, cur);
  XSync(d, False);
  CHECK(x_errors == 0);
  /* the maximize test resized it (ForgetGravity): drawn again; then kept on screen for the server's picture dump */
  draw_scene(d, w, gc);
  XSync(d, False);
  if (getenv("XLIB_CLIENT_LINGER")) sleep((unsigned)atoi(getenv("XLIB_CLIENT_LINGER")));
  XFreeGC(d, gc);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  printf("xlib_client: %d checks, %d failed, %d X errors\n", checks, failures, x_errors);
  return failures == 0 ? 0 : 1;
}
