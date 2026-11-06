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

/* MDI of user32.dll (included by window.inc after the menus): the "MDIClient" window class, DefFrameProc, DefMDIChildProc, CreateMDIWindow, TileWindows, CascadeWindows and
 * ArrangeIconicWindows. The client keeps its state block (the frame, the active and the maximized child, the window menu and the list of children) in its private data. A child is
 * made by WM_MDICREATE (or CreateMDIWindow): a child window of the client with WS_EX_MDICHILD; the first ids from the client's CLIENTCREATESTRUCT number the entries of the window
 * menu. A maximized child fills the client area (its caption is not merged into the frame's menu bar). Not done: the A form of WM_MDICREATE sent with SendMessageA, the "More windows"
 * dialog, the system menu icon and buttons in the frame's menu bar, MDIS_ALLCHILDSTYLES beyond keeping the style bits, TranslateMDISysAccel. */

#define WM_MDICREATE_ 0x220
#define WM_MDIDESTROY_ 0x221
#define WM_MDIACTIVATE_ 0x222
#define WM_MDIRESTORE_ 0x223
#define WM_MDINEXT_ 0x224
#define WM_MDIMAXIMIZE_ 0x225
#define WM_MDITILE_ 0x226
#define WM_MDICASCADE_ 0x227
#define WM_MDIICONARRANGE_ 0x228
#define WM_MDIGETACTIVE_ 0x229
#define WM_MDISETMENU_ 0x230
#define WM_MDIREFRESHMENU_ 0x234
#define WM_CHILDACTIVATE_ 0x22
#define WM_NCDESTROY_ 0x82
#define WM_SETFOCUS_ 0x7
#define WM_SETTEXT_ 0xC
#define WM_COMMAND_ 0x111
#define SC_NEXTWINDOW_ 0xF040
#define SC_PREVWINDOW_ 0xF050
#define SC_MAXIMIZE_ 0xF030
#define SC_RESTORE_ 0xF120
#define SC_MINIMIZE_ 0xF020
#define SC_CLOSE_ 0xF060
#define MDITILE_HORIZONTAL_ 1
#define MDITILE_SKIPDISABLED_ 2
#define WS_EX_MDICHILD_ 0x40
#define WS_MINIMIZE_ 0x20000000
#define WS_MAXIMIZE_ 0x01000000
#define MDI_MAX 256
#ifndef LOWORD
#define LOWORD(x) ((WORD)((ULONG_PTR)(x) & 0xFFFF))
#define HIWORD(x) ((WORD)(((ULONG_PTR)(x) >> 16) & 0xFFFF))
#endif
#define CW_USEDEFAULT_ ((int)0x80000000)

typedef struct { HANDLE frame, active, window_menu, maximized, merged; UINT first_id; int count, cascade; HANDLE child[MDI_MAX]; RECT_ restore[MDI_MAX]; } MdiState;
typedef struct { POINT_ reserved, max_size, max_pos, min_track, max_track; } MINMAXINFO_;
typedef struct { const WCHAR *cls, *title; HANDLE owner; int x, y, cx, cy; DWORD style; LONG_PTR lparam; } MdiCreate_;
typedef struct { HANDLE window_menu; UINT first_id; } MdiClientCreate_;

EXPORT int WINAPI GetWindowTextW(HANDLE hwnd, LPWSTR buffer, int count);
EXPORT BOOL WINAPI MoveWindow(HANDLE hwnd, int x, int y, int cx, int cy, BOOL repaint);
EXPORT BOOL WINAPI AppendMenuW(HANDLE menu, UINT flags, ULONG_PTR id, LPCWSTR item);
EXPORT BOOL WINAPI InsertMenuW(HANDLE menu, UINT pos, UINT flags, ULONG_PTR id, LPCWSTR item);
EXPORT BOOL WINAPI RemoveMenu(HANDLE menu, UINT pos, UINT flags);
EXPORT UINT WINAPI GetMenuState(HANDLE menu, UINT pos, UINT flags);
EXPORT int WINAPI GetClassNameW(HANDLE hwnd, LPWSTR buffer, int count);
EXPORT DWORD WINAPI GetClassInfoExW(HANDLE instance, const WCHAR *name, WNDCLASSEX_ *wc);
EXPORT LONG_PTR WINAPI SetWindowLongPtrW(HANDLE hwnd, int index, LONG_PTR value);

static volatile LONG g_mdi_creating;
static BOOL is_mdi_client(HANDLE hwnd) {
  WCHAR name[16];
  static const WCHAR want[] = L"MDIClient";
  if (GetClassNameW(hwnd, name, 16) != 9) return FALSE;
  for (int i = 0; i < 9; ++i) if (name[i] != want[i]) return FALSE;
  return ctl_state(hwnd) != NULL;
}
/* The system icon and the minimize / restore / close buttons of a maximized child live in the frame's menu bar: four items, the buttons right-justified with magic bitmaps. */
static void mdi_merge(HANDLE client, MdiState *st, HANDLE child, BOOL on);
static int mdi_find(const MdiState *st, HANDLE child) { for (int i = 0; i < st->count; ++i) if (st->child[i] == child) return i; return -1; }
static HANDLE mdi_client_of(HANDLE child) { return GetParent(child); }
static MdiState *mdi_state(HANDLE client) { return IsWindow(client) ? ctl_state(client) : NULL; }

static void mdi_merge(HANDLE client, MdiState *st, HANDLE child, BOOL on) {
  (void)client;
  HANDLE menu = st->frame ? GetMenu(st->frame) : NULL;
  if (on) {
    if (st->merged == child) return;
    if (st->merged) mdi_merge(client, st, st->merged, FALSE);
    if (!menu) return;
    HANDLE sys = GetSystemMenu(child, FALSE);
    InsertMenuW(menu, 0, MF_BYPOSITION_ | MFT_BITMAP_ | (sys ? MF_POPUP_ : 0), sys ? (ULONG_PTR)sys : SC_RESTORE_, (LPCWSTR)(ULONG_PTR)1 /* HBMMENU_SYSTEM */);
    AppendMenuW(menu, MFT_BITMAP_ | 0x4000 /* MF_HELP */, SC_MINIMIZE_, (LPCWSTR)(ULONG_PTR)3 /* HBMMENU_MBAR_MINIMIZE */);
    AppendMenuW(menu, MFT_BITMAP_ | 0x4000, SC_RESTORE_, (LPCWSTR)(ULONG_PTR)2 /* HBMMENU_MBAR_RESTORE */);
    AppendMenuW(menu, MFT_BITMAP_ | 0x4000, SC_CLOSE_, (LPCWSTR)(ULONG_PTR)5 /* HBMMENU_MBAR_CLOSE */);
    st->merged = child;
  } else {
    if (st->merged != child) return;
    st->merged = NULL;
    if (!menu) return;
    RemoveMenu(menu, SC_MINIMIZE_, 0);
    RemoveMenu(menu, SC_RESTORE_, 0);
    RemoveMenu(menu, SC_CLOSE_, 0);
    if (GetMenuState(menu, 0, MF_BYPOSITION_) != (UINT)-1 && (GetMenuState(menu, 0, MF_BYPOSITION_) & MFT_BITMAP_)) RemoveMenu(menu, 0, MF_BYPOSITION_);
  }
  DrawMenuBar(st->frame);
}

/* the window list entries of the window menu: "&1 title" with the active child checked */
static void mdi_refresh_menu(HANDLE client, MdiState *st) {
  HANDLE menu = st->window_menu;
  if (!menu || !IsMenu(menu) || st->first_id == 0) return;
  for (UINT id = st->first_id; id < st->first_id + 9; ++id) DeleteMenu(menu, id, 0);
  if (st->count > 0) {
    const int n = GetMenuItemCount(menu);
    MII_ last = {sizeof last, MIIM_FTYPE_};
    if (n > 0 && GetMenuItemInfoW(menu, n - 1, TRUE, &last) && !(last.fType & MFT_SEPARATOR_)) AppendMenuW(menu, MF_SEPARATOR_, 0, NULL);
    for (int i = 0; i < st->count && i < 9; ++i) {
      WCHAR text[130];
      text[0] = (WCHAR)(L'1' + i); text[1] = L' ';
      GetWindowTextW(st->child[i], text + 2, 120);
      AppendMenuW(menu, st->child[i] == st->active ? 8 /* MF_CHECKED */ : 0, st->first_id + (UINT)i, text);
    }
  }
  if (st->frame) DrawMenuBar(st->frame);
  (void)client;
}

static void mdi_activate(HANDLE client, MdiState *st, HANDLE child) {
  if (child == st->active) return;
  HANDLE old = st->active;
  if (old && IsWindow(old)) SendMessageW(old, WM_MDIACTIVATE_, (ULONG_PTR)old, (LONG_PTR)child);
  st->active = child;
  if (child) {
    SetWindowPos(child, NULL, 0, 0, 0, 0, 0x2 | 0x1 | 0x10 /* NOMOVE | NOSIZE | NOACTIVATE */);
    SendMessageW(child, WM_MDIACTIVATE_, (ULONG_PTR)old, (LONG_PTR)child);
    SetFocus(child);
  }
  mdi_refresh_menu(client, st);
}
static HANDLE mdi_top_child(HANDLE client, MdiState *st, HANDLE skip) {
  for (HANDLE w = GetWindow(client, 5); w; w = GetWindow(w, 2))
    if (w != skip && mdi_find(st, w) >= 0 && (GetWindowLongW(w, -16) & 0x10000000 /* WS_VISIBLE */)) return w;
  return NULL;
}

static void mdi_client_rect(HANDLE client, RECT_ *rc) { GetClientRect(client, rc); }
/* where a maximized child goes: its client area is the client area of the MDI client, so its frame and caption lie outside */
static void mdi_max_rect(HANDLE client, HANDLE child, RECT_ *rc) {
  GetClientRect(client, rc);
  AdjustWindowRectEx(rc, (DWORD)GetWindowLongW(child, -16), FALSE, (DWORD)GetWindowLongW(child, -20));
}
static void mdi_max_rect(HANDLE client, HANDLE child, RECT_ *rc);
static void mdi_maximize(HANDLE client, MdiState *st, HANDLE child) {
  const int i = mdi_find(st, child);
  if (i < 0) return;
  if (st->maximized && st->maximized != child) {      /* only one is maximized */
    HANDLE other = st->maximized;
    const int j = mdi_find(st, other);
    st->maximized = NULL;
    if (j >= 0) { SetWindowLongW(other, -16, GetWindowLongW(other, -16) & ~WS_MAXIMIZE_); SetWindowPos(other, NULL, st->restore[j].left, st->restore[j].top, st->restore[j].right - st->restore[j].left, st->restore[j].bottom - st->restore[j].top, 0x4 | 0x10); }
  }
  if (st->maximized != child) {
    RECT_ wr;
    POINT_ p;
    GetWindowRect(child, &wr);
    p.x = wr.left; p.y = wr.top; ScreenToClient(client, &p);
    st->restore[i].left = p.x; st->restore[i].top = p.y; st->restore[i].right = p.x + (wr.right - wr.left); st->restore[i].bottom = p.y + (wr.bottom - wr.top);
  }
  RECT_ rc;
  SetWindowLongW(child, -16, GetWindowLongW(child, -16) | WS_MAXIMIZE_);
  mdi_max_rect(client, child, &rc);
  SetWindowPos(child, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0x4 | 0x10 /* NOZORDER | NOACTIVATE */);
  st->maximized = child;
  mdi_merge(client, st, child, TRUE);
}
static void mdi_restore(HANDLE client, MdiState *st, HANDLE child) {
  const int i = mdi_find(st, child);
  if (i < 0) return;
  if (st->maximized == child) {
    st->maximized = NULL;
    SetWindowLongW(child, -16, GetWindowLongW(child, -16) & ~WS_MAXIMIZE_);
    SetWindowPos(child, NULL, st->restore[i].left, st->restore[i].top, st->restore[i].right - st->restore[i].left, st->restore[i].bottom - st->restore[i].top, 0x4 | 0x10);
    mdi_merge(client, st, child, FALSE);
  } else if (GetWindowLongW(child, -16) & WS_MINIMIZE_) {
    ShowWindow(child, 9 /* SW_RESTORE */);
  }
  (void)client;
}

/* a child that is gone (WM_MDIDESTROY or its WM_NCDESTROY): out of the list, the next one active */
static void mdi_forget(HANDLE client, MdiState *st, HANDLE child) {
  const int i = mdi_find(st, child);
  if (i < 0) return;
  for (int k = i; k + 1 < st->count; ++k) { st->child[k] = st->child[k + 1]; st->restore[k] = st->restore[k + 1]; }
  --st->count;
  if (st->maximized == child) st->maximized = NULL;
  mdi_merge(client, st, child, FALSE);
  if (st->active == child) { st->active = NULL; HANDLE next = mdi_top_child(client, st, child); if (next) mdi_activate(client, st, next); }
  mdi_refresh_menu(client, st);
}

/* ---- arranging --------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* The windows TileWindows and CascadeWindows arrange, top of the z-order first: the given list or the children of `parent`; a window must be a visible child of the parent with a
 * caption and frame (WS_CAPTION), not minimized, and not disabled when asked to skip those. */
static int arrange_collect(HANDLE parent, HANDLE *out, int max, const HANDLE *given, int given_count, BOOL skip_disabled) {
  int n = 0;
  HANDLE w = given ? (given_count > 0 ? given[0] : NULL) : GetWindow(parent, 5 /* GW_CHILD */);
  for (int k = 0; w && n < max;) {
    const DWORD style = (DWORD)GetWindowLongW(w, -16);
    const BOOL eligible = IsWindow(w) && GetAncestor(w, 1 /* GA_PARENT */) == parent && (style & 0x10000000) && !(style & WS_MINIMIZE_) &&
                          (style & 0x00C00000) == 0x00C00000 && !(skip_disabled && (style & 0x08000000));
    if (eligible) out[n++] = w;
    if (given) { ++k; w = k < given_count ? given[k] : NULL; } else w = GetWindow(w, 2 /* GW_HWNDNEXT */);
  }
  return n;
}
/* Puts the windows in the area: a cascade starts with the last (the bottom one) at the corner and steps by the caption and frame, going back to the edge where the next would stick out;
 * a tile fills columns top to bottom, the first (topmost) window first. A window without a sizing frame is moved, not resized. */
static void arrange_place(HANDLE w, int x, int y, int cx, int cy) {
  const DWORD style = (DWORD)GetWindowLongW(w, -16);
  if (style & WS_MAXIMIZE_) ShowWindow(w, 9 /* SW_RESTORE */);
  if (style & 0x00040000 /* WS_THICKFRAME */) SetWindowPos(w, NULL, x, y, cx, cy, 0x4 | 0x10);
  else SetWindowPos(w, NULL, x, y, 0, 0, 0x1 | 0x4 | 0x10);
}
static void arrange_windows(const RECT_ *area, HANDLE *list, int n, BOOL cascade, BOOL horizontal) {
  const int width = area->right - area->left, height = area->bottom - area->top;
  if (n <= 0) return;
  if (cascade) {
    const int spacing = GetSystemMetrics(4 /* SM_CYCAPTION */) + GetSystemMetrics(8 /* SM_CYDLGFRAME */);
    const int cw = width * 3 / 4, ch = height * 3 / 4;
    int x = area->left, y = area->top;
    for (int i = n - 1; i >= 0; --i) {
      arrange_place(list[i], x, y, cw, ch);
      x += spacing; y += spacing;
      if (x + cw > area->right) x = area->left;
      if (y + ch > area->bottom) y = area->top;
    }
    return;
  }
  int a = 1;
  while ((a + 1) * (a + 1) <= n) ++a;
  const int b = n / a;
  const int columns = horizontal ? a : b, base = horizontal ? b : a, extra = n - base * columns;
  int next = 0;
  for (int c = 0; c < columns && next < n; ++c) {
    const int rows = base + (c >= columns - extra ? 1 : 0);
    int w = width / columns;
    if (w < GetSystemMetrics(28 /* SM_CXMIN */)) w = GetSystemMetrics(28);
    for (int r = 0; r < rows && next < n; ++r) arrange_place(list[next++], area->left + c * (width / columns), area->top + r * (height / rows), w, height / rows);
  }
}
EXPORT WORD WINAPI TileWindows(HANDLE parent, UINT how, const RECT_ *rect, UINT count, const HANDLE *kids) {
  HANDLE list[MDI_MAX];
  RECT_ area;
  if (!parent) parent = GetDesktopWindow();
  if (rect) area = *rect; else GetClientRect(parent, &area);
  const int n = arrange_collect(parent, list, MDI_MAX, kids, (int)count, (how & MDITILE_SKIPDISABLED_) != 0);
  arrange_windows(&area, list, n, FALSE, (how & MDITILE_HORIZONTAL_) != 0);
  return (WORD)n;
}
EXPORT WORD WINAPI CascadeWindows(HANDLE parent, UINT how, const RECT_ *rect, UINT count, const HANDLE *kids) {
  HANDLE list[MDI_MAX];
  RECT_ area;
  if (!parent) parent = GetDesktopWindow();
  if (rect) area = *rect; else GetClientRect(parent, &area);
  const int n = arrange_collect(parent, list, MDI_MAX, kids, (int)count, (how & MDITILE_SKIPDISABLED_) != 0);
  arrange_windows(&area, list, n, TRUE, FALSE);
  return (WORD)n;
}
EXPORT UINT WINAPI ArrangeIconicWindows(HANDLE parent) {
  RECT_ area;
  GetClientRect(parent, &area);
  int cx = GetSystemMetrics(47 /* SM_CXMINSPACING */), cy = GetSystemMetrics(48 /* SM_CYMINSPACING */);
  if (cx <= 0) cx = 160;
  if (cy <= 0) cy = 24;
  int x = 0, y = area.bottom - cy, placed = 0;
  for (HANDLE w = GetWindow(parent, 5); w; w = GetWindow(w, 2)) {
    if (!(GetWindowLongW(w, -16) & WS_MINIMIZE_)) continue;
    if (x + cx > area.right && x > 0) { x = 0; y -= cy; }
    SetWindowPos(w, NULL, x, y, 0, 0, 0x1 | 0x4 | 0x10);
    x += cx;
    ++placed;
  }
  return placed ? (UINT)cy : 0;
}

/* ---- the client -------------------------------------------------------------------------------------------------------------------------------------------------------- */
static HANDLE mdi_create_child(HANDLE client, MdiState *st, const MdiCreate_ *mcs) {
  if (st->count >= MDI_MAX) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  DWORD style;
  if (GetWindowLongW(client, -16) & 1 /* MDIS_ALLCHILDSTYLES */) {      /* the program's styles, as a clipped child (a popup is refused) */
    if (mcs->style & 0x80000000u) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    style = mcs->style | 0x40000000 | 0x04000000;
  } else {      /* the program's styles without WS_POPUP, plus those of a document window */
    style = (mcs->style & ~0x80000000u) | 0x40000000 | 0x10000000 | 0x04000000 | 0x00C00000 | 0x00080000 | 0x00040000 | 0x00020000 | 0x00010000;
  }
  RECT_ rc;
  mdi_client_rect(client, &rc);
  int x = mcs->x, y = mcs->y, cx = mcs->cx, cy = mcs->cy;
  const int step = GetSystemMetrics(4) + GetSystemMetrics(33);
  if (x == CW_USEDEFAULT_ || y == CW_USEDEFAULT_) { x = y = (st->cascade % 8) * step; ++st->cascade; }
  if (cx == CW_USEDEFAULT_) cx = (rc.right * 3) / 4;
  if (cy == CW_USEDEFAULT_) cy = (rc.bottom * 3) / 4;
  /* The child's creation parameter is the structure in the character set of its class: a program that gave the other one has its structure converted (WM_MDICREATE from an A caller to a
   * W class is the W structure that was made for it; a W caller to an A class gets an A copy). */
  WNDCLASSEX_ wc;
  for (size_t_ i = 0; i < sizeof wc; ++i) ((BYTE *)&wc)[i] = 0;
  wc.cbSize = sizeof wc;
  const BOOL class_is_a = GetClassInfoExW(mcs->owner ? mcs->owner : GetModuleHandleW(NULL), mcs->cls, &wc) && thunk_slot(wc.lpfnWndProc) >= 0;
  MdiCreate_ narrow = *mcs;
  char *cls_a = NULL, *title_a = NULL;
  void *params = (void *)mcs;
  if (g_mdi_params && !class_is_a) params = (void *)mcs;      /* A caller, W class: the W structure */
  else if (g_mdi_params) params = g_mdi_params;      /* A caller, A class: its own structure */
  else if (class_is_a) { cls_a = narrow_of(mcs->cls); title_a = narrow_of(mcs->title); narrow.cls = (const WCHAR *)cls_a; narrow.title = (const WCHAR *)title_a; params = &narrow; }      /* W caller, A class */
  g_mdi_params = NULL;
  g_mdi_creating = 1;
  HANDLE child = CreateWindowExW(WS_EX_MDICHILD_, mcs->cls, mcs->title, style, x, y, cx, cy, client, NULL, mcs->owner, params);
  g_mdi_creating = 0;
  narrow_free(cls_a, mcs->cls); narrow_free(title_a, mcs->title);
  if (!child) return NULL;
  SetWindowLongPtrW(child, -12 /* GWLP_ID */, (LONG_PTR)(st->first_id + (UINT)st->count));
  st->child[st->count] = child;
  { RECT_ r = {x, y, x + cx, y + cy}; st->restore[st->count] = r; }
  ++st->count;
  if (st->maximized) mdi_maximize(client, st, child);
  else if (style & WS_MAXIMIZE_) { SetWindowLongW(child, -16, style & ~WS_MAXIMIZE_); mdi_maximize(client, st, child); }
  if ((GetWindowLongW(child, -16) & 0x10000000) && !(GetWindowLongW(child, -16) & WS_MINIMIZE_)) mdi_activate(client, st, child);
  mdi_refresh_menu(client, st);
  return child;
}

static LONG_PTR WINAPI mdiclient_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  MdiState *st = message == 0x81 /* WM_NCCREATE */ ? NULL : ctl_state(hwnd);
  switch (message) {
    case 0x81: {
      MdiState *created = ctl_alloc(hwnd, sizeof *created);
      if (!created) return 0;
      created->frame = GetParent(hwnd);
      return builtin_proc(hwnd, message, wparam, lparam);
    }
    case 0x1 /* WM_CREATE */: {
      st = ctl_state(hwnd);
      const MdiClientCreate_ *cc = *(const MdiClientCreate_ **)lparam;
      if (st && cc) { st->window_menu = cc->window_menu; st->first_id = cc->first_id; }
      return 0;
    }
    case WM_NCDESTROY_: ctl_free(hwnd); return builtin_proc(hwnd, message, wparam, lparam);
    case 0x14 /* WM_ERASEBKGND */: {
      RECT_ rc;
      GetClientRect(hwnd, &rc);
      FillRect((HANDLE)wparam, &rc, GetSysColorBrush(12 /* COLOR_APPWORKSPACE */));
      return 1;
    }
    default: break;
  }
  if (!st) return builtin_proc(hwnd, message, wparam, lparam);
  switch (message) {
    case WM_MDICREATE_: return (LONG_PTR)mdi_create_child(hwnd, st, (const MdiCreate_ *)lparam);
    case WM_MDIDESTROY_: {
      HANDLE child = (HANDLE)wparam;
      if (mdi_find(st, child) < 0) return 0;
      mdi_forget(hwnd, st, child);
      DestroyWindow(child);
      return 0;
    }
    case WM_MDIACTIVATE_: if (mdi_find(st, (HANDLE)wparam) >= 0) mdi_activate(hwnd, st, (HANDLE)wparam); return 0;
    case WM_MDIRESTORE_: mdi_restore(hwnd, st, (HANDLE)wparam); return 0;
    case WM_MDIMAXIMIZE_: mdi_maximize(hwnd, st, (HANDLE)wparam); return 0;
    case WM_MDINEXT_: {
      HANDLE from = wparam ? (HANDLE)wparam : st->active, next = NULL;
      if (!from || st->count < 2) return 0;
      if (lparam) { next = GetWindow(hwnd, 5); next = GetWindow(next, 1); while (next && mdi_find(st, next) < 0) next = GetWindow(next, 3); }
      else { next = GetWindow(from, 2); while (next && mdi_find(st, next) < 0) next = GetWindow(next, 2); if (!next) next = mdi_top_child(hwnd, st, NULL); }
      if (next && next != from) mdi_activate(hwnd, st, next);
      return 0;
    }
    case WM_MDITILE_: case WM_MDICASCADE_: {
      HANDLE list[MDI_MAX];
      RECT_ rc;
      GetClientRect(hwnd, &rc);
      if (st->maximized) mdi_restore(hwnd, st, st->maximized);
      const int n = arrange_collect(hwnd, list, MDI_MAX, NULL, 0, (wparam & MDITILE_SKIPDISABLED_) != 0);
      arrange_windows(&rc, list, n, message == WM_MDICASCADE_, message == WM_MDITILE_ && (wparam & MDITILE_HORIZONTAL_));
      return 1;
    }
    case WM_MDIICONARRANGE_: ArrangeIconicWindows(hwnd); return 0;
    case WM_MDIGETACTIVE_: if (lparam) *(BOOL *)lparam = st->active && st->active == st->maximized; return (LONG_PTR)st->active;
    case WM_MDISETMENU_: {
      HANDLE old = GetMenu(st->frame);
      if (wparam) { SetMenu(st->frame, (HANDLE)wparam); }
      if (lparam) st->window_menu = (HANDLE)lparam;
      mdi_refresh_menu(hwnd, st);
      return (LONG_PTR)old;
    }
    case WM_MDIREFRESHMENU_: mdi_refresh_menu(hwnd, st); return (LONG_PTR)GetMenu(st->frame);
    case 0x5 /* WM_SIZE */:
      if (st->maximized) { SetWindowPos(st->maximized, NULL, 0, 0, LOWORD(lparam), HIWORD(lparam), 0x4 | 0x10); }
      return builtin_proc(hwnd, message, wparam, lparam);
    default: return builtin_proc(hwnd, message, wparam, lparam);
  }
}

/* ---- the frame and the children ------------------------------------------------------------------------------------------------------------------------------- */
static LONG_PTR frame_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  HANDLE client = (HANDLE)ctx;
  MdiState *st = client ? mdi_state(client) : NULL;
  if (st) {
    switch (message) {
      case WM_SETFOCUS_: SetFocus(client); return 0;
      case WM_COMMAND_: {
        const UINT id = LOWORD(wparam);
        if (st->first_id && id >= st->first_id && id < st->first_id + 9 && (int)(id - st->first_id) < st->count) { mdi_activate(client, st, st->child[id - st->first_id]); return 0; }
        break;
      }
      case 0x112 /* WM_SYSCOMMAND */:
        if ((wparam & 0xFFF0) == (SC_NEXTWINDOW_ & 0xFFF0) || (wparam & 0xFFF0) == (SC_PREVWINDOW_ & 0xFFF0)) {
          SendMessageW(client, WM_MDINEXT_, 0, (wparam & 0xFFF0) == (SC_PREVWINDOW_ & 0xFFF0));
          return 0;
        }
        break;
      default: break;
    }
  }
  return def_proc_w(NULL, hwnd, message, wparam, lparam);
}
EXPORT LONG_PTR WINAPI DefFrameProcW(HANDLE hwnd, HANDLE client, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return frame_w(client, hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI DefFrameProcA(HANDLE hwnd, HANDLE client, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return a_to_w(frame_w, client, hwnd, message, wparam, lparam); }

static LONG_PTR child_w(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  (void)ctx;
  HANDLE client = mdi_client_of(hwnd);
  MdiState *st = (client && IsWindow(client)) ? mdi_state(client) : NULL;
  if (message == 0x24 /* WM_GETMINMAXINFO */ && st && lparam) {      /* (asked while the child is being made, before it is in the list) */
    RECT_ rc;
    MINMAXINFO_ *mm = (MINMAXINFO_ *)lparam;
    mdi_max_rect(client, hwnd, &rc);
    mm->max_size.x = rc.right - rc.left; mm->max_size.y = rc.bottom - rc.top; mm->max_pos.x = rc.left; mm->max_pos.y = rc.top;
    return 0;
  }
  if (st && mdi_find(st, hwnd) >= 0) {
    switch (message) {
      case WM_CHILDACTIVATE_: case 0x21 /* WM_MOUSEACTIVATE */: case WM_SETFOCUS_:
        if (st->active != hwnd) SendMessageW(client, WM_MDIACTIVATE_, (ULONG_PTR)hwnd, 0);
        if (message == 0x21) return 1 /* MA_ACTIVATE */;
        break;
      case 0x112 /* WM_SYSCOMMAND */: {
        const UINT cmd = (UINT)(wparam & 0xFFF0);
        if (cmd == (SC_MAXIMIZE_ & 0xFFF0)) { SendMessageW(client, WM_MDIMAXIMIZE_, (ULONG_PTR)hwnd, 0); return 0; }
        if (cmd == (SC_RESTORE_ & 0xFFF0)) { SendMessageW(client, WM_MDIRESTORE_, (ULONG_PTR)hwnd, 0); return 0; }
        if (cmd == (SC_NEXTWINDOW_ & 0xFFF0)) { SendMessageW(client, WM_MDINEXT_, (ULONG_PTR)hwnd, 0); return 0; }
        if (cmd == (SC_PREVWINDOW_ & 0xFFF0)) { SendMessageW(client, WM_MDINEXT_, (ULONG_PTR)hwnd, 1); return 0; }
        break;
      }
      case 0x5 /* WM_SIZE */:
        if (wparam == 2 /* SIZE_MAXIMIZED */) mdi_merge(client, st, hwnd, TRUE);
        else if (wparam == 0 || wparam == 1) mdi_merge(client, st, hwnd, FALSE);
        break;
      case WM_NCDESTROY_: mdi_forget(client, st, hwnd); break;
      case WM_SETTEXT_: { const LONG_PTR r = def_proc_w(NULL, hwnd, message, wparam, lparam); mdi_refresh_menu(client, st); return r; }
      default: break;
    }
  }
  return def_proc_w(NULL, hwnd, message, wparam, lparam);
}
EXPORT LONG_PTR WINAPI DefMDIChildProcW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return child_w(NULL, hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI DefMDIChildProcA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return a_to_w(child_w, NULL, hwnd, message, wparam, lparam); }

/* CreateWindowEx with WS_EX_MDICHILD under an MDIClient: the client makes the child; the program's parameter becomes the lParam of the MDICREATESTRUCT the child gets */
static HANDLE mdi_create_through_client(HANDLE client, const WCHAR *cls, const WCHAR *title, DWORD style, int x, int y, int cx, int cy, HANDLE instance, void *params, BOOL ansi) {
  MdiCreate_ w = {cls, title, instance, x, y, cx, cy, style, (LONG_PTR)params};
  MdiCreate_ a = w;
  char *cls_a = NULL, *title_a = NULL;
  if (ansi) { cls_a = narrow_of(cls); title_a = narrow_of(title); a.cls = (const WCHAR *)cls_a; a.title = (const WCHAR *)title_a; g_mdi_params = &a; }
  const HANDLE h = (HANDLE)SendMessageW(client, WM_MDICREATE_, 0, (LONG_PTR)&w);
  g_mdi_params = NULL;
  narrow_free(cls_a, cls); narrow_free(title_a, title);
  return h;
}
EXPORT HANDLE WINAPI CreateMDIWindowW(LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int cx, int cy, HANDLE parent, HANDLE instance, LPARAM lparam) {
  MdiCreate_ mcs = {cls, title, instance, x, y, cx, cy, style, lparam};
  return (HANDLE)SendMessageW(parent, WM_MDICREATE_, 0, (LONG_PTR)&mcs);
}
EXPORT HANDLE WINAPI CreateMDIWindowA(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy, HANDLE parent, HANDLE instance, LPARAM lparam) {
  struct { LPCSTR cls, title; HANDLE owner; int x, y, cx, cy; DWORD style; LPARAM lparam; } mcs = {cls, title, instance, x, y, cx, cy, style, lparam};
  return (HANDLE)SendMessageA(parent, WM_MDICREATE_, 0, (LONG_PTR)&mcs);
}
