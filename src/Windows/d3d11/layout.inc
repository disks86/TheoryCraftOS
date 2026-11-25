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
/* Part of d3d11.c: CPU access to textures of default usage (Direct3D 11.3: D3D11_FEATURE_D3D11_OPTIONS2's MapOnDefaultTextures and StandardSwizzle).
 *
 * A texture of D3D11_USAGE_DEFAULT may have D3D11_CPU_ACCESS_READ and _WRITE (not a depth buffer, multisampled, shared, tiled or a video format). How the CPU reaches it is
 * its D3D11_TEXTURE_LAYOUT (CreateTexture2D1 / 3D1):
 * - UNDEFINED (the driver's layout): Map with no D3D11_MAPPED_SUBRESOURCE (a pointer given is E_INVALIDARG) says the CPU will use the subresource; while it is mapped,
 *   ID3D11Device3::WriteToSubresource and ReadFromSubresource copy a box of it from and to the program's memory, at once (the GPU's work before is done first).
 * - ROW_MAJOR (a 2D texture of one level and one slice): Map hands out the texels in rows, as a staging texture's.
 * - 64K_STANDARD_SWIZZLE (2D or 3D, elements of 1 to 16 bytes, a block of a compressed format one element): Map hands out the subresource in Direct3D's standard swizzle
 *   (src/Windows/tcos_swizzle.h): 64 KiB tiles in rows of tiles; RowPitch is the bytes of a row of tiles, DepthPitch of a layer of them (a 3D tile is several slices deep).
 * The texture itself is the kernel's image like any other: a Map reads the subresource into the CPU's copy in the layout asked for (whatever the map type, so that what the
 * program does not write keeps its texels), and an Unmap after a map for writing puts the copy back. Map types: READ needs CPU_ACCESS_READ, WRITE CPU_ACCESS_WRITE,
 * READ_WRITE both; WRITE_DISCARD and WRITE_NO_OVERWRITE are for dynamic resources (E_INVALIDARG). */

/* Whether a D3D11_TEXTURE_LAYOUT can be had for the description (`three_d`: a volume, its description as a 2D one of one slice). */
static BOOL layout_valid(UINT layout, BOOL three_d, const D3D11_TEXTURE2D_DESC *desc) {
  if (layout == D3D11_TEXTURE_LAYOUT_UNDEFINED) return TRUE;
  if (layout != D3D11_TEXTURE_LAYOUT_ROW_MAJOR && layout != D3D11_TEXTURE_LAYOUT_64K_STANDARD_SWIZZLE) return FALSE;
  const TexFormat *f = tex_format(desc->Format);
  const UINT refused_misc = D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_SHARED |
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (!f || (f->flags & (FMT_DEPTH | FMT_PLANAR | FMT_YUV)) || desc->Usage != D3D11_USAGE_DEFAULT || desc->SampleDesc.Count != 1 || (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
      (desc->MiscFlags & refused_misc))
    return FALSE;
  if (layout == D3D11_TEXTURE_LAYOUT_ROW_MAJOR) return !three_d && desc->MipLevels == 1 && desc->ArraySize == 1;
  const UINT bpe = f->bpp;      /* (a compressed format's: its block's) */
  return bpe == 1 || bpe == 2 || bpe == 4 || bpe == 8 || bpe == 16;
}

/* A level's size in elements (a compressed format's elements are its 4 x 4 blocks) and its slices. */
static void layout_elements(const Tex *t, UINT sub, UINT *w, UINT *h, UINT *d) {
  const UINT level = sub % t->desc.MipLevels, lw = mip_size(t->desc.Width, level), lh = mip_size(t->desc.Height, level);
  *w = t->block ? (lw + 3) / 4 : lw;
  *h = t->block ? (lh + 3) / 4 : lh;
  *d = tex_slices(t, sub);
}

/* The CPU's copy of a subresource of a swizzled or row-major texture, read from the texture (the kernel's image, after the work recorded before it). */
static HRESULT layout_fill(Dev *d, Tex *t, UINT sub) {
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level), slices = tex_slices(t, sub);
  const SIZE_T pitch = tex_pitch(t, w), slice_bytes = pitch * tex_rows(t, h);
  if (t->layout == D3D11_TEXTURE_LAYOUT_ROW_MAJOR) {
    HRESULT hr = S_OK;
    for (UINT z = 0; SUCCEEDED(hr) && z < slices; ++z) hr = tex_read(d, t, sub, z, t->shadow[sub] + z * slice_bytes);
    return hr;
  }
  TcosSwizzle s;
  UINT ew, eh, ed;
  layout_elements(t, sub, &ew, &eh, &ed);
  if (!tcos_swizzle_init(&s, t->bpp, t->kind == 1)) return E_INVALIDARG;
  BYTE *linear = mem_alloc(slice_bytes);
  if (!linear) return E_OUTOFMEMORY;
  HRESULT hr = S_OK;
  const SIZE_T bytes = tcos_swizzle_bytes(&s, ew, eh, ed);
  for (UINT z = 0; SUCCEEDED(hr) && z < slices; ++z) {
    hr = tex_read(d, t, sub, z, linear);
    if (SUCCEEDED(hr) && !tcos_swizzle_copy(&s, t->shadow[sub], bytes, ew, eh, ed, 0, 0, z, ew, eh, 1, linear, pitch, slice_bytes, TRUE)) hr = E_FAIL;
  }
  mem_free(linear);
  return hr;
}
/* ... and put back into the texture. */
static void layout_flush(Dev *d, Tex *t, UINT sub) {
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level), slices = tex_slices(t, sub);
  const SIZE_T pitch = tex_pitch(t, w), slice_bytes = pitch * tex_rows(t, h);
  if (t->layout == D3D11_TEXTURE_LAYOUT_ROW_MAJOR) {
    for (UINT z = 0; z < slices; ++z) tex_write(d, t, sub, 0, 0, z, w, h, t->shadow[sub] + z * slice_bytes, (UINT)pitch);
    return;
  }
  TcosSwizzle s;
  UINT ew, eh, ed;
  layout_elements(t, sub, &ew, &eh, &ed);
  if (!tcos_swizzle_init(&s, t->bpp, t->kind == 1)) return;
  BYTE *linear = mem_alloc(slice_bytes);
  if (!linear) return;
  const SIZE_T bytes = tcos_swizzle_bytes(&s, ew, eh, ed);
  for (UINT z = 0; z < slices; ++z)
    if (tcos_swizzle_copy(&s, t->shadow[sub], bytes, ew, eh, ed, 0, 0, z, ew, eh, 1, linear, pitch, slice_bytes, FALSE)) tex_write(d, t, sub, 0, 0, z, w, h, linear, (UINT)pitch);
  mem_free(linear);
}

/* Map of a default texture with CPU access (the immediate context's; the device is locked). */
static HRESULT tex_map_default(Dev *d, Tex *t, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE *out) {
  const UINT access = t->desc.CPUAccessFlags;
  if (sub >= tex_subresources(t) || t->mapped[sub]) return E_INVALIDARG;
  if ((type == D3D11_MAP_READ && !(access & D3D11_CPU_ACCESS_READ)) || (type == D3D11_MAP_WRITE && !(access & D3D11_CPU_ACCESS_WRITE)) ||
      (type == D3D11_MAP_READ_WRITE && (access & (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)) != (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)) ||
      (type != D3D11_MAP_READ && type != D3D11_MAP_WRITE && type != D3D11_MAP_READ_WRITE))
    return E_INVALIDARG;
  if (t->layout == D3D11_TEXTURE_LAYOUT_UNDEFINED) {
    /* (no address: the subresource is reached with WriteToSubresource / ReadFromSubresource) */
    if (out) return E_INVALIDARG;
    t->mapped[sub] = type;
    return S_OK;
  }
  if (!out) return E_INVALIDARG;
  UINT ew, eh, ed;
  layout_elements(t, sub, &ew, &eh, &ed);
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  SIZE_T bytes, row_pitch, depth_pitch;
  if (t->layout == D3D11_TEXTURE_LAYOUT_ROW_MAJOR) {
    row_pitch = tex_pitch(t, w);
    depth_pitch = row_pitch * tex_rows(t, h);
    bytes = depth_pitch * ed;
  } else {
    TcosSwizzle s;
    UINT across, down, deep;
    if (!tcos_swizzle_init(&s, t->bpp, t->kind == 1) || !tcos_swizzle_tiles(&s, ew, eh, ed, &across, &down, &deep)) return E_INVALIDARG;
    bytes = tcos_swizzle_bytes(&s, ew, eh, ed);
    row_pitch = (SIZE_T)across * 65536u;
    depth_pitch = row_pitch * down;
    if (!bytes || depth_pitch > 0xFFFFFFFFu) return E_OUTOFMEMORY;
  }
  /* (the copy of a swizzled level is larger than a row-major one: made for its layout) */
  mem_free(t->shadow[sub]);
  t->shadow[sub] = mem_alloc(bytes);
  if (!t->shadow[sub]) return E_OUTOFMEMORY;
  memset(t->shadow[sub], 0, bytes);
  const HRESULT hr = layout_fill(d, t, sub);
  if (FAILED(hr)) return hr;
  t->mapped[sub] = type;
  out->pData = t->shadow[sub];
  out->RowPitch = (UINT)row_pitch;
  out->DepthPitch = (UINT)depth_pitch;
  return S_OK;
}
static void tex_unmap_default(Dev *d, Tex *t, UINT sub) {
  if (sub >= tex_subresources(t) || !t->mapped[sub]) return;
  const D3D11_MAP type = t->mapped[sub];
  t->mapped[sub] = 0;
  if (t->layout != D3D11_TEXTURE_LAYOUT_UNDEFINED && type != D3D11_MAP_READ) layout_flush(d, t, sub);
}

/* WriteToSubresource (`write`) and ReadFromSubresource: a box of a mapped subresource of a default texture of the driver's layout, from or to the program's memory (rows
 * `row_pitch` apart, slices `depth_pitch`; a compressed format's rows are rows of blocks). The subresource must be mapped for that (WRITE / READ_WRITE to write, READ /
 * READ_WRITE to read); a call that is not valid does nothing, as the methods return nothing. */
static void tex_cpu_copy(Dev *d, Tex *t, UINT sub, const D3D11_BOX *box, BYTE *mem, UINT row_pitch, UINT depth_pitch, BOOL write) {
  if (!t || !mem || t->desc.Usage != D3D11_USAGE_DEFAULT || t->layout != D3D11_TEXTURE_LAYOUT_UNDEFINED || sub >= tex_subresources(t)) return;
  const D3D11_MAP type = t->mapped[sub];
  if (write ? (type != D3D11_MAP_WRITE && type != D3D11_MAP_READ_WRITE) : (type != D3D11_MAP_READ && type != D3D11_MAP_READ_WRITE)) return;
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level), slices = tex_slices(t, sub);
  D3D11_BOX b = {0, 0, 0, w, h, slices};
  if (box) b = *box;
  if (b.left >= b.right || b.top >= b.bottom || b.front >= b.back) return;      /* (an empty box: nothing) */
  if (b.right > w || b.bottom > h || b.back > slices) return;
  if (t->block && (b.left % 4 || b.top % 4 || (b.right % 4 && b.right != w) || (b.bottom % 4 && b.bottom != h))) return;
  const UINT bw = b.right - b.left, bh = b.bottom - b.top;
  if (write) {
    for (UINT z = b.front; z < b.back; ++z) tex_write(d, t, sub, b.left, b.top, z, bw, bh, mem + (SIZE_T)(z - b.front) * depth_pitch, row_pitch);
    return;
  }
  const SIZE_T pitch = tex_pitch(t, w), slice_bytes = pitch * tex_rows(t, h);
  BYTE *linear = mem_alloc(slice_bytes);
  if (!linear) return;
  const SIZE_T x_bytes = tex_pitch(t, b.left), row_bytes = tex_pitch(t, b.right) - x_bytes;
  const UINT first_row = t->block ? b.top / 4 : b.top, rows = tex_rows(t, b.bottom) - first_row;
  for (UINT z = b.front; z < b.back; ++z) {
    if (FAILED(tex_read(d, t, sub, z, linear))) break;
    for (UINT y = 0; y < rows; ++y) memcpy(mem + (SIZE_T)(z - b.front) * depth_pitch + (SIZE_T)y * row_pitch, linear + (SIZE_T)(first_row + y) * pitch + x_bytes, row_bytes);
  }
  mem_free(linear);
}
