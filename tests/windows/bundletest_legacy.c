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

/* tests/windows/bundletest.c's draws with the older interfaces, each in a file of its own because their headers cannot be in one file with d3d9.h:
 * -DBUNDLE_DDRAW a Direct3D 7 device on a DirectDraw surface, -DBUNDLE_D3D8 a Direct3D 8 device on the window. Each draws a green quad into the left half of a
 * cleared target and reads it back. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { float x, y, z, rhw; DWORD diffuse; } TlVertex;
static int near_colour(DWORD p, DWORD want) {
  for (int s = 0; s < 24; s += 8) {
    const int a = (int)((p >> s) & 255), b = (int)((want >> s) & 255);
    if (a - b > 3 || b - a > 3) return 0;
  }
  return 1;
}
#define SIZE 64

#ifdef BUNDLE_DDRAW
#define INITGUID
#include <ddraw.h>
#include <d3d.h>
static DWORD lock_pixel7(IDirectDrawSurface7 *s, int x, int y) {
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) return 0xDEADBEEF;
  DWORD v = 0;
  memcpy(&v, (BYTE *)d.lpSurface + (SIZE_T)y * d.lPitch + (SIZE_T)x * 4, 4);
  IDirectDrawSurface7_Unlock(s, NULL);
  return v;
}
/* ddraw.dll (bare name: System32's), DirectDrawCreateEx, IDirect3D7::CreateDevice on an offscreen 3D surface. */
int bundle_draw_ddraw(HWND window) {
  HMODULE ddraw = LoadLibraryA("ddraw.dll");
  typedef HRESULT (WINAPI *CreateEx)(GUID *, void **, REFIID, IUnknown *);
  CreateEx create = ddraw ? (CreateEx)(void (*)(void))GetProcAddress(ddraw, "DirectDrawCreateEx") : NULL;
  IDirectDraw7 *dd = NULL;
  if (!create || FAILED(create(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) || !dd) return 0;
  int ok = 0;
  IDirect3D7 *d3d = NULL;
  IDirectDrawSurface7 *rt = NULL;
  IDirect3DDevice7 *device = NULL;
  IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL);
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.dwWidth = d.dwHeight = SIZE;
  d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
  d.ddpfPixelFormat.dwSize = sizeof d.ddpfPixelFormat;
  d.ddpfPixelFormat.dwFlags = DDPF_RGB;
  d.ddpfPixelFormat.dwRGBBitCount = 32;
  d.ddpfPixelFormat.dwRBitMask = 0xFF0000;
  d.ddpfPixelFormat.dwGBitMask = 0xFF00;
  d.ddpfPixelFormat.dwBBitMask = 0xFF;
  if (SUCCEEDED(IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) && d3d && SUCCEEDED(IDirectDraw7_CreateSurface(dd, &d, &rt, NULL)) && rt &&
      SUCCEEDED(IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, rt, &device)) && device) {
    IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    const TlVertex q[4] = {{0, 0, 0, 1, 0xFF00FF00}, {32, 0, 0, 1, 0xFF00FF00}, {0, 64, 0, 1, 0xFF00FF00}, {32, 64, 0, 1, 0xFF00FF00}};
    if (SUCCEEDED(IDirect3DDevice7_BeginScene(device))) {
      IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, (void *)q, 4, 0);
      IDirect3DDevice7_EndScene(device);
    }
    const DWORD left = lock_pixel7(rt, 10, 10), right = lock_pixel7(rt, 50, 10);
    ok = near_colour(left, 0x00FF00) && near_colour(right, 0x0000FF);
    if (!ok) printf("bundletest: ddraw read back %#lx %#lx\n", (unsigned long)left, (unsigned long)right);
  }
  if (device) IDirect3DDevice7_Release(device);
  if (rt) IDirectDrawSurface7_Release(rt);
  if (d3d) IDirect3D7_Release(d3d);
  IDirectDraw7_Release(dd);
  return ok;
}
#endif

#ifdef BUNDLE_D3D8
#include <d3d8.h>
/* Direct3DCreate8 of `d3d8`, a device with a lockable back buffer. */
int bundle_draw_d3d8(HMODULE d3d8, HWND window) {
  typedef IDirect3D8 *(WINAPI *Create8)(UINT);
  Create8 create = d3d8 ? (Create8)(void (*)(void))GetProcAddress(d3d8, "Direct3DCreate8") : NULL;
  IDirect3D8 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
  if (!d3d) return 0;
  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = pp.BackBufferHeight = SIZE;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = window;
  pp.Windowed = TRUE;
  pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  IDirect3DDevice8 *device = NULL;
  int ok = 0;
  if (SUCCEEDED(IDirect3D8_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)) && device) {
    const TlVertex q[4] = {{0, 0, 0.5f, 1, 0xFF00FF00}, {32, 0, 0.5f, 1, 0xFF00FF00}, {0, 64, 0.5f, 1, 0xFF00FF00}, {32, 64, 0.5f, 1, 0xFF00FF00}};
    IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_BeginScene(device);
    IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice8_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    IDirect3DDevice8_EndScene(device);
    IDirect3DSurface8 *back = NULL;
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(IDirect3DDevice8_GetBackBuffer(device, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
      if (SUCCEEDED(IDirect3DSurface8_LockRect(back, &locked, NULL, D3DLOCK_READONLY))) {
        const DWORD *row = (const DWORD *)((const BYTE *)locked.pBits + 10 * locked.Pitch);
        ok = near_colour(row[10], 0x00FF00) && near_colour(row[50], 0xFF0000);
        if (!ok) printf("bundletest: d3d8 read back %#lx %#lx\n", (unsigned long)row[10], (unsigned long)row[50]);
        IDirect3DSurface8_UnlockRect(back);
      }
      IDirect3DSurface8_Release(back);
    }
    IDirect3DDevice8_Release(device);
  }
  IDirect3D8_Release(d3d);
  return ok;
}
#endif
