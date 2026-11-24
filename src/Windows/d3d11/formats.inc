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

/* Part of d3d11.c: the texture formats, and how each is kept. The kernel's images have four channels (RGBA32 float/uint/sint, RGBA16 float/unorm/snorm/uint/sint,
 * RGB10A2, RGBA8 unorm/sRGB/uint, BGRA8, the depth formats and BC1 .. BC3). A format of fewer channels is kept in the four-channel format of its kind, a channel a
 * texel wider where the kernel has no narrow one (8-bit snorm and sint in 16 bits); the channels it does not have read as 0 and alpha as 1, as Direct3D reads them,
 * and a draw into one writes only its own channels (pipeline_get masks the others). Packed formats are converted texel by texel. The CPU's side (UpdateSubresource,
 * Map, initial data) sees the program's format: texels are converted on the way in and out (tex_to_stored, tex_from_stored). */

enum { CH_UNORM, CH_SNORM, CH_UINT, CH_SINT, CH_FLOAT };
enum { PACK_NONE, PACK_R11G11B10, PACK_565, PACK_5551, PACK_4444, PACK_1010102U, PACK_999E5 };
#define FMT_TARGET 1u      /* may be a render target */
#define FMT_DEPTH 2u       /* a depth-stencil format */
#define FMT_BLOCK 4u       /* block-compressed (sampled only) */
#define FMT_SAMPLE 8u      /* only sampled (no render target, no unordered access) */
#define FMT_PLANAR 16u     /* NV12, P010, P016: a luma plane and a half-size plane of U, V (each a texture of its own below: `bpp` is a luma sample's bytes) */
#define FMT_YUV 32u        /* AYUV, Y410, Y416, YUY2, Y210, Y216: packed YUV, kept as it is (Direct3D 12's), seen through views of its pixels' RGB format only */

typedef struct TexFormat {
  DXGI_FORMAT format, stored;
  BYTE bpp, stored_bpp;
  BYTE channels;           /* 0: kept as it is (the bytes are copied); else the program's channels, `size` bytes each, kept as four of `stored_size` bytes */
  BYTE size, stored_size;
  BYTE type;               /* CH_*: what the channels are (for the missing ones and the widening) */
  BYTE packed;             /* PACK_*: converted texel by texel */
  BYTE alpha_only;         /* A8: the one channel is alpha */
  BYTE flags;              /* FMT_* */
} TexFormat;

#define F4(f, s, b, fl) {f, s, b, b, 0, 0, 0, 0, PACK_NONE, 0, fl}
#define FW(f, s, b, sb, ch, sz, ssz, ty, fl) {f, s, b, sb, ch, sz, ssz, ty, PACK_NONE, 0, fl}
#define FP(f, s, b, sb, pk, fl) {f, s, b, sb, 0, 0, 0, 0, pk, 0, fl}
static const TexFormat g_tex_formats[] = {
    /* four channels the kernel has */
    F4(DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_TYPELESS, 16, FMT_TARGET), F4(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, FMT_TARGET),
    F4(DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_UINT, 16, FMT_TARGET), F4(DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_SINT, 16, FMT_TARGET),
    F4(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_TYPELESS, 8, FMT_TARGET), F4(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, FMT_TARGET),
    F4(DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, 8, FMT_TARGET), F4(DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_UINT, 8, FMT_TARGET),
    F4(DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 8, FMT_TARGET), F4(DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_SINT, 8, FMT_TARGET),
    F4(DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_TYPELESS, 4, FMT_TARGET), F4(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, 4, FMT_TARGET),
    F4(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, FMT_TARGET), F4(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 4, FMT_TARGET),
    F4(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, FMT_TARGET), F4(DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UINT, 4, FMT_TARGET),
    F4(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 4, FMT_TARGET), F4(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS, 4, FMT_TARGET),
    F4(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 4, FMT_TARGET), F4(DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM, 4, FMT_TARGET),
    F4(DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_TYPELESS, 4, FMT_TARGET),
    /* (8-bit snorm and sint: in 16 bits) */
    FW(DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 4, 8, 4, 1, 2, CH_SNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R16G16B16A16_SINT, 4, 8, 4, 1, 2, CH_SINT, FMT_TARGET),
    /* fewer channels: in the four-channel format of their kind */
    FW(DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32A32_TYPELESS, 12, 16, 3, 4, 4, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 12, 16, 3, 4, 4, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT, 12, 16, 3, 4, 4, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT, 12, 16, 3, 4, 4, CH_SINT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32B32A32_TYPELESS, 8, 16, 2, 4, 4, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 16, 2, 4, 4, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_UINT, 8, 16, 2, 4, 4, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32A32_SINT, 8, 16, 2, 4, 4, CH_SINT, FMT_TARGET),
    FW(DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 16, 1, 4, 4, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32B32A32_UINT, 4, 16, 1, 4, 4, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32B32A32_SINT, 4, 16, 1, 4, 4, CH_SINT, FMT_TARGET),
    FW(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16B16A16_TYPELESS, 4, 8, 2, 2, 2, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 8, 2, 2, 2, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, 4, 8, 2, 2, 2, CH_UNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16B16A16_UINT, 4, 8, 2, 2, 2, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 4, 8, 2, 2, 2, CH_SNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16B16A16_SINT, 4, 8, 2, 2, 2, CH_SINT, FMT_TARGET),
    FW(DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 8, 1, 2, 2, CH_FLOAT, FMT_TARGET),
    FW(DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, 2, 8, 1, 2, 2, CH_UNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16B16A16_UINT, 2, 8, 1, 2, 2, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 2, 8, 1, 2, 2, CH_SNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16G16B16A16_SINT, 2, 8, 1, 2, 2, CH_SINT, FMT_TARGET),
    FW(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 4, 2, 1, 1, CH_UNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 4, 2, 1, 1, CH_UNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8B8A8_UINT, 2, 4, 2, 1, 1, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 2, 8, 2, 1, 2, CH_SNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R16G16B16A16_SINT, 2, 8, 2, 1, 2, CH_SINT, FMT_TARGET),
    FW(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, 1, 1, 1, CH_UNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, 1, 1, 1, CH_UNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8B8A8_UINT, 1, 4, 1, 1, 1, CH_UINT, FMT_TARGET),
    FW(DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, 1, 8, 1, 1, 2, CH_SNORM, FMT_TARGET),
    FW(DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R16G16B16A16_SINT, 1, 8, 1, 1, 2, CH_SINT, FMT_TARGET),
    {DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, 1, 1, 1, CH_UNORM, PACK_NONE, 1, FMT_TARGET},
    /* packed: converted texel by texel */
    FP(DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 8, PACK_R11G11B10, FMT_TARGET),
    FP(DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 4, PACK_565, FMT_TARGET),
    FP(DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 4, PACK_5551, FMT_TARGET),
    FP(DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 4, PACK_4444, FMT_TARGET),
    FP(DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_R16G16B16A16_UINT, 4, 8, PACK_1010102U, FMT_TARGET),
    FP(DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 8, PACK_999E5, FMT_SAMPLE),
    /* depth: the CPU does not reach them. (D16: in 32-bit float; D32 with 8 bits of stencil: in 24 bits of depth, the kernel having no 32-bit depth with stencil) */
    F4(DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, 4, FMT_DEPTH), F4(DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, FMT_DEPTH),
    F4(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_TYPELESS, 4, FMT_DEPTH), F4(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, 4, FMT_DEPTH),
    {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT, 2, 4, 0, 0, 0, 0, PACK_NONE, 0, FMT_DEPTH}, {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D32_FLOAT, 2, 4, 0, 0, 0, 0, PACK_NONE, 0, FMT_DEPTH},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 8, 4, 0, 0, 0, 0, PACK_NONE, 0, FMT_DEPTH},
    {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 8, 4, 0, 0, 0, 0, PACK_NONE, 0, FMT_DEPTH},
    /* block-compressed: kept as they are (the kernel decodes them where the device does not sample them so); sampled only */
    F4(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_TYPELESS, 8, FMT_BLOCK), F4(DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, 8, FMT_BLOCK),
    F4(DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC1_UNORM_SRGB, 8, FMT_BLOCK), F4(DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_TYPELESS, 16, FMT_BLOCK),
    F4(DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, 16, FMT_BLOCK), F4(DXGI_FORMAT_BC2_UNORM_SRGB, DXGI_FORMAT_BC2_UNORM_SRGB, 16, FMT_BLOCK),
    F4(DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_TYPELESS, 16, FMT_BLOCK), F4(DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, 16, FMT_BLOCK),
    F4(DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB, 16, FMT_BLOCK),
    /* planar YUV: Direct3D 12's planar textures (each plane of each slice an image of the kernel's); their views are of one plane (R8 / R8G8, R16 / R16G16) */
    F4(DXGI_FORMAT_NV12, DXGI_FORMAT_NV12, 1, FMT_TARGET | FMT_PLANAR), F4(DXGI_FORMAT_P010, DXGI_FORMAT_P010, 2, FMT_TARGET | FMT_PLANAR),
    F4(DXGI_FORMAT_P016, DXGI_FORMAT_P016, 2, FMT_TARGET | FMT_PLANAR),
    /* packed YUV (DXGI_FORMAT): AYUV as R8G8B8A8 (V, U, Y, A; drawn into too), Y410 as R10G10B10A2 (U, Y, V, A), Y416 as R16G16B16A16 (U, Y, V, A); YUY2 as R8G8B8A8 and
     * Y210 / Y216 as R16G16B16A16 of half the width (Y0, U, Y1, V: `bpp` is a pixel's share of a pair). Shader resource and unordered access views of the others */
    F4(DXGI_FORMAT_AYUV, DXGI_FORMAT_AYUV, 4, FMT_TARGET | FMT_YUV), F4(DXGI_FORMAT_Y410, DXGI_FORMAT_Y410, 4, FMT_YUV), F4(DXGI_FORMAT_Y416, DXGI_FORMAT_Y416, 8, FMT_YUV),
    F4(DXGI_FORMAT_YUY2, DXGI_FORMAT_YUY2, 2, FMT_YUV), F4(DXGI_FORMAT_Y210, DXGI_FORMAT_Y210, 4, FMT_YUV), F4(DXGI_FORMAT_Y216, DXGI_FORMAT_Y216, 4, FMT_YUV),
};
/* A packed 4:2:2 format: a pixel of its views is a pair of the texture's (columns and widths on whole pairs). */
static BOOL format_pairs(DXGI_FORMAT f) { return f == DXGI_FORMAT_YUY2 || f == DXGI_FORMAT_Y210 || f == DXGI_FORMAT_Y216; }
/* Whether a view of a packed YUV texture may have `view` (DXGI_FORMAT): R8G8B8A8_UNORM / _UINT for AYUV and YUY2, R10G10B10A2_UNORM / _UINT for Y410, R16G16B16A16_UNORM /
 * _UINT for Y416, Y210, Y216; render targets of AYUV alone. (The R32_UINT unordered access view and YUY2's R8G8_B8G8_UNORM view are not had: Direct3D 12 below has
 * neither.) */
static BOOL yuv_view_ok(DXGI_FORMAT tex, DXGI_FORMAT view, BOOL render_target) {
  switch (tex) {
    case DXGI_FORMAT_AYUV: return view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UINT;
    case DXGI_FORMAT_YUY2: return !render_target && (view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UINT);
    case DXGI_FORMAT_Y410: return !render_target && (view == DXGI_FORMAT_R10G10B10A2_UNORM || view == DXGI_FORMAT_R10G10B10A2_UINT);
    case DXGI_FORMAT_Y416: case DXGI_FORMAT_Y210: case DXGI_FORMAT_Y216: return !render_target && (view == DXGI_FORMAT_R16G16B16A16_UNORM || view == DXGI_FORMAT_R16G16B16A16_UINT);
    default: return FALSE;
  }
}
#undef F4
#undef FW
#undef FP

static const TexFormat g_raw32 = {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 4, 0, 0, 0, 0, PACK_NONE, 0, FMT_SAMPLE};
static const TexFormat g_raw64 = {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R16G16B16A16_TYPELESS, 8, 8, 0, 0, 0, 0, PACK_NONE, 0, FMT_SAMPLE};
static const TexFormat *tex_format(DXGI_FORMAT format) {
  for (UINT i = 0; i < sizeof g_tex_formats / sizeof g_tex_formats[0]; ++i)
    if (g_tex_formats[i].format == format) return &g_tex_formats[i];
  return NULL;
}
/* A depth format a depth-stencil texture of `format` is made with (the typeless ones by what they are for). */
static const TexFormat *tex_format_for(DXGI_FORMAT format, UINT bind) {
  const TexFormat *f = tex_format(format);
  if (!f) return NULL;
  const BOOL depth = (bind & D3D11_BIND_DEPTH_STENCIL) != 0;
  /* (R32_TYPELESS, R24G8_TYPELESS, R16_TYPELESS, R32G8X24_TYPELESS are depth formats only when bound as one: R32_TYPELESS and R16_TYPELESS are colour ones otherwise) */
  if (!depth && format == DXGI_FORMAT_R32_TYPELESS) return tex_format(DXGI_FORMAT_R32_FLOAT);
  if (!depth && format == DXGI_FORMAT_R16_TYPELESS) return tex_format(DXGI_FORMAT_R16_UNORM);
  /* (R24G8 and R32G8X24 not made for depth: their bytes, kept as they are in a format of their size - what views of them read is not converted) */
  if (!depth && format == DXGI_FORMAT_R24G8_TYPELESS && !(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))) return &g_raw32;
  if (!depth && format == DXGI_FORMAT_R32G8X24_TYPELESS && !(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))) return &g_raw64;
  if (!!(f->flags & FMT_DEPTH) != depth) return NULL;
  if ((f->flags & (FMT_BLOCK | FMT_SAMPLE)) && (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))) return NULL;
  if ((bind & D3D11_BIND_RENDER_TARGET) && !(f->flags & FMT_TARGET)) return NULL;
  return f;
}
/* The write mask of a render target of a format of fewer channels than it is kept with (D3D12_COLOR_WRITE_ENABLE_*: the channels it has). */
static UINT8 tex_format_mask(const TexFormat *f) {
  if (!f || (!f->channels && f->packed != PACK_R11G11B10 && f->packed != PACK_565)) return 0xF;
  if (f->alpha_only) return 8;
  const UINT n = f->packed ? 3 : f->channels;
  return (UINT8)((1u << n) - 1);
}

/* ---- the texels -------------------------------------------------------------------------------------------------------------------------------------------------- */

static FLOAT half_to_float(UINT16 h) {
  const UINT32 sign = (UINT32)(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
  UINT32 bits;
  if (exponent == 0) {
    if (!mantissa) bits = sign;
    else { UINT32 e = 113, m = mantissa; while (!(m & 0x400u)) { m <<= 1; --e; } bits = sign | (e << 23) | ((m & 0x3FFu) << 13); }
  } else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
  else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  FLOAT f;
  memcpy(&f, &bits, 4);
  return f;
}
static UINT16 half_of(FLOAT f);      /* (context.inc) */
/* An unsigned float of `mantissa_bits` and 5 exponent bits (R11G11B10's), from and to a float. */
static FLOAT small_float_to_float(UINT32 v, UINT mantissa_bits) {
  const UINT32 exponent = v >> mantissa_bits, mantissa = v & ((1u << mantissa_bits) - 1);
  if (exponent == 31) return mantissa ? 0.0f / 0.0f : 1.0f / 0.0f;
  if (exponent == 0) return (FLOAT)mantissa / (FLOAT)(1u << mantissa_bits) / 16384.0f;
  FLOAT scale = 1.0f;
  for (INT e = (INT)exponent - 15; e > 0; --e) scale *= 2.0f;
  for (INT e = (INT)exponent - 15; e < 0; ++e) scale *= 0.5f;
  return scale * (1.0f + (FLOAT)mantissa / (FLOAT)(1u << mantissa_bits));
}
static UINT32 float_to_small_float(FLOAT f, UINT mantissa_bits) {
  if (!(f > 0.0f)) return 0;      /* (negative and NaN: 0; no sign) */
  const UINT16 h = half_of(f);      /* (the same exponent bias and range as a half) */
  if ((h & 0x7C00u) == 0x7C00u) return (31u << mantissa_bits) | ((h & 0x3FFu) ? 1u : 0u);
  return (UINT32)(h & 0x7FFFu) >> (10 - mantissa_bits);
}
static UINT16 load16(const BYTE *p) { UINT16 v; memcpy(&v, p, 2); return v; }
static UINT32 load32(const BYTE *p) { UINT32 v; memcpy(&v, p, 4); return v; }
static void store16(BYTE *p, UINT16 v) { memcpy(p, &v, 2); }
static void store32(BYTE *p, UINT32 v) { memcpy(p, &v, 4); }

/* One channel of `size` bytes as a channel of `to_size` of the same kind (`type`). */
static void channel_widen(const BYTE *in, UINT size, BYTE *out, UINT to_size, UINT type) {
  if (size == to_size) { memcpy(out, in, size); return; }
  /* (only 8 to 16 bits) */
  const BYTE v = in[0];
  INT16 w;
  switch (type) {
    case CH_UNORM: store16(out, (UINT16)(v * 257u)); return;
    case CH_UINT: store16(out, v); return;
    case CH_SINT: store16(out, (UINT16)(INT16)(INT8)v); return;
    case CH_SNORM: {
      const INT s = (INT8)v;
      w = (INT16)(s <= -127 ? -32767 : (s * 32767 + (s >= 0 ? 63 : -63)) / 127);
      store16(out, (UINT16)w);
      return;
    }
    default: memcpy(out, in, size < to_size ? size : to_size); return;
  }
}
static void channel_narrow(const BYTE *in, UINT size, BYTE *out, UINT to_size, UINT type) {
  if (size == to_size) { memcpy(out, in, size); return; }
  const UINT16 w = load16(in);
  switch (type) {
    case CH_UNORM: out[0] = (BYTE)((w + 128u) / 257u); return;
    case CH_UINT: out[0] = (BYTE)(w > 255 ? 255 : w); return;
    case CH_SINT: { const INT16 s = (INT16)w; out[0] = (BYTE)(INT8)(s > 127 ? 127 : s < -128 ? -128 : s); return; }
    case CH_SNORM: { const INT s = (INT16)w; out[0] = (BYTE)(INT8)((s * 127 + (s >= 0 ? 16383 : -16383)) / 32767); return; }
    default: out[0] = in[0]; return;
  }
}
/* The value of a channel the program's format does not have: 0, and 1 for alpha. */
static void channel_missing(BYTE *out, UINT size, UINT type, BOOL alpha) {
  memset(out, 0, size);
  if (!alpha) return;
  switch (type) {
    case CH_UNORM: memset(out, 0xFF, size); return;
    case CH_SNORM: if (size == 1) out[0] = 0x7F; else store16(out, 0x7FFF); return;
    case CH_UINT: case CH_SINT: out[0] = 1; return;
    case CH_FLOAT: if (size == 2) store16(out, 0x3C00); else store32(out, 0x3F800000u); return;
  }
}

/* A texel of the program's format as the stored one. */
static void texel_to_stored(const TexFormat *f, const BYTE *in, BYTE *out) {
  if (f->packed) {
    UINT32 v = f->bpp == 2 ? load16(in) : load32(in);
    switch (f->packed) {
      case PACK_R11G11B10:
        store16(out, half_of(small_float_to_float(v & 0x7FFu, 6)));
        store16(out + 2, half_of(small_float_to_float((v >> 11) & 0x7FFu, 6)));
        store16(out + 4, half_of(small_float_to_float(v >> 22, 5)));
        store16(out + 6, 0x3C00);
        return;
      case PACK_565: out[0] = (BYTE)((v & 31u) * 255u / 31u); out[1] = (BYTE)(((v >> 5) & 63u) * 255u / 63u); out[2] = (BYTE)(((v >> 11) & 31u) * 255u / 31u); out[3] = 255; return;
      case PACK_5551: out[0] = (BYTE)((v & 31u) * 255u / 31u); out[1] = (BYTE)(((v >> 5) & 31u) * 255u / 31u); out[2] = (BYTE)(((v >> 10) & 31u) * 255u / 31u); out[3] = (v >> 15) ? 255 : 0; return;
      case PACK_4444: out[0] = (BYTE)((v & 15u) * 17u); out[1] = (BYTE)(((v >> 4) & 15u) * 17u); out[2] = (BYTE)(((v >> 8) & 15u) * 17u); out[3] = (BYTE)((v >> 12) * 17u); return;
      case PACK_1010102U: store16(out, (UINT16)(v & 1023u)); store16(out + 2, (UINT16)((v >> 10) & 1023u)); store16(out + 4, (UINT16)((v >> 20) & 1023u)); store16(out + 6, (UINT16)(v >> 30)); return;
      case PACK_999E5: {
        FLOAT scale = 1.0f;
        for (INT e = (INT)(v >> 27) - 15 - 9; e > 0; --e) scale *= 2.0f;
        for (INT e = (INT)(v >> 27) - 15 - 9; e < 0; ++e) scale *= 0.5f;
        store16(out, half_of((FLOAT)(v & 511u) * scale));
        store16(out + 2, half_of((FLOAT)((v >> 9) & 511u) * scale));
        store16(out + 4, half_of((FLOAT)((v >> 18) & 511u) * scale));
        store16(out + 6, 0x3C00);
        return;
      }
    }
    return;
  }
  if (!f->channels) { memcpy(out, in, f->bpp); return; }
  for (UINT c = 0; c < 4; ++c) {
    BYTE *to = out + c * f->stored_size;
    const BOOL have = f->alpha_only ? c == 3 : c < f->channels;
    if (have) channel_widen(in + (f->alpha_only ? 0 : c * f->size), f->size, to, f->stored_size, f->type);
    else channel_missing(to, f->stored_size, f->type, c == 3);
  }
}
/* A stored texel as the program's format has it. */
static void texel_from_stored(const TexFormat *f, const BYTE *in, BYTE *out) {
  if (f->packed) {
    UINT32 v = 0;
    switch (f->packed) {
      case PACK_R11G11B10:
        v = float_to_small_float(half_to_float(load16(in)), 6) | float_to_small_float(half_to_float(load16(in + 2)), 6) << 11 | float_to_small_float(half_to_float(load16(in + 4)), 5) << 22;
        break;
      case PACK_565: v = (in[0] * 31u + 127u) / 255u | ((in[1] * 63u + 127u) / 255u) << 5 | ((in[2] * 31u + 127u) / 255u) << 11; break;
      case PACK_5551: v = (in[0] * 31u + 127u) / 255u | ((in[1] * 31u + 127u) / 255u) << 5 | ((in[2] * 31u + 127u) / 255u) << 10 | (in[3] >= 128 ? 1u : 0u) << 15; break;
      case PACK_4444: v = (in[0] + 8u) / 17u | ((in[1] + 8u) / 17u) << 4 | ((in[2] + 8u) / 17u) << 8 | ((in[3] + 8u) / 17u) << 12; break;
      case PACK_1010102U: v = (load16(in) & 1023u) | (load16(in + 2) & 1023u) << 10 | (load16(in + 4) & 1023u) << 20 | (load16(in + 6) & 3u) << 30; break;
      case PACK_999E5: {
        /* (the shared exponent of the largest channel, as DXGI does it) */
        FLOAT rgb[3] = {half_to_float(load16(in)), half_to_float(load16(in + 2)), half_to_float(load16(in + 4))}, largest = 0;
        for (UINT k = 0; k < 3; ++k) { if (!(rgb[k] > 0)) rgb[k] = 0; if (rgb[k] > 65408.0f) rgb[k] = 65408.0f; if (rgb[k] > largest) largest = rgb[k]; }
        INT e = -16;
        FLOAT p = 1.0f / 65536.0f;
        while (e < 15 && p <= largest) { p *= 2.0f; ++e; }
        const UINT32 exponent = (UINT32)(e + 1 + 15 > 0 ? e + 1 + 15 : 0);
        FLOAT scale = 1.0f;
        for (INT k = (INT)exponent - 15 - 9; k > 0; --k) scale *= 0.5f;
        for (INT k = (INT)exponent - 15 - 9; k < 0; ++k) scale *= 2.0f;
        UINT32 m[3];
        for (UINT k = 0; k < 3; ++k) { const FLOAT x = rgb[k] * scale + 0.5f; m[k] = x >= 511.0f ? 511u : (UINT32)x; }
        v = m[0] | m[1] << 9 | m[2] << 18 | exponent << 27;
        break;
      }
    }
    if (f->bpp == 2) store16(out, (UINT16)v); else store32(out, v);
    return;
  }
  if (!f->channels) { memcpy(out, in, f->bpp); return; }
  if (f->alpha_only) { channel_narrow(in + 3 * f->stored_size, f->stored_size, out, f->size, f->type); return; }
  for (UINT c = 0; c < f->channels; ++c) channel_narrow(in + c * f->stored_size, f->stored_size, out + c * f->size, f->size, f->type);
}
/* A row of `w` texels, the program's to the stored format and back (a row of blocks of a compressed format is copied). */
static void row_to_stored(const TexFormat *f, const BYTE *in, BYTE *out, UINT w, UINT bytes) {
  if (!f->channels && !f->packed) { memcpy(out, in, bytes); return; }
  for (UINT i = 0; i < w; ++i) texel_to_stored(f, in + (SIZE_T)i * f->bpp, out + (SIZE_T)i * f->stored_bpp);
}
static void row_from_stored(const TexFormat *f, const BYTE *in, BYTE *out, UINT w, UINT bytes) {
  if (!f->channels && !f->packed) { memcpy(out, in, bytes); return; }
  for (UINT i = 0; i < w; ++i) texel_from_stored(f, in + (SIZE_T)i * f->stored_bpp, out + (SIZE_T)i * f->bpp);
}

/* ---- planar formats ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* A plane of a planar format as a format of its own (what views and copies of it are): plane 0 the luma (R8 / R16), plane 1 U and V (R8G8 / R16G16). */
static DXGI_FORMAT plane_format_of(DXGI_FORMAT planar, UINT plane) {
  const BOOL sixteen = planar == DXGI_FORMAT_P010 || planar == DXGI_FORMAT_P016;
  return plane == 0 ? (sixteen ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM) : (sixteen ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM);
}
/* The plane a view of a planar texture names by its format: R8_UNORM / R16_UNORM the luma, R8G8_UNORM / R16G16_UNORM the chroma, and their integer forms R8_UINT / R16_UINT
 * and R8G8_UINT / R16G16_UINT (Direct3D 12 below reads and draws those through mirrors of the plane in the integer format of its bits). FALSE for another format: the planar
 * format itself (a view is of one plane), and the typeless forms, which a view cannot have. */
static BOOL plane_of_view_format(DXGI_FORMAT planar, DXGI_FORMAT view, UINT *plane) {
  const BOOL sixteen = planar == DXGI_FORMAT_P010 || planar == DXGI_FORMAT_P016;
  for (UINT p = 0; p < 2; ++p)
    if (view == plane_format_of(planar, p)) { *plane = p; return TRUE; }
  if (view == (sixteen ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT)) { *plane = 0; return TRUE; }
  if (view == (sixteen ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R8G8_UINT)) { *plane = 1; return TRUE; }
  return FALSE;
}
