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

/* The "ListBox" class (included by window.inc after controls.inc): a list of strings (or owner-drawn items) with item data, single, multiple and extended selection, sorting, the LB_*
 * messages, LBN_* notifications, keyboard and mouse selection, and plain painting. Not done: scroll bar windows (the top item follows the selection and LB_SETTOPINDEX / the mouse wheel),
 * multi-column layout, tab stops, LB_DIR / LB_ADDFILE, owner-draw painting beyond WM_DRAWITEM (sent, not drawn by the control), LBS_NODATA content. */
#define LBS_NOTIFY_ 0x1
#define LBS_SORT_ 0x2
#define LBS_NOREDRAW_ 0x4
#define LBS_MULTIPLESEL_ 0x8
#define LBS_OWNERDRAWFIXED_ 0x10
#define LBS_OWNERDRAWVARIABLE_ 0x20
#define LBS_HASSTRINGS_ 0x40
#define LBS_NOINTEGRALHEIGHT_ 0x100
#define LBS_EXTENDEDSEL_ 0x800
#define LBS_NODATA_ 0x2000
#define LBS_NOSEL_ 0x4000
#define LB_ERR_ (-1)
#define LB_ERRSPACE_ (-2)
#define LBN_SELCHANGE_ 1
#define LBN_DBLCLK_ 2
#define LBN_SELCANCEL_ 3
#define LBN_SETFOCUS_ 4
#define LBN_KILLFOCUS_ 5
enum { LB_ADDSTRING_ = 0x180, LB_INSERTSTRING_, LB_DELETESTRING_, LB_SELITEMRANGEEX_, LB_RESETCONTENT_, LB_SETSEL_, LB_SETCURSEL_, LB_GETSEL_, LB_GETCURSEL_, LB_GETTEXT_, LB_GETTEXTLEN_, LB_GETCOUNT_,
       LB_SELECTSTRING_, LB_DIR_, LB_GETTOPINDEX_, LB_FINDSTRING_, LB_GETSELCOUNT_, LB_GETSELITEMS_, LB_SETTABSTOPS_, LB_GETHORIZONTALEXTENT_, LB_SETHORIZONTALEXTENT_, LB_SETCOLUMNWIDTH_, LB_ADDFILE_,
       LB_SETTOPINDEX_, LB_GETITEMRECT_, LB_GETITEMDATA_, LB_SETITEMDATA_, LB_SELITEMRANGE_, LB_SETANCHORINDEX_, LB_GETANCHORINDEX_, LB_SETCARETINDEX_, LB_GETCARETINDEX_, LB_SETITEMHEIGHT_,
       LB_GETITEMHEIGHT_, LB_FINDSTRINGEXACT_, LB_SETLOCALE_ = 0x1A5, LB_GETLOCALE_, LB_SETCOUNT_, LB_INITSTORAGE_, LB_ITEMFROMPOINT_, LB_GETLISTBOXINFO_ = 0x1B2 };
typedef struct { WCHAR *text; ULONG_PTR data; BOOL selected; int height; } ListItem;
typedef struct { HANDLE parent; ListItem *items; int count, capacity; int cur_sel, caret, anchor, top, item_height, horizontal_extent; BOOL redraw; HANDLE font; UINT locale; BOOL focused; LONG sel_style; } ListState;

static int list_ws_len(const WCHAR *s) { int n = 0; while (s && s[n]) ++n; return n; }
static WCHAR list_lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static int list_compare(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) { const WCHAR x = list_lower(*a), y = list_lower(*b); if (x != y) return x < y ? -1 : 1; if (!x) return 0; }
}
static int list_prefix_match(const WCHAR *text, const WCHAR *prefix) { for (; *prefix; ++text, ++prefix) if (list_lower(*text) != list_lower(*prefix)) return 0; return 1; }
static WCHAR *list_dup(const WCHAR *s) {
  const int n = list_ws_len(s);
  WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)n + 1) * 2);
  if (copy) for (int i = 0; i <= n; ++i) copy[i] = s[i];
  return copy;
}
static BOOL list_owner_without_strings(LONG style) { return (style & (LBS_OWNERDRAWFIXED_ | LBS_OWNERDRAWVARIABLE_)) && !(style & LBS_HASSTRINGS_); }
static int list_visible_count(HANDLE hwnd, ListState *st) {
  RECT_ r;
  GetClientRect(hwnd, &r);
  const int n = st->item_height > 0 ? (r.bottom - r.top) / st->item_height : 0;
  return n > 0 ? n : 1;
}
static void list_scroll_into_view(HANDLE hwnd, ListState *st, int index) {
  const int visible = list_visible_count(hwnd, st);
  if (index < st->top) st->top = index;
  else if (index >= st->top + visible) st->top = index - visible + 1;
  if (st->top < 0) st->top = 0;
}
static void list_redraw(HANDLE hwnd, ListState *st) { if (st->redraw) ctl_repaint(hwnd); }
static BOOL list_grow(ListState *st) {
  if (st->count < st->capacity) return TRUE;
  const int capacity = st->capacity ? st->capacity * 2 : 16;
  ListItem *bigger = HeapAlloc(GetProcessHeap(), 8, (SIZE_T)capacity * sizeof(ListItem));
  if (!bigger) return FALSE;
  for (int i = 0; i < st->count; ++i) bigger[i] = st->items[i];
  if (st->items) HeapFree(GetProcessHeap(), 0, st->items);
  st->items = bigger;
  st->capacity = capacity;
  return TRUE;
}
static void list_clear(ListState *st) {
  for (int i = 0; i < st->count; ++i) if (st->items[i].text) HeapFree(GetProcessHeap(), 0, st->items[i].text);
  st->count = 0;
  st->cur_sel = st->caret = st->anchor = -1;
  st->top = 0;
}
/* Inserts an item at `index` (-1 appends; with LBS_SORT the place is found by the text). Answers the index or LB_ERR(SPACE). */
static int list_insert(HANDLE hwnd, ListState *st, int index, const WCHAR *text, ULONG_PTR data) {
  const LONG style = ctl_style(hwnd);
  const BOOL strings = !list_owner_without_strings(style);
  if (index < -1 || index > st->count) return LB_ERR_;
  if ((style & LBS_SORT_) && strings && text) { index = 0; while (index < st->count && list_compare(st->items[index].text, text) <= 0) ++index; }
  else if (index == -1) index = st->count;
  if (!list_grow(st)) return LB_ERRSPACE_;
  for (int i = st->count; i > index; --i) st->items[i] = st->items[i - 1];
  ListItem item = {NULL, 0, FALSE, st->item_height};
  if (strings) { item.text = list_dup(text ? text : L""); if (!item.text) return LB_ERRSPACE_; }
  else item.data = data;
  if (strings) item.data = 0;
  st->items[index] = item;
  ++st->count;
  if (st->cur_sel >= index) ++st->cur_sel;
  if (st->caret >= index) ++st->caret;
  list_redraw(hwnd, st);
  return index;
}
static void list_notify(HANDLE hwnd, UINT code) { if (ctl_style(hwnd) & LBS_NOTIFY_) ctl_notify(hwnd, code); }
/* The selection by the keyboard or mouse: single selection moves, multiple selection toggles, extended selection selects a range from the anchor. */
static void list_select_user(HANDLE hwnd, ListState *st, int index, BOOL toggle, BOOL extend) {
  const LONG style = ctl_style(hwnd);
  if (index < 0 || index >= st->count || (style & LBS_NOSEL_)) return;
  if (st->sel_style & LBS_EXTENDEDSEL_) {
    if (extend && st->anchor >= 0) {
      const int a = st->anchor < index ? st->anchor : index, b = st->anchor < index ? index : st->anchor;
      for (int i = 0; i < st->count; ++i) st->items[i].selected = i >= a && i <= b;
    } else if (toggle) { st->items[index].selected = !st->items[index].selected; st->anchor = index; }
    else { for (int i = 0; i < st->count; ++i) st->items[i].selected = i == index; st->anchor = index; }
  } else if (st->sel_style & LBS_MULTIPLESEL_) { st->items[index].selected = !st->items[index].selected; st->anchor = index; }
  else { st->cur_sel = index; st->anchor = index; }
  st->caret = index;
  list_scroll_into_view(hwnd, st, index);
  list_redraw(hwnd, st);
  list_notify(hwnd, LBN_SELCHANGE_);
}
static int list_find(ListState *st, int start, const WCHAR *text, BOOL exact) {
  if (st->count == 0) return LB_ERR_;
  for (int n = 0; n < st->count; ++n) {
    const int i = (start + 1 + n) % st->count;
    if (!st->items[i].text) continue;
    if (exact ? list_compare(st->items[i].text, text) == 0 : list_prefix_match(st->items[i].text, text)) return i;
  }
  return LB_ERR_;
}
static void list_paint(HANDLE hwnd, HANDLE hdc, ListState *st) {
  RECT_ r;
  const LONG style = ctl_style(hwnd);
  GetClientRect(hwnd, &r);
  const int saved = SaveDC(hdc);
  ctl_clip(hwnd, hdc);
  ctl_select_font(hdc, st->font);
  HANDLE brush = ctl_color(hwnd, hdc, 0x0134 /* WM_CTLCOLORLISTBOX */);
  FillRect(hdc, &r, brush);
  SetBkMode(hdc, 1);
  for (int i = st->top; i < st->count; ++i) {
    RECT_ ir = {r.left, r.top + (i - st->top) * st->item_height, r.right, r.top + (i - st->top + 1) * st->item_height};
    if (ir.top >= r.bottom) break;
    const BOOL selected = (st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_)) ? st->items[i].selected : i == st->cur_sel;
    if (selected) { FillRect(hdc, &ir, GetSysColorBrush(13 /* COLOR_HIGHLIGHT */)); SetTextColor(hdc, GetSysColor(14 /* COLOR_HIGHLIGHTTEXT */)); }
    else SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT_));
    if (st->items[i].text) { RECT_ tr = {ir.left + 2, ir.top, ir.right, ir.bottom}; ctl_text(hdc, st->items[i].text, &tr, DT_VCENTER_ | DT_SINGLELINE_ | DT_NOPREFIX_, NULL); }
    if (st->focused && i == st->caret) DrawFocusRect(hdc, &ir);
  }
  RestoreDC(hdc, saved);
}
static int list_item_from_point(HANDLE hwnd, ListState *st, int x, int y, BOOL *outside) {
  RECT_ r;
  GetClientRect(hwnd, &r);
  int index = st->top + (y >= 0 && st->item_height > 0 ? y / st->item_height : -1);
  *outside = x < r.left || x >= r.right || y < r.top || y >= r.bottom || index < 0 || index >= st->count;
  if (index >= st->count) index = st->count - 1;
  if (index < 0) index = 0;
  return index;
}

static LONG_PTR WINAPI listbox_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  ListState *st = message == 0x0081 ? NULL : ctl_state(hwnd);
  const LONG style = ctl_style(hwnd);
  const int index = (int)wparam;
  if (!st && message != 0x0081 && message != 0x0082) return DefWindowProcW(hwnd, message, wparam, lparam);
  switch (message) {
    case 0x0081:
      if (!(st = ctl_alloc(hwnd, sizeof(ListState)))) return FALSE;
      st->parent = (HANDLE)((CreateStruct_ *)lparam)->parent;
      st->cur_sel = st->caret = st->anchor = -1;
      st->item_height = 16;
      st->redraw = TRUE;
      st->locale = 0x0409;
      st->sel_style = style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_);      /* (the selection mode is the one the box was made with) */
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0001: {      /* WM_CREATE: without LBS_NOINTEGRALHEIGHT the height shows whole items */
      if (!(style & (LBS_NOINTEGRALHEIGHT_ | LBS_OWNERDRAWVARIABLE_))) {
        RECT_ c, w;
        GetClientRect(hwnd, &c);
        GetWindowRect(hwnd, &w);
        const int extra = c.bottom % st->item_height;
        if (extra) SetWindowPos(hwnd, NULL, 0, 0, w.right - w.left, w.bottom - w.top - extra, 0x16 /* SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE */);
      }
      return 0;
    }
    case 0x0082: list_clear(st); if (st->items) HeapFree(GetProcessHeap(), 0, st->items); ctl_free(hwnd); return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0014: return 1;
    case 0x000F: case 0x0318: {
      PAINTSTRUCT_ ps;
      HANDLE hdc = message == 0x0318 ? (HANDLE)wparam : BeginPaint(hwnd, &ps);
      if (hdc) { list_paint(hwnd, hdc, st); if (message == 0x000F) EndPaint(hwnd, &ps); }
      return 0;
    }
    case 0x000A: ctl_repaint(hwnd); return 0;
    case 0x000B: st->redraw = wparam != 0; if (st->redraw) ctl_repaint(hwnd); return 0;      /* WM_SETREDRAW */
    case 0x0030: st->font = (HANDLE)wparam; if (lparam) ctl_repaint(hwnd); return 0;
    case 0x0031: return (LONG_PTR)st->font;
    case 0x0087: return DLGC_WANTARROWS_ | DLGC_WANTCHARS_;
    case 0x0007: st->focused = TRUE; ctl_repaint(hwnd); list_notify(hwnd, LBN_SETFOCUS_); return 0;
    case 0x0008: st->focused = FALSE; ctl_repaint(hwnd); list_notify(hwnd, LBN_KILLFOCUS_); return 0;
    case 0x0201: case 0x0203: {      /* WM_LBUTTONDOWN, WM_LBUTTONDBLCLK */
      BOOL outside;
      SetFocus(hwnd);
      if (st->count == 0) return 0;
      const int hit = list_item_from_point(hwnd, st, (short)(lparam & 0xFFFF), (short)((lparam >> 16) & 0xFFFF), &outside);
      if (outside) return 0;
      if (message == 0x0203) { list_notify(hwnd, LBN_DBLCLK_); return 0; }
      list_select_user(hwnd, st, hit, (wparam & 0x8) != 0 /* MK_CONTROL */, (wparam & 0x4) != 0 /* MK_SHIFT */);
      return 0;
    }
    case 0x0100: {      /* WM_KEYDOWN */
      int target = st->caret < 0 ? 0 : st->caret;
      switch (wparam) {
        case 0x26: --target; break;
        case 0x28: ++target; break;
        case 0x21: target -= list_visible_count(hwnd, st) - 1; break;
        case 0x22: target += list_visible_count(hwnd, st) - 1; break;
        case 0x24: target = 0; break;
        case 0x23: target = st->count - 1; break;
        default: return DefWindowProcW(hwnd, message, wparam, lparam);
      }
      if (target < 0) target = 0;
      if (target >= st->count) target = st->count - 1;
      if (st->count) list_select_user(hwnd, st, target, FALSE, (GetKeyState(0x10) & 0x8000) != 0);
      return 0;
    }
    case 0x0102: {      /* WM_CHAR: a typed character selects the next item that starts with it */
      WCHAR prefix[2] = {(WCHAR)wparam, 0};
      if (wparam < 0x20 || (style & LBS_HASSTRINGS_) == 0 && list_owner_without_strings(style)) return 0;
      const int found = list_find(st, st->caret, prefix, FALSE);
      if (found >= 0) list_select_user(hwnd, st, found, FALSE, FALSE);
      return 0;
    }
    case 0x020A: {      /* WM_MOUSEWHEEL: three items per notch */
      const int notches = (short)((wparam >> 16) & 0xFFFF) / 120;
      st->top -= notches * 3;
      if (st->top > st->count - 1) st->top = st->count - 1;
      if (st->top < 0) st->top = 0;
      ctl_repaint(hwnd);
      return 0;
    }
    case LB_ADDSTRING_: return list_insert(hwnd, st, -1, (const WCHAR *)lparam, (ULONG_PTR)lparam);
    case LB_INSERTSTRING_: return list_insert(hwnd, st, (int)wparam, (const WCHAR *)lparam, (ULONG_PTR)lparam);
    case LB_DELETESTRING_: {
      if (index < 0 || index >= st->count) return LB_ERR_;
      if (st->items[index].text) HeapFree(GetProcessHeap(), 0, st->items[index].text);
      for (int i = index; i + 1 < st->count; ++i) st->items[i] = st->items[i + 1];
      --st->count;
      if (st->cur_sel == index) st->cur_sel = -1; else if (st->cur_sel > index) --st->cur_sel;
      if (st->caret >= st->count) st->caret = st->count - 1;
      if (st->top > 0 && st->top >= st->count) st->top = st->count - 1 < 0 ? 0 : st->count - 1;
      list_redraw(hwnd, st);
      return st->count;
    }
    case LB_RESETCONTENT_: list_clear(st); list_redraw(hwnd, st); return 0;
    case LB_GETCOUNT_: return st->count;
    case LB_GETCURSEL_: return (st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_)) ? st->caret : st->cur_sel;
    case LB_SETCURSEL_:
      if (st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_)) return LB_ERR_;
      if (index < -1 || index >= st->count) return LB_ERR_;
      st->cur_sel = index;
      if (index >= 0) { st->caret = index; list_scroll_into_view(hwnd, st, index); }
      list_redraw(hwnd, st);
      return index;
    case LB_GETSEL_: if (index < 0 || index >= st->count) return LB_ERR_; return (st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_)) ? st->items[index].selected : index == st->cur_sel;
    case LB_SETSEL_: {
      if (!(st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_))) return LB_ERR_;
      if ((int)lparam == -1) { for (int i = 0; i < st->count; ++i) st->items[i].selected = wparam != 0; }
      else if ((int)lparam < 0 || (int)lparam >= st->count) return LB_ERR_;
      else st->items[(int)lparam].selected = wparam != 0;
      list_redraw(hwnd, st);
      return 0;
    }
    case LB_GETSELCOUNT_: { if (!(st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_))) return LB_ERR_; int n = 0; for (int i = 0; i < st->count; ++i) n += st->items[i].selected; return n; }
    case LB_GETSELITEMS_: {
      int n = 0;
      int *out = (int *)lparam;
      if (!(st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_))) return LB_ERR_;
      for (int i = 0; i < st->count && n < index; ++i) if (st->items[i].selected) out[n++] = i;
      return n;
    }
    case LB_SELITEMRANGE_: case LB_SELITEMRANGEEX_: {
      int a = message == LB_SELITEMRANGE_ ? (int)(lparam & 0xFFFF) : (int)wparam, b = message == LB_SELITEMRANGE_ ? (int)((lparam >> 16) & 0xFFFF) : (int)lparam;
      BOOL on = message == LB_SELITEMRANGE_ ? wparam != 0 : TRUE;
      if (message == LB_SELITEMRANGEEX_ && a > b) { const int x = a; a = b; b = x; on = FALSE; }
      if (!(st->sel_style & (LBS_MULTIPLESEL_ | LBS_EXTENDEDSEL_))) return LB_ERR_;
      for (int i = a < 0 ? 0 : a; i <= b && i < st->count; ++i) st->items[i].selected = on;
      list_redraw(hwnd, st);
      return 0;
    }
    case LB_GETTEXTLEN_: return index >= 0 && index < st->count ? (st->items[index].text ? list_ws_len(st->items[index].text) : (int)sizeof(ULONG_PTR)) : LB_ERR_;
    case LB_GETTEXT_: {
      if (index < 0 || index >= st->count || !lparam) return LB_ERR_;
      if (!st->items[index].text) { *(ULONG_PTR *)lparam = st->items[index].data; return (LONG_PTR)sizeof(ULONG_PTR); }
      const int n = list_ws_len(st->items[index].text);
      WCHAR *out = (WCHAR *)lparam;
      for (int i = 0; i <= n; ++i) out[i] = st->items[index].text[i];
      return n;
    }
    case LB_GETITEMDATA_: return index >= 0 && index < st->count ? (LONG_PTR)st->items[index].data : LB_ERR_;
    case LB_SETITEMDATA_: if (index < 0 || index >= st->count) return LB_ERR_; st->items[index].data = (ULONG_PTR)lparam; return TRUE;
    case LB_FINDSTRING_: case LB_FINDSTRINGEXACT_: return list_find(st, (int)wparam, (const WCHAR *)lparam, message == LB_FINDSTRINGEXACT_);
    case LB_SELECTSTRING_: {
      const int found = list_find(st, (int)wparam, (const WCHAR *)lparam, FALSE);
      if (found >= 0) { st->cur_sel = found; st->caret = found; list_scroll_into_view(hwnd, st, found); list_redraw(hwnd, st); }
      return found;
    }
    case LB_GETTOPINDEX_: return st->top;
    case LB_SETTOPINDEX_: if (index < 0 || index >= st->count) return LB_ERR_; st->top = index; list_redraw(hwnd, st); return 0;
    case LB_GETITEMRECT_: {
      RECT_ *out = (RECT_ *)lparam, c;
      if (index < 0 || index >= st->count || !out) return LB_ERR_;
      GetClientRect(hwnd, &c);
      out->left = c.left; out->right = c.right;
      out->top = c.top + (index - st->top) * st->item_height;
      out->bottom = out->top + st->item_height;
      return 0;
    }
    case LB_SETITEMHEIGHT_: if (!(style & LBS_OWNERDRAWVARIABLE_)) { if ((int)lparam <= 0 || (int)lparam > 255) return LB_ERR_; st->item_height = (int)lparam; for (int i = 0; i < st->count; ++i) st->items[i].height = (int)lparam; ctl_repaint(hwnd); return 0; }
      if (index < 0 || index >= st->count || (int)lparam <= 0 || (int)lparam > 255) return LB_ERR_;
      st->items[index].height = (int)lparam; ctl_repaint(hwnd); return 0;
    case LB_GETITEMHEIGHT_: if (!(style & LBS_OWNERDRAWVARIABLE_)) return st->item_height; return index >= 0 && index < st->count ? st->items[index].height : LB_ERR_;
    case LB_GETCARETINDEX_: return st->caret < 0 ? 0 : st->caret;
    case LB_SETCARETINDEX_: if (index < 0 || index >= st->count) return LB_ERR_; st->caret = index; list_scroll_into_view(hwnd, st, index); list_redraw(hwnd, st); return 0;
    case LB_GETANCHORINDEX_: return st->anchor;
    case LB_SETANCHORINDEX_: if (index < -1 || index >= st->count) return LB_ERR_; st->anchor = index; return 0;
    case LB_GETHORIZONTALEXTENT_: return st->horizontal_extent;
    case LB_SETHORIZONTALEXTENT_: st->horizontal_extent = index; return 0;
    case LB_SETCOLUMNWIDTH_: case LB_SETTABSTOPS_: return message == LB_SETTABSTOPS_ ? TRUE : 0;
    case LB_SETLOCALE_: { const UINT old = st->locale; st->locale = (UINT)wparam; return old; }
    case LB_GETLOCALE_: return st->locale;
    case LB_INITSTORAGE_: return LB_ERRSPACE_ < 0 ? (LONG_PTR)(st->capacity > index ? st->capacity : index) : 0;
    case LB_SETCOUNT_: return LB_ERR_;
    case LB_ITEMFROMPOINT_: {
      BOOL outside;
      const int hit = list_item_from_point(hwnd, st, (short)(lparam & 0xFFFF), (short)((lparam >> 16) & 0xFFFF), &outside);
      return (LONG_PTR)((outside ? 1 : 0) << 16 | (hit & 0xFFFF));
    }
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
