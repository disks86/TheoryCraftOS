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
/* The exports Windows' ddraw.dll has besides the DirectDraw objects: the global lock, GetSurfaceFromDC, and the entry points its own components (the
 * Direct3D runtime, dsound.dll, the application compatibility layer) call. Their "local surface" handles (DDRAWI_DDRAWSURFACE_LCL) are this
 * library's surfaces: GetDDSurfaceLocal hands one out and the others take it back, checked against the surfaces that exist. Included by ddraw.c. */

EXPORT void WINAPI AcquireDDThreadLock(void) { dd_lock(); }
EXPORT void WINAPI ReleaseDDThreadLock(void) { dd_unlock(); }

/* A surface of this process by its "local" handle (NULL: there is no such surface). */
static Surface *surface_from_local(const void *lcl) {
  if (!lcl) return NULL;
  for (DDraw *dd = g_ddraws; dd; dd = dd->next)
    for (Surface *s = dd->surfaces; s; s = s->list_next) if ((const void *)s == lcl) return s;
  return NULL;
}

/* GetSurfaceFromDC: the surface whose GetDC gave `dc` (any DirectDraw object's), its IDirectDrawSurface interface with a reference, and in *device the
 * display's device context. */
static HDC g_display_dc;
EXPORT HRESULT WINAPI GetSurfaceFromDC(HDC dc, IDirectDrawSurface4 **out, HDC *device) {
  if (!out) return E_INVALIDARG;
  if (!device) { *out = NULL; return E_INVALIDARG; }
  dd_lock();
  for (DDraw *dd = g_ddraws; dd; dd = dd->next) {
    if (dd->flags & DD_DEAD) continue;
    Surface *s;
    if (SUCCEEDED(dd_get_surface_from_dc(dd, dc, &s))) {
      s1_AddRef(IFACE1(s));
      *out = (IDirectDrawSurface4 *)IFACE1(s);
      if (!g_display_dc) g_display_dc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
      *device = g_display_dc;
      dd_unlock();
      return DD_OK;
    }
  }
  dd_unlock();
  *out = NULL;
  *device = NULL;
  return DDERR_NOTFOUND;
}

/* D3DParseUnknownCommand: for a driver's DrawPrimitives2, the Direct3D runtime's parsing of the commands the documentation names for it -
 * D3DDP2OP_VIEWPORTINFO and D3DDP2OP_WINFO: *next gets the command after it; any other command is D3DERR_COMMAND_UNPARSED. */
#define DD_DP2OP_VIEWPORTINFO 28u
#define DD_DP2OP_WINFO 29u
#define DD_ERR_COMMAND_UNPARSED ((HRESULT)0x88760BB8u)
EXPORT HRESULT WINAPI D3DParseUnknownCommand(void *command, void **next) {
  if (!command || !next) return DDERR_INVALIDPARAMS;
  const BYTE *c = command;
  const WORD count = (WORD)(c[2] | (c[3] << 8));      /* D3DHAL_DP2COMMAND { bCommand; bReserved; wStateCount } */
  switch (c[0]) {
    case DD_DP2OP_VIEWPORTINFO: *next = (void *)(c + 4 + (SIZE_T)count * 16); return DD_OK;      /* (D3DHAL_DP2VIEWPORTINFO: x, y, width, height) */
    case DD_DP2OP_WINFO: *next = (void *)(c + 4 + (SIZE_T)count * 8); return DD_OK;              /* (D3DHAL_DP2WINFO: near and far w) */
    default: return DD_ERR_COMMAND_UNPARSED;
  }
}

/* GetDDSurfaceLocal: the local handle of a surface (of the DirectDraw object given). */
EXPORT HRESULT WINAPI GetDDSurfaceLocal(IDirectDraw *dd, IDirectDrawSurface *surface, void **lcl) {
  if (!lcl) return DDERR_INVALIDPARAMS;
  *lcl = NULL;
  Surface *s = surface_from_any(surface);
  if (!s) return DDERR_INVALIDPARAMS;
  if (dd) {
    /* (any version's DirectDraw pointer; the surface must be that object's) */
    const void *vt = *(const void *const *)dd;
    DDraw *o = vt == &dd7_vtbl ? DD7(dd) : vt == &dd4_vtbl ? DD4(dd) : vt == &dd2_vtbl ? DD2(dd) : vt == &dd1_vtbl ? DD1(dd) : NULL;
    if (!o || o != s->dd) return DDERR_INVALIDPARAMS;
  }
  *lcl = s;
  return DD_OK;
}
EXPORT HRESULT WINAPI DDInternalLock(void *lcl, void **memory) {
  if (!memory) return DDERR_INVALIDPARAMS;
  dd_lock();
  Surface *s = surface_from_local(lcl);
  if (!s) { dd_unlock(); return DDERR_INVALIDPARAMS; }
  DDSURFACEDESC2 d;
  zero(&d, sizeof d);
  d.dwSize = sizeof d;
  HRESULT hr = surface_lock(s, NULL, &d, sizeof d, DDLOCK_WAIT, NULL);
  *memory = SUCCEEDED(hr) ? d.lpSurface : NULL;
  dd_unlock();
  return hr;
}
EXPORT HRESULT WINAPI DDInternalUnlock(void *lcl) {
  dd_lock();
  Surface *s = surface_from_local(lcl);
  HRESULT hr = s ? surface_unlock(s, NULL) : DDERR_INVALIDPARAMS;
  dd_unlock();
  return hr;
}
EXPORT HRESULT WINAPI DDGetAttachedSurfaceLcl(void *lcl, DDSCAPS2 *caps, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  dd_lock();
  Surface *s = surface_from_local(lcl), *a = NULL;
  HRESULT hr = s ? surface_get_attached(s, caps, 7, &a) : DDERR_INVALIDPARAMS;
  if (SUCCEEDED(hr)) *out = a;
  dd_unlock();
  return hr;
}
/* CompleteCreateSysmemSurface: the runtime's step after making a system-memory surface for a driver; surfaces here are whole when CreateSurface
 * returns, so there is nothing left but the check. */
EXPORT HRESULT WINAPI CompleteCreateSysmemSurface(void *lcl) {
  dd_lock();
  Surface *s = surface_from_local(lcl);
  HRESULT hr = !s ? DDERR_INVALIDPARAMS : (s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? DD_OK : DDERR_INVALIDSURFACETYPE;
  dd_unlock();
  return hr;
}
/* GetOLEThunkData: the data ddraw.dll keeps for the 16-bit OLE thunks of Windows 9x. There are no 16-bit thunks here: every index has none (0). */
EXPORT ULONG_PTR WINAPI GetOLEThunkData(DWORD index) { (void)index; return 0; }

/* RegisterSpecialCase: the application compatibility layer tells ddraw.dll which of its special cases a program needs; this DirectDraw has none of
 * those workarounds, and keeps what it was told (GetOLEThunkData does not hand it out either). */
static DWORD g_special_case[4];
EXPORT HRESULT WINAPI RegisterSpecialCase(DWORD a, DWORD b, DWORD c, DWORD d) {
  dd_lock();
  g_special_case[0] = a; g_special_case[1] = b; g_special_case[2] = c; g_special_case[3] = d;
  dd_unlock();
  return DD_OK;
}
/* SetAppCompatData (Windows Vista on): the compatibility switches of ddraw.dll by index (1 .. 12). The ones that change something here: none of them
 * concerns a behaviour this DirectDraw has (they switch off Windows' own emulation shortcuts); they are kept, and an index Windows does not know is
 * DDERR_INVALIDPARAMS. */
static DWORD g_app_compat[13];
EXPORT HRESULT WINAPI SetAppCompatData(DWORD index, DWORD data) {
  if (index < 1 || index > 12) return DDERR_INVALIDPARAMS;
  dd_lock();
  g_app_compat[index] = data;
  dd_unlock();
  return DD_OK;
}
/* DSoundHelp: dsound.dll tells ddraw.dll the window of a DirectSound object (and its process), so that an exclusive DirectDraw program and its sound
 * share the focus window. */
static HWND g_dsound_window;
EXPORT HRESULT WINAPI DSoundHelp(HWND window, DWORD pid, DWORD reserved) {
  (void)pid; (void)reserved;
  if (window && !IsWindow(window)) return DDERR_INVALIDPARAMS;
  dd_lock();
  g_dsound_window = window;
  dd_unlock();
  return DD_OK;
}
