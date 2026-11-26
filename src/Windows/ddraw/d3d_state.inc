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
/* The states of a Direct3D device (d3d.inc): what the program set is kept here (Get* answer it), the parts Direct3D 9 has not got yet are marked
 * dirty and go to it before the next draw (d3_flush), state blocks are copies of parts of it. */

#define MASK_SET(bits, i) ((bits)[(i) >> 5] |= 1u << ((i) & 31u))
#define MASK_HAS(bits, i) (((bits)[(i) >> 5] >> ((i) & 31u)) & 1u)

static BOOL mask_lights(D3dMask *m, UINT count) {
  if (m->light_count >= count) return TRUE;
  BYTE *grown = mem_grow(m->lights, count);
  if (!grown) return FALSE;
  m->lights = grown;
  m->light_count = count;
  return TRUE;
}
static BOOL state_lights(D3dState *s, UINT count) {
  if (s->light_count >= count) return TRUE;
  D3dLight7 *grown = mem_grow(s->lights, (SIZE_T)count * sizeof *grown);
  if (!grown) return FALSE;
  s->lights = grown;
  s->light_count = count;
  return TRUE;
}
static void mask_clear(D3dMask *m) {
  BYTE *lights = m->lights;
  const UINT n = m->light_count;
  zero(m, sizeof *m);
  if (lights) zero(lights, n);
  m->lights = lights;
  m->light_count = n;
}
static void mask_all(D3dMask *m, UINT lights) {
  for (UINT i = 0; i < 8; ++i) m->rs[i] = 0xFFFFFFFFu;
  for (UINT i = 0; i < D3D_STAGES; ++i) m->tss[i] = 0xFFFFFFFFu;
  m->texture = 0xFFu;
  m->xf = 0xFFFFFFFFu;
  m->viewport = m->material = TRUE;
  m->planes = 0xFFFFFFFFu;
  if (mask_lights(m, lights)) for (UINT i = 0; i < lights; ++i) m->lights[i] = 3;
}

static void d3_state_defaults(D3dDevice *dev) {
  D3dState *s = &dev->st;
  const BOOL depth = FALSE;
  for (UINT i = 1; i < 256; ++i) dl_render_state_default(dev->version, i, depth, (uint32_t *)&s->rs[i]);
  for (UINT st = 0; st < D3D_STAGES; ++st)
    for (UINT i = 0; i < D3D_TSS; ++i) s->tss[st][i] = dl_texture_stage_default(st, i);
  for (UINT i = 0; i < D3D_XF; ++i) dl_matrix_identity(&s->xf[i]);
  s->viewport.dwX = s->viewport.dwY = 0;
  s->viewport.dwWidth = dev->target ? dev->target->desc.dwWidth : 0;
  s->viewport.dwHeight = dev->target ? dev->target->desc.dwHeight : 0;
  s->viewport.dvMinZ = 0.0f;
  s->viewport.dvMaxZ = 1.0f;
  for (UINT i = 1; i <= 8; ++i) dl_light_state_default(i, (uint32_t *)&dev->light_state[i]);
  dev->texture_map_blend = dev->version < 7 ? D3DTBLEND_MODULATE : 0;
  dev->legacy_blend = dev->version < 7;
  dl_matrix_identity(&dev->clip);
  dev->clip_status.dwFlags = D3DCLIPSTATUS_STATUS;
  dev->clip_status.dwStatus = D3DSTATUS_DEFAULT;
  dev->clip_status.minx = dev->clip_status.miny = dev->clip_status.minz = 0.0f;
  dev->clip_status.maxx = dev->clip_status.maxy = dev->clip_status.maxz = 0.0f;
  dev->all_dirty = TRUE;
}

/* ---- setting ---------------------------------------------------------------------------------------------------------------------------------------
 * Each setter takes a value that has been checked; while a state block is being recorded it goes into the block instead (as Direct3D 7). */

static D3dState *set_target(D3dDevice *dev, D3dMask **mask) {
  if (dev->recording) { *mask = &dev->recording->mask; return &dev->recording->state; }
  *mask = &dev->dirty;
  return &dev->st;
}

/* (the sampler states of stage 0 that Direct3D 1-7 render states name: kept as stage 0's texture stage states, which share them - Wine) */
static BOOL rs_is_sampler(DWORD state) {
  switch (state) {
  case D3DRENDERSTATE_TEXTUREADDRESS: case D3DRENDERSTATE_TEXTUREADDRESSU: case D3DRENDERSTATE_TEXTUREADDRESSV: case D3DRENDERSTATE_BORDERCOLOR:
  case D3DRENDERSTATE_TEXTUREMAG: case D3DRENDERSTATE_TEXTUREMIN: case D3DRENDERSTATE_MIPMAPLODBIAS: case D3DRENDERSTATE_ANISOTROPY:
    return TRUE;
  default: return FALSE;
  }
}
static void set_tss_value(D3dState *s, D3dMask *m, UINT stage, UINT state, DWORD value) {
  s->tss[stage][state] = value;
  m->tss[stage] |= 1u << state;
}
static void d3_set_rs(D3dDevice *dev, DWORD state, DWORD value) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  s->rs[state] = value;
  MASK_SET(m->rs, state);
  if (!rs_is_sampler(state)) return;
  switch (state) {
  case D3DRENDERSTATE_TEXTUREADDRESS:
    set_tss_value(s, m, 0, D3DTSS_ADDRESS, value);
    set_tss_value(s, m, 0, D3DTSS_ADDRESSU, value);
    set_tss_value(s, m, 0, D3DTSS_ADDRESSV, value);
    break;
  case D3DRENDERSTATE_TEXTUREADDRESSU: set_tss_value(s, m, 0, D3DTSS_ADDRESSU, value); break;
  case D3DRENDERSTATE_TEXTUREADDRESSV: set_tss_value(s, m, 0, D3DTSS_ADDRESSV, value); break;
  case D3DRENDERSTATE_BORDERCOLOR: set_tss_value(s, m, 0, D3DTSS_BORDERCOLOR, value); break;
  case D3DRENDERSTATE_TEXTUREMAG: set_tss_value(s, m, 0, D3DTSS_MAGFILTER, dl_tfg_from_d3d9(dl_mag_filter(value))); break;
  case D3DRENDERSTATE_TEXTUREMIN: {
    uint32_t min, mip;
    dl_min_filter(value, &min, &mip);
    set_tss_value(s, m, 0, D3DTSS_MINFILTER, min == DL_TEXF9_LINEAR ? D3DTFN_LINEAR : D3DTFN_POINT);
    set_tss_value(s, m, 0, D3DTSS_MIPFILTER, dl_tfp_from_d3d9(mip));
    break;
  }
  case D3DRENDERSTATE_MIPMAPLODBIAS: set_tss_value(s, m, 0, D3DTSS_MIPMAPLODBIAS, value); break;
  case D3DRENDERSTATE_ANISOTROPY: set_tss_value(s, m, 0, D3DTSS_MAXANISOTROPY, value); break;
  }
}
/* What GetRenderState reports of a sampler state (from stage 0's). */
static DWORD rs_sampler_value(const D3dDevice *dev, DWORD state) {
  const DWORD *t = dev->st.tss[0];
  switch (state) {
  case D3DRENDERSTATE_TEXTUREADDRESS: case D3DRENDERSTATE_TEXTUREADDRESSU: return t[D3DTSS_ADDRESSU];
  case D3DRENDERSTATE_TEXTUREADDRESSV: return t[D3DTSS_ADDRESSV];
  case D3DRENDERSTATE_BORDERCOLOR: return t[D3DTSS_BORDERCOLOR];
  case D3DRENDERSTATE_TEXTUREMAG: return dl_legacy_mag_filter(dl_tfg_to_d3d9(t[D3DTSS_MAGFILTER]));
  case D3DRENDERSTATE_TEXTUREMIN: return dl_legacy_min_filter(t[D3DTSS_MINFILTER] == D3DTFN_LINEAR ? DL_TEXF9_LINEAR : DL_TEXF9_POINT, dl_tfp_to_d3d9(t[D3DTSS_MIPFILTER]));
  case D3DRENDERSTATE_MIPMAPLODBIAS: return t[D3DTSS_MIPMAPLODBIAS];
  case D3DRENDERSTATE_ANISOTROPY: return t[D3DTSS_MAXANISOTROPY];
  default: return dev->st.rs[state];
  }
}

static void d3_set_tss(D3dDevice *dev, UINT stage, UINT state, DWORD value) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  set_tss_value(s, m, stage, state, value);
  if (state == D3DTSS_ADDRESS) {
    set_tss_value(s, m, stage, D3DTSS_ADDRESSU, value);
    set_tss_value(s, m, stage, D3DTSS_ADDRESSV, value);
  }
}
static void d3_set_texture(D3dDevice *dev, UINT stage, Surface *t) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  s->texture[stage] = t;
  m->texture |= 1u << stage;
}
static void d3_set_xf(D3dDevice *dev, UINT index, const DlMatrix *matrix) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  s->xf[index] = *matrix;
  m->xf |= 1u << index;
}
static void d3_set_viewport(D3dDevice *dev, const D3DVIEWPORT7 *vp) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  s->viewport = *vp;
  m->viewport = TRUE;
}
static void d3_set_material(D3dDevice *dev, const D3DMATERIAL7 *mat) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  s->material = *mat;
  m->material = TRUE;
}
#define D3D_MAX_LIGHT_INDEX 0x10000u
/* SetLight / LightEnable (the light gets Direct3D's default light first if it had none: LightEnable of a light never set). */
static HRESULT d3_light(D3dDevice *dev, DWORD index, const D3DLIGHT7 *light, int enable) {
  if (index >= D3D_MAX_LIGHT_INDEX) return DDERR_INVALIDPARAMS;
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  if (!state_lights(s, index + 1) || !mask_lights(m, index + 1)) return DDERR_OUTOFMEMORY;
  D3dLight7 *l = &s->lights[index];
  if (!l->set && !light) {
    /* (the default light: directional, white, pointing along +z - and from the device's own state when recording, so it is the same light) */
    if (dev->recording && index < dev->st.light_count && dev->st.lights[index].set) *l = dev->st.lights[index];
    else {
      zero(&l->light, sizeof l->light);
      l->light.dltType = D3DLIGHT_DIRECTIONAL;
      l->light.dcvDiffuse.r = l->light.dcvDiffuse.g = l->light.dcvDiffuse.b = 1.0f;
      l->light.dvDirection.z = 1.0f;
    }
    l->set = TRUE;
    m->lights[index] |= 1;
  }
  if (light) { l->light = *light; l->set = TRUE; m->lights[index] |= 1; }
  if (enable >= 0) { l->enabled = enable != 0; m->lights[index] |= 2; }
  return D3D_OK;
}
static void d3_set_plane(D3dDevice *dev, UINT index, const float plane[4]) {
  D3dMask *m;
  D3dState *s = set_target(dev, &m);
  for (UINT i = 0; i < 4; ++i) s->plane[index][i] = plane[i];
  m->planes |= 1u << index;
}

/* ---- state blocks ----------------------------------------------------------------------------------------------------------------------------------- */

/* The render states and texture stage states of Direct3D 7's D3DSBT_PIXELSTATE / D3DSBT_VERTEXSTATE (Direct3D 9's lists, with Direct3D 7's own states
 * where they belong). */
static const BYTE kPixelRs[] = {2, 4, 7, 8, 9, 10, 11, 12, 14, 15, 16, 19, 20, 23, 24, 25, 26, 27, 30, 31, 32, 33, 34, 36, 37, 38, 39, 40, 41, 46, 47, 52, 53, 54,
                                55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135};
static const BYTE kVertexRs[] = {22, 28, 34, 35, 36, 37, 38, 48, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152};
static void block_mask_type(D3dMask *m, DWORD type, const D3dDevice *dev) {
  mask_clear(m);
  if (type == D3DSBT_ALL) { mask_all(m, dev->st.light_count); return; }
  if (type == D3DSBT_PIXELSTATE) {
    for (UINT i = 0; i < sizeof kPixelRs; ++i) MASK_SET(m->rs, kPixelRs[i]);
    for (UINT i = D3DRENDERSTATE_STIPPLEPATTERN00; i < D3DRENDERSTATE_STIPPLEPATTERN00 + 32; ++i) MASK_SET(m->rs, i);
    for (UINT st = 0; st < D3D_STAGES; ++st) m->tss[st] = 0xFFFFFFFFu & ~((1u << D3DTSS_TEXCOORDINDEX) | (1u << D3DTSS_TEXTURETRANSFORMFLAGS));
    return;
  }
  for (UINT i = 0; i < sizeof kVertexRs; ++i) MASK_SET(m->rs, kVertexRs[i]);
  for (UINT st = 0; st < D3D_STAGES; ++st) m->tss[st] = (1u << D3DTSS_TEXCOORDINDEX) | (1u << D3DTSS_TEXTURETRANSFORMFLAGS);
  if (mask_lights(m, dev->st.light_count))
    for (UINT i = 0; i < dev->st.light_count; ++i) if (dev->st.lights[i].set) m->lights[i] = 3;
}
static void block_free(D3dStateBlock *b) {
  if (!b) return;
  mem_free(b->state.lights);
  mem_free(b->mask.lights);
  mem_free(b);
}
/* The device's current values of what the block holds into it (CaptureStateBlock, CreateStateBlock). */
static void block_capture(D3dStateBlock *b, const D3dDevice *dev) {
  const D3dState *s = &dev->st;
  for (UINT i = 1; i < 256; ++i) if (MASK_HAS(b->mask.rs, i)) b->state.rs[i] = s->rs[i];
  for (UINT st = 0; st < D3D_STAGES; ++st)
    for (UINT i = 0; i < D3D_TSS; ++i) if ((b->mask.tss[st] >> i) & 1u) b->state.tss[st][i] = s->tss[st][i];
  for (UINT st = 0; st < D3D_STAGES; ++st) if ((b->mask.texture >> st) & 1u) b->state.texture[st] = s->texture[st];
  for (UINT i = 0; i < D3D_XF; ++i) if ((b->mask.xf >> i) & 1u) b->state.xf[i] = s->xf[i];
  if (b->mask.viewport) b->state.viewport = s->viewport;
  if (b->mask.material) b->state.material = s->material;
  for (UINT i = 0; i < D3D_PLANES; ++i) if ((b->mask.planes >> i) & 1u) for (UINT k = 0; k < 4; ++k) b->state.plane[i][k] = s->plane[i][k];
  if (state_lights(&b->state, b->mask.light_count))
    for (UINT i = 0; i < b->mask.light_count; ++i) {
      if (!b->mask.lights[i]) continue;
      if (i < s->light_count) b->state.lights[i] = s->lights[i];
      else zero(&b->state.lights[i], sizeof b->state.lights[i]);
    }
}
/* ApplyStateBlock: what the block holds becomes the device's. */
static void block_apply(const D3dStateBlock *b, D3dDevice *dev) {
  const D3dState *s = &b->state;
  for (UINT i = 1; i < 256; ++i) if (MASK_HAS(b->mask.rs, i) && !rs_is_sampler(i)) d3_set_rs(dev, i, s->rs[i]);
  for (UINT st = 0; st < D3D_STAGES; ++st)
    for (UINT i = 0; i < D3D_TSS; ++i) if ((b->mask.tss[st] >> i) & 1u) d3_set_tss(dev, st, i, s->tss[st][i]);
  for (UINT st = 0; st < D3D_STAGES; ++st) if ((b->mask.texture >> st) & 1u) d3_set_texture(dev, st, s->texture[st]);
  for (UINT i = 0; i < D3D_XF; ++i) if ((b->mask.xf >> i) & 1u) d3_set_xf(dev, i, &s->xf[i]);
  if (b->mask.viewport) d3_set_viewport(dev, &s->viewport);
  if (b->mask.material) d3_set_material(dev, &s->material);
  for (UINT i = 0; i < D3D_PLANES; ++i) if ((b->mask.planes >> i) & 1u) d3_set_plane(dev, i, s->plane[i]);
  for (UINT i = 0; i < b->mask.light_count && i < s->light_count; ++i) {
    if (!b->mask.lights[i] || !s->lights[i].set) continue;
    d3_light(dev, i, (b->mask.lights[i] & 1) ? &s->lights[i].light : NULL, (b->mask.lights[i] & 2) ? (int)s->lights[i].enabled : -1);
  }
}
/* A recorded block: what was set while recording (the mask) - BeginStateBlock starts it empty. */
static D3dStateBlock *block_new(void) { return mem_alloc(sizeof(D3dStateBlock)); }

/* A surface that goes: no state refers to it any more. */
static void d3_device_surface_gone(D3dDevice *dev, Surface *s) {
  for (UINT st = 0; st < D3D_STAGES; ++st) {
    if (dev->st.texture[st] == s) { dev->st.texture[st] = NULL; dev->dirty.texture |= 1u << st; }
    if (dev->recording && dev->recording->state.texture[st] == s) dev->recording->state.texture[st] = NULL;
    if (dev->bound_res[st] && dev->bound_res[st] == s->store.shadow.resource) {
      if (dev->dd3 && dev->dd3->dev && dev->dd3->active == dev) D3D_CALL(dev->dd3->dev, SetTexture, st, NULL);
      dev->bound_res[st] = NULL;
    }
  }
  for (UINT i = 0; i < dev->blocks.count; ++i) {
    D3dStateBlock *b = handle_get(&dev->blocks, i + 1, H_STATEBLOCK);
    if (b) for (UINT st = 0; st < D3D_STAGES; ++st) if (b->state.texture[st] == s) b->state.texture[st] = NULL;
  }
  if (dev->target == s) dev->target = NULL;
}

/* ---- to Direct3D 9 ---------------------------------------------------------------------------------------------------------------------------------- */

static void ops_apply(D3dDevice *dev, const DlStateOps *o) {
  IDirect3DDevice9 *d9 = dev->dd3->dev;
  for (unsigned i = 0; i < o->count; ++i) {
    const DlStateOp *op = &o->op[i];
    switch (op->target) {
    case DL_SET_RENDER: D3D_CALL(d9, SetRenderState, op->state, op->value); break;
    case DL_SET_STAGE: D3D_CALL(d9, SetTextureStageState, op->index, op->state, op->value); break;
    case DL_SET_SAMPLER: D3D_CALL(d9, SetSamplerState, op->index, (D3DSAMPLERSTATETYPE)op->state, op->value); break;
    default: break;      /* (textures and materials are the device's: d3_flush) */
    }
  }
}

/* The depth buffer attached to a surface (NULL: none). */
static Surface *attached_depth(Surface *target) {
  if (!target) return NULL;
  for (UINT i = 0; i < MAX_COMPLEX && target->complex[i]; ++i)
    if (target->complex[i]->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return target->complex[i];
  for (Surface *c = target->next_attached; c; c = c->next_attached)
    if (c->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return c;
  return NULL;
}
static UINT depth_bits(const Surface *z) {
  if (!z) return 24;
  const UINT bits = (UINT)dd_mask_max(dd_format(z->fmt)->r);
  UINT n = 0;
  for (UINT v = bits; v; v >>= 1) ++n;
  return n ? n : 24;
}

/* Binds the device's target and depth buffer (their copies made and up to date). */
static HRESULT bind_targets(D3dDevice *dev) {
  D3dDD *d = dev->dd3;
  if (!dev->target) return DDERR_INVALIDPARAMS;
  D3dRes *t = target_res(d, dev->target);
  if (!t) return DDERR_OUTOFMEMORY;
  Surface *z = attached_depth(dev->target);
  D3dRes *zr = z ? depth_res(d, z) : NULL;
  if (t->surface != d->bound_target) {
    if (FAILED(D3D_CALL(d->dev, SetRenderTarget, 0, t->surface))) return DDERR_INVALIDPARAMS;
    d->bound_target = t->surface;
    dev->dirty.viewport = TRUE;
  }
  IDirect3DSurface9 *zs = zr ? zr->surface : NULL;
  if (zs != d->bound_depth) {
    D3D_CALL(d->dev, SetDepthStencilSurface, zs);
    d->bound_depth = zs;
  }
  return D3D_OK;
}

/* The CPU lighting of a device (Direct3D 2 and 3: the active lights of its current viewport and the material of its MATERIAL light state; Direct3D 7:
 * its lights, material and render states - for ProcessVertices). */
static void d3_lighting(D3dDevice *dev, DlLighting *l) {
  dl_lighting_init(l, dev->version);
  if (dev->version < 7) {
    for (UINT i = 1; i <= 8; ++i) dl_lighting_apply_light_state(l, i, dev->light_state[i]);
    D3dMaterial *mat = handle_get(&g_handles, dev->light_state[D3DLIGHTSTATE_MATERIAL], H_MATERIAL);
    l->enabled = mat != NULL;
    if (mat) dl_material7_from_legacy(&mat->material, &l->material);
    l->specular = dev->st.rs[D3DRENDERSTATE_SPECULARENABLE] != 0;
    if (dev->current_viewport)
      for (D3dLight *li = dev->current_viewport->lights; li && l->lights < DL_LIGHTS; li = li->next_in_viewport) {
        int active = 0;
        if (!li->set) continue;
        if (dl_light_from_legacy(&li->light, &l->light[l->lights], &active) != DL_OK || !active) continue;
        /* (a light of no colour adds nothing: it does not take one of the DL_LIGHTS places - Wine's test_specular_lighting puts 64 black lights
         * before the one it tests) */
        const DlLightEntry *e = &l->light[l->lights];
        BOOL dark = TRUE;
        for (int k = 0; k < 3 && dark; ++k) dark = e->diffuse[k] == 0.0f && e->specular[k] == 0.0f && e->ambient[k] == 0.0f;
        if (!dark) ++l->lights;
      }
    return;
  }
  const DWORD *rs = dev->st.rs;
  l->enabled = rs[D3DRENDERSTATE_LIGHTING] != 0;
  memcpy(&l->material, &dev->st.material, sizeof l->material);
  {
    float c[4];
    dl_color_to_float(rs[D3DRENDERSTATE_AMBIENT], c);
    l->ambient[0] = c[0]; l->ambient[1] = c[1]; l->ambient[2] = c[2];
  }
  l->specular = rs[D3DRENDERSTATE_SPECULARENABLE] != 0;
  l->local_viewer = rs[D3DRENDERSTATE_LOCALVIEWER] != 0;
  l->normalize = rs[D3DRENDERSTATE_NORMALIZENORMALS] != 0;
  l->color_vertex = rs[D3DRENDERSTATE_COLORVERTEX] != 0;
  l->diffuse_source = (int)rs[D3DRENDERSTATE_DIFFUSEMATERIALSOURCE];
  l->ambient_source = (int)rs[D3DRENDERSTATE_AMBIENTMATERIALSOURCE];
  l->specular_source = (int)rs[D3DRENDERSTATE_SPECULARMATERIALSOURCE];
  l->emissive_source = (int)rs[D3DRENDERSTATE_EMISSIVEMATERIALSOURCE];
  l->fog_mode = rs[D3DRENDERSTATE_FOGENABLE] ? (int)rs[D3DRENDERSTATE_FOGVERTEXMODE] : 0;
  l->fog_start = dl_float_of(rs[D3DRENDERSTATE_FOGSTART]);
  l->fog_end = dl_float_of(rs[D3DRENDERSTATE_FOGEND]);
  l->fog_density = dl_float_of(rs[D3DRENDERSTATE_FOGDENSITY]);
  l->range_fog = rs[D3DRENDERSTATE_RANGEFOGENABLE] != 0;
  for (UINT i = 0; i < dev->st.light_count && l->lights < 8; ++i) {
    const D3dLight7 *li = &dev->st.lights[i];
    if (li->set && li->enabled && dl_light_from_light7((const DlLight7 *)&li->light, &l->light[l->lights]) == DL_OK) ++l->lights;
  }
}
/* The transform of a device for the CPU: Direct3D 1-3 with the current viewport's clip space; Direct3D 7 with its own projection and user planes. */
static void d3_transform(D3dDevice *dev, DlTransform *t) {
  dl_transform_init(t);
  t->world = dev->st.xf[D3DTRANSFORMSTATE_WORLD];
  t->view = dev->st.xf[D3DTRANSFORMSTATE_VIEW];
  t->projection = dev->st.xf[D3DTRANSFORMSTATE_PROJECTION];
  if (dev->version < 7) t->clip = dev->clip;
  memcpy(&t->viewport, &dev->st.viewport, sizeof t->viewport);
  /* (an empty or inverted depth range is 0.001 deep, as Windows - Wine's test_process_vertices) */
  if (!(t->viewport.dvMaxZ > t->viewport.dvMinZ)) t->viewport.dvMaxZ = t->viewport.dvMinZ + 0.001f;
  if (dev->version >= 7) {
    t->user_planes = dev->st.rs[D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3Fu;
    for (UINT i = 0; i < 6; ++i) for (UINT k = 0; k < 4; ++k) t->user_plane[i][k] = dev->st.plane[i][k];
  }
  dl_transform_prepare(t);
}

/* The texture kind stage 0 has for TEXTUREMAPBLEND: 0 none, 1 without alpha, 2 with alpha. */
static int stage0_kind(const D3dDevice *dev) {
  const D3dRes *r = dev->bound_res[0];
  if (!dev->st.texture[0] || !r) return 0;
  return r->has_alpha ? 2 : 1;
}

/* Binds the textures (each up to date) and the colour keying they need. */
static void bind_textures(D3dDevice *dev) {
  D3dDD *d = dev->dd3;
  const BOOL keying = dev->st.rs[D3DRENDERSTATE_COLORKEYENABLE] != 0;
  UINT kill = 0;
  const int before = stage0_kind(dev);
  for (UINT st = 0; st < D3D_STAGES; ++st) {
    Surface *s = dev->st.texture[st];
    D3dRes *r = s ? texture_res(d, texture_root(s), keying) : NULL;
    IDirect3DBaseTexture9 *bt = r ? (r->cube ? (IDirect3DBaseTexture9 *)r->cube : (IDirect3DBaseTexture9 *)r->texture) : NULL;
    if (r && r->keyed) kill |= 1u << st;
    if (r != dev->bound_res[st] || dev->bound_stamp[st] != d->tex_stamp || (dev->dirty.texture >> st) & 1u) {
      D3D_CALL(d->dev, SetTexture, st, bt);
      dev->bound_res[st] = r;
      dev->bound_stamp[st] = d->tex_stamp;
    }
  }
  dev->dirty.texture = 0;
  /* (keyed texels are alpha 0 and no other texel of a keyed texture is: the pixel stage takes them away by the texel itself, before the stages and
   * the alpha test - Windows discards them whatever alpha the stages make, Wine's test_colorkey_precision and test_ck_rgba) */
  for (UINT st = 0; st < D3D_STAGES; ++st) {
    const UINT bit = 1u << st;
    if (dev->bound_key_kill != ~0u && (dev->bound_key_kill & bit) == (kill & bit)) continue;
    D3D_CALL(d->dev, SetTextureStageState, st, D9_TSS_KEY_KILL, (kill & bit) ? TRUE : FALSE);
  }
  dev->bound_key_kill = kill;
  /* (MODULATE takes the texture's alpha only if it has one: stage 0 again when that changed) */
  if (dev->legacy_blend && stage0_kind(dev) != before) MASK_SET(dev->dirty.rs, D3DRENDERSTATE_TEXTUREMAPBLEND);
}

/* Puts the device's dirty states (all of them, if Direct3D 9 had another device's) into Direct3D 9, binds its targets and textures. */
static HRESULT d3_flush(D3dDevice *dev) {
  D3dDD *d = dev->dd3;
  if (!d || !d->dev) return DDERR_GENERIC;
  if (d->active != dev) {
    if (d->active) d->active->all_dirty = TRUE;
    d->active = dev;
    dev->all_dirty = TRUE;
  }
  if (dev->all_dirty) {
    mask_all(&dev->dirty, dev->st.light_count);
    for (UINT st = 0; st < D3D_STAGES; ++st) dev->bound_res[st] = NULL;
    dev->bound_key_kill = ~0u;
    d->bound_target = d->bound_depth = NULL;
    dev->all_dirty = FALSE;
    /* (Direct3D 1-3 draw transformed vertices: no Direct3D 9 lighting) */
    if (dev->version < 7) D3D_CALL(d->dev, SetRenderState, D9_RS_LIGHTING, FALSE);
  }
  HRESULT hr = bind_targets(dev);
  if (FAILED(hr)) return hr;
  IDirect3DDevice9 *d9 = d->dev;
  DlStateContext ctx;
  ctx.wrap0 = dev->st.rs[D3DRENDERSTATE_WRAP0];
  ctx.texture_map_blend = 0;
  ctx.texture = 0;
  ctx.depth_bits = depth_bits(attached_depth(dev->target));
  /* render states */
  for (UINT w = 0; w < 8; ++w) {
    DWORD bits = dev->dirty.rs[w];
    if (!bits) continue;
    dev->dirty.rs[w] = 0;
    for (UINT b = 0; b < 32; ++b) {
      if (!((bits >> b) & 1u)) continue;
      const UINT state = w * 32 + b;
      if (!state || rs_is_sampler(state) || state == D3DRENDERSTATE_TEXTUREMAPBLEND || state == D3DRENDERSTATE_TEXTUREHANDLE) continue;
      if (dev->version >= 7 && (state == D3DRENDERSTATE_LIGHTING)) { D3D_CALL(d9, SetRenderState, D9_RS_LIGHTING, dev->st.rs[state]); continue; }
      if (dev->version < 7 && (state == D3DRENDERSTATE_FOGVERTEXMODE || state == D3DRENDERSTATE_LIGHTING)) continue;
      DlStateOps ops;
      if (dl_render_state(dev->version < 7 ? 3 : 7, state, dev->st.rs[state], &ctx, &ops) == DL_OK) ops_apply(dev, &ops);
    }
  }
  /* texture stage states (stage 0's take the sampler states of the render states too) */
  for (UINT st = 0; st < D3D_STAGES; ++st) {
    DWORD bits = dev->dirty.tss[st];
    if (!bits) continue;
    dev->dirty.tss[st] = 0;
    for (UINT i = 1; i < D3D_TSS; ++i) {
      if (!((bits >> i) & 1u) || i == D3DTSS_ADDRESS) continue;
      if (dev->legacy_blend && dev->version < 7 && (st == 0 || st == 1) && i <= D3DTSS_ALPHAARG2) continue;      /* (TEXTUREMAPBLEND has them) */
      DlStateOps ops;
      if (dl_texture_stage_state(st, i, dev->st.tss[st][i], &ops) == DL_OK) ops_apply(dev, &ops);
    }
  }
  bind_textures(dev);
  ctx.texture = stage0_kind(dev);
  if (dev->legacy_blend && MASK_HAS(dev->dirty.rs, D3DRENDERSTATE_TEXTUREMAPBLEND)) {
    dev->dirty.rs[D3DRENDERSTATE_TEXTUREMAPBLEND >> 5] &= ~(1u << (D3DRENDERSTATE_TEXTUREMAPBLEND & 31u));
    DlStateOps ops;
    if (dl_texture_map_blend(dev->texture_map_blend, 0, ctx.texture, &ops) == DL_OK) ops_apply(dev, &ops);
    if (dev->texture_map_blend != D3DTBLEND_MODULATEMASK) {
      D3D_CALL(d9, SetTextureStageState, 1, D9_TSS_COLOROP, dev->st.tss[1][D3DTSS_COLOROP]);
      D3D_CALL(d9, SetTextureStageState, 1, D9_TSS_ALPHAOP, dev->st.tss[1][D3DTSS_ALPHAOP]);
    }
  }
  dev->dirty.rs[D3DRENDERSTATE_TEXTUREMAPBLEND >> 5] &= ~(1u << (D3DRENDERSTATE_TEXTUREMAPBLEND & 31u));
  /* transforms (Direct3D 7: the program's; 1-3 draw transformed vertices) */
  if (dev->version >= 7 && dev->dirty.xf) {
    for (UINT i = 0; i < D3D_XF; ++i) {
      if (!((dev->dirty.xf >> i) & 1u)) continue;
      const UINT t9 = dl_transform_state(7, i);
      if (t9) D3D_CALL(d9, SetTransform, t9, (const D3DMATRIX *)&dev->st.xf[i]);
    }
  }
  dev->dirty.xf = 0;
  if (dev->dirty.viewport) {
    D3DVIEWPORT9 vp;
    vp.X = dev->st.viewport.dwX;
    vp.Y = dev->st.viewport.dwY;
    vp.Width = dev->st.viewport.dwWidth;
    vp.Height = dev->st.viewport.dwHeight;
    vp.MinZ = dev->st.viewport.dvMinZ;
    vp.MaxZ = dev->st.viewport.dvMaxZ;
    /* (a viewport outside the target is cut to it: Direct3D 9 refuses one that is not inside) */
    const UINT tw = dev->target ? dev->target->desc.dwWidth : 0, th = dev->target ? dev->target->desc.dwHeight : 0;
    if (vp.X > tw) vp.X = tw;
    if (vp.Y > th) vp.Y = th;
    if (vp.Width > tw - vp.X) vp.Width = tw - vp.X;
    if (vp.Height > th - vp.Y) vp.Height = th - vp.Y;
    if (dev->version < 7) { vp.MinZ = 0.0f; vp.MaxZ = 1.0f; }
    D3D_CALL(d9, SetViewport, &vp);
    dev->dirty.viewport = FALSE;
  }
  if (dev->version >= 7) {
    if (dev->dirty.material) D3D_CALL(d9, SetMaterial, (const D3DMATERIAL9 *)&dev->st.material);
    dev->dirty.material = FALSE;
    BOOL lights = FALSE;
    for (UINT i = 0; i < dev->dirty.light_count; ++i) if (dev->dirty.lights[i]) { lights = TRUE; dev->dirty.lights[i] = 0; }
    if (lights) {
      /* (the first eight enabled lights, by index, are Direct3D 9's lights 0 .. 7) */
      UINT slot = 0;
      for (UINT i = 0; i < dev->st.light_count && slot < 8; ++i) {
        const D3dLight7 *l = &dev->st.lights[i];
        if (!l->set || !l->enabled) continue;
        D3D_CALL(d9, SetLight, slot, (const D3DLIGHT9 *)&l->light);
        D3D_CALL(d9, LightEnable, slot, TRUE);
        ++slot;
      }
      for (; slot < 8; ++slot) D3D_CALL(d9, LightEnable, slot, FALSE);
    }
    for (UINT i = 0; i < D3D_PLANES && dev->dirty.planes; ++i) {
      if (!((dev->dirty.planes >> i) & 1u)) continue;
      if (i < dev->dd3->caps9.MaxUserClipPlanes) D3D_CALL(d9, SetClipPlane, i, dev->st.plane[i]);
    }
    dev->dirty.planes = 0;
  } else {
    dev->dirty.material = FALSE;
    dev->dirty.planes = 0;
    for (UINT i = 0; i < dev->dirty.light_count; ++i) dev->dirty.lights[i] = 0;
  }
  return D3D_OK;
}

/* After a draw: the target (and the depth buffer) have newer pixels than the CPU's. */
static void d3_drawn(D3dDevice *dev) {
  if (!dev->target) return;
  dev->target->store.shadow.state |= DD_SHADOW_GPU_NEWER;
  Surface *z = attached_depth(dev->target);
  if (z && dev->st.rs[D3DRENDERSTATE_ZENABLE] && (dev->st.rs[D3DRENDERSTATE_ZWRITEENABLE] || dev->st.rs[D3DRENDERSTATE_STENCILENABLE]))
    z->store.shadow.state |= DD_SHADOW_GPU_NEWER;
}
