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

/* d3d10_1.dll for TheoryCraftOS: Direct3D 10.1's library. Its devices are d3d10core.dll's of a feature level (10_1, 10_0 or 9_x: D3D10CreateDevice1); everything else it
 * exports is d3d10.dll's, forwarded as Windows does (d3d10_1.def). */
#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10_1.h>
#include <dxgi.h>

#define EXPORT __declspec(dllexport)

/* (d3d10core.dll) */
__declspec(dllimport) HRESULT WINAPI D3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL level, ID3D10Device **device);

static BOOL level_valid(D3D10_FEATURE_LEVEL1 level) {
  return level == D3D10_FEATURE_LEVEL_10_0 || level == D3D10_FEATURE_LEVEL_10_1 || level == D3D10_FEATURE_LEVEL_9_1 || level == D3D10_FEATURE_LEVEL_9_2 ||
         level == D3D10_FEATURE_LEVEL_9_3;
}
/* The adapter of a driver type and a device of `level` on it, as d3d10.dll's D3D10CreateDevice does it (DXGI_ERROR_UNSUPPORTED when the adapter has not that level). */
static HRESULT device_on(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, D3D10_FEATURE_LEVEL1 level, ID3D10Device1 **out) {
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
      case D3D10_DRIVER_TYPE_WARP: hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter); break;
      case D3D10_DRIVER_TYPE_SOFTWARE: hr = software ? IDXGIFactory_CreateSoftwareAdapter(factory, software, &adapter) : E_FAIL; break;
      default: {
        HMODULE ref = LoadLibraryA("d3d10ref.dll");      /* (the DirectX SDK's reference rasterizer, the NULL device's too) */
        if (!ref) { hr = E_FAIL; break; }
        hr = IDXGIFactory_CreateSoftwareAdapter(factory, ref, &adapter);
        FreeLibrary(ref);
        break;
      }
    }
    if (FAILED(hr)) { IDXGIFactory_Release(factory); return hr; }
  }
  ID3D10Device *device = NULL;
  hr = D3D10CoreCreateDevice(factory, adapter, flags, (D3D_FEATURE_LEVEL)level, &device);
  IDXGIAdapter_Release(adapter);
  IDXGIFactory_Release(factory);
  if (FAILED(hr)) return hr;
  hr = ID3D10Device_QueryInterface(device, &IID_ID3D10Device1, (void **)out);
  ID3D10Device_Release(device);
  return hr;
}
EXPORT HRESULT WINAPI D3D10CreateDevice1(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, D3D10_FEATURE_LEVEL1 level, UINT sdk, ID3D10Device1 **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (sdk != D3D10_1_SDK_VERSION || !level_valid(level)) return E_INVALIDARG;
  return device_on(adapter, type, software, flags, level, out);
}
EXPORT HRESULT WINAPI D3D10CreateDeviceAndSwapChain1(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, D3D10_FEATURE_LEVEL1 level, UINT sdk,
                                                     DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain, ID3D10Device1 **out) {
  /* (the device is needed, the swap chain only when one is asked for: its description is then the swap chain's to check) */
  if (chain) *chain = NULL;
  if (!out) return E_INVALIDARG;
  HRESULT hr = D3D10CreateDevice1(adapter, type, software, flags, level, sdk, out);
  if (FAILED(hr) || !chain) return hr;
  IDXGIDevice *dxgi = NULL;
  IDXGIAdapter *a = NULL;
  IDXGIFactory *factory = NULL;
  hr = ID3D10Device1_QueryInterface(*out, &IID_IDXGIDevice, (void **)&dxgi);
  if (SUCCEEDED(hr)) { hr = IDXGIDevice_GetAdapter(dxgi, &a); IDXGIDevice_Release(dxgi); }
  if (SUCCEEDED(hr)) { hr = IDXGIAdapter_GetParent(a, &IID_IDXGIFactory, (void **)&factory); IDXGIAdapter_Release(a); }
  if (SUCCEEDED(hr)) { hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)*out, desc, chain); IDXGIFactory_Release(factory); }
  if (FAILED(hr)) { ID3D10Device1_Release(*out); *out = NULL; }
  return hr;
}
/* The runtime's version: that of the SDK it was built for. */
EXPORT UINT WINAPI D3D10GetVersion(void) { return D3D10_1_SDK_VERSION; }
/* Vista's switch back to the runtime before its service pack: there is no older runtime here. */
EXPORT HRESULT WINAPI RevertToOldImplementation(void) { return E_NOTIMPL; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reason; (void)reserved;
  return TRUE;
}
