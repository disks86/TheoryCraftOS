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

/* Part of d3d9.c: the device itself - what it can do, its targets, when it is lost, how it is made and reset (and 9Ex's ResetEx and the rest of
 * IDirect3DDevice9Ex) - the Direct3D object a program asks about the adapter (IDirect3D9, IDirect3D9Ex), and the entry points of the library. */

/* ---- what the device can do -------------------------------------------------------------------------------------------------------------------------- */

/* Whether the kernel keeps the levels of every image - of cubes and volumes too - on the adapter (graphics::kFeature2MipmappedLayers: the CPU renderer, virgl). */
/* What the kernel's 3D device can do (its features words), asked once: bit 10 of features2 is mipmapped layers (the levels of cubes and volumes), bit 5 of
 * features several render targets at once. */
static volatile LONG g_adapter_known;      /* (0 not asked yet, 1 asked) */
static UINT g_adapter_features, g_adapter_features2;
static void adapter_features(void) {
  if (g_adapter_known) return;
  KmtAdapter adapters[1];
  KmtCaps caps;
  memset(&caps, 0, sizeof caps);
  if (kmt_enum_adapters(adapters, 1, NULL) >= 1) {
    if (KMT_OK(kmt_adapter_caps(adapters[0].handle, &caps))) {
      g_adapter_features = caps.features;
      g_adapter_features2 = caps.features2;
    }
    kmt_close_adapter(adapters[0].handle);
  }
  InterlockedExchange(&g_adapter_known, 1);
}
static int adapter_mipmapped_layers(void) {
  adapter_features();
  return (g_adapter_features2 & (1u << 10)) != 0;
}
/* The render targets a draw writes at once: 4 where the kernel draws into several (D3DCAPS9.NumSimultaneousRTs), else 1. */
static UINT adapter_targets(void) {
  adapter_features();
  return (g_adapter_features & (1u << 5)) ? TARGETS : 1u;
}
static void caps_fill(D3DCAPS9 *caps) {
  memset(caps, 0, sizeof *caps);
  caps->DeviceType = D3DDEVTYPE_HAL;
  caps->Caps = D3DCAPS_READ_SCANLINE;
  caps->Caps2 = D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_FULLSCREENGAMMA;
  caps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
  caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO | D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR;
  caps->CursorCaps = D3DCURSORCAPS_COLOR;
  caps->DevCaps = D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                  D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                  D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_PUREDEVICE | D3DDEVCAPS_CANBLTSYSTONONLOCAL;
  caps->DevCaps2 = D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES | D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
  caps->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP |
                            D3DPMISCCAPS_SEPARATEALPHABLEND | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_PERSTAGECONSTANT | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS;
  caps->MaxActiveLights = FFL_LIGHTS;
  caps->VertexProcessingCaps = D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TEXGEN |
                               D3DVTXPCAPS_TEXGEN_SPHEREMAP | D3DVTXPCAPS_TWEENING;
  /* (vertex blending of up to four matrices a vertex, of the 256 world matrices with indices: done where the vertices are decoded) */
  caps->MaxVertexBlendMatrices = 4;
  caps->MaxVertexBlendMatrixIndex = 255;
  caps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_WFOG |
                     D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_ANISOTROPY;
  caps->ZCmpCaps = caps->AlphaCmpCaps = 0xFF;      /* (all eight comparisons) */
  caps->SrcBlendCaps = caps->DestBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA |
                                             D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                                             D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
  caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_FOGGOURAUD;
  caps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP |
                      D3DPTEXTURECAPS_ALPHAPALETTE | D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE;
  /* (the levels of cubes and volumes: where the kernel keeps the levels of every image) */
  if (adapter_mipmapped_layers()) caps->TextureCaps |= D3DPTEXTURECAPS_MIPCUBEMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP;
  caps->TextureFilterCaps = caps->CubeTextureFilterCaps = caps->VolumeTextureFilterCaps =
      D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR;
  caps->TextureFilterCaps |= D3DPTFILTERCAPS_MINFANISOTROPIC;
  caps->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
  /* (wrap, mirror, clamp, border - D3DSAMP_BORDERCOLOR -, mirror once, each direction its own) */
  caps->TextureAddressCaps = caps->VolumeTextureAddressCaps =
      D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_MIRRORONCE | D3DPTADDRESSCAPS_INDEPENDENTUV;
  caps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
  caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
  caps->MaxVolumeExtent = 256;
  caps->MaxTextureRepeat = 8192;
  caps->MaxTextureAspectRatio = 4096;
  caps->MaxAnisotropy = 16;
  caps->MaxVertexW = 1e10f;
  caps->GuardBandLeft = caps->GuardBandTop = -32768.0f;
  caps->GuardBandRight = caps->GuardBandBottom = 32768.0f;
  caps->StencilCaps = 0xFF | D3DSTENCILCAPS_TWOSIDED;
  caps->FVFCaps = TEXTURE_SETS | D3DFVFCAPS_PSIZE;      /* (the sets of texture coordinates a vertex can have) */
  /* (every operation of the texture stages but D3DTOP_PREMODULATE: ff_stages.h) */
  caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
                        D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                        D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM | D3DTEXOPCAPS_BLENDCURRENTALPHA |
                        D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA | D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
                        D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP | D3DTEXOPCAPS_BUMPENVMAP |
                        D3DTEXOPCAPS_BUMPENVMAPLUMINANCE;
  caps->MaxTextureBlendStages = STAGES;
  caps->MaxSimultaneousTextures = STAGES;
  caps->MaxUserClipPlanes = 0;
  caps->MaxPrimitiveCount = INDEX_RING / 12;
  caps->MaxVertexIndex = 0x00FFFFFF;
  caps->MaxStreams = STREAMS;
  caps->MaxStreamStride = 508;
  caps->MaxPointSize = MAX_POINT_SIZE;
  caps->NumSimultaneousRTs = adapter_targets();
  if (caps->NumSimultaneousRTs > 1)      /* (each target its own write mask and format; blending and the alpha test on all of them) */
    caps->PrimitiveMiscCaps |= D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING;
  caps->MasterAdapterOrdinal = caps->AdapterOrdinal = 0;
  caps->NumberOfAdaptersInGroup = 1;
  caps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4 | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N;
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

static int display_mode_same(const DEVMODEW *a, const DEVMODEW *b) {
  return a->dmPelsWidth == b->dmPelsWidth && a->dmPelsHeight == b->dmPelsHeight && a->dmBitsPerPel == b->dmBitsPerPel;
}
static void display_devmode(DEVMODEW *dm) {
  memset(dm, 0, sizeof *dm);
  dm->dmSize = sizeof *dm;
  EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, dm);
}
/* Whether the device is lost, by Direct3D 9's rules: a full-screen device when its window is not in front or is minimized, any device when the display mode
 * changed from the one it was made or reset in (another program's full-screen mode); once lost, it stays "not reset" until Reset. A 9Ex device is never lost. */
static HRESULT device_lost_state(Dev *d) {
  if (d->ex) return D3D_OK;
  if (!d->lost) {
    DEVMODEW now;
    display_devmode(&now);
    BOOL lose = !display_mode_same(&now, &d->lost_mode);
    if (d->chain && !d->chain->pp.Windowed) {
      HWND focus = d->creation.hFocusWindow ? d->creation.hFocusWindow : d->chain->window;
      if (focus && (IsIconic(focus) || GetForegroundWindow() != focus)) lose = TRUE;
    }
    if (lose) d->lost = d->not_reset = 1;
  }
  if (!d->lost) return D3D_OK;
  /* (lost while what lost it lasts; then waiting for Reset) */
  if (d->chain && !d->chain->pp.Windowed) {
    HWND focus = d->creation.hFocusWindow ? d->creation.hFocusWindow : d->chain->window;
    if (focus && (IsIconic(focus) || GetForegroundWindow() != focus)) return D3DERR_DEVICELOST;
  }
  return D3DERR_DEVICENOTRESET;
}
static HRESULT STDMETHODCALLTYPE dev_TestCooperativeLevel(Dev *d) { return device_lost_state(d); }
/* The texture memory there is: the adapter's memory (video and shared) less what this device holds, rounded down to a megabyte as Direct3D 9 gives it. */
static UINT STDMETHODCALLTYPE dev_GetAvailableTextureMem(Dev *d) {
  UINT adapter = 0;
  UINT64 sizes[3] = {0, 0, 0};
  if (KMT_OK(kmt_open_adapter(ID3D12Device_GetAdapterLuid(d->d12), &adapter))) {
    kmt_segment_sizes(adapter, sizes);
    kmt_close_adapter(adapter);
  }
  /* (the adapter's own memory and the system memory it may use; as on Windows, half of the computer's memory counts as shared where the adapter names less) */
  MEMORYSTATUSEX memory;
  memset(&memory, 0, sizeof memory);
  memory.dwLength = sizeof memory;
  const UINT64 half = GlobalMemoryStatusEx(&memory) ? memory.ullTotalPhys / 2 : 0;
  UINT64 total = sizes[0] + sizes[1] + (sizes[2] > half ? sizes[2] : half);
  const UINT64 used = d->video_bytes;
  total = total > used ? total - used : 0;
  if (total > 0xFFF00000ull) total = 0xFFF00000ull;
  return (UINT)(total & ~0xFFFFFull);
}
/* (managed resources are in the device's memory and the program's both: there is nothing to put out, and nothing to bring in later) */
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
static HRESULT STDMETHODCALLTYPE dev_GetCreationParameters(Dev *d, D3DDEVICE_CREATION_PARAMETERS *parameters) {
  if (!parameters) return D3DERR_INVALIDCALL;
  *parameters = d->creation;
  return D3D_OK;
}
/* ValidateDevice: one pass, unless a stage has an operation the device has not (D3DTOP_PREMODULATE; the bump environment ones as alpha operations, which
 * Direct3D 9 has only for the colour) or an argument that is none. */
static HRESULT STDMETHODCALLTYPE dev_ValidateDevice(Dev *d, DWORD *passes) {
  for (UINT i = 0; i < STAGES; ++i) {
    const DWORD *t = d->state.stage[i];
    if (t[D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
    if (t[D3DTSS_COLOROP] == D3DTOP_PREMODULATE || t[D3DTSS_COLOROP] > D3DTOP_LERP)
      return D3DERR_UNSUPPORTEDCOLOROPERATION;
    if (t[D3DTSS_ALPHAOP] == D3DTOP_BUMPENVMAP || t[D3DTSS_ALPHAOP] == D3DTOP_BUMPENVMAPLUMINANCE || t[D3DTSS_ALPHAOP] == D3DTOP_PREMODULATE || t[D3DTSS_ALPHAOP] > D3DTOP_LERP)
      return D3DERR_UNSUPPORTEDALPHAOPERATION;
    if ((t[D3DTSS_COLORARG1] & D3DTA_SELECTMASK) > D3DTA_CONSTANT || (t[D3DTSS_COLORARG2] & D3DTA_SELECTMASK) > D3DTA_CONSTANT) return D3DERR_UNSUPPORTEDCOLORARG;
    if ((t[D3DTSS_ALPHAARG1] & D3DTA_SELECTMASK) > D3DTA_CONSTANT || (t[D3DTSS_ALPHAARG2] & D3DTA_SELECTMASK) > D3DTA_CONSTANT) return D3DERR_UNSUPPORTEDALPHAARG;
  }
  if (passes) *passes = 1;
  return D3D_OK;
}

/* ---- what is drawn into ----------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_SetRenderTarget(Dev *d, DWORD index, IDirect3DSurface9 *surface) {
  Surface *s = surface_of(surface);
  if (index >= adapter_targets()) return D3DERR_INVALIDCALL;      /* (D3DCAPS9.NumSimultaneousRTs) */
  if (index > 0 && !surface) {      /* (targets 1 .. 3 may be unset; 0 may not) */
    EnterCriticalSection(&d->lock);
    bind(&d->target[index], NULL);
    d->targets_dirty = 1;
    LeaveCriticalSection(&d->lock);
    return D3D_OK;
  }
  if (!s || s->c.dev != d || !surface_drawable(s)) return D3DERR_INVALIDCALL;      /* (a level of a cube the kernel does not keep: see tex_make_kind) */
  EnterCriticalSection(&d->lock);
  bind(&d->target[index], s);
  if (index > 0) {      /* (the viewport stays as it is for the others) */
    d->targets_dirty = 1;
    LeaveCriticalSection(&d->lock);
    return D3D_OK;
  }
  /* (as Direct3D 9 does: the viewport and the scissor rectangle become the whole of the new target) */
  const D3DVIEWPORT9 viewport = {0, 0, sub_width(s->tex, s->sub), sub_height(s->tex, s->sub), 0.0f, 1.0f};
  d->state.viewport = viewport;
  SetRect(&d->state.scissor, 0, 0, (int)viewport.Width, (int)viewport.Height);
  d->targets_dirty = 1;
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetRenderTarget(Dev *d, DWORD index, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (index >= adapter_targets()) return D3DERR_INVALIDCALL;
  if (!d->target[index]) return D3DERR_NOTFOUND;
  child_AddRef(&d->target[index]->c);
  *out = (IDirect3DSurface9 *)d->target[index];
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetDepthStencilSurface(Dev *d, IDirect3DSurface9 *surface) {
  Surface *s = surface_of(surface);
  if (surface && (!s || s->c.dev != d || !s->tex->depth)) return D3DERR_INVALIDCALL;
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
  child_AddRef(&d->depth->c);
  *out = (IDirect3DSurface9 *)d->depth;
  return D3D_OK;
}

/* ---- the implicit swap chain, Reset ----------------------------------------------------------------------------------------------------------------- */

/* The device's swap chain and depth buffer go (Reset, the device's end). */
/* A swap chain's buffer or the device's depth buffer that a 9Ex reset leaves with the program: a surface of its own (it frees its texture with its last reference),
 * counted as a resource of the default pool, its container the device. */
static void surface_orphan(Dev *d, Surface *s) {
  s->kind = SURFACE_ALONE;
  s->container = NULL;
  s->c.kept = 0;
  default_resource_made(d, s->tex->pool);
}
static void targets_release(Dev *d) {
  for (UINT i = 0; i < TARGETS; ++i) bind(&d->target[i], NULL);
  bind(&d->depth, NULL);
  d->clear_count = 0;
  if (d->auto_depth) { Tex *t = d->auto_depth->tex; obj_free(OBJ(d->auto_depth)); obj_free(OBJ(t)); d->auto_depth = NULL; }
  if (d->chain) { obj_free(OBJ(d->chain)); d->chain = NULL; }
}
/* Makes the implicit swap chain and the depth buffer the parameters ask for, and sets them. */
static HRESULT targets_make(Dev *d, D3DPRESENT_PARAMETERS *pp) {
  SwapChain *c = NULL;
  HRESULT hr = chain_create(d, pp, 1, &c);
  if (FAILED(hr)) return hr;
  d->chain = c;
  bind(&d->target[0], c->surface[0]);
  if (pp->EnableAutoDepthStencil) {
    Tex *t = NULL;
    hr = tex_make(d, pp->BackBufferWidth, pp->BackBufferHeight, 1, D3DUSAGE_DEPTHSTENCIL, pp->AutoDepthStencilFormat, D3DPOOL_DEFAULT, TEX_IMPLICIT, NULL, &t);
    if (FAILED(hr)) return hr;
    d->auto_depth = surface_make(d, t, SURFACE_IMPLICIT, NULL);
    if (!d->auto_depth) { obj_free(OBJ(t)); return E_OUTOFMEMORY; }
    bind(&d->depth, d->auto_depth);
  }
  display_devmode(&d->lost_mode);
  d->targets_dirty = 1;
  return D3D_OK;
}
/* Reset: new buffers of the new parameters and the state of a new device. A device that is not 9Ex is reset only when the program holds no resource of the
 * default pool (and no additional swap chain): else it fails and stays lost. */
static HRESULT device_reset(Dev *d, D3DPRESENT_PARAMETERS *parameters, const D3DDISPLAYMODEEX *mode) {
  if (!parameters) return D3DERR_INVALIDCALL;
  HRESULT hr = present_parameters_check(d, parameters, d->creation.hFocusWindow);
  if (FAILED(hr)) { if (!d->ex) d->lost = d->not_reset = 1; return hr; }
  EnterCriticalSection(&d->lock);
  /* (what the device has set goes first - a texture or target the program let go of goes with it -; then the program may hold no buffer of the swap chain, and,
   * not 9Ex, no resource of the default pool) */
  if (!d->ex) {
    flush(d);
    state_release(&d->state);
    for (UINT i = 1; i < TARGETS; ++i) bind(&d->target[i], NULL);
    if (d->chain && d->chain->count) bind(&d->target[0], d->chain->surface[0]);
    bind(&d->depth, d->auto_depth);
  }
  BOOL held = d->auto_depth && d->auto_depth->c.o.ref > 0;
  for (UINT i = 0; d->chain && i < d->chain->count; ++i) held = held || d->chain->surface[i]->c.o.ref > 0;
  if (d->ex && held) {
    /* (9Ex resets with the buffers held: they stay the program's, surfaces of their own whose container is the device - the new swap chain has new ones) */
    for (UINT i = 0; d->chain && i < d->chain->count; ++i) {
      Surface *s = d->chain->surface[i];
      if (!s || s->c.o.ref == 0) continue;
      for (UINT j = 0; j <= MAX_BACK_BUFFERS; ++j)
        if (d->chain->buffer[j] == s->tex) d->chain->buffer[j] = NULL;
      d->chain->surface[i] = NULL;
      surface_orphan(d, s);
    }
    if (d->auto_depth && d->auto_depth->c.o.ref > 0) {
      bind(&d->depth, NULL);
      surface_orphan(d, d->auto_depth);
      d->auto_depth = NULL;
    }
    held = FALSE;
  }
  if (held || (!d->ex && d->default_resources > 0)) {
    d->lost = d->not_reset = 1;
    LeaveCriticalSection(&d->lock);
    return D3DERR_INVALIDCALL;
  }
  flush(d);
  if (d->recording) { obj_free(OBJ(d->recording)); d->recording = NULL; }      /* (a block being recorded holds no reference of the device) */
  /* (a 9Ex device keeps its state through a reset, but for the viewport and the scissor rectangle, which become the new back buffer's) */
  if (!d->ex) state_release(&d->state);
  const BOOL was_fullscreen = d->chain && !d->chain->pp.Windowed;
  targets_release(d);
  if (was_fullscreen && parameters->Windowed) display_set(NULL);
  D3DPRESENT_PARAMETERS pp = *parameters;
  if (mode && !pp.Windowed) { pp.BackBufferWidth = mode->Width; pp.BackBufferHeight = mode->Height; pp.FullScreen_RefreshRateInHz = mode->RefreshRate; }
  hr = targets_make(d, &pp);
  if (SUCCEEDED(hr)) {
    if (!d->ex) state_defaults(&d->state, d->auto_depth != NULL, pp.BackBufferWidth, pp.BackBufferHeight);
    else {
      d->state.viewport.X = d->state.viewport.Y = 0;
      d->state.viewport.Width = pp.BackBufferWidth;
      d->state.viewport.Height = pp.BackBufferHeight;
      SetRect(&d->state.scissor, 0, 0, (int)pp.BackBufferWidth, (int)pp.BackBufferHeight);
    }
    *parameters = pp;
    d->lost = d->not_reset = 0;
  } else if (!d->ex) d->lost = d->not_reset = 1;
  d->in_scene = 0;
  LeaveCriticalSection(&d->lock);
  return SUCCEEDED(hr) ? D3D_OK : hr == E_OUTOFMEMORY || hr == D3DERR_OUTOFVIDEOMEMORY ? hr : D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE dev_Reset(Dev *d, D3DPRESENT_PARAMETERS *parameters) { return device_reset(d, parameters, NULL); }
/* (9Ex) ResetEx: a full-screen swap chain takes the mode given (which a windowed one must not have) */
static HRESULT STDMETHODCALLTYPE dev_ResetEx(Dev *d, D3DPRESENT_PARAMETERS *parameters, D3DDISPLAYMODEEX *mode) {
  if (!parameters) return D3DERR_INVALIDCALL;
  if ((parameters->Windowed && mode) || (!parameters->Windowed && !mode)) return D3DERR_INVALIDCALL;
  if (mode && (mode->Size != sizeof *mode || mode->Width != parameters->BackBufferWidth || mode->Height != parameters->BackBufferHeight)) return D3DERR_INVALIDCALL;
  return device_reset(d, parameters, mode);
}

/* ---- the rest of IDirect3DDevice9Ex ----------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_PresentEx(Dev *d, const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty, DWORD flags) {
  if (!d->chain) return D3DERR_INVALIDCALL;
  return chain_Present(d->chain, src, dst, window, dirty, flags);
}
static HRESULT STDMETHODCALLTYPE dev_GetGPUThreadPriority(Dev *d, INT *priority) {
  if (!priority) return D3DERR_INVALIDCALL;
  *priority = d->gpu_priority;
  return D3D_OK;
}
/* The priority of the device's work against other devices' (-7 .. 7): kept; the kernel's queue runs work in order. */
static HRESULT STDMETHODCALLTYPE dev_SetGPUThreadPriority(Dev *d, INT priority) {
  if (priority < -7 || priority > 7) return D3DERR_INVALIDCALL;
  d->gpu_priority = priority;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_WaitForVBlank(Dev *d, UINT chain) {
  if (chain != 0) return D3DERR_INVALIDCALL;
  wait_for_vblank(d, 1);
  return D3D_OK;
}
/* Whether resources are in video memory: they all are (the device's textures are its own, the rest is sent when drawn). */
static HRESULT STDMETHODCALLTYPE dev_CheckResourceResidency(Dev *d, IDirect3DResource9 **resources, UINT32 count) {
  (void)d;
  if (count > 65535 || (count && !resources)) return D3DERR_INVALIDCALL;
  return D3D_OK;
}
/* The frames the device may be ahead of the display: 1 .. 30, 0 the default of 3. (The queue here runs each list before it returns: the device is never ahead.) */
static HRESULT STDMETHODCALLTYPE dev_SetMaximumFrameLatency(Dev *d, UINT latency) {
  if (latency > 30) return D3DERR_INVALIDCALL;
  d->max_frame_latency = latency ? latency : 3;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetMaximumFrameLatency(Dev *d, UINT *latency) {
  if (!latency) return D3DERR_INVALIDCALL;
  *latency = d->max_frame_latency;
  return D3D_OK;
}
/* CheckDeviceState: S_OK, or S_PRESENT_OCCLUDED for a window that cannot be seen (minimized, hidden, or a full-screen device not in front), or
 * S_PRESENT_MODE_CHANGED when the display is in another mode than a full-screen swap chain's. */
static HRESULT STDMETHODCALLTYPE dev_CheckDeviceState(Dev *d, HWND window) {
  HWND w = window ? window : d->chain ? d->chain->window : NULL;
  if (w && (!IsWindow(w))) return D3DERR_INVALIDCALL;
  if (w && (IsIconic(w) || !IsWindowVisible(w))) return S_PRESENT_OCCLUDED;
  if (d->chain && !d->chain->pp.Windowed) {
    D3DDISPLAYMODEEX now;
    display_current(&now);
    if (now.Width != d->chain->mode.Width || now.Height != d->chain->mode.Height) return S_PRESENT_MODE_CHANGED;
    HWND focus = d->creation.hFocusWindow ? d->creation.hFocusWindow : d->chain->window;
    if (focus && GetForegroundWindow() != focus) return S_PRESENT_OCCLUDED;
  }
  return D3D_OK;
}
/* The kernel of a monochrome convolution (ComposeRects' filtering): kept; a row and a column of weights of the width and height, or the box filter for NULL. */
static HRESULT STDMETHODCALLTYPE dev_SetConvolutionMonoKernel(Dev *d, UINT width, UINT height, float *rows, float *columns) {
  if (width == 0 || height == 0 || width > 7 || height > 7) return D3DERR_INVALIDCALL;
  d->conv_width = width;
  d->conv_height = height;
  for (UINT i = 0; i < width; ++i) d->conv_rows[i] = rows ? rows[i] : 1.0f / (float)width;
  for (UINT i = 0; i < height; ++i) d->conv_columns[i] = columns ? columns[i] : 1.0f / (float)height;
  return D3D_OK;
}
/* ComposeRects: rectangles of a D3DFMT_A1 surface (glyphs) composed into a target, as the D3DCOMPOSERECTDESC and D3DCOMPOSERECTDESTINATION vertex buffers say. The
 * device has no D3DFMT_A1 surfaces (CheckDeviceFormat refuses the format), so there is no source a program could give: D3DERR_INVALIDCALL, as for any other
 * surface. */
static HRESULT STDMETHODCALLTYPE dev_ComposeRects(Dev *d, IDirect3DSurface9 *src, IDirect3DSurface9 *dst, IDirect3DVertexBuffer9 *src_descs, UINT count,
                                                  IDirect3DVertexBuffer9 *dst_descs, D3DCOMPOSERECTSOP op, INT x, INT y) {
  (void)x; (void)y;
  Surface *s = surface_of(src), *t = surface_of(dst);
  if (!s || !t || s->c.dev != d || t->c.dev != d || !vb_of(src_descs) || !vb_of(dst_descs) || count > 0xFFFF || op < D3DCOMPOSERECTS_COPY || op > D3DCOMPOSERECTS_NEG)
    return D3DERR_INVALIDCALL;
  if (s->tex->format != D3DFMT_A1) return D3DERR_INVALIDCALL;
  return D3DERR_INVALIDCALL;
}

/* ---- making a device ---------------------------------------------------------------------------------------------------------------------------------- */

static void dev_destroy(DxObj *o) {
  Dev *d = (Dev *)o;
  InterlockedExchange(&d->o.ref, 1 << 20);      /* (the references given back below are not the last again) */
  if (d->list) flush(d);
  if (d->recording) { obj_free(OBJ(d->recording)); d->recording = NULL; }
  state_release(&d->state);
  targets_release(d);
  if (d->cursor) DestroyCursor(d->cursor);
  for (UINT i = 0; i < d->fvf_decl_count; ++i) obj_free(OBJ(d->fvf_decls[i].decl));
  mem_free(d->fvf_decls);
  if (d->blank) obj_free(OBJ(d->blank));
  for (UINT k = 0; k < 4; ++k) if (d->blank_kind[k]) obj_free(OBJ(d->blank_kind[k]));
  for (UINT i = 0; i < d->fixed_count; ++i) obj_free(OBJ(d->fixed[i].shader));
  for (UINT i = 0; i < d->pipeline_count; ++i) if (d->pipelines[i].state) ID3D12PipelineState_Release(d->pipelines[i].state);
  queries_stop(d);
  mem_free(d->palettes);
  mem_free(d->palette_set);
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

/* The Direct3D 12 half of a device, and its swap chain. */
static HRESULT dev_start(Dev *d, D3DPRESENT_PARAMETERS *pp) {
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
  d->current_palette = 0xFFFF;
  d->max_frame_latency = 3;
  queries_start(d);

  hr = tex_make(d, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, TEX_INTERNAL, NULL, &d->blank);
  if (FAILED(hr)) return hr;
  d->blank->shadow[0] = mem_alloc(4);
  if (!d->blank->shadow[0]) return E_OUTOFMEMORY;
  memset(d->blank->shadow[0], 255, 4);
  hr = tex_upload(d, d->blank, 0);
  if (FAILED(hr)) return hr;
  /* (and a white volume and cube, for samplers declared as those with nothing of their kind set; an adapter without them has none, and gets the 2D one) */
  for (UINT kind = 1; kind <= 3; kind += 2) {
    Tex *t = NULL;
    if (FAILED(tex_make_kind(d, kind, 1, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, TEX_INTERNAL, NULL, &t))) continue;
    d->blank_kind[kind] = t;
    for (UINT sub = 0; sub < tex_subresources(t); ++sub) {
      t->shadow[sub] = mem_alloc(4);
      if (!t->shadow[sub]) return E_OUTOFMEMORY;
      memset(t->shadow[sub], 255, 4);
      hr = tex_upload(d, t, sub);
      if (FAILED(hr)) return hr;
    }
  }
  hr = targets_make(d, pp);
  if (FAILED(hr)) return hr;
  state_defaults(&d->state, d->auto_depth != NULL, pp->BackBufferWidth, pp->BackBufferHeight);
  return D3D_OK;
}

/* ---- the Direct3D object ------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE d3d_RegisterSoftwareDevice(Direct3D *o, void *function) { (void)o; return function ? D3D_OK : D3DERR_INVALIDCALL; }
static UINT STDMETHODCALLTYPE d3d_GetAdapterCount(Direct3D *o) { (void)o; return 1; }
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterIdentifier(Direct3D *o, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) {
  (void)o;
  static const char driver[] = "d3d9.dll", description[] = "TheoryCraftOS Direct3D adapter", device[] = "\\\\.\\DISPLAY1";
  if (adapter != 0 || !id || (flags & ~(DWORD)D3DENUM_WHQL_LEVEL)) return D3DERR_INVALIDCALL;
  memset(id, 0, sizeof *id);
  memcpy(id->Driver, driver, sizeof driver);
  memcpy(id->Description, description, sizeof description);
  memcpy(id->DeviceName, device, sizeof device);
  id->DriverVersion.QuadPart = 0x000A000000000000ll;
  id->VendorId = 0x1414;      /* (Microsoft's, as its software adapters have) */
  id->DeviceId = 0x008C;
  /* (an identifier that stays the same for this adapter and driver: made of what describes them) */
  static const GUID identifier = {0x7c0e5a10, 0x2d5e, 0x4a5d, {0x9a, 0x7e, 0x54, 0x43, 0x4f, 0x53, 0x09, 0x09}};
  id->DeviceIdentifier = identifier;
  id->WHQLLevel = (flags & D3DENUM_WHQL_LEVEL) ? 1 : 0;
  return D3D_OK;
}
static UINT STDMETHODCALLTYPE d3d_GetAdapterModeCount(Direct3D *o, UINT adapter, D3DFORMAT format) { (void)o; return adapter == 0 ? display_mode_count(format) : 0; }
static HRESULT STDMETHODCALLTYPE d3d_EnumAdapterModes(Direct3D *o, UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE *mode) {
  (void)o;
  if (adapter != 0 || !mode || !display_format(format)) return D3DERR_INVALIDCALL;      /* (a format with no modes is not one of the display's) */
  D3DDISPLAYMODEEX m;
  if (!display_mode_at(format, index, &m)) return D3DERR_INVALIDCALL;
  display_mode_plain(&m, mode);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterDisplayMode(Direct3D *o, UINT adapter, D3DDISPLAYMODE *mode) {
  (void)o;
  if (adapter != 0 || !mode) return D3DERR_INVALIDCALL;
  D3DDISPLAYMODEEX m;
  display_current(&m);
  display_mode_plain(&m, mode);
  return D3D_OK;
}
/* A back buffer format goes with a display format: the same pixels, or the same with alpha; windowed, any back buffer format the device has. */
static BOOL backbuffer_on_display(D3DFORMAT display, D3DFORMAT back) {
  if (back == display) return TRUE;
  return (display == D3DFMT_X8R8G8B8 && back == D3DFMT_A8R8G8B8) || (display == D3DFMT_X1R5G5B5 && back == D3DFMT_A1R5G5B5);
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceType(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) {
  (void)o;
  if (adapter != 0 || (type != D3DDEVTYPE_HAL && type != D3DDEVTYPE_REF && type != D3DDEVTYPE_SW)) return D3DERR_INVALIDCALL;
  if (!display_format(display)) return windowed && display != D3DFMT_UNKNOWN ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL;
  if (back == D3DFMT_UNKNOWN) return windowed ? D3D_OK : D3DERR_INVALIDCALL;
  if (!backbuffer_format(back)) return D3DERR_NOTAVAILABLE;
  return windowed || backbuffer_on_display(display, back) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
/* What a format can be: a target, a depth buffer, a texture of the kinds, a volume; and what the device does with it (the D3DUSAGE_QUERY_* questions). */
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormat(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) {
  (void)o; (void)type;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  if (!display_format(display)) return D3DERR_NOTAVAILABLE;
  if (resource == D3DRTYPE_VERTEXBUFFER) return format == D3DFMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;
  if (resource == D3DRTYPE_INDEXBUFFER) return format == D3DFMT_INDEX16 || format == D3DFMT_INDEX32 ? D3D_OK : D3DERR_NOTAVAILABLE;
  if (resource != D3DRTYPE_SURFACE && resource != D3DRTYPE_TEXTURE && resource != D3DRTYPE_CUBETEXTURE && resource != D3DRTYPE_VOLUMETEXTURE && resource != D3DRTYPE_VOLUME)
    return D3DERR_INVALIDCALL;
  const BOOL volume = resource == D3DRTYPE_VOLUMETEXTURE || resource == D3DRTYPE_VOLUME, cube = resource == D3DRTYPE_CUBETEXTURE;
  const BOOL depth = format_is_depth(format), plain = format_bytes(format) != 0, block = dxt_block(format) != 0;
  if (!depth && !plain && !block) return D3DERR_NOTAVAILABLE;
  if ((usage & D3DUSAGE_DEPTHSTENCIL) && (!depth || volume || cube)) return D3DERR_NOTAVAILABLE;
  if ((usage & D3DUSAGE_RENDERTARGET) && (!format_is_target(format) || volume)) return D3DERR_NOTAVAILABLE;
  if (depth && !(usage & D3DUSAGE_DEPTHSTENCIL) && resource != D3DRTYPE_SURFACE) return D3DERR_NOTAVAILABLE;
  if (block && (volume || (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))) return D3DERR_NOTAVAILABLE;
  if (format_is_palette(format) && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING))) return D3DERR_NOTAVAILABLE;
  /* (the signed formats are textures: no targets, depth or vertex textures; they are the bump maps of the fixed function's bump environment operations) */
  if (format_is_signed(format) && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING))) return D3DERR_NOTAVAILABLE;
  if ((usage & D3DUSAGE_QUERY_LEGACYBUMPMAP) && !format_is_signed(format)) return D3DERR_NOTAVAILABLE;
  if (usage & (D3DUSAGE_DMAP | D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE | D3DUSAGE_QUERY_VERTEXTEXTURE)) return D3DERR_NOTAVAILABLE;
  if ((usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) && !format_is_target(format)) return D3DERR_NOTAVAILABLE;
  /* (the levels of a 2D texture of a format that can be drawn into are made from its top level, as Windows' drivers make them by drawing; of the others the texture
   * is made, with no levels made) */
  if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (!format_is_target(format) || cube || volume)) return D3DOK_NOAUTOGEN;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceMultiSampleType(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE samples,
                                                                DWORD *levels) {
  (void)o; (void)type; (void)windowed;
  if (adapter != 0 || (UINT)samples > D3DMULTISAMPLE_16_SAMPLES) return D3DERR_INVALIDCALL;
  if (!format_is_target(format) && !format_is_depth(format)) { if (levels) *levels = 0; return D3DERR_NOTAVAILABLE; }
  if (samples != D3DMULTISAMPLE_NONE) { if (levels) *levels = 0; return D3DERR_NOTAVAILABLE; }
  if (levels) *levels = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_CheckDepthStencilMatch(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT target, D3DFORMAT depth) {
  (void)o; (void)type;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  if (!display_format(display)) return D3DERR_NOTAVAILABLE;
  return format_is_target(target) && format_is_depth(depth) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
/* What StretchRect converts: from any colour format of a surface the device has into a target's format. */
static HRESULT STDMETHODCALLTYPE d3d_CheckDeviceFormatConversion(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DFORMAT from, D3DFORMAT to) {
  (void)o; (void)type;
  if (adapter != 0) return D3DERR_INVALIDCALL;
  return format_bytes(from) && !format_is_palette(from) && !format_is_signed(from) && format_is_target(to) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE d3d_GetDeviceCaps(Direct3D *o, UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) {
  (void)o;
  if (adapter != 0 || !caps) return D3DERR_INVALIDCALL;
  if (type != D3DDEVTYPE_HAL && type != D3DDEVTYPE_REF && type != D3DDEVTYPE_SW) return D3DERR_INVALIDCALL;
  caps_fill(caps);
  caps->DeviceType = type;
  return D3D_OK;
}
static HMONITOR STDMETHODCALLTYPE d3d_GetAdapterMonitor(Direct3D *o, UINT adapter) {
  (void)o;
  return adapter == 0 ? MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY) : NULL;
}
static HRESULT device_create(Direct3D *o, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS *parameters, const D3DDISPLAYMODEEX *mode, int ex,
                             Dev **out) {
  *out = NULL;
  if (adapter != 0 || !parameters || (type != D3DDEVTYPE_HAL && type != D3DDEVTYPE_REF && type != D3DDEVTYPE_SW)) return D3DERR_INVALIDCALL;
  if (!focus && !parameters->hDeviceWindow && !(ex && parameters->Windowed)) return D3DERR_INVALIDCALL;      /* (a windowed 9Ex device may have no window at all: PresentEx names one) */
  /* (exactly one way of processing vertices) */
  const DWORD vp = behaviour & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING);
  if (vp != D3DCREATE_SOFTWARE_VERTEXPROCESSING && vp != D3DCREATE_HARDWARE_VERTEXPROCESSING && vp != D3DCREATE_MIXED_VERTEXPROCESSING) return D3DERR_INVALIDCALL;
  if (ex && ((mode && parameters->Windowed) || (!mode && !parameters->Windowed))) return D3DERR_INVALIDCALL;
  Dev *d = mem_alloc(sizeof *d);
  if (!d) return E_OUTOFMEMORY;
  dx_init(&d->o, &dev_vtbl, ex ? g_dev_ex_iids : g_dev_iids, dev_destroy);
  InitializeCriticalSection(&d->lock);
  dx_AddRef(OBJ(o));
  d->parent = o;
  d->ex = ex;
  d->creation.AdapterOrdinal = adapter;
  d->creation.DeviceType = type;
  d->creation.hFocusWindow = focus;
  d->creation.BehaviorFlags = behaviour;
  d->software_vp = vp == D3DCREATE_SOFTWARE_VERTEXPROCESSING;
  D3DPRESENT_PARAMETERS pp = *parameters;
  if (mode && !pp.Windowed) { pp.BackBufferWidth = mode->Width; pp.BackBufferHeight = mode->Height; pp.FullScreen_RefreshRateInHz = mode->RefreshRate; }
  HRESULT hr = present_parameters_check(d, &pp, focus);
  if (SUCCEEDED(hr)) hr = dev_start(d, &pp);
  if (FAILED(hr)) { dx_Release(OBJ(d)); return hr == E_OUTOFMEMORY || hr == D3DERR_INVALIDCALL || hr == D3DERR_OUTOFVIDEOMEMORY ? hr : D3DERR_NOTAVAILABLE; }
  *parameters = pp;      /* (the size and the format that were chosen) */
  *out = d;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_CreateDevice(Direct3D *o, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS *parameters, IDirect3DDevice9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  Dev *d = NULL;
  const HRESULT hr = device_create(o, adapter, type, focus, behaviour, parameters, NULL, o->ex, &d);
  *out = (IDirect3DDevice9 *)d;
  return hr;
}
/* (9Ex) */
static HRESULT STDMETHODCALLTYPE d3d_CreateDeviceEx(Direct3D *o, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS *parameters, D3DDISPLAYMODEEX *mode,
                                                   IDirect3DDevice9Ex **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (mode && mode->Size != sizeof *mode) return D3DERR_INVALIDCALL;
  Dev *d = NULL;
  const HRESULT hr = device_create(o, adapter, type, focus, behaviour, parameters, mode, 1, &d);
  *out = (IDirect3DDevice9Ex *)d;
  return hr;
}
static BOOL mode_filter_valid(const D3DDISPLAYMODEFILTER *filter) {
  return filter && filter->Size == sizeof *filter && filter->ScanLineOrdering <= D3DSCANLINEORDERING_INTERLACED;
}
static UINT STDMETHODCALLTYPE d3d_GetAdapterModeCountEx(Direct3D *o, UINT adapter, const D3DDISPLAYMODEFILTER *filter) {
  (void)o;
  if (adapter != 0 || !mode_filter_valid(filter) || filter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED) return 0;
  return display_mode_count(filter->Format);
}
static HRESULT STDMETHODCALLTYPE d3d_EnumAdapterModesEx(Direct3D *o, UINT adapter, const D3DDISPLAYMODEFILTER *filter, UINT index, D3DDISPLAYMODEEX *mode) {
  (void)o;
  if (adapter != 0 || !mode_filter_valid(filter) || !mode || mode->Size != sizeof *mode || !display_format(filter->Format)) return D3DERR_INVALIDCALL;
  if (filter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED || !display_mode_at(filter->Format, index, mode)) return D3DERR_INVALIDCALL;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterDisplayModeEx(Direct3D *o, UINT adapter, D3DDISPLAYMODEEX *mode, D3DDISPLAYROTATION *rotation) {
  (void)o;
  if (adapter != 0 || (mode && mode->Size != sizeof *mode)) return D3DERR_INVALIDCALL;
  if (mode) display_current(mode);
  if (rotation) *rotation = display_rotation();
  return D3D_OK;
}
/* The LUID of the adapter (the first of the kernel's, which D3D12CreateDevice takes for its default). */
static HRESULT STDMETHODCALLTYPE d3d_GetAdapterLUID(Direct3D *o, UINT adapter, LUID *luid) {
  (void)o;
  if (adapter != 0 || !luid) return D3DERR_INVALIDCALL;
  KmtAdapter adapters[1];
  if (kmt_enum_adapters(adapters, 1, NULL) < 1) return D3DERR_NOTAVAILABLE;
  *luid = adapters[0].luid;
  kmt_close_adapter(adapters[0].handle);
  return D3D_OK;
}

/* ---- the entry points ------------------------------------------------------------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_private_lock);
  return TRUE;
}

static Direct3D *direct3d_make(int ex) {
  Direct3D *o = mem_alloc(sizeof *o);
  if (!o) return NULL;
  dx_init(&o->o, &d3d_vtbl, ex ? g_d3d_ex_iids : g_d3d_iids, NULL);
  o->ex = ex;
  return o;
}
EXPORT IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version) {
  (void)sdk_version;
  return (IDirect3D9 *)direct3d_make(0);
}
EXPORT HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **out) {
  (void)sdk_version;
  if (!out) return D3DERR_INVALIDCALL;
  *out = (IDirect3D9Ex *)direct3d_make(1);
  return *out ? D3D_OK : E_OUTOFMEMORY;
}
