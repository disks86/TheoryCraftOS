/* A Windows GUI program for checking by eye (or by screendump) how the desktop shows a program's windows beyond the basics:
 *   - "Fenêtre – Tést" (a title outside ASCII) with a red-and-white icon of its own, the hand cursor over it (its class cursor), and the I-beam over its left half
 *     (SetCursor while the mouse moves there);
 *   - an owned window ("Owned") that stays above it when it is clicked;
 *   - a window without a caption (blue, a darker bar along its top) that the user drags by that bar (WM_NCLBUTTONDOWN with HTCAPTION, as custom title bars do).
 * It keeps running until the main window is closed.
 *   x86_64-w64-mingw32-gcc -O1 -mwindows -municode -o windowstest.exe tests/windows/windowstest.c -luser32 -lgdi32
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */
#include <windows.h>

static void fill(HWND hwnd, COLORREF color) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT r;
  GetClientRect(hwnd, &r);
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &r, brush);
  DeleteObject(brush);
  if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) {      /* the captionless window's own title bar */
    RECT bar = {0, 0, r.right, 20};
    brush = CreateSolidBrush(RGB(0, 0, 90));
    FillRect(dc, &bar, brush);
    DeleteObject(brush);
  }
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: fill(hwnd, RGB(0, 150, 60)); return 0;
    case WM_MOUSEMOVE: {
      RECT r;
      GetClientRect(hwnd, &r);
      if ((short)LOWORD(lparam) < r.right / 2) SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM));
      else SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
      return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK other_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: fill(hwnd, (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) ? RGB(40, 80, 220) : RGB(220, 160, 0)); return 0;
    case WM_LBUTTONDOWN:
      if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) && HIWORD(lparam) < 20) {      /* the bar: move the window as a caption would */
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
      }
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* A 16 x 16 icon: red with a white square in the middle. */
static HICON make_icon(void) {
  DWORD pixels[16 * 16];
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) pixels[y * 16 + x] = (x >= 5 && x < 11 && y >= 5 && y < 11) ? 0xFFFFFFFFu : 0xFFD01010u;
  ICONINFO info = {0};
  info.fIcon = TRUE;
  info.hbmColor = CreateBitmap(16, 16, 1, 32, pixels);
  info.hbmMask = CreateBitmap(16, 16, 1, 1, NULL);
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(info.hbmColor);
  DeleteObject(info.hbmMask);
  return icon;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command, int show) {
  (void)previous; (void)command; (void)show;
  WNDCLASSW cls = {0};
  cls.lpfnWndProc = main_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
  cls.lpszClassName = L"windowstest";
  RegisterClassW(&cls);
  cls.lpfnWndProc = other_proc;
  cls.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  cls.lpszClassName = L"windowstest-other";
  RegisterClassW(&cls);
  HWND main = CreateWindowExW(0, L"windowstest", L"Fen\x00eatre \x2013 T\x00e9st", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 420, 300, NULL, NULL, instance, NULL);
  SendMessageW(main, WM_SETICON, ICON_SMALL, (LPARAM)make_icon());
  CreateWindowExW(0, L"windowstest-other", L"Owned", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 300, 200, 260, 160, main, NULL, instance, NULL);
  CreateWindowExW(WS_EX_APPWINDOW, L"windowstest-other", L"No caption", WS_POPUP | WS_VISIBLE, 600, 420, 240, 140, NULL, NULL, instance, NULL);
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
