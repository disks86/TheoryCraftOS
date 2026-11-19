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

#pragma once
#include "Hardware/GraphicsProcessingUnit/Intel/IntelDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelScanout.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuMuxPanel.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// What the render engine driver needs of an Intel GPU, found while the display engine was set up: its register window and the global GTT table.
struct IntelRenderHardware {
  intel_gt::IRegisterBank* registers = nullptr;
  MmioRegion ggtt_table;
  uint16_t device_id = 0;
  uint8_t revision = 0;
};

// The built-in eDP panel of an Intel integrated GPU, for the display mux switch of a muxed laptop (Multi/GpuMuxSwitch, IPanelOutput): the panel
// is let go (the panel moves to the discrete GPU) and lit again (it comes back), after i915's order (intel_ddi.c, intel_pps.c, intel_backlight.c):
//
//   release: backlight off (PP_CONTROL's EDP_BLC_ENABLE, then the PCH PWM, BLC_PWM_PCH_CTL1) and T9 waited; the pipe's plane and source size saved
//            (IntelDisplay's Display::save) and the transcoder stopped (TRANSCONF enable, then its state bit); the transcoder's DDI function off;
//            the port off (DP_TP_CTL, DDI_BUF_CTL, then its idle bit); the panel power off through the panel power sequencer (PP_CONTROL's power
//            target and reset bits cleared, PP_STATUS waited to go idle-off: the sequencer makes T10 and starts T11+T12 itself).
//   light:   the power cycle time waited out (PP_STATUS's cycle delay bit), the panel power on (PP_STATUS idle-on, T1+T3 by the sequencer); the port
//            on with DP_TP_CTL sending training pattern 1, DDI_BUF_CTL as the firmware had it and its idle bit waited to clear; eDP's training
//            without AUX ("no AUX handshake": pattern 1, then 2, at least 500 us each, then idle and normal pixels); the transcoder's DDI function
//            and the transcoder on as the firmware had them (the panel's own timings stay in the transcoder registers, untouched); the plane put
//            back (Display::restore); T8 waited and the backlight on.
//
// The PLL and the DDI clock routing are never touched (they keep running while the panel is away), so the panel is lit again at exactly the mode
// the firmware lit it at boot: probe() reads the port, transcoder and backlight registers as the firmware left them, and has_panel_output() is false
// when there was no eDP panel lit then (the OS started with the panel on the discrete GPU: its link settings are not known). Gen9.5 and Gen11 drive
// eDP through the eDP transcoder (TRANS_DDI_FUNC_CTL_EDP, its input the firmware's pipe); Xe-LP through the pipe's own transcoder on DDI A.
// Waits are T8/T9 from PP_ON_DELAYS/PP_OFF_DELAYS (100 us units), 50 ms each when the firmware left none (the eDP specification's upper limits).
//
// Not verified on hardware (the host tests drive it against a register model). A panel that needs AUX link training (one that does not support
// eDP's training without a handshake) stays dark: light_panel() then cannot tell (the port reports no failure); IntelDisplay has no AUX channel.
// Thread-safety: none of its own; the mux switch serializes every call.
class IntelPanelOutput final : public IPanelOutput {
 public:
  using DelayFn = void (*)(uint32_t us);
  // The panel power sequencer (the PCH's first one), backlight and the port and transcoder registers, as i915 names them.
  static constexpr uint64_t kPpStatus = 0xC7200, kPpControl = 0xC7204, kPpOnDelays = 0xC7208, kPpOffDelays = 0xC720C;
  static constexpr uint32_t kPpOn = 1u << 31, kPpSequenceMask = 3u << 28, kPpCycleDelayActive = 1u << 27;
  static constexpr uint32_t kPowerTargetOn = 1u << 0, kPowerReset = 1u << 1, kBacklightEnable = 1u << 2, kForceVdd = 1u << 3;
  static constexpr uint64_t kBlcPwmPchCtl1 = 0xC8250;
  static constexpr uint32_t kBlcPwmEnable = 1u << 31;
  static constexpr uint64_t kDdiBufCtlA = 0x64000, kDpTpCtlA = 0x64040, kDdiStride = 0x100, kTglDpTpCtlA = 0x60540;
  static constexpr uint32_t kDdiBufEnable = 1u << 31, kDdiBufIdle = 1u << 7;
  static constexpr uint32_t kDpTpEnable = 1u << 31, kDpTpPatternMask = 7u << 8, kDpTpPattern1 = 0u << 8, kDpTpPattern2 = 1u << 8,
                            kDpTpIdle = 2u << 8, kDpTpNormal = 3u << 8;
  static constexpr uint64_t kTransDdiFuncCtlA = 0x60400, kTransDdiFuncCtlEdp = 0x6F400, kTransConfA = 0x70008, kTransConfEdp = 0x7F008, kTransStride = 0x1000;
  static constexpr uint32_t kFuncEnable = 1u << 31, kFuncModeMask = 7u << 24, kFuncModeDpSst = 2u << 24, kEdpInputMask = 7u << 12;
  static constexpr uint32_t kConfEnable = 1u << 31, kConfState = 1u << 30;
  static constexpr uint32_t kDefaultBacklightDelayUs = 50000, kPowerTimeoutUs = 5000000, kCycleTimeoutUs = 1500000, kPipeTimeoutUs = 100000,
                            kPortTimeoutUs = 2000, kTrainingPatternUs = 500;
  static constexpr uint32_t kNoTranscoder = 0xFF, kEdpTranscoder = 0xFE;

  // `display` is a Display of the same registers that probe()d the firmware's pipe; both must outlive this object. `delay` waits (it may sleep);
  // without one (null) the panel is not lit again: its power sequence needs real waits.
  IntelPanelOutput(intel_gt::IRegisterBank& registers, intel_display::Display& display, IntelGpuArchitecture architecture, DelayFn delay)
      : r_(registers), display_(display), architecture_(architecture), delay_(delay) {}

  // Finds the eDP panel the firmware lit: the eDP transcoder fed by the display's pipe (Gen9.5, Gen11) or the pipe's transcoder in DisplayPort
  // mode on DDI A (Xe-LP), its port on, the panel power on. Keeps what it needs to light it again. False if there is none.
  [[nodiscard]] bool probe();

  bool has_panel_output() const override { return found_; }
  bool panel_lit() const override { return lit_; }
  bool can_light_panel() const override { return found_ && delay_; }
  void set_delay(DelayFn delay) { delay_ = delay; }
  [[nodiscard]] PanelLightStatus light_panel() override;
  void release_panel() override;
  const char* panel_status() const override { return status_; }

  // What probe() found: kEdpTranscoder or the transcoder's number, and the DDI port (0 is DDI A).
  uint32_t transcoder() const { return transcoder_; }
  uint32_t port() const { return port_; }
  // Waited in all, in microseconds (for the tests and the log).
  uint64_t waited_us() const { return waited_us_; }

 private:
  uint64_t func_ctl() const { return transcoder_ == kEdpTranscoder ? kTransDdiFuncCtlEdp : kTransDdiFuncCtlA + kTransStride * transcoder_; }
  uint64_t trans_conf() const { return transcoder_ == kEdpTranscoder ? kTransConfEdp : kTransConfA + kTransStride * transcoder_; }
  uint64_t ddi_buf_ctl() const { return kDdiBufCtlA + kDdiStride * port_; }
  uint64_t dp_tp_ctl() const;
  uint32_t backlight_on_us() const;
  uint32_t backlight_off_us() const;
  void wait_us(uint32_t us);
  // Waits until (register & mask) == value, at most `timeout_us`.
  bool wait_for(uint64_t offset, uint32_t mask, uint32_t value, uint32_t timeout_us);
  bool panel_power_on();
  void panel_power_off();
  void backlight_off();
  void stop_transcoder();
  void stop_port();
  // Everything that light_panel() may have switched on, off again in release order.
  void dark();

  intel_gt::IRegisterBank& r_;
  intel_display::Display& display_;
  IntelGpuArchitecture architecture_;
  DelayFn delay_;
  bool found_ = false, lit_ = false;
  uint32_t transcoder_ = kNoTranscoder, port_ = 0;
  // As the firmware left them (probe()).
  uint32_t ddi_buf_ = 0, dp_tp_ = 0, func_ = 0, conf_ = 0, pp_control_ = 0, blc_ = 0;
  intel_display::Snapshot plane_;                // the plane and the pipe's source size, saved when the panel was let go
  uint64_t waited_us_ = 0;
  const char* status_ = "no eDP panel found";
};

// Intel display controller (PCI vendor 0x8086), identified by device ID down to its architecture (UHD 6xx, Iris Xe, Arc,
// Xe2, Xe3; see IntelGpuGenerations.hpp).
//
// The firmware (UEFI GOP) still starts it: it lists the modes, gives the first framebuffer and the EDID. On the integrated GPUs of Gen9.5 to Xe-LP the display
// engine is then taken over natively (IntelDisplay, IntelScanout): the first mode change moves the screen to a framebuffer of ours, and every mode after
// that is set through the display registers, not through the firmware. Anything native mode setting cannot do (another GPU, a state it does not recognise, the
// mapping failing, the engine not taking a mode before it ever took one) leaves the firmware path in charge, as before.
class IntelGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  explicit IntelGpu(const GpuContext& context)
      : UefiGopGpu(context), info_(intel_gpu_identify(context.device.device_id)), context_(context) {}

  const char* name() const override { return info_.name; }
  const IntelGpuInfo& generation() const { return info_; }

  bool initialize() override;
  void shutdown() override;
  uint32_t current_mode() const override { return native_active_ ? native_mode_ : UefiGopGpu::current_mode(); }
  bool set_mode(uint32_t index) override;
  // True once the screen is driven by the display engine (not the firmware).
  bool native_mode_setting() const { return native_active_; }
  // True if the display engine was found and could be driven (it is used from the first mode change).
  bool native_available() const { return scanout_ && scanout_->ready(); }
  // The registers and the global GTT of a Gen9.5 GPU whose register BAR was mapped, for the render engine driver (Graphics/Graphics3D/IntelGen9); false for any other
  // generation or when the mapping failed. The pointers stay valid until shutdown().
  bool render_hardware(IntelRenderHardware* out);
  // The built-in panel for the display mux switch (IntelPanelOutput above): there when native mode setting is available and the firmware lit an eDP
  // panel at boot; null otherwise (the caller then falls back to GopPanelOutput, which cannot light the panel again).
  IPanelOutput* panel_output() { return panel_.get(); }
  // How the panel's power sequence waits (kernel::sleep_us; set by whoever attaches the panel to the switch, before it is used).
  void set_panel_delay(IntelPanelOutput::DelayFn delay) { if (panel_) panel_->set_delay(delay); }

  // What is drawn has to be flushed out of the CPU's caches for the display engine to see it.
  void put_pixel(uint32_t x, uint32_t y, uint32_t color) override { UefiGopGpu::put_pixel(x, y, color); flush_dirty(); }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) override { UefiGopGpu::fill_rect(x, y, w, h, color); flush_dirty(); }
  void copy_rect(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) override { UefiGopGpu::copy_rect(sx, sy, dx, dy, w, h); flush_dirty(); }
  void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels, uint32_t pitch_pixels) override {
    UefiGopGpu::blit(x, y, w, h, pixels, pitch_pixels);
    flush_dirty();
  }
  void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) override { UefiGopGpu::draw_char(x, y, c, fg, bg); flush_dirty(); }
  void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg, uint32_t bg) override { UefiGopGpu::draw_text(x, y, text, fg, bg); flush_dirty(); }

 private:
  void try_native();
  void flush_dirty();

  IntelGpuInfo info_;
  GpuContext context_;
  intel_gt::MmioRegisterBank bank_{MmioRegion()};
  MmioRegion ggtt_table_;
  bool registers_mapped_ = false;
  UniquePointer<intel_display::Scanout> scanout_;
  ByteView native_memory_;
  uint32_t native_pitch_ = 0;
  bool native_active_ = false;
  uint32_t native_mode_ = 0;
  UniquePointer<intel_display::Display> panel_display_;   // (before panel_, which refers to it)
  UniquePointer<IntelPanelOutput> panel_;
};

}  // namespace hardware
