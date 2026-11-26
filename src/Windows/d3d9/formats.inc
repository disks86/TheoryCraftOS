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
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_A8R3G3B2: case D3DFMT_A8P8:
    case D3DFMT_L16:
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
static UINT format_pitch(D3DFORMAT format, UINT w) { return dxt_block(format) ? (w + 3) / 4 * dxt_block(format) : w * format_bytes(format); }
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
/* A palette format: its pixels are numbers of the entries of the current palette. */
static BOOL format_is_palette(D3DFORMAT format) { return format == D3DFMT_P8 || format == D3DFMT_A8P8; }
/* The formats of a target (a back buffer, a render target, a texture drawn into): what the pixel shader's colour can be written as. */
static BOOL format_is_target(D3DFORMAT format) {
  return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 || format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5 ||
         format == D3DFMT_A4R4G4B4 || format == D3DFMT_X4R4G4B4 || format == D3DFMT_A8B8G8R8 || format == D3DFMT_X8B8G8R8;
}
/* The formats of a display mode and of a back buffer. */
static BOOL display_format(D3DFORMAT format) { return format == D3DFMT_X8R8G8B8 || format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5; }
static BOOL backbuffer_format(D3DFORMAT format) {
  return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 || format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5;
}
/* The bits of a display format's pixels. */
static UINT display_bits(D3DFORMAT format) { return format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 ? 32 : 16; }

/* ---- rows of pixels --------------------------------------------------------------------------------------------------------------------------------- */

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
      case D3DFMT_G16R16:      /* (red, green; blue 1 as Direct3D 9 samples it) */
        memcpy(&v, in + 4 * i, 4);
        out[2] = (BYTE)((v & 0xFFFF) >> 8); out[1] = (BYTE)(v >> 24); out[0] = 255; out[3] = 255;
        break;
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
      case D3DFMT_G16R16: v = ((UINT)r * 257u) | ((UINT)g * 257u) << 16; memcpy(out + 4 * i, &v, 4); break;
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
/* A D3DCOLOR (alpha, red, green, blue from the top byte down) as a B8G8R8A8 pixel. */
static void colour_to_bgra(D3DCOLOR colour, BYTE out[4]) { out[0] = (BYTE)colour; out[1] = (BYTE)(colour >> 8); out[2] = (BYTE)(colour >> 16); out[3] = (BYTE)(colour >> 24); }
