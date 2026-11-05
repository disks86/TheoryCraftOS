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

/* Menus of user32.dll, the user-interface half (included by window.inc after menu.inc): the layout and drawing of a menu, the popup window of class "#32768", the modal loop of
 * TrackPopupMenuEx and of the menu bar of a window, the keyboard entry into a menu (Alt, F10, Alt+Space, Alt+letter) and the system menu. The items come from win32k's menus (menu.inc).
 * The menu bar is drawn in the non-client area of its window by DefWindowProc (WM_NCPAINT, DrawMenuBar); the area itself (SM_CYMENU high) is reserved by win32k.
 * Notification order follows Windows: WM_ENTERMENULOOP, WM_INITMENU, WM_INITMENUPOPUP, WM_MENUSELECT ... WM_UNINITMENUPOPUP, WM_EXITMENULOOP, then the command is posted
 * (WM_COMMAND, WM_SYSCOMMAND for the system menu, WM_MENUCOMMAND for menus with MNS_NOTIFYBYPOS) or returned (TPM_RETURNCMD). Not done: scroll arrows for menus taller than the
 * screen, the delay before a submenu opens, multi-row menu bars, MDI child system menus. */

#define WM_INITMENU_ 0x116
#define WM_INITMENUPOPUP_ 0x117
#define WM_MENUSELECT_ 0x11F
#define WM_MENUCHAR_ 0x120
#define WM_ENTERMENULOOP_ 0x211
#define WM_EXITMENULOOP_ 0x212
#define WM_UNINITMENUPOPUP_ 0x125
#define WM_MENUCOMMAND_ 0x126
#define WM_MEASUREITEM_ 0x2C
#define WM_DRAWITEM_ 0x2B
#define WM_CANCELMODE_ 0x1F
#define WM_ENTERIDLE_ 0x121
#define WM_SYSCOMMAND_ 0x112
#define WM_NCHITTEST_ 0x84
#define WM_NCPAINT_ 0x85
#define WM_NCACTIVATE_ 0x86
#define WM_NCLBUTTONDOWN_ 0xA1
#define WM_NCRBUTTONUP_ 0xA5
#define WM_SYSKEYDOWN_ 0x104
#define WM_SYSKEYUP_ 0x105
#define WM_SYSCHAR_ 0x106
#define WM_KEYDOWN_ 0x100
#define WM_CHAR_ 0x102
#define WM_MOUSEMOVE_ 0x200
#define WM_LBUTTONDOWN_ 0x201
#define WM_LBUTTONUP_ 0x202
#define WM_RBUTTONDOWN_ 0x204
#define WM_RBUTTONUP_ 0x205
#define SC_KEYMENU_ 0xF100
#define HTMENU_ 5
#define HTCAPTION_ 2
#define HTSYSMENU_ 3
#define MNS_NOTIFYBYPOS_ 0x08000000
#define MIM_STYLE_ 0x10
#define MFS_GRAYED_ 0x3
#define MFS_CHECKED_ 0x8
#define MFS_HILITE_ 0x80
#define MFS_DEFAULT_ 0x1000
#define MFT_RADIOCHECK_ 0x200
#define MFT_MENUBREAK_ 0x40
#define MFT_MENUBARBREAK_ 0x20
#define MFT_RIGHTJUSTIFY_ 0x4000
#define MF_MOUSESELECT_ 0x8000
#define MF_SYSMENU_ 0x2000
#define TPM_RIGHTBUTTON_ 0x2
#define TPM_CENTERALIGN_ 0x4
#define TPM_RIGHTALIGN_ 0x8
#define TPM_VCENTERALIGN_ 0x10
#define TPM_BOTTOMALIGN_ 0x20
#define TPM_NONOTIFY_ 0x80
#define TPM_RETURNCMD_ 0x100
#define TPM_RECURSE_ 0x1
#define COLOR_MENU_ 4
#define COLOR_MENUTEXT_ 7
#define COLOR_HIGHLIGHT_ 13
#define COLOR_HIGHLIGHTTEXT_ 14
#define COLOR_3DHILIGHT_ 20
#define COLOR_MENUBAR_ 30
#define VK_MENU_ 0x12
#define VK_ESCAPE_ 0x1B
#define VK_RETURN_ 0x0D
#define VK_SPACE_ 0x20
#define VK_END_ 0x23
#define VK_HOME_ 0x24
#define VK_LEFT_ 0x25
#define VK_UP_ 0x26
#define VK_RIGHT_ 0x27
#define VK_DOWN_ 0x28
#define VK_F10_ 0x79
#define ERROR_POPUP_ALREADY_ACTIVE_ 1446
#define MSGF_MENU_ 2
#define MT_MAX_LEVELS 8
#define MENU_PAD 2           /* between the border of a popup and its items */
#define MENU_ARROW 10
#define MENU_TAB_GAP 16

typedef RECT_ MRect_;
typedef struct { LONG x, y; } MPoint_;
typedef struct {
  int x, y, w, h;            /* in the client area of the popup, or of the bar */
  UINT id, type, state;
  HANDLE sub;
  ULONG_PTR data;
  HANDLE bitmap, bmp_checked, bmp_unchecked;
  WCHAR *text;               /* with '&' and the tab; NULL for a separator or a bitmap */
  int text_w, accel_w, mnemonic;     /* the width of the text before the tab and after it; the index of the mnemonic character in the text without '&' (-1: none) */
} MItem_;
typedef struct { HANDLE menu; BOOL bar; int count; MItem_ *items; int width, height, text_h; } MView_;
typedef struct { HANDLE menu, hwnd; MView_ view; int sel, x, y; BOOL bar; } MLevel_;
typedef struct {
  HANDLE owner; UINT flags; BOOL sysmenu, done, moved, down_seen, keyboard, bar_mode;
  UINT result_id; HANDLE result_menu; int result_index; BOOL have_result;
  int depth; MLevel_ lv[MT_MAX_LEVELS];
  HANDLE bar_hwnd;
} MTrack_;
static MTrack_ *g_mt;
static struct { HANDLE menu; int x, y; } g_popup_place[8];      /* where the popups last were: GetMenuItemRect still answers after the menu closed */
static int g_popup_place_next;
static BOOL g_alt_alone;

IMPORT BOOL WINAPI SetViewportOrgEx(HANDLE hdc, int x, int y, POINT_ *old);
EXPORT HANDLE WINAPI GetWindowDC(HANDLE window);
EXPORT BOOL WINAPI ClientToScreen(HANDLE hwnd, POINT_ *p);
EXPORT BOOL WINAPI GetWindowRect(HANDLE hwnd, RECT_ *rect);
EXPORT BOOL WINAPI UpdateWindow(HANDLE hwnd);
EXPORT BOOL WINAPI PeekMessageW(MSG_ *msg, HANDLE hwnd, UINT first, UINT last, UINT flags);
EXPORT BOOL WINAPI PostMessageW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);
EXPORT BOOL WINAPI DestroyWindow(HANDLE hwnd);
EXPORT BOOL WINAPI ShowWindow(HANDLE hwnd, int cmd);
EXPORT BOOL WINAPI AdjustWindowRectEx(RECT_ *rect, DWORD style, BOOL menu, DWORD ex_style);

/* ---- layout ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static int menu_text_extent(HANDLE hdc, const WCHAR *s, int n) {
  SIZE_ sz = {0, 0};
  if (n > 0) GetTextExtentPoint32W(hdc, s, n, &sz);
  return sz.cx;
}
/* the text without '&' (a mnemonic marker), up to `limit` characters; *mnemonic is the index of the marked character in the result */
static int menu_clean(const WCHAR *s, int n, WCHAR *out, int limit, int *mnemonic) {
  int k = 0;
  if (mnemonic) *mnemonic = -1;
  for (int i = 0; i < n && k < limit; ++i) {
    if (s[i] == L'&') {
      if (i + 1 < n && s[i + 1] == L'&') { out[k++] = L'&'; ++i; }
      else if (mnemonic && *mnemonic < 0 && i + 1 < n) *mnemonic = k;
      continue;
    }
    out[k++] = s[i];
  }
  return k;
}
static void menu_view_free(MView_ *v) {
  for (int i = 0; i < v->count; ++i) u32_free(v->items[i].text);
  u32_free(v->items);
  v->items = NULL;
  v->count = 0;
}
static HANDLE menu_font(void) { return GetStockObject(17 /* DEFAULT_GUI_FONT */); }

/* The items of a menu with their rectangles. A popup is a column (or several, at MFT_MENUBREAK / MFT_MENUBARBREAK) of items; a bar is one row. `owner` is asked to measure owner-draw items. */
static BOOL menu_view_build(HANDLE menu, BOOL bar, HANDLE owner, MView_ *v) {
  int count = GetMenuItemCount(menu);
  v->menu = menu; v->bar = bar; v->count = 0; v->items = NULL; v->width = v->height = 0;
  if (count < 0) return FALSE;
  HANDLE screen = GetDC(NULL);
  HANDLE old = SelectObject(screen, menu_font());
  SIZE_ unit;
  GetTextExtentPoint32W(screen, L"Ay", 2, &unit);
  v->text_h = unit.cy > 0 ? unit.cy : 16;
  if (count > 0) v->items = u32_alloc((SIZE_T)count * sizeof(MItem_));
  if (count > 0 && !v->items) { SelectObject(screen, old); ReleaseDC(NULL, screen); return FALSE; }
  const int check_w = GetSystemMetrics(71 /* SM_CXMENUCHECK */) > 0 ? GetSystemMetrics(71) : 15;
  const int bar_h = GetSystemMetrics(15 /* SM_CYMENU */);
  const int avec = (menu_text_extent(screen, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", 52) + 26) / 52;      /* the average character width: the margin of an owner-draw item */
  int x = bar ? 0 : MENU_PAD, y = bar ? 0 : MENU_PAD, col_w = 0, col_start = 0, total_w = 0, total_h = 0;
  if (!bar && count > 0) { MII_ first = {sizeof first, MIIM_FTYPE_}; if (GetMenuItemInfoW(menu, 0, TRUE, &first) && (first.fType & (MFT_MENUBREAK_ | MFT_MENUBARBREAK_))) x += 4; }      /* a break before the first item still leaves its gap */
  for (int i = 0; i < count; ++i) {
    MItem_ *it = &v->items[i];
    for (size_t_ b = 0; b < sizeof *it; ++b) ((BYTE *)it)[b] = 0;
    it->mnemonic = -1;
    MII_ mii = {sizeof mii, MIIM_FTYPE_ | MIIM_STATE_ | MIIM_ID_ | MIIM_SUBMENU_ | MIIM_DATA_ | MIIM_BITMAP_ | MIIM_CHECKMARKS_ | MIIM_STRING_};
    if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
    it->id = mii.wID; it->type = mii.fType; it->state = mii.fState; it->sub = mii.hSubMenu; it->data = mii.dwItemData;
    it->bitmap = mii.hbmpItem; it->bmp_checked = mii.hbmpChecked; it->bmp_unchecked = mii.hbmpUnchecked;
    v->count = i + 1;
    if (mii.cch > 0 && !(it->type & (MFT_BITMAP_ | MFT_SEPARATOR_))) {
      it->text = u32_alloc(((SIZE_T)mii.cch + 1) * 2);
      if (it->text) {
        MII_ get = {sizeof get, MIIM_STRING_};
        get.dwTypeData = it->text; get.cch = mii.cch + 1;
        if (!GetMenuItemInfoW(menu, i, TRUE, &get)) it->text[0] = 0;
      }
    }
    if (!bar && i > 0 && (it->type & (MFT_MENUBREAK_ | MFT_MENUBARBREAK_))) {      /* a new column */
      for (int j = col_start; j < i; ++j) v->items[j].w = col_w;
      x += col_w + 4;
      total_w = x;
      y = MENU_PAD; col_w = 0; col_start = i;
    }
    it->x = x; it->y = y;
    if ((it->type & MFT_SEPARATOR_) && !(it->type & MFT_OWNERDRAW_)) { it->h = 7; it->w = 0; }
    else if (it->type & MFT_OWNERDRAW_) {
      struct { UINT type, ctl, item, width, height; ULONG_PTR data; } mis = {1 /* ODT_MENU */, 0, it->id, 0, (UINT)(GetDialogBaseUnits() >> 16), it->data};
      if (owner) SendMessageW(owner, WM_MEASUREITEM_, 0, (LONG_PTR)&mis);
      it->w = (int)mis.width + (bar ? 0 : 2 * avec); it->h = (int)mis.height;
    } else if (it->type & MFT_BITMAP_) {
      Bitmap_ bm;
      const BOOL real = ((ULONG_PTR)it->bitmap >> 16) != 0 && (ULONG_PTR)it->bitmap != (ULONG_PTR)-1 && GetObjectW(it->bitmap, sizeof bm, &bm) != 0;
      it->w = real ? bm.width + 4 : 16; it->h = real ? bm.height + 2 : v->text_h + 6;
    } else {
      WCHAR clean[256];
      const int len = it->text ? (int)ws_len(it->text) : 0;
      int tab = len;
      for (int k = 0; k < len; ++k) if (it->text[k] == L'\t') { tab = k; break; }
      const int n = menu_clean(it->text, tab, clean, 255, &it->mnemonic);
      it->text_w = menu_text_extent(screen, clean, n);
      if (tab < len) { const int a = menu_clean(it->text + tab + 1, len - tab - 1, clean, 255, NULL); it->accel_w = menu_text_extent(screen, clean, a); }
      if (bar) it->w = it->text_w + 16;
      else {
        it->w = check_w + 8 + it->text_w + (it->accel_w ? MENU_TAB_GAP + it->accel_w : 0) + 12 + (it->sub ? MENU_ARROW : 0);
        if (it->bitmap && ((ULONG_PTR)it->bitmap >> 16) != 0) { Bitmap_ bm; if (GetObjectW(it->bitmap, sizeof bm, &bm)) it->w += bm.width + 4; }
      }
      it->h = bar ? bar_h - 1 : v->text_h + 6;
    }
    if (bar) { it->h = bar_h - 1; x += it->w; }
    else { y += it->h; if (it->w > col_w) col_w = it->w; if (y + MENU_PAD > total_h) total_h = y + MENU_PAD; }
  }
  if (bar) { total_w = x; total_h = bar_h; }
  else { for (int j = col_start; j < v->count; ++j) v->items[j].w = col_w; total_w = x + col_w + MENU_PAD; if (total_h < 2 * MENU_PAD) total_h = 2 * MENU_PAD; }
  v->width = total_w; v->height = total_h;
  if (bar) {      /* MFT_RIGHTJUSTIFY: the item and those after it end at the right edge (the caller sets the width) */
  }
  SelectObject(screen, old);
  ReleaseDC(NULL, screen);
  return TRUE;
}
static int menu_view_hit(const MView_ *v, int x, int y) {
  for (int i = 0; i < v->count; ++i) {
    const MItem_ *it = &v->items[i];
    if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return i;
  }
  return -1;
}

/* ---- drawing ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static void menu_fill(HANDLE hdc, int l, int t, int r, int b, DWORD color) {
  RECT_ rc = {l, t, r, b};
  HANDLE brush = CreateSolidBrush(color);
  FillRect(hdc, &rc, brush);
  DeleteObject(brush);
}
static void menu_hline(HANDLE hdc, int x, int y, int w, DWORD color) { menu_fill(hdc, x, y, x + w, y + 1, color); }
static void menu_draw_string(HANDLE hdc, const WCHAR *s, int n, int x, int y, int mnemonic, DWORD color, int text_h) {
  WCHAR clean[256];
  int mn = -1;
  const int k = menu_clean(s, n, clean, 255, &mn);
  if (mnemonic < 0) mn = -1;
  SetTextColor(hdc, color);
  ExtTextOutW(hdc, x, y, 0, NULL, clean, (UINT)k, NULL);
  if (mn >= 0) {
    const int ux = x + menu_text_extent(hdc, clean, mn), uw = menu_text_extent(hdc, clean + mn, 1);
    menu_hline(hdc, ux, y + text_h - 1, uw, color);
  }
}
static void menu_draw_bitmap(HANDLE hdc, HANDLE bitmap, int x, int y) {
  Bitmap_ bm;
  if (((ULONG_PTR)bitmap >> 16) == 0 || (ULONG_PTR)bitmap == (ULONG_PTR)-1 || GetObjectW(bitmap, sizeof bm, &bm) == 0) return;
  HANDLE mem = CreateCompatibleDC(hdc);
  if (!mem) return;
  HANDLE old = SelectObject(mem, bitmap);
  BitBlt(hdc, x, y, bm.width, bm.height, mem, 0, 0, SRCCOPY_);
  SelectObject(mem, old);
  DeleteDC(mem);
}
static void menu_draw_check(HANDLE hdc, int x, int y, int h, BOOL radio, DWORD color) {
  HANDLE brush = CreateSolidBrush(color);
  HANDLE old = SelectObject(hdc, brush);
  const int cy = y + h / 2;
  if (radio) { PatBlt(hdc, x + 3, cy - 1, 5, 3, PATCOPY_); PatBlt(hdc, x + 4, cy - 2, 3, 5, PATCOPY_); }
  else {      /* a tick */
    PatBlt(hdc, x + 1, cy, 1, 2, PATCOPY_); PatBlt(hdc, x + 2, cy + 1, 1, 2, PATCOPY_); PatBlt(hdc, x + 3, cy + 2, 1, 2, PATCOPY_);
    PatBlt(hdc, x + 4, cy + 1, 1, 2, PATCOPY_); PatBlt(hdc, x + 5, cy, 1, 2, PATCOPY_); PatBlt(hdc, x + 6, cy - 1, 1, 2, PATCOPY_); PatBlt(hdc, x + 7, cy - 2, 1, 2, PATCOPY_);
  }
  SelectObject(hdc, old);
  DeleteObject(brush);
}
static void menu_draw_arrow(HANDLE hdc, int x, int y, int h, DWORD color) {
  HANDLE brush = CreateSolidBrush(color);
  HANDLE old = SelectObject(hdc, brush);
  const int cy = y + h / 2;
  for (int i = 0; i < 4; ++i) PatBlt(hdc, x + i, cy - 4 + i, 1, 9 - 2 * i, PATCOPY_);
  SelectObject(hdc, old);
  DeleteObject(brush);
}
/* A menu (or a bar) in a device context whose origin is the top-left of the menu's client area; `sel` is the highlighted item. */
static void menu_paint(HANDLE hdc, const MView_ *v, int sel, HANDLE owner, HANDLE hmenu, int bar_width) {
  const DWORD back = GetSysColor(v->bar ? COLOR_MENUBAR_ : COLOR_MENU_), text = GetSysColor(COLOR_MENUTEXT_);
  const DWORD hi_back = GetSysColor(COLOR_HIGHLIGHT_), hi_text = GetSysColor(COLOR_HIGHLIGHTTEXT_), gray = GetSysColor(COLOR_GRAYTEXT_);
  const int check_w = GetSystemMetrics(71) > 0 ? GetSystemMetrics(71) : 15;
  HANDLE old = SelectObject(hdc, menu_font());
  SetBkMode(hdc, 1 /* TRANSPARENT */);
  if (!v->bar) menu_fill(hdc, 0, 0, v->width, v->height, back);
  else menu_fill(hdc, 0, 0, bar_width > v->width ? bar_width : v->width, v->height, back);
  for (int i = 0; i < v->count; ++i) {
    const MItem_ *it = &v->items[i];
    const BOOL disabled = (it->state & MFS_GRAYED_) != 0, selected = i == sel;
    if ((it->type & MFT_SEPARATOR_) && !(it->type & MFT_OWNERDRAW_)) {
      menu_hline(hdc, it->x + 2, it->y + it->h / 2 - 1, it->w - 4, GetSysColor(COLOR_BTNSHADOW_));
      menu_hline(hdc, it->x + 2, it->y + it->h / 2, it->w - 4, GetSysColor(COLOR_3DHILIGHT_));
      continue;
    }
    if (selected && !disabled) menu_fill(hdc, it->x, it->y, it->x + it->w, it->y + it->h, hi_back);
    if (it->type & MFT_OWNERDRAW_) {
      struct { UINT type, ctl, item, action, state; HANDLE hwnd; HANDLE dc; RECT_ rc; ULONG_PTR data; } dis =
          {1 /* ODT_MENU */, 0, it->id, 1 /* ODA_DRAWENTIRE */, (selected ? 1u : 0u) | (disabled ? 6u : 0u) | ((it->state & MFS_CHECKED_) ? 8u : 0u), hmenu, hdc, {it->x, it->y, it->x + it->w, it->y + it->h}, it->data};
      if (owner) SendMessageW(owner, WM_DRAWITEM_, 0, (LONG_PTR)&dis);
      continue;
    }
    const DWORD color = disabled ? gray : (selected ? hi_text : text);
    if (it->type & MFT_BITMAP_) { menu_draw_bitmap(hdc, it->bitmap, it->x + 2, it->y + 1); continue; }
    if (v->bar) {
      if (it->text) menu_draw_string(hdc, it->text, (int)ws_len(it->text), it->x + 8, it->y + (it->h - v->text_h) / 2, it->mnemonic, color, v->text_h);
      continue;
    }
    if (it->state & MFS_CHECKED_) {
      HANDLE custom = it->bmp_checked;
      if (custom && ((ULONG_PTR)custom >> 16) != 0) menu_draw_bitmap(hdc, custom, it->x + 2, it->y + 2);
      else menu_draw_check(hdc, it->x + 2, it->y, it->h, (it->type & MFT_RADIOCHECK_) != 0, color);
    } else if (it->bmp_unchecked && ((ULONG_PTR)it->bmp_unchecked >> 16) != 0) menu_draw_bitmap(hdc, it->bmp_unchecked, it->x + 2, it->y + 2);
    int tx = it->x + check_w + 8;
    if (it->bitmap && ((ULONG_PTR)it->bitmap >> 16) != 0) {
      Bitmap_ bm;
      menu_draw_bitmap(hdc, it->bitmap, tx - 4, it->y + 2);
      if (GetObjectW(it->bitmap, sizeof bm, &bm)) tx += bm.width + 4;
    }
    if (it->text) {
      const int len = (int)ws_len(it->text);
      int tab = len;
      for (int k = 0; k < len; ++k) if (it->text[k] == L'\t') { tab = k; break; }
      const int ty = it->y + (it->h - v->text_h) / 2;
      if (disabled && !selected) menu_draw_string(hdc, it->text, tab, tx + 1, ty + 1, it->mnemonic, GetSysColor(COLOR_3DHILIGHT_), v->text_h);
      menu_draw_string(hdc, it->text, tab, tx, ty, it->mnemonic, color, v->text_h);
      if (tab < len) menu_draw_string(hdc, it->text + tab + 1, len - tab - 1, it->x + it->w - 12 - (it->sub ? MENU_ARROW : 0) - it->accel_w, ty, -1, color, v->text_h);
    }
    if (it->sub) menu_draw_arrow(hdc, it->x + it->w - MENU_ARROW - 4, it->y, it->h, color);
    if (it->state & MFS_DEFAULT_) { /* the default item is shown in bold in Windows; here it is not distinguished */ }
  }
  SelectObject(hdc, old);
}

/* ---- the window of a popup menu --------------------------------------------------------------------------------------------------------------------------------------- */
static MLevel_ *mt_level_of(HANDLE hwnd) {
  if (!g_mt) return NULL;
  for (int i = 0; i < g_mt->depth; ++i) if (g_mt->lv[i].hwnd == hwnd) return &g_mt->lv[i];
  return NULL;
}
static LONG_PTR WINAPI menu_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  switch (message) {
    case 0x14 /* WM_ERASEBKGND */: return 1;
    case 0x1E1 /* MN_GETHMENU */: { MLevel_ *lv = mt_level_of(hwnd); return lv ? (LONG_PTR)lv->menu : 0; }
    case 0x21 /* WM_MOUSEACTIVATE */: return 3 /* MA_NOACTIVATE */;
    case 0xF /* WM_PAINT */: {
      PAINTSTRUCT_ ps;
      HANDLE hdc = BeginPaint(hwnd, &ps);
      MLevel_ *lv = mt_level_of(hwnd);
      if (hdc && lv) menu_paint(hdc, &lv->view, lv->sel, g_mt->owner, lv->menu, 0);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default: return builtin_proc(hwnd, message, wparam, lparam);
  }
}

/* ---- tracking ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static void mt_repaint(MLevel_ *lv, HANDLE owner) {
  if (lv->bar) {
    HANDLE win = g_mt->bar_hwnd;
    RECT_ wr;
    if (!win || !GetWindowRect(win, &wr)) return;
    HANDLE hdc = GetWindowDC(win);
    if (!hdc) return;
    SaveDC(hdc);
    /* the bar is the strip above the client area; the device context is the window's, with its origin at the top-left of the window */
    SetViewportOrgEx(hdc, lv->x - wr.left, lv->y - wr.top, NULL);
    menu_paint(hdc, &lv->view, lv->sel, owner, lv->menu, 0);
    RestoreDC(hdc, -1);
    ReleaseDC(win, hdc);
    return;
  }
  if (lv->hwnd) { InvalidateRect(lv->hwnd, NULL, FALSE); UpdateWindow(lv->hwnd); }
}
static BOOL mt_item_selectable(const MItem_ *it) { return !(it->type & MFT_SEPARATOR_); }

/* WM_MENUSELECT for the highlighted item (or for none) */
static void mt_notify_select(MTrack_ *t, MLevel_ *lv, int index, BOOL by_mouse) {
  if (t->flags & TPM_NONOTIFY_) return;
  if (index < 0 || index >= lv->view.count) {
    SendMessageW(t->owner, WM_MENUSELECT_, 0xFFFF0000u, 0);
    return;
  }
  const MItem_ *it = &lv->view.items[index];
  UINT flags = (it->state & (MFS_GRAYED_ | MFS_CHECKED_ | MFS_HILITE_ | MFS_DEFAULT_)) | (it->type & (MFT_BITMAP_ | MFT_OWNERDRAW_ | MFT_SEPARATOR_));
  if (it->sub) flags |= MF_POPUP_;
  if (t->sysmenu && lv == &t->lv[0]) flags |= MF_SYSMENU_;
  if (by_mouse) flags |= MF_MOUSESELECT_;
  SendMessageW(t->owner, WM_MENUSELECT_, (it->sub ? (UINT)index : it->id) | (flags << 16), (LONG_PTR)lv->menu);
}
static void mt_select(MTrack_ *t, int level, int index, BOOL by_mouse) {
  MLevel_ *lv = &t->lv[level];
  if (lv->sel == index) return;
  lv->sel = index;
  mt_repaint(lv, t->owner);
  mt_notify_select(t, lv, index, by_mouse);
}
static void mt_close_to(MTrack_ *t, int depth) {
  while (t->depth > depth) {
    MLevel_ *lv = &t->lv[--t->depth];
    if (!lv->bar) { g_popup_place[g_popup_place_next & 7].menu = lv->menu; g_popup_place[g_popup_place_next & 7].x = lv->x; g_popup_place[g_popup_place_next & 7].y = lv->y; ++g_popup_place_next; }
    if (!lv->bar && !(t->flags & TPM_NONOTIFY_)) SendMessageW(t->owner, WM_UNINITMENUPOPUP_, (ULONG_PTR)lv->menu, (t->sysmenu && t->depth == 0) ? (LONG_PTR)(MF_SYSMENU_ << 16) : 0);
    if (lv->hwnd) DestroyWindow(lv->hwnd);
    menu_view_free(&lv->view);
    lv->hwnd = NULL;
  }
}
static void mt_screen_size(int *w, int *h) { *w = GetSystemMetrics(0 /* SM_CXSCREEN */); *h = GetSystemMetrics(1 /* SM_CYSCREEN */); if (*w <= 0) *w = 1024; if (*h <= 0) *h = 768; }

/* Opens a popup level at the screen position (x, y) (its top-left); `flip_x` is where it goes when it does not fit on the right (the left edge of the parent); returns the new level or -1. */
static int mt_open_popup(MTrack_ *t, HANDLE menu, int x, int y, int flip_x, UINT parent_index) {
  if (t->depth >= MT_MAX_LEVELS) return -1;
  if (!(t->flags & TPM_NONOTIFY_)) SendMessageW(t->owner, WM_INITMENUPOPUP_, (ULONG_PTR)menu, (LONG_PTR)(parent_index | ((t->sysmenu && t->depth == 0) ? (UINT)(1u << 16) : 0)));
  MLevel_ *lv = &t->lv[t->depth];
  if (!menu_view_build(menu, FALSE, t->owner, &lv->view)) return -1;
  lv->menu = menu; lv->bar = FALSE; lv->sel = -1;
  MRect_ rc = {0, 0, lv->view.width, lv->view.height};
  AdjustWindowRectEx(&rc, 0x80000000u | 0x00800000u /* WS_POPUP | WS_BORDER */, FALSE, 0x80 | 0x8 /* WS_EX_TOOLWINDOW | WS_EX_TOPMOST */);
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  int sw, sh;
  mt_screen_size(&sw, &sh);
  if (x + w > sw) x = flip_x >= 0 && flip_x - w >= 0 ? flip_x - w : sw - w;
  if (y + h > sh) y = sh - h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  lv->hwnd = CreateWindowExW(0x80 | 0x8, L"#32768", NULL, 0x80000000u | 0x00800000u, x, y, w, h, t->owner, NULL, GetModuleHandleW(L"user32.dll"), NULL);
  if (!lv->hwnd) { menu_view_free(&lv->view); return -1; }
  lv->x = x - rc.left; lv->y = y - rc.top;      /* the client origin: the window position less the border */
  ++t->depth;
  ShowWindow(lv->hwnd, 4 /* SW_SHOWNOACTIVATE */);
  UpdateWindow(lv->hwnd);
  return t->depth - 1;
}
/* The submenu of the highlighted item of a level, opened (the levels below are closed first). */
static BOOL mt_open_sub(MTrack_ *t, int level) {
  MLevel_ *lv = &t->lv[level];
  if (lv->sel < 0) return FALSE;
  const MItem_ *it = &lv->view.items[lv->sel];
  mt_close_to(t, level + 1);
  if (!it->sub || (it->state & MFS_GRAYED_)) return FALSE;
  if (lv->bar) t->bar_mode = 2;
  int x, y, flip;
  if (lv->bar) { x = lv->x + it->x; y = lv->y + lv->view.height; flip = -1; }
  else { x = lv->x + it->x + it->w - 3; y = lv->y + it->y - MENU_PAD; flip = lv->x + it->x + 3; }
  return mt_open_popup(t, it->sub, x, y, flip, (UINT)lv->sel) >= 0;
}
static int mt_hit(MTrack_ *t, int px, int py, int *index) {
  for (int d = t->depth - 1; d >= 0; --d) {
    MLevel_ *lv = &t->lv[d];
    const int x = px - lv->x, y = py - lv->y;
    if (x < 0 || y < 0 || x >= lv->view.width + (lv->bar ? 4096 : 0) || y >= lv->view.height) continue;
    *index = menu_view_hit(&lv->view, x, y);
    return d;
  }
  return -1;
}
static void mt_choose(MTrack_ *t, int level, int index) {
  MLevel_ *lv = &t->lv[level];
  const MItem_ *it = &lv->view.items[index];
  t->result_id = it->id; t->result_menu = lv->menu; t->result_index = index; t->have_result = TRUE; t->done = TRUE;
}
static void mt_activate(MTrack_ *t, int level) {
  MLevel_ *lv = &t->lv[level];
  if (lv->sel < 0) return;
  const MItem_ *it = &lv->view.items[lv->sel];
  if (it->sub && IsMenu(it->sub)) { mt_open_sub(t, level); if (t->depth > level + 1 && t->keyboard) mt_select(t, level + 1, 0, FALSE); return; }      /* (a destroyed submenu: the item is chosen) */
  if (it->state & MFS_GRAYED_) return;
  mt_choose(t, level, lv->sel);
}
static int mt_step(const MView_ *v, int from, int dir) {
  if (v->count == 0) return -1;
  int i = from;
  for (int n = 0; n < v->count; ++n) {
    i += dir;
    if (i < 0) i = v->count - 1;
    if (i >= v->count) i = 0;
    if (mt_item_selectable(&v->items[i])) return i;
  }
  return -1;
}
/* the item of a level with this mnemonic: the first after the selection; *unique is set when no other item has it */
static int mt_mnemonic(const MView_ *v, int sel, WCHAR ch, BOOL *unique) {
  int found = -1, count = 0;
  WCHAR up = ch >= L'a' && ch <= L'z' ? (WCHAR)(ch - 32) : ch;
  for (int n = 1; n <= v->count; ++n) {
    const int i = (sel + n + v->count) % v->count;
    const MItem_ *it = &v->items[i];
    if (!it->text || it->mnemonic < 0) continue;
    WCHAR clean[256];
    int mn;
    const int k = menu_clean(it->text, (int)ws_len(it->text), clean, 255, &mn);
    if (mn < 0 || mn >= k) continue;
    WCHAR c = clean[mn];
    if (c >= L'a' && c <= L'z') c = (WCHAR)(c - 32);
    if (c == up) { if (found < 0) found = i; ++count; }
  }
  if (unique) *unique = count == 1;
  return found;
}

static void mt_key(MTrack_ *t, UINT vk) {
  const int level = t->depth - 1;
  MLevel_ *lv = &t->lv[level];
  t->keyboard = TRUE;
  switch (vk) {
    case VK_ESCAPE_:
      if (t->depth > 1) { mt_close_to(t, t->depth - 1); if (t->depth == 1 && t->lv[0].bar) t->bar_mode = 0; }
      else t->done = TRUE;
      break;
    case VK_UP_: case VK_DOWN_:
      if (lv->bar) { if (vk == VK_DOWN_) { if (lv->sel < 0) mt_select(t, level, mt_step(&lv->view, -1, 1), FALSE); mt_activate(t, level); } }
      else mt_select(t, level, mt_step(&lv->view, lv->sel < 0 ? (vk == VK_DOWN_ ? -1 : 0) : lv->sel, vk == VK_DOWN_ ? 1 : -1), FALSE);
      break;
    case VK_HOME_: case VK_END_:
      if (!lv->bar) mt_select(t, level, vk == VK_HOME_ ? mt_step(&lv->view, -1, 1) : mt_step(&lv->view, 0, -1), FALSE);
      break;
    case VK_LEFT_: case VK_RIGHT_: {
      const int dir = vk == VK_RIGHT_ ? 1 : -1;
      if (lv->bar) {      /* the next item of the bar, with its popup if one is open */
        const BOOL had_popup = t->depth > 1;
        mt_close_to(t, 1);
        mt_select(t, 0, mt_step(&lv->view, lv->sel, dir), FALSE);
        if (had_popup) { mt_open_sub(t, 0); if (t->depth > 1) mt_select(t, 1, mt_step(&t->lv[1].view, -1, 1), FALSE); }
      } else if (dir > 0 && lv->sel >= 0 && lv->view.items[lv->sel].sub) {
        mt_open_sub(t, level);
        if (t->depth > level + 1) mt_select(t, level + 1, mt_step(&t->lv[level + 1].view, -1, 1), FALSE);
      } else if (dir < 0 && t->depth > 1 && !t->lv[level - 1].bar) {
        mt_close_to(t, t->depth - 1);
      } else if (t->depth > 1 && t->lv[level - 1].bar) {      /* a popup of the bar: the neighbour on the bar */
        MLevel_ *bar = &t->lv[level - 1];
        mt_close_to(t, 1);
        mt_select(t, 0, mt_step(&bar->view, bar->sel, dir), FALSE);
        mt_open_sub(t, 0);
        if (t->depth > 1) mt_select(t, 1, mt_step(&t->lv[1].view, -1, 1), FALSE);
      }
      break;
    }
    case VK_RETURN_: mt_activate(t, level); break;
    case VK_F10_: case VK_MENU_: t->done = TRUE; break;
    default: break;
  }
}
static void mt_char(MTrack_ *t, WCHAR ch) {
  const int level = t->depth - 1;
  MLevel_ *lv = &t->lv[level];
  if (ch < 0x20) return;
  BOOL unique = FALSE;
  const int i = mt_mnemonic(&lv->view, lv->sel < 0 ? -1 : lv->sel, ch, &unique);
  if (i < 0) {
    if (t->flags & TPM_NONOTIFY_) return;
    const LONG_PTR r = SendMessageW(t->owner, WM_MENUCHAR_, (ULONG_PTR)ch | ((ULONG_PTR)((t->sysmenu && lv == &t->lv[0] ? MF_SYSMENU_ : 0) | (lv->bar ? 0 : MF_POPUP_)) << 16), (LONG_PTR)lv->menu);
    if ((r >> 16) == 2 /* MNC_EXECUTE */ && (int)(r & 0xFFFF) < lv->view.count) { mt_select(t, level, (int)(r & 0xFFFF), FALSE); mt_activate(t, level); }
    else if ((r >> 16) == 1 /* MNC_SELECT */ && (int)(r & 0xFFFF) < lv->view.count) mt_select(t, level, (int)(r & 0xFFFF), FALSE);
    return;
  }
  mt_select(t, level, i, FALSE);
  if (unique || lv->view.items[i].sub) mt_activate(t, level);
}

static void mt_mouse(MTrack_ *t, UINT message, int px, int py) {
  int index = -1;
  const int level = mt_hit(t, px, py, &index);
  if (message == WM_MOUSEMOVE_) {
    if (level < 0) return;
    t->moved = TRUE;
    t->keyboard = FALSE;
    MLevel_ *lv = &t->lv[level];
    if (index >= 0 && !mt_item_selectable(&lv->view.items[index])) index = -1;
    if (lv->bar) {
      if (index < 0 || index == lv->sel) return;
      if (t->bar_mode == 2) { mt_close_to(t, 1); mt_select(t, 0, index, TRUE); mt_open_sub(t, 0); }
      else mt_select(t, 0, index, TRUE);
      return;
    }
    if (index != lv->sel) { mt_close_to(t, level + 1); mt_select(t, level, index, TRUE); }
    if (index >= 0 && lv->view.items[index].sub && t->depth == level + 1) mt_open_sub(t, level);
    return;
  }
  if (message == WM_LBUTTONDOWN_ || message == WM_RBUTTONDOWN_) {
    t->down_seen = TRUE;
    if (level < 0) { t->done = TRUE; return; }      /* a click outside closes the menu */
    MLevel_ *lv = &t->lv[level];
    if (index < 0 || !mt_item_selectable(&lv->view.items[index])) return;
    if (lv->bar) {      /* a click on the bar opens the popup of the item, or closes it again */
      const BOOL same = lv->sel == index && t->depth > 1;
      mt_close_to(t, 1);
      mt_select(t, level, index, TRUE);
      if (same) { t->bar_mode = 0; return; }
      if (lv->view.items[index].sub) mt_open_sub(t, level); else if (!(lv->view.items[index].state & MFS_GRAYED_)) mt_choose(t, level, index);
      return;
    }
    mt_select(t, level, index, TRUE);
    if (lv->view.items[index].sub) mt_open_sub(t, level);
    return;
  }
  if (message == WM_LBUTTONUP_ || message == WM_RBUTTONUP_) {
    if (!t->down_seen && !t->moved) return;
    if (level < 0) return;
    MLevel_ *lv = &t->lv[level];
    if (index < 0 || lv->bar || !mt_item_selectable(&lv->view.items[index])) return;
    if (lv->view.items[index].sub) { if (t->depth == level + 1) mt_open_sub(t, level); return; }
    if (lv->view.items[index].state & MFS_GRAYED_) return;
    mt_choose(t, level, index);
  }
}

/* The modal loop; the levels already open are the menu's. */
static void mt_loop(MTrack_ *t) {
  HANDLE old_capture = GetCapture();
  SetCapture(t->owner);
  MSG_ msg;
  BOOL idle_sent = FALSE;
  while (!t->done) {
    if (!idle_sent && !PeekMessageW(&msg, NULL, 0, 0, 0 /* PM_NOREMOVE */)) {      /* no input: the owner is told once, as Windows does */
      idle_sent = TRUE;
      MLevel_ *top = &t->lv[t->depth > 0 ? t->depth - 1 : 0];
      SendMessageW(t->owner, WM_ENTERIDLE_, MSGF_MENU_, (LONG_PTR)(top->hwnd ? top->hwnd : t->owner));
      continue;
    }
    idle_sent = FALSE;
    const int r = GetMessageW(&msg, NULL, 0, 0);
    if (r <= 0) { if (r == 0) PostQuitMessage((int)msg.wparam); break; }
    if (CallMsgFilterW(&msg, MSGF_MENU_)) continue;
    switch (msg.message) {
      case WM_MOUSEMOVE_: case WM_LBUTTONDOWN_: case WM_LBUTTONUP_:
        mt_mouse(t, msg.message, msg.pt.x, msg.pt.y);
        continue;
      case WM_RBUTTONDOWN_: case WM_RBUTTONUP_:
        if (t->flags & TPM_RIGHTBUTTON_) mt_mouse(t, msg.message, msg.pt.x, msg.pt.y); else if (msg.message == WM_RBUTTONDOWN_) t->done = TRUE;
        continue;
      case WM_KEYDOWN_: case WM_SYSKEYDOWN_:
        if (msg.wparam == VK_ESCAPE_ || msg.wparam == VK_UP_ || msg.wparam == VK_DOWN_ || msg.wparam == VK_LEFT_ || msg.wparam == VK_RIGHT_ || msg.wparam == VK_RETURN_ || msg.wparam == VK_HOME_ ||
            msg.wparam == VK_END_ || msg.wparam == VK_F10_ || msg.wparam == VK_MENU_) mt_key(t, (UINT)msg.wparam);
        else TranslateMessage(&msg);
        continue;
      case WM_CHAR_: case WM_SYSCHAR_: mt_char(t, (WCHAR)msg.wparam); continue;
      case 0x101 /* WM_KEYUP */: case WM_SYSKEYUP_: continue;
      default: break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  ReleaseCapture();
  if (old_capture) SetCapture(old_capture);
}

static void mt_finish(MTrack_ *t, BOOL popup_loop) {
  mt_notify_select(t, &t->lv[t->depth > 0 ? t->depth - 1 : 0], -1, FALSE);
  mt_close_to(t, 0);
  if (!(t->flags & TPM_NONOTIFY_)) SendMessageW(t->owner, WM_EXITMENULOOP_, popup_loop ? 1 : 0, 0);
}
/* the command of the chosen item as a message to the owner (not for TPM_RETURNCMD) */
static void mt_post_command(MTrack_ *t) {
  if (!t->have_result || (t->flags & (TPM_NONOTIFY_ | TPM_RETURNCMD_))) return;
  MENUINFO_ info = {sizeof info, MIM_STYLE_};
  if (GetMenuInfo(t->result_menu, &info) && (info.dwStyle & MNS_NOTIFYBYPOS_)) { PostMessageW(t->owner, WM_MENUCOMMAND_, (ULONG_PTR)t->result_index, (LONG_PTR)t->result_menu); return; }
  PostMessageW(t->owner, (t->sysmenu || (t->result_id >= 0xF000 && t->result_id < 0xFFFF && t->result_menu == GetSystemMenu(t->owner, FALSE))) ? WM_SYSCOMMAND_ : 0x111 /* WM_COMMAND */, t->result_id, 0);
}

EXPORT BOOL WINAPI EndMenu(void) {
  if (!g_mt) return FALSE;
  g_mt->done = TRUE;
  PostMessageW(g_mt->owner, 0 /* WM_NULL */, 0, 0);
  return TRUE;
}
static UINT track_popup(HANDLE menu, UINT flags, int x, int y, HANDLE owner, BOOL sysmenu) {
  if (!IsMenu(menu)) { SetLastError(ERROR_INVALID_MENU_HANDLE_); return 0; }
  if (!IsWindow(owner)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return 0; }
  if (g_mt && !(flags & TPM_RECURSE_)) { SetLastError(ERROR_POPUP_ALREADY_ACTIVE_); return 0; }
  MTrack_ *t = u32_alloc(sizeof *t);
  if (!t) return 0;
  for (size_t_ i = 0; i < sizeof *t; ++i) ((BYTE *)t)[i] = 0;
  MTrack_ *outer = g_mt;
  g_mt = t;
  t->owner = owner; t->flags = flags; t->sysmenu = sysmenu;
  if (!(flags & TPM_NONOTIFY_)) {
    SendMessageW(owner, WM_ENTERMENULOOP_, 1, 0);
    SendMessageW(owner, WM_INITMENU_, (ULONG_PTR)menu, 0);
  }
  /* the size decides the alignment, so build the view once for it */
  MView_ probe;
  int w = 0, h = 0;
  if (menu_view_build(menu, FALSE, owner, &probe)) {
    MRect_ rc = {0, 0, probe.width, probe.height};
    AdjustWindowRectEx(&rc, 0x80000000u | 0x00800000u, FALSE, 0x88);
    w = rc.right - rc.left; h = rc.bottom - rc.top;
    menu_view_free(&probe);
  }
  if (flags & TPM_RIGHTALIGN_) x -= w; else if (flags & TPM_CENTERALIGN_) x -= w / 2;
  if (flags & TPM_BOTTOMALIGN_) y -= h; else if (flags & TPM_VCENTERALIGN_) y -= h / 2;
  UINT result = 0;
  if (mt_open_popup(t, menu, x, y, -1, 0) >= 0) {
    mt_loop(t);
    mt_finish(t, TRUE);
    mt_post_command(t);
    result = (flags & TPM_RETURNCMD_) ? (t->have_result ? t->result_id : 0) : 1;
  } else {
    if (!(flags & TPM_NONOTIFY_)) SendMessageW(owner, WM_EXITMENULOOP_, 1, 0);
  }
  g_mt = outer;
  u32_free(t);
  return result;
}
EXPORT BOOL WINAPI TrackPopupMenu(HANDLE menu, UINT flags, int x, int y, int reserved, HANDLE owner, const RECT_ *rect) {
  (void)reserved; (void)rect;
  return track_popup(menu, flags, x, y, owner, FALSE);
}
EXPORT BOOL WINAPI TrackPopupMenuEx(HANDLE menu, UINT flags, int x, int y, HANDLE owner, void *params) { (void)params; return track_popup(menu, flags, x, y, owner, FALSE); }

/* ---- the menu bar of a window ------------------------------------------------------------------------------------------------------------------------------------- */
#define WS_CHILD_MENU_ 0x40000000
/* The bar of a top-level window in screen coordinates: the strip above the client area. FALSE when the window has no menu. */
static BOOL menu_bar_rect(HANDLE hwnd, MRect_ *screen, HANDLE *menu_out) {
  if (!IsWindow(hwnd) || (GetWindowLongW(hwnd, -16 /* GWL_STYLE */) & WS_CHILD_MENU_)) return FALSE;
  HANDLE menu = GetMenu(hwnd);
  if (!menu || !IsMenu(menu)) return FALSE;
  RECT_ cr;
  POINT_ origin = {0, 0};
  if (!GetClientRect(hwnd, &cr) || !ClientToScreen(hwnd, &origin)) return FALSE;
  const int cy = GetSystemMetrics(15 /* SM_CYMENU */);
  screen->left = origin.x; screen->right = origin.x + (cr.right - cr.left); screen->top = origin.y - cy; screen->bottom = origin.y;
  if (menu_out) *menu_out = menu;
  return TRUE;
}
/* MFT_RIGHTJUSTIFY: that item and the ones after it end at the right edge of a bar `width` wide */
static void menu_bar_justify(MView_ *v, int width) {
  for (int i = 0; i < v->count; ++i) {
    if (!(v->items[i].type & MFT_RIGHTJUSTIFY_)) continue;
    int total = 0;
    for (int j = i; j < v->count; ++j) total += v->items[j].w;
    int x = width - total;
    if (x < v->items[i].x) x = v->items[i].x;
    for (int j = i; j < v->count; ++j) { v->items[j].x = x; x += v->items[j].w; }
    break;
  }
}
static void menu_bar_draw(HANDLE hwnd, int sel) {
  MRect_ br;
  HANDLE menu;
  RECT_ wr;
  if (!menu_bar_rect(hwnd, &br, &menu) || !GetWindowRect(hwnd, &wr) || !IsWindowVisible(hwnd)) return;
  MView_ v;
  if (!menu_view_build(menu, TRUE, hwnd, &v)) return;
  menu_bar_justify(&v, br.right - br.left);
  HANDLE hdc = GetWindowDC(hwnd);
  if (hdc) {
    SaveDC(hdc);
    SetViewportOrgEx(hdc, br.left - wr.left, br.top - wr.top, NULL);
    IntersectClipRect(hdc, 0, 0, br.right - br.left, br.bottom - br.top);
    menu_paint(hdc, &v, sel, hwnd, menu, br.right - br.left);
    RestoreDC(hdc, -1);
    ReleaseDC(hwnd, hdc);
  }
  menu_view_free(&v);
}
EXPORT BOOL WINAPI DrawMenuBar(HANDLE hwnd) {
  if (!(NtUserDrawMenuBar((ULONG_PTR)hwnd) & 0xFFFFFFFFu)) return FALSE;
  menu_bar_draw(hwnd, g_mt && g_mt->bar_hwnd == hwnd ? g_mt->lv[0].sel : -1);
  return TRUE;
}

/* the modal loop of the bar: `first` is the item to highlight (-1: none), `open` opens its popup */
static void menu_bar_track(HANDLE hwnd, int first, BOOL open, BOOL mouse) {
  MRect_ br;
  HANDLE menu;
  if (g_mt || !menu_bar_rect(hwnd, &br, &menu)) return;
  MTrack_ *t = u32_alloc(sizeof *t);
  if (!t) return;
  for (size_t_ i = 0; i < sizeof *t; ++i) ((BYTE *)t)[i] = 0;
  g_mt = t;
  t->owner = hwnd; t->bar_hwnd = hwnd; t->keyboard = !mouse; t->down_seen = mouse;
  SendMessageW(hwnd, WM_ENTERMENULOOP_, 0, 0);
  SendMessageW(hwnd, WM_INITMENU_, (ULONG_PTR)menu, 0);
  MLevel_ *lv = &t->lv[0];
  if (menu_view_build(menu, TRUE, hwnd, &lv->view)) {
    menu_bar_justify(&lv->view, br.right - br.left);
    lv->menu = menu; lv->bar = TRUE; lv->sel = -1; lv->x = br.left; lv->y = br.top;
    t->depth = 1;
    if (first >= 0 && first < lv->view.count) {
      mt_select(t, 0, first, mouse);
      if (open) {
        if (lv->view.items[first].sub) { mt_open_sub(t, 0); if (!mouse && t->depth > 1) mt_select(t, 1, mt_step(&t->lv[1].view, -1, 1), FALSE); }
        else if (!(lv->view.items[first].state & MFS_GRAYED_)) mt_choose(t, 0, first);
      }
    }
    mt_loop(t);
    mt_finish(t, FALSE);
    mt_post_command(t);
  } else {
    SendMessageW(hwnd, WM_EXITMENULOOP_, 0, 0);
  }
  g_mt = NULL;
  u32_free(t);
  menu_bar_draw(hwnd, -1);
}
static void menu_bar_track_mouse(HANDLE hwnd, LONG_PTR lparam) {
  MRect_ br;
  HANDLE menu;
  if (!menu_bar_rect(hwnd, &br, &menu)) return;
  MView_ v;
  int index = -1;
  if (menu_view_build(menu, TRUE, hwnd, &v)) {
    menu_bar_justify(&v, br.right - br.left);
    index = menu_view_hit(&v, (int)(short)(lparam & 0xFFFF) - br.left, (int)(short)((lparam >> 16) & 0xFFFF) - br.top);
    menu_view_free(&v);
  }
  menu_bar_track(hwnd, index, TRUE, TRUE);
}

/* ---- the system menu ----------------------------------------------------------------------------------------------------------------------------------------------------- */
static void sysmenu_update(HANDLE hwnd, HANDLE menu) {
  const DWORD style = (DWORD)GetWindowLongW(hwnd, -16);
  const BOOL max = (style & 0x01000000) != 0, min = (style & 0x20000000) != 0;
  EnableMenuItem(menu, 0xF120, 0 | (max || min ? 0 : 3));      /* Restore */
  EnableMenuItem(menu, 0xF010, 0 | (max || min ? 3 : 0));      /* Move */
  EnableMenuItem(menu, 0xF000, 0 | ((style & 0x40000) && !max && !min ? 0 : 3));      /* Size */
  EnableMenuItem(menu, 0xF020, 0 | ((style & 0x20000) && !min ? 0 : 3));      /* Minimize */
  EnableMenuItem(menu, 0xF030, 0 | ((style & 0x10000) && !max ? 0 : 3));      /* Maximize */
}
static void menu_sys_track(HANDLE hwnd, int x, int y, BOOL right_button) {
  HANDLE menu = GetSystemMenu(hwnd, FALSE);
  if (!menu || g_mt) return;
  sysmenu_update(hwnd, menu);
  const UINT cmd = track_popup(menu, TPM_RETURNCMD_ | (right_button ? TPM_RIGHTBUTTON_ : 0), x, y, hwnd, TRUE);
  if (cmd) PostMessageW(hwnd, WM_SYSCOMMAND_, cmd, 0);
}

/* WM_SYSCOMMAND with SC_KEYMENU: lparam is the character typed with Alt (0: Alt or F10 alone, ' ': the system menu) */
static void menu_key_menu(HANDLE hwnd, LONG_PTR ch) {
  if (ch == L' ') {
    RECT_ wr;
    if ((GetWindowLongW(hwnd, -16) & 0x00080000 /* WS_SYSMENU */) && GetWindowRect(hwnd, &wr)) menu_sys_track(hwnd, wr.left, wr.top + GetSystemMetrics(4 /* SM_CYCAPTION */), FALSE);
    return;
  }
  MRect_ br;
  HANDLE menu;
  if (!menu_bar_rect(hwnd, &br, &menu)) return;
  if (ch == 0) { menu_bar_track(hwnd, 0, FALSE, FALSE); return; }
  MView_ v;
  int index = -1;
  BOOL unique = FALSE;
  if (menu_view_build(menu, TRUE, hwnd, &v)) { index = mt_mnemonic(&v, -1, (WCHAR)ch, &unique); menu_view_free(&v); }
  if (index >= 0) menu_bar_track(hwnd, index, TRUE, FALSE);
}

/* DefWindowProc: what the menu bar adds to a window's default processing. TRUE: the message is done (*result). */
static BOOL menu_def_before(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, LONG_PTR *result) {
  *result = 0;
  switch (message) {
    case WM_SYSKEYDOWN_:
      if (wparam == VK_MENU_) { g_alt_alone = TRUE; return FALSE; }
      g_alt_alone = FALSE;
      if (wparam == VK_F10_) { SendMessageW(hwnd, WM_SYSCOMMAND_, SC_KEYMENU_, 0); return TRUE; }
      return FALSE;
    case WM_KEYDOWN_: g_alt_alone = FALSE; return FALSE;
    case WM_SYSKEYUP_:
      if (wparam == VK_MENU_ && g_alt_alone) { g_alt_alone = FALSE; SendMessageW(hwnd, WM_SYSCOMMAND_, SC_KEYMENU_, 0); return TRUE; }
      return FALSE;
    case WM_SYSCHAR_:
      g_alt_alone = FALSE;
      if (wparam == VK_RETURN_ || wparam == 0x1B) return FALSE;
      SendMessageW(hwnd, WM_SYSCOMMAND_, SC_KEYMENU_, (LONG_PTR)wparam);
      return TRUE;
    case WM_SYSCOMMAND_:
      if ((wparam & 0xFFF0) == (SC_KEYMENU_ & 0xFFF0)) { menu_key_menu(hwnd, lparam); return TRUE; }
      return FALSE;
    case WM_CANCELMODE_: if (g_mt) EndMenu(); return FALSE;
    case WM_NCLBUTTONDOWN_:
      if (wparam == HTMENU_) { menu_bar_track_mouse(hwnd, lparam); return TRUE; }
      return FALSE;
    case WM_NCRBUTTONUP_:
      if ((wparam == HTCAPTION_ || wparam == HTSYSMENU_) && (GetWindowLongW(hwnd, -16) & 0x00080000)) { menu_sys_track(hwnd, (int)(short)(lparam & 0xFFFF), (int)(short)((lparam >> 16) & 0xFFFF), TRUE); return TRUE; }
      return FALSE;
    default: return FALSE;
  }
}
static void menu_def_after(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, LONG_PTR *result) {
  (void)wparam;
  if (message == WM_NCPAINT_ || message == WM_NCACTIVATE_) menu_bar_draw(hwnd, g_mt && g_mt->bar_hwnd == hwnd ? g_mt->lv[0].sel : -1);
  else if (message == WM_NCHITTEST_ && *result != 0 /* HTNOWHERE aside */) {
    MRect_ br;
    const int x = (int)(short)(lparam & 0xFFFF), y = (int)(short)((lparam >> 16) & 0xFFFF);
    if (menu_bar_rect(hwnd, &br, NULL) && x >= br.left && x < br.right && y >= br.top && y < br.bottom) *result = HTMENU_;
  }
}

/* ---- geometry queries ------------------------------------------------------------------------------------------------------------------------------------------------ */
EXPORT BOOL WINAPI GetMenuItemRect(HANDLE hwnd, HANDLE menu, UINT item, RECT_ *rect) {
  if (!rect) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!IsMenu(menu)) { SetLastError(ERROR_INVALID_MENU_HANDLE_); return FALSE; }
  if (g_mt) {
    for (int d = 0; d < g_mt->depth; ++d) {
      MLevel_ *lv = &g_mt->lv[d];
      if (lv->menu != menu || (int)item >= lv->view.count) continue;
      const MItem_ *it = &lv->view.items[item];
      rect->left = lv->x + it->x; rect->top = lv->y + it->y; rect->right = rect->left + it->w; rect->bottom = rect->top + it->h;
      return TRUE;
    }
  }
  for (int n = 1; n <= 8; ++n) {      /* a popup that was shown: its place is kept */
    const int slot = (g_popup_place_next - n) & 7;
    if (g_popup_place[slot].menu != menu) continue;
    MView_ v;
    BOOL ok = FALSE;
    if (menu_view_build(menu, FALSE, hwnd, &v)) {
      if ((int)item < v.count) {
        const MItem_ *it = &v.items[item];
        rect->left = g_popup_place[slot].x + it->x; rect->top = g_popup_place[slot].y + it->y; rect->right = rect->left + it->w; rect->bottom = rect->top + it->h;
        ok = TRUE;
      }
      menu_view_free(&v);
    }
    if (ok) return TRUE;
    break;
  }
  MRect_ br;
  HANDLE bar;
  if (hwnd && menu_bar_rect(hwnd, &br, &bar) && bar == menu) {
    MView_ v;
    BOOL ok = FALSE;
    if (menu_view_build(menu, TRUE, hwnd, &v)) {
      menu_bar_justify(&v, br.right - br.left);
      if ((int)item < v.count) {
        const MItem_ *it = &v.items[item];
        rect->left = br.left + it->x; rect->top = br.top + it->y; rect->right = rect->left + it->w; rect->bottom = rect->top + it->h;
        ok = TRUE;
      }
      menu_view_free(&v);
    }
    if (ok) return TRUE;
  }
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}
EXPORT int WINAPI MenuItemFromPoint(HANDLE hwnd, HANDLE menu, MPoint_ pt) {
  if (g_mt) {
    for (int d = 0; d < g_mt->depth; ++d) {
      MLevel_ *lv = &g_mt->lv[d];
      if (lv->menu == menu) return menu_view_hit(&lv->view, pt.x - lv->x, pt.y - lv->y);
    }
  }
  MRect_ br;
  HANDLE bar;
  if (hwnd && menu_bar_rect(hwnd, &br, &bar) && bar == menu) {
    MView_ v;
    int index = -1;
    if (menu_view_build(menu, TRUE, hwnd, &v)) {
      menu_bar_justify(&v, br.right - br.left);
      index = menu_view_hit(&v, pt.x - br.left, pt.y - br.top);
      menu_view_free(&v);
    }
    return index;
  }
  return -1;
}
typedef struct { DWORD cbSize; MRect_ rcBar; HANDLE hMenu, hwndMenu; DWORD flags; } MENUBARINFO_;
EXPORT BOOL WINAPI GetMenuBarInfo(HANDLE hwnd, LONG object, LONG item, MENUBARINFO_ *info) {
  if (!info || info->cbSize != sizeof *info || item < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; }
  MRect_ br;
  HANDLE menu;
  if (object == -3 /* OBJID_MENU */) {
    if (!menu_bar_rect(hwnd, &br, &menu)) return FALSE;
    info->rcBar = br;
    info->hMenu = menu; info->hwndMenu = NULL;
    info->flags = (g_mt && g_mt->bar_hwnd == hwnd) ? 1 : 0;
    if (item > 0) {
      RECT_ r;
      if (!GetMenuItemRect(hwnd, menu, (UINT)(item - 1), &r)) return FALSE;
      info->rcBar.left = r.left; info->rcBar.top = r.top; info->rcBar.right = r.right; info->rcBar.bottom = r.bottom;
    }
    return TRUE;
  }
  if (object == -1 /* OBJID_SYSMENU */) {
    RECT_ wr;
    menu = GetSystemMenu(hwnd, FALSE);
    if (!menu || !GetWindowRect(hwnd, &wr)) return FALSE;
    const int cy = GetSystemMetrics(4);
    info->rcBar.left = wr.left; info->rcBar.top = wr.top; info->rcBar.right = wr.left + cy; info->rcBar.bottom = wr.top + cy;
    info->hMenu = menu; info->hwndMenu = NULL; info->flags = 0;
    return TRUE;
  }
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}
EXPORT DWORD WINAPI CalcMenuBar(HANDLE hwnd, DWORD left, DWORD right, DWORD top, RECT_ *rect) {
  (void)hwnd; (void)left; (void)right; (void)top;
  const int cy = GetSystemMetrics(15);
  if (rect) rect->bottom = rect->top + cy;
  return (DWORD)cy;
}
EXPORT DWORD WINAPI DrawMenuBarTemp(HANDLE hwnd, HANDLE hdc, RECT_ *rect, HANDLE menu, HANDLE font) {
  (void)font;
  if (!rect || !hdc || !IsMenu(menu)) return 0;
  MView_ v;
  if (!menu_view_build(menu, TRUE, hwnd, &v)) return 0;
  menu_bar_justify(&v, rect->right - rect->left);
  SaveDC(hdc);
  SetViewportOrgEx(hdc, rect->left, rect->top, NULL);
  IntersectClipRect(hdc, 0, 0, rect->right - rect->left, rect->bottom - rect->top);
  menu_paint(hdc, &v, -1, hwnd, menu, rect->right - rect->left);
  RestoreDC(hdc, -1);
  const DWORD height = (DWORD)v.height;
  menu_view_free(&v);
  return height;
}
