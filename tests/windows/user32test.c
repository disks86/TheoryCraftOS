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
  CHECK(GetWindow(hwnd, GW_CHILD) == child2 && GetWindow(child2, GW_HWNDNEXT) == child1 && GetWindow(child1, GW_HWNDPREV) == child2);      /* (the newest child is on top) */
  CHECK(GetTopWindow(hwnd) == child2);
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

  /* the system metrics are the window manager's */
  CHECK(GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0 && GetSystemMetrics(SM_CYCAPTION) > 0);

  printf("user32test: %d failures\n", failures);
  return failures;
}
