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

/* d3d8.dll: render states, texture stage states and state blocks. Included by d3d8.c.
 *
 * Render states have Direct3D 9's numbers but five: D3DRS_LINEPATTERN and D3DRS_ZVISIBLE (which Direct3D 8 itself does not draw with) are kept
 * here, D3DRS_ZBIAS (0 .. 16, in units of the depth buffer) is set as Direct3D 9's D3DRS_DEPTHBIAS, D3DRS_SOFTWAREVERTEXPROCESSING is
 * SetSoftwareVertexProcessing and D3DRS_PATCHSEGMENTS SetNPatchMode; D3DRS_EDGEANTIALIAS is D3DRS_ANTIALIASEDLINEENABLE. The texture stage
 * states that are sampler states in Direct3D 9 (ADDRESSU/V/W, BORDERCOLOR, MAG/MIN/MIPFILTER, MIPMAPLODBIAS, MAXMIPLEVEL, MAXANISOTROPY) are
 * SetSamplerState of the stage's sampler. A state block is a Direct3D 9 state block and what it holds of Direct3D 8's own state (State8). */

#define STAGES8 8u

enum { RS_INVALID = 0, RS_SAME, RS_EXTRA, RS_MAPPED };

/* What a Direct3D 8 render state is: the same in Direct3D 9, Direct3D 8's own (`*extra`: which), Direct3D 9's under another number (`*to`), or no
 * render state of Direct3D 8. */
static int render_state_kind(DWORD state, UINT *extra, DWORD *to) {
  switch (state) {
  case D3DRS_LINEPATTERN: *extra = EXTRA_LINEPATTERN; return RS_EXTRA;
  case D3DRS_ZVISIBLE: *extra = EXTRA_ZVISIBLE; return RS_EXTRA;
  case D3DRS_ZBIAS: *extra = EXTRA_ZBIAS; return RS_EXTRA;
  case D3DRS_SOFTWAREVERTEXPROCESSING: *extra = EXTRA_SOFTWAREVERTEXPROCESSING; return RS_EXTRA;
  case D3DRS_PATCHSEGMENTS: *extra = EXTRA_PATCHSEGMENTS; return RS_EXTRA;
  case D3DRS_EDGEANTIALIAS: *to = D3D9_RS_ANTIALIASEDLINEENABLE; return RS_MAPPED;
  case D3DRS_ZENABLE: case D3DRS_FILLMODE: case D3DRS_SHADEMODE: case D3DRS_ZWRITEENABLE: case D3DRS_ALPHATESTENABLE: case D3DRS_LASTPIXEL:
  case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_CULLMODE: case D3DRS_ZFUNC: case D3DRS_ALPHAREF: case D3DRS_ALPHAFUNC: case D3DRS_DITHERENABLE:
  case D3DRS_ALPHABLENDENABLE: case D3DRS_FOGENABLE: case D3DRS_SPECULARENABLE: case D3DRS_FOGCOLOR: case D3DRS_FOGTABLEMODE: case D3DRS_FOGSTART:
  case D3DRS_FOGEND: case D3DRS_FOGDENSITY: case D3DRS_RANGEFOGENABLE: case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL:
  case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC: case D3DRS_STENCILREF: case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_TEXTUREFACTOR:
  case D3DRS_WRAP0: case D3DRS_WRAP1: case D3DRS_WRAP2: case D3DRS_WRAP3: case D3DRS_WRAP4: case D3DRS_WRAP5: case D3DRS_WRAP6: case D3DRS_WRAP7:
  case D3DRS_CLIPPING: case D3DRS_LIGHTING: case D3DRS_AMBIENT: case D3DRS_FOGVERTEXMODE: case D3DRS_COLORVERTEX: case D3DRS_LOCALVIEWER:
  case D3DRS_NORMALIZENORMALS: case D3DRS_DIFFUSEMATERIALSOURCE: case D3DRS_SPECULARMATERIALSOURCE: case D3DRS_AMBIENTMATERIALSOURCE:
  case D3DRS_EMISSIVEMATERIALSOURCE: case D3DRS_VERTEXBLEND: case D3DRS_CLIPPLANEENABLE: case D3DRS_POINTSIZE: case D3DRS_POINTSIZE_MIN:
  case D3DRS_POINTSPRITEENABLE: case D3DRS_POINTSCALEENABLE: case D3DRS_POINTSCALE_A: case D3DRS_POINTSCALE_B: case D3DRS_POINTSCALE_C:
  case D3DRS_MULTISAMPLEANTIALIAS: case D3DRS_MULTISAMPLEMASK: case D3DRS_PATCHEDGESTYLE: case D3DRS_DEBUGMONITORTOKEN: case D3DRS_POINTSIZE_MAX:
  case D3DRS_INDEXEDVERTEXBLENDENABLE: case D3DRS_COLORWRITEENABLE: case D3DRS_TWEENFACTOR: case D3DRS_BLENDOP:
  case D3DRS_POSITIONORDER: case D3DRS_NORMALORDER:      /* (Direct3D 9's POSITIONDEGREE and NORMALDEGREE: the same numbers and values) */
    return RS_SAME;
  default:
    return RS_INVALID;
  }
}

static float float_of(DWORD bits) { union { DWORD d; float f; } u; u.d = bits; return u.f; }
static DWORD bits_of(float f) { union { DWORD d; float f; } u; u.f = f; return u.d; }

/* D3DRS_ZBIAS as Direct3D 9's depth bias: so many units of the depth buffer that is set, towards the eye. */
static DWORD depth_bias_of(Device8 *d, DWORD zbias) {
  if (!zbias) return 0;
  UINT bits = 24;
  IDirect3DSurface9 *ds = NULL;
  if (SUCCEEDED(CALL9(D9(d), GetDepthStencilSurface, &ds)) && ds) {
    D3DSURFACE_DESC9 desc;
    if (SUCCEEDED(CALL9(ds, GetDesc, &desc))) bits = format_depth_bits(desc.Format);
    CALL9(ds, Release);
  }
  float unit = 1.0f;
  for (UINT i = 0; i < bits; ++i) unit *= 0.5f;
  return bits_of(-(float)zbias * unit);
}

/* Sets one of Direct3D 8's own render states on the Direct3D 9 device (for the ones it has a form of). */
static HRESULT extra_apply(Device8 *d, UINT extra, DWORD value) {
  switch (extra) {
  case EXTRA_ZBIAS: return CALL9(D9(d), SetRenderState, (D3DRENDERSTATETYPE)D3D9_RS_DEPTHBIAS, depth_bias_of(d, value));
  case EXTRA_SOFTWAREVERTEXPROCESSING: return CALL9(D9(d), SetSoftwareVertexProcessing, value ? TRUE : FALSE);
  case EXTRA_PATCHSEGMENTS: return CALL9(D9(d), SetNPatchMode, float_of(value));
  default: return D3D_OK;      /* (LINEPATTERN and ZVISIBLE: kept, nothing draws with them) */
  }
}

static HRESULT STDMETHODCALLTYPE dev_SetRenderState(Device8 *d, D3DRENDERSTATETYPE state, DWORD value) {
  UINT extra = 0;
  DWORD to = 0;
  switch (render_state_kind(state, &extra, &to)) {
  case RS_SAME: return CALL9(D9(d), SetRenderState, state, value);
  case RS_MAPPED: return CALL9(D9(d), SetRenderState, (D3DRENDERSTATETYPE)to, value);
  case RS_EXTRA: {
    if (extra == EXTRA_SOFTWAREVERTEXPROCESSING && value && !(d->behaviour & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)))
      return D3DERR_INVALIDCALL;      /* (a device of hardware vertex processing has no software processing to switch to) */
    EnterCriticalSection(&d->lock);
    const HRESULT hr = extra_apply(d, extra, value);
    if (SUCCEEDED(hr)) state_target(d, HAS_EXTRA(extra))->extra[extra] = value;
    LeaveCriticalSection(&d->lock);
    return hr;
  }
  default:
    return D3DERR_INVALIDCALL;
  }
}
static HRESULT STDMETHODCALLTYPE dev_GetRenderState(Device8 *d, D3DRENDERSTATETYPE state, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  UINT extra = 0;
  DWORD to = 0;
  switch (render_state_kind(state, &extra, &to)) {
  case RS_SAME: return CALL9(D9(d), GetRenderState, state, value);
  case RS_MAPPED: return CALL9(D9(d), GetRenderState, (D3DRENDERSTATETYPE)to, value);
  case RS_EXTRA: *value = d->state.extra[extra]; return D3D_OK;
  default: return D3DERR_INVALIDCALL;
  }
}

/* The Direct3D 9 sampler state of a Direct3D 8 texture stage state, or 0 for one that is a texture stage state in both. */
static DWORD sampler_state_of(DWORD type) {
  switch (type) {
  case D3DTSS_ADDRESSU: return D3DSAMP_ADDRESSU;
  case D3DTSS_ADDRESSV: return D3DSAMP_ADDRESSV;
  case D3DTSS_ADDRESSW: return D3DSAMP_ADDRESSW;
  case D3DTSS_BORDERCOLOR: return D3DSAMP_BORDERCOLOR;
  case D3DTSS_MAGFILTER: return D3DSAMP_MAGFILTER;
  case D3DTSS_MINFILTER: return D3DSAMP_MINFILTER;
  case D3DTSS_MIPFILTER: return D3DSAMP_MIPFILTER;
  case D3DTSS_MIPMAPLODBIAS: return D3DSAMP_MIPMAPLODBIAS;
  case D3DTSS_MAXMIPLEVEL: return D3DSAMP_MAXMIPLEVEL;
  case D3DTSS_MAXANISOTROPY: return D3DSAMP_MAXANISOTROPY;
  default: return 0;
  }
}
static BOOL stage_state_valid(DWORD stage, DWORD type) { return stage < STAGES8 && type >= D3DTSS_COLOROP && type <= D3DTSS_RESULTARG && type != 12; }

static HRESULT STDMETHODCALLTYPE dev_SetTextureStageState(Device8 *d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (!stage_state_valid(stage, type)) return D3DERR_INVALIDCALL;
  const DWORD sampler = sampler_state_of(type);
  if (sampler) return CALL9(D9(d), SetSamplerState, stage, (D3DSAMPLERSTATETYPE)sampler, value);
  return CALL9(D9(d), SetTextureStageState, stage, type, value);
}
static HRESULT STDMETHODCALLTYPE dev_GetTextureStageState(Device8 *d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  if (!value || !stage_state_valid(stage, type)) return D3DERR_INVALIDCALL;
  const DWORD sampler = sampler_state_of(type);
  if (sampler) return CALL9(D9(d), GetSamplerState, stage, (D3DSAMPLERSTATETYPE)sampler, value);
  return CALL9(D9(d), GetTextureStageState, stage, type, value);
}

/* The state Direct3D 8 starts a device with (and Reset gives it again) of what is kept here. */
static void state8_defaults(Device8 *d) {
  memset(&d->state, 0, sizeof d->state);
  d->state.extra[EXTRA_SOFTWAREVERTEXPROCESSING] = (d->behaviour & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? TRUE : FALSE;
  d->state.extra[EXTRA_PATCHSEGMENTS] = bits_of(1.0f);
}

/* The render states whose first value Direct3D 8 has otherwise than Direct3D 9 (D3DRS_POINTSIZE_MIN: 0, not 1), set on a new or reset device. */
static void defaults8_apply(Device8 *d) {
  CALL9(D9(d), SetRenderState, D3DRS_POINTSIZE_MIN, 0);
}

/* ---- state blocks ----------------------------------------------------------------------------------------------------------------------------------- */

static UINT state_block_parts(D3DSTATEBLOCKTYPE type) {
  switch (type) {
  case D3DSBT_ALL: return HAS_ALL;
  case D3DSBT_PIXELSTATE: return HAS_PS | HAS_EXTRA(EXTRA_LINEPATTERN) | HAS_EXTRA(EXTRA_ZBIAS);
  case D3DSBT_VERTEXSTATE: return HAS_VS | HAS_EXTRA(EXTRA_SOFTWAREVERTEXPROCESSING) | HAS_EXTRA(EXTRA_PATCHSEGMENTS);
  default: return 0;
  }
}
static void state_block_free(StateBlock8 *b) {
  if (!b) return;
  if (b->sb9) CALL9(b->sb9, Release);
  mem_free(b);
}
/* Copies the parts `has` of one State8 into another. */
static void state8_copy(State8 *to, const State8 *from, UINT has) {
  if (has & HAS_VS) to->vs = from->vs;
  if (has & HAS_PS) to->ps = from->ps;
  if (has & HAS_BASE) to->base_vertex = from->base_vertex;
  for (UINT i = 0; i < EXTRA_STATES; ++i)
    if (has & HAS_EXTRA(i)) to->extra[i] = from->extra[i];
}

static HRESULT STDMETHODCALLTYPE dev_CreateStateBlock(Device8 *d, D3DSTATEBLOCKTYPE type, DWORD *token) {
  if (!token) return D3DERR_INVALIDCALL;
  const UINT parts = state_block_parts(type);
  if (!parts) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = d->recording ? D3DERR_INVALIDCALL : D3D_OK;
  StateBlock8 *b = SUCCEEDED(hr) ? mem_alloc(sizeof *b) : NULL;
  if (SUCCEEDED(hr) && !b) hr = E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) hr = CALL9(D9(d), CreateStateBlock, type, &b->sb9);
  DWORD h = 0;
  if (SUCCEEDED(hr)) {
    b->has = parts;
    state8_copy(&b->state, &d->state, parts);
    h = handle_add(&d->state_blocks, b);
    if (!h) hr = E_OUTOFMEMORY;
  }
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { state_block_free(b); return hr; }
  *token = h;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_BeginStateBlock(Device8 *d) {
  EnterCriticalSection(&d->lock);
  HRESULT hr = d->recording ? D3DERR_INVALIDCALL : D3D_OK;
  StateBlock8 *b = SUCCEEDED(hr) ? mem_alloc(sizeof *b) : NULL;
  if (SUCCEEDED(hr) && !b) hr = E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) hr = CALL9(D9(d), BeginStateBlock);
  if (SUCCEEDED(hr)) d->recording = b;
  else mem_free(b);
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_EndStateBlock(Device8 *d, DWORD *token) {
  if (!token) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  StateBlock8 *b = d->recording;
  HRESULT hr = b ? D3D_OK : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr)) {
    d->recording = NULL;
    hr = CALL9(D9(d), EndStateBlock, &b->sb9);
  }
  DWORD h = 0;
  if (SUCCEEDED(hr)) {
    h = handle_add(&d->state_blocks, b);
    if (!h) hr = E_OUTOFMEMORY;
  }
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { if (b) state_block_free(b); return hr; }
  *token = h;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_ApplyStateBlock(Device8 *d, DWORD token) {
  if (!token && !d->recording) return D3D_OK;      /* (no block: nothing to apply, as Direct3D 8 answers it) */
  EnterCriticalSection(&d->lock);
  StateBlock8 *b = d->recording ? NULL : handle_get(&d->state_blocks, token);
  HRESULT hr = b ? CALL9(b->sb9, Apply) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr)) {
    state8_copy(&d->state, &b->state, b->has);
    /* (a shader the block names that was deleted since: none) */
    if ((b->has & HAS_VS) && d->state.vs > VS_HANDLE_BASE && !vertex_shader_of(d, d->state.vs)) d->state.vs = 0;
    if ((b->has & HAS_PS) && d->state.ps && !pixel_shader_of(d, d->state.ps)) d->state.ps = 0;
    /* Direct3D 9's block set the device's depth bias; the others of Direct3D 8's own are set from what the block holds. */
    if (b->has & HAS_EXTRA(EXTRA_SOFTWAREVERTEXPROCESSING)) extra_apply(d, EXTRA_SOFTWAREVERTEXPROCESSING, d->state.extra[EXTRA_SOFTWAREVERTEXPROCESSING]);
    if (b->has & HAS_EXTRA(EXTRA_PATCHSEGMENTS)) extra_apply(d, EXTRA_PATCHSEGMENTS, d->state.extra[EXTRA_PATCHSEGMENTS]);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_CaptureStateBlock(Device8 *d, DWORD token) {
  EnterCriticalSection(&d->lock);
  StateBlock8 *b = d->recording ? NULL : handle_get(&d->state_blocks, token);
  HRESULT hr = b ? CALL9(b->sb9, Capture) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr)) state8_copy(&b->state, &d->state, b->has);
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_DeleteStateBlock(Device8 *d, DWORD token) {
  EnterCriticalSection(&d->lock);
  StateBlock8 *b = handle_remove(&d->state_blocks, token);
  LeaveCriticalSection(&d->lock);
  if (!b) return D3DERR_INVALIDCALL;
  state_block_free(b);
  return D3D_OK;
}
