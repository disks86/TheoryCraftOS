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

/* Part of d3d9.c: the device itself - its targets, Present, the copies between surfaces, how it is made and reset - the Direct3D object a program
 * asks about the adapter, and the entry points of the library. */

/* ---- what the device can do -------------------------------------------------------------------------------------------------------------------------- */

static void caps_fill(D3DCAPS9 *caps) {
  memset(caps, 0, sizeof *caps);
  caps->DeviceType = D3DDEVTYPE_HAL;
  caps->Caps2 = D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP;
  caps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD;
  caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
  caps->DevCaps = D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                  D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY;
  caps->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP |
                            D3DPMISCCAPS_SEPARATEALPHABLEND | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_PERSTAGECONSTANT;
  caps->MaxActiveLights = FFL_LIGHTS;
  caps->VertexProcessingCaps = D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
  caps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_WFOG;
  caps->ZCmpCaps = caps->AlphaCmpCaps = 0xFF;      /* (all eight comparisons; of the alpha test the ones that keep the higher alphas work) */
  caps->SrcBlendCaps = caps->DestBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA |
                                             D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                                             D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
  caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND;
  caps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_PROJECTED;
  caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                            D3DPTFILTERCAPS_MIPFLINEAR;
  caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_CLAMP;
  caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
  caps->MaxTextureRepeat = 8192;
  caps->MaxTextureAspectRatio = 4096;
  caps->MaxAnisotropy = 1;
  caps->MaxVertexW = 1e10f;
  caps->GuardBandLeft = caps->GuardBandTop = -32768.0f;
  caps->GuardBandRight = caps->GuardBandBottom = 32768.0f;
  caps->StencilCaps = 0xFF;
  caps->FVFCaps = TEXTURE_SETS | D3DFVFCAPS_PSIZE;      /* (the sets of texture coordinates a vertex can have) */
  /* (every operation of the texture stages but the bump environment ones and D3DTOP_PREMODULATE: ff_stages.h) */
  caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
                        D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                        D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA |
                        D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA | D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
                        D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP;
  caps->MaxTextureBlendStages = STAGES;
  caps->MaxSimultaneousTextures = STAGES;
  caps->MaxPrimitiveCount = INDEX_RING / 12;
  caps->MaxVertexIndex = 0x00FFFFFF;
  caps->MaxStreams = STREAMS;
  caps->MaxStreamStride = 508;
  caps->MaxPointSize = 1.0f;
  caps->NumSimultaneousRTs = 1;
  caps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
  /* The program's shaders: what the translator takes (sm_to_dxbc.h), which is not everything of the version here. */
  caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  caps->MaxVertexShaderConst = VS_FLOATS;
  caps->PixelShader1xMaxValue = 65504.0f;
  caps->VS20Caps.DynamicFlowControlDepth = caps->PS20Caps.DynamicFlowControlDepth = 24;
  caps->VS20Caps.NumTemps = caps->PS20Caps.NumTemps = 32;
  caps->PS20Caps.NumInstructionSlots = 512;
  caps->MaxVShaderInstructionsExecuted = caps->MaxPShaderInstructionsExecuted = 65535;
  caps->MaxVertexShader30InstructionSlots = caps->MaxPixelShader30InstructionSlots = 4096;
}

/* ---- the device's own methods ----------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_TestCooperativeLevel(Dev *d) { (void)d; return D3D_OK; }
static UINT STDMETHODCALLTYPE dev_GetAvailableTextureMem(Dev *d) { (void)d; return 256u << 20; }
static HRESULT STDMETHODCALLTYPE dev_EvictManagedResources(Dev *d) { (void)d; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE dev_GetDirect3D(Dev *d, IDirect3D9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  dx_AddRef(OBJ(d->parent));
  *out = (IDirect3D9 *)d->parent;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetDeviceCaps(Dev *d, D3DCAPS9 *caps) {
  (void)d;
  if (!caps) return D3DERR_INVALIDCALL;
  caps_fill(caps);
  return D3D_OK;
}
static void display_mode(D3DDISPLAYMODE *mode) {
  mode->Width = (UINT)GetSystemMetrics(SM_CXSCREEN);
  mode->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
  if (!mode->Width || !mode->Height) { mode->Width = 1024; mode->Height = 768; }
  mode->RefreshRate = 60;
  mode->Format = D3DFMT_X8R8G8B8;
}
static HRESULT STDMETHODCALLTYPE dev_GetDisplayMode(Dev *d, UINT chain, D3DDISPLAYMODE *mode) {
  (void)d;
  if (chain != 0 || !mode) return D3DERR_INVALIDCALL;
  display_mode(mode);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetCreationParameters(Dev *d, D3DDEVICE_CREATION_PARAMETERS *parameters) {
  if (!parameters) return D3DERR_INVALIDCALL;
  *parameters = d->creation;
  return D3D_OK;
}
static void STDMETHODCALLTYPE dev_SetCursorPosition(Dev *d, int x, int y, DWORD flags) { (void)d; (void)x; (void)y; (void)flags; }
static BOOL STDMETHODCALLTYPE dev_ShowCursor(Dev *d, BOOL show) { (void)d; (void)show; return FALSE; }
static UINT STDMETHODCALLTYPE dev_GetNumberOfSwapChains(Dev *d) { (void)d; return 1; }
static HRESULT STDMETHODCALLTYPE dev_SetDialogBoxMode(Dev *d, BOOL enable) { (void)d; (void)enable; return D3D_OK; }
static void STDMETHODCALLTYPE dev_SetGammaRamp(Dev *d, UINT chain, DWORD flags, const D3DGAMMARAMP *ramp) { (void)d; (void)chain; (void)flags; (void)ramp; }
static void STDMETHODCALLTYPE dev_GetGammaRamp(Dev *d, UINT chain, D3DGAMMARAMP *ramp) {
  (void)d; (void)chain;
  if (!ramp) return;
  for (UINT i = 0; i < 256; ++i) ramp->red[i] = ramp->green[i] = ramp->blue[i] = (WORD)(i * 257);
}
static HRESULT STDMETHODCALLTYPE dev_ValidateDevice(Dev *d, DWORD *passes) { (void)d; if (passes) *passes = 1; return D3D_OK; }

/* ---- what is drawn into ----------------------------------------------------------------------------------------------------------------------------- */

static Surface *surface_of(IDirect3DSurface9 *surface) {
  if (!surface || ((DxObj *)surface)->lpVtbl != (const void *)&surf_vtbl) return NULL;
  return (Surface *)surface;
}
static HRESULT STDMETHODCALLTYPE dev_GetBackBuffer(Dev *d, UINT chain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out) {
  (void)type;
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (chain != 0 || index != 0) return D3DERR_INVALIDCALL;
  dx_AddRef(OBJ(d->back_surface));
  *out = (IDirect3DSurface9 *)d->back_surface;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetRenderTarget(Dev *d, DWORD index, IDirect3DSurface9 *surface) {
  Surface *s = surface_of(surface);
  if (index != 0) return surface ? D3DERR_INVALIDCALL : D3D_OK;
  if (!s || !s->tex->target || s->level != 0) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  bind(&d->target, s);
  /* (as Direct3D 9 does: the viewport becomes the whole of the new target) */
  const D3DVIEWPORT9 viewport = {0, 0, s->tex->width, s->tex->height, 0.0f, 1.0f};
  d->state.viewport = viewport;
  SetRect(&d->state.scissor, 0, 0, (int)s->tex->width, (int)s->tex->height);
  d->targets_dirty = 1;
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetRenderTarget(Dev *d, DWORD index, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (index != 0 || !d->target) return D3DERR_NOTFOUND;
  dx_AddRef(OBJ(d->target));
  *out = (IDirect3DSurface9 *)d->target;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetDepthStencilSurface(Dev *d, IDirect3DSurface9 *surface) {
  Surface *s = surface_of(surface);
  if (surface && (!s || !s->tex->depth)) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  bind(&d->depth, s);
  d->targets_dirty = 1;
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetDepthStencilSurface(Dev *d, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (!d->depth) return D3DERR_NOTFOUND;
  dx_AddRef(OBJ(d->depth));
  *out = (IDirect3DSurface9 *)d->depth;
  return D3D_OK;
}

/* ---- copies between surfaces ------------------------------------------------------------------------------------------------------------------------ */

/* The pixels of a surface in the program's memory, there if they were not; NULL when there is no memory for them. */
static BYTE *surface_pixels(Surface *s) {
  Tex *t = s->tex;
  const UINT bytes = format_bytes(t->format);
  if (!bytes) return NULL;
  if (!t->shadow[s->level]) t->shadow[s->level] = mem_alloc((SIZE_T)level_size(t->width, s->level) * level_size(t->height, s->level) * bytes);
  return t->shadow[s->level];
}
/* A target's pixels into a surface in memory of the same size (32 bits a pixel). */
static HRESULT STDMETHODCALLTYPE dev_GetRenderTargetData(Dev *d, IDirect3DSurface9 *from, IDirect3DSurface9 *to) {
  Surface *source = surface_of(from), *dest = surface_of(to);
  if (!source || !dest || !source->tex->target || dest->tex->res || format_bytes(dest->tex->format) != 4) return D3DERR_INVALIDCALL;
  const UINT w = level_size(dest->tex->width, dest->level), h = level_size(dest->tex->height, dest->level);
  if (w != level_size(source->tex->width, source->level) || h != level_size(source->tex->height, source->level)) return D3DERR_INVALIDCALL;
  BYTE *pixels = surface_pixels(dest);
  if (!pixels) return E_OUTOFMEMORY;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = tex_download(d, source->tex, source->level, pixels, w * 4);
  LeaveCriticalSection(&d->lock);
  return hr;
}
/* A surface in memory into one of a texture, whole and of the same size and format (the other cases are not there). */
static HRESULT STDMETHODCALLTYPE dev_UpdateSurface(Dev *d, IDirect3DSurface9 *from, const RECT *rect, IDirect3DSurface9 *to, const POINT *point) {
  Surface *source = surface_of(from), *dest = surface_of(to);
  if (!source || !dest || rect || (point && (point->x || point->y)) || source->tex->format != dest->tex->format || !source->tex->shadow[source->level]) return D3DERR_INVALIDCALL;
  const UINT w = level_size(dest->tex->width, dest->level), h = level_size(dest->tex->height, dest->level), bytes = format_bytes(dest->tex->format);
  if (w != level_size(source->tex->width, source->level) || h != level_size(source->tex->height, source->level) || !bytes) return D3DERR_INVALIDCALL;
  BYTE *pixels = surface_pixels(dest);
  if (!pixels) return E_OUTOFMEMORY;
  memcpy(pixels, source->tex->shadow[source->level], (SIZE_T)w * h * bytes);
  EnterCriticalSection(&d->lock);
  const HRESULT hr = tex_upload(d, dest->tex, dest->level);
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_UpdateTexture(Dev *d, IDirect3DBaseTexture9 *from, IDirect3DBaseTexture9 *to) {
  if (!from || !to || ((DxObj *)from)->lpVtbl != (const void *)&tex_vtbl || ((DxObj *)to)->lpVtbl != (const void *)&tex_vtbl) return D3DERR_INVALIDCALL;
  Tex *source = (Tex *)from, *dest = (Tex *)to;
  const UINT bytes = format_bytes(dest->format);
  if (source->format != dest->format || source->width != dest->width || source->height != dest->height || !bytes) return D3DERR_INVALIDCALL;
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  for (UINT level = 0; SUCCEEDED(hr) && level < dest->levels && level < source->levels; ++level) {
    const SIZE_T size = (SIZE_T)level_size(dest->width, level) * level_size(dest->height, level) * bytes;
    if (!source->shadow[level]) continue;
    if (!dest->shadow[level]) dest->shadow[level] = mem_alloc(size);
    if (!dest->shadow[level]) { hr = E_OUTOFMEMORY; break; }
    memcpy(dest->shadow[level], source->shadow[level], size);
    hr = tex_upload(d, dest, level);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
/* One target into another, whole and of one size (no stretching, no part). */
static HRESULT STDMETHODCALLTYPE dev_StretchRect(Dev *d, IDirect3DSurface9 *from, const RECT *from_rect, IDirect3DSurface9 *to, const RECT *to_rect, D3DTEXTUREFILTERTYPE filter) {
  (void)filter;
  Surface *source = surface_of(from), *dest = surface_of(to);
  if (!source || !dest || from_rect || to_rect || !source->tex->res || !dest->tex->res || source->tex->depth || dest->tex->depth || source->tex == dest->tex) return D3DERR_INVALIDCALL;
  if (source->tex->levels != 1 || dest->tex->levels != 1 || source->tex->width != dest->tex->width || source->tex->height != dest->tex->height) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  ID3D12GraphicsCommandList_CopyResource(d->list, dest->tex->res, source->tex->res);
  source->tex->used = dest->tex->used = d->epoch;
  d->recorded = 1;
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_ColorFill(Dev *d, IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR colour) {
  Surface *s = surface_of(surface);
  if (!s || rect || !s->tex->target) return D3DERR_INVALIDCALL;
  const float c[4] = {((colour >> 16) & 255) / 255.0f, ((colour >> 8) & 255) / 255.0f, (colour & 255) / 255.0f, (colour >> 24) / 255.0f};
  EnterCriticalSection(&d->lock);
  ID3D12GraphicsCommandList_ClearRenderTargetView(d->list, s->tex->view, c, 0, NULL);
  s->tex->used = d->epoch;
  d->recorded = 1;
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}

/* ---- the back buffer and Present -------------------------------------------------------------------------------------------------------------------- */

/* The device's own objects hold no reference of the device (they go with it). */
static void forget_device_reference(Dev *d) { InterlockedDecrement(&d->o.ref); }
static void take_device_reference(Dev *d) { InterlockedIncrement(&d->o.ref); }

static void targets_release(Dev *d) {
  bind(&d->target, NULL);
  bind(&d->depth, NULL);
  d->clear_count = 0;
  if (d->back_surface) { take_device_reference(d); dx_Release(OBJ(d->back_surface)); d->back_surface = NULL; }
  if (d->depth_surface) { take_device_reference(d); dx_Release(OBJ(d->depth_surface)); d->depth_surface = NULL; }
  if (d->back) { take_device_reference(d); dx_Release(OBJ(d->back)); d->back = NULL; }
  if (d->auto_depth) { take_device_reference(d); dx_Release(OBJ(d->auto_depth)); d->auto_depth = NULL; }
}
/* Makes the back buffer (the Direct3D 12 queue's, as dxgi.dll's swap chains have theirs) and the depth buffer the parameters ask for. */
static HRESULT targets_make(Dev *d) {
  D3DPRESENT_PARAMETERS *pp = &d->present;
  const HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : d->creation.hFocusWindow;
  if (pp->BackBufferWidth == 0 || pp->BackBufferHeight == 0) {
    RECT client = {0, 0, 0, 0};
    if (window) GetClientRect(window, &client);
    if (!pp->BackBufferWidth) pp->BackBufferWidth = client.right > client.left ? (UINT)(client.right - client.left) : 8;
    if (!pp->BackBufferHeight) pp->BackBufferHeight = client.bottom > client.top ? (UINT)(client.bottom - client.top) : 8;
  }
  if (pp->BackBufferWidth > 4096 || pp->BackBufferHeight > 4096) return D3DERR_INVALIDCALL;
  if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
  if (format_bytes(pp->BackBufferFormat) != 4) pp->BackBufferFormat = D3DFMT_X8R8G8B8;      /* (the 16-bit modes: the buffer has 32 bits all the same) */
  pp->BackBufferCount = 1;
  ITcosPresenter *presenter = NULL;
  IUnknown *made = NULL;
  if (FAILED(ID3D12CommandQueue_QueryInterface(d->queue, &IID_ITcosPresenter, (void **)&presenter))) return D3DERR_NOTAVAILABLE;
  HRESULT hr = presenter->lpVtbl->CreateBackBuffers(presenter, 1, pp->BackBufferWidth, pp->BackBufferHeight, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &made);
  presenter->lpVtbl->Release(presenter);
  if (FAILED(hr) || !made) return D3DERR_NOTAVAILABLE;
  hr = tex_make(d, pp->BackBufferWidth, pp->BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, pp->BackBufferFormat, D3DPOOL_DEFAULT, FALSE, (ID3D12Resource *)made, &d->back);
  made->lpVtbl->Release(made);
  if (FAILED(hr)) return hr;
  forget_device_reference(d);
  d->back_surface = surface_make(d, d->back, 0, 1);
  if (!d->back_surface) return E_OUTOFMEMORY;
  forget_device_reference(d);
  bind(&d->target, d->back_surface);
  if (pp->EnableAutoDepthStencil) {
    hr = tex_make(d, pp->BackBufferWidth, pp->BackBufferHeight, 1, D3DUSAGE_DEPTHSTENCIL, format_is_depth(pp->AutoDepthStencilFormat) ? pp->AutoDepthStencilFormat : D3DFMT_D24S8,
                  D3DPOOL_DEFAULT, FALSE, NULL, &d->auto_depth);
    if (FAILED(hr)) return hr;
    forget_device_reference(d);
    d->depth_surface = surface_make(d, d->auto_depth, 0, 1);
    if (!d->depth_surface) return E_OUTOFMEMORY;
    forget_device_reference(d);
    bind(&d->depth, d->depth_surface);
  }
  d->targets_dirty = 1;
  return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_Present(Dev *d, const RECT *source, const RECT *dest, HWND window_override, const RGNDATA *dirty) {
  (void)source; (void)dest; (void)dirty;      /* (the whole back buffer, at the window's corner) */
  const HWND window = window_override ? window_override : d->present.hDeviceWindow ? d->present.hDeviceWindow : d->creation.hFocusWindow;
  const UINT w = d->back->width, h = d->back->height;
  ITcosPresenter *presenter = NULL;
  BYTE *pixels = mem_alloc((SIZE_T)w * h * 4);
  if (!pixels) return E_OUTOFMEMORY;
  if (FAILED(ID3D12CommandQueue_QueryInterface(d->queue, &IID_ITcosPresenter, (void **)&presenter))) { mem_free(pixels); return D3DERR_DEVICELOST; }
  EnterCriticalSection(&d->lock);
  flush(d);
  const HRESULT hr = presenter->lpVtbl->ReadBackBuffer(presenter, (IUnknown *)d->back->res, pixels, w * 4);
  LeaveCriticalSection(&d->lock);
  presenter->lpVtbl->Release(presenter);
  HDC dc = SUCCEEDED(hr) && window ? GetDC(window) : NULL;
  if (dc) {
    BITMAPINFO info;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)w;
    info.bmiHeader.biHeight = -(LONG)h;      /* (top-down) */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, w, h, 0, 0, 0, h, pixels, &info, DIB_RGB_COLORS);
    ReleaseDC(window, dc);
  }
  mem_free(pixels);
  return SUCCEEDED(hr) ? D3D_OK : D3DERR_DEVICELOST;
}

static HRESULT STDMETHODCALLTYPE dev_Reset(Dev *d, D3DPRESENT_PARAMETERS *parameters) {
  if (!parameters) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  flush(d);
  state_release(&d->state);
  targets_release(d);
  d->present = *parameters;
  HRESULT hr = targets_make(d);
  if (SUCCEEDED(hr)) {
    state_defaults(&d->state, d->auto_depth != NULL, d->back->width, d->back->height);
    *parameters = d->present;
  }
  d->in_scene = 0;
  LeaveCriticalSection(&d->lock);
  return SUCCEEDED(hr) ? D3D_OK : D3DERR_INVALIDCALL;
}

/* ---- making a device ---------------------------------------------------------------------------------------------------------------------------------- */

static void dev_destroy(DxObj *o) {
  Dev *d = (Dev *)o;
  InterlockedExchange(&d->o.ref, 1 << 20);      /* (the references given back below are not the last again) */
  if (d->list) flush(d);
  state_release(&d->state);
  targets_release(d);
  if (d->blank) { take_device_reference(d); dx_Release(OBJ(d->blank)); }
  for (UINT k = 0; k < 4; ++k) if (d->blank_kind[k]) { take_device_reference(d); dx_Release(OBJ(d->blank_kind[k])); }
  for (UINT i = 0; i < d->fixed_count; ++i) { take_device_reference(d); dx_Release(OBJ(d->fixed[i].shader)); }
  for (UINT i = 0; i < d->pipeline_count; ++i) if (d->pipelines[i].state) ID3D12PipelineState_Release(d->pipelines[i].state);
  if (d->vertex_ring) ID3D12Resource_Release(d->vertex_ring);
  if (d->index_ring) ID3D12Resource_Release(d->index_ring);
  if (d->constant_ring) ID3D12Resource_Release(d->constant_ring);
  if (d->view_heap) ID3D12DescriptorHeap_Release(d->view_heap);
  if (d->sampler_heap) ID3D12DescriptorHeap_Release(d->sampler_heap);
  if (d->rtv_heap) ID3D12DescriptorHeap_Release(d->rtv_heap);
  if (d->dsv_heap) ID3D12DescriptorHeap_Release(d->dsv_heap);
  for (UINT i = 0; i < ROOTS; ++i) if (d->roots[i]) ID3D12RootSignature_Release(d->roots[i]);
  if (d->list) ID3D12GraphicsCommandList_Release(d->list);
  if (d->allocator) ID3D12CommandAllocator_Release(d->allocator);
  if (d->queue) ID3D12CommandQueue_Release(d->queue);
  if (d->d12) ID3D12Device_Release(d->d12);
  if (d->parent) dx_Release(OBJ(d->parent));
  DeleteCriticalSection(&d->lock);
}

static HRESULT make_heap(Dev *d, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, BOOL visible, ID3D12DescriptorHeap **out) {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {type, count, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  return ID3D12Device_CreateDescriptorHeap(d->d12, &desc, &IID_ID3D12DescriptorHeap, (void **)out);
}
/* A ring of upload memory. It is not kept mapped: d3d12.dll compares every mapped upload buffer with what it sent, byte by byte, at each submission, and
 * sends one unmapped since without comparing. The memory stays where Map put it (d3d12.dll's CPU copy), and flush() marks the rings a list wrote. */
static BOOL make_ring(Dev *d, UINT bytes, ID3D12Resource **ring, BYTE **data) {
  *ring = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, bytes);
  if (!*ring || FAILED(ID3D12Resource_Map(*ring, 0, NULL, (void **)data)) || !*data) return FALSE;
  ID3D12Resource_Unmap(*ring, 0, NULL);
  return TRUE;
}

/* The Direct3D 12 half of a device. */
static HRESULT dev_start(Dev *d) {
  HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d->d12);
  if (FAILED(hr) || !d->d12) return D3DERR_NOTAVAILABLE;
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  hr = ID3D12Device_CreateCommandQueue(d->d12, &qd, &IID_ID3D12CommandQueue, (void **)&d->queue);
  if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(d->d12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&d->allocator);
  if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(d->d12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d->list);
  if (FAILED(hr)) return D3DERR_NOTAVAILABLE;

  if (!root_get(d, 2)) return D3DERR_NOTAVAILABLE;      /* (the root signature of a pixel shader with one or two textures; the others when a shader needs them) */

  hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_SLOTS, FALSE, &d->rtv_heap);
  if (SUCCEEDED(hr)) hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_SLOTS, FALSE, &d->dsv_heap);
  if (SUCCEEDED(hr)) hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, VIEW_HEAP, TRUE, &d->view_heap);
  if (SUCCEEDED(hr)) hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, VIEW_HEAP, TRUE, &d->sampler_heap);
  if (FAILED(hr)) return D3DERR_NOTAVAILABLE;
  for (UINT i = 0; i < RTV_SLOTS; ++i) d->rtv_free[i] = RTV_SLOTS - 1 - i;
  for (UINT i = 0; i < DSV_SLOTS; ++i) d->dsv_free[i] = DSV_SLOTS - 1 - i;
  d->rtv_free_count = RTV_SLOTS;
  d->dsv_free_count = DSV_SLOTS;
  if (!make_ring(d, VERTEX_RING, &d->vertex_ring, &d->vertex_data) || !make_ring(d, INDEX_RING, &d->index_ring, &d->index_data) ||
      !make_ring(d, CONSTANT_RING, &d->constant_ring, &d->constant_data))
    return E_OUTOFMEMORY;
  d->epoch = 1;
  d->targets_dirty = 1;

  hr = tex_make(d, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, FALSE, NULL, &d->blank);
  if (FAILED(hr)) return hr;
  forget_device_reference(d);
  d->blank->shadow[0] = mem_alloc(4);
  if (!d->blank->shadow[0]) return E_OUTOFMEMORY;
  memset(d->blank->shadow[0], 255, 4);
  hr = tex_upload(d, d->blank, 0);
  if (FAILED(hr)) return hr;
  /* (and a white volume and cube, for samplers declared as those with nothing of their kind set; an adapter without them has none, and gets the 2D one) */
  for (UINT kind = 1; kind <= 3; kind += 2) {
    Tex *t = NULL;
    if (FAILED(tex_make_kind(d, kind, 1, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, FALSE, NULL, &t))) continue;
    forget_device_reference(d);
    d->blank_kind[kind] = t;
    for (UINT sub = 0; sub < tex_subresources(t); ++sub) {
      t->shadow[sub] = mem_alloc(4);
      if (!t->shadow[sub]) return E_OUTOFMEMORY;
      memset(t->shadow[sub], 255, 4);
      hr = tex_upload(d, t, sub);
      if (FAILED(hr)) return hr;
    }
  }
  hr = targets_make(d);
  if (FAILED(hr)) return hr;
  state_defaults(&d->state, d->auto_depth != NULL, d->back->width, d->back->height);
  return D3D_OK;
}

/* ---- the Direct3D object ------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE d3d_RegisterSoftwareDevice(Direct3D *o, void *function) { (void)o; (void)function; return D3D_OK; }
static UINT STDMETHODCALLTYPE d3d_GetAdapterCount(Direct3D *o) { (void)o; return 1; }
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterIdentifier(Direct3D *o, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) {
  (void)o; (void)flags;
  static const char driver[] = "d3d9.dll", description[] = "TheoryCraftOS Direct3D adapter", device[] = "\\\\.\\DISPLAY1";
  if (adapter != 0 || !id) return D3DERR_INVALIDCALL;
  memset(id, 0, sizeof *id);
  memcpy(id->Driver, driver, sizeof driver);
  memcpy(id->Description, description, sizeof description);
  memcpy(id->DeviceName, device, sizeof device);
  id->DriverVersion.QuadPart = 0x000A000000000000ll;
  id->VendorId = 0x1414;      /* (Microsoft's, as its software adapters have) */
  id->DeviceId = 0x008C;
  return D3D_OK;
}
static BOOL display_format(D3DFORMAT format) { return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8; }
static UINT STDMETHODCALLTYPE d3d_GetAdapterModeCount(Direct3D *o, UINT adapter, D3DFORMAT format) { (void)o; return adapter == 0 && display_format(format) ? 1 : 0; }
static HRESULT STDMETHODCALLTYPE d3d_EnumAdapterModes(Direct3D *o, UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE *mode) {
  (void)o;
  if (adapter != 0 || !display_format(format) || index != 0 || !mode) return D3DERR_INVALIDCALL;
  display_mode(mode);
  mode->Format = format;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterDisplayMode(Direct3D *o, UINT adapter, D3DDISPLAYMODE *mode) {
  (void)o;
  if (adapter != 0 || !mode) return D3DERR_INVALIDCALL;
  display_mode(mode);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceType(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) {
  (void)o; (void)type; (void)windowed;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  return display_format(display) && (back == D3DFMT_UNKNOWN || display_format(back)) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormat(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) {
  (void)o; (void)type; (void)display;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  if (resource != D3DRTYPE_SURFACE && resource != D3DRTYPE_TEXTURE && resource != D3DRTYPE_CUBETEXTURE && resource != D3DRTYPE_VOLUMETEXTURE) return D3DERR_NOTAVAILABLE;
  const BOOL layered = resource == D3DRTYPE_CUBETEXTURE || resource == D3DRTYPE_VOLUMETEXTURE;      /* (sampled only: nothing draws into a cube or a volume here) */
  if (usage & D3DUSAGE_DEPTHSTENCIL) return format_is_depth(format) && !layered ? D3D_OK : D3DERR_NOTAVAILABLE;
  if (usage & D3DUSAGE_RENDERTARGET) return display_format(format) && !layered ? D3D_OK : D3DERR_NOTAVAILABLE;
  if (dxt_block(format)) return resource != D3DRTYPE_VOLUMETEXTURE ? D3D_OK : D3DERR_NOTAVAILABLE;      /* (DXT1 .. DXT5: 2D textures and cubes) */
  return format_bytes(format) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceMultiSampleType(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE samples,
                                                                DWORD *levels) {
  (void)o; (void)type; (void)format; (void)windowed;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  if (levels) *levels = samples == D3DMULTISAMPLE_NONE ? 1 : 0;
  return samples == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDepthStencilMatch(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT target, D3DFORMAT depth) {
  (void)o; (void)type; (void)display;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  return display_format(target) && format_is_depth(depth) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormatConversion(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT from, D3DFORMAT to) {
  (void)o; (void)type;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  return format_bytes(from) && display_format(to) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_GetDeviceCaps(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) {
  (void)o; (void)type;
  if (adapter != 0 || !caps) return D3DERR_INVALIDCALL;
  caps_fill(caps);
  return D3D_OK;
}
static HMONITOR STDMETHODCALLTYPE d3d_GetAdapterMonitor(Direct3D *o, UINT adapter) {
  (void)o;
  return adapter == 0 ? MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY) : NULL;
}
static HRESULT STDMETHODCALLTYPE d3d_CreateDevice(Direct3D *o, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS *parameters, IDirect3DDevice9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (adapter != 0 || !parameters || (type != D3DDEVTYPE_HAL && type != D3DDEVTYPE_REF && type != D3DDEVTYPE_SW)) return D3DERR_INVALIDCALL;
  if (!focus && !parameters->hDeviceWindow) return D3DERR_INVALIDCALL;
  Dev *d = mem_alloc(sizeof *d);
  if (!d) return E_OUTOFMEMORY;
  dx_init(&d->o, &dev_vtbl, g_dev_iids, dev_destroy);
  InitializeCriticalSection(&d->lock);
  dx_AddRef(OBJ(o));
  d->parent = o;
  d->creation.AdapterOrdinal = adapter;
  d->creation.DeviceType = type;
  d->creation.hFocusWindow = focus;
  d->creation.BehaviorFlags = behaviour;
  d->present = *parameters;
  const HRESULT hr = dev_start(d);
  if (FAILED(hr)) { dx_Release(OBJ(d)); return hr == E_OUTOFMEMORY ? hr : D3DERR_NOTAVAILABLE; }
  *parameters = d->present;      /* (the size and the format that were chosen) */
  *out = (IDirect3DDevice9 *)d;
  return D3D_OK;
}

/* ---- the entry points ------------------------------------------------------------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_private_lock);
  return TRUE;
}

EXPORT IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
  (void)sdk_version;
  Direct3D *o = mem_alloc(sizeof *o);
  if (!o) return NULL;
  dx_init(&o->o, &d3d_vtbl, g_d3d_iids, NULL);
  return (IDirect3D9 *)o;
}
/* (the extended interfaces of Windows Vista: not there, a program takes the plain ones) */
EXPORT HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **out) {
  (void)sdk_version;
  if (out) *out = NULL;
  return D3DERR_NOTAVAILABLE;
}
