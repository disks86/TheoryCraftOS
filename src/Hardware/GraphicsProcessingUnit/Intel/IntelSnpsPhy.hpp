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

// The Synopsys PHY of DG2 (Arc Alchemist), after the MIT-licensed i915 driver's display/intel_snps_phy.c and intel_ddi_buf_trans.c (Linux v6.12).
// Each of DDI A-D has a PHY of its own with a port PLL inside it, the MPLLB; nothing is shared between ports, so there is no DPLL manager. The MPLLB
// is not computed: like i915 (whose comment says a proper algorithm needs a license it does not have) a rate is set only if a table has it - the DP
// link rates RBR, HBR, HBR2 and HBR3, and 44 HDMI TMDS clocks. The tables are generated (IntelSnpsPhyTables.inc) from i915's.
//
// Programmed here: the MPLLB registers and its enable through DG2_PLL_ENABLE (intel_mpllb_enable / _disable), the per-lane TX equalisation of a
// DP drive level or of HDMI (intel_snps_phy_set_signal_levels with dg2_snps_trans), the wait for the PHYs' calibration at display init.
// The UHBR rates (128b/132b) and eDP's own rates are not taken.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelDg2Registers.hpp"

namespace hardware {
namespace intel_dg2 {

struct MpllbState {
  uint32_t clock_khz;      // the DP link clock (162000 ...) or the HDMI TMDS clock
  uint32_t ref_control, cp, div, div2, fracn1, fracn2, sscen, sscstep;
};

ArrayView<const MpllbState> dp_mpllb_tables();
ArrayView<const MpllbState> hdmi_mpllb_tables();
// The state for a DP link clock or an HDMI TMDS clock: exact matches only (intel_mpllb_calc_state). Null if there is none.
const MpllbState* find_dp_mpllb(uint32_t link_khz);
const MpllbState* find_hdmi_mpllb(uint32_t tmds_khz);
// The port clock a state produces (intel_mpllb_calc_port_clock, 100 MHz reference): the check that the table and the formula agree.
uint32_t mpllb_port_clock_khz(const MpllbState& state);

// One entry of dg2_snps_trans: what SNPS_PHY_TX_EQ gets for a drive level.
struct SnpsTrans {
  uint8_t vswing, pre_cursor, post_cursor;
};
constexpr uint32_t kSnpsTransEntries = 10;
constexpr uint32_t kHdmiDefaultLevel = kSnpsTransEntries - 1;   // hdmi_default_entry (no VBT level shift)
const SnpsTrans& snps_trans(uint32_t level);
// The level of a DP_TRAINING_LANEx_SET value (translate_signal_level: VS0 PE0..3, VS1 PE0..2, VS2 PE0..1, VS3 PE0); 0 for a combination there is none of.
uint32_t dp_level(uint8_t train_set);
// SNPS_PHY_TX_EQ's value for a level.
uint32_t tx_eq_value(uint32_t level);

class SnpsPhy {
 public:
  SnpsPhy(intel_gt::IRegisterBank& registers, const IClock& clock) : r_(registers), clock_(clock) {}
  // The MPLLB of `port`'s PHY set to `state` and enabled, then forced on, then the lock waited for (5 ms). False when it did not lock.
  [[nodiscard]] bool enable_pll(const PortInfo& port, const MpllbState& state);
  // Off (enable cleared, force cleared, the lock waited out). False if it would not let go.
  bool disable_pll(const PortInfo& port);
  // The same level on all four lanes.
  void set_levels(const PortInfo& port, uint32_t level);
  // intel_snps_phy_wait_for_calibration: DG2_PHY_MISC's TX acknowledge bits of each drivable port's PHY clear within 25 ms. The PHYs that did not
  // calibrate are returned as a bit mask by phy (i915 notes it and carries on).
  uint32_t wait_for_calibration();

 private:
  intel_gt::IRegisterBank& r_;
  const IClock& clock_;
};

}  // namespace intel_dg2
}  // namespace hardware
