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

/* d3d8.dll: IDirect3D8 - the adapters, their modes and formats, and making a device. Included by d3d8.c. */

#define D3D8(o) (((Direct3D8 *)(o))->d9)

/* The formats Direct3D 8 enumerates display modes of, one list after the other (Direct3D 9 asks for one format at a time). */
static const D3DFORMAT g_mode_formats[] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A2R10G10B10};

static HRESULT STDMETHODCALLTYPE d3d_RegisterSoftwareDevice(Direct3D8 *o, void *function) { return CALL9(D3D8(o), RegisterSoftwareDevice, function); }
static UINT STDMETHODCALLTYPE d3d_GetAdapterCount(Direct3D8 *o) { return CALL9(D3D8(o), GetAdapterCount); }
/* D3DENUM_NO_WHQL_LEVEL (Direct3D 8: leave the WHQL level out) is the opposite of Direct3D 9's D3DENUM_WHQL_LEVEL (ask for it). */
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterIdentifier(Direct3D8 *o, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8 *id) {
  if (!id || (flags & ~(DWORD)D3DENUM_NO_WHQL_LEVEL)) return D3DERR_INVALIDCALL;
  D3DADAPTER_IDENTIFIER9 i9;
  const HRESULT hr = CALL9(D3D8(o), GetAdapterIdentifier, adapter, (flags & D3DENUM_NO_WHQL_LEVEL) ? 0 : D3D9_ENUM_WHQL_LEVEL, &i9);
  if (SUCCEEDED(hr)) identifier_from9(&i9, id);
  return hr;
}
static UINT STDMETHODCALLTYPE d3d_GetAdapterModeCount(Direct3D8 *o, UINT adapter) {
  UINT n = 0;
  for (UINT f = 0; f < sizeof g_mode_formats / sizeof g_mode_formats[0]; ++f) n += CALL9(D3D8(o), GetAdapterModeCount, adapter, g_mode_formats[f]);
  return n;
}
static HRESULT STDMETHODCALLTYPE d3d_EnumAdapterModes(Direct3D8 *o, UINT adapter, UINT index, D3DDISPLAYMODE *mode) {
  if (!mode) return D3DERR_INVALIDCALL;
  for (UINT f = 0; f < sizeof g_mode_formats / sizeof g_mode_formats[0]; ++f) {
    const UINT n = CALL9(D3D8(o), GetAdapterModeCount, adapter, g_mode_formats[f]);
    if (index < n) return CALL9(D3D8(o), EnumAdapterModes, adapter, g_mode_formats[f], index, mode);
    index -= n;
  }
  return D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterDisplayMode(Direct3D8 *o, UINT adapter, D3DDISPLAYMODE *mode) {
  if (!mode) return D3DERR_INVALIDCALL;
  return CALL9(D3D8(o), GetAdapterDisplayMode, adapter, mode);
}
/* (Direct3D 8 has no back buffer of the display's format by D3DFMT_UNKNOWN, which Direct3D 9 takes for a window) */
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceType(Direct3D8 *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) {
  if (back == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
  /* (and Direct3D 8 converts no format when it presents: the back buffer is the display's format, or that format with alpha) */
  const BOOL same = back == display || (display == D3DFMT_X8R8G8B8 && back == D3DFMT_A8R8G8B8) || (display == D3DFMT_X1R5G5B5 && back == D3DFMT_A1R5G5B5);
  if (!same) return D3DERR_NOTAVAILABLE;
  return CALL9(D3D8(o), CheckDeviceType, adapter, type, display, back, windowed);
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormat(Direct3D8 *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) {
  if (usage & ~USAGE8_MASK) return D3DERR_INVALIDCALL;
  /* (Direct3D 8 answers no question about buffers: every vertex or index buffer format can be made) */
  if (resource == D3DRTYPE_VERTEXBUFFER || resource == D3DRTYPE_INDEXBUFFER) return D3DERR_INVALIDCALL;
  return CALL9(D3D8(o), CheckDeviceFormat, adapter, type, display, usage, resource, format);
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceMultiSampleType(Direct3D8 *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE samples) {
  return CALL9(D3D8(o), CheckDeviceMultiSampleType, adapter, type, format, windowed, samples, NULL);
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDepthStencilMatch(Direct3D8 *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT target, D3DFORMAT depth) {
  return CALL9(D3D8(o), CheckDepthStencilMatch, adapter, type, display, target, depth);
}
static HRESULT STDMETHODCALLTYPE d3d_GetDeviceCaps(Direct3D8 *o, UINT adapter, D3DDEVTYPE type, D3DCAPS8 *caps) {
  if (!caps) return D3DERR_INVALIDCALL;
  D3DCAPS9 c9;
  const HRESULT hr = CALL9(D3D8(o), GetDeviceCaps, adapter, type, &c9);
  if (SUCCEEDED(hr)) caps_from9(&c9, caps);
  return hr;
}
static HMONITOR STDMETHODCALLTYPE d3d_GetAdapterMonitor(Direct3D8 *o, UINT adapter) { return CALL9(D3D8(o), GetAdapterMonitor, adapter); }

/* Unless the program asks to keep its own (D3DCREATE_FPU_PRESERVE), Direct3D 8 sets the x87 unit to single precision, rounding to the nearest, with
 * every exception masked - before the device is made, so the window's messages during CreateDevice see it too. */
static void fpu_setup(DWORD behaviour) {
  if (behaviour & D3DCREATE_FPU_PRESERVE) return;
#if defined(__i386__) || defined(__x86_64__)
  const unsigned short control = 0x007F;
  __asm__ volatile("fldcw %0" : : "m"(control));
#endif
}

static HRESULT STDMETHODCALLTYPE d3d_CreateDevice(Direct3D8 *o, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS *parameters,
                                                  IDirect3DDevice8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  D3DPRESENT_PARAMETERS9 p9;
  HRESULT hr = present_to9(parameters, &p9, FALSE);
  if (FAILED(hr)) return hr;
  const DWORD processing = behaviour & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING);
  if (processing != D3DCREATE_SOFTWARE_VERTEXPROCESSING && processing != D3DCREATE_HARDWARE_VERTEXPROCESSING && processing != D3DCREATE_MIXED_VERTEXPROCESSING)
    return D3DERR_INVALIDCALL;      /* (exactly one kind of vertex processing) */
  fpu_setup(behaviour);
  IDirect3DDevice9 *d9 = NULL;
  hr = CALL9(D3D8(o), CreateDevice, adapter, type, focus, behaviour, &p9, &d9);
  if (FAILED(hr)) return hr;
  Device8 *d = mem_alloc(sizeof *d);
  if (!d) { CALL9(d9, Release); return E_OUTOFMEMORY; }
  obj_init(&d->o, &dev_vtbl, KIND_DEVICE, NULL, (IUnknown *)d9);
  d->d9 = d9;
  d->parent = o;
  obj_AddRef(&o->o);
  InitializeCriticalSection(&d->lock);
  d->behaviour = behaviour;
  if (FAILED(CALL9(d9, GetDeviceCaps, &d->caps9))) memset(&d->caps9, 0, sizeof d->caps9);
  /* The vertex shader constants a program may set: the caps' (software vertex processing has Direct3D 8's 256 whatever the hardware has). */
  D3DCAPS8 c8;
  caps_from9(&d->caps9, &c8);
  d->vs_constants = (behaviour & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? D8_CONSTANTS : c8.MaxVertexShaderConst;
  state8_defaults(d);
  defaults8_apply(d);
  if (d->state.extra[EXTRA_SOFTWAREVERTEXPROCESSING]) CALL9(d9, SetSoftwareVertexProcessing, TRUE);
  present_from9(&p9, parameters);
  d->present = *parameters;
  implicit_refresh(d);
  *out = (IDirect3DDevice8 *)d;
  return D3D_OK;
}
