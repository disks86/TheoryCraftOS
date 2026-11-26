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

/* d3d10core.dll for TheoryCraftOS: the core of Direct3D 10. Its device is d3d11.dll's (a Direct3D 11 device that is also ID3D10Device1: src/Windows/d3d11/d3d10.inc), so
 * D3D10CoreCreateDevice and the driver entry points are forwarders to d3d11.dll (d3d10core.def); what is left is here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10.h>
#include <dxgi.h>

#define EXPORT __declspec(dllexport)

/* The SDK layers (d3d10sdklayers.dll) register with the core here. No SDK layer is part of this system: what Windows answers without the SDK component, as d3d11.dll's
 * D3D11CoreRegisterLayers. */
EXPORT HRESULT WINAPI D3D10CoreRegisterLayers(void) { return DXGI_ERROR_SDK_COMPONENT_MISSING; }
/* (d3d11.dll's: a function of this library's own that calls it, so that d3d11.dll is an import of this one - a forwarder alone does not make the loader load it) */
__declspec(dllimport) HRESULT WINAPI TcosD3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL level, ID3D10Device **device);
EXPORT HRESULT WINAPI D3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL level, ID3D10Device **device) {
  return TcosD3D10CoreCreateDevice(factory, adapter, flags, level, device);
}
/* The core's version: that of the SDK it was built for. */
EXPORT UINT WINAPI D3D10CoreGetVersion(void) { return D3D10_SDK_VERSION; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reason; (void)reserved;
  return TRUE;
}
