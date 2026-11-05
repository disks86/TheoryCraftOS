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

/* Scrolling of window contents and the scroll bar query (included by window.inc after scroll.inc). win32k has no services for this, so it is done over gdi32 and the window functions: the
 * pixels of the scrolled area are copied in the window's device context, the area that was uncovered is invalidated, and with SW_SCROLLCHILDREN the child windows move. The update region
 * the window already had is not moved along with the pixels. */

#define SW_SCROLLCHILDREN_ 1
#define SW_INVALIDATE_ 2
#define SW_ERASE_ 4
#define RGN_OR_ 2
#define SWP_NOSIZE_ 1
#define SWP_NOZORDER_ 4
#define SWP_NOACTIVATE_ 0x10
IMPORT HANDLE WINAPI CreateRectRgn(int left, int top, int right, int bottom);
IMPORT int WINAPI CombineRgn(HANDLE dest, HANDLE a, HANDLE b, int mode);
IMPORT int WINAPI GetRgnBox(HANDLE region, RECT_ *box);
IMPORT BOOL WINAPI SetRectRgn(HANDLE region, int l, int t, int r, int b);
EXPORT HANDLE WINAPI GetWindow(HANDLE hwnd, UINT relation);
EXPORT BOOL WINAPI ScreenToClient(HANDLE hwnd, POINT_ *p);
EXPORT BOOL WINAPI ClientToScreen(HANDLE hwnd, POINT_ *p);
EXPORT BOOL WINAPI GetWindowRect(HANDLE hwnd, RECT_ *rect);
EXPORT BOOL WINAPI InvalidateRgn(HANDLE hwnd, HANDLE region, BOOL erase);

static BOOL rect_meet(const RECT_ *a, const RECT_ *b, RECT_ *out) {
  out->left = a->left > b->left ? a->left : b->left; out->top = a->top > b->top ? a->top : b->top;
  out->right = a->right < b->right ? a->right : b->right; out->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
  return out->left < out->right && out->top < out->bottom;
}
/* Scrolls the area `area` (clipped to `clip`) of a device context by (dx, dy). The uncovered part is put in `uncovered` as up to four rectangles; returns how many. */
static int scroll_pixels(HANDLE hdc, int dx, int dy, const RECT_ *area, const RECT_ *clip, RECT_ *uncovered) {
  RECT_ visible = *area, moved, source;
  if (clip && !rect_meet(area, clip, &visible)) return 0;
  RECT_ shifted = {area->left + dx, area->top + dy, area->right + dx, area->bottom + dy};
  if (!rect_meet(&shifted, &visible, &moved)) { uncovered[0] = visible; return 1; }
  source.left = moved.left - dx; source.top = moved.top - dy; source.right = moved.right - dx; source.bottom = moved.bottom - dy;
  if (hdc && (dx || dy)) BitBlt(hdc, moved.left, moved.top, moved.right - moved.left, moved.bottom - moved.top, hdc, source.left, source.top, SRCCOPY_);
  int n = 0;
  if (moved.top > visible.top) { RECT_ r = {visible.left, visible.top, visible.right, moved.top}; uncovered[n++] = r; }
  if (moved.bottom < visible.bottom) { RECT_ r = {visible.left, moved.bottom, visible.right, visible.bottom}; uncovered[n++] = r; }
  if (moved.left > visible.left) { RECT_ r = {visible.left, moved.top, moved.left, moved.bottom}; uncovered[n++] = r; }
  if (moved.right < visible.right) { RECT_ r = {moved.right, moved.top, visible.right, moved.bottom}; uncovered[n++] = r; }
  return n;
}
/* the rectangles as a region (the result of CombineRgn: 1 empty, 2 one rectangle, 3 several) and its bounding box */
static int scroll_region(HANDLE region, const RECT_ *rects, int n, RECT_ *box) {
  RECT_ none = {0, 0, 0, 0};
  *box = none;
  if (n == 0) { if (region) SetRectRgn(region, 0, 0, 0, 0); return 1; }
  HANDLE own = region ? NULL : CreateRectRgn(0, 0, 0, 0);
  HANDLE target = region ? region : own;
  SetRectRgn(target, rects[0].left, rects[0].top, rects[0].right, rects[0].bottom);
  int kind = 2;
  for (int i = 1; i < n; ++i) {
    HANDLE part = CreateRectRgn(rects[i].left, rects[i].top, rects[i].right, rects[i].bottom);
    kind = CombineRgn(target, target, part, RGN_OR_);
    DeleteObject(part);
  }
  GetRgnBox(target, box);
  if (own) DeleteObject(own);
  return kind;
}
EXPORT BOOL WINAPI ScrollDC(HANDLE hdc, int dx, int dy, const RECT_ *scroll, const RECT_ *clip, HANDLE update_region, RECT_ *update_rect) {
  if (!hdc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  RECT_ area = {0, 0, 0x7FFF, 0x7FFF}, uncovered[4], box;
  if (scroll) area = *scroll;
  const int n = scroll_pixels(hdc, dx, dy, &area, clip, uncovered);
  scroll_region(update_region, uncovered, n, &box);
  if (update_rect) *update_rect = box;
  return TRUE;
}
EXPORT int WINAPI ScrollWindowEx(HANDLE hwnd, int dx, int dy, const RECT_ *scroll, const RECT_ *clip, HANDLE update_region, RECT_ *update_rect, UINT flags) {
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return 0; }
  RECT_ client, uncovered[4], box;
  GetClientRect(hwnd, &client);
  RECT_ area = scroll ? *scroll : client;
  RECT_ limit = client;
  if (clip && !rect_meet(clip, &client, &limit)) limit = (RECT_){0, 0, 0, 0};
  HANDLE hdc = GetDC(hwnd);
  const int n = scroll_pixels(hdc, dx, dy, &area, &limit, uncovered);
  if (hdc) ReleaseDC(hwnd, hdc);
  const int kind = scroll_region(update_region, uncovered, n, &box);
  if (update_rect) *update_rect = box;
  if (flags & SW_SCROLLCHILDREN_) {
    for (HANDLE child = GetWindow(hwnd, 5 /* GW_CHILD */); child; child = GetWindow(child, 2 /* GW_HWNDNEXT */)) {
      RECT_ wr, hit;
      GetWindowRect(child, &wr);
      POINT_ a = {wr.left, wr.top}, b = {wr.right, wr.bottom};
      ScreenToClient(hwnd, &a); ScreenToClient(hwnd, &b);
      RECT_ cr = {a.x, a.y, b.x, b.y};
      if (scroll && !rect_meet(&cr, &area, &hit)) continue;
      SetWindowPos(child, NULL, a.x + dx, a.y + dy, 0, 0, SWP_NOSIZE_ | SWP_NOZORDER_ | SWP_NOACTIVATE_);
    }
  }
  if ((flags & SW_INVALIDATE_) && n > 0) {
    HANDLE region = CreateRectRgn(0, 0, 0, 0);
    scroll_region(region, uncovered, n, &box);
    InvalidateRgn(hwnd, region, (flags & SW_ERASE_) != 0);
    DeleteObject(region);
  }
  return kind;
}
EXPORT BOOL WINAPI ScrollWindow(HANDLE hwnd, int dx, int dy, const RECT_ *scroll, const RECT_ *clip) {
  return ScrollWindowEx(hwnd, dx, dy, scroll, clip, NULL, NULL, SW_INVALIDATE_ | SW_ERASE_ | (scroll ? 0 : SW_SCROLLCHILDREN_)) != 0;
}

/* ---- GetScrollBarInfo ----------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { DWORD size; RECT_ bar; int button, thumb_top, thumb_bottom, reserved; DWORD state[6]; } SCROLLBARINFO_;
#define STATE_SYSTEM_UNAVAILABLE_ 1
#define STATE_SYSTEM_INVISIBLE_ 0x8000
EXPORT BOOL WINAPI GetScrollBarInfo(HANDLE hwnd, LONG object, SCROLLBARINFO_ *info) {
  if (!info || info->size != sizeof *info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; }
  const DWORD style = (DWORD)GetWindowLongW(hwnd, -16);
  BOOL vertical, control = FALSE;
  int bar;
  if (object == -5 /* OBJID_VSCROLL */) { vertical = TRUE; bar = 1; }
  else if (object == -6 /* OBJID_HSCROLL */) { vertical = FALSE; bar = 0; }
  else if (object == -4 /* OBJID_CLIENT */) { control = TRUE; bar = SB_CTL_; vertical = (style & SBS_VERT_) != 0; }
  else { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (size_t_ i = 0; i < sizeof *info; ++i) if (i >= 4) ((BYTE *)info)[i] = 0;
  const int thickness = GetSystemMetrics(vertical ? 2 /* SM_CXVSCROLL */ : 3 /* SM_CYHSCROLL */);
  RECT_ rc;
  if (control) GetWindowRect(hwnd, &rc);
  else {
    RECT_ cr;
    POINT_ o = {0, 0};
    GetClientRect(hwnd, &cr);
    ClientToScreen(hwnd, &o);
    if (vertical) { rc.left = o.x + cr.right; rc.right = rc.left + thickness; rc.top = o.y; rc.bottom = o.y + cr.bottom; }
    else { rc.top = o.y + cr.bottom; rc.bottom = rc.top + thickness; rc.left = o.x; rc.right = o.x + cr.right; }
  }
  info->bar = rc;
  info->button = thickness;
  if (!control && !(style & (vertical ? 0x00200000 /* WS_VSCROLL */ : 0x00100000 /* WS_HSCROLL */))) {
    for (int i = 0; i < 6; ++i) info->state[i] = STATE_SYSTEM_INVISIBLE_;
    return TRUE;
  }
  ScrollState st;
  for (size_t_ i = 0; i < sizeof st; ++i) ((BYTE *)&st)[i] = 0;
  st.si.size = sizeof st.si; st.si.mask = SIF_ALL_;
  GetScrollInfo(hwnd, bar, &st.si);
  const int length = vertical ? rc.bottom - rc.top : rc.right - rc.left;
  int arrow, start, size;
  scroll_geometry(&st, vertical, length, thickness, &arrow, &start, &size);
  info->button = arrow;
  info->thumb_top = start;
  info->thumb_bottom = start + size;
  if (st.si.max <= st.si.min) info->state[0] |= STATE_SYSTEM_UNAVAILABLE_;
  return TRUE;
}
