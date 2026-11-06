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

/* dinput8.dll for TheoryCraftOS: DirectInput 8 with no devices behind it. DirectInput8Create hands out a real IDirectInput8A/W object (a stub answered DI_OK and left the
 * pointer NULL, and programs that enumerate game controllers right after - Godot's joypad code, SDL - called through it). The object enumerates no devices, finds none and
 * creates none (DIERR_DEVICENOTREG), so a program falls back to its other input paths (window messages for the keyboard and mouse, XInput for gamepads). Not done: the system
 * keyboard and mouse devices (GUID_SysKeyboard / GUID_SysMouse), action mapping (ConfigureDevices answers DIERR_UNSUPPORTED) and the c_dfDI* data formats. CLSID_DirectInput8 has a class factory
 * (ole32 finds it in this library) for programs that make the object with CoCreateInstance. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* (the GUIDs are spelt here: the library is not linked with dxguid) */
static const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID kIidDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

static BOOL same_guid(const GUID *a, const GUID *b) {
  const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
  for (SIZE_T i = 0; i < sizeof(GUID); ++i) if (x[i] != y[i]) return FALSE;
  return TRUE;
}

#define DIEDFL_VALID_ (DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK | DIEDFL_INCLUDEALIASES | DIEDFL_INCLUDEPHANTOMS | DIEDFL_INCLUDEHIDDEN)

/* One object answers both interfaces: the A and W tables differ only in the string types of their arguments, which nothing here reads. */
typedef struct {
  const void *vtbl_a;      /* IDirectInput8A (first: the object's pointer is the A interface) */
  const void *vtbl_w;      /* IDirectInput8W */
  LONG refs;
  BOOL initialized;
} DirectInput;

static DirectInput *from_a(void *iface) { return (DirectInput *)iface; }
static DirectInput *from_w(void *iface) { return (DirectInput *)((BYTE *)iface - offsetof(DirectInput, vtbl_w)); }

static HRESULT check_version(HINSTANCE instance, DWORD version) {
  if (!instance) return DIERR_INVALIDPARAM;
  if (version < 0x0800) return DIERR_OLDDIRECTINPUTVERSION;
  if (version > 0x0800) return DIERR_BETADIRECTINPUTVERSION;
  return DI_OK;
}

static HRESULT query(DirectInput *d, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &kIidDirectInput8A)) *out = &d->vtbl_a;
  else if (same_guid(riid, &kIidDirectInput8W)) *out = &d->vtbl_w;
  else return E_NOINTERFACE;
  InterlockedIncrement(&d->refs);
  return S_OK;
}
static ULONG add_ref(DirectInput *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG release(DirectInput *d) {
  const LONG left = InterlockedDecrement(&d->refs);
  if (left == 0) HeapFree(GetProcessHeap(), 0, d);
  return (ULONG)left;
}
static HRESULT create_device(DirectInput *d, const GUID *guid, void **device, IUnknown *outer) {
  (void)outer;
  if (!device) return E_POINTER;
  *device = NULL;
  if (!guid) return E_POINTER;
  return d->initialized ? DIERR_DEVICENOTREG : DIERR_NOTINITIALIZED;
}
static HRESULT enum_devices(DirectInput *d, DWORD type, void *callback, DWORD flags) {
  if (!callback || (flags & ~DIEDFL_VALID_) != 0) return DIERR_INVALIDPARAM;
  if (type > DI8DEVCLASS_GAMECTRL && (type & 0xFF) < DI8DEVTYPE_DEVICE) return DIERR_INVALIDPARAM;      /* neither a class nor a device type */
  if (!d->initialized) return DIERR_NOTINITIALIZED;
  return DI_OK;      /* (no device to tell the callback about) */
}
static HRESULT device_status(DirectInput *d, const GUID *guid) {
  if (!guid) return E_POINTER;
  return d->initialized ? DIERR_DEVICENOTREG : DIERR_NOTINITIALIZED;
}
static HRESULT initialize(DirectInput *d, HINSTANCE instance, DWORD version) {
  const HRESULT hr = check_version(instance, version);
  if (SUCCEEDED(hr)) d->initialized = TRUE;
  return hr;
}
static HRESULT find_device(DirectInput *d, const GUID *guid, const void *name, GUID *instance) {
  if (!guid || !name || !instance) return E_POINTER;
  return d->initialized ? DIERR_DEVICENOTREG : DIERR_NOTINITIALIZED;
}
static HRESULT enum_semantics(DirectInput *d, void *format, void *callback, DWORD flags) {
  (void)flags;
  if (!format || !callback) return DIERR_INVALIDPARAM;
  return d->initialized ? DI_OK : DIERR_NOTINITIALIZED;
}

/* The A table. */
static HRESULT WINAPI a_query(IDirectInput8A *i, REFIID riid, void **out) { return query(from_a(i), riid, out); }
static ULONG WINAPI a_add_ref(IDirectInput8A *i) { return add_ref(from_a(i)); }
static ULONG WINAPI a_release(IDirectInput8A *i) { return release(from_a(i)); }
static HRESULT WINAPI a_create_device(IDirectInput8A *i, REFGUID g, LPDIRECTINPUTDEVICE8A *dev, LPUNKNOWN outer) { return create_device(from_a(i), g, (void **)dev, outer); }
static HRESULT WINAPI a_enum_devices(IDirectInput8A *i, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ctx, DWORD flags) { (void)ctx; return enum_devices(from_a(i), type, (void *)cb, flags); }
static HRESULT WINAPI a_get_device_status(IDirectInput8A *i, REFGUID g) { return device_status(from_a(i), g); }
static HRESULT WINAPI a_run_control_panel(IDirectInput8A *i, HWND owner, DWORD flags) { (void)i; (void)owner; return flags ? DIERR_INVALIDPARAM : DI_OK; }
static HRESULT WINAPI a_initialize(IDirectInput8A *i, HINSTANCE inst, DWORD version) { return initialize(from_a(i), inst, version); }
static HRESULT WINAPI a_find_device(IDirectInput8A *i, REFGUID g, LPCSTR name, LPGUID out) { return find_device(from_a(i), g, name, out); }
static HRESULT WINAPI a_enum_semantics(IDirectInput8A *i, LPCSTR user, LPDIACTIONFORMATA format, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ctx, DWORD flags) {
  (void)user; (void)ctx;
  return enum_semantics(from_a(i), format, (void *)cb, flags);
}
static HRESULT WINAPI a_configure_devices(IDirectInput8A *i, LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA params, DWORD flags, LPVOID ctx) {
  (void)i; (void)cb; (void)params; (void)flags; (void)ctx;
  return DIERR_UNSUPPORTED;
}
static const IDirectInput8AVtbl kVtblA = {a_query, a_add_ref, a_release, a_create_device, a_enum_devices, a_get_device_status, a_run_control_panel, a_initialize, a_find_device,
                                          a_enum_semantics, a_configure_devices};

/* The W table. */
static HRESULT WINAPI w_query(IDirectInput8W *i, REFIID riid, void **out) { return query(from_w(i), riid, out); }
static ULONG WINAPI w_add_ref(IDirectInput8W *i) { return add_ref(from_w(i)); }
static ULONG WINAPI w_release(IDirectInput8W *i) { return release(from_w(i)); }
static HRESULT WINAPI w_create_device(IDirectInput8W *i, REFGUID g, LPDIRECTINPUTDEVICE8W *dev, LPUNKNOWN outer) { return create_device(from_w(i), g, (void **)dev, outer); }
static HRESULT WINAPI w_enum_devices(IDirectInput8W *i, DWORD type, LPDIENUMDEVICESCALLBACKW cb, LPVOID ctx, DWORD flags) { (void)ctx; return enum_devices(from_w(i), type, (void *)cb, flags); }
static HRESULT WINAPI w_get_device_status(IDirectInput8W *i, REFGUID g) { return device_status(from_w(i), g); }
static HRESULT WINAPI w_run_control_panel(IDirectInput8W *i, HWND owner, DWORD flags) { (void)i; (void)owner; return flags ? DIERR_INVALIDPARAM : DI_OK; }
static HRESULT WINAPI w_initialize(IDirectInput8W *i, HINSTANCE inst, DWORD version) { return initialize(from_w(i), inst, version); }
static HRESULT WINAPI w_find_device(IDirectInput8W *i, REFGUID g, LPCWSTR name, LPGUID out) { return find_device(from_w(i), g, name, out); }
static HRESULT WINAPI w_enum_semantics(IDirectInput8W *i, LPCWSTR user, LPDIACTIONFORMATW format, LPDIENUMDEVICESBYSEMANTICSCBW cb, LPVOID ctx, DWORD flags) {
  (void)user; (void)ctx;
  return enum_semantics(from_w(i), format, (void *)cb, flags);
}
static HRESULT WINAPI w_configure_devices(IDirectInput8W *i, LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSW params, DWORD flags, LPVOID ctx) {
  (void)i; (void)cb; (void)params; (void)flags; (void)ctx;
  return DIERR_UNSUPPORTED;
}
static const IDirectInput8WVtbl kVtblW = {w_query, w_add_ref, w_release, w_create_device, w_enum_devices, w_get_device_status, w_run_control_panel, w_initialize, w_find_device,
                                          w_enum_semantics, w_configure_devices};

EXPORT HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID *out, LPUNKNOWN outer) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!riid || !(same_guid(riid, &kIidUnknown) || same_guid(riid, &kIidDirectInput8A) || same_guid(riid, &kIidDirectInput8W))) return E_NOINTERFACE;
  const HRESULT valid = check_version(instance, version);
  if (FAILED(valid)) return valid;
  DirectInput *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
  if (!d) return DIERR_OUTOFMEMORY;
  d->vtbl_a = &kVtblA;
  d->vtbl_w = &kVtblW;
  d->refs = 1;
  d->initialized = TRUE;
  const HRESULT hr = query(d, riid, out);
  release(d);
  return hr;
}

/* The class factory of CLSID_DirectInput8 (CoCreateInstance, as SDL does): the object it makes wants Initialize before use. */
static const GUID kClsidDirectInput8 = {0x25E609E4, 0xB259, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kIidClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static HRESULT WINAPI cf_query(IClassFactory *self, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (riid && (same_guid(riid, &kIidUnknown) || same_guid(riid, &kIidClassFactory))) { *out = self; return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI cf_add_ref(IClassFactory *self) { (void)self; return 2; }      /* (a static object) */
static ULONG WINAPI cf_release(IClassFactory *self) { (void)self; return 1; }
static HRESULT WINAPI cf_create(IClassFactory *self, IUnknown *outer, REFIID riid, void **out) {
  (void)self;
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  DirectInput *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
  if (!d) return E_OUTOFMEMORY;
  d->vtbl_a = &kVtblA;
  d->vtbl_w = &kVtblW;
  d->refs = 1;      /* (not initialized: the program calls Initialize) */
  const HRESULT hr = query(d, riid, out);
  release(d);
  return hr;
}
static HRESULT WINAPI cf_lock(IClassFactory *self, BOOL lock) { (void)self; (void)lock; return S_OK; }
static const IClassFactoryVtbl kFactoryVtbl = {cf_query, cf_add_ref, cf_release, cf_create, cf_lock};
static IClassFactory g_factory = {(IClassFactoryVtbl *)&kFactoryVtbl};

EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!clsid || !same_guid(clsid, &kClsidDirectInput8)) return CLASS_E_CLASSNOTAVAILABLE;
  return cf_query(&g_factory, riid, out);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reason; (void)reserved;
  return TRUE;
}
