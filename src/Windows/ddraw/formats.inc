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
/* The pixel formats of DirectDraw (DDPIXELFORMAT) and those of ddraw_pixels.h, and the surface descriptions of the interface versions
 * (DDSURFACEDESC of IDirectDraw/2 and IDirectDrawSurface/2/3, DDSURFACEDESC2 of the others). Included by ddraw.c. */

/* The format a DDPIXELFORMAT describes (DD_FMT_UNKNOWN: none DirectDraw has), by the rules Windows matches them with: the flags, the bit count and the
 * masks. */
static UINT format_from_ddpf(const DDPIXELFORMAT *pf) {
  const DWORD f = pf->dwFlags;
  if (f & DDPF_PALETTEINDEXED8) return DD_FMT_P8;
  if (f & DDPF_PALETTEINDEXED4) return DD_FMT_P4;
  if (f & DDPF_PALETTEINDEXED2) return DD_FMT_P2;
  if (f & DDPF_PALETTEINDEXED1) return DD_FMT_P1;
  if (f & DDPF_RGB) {
    const DWORD r = pf->dwRBitMask, g = pf->dwGBitMask, b = pf->dwBBitMask, a = (f & DDPF_ALPHAPIXELS) ? pf->dwRGBAlphaBitMask : 0;
    switch (pf->dwRGBBitCount) {
      case 8:
        if (r == 0xE0 && g == 0x1C && b == 0x03) return DD_FMT_R3G3B2;
        return DD_FMT_UNKNOWN;
      case 16:
        if (r == 0xF800 && g == 0x07E0 && b == 0x001F && !a) return DD_FMT_R5G6B5;
        if (r == 0x7C00 && g == 0x03E0 && b == 0x001F) return a == 0x8000 ? DD_FMT_A1R5G5B5 : a ? DD_FMT_UNKNOWN : DD_FMT_X1R5G5B5;
        if (r == 0x0F00 && g == 0x00F0 && b == 0x000F) return a == 0xF000 ? DD_FMT_A4R4G4B4 : a ? DD_FMT_UNKNOWN : DD_FMT_X4R4G4B4;
        if (r == 0x00E0 && g == 0x001C && b == 0x0003 && a == 0xFF00) return DD_FMT_A8R3G3B2;
        return DD_FMT_UNKNOWN;
      case 24:
        if (r == 0xFF0000 && g == 0xFF00 && b == 0xFF && !a) return DD_FMT_R8G8B8;
        return DD_FMT_UNKNOWN;
      case 32:
        if (r == 0xFF0000 && g == 0xFF00 && b == 0xFF) return a == 0xFF000000u ? DD_FMT_A8R8G8B8 : a ? DD_FMT_UNKNOWN : DD_FMT_X8R8G8B8;
        if (r == 0xFF && g == 0xFF00 && b == 0xFF0000) return a == 0xFF000000u ? DD_FMT_A8B8G8R8 : a ? DD_FMT_UNKNOWN : DD_FMT_X8B8G8R8;
        if (r == 0x3FF00000u && g == 0xFFC00 && b == 0x3FF && a == 0xC0000000u) return DD_FMT_A2R10G10B10;
        if (r == 0x3FF && g == 0xFFC00 && b == 0x3FF00000u && a == 0xC0000000u) return DD_FMT_A2B10G10R10;
        return DD_FMT_UNKNOWN;
      default: return DD_FMT_UNKNOWN;
    }
  }
  if (f & DDPF_ALPHA) return pf->dwAlphaBitDepth == 8 ? DD_FMT_A8 : DD_FMT_UNKNOWN;
  if (f & DDPF_LUMINANCE) {
    if (f & DDPF_ALPHAPIXELS) {
      /* (dwLuminanceBitCount is the luminance's bits or the pixel's: both are seen) */
      if ((pf->dwLuminanceBitCount == 4 || pf->dwLuminanceBitCount == 8) && pf->dwLuminanceBitMask == 0x0F && pf->dwLuminanceAlphaBitMask == 0xF0) return DD_FMT_A4L4;
      if ((pf->dwLuminanceBitCount == 8 || pf->dwLuminanceBitCount == 16) && pf->dwLuminanceBitMask == 0xFF && pf->dwLuminanceAlphaBitMask == 0xFF00) return DD_FMT_A8L8;
      return DD_FMT_UNKNOWN;
    }
    if (pf->dwLuminanceBitCount == 8 && (pf->dwLuminanceBitMask == 0xFF || !pf->dwLuminanceBitMask)) return DD_FMT_L8;
    if (pf->dwLuminanceBitCount == 16 && pf->dwLuminanceBitMask == 0xFFFF) return DD_FMT_L16;
    return DD_FMT_UNKNOWN;
  }
  if (f & DDPF_ZBUFFER) {
    const DWORD z = pf->dwZBitMask, bits = pf->dwZBufferBitDepth;
    if (f & DDPF_STENCILBUFFER) {
      const DWORD s = pf->dwStencilBitMask;
      if (bits == 16 && pf->dwStencilBitDepth == 1 && (z == 0x7FFF || !z) && (s == 0x8000 || !s)) return DD_FMT_D15S1;
      if (bits == 32 && pf->dwStencilBitDepth == 8 && (z == 0x00FFFFFF || !z) && (s == 0xFF000000u || !s)) return DD_FMT_D24S8;
      if (bits == 32 && pf->dwStencilBitDepth == 4 && (z == 0x00FFFFFF || !z) && (s == 0x0F000000u || !s)) return DD_FMT_D24X4S4;
      return DD_FMT_UNKNOWN;
    }
    if (bits == 16 && (z == 0xFFFF || !z)) return DD_FMT_D16;
    if (bits == 24) return DD_FMT_D24X8;
    if (bits == 32) {
      if (z == 0x00FFFFFF) return DD_FMT_D24X8;
      if (z == 0xFFFFFF00u) return DD_FMT_D24X8H;
      if (z == 0xFFFFFFFFu) return DD_FMT_D32;
    }
    return DD_FMT_UNKNOWN;
  }
  if (f & DDPF_FOURCC) {
    for (UINT i = DD_FMT_YUY2; i <= DD_FMT_DXT5; ++i) if (dd_format(i)->fourcc == pf->dwFourCC) return i;
    return DD_FMT_UNKNOWN;
  }
  if (f & DDPF_BUMPDUDV) {
    const DWORD bits = pf->dwBumpBitCount, du = pf->dwBumpDuBitMask, dv = pf->dwBumpDvBitMask, l = pf->dwBumpLuminanceBitMask;
    if (bits == 16 && du == 0xFF && dv == 0xFF00 && !l) return DD_FMT_V8U8;
    if (bits == 16 && du == 0x1F && dv == 0x3E0 && l == 0xFC00) return DD_FMT_L6V5U5;
    if (bits == 32 && du == 0xFF && dv == 0xFF00 && l == 0xFF0000 && !(f & DDPF_ALPHAPIXELS)) return DD_FMT_X8L8V8U8;
    if (bits == 32 && du == 0xFF && dv == 0xFF00 && l == 0xFF0000 && (f & DDPF_ALPHAPIXELS) && pf->dwRGBAlphaBitMask == 0xFF000000u) return DD_FMT_Q8W8V8U8;
    if (bits == 32 && du == 0xFFFF && dv == 0xFFFF0000u && !l) return DD_FMT_V16U16;
    return DD_FMT_UNKNOWN;
  }
  return DD_FMT_UNKNOWN;
}

/* The DDPIXELFORMAT of a format, as Windows reports it (GetPixelFormat, GetSurfaceDesc, GetDisplayMode). */
static void ddpf_from_format(UINT fmt, DDPIXELFORMAT *pf) {
  zero(pf, sizeof *pf);
  pf->dwSize = sizeof *pf;
  const DdFormatInfo *f = dd_format(fmt);
  switch (f->kind) {
    case DD_KIND_PALETTE:
      pf->dwFlags = DDPF_RGB | (fmt == DD_FMT_P8 ? DDPF_PALETTEINDEXED8 : fmt == DD_FMT_P4 ? DDPF_PALETTEINDEXED4 : fmt == DD_FMT_P2 ? DDPF_PALETTEINDEXED2 : DDPF_PALETTEINDEXED1);
      pf->dwRGBBitCount = f->bits;
      break;
    case DD_KIND_RGB:
      pf->dwFlags = DDPF_RGB | (f->a ? DDPF_ALPHAPIXELS : 0);
      pf->dwRGBBitCount = f->bits;
      pf->dwRBitMask = f->r;
      pf->dwGBitMask = f->g;
      pf->dwBBitMask = f->b;
      pf->dwRGBAlphaBitMask = f->a;
      break;
    case DD_KIND_ALPHA:
      pf->dwFlags = DDPF_ALPHA;
      pf->dwAlphaBitDepth = 8;
      break;
    case DD_KIND_LUMINANCE:
      pf->dwFlags = DDPF_LUMINANCE | (f->a ? DDPF_ALPHAPIXELS : 0);
      pf->dwLuminanceBitCount = fmt == DD_FMT_A4L4 ? 4 : f->bits;
      pf->dwLuminanceBitMask = f->r;
      pf->dwLuminanceAlphaBitMask = f->a;
      break;
    case DD_KIND_BUMP:
      pf->dwFlags = DDPF_BUMPDUDV | (f->b ? DDPF_BUMPLUMINANCE : 0) | (fmt == DD_FMT_Q8W8V8U8 ? DDPF_ALPHAPIXELS : 0);
      pf->dwBumpBitCount = f->bits;
      pf->dwBumpDuBitMask = f->r;
      pf->dwBumpDvBitMask = f->g;
      pf->dwBumpLuminanceBitMask = f->b;
      pf->dwRGBAlphaBitMask = f->a;
      break;
    case DD_KIND_DEPTH:
      pf->dwFlags = DDPF_ZBUFFER | (f->g ? DDPF_STENCILBUFFER : 0);
      pf->dwZBufferBitDepth = f->bits;
      pf->dwZBitMask = f->r;
      pf->dwStencilBitMask = f->g;
      pf->dwStencilBitDepth = fmt == DD_FMT_D15S1 ? 1 : fmt == DD_FMT_D24S8 ? 8 : fmt == DD_FMT_D24X4S4 ? 4 : 0;
      break;
    case DD_KIND_YUV_PACKED:
    case DD_KIND_YUV_PLANAR:
      pf->dwFlags = DDPF_FOURCC;
      pf->dwFourCC = f->fourcc;
      pf->dwYUVBitCount = f->kind == DD_KIND_YUV_PACKED ? 16 : 12;
      break;
    case DD_KIND_BLOCKS:
      pf->dwFlags = DDPF_FOURCC;
      pf->dwFourCC = f->fourcc;
      break;
    default:
      break;
  }
}

/* The format of a display mode of `bpp` bits (what a primary surface without a pixel format gets). */
static UINT format_of_display_bpp(DWORD bpp) {
  switch (bpp) {
    case 8: return DD_FMT_P8;
    case 15: return DD_FMT_X1R5G5B5;
    case 16: return DD_FMT_R5G6B5;
    case 24: return DD_FMT_R8G8B8;
    case 32: return DD_FMT_X8R8G8B8;
    default: return DD_FMT_UNKNOWN;
  }
}

static BOOL ddpf_is_palette(const DDPIXELFORMAT *pf) {
  return (pf->dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXED8)) != 0;
}

/* ---- surface descriptions --------------------------------------------------------------------------------------------------------------------------
 * DDSURFACEDESC (IDirectDraw, IDirectDraw2, IDirectDrawSurface .. 3) is DDSURFACEDESC2 up to ddsCaps.dwCaps; what the older one calls
 * dwZBufferBitDepth (DDSD_ZBUFFERBITDEPTH) is a z buffer's pixel format in the newer. ddsCaps is read whether or not DDSD_CAPS is set (as Windows
 * does). */
static void ddsd_to_ddsd2(const DDSURFACEDESC *in, DDSURFACEDESC2 *out) {
  zero(out, sizeof *out);
  out->dwSize = sizeof *out;
  out->dwFlags = in->dwFlags & ~(DWORD)DDSD_ZBUFFERBITDEPTH;
  if (in->dwFlags & DDSD_WIDTH) out->dwWidth = in->dwWidth;
  if (in->dwFlags & DDSD_HEIGHT) out->dwHeight = in->dwHeight;
  if (in->dwFlags & DDSD_PIXELFORMAT) out->ddpfPixelFormat = in->ddpfPixelFormat;
  else if (in->dwFlags & DDSD_ZBUFFERBITDEPTH) {
    out->dwFlags |= DDSD_PIXELFORMAT;
    out->ddpfPixelFormat.dwSize = sizeof out->ddpfPixelFormat;
    out->ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    out->ddpfPixelFormat.dwZBufferBitDepth = in->dwZBufferBitDepth;
    out->ddpfPixelFormat.dwZBitMask = in->dwZBufferBitDepth && in->dwZBufferBitDepth <= 32 ? ~0u >> (32 - in->dwZBufferBitDepth) : 0;
  }
  out->ddsCaps.dwCaps = in->ddsCaps.dwCaps;
  if (in->dwFlags & DDSD_PITCH) out->lPitch = in->lPitch;
  if (in->dwFlags & DDSD_BACKBUFFERCOUNT) out->dwBackBufferCount = in->dwBackBufferCount;
  if (in->dwFlags & DDSD_ALPHABITDEPTH) out->dwAlphaBitDepth = in->dwAlphaBitDepth;
  out->lpSurface = in->lpSurface;
  if (in->dwFlags & DDSD_CKDESTOVERLAY) out->ddckCKDestOverlay = in->ddckCKDestOverlay;
  if (in->dwFlags & DDSD_CKDESTBLT) out->ddckCKDestBlt = in->ddckCKDestBlt;
  if (in->dwFlags & DDSD_CKSRCOVERLAY) out->ddckCKSrcOverlay = in->ddckCKSrcOverlay;
  if (in->dwFlags & DDSD_CKSRCBLT) out->ddckCKSrcBlt = in->ddckCKSrcBlt;
  if (in->dwFlags & DDSD_MIPMAPCOUNT) out->dwMipMapCount = in->dwMipMapCount;
  if (in->dwFlags & DDSD_REFRESHRATE) out->dwRefreshRate = in->dwRefreshRate;
  if (in->dwFlags & DDSD_LINEARSIZE) out->dwLinearSize = in->dwLinearSize;
}
static void ddsd2_to_ddsd(const DDSURFACEDESC2 *in, DDSURFACEDESC *out) {
  zero(out, sizeof *out);
  out->dwSize = sizeof *out;
  out->dwFlags = in->dwFlags & ~(DWORD)(DDSD_TEXTURESTAGE | DDSD_FVF | DDSD_SRCVBHANDLE | DDSD_DEPTH);
  if (in->dwFlags & DDSD_WIDTH) out->dwWidth = in->dwWidth;
  if (in->dwFlags & DDSD_HEIGHT) out->dwHeight = in->dwHeight;
  if (in->dwFlags & DDSD_PIXELFORMAT) {
    out->ddpfPixelFormat = in->ddpfPixelFormat;
    if ((in->dwFlags & DDSD_CAPS) && (in->ddsCaps.dwCaps & DDSCAPS_ZBUFFER)) {
      /* (a z buffer of DirectDraw 1 .. 6 tells its depth with DDSD_ZBUFFERBITDEPTH; its pixel format is there too, without the flag) */
      out->dwFlags &= ~(DWORD)DDSD_PIXELFORMAT;
      out->dwFlags |= DDSD_ZBUFFERBITDEPTH;
      out->dwZBufferBitDepth = in->ddpfPixelFormat.dwZBufferBitDepth;
    }
  }
  out->ddsCaps.dwCaps = in->ddsCaps.dwCaps;
  if (in->dwFlags & DDSD_PITCH) out->lPitch = in->lPitch;
  if (in->dwFlags & DDSD_BACKBUFFERCOUNT) out->dwBackBufferCount = in->dwBackBufferCount;
  if (in->dwFlags & DDSD_ALPHABITDEPTH) out->dwAlphaBitDepth = in->dwAlphaBitDepth;
  out->lpSurface = in->lpSurface;
  if (in->dwFlags & DDSD_CKDESTOVERLAY) out->ddckCKDestOverlay = in->ddckCKDestOverlay;
  if (in->dwFlags & DDSD_CKDESTBLT) out->ddckCKDestBlt = in->ddckCKDestBlt;
  if (in->dwFlags & DDSD_CKSRCOVERLAY) out->ddckCKSrcOverlay = in->ddckCKSrcOverlay;
  if (in->dwFlags & DDSD_CKSRCBLT) out->ddckCKSrcBlt = in->ddckCKSrcBlt;
  if (in->dwFlags & DDSD_MIPMAPCOUNT) out->dwMipMapCount = in->dwMipMapCount;
  if (in->dwFlags & DDSD_REFRESHRATE) out->dwRefreshRate = in->dwRefreshRate;
  if (in->dwFlags & DDSD_LINEARSIZE) out->dwLinearSize = in->dwLinearSize;
}
static void caps_to_caps2(const DDSCAPS *in, DDSCAPS2 *out) { zero(out, sizeof *out); out->dwCaps = in->dwCaps; }

/* The DdImage of a surface's memory (its palette's colours as A8R8G8B8 in `palette`, 256 entries, if it has one). */
static void surface_image(const Surface *s, DdImage *im, uint32_t *palette) {
  zero(im, sizeof *im);
  im->data = s->store.mem;
  im->size = s->store.size;
  im->width = s->desc.dwWidth;
  im->height = s->desc.dwHeight;
  im->pitch = dd_format_is_blocks(s->fmt) ? dd_min_pitch(s->fmt, s->desc.dwWidth) : (uint32_t)s->desc.lPitch;
  im->fmt = s->fmt;
  if (palette && dd_format_is_palette(s->fmt)) {
    const UINT n = 1u << dd_format(s->fmt)->bits;
    for (UINT i = 0; i < 256; ++i) palette[i] = 0xFF000000u;
    if (s->palette)
      for (UINT i = 0; i < n && i < s->palette->count; ++i) {
        const PALETTEENTRY *e = &s->palette->entries[i];
        const DWORD alpha = (s->palette->flags & DDPCAPS_ALPHA) ? e->peFlags : 0xFF;
        palette[i] = (alpha << 24) | ((DWORD)e->peRed << 16) | ((DWORD)e->peGreen << 8) | e->peBlue;
      }
    im->palette = palette;
    im->palette_count = n;
  }
}
