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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelSnpsPhy.hpp"

namespace hardware {
namespace intel_dg2 {

namespace {
#include "Hardware/GraphicsProcessingUnit/Intel/IntelSnpsPhyTables.inc"

// dg2_snps_trans (intel_ddi_buf_trans.c): VS 0 PE 0..3, VS 1 PE 0..2, VS 2 PE 0..1, VS 3 PE 0.
constexpr SnpsTrans kTrans[kSnpsTransEntries] = {
    {25, 0, 0}, {32, 0, 6}, {35, 0, 10}, {43, 0, 17}, {35, 0, 0}, {45, 0, 8}, {48, 0, 14}, {47, 0, 0}, {55, 0, 7}, {62, 0, 0},
};
constexpr uint32_t kPllLockTimeoutUs = 5000;   // intel_de_wait_for_set(..., PLL_LOCK, 5)

const MpllbState* find(ArrayView<const MpllbState> tables, uint32_t khz) {
  for (const MpllbState& s : tables)
    if (s.clock_khz == khz) return &s;
  return nullptr;
}
}  // namespace

ArrayView<const MpllbState> dp_mpllb_tables() { return ArrayView<const MpllbState>(kDpTables); }
ArrayView<const MpllbState> hdmi_mpllb_tables() { return ArrayView<const MpllbState>(kHdmiTables); }
const MpllbState* find_dp_mpllb(uint32_t link_khz) { return find(dp_mpllb_tables(), link_khz); }
const MpllbState* find_hdmi_mpllb(uint32_t tmds_khz) { return find(hdmi_mpllb_tables(), tmds_khz); }

uint32_t mpllb_port_clock_khz(const MpllbState& s) {
  uint64_t refclk = 100000;
  const uint32_t ref_div = (s.div2 >> 12) & 7;                       // SNPS_PHY_MPLLB_REF_CLK_DIV
  if (ref_div == 0) return 0;
  refclk >>= ref_div - 1;
  uint64_t quot = 0, rem = 0, den = 1;
  if (s.fracn1 & (1u << 31)) {                                       // SNPS_PHY_MPLLB_FRACN_EN
    quot = s.fracn2 & 0xFFFF;
    rem = s.fracn2 >> 16;
    den = s.fracn1 & 0xFFFF;
    if (den == 0) return 0;
  }
  const uint64_t multiplier = (s.div2 & 0xFFF) / 2 + 16;            // SNPS_PHY_MPLLB_MULTIPLIER
  const uint32_t tx_clk_div = (s.div >> 5) & 7;                      // SNPS_PHY_MPLLB_TX_CLK_DIV
  const uint64_t divisor = uint64_t{10} << (tx_clk_div + 16);
  const uint64_t numerator = refclk * ((multiplier << 16) + quot) + (refclk * rem + den / 2) / den;
  return static_cast<uint32_t>((numerator + divisor / 2) / divisor);
}

const SnpsTrans& snps_trans(uint32_t level) { return kTrans[level < kSnpsTransEntries ? level : 0]; }

uint32_t dp_level(uint8_t train_set) {
  const uint32_t vs = train_set & 3, pe = (train_set >> 3) & 3;
  static constexpr uint8_t kFirst[4] = {0, 4, 7, 9};                 // the first level of each voltage swing
  static constexpr uint8_t kCount[4] = {4, 3, 2, 1};
  return pe < kCount[vs] ? kFirst[vs] + pe : 0;
}

uint32_t tx_eq_value(uint32_t level) {
  const SnpsTrans& t = snps_trans(level);
  return (uint32_t{t.vswing} & 0x3F) << 18 | (uint32_t{t.post_cursor} & 0x3F) << 10 | (uint32_t{t.pre_cursor} & 0x3F) << 2;   // TX_EQ_MAIN, _POST, _PRE
}

bool SnpsPhy::enable_pll(const PortInfo& port, const MpllbState& s) {
  // 3. the PLL registers for the frequency (REF_CONTROL is the firmware's; i915 never writes it)
  r_.write32(snps_phy(port.phy, kSnpsMpllbCp), s.cp);
  r_.write32(snps_phy(port.phy, kSnpsMpllbDiv), s.div);
  r_.write32(snps_phy(port.phy, kSnpsMpllbDiv2), s.div2);
  r_.write32(snps_phy(port.phy, kSnpsMpllbSscen), s.sscen);
  r_.write32(snps_phy(port.phy, kSnpsMpllbSscstep), s.sscstep);
  r_.write32(snps_phy(port.phy, kSnpsMpllbFracn1), s.fracn1);
  r_.write32(snps_phy(port.phy, kSnpsMpllbFracn2), s.fracn2);
  // 5. PLL enable; 9. the force bit (only after the enable, so the PLL has sampled the dividers); 10. the lock
  rmw(r_, port.pll_enable, 0, kPllEnable);
  r_.write32(snps_phy(port.phy, kSnpsMpllbDiv), s.div | kSnpsMpllbForceEn);
  return wait_for(r_, clock_, port.pll_enable, kPllLock, kPllLock, kPllLockTimeoutUs);
}

bool SnpsPhy::disable_pll(const PortInfo& port) {
  rmw(r_, port.pll_enable, kPllEnable, 0);
  rmw(r_, snps_phy(port.phy, kSnpsMpllbDiv), kSnpsMpllbForceEn, 0);
  return wait_for(r_, clock_, port.pll_enable, kPllLock, 0, kPllLockTimeoutUs);
}

void SnpsPhy::set_levels(const PortInfo& port, uint32_t level) {
  for (uint32_t lane = 0; lane < 4; ++lane) r_.write32(snps_tx_eq(port.phy, lane), tx_eq_value(level));
}

uint32_t SnpsPhy::wait_for_calibration() {
  uint32_t failed = 0;
  for (const PortInfo& p : kPorts) {
    if (!p.drivable) continue;
    if (!wait_for(r_, clock_, p.phy_misc, kPhyMiscDpTxAckMask, 0, kSnpsCalibrationTimeoutUs)) failed |= 1u << p.phy;
  }
  return failed;
}

}  // namespace intel_dg2
}  // namespace hardware
