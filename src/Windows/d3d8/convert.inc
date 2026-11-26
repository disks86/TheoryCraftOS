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

/* d3d8.dll: the structures that differ between Direct3D 8 and 9 - the caps, the presentation parameters, the descriptions of surfaces and volumes,
 * the adapter's identifier - and the sizes of formats (D3DSURFACE_DESC.Size). Included by d3d8.c. */

/* ---- caps ------------------------------------------------------------------------------------------------------------------------------------------- */

/* The bits of each caps field that Direct3D 8 defines (d3d8caps.h): Direct3D 9's later ones are left out. */
#define CAPS8_CAPS 0x00020000u                    /* D3DCAPS_READ_SCANLINE */
#define CAPS8_CAPS2 0x30120002u                   /* NO2DDURING3DSCENE, FULLSCREENGAMMA, CANCALIBRATEGAMMA, CANMANAGERESOURCE, DYNAMICTEXTURES (and CANRENDERWINDOWED: below) */
#define CAPS8_CAPS3 0x00000020u                   /* ALPHA_FULLSCREEN_FLIP_OR_DISCARD */
#define CAPS8_PRIMITIVEMISC 0x00001FF6u            /* MASKZ .. NULLREFERENCE */
#define CAPS8_RASTER 0x00F7A199u                   /* DITHER .. STRETCHBLTMULTISAMPLE but ZBIAS and ANTIALIASEDGES (below), without Direct3D 9's SCISSORTEST, SLOPESCALEDEPTHBIAS, DEPTHBIAS, MULTISAMPLE_TOGGLE */
#define CAPS8_TEXTURE 0x0007EDE7u                  /* PERSPECTIVE .. VOLUMEMAP_POW2 (no NOPROJECTEDBUMPENV) */
#define CAPS8_FILTER 0x1F030700u                   /* MINF*, MIPF*, MAGF* of Direct3D 8 */
#define CAPS8_BLEND 0x00001FFFu                    /* ZERO .. BOTHINVSRCALPHA (no BLENDFACTOR) */
#define CAPS8_LINE 0x0000001Fu                     /* TEXTURE, ZTEST, BLEND, ALPHACMP, FOG (ANTIALIAS is Direct3D 9's) */
#define CAPS8_STENCIL 0x000000FFu                  /* the eight operations (TWOSIDED is Direct3D 9's) */
#define CAPS8_VERTEXPROCESSING 0x0000007Bu         /* TEXGEN, MATERIALSOURCE7, DIRECTIONALLIGHTS, POSITIONALLIGHTS, LOCALVIEWER, TWEENING */
#define D3DVTXPCAPS8_NO_VSDT_UBYTE4 0x00000080u    /* (Direct3D 8: UBYTE4 vertices cannot be read; Direct3D 9 says the opposite with D3DDTCAPS_UBYTE4) */

static DWORD min_dword(DWORD a, DWORD b) { return a < b ? a : b; }

/* D3DCAPS8 from Direct3D 9's caps: the same numbers, the bits Direct3D 8 has, shader versions of Direct3D 8 (vertex shaders 1.1, pixel shaders 1.4). */
static void caps_from9(const D3DCAPS9 *c9, D3DCAPS8 *c8) {
  memset(c8, 0, sizeof *c8);
  c8->DeviceType = c9->DeviceType;
  c8->AdapterOrdinal = c9->AdapterOrdinal;
  c8->Caps = c9->Caps & CAPS8_CAPS;
  c8->Caps2 = (c9->Caps2 & CAPS8_CAPS2) | D3DCAPS2_CANRENDERWINDOWED;      /* (Direct3D 9 renders into windows always and dropped the bit) */
  c8->Caps3 = c9->Caps3 & CAPS8_CAPS3;
  c8->PresentationIntervals = c9->PresentationIntervals;
  c8->CursorCaps = c9->CursorCaps;
  c8->DevCaps = c9->DevCaps;
  c8->PrimitiveMiscCaps = c9->PrimitiveMiscCaps & CAPS8_PRIMITIVEMISC;
  c8->RasterCaps = c9->RasterCaps & CAPS8_RASTER;
  if (c9->RasterCaps & D3D9_PRASTERCAPS_DEPTHBIAS) c8->RasterCaps |= D3DPRASTERCAPS_ZBIAS;      /* (D3DRS_ZBIAS is set as Direct3D 9's depth bias) */
  if (c9->LineCaps & D3D9_LINECAPS_ANTIALIAS) c8->RasterCaps |= D3DPRASTERCAPS_ANTIALIASEDGES;    /* (D3DRS_EDGEANTIALIAS is D3DRS_ANTIALIASEDLINEENABLE) */
  c8->ZCmpCaps = c9->ZCmpCaps;
  c8->SrcBlendCaps = c9->SrcBlendCaps & CAPS8_BLEND;
  c8->DestBlendCaps = c9->DestBlendCaps & CAPS8_BLEND;
  c8->AlphaCmpCaps = c9->AlphaCmpCaps;
  c8->ShadeCaps = c9->ShadeCaps;
  c8->TextureCaps = c9->TextureCaps & CAPS8_TEXTURE;
  c8->TextureFilterCaps = c9->TextureFilterCaps & CAPS8_FILTER;
  c8->CubeTextureFilterCaps = c9->CubeTextureFilterCaps & CAPS8_FILTER;
  c8->VolumeTextureFilterCaps = c9->VolumeTextureFilterCaps & CAPS8_FILTER;
  c8->TextureAddressCaps = c9->TextureAddressCaps;
  c8->VolumeTextureAddressCaps = c9->VolumeTextureAddressCaps;
  c8->LineCaps = c9->LineCaps & CAPS8_LINE;
  c8->MaxTextureWidth = c9->MaxTextureWidth;
  c8->MaxTextureHeight = c9->MaxTextureHeight;
  c8->MaxVolumeExtent = c9->MaxVolumeExtent;
  c8->MaxTextureRepeat = c9->MaxTextureRepeat;
  c8->MaxTextureAspectRatio = c9->MaxTextureAspectRatio;
  c8->MaxAnisotropy = c9->MaxAnisotropy;
  c8->MaxVertexW = c9->MaxVertexW;
  c8->GuardBandLeft = c9->GuardBandLeft;
  c8->GuardBandTop = c9->GuardBandTop;
  c8->GuardBandRight = c9->GuardBandRight;
  c8->GuardBandBottom = c9->GuardBandBottom;
  c8->ExtentsAdjust = c9->ExtentsAdjust;
  c8->StencilCaps = c9->StencilCaps & CAPS8_STENCIL;
  c8->FVFCaps = c9->FVFCaps;
  c8->TextureOpCaps = c9->TextureOpCaps;
  c8->MaxTextureBlendStages = c9->MaxTextureBlendStages;
  c8->MaxSimultaneousTextures = c9->MaxSimultaneousTextures;
  c8->VertexProcessingCaps = c9->VertexProcessingCaps & CAPS8_VERTEXPROCESSING;
  if (!(c9->DeclTypes & D3D9_DTCAPS_UBYTE4)) c8->VertexProcessingCaps |= D3DVTXPCAPS8_NO_VSDT_UBYTE4;
  c8->MaxActiveLights = c9->MaxActiveLights;
  c8->MaxUserClipPlanes = c9->MaxUserClipPlanes;
  c8->MaxVertexBlendMatrices = c9->MaxVertexBlendMatrices;
  c8->MaxVertexBlendMatrixIndex = c9->MaxVertexBlendMatrixIndex;
  c8->MaxPointSize = c9->MaxPointSize;
  c8->MaxPrimitiveCount = c9->MaxPrimitiveCount;
  c8->MaxVertexIndex = c9->MaxVertexIndex;
  c8->MaxStreams = min_dword(c9->MaxStreams, 16);
  c8->MaxStreamStride = c9->MaxStreamStride;
  /* Shader versions of Direct3D 8: what Direct3D 9 runs of them. */
  const DWORD vs = c9->VertexShaderVersion & 0xFFFFu, ps = c9->PixelShaderVersion & 0xFFFFu;
  c8->VertexShaderVersion = vs >= 0x0101u ? D3DVS_VERSION(1, 1) : vs ? (0xFFFE0000u | vs) : 0;
  c8->MaxVertexShaderConst = vs ? min_dword(c9->MaxVertexShaderConst, D8_CONSTANTS) : 0;
  c8->PixelShaderVersion = ps >= 0x0104u ? D3DPS_VERSION(1, 4) : ps ? (0xFFFF0000u | ps) : 0;
  c8->MaxPixelShaderValue = c9->PixelShader1xMaxValue;
}

/* ---- presentation parameters ------------------------------------------------------------------------------------------------------------------------ */

/* Direct3D 8's rules for D3DPRESENT_PARAMETERS (the same for CreateDevice, CreateAdditionalSwapChain and Reset), and the Direct3D 9 form: the swap
 * effect D3DSWAPEFFECT_COPY_VSYNC is a copy at the vertical blank, a window presents at once with the others (Direct3D 9's default waits), and
 * there is no multisample quality. */
static HRESULT present_to9(const D3DPRESENT_PARAMETERS *p8, D3DPRESENT_PARAMETERS9 *p9, BOOL reset) {
  memset(p9, 0, sizeof *p9);
  if (!p8) return D3DERR_INVALIDCALL;
  const DWORD effect = p8->SwapEffect;
  if (effect < D3DSWAPEFFECT_DISCARD || effect > D3DSWAPEFFECT_COPY_VSYNC) return D3DERR_INVALIDCALL;
  if (p8->BackBufferCount > D3DPRESENT_BACK_BUFFERS_MAX) return D3DERR_INVALIDCALL;
  if ((effect == D3DSWAPEFFECT_COPY || effect == D3DSWAPEFFECT_COPY_VSYNC) && p8->BackBufferCount > 1) return D3DERR_INVALIDCALL;
  (void)reset;
  if (p8->Windowed && p8->FullScreen_RefreshRateInHz) return D3DERR_INVALIDCALL;      /* (a window has the display's rate) */
  p9->BackBufferWidth = p8->BackBufferWidth;
  p9->BackBufferHeight = p8->BackBufferHeight;
  p9->BackBufferFormat = p8->BackBufferFormat;
  p9->BackBufferCount = p8->BackBufferCount;
  p9->MultiSampleType = p8->MultiSampleType;
  p9->MultiSampleQuality = 0;
  p9->SwapEffect = effect == D3DSWAPEFFECT_COPY_VSYNC ? D3D9_SWAPEFFECT_COPY : effect;
  p9->hDeviceWindow = p8->hDeviceWindow;
  p9->Windowed = p8->Windowed;
  p9->EnableAutoDepthStencil = p8->EnableAutoDepthStencil;
  p9->AutoDepthStencilFormat = p8->AutoDepthStencilFormat;
  p9->Flags = p8->Flags;
  p9->FullScreen_RefreshRateInHz = p8->FullScreen_RefreshRateInHz;
  if (p8->Windowed) p9->PresentationInterval = effect == D3DSWAPEFFECT_COPY_VSYNC ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  else p9->PresentationInterval = p8->FullScreen_PresentationInterval;
  return D3D_OK;
}
/* What Direct3D 8 writes back into the program's parameters: the count of back buffers only (a window's size of 0 stays 0, unlike Direct3D 9). */
static void present_from9(const D3DPRESENT_PARAMETERS9 *p9, D3DPRESENT_PARAMETERS *p8) {
  p8->BackBufferCount = p9->BackBufferCount ? p9->BackBufferCount : 1;
}

/* ---- formats ---------------------------------------------------------------------------------------------------------------------------------------- */

/* The bits a pixel of a format has (0: a compressed or unknown format), and the bytes of a block of the compressed ones. */
static UINT format_bits(D3DFORMAT f) {
  switch ((DWORD)f) {
  case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8:
  case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_W11V11U10: case D3DFMT_A2W10V10U10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8:
  case D3DFMT_D24X4S4: case D3DFMT_INDEX32: case 32 /* A8B8G8R8 */: case 33 /* X8B8G8R8 */: case 82 /* D32F_LOCKABLE */: case 83 /* D24FS8 */:
  case 112 /* R16F G16F */: case 114 /* R32F */:
    return 32;
  case D3DFMT_R8G8B8:
    return 24;
  case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4: case D3DFMT_A8P8:
  case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_D16: case D3DFMT_INDEX16:
  case D3DFMT_UYVY: case D3DFMT_YUY2: case 111 /* R16F */:
    return 16;
  case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4:
    return 8;
  case 36 /* A16B16G16R16 */: case 113 /* A16B16G16R16F */: case 115 /* G32R32F */:
    return 64;
  case 116 /* A32B32G32R32F */:
    return 128;
  default:
    return 0;
  }
}
static UINT format_block_bytes(D3DFORMAT f) {
  switch ((DWORD)f) {
  case D3DFMT_DXT1: return 8;
  case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 16;
  default: return 0;
  }
}
/* The bytes of a surface or volume of a format (D3DSURFACE_DESC.Size, D3DVOLUME_DESC.Size): blocks of 4 x 4 for the compressed formats. */
static UINT format_size(D3DFORMAT f, UINT width, UINT height, UINT depth) {
  const UINT block = format_block_bytes(f);
  if (block) return (width + 3) / 4 * ((height + 3) / 4) * block * depth;
  const UINT64 bytes = (UINT64)width * height * depth * format_bits(f) / 8;
  return bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (UINT)bytes;
}
static BOOL format_is_depth(D3DFORMAT f) {
  switch ((DWORD)f) {
  case D3DFMT_D16_LOCKABLE: case D3DFMT_D32: case D3DFMT_D15S1: case D3DFMT_D24S8: case D3DFMT_D16: case D3DFMT_D24X8: case D3DFMT_D24X4S4: return TRUE;
  default: return FALSE;
  }
}
/* The bits of depth a depth format has (for D3DRS_ZBIAS, in units of the depth buffer). */
static UINT format_depth_bits(D3DFORMAT f) {
  switch ((DWORD)f) {
  case D3DFMT_D16_LOCKABLE: case D3DFMT_D16: return 16;
  case D3DFMT_D15S1: return 15;
  case D3DFMT_D32: return 32;
  default: return 24;
  }
}

/* ---- descriptions ----------------------------------------------------------------------------------------------------------------------------------- */

static void surface_desc_from9(const D3DSURFACE_DESC9 *d9, D3DSURFACE_DESC *d8) {
  d8->Format = d9->Format;
  d8->Type = d9->Type;
  d8->Usage = d9->Usage;
  d8->Pool = d9->Pool;
  d8->Size = format_size(d9->Format, d9->Width, d9->Height, 1);
  d8->MultiSampleType = d9->MultiSampleType;
  d8->Width = d9->Width;
  d8->Height = d9->Height;
}
static void volume_desc_from9(const D3DVOLUME_DESC9 *d9, D3DVOLUME_DESC *d8) {
  d8->Format = d9->Format;
  d8->Type = d9->Type;
  d8->Usage = d9->Usage;
  d8->Pool = d9->Pool;
  d8->Size = format_size(d9->Format, d9->Width, d9->Height, d9->Depth);
  d8->Width = d9->Width;
  d8->Height = d9->Height;
  d8->Depth = d9->Depth;
}
static void identifier_from9(const D3DADAPTER_IDENTIFIER9 *i9, D3DADAPTER_IDENTIFIER8 *i8) {
  memcpy(i8->Driver, i9->Driver, sizeof i8->Driver);
  memcpy(i8->Description, i9->Description, sizeof i8->Description);
  i8->DriverVersion = i9->DriverVersion;
  i8->VendorId = i9->VendorId;
  i8->DeviceId = i9->DeviceId;
  i8->SubSysId = i9->SubSysId;
  i8->Revision = i9->Revision;
  i8->DeviceIdentifier = i9->DeviceIdentifier;
  i8->WHQLLevel = i9->WHQLLevel;
}
