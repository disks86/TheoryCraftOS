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

/* Part of d3d9.c: swap chains (IDirect3DSwapChain9 and 9Ex) - the device's implicit one and additional ones - Present, the front buffer, the raster status
 * and the display modes, the gamma ramp, the cursor.
 *
 * A swap chain's back buffers and its front buffer are Direct3D 12 textures of the queue's (ITcosPresenter::CreateBackBuffers). Present reads the back buffer
 * to show (ReadBackBuffer: it waits for the work queued before), puts its pixels into the window with StretchDIBits - a part of it into a part of the window,
 * through the dirty region, for D3DSWAPEFFECT_COPY - and then, for D3DSWAPEFFECT_FLIP and DISCARD, the buffers move on: the back buffer shown is the front buffer
 * now, each back buffer has the pixels of the one after it, the last the old front buffer's (the surfaces stay; the textures behind them change places). The
 * presentation interval waits for the display's vertical blank (D3DKMTWaitForVerticalBlankEvent). */

/* ---- the display ------------------------------------------------------------------------------------------------------------------------------------ */

/* The display format of so many bits a pixel. */
static D3DFORMAT format_of_bits(DWORD bits) { return bits == 16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8; }
/* The display mode the desktop has now. */
static void display_current(D3DDISPLAYMODEEX *mode) {
  DEVMODEW dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  memset(mode, 0, sizeof *mode);
  mode->Size = sizeof *mode;
  if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
    mode->Width = dm.dmPelsWidth;
    mode->Height = dm.dmPelsHeight;
    mode->RefreshRate = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
    mode->Format = format_of_bits(dm.dmBitsPerPel);
  } else {
    mode->Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
    mode->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
    mode->RefreshRate = 60;
    mode->Format = D3DFMT_X8R8G8B8;
  }
  mode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
}
/* How the display is turned (its DEVMODE's orientation), as Direct3D 9Ex names it. */
static D3DDISPLAYROTATION display_rotation(void) {
  DEVMODEW dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) return D3DDISPLAYROTATION_IDENTITY;
  return dm.dmDisplayOrientation == DMDO_90 ? D3DDISPLAYROTATION_90 : dm.dmDisplayOrientation == DMDO_180 ? D3DDISPLAYROTATION_180
       : dm.dmDisplayOrientation == DMDO_270 ? D3DDISPLAYROTATION_270 : D3DDISPLAYROTATION_IDENTITY;
}
static void display_mode_plain(const D3DDISPLAYMODEEX *from, D3DDISPLAYMODE *to) {
  to->Width = from->Width;
  to->Height = from->Height;
  to->RefreshRate = from->RefreshRate;
  to->Format = from->Format;
}
/* The `index`th display mode of a format (the display's modes of its bits, each size and refresh rate once, in the order the display gives them). FALSE: there
 * is none. The modes of 32 bits are also those of X8R8G8B8 and A8R8G8B8's... a display mode is X8R8G8B8, R5G6B5 or X1R5G5B5. */
static BOOL display_mode_at(D3DFORMAT format, UINT index, D3DDISPLAYMODEEX *out) {
  if (!display_format(format)) return FALSE;
  const DWORD bits = display_bits(format);
  UINT found = 0;
  DEVMODEW dm, seen;
  for (DWORD i = 0;; ++i) {
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(NULL, i, &dm)) break;
    /* (a display that lists no 16-bit modes still shows them: every size it has is a mode of each format here) */
    if (dm.dmBitsPerPel && dm.dmBitsPerPel != bits && dm.dmBitsPerPel != 32) continue;
    BOOL again = FALSE;
    for (DWORD k = 0; k < i && !again; ++k) {
      memset(&seen, 0, sizeof seen);
      seen.dmSize = sizeof seen;
      if (EnumDisplaySettingsW(NULL, k, &seen) && seen.dmPelsWidth == dm.dmPelsWidth && seen.dmPelsHeight == dm.dmPelsHeight &&
          seen.dmDisplayFrequency == dm.dmDisplayFrequency && (!seen.dmBitsPerPel || seen.dmBitsPerPel == bits || seen.dmBitsPerPel == 32))
        again = TRUE;
    }
    if (again) continue;
    if (found++ == index) {
      memset(out, 0, sizeof *out);
      out->Size = sizeof *out;
      out->Width = dm.dmPelsWidth;
      out->Height = dm.dmPelsHeight;
      out->RefreshRate = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
      out->Format = format;
      out->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      return TRUE;
    }
  }
  /* (a display that enumerates nothing has its current mode) */
  if (index == 0 && found == 0) {
    display_current(out);
    out->Format = format;
    return TRUE;
  }
  return FALSE;
}
static UINT display_mode_count(D3DFORMAT format) {
  D3DDISPLAYMODEEX mode;
  UINT n = 0;
  while (display_mode_at(format, n, &mode)) ++n;
  return n;
}
/* Whether a full-screen swap chain may have a size and refresh rate (0: any) in a format: a display mode of it. */
static BOOL display_has_mode(D3DFORMAT format, UINT width, UINT height, UINT refresh) {
  D3DDISPLAYMODEEX mode;
  for (UINT i = 0; display_mode_at(format, i, &mode); ++i)
    if (mode.Width == width && mode.Height == height && (!refresh || mode.RefreshRate == refresh)) return TRUE;
  return FALSE;
}
/* Sets the display mode (full screen); NULL: the one the desktop is registered with. */
static BOOL display_set(const D3DDISPLAYMODEEX *mode) {
  if (!mode) return ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL) == DISP_CHANGE_SUCCESSFUL;
  D3DDISPLAYMODEEX now;
  display_current(&now);
  if (now.Width == mode->Width && now.Height == mode->Height && (now.RefreshRate == mode->RefreshRate || !mode->RefreshRate)) return TRUE;
  DEVMODEW dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  dm.dmPelsWidth = mode->Width;
  dm.dmPelsHeight = mode->Height;
  dm.dmBitsPerPel = display_bits(mode->Format);
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
  if (mode->RefreshRate) { dm.dmDisplayFrequency = mode->RefreshRate; dm.dmFields |= DM_DISPLAYFREQUENCY; }
  return ChangeDisplaySettingsExW(NULL, &dm, NULL, CDS_FULLSCREEN, NULL) == DISP_CHANGE_SUCCESSFUL;
}

/* ---- swap chains ------------------------------------------------------------------------------------------------------------------------------------ */

static SwapChain *chain_of(void *p) { return p && ((DxObj *)p)->lpVtbl == (const void *)&chain_vtbl ? p : NULL; }
static void chain_free_buffers(SwapChain *c) {
  for (UINT i = 0; i < MAX_BACK_BUFFERS; ++i) {
    if (c->surface[i]) { obj_free(OBJ(c->surface[i])); c->surface[i] = NULL; }
  }
  for (UINT i = 0; i <= MAX_BACK_BUFFERS; ++i) {
    if (c->buffer[i]) { obj_free(OBJ(c->buffer[i])); c->buffer[i] = NULL; }
  }
  c->count = 0;
}
/* Leaves full screen: the window as it was, and the desktop's display mode. */
static void chain_leave_fullscreen(SwapChain *c);
static void chain_destroy(DxObj *o) {
  SwapChain *c = (SwapChain *)o;
  Dev *d = c->c.dev;
  EnterCriticalSection(&d->lock);
  flush(d);
  chain_free_buffers(c);
  if (!c->pp.Windowed) chain_leave_fullscreen(c);
  LeaveCriticalSection(&d->lock);
  mem_free(c->pixels);
  if (!c->implicit) default_resource_gone(d, D3DPOOL_DEFAULT);
}

/* What a set of present parameters may be (CreateDevice, Reset, CreateAdditionalSwapChain), Direct3D 9's rules: a swap effect of its (and FLIPEX for 9Ex), one
 * back buffer with D3DSWAPEFFECT_COPY, at most 3 (9Ex: 30), a window to show in, a format of the display or of a back buffer, a full-screen size of a display
 * mode, no refresh rate when windowed, an interval the caps have, no multisampling (the device has none). */
static HRESULT present_parameters_check(Dev *d, const D3DPRESENT_PARAMETERS *pp, HWND focus) {
  const BOOL ex = d ? d->ex : FALSE;
  if (pp->SwapEffect < D3DSWAPEFFECT_DISCARD || pp->SwapEffect > (ex ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_COPY) || pp->SwapEffect == D3DSWAPEFFECT_OVERLAY) return D3DERR_INVALIDCALL;
  if (pp->BackBufferCount > (ex ? 30u : 3u)) return D3DERR_INVALIDCALL;
  if (pp->SwapEffect == D3DSWAPEFFECT_COPY && pp->BackBufferCount > 1) return D3DERR_INVALIDCALL;
  if (pp->SwapEffect == D3DSWAPEFFECT_FLIPEX && pp->BackBufferCount < 1) return D3DERR_INVALIDCALL;
  if (!pp->hDeviceWindow && !focus && !(ex && pp->Windowed)) return D3DERR_INVALIDCALL;      /* (a windowed 9Ex device may have no window: PresentEx names one) */
  if (pp->MultiSampleType != D3DMULTISAMPLE_NONE) return D3DERR_INVALIDCALL;
  if (pp->Flags & ~(DWORD)(D3DPRESENTFLAG_LOCKABLE_BACKBUFFER | D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL | D3DPRESENTFLAG_DEVICECLIP | D3DPRESENTFLAG_VIDEO |
                           D3DPRESENTFLAG_NOAUTOROTATE | D3DPRESENTFLAG_UNPRUNEDMODE | D3DPRESENTFLAG_OVERLAY_LIMITEDRGB | D3DPRESENTFLAG_OVERLAY_YCbCr_BT709 |
                           D3DPRESENTFLAG_OVERLAY_YCbCr_xvYCC | D3DPRESENTFLAG_RESTRICTED_CONTENT | D3DPRESENTFLAG_RESTRICT_SHARED_RESOURCE_DRIVER))
    return D3DERR_INVALIDCALL;
  const UINT interval = pp->PresentationInterval;
  if (interval != D3DPRESENT_INTERVAL_DEFAULT && interval != D3DPRESENT_INTERVAL_ONE && interval != D3DPRESENT_INTERVAL_IMMEDIATE &&
      (pp->Windowed || (interval != D3DPRESENT_INTERVAL_TWO && interval != D3DPRESENT_INTERVAL_THREE && interval != D3DPRESENT_INTERVAL_FOUR)))
    return D3DERR_INVALIDCALL;
  if (pp->Windowed) {
    if (pp->FullScreen_RefreshRateInHz) return D3DERR_INVALIDCALL;
    if (pp->BackBufferFormat != D3DFMT_UNKNOWN && !backbuffer_format(pp->BackBufferFormat)) return D3DERR_INVALIDCALL;
  } else {
    if (!backbuffer_format(pp->BackBufferFormat)) return D3DERR_INVALIDCALL;
    const D3DFORMAT display = pp->BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat == D3DFMT_A1R5G5B5 ? D3DFMT_X1R5G5B5 : pp->BackBufferFormat;
    if (!display_has_mode(display, pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz)) return D3DERR_INVALIDCALL;
  }
  if (pp->EnableAutoDepthStencil && !format_is_depth(pp->AutoDepthStencilFormat)) return D3DERR_INVALIDCALL;
  return D3D_OK;
}

/* Full screen: the window covers the whole display, in the display mode of the back buffers, made visible and topmost with its own style kept, as Windows does
 * it (Wine's test_window_style holds what Windows does); D3DCREATE_NOWINDOWCHANGES leaves the window alone. Back to a window: the style stays (visible), the
 * topmost extended style goes again if the window was visible when it went full screen (and stays if it was not, as on Windows). */
static LONG g_saved_style, g_saved_exstyle;
static RECT g_saved_rect;
static HWND g_fullscreen_window;
static HRESULT chain_enter_fullscreen(SwapChain *c) {
  Dev *d = c->c.dev;
  if (!display_set(&c->mode)) return D3DERR_NOTAVAILABLE;
  if (!(d->creation.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES) && c->window && c->window != g_fullscreen_window) {
    g_saved_style = GetWindowLongW(c->window, GWL_STYLE);
    g_saved_exstyle = GetWindowLongW(c->window, GWL_EXSTYLE);
    GetWindowRect(c->window, &g_saved_rect);
    g_fullscreen_window = c->window;
    SetWindowPos(c->window, HWND_TOPMOST, 0, 0, (int)c->mode.Width, (int)c->mode.Height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  }
  /* (a full-screen device's window is in front: it is not lost, nor occluded, when it is made) */
  HWND focus = d->creation.hFocusWindow ? d->creation.hFocusWindow : c->window;
  if (focus && GetForegroundWindow() != focus) SetForegroundWindow(focus);
  return D3D_OK;
}
static void chain_leave_fullscreen(SwapChain *c) {
  if (g_fullscreen_window && g_fullscreen_window == c->window) {
    const BOOL was_visible = (g_saved_style & WS_VISIBLE) != 0;
    SetWindowPos(c->window, was_visible ? HWND_NOTOPMOST : NULL, g_saved_rect.left, g_saved_rect.top, g_saved_rect.right - g_saved_rect.left,
                 g_saved_rect.bottom - g_saved_rect.top, SWP_NOACTIVATE | (was_visible ? 0u : (UINT)SWP_NOZORDER));
    g_fullscreen_window = NULL;
  }
  display_set(NULL);
}

/* Makes the buffers of a swap chain of `pp` (the sizes and formats worked out into it): count back buffers and a front buffer. */
static HRESULT chain_make_buffers(Dev *d, SwapChain *c) {
  D3DPRESENT_PARAMETERS *pp = &c->pp;
  ITcosPresenter *presenter = NULL;
  IUnknown *made[MAX_BACK_BUFFERS + 1];
  memset(made, 0, sizeof made);
  if (FAILED(ID3D12CommandQueue_QueryInterface(d->queue, &IID_ITcosPresenter, (void **)&presenter))) return D3DERR_NOTAVAILABLE;
  HRESULT hr = presenter->lpVtbl->CreateBackBuffers(presenter, pp->BackBufferCount + 1, pp->BackBufferWidth, pp->BackBufferHeight, DXGI_FORMAT_B8G8R8A8_UNORM, 0, made);
  presenter->lpVtbl->Release(presenter);
  if (FAILED(hr)) return hr == E_OUTOFMEMORY ? D3DERR_OUTOFVIDEOMEMORY : D3DERR_NOTAVAILABLE;
  for (UINT i = 0; i <= pp->BackBufferCount; ++i) {
    Tex *t = NULL;
    if (SUCCEEDED(hr))
      hr = tex_make(d, pp->BackBufferWidth, pp->BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, pp->BackBufferFormat, D3DPOOL_DEFAULT,
                    TEX_IMPLICIT | ((pp->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) ? TEX_LOCKABLE : 0), (ID3D12Resource *)made[i], &t);
    if (made[i]) made[i]->lpVtbl->Release(made[i]);
    c->buffer[i] = t;
  }
  if (FAILED(hr)) { chain_free_buffers(c); return hr; }
  c->count = pp->BackBufferCount;
  for (UINT i = 0; i < c->count; ++i) {
    c->surface[i] = surface_make(d, c->buffer[i], SURFACE_IMPLICIT, OBJ(c));
    if (!c->surface[i]) { chain_free_buffers(c); return E_OUTOFMEMORY; }
  }
  return D3D_OK;
}
/* A swap chain of the present parameters, which get what was worked out: the window's size where 0, the display's format where unknown, one back buffer for 0,
 * the focus window for none. `implicit`: the device's own (no reference of its own: it goes with the device). */
static HRESULT chain_create(Dev *d, D3DPRESENT_PARAMETERS *pp, int implicit, SwapChain **out) {
  *out = NULL;
  HRESULT hr = present_parameters_check(d, pp, d->creation.hFocusWindow);
  if (FAILED(hr)) return hr;
  if (!implicit && !pp->Windowed) return D3DERR_INVALIDCALL;      /* (an additional swap chain is windowed) */
  SwapChain *c = child_new_ex(sizeof *c, d, &chain_vtbl, d->ex ? g_chain_ex_iids : g_chain_iids, chain_destroy, implicit);
  if (!c) return E_OUTOFMEMORY;
  c->implicit = implicit;
  c->pp = *pp;
  if (!c->pp.hDeviceWindow) c->pp.hDeviceWindow = d->creation.hFocusWindow;
  c->window = c->pp.hDeviceWindow;
  if (!c->pp.BackBufferCount) c->pp.BackBufferCount = 1;
  display_current(&c->mode);
  if (c->pp.Windowed) {
    if (!c->pp.BackBufferWidth || !c->pp.BackBufferHeight) {
      RECT client = {0, 0, 0, 0};
      if (c->window) GetClientRect(c->window, &client);
      if (!c->pp.BackBufferWidth) c->pp.BackBufferWidth = client.right > client.left ? (UINT)(client.right - client.left) : 1;
      if (!c->pp.BackBufferHeight) c->pp.BackBufferHeight = client.bottom > client.top ? (UINT)(client.bottom - client.top) : 1;
    }
    if (c->pp.BackBufferFormat == D3DFMT_UNKNOWN) c->pp.BackBufferFormat = c->mode.Format;
  } else {
    c->mode.Width = c->pp.BackBufferWidth;
    c->mode.Height = c->pp.BackBufferHeight;
    if (c->pp.FullScreen_RefreshRateInHz) c->mode.RefreshRate = c->pp.FullScreen_RefreshRateInHz;
    c->mode.Format = c->pp.BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : c->pp.BackBufferFormat == D3DFMT_A1R5G5B5 ? D3DFMT_X1R5G5B5 : c->pp.BackBufferFormat;
  }
  if (c->pp.BackBufferWidth > 4096 || c->pp.BackBufferHeight > 4096) hr = D3DERR_INVALIDCALL;
  for (UINT i = 0; i < 256; ++i) c->ramp.red[i] = c->ramp.green[i] = c->ramp.blue[i] = (WORD)(i * 257);
  if (SUCCEEDED(hr) && !c->pp.Windowed) hr = chain_enter_fullscreen(c);
  if (SUCCEEDED(hr)) hr = chain_make_buffers(d, c);
  if (FAILED(hr)) {
    if (!implicit) { c->c.o.ref = 0; obj_free(OBJ(c)); dx_Release(OBJ(d)); }
    else obj_free(OBJ(c));
    return hr;
  }
  if (!implicit) default_resource_made(d, D3DPOOL_DEFAULT);
  /* (the program's parameters get what was worked out, but for the window: GetPresentParameters gives that) */
  const HWND given = pp->hDeviceWindow;
  *pp = c->pp;
  pp->hDeviceWindow = given;
  *out = c;
  return D3D_OK;
}

/* Waits for the display's vertical blank so many times (the presentation interval). */
static void wait_for_vblank(Dev *d, UINT count) {
  UINT adapter = 0;
  if (!KMT_OK(kmt_open_adapter(ID3D12Device_GetAdapterLuid(d->d12), &adapter))) return;
  for (UINT i = 0; i < count; ++i) {
    BYTE wait[12] __attribute__((aligned(8))) = {0};      /* D3DKMT_WAITFORVERTICALBLANKEVENT { hAdapter @0; hDevice @4; VidPnSourceId @8 } */
    KPUT32(wait, 0, adapter);
    if (!KMT_OK(D3DKMTWaitForVerticalBlankEvent(wait))) break;
  }
  kmt_close_adapter(adapter);
}
/* The flips of FLIP and DISCARD: the front buffer is the one shown, each back buffer takes the one after it, the last the old front buffer. */
static void chain_flip(Dev *d, SwapChain *c) {
  if (c->pp.SwapEffect == D3DSWAPEFFECT_COPY || !c->count) return;
  ID3D12Resource *front = c->buffer[c->count]->res;
  c->buffer[c->count]->res = c->buffer[0]->res;
  for (UINT i = 0; i + 1 < c->count; ++i) c->buffer[i]->res = c->buffer[i + 1]->res;
  c->buffer[c->count - 1]->res = front;
  for (UINT i = 0; i < c->count; ++i) {
    tex_make_view(d, c->buffer[i]);
    ++c->buffer[i]->generation;
    for (UINT s = 0; s < MAX_SUBS; ++s) { mem_free(c->buffer[i]->shadow[s]); c->buffer[i]->shadow[s] = NULL; }
  }
  d->targets_dirty = 1;
  d->tables_valid = 0;
}
/* Shows the first back buffer: `src` of it (NULL: all) into `dst` of the window (NULL: its client area), only the parts the dirty region names (COPY). */
static HRESULT chain_present(SwapChain *c, const RECT *src, const RECT *dst, HWND override, const RGNDATA *dirty, DWORD flags) {
  Dev *d = c->c.dev;
  (void)flags;
  if (!c->count) return D3DERR_INVALIDCALL;
  const HWND window = override ? override : c->window;
  if (!window) {      /* (a 9Ex device with no window, presenting to none: the buffers go round, nothing is shown) */
    EnterCriticalSection(&d->lock);
    flush(d);
    chain_flip(d, c);
    LeaveCriticalSection(&d->lock);
    return D3D_OK;
  }
  const UINT w = c->pp.BackBufferWidth, h = c->pp.BackBufferHeight;
  if (!c->pixels || c->pixel_bytes < (SIZE_T)w * h * 4) {
    mem_free(c->pixels);
    c->pixels = mem_alloc((SIZE_T)w * h * 4);
    c->pixel_bytes = c->pixels ? (SIZE_T)w * h * 4 : 0;
    if (!c->pixels) return E_OUTOFMEMORY;
  }
  ITcosPresenter *presenter = NULL;
  if (FAILED(ID3D12CommandQueue_QueryInterface(d->queue, &IID_ITcosPresenter, (void **)&presenter))) return D3DERR_DEVICELOST;
  EnterCriticalSection(&d->lock);
  flush(d);
  HRESULT hr = presenter->lpVtbl->ReadBackBuffer(presenter, (IUnknown *)c->buffer[0]->res, c->pixels, w * 4);
  if (SUCCEEDED(hr)) chain_flip(d, c);
  LeaveCriticalSection(&d->lock);
  presenter->lpVtbl->Release(presenter);
  if (FAILED(hr)) return D3DERR_DEVICELOST;
  /* (a gamma ramp of a full-screen swap chain: the pixels through it) */
  if (!c->pp.Windowed && c->ramp_set)
    for (SIZE_T i = 0; i < (SIZE_T)w * h; ++i) {
      BYTE *p = c->pixels + i * 4;
      p[0] = (BYTE)(c->ramp.blue[p[0]] >> 8);
      p[1] = (BYTE)(c->ramp.green[p[1]] >> 8);
      p[2] = (BYTE)(c->ramp.red[p[2]] >> 8);
    }
  RECT s = {0, 0, (LONG)w, (LONG)h}, t;
  if (src && c->pp.Windowed) s = *src;
  if (!window || !GetClientRect(window, &t)) SetRect(&t, 0, 0, (int)w, (int)h);
  if (!c->pp.Windowed) SetRect(&t, 0, 0, (int)c->mode.Width, (int)c->mode.Height);
  else if (dst) t = *dst;
  HDC dc = window ? GetDC(window) : NULL;
  if (dc) {
    HRGN clip = NULL;
    if (dirty && c->pp.Windowed && c->pp.SwapEffect == D3DSWAPEFFECT_COPY && dirty->rdh.nCount) {
      /* (the dirty region is in the back buffer's pixels: it goes to the window as the source rectangle does) */
      const RECT *r = (const RECT *)dirty->Buffer;
      for (DWORD i = 0; i < dirty->rdh.nCount; ++i) {
        const LONG sw = s.right - s.left ? s.right - s.left : 1, sh = s.bottom - s.top ? s.bottom - s.top : 1;
        const RECT m = {t.left + (r[i].left - s.left) * (t.right - t.left) / sw, t.top + (r[i].top - s.top) * (t.bottom - t.top) / sh,
                        t.left + (r[i].right - s.left) * (t.right - t.left) / sw, t.top + (r[i].bottom - s.top) * (t.bottom - t.top) / sh};
        HRGN part = CreateRectRgnIndirect(&m);
        if (!clip) clip = part;
        else if (part) { CombineRgn(clip, clip, part, RGN_OR); DeleteObject(part); }
      }
      if (clip) SelectClipRgn(dc, clip);
    }
    BITMAPINFO info;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)w;
    info.bmiHeader.biHeight = -(LONG)h;      /* (top-down) */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (t.right - t.left == s.right - s.left && t.bottom - t.top == s.bottom - s.top)
      SetDIBitsToDevice(dc, t.left, t.top, (DWORD)(s.right - s.left), (DWORD)(s.bottom - s.top), s.left, (int)h - s.bottom, 0, h, c->pixels, &info, DIB_RGB_COLORS);
    else
      StretchDIBits(dc, t.left, t.top, t.right - t.left, t.bottom - t.top, s.left, s.top, s.right - s.left, s.bottom - s.top, c->pixels, &info, DIB_RGB_COLORS, SRCCOPY);
    if (clip) { SelectClipRgn(dc, NULL); DeleteObject(clip); }
    ReleaseDC(window, dc);
  }
  ++c->presents;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  c->last_present_qpc = (UINT64)now.QuadPart;
  /* (the presentation interval: the next vertical blank, or so many) */
  const UINT interval = c->pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : c->pp.PresentationInterval == D3DPRESENT_INTERVAL_TWO ? 2
                      : c->pp.PresentationInterval == D3DPRESENT_INTERVAL_THREE ? 3 : c->pp.PresentationInterval == D3DPRESENT_INTERVAL_FOUR ? 4 : 1;
  if (interval && !(flags & D3DPRESENT_FORCEIMMEDIATE)) wait_for_vblank(d, interval);
  return D3D_OK;
}

static HRESULT device_lost_state(Dev *d);
static HRESULT STDMETHODCALLTYPE chain_Present(SwapChain *c, const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty, DWORD flags) {
  Dev *d = c->c.dev;
  const HRESULT lost = device_lost_state(d);
  if (lost == D3DERR_DEVICELOST || lost == D3DERR_DEVICENOTRESET) return d->ex ? S_PRESENT_OCCLUDED : D3DERR_DEVICELOST;
  if (flags & ~(DWORD)(D3DPRESENT_DONOTWAIT | D3DPRESENT_LINEAR_CONTENT | D3DPRESENT_DONOTFLIP | D3DPRESENT_FLIPRESTART | D3DPRESENT_VIDEO_RESTRICT_TO_MONITOR |
                       D3DPRESENT_UPDATEOVERLAYONLY | D3DPRESENT_HIDEOVERLAY | D3DPRESENT_UPDATECOLORKEY | D3DPRESENT_FORCEIMMEDIATE))
    return D3DERR_INVALIDCALL;
  return chain_present(c, src, dst, window, dirty, flags);
}
/* The front buffer - what is shown - into a surface in system memory of A8R8G8B8: the desktop's size, the window's part of it the picture shown last (full
 * screen: the display, the picture). */
static HRESULT chain_front_buffer(SwapChain *c, Surface *s) {
  Dev *d = c->c.dev;
  D3DDISPLAYMODEEX mode;
  display_current(&mode);
  if (!s || s->tex->format != D3DFMT_A8R8G8B8 || s->tex->pool != D3DPOOL_SYSTEMMEM || sub_width(s->tex, s->sub) != mode.Width || sub_height(s->tex, s->sub) != mode.Height)
    return D3DERR_INVALIDCALL;
  BYTE *out = tex_shadow(s->tex, s->sub);
  if (!out) return E_OUTOFMEMORY;
  const UINT w = mode.Width, h = mode.Height;
  /* (the screen as it is, through GDI; where that cannot be read, black with the picture shown last where the window is) */
  BOOL captured = FALSE;
  HDC screen = GetDC(NULL);
  if (screen) {
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)w;
    info.bmiHeader.biHeight = -(LONG)h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (bitmap && bits) {
      HGDIOBJ old = SelectObject(memory, bitmap);
      captured = BitBlt(memory, 0, 0, (int)w, (int)h, screen, 0, 0, SRCCOPY);
      GdiFlush();
      if (captured) memcpy(out, bits, (SIZE_T)w * h * 4);
      SelectObject(memory, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(NULL, screen);
  }
  if (!captured) {
    memset(out, 0, (SIZE_T)w * h * 4);
    if (c->pixels && c->presents) {
      POINT corner = {0, 0};
      if (c->pp.Windowed && c->window) ClientToScreen(c->window, &corner);
      for (UINT y = 0; y < c->pp.BackBufferHeight; ++y) {
        const LONG oy = corner.y + (LONG)y;
        if (oy < 0 || oy >= (LONG)h) continue;
        for (UINT x = 0; x < c->pp.BackBufferWidth; ++x) {
          const LONG ox = corner.x + (LONG)x;
          if (ox >= 0 && ox < (LONG)w) memcpy(out + ((SIZE_T)oy * w + (SIZE_T)ox) * 4, c->pixels + ((SIZE_T)y * c->pp.BackBufferWidth + x) * 4, 4);
        }
      }
    }
  }
  for (SIZE_T i = 3; i < (SIZE_T)w * h * 4; i += 4) out[i] = 0xFF;
  (void)d;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE chain_GetFrontBufferData(SwapChain *c, IDirect3DSurface9 *surface) {
  Surface *s = surface_of(surface);
  if (!s || s->c.dev != c->c.dev) return D3DERR_INVALIDCALL;
  return chain_front_buffer(c, s);
}
static HRESULT STDMETHODCALLTYPE chain_GetBackBuffer(SwapChain *c, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out) {
  (void)type;      /* (stereo buffers are the same as the mono ones: the type does not count) */
  if (!out) return D3DERR_INVALIDCALL;
  if (index >= c->count) return D3DERR_INVALIDCALL;
  child_AddRef(&c->surface[index]->c);
  *out = (IDirect3DSurface9 *)c->surface[index];
  return D3D_OK;
}
/* Where the display's beam is (D3DKMTGetScanLine: in the blank the scan line is 0, as Direct3D 9 gives it). */
static HRESULT raster_status(Dev *d, D3DRASTER_STATUS *status) {
  if (!status) return D3DERR_INVALIDCALL;
  UINT adapter = 0;
  if (!KMT_OK(kmt_open_adapter(ID3D12Device_GetAdapterLuid(d->d12), &adapter))) return D3DERR_INVALIDCALL;
  BYTE line[16] __attribute__((aligned(8))) = {0};      /* D3DKMT_GETSCANLINE { hAdapter @0; VidPnSourceId @4; InVerticalBlank @8; ScanLine @12 } */
  KPUT32(line, 0, adapter);
  const NTSTATUS st = D3DKMTGetScanLine(line);
  kmt_close_adapter(adapter);
  if (!KMT_OK(st)) return D3DERR_INVALIDCALL;
  const BOOL blank = line[8] != 0;
  status->InVBlank = blank;
  status->ScanLine = blank ? 0 : KGET32(line, 12);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE chain_GetRasterStatus(SwapChain *c, D3DRASTER_STATUS *status) { return raster_status(c->c.dev, status); }
static HRESULT STDMETHODCALLTYPE chain_GetDisplayMode(SwapChain *c, D3DDISPLAYMODE *mode) {
  if (!mode) return D3DERR_INVALIDCALL;
  D3DDISPLAYMODEEX now;
  display_current(&now);
  display_mode_plain(&now, mode);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE chain_GetPresentParameters(SwapChain *c, D3DPRESENT_PARAMETERS *pp) {
  if (!pp) return D3DERR_INVALIDCALL;
  *pp = c->pp;
  return D3D_OK;
}
/* (9Ex) the count of Presents, and what Direct3D 9Ex gives of them (D3DPRESENTSTATS: the counts of what was shown; the times by QueryPerformanceCounter) */
static HRESULT STDMETHODCALLTYPE chain_GetLastPresentCount(SwapChain *c, UINT *count) {
  if (!count) return D3DERR_INVALIDCALL;
  *count = c->presents;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE chain_GetPresentStats(SwapChain *c, D3DPRESENTSTATS *stats) {
  if (!stats) return D3DERR_INVALIDCALL;
  /* (statistics are of full-screen swap chains and FLIPEX ones; a windowed one of the others has none, as on Windows) */
  if (c->pp.Windowed && c->pp.SwapEffect != D3DSWAPEFFECT_FLIPEX) return D3DERR_INVALIDCALL;
  memset(stats, 0, sizeof *stats);
  stats->PresentCount = stats->PresentRefreshCount = stats->SyncRefreshCount = c->presents;
  stats->SyncQPCTime.QuadPart = (LONGLONG)c->last_present_qpc;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE chain_GetDisplayModeEx(SwapChain *c, D3DDISPLAYMODEEX *mode, D3DDISPLAYROTATION *rotation) {
  (void)c;
  if (mode && mode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
  if (mode) display_current(mode);
  if (rotation) *rotation = display_rotation();
  return D3D_OK;
}

/* ---- the device's swap chain methods ---------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_CreateAdditionalSwapChain(Dev *d, D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (!pp || !d->chain || !d->chain->pp.Windowed) return D3DERR_INVALIDCALL;      /* (a full-screen device has no others) */
  SwapChain *c = NULL;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = chain_create(d, pp, 0, &c);
  LeaveCriticalSection(&d->lock);
  *out = (IDirect3DSwapChain9 *)c;
  return hr;
}
/* (only the implicit swap chain is the device's: one, number 0) */
static HRESULT STDMETHODCALLTYPE dev_GetSwapChain(Dev *d, UINT index, IDirect3DSwapChain9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (index != 0 || !d->chain) return D3DERR_INVALIDCALL;
  child_AddRef(&d->chain->c);
  *out = (IDirect3DSwapChain9 *)d->chain;
  return D3D_OK;
}
static UINT STDMETHODCALLTYPE dev_GetNumberOfSwapChains(Dev *d) { return d->chain ? 1 : 0; }
static HRESULT STDMETHODCALLTYPE dev_GetBackBuffer(Dev *d, UINT chain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (chain != 0 || !d->chain) return D3DERR_INVALIDCALL;
  return chain_GetBackBuffer(d->chain, index, type, out);
}
static HRESULT STDMETHODCALLTYPE dev_Present(Dev *d, const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty) {
  if (!d->chain) return D3DERR_INVALIDCALL;
  if (d->in_scene) return D3DERR_INVALIDCALL;
  return chain_Present(d->chain, src, dst, window, dirty, 0);
}
static HRESULT STDMETHODCALLTYPE dev_GetFrontBufferData(Dev *d, UINT chain, IDirect3DSurface9 *surface) {
  if (chain != 0 || !d->chain) return D3DERR_INVALIDCALL;
  return chain_GetFrontBufferData(d->chain, surface);
}
static HRESULT STDMETHODCALLTYPE dev_GetRasterStatus(Dev *d, UINT chain, D3DRASTER_STATUS *status) {
  if (chain != 0) return D3DERR_INVALIDCALL;
  return raster_status(d, status);
}
static HRESULT STDMETHODCALLTYPE dev_GetDisplayMode(Dev *d, UINT chain, D3DDISPLAYMODE *mode) {
  if (chain != 0 || !mode || !d->chain) return D3DERR_INVALIDCALL;
  return chain_GetDisplayMode(d->chain, mode);
}
/* (9Ex) */
static HRESULT STDMETHODCALLTYPE dev_GetDisplayModeEx(Dev *d, UINT chain, D3DDISPLAYMODEEX *mode, D3DDISPLAYROTATION *rotation) {
  if (chain != 0 || !d->chain) return D3DERR_INVALIDCALL;
  return chain_GetDisplayModeEx(d->chain, mode, rotation);
}

/* The gamma ramp of a swap chain: what was set is given back; a full-screen swap chain shows its pixels through it (D3DSGR_CALIBRATE: there is no calibrator
 * here, so the ramp is used as it is). */
static void STDMETHODCALLTYPE dev_SetGammaRamp(Dev *d, UINT chain, DWORD flags, const D3DGAMMARAMP *ramp) {
  (void)flags;
  if (chain != 0 || !ramp || !d->chain) return;
  d->chain->ramp = *ramp;
  d->chain->ramp_set = 0;
  for (UINT i = 0; i < 256 && !d->chain->ramp_set; ++i)
    if (ramp->red[i] != i * 257 || ramp->green[i] != i * 257 || ramp->blue[i] != i * 257) d->chain->ramp_set = 1;
}
static void STDMETHODCALLTYPE dev_GetGammaRamp(Dev *d, UINT chain, D3DGAMMARAMP *ramp) {
  if (!ramp) return;
  if (chain != 0 || !d->chain) { memset(ramp, 0, sizeof *ramp); return; }
  *ramp = d->chain->ramp;
}
/* Dialog boxes over a full-screen device: they are shown as they are here (the picture is put into the window by GDI), so the mode is a check of what it needs:
 * full screen, a lockable back buffer, D3DSWAPEFFECT_DISCARD, no multisampling. */
static HRESULT STDMETHODCALLTYPE dev_SetDialogBoxMode(Dev *d, BOOL enable) {
  if (!d->chain) return D3DERR_INVALIDCALL;
  const D3DPRESENT_PARAMETERS *pp = &d->chain->pp;
  if (enable && (pp->Windowed || !(pp->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) || pp->SwapEffect != D3DSWAPEFFECT_DISCARD || pp->MultiSampleType != D3DMULTISAMPLE_NONE))
    return D3DERR_INVALIDCALL;
  d->dialog_box_mode = enable ? TRUE : FALSE;
  return D3D_OK;
}

/* ---- the cursor ------------------------------------------------------------------------------------------------------------------------------------- */

/* SetCursorProperties: the cursor is the pixels of an A8R8G8B8 surface, its sides powers of two and no larger than the display, with its hot spot inside; it
 * becomes a cursor of the window system (CreateIconIndirect), set while the device shows its cursor. */
static HRESULT STDMETHODCALLTYPE dev_SetCursorProperties(Dev *d, UINT x, UINT y, IDirect3DSurface9 *bitmap) {
  Surface *s = surface_of(bitmap);
  if (!s || s->c.dev != d || s->tex->format != D3DFMT_A8R8G8B8) return D3DERR_INVALIDCALL;
  const UINT w = sub_width(s->tex, s->sub), h = sub_height(s->tex, s->sub);
  D3DDISPLAYMODEEX mode;
  display_current(&mode);
  if ((w & (w - 1)) || (h & (h - 1)) || w > mode.Width || h > mode.Height || x >= w || y >= h) return D3DERR_INVALIDCALL;
  BYTE *pixels = mem_alloc((SIZE_T)w * h * 4);
  if (!pixels) return E_OUTOFMEMORY;
  const RECT all = {0, 0, (LONG)w, (LONG)h};
  EnterCriticalSection(&d->lock);
  HRESULT hr = surface_read(d, s, &all, pixels, w * 4);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { mem_free(pixels); return hr; }
  BITMAPINFO info;
  memset(&info, 0, sizeof info);
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = (LONG)w;
  info.bmiHeader.biHeight = -(LONG)h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void *bits = NULL;
  HDC dc = GetDC(NULL);
  HBITMAP colour = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
  HBITMAP mask = CreateBitmap((int)w, (int)h, 1, 1, NULL);
  if (dc) ReleaseDC(NULL, dc);
  HCURSOR cursor = NULL;
  if (colour && bits && mask) {
    memcpy(bits, pixels, (SIZE_T)w * h * 4);
    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    ii.fIcon = FALSE;
    ii.xHotspot = x;
    ii.yHotspot = y;
    ii.hbmMask = mask;
    ii.hbmColor = colour;
    cursor = (HCURSOR)CreateIconIndirect(&ii);
  }
  if (colour) DeleteObject(colour);
  if (mask) DeleteObject(mask);
  mem_free(pixels);
  if (!cursor) return D3DERR_INVALIDCALL;
  if (d->cursor) DestroyCursor(d->cursor);
  d->cursor = cursor;
  if (d->cursor_shown) SetCursor(d->cursor);
  return D3D_OK;
}
static void STDMETHODCALLTYPE dev_SetCursorPosition(Dev *d, int x, int y, DWORD flags) {
  (void)flags;      /* (D3DCURSOR_IMMEDIATE_UPDATE: the window system's cursor moves at once either way) */
  d->cursor_position.x = x;
  d->cursor_position.y = y;
  if (d->cursor_shown) SetCursorPos(x, y);
}
/* Shows or hides the device's cursor; it answers whether it was shown (which it is only once a cursor was set). */
static BOOL STDMETHODCALLTYPE dev_ShowCursor(Dev *d, BOOL show) {
  const BOOL was = d->cursor_shown;
  if (show && !d->cursor) return was;      /* (no cursor yet: it stays hidden, as Windows has it - the next ShowCursor after SetCursorProperties still answers FALSE) */
  d->cursor_shown = show ? TRUE : FALSE;
  if (d->cursor_shown && d->cursor) SetCursor(d->cursor);
  return was;
}
