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

/* Part of d3d12.c: the Agility SDK's interfaces - D3D12GetInterface with CLSID_D3D12SDKConfiguration (ID3D12SDKConfiguration, ID3D12SDKConfiguration1) and
 * CLSID_D3D12DeviceFactory (ID3D12DeviceFactory), the device's ID3D12DeviceConfiguration - and the making of devices, which D3D12CreateDevice and the factories share.
 *
 * On Windows d3d12.dll is a loader: the core (D3D12Core.dll) comes from the system or, for a program that exports D3D12SDKVersion and D3D12SDKPath (or calls
 * SetSDKVersion / CreateDeviceFactory), from the program's own folder (.\D3D12\D3D12Core.dll), and the core talks to the display driver's user-mode part. Here there
 * is one core, this library, over the kernel's Direct3D (no WDDM user-mode driver exists for Microsoft's core to talk to): a program's D3D12SDKVersion and D3D12SDKPath
 * and its D3D12Core.dll are never loaded. Every version up to TCOS_SDK_VERSION runs on this core - each SDK version's interfaces are a superset of the earlier
 * ones', and the optional features of each are answered by CheckFeatureSupport as the adapter has them -, so such a version is accepted and reported
 * (ID3D12DeviceConfiguration::GetDesc's SDKVersion). A newer version - interfaces this core does not have - is refused where a program asks for it explicitly
 * (SetSDKVersion, CreateDeviceFactory: D3D12_ERROR_INVALID_REDIST, Windows' answer for an SDK version it cannot provide); a program that only exports a newer
 * D3D12SDKVersion runs on this core, which reports its own version (the program's D3D12Core.dll would not run here at all).
 *
 * The SDK layers (CLSID_D3D12Debug, CLSID_D3D12Tools: D3D12SDKLayers.dll's) are not part of this system: DXGI_ERROR_SDK_COMPONENT_MISSING, as D3D12GetDebugInterface.
 * Experimental features need Windows' developer mode, which this system does not have: E_NOINTERFACE for any (as Windows without it). DRED
 * (CLSID_D3D12DeviceRemovedExtendedData) is not implemented: E_NOINTERFACE - an open gap. */

#define TCOS_SDK_VERSION 614u      /* Agility SDK 1.614: ID3D12Device14, ID3D12GraphicsCommandList10, root signature 1.2 - the newest version all of whose interfaces this core has */
#define D3D12_ERROR_INVALID_REDIST_ ((HRESULT)0x887E0003L)      /* D3D12_ERROR_INVALID_REDIST (winerror.h of the Agility SDK; mingw's lacks it) */
/* {114863bf-c386-4aee-b39d-8f0bbb062955} (d3d12.h of the Agility SDK; mingw's lacks it) */
static const GUID CLSID_D3D12DeviceFactory_ = {0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};

/* ---- the SDK version of the process ------------------------------------------------------------------------------------------------------------------------------------- */

/* What the program asked for: its export D3D12SDKVersion (a UINT; D3D12SDKPath, the folder of its D3D12Core.dll, is not used - this library is the core), a version
 * of this core at most; without one, this core's own. SetSDKVersion changes it until the first device is made. */
static UINT sdk_version_of_process(void) {
  UINT v = g_sdk_version;
  if (v) return v;
  const UINT *exported = (const UINT *)(void *)GetProcAddress(GetModuleHandleW(NULL), "D3D12SDKVersion");
  v = exported && *exported && *exported <= TCOS_SDK_VERSION ? *exported : TCOS_SDK_VERSION;
  InterlockedCompareExchange((LONG *)&g_sdk_version, (LONG)v, 0);
  return g_sdk_version;
}
/* A version a program asks for by name: one of this core's (1 .. TCOS_SDK_VERSION), with a path (the folder Windows would load its core from). */
static HRESULT sdk_version_check(UINT version, const char *path) {
  if (!path || version == 0) return E_INVALIDARG;
  return version <= TCOS_SDK_VERSION ? S_OK : D3D12_ERROR_INVALID_REDIST_;
}
static BOOL devices_alive(void) {
  AcquireSRWLockShared(&g_devices_lock);
  const BOOL any = g_devices != NULL;
  ReleaseSRWLockShared(&g_devices_lock);
  return any;
}
/* The process's version, as SetSDKVersion and ApplyToGlobalState set it: once a device is made, only to what it already is (the core of a process cannot change under
 * its devices). */
static HRESULT sdk_version_apply(UINT version) {
  if (devices_alive() && version != sdk_version_of_process()) return DXGI_ERROR_INVALID_CALL;
  InterlockedExchange((LONG *)&g_sdk_version, (LONG)version);
  return S_OK;
}

/* ---- making devices ------------------------------------------------------------------------------------------------------------------------------------------------------ */

/* A reference to a device of the live list, unless its last one is going (its count is 0: dev_destroy is about to take it off the list). Under g_devices_lock. */
static BOOL dev_try_addref(Dev *d) {
  for (;;) {
    const LONG n = d->o.ref;
    if (n <= 0) return FALSE;
    if (InterlockedCompareExchange(&d->o.ref, n + 1, n) == n) return TRUE;
  }
}

/* The adapter a device is made on: the LUID of the adapter given (any IDXGIAdapter: this system's dxgi.dll's or another one's - DXVK's - whose LUID is the kernel's
 * adapter's), or the first of the kernel's. */
static HRESULT device_adapter_luid(IUnknown *adapter, LUID *luid) {
  if (adapter) {
    IDXGIAdapter *a = NULL;
    DXGI_ADAPTER_DESC desc;
    if (FAILED(adapter->lpVtbl->QueryInterface(adapter, &IID_IDXGIAdapter, (void **)&a)) || !a) return E_INVALIDARG;
    const HRESULT hr = IDXGIAdapter_GetDesc(a, &desc);
    IDXGIAdapter_Release(a);
    if (FAILED(hr)) return hr;
    *luid = desc.AdapterLuid;
    return S_OK;
  }
  KmtAdapter list[16];
  const UINT n = kmt_enum_adapters(list, 16, NULL);
  if (n == 0) return DXGI_ERROR_UNSUPPORTED;
  *luid = list[0].luid;
  for (UINT i = 0; i < n; ++i) kmt_close_adapter(list[i].handle);
  return S_OK;
}

/* A device of feature level `minimum` at least on the adapter, for SDK version `sdk_version`. `factory_flags`: D3D12_DEVICE_FACTORY_FLAGS of a device factory, or ~0u
 * for D3D12CreateDevice (which makes a new device each time; the first on an adapter is its singleton). A factory: an adapter's singleton that is there already is
 * handed out when the flags allow (ALLOW_RETURNING_EXISTING_DEVICE: one of the same version and a level high enough; ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE: any),
 * else a new device would be a second singleton - DXGI_ERROR_ALREADY_EXISTS - unless DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON makes it one that is not. */
static HRESULT device_make(IUnknown *adapter, D3D_FEATURE_LEVEL minimum, UINT sdk_version, UINT factory_flags, REFIID riid, void **out) {
  /* (a value that names no feature level is not one; a level the adapter lacks - device.inc dev_feature_level: 11_1 on the CPU renderer, 11_0 on virgl and Venus, nothing above
   * 11_1 anywhere - is refused below as the adapter not supporting it) */
  switch ((UINT)minimum) {
    case 0: case D3D_FEATURE_LEVEL_1_0_CORE: case D3D_FEATURE_LEVEL_9_1: case D3D_FEATURE_LEVEL_9_2: case D3D_FEATURE_LEVEL_9_3: case D3D_FEATURE_LEVEL_10_0: case D3D_FEATURE_LEVEL_10_1:
    case D3D_FEATURE_LEVEL_11_0: case D3D_FEATURE_LEVEL_11_1: case D3D_FEATURE_LEVEL_12_0: case D3D_FEATURE_LEVEL_12_1: case D3D_FEATURE_LEVEL_12_2: break;
    default: return E_INVALIDARG;
  }
  LUID luid = {0, 0};
  HRESULT hr = device_adapter_luid(adapter, &luid);
  if (FAILED(hr)) return hr;
  if (out) *out = NULL;
  const BOOL factory = factory_flags != ~0u;
  Dev *existing = NULL;
  AcquireSRWLockShared(&g_devices_lock);
  for (Dev *d = g_devices; d && !existing; d = d->next_live)
    if (d->singleton && d->luid.LowPart == luid.LowPart && d->luid.HighPart == luid.HighPart && dev_try_addref(d)) existing = d;
  ReleaseSRWLockShared(&g_devices_lock);
  if (factory && existing) {
    const BOOL compatible = existing->sdk_version == sdk_version && (minimum == 0 || dev_feature_level(existing) >= minimum);
    if ((compatible && (factory_flags & D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE)) ||
        (factory_flags & D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE)) {
      if (!out) { dx_Release(OBJ(existing)); return S_FALSE; }
      hr = dx_QueryInterface(OBJ(existing), riid, out);
      dx_Release(OBJ(existing));
      return hr;
    }
    if (!(factory_flags & D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)) {
      dx_Release(OBJ(existing));
      return DXGI_ERROR_ALREADY_EXISTS;
    }
  }
  const BOOL store = !existing && !(factory && (factory_flags & D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON));
  if (existing) dx_Release(OBJ(existing));
  NTSTATUS st = 0;
  Dev *d = dev_create(luid, &st);
  if (!d) return KMT_OK(st) ? E_OUTOFMEMORY : DXGI_ERROR_UNSUPPORTED;
  d->sdk_version = sdk_version;
  /* (a program that asks for a feature level the adapter does not have gets what Direct3D 12 says: the adapter does not support it) */
  if (minimum != 0 && dev_feature_level(d) < minimum) {
    dx_Release(OBJ(d));
    return DXGI_ERROR_UNSUPPORTED;
  }
  if (!out) {      /* (the question "could a device be made?") */
    dx_Release(OBJ(d));
    return S_FALSE;
  }
  if (store) {      /* (the adapter's singleton - unless another became it meanwhile) */
    AcquireSRWLockExclusive(&g_devices_lock);
    BOOL taken = FALSE;
    for (Dev *o = g_devices; o; o = o->next_live)
      if (o != d && o->singleton && o->o.ref > 0 && o->luid.LowPart == luid.LowPart && o->luid.HighPart == luid.HighPart) taken = TRUE;
    d->singleton = !taken;
    ReleaseSRWLockExclusive(&g_devices_lock);
  }
  return query_out((IUnknown *)d, riid, out);
}

/* ---- ID3D12DeviceConfiguration (the device's face) -------------------------------------------------------------------------------------------------------------------- */

static Dev *dev_of_config(ID3D12DeviceConfiguration *c) { return (Dev *)((BYTE *)c - offsetof(Dev, config)); }
static HRESULT STDMETHODCALLTYPE devconfig_QueryInterface(ID3D12DeviceConfiguration *c, REFIID riid, void **out) { return dev_QueryInterface(dev_of_config(c), riid, out); }
static ULONG STDMETHODCALLTYPE devconfig_AddRef(ID3D12DeviceConfiguration *c) { return dx_AddRef(OBJ(dev_of_config(c))); }
static ULONG STDMETHODCALLTYPE devconfig_Release(ID3D12DeviceConfiguration *c) { return dx_Release(OBJ(dev_of_config(c))); }
/* (no debug layer, no GPU-based validation, no DRED: none of the flags; no experimental features) */
static D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE devconfig_GetDesc(ID3D12DeviceConfiguration *c, D3D12_DEVICE_CONFIGURATION_DESC *ret) {
  memset(ret, 0, sizeof *ret);
  ret->Flags = D3D12_DEVICE_FLAG_NONE;
  ret->GpuBasedValidationFlags = 0;
  ret->SDKVersion = dev_of_config(c)->sdk_version;
  ret->NumEnabledExperimentalFeatures = 0;
  return ret;
}
static HRESULT STDMETHODCALLTYPE devconfig_GetEnabledExperimentalFeatures(ID3D12DeviceConfiguration *c, GUID *guids, UINT count) {
  (void)c;
  /* (the count must be NumEnabledExperimentalFeatures: none) */
  if (count != 0 || (count && !guids)) return E_INVALIDARG;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE devconfig_SerializeVersionedRootSignature(ID3D12DeviceConfiguration *c, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
                                                                           ID3DBlob **error) {
  (void)c;
  return D3D12SerializeVersionedRootSignature(desc, blob, error);
}
static HRESULT STDMETHODCALLTYPE devconfig_CreateVersionedRootSignatureDeserializer(ID3D12DeviceConfiguration *c, const void *blob, SIZE_T size, REFIID riid, void **out) {
  (void)c;
  return D3D12CreateVersionedRootSignatureDeserializer(blob, size, riid, out);
}
const ID3D12DeviceConfigurationVtbl devconfig_vtbl = {devconfig_QueryInterface, devconfig_AddRef, devconfig_Release, devconfig_GetDesc, devconfig_GetEnabledExperimentalFeatures,
                                                      devconfig_SerializeVersionedRootSignature, devconfig_CreateVersionedRootSignatureDeserializer};

/* ---- ID3D12DeviceFactory -------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct DevFactory {
  DxObj o;
  UINT sdk_version;
  D3D12_DEVICE_FACTORY_FLAGS flags;
} DevFactory;
static const GUID *const g_devfactory_iids[] = {&IID_ID3D12DeviceFactory, NULL};
#define DEVICE_FACTORY_FLAGS_ALL                                                                                                                                                 \
  (D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE | D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE |                                        \
   D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)

/* (the configuration of the process: its SDK version - experimental features and the SDK layers' settings, the rest of Windows' global state, cannot be set here) */
static HRESULT STDMETHODCALLTYPE devfac_InitializeFromGlobalState(DevFactory *f) {
  f->sdk_version = sdk_version_of_process();
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE devfac_ApplyToGlobalState(DevFactory *f) { return sdk_version_apply(f->sdk_version); }
static HRESULT STDMETHODCALLTYPE devfac_SetFlags(DevFactory *f, D3D12_DEVICE_FACTORY_FLAGS flags) {
  if ((UINT)flags & ~(UINT)DEVICE_FACTORY_FLAGS_ALL) return E_INVALIDARG;
  f->flags = flags;
  return S_OK;
}
static D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE devfac_GetFlags(DevFactory *f) { return f->flags; }
static HRESULT configuration_interface(REFCLSID clsid, REFIID riid, void **out);
static HRESULT STDMETHODCALLTYPE devfac_GetConfigurationInterface(DevFactory *f, REFCLSID clsid, REFIID riid, void **out) {
  (void)f;
  return configuration_interface(clsid, riid, out);
}
static HRESULT STDMETHODCALLTYPE devfac_EnableExperimentalFeatures(DevFactory *f, UINT count, const IID *iids, void *configuration, UINT *sizes) {
  (void)f;
  return D3D12EnableExperimentalFeatures(count, iids, configuration, sizes);
}
static HRESULT STDMETHODCALLTYPE devfac_CreateDevice(DevFactory *f, IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **out) {
  return device_make(adapter, level, f->sdk_version, (UINT)f->flags, riid, out);
}
static const ID3D12DeviceFactoryVtbl devfac_vtbl = {
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->QueryInterface))dx_QueryInterface,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->AddRef))dx_AddRef,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->Release))dx_Release,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->InitializeFromGlobalState))devfac_InitializeFromGlobalState,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->ApplyToGlobalState))devfac_ApplyToGlobalState,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->SetFlags))devfac_SetFlags,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->GetFlags))devfac_GetFlags,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->GetConfigurationInterface))devfac_GetConfigurationInterface,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->EnableExperimentalFeatures))devfac_EnableExperimentalFeatures,
    (__typeof__(((ID3D12DeviceFactoryVtbl *)0)->CreateDevice))devfac_CreateDevice,
};
static HRESULT device_factory_make(UINT sdk_version, REFIID riid, void **out) {
  DevFactory *f = mem_alloc(sizeof *f);
  if (!f) return E_OUTOFMEMORY;
  dx_init(OBJ(f), &devfac_vtbl, g_devfactory_iids, NULL);
  f->sdk_version = sdk_version;
  f->flags = D3D12_DEVICE_FACTORY_FLAG_NONE;
  const HRESULT hr = dx_QueryInterface(OBJ(f), riid, out);
  dx_Release(OBJ(f));
  return hr;
}

/* ---- ID3D12SDKConfiguration, ID3D12SDKConfiguration1 ------------------------------------------------------------------------------------------------------------------- */

static const GUID *const g_sdkconfig_iids[] = {&IID_ID3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, NULL};
static HRESULT STDMETHODCALLTYPE sdkcfg_SetSDKVersion(DxObj *o, UINT version, const char *path) {
  (void)o;
  const HRESULT hr = sdk_version_check(version, path);
  return FAILED(hr) ? hr : sdk_version_apply(version);
}
static HRESULT STDMETHODCALLTYPE sdkcfg_CreateDeviceFactory(DxObj *o, UINT version, const char *path, REFIID riid, void **out) {
  (void)o;
  if (!out) return E_POINTER;
  *out = NULL;
  const HRESULT hr = sdk_version_check(version, path);
  return FAILED(hr) ? hr : device_factory_make(version, riid, out);
}
/* (no SDK's core is ever loaded: none to free) */
static void STDMETHODCALLTYPE sdkcfg_FreeUnusedSDKs(DxObj *o) { (void)o; }
static const ID3D12SDKConfiguration1Vtbl sdkcfg_vtbl = {
    (__typeof__(((ID3D12SDKConfiguration1Vtbl *)0)->QueryInterface))dx_QueryInterface,
    (__typeof__(((ID3D12SDKConfiguration1Vtbl *)0)->AddRef))dx_AddRef,
    (__typeof__(((ID3D12SDKConfiguration1Vtbl *)0)->Release))dx_Release,
    (__typeof__(((ID3D12SDKConfiguration1Vtbl *)0)->SetSDKVersion))sdkcfg_SetSDKVersion,
    (__typeof__(((ID3D12SDKConfiguration1Vtbl *)0)->CreateDeviceFactory))sdkcfg_CreateDeviceFactory,
    (__typeof__(((ID3D12SDKConfiguration1Vtbl *)0)->FreeUnusedSDKs))sdkcfg_FreeUnusedSDKs,
};

/* What D3D12GetInterface and ID3D12DeviceFactory::GetConfigurationInterface hand out. */
static HRESULT configuration_interface(REFCLSID clsid, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!clsid || !riid) return E_INVALIDARG;
  if (guid_eq(clsid, &CLSID_D3D12Debug) || guid_eq(clsid, &CLSID_D3D12Tools)) return DXGI_ERROR_SDK_COMPONENT_MISSING;      /* (D3D12SDKLayers.dll's) */
  if (guid_eq(clsid, &CLSID_D3D12SDKConfiguration)) {
    DxObj *c = mem_alloc(sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    dx_init(c, &sdkcfg_vtbl, g_sdkconfig_iids, NULL);
    const HRESULT hr = dx_QueryInterface(c, riid, out);
    dx_Release(c);
    return hr;
  }
  if (guid_eq(clsid, &CLSID_D3D12DeviceFactory_)) return device_factory_make(sdk_version_of_process(), riid, out);
  return E_NOINTERFACE;      /* (CLSID_D3D12DeviceRemovedExtendedData: no DRED here; any other class: none) */
}
