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

/* The parts of Direct3D 1, 2, 3 and 7 (ddraw.dll's IDirect3D* and IDirect3DDevice*) that are logic rather than COM objects, for ddraw.dll to
 * build its devices on d3d9.dll with (docs/Direct3DLegacy.md). Plain C that is also C++, with no library and no windows.h: the structures are
 * defined here with the layouts of d3dtypes.h and d3dcaps.h (on i386 and x86-64 alike; tests/data/ddraw/d3d_legacy_layout.c holds them against
 * mingw's headers with both compilers, tests/D3dLegacyTest.cpp tests the rest on the host under ASan and UBSan).
 *
 *   execute buffers   D3DEXECUTEDATA checks and an executor that walks a D3DINSTRUCTION stream one command at a time, bounds-checked, with
 *                     BRANCHFORWARD, SETSTATUS and EXIT done inside it (they only touch the execute buffer's D3DSTATUS)
 *   vertices          the transform of Direct3D 1-3 (world, view, projection and the viewport's clip space), clip codes, screen mapping, extents and
 *                     clip status, the lighting of Direct3D 1-3 (and of 7) on the CPU, vertex fog, a homogeneous clipper, Viewport::TransformVertices
 *   caps              D3DDEVICEDESC (the three sizes) and D3DDEVICEDESC7 from Direct3D 9's caps, the devices each version enumerates, FindDevice
 *   states            legacy render, light, texture stage and transform states as Direct3D 9 render, texture stage and sampler states; defaults
 *   conversions       lights, materials, viewports, FVF codes (validation, strides, Direct3D 9 vertex elements), D3DVERTEXTYPE, strided data,
 *                     primitive counts, ComputeSphereVisibility
 *
 * Every number from a program is checked before it is used: buffers come in as views (DlConstBytes / DlBytes), and a function that reads one
 * refuses what does not fit in it. */
#ifndef TCOS_D3D_LEGACY_H
#define TCOS_D3D_LEGACY_H

#include <stddef.h>
#include <stdint.h>
/* The square root, the power and the cosine of d3d9's fixed-function lighting (written out: the DLLs have no C runtime). Its own lighting and texture
 * coordinate functions are not used here (Direct3D 1-3 light differently), so they are not warned about. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../d3d9/ff_lighting.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define DL_API static inline

/* ==== results ================================================================================================================================ */

typedef int32_t DlResult;      /* an HRESULT */
#define DL_HR(x) ((DlResult)(int64_t)(x))
#define DL_OK 0
#define DL_DDERR_INVALIDPARAMS DL_HR(0x80070057)     /* E_INVALIDARG */
#define DL_DDERR_UNSUPPORTED DL_HR(0x80004001)       /* E_NOTIMPL */
#define DL_DDERR_NOTFOUND DL_HR(0x887600FF)          /* MAKE_DDHRESULT(255) */
#define DL_D3DERR_EXECUTE_FAILED DL_HR(0x887602CC)   /* MAKE_DDHRESULT(716) */
#define DL_D3DERR_INVALIDVERTEXTYPE DL_HR(0x887602E1)    /* 737 */
#define DL_D3DERR_INVALIDPRIMITIVETYPE DL_HR(0x887602E0) /* 736 */
#define DL_D3DERR_VIEWPORTDATANOTSET DL_HR(0x88760305)   /* 773 */
#define DL_D3DERR_INVALIDVERTEXFORMAT DL_HR(0x88760800)  /* 2048 */

/* ==== views ================================================================================================================================== */

/* A run of bytes this code may read, and one it may write. Built once at the ABI boundary (the program's pointer and the size it says it has);
 * everything below reads through dl_read* and writes through dl_write*, which refuse what is outside. */
typedef struct DlConstBytes { const uint8_t *data; size_t size; } DlConstBytes;
typedef struct DlBytes { uint8_t *data; size_t size; } DlBytes;

DL_API DlConstBytes dl_const_bytes(const void *data, size_t size) {
  DlConstBytes v;
  v.data = (const uint8_t *)data;      /* (the one place a program's pointer becomes a view) */
  v.size = data ? size : 0;
  return v;
}
DL_API DlBytes dl_bytes(void *data, size_t size) {
  DlBytes v;
  v.data = (uint8_t *)data;
  v.size = data ? size : 0;
  return v;
}
/* `count` bytes from `offset` lie within `size` (without the sum wrapping). */
DL_API int dl_fits(size_t size, size_t offset, size_t count) { return offset <= size && count <= size - offset; }
/* Copies `count` bytes into `to` from `from`; 0 (and nothing copied) if either side is too short. */
DL_API int dl_copy(DlBytes to, size_t to_offset, DlConstBytes from, size_t from_offset, size_t count) {
  if (!dl_fits(to.size, to_offset, count) || !dl_fits(from.size, from_offset, count)) return 0;
  for (size_t i = 0; i < count; ++i) to.data[to_offset + i] = from.data[from_offset + i];
  return 1;
}
DL_API int dl_read(DlConstBytes from, size_t offset, void *out, size_t count) {
  DlBytes to = dl_bytes(out, count);
  return dl_copy(to, 0, from, offset, count);
}
DL_API int dl_write(DlBytes to, size_t offset, const void *in, size_t count) {
  return dl_copy(to, offset, dl_const_bytes(in, count), 0, count);
}
DL_API void dl_zero(void *out, size_t count) {
  uint8_t *p = (uint8_t *)out;
  for (size_t i = 0; i < count; ++i) p[i] = 0;
}
DL_API uint32_t dl_u16(DlConstBytes v, size_t at) {
  uint8_t b[2] = {0, 0};
  dl_read(v, at, b, 2);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8);
}
DL_API uint32_t dl_u32(DlConstBytes v, size_t at) {
  uint8_t b[4] = {0, 0, 0, 0};
  dl_read(v, at, b, 4);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
/* The bits of a float and the float of bits (render state values carry floats as DWORDs). */
DL_API float dl_float_of(uint32_t bits) { union { uint32_t u; float f; } x; x.u = bits; return x.f; }
DL_API uint32_t dl_bits_of(float f) { union { uint32_t u; float f; } x; x.f = f; return x.u; }

/* ==== structures (d3dtypes.h / d3dcaps.h) ====================================================================================================== */

typedef struct DlGuid { uint32_t data1; uint16_t data2, data3; uint8_t data4[8]; } DlGuid;
typedef struct DlRect { int32_t x1, y1, x2, y2; } DlRect;                                   /* D3DRECT */
typedef struct DlColorValue { float r, g, b, a; } DlColorValue;                              /* D3DCOLORVALUE */
typedef struct DlVector { float x, y, z; } DlVector;                                         /* D3DVECTOR */
typedef struct DlMatrix { float m[4][4]; } DlMatrix;                                         /* D3DMATRIX: m[r][c] is _(r+1)(c+1) */

typedef struct DlVertex { float x, y, z, nx, ny, nz, tu, tv; } DlVertex;                     /* D3DVERTEX */
typedef struct DlLVertex { float x, y, z; uint32_t reserved, color, specular; float tu, tv; } DlLVertex;       /* D3DLVERTEX */
typedef struct DlTlVertex { float sx, sy, sz, rhw; uint32_t color, specular; float tu, tv; } DlTlVertex;       /* D3DTLVERTEX */
typedef struct DlHVertex { uint32_t flags; float hx, hy, hz; } DlHVertex;                    /* D3DHVERTEX */

typedef struct DlStatus { uint32_t dwFlags, dwStatus; DlRect drExtent; } DlStatus;          /* D3DSTATUS */
typedef struct DlExecuteData {                                                               /* D3DEXECUTEDATA */
  uint32_t dwSize, dwVertexOffset, dwVertexCount, dwInstructionOffset, dwInstructionLength, dwHVertexOffset;
  DlStatus dsStatus;
} DlExecuteData;
typedef struct DlExecuteBufferDesc { uint32_t dwSize, dwFlags, dwCaps, dwBufferSize; void *lpData; } DlExecuteBufferDesc;  /* D3DEXECUTEBUFFERDESC */

/* The records of an execute buffer. */
typedef struct DlInstruction { uint8_t bOpcode, bSize; uint16_t wCount; } DlInstruction;   /* D3DINSTRUCTION */
typedef struct DlPoint { uint16_t wCount, wFirst; } DlPoint;                                 /* D3DPOINT */
typedef struct DlLine { uint16_t v1, v2; } DlLine;                                           /* D3DLINE */
typedef struct DlTriangle { uint16_t v1, v2, v3, wFlags; } DlTriangle;                       /* D3DTRIANGLE */
typedef struct DlMatrixLoad { uint32_t hDestMatrix, hSrcMatrix; } DlMatrixLoad;              /* D3DMATRIXLOAD */
typedef struct DlMatrixMultiply { uint32_t hDestMatrix, hSrcMatrix1, hSrcMatrix2; } DlMatrixMultiply;   /* D3DMATRIXMULTIPLY */
typedef struct DlState { uint32_t type, arg; } DlState;                                      /* D3DSTATE (arg: dwArg[0] / dvArg[0]) */
typedef struct DlProcessVertices { uint32_t dwFlags; uint16_t wStart, wDest; uint32_t dwCount, dwReserved; } DlProcessVertices;  /* D3DPROCESSVERTICES */
typedef struct DlTextureLoad { uint32_t hDestTexture, hSrcTexture; } DlTextureLoad;          /* D3DTEXTURELOAD */
typedef struct DlBranch { uint32_t dwMask, dwValue, bNegate, dwOffset; } DlBranch;           /* D3DBRANCH */
typedef struct DlSpan { uint16_t wCount, wFirst; } DlSpan;                                   /* D3DSPAN */

typedef struct DlViewport1 { uint32_t dwSize, dwX, dwY, dwWidth, dwHeight; float dvScaleX, dvScaleY, dvMaxX, dvMaxY, dvMinZ, dvMaxZ; } DlViewport1;   /* D3DVIEWPORT */
typedef struct DlViewport2 { uint32_t dwSize, dwX, dwY, dwWidth, dwHeight; float dvClipX, dvClipY, dvClipWidth, dvClipHeight, dvMinZ, dvMaxZ; } DlViewport2;  /* D3DVIEWPORT2 */
typedef struct DlViewport7 { uint32_t dwX, dwY, dwWidth, dwHeight; float dvMinZ, dvMaxZ; } DlViewport7;   /* D3DVIEWPORT7 (= D3DVIEWPORT9) */

typedef struct DlLight2 {        /* D3DLIGHT (dwSize 76: no dwFlags) and D3DLIGHT2 (80) */
  uint32_t dwSize, dltType;
  DlColorValue dcvColor;
  DlVector dvPosition, dvDirection;
  float dvRange, dvFalloff, dvAttenuation0, dvAttenuation1, dvAttenuation2, dvTheta, dvPhi;
  uint32_t dwFlags;
} DlLight2;
typedef struct DlLight7 {        /* D3DLIGHT7 (= D3DLIGHT9) */
  uint32_t dltType;
  DlColorValue dcvDiffuse, dcvSpecular, dcvAmbient;
  DlVector dvPosition, dvDirection;
  float dvRange, dvFalloff, dvAttenuation0, dvAttenuation1, dvAttenuation2, dvTheta, dvPhi;
} DlLight7;
typedef struct DlMaterial { uint32_t dwSize; DlColorValue diffuse, ambient, specular, emissive; float power; uint32_t hTexture, dwRampSize; } DlMaterial;  /* D3DMATERIAL */
typedef struct DlMaterial7 { DlColorValue diffuse, ambient, specular, emissive; float power; } DlMaterial7;   /* D3DMATERIAL7 (= D3DMATERIAL9) */

typedef struct DlTransformData {      /* D3DTRANSFORMDATA */
  uint32_t dwSize;
  void *lpIn;
  uint32_t dwInSize;
  void *lpOut;
  uint32_t dwOutSize;
  DlHVertex *lpHOut;
  uint32_t dwClip, dwClipIntersection, dwClipUnion;
  DlRect drExtent;
} DlTransformData;
typedef struct DlPtrStride { void *lpvData; uint32_t dwStride; } DlPtrStride;                /* D3DDP_PTRSTRIDE */
typedef struct DlStridedData { DlPtrStride position, normal, diffuse, specular, textureCoords[8]; } DlStridedData;   /* D3DDRAWPRIMITIVESTRIDEDDATA */

typedef struct DlTransformCaps { uint32_t dwSize, dwCaps; } DlTransformCaps;
typedef struct DlLightingCaps { uint32_t dwSize, dwCaps, dwLightingModel, dwNumLights; } DlLightingCaps;
typedef struct DlPrimCaps {           /* D3DPRIMCAPS */
  uint32_t dwSize, dwMiscCaps, dwRasterCaps, dwZCmpCaps, dwSrcBlendCaps, dwDestBlendCaps, dwAlphaCmpCaps, dwShadeCaps, dwTextureCaps, dwTextureFilterCaps,
      dwTextureBlendCaps, dwTextureAddressCaps, dwStippleWidth, dwStippleHeight;
} DlPrimCaps;
typedef struct DlDeviceDesc {         /* D3DDEVICEDESC: 172 bytes for Direct3D 1, 204 for 2, 252 for 3 (dwSize says which) */
  uint32_t dwSize, dwFlags, dcmColorModel, dwDevCaps;
  DlTransformCaps dtcTransformCaps;
  int32_t bClipping;
  DlLightingCaps dlcLightingCaps;
  DlPrimCaps dpcLineCaps, dpcTriCaps;
  uint32_t dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth, dwMaxBufferSize, dwMaxVertexCount;
  /* (Direct3D 2) */
  uint32_t dwMinTextureWidth, dwMinTextureHeight, dwMaxTextureWidth, dwMaxTextureHeight, dwMinStippleWidth, dwMaxStippleWidth, dwMinStippleHeight, dwMaxStippleHeight;
  /* (Direct3D 3) */
  uint32_t dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
  float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom, dvExtentsAdjust;
  uint32_t dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
  uint16_t wMaxTextureBlendStages, wMaxSimultaneousTextures;
} DlDeviceDesc;
#define DL_DEVICEDESC_V1_SIZE 172u       /* offsetof(D3DDEVICEDESC, dwMinTextureWidth) */
#define DL_DEVICEDESC_V2_SIZE 204u       /* offsetof(D3DDEVICEDESC, dwMaxTextureRepeat) */
#define DL_DEVICEDESC_V3_SIZE 252u       /* sizeof(D3DDEVICEDESC) */
typedef struct DlDeviceDesc7 {        /* D3DDEVICEDESC7 */
  uint32_t dwDevCaps;
  DlPrimCaps dpcLineCaps, dpcTriCaps;
  uint32_t dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth, dwMinTextureWidth, dwMinTextureHeight, dwMaxTextureWidth, dwMaxTextureHeight, dwMaxTextureRepeat,
      dwMaxTextureAspectRatio, dwMaxAnisotropy;
  float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom, dvExtentsAdjust;
  uint32_t dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
  uint16_t wMaxTextureBlendStages, wMaxSimultaneousTextures;
  uint32_t dwMaxActiveLights;
  float dvMaxVertexW;
  DlGuid deviceGUID;
  uint16_t wMaxUserClipPlanes, wMaxVertexBlendMatrices;
  uint32_t dwVertexProcessingCaps, dwReserved1, dwReserved2, dwReserved3, dwReserved4;
} DlDeviceDesc7;
typedef struct DlFindDeviceSearch { uint32_t dwSize, dwFlags; int32_t bHardware; uint32_t dcmColorModel; DlGuid guid; uint32_t dwCaps; DlPrimCaps dpcPrimCaps; } DlFindDeviceSearch;
#define DL_FINDDEVICESEARCH_SIZE 92u
#define DL_FINDDEVICERESULT_V1_SIZE (20u + 2u * DL_DEVICEDESC_V1_SIZE)     /* 364: dwSize, guid, two D3DDEVICEDESC_V1 */
#define DL_FINDDEVICERESULT_V2_SIZE (20u + 2u * DL_DEVICEDESC_V2_SIZE)     /* 428 */
#define DL_FINDDEVICERESULT_SIZE (20u + 2u * DL_DEVICEDESC_V3_SIZE)        /* 524 */

/* ==== constants ================================================================================================================================= */

enum { DL_OP_POINT = 1, DL_OP_LINE, DL_OP_TRIANGLE, DL_OP_MATRIXLOAD, DL_OP_MATRIXMULTIPLY, DL_OP_STATETRANSFORM, DL_OP_STATELIGHT, DL_OP_STATERENDER,
       DL_OP_PROCESSVERTICES, DL_OP_TEXTURELOAD, DL_OP_EXIT, DL_OP_BRANCHFORWARD, DL_OP_SPAN, DL_OP_SETSTATUS };
#define DL_TRIFLAG_START 0x0000u
#define DL_TRIFLAG_ODD 0x001Eu
#define DL_TRIFLAG_EVEN 0x001Fu
#define DL_TRIFLAG_EDGEENABLE1 0x0100u        /* v1 - v2 */
#define DL_TRIFLAG_EDGEENABLE2 0x0200u        /* v2 - v3 */
#define DL_TRIFLAG_EDGEENABLE3 0x0400u        /* v3 - v1 */
#define DL_PROCESSVERTICES_TRANSFORMLIGHT 0u
#define DL_PROCESSVERTICES_TRANSFORM 1u
#define DL_PROCESSVERTICES_COPY 2u
#define DL_PROCESSVERTICES_OPMASK 7u
#define DL_PROCESSVERTICES_UPDATEEXTENTS 8u
#define DL_PROCESSVERTICES_NOCOLOR 16u
#define DL_SETSTATUS_STATUS 1u
#define DL_SETSTATUS_EXTENTS 2u
#define DL_SETSTATUS_ALL 3u
#define DL_EXECUTE_CLIPPED 1u
#define DL_EXECUTE_UNCLIPPED 2u

#define DL_CLIP_LEFT 0x001u
#define DL_CLIP_RIGHT 0x002u
#define DL_CLIP_TOP 0x004u
#define DL_CLIP_BOTTOM 0x008u
#define DL_CLIP_FRONT 0x010u
#define DL_CLIP_BACK 0x020u
#define DL_CLIP_GEN0 0x040u                   /* .. D3DCLIP_GEN5 0x800: the user clip planes */
#define DL_CLIP_ALL 0xFFFu
#define DL_STATUS_CLIPUNIONALL 0x00000FFFu
#define DL_STATUS_CLIPINTERSECTIONALL 0x00FFF000u
#define DL_STATUS_ZNOTVISIBLE 0x01000000u
#define DL_STATUS_DEFAULT (DL_STATUS_CLIPINTERSECTIONALL | DL_STATUS_ZNOTVISIBLE)
#define DL_TRANSFORM_CLIPPED 1u
#define DL_TRANSFORM_UNCLIPPED 2u

enum { DL_LIGHT_POINT = 1, DL_LIGHT_SPOT, DL_LIGHT_DIRECTIONAL, DL_LIGHT_PARALLELPOINT, DL_LIGHT_GLSPOT };
#define DL_LIGHT_ACTIVE 1u
#define DL_LIGHT_NO_SPECULAR 2u
enum { DL_FOG_NONE = 0, DL_FOG_EXP, DL_FOG_EXP2, DL_FOG_LINEAR };
#define DL_COLOR_MONO 1u
#define DL_COLOR_RGB 2u
enum { DL_MCS_MATERIAL = 0, DL_MCS_COLOR1, DL_MCS_COLOR2 };

enum { DL_VT_VERTEX = 1, DL_VT_LVERTEX, DL_VT_TLVERTEX };
enum { DL_PT_POINTLIST = 1, DL_PT_LINELIST, DL_PT_LINESTRIP, DL_PT_TRIANGLELIST, DL_PT_TRIANGLESTRIP, DL_PT_TRIANGLEFAN };
#define DL_FVF_RESERVED0 0x001u
#define DL_FVF_POSITION_MASK 0x00Eu
#define DL_FVF_XYZ 0x002u
#define DL_FVF_XYZRHW 0x004u
#define DL_FVF_XYZB1 0x006u
#define DL_FVF_XYZB5 0x00Eu
#define DL_FVF_NORMAL 0x010u
#define DL_FVF_RESERVED1 0x020u               /* (D3DLVERTEX's dwReserved: four bytes after the position) */
#define DL_FVF_DIFFUSE 0x040u
#define DL_FVF_SPECULAR 0x080u
#define DL_FVF_TEXCOUNT_MASK 0xF00u
#define DL_FVF_TEXCOUNT_SHIFT 8u
#define DL_FVF_RESERVED2 0xF000u
#define DL_FVF_VERTEX 0x112u                  /* XYZ | NORMAL | TEX1 */
#define DL_FVF_LVERTEX 0x1E2u                 /* XYZ | RESERVED1 | DIFFUSE | SPECULAR | TEX1 */
#define DL_FVF_TLVERTEX 0x1C4u                /* XYZRHW | DIFFUSE | SPECULAR | TEX1 */
#define DL_DP_DONOTCLIP 4u
#define DL_DP_DONOTUPDATEEXTENTS 8u
#define DL_DP_DONOTLIGHT 16u

#define DL_VIS_INTERSECT_FRUSTUM 1u
#define DL_VIS_OUTSIDE_FRUSTUM 2u
#define DL_VIS_INTERSECT_LEFT (1u << 2)       /* plane n (left, right, top, bottom, near, far): INTERSECT << 2n, OUTSIDE (2 << 2) << 2n */
#define DL_VIS_OUTSIDE_LEFT (2u << 2)

/* ==== matrices ==================================================================================================================================== */

DL_API void dl_matrix_identity(DlMatrix *m) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m->m[r][c] = r == c ? 1.0f : 0.0f;
}
/* out = a * b (a row vector times out is the vector times a, then b). `out` may be `a` or `b`. Also D3DOP_MATRIXMULTIPLY: dest = src1 * src2. */
DL_API void dl_matrix_multiply(DlMatrix *out, const DlMatrix *a, const DlMatrix *b) {
  DlMatrix t;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) t.m[r][c] = a->m[r][0] * b->m[0][c] + a->m[r][1] * b->m[1][c] + a->m[r][2] * b->m[2][c] + a->m[r][3] * b->m[3][c];
  *out = t;
}
/* The inverse of a 4 x 4 matrix (cofactors over the determinant); 0 and the identity for one that has none. */
DL_API int dl_matrix_invert(DlMatrix *out, const DlMatrix *in) {
  const float *m = &in->m[0][0];
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!(det > 1e-30f || det < -1e-30f)) {
    dl_matrix_identity(out);
    return 0;
  }
  for (int i = 0; i < 16; ++i) out->m[i / 4][i % 4] = inv[i] / det;
  return 1;
}
/* A point (w = 1) times a matrix: four numbers. */
DL_API void dl_transform_point(const DlMatrix *m, const float p[3], float out[4]) {
  for (int c = 0; c < 4; ++c) out[c] = p[0] * m->m[0][c] + p[1] * m->m[1][c] + p[2] * m->m[2][c] + m->m[3][c];
}

/* ==== execute buffers ============================================================================================================================ */

#define DL_EXECUTEDATA_SIZE 48u
#define DL_VERTEX_SIZE 32u             /* D3DVERTEX, D3DLVERTEX and D3DTLVERTEX alike */

/* SetExecuteData / Execute: the execute data describes vertices and instructions that lie in the buffer (of `buffer_size` bytes). */
DL_API DlResult dl_execute_data_check(const DlExecuteData *data, uint32_t buffer_size) {
  if (!data || data->dwSize != DL_EXECUTEDATA_SIZE) return DL_DDERR_INVALIDPARAMS;
  if (!dl_fits(buffer_size, data->dwInstructionOffset, data->dwInstructionLength)) return DL_DDERR_INVALIDPARAMS;
  if (data->dwVertexOffset > buffer_size || data->dwVertexCount > (buffer_size - data->dwVertexOffset) / DL_VERTEX_SIZE) return DL_DDERR_INVALIDPARAMS;
  return DL_OK;
}

/* One instruction for the device to carry out: `count` records of `size` bytes from `at` in the buffer. */
typedef struct DlCommand { uint32_t opcode, count, size, at, header; } DlCommand;

/* Walks an execute buffer's instructions. The status is the execute buffer's own (D3DEXECUTEDATA.dsStatus, which GetExecuteData reports): SETSTATUS
 * writes it, BRANCHFORWARD reads it, and the device updates it when it processes vertices (dl_status_merge). */
typedef struct DlExecutor {
  DlConstBytes buffer;
  uint32_t end;                  /* one past the last instruction byte */
  uint32_t pos;
  uint32_t vertex_count;         /* dwVertexCount: what primitives and PROCESSVERTICES may refer to */
  DlStatus *status;
  int done;
  DlResult error;                /* why it stopped, if it was not the end */
} DlExecutor;

enum { DL_NEXT_ERROR = -1, DL_NEXT_END = 0, DL_NEXT_COMMAND = 1 };

DL_API DlResult dl_execute_begin(DlExecutor *x, DlConstBytes buffer, const DlExecuteData *data, DlStatus *status) {
  dl_zero(x, sizeof *x);
  x->done = 1;
  const uint64_t whole = buffer.size;
  if (!status || (whole >> 32)) return DL_DDERR_INVALIDPARAMS;
  const DlResult r = dl_execute_data_check(data, (uint32_t)buffer.size);
  if (r != DL_OK) return r;
  x->buffer = buffer;
  x->pos = data->dwInstructionOffset;
  x->end = data->dwInstructionOffset + data->dwInstructionLength;
  x->vertex_count = data->dwVertexCount;
  x->status = status;
  x->done = 0;
  return DL_OK;
}

/* The smallest record each instruction may have (bSize below it is refused): 0 for the ones without records. */
DL_API uint32_t dl_record_size(uint32_t opcode) {
  switch (opcode) {
  case DL_OP_POINT: case DL_OP_LINE: case DL_OP_SPAN: return 4;
  case DL_OP_TRIANGLE: case DL_OP_MATRIXLOAD: case DL_OP_STATETRANSFORM: case DL_OP_STATELIGHT: case DL_OP_STATERENDER: case DL_OP_TEXTURELOAD: return 8;
  case DL_OP_MATRIXMULTIPLY: return 12;
  case DL_OP_PROCESSVERTICES: case DL_OP_BRANCHFORWARD: return 16;
  case DL_OP_SETSTATUS: return 24;
  default: return 0;
  }
}

DL_API int dl_execute_fail(DlExecutor *x, DlResult why) {
  x->done = 1;
  x->error = why;
  return DL_NEXT_ERROR;
}

/* The next instruction for the device, DL_NEXT_END when the stream is over (D3DOP_EXIT, a branch taken with offset 0, or the end of the instruction
 * area), DL_NEXT_ERROR (x->error, D3DERR_EXECUTE_FAILED) when an instruction does not fit in the area or its records are smaller than they must be,
 * or a branch goes past the end. What came before an error has been carried out; nothing after it is.
 *
 * Done here: EXIT, BRANCHFORWARD (each record in turn: (status & mask) == value, negated by bNegate, branches dwOffset bytes forward from the
 * instruction's header - Wine's reading - or ends the buffer for an offset of 0), SETSTATUS (the status, the extents or both, as its dwFlags say),
 * and instructions with no records. Opcodes Direct3D does not have are skipped over (count * size bytes), as Windows and Wine do. Every step goes
 * forward, so a buffer cannot make this loop. */
DL_API int dl_execute_next(DlExecutor *x, DlCommand *cmd) {
  while (!x->done) {
    if (x->pos >= x->end) {
      x->done = 1;
      return DL_NEXT_END;
    }
    if (x->end - x->pos < 4) return dl_execute_fail(x, DL_D3DERR_EXECUTE_FAILED);
    const uint32_t header = x->pos;
    const uint32_t opcode = x->buffer.data[header], size = x->buffer.data[header + 1], count = dl_u16(x->buffer, header + 2);
    const uint32_t at = header + 4;
    const uint64_t length = (uint64_t)count * size;
    if (opcode == DL_OP_EXIT) {
      x->done = 1;
      return DL_NEXT_END;
    }
    if (length > x->end - at) return dl_execute_fail(x, DL_D3DERR_EXECUTE_FAILED);
    if (count && size < dl_record_size(opcode)) return dl_execute_fail(x, DL_D3DERR_EXECUTE_FAILED);
    x->pos = at + (uint32_t)length;
    if (!count) continue;
    if (opcode == DL_OP_SETSTATUS) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = at + i * size;
        const uint32_t flags = dl_u32(x->buffer, r);
        if (flags & DL_SETSTATUS_STATUS) x->status->dwStatus = dl_u32(x->buffer, r + 4);
        if (flags & DL_SETSTATUS_EXTENTS) {
          x->status->drExtent.x1 = (int32_t)dl_u32(x->buffer, r + 8);
          x->status->drExtent.y1 = (int32_t)dl_u32(x->buffer, r + 12);
          x->status->drExtent.x2 = (int32_t)dl_u32(x->buffer, r + 16);
          x->status->drExtent.y2 = (int32_t)dl_u32(x->buffer, r + 20);
        }
      }
      continue;
    }
    if (opcode == DL_OP_BRANCHFORWARD) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = at + i * size;
        const uint32_t mask = dl_u32(x->buffer, r), value = dl_u32(x->buffer, r + 4), negate = dl_u32(x->buffer, r + 8), offset = dl_u32(x->buffer, r + 12);
        int taken = (x->status->dwStatus & mask) == value;
        if (negate) taken = !taken;
        if (!taken) continue;
        if (!offset) {
          x->done = 1;
          return DL_NEXT_END;
        }
        const uint64_t target = (uint64_t)header + offset;
        if (target > x->end) return dl_execute_fail(x, DL_D3DERR_EXECUTE_FAILED);
        x->pos = (uint32_t)target;
        break;
      }
      continue;
    }
    if (opcode < DL_OP_POINT || opcode > DL_OP_SETSTATUS) continue;      /* (not an instruction: skipped) */
    cmd->opcode = opcode;
    cmd->count = count;
    cmd->size = size;
    cmd->at = at;
    cmd->header = header;
    return DL_NEXT_COMMAND;
  }
  return x->error ? DL_NEXT_ERROR : DL_NEXT_END;
}

/* The records of a command. Each gives DL_OK, or DL_RANGE for one that refers to vertices the execute data does not have (the device skips that
 * record: drawing it would read past the vertex buffer), or DL_BAD for an index past the command or a command of another kind. */
enum { DL_RANGE = 1, DL_BAD = 2 };

DL_API int dl_record(const DlExecutor *x, const DlCommand *cmd, uint32_t opcode, uint32_t i, uint32_t *r) {
  if (cmd->opcode != opcode || i >= cmd->count) return 0;
  *r = cmd->at + i * cmd->size;
  return dl_fits(x->buffer.size, *r, dl_record_size(opcode));
}
/* D3DOP_POINT: wCount vertices from wFirst, drawn as points. */
DL_API int dl_command_point(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlPoint *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_POINT, i, &r)) return DL_BAD;
  out->wCount = (uint16_t)dl_u16(x->buffer, r);
  out->wFirst = (uint16_t)dl_u16(x->buffer, r + 2);
  return (uint32_t)out->wFirst + out->wCount <= x->vertex_count ? DL_OK : DL_RANGE;
}
/* D3DOP_SPAN: wCount vertices from wFirst, each one pixel of a span (the device draws them as points). */
DL_API int dl_command_span(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlSpan *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_SPAN, i, &r)) return DL_BAD;
  out->wCount = (uint16_t)dl_u16(x->buffer, r);
  out->wFirst = (uint16_t)dl_u16(x->buffer, r + 2);
  return (uint32_t)out->wFirst + out->wCount <= x->vertex_count ? DL_OK : DL_RANGE;
}
DL_API int dl_command_line(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlLine *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_LINE, i, &r)) return DL_BAD;
  out->v1 = (uint16_t)dl_u16(x->buffer, r);
  out->v2 = (uint16_t)dl_u16(x->buffer, r + 2);
  return out->v1 < x->vertex_count && out->v2 < x->vertex_count ? DL_OK : DL_RANGE;
}
/* D3DOP_TRIANGLE: the three vertices as they are (the D3DTRIFLAG_START / STARTFLAT / ODD / EVEN flags say how a strip or fan was made, for drivers
 * that reuse vertices; they do not change which vertices a triangle has) and the edge flags, which the wireframe fill mode draws by. */
DL_API int dl_command_triangle(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlTriangle *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_TRIANGLE, i, &r)) return DL_BAD;
  out->v1 = (uint16_t)dl_u16(x->buffer, r);
  out->v2 = (uint16_t)dl_u16(x->buffer, r + 2);
  out->v3 = (uint16_t)dl_u16(x->buffer, r + 4);
  out->wFlags = (uint16_t)dl_u16(x->buffer, r + 6);
  return out->v1 < x->vertex_count && out->v2 < x->vertex_count && out->v3 < x->vertex_count ? DL_OK : DL_RANGE;
}
/* The edges of a triangle the wireframe mode draws (D3DFILL_WIREFRAME): pairs of corners (0 .. 2) into `pairs`, their number back. */
DL_API unsigned dl_triangle_edges(uint32_t flags, uint8_t pairs[6]) {
  unsigned n = 0;
  if (flags & DL_TRIFLAG_EDGEENABLE1) { pairs[n++] = 0; pairs[n++] = 1; }
  if (flags & DL_TRIFLAG_EDGEENABLE2) { pairs[n++] = 1; pairs[n++] = 2; }
  if (flags & DL_TRIFLAG_EDGEENABLE3) { pairs[n++] = 2; pairs[n++] = 0; }
  return n / 2;
}
/* D3DOP_MATRIXLOAD: the matrix of hSrcMatrix is copied into hDestMatrix. */
DL_API int dl_command_matrix_load(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlMatrixLoad *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_MATRIXLOAD, i, &r)) return DL_BAD;
  out->hDestMatrix = dl_u32(x->buffer, r);
  out->hSrcMatrix = dl_u32(x->buffer, r + 4);
  return DL_OK;
}
/* D3DOP_MATRIXMULTIPLY: dest = src1 * src2 (dl_matrix_multiply); a handle that is not a matrix leaves dest as it is. */
DL_API int dl_command_matrix_multiply(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlMatrixMultiply *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_MATRIXMULTIPLY, i, &r)) return DL_BAD;
  out->hDestMatrix = dl_u32(x->buffer, r);
  out->hSrcMatrix1 = dl_u32(x->buffer, r + 4);
  out->hSrcMatrix2 = dl_u32(x->buffer, r + 8);
  return DL_OK;
}
/* D3DOP_STATETRANSFORM (type: D3DTRANSFORMSTATE_*, arg: a matrix handle), STATELIGHT (D3DLIGHTSTATE_*), STATERENDER (D3DRENDERSTATE_*). */
DL_API int dl_command_state(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlState *out) {
  if (cmd->opcode != DL_OP_STATETRANSFORM && cmd->opcode != DL_OP_STATELIGHT && cmd->opcode != DL_OP_STATERENDER) return DL_BAD;
  uint32_t r;
  if (!dl_record(x, cmd, cmd->opcode, i, &r)) return DL_BAD;
  out->type = dl_u32(x->buffer, r);
  out->arg = dl_u32(x->buffer, r + 4);
  return DL_OK;
}
/* D3DOP_PROCESSVERTICES: dwCount vertices from wStart into the processed vertices from wDest, both within dwVertexCount. */
DL_API int dl_command_process_vertices(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlProcessVertices *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_PROCESSVERTICES, i, &r)) return DL_BAD;
  out->dwFlags = dl_u32(x->buffer, r);
  out->wStart = (uint16_t)dl_u16(x->buffer, r + 4);
  out->wDest = (uint16_t)dl_u16(x->buffer, r + 6);
  out->dwCount = dl_u32(x->buffer, r + 8);
  out->dwReserved = dl_u32(x->buffer, r + 12);
  if (out->dwCount > x->vertex_count || out->wStart > x->vertex_count - out->dwCount || out->wDest > x->vertex_count - out->dwCount) return DL_RANGE;
  return DL_OK;
}
/* D3DOP_TEXTURELOAD: IDirect3DTexture::Load of hSrcTexture into hDestTexture (handles that are not textures: the record is skipped). */
DL_API int dl_command_texture_load(const DlExecutor *x, const DlCommand *cmd, uint32_t i, DlTextureLoad *out) {
  uint32_t r;
  if (!dl_record(x, cmd, DL_OP_TEXTURELOAD, i, &r)) return DL_BAD;
  out->hDestTexture = dl_u32(x->buffer, r);
  out->hSrcTexture = dl_u32(x->buffer, r + 4);
  return DL_OK;
}
/* A vertex of the execute buffer's vertex data (32 bytes: a D3DVERTEX, D3DLVERTEX or D3DTLVERTEX as the instruction says). */
DL_API int dl_execute_vertex(DlConstBytes buffer, const DlExecuteData *data, uint32_t index, void *out32) {
  if (index >= data->dwVertexCount) return 0;
  return dl_read(buffer, (size_t)data->dwVertexOffset + (size_t)index * DL_VERTEX_SIZE, out32, DL_VERTEX_SIZE);
}

/* ==== the transform =============================================================================================================================== */

/* What a vertex goes through: world, view, the program's projection and (Direct3D 1-3) the clip space of the viewport, then the viewport's
 * rectangle. Fill the first part, then dl_transform_prepare. */
typedef struct DlTransform {
  DlMatrix world, view, projection;     /* as the program set them */
  DlMatrix clip;                        /* the viewport's clip space (dl_viewport7_from_v1/v2); the identity for Direct3D 7 */
  DlViewport7 viewport;
  uint32_t user_planes;                 /* D3DRENDERSTATE_CLIPPLANEENABLE (Direct3D 7): bits of the six user clip planes */
  float user_plane[6][4];               /* SetClipPlane: in world space */
  /* worked out by dl_transform_prepare */
  DlMatrix world_view, total, view_inverse;
  float normal_matrix[3][3];            /* normals into world space */
  float eye[3], eye_axis[3];            /* the camera in world space and the way it looks */
} DlTransform;

DL_API void dl_transform_init(DlTransform *t) {
  dl_zero(t, sizeof *t);
  dl_matrix_identity(&t->world);
  dl_matrix_identity(&t->view);
  dl_matrix_identity(&t->projection);
  dl_matrix_identity(&t->clip);
  t->viewport.dvMaxZ = 1.0f;
}
DL_API void dl_transform_prepare(DlTransform *t) {
  dl_matrix_multiply(&t->world_view, &t->world, &t->view);
  dl_matrix_multiply(&t->total, &t->world_view, &t->projection);
  dl_matrix_multiply(&t->total, &t->total, &t->clip);
  dl_matrix_invert(&t->view_inverse, &t->view);
  ffl_normal_matrix(t->world.m, t->normal_matrix);
  for (int i = 0; i < 3; ++i) {
    t->eye[i] = t->view_inverse.m[3][i];
    t->eye_axis[i] = t->view_inverse.m[2][i];
  }
  ffl_normalize(t->eye_axis);
}

/* The clip codes of a point in clip space (x, y, z, w): -w <= x, y <= w and 0 <= z <= w inside, as Direct3D 1-3's transform and Wine's
 * TransformVertices test them (the viewport's clip volume is in the clip space matrix). User clip planes come as distances (inside if >= 0). */
DL_API uint32_t dl_clip_code(const float p[4], const float *user, uint32_t user_planes) {
  uint32_t code = 0;
  if (p[0] < -p[3]) code |= DL_CLIP_LEFT;
  if (p[0] > p[3]) code |= DL_CLIP_RIGHT;
  if (p[1] > p[3]) code |= DL_CLIP_TOP;
  if (p[1] < -p[3]) code |= DL_CLIP_BOTTOM;
  if (p[2] < 0.0f) code |= DL_CLIP_FRONT;
  if (p[2] > p[3]) code |= DL_CLIP_BACK;
  for (unsigned i = 0; i < 6 && user; ++i)
    if ((user_planes & (1u << i)) && user[i] < 0.0f) code |= DL_CLIP_GEN0 << i;
  return code;
}

/* A point in clip space onto the screen: the viewport's rectangle and depth range. */
DL_API void dl_to_screen(const DlViewport7 *vp, const float p[4], float *sx, float *sy, float *sz, float *rhw) {
  const float r = (p[3] > 0.0f || p[3] < 0.0f) ? 1.0f / p[3] : 0.0f;      /* (w 0: no point on the screen; rhw 0) */
  *sx = (float)vp->dwX + (1.0f + p[0] * r) * (float)vp->dwWidth * 0.5f;
  *sy = (float)vp->dwY + (1.0f - p[1] * r) * (float)vp->dwHeight * 0.5f;
  *sz = vp->dvMinZ + p[2] * r * (vp->dvMaxZ - vp->dvMinZ);
  *rhw = r;
}
/* And back, for vertices that come already transformed (D3DPROCESSVERTICES_COPY): the clip space point the clipper can work with. */
DL_API void dl_from_screen(const DlViewport7 *vp, float sx, float sy, float sz, float rhw, float p[4]) {
  const float w = (rhw > 0.0f || rhw < 0.0f) ? 1.0f / rhw : 1.0f;
  const float hw = (float)vp->dwWidth * 0.5f, hh = (float)vp->dwHeight * 0.5f, dz = vp->dvMaxZ - vp->dvMinZ;
  const float nx = hw > 0.0f ? (sx - (float)vp->dwX) / hw - 1.0f : 0.0f;
  const float ny = hh > 0.0f ? 1.0f - (sy - (float)vp->dwY) / hh : 0.0f;
  const float nz = (dz > 0.0f || dz < 0.0f) ? (sz - vp->dvMinZ) / dz : sz;
  p[0] = nx * w;
  p[1] = ny * w;
  p[2] = nz * w;
  p[3] = w;
}

/* ==== viewports =================================================================================================================================== */

#define DL_VIEWPORT1_SIZE 44u
#define DL_VIEWPORT2_SIZE 44u

/* The clip space of a D3DVIEWPORT (Wine's viewport_activate): projected x times dvScaleX lands dvScaleX pixels from the viewport's centre, so the
 * clip space scales x by dvScaleX / (dwWidth / 2). The depth range is 0 .. 1 (SetViewport forces dvMinZ 0 and dvMaxZ 1); dvMaxX / dvMaxY do not
 * clip (the rectangle does). */
DL_API void dl_viewport7_from_v1(const DlViewport1 *v, DlViewport7 *out, DlMatrix *clip) {
  out->dwX = v->dwX;
  out->dwY = v->dwY;
  out->dwWidth = v->dwWidth;
  out->dwHeight = v->dwHeight;
  out->dvMinZ = 0.0f;
  out->dvMaxZ = 1.0f;
  dl_matrix_identity(clip);
  clip->m[0][0] = v->dwWidth ? 2.0f * v->dvScaleX / (float)v->dwWidth : 0.0f;
  clip->m[1][1] = v->dwHeight ? 2.0f * v->dvScaleY / (float)v->dwHeight : 0.0f;
}
/* A D3DVIEWPORT2: the clip volume dvClipX .. dvClipX + dvClipWidth, dvClipY - dvClipHeight .. dvClipY (y up), dvMinZ .. dvMaxZ goes to the
 * whole viewport. */
DL_API void dl_viewport7_from_v2(const DlViewport2 *v, DlViewport7 *out, DlMatrix *clip) {
  out->dwX = v->dwX;
  out->dwY = v->dwY;
  out->dwWidth = v->dwWidth;
  out->dwHeight = v->dwHeight;
  out->dvMinZ = 0.0f;
  out->dvMaxZ = 1.0f;
  dl_matrix_identity(clip);
  const float dz = v->dvMaxZ - v->dvMinZ;
  const int w = v->dvClipWidth > 0.0f || v->dvClipWidth < 0.0f, h = v->dvClipHeight > 0.0f || v->dvClipHeight < 0.0f, z = dz > 0.0f || dz < 0.0f;
  clip->m[0][0] = w ? 2.0f / v->dvClipWidth : 0.0f;
  clip->m[1][1] = h ? 2.0f / v->dvClipHeight : 0.0f;
  clip->m[2][2] = z ? 1.0f / dz : 0.0f;
  clip->m[3][0] = w ? -2.0f * v->dvClipX / v->dvClipWidth - 1.0f : 0.0f;
  clip->m[3][1] = h ? -2.0f * v->dvClipY / v->dvClipHeight + 1.0f : 0.0f;
  clip->m[3][2] = z ? -v->dvMinZ / dz : 0.0f;
}
/* GetViewport of a viewport set with SetViewport2, and GetViewport2 of one set with SetViewport (what Wine reports). */
DL_API void dl_viewport1_from_v2(const DlViewport2 *v, DlViewport1 *out) {
  out->dwSize = DL_VIEWPORT1_SIZE;
  out->dwX = v->dwX;
  out->dwY = v->dwY;
  out->dwWidth = v->dwWidth;
  out->dwHeight = v->dwHeight;
  out->dvScaleX = (v->dvClipWidth > 0.0f || v->dvClipWidth < 0.0f) ? (float)v->dwWidth / v->dvClipWidth : 0.0f;
  out->dvScaleY = (v->dvClipHeight > 0.0f || v->dvClipHeight < 0.0f) ? (float)v->dwHeight / v->dvClipHeight : 0.0f;
  out->dvMaxX = v->dvClipWidth + v->dvClipX;
  out->dvMaxY = v->dvClipY;
  out->dvMinZ = 0.0f;
  out->dvMaxZ = 1.0f;
}
DL_API void dl_viewport2_from_v1(const DlViewport1 *v, DlViewport2 *out) {
  out->dwSize = DL_VIEWPORT2_SIZE;
  out->dwX = v->dwX;
  out->dwY = v->dwY;
  out->dwWidth = v->dwWidth;
  out->dwHeight = v->dwHeight;
  out->dvClipWidth = (v->dvScaleX > 0.0f || v->dvScaleX < 0.0f) ? (float)v->dwWidth / v->dvScaleX : 0.0f;
  out->dvClipHeight = (v->dvScaleY > 0.0f || v->dvScaleY < 0.0f) ? (float)v->dwHeight / v->dvScaleY : 0.0f;
  out->dvClipX = -out->dvClipWidth / 2.0f;
  out->dvClipY = out->dvClipHeight / 2.0f;
  out->dvMinZ = 0.0f;
  out->dvMaxZ = 1.0f;
}
/* SetViewport / SetViewport2: the size, and (devices of Direct3D 2 and later) the rectangle within the render target. */
DL_API DlResult dl_viewport_check(uint32_t size, uint32_t expected, unsigned device_version, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                  uint32_t target_width, uint32_t target_height) {
  if (size != expected) return DL_DDERR_INVALIDPARAMS;
  if (device_version > 1 && (x > target_width || width > target_width - x || y > target_height || height > target_height - y)) return DL_DDERR_INVALIDPARAMS;
  return DL_OK;
}
/* The projection Direct3D 9 is given for a Direct3D 1-3 device: the program's projection, then the clip space. GetTransform reports the program's. */
DL_API void dl_legacy_projection(const DlMatrix *program, const DlMatrix *clip, DlMatrix *out) { dl_matrix_multiply(out, program, clip); }

/* ==== lighting ==================================================================================================================================== */

#define DL_LIGHTS 32u      /* lights at once; Direct3D 1-3 have no limit, the device reports dlcLightingCaps.dwNumLights */

/* One light as the lighting here uses it, in world space. */
typedef struct DlLightEntry {
  uint32_t type;           /* DL_LIGHT_* */
  float diffuse[3], specular[3], ambient[3];
  float position[3], direction[3];
  float range, falloff, attenuation[3], theta, phi;
} DlLightEntry;

typedef struct DlLighting {
  int legacy;              /* 1: Direct3D 1-3 (attenuation over the range, parallel point lights); 0: Direct3D 7 */
  int enabled;             /* there is lighting: a material is set (1-3) / D3DRENDERSTATE_LIGHTING (7) */
  DlMaterial7 material;
  float ambient[3];        /* D3DLIGHTSTATE_AMBIENT / D3DRENDERSTATE_AMBIENT */
  int specular;            /* D3DRENDERSTATE_SPECULARENABLE */
  int local_viewer;        /* D3DRENDERSTATE_LOCALVIEWER (Direct3D 1-3: always) */
  int normalize;           /* D3DRENDERSTATE_NORMALIZENORMALS (Direct3D 1-3: always) */
  int color_vertex;        /* D3DLIGHTSTATE_COLORVERTEX / D3DRENDERSTATE_COLORVERTEX */
  int diffuse_source, ambient_source, specular_source, emissive_source;    /* D3DMCS_*: where the colours come from when color_vertex is on */
  int fog_mode;            /* D3DLIGHTSTATE_FOGMODE / D3DRENDERSTATE_FOGVERTEXMODE */
  float fog_start, fog_end, fog_density;
  int range_fog;           /* D3DRENDERSTATE_RANGEFOGENABLE */
  unsigned lights;
  DlLightEntry light[DL_LIGHTS];
} DlLighting;

/* The states a device of Direct3D `version` starts with. */
DL_API void dl_lighting_init(DlLighting *l, unsigned version) {
  dl_zero(l, sizeof *l);
  l->legacy = version < 7;
  l->enabled = version >= 7;
  l->local_viewer = 1;
  l->normalize = version < 7;
  l->color_vertex = 1;
  l->diffuse_source = DL_MCS_COLOR1;
  l->specular_source = DL_MCS_COLOR2;
  l->specular = version == 2;
  l->fog_end = 1.0f;
  l->fog_density = 1.0f;
}

DL_API void dl_color_to_float(uint32_t c, float out[4]) {
  out[0] = (float)((c >> 16) & 0xFF) / 255.0f;
  out[1] = (float)((c >> 8) & 0xFF) / 255.0f;
  out[2] = (float)(c & 0xFF) / 255.0f;
  out[3] = (float)(c >> 24) / 255.0f;
}
DL_API uint32_t dl_unit_to_byte(float x) {
  if (!(x > 0.0f)) return 0;
  if (x >= 1.0f) return 255;
  return (uint32_t)(x * 255.0f + 0.5f);
}
DL_API uint32_t dl_color_from_float(const float c[4]) {
  return (dl_unit_to_byte(c[3]) << 24) | (dl_unit_to_byte(c[0]) << 16) | (dl_unit_to_byte(c[1]) << 8) | dl_unit_to_byte(c[2]);
}

/* e ^ -x for x >= 0 (vertex fog): ffl_pow of 1 / e. */
DL_API float dl_exp_neg(float x) {
  if (!(x > 0.0f)) return 1.0f;
  if (x > 100.0f) return 0.0f;
  return ffl_pow(0.36787944117144233f, x);
}
/* The fog factor of a vertex at `distance` (1: no fog, 0: all fog). */
DL_API float dl_fog_factor(int mode, float start, float end, float density, float distance) {
  float f = 1.0f;
  switch (mode) {
  case DL_FOG_LINEAR:
    if (end > start || end < start) f = (end - distance) / (end - start);
    else f = distance < start ? 1.0f : 0.0f;
    break;
  case DL_FOG_EXP: f = dl_exp_neg(density * distance); break;
  case DL_FOG_EXP2: { const float d = density * distance; f = dl_exp_neg(d * d); break; }
  default: return 1.0f;
  }
  return f > 1.0f ? 1.0f : f > 0.0f ? f : 0.0f;
}

/* Lights one vertex: position and normal in object space, the vertex's colours (NULL for one it has not: the material's own is used), `diffuse`
 * (four numbers) and `specular` (three) out. Both models:
 *
 *   colour   = emissive + ambient * (global ambient + sum of light ambients) + diffuse * sum(att * spot * max(N.L, 0) * light diffuse)
 *   specular = specular * sum(att * spot * (N.H) ^ power * light specular)      (lights with N.L > 0, SPECULARENABLE on)
 *
 * Direct3D 7 weighs a positional light by 1 / (a0 + a1 d + a2 d^2). Direct3D 1-3 weigh it by a0 + a1 r + a2 r^2 with r = (range - d) / range, the
 * share of its range still to go (so a0 = 0, a1 = 1 fades it out linearly to nothing at the range) - the legacy attenuation wined3d implements for
 * ddraw. Both give no light past the range. A parallel point light (1-3) shines from its position onto the whole object: its direction is from the
 * object's origin (the world matrix's translation), not from each vertex, and it is not attenuated. H is half way between L and the direction to
 * the camera (from the vertex for a local viewer, back along the camera's axis otherwise). */
DL_API void dl_light_vertex(const DlTransform *t, const DlLighting *l, const float position[3], const float *normal, const float *vertex_diffuse,
                            const float *vertex_specular, float diffuse[4], float specular[3]) {
  float place[4], n[3] = {0.0f, 0.0f, 0.0f};
  dl_transform_point(&t->world, position, place);
  if (normal) {
    for (int i = 0; i < 3; ++i) n[i] = normal[0] * t->normal_matrix[0][i] + normal[1] * t->normal_matrix[1][i] + normal[2] * t->normal_matrix[2][i];
    if (l->normalize) ffl_normalize(n);
  }
  float to_viewer[3];
  if (l->local_viewer) {
    for (int i = 0; i < 3; ++i) to_viewer[i] = t->eye[i] - place[i];
    ffl_normalize(to_viewer);
  } else {
    for (int i = 0; i < 3; ++i) to_viewer[i] = -t->eye_axis[i];
  }
  float ambient_sum[3] = {l->ambient[0], l->ambient[1], l->ambient[2]}, diffuse_sum[3] = {0, 0, 0}, specular_sum[3] = {0, 0, 0};
  for (unsigned k = 0; k < l->lights && k < DL_LIGHTS; ++k) {
    const DlLightEntry *light = &l->light[k];
    float to_light[3], weight = 1.0f;
    if (light->type == DL_LIGHT_DIRECTIONAL) {
      for (int i = 0; i < 3; ++i) to_light[i] = -light->direction[i];
      ffl_normalize(to_light);
    } else if (light->type == DL_LIGHT_PARALLELPOINT) {
      for (int i = 0; i < 3; ++i) to_light[i] = light->position[i] - t->world.m[3][i];
      ffl_normalize(to_light);
    } else if (light->type == DL_LIGHT_POINT || light->type == DL_LIGHT_SPOT) {
      for (int i = 0; i < 3; ++i) to_light[i] = light->position[i] - place[i];
      const float distance = ffl_normalize(to_light);
      if (!(distance <= light->range)) continue;
      if (l->legacy) {
        const float r = light->range > 0.0f ? (light->range - distance) / light->range : 0.0f;
        weight = light->attenuation[0] + light->attenuation[1] * r + light->attenuation[2] * r * r;
        if (!(weight > 0.0f)) continue;
      } else {
        const float divisor = light->attenuation[0] + light->attenuation[1] * distance + light->attenuation[2] * distance * distance;
        if (!(divisor > 0.0f)) continue;
        weight = 1.0f / divisor;
      }
      if (light->type == DL_LIGHT_SPOT) {
        float axis[3] = {light->direction[0], light->direction[1], light->direction[2]};
        ffl_normalize(axis);
        const float rho = -ffl_dot(to_light, axis);
        const float cos_outer = ffl_cos(light->phi * 0.5f), cos_inner = ffl_cos(light->theta * 0.5f);
        if (rho <= cos_outer) continue;
        if (rho < cos_inner && cos_inner > cos_outer) weight *= ffl_pow((rho - cos_outer) / (cos_inner - cos_outer), light->falloff);
      }
    } else {
      continue;
    }
    for (int i = 0; i < 3; ++i) ambient_sum[i] += weight * light->ambient[i];
    const float facing = ffl_dot(n, to_light);
    if (!(facing > 0.0f)) continue;
    for (int i = 0; i < 3; ++i) diffuse_sum[i] += weight * facing * light->diffuse[i];
    if (!l->specular) continue;
    if (l->legacy && !(l->material.power > 0.0f)) continue;      /* (Direct3D 1-3: a power of 0 has no highlight - Wine's test_specular_lighting) */
    float half[3] = {to_light[0] + to_viewer[0], to_light[1] + to_viewer[1], to_light[2] + to_viewer[2]};
    ffl_normalize(half);
    const float shine = ffl_dot(n, half);
    if (!(shine > 0.0f)) continue;
    const float lit = weight * ffl_pow(shine, l->material.power);
    for (int i = 0; i < 3; ++i) specular_sum[i] += lit * light->specular[i];
  }
  const float md[4] = {l->material.diffuse.r, l->material.diffuse.g, l->material.diffuse.b, l->material.diffuse.a};
  const float ma[4] = {l->material.ambient.r, l->material.ambient.g, l->material.ambient.b, l->material.ambient.a};
  const float ms[4] = {l->material.specular.r, l->material.specular.g, l->material.specular.b, l->material.specular.a};
  const float me[4] = {l->material.emissive.r, l->material.emissive.g, l->material.emissive.b, l->material.emissive.a};
  const float *sources[3] = {NULL, vertex_diffuse, vertex_specular};
  const float *cd = md, *ca = ma, *cs = ms, *ce = me;
  if (l->color_vertex) {
    if (l->diffuse_source >= 1 && l->diffuse_source <= 2 && sources[l->diffuse_source]) cd = sources[l->diffuse_source];
    if (l->ambient_source >= 1 && l->ambient_source <= 2 && sources[l->ambient_source]) ca = sources[l->ambient_source];
    if (l->specular_source >= 1 && l->specular_source <= 2 && sources[l->specular_source]) cs = sources[l->specular_source];
    if (l->emissive_source >= 1 && l->emissive_source <= 2 && sources[l->emissive_source]) ce = sources[l->emissive_source];
  }
  for (int i = 0; i < 3; ++i) {
    diffuse[i] = ffl_clamp(ce[i] + ca[i] * ambient_sum[i] + cd[i] * diffuse_sum[i]);
    specular[i] = l->specular ? ffl_clamp(cs[i] * specular_sum[i]) : 0.0f;
  }
  diffuse[3] = ffl_clamp(cd[3]);
}

/* ==== vertex processing ================================================================================================================================ */

#define DL_TEXTURE_SETS 8u
#define DL_CLIP_MAX 16u        /* vertices of a triangle clipped by the six planes and six user planes, at most (3 + 12) */

/* A processed vertex before it is put on the screen: clip space, colours as numbers, texture coordinates, the user clip planes' distances. The
 * clipper interpolates all of it. */
typedef struct DlClipVertex {
  float position[4];
  float diffuse[4], specular[4];
  float tex[DL_TEXTURE_SETS][4];
  float user[6];
  uint32_t clip;           /* the clip codes */
} DlClipVertex;

/* What processing some vertices did: the union and intersection of their clip codes and the rectangle the ones on the screen cover. */
typedef struct DlProcessResult {
  uint32_t clip_union, clip_intersection, count;
  int has_extent;
  float x1, y1, x2, y2;
} DlProcessResult;

DL_API void dl_result_init(DlProcessResult *r) {
  dl_zero(r, sizeof *r);
  r->clip_intersection = DL_CLIP_ALL;
}
DL_API void dl_result_add(DlProcessResult *r, const DlTransform *t, const DlClipVertex *v) {
  r->clip_union |= v->clip;
  r->clip_intersection &= v->clip;
  ++r->count;
  if (v->clip || !(v->position[3] > 0.0f)) return;      /* (the extents are of what is on the screen) */
  float sx, sy, sz, rhw;
  dl_to_screen(&t->viewport, v->position, &sx, &sy, &sz, &rhw);
  if (!r->has_extent) {
    r->x1 = r->x2 = sx;
    r->y1 = r->y2 = sy;
    r->has_extent = 1;
    return;
  }
  if (sx < r->x1) r->x1 = sx;
  if (sx > r->x2) r->x2 = sx;
  if (sy < r->y1) r->y1 = sy;
  if (sy > r->y2) r->y2 = sy;
}

DL_API int32_t dl_floor_int(float x) {
  if (!(x > -2147483520.0f)) return INT32_MIN;
  if (!(x < 2147483520.0f)) return INT32_MAX;
  int32_t i = (int32_t)x;
  if ((float)i > x) --i;
  return i;
}
DL_API int32_t dl_ceil_int(float x) {
  if (!(x > -2147483520.0f)) return INT32_MIN;
  if (!(x < 2147483520.0f)) return INT32_MAX;
  int32_t i = (int32_t)x;
  if ((float)i < x) ++i;
  return i;
}

/* Merges a processing result into an execute buffer's (or a D3D7 ProcessVertices') status: clip union bits OR, intersection bits AND (they start
 * all set, D3DSTATUS_DEFAULT), and, with `update_extents`, the extents grown to cover the vertices (a rectangle whose x2 < x1 counts as empty,
 * like the 2048, 2048, 0, 0 programs start with). */
DL_API void dl_status_merge(DlStatus *s, const DlProcessResult *r, int update_extents) {
  if (!r->count) return;
  s->dwStatus |= r->clip_union & DL_STATUS_CLIPUNIONALL;
  s->dwStatus &= ~DL_STATUS_CLIPINTERSECTIONALL | (r->clip_intersection << 12);
  if (!update_extents || !r->has_extent) return;
  const int32_t x1 = dl_floor_int(r->x1), y1 = dl_floor_int(r->y1), x2 = dl_ceil_int(r->x2), y2 = dl_ceil_int(r->y2);
  if (s->drExtent.x2 < s->drExtent.x1 || s->drExtent.y2 < s->drExtent.y1) {
    s->drExtent.x1 = x1;
    s->drExtent.y1 = y1;
    s->drExtent.x2 = x2;
    s->drExtent.y2 = y2;
    return;
  }
  if (x1 < s->drExtent.x1) s->drExtent.x1 = x1;
  if (y1 < s->drExtent.y1) s->drExtent.y1 = y1;
  if (x2 > s->drExtent.x2) s->drExtent.x2 = x2;
  if (y2 > s->drExtent.y2) s->drExtent.y2 = y2;
}

/* Transforms (and lights, with `light` and a lighting that is enabled) one vertex. `normal`, `diffuse` and `specular` may be NULL (a vertex without
 * them: white, and black with alpha 1). `tex` holds `sets` sets of four numbers. Vertex fog, when the fog mode is not NONE, goes into the specular
 * alpha - in Direct3D 1-3 only for vertices that are lit (fog is part of their lighting module: D3DLVERTEX carry their own); otherwise that alpha
 * is the vertex's own. */
DL_API void dl_process_vertex(const DlTransform *t, const DlLighting *l, int light, const float position[3], const float *normal, const uint32_t *diffuse,
                              const uint32_t *specular, const float (*tex)[4], unsigned sets, DlClipVertex *out) {
  dl_zero(out, sizeof *out);
  dl_transform_point(&t->total, position, out->position);
  float world[4];
  dl_transform_point(&t->world, position, world);
  for (unsigned i = 0; i < 6; ++i)
    out->user[i] = t->user_plane[i][0] * world[0] + t->user_plane[i][1] * world[1] + t->user_plane[i][2] * world[2] + t->user_plane[i][3];
  out->clip = dl_clip_code(out->position, out->user, t->user_planes);
  float vd[4] = {1.0f, 1.0f, 1.0f, 1.0f}, vs[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  if (diffuse) dl_color_to_float(*diffuse, vd);
  if (specular) dl_color_to_float(*specular, vs);
  if (light && l->enabled) {
    float lit_specular[3];
    dl_light_vertex(t, l, position, normal, diffuse ? vd : NULL, specular ? vs : NULL, out->diffuse, lit_specular);
    for (int i = 0; i < 3; ++i) out->specular[i] = lit_specular[i];
    out->specular[3] = vs[3];
  } else {
    for (int i = 0; i < 4; ++i) {
      out->diffuse[i] = vd[i];
      out->specular[i] = vs[i];
    }
  }
  if (l->fog_mode != DL_FOG_NONE && (!l->legacy || (light && l->enabled))) {      /* (Direct3D 1-3: fog is the lighting module's) */
    float eye[4];
    dl_transform_point(&t->world_view, position, eye);
    const float distance = l->range_fog ? ffl_sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]) : eye[2];
    out->specular[3] = dl_fog_factor(l->fog_mode, l->fog_start, l->fog_end, l->fog_density, distance);
  }
  for (unsigned s = 0; s < sets && s < DL_TEXTURE_SETS; ++s)
    for (int i = 0; i < 4; ++i) out->tex[s][i] = tex[s][i];
}

/* A processed vertex as a D3DTLVERTEX (the first texture set). */
DL_API void dl_tl_from_clip(const DlTransform *t, const DlClipVertex *v, DlTlVertex *out) {
  dl_to_screen(&t->viewport, v->position, &out->sx, &out->sy, &out->sz, &out->rhw);
  out->color = dl_color_from_float(v->diffuse);
  out->specular = dl_color_from_float(v->specular);
  out->tu = v->tex[0][0];
  out->tv = v->tex[0][1];
}
/* A D3DTLVERTEX as a processed vertex (what is already on the screen, for the clipper). */
DL_API void dl_clip_from_tl(const DlTransform *t, const DlTlVertex *v, DlClipVertex *out) {
  dl_zero(out, sizeof *out);
  dl_from_screen(&t->viewport, v->sx, v->sy, v->sz, v->rhw, out->position);
  dl_color_to_float(v->color, out->diffuse);
  dl_color_to_float(v->specular, out->specular);
  out->tex[0][0] = v->tu;
  out->tex[0][1] = v->tv;
  out->tex[0][3] = 1.0f;
}

/* D3DOP_PROCESSVERTICES for one record: the execute buffer's vertices from wStart (D3DVERTEX for TRANSFORMLIGHT, D3DLVERTEX for TRANSFORM,
 * D3DTLVERTEX for COPY) into `dst` (the device's processed vertices, D3DTLVERTEX each) from wDest, and their clip-space forms into `clip_out` (the same
 * number of entries as dst has vertices; NULL if the device does not clip). The status takes the clip codes (not for COPY: those are on the screen
 * already) and, with UPDATEEXTENTS, the extents. NOCOLOR leaves the colours of the destination as they were. DL_RANGE if the record does not fit. */
DL_API int dl_execute_process(const DlProcessVertices *rec, DlConstBytes buffer, const DlExecuteData *data, DlBytes dst, DlClipVertex *clip_out,
                              size_t clip_count, const DlTransform *t, const DlLighting *l, DlStatus *status) {
  const uint32_t op = rec->dwFlags & DL_PROCESSVERTICES_OPMASK;
  if (op > DL_PROCESSVERTICES_COPY) return DL_BAD;
  if (rec->dwCount > data->dwVertexCount || rec->wStart > data->dwVertexCount - rec->dwCount) return DL_RANGE;
  const size_t dst_vertices = dst.size / DL_VERTEX_SIZE;
  if (rec->dwCount > dst_vertices || rec->wDest > dst_vertices - rec->dwCount) return DL_RANGE;
  if (clip_out && (rec->dwCount > clip_count || rec->wDest > clip_count - rec->dwCount)) return DL_RANGE;
  DlProcessResult result;
  dl_result_init(&result);
  for (uint32_t i = 0; i < rec->dwCount; ++i) {
    uint8_t raw[DL_VERTEX_SIZE];
    if (!dl_execute_vertex(buffer, data, rec->wStart + i, raw)) return DL_RANGE;
    const size_t at = ((size_t)rec->wDest + i) * DL_VERTEX_SIZE;
    DlTlVertex tl;
    DlClipVertex cv;
    if (op == DL_PROCESSVERTICES_COPY) {
      dl_read(dl_const_bytes(raw, sizeof raw), 0, &tl, sizeof tl);
      dl_clip_from_tl(t, &tl, &cv);
    } else {
      DlConstBytes in = dl_const_bytes(raw, sizeof raw);
      float tex[1][4] = {{0.0f, 0.0f, 0.0f, 1.0f}};
      if (op == DL_PROCESSVERTICES_TRANSFORMLIGHT) {
        DlVertex v;
        dl_read(in, 0, &v, sizeof v);
        const float p[3] = {v.x, v.y, v.z}, n[3] = {v.nx, v.ny, v.nz};
        tex[0][0] = v.tu;
        tex[0][1] = v.tv;
        dl_process_vertex(t, l, 1, p, n, NULL, NULL, (const float (*)[4])tex, 1, &cv);
      } else {
        DlLVertex v;
        dl_read(in, 0, &v, sizeof v);
        const float p[3] = {v.x, v.y, v.z};
        tex[0][0] = v.tu;
        tex[0][1] = v.tv;
        dl_process_vertex(t, l, 0, p, NULL, &v.color, &v.specular, (const float (*)[4])tex, 1, &cv);
      }
      dl_result_add(&result, t, &cv);
      dl_tl_from_clip(t, &cv, &tl);
    }
    if (rec->dwFlags & DL_PROCESSVERTICES_NOCOLOR) {
      DlTlVertex old;
      if (dl_read(dl_const_bytes(dst.data, dst.size), at, &old, sizeof old)) {
        tl.color = old.color;
        tl.specular = old.specular;
        dl_color_to_float(old.color, cv.diffuse);
        dl_color_to_float(old.specular, cv.specular);
      }
    }
    dl_write(dst, at, &tl, sizeof tl);
    if (clip_out) clip_out[rec->wDest + i] = cv;
  }
  if (op != DL_PROCESSVERTICES_COPY) dl_status_merge(status, &result, (rec->dwFlags & DL_PROCESSVERTICES_UPDATEEXTENTS) != 0);
  return DL_OK;
}

/* ==== clipping ======================================================================================================================================= */

/* How far inside plane `p` (0 left, 1 right, 2 top, 3 bottom, 4 front, 5 back, 6 .. 11 the user planes) a vertex is: inside if >= 0. */
DL_API float dl_plane_distance(const DlClipVertex *v, unsigned p) {
  const float *q = v->position;
  switch (p) {
  case 0: return q[0] + q[3];
  case 1: return q[3] - q[0];
  case 2: return q[3] - q[1];
  case 3: return q[1] + q[3];
  case 4: return q[2];
  case 5: return q[3] - q[2];
  default: return p < 12 ? v->user[p - 6] : 0.0f;
  }
}
DL_API void dl_clip_lerp(const DlClipVertex *a, const DlClipVertex *b, float s, DlClipVertex *out) {
  for (int i = 0; i < 4; ++i) {
    out->position[i] = a->position[i] + (b->position[i] - a->position[i]) * s;
    out->diffuse[i] = a->diffuse[i] + (b->diffuse[i] - a->diffuse[i]) * s;
    out->specular[i] = a->specular[i] + (b->specular[i] - a->specular[i]) * s;
  }
  for (unsigned t = 0; t < DL_TEXTURE_SETS; ++t)
    for (int i = 0; i < 4; ++i) out->tex[t][i] = a->tex[t][i] + (b->tex[t][i] - a->tex[t][i]) * s;
  for (int i = 0; i < 6; ++i) out->user[i] = a->user[i] + (b->user[i] - a->user[i]) * s;
  out->clip = 0;
}
/* Clips a convex polygon (`count` vertices of `poly`, which has room for DL_CLIP_MAX) against the planes whose clip codes are in `planes` (DL_CLIP_*
 * bits; DL_CLIP_ALL for the frustum and every user plane the transform enables), Sutherland-Hodgman in clip space. The vertices that are left, in
 * order (a fan), are back in `poly`; their number is returned (0: nothing is left). Each new vertex is worked out from the edge's inside end, so
 * two polygons that share an edge are cut at the same point. */
DL_API unsigned dl_clip_polygon(DlClipVertex poly[DL_CLIP_MAX], unsigned count, uint32_t planes) {
  DlClipVertex scratch[DL_CLIP_MAX];
  if (count > DL_CLIP_MAX) count = DL_CLIP_MAX;
  for (unsigned p = 0; p < 12 && count; ++p) {
    if (!(planes & (1u << p))) continue;
    unsigned out = 0;
    for (unsigned i = 0; i < count; ++i) {
      const DlClipVertex *a = &poly[i], *b = &poly[(i + 1) % count];
      const float da = dl_plane_distance(a, p), db = dl_plane_distance(b, p);
      const int ina = da >= 0.0f, inb = db >= 0.0f;
      if (ina && out < DL_CLIP_MAX) scratch[out++] = *a;
      if (ina != inb && out < DL_CLIP_MAX) {
        if (ina) dl_clip_lerp(a, b, da / (da - db), &scratch[out]);
        else dl_clip_lerp(b, a, db / (db - da), &scratch[out]);
        ++out;
      }
    }
    for (unsigned i = 0; i < out; ++i) poly[i] = scratch[i];
    count = out < 3 ? 0 : out;
  }
  for (unsigned i = 0; i < count; ++i) poly[i].clip = 0;
  return count;
}
/* What to do with a triangle whose vertices have these clip codes: 0 draw it as it is, 1 nothing of it is inside one plane (drop it), 2 clip it. */
DL_API int dl_triangle_clip_class(uint32_t a, uint32_t b, uint32_t c) {
  if (!(a | b | c)) return 0;
  if (a & b & c) return 1;
  return 2;
}
/* Clips the line a - b: 0 if nothing is left, else 1 with the ends in a and b. */
DL_API int dl_clip_line(DlClipVertex *a, DlClipVertex *b, uint32_t planes) {
  for (unsigned p = 0; p < 12; ++p) {
    if (!(planes & (1u << p))) continue;
    const float da = dl_plane_distance(a, p), db = dl_plane_distance(b, p);
    if (da < 0.0f && db < 0.0f) return 0;
    if (da < 0.0f) {
      DlClipVertex n;
      dl_clip_lerp(b, a, db / (db - da), &n);
      *a = n;
    } else if (db < 0.0f) {
      DlClipVertex n;
      dl_clip_lerp(a, b, da / (da - db), &n);
      *b = n;
    }
  }
  a->clip = b->clip = 0;
  return 1;
}
/* The planes a transform clips against: the six of the frustum and the user planes it enables. */
DL_API uint32_t dl_clip_planes(const DlTransform *t) { return 0x3Fu | ((t->user_planes & 0x3Fu) << 6); }

/* ==== IDirect3DViewport::TransformVertices ================================================================================================== */

/* `count` vertices of `in` (dwInSize bytes each, x y z first) into `out` (dwOutSize bytes each: sx sy sz rhw, then the bytes after the input's first 16
 * as they are) and, with D3DTRANSFORM_CLIPPED, D3DHVERTEX into `hout` - Wine's reading of what Windows does: the clip codes are worked out either way;
 * a clipped vertex gets x y z w of clip space in `out` instead of screen numbers; hout has the clip codes and x y z before the viewport's clip space;
 * `*offscreen` is the clip codes every vertex has (D3DTRANSFORM_CLIPPED), 0 otherwise. The transform's viewport and clip space are the viewport's. */
DL_API DlResult dl_transform_vertices(const DlTransform *t, uint32_t count, DlConstBytes in, uint32_t in_stride, DlBytes out, uint32_t out_stride, DlHVertex *hout,
                                      size_t hout_count, uint32_t flags, uint32_t *offscreen) {
  if (!(flags & (DL_TRANSFORM_CLIPPED | DL_TRANSFORM_UNCLIPPED))) return DL_DDERR_INVALIDPARAMS;
  const int clipped = (flags & DL_TRANSFORM_CLIPPED) != 0;
  if (in_stride < 12 || out_stride < 16 || (clipped && (!hout || hout_count < count))) return DL_DDERR_INVALIDPARAMS;
  if ((uint64_t)count * in_stride > in.size + (uint64_t)in_stride - 12u || (uint64_t)count * out_stride > out.size + (uint64_t)out_stride - 16u)
    return DL_DDERR_INVALIDPARAMS;      /* (the last vertex needs only what is read or written of it) */
  uint32_t all = clipped ? DL_CLIP_ALL : 0;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = (size_t)i * in_stride, to = (size_t)i * out_stride;
    float p[3], c[4];
    dl_read(in, at, p, sizeof p);
    dl_transform_point(&t->total, p, c);
    if (clipped) {
      const uint32_t code = dl_clip_code(c, NULL, 0);
      DlHVertex *h = &hout[i];
      h->flags = code;
      h->hx = (t->clip.m[0][0] > 0.0f || t->clip.m[0][0] < 0.0f) ? (c[0] - t->clip.m[3][0] * c[3]) / t->clip.m[0][0] : 0.0f;
      h->hy = (t->clip.m[1][1] > 0.0f || t->clip.m[1][1] < 0.0f) ? (c[1] - t->clip.m[3][1] * c[3]) / t->clip.m[1][1] : 0.0f;
      h->hz = (t->clip.m[2][2] > 0.0f || t->clip.m[2][2] < 0.0f) ? (c[2] - t->clip.m[3][2] * c[3]) / t->clip.m[2][2] : 0.0f;
      all &= code;
      if (code) {
        dl_write(out, to, c, sizeof c);
        continue;
      }
    }
    float s[4];
    dl_to_screen(&t->viewport, c, &s[0], &s[1], &s[2], &s[3]);
    dl_write(out, to, s, sizeof s);
    const uint32_t rest_in = in_stride > 16 ? in_stride - 16 : 0, rest_out = out_stride - 16;
    uint32_t rest = rest_in < rest_out ? rest_in : rest_out;
    if (rest > 16) rest = 16;      /* (four numbers of the vertex's other data are copied, no more - Wine's test_transform_vertices) */
    if (rest) dl_copy(out, to + 16, in, at + 16, rest);
  }
  if (offscreen) *offscreen = all;
  return DL_OK;
}

/* ==== FVF codes ======================================================================================================================================= */

/* Where each part of a vertex of an FVF code is (offsets in bytes; -1: it has none). */
typedef struct DlFvfLayout {
  uint32_t stride;
  uint32_t position_size;          /* 12 (XYZ), 16 (XYZRHW), 12 + 4 n (XYZBn) */
  int32_t blend, normal, reserved, diffuse, specular;
  unsigned blend_weights, textures;
  int32_t tex[DL_TEXTURE_SETS];
  unsigned tex_size[DL_TEXTURE_SETS];   /* numbers in each set: 1 .. 4 */
} DlFvfLayout;

/* The numbers of texture set `i` (D3DFVF_TEXCOORDSIZEn: 0 two, 1 three, 2 four, 3 one). */
DL_API unsigned dl_fvf_tex_size(uint32_t fvf, unsigned i) {
  static const unsigned sizes[4] = {2, 3, 4, 1};
  return sizes[(fvf >> (16 + 2 * i)) & 3u];
}
/* Whether Direct3D `version` (3: IDirect3DDevice3 and IDirect3D3's vertex buffers; 7) takes the code: a position, no reserved bits (bit 0 and
 * 0xF000), at most eight texture sets, texture coordinate sizes (D3DFVF_TEXCOORDSIZEn, Direct3D 7 only) for no set the vertex does not have.
 * D3DFVF_RESERVED1 is allowed: it is D3DLVERTEX's reserved word. */
DL_API int dl_fvf_valid(uint32_t fvf, unsigned version) {
  if (fvf & (DL_FVF_RESERVED0 | DL_FVF_RESERVED2)) return 0;
  if (!(fvf & DL_FVF_POSITION_MASK)) return 0;
  const unsigned textures = (fvf & DL_FVF_TEXCOUNT_MASK) >> DL_FVF_TEXCOUNT_SHIFT;
  if (textures > DL_TEXTURE_SETS) return 0;
  if (version < 7 && (fvf >> 16)) return 0;          /* (texture coordinate sizes are Direct3D 7's) */
  if (textures < DL_TEXTURE_SETS && (fvf >> (16 + 2 * textures))) return 0;      /* (sizes of sets the vertex does not have) */
  return 1;
}
/* The layout of a valid FVF code (dl_fvf_valid); 0 for one that is not. */
DL_API int dl_fvf_layout(uint32_t fvf, DlFvfLayout *out) {
  dl_zero(out, sizeof *out);
  out->blend = out->normal = out->reserved = out->diffuse = out->specular = -1;
  for (unsigned i = 0; i < DL_TEXTURE_SETS; ++i) out->tex[i] = -1;
  if (!dl_fvf_valid(fvf, 7)) return 0;
  uint32_t at = 0;
  const uint32_t position = fvf & DL_FVF_POSITION_MASK;
  if (position == DL_FVF_XYZRHW) {
    out->position_size = 16;
  } else {
    out->blend_weights = position == DL_FVF_XYZ ? 0u : (position - DL_FVF_XYZRHW) / 2u;      /* (XYZB1 6 .. XYZB5 0xE) */
    out->position_size = 12 + 4 * out->blend_weights;
    if (out->blend_weights) out->blend = 12;
  }
  at = out->position_size;
  if (fvf & DL_FVF_NORMAL) { out->normal = (int32_t)at; at += 12; }
  if (fvf & DL_FVF_RESERVED1) { out->reserved = (int32_t)at; at += 4; }
  if (fvf & DL_FVF_DIFFUSE) { out->diffuse = (int32_t)at; at += 4; }
  if (fvf & DL_FVF_SPECULAR) { out->specular = (int32_t)at; at += 4; }
  out->textures = (fvf & DL_FVF_TEXCOUNT_MASK) >> DL_FVF_TEXCOUNT_SHIFT;
  for (unsigned i = 0; i < out->textures; ++i) {
    out->tex[i] = (int32_t)at;
    out->tex_size[i] = dl_fvf_tex_size(fvf, i);
    at += 4 * out->tex_size[i];
  }
  out->stride = at;
  return 1;
}
/* The size of a vertex of a code (0: not valid). */
DL_API uint32_t dl_fvf_stride(uint32_t fvf) {
  DlFvfLayout l;
  return dl_fvf_layout(fvf, &l) ? l.stride : 0;
}
/* The FVF code of a D3DVERTEXTYPE (0: none, D3DERR_INVALIDVERTEXTYPE - Wine returns DDERR_INVALIDPARAMS). */
DL_API uint32_t dl_fvf_of_vertex_type(uint32_t type) {
  switch (type) {
  case DL_VT_VERTEX: return DL_FVF_VERTEX;
  case DL_VT_LVERTEX: return DL_FVF_LVERTEX;
  case DL_VT_TLVERTEX: return DL_FVF_TLVERTEX;
  default: return 0;
  }
}

/* Direct3D 9's D3DVERTEXELEMENT9 (member for member, as d3d8's D8Element). */
typedef struct DlVertexElement { uint16_t stream, offset; uint8_t type, method, usage, usage_index; } DlVertexElement;
enum { DL_DECLTYPE_FLOAT1 = 0, DL_DECLTYPE_FLOAT2, DL_DECLTYPE_FLOAT3, DL_DECLTYPE_FLOAT4, DL_DECLTYPE_D3DCOLOR, DL_DECLTYPE_UNUSED = 17 };
enum { DL_USAGE_POSITION = 0, DL_USAGE_BLENDWEIGHT = 1, DL_USAGE_NORMAL = 3, DL_USAGE_TEXCOORD = 5, DL_USAGE_POSITIONT = 9, DL_USAGE_COLOR = 10 };
#define DL_MAX_ELEMENTS 14u        /* position, weights, normal, two colours, eight sets and the end */

/* A Direct3D 9 vertex declaration for an FVF code (with the end element: stream 0xFF, type UNUSED): what d3d9 is given instead of the code, since
 * Direct3D 9 has no D3DFVF_RESERVED1 (its 0x20 is D3DFVF_PSIZE) - the reserved four bytes of a D3DLVERTEX are skipped. The number of elements,
 * the end one included; 0 for a code that is not valid. */
DL_API unsigned dl_fvf_elements(uint32_t fvf, DlVertexElement out[DL_MAX_ELEMENTS]) {
  DlFvfLayout l;
  if (!dl_fvf_layout(fvf, &l)) return 0;
  unsigned n = 0;
#define DL_ELEMENT(off, ty, us, ix) do { out[n].stream = 0; out[n].offset = (uint16_t)(off); out[n].type = (uint8_t)(ty); out[n].method = 0; \
                                         out[n].usage = (uint8_t)(us); out[n].usage_index = (uint8_t)(ix); ++n; } while (0)
  if ((fvf & DL_FVF_POSITION_MASK) == DL_FVF_XYZRHW) DL_ELEMENT(0, DL_DECLTYPE_FLOAT4, DL_USAGE_POSITIONT, 0);
  else DL_ELEMENT(0, DL_DECLTYPE_FLOAT3, DL_USAGE_POSITION, 0);
  if (l.blend_weights) DL_ELEMENT(12, DL_DECLTYPE_FLOAT1 + (l.blend_weights > 4 ? 3 : l.blend_weights - 1), DL_USAGE_BLENDWEIGHT, 0);
  if (l.normal >= 0) DL_ELEMENT(l.normal, DL_DECLTYPE_FLOAT3, DL_USAGE_NORMAL, 0);
  if (l.diffuse >= 0) DL_ELEMENT(l.diffuse, DL_DECLTYPE_D3DCOLOR, DL_USAGE_COLOR, 0);
  if (l.specular >= 0) DL_ELEMENT(l.specular, DL_DECLTYPE_D3DCOLOR, DL_USAGE_COLOR, 1);
  for (unsigned i = 0; i < l.textures; ++i) DL_ELEMENT(l.tex[i], DL_DECLTYPE_FLOAT1 + l.tex_size[i] - 1, DL_USAGE_TEXCOORD, i);
#undef DL_ELEMENT
  /* (an XYZB5 vertex has five weights: the fifth is not an element of Direct3D 9's FLOAT4 - d3d9's vertex blending uses at most three anyway) */
  out[n].stream = 0xFF;
  out[n].offset = 0;
  out[n].type = DL_DECLTYPE_UNUSED;
  out[n].method = 0;
  out[n].usage = 0;
  out[n].usage_index = 0;
  return n + 1;
}

/* The number of primitives `vertices` vertices of a D3DPRIMITIVETYPE make (0 for a type that is not one, or too few vertices). */
DL_API uint32_t dl_primitive_count(uint32_t type, uint32_t vertices) {
  switch (type) {
  case DL_PT_POINTLIST: return vertices;
  case DL_PT_LINELIST: return vertices / 2;
  case DL_PT_LINESTRIP: return vertices >= 2 ? vertices - 1 : 0;
  case DL_PT_TRIANGLELIST: return vertices / 3;
  case DL_PT_TRIANGLESTRIP: case DL_PT_TRIANGLEFAN: return vertices >= 3 ? vertices - 2 : 0;
  default: return 0;
  }
}

/* The strided data of DrawPrimitiveStrided / DrawIndexedPrimitiveStrided (one D3DDP_PTRSTRIDE per part of the code) as interleaved vertices of the
 * code: `count` vertices from `first` into `out` (count * stride bytes). Each part is read from its pointer; `available` is how many vertices from
 * the start each pointer may be read for (the device's bound: the vertices the call says it draws). The position stream holds the blend weights
 * too. DDERR_INVALIDPARAMS for a code that is not valid, a part with no pointer, or an `out` too small. */
DL_API DlResult dl_gather_strided(uint32_t fvf, const DlStridedData *data, uint32_t first, uint32_t count, uint32_t available, DlBytes out) {
  DlFvfLayout l;
  if (!dl_fvf_layout(fvf, &l) || !data) return DL_DDERR_INVALIDPARAMS;
  if (first > available || count > available - first || (uint64_t)count * l.stride > out.size) return DL_DDERR_INVALIDPARAMS;
  struct Part { const DlPtrStride *from; int32_t at; uint32_t size; } parts[4 + DL_TEXTURE_SETS];
  unsigned n = 0;
  parts[n].from = &data->position; parts[n].at = 0; parts[n].size = l.position_size; ++n;
  if (l.normal >= 0) { parts[n].from = &data->normal; parts[n].at = l.normal; parts[n].size = 12; ++n; }
  if (l.diffuse >= 0) { parts[n].from = &data->diffuse; parts[n].at = l.diffuse; parts[n].size = 4; ++n; }
  if (l.specular >= 0) { parts[n].from = &data->specular; parts[n].at = l.specular; parts[n].size = 4; ++n; }
  for (unsigned i = 0; i < l.textures; ++i) { parts[n].from = &data->textureCoords[i]; parts[n].at = l.tex[i]; parts[n].size = 4 * l.tex_size[i]; ++n; }
  for (unsigned p = 0; p < n; ++p) {
    if (!parts[p].from->lpvData) return DL_DDERR_INVALIDPARAMS;
    /* (the program says each pointer has `available` vertices of its stride: its view; a stride of 0 repeats one vertex) */
    const uint32_t stride = parts[p].from->dwStride;
    const size_t extent = available ? (size_t)(available - 1) * stride + parts[p].size : 0;
    const DlConstBytes from = dl_const_bytes(parts[p].from->lpvData, extent);
    for (uint32_t i = 0; i < count; ++i)
      if (!dl_copy(out, (size_t)i * l.stride + (size_t)parts[p].at, from, (size_t)(first + i) * stride, parts[p].size)) return DL_DDERR_INVALIDPARAMS;
  }
  if (l.reserved >= 0)
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t zero = 0;
      dl_write(out, (size_t)i * l.stride + (size_t)l.reserved, &zero, 4);
    }
  return DL_OK;
}

/* Processes `count` vertices of an FVF code (`in`, count * stride bytes) - Direct3D 2/3 DrawPrimitive of untransformed vertices, and vertex buffers'
 * ProcessVertices - into clip-space vertices. They are lit if `light` (D3DVOP_LIGHT / not D3DDP_DONOTLIGHT), the lighting is enabled and they have
 * a normal. Transformed vertices (XYZRHW) come back as dl_clip_from_tl makes them. `result` takes the clip codes and extents. */
DL_API DlResult dl_process_fvf(const DlTransform *t, const DlLighting *l, int light, uint32_t fvf, DlConstBytes in, uint32_t count, DlClipVertex *out, size_t out_count,
                               DlProcessResult *result) {
  DlFvfLayout lay;
  if (!dl_fvf_layout(fvf, &lay)) return DL_D3DERR_INVALIDVERTEXFORMAT;
  if (count > out_count || (uint64_t)count * lay.stride > in.size) return DL_DDERR_INVALIDPARAMS;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t base = (size_t)i * lay.stride;
    float p[4] = {0, 0, 0, 1}, n[3] = {0, 0, 0}, tex[DL_TEXTURE_SETS][4];
    uint32_t diffuse = 0xFFFFFFFFu, specular = 0xFF000000u;
    const int rhw = (fvf & DL_FVF_POSITION_MASK) == DL_FVF_XYZRHW;      /* (XYZB1 is 16 bytes too: a position and a weight) */
    dl_read(in, base, p, rhw ? 16 : 12);
    if (lay.normal >= 0) dl_read(in, base + (size_t)lay.normal, n, 12);
    if (lay.diffuse >= 0) diffuse = dl_u32(in, base + (size_t)lay.diffuse);
    if (lay.specular >= 0) specular = dl_u32(in, base + (size_t)lay.specular);
    for (unsigned s = 0; s < DL_TEXTURE_SETS; ++s) {
      tex[s][0] = tex[s][1] = tex[s][2] = 0.0f;
      tex[s][3] = 1.0f;
      if (s < lay.textures) dl_read(in, base + (size_t)lay.tex[s], tex[s], 4 * lay.tex_size[s]);
    }
    if (rhw) {
      DlTlVertex tl;
      tl.sx = p[0]; tl.sy = p[1]; tl.sz = p[2]; tl.rhw = p[3];
      tl.color = diffuse; tl.specular = specular; tl.tu = tex[0][0]; tl.tv = tex[0][1];
      dl_clip_from_tl(t, &tl, &out[i]);
      for (unsigned s = 0; s < lay.textures; ++s)
        for (int k = 0; k < 4; ++k) out[i].tex[s][k] = tex[s][k];
      continue;
    }
    dl_process_vertex(t, l, light && lay.normal >= 0, p, lay.normal >= 0 ? n : NULL, lay.diffuse >= 0 ? &diffuse : NULL, lay.specular >= 0 ? &specular : NULL,
                      (const float (*)[4])tex, lay.textures, &out[i]);
    if (result) dl_result_add(result, t, &out[i]);
  }
  return DL_OK;
}
/* The code processed vertices are drawn with: XYZRHW, both colours and the input's texture sets with their sizes. */
DL_API uint32_t dl_output_fvf(uint32_t fvf) { return DL_FVF_XYZRHW | DL_FVF_DIFFUSE | DL_FVF_SPECULAR | (fvf & DL_FVF_TEXCOUNT_MASK) | (fvf & 0xFFFF0000u); }
/* Processed vertices as vertices of dl_output_fvf(fvf): `count` from `in` into `out`. */
DL_API DlResult dl_project_fvf(const DlTransform *t, uint32_t fvf, const DlClipVertex *in, uint32_t count, DlBytes out) {
  DlFvfLayout lay;
  if (!dl_fvf_layout(dl_output_fvf(fvf), &lay)) return DL_D3DERR_INVALIDVERTEXFORMAT;
  if ((uint64_t)count * lay.stride > out.size) return DL_DDERR_INVALIDPARAMS;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t base = (size_t)i * lay.stride;
    float s[4];
    dl_to_screen(&t->viewport, in[i].position, &s[0], &s[1], &s[2], &s[3]);
    dl_write(out, base, s, 16);
    const uint32_t d = dl_color_from_float(in[i].diffuse), sp = dl_color_from_float(in[i].specular);
    dl_write(out, base + (size_t)lay.diffuse, &d, 4);
    dl_write(out, base + (size_t)lay.specular, &sp, 4);
    for (unsigned k = 0; k < lay.textures; ++k) dl_write(out, base + (size_t)lay.tex[k], in[i].tex[k], 4 * lay.tex_size[k]);
  }
  return DL_OK;
}

/* ==== caps =============================================================================================================================================== */

/* The fields of Direct3D 9's D3DCAPS9 the descriptions are made from (copied from IDirect3DDevice9::GetDeviceCaps). `color_key`: the device
 * implements colour keyed textures (D3DRENDERSTATE_COLORKEYENABLE), which Direct3D 1-6 programs look for as D3DPTEXTURECAPS_TRANSPARENCY. */
typedef struct DlCaps9 {
  uint32_t DevCaps, PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps, TextureFilterCaps,
      TextureAddressCaps, LineCaps, MaxTextureWidth, MaxTextureHeight, MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
  float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
  uint32_t StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps, MaxActiveLights, MaxUserClipPlanes,
      MaxVertexBlendMatrices;
  int color_key;
} DlCaps9;

enum { DL_DEVICE_NONE = 0, DL_DEVICE_RAMP, DL_DEVICE_RGB, DL_DEVICE_MMX, DL_DEVICE_HAL, DL_DEVICE_TNLHAL, DL_DEVICE_REF, DL_DEVICE_NULL };

static const DlGuid dl_guid_ramp = {0xF2086B20u, 0x259F, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
static const DlGuid dl_guid_rgb = {0xA4665C60u, 0x2673, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
static const DlGuid dl_guid_hal = {0x84E63DE0u, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};
static const DlGuid dl_guid_mmx = {0x881949A1u, 0xD6F3, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};
static const DlGuid dl_guid_ref = {0x50936643u, 0x13E9, 0x11D1, {0x89, 0xAA, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};
static const DlGuid dl_guid_tnlhal = {0xF5049E78u, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};
static const DlGuid dl_guid_null = {0x8767DF22u, 0xBACC, 0x11D1, {0x89, 0x69, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};

DL_API int dl_guid_equal(const DlGuid *a, const DlGuid *b) {
  if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3) return 0;
  for (int i = 0; i < 8; ++i)
    if (a->data4[i] != b->data4[i]) return 0;
  return 1;
}
DL_API const DlGuid *dl_device_guid(int kind) {
  switch (kind) {
  case DL_DEVICE_RAMP: return &dl_guid_ramp;
  case DL_DEVICE_RGB: return &dl_guid_rgb;
  case DL_DEVICE_MMX: return &dl_guid_mmx;
  case DL_DEVICE_HAL: return &dl_guid_hal;
  case DL_DEVICE_TNLHAL: return &dl_guid_tnlhal;
  case DL_DEVICE_REF: return &dl_guid_ref;
  case DL_DEVICE_NULL: return &dl_guid_null;
  default: return NULL;
  }
}
/* The device a GUID names (CreateDevice, QueryInterface of a surface, FindDevice), DL_DEVICE_NONE for another GUID. */
DL_API int dl_device_kind(const DlGuid *guid) {
  for (int k = DL_DEVICE_RAMP; k <= DL_DEVICE_NULL; ++k)
    if (dl_guid_equal(guid, dl_device_guid(k))) return k;
  return DL_DEVICE_NONE;
}
/* The name and description EnumDevices gives (Windows' names: programs look for "Direct3D HAL" and "RGB Emulation"). */
DL_API const char *dl_device_name(int kind) {
  switch (kind) {
  case DL_DEVICE_RAMP: return "Ramp Emulation";
  case DL_DEVICE_RGB: return "RGB Emulation";
  case DL_DEVICE_MMX: return "MMX Emulation";
  case DL_DEVICE_HAL: return "Direct3D HAL";
  case DL_DEVICE_TNLHAL: return "Direct3D T&L HAL";
  case DL_DEVICE_REF: return "Reference Rasterizer";
  case DL_DEVICE_NULL: return "Null device";
  default: return "";
  }
}
DL_API const char *dl_device_description(int kind) {
  switch (kind) {
  case DL_DEVICE_RAMP: return "Microsoft Direct3D Mono(Ramp) Software Emulation";
  case DL_DEVICE_RGB: return "Microsoft Direct3D RGB Software Emulation";
  case DL_DEVICE_MMX: return "Microsoft Direct3D MMX Software Emulation";
  case DL_DEVICE_HAL: return "Microsoft Direct3D Hardware acceleration through Direct3D HAL";
  case DL_DEVICE_TNLHAL: return "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device";
  case DL_DEVICE_REF: return "Microsoft Reference Rasterizer";
  case DL_DEVICE_NULL: return "Microsoft Null Device";
  default: return "";
  }
}
/* The devices EnumDevices of IDirect3D `version` (1, 2, 3, 7) gives, in order (Wine's): 1 and 2 Ramp, RGB, HAL; 3 RGB, HAL; 7 RGB, HAL, T&L HAL.
 * DL_DEVICE_NONE past the last. */
DL_API int dl_enum_device(unsigned version, unsigned index) {
  static const int v12[] = {DL_DEVICE_RAMP, DL_DEVICE_RGB, DL_DEVICE_HAL}, v3[] = {DL_DEVICE_RGB, DL_DEVICE_HAL}, v7[] = {DL_DEVICE_RGB, DL_DEVICE_HAL, DL_DEVICE_TNLHAL};
  if (version == 1 || version == 2) return index < 3 ? v12[index] : DL_DEVICE_NONE;
  if (version == 3) return index < 2 ? v3[index] : DL_DEVICE_NONE;
  if (version == 7) return index < 3 ? v7[index] : DL_DEVICE_NONE;
  return DL_DEVICE_NONE;
}
/* The devices CreateDevice of each version accepts: what it enumerates and MMX (Direct3D 1-3; MMX and Ramp are drawn as RGB, as DirectX 6 and later
 * do), and the reference rasterizer; T&L HAL only for 7, Ramp not for 3 and 7 (DDERR_INVALIDPARAMS / E_NOINTERFACE is the device layer's choice). */
DL_API int dl_device_creatable(unsigned version, int kind) {
  switch (kind) {
  case DL_DEVICE_HAL: case DL_DEVICE_RGB: case DL_DEVICE_REF: return 1;
  case DL_DEVICE_MMX: return version < 7;
  case DL_DEVICE_RAMP: return version <= 2;
  case DL_DEVICE_TNLHAL: return version == 7;
  default: return 0;
  }
}
/* The size of the D3DDEVICEDESC of each version. */
DL_API uint32_t dl_device_desc_size(unsigned version) {
  return version <= 1 ? DL_DEVICEDESC_V1_SIZE : version == 2 ? DL_DEVICEDESC_V2_SIZE : DL_DEVICEDESC_V3_SIZE;
}

/* Bits of the caps (d3dcaps.h). */
#define DL_DEVCAPS_DRAWPRIMITIVES2EX 0x00008000u
#define DL_DEVCAPS_HWTRANSFORMANDLIGHT 0x00010000u
#define DL_DEVCAPS_HWRASTERIZATION 0x00080000u
#define DL_DEVCAPS7_MASK 0x000BFFFFu
#define DL_PRASTERCAPS_ANTIALIASEDGES 0x00001000u
#define DL_PRASTERCAPS_ZBIAS 0x00004000u
#define DL_PRASTERCAPS_FOGVERTEX 0x00000080u
#define DL_PRASTERCAPS7_MASK (0x003FFFFFu & ~DL_PRASTERCAPS_ZBIAS)
#define DL_PRASTERCAPS9_DEPTHBIAS 0x04000000u
#define DL_LINECAPS9_ANTIALIAS 0x00000020u
#define DL_PTEXTURECAPS_PERSPECTIVE 0x001u
#define DL_PTEXTURECAPS_POW2 0x002u
#define DL_PTEXTURECAPS_TRANSPARENCY 0x008u
#define DL_PTEXTURECAPS_NONPOW2CONDITIONAL 0x100u
#define DL_PTEXTURECAPS7_MASK 0x1DFFu
#define DL_PTFILTERCAPS_NEAREST 0x01u
#define DL_PTFILTERCAPS_LINEAR 0x02u
#define DL_PTFILTERCAPS_MIPNEAREST 0x04u
#define DL_PTFILTERCAPS_MIPLINEAR 0x08u
#define DL_PTFILTERCAPS_LINEARMIPNEAREST 0x10u
#define DL_PTFILTERCAPS_LINEARMIPLINEAR 0x20u
#define DL_PTFILTERCAPS_MINFPOINT 0x00000100u
#define DL_PTFILTERCAPS_MINFLINEAR 0x00000200u
#define DL_PTFILTERCAPS_MIPFPOINT 0x00010000u
#define DL_PTFILTERCAPS_MIPFLINEAR 0x00020000u
#define DL_PTFILTERCAPS_MAGFPOINT 0x01000000u
#define DL_PTFILTERCAPS_MAGFLINEAR 0x02000000u
#define DL_PTFILTERCAPS7_MASK 0x1F03073Fu
#define DL_PSHADECAPS7_MASK 0x001FFFFFu
#define DL_PTBLENDCAPS_ALL 0xFFu          /* DECAL .. ADD: every texture map blend (dl_texture_map_blend) */
#define DL_VTXPCAPS_VERTEXFOG 0x04u
#define DL_FVFCAPS7_MASK 0x0008FFFFu
#define DL_DDBD_16 0x400u
#define DL_DDBD_24 0x200u
#define DL_DDBD_32 0x100u
#define DL_DD_ALL 0x7FFu                  /* D3DDD_COLORMODEL .. D3DDD_MAXVERTEXCOUNT */
#define DL_LIGHTCAPS_POINT 1u
#define DL_LIGHTCAPS_SPOT 2u
#define DL_LIGHTCAPS_DIRECTIONAL 4u
#define DL_LIGHTCAPS_PARALLELPOINT 8u
#define DL_LIGHTINGMODEL_RGB 1u
#define DL_TRANSFORMCAPS_CLIP 1u

DL_API uint16_t dl_clamp16(uint32_t x, uint32_t limit) { return (uint16_t)(x < limit ? x : limit); }

/* The primitive caps (lines and triangles alike) out of Direct3D 9's, with the masks Wine uses, the legacy bits that follow from Direct3D 9's
 * (ZBIAS from DEPTHBIAS, ANTIALIASEDGES from antialiased lines, the D3DFILTER_* filters from the MIN/MAG/MIP ones, flat shading wherever Gouraud
 * is, conditional powers of two for a device with none) and the texture map blends this implementation does. */
DL_API void dl_prim_caps(const DlCaps9 *c, DlPrimCaps *p) {
  dl_zero(p, sizeof *p);
  p->dwSize = sizeof *p;
  p->dwMiscCaps = c->PrimitiveMiscCaps & 0x7Fu;
  p->dwRasterCaps = c->RasterCaps & DL_PRASTERCAPS7_MASK;
  if (c->RasterCaps & DL_PRASTERCAPS9_DEPTHBIAS) p->dwRasterCaps |= DL_PRASTERCAPS_ZBIAS;
  if (c->LineCaps & DL_LINECAPS9_ANTIALIAS) p->dwRasterCaps |= DL_PRASTERCAPS_ANTIALIASEDGES;
  p->dwZCmpCaps = c->ZCmpCaps & 0xFFu;
  p->dwSrcBlendCaps = c->SrcBlendCaps & 0x1FFFu;
  p->dwDestBlendCaps = c->DestBlendCaps & 0x1FFFu;
  p->dwAlphaCmpCaps = c->AlphaCmpCaps & 0xFFu;
  uint32_t shade = c->ShadeCaps & DL_PSHADECAPS7_MASK;
  if (shade & 0x00000008u) shade |= 0x00000002u;      /* COLORGOURAUDRGB: COLORFLATRGB */
  if (shade & 0x00000200u) shade |= 0x00000080u;      /* SPECULARGOURAUDRGB: SPECULARFLATRGB */
  if (shade & 0x00004000u) shade |= 0x00001000u;      /* ALPHAGOURAUDBLEND: ALPHAFLATBLEND */
  if (shade & 0x00080000u) shade |= 0x00040000u;      /* FOGGOURAUD: FOGFLAT */
  p->dwShadeCaps = shade;
  p->dwTextureCaps = c->TextureCaps & DL_PTEXTURECAPS7_MASK;
  if (!(p->dwTextureCaps & DL_PTEXTURECAPS_POW2)) p->dwTextureCaps |= DL_PTEXTURECAPS_POW2 | DL_PTEXTURECAPS_NONPOW2CONDITIONAL;
  if (c->color_key) p->dwTextureCaps |= DL_PTEXTURECAPS_TRANSPARENCY;
  uint32_t filter = c->TextureFilterCaps & DL_PTFILTERCAPS7_MASK;
  const int min_point = (filter & DL_PTFILTERCAPS_MINFPOINT) != 0, min_linear = (filter & DL_PTFILTERCAPS_MINFLINEAR) != 0;
  const int mag_point = (filter & DL_PTFILTERCAPS_MAGFPOINT) != 0, mag_linear = (filter & DL_PTFILTERCAPS_MAGFLINEAR) != 0;
  const int mip_point = (filter & DL_PTFILTERCAPS_MIPFPOINT) != 0, mip_linear = (filter & DL_PTFILTERCAPS_MIPFLINEAR) != 0;
  if (min_point && mag_point) filter |= DL_PTFILTERCAPS_NEAREST;
  if (min_linear && mag_linear) filter |= DL_PTFILTERCAPS_LINEAR;
  if (min_point && mip_point) filter |= DL_PTFILTERCAPS_MIPNEAREST;
  if (min_linear && mip_point) filter |= DL_PTFILTERCAPS_MIPLINEAR;
  if (min_point && mip_linear) filter |= DL_PTFILTERCAPS_LINEARMIPNEAREST;
  if (min_linear && mip_linear) filter |= DL_PTFILTERCAPS_LINEARMIPLINEAR;
  p->dwTextureFilterCaps = filter;
  p->dwTextureBlendCaps = DL_PTBLENDCAPS_ALL;
  p->dwTextureAddressCaps = c->TextureAddressCaps & 0x1Fu;
  p->dwStippleWidth = 32;
  p->dwStippleHeight = 32;
}

/* D3DDEVICEDESC7 of a device (Wine's ddraw_get_d3dcaps, then EnumDevices' per-device caps: RGB has no hardware transform, no DrawPrimitives2Ex
 * and no hardware rasterization; HAL no hardware transform; T&L HAL all of it). */
DL_API void dl_device_desc7(const DlCaps9 *c, int kind, DlDeviceDesc7 *d) {
  dl_zero(d, sizeof *d);
  d->dwDevCaps = c->DevCaps & DL_DEVCAPS7_MASK;
  if (kind == DL_DEVICE_RGB || kind == DL_DEVICE_MMX || kind == DL_DEVICE_RAMP || kind == DL_DEVICE_REF)
    d->dwDevCaps &= ~(DL_DEVCAPS_HWTRANSFORMANDLIGHT | DL_DEVCAPS_DRAWPRIMITIVES2EX | DL_DEVCAPS_HWRASTERIZATION);
  else if (kind == DL_DEVICE_HAL)
    d->dwDevCaps &= ~DL_DEVCAPS_HWTRANSFORMANDLIGHT;
  dl_prim_caps(c, &d->dpcLineCaps);
  d->dpcTriCaps = d->dpcLineCaps;
  d->dwDeviceRenderBitDepth = DL_DDBD_16 | DL_DDBD_24 | DL_DDBD_32;
  d->dwDeviceZBufferBitDepth = DL_DDBD_16 | DL_DDBD_24;
  d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
  d->dwMaxTextureWidth = c->MaxTextureWidth;
  d->dwMaxTextureHeight = c->MaxTextureHeight;
  d->dwMaxTextureRepeat = c->MaxTextureRepeat;
  d->dwMaxTextureAspectRatio = c->MaxTextureAspectRatio;
  d->dwMaxAnisotropy = c->MaxAnisotropy;
  d->dvGuardBandLeft = c->GuardBandLeft;
  d->dvGuardBandTop = c->GuardBandTop;
  d->dvGuardBandRight = c->GuardBandRight;
  d->dvGuardBandBottom = c->GuardBandBottom;
  d->dvExtentsAdjust = c->ExtentsAdjust;
  d->dwStencilCaps = c->StencilCaps & 0xFFu;
  d->dwFVFCaps = c->FVFCaps & DL_FVFCAPS7_MASK;
  d->dwTextureOpCaps = c->TextureOpCaps & 0x00FFFFFFu;
  d->wMaxTextureBlendStages = dl_clamp16(c->MaxTextureBlendStages, 0xFFFF);
  d->wMaxSimultaneousTextures = dl_clamp16(c->MaxSimultaneousTextures, 0xFFFF);
  d->dwMaxActiveLights = c->MaxActiveLights;
  d->dvMaxVertexW = c->MaxVertexW;
  const DlGuid *g = dl_device_guid(kind);
  d->deviceGUID = g ? *g : dl_guid_tnlhal;
  d->wMaxUserClipPlanes = dl_clamp16(c->MaxUserClipPlanes, 32);
  d->wMaxVertexBlendMatrices = dl_clamp16(c->MaxVertexBlendMatrices, 0xFFFF);
  d->dwVertexProcessingCaps = c->VertexProcessingCaps & 0x3Bu;
  if (c->RasterCaps & DL_PRASTERCAPS_FOGVERTEX) d->dwVertexProcessingCaps |= DL_VTXPCAPS_VERTEXFOG;
}

/* The D3DDEVICEDESC of the Direct3D 7 one (Wine's ddraw_d3dcaps1_from_7), with dwSize the size of `version`'s: what lies past it is zero. */
DL_API void dl_device_desc_from7(const DlDeviceDesc7 *d7, unsigned version, DlDeviceDesc *d) {
  dl_zero(d, sizeof *d);
  d->dwSize = DL_DEVICEDESC_V3_SIZE;
  d->dwFlags = DL_DD_ALL;
  d->dcmColorModel = DL_COLOR_RGB;
  d->dwDevCaps = d7->dwDevCaps;
  d->dtcTransformCaps.dwSize = sizeof d->dtcTransformCaps;
  d->dtcTransformCaps.dwCaps = DL_TRANSFORMCAPS_CLIP;
  d->bClipping = 1;
  d->dlcLightingCaps.dwSize = sizeof d->dlcLightingCaps;
  d->dlcLightingCaps.dwCaps = DL_LIGHTCAPS_DIRECTIONAL | DL_LIGHTCAPS_PARALLELPOINT | DL_LIGHTCAPS_POINT | DL_LIGHTCAPS_SPOT;
  d->dlcLightingCaps.dwLightingModel = DL_LIGHTINGMODEL_RGB;
  d->dlcLightingCaps.dwNumLights = d7->dwMaxActiveLights;
  d->dpcLineCaps = d7->dpcLineCaps;
  d->dpcTriCaps = d7->dpcTriCaps;
  d->dwDeviceRenderBitDepth = d7->dwDeviceRenderBitDepth;
  d->dwDeviceZBufferBitDepth = d7->dwDeviceZBufferBitDepth;
  d->dwMaxBufferSize = 0;
  d->dwMaxVertexCount = 2048;
  d->dwMinTextureWidth = d7->dwMinTextureWidth;
  d->dwMinTextureHeight = d7->dwMinTextureHeight;
  d->dwMaxTextureWidth = d7->dwMaxTextureWidth;
  d->dwMaxTextureHeight = d7->dwMaxTextureHeight;
  d->dwMinStippleWidth = d->dwMinStippleHeight = 1;
  d->dwMaxStippleWidth = d->dwMaxStippleHeight = 32;
  d->dwMaxTextureRepeat = d7->dwMaxTextureRepeat;
  d->dwMaxTextureAspectRatio = d7->dwMaxTextureAspectRatio;
  d->dwMaxAnisotropy = d7->dwMaxAnisotropy;
  d->dvGuardBandLeft = d7->dvGuardBandLeft;
  d->dvGuardBandTop = d7->dvGuardBandTop;
  d->dvGuardBandRight = d7->dvGuardBandRight;
  d->dvGuardBandBottom = d7->dvGuardBandBottom;
  d->dvExtentsAdjust = d7->dvExtentsAdjust;
  d->dwStencilCaps = d7->dwStencilCaps;
  d->dwFVFCaps = d7->dwFVFCaps;
  d->dwTextureOpCaps = d7->dwTextureOpCaps;
  d->wMaxTextureBlendStages = d7->wMaxTextureBlendStages;
  d->wMaxSimultaneousTextures = d7->wMaxSimultaneousTextures;
  /* what the version's structure does not have */
  const uint32_t size = dl_device_desc_size(version);
  if (size < DL_DEVICEDESC_V3_SIZE) {
    DlBytes all = dl_bytes(d, sizeof *d);
    for (size_t i = size; i < all.size; ++i) all.data[i] = 0;
  }
  d->dwSize = size;
}

/* The hardware and software descriptions EnumDevices / FindDevice of `version` give for a device (Wine's enum_devices_d3d3):
 *   software devices (Ramp, RGB, MMX, reference): the hardware one with no colour model, no flags, no hardware transform / DrawPrimitives2Ex /
 *     rasterization and no POW2 / NONPOW2CONDITIONAL / PERSPECTIVE texture caps; the software one complete but for the same device caps, with the
 *     colour model D3DCOLOR_MONO for Ramp;
 *   HAL: the hardware one complete; the software one with no colour model, no hardware transform or DrawPrimitives2Ex, and without those texture caps.
 * (Rollcage tells the two apart by POW2: in the software caps of an emulation device, in the hardware caps of the HAL.) */
DL_API void dl_device_descs(const DlCaps9 *c, int kind, unsigned version, DlDeviceDesc *hal, DlDeviceDesc *hel) {
  DlDeviceDesc7 d7;
  dl_device_desc7(c, DL_DEVICE_TNLHAL, &d7);
  DlDeviceDesc base;
  dl_device_desc_from7(&d7, 3, &base);
  *hal = base;
  *hel = base;
  const uint32_t texture_strip = DL_PTEXTURECAPS_POW2 | DL_PTEXTURECAPS_NONPOW2CONDITIONAL | DL_PTEXTURECAPS_PERSPECTIVE;
  if (kind == DL_DEVICE_HAL || kind == DL_DEVICE_TNLHAL) {
    hel->dpcLineCaps.dwTextureCaps &= ~texture_strip;
    hel->dpcTriCaps.dwTextureCaps &= ~texture_strip;
    hel->dcmColorModel = 0;
    hel->dwDevCaps &= ~(DL_DEVCAPS_HWTRANSFORMANDLIGHT | DL_DEVCAPS_DRAWPRIMITIVES2EX);
  } else {
    hal->dpcLineCaps.dwTextureCaps &= ~texture_strip;
    hal->dpcTriCaps.dwTextureCaps &= ~texture_strip;
    hal->dcmColorModel = 0;
    hal->dwFlags = 0;
    const uint32_t sw = DL_DEVCAPS_HWTRANSFORMANDLIGHT | DL_DEVCAPS_DRAWPRIMITIVES2EX | DL_DEVCAPS_HWRASTERIZATION;
    hal->dwDevCaps &= ~sw;
    hel->dwDevCaps &= ~sw;
    hel->dcmColorModel = kind == DL_DEVICE_RAMP ? DL_COLOR_MONO : DL_COLOR_RGB;
  }
  /* the version's size (and nothing past it) */
  const uint32_t size = dl_device_desc_size(version);
  DlBytes a = dl_bytes(hal, sizeof *hal), b = dl_bytes(hel, sizeof *hel);
  for (size_t i = size; i < a.size; ++i) a.data[i] = b.data[i] = 0;
  hal->dwSize = hel->dwSize = size;
}

/* The devices FindDevice of each version looks through, in order (Wine's lists less its own GUID). */
DL_API int dl_find_device_candidate(unsigned version, unsigned index) {
  static const int v1[] = {DL_DEVICE_HAL, DL_DEVICE_RGB, DL_DEVICE_RAMP}, v2[] = {DL_DEVICE_HAL, DL_DEVICE_MMX, DL_DEVICE_RGB, DL_DEVICE_RAMP}, v3[] = {DL_DEVICE_HAL, DL_DEVICE_RGB};
  if (version <= 1) return index < 3 ? v1[index] : DL_DEVICE_NONE;
  if (version == 2) return index < 4 ? v2[index] : DL_DEVICE_NONE;
  return index < 2 ? v3[index] : DL_DEVICE_NONE;
}

#define DL_FDS_COLORMODEL 0x0001u
#define DL_FDS_GUID 0x0002u
#define DL_FDS_HARDWARE 0x0004u
#define DL_FDS_TRIANGLES 0x0008u
#define DL_FDS_LINES 0x0010u
#define DL_FDS_MISCCAPS 0x0020u
#define DL_FDS_RASTERCAPS 0x0040u
#define DL_FDS_ZCMPCAPS 0x0080u
#define DL_FDS_ALPHACMPCAPS 0x0100u
#define DL_FDS_SRCBLENDCAPS 0x0200u
#define DL_FDS_DSTBLENDCAPS 0x0400u
#define DL_FDS_SHADECAPS 0x0800u
#define DL_FDS_TEXTURECAPS 0x1000u
#define DL_FDS_TEXTUREFILTERCAPS 0x2000u
#define DL_FDS_TEXTUREBLENDCAPS 0x4000u
#define DL_FDS_TEXTUREADDRESSCAPS 0x8000u

DL_API int dl_caps_have(uint32_t have, uint32_t want) { return (have & want) == want; }

/* IDirect3D/2/3::FindDevice: the first device of the version's list that the search allows (its GUID; hardware or not; its colour model; the
 * primitive caps of lines or triangles - triangles unless D3DFDS_LINES - containing every bit asked for in the fields the flags name), written into
 * the program's D3DFINDDEVICERESULT (`result`, of the size its dwSize says: the full one or the Direct3D 1 / 2 one, whose descriptions are shorter).
 * DDERR_INVALIDPARAMS for a search or result of another size; DDERR_NOTFOUND when no device matches. */
DL_API DlResult dl_find_device(const DlCaps9 *c, unsigned version, const DlFindDeviceSearch *search, DlBytes result) {
  if (!search || search->dwSize != DL_FINDDEVICESEARCH_SIZE || result.size < 4) return DL_DDERR_INVALIDPARAMS;
  const uint32_t size = dl_u32(dl_const_bytes(result.data, result.size), 0);
  uint32_t slot;
  if (size == DL_FINDDEVICERESULT_SIZE) slot = DL_DEVICEDESC_V3_SIZE;
  else if (size == DL_FINDDEVICERESULT_V2_SIZE) slot = DL_DEVICEDESC_V2_SIZE;
  else if (size == DL_FINDDEVICERESULT_V1_SIZE) slot = DL_DEVICEDESC_V1_SIZE;
  else return DL_DDERR_INVALIDPARAMS;
  if (result.size < size) return DL_DDERR_INVALIDPARAMS;
  for (unsigned i = 0;; ++i) {
    const int kind = dl_find_device_candidate(version, i);
    if (kind == DL_DEVICE_NONE) return DL_DDERR_NOTFOUND;
    if ((search->dwFlags & DL_FDS_GUID) && !dl_guid_equal(&search->guid, dl_device_guid(kind))) continue;
    const int hardware = kind == DL_DEVICE_HAL;
    if ((search->dwFlags & DL_FDS_HARDWARE) && (search->bHardware != 0) != hardware) continue;
    DlDeviceDesc hal, hel;
    dl_device_descs(c, kind, version, &hal, &hel);
    const DlDeviceDesc *own = hardware ? &hal : &hel;
    if ((search->dwFlags & DL_FDS_COLORMODEL) && !(own->dcmColorModel & search->dcmColorModel)) continue;
    const DlPrimCaps *have = (search->dwFlags & DL_FDS_LINES) ? &own->dpcLineCaps : &own->dpcTriCaps;
    const DlPrimCaps *want = &search->dpcPrimCaps;
    const uint32_t f = search->dwFlags;
    if ((f & DL_FDS_MISCCAPS) && !dl_caps_have(have->dwMiscCaps, want->dwMiscCaps)) continue;
    if ((f & DL_FDS_RASTERCAPS) && !dl_caps_have(have->dwRasterCaps, want->dwRasterCaps)) continue;
    if ((f & DL_FDS_ZCMPCAPS) && !dl_caps_have(have->dwZCmpCaps, want->dwZCmpCaps)) continue;
    if ((f & DL_FDS_ALPHACMPCAPS) && !dl_caps_have(have->dwAlphaCmpCaps, want->dwAlphaCmpCaps)) continue;
    if ((f & DL_FDS_SRCBLENDCAPS) && !dl_caps_have(have->dwSrcBlendCaps, want->dwSrcBlendCaps)) continue;
    if ((f & DL_FDS_DSTBLENDCAPS) && !dl_caps_have(have->dwDestBlendCaps, want->dwDestBlendCaps)) continue;
    if ((f & DL_FDS_SHADECAPS) && !dl_caps_have(have->dwShadeCaps, want->dwShadeCaps)) continue;
    if ((f & DL_FDS_TEXTURECAPS) && !dl_caps_have(have->dwTextureCaps, want->dwTextureCaps)) continue;
    if ((f & DL_FDS_TEXTUREFILTERCAPS) && !dl_caps_have(have->dwTextureFilterCaps, want->dwTextureFilterCaps)) continue;
    if ((f & DL_FDS_TEXTUREBLENDCAPS) && !dl_caps_have(have->dwTextureBlendCaps, want->dwTextureBlendCaps)) continue;
    if ((f & DL_FDS_TEXTUREADDRESSCAPS) && !dl_caps_have(have->dwTextureAddressCaps, want->dwTextureAddressCaps)) continue;
    /* (as much of the version's description as the slot holds; its dwSize stays the version's - Wine's test_find_device) */
    const uint32_t desc_size = slot < hal.dwSize ? slot : hal.dwSize;
    dl_write(result, 4, dl_device_guid(kind), 16);
    dl_copy(result, 20, dl_const_bytes(&hal, sizeof hal), 0, desc_size);
    dl_copy(result, 20 + slot, dl_const_bytes(&hel, sizeof hel), 0, desc_size);
    for (uint32_t z = desc_size; z < slot; ++z) {      /* (a slot larger than the version's description: the rest is zero) */
      const uint8_t zero = 0;
      dl_write(result, 20 + z, &zero, 1);
      dl_write(result, 20 + slot + z, &zero, 1);
    }
    return DL_OK;
  }
}

/* ==== states ============================================================================================================================================== */

/* Direct3D 9's numbers the mapping produces. */
enum {
  DL_RS9_ZENABLE = 7, DL_RS9_FILLMODE = 8, DL_RS9_SHADEMODE = 9, DL_RS9_ZWRITEENABLE = 14, DL_RS9_ALPHATESTENABLE = 15, DL_RS9_LASTPIXEL = 16,
  DL_RS9_SRCBLEND = 19, DL_RS9_DESTBLEND = 20, DL_RS9_CULLMODE = 22, DL_RS9_ZFUNC = 23, DL_RS9_ALPHAREF = 24, DL_RS9_ALPHAFUNC = 25,
  DL_RS9_DITHERENABLE = 26, DL_RS9_ALPHABLENDENABLE = 27, DL_RS9_FOGENABLE = 28, DL_RS9_SPECULARENABLE = 29, DL_RS9_FOGCOLOR = 34,
  DL_RS9_FOGTABLEMODE = 35, DL_RS9_FOGSTART = 36, DL_RS9_FOGEND = 37, DL_RS9_FOGDENSITY = 38, DL_RS9_RANGEFOGENABLE = 48, DL_RS9_STENCILENABLE = 52,
  DL_RS9_TEXTUREFACTOR = 60, DL_RS9_WRAP0 = 128, DL_RS9_CLIPPING = 136, DL_RS9_LIGHTING = 137, DL_RS9_AMBIENT = 139, DL_RS9_FOGVERTEXMODE = 140,
  DL_RS9_COLORVERTEX = 141, DL_RS9_LOCALVIEWER = 142, DL_RS9_NORMALIZENORMALS = 143, DL_RS9_DIFFUSEMATERIALSOURCE = 145, DL_RS9_VERTEXBLEND = 151,
  DL_RS9_CLIPPLANEENABLE = 152, DL_RS9_MULTISAMPLEANTIALIAS = 161, DL_RS9_ANTIALIASEDLINEENABLE = 176, DL_RS9_DEPTHBIAS = 195
};
enum { DL_TSS9_COLOROP = 1, DL_TSS9_COLORARG1, DL_TSS9_COLORARG2, DL_TSS9_ALPHAOP, DL_TSS9_ALPHAARG1, DL_TSS9_ALPHAARG2, DL_TSS9_TEXCOORDINDEX = 11,
       DL_TSS9_TEXTURETRANSFORMFLAGS = 24, DL_TSS9_RESULTARG = 28, DL_TSS9_CONSTANT = 32 };
enum { DL_SAMP9_ADDRESSU = 1, DL_SAMP9_ADDRESSV, DL_SAMP9_ADDRESSW, DL_SAMP9_BORDERCOLOR, DL_SAMP9_MAGFILTER, DL_SAMP9_MINFILTER, DL_SAMP9_MIPFILTER,
       DL_SAMP9_MIPMAPLODBIAS, DL_SAMP9_MAXMIPLEVEL, DL_SAMP9_MAXANISOTROPY };
enum { DL_TEXF9_NONE = 0, DL_TEXF9_POINT, DL_TEXF9_LINEAR, DL_TEXF9_ANISOTROPIC, DL_TEXF9_PYRAMIDALQUAD = 6, DL_TEXF9_GAUSSIANQUAD = 7 };
enum { DL_TOP_DISABLE = 1, DL_TOP_SELECTARG1, DL_TOP_SELECTARG2, DL_TOP_MODULATE, DL_TOP_ADD = 7, DL_TOP_BLENDTEXTUREALPHA = 13 };
enum { DL_TA_DIFFUSE = 0, DL_TA_CURRENT = 1, DL_TA_TEXTURE = 2, DL_TA_CONSTANT = 6 };

/* The legacy render states (d3dtypes.h). */
enum {
  DL_RS_TEXTUREHANDLE = 1, DL_RS_ANTIALIAS = 2, DL_RS_TEXTUREADDRESS = 3, DL_RS_TEXTUREPERSPECTIVE = 4, DL_RS_WRAPU = 5, DL_RS_WRAPV = 6,
  DL_RS_ZENABLE = 7, DL_RS_LINEPATTERN = 10, DL_RS_MONOENABLE = 11, DL_RS_ROP2 = 12, DL_RS_PLANEMASK = 13, DL_RS_TEXTUREMAG = 17, DL_RS_TEXTUREMIN = 18,
  DL_RS_TEXTUREMAPBLEND = 21, DL_RS_ALPHABLENDENABLE = 27, DL_RS_SPECULARENABLE = 29, DL_RS_ZVISIBLE = 30, DL_RS_SUBPIXEL = 31, DL_RS_SUBPIXELX = 32,
  DL_RS_STIPPLEDALPHA = 33, DL_RS_STIPPLEENABLE = 39, DL_RS_EDGEANTIALIAS = 40, DL_RS_COLORKEYENABLE = 41, DL_RS_BORDERCOLOR = 43,
  DL_RS_TEXTUREADDRESSU = 44, DL_RS_TEXTUREADDRESSV = 45, DL_RS_MIPMAPLODBIAS = 46, DL_RS_ZBIAS = 47, DL_RS_ANISOTROPY = 49, DL_RS_FLUSHBATCH = 50,
  DL_RS_TRANSLUCENTSORTINDEPENDENT = 51, DL_RS_STIPPLEPATTERN00 = 64, DL_RS_STIPPLEPATTERN31 = 95, DL_RS_WRAP0 = 128, DL_RS_WRAP7 = 135,
  DL_RS_CLIPPING = 136, DL_RS_LIGHTING = 137, DL_RS_EXTENTS = 138, DL_RS_LOCALVIEWER = 142, DL_RS_NORMALIZENORMALS = 143,
  DL_RS_COLORKEYBLENDENABLE = 144, DL_RS_CLIPPLANEENABLE = 152
};
enum { DL_TBLEND_DECAL = 1, DL_TBLEND_MODULATE, DL_TBLEND_DECALALPHA, DL_TBLEND_MODULATEALPHA, DL_TBLEND_DECALMASK, DL_TBLEND_MODULATEMASK, DL_TBLEND_COPY,
       DL_TBLEND_ADD };
enum { DL_FILTER_NEAREST = 1, DL_FILTER_LINEAR, DL_FILTER_MIPNEAREST, DL_FILTER_MIPLINEAR, DL_FILTER_LINEARMIPNEAREST, DL_FILTER_LINEARMIPLINEAR };
enum { DL_LIGHTSTATE_MATERIAL = 1, DL_LIGHTSTATE_AMBIENT, DL_LIGHTSTATE_COLORMODEL, DL_LIGHTSTATE_FOGMODE, DL_LIGHTSTATE_FOGSTART, DL_LIGHTSTATE_FOGEND,
       DL_LIGHTSTATE_FOGDENSITY, DL_LIGHTSTATE_COLORVERTEX };
enum { DL_TSS_ADDRESS = 12, DL_TSS_ADDRESSU, DL_TSS_ADDRESSV, DL_TSS_BORDERCOLOR, DL_TSS_MAGFILTER, DL_TSS_MINFILTER, DL_TSS_MIPFILTER, DL_TSS_MIPMAPLODBIAS,
       DL_TSS_MAXMIPLEVEL, DL_TSS_MAXANISOTROPY, DL_TSS_TEXTURETRANSFORMFLAGS = 24 };
enum { DL_TFG_POINT = 1, DL_TFG_LINEAR, DL_TFG_FLATCUBIC, DL_TFG_GAUSSIANCUBIC, DL_TFG_ANISOTROPIC };
enum { DL_TFN_POINT = 1, DL_TFN_LINEAR, DL_TFN_ANISOTROPIC };
enum { DL_TFP_NONE = 1, DL_TFP_POINT, DL_TFP_LINEAR };
enum { DL_TRANSFORMSTATE_WORLD = 1, DL_TRANSFORMSTATE_VIEW, DL_TRANSFORMSTATE_PROJECTION, DL_TRANSFORMSTATE_WORLD1, DL_TRANSFORMSTATE_WORLD3 = 6,
       DL_TRANSFORMSTATE_TEXTURE0 = 16, DL_TRANSFORMSTATE_TEXTURE7 = 23 };

/* What the device does for a state: Direct3D 9 calls, in order. */
enum {
  DL_SET_RENDER = 1,       /* SetRenderState(state, value) */
  DL_SET_STAGE,            /* SetTextureStageState(index, state, value) */
  DL_SET_SAMPLER,          /* SetSamplerState(index, state, value) */
  DL_SET_TEXTURE,          /* SetTexture(index, the texture of legacy handle `value`; 0: none) - DDERR_INVALIDPARAMS from the device for a handle
                              that is not a texture */
  DL_SET_MATERIAL          /* the material of handle `value` becomes the device's (0: none) - DDERR_INVALIDPARAMS for a handle that is not one */
};
#define DL_MAX_STATE_OPS 16u
typedef struct DlStateOp { uint32_t target, index, state, value; } DlStateOp;
typedef struct DlStateOps { unsigned count; DlStateOp op[DL_MAX_STATE_OPS]; } DlStateOps;

DL_API void dl_ops_add(DlStateOps *o, uint32_t target, uint32_t index, uint32_t state, uint32_t value) {
  if (o->count >= DL_MAX_STATE_OPS) return;
  o->op[o->count].target = target;
  o->op[o->count].index = index;
  o->op[o->count].state = state;
  o->op[o->count].value = value;
  ++o->count;
}

/* What a mapping needs of the device's state. */
typedef struct DlStateContext {
  uint32_t wrap0;              /* D3DRENDERSTATE_WRAP0 as it is (WRAPU / WRAPV set its bits) */
  uint32_t texture_map_blend;  /* the current D3DRENDERSTATE_TEXTUREMAPBLEND (0: none set since stage states were set directly) */
  int texture;                 /* stage 0's texture: 0 none, 1 without alpha, 2 with alpha (or colour keyed) */
  uint32_t depth_bits;         /* of the depth buffer (D3DRENDERSTATE_ZBIAS), 24 when unknown */
} DlStateContext;

/* D3DRENDERSTATE_ZBIAS (0 .. 16) as Direct3D 9's depth bias: so many units of the depth buffer towards the eye (as d3d8.dll does it). */
DL_API uint32_t dl_depth_bias(uint32_t zbias, uint32_t depth_bits) {
  if (!zbias) return 0;
  if (!depth_bits || depth_bits > 32) depth_bits = 24;
  float unit = 1.0f;
  for (uint32_t i = 0; i < depth_bits; ++i) unit *= 0.5f;
  return dl_bits_of(-(float)zbias * unit);
}

/* D3DRENDERSTATE_TEXTUREMAPBLEND as stage 0 (and for MODULATEMASK stage 1) of Direct3D 9 (C: the colour, A: the alpha; Ct / At the texture, Cf / Af
 * the diffuse colour):
 *   DECAL, COPY      C = Ct, A = At
 *   MODULATE         C = Ct Cf, A = At if the texture has alpha, else Af (Wine's fixup_texture_alpha_op: the device maps it again when stage 0's
 *                    texture changes)
 *   MODULATEALPHA    C = Ct Cf, A = At Af
 *   DECALALPHA       C = Ct At + Cf (1 - At), A = Af
 *   ADD              C = Ct + Cf, A = Af
 *   DECALMASK        C = At ? Ct : Cf, A = Af            (the mask is the texture's alpha: exact for 1-bit alpha, the textures it is for)
 *   MODULATEMASK     C = At ? Ct Cf : Cf, A = Af         (stage 0 makes At ? Ct : 1 with its constant white, stage 1 multiplies by Cf)
 * With no texture on stage 0 every mode is C = Cf, A = Af. Leaving MODULATEMASK turns stage 1 off again. DDERR_INVALIDPARAMS for a value that is
 * not a mode. `o` is cleared first. */
DL_API DlResult dl_texture_map_blend(uint32_t mode, uint32_t previous, int texture, DlStateOps *o) {
  o->count = 0;
  if (mode < DL_TBLEND_DECAL || mode > DL_TBLEND_ADD) return DL_DDERR_INVALIDPARAMS;
  uint32_t cop = DL_TOP_SELECTARG1, c1 = DL_TA_TEXTURE, c2 = DL_TA_CURRENT, aop = DL_TOP_SELECTARG1, a1 = DL_TA_TEXTURE, a2 = DL_TA_CURRENT;
  switch (mode) {
  case DL_TBLEND_DECAL: case DL_TBLEND_COPY: break;
  case DL_TBLEND_MODULATE: cop = DL_TOP_MODULATE; aop = texture == 2 ? DL_TOP_SELECTARG1 : DL_TOP_SELECTARG2; break;
  case DL_TBLEND_MODULATEALPHA: cop = DL_TOP_MODULATE; aop = DL_TOP_MODULATE; break;
  case DL_TBLEND_DECALALPHA: case DL_TBLEND_DECALMASK: cop = DL_TOP_BLENDTEXTUREALPHA; aop = DL_TOP_SELECTARG2; break;
  case DL_TBLEND_ADD: cop = DL_TOP_ADD; aop = DL_TOP_SELECTARG2; break;
  case DL_TBLEND_MODULATEMASK: cop = DL_TOP_BLENDTEXTUREALPHA; c2 = DL_TA_CONSTANT; aop = DL_TOP_SELECTARG2; break;
  }
  if (!texture) {
    cop = DL_TOP_SELECTARG2;
    aop = DL_TOP_SELECTARG2;
    c2 = DL_TA_CURRENT;
  }
  dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_COLOROP, cop);
  dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_COLORARG1, c1);
  dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_COLORARG2, c2);
  dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAOP, aop);
  dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAARG1, a1);
  dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAARG2, a2);
  if (mode == DL_TBLEND_MODULATEMASK && texture) {
    dl_ops_add(o, DL_SET_STAGE, 0, DL_TSS9_CONSTANT, 0xFFFFFFFFu);
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_COLOROP, DL_TOP_MODULATE);
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_COLORARG1, DL_TA_CURRENT);
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_COLORARG2, DL_TA_DIFFUSE);
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_ALPHAOP, DL_TOP_SELECTARG1);
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_ALPHAARG1, DL_TA_CURRENT);
  } else if (previous == DL_TBLEND_MODULATEMASK) {
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_COLOROP, DL_TOP_DISABLE);
    dl_ops_add(o, DL_SET_STAGE, 1, DL_TSS9_ALPHAOP, DL_TOP_DISABLE);
  }
  return DL_OK;
}

/* D3DRENDERSTATE_TEXTUREMAG as stage 0's magnification filter (Wine's table: the MIP forms are their non-MIP filter; anything else point). */
DL_API uint32_t dl_mag_filter(uint32_t filter) {
  switch (filter) {
  case DL_FILTER_LINEAR: case DL_FILTER_MIPLINEAR: case DL_FILTER_LINEARMIPLINEAR: return DL_TEXF9_LINEAR;
  default: return DL_TEXF9_POINT;
  }
}
/* D3DRENDERSTATE_TEXTUREMIN as stage 0's minification and mip filters. */
DL_API void dl_min_filter(uint32_t filter, uint32_t *min, uint32_t *mip) {
  switch (filter) {
  case DL_FILTER_LINEAR: *min = DL_TEXF9_LINEAR; *mip = DL_TEXF9_NONE; break;
  case DL_FILTER_MIPNEAREST: *min = DL_TEXF9_POINT; *mip = DL_TEXF9_POINT; break;
  case DL_FILTER_MIPLINEAR: *min = DL_TEXF9_LINEAR; *mip = DL_TEXF9_POINT; break;
  case DL_FILTER_LINEARMIPNEAREST: *min = DL_TEXF9_POINT; *mip = DL_TEXF9_LINEAR; break;
  case DL_FILTER_LINEARMIPLINEAR: *min = DL_TEXF9_LINEAR; *mip = DL_TEXF9_LINEAR; break;
  default: *min = DL_TEXF9_POINT; *mip = DL_TEXF9_NONE; break;
  }
}
/* GetRenderState of TEXTUREMAG / TEXTUREMIN: the D3DFILTER_* Direct3D 9's filters say (what Wine reports). */
DL_API uint32_t dl_legacy_mag_filter(uint32_t mag) { return mag == DL_TEXF9_LINEAR ? DL_FILTER_LINEAR : DL_FILTER_NEAREST; }
DL_API uint32_t dl_legacy_min_filter(uint32_t min, uint32_t mip) {
  if (min == DL_TEXF9_LINEAR) return mip == DL_TEXF9_POINT ? DL_FILTER_MIPLINEAR : mip == DL_TEXF9_LINEAR ? DL_FILTER_LINEARMIPLINEAR : DL_FILTER_LINEAR;
  return mip == DL_TEXF9_POINT ? DL_FILTER_MIPNEAREST : mip == DL_TEXF9_LINEAR ? DL_FILTER_LINEARMIPNEAREST : DL_FILTER_NEAREST;
}

/* Whether the number is a render state of d3dtypes.h. */
DL_API int dl_render_state_known(uint32_t s) {
  if (s >= 1 && s <= 41) return 1;
  if (s >= 43 && s <= 60) return 1;
  if (s >= DL_RS_STIPPLEPATTERN00 && s <= DL_RS_STIPPLEPATTERN31) return 1;
  if (s >= DL_RS_WRAP0 && s <= 148) return 1;
  return s == 151 || s == DL_RS_CLIPPLANEENABLE;
}

/* SetRenderState of a device of Direct3D `version` (1: the execute buffer's STATERENDER; 2, 3, 7) as Direct3D 9 calls into `o` (cleared first):
 *
 *   the states Direct3D 9 has under the same number (ZENABLE .. TEXTUREFACTOR, WRAP0 .. 7, CLIPPING .. CLIPPLANEENABLE, ALPHABLENDENABLE alias
 *   BLENDENABLE) are passed on; EDGEANTIALIAS is ANTIALIASEDLINEENABLE, ANTIALIAS (any mode) MULTISAMPLEANTIALIAS, ZBIAS DEPTHBIAS (dl_depth_bias);
 *   TEXTUREADDRESS / U / V, BORDERCOLOR, TEXTUREMAG / MIN, MIPMAPLODBIAS and ANISOTROPY are sampler 0's; WRAPU / WRAPV bits of WRAP0;
 *   TEXTUREHANDLE binds stage 0's texture, TEXTUREMAPBLEND sets stage 0 (dl_texture_map_blend);
 *   states with no Direct3D 9 form are kept by the device for GetRenderState and do nothing here: TEXTUREPERSPECTIVE (Direct3D 9 is always
 *   perspective correct), LINEPATTERN, MONOENABLE, ROP2, PLANEMASK, SUBPIXEL / X, STIPPLEDALPHA, STIPPLEENABLE, STIPPLEPATTERN00 .. 31, FLUSHBATCH,
 *   TRANSLUCENTSORTINDEPENDENT, EXTENTS (the device's own: DrawPrimitive updates the extents unless D3DDP_DONOTUPDATEEXTENTS), and what the device
 *   carries out itself: ZVISIBLE, COLORKEYENABLE, COLORKEYBLENDENABLE (the caps report none of stippling, ROP2, line patterns);
 *   Direct3D 1-3: LIGHTING, NORMALIZENORMALS and LOCALVIEWER are taken and do nothing (Wine; GetRenderState reports 0xFFFFFFFF, see
 *   dl_render_state_get_override).
 *
 * DDERR_INVALIDPARAMS: state 0 or one of 256 and above (D3DSTATE_OVERRIDE_BIAS), TEXTUREHANDLE and TEXTUREMAPBLEND on Direct3D 7 (Wine), and a
 * TEXTUREMAPBLEND that is not a mode. Numbers below 256 that d3dtypes.h does not name are kept, as Windows' runtime passes them to the driver. */
DL_API DlResult dl_render_state(unsigned version, uint32_t state, uint32_t value, const DlStateContext *ctx, DlStateOps *o) {
  o->count = 0;
  if (!state || state >= 256) return DL_DDERR_INVALIDPARAMS;
  if (version >= 7 && (state == DL_RS_TEXTUREHANDLE || state == DL_RS_TEXTUREMAPBLEND)) return DL_DDERR_INVALIDPARAMS;
  if (version < 7 && (state == DL_RS_LIGHTING || state == DL_RS_NORMALIZENORMALS || state == DL_RS_LOCALVIEWER)) return DL_OK;
  switch (state) {
  case DL_RS_TEXTUREHANDLE: dl_ops_add(o, DL_SET_TEXTURE, 0, 0, value); return DL_OK;
  case DL_RS_TEXTUREMAPBLEND: return dl_texture_map_blend(value, ctx ? ctx->texture_map_blend : 0, ctx ? ctx->texture : 2, o);
  case DL_RS_ANTIALIAS: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_MULTISAMPLEANTIALIAS, value ? 1u : 0u); return DL_OK;
  case DL_RS_TEXTUREADDRESS:
    dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSU, value);
    dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSV, value);
    return DL_OK;
  case DL_RS_TEXTUREADDRESSU: dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSU, value); return DL_OK;
  case DL_RS_TEXTUREADDRESSV: dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSV, value); return DL_OK;
  case DL_RS_BORDERCOLOR: dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_BORDERCOLOR, value); return DL_OK;
  case DL_RS_TEXTUREMAG: dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_MAGFILTER, dl_mag_filter(value)); return DL_OK;
  case DL_RS_TEXTUREMIN: {
    uint32_t min, mip;
    dl_min_filter(value, &min, &mip);
    dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_MIPFILTER, mip);
    dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_MINFILTER, min);
    return DL_OK;
  }
  case DL_RS_MIPMAPLODBIAS: dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_MIPMAPLODBIAS, value); return DL_OK;
  case DL_RS_ANISOTROPY: dl_ops_add(o, DL_SET_SAMPLER, 0, DL_SAMP9_MAXANISOTROPY, value); return DL_OK;
  case DL_RS_WRAPU: case DL_RS_WRAPV: {
    const uint32_t bit = state == DL_RS_WRAPU ? 1u : 2u, wrap0 = ctx ? ctx->wrap0 : 0;
    dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_WRAP0, value ? (wrap0 | bit) : (wrap0 & ~bit));
    return DL_OK;
  }
  case DL_RS_EDGEANTIALIAS: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_ANTIALIASEDLINEENABLE, value ? 1u : 0u); return DL_OK;
  case DL_RS_ZBIAS: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_DEPTHBIAS, dl_depth_bias(value, ctx ? ctx->depth_bits : 24)); return DL_OK;
  case DL_RS_TEXTUREPERSPECTIVE: case DL_RS_LINEPATTERN: case DL_RS_MONOENABLE: case DL_RS_ROP2: case DL_RS_PLANEMASK: case DL_RS_ZVISIBLE:
  case DL_RS_SUBPIXEL: case DL_RS_SUBPIXELX: case DL_RS_STIPPLEDALPHA: case DL_RS_STIPPLEENABLE: case DL_RS_COLORKEYENABLE: case DL_RS_FLUSHBATCH:
  case DL_RS_TRANSLUCENTSORTINDEPENDENT: case DL_RS_EXTENTS: case DL_RS_COLORKEYBLENDENABLE:
    return DL_OK;
  default:
    break;
  }
  if (state >= DL_RS_STIPPLEPATTERN00 && state <= DL_RS_STIPPLEPATTERN31) return DL_OK;
  if (!dl_render_state_known(state)) return DL_OK;
  dl_ops_add(o, DL_SET_RENDER, 0, state, value);
  return DL_OK;
}
/* GetRenderState answers the device gives from what it stores rather than what was set: Direct3D 1-3's LIGHTING, NORMALIZENORMALS and LOCALVIEWER
 * report 0xFFFFFFFF (Wine). 1 with `*value` set for those. */
DL_API int dl_render_state_get_override(unsigned version, uint32_t state, uint32_t *value) {
  if (version < 7 && (state == DL_RS_LIGHTING || state == DL_RS_NORMALIZENORMALS || state == DL_RS_LOCALVIEWER)) {
    *value = 0xFFFFFFFFu;
    return 1;
  }
  return 0;
}

/* The value each render state starts with on a device of `version` (Direct3D 9's defaults and Wine's ddraw ones: COLORKEYENABLE on for Direct3D 1,
 * SPECULARENABLE on for 2, TEXTUREMAPBLEND MODULATE and NORMALIZENORMALS on before 7, ZENABLE as a depth buffer is attached). 0 for a state that is
 * not one (then `*value` is 0). */
DL_API int dl_render_state_default(unsigned version, uint32_t state, int has_depth, uint32_t *value) {
  uint32_t v = 0;
  int known = 1;
  switch (state) {
  case DL_RS_ZENABLE: v = has_depth ? 1u : 0u; break;
  case DL_RS_TEXTUREADDRESS: case DL_RS_TEXTUREADDRESSU: case DL_RS_TEXTUREADDRESSV: v = 1; break;      /* D3DTADDRESS_WRAP */
  case DL_RS_TEXTUREPERSPECTIVE: v = 1; break;
  case 8: v = 3; break;            /* FILLMODE: SOLID */
  case 9: v = 2; break;            /* SHADEMODE: GOURAUD */
  case DL_RS_ROP2: v = 13; break;  /* R2_COPYPEN */
  case DL_RS_PLANEMASK: v = 0xFFFFFFFFu; break;
  case 14: v = 1; break;           /* ZWRITEENABLE */
  case 16: v = 1; break;           /* LASTPIXEL */
  case DL_RS_TEXTUREMAG: case DL_RS_TEXTUREMIN: v = DL_FILTER_NEAREST; break;
  case 19: v = 2; break;           /* SRCBLEND: ONE */
  case 20: v = 1; break;           /* DESTBLEND: ZERO */
  case DL_RS_TEXTUREMAPBLEND: v = version < 7 ? DL_TBLEND_MODULATE : 0; known = version < 7; break;
  case 22: v = 3; break;           /* CULLMODE: CCW */
  case 23: v = 4; break;           /* ZFUNC: LESSEQUAL */
  case 25: v = 8; break;           /* ALPHAFUNC: ALWAYS */
  case DL_RS_SPECULARENABLE: v = version == 2 ? 1u : 0u; break;
  case 37: v = dl_bits_of(1.0f); break;     /* FOGEND */
  case 38: v = dl_bits_of(1.0f); break;     /* FOGDENSITY */
  case DL_RS_COLORKEYENABLE: v = version == 1 ? 1u : 0u; break;
  case DL_RS_ANISOTROPY: v = 1; break;
  case 53: case 54: case 55: v = 1; break;  /* STENCILFAIL / ZFAIL / PASS: KEEP */
  case 56: v = 8; break;                    /* STENCILFUNC: ALWAYS */
  case 58: case 59: case 60: v = 0xFFFFFFFFu; break;   /* STENCILMASK, STENCILWRITEMASK, TEXTUREFACTOR */
  case DL_RS_CLIPPING: v = 1; break;
  case DL_RS_LIGHTING: v = version < 7 ? 0xFFFFFFFFu : 1u; break;
  case 141: v = 1; break;          /* COLORVERTEX */
  case DL_RS_LOCALVIEWER: v = version < 7 ? 0xFFFFFFFFu : 1u; break;
  case DL_RS_NORMALIZENORMALS: v = version < 7 ? 0xFFFFFFFFu : 0u; break;
  case 145: v = 1; break;          /* DIFFUSEMATERIALSOURCE: COLOR1 */
  case 146: v = 2; break;          /* SPECULARMATERIALSOURCE: COLOR2 */
  default: known = state && state < 256 && dl_render_state_known(state); break;
  }
  if (version >= 7 && state == DL_RS_TEXTUREHANDLE) known = 0;
  *value = known ? v : 0;
  return known;
}

/* SetLightState (Direct3D 2 and 3, and the execute buffer's STATELIGHT): MATERIAL is the material of the handle (the device looks it up), AMBIENT
 * D3DRENDERSTATE_AMBIENT, FOGMODE FOGVERTEXMODE, FOGSTART / END / DENSITY the fog's, COLORVERTEX COLORVERTEX; COLORMODEL takes D3DCOLOR_RGB and
 * D3DCOLOR_MONO (drawn as RGB) and does nothing. DDERR_INVALIDPARAMS for state 0, one past COLORVERTEX, and another colour model. (The CPU lighting
 * of dl_light_vertex reads the same values from DlLighting, which the device keeps alongside.) GetLightState of COLORMODEL reports D3DCOLOR_RGB. */
DL_API DlResult dl_light_state(uint32_t state, uint32_t value, DlStateOps *o) {
  o->count = 0;
  switch (state) {
  case DL_LIGHTSTATE_MATERIAL: dl_ops_add(o, DL_SET_MATERIAL, 0, 0, value); return DL_OK;
  case DL_LIGHTSTATE_AMBIENT: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_AMBIENT, value); return DL_OK;
  case DL_LIGHTSTATE_COLORMODEL: return value == DL_COLOR_RGB || value == DL_COLOR_MONO ? DL_OK : DL_DDERR_INVALIDPARAMS;
  case DL_LIGHTSTATE_FOGMODE: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_FOGVERTEXMODE, value); return DL_OK;
  case DL_LIGHTSTATE_FOGSTART: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_FOGSTART, value); return DL_OK;
  case DL_LIGHTSTATE_FOGEND: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_FOGEND, value); return DL_OK;
  case DL_LIGHTSTATE_FOGDENSITY: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_FOGDENSITY, value); return DL_OK;
  case DL_LIGHTSTATE_COLORVERTEX: dl_ops_add(o, DL_SET_RENDER, 0, DL_RS9_COLORVERTEX, value); return DL_OK;
  default: return DL_DDERR_INVALIDPARAMS;
  }
}
/* A light state into the CPU lighting's settings (after dl_light_state took it). */
DL_API void dl_lighting_apply_light_state(DlLighting *l, uint32_t state, uint32_t value) {
  switch (state) {
  case DL_LIGHTSTATE_AMBIENT: { float c[4]; dl_color_to_float(value, c); l->ambient[0] = c[0]; l->ambient[1] = c[1]; l->ambient[2] = c[2]; break; }
  case DL_LIGHTSTATE_FOGMODE: l->fog_mode = value <= DL_FOG_LINEAR ? (int)value : DL_FOG_NONE; break;
  case DL_LIGHTSTATE_FOGSTART: l->fog_start = dl_float_of(value); break;
  case DL_LIGHTSTATE_FOGEND: l->fog_end = dl_float_of(value); break;
  case DL_LIGHTSTATE_FOGDENSITY: l->fog_density = dl_float_of(value); break;
  case DL_LIGHTSTATE_COLORVERTEX: l->color_vertex = value != 0; break;
  default: break;
  }
}

/* The value each light state starts with: no material, black ambient, RGB, no fog, fog 0 .. 1 with density 1, COLORVERTEX on. */
DL_API int dl_light_state_default(uint32_t state, uint32_t *value) {
  switch (state) {
  case DL_LIGHTSTATE_MATERIAL: case DL_LIGHTSTATE_AMBIENT: case DL_LIGHTSTATE_FOGMODE: case DL_LIGHTSTATE_FOGSTART: *value = 0; return 1;
  case DL_LIGHTSTATE_COLORMODEL: *value = DL_COLOR_RGB; return 1;
  case DL_LIGHTSTATE_FOGEND: case DL_LIGHTSTATE_FOGDENSITY: *value = dl_bits_of(1.0f); return 1;
  case DL_LIGHTSTATE_COLORVERTEX: *value = 1; return 1;
  default: *value = 0; return 0;
  }
}

/* Direct3D 7's magnification and mip filters (D3DTFG_*, D3DTFP_*) as Direct3D 9's (Wine: an unknown magnification filter is point, mip filter 0 is
 * linear and an unknown one none; FLATCUBIC / GAUSSIANCUBIC are d3d9's PYRAMIDALQUAD / GAUSSIANQUAD, the filters whose caps bits they share). The
 * minification filters have the same numbers. */
DL_API uint32_t dl_tfg_to_d3d9(uint32_t v) {
  switch (v) {
  case DL_TFG_POINT: return DL_TEXF9_POINT;
  case DL_TFG_LINEAR: return DL_TEXF9_LINEAR;
  case DL_TFG_FLATCUBIC: return DL_TEXF9_PYRAMIDALQUAD;
  case DL_TFG_GAUSSIANCUBIC: return DL_TEXF9_GAUSSIANQUAD;
  case DL_TFG_ANISOTROPIC: return DL_TEXF9_ANISOTROPIC;
  default: return DL_TEXF9_POINT;
  }
}
DL_API uint32_t dl_tfp_to_d3d9(uint32_t v) {
  switch (v) {
  case DL_TFP_NONE: return DL_TEXF9_NONE;
  case DL_TFP_POINT: return DL_TEXF9_POINT;
  case 0: case DL_TFP_LINEAR: return DL_TEXF9_LINEAR;
  default: return DL_TEXF9_NONE;
  }
}
DL_API uint32_t dl_tfg_from_d3d9(uint32_t v) {
  switch (v) {
  case DL_TEXF9_LINEAR: return DL_TFG_LINEAR;
  case DL_TEXF9_PYRAMIDALQUAD: return DL_TFG_FLATCUBIC;
  case DL_TEXF9_GAUSSIANQUAD: return DL_TFG_GAUSSIANCUBIC;
  case DL_TEXF9_ANISOTROPIC: return DL_TFG_ANISOTROPIC;
  default: return DL_TFG_POINT;
  }
}
DL_API uint32_t dl_tfp_from_d3d9(uint32_t v) { return v == DL_TEXF9_POINT ? DL_TFP_POINT : v == DL_TEXF9_LINEAR ? DL_TFP_LINEAR : DL_TFP_NONE; }

#define DL_STAGES 8u

/* SetTextureStageState of Direct3D 3 and 7: ADDRESS (both U and V), ADDRESSU / V, BORDERCOLOR, MAG / MIN / MIPFILTER, MIPMAPLODBIAS, MAXMIPLEVEL and
 * MAXANISOTROPY are the stage's sampler states; the others have Direct3D 9's numbers. A state past TEXTURETRANSFORMFLAGS (or 0) is taken and does
 * nothing (Wine). DDERR_INVALIDPARAMS for a stage past the eighth. (Direct3D 3 also leaves the legacy texture blending: the device forgets
 * TEXTUREMAPBLEND, ctx->texture_map_blend 0.) */
DL_API DlResult dl_texture_stage_state(uint32_t stage, uint32_t state, uint32_t value, DlStateOps *o) {
  o->count = 0;
  if (stage >= DL_STAGES) return DL_DDERR_INVALIDPARAMS;
  switch (state) {
  case DL_TSS_ADDRESS:
    dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_ADDRESSU, value);
    dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_ADDRESSV, value);
    return DL_OK;
  case DL_TSS_ADDRESSU: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_ADDRESSU, value); return DL_OK;
  case DL_TSS_ADDRESSV: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_ADDRESSV, value); return DL_OK;
  case DL_TSS_BORDERCOLOR: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_BORDERCOLOR, value); return DL_OK;
  case DL_TSS_MAGFILTER: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_MAGFILTER, dl_tfg_to_d3d9(value)); return DL_OK;
  case DL_TSS_MINFILTER: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_MINFILTER, value); return DL_OK;
  case DL_TSS_MIPFILTER: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_MIPFILTER, dl_tfp_to_d3d9(value)); return DL_OK;
  case DL_TSS_MIPMAPLODBIAS: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_MIPMAPLODBIAS, value); return DL_OK;
  case DL_TSS_MAXMIPLEVEL: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_MAXMIPLEVEL, value); return DL_OK;
  case DL_TSS_MAXANISOTROPY: dl_ops_add(o, DL_SET_SAMPLER, stage, DL_SAMP9_MAXANISOTROPY, value); return DL_OK;
  default:
    if (state == 0 || state > DL_TSS_TEXTURETRANSFORMFLAGS) return DL_OK;
    dl_ops_add(o, DL_SET_STAGE, stage, state, value);
    return DL_OK;
  }
}
/* The value each texture stage state starts with (Direct3D 7's: stage 0 modulates, the others are off; each stage reads its own coordinates). */
DL_API uint32_t dl_texture_stage_default(uint32_t stage, uint32_t state) {
  switch (state) {
  case 1: return stage == 0 ? DL_TOP_MODULATE : DL_TOP_DISABLE;      /* COLOROP */
  case 4: return stage == 0 ? DL_TOP_SELECTARG1 : DL_TOP_DISABLE;    /* ALPHAOP */
  case 2: case 5: return DL_TA_TEXTURE;                              /* COLORARG1, ALPHAARG1 */
  case 3: case 6: return DL_TA_CURRENT;                              /* COLORARG2, ALPHAARG2 */
  case 11: return stage;                                             /* TEXCOORDINDEX */
  case DL_TSS_ADDRESS: case DL_TSS_ADDRESSU: case DL_TSS_ADDRESSV: return 1;    /* WRAP */
  case DL_TSS_MAGFILTER: return DL_TFG_POINT;
  case DL_TSS_MINFILTER: return DL_TFN_POINT;
  case DL_TSS_MIPFILTER: return DL_TFP_NONE;
  case DL_TSS_MAXANISOTROPY: return 1;
  default: return 0;
  }
}

/* A D3DTRANSFORMSTATETYPE as Direct3D 9's D3DTRANSFORMSTATETYPE: WORLD 256, VIEW 2, PROJECTION 3, WORLD1 .. 3 257 .. 259, TEXTURE0 .. 7 16 .. 23.
 * Direct3D 1-3 have only WORLD, VIEW and PROJECTION. 0 (DDERR_INVALIDPARAMS) for another. */
DL_API uint32_t dl_transform_state(unsigned version, uint32_t state) {
  switch (state) {
  case DL_TRANSFORMSTATE_WORLD: return 256;
  case DL_TRANSFORMSTATE_VIEW: return 2;
  case DL_TRANSFORMSTATE_PROJECTION: return 3;
  default:
    if (version < 7) return 0;
    if (state >= DL_TRANSFORMSTATE_WORLD1 && state <= DL_TRANSFORMSTATE_WORLD3) return 256 + state - 3;
    if (state >= DL_TRANSFORMSTATE_TEXTURE0 && state <= DL_TRANSFORMSTATE_TEXTURE7) return state;
    return 0;
  }
}

/* ==== lights and materials ================================================================================================================================= */

#define DL_LIGHT_SIZE 76u
#define DL_LIGHT2_SIZE 80u
#define DL_MATERIAL_SIZE 80u

/* IDirect3DLight::SetLight: a D3DLIGHT (dwSize 76) or D3DLIGHT2 (80) as a light for dl_light_vertex: the colour is the diffuse and (unless
 * D3DLIGHT_NO_SPECULAR) the specular colour; lights of Direct3D 1-3 add nothing to the ambient. `*active`: a D3DLIGHT is always on, a D3DLIGHT2 when
 * D3DLIGHT_ACTIVE is set. DDERR_INVALIDPARAMS for a smaller size or a type that is not POINT .. PARALLELPOINT (Wine refuses GLSPOT too). */
DL_API DlResult dl_light_from_legacy(const DlLight2 *in, DlLightEntry *out, int *active) {
  if (!in || in->dwSize < DL_LIGHT_SIZE) return DL_DDERR_INVALIDPARAMS;
  if (in->dltType < DL_LIGHT_POINT || in->dltType > DL_LIGHT_PARALLELPOINT) return DL_DDERR_INVALIDPARAMS;
  const uint32_t flags = in->dwSize >= DL_LIGHT2_SIZE ? in->dwFlags : DL_LIGHT_ACTIVE;
  dl_zero(out, sizeof *out);
  out->type = in->dltType;
  const float c[3] = {in->dcvColor.r, in->dcvColor.g, in->dcvColor.b};
  for (int i = 0; i < 3; ++i) {
    out->diffuse[i] = c[i];
    out->specular[i] = (flags & DL_LIGHT_NO_SPECULAR) ? 0.0f : c[i];
  }
  out->position[0] = in->dvPosition.x; out->position[1] = in->dvPosition.y; out->position[2] = in->dvPosition.z;
  out->direction[0] = in->dvDirection.x; out->direction[1] = in->dvDirection.y; out->direction[2] = in->dvDirection.z;
  out->range = in->dvRange;
  out->falloff = in->dvFalloff;
  out->attenuation[0] = in->dvAttenuation0;
  out->attenuation[1] = in->dvAttenuation1;
  out->attenuation[2] = in->dvAttenuation2;
  out->theta = in->dvTheta;
  out->phi = in->dvPhi;
  *active = (flags & DL_LIGHT_ACTIVE) != 0;
  return DL_OK;
}
/* IDirect3DDevice7::SetLight: POINT, SPOT and DIRECTIONAL (DDERR_INVALIDPARAMS otherwise), as a light for dl_light_vertex in Direct3D 7's model. */
DL_API DlResult dl_light_from_light7(const DlLight7 *in, DlLightEntry *out) {
  if (!in || in->dltType < DL_LIGHT_POINT || in->dltType > DL_LIGHT_DIRECTIONAL) return DL_DDERR_INVALIDPARAMS;
  dl_zero(out, sizeof *out);
  out->type = in->dltType;
  out->diffuse[0] = in->dcvDiffuse.r; out->diffuse[1] = in->dcvDiffuse.g; out->diffuse[2] = in->dcvDiffuse.b;
  out->specular[0] = in->dcvSpecular.r; out->specular[1] = in->dcvSpecular.g; out->specular[2] = in->dcvSpecular.b;
  out->ambient[0] = in->dcvAmbient.r; out->ambient[1] = in->dcvAmbient.g; out->ambient[2] = in->dcvAmbient.b;
  out->position[0] = in->dvPosition.x; out->position[1] = in->dvPosition.y; out->position[2] = in->dvPosition.z;
  out->direction[0] = in->dvDirection.x; out->direction[1] = in->dvDirection.y; out->direction[2] = in->dvDirection.z;
  out->range = in->dvRange;
  out->falloff = in->dvFalloff;
  out->attenuation[0] = in->dvAttenuation0;
  out->attenuation[1] = in->dvAttenuation1;
  out->attenuation[2] = in->dvAttenuation2;
  out->theta = in->dvTheta;
  out->phi = in->dvPhi;
  return DL_OK;
}
/* A legacy light as a D3DLIGHT7 / D3DLIGHT9, for a device that lights Direct3D 1-3 geometry with d3d9's fixed function: the fields as Wine maps
 * them (colour into diffuse and, without NO_SPECULAR, specular; no ambient). 1 if d3d9 lights it the same way; 0 where it cannot: a parallel point
 * light (made a directional light along -position; d3d9 has no per-object direction) or a positional light whose legacy attenuation (a0 + a1 r +
 * a2 r^2 over the range) is not the constant 1 / a0. The CPU lighting (dl_process_vertex) is exact for all of them. */
DL_API int dl_light7_from_legacy(const DlLightEntry *in, DlLight7 *out) {
  dl_zero(out, sizeof *out);
  out->dltType = in->type == DL_LIGHT_PARALLELPOINT ? (uint32_t)DL_LIGHT_DIRECTIONAL : in->type;
  out->dcvDiffuse.r = in->diffuse[0]; out->dcvDiffuse.g = in->diffuse[1]; out->dcvDiffuse.b = in->diffuse[2]; out->dcvDiffuse.a = 1.0f;
  out->dcvSpecular.r = in->specular[0]; out->dcvSpecular.g = in->specular[1]; out->dcvSpecular.b = in->specular[2]; out->dcvSpecular.a = 1.0f;
  out->dvPosition.x = in->position[0]; out->dvPosition.y = in->position[1]; out->dvPosition.z = in->position[2];
  out->dvDirection.x = in->direction[0]; out->dvDirection.y = in->direction[1]; out->dvDirection.z = in->direction[2];
  out->dvRange = in->range;
  out->dvFalloff = in->falloff;
  out->dvTheta = in->theta;
  out->dvPhi = in->phi;
  if (in->type == DL_LIGHT_PARALLELPOINT) {
    out->dvDirection.x = -in->position[0];
    out->dvDirection.y = -in->position[1];
    out->dvDirection.z = -in->position[2];
    out->dvAttenuation0 = 1.0f;
    return 0;
  }
  if (in->type == DL_LIGHT_DIRECTIONAL) {
    out->dvAttenuation0 = 1.0f;
    return 1;
  }
  /* a constant legacy weight a0 + a1 + a2 at the light, a0 at the range: exact only when a1 = a2 = 0, as 1 / (1 / a0) */
  const float a0 = in->attenuation[0];
  out->dvAttenuation0 = a0 > 0.0f ? 1.0f / a0 : 1.0f;
  return !(in->attenuation[1] > 0.0f || in->attenuation[1] < 0.0f || in->attenuation[2] > 0.0f || in->attenuation[2] < 0.0f) && a0 > 0.0f;
}
/* IDirect3DMaterial::SetMaterial: a D3DMATERIAL (dwSize 80) as Direct3D 7's (hTexture and dwRampSize are the ramp model's and kept by the material).
 * DDERR_INVALIDPARAMS for another size. */
DL_API DlResult dl_material7_from_legacy(const DlMaterial *in, DlMaterial7 *out) {
  if (!in || in->dwSize != DL_MATERIAL_SIZE) return DL_DDERR_INVALIDPARAMS;
  out->diffuse = in->diffuse;
  out->ambient = in->ambient;
  out->specular = in->specular;
  out->emissive = in->emissive;
  out->power = in->power;
  return DL_OK;
}

/* ==== ComputeSphereVisibility ================================================================================================================================ */

/* The six planes of the view volume in object space (left, right, top, bottom, front, back), from world * view * projection (with the clip space,
 * for Direct3D 3): inside where a x + b y + c z + d >= 0. */
DL_API void dl_frustum_planes(const DlTransform *t, float planes[6][4]) {
  const DlMatrix *m = &t->total;
  for (int i = 0; i < 4; ++i) {
    const float x = m->m[i][0], y = m->m[i][1], z = m->m[i][2], w = m->m[i][3];
    planes[0][i] = w + x;
    planes[1][i] = w - x;
    planes[2][i] = w - y;
    planes[3][i] = w + y;
    planes[4][i] = z;
    planes[5][i] = w - z;
  }
}
/* Where a sphere is against a plane (Wine's in_plane): bit 0 it reaches across it (union), bit 1 it is wholly outside (intersection too).
 * `inclusive` (Direct3D 3) counts touching as crossing. */
DL_API uint32_t dl_sphere_plane(const float p[4], const float center[3], float radius, int inclusive) {
  const float norm = ffl_sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  if (!(norm > 0.0f)) return 0;
  const float distance = (p[0] * center[0] + p[1] * center[1] + p[2] * center[2] + p[3]) / norm;
  const float magnitude = distance < 0.0f ? -distance : distance;
  if (inclusive ? magnitude <= radius : magnitude < radius) return 1;
  if (inclusive ? distance <= -radius : distance < -radius) return 3;
  return 0;
}
/* IDirect3DDevice7::ComputeSphereVisibility for one sphere (object space): D3DSTATUS_CLIPUNION* (bit n) and CLIPINTERSECTION* (bit 12 + n) for the
 * six planes of the frustum and (bits 6 .. 11) the user clip planes the transform enables (world space: the sphere's centre is moved by the world
 * matrix and its radius scaled by the matrix's largest axis). 0: wholly visible. */
DL_API uint32_t dl_sphere_visibility7(const DlTransform *t, const float center[3], float radius) {
  float planes[6][4];
  dl_frustum_planes(t, planes);
  uint32_t result = 0;
  for (unsigned i = 0; i < 6; ++i) {
    const uint32_t r = dl_sphere_plane(planes[i], center, radius, 0);
    if (r & 1) result |= DL_CLIP_LEFT << i;
    if (r & 2) result |= (DL_CLIP_LEFT << i) << 12;
  }
  if (t->user_planes & 0x3Fu) {
    float world[4], scale = 0.0f;
    dl_transform_point(&t->world, center, world);
    for (int r = 0; r < 3; ++r) {
      const float s = ffl_sqrt(t->world.m[r][0] * t->world.m[r][0] + t->world.m[r][1] * t->world.m[r][1] + t->world.m[r][2] * t->world.m[r][2]);
      if (s > scale) scale = s;
    }
    for (unsigned i = 0; i < 6; ++i) {
      if (!(t->user_planes & (1u << i))) continue;
      const uint32_t r = dl_sphere_plane(t->user_plane[i], world, radius * scale, 0);
      if (r & 1) result |= DL_CLIP_GEN0 << i;
      if (r & 2) result |= (DL_CLIP_GEN0 << i) << 12;
    }
  }
  return result;
}
/* IDirect3DDevice3::ComputeSphereVisibility for one sphere: D3DVIS_* - per plane INTERSECT (1) or OUTSIDE (2) at bits 2 + 2n, and
 * D3DVIS_OUTSIDE_FRUSTUM if it is outside any plane, else D3DVIS_INTERSECT_FRUSTUM if it crosses one. */
DL_API uint32_t dl_sphere_visibility3(const DlTransform *t, const float center[3], float radius) {
  float planes[6][4];
  dl_frustum_planes(t, planes);
  uint32_t result = 0;
  int outside = 0, crossing = 0;
  for (unsigned i = 0; i < 6; ++i) {
    const uint32_t r = dl_sphere_plane(planes[i], center, radius, 1);
    if (r == 3) {
      result |= DL_VIS_OUTSIDE_LEFT << (2 * i);
      outside = 1;
    } else if (r == 1) {
      result |= DL_VIS_INTERSECT_LEFT << (2 * i);
      crossing = 1;
    }
  }
  if (outside) result |= DL_VIS_OUTSIDE_FRUSTUM;
  else if (crossing) result |= DL_VIS_INTERSECT_FRUSTUM;
  return result;
}

#endif
