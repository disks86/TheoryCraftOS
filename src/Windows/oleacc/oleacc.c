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

/* oleacc.dll for TheoryCraftOS: Microsoft Active Accessibility. There are no accessibility clients and no standard accessible objects here, so the calls that would hand
 * out or marshal an IAccessible answer E_NOTIMPL with their out-parameters cleared (a stub answered S_OK and left them unset). LresultFromObject's LRESULT is that
 * HRESULT, which a window procedure returns for WM_GETOBJECT as Windows' own does on failure. The rest is still stubs (src/Windows/stubs/oleacc.inc). Built freestanding
 * over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define E_NOTIMPL_ ((HRESULT)0x80004001)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT HRESULT WINAPI AccessibleObjectFromWindow(HWND window, DWORD object_id, const GUID *iid, void **object) {
  (void)window; (void)object_id; (void)iid;
  if (object) *object = NULL;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI CreateStdAccessibleObject(HWND window, LONG object_id, const GUID *iid, void **object) {
  (void)window; (void)object_id; (void)iid;
  if (object) *object = NULL;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI CreateStdAccessibleProxyW(HWND window, LPCWSTR class_name, LONG object_id, const GUID *iid, void **object) {
  (void)window; (void)class_name; (void)object_id; (void)iid;
  if (object) *object = NULL;
  return E_NOTIMPL_;
}
EXPORT LRESULT WINAPI LresultFromObject(const GUID *iid, WPARAM wparam, void *object) {
  (void)iid; (void)wparam; (void)object;
  return (LRESULT)E_NOTIMPL_;
}
EXPORT HRESULT WINAPI ObjectFromLresult(LRESULT result, const GUID *iid, WPARAM wparam, void **object) {
  (void)result; (void)iid; (void)wparam;
  if (object) *object = NULL;
  return E_NOTIMPL_;
}
/* AccessibleChildren(IAccessible *, LONG start, LONG count, VARIANT *children, LONG *obtained) */
EXPORT HRESULT WINAPI AccessibleChildren(void *container, LONG start, LONG count, void *children, LONG *obtained) {
  (void)container; (void)start; (void)count; (void)children;
  if (obtained) *obtained = 0;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI WindowFromAccessibleObject(void *object, HWND *window) {
  (void)object;
  if (window) *window = NULL;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI AccessibleObjectFromPoint(POINT point, void **object, void *child) {
  (void)point; (void)child;
  if (object) *object = NULL;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI AccessibleObjectFromEvent(HWND window, DWORD object_id, DWORD child_id, void **object, void *child) {
  (void)window; (void)object_id; (void)child_id; (void)child;
  if (object) *object = NULL;
  return E_NOTIMPL_;
}
