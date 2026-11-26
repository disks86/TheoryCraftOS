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

/* Monitors and display settings of user32.dll (included by window.inc after the scrolling): win32k knows the desktop's monitors (one, the screen, unless the kernel's desktop
 * set more). GetMonitorInfo and the MonitorFrom functions answer for them (a point or window off every monitor is handled by the flags: nothing, the primary monitor or the
 * nearest), EnumDisplayMonitors calls back for each (for a device context: the primary only), and the display settings come from the window manager (the A forms convert
 * DEVMODEA and DISPLAY_DEVICEA to and from the W structures; the names of devices are ASCII). */

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
  HANDLE primary = MonitorFromRect(&zero, 1 /* MONITOR_DEFAULTTOPRIMARY */);
  if (!primary) return FALSE;
  /* win32k numbers the monitors from the primary's handle up (Win32kUserMiscMonitor.cpp); the first handle it does not know ends the list */
  const DWORD saved_error = GetLastError();
  for (ULONG_PTR i = 0; i < 8; ++i) {
    HANDLE monitor = (HANDLE)((ULONG_PTR)primary + i);
    MonitorInfoW_ info;
    for (size_t_ k = 0; k < sizeof info; ++k) ((BYTE *)&info)[k] = 0;
    info.size = 40;
    if (!GetMonitorInfoW(monitor, &info)) { if (i == 0) return FALSE; break; }
    RECT_ area = info.monitor;
    if (hdc) {      /* in device coordinates: the rectangle of the monitor within the clip of the context, from the origin of its window (the primary only) */
      RECT_ box;
      if (i > 0) break;
      if (GetClipBox(hdc, &box) == 0 /* ERROR */) return FALSE;
      area.left = 0; area.top = 0; area.right = info.monitor.right - info.monitor.left; area.bottom = info.monitor.bottom - info.monitor.top;
    }
    if (clip) { RECT_ meet; if (!rect_meet(&area, clip, &meet)) continue; area = meet; }
    if (!proc(monitor, hdc, &area, data)) break;
  }
  SetLastError(saved_error);
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
static HANDLE *list_windows(HANDLE parent, BOOL recursive, DWORD thread, ULONG *count);
EXPORT LONG_PTR WINAPI SendMessageTimeoutW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, UINT flags, UINT timeout, ULONG_PTR *result);
/* The current mode as width, height and bits (0s: none). */
static void display_mode_now(DWORD *w, DWORD *h, DWORD *bpp) {
  BYTE dm[220];
  for (UINT i = 0; i < sizeof dm; ++i) dm[i] = 0;
  *(WORD *)(dm + 68) = 220;
  *w = *h = *bpp = 0;
  if (!EnumDisplaySettingsExW(NULL, (DWORD)-1 /* ENUM_CURRENT_SETTINGS */, dm, 0)) return;
  *bpp = *(DWORD *)(dm + 168);
  *w = *(DWORD *)(dm + 172);
  *h = *(DWORD *)(dm + 176);
}
EXPORT LONG WINAPI ChangeDisplaySettingsExW(LPCWSTR device, void *devmode, HANDLE hwnd, DWORD flags, void *lparam) {
  UNICODE_STRING us;
  if (device) us_name(&us, device);
  DWORD w0, h0, b0;
  display_mode_now(&w0, &h0, &b0);
  const LONG r = (LONG)NtUserChangeDisplaySettings(device ? (ULONG_PTR)&us : 0, (ULONG_PTR)devmode, (ULONG_PTR)hwnd, flags, (ULONG_PTR)lparam);
  if (r == 0 /* DISP_CHANGE_SUCCESSFUL */ && !(flags & 2 /* CDS_TEST */)) {
    /* (a mode that changed is told to every top-level window: WM_DISPLAYCHANGE with the depth and the size, as Windows sends it) */
    DWORD w1, h1, b1;
    display_mode_now(&w1, &h1, &b1);
    if (w1 != w0 || h1 != h0 || b1 != b0) {
      ULONG n = 0;
      HANDLE *list = list_windows(NULL, FALSE, 0, &n);
      for (ULONG i = 0; list && i < n; ++i) {
        ULONG_PTR result = 0;
        SendMessageTimeoutW(list[i], 0x007E /* WM_DISPLAYCHANGE */, b1, (LONG_PTR)(((h1 & 0xFFFF) << 16) | (w1 & 0xFFFF)), 0x0002 /* SMTO_ABORTIFHUNG */, 2000, &result);
      }
      u32_free(list);
    }
  }
  return r;
}
EXPORT LONG WINAPI ChangeDisplaySettingsW(void *devmode, DWORD flags) { return ChangeDisplaySettingsExW(NULL, devmode, NULL, flags, NULL); }

/* DEVMODEA <-> DEVMODEW: the 32 characters of the device name, then dmSpecVersion .. dmCollate (38 bytes), the 32 characters of the form name, then
 * dmLogPixels .. the end (54 bytes); DEVMODEA is 156 bytes (dmSize at 36), DEVMODEW 220 (dmSize at 68). Older, shorter DEVMODEAs keep what they have. */
#define DEVMODEA_SIZE_ 156u
#define DEVMODEW_SIZE_ 220u
static void devmode_a_to_w(const BYTE *a, BYTE *w) {
  for (UINT i = 0; i < DEVMODEW_SIZE_; ++i) w[i] = 0;
  UINT size = *(const WORD *)(a + 36);
  if (size > DEVMODEA_SIZE_) size = DEVMODEA_SIZE_;
  for (UINT i = 0; i < 32; ++i) ((WCHAR *)w)[i] = (BYTE)a[i];
  for (UINT i = 0; i < 38 && 32 + i < size; ++i) w[64 + i] = a[32 + i];
  for (UINT i = 0; i < 32 && 70 + i < size; ++i) ((WCHAR *)(w + 102))[i] = (BYTE)a[70 + i];
  for (UINT i = 0; i < 54 && 102 + i < size; ++i) w[166 + i] = a[102 + i];
  *(WORD *)(w + 68) = (WORD)DEVMODEW_SIZE_;
  *(WORD *)(w + 70) = 0;      /* (dmDriverExtra: none is passed on) */
}
static void devmode_w_to_a(const BYTE *w, BYTE *a, UINT size) {
  BYTE full[DEVMODEA_SIZE_];
  for (UINT i = 0; i < DEVMODEA_SIZE_; ++i) full[i] = 0;
  for (UINT i = 0; i < 32; ++i) { const WCHAR c = ((const WCHAR *)w)[i]; full[i] = (BYTE)(c < 0x80 ? c : '?'); }
  for (UINT i = 0; i < 38; ++i) full[32 + i] = w[64 + i];
  for (UINT i = 0; i < 32; ++i) { const WCHAR c = ((const WCHAR *)(w + 102))[i]; full[70 + i] = (BYTE)(c < 0x80 ? c : '?'); }
  for (UINT i = 0; i < 54; ++i) full[102 + i] = w[166 + i];
  if (size > DEVMODEA_SIZE_) size = DEVMODEA_SIZE_;
  *(WORD *)(full + 36) = (WORD)size;
  *(WORD *)(full + 38) = 0;
  for (UINT i = 0; i < size; ++i) a[i] = full[i];
}
/* A device name of the A forms as a wide string (NULL stays NULL). */
static const WCHAR *device_name_wide(const char *name, WCHAR *out, UINT capacity) {
  if (!name) return NULL;
  UINT i = 0;
  for (; name[i] && i + 1 < capacity; ++i) out[i] = (BYTE)name[i];
  out[i] = 0;
  return out;
}
EXPORT BOOL WINAPI EnumDisplaySettingsExA(LPCSTR device, DWORD mode, void *devmode, DWORD flags) {
  if (!devmode) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR name[64];
  BYTE w[DEVMODEW_SIZE_];
  for (UINT i = 0; i < DEVMODEW_SIZE_; ++i) w[i] = 0;
  *(WORD *)(w + 68) = (WORD)DEVMODEW_SIZE_;
  if (!EnumDisplaySettingsExW(device_name_wide(device, name, 64), mode, w, flags)) return FALSE;
  UINT size = *(WORD *)((BYTE *)devmode + 36);
  if (size < 124 || size > DEVMODEA_SIZE_) size = DEVMODEA_SIZE_;      /* (the whole DEVMODEA unless dmSize names an older, shorter one) */
  devmode_w_to_a(w, devmode, size);
  return TRUE;
}
EXPORT BOOL WINAPI EnumDisplaySettingsA(LPCSTR device, DWORD mode, void *devmode) { return EnumDisplaySettingsExA(device, mode, devmode, 0); }
EXPORT LONG WINAPI ChangeDisplaySettingsExA(LPCSTR device, void *devmode, HANDLE hwnd, DWORD flags, void *lparam) {
  WCHAR name[64];
  BYTE w[DEVMODEW_SIZE_];
  if (devmode) devmode_a_to_w(devmode, w);
  return ChangeDisplaySettingsExW(device_name_wide(device, name, 64), devmode ? w : NULL, hwnd, flags, lparam);
}
EXPORT LONG WINAPI ChangeDisplaySettingsA(void *devmode, DWORD flags) { return ChangeDisplaySettingsExA(NULL, devmode, NULL, flags, NULL); }
/* DISPLAY_DEVICEA: cb, DeviceName[32], DeviceString[128], StateFlags, DeviceID[128], DeviceKey[128] (424 bytes); the W form is 840. */
EXPORT BOOL WINAPI EnumDisplayDevicesA(LPCSTR device, DWORD index, void *info, DWORD flags) {
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD cb = *(DWORD *)info;
  if (cb < 168 || cb > 424) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }      /* (at least through StateFlags) */
  WCHAR name[64];
  BYTE w[840];
  for (UINT i = 0; i < sizeof w; ++i) w[i] = 0;
  *(DWORD *)w = 840;
  if (!EnumDisplayDevicesW(device_name_wide(device, name, 64), index, w, flags)) return FALSE;
  BYTE a[424];
  for (UINT i = 0; i < sizeof a; ++i) a[i] = 0;
  static const struct { UINT a, w, chars; } strings[] = {{4, 4, 32}, {36, 68, 128}, {168, 328, 128}, {296, 584, 128}};
  for (UINT k = 0; k < 4; ++k)
    for (UINT i = 0; i < strings[k].chars; ++i) { const WCHAR c = ((WCHAR *)(w + strings[k].w))[i]; a[strings[k].a + i] = (BYTE)(c < 0x80 ? c : '?'); }
  *(DWORD *)(a + 164) = *(DWORD *)(w + 324);
  *(DWORD *)a = cb;
  for (UINT i = 0; i < cb; ++i) ((BYTE *)info)[i] = a[i];
  return TRUE;
}

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
