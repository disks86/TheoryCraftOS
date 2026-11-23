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

/* Part of d3d11.c: devices made on a program's own Direct3D 12 device (D3D11On12CreateDevice, ID3D11On12Device1), D3D11CoreCreateDevice, and the Windows Runtime's
 * face of a device and a surface (CreateDirect3D11DeviceFromDXGIDevice, CreateDirect3D11SurfaceFromDXGISurface: IDirect3DDevice, IDirect3DSurface, IClosable and
 * IDirect3DDxgiInterfaceAccess). */

/* ---- 11 on 12 -------------------------------------------------------------------------------------------------------------------------------------------------------- */

static Dev *dev_of_on12(ID3D11On12Device1 *p) { return (Dev *)((BYTE *)p - offsetof(Dev, on12)); }
static HRESULT STDMETHODCALLTYPE on12_QueryInterface(ID3D11On12Device1 *p, REFIID riid, void **out) { return dev_QueryInterface(dev_of_on12(p), riid, out); }
static ULONG STDMETHODCALLTYPE on12_AddRef(ID3D11On12Device1 *p) { return dx_AddRef(OBJ(dev_of_on12(p))); }
static ULONG STDMETHODCALLTYPE on12_Release(ID3D11On12Device1 *p) { return dx_Release(OBJ(dev_of_on12(p))); }

/* A Direct3D 12 resource of the device as a Direct3D 11 one: a buffer, or a texture of the kind its dimension says, with the bind, misc and CPU access flags given.
 * A texture's format must be one the kernel keeps as it is (formats.inc): a texture of a format kept wider has no Direct3D 12 resource of its own format. */
static HRESULT STDMETHODCALLTYPE on12_CreateWrappedResource(ID3D11On12Device1 *p, IUnknown *resource12, const D3D11_RESOURCE_FLAGS *flags, D3D12_RESOURCE_STATES in_state,
                                                            D3D12_RESOURCE_STATES out_state, REFIID riid, void **out) {
  extern const ID3D11Texture3DVtbl tex3_vtbl;
  extern const ID3D11Texture1DVtbl tex1d_vtbl;
  (void)in_state; (void)out_state;      /* (the kernel's allocations are not state tracked: there is no transition to make) */
  Dev *d = dev_of_on12(p);
  if (out) *out = NULL;
  if (!resource12 || !flags || !out) return E_INVALIDARG;
  ID3D12Resource *res = NULL;
  if (FAILED(IUnknown_QueryInterface(resource12, &IID_ID3D12Resource, (void **)&res)) || !res) return E_INVALIDARG;
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(res);
  D3D12_HEAP_PROPERTIES heap;
  D3D12_HEAP_FLAGS heap_flags;
  memset(&heap, 0, sizeof heap);
  ID3D12Resource_GetHeapProperties(res, &heap, &heap_flags);
  HRESULT hr = E_INVALIDARG;
  DxObj *made = NULL;
  if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (rd.Width == 0 || rd.Width > (256u << 20)) { ID3D12Resource_Release(res); return E_INVALIDARG; }
    Buf *b = child_new(sizeof *b, d, &buf_vtbl, g_buf_iids, buf_destroy);
    if (!b) { ID3D12Resource_Release(res); return E_OUTOFMEMORY; }
    b->desc.ByteWidth = (UINT)rd.Width;
    b->desc.Usage = heap.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    b->desc.BindFlags = flags->BindFlags;
    b->desc.CPUAccessFlags = flags->CPUAccessFlags;
    b->desc.MiscFlags = flags->MiscFlags;
    b->desc.StructureByteStride = flags->StructureByteStride;
    b->res = res;
    b->bytes = rd.Width;
    b->version = 1;
    if (heap.Type == D3D12_HEAP_TYPE_UPLOAD) {
      /* (upload memory: the CPU's copy, as with this device's own buffers) */
      if (FAILED(ID3D12Resource_Map(res, 0, NULL, (void **)&b->data)) || !b->data) { b->res = NULL; ID3D12Resource_Release(res); dx_Release(OBJ(b)); return E_INVALIDARG; }
      ID3D12Resource_Unmap(res, 0, NULL);
      b->uav = (flags->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    } else {
      b->gpu = 1;
    }
    made = OBJ(b);
    hr = S_OK;
  } else {
    const D3D11_TEXTURE2D_DESC desc = {(UINT)rd.Width, rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? rd.Height : rd.Height, rd.MipLevels,
                                       rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : rd.DepthOrArraySize, rd.Format, rd.SampleDesc, D3D11_USAGE_DEFAULT,
                                       flags->BindFlags, flags->CPUAccessFlags, flags->MiscFlags};
    const TexFormat *f = tex_format_for(rd.Format, flags->BindFlags);
    Tex *t = NULL;
    if (f && f->stored == rd.Format) {
      EnterCriticalSection(&d->lock);
      hr = tex_make(d, &desc, rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? rd.DepthOrArraySize : 0, res, &t);
      LeaveCriticalSection(&d->lock);
    }
    if (SUCCEEDED(hr) && t) {
      if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        static const GUID *const iids3[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture3D, NULL};
        t->c.o.lpVtbl = (const void *)&tex3_vtbl;
        t->c.o.iids = iids3;
        const D3D11_TEXTURE3D_DESC d3 = {desc.Width, desc.Height, rd.DepthOrArraySize, t->desc.MipLevels, desc.Format, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags};
        t->desc3 = d3;
      } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        static const GUID *const iids1[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture1D, NULL};
        t->c.o.lpVtbl = (const void *)&tex1d_vtbl;
        t->c.o.iids = iids1;
        t->one_d = TRUE;
        const D3D11_TEXTURE1D_DESC d1 = {desc.Width, t->desc.MipLevels, desc.ArraySize, desc.Format, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags};
        t->desc1 = d1;
      }
      made = OBJ(t);
    }
    ID3D12Resource_Release(res);      /* (the texture took its own reference) */
  }
  if (FAILED(hr) || !made) return FAILED(hr) ? hr : E_INVALIDARG;
  hr = IUnknown_QueryInterface((IUnknown *)made, riid, out);
  dx_Release(made);
  return hr;
}
/* ---- shared resources -------------------------------------------------------------------------------------------------------------------------------------- */
/* A texture made D3D11_RESOURCE_MISC_SHARED_NTHANDLE is a Direct3D 12 resource made D3D12_HEAP_FLAG_SHARED (tex_make), its description carried along
 * (tex_share_blob); IDXGIResource1::CreateSharedHandle makes the NT handle (dxgi_resource.inc) and OpenSharedResource1 opens it as a texture of this device over the
 * same memory. A resource Direct3D 12 shared (no description of ours) is opened as the texture its Direct3D 12 description says, bound as its flags allow. The legacy
 * shared handles of IDXGIResource::GetSharedHandle are not made (DXGI_ERROR_UNSUPPORTED there), so OpenSharedResource takes none. */
static void tex_take_kind(Tex *t, UINT kind, UINT depth) {
  extern const ID3D11Texture3DVtbl tex3_vtbl;
  extern const ID3D11Texture1DVtbl tex1d_vtbl;
  static const GUID *const iids3[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture3D, NULL};
  static const GUID *const iids1[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture1D, NULL};
  if (kind == 1) {
    t->c.o.lpVtbl = (const void *)&tex3_vtbl;
    t->c.o.iids = iids3;
    const D3D11_TEXTURE3D_DESC d3 = {t->desc.Width, t->desc.Height, depth, t->desc.MipLevels, t->desc.Format, t->desc.Usage, t->desc.BindFlags, t->desc.CPUAccessFlags, t->desc.MiscFlags};
    t->desc3 = d3;
  } else if (kind == 2) {
    t->c.o.lpVtbl = (const void *)&tex1d_vtbl;
    t->c.o.iids = iids1;
    t->one_d = TRUE;
    const D3D11_TEXTURE1D_DESC d1 = {t->desc.Width, t->desc.MipLevels, t->desc.ArraySize, t->desc.Format, t->desc.Usage, t->desc.BindFlags, t->desc.CPUAccessFlags, t->desc.MiscFlags};
    t->desc1 = d1;
  }
}
static HRESULT open_shared_texture(Dev *d, ID3D12Resource *res, REFIID riid, void **out) {
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(res);
  if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return E_INVALIDARG;      /* (Direct3D 11 shares textures) */
  BYTE blob[TEX_SHARE_BYTES];
  D3D11_TEXTURE2D_DESC desc;
  UINT kind = 0, slices = 0;
  const UINT size = TcosD3D12SharedExtra(res, blob, sizeof blob);
  if (!tex_share_read(blob, size, &desc, &kind, &slices)) {
    /* (Direct3D 12's: the texture its description says, bound as its flags allow) */
    kind = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 2 : 0;
    slices = kind == 1 ? rd.DepthOrArraySize : 0;
    UINT bind = (rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ? 0u : (UINT)D3D11_BIND_SHADER_RESOURCE;
    if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) bind |= D3D11_BIND_RENDER_TARGET;
    if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) bind = (bind & ~(UINT)D3D11_BIND_SHADER_RESOURCE) | D3D11_BIND_DEPTH_STENCIL;
    if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) bind |= D3D11_BIND_UNORDERED_ACCESS;
    const D3D11_TEXTURE2D_DESC from12 = {(UINT)rd.Width, rd.Height, rd.MipLevels, kind == 1 ? 1u : rd.DepthOrArraySize, rd.Format, rd.SampleDesc, D3D11_USAGE_DEFAULT,
                                         bind, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
    desc = from12;
  }
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_make(d, &desc, slices, res, &t);
  if (SUCCEEDED(hr)) tex_take_kind(t, kind, slices);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) return hr;
  hr = IUnknown_QueryInterface((IUnknown *)t, riid, out);
  dx_Release(OBJ(t));
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_OpenSharedResource1(Dev *d, HANDLE handle, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!handle || !riid || !out) return E_INVALIDARG;
  ID3D12Resource *res = NULL;
  HRESULT hr = ID3D12Device_OpenSharedHandle(d->d12, handle, &IID_ID3D12Resource, (void **)&res);
  if (FAILED(hr) || !res) return hr == E_NOINTERFACE || SUCCEEDED(hr) ? E_INVALIDARG : hr;      /* (a fence's or a heap's handle: no resource) */
  hr = open_shared_texture(d, res, riid, out);
  ID3D12Resource_Release(res);
  return hr;
}
/* (the legacy handles of GetSharedHandle: none is made here) */
static HRESULT STDMETHODCALLTYPE dev_OpenSharedResource(Dev *d, HANDLE handle, REFIID riid, void **out) {
  (void)d; (void)handle; (void)riid;
  if (out) *out = NULL;
  return E_INVALIDARG;
}
/* DXGI_SHARED_RESOURCE_READ and _WRITE: what a handle may be asked for. */
#define SHARED_ACCESS_ALL (0x80000000u | 0x1u)
static HRESULT STDMETHODCALLTYPE dev_OpenSharedResourceByName(Dev *d, LPCWSTR name, DWORD access, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!name || !riid || !out || !access || (access & ~SHARED_ACCESS_ALL)) return E_INVALIDARG;
  HANDLE handle = NULL;
  HRESULT hr = ID3D12Device_OpenSharedHandleByName(d->d12, name, GENERIC_ALL, &handle);
  if (FAILED(hr)) return hr;
  hr = dev_OpenSharedResource1(d, handle, riid, out);
  CloseHandle(handle);
  return hr;
}

/* Before Direct3D 12 uses what the device wrote into wrapped resources: what was recorded is sent (the queue runs it before it returns). */
static void STDMETHODCALLTYPE on12_ReleaseWrappedResources(ID3D11On12Device1 *p, ID3D11Resource *const *resources, UINT count) {
  Dev *d = dev_of_on12(p);
  (void)resources; (void)count;
  EnterCriticalSection(&d->lock);
  flush(d);
  LeaveCriticalSection(&d->lock);
}
/* (Direct3D 12's work on the queue has run when its ExecuteCommandLists returned: nothing to wait for) */
static void STDMETHODCALLTYPE on12_AcquireWrappedResources(ID3D11On12Device1 *p, ID3D11Resource *const *resources, UINT count) { (void)p; (void)resources; (void)count; }
static HRESULT STDMETHODCALLTYPE on12_GetD3D12Device(ID3D11On12Device1 *p, REFIID riid, ID3D12Device **out) {
  if (!out) return E_POINTER;
  return ID3D12Device_QueryInterface(dev_of_on12(p)->d12, riid, (void **)out);
}
static const ID3D11On12Device1Vtbl g_on12_vtbl = {on12_QueryInterface, on12_AddRef, on12_Release, on12_CreateWrappedResource, on12_ReleaseWrappedResources,
                                                  on12_AcquireWrappedResources, on12_GetD3D12Device};

/* A Direct3D 11 device on the program's Direct3D 12 device and its first queue (the others are not used: one queue runs everything here). */
EXPORT HRESULT WINAPI D3D11On12CreateDevice(IUnknown *device12, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, IUnknown *const *queues, UINT queue_count, UINT node_mask,
                                            ID3D11Device **device, ID3D11DeviceContext **context, D3D_FEATURE_LEVEL *feature_level) {
  if (device) *device = NULL;
  if (context) *context = NULL;
  if (feature_level) *feature_level = 0;
  if (!device12 || !queues || !queue_count || node_mask > 1) return E_INVALIDARG;
  if (flags & D3D11_CREATE_DEVICE_DEBUG) return DXGI_ERROR_SDK_COMPONENT_MISSING;
  ID3D12Device *d12 = NULL;
  ID3D12CommandQueue *queue = NULL;
  if (FAILED(IUnknown_QueryInterface(device12, &IID_ID3D12Device, (void **)&d12)) || !d12) return E_INVALIDARG;
  if (!queues[0] || FAILED(IUnknown_QueryInterface(queues[0], &IID_ID3D12CommandQueue, (void **)&queue)) || !queue) { ID3D12Device_Release(d12); return E_INVALIDARG; }
  Dev *d = NULL;
  const HRESULT hr = dev_create_on(NULL, flags, levels, level_count, d12, queue, &d);
  ID3D12CommandQueue_Release(queue);
  ID3D12Device_Release(d12);
  if (FAILED(hr)) return hr;
  if (feature_level) *feature_level = d->level;
  if (context) { ctx_AddRef(d->ctx); *context = (ID3D11DeviceContext *)d->ctx; }
  if (device) *device = (ID3D11Device *)d;
  else dx_Release(OBJ(d));
  return S_OK;
}

/* What d3d10core.dll and the layers make devices with: a device on the adapter (or the factory's first). */
EXPORT HRESULT WINAPI D3D11CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, ID3D11Device **device) {
  IDXGIAdapter *chosen = adapter;
  if (device) *device = NULL;
  if (!chosen && factory && FAILED(IDXGIFactory_EnumAdapters(factory, 0, &chosen))) return DXGI_ERROR_UNSUPPORTED;
  else if (chosen == adapter && chosen) IDXGIAdapter_AddRef(chosen);
  if (flags & D3D11_CREATE_DEVICE_DEBUG) { if (chosen) IDXGIAdapter_Release(chosen); return DXGI_ERROR_SDK_COMPONENT_MISSING; }
  Dev *d = NULL;
  const HRESULT hr = dev_create(chosen, flags, levels, level_count, &d);
  if (chosen) IDXGIAdapter_Release(chosen);
  if (FAILED(hr)) return hr;
  if (device) *device = (ID3D11Device *)d;
  else dx_Release(OBJ(d));
  return S_OK;
}

/* ---- the Windows Runtime's face -------------------------------------------------------------------------------------------------------------------------------- */

DEFINE_GUID(IID_TcosIInspectable, 0xaf86e2e0, 0xb12d, 0x4c6a, 0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90);
DEFINE_GUID(IID_TcosIDirect3DDevice, 0xa37624ab, 0x8d5f, 0x4650, 0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70);
DEFINE_GUID(IID_TcosIDirect3DSurface, 0x0bf4a146, 0x13c1, 0x4694, 0xbe, 0xe3, 0x7a, 0xbf, 0x15, 0xea, 0xf5, 0x86);
DEFINE_GUID(IID_TcosIClosable, 0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e);
DEFINE_GUID(IID_TcosIDirect3DDxgiInterfaceAccess, 0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1);
#define RO_E_CLOSED_ ((HRESULT)0x80000013)

/* (the faces: IDirect3DDevice / IDirect3DSurface (IInspectable, then Trim / get_Description), IClosable (IInspectable, then Close), IDirect3DDxgiInterfaceAccess
 * (IUnknown, then GetInterface)) */
typedef struct WinRtFace { const void *const *lpVtbl; } WinRtFace;
typedef struct SurfaceDescription { INT32 Width, Height; INT32 Format; struct { INT32 Count, Quality; } MultisampleDescription; } SurfaceDescription;
typedef struct WinRtObj {
  WinRtFace main, closable, access;
  LONG ref;
  BOOL surface;                /* IDirect3DSurface (else IDirect3DDevice) */
  IUnknown *inner;             /* the IDXGIDevice or IDXGISurface (NULL once closed) */
  CRITICAL_SECTION lock;
} WinRtObj;
static WinRtObj *winrt_of(void *face, UINT which) { return (WinRtObj *)((BYTE *)face - (which == 0 ? offsetof(WinRtObj, main) : which == 1 ? offsetof(WinRtObj, closable) : offsetof(WinRtObj, access))); }
static HRESULT winrt_qi(WinRtObj *o, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_TcosIInspectable) || guid_eq(riid, o->surface ? &IID_TcosIDirect3DSurface : &IID_TcosIDirect3DDevice)) *out = &o->main;
  else if (guid_eq(riid, &IID_TcosIClosable)) *out = &o->closable;
  else if (guid_eq(riid, &IID_TcosIDirect3DDxgiInterfaceAccess)) *out = &o->access;
  else return E_NOINTERFACE;
  InterlockedIncrement(&o->ref);
  return S_OK;
}
static ULONG winrt_release(WinRtObj *o) {
  const LONG n = InterlockedDecrement(&o->ref);
  if (n == 0) {
    if (o->inner) IUnknown_Release(o->inner);
    DeleteCriticalSection(&o->lock);
    mem_free(o);
  }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE wr_main_QueryInterface(void *f, REFIID riid, void **out) { return winrt_qi(winrt_of(f, 0), riid, out); }
static ULONG STDMETHODCALLTYPE wr_main_AddRef(void *f) { return (ULONG)InterlockedIncrement(&winrt_of(f, 0)->ref); }
static ULONG STDMETHODCALLTYPE wr_main_Release(void *f) { return winrt_release(winrt_of(f, 0)); }
static HRESULT STDMETHODCALLTYPE wr_close_QueryInterface(void *f, REFIID riid, void **out) { return winrt_qi(winrt_of(f, 1), riid, out); }
static ULONG STDMETHODCALLTYPE wr_close_AddRef(void *f) { return (ULONG)InterlockedIncrement(&winrt_of(f, 1)->ref); }
static ULONG STDMETHODCALLTYPE wr_close_Release(void *f) { return winrt_release(winrt_of(f, 1)); }
static HRESULT STDMETHODCALLTYPE wr_access_QueryInterface(void *f, REFIID riid, void **out) { return winrt_qi(winrt_of(f, 2), riid, out); }
static ULONG STDMETHODCALLTYPE wr_access_AddRef(void *f) { return (ULONG)InterlockedIncrement(&winrt_of(f, 2)->ref); }
static ULONG STDMETHODCALLTYPE wr_access_Release(void *f) { return winrt_release(winrt_of(f, 2)); }

/* IInspectable: the interfaces (in CoTaskMemAlloc memory), the class name (an HSTRING of combase.dll's), the trust level (base trust). */
static HRESULT winrt_iids(WinRtObj *o, ULONG *count, IID **iids) {
  typedef void *(WINAPI *TaskAlloc)(SIZE_T);
  if (!count || !iids) return E_INVALIDARG;
  *count = 0;
  *iids = NULL;
  HMODULE combase = LoadLibraryW(L"combase.dll");
  TaskAlloc alloc = combase ? (TaskAlloc)(void (*)(void))GetProcAddress(combase, "CoTaskMemAlloc") : NULL;
  IID *list = alloc ? alloc(2 * sizeof(IID)) : NULL;
  if (!list) return E_OUTOFMEMORY;
  list[0] = o->surface ? IID_TcosIDirect3DSurface : IID_TcosIDirect3DDevice;
  list[1] = IID_TcosIClosable;
  *count = 2;
  *iids = list;
  return S_OK;
}
static HRESULT winrt_class_name(WinRtObj *o, void **name) {
  typedef HRESULT (WINAPI *CreateString)(const WCHAR *, UINT32, void **);
  static const WCHAR device_name[] = L"Windows.Graphics.DirectX.Direct3D11.IDirect3DDevice", surface_name[] = L"Windows.Graphics.DirectX.Direct3D11.IDirect3DSurface";
  if (!name) return E_INVALIDARG;
  *name = NULL;
  HMODULE combase = LoadLibraryW(L"combase.dll");
  CreateString create = combase ? (CreateString)(void (*)(void))GetProcAddress(combase, "WindowsCreateString") : NULL;
  if (!create) return E_NOTIMPL;
  const WCHAR *text = o->surface ? surface_name : device_name;
  return create(text, (UINT32)wlen(text), name);
}
static HRESULT STDMETHODCALLTYPE wr_main_GetIids(void *f, ULONG *count, IID **iids) { return winrt_iids(winrt_of(f, 0), count, iids); }
static HRESULT STDMETHODCALLTYPE wr_main_GetRuntimeClassName(void *f, void **name) { return winrt_class_name(winrt_of(f, 0), name); }
static HRESULT STDMETHODCALLTYPE wr_main_GetTrustLevel(void *f, INT *level) { (void)f; if (!level) return E_INVALIDARG; *level = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE wr_close_GetIids(void *f, ULONG *count, IID **iids) { return winrt_iids(winrt_of(f, 1), count, iids); }
static HRESULT STDMETHODCALLTYPE wr_close_GetRuntimeClassName(void *f, void **name) { return winrt_class_name(winrt_of(f, 1), name); }
static HRESULT STDMETHODCALLTYPE wr_close_GetTrustLevel(void *f, INT *level) { (void)f; if (!level) return E_INVALIDARG; *level = 0; return S_OK; }

/* IDirect3DDevice::Trim: IDXGIDevice3::Trim of the device. IDirect3DSurface::get_Description: the surface's size, format and samples. */
static HRESULT STDMETHODCALLTYPE wr_device_Trim(void *f) {
  WinRtObj *o = winrt_of(f, 0);
  IDXGIDevice3 *d3 = NULL;
  EnterCriticalSection(&o->lock);
  if (!o->inner) { LeaveCriticalSection(&o->lock); return RO_E_CLOSED_; }
  if (SUCCEEDED(IUnknown_QueryInterface(o->inner, &IID_IDXGIDevice3, (void **)&d3)) && d3) { IDXGIDevice3_Trim(d3); IDXGIDevice3_Release(d3); }
  LeaveCriticalSection(&o->lock);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE wr_surface_get_Description(void *f, SurfaceDescription *out) {
  WinRtObj *o = winrt_of(f, 0);
  if (!out) return E_POINTER;
  EnterCriticalSection(&o->lock);
  if (!o->inner) { LeaveCriticalSection(&o->lock); return RO_E_CLOSED_; }
  DXGI_SURFACE_DESC desc;
  const HRESULT hr = IDXGISurface_GetDesc((IDXGISurface *)o->inner, &desc);
  LeaveCriticalSection(&o->lock);
  if (FAILED(hr)) return hr;
  out->Width = (INT32)desc.Width;
  out->Height = (INT32)desc.Height;
  out->Format = (INT32)desc.Format;      /* (DirectXPixelFormat has DXGI_FORMAT's numbers) */
  out->MultisampleDescription.Count = (INT32)desc.SampleDesc.Count;
  out->MultisampleDescription.Quality = (INT32)desc.SampleDesc.Quality;
  return S_OK;
}
/* IClosable::Close: the device or surface is let go; what is asked of it after answers RO_E_CLOSED. */
static HRESULT STDMETHODCALLTYPE wr_close_Close(void *f) {
  WinRtObj *o = winrt_of(f, 1);
  EnterCriticalSection(&o->lock);
  IUnknown *inner = o->inner;
  o->inner = NULL;
  LeaveCriticalSection(&o->lock);
  if (inner) IUnknown_Release(inner);
  return S_OK;
}
/* IDirect3DDxgiInterfaceAccess::GetInterface: the DXGI object (or another of its interfaces: the ID3D11Device, the ID3D11Texture2D). */
static HRESULT STDMETHODCALLTYPE wr_access_GetInterface(void *f, REFIID riid, void **out) {
  WinRtObj *o = winrt_of(f, 2);
  if (!out) return E_POINTER;
  *out = NULL;
  EnterCriticalSection(&o->lock);
  const HRESULT hr = o->inner ? IUnknown_QueryInterface(o->inner, riid, out) : RO_E_CLOSED_;
  LeaveCriticalSection(&o->lock);
  return hr;
}
static const void *const g_wr_device_vtbl[] = {(const void *)wr_main_QueryInterface, (const void *)wr_main_AddRef, (const void *)wr_main_Release, (const void *)wr_main_GetIids,
                                              (const void *)wr_main_GetRuntimeClassName, (const void *)wr_main_GetTrustLevel, (const void *)wr_device_Trim};
static const void *const g_wr_surface_vtbl[] = {(const void *)wr_main_QueryInterface, (const void *)wr_main_AddRef, (const void *)wr_main_Release, (const void *)wr_main_GetIids,
                                               (const void *)wr_main_GetRuntimeClassName, (const void *)wr_main_GetTrustLevel, (const void *)wr_surface_get_Description};
static const void *const g_wr_close_vtbl[] = {(const void *)wr_close_QueryInterface, (const void *)wr_close_AddRef, (const void *)wr_close_Release, (const void *)wr_close_GetIids,
                                             (const void *)wr_close_GetRuntimeClassName, (const void *)wr_close_GetTrustLevel, (const void *)wr_close_Close};
static const void *const g_wr_access_vtbl[] = {(const void *)wr_access_QueryInterface, (const void *)wr_access_AddRef, (const void *)wr_access_Release, (const void *)wr_access_GetInterface};

static HRESULT winrt_make(IUnknown *inner, BOOL surface, IUnknown **out) {
  WinRtObj *o = mem_alloc(sizeof *o);
  if (!o) return E_OUTOFMEMORY;
  memset(o, 0, sizeof *o);
  o->main.lpVtbl = surface ? g_wr_surface_vtbl : g_wr_device_vtbl;
  o->closable.lpVtbl = g_wr_close_vtbl;
  o->access.lpVtbl = g_wr_access_vtbl;
  o->ref = 1;
  o->surface = surface;
  o->inner = inner;
  InitializeCriticalSection(&o->lock);
  *out = (IUnknown *)&o->main;
  return S_OK;
}
EXPORT HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice *device, IUnknown **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!device) return E_INVALIDARG;
  IDXGIDevice *own = NULL;
  if (FAILED(IDXGIDevice_QueryInterface(device, &IID_IDXGIDevice, (void **)&own)) || !own) return E_INVALIDARG;
  const HRESULT hr = winrt_make((IUnknown *)own, FALSE, out);
  if (FAILED(hr)) IDXGIDevice_Release(own);
  return hr;
}
EXPORT HRESULT WINAPI CreateDirect3D11SurfaceFromDXGISurface(IDXGISurface *surface, IUnknown **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!surface) return E_INVALIDARG;
  IDXGISurface *own = NULL;
  if (FAILED(IDXGISurface_QueryInterface(surface, &IID_IDXGISurface, (void **)&own)) || !own) return E_INVALIDARG;
  const HRESULT hr = winrt_make((IUnknown *)own, TRUE, out);
  if (FAILED(hr)) IDXGISurface_Release(own);
  return hr;
}
