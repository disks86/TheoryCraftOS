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

/* A ReShade-style proxy that a program brings in its own folder: d3d9.dll (-DPROXY_D3D9) or dxgi.dll (-DPROXY_DXGI). It loads the system's library of its own
 * name by its full path (GetSystemDirectory + "\d3d9.dll") - on Windows a second library of that name - and passes the calls on. The d3d9 proxy wraps
 * IDirect3D9 in an object of its own whose methods pass every call on (QueryInterface too, for the interfaces it does not know), as ReShade's does.
 * TcosProxyCalls says how many calls went through it. Used by tests/windows/bundletest.c (tests/bundled_dlls_qemu.sh).
 *   x86_64-w64-mingw32-gcc -O1 -shared -DPROXY_D3D9 -Wl,--kill-at -o d3d9.dll tests/windows/bundle_proxy.c -luuid */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>

#define EXPORT __declspec(dllexport)

static LONG g_calls;
static HMODULE g_system;
EXPORT LONG WINAPI TcosProxyCalls(void) { return g_calls; }
EXPORT HMODULE WINAPI TcosProxySystemLibrary(void) { return g_system; }

/* The system's library of this name, loaded by its path the first time it is needed (never in DllMain: the loader lock). */
static FARPROC system_function(const WCHAR *file, const char *name) {
  if (!g_system) {
    WCHAR path[MAX_PATH + 32];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return NULL;
    lstrcpyW(path + n, file);
    g_system = LoadLibraryW(path);
    if (!g_system) return NULL;
  }
  InterlockedIncrement(&g_calls);
  return GetProcAddress(g_system, name);
}

#ifdef PROXY_D3D9
/* ---- IDirect3D9 passed on ---- */
typedef struct { IDirect3D9 face; IDirect3D9 *real; LONG refs; } Wrapper;
static Wrapper *of(IDirect3D9 *face) { return (Wrapper *)face; }
static HRESULT WINAPI w_QueryInterface(IDirect3D9 *self, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirect3D9)) {
    *out = self;
    IDirect3D9_AddRef(self);
    return S_OK;
  }
  InterlockedIncrement(&g_calls);
  return IDirect3D9_QueryInterface(of(self)->real, iid, out);      /* (an interface it does not know: the real object's) */
}
static ULONG WINAPI w_AddRef(IDirect3D9 *self) { return (ULONG)InterlockedIncrement(&of(self)->refs); }
static ULONG WINAPI w_Release(IDirect3D9 *self) {
  const LONG left = InterlockedDecrement(&of(self)->refs);
  if (left == 0) {
    IDirect3D9_Release(of(self)->real);
    HeapFree(GetProcessHeap(), 0, of(self));
  }
  return (ULONG)left;
}
static HRESULT WINAPI w_RegisterSoftwareDevice(IDirect3D9 *self, void *f) { return IDirect3D9_RegisterSoftwareDevice(of(self)->real, f); }
static UINT WINAPI w_GetAdapterCount(IDirect3D9 *self) { return IDirect3D9_GetAdapterCount(of(self)->real); }
static HRESULT WINAPI w_GetAdapterIdentifier(IDirect3D9 *self, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id) { return IDirect3D9_GetAdapterIdentifier(of(self)->real, a, f, id); }
static UINT WINAPI w_GetAdapterModeCount(IDirect3D9 *self, UINT a, D3DFORMAT f) { return IDirect3D9_GetAdapterModeCount(of(self)->real, a, f); }
static HRESULT WINAPI w_EnumAdapterModes(IDirect3D9 *self, UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE *mode) { return IDirect3D9_EnumAdapterModes(of(self)->real, a, f, m, mode); }
static HRESULT WINAPI w_GetAdapterDisplayMode(IDirect3D9 *self, UINT a, D3DDISPLAYMODE *mode) { return IDirect3D9_GetAdapterDisplayMode(of(self)->real, a, mode); }
static HRESULT WINAPI w_CheckDeviceType(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT b, BOOL w) { return IDirect3D9_CheckDeviceType(of(self)->real, a, t, f, b, w); }
static HRESULT WINAPI w_CheckDeviceFormat(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT f, DWORD u, D3DRESOURCETYPE r, D3DFORMAT c) {
  return IDirect3D9_CheckDeviceFormat(of(self)->real, a, t, f, u, r, c);
}
static HRESULT WINAPI w_CheckDeviceMultiSampleType(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD *q) {
  return IDirect3D9_CheckDeviceMultiSampleType(of(self)->real, a, t, f, w, m, q);
}
static HRESULT WINAPI w_CheckDepthStencilMatch(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT r, D3DFORMAT d) {
  return IDirect3D9_CheckDepthStencilMatch(of(self)->real, a, t, f, r, d);
}
static HRESULT WINAPI w_CheckDeviceFormatConversion(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) { return IDirect3D9_CheckDeviceFormatConversion(of(self)->real, a, t, s, d); }
static HRESULT WINAPI w_GetDeviceCaps(IDirect3D9 *self, UINT a, D3DDEVTYPE t, D3DCAPS9 *caps) { return IDirect3D9_GetDeviceCaps(of(self)->real, a, t, caps); }
static HMONITOR WINAPI w_GetAdapterMonitor(IDirect3D9 *self, UINT a) { return IDirect3D9_GetAdapterMonitor(of(self)->real, a); }
static HRESULT WINAPI w_CreateDevice(IDirect3D9 *self, UINT a, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **device) {
  InterlockedIncrement(&g_calls);
  return IDirect3D9_CreateDevice(of(self)->real, a, t, w, f, pp, device);
}
static IDirect3D9Vtbl g_wrapper = {
  w_QueryInterface, w_AddRef, w_Release, w_RegisterSoftwareDevice, w_GetAdapterCount, w_GetAdapterIdentifier, w_GetAdapterModeCount, w_EnumAdapterModes,
  w_GetAdapterDisplayMode, w_CheckDeviceType, w_CheckDeviceFormat, w_CheckDeviceMultiSampleType, w_CheckDepthStencilMatch, w_CheckDeviceFormatConversion,
  w_GetDeviceCaps, w_GetAdapterMonitor, w_CreateDevice,
};

static const WCHAR kFile[] = L"\\d3d9.dll";
typedef IDirect3D9 *(WINAPI *Create9)(UINT);
typedef HRESULT (WINAPI *Create9Ex)(UINT, IDirect3D9Ex **);
EXPORT IDirect3D9 *WINAPI Direct3DCreate9(UINT version) {
  Create9 create = (Create9)(void (*)(void))system_function(kFile, "Direct3DCreate9");
  IDirect3D9 *real = create ? create(version) : NULL;
  if (!real) return NULL;
  Wrapper *w = HeapAlloc(GetProcessHeap(), 0, sizeof *w);
  if (!w) { IDirect3D9_Release(real); return NULL; }
  w->face.lpVtbl = &g_wrapper;
  w->real = real;
  w->refs = 1;
  return &w->face;
}
EXPORT HRESULT WINAPI Direct3DCreate9Ex(UINT version, IDirect3D9Ex **out) {
  Create9Ex create = (Create9Ex)(void (*)(void))system_function(kFile, "Direct3DCreate9Ex");
  return create ? create(version, out) : E_NOTIMPL;
}
#endif

#ifdef PROXY_DXGI
static const WCHAR kFile[] = L"\\dxgi.dll";
typedef HRESULT (WINAPI *Factory)(REFIID, void **);
typedef HRESULT (WINAPI *Factory2)(UINT, REFIID, void **);
EXPORT HRESULT WINAPI CreateDXGIFactory(REFIID iid, void **out) {
  Factory f = (Factory)(void (*)(void))system_function(kFile, "CreateDXGIFactory");
  return f ? f(iid, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI CreateDXGIFactory1(REFIID iid, void **out) {
  Factory f = (Factory)(void (*)(void))system_function(kFile, "CreateDXGIFactory1");
  return f ? f(iid, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID iid, void **out) {
  Factory2 f = (Factory2)(void (*)(void))system_function(kFile, "CreateDXGIFactory2");
  return f ? f(flags, iid, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID iid, void **out) {
  Factory2 f = (Factory2)(void (*)(void))system_function(kFile, "DXGIGetDebugInterface1");
  return f ? f(flags, iid, out) : E_NOTIMPL;
}
#endif

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) { (void)module; (void)reason; (void)reserved; return TRUE; }
