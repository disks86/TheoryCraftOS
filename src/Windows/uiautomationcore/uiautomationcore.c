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

/* uiautomationcore.dll for TheoryCraftOS: UI Automation. There are no automation clients (screen readers) here, so nothing listens: the events a provider raises and
 * the disconnects succeed and go nowhere, UiaClientsAreListening says FALSE, WM_GETOBJECT is answered with 0 (UiaReturnRawElementProvider), and whatever would hand out
 * an object (the host provider of a window, the reserved values) fails with E_NOTIMPL and a NULL pointer. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT BOOL WINAPI UiaClientsAreListening(void) { return FALSE; }
EXPORT LRESULT WINAPI UiaReturnRawElementProvider(HWND window, WPARAM wparam, LPARAM lparam, void *provider) { (void)window; (void)wparam; (void)lparam; (void)provider; return 0; }
EXPORT HRESULT WINAPI UiaHostProviderFromHwnd(HWND window, void **provider) { (void)window; if (provider) *provider = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI UiaGetReservedMixedAttributeValue(void **value) { if (value) *value = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI UiaGetReservedNotSupportedValue(void **value) { if (value) *value = NULL; return E_NOTIMPL; }

/* Events and disconnects: no client, nothing to tell. (The VARIANTs of the property event are passed by value, which the x64 convention turns into pointers.) */
EXPORT HRESULT WINAPI UiaRaiseAutomationEvent(void *provider, int event) { (void)provider; (void)event; return S_OK; }
EXPORT HRESULT WINAPI UiaRaiseAutomationPropertyChangedEvent(void *provider, int property, const void *old_value, const void *new_value) {
  (void)provider; (void)property; (void)old_value; (void)new_value;
  return S_OK;
}
EXPORT HRESULT WINAPI UiaRaiseStructureChangedEvent(void *provider, int type, int *runtime_id, int length) { (void)provider; (void)type; (void)runtime_id; (void)length; return S_OK; }
EXPORT HRESULT WINAPI UiaDisconnectProvider(void *provider) { return provider ? S_OK : E_INVALIDARG; }
EXPORT HRESULT WINAPI UiaDisconnectAllProviders(void) { return S_OK; }
