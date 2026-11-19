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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04, kGmchControl = 0x50;
constexpr uint32_t kMemorySpace = 1u << 1;
constexpr uint64_t kRegisterBytes = 0x200000, kBarSize = 16u << 20;
}  // namespace

bool IntelGpu::initialize() {
  if (!UefiGopGpu::initialize()) return false;
  try_native();
  return true;
}

// Finds what the display engine needs (its registers, the global GTT, the pipe the firmware is using). Any failure leaves the firmware in charge.
void IntelGpu::try_native() {
  const PciDeviceInfo& d = context_.device;
  if (!context_.registers || !context_.pci || !context_.allocator || d.vendor_id == 0 || !intel_display::supported(info_.architecture, info_.discrete)) return;
  if (!(context_.pci->read32(d.address, kPciCommand) & kMemorySpace)) return;
  const uint64_t base = context_.pci->memory_bar(d.address, 0);
  intel_gt::GgttGeometry geometry;
  if (base == 0 || !intel_gt::ggtt_geometry(static_cast<uint16_t>(context_.pci->read32(d.address, kGmchControl)), kBarSize, &geometry)) return;
  MmioRegion registers, table;
  if (!context_.registers->map_registers(base, kRegisterBytes, &registers) || !context_.registers->map_registers(base + geometry.table_offset, geometry.table_bytes, &table)) return;
  bank_ = intel_gt::MmioRegisterBank(registers);
  ggtt_table_ = table;
  registers_mapped_ = true;
  scanout_ = UniquePointer<intel_display::Scanout>::make(*context_.allocator, bank_, table, info_.architecture, *context_.allocator);
  if (scanout_ && !scanout_->initialize()) scanout_ = UniquePointer<intel_display::Scanout>();
  if (!scanout_) return;
  // The built-in panel, for the display switch of a muxed laptop: a Display of its own (the scanout's is not to be written by anyone else).
  panel_display_ = UniquePointer<intel_display::Display>::make(*context_.allocator, bank_, info_.architecture);
  if (panel_display_ && panel_display_->probe())
    panel_ = UniquePointer<IntelPanelOutput>::make(*context_.allocator, bank_, *panel_display_, info_.architecture, nullptr);   // (the delay: set_panel_delay)
  if (panel_ && !panel_->probe()) panel_ = UniquePointer<IntelPanelOutput>();
  if (!panel_) panel_display_ = UniquePointer<intel_display::Display>();
}

bool IntelGpu::render_hardware(IntelRenderHardware* out) {
  if (!registers_mapped_ || info_.architecture != IntelGpuArchitecture::Gen9_5 || info_.discrete || !ggtt_table_.valid()) return false;
  out->registers = &bank_;
  out->ggtt_table = ggtt_table_;
  out->device_id = context_.device.device_id;
  out->revision = static_cast<uint8_t>(context_.pci ? context_.pci->read32(context_.device.address, 0x08) & 0xFF : 0);   // config offset 8: revision ID
  return true;
}

bool IntelGpu::set_mode(uint32_t index) {
  DisplayMode m;
  if (scanout_ && scanout_->ready() && mode_info(index, &m)) {
    ByteView memory;
    uint32_t pitch = 0;
    if (scanout_->set_mode(m.width, m.height, &memory, &pitch)) {
      native_memory_ = memory;
      native_pitch_ = pitch;
      native_mode_ = index;
      native_active_ = true;
      set_framebuffer(memory, m.width, m.height, pitch, /*bgr=*/true);
      return true;
    }
    // The display engine is showing our framebuffer already: the firmware's SetMode would change a buffer nobody scans out, so the screen stays as it is.
    if (native_active_) return false;
  }
  return UefiGopGpu::set_mode(index);
}

void IntelGpu::shutdown() {
  if (native_active_) {
    native_active_ = false;
    scanout_->shutdown();       // the firmware's plane is back, and our memory is gone
    apply_current_mode();       // so draw into the firmware's framebuffer again
  }
  panel_ = UniquePointer<IntelPanelOutput>();
  panel_display_ = UniquePointer<intel_display::Display>();
  scanout_ = UniquePointer<intel_display::Scanout>();
  registers_mapped_ = false;
}

void IntelGpu::flush_dirty() {
  if (!native_active_) return;
  uint32_t x, y, w, h;
  while (take_dirty(&x, &y, &w, &h)) intel_display::Scanout::flush(native_memory_, native_pitch_, x, y, w, h);
}

// --- The built-in panel (IntelPanelOutput) -----------------------------------------------------------------------------------------------

uint64_t IntelPanelOutput::dp_tp_ctl() const {
  // Xe-LP moved DP_TP_CTL to the transcoder (TGL_DP_TP_CTL); before it is the port's.
  if (architecture_ == IntelGpuArchitecture::XeLp && transcoder_ != kEdpTranscoder) return kTglDpTpCtlA + kTransStride * transcoder_;
  return kDpTpCtlA + kDdiStride * port_;
}

uint32_t IntelPanelOutput::backlight_on_us() const {
  const uint32_t t8 = (r_.read32(kPpOnDelays) & 0x1FFF) * 100u;   // power on to backlight on, in 100 us units
  return t8 ? t8 : kDefaultBacklightDelayUs;
}

uint32_t IntelPanelOutput::backlight_off_us() const {
  const uint32_t t9 = (r_.read32(kPpOffDelays) & 0x1FFF) * 100u;  // backlight off to power down
  return t9 ? t9 : kDefaultBacklightDelayUs;
}

void IntelPanelOutput::wait_us(uint32_t us) {
  if (us == 0) return;
  if (delay_) delay_(us);
  waited_us_ += us;
}

bool IntelPanelOutput::wait_for(uint64_t offset, uint32_t mask, uint32_t value, uint32_t timeout_us) {
  constexpr uint32_t kStepUs = 100;
  for (uint32_t waited = 0;; waited += kStepUs) {
    if ((r_.read32(offset) & mask) == value) return true;
    if (waited >= timeout_us) return false;
    wait_us(kStepUs);
  }
}

bool IntelPanelOutput::probe() {
  found_ = false;
  lit_ = false;
  transcoder_ = kNoTranscoder;
  if (!display_.active()) {
    status_ = "the display engine shows nothing the firmware lit";
    return false;
  }
  const uint32_t pipe = display_.pipe();
  // Gen9.5 and Gen11: the eDP transcoder, whose input select names the pipe it is fed from (0: pipe A always on, 4..7: pipe A..D on/off).
  if (architecture_ != IntelGpuArchitecture::XeLp) {
    const uint32_t f = r_.read32(kTransDdiFuncCtlEdp);
    const uint32_t input = (f & kEdpInputMask) >> 12;
    const uint32_t fed_by = input == 0 ? 0 : input >= 4 ? input - 4 : 0xFF;
    if ((f & kFuncEnable) && fed_by == pipe) {
      transcoder_ = kEdpTranscoder;
      port_ = (f >> 28) & 7;
    }
  } else {
    // Xe-LP: no eDP transcoder; the panel is on the pipe's own transcoder, in DisplayPort mode on DDI A (TGL port select: port + 1).
    const uint32_t f = r_.read32(kTransDdiFuncCtlA + kTransStride * pipe);
    if ((f & kFuncEnable) && (f & kFuncModeMask) == kFuncModeDpSst && ((f >> 27) & 0xF) == 1) {
      transcoder_ = pipe;
      port_ = 0;
    }
  }
  if (transcoder_ == kNoTranscoder) {
    status_ = "the firmware's pipe does not drive an eDP panel";
    return false;
  }
  ddi_buf_ = r_.read32(ddi_buf_ctl());
  dp_tp_ = r_.read32(dp_tp_ctl());
  func_ = r_.read32(func_ctl());
  conf_ = r_.read32(trans_conf());
  pp_control_ = r_.read32(kPpControl);
  blc_ = r_.read32(kBlcPwmPchCtl1);
  if (!(ddi_buf_ & kDdiBufEnable) || !(conf_ & kConfEnable) || !(r_.read32(kPpStatus) & kPpOn)) {
    status_ = "the eDP panel was not lit at start-up: its link settings are not known";
    transcoder_ = kNoTranscoder;
    return false;
  }
  found_ = true;
  lit_ = true;
  status_ = "lit by the firmware";
  return true;
}

void IntelPanelOutput::backlight_off() {
  r_.write32(kPpControl, r_.read32(kPpControl) & ~kBacklightEnable);
  r_.write32(kBlcPwmPchCtl1, r_.read32(kBlcPwmPchCtl1) & ~kBlcPwmEnable);
  wait_us(backlight_off_us());                    // T9: backlight off before the video stops
}

void IntelPanelOutput::stop_transcoder() {
  const uint32_t conf = r_.read32(trans_conf());
  if (conf & kConfEnable) {
    r_.write32(trans_conf(), conf & ~kConfEnable);
    (void)wait_for(trans_conf(), kConfState, 0, kPipeTimeoutUs);   // (i915 goes on when it does not stop: so does this)
  }
  r_.write32(func_ctl(), r_.read32(func_ctl()) & ~kFuncEnable);
}

void IntelPanelOutput::stop_port() {
  r_.write32(dp_tp_ctl(), r_.read32(dp_tp_ctl()) & ~kDpTpEnable);
  const uint32_t buf = r_.read32(ddi_buf_ctl());
  if (buf & kDdiBufEnable) {
    r_.write32(ddi_buf_ctl(), buf & ~kDdiBufEnable);
    (void)wait_for(ddi_buf_ctl(), kDdiBufIdle, kDdiBufIdle, kPortTimeoutUs);
  }
}

void IntelPanelOutput::panel_power_off() {
  r_.write32(kPpControl, r_.read32(kPpControl) & ~(kPowerTargetOn | kPowerReset | kForceVdd | kBacklightEnable));
  // The sequencer waits T10, switches the power off and starts the power cycle time (T11+T12) itself.
  (void)wait_for(kPpStatus, kPpOn | kPpSequenceMask, 0, kPowerTimeoutUs);
}

bool IntelPanelOutput::panel_power_on() {
  // A panel that was just switched off stays off for the power cycle time: the sequencer says when it is over.
  (void)wait_for(kPpStatus, kPpCycleDelayActive, 0, kCycleTimeoutUs);
  r_.write32(kPpControl, (pp_control_ & ~(kBacklightEnable | kForceVdd)) | kPowerTargetOn | kPowerReset);
  return wait_for(kPpStatus, kPpOn | kPpSequenceMask, kPpOn, kPowerTimeoutUs);
}

void IntelPanelOutput::dark() {
  backlight_off();
  stop_transcoder();
  stop_port();
  panel_power_off();
}

PanelLightStatus IntelPanelOutput::light_panel() {
  if (lit_) return PanelLightStatus::Ok;
  if (!found_) {
    status_ = "no eDP panel found";
    return PanelLightStatus::NoPanel;
  }
  if (!delay_) {
    status_ = "the panel's power sequence has no way to wait";
    return PanelLightStatus::NotSupported;
  }
  // 1. Panel power (T1+T3 are the sequencer's).
  if (!panel_power_on()) {
    panel_power_off();
    status_ = "the panel's power did not come on";
    return PanelLightStatus::PowerFailed;
  }
  // 2. The port, sending training pattern 1, as the firmware had it; the buffer must leave its idle state.
  r_.write32(dp_tp_ctl(), (dp_tp_ & ~kDpTpPatternMask) | kDpTpEnable | kDpTpPattern1);
  r_.write32(ddi_buf_ctl(), ddi_buf_ | kDdiBufEnable);
  wait_us(600);                                   // (i915: 600 us after enabling the buffer)
  if (!wait_for(ddi_buf_ctl(), kDdiBufIdle, 0, kPortTimeoutUs)) {
    stop_port();
    panel_power_off();
    status_ = "the eDP port did not start";
    return PanelLightStatus::LinkTraining;
  }
  // 3. eDP training without an AUX handshake: pattern 1, pattern 2, idle, then normal pixels.
  wait_us(kTrainingPatternUs);
  r_.write32(dp_tp_ctl(), (r_.read32(dp_tp_ctl()) & ~kDpTpPatternMask) | kDpTpPattern2);
  wait_us(kTrainingPatternUs);
  r_.write32(dp_tp_ctl(), (r_.read32(dp_tp_ctl()) & ~kDpTpPatternMask) | kDpTpIdle);
  wait_us(1000);                                  // (port A has no DP_TP_STATUS to wait on: i915 waits a fixed time)
  // 4. The transcoder's DDI function and the transcoder, at the timings the firmware left in it.
  r_.write32(func_ctl(), func_ | kFuncEnable);
  r_.write32(trans_conf(), conf_ | kConfEnable);
  if (!wait_for(trans_conf(), kConfState, kConfState, kPipeTimeoutUs)) {
    stop_transcoder();
    stop_port();
    panel_power_off();
    status_ = "the eDP transcoder did not start";
    return PanelLightStatus::ModeSet;
  }
  r_.write32(dp_tp_ctl(), (r_.read32(dp_tp_ctl()) & ~kDpTpPatternMask) | kDpTpNormal);
  // 5. The plane as it was when the panel was let go (the surface write arms it).
  if (plane_.valid) display_.restore(plane_);
  // 6. T8, then the backlight.
  wait_us(backlight_on_us());
  r_.write32(kBlcPwmPchCtl1, blc_ | kBlcPwmEnable);
  r_.write32(kPpControl, r_.read32(kPpControl) | kBacklightEnable);
  lit_ = true;
  status_ = "lit";
  return PanelLightStatus::Ok;
}

void IntelPanelOutput::release_panel() {
  if (!lit_) return;
  plane_ = display_.save();
  dark();
  lit_ = false;
  status_ = "released";
}

}  // namespace hardware
