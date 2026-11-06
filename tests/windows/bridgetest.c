/* A Windows GUI program for checking the desktop bridge by eye (or by screendump): a window whose client area is red, turns green on the key G, blue on a left
 * click, and yellow once raw input has reported 20 mouse motions. It keeps running until its window is closed.
 *   x86_64-w64-mingw32-gcc -O1 -mwindows -o bridgetest.exe tests/windows/bridgetest.c -luser32 -lgdi32
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */
#include <windows.h>

static COLORREF g_color = RGB(200, 0, 0);
static int g_motions;

static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      HBRUSH brush = CreateSolidBrush(g_color);
      RECT r;
      GetClientRect(hwnd, &r);
      FillRect(dc, &r, brush);
      DeleteObject(brush);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CHAR:
      if (wparam == 'g' || wparam == 'G') { g_color = RGB(0, 200, 0); InvalidateRect(hwnd, NULL, TRUE); }
      return 0;
    case WM_LBUTTONDOWN:
      g_color = RGB(0, 0, 200);
      InvalidateRect(hwnd, NULL, TRUE);
      return 0;
    case WM_INPUT: {
      RAWINPUT input;
      UINT size = sizeof input;
      if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 && input.header.dwType == RIM_TYPEMOUSE &&
          (input.data.mouse.lLastX || input.data.mouse.lLastY) && ++g_motions == 20) {
        g_color = RGB(220, 220, 0);
        InvalidateRect(hwnd, NULL, TRUE);
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
  (void)previous; (void)command; (void)show;
  WNDCLASSW cls = {0};
  cls.lpfnWndProc = proc;
  cls.hInstance = instance;
  cls.lpszClassName = L"bridgetest";
  RegisterClassW(&cls);
  HWND hwnd = CreateWindowExW(0, L"bridgetest", L"Bridge test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL, instance, NULL);
  RAWINPUTDEVICE mouse = {1, 2, 0, hwnd};
  RegisterRawInputDevices(&mouse, 1, sizeof mouse);
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
