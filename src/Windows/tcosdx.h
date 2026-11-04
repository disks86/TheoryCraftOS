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

/* The one interface dxgi.dll and the Direct3D runtimes (d3d12.dll) share beyond the public ones. On Windows a swap chain asks the D3D device for its buffers and presents through
 * the display driver; here dxgi.dll's swap chain asks the object it was made with (the command queue of Direct3D 12: QueryInterface for this) to make the back buffers, and to
 * hand over the pixels of one, which dxgi.dll puts in the window. */
#ifndef TCOS_TCOSDX_H
#define TCOS_TCOSDX_H

#include <windows.h>
#include <dxgi.h>

/* {7c0e5a10-2d5e-4a5d-9a7e-54434f530101} */
static const GUID IID_ITcosPresenter = {0x7c0e5a10, 0x2d5e, 0x4a5d, {0x9a, 0x7e, 0x54, 0x43, 0x4f, 0x53, 0x01, 0x01}};

typedef struct ITcosPresenter ITcosPresenter;
typedef struct ITcosPresenterVtbl {
  HRESULT(STDMETHODCALLTYPE *QueryInterface)(ITcosPresenter *This, REFIID riid, void **ppv);
  ULONG(STDMETHODCALLTYPE *AddRef)(ITcosPresenter *This);
  ULONG(STDMETHODCALLTYPE *Release)(ITcosPresenter *This);
  /* `count` back buffers of the size and format (DXGI_FORMAT), as objects of the runtime's resource interface; out[i] is owned by the caller. */
  HRESULT(STDMETHODCALLTYPE *CreateBackBuffers)(ITcosPresenter *This, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT usage, IUnknown **out);
  /* Waits until the work queued before is done, then copies the pixels of a back buffer as B8G8R8A8 rows (`pitch` bytes apart, top row first) into `dst` (width x height). */
  HRESULT(STDMETHODCALLTYPE *ReadBackBuffer)(ITcosPresenter *This, IUnknown *buffer, void *dst, UINT pitch);
} ITcosPresenterVtbl;
struct ITcosPresenter { const ITcosPresenterVtbl *lpVtbl; };

#endif
