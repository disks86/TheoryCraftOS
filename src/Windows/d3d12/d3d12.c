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

/* d3d12.dll for TheoryCraftOS: Direct3D 12 (ID3D12Device, command queues, lists and allocators, fences, resources and heaps, descriptor heaps, root signatures, pipeline states,
 * D3D12CreateDevice and the root signature serializer).
 *
 * Where the work is done. Everything here is a user-mode driver of the kernel's Direct3D (src/Kernel/Dxgkrnl, reached with the D3DKMT thunks of gdi32.dll, see
 * src/Process/Win32k/Dxg): resources are allocations, queues are contexts, a command list records the kernel's command stream (CommandStream.hpp) and ExecuteCommandLists renders it,
 * fences are synchronization objects, root signatures and descriptor heaps are kernel objects made with the private escape, shaders are DXBC or DXIL (Shader Model 5 / 6)
 * the kernel translates for the adapter's device. Details of each part are in the .inc file; docs/win32k/Dxg.md lists what the kernel does.
 *
 * What Direct3D 12 has that this does not (the device says so with DXGI_ERROR_UNSUPPORTED, E_NOTIMPL or by ignoring the call, as the part's file says). The counts of vertex inputs,
 * interpolants, texture units, render targets and vertex buffers are the adapter's (DeviceInfo::limits of the kernel's graphics device, read with the UMDRIVERPRIVATE query): the CPU
 * renderer takes 16, 16, 16, 8 and 8; virgl (a GPU through virglrenderer) 16, 14 or 15, 16, 8 and 8, which is feature level 11_0 here; the other GPU backends what their
 * encoders were made for (feature level 9_x, so D3D12CreateDevice with the usual minimum of 11_0 is refused on them). tests/d3d12_qemu.sh runs the test program on virgl.
 * Whatever the device:
 *   - four unordered access buffers and four buffer SRVs, no 1D textures, mip chains and arrays only on sampled textures (a render target or depth buffer has one level and
 *     one layer, and a depth buffer cannot be sampled), no texture UAVs, buffer SRVs only on buffers made with unordered access, texture formats of four channels only
 *     (base.inc g_formats: no one- or two-channel, packed-float or block-compressed formats), depth bias and a back-face stencil state are not applied (the blend factor is: the CPU renderer and virgl),
 *     one viewport and scissor
 *   - a pass with a depth buffer and no render target draws into a colour image of the buffer's size that nothing reads (cmdlist.inc depth_companion): it costs that memory
 *   - sampler border colours are the three of a static sampler (another colour is the nearest of them); a device with wrap and clamp alone uses the nearer of those
 *   - multisampled images are render targets and depth buffers of the adapter's sample counts, drawn into and resolved (not read by shaders through this DLL)
 *   - pipeline statistics and stream output queries, tiled depth/multisampled/array resources, shared handles (DXGI_ERROR_UNSUPPORTED: no NT-handle sharing in win32k),
 *     resource state validation (barriers are accepted and not looked at, except aliasing), the debug layer, dual-source blending, unordered access to textures
 *   - of the later interfaces (ID3D12Device1 .. 10, ID3D12GraphicsCommandList1 .. 7: device1.inc, cmdlist1.inc, library.inc, rootsig.inc), the features reported at their
 *     "not supported" tier: ray tracing, mesh shaders, variable-rate shading, view instancing, depth bounds, sample positions, sampler feedback, protected sessions, meta
 *     commands
 *   - vertex elements: 32-bit floats on every device; bytes, 16-bit numbers, half floats and 32-bit integers (pso.inc vertex_format) where the device reads them (the CPU
 *     renderer and virgl)
 *     (queries are occlusion, binary occlusion and timestamps; ExecuteIndirect is made when the list is executed; bundles replay their calls: extras.inc)
 *   - Shader Model 6.x features beyond what the kernel's DXIL translator reads (it reads vertex, pixel, geometry, hull, domain and compute shaders; see Dxg.md)
 * Built freestanding over kernel32.dll, gdi32.dll and dxgi.dll's interfaces with the OS's mingw compiler. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "../tcosdx.h"

#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "base.inc"
#include "nullps.inc"
#include "resource.inc"
#include "heaps.inc"
#include "pso.inc"
#include "rootsig.inc"
#include "cmdlist.inc"
#include "cmdlist1.inc"
#include "extras.inc"
#include "queue.inc"
#include "device.inc"
#include "device1.inc"
#include "library.inc"

#include "vtbl.inc"

/* ---- the entry points -------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* (src/Windows/tcosdx.h) */
EXPORT void WINAPI TcosD3D12PipelineOptions(ID3D12Device *device, UINT pixel_constant_base, UINT shader_kind) {
  Dev *dev = (Dev *)device;
  if (!dev) return;
  dev->next_pixel_base = pixel_constant_base;
  dev->next_shader_kind = shader_kind;
}

EXPORT void WINAPI TcosD3D12StageConstantBases(ID3D12Device *device, UINT geometry, UINT hull, UINT domain) {
  Dev *dev = (Dev *)device;
  if (!dev) return;
  dev->next_stage_base[0] = geometry;
  dev->next_stage_base[1] = hull;
  dev->next_stage_base[2] = domain;
}
EXPORT void WINAPI TcosD3D12ResourceOptions(ID3D12Device *device, UINT flags) {
  Dev *dev = (Dev *)device;
  if (dev) dev->next_resource_flags = flags;
}
EXPORT void WINAPI TcosD3D12BeginQueryKind(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap, UINT index, UINT kind) {
  if (list) list_begin_query_kind((CmdList *)list, (QHeap *)heap, index, kind);
}

EXPORT HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum, REFIID riid, void **out) {
  LUID luid = {0, 0};
  NTSTATUS st = 0;
  if (minimum > D3D_FEATURE_LEVEL_11_0 && minimum != 0) return DXGI_ERROR_UNSUPPORTED;      /* (the levels above 11_0 need features the kernel's pipelines do not have) */
  if (adapter) {
    IDXGIAdapter *a = NULL;
    DXGI_ADAPTER_DESC desc;
    if (FAILED(adapter->lpVtbl->QueryInterface(adapter, &IID_IDXGIAdapter, (void **)&a))) return E_INVALIDARG;
    const HRESULT hr = IDXGIAdapter_GetDesc(a, &desc);
    IDXGIAdapter_Release(a);
    if (FAILED(hr)) return hr;
    luid = desc.AdapterLuid;
  } else {
    KmtAdapter list[16];
    const UINT n = kmt_enum_adapters(list, 16, NULL);
    if (n == 0) return DXGI_ERROR_UNSUPPORTED;
    luid = list[0].luid;
    for (UINT i = 0; i < n; ++i) kmt_close_adapter(list[i].handle);
  }
  if (!out) return S_FALSE;      /* (the question "could a device be made?") */
  *out = NULL;
  Dev *d = dev_create(luid, &st);
  if (!d) return KMT_OK(st) ? E_OUTOFMEMORY : DXGI_ERROR_UNSUPPORTED;
  /* (a program that asks for a feature level the adapter's limits do not allow gets what Direct3D 12 says: the adapter does not support it) */
  if (minimum != 0 && dev_feature_level(d) < minimum) {
    dx_Release(OBJ(d));
    return DXGI_ERROR_UNSUPPORTED;
  }
  return query_out((IUnknown *)d, riid, out);
}

EXPORT HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **out) {
  if (out) *out = NULL;
  return DXGI_ERROR_SDK_COMPONENT_MISSING;
}
EXPORT HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT count, const IID *iids, void *configuration, UINT *sizes) { return count == 0 ? S_OK : E_NOINTERFACE; }
EXPORT HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void **out) { if (out) *out = NULL; return E_NOINTERFACE; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_private_lock);
  }
  return TRUE;
}
