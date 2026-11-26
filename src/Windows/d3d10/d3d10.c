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

/* d3d10.dll for TheoryCraftOS: Direct3D 10's library. The device is d3d11.dll's (a Direct3D 11 device that is also ID3D10Device1, src/Windows/d3d11/d3d10.inc),
 * made through d3d10core.dll's D3D10CoreCreateDevice; this library adds what Direct3D 10 had around it:
 *   D3D10CreateDevice(AndSwapChain)   the adapter for the driver type, the device, a swap chain of dxgi.dll
 *   state blocks                      D3D10_STATE_BLOCK_MASK and ID3D10StateBlock (stateblock.inc)
 *   effects                           the Effects 10 framework over fx_4_0 / fx_4_1 binaries (effect.inc, fx4.h)
 *   shaders                           reflection, signatures, the disassembler and the preprocessor (shader.inc)
 *   blobs, profiles                   D3D10CreateBlob, D3D10Get*ShaderProfile
 * Compiling HLSL (D3D10CompileShader, D3D10CompileEffectFromMemory) is d3dcompiler_47.dll's D3DCompile, as Windows' later d3d10.dll does: this system has no HLSL compiler of
 * its own (its d3dcompiler_47.dll answers E_NOTIMPL), so a program that compiles at run time needs Microsoft's d3dcompiler_47.dll next to it. docs/Direct3D10.md. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d10_1.h>
#include <d3d10effect.h>
#include <d3d10shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define OBJ(x) ((DxObj *)(x))
#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release

/* (d3d10core.dll) */
__declspec(dllimport) HRESULT WINAPI D3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL level, ID3D10Device **device);

/* ---- blobs ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Blob { DxObj o; SIZE_T size; BYTE *data; } Blob;
static void blob_destroy(DxObj *o) { mem_free(((Blob *)o)->data); }
static void *STDMETHODCALLTYPE blob_GetBufferPointer(Blob *b) { return b->data; }
static SIZE_T STDMETHODCALLTYPE blob_GetBufferSize(Blob *b) { return b->size; }
extern const ID3D10BlobVtbl blob_vtbl;
/* A blob of `size` bytes (zeros), copied from `data` when there is some. */
static HRESULT blob_make(SIZE_T size, const void *data, ID3D10Blob **out) {
  static const GUID *const iids[] = {&IID_ID3D10Blob, NULL};
  *out = NULL;
  Blob *b = mem_alloc(sizeof *b);
  if (!b) return E_OUTOFMEMORY;
  dx_init(&b->o, &blob_vtbl, iids, blob_destroy);
  b->data = mem_alloc(size ? size : 1);
  if (!b->data) { mem_free(b); return E_OUTOFMEMORY; }
  b->size = size;
  if (data && size) memcpy(b->data, data, size);
  *out = (ID3D10Blob *)b;
  return S_OK;
}
EXPORT HRESULT WINAPI D3D10CreateBlob(SIZE_T size, ID3D10Blob **out) {
  if (!out) return E_INVALIDARG;
  return blob_make(size, NULL, out);
}

/* ---- devices ------------------------------------------------------------------------------------------------------------------------------------------------------ */

/* The adapter of a driver type, and the device of `level` on it. An adapter given goes with the hardware driver type only; WARP is the default adapter here (this system's
 * adapters are its own renderers); the reference rasterizer (d3d10ref.dll) and the NULL device are SDK parts this system has not; a software rasterizer is the program's
 * module, which dxgi.dll's CreateSoftwareAdapter takes. */
static HRESULT d10_device_on(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, D3D_FEATURE_LEVEL level, ID3D10Device **out) {
  IDXGIFactory *factory = NULL;
  HRESULT hr;
  if (adapter) {
    if (type != D3D10_DRIVER_TYPE_HARDWARE) return E_INVALIDARG;
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
    if (FAILED(hr)) return hr;
    IDXGIAdapter_AddRef(adapter);
  } else {
    if ((UINT)type > D3D10_DRIVER_TYPE_WARP) return E_INVALIDARG;
    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
    if (FAILED(hr)) return hr;
    switch (type) {
      case D3D10_DRIVER_TYPE_HARDWARE:
      case D3D10_DRIVER_TYPE_WARP:
        hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
        break;
      case D3D10_DRIVER_TYPE_SOFTWARE:
        hr = software ? IDXGIFactory_CreateSoftwareAdapter(factory, software, &adapter) : E_FAIL;
        break;
      default: {
        /* (the reference rasterizer - for the NULL device too - is d3d10ref.dll of the DirectX SDK) */
        HMODULE ref = LoadLibraryA("d3d10ref.dll");
        if (!ref) { hr = E_FAIL; break; }
        hr = IDXGIFactory_CreateSoftwareAdapter(factory, ref, &adapter);
        FreeLibrary(ref);
        break;
      }
    }
    if (FAILED(hr)) { IDXGIFactory_Release(factory); return hr; }
  }
  hr = D3D10CoreCreateDevice(factory, adapter, flags, level, out);
  IDXGIAdapter_Release(adapter);
  IDXGIFactory_Release(factory);
  return hr;
}
EXPORT HRESULT WINAPI D3D10CreateDevice(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, UINT sdk, ID3D10Device **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (sdk != D3D10_SDK_VERSION) return E_INVALIDARG;
  return d10_device_on(adapter, type, software, flags, D3D_FEATURE_LEVEL_10_0, out);
}
/* The device, then a swap chain of the adapter's factory for it (no device when the swap chain cannot be made). */
static HRESULT d10_swap_chain_for(ID3D10Device *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain) {
  IDXGIDevice *dxgi = NULL;
  IDXGIAdapter *adapter = NULL;
  IDXGIFactory *factory = NULL;
  HRESULT hr = ID3D10Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
  if (SUCCEEDED(hr)) { hr = IDXGIDevice_GetAdapter(dxgi, &adapter); IDXGIDevice_Release(dxgi); }
  if (SUCCEEDED(hr)) { hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory); IDXGIAdapter_Release(adapter); }
  if (SUCCEEDED(hr)) { hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, desc, chain); IDXGIFactory_Release(factory); }
  return hr;
}
EXPORT HRESULT WINAPI D3D10CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, UINT sdk, DXGI_SWAP_CHAIN_DESC *desc,
                                                    IDXGISwapChain **chain, ID3D10Device **out) {
  /* (the device is needed, the swap chain only when one is asked for: its description is then the swap chain's to check) */
  if (chain) *chain = NULL;
  if (!out) return E_INVALIDARG;
  HRESULT hr = D3D10CreateDevice(adapter, type, software, flags, sdk, out);
  if (FAILED(hr) || !chain) return hr;
  hr = d10_swap_chain_for(*out, desc, chain);
  if (FAILED(hr)) { ID3D10Device_Release(*out); *out = NULL; }
  return hr;
}

/* ---- profiles ------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* The best profile for the device: shader model 4.1 on a 10.1 device, 4.0 on a 10.0 one (and with no device), 4.0 of level 9 on a 9 one (which has no geometry stage). */
static D3D_FEATURE_LEVEL d10_level_of(ID3D10Device *device) {
  ID3D10Device1 *d1 = NULL;
  if (!device || FAILED(ID3D10Device_QueryInterface(device, &IID_ID3D10Device1, (void **)&d1)) || !d1) return D3D_FEATURE_LEVEL_10_0;
  const D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)ID3D10Device1_GetFeatureLevel(d1);
  ID3D10Device1_Release(d1);
  return level;
}
EXPORT const char *WINAPI D3D10GetVertexShaderProfile(ID3D10Device *device) {
  const D3D_FEATURE_LEVEL l = d10_level_of(device);
  return l >= D3D_FEATURE_LEVEL_10_1 ? "vs_4_1" : l >= D3D_FEATURE_LEVEL_10_0 ? "vs_4_0" : l >= D3D_FEATURE_LEVEL_9_3 ? "vs_4_0_level_9_3" : "vs_4_0_level_9_1";
}
EXPORT const char *WINAPI D3D10GetPixelShaderProfile(ID3D10Device *device) {
  const D3D_FEATURE_LEVEL l = d10_level_of(device);
  return l >= D3D_FEATURE_LEVEL_10_1 ? "ps_4_1" : l >= D3D_FEATURE_LEVEL_10_0 ? "ps_4_0" : l >= D3D_FEATURE_LEVEL_9_3 ? "ps_4_0_level_9_3" : "ps_4_0_level_9_1";
}
EXPORT const char *WINAPI D3D10GetGeometryShaderProfile(ID3D10Device *device) {
  const D3D_FEATURE_LEVEL l = d10_level_of(device);
  return l >= D3D_FEATURE_LEVEL_10_1 ? "gs_4_1" : l >= D3D_FEATURE_LEVEL_10_0 ? "gs_4_0" : NULL;
}

/* ---- compiling: d3dcompiler_47.dll's ---------------------------------------------------------------------------------------------------------------------------------- */

typedef HRESULT (WINAPI *D3DCompileFn)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *, const char *, const char *, UINT, UINT, ID3DBlob **,
                                        ID3DBlob **);
static HRESULT d10_compile(const void *data, SIZE_T size, const char *file, const D3D10_SHADER_MACRO *defines, ID3D10Include *include, const char *entry, const char *profile,
                           UINT flags, UINT effect_flags, ID3D10Blob **code, ID3D10Blob **errors) {
  if (code) *code = NULL;
  if (errors) *errors = NULL;
  if (!data || !code) return E_INVALIDARG;
  HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
  D3DCompileFn compile = m ? (D3DCompileFn)(void (*)(void))GetProcAddress(m, "D3DCompile") : NULL;
  if (!compile) return E_NOTIMPL;      /* (no compiler in this system: see above) */
  const HRESULT hr = compile(data, size, file, (const D3D_SHADER_MACRO *)defines, (ID3DInclude *)include, entry, profile, flags, effect_flags, code, errors);
  if (SUCCEEDED(hr) && !*code) return E_NOTIMPL;      /* (a compiler that answered without code compiled nothing) */
  return hr;
}
EXPORT HRESULT WINAPI D3D10CompileShader(const char *data, SIZE_T size, const char *file, const D3D10_SHADER_MACRO *defines, ID3D10Include *include, const char *entry,
                                         const char *profile, UINT flags, ID3D10Blob **code, ID3D10Blob **errors) {
  return d10_compile(data, size, file, defines, include, entry, profile, flags, 0, code, errors);
}
EXPORT HRESULT WINAPI D3D10CompileEffectFromMemory(void *data, SIZE_T size, const char *file, const D3D10_SHADER_MACRO *defines, ID3D10Include *include, UINT flags,
                                                   UINT effect_flags, ID3D10Blob **effect, ID3D10Blob **errors) {
  return d10_compile(data, size, file, defines, include, NULL, "fx_4_0", flags, effect_flags, effect, errors);
}

/* ---- the undocumented exports -------------------------------------------------------------------------------------------------------------------------------------- */

/* The runtime's version: that of the SDK it was built for. */
EXPORT UINT WINAPI D3D10GetVersion(void) { return D3D10_SDK_VERSION; }
/* The SDK layers (the debug layer, the switch to the reference rasterizer) register here. No SDK layer is part of this system: what Windows answers without the SDK
 * component, as d3d11.dll's D3D11CoreRegisterLayers. */
EXPORT HRESULT WINAPI D3D10RegisterLayers(void) { return DXGI_ERROR_SDK_COMPONENT_MISSING; }
/* Vista's switch back to the runtime before its service pack: there is no older runtime here. */
EXPORT HRESULT WINAPI RevertToOldImplementation(void) { return E_NOTIMPL; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_private_lock);
  return TRUE;
}

#include "stateblock.inc"
#include "shader.inc"
#include "effect.inc"
#include "vtbl.inc"
