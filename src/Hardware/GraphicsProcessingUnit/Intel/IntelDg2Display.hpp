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

// The display engine of DG2 (Arc Alchemist, display version 13) programmed from scratch, step by step as the MIT-licensed i915 driver (Linux v6.12)
// does it: nobody lit a screen on this GPU, so every clock, power well, PLL, port, transcoder, pipe and plane starts off.
//
//   - Display core init (intel_display_power.c icl_display_core_init): DC states off, the PCH reset handshake, power well 1 (with the PG0 and PG1
//     fuse distribution waited for), CDCLK, the DBUF slices pipe A uses (S1 and S2 on DG2) and their tracker setting, the MBUS ABOX credits, the
//     SNPS PHYs' calibration, Wa_14011508470 and Wa_14011503030. Then PG A (pipe A) and PG2 (DDI C and D and their AUX), which hang off PG1 in
//     Xe_LPD's power well tree. No BW_BUDDY (not used on discrete parts after DG1), no DMC (see below).
//   - CDCLK (intel_cdclk.c): DG2's one VCO (34 x 38.4 MHz) and the squash waveform of dg2_cdclk_table; no PCODE handshake on DG2.
//   - The AUX channel of each DDI (intel_dp_aux.c, the Xe_LPD register set; the DRM helper's native and I2C-over-AUX transfers on top), the GMBUS
//     DDC controller (intel_gmbus.c) for HDMI EDIDs.
//   - The output: DisplayPort SST (tgl_ddi_pre_enable_dp: the MPLLB, the DDI IO well, TRANS_CLK_SEL, TRANS_DDI_FUNC_CTL, the drive levels, link
//     training through DpLinkTrainer with this DDI as the source, idle and normal patterns) or HDMI/DVI (intel_ddi_pre_enable_hdmi /
//     intel_enable_ddi_hdmi: the MPLLB at the TMDS clock, the HDMI drive level, DDI_BUF_CTL), then the transcoder (M/N for DP, the timings, the
//     frame start delay, TRANSCONF), PIPE_MISC, the plane's DDB and watermark, the plane, and TRANS_DDI_FUNC_CTL and TRANSCONF enabled.
//
// The DMC firmware (i915 loads dg2_dmc_ver2_08.bin): it saves and restores display state across the DC5/DC6 power states and handles PG1 during
// them. As far as i915 shows, a mode set does not need it - intel_dmc.c treats missing firmware as "DC states disabled" and the display works, and
// this driver keeps DC states off (DC_STATE_EN 0) all the time. Confidence: medium-high; it is a reading of i915, not a test on a card.
//
// Assumed or simplified (each named where it is done): the plane watermark is one level (0), computed for an assumed 20 us memory latency
// instead of PCODE's latency table (DG2 reads it through the PCODE mailbox), with SAGV never enabled; the DP M/N values use the classic formula without
// i915 6.12's bandwidth-overhead factor (SSC, FEC); no infoframes, audio or HDMI 2.0 scrambling (so TMDS at most 340 MHz); 8 bits per colour;
// one pipe (A); no hot plug interrupts (outputs are found by asking each AUX channel and DDC bus).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelSnpsPhy.hpp"

namespace hardware {
namespace intel_dg2 {

// ------------------------------------------------------------------ the AUX channel of a DDI
class DpAux final : public II2cBus, public IDpcdAux {
 public:
  static constexpr uint8_t kI2cWrite = 0x0, kI2cRead = 0x1, kI2cMot = 0x4, kNativeWrite = 0x8, kNativeRead = 0x9;   // DP_AUX_* requests
  static constexpr uint8_t kReplyAck = 0x0, kNativeNack = 0x1, kNativeDefer = 0x2, kI2cNack = 0x4, kI2cDefer = 0x8;  // reply nibble
  static constexpr uint32_t kMaxPayload = 16;
  static constexpr int32_t kTimedOut = -1, kIo = -2, kBusy = -3, kInvalid = -4;

  DpAux(intel_gt::IRegisterBank& registers, const IClock& clock, const PortInfo& port) : r_(registers), clock_(clock), port_(port) {}

  // intel_dp_aux_xfer: `send` (at most 20 bytes) out, up to recv.size() bytes of the reply back. The bytes received, or a negative result.
  int32_t xfer(ConstByteView send, ByteView recv);
  // One AUX message (intel_dp_aux_transfer): the reply nibble in *reply; the payload bytes moved, or a negative result.
  int32_t transfer(uint8_t request, uint32_t address, ByteView buffer, uint8_t* reply);

  bool dpcd_read(uint32_t address, ByteView out) override;
  bool dpcd_write(uint32_t address, ConstByteView data) override;
  // I2C over AUX (drm_dp_i2c_xfer): each message opened with a bare address request (MOT), its data in 16-byte pieces, the transaction closed with
  // a bare address without MOT.
  bool transfer(ArrayView<Message> messages) override;

 private:
  bool native(uint8_t request, uint32_t address, ByteView buffer);
  bool i2c_piece(uint8_t request, uint8_t address, ByteView buffer);

  intel_gt::IRegisterBank& r_;
  const IClock& clock_;
  const PortInfo& port_;
};

// ------------------------------------------------------------------ the GMBUS DDC controller, on one pin
class Gmbus final : public II2cBus {
 public:
  Gmbus(intel_gt::IRegisterBank& registers, const IClock& clock, uint8_t pin) : r_(registers), clock_(clock), pin_(pin) {}
  // do_gmbus_xfer: a 1-byte write followed by a message to the same address goes as one INDEX cycle; then a STOP cycle and the wait for idle. On a
  // NAK the controller is reset (SW_CLR_INT).
  bool transfer(ArrayView<Message> messages) override;

 private:
  bool wait(uint32_t status);
  bool read(uint8_t address, ByteView out, uint32_t index_cycle);
  bool write(uint8_t address, ConstByteView data);
  void reset();

  intel_gt::IRegisterBank& r_;
  const IClock& clock_;
  uint8_t pin_;
};

// ------------------------------------------------------------------ what a mode set needs
enum class Output : uint8_t { DisplayPort, Hdmi, Dvi };

struct ModeRequest {
  const PortInfo* port = nullptr;
  Output output = Output::Hdmi;
  CrtcMode mode;
  // DisplayPort: the link (DpLinkConfig::choose) and the sink, trained through `aux`.
  uint32_t lanes = 0, link_khz = 0;
  IDpcdAux* aux = nullptr;
  Dpcd::Capabilities caps;
  // The picture: a linear XRGB8888 surface at a graphics (GGTT) address, `stride_bytes` a multiple of 64.
  uint64_t surface = 0;
  uint32_t stride_bytes = 0;
};

enum class Stage : uint8_t { Done, Unsupported, Cdclk, Pll, DdiPower, LinkTraining, Buffer, Transcoder };
const char* stage_name(Stage stage);

// DP M/N (intel_link_compute_m_n with fixed N, the classic data ratio): TU 64.
struct LinkMn {
  uint32_t data_m = 0, data_n = 0, link_m = 0, link_n = 0;
};
LinkMn compute_link_mn(uint32_t bits_per_pixel, uint32_t lanes, uint32_t pixel_khz, uint32_t link_khz);
// The lowest CDCLK of dg2_cdclk_table that carries `pixel_khz` (two pixels per clock); 0 if none does.
uint32_t cdclk_for(uint32_t pixel_khz);
// CDCLK_SQUASH_CTL's waveform for a table entry (0 for a clock not in the table).
uint16_t cdclk_waveform(uint32_t cdclk_khz);
// The level-0 watermark in DBUF blocks and lines for a linear 4-byte plane (assumed latency, see above).
struct Watermark {
  uint32_t blocks = 0, lines = 0;
};
Watermark plane_watermark(const CrtcMode& mode, uint32_t ddb_blocks);
constexpr uint32_t kAssumedLatencyUs = 20;
constexpr uint32_t kPlaneDdbBlocks = 1024;     // DBUF slice S1, the first of pipe A's two

class Engine {
 public:
  Engine(intel_gt::IRegisterBank& registers, const IClock& clock) : r_(registers), clock_(clock), phy_(registers, clock) {}

  // Display core init and the power wells of pipe A, PG2. False (and why() says which) when a well, the CDCLK PLL or a DBUF slice did not come up.
  [[nodiscard]] bool init();
  // Power wells of one port: its AUX (before any AUX or DDC traffic) and its DDI IO lanes.
  [[nodiscard]] bool enable_aux_power(const PortInfo& port);
  void disable_aux_power(const PortInfo& port);
  [[nodiscard]] bool enable_ddi_power(const PortInfo& port);
  void disable_ddi_power(const PortInfo& port);
  // CDCLK to a table entry (squash only once the PLL runs: every DG2 entry has the same VCO).
  [[nodiscard]] bool set_cdclk(uint32_t cdclk_khz);
  uint32_t cdclk_khz() const { return cdclk_; }
  uint32_t uncalibrated_phys() const { return uncalibrated_; }

  // The whole mode set on pipe A / transcoder A. On a failure what was enabled is taken down again; the stage that failed is returned.
  [[nodiscard]] Stage enable(const ModeRequest& request);
  // The outcome of the last DisplayPort link training.
  const DpLinkTrainer::Outcome& training() const { return training_; }
  // Plane, transcoder, port, PLL and DDI IO well off (the output enable() lit).
  void disable();
  bool lit() const { return lit_ != nullptr; }
  // Points the plane at another surface of the same size (the plane's registers armed by PLANE_SURF).
  void flip(uint64_t surface);
  const char* why() const { return why_; }

 private:
  class Source;
  bool fail(const char* why) {
    why_ = why;
    return false;
  }
  bool enable_well(uint64_t ctl, uint32_t idx, bool fuses, bool fixed_delay);
  void disable_well(uint64_t ctl, uint32_t idx);
  void program_timings(const CrtcMode& mode);
  void program_plane(const ModeRequest& request);
  void teardown(const PortInfo& port, Output output, bool pll, bool ddi_power);

  intel_gt::IRegisterBank& r_;
  const IClock& clock_;
  SnpsPhy phy_;
  uint32_t cdclk_ = 0;
  uint32_t uncalibrated_ = 0;
  const PortInfo* lit_ = nullptr;
  Output lit_output_ = Output::Hdmi;
  DpLinkTrainer::Outcome training_;
  const char* why_ = "";
};

}  // namespace intel_dg2
}  // namespace hardware
