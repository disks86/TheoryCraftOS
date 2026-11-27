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

/* A "foreign" Direct3D library that a program brings in its own folder (as it would bring DXVK's d3d9.dll or vkd3d-proton's d3d12.dll): the public entry points
 * of one of d3d9.dll, d3d12.dll, d3d11.dll or d3d10.dll (-DFOREIGN_D3D9 and so on), each failing, and none of this OS's private exports. If the system's
 * libraries bound to it, they would get stubs for their private imports and failures for the public ones. tests/windows/bundletest.c (tests/bundled_dlls_qemu.sh)
 * checks that they do not, and that the program's own LoadLibrary of the name gets this one.
 *   x86_64-w64-mingw32-gcc -O1 -shared -DFOREIGN_D3D9 -Wl,--kill-at -o d3d9.dll tests/windows/bundle_foreign.c */
#include <windows.h>

#define EXPORT __declspec(dllexport)

/* (what tells this library from the system's) */
EXPORT int WINAPI TcosForeignLibrary(void) { return 1; }

#ifdef FOREIGN_D3D9
EXPORT void *WINAPI Direct3DCreate9(UINT version) { (void)version; return NULL; }
EXPORT HRESULT WINAPI Direct3DCreate9Ex(UINT version, void **out) { (void)version; if (out) *out = NULL; return E_FAIL; }
#endif
#ifdef FOREIGN_D3D12
EXPORT HRESULT WINAPI D3D12CreateDevice(void *adapter, int level, const GUID *iid, void **out) { (void)adapter; (void)level; (void)iid; if (out) *out = NULL; return E_FAIL; }
EXPORT HRESULT WINAPI D3D12GetDebugInterface(const GUID *iid, void **out) { (void)iid; if (out) *out = NULL; return E_FAIL; }
EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const void *desc, int version, void **blob, void **error) { (void)desc; (void)version; if (blob) *blob = NULL; if (error) *error = NULL; return E_FAIL; }
#endif
#ifdef FOREIGN_D3D11
EXPORT HRESULT WINAPI D3D11CreateDevice(void *adapter, int type, HMODULE software, UINT flags, const void *levels, UINT count, UINT sdk, void **device, void *level, void **context) {
  (void)adapter; (void)type; (void)software; (void)flags; (void)levels; (void)count; (void)sdk; (void)level;
  if (device) *device = NULL;
  if (context) *context = NULL;
  return E_FAIL;
}
EXPORT HRESULT WINAPI D3D11CreateDeviceAndSwapChain(void *adapter, int type, HMODULE software, UINT flags, const void *levels, UINT count, UINT sdk, const void *desc, void **chain,
                                                    void **device, void *level, void **context) {
  (void)desc;
  if (chain) *chain = NULL;
  return D3D11CreateDevice(adapter, type, software, flags, levels, count, sdk, device, level, context);
}
#endif
#ifdef FOREIGN_D3D10
EXPORT HRESULT WINAPI D3D10CreateDevice(void *adapter, int type, HMODULE software, UINT flags, UINT sdk, void **device) {
  (void)adapter; (void)type; (void)software; (void)flags; (void)sdk;
  if (device) *device = NULL;
  return E_FAIL;
}
#endif

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) { (void)module; (void)reason; (void)reserved; return TRUE; }
