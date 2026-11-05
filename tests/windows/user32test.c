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
  CHECK(SetParent(child1, NULL) == hwnd && GetParent(child1) == NULL);
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

  /* the system metrics are the window manager's */
  CHECK(GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0 && GetSystemMetrics(SM_CYCAPTION) > 0);

  printf("user32test: %d failures\n", failures);
  return failures;
}
