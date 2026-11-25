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

/* Part of d3d11.c: render targets with levels or layers, and GenerateMips.
 *
 * The kernel draws into images of one level and one layer, and samples textures of many. A render target texture of several levels, or an array, a cube or a
 * volume to draw into, is therefore a texture the kernel samples (Tex.shadowed), and each of its render target views has a target of its own the size of its
 * level (View.scratch). A draw or a clear goes into the view's target; before the texture is read - sampled, copied, mapped, filtered by GenerateMips - what its
 * views drew is copied into it (tex_sync), and before a view is drawn into again after the texture was written another way, the view's target is loaded from it
 * (rtv_load). The copies go through a buffer of the view's (View.link): the kernel copies images to and from buffers at any level of a texture and any layer of an
 * array, cube or volume (only level 0 of those: the kernel keeps one level of them). Both copies are recorded in order with the draws. */

/* The level and layer (or slice of a volume) a render target view's description names; FALSE for one the texture does not have. (A view of several layers draws
 * into the first: the kernel takes no SV_RenderTargetArrayIndex.) */
static BOOL rtv_subresource(const Tex *t, const D3D11_RENDER_TARGET_VIEW_DESC *desc, UINT *level, UINT *layer) {
  *level = *layer = 0;
  const UINT layers = t->kind == 1 ? t->slices : t->desc.ArraySize;
  switch (desc->ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE1D: *level = desc->Texture1D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: *level = desc->Texture1DArray.MipSlice; *layer = desc->Texture1DArray.FirstArraySlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2D: *level = desc->Texture2D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: *level = desc->Texture2DArray.MipSlice; *layer = desc->Texture2DArray.FirstArraySlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS: break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: *layer = desc->Texture2DMSArray.FirstArraySlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE3D: *level = desc->Texture3D.MipSlice; *layer = desc->Texture3D.FirstWSlice; break;
    default: return FALSE;
  }
  if (*level >= t->desc.MipLevels) return FALSE;
  const UINT have = t->kind == 1 ? mip_size(layers, *level) : layers;
  return *layer < have;
}

static HRESULT rtv_shadow_make(Dev *d, View *v, UINT level, UINT layer) {
  Tex *t = v->tex;
  const UINT w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, t->stored, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&v->scratch);
  if (FAILED(hr) || !v->scratch) { v->scratch = NULL; return FAILED(hr) ? hr : E_OUTOFMEMORY; }
  v->link_pitch = (w * t->stored_bpp + 255u) & ~255u;
  v->link = make_buffer(d, D3D12_HEAP_TYPE_DEFAULT, (UINT64)v->link_pitch * h);
  if (!v->link) { ID3D12Resource_Release(v->scratch); v->scratch = NULL; return E_OUTOFMEMORY; }
  v->level = level;
  v->layer = layer;
  v->loaded_version = t->version - 1;      /* (loaded at the first draw) */
  v->next_rt = t->rt_views;
  t->rt_views = v;
  return S_OK;
}

/* One copy between a view's target (or the texture's subresource) and its link buffer. `to_buffer`: from the image into the buffer. */
static void rtv_copy(Dev *d, View *v, ID3D12Resource *image, UINT sub, UINT z, BOOL to_buffer) {
  D3D12_TEXTURE_COPY_LOCATION img, buf;
  memset(&img, 0, sizeof img);
  memset(&buf, 0, sizeof buf);
  img.pResource = image;
  img.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  img.SubresourceIndex = sub;
  buf.pResource = v->link;
  buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  buf.PlacedFootprint.Footprint.Format = v->tex->stored;
  buf.PlacedFootprint.Footprint.Width = v->width;
  buf.PlacedFootprint.Footprint.Height = v->height;
  buf.PlacedFootprint.Footprint.Depth = 1;
  buf.PlacedFootprint.Footprint.RowPitch = v->link_pitch;
  if (to_buffer) {
    const D3D12_BOX slice = {0, 0, z, v->width, v->height, z + 1};
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &buf, 0, 0, 0, &img, z ? &slice : NULL);
  } else {
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &img, 0, 0, z, &buf, NULL);
  }
  d->recorded = 1;
}
/* The subresource of the texture a view is of, and the slice of a volume's; FALSE for a level the kernel does not keep (Tex.kmips). */
static BOOL rtv_target_sub(const View *v, UINT *sub, UINT *z) {
  const Tex *t = v->tex;
  if (v->level >= t->kmips) return FALSE;
  if (t->kind == 1) { *sub = v->level; *z = v->layer; }
  else { *sub = v->level + v->layer * t->kmips; *z = 0; }
  return TRUE;
}
/* What the view drew into the texture. */
static void rtv_store(Dev *d, View *v) {
  Tex *t = v->tex;
  UINT sub, z;
  v->newer = FALSE;
  if (rtv_target_sub(v, &sub, &z)) {
    tex_before_write(d, t);      /* (the kernel: not written after the list sampled it) */
    rtv_copy(d, v, v->scratch, 0, 0, TRUE);
    rtv_copy(d, v, t->res, sub, z, FALSE);
    t->used = t->written = d->epoch;
  }
  ++t->version;
  v->loaded_version = t->version;
}
static void tex_sync(Dev *d, Tex *t) {
  if (!t || !t->shadowed) return;
  for (View *v = t->rt_views; v; v = v->next_rt) if (v->newer) rtv_store(d, v);
}
/* Before a draw or a clear into the view: its target loaded from the texture when that changed since (not for a clear of all of it). Then it holds what is newer. */
static void rtv_load(Dev *d, View *v, BOOL whole) {
  if (!v->scratch) return;
  if (!v->newer && v->loaded_version != v->tex->version) {
    tex_sync(d, v->tex);      /* (another view of the same subresource may have drawn) */
    UINT sub, z;
    if (!whole && rtv_target_sub(v, &sub, &z)) {
      tex_before_read(d, v->tex);
      rtv_copy(d, v, v->tex->res, sub, z, TRUE);
      rtv_copy(d, v, v->scratch, 0, 0, FALSE);
      v->tex->used = d->epoch;
    }
    v->loaded_version = v->tex->version;
  }
  v->newer = TRUE;
}
static void rtv_shadow_destroy(Dev *d, View *v) {
  Tex *t = v->tex;
  if (v->newer) rtv_store(d, v);
  settle(d, d->epoch);      /* (the list reads the target and the buffer) */
  for (View **at = &t->rt_views; *at; at = &(*at)->next_rt)
    if (*at == v) { *at = v->next_rt; break; }
  ID3D12Resource_Release(v->scratch);
  ID3D12Resource_Release(v->link);
  v->scratch = v->link = NULL;
}

/* ---- GenerateMips ---------------------------------------------------------------------------------------------------------------------------------------------------- */
/* The levels of a texture made from its first, each the box-filtered half of the one before (the average of up to four texels, an odd edge's last texel kept), in
 * linear colour (sRGB texels are filtered as the light they stand for). Done on the CPU: the kernel's own GenerateMipmaps averages bytes, which is right for 8-bit
 * unorm texels alone. */

/* (the library has no C runtime: log2 by the mantissa's series, 2^x by its integer part and a series) */
static FLOAT log2f_(FLOAT x) {
  UINT32 bits;
  memcpy(&bits, &x, 4);
  const INT e = (INT)((bits >> 23) & 255u) - 127;
  bits = (bits & 0x7FFFFFu) | 0x3F800000u;
  FLOAT m;
  memcpy(&m, &bits, 4);
  const FLOAT t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
  const FLOAT ln = 2.0f * t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9)))));
  return (FLOAT)e + ln * 1.44269504f;
}
static FLOAT exp2f_(FLOAT x) {
  if (x < -126.0f) return 0.0f;
  if (x > 127.0f) x = 127.0f;
  INT i = (INT)x;
  if ((FLOAT)i > x) --i;
  const FLOAT f = (x - (FLOAT)i) * 0.693147181f;
  FLOAT term = 1.0f, sum = 1.0f;
  for (UINT k = 1; k < 9; ++k) { term *= f / (FLOAT)k; sum += term; }
  const UINT32 bits = (UINT32)(i + 127) << 23;
  FLOAT scale;
  memcpy(&scale, &bits, 4);
  return sum * scale;
}
static FLOAT powf_(FLOAT x, FLOAT y) { return x <= 0.0f ? 0.0f : exp2f_(y * log2f_(x)); }
static FLOAT srgb_to_linear(FLOAT c) { return c <= 0.04045f ? c / 12.92f : powf_((c + 0.055f) / 1.055f, 2.4f); }
static FLOAT linear_to_srgb(FLOAT c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf_(c, 1.0f / 2.4f) - 0.055f; }
/* A stored texel as four floats, and back (the stored formats: formats.inc). FALSE for one that is not filtered (integers, blocks). */
static BOOL texel_decode(DXGI_FORMAT f, const BYTE *in, FLOAT out[4]) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      for (UINT k = 0; k < 4; ++k) out[k] = in[k] / 255.0f;
      return TRUE;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      for (UINT k = 0; k < 4; ++k) out[k] = k < 3 ? srgb_to_linear(in[k] / 255.0f) : in[k] / 255.0f;
      return TRUE;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      for (UINT k = 0; k < 4; ++k) out[k] = half_to_float(load16(in + 2 * k));
      return TRUE;
    case DXGI_FORMAT_R16G16B16A16_UNORM: for (UINT k = 0; k < 4; ++k) out[k] = load16(in + 2 * k) / 65535.0f; return TRUE;
    case DXGI_FORMAT_R16G16B16A16_SNORM: for (UINT k = 0; k < 4; ++k) { const FLOAT x = (INT16)load16(in + 2 * k) / 32767.0f; out[k] = x < -1.0f ? -1.0f : x; } return TRUE;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: memcpy(out, in, 16); return TRUE;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
      const UINT32 v = load32(in);
      out[0] = (v & 1023u) / 1023.0f; out[1] = ((v >> 10) & 1023u) / 1023.0f; out[2] = ((v >> 20) & 1023u) / 1023.0f; out[3] = (v >> 30) / 3.0f;
      return TRUE;
    }
    /* (the one- and two-channel 8-bit ones, alpha alone, and the 16-bit packed ones: the channels they have, the others 0 and alpha 1) */
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: out[0] = in[0] / 255.0f; out[1] = out[2] = 0.0f; out[3] = 1.0f; return TRUE;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_TYPELESS: out[0] = in[0] / 255.0f; out[1] = in[1] / 255.0f; out[2] = 0.0f; out[3] = 1.0f; return TRUE;
    case DXGI_FORMAT_A8_UNORM: out[0] = out[1] = out[2] = 0.0f; out[3] = in[0] / 255.0f; return TRUE;
    case DXGI_FORMAT_B5G6R5_UNORM: { const UINT v = load16(in); out[0] = (v >> 11) / 31.0f; out[1] = ((v >> 5) & 63u) / 63.0f; out[2] = (v & 31u) / 31.0f; out[3] = 1.0f; return TRUE; }
    case DXGI_FORMAT_B5G5R5A1_UNORM: { const UINT v = load16(in); out[0] = ((v >> 10) & 31u) / 31.0f; out[1] = ((v >> 5) & 31u) / 31.0f; out[2] = (v & 31u) / 31.0f; out[3] = (FLOAT)(v >> 15); return TRUE; }
    case DXGI_FORMAT_B4G4R4A4_UNORM: { const UINT v = load16(in); out[0] = ((v >> 8) & 15u) / 15.0f; out[1] = ((v >> 4) & 15u) / 15.0f; out[2] = (v & 15u) / 15.0f; out[3] = (v >> 12) / 15.0f; return TRUE; }
    default: return FALSE;
  }
}
static void texel_encode(DXGI_FORMAT f, const FLOAT in[4], BYTE *out) {
  switch (f) {
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: out[0] = (BYTE)unorm_of(in[0], 8); return;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_TYPELESS: out[0] = (BYTE)unorm_of(in[0], 8); out[1] = (BYTE)unorm_of(in[1], 8); return;
    case DXGI_FORMAT_A8_UNORM: out[0] = (BYTE)unorm_of(in[3], 8); return;
    case DXGI_FORMAT_B5G6R5_UNORM: store16(out, (UINT16)(unorm_of(in[2], 5) | unorm_of(in[1], 6) << 5 | unorm_of(in[0], 5) << 11)); return;
    case DXGI_FORMAT_B5G5R5A1_UNORM: store16(out, (UINT16)(unorm_of(in[2], 5) | unorm_of(in[1], 5) << 5 | unorm_of(in[0], 5) << 10 | unorm_of(in[3], 1) << 15)); return;
    case DXGI_FORMAT_B4G4R4A4_UNORM: store16(out, (UINT16)(unorm_of(in[2], 4) | unorm_of(in[1], 4) << 4 | unorm_of(in[0], 4) << 8 | unorm_of(in[3], 4) << 12)); return;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      for (UINT k = 0; k < 4; ++k) out[k] = (BYTE)unorm_of(k < 3 ? linear_to_srgb(in[k]) : in[k], 8);
      return;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: for (UINT k = 0; k < 4; ++k) store16(out + 2 * k, half_of(in[k])); return;
    case DXGI_FORMAT_R16G16B16A16_UNORM: for (UINT k = 0; k < 4; ++k) store16(out + 2 * k, (UINT16)unorm_of(in[k], 16)); return;
    case DXGI_FORMAT_R16G16B16A16_SNORM: for (UINT k = 0; k < 4; ++k) store16(out + 2 * k, (UINT16)snorm_of(in[k], 16)); return;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: memcpy(out, in, 16); return;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      store32(out, unorm_of(in[0], 10) | unorm_of(in[1], 10) << 10 | unorm_of(in[2], 10) << 20 | unorm_of(in[3], 2) << 30);
      return;
    default: for (UINT k = 0; k < 4; ++k) out[k] = (BYTE)unorm_of(in[k], 8); return;
  }
}
static void STDMETHODCALLTYPE ctx_GenerateMips(Ctx *c, ID3D11ShaderResourceView *view) {
  View *v = (View *)view;
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_GENERATE_MIPS, .obj = {view}}); return; }
  if (!v || !v->tex) return;
  Tex *t = v->tex;
  /* (a texture that is a render target and a shader resource, made with D3D11_RESOURCE_MISC_GENERATE_MIPS) */
  if (!(t->desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) || (t->desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) !=
      (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) || t->kmips < 2 || predicated_out(c->dev))
    return;      /* (the levels of a volume, an array or a cube beyond the first are not kept: there is nothing to make) */
  FLOAT probe[4];
  BYTE zero[16] = {0};
  if (!texel_decode(t->stored, zero, probe)) return;
  Dev *d = c->dev;
  LOCK(c);
  const UINT bpp = t->stored_bpp;
  for (UINT layer = 0; layer < t->desc.ArraySize; ++layer) {
    UINT w = t->desc.Width, h = t->desc.Height;
    BYTE *above = mem_alloc((SIZE_T)w * h * bpp);
    if (!above || FAILED(tex_read_rows(d, t, layer * t->desc.MipLevels, 0, above, TRUE))) { mem_free(above); break; }
    for (UINT level = 1; level < t->kmips; ++level) {
      const UINT nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
      BYTE *below = mem_alloc((SIZE_T)nw * nh * bpp);
      if (!below) break;
      for (UINT y = 0; y < nh; ++y)
        for (UINT x = 0; x < nw; ++x) {
          FLOAT sum[4] = {0, 0, 0, 0}, texel[4];
          UINT n = 0;
          for (UINT dy = 0; dy < 2; ++dy)
            for (UINT dx = 0; dx < 2; ++dx) {
              const UINT sx = 2 * x + dx, sy = 2 * y + dy;
              if (sx >= w || sy >= h) continue;
              texel_decode(t->stored, above + ((SIZE_T)sy * w + sx) * bpp, texel);
              for (UINT k = 0; k < 4; ++k) sum[k] += texel[k];
              ++n;
            }
          for (UINT k = 0; k < 4; ++k) sum[k] /= (FLOAT)n;
          texel_encode(t->stored, sum, below + ((SIZE_T)y * nw + x) * bpp);
        }
      tex_write_rows(d, t, level + layer * t->desc.MipLevels, 0, 0, 0, nw, nh, below, nw * bpp, TRUE);
      mem_free(above);
      above = below;
      w = nw;
      h = nh;
    }
    mem_free(above);
  }
  UNLOCK(c);
}
