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

/* dxgi.dll for TheoryCraftOS: the DirectX Graphics Infrastructure (IDXGIFactory .. IDXGIFactory6, IDXGIAdapter .. IDXGIAdapter4, IDXGIOutput .. IDXGIOutput6, IDXGISwapChain ..
 * IDXGISwapChain3, CreateDXGIFactory / 1 / 2).
 *
 * Where the work is done. The adapters are the ones of the kernel-mode Direct3D driver (src/Kernel/Dxgkrnl), found with the D3DKMT thunks of gdi32.dll (EnumAdapters2,
 * QueryAdapterInfo: src/Process/Win32k/Dxg/DxgAdapters.cpp): the name, the memory and the capabilities come from there, the displays from the kernel's display mode list. Everything
 * else is in this DLL: the objects and their interfaces, private data, the display mode lists, closest-mode matching, the checks of the swap chain descriptions and the HRESULTs.
 *
 * Swap chains. A Direct3D runtime makes the back buffers: the swap chain asks the device it was created with (a Direct3D 12 command queue, a Direct3D 11 or 10 device) for the
 * ITcosPresenter interface (tcosdx.h), has it create the buffers, and on Present has it hand over the pixels of the buffer, which go to the window with SetDIBitsToDevice.
 * Any other runtime - one that does not answer ITcosPresenter: DXVK, vkd3d-proton, a program's own - gets the same swap chain through its public interfaces (the
 * "fallback" below): the back buffers are textures it makes with CreateTexture2D or CreateCommittedResource, and Present copies the buffer into a staging texture
 * (CopyResource, Map) or, for Direct3D 12, into a readback buffer with a command list on the swap chain's queue and a fence wait, and shows the pixels the same way. A
 * proxy's wrapper (ReShade) that forwards ITcosPresenter is the first kind; one that does not, the second. The buffer index of a swap chain of Direct3D 12 moves on at each
 * Present (FLIP_SEQUENTIAL, FLIP_DISCARD: the only effects Direct3D 12 takes); one of Direct3D 10 or 11 always draws into buffer 0 and shows buffer 0, whatever the effect,
 * as Windows renames its buffers. There is no compositor, no tearing and no exclusive full screen (SetFullscreenState(TRUE) is refused with
 * DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, as on a system whose display cannot be taken over).
 *
 * Not here: output duplication, gamma control (it reports one control point and no ramp), HDR (everything is sRGB, 8 bits), overlays, frame statistics, stereo, IDXGIDevice and the
 * surface interfaces (Direct3D 10/11 use them; no such runtime exists in this system yet), debug interfaces (DXGIGetDebugInterface1 answers DXGI_ERROR_SDK_COMPONENT_MISSING).
 * Built freestanding over kernel32.dll, gdi32.dll and user32.dll with the OS's mingw compiler. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIDL_C_INLINE_WRAPPERS 1      /* (the methods that return a structure: their hidden-pointer ABI) */
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <unknwn.h>
#include <dxgi1_6.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include "../tcosdx.h"

#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");


#define VTBL(type, name) ((type##Vtbl *)&name)
#define OBJ(o) ((DxObj *)(o))

/* ---- the adapters of the machine ------------------------------------------------------------------------------------------------------------------------------------------- */

#define MAX_ADAPTERS 16
typedef struct AdapterInfo {
  LUID luid;
  KmtCaps caps;
  UINT64 segments[3];
  UINT sources;
  int software;
  WCHAR name[128];
  UINT width, height;      /* the desktop of the display, 0 if the adapter has none */
} AdapterInfo;

static void widen(const char *src, WCHAR *dst, UINT cap) {
  UINT i = 0;
  for (; src[i] && i + 1 < cap; ++i) dst[i] = (unsigned char)src[i];
  dst[i] = 0;
}

/* Reads every adapter of the kernel (opens them, asks, closes them). */
static UINT read_adapters(AdapterInfo *out) {
  KmtAdapter list[MAX_ADAPTERS];
  const UINT n = kmt_enum_adapters(list, MAX_ADAPTERS, NULL);
  for (UINT i = 0; i < n; ++i) {
    AdapterInfo *a = &out[i];
    memset(a, 0, sizeof *a);
    a->luid = list[i].luid;
    a->sources = list[i].sources;
    if (!KMT_OK(kmt_adapter_caps(list[i].handle, &a->caps))) memset(&a->caps, 0, sizeof a->caps);
    a->caps.name[63] = 0;
    kmt_segment_sizes(list[i].handle, a->segments);
    a->software = !(a->caps.flags & KMT_CAPS_ACCELERATED);
    widen(a->caps.name[0] ? a->caps.name : "TheoryCraftOS Graphics", a->name, 128);
    if (a->sources) {      /* (the desktop of the display is the size of the screen) */
      a->width = (UINT)GetSystemMetrics(0);
      a->height = (UINT)GetSystemMetrics(1);
    }
    kmt_close_adapter(list[i].handle);
  }
  return n;
}

/* ---- the objects ------------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Factory {
  DxObj o;
  UINT flags;
  UINT count;
  AdapterInfo adapters[MAX_ADAPTERS];
  HWND association;
} Factory;

typedef struct Adapter {
  DxObj o;
  Factory *factory;
  UINT index;
  UINT handle;
  AdapterInfo info;
} Adapter;

typedef struct Output {
  DxObj o;
  Adapter *adapter;
} Output;

#define MAX_BUFFERS 16
/* How a swap chain gets its buffers and their pixels: the runtime's ITcosPresenter, or the runtime's public interfaces (a runtime that is not this system's) */
enum { SC_PRESENTER, SC_D3D11, SC_D3D10, SC_D3D12 };
typedef struct SwapChain {
  DxObj o;
  Factory *factory;
  IUnknown *device;
  int kind;                      /* SC_* */
  BOOL on_queue;                 /* made with a Direct3D 12 command queue: the buffer index moves on at each Present */
  ITcosPresenter *presenter;     /* SC_PRESENTER */
  /* the fallback (fallback_*): a Direct3D 11 device and its immediate context, a Direct3D 10 device, or a Direct3D 12 queue with what copies a buffer to memory */
  ID3D11Device *d11;
  ID3D11DeviceContext *ctx11;
  ID3D11Texture2D *staging11;
  ID3D10Device *d10;
  ID3D10Texture2D *staging10;
  ID3D12CommandQueue *queue12;
  ID3D12Device *dev12;
  ID3D12CommandAllocator *alloc12;
  ID3D12GraphicsCommandList *list12;
  ID3D12Fence *fence12;
  UINT64 fence_value;
  HANDLE fence_event;
  ID3D12Resource *readback12;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint12;
  UINT64 readback_bytes;
  HWND window;
  DXGI_SWAP_CHAIN_DESC1 desc;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen;
  IUnknown *buffers[MAX_BUFFERS];
  UINT current;
  UINT present_count;
  UINT latency;
  BYTE *pixels;
  SIZE_T pixels_size;
} SwapChain;

static const GUID *const g_factory_iids[] = {&IID_IDXGIObject, &IID_IDXGIFactory, &IID_IDXGIFactory1, &IID_IDXGIFactory2, &IID_IDXGIFactory3, &IID_IDXGIFactory4, &IID_IDXGIFactory5,
                                             &IID_IDXGIFactory6, NULL};
static const GUID *const g_adapter_iids[] = {&IID_IDXGIObject, &IID_IDXGIAdapter, &IID_IDXGIAdapter1, &IID_IDXGIAdapter2, &IID_IDXGIAdapter3, &IID_IDXGIAdapter4, NULL};
static const GUID *const g_output_iids[] = {&IID_IDXGIObject, &IID_IDXGIOutput, &IID_IDXGIOutput1, &IID_IDXGIOutput2, &IID_IDXGIOutput3, &IID_IDXGIOutput4, &IID_IDXGIOutput5,
                                            &IID_IDXGIOutput6, NULL};
static const GUID *const g_swapchain_iids[] = {&IID_IDXGIObject, &IID_IDXGIDeviceSubObject, &IID_IDXGISwapChain, &IID_IDXGISwapChain1, &IID_IDXGISwapChain2, &IID_IDXGISwapChain3, NULL};

#define SC_ADAPTER_LAST_INDEX 0

/* ---- helpers ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static int is_swap_format(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R10G10B10A2_UNORM ||
         f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static HRESULT adapter_object(Factory *f, UINT index, REFIID riid, void **out);

/* ---- the factory ------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fac_GetParent(Factory *This, REFIID riid, void **parent) { if (parent) *parent = NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE fac_EnumAdapters(Factory *This, UINT index, IDXGIAdapter **adapter) {
  if (!adapter) return DXGI_ERROR_INVALID_CALL;
  *adapter = NULL;
  if (index >= This->count) return DXGI_ERROR_NOT_FOUND;
  return adapter_object(This, index, &IID_IDXGIAdapter, (void **)adapter);
}
static HRESULT STDMETHODCALLTYPE fac_EnumAdapters1(Factory *This, UINT index, IDXGIAdapter1 **adapter) {
  if (!adapter) return DXGI_ERROR_INVALID_CALL;
  *adapter = NULL;
  if (index >= This->count) return DXGI_ERROR_NOT_FOUND;
  return adapter_object(This, index, &IID_IDXGIAdapter1, (void **)adapter);
}
static HRESULT STDMETHODCALLTYPE fac_MakeWindowAssociation(Factory *This, HWND window, UINT flags) { This->association = window; return S_OK; }
static HRESULT STDMETHODCALLTYPE fac_GetWindowAssociation(Factory *This, HWND *window) {
  if (!window) return DXGI_ERROR_INVALID_CALL;
  *window = This->association;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fac_CreateSoftwareAdapter(Factory *This, HMODULE module, IDXGIAdapter **adapter) {
  if (!module || !adapter) return DXGI_ERROR_INVALID_CALL;
  *adapter = NULL;
  return E_NOTIMPL;
}
static BOOL STDMETHODCALLTYPE fac_IsCurrent(Factory *This) { return TRUE; }
static BOOL STDMETHODCALLTYPE fac_IsWindowedStereoEnabled(Factory *This) { return FALSE; }
static HRESULT STDMETHODCALLTYPE fac_GetSharedResourceAdapterLuid(Factory *This, HANDLE resource, LUID *luid) { return DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE fac_RegisterStereoStatusWindow(Factory *This, HWND window, UINT message, DWORD *cookie) { if (cookie) *cookie = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE fac_RegisterStereoStatusEvent(Factory *This, HANDLE event, DWORD *cookie) { if (cookie) *cookie = 0; return S_OK; }
static void STDMETHODCALLTYPE fac_UnregisterStereoStatus(Factory *This, DWORD cookie) {}
static HRESULT STDMETHODCALLTYPE fac_RegisterOcclusionStatusWindow(Factory *This, HWND window, UINT message, DWORD *cookie) { if (cookie) *cookie = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE fac_RegisterOcclusionStatusEvent(Factory *This, HANDLE event, DWORD *cookie) { if (cookie) *cookie = 0; return S_OK; }
static void STDMETHODCALLTYPE fac_UnregisterOcclusionStatus(Factory *This, DWORD cookie) {}
static UINT STDMETHODCALLTYPE fac_GetCreationFlags(Factory *This) { return This->flags; }
static HRESULT STDMETHODCALLTYPE fac_EnumAdapterByLuid(Factory *This, LUID luid, REFIID iid, void **adapter) {
  if (!adapter) return DXGI_ERROR_INVALID_CALL;
  *adapter = NULL;
  for (UINT i = 0; i < This->count; ++i)
    if (This->adapters[i].luid.LowPart == luid.LowPart && This->adapters[i].luid.HighPart == luid.HighPart) return adapter_object(This, i, iid, adapter);
  return DXGI_ERROR_NOT_FOUND;
}
static HRESULT STDMETHODCALLTYPE fac_EnumWarpAdapter(Factory *This, REFIID iid, void **adapter) {
  if (!adapter) return DXGI_ERROR_INVALID_CALL;
  *adapter = NULL;
  for (UINT i = 0; i < This->count; ++i)
    if (This->adapters[i].software) return adapter_object(This, i, iid, adapter);
  return DXGI_ERROR_NOT_FOUND;
}
static HRESULT STDMETHODCALLTYPE fac_CheckFeatureSupport(Factory *This, DXGI_FEATURE feature, void *data, UINT size) {
  if (feature == DXGI_FEATURE_PRESENT_ALLOW_TEARING) {
    if (!data || size != sizeof(BOOL)) return DXGI_ERROR_INVALID_CALL;
    *(BOOL *)data = FALSE;
    return S_OK;
  }
  return DXGI_ERROR_INVALID_CALL;
}
/* Adapters in the order of the preference: a GPU that draws before the software one (for minimum power: the software one is the only one of its kind here). */
static HRESULT STDMETHODCALLTYPE fac_EnumAdapterByGpuPreference(Factory *This, UINT index, DXGI_GPU_PREFERENCE preference, REFIID iid, void **adapter) {
  UINT order[MAX_ADAPTERS], n = 0;
  if (!adapter) return DXGI_ERROR_INVALID_CALL;
  *adapter = NULL;
  for (int want_software = 0; want_software < 2; ++want_software)
    for (UINT i = 0; i < This->count; ++i)
      if (!!This->adapters[i].software == want_software) order[n++] = i;
  if (preference == DXGI_GPU_PREFERENCE_UNSPECIFIED) {
    n = This->count;
    for (UINT i = 0; i < n; ++i) order[i] = i;
  } else if (preference == DXGI_GPU_PREFERENCE_MINIMUM_POWER) {
    n = 0;
    for (int want_software = 1; want_software >= 0; --want_software)
      for (UINT i = 0; i < This->count; ++i)
        if (!!This->adapters[i].software == want_software) order[n++] = i;
  }
  if (index >= n) return DXGI_ERROR_NOT_FOUND;
  return adapter_object(This, order[index], iid, adapter);
}

static HRESULT swap_chain_create(Factory *f, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,
                                 IDXGISwapChain1 **out);

static HRESULT STDMETHODCALLTYPE fac_CreateSwapChain(Factory *This, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain) {
  DXGI_SWAP_CHAIN_DESC1 d;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs;
  IDXGISwapChain1 *c = NULL;
  if (!device || !desc || !chain) return DXGI_ERROR_INVALID_CALL;
  *chain = NULL;
  /* (a swap chain of this call needs its window; a format no swap chain has is E_INVALIDARG, as Windows answers) */
  if (!desc->OutputWindow) return DXGI_ERROR_INVALID_CALL;
  if (!is_swap_format(desc->BufferDesc.Format)) return E_INVALIDARG;
  memset(&d, 0, sizeof d);
  memset(&fs, 0, sizeof fs);
  d.Width = desc->BufferDesc.Width;
  d.Height = desc->BufferDesc.Height;
  d.Format = desc->BufferDesc.Format;
  d.SampleDesc = desc->SampleDesc;
  d.BufferUsage = desc->BufferUsage;
  d.BufferCount = desc->BufferCount;
  d.Scaling = desc->BufferDesc.Scaling == DXGI_MODE_SCALING_UNSPECIFIED ? DXGI_SCALING_STRETCH : DXGI_SCALING_STRETCH;
  d.SwapEffect = desc->SwapEffect;
  d.Flags = desc->Flags;
  d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  fs.RefreshRate = desc->BufferDesc.RefreshRate;
  fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
  fs.Scaling = desc->BufferDesc.Scaling;
  fs.Windowed = desc->Windowed;
  *chain = NULL;
  const HRESULT hr = swap_chain_create(This, device, desc->OutputWindow, &d, &fs, &c);
  if (FAILED(hr)) return hr;
  *chain = (IDXGISwapChain *)c;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fac_CreateSwapChainForHwnd(Factory *This, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGIOutput *restrict_to, IDXGISwapChain1 **chain) {
  if (!device || !window || !desc || !chain) return DXGI_ERROR_INVALID_CALL;
  *chain = NULL;
  return swap_chain_create(This, device, window, desc, fullscreen, chain);
}
static HRESULT STDMETHODCALLTYPE fac_CreateSwapChainForCoreWindow(Factory *This, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to,
                                                                  IDXGISwapChain1 **chain) {
  if (chain) *chain = NULL;
  return E_NOTIMPL;      /* (no core windows: those are the UWP ones) */
}
static HRESULT STDMETHODCALLTYPE fac_CreateSwapChainForComposition(Factory *This, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to,
                                                                   IDXGISwapChain1 **chain) {
  if (!device || !desc || !chain) return DXGI_ERROR_INVALID_CALL;
  *chain = NULL;
  return swap_chain_create(This, device, NULL, desc, NULL, chain);
}

static void factory_destroy(DxObj *o) { (void)o; }

/* ---- the adapter ------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void adapter_destroy(DxObj *o) {
  Adapter *a = (Adapter *)o;
  if (a->handle) kmt_close_adapter(a->handle);
  if (a->factory) dx_Release(OBJ(a->factory));
}

static HRESULT adapter_object(Factory *f, UINT index, REFIID riid, void **out) {
  extern const IDXGIAdapter4Vtbl adp_vtbl;
  Adapter *a = mem_alloc(sizeof *a);
  if (!a) return E_OUTOFMEMORY;
  dx_init(OBJ(a), &adp_vtbl, g_adapter_iids, adapter_destroy);
  a->factory = f;
  dx_AddRef(OBJ(f));
  a->index = index;
  a->info = f->adapters[index];
  kmt_open_adapter(a->info.luid, &a->handle);
  const HRESULT hr = dx_QueryInterface(OBJ(a), riid, out);
  dx_Release(OBJ(a));
  return hr;
}

static HRESULT STDMETHODCALLTYPE adp_GetParent(Adapter *This, REFIID riid, void **parent) { return dx_QueryInterface(OBJ(This->factory), riid, parent); }
static HRESULT STDMETHODCALLTYPE adp_EnumOutputs(Adapter *This, UINT index, IDXGIOutput **output) {
  extern const IDXGIOutput6Vtbl out_vtbl;
  if (!output) return DXGI_ERROR_INVALID_CALL;
  *output = NULL;
  if (index != 0 || !This->info.sources) return DXGI_ERROR_NOT_FOUND;
  Output *o = mem_alloc(sizeof *o);
  if (!o) return E_OUTOFMEMORY;
  dx_init(OBJ(o), &out_vtbl, g_output_iids, NULL);
  o->adapter = This;
  dx_AddRef(OBJ(This));
  *output = (IDXGIOutput *)o;
  return S_OK;
}
static void fill_desc(Adapter *a, WCHAR *name, UINT *vendor, UINT *device, UINT *subsys, UINT *revision, SIZE_T *dedicated_video, SIZE_T *dedicated_system, SIZE_T *shared, LUID *luid) {
  memcpy(name, a->info.name, sizeof a->info.name);
  /* The CPU renderer is Microsoft's Basic Render Driver (what WARP shows); a GPU shows the PCI numbers the kernel has for it (programs choose their paths and
   * workarounds by them: Chrome takes 0x1414:0x8C for software and composites on the CPU). The kernel's own numbers past 0xFFFF are not PCI vendors: 0. */
  const UINT kernel_vendor = a->info.caps.vendor_id <= 0xFFFF ? a->info.caps.vendor_id : 0;
  *vendor = a->info.software ? 0x1414 : kernel_vendor;
  *device = a->info.software ? 0x8C : (kernel_vendor ? a->info.caps.device_id : 0);
  *subsys = 0;
  *revision = 0;
  *dedicated_video = (SIZE_T)a->info.segments[0];
  *dedicated_system = (SIZE_T)a->info.segments[1];
  *shared = (SIZE_T)a->info.segments[2];
  *luid = a->info.luid;
}
static HRESULT STDMETHODCALLTYPE adp_GetDesc(Adapter *This, DXGI_ADAPTER_DESC *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  memset(d, 0, sizeof *d);
  fill_desc(This, d->Description, &d->VendorId, &d->DeviceId, &d->SubSysId, &d->Revision, &d->DedicatedVideoMemory, &d->DedicatedSystemMemory, &d->SharedSystemMemory, &d->AdapterLuid);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE adp_GetDesc1(Adapter *This, DXGI_ADAPTER_DESC1 *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  memset(d, 0, sizeof *d);
  fill_desc(This, d->Description, &d->VendorId, &d->DeviceId, &d->SubSysId, &d->Revision, &d->DedicatedVideoMemory, &d->DedicatedSystemMemory, &d->SharedSystemMemory, &d->AdapterLuid);
  d->Flags = This->info.software ? DXGI_ADAPTER_FLAG_SOFTWARE : DXGI_ADAPTER_FLAG_NONE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE adp_GetDesc2(Adapter *This, DXGI_ADAPTER_DESC2 *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  memset(d, 0, sizeof *d);
  fill_desc(This, d->Description, &d->VendorId, &d->DeviceId, &d->SubSysId, &d->Revision, &d->DedicatedVideoMemory, &d->DedicatedSystemMemory, &d->SharedSystemMemory, &d->AdapterLuid);
  d->Flags = This->info.software ? DXGI_ADAPTER_FLAG_SOFTWARE : DXGI_ADAPTER_FLAG_NONE;
  d->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
  d->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE adp_GetDesc3(Adapter *This, DXGI_ADAPTER_DESC3 *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  memset(d, 0, sizeof *d);
  fill_desc(This, d->Description, &d->VendorId, &d->DeviceId, &d->SubSysId, &d->Revision, &d->DedicatedVideoMemory, &d->DedicatedSystemMemory, &d->SharedSystemMemory, &d->AdapterLuid);
  d->Flags = This->info.software ? DXGI_ADAPTER_FLAG3_SOFTWARE : DXGI_ADAPTER_FLAG3_NONE;
  d->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
  d->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
  return S_OK;
}
/* The user-mode driver the adapter has: d3d12.dll for the Direct3D 12 and 11on12 interfaces (the version is that of this system's driver model, WDDM 1.3 = 1300 in 16.16). */
static const GUID kIID_ID3D10Device = {0x9b7e4c0f, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};      /* (d3d10.h's IID_ID3D10Device) */
static HRESULT STDMETHODCALLTYPE adp_CheckInterfaceSupport(Adapter *This, REFGUID guid, LARGE_INTEGER *umd) {
  if (!guid || !umd) return DXGI_ERROR_INVALID_CALL;
  /* (the version of the user-mode driver, which programs read as the driver's: Chrome and ANGLE ask with IDXGIDevice. These libraries are the driver; 31 is a
   * WDDM 3.x driver's first number, as on Windows 11) */
  if (guid_eq(guid, &IID_IDXGIDevice) || guid_eq(guid, &IID_IDXGIDevice1) || guid_eq(guid, &kIID_ID3D10Device)) {
    umd->QuadPart = (LONGLONG)(((ULONGLONG)31 << 48) | ((ULONGLONG)0 << 32) | ((ULONGLONG)101 << 16) | 1);      /* 31.0.101.1 */
    return S_OK;
  }
  return DXGI_ERROR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE adp_QueryVideoMemoryInfo(Adapter *This, UINT node, DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO *info) {
  if (!info || node != 0 || (group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL && group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)) return E_INVALIDARG;
  memset(info, 0, sizeof *info);
  const UINT64 total = group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? This->info.segments[0] : This->info.segments[2];
  info->Budget = total;
  info->AvailableForReservation = total / 2;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE adp_SetVideoMemoryReservation(Adapter *This, UINT node, DXGI_MEMORY_SEGMENT_GROUP group, UINT64 reservation) {
  if (node != 0 || (group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL && group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)) return E_INVALIDARG;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE adp_RegisterVideoMemoryBudgetChangeNotificationEvent(Adapter *This, HANDLE event, DWORD *cookie) { if (cookie) *cookie = 1; return S_OK; }
static void STDMETHODCALLTYPE adp_UnregisterVideoMemoryBudgetChangeNotification(Adapter *This, DWORD cookie) {}
static HRESULT STDMETHODCALLTYPE adp_RegisterHardwareContentProtectionTeardownStatusEvent(Adapter *This, HANDLE event, DWORD *cookie) { return DXGI_ERROR_UNSUPPORTED; }
static void STDMETHODCALLTYPE adp_UnregisterHardwareContentProtectionTeardownStatus(Adapter *This, DWORD cookie) {}

/* ---- the output -------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void output_destroy(DxObj *o) { dx_Release(OBJ(((Output *)o)->adapter)); }

static HRESULT STDMETHODCALLTYPE out_GetParent(Output *This, REFIID riid, void **parent) { return dx_QueryInterface(OBJ(This->adapter), riid, parent); }
static HRESULT STDMETHODCALLTYPE out_GetDesc(Output *This, DXGI_OUTPUT_DESC *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  memset(d, 0, sizeof *d);
  const WCHAR name[] = {'\\', '\\', '.', '\\', 'D', 'I', 'S', 'P', 'L', 'A', 'Y', '1', 0};
  memcpy(d->DeviceName, name, sizeof name);
  d->DesktopCoordinates.right = (LONG)This->adapter->info.width;
  d->DesktopCoordinates.bottom = (LONG)This->adapter->info.height;
  d->AttachedToDesktop = TRUE;
  d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
  d->Monitor = (HMONITOR)(UINT_PTR)1;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE out_GetDesc1(Output *This, DXGI_OUTPUT_DESC1 *d) {
  DXGI_OUTPUT_DESC base;
  if (!d) return DXGI_ERROR_INVALID_CALL;
  out_GetDesc(This, &base);
  memset(d, 0, sizeof *d);
  memcpy(d->DeviceName, base.DeviceName, sizeof d->DeviceName);
  d->DesktopCoordinates = base.DesktopCoordinates;
  d->AttachedToDesktop = TRUE;
  d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
  d->Monitor = base.Monitor;
  d->BitsPerColor = 8;
  d->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  d->RedPrimary[0] = 0.64f; d->RedPrimary[1] = 0.33f;
  d->GreenPrimary[0] = 0.30f; d->GreenPrimary[1] = 0.60f;
  d->BluePrimary[0] = 0.15f; d->BluePrimary[1] = 0.06f;
  d->WhitePoint[0] = 0.3127f; d->WhitePoint[1] = 0.3290f;
  d->MinLuminance = 0.0f; d->MaxLuminance = 80.0f; d->MaxFullFrameLuminance = 80.0f;
  return S_OK;
}

/* The modes the display offers: the kernel's list (sizes) at 60 Hz, 32-bit colour. */
static UINT mode_list(Output *o, DXGI_MODE_DESC1 *out, UINT capacity) {
  BYTE block[24] __attribute__((aligned(8))) = {0};
  BYTE modes[32 * 64] __attribute__((aligned(8)));
  UINT n = 0;
  KPUT32(block, 0, o->adapter->handle);
  KPUT64(block, 8, modes);
  KPUT32(block, 16, 64);
  if (KMT_OK(D3DKMTGetDisplayModeList(block))) n = KGET32(block, 16);
  if (n > 64) n = 64;
  if (n == 0 && o->adapter->info.width) {
    KPUT32(modes, 0, o->adapter->info.width);
    KPUT32(modes, 4, o->adapter->info.height);
    n = 1;
  }
  for (UINT i = 0; i < n && i < capacity; ++i) {
    memset(&out[i], 0, sizeof out[i]);
    out[i].Width = KGET32(modes + i * 32, 0);
    out[i].Height = KGET32(modes + i * 32, 4);
    out[i].RefreshRate.Numerator = 60;
    out[i].RefreshRate.Denominator = 1;
    out[i].ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    out[i].Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  }
  return n;
}
static HRESULT list_modes(Output *This, DXGI_FORMAT format, UINT *count, DXGI_MODE_DESC1 *out1, DXGI_MODE_DESC *out) {
  DXGI_MODE_DESC1 modes[64];
  if (!count) return DXGI_ERROR_INVALID_CALL;
  if (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
    *count = 0;
    return S_OK;
  }
  const UINT n = mode_list(This, modes, 64);
  if (!out1 && !out) { *count = n; return S_OK; }
  const UINT room = *count;
  for (UINT i = 0; i < n && i < room; ++i) {
    modes[i].Format = format;
    if (out1) out1[i] = modes[i];
    else {
      out[i].Width = modes[i].Width; out[i].Height = modes[i].Height; out[i].RefreshRate = modes[i].RefreshRate; out[i].Format = format;
      out[i].ScanlineOrdering = modes[i].ScanlineOrdering; out[i].Scaling = modes[i].Scaling;
    }
  }
  *count = n < room ? n : room;
  return n > room ? DXGI_ERROR_MORE_DATA : S_OK;
}
static HRESULT STDMETHODCALLTYPE out_GetDisplayModeList(Output *This, DXGI_FORMAT format, UINT flags, UINT *count, DXGI_MODE_DESC *desc) { return list_modes(This, format, count, NULL, desc); }
static HRESULT STDMETHODCALLTYPE out_GetDisplayModeList1(Output *This, DXGI_FORMAT format, UINT flags, UINT *count, DXGI_MODE_DESC1 *desc) { return list_modes(This, format, count, desc, NULL); }
static HRESULT closest(Output *This, const DXGI_MODE_DESC1 *want, DXGI_MODE_DESC1 *got) {
  DXGI_MODE_DESC1 modes[64];
  if (!want || !got) return DXGI_ERROR_INVALID_CALL;
  const UINT n = mode_list(This, modes, 64);
  if (n == 0) return DXGI_ERROR_NOT_FOUND;
  UINT best = 0;
  UINT64 best_cost = ~(UINT64)0;
  for (UINT i = 0; i < n; ++i) {
    const UINT64 dw = want->Width ? (modes[i].Width > want->Width ? modes[i].Width - want->Width : want->Width - modes[i].Width) : 0;
    const UINT64 dh = want->Height ? (modes[i].Height > want->Height ? modes[i].Height - want->Height : want->Height - modes[i].Height) : 0;
    const UINT64 cost = dw * dw + dh * dh;
    if (cost < best_cost) { best_cost = cost; best = i; }
  }
  *got = modes[best];
  got->Format = want->Format ? want->Format : DXGI_FORMAT_B8G8R8A8_UNORM;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE out_FindClosestMatchingMode(Output *This, const DXGI_MODE_DESC *mode, DXGI_MODE_DESC *got, IUnknown *device) {
  DXGI_MODE_DESC1 want, found;
  if (!mode || !got) return DXGI_ERROR_INVALID_CALL;
  memset(&want, 0, sizeof want);
  want.Width = mode->Width; want.Height = mode->Height; want.Format = mode->Format;
  const HRESULT hr = closest(This, &want, &found);
  if (FAILED(hr)) return hr;
  got->Width = found.Width; got->Height = found.Height; got->RefreshRate = found.RefreshRate; got->Format = found.Format;
  got->ScanlineOrdering = found.ScanlineOrdering; got->Scaling = found.Scaling;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE out_FindClosestMatchingMode1(Output *This, const DXGI_MODE_DESC1 *mode, DXGI_MODE_DESC1 *got, IUnknown *device) { return closest(This, mode, got); }
static HRESULT STDMETHODCALLTYPE out_WaitForVBlank(Output *This) { Sleep(16); return S_OK; }
static HRESULT STDMETHODCALLTYPE out_TakeOwnership(Output *This, IUnknown *device, BOOL exclusive) { return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; }
static void STDMETHODCALLTYPE out_ReleaseOwnership(Output *This) {}
static HRESULT STDMETHODCALLTYPE out_GetGammaControlCapabilities(Output *This, DXGI_GAMMA_CONTROL_CAPABILITIES *caps) {
  if (!caps) return DXGI_ERROR_INVALID_CALL;
  memset(caps, 0, sizeof *caps);
  caps->MaxConvertedValue = 1.0f;
  caps->NumGammaControlPoints = 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE out_SetGammaControl(Output *This, const DXGI_GAMMA_CONTROL *gamma) { return DXGI_ERROR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE out_GetGammaControl(Output *This, DXGI_GAMMA_CONTROL *gamma) {
  if (!gamma) return DXGI_ERROR_INVALID_CALL;
  memset(gamma, 0, sizeof *gamma);
  gamma->Scale.Red = gamma->Scale.Green = gamma->Scale.Blue = 1.0f;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE out_GetFrameStatistics(Output *This, DXGI_FRAME_STATISTICS *stats) { return DXGI_ERROR_UNSUPPORTED; }
static BOOL STDMETHODCALLTYPE out_SupportsOverlays(Output *This) { return FALSE; }
static HRESULT STDMETHODCALLTYPE out_CheckOverlaySupport(Output *This, DXGI_FORMAT format, IUnknown *device, UINT *flags) { if (flags) *flags = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE out_CheckOverlayColorSpaceSupport(Output *This, DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE space, IUnknown *device, UINT *flags) { if (flags) *flags = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE out_CheckHardwareCompositionSupport(Output *This, UINT *flags) { if (flags) *flags = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE out_DuplicateOutput(Output *This, IUnknown *device, IDXGIOutputDuplication **dup) { if (dup) *dup = NULL; return DXGI_ERROR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE out_DuplicateOutput1(Output *This, IUnknown *device, UINT flags, UINT count, const DXGI_FORMAT *formats, IDXGIOutputDuplication **dup) {
  if (dup) *dup = NULL;
  return DXGI_ERROR_UNSUPPORTED;
}

/* ---- the swap chain ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void release_buffers(SwapChain *s) {
  for (UINT i = 0; i < MAX_BUFFERS; ++i)
    if (s->buffers[i]) { s->buffers[i]->lpVtbl->Release(s->buffers[i]); s->buffers[i] = NULL; }
  /* (what the fallback copies through has the buffers' size and format: made again with them) */
  if (s->staging11) { ID3D11Texture2D_Release(s->staging11); s->staging11 = NULL; }
  if (s->staging10) { ID3D10Texture2D_Release(s->staging10); s->staging10 = NULL; }
  if (s->readback12) { ID3D12Resource_Release(s->readback12); s->readback12 = NULL; }
}
/* (waits until the swap chain's copies on its Direct3D 12 queue are done) */
static void fallback12_wait(SwapChain *s) {
  if (!s->fence12 || ID3D12Fence_GetCompletedValue(s->fence12) >= s->fence_value) return;
  if (s->fence_event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(s->fence12, s->fence_value, s->fence_event))) WaitForSingleObject(s->fence_event, INFINITE);
}
static void swap_chain_destroy(DxObj *o) {
  SwapChain *s = (SwapChain *)o;
  fallback12_wait(s);
  release_buffers(s);
  mem_free(s->pixels);
  if (s->presenter) s->presenter->lpVtbl->Release(s->presenter);
  if (s->ctx11) ID3D11DeviceContext_Release(s->ctx11);
  if (s->d11) ID3D11Device_Release(s->d11);
  if (s->d10) ID3D10Device_Release(s->d10);
  if (s->list12) ID3D12GraphicsCommandList_Release(s->list12);
  if (s->alloc12) ID3D12CommandAllocator_Release(s->alloc12);
  if (s->fence12) ID3D12Fence_Release(s->fence12);
  if (s->fence_event) CloseHandle(s->fence_event);
  if (s->dev12) ID3D12Device_Release(s->dev12);
  if (s->queue12) ID3D12CommandQueue_Release(s->queue12);
  if (s->device) s->device->lpVtbl->Release(s->device);
  if (s->factory) dx_Release(OBJ(s->factory));
}

/* ---- the fallback: a runtime that is not this system's, through its public interfaces ---- */

/* The bind flags of a back buffer: what its DXGI usage says (Direct3D 11 and 10 have the same values). */
static UINT back_buffer_binds(UINT usage) {
  UINT bind = 0;
  if (usage & DXGI_USAGE_RENDER_TARGET_OUTPUT) bind |= D3D11_BIND_RENDER_TARGET;
  if (usage & DXGI_USAGE_SHADER_INPUT) bind |= D3D11_BIND_SHADER_RESOURCE;
  if (usage & DXGI_USAGE_UNORDERED_ACCESS) bind |= D3D11_BIND_UNORDERED_ACCESS;
  return bind;
}
static HRESULT fallback_buffers(SwapChain *s) {
  const DXGI_SWAP_CHAIN_DESC1 *d = &s->desc;
  HRESULT hr = S_OK;
  if (s->kind == SC_D3D11) {
    const D3D11_TEXTURE2D_DESC td = {d->Width, d->Height, 1, 1, d->Format, {1, 0}, D3D11_USAGE_DEFAULT, back_buffer_binds(d->BufferUsage), 0,
                                     (d->Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) ? (UINT)D3D11_RESOURCE_MISC_GDI_COMPATIBLE : 0u};
    for (UINT i = 0; i < d->BufferCount && SUCCEEDED(hr); ++i) hr = ID3D11Device_CreateTexture2D(s->d11, &td, NULL, (ID3D11Texture2D **)&s->buffers[i]);
    const D3D11_TEXTURE2D_DESC sd = {d->Width, d->Height, 1, 1, d->Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(s->d11, &sd, NULL, &s->staging11);
  } else if (s->kind == SC_D3D10) {
    const D3D10_TEXTURE2D_DESC td = {d->Width, d->Height, 1, 1, d->Format, {1, 0}, D3D10_USAGE_DEFAULT, back_buffer_binds(d->BufferUsage), 0,
                                     (d->Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) ? (UINT)D3D10_RESOURCE_MISC_GDI_COMPATIBLE : 0u};
    for (UINT i = 0; i < d->BufferCount && SUCCEEDED(hr); ++i) hr = ID3D10Device_CreateTexture2D(s->d10, &td, NULL, (ID3D10Texture2D **)&s->buffers[i]);
    const D3D10_TEXTURE2D_DESC sd = {d->Width, d->Height, 1, 1, d->Format, {1, 0}, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ, 0};
    if (SUCCEEDED(hr)) hr = ID3D10Device_CreateTexture2D(s->d10, &sd, NULL, &s->staging10);
  } else {
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = d->Width;
    rd.Height = d->Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = d->Format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | ((d->BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    /* (a back buffer begins in the PRESENT state - COMMON -, as Windows hands it out) */
    for (UINT i = 0; i < d->BufferCount && SUCCEEDED(hr); ++i)
      hr = ID3D12Device_CreateCommittedResource(s->dev12, &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PRESENT, NULL, &IID_ID3D12Resource, (void **)&s->buffers[i]);
    if (SUCCEEDED(hr)) {
      UINT rows = 0;
      UINT64 row_bytes = 0;
      ID3D12Device_GetCopyableFootprints(s->dev12, &rd, 0, 1, 0, &s->footprint12, &rows, &row_bytes, &s->readback_bytes);
      const D3D12_HEAP_PROPERTIES rb = {D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
      D3D12_RESOURCE_DESC bd;
      memset(&bd, 0, sizeof bd);
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = s->readback_bytes;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.Format = DXGI_FORMAT_UNKNOWN;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      hr = s->readback_bytes ? ID3D12Device_CreateCommittedResource(s->dev12, &rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                                    (void **)&s->readback12)
                             : E_INVALIDARG;
    }
  }
  return hr;
}

/* One row of a back buffer as B8G8R8A8 (what goes to the window; sRGB values are shown as they are). */
static UINT half_to_byte(UINT h) {
  const UINT e = (h >> 10) & 31, m = h & 1023;
  if (h & 0x8000) return 0;
  if (e == 0) return 0;
  if (e >= 15) return 255;
  const UINT v = ((1024 + m) * 255) >> (10 + 15 - e);      /* value = (1 + m / 1024) * 2^(e - 15) */
  return v > 255 ? 255 : v;
}
static void row_to_bgra(DXGI_FORMAT format, const BYTE *src, BYTE *dst, UINT width) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: memcpy(dst, src, (SIZE_T)width * 4); break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      for (UINT x = 0; x < width; ++x) { dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = src[x * 4 + 3]; }
      break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      for (UINT x = 0; x < width; ++x) {
        UINT v;
        memcpy(&v, src + x * 4, 4);
        dst[x * 4] = (BYTE)(((v >> 20) & 1023) >> 2); dst[x * 4 + 1] = (BYTE)(((v >> 10) & 1023) >> 2); dst[x * 4 + 2] = (BYTE)((v & 1023) >> 2);
        dst[x * 4 + 3] = (BYTE)((v >> 30) * 85);
      }
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      for (UINT x = 0; x < width; ++x) {
        UINT16 c[4];
        memcpy(c, src + (SIZE_T)x * 8, 8);
        dst[x * 4] = (BYTE)half_to_byte(c[2]); dst[x * 4 + 1] = (BYTE)half_to_byte(c[1]); dst[x * 4 + 2] = (BYTE)half_to_byte(c[0]); dst[x * 4 + 3] = (BYTE)half_to_byte(c[3]);
      }
      break;
    default: memset(dst, 0, (SIZE_T)width * 4); break;
  }
}
static UINT format_bytes(DXGI_FORMAT format) { return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }

/* The pixels of a back buffer, as the presenter's ReadBackBuffer gives them: B8G8R8A8 rows `pitch` apart, after the work queued before. */
static HRESULT fallback_read(SwapChain *s, IUnknown *buffer, BYTE *dst, UINT pitch) {
  const UINT w = s->desc.Width, h = s->desc.Height;
  if (s->kind == SC_D3D11) {
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_CopyResource(s->ctx11, (ID3D11Resource *)s->staging11, (ID3D11Resource *)buffer);
    HRESULT hr = ID3D11DeviceContext_Map(s->ctx11, (ID3D11Resource *)s->staging11, 0, D3D11_MAP_READ, 0, &m);      /* (waits for the copy) */
    if (FAILED(hr)) return hr;
    if (m.RowPitch < w * format_bytes(s->desc.Format)) { ID3D11DeviceContext_Unmap(s->ctx11, (ID3D11Resource *)s->staging11, 0); return E_FAIL; }
    for (UINT y = 0; y < h; ++y) row_to_bgra(s->desc.Format, (const BYTE *)m.pData + (SIZE_T)y * m.RowPitch, dst + (SIZE_T)y * pitch, w);
    ID3D11DeviceContext_Unmap(s->ctx11, (ID3D11Resource *)s->staging11, 0);
    return S_OK;
  }
  if (s->kind == SC_D3D10) {
    D3D10_MAPPED_TEXTURE2D m;
    ID3D10Device_CopyResource(s->d10, (ID3D10Resource *)s->staging10, (ID3D10Resource *)buffer);
    HRESULT hr = ID3D10Texture2D_Map(s->staging10, 0, D3D10_MAP_READ, 0, &m);
    if (FAILED(hr)) return hr;
    if (m.RowPitch < w * format_bytes(s->desc.Format)) { ID3D10Texture2D_Unmap(s->staging10, 0); return E_FAIL; }
    for (UINT y = 0; y < h; ++y) row_to_bgra(s->desc.Format, (const BYTE *)m.pData + (SIZE_T)y * m.RowPitch, dst + (SIZE_T)y * pitch, w);
    ID3D10Texture2D_Unmap(s->staging10, 0);
    return S_OK;
  }
  /* (Direct3D 12: the buffer - in the PRESENT state the program left it in - copied into the readback buffer by a list of the queue's own type, the fence waited for) */
  HRESULT hr = ID3D12CommandAllocator_Reset(s->alloc12);
  if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Reset(s->list12, s->alloc12, NULL);
  if (FAILED(hr)) return hr;
  D3D12_RESOURCE_BARRIER b;
  memset(&b, 0, sizeof b);
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = (ID3D12Resource *)buffer;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  ID3D12GraphicsCommandList_ResourceBarrier(s->list12, 1, &b);
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = s->readback12;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = s->footprint12;
  from.pResource = (ID3D12Resource *)buffer;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = 0;
  ID3D12GraphicsCommandList_CopyTextureRegion(s->list12, &to, 0, 0, 0, &from, NULL);
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  ID3D12GraphicsCommandList_ResourceBarrier(s->list12, 1, &b);
  hr = ID3D12GraphicsCommandList_Close(s->list12);
  if (FAILED(hr)) return hr;
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)s->list12};
  ID3D12CommandQueue_ExecuteCommandLists(s->queue12, 1, lists);
  hr = ID3D12CommandQueue_Signal(s->queue12, s->fence12, ++s->fence_value);
  if (FAILED(hr)) return hr;
  fallback12_wait(s);
  void *data = NULL;
  const D3D12_RANGE read = {0, (SIZE_T)s->readback_bytes};
  hr = ID3D12Resource_Map(s->readback12, 0, &read, &data);
  if (FAILED(hr) || !data) return FAILED(hr) ? hr : E_FAIL;
  const UINT row_pitch = s->footprint12.Footprint.RowPitch;
  if (row_pitch < w * format_bytes(s->desc.Format) || (UINT64)row_pitch * h > s->readback_bytes - s->footprint12.Offset + row_pitch) hr = E_FAIL;
  for (UINT y = 0; SUCCEEDED(hr) && y < h; ++y)
    row_to_bgra(s->desc.Format, (const BYTE *)data + s->footprint12.Offset + (SIZE_T)y * row_pitch, dst + (SIZE_T)y * pitch, w);
  const D3D12_RANGE none = {0, 0};
  ID3D12Resource_Unmap(s->readback12, 0, &none);
  return hr;
}

/* What the fallback keeps of the runtime: its device (and immediate context), or the queue, its device, a list of the queue's type, a fence. */
static HRESULT fallback_start(SwapChain *s, IUnknown *device) {
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&s->queue12)) && s->queue12) {
    s->kind = SC_D3D12;
    D3D12_COMMAND_QUEUE_DESC qd = ID3D12CommandQueue_GetDesc(s->queue12);
    HRESULT hr = ID3D12CommandQueue_GetDevice(s->queue12, &IID_ID3D12Device, (void **)&s->dev12);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(s->dev12, qd.Type, &IID_ID3D12CommandAllocator, (void **)&s->alloc12);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(s->dev12, 0, qd.Type, s->alloc12, NULL, &IID_ID3D12GraphicsCommandList, (void **)&s->list12);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Close(s->list12);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateFence(s->dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&s->fence12);
    if (SUCCEEDED(hr)) {
      s->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
      if (!s->fence_event) hr = E_OUTOFMEMORY;
    }
    return hr;
  }
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, &IID_ID3D11Device, (void **)&s->d11)) && s->d11) {
    s->kind = SC_D3D11;
    ID3D11Device_GetImmediateContext(s->d11, &s->ctx11);
    return s->ctx11 ? S_OK : E_FAIL;
  }
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, &IID_ID3D10Device, (void **)&s->d10)) && s->d10) {
    s->kind = SC_D3D10;
    return S_OK;
  }
  return DXGI_ERROR_INVALID_CALL;      /* (no device a swap chain can be made with) */
}

static HRESULT make_buffers(SwapChain *s) {
  if (s->kind == SC_D3D12) fallback12_wait(s);      /* (a copy of a buffer that goes may still run) */
  release_buffers(s);
  const HRESULT hr = s->kind == SC_PRESENTER ? s->presenter->lpVtbl->CreateBackBuffers(s->presenter, s->desc.BufferCount, s->desc.Width, s->desc.Height, s->desc.Format,
                                                                                       s->desc.BufferUsage, s->buffers)
                                             : fallback_buffers(s);
  if (FAILED(hr)) release_buffers(s);
  s->current = 0;
  return hr;
}

static HRESULT swap_chain_create(Factory *f, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGISwapChain1 **out) {
  extern const IDXGISwapChain3Vtbl sc_vtbl;
  DXGI_SWAP_CHAIN_DESC1 d = *desc;
  if (d.Width == 0 || d.Height == 0) {
    RECT r = {0, 0, 0, 0};
    if (window && GetClientRect(window, &r)) { d.Width = (UINT)(r.right - r.left); d.Height = (UINT)(r.bottom - r.top); }
    if (d.Width == 0) d.Width = 8;
    if (d.Height == 0) d.Height = 8;
  }
  if (d.BufferCount == 0) d.BufferCount = 1;
  if (d.BufferCount > MAX_BUFFERS || d.Width > 16384 || d.Height > 16384 || !is_swap_format(d.Format) || d.SampleDesc.Count > 1 || d.Stereo) return DXGI_ERROR_INVALID_CALL;
  ID3D12CommandQueue *q = NULL;      /* (a Direct3D 12 swap chain: the index of its buffer moves on) */
  const BOOL on_queue = SUCCEEDED(device->lpVtbl->QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&q)) && q;
  if (q) ID3D12CommandQueue_Release(q);
  /* (Direct3D 12 presents with the flip effects only; a flip effect needs two buffers at least - Windows' answers, Wine's test_swapchain_backbuffer_index) */
  const BOOL flip = d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if ((on_queue && !flip) || (flip && d.BufferCount < 2)) return DXGI_ERROR_INVALID_CALL;
  SwapChain *s = mem_alloc(sizeof *s);
  if (!s) return E_OUTOFMEMORY;
  dx_init(OBJ(s), &sc_vtbl, g_swapchain_iids, swap_chain_destroy);
  s->factory = f;
  dx_AddRef(OBJ(f));
  s->device = device;
  device->lpVtbl->AddRef(device);
  s->on_queue = on_queue;
  /* (this system's runtime - or a proxy that hands its ITcosPresenter on - makes the buffers itself; any other through its public interfaces) */
  HRESULT hr = S_OK;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, &IID_ITcosPresenter, (void **)&s->presenter)) && s->presenter) s->kind = SC_PRESENTER;
  else {
    s->presenter = NULL;
    hr = fallback_start(s, device);
  }
  s->window = window;
  s->desc = d;
  if (fullscreen) s->fullscreen = *fullscreen;
  else s->fullscreen.Windowed = TRUE;
  s->latency = 3;
  if (SUCCEEDED(hr)) hr = make_buffers(s);
  if (FAILED(hr)) { dx_Release(OBJ(s)); return hr; }
  *out = (IDXGISwapChain1 *)s;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE sc_GetParent(SwapChain *This, REFIID riid, void **parent) { return dx_QueryInterface(OBJ(This->factory), riid, parent); }
static HRESULT STDMETHODCALLTYPE sc_GetDevice(SwapChain *This, REFIID riid, void **device) { return This->device->lpVtbl->QueryInterface(This->device, riid, device); }
static HRESULT STDMETHODCALLTYPE sc_GetBuffer(SwapChain *This, UINT index, REFIID riid, void **surface) {
  if (!surface) return DXGI_ERROR_INVALID_CALL;
  *surface = NULL;
  if (index >= This->desc.BufferCount || !This->buffers[index]) return DXGI_ERROR_INVALID_CALL;
  return This->buffers[index]->lpVtbl->QueryInterface(This->buffers[index], riid, surface);
}

/* Puts the pixels of the buffer in the window. */
static HRESULT show(SwapChain *s, IUnknown *buffer) {
  const SIZE_T bytes = (SIZE_T)s->desc.Width * s->desc.Height * 4;
  if (s->pixels_size < bytes) {
    mem_free(s->pixels);
    s->pixels = mem_alloc(bytes);
    s->pixels_size = s->pixels ? bytes : 0;
    if (!s->pixels) return E_OUTOFMEMORY;
  }
  const HRESULT hr = s->kind == SC_PRESENTER ? s->presenter->lpVtbl->ReadBackBuffer(s->presenter, buffer, s->pixels, s->desc.Width * 4)
                                             : fallback_read(s, buffer, s->pixels, s->desc.Width * 4);
  if (FAILED(hr) || !s->window) return hr;
  HDC dc = GetDC(s->window);
  if (!dc) return S_OK;      /* (a window the display cannot give a context: nothing to show it on) */
  BITMAPINFO info;
  memset(&info, 0, sizeof info);
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = (LONG)s->desc.Width;
  info.bmiHeader.biHeight = -(LONG)s->desc.Height;      /* (top-down) */
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  SetDIBitsToDevice(dc, 0, 0, s->desc.Width, s->desc.Height, 0, 0, 0, s->desc.Height, s->pixels, &info, DIB_RGB_COLORS);
  ReleaseDC(s->window, dc);
  return S_OK;
}
static int flip_model(const SwapChain *s) { return s->desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || s->desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD; }
static HRESULT STDMETHODCALLTYPE sc_Present(SwapChain *This, UINT interval, UINT flags) {
  if (interval > 4 || (flags & ~(DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE | DXGI_PRESENT_RESTART | DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_RESTRICT_TO_OUTPUT)))
    return DXGI_ERROR_INVALID_CALL;
  if (flags & DXGI_PRESENT_TEST) return S_OK;
  /* (Direct3D 12 draws into buffer GetCurrentBackBufferIndex, which moves on; Direct3D 10 and 11 always into buffer 0 - Windows renames the buffers behind it) */
  const BOOL rotate = This->on_queue && flip_model(This);
  IUnknown *front = This->buffers[rotate ? This->current : 0];
  if (!front) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = show(This, front);
  if (FAILED(hr)) return hr;
  This->present_count++;
  if (rotate) This->current = (This->current + 1) % This->desc.BufferCount;
  if (interval) Sleep(0);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_Present1(SwapChain *This, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS *parameters) { return sc_Present(This, interval, flags); }
static HRESULT STDMETHODCALLTYPE sc_SetFullscreenState(SwapChain *This, BOOL fullscreen, IDXGIOutput *target) {
  if (fullscreen) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetFullscreenState(SwapChain *This, BOOL *fullscreen, IDXGIOutput **target) {
  if (fullscreen) *fullscreen = FALSE;
  if (target) *target = NULL;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetDesc(SwapChain *This, DXGI_SWAP_CHAIN_DESC *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  memset(d, 0, sizeof *d);
  d->BufferDesc.Width = This->desc.Width;
  d->BufferDesc.Height = This->desc.Height;
  d->BufferDesc.Format = This->desc.Format;
  d->BufferDesc.RefreshRate = This->fullscreen.RefreshRate;
  d->BufferDesc.ScanlineOrdering = This->fullscreen.ScanlineOrdering;
  d->BufferDesc.Scaling = This->fullscreen.Scaling;
  d->SampleDesc = This->desc.SampleDesc;
  d->BufferUsage = This->desc.BufferUsage;
  d->BufferCount = This->desc.BufferCount;
  d->OutputWindow = This->window;
  d->Windowed = TRUE;
  d->SwapEffect = This->desc.SwapEffect;
  d->Flags = This->desc.Flags;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetDesc1(SwapChain *This, DXGI_SWAP_CHAIN_DESC1 *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  *d = This->desc;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetFullscreenDesc(SwapChain *This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *d) {
  if (!d) return DXGI_ERROR_INVALID_CALL;
  *d = This->fullscreen;
  d->Windowed = TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetHwnd(SwapChain *This, HWND *hwnd) {
  if (!hwnd) return DXGI_ERROR_INVALID_CALL;
  *hwnd = This->window;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetCoreWindow(SwapChain *This, REFIID riid, void **window) { if (window) *window = NULL; return DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE sc_ResizeBuffers(SwapChain *This, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
  /* (every reference to a buffer must have been released: the runtime refuses to drop a buffer that is still held, which shows as a buffer that cannot be made again) */
  for (UINT i = 0; i < This->desc.BufferCount; ++i) {
    if (!This->buffers[i]) continue;
    This->buffers[i]->lpVtbl->AddRef(This->buffers[i]);
    const ULONG refs = This->buffers[i]->lpVtbl->Release(This->buffers[i]);
    if (refs > 1) return DXGI_ERROR_INVALID_CALL;
  }
  DXGI_SWAP_CHAIN_DESC1 old = This->desc;
  if (count) This->desc.BufferCount = count;
  if (format != DXGI_FORMAT_UNKNOWN) This->desc.Format = format;
  if (width == 0 || height == 0) {
    RECT r = {0, 0, 0, 0};
    if (This->window && GetClientRect(This->window, &r)) { width = (UINT)(r.right - r.left); height = (UINT)(r.bottom - r.top); }
    if (width == 0) width = old.Width;
    if (height == 0) height = old.Height;
  }
  This->desc.Width = width;
  This->desc.Height = height;
  This->desc.Flags = flags ? flags : This->desc.Flags;
  if (This->desc.BufferCount > MAX_BUFFERS || !is_swap_format(This->desc.Format)) { This->desc = old; return DXGI_ERROR_INVALID_CALL; }
  const HRESULT hr = make_buffers(This);
  if (FAILED(hr)) { This->desc = old; make_buffers(This); }
  return hr;
}
static HRESULT STDMETHODCALLTYPE sc_ResizeBuffers1(SwapChain *This, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT *masks, IUnknown *const *queues) {
  return sc_ResizeBuffers(This, count, width, height, format, flags);
}
static HRESULT STDMETHODCALLTYPE sc_ResizeTarget(SwapChain *This, const DXGI_MODE_DESC *mode) { return mode ? S_OK : DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE sc_GetContainingOutput(SwapChain *This, IDXGIOutput **output) {
  IDXGIAdapter *adapter = NULL;
  if (!output) return DXGI_ERROR_INVALID_CALL;
  *output = NULL;
  HRESULT hr = fac_EnumAdapters(This->factory, 0, &adapter);
  if (FAILED(hr)) return hr;
  hr = adp_EnumOutputs((Adapter *)adapter, 0, output);
  dx_Release(OBJ(adapter));
  return hr;
}
static HRESULT STDMETHODCALLTYPE sc_GetFrameStatistics(SwapChain *This, DXGI_FRAME_STATISTICS *stats) { return DXGI_ERROR_FRAME_STATISTICS_DISJOINT; }
static HRESULT STDMETHODCALLTYPE sc_GetLastPresentCount(SwapChain *This, UINT *count) {
  if (!count) return DXGI_ERROR_INVALID_CALL;
  *count = This->present_count;
  return S_OK;
}
static BOOL STDMETHODCALLTYPE sc_IsTemporaryMonoSupported(SwapChain *This) { return FALSE; }
static HRESULT STDMETHODCALLTYPE sc_GetRestrictToOutput(SwapChain *This, IDXGIOutput **output) { if (output) *output = NULL; return S_OK; }
static HRESULT STDMETHODCALLTYPE sc_SetBackgroundColor(SwapChain *This, const DXGI_RGBA *color) { return color ? S_OK : DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE sc_GetBackgroundColor(SwapChain *This, DXGI_RGBA *color) {
  if (!color) return DXGI_ERROR_INVALID_CALL;
  memset(color, 0, sizeof *color);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_SetRotation(SwapChain *This, DXGI_MODE_ROTATION rotation) { return rotation == DXGI_MODE_ROTATION_IDENTITY || rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? S_OK : DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE sc_GetRotation(SwapChain *This, DXGI_MODE_ROTATION *rotation) {
  if (!rotation) return DXGI_ERROR_INVALID_CALL;
  *rotation = DXGI_MODE_ROTATION_IDENTITY;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_SetSourceSize(SwapChain *This, UINT width, UINT height) { return S_OK; }
static HRESULT STDMETHODCALLTYPE sc_GetSourceSize(SwapChain *This, UINT *width, UINT *height) {
  if (!width || !height) return DXGI_ERROR_INVALID_CALL;
  *width = This->desc.Width;
  *height = This->desc.Height;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_SetMaximumFrameLatency(SwapChain *This, UINT latency) {
  if (latency == 0 || latency > 16) return DXGI_ERROR_INVALID_CALL;
  This->latency = latency;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_GetMaximumFrameLatency(SwapChain *This, UINT *latency) {
  if (!latency) return DXGI_ERROR_INVALID_CALL;
  *latency = This->latency;
  return S_OK;
}
static HANDLE STDMETHODCALLTYPE sc_GetFrameLatencyWaitableObject(SwapChain *This) { return NULL; }
static HRESULT STDMETHODCALLTYPE sc_SetMatrixTransform(SwapChain *This, const DXGI_MATRIX_3X2_F *matrix) { return DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE sc_GetMatrixTransform(SwapChain *This, DXGI_MATRIX_3X2_F *matrix) { return DXGI_ERROR_INVALID_CALL; }
static UINT STDMETHODCALLTYPE sc_GetCurrentBackBufferIndex(SwapChain *This) { return This->current; }
static HRESULT STDMETHODCALLTYPE sc_CheckColorSpaceSupport(SwapChain *This, DXGI_COLOR_SPACE_TYPE space, UINT *support) {
  if (!support) return DXGI_ERROR_INVALID_CALL;
  *support = space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sc_SetColorSpace1(SwapChain *This, DXGI_COLOR_SPACE_TYPE space) { return space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? S_OK : E_INVALIDARG; }

/* ---- the vtables (generated by scripts/gen-dx-vtables.py from the functions above) ----------------------------------------------------------------------------------------- */

#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release
#define obj_SetPrivateData dx_SetPrivateData
#define obj_SetPrivateDataInterface dx_SetPrivateDataInterface
#define obj_GetPrivateData dx_GetPrivateData
#include "vtbl.inc"

/* ---- the entry points -------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* (a factory of CreateDXGIFactory is DXGI 1.0's: IDXGIFactory alone, as on Windows - IDXGIFactory1 and later are CreateDXGIFactory1's and CreateDXGIFactory2's) */
static const GUID *const g_factory10_iids[] = {&IID_IDXGIObject, &IID_IDXGIFactory, NULL};
static HRESULT create_factory(UINT flags, REFIID riid, void **out, BOOL dxgi10) {
  if (!out) return DXGI_ERROR_INVALID_CALL;
  *out = NULL;
  Factory *f = mem_alloc(sizeof *f);
  if (!f) return E_OUTOFMEMORY;
  dx_init(OBJ(f), &fac_vtbl, dxgi10 ? g_factory10_iids : g_factory_iids, factory_destroy);
  f->flags = flags;
  f->count = read_adapters(f->adapters);
  const HRESULT hr = dx_QueryInterface(OBJ(f), riid, out);
  dx_Release(OBJ(f));
  return hr;
}

EXPORT HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **factory) { return create_factory(0, riid, factory, TRUE); }
EXPORT HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **factory) { return create_factory(0, riid, factory, FALSE); }
EXPORT HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **factory) { return create_factory(flags, riid, factory, FALSE); }
EXPORT HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **debug) {
  if (debug) *debug = NULL;
  return DXGI_ERROR_SDK_COMPONENT_MISSING;
}
EXPORT HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void) { return S_OK; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_private_lock);
  return TRUE;
}
