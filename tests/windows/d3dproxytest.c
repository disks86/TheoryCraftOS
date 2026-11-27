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

/* The Direct3D family with objects that are not its own (src/Windows/tcos_object.h): the program wraps the runtimes' objects in pass-through COM wrappers of its own
 * and hands those to the runtimes, as a proxy (ReShade, an overlay) does.
 *   - a "proxy" wrapper answers its own interfaces itself and forwards every other IID to the object it wraps (ReShade's way): the runtimes see through it;
 *   - a "foreign" wrapper answers only its own interfaces: an object of another runtime (DXVK, vkd3d-proton) as far as the libraries can tell - its public methods
 *     work (they go to the wrapped object), nothing else does.
 * Checked: the identity interface itself, d3d12.dll's private exports with foreign objects, D3D11On12CreateDevice and CreateWrappedResource with proxies and foreign
 * objects, dxgi swap chains on proxied and foreign Direct3D 11, 10 and 12 devices and queues (the foreign ones get the fallback swap chain: the pixels Present puts in
 * the window are read back with GDI), Direct3D 9 calls with proxied and foreign objects and d3d9.dll's private exports, the Agility SDK interfaces
 * (ID3D12SDKConfiguration1, ID3D12DeviceFactory, ID3D12DeviceConfiguration, the program's D3D12SDKVersion export), and Vulkan's deviceLUID against DXGI's adapters.
 * 64-bit (tests/d3d12_qemu.sh d3dproxytest) and 32-bit (tests/wow64_qemu.sh d3dproxytest). */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS 1
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "../../src/Windows/tcosdx.h"

/* (the Agility SDK version this program says it is built for: d3d12.dll reads it, as Windows' loader does; 0x887E0003 is D3D12_ERROR_INVALID_REDIST) */
__declspec(dllexport) extern const UINT D3D12SDKVersion;
const UINT D3D12SDKVersion = 610;
__declspec(dllexport) extern const char *D3D12SDKPath;
const char *D3D12SDKPath = ".\\D3D12\\";
static const GUID CLSID_D3D12DeviceFactory_ = {0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};

static int g_checks, g_failed;
#define CHECK(x) do { ++g_checks; if (!(x)) { ++g_failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)
#define CHECK_HR(x) do { HRESULT hr_ = (x); ++g_checks; if (FAILED(hr_)) { ++g_failed; printf("FAILED line %d: %s = %08lx\n", __LINE__, #x, (unsigned long)hr_); } } while (0)

/* ---- the wrappers ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* A wrapper: its vtable's slots 3 and up are thunks that put the wrapped object in place of `this` and jump to the same slot of its vtable, so every method of every
 * interface is passed through as it is (the arguments untouched). QueryInterface, AddRef and Release are the wrapper's own. */
typedef struct Wrap {
  const void *const *lpVtbl;
  IUnknown *real;          /* at the second pointer: the thunks read it there */
  LONG ref;
  const GUID *const *iids; /* what the wrapper answers itself (IUnknown too) */
  int forward;             /* a proxy: other IIDs go to the wrapped object */
} Wrap;
#define SLOTS 200
static void *g_vtbl[SLOTS];

static HRESULT STDMETHODCALLTYPE wrap_QueryInterface(Wrap *w, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  int own = IsEqualGUID(riid, &IID_IUnknown);
  for (const GUID *const *i = w->iids; !own && *i; ++i) own = IsEqualGUID(riid, *i);
  if (own) {
    InterlockedIncrement(&w->ref);
    *out = w;
    return S_OK;
  }
  return w->forward ? w->real->lpVtbl->QueryInterface(w->real, riid, out) : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE wrap_AddRef(Wrap *w) { return (ULONG)InterlockedIncrement(&w->ref); }
static ULONG STDMETHODCALLTYPE wrap_Release(Wrap *w) {
  const LONG n = InterlockedDecrement(&w->ref);
  if (n == 0) {
    w->real->lpVtbl->Release(w->real);
    HeapFree(GetProcessHeap(), 0, w);
  }
  return (ULONG)n;
}

static BOOL wrappers_init(void) {
#ifdef _WIN64
  enum { THUNK = 16 };      /* (13 bytes of code) */
#else
  enum { THUNK = 20 };      /* (19 bytes of code) */
#endif
  BYTE *code = VirtualAlloc(NULL, SLOTS * THUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!code) return FALSE;
  for (UINT i = 3; i < SLOTS; ++i) {
    BYTE *t = code + i * THUNK;
    const UINT32 disp = i * (UINT32)sizeof(void *);
#ifdef _WIN64
    const BYTE head[] = {0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0};      /* mov rcx,[rcx+8]; mov rax,[rcx]; jmp [rax+disp32] */
#else
    const BYTE head[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0x89, 0x44, 0x24, 0x04, 0x8B, 0x08, 0xFF, 0xA1};      /* this = this->real; jmp [vtbl+disp32] */
#endif
    memcpy(t, head, sizeof head);
    memcpy(t + sizeof head, &disp, 4);
    g_vtbl[i] = t;
  }
  DWORD old = 0;
  if (!VirtualProtect(code, SLOTS * THUNK, PAGE_EXECUTE_READ, &old)) return FALSE;
  FlushInstructionCache(GetCurrentProcess(), code, SLOTS * THUNK);
  g_vtbl[0] = (void *)wrap_QueryInterface;
  g_vtbl[1] = (void *)wrap_AddRef;
  g_vtbl[2] = (void *)wrap_Release;
  return TRUE;
}
/* A wrapper of `real` (which it takes a reference of) answering `iids` itself; `forward`: a proxy. */
static void *wrap(void *real, const GUID *const *iids, int forward) {
  Wrap *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
  if (!w || !real) return NULL;
  w->lpVtbl = (const void *const *)g_vtbl;
  w->real = real;
  ((IUnknown *)real)->lpVtbl->AddRef((IUnknown *)real);
  w->ref = 1;
  w->iids = iids;
  w->forward = forward;
  return w;
}

static const GUID *const kDevice12[] = {&IID_ID3D12Object, &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4, &IID_ID3D12Device5, NULL};
static const GUID *const kQueue12[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12CommandQueue, NULL};
static const GUID *const kResource12[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12Resource, &IID_ID3D12Resource1, NULL};
static const GUID *const kDevice11[] = {&IID_ID3D11Device, &IID_ID3D11Device1, NULL};
static const GUID *const kDevice10[] = {&IID_ID3D10Device, &IID_ID3D10Device1, NULL};
static const GUID *const kSurface9[] = {&IID_IDirect3DResource9, &IID_IDirect3DSurface9, NULL};
static const GUID *const kTexture9[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DTexture9, NULL};
static const GUID *const kBuffer9[] = {&IID_IDirect3DResource9, &IID_IDirect3DVertexBuffer9, NULL};
static const GUID *const kShader9[] = {&IID_IDirect3DVertexShader9, NULL};
static const GUID *const kDevice9[] = {&IID_IDirect3DDevice9, NULL};

/* ---- helpers --------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HWND g_window;
static HWND make_window(const wchar_t *name) {
  WNDCLASSW cls;
  memset(&cls, 0, sizeof cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"d3dproxytest";
  RegisterClassW(&cls);
  return CreateWindowExW(0, L"d3dproxytest", name, WS_POPUP | WS_VISIBLE, 40, 40, 160, 120, NULL, NULL, cls.hInstance, NULL);
}
/* What the window shows at (x, y), as GDI reads it back (0x00BBGGRR). */
static COLORREF window_pixel(HWND window, int x, int y) {
  HDC dc = GetDC(window);
  COLORREF c = dc ? GetPixel(dc, x, y) : CLR_INVALID;
  if (dc) ReleaseDC(window, dc);
  return c;
}

/* Direct3D 12: a command list run on the queue and waited for. */
typedef struct Run12 {
  ID3D12Device *dev;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *alloc;
  ID3D12GraphicsCommandList *list;
  ID3D12Fence *fence;
  UINT64 value;
} Run12;
static BOOL run12_begin(Run12 *r, ID3D12Device *dev, ID3D12CommandQueue *queue) {
  memset(r, 0, sizeof *r);
  r->dev = dev;
  r->queue = queue;
  if (FAILED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&r->alloc))) return FALSE;
  if (FAILED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&r->list))) return FALSE;
  return SUCCEEDED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&r->fence));
}
static void run12_end(Run12 *r) {
  ID3D12GraphicsCommandList_Close(r->list);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)r->list};
  ID3D12CommandQueue_ExecuteCommandLists(r->queue, 1, lists);
  ID3D12CommandQueue_Signal(r->queue, r->fence, ++r->value);
  if (ID3D12Fence_GetCompletedValue(r->fence) < r->value) {
    HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(r->fence, r->value, e);
    WaitForSingleObject(e, 10000);
    CloseHandle(e);
  }
  ID3D12CommandAllocator_Reset(r->alloc);
  ID3D12GraphicsCommandList_Reset(r->list, r->alloc, NULL);
}
static void run12_free(Run12 *r) {
  if (r->list) { ID3D12GraphicsCommandList_Close(r->list); ID3D12GraphicsCommandList_Release(r->list); }
  if (r->alloc) ID3D12CommandAllocator_Release(r->alloc);
  if (r->fence) ID3D12Fence_Release(r->fence);
}
static void barrier12(ID3D12GraphicsCommandList *l, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b;
  memset(&b, 0, sizeof b);
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &b);
}
static ID3D12Resource *texture12(ID3D12Device *dev, UINT w, UINT h, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd;
  memset(&rd, 0, sizeof rd);
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ID3D12Resource *t = NULL;
  ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&t);
  return t;
}
/* The first pixel of a B8G8R8A8 texture in state `state` (0xAARRGGBB), copied out through a readback buffer. */
static UINT32 texture12_pixel(Run12 *r, ID3D12Resource *t, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC bd;
  memset(&bd, 0, sizeof bd);
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 65536;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource *rb = NULL;
  if (FAILED(ID3D12Device_CreateCommittedResource(r->dev, &heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb))) return 0;
  const D3D12_RESOURCE_DESC td = ID3D12Resource_GetDesc(t);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  UINT64 row_bytes, total;
  ID3D12Device_GetCopyableFootprints(r->dev, &td, 0, 1, 0, &fp, &rows, &row_bytes, &total);
  D3D12_TEXTURE_COPY_LOCATION to = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp}}, from = {t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) barrier12(r->list, t, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ID3D12GraphicsCommandList_CopyTextureRegion(r->list, &to, 0, 0, 0, &from, NULL);
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) barrier12(r->list, t, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  run12_end(r);
  UINT32 px = 0;
  void *data = NULL;
  if (SUCCEEDED(ID3D12Resource_Map(rb, 0, NULL, &data)) && data) {
    memcpy(&px, (BYTE *)data + fp.Offset, 4);
    ID3D12Resource_Unmap(rb, 0, NULL);
  }
  ID3D12Resource_Release(rb);
  return px;
}
/* Clears a texture in the PRESENT state (a back buffer) to a colour, with an RTV of a heap of its own. */
static void clear12(Run12 *r, ID3D12Resource *t, const float colour[4]) {
  ID3D12DescriptorHeap *heap = NULL;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(ID3D12Device_CreateDescriptorHeap(r->dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap))) return;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
  ID3D12Device_CreateRenderTargetView(r->dev, t, NULL, rtv);
  barrier12(r->list, t, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12GraphicsCommandList_ClearRenderTargetView(r->list, rtv, colour, 0, NULL);
  barrier12(r->list, t, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  run12_end(r);
  ID3D12DescriptorHeap_Release(heap);
}

/* ---- the identity interface and d3d12.dll's private exports ------------------------------------------------------------------------------------------------------------- */

static void test_identity(ID3D12Device *dev) {
  IUnknown *got = NULL;
  CHECK(SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ITcosObject, (void **)&got)) && got == (IUnknown *)dev);
  if (got) IUnknown_Release(got);
  ID3D12Device *proxy = wrap(dev, kDevice12, 1), *foreign = wrap(dev, kDevice12, 0);
  got = NULL;
  CHECK(SUCCEEDED(ID3D12Device_QueryInterface(proxy, &IID_ITcosObject, (void **)&got)) && got == (IUnknown *)dev);      /* (a proxy forwards it: the real device) */
  if (got) IUnknown_Release(got);
  got = NULL;
  CHECK(ID3D12Device_QueryInterface(foreign, &IID_ITcosObject, (void **)&got) == E_NOINTERFACE && !got);
  /* the private exports: the device behind a proxy, never a foreign object's memory */
  HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
  typedef HRESULT(WINAPI * Options)(ID3D12Device *, UINT, UINT);
  typedef UINT(WINAPI * Features3)(ID3D12Device *);
  typedef HRESULT(WINAPI * ObjectFn)(IUnknown *, REFIID, void **);
  typedef HRESULT(WINAPI * Extra)(ID3D12Device *, const void *, UINT);
  Options options = (Options)(void *)GetProcAddress(d3d12, "TcosD3D12PipelineOptions");
  Features3 features3 = (Features3)(void *)GetProcAddress(d3d12, "TcosD3D12DeviceFeatures3");
  ObjectFn object = (ObjectFn)(void *)GetProcAddress(d3d12, "TcosD3D12Object");
  Extra extra = (Extra)(void *)GetProcAddress(d3d12, "TcosD3D12ShareExtra");
  CHECK(options && features3 && object && extra);
  if (options && features3 && object && extra) {
    CHECK(options(foreign, 1, 0) == E_INVALIDARG);
    CHECK(options(proxy, 0, 0) == S_OK);
    CHECK(options(dev, 0, 0) == S_OK);
    CHECK(features3(foreign) == 0);
    CHECK(features3(proxy) == features3(dev));
    BYTE blob[80] = {0};
    CHECK(extra(foreign, blob, 8) == E_INVALIDARG);
    CHECK(extra(dev, blob, 80) == E_INVALIDARG);      /* (more than the 64 bytes it carries) */
    CHECK(extra(dev, NULL, 0) == S_OK);
    ID3D12Device *real = NULL;
    CHECK(object((IUnknown *)proxy, &IID_ID3D12Device, (void **)&real) == S_OK && real == dev);
    if (real) ID3D12Device_Release(real);
    real = NULL;
    CHECK(object((IUnknown *)foreign, &IID_ID3D12Device, (void **)&real) == E_INVALIDARG && !real);
    ID3D12CommandQueue *q = NULL;
    CHECK(object((IUnknown *)dev, &IID_ID3D12CommandQueue, (void **)&q) == E_NOINTERFACE && !q);
  }
  ID3D12Device_Release(proxy);
  ID3D12Device_Release(foreign);
}

/* ---- 11 on 12 with proxied and foreign objects --------------------------------------------------------------------------------------------------------------------------- */

static void test_11on12(ID3D12Device *dev, ID3D12CommandQueue *queue) {
  ID3D12Device *proxy_dev = wrap(dev, kDevice12, 1), *foreign_dev = wrap(dev, kDevice12, 0);
  ID3D12CommandQueue *proxy_queue = wrap(queue, kQueue12, 1), *foreign_queue = wrap(queue, kQueue12, 0);
  ID3D11Device *d11 = NULL;
  ID3D11DeviceContext *ctx = NULL;
  D3D_FEATURE_LEVEL level = 0;
  IUnknown *queues[1] = {(IUnknown *)foreign_queue};
  /* (another runtime's device or queue: no device - E_INVALIDARG, nothing touched) */
  CHECK(D3D11On12CreateDevice((IUnknown *)foreign_dev, 0, NULL, 0, queues, 1, 0, &d11, &ctx, &level) == E_INVALIDARG && !d11 && !ctx);
  queues[0] = (IUnknown *)proxy_queue;
  CHECK(D3D11On12CreateDevice((IUnknown *)foreign_dev, 0, NULL, 0, queues, 1, 0, &d11, &ctx, &level) == E_INVALIDARG && !d11);
  queues[0] = (IUnknown *)foreign_queue;
  CHECK(D3D11On12CreateDevice((IUnknown *)proxy_dev, 0, NULL, 0, queues, 1, 0, &d11, &ctx, &level) == E_INVALIDARG && !d11);
  /* (ReShade's wrappers: seen through) */
  queues[0] = (IUnknown *)proxy_queue;
  CHECK_HR(D3D11On12CreateDevice((IUnknown *)proxy_dev, 0, NULL, 0, queues, 1, 0, &d11, &ctx, &level));
  if (d11 && ctx) {
    CHECK(level >= D3D_FEATURE_LEVEL_11_0);
    ID3D11On12Device1 *on12 = NULL;
    CHECK_HR(ID3D11Device_QueryInterface(d11, &IID_ID3D11On12Device1, (void **)&on12));
    ID3D12Resource *tex = texture12(dev, 16, 16, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource *proxy_tex = wrap(tex, kResource12, 1), *foreign_tex = wrap(tex, kResource12, 0);
    if (on12 && tex) {
      const D3D11_RESOURCE_FLAGS flags = {D3D11_BIND_RENDER_TARGET, 0, 0, 0};
      ID3D11Resource *res11 = NULL;
      CHECK(ID3D11On12Device1_CreateWrappedResource(on12, (IUnknown *)foreign_tex, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   &IID_ID3D11Resource, (void **)&res11) == E_INVALIDARG && !res11);
      CHECK_HR(ID3D11On12Device1_CreateWrappedResource(on12, (IUnknown *)proxy_tex, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                      &IID_ID3D11Resource, (void **)&res11));
      if (res11) {
        ID3D11RenderTargetView *rtv = NULL;
        CHECK_HR(ID3D11Device_CreateRenderTargetView(d11, res11, NULL, &rtv));
        if (rtv) {
          const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
          ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
          ID3D11RenderTargetView_Release(rtv);
        }
        ID3D11On12Device1_ReleaseWrappedResources(on12, &res11, 1);
        ID3D11DeviceContext_Flush(ctx);
        Run12 r;
        if (run12_begin(&r, dev, queue)) CHECK(texture12_pixel(&r, tex, D3D12_RESOURCE_STATE_RENDER_TARGET) == 0xFF00FF00u);      /* (what Direct3D 11 drew, in Direct3D 12) */
        run12_free(&r);
        ID3D11Resource_Release(res11);
      }
      ID3D12Device *back = NULL;
      CHECK(SUCCEEDED(ID3D11On12Device1_GetD3D12Device(on12, &IID_ID3D12Device, &back)) && back == dev);
      if (back) ID3D12Device_Release(back);
    }
    if (proxy_tex) ID3D12Resource_Release(proxy_tex);
    if (foreign_tex) ID3D12Resource_Release(foreign_tex);
    if (tex) ID3D12Resource_Release(tex);
    if (on12) ID3D11On12Device1_Release(on12);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(d11);
  }
  ID3D12Device_Release(proxy_dev);
  ID3D12Device_Release(foreign_dev);
  ID3D12CommandQueue_Release(proxy_queue);
  ID3D12CommandQueue_Release(foreign_queue);
}

/* ---- dxgi swap chains on proxied and foreign devices -------------------------------------------------------------------------------------------------------------------- */

static IDXGIFactory2 *g_factory;

static DXGI_SWAP_CHAIN_DESC1 chain_desc(UINT count, DXGI_SWAP_EFFECT effect) {
  DXGI_SWAP_CHAIN_DESC1 sd;
  memset(&sd, 0, sizeof sd);
  sd.Width = 64;
  sd.Height = 48;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = count;
  sd.SwapEffect = effect;
  return sd;
}

/* A Direct3D 11 swap chain on `device` (the real one, a proxy or a foreign one): cleared through the real device, presented, read back from the window. */
static void swap11(ID3D11Device *real, IUnknown *device, const char *what, const float colour[4], COLORREF expect) {
  DXGI_SWAP_CHAIN_DESC1 sd = chain_desc(2, DXGI_SWAP_EFFECT_FLIP_DISCARD);
  IDXGISwapChain1 *chain = NULL;
  printf("d3dproxytest: swap11 %s\n", what);
  const HRESULT made = IDXGIFactory2_CreateSwapChainForHwnd(g_factory, device, g_window, &sd, NULL, NULL, &chain);
  CHECK(made == S_OK && chain);
  if (!chain) { printf("swap chain on %s: %08lx\n", what, (unsigned long)made); return; }
  ID3D11DeviceContext *ctx = NULL;
  ID3D11Device_GetImmediateContext(real, &ctx);
  IDXGISwapChain3 *chain3 = NULL;
  IDXGISwapChain1_QueryInterface(chain, &IID_IDXGISwapChain3, (void **)&chain3);
  for (int frame = 0; frame < 3; ++frame) {
    ID3D11Texture2D *back = NULL;
    CHECK_HR(IDXGISwapChain1_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&back));
    if (!back) break;
    ID3D11RenderTargetView *rtv = NULL;
    CHECK_HR(ID3D11Device_CreateRenderTargetView(real, (ID3D11Resource *)back, NULL, &rtv));
    if (rtv) {
      const float c[4] = {colour[0], colour[1], colour[2], 1.0f};
      ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
      ID3D11RenderTargetView_Release(rtv);
    }
    ID3D11Texture2D_Release(back);
    CHECK_HR(IDXGISwapChain1_Present(chain, 0, 0));
    const COLORREF got = window_pixel(g_window, 5, 5);
    if (got != expect) printf("%s frame %d: the window shows %06lx, not %06lx\n", what, frame, (unsigned long)got, (unsigned long)expect);
    CHECK(got == expect);
    if (chain3) CHECK(IDXGISwapChain3_GetCurrentBackBufferIndex(chain3) == 0);      /* (Direct3D 11: always buffer 0) */
  }
  /* (the buffers made again, and presented) */
  CHECK_HR(IDXGISwapChain1_ResizeBuffers(chain, 0, 32, 24, DXGI_FORMAT_UNKNOWN, 0));
  ID3D11Texture2D *back = NULL;
  CHECK_HR(IDXGISwapChain1_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&back));
  if (back) {
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(back, &td);
    CHECK(td.Width == 32 && td.Height == 24 && td.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    ID3D11RenderTargetView *rtv = NULL;
    if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(real, (ID3D11Resource *)back, NULL, &rtv)) && rtv) {
      const float white[4] = {1, 1, 1, 1};
      ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, white);
      ID3D11RenderTargetView_Release(rtv);
    }
    ID3D11Texture2D_Release(back);
    CHECK_HR(IDXGISwapChain1_Present(chain, 0, 0));
    CHECK(window_pixel(g_window, 5, 5) == RGB(255, 255, 255));
  }
  IUnknown *of_chain = NULL;
  CHECK(SUCCEEDED(IDXGISwapChain1_GetDevice(chain, &IID_IUnknown, (void **)&of_chain)) && of_chain == device);
  if (of_chain) IUnknown_Release(of_chain);
  if (chain3) IDXGISwapChain3_Release(chain3);
  if (ctx) ID3D11DeviceContext_Release(ctx);
  IDXGISwapChain1_Release(chain);
}

static void test_swap_chains_11(void) {
  ID3D11Device *d11 = NULL;
  CHECK_HR(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d11, NULL, NULL));
  if (!d11) return;
  const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, yellow[4] = {1, 1, 0, 1};
  swap11(d11, (IUnknown *)d11, "Direct3D 11", red, RGB(255, 0, 0));
  ID3D11Device *proxy = wrap(d11, kDevice11, 1), *foreign = wrap(d11, kDevice11, 0);
  swap11(d11, (IUnknown *)proxy, "a proxy of Direct3D 11", blue, RGB(0, 0, 255));
  swap11(d11, (IUnknown *)foreign, "a foreign Direct3D 11 (the fallback)", yellow, RGB(255, 255, 0));
  /* (a flip effect needs two buffers) */
  DXGI_SWAP_CHAIN_DESC1 sd = chain_desc(1, DXGI_SWAP_EFFECT_FLIP_DISCARD);
  IDXGISwapChain1 *chain = NULL;
  CHECK(IDXGIFactory2_CreateSwapChainForHwnd(g_factory, (IUnknown *)foreign, g_window, &sd, NULL, NULL, &chain) == DXGI_ERROR_INVALID_CALL && !chain);
  ID3D11Device_Release(proxy);
  ID3D11Device_Release(foreign);
  ID3D11Device_Release(d11);
}

static void test_swap_chain_10(void) {
  ID3D10Device *d10 = NULL;
  CHECK_HR(D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &d10));
  if (!d10) return;
  ID3D10Device *foreign = wrap(d10, kDevice10, 0);
  DXGI_SWAP_CHAIN_DESC1 sd = chain_desc(1, DXGI_SWAP_EFFECT_DISCARD);
  IDXGISwapChain1 *chain = NULL;
  CHECK_HR(IDXGIFactory2_CreateSwapChainForHwnd(g_factory, (IUnknown *)foreign, g_window, &sd, NULL, NULL, &chain));
  if (chain) {
    ID3D10Texture2D *back = NULL;
    CHECK_HR(IDXGISwapChain1_GetBuffer(chain, 0, &IID_ID3D10Texture2D, (void **)&back));
    if (back) {
      ID3D10RenderTargetView *rtv = NULL;
      CHECK_HR(ID3D10Device_CreateRenderTargetView(d10, (ID3D10Resource *)back, NULL, &rtv));
      if (rtv) {
        const float cyan[4] = {0, 1, 1, 1};
        ID3D10Device_ClearRenderTargetView(d10, rtv, cyan);
        ID3D10RenderTargetView_Release(rtv);
      }
      ID3D10Texture2D_Release(back);
    }
    CHECK_HR(IDXGISwapChain1_Present(chain, 0, 0));
    const COLORREF got = window_pixel(g_window, 5, 5);
    if (got != RGB(0, 255, 255)) printf("foreign Direct3D 10: the window shows %06lx\n", (unsigned long)got);
    CHECK(got == RGB(0, 255, 255));
    IDXGISwapChain1_Release(chain);
  }
  ID3D10Device_Release(foreign);
  ID3D10Device_Release(d10);
}

/* A Direct3D 12 swap chain on `queue` (real, proxy or foreign): each buffer cleared through the real device, presented, read back from the window. */
static void swap12(ID3D12Device *dev, ID3D12CommandQueue *real_queue, IUnknown *queue, const char *what) {
  DXGI_SWAP_CHAIN_DESC1 sd = chain_desc(2, DXGI_SWAP_EFFECT_DISCARD);
  IDXGISwapChain1 *chain = NULL;
  CHECK(IDXGIFactory2_CreateSwapChainForHwnd(g_factory, queue, g_window, &sd, NULL, NULL, &chain) == DXGI_ERROR_INVALID_CALL && !chain);      /* (flip effects only) */
  sd = chain_desc(2, DXGI_SWAP_EFFECT_FLIP_DISCARD);
  const HRESULT made = IDXGIFactory2_CreateSwapChainForHwnd(g_factory, queue, g_window, &sd, NULL, NULL, &chain);
  CHECK(made == S_OK && chain);
  if (!chain) { printf("swap chain on %s: %08lx\n", what, (unsigned long)made); return; }
  IDXGISwapChain3 *chain3 = NULL;
  CHECK_HR(IDXGISwapChain1_QueryInterface(chain, &IID_IDXGISwapChain3, (void **)&chain3));
  Run12 r;
  if (chain3 && run12_begin(&r, dev, real_queue)) {
    static const float colours[3][4] = {{1, 0, 1, 1}, {0, 0.5f, 1, 1}, {0.25f, 1, 0.5f, 1}};
    for (int frame = 0; frame < 3; ++frame) {
      const UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(chain3);
      CHECK(index == (UINT)frame % 2);
      ID3D12Resource *back = NULL;
      CHECK_HR(IDXGISwapChain3_GetBuffer(chain3, index, &IID_ID3D12Resource, (void **)&back));
      if (!back) break;
      clear12(&r, back, colours[frame]);
      ID3D12Resource_Release(back);
      CHECK_HR(IDXGISwapChain3_Present(chain3, 0, 0));
      const COLORREF expect = RGB((BYTE)(colours[frame][0] * 255.0f + 0.5f), (BYTE)(colours[frame][1] * 255.0f + 0.5f), (BYTE)(colours[frame][2] * 255.0f + 0.5f));
      const COLORREF got = window_pixel(g_window, 5, 5);
      const int close = abs((int)GetRValue(got) - (int)GetRValue(expect)) <= 1 && abs((int)GetGValue(got) - (int)GetGValue(expect)) <= 1 &&
                        abs((int)GetBValue(got) - (int)GetBValue(expect)) <= 1;
      if (!close) printf("%s frame %d: the window shows %06lx, not %06lx\n", what, frame, (unsigned long)got, (unsigned long)expect);
      CHECK(close);
    }
    CHECK_HR(IDXGISwapChain3_ResizeBuffers(chain3, 3, 40, 30, DXGI_FORMAT_UNKNOWN, 0));
    CHECK(IDXGISwapChain3_GetCurrentBackBufferIndex(chain3) == 0);
    ID3D12Resource *back = NULL;
    CHECK_HR(IDXGISwapChain3_GetBuffer(chain3, 2, &IID_ID3D12Resource, (void **)&back));
    if (back) {
      const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(back);
      CHECK(rd.Width == 40 && rd.Height == 30);
      ID3D12Resource_Release(back);
    }
    run12_free(&r);
  }
  if (chain3) IDXGISwapChain3_Release(chain3);
  IDXGISwapChain1_Release(chain);
}

static void test_swap_chains_12(ID3D12Device *dev, ID3D12CommandQueue *queue) {
  ID3D12CommandQueue *proxy = wrap(queue, kQueue12, 1), *foreign = wrap(queue, kQueue12, 0);
  swap12(dev, queue, (IUnknown *)queue, "Direct3D 12");
  swap12(dev, queue, (IUnknown *)proxy, "a proxy of Direct3D 12");
  swap12(dev, queue, (IUnknown *)foreign, "a foreign Direct3D 12 (the fallback)");
  /* (a device is no swap chain's: a queue is) */
  DXGI_SWAP_CHAIN_DESC1 sd = chain_desc(2, DXGI_SWAP_EFFECT_FLIP_DISCARD);
  IDXGISwapChain1 *chain = NULL;
  ID3D12Device *foreign_dev = wrap(dev, kDevice12, 0);
  CHECK(FAILED(IDXGIFactory2_CreateSwapChainForHwnd(g_factory, (IUnknown *)foreign_dev, g_window, &sd, NULL, NULL, &chain)) && !chain);
  ID3D12Device_Release(foreign_dev);
  ID3D12CommandQueue_Release(proxy);
  ID3D12CommandQueue_Release(foreign);
}

/* ---- Direct3D 9 --------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void test_d3d9(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  CHECK(d3d != NULL);
  if (!d3d) return;
  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 48;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_window;
  pp.Windowed = TRUE;
  IDirect3DDevice9 *dev = NULL;
  CHECK_HR(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev));
  if (dev) {
    IUnknown *got = NULL;
    CHECK(SUCCEEDED(IDirect3DDevice9_QueryInterface(dev, &IID_ITcosObject, (void **)&got)) && got == (IUnknown *)dev);
    if (got) IUnknown_Release(got);
    IDirect3DSurface9 *rt = NULL, *mem = NULL;
    CHECK_HR(IDirect3DDevice9_CreateRenderTarget(dev, 16, 16, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));
    CHECK_HR(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 16, 16, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &mem, NULL));
    if (rt && mem) {
      IDirect3DSurface9 *proxy = wrap(rt, kSurface9, 1), *foreign = wrap(rt, kSurface9, 0);
      CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, foreign) == D3DERR_INVALIDCALL);
      CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, proxy) == D3D_OK);
      IDirect3DSurface9 *bound = NULL;
      CHECK(SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &bound)) && bound == rt);      /* (the device holds the object itself) */
      if (bound) IDirect3DSurface9_Release(bound);
      CHECK_HR(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x12, 0x34, 0x56), 1.0f, 0));
      CHECK_HR(IDirect3DDevice9_GetRenderTargetData(dev, rt, mem));
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(IDirect3DSurface9_LockRect(mem, &lr, NULL, D3DLOCK_READONLY))) {
        CHECK((*(const DWORD *)lr.pBits & 0xFFFFFF) == 0x123456);
        IDirect3DSurface9_UnlockRect(mem);
      }
      /* (through a proxy of the device itself) */
      IDirect3DDevice9 *proxy_dev = wrap(dev, kDevice9, 1);
      CHECK_HR(IDirect3DDevice9_Clear(proxy_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x65, 0x43, 0x21), 1.0f, 0));
      CHECK_HR(IDirect3DDevice9_GetRenderTargetData(proxy_dev, (IDirect3DSurface9 *)proxy, mem));
      if (SUCCEEDED(IDirect3DSurface9_LockRect(mem, &lr, NULL, D3DLOCK_READONLY))) {
        CHECK((*(const DWORD *)lr.pBits & 0xFFFFFF) == 0x654321);
        IDirect3DSurface9_UnlockRect(mem);
      }
      IDirect3DDevice9_Release(proxy_dev);
      IDirect3DSurface9 *back = NULL;
      IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
      if (back) { IDirect3DDevice9_SetRenderTarget(dev, 0, back); IDirect3DSurface9_Release(back); }
      IDirect3DSurface9_Release(proxy);
      IDirect3DSurface9_Release(foreign);
    }
    if (rt) IDirect3DSurface9_Release(rt);
    if (mem) IDirect3DSurface9_Release(mem);
    IDirect3DTexture9 *tex = NULL;
    CHECK_HR(IDirect3DDevice9_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL));
    if (tex) {
      IDirect3DTexture9 *proxy = wrap(tex, kTexture9, 1), *foreign = wrap(tex, kTexture9, 0);
      CHECK(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)proxy) == D3D_OK);
      IDirect3DBaseTexture9 *got = NULL;
      CHECK(SUCCEEDED(IDirect3DDevice9_GetTexture(dev, 0, &got)) && got == (IDirect3DBaseTexture9 *)tex);
      if (got) IDirect3DBaseTexture9_Release(got);
      CHECK(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)foreign) == D3DERR_INVALIDCALL);
      IDirect3DDevice9_SetTexture(dev, 0, NULL);
      IDirect3DTexture9_Release(proxy);
      IDirect3DTexture9_Release(foreign);
      IDirect3DTexture9_Release(tex);
    }
    IDirect3DVertexBuffer9 *vb = NULL;
    CHECK_HR(IDirect3DDevice9_CreateVertexBuffer(dev, 256, 0, 0, D3DPOOL_DEFAULT, &vb, NULL));
    if (vb) {
      IDirect3DVertexBuffer9 *proxy = wrap(vb, kBuffer9, 1), *foreign = wrap(vb, kBuffer9, 0);
      CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, proxy, 0, 16) == D3D_OK);
      CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, foreign, 0, 16) == D3DERR_INVALIDCALL);
      IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
      IDirect3DVertexBuffer9_Release(proxy);
      IDirect3DVertexBuffer9_Release(foreign);
      IDirect3DVertexBuffer9_Release(vb);
    }
    static const DWORD vs11[] = {0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x00000001, 0xC00F0000, 0x90E40000, 0x0000FFFF};      /* vs_1_1: mov oPos, v0 */
    IDirect3DVertexShader9 *vs = NULL;
    CHECK_HR(IDirect3DDevice9_CreateVertexShader(dev, vs11, &vs));
    if (vs) {
      IDirect3DVertexShader9 *proxy = wrap(vs, kShader9, 1), *foreign = wrap(vs, kShader9, 0);
      CHECK(IDirect3DDevice9_SetVertexShader(dev, proxy) == D3D_OK);
      CHECK(IDirect3DDevice9_SetVertexShader(dev, foreign) == D3DERR_INVALIDCALL);
      IDirect3DDevice9_SetVertexShader(dev, NULL);
      IDirect3DVertexShader9_Release(proxy);
      IDirect3DVertexShader9_Release(foreign);
      IDirect3DVertexShader9_Release(vs);
    }
    /* d3d9.dll's own extensions: its private calls only, on its own devices; a program's SetTextureStageState(33) is a type Direct3D 9 does not have */
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    typedef HRESULT(WINAPI * KeyKill)(IDirect3DDevice9 *, DWORD, BOOL);
    typedef HRESULT(WINAPI * Lockable)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **);
    KeyKill key_kill = (KeyKill)(void *)GetProcAddress(d3d9, "TcosD3D9SetStageKeyKill");
    Lockable lockable = (Lockable)(void *)GetProcAddress(d3d9, "TcosD3D9CreateLockableDepthStencil");
    CHECK(key_kill && lockable);
    IDirect3DDevice9 *foreign_dev = wrap(dev, kDevice9, 0), *proxy_dev = wrap(dev, kDevice9, 1);
    if (key_kill && lockable) {
      CHECK(key_kill(dev, 0, TRUE) == D3D_OK);
      CHECK(key_kill(proxy_dev, 0, FALSE) == D3D_OK);
      CHECK(key_kill(foreign_dev, 0, TRUE) == D3DERR_INVALIDCALL);
      IDirect3DSurface9 *ds = NULL;
      CHECK(lockable(foreign_dev, 16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds) == D3DERR_INVALIDCALL && !ds);
      CHECK(lockable(dev, 16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds) == D3D_OK && ds);
      if (ds) {
        D3DLOCKED_RECT lr;
        CHECK(SUCCEEDED(IDirect3DSurface9_LockRect(ds, &lr, NULL, 0)));      /* (what makes it ddraw.dll's: a D24S8 that locks) */
        IDirect3DSurface9_UnlockRect(ds);
        IDirect3DSurface9_Release(ds);
      }
    }
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(dev, 0, (D3DTEXTURESTAGESTATETYPE)33, TRUE));
    DWORD value = 7;
    CHECK(SUCCEEDED(IDirect3DDevice9_GetTextureStageState(dev, 0, (D3DTEXTURESTAGESTATETYPE)33, &value)) && value == 0);
    IDirect3DDevice9_Release(foreign_dev);
    IDirect3DDevice9_Release(proxy_dev);
    IDirect3DDevice9_Release(dev);
  }
  IDirect3D9Ex *d3dex = NULL;
  if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dex)) && d3dex) {
    IDirect3DDevice9Ex *ex = NULL;
    pp.hDeviceWindow = g_window;
    if (SUCCEEDED(IDirect3D9Ex_CreateDeviceEx(d3dex, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &ex)) && ex) {
      IDirect3DSurface9 *ds = NULL;
      CHECK(IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx(ex, 16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL, 0x40000000) == D3DERR_INVALIDCALL && !ds);
      CHECK_HR(IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx(ex, 16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL, 0));
      if (ds) IDirect3DSurface9_Release(ds);
      IDirect3DDevice9Ex_Release(ex);
    } else CHECK(0);
    IDirect3D9Ex_Release(d3dex);
  }
  IDirect3D9_Release(d3d);
}

/* ---- the Agility SDK ----------------------------------------------------------------------------------------------------------------------------------------------------- */

/* (D3D12GetInterface by name: mingw's 32-bit import library has no entry for it) */
static HRESULT get_interface(REFCLSID clsid, REFIID riid, void **out) {
  typedef HRESULT(WINAPI * Fn)(REFCLSID, REFIID, void **);
  Fn fn = (Fn)(void *)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12GetInterface");
  return fn ? fn(clsid, riid, out) : E_NOTIMPL;
}
static void test_agility(ID3D12Device *first) {
  ID3D12DeviceConfiguration *config = NULL;
  CHECK_HR(ID3D12Device_QueryInterface(first, &IID_ID3D12DeviceConfiguration, (void **)&config));
  if (config) {
    const D3D12_DEVICE_CONFIGURATION_DESC d = ID3D12DeviceConfiguration_GetDesc(config);
    CHECK(d.SDKVersion == 610 && d.Flags == 0 && d.NumEnabledExperimentalFeatures == 0);      /* (this program's D3D12SDKVersion export) */
    CHECK_HR(ID3D12DeviceConfiguration_GetEnabledExperimentalFeatures(config, NULL, 0));
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs;
    memset(&rs, 0, sizeof rs);
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    ID3DBlob *blob = NULL, *error = NULL;
    CHECK_HR(ID3D12DeviceConfiguration_SerializeVersionedRootSignature(config, &rs, &blob, &error));
    if (blob) {
      ID3D12VersionedRootSignatureDeserializer *des = NULL;
      CHECK_HR(ID3D12DeviceConfiguration_CreateVersionedRootSignatureDeserializer(config, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                                                    &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&des));
      if (des) ID3D12VersionedRootSignatureDeserializer_Release(des);
      ID3D10Blob_Release(blob);
    }
    ID3D12Device *back = NULL;
    CHECK(SUCCEEDED(ID3D12DeviceConfiguration_QueryInterface(config, &IID_ID3D12Device, (void **)&back)) && back == first);
    if (back) ID3D12Device_Release(back);
    ID3D12DeviceConfiguration_Release(config);
  }
  void *none = (void *)1;
  CHECK(get_interface(&CLSID_D3D12Debug, &IID_ID3D12Debug, &none) == DXGI_ERROR_SDK_COMPONENT_MISSING && !none);
  ID3D12SDKConfiguration1 *sdk = NULL;
  CHECK_HR(get_interface(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&sdk));
  if (sdk) {
    CHECK(ID3D12SDKConfiguration1_SetSDKVersion(sdk, 0, ".\\D3D12\\") == E_INVALIDARG);
    CHECK(ID3D12SDKConfiguration1_SetSDKVersion(sdk, 610, NULL) == E_INVALIDARG);
    CHECK(ID3D12SDKConfiguration1_SetSDKVersion(sdk, 9999, ".\\D3D12\\") == (HRESULT)0x887E0003);      /* (newer than this system's core: D3D12_ERROR_INVALID_REDIST) */
    CHECK(ID3D12SDKConfiguration1_SetSDKVersion(sdk, 608, ".\\D3D12\\") == DXGI_ERROR_INVALID_CALL);      /* (a device lives already, of 610) */
    CHECK_HR(ID3D12SDKConfiguration1_SetSDKVersion(sdk, 610, ".\\D3D12\\"));
    ID3D12DeviceFactory *factory = NULL;
    CHECK(ID3D12SDKConfiguration1_CreateDeviceFactory(sdk, 9999, ".\\D3D12\\", &IID_ID3D12DeviceFactory, (void **)&factory) == (HRESULT)0x887E0003 && !factory);
    CHECK_HR(ID3D12SDKConfiguration1_CreateDeviceFactory(sdk, 608, ".\\D3D12\\", &IID_ID3D12DeviceFactory, (void **)&factory));
    if (factory) {
      CHECK(ID3D12DeviceFactory_GetFlags(factory) == D3D12_DEVICE_FACTORY_FLAG_NONE);
      CHECK(ID3D12DeviceFactory_SetFlags(factory, (D3D12_DEVICE_FACTORY_FLAGS)0x8) == E_INVALIDARG);
      CHECK(ID3D12DeviceFactory_EnableExperimentalFeatures(factory, 0, NULL, NULL, NULL) == S_OK);
      void *debug = (void *)1;
      CHECK(ID3D12DeviceFactory_GetConfigurationInterface(factory, &CLSID_D3D12Debug, &IID_ID3D12Debug, &debug) == DXGI_ERROR_SDK_COMPONENT_MISSING && !debug);
      ID3D12Device *dev = NULL;
      /* (the adapter has its singleton - the first device -: a second needs the factory's leave) */
      CHECK(ID3D12DeviceFactory_CreateDevice(factory, NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev) == DXGI_ERROR_ALREADY_EXISTS && !dev);
      CHECK_HR(ID3D12DeviceFactory_SetFlags(factory, D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE));
      CHECK(ID3D12DeviceFactory_CreateDevice(factory, NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev) == DXGI_ERROR_ALREADY_EXISTS && !dev);      /* (608 is not 610) */
      CHECK_HR(ID3D12DeviceFactory_SetFlags(factory, D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE));
      CHECK(SUCCEEDED(ID3D12DeviceFactory_CreateDevice(factory, NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)) && dev == first);
      if (dev) { ID3D12Device_Release(dev); dev = NULL; }
      CHECK_HR(ID3D12DeviceFactory_SetFlags(factory, D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON));
      CHECK_HR(ID3D12DeviceFactory_CreateDevice(factory, NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
      if (dev) {
        CHECK(dev != first);
        ID3D12DeviceConfiguration *c = NULL;
        CHECK_HR(ID3D12Device_QueryInterface(dev, &IID_ID3D12DeviceConfiguration, (void **)&c));
        if (c) {
          CHECK(ID3D12DeviceConfiguration_GetDesc(c).SDKVersion == 608);
          ID3D12DeviceConfiguration_Release(c);
        }
        /* (a device that draws: a clear read back) */
        ID3D12CommandQueue *queue = NULL;
        const D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        CHECK_HR(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
        Run12 r;
        ID3D12Resource *t = queue ? texture12(dev, 8, 8, D3D12_RESOURCE_STATE_PRESENT) : NULL;
        if (t && run12_begin(&r, dev, queue)) {
          const float orange[4] = {1, 0.5f, 0, 1};
          clear12(&r, t, orange);
          const UINT32 px = texture12_pixel(&r, t, D3D12_RESOURCE_STATE_PRESENT);
          CHECK((px & 0xFFFF00FFu) == 0xFFFF0000u && ((px >> 8) & 0xFF) >= 0x7F && ((px >> 8) & 0xFF) <= 0x80);
          run12_free(&r);
        }
        if (t) ID3D12Resource_Release(t);
        if (queue) ID3D12CommandQueue_Release(queue);
        ID3D12Device_Release(dev);
      }
      CHECK_HR(ID3D12DeviceFactory_InitializeFromGlobalState(factory));
      CHECK_HR(ID3D12DeviceFactory_ApplyToGlobalState(factory));      /* (610: what the process has) */
      ID3D12DeviceFactory_Release(factory);
    }
    ID3D12SDKConfiguration1_Release(sdk);
  }
  ID3D12DeviceFactory *global = NULL;
  CHECK_HR(get_interface(&CLSID_D3D12DeviceFactory_, &IID_ID3D12DeviceFactory, (void **)&global));
  if (global) {
    ID3D12Device *dev = NULL;
    CHECK_HR(ID3D12DeviceFactory_SetFlags(global, D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE));
    CHECK(SUCCEEDED(ID3D12DeviceFactory_CreateDevice(global, NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)) && dev == first);      /* (610 = 610) */
    if (dev) ID3D12Device_Release(dev);
    ID3D12DeviceFactory_Release(global);
  }
}

/* ---- Vulkan: the LUID of the device is its DXGI adapter's ---------------------------------------------------------------------------------------------------------------- */

typedef struct VkAppInfo_ { int sType; const void *pNext; const char *app; UINT32 appv; const char *engine; UINT32 enginev; UINT32 api; } VkAppInfo_;
typedef struct VkInstInfo_ { int sType; const void *pNext; UINT32 flags; const VkAppInfo_ *app; UINT32 layers; const char *const *layer_names; UINT32 exts; const char *const *ext_names; } VkInstInfo_;
typedef struct VkIdProps_ { int sType; void *pNext; BYTE device_uuid[16], driver_uuid[16], luid[8]; UINT32 node_mask; UINT32 luid_valid; } VkIdProps_;
typedef struct VkProps2_ { int sType; void *pNext; __attribute__((aligned(8))) BYTE properties[1024]; } VkProps2_;
#ifdef _WIN64
#define VKCALL
#else
#define VKCALL __stdcall
#endif
static void test_vulkan_luid(void) {
  HMODULE vk = LoadLibraryA("vulkan-1.dll");
  if (!vk) { printf("vulkan: no vulkan-1.dll\n"); return; }
  typedef int(VKCALL * CreateInstance)(const VkInstInfo_ *, const void *, void **);
  typedef void(VKCALL * DestroyInstance)(void *, const void *);
  typedef void *(VKCALL * GetProc)(void *, const char *);
  typedef int(VKCALL * Enumerate)(void *, UINT32 *, void **);
  typedef void(VKCALL * Props)(void *, void *);
  CreateInstance create = (CreateInstance)(void *)GetProcAddress(vk, "vkCreateInstance");
  GetProc proc = (GetProc)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr");
  if (!create || !proc) { FreeLibrary(vk); return; }
  const VkAppInfo_ app = {0, NULL, "d3dproxytest", 1, NULL, 0, (1u << 22) | (1u << 12)};      /* (Vulkan 1.1) */
  const VkInstInfo_ ci = {1, NULL, 0, &app, 0, NULL, 0, NULL};
  void *inst = NULL;
  if (create(&ci, NULL, &inst) != 0 || !inst) { printf("vulkan: no instance of 1.1 (the Direct3D backend is Vulkan 1.0: no deviceLUID)\n"); FreeLibrary(vk); return; }
  Enumerate enumerate = (Enumerate)proc(inst, "vkEnumeratePhysicalDevices");
  Props props = (Props)proc(inst, "vkGetPhysicalDeviceProperties");
  Props props2 = (Props)proc(inst, "vkGetPhysicalDeviceProperties2");
  DestroyInstance destroy = (DestroyInstance)proc(inst, "vkDestroyInstance");
  void *pd[4] = {0};
  UINT32 n = 4;
  if (enumerate && props && props2 && enumerate(inst, &n, pd) >= 0 && n) {
    static VkProps2_ p;
    memset(&p, 0, sizeof p);
    props(pd[0], p.properties);
    const UINT32 api = *(const UINT32 *)p.properties;
    if (api >= ((1u << 22) | (1u << 12))) {
      VkIdProps_ id;
      memset(&id, 0, sizeof id);
      id.sType = 1000071004;      /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES */
      p.sType = 1000059001;       /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 */
      p.pNext = &id;
      props2(pd[0], &p);
      CHECK(id.luid_valid);
      int found = 0;
      IDXGIAdapter1 *a = NULL;
      for (UINT i = 0; SUCCEEDED(IDXGIFactory2_EnumAdapters1(g_factory, i, &a)); ++i) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        if (memcmp(&d.AdapterLuid, id.luid, 8) == 0) found = 1;
        IDXGIAdapter1_Release(a);
      }
      printf("vulkan: deviceLUID %s a DXGI adapter's\n", found ? "is" : "is NOT");
      CHECK(found);
    } else {
      printf("vulkan: the device is Vulkan 1.0 (the Direct3D backend): no deviceLUID to compare\n");
    }
  }
  if (destroy) destroy(inst, NULL);
  FreeLibrary(vk);
}

/* (a fault in a wrapper's thunk: which wrapper, so that a reference counted wrongly shows) */
static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
#ifdef _WIN64
  const Wrap *w = (const Wrap *)e->ContextRecord->Rcx;
  printf("d3dproxytest: fault at %p, rcx %p (its vtable %p, ours %p), return %p\n", e->ExceptionRecord->ExceptionAddress, (void *)w,
         w ? (const void *)w->lpVtbl : NULL, (void *)g_vtbl, *(void **)e->ContextRecord->Rsp);
#else
  printf("d3dproxytest: fault at %p, return %p\n", e->ExceptionRecord->ExceptionAddress, *(void **)e->ContextRecord->Esp);
#endif
  return EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  AddVectoredExceptionHandler(1, on_fault);
  printf("d3dproxytest: modules dxgi %p d3d11 %p d3d12 %p exe %p\n", (void *)LoadLibraryA("dxgi.dll"), (void *)LoadLibraryA("d3d11.dll"), (void *)LoadLibraryA("d3d12.dll"), (void *)GetModuleHandleA(NULL));
  CHECK(wrappers_init());
  g_window = make_window(L"d3dproxytest");
  CHECK(g_window != NULL);
  CHECK_HR(CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&g_factory));
  ID3D12Device *dev = NULL;
  CHECK_HR(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
  ID3D12CommandQueue *queue = NULL;
  if (dev) {
    const D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    CHECK_HR(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
  }
  if (dev && queue && g_factory && g_window) {
    printf("d3dproxytest: identity\n");
    test_identity(dev);
    printf("d3dproxytest: 11 on 12\n");
    test_11on12(dev, queue);
    printf("d3dproxytest: swap chains (11)\n");
    test_swap_chains_11();
    printf("d3dproxytest: swap chains (10)\n");
    test_swap_chain_10();
    printf("d3dproxytest: swap chains (12)\n");
    test_swap_chains_12(dev, queue);
    printf("d3dproxytest: Direct3D 9\n");
    test_d3d9();
    printf("d3dproxytest: Agility SDK\n");
    test_agility(dev);
    printf("d3dproxytest: Vulkan\n");
    test_vulkan_luid();
  }
  if (queue) ID3D12CommandQueue_Release(queue);
  if (dev) ID3D12Device_Release(dev);
  if (g_factory) IDXGIFactory2_Release(g_factory);
  if (g_failed == 0) {
    printf("d3dproxytest: all %d checks passed\n", g_checks);
    return 42;
  }
  printf("d3dproxytest: %d checks, %d failed\n", g_checks, g_failed);
  return 1;
}
