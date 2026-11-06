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

/* Monitors and display settings of user32.dll (included by window.inc after the scrolling): win32k knows one monitor, the screen. GetMonitorInfo and the MonitorFrom functions answer for it
 * (a point or window outside it is handled by the flags: nothing, the primary monitor or the nearest, which is the same), EnumDisplayMonitors calls back once, and the display settings
 * come from the window manager (the W forms only; the DEVMODEA forms stay unimplemented). */

#define MONITOR_DEFAULTTONULL_ 0
#define ERROR_INVALID_MONITOR_HANDLE_ 1461
IMPORT int WINAPI GetClipBox(HANDLE hdc, RECT_ *box);
SVC NtUserEnumDisplaySettings(ULONG_PTR device, ULONG_PTR mode, ULONG_PTR devmode, ULONG_PTR flags);
SVC NtUserEnumDisplayDevices(ULONG_PTR device, ULONG_PTR index, ULONG_PTR info, ULONG_PTR flags);
typedef struct { DWORD size; RECT_ monitor, work; DWORD flags; WCHAR device[32]; } MonitorInfoW_;
EXPORT BOOL WINAPI GetMonitorInfoW(HANDLE monitor, void *info) {
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserCallTwoParam((ULONG_PTR)monitor, (ULONG_PTR)info, 2 /* GetMonitorInfo */) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI GetMonitorInfoA(HANDLE monitor, void *info) {
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD size = *(DWORD *)info;
  if (size != 40 && size != 72) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }      /* MONITORINFO or MONITORINFOEXA */
  MonitorInfoW_ w;
  for (size_t_ i = 0; i < sizeof w; ++i) ((BYTE *)&w)[i] = 0;
  w.size = size == 40 ? 40 : 104;
  if (!GetMonitorInfoW(monitor, &w)) return FALSE;
  BYTE *out = info;
  for (size_t_ i = 0; i < 40; ++i) out[i] = ((BYTE *)&w)[i];
  *(DWORD *)out = size;
  if (size == 72) { char *name = (char *)(out + 40); int i = 0; for (; i < 31 && w.device[i]; ++i) name[i] = (char)w.device[i]; name[i] = 0; for (++i; i < 32; ++i) name[i] = 0; }
  return TRUE;
}
EXPORT HANDLE WINAPI MonitorFromRect(const RECT_ *rect, DWORD flags) {
  if (!rect || flags > 2) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return (HANDLE)(NtUserCallTwoParam((ULONG_PTR)rect, flags, 4 /* MonitorFromRect */) & 0xFFFFFFFFu);
}
EXPORT HANDLE WINAPI MonitorFromPoint(POINT_ pt, DWORD flags) {
  RECT_ r = {pt.x, pt.y, pt.x + 1, pt.y + 1};
  return MonitorFromRect(&r, flags);
}
EXPORT HANDLE WINAPI MonitorFromWindow(HANDLE hwnd, DWORD flags) {
  RECT_ r = {0, 0, 0, 0};
  if (flags > 2) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (IsWindow(hwnd)) GetWindowRect(hwnd, &r); else if (flags == MONITOR_DEFAULTTONULL_) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return NULL; }
  return MonitorFromRect(&r, flags);
}
typedef BOOL (WINAPI *MONITORENUMPROC_)(HANDLE, HANDLE, RECT_ *, LONG_PTR);
EXPORT BOOL WINAPI EnumDisplayMonitors(HANDLE hdc, const RECT_ *clip, MONITORENUMPROC_ proc, LONG_PTR data) {
  if (!proc) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  RECT_ zero = {0, 0, 0, 0};
  HANDLE monitor = MonitorFromRect(&zero, 1 /* MONITOR_DEFAULTTOPRIMARY */);
  MonitorInfoW_ info;
  for (size_t_ i = 0; i < sizeof info; ++i) ((BYTE *)&info)[i] = 0;
  info.size = 40;
  if (!monitor || !GetMonitorInfoW(monitor, &info)) return FALSE;
  RECT_ area = info.monitor;
  if (hdc) {      /* in device coordinates: the rectangle of the monitor within the clip of the context, from the origin of its window */
    RECT_ box;
    if (GetClipBox(hdc, &box) == 0 /* ERROR */) return FALSE;
    area.left = 0; area.top = 0; area.right = info.monitor.right - info.monitor.left; area.bottom = info.monitor.bottom - info.monitor.top;
  }
  if (clip) { RECT_ meet; if (!rect_meet(&area, clip, &meet)) return TRUE; area = meet; }
  proc(monitor, hdc, &area, data);
  return TRUE;
}
EXPORT BOOL WINAPI EnumDisplaySettingsExW(LPCWSTR device, DWORD mode, void *devmode, DWORD flags) {
  UNICODE_STRING us;
  if (device) us_name(&us, device);
  if (!devmode) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserEnumDisplaySettings(device ? (ULONG_PTR)&us : 0, mode, (ULONG_PTR)devmode, flags) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI EnumDisplaySettingsW(LPCWSTR device, DWORD mode, void *devmode) { return EnumDisplaySettingsExW(device, mode, devmode, 0); }
EXPORT BOOL WINAPI EnumDisplayDevicesW(LPCWSTR device, DWORD index, void *info, DWORD flags) {
  UNICODE_STRING us;
  if (device) us_name(&us, device);
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserEnumDisplayDevices(device ? (ULONG_PTR)&us : 0, index, (ULONG_PTR)info, flags) & 0xFFFFFFFFu) != 0;
}
