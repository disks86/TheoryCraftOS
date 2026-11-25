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
 *   - fixed state the device lacks is refused (pso.inc): depth bias, depth clip, wireframe, two-sided stencil, dual-source blending, logic ops, anisotropy, custom border
 *     colours and several viewports need the kernel's RasterizerState, DualSourceBlend, LogicOps, SamplerState and Viewports features (the CPU renderer and virgl;
 *     virglrenderer 1.3.0 drops anisotropy, and virgl refuses a layer or viewport chosen in a vertex or domain shader)
 *   - texture formats: every colour format and BC1-BC7 (base.inc g_formats) on the CPU renderer and virgl, except R1_UNORM; R8 and R16 unorm are kept widened;
 *     depth buffers are sampled at level 0 only and are not copied; ResourceMinLODClamp is ignored; no gather from cube arrays
 *   - a pass with a depth buffer and no render target draws into a colour image of the buffer's size that nothing reads (cmdlist.inc depth_companion): it costs that memory
 *   - multisampled images are render targets and depth buffers of the adapter's sample counts, drawn into and resolved (not read by shaders through this DLL)
 *   - tiled resources at tier 1 only, heaps shared across adapters (DXGI_ERROR_UNSUPPORTED; shared handles of one adapter's resources and heaps, and of fences of any
 *     adapter, are made: device.inc), resource state validation (barriers are accepted and not looked at, except aliasing), the debug layer
 *   - of the later interfaces (ID3D12Device1 .. 14, ID3D12GraphicsCommandList1 .. 10: device1.inc, cmdlist1.inc, library.inc, rootsig.inc; what mingw lacks of them is in
 *     d3d12_types.h), the features reported at their "not supported" tier: ray tracing, mesh shaders, variable-rate shading, view instancing, depth bounds, sample
 *     positions, sampler feedback, work graphs, protected sessions, meta commands, D3D12_SAMPLER_DESC2, a strip cut set by the command list (OPTIONS15)
 *   - vertex elements: every Direct3D 11 input-assembler format and instance step rates (pso.inc vertex_format) where the device reads them (the CPU renderer and virgl)
 *     (queries are occlusion, binary occlusion, pipeline statistics (and 1), stream output statistics and timestamps (also on copy queues), any number open at once;
 *     stream output views stop at SizeInBytes and start after the filled size they hold when their draws run; ExecuteIndirect is made when the list is executed;
 *     bundles replay their calls: extras.inc). A filled size is written after each chunk of the list's stream (a draw and a copy of it later in the same chunk are
 *     put in two), not after each draw: a shader of a later draw of the chunk that reads it as a buffer sees the value before
 *   - input layouts are checked against the vertex shader's input signature as the runtime does (src/Windows/dx_layout.h); an input the shader declares and never reads
 *     is not looked for (not checked against Windows: kept lenient)
 *   - Shader Model 6.x features beyond what the kernel's DXIL translator reads (it reads vertex, pixel, geometry, hull, domain and compute shaders; see Dxg.md)
 * Built freestanding over kernel32.dll, gdi32.dll and dxgi.dll's interfaces with the OS's mingw compiler. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d12.h>
#include <d3d12video.h>
#include "d3d12_types.h"
#include "video_types.h"
#include <dxgi1_6.h>
#include "../tcosdx.h"
#include "../tcos_swizzle.h"
#include "../dx_layout.h"

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
#include "video.inc"
#include "encode.inc"
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
/* (src/Windows/tcosdx.h) What the next resource made D3D12_HEAP_FLAG_SHARED carries for the runtime above it (up to 64 bytes: d3d11.dll's description of its
 * texture), and what a resource - made shared, or opened from a shared handle - carries: its size (0: none), up to `capacity` bytes copied. */
EXPORT void WINAPI TcosD3D12ShareExtra(ID3D12Device *device, const void *data, UINT size) {
  Dev *dev = (Dev *)device;
  if (!dev) return;
  dev->next_share_extra_size = data && size <= sizeof dev->next_share_extra ? size : 0;
  if (dev->next_share_extra_size) memcpy(dev->next_share_extra, data, size);
}
EXPORT UINT WINAPI TcosD3D12SharedExtra(ID3D12Resource *resource, void *out, UINT capacity) {
  Resource *r = (Resource *)resource;
  if (!r) return 0;
  if (out) memcpy(out, r->share_extra, r->share_extra_size < capacity ? r->share_extra_size : capacity);
  return r->share_extra_size;
}
/* (src/Windows/tcosdx.h) The win32k resource of a resource made shared or opened from a shared handle (0: neither): what D3DKMTShareObjects shares, which d3d11.dll
 * shares itself together with the texture's keyed mutex. */
EXPORT UINT WINAPI TcosD3D12SharedResource(ID3D12Resource *resource) {
  extern const ID3D12Resource2Vtbl res_vtbl;
  Resource *r = (Resource *)resource;
  return r && r->d.o.lpVtbl == (const void *)&res_vtbl ? r->kresource : 0;
}
EXPORT UINT WINAPI TcosD3D12GlobalShare(ID3D12Resource *resource) {
  extern const ID3D12Resource2Vtbl res_vtbl;
  Resource *r = (Resource *)resource;
  return r && r->d.o.lpVtbl == (const void *)&res_vtbl ? r->global_share : 0;
}
EXPORT HRESULT WINAPI TcosD3D12OpenShared(ID3D12Device *device, HANDLE handle, int global, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!device || !handle || !riid || !out) return E_INVALIDARG;
  IUnknown *made = NULL;
  const HRESULT hr = open_shared((Dev *)device, (UINT64)(UINT_PTR)handle, global ? SHARED_GLOBAL : SHARED_NT, &made);
  if (FAILED(hr)) return hr;
  return query_out(made, riid, out);
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
