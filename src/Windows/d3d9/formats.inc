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

/* Part of d3d9.c: the pixel formats - what a pixel of each is, and how a row of one becomes the rows of the Direct3D 12 texture behind it (B8G8R8A8)
 * and back. P8 and A8P8 go through a palette (the device's current one, SetCurrentTexturePalette). */

/* ---- what a format is ------------------------------------------------------------------------------------------------------------------------------- */

/* The bytes of a pixel of a format a texture or surface may have; 0 for one this runtime has not (and for the compressed and depth formats). */
static UINT format_bytes(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_A32B32G32R32F: return 16;
    case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_A16B16G16R16: return 8;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16:
    case D3DFMT_G16R16F: case D3DFMT_R32F: case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_A2W10V10U10:
      return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_A8R3G3B2: case D3DFMT_A8P8:
    case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_CxV8U8:
      return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A4L4: case D3DFMT_R3G3B2: case D3DFMT_P8: return 1;
    default: return 0;
  }
}
/* The bytes of a 4 x 4 block of a block-compressed format (DXT1 .. DXT5, which Direct3D 12 calls BC1 .. BC3); 0 for the others. Such a texture keeps its blocks: the
 * kernel samples them as they are on a device that has them and decodes them for the others. */
static UINT dxt_block(D3DFORMAT format) { return format == D3DFMT_DXT1 ? 8 : format == D3DFMT_DXT2 || format == D3DFMT_DXT3 || format == D3DFMT_DXT4 || format == D3DFMT_DXT5 ? 16 : 0; }
static DXGI_FORMAT dxt_dxgi(D3DFORMAT format) {
  return format == D3DFMT_DXT1 ? DXGI_FORMAT_BC1_UNORM : format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
}
/* The bytes of a row of a level `w` pixels wide as the program has it (a row of blocks of a compressed format), and the rows of a level `h` high. */
/* The packed YUV formats (YUY2, UYVY: two pixels in four bytes): resources of the scratch pool only, which the device never draws or samples (CheckDeviceFormat refuses them). */
static BOOL format_is_yuv(D3DFORMAT format) { return format == D3DFMT_YUY2 || format == D3DFMT_UYVY; }
static BOOL format_is_depth(D3DFORMAT format);
/* The bytes of a depth buffer's pixel as a lock gives it (the 16-bit formats 2, the others 4). */
static UINT depth_bytes(D3DFORMAT format) { return format == D3DFMT_D16 || format == D3DFMT_D16_LOCKABLE || format == D3DFMT_D15S1 ? 2 : 4; }
static UINT format_pitch(D3DFORMAT format, UINT w) {
  if (format_is_depth(format)) return w * depth_bytes(format);
  return dxt_block(format) ? (w + 3) / 4 * dxt_block(format) : format_is_yuv(format) ? (w + 1) / 2 * 4 : w * format_bytes(format);
}
static UINT format_rows(D3DFORMAT format, UINT h) { return dxt_block(format) ? (h + 3) / 4 : h; }
static BOOL format_is_depth(D3DFORMAT format) {
  return format == D3DFMT_D16 || format == D3DFMT_D24S8 || format == D3DFMT_D24X8 || format == D3DFMT_D32 || format == D3DFMT_D16_LOCKABLE || format == D3DFMT_D24X4S4 ||
         format == D3DFMT_D15S1 || format == D3DFMT_D24FS8 || format == D3DFMT_D32F_LOCKABLE || format == D3DFMT_D32_LOCKABLE;
}
/* A depth format with stencil bits. */
static BOOL format_has_stencil(D3DFORMAT format) { return format == D3DFMT_D24S8 || format == D3DFMT_D24X4S4 || format == D3DFMT_D15S1 || format == D3DFMT_D24FS8; }
/* The depth formats a program may lock (they keep their pixels where the program can read them). */
static BOOL format_depth_lockable(D3DFORMAT format) { return format == D3DFMT_D16_LOCKABLE || format == D3DFMT_D32F_LOCKABLE || format == D3DFMT_D32_LOCKABLE; }
/* The Direct3D 12 format of a depth buffer of a format: the 24-bit ones and those with stencil are D24S8, the others 32-bit float. */
static DXGI_FORMAT depth_dxgi(D3DFORMAT format) {
  return format == D3DFMT_D24S8 || format == D3DFMT_D24X8 || format == D3DFMT_D24X4S4 || format == D3DFMT_D15S1 || format == D3DFMT_D24FS8 ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                                                                                                                         : DXGI_FORMAT_D32_FLOAT;
}
/* The formats whose Direct3D 12 texture is of their own pixels (floats, 16 and 10 bits a channel), not B8G8R8A8: its format, and the bytes of a pixel there. */
static DXGI_FORMAT wide_dxgi(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_R16F: return DXGI_FORMAT_R16_FLOAT;
    case D3DFMT_G16R16F: return DXGI_FORMAT_R16G16_FLOAT;
    case D3DFMT_A16B16G16R16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case D3DFMT_R32F: return DXGI_FORMAT_R32_FLOAT;
    case D3DFMT_G32R32F: return DXGI_FORMAT_R32G32_FLOAT;
    case D3DFMT_A32B32G32R32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DFMT_G16R16: return DXGI_FORMAT_R16G16_UNORM;
    case D3DFMT_A16B16G16R16: return DXGI_FORMAT_R16G16B16A16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
static BOOL format_is_wide(D3DFORMAT format) { return wide_dxgi(format) != DXGI_FORMAT_UNKNOWN; }
/* The signed (bump map) formats: their Direct3D 12 texture is of signed numbers, -1 .. 1 (row_to_signed). V8U8, Q8W8V8U8 and V16U16 keep their pixels as they
 * are (R8G8_SNORM, R8G8B8A8_SNORM, R16G16_SNORM); the ones that mix signed and unsigned channels (X8L8V8U8, L6V5U5, A2W10V10U10: an unsigned luminance or
 * alpha) are R16G16B16A16_SNORM, which holds all of them exactly; CxV8U8 is R8G8B8A8_SNORM with its third number sqrt(1 - u * u - v * v) made at upload.
 * They are textures only (no targets, no levels made: Windows' drivers have neither). */
static DXGI_FORMAT signed_dxgi(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_V8U8: return DXGI_FORMAT_R8G8_SNORM;
    case D3DFMT_Q8W8V8U8: case D3DFMT_CxV8U8: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case D3DFMT_V16U16: return DXGI_FORMAT_R16G16_SNORM;
    case D3DFMT_X8L8V8U8: case D3DFMT_L6V5U5: case D3DFMT_A2W10V10U10: return DXGI_FORMAT_R16G16B16A16_SNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
static BOOL format_is_signed(D3DFORMAT format) { return signed_dxgi(format) != DXGI_FORMAT_UNKNOWN; }
/* The format of the Direct3D 12 texture of a colour format, and the bytes of a pixel of it. */
static DXGI_FORMAT gpu_dxgi(D3DFORMAT format) {
  return format_is_wide(format) ? wide_dxgi(format) : format_is_signed(format) ? signed_dxgi(format) : dxt_block(format) ? dxt_dxgi(format) : DXGI_FORMAT_B8G8R8A8_UNORM;
}
static UINT gpu_bytes(D3DFORMAT format) {
  if (format_is_wide(format)) return format_bytes(format);
  if (format_is_signed(format)) return signed_dxgi(format) == DXGI_FORMAT_R16G16B16A16_SNORM ? 8 : format_bytes(format) == 2 && format != D3DFMT_CxV8U8 ? 2 : 4;
  return 4;
}
/* What a sampler of Direct3D 9 reads of a format's missing channels and Direct3D 12 does not: 1 where Direct3D 9 has it and Direct3D 12 0 (x y z w; R16F and R32F are
 * (r, 1, 1, 1), the two-channel ones (r, g, 1, 1)). */
static void format_fill(D3DFORMAT format, float fill[4]) {
  fill[0] = fill[1] = fill[2] = fill[3] = 0.0f;
  if (format == D3DFMT_R16F || format == D3DFMT_R32F) fill[1] = fill[2] = 1.0f;
  else if (format == D3DFMT_G16R16F || format == D3DFMT_G32R32F || format == D3DFMT_G16R16 || format == D3DFMT_V8U8 || format == D3DFMT_V16U16) fill[2] = 1.0f;
}
/* A palette format: its pixels are numbers of the entries of the current palette. */
static BOOL format_is_palette(D3DFORMAT format) { return format == D3DFMT_P8 || format == D3DFMT_A8P8; }
/* The formats of a target (a back buffer, a render target, a texture drawn into): what the pixel shader's colour can be written as. */
static BOOL format_is_target(D3DFORMAT format) {
  return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 || format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5 ||
         format == D3DFMT_A4R4G4B4 || format == D3DFMT_X4R4G4B4 || format == D3DFMT_A8B8G8R8 || format == D3DFMT_X8B8G8R8 || format_is_wide(format);
}
/* The formats of a display mode and of a back buffer. */
static BOOL display_format(D3DFORMAT format) { return format == D3DFMT_X8R8G8B8 || format == D3DFMT_R5G6B5; }
static BOOL backbuffer_format(D3DFORMAT format) {
  return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 || format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5;
}
/* The bits of a display format's pixels. */
static UINT display_bits(D3DFORMAT format) { return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 ? 32 : 16; }

/* ---- rows of pixels --------------------------------------------------------------------------------------------------------------------------------- */

static void wide_to_float(D3DFORMAT format, const BYTE *p, float out[4]);
static void float_to_wide(D3DFORMAT format, const float in[4], BYTE *p);
static UINT unorm_bits(float f, UINT max);

static BYTE scale_bits(UINT value, UINT bits) { return (BYTE)(bits >= 8 ? value >> (bits - 8) : value * 255u / ((1u << bits) - 1u)); }
static UINT to_bits(BYTE value, UINT bits) { return bits >= 8 ? (UINT)value << (bits - 8) : ((UINT)value * ((1u << bits) - 1u) + 127u) / 255u; }

/* A row of pixels of `format` as B8G8R8A8, the format of the Direct3D 12 textures. `palette`: the 256 entries P8 and A8P8 read (NULL: no palette was
 * ever set, whose entries Direct3D 9 leaves undefined; they read as opaque white). */
static void row_to_bgra(D3DFORMAT format, const BYTE *in, UINT pixels, BYTE *out, const PALETTEENTRY *palette) {
  for (UINT i = 0; i < pixels; ++i, out += 4) {
    UINT v;
    switch (format) {
      case D3DFMT_A8R8G8B8: memcpy(out, in + 4 * i, 4); break;
      case D3DFMT_X8R8G8B8: memcpy(out, in + 4 * i, 3); out[3] = 255; break;
      case D3DFMT_A8B8G8R8: out[0] = in[4 * i + 2]; out[1] = in[4 * i + 1]; out[2] = in[4 * i]; out[3] = in[4 * i + 3]; break;
      case D3DFMT_X8B8G8R8: out[0] = in[4 * i + 2]; out[1] = in[4 * i + 1]; out[2] = in[4 * i]; out[3] = 255; break;
      case D3DFMT_R8G8B8: memcpy(out, in + 3 * i, 3); out[3] = 255; break;
      case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: {
        memcpy(&v, in + 4 * i, 4);
        const BYTE first = scale_bits(v & 1023, 10), second = scale_bits((v >> 10) & 1023, 10), third = scale_bits((v >> 20) & 1023, 10);
        out[0] = format == D3DFMT_A2R10G10B10 ? first : third;
        out[1] = second;
        out[2] = format == D3DFMT_A2R10G10B10 ? third : first;
        out[3] = scale_bits(v >> 30, 2);
        break;
      }
      case D3DFMT_G16R16: case D3DFMT_R16F: case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: case D3DFMT_R32F: case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F:
      case D3DFMT_A16B16G16R16: {      /* (through four floats, clamped to 0 .. 1) */
        float f[4];
        wide_to_float(format, in + (SIZE_T)i * format_bytes(format), f);
        out[2] = (BYTE)unorm_bits(f[0], 255); out[1] = (BYTE)unorm_bits(f[1], 255); out[0] = (BYTE)unorm_bits(f[2], 255); out[3] = (BYTE)unorm_bits(f[3], 255);
        break;
      }
      case D3DFMT_R5G6B5:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = scale_bits(v & 31, 5); out[1] = scale_bits((v >> 5) & 63, 6); out[2] = scale_bits(v >> 11, 5); out[3] = 255;
        break;
      case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = scale_bits(v & 31, 5); out[1] = scale_bits((v >> 5) & 31, 5); out[2] = scale_bits((v >> 10) & 31, 5);
        out[3] = format == D3DFMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
        break;
      case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = (BYTE)((v & 15) * 17); out[1] = (BYTE)(((v >> 4) & 15) * 17); out[2] = (BYTE)(((v >> 8) & 15) * 17);
        out[3] = format == D3DFMT_X4R4G4B4 ? 255 : (BYTE)((v >> 12) * 17);
        break;
      case D3DFMT_A8R3G3B2:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = scale_bits(v & 3, 2); out[1] = scale_bits((v >> 2) & 7, 3); out[2] = scale_bits((v >> 5) & 7, 3); out[3] = (BYTE)(v >> 8);
        break;
      case D3DFMT_R3G3B2:
        v = in[i];
        out[0] = scale_bits(v & 3, 2); out[1] = scale_bits((v >> 2) & 7, 3); out[2] = scale_bits((v >> 5) & 7, 3); out[3] = 255;
        break;
      case D3DFMT_A8L8: out[0] = out[1] = out[2] = in[2 * i]; out[3] = in[2 * i + 1]; break;
      case D3DFMT_A4L4: out[0] = out[1] = out[2] = (BYTE)((in[i] & 15) * 17); out[3] = (BYTE)((in[i] >> 4) * 17); break;
      case D3DFMT_L8: out[0] = out[1] = out[2] = in[i]; out[3] = 255; break;
      case D3DFMT_L16: out[0] = out[1] = out[2] = in[2 * i + 1]; out[3] = 255; break;
      case D3DFMT_A8: out[0] = out[1] = out[2] = 0; out[3] = in[i]; break;
      case D3DFMT_P8: case D3DFMT_A8P8: {
        const BYTE index = format == D3DFMT_P8 ? in[i] : in[2 * i];
        if (palette) {
          out[0] = palette[index].peBlue; out[1] = palette[index].peGreen; out[2] = palette[index].peRed;
          out[3] = format == D3DFMT_A8P8 ? in[2 * i + 1] : palette[index].peFlags;
        } else out[0] = out[1] = out[2] = out[3] = 255;
        break;
      }
      default: out[0] = out[1] = out[2] = out[3] = 255; break;
    }
  }
}
/* A row of B8G8R8A8 pixels as `format` (what a target's pixels are when the program locks it, or a copy writes into memory of another format). P8 and
 * A8P8 never come this way: they are not targets, and a copy into them from another format is refused before. */
static void row_from_bgra(D3DFORMAT format, const BYTE *in, UINT pixels, BYTE *out) {
  for (UINT i = 0; i < pixels; ++i, in += 4) {
    const BYTE b = in[0], g = in[1], r = in[2], a = in[3];
    UINT v;
    switch (format) {
      case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: memcpy(out + 4 * i, in, 4); if (format == D3DFMT_X8R8G8B8) out[4 * i + 3] = 255; break;
      case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: out[4 * i] = r; out[4 * i + 1] = g; out[4 * i + 2] = b; out[4 * i + 3] = format == D3DFMT_X8B8G8R8 ? 255 : a; break;
      case D3DFMT_R8G8B8: out[3 * i] = b; out[3 * i + 1] = g; out[3 * i + 2] = r; break;
      case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: {
        const UINT first = to_bits(format == D3DFMT_A2R10G10B10 ? b : r, 10), third = to_bits(format == D3DFMT_A2R10G10B10 ? r : b, 10);
        v = first | to_bits(g, 10) << 10 | third << 20 | to_bits(a, 2) << 30;
        memcpy(out + 4 * i, &v, 4);
        break;
      }
      case D3DFMT_G16R16: case D3DFMT_R16F: case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: case D3DFMT_R32F: case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F:
      case D3DFMT_A16B16G16R16: {
        const float f[4] = {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        float_to_wide(format, f, out + (SIZE_T)i * format_bytes(format));
        break;
      }
      case D3DFMT_R5G6B5: v = to_bits(b, 5) | to_bits(g, 6) << 5 | to_bits(r, 5) << 11; goto two;
      case D3DFMT_X1R5G5B5: v = to_bits(b, 5) | to_bits(g, 5) << 5 | to_bits(r, 5) << 10 | 0x8000u; goto two;
      case D3DFMT_A1R5G5B5: v = to_bits(b, 5) | to_bits(g, 5) << 5 | to_bits(r, 5) << 10 | (a >= 128 ? 0x8000u : 0u); goto two;
      case D3DFMT_A4R4G4B4: v = to_bits(b, 4) | to_bits(g, 4) << 4 | to_bits(r, 4) << 8 | to_bits(a, 4) << 12; goto two;
      case D3DFMT_X4R4G4B4: v = to_bits(b, 4) | to_bits(g, 4) << 4 | to_bits(r, 4) << 8 | 0xF000u; goto two;
      case D3DFMT_A8R3G3B2: v = to_bits(b, 2) | to_bits(g, 3) << 2 | to_bits(r, 3) << 5 | (UINT)a << 8; goto two;
      case D3DFMT_A8L8: v = r | (UINT)a << 8; goto two;
      case D3DFMT_L16: v = (UINT)r * 257u; goto two;
      two:
        out[2 * i] = (BYTE)v; out[2 * i + 1] = (BYTE)(v >> 8);
        break;
      case D3DFMT_R3G3B2: out[i] = (BYTE)(to_bits(b, 2) | to_bits(g, 3) << 2 | to_bits(r, 3) << 5); break;
      case D3DFMT_A4L4: out[i] = (BYTE)(to_bits(r, 4) | to_bits(a, 4) << 4); break;
      case D3DFMT_L8: out[i] = r; break;
      case D3DFMT_A8: out[i] = a; break;
      default: break;
    }
  }
}
/* ---- the wide formats ------------------------------------------------------------------------------------------------------------------------------- */

static float half_float(UINT16 h) {
  const UINT e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = (float)m / 1024.0f / 16384.0f;
  else if (e == 31) v = m ? 0.0f : 65504.0f * 2.0f;      /* (infinity as a large number; NaN as 0: they are clamped where they are used as colours) */
  else {
    v = 1.0f + (float)m / 1024.0f;
    for (UINT k = e; k > 15; --k) v *= 2.0f;
    for (UINT k = e; k < 15; ++k) v *= 0.5f;
  }
  return (h & 0x8000) ? -v : v;
}
static UINT16 float_half(float f) {
  UINT bits;
  memcpy(&bits, &f, 4);
  const UINT sign = (bits >> 16) & 0x8000;
  const int e = (int)((bits >> 23) & 255) - 127 + 15;
  const UINT m = bits & 0x7FFFFF;
  if (((bits >> 23) & 255) == 255) return (UINT16)(sign | 0x7C00 | (m ? 0x200 : 0));
  if (e >= 31) return (UINT16)(sign | 0x7C00);
  if (e <= 0) {
    if (e < -10) return (UINT16)sign;
    const UINT full = m | 0x800000;
    return (UINT16)(sign | ((full >> (14 - e)) + ((full >> (13 - e)) & 1)));
  }
  UINT h = sign | ((UINT)e << 10) | (m >> 13);
  if (m & 0x1000) ++h;      /* (rounded to the nearest) */
  return (UINT16)h;
}
/* A pixel of a wide format (as the program has it) as four floats, and four floats as one. Missing channels read as Direct3D 9 has them: green and blue 1, alpha 1. */
static void wide_to_float(D3DFORMAT format, const BYTE *p, float out[4]) {
  UINT16 h[4];
  float f[4];
  UINT v;
  out[0] = 0.0f; out[1] = out[2] = out[3] = 1.0f;
  switch (format) {
    case D3DFMT_R16F: memcpy(h, p, 2); out[0] = half_float(h[0]); break;
    case D3DFMT_G16R16F: memcpy(h, p, 4); out[0] = half_float(h[0]); out[1] = half_float(h[1]); break;
    case D3DFMT_A16B16G16R16F: memcpy(h, p, 8); for (int i = 0; i < 4; ++i) out[i] = half_float(h[i]); break;
    case D3DFMT_R32F: memcpy(f, p, 4); out[0] = f[0]; break;
    case D3DFMT_G32R32F: memcpy(f, p, 8); out[0] = f[0]; out[1] = f[1]; break;
    case D3DFMT_A32B32G32R32F: memcpy(out, p, 16); break;
    case D3DFMT_G16R16: memcpy(h, p, 4); out[0] = h[0] / 65535.0f; out[1] = h[1] / 65535.0f; break;
    case D3DFMT_A16B16G16R16: memcpy(h, p, 8); for (int i = 0; i < 4; ++i) out[i] = h[i] / 65535.0f; break;
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: {
      memcpy(&v, p, 4);
      const float low = (v & 1023) / 1023.0f, mid = ((v >> 10) & 1023) / 1023.0f, high = ((v >> 20) & 1023) / 1023.0f;
      out[0] = format == D3DFMT_A2B10G10R10 ? low : high;
      out[1] = mid;
      out[2] = format == D3DFMT_A2B10G10R10 ? high : low;
      out[3] = (v >> 30) / 3.0f;
      break;
    }
    default: break;
  }
}
static UINT unorm_bits(float f, UINT max) { return f != f || f <= 0.0f ? 0u : f >= 1.0f ? max : (UINT)(f * (float)max + 0.5f); }
static void float_to_wide(D3DFORMAT format, const float in[4], BYTE *p) {
  UINT16 h[4];
  UINT v;
  switch (format) {
    case D3DFMT_R16F: h[0] = float_half(in[0]); memcpy(p, h, 2); break;
    case D3DFMT_G16R16F: h[0] = float_half(in[0]); h[1] = float_half(in[1]); memcpy(p, h, 4); break;
    case D3DFMT_A16B16G16R16F: for (int i = 0; i < 4; ++i) h[i] = float_half(in[i]); memcpy(p, h, 8); break;
    case D3DFMT_R32F: memcpy(p, in, 4); break;
    case D3DFMT_G32R32F: memcpy(p, in, 8); break;
    case D3DFMT_A32B32G32R32F: memcpy(p, in, 16); break;
    case D3DFMT_G16R16: h[0] = (UINT16)unorm_bits(in[0], 65535); h[1] = (UINT16)unorm_bits(in[1], 65535); memcpy(p, h, 4); break;
    case D3DFMT_A16B16G16R16: for (int i = 0; i < 4; ++i) h[i] = (UINT16)unorm_bits(in[i], 65535); memcpy(p, h, 8); break;
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: {
      const UINT r = unorm_bits(in[0], 1023), g = unorm_bits(in[1], 1023), b = unorm_bits(in[2], 1023), a = unorm_bits(in[3], 3);
      v = format == D3DFMT_A2B10G10R10 ? (r | g << 10 | b << 20 | a << 30) : (b | g << 10 | r << 20 | a << 30);
      memcpy(p, &v, 4);
      break;
    }
    default: break;
  }
}
/* A row of a wide format as its Direct3D 12 texture has it, and back: the pixels as they are but for A2R10G10B10, whose red and blue change places (R10G10B10A2 has
 * red in the low bits). */
static void row_to_gpu(D3DFORMAT format, const BYTE *in, UINT pixels, BYTE *out) {
  if (format != D3DFMT_A2R10G10B10) { memcpy(out, in, (SIZE_T)pixels * format_bytes(format)); return; }
  for (UINT i = 0; i < pixels; ++i) {
    UINT v;
    memcpy(&v, in + 4 * i, 4);
    v = (v & 0xC00FFC00u) | (v & 1023u) << 20 | ((v >> 20) & 1023u);
    memcpy(out + 4 * i, &v, 4);
  }
}
static void row_from_gpu(D3DFORMAT format, const BYTE *in, UINT pixels, BYTE *out) { row_to_gpu(format, in, pixels, out); }      /* (the swap is its own inverse) */

/* ---- the signed formats ----------------------------------------------------------------------------------------------------------------------------- */

/* A signed number of `bits` bits (the low bits of `v`) as a 16-bit signed normalized code: the lowest code is -1 as the one above it (Direct3D's rule). */
static INT16 snorm16_of(UINT v, UINT bits) {
  const int top = (1 << (bits - 1)) - 1;
  int s = (int)(v & ((1u << bits) - 1u));
  if (s > top) s -= 1 << bits;
  if (s < -top) s = -top;
  return (INT16)((s * 32767 + (s < 0 ? -top / 2 : top / 2)) / top);
}
/* An unsigned number of `bits` bits as a 16-bit signed normalized code of 0 .. 1. */
static INT16 snorm16_unsigned(UINT v, UINT bits) {
  const UINT top = (1u << bits) - 1u;
  return (INT16)(((v & top) * 32767u + top / 2u) / top);
}
/* The integer square root of a number up to 127 * 127 * 2, rounded down. */
static UINT isqrt(UINT v) {
  UINT r = 0;
  while ((r + 1) * (r + 1) <= v) ++r;
  return r;
}
/* A row of a signed format as its Direct3D 12 texture has it (signed_dxgi), `gpu_bytes` a pixel. */
static void row_to_signed(D3DFORMAT format, const BYTE *in, UINT pixels, BYTE *out) {
  for (UINT i = 0; i < pixels; ++i) {
    UINT v = 0;
    INT16 w[4];
    switch (format) {
      case D3DFMT_V8U8: memcpy(out + 2 * i, in + 2 * i, 2); break;
      case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: memcpy(out + 4 * i, in + 4 * i, 4); break;
      case D3DFMT_CxV8U8: {      /* (u, v and the third number of the unit vector they begin) */
        const signed char u = (signed char)in[2 * i], vv = (signed char)in[2 * i + 1];
        const int cu = u < -127 ? -127 : u, cv = vv < -127 ? -127 : vv, rest = 127 * 127 - cu * cu - cv * cv;
        out[4 * i] = (BYTE)u;
        out[4 * i + 1] = (BYTE)vv;
        out[4 * i + 2] = (BYTE)(rest > 0 ? isqrt((UINT)rest) : 0u);
        out[4 * i + 3] = 127;
        break;
      }
      case D3DFMT_X8L8V8U8:
        memcpy(&v, in + 4 * i, 4);
        w[0] = snorm16_of(v, 8); w[1] = snorm16_of(v >> 8, 8); w[2] = snorm16_unsigned(v >> 16, 8); w[3] = 32767;
        memcpy(out + 8 * i, w, 8);
        break;
      case D3DFMT_L6V5U5:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        w[0] = snorm16_of(v, 5); w[1] = snorm16_of(v >> 5, 5); w[2] = snorm16_unsigned(v >> 10, 6); w[3] = 32767;
        memcpy(out + 8 * i, w, 8);
        break;
      case D3DFMT_A2W10V10U10:
        memcpy(&v, in + 4 * i, 4);
        w[0] = snorm16_of(v, 10); w[1] = snorm16_of(v >> 10, 10); w[2] = snorm16_of(v >> 20, 10); w[3] = snorm16_unsigned(v >> 30, 2);
        memcpy(out + 8 * i, w, 8);
        break;
      default: break;
    }
  }
}

/* A D3DCOLOR (alpha, red, green, blue from the top byte down) as a B8G8R8A8 pixel. */
static void colour_to_bgra(D3DCOLOR colour, BYTE out[4]) { out[0] = (BYTE)colour; out[1] = (BYTE)(colour >> 8); out[2] = (BYTE)(colour >> 16); out[3] = (BYTE)(colour >> 24); }
