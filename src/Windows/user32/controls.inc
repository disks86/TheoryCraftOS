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

/* The built-in controls of user32.dll: the window procedures of the "Static", "Button" and "Edit" classes (included by window.inc; the classes are registered by builtin_classes).
 * Each control keeps its state in a block of the process heap that the first window long points to (the classes have 8 bytes of window extra); the block is made by WM_NCCREATE and freed
 * by WM_NCDESTROY. The messages, notifications (WM_COMMAND to the parent), colour messages (WM_CTLCOLOR*) and the order in which they are sent follow Windows (Wine's tests). Drawing is
 * plain: backgrounds, text, frames, check boxes and push button edges over gdi32. */

/* ---- gdi32 --------------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { LONG cx, cy; } SIZE_;
typedef struct { HANDLE hdc; BOOL erase; RECT_ rc; BOOL restore, inc; BYTE reserved[32]; } PAINTSTRUCT_;
#define WS_DISABLED_ 0x08000000
#define HTTRANSPARENT_ (-1)
IMPORT HANDLE WINAPI SelectObject(HANDLE hdc, HANDLE object);
IMPORT HANDLE WINAPI GetStockObject(int index);
IMPORT BOOL WINAPI DeleteObject(HANDLE object);
IMPORT BOOL WINAPI PatBlt(HANDLE hdc, int x, int y, int width, int height, DWORD rop);
IMPORT BOOL WINAPI ExtTextOutW(HANDLE hdc, int x, int y, UINT options, const RECT_ *rect, LPCWSTR text, UINT count, const INT *dx);
IMPORT BOOL WINAPI GetTextExtentPoint32W(HANDLE hdc, LPCWSTR text, int count, SIZE_ *size);
IMPORT int WINAPI SetBkMode(HANDLE hdc, int mode);
IMPORT DWORD WINAPI SetTextColor(HANDLE hdc, DWORD color);
IMPORT DWORD WINAPI SetBkColor(HANDLE hdc, DWORD color);
IMPORT int WINAPI IntersectClipRect(HANDLE hdc, int left, int top, int right, int bottom);
IMPORT int WINAPI SaveDC(HANDLE hdc);
IMPORT BOOL WINAPI RestoreDC(HANDLE hdc, int saved);
IMPORT HANDLE WINAPI CreateSolidBrush(DWORD color);
#define PATCOPY_ 0x00F00021u
#define DSTINVERT_ 0x00550009u
#define COLOR_WINDOW_ 5
#define COLOR_WINDOWFRAME_ 6
#define COLOR_WINDOWTEXT_ 8
#define COLOR_BTNFACE_ 15
#define COLOR_BTNSHADOW_ 16
#define COLOR_GRAYTEXT_ 17
#define COLOR_BTNTEXT_ 18
#define COLOR_BTNHILIGHT_ 20
#define COLOR_3DDKSHADOW_ 21
#define COLOR_BACKGROUND_ 1

/* The brush of a system colour or a brush handle (a small number + 1 is a colour index, as for window classes). */
static HANDLE brush_of(HANDLE brush) { return ((ULONG_PTR)brush >> 16) == 0 && brush ? GetSysColorBrush((int)(ULONG_PTR)brush - 1) : brush; }
EXPORT int WINAPI FillRect(HANDLE hdc, const RECT_ *r, HANDLE brush) {
  if (!r || !hdc) return 0;
  brush = brush_of(brush);
  HANDLE old = SelectObject(hdc, brush);
  PatBlt(hdc, r->left, r->top, r->right - r->left, r->bottom - r->top, PATCOPY_);
  SelectObject(hdc, old);
  return 1;
}
EXPORT int WINAPI FrameRect(HANDLE hdc, const RECT_ *r, HANDLE brush) {
  if (!r || !hdc) return 0;
  const int w = r->right - r->left, h = r->bottom - r->top;
  if (w <= 0 || h <= 0) return 1;
  HANDLE old = SelectObject(hdc, brush_of(brush));
  PatBlt(hdc, r->left, r->top, w, 1, PATCOPY_);
  PatBlt(hdc, r->left, r->bottom - 1, w, 1, PATCOPY_);
  PatBlt(hdc, r->left, r->top + 1, 1, h - 2, PATCOPY_);
  PatBlt(hdc, r->right - 1, r->top + 1, 1, h - 2, PATCOPY_);
  SelectObject(hdc, old);
  return 1;
}
EXPORT BOOL WINAPI DrawFocusRect(HANDLE hdc, const RECT_ *r) {
  if (!r || !hdc) return FALSE;
  const int w = r->right - r->left, h = r->bottom - r->top;
  if (w <= 0 || h <= 0) return TRUE;
  PatBlt(hdc, r->left, r->top, w, 1, DSTINVERT_);
  PatBlt(hdc, r->left, r->bottom - 1, w, 1, DSTINVERT_);
  PatBlt(hdc, r->left, r->top + 1, 1, h - 2, DSTINVERT_);
  PatBlt(hdc, r->right - 1, r->top + 1, 1, h - 2, DSTINVERT_);
  return TRUE;
}

/* ---- text ---------------------------------------------------------------------------------------------------------------------------------------------------------- */
#define DT_CENTER_ 0x1
#define DT_RIGHT_ 0x2
#define DT_VCENTER_ 0x4
#define DT_BOTTOM_ 0x8
#define DT_WORDBREAK_ 0x10
#define DT_SINGLELINE_ 0x20
#define DT_NOPREFIX_ 0x800
#define DT_NOCLIP_ 0x100
#define DT_EXPANDTABS_ 0x40

typedef struct { WCHAR text[1024]; } Line_;
/* Draws a string in a rectangle (a small DrawText): alignment, a single line or lines broken at '\n' and, with DT_WORDBREAK, at spaces; '&' marks a mnemonic (dropped, "&&" is '&'). */
static void ctl_text(HANDLE hdc, const WCHAR *s, RECT_ *r, DWORD flags, int *used_height) {
  WCHAR clean[1024];
  int n = 0;
  for (; s && *s && n < 1023; ++s) {
    if (*s == L'&' && !(flags & DT_NOPREFIX_)) { if (s[1] == L'&') { clean[n++] = L'&'; ++s; } continue; }
    clean[n++] = *s;
  }
  clean[n] = 0;
  SIZE_ unit;
  GetTextExtentPoint32W(hdc, L"Ay", 2, &unit);
  const int line_height = unit.cy > 0 ? unit.cy : 16;
  const int width = r->right - r->left;
  int starts[128], lengths[128], lines = 0;
  for (int i = 0; i <= n && lines < 128;) {
    int end = i;
    while (end < n && clean[end] != L'\n' && clean[end] != L'\r') ++end;
    int from = i;
    for (;;) {
      int len = end - from, fit = len;
      if (flags & DT_WORDBREAK_) {
        SIZE_ sz;
        GetTextExtentPoint32W(hdc, clean + from, len, &sz);
        if (sz.cx > width && len > 1) {
          fit = len;
          while (fit > 1) {
            GetTextExtentPoint32W(hdc, clean + from, fit, &sz);
            if (sz.cx <= width) break;
            --fit;
          }
          int brk = fit;
          while (brk > 0 && clean[from + brk] != L' ' && clean[from + brk - 1] != L' ') --brk;
          if (brk > 0) fit = brk;
        }
      }
      if (lines < 128) { starts[lines] = from; lengths[lines] = fit; ++lines; }
      from += fit;
      while (from < end && clean[from] == L' ' && (flags & DT_WORDBREAK_) && fit != end - (from - fit)) { ++from; break; }
      if (from >= end) break;
    }
    i = end + 1;
    if (end < n && clean[end] == L'\r' && clean[end + 1] == L'\n') ++i;
    if (end >= n) break;
  }
  if (flags & DT_SINGLELINE_) { lines = 1; starts[0] = 0; lengths[0] = n; }
  const int total = lines * line_height;
  int y = r->top;
  if (flags & DT_VCENTER_) y = r->top + (r->bottom - r->top - total) / 2;
  else if (flags & DT_BOTTOM_) y = r->bottom - total;
  if (used_height) *used_height = total;
  for (int l = 0; l < lines; ++l, y += line_height) {
    SIZE_ sz;
    GetTextExtentPoint32W(hdc, clean + starts[l], lengths[l], &sz);
    int x = r->left;
    if (flags & DT_CENTER_) x = r->left + (width - sz.cx) / 2;
    else if (flags & DT_RIGHT_) x = r->right - sz.cx;
    ExtTextOutW(hdc, x, y, (flags & DT_NOCLIP_) ? 0 : 4 /* ETO_CLIPPED */, r, clean + starts[l], (UINT)lengths[l], NULL);
  }
}

static LONG ctl_style(HANDLE hwnd) { return GetWindowLongW(hwnd, -16); }
/* The state block of a control: its pointer is kept in the window's private data (win32k: NtUserCallHwndParam GetPrivateData/SetPrivateData), not in the window extra bytes the program can use. */
enum { CHP_GET_PRIVATE_DATA = 30, CHP_SET_PRIVATE_DATA = 31 };
static void *ctl_state(HANDLE hwnd) {
  struct { DWORD offset, size; ULONG_PTR value; } params = {0, 8, 0};
  return (void *)NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)&params, CHP_GET_PRIVATE_DATA);
}
static void ctl_set_state(HANDLE hwnd, void *p) {
  struct { DWORD offset, size; ULONG_PTR value; } params = {0, 8, (ULONG_PTR)p};
  NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)&params, CHP_SET_PRIVATE_DATA);
}
static void *ctl_alloc(HANDLE hwnd, SIZE_T bytes) {
  void *p = HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, bytes);
  if (p) ctl_set_state(hwnd, p);
  return p;
}
static void ctl_free(HANDLE hwnd) { void *p = ctl_state(hwnd); if (p) { ctl_set_state(hwnd, NULL); HeapFree(GetProcessHeap(), 0, p); } }

/* The window a control reports to: the parent it was created with (the first member of every state block). */
static HANDLE ctl_parent(HANDLE hwnd) { HANDLE *state = ctl_state(hwnd); return state ? *state : GetParent(hwnd); }

/* The colours and background of a control: the parent is asked (WM_CTLCOLOR*), without a parent or an answer the colours of the system are used. */
static HANDLE ctl_color(HANDLE hwnd, HANDLE hdc, UINT message) {
  HANDLE parent = ctl_parent(hwnd);
  HANDLE brush = (HANDLE)SendMessageW(parent ? parent : hwnd, message, (ULONG_PTR)hdc, (LONG_PTR)hwnd);
  if (!brush) {
    const int text = message == 0x0133 /* WM_CTLCOLOREDIT */ ? COLOR_WINDOWTEXT_ : message == 0x0135 /* WM_CTLCOLORBTN */ ? COLOR_BTNTEXT_ : COLOR_WINDOWTEXT_;
    const int back = message == 0x0133 ? COLOR_WINDOW_ : COLOR_BTNFACE_;
    SetTextColor(hdc, GetSysColor(text));
    SetBkColor(hdc, GetSysColor(back));
    brush = GetSysColorBrush(back);
  }
  return brush;
}
/* The device context of a control for painting outside WM_PAINT, clipped to the client area. */
static void ctl_clip(HANDLE hwnd, HANDLE hdc) { RECT_ r; GetClientRect(hwnd, &r); IntersectClipRect(hdc, r.left, r.top, r.right, r.bottom); }

static void ctl_notify(HANDLE hwnd, UINT code) {
  HANDLE parent = ctl_parent(hwnd);
  if (parent) SendMessageW(parent, 0x0111 /* WM_COMMAND */, (ULONG_PTR)(((ULONG_PTR)code << 16) | ((ULONG_PTR)GetWindowLongPtrW(hwnd, -12) & 0xFFFF)), (LONG_PTR)hwnd);
}
static BOOL ctl_is_visible_enabled_window(HANDLE hwnd) { return IsWindowVisible(hwnd); }
static void ctl_repaint(HANDLE hwnd) { if (ctl_is_visible_enabled_window(hwnd)) InvalidateRect(hwnd, NULL, FALSE); }
static void ctl_select_font(HANDLE hdc, HANDLE font) { if (font) SelectObject(hdc, font); }

/* ---- Static ----------------------------------------------------------------------------------------------------------------------------------------------------- */
#define SS_TYPEMASK_ 0x1F
enum { SS_LEFT_, SS_CENTER_, SS_RIGHT_, SS_ICON_, SS_BLACKRECT_, SS_GRAYRECT_, SS_WHITERECT_, SS_BLACKFRAME_, SS_GRAYFRAME_, SS_WHITEFRAME_, SS_USERITEM_, SS_SIMPLE_, SS_LEFTNOWORDWRAP_,
       SS_OWNERDRAW_, SS_BITMAP_, SS_ENHMETAFILE_, SS_ETCHEDHORZ_, SS_ETCHEDVERT_, SS_ETCHEDFRAME_ };
#define SS_NOPREFIX_ 0x80
#define SS_NOTIFY_ 0x100
#define SS_SUNKEN_ 0x1000
#define SS_ENDELLIPSIS_ 0x4000
#define STM_SETICON_ 0x170
#define STM_GETICON_ 0x171
#define STM_SETIMAGE_ 0x172
#define STM_GETIMAGE_ 0x173
#define DLGC_STATIC_ 0x100
typedef struct { HANDLE parent, image, font; } StaticState;

EXPORT BOOL WINAPI DrawIconEx(HANDLE hdc, int x, int y, HANDLE icon, int cx, int cy, UINT step, HANDLE background, UINT flags);
IMPORT HANDLE WINAPI CreateCompatibleDC(HANDLE hdc);
IMPORT BOOL WINAPI DeleteDC(HANDLE hdc);
IMPORT BOOL WINAPI BitBlt(HANDLE hdc, int x, int y, int cx, int cy, HANDLE source, int sx, int sy, DWORD rop);
typedef struct { LONG type, width, height, width_bytes; WORD planes, bits; void *data; } Bitmap_;
IMPORT int WINAPI GetObjectW(HANDLE object, int size, void *buffer);
#define SRCCOPY_ 0x00CC0020u
static void static_paint(HANDLE hwnd, HANDLE hdc, StaticState *st) {
  RECT_ r;
  const LONG style = ctl_style(hwnd);
  const UINT type = (UINT)style & SS_TYPEMASK_;
  GetClientRect(hwnd, &r);
  const int saved = SaveDC(hdc);
  ctl_clip(hwnd, hdc);
  ctl_select_font(hdc, st ? st->font : NULL);
  HANDLE brush = ctl_color(hwnd, hdc, 0x0138 /* WM_CTLCOLORSTATIC */);
  switch (type) {
    case SS_LEFT_: case SS_CENTER_: case SS_RIGHT_: case SS_SIMPLE_: case SS_LEFTNOWORDWRAP_: {
      WCHAR text[512];
      FillRect(hdc, &r, brush);
      const int n = GetWindowTextW(hwnd, text, 512);
      if (n > 0) {
        DWORD flags = (type == SS_CENTER_ ? DT_CENTER_ : type == SS_RIGHT_ ? DT_RIGHT_ : 0) | ((style & SS_NOPREFIX_) ? DT_NOPREFIX_ : 0) | DT_EXPANDTABS_;
        if (type == SS_SIMPLE_ || type == SS_LEFTNOWORDWRAP_) flags |= DT_SINGLELINE_; else flags |= DT_WORDBREAK_;
        if (style & 0x0200 /* SS_CENTERIMAGE */) flags |= DT_VCENTER_ | DT_SINGLELINE_;
        if (!(style & WS_DISABLED_)) SetBkMode(hdc, 1 /* TRANSPARENT */);
        else SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT_));
        ctl_text(hdc, text, &r, flags, NULL);
      }
      break;
    }
    case SS_ICON_: case SS_BITMAP_: {
      FillRect(hdc, &r, brush);
      if (st && st->image) {
        if (type == SS_ICON_) DrawIconEx(hdc, 0, 0, st->image, 0, 0, 0, NULL, 3 | 4 /* DI_NORMAL | DI_DEFAULTSIZE */);
        else {
          Bitmap_ bm;
          HANDLE mem = CreateCompatibleDC(hdc);
          if (mem && GetObjectW(st->image, sizeof bm, &bm)) {
            HANDLE old = SelectObject(mem, st->image);
            BitBlt(hdc, 0, 0, bm.width, bm.height, mem, 0, 0, SRCCOPY_);
            SelectObject(mem, old);
          }
          if (mem) DeleteDC(mem);
        }
      }
      break;
    }
    case SS_BLACKRECT_: FillRect(hdc, &r, GetSysColorBrush(COLOR_WINDOWFRAME_)); break;
    case SS_GRAYRECT_: FillRect(hdc, &r, GetSysColorBrush(COLOR_BACKGROUND_)); break;
    case SS_WHITERECT_: FillRect(hdc, &r, GetSysColorBrush(COLOR_WINDOW_)); break;
    case SS_BLACKFRAME_: FrameRect(hdc, &r, GetSysColorBrush(COLOR_WINDOWFRAME_)); break;
    case SS_GRAYFRAME_: FrameRect(hdc, &r, GetSysColorBrush(COLOR_BACKGROUND_)); break;
    case SS_WHITEFRAME_: FrameRect(hdc, &r, GetSysColorBrush(COLOR_WINDOW_)); break;
    case SS_ETCHEDHORZ_: case SS_ETCHEDVERT_: case SS_ETCHEDFRAME_: FrameRect(hdc, &r, GetSysColorBrush(COLOR_BTNSHADOW_)); break;
    default: FillRect(hdc, &r, brush); break;
  }
  RestoreDC(hdc, saved);
}
static LONG_PTR WINAPI static_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  StaticState *st = message == 0x0081 /* WM_NCCREATE */ ? NULL : ctl_state(hwnd);
  const LONG style = ctl_style(hwnd);
  const UINT type = (UINT)style & SS_TYPEMASK_;
  switch (message) {
    case 0x0081: {      /* WM_NCCREATE */
      if (!(st = ctl_alloc(hwnd, sizeof(StaticState)))) return FALSE;
      st->parent = (HANDLE)((CreateStruct_ *)lparam)->parent;
      if ((style & SS_SUNKEN_) || type == SS_ETCHEDHORZ_ || type == SS_ETCHEDVERT_) SetWindowLongW(hwnd, -20, GetWindowLongW(hwnd, -20) | 0x20000 /* WS_EX_STATICEDGE */);
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case 0x0083: {      /* WM_NCCALCSIZE: the etched lines have no client area across */
      const LONG_PTR r = DefWindowProcW(hwnd, message, wparam, lparam);
      RECT_ *rect = wparam ? (RECT_ *)lparam : (RECT_ *)lparam;
      if (type == SS_ETCHEDHORZ_) rect->bottom = rect->top;
      else if (type == SS_ETCHEDVERT_) rect->right = rect->left;
      return r;
    }
    case 0x0082: ctl_free(hwnd); return DefWindowProcW(hwnd, message, wparam, lparam);      /* WM_NCDESTROY */
    case 0x0014: return 1;      /* WM_ERASEBKGND: the background is painted with the text */
    case 0x000F: case 0x0318: { /* WM_PAINT, WM_PRINTCLIENT */
      PAINTSTRUCT_ ps;
      HANDLE hdc = message == 0x0318 ? (HANDLE)wparam : BeginPaint(hwnd, &ps);
      if (hdc) { static_paint(hwnd, hdc, st); if (message == 0x000F) EndPaint(hwnd, &ps); }
      return 0;
    }
    case 0x000A: ctl_repaint(hwnd); return 0;      /* WM_ENABLE */
    case 0x0030: if (st) st->font = (HANDLE)wparam; if (lparam) ctl_repaint(hwnd); return 0;      /* WM_SETFONT */
    case 0x0031: return (LONG_PTR)(st ? st->font : NULL);      /* WM_GETFONT */
    case 0x000C: { const LONG_PTR r = DefWindowProcW(hwnd, message, wparam, lparam); ctl_repaint(hwnd); return r; }      /* WM_SETTEXT */
    case 0x0084: return (style & SS_NOTIFY_) ? DefWindowProcW(hwnd, message, wparam, lparam) : HTTRANSPARENT_;      /* WM_NCHITTEST */
    case 0x0087: return DLGC_STATIC_;      /* WM_GETDLGCODE */
    case STM_SETICON_: case STM_SETIMAGE_: {
      HANDLE old = st ? st->image : NULL;
      if (st) st->image = (HANDLE)(message == STM_SETICON_ ? wparam : lparam);
      ctl_repaint(hwnd);
      return (LONG_PTR)old;
    }
    case STM_GETICON_: case STM_GETIMAGE_: return (LONG_PTR)(st ? st->image : NULL);
    case 0x0201: case 0x0203: case 0x0202:      /* WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONUP: a click notifies the parent when SS_NOTIFY */
      if (style & SS_NOTIFY_) { if (message == 0x0201) ctl_notify(hwnd, 0 /* STN_CLICKED */); else if (message == 0x0203) ctl_notify(hwnd, 1 /* STN_DBLCLK */); }
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* ---- Button ----------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { BS_PUSHBUTTON_, BS_DEFPUSHBUTTON_, BS_CHECKBOX_, BS_AUTOCHECKBOX_, BS_RADIOBUTTON_, BS_3STATE_, BS_AUTO3STATE_, BS_GROUPBOX_, BS_USERBUTTON_, BS_AUTORADIOBUTTON_, BS_PUSHBOX_,
       BS_OWNERDRAW_ };
#define BS_TYPEMASK_ 0xF
#define BS_LEFTTEXT_ 0x20
#define BS_NOTIFY_ 0x4000
#define BS_PUSHLIKE_ 0x1000
#define BM_GETCHECK_ 0xF0
#define BM_SETCHECK_ 0xF1
#define BM_GETSTATE_ 0xF2
#define BM_SETSTATE_ 0xF3
#define BM_SETSTYLE_ 0xF4
#define BM_CLICK_ 0xF5
#define BM_GETIMAGE_ 0xF6
#define BM_SETIMAGE_ 0xF7
#define BM_SETDONTCLICK_ 0xF8
#define BST_UNCHECKED_ 0
#define BST_CHECKED_ 1
#define BST_INDETERMINATE_ 2
#define BST_PUSHED_ 4
#define BST_FOCUS_ 8
#define BN_CLICKED_ 0
#define BN_PAINT_ 1
#define BN_HILITE_ 2
#define BN_UNHILITE_ 3
#define BN_DISABLE_ 4
#define BN_DBLCLK_ 5
#define BN_SETFOCUS_ 6
#define BN_KILLFOCUS_ 7
#define DLGC_WANTARROWS_ 1
#define DLGC_WANTCHARS_ 0x80
#define DLGC_UNDEFPUSHBUTTON_ 0x20
#define DLGC_DEFPUSHBUTTON_ 0x10
#define DLGC_RADIOBUTTON_ 0x40
#define DLGC_BUTTON_ 0x2000
#define WS_TABSTOP_ 0x00010000
#define WS_GROUP_ 0x00020000
typedef struct { HANDLE parent; DWORD state; HANDLE image, font; DWORD image_type; } ButtonState;

static UINT button_type(HANDLE hwnd) { return (UINT)ctl_style(hwnd) & BS_TYPEMASK_; }
static int button_is_check(UINT type) { return type == BS_CHECKBOX_ || type == BS_AUTOCHECKBOX_ || type == BS_3STATE_ || type == BS_AUTO3STATE_ || type == BS_RADIOBUTTON_ || type == BS_AUTORADIOBUTTON_; }
static int button_is_3state(UINT type) { return type == BS_3STATE_ || type == BS_AUTO3STATE_; }
static int button_is_radio(UINT type) { return type == BS_RADIOBUTTON_ || type == BS_AUTORADIOBUTTON_; }

static void button_paint(HANDLE hwnd, HANDLE hdc, ButtonState *st) {
  RECT_ r;
  const LONG style = ctl_style(hwnd);
  const UINT type = (UINT)style & BS_TYPEMASK_;
  const DWORD state = st ? st->state : 0;
  WCHAR text[256];
  GetClientRect(hwnd, &r);
  const int saved = SaveDC(hdc);
  ctl_clip(hwnd, hdc);
  ctl_select_font(hdc, st ? st->font : NULL);
  HANDLE brush = ctl_color(hwnd, hdc, 0x0135 /* WM_CTLCOLORBTN */);
  GetWindowTextW(hwnd, text, 256);
  SetBkMode(hdc, 1);
  if (style & WS_DISABLED_) SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT_));
  if (type == BS_GROUPBOX_) {
    RECT_ frame = r;
    frame.top += 8;
    FillRect(hdc, &r, brush);
    FrameRect(hdc, &frame, GetSysColorBrush(COLOR_BTNSHADOW_));
    RECT_ title = {r.left + 8, r.top, r.right - 8, r.top + 16};
    ctl_text(hdc, text, &title, DT_SINGLELINE_ | DT_NOCLIP_, NULL);
  } else if (button_is_check(type) && !(style & BS_PUSHLIKE_)) {
    FillRect(hdc, &r, brush);
    const int box = 13, y = (r.bottom - r.top - box) / 2;
    const int left_text = (style & BS_LEFTTEXT_) != 0;
    RECT_ b = {left_text ? r.right - box : r.left, r.top + y, (left_text ? r.right - box : r.left) + box, r.top + y + box};
    FillRect(hdc, &b, GetSysColorBrush(COLOR_WINDOW_));
    FrameRect(hdc, &b, GetSysColorBrush(COLOR_BTNSHADOW_));
    if ((state & 3) == BST_CHECKED_ || (state & 3) == BST_INDETERMINATE_) {
      RECT_ mark = {b.left + 3, b.top + 3, b.right - 3, b.bottom - 3};
      FillRect(hdc, &mark, GetSysColorBrush(button_is_radio(type) || (state & 3) == BST_CHECKED_ ? COLOR_WINDOWTEXT_ : COLOR_BTNSHADOW_));
    }
    RECT_ t = {left_text ? r.left : r.left + box + 4, r.top, left_text ? r.right - box - 4 : r.right, r.bottom};
    ctl_text(hdc, text, &t, DT_VCENTER_ | DT_SINGLELINE_, NULL);
    if (state & BST_FOCUS_) DrawFocusRect(hdc, &t);
  } else {
    FillRect(hdc, &r, brush);
    FrameRect(hdc, &r, GetSysColorBrush(type == BS_DEFPUSHBUTTON_ ? COLOR_WINDOWFRAME_ : COLOR_BTNSHADOW_));
    RECT_ t = r;
    if (state & BST_PUSHED_) { t.left++; t.top++; }
    ctl_text(hdc, text, &t, DT_CENTER_ | DT_VCENTER_ | DT_SINGLELINE_, NULL);
    if (state & BST_FOCUS_) { RECT_ f = {r.left + 3, r.top + 3, r.right - 3, r.bottom - 3}; DrawFocusRect(hdc, &f); }
  }
  RestoreDC(hdc, saved);
}
static void button_draw_now(HANDLE hwnd, ButtonState *st) {      /* painting outside WM_PAINT: through the window's own device context */
  if (!IsWindowVisible(hwnd)) return;
  HANDLE hdc = GetDC(hwnd);
  if (hdc) { button_paint(hwnd, hdc, st); ReleaseDC(hwnd, hdc); }
}
/* An auto radio button that is checked unchecks the others of its group (the controls from the one with WS_GROUP to the next that has it). */
static void button_uncheck_group(HANDLE hwnd) {
  HANDLE first = hwnd;
  for (HANDLE w = hwnd; w; w = GetWindow(w, 3 /* GW_HWNDPREV */)) { first = w; if (ctl_style(w) & WS_GROUP_) break; }
  for (HANDLE w = first; w; w = GetWindow(w, 2 /* GW_HWNDNEXT */)) {
    if (w != first && (ctl_style(w) & WS_GROUP_)) break;
    if (w != hwnd && (SendMessageW(w, 0x0087, 0, 0) & 0x40 /* DLGC_RADIOBUTTON */)) SendMessageW(w, BM_SETCHECK_, BST_UNCHECKED_, 0);
  }
}

static LONG_PTR WINAPI button_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  ButtonState *st = message == 0x0081 ? NULL : ctl_state(hwnd);
  const LONG style = ctl_style(hwnd);
  const UINT type = (UINT)style & BS_TYPEMASK_;
  if (!st && message != 0x0081 && message != 0x0082) return DefWindowProcW(hwnd, message, wparam, lparam);
  switch (message) {
    case 0x0081:      /* WM_NCCREATE */
      if (!(st = ctl_alloc(hwnd, sizeof(ButtonState)))) return FALSE;
      st->parent = (HANDLE)((CreateStruct_ *)lparam)->parent;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0082: ctl_free(hwnd); return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0087: {      /* WM_GETDLGCODE */
      LONG_PTR code = DLGC_BUTTON_;
      if (type == BS_PUSHBUTTON_ || type == BS_USERBUTTON_) code |= DLGC_UNDEFPUSHBUTTON_;
      else if (type == BS_DEFPUSHBUTTON_) code |= DLGC_DEFPUSHBUTTON_;
      else if (button_is_radio(type)) code |= DLGC_RADIOBUTTON_;
      else if (type == BS_GROUPBOX_) code = 0x100 /* DLGC_STATIC */;
      return code;
    }
    case 0x0084: return type == BS_GROUPBOX_ ? HTTRANSPARENT_ : DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0014: return 1;      /* WM_ERASEBKGND */
    case 0x000F: case 0x0318: {
      PAINTSTRUCT_ ps;
      HANDLE hdc = message == 0x0318 ? (HANDLE)wparam : BeginPaint(hwnd, &ps);
      if (hdc) { if (type != BS_OWNERDRAW_) button_paint(hwnd, hdc, st); if (message == 0x000F) EndPaint(hwnd, &ps); }
      return 0;
    }
    case 0x000A: ctl_repaint(hwnd); return 0;      /* WM_ENABLE */
    case 0x000C: { const LONG_PTR r = DefWindowProcW(hwnd, message, wparam, lparam); ctl_repaint(hwnd); return r; }
    case 0x0030: st->font = (HANDLE)wparam; if (lparam) ctl_repaint(hwnd); return 0;
    case 0x0031: return (LONG_PTR)st->font;
    case 0x0007:      /* WM_SETFOCUS */
      st->state |= BST_FOCUS_;
      button_draw_now(hwnd, st);
      if (style & BS_NOTIFY_) ctl_notify(hwnd, BN_SETFOCUS_);
      if (type == BS_AUTORADIOBUTTON_ && !(st->state & BST_CHECKED_)) {      /* an auto radio button that gets the focus is clicked */
        SendMessageW(hwnd, BM_SETCHECK_, BST_CHECKED_, 0);
        ctl_notify(hwnd, BN_CLICKED_);
      }
      return 0;
    case 0x0008:      /* WM_KILLFOCUS */
      st->state &= ~(DWORD)BST_FOCUS_;
      button_draw_now(hwnd, st);
      if (style & BS_NOTIFY_) ctl_notify(hwnd, BN_KILLFOCUS_);
      if (st->state & BST_PUSHED_) ReleaseCapture();
      return 0;
    case 0x0201: case 0x0203:      /* WM_LBUTTONDOWN, WM_LBUTTONDBLCLK */
      if (style & WS_DISABLED_) return 0;
      if (message == 0x0203 && (style & BS_NOTIFY_)) ctl_notify(hwnd, BN_DBLCLK_);
      SetCapture(hwnd);
      SetFocus(hwnd);
      SendMessageW(hwnd, BM_SETSTATE_, TRUE, 0);
      return 0;
    case 0x0202: {      /* WM_LBUTTONUP */
      if (!(st->state & BST_PUSHED_)) return 0;
      SendMessageW(hwnd, BM_SETSTATE_, FALSE, 0);
      ReleaseCapture();
      RECT_ r;
      POINT_ p = {(short)(lparam & 0xFFFF), (short)((lparam >> 16) & 0xFFFF)};
      GetClientRect(hwnd, &r);
      if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) {
        switch (type) {
          case BS_AUTOCHECKBOX_: SendMessageW(hwnd, BM_SETCHECK_, !(st->state & 3), 0); break;
          case BS_AUTORADIOBUTTON_: SendMessageW(hwnd, BM_SETCHECK_, BST_CHECKED_, 0); break;
          case BS_AUTO3STATE_: SendMessageW(hwnd, BM_SETCHECK_, ((st->state & 3) + 1) % 3, 0); break;
        }
        ctl_notify(hwnd, BN_CLICKED_);
      }
      return 0;
    }
    case 0x0215: case 0x001F:      /* WM_CAPTURECHANGED, WM_CANCELMODE: the pressed state ends */
      if ((message == 0x001F || (HANDLE)lparam != hwnd) && (st->state & BST_PUSHED_)) SendMessageW(hwnd, BM_SETSTATE_, FALSE, 0);
      return message == 0x001F ? DefWindowProcW(hwnd, message, wparam, lparam) : 0;
    case 0x0100: case 0x0101:      /* WM_KEYDOWN, WM_KEYUP: the space bar presses and clicks */
      if (wparam == 0x20) {
        if (message == 0x0100 && !(lparam & 0x40000000)) SendMessageW(hwnd, BM_SETSTATE_, TRUE, 0);
        else if (message == 0x0101 && (st->state & BST_PUSHED_)) {
          SendMessageW(hwnd, BM_SETSTATE_, FALSE, 0);
          switch (type) {
            case BS_AUTOCHECKBOX_: SendMessageW(hwnd, BM_SETCHECK_, !(st->state & 3), 0); break;
            case BS_AUTORADIOBUTTON_: SendMessageW(hwnd, BM_SETCHECK_, BST_CHECKED_, 0); break;
            case BS_AUTO3STATE_: SendMessageW(hwnd, BM_SETCHECK_, ((st->state & 3) + 1) % 3, 0); break;
          }
          ctl_notify(hwnd, BN_CLICKED_);
        }
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case BM_GETCHECK_: return button_is_check(type) ? (LONG_PTR)(st->state & 3) : 0;
    case BM_SETCHECK_: {
      if (button_is_radio(type)) {
        const LONG s = ctl_style(hwnd);
        SetWindowLongW(hwnd, -16, wparam ? (s | WS_TABSTOP_) : (s & ~WS_TABSTOP_));
      } else if (!button_is_check(type)) return 0;
      if (wparam > (button_is_3state(type) ? (ULONG_PTR)BST_INDETERMINATE_ : (ULONG_PTR)BST_CHECKED_)) wparam = BST_UNCHECKED_;
      if ((st->state & 3) != wparam) {
        st->state = (st->state & ~3u) | (DWORD)wparam;
        if (type == BS_AUTORADIOBUTTON_ && wparam == BST_CHECKED_) button_uncheck_group(hwnd);
        ctl_repaint(hwnd);
      }
      return 0;
    }
    case BM_GETSTATE_: return (LONG_PTR)(st->state & 0xF);
    case BM_SETSTATE_: {
      const DWORD old = st->state;
      if (wparam) st->state |= BST_PUSHED_; else st->state &= ~(DWORD)BST_PUSHED_;
      if (old != st->state) button_draw_now(hwnd, st);
      return 0;
    }
    case BM_CLICK_:
      SendMessageW(hwnd, 0x0201, 0, 0);
      SendMessageW(hwnd, 0x0202, 0, 0);
      return 0;
    case BM_SETSTYLE_: {
      const LONG s = ctl_style(hwnd);
      SetWindowLongW(hwnd, -16, (s & ~0xFFFF) | (LONG)(wparam & 0xFFFF));
      if (lparam) ctl_repaint(hwnd);
      return 0;
    }
    case BM_GETIMAGE_: return (LONG_PTR)st->image;
    case BM_SETIMAGE_: { HANDLE old = st->image; st->image = (HANDLE)lparam; st->image_type = (DWORD)wparam; ctl_repaint(hwnd); return (LONG_PTR)old; }
    case BM_SETDONTCLICK_: return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* ---- Edit ------------------------------------------------------------------------------------------------------------------------------------------------------- *
 * A text model with a selection: single-line and multi-line (lines are broken by CR LF in the text), the EM_* messages, character and key input, notifications, a plain painting of the text
 * and the selection. Not done: scrolling and wrapping, the undo buffer, the clipboard messages (WM_COPY, WM_CUT, WM_PASTE do nothing), IME, rich text. */
#define ES_MULTILINE_ 0x4
#define ES_UPPERCASE_ 0x8
#define ES_LOWERCASE_ 0x10
#define ES_PASSWORD_ 0x20
#define ES_AUTOVSCROLL_ 0x40
#define ES_AUTOHSCROLL_ 0x80
#define ES_NOHIDESEL_ 0x100
#define ES_READONLY_ 0x800
#define ES_WANTRETURN_ 0x1000
#define ES_NUMBER_ 0x2000
#define EN_SETFOCUS_ 0x100
#define EN_KILLFOCUS_ 0x200
#define EN_CHANGE_ 0x300
#define EN_UPDATE_ 0x400
#define EN_MAXTEXT_ 0x501
#define EDIT_DEFAULT_LIMIT 30000u
typedef struct { HANDLE parent; WCHAR *text; UINT length, capacity, limit, sel_start, sel_end; WCHAR password; BOOL modified, readonly; HANDLE font; UINT margins; } EditState;

static void edit_mirror(HANDLE hwnd, EditState *st) { DefWindowProcW(hwnd, 0x000C, 0, (LONG_PTR)st->text); }      /* (the window manager keeps the text for FindWindow and the like) */
static BOOL edit_reserve(EditState *st, UINT need) {
  if (need + 1 <= st->capacity) return TRUE;
  UINT capacity = st->capacity ? st->capacity : 64;
  while (capacity < need + 1) capacity *= 2;
  WCHAR *bigger = HeapAlloc(GetProcessHeap(), 8, (SIZE_T)capacity * 2);
  if (!bigger) return FALSE;
  for (UINT i = 0; i < st->length; ++i) bigger[i] = st->text[i];
  if (st->text) HeapFree(GetProcessHeap(), 0, st->text);
  st->text = bigger;
  st->capacity = capacity;
  return TRUE;
}
/* Replaces [from, to) with `s` (n characters), within the limit. Answers the number of characters put in. */
static UINT edit_replace(HANDLE hwnd, EditState *st, UINT from, UINT to, const WCHAR *s, UINT n, BOOL notify) {
  const LONG style = ctl_style(hwnd);
  if (to < from) { UINT x = from; from = to; to = x; }
  if (to > st->length) to = st->length;
  if (from > st->length) from = st->length;
  const UINT kept = st->length - (to - from);
  if (kept >= st->limit) n = 0; else if (n > st->limit - kept) { n = st->limit - kept; if (notify) ctl_notify(hwnd, EN_MAXTEXT_); }
  if (!edit_reserve(st, kept + n)) return 0;
  const UINT tail = st->length - to;
  if (n > to - from) for (UINT i = tail; i > 0; --i) st->text[from + n + i - 1] = st->text[to + i - 1];
  else for (UINT i = 0; i < tail; ++i) st->text[from + n + i] = st->text[to + i];
  for (UINT i = 0; i < n; ++i) {
    WCHAR c = s[i];
    if (style & ES_UPPERCASE_) { if (c >= 'a' && c <= 'z') c = (WCHAR)(c - 32); }
    else if (style & ES_LOWERCASE_) { if (c >= 'A' && c <= 'Z') c = (WCHAR)(c + 32); }
    st->text[from + i] = c;
  }
  st->length = kept + n;
  st->text[st->length] = 0;
  st->sel_start = st->sel_end = from + n;
  st->modified = TRUE;
  edit_mirror(hwnd, st);
  if (notify) { ctl_notify(hwnd, EN_UPDATE_); ctl_notify(hwnd, EN_CHANGE_); }
  ctl_repaint(hwnd);
  return n;
}
static UINT edit_line_count(EditState *st, LONG style) {
  UINT lines = 1;
  if (style & ES_MULTILINE_) for (UINT i = 0; i < st->length; ++i) if (st->text[i] == L'\n') ++lines;
  return lines;
}
/* The index of the first character of line `line` (the length of the text for a line that is not there: -1 is answered by the caller). */
static BOOL edit_line_start(EditState *st, LONG style, UINT line, UINT *start) {
  UINT l = 0, i = 0;
  if (!(style & ES_MULTILINE_)) { *start = 0; return line == 0; }
  while (l < line) {
    while (i < st->length && st->text[i] != L'\n') ++i;
    if (i >= st->length) return FALSE;
    ++i; ++l;
  }
  *start = i;
  return TRUE;
}
static UINT edit_line_length_at(EditState *st, LONG style, UINT start) {
  UINT end = start;
  while (end < st->length && !((style & ES_MULTILINE_) && (st->text[end] == L'\r' || st->text[end] == L'\n'))) ++end;
  return end - start;
}
static UINT edit_line_of(EditState *st, LONG style, UINT index) {
  UINT line = 0;
  if (!(style & ES_MULTILINE_)) return 0;
  if (index > st->length) index = st->length;
  for (UINT i = 0; i < index; ++i) if (st->text[i] == L'\n') ++line;
  return line;
}

static void edit_paint(HANDLE hwnd, HANDLE hdc, EditState *st) {
  RECT_ r;
  const LONG style = ctl_style(hwnd);
  GetClientRect(hwnd, &r);
  const int saved = SaveDC(hdc);
  ctl_clip(hwnd, hdc);
  ctl_select_font(hdc, st->font);
  HANDLE brush = ctl_color(hwnd, hdc, ((style & WS_DISABLED_) || st->readonly) ? 0x0138 : 0x0133 /* WM_CTLCOLOREDIT */);
  FillRect(hdc, &r, brush);
  SetBkMode(hdc, 1);
  SIZE_ unit;
  GetTextExtentPoint32W(hdc, L"Ay", 2, &unit);
  const int line_height = unit.cy > 0 ? unit.cy : 16;
  const int left = r.left + 1 + (int)(st->margins & 0xFFFF), top = r.top + 1;
  UINT start = 0;
  for (UINT line = 0; edit_line_start(st, style, line, &start); ++line) {
    const UINT n = edit_line_length_at(st, style, start);
    WCHAR shown[512];
    UINT count = n > 511 ? 511 : n;
    for (UINT i = 0; i < count; ++i) shown[i] = (style & ES_PASSWORD_) && st->password ? st->password : st->text[start + i];
    const int y = top + (int)line * line_height;
    if (y >= r.bottom) break;
    const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
    const BOOL focused = GetFocus() == hwnd;
    if (a != b && (focused || (style & ES_NOHIDESEL_)) && b > start && a < start + n + 1) {
      const UINT s0 = a > start ? a - start : 0, s1 = b < start + n ? b - start : n;
      SIZE_ w0, w1;
      GetTextExtentPoint32W(hdc, shown, (int)(s0 > count ? count : s0), &w0);
      GetTextExtentPoint32W(hdc, shown, (int)(s1 > count ? count : s1), &w1);
      RECT_ hi = {left + w0.cx, y, left + w1.cx, y + line_height};
      FillRect(hdc, &hi, GetSysColorBrush(13 /* COLOR_HIGHLIGHT */));
    }
    if (count) {
      if (style & WS_DISABLED_) SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT_));
      ExtTextOutW(hdc, left, y, 0, NULL, shown, count, NULL);
    }
  }
  RestoreDC(hdc, saved);
}
static UINT edit_index_at(HANDLE hwnd, EditState *st, int x, int y) {
  const LONG style = ctl_style(hwnd);
  HANDLE hdc = GetDC(hwnd);
  if (!hdc) return st->length;
  ctl_select_font(hdc, st->font);
  SIZE_ unit;
  GetTextExtentPoint32W(hdc, L"Ay", 2, &unit);
  const int line_height = unit.cy > 0 ? unit.cy : 16;
  UINT line = (style & ES_MULTILINE_) && y > 0 ? (UINT)((y - 1) / line_height) : 0, start = 0;
  if (!edit_line_start(st, style, line, &start)) { line = edit_line_count(st, style) - 1; edit_line_start(st, style, line, &start); }
  const UINT n = edit_line_length_at(st, style, start);
  UINT best = 0;
  for (UINT i = 1; i <= n; ++i) {
    SIZE_ w;
    GetTextExtentPoint32W(hdc, st->text + start, (int)i, &w);
    if (1 + (int)(st->margins & 0xFFFF) + w.cx - 0 > x + 0 && 1 + (int)(st->margins & 0xFFFF) + w.cx - (w.cx / (int)i) / 2 > x) break;
    best = i;
  }
  ReleaseDC(hwnd, hdc);
  return start + best;
}
static void edit_set_sel(HANDLE hwnd, EditState *st, LONG a, LONG b) {
  if (a == -1) { b = a = (LONG)st->sel_end; if (0) b = a; st->sel_start = st->sel_end = (UINT)a; ctl_repaint(hwnd); return; }
  if (b == -1) b = (LONG)st->length;
  if (a < 0) a = 0;
  if (b < 0) b = 0;
  if ((UINT)a > st->length) a = (LONG)st->length;
  if ((UINT)b > st->length) b = (LONG)st->length;
  st->sel_start = (UINT)a;
  st->sel_end = (UINT)b;
  ctl_repaint(hwnd);
}

/* Copy and paste of the selection as CF_UNICODETEXT (the clipboard is shared with the desktop's own programs). */
static BOOL edit_copy(HANDLE hwnd, EditState *st) {
  const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
  if (a == b || (ctl_style(hwnd) & ES_PASSWORD_)) return FALSE;
  HANDLE memory = GlobalAlloc(2 /* GMEM_MOVEABLE */, ((SIZE_T)(b - a) + 1) * 2);
  WCHAR *dst = memory ? GlobalLock(memory) : NULL;
  if (!dst) { if (memory) GlobalFree(memory); return FALSE; }
  for (UINT i = a; i < b; ++i) dst[i - a] = st->text[i];
  dst[b - a] = 0;
  GlobalUnlock(memory);
  BOOL ok = FALSE;
  if (OpenClipboard(hwnd)) {
    EmptyClipboard();
    ok = SetClipboardData(13 /* CF_UNICODETEXT */, memory) != NULL;
    CloseClipboard();
  }
  if (!ok) GlobalFree(memory);
  return ok;
}
static void edit_paste(HANDLE hwnd, EditState *st) {
  if (st->readonly || (ctl_style(hwnd) & WS_DISABLED_) || !OpenClipboard(hwnd)) return;
  HANDLE memory = GetClipboardData(13 /* CF_UNICODETEXT */);
  const WCHAR *src = memory ? GlobalLock(memory) : NULL;
  if (src) {
    UINT n = 0;
    const BOOL multiline = (ctl_style(hwnd) & ES_MULTILINE_) != 0;
    while (src[n] && (multiline || (src[n] != L'\r' && src[n] != L'\n'))) ++n;
    const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
    edit_replace(hwnd, st, a, b, src, n, TRUE);
    GlobalUnlock(memory);
  }
  CloseClipboard();
}

static LONG_PTR WINAPI edit_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  EditState *st = message == 0x0081 ? NULL : ctl_state(hwnd);
  const LONG style = ctl_style(hwnd);
  if (!st && message != 0x0081 && message != 0x0082) return DefWindowProcW(hwnd, message, wparam, lparam);
  switch (message) {
    case 0x0081:      /* WM_NCCREATE */
      if (!(st = ctl_alloc(hwnd, sizeof(EditState)))) return FALSE;
      st->parent = (HANDLE)((CreateStruct_ *)lparam)->parent;
      st->limit = EDIT_DEFAULT_LIMIT;
      st->readonly = (style & ES_READONLY_) != 0;
      st->password = L'*';
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0082:      /* WM_NCDESTROY */
      if (st) {
        if (st->text) HeapFree(GetProcessHeap(), 0, st->text);
        ctl_free(hwnd);
        DefWindowProcW(hwnd, 0x000C, 0, (LONG_PTR)L"");      /* (the text goes with the control) */
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0001: {      /* WM_CREATE: the title is the text */
      const CreateStruct_ *cs = (const CreateStruct_ *)lparam;
      const WCHAR *name = cs->name;
      if (name && ((ULONG_PTR)name >> 16) != 0) { UINT n = 0; while (name[n]) ++n; edit_replace(hwnd, st, 0, 0, name, n, FALSE); st->modified = FALSE; st->sel_start = st->sel_end = 0; }
      return 0;
    }
    case 0x000C: {      /* WM_SETTEXT */
      const WCHAR *s = (const WCHAR *)lparam;
      UINT n = 0;
      while (s && s[n]) ++n;
      st->length = 0;
      edit_replace(hwnd, st, 0, 0, s ? s : L"", n, FALSE);
      st->modified = FALSE;
      st->sel_start = st->sel_end = 0;
      ctl_notify(hwnd, EN_UPDATE_);
      ctl_notify(hwnd, EN_CHANGE_);
      return TRUE;
    }
    case 0x000D: {      /* WM_GETTEXT */
      WCHAR *out = (WCHAR *)lparam;
      if (!out || !wparam) return 0;
      UINT n = st->length < wparam - 1 ? st->length : (UINT)wparam - 1;
      for (UINT i = 0; i < n; ++i) out[i] = st->text[i];
      out[n] = 0;
      return n;
    }
    case 0x000E: return st->length;      /* WM_GETTEXTLENGTH */
    case 0x0014: return 1;
    case 0x000F: case 0x0318: {
      PAINTSTRUCT_ ps;
      HANDLE hdc = message == 0x0318 ? (HANDLE)wparam : BeginPaint(hwnd, &ps);
      if (hdc) { edit_paint(hwnd, hdc, st); if (message == 0x000F) EndPaint(hwnd, &ps); }
      return 0;
    }
    case 0x000A: ctl_repaint(hwnd); return 0;
    case 0x0030: st->font = (HANDLE)wparam; if (lparam) ctl_repaint(hwnd); return 0;
    case 0x0031: return (LONG_PTR)st->font;
    case 0x0087: {      /* WM_GETDLGCODE */
      LONG_PTR code = DLGC_WANTCHARS_ | DLGC_WANTARROWS_ | 0x8 /* DLGC_HASSETSEL */;
      if (style & ES_MULTILINE_) code |= 0x4 /* DLGC_WANTALLKEYS */;
      return code;
    }
    case 0x0007: ctl_repaint(hwnd); ctl_notify(hwnd, EN_SETFOCUS_); return 0;
    case 0x0008: ctl_repaint(hwnd); ctl_notify(hwnd, EN_KILLFOCUS_); return 0;
    case 0x0201:      /* WM_LBUTTONDOWN */
      SetFocus(hwnd);
      SetCapture(hwnd);
      st->sel_start = st->sel_end = edit_index_at(hwnd, st, (short)(lparam & 0xFFFF), (short)((lparam >> 16) & 0xFFFF));
      ctl_repaint(hwnd);
      return 0;
    case 0x0200:      /* WM_MOUSEMOVE: with the button down the selection grows */
      if ((wparam & 1) && GetCapture() == hwnd) { st->sel_end = edit_index_at(hwnd, st, (short)(lparam & 0xFFFF), (short)((lparam >> 16) & 0xFFFF)); ctl_repaint(hwnd); }
      return 0;
    case 0x0202: if (GetCapture() == hwnd) ReleaseCapture(); return 0;
    case 0x0102: {      /* WM_CHAR */
      WCHAR c = (WCHAR)wparam;
      if (c == 3) { edit_copy(hwnd, st); return 0; }      /* (copy and select all work in a read-only box) */
      if (c == 1) { edit_set_sel(hwnd, st, 0, -1); return 0; }
      if (st->readonly || (style & WS_DISABLED_)) return 0;
      const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
      if (c == 8) {      /* backspace */
        if (a != b) edit_replace(hwnd, st, a, b, NULL, 0, TRUE);
        else if (a > 0) {
          UINT from = a - 1;
          if (from > 0 && st->text[from] == L'\n' && st->text[from - 1] == L'\r') --from;
          edit_replace(hwnd, st, from, a, NULL, 0, TRUE);
        }
        return 0;
      }
      if (c == L'\r' || c == L'\n') {
        if (!(style & ES_MULTILINE_) || (c == L'\n' && 0)) return 0;
        edit_replace(hwnd, st, a, b, L"\r\n", 2, TRUE);
        return 0;
      }
      if (c == 3) { edit_copy(hwnd, st); return 0; }      /* Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+A */
      if (c == 24) { SendMessageW(hwnd, 0x0300, 0, 0); return 0; }
      if (c == 22) { edit_paste(hwnd, st); return 0; }
      if (c == 1) { edit_set_sel(hwnd, st, 0, -1); return 0; }
      if (c == L'\t' && !(style & ES_MULTILINE_)) return 0;
      if (c < 0x20 && c != L'\t') return 0;
      if ((style & ES_NUMBER_) && (c < L'0' || c > L'9')) return 0;
      edit_replace(hwnd, st, a, b, &c, 1, TRUE);
      return 0;
    }
    case 0x0100: {      /* WM_KEYDOWN: movement and delete */
      const BOOL shift = (GetKeyState(0x10) & 0x8000) != 0;
      UINT pos = st->sel_end;
      BOOL moved = TRUE;
      switch (wparam) {
        case 0x25: if (pos > 0) { --pos; if (pos > 0 && st->text[pos] == L'\n' && st->text[pos - 1] == L'\r') --pos; } break;      /* VK_LEFT */
        case 0x27: if (pos < st->length) { ++pos; if (pos < st->length && st->text[pos - 1] == L'\r' && st->text[pos] == L'\n') ++pos; } break;      /* VK_RIGHT */
        case 0x24: { UINT line = edit_line_of(st, style, pos), start = 0; edit_line_start(st, style, line, &start); pos = start; break; }      /* VK_HOME */
        case 0x23: { UINT line = edit_line_of(st, style, pos), start = 0; edit_line_start(st, style, line, &start); pos = start + edit_line_length_at(st, style, start); break; }      /* VK_END */
        case 0x26: case 0x28: {      /* VK_UP, VK_DOWN */
          if (!(style & ES_MULTILINE_)) { moved = FALSE; break; }
          UINT line = edit_line_of(st, style, pos), start = 0, column;
          edit_line_start(st, style, line, &start);
          column = pos - start;
          if (wparam == 0x26 ? line == 0 : line + 1 >= edit_line_count(st, style)) { moved = FALSE; break; }
          line = wparam == 0x26 ? line - 1 : line + 1;
          edit_line_start(st, style, line, &start);
          const UINT len = edit_line_length_at(st, style, start);
          pos = start + (column < len ? column : len);
          break;
        }
        case 0x2E: {      /* VK_DELETE */
          if (st->readonly) return 0;
          const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
          if (a != b) edit_replace(hwnd, st, a, b, NULL, 0, TRUE);
          else if (a < st->length) edit_replace(hwnd, st, a, a + ((st->text[a] == L'\r' && a + 1 < st->length) ? 2 : 1), NULL, 0, TRUE);
          return 0;
        }
        default: moved = FALSE; break;
      }
      if (moved) { st->sel_end = pos; if (!shift) st->sel_start = pos; ctl_repaint(hwnd); return 0; }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case 0x0301: edit_copy(hwnd, st); return 0;      /* WM_COPY */
    case 0x0300: if (!st->readonly) { if (edit_copy(hwnd, st)) { const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start; edit_replace(hwnd, st, a, b, NULL, 0, TRUE); } } return 0;      /* WM_CUT */
    case 0x0302: edit_paste(hwnd, st); return 0;      /* WM_PASTE */
    case 0x0303: if (st->readonly) return 0; { const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start; if (a != b) edit_replace(hwnd, st, a, b, NULL, 0, TRUE); } return 0;      /* WM_CLEAR */
    case 0x00B0: {      /* EM_GETSEL */
      const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
      if (wparam) *(DWORD *)wparam = a;
      if (lparam) *(DWORD *)lparam = b;
      return (LONG_PTR)((b << 16) | (a & 0xFFFF));
    }
    case 0x00B1: edit_set_sel(hwnd, st, (LONG)wparam, (LONG)lparam); return 0;      /* EM_SETSEL */
    case 0x00C2: {      /* EM_REPLACESEL */
      const WCHAR *s = (const WCHAR *)lparam;
      UINT n = 0;
      while (s && s[n]) ++n;
      const UINT a = st->sel_start < st->sel_end ? st->sel_start : st->sel_end, b = st->sel_start < st->sel_end ? st->sel_end : st->sel_start;
      edit_replace(hwnd, st, a, b, s ? s : L"", n, TRUE);
      return 0;
    }
    case 0x00BA: return edit_line_count(st, style);      /* EM_GETLINECOUNT */
    case 0x00BB: {      /* EM_LINEINDEX */
      UINT line = wparam == (ULONG_PTR)-1 ? edit_line_of(st, style, st->sel_end) : (UINT)wparam, start = 0;
      return edit_line_start(st, style, line, &start) ? (LONG_PTR)start : -1;
    }
    case 0x00C1: {      /* EM_LINELENGTH: of the line the character is in (-1: of the selection's line) */
      UINT index = wparam == (ULONG_PTR)-1 ? st->sel_start : (UINT)wparam, start = 0;
      if (index > st->length) return 0;
      edit_line_start(st, style, edit_line_of(st, style, index), &start);
      return edit_line_length_at(st, style, start);
    }
    case 0x00C9: return edit_line_of(st, style, wparam == (ULONG_PTR)-1 ? st->sel_start : (UINT)wparam);      /* EM_LINEFROMCHAR */
    case 0x00C4: {      /* EM_GETLINE: the first word of the buffer is its size */
      UINT start = 0;
      WCHAR *buffer = (WCHAR *)lparam;
      if (!buffer || !edit_line_start(st, style, (UINT)wparam, &start)) return 0;
      const UINT n = edit_line_length_at(st, style, start), room = *(WORD *)buffer;
      const UINT copy = n < room ? n : room;
      for (UINT i = 0; i < copy; ++i) buffer[i] = st->text[start + i];
      if (copy < room) buffer[copy] = 0;
      return copy;
    }
    case 0x00C5: case 0x00B5: st->limit = wparam ? (UINT)wparam : 0x7FFFFFFE; return 0;      /* EM_SETLIMITTEXT */
    case 0x00D5: return st->limit;      /* EM_GETLIMITTEXT */
    case 0x00CF: st->readonly = wparam != 0; SetWindowLongW(hwnd, -16, wparam ? (style | ES_READONLY_) : (style & ~ES_READONLY_)); ctl_repaint(hwnd); return 1;      /* EM_SETREADONLY */
    case 0x00B8: return st->modified;      /* EM_GETMODIFY */
    case 0x00B9: st->modified = wparam != 0; return 0;      /* EM_SETMODIFY */
    case 0x00CC: st->password = (WCHAR)wparam; ctl_repaint(hwnd); return 0;      /* EM_SETPASSWORDCHAR */
    case 0x00D2: return (style & ES_PASSWORD_) ? st->password : 0;      /* EM_GETPASSWORDCHAR */
    case 0x00D3: {      /* EM_SETMARGINS */
      if (wparam & 1) st->margins = (st->margins & 0xFFFF0000u) | (UINT)(lparam & 0xFFFF);
      if (wparam & 2) st->margins = (st->margins & 0xFFFFu) | ((UINT)lparam & 0xFFFF0000u);
      ctl_repaint(hwnd);
      return 0;
    }
    case 0x00D4: return (LONG_PTR)st->margins;      /* EM_GETMARGINS */
    case 0x00B6: case 0x00B7: return 0;      /* EM_LINESCROLL, EM_SCROLLCARET */
    case 0x00CE: return 0;      /* EM_GETFIRSTVISIBLELINE */
    case 0x00C6: case 0x00C7: return 0;      /* EM_CANUNDO, EM_UNDO */
    case 0x00CD: return 0;      /* EM_EMPTYUNDOBUFFER */
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
