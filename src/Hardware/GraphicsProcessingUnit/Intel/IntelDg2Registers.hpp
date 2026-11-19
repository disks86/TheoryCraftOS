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

// The display registers of DG2 (Arc Alchemist, display version 13, "Xe_HPD") that the secondary display driver programs, with the polling helper
// every step uses. Offsets and bits are copied from the MIT-licensed i915 driver, Linux v6.12, the file named on each block (fetched from
// git.kernel.org and read, not recalled). Everything is pipe A / transcoder A: the driver lights one output.
#pragma once
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_dg2 {

// ------------------------------------------------------------------ DC states, PCH handshake, workarounds (i915_reg.h)
constexpr uint64_t kDcStateEn = 0x45504;                 // DC_STATE_EN; 0 = DC_STATE_DISABLE
constexpr uint64_t kNdeRstwrnOpt = 0x46408;              // HSW_NDE_RSTWRN_OPT
constexpr uint32_t kResetPchHandshakeEnable = 1u << 4;
constexpr uint64_t kChickenDcpr2 = 0x46434;              // GEN11_CHICKEN_DCPR_2 (Wa_14011508470)
constexpr uint32_t kDcpr2Wa14011508470 = (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27);
constexpr uint64_t kDisplayErrFatalMask = 0x4421C;       // XELPD_DISPLAY_ERR_FATAL_MASK (Wa_14011503030: ~0)

// ------------------------------------------------------------------ power wells (i915_reg.h, intel_display_power_map.c xehpd_power_wells)
// The driver's request register of each set (the BIOS's is 4 bytes below, the debug one 8 above). Request bit 2 * idx + 1, state bit 2 * idx.
constexpr uint64_t kPwrWellCtl2 = 0x45404;               // HSW_PWR_WELL_CTL2: PW_1 .. PW_D
constexpr uint64_t kPwrWellCtlAux2 = 0x45444;            // ICL_PWR_WELL_CTL_AUX2
constexpr uint64_t kPwrWellCtlDdi2 = 0x45454;            // ICL_PWR_WELL_CTL_DDI2
constexpr uint32_t pw_request(uint32_t idx) { return 2u << (idx * 2); }
constexpr uint32_t pw_state(uint32_t idx) { return 1u << (idx * 2); }
constexpr uint32_t kPwIdx1 = 0, kPwIdx2 = 1, kPwIdxA = 5, kPwIdxB = 6, kPwIdxC = 7, kPwIdxD = 8;   // ICL_PW_CTL_IDX_PW_1/2, XELPD_PW_CTL_IDX_PW_A..D
constexpr uint64_t kFuseStatus = 0x42000;                // SKL_FUSE_STATUS
constexpr uint32_t fuse_pg_dist(uint32_t pg) { return 1u << (27 - pg); }   // SKL_FUSE_PG_DIST_STATUS
constexpr uint32_t pg_of_well(uint32_t idx) { return idx + 1; }            // ICL_PW_CTL_IDX_TO_PG: PW_1 is PG1, PW_A (idx 5) is "PG6"
constexpr uint32_t kAuxWellFixedDelayUs = 600;           // DG2: the AUX wells are not watched; 600-1200 us (hsw_wait_for_power_well_enable)

// ------------------------------------------------------------------ CDCLK (i915_reg.h, intel_cdclk.c dg2_cdclk_table)
constexpr uint64_t kCdclkCtl = 0x46000;                  // CDCLK_CTL
constexpr uint32_t kCdclkCd2xDiv1 = 0u << 22;            // BXT_CDCLK_CD2X_DIV_SEL_1
constexpr uint32_t kCdclkCd2xPipeNone = 7u << 19;        // TGL_CDCLK_CD2X_PIPE_NONE
constexpr uint32_t kCdclkDecimalMask = 0x7FF;
constexpr uint64_t kCdclkSquashCtl = 0x46008;            // CDCLK_SQUASH_CTL
constexpr uint32_t kSquashEnable = 1u << 31, kSquashWindow15 = 0xFu << 24;
constexpr uint64_t kDePllEnable = 0x46070;               // BXT_DE_PLL_ENABLE: ratio in bits 7:0 (ICL_CDCLK_PLL_RATIO)
constexpr uint32_t kDePllEnableBit = 1u << 31, kDePllLock = 1u << 30, kDePllRatioMask = 0xFF;
constexpr uint32_t kCdclkRefKhz = 38400, kCdclkRatio = 34;            // DG2: a fixed 38.4 MHz reference and one VCO (1305.6 MHz)
constexpr uint32_t kCdclkVcoKhz = kCdclkRefKhz * kCdclkRatio;

// ------------------------------------------------------------------ DBUF and MBUS (skl_watermark_regs.h, i915_reg.h)
inline constexpr uint64_t kDbufCtlS[4] = {0x45008, 0x44FE8, 0x44300, 0x44304};   // DBUF_CTL_S(0..3)
constexpr uint32_t kDbufPowerRequest = 1u << 31, kDbufPowerState = 1u << 30;
constexpr uint32_t kDbufTrackerMask = 0x1Fu << 19, kDbufTracker8 = 8u << 19;   // DBUF_TRACKER_STATE_SERVICE(8)
inline constexpr uint64_t kMbusAboxCtl[2] = {0x45038, 0x45048};                 // MBUS_ABOX_CTL(0), (1): abox_mask GENMASK(1, 0)
constexpr uint32_t kAboxCreditMask = (3u << 20) | (0xFu << 16) | (0x1Fu << 8) | 0x1Fu;
constexpr uint32_t kAboxCredits = (1u << 20) | (1u << 16) | (16u << 8) | 16u;   // icl_mbus_init: BW 1, B 1, BT pools 16 / 16
constexpr uint64_t kPipeMbusDboxCtlA = 0x7003C;                           // PIPE_MBUS_DBOX_CTL(PIPE_A)
// intel_mbus_dbox_update, display version 12/13 (not ADL-P): B2B max 16, delay 1, regulate, A credit 2, BW credit 2, B credit 12
constexpr uint32_t kDboxValue = (16u << 20) | (1u << 17) | (1u << 16) | (2u << 14) | (12u << 8) | 2u;
constexpr uint32_t kDbufBlocksPerSlice = 1024;          // dbuf.size 4096 over four slices; DG2 gives pipe A slices S1 and S2 (dg2_allowed_dbufs)

// ------------------------------------------------------------------ ports, PLLs, PHY (i915_reg.h, intel_snps_phy_regs.h, intel_display.c)
constexpr uint64_t ddi_buf_ctl(uint32_t port) { return 0x64000 + port * 0x100ull; }      // DDI_BUF_CTL
constexpr uint32_t kDdiBufEnable = 1u << 31, kDdiBufIsIdle = 1u << 7;
constexpr uint32_t ddi_port_width(uint32_t lanes) { return (lanes - 1) << 1; }
constexpr uint32_t kDdiBufActiveTimeoutUs = 1200;       // intel_wait_ddi_buf_active: DG2
constexpr uint64_t aux_ch_ctl(uint32_t port) { return 0x64010 + port * 0x100ull; }       // DP_AUX_CH_CTL (the AUX channel number equals the port's on DG2)
constexpr uint64_t aux_ch_data(uint32_t port, uint32_t i) { return 0x64014 + port * 0x100ull + i * 4ull; }
constexpr uint32_t kPllEnable = 1u << 31, kPllLock = 1u << 30;                           // DPLL_ENABLE bits (DG2_PLL_ENABLE, MG_PLL_ENABLE)
constexpr uint64_t snps_phy(uint32_t phy, uint64_t reg_a) { return reg_a + phy * 0x1000ull; }   // _MMIO_SNPS: PHY A at 0x168000, B 0x1000 further
constexpr uint64_t kSnpsMpllbCp = 0x168000, kSnpsMpllbDiv = 0x168004, kSnpsMpllbFracn1 = 0x168008, kSnpsMpllbFracn2 = 0x16800C,
                   kSnpsMpllbSscen = 0x168014, kSnpsMpllbSscstep = 0x168018, kSnpsMpllbDiv2 = 0x16801C, kSnpsRefControl = 0x168188,
                   kSnpsTxReq = 0x168200;
constexpr uint64_t snps_tx_eq(uint32_t phy, uint32_t lane) { return 0x168300 + phy * 0x1000ull + lane * 0x10ull; }
constexpr uint32_t kSnpsMpllbForceEn = 1u << 31;
constexpr uint32_t kSnpsRefRangeMask = 0x1Fu << 27;
constexpr uint32_t kPhyMiscDpTxAckMask = 0xFu << 20;    // DG2_PHY_DP_TX_ACK_MASK (calibration done when clear)
constexpr uint32_t kSnpsCalibrationTimeoutUs = 25000;

// ------------------------------------------------------------------ transcoder A (i915_reg.h)
constexpr uint64_t kTransHtotalA = 0x60000, kTransHblankA = 0x60004, kTransHsyncA = 0x60008, kTransVtotalA = 0x6000C, kTransVblankA = 0x60010,
                   kTransVsyncA = 0x60014, kPipeSrcA = 0x6001C, kTransVsyncShiftA = 0x60028, kTransMultA = 0x6002C, kPipeDataM1A = 0x60030,
                   kPipeDataN1A = 0x60034, kPipeLinkM1A = 0x60040, kPipeLinkN1A = 0x60044, kTransSetContextLatencyA = 0x6007C;
constexpr uint32_t kTuSize64 = 63u << 25;               // TU_SIZE(64)
constexpr uint32_t kDataLinkMnMask = 0xFFFFFF;          // DATA_LINK_M_N_MASK
constexpr uint64_t kTransDdiFuncCtlA = 0x60400, kTransDdiFuncCtl2A = 0x60404;
constexpr uint32_t kTransDdiFuncEnable = 1u << 31;
constexpr uint32_t trans_ddi_select_port(uint32_t port) { return (port + 1) << 27; }    // TGL_TRANS_DDI_SELECT_PORT
constexpr uint32_t kTransDdiModeHdmi = 0u << 24, kTransDdiModeDvi = 1u << 24, kTransDdiModeDpSst = 2u << 24;
constexpr uint32_t kTransDdiBpc8 = 0u << 20, kTransDdiPvsync = 1u << 17, kTransDdiPhsync = 1u << 16;
constexpr uint64_t kDpTpCtlA = 0x60540, kDpTpStatusA = 0x60544;   // TGL_DP_TP_CTL / _STATUS (per transcoder from display version 12)
constexpr uint32_t kDpTpEnable = 1u << 31, kDpTpModeSst = 0u << 27, kDpTpEnhancedFrame = 1u << 18, kDpTpTrainMask = 7u << 8;
constexpr uint32_t kDpTpPat1 = 0u << 8, kDpTpPat2 = 1u << 8, kDpTpIdle = 2u << 8, kDpTpNormal = 3u << 8, kDpTpPat3 = 4u << 8;
constexpr uint32_t kDpTpStatusIdleDone = 1u << 25;
constexpr uint64_t kTransClkSelA = 0x46140;             // TRANS_CLK_SEL(TRANSCODER_A)
constexpr uint32_t trans_clk_sel_port(uint32_t phy) { return (phy + 1) << 28; }        // TGL_TRANS_CLK_SEL_PORT, by PHY on display version 13
constexpr uint64_t kChickenTransA = 0x420C0;            // CHICKEN_TRANS(A): HSW_FRAME_START_DELAY in bits 28:27
constexpr uint32_t kFrameStartDelayMask = 3u << 27;

// ------------------------------------------------------------------ pipe A and its primary plane (i915_reg.h, skl_universal_plane_regs.h)
constexpr uint64_t kTransConfA = 0x70008, kPipeMiscA = 0x70030;
constexpr uint32_t kTransConfEnable = 1u << 31, kTransConfState = 1u << 30;
constexpr uint32_t kPipeMiscBpc8 = 0u << 5, kPipeMiscRoundingTrunc = 1u << 8;
constexpr uint64_t kPlaneCtlA = 0x70180, kPlaneStrideA = 0x70188, kPlanePosA = 0x7018C, kPlaneSizeA = 0x70190, kPlaneSurfA = 0x7019C,
                   kPlaneOffsetA = 0x701A4, kPlaneAuxDistA = 0x701C0, kPlaneColorCtlA = 0x701CC, kPlaneWmA0 = 0x70240, kPlaneWmTransA = 0x70268,
                   kPlaneBufCfgA = 0x7027C;
constexpr uint32_t kPlaneEnable = 1u << 31, kPlaneFormatXrgb8888 = 4u << 24;           // tiling 0 = linear, order BGRX
constexpr uint32_t kPlaneColorGammaDisable = 1u << 13;  // PLANE_COLOR_PLANE_GAMMA_DISABLE (alpha disabled: 0)
constexpr uint32_t kPlaneWmEnable = 1u << 31, kPlaneWmLinesShift = 14, kPlaneWmLinesMask = 0x1FFF, kPlaneWmBlocksMask = 0xFFF;
constexpr uint32_t kPlaneWmLevels = 8;

// ------------------------------------------------------------------ GMBUS (intel_gmbus_regs.h; the PCH display base 0xC0000 on DG2)
constexpr uint64_t kGmbus0 = 0xC5100, kGmbus1 = 0xC5104, kGmbus2 = 0xC5108, kGmbus3 = 0xC510C, kGmbus4 = 0xC5110, kGmbus5 = 0xC5120;
constexpr uint32_t kGmbusRate100k = 0u << 8;
constexpr uint32_t kGmbusSwClrInt = 1u << 31, kGmbusSwRdy = 1u << 30, kGmbusCycleWait = 1u << 25, kGmbusCycleIndex = 2u << 25, kGmbusCycleStop = 4u << 25;
constexpr uint32_t kGmbusByteCountShift = 16, kGmbusSlaveIndexShift = 8, kGmbusSlaveAddrShift = 1, kGmbusSlaveRead = 1;
constexpr uint32_t kGmbusHwWaitPhase = 1u << 14, kGmbusHwRdy = 1u << 11, kGmbusSatoer = 1u << 10, kGmbusActive = 1u << 9;
constexpr uint32_t kGmbusMaxBytes = 511;                // GEN9_GMBUS_BYTE_COUNT_MAX

// ------------------------------------------------------------------ the SoC part (i915_reg.h, gt/intel_gtt.h)
constexpr uint64_t kGuCntl = 0x101010;                  // GU_CNTL
constexpr uint32_t kLmemInit = 1u << 7;                 // LMEM_INIT: the card's firmware has trained local memory
constexpr uint64_t kGgttPteLm = 1ull << 1;              // GEN12_GGTT_PTE_LM: the page is in local memory

// ------------------------------------------------------------------ the ports of DG2 (xe_hpd_display port_mask; intel_port_to_phy, intel_gmbus.c gmbus_pins_dg2)
enum class Ddi : uint8_t { A, B, C, D, Tc1 };
struct PortInfo {
  Ddi ddi;
  const char* name;
  uint8_t port;          // enum port: DDI_BUF_CTL, DP_AUX_CH and TRANS_DDI_FUNC_CTL's port select (A 0, B 1, C 2, TC1 3, D_XELPD 7)
  uint8_t phy;           // enum phy: the SNPS PHY's registers, TRANS_CLK_SEL
  uint8_t aux_well;      // index in the AUX power well set
  uint8_t ddi_well;      // index in the DDI IO power well set
  uint8_t gmbus_pin;     // the DDC pin of an HDMI output (dg1_encoder_to_ddc_pin: phy + 1; TC1 9)
  bool needs_pg2;        // the port's lanes and AUX are in the PG2 domains (XELPD_DC_OFF_PORT_POWER_DOMAINS)
  bool drivable;         // driven by this driver (not TC1: see kPorts)
  uint64_t pll_enable;   // DG2_PLL_ENABLE(phy) or MG_PLL_ENABLE(0)
  uint64_t phy_misc;     // DG2_PHY_MISC(phy)
};
// TC1 is listed but not driven: i915 v6.12 maps port TC1 to PHY_F (intel_port_to_phy), for which intel_phy_is_snps() is false, while its comments call it
// "PHY E" (DG2_PHY_MISC special-cases it at 0x64C14). Which SNPS register block and TRANS_CLK_SEL value it takes is therefore not settled here.
inline constexpr PortInfo kPorts[] = {
    {Ddi::A, "DDI A", 0, 0, 0, 0, 1, false, true, 0x46010, 0x64C00},
    {Ddi::B, "DDI B", 1, 1, 1, 1, 2, false, true, 0x46014, 0x64C04},
    {Ddi::C, "DDI C", 2, 2, 2, 2, 3, true, true, 0x46018, 0x64C08},
    {Ddi::D, "DDI D", 7, 3, 7, 7, 4, true, true, 0x4601C, 0x64C0C},
    {Ddi::Tc1, "DDI TC1", 3, 4, 3, 3, 9, true, false, 0x46030, 0x64C14},
};
constexpr uint32_t kPortCount = sizeof(kPorts) / sizeof(kPorts[0]);

// ------------------------------------------------------------------ waiting
// Polls `offset` until (value & mask) == want; false if `timeout_us` passed first.
[[nodiscard]] inline bool wait_for(intel_gt::IRegisterBank& r, const IClock& clock, uint64_t offset, uint32_t mask, uint32_t want, uint32_t timeout_us) {
  const uint64_t start = clock.now_us();
  for (;;) {
    if ((r.read32(offset) & mask) == want) return true;
    if (clock.now_us() - start > timeout_us) return (r.read32(offset) & mask) == want;
  }
}
inline void delay_us(const IClock& clock, uint32_t us) {
  const uint64_t start = clock.now_us();
  while (clock.now_us() - start < us) {}
}
inline void rmw(intel_gt::IRegisterBank& r, uint64_t offset, uint32_t clear, uint32_t set) { r.write32(offset, (r.read32(offset) & ~clear) | set); }

}  // namespace intel_dg2
}  // namespace hardware
