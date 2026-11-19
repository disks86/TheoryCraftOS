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

// What a display driver offers the display mux switch (Multi/GpuMuxSwitch) for a laptop's built-in panel: light it (power it up, read its EDID,
// set a mode, train the link, turn the backlight on) and let it go again (backlight off, the output stopped, the panel's power off). Linux's
// vga_switcheroo asks the same of a driver through its client ops (reprobe, set_gpu_state) and the DRM core's modeset.
//
// GopPanelOutput is the integrated GPU's side when its driver is only the firmware's framebuffer (UEFI GOP, and the Intel/AMD boot display
// drivers, which run on it): the firmware lit the panel at boot, and nothing here can light it again once it is let go. Letting it go writes
// nothing (the integrated GPU's pipe keeps running; its picture just stops reaching the panel when the mux moves), but from then on
// can_light_panel() is false, so the switch back to it is refused instead of leaving the panel dark. An Intel integrated GPU with native mode
// setting has a real one instead (IntelPanelOutput, Intel/IntelGpu.hpp), which can light the panel again.
//
// IBuiltInScreenSwitch is what a settings page sees of all this: which GPU drives the built-in screen, whether the other one can, and the switch,
// which moves the desktop's monitor of the panel along (GpuMuxDesktop, Multi/GpuMuxSwitch.hpp).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp"

namespace hardware {

enum class PanelLightStatus : uint8_t {
  Ok,
  NoPanel,          // the driver has no built-in panel output (the board's tables list none)
  NotSupported,     // the driver cannot light the panel (a firmware framebuffer that let it go, a display it does not set modes on)
  Busy,             // the driver's display already drives another output (several heads at once are not done)
  NoEdid,           // the panel did not answer with an EDID once powered
  NoMode,           // none of the panel's modes fits the output
  PowerFailed,      // the panel's power could not be switched
  LinkTraining,     // eDP: the link did not train
  ModeSet,          // the mode set itself failed (the supervisor, the surface, the memory)
};
const char* panel_light_status_name(PanelLightStatus status);

class IPanelOutput {
 public:
  // The board has a built-in panel output this driver could drive (a muxed laptop's discrete GPU lists the eDP/LVDS output in its tables).
  virtual bool has_panel_output() const = 0;
  // The panel is lit by this driver now.
  virtual bool panel_lit() const = 0;
  // light_panel() can work at all now (not that it will: the panel may still not answer). A firmware framebuffer that let the panel go says
  // false, so does a driver busy with another output.
  virtual bool can_light_panel() const = 0;
  // Powers the panel up, reads its EDID (the mux's DDC lines, where it has them, already point here), sets its mode and turns the backlight on.
  // On a failure nothing is left lit or powered by this call.
  [[nodiscard]] virtual PanelLightStatus light_panel() = 0;
  // Backlight off, the output stopped, the panel's power off (with the panel's power sequence delays). Nothing happens if it is not lit.
  virtual void release_panel() = 0;
  // A word on the last light_panel() or the state (for the log and a settings page).
  virtual const char* panel_status() const = 0;

 protected:
  IPanelOutput() = default;
  ~IPanelOutput() = default;
};

// The integrated GPU's panel as the firmware's framebuffer has it (see above). Thread-safety: the mux switch serializes every call.
class GopPanelOutput final : public IPanelOutput {
 public:
  // `lit_at_boot`: the firmware showed its framebuffer on the built-in panel (the mux pointed at the integrated GPU when the OS started).
  explicit GopPanelOutput(bool lit_at_boot = true) : lit_(lit_at_boot) {}
  bool has_panel_output() const override { return true; }
  bool panel_lit() const override { return lit_; }
  // Only while it still has the panel: "lighting" it then changes nothing.
  bool can_light_panel() const override { return lit_; }
  [[nodiscard]] PanelLightStatus light_panel() override { return lit_ ? PanelLightStatus::Ok : PanelLightStatus::NotSupported; }
  void release_panel() override { lit_ = false; }
  const char* panel_status() const override {
    return lit_ ? "lit by the firmware" : "the firmware's picture cannot be brought back to the built-in screen until the computer restarts";
  }

 private:
  bool lit_;
};

// The display switch as a settings page uses it (see the file comment). Thread-safety: the implementation's; GpuMuxDesktop's calls may come from any
// thread, but the monitors it moves are the desktop's, so switch_to() is called where the desktop changes its monitors (its settings page).
class IBuiltInScreenSwitch {
 public:
  struct Result {
    bool ok = false;
    bool rolled_back = true;   // after a failure: the screen is back where it was (false: it may be dark, and where it is is not known)
    const char* reason = "";   // why it failed, a phrase in the user's words ("that GPU would not wake")
  };
  // The GPU that drives the built-in screen now; false when that is not known (an earlier switch could not be undone).
  [[nodiscard]] virtual bool active(GpuMuxTarget* out) const = 0;
  // The name of one side's GPU (its driver's), for the page.
  virtual const char* gpu_name(GpuMuxTarget side) const = 0;
  // Whether switch_to(side) would be tried now; `why` (may be null) gets the reason when not.
  [[nodiscard]] virtual bool can_switch_to(GpuMuxTarget side, const char** why) const = 0;
  // Moves the built-in panel to that side's GPU, and the desktop's monitor of it along. The caller then makes the monitors real (applies them and
  // rebuilds what draws the desktop) while nothing draws, and switches back if that fails.
  [[nodiscard]] virtual Result switch_to(GpuMuxTarget side) = 0;

 protected:
  IBuiltInScreenSwitch() = default;
  ~IBuiltInScreenSwitch() = default;
};

}  // namespace hardware
