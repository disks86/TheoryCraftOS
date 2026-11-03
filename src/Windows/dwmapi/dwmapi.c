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

/* dwmapi.dll for TheoryCraftOS: the Desktop Window Manager API. Windows keeps the state of the compositor in dwm.exe and dwmapi.dll talks to it over ALPC; this OS composes in
 * the kernel, so the same state (the DWMWA_* attributes of windows, frame margins, blur behind, thumbnails, iconic bitmaps) is kept by win32k (src/Process/Win32k/
 * Win32kUserMiscDwm.cpp) and reached with the one service NtTcosDwm. What is done here is what Windows decides in the DLL: checking the arguments, the HRESULTs, the attribute
 * table (sizes, ranges, read-only attributes, defaults), merging the partial DWM_BLURBEHIND, and the timing of the (fixed 60 Hz) refresh that DwmFlush and
 * DwmGetCompositionTimingInfo report. Composition is always on (since Windows 8 it cannot be turned off). Not here: the exports for the remote-desktop and touch-feedback
 * parts of the DWM (DwmShowContact ..., DwmRenderGesture, the Dwmp* functions), the MIL content and graphics-stream functions, and DwmTransitionOwnedWindow /
 * DwmSetPresentParameters; they stay stubs (src/Windows/stubs/dwmapi.inc). Built freestanding over kernel32.dll and ntdll.dll with the OS's mingw compiler. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#undef DECLSPEC_IMPORT      /* (dwmapi.h declares its functions imported: these are the definitions) */
#define DECLSPEC_IMPORT
#include <dwmapi.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

__declspec(dllimport) NTSTATUS NTAPI NtTcosDwm(ULONG operation, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d);

/* NtTcosDwm operations (src/Process/Win32k/Win32kUserMiscDwm.cpp). */
#define DWM_OP_SET_PROP 1
#define DWM_OP_GET_PROP 2
#define DWM_OP_REGISTER_THUMBNAIL 3
#define DWM_OP_UNREGISTER_THUMBNAIL 4
#define DWM_OP_UPDATE_THUMBNAIL 5
#define DWM_OP_THUMBNAIL_SOURCE_SIZE 6

/* The keys of the per-window values besides the DWMWA_* attributes (1 .. DWMWA_LAST - 1). */
#define DWM_KEY_MARGINS 0x100
#define DWM_KEY_BLUR 0x101
#define DWM_KEY_ICONIC_THUMBNAIL 0x102
#define DWM_KEY_LIVE_PREVIEW 0x103

#define THUMBNAIL_PROPS_WIRE_SIZE 48      /* DWM_THUMBNAIL_PROPERTIES is packed (45 bytes); the kernel copies this many */
#define DWM_TNP_ALL (DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY)

#define E_HANDLE_ ((HRESULT)0x80070006)
#define E_ACCESSDENIED_ ((HRESULT)0x80070005)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000E)
#define E_INVALIDARG_ ((HRESULT)0x80070057)
#define E_NOTIMPL_ ((HRESULT)0x80004001)
#define E_FAIL_ ((HRESULT)0x80004005)
#define E_POINTER_ ((HRESULT)0x80004003)
#define HR_INVALID_WINDOW ((HRESULT)0x80070578)      /* HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) */

#define STATUS_NOT_FOUND_ ((NTSTATUS)0xC0000225)
#define STATUS_INVALID_WINDOW_ ((NTSTATUS)0xC0000300)

#define DWMWA_COLOR_DEFAULT_ 0xFFFFFFFFu
#define REFRESH_RATE 60
#define MAX_DX_FRAME_DURATION 8

static void copy_bytes(void *dest, const void *source, SIZE_T n) {
  unsigned char *d = dest;
  const unsigned char *s = source;
  while (n--) *d++ = *s++;
}
static void zero_bytes(void *dest, SIZE_T n) {
  unsigned char *d = dest;
  while (n--) *d++ = 0;
}

static HRESULT hresult_of(NTSTATUS status) {
  if (status >= 0) return S_OK;
  switch ((ULONG)status) {
    case 0xC0000300u: return HR_INVALID_WINDOW;
    case 0xC0000022u: return E_ACCESSDENIED_;
    case 0xC0000008u: return E_HANDLE_;
    case 0xC0000017u: return E_OUTOFMEMORY_;
    case 0xC000000Du: case 0xC0000004u: return E_INVALIDARG_;
    case 0xC0000005u: return E_POINTER_;
    default: return E_FAIL_;
  }
}

static NTSTATUS set_value(HWND window, ULONG key, const void *data, ULONG size) {
  return NtTcosDwm(DWM_OP_SET_PROP, (ULONG_PTR)window, key, (ULONG_PTR)data, size);
}
static NTSTATUS get_value(HWND window, ULONG key, void *out, ULONG size) {
  return NtTcosDwm(DWM_OP_GET_PROP, (ULONG_PTR)window, key, (ULONG_PTR)out, size);
}
/* Whether `window` is a window: asking for a value no one stores answers "not found" for a window and "invalid window" for anything else. */
static HRESULT check_window(HWND window) {
  const NTSTATUS status = get_value(window, 0xFFFF, &(DWORD){0}, sizeof(DWORD));
  return status == STATUS_NOT_FOUND_ ? S_OK : status < 0 ? hresult_of(status) : S_OK;
}

/* ---- composition state ---------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI DwmIsCompositionEnabled(WINBOOL *enabled) {
  if (!enabled) return E_INVALIDARG_;
  *enabled = TRUE;
  return S_OK;
}

EXPORT HRESULT WINAPI DwmEnableComposition(UINT action) {
  (void)action;      /* since Windows 8 composition is always on: the call does nothing */
  return S_OK;
}

EXPORT HRESULT WINAPI DwmEnableMMCSS(WINBOOL enable) {
  (void)enable;
  return S_OK;
}

EXPORT HRESULT WINAPI DwmGetColorizationColor(DWORD *colorization, WINBOOL *opaque_blend) {
  if (!colorization || !opaque_blend) return E_INVALIDARG_;
  *colorization = 0xC40078D7u;      /* the Windows default accent colour, ARGB */
  *opaque_blend = FALSE;
  return S_OK;
}

EXPORT WINBOOL WINAPI DwmDefWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result) {
  (void)window; (void)message; (void)wparam; (void)lparam;
  if (result) *result = 0;      /* there are no caption buttons drawn by the compositor to hit-test: the program handles the message itself */
  return FALSE;
}

/* ---- timing: the display refreshes at 60 Hz, counted from the first QueryPerformanceCounter tick of the system ------------------------------------------------- */
static void refresh_state(ULONGLONG *period, ULONGLONG *now, ULONGLONG *count, ULONGLONG *vblank) {
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  *period = (ULONGLONG)frequency.QuadPart / REFRESH_RATE;
  if (*period == 0) *period = 1;
  *now = (ULONGLONG)counter.QuadPart;
  *count = *now / *period;
  *vblank = *count * *period;
}

EXPORT HRESULT WINAPI DwmFlush(void) {
  ULONGLONG period, now, count, vblank;
  refresh_state(&period, &now, &count, &vblank);
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  const ULONGLONG remaining = vblank + period - now;      /* QPC ticks to the next refresh */
  const ULONGLONG ms = remaining * 1000 / (ULONGLONG)frequency.QuadPart;
  Sleep((DWORD)(ms ? ms : 1));
  return S_OK;
}

EXPORT HRESULT WINAPI DwmGetCompositionTimingInfo(HWND window, DWM_TIMING_INFO *info) {
  if (!info) return E_INVALIDARG_;
  if (info->cbSize != sizeof(DWM_TIMING_INFO)) return E_INVALIDARG_;
  if (window) {
    const HRESULT hr = check_window(window);
    if (FAILED(hr)) return hr;
  }
  ULONGLONG period, now, count, vblank;
  refresh_state(&period, &now, &count, &vblank);
  zero_bytes(info, sizeof(*info));
  info->cbSize = sizeof(DWM_TIMING_INFO);
  info->rateRefresh.uiNumerator = REFRESH_RATE;
  info->rateRefresh.uiDenominator = 1;
  info->qpcRefreshPeriod = period;
  info->rateCompose.uiNumerator = REFRESH_RATE;
  info->rateCompose.uiDenominator = 1;
  info->qpcVBlank = vblank;
  info->cRefresh = count;
  info->qpcCompose = vblank;
  info->cFrame = count;
  info->cRefreshFrame = count;
  info->cFrameSubmitted = count;
  info->cFrameConfirmed = count;
  info->cRefreshConfirmed = count;
  info->cFrameDisplayed = count;
  info->qpcFrameDisplayed = vblank;
  info->cRefreshFrameDisplayed = count;
  info->cFrameComplete = count;
  info->qpcFrameComplete = vblank;
  info->cFramePending = count;
  info->qpcFramePending = vblank;
  info->cFramesDisplayed = count;
  info->cFramesComplete = count;
  info->cFramesPending = count;
  info->cFramesAvailable = count;
  info->cRefreshNextDisplayed = count + 1;
  info->cRefreshNextPresented = count + 1;
  info->cRefreshesDisplayed = count;
  info->cRefreshesPresented = count;
  info->cRefreshStarted = count;
  return S_OK;
}

EXPORT HRESULT WINAPI DwmSetDxFrameDuration(HWND window, INT refreshes) {
  if (refreshes < 1 || refreshes > MAX_DX_FRAME_DURATION) return E_INVALIDARG_;
  return check_window(window);
}

EXPORT HRESULT WINAPI DwmModifyPreviousDxFrameDuration(HWND window, INT refreshes, WINBOOL relative) {
  (void)relative;
  if (refreshes < -MAX_DX_FRAME_DURATION || refreshes > MAX_DX_FRAME_DURATION) return E_INVALIDARG_;
  return check_window(window);
}

/* ---- window attributes ---------------------------------------------------------------------------------------------------------------------------------- */
/* What an attribute is: its size and range (the largest value allowed, 0 = any) and whether the program may set it; the defaults of the unset ones are in get_default(). */
static int attribute_info(DWORD attribute, ULONG *size, DWORD *maximum, int *settable) {
  *size = sizeof(DWORD);
  *maximum = 0;
  *settable = 1;
  switch (attribute) {
    case DWMWA_NCRENDERING_ENABLED: case DWMWA_CLOAKED: case DWMWA_VISIBLE_FRAME_BORDER_THICKNESS: *settable = 0; return 1;
    case DWMWA_CAPTION_BUTTON_BOUNDS: case DWMWA_EXTENDED_FRAME_BOUNDS: *size = sizeof(RECT); *settable = 0; return 1;
    case DWMWA_NCRENDERING_POLICY: *maximum = DWMNCRP_ENABLED; return 1;
    case DWMWA_FLIP3D_POLICY: *maximum = DWMFLIP3D_EXCLUDEABOVE; return 1;
    case DWMWA_WINDOW_CORNER_PREFERENCE: *maximum = DWMWCP_ROUNDSMALL; return 1;
    case DWMWA_SYSTEMBACKDROP_TYPE: *maximum = 4; return 1;
    case DWMWA_TRANSITIONS_FORCEDISABLED: case DWMWA_ALLOW_NCPAINT: case DWMWA_NONCLIENT_RTL_LAYOUT: case DWMWA_FORCE_ICONIC_REPRESENTATION: case DWMWA_HAS_ICONIC_BITMAP:
    case DWMWA_DISALLOW_PEEK: case DWMWA_EXCLUDED_FROM_PEEK: case DWMWA_CLOAK: case DWMWA_FREEZE_REPRESENTATION: case DWMWA_PASSIVE_UPDATE_MODE: case DWMWA_USE_HOSTBACKDROPBRUSH:
    case DWMWA_USE_IMMERSIVE_DARK_MODE: case DWMWA_BORDER_COLOR: case DWMWA_CAPTION_COLOR: case DWMWA_TEXT_COLOR: return 1;
    default: return 0;
  }
}

EXPORT HRESULT WINAPI DwmSetWindowAttribute(HWND window, DWORD attribute, LPCVOID value, DWORD size) {
  ULONG want;
  DWORD maximum;
  int settable;
  if (!attribute_info(attribute, &want, &maximum, &settable) || !settable || !value || size != want) return E_INVALIDARG_;
  DWORD v = 0;
  copy_bytes(&v, value, sizeof(v));
  if (maximum != 0 && v > maximum) return E_INVALIDARG_;
  /* Booleans are stored as 0 / 1 (a BOOL with any non-zero value is TRUE); the colour attributes keep what the program gave. */
  if (attribute != DWMWA_BORDER_COLOR && attribute != DWMWA_CAPTION_COLOR && attribute != DWMWA_TEXT_COLOR && maximum == 0) v = v != 0;
  return hresult_of(set_value(window, attribute, &v, sizeof(v)));
}

EXPORT HRESULT WINAPI DwmGetWindowAttribute(HWND window, DWORD attribute, PVOID value, DWORD size) {
  ULONG want;
  DWORD maximum;
  int settable;
  if (!attribute_info(attribute, &want, &maximum, &settable) || !value || size != want) return E_INVALIDARG_;
  if (attribute == DWMWA_CAPTION_BUTTON_BOUNDS) return E_NOTIMPL_;      /* the caption buttons are drawn by the program's window manager, not by the compositor */
  if (attribute == DWMWA_EXTENDED_FRAME_BOUNDS) return hresult_of(get_value(window, attribute, value, want));
  DWORD v = 0;
  NTSTATUS status = get_value(window, attribute, &v, sizeof(v));
  if (status == STATUS_NOT_FOUND_) {
    HRESULT hr = check_window(window);
    if (FAILED(hr)) return hr;
    switch (attribute) {      /* the value of a window that never set it */
      case DWMWA_NCRENDERING_ENABLED: v = TRUE; break;
      case DWMWA_VISIBLE_FRAME_BORDER_THICKNESS: v = 1; break;
      case DWMWA_BORDER_COLOR: case DWMWA_CAPTION_COLOR: case DWMWA_TEXT_COLOR: v = DWMWA_COLOR_DEFAULT_; break;
      case DWMWA_CLOAKED: {
        DWORD cloak = 0;
        if (get_value(window, DWMWA_CLOAK, &cloak, sizeof(cloak)) >= 0 && cloak) v = DWM_CLOAKED_APP;
        break;
      }
      default: v = 0;
    }
    status = 0;
  }
  if (status < 0) return hresult_of(status);
  if (attribute == DWMWA_CLOAKED && v == 0) {      /* a cloaked window (DWMWA_CLOAK) reports why */
    DWORD cloak = 0;
    if (get_value(window, DWMWA_CLOAK, &cloak, sizeof(cloak)) >= 0 && cloak) v = DWM_CLOAKED_APP;
  }
  copy_bytes(value, &v, sizeof(v));
  return S_OK;
}

/* ---- frame and blur ------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND window, const MARGINS *margins) {
  if (!margins) return E_INVALIDARG_;
  MARGINS m;
  copy_bytes(&m, margins, sizeof(m));
  return hresult_of(set_value(window, DWM_KEY_MARGINS, &m, sizeof(m)));
}

EXPORT HRESULT WINAPI DwmEnableBlurBehindWindow(HWND window, const DWM_BLURBEHIND *blur) {
  if (!blur) return E_INVALIDARG_;
  DWM_BLURBEHIND given, merged;
  copy_bytes(&given, blur, sizeof(given));
  if (given.dwFlags & ~(DWORD)(DWM_BB_ENABLE | DWM_BB_BLURREGION | DWM_BB_TRANSITIONONMAXIMIZED)) return E_INVALIDARG_;
  zero_bytes(&merged, sizeof(merged));
  const NTSTATUS had = get_value(window, DWM_KEY_BLUR, &merged, sizeof(merged));      /* only the fields named by dwFlags change */
  if (had < 0 && had != STATUS_NOT_FOUND_) return hresult_of(had);
  if (given.dwFlags & DWM_BB_ENABLE) merged.fEnable = given.fEnable != 0;
  if (given.dwFlags & DWM_BB_BLURREGION) merged.hRgnBlur = given.hRgnBlur;
  if (given.dwFlags & DWM_BB_TRANSITIONONMAXIMIZED) merged.fTransitionOnMaximized = given.fTransitionOnMaximized != 0;
  merged.dwFlags = 0;
  return hresult_of(set_value(window, DWM_KEY_BLUR, &merged, sizeof(merged)));
}

/* ---- thumbnails ----------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI DwmRegisterThumbnail(HWND destination, HWND source, PHTHUMBNAIL thumbnail) {
  if (!thumbnail) return E_INVALIDARG_;
  HANDLE handle = NULL;
  const NTSTATUS status = NtTcosDwm(DWM_OP_REGISTER_THUMBNAIL, (ULONG_PTR)destination, (ULONG_PTR)source, (ULONG_PTR)&handle, 0);
  if (status < 0) return hresult_of(status);
  *thumbnail = handle;
  return S_OK;
}

EXPORT HRESULT WINAPI DwmUnregisterThumbnail(HTHUMBNAIL thumbnail) {
  return hresult_of(NtTcosDwm(DWM_OP_UNREGISTER_THUMBNAIL, (ULONG_PTR)thumbnail, 0, 0, 0));
}

EXPORT HRESULT WINAPI DwmUpdateThumbnailProperties(HTHUMBNAIL thumbnail, const DWM_THUMBNAIL_PROPERTIES *properties) {
  if (!properties) return E_INVALIDARG_;
  unsigned char wire[THUMBNAIL_PROPS_WIRE_SIZE];
  zero_bytes(wire, sizeof(wire));
  copy_bytes(wire, properties, sizeof(DWM_THUMBNAIL_PROPERTIES));
  DWORD flags = 0;
  copy_bytes(&flags, wire, sizeof(flags));
  if (flags == 0 || (flags & ~(DWORD)DWM_TNP_ALL)) return E_INVALIDARG_;
  return hresult_of(NtTcosDwm(DWM_OP_UPDATE_THUMBNAIL, (ULONG_PTR)thumbnail, (ULONG_PTR)wire, 0, 0));
}

EXPORT HRESULT WINAPI DwmQueryThumbnailSourceSize(HTHUMBNAIL thumbnail, PSIZE size) {
  if (!size) return E_INVALIDARG_;
  LONG s[2] = {0, 0};
  const NTSTATUS status = NtTcosDwm(DWM_OP_THUMBNAIL_SOURCE_SIZE, (ULONG_PTR)thumbnail, (ULONG_PTR)s, 0, 0);
  if (status < 0) return hresult_of(status);
  size->cx = s[0];
  size->cy = s[1];
  return S_OK;
}

/* ---- iconic representations (taskbar thumbnails and Aero Peek of a window that is not shown) ----------------------------------------------------------------- */
EXPORT HRESULT WINAPI DwmSetIconicThumbnail(HWND window, HBITMAP bitmap, DWORD flags) {
  if (flags & ~(DWORD)DWM_SIT_DISPLAYFRAME) return E_INVALIDARG_;
  const HANDLE value[2] = {bitmap, (HANDLE)(ULONG_PTR)flags};
  return hresult_of(set_value(window, DWM_KEY_ICONIC_THUMBNAIL, value, sizeof(value)));
}

EXPORT HRESULT WINAPI DwmSetIconicLivePreviewBitmap(HWND window, HBITMAP bitmap, POINT *client_offset, DWORD flags) {
  if (flags & ~(DWORD)DWM_SIT_DISPLAYFRAME) return E_INVALIDARG_;
  LONG value[6] = {0};
  copy_bytes(value, &bitmap, sizeof(bitmap));
  if (client_offset) { value[2] = client_offset->x; value[3] = client_offset->y; }
  value[4] = (LONG)flags;
  return hresult_of(set_value(window, DWM_KEY_LIVE_PREVIEW, value, sizeof(value)));
}

EXPORT HRESULT WINAPI DwmInvalidateIconicBitmaps(HWND window) {
  /* The compositor asks the program for new bitmaps (WM_DWMSENDICONICTHUMBNAIL); the ones it has are dropped. */
  const HRESULT hr = check_window(window);
  if (FAILED(hr)) return hr;
  const HANDLE none[2] = {NULL, NULL};
  const LONG none_preview[6] = {0};
  HRESULT a = hresult_of(set_value(window, DWM_KEY_ICONIC_THUMBNAIL, none, sizeof(none)));
  HRESULT b = hresult_of(set_value(window, DWM_KEY_LIVE_PREVIEW, none_preview, sizeof(none_preview)));
  return FAILED(a) ? a : b;
}
