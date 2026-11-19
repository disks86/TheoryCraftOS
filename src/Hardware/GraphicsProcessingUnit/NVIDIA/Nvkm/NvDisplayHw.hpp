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

// THE register and method table of the native NVIDIA display code (NvDisplay.cpp, NvDisplayMode.cpp). Everything the display code writes to the
// GPU or into a push buffer is named here and nowhere else.
//
// Sources: nouveau (drivers/gpu/drm/nouveau: nvkm/engine/disp/nv50.c, gf119.c, gv100.c, tu102.c, ga102.c, sorgv100.c, sortu102.c, sorga102.c,
// headgv100.c, chan.c, nvkm/core/ramht.c, nvkm/engine/dma/usernv50.c, usergf119.c, usergv100.c, dispnv50/core507d.c, core907d.c, corec37d.c,
// corec57d.c, head507d.c, head907d.c, headc37d.c, headc57d.c, wndwc37e.c, wndwc57e.c, wndwc67e.c, wimmc37b.c) and NVIDIA's published display
// class headers (cl507d.h, cl907d.h, clc37d.h, clc37e.h, clc37b.h, clc57d.h, clc67d.h), all AS REMEMBERED: no source was at hand
// while this was written, and nothing here has met real hardware. Each entry carries a confidence mark:
//   [S] sure: the value appears in several places of nouveau that agree with each other (the readback offsets match the method numbers, ...)
//   [R] remembered from one place; likely right
//   [?] uncertain: check against nouveau / the class headers before trusting it on hardware
#pragma once
#include <stdint.h>

namespace hardware {
namespace nv {
namespace disp_hw {

// ---------------------------------------------------------------------------------------------------------------------------------------
// Display engine registers (BAR0)
// ---------------------------------------------------------------------------------------------------------------------------------------
// Taking the display from the VBIOS (nv50_disp_init / gf119_disp_init: "steal display away from vbios")
constexpr uint32_t kNv50VbiosOwner = 0x610024;      // [R] bit 8 set: the VBIOS (its EVO "core channel") owns the display; written 0x100 to take it
constexpr uint32_t kGf119VbiosOwner = 0x6100ac;     // [S] the same bit on GF119..Pascal (the register is also the supervisor status, bits 2:0)
constexpr uint32_t kVbiosOwnerBit = 0x00000100;
constexpr uint32_t kVbiosHandshake = 0x6194e8;      // [R] NV50..Pascal: bit 0 cleared by the driver, bit 1 drops when the VBIOS lets go
constexpr uint32_t kGv100Handshake = 0x6254e8;      // [R] Volta/Turing/GA102: the same handshake ("Claim ownership of display")
constexpr uint32_t kHandshakeRequest = 0x00000001, kHandshakeBusy = 0x00000002;

// Instance memory: where the display engine finds its RAMHT and context DMA objects
constexpr uint32_t kInstance = 0x610010;            // [S] NV50/GF119: (address >> 8) | 9.  GV100+: 0x8 | target (1 VRAM, 2 ncoh, 3 coh)
constexpr uint32_t kInstanceEnableNv50 = 0x9;       // [S]
constexpr uint32_t kGv100InstanceAddress = 0x610014;   // [S] GV100+: address >> 16 (64 KiB aligned)
constexpr uint32_t kGv100InstanceEnable = 0x8;      // [R]

// Interrupt enables. Everything is left off here: nothing in the kernel takes the GPU's interrupt yet, so the supervisor is polled (NvDisplayMode).
constexpr uint32_t kNv50IntrEnable0 = 0x610028, kNv50IntrEnable1 = 0x61002c;           // [R] nouveau: 0, 0x370
constexpr uint32_t kGf119IntrEnable0 = 0x610090, kGf119IntrEnableErr = 0x6100a0, kGf119IntrEnableSuper = 0x6100b0;   // [S] nouveau: 0, 0, 0x307

// Capability mirrors (the "copy what the display can do into EVO" loops of *_disp_init)
constexpr uint32_t kNv50Caps = 0x614004, kNv50CapsMirror = 0x610184;                   // [R]
constexpr uint32_t kNv50HeadCaps = 0x616100, kNv50HeadCapsMirror = 0x610190;           // [R] 4 words, head stride 0x800 / 0x10
constexpr uint32_t kNv50SorCaps = 0x61c000, kNv50SorCapsMirror = 0x6101e0;             // [R] SOR stride 0x800 / 4
constexpr uint32_t kGf119HeadCaps = 0x616104, kGf119HeadCapsMirror = 0x6101b4;         // [R] 3 words, head stride 0x800 both
constexpr uint32_t kGf119SorCaps = 0x61c000, kGf119SorCapsMirror = 0x6301c4;           // [?] nouveau writes 0x6301c4 + i*0x800 (or 0x6101c4?)
constexpr uint32_t kGf119UnderflowReport = 0x616308;                                   // [R] masked 0x111 -> 0x10 per head (gk104 underflow workaround)
constexpr uint32_t kGv100LockPins = 0x610068, kGv100LockPinsMirror = 0x640008;         // [R] Turing writes 0x21 instead of the copy
constexpr uint32_t kGv100CapsEnable = 0x640000;                                        // [R] bit 8+i: SOR i caps valid
constexpr uint32_t kGv100SorCapsMirror = 0x640144;                                     // [R] stride 8
constexpr uint32_t kGv100HeadRgCaps = 0x616300, kGv100HeadRgMirror = 0x640048;         // [R] head stride 0x800 / 0x20
constexpr uint32_t kGv100WindowCapsEnable = 0x640004;                                  // [R] bit per window
constexpr uint32_t kGv100Misc = 0x610078;                                              // [R] bit 0 set after the caps
constexpr uint32_t kGv100HeadPostcomp = 0x616100, kGv100HeadPostcompMirror = 0x640030, kGv100PostcompWords = 6;   // [R] head stride 0x800 / 0x20
constexpr uint32_t kTu102HeadPostcomp = 0x616140, kTu102HeadPostcompMirror = 0x640680, kTu102PostcompWords = 5;   // [R]
constexpr uint32_t kGv100WindowCaps = 0x630050, kGv100WindowCapsMirror = 0x6401e4;     // [R] 6 words, window stride 0x800 / 0x20
constexpr uint32_t kTu102WindowCaps = 0x630100, kTu102WindowCapsMirror = 0x640780;     // [R] 6 words, masked 0x7fff into the mirror
constexpr uint32_t kTu102WindowCapsMask = 0x00007fff;
constexpr uint32_t kTu102WindowCapsValid = 0x64000c, kTu102WindowCapsValidBit = 0x100; // [R]
constexpr uint32_t kTu102LockPinsValue = 0x00000021;                                   // [R] "XXX" in nouveau
constexpr uint32_t kGv100Ihub = 0x62e000, kGv100IhubMirror = 0x640010;                 // [R] gv100 4 words, tu102 3
constexpr uint32_t kGf119DacCaps = 0x61a000, kGf119DacCapsMirror = 0x6101c0;           // [R] DAC stride 0x800 both
constexpr uint32_t kNv50DacCaps = 0x61a000, kNv50DacCapsMirror = 0x6101d0;             // [R] stride 0x800 / 4

// What the display has
constexpr uint32_t kGf119HeadCount = 0x022448;      // [S] number of heads (gf119_head_cnt)
constexpr uint32_t kGf119Present = 0x612004;        // [S] bits 3:0 heads, 7:4 DACs, 15:8 SORs present
constexpr uint32_t kGv100Present = 0x610060;        // [S] bits 7:0 heads, 15:8 SORs
constexpr uint32_t kGv100WindowsPresent = 0x610064; // [S] bit per window
constexpr uint32_t kGv100Counts = 0x610074;         // [S] bits 3:0 heads, 11:8 SORs, 25:20 windows
constexpr uint32_t kNv50Heads = 2;                  // [S] nv50_head_cnt: always two
constexpr uint32_t kNv50SorPresent = 0x610184;      // [R] bits 27:24 (the caps mirror)

// The core channel (nv50_disp_core_init / gf119_disp_core_init / gv100_disp_core_init)
constexpr uint32_t kNv50CoreCtrl = 0x610200, kNv50CorePush = 0x610204, kNv50CorePush1 = 0x610208, kNv50CorePush2 = 0x61020c;   // [S]
constexpr uint32_t kGf119CoreCtrl = 0x610490, kGf119CorePush = 0x610494, kGf119CorePush1 = 0x610498, kGf119CorePush2 = 0x61049c;   // [S]
constexpr uint32_t kCorePush1Value = 0x00010000;    // [S] both
constexpr uint32_t kNv50CorePush2Value = 0x00000000, kGf119CorePush2Value = 0x00000001;   // [S]
constexpr uint32_t kCtrlPutEnable = 0x00000010;     // [S] set before PUT is written
constexpr uint32_t kCtrlStartCore = 0x01000013;     // [S]
constexpr uint32_t kCtrlBusy = 0x80000000;          // [S] cleared once the channel is up
constexpr uint32_t kCtrlEnableBits = 0x00000003;    // [S] cleared to stop the channel (fini)
constexpr uint32_t kCtrlStopBusyMask = 0x001e0000;  // [R] fini waits for these to clear
constexpr uint32_t kGv100CoreCtrl = 0x610b00, kGv100PushHigh = 0x610b20, kGv100PushLow = 0x610b24, kGv100Push2 = 0x610b28, kGv100Push3 = 0x610b2c;   // [S] +0x10 per channel
constexpr uint32_t kGv100Push2Value = 0x00000001, kGv100Push3Value = 0x00000040;   // [R]
constexpr uint32_t kGv100ChannelStride = 0x10;
constexpr uint32_t kGv100CtrlStatusMask = 0x00030000, kGv100CtrlStatusUp = 0x00010000;   // [R] core: (ctrl & 0x30000) == 0x10000 once running
constexpr uint32_t kGv100WindowStart = 0x00000011;  // [R] window channel ctrl value
constexpr uint32_t kGv100WindowIdle = 0x610664;     // [?] + (window * 4): (& 0xf0000) == 0x40000 when idle (gv100_disp_dmac_idle)
constexpr uint32_t kGv100WindowIdleMask = 0x000f0000, kGv100WindowIdleValue = 0x00040000;
// The window immediate channels (c37b, gv100_disp_wimm: ctrl/user 33): channel id 33 + window, so ctrl 0x610b00 + (33 + w) * 0x10, the user
// area 0x690000 + (32 + w) * 0x1000 and the idle word 0x610664 + (32 + w) * 4 (gv100_disp_chan_user / _dmac_idle: (chid - 1) * stride).
constexpr uint32_t kGv100WimmChid = 33;             // [R]
// Push buffer address as written to the channel (chan.c): target in bits 1:0, address >> 8 above
constexpr uint32_t kPushTargetVram = 1, kPushTargetNoncoherent = 2, kPushTargetCoherent = 3;   // [S]

// The channels' user areas: PUT at +0, GET at +4 (the client writes PUT in bytes)
constexpr uint32_t kCoreUserPre = 0x640000;         // [S] NV50/GF119 core channel (chid 0)
constexpr uint32_t kCoreUserGv100 = 0x680000;       // [S]
constexpr uint32_t kWindowUserGv100 = 0x690000;     // [S] + window * 0x1000
constexpr uint32_t kUserPut = 0x0, kUserGet = 0x4, kWindowUserStride = 0x1000;

// The method state the hardware lets be read back (nvkm_disp_chan_mthd dumps, *_head_state): the assembly copy (what the next UPDATE will
// apply) and the armed copy (what is shown).
constexpr uint32_t kGf119Armed = 0x640000;          // [S] + method   (gf119_head_state: asy is +0x20000 from this)
constexpr uint32_t kGf119Assembly = 0x660000;       // [S] + method
constexpr uint32_t kGv100Assembly = 0x680000;       // [S] + method
constexpr uint32_t kGv100Armed = 0x688000;          // [S] + method   (gv100_head_state: arm is +0x8000)
constexpr uint32_t kGv100WindowAssembly = 0x690000; // [R] + window * 0x1000 + method
constexpr uint32_t kGv100WindowArmed = 0x690800;    // [R] + window * 0x1000 + method (prev = +0x800)
// NV50 has no linear mirror: each method has its own register pair (assembly, armed = +4), head stride 0x540 (nv50_disp_core_mthd_head).
constexpr uint32_t kNv50ArmedDelta = 4, kNv50HeadStateStride = 0x540;
constexpr uint32_t kNv50StateRasterSize = 0x610af8, kNv50StateSyncEnd = 0x610b00, kNv50StateBlankEnd = 0x610ae8, kNv50StateBlankStart = 0x610af0;   // [R]
constexpr uint32_t kNv50StatePixelClock = 0x610ad0; // [R]
constexpr uint32_t kNv50StateOffset = 0x610a84, kNv50StateSize = 0x610b18, kNv50StateStorage = 0x610b20, kNv50StateParams = 0x610ac8, kNv50StateIso = 0x610a38;   // [?]
constexpr uint32_t kNv50StatePointIn = 0x610a58;    // [?] (0x0880? listed against 0x08c0 in our method list; only read back for completeness)

// Supervisor (polled). GF119: status 0x6100ac bits 2:0, written back to clear; per-head "what changed" masks; the final handshake.
constexpr uint32_t kGf119SuperStatus = 0x6100ac;    // [S]
constexpr uint32_t kGf119SuperHeadMask = 0x6101d4;  // [S] + head * 0x800
constexpr uint32_t kGf119SuperAck = 0x6101d0;       // [S] written 0x80000000 when a stage is done
constexpr uint32_t kSuperAckValue = 0x80000000;
constexpr uint32_t kSuperHeadModeset = 0x00001000;  // [S] the head's mask bit nouveau keys 1.0 / 2.x / 3.0 on
constexpr uint32_t kGv100SuperStatus = 0x611c30, kGv100SuperClear = 0x611860, kGv100SuperHeadMask = 0x6107ac, kGv100SuperAck = 0x6107a8;   // [R] head mask stride 4
// (gv100_disp_intr_ctrl_disp: 0x611c30 bits 2:0 pending, cleared by writing them to 0x611860; gv100_disp_super: the head masks at 0x6107ac + head
// * 4 with the same 0x1000 / 0x10000 bits as GF119, cleared to 0, then 0x6107a8 = 0x80000000. GA102 uses the same (ga102_disp: gv100_disp_super).)

// Head and SOR registers touched in supervisor 2.2 and DP training (gf119_head_rgclk, gf119_sor_clock, gf119_sor_dp_*, g94_sor_dp_power)
constexpr uint32_t kGf119HeadRgClk = 0x612200;      // [R] + head * 0x800, bits 3:0 the RG divider
constexpr uint32_t kGf119SorClock = 0x612300;       // [R] + sor * 0x800: bits 22:18 the link speed (DP bandwidth code, TMDS 0x0a/0x14), 10:8 div2, 2:0 div1
constexpr uint32_t kSorClockSpeedMask = 0x007c0000, kSorClockSpeedShift = 18, kSorClockDivMask = 0x00000707;
constexpr uint32_t kSorStride = 0x800, kSorLinkBStride = 0x80;   // [S] nv50_ior_base / nv50_sor_link (link B is +0x80)
constexpr uint32_t kSorDpCtrl = 0x61c10c;           // [R] + link: bits 20:16 lane mask, 14 enhanced framing, 30 MST
constexpr uint32_t kSorDpCtrlMask = 0x401f4000, kSorDpEnhancedFraming = 0x00004000;
constexpr uint32_t kSorDpPatternA = 0x61c110, kSorDpPatternB = 0x61c12c;   // [R] training pattern per lane byte (0x10 off, 0x01..0x03 TPS1..3)
constexpr uint32_t kSorDpPatternMask = 0x1f1f1f1f;
constexpr uint32_t kSorDpDriveCurrent = 0x61c118, kSorDpPreEmphasis = 0x61c120, kSorDpLaneEnable = 0x61c130, kSorDpPostCursor = 0x61c13c;   // [R] + link
constexpr uint32_t kSorDpPower = 0x61c034;          // [R] + sor: bit 31 set, then waits for it to clear
constexpr uint8_t kGf119DpLaneMap[4] = {2, 1, 0, 3};   // [R] gf119_sor_dp: lane n's byte in the drive registers
constexpr uint32_t kSorDpPowerLaneMask = 0x0000000f;   // [R] in kSorDpLaneEnable: a bit per powered lane (g94_sor_dp_power)
constexpr uint32_t kSorDpPowerPending = 0x80000000;
constexpr uint32_t kSuperHeadClock = 0x00010000;       // [S] the head's mask bit for supervisor 2.1 (the VPLL)
// DP "internal configuration" of supervisor 2.2 (nv50_disp_super_2_2_dp): per head / per link
constexpr uint32_t kGf119DpWatermark = 0x616610;       // [?] + head * 0x800: mask 0x0800003f, 0x08000000 | watermark
constexpr uint32_t kGf119DpAudioH = 0x616620, kGf119DpAudioV = 0x616624;   // [?] + head * 0x800: masks 0xffff / 0xffffff
constexpr uint32_t kGv100DpWatermark = 0x616550;       // [R] gv100_sor_dp_watermark: + head * 0x800, mask 0x0c00003f, 0x08000000 | watermark
constexpr uint32_t kGv100DpAudioH = 0x616568, kGv100DpAudioV = 0x61656c;   // [R] gv100_sor_dp_audio_sym: + head * 0x800, masks 0xffff / 0xffffff
// Volta and later SOR DisplayPort differences
constexpr uint8_t kGm200DpLaneMap[4] = {0, 1, 2, 3};   // [R] gm200_sor_dp / gv100_sor_dp / ga102: lanes not swizzled any more
constexpr uint32_t kGm200DpTxPuMask = 0x00000f00;      // [R] gm200_sor_dp_drive: pu & 0x0f into bits 11:8 of 0x61c130 (gf119: 0xff00)
// tu102_sor_dp_links / ga102_sor_dp_links, after the link rate: 40 ms, then 0x612300 bits 17:16 = 1 and 0x61c10c bits 1:0 = 1 ("XXX" in nouveau)
constexpr uint32_t kTu102SorClockEnableMask = 0x00030000, kTu102SorClockEnable = 0x00010000;   // [R]
constexpr uint32_t kTu102SorDpEnableMask = 0x00000003, kTu102SorDpEnable = 0x00000001;         // [R]
constexpr uint32_t kTu102DpLinkDelayUs = 40000;        // [R]
// ga102_sor_dp_links: the link rate is an index in 0x612300 bits 22:18, not the bandwidth code: RBR 0, HBR 1, HBR2 2, HBR3 3, and the eDP
// rates 2.16 / 2.43 / 3.24 / 4.32 Gbit/s 4..7.                                                                                        [R]
constexpr uint8_t kGa102DpRateCodes[8] = {0x06, 0x0a, 0x14, 0x1e, 0x08, 0x09, 0x0c, 0x10};
// ga102_sor_clock: no 0x612300 for the clock any more; per SOR at 0x00ec00 + sor * 0x10                                             [R]
constexpr uint32_t kGa102SorClockDiv = 0x00ec04, kGa102SorClockUnk = 0x00ec08, kGa102SorClockStride = 0x10;
constexpr uint32_t kSorDpActiveSym = 0x61c128;         // [?] + link: VTUa << 24 | VTUf << 16 | VTUi << 8 (mask 0x010f7f3f); TU << 2 in kSorDpCtrl (0x1fc)
constexpr uint32_t kSorDpTuMask = 0x000001fc, kSorDpActiveSymMask = 0x010f7f3f;
// DCB GPIO functions (nouveau include/nvkm/subdev/bios/gpio.h)
constexpr uint8_t kGpioPanelPower = 0x01;              // [S] DCB_GPIO_PANEL_POWER (NvBiosTables.hpp's comment says 0x0c: that is TVDAC0 in nouveau)

// ---------------------------------------------------------------------------------------------------------------------------------------
// Context DMA objects and the RAMHT (nvkm/core/ramht.c, nvkm/engine/dma/user*.c)
// ---------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kRamhtBytes = 0x1000;            // [S] nvkm_ramht_new(device, 0x1000, ...) at the start of the instance memory
constexpr uint32_t kRamhtBits = 9;                  // [S] order_base_2(0x1000 / 8)
constexpr uint32_t kGv100RamhtBytes = 0x2000;       // [R] gv100/tu102/ga102 nvkm_disp_func.ramht_size (nv50_disp_oneinit: 0x1000 when unset)
constexpr uint32_t kGv100RamhtBits = 10;            // [R] order_base_2(0x2000 / 8)
constexpr uint32_t kInstanceBytes = 0x10000;        // [S] gf119/gv100 allocate 64 KiB (aligned to it)
constexpr uint32_t kCtxDmaAlign = 32;               // [R] objects bound with 16..32-byte alignment; 32 used
// The context word of a RAMHT entry: (object offset in the instance memory << shift) | per-generation channel bits
constexpr uint32_t kNv50RamhtShift = 10;            // [R] nv50_disp_chan_bind: -10, chid << 28 | chid
constexpr uint32_t kGf119RamhtShift = 9;            // [R] gf119_disp_chan_bind: -9, chid << 27 | 1
constexpr uint32_t kGv100RamhtShift = 9;            // [?] gv100_disp_chan_bind: -9, chid << 25 | 0x40
// Context DMA layouts (24 bytes pre-Volta, 20 from Volta)
constexpr uint32_t kNv50CtxDmaVram = 0x0011003d;    // [?] class 0x3d | target VRAM 0x10000 | priv US (1 << 20), pitch kind 0
constexpr uint32_t kNv50CtxDmaFlags5 = 0x00010000;  // [?] part 256
constexpr uint32_t kGf119CtxDmaVram = 0x00000009;   // [R] target VRAM (kind pitch, page LP)
constexpr uint32_t kGv100CtxDmaVram = 0x00000001;   // [?] target VRAM; start/limit stored >> 8
// GA102 (c67d/c67e) still names memory through context DMAs (gv100_dmaobj, ga102_dma_new = gv100's); the handle-less SET_SURFACE_ADDRESS
// methods came with Blackwell (ca7d), which this code does not drive.                                                               [R]

// ---------------------------------------------------------------------------------------------------------------------------------------
// Push buffer words (EVO DMA)
// ---------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kPushCountShift = 18;            // [S] header: count << 18 | method (bytes)
constexpr uint32_t kPushCountMax = 0x7ff;           // [R] 11-bit count
constexpr uint32_t kPushJumpToStart = 0x20000000;   // [S] nv50_dmac_wind: jump to offset 0
constexpr uint32_t kPushNonIncrementing = 0x40000000;   // [R] not used here

// Handles of the context DMAs this driver creates (nouveau's NV50_DISP_HANDLE_SYNCBUF / _VRAM)
constexpr uint32_t kHandleNotifier = 0xf0000000;    // [S]
constexpr uint32_t kHandleVram = 0xf0000001;        // [S]

// ---------------------------------------------------------------------------------------------------------------------------------------
// Core channel methods
// ---------------------------------------------------------------------------------------------------------------------------------------
// NV50 (507d and the later Tesla 827d/837d/857d/887d, same layout for these). Head stride 0x400.
namespace m507d {
constexpr uint32_t kUpdate = 0x0080, kNotifierControl = 0x0084, kContextDmaNotifier = 0x0088;   // [S]
constexpr uint32_t kNotifierEnable = 0x80000000;    // [S] NOTIFY 31:31, MODE 30:30 (WRITE 0), OFFSET 11:2 in words
constexpr uint32_t kSorControl = 0x0600, kSorStride = 0x40;   // [S]
constexpr uint32_t kHeadStride = 0x400;
constexpr uint32_t kHeadPixelClock = 0x0804;        // [S] 0x00800000 | kHz
constexpr uint32_t kHeadRasterSize = 0x0814, kHeadSyncEnd = 0x0818, kHeadBlankEnd = 0x081c, kHeadBlankStart = 0x0820, kHeadVertBlank2 = 0x0824;   // [S]
constexpr uint32_t kHeadOffset = 0x0860, kHeadSize = 0x0868, kHeadStorage = 0x086c, kHeadParams = 0x0870, kHeadIso = 0x0874;   // [S]
constexpr uint32_t kHeadPointIn = 0x08c0;           // [S]
constexpr uint32_t kStorageLayoutPitch = 1u << 20;  // [S] MEMORY_LAYOUT 20:20
}  // namespace m507d

// GF119 .. Pascal (907d, 917d, 927d, 947d, 957d, 977d, 987d). Head stride 0x300.
namespace m907d {
constexpr uint32_t kUpdate = 0x0080, kNotifierControl = 0x0084, kContextDmaNotifier = 0x0088;   // [S]
constexpr uint32_t kNotifierEnable = 0x80000000;    // [S] as 507d
constexpr uint32_t kSorControl = 0x0200, kSorStride = 0x20;   // [S] OWNER_MASK 3:0 (a bit per head), PROTOCOL 11:8
constexpr uint32_t kHeadStride = 0x300;
constexpr uint32_t kHeadOutputResource = 0x0404;    // [S] HSYNC_POLARITY 3, VSYNC_POLARITY 4, PIXEL_DEPTH 9:6
constexpr uint32_t kHeadControl = 0x0408;           // [S] 0x31ec6000 | head << 25 | interlace
constexpr uint32_t kHeadControlBase = 0x31ec6000;   // [R] nouveau's magic value
constexpr uint32_t kHeadPresentControl = 0x0410;    // [R] written 0 before the raster
constexpr uint32_t kHeadRasterSize = 0x0414, kHeadSyncEnd = 0x0418, kHeadBlankEnd = 0x041c, kHeadBlankStart = 0x0420, kHeadVertBlank2 = 0x0424;   // [S]
constexpr uint32_t kHeadUnknown42c = 0x042c, kHeadUnknown430 = 0x0430;   // [R] nouveau writes 0, 0xffffff00 ("???")
constexpr uint32_t kHeadPixelClock = 0x0450, kHeadPixelClockConfig = 0x0454, kHeadPixelClockMax = 0x0458;   // [S] Hz, 0x00200000, Hz
constexpr uint32_t kHeadPixelClockConfigValue = 0x00200000;   // [R]
constexpr uint32_t kHeadOffset = 0x0460, kHeadSize = 0x0468, kHeadStorage = 0x046c, kHeadParams = 0x0470, kHeadIso = 0x0474;   // [S]
constexpr uint32_t kHeadViewportPointOut = 0x0494, kHeadPointIn = 0x04b0, kHeadViewportSizeIn = 0x04b8, kHeadViewportSizeOut = 0x04c0;   // [R] size out x3 (out, min, max)
constexpr uint32_t kStorageLayoutPitch = 1u << 24;  // [S] MEMORY_LAYOUT 24:24
constexpr uint32_t kDepth18 = 2, kDepth24 = 5, kDepth30 = 6;   // [R] PIXEL_DEPTH values (BPP_18_444, BPP_24_444, BPP_30_444)
}  // namespace m907d

// SOR protocols (both 507d and 907d SOR_SET_CONTROL.PROTOCOL 11:8)
constexpr uint32_t kProtocolLvds = 0x0, kProtocolTmdsA = 0x1, kProtocolTmdsB = 0x2, kProtocolDualTmds = 0x5, kProtocolDpA = 0x8, kProtocolDpB = 0x9;   // [S]

// Volta (c37d), Turing (c57d) and GA102 (c67d: nouveau drives it with corec57d). Head base 0x2000, stride 0x400.
namespace mc37d {
constexpr uint32_t kUpdate = 0x0200, kContextDmaNotifier = 0x0208, kNotifierControl = 0x020c;   // [S]
constexpr uint32_t kInterlockFlags = 0x0218, kWindowInterlockFlags = 0x021c;   // [R]
constexpr uint32_t kUpdateValue = 0x00000001;       // [R] corec37d_update
constexpr uint32_t kNotifierEnable = 0x00001000;    // [R] NOTIFY 12:12, MODE 0:0, OFFSET 11:4 in 16-byte units
constexpr uint32_t kSorControl = 0x0300, kSorStride = 0x20;   // [R]
constexpr uint32_t kWindowControl = 0x1000, kWindowStride = 0x80;   // [S] WINDOW_SET_CONTROL.OWNER 3:0 (the head; 0xf none)
constexpr uint32_t kWindowOwnerMask = 0xf, kWindowOwnerNone = 0xf;  // [?]
constexpr uint32_t kHeadBase = 0x2000, kHeadStride = 0x400;
constexpr uint32_t kHeadOutputResource = 0x2004, kHeadControl = 0x2008, kHeadPixelClock = 0x200c;   // [S]
constexpr uint32_t kHeadRasterSize = 0x2064, kHeadSyncEnd = 0x2068, kHeadBlankEnd = 0x206c, kHeadBlankStart = 0x2070, kHeadVertBlank2 = 0x2074;   // [S]
// The rest of a mode set (headc37d_mode / _view / _or / _procamp, corec37d_init / _wndw_owner), as the pre-NVVAL evo_mthd numbers were
constexpr uint32_t kHeadProcamp = 0x2000;           // [R] c37d: SAT_COS 15:4 (0x400 = nouveau's default vibrance), SAT_SINE 27:16; c57d+: 0 (RGB, VESA)
constexpr uint32_t kProcampC37dDefault = 0x400u << 4;   // [?]
constexpr uint32_t kHeadPixelClockMax = 0x2028;     // [R] Hz
constexpr uint32_t kHeadUsageBounds = 0x2030;       // [R] "XXX: HEAD_USAGE_BOUNDS, doesn't belong here"
constexpr uint32_t kHeadUsageBoundsC37d = 0x00000124, kHeadUsageBoundsC57d = 0x00001014;   // [?] cursor 256x256, output LUT, upscaling (c57d: + 2 taps)
constexpr uint32_t kHeadViewportSizeIn = 0x204c;    // [R]
constexpr uint32_t kHeadViewportPointOutAdjust = 0x2058, kHeadViewportSizeOut = 0x205c, kHeadViewportSizeOutMin = 0x2060;   // [R]
// HEAD_SET_CONTROL_OUTPUT_RESOURCE: CRC_MODE 1:0, HSYNC_POLARITY 2:2, VSYNC_POLARITY 3:3, PIXEL_DEPTH 7:4 (BPP_18_444 1, BPP_24_444 4, BPP_30_444 5)
constexpr uint32_t kOrCrcComplete = 0x1, kOrHsyncShift = 2, kOrVsyncShift = 3, kOrDepthShift = 4;   // [?]
constexpr uint32_t kDepth18 = 1, kDepth24 = 4, kDepth30 = 5;   // [R] headc37d_or's conversion of nouveau's or.depth (2, 5, 6)
// The windows (corec37d_init): format usage bounds (RGB packed 1/2/4/8 bpp), rotated 0, usage bounds (max pixels per line 0x7fff, input LUT,
// two taps); WINDOW_SET_CONTROL.OWNER = window >> 1 (corec37d_wndw_owner: two windows per head, eight windows assumed by nouveau).
constexpr uint32_t kWindowFormatUsage = 0x1004, kWindowRotatedUsage = 0x1008, kWindowUsage = 0x1010;   // [R] + window * 0x80
constexpr uint32_t kWindowFormatUsageC37d = 0x0000001f, kWindowFormatUsageC57d = 0x0000000f;   // [R]
constexpr uint32_t kWindowUsageC37d = 0x00127fff, kWindowUsageC57d = 0x00117fff;               // [R]
constexpr uint32_t kWindowsPerHead = 2, kWindowsAssigned = 8;
}  // namespace mc37d

// Volta/Turing/GA102 window channel (c37e, c57e, c67e: the same numbers for what is used here; c67e [?], nouveau's wndwc67e differs in
// the colour methods, not remembered to differ in these).
namespace mc37e {
constexpr uint32_t kUpdate = 0x0200, kUpdateValue = 0x00000001;   // [S]
constexpr uint32_t kContextDmaNotifier = 0x021c, kNotifierControl = 0x0220;   // [?] consecutive (wndwc37e_ntfy_set); notifier on while the handle is set
constexpr uint32_t kSize = 0x0224, kStorage = 0x0228, kParams = 0x022c, kPlanarStorage = 0x0230;   // [R] SET_SIZE, SET_STORAGE, SET_PARAMS, SET_PLANAR_STORAGE(0)
constexpr uint32_t kIso = 0x0240;                   // [R] SET_CONTEXT_DMA_ISO(0)
constexpr uint32_t kOffset = 0x0260;                // [R] SET_OFFSET(0), >> 8
constexpr uint32_t kPointIn = 0x0290, kSizeIn = 0x0298, kSizeOut = 0x02a4;   // [R]
constexpr uint32_t kStorageLayoutPitch = 1u << 4;   // [?] MEMORY_LAYOUT 4:4
constexpr uint32_t kPlanarPitchMask = 0x1fff, kPlanarPitchUnit = 64;   // [R] PITCH 12:0 in 64-byte units
constexpr uint32_t kFormatMask = 0xff;              // [R] SET_PARAMS.FORMAT 7:0
// Mode setting from scratch also sends these (wndwc37e_image_set's SET_PRESENT_CONTROL, wndwc37e_blend_set's composition): an opaque window
// at the bottom of the stack (depth 255, K1 = 0xff, source factor K1, destination factor NEG_K1: what nouveau computes for a plane without
// alpha); c57e/c67e use the same numbers for these.
constexpr uint32_t kPresentControl = 0x0308, kPresentInterval1 = 0x00000001;   // [R] BEGIN_MODE 6:4 (0 non-tearing), MIN_PRESENT_INTERVAL 3:0
constexpr uint32_t kCompositionControl = 0x02ec, kCompositionConstantAlpha = 0x02f0, kCompositionFactorSelect = 0x02f4, kCompositionMaxZ = 0x02f8;   // [?]
constexpr uint32_t kCompositionDepthOpaque = 0xffu << 4, kCompositionK1Opaque = 0xff, kCompositionFactorsOpaque = 0x00004422,
                   kCompositionMaxZValue = 0xffff0000;   // [?]
}  // namespace mc37e

// The window immediate channel (c37b, wimmc37b.c): where the window sits on the head.
namespace mc37b {
constexpr uint32_t kUpdate = 0x0200, kUpdateValue = 0x00000001;   // [R] INTERLOCK_WITH_WINDOW 1:1 left 0
constexpr uint32_t kPointOut = 0x0208;              // [R] SET_POINT_OUT(0): y << 16 | x
}  // namespace mc37b

// Notifiers. 507d/907d: word 0 bit 0 DONE.  c37d/c37e (NV_DISP_NOTIFIER): word 0 bits 31:30 STATUS, 2 = FINISHED; 16 bytes.
constexpr uint32_t kNotifierDoneEvo = 0x00000001;   // [R]
constexpr uint32_t kNotifierStatusShift = 30, kNotifierStatusMask = 0x3, kNotifierFinished = 2;   // [R]

// Surface formats (the same codes in every class): A8R8G8B8, X8R8G8B8.
constexpr uint8_t kFormatA8R8G8B8 = 0xcf, kFormatX8R8G8B8 = 0xe6;   // [S]

}  // namespace disp_hw
}  // namespace nv
}  // namespace hardware
