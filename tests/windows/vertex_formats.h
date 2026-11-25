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

/* The vertex element formats of Direct3D 10 and later (D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER) with a value of each and the colour the vs_fmt shaders
 * (tests/data/dxbc/vs_fmt*.hlsl) draw it as: for d3d11test.c and d3d12test.c. */
#pragma once

/* One vertex element format and what the vs_fmt* shaders show of it: the element's bytes (16 at most), the shader, and the colour expected (red, green, blue, alpha). */
typedef struct { DXGI_FORMAT format; BYTE bytes[16]; int shader; int rgba[4]; const char *name; } VertexFormatCase;
#define VF_FLOAT 0
#define VF_UINT 1
#define VF_SINT 2
/* (the little-endian bytes of 32-bit and 16-bit values) */
#define W32(v) (BYTE)((v) & 255), (BYTE)(((v) >> 8) & 255), (BYTE)(((v) >> 16) & 255), (BYTE)(((unsigned)(v) >> 24) & 255)
#define W16(v) (BYTE)((v) & 255), (BYTE)(((v) >> 8) & 255)
/* (float bits: 0.5 0x3F000000, -0.5 0xBF000000, 1 0x3F800000, -1 0xBF800000) */
static const VertexFormatCase g_vertex_formats[] = {
  {DXGI_FORMAT_R32G32B32A32_FLOAT, {W32(0x3F000000u), W32(0xBF000000u), W32(0x3F800000u), W32(0)}, VF_FLOAT, {191, 64, 255, 128}, "R32G32B32A32_FLOAT"},
  {DXGI_FORMAT_R32G32B32_FLOAT, {W32(0x3F000000u), W32(0xBF000000u), W32(0x3F800000u)}, VF_FLOAT, {191, 64, 255, 255}, "R32G32B32_FLOAT"},
  {DXGI_FORMAT_R32G32_FLOAT, {W32(0x3F000000u), W32(0xBF800000u)}, VF_FLOAT, {191, 0, 128, 255}, "R32G32_FLOAT"},
  {DXGI_FORMAT_R32_FLOAT, {W32(0xBF000000u)}, VF_FLOAT, {64, 128, 128, 255}, "R32_FLOAT"},
  {DXGI_FORMAT_R16G16B16A16_FLOAT, {W16(0x3C00), W16(0xBC00), W16(0x3800), W16(0)}, VF_FLOAT, {255, 0, 191, 128}, "R16G16B16A16_FLOAT"},
  {DXGI_FORMAT_R16G16_FLOAT, {W16(0xB800), W16(0x3C00)}, VF_FLOAT, {64, 255, 128, 255}, "R16G16_FLOAT"},
  {DXGI_FORMAT_R16_FLOAT, {W16(0x3800)}, VF_FLOAT, {191, 128, 128, 255}, "R16_FLOAT"},
  {DXGI_FORMAT_R16G16B16A16_UNORM, {W16(65535), W16(0), W16(32768), W16(16384)}, VF_FLOAT, {255, 128, 191, 159}, "R16G16B16A16_UNORM"},
  {DXGI_FORMAT_R16G16_UNORM, {W16(0), W16(65535)}, VF_FLOAT, {128, 255, 128, 255}, "R16G16_UNORM"},
  {DXGI_FORMAT_R16_UNORM, {W16(65535)}, VF_FLOAT, {255, 128, 128, 255}, "R16_UNORM"},
  {DXGI_FORMAT_R16G16B16A16_SNORM, {W16(32767), W16(0x8000), W16(0), W16(16384)}, VF_FLOAT, {255, 0, 128, 191}, "R16G16B16A16_SNORM"},
  {DXGI_FORMAT_R16G16_SNORM, {W16(0x8001), W16(32767)}, VF_FLOAT, {0, 255, 128, 255}, "R16G16_SNORM"},
  {DXGI_FORMAT_R16_SNORM, {W16(0xC000)}, VF_FLOAT, {64, 128, 128, 255}, "R16_SNORM"},
  {DXGI_FORMAT_R8G8B8A8_UNORM, {255, 0, 128, 64}, VF_FLOAT, {255, 128, 192, 160}, "R8G8B8A8_UNORM"},
  {DXGI_FORMAT_R8G8_UNORM, {0, 255}, VF_FLOAT, {128, 255, 128, 255}, "R8G8_UNORM"},
  {DXGI_FORMAT_R8_UNORM, {255}, VF_FLOAT, {255, 128, 128, 255}, "R8_UNORM"},
  {DXGI_FORMAT_R8G8B8A8_SNORM, {127, 0x80, 0, 64}, VF_FLOAT, {255, 0, 128, 192}, "R8G8B8A8_SNORM (-128 is -1)"},
  {DXGI_FORMAT_R8G8_SNORM, {0x81, 127}, VF_FLOAT, {0, 255, 128, 255}, "R8G8_SNORM"},
  {DXGI_FORMAT_R8_SNORM, {0xC0}, VF_FLOAT, {64, 128, 128, 255}, "R8_SNORM"},
  {DXGI_FORMAT_R10G10B10A2_UNORM, {W32(1023u | 0u << 10 | 512u << 20 | 1u << 30)}, VF_FLOAT, {255, 128, 191, 170}, "R10G10B10A2_UNORM"},
  {DXGI_FORMAT_R11G11B10_FLOAT, {W32((15u << 6) | (14u << 6) << 11 | 0u << 22)}, VF_FLOAT, {255, 191, 128, 255}, "R11G11B10_FLOAT"},
  {DXGI_FORMAT_B8G8R8A8_UNORM, {0, 128, 255, 0}, VF_FLOAT, {255, 192, 128, 128}, "B8G8R8A8_UNORM"},
  {DXGI_FORMAT_B8G8R8X8_UNORM, {0, 128, 255, 77}, VF_FLOAT, {255, 192, 128, 255}, "B8G8R8X8_UNORM (the fourth byte is not alpha)"},
  {DXGI_FORMAT_R32G32B32A32_UINT, {W32(1023), W32(0), W32(512), W32(3)}, VF_UINT, {255, 0, 128, 255}, "R32G32B32A32_UINT"},
  {DXGI_FORMAT_R32G32B32_UINT, {W32(0), W32(1023), W32(0)}, VF_UINT, {0, 255, 0, 85}, "R32G32B32_UINT (w is the integer 1)"},
  {DXGI_FORMAT_R32G32_UINT, {W32(512), W32(1023)}, VF_UINT, {128, 255, 0, 85}, "R32G32_UINT"},
  {DXGI_FORMAT_R32_UINT, {W32(1023)}, VF_UINT, {255, 0, 0, 85}, "R32_UINT"},
  {DXGI_FORMAT_R16G16B16A16_UINT, {W16(1023), W16(256), W16(0), W16(0)}, VF_UINT, {255, 64, 0, 0}, "R16G16B16A16_UINT"},
  {DXGI_FORMAT_R16G16_UINT, {W16(256), W16(1023)}, VF_UINT, {64, 255, 0, 85}, "R16G16_UINT"},
  {DXGI_FORMAT_R16_UINT, {W16(512)}, VF_UINT, {128, 0, 0, 85}, "R16_UINT"},
  {DXGI_FORMAT_R8G8B8A8_UINT, {255, 128, 0, 3}, VF_UINT, {64, 32, 0, 255}, "R8G8B8A8_UINT"},
  {DXGI_FORMAT_R8G8_UINT, {128, 255}, VF_UINT, {32, 64, 0, 85}, "R8G8_UINT"},
  {DXGI_FORMAT_R8_UINT, {255}, VF_UINT, {64, 0, 0, 85}, "R8_UINT"},
  {DXGI_FORMAT_R10G10B10A2_UINT, {W32(1023u | 512u << 10 | 0u << 20 | 2u << 30)}, VF_UINT, {255, 128, 0, 170}, "R10G10B10A2_UINT"},
  {DXGI_FORMAT_R32G32B32A32_SINT, {W32(64), W32(-64), W32(0), W32(32)}, VF_SINT, {191, 64, 128, 160}, "R32G32B32A32_SINT"},
  {DXGI_FORMAT_R32G32B32_SINT, {W32(-128), W32(64), W32(0)}, VF_SINT, {0, 191, 128, 129}, "R32G32B32_SINT"},
  {DXGI_FORMAT_R32G32_SINT, {W32(127), W32(-64)}, VF_SINT, {255, 64, 128, 129}, "R32G32_SINT"},
  {DXGI_FORMAT_R32_SINT, {W32(-64)}, VF_SINT, {64, 128, 128, 129}, "R32_SINT"},
  {DXGI_FORMAT_R16G16B16A16_SINT, {W16(64), W16(0xFFC0), W16(0), W16(32)}, VF_SINT, {191, 64, 128, 160}, "R16G16B16A16_SINT (sign extended)"},
  {DXGI_FORMAT_R16G16_SINT, {W16(0xFF80), W16(64)}, VF_SINT, {0, 191, 128, 129}, "R16G16_SINT"},
  {DXGI_FORMAT_R16_SINT, {W16(0xFFC0)}, VF_SINT, {64, 128, 128, 129}, "R16_SINT"},
  {DXGI_FORMAT_R8G8B8A8_SINT, {64, 0xC0, 0, 32}, VF_SINT, {191, 64, 128, 160}, "R8G8B8A8_SINT"},
  {DXGI_FORMAT_R8G8_SINT, {0x80, 64}, VF_SINT, {0, 191, 128, 129}, "R8G8_SINT"},
  {DXGI_FORMAT_R8_SINT, {0xC0}, VF_SINT, {64, 128, 128, 129}, "R8_SINT"},
};
/* Whether a pixel (0xAARRGGBB) is the colour, give or take 3 in each channel. */
static int vf_near8(int a, int b) { return a - b <= 3 && b - a <= 3; }
static int colour_is(UINT32 p, const int rgba[4]) {
  return vf_near8((int)((p >> 16) & 255), rgba[0]) && vf_near8((int)((p >> 8) & 255), rgba[1]) && vf_near8((int)(p & 255), rgba[2]) && vf_near8((int)(p >> 24), rgba[3]);
}
