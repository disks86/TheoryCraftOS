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

/* The window system of user32.dll over win32k: classes, window creation with its messages (window procedures run in this process through the upcalls of win32u.dll), the tree,
 * styles, text, properties, posted and sent messages, the message loop, timers, enumeration, show/hide and placement. Prints "Test failed" per wrong answer (scripts/winetest.sh
 * counts them), exit code = failures. Build: tests/windows/build.sh. Run: copy the exe to build/winetests/user32_tcos.exe and scripts/winetest.sh user32 tcos. */
#define _WIN32_WINNT 0x0A00      /* (touch injection and pointer messages) */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { ++failures; printf("user32test.c:%d: Test failed: %s\n", __LINE__, #c); } } while (0)

#define MAX_LOG 64
static UINT g_log[MAX_LOG];
static int g_log_count;
static void log_message(UINT m) { if (g_log_count < MAX_LOG) g_log[g_log_count++] = m; }
static int log_has(UINT m) { for (int i = 0; i < g_log_count; ++i) if (g_log[i] == m) return 1; return 0; }
static int log_index(UINT m) { for (int i = 0; i < g_log_count; ++i) if (g_log[i] == m) return i; return -1; }

static LRESULT CALLBACK test_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCREATE: case WM_CREATE: case WM_SIZE: case WM_MOVE: case WM_SHOWWINDOW: case WM_DESTROY: case WM_NCDESTROY: case WM_USER + 1: case WM_USER + 2:
      log_message(message);
      if (message == WM_CREATE) { CREATESTRUCTA *cs = (CREATESTRUCTA *)lparam; if (cs->lpCreateParams) *(HWND *)cs->lpCreateParams = hwnd; }
      if (message == WM_USER + 2) return wparam * 2 + lparam;
      break;
    case WM_TIMER: log_message(WM_TIMER); PostQuitMessage(7); return 0;
  }
  return DefWindowProcA(hwnd, message, wparam, lparam);
}

static void pump(void) { MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); } }

static BOOL CALLBACK count_windows(HWND hwnd, LPARAM param) { (void)hwnd; ++*(int *)param; return TRUE; }

static DWORD WINAPI sender(void *param) { return (DWORD)SendMessageA((HWND)param, WM_USER + 2, 10, 5); }


/* ---- the controls ------------------------------------------------------------------------------------------------------------------------------------------------ */
static int g_commands, g_last_code, g_last_id, g_colors_static, g_colors_btn, g_colors_edit;
static LRESULT CALLBACK parent_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_COMMAND: ++g_commands; g_last_id = LOWORD(wparam); g_last_code = HIWORD(wparam); return 0;
    case WM_CTLCOLORSTATIC: ++g_colors_static; break;
    case WM_CTLCOLORBTN: ++g_colors_btn; break;
    case WM_CTLCOLOREDIT: ++g_colors_edit; break;
  }
  return DefWindowProcA(hwnd, message, wparam, lparam);
}

static void test_controls(HINSTANCE instance) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = parent_proc; wc.hInstance = instance; wc.lpszClassName = "TcosParentClass";
  CHECK(RegisterClassA(&wc));
  HWND parent = CreateWindowExA(0, "TcosParentClass", "parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 300, 200, NULL, NULL, instance, NULL);
  CHECK(parent != NULL);
  char text[64];

  /* Static: text, the etched types and notification styles */
  HWND st = CreateWindowA("static", "Hello", WS_CHILD | WS_VISIBLE, 5, 5, 100, 20, parent, (HMENU)10, instance, NULL);
  CHECK(st && GetWindowTextA(st, text, sizeof text) == 5 && !strcmp(text, "Hello"));
  CHECK(SendMessageA(st, WM_GETDLGCODE, 0, 0) == DLGC_STATIC);
  CHECK(SendMessageA(st, WM_NCHITTEST, 0, 0) == HTTRANSPARENT);
  CHECK(SendMessageA(st, WM_SETFONT, 0xcafe, 0) == 0 && SendMessageA(st, WM_GETFONT, 0, 0) == 0xcafe);
  CHECK(SendMessageA(st, STM_SETIMAGE, IMAGE_BITMAP, 0x1234) == 0 && SendMessageA(st, STM_GETIMAGE, IMAGE_BITMAP, 0) == 0x1234);
  HWND etched = CreateWindowA("static", "", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 40, 100, 10, parent, (HMENU)11, instance, NULL);
  RECT r;
  CHECK(etched && GetWindowLongA(etched, GWL_EXSTYLE) == WS_EX_STATICEDGE && GetClientRect(etched, &r) && r.bottom == 0 && r.right > 0);
  g_colors_static = 0;
  UpdateWindow(parent);
  InvalidateRect(st, NULL, FALSE);
  UpdateWindow(st);
  CHECK(g_colors_static >= 1);      /* the static control asked its parent for its colours while painting */

  /* Button: click, check states, auto check box and radio group, notifications */
  HWND push = CreateWindowA("button", "OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 5, 60, 60, 20, parent, (HMENU)20, instance, NULL);
  CHECK(push && (SendMessageA(push, WM_GETDLGCODE, 0, 0) & DLGC_UNDEFPUSHBUTTON));
  g_commands = 0;
  SendMessageA(push, BM_CLICK, 0, 0);
  CHECK(g_commands == 1 && g_last_id == 20 && g_last_code == BN_CLICKED);
  CHECK(!(SendMessageA(push, BM_GETSTATE, 0, 0) & BST_PUSHED));
  SendMessageA(push, BM_SETSTATE, TRUE, 0);
  CHECK(SendMessageA(push, BM_GETSTATE, 0, 0) & BST_PUSHED);
  SendMessageA(push, BM_SETSTATE, FALSE, 0);
  HWND check = CreateWindowA("button", "Check", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 5, 90, 80, 20, parent, (HMENU)21, instance, NULL);
  CHECK(SendMessageA(check, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
  SendMessageA(check, BM_CLICK, 0, 0);
  CHECK(SendMessageA(check, BM_GETCHECK, 0, 0) == BST_CHECKED);
  SendMessageA(check, BM_CLICK, 0, 0);
  CHECK(SendMessageA(check, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
  SendMessageA(check, BM_SETCHECK, BST_INDETERMINATE, 0);
  CHECK(SendMessageA(check, BM_GETCHECK, 0, 0) == BST_UNCHECKED);      /* (a two-state box has no third state) */
  HWND tri = CreateWindowA("button", "Tri", WS_CHILD | WS_VISIBLE | BS_AUTO3STATE, 5, 110, 80, 20, parent, (HMENU)22, instance, NULL);
  SendMessageA(tri, BM_SETCHECK, BST_INDETERMINATE, 0);
  CHECK(SendMessageA(tri, BM_GETCHECK, 0, 0) == BST_INDETERMINATE);
  HWND radio1 = CreateWindowA("button", "A", WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON, 100, 60, 50, 20, parent, (HMENU)30, instance, NULL);
  HWND radio2 = CreateWindowA("button", "B", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 100, 80, 50, 20, parent, (HMENU)31, instance, NULL);
  SendMessageA(radio1, BM_SETCHECK, BST_CHECKED, 0);
  CHECK(GetWindowLongA(radio1, GWL_STYLE) & WS_TABSTOP);
  SendMessageA(radio2, BM_CLICK, 0, 0);
  CHECK(SendMessageA(radio2, BM_GETCHECK, 0, 0) == BST_CHECKED && SendMessageA(radio1, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
  CHECK(!(GetWindowLongA(radio1, GWL_STYLE) & WS_TABSTOP) && (GetWindowLongA(radio2, GWL_STYLE) & WS_TABSTOP));
  g_colors_btn = 0;
  InvalidateRect(push, NULL, FALSE);
  UpdateWindow(push);
  CHECK(g_colors_btn >= 1);

  /* Edit: text, selection, replacement, limits, notifications, lines, key input */
  HWND edit = CreateWindowA("edit", "abc", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 5, 140, 120, 20, parent, (HMENU)40, instance, NULL);
  CHECK(edit && GetWindowTextA(edit, text, sizeof text) == 3 && !strcmp(text, "abc"));
  CHECK(SendMessageA(edit, WM_GETTEXTLENGTH, 0, 0) == 3);
  g_commands = 0;
  SendMessageA(edit, EM_SETSEL, 1, 2);
  DWORD a = 9, b = 9;
  SendMessageA(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
  CHECK(a == 1 && b == 2);
  SendMessageA(edit, EM_REPLACESEL, 0, (LPARAM)"XYZ");
  CHECK(GetWindowTextA(edit, text, sizeof text) == 5 && !strcmp(text, "aXYZc"));
  CHECK(g_commands == 2 && g_last_code == EN_CHANGE);      /* EN_UPDATE, EN_CHANGE */
  SendMessageA(edit, EM_SETSEL, 5, 5);
  g_commands = 0;
  SendMessageA(edit, WM_CHAR, 'd', 0);
  CHECK(GetWindowTextA(edit, text, sizeof text) == 6 && !strcmp(text, "aXYZcd") && g_commands == 2);
  SendMessageA(edit, WM_CHAR, 8, 0);      /* backspace */
  CHECK(GetWindowTextA(edit, text, sizeof text) == 5 && !strcmp(text, "aXYZc"));
  SendMessageA(edit, EM_SETLIMITTEXT, 6, 0);
  CHECK(SendMessageA(edit, EM_GETLIMITTEXT, 0, 0) == 6);
  g_commands = 0;
  SendMessageA(edit, EM_SETSEL, 5, 5);
  SendMessageA(edit, EM_REPLACESEL, 0, (LPARAM)"12345");
  CHECK(SendMessageA(edit, WM_GETTEXTLENGTH, 0, 0) == 6 && g_commands == 3);      /* EN_MAXTEXT, EN_UPDATE, EN_CHANGE */
  CHECK(SendMessageA(edit, EM_GETMODIFY, 0, 0) == TRUE);
  SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
  CHECK(SendMessageA(edit, EM_GETMODIFY, 0, 0) == FALSE);
  SendMessageA(edit, EM_SETREADONLY, TRUE, 0);
  SendMessageA(edit, WM_CHAR, 'q', 0);
  CHECK(SendMessageA(edit, WM_GETTEXTLENGTH, 0, 0) == 6);
  CHECK(GetWindowLongA(edit, GWL_STYLE) & ES_READONLY);
  CHECK(SendMessageA(edit, WM_GETDLGCODE, 0, 0) == (DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS));
  HWND multi = CreateWindowA("edit", "one\r\ntwo\r\nthree", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 130, 140, 100, 40, parent, (HMENU)41, instance, NULL);
  CHECK(SendMessageA(multi, EM_GETLINECOUNT, 0, 0) == 3);
  CHECK(SendMessageA(multi, EM_LINEINDEX, 1, 0) == 5 && SendMessageA(multi, EM_LINEINDEX, 2, 0) == 10 && SendMessageA(multi, EM_LINEINDEX, 3, 0) == -1);
  CHECK(SendMessageA(multi, EM_LINELENGTH, 5, 0) == 3 && SendMessageA(multi, EM_LINEFROMCHAR, 12, 0) == 2);
  char line[16] = {8, 0};
  CHECK(SendMessageA(multi, EM_GETLINE, 2, (LPARAM)line) == 5 && !memcmp(line, "three", 5));
  CHECK(SendMessageA(multi, WM_GETDLGCODE, 0, 0) & DLGC_WANTALLKEYS);
  SendMessageA(multi, WM_SETTEXT, 0, (LPARAM)"x");
  CHECK(SendMessageA(multi, EM_GETLINECOUNT, 0, 0) == 1);
  SendMessageA(multi, EM_SETSEL, 1, 1);
  SendMessageA(multi, WM_CHAR, '\r', 0);
  SendMessageA(multi, WM_CHAR, 'y', 0);
  CHECK(GetWindowTextA(multi, text, sizeof text) == 4 && !memcmp(text, "x\r\ny", 4) && SendMessageA(multi, EM_GETLINECOUNT, 0, 0) == 2);
  g_colors_edit = 0;
  InvalidateRect(edit, NULL, FALSE);
  UpdateWindow(edit);
  CHECK(g_colors_edit + g_colors_static >= 1);

  CHECK(DestroyWindow(parent));
  CHECK(!IsWindow(edit) && !IsWindow(push));
}

static void test_icons_and_carets(HINSTANCE instance) {
  /* system cursors and icons */
  HCURSOR arrow = LoadCursorA(NULL, IDC_ARROW);
  CHECK(arrow != NULL && LoadCursorA(NULL, IDC_ARROW) == arrow && LoadCursorA(NULL, IDC_IBEAM) != arrow);
  CHECK(LoadIconA(NULL, IDI_APPLICATION) != NULL);
  HCURSOR old = SetCursor(arrow);
  (void)old;
  CHECK(GetCursor() == arrow);
  int count = ShowCursor(TRUE);
  CHECK(ShowCursor(FALSE) == count - 1);

  /* an icon made from bits: 8x8, 1 bit colour, AND mask and XOR image */
  BYTE and_bits[8], xor_bits[8];
  memset(and_bits, 0xFF, sizeof and_bits);
  memset(xor_bits, 0x00, sizeof xor_bits);
  and_bits[0] = 0x00;
  xor_bits[0] = 0xFF;
  HICON icon = CreateIcon(instance, 8, 8, 1, 4, and_bits, xor_bits);
  ICONINFO info;
  if (!icon) icon = CreateIcon(instance, 8, 8, 1, 1, and_bits, xor_bits);
  CHECK(icon != NULL);
  if (icon) {
    CHECK(GetIconInfo(icon, &info) && info.fIcon && info.hbmMask != NULL);
    BITMAP bm;
    CHECK(GetObjectA(info.hbmMask, sizeof bm, &bm) && bm.bmWidth == 8);
    DeleteObject(info.hbmMask);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    HICON copy = CopyIcon(icon);
    CHECK(copy != NULL && copy != icon);
    if (copy) CHECK(DestroyIcon(copy));
    HDC dc = GetDC(NULL);
    CHECK(DrawIconEx(dc, 0, 0, icon, 8, 8, 0, NULL, DI_NORMAL));
    ReleaseDC(NULL, dc);
    CHECK(DestroyIcon(icon));
  }
  CHECK(!GetIconInfo((HICON)(ULONG_PTR)0x1234, &info));

  /* carets belong to the thread's windows */
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = DefWindowProcA; wc.hInstance = instance; wc.lpszClassName = "TcosCaretClass";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("TcosCaretClass", "c", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 100, 100, NULL, NULL, instance, NULL);
  CHECK(CreateCaret(hwnd, NULL, 2, 12));
  CHECK(SetCaretPos(10, 20));
  POINT p;
  CHECK(GetCaretPos(&p) && p.x == 10 && p.y == 20);
  CHECK(ShowCaret(hwnd) && HideCaret(hwnd));
  CHECK(DestroyCaret());
  CHECK(!SetCaretPos(1, 1));
  CHECK(DestroyWindow(hwnd));
  UnregisterClassA("TcosCaretClass", instance);
}

static int g_scroll_codes[16], g_scroll_count;
static LRESULT CALLBACK scroll_parent_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_VSCROLL && g_scroll_count < 16) g_scroll_codes[g_scroll_count++] = LOWORD(wparam);
  return DefWindowProcA(hwnd, message, wparam, lparam);
}

static void test_scroll_bars(HINSTANCE instance) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = scroll_parent_proc; wc.hInstance = instance; wc.lpszClassName = "TcosScrollParent";
  RegisterClassA(&wc);
  HWND parent = CreateWindowA("TcosScrollParent", "s", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL, 0, 0, 200, 200, NULL, NULL, instance, NULL);
  /* the bars of a window */
  SCROLLINFO si = {sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, 100, 10, 30, 0};
  CHECK(SetScrollInfo(parent, SB_VERT, &si, TRUE) == 30);
  SCROLLINFO got = {sizeof got, SIF_ALL, 0, 0, 0, 0, 0};
  CHECK(GetScrollInfo(parent, SB_VERT, &got) && got.nMin == 0 && got.nMax == 100 && got.nPage == 10 && got.nPos == 30);
  CHECK(SetScrollPos(parent, SB_VERT, 500, TRUE) == 30 && GetScrollPos(parent, SB_VERT) == 91);      /* (the position stops at max - page + 1) */
  int lo = -1, hi = -1;
  CHECK(SetScrollRange(parent, SB_VERT, 5, 50, TRUE) && GetScrollRange(parent, SB_VERT, &lo, &hi) && lo == 5 && hi == 50);
  /* a scroll bar control keeps its own state and reports to its parent */
  HWND bar = CreateWindowA("scrollbar", "", WS_CHILD | WS_VISIBLE | SBS_VERT, 150, 0, 16, 100, parent, (HMENU)5, instance, NULL);
  CHECK(bar != NULL);
  SendMessageA(bar, SBM_SETRANGE, 0, 100);
  CHECK(SendMessageA(bar, SBM_SETPOS, 40, TRUE) == 0 && SendMessageA(bar, SBM_GETPOS, 0, 0) == 40);
  int a = -1, b = -1;
  SendMessageA(bar, SBM_GETRANGE, (WPARAM)&a, (LPARAM)&b);
  CHECK(a == 0 && b == 100);
  CHECK(GetScrollPos(bar, SB_CTL) == 40);
  SCROLLINFO ci = {sizeof ci, SIF_PAGE | SIF_POS, 0, 0, 20, 90, 0};
  CHECK(SetScrollInfo(bar, SB_CTL, &ci, TRUE) == 81);      /* (page 20: the top is 100 - 19) */
  g_scroll_count = 0;
  SendMessageA(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 4));      /* the upper arrow */
  SendMessageA(bar, WM_LBUTTONUP, 0, MAKELPARAM(8, 4));
  CHECK(g_scroll_count == 2 && g_scroll_codes[0] == SB_LINEUP && g_scroll_codes[1] == SB_ENDSCROLL);
  CHECK(EnableScrollBar(bar, SB_CTL, ESB_DISABLE_BOTH));
  CHECK(DestroyWindow(parent));
  UnregisterClassA("TcosScrollParent", instance);
}

static int g_list_notes[16], g_list_note_count;
static LRESULT CALLBACK list_parent_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_COMMAND && g_list_note_count < 16) g_list_notes[g_list_note_count++] = HIWORD(wparam);
  return DefWindowProcA(hwnd, message, wparam, lparam);
}

static void test_list_and_combo(HINSTANCE instance) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = list_parent_proc; wc.hInstance = instance; wc.lpszClassName = "TcosListParent";
  RegisterClassA(&wc);
  HWND parent = CreateWindowA("TcosListParent", "l", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 400, 300, NULL, NULL, instance, NULL);
  char text[64];

  /* list box: strings, data, sorting, selection and notifications */
  HWND list = CreateWindowA("listbox", "", WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_SORT | LBS_NOINTEGRALHEIGHT, 5, 5, 100, 100, parent, (HMENU)1, instance, NULL);
  CHECK(list != NULL);
  CHECK(SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"pear") == 0);
  CHECK(SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"apple") == 0);      /* (sorted: before pear) */
  CHECK(SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"zebra") == 2);
  CHECK(SendMessageA(list, LB_GETCOUNT, 0, 0) == 3);
  CHECK(SendMessageA(list, LB_GETTEXTLEN, 1, 0) == 4 && SendMessageA(list, LB_GETTEXT, 1, (LPARAM)text) == 4 && !strcmp(text, "pear"));
  CHECK(SendMessageA(list, LB_SETITEMDATA, 1, 77) && SendMessageA(list, LB_GETITEMDATA, 1, 0) == 77);
  CHECK(SendMessageA(list, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"ze") == 2 && SendMessageA(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)"APPLE") == 0);
  CHECK(SendMessageA(list, LB_GETCURSEL, 0, 0) == LB_ERR);
  CHECK(SendMessageA(list, LB_SETCURSEL, 2, 0) == 2 && SendMessageA(list, LB_GETCURSEL, 0, 0) == 2 && SendMessageA(list, LB_GETSEL, 2, 0) > 0);
  CHECK(SendMessageA(list, LB_SETCURSEL, 9, 0) == LB_ERR);
  g_list_note_count = 0;
  SendMessageA(list, WM_KEYDOWN, VK_UP, 0);
  CHECK(SendMessageA(list, LB_GETCURSEL, 0, 0) == 1 && g_list_note_count == 1 && g_list_notes[0] == LBN_SELCHANGE);
  SendMessageA(list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 2));      /* the first item */
  CHECK(SendMessageA(list, LB_GETCURSEL, 0, 0) == 0);
  RECT r;
  CHECK(SendMessageA(list, LB_GETITEMRECT, 1, (LPARAM)&r) != LB_ERR && r.top > 0 && r.bottom > r.top);
  CHECK(SendMessageA(list, LB_DELETESTRING, 0, 0) == 2 && SendMessageA(list, LB_GETCOUNT, 0, 0) == 2);
  SendMessageA(list, LB_RESETCONTENT, 0, 0);
  CHECK(SendMessageA(list, LB_GETCOUNT, 0, 0) == 0);
  HWND multi = CreateWindowA("listbox", "", WS_CHILD | WS_VISIBLE | LBS_MULTIPLESEL, 110, 5, 100, 100, parent, (HMENU)2, instance, NULL);
  SendMessageA(multi, LB_ADDSTRING, 0, (LPARAM)"a"); SendMessageA(multi, LB_ADDSTRING, 0, (LPARAM)"b"); SendMessageA(multi, LB_ADDSTRING, 0, (LPARAM)"c");
  CHECK(SendMessageA(multi, LB_SETSEL, TRUE, 0) == 0 && SendMessageA(multi, LB_SETSEL, TRUE, 2) == 0);
  CHECK(SendMessageA(multi, LB_GETSELCOUNT, 0, 0) == 2 && SendMessageA(multi, LB_SETCURSEL, 1, 0) == LB_ERR);
  int items[4];
  CHECK(SendMessageA(multi, LB_GETSELITEMS, 4, (LPARAM)items) == 2 && items[0] == 0 && items[1] == 2);

  /* combo box: the list and the edit control behind one window */
  HWND combo = CreateWindowA("combobox", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_HASSTRINGS, 5, 120, 120, 150, parent, (HMENU)3, instance, NULL);
  CHECK(combo != NULL);
  CHECK(SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"one") == 0 && SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"two") == 1 && SendMessageA(combo, CB_GETCOUNT, 0, 0) == 2);
  CHECK(SendMessageA(combo, CB_GETLBTEXT, 1, (LPARAM)text) == 3 && !strcmp(text, "two"));
  CHECK(SendMessageA(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)"two") == 1);
  g_list_note_count = 0;
  CHECK(SendMessageA(combo, CB_SETCURSEL, 1, 0) == 1 && GetWindowTextA(combo, text, sizeof text) == 3 && !strcmp(text, "two"));
  CHECK(SendMessageA(combo, CB_GETCURSEL, 0, 0) == 1);
  SetWindowTextA(combo, "typed");
  CHECK(GetWindowTextA(combo, text, sizeof text) == 5 && !strcmp(text, "typed"));
  CHECK(!SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0));
  SendMessageA(combo, CB_SHOWDROPDOWN, TRUE, 0);
  CHECK(SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0));
  SendMessageA(combo, CB_SHOWDROPDOWN, FALSE, 0);
  CHECK(!SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0));
  HWND fixed = CreateWindowA("combobox", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 130, 120, 120, 150, parent, (HMENU)4, instance, NULL);
  SendMessageA(fixed, CB_ADDSTRING, 0, (LPARAM)"red");
  SendMessageA(fixed, CB_SETCURSEL, 0, 0);
  CHECK(GetWindowTextA(fixed, text, sizeof text) == 3 && !strcmp(text, "red"));
  CHECK(DestroyWindow(parent));
  UnregisterClassA("TcosListParent", instance);
}

static int g_init_dialog, g_dlg_commands;
static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_INITDIALOG:
      ++g_init_dialog;
      SetDlgItemTextA(hwnd, 101, "from init");
      SetDlgItemInt(hwnd, 102, 1234, FALSE);
      CheckDlgButton(hwnd, 103, BST_CHECKED);
      PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
      return TRUE;
    case WM_COMMAND:
      ++g_dlg_commands;
      if (LOWORD(wparam) == IDOK) { EndDialog(hwnd, 42); return TRUE; }
      break;
  }
  return FALSE;
}

/* a dialog template in memory: a static, an edit, a check box and the OK button */
static void put_word(WORD **p, WORD v) { *(*p)++ = v; }
static void put_dword(WORD **p, DWORD v) { *(DWORD *)*p = v; *p += 2; }
static void put_string(WORD **p, const wchar_t *s) { while (*s) *(*p)++ = (WORD)*s++; *(*p)++ = 0; }
static void align4(WORD *base, WORD **p) { while (((char *)*p - (char *)base) & 3) *(*p)++ = 0; }
static void put_item(WORD *base, WORD **p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text) {
  align4(base, p);
  put_dword(p, style | WS_CHILD | WS_VISIBLE); put_dword(p, 0);
  put_word(p, (WORD)x); put_word(p, (WORD)y); put_word(p, (WORD)cx); put_word(p, (WORD)cy); put_word(p, id);
  put_word(p, 0xFFFF); put_word(p, cls);
  put_string(p, text);
  put_word(p, 0);
}

static void test_dialogs(HINSTANCE instance) {
  WORD buffer[512], *p = buffer;
  memset(buffer, 0, sizeof buffer);
  put_dword(&p, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME); put_dword(&p, 0);
  put_word(&p, 4);
  put_word(&p, 10); put_word(&p, 10); put_word(&p, 120); put_word(&p, 70);
  put_word(&p, 0); put_word(&p, 0);
  put_string(&p, L"Test dialog");
  put_item(buffer, &p, SS_LEFT, 5, 5, 50, 10, 100, 0x82, L"label");
  put_item(buffer, &p, WS_TABSTOP | ES_AUTOHSCROLL, 5, 20, 50, 12, 101, 0x81, L"");
  put_item(buffer, &p, WS_TABSTOP | BS_AUTOCHECKBOX, 5, 35, 50, 10, 103, 0x80, L"check");
  put_item(buffer, &p, WS_TABSTOP | BS_DEFPUSHBUTTON, 60, 50, 40, 12, IDOK, 0x80, L"OK");
  HWND owner = CreateWindowA("static", "owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 300, 200, NULL, NULL, instance, NULL);
  g_init_dialog = 0; g_dlg_commands = 0;
  INT_PTR r = DialogBoxIndirectParamA(instance, (LPCDLGTEMPLATEA)buffer, owner, dialog_proc, 0);
  CHECK(r == 42 && g_init_dialog == 1 && g_dlg_commands >= 1);
  CHECK(IsWindowEnabled(owner));

  /* modeless: the items by id, text, numbers, check state, tab order */
  HWND dlg = CreateDialogIndirectParamA(instance, (LPCDLGTEMPLATEA)buffer, owner, dialog_proc, 0);
  CHECK(dlg != NULL && IsWindow(dlg));
  if (dlg) {
    char text[32];
    HWND edit = GetDlgItem(dlg, 101);
    CHECK(edit != NULL && GetDlgCtrlID(edit) == 101 && GetParent(edit) == dlg && GetDlgItem(dlg, 999) == NULL);
    CHECK(GetDlgItemTextA(dlg, 101, text, sizeof text) == 9 && !strcmp(text, "from init"));
    BOOL ok = FALSE;
    SetDlgItemTextA(dlg, 101, "77");
    CHECK(GetDlgItemInt(dlg, 101, &ok, FALSE) == 77 && ok);
    SetDlgItemTextA(dlg, 101, "x");
    GetDlgItemInt(dlg, 101, &ok, FALSE);
    CHECK(!ok);
    CHECK(IsDlgButtonChecked(dlg, 103) == BST_CHECKED);
    CheckDlgButton(dlg, 103, BST_UNCHECKED);
    CHECK(IsDlgButtonChecked(dlg, 103) == BST_UNCHECKED);
    CHECK(GetNextDlgTabItem(dlg, edit, FALSE) == GetDlgItem(dlg, 103) && GetNextDlgTabItem(dlg, edit, TRUE) == GetDlgItem(dlg, IDOK));
    CHECK(GetDlgItemTextA(dlg, 100, text, sizeof text) == 5 && !strcmp(text, "label"));
    CHECK(SendDlgItemMessageA(dlg, 101, WM_GETTEXTLENGTH, 0, 0) == 1);
    RECT r1 = {0, 0, 4, 8};
    CHECK(MapDialogRect(dlg, &r1) && r1.right > 0 && r1.bottom > 0);
    CHECK(GetWindowLongPtrA(dlg, DWLP_DLGPROC) == (LONG_PTR)dialog_proc);
    SendMessageA(dlg, DM_SETDEFID, IDOK, 0);
    CHECK(LOWORD(SendMessageA(dlg, DM_GETDEFID, 0, 0)) == IDOK);
    CHECK(DestroyWindow(dlg) && !IsWindow(edit));
  }
  CHECK(DialogBoxIndirectParamA(instance, NULL, owner, dialog_proc, 0) == -1);
  DestroyWindow(owner);
}

static int g_clip_updates;
static LRESULT CALLBACK clip_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLIPBOARDUPDATE) ++g_clip_updates;
  return DefWindowProcA(hwnd, message, wparam, lparam);
}

static HGLOBAL global_text_a(const char *s) {
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(s) + 1);
  memcpy(GlobalLock(h), s, strlen(s) + 1);
  GlobalUnlock(h);
  return h;
}

static void test_clipboard(HINSTANCE instance) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = clip_proc; wc.hInstance = instance; wc.lpszClassName = "TcosClipClass";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("TcosClipClass", "clip", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, instance, NULL);
  CHECK(AddClipboardFormatListener(hwnd));

  /* nothing works without OpenClipboard */
  CHECK(!EmptyClipboard() && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN);
  CHECK(OpenClipboard(hwnd) && GetOpenClipboardWindow() == hwnd);
  CHECK(EmptyClipboard() && GetClipboardOwner() == hwnd);
  DWORD sequence = GetClipboardSequenceNumber();
  HGLOBAL text = global_text_a("hello clipboard");
  CHECK(SetClipboardData(CF_TEXT, text) == text);
  CHECK(CloseClipboard() && GetOpenClipboardWindow() == NULL);
  CHECK(GetClipboardSequenceNumber() > sequence);

  /* the text formats are made from each other */
  CHECK(IsClipboardFormatAvailable(CF_TEXT) && IsClipboardFormatAvailable(CF_UNICODETEXT) && IsClipboardFormatAvailable(CF_OEMTEXT) && !IsClipboardFormatAvailable(CF_BITMAP));
  CHECK(OpenClipboard(NULL));
  HANDLE a = GetClipboardData(CF_TEXT), w = GetClipboardData(CF_UNICODETEXT);
  CHECK(a == text && GetClipboardData(CF_TEXT) == a);      /* (the owner gets its own handle back, every time) */
  const WCHAR *wide = w ? GlobalLock(w) : NULL;
  CHECK(wide && !wcscmp(wide, L"hello clipboard") && GlobalSize(w) >= 32);
  if (w) GlobalUnlock(w);
  UINT f = 0, count = 0;
  while ((f = EnumClipboardFormats(f)) != 0) ++count;
  CHECK(count >= 3 && CountClipboardFormats() == (int)count);
  CHECK(CloseClipboard());

  /* a listener hears of the change when the clipboard closes */
  MSG msg;
  while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
  CHECK(g_clip_updates >= 1);

  /* formats of the program's own: registered names, binary data */
  UINT own = RegisterClipboardFormatA("TcosOwnFormat");
  CHECK(own >= 0xC000);
  char name[32];
  CHECK(GetClipboardFormatNameA(own, name, sizeof name) == 13 && !strcmp(name, "TcosOwnFormat"));
  HGLOBAL blob = GlobalAlloc(GMEM_MOVEABLE, 5);
  memcpy(GlobalLock(blob), "\x01\x02\x00\x03\x04", 5);
  GlobalUnlock(blob);
  CHECK(OpenClipboard(hwnd) && EmptyClipboard() && SetClipboardData(own, blob) == blob && CloseClipboard());
  CHECK(IsClipboardFormatAvailable(own) && !IsClipboardFormatAvailable(CF_TEXT));
  CHECK(OpenClipboard(NULL));
  HANDLE got = GetClipboardData(own);
  const BYTE *bytes = got ? GlobalLock(got) : NULL;
  CHECK(bytes && GlobalSize(got) == 5 && bytes[2] == 0 && bytes[4] == 4);
  if (got) GlobalUnlock(got);
  CloseClipboard();

  /* copy and paste between two edit controls */
  HWND from = CreateWindowA("edit", "copy this text", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 30, NULL, NULL, instance, NULL);
  HWND to = CreateWindowA("edit", "", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 40, 200, 30, NULL, NULL, instance, NULL);
  SendMessageA(from, EM_SETSEL, 5, 9);
  SendMessageA(from, WM_COPY, 0, 0);
  SendMessageA(to, WM_PASTE, 0, 0);
  char text_a[64];
  CHECK(GetWindowTextA(to, text_a, sizeof text_a) == 4 && !strcmp(text_a, "this"));
  SendMessageA(from, EM_SETSEL, 0, 4);
  SendMessageA(from, WM_CUT, 0, 0);
  CHECK(GetWindowTextA(from, text_a, sizeof text_a) == 10 && !strcmp(text_a, " this text"));
  SendMessageA(to, EM_SETSEL, 4, 4);
  SendMessageA(to, WM_CHAR, 22, 0);      /* Ctrl+V */
  CHECK(GetWindowTextA(to, text_a, sizeof text_a) == 8 && !strcmp(text_a, "thiscopy"));
  CHECK(OpenClipboard(NULL) && IsClipboardFormatAvailable(CF_UNICODETEXT) && CloseClipboard());

  /* a bitmap is kept as a DIB too, so a reader of either format finds it */
  HBITMAP bmp = CreateBitmap(4, 3, 1, 32, NULL);
  CHECK(bmp != NULL);
  CHECK(OpenClipboard(hwnd) && EmptyClipboard() && SetClipboardData(CF_BITMAP, bmp) == bmp && CloseClipboard());
  CHECK(IsClipboardFormatAvailable(CF_BITMAP) && IsClipboardFormatAvailable(CF_DIB));
  CHECK(OpenClipboard(NULL));
  HANDLE dib = GetClipboardData(CF_DIB);
  const BITMAPINFOHEADER *header = dib ? GlobalLock(dib) : NULL;
  CHECK(header && header->biWidth == 4 && header->biHeight == 3 && header->biBitCount == 32);
  if (dib) GlobalUnlock(dib);
  CHECK(GetClipboardData(CF_BITMAP) == bmp);
  CloseClipboard();

  CHECK(OpenClipboard(NULL) && EmptyClipboard() && CloseClipboard() && CountClipboardFormats() == 0);
  CHECK(RemoveClipboardFormatListener(hwnd));
  DestroyWindow(from); DestroyWindow(to); DestroyWindow(hwnd);
  UnregisterClassA("TcosClipClass", instance);
}

/* ---- hooks ---- */
static int g_hook_calls[16], g_hook_discard, g_hook_chain;
static HHOOK g_hook_handle[16];
static UINT g_hook_last_msg;
static LRESULT CALLBACK getmsg_hook(int code, WPARAM wparam, LPARAM lparam) {
  ++g_hook_calls[WH_GETMESSAGE];
  if (code >= 0) g_hook_last_msg = ((MSG *)lparam)->message;
  return CallNextHookEx(g_hook_handle[WH_GETMESSAGE], code, wparam, lparam);
}
static LRESULT CALLBACK getmsg_hook2(int code, WPARAM wparam, LPARAM lparam) {
  ++g_hook_chain;
  return CallNextHookEx(NULL, code, wparam, lparam);
}
static LRESULT CALLBACK kbd_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) ++g_hook_calls[WH_KEYBOARD];
  return g_hook_discard;
}
static LRESULT CALLBACK cwp_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && ((CWPSTRUCT *)lparam)->message == WM_USER + 7) { ++g_hook_calls[WH_CALLWNDPROC]; CHECK(wparam == 1); }
  return CallNextHookEx(NULL, code, wparam, lparam);
}
static LRESULT CALLBACK cwpret_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && ((CWPRETSTRUCT *)lparam)->message == WM_USER + 7) { ++g_hook_calls[WH_CALLWNDPROCRET]; CHECK(((CWPRETSTRUCT *)lparam)->lResult == 77); }
  return CallNextHookEx(NULL, code, wparam, lparam);
}
static LRESULT CALLBACK cbt_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HCBT_DESTROYWND) { ++g_hook_calls[WH_CBT]; return g_hook_discard; }
  return 0;
}
static LRESULT CALLBACK ret77_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_USER + 7) return 77;
  return DefWindowProcA(hwnd, msg, wparam, lparam);
}
static void test_hooks(HINSTANCE instance) {
  DWORD tid = GetCurrentThreadId();
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = ret77_proc; wc.hInstance = instance; wc.lpszClassName = "TcosHookClass";
  CHECK(RegisterClassA(&wc) != 0);
  HWND hwnd = CreateWindowExA(0, "TcosHookClass", "hook", WS_OVERLAPPED, 0, 0, 50, 50, NULL, NULL, instance, NULL);
  CHECK(hwnd != NULL);

  CHECK(SetWindowsHookExA(99, getmsg_hook, NULL, tid) == NULL);
  CHECK(SetWindowsHookExA(WH_GETMESSAGE, NULL, NULL, tid) == NULL);
  CHECK(UnhookWindowsHookEx((HHOOK)0x1234) == FALSE);

  /* WH_GETMESSAGE: a chain of two, the newest first; each sees the message PeekMessage returns */
  g_hook_handle[WH_GETMESSAGE] = SetWindowsHookExA(WH_GETMESSAGE, getmsg_hook, NULL, tid);
  CHECK(g_hook_handle[WH_GETMESSAGE] != NULL);
  PostMessageA(hwnd, WM_USER + 1, 0, 0);
  MSG msg;
  CHECK(PeekMessageA(&msg, hwnd, WM_USER + 1, WM_USER + 1, PM_NOREMOVE));
  CHECK(g_hook_calls[WH_GETMESSAGE] == 1 && g_hook_last_msg == WM_USER + 1);
  CHECK(PeekMessageA(&msg, hwnd, WM_USER + 1, WM_USER + 1, PM_REMOVE));
  CHECK(g_hook_calls[WH_GETMESSAGE] == 2);
  HHOOK second = SetWindowsHookExA(WH_GETMESSAGE, getmsg_hook2, NULL, tid);
  CHECK(second != NULL);
  PostMessageA(hwnd, WM_USER + 2, 0, 0);
  CHECK(GetMessageA(&msg, hwnd, WM_USER + 2, WM_USER + 2) == 1);
  CHECK(g_hook_chain == 1 && g_hook_calls[WH_GETMESSAGE] == 3);
  CHECK(UnhookWindowsHookEx(second));
  CHECK(UnhookWindowsHookEx(g_hook_handle[WH_GETMESSAGE]));
  CHECK(!UnhookWindowsHookEx(g_hook_handle[WH_GETMESSAGE]));
  PostMessageA(hwnd, WM_USER + 3, 0, 0);
  CHECK(PeekMessageA(&msg, hwnd, WM_USER + 3, WM_USER + 3, PM_REMOVE) && g_hook_calls[WH_GETMESSAGE] == 3);

  /* WH_KEYBOARD: a nonzero result discards a removed message */
  HHOOK kbd = SetWindowsHookExA(WH_KEYBOARD, kbd_hook, NULL, tid);
  CHECK(kbd != NULL);
  g_hook_discard = 1;
  PostMessageA(hwnd, WM_KEYDOWN, 'A', 0);
  CHECK(!PeekMessageA(&msg, hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) && g_hook_calls[WH_KEYBOARD] == 1);
  g_hook_discard = 0;
  PostMessageA(hwnd, WM_KEYDOWN, 'B', 0);
  CHECK(PeekMessageA(&msg, hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) && msg.wParam == 'B' && g_hook_calls[WH_KEYBOARD] == 2);
  CHECK(UnhookWindowsHookEx(kbd));

  /* WH_CALLWNDPROC / WH_CALLWNDPROCRET for a sent message */
  HHOOK cwp = SetWindowsHookExA(WH_CALLWNDPROC, cwp_hook, NULL, tid);
  HHOOK cwpret = SetWindowsHookExA(WH_CALLWNDPROCRET, cwpret_hook, NULL, tid);
  CHECK(cwp != NULL && cwpret != NULL);
  CHECK(SendMessageA(hwnd, WM_USER + 7, 0, 0) == 77);
  CHECK(g_hook_calls[WH_CALLWNDPROC] == 1 && g_hook_calls[WH_CALLWNDPROCRET] == 1);
  CHECK(UnhookWindowsHookEx(cwp) && UnhookWindowsHookEx(cwpret));
  CHECK(SendMessageA(hwnd, WM_USER + 7, 0, 0) == 77 && g_hook_calls[WH_CALLWNDPROC] == 1);

  /* the old API removes by procedure */
  CHECK(SetWindowsHookA(WH_GETMESSAGE, (HOOKPROC)getmsg_hook) != NULL);
  CHECK(UnhookWindowsHook(WH_GETMESSAGE, (HOOKPROC)getmsg_hook));

  /* WH_CBT can stop a window from being destroyed */
  HHOOK cbt = SetWindowsHookExA(WH_CBT, cbt_hook, NULL, tid);
  CHECK(cbt != NULL);
  g_hook_discard = 1;
  CHECK(!DestroyWindow(hwnd) && IsWindow(hwnd) && g_hook_calls[WH_CBT] == 1);
  g_hook_discard = 0;
  CHECK(DestroyWindow(hwnd) && !IsWindow(hwnd) && g_hook_calls[WH_CBT] == 2);
  CHECK(UnhookWindowsHookEx(cbt));
  CHECK(UnregisterClassA("TcosHookClass", instance));
}

/* ---- menus ---- */
static int g_menu_stage, g_menu_commands[16], g_menu_command_count, g_menu_events[64], g_menu_event_count;
static HMENU g_menu_under_test;
static HWND g_menu_window;
static LRESULT CALLBACK menu_owner_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_COMMAND || msg == WM_SYSCOMMAND) { if (g_menu_command_count < 16) g_menu_commands[g_menu_command_count++] = (int)LOWORD(wparam); }
  if ((msg == WM_INITMENU || msg == WM_INITMENUPOPUP || msg == WM_MENUSELECT || msg == WM_ENTERMENULOOP || msg == WM_EXITMENULOOP || msg == WM_UNINITMENUPOPUP) && g_menu_event_count < 64)
    g_menu_events[g_menu_event_count++] = (int)msg;
  return DefWindowProcA(hwnd, msg, wparam, lparam);
}
static void post_key(UINT vk) { PostMessageA(g_menu_window, WM_KEYDOWN, vk, 0); }
static void CALLBACK menu_timer(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)msg; (void)time;
  KillTimer(hwnd, id);
  switch (g_menu_stage) {
    case 1: post_key(VK_DOWN); post_key(VK_DOWN); post_key(VK_RETURN); break;
    case 2: PostMessageA(g_menu_window, WM_CHAR, 'w', 0); break;
    case 3: post_key(VK_DOWN); post_key(VK_ESCAPE); break;
    case 4: post_key(VK_DOWN); post_key(VK_DOWN); post_key(VK_DOWN); post_key(VK_DOWN); post_key(VK_RIGHT); post_key(VK_DOWN); post_key(VK_RETURN); break;
    case 5: {      /* the mouse: move to the third item and click */
      RECT r;
      CHECK(GetMenuItemRect(NULL, g_menu_under_test, 2, &r));
      INPUT in[3];
      memset(in, 0, sizeof in);
      const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
      in[0].type = INPUT_MOUSE; in[0].mi.dx = (LONG)(((r.left + r.right) / 2) * 65535 / sw); in[0].mi.dy = (LONG)(((r.top + r.bottom) / 2) * 65535 / sh); in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
      in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
      in[2].type = INPUT_MOUSE; in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
      SendInput(3, in, sizeof(INPUT));
      break;
    }
    case 6: EndMenu(); break;
    case 7: post_key(VK_DOWN); post_key(VK_RETURN); break;      /* a bar: the popup of the first item, then its first entry */
    case 8: post_key(VK_RIGHT); post_key(VK_DOWN); post_key(VK_RETURN); break;
    default: break;
  }
}
static UINT run_track(HMENU menu, UINT flags, int stage) {
  g_menu_stage = stage;
  g_menu_under_test = menu;
  CHECK(SetTimer(g_menu_window, 77, 30, menu_timer) != 0);
  return (UINT)TrackPopupMenu(menu, flags, 40, 40, 0, g_menu_window, NULL);
}
static void test_menus(HINSTANCE instance) {
  /* the data: classic calls, item info, ANSI and Unicode */
  HMENU menu = CreatePopupMenu(), sub = CreatePopupMenu();
  CHECK(menu && IsMenu(menu) && sub && !IsMenu((HMENU)0x1234));
  CHECK(AppendMenuA(menu, MF_STRING, 100, "&One\tCtrl+1"));
  CHECK(AppendMenuA(menu, MF_SEPARATOR, 0, NULL));
  CHECK(AppendMenuA(menu, MF_STRING | MF_CHECKED, 101, "T&wo"));
  CHECK(AppendMenuA(menu, MF_STRING | MF_GRAYED, 102, "Three"));
  CHECK(AppendMenuA(sub, MF_STRING, 200, "Sub &A"));
  CHECK(AppendMenuA(sub, MF_STRING, 201, "Sub &B"));
  CHECK(AppendMenuA(menu, MF_POPUP, (UINT_PTR)sub, "&More"));
  CHECK(GetMenuItemCount(menu) == 5);
  CHECK(GetMenuItemCount((HMENU)0x1234) == -1);
  char text[64];
  CHECK(GetMenuStringA(menu, 0, text, sizeof text, MF_BYPOSITION) == 11 && !strcmp(text, "&One\tCtrl+1"));
  CHECK(GetMenuStringA(menu, 101, text, sizeof text, MF_BYCOMMAND) == 4 && !strcmp(text, "T&wo"));
  CHECK(GetMenuState(menu, 101, MF_BYCOMMAND) == MF_CHECKED);
  CHECK((GetMenuState(menu, 102, MF_BYCOMMAND) & MF_GRAYED) != 0);
  CHECK((GetMenuState(menu, 1, MF_BYPOSITION) & MF_SEPARATOR) != 0);
  CHECK(GetSubMenu(menu, 4) == sub && GetMenuItemID(menu, 4) == (UINT)-1 && GetMenuItemID(menu, 0) == 100);
  CHECK(GetMenuState(menu, 4, MF_BYPOSITION) == (UINT)(MF_POPUP | (2 << 8)));
  CHECK(CheckMenuItem(menu, 100, MF_CHECKED) == 0 && CheckMenuItem(menu, 100, MF_UNCHECKED) == MF_CHECKED);
  CHECK(EnableMenuItem(menu, 102, MF_ENABLED) == MF_GRAYED && !(GetMenuState(menu, 102, MF_BYCOMMAND) & MF_GRAYED));
  EnableMenuItem(menu, 102, MF_GRAYED);
  CHECK(CheckMenuRadioItem(menu, 100, 101, 100, MF_BYCOMMAND));
  CHECK((GetMenuState(menu, 100, MF_BYCOMMAND) & MF_CHECKED) && !(GetMenuState(menu, 101, MF_BYCOMMAND) & MF_CHECKED));
  MENUITEMINFOW mii;
  WCHAR wide[32];
  memset(&mii, 0, sizeof mii);
  mii.cbSize = sizeof mii; mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE; mii.dwTypeData = wide; mii.cch = 32;
  CHECK(GetMenuItemInfoW(menu, 2, TRUE, &mii) && mii.wID == 101 && mii.cch == 4 && !wcscmp(wide, L"T&wo"));
  memset(&mii, 0, sizeof mii);
  mii.cbSize = sizeof mii; mii.fMask = MIIM_STRING | MIIM_ID; mii.dwTypeData = (LPWSTR)L"Renamed"; mii.wID = 111;
  CHECK(SetMenuItemInfoW(menu, 2, TRUE, &mii));
  CHECK(GetMenuStringW(menu, 111, wide, 32, MF_BYCOMMAND) == 7 && !wcscmp(wide, L"Renamed"));
  CHECK(ModifyMenuA(menu, 111, MF_BYCOMMAND | MF_STRING | MF_CHECKED, 101, "T&wo"));
  CHECK(InsertMenuA(menu, 0, MF_BYPOSITION | MF_STRING, 99, "First") && GetMenuItemID(menu, 0) == 99 && GetMenuItemCount(menu) == 6);
  CHECK(DeleteMenu(menu, 99, MF_BYCOMMAND) && GetMenuItemCount(menu) == 5);
  CHECK(!DeleteMenu(menu, 12345, MF_BYCOMMAND) && GetLastError() == 1456 /* ERROR_MENU_ITEM_NOT_FOUND */);
  CHECK(SetMenuDefaultItem(menu, 100, FALSE) && GetMenuDefaultItem(menu, FALSE, 0) == 100);

  /* the window the menus belong to */
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = menu_owner_proc; wc.hInstance = instance; wc.lpszClassName = "TcosMenuOwner";
  CHECK(RegisterClassA(&wc) != 0);
  g_menu_window = CreateWindowExA(0, "TcosMenuOwner", "menus", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 400, 300, NULL, NULL, instance, NULL);
  CHECK(g_menu_window != NULL);

  /* TrackPopupMenu with the keyboard: Down skips the separator, Return chooses */
  g_menu_event_count = 0;
  CHECK(run_track(menu, TPM_RETURNCMD, 1) == 101);
  CHECK(g_menu_event_count >= 5 && g_menu_events[0] == WM_ENTERMENULOOP && g_menu_events[1] == WM_INITMENU && g_menu_events[2] == WM_INITMENUPOPUP);
  CHECK(g_menu_events[g_menu_event_count - 1] == WM_EXITMENULOOP && g_menu_events[g_menu_event_count - 2] == WM_UNINITMENUPOPUP);
  { int selects = 0; for (int i = 0; i < g_menu_event_count; ++i) if (g_menu_events[i] == WM_MENUSELECT) ++selects; CHECK(selects >= 3); }
  CHECK(run_track(menu, TPM_RETURNCMD, 2) == 101);      /* the mnemonic */
  CHECK(run_track(menu, TPM_RETURNCMD, 3) == 0);      /* Escape */
  CHECK(run_track(menu, TPM_RETURNCMD, 4) == 201);      /* the submenu */
  CHECK(run_track(menu, TPM_RETURNCMD, 6) == 0);      /* EndMenu */
  CHECK(run_track(menu, TPM_RETURNCMD, 5) == 101);      /* the mouse */
  { MSG m; g_menu_command_count = 0; CHECK(run_track(menu, 0, 1) == TRUE); CHECK(PeekMessageA(&m, g_menu_window, WM_COMMAND, WM_COMMAND, PM_REMOVE) && LOWORD(m.wParam) == 101); }
  CHECK(!TrackPopupMenu(menu, 0, 0, 0, 0, NULL, NULL) && GetLastError() == ERROR_INVALID_WINDOW_HANDLE);
  CHECK(!TrackPopupMenu((HMENU)0x1234, 0, 0, 0, 0, g_menu_window, NULL) && GetLastError() == 1401);
  CHECK(!EndMenu());

  /* a menu bar: the popup of the first item, Return on its first entry sends WM_COMMAND */
  HMENU bar = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu();
  AppendMenuA(file, MF_STRING, 300, "&Open");
  AppendMenuA(file, MF_STRING, 301, "&Save");
  AppendMenuA(edit, MF_STRING, 310, "&Copy");
  AppendMenuA(edit, MF_STRING, 311, "&Paste");
  AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");
  AppendMenuA(bar, MF_POPUP, (UINT_PTR)edit, "&Edit");
  RECT before, after;
  GetClientRect(g_menu_window, &before);
  CHECK(GetMenu(g_menu_window) == NULL && SetMenu(g_menu_window, bar) && GetMenu(g_menu_window) == bar);
  GetClientRect(g_menu_window, &after);
  CHECK(after.bottom < before.bottom);      /* the bar takes room from the client area */
  MENUBARINFO mbi;
  memset(&mbi, 0, sizeof mbi);
  mbi.cbSize = sizeof mbi;
  CHECK(GetMenuBarInfo(g_menu_window, OBJID_MENU, 0, &mbi) && mbi.hMenu == bar && mbi.rcBar.bottom > mbi.rcBar.top);
  RECT item0, item1;
  CHECK(GetMenuItemRect(g_menu_window, bar, 0, &item0) && GetMenuItemRect(g_menu_window, bar, 1, &item1));
  CHECK(item0.right <= item1.left && item0.top == mbi.rcBar.top && item0.bottom <= mbi.rcBar.bottom);
  POINT in_item1 = {(item1.left + item1.right) / 2, (item1.top + item1.bottom) / 2};
  CHECK(MenuItemFromPoint(g_menu_window, bar, in_item1) == 1);
  CHECK(SendMessageA(g_menu_window, WM_NCHITTEST, 0, MAKELPARAM(in_item1.x, in_item1.y)) == HTMENU);
  g_menu_command_count = 0;
  g_menu_stage = 7;
  SetTimer(g_menu_window, 78, 30, menu_timer);
  SendMessageA(g_menu_window, WM_SYSCOMMAND, SC_KEYMENU, 0);
  CHECK(DrawMenuBar(g_menu_window));
  { MSG m; CHECK(PeekMessageA(&m, g_menu_window, WM_COMMAND, WM_COMMAND, PM_REMOVE) && LOWORD(m.wParam) == 300); }
  g_menu_stage = 8;
  SetTimer(g_menu_window, 79, 30, menu_timer);
  SendMessageA(g_menu_window, WM_SYSCOMMAND, SC_KEYMENU, 'f');      /* Alt+F: the File popup; Right goes to Edit, Down Return: Paste */
  { MSG m; CHECK(PeekMessageA(&m, g_menu_window, WM_COMMAND, WM_COMMAND, PM_REMOVE) && LOWORD(m.wParam) == 311); }
  CHECK(SetMenu(g_menu_window, NULL) && GetMenu(g_menu_window) == NULL);
  CHECK(IsMenu(bar));      /* SetMenu does not destroy the old menu */

  /* a menu from a template: the old format (a popup with entries and a separator) and the extended one */
  static const WORD old_template[] = {0, 0,
      0x0010, 'F', 'i', 'l', 'e', 0, 0x0000, 400, 'N', 'e', 'w', 0, 0x0000, 0, 0, 0x0000 | 0x0080, 401, 'Q', 0,
      0x0090 & ~0x10, 402, 'L', 'a', 's', 't', 0};
  HMENU loaded = LoadMenuIndirectA(old_template);
  CHECK(loaded != NULL && GetMenuItemCount(loaded) >= 1);
  if (loaded) {
    HMENU file_menu = GetSubMenu(loaded, 0);
    CHECK(file_menu != NULL && GetMenuItemCount(file_menu) == 3 && GetMenuItemID(file_menu, 0) == 400 && (GetMenuState(file_menu, 1, MF_BYPOSITION) & MF_SEPARATOR) != 0);
    DestroyMenu(loaded);
    CHECK(!IsMenu(loaded) && !IsMenu(file_menu));
  }

  /* the system menu */
  HMENU sys = GetSystemMenu(g_menu_window, FALSE);
  CHECK(sys != NULL && GetMenuItemCount(sys) >= 5);
  CHECK(GetSystemMenu(g_menu_window, FALSE) == sys);
  CHECK(GetMenuState(sys, SC_CLOSE, MF_BYCOMMAND) != (UINT)-1);

  DestroyWindow(g_menu_window);
  CHECK(!IsMenu(sys));      /* the window's menus go with it */
  UnregisterClassA("TcosMenuOwner", instance);
  DestroyMenu(menu);
}

/* ---- scrolling ---- */
static void test_scrolling(HINSTANCE instance) {
  HWND hwnd = CreateWindowExA(0, "Static", "x", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL, 10, 10, 300, 200, NULL, NULL, instance, NULL);
  CHECK(hwnd != NULL);
  HWND child = CreateWindowExA(0, "Static", "c", WS_CHILD | WS_VISIBLE, 20, 30, 40, 20, hwnd, NULL, instance, NULL);
  SCROLLINFO si = {sizeof si, SIF_ALL, 0, 99, 10, 30, 0};
  SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  SCROLLBARINFO sbi;
  memset(&sbi, 0, sizeof sbi);
  sbi.cbSize = sizeof sbi;
  CHECK(GetScrollBarInfo(hwnd, OBJID_VSCROLL, &sbi));
  CHECK(sbi.rcScrollBar.right - sbi.rcScrollBar.left == GetSystemMetrics(SM_CXVSCROLL) && sbi.rcScrollBar.bottom > sbi.rcScrollBar.top);
  CHECK(!(sbi.rgstate[0] & STATE_SYSTEM_INVISIBLE) && sbi.dxyLineButton > 0 && sbi.xyThumbBottom > sbi.xyThumbTop && sbi.xyThumbTop >= sbi.dxyLineButton);
  memset(&sbi, 0, sizeof sbi);
  sbi.cbSize = sizeof sbi;
  CHECK(GetScrollBarInfo(hwnd, OBJID_HSCROLL, &sbi) && (sbi.rgstate[0] & STATE_SYSTEM_INVISIBLE));
  sbi.cbSize = 4;
  CHECK(!GetScrollBarInfo(hwnd, OBJID_VSCROLL, &sbi) && GetLastError() == ERROR_INVALID_PARAMETER);

  /* ScrollWindowEx: the uncovered strip is invalid, children move */
  ValidateRect(hwnd, NULL);
  HRGN rgn = CreateRectRgn(0, 0, 0, 0);
  RECT update, rc;
  int kind = ScrollWindowEx(hwnd, 0, 10, NULL, NULL, rgn, &update, SW_SCROLLCHILDREN | SW_INVALIDATE);
  GetClientRect(hwnd, &rc);
  CHECK(kind == SIMPLEREGION && update.top == 0 && update.bottom == 10 && update.left == 0 && update.right == rc.right);
  RECT cr;
  GetWindowRect(child, &cr);
  POINT p = {cr.left, cr.top};
  ScreenToClient(hwnd, &p);
  CHECK(p.x == 20 && p.y == 40);
  CHECK(GetUpdateRect(hwnd, &rc, FALSE) && rc.top == 0 && rc.bottom >= 10);
  kind = ScrollWindowEx(hwnd, 5, 5, NULL, NULL, rgn, &update, 0);
  CHECK(kind == COMPLEXREGION);
  RECT area = {0, 0, 50, 50};
  CHECK(ScrollWindow(hwnd, 100, 0, &area, NULL));      /* scrolled completely out */
  HDC dc = GetDC(hwnd);
  CHECK(ScrollDC(dc, 0, -5, &area, NULL, rgn, &update) && update.top == 45 && update.bottom == 50);
  ReleaseDC(hwnd, dc);
  CHECK(!ScrollWindowEx((HWND)0x1234, 0, 1, NULL, NULL, NULL, NULL, 0) && GetLastError() == ERROR_INVALID_WINDOW_HANDLE);
  DeleteObject(rgn);
  DestroyWindow(hwnd);
}

/* ---- MDI ---- */
static int g_mdi_activations;
static LRESULT CALLBACK mdi_frame_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  extern HWND g_mdi_client;
  return DefFrameProcA(hwnd, g_mdi_client, msg, wparam, lparam);
}
HWND g_mdi_client;
static LRESULT CALLBACK mdi_child_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_MDIACTIVATE && (HWND)lparam == hwnd) ++g_mdi_activations;
  return DefMDIChildProcA(hwnd, msg, wparam, lparam);
}
static void test_mdi(HINSTANCE instance) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = mdi_frame_proc; wc.hInstance = instance; wc.lpszClassName = "TcosMdiFrame";
  CHECK(RegisterClassA(&wc) != 0);
  wc.lpfnWndProc = mdi_child_proc; wc.lpszClassName = "TcosMdiChild";
  CHECK(RegisterClassA(&wc) != 0);
  HMENU bar = CreateMenu(), windows = CreatePopupMenu();
  AppendMenuA(bar, MF_POPUP, (UINT_PTR)windows, "&Window");
  HWND frame = CreateWindowExA(0, "TcosMdiFrame", "frame", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 600, 400, NULL, bar, instance, NULL);
  CHECK(frame != NULL);
  CLIENTCREATESTRUCT ccs = {windows, 5000};
  g_mdi_client = CreateWindowExA(0, "MDIClient", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 500, 300, frame, NULL, instance, &ccs);
  CHECK(g_mdi_client != NULL);
  CHECK(SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) == 0);

  HWND c1 = CreateMDIWindowA("TcosMdiChild", "One", 0, 0, 0, 200, 150, g_mdi_client, instance, 0);
  CHECK(c1 != NULL && (GetWindowLongA(c1, GWL_EXSTYLE) & WS_EX_MDICHILD) && GetParent(c1) == g_mdi_client);
  CHECK((HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) == c1 && g_mdi_activations == 1);
  MDICREATESTRUCTA mcs = {"TcosMdiChild", "Two", instance, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0};
  HWND c2 = (HWND)SendMessageW(g_mdi_client, WM_MDICREATE, 0, (LPARAM)&(MDICREATESTRUCTW){L"TcosMdiChild", L"Two", instance, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0});
  (void)mcs;
  CHECK(c2 != NULL && (HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) == c2);
  HWND c3 = CreateMDIWindowA("TcosMdiChild", "Three", 0, 10, 10, 200, 150, g_mdi_client, instance, 0);
  CHECK(c3 != NULL && GetTopWindow(g_mdi_client) == c3);

  /* the window menu lists the children, the active one checked */
  char text[64];
  CHECK(GetMenuStringA(windows, 5000, text, sizeof text, MF_BYCOMMAND) > 0 && !strcmp(text, "1 One"));
  CHECK(GetMenuStringA(windows, 5002, text, sizeof text, MF_BYCOMMAND) > 0 && !strcmp(text, "3 Three") && (GetMenuState(windows, 5002, MF_BYCOMMAND) & MF_CHECKED));

  /* next / previous / activate */
  CHECK(SendMessageA(g_mdi_client, WM_MDINEXT, (WPARAM)c3, 0) == 0 && (HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) != c3);
  SendMessageA(g_mdi_client, WM_MDIACTIVATE, (WPARAM)c1, 0);
  CHECK((HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) == c1 && GetTopWindow(g_mdi_client) == c1);
  SendMessageA(frame, WM_COMMAND, 5001, 0);      /* the window menu entry of the second child */
  CHECK((HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) == c2);

  /* maximize and restore */
  RECT before, rc, client;
  GetWindowRect(c2, &before);
  BOOL maximized = FALSE;
  SendMessageA(g_mdi_client, WM_MDIMAXIMIZE, (WPARAM)c2, 0);
  CHECK((HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, (LPARAM)&maximized) == c2 && maximized && IsZoomed(c2));
  GetClientRect(g_mdi_client, &client);
  GetClientRect(c2, &rc);      /* the client area of the maximized child is the client area of the MDI client (its frame lies outside) */
  CHECK(rc.right == client.right && rc.bottom == client.bottom);
  SendMessageA(g_mdi_client, WM_MDIRESTORE, (WPARAM)c2, 0);
  GetWindowRect(c2, &rc);
  CHECK(!IsZoomed(c2) && rc.left == before.left && rc.top == before.top && rc.right == before.right);

  /* tile and cascade */
  CHECK(SendMessageA(g_mdi_client, WM_MDITILE, 0, 0));
  RECT r1, r2, r3;
  GetWindowRect(c1, &r1); GetWindowRect(c2, &r2); GetWindowRect(c3, &r3);
  CHECK(r1.right - r1.left > 0 && !(r1.left < r2.right && r2.left < r1.right && r1.top < r2.bottom && r2.top < r1.bottom));
  CHECK(SendMessageA(g_mdi_client, WM_MDICASCADE, 0, 0));
  GetWindowRect(c1, &r1); GetWindowRect(c2, &r2);
  CHECK(r1.left != r2.left && r1.top != r2.top);
  CHECK(TileWindows(g_mdi_client, MDITILE_VERTICAL, NULL, 0, NULL) == 3 && CascadeWindows(g_mdi_client, 0, NULL, 0, NULL) == 3);

  /* destroying the active child activates another, the menu follows */
  HWND active = (HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0);
  SendMessageA(g_mdi_client, WM_MDIDESTROY, (WPARAM)active, 0);
  CHECK(!IsWindow(active) && (HWND)SendMessageA(g_mdi_client, WM_MDIGETACTIVE, 0, 0) != NULL && GetMenuState(windows, 5002, MF_BYCOMMAND) == (UINT)-1);
  DestroyWindow(c1);      /* destroyed directly: DefMDIChildProc tells the client */
  CHECK(GetMenuState(windows, 5001, MF_BYCOMMAND) == (UINT)-1);
  HANDLE old = (HANDLE)SendMessageA(g_mdi_client, WM_MDISETMENU, 0, (LPARAM)windows);
  CHECK(old == bar);
  DestroyWindow(frame);
  CHECK(!IsWindow(g_mdi_client));
  UnregisterClassA("TcosMdiChild", instance);
  UnregisterClassA("TcosMdiFrame", instance);
}

/* ---- touches ----------------------------------------------------------------------------------------------------------------------------------------------------
 * Injected fingers (InjectTouchInput, the path the desktop's touchscreen takes): pointer messages with their touch information, the first finger left to
 * DefWindowProc clicking as the mouse, and WM_TOUCH for a window registered for touch. */
static int g_ptr_down, g_ptr_update, g_ptr_up, g_ptr_x, g_ptr_y, g_ptr_type_ok, g_lbutton_down, g_lbutton_up, g_touch_msgs, g_touch_down, g_touch_x;
static int g_handle_pointers;
static LRESULT CALLBACK touch_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP: {
      const UINT32 id = GET_POINTERID_WPARAM(wparam);
      POINTER_TOUCH_INFO info;
      POINTER_INPUT_TYPE type = 0;
      if (GetPointerType(id, &type) && type == PT_TOUCH && GetPointerTouchInfo(id, &info) && info.pointerInfo.pointerId == id &&
          info.pointerInfo.hwndTarget == hwnd && IS_POINTER_PRIMARY_WPARAM(wparam))
        ++g_ptr_type_ok;
      g_ptr_x = info.pointerInfo.ptPixelLocation.x;
      g_ptr_y = info.pointerInfo.ptPixelLocation.y;
      if (message == WM_POINTERDOWN) ++g_ptr_down;
      else if (message == WM_POINTERUPDATE) ++g_ptr_update;
      else ++g_ptr_up;
      if (g_handle_pointers) return 0;
      break;      /* (left to DefWindowProc: the mouse's turn) */
    }
    case WM_LBUTTONDOWN: ++g_lbutton_down; return 0;
    case WM_LBUTTONUP: ++g_lbutton_up; return 0;
    case WM_TOUCH: {
      TOUCHINPUT in[4];
      const UINT n = LOWORD(wparam);
      ++g_touch_msgs;
      if (n >= 1 && n <= 4 && GetTouchInputInfo((HTOUCHINPUT)lparam, n, in, sizeof(TOUCHINPUT))) {
        if (in[0].dwFlags & TOUCHEVENTF_DOWN) ++g_touch_down;
        g_touch_x = in[0].x;
      }
      CloseTouchInputHandle((HTOUCHINPUT)lparam);
      return 0;
    }
  }
  return DefWindowProcA(hwnd, message, wparam, lparam);
}
static void inject(int x, int y, POINTER_FLAGS flags) {
  POINTER_TOUCH_INFO c;
  memset(&c, 0, sizeof c);
  c.pointerInfo.pointerType = PT_TOUCH;
  c.pointerInfo.pointerId = 0;
  c.pointerInfo.ptPixelLocation.x = x;
  c.pointerInfo.ptPixelLocation.y = y;
  c.pointerInfo.pointerFlags = flags;
  CHECK(InjectTouchInput(1, &c));
}
static void pump_for(int ms) { DWORD end = GetTickCount() + (DWORD)ms; while ((int)(end - GetTickCount()) > 0) { pump(); Sleep(5); } }
static void test_touch(HINSTANCE instance) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = touch_proc; wc.hInstance = instance; wc.lpszClassName = "TcosTouch";
  CHECK(RegisterClassA(&wc) != 0);
  HWND w = CreateWindowExA(WS_EX_TOPMOST, "TcosTouch", "touch", WS_POPUP | WS_VISIBLE, 200, 150, 300, 200, NULL, NULL, instance, NULL);
  CHECK(w != NULL);
  SetForegroundWindow(w);
  pump_for(100);
  const POINTER_INPUT_TYPE none = 0;
  POINTER_INPUT_TYPE type = none;
  CHECK(!GetPointerType(77, &type));
  CHECK(!InjectTouchInput(0, NULL));
  CHECK(InitializeTouchInjection(4, TOUCH_FEEDBACK_NONE));
  /* handled pointer messages: down, update, up with their information; no mouse */
  g_handle_pointers = 1;
  inject(300, 250, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
  pump_for(50);
  CHECK(g_ptr_down == 1 && g_ptr_x == 300 && g_ptr_y == 250);
  inject(310, 255, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
  pump_for(50);
  CHECK(g_ptr_update == 1 && g_ptr_x == 310);
  inject(310, 255, POINTER_FLAG_UP);
  pump_for(50);
  CHECK(g_ptr_up == 1 && g_ptr_type_ok == 3 && g_lbutton_down == 0);
  CHECK(GetSystemMetrics(SM_DIGITIZER) & NID_READY);
  /* left to DefWindowProc: the window is clicked */
  g_handle_pointers = 0;
  inject(320, 260, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
  pump_for(80);
  inject(320, 260, POINTER_FLAG_UP);
  pump_for(80);
  CHECK(g_ptr_down == 2 && g_lbutton_down == 1 && g_lbutton_up == 1);
  /* registered for touch: WM_TOUCH (x in hundredths of a pixel) */
  CHECK(RegisterTouchWindow(w, 0));
  ULONG flags = 1;
  CHECK(IsTouchWindow(w, &flags) && flags == 0);
  inject(330, 270, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
  pump_for(50);
  inject(330, 270, POINTER_FLAG_UP);
  pump_for(50);
  CHECK(g_touch_msgs == 2 && g_touch_down == 1 && g_touch_x == 33000 && g_ptr_down == 2);
  CHECK(UnregisterTouchWindow(w) && !IsTouchWindow(w, NULL));
  CHECK(!CloseTouchInputHandle((HTOUCHINPUT)12345));
  DestroyWindow(w);
  pump();
}

int main(void) {
  HINSTANCE instance = GetModuleHandleA(NULL);
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = test_proc; wc.hInstance = instance; wc.lpszClassName = "TcosTestClass"; wc.cbWndExtra = 16; wc.style = CS_DBLCLKS;
  ATOM atom = RegisterClassA(&wc);
  CHECK(atom != 0);
  CHECK(RegisterClassA(&wc) == 0 && GetLastError() == ERROR_CLASS_ALREADY_EXISTS);

  WNDCLASSA info;
  CHECK(GetClassInfoA(instance, "TcosTestClass", &info) == atom);
  CHECK(info.lpfnWndProc == test_proc && info.cbWndExtra == 16 && info.hInstance == instance);
  CHECK(!GetClassInfoA(instance, "NoSuchClass", &info) && GetLastError() == ERROR_CLASS_DOES_NOT_EXIST);
  CHECK(GetClassInfoA(NULL, "Button", &info) != 0);

  /* creation: the messages come in order, the window procedure sees the handle during WM_CREATE */
  HWND seen = NULL;
  HWND hwnd = CreateWindowExA(0, "TcosTestClass", "Title", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, NULL, NULL, instance, &seen);
  CHECK(hwnd != NULL && hwnd == seen);
  CHECK(IsWindow(hwnd) && !IsWindowVisible(hwnd) && IsWindowEnabled(hwnd));
  CHECK(log_index(WM_NCCREATE) == 0 && log_index(WM_CREATE) == 1);
  CHECK((GetWindowLongA(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION);
  CHECK(GetWindowLongA(hwnd, GWL_EXSTYLE) & WS_EX_WINDOWEDGE);
  CHECK(GetWindowLongPtrA(hwnd, GWLP_WNDPROC) == (LONG_PTR)test_proc);
  CHECK(GetWindowLongPtrA(hwnd, GWLP_HINSTANCE) == (LONG_PTR)instance);
  char text[64], name[64];
  CHECK(GetWindowTextA(hwnd, text, sizeof text) == 5 && !strcmp(text, "Title") && GetWindowTextLengthA(hwnd) == 5);
  CHECK(GetClassNameA(hwnd, name, sizeof name) == 13 && !strcmp(name, "TcosTestClass"));
  CHECK(GetParent(hwnd) == NULL && GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow() && GetAncestor(hwnd, GA_ROOT) == hwnd);
  RECT r;
  CHECK(GetWindowRect(hwnd, &r) && r.left == 10 && r.top == 20 && r.right == 310 && r.bottom == 220);
  CHECK(GetClientRect(hwnd, &r) && r.left == 0 && r.top == 0 && r.right > 250 && r.right < 300);
  DWORD pid = 0;
  CHECK(GetWindowThreadProcessId(hwnd, &pid) == GetCurrentThreadId() && pid == GetCurrentProcessId());

  /* extra bytes, text, long values, properties */
  CHECK(SetWindowLongA(hwnd, 0, 1234) == 0 && GetWindowLongA(hwnd, 0) == 1234);
  CHECK(GetWindowLongA(hwnd, 100) == 0 && GetLastError() == ERROR_INVALID_INDEX);
  CHECK(SetWindowTextA(hwnd, "Other") && GetWindowTextA(hwnd, text, sizeof text) == 5 && !strcmp(text, "Other"));
  CHECK(SetPropA(hwnd, "tcos", (HANDLE)0x55) && GetPropA(hwnd, "tcos") == (HANDLE)0x55 && RemovePropA(hwnd, "tcos") == (HANDLE)0x55 && GetPropA(hwnd, "tcos") == NULL);

  /* sent and posted messages: SendMessage calls the procedure at once, PostMessage queues */
  CHECK(SendMessageA(hwnd, WM_USER + 2, 10, 5) == 25);
  g_log_count = 0;
  CHECK(PostMessageA(hwnd, WM_USER + 1, 0, 0));
  CHECK(!log_has(WM_USER + 1));
  MSG msg;
  CHECK(PeekMessageA(&msg, hwnd, WM_USER + 1, WM_USER + 1, PM_NOREMOVE) && msg.message == WM_USER + 1);
  CHECK(GetMessageA(&msg, hwnd, 0, 0) == 1 && msg.hwnd == hwnd);
  DispatchMessageA(&msg);
  CHECK(log_has(WM_USER + 1));
  CHECK(!PeekMessageA(&msg, hwnd, 0, 0, PM_REMOVE));
  DWORD tid;
  HANDLE thread = CreateThread(NULL, 0, sender, hwnd, 0, &tid);      /* another thread sends: this thread answers while it waits in its message loop */
  DWORD end = GetTickCount() + 5000;
  while (WaitForSingleObject(thread, 0) == WAIT_TIMEOUT && (int)(end - GetTickCount()) > 0) { MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT); pump(); }
  DWORD code = 0;
  GetExitCodeThread(thread, &code);
  CHECK(code == 25);
  CloseHandle(thread);

  /* show and hide, size and move */
  g_log_count = 0;
  CHECK(!ShowWindow(hwnd, SW_SHOW) && IsWindowVisible(hwnd) && log_has(WM_SHOWWINDOW));
  CHECK(ShowWindow(hwnd, SW_HIDE) && !IsWindowVisible(hwnd));
  g_log_count = 0;
  CHECK(MoveWindow(hwnd, 50, 60, 120, 90, TRUE));
  CHECK(GetWindowRect(hwnd, &r) && r.left == 50 && r.top == 60 && r.right == 170 && r.bottom == 150);
  CHECK(log_has(WM_MOVE) && log_has(WM_SIZE));
  CHECK(SetWindowPos(hwnd, NULL, 0, 0, 200, 100, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
  CHECK(GetWindowRect(hwnd, &r) && r.right - r.left == 200 && r.bottom - r.top == 100);
  WINDOWPLACEMENT placement = {0};
  placement.length = sizeof placement;
  CHECK(GetWindowPlacement(hwnd, &placement) && placement.rcNormalPosition.left == 50);

  /* the tree: children, owner, enumeration, hit testing, coordinates */
  HWND child1 = CreateWindowExA(0, "TcosTestClass", "c1", WS_CHILD | WS_VISIBLE, 5, 6, 40, 30, hwnd, (HMENU)1, instance, NULL);
  HWND child2 = CreateWindowExA(0, "TcosTestClass", "c2", WS_CHILD | WS_VISIBLE, 50, 6, 40, 30, hwnd, (HMENU)2, instance, NULL);
  CHECK(child1 && child2 && GetParent(child1) == hwnd && GetAncestor(child1, GA_ROOT) == hwnd && IsChild(hwnd, child2) && !IsChild(child1, child2));
  CHECK(GetWindowLongA(child2, GWL_ID) == 2);
  CHECK(GetWindow(hwnd, GW_CHILD) == child1 && GetWindow(child1, GW_HWNDNEXT) == child2 && GetWindow(child2, GW_HWNDPREV) == child1);      /* (a new child goes below the others) */
  CHECK(GetTopWindow(hwnd) == child1 && GetWindow(child2, GW_HWNDLAST) == child2);
  int count = 0;
  CHECK(EnumChildWindows(hwnd, count_windows, (LPARAM)&count) && count == 2);
  count = 0;
  EnumWindows(count_windows, (LPARAM)&count);
  CHECK(count >= 1);
  count = 0;
  EnumThreadWindows(GetCurrentThreadId(), count_windows, (LPARAM)&count);
  CHECK(count >= 1);
  POINT p = {0, 0};
  CHECK(ClientToScreen(child1, &p) && p.x > 50 && p.y > 60);
  CHECK(ScreenToClient(child1, &p) && p.x == 0 && p.y == 0);
  CHECK(FindWindowExA(hwnd, NULL, "TcosTestClass", "c2") == child2);
  CHECK(FindWindowA("TcosTestClass", "Other") == hwnd);
  CHECK(SetParent(child1, NULL) == hwnd && GetParent(child1) == GetDesktopWindow());
  CHECK(!SetParent(hwnd, child2) && IsWindow(hwnd));      /* (a window cannot become the child of its own child) */
  CHECK(EnableWindow(child2, FALSE) == FALSE && !IsWindowEnabled(child2));
  EnableWindow(child2, TRUE);

  /* focus and activation */
  CHECK(ShowWindow(hwnd, SW_SHOW) == FALSE);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  CHECK(GetFocus() == hwnd && GetActiveWindow() == hwnd);

  /* timers: the message loop gets WM_TIMER and the procedure quits the loop */
  g_log_count = 0;
  CHECK(SetTimer(hwnd, 77, 20, NULL) == 77);
  end = GetTickCount() + 5000;
  int got = 0;
  while (GetTickCount() < end && (got = GetMessageA(&msg, NULL, 0, 0)) > 0) DispatchMessageA(&msg);
  CHECK(got == 0 && msg.message == WM_QUIT && msg.wParam == 7 && log_has(WM_TIMER));
  CHECK(KillTimer(hwnd, 77));

  /* destruction: WM_DESTROY before WM_NCDESTROY, children go with the parent, the class cannot be removed while windows exist */
  CHECK(!UnregisterClassA("TcosTestClass", instance) && GetLastError() == ERROR_CLASS_HAS_WINDOWS);
  g_log_count = 0;
  CHECK(DestroyWindow(hwnd) && !IsWindow(hwnd) && !IsWindow(child2));
  CHECK(log_index(WM_DESTROY) >= 0 && log_index(WM_NCDESTROY) > log_index(WM_DESTROY));
  CHECK(!DestroyWindow(hwnd) && GetLastError() == ERROR_INVALID_WINDOW_HANDLE);
  CHECK(DestroyWindow(child1));
  CHECK(UnregisterClassA("TcosTestClass", instance));
  CHECK(CreateWindowExA(0, "TcosTestClass", NULL, 0, 0, 0, 1, 1, NULL, NULL, instance, NULL) == NULL && GetLastError() == ERROR_CANNOT_FIND_WND_CLASS);

  test_controls(instance);
  test_icons_and_carets(instance);
  test_scroll_bars(instance);
  test_list_and_combo(instance);
  test_dialogs(instance);
  test_clipboard(instance);
  test_hooks(instance);
  test_menus(instance);
  test_scrolling(instance);
  test_mdi(instance);
  test_touch(instance);

  /* the system metrics are the window manager's */
  CHECK(GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0 && GetSystemMetrics(SM_CYCAPTION) > 0);

  printf("user32test: %d failures\n", failures);
  return failures;
}
