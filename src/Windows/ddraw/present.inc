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
/* The primary surface on the screen. Whatever the CPU writes into the front buffer of the primary surface (Unlock of a writing lock, ReleaseDC,
 * Blt/BltFast into it, Flip, a palette change, a gamma ramp) is shown at once through GDI:
 *   - in the normal cooperative level the primary surface is the desktop, in screen coordinates: a rectangle goes to the screen, or, when the surface
 *     has a clipper with a window, into that window (clipped to its client area, which GDI clips further to what of it is visible);
 *   - in exclusive full-screen mode it fills the device window, which covers the screen.
 * The pixels go through A8R8G8B8 (dd_to_argb): an 8-bit primary through its palette, 16-bit ones converted, so a display mode of 8 or 16 bits shows on
 * the 32-bit scanout; the gamma ramp of the primary (IDirectDrawGammaControl) is applied on the way. Included by ddraw.c.
 *
 * What is drawn on the screen by others is not read back: a primary surface starts with what the screen showed when it was made, and after that
 * holds what the program drew into it. */

static void apply_gamma(const DDGAMMARAMP *ramp, BYTE *bgra, SIZE_T pixels) {
  for (SIZE_T i = 0; i < pixels; ++i, bgra += 4) {
    bgra[0] = (BYTE)(ramp->blue[bgra[0]] >> 8);
    bgra[1] = (BYTE)(ramp->green[bgra[1]] >> 8);
    bgra[2] = (BYTE)(ramp->red[bgra[2]] >> 8);
  }
}

/* Where the primary of `dd` is shown: the window (NULL: the screen) and the screen position of that device context's origin. `window_dc`: the DC is
 * the whole window's (exclusive mode: the window covers the screen, frame and all). */
static HWND present_target(const DDraw *dd, const Surface *s, POINT *origin, BOOL *window_dc) {
  origin->x = origin->y = 0;
  *window_dc = FALSE;
  if ((dd->coop & DDSCL_EXCLUSIVE) && dd->dest_window && IsWindow(dd->dest_window)) {
    RECT wr;
    if (GetWindowRect(dd->dest_window, &wr)) { origin->x = wr.left; origin->y = wr.top; }
    *window_dc = TRUE;
    return dd->dest_window;
  }
  if (s->clipper && s->clipper->window && IsWindow(s->clipper->window)) {
    ClientToScreen(s->clipper->window, origin);
    return s->clipper->window;
  }
  return NULL;
}

/* Shows `rect` (in the surface's coordinates; NULL: all of it) of the primary surface's front buffer. */
static void present_primary(Surface *s, const RECT *rect) {
  if (!s || !(s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) || !s->dd || (s->dd->flags & DD_DEAD) || !s->store.mem) return;
  DDraw *dd = s->dd;
  RECT all, r;
  rect_set(&all, 0, 0, (LONG)s->desc.dwWidth, (LONG)s->desc.dwHeight);
  if (!rect_intersect(&r, rect ? rect : &all, &all)) return;
  POINT origin;
  BOOL window_dc;
  const HWND window = present_target(dd, s, &origin, &window_dc);
  if (window && !window_dc) {
    /* (a window's primary: only what lies on its client area, in screen coordinates) */
    RECT client, on_screen;
    if (!GetClientRect(window, &client)) return;
    OffsetRect(&client, origin.x, origin.y);
    if (!rect_intersect(&on_screen, &r, &client)) return;
    r = on_screen;
  }
  cpu_access(s, &r);
  const UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
  const SIZE_T bytes = (SIZE_T)w * h * 4;
  BYTE *bits = mem_alloc(bytes);
  if (!bits) return;
  uint32_t palette[256];
  DdImage im;
  surface_image(s, &im, palette);
  if (dd_format_is_palette(s->fmt)) for (UINT i = 0; i < 256; ++i) palette[i] |= 0xFF000000u;
  const DdRect dr = {r.left, r.top, r.right, r.bottom};
  if (dd_to_argb(&im, &dr, bits, bytes, w * 4, 0, 0, 0) != DD_PIX_OK) { mem_free(bits); return; }
  if (s->gamma_set) apply_gamma(&s->gamma, bits, (SIZE_T)w * h);
  HDC dc = window_dc ? GetWindowDC(window) : GetDC(window);
  if (dc) {
    BITMAPINFO info;
    zero(&info, sizeof info);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)w;
    info.bmiHeader.biHeight = -(LONG)h;      /* (top-down) */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, r.left - origin.x, r.top - origin.y, w, h, 0, 0, 0, h, bits, &info, DIB_RGB_COLORS);
    ReleaseDC(window, dc);
  }
  mem_free(bits);
}
static void present_primary_all(Surface *primary) { present_primary(primary, NULL); }

/* The front buffer of the primary surface a surface belongs to (itself, or the front of its flipping chain), if it is one that is shown. */
static BOOL surface_is_visible_front(const Surface *s) {
  return (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
}

/* A new primary surface starts with what the screen shows (32-bit modes; a palette or 16-bit primary starts black, the program draws all of it). */
static void primary_read_screen(Surface *s) {
  if (s->fmt != DD_FMT_X8R8G8B8 && s->fmt != DD_FMT_A8R8G8B8) return;
  const UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
  HDC screen = GetDC(NULL);
  if (!screen) return;
  HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO info;
  zero(&info, sizeof info);
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = (LONG)w;
  info.bmiHeader.biHeight = -(LONG)h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void *bits = NULL;
  HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
  if (bitmap && bits) {
    HGDIOBJ old = SelectObject(memory, bitmap);
    if (BitBlt(memory, 0, 0, (int)w, (int)h, screen, 0, 0, SRCCOPY)) {
      GdiFlush();
      for (UINT y = 0; y < h; ++y) memcpy(s->store.mem + (SIZE_T)y * (SIZE_T)s->desc.lPitch, (BYTE *)bits + (SIZE_T)y * w * 4, (SIZE_T)w * 4);
    }
    SelectObject(memory, old);
  }
  if (bitmap) DeleteObject(bitmap);
  if (memory) DeleteDC(memory);
  ReleaseDC(NULL, screen);
}
