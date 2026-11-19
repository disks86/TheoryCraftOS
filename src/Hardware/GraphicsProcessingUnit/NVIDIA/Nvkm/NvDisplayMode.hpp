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

// Full mode setting on one head and one SOR (milestone B), for the GF119..Pascal display (907d): the timings as nouveau's
// nv50_head_atomic_check_mode turns a DRM mode into raster values, the SOR picked from the DCB output (OR mask, heads, link), the core
// channel methods of head907d.c/sor907d.c, the supervisor the UPDATE raises (polled: the VPLL through INvPixelClock in 2.1, the RG divider,
// the SOR clock and the DP transfer unit in 2.2), DisplayPort link training with nouveau's fallback from the fastest link configuration
// down (nvkm/engine/disp/dp.c) over the shared DpLinkTrainer, and the panel power of LVDS/eDP through a GPIO callback.
//
// Not done (and refused with NotImplemented): NV50 and Volta/Turing mode setting (their method numbers for the raster are in NvDisplayHw.hpp,
// but their supervisors and Volta's window usage bounds are not written), the VBIOS IED scripts nouveau runs in supervisor 2.x (some boards
// need them for TMDS/DP: the VBIOS parser does not expose them yet), HDMI infoframes/audio, MST, DSC.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"

namespace hardware {
namespace nv {

// A mode as DRM states it (pixels and lines from the start of the active area).
struct DisplayTiming {
  uint32_t clock_khz = 0;
  uint32_t hdisplay = 0, hsync_start = 0, hsync_end = 0, htotal = 0;
  uint32_t vdisplay = 0, vsync_start = 0, vsync_end = 0, vtotal = 0;
  bool hsync_negative = false, vsync_negative = false, interlaced = false;
};

// The same mode as the hardware counts it (from one unit into the sync pulse), nouveau's nv50_head_mode.
struct HeadRaster {
  uint32_t h_total = 0, h_sync_end = 0, h_blank_end = 0, h_blank_start = 0;
  uint32_t v_total = 0, v_sync_end = 0, v_blank_end = 0, v_blank_start = 0, v_blank2_start = 0, v_blank2_end = 0;
  uint32_t blank_us = 0;
  uint32_t clock_khz = 0;
  uint32_t hdisplay = 0, vdisplay = 0;
  bool interlaced = false;
  uint32_t raster_size() const { return v_total << 16 | h_total; }
  uint32_t sync_end() const { return v_sync_end << 16 | h_sync_end; }
  uint32_t blank_end() const { return v_blank_end << 16 | h_blank_end; }
  uint32_t blank_start() const { return v_blank_start << 16 | h_blank_start; }
  uint32_t vert_blank2() const { return v_blank2_end << 16 | v_blank2_start; }
};
// False for timings that are not a mode (sync outside the blanking, zero clock, larger than the raster registers hold).
[[nodiscard]] bool compute_head_raster(const DisplayTiming& timing, HeadRaster* out);

// Which SOR and head drive a DCB output, and how.
struct OutputRoute {
  uint32_t sor = 0;
  uint32_t head = 0;
  uint8_t link = 1;                 // 1 link A, 2 link B, 3 both (dual-link TMDS)
  uint32_t protocol = 0;            // disp_hw::kProtocol*
  bool dp = false;
  bool panel = false;               // LVDS (or eDP, by the caller's say): needs the panel powered
};
// `head_wanted`: the head to use if the output may (else the lowest it may use). False if no SOR or head fits, or the output is not a SOR one
// driven here (TMDS, DP, LVDS).
[[nodiscard]] bool route_output(const DcbOutput& output, uint32_t sor_mask, uint32_t head_mask, uint32_t head_wanted, uint32_t clock_khz, OutputRoute* out);

// The GF119 core channel methods of a full mode set (SOR control, the head's output resource and control, raster, pixel clock, viewport,
// and its surface). `depth_bpp` 18, 24 or 30.
[[nodiscard]] bool gf119_mode_methods(const NvDisplayEngine& engine, uint32_t head, const HeadRaster& raster, const OutputRoute& route, uint32_t depth_bpp,
                                      bool hsync_negative, bool vsync_negative, const ScanoutSurface& surface, MethodList* out);

// The display PLL of a head (nouveau: nvkm_devinit_pll_set(devinit, PLL_VPLL0 + head, khz)); the clock code implements it.
class INvPixelClock {
 public:
  [[nodiscard]] virtual bool set_vpll(uint32_t head, uint32_t khz) = 0;

 protected:
  INvPixelClock() = default;
  ~INvPixelClock() = default;
};

// A GPIO by DCB function (nouveau: nvkm_gpio_set(gpio, 0, function, 0xff, on)); the GPIO code implements it.
class INvGpio {
 public:
  [[nodiscard]] virtual bool set_function(uint8_t function, bool on) = 0;

 protected:
  INvGpio() = default;
  ~INvGpio() = default;
};

// The transmitter's drive for a voltage swing / pre-emphasis level pair, as the VBIOS DP table gives it (gf119_sor_dp_drive's dc, pe, pu, pc).
struct DpDriveLevel {
  uint8_t drive_current = 0, pre_emphasis = 0, tx_pu = 0, post_cursor = 0;
};
struct DpDriveTable {
  DpDriveLevel level[4][4];         // [voltage swing][pre-emphasis]
};
// Placeholder values in the shape of a VBIOS table, NOT taken from any VBIOS: the VBIOS parser should supply the board's own (BIT 'd' table).
const DpDriveTable& placeholder_dp_drive_table();

// The DisplayPort side of a GF119..Pascal SOR (gf119_sor_dp_links / _pattern / _drive, g94_sor_dp_power), as DpLinkTrainer's source.
class NvSorDp final : public IDpSource {
 public:
  NvSorDp(INvRegisters& registers, uint32_t sor, uint8_t link, const DpDriveTable& drive, const DisplayTimeouts& timeouts)
      : r_(registers), sor_(sor), link_(link), drive_(drive), timeouts_(timeouts) {}
  // The link rate (bandwidth code), lanes and framing on the SOR, and the lanes powered.
  [[nodiscard]] bool set_link(uint8_t bandwidth_code, uint32_t lanes, bool enhanced_framing);
  bool start_training() override { return true; }
  bool set_pattern(uint8_t pattern) override;
  bool complete_training() override { return set_pattern(0); }
  bool set_drive(uint8_t lane_set) override;
  void delay_us(uint32_t microseconds) override { r_.delay_us(microseconds); }
  uint32_t lanes() const { return lanes_; }
  uint8_t bandwidth_code() const { return bw_; }
  bool enhanced_framing() const { return enhanced_; }

 private:
  uint32_t sor_base() const { return sor_ * 0x800; }
  uint32_t link_base() const { return sor_base() + (link_ == 2 ? 0x80 : 0); }

  INvRegisters& r_;
  uint32_t sor_;
  uint8_t link_;
  const DpDriveTable& drive_;
  DisplayTimeouts timeouts_;
  uint32_t lanes_ = 0;
  uint8_t bw_ = 0;
  bool enhanced_ = false;
};

struct DpTrainResult {
  static constexpr uint32_t kMaxAttempts = 16;
  bool trained = false;
  uint8_t bandwidth_code = 0;
  uint32_t lanes = 0;
  uint32_t attempts = 0;
  uint8_t tried_bandwidth[kMaxAttempts] = {};
  uint8_t tried_lanes[kMaxAttempts] = {};
  DpLinkTrainer::Result last = DpLinkTrainer::Result::SinkPower;
};
// nouveau's nvkm_dp_train loop: the link rates from the fastest the sink and the DCB entry allow down, and for each the lanes from the most
// down to one, skipping configurations that cannot carry `data_kBps` (the pixel clock times bytes per pixel); the first that trains is kept.
[[nodiscard]] DpTrainResult train_dp_link(IDpcdAux& aux, NvSorDp& sor, const Dpcd::Capabilities& caps, const DcbOutput& output, uint64_t data_kBps);

// The DP transfer unit and the symbols left for audio in the blanking (nv50_disp_super_2_2_dp).
struct DpTransferUnit {
  bool valid = false;
  uint32_t tu = 0, vtu_a = 0, vtu_f = 0, vtu_i = 0;
  uint32_t watermark = 0;
  uint32_t hblank_symbols = 0, vblank_symbols = 0;
};
DpTransferUnit compute_dp_transfer_unit(const HeadRaster& raster, uint8_t bandwidth_code, uint32_t lanes, bool enhanced_framing, uint32_t depth_bpp);

class NvModeSetter final : public IDisplayPoller {
 public:
  struct Request {
    DcbOutput output;
    uint32_t head = 0;
    DisplayTiming timing;
    ScanoutSurface surface;         // what the head shows (offset relative to VRAM)
    uint32_t depth_bpp = 24;
    bool edp = false;               // a DP output that is an internal panel (power it through the GPIO first)
  };

  // `clock`, `gpio`, `aux` may be null when the output does not need them (aux for DP, gpio for panels; without `clock` the VPLL is left).
  NvModeSetter(NvDisplayEngine& engine, INvPixelClock* clock, INvGpio* gpio, IDpcdAux* aux, const DpDriveTable* drive = nullptr)
      : engine_(engine), clock_(clock), gpio_(gpio), aux_(aux), drive_(drive ? *drive : placeholder_dp_drive_table()) {}

  [[nodiscard]] DisplayStatus set_mode(const Request& request);
  // Blanks `head` and lets go of `sor` (nouveau's atomic disable: sor907d_ctrl with no owner, head907d_core_clr taking the surface away), an
  // UPDATE through the supervisor (whose stages are acknowledged without touching clocks). GF119..Pascal; the display must be claimed.
  [[nodiscard]] DisplayStatus disable(uint32_t head, uint32_t sor);
  const DpTrainResult& dp_result() const { return dp_; }
  uint32_t supervisor_stages() const { return stages_; }   // the stages (bit 0 = 1, bit 1 = 2, bit 2 = 3) handled during the last set_mode
  const DpTransferUnit& transfer_unit() const { return tu_; }

  // The supervisor, polled while the UPDATE is waited for.
  void poll() override;

 private:
  void supervisor_2(uint32_t head_mask_bits[NvDisplayEngine::kMaxHeads]);

  NvDisplayEngine& engine_;
  INvPixelClock* clock_;
  INvGpio* gpio_;
  IDpcdAux* aux_;
  const DpDriveTable& drive_;
  bool active_ = false;
  bool disabling_ = false;          // disable(): the supervisor's stages are only acknowledged
  OutputRoute route_;
  HeadRaster raster_;
  uint32_t depth_ = 24;
  DpTrainResult dp_;
  DpTransferUnit tu_;
  uint32_t stages_ = 0;
  bool vpll_failed_ = false;
};

}  // namespace nv
}  // namespace hardware
