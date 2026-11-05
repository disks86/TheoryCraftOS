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

/* Scroll bars of user32.dll (included by window.inc after controls.inc): the SetScrollInfo family and the "ScrollBar" class. The bars of a window (SB_HORZ, SB_VERT) are win32k's
 * (NtUserSetScrollInfo, NtUserCallHwndParam GetScrollInfo, NtUserShowScrollBar, NtUserEnableScrollBar); the state of a scroll bar control (SB_CTL) is the control's own, so the same
 * functions send the SBM_* messages to it. The control tracks the mouse and the keys and tells its parent with WM_HSCROLL / WM_VSCROLL; it does not change its own position for the
 * arrows and page areas (the parent does), only while the thumb is dragged. */

typedef struct { UINT size, mask; int min, max; UINT page; int pos, track_pos; } ScrollInfo_;
SVC NtUserSetScrollInfo(ULONG_PTR hwnd, ULONG_PTR bar, ULONG_PTR info, ULONG_PTR redraw);
SVC NtUserShowScrollBar(ULONG_PTR hwnd, ULONG_PTR bar, ULONG_PTR show);
SVC NtUserEnableScrollBar(ULONG_PTR hwnd, ULONG_PTR bar, ULONG_PTR flags);
#define SB_CTL_ 2
#define SIF_RANGE_ 1
#define SIF_PAGE_ 2
#define SIF_POS_ 4
#define SIF_TRACKPOS_ 0x10
#define SIF_ALL_ 0x17
#define SIF_DISABLENOSCROLL_ 8
#define SBM_SETPOS_ 0xE0
#define SBM_GETPOS_ 0xE1
#define SBM_SETRANGE_ 0xE2
#define SBM_GETRANGE_ 0xE3
#define SBM_ENABLE_ARROWS_ 0xE4
#define SBM_SETRANGEREDRAW_ 0xE6
#define SBM_SETSCROLLINFO_ 0xE9
#define SBM_GETSCROLLINFO_ 0xEA
#define SBS_VERT_ 1
#define SB_LINEUP_ 0
#define SB_LINEDOWN_ 1
#define SB_PAGEUP_ 2
#define SB_PAGEDOWN_ 3
#define SB_THUMBPOSITION_ 4
#define SB_THUMBTRACK_ 5
#define SB_ENDSCROLL_ 8
#define ESB_ENABLE_BOTH_ 0
#define ESB_DISABLE_LTUP_ 1
#define ESB_DISABLE_RTDN_ 2
#define ESB_DISABLE_BOTH_ 3

EXPORT int WINAPI SetScrollInfo(HANDLE hwnd, int bar, const ScrollInfo_ *info, BOOL redraw) {
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (bar == SB_CTL_) return (int)SendMessageW(hwnd, SBM_SETSCROLLINFO_, redraw, (LONG_PTR)info);
  return (int)(LONG)NtUserSetScrollInfo((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)bar, (ULONG_PTR)info, redraw);
}
EXPORT BOOL WINAPI GetScrollInfo(HANDLE hwnd, int bar, ScrollInfo_ *info) {
  if (!info || info->size != sizeof *info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (bar == SB_CTL_) return SendMessageW(hwnd, SBM_GETSCROLLINFO_, 0, (LONG_PTR)info) != 0;
  struct { DWORD bar, pad; ScrollInfo_ *info; } params = {(DWORD)bar, 0, info};
  return BOOL_OF(NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)&params, 7 /* GetScrollInfo */));
}
EXPORT int WINAPI SetScrollPos(HANDLE hwnd, int bar, int pos, BOOL redraw) {
  ScrollInfo_ si = {sizeof si, SIF_POS_, 0, 0, 0, pos, 0}, old = {sizeof old, SIF_POS_, 0, 0, 0, 0, 0};
  if (!GetScrollInfo(hwnd, bar, &old)) return 0;
  SetScrollInfo(hwnd, bar, &si, redraw);
  return old.pos;
}
EXPORT int WINAPI GetScrollPos(HANDLE hwnd, int bar) {
  ScrollInfo_ si = {sizeof si, SIF_POS_, 0, 0, 0, 0, 0};
  return GetScrollInfo(hwnd, bar, &si) ? si.pos : 0;
}
EXPORT int WINAPI SetScrollRange(HANDLE hwnd, int bar, int min, int max, BOOL redraw) {
  ScrollInfo_ si = {sizeof si, SIF_RANGE_, min, max, 0, 0, 0};
  if (max < min || (LONG_PTR)max - min > 0x7FFFFFFF) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetScrollInfo(hwnd, bar, &si, redraw);
  return TRUE;
}
EXPORT BOOL WINAPI GetScrollRange(HANDLE hwnd, int bar, int *min, int *max) {
  ScrollInfo_ si = {sizeof si, SIF_RANGE_, 0, 0, 0, 0, 0};
  const BOOL ok = GetScrollInfo(hwnd, bar, &si);
  if (min) *min = ok ? si.min : 0;
  if (max) *max = ok ? si.max : 0;
  return ok;
}
EXPORT BOOL WINAPI ShowScrollBar(HANDLE hwnd, int bar, BOOL show) {
  if (bar == SB_CTL_) return ShowWindow(hwnd, show ? 5 /* SW_SHOW */ : 0), TRUE;
  if (bar == 3 /* SB_BOTH */) { NtUserShowScrollBar((ULONG_PTR)hwnd, 0, show); return BOOL_OF(NtUserShowScrollBar((ULONG_PTR)hwnd, 1, show)); }
  return BOOL_OF(NtUserShowScrollBar((ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)bar, show));
}
EXPORT BOOL WINAPI EnableScrollBar(HANDLE hwnd, UINT bar, UINT flags) {
  if (bar == SB_CTL_) return SendMessageW(hwnd, SBM_ENABLE_ARROWS_, flags, 0) != 0;
  return BOOL_OF(NtUserEnableScrollBar((ULONG_PTR)hwnd, bar, flags));
}

/* ---- the control ---------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { HANDLE parent; ScrollInfo_ si; UINT disabled; int tracking; int drag_offset; BOOL tracking_thumb; } ScrollState;

static int scroll_top(const ScrollInfo_ *si) { const int span = si->page ? (int)si->page - 1 : 0; const int top = si->max - span; return top < si->min ? si->min : top; }
static int scroll_clamp(const ScrollInfo_ *si, int pos) { if (pos < si->min) pos = si->min; const int top = scroll_top(si); return pos > top ? top : pos; }
/* The geometry of a bar of `length` pixels across `thickness`: the arrow size, the track and the thumb (start and length). */
static void scroll_geometry(const ScrollState *st, BOOL vertical, int length, int thickness, int *arrow, int *thumb_start, int *thumb_size) {
  int a = thickness < length / 2 ? thickness : length / 2;
  const int track = length - 2 * a;
  const int range = st->si.max - st->si.min - (st->si.page ? (int)st->si.page - 1 : 0) + 1;
  int size = st->si.page && st->si.max > st->si.min ? (int)((long long)track * st->si.page / ((long long)st->si.max - st->si.min + 1)) : a;
  if (size < 8) size = 8;
  if (size > track) size = track > 0 ? track : 0;
  *arrow = a;
  *thumb_size = size;
  *thumb_start = a + (range > 1 ? (int)((long long)(st->si.pos - st->si.min) * (track - size) / (range - 1)) : 0);
  (void)vertical;
}
static void scroll_paint(HANDLE hwnd, HANDLE hdc, ScrollState *st) {
  RECT_ r;
  GetClientRect(hwnd, &r);
  const BOOL vertical = (ctl_style(hwnd) & SBS_VERT_) != 0;
  const int length = vertical ? r.bottom : r.right, thickness = vertical ? r.right : r.bottom;
  int arrow, start, size;
  scroll_geometry(st, vertical, length, thickness, &arrow, &start, &size);
  FillRect(hdc, &r, GetSysColorBrush(0 /* COLOR_SCROLLBAR */ + 0));
  RECT_ first = vertical ? (RECT_){0, 0, r.right, arrow} : (RECT_){0, 0, arrow, r.bottom};
  RECT_ last = vertical ? (RECT_){0, r.bottom - arrow, r.right, r.bottom} : (RECT_){r.right - arrow, 0, r.right, r.bottom};
  RECT_ thumb = vertical ? (RECT_){0, start, r.right, start + size} : (RECT_){start, 0, start + size, r.bottom};
  FillRect(hdc, &first, GetSysColorBrush(COLOR_BTNFACE_));
  FrameRect(hdc, &first, GetSysColorBrush(COLOR_BTNSHADOW_));
  FillRect(hdc, &last, GetSysColorBrush(COLOR_BTNFACE_));
  FrameRect(hdc, &last, GetSysColorBrush(COLOR_BTNSHADOW_));
  if (st->si.max > st->si.min && !(ctl_style(hwnd) & WS_DISABLED_)) { FillRect(hdc, &thumb, GetSysColorBrush(COLOR_BTNFACE_)); FrameRect(hdc, &thumb, GetSysColorBrush(COLOR_BTNSHADOW_)); }
}
static void scroll_notify(HANDLE hwnd, ScrollState *st, UINT code, int pos) {
  const BOOL vertical = (ctl_style(hwnd) & SBS_VERT_) != 0;
  HANDLE parent = ctl_parent(hwnd);
  (void)st;
  if (parent) SendMessageW(parent, vertical ? 0x0115 /* WM_VSCROLL */ : 0x0114 /* WM_HSCROLL */, (ULONG_PTR)(code | ((UINT)(pos & 0xFFFF) << 16)), (LONG_PTR)hwnd);
}
/* A set of SCROLLINFO fields; answers the position. */
static int scroll_set_info(HANDLE hwnd, ScrollState *st, const ScrollInfo_ *in, BOOL redraw) {
  const int old = st->si.pos;
  if (in->mask & SIF_RANGE_) { st->si.min = in->min; st->si.max = in->max < in->min ? in->min : in->max; }
  if (in->mask & SIF_PAGE_) { st->si.page = in->page; const long long span = (long long)st->si.max - st->si.min + 1; if (st->si.page > span) st->si.page = (UINT)span; }
  if (in->mask & SIF_POS_) st->si.pos = in->pos;
  st->si.pos = scroll_clamp(&st->si, st->si.pos);
  if (in->mask & SIF_DISABLENOSCROLL_) st->disabled = (st->si.max - st->si.min < (int)st->si.page || st->si.max == st->si.min) ? ESB_DISABLE_BOTH_ : 0;
  if (redraw || st->si.pos != old) ctl_repaint(hwnd);
  return st->si.pos;
}

static LONG_PTR WINAPI scrollbar_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  ScrollState *st = message == 0x0081 ? NULL : ctl_state(hwnd);
  const LONG style = ctl_style(hwnd);
  if (!st && message != 0x0081 && message != 0x0082) return DefWindowProcW(hwnd, message, wparam, lparam);
  switch (message) {
    case 0x0081:
      if (!(st = ctl_alloc(hwnd, sizeof(ScrollState)))) return FALSE;
      st->parent = (HANDLE)((CreateStruct_ *)lparam)->parent;
      st->si.size = sizeof st->si;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0082: ctl_free(hwnd); return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0014: return 1;
    case 0x000F: case 0x0318: {
      PAINTSTRUCT_ ps;
      HANDLE hdc = message == 0x0318 ? (HANDLE)wparam : BeginPaint(hwnd, &ps);
      if (hdc) { scroll_paint(hwnd, hdc, st); if (message == 0x000F) EndPaint(hwnd, &ps); }
      return 0;
    }
    case 0x000A: ctl_repaint(hwnd); return 0;
    case 0x0087: return DLGC_WANTARROWS_;
    case SBM_SETPOS_: { const int old = st->si.pos; ScrollInfo_ in = {sizeof in, SIF_POS_, 0, 0, 0, (int)wparam, 0}; scroll_set_info(hwnd, st, &in, (BOOL)lparam); return old; }
    case SBM_GETPOS_: return st->si.pos;
    case SBM_SETRANGE_: case SBM_SETRANGEREDRAW_: {
      ScrollInfo_ in = {sizeof in, SIF_RANGE_, (int)wparam, (int)lparam, 0, 0, 0};
      scroll_set_info(hwnd, st, &in, message == SBM_SETRANGEREDRAW_);
      return 0;
    }
    case SBM_GETRANGE_: if (wparam) *(int *)wparam = st->si.min; if (lparam) *(int *)lparam = st->si.max; return 0;
    case SBM_ENABLE_ARROWS_: {
      if (wparam > ESB_DISABLE_BOTH_) return FALSE;
      const BOOL same = st->disabled == wparam;
      st->disabled = (UINT)wparam;
      ctl_repaint(hwnd);
      return same ? FALSE : TRUE;
    }
    case SBM_SETSCROLLINFO_: {
      const ScrollInfo_ *in = (const ScrollInfo_ *)lparam;
      if (!in || in->size != sizeof *in || (in->mask & ~(UINT)(SIF_ALL_ | SIF_DISABLENOSCROLL_))) return 0;
      return scroll_set_info(hwnd, st, in, (BOOL)wparam);
    }
    case SBM_GETSCROLLINFO_: {
      ScrollInfo_ *out = (ScrollInfo_ *)lparam;
      if (!out || out->size != sizeof *out) return FALSE;
      if (out->mask & SIF_RANGE_) { out->min = st->si.min; out->max = st->si.max; }
      if (out->mask & SIF_PAGE_) out->page = st->si.page;
      if (out->mask & SIF_POS_) out->pos = st->si.pos;
      if (out->mask & SIF_TRACKPOS_) out->track_pos = st->tracking_thumb ? st->tracking : st->si.pos;
      return TRUE;
    }
    case 0x0201: {      /* WM_LBUTTONDOWN: arrows, page areas and the thumb */
      RECT_ r;
      GetClientRect(hwnd, &r);
      if (style & WS_DISABLED_) return 0;
      const BOOL vertical = (style & SBS_VERT_) != 0;
      const int length = vertical ? r.bottom : r.right, thickness = vertical ? r.right : r.bottom;
      const int at = vertical ? (short)((lparam >> 16) & 0xFFFF) : (short)(lparam & 0xFFFF);
      int arrow, start, size;
      scroll_geometry(st, vertical, length, thickness, &arrow, &start, &size);
      SetCapture(hwnd);
      if (GetFocus() != hwnd && !(style & 0x4 /* SBS_NOFOCUS? no: keep focus */)) { /* (scroll bars take no focus on click) */ }
      if (at < arrow) { if (!(st->disabled & ESB_DISABLE_LTUP_)) scroll_notify(hwnd, st, SB_LINEUP_, 0); }
      else if (at >= length - arrow) { if (!(st->disabled & ESB_DISABLE_RTDN_)) scroll_notify(hwnd, st, SB_LINEDOWN_, 0); }
      else if (at < start) scroll_notify(hwnd, st, SB_PAGEUP_, 0);
      else if (at >= start + size) scroll_notify(hwnd, st, SB_PAGEDOWN_, 0);
      else { st->tracking_thumb = TRUE; st->drag_offset = at - start; st->tracking = st->si.pos; }
      return 0;
    }
    case 0x0200: {      /* WM_MOUSEMOVE while the thumb is held */
      if (!st->tracking_thumb) return 0;
      RECT_ r;
      GetClientRect(hwnd, &r);
      const BOOL vertical = (style & SBS_VERT_) != 0;
      const int length = vertical ? r.bottom : r.right, thickness = vertical ? r.right : r.bottom;
      const int at = vertical ? (short)((lparam >> 16) & 0xFFFF) : (short)(lparam & 0xFFFF);
      int arrow, start, size;
      scroll_geometry(st, vertical, length, thickness, &arrow, &start, &size);
      const int track = length - 2 * arrow - size, range = scroll_top(&st->si) - st->si.min;
      int pos = st->si.min;
      if (track > 0 && range > 0) { int rel = at - st->drag_offset - arrow; if (rel < 0) rel = 0; if (rel > track) rel = track; pos = st->si.min + (int)((long long)rel * range / track); }
      st->tracking = pos;
      st->si.pos = pos;
      ctl_repaint(hwnd);
      scroll_notify(hwnd, st, SB_THUMBTRACK_, pos);
      return 0;
    }
    case 0x0202: {      /* WM_LBUTTONUP */
      if (GetCapture() != hwnd) return 0;
      ReleaseCapture();
      if (st->tracking_thumb) { st->tracking_thumb = FALSE; scroll_notify(hwnd, st, SB_THUMBPOSITION_, st->si.pos); }
      scroll_notify(hwnd, st, SB_ENDSCROLL_, 0);
      return 0;
    }
    case 0x0215: st->tracking_thumb = FALSE; return 0;      /* WM_CAPTURECHANGED */
    case 0x0100: {      /* WM_KEYDOWN: the keys scroll by lines, pages and to the ends */
      const BOOL vertical = (style & SBS_VERT_) != 0;
      UINT code = 0xFFFF;
      switch (wparam) {
        case 0x26: if (vertical) code = SB_LINEUP_; break;
        case 0x28: if (vertical) code = SB_LINEDOWN_; break;
        case 0x25: if (!vertical) code = SB_LINEUP_; break;
        case 0x27: if (!vertical) code = SB_LINEDOWN_; break;
        case 0x21: code = SB_PAGEUP_; break;
        case 0x22: code = SB_PAGEDOWN_; break;
      }
      if (code != 0xFFFF) { scroll_notify(hwnd, st, code, 0); return 0; }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case 0x0101: scroll_notify(hwnd, st, SB_ENDSCROLL_, 0); return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
