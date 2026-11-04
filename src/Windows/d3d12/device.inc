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

/* d3d12.dll: the device (ID3D12Device) and the factory functions. Included by d3d12.c after the other parts. */

/* The private fence handles and the like are in the other parts; the device is made here. */
static void dev_destroy(DxObj *o) {
  Dev *d = (Dev *)o;
  if (d->null_target) kmt_destroy_allocation(d->kdev, d->null_target);
  if (d->kdev) kmt_destroy_device(d->kdev);
  if (d->adapter) kmt_close_adapter(d->adapter);
}

/* ---- ID3D12Device ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

static UINT STDMETHODCALLTYPE dev_GetNodeCount(Dev *This) { return 1; }
static LUID *STDMETHODCALLTYPE dev_GetAdapterLuid(Dev *This, LUID *ret) { *ret = This->luid; return ret; }
static HRESULT STDMETHODCALLTYPE dev_GetDeviceRemovedReason(Dev *This) { return This->removed ? DXGI_ERROR_DEVICE_REMOVED : S_OK; }
static HRESULT STDMETHODCALLTYPE dev_SetStablePowerState(Dev *This, BOOL enable) { return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_MakeResident(Dev *This, UINT count, ID3D12Pageable *const *objects) { return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_Evict(Dev *This, UINT count, ID3D12Pageable *const *objects) { return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_CreateSharedHandle(Dev *This, ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dev_OpenSharedHandle(Dev *This, HANDLE handle, REFIID riid, void **object) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dev_OpenSharedHandleByName(Dev *This, const WCHAR *name, DWORD access, HANDLE *handle) { return E_NOTIMPL; }

static UINT STDMETHODCALLTYPE dev_GetDescriptorHandleIncrementSize(Dev *This, D3D12_DESCRIPTOR_HEAP_TYPE type) { return type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES ? DESCRIPTOR_STRIDE : 0; }

static D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE dev_GetCustomHeapProperties(Dev *This, D3D12_HEAP_PROPERTIES *ret, UINT node, D3D12_HEAP_TYPE type) {
  memset(ret, 0, sizeof *ret);
  ret->Type = D3D12_HEAP_TYPE_CUSTOM;
  ret->CreationNodeMask = ret->VisibleNodeMask = 1;
  switch (type) {
    case D3D12_HEAP_TYPE_DEFAULT: ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L1; break;
    case D3D12_HEAP_TYPE_UPLOAD: ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0; break;
    default: ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0; break;
  }
  return ret;
}

/* The feature level the adapter's limits (what the kernel's graphics device takes, DeviceInfo::limits) allow: 11_0 needs 16 vertex inputs, 16 interpolants, 16 texture units and 8 render targets,
 * 10_0 the same but fewer interpolants (the ones it needs are 16 as well), and below that 9_3 (4 render targets, 16 textures) or 9_1. */
static D3D_FEATURE_LEVEL dev_feature_level(const Dev *d) {
  const KmtCaps *c = &d->caps;
  if (c->max_vertex_attributes >= 16 && c->max_varyings >= 16 && c->max_texture_units >= 16 && c->max_color_targets >= 8) return D3D_FEATURE_LEVEL_11_0;
  if (c->max_texture_units >= 16 && c->max_color_targets >= 4 && c->max_varyings >= 8) return D3D_FEATURE_LEVEL_9_3;
  return D3D_FEATURE_LEVEL_9_1;
}

/* What the device can do: Direct3D 12 on feature level 11_0 with the tiers at their lowest. The limits that are lower than the levels' (texture units, vertex attributes, ...) are
 * those of the kernel's pipeline model: docs/win32k/Dxg.md and d3d12.c say which. */
static HRESULT STDMETHODCALLTYPE dev_CheckFeatureSupport(Dev *This, D3D12_FEATURE feature, void *data, UINT size) {
  if (!data) return E_INVALIDARG;
  switch (feature) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
      D3D12_FEATURE_DATA_D3D12_OPTIONS *o = data;
      if (size != sizeof *o) return E_INVALIDARG;
      memset(o, 0, sizeof *o);
      o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
      o->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
      o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_1;
      o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
      o->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
      D3D12_FEATURE_DATA_ARCHITECTURE *a = data;
      if (size != sizeof *a || a->NodeIndex != 0) return E_INVALIDARG;
      a->TileBasedRenderer = FALSE;
      a->UMA = This->software ? TRUE : FALSE;
      a->CacheCoherentUMA = a->UMA;
      return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
      D3D12_FEATURE_DATA_FEATURE_LEVELS *f = data;
      if (size != sizeof *f) return E_INVALIDARG;
      D3D_FEATURE_LEVEL best = (D3D_FEATURE_LEVEL)0;
      const D3D_FEATURE_LEVEL top = dev_feature_level(This);
      for (UINT i = 0; i < f->NumFeatureLevels; ++i)
        if (f->pFeatureLevelsRequested[i] <= top && f->pFeatureLevelsRequested[i] > best) best = f->pFeatureLevelsRequested[i];
      f->MaxSupportedFeatureLevel = best ? best : top;
      return best ? S_OK : E_INVALIDARG;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      D3D12_FEATURE_DATA_FORMAT_SUPPORT *f = data;
      if (size != sizeof *f) return E_INVALIDARG;
      const FormatInfo *i = format_info(f->Format);
      f->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      f->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
      if (i) {
        f->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D;
        if (i->depth) f->Support1 |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
        else f->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE;
      } else if (format_bytes(f->Format)) {
        f->Support1 = D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
      }
      if (f->Format == DXGI_FORMAT_R16_UINT || f->Format == DXGI_FORMAT_R32_UINT) f->Support1 |= D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER;
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *m = data;
      if (size != sizeof *m) return E_INVALIDARG;
      m->NumQualityLevels = m->SampleCount == 1 ? 1 : 0;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO: {
      D3D12_FEATURE_DATA_FORMAT_INFO *f = data;
      if (size != sizeof *f) return E_INVALIDARG;
      f->PlaneCount = 1;
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *g = data;
      if (size != sizeof *g) return E_INVALIDARG;
      g->MaxGPUVirtualAddressBitsPerResource = 40;
      g->MaxGPUVirtualAddressBitsPerProcess = 40;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      D3D12_FEATURE_DATA_SHADER_MODEL *s = data;
      if (size != sizeof *s) return E_INVALIDARG;
      if (s->HighestShaderModel > D3D_SHADER_MODEL_5_1) s->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      D3D12_FEATURE_DATA_ROOT_SIGNATURE *r = data;
      if (size != sizeof *r) return E_INVALIDARG;
      r->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      D3D12_FEATURE_DATA_D3D12_OPTIONS1 *o = data;
      if (size != sizeof *o) return E_INVALIDARG;
      memset(o, 0, sizeof *o);
      o->TotalLaneCount = 1;
      return S_OK;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
      D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *p = data;
      if (size != sizeof *p) return E_INVALIDARG;
      p->PriorityForTypeIsSupported = p->CommandListType == D3D12_COMMAND_LIST_TYPE_DIRECT && p->Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      return S_OK;
    }
    case D3D12_FEATURE_EXISTING_HEAPS: {
      D3D12_FEATURE_DATA_EXISTING_HEAPS *e = data;
      if (size != sizeof *e) return E_INVALIDARG;
      e->Supported = FALSE;
      return S_OK;
    }
    default: return E_INVALIDARG;
  }
}

/* ---- the whole table is in the other parts: the creation methods ------------------------------------------------------------------------------------------------------------ */

static Dev *dev_create(LUID luid, NTSTATUS *status_out) {
  Dev *d = mem_alloc(sizeof *d);
  if (!d) return NULL;
  extern const ID3D12DeviceVtbl dev_vtbl;
  dx_init(OBJ(d), &dev_vtbl, g_device_iids, dev_destroy);
  d->luid = luid;
  NTSTATUS st = kmt_open_adapter(luid, &d->adapter);
  if (KMT_OK(st)) st = kmt_create_device(d->adapter, &d->kdev);
  if (KMT_OK(st) && !KMT_OK(kmt_adapter_caps(d->adapter, &d->caps))) memset(&d->caps, 0, sizeof d->caps);
  kmt_segment_sizes(d->adapter, d->segments);
  d->software = !(d->caps.flags & KMT_CAPS_ACCELERATED);
  if (!KMT_OK(st)) {
    *status_out = st;
    dx_Release(OBJ(d));
    return NULL;
  }
  *status_out = st;
  return d;
}
