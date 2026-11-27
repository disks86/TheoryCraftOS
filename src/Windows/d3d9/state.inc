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

/* Part of d3d9.c: the state a program sets on the device - render, sampler and texture stage states, the transforms, the viewport, the lights, what is
 * bound, the constants - and state blocks. Setting only remembers; a draw reads it (draw.inc). Between BeginStateBlock and EndStateBlock what is set goes
 * into the block being recorded and not into the device, as Direct3D 9 has it. */

static const D3DMATRIX g_identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
static DWORD float_bits(float f) { DWORD v; memcpy(&v, &f, 4); return v; }

/* The render states of the two kinds of state block (D3DSBT_VERTEXSTATE, D3DSBT_PIXELSTATE: Direct3D 9's lists; a state may be in both). */
static const BYTE g_vertex_render_states[] = {
    D3DRS_ADAPTIVETESS_W, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y, D3DRS_ADAPTIVETESS_Z, D3DRS_AMBIENT, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_CLIPPING,
    D3DRS_CLIPPLANEENABLE, D3DRS_COLORVERTEX, D3DRS_CULLMODE, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_ENABLEADAPTIVETESSELLATION,
    D3DRS_FOGCOLOR, D3DRS_FOGDENSITY, D3DRS_FOGENABLE, D3DRS_FOGEND, D3DRS_FOGSTART, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE, D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_LIGHTING, D3DRS_LOCALVIEWER, D3DRS_MAXTESSELLATIONLEVEL, D3DRS_MINTESSELLATIONLEVEL, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK,
    D3DRS_NORMALDEGREE, D3DRS_NORMALIZENORMALS, D3DRS_PATCHEDGESTYLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C, D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSIZE, D3DRS_POINTSIZE_MAX, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE, D3DRS_POSITIONDEGREE, D3DRS_RANGEFOGENABLE, D3DRS_SHADEMODE,
    D3DRS_SPECULARENABLE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_TWEENFACTOR, D3DRS_VERTEXBLEND};
static const BYTE g_pixel_render_states[] = {
    D3DRS_ALPHABLENDENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHAREF, D3DRS_ALPHATESTENABLE, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_BLENDFACTOR, D3DRS_BLENDOP, D3DRS_BLENDOPALPHA,
    D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILZFAIL, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3, D3DRS_DEPTHBIAS, D3DRS_DESTBLEND, D3DRS_DESTBLENDALPHA, D3DRS_DITHERENABLE, D3DRS_FILLMODE, D3DRS_FOGDENSITY, D3DRS_FOGEND,
    D3DRS_FOGSTART, D3DRS_LASTPIXEL, D3DRS_SCISSORTESTENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SHADEMODE, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_SRCBLEND,
    D3DRS_SRCBLENDALPHA, D3DRS_SRGBWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILFUNC, D3DRS_STENCILMASK, D3DRS_STENCILPASS, D3DRS_STENCILREF,
    D3DRS_STENCILWRITEMASK, D3DRS_STENCILZFAIL, D3DRS_TEXTUREFACTOR, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12,
    D3DRS_WRAP13, D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_ZENABLE,
    D3DRS_ZFUNC, D3DRS_ZWRITEENABLE};
/* ... and their texture stage and sampler states (D3DTSS_TEXTURETRANSFORMFLAGS is in both). */
#define VERTEX_STAGE_STATES ((1ull << D3DTSS_TEXCOORDINDEX) | (1ull << D3DTSS_TEXTURETRANSFORMFLAGS))
#define PIXEL_STAGE_STATES                                                                                                                                       \
  ((1ull << D3DTSS_COLOROP) | (1ull << D3DTSS_COLORARG1) | (1ull << D3DTSS_COLORARG2) | (1ull << D3DTSS_ALPHAOP) | (1ull << D3DTSS_ALPHAARG1) |                   \
   (1ull << D3DTSS_ALPHAARG2) | (1ull << D3DTSS_BUMPENVMAT00) | (1ull << D3DTSS_BUMPENVMAT01) | (1ull << D3DTSS_BUMPENVMAT10) | (1ull << D3DTSS_BUMPENVMAT11) | \
   (1ull << D3DTSS_BUMPENVLSCALE) | (1ull << D3DTSS_BUMPENVLOFFSET) | (1ull << D3DTSS_TEXTURETRANSFORMFLAGS) | (1ull << D3DTSS_COLORARG0) |                    \
   (1ull << D3DTSS_ALPHAARG0) | (1ull << D3DTSS_RESULTARG) | (1ull << D3DTSS_CONSTANT))
#define VERTEX_SAMPLER_STATES (1u << D3DSAMP_DMAPOFFSET)
#define PIXEL_SAMPLER_STATES (((1u << SAMPLER_STATES) - 2u) & ~VERTEX_SAMPLER_STATES)

/* ---- the lights ------------------------------------------------------------------------------------------------------------------------------------- */

/* The light of a number, or NULL. */
static Light *light_find(const State *s, DWORD index) {
  UINT low = 0, high = s->light_count;
  while (low < high) {
    const UINT mid = low + (high - low) / 2;
    if (s->lights[mid].index == index) return &s->lights[mid];
    if (s->lights[mid].index < index) low = mid + 1; else high = mid;
  }
  return NULL;
}
/* The light of a number, made (as Direct3D 9's default light: white, directional, along z, off) if there was none; NULL without memory. */
static Light *light_put(State *s, DWORD index) {
  Light *l = light_find(s, index);
  if (l) return l;
  if (s->light_count == s->light_capacity) {
    const UINT capacity = s->light_capacity ? s->light_capacity * 2 : 8;
    if (capacity > 0x00FFFFFFu) return NULL;
    Light *more = mem_alloc(sizeof(Light) * capacity);
    if (!more) return NULL;
    if (s->light_count) memcpy(more, s->lights, sizeof(Light) * s->light_count);
    mem_free(s->lights);
    s->lights = more;
    s->light_capacity = capacity;
  }
  UINT at = 0;
  while (at < s->light_count && s->lights[at].index < index) ++at;
  memmove(&s->lights[at + 1], &s->lights[at], sizeof(Light) * (s->light_count - at));
  ++s->light_count;
  l = &s->lights[at];
  memset(l, 0, sizeof *l);
  l->index = index;
  l->light.Type = D3DLIGHT_DIRECTIONAL;
  l->light.Diffuse.r = l->light.Diffuse.g = l->light.Diffuse.b = 1.0f;
  l->light.Direction.z = 1.0f;
  return l;
}

/* ---- the state of a device -------------------------------------------------------------------------------------------------------------------------- */

/* The state of a new device (and of one that was reset). `depth`: it has a depth buffer of its own. */
static void state_defaults(State *s, BOOL depth, UINT width, UINT height) {
  memset(s->rs, 0, sizeof s->rs);
  for (UINT i = 0; i < STREAMS; ++i) { s->stream[i].freq = 1; s->stream[i].offset = s->stream[i].stride = 0; }
  DWORD *rs = s->rs;
  rs[D3DRS_ZENABLE] = depth ? D3DZB_TRUE : D3DZB_FALSE;
  rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  rs[D3DRS_ZWRITEENABLE] = TRUE;
  rs[D3DRS_LASTPIXEL] = TRUE;
  rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_FOGEND] = rs[D3DRS_FOGDENSITY] = float_bits(1.0f);
  rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
  rs[D3DRS_TEXTUREFACTOR] = rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFFu;
  rs[D3DRS_CLIPPING] = TRUE;
  rs[D3DRS_LIGHTING] = TRUE;
  rs[D3DRS_COLORVERTEX] = TRUE;
  rs[D3DRS_LOCALVIEWER] = TRUE;
  rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  rs[D3DRS_AMBIENTMATERIALSOURCE] = rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  rs[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
  rs[D3DRS_POINTSIZE] = rs[D3DRS_POINTSIZE_MIN] = float_bits(1.0f);
  rs[D3DRS_POINTSCALE_A] = float_bits(1.0f);
  rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
  rs[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
  rs[D3DRS_DEBUGMONITORTOKEN] = 0xBAADCAFEu;
  rs[D3DRS_POINTSIZE_MAX] = float_bits(MAX_POINT_SIZE);
  rs[D3DRS_COLORWRITEENABLE] = rs[D3DRS_COLORWRITEENABLE1] = rs[D3DRS_COLORWRITEENABLE2] = rs[D3DRS_COLORWRITEENABLE3] = 0xF;
  rs[D3DRS_BLENDOP] = rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  rs[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
  rs[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
  rs[D3DRS_MINTESSELLATIONLEVEL] = rs[D3DRS_MAXTESSELLATIONLEVEL] = float_bits(1.0f);
  rs[D3DRS_ADAPTIVETESS_Z] = float_bits(1.0f);
  rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  memset(s->sampler, 0, sizeof s->sampler);
  memset(s->stage, 0, sizeof s->stage);
  for (UINT i = 0; i < SAMPLERS; ++i) {
    s->sampler[i][D3DSAMP_ADDRESSU] = s->sampler[i][D3DSAMP_ADDRESSV] = s->sampler[i][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    s->sampler[i][D3DSAMP_MAGFILTER] = s->sampler[i][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    s->sampler[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    s->sampler[i][D3DSAMP_MAXANISOTROPY] = 1;
    s->sampler[i][D3DSAMP_DMAPOFFSET] = 0;
  }
  for (UINT i = 0; i < STAGES; ++i) {
    s->stage[i][D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    s->stage[i][D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    s->stage[i][D3DTSS_COLORARG1] = s->stage[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    s->stage[i][D3DTSS_COLORARG2] = s->stage[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    s->stage[i][D3DTSS_COLORARG0] = s->stage[i][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    s->stage[i][D3DTSS_TEXCOORDINDEX] = i;
    s->stage[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    s->texture[i] = g_identity;
  }
  for (UINT i = 0; i < WORLD_MATRICES; ++i) s->world[i] = g_identity;
  s->view = s->projection = g_identity;
  const D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
  s->viewport = viewport;
  SetRect(&s->scissor, 0, 0, (int)width, (int)height);
  memset(&s->material, 0, sizeof s->material);
  s->light_count = 0;
  memset(s->clip_plane, 0, sizeof s->clip_plane);
  s->npatch = 0.0f;
  s->fvf = 0;
  memset(s->vs_float, 0, sizeof s->vs_float);
  memset(s->ps_float, 0, sizeof s->ps_float);
  memset(s->vs_int, 0, sizeof s->vs_int);
  memset(s->ps_int, 0, sizeof s->ps_int);
  s->vs_bool = s->ps_bool = 0;
}
/* Gives back what a state holds (the counts of what is set, and its lights). */
static void state_release(State *s) {
  bind(&s->decl, NULL);
  bind(&s->vs, NULL);
  bind(&s->ps, NULL);
  bind(&s->indices, NULL);
  for (UINT i = 0; i < STREAMS; ++i) bind(&s->stream[i].buffer, NULL);
  for (UINT i = 0; i < SAMPLERS; ++i) bind(&s->tex[i], NULL);
  mem_free(s->lights);
  s->lights = NULL;
  s->light_count = s->light_capacity = 0;
}

/* ---- which parts a state block has ------------------------------------------------------------------------------------------------------------------ */

static void mask_bit(DWORD *bits, UINT n) { bits[n / 32] |= 1u << (n % 32); }
static BOOL mask_has(const DWORD *bits, UINT n) { return (bits[n / 32] >> (n % 32)) & 1; }
static void mask_vertex(StateMask *m) {
  for (UINT i = 0; i < sizeof g_vertex_render_states; ++i) mask_bit(m->rs, g_vertex_render_states[i]);
  for (UINT i = 0; i < STAGES; ++i) m->stage[i] |= VERTEX_STAGE_STATES;
  for (UINT i = 0; i < SAMPLERS; ++i) m->sampler[i] |= VERTEX_SAMPLER_STATES;
  memset(m->vs_float, 0xFF, sizeof m->vs_float);
  m->vs_int = m->vs_bool = 0xFFFF;
  m->vs = m->decl = m->fvf = m->npatch = TRUE;
  m->all_lights = TRUE;
}
static void mask_pixel(StateMask *m) {
  for (UINT i = 0; i < sizeof g_pixel_render_states; ++i) mask_bit(m->rs, g_pixel_render_states[i]);
  for (UINT i = 0; i < STAGES; ++i) m->stage[i] |= PIXEL_STAGE_STATES;
  for (UINT i = 0; i < SAMPLERS; ++i) m->sampler[i] |= PIXEL_SAMPLER_STATES;
  memset(m->ps_float, 0xFF, sizeof m->ps_float);
  m->ps_int = m->ps_bool = 0xFFFF;
  m->ps = TRUE;
}
static void mask_all(StateMask *m) {
  mask_vertex(m);
  mask_pixel(m);
  for (UINT i = 1; i < RENDER_STATES; ++i) if (i != D3DRS_DEBUGMONITORTOKEN) mask_bit(m->rs, i);
  for (UINT i = 0; i < STAGES; ++i) m->stage[i] = ((1ull << STAGE_STATES) - 2u);
  for (UINT i = 0; i < SAMPLERS; ++i) m->sampler[i] = (1u << SAMPLER_STATES) - 2u;
  memset(m->transform, 0xFF, sizeof m->transform);
  m->streams = m->stream_freqs = (1u << STREAMS) - 1u;
  m->textures = (1u << SAMPLERS) - 1u;
  m->clip_planes = (1u << CLIP_PLANES) - 1u;
  m->viewport = m->scissor = m->material = m->indices = TRUE;
}
static BOOL mask_add_light(StateMask *m, DWORD index) {
  if (m->all_lights) return TRUE;
  for (UINT i = 0; i < m->light_count; ++i) if (m->light[i] == index) return TRUE;
  if (m->light_count == m->light_capacity) {
    const UINT capacity = m->light_capacity ? m->light_capacity * 2 : 8;
    DWORD *more = mem_alloc(sizeof(DWORD) * capacity);
    if (!more) return FALSE;
    if (m->light_count) memcpy(more, m->light, sizeof(DWORD) * m->light_count);
    mem_free(m->light);
    m->light = more;
    m->light_capacity = capacity;
  }
  m->light[m->light_count++] = index;
  return TRUE;
}

/* Copies the parts `m` names from one state to another, with the counts of what is set moved. `to_device`: `to` is the device's (its targets are set again). */
static void lights_copy(State *to, const State *from, const Light *l) {
  Light *t = light_put(to, l->index);
  if (t) *t = *l;
  (void)from;
}
static void state_transfer(Dev *d, State *to, const State *from, const StateMask *m, BOOL to_device) {
  for (UINT i = 0; i < RENDER_STATES; ++i) if (mask_has(m->rs, i)) to->rs[i] = from->rs[i];
  for (UINT i = 0; i < SAMPLERS; ++i)
    for (UINT k = 1; k < SAMPLER_STATES; ++k) if ((m->sampler[i] >> k) & 1) to->sampler[i][k] = from->sampler[i][k];
  for (UINT i = 0; i < STAGES; ++i)
    for (UINT k = 1; k < STAGE_STATES; ++k) if ((m->stage[i] >> k) & 1) to->stage[i][k] = from->stage[i][k];
  for (UINT i = 0; i < TRANSFORMS; ++i) {
    if (!mask_has(m->transform, i)) continue;
    if (i == D3DTS_VIEW) to->view = from->view;
    else if (i == D3DTS_PROJECTION) to->projection = from->projection;
    else if (i >= D3DTS_TEXTURE0 && i <= D3DTS_TEXTURE7) to->texture[i - D3DTS_TEXTURE0] = from->texture[i - D3DTS_TEXTURE0];
    else if (i >= 256) to->world[i - 256] = from->world[i - 256];
  }
  if (m->viewport) to->viewport = from->viewport;
  if (m->scissor) to->scissor = from->scissor;
  if (m->material) to->material = from->material;
  if (m->npatch) to->npatch = from->npatch;
  for (UINT i = 0; i < CLIP_PLANES; ++i) if ((m->clip_planes >> i) & 1) memcpy(to->clip_plane[i], from->clip_plane[i], sizeof to->clip_plane[i]);
  if (m->all_lights) for (UINT i = 0; i < from->light_count; ++i) lights_copy(to, from, &from->lights[i]);
  else
    for (UINT i = 0; i < m->light_count; ++i) {
      const Light *l = light_find(from, m->light[i]);
      if (l) lights_copy(to, from, l);
    }
  if (m->fvf) to->fvf = from->fvf;
  if (m->decl && (from->decl || !to_device)) bind(&to->decl, from->decl);      /* (a block that has no declaration leaves the device's as it is when applied, as Windows does: test_vertex_declaration_stateblock) */
  if (m->vs) bind(&to->vs, from->vs);
  if (m->ps) bind(&to->ps, from->ps);
  if (m->indices) bind(&to->indices, from->indices);
  for (UINT i = 0; i < STREAMS; ++i) {
    if ((m->streams >> i) & 1) {
      bind(&to->stream[i].buffer, from->stream[i].buffer);
      to->stream[i].offset = from->stream[i].offset;
      to->stream[i].stride = from->stream[i].stride;
    }
    if ((m->stream_freqs >> i) & 1) to->stream[i].freq = from->stream[i].freq;
  }
  for (UINT i = 0; i < SAMPLERS; ++i) if ((m->textures >> i) & 1) bind(&to->tex[i], from->tex[i]);
  for (UINT i = 0; i < VS_FLOATS; ++i) if (mask_has(m->vs_float, i)) memcpy(to->vs_float[i], from->vs_float[i], 16);
  for (UINT i = 0; i < PS_FLOATS; ++i) if (mask_has(m->ps_float, i)) memcpy(to->ps_float[i], from->ps_float[i], 16);
  for (UINT i = 0; i < 16; ++i) {
    if ((m->vs_int >> i) & 1) memcpy(to->vs_int[i], from->vs_int[i], 16);
    if ((m->ps_int >> i) & 1) memcpy(to->ps_int[i], from->ps_int[i], 16);
  }
  to->vs_bool = (to->vs_bool & ~m->vs_bool) | (from->vs_bool & m->vs_bool);
  to->ps_bool = (to->ps_bool & ~m->ps_bool) | (from->ps_bool & m->ps_bool);
  if (to_device) d->targets_dirty = 1;
}

/* Where a Set method writes: the device's state, or the block being recorded (with what it sets marked in its mask). */
static State *state_for_set(Dev *d, StateMask **mask) {
  if (d->recording) { *mask = &d->recording->mask; return &d->recording->state; }
  *mask = NULL;
  return &d->state;
}

/* ---- render, sampler and stage states -------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_SetRenderState(Dev *d, D3DRENDERSTATETYPE state, DWORD value) {
  if ((UINT)state >= RENDER_STATES || state == 0) return D3D_OK;      /* (a state Direct3D 9 does not have: ignored, as it does) */
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->rs[state] = value;
  if (m) mask_bit(m->rs, state);
  else if (state == D3DRS_SCISSORTESTENABLE) d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetRenderState(Dev *d, D3DRENDERSTATETYPE state, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  *value = (UINT)state < RENDER_STATES ? d->state.rs[state] : 0;
  return D3D_OK;
}
/* The slot of a sampler number (0 .. 15, D3DDMAPSAMPLER, D3DVERTEXTEXTURESAMPLER0 .. 3); SAMPLERS for one Direct3D 9 does not have. */
static UINT sampler_slot(DWORD sampler) {
  if (sampler < TEXTURE_SLOTS) return sampler;
  if (sampler >= D3DDMAPSAMPLER && sampler <= D3DVERTEXTEXTURESAMPLER3) return TEXTURE_SLOTS + (sampler - D3DDMAPSAMPLER);
  return SAMPLERS;
}
static HRESULT STDMETHODCALLTYPE dev_SetSamplerState(Dev *d, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  const UINT slot = sampler_slot(sampler);
  if (slot >= SAMPLERS || (UINT)type >= SAMPLER_STATES || type == 0) return D3D_OK;
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->sampler[slot][type] = value;
  if (m) m->sampler[slot] |= 1u << type;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetSamplerState(Dev *d, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  const UINT slot = sampler_slot(sampler);
  *value = slot < SAMPLERS && (UINT)type < SAMPLER_STATES ? d->state.sampler[slot][type] : 0;
  return D3D_OK;
}
static void stage_state_set(Dev *d, DWORD stage, UINT type, DWORD value) {
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->stage[stage][type] = value;
  if (m) m->stage[stage] |= 1ull << type;
}
/* (a type Direct3D 9 does not have is ignored - TCOS_TSS_KEY_KILL too, which is ddraw.dll's through TcosD3D9SetStageKeyKill) */
static HRESULT STDMETHODCALLTYPE dev_SetTextureStageState(Dev *d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage >= STAGES || (UINT)type >= STAGE_STATES || type == 0 || (UINT)type == TCOS_TSS_KEY_KILL) return D3D_OK;
  stage_state_set(d, stage, (UINT)type, value);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetTextureStageState(Dev *d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  *value = stage < STAGES && (UINT)type < STAGE_STATES && (UINT)type != TCOS_TSS_KEY_KILL ? d->state.stage[stage][type] : 0;
  return D3D_OK;
}

/* ---- transforms, the viewport, the material, clip planes ------------------------------------------------------------------------------------------- */

/* The matrix of a transform's number (D3DTS_WORLDMATRIX(n) is 256 + n), or NULL for one Direct3D 9 does not have. */
static D3DMATRIX *transform_in(State *s, D3DTRANSFORMSTATETYPE which) {
  if (which == D3DTS_VIEW) return &s->view;
  if (which == D3DTS_PROJECTION) return &s->projection;
  if (which >= D3DTS_TEXTURE0 && which <= D3DTS_TEXTURE7) return &s->texture[which - D3DTS_TEXTURE0];
  if ((UINT)which >= 256 && (UINT)which < 256 + WORLD_MATRICES) return &s->world[which - 256];
  return NULL;
}
/* a * b, as Direct3D multiplies (a row vector goes through a first). */
static D3DMATRIX matrix_multiply(const D3DMATRIX *a, const D3DMATRIX *b) {
  D3DMATRIX r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a->m[i][k] * b->m[k][j];
      r.m[i][j] = sum;
    }
  return r;
}
static HRESULT STDMETHODCALLTYPE dev_SetTransform(Dev *d, D3DTRANSFORMSTATETYPE which, const D3DMATRIX *matrix) {
  if (!matrix) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  D3DMATRIX *t = transform_in(s, which);
  if (!t) return D3D_OK;      /* (a transform Direct3D 9 does not have: ignored) */
  *t = *matrix;
  if (m) mask_bit(m->transform, (UINT)which);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetTransform(Dev *d, D3DTRANSFORMSTATETYPE which, D3DMATRIX *matrix) {
  if (!matrix) return D3DERR_INVALIDCALL;
  const D3DMATRIX *t = transform_in(&d->state, which);
  *matrix = t ? *t : g_identity;
  return D3D_OK;
}
/* (Direct3D 9 multiplies the device's matrix, recording or not: a state block does not get it) */
static HRESULT STDMETHODCALLTYPE dev_MultiplyTransform(Dev *d, D3DTRANSFORMSTATETYPE which, const D3DMATRIX *matrix) {
  if (!matrix) return D3DERR_INVALIDCALL;
  D3DMATRIX *t = transform_in(&d->state, which);
  if (t) *t = matrix_multiply(matrix, t);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetViewport(Dev *d, const D3DVIEWPORT9 *viewport) {
  if (!viewport) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->viewport = *viewport;
  if (m) m->viewport = TRUE;
  else d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetViewport(Dev *d, D3DVIEWPORT9 *viewport) {
  if (!viewport) return D3DERR_INVALIDCALL;
  *viewport = d->state.viewport;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetScissorRect(Dev *d, const RECT *rect) {
  if (!rect) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->scissor = *rect;
  if (m) m->scissor = TRUE;
  else d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetScissorRect(Dev *d, RECT *rect) {
  if (!rect) return D3DERR_INVALIDCALL;
  *rect = d->state.scissor;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetMaterial(Dev *d, const D3DMATERIAL9 *material) {
  if (!material) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->material = *material;
  if (m) m->material = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetMaterial(Dev *d, D3DMATERIAL9 *material) {
  if (!material) return D3DERR_INVALIDCALL;
  *material = d->state.material;
  return D3D_OK;
}
/* The lights, by their numbers (any number: they are kept in their order); draw.inc lights the vertices of the fixed-function pipeline with those that shine. */
static HRESULT STDMETHODCALLTYPE dev_SetLight(Dev *d, DWORD index, const D3DLIGHT9 *light) {
  if (!light || light->Type < D3DLIGHT_POINT || light->Type > D3DLIGHT_DIRECTIONAL) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  Light *l = light_put(s, index);
  if (!l || (m && !mask_add_light(m, index))) return E_OUTOFMEMORY;
  l->light = *light;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetLight(Dev *d, DWORD index, D3DLIGHT9 *light) {
  const Light *l = light_find(&d->state, index);
  if (!light || !l) return D3DERR_INVALIDCALL;
  *light = l->light;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_LightEnable(Dev *d, DWORD index, BOOL enable) {
  StateMask *m;
  State *s = state_for_set(d, &m);
  Light *l = light_put(s, index);      /* (a light nobody set is a white one that shines along z) */
  if (!l || (m && !mask_add_light(m, index))) return E_OUTOFMEMORY;
  l->enabled = enable ? TRUE : FALSE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetLightEnable(Dev *d, DWORD index, BOOL *enable) {
  const Light *l = light_find(&d->state, index);
  if (!enable || !l) return D3DERR_INVALIDCALL;
  *enable = l->enabled ? 128 : FALSE;      /* (Direct3D 9 answers 128 for a light that shines) */
  return D3D_OK;
}
/* (a plane past the last one there is is the last one, as Windows has it: test_clip_planes_limits) */
static HRESULT STDMETHODCALLTYPE dev_SetClipPlane(Dev *d, DWORD index, const float *plane) {
  if (!plane) return D3DERR_INVALIDCALL;
  if (index >= CLIP_PLANES) index = CLIP_PLANES - 1;
  StateMask *m;
  State *s = state_for_set(d, &m);
  memcpy(s->clip_plane[index], plane, sizeof s->clip_plane[index]);
  if (m) m->clip_planes |= 1u << index;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetClipPlane(Dev *d, DWORD index, float *plane) {
  if (!plane) return D3DERR_INVALIDCALL;
  if (index >= CLIP_PLANES) index = CLIP_PLANES - 1;
  memcpy(plane, d->state.clip_plane[index], sizeof d->state.clip_plane[index]);
  return D3D_OK;
}
/* The clip status: what the program set is what it gets back (the device does not work out the clip codes of its vertices: D3DCAPS9 has no clip status). */
static HRESULT STDMETHODCALLTYPE dev_SetClipStatus(Dev *d, const D3DCLIPSTATUS9 *status) {
  if (!status) return D3DERR_INVALIDCALL;
  d->clip_status = *status;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetClipStatus(Dev *d, D3DCLIPSTATUS9 *status) {
  if (!status) return D3DERR_INVALIDCALL;
  *status = d->clip_status;
  return D3D_OK;
}
/* N-patches: the segments are kept (and recorded); the caps say the device has none, so a draw draws its triangles flat as Direct3D 9 does then. */
static HRESULT STDMETHODCALLTYPE dev_SetNPatchMode(Dev *d, float segments) {
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->npatch = segments;
  if (m) m->npatch = TRUE;
  return D3D_OK;
}
static float STDMETHODCALLTYPE dev_GetNPatchMode(Dev *d) { return d->state.npatch; }
/* Software vertex processing: a device made with D3DCREATE_MIXED_VERTEXPROCESSING switches; the others keep what they were made with. (The vertices are
 * processed by the same code either way here.) */
static HRESULT STDMETHODCALLTYPE dev_SetSoftwareVertexProcessing(Dev *d, BOOL software) {
  if (d->creation.BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) d->software_vp = software ? TRUE : FALSE;
  return D3D_OK;
}
static BOOL STDMETHODCALLTYPE dev_GetSoftwareVertexProcessing(Dev *d) { return d->software_vp; }

/* ---- what is bound ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT decl_make(Dev *d, const D3DVERTEXELEMENT9 *elements, int implicit, Decl **out);
/* The declaration the device has for an FVF code (SetFVF makes it: GetVertexDeclaration gives it, the same one each time). Kept by the device; NULL without memory. */
typedef struct FvfDecl { DWORD fvf; Decl *decl; } FvfDecl;
static Decl *fvf_decl(Dev *d, DWORD fvf);
static HRESULT STDMETHODCALLTYPE dev_SetFVF(Dev *d, DWORD fvf) {
  StateMask *m;
  State *s = state_for_set(d, &m);
  if (fvf) {
    Decl *decl = fvf_decl(d, fvf);
    if (!decl) return E_OUTOFMEMORY;
    bind(&s->decl, decl);
    if (m) m->decl = TRUE;
  }
  s->fvf = fvf;
  if (m) m->fvf = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetFVF(Dev *d, DWORD *fvf) {
  if (!fvf) return D3DERR_INVALIDCALL;
  *fvf = d->state.fvf;
  return D3D_OK;
}
/* (the objects a program sets: this library's own, or what a proxy's wrapper of one holds - tcos_object.h; anything else is no object of the device) */
static Decl *decl_of(IDirect3DVertexDeclaration9 *decl) { return (Decl *)tcos_own(decl, &decl_vtbl); }
static HRESULT STDMETHODCALLTYPE dev_SetVertexDeclaration(Dev *d, IDirect3DVertexDeclaration9 *decl) {
  Decl *x = decl_of(decl);
  if (decl && !x) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  bind(&s->decl, x);
  s->fvf = x ? x->fvf : 0;      /* (GetFVF gives what Direct3D 9 makes of the declaration) */
  if (m) m->decl = m->fvf = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexDeclaration(Dev *d, IDirect3DVertexDeclaration9 **decl) {
  if (!decl) return D3DERR_INVALIDCALL;
  *decl = (IDirect3DVertexDeclaration9 *)d->state.decl;
  if (*decl) child_AddRef(&d->state.decl->c);
  return D3D_OK;
}
static Buffer *vb_of(void *p) { return (Buffer *)tcos_own(p, &vb_vtbl); }
static Buffer *ib_of(void *p) { return (Buffer *)tcos_own(p, &ib_vtbl); }
static HRESULT STDMETHODCALLTYPE dev_SetStreamSource(Dev *d, UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) {
  Buffer *b = vb_of(buffer);
  if (stream >= STREAMS || (buffer && !b) || (offset & 3)) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  bind(&s->stream[stream].buffer, b);
  /* (unbinding a stream keeps its offset and stride, as Direct3D 9 does) */
  if (b) {
    s->stream[stream].offset = offset;
    s->stream[stream].stride = stride;
  }
  if (m) m->streams |= 1u << stream;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetStreamSource(Dev *d, UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) {
  if (stream >= STREAMS || !buffer) return D3DERR_INVALIDCALL;
  *buffer = (IDirect3DVertexBuffer9 *)d->state.stream[stream].buffer;
  if (*buffer) child_AddRef(&d->state.stream[stream].buffer->c);
  if (offset) *offset = d->state.stream[stream].offset;
  if (stride) *stride = d->state.stream[stream].stride;
  return D3D_OK;
}
/* A stream's frequency (instancing, draw.inc): D3DSTREAMSOURCE_INDEXEDDATA | n on stream 0 draws n instances of an indexed draw, the streams with that flag (or none)
 * are per vertex, and those with D3DSTREAMSOURCE_INSTANCEDATA | n give instance i element i / n. A plain divider is kept and given back; vertex processing that is
 * not in software takes no frequency for non-indexed data, so it changes no draw. What Direct3D 9 refuses: 0, both flags, the instance flag on stream 0. */
static HRESULT STDMETHODCALLTYPE dev_SetStreamSourceFreq(Dev *d, UINT stream, UINT divider) {
  if (stream >= STREAMS || divider == 0 || (divider & D3DSTREAMSOURCE_INDEXEDDATA && divider & D3DSTREAMSOURCE_INSTANCEDATA) ||
      (stream == 0 && (divider & D3DSTREAMSOURCE_INSTANCEDATA)))
    return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  s->stream[stream].freq = divider;
  if (m) m->stream_freqs |= 1u << stream;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetStreamSourceFreq(Dev *d, UINT stream, UINT *divider) {
  if (stream >= STREAMS || !divider) return D3DERR_INVALIDCALL;
  *divider = d->state.stream[stream].freq ? d->state.stream[stream].freq : 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetIndices(Dev *d, IDirect3DIndexBuffer9 *buffer) {
  Buffer *b = ib_of(buffer);
  if (buffer && !b) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  bind(&s->indices, b);
  if (m) m->indices = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetIndices(Dev *d, IDirect3DIndexBuffer9 **buffer) {
  if (!buffer) return D3DERR_INVALIDCALL;
  *buffer = (IDirect3DIndexBuffer9 *)d->state.indices;
  if (*buffer) child_AddRef(&d->state.indices->c);
  return D3D_OK;
}
static Tex *texture_of(void *p) {
  static const void *const kinds[] = {&tex_vtbl, &cube_vtbl, &vol_vtbl};
  return (Tex *)tcos_own_any(p, kinds, 3);
}
static HRESULT STDMETHODCALLTYPE dev_SetTexture(Dev *d, DWORD stage, IDirect3DBaseTexture9 *texture) {
  const UINT slot = sampler_slot(stage);
  Tex *t = texture_of(texture);
  if (slot >= SAMPLERS) return D3D_OK;      /* (a stage Direct3D 9 does not have: ignored) */
  if (texture && (!t || t->c.dev != d)) return D3DERR_INVALIDCALL;
  if (t && t->pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  bind(&s->tex[slot], t);
  if (m) m->textures |= 1u << slot;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetTexture(Dev *d, DWORD stage, IDirect3DBaseTexture9 **texture) {
  if (!texture) return D3DERR_INVALIDCALL;
  const UINT slot = sampler_slot(stage);
  Tex *t = slot < SAMPLERS ? d->state.tex[slot] : NULL;
  *texture = (IDirect3DBaseTexture9 *)t;
  if (t) child_AddRef(&t->c);
  return D3D_OK;
}

/* ---- the program's shaders -------------------------------------------------------------------------------------------------------------------------- */

static void shader_destroy(DxObj *o) {
  Shader *s = (Shader *)o;
  mem_free(s->code);
  mem_free(s->dxbc);
}
static HRESULT STDMETHODCALLTYPE shader_GetFunction(Shader *s, void *data, UINT *size) {
  if (!size) return D3DERR_INVALIDCALL;
  if (data) {
    if (*size < s->code_bytes) return D3DERR_INVALIDCALL;
    memcpy(data, s->code, s->code_bytes);
  }
  *size = s->code_bytes;
  return D3D_OK;
}
/* Makes a shader of the tokens at `code`, which end where the shader says they do. The translation is what a draw's pipeline is made of; a shader
 * the translator does not take (sm_to_dxbc.h says which) cannot be made. `implicit`: the device's own (the pixel shaders of the texture stages). */
static HRESULT shader_make_ex(Dev *d, const DWORD *code, BOOL vertex, int implicit, Shader **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (!code) return D3DERR_INVALIDCALL;
  /* (the two are far too big for the stack) */
  SmShader *translated = mem_alloc(sizeof *translated);
  SmContext *context = mem_alloc(sizeof *context);
  Shader *s = NULL;
  HRESULT hr = E_OUTOFMEMORY;
  if (translated && context) {
    const int status = sm_translate((const uint32_t *)code, SM_MAX_TOKENS, translated, context);
    hr = status == SM_OK && (translated->vertex != 0) == (vertex != 0) ? D3D_OK : D3DERR_INVALIDCALL;
    if (status == SM_UNSUPPORTED) OutputDebugStringA(vertex ? "d3d9: a vertex shader uses what this Direct3D 9 has not\n" : "d3d9: a pixel shader uses what this Direct3D 9 has not\n");
  }
  if (SUCCEEDED(hr)) {
    s = child_new_ex(sizeof *s, d, vertex ? (const void *)&vs_vtbl : (const void *)&ps_vtbl, vertex ? g_vs_iids : g_ps_iids, shader_destroy, implicit);
    hr = s ? D3D_OK : E_OUTOFMEMORY;
  }
  if (s) {
    s->magic = SHADER_MAGIC;
    s->vertex = vertex;
    s->code_bytes = (UINT)translated->tokens * 4;
    s->dxbc_bytes = (UINT)translated->dxbc_size;
    s->code = mem_alloc(s->code_bytes);
    s->dxbc = mem_alloc(s->dxbc_bytes);
    if (s->code && s->dxbc) {
      memcpy(s->code, code, s->code_bytes);
      memcpy(s->dxbc, translated->dxbc, s->dxbc_bytes);
      s->floats = translated->float_registers;
      s->bools = translated->bools_used;
      s->registers = translated->constant_registers;
      s->major = translated->major;
      s->state = translated->state_register;
      s->fill = translated->fill_register;
      s->bump = translated->bump_register;
      s->bump_stages = translated->bump_stages;
      for (UINT i = 0; i < 16; ++i) if (translated->sampler_mask >> i & 1) s->sampler_count = i + 1;
      memcpy(s->sampler_kind, translated->sampler_kind, sizeof s->sampler_kind);
      for (UINT i = 0; i < TEXTURE_SETS; ++i) if (translated->varying_mask >> (SM_SLOT_TEXCOORD + i) & 1) s->coordinates = i + 1;
      s->input_count = translated->input_count;
      memcpy(s->inputs, translated->inputs, sizeof s->inputs);
      *out = s;
    } else {
      obj_free(OBJ(s));
      if (!implicit) dx_Release(OBJ(d));
      hr = E_OUTOFMEMORY;
    }
  }
  mem_free(translated);
  mem_free(context);
  return hr;
}
static HRESULT shader_make(Dev *d, const DWORD *code, BOOL vertex, Shader **out) { return shader_make_ex(d, code, vertex, 0, out); }
static HRESULT STDMETHODCALLTYPE dev_CreateVertexShader(Dev *d, const DWORD *code, IDirect3DVertexShader9 **shader) { return shader_make(d, code, TRUE, (Shader **)shader); }
static HRESULT STDMETHODCALLTYPE dev_CreatePixelShader(Dev *d, const DWORD *code, IDirect3DPixelShader9 **shader) { return shader_make(d, code, FALSE, (Shader **)shader); }
/* (a shader is known by its own mark, not its vtable: Direct3D 9 takes one whose vtable a program changed. The mark is read only of an object of this library: one
 * with its vtable, or what IID_ITcosObject gives - the object behind a proxy's wrapper, or one whose vtable was changed; tcos_object.h. Another runtime's object
 * answers no IID_ITcosObject and is not read) */
static Shader *shader_of(const void *p, int vertex) {
  if (!p) return NULL;
  const void *v = ((const DxObj *)p)->lpVtbl;
  const Shader *s = v == (const void *)&vs_vtbl || v == (const void *)&ps_vtbl ? p : (const Shader *)tcos_canonical(p);
  return s && s->magic == SHADER_MAGIC && s->vertex == vertex ? (Shader *)s : NULL;
}
static HRESULT STDMETHODCALLTYPE dev_SetVertexShader(Dev *d, IDirect3DVertexShader9 *given) {
  Shader *shader = shader_of(given, 1);
  if (given && (!shader || shader->c.dev != d)) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  bind(&s->vs, shader);
  if (m) m->vs = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShader(Dev *d, IDirect3DVertexShader9 **shader) {
  if (!shader) return D3DERR_INVALIDCALL;
  *shader = (IDirect3DVertexShader9 *)d->state.vs;
  if (*shader) child_AddRef(&d->state.vs->c);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetPixelShader(Dev *d, IDirect3DPixelShader9 *given) {
  Shader *shader = shader_of(given, 0);
  if (given && (!shader || shader->c.dev != d)) return D3DERR_INVALIDCALL;
  StateMask *m;
  State *s = state_for_set(d, &m);
  bind(&s->ps, shader);
  if (m) m->ps = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetPixelShader(Dev *d, IDirect3DPixelShader9 **shader) {
  if (!shader) return D3DERR_INVALIDCALL;
  *shader = (IDirect3DPixelShader9 *)d->state.ps;
  if (*shader) child_AddRef(&d->state.ps->c);
  return D3D_OK;
}

/* The constants: registers of four floats or four integers, and bools, kept for each stage whatever shader is set. */
/* `count` registers of 16 bytes from `first`, between a program's memory and the `limit` registers at `registers`; `mask`: a bit for each register written. */
static HRESULT registers_copy(void *registers, UINT limit, UINT first, void *to, const void *from, UINT count, DWORD *mask) {
  if ((!to && !from) || first > limit || count > limit - first) return D3DERR_INVALIDCALL;
  if (count && from) memcpy((BYTE *)registers + (SIZE_T)first * 16, from, (SIZE_T)count * 16);
  if (count && to) memcpy(to, (const BYTE *)registers + (SIZE_T)first * 16, (SIZE_T)count * 16);
  if (mask && from) for (UINT i = 0; i < count; ++i) mask_bit(mask, first + i);
  return D3D_OK;
}
static HRESULT bools_set(DWORD *bools, UINT first, const BOOL *data, UINT count, DWORD *mask) {
  if (!data || first > 16 || count > 16 - first) return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < count; ++i) *bools = (*bools & ~(1u << (first + i))) | (data[i] ? 1u << (first + i) : 0u);
  if (mask) for (UINT i = 0; i < count; ++i) *mask |= 1u << (first + i);
  return D3D_OK;
}
static HRESULT bools_get(DWORD bools, UINT first, BOOL *data, UINT count) {
  if (!data || first > 16 || count > 16 - first) return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < count; ++i) data[i] = (bools >> (first + i)) & 1;
  return D3D_OK;
}
#define SET_CONSTANTS(field, limit, mask_field)                                                       \
  StateMask *m;                                                                                       \
  State *s = state_for_set(d, &m);                                                                    \
  return registers_copy(s->field, limit, first, NULL, data, count, m ? (DWORD *)&m->mask_field : NULL)
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantF(Dev *d, UINT first, const float *data, UINT count) { SET_CONSTANTS(vs_float, VS_FLOATS, vs_float); }
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstantF(Dev *d, UINT first, float *data, UINT count) { return registers_copy(d->state.vs_float, VS_FLOATS, first, data, NULL, count, NULL); }
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantI(Dev *d, UINT first, const int *data, UINT count) { SET_CONSTANTS(vs_int, 16, vs_int); }
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstantI(Dev *d, UINT first, int *data, UINT count) { return registers_copy(d->state.vs_int, 16, first, data, NULL, count, NULL); }
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantB(Dev *d, UINT first, const BOOL *data, UINT count) {
  StateMask *m;
  State *s = state_for_set(d, &m);
  return bools_set(&s->vs_bool, first, data, count, m ? &m->vs_bool : NULL);
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstantB(Dev *d, UINT first, BOOL *data, UINT count) { return bools_get(d->state.vs_bool, first, data, count); }
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstantF(Dev *d, UINT first, const float *data, UINT count) { SET_CONSTANTS(ps_float, PS_FLOATS, ps_float); }
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstantF(Dev *d, UINT first, float *data, UINT count) { return registers_copy(d->state.ps_float, PS_FLOATS, first, data, NULL, count, NULL); }
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstantI(Dev *d, UINT first, const int *data, UINT count) { SET_CONSTANTS(ps_int, 16, ps_int); }
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstantI(Dev *d, UINT first, int *data, UINT count) { return registers_copy(d->state.ps_int, 16, first, data, NULL, count, NULL); }
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstantB(Dev *d, UINT first, const BOOL *data, UINT count) {
  StateMask *m;
  State *s = state_for_set(d, &m);
  return bools_set(&s->ps_bool, first, data, count, m ? &m->ps_bool : NULL);
}
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstantB(Dev *d, UINT first, BOOL *data, UINT count) { return bools_get(d->state.ps_bool, first, data, count); }
#undef SET_CONSTANTS

/* ---- state blocks ----------------------------------------------------------------------------------------------------------------------------------- */

static void sb_destroy(DxObj *o) {
  StateBlock *b = (StateBlock *)o;
  state_release(&b->state);
  mem_free(b->mask.light);
}
static StateBlock *sb_make(Dev *d) {
  StateBlock *b = child_new(sizeof *b, d, &sb_vtbl, g_sb_iids, sb_destroy);
  if (!b) return NULL;
  /* (a recorded block starts from the device's state: what it does not record it never applies) */
  memcpy(b->state.rs, d->state.rs, sizeof b->state.rs);
  return b;
}
/* Capture: the parts the block holds, taken from the device again. */
static HRESULT STDMETHODCALLTYPE sb_Capture(StateBlock *b) {
  Dev *d = b->c.dev;
  if (d->recording) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  /* (Direct3D 9 does not take the streams' offsets again: they stay what the block had) */
  UINT offsets[STREAMS];
  for (UINT i = 0; i < STREAMS; ++i) offsets[i] = b->state.stream[i].offset;
  state_transfer(d, &b->state, &d->state, &b->mask, FALSE);
  for (UINT i = 0; i < STREAMS; ++i) b->state.stream[i].offset = offsets[i];
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
/* Apply: the parts the block holds, set on the device (not while a block is being recorded: Direct3D 9 refuses it then). */
static HRESULT STDMETHODCALLTYPE sb_Apply(StateBlock *b) {
  Dev *d = b->c.dev;
  if (d->recording) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  state_transfer(d, &d->state, &b->state, &b->mask, TRUE);
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateStateBlock(Dev *d, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE) return D3DERR_INVALIDCALL;
  if (d->recording) return D3DERR_INVALIDCALL;
  StateBlock *b = sb_make(d);
  if (!b) return E_OUTOFMEMORY;
  if (type == D3DSBT_ALL) mask_all(&b->mask);
  else if (type == D3DSBT_VERTEXSTATE) mask_vertex(&b->mask);
  else mask_pixel(&b->mask);
  EnterCriticalSection(&d->lock);
  state_transfer(d, &b->state, &d->state, &b->mask, FALSE);
  LeaveCriticalSection(&d->lock);
  *out = (IDirect3DStateBlock9 *)b;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_BeginStateBlock(Dev *d) {
  if (d->recording) return D3DERR_INVALIDCALL;
  StateBlock *b = sb_make(d);
  if (!b) return E_OUTOFMEMORY;
  /* (the block being recorded holds no reference of the device yet, as in Direct3D 9: EndStateBlock gives it its first) */
  dx_Release(OBJ(d));
  b->c.o.ref = 0;
  b->c.kept = 1;
  /* (the block starts as the device is, so that what is recorded on top - a part of the constants, the lights - has the rest of its part) */
  StateMask all;
  memset(&all, 0, sizeof all);
  mask_all(&all);
  EnterCriticalSection(&d->lock);
  state_transfer(d, &b->state, &d->state, &all, FALSE);
  LeaveCriticalSection(&d->lock);
  d->recording = b;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_EndStateBlock(Dev *d, IDirect3DStateBlock9 **out) {
  if (!out || !d->recording) return D3DERR_INVALIDCALL;
  d->recording->c.kept = 0;
  child_AddRef(&d->recording->c);
  *out = (IDirect3DStateBlock9 *)d->recording;
  d->recording = NULL;
  return D3D_OK;
}

/* ---- palettes --------------------------------------------------------------------------------------------------------------------------------------- */

/* The device's palettes, by number (D3DCAPS9 says up to 16384... Direct3D 9 takes numbers below 65536), 256 entries each, made as they are set. */
#define PALETTES 65536u
static const PALETTEENTRY *current_palette(Dev *d) {
  return d->current_palette < d->palette_count && d->palette_set[d->current_palette] ? d->palettes[d->current_palette] : NULL;
}
static HRESULT STDMETHODCALLTYPE dev_SetPaletteEntries(Dev *d, UINT number, const PALETTEENTRY *entries) {
  if (!entries || number >= PALETTES) return D3DERR_INVALIDCALL;
  /* (D3DPTEXTURECAPS_ALPHAPALETTE: the entries' flags are the alpha; a device without it would need them all 0xFF) */
  EnterCriticalSection(&d->lock);
  if (number >= d->palette_count) {
    UINT count = d->palette_count ? d->palette_count : 16;
    while (count <= number) count *= 2;
    if (count > PALETTES) count = PALETTES;
    PALETTEENTRY(*more)[256] = mem_alloc(sizeof(PALETTEENTRY[256]) * count);
    BYTE *set = mem_alloc(count);
    if (!more || !set) { mem_free(more); mem_free(set); LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
    if (d->palette_count) { memcpy(more, d->palettes, sizeof(PALETTEENTRY[256]) * d->palette_count); memcpy(set, d->palette_set, d->palette_count); }
    mem_free(d->palettes);
    mem_free(d->palette_set);
    d->palettes = more;
    d->palette_set = set;
    d->palette_count = count;
  }
  memcpy(d->palettes[number], entries, sizeof(PALETTEENTRY[256]));
  d->palette_set[number] = 1;
  if (number == d->current_palette) ++d->palette_version;
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetPaletteEntries(Dev *d, UINT number, PALETTEENTRY *entries) {
  if (!entries || number >= d->palette_count || !d->palette_set[number]) return D3DERR_INVALIDCALL;
  memcpy(entries, d->palettes[number], sizeof(PALETTEENTRY[256]));
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetCurrentTexturePalette(Dev *d, UINT number) {
  if (number >= d->palette_count || !d->palette_set[number]) return D3DERR_INVALIDCALL;
  if (number != d->current_palette) {
    d->current_palette = number;
    ++d->palette_version;
  }
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetCurrentTexturePalette(Dev *d, UINT *number) {
  if (!number) return D3DERR_INVALIDCALL;
  *number = d->current_palette;
  return D3D_OK;
}

/* ---- the declarations of FVF codes ------------------------------------------------------------------------------------------------------------------ */

#define FVF_DECLS 64u
static Decl *fvf_decl(Dev *d, DWORD fvf) {
  for (UINT i = 0; i < d->fvf_decl_count; ++i) if (d->fvf_decls[i].fvf == fvf) return d->fvf_decls[i].decl;
  D3DVERTEXELEMENT9 elements[17];
  fvf_elements(fvf, elements);
  Decl *decl = NULL;
  if (FAILED(decl_make(d, elements, 1, &decl))) return NULL;
  decl->fvf = fvf;
  if (d->fvf_decl_count == d->fvf_decl_capacity) {
    const UINT capacity = d->fvf_decl_capacity ? d->fvf_decl_capacity * 2 : 16;
    FvfDecl *more = mem_alloc(sizeof(FvfDecl) * capacity);
    if (!more) { obj_free(OBJ(decl)); return NULL; }
    if (d->fvf_decl_count) memcpy(more, d->fvf_decls, sizeof(FvfDecl) * d->fvf_decl_count);
    mem_free(d->fvf_decls);
    d->fvf_decls = more;
    d->fvf_decl_capacity = capacity;
  }
  d->fvf_decls[d->fvf_decl_count].fvf = fvf;
  d->fvf_decls[d->fvf_decl_count].decl = decl;
  ++d->fvf_decl_count;
  return decl;
}
