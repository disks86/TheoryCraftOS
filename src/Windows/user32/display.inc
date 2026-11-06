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

/* ---- display configuration -------------------------------------------------------------------------------------------------------------------------------------------
 * The paths and modes of the one monitor and the display mode changes are the window manager's (src/Process/Win32k/Win32kUserMiscMonitor.cpp). The query services answer Win32
 * errors themselves; NtUserDisplayConfigGetDeviceInfo answers an NTSTATUS. */
SVC NtUserGetDisplayConfigBufferSizes(ULONG_PTR flags, ULONG_PTR paths, ULONG_PTR modes);
SVC NtUserQueryDisplayConfig(ULONG_PTR flags, ULONG_PTR path_count, ULONG_PTR paths, ULONG_PTR mode_count, ULONG_PTR modes, ULONG_PTR topology);
SVC NtUserDisplayConfigGetDeviceInfo(ULONG_PTR packet);
SVC NtUserChangeDisplaySettings(ULONG_PTR device, ULONG_PTR devmode, ULONG_PTR hwnd, ULONG_PTR flags, ULONG_PTR lparam);
IMPORT ULONG NTAPI RtlNtStatusToDosError(LONG status);
EXPORT LONG WINAPI GetDisplayConfigBufferSizes(UINT flags, UINT *path_count, UINT *mode_count) { return (LONG)NtUserGetDisplayConfigBufferSizes(flags, (ULONG_PTR)path_count, (ULONG_PTR)mode_count); }
EXPORT LONG WINAPI QueryDisplayConfig(UINT flags, UINT *path_count, void *paths, UINT *mode_count, void *modes, UINT *topology) {
  return (LONG)NtUserQueryDisplayConfig(flags, (ULONG_PTR)path_count, (ULONG_PTR)paths, (ULONG_PTR)mode_count, (ULONG_PTR)modes, (ULONG_PTR)topology);
}
EXPORT LONG WINAPI DisplayConfigGetDeviceInfo(void *packet) {
  return (LONG)RtlNtStatusToDosError((LONG)NtUserDisplayConfigGetDeviceInfo((ULONG_PTR)packet));
}
EXPORT LONG WINAPI ChangeDisplaySettingsExW(LPCWSTR device, void *devmode, HANDLE hwnd, DWORD flags, void *lparam) {
  UNICODE_STRING us;
  if (device) us_name(&us, device);
  return (LONG)NtUserChangeDisplaySettings(device ? (ULONG_PTR)&us : 0, (ULONG_PTR)devmode, (ULONG_PTR)hwnd, flags, (ULONG_PTR)lparam);
}
EXPORT LONG WINAPI ChangeDisplaySettingsW(void *devmode, DWORD flags) { return ChangeDisplaySettingsExW(NULL, devmode, NULL, flags, NULL); }

/* ---- DPI awareness ---------------------------------------------------------------------------------------------------------------------------------------------------
 * win32k keeps the awareness of the process and of each thread as Wine's ntuser contexts: the awareness in the low nibble, the version of the model above it, the dpi in bits
 * 8..16 (96 for unaware, the system dpi for system aware, none for per monitor), 0x40000000 for GDI scaling (unaware only) and 0x80000000 for "the process's" (a thread that has
 * none of its own; handed back to SetThreadDpiAwarenessContext it makes the thread follow the process again). The public DPI_AWARENESS_CONTEXT values -1 ... -5 are mapped to
 * those. One monitor at the system dpi: a window's dpi is 96 for an unaware process and the dpi of the monitor otherwise. */
SVC NtUserGetSystemDpiForProcess(ULONG_PTR process);
SVC NtUserGetProcessDpiAwarenessContext(ULONG_PTR process);
SVC NtUserSetProcessDpiAwarenessContext(ULONG_PTR context, ULONG_PTR reserved);
SVC NtUserGetDpiForMonitor(ULONG_PTR monitor, ULONG_PTR type, ULONG_PTR x, ULONG_PTR y);
SVC NtUserLogicalToPerMonitorDPIPhysicalPoint(ULONG_PTR hwnd, ULONG_PTR point);
IMPORT DWORD WINAPI GetProcessId(HANDLE process);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
#define DPI_CURRENT_PROCESS_ ((ULONG_PTR)-1)
#define DPI_INHERITED_ 0x80000000u
#define DPI_GDI_SCALED_ 0x40000000u
/* The dpi of the screen, whatever the awareness of the caller. */
static UINT real_system_dpi(void) {
  POINT_ origin = {0, 0};
  UINT x = 96, y = 96;
  HANDLE monitor = MonitorFromPoint(origin, 1 /* MONITOR_DEFAULTTOPRIMARY */);
  return monitor && NtUserGetDpiForMonitor((ULONG_PTR)monitor, 0, (ULONG_PTR)&x, (ULONG_PTR)&y) ? x : 96;
}
static UINT dpi_context_of(HANDLE handle) {
  switch ((LONG_PTR)handle) {
    case -1: return 0x6010;                              /* DPI_AWARENESS_CONTEXT_UNAWARE */
    case -2: return 0x11 | real_system_dpi() << 8;       /* SYSTEM_AWARE */
    case -3: return 0x12;                                /* PER_MONITOR_AWARE */
    case -4: return 0x22;                                /* PER_MONITOR_AWARE_V2 */
    case -5: return 0x40006010;                          /* UNAWARE_GDISCALED */
    default: return (UINT)(ULONG_PTR)handle;
  }
}
/* Windows' rules; `system_dpi` 0 accepts a system aware context of any dpi (IsValidDpiAwarenessContext), else it must be that dpi (the setters). */
static BOOL dpi_context_valid(UINT context, UINT system_dpi) {
  const UINT awareness = context & 0xF, version = (context >> 4) & 0xF, dpi = (context >> 8) & 0x1FF, flags = context & 0xFFFE0000u;
  if ((flags & ~0xE0000000u) != 0) return FALSE;
  switch (awareness) {
    case 0: return version == 1 && dpi == 96;
    case 1: return !(flags & DPI_GDI_SCALED_) && version == 1 && (system_dpi ? dpi == system_dpi : TRUE);
    case 2: return !(flags & DPI_GDI_SCALED_) && (version == 1 || version == 2) && dpi == 0;
    default: return FALSE;
  }
}
static UINT process_dpi_context(void) { return (UINT)NtUserGetProcessDpiAwarenessContext(DPI_CURRENT_PROCESS_); }
EXPORT BOOL WINAPI IsValidDpiAwarenessContext(HANDLE context) { return dpi_context_valid(dpi_context_of(context), 0); }
EXPORT int WINAPI GetAwarenessFromDpiAwarenessContext(HANDLE context) {
  const UINT c = dpi_context_of(context);
  return dpi_context_valid(c, 0) ? (int)(c & 0xF) : -1 /* DPI_AWARENESS_INVALID */;
}
EXPORT UINT WINAPI GetDpiFromDpiAwarenessContext(HANDLE context) {
  const UINT c = dpi_context_of(context);
  return dpi_context_valid(c, 0) ? (c >> 8) & 0x1FF : 0;
}
EXPORT BOOL WINAPI AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b) {
  const UINT ca = dpi_context_of(a), cb = dpi_context_of(b);
  if (!dpi_context_valid(ca, 0) || !dpi_context_valid(cb, 0)) return FALSE;
  return (ca & ~DPI_INHERITED_) == (cb & ~DPI_INHERITED_);
}
EXPORT BOOL WINAPI SetProcessDpiAwarenessContext(HANDLE context) {
  const UINT c = dpi_context_of(context);
  if (!dpi_context_valid(c, real_system_dpi())) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return BOOL_OF(NtUserSetProcessDpiAwarenessContext(c & ~DPI_INHERITED_, 0));
}
EXPORT BOOL WINAPI SetProcessDPIAware(void) {
  SetProcessDpiAwarenessContext((HANDLE)(LONG_PTR)-2);
  return TRUE;      /* (also when the awareness was set already, as on Windows) */
}
EXPORT BOOL WINAPI SetProcessDpiAwarenessInternal(int awareness) {
  if (awareness < 0 || awareness > 2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return BOOL_OF(NtUserSetProcessDpiAwarenessContext(0x10u | (UINT)awareness, 0));
}
/* The current process by any of its handles (NULL, the pseudo handle, a handle of its own); another process's handle answers that process's default; anything else is invalid. */
static BOOL dpi_process(HANDLE process, ULONG_PTR *which) {
  if (!process || process == (HANDLE)DPI_CURRENT_PROCESS_) { *which = DPI_CURRENT_PROCESS_; return TRUE; }
  const DWORD pid = GetProcessId(process);
  if (!pid) return FALSE;
  *which = pid == GetCurrentProcessId() ? DPI_CURRENT_PROCESS_ : (ULONG_PTR)process;
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessDpiAwarenessInternal(HANDLE process, int *awareness) {
  ULONG_PTR which;
  if (!awareness) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!dpi_process(process, &which)) { *awareness = -1; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *awareness = (int)(NtUserGetProcessDpiAwarenessContext(which) & 0xF);
  return TRUE;
}
EXPORT HANDLE WINAPI GetDpiAwarenessContextForProcess(HANDLE process) {
  ULONG_PTR which;
  if (!dpi_process(process, &which)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return (HANDLE)(ULONG_PTR)(UINT)NtUserGetProcessDpiAwarenessContext(which);
}
/* The thread's context: the service that sets it answers the previous one (marked DPI_INHERITED_ when it was the process's), and 0 makes the thread follow the process; so
 * reading it is such a set, undone when the thread had a context of its own. */
static UINT thread_dpi_context(void) {
  const UINT previous = (UINT)NtUserCallOneParam(0, 12 /* SetThreadDpiAwarenessContext */);
  if (previous && !(previous & DPI_INHERITED_)) NtUserCallOneParam(previous, 12);
  return previous & ~DPI_INHERITED_;
}
EXPORT HANDLE WINAPI SetThreadDpiAwarenessContext(HANDLE context) {
  const UINT c = dpi_context_of(context);
  if (!dpi_context_valid(c, real_system_dpi())) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return (HANDLE)(ULONG_PTR)(UINT)NtUserCallOneParam(c, 12 /* SetThreadDpiAwarenessContext */);
}
EXPORT HANDLE WINAPI GetThreadDpiAwarenessContext(void) { return (HANDLE)(ULONG_PTR)thread_dpi_context(); }
EXPORT BOOL WINAPI IsProcessDPIAware(void) { return (thread_dpi_context() & 0xF) != 0; }      /* (the thread's awareness, as on Windows) */
EXPORT HANDLE WINAPI GetWindowDpiAwarenessContext(HANDLE hwnd) {
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return NULL; }
  return (HANDLE)(ULONG_PTR)process_dpi_context();
}
EXPORT UINT WINAPI GetDpiForSystem(void) { return (UINT)NtUserGetSystemDpiForProcess(0); }
EXPORT UINT WINAPI GetSystemDpiForProcess(HANDLE process) { return (UINT)NtUserGetSystemDpiForProcess((ULONG_PTR)process); }
EXPORT UINT WINAPI GetDpiForWindow(HANDLE hwnd) {
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return 0; }
  const UINT context = process_dpi_context();
  if ((context & 0xF) == 0) return 96;      /* USER_DEFAULT_SCREEN_DPI */
  if ((context & 0xF) == 1) return (context >> 8) & 0x1FF;
  UINT x = 96, y = 96;
  HANDLE monitor = MonitorFromWindow(hwnd, 2 /* MONITOR_DEFAULTTONEAREST */);
  return monitor && NtUserGetDpiForMonitor((ULONG_PTR)monitor, 0, (ULONG_PTR)&x, (ULONG_PTR)&y) ? x : 96;
}
EXPORT int WINAPI GetSystemMetricsForDpi(int index, UINT dpi) { return (int)(NtUserCallTwoParam((ULONG_PTR)(LONG_PTR)index, dpi, 3 /* GetSystemMetricsForDpi */) & 0xFFFFFFFFu); }
EXPORT BOOL WINAPI EnableNonClientDpiScaling(HANDLE hwnd) {
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; }
  return TRUE;      /* (the frame is drawn at the system dpi already) */
}
/* One monitor at the system dpi: logical and physical coordinates are the same for every window (win32k checks the window and the point); a point outside the window, its
 * right and bottom edges included, is refused as on Windows. */
static BOOL dpi_point_identity(HANDLE hwnd, POINT_ *p) {
  RECT_ r;
  if (!p) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!BOOL_OF(NtUserLogicalToPerMonitorDPIPhysicalPoint((ULONG_PTR)hwnd, (ULONG_PTR)p))) return FALSE;
  if (!GetWindowRect(hwnd, &r) || p->x < r.left || p->x > r.right || p->y < r.top || p->y > r.bottom) return FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI LogicalToPhysicalPoint(HANDLE hwnd, POINT_ *p) { return dpi_point_identity(hwnd, p); }
EXPORT BOOL WINAPI PhysicalToLogicalPoint(HANDLE hwnd, POINT_ *p) { return dpi_point_identity(hwnd, p); }
EXPORT BOOL WINAPI LogicalToPhysicalPointForPerMonitorDPI(HANDLE hwnd, POINT_ *p) { return dpi_point_identity(hwnd, p); }
EXPORT BOOL WINAPI PhysicalToLogicalPointForPerMonitorDPI(HANDLE hwnd, POINT_ *p) { return dpi_point_identity(hwnd, p); }
