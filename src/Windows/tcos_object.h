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

/* The identity of the Direct3D family's own objects (dxgi.dll, d3d12.dll, d3d11.dll, d3d10*.dll, d3d9.dll, d3d8.dll, ddraw.dll).
 *
 * A pointer a program hands to one of these libraries is not always an object of the library: a proxy (ReShade, an overlay, a frame limiter) wraps the runtime's
 * objects in its own and hands those out, and a foreign runtime (DXVK, vkd3d-proton, a program's own Direct3D 12 core) has objects of its own layout. Only the
 * public interface of such an object may be used. Where a library needs the internals of an object (its own struct, a private export of the runtime below), it
 * first makes sure the object is its own:
 *   1. the vtable is the library's (the fast path: the program passed the runtime's object itself);
 *   2. else QueryInterface(IID_ITcosObject): every object of these libraries answers it with its canonical interface pointer (the object itself, AddRef'd), a
 *      proxy that forwards IIDs it does not know (as ReShade does) hands back the object it wraps, a foreign object answers E_NOINTERFACE; the vtable of what came
 *      back is checked again;
 *   3. else the call fails with what Windows answers for an invalid argument there (E_INVALIDARG, D3DERR_INVALIDCALL, DXGI_ERROR_INVALID_CALL), never touching
 *      the object's memory.
 * tcos_own() does 1 and 2. It returns a borrowed pointer: the caller's own reference to the wrapper keeps the wrapped object alive (a proxy holds its object). */
#ifndef TCOS_OBJECT_H
#define TCOS_OBJECT_H

#include <windows.h>
#include <unknwn.h>

/* {7c0e5a10-2d5e-4a5d-9a7e-54434f530102} */
static const GUID IID_ITcosObject __attribute__((unused)) = {0x7c0e5a10, 0x2d5e, 0x4a5d, {0x9a, 0x7e, 0x54, 0x43, 0x4f, 0x53, 0x01, 0x02}};

static inline int tcos_is_object_iid(REFIID riid) {
  const unsigned char *a = (const unsigned char *)riid, *b = (const unsigned char *)&IID_ITcosObject;
  for (unsigned i = 0; i < sizeof(GUID); ++i)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* The library's own object behind `object` when its vtable is `vtbl` (directly, or through IID_ITcosObject), else NULL. Borrowed: no reference is added. */
static inline void *tcos_own(const void *object, const void *vtbl) {
  if (!object) return NULL;
  IUnknown *u = (IUnknown *)object;
  if ((const void *)u->lpVtbl == vtbl) return u;
  IUnknown *real = NULL;
  if (FAILED(u->lpVtbl->QueryInterface(u, &IID_ITcosObject, (void **)&real)) || !real) return NULL;
  const int ours = (const void *)real->lpVtbl == vtbl;
  real->lpVtbl->Release(real);      /* (the wrapper's reference keeps it) */
  return ours ? real : NULL;
}
/* The same for an object that may have one of several vtables (`count` of them). */
static inline void *tcos_own_any(const void *object, const void *const *vtbls, unsigned count) {
  if (!object) return NULL;
  IUnknown *u = (IUnknown *)object;
  for (unsigned i = 0; i < count; ++i)
    if ((const void *)u->lpVtbl == vtbls[i]) return u;
  IUnknown *real = NULL;
  if (FAILED(u->lpVtbl->QueryInterface(u, &IID_ITcosObject, (void **)&real)) || !real) return NULL;
  void *found = NULL;
  for (unsigned i = 0; i < count; ++i)
    if ((const void *)real->lpVtbl == vtbls[i]) found = real;
  real->lpVtbl->Release(real);
  return found;
}
/* The canonical object behind `object` (one of this family's, whatever its kind), or NULL for a foreign one. Borrowed. */
static inline IUnknown *tcos_canonical(const void *object) {
  if (!object) return NULL;
  IUnknown *u = (IUnknown *)object, *real = NULL;
  if (FAILED(u->lpVtbl->QueryInterface(u, &IID_ITcosObject, (void **)&real)) || !real) return NULL;
  real->lpVtbl->Release(real);
  return real;
}

#endif
