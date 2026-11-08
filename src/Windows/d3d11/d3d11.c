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

/* d3d11.dll for TheoryCraftOS: there is no Direct3D 11 runtime yet (Direct3D 12 is in d3d12.dll), so a device cannot be created: DXGI_ERROR_UNSUPPORTED, the answer of
 * Windows on a machine with no Direct3D 11 driver, with every out-parameter cleared. Programs then take their fallback (Chrome: SwiftShader / WARP-less software GL).
 * A stub answered S_OK and left the device pointer unset. The rest is still stubs (src/Windows/stubs/d3d11.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define DXGI_ERROR_UNSUPPORTED_ ((HRESULT)0x887A0004)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

/* D3D11CreateDevice(adapter, driver type, software module, flags, feature levels, count, SDK version, device, feature level, immediate context) */
EXPORT HRESULT WINAPI D3D11CreateDevice(void *adapter, int driver_type, HMODULE software, UINT flags, const int *levels, UINT level_count, UINT sdk, void **device,
                                        int *feature_level, void **context) {
  (void)adapter; (void)driver_type; (void)software; (void)flags; (void)levels; (void)level_count; (void)sdk;
  if (device) *device = NULL;
  if (feature_level) *feature_level = 0;
  if (context) *context = NULL;
  return DXGI_ERROR_UNSUPPORTED_;
}

/* D3D11CreateDeviceAndSwapChain(the same, then the swap chain description and the swap chain before the device) */
EXPORT HRESULT WINAPI D3D11CreateDeviceAndSwapChain(void *adapter, int driver_type, HMODULE software, UINT flags, const int *levels, UINT level_count, UINT sdk,
                                                    const void *description, void **swap_chain, void **device, int *feature_level, void **context) {
  (void)description;
  if (swap_chain) *swap_chain = NULL;
  return D3D11CreateDevice(adapter, driver_type, software, flags, levels, level_count, sdk, device, feature_level, context);
}

EXPORT HRESULT WINAPI D3D11On12CreateDevice(void *device12, UINT flags, const int *levels, UINT level_count, void *const *queues, UINT queue_count, UINT node_mask,
                                            void **device, void **context, int *feature_level) {
  (void)device12; (void)flags; (void)levels; (void)level_count; (void)queues; (void)queue_count; (void)node_mask;
  if (device) *device = NULL;
  if (context) *context = NULL;
  if (feature_level) *feature_level = 0;
  return DXGI_ERROR_UNSUPPORTED_;
}
