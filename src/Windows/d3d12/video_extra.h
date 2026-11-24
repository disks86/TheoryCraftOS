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

/* What d3d11.dll's video processor asks of d3d12.dll's ProcessFrames beyond Direct3D 12's arguments (Direct3D 11 has stereo packings, view choices, a mono offset,
 * HDR metadata, half-rate deinterlacing, motion compensated deinterlacing, repeated frames and 2:2 inverse telecine that Direct3D 12's video processor has not): set
 * as private data of the video process command list under
 * TCOS_VIDEO_PROCESS_EXTRA_GUID just before ProcessFrames, which reads it (and the list keeps it until it is set again or removed). Not an interface of Windows':
 * only our two DLLs know it. */
#ifndef TCOS_VIDEO_EXTRA_H
#define TCOS_VIDEO_EXTRA_H

/* {6a1c0d2e-4f61-4a5b-9d1e-7c3b5e8f2a90} */
static const GUID TCOS_VIDEO_PROCESS_EXTRA_GUID = {0x6a1c0d2e, 0x4f61, 0x4a5b, {0x9d, 0x1e, 0x7c, 0x3b, 0x5e, 0x8f, 0x2a, 0x90}};

enum { TCOS_STEREO_NONE, TCOS_STEREO_HORIZONTAL, TCOS_STEREO_VERTICAL, TCOS_STEREO_ROWS, TCOS_STEREO_COLUMNS, TCOS_STEREO_CHECKERBOARD, TCOS_STEREO_MONO_OFFSET,
       TCOS_STEREO_SEPARATE };   /* (the first seven are hardware::VpStereoFormat's) */
#define TCOS_FIELD_FROM_INDEX 0xFFu
#define TCOS_DEINTERLACE_FROM_DESC 0u              /* the input desc's DeinterlaceMode */
#define TCOS_DEINTERLACE_MOTION_COMPENSATED 3u     /* (the kernel's VpDeinterlace::MotionCompensated) */
#define TCOS_HDR10PLUS_BYTES 72u                    /* DXGI_HDR_METADATA_HDR10PLUS */
#define TCOS_VIDEO_EXTRA_VERSION 2u

typedef struct TcosVideoStreamExtra {
  UINT stereo;            /* TCOS_STEREO_*: how the picture holds its views (replaces the input desc's StereoFormat) */
  UINT view_frame[2];     /* the frame (0, 1) each output view shows: [0] the left view (or, with mono output, the base view), [1] the right */
  UINT flip_frame;        /* 0 none, 1 frame 0 is stored upside down, 2 frame 1 (D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE) */
  INT mono_offset;        /* MONO_OFFSET: the right view is moved right by it, the left view left */
  UINT field;             /* the field shown (0 first, 1 second), or TCOS_FIELD_FROM_INDEX: RateInfo.OutputIndex's parity */
  FLOAT hdr[4];           /* the content's light levels, cd/m2: max mastering, min mastering, MaxCLL, MaxFALL (0: not known) */
  UINT deinterlace;       /* TCOS_DEINTERLACE_*: how an interlaced stream is deinterlaced (the desc's mode, or motion compensated) */
  UINT repeat;            /* frame rate conversion repeats frames (D3D11's RepeatFrame) instead of blending them */
  UINT telecine22;        /* an interlaced stream at the output's rate is field matched (2:2 inverse telecine) instead of deinterlaced at half rate */
  UINT hdr10plus_size;    /* HDR10+ metadata of the picture: its bytes (0: none) */
  BYTE hdr10plus[TCOS_HDR10PLUS_BYTES];
} TcosVideoStreamExtra;

typedef struct TcosVideoProcessExtra {
  UINT version;           /* TCOS_VIDEO_EXTRA_VERSION */
  UINT count;             /* streams described (the ProcessFrames' streams in order) */
  FLOAT display[4];       /* the output's light levels (as hdr) */
  TcosVideoStreamExtra stream[16];
} TcosVideoProcessExtra;

#endif
