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

/* The smaller user32.dll functions of src/Windows/user32/misc.inc: rectangles, LoadString from a string table, OEM conversions, message boxes, window stations and desktops,
 * GetLastInputInfo, notifications, pointer devices, regions, frames, DDE. Prints "Test failed" per wrong answer, exit code = failures.
 * Build: tests/windows/build.sh. Run: copy the exe to build/winetests/user32_misc.exe and scripts/winetest.sh user32 misc. */
#include <initguid.h>
#include <windows.h>
#include <dbt.h>
#include <winnt.h>
#include <ddeml.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

BOOL WINAPI IsWindowArranged(HWND hwnd);      /* (not in every mingw header) */
static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("user32misctest.c:%d: Test failed: %s\n", __LINE__, #c); } } while (0)

static void test_rects(void) {
  RECT a, b, r;
  CHECK(SetRect(&a, 0, 0, 10, 10) && a.right == 10 && a.bottom == 10);
  SetRect(&b, 5, 5, 20, 20);
  CHECK(IntersectRect(&r, &a, &b) && r.left == 5 && r.top == 5 && r.right == 10 && r.bottom == 10);
  SetRect(&b, 10, 0, 20, 10);      /* touching only: empty */
  CHECK(!IntersectRect(&r, &a, &b) && r.left == 0 && r.right == 0 && r.top == 0 && r.bottom == 0);
  SetRect(&b, 5, 5, 20, 20);
  CHECK(UnionRect(&r, &a, &b) && r.left == 0 && r.top == 0 && r.right == 20 && r.bottom == 20);
  CHECK(!IsRectEmpty(&a));
  SetRect(&r, 3, 3, 3, 9);
  CHECK(IsRectEmpty(&r));
  SetRect(&r, 5, 5, 2, 9);
  CHECK(IsRectEmpty(&r));
  CHECK(IsRectEmpty(NULL));
  r = a;
  CHECK(EqualRect(&r, &a));
  CHECK(InflateRect(&r, 2, 3) && r.left == -2 && r.top == -3 && r.right == 12 && r.bottom == 13);
  CHECK(!EqualRect(&r, &a));
  CHECK(OffsetRect(&r, 2, 3) && r.left == 0 && r.top == 0 && r.right == 14 && r.bottom == 16);
  POINT in = {0, 0}, edge = {10, 5}, out = {-1, 5};
  CHECK(PtInRect(&a, in) && !PtInRect(&a, edge) && !PtInRect(&a, out));
  SetRect(&b, 0, 0, 4, 10);      /* covers the left side: what remains is 4..10 */
  CHECK(SubtractRect(&r, &a, &b) && r.left == 4 && r.right == 10 && r.top == 0 && r.bottom == 10);
  CHECK(!SubtractRect(&r, &a, &a));
  CHECK(SetRectEmpty(&r) && IsRectEmpty(&r) && CopyRect(&r, &a) && EqualRect(&r, &a));
}

static void test_strings(void) {
  HINSTANCE self = GetModuleHandleW(NULL);
  WCHAR buffer[64];
  CHECK(LoadStringW(self, 1, buffer, 64) == 5 && wcscmp(buffer, L"Hello") == 0);
  CHECK(LoadStringW(NULL, 1, buffer, 64) == 5);
  CHECK(LoadStringW(self, 1, buffer, 3) == 2 && wcscmp(buffer, L"He") == 0);      /* cut to max - 1 */
  CHECK(LoadStringW(self, 15, buffer, 64) == 24 && wcscmp(buffer, L"last of the first bundle") == 0);
  CHECK(LoadStringW(self, 17, buffer, 64) == 13 && wcscmp(buffer, L"Second bundle") == 0);
  buffer[0] = 'x';
  CHECK(LoadStringW(self, 2, buffer, 64) == 0 && buffer[0] == 0);      /* empty */
  buffer[0] = 'x';
  CHECK(LoadStringW(self, 99, buffer, 64) == 0 && buffer[0] == 0);      /* no such bundle */
  const WCHAR *raw = NULL;
  CHECK(LoadStringW(self, 17, (LPWSTR)&raw, 0) == 13 && raw && wcsncmp(raw, L"Second bundle", 13) == 0);      /* a pointer into the resource */
  char narrow[64];
  CHECK(LoadStringA(self, 17, narrow, 64) == 13 && strcmp(narrow, "Second bundle") == 0);
  CHECK(LoadStringA(self, 1, narrow, 4) == 3 && strcmp(narrow, "Hel") == 0);

  char oem[8];
  CHECK(CharToOemA("abc", oem) && strcmp(oem, "abc") == 0);
  CHECK(CharToOemBuffW(L"xyz", oem, 3) && memcmp(oem, "xyz", 3) == 0);
  char back[8];
  CHECK(OemToCharA("hi", back) && strcmp(back, "hi") == 0);
  CHECK(OemToCharBuffA("ok!", back, 3) && memcmp(back, "ok!", 3) == 0);

  CHECK(MessageBoxW(NULL, L"user32misctest message box", L"Test", MB_OK) == IDOK);
  CHECK(MessageBoxA(NULL, "a question", "Test", MB_YESNO) == IDYES);
  CHECK(MessageBoxW(NULL, L"default button 2", L"Test", MB_OKCANCEL | MB_DEFBUTTON2) == IDCANCEL);
}

static void test_stations(void) {
  HWINSTA station = GetProcessWindowStation();
  CHECK(station != NULL);
  WCHAR name[64];
  DWORD needed = 0;
  CHECK(GetUserObjectInformationW(station, UOI_NAME, name, sizeof name, &needed) && wcscmp(name, L"WinSta0") == 0 && needed == sizeof L"WinSta0");
  SetLastError(0);
  CHECK(!GetUserObjectInformationW(station, UOI_NAME, name, 4, &needed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed == sizeof L"WinSta0");
  CHECK(GetUserObjectInformationW(station, UOI_TYPE, name, sizeof name, &needed) && wcscmp(name, L"WindowStation") == 0);
  USEROBJECTFLAGS flags;
  CHECK(GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof flags, &needed) && needed == sizeof flags);
  char narrow[64];
  CHECK(GetUserObjectInformationA(station, UOI_NAME, narrow, sizeof narrow, &needed) && strcmp(narrow, "WinSta0") == 0 && needed == 8);

  HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
  CHECK(desktop != NULL);
  CHECK(GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name, &needed) && wcscmp(name, L"Default") == 0);
  CHECK(GetUserObjectInformationW(desktop, UOI_TYPE, name, sizeof name, &needed) && wcscmp(name, L"Desktop") == 0);
  CHECK(SetThreadDesktop(desktop));

  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  CHECK(input != NULL);
  if (input) CHECK(CloseDesktop(input));

  HDESK made = CreateDesktopW(L"tcos-misc-test", NULL, NULL, 0, GENERIC_ALL, NULL);
  CHECK(made != NULL);
  if (made) {
    CHECK(GetUserObjectInformationW(made, UOI_NAME, name, sizeof name, &needed) && wcscmp(name, L"tcos-misc-test") == 0);
    HDESK opened = OpenDesktopW(L"tcos-misc-test", 0, FALSE, GENERIC_ALL);
    CHECK(opened != NULL);
    if (opened) CHECK(CloseDesktop(opened));
    CHECK(CloseDesktop(made));
  }
  HWINSTA other = CreateWindowStationW(L"tcos-misc-station", 0, WINSTA_ALL_ACCESS, NULL);
  CHECK(other != NULL);
  if (other) {
    CHECK(GetUserObjectInformationW(other, UOI_NAME, name, sizeof name, &needed) && wcscmp(name, L"tcos-misc-station") == 0);
    CHECK(CloseWindowStation(other));
  }
  CHECK(SetProcessWindowStation(station));
}

static void test_input_and_windows(void) {
  LASTINPUTINFO info = {sizeof info, 0};
  CHECK(GetLastInputInfo(&info));
  LASTINPUTINFO bad = {0, 0};
  CHECK(!GetLastInputInfo(&bad));

  UINT32 count = 99;
  CHECK(GetPointerDevices(&count, NULL) && count == 0);
  POINTER_INPUT_TYPE type = 1;
  CHECK(!GetPointerType(1, &type));

  HWND window = CreateWindowExW(0, L"Static", L"misc", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
  CHECK(window != NULL);
  HPOWERNOTIFY power = RegisterPowerSettingNotification(window, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
  CHECK(power != NULL);
  CHECK(UnregisterPowerSettingNotification(power));
  CHECK(!UnregisterPowerSettingNotification(power));      /* only once */
  HPOWERNOTIFY suspend = RegisterSuspendResumeNotification(window, DEVICE_NOTIFY_WINDOW_HANDLE);
  CHECK(suspend != NULL);
  CHECK(UnregisterSuspendResumeNotification(suspend));
  DEV_BROADCAST_DEVICEINTERFACE_W filter;
  memset(&filter, 0, sizeof filter);
  filter.dbcc_size = sizeof filter;
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  HDEVNOTIFY device = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
  CHECK(device != NULL);
  CHECK(UnregisterDeviceNotification(device));

  DWORD affinity = 7;
  CHECK(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_NONE);
  CHECK(SetWindowDisplayAffinity(window, WDA_MONITOR));
  CHECK(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_MONITOR);

  HRGN region = CreateRectRgn(0, 0, 0, 0);
  CHECK(GetWindowRgn(window, region) == ERROR);      /* no region yet */
  CHECK(SetWindowRgn(window, CreateRectRgn(1, 2, 8, 9), FALSE));
  RECT box;
  CHECK(GetWindowRgnBox(window, &box) == SIMPLEREGION && box.left == 1 && box.top == 2 && box.right == 8 && box.bottom == 9);
  DeleteObject(region);

  HDC dc = GetDC(window);
  CHECK(WindowFromDC(dc) == window);
  RECT r = {0, 0, 10, 10};
  CHECK(DrawEdge(dc, &r, EDGE_SUNKEN, BF_RECT | BF_ADJUST) && r.left == 2 && r.top == 2 && r.right == 8 && r.bottom == 8);
  CHECK(DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH));
  CHECK(InvertRect(dc, &r));
  ReleaseDC(window, dc);

  TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, window, HOVER_DEFAULT};
  CHECK(TrackMouseEvent(&tme));
  TRACKMOUSEEVENT query = {sizeof query, TME_QUERY, NULL, 0};
  CHECK(TrackMouseEvent(&query));
  CHECK(!IsWindowArranged(window));
  DestroyWindow(window);

  DWORD instance = 1;
  CHECK(DdeInitializeW(&instance, NULL, APPCMD_CLIENTONLY, 0) != DMLERR_NO_ERROR && instance == 0);
  CHECK(DdeConnect(0, NULL, NULL, NULL) == NULL);
  CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) > 0);
}

int main(void) {
  test_rects();
  test_strings();
  test_stations();
  test_input_and_windows();
  printf("user32misctest: %d checks, %d failed\n", checks, failures);
  return failures;
}
