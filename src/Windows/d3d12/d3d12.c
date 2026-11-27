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
 * renderer takes 16, 16, 16, 8 and 8; virgl (a GPU through virglrenderer) 16, 14 or 15, 16, 8 and 8; the other GPU backends what their encoders were made for (feature level
 * 9_x, so D3D12CreateDevice with the usual minimum of 11_0 is refused on them). tests/d3d12_qemu.sh runs the test program on virgl.
 * Feature levels (device.inc dev_feature_level): 11_1 on the CPU renderer (logic operations, UAVs in every stage, target-independent rasterization with ForcedSampleCount up to
 * 16), 11_0 on virgl and Venus (no forced sample count). None above 11_1: 12_0 needs resource binding tier 2 - the kernel binds 16 texture units and samplers a stage - and
 * Shader Model 6.0's wave operations; 12_1 adds conservative rasterization tier 1 and rasterizer ordered views, which the CPU renderer (and Venus the views) have and OPTIONS
 * reports on their own.
 * Whatever the device:
 *   - fixed state the device lacks is refused (pso.inc): depth bias, depth clip, wireframe, two-sided stencil, dual-source blending, logic ops, anisotropy, custom border
 *     colours and several viewports need the kernel's RasterizerState, DualSourceBlend, LogicOps, SamplerState and Viewports features (the CPU renderer and virgl;
 *     virglrenderer 1.3.0 drops anisotropy, and virgl refuses a layer or viewport chosen in a vertex or domain shader)
 *   - texture formats: every colour format and BC1-BC7 (base.inc g_formats) on the CPU renderer and virgl, R1_UNORM sampled and loaded (no render target, unordered access,
 *     volume, cube or CPU-visible memory: the format's support); R8 / R8G8 / R16 / R16G16 unorm and typeless as the kernel's own narrow images where the adapter has them for
 *     the texture's use (widened elsewhere, where unordered access to them, volumes' slices too, goes through mirrors that give the view's channels back; a typeless one's views
 *     of the other kinds of its family are mirrors - of a volume a volume of the view's levels; at most 4096 subresources)
 *   - depth buffers: shader resource views of any levels and layers (heaps.inc depth_srv), copied as whole subresources a plane at a time (the stencil's subresources after the
 *     depth's, footprints of R32_TYPELESS words and R8_TYPELESS bytes: cmdlist.inc copy_whole_region), as multisampled images are; not read by shaders while they are the pass's
 *     depth buffer, not of several samples when read by shaders, the samples of a multisampled one not copied to buffers (Direct3D 12 has no such copy)
 *   - ResourceMinLODClamp (a view's least level of detail, in the resource's levels) raises the sampler's MinLOD in the kernel; SampleCmp of a mipmapped texture compares the view's
 *     first level on virgl
 *   - samplers: the MINIMUM_ / MAXIMUM_ filters and coordinates in texels (D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES) where the kernel's device has them (the CPU renderer
 *     and Venus - whose shaders divide the coordinates by the level's size -; a draw with one is refused on virgl); the separate minification and magnification filters on the
 *     CPU renderer, Venus and virgl (one filter, linear where either is, elsewhere)
 *   - lines: aliased, alpha-antialiased and quadrilateral 1.4 / 1.0 wide on the CPU renderer; virgl's are the host's smooth and multisampled lines (its aliased line on a target
 *     of one sample); the other devices draw aliased lines and refuse narrow quadrilateral ones
 *   - a pass with a depth buffer and no render target draws into a colour image of the buffer's size that nothing reads (cmdlist.inc depth_companion): it costs that memory
 *   - multisampled images are render targets and depth buffers of the adapter's sample counts, drawn into, resolved and read by shaders (TEXTURE2DMS / TEXTURE2DMSARRAY views
 *     of colour targets and of depth buffers' depth and stencil, MULTISAMPLE_LOAD in the format support) where the kernel's device has DeviceFeature3 bits 0-2 (the CPU renderer,
 *     virgl, Venus); arrays of them (DepthOrArraySize > 1) drawn into a slice at a time and resolved a slice at a time (ResolveSubresource / ResolveSubresourceRegion of a slice);
 *     no unordered access to them, no CPU access to their samples, no 16-bit or wider multisampled formats (the kernel's multisampled images are 8-bit colour and depth), the least and
 *     greatest of a slice other than 0 not on virgl and Venus
 *   - tiled resources (extras.inc, base.inc dev_tiled_tier): tier 4 on the CPU renderer and Venus (the host's sparse images and sparse lookups, where its blocks are the
 *     tiles), tier 1 on virgl (TGSI has no residency feedback or lookup clamp); reserved depth buffers
 *     of D32_FLOAT and D16_UNORM where the kernel's device moves depth planes (base.inc format_tiled_depth; not D24S8 / D32S8: two planes), no multisampled reserved textures (no
 *     device moves a multisampled image's samples to or from memory: no backing for the tiles, no tiled quality level); the status of buffer and UAV loads (ld_raw_s /
 *     ld_structured_s / ld_uav_typed_s) on the CPU renderer (refused on Venus, whose SPIR-V has no residency of buffers: the shader is not made); heaps shared across adapters (DXGI_ERROR_UNSUPPORTED; shared handles of one adapter's resources and heaps, and of fences of any
 *     adapter, are made: device.inc), resource state validation (barriers are accepted and not looked at, except aliasing), the debug layer
 *   - of the later interfaces (ID3D12Device1 .. 14, ID3D12GraphicsCommandList1 .. 10: device1.inc, cmdlist1.inc, library.inc, rootsig.inc; what mingw lacks of them is in
 *     d3d12_types.h), the features reported at their "not supported" tier: ray tracing, mesh shaders, sampler feedback, work graphs, protected sessions, meta commands
 *     (D3D12_SAMPLER_DESC2, the strip cut of the command list and triangle fans are done: heaps.inc sampler_descriptor, cmdlist1.inc IASetIndexBufferStripCutValue); and on
 *     virgl (the CPU renderer has them: cmdlist1.inc) the depth bounds test, programmable sample positions, view instancing, variable-rate shading and barycentrics; on
 *     Venus all of them where the host's Vulkan has them (sample locations, fragment shading rate, fragment shader barycentrics; view instancing is the draw loop), the
 *     variable-rate shading tier 2 without AdditionalShadingRatesSupported where the host's coarse pixels stop at 2x2 (RADV: DEV_FEATURE3_COARSE_PIXELS_2X2_ONLY)
 *   - of those: view instancing is tier 2 (draw-call looping; every stage runs for every view); centroid, noperspective and sample interpolation, per-sample shading
 *     (SV_SampleIndex), EvaluateAttributeAtSample / Snapped / Centroid, SV_Coverage in and out and the rasterizer's sample count and positions are the kernel's (DeviceFeature3:
 *     the CPU renderer, virgl and Venus); SV_InnerCoverage on the CPU renderer only, whose conservative rasterization stays tier 1 (tier 3 wants the post-snap degenerate triangles of
 *     tier 2 drawn: the CPU renderer culls a triangle of no area)
 *   - HighestShaderModel (device.inc): 6.1 on the CPU renderer, whose lane model runs Shader Model 6.0's wave and quad operations (and 6.5's WaveMatch / WaveMultiPrefix*,
 *     6.7's QuadAny / QuadAll): OPTIONS1 WaveOps, WaveLaneCountMin 4 (a pixel shader's wave is its quad), Max 32 (a compute shader's), Int64ShaderOps; 5.1 on virgl and Venus,
 *     whose TGSI has no wave operations (the shaders are refused there; DXIL of 6.1 - 6.6 with SV_ViewID, SV_Barycentrics, GetAttributeAtVertex, SV_ShadingRate and
 *     IsHelperLane is taken there too); not 6.2, whose fp32 denormal modes the interpreter does not keep
 *   - vertex elements: every Direct3D 11 input-assembler format and instance step rates (pso.inc vertex_format) where the device reads them (the CPU renderer and virgl); a slot
 *     with no buffer (a null view) reads 0 - the components the format lacks 0, 0, 0, 1 -, and a stride of 0 gives every vertex the first element (the kernel's
 *     DEV_FEATURE3_NULL_VERTEX_BUFFERS / ZERO_STRIDE: the CPU renderer, virgl, Venus); input slots 0 .. 7 (Direct3D 12 has 32)
 *     (queries are occlusion, binary occlusion, pipeline statistics (and 1), stream output statistics and timestamps (also on copy queues), any number open at once;
 *     stream output views stop at SizeInBytes and start after the filled size they hold when their draws run; ExecuteIndirect is made when the list is executed;
 *     bundles replay their calls: extras.inc - they inherit the caller's root arguments, and setting the root signature already set, in a bundle or a list, keeps them). A filled size is written after each chunk of the list's stream (a draw and a copy of it later in the same chunk are
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
#include "agility.inc"

/* ---- the entry points -------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* (src/Windows/tcosdx.h) The private exports take only this library's own objects: a proxy's wrapper is seen through (IID_ITcosObject, tcos_object.h), anything else - a
 * foreign runtime's object - is refused with E_INVALIDARG (a count of 0) and never touched. */
static Dev *own_dev(const void *p) { return (Dev *)tcos_own(p, &dev_vtbl); }
static Resource *own_res(const void *p) { return (Resource *)tcos_own(p, &res_vtbl); }
static CmdList *own_list(const void *p) { return (CmdList *)tcos_own(p, &list_vtbl); }
static QHeap *own_qheap(const void *p) { return (QHeap *)tcos_own(p, &qheap_vtbl); }

/* (src/Windows/tcosdx.h) This library's own object behind `object` - the object itself, or the one a proxy's wrapper holds (IID_ITcosObject) - as interface `riid`
 * (AddRef'd): what the runtimes above take a program's Direct3D 12 objects with (d3d11.dll's D3D11On12CreateDevice and CreateWrappedResource), so that every
 * call they make afterwards goes to an object of this library. E_INVALIDARG for an object of another runtime (no QueryInterface but IID_ITcosObject is asked of it),
 * E_NOINTERFACE when this library's object has no such interface. */
EXPORT HRESULT WINAPI TcosD3D12Object(IUnknown *object, REFIID riid, void **out) {
  static const void *const kinds[] = {&dev_vtbl, &queue_vtbl, &alloc_vtbl, &list_vtbl, &fence_vtbl, &res_vtbl, &heap_vtbl, &dheap_vtbl, &rs_vtbl, &pso_vtbl, &qheap_vtbl,
                                      &sig_vtbl, &plib_vtbl, &scache_vtbl, &ltrack_vtbl, &vdev_vtbl, &vdec_vtbl, &vdheap_vtbl, &vproc_vtbl, &vdlist_vtbl, &vplist_vtbl,
                                      &vme_vtbl, &vmvh_vtbl, &venc_vtbl, &veheap_vtbl, &velist_vtbl};
  if (out) *out = NULL;
  if (!object || !riid || !out) return E_INVALIDARG;
  IUnknown *own = (IUnknown *)tcos_own_any(object, kinds, sizeof kinds / sizeof kinds[0]);
  if (!own) return E_INVALIDARG;
  return own->lpVtbl->QueryInterface(own, riid, out);
}

EXPORT HRESULT WINAPI TcosD3D12PipelineOptions(ID3D12Device *device, UINT pixel_constant_base, UINT shader_kind) {
  Dev *dev = own_dev(device);
  if (!dev) return E_INVALIDARG;
  dev->next_pixel_base = pixel_constant_base;
  dev->next_shader_kind = shader_kind;
  return S_OK;
}

EXPORT HRESULT WINAPI TcosD3D12StageConstantBases(ID3D12Device *device, UINT geometry, UINT hull, UINT domain) {
  Dev *dev = own_dev(device);
  if (!dev) return E_INVALIDARG;
  dev->next_stage_base[0] = geometry;
  dev->next_stage_base[1] = hull;
  dev->next_stage_base[2] = domain;
  return S_OK;
}
EXPORT HRESULT WINAPI TcosD3D12DeviceFeatures(ID3D12Device *device, UINT *features, UINT *features2) {
  const Dev *dev = own_dev(device);
  if (features) *features = dev ? dev->caps.features : 0;
  if (features2) *features2 = dev ? dev->caps.features2 : 0;
  return dev ? S_OK : E_INVALIDARG;
}
/* The third word of the kernel device's features (graphics::DeviceFeature3, DEV_FEATURE3_*); 0 (none) for an object that is not a device of this library. */
EXPORT UINT WINAPI TcosD3D12DeviceFeatures3(ID3D12Device *device) {
  const Dev *dev = own_dev(device);
  return dev ? dev->caps.features3 : 0;
}
EXPORT HRESULT WINAPI TcosD3D12ResourceOptions(ID3D12Device *device, UINT flags) {
  Dev *dev = own_dev(device);
  if (!dev) return E_INVALIDARG;
  dev->next_resource_flags = flags;
  return S_OK;
}
/* (src/Windows/tcosdx.h) What the next resource made D3D12_HEAP_FLAG_SHARED carries for the runtime above it (up to 64 bytes: d3d11.dll's description of its
 * texture), and what a resource - made shared, or opened from a shared handle - carries: its size (0: none), up to `capacity` bytes copied. */
EXPORT HRESULT WINAPI TcosD3D12ShareExtra(ID3D12Device *device, const void *data, UINT size) {
  Dev *dev = own_dev(device);
  if (!dev) return E_INVALIDARG;
  if (data && size > sizeof dev->next_share_extra) return E_INVALIDARG;
  dev->next_share_extra_size = data ? size : 0;
  if (dev->next_share_extra_size) memcpy(dev->next_share_extra, data, size);
  return S_OK;
}
EXPORT UINT WINAPI TcosD3D12SharedExtra(ID3D12Resource *resource, void *out, UINT capacity) {
  const Resource *r = own_res(resource);
  if (!r) return 0;
  if (out) memcpy(out, r->share_extra, r->share_extra_size < capacity ? r->share_extra_size : capacity);
  return r->share_extra_size;
}
/* (src/Windows/tcosdx.h) The win32k resource of a resource made shared or opened from a shared handle (0: neither): what D3DKMTShareObjects shares, which d3d11.dll
 * shares itself together with the texture's keyed mutex. */
EXPORT UINT WINAPI TcosD3D12SharedResource(ID3D12Resource *resource) {
  const Resource *r = own_res(resource);
  return r ? r->kresource : 0;
}
EXPORT UINT WINAPI TcosD3D12GlobalShare(ID3D12Resource *resource) {
  const Resource *r = own_res(resource);
  return r ? r->global_share : 0;
}
EXPORT HRESULT WINAPI TcosD3D12OpenShared(ID3D12Device *device, HANDLE handle, int global, REFIID riid, void **out) {
  if (out) *out = NULL;
  Dev *dev = own_dev(device);
  if (!dev || !handle || !riid || !out) return E_INVALIDARG;
  IUnknown *made = NULL;
  const HRESULT hr = open_shared(dev, (UINT64)(UINT_PTR)handle, global ? SHARED_GLOBAL : SHARED_NT, &made);
  if (FAILED(hr)) return hr;
  return query_out(made, riid, out);
}
EXPORT HRESULT WINAPI TcosD3D12BeginQueryKind(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap, UINT index, UINT kind) {
  CmdList *l = own_list(list);
  QHeap *h = own_qheap(heap);
  if (!l || !h) return E_INVALIDARG;
  list_begin_query_kind(l, h, index, kind);
  return S_OK;
}

/* (agility.inc device_make: a new device each time - the adapter's first is its singleton, which a device factory may hand out again) */
EXPORT HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum, REFIID riid, void **out) {
  return device_make(adapter, minimum, sdk_version_of_process(), ~0u, riid, out);
}

EXPORT HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **out) {
  if (out) *out = NULL;
  return DXGI_ERROR_SDK_COMPONENT_MISSING;
}
EXPORT HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT count, const IID *iids, void *configuration, UINT *sizes) { return count == 0 ? S_OK : E_NOINTERFACE; }
/* The Agility SDK's classes (agility.inc): CLSID_D3D12SDKConfiguration, CLSID_D3D12DeviceFactory; the SDK layers' are missing. */
EXPORT HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void **out) { return configuration_interface(clsid, riid, out); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_private_lock);
  }
  return TRUE;
}
