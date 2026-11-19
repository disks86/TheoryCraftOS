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

// Moves a muxed laptop's built-in panel from one GPU to the other, as Linux's vga_switcheroo_switchto does (vga_switcheroo.c,
// vga_switchto_stage1/stage2):
//
//   1. Preconditions: a mux (IGpuMux: Apple gmux, AMD ATPX, NVIDIA's hybrid _DSM); a driver on each side that registered its panel output
//      (IPanelOutput, Multi/GpuMuxPanel.hpp); the target's driver able to light the panel; no other switch in flight (a second caller gets
//      Busy at once instead of waiting).
//   2. The target GPU woken and kept awake: a lease (IGpuLeases, the GpuManager's through GpuManagerLeases).
//   3. The DDC lines moved first where the mux can do that alone (gmux before retina, ATPX's I2C mux), so the target reads the panel's EDID.
//   4. The target driver lights the panel (power, EDID, mode, link, backlight).
//   5. The display mux moved.
//   6. The old GPU's driver lets the panel go, and the switcher's lease on the old GPU is given back (it may sleep again).
//
// Any failure in 3-5 is undone in reverse order: the mux moved back (if it was moved), the target driver releases what it lit, the DDC lines
// moved back, the lease given back. The report says what failed and whether undoing it worked (if the mux will not move back the panel may be
// dark: rollback_clean is false and the switcher no longer claims to know where the panel is).
//
// With the firmware's framebuffer as the integrated GPU's driver (GopPanelOutput), the switch to the discrete GPU works (the integrated GPU's
// picture simply stops reaching the panel) and the switch back is refused: nothing can light the panel on that side again.
//
// Moving the desktop onto the other GPU's framebuffer is the caller's job (a settings page): this only moves the panel. Thread-safe: every
// call may come from any core; a switch holds the lock for its whole length (the panel power sequence takes several hundred milliseconds).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuMuxPanel.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPlatformPower.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class GpuManager;

// Keeps a GPU awake while the panel is on it.
class IGpuLeases {
 public:
  [[nodiscard]] virtual bool acquire(uint32_t index) = 0;
  virtual void release(uint32_t index) = 0;

 protected:
  IGpuLeases() = default;
  ~IGpuLeases() = default;
};

// The GpuManager's render leases (wake on the first, sleep after the last).
class GpuManagerLeases final : public IGpuLeases {
 public:
  explicit GpuManagerLeases(GpuManager& manager) : manager_(manager) {}
  [[nodiscard]] bool acquire(uint32_t index) override;
  void release(uint32_t index) override;

 private:
  GpuManager& manager_;
};

enum class GpuMuxSwitchStatus : uint8_t {
  Switched,
  AlreadyActive,    // the panel is on that GPU and lit
  Busy,             // another switch is in flight
  NoOutput,         // no driver registered its panel output for that side (or for the side the panel is on now)
  NoPanelOutput,    // the target's driver has no built-in panel output
  CannotLight,      // the target's driver cannot light the panel (the reason says why)
  WakeFailed,       // the target GPU would not wake
  LightFailed,      // the target's driver tried and failed (the report's `light` says how); undone
  MuxFailed,        // the mux would not move; undone
  StateUnknown,     // an earlier rollback failed: the switcher does not know where the panel is (set_known_active() after checking)
};
const char* gpu_mux_switch_status_name(GpuMuxSwitchStatus status);
const char* gpu_mux_target_name(GpuMuxTarget target);

struct GpuMuxSwitchReport {
  GpuMuxSwitchStatus status = GpuMuxSwitchStatus::NoOutput;
  const char* reason = "";
  GpuMuxTarget from = GpuMuxTarget::Integrated;
  GpuMuxTarget to = GpuMuxTarget::Integrated;
  bool woke = false;                 // a lease was taken on the target GPU
  bool ddc_switched = false;         // the DDC lines were moved before the panel was lit
  PanelLightStatus light = PanelLightStatus::Ok;
  bool mux_switched = false;         // the display mux moved (and stayed, on success)
  bool rolled_back = false;          // a failure was undone
  bool rollback_clean = true;        // ... and every step of undoing it worked
  bool ok() const { return status == GpuMuxSwitchStatus::Switched || status == GpuMuxSwitchStatus::AlreadyActive; }
};

class GpuMuxSwitch {
 public:
  // `leases` may be null (nothing to wake: tests, or a machine whose GPUs never sleep); a GPU index of -1 is not leased either. `boot` is the
  // side the panel was on when the OS started (where the firmware showed its picture); the mux's own answer wins when it can give one.
  GpuMuxSwitch(IGpuMux& mux, IGpuLeases* leases, int32_t integrated_index, int32_t discrete_index, GpuMuxTarget boot = GpuMuxTarget::Integrated)
      : mux_(mux), leases_(leases), active_(static_cast<uint8_t>(boot)) {
    index_[0] = integrated_index;
    index_[1] = discrete_index;
  }
  ~GpuMuxSwitch();
  GpuMuxSwitch(const GpuMuxSwitch&) = delete;
  GpuMuxSwitch& operator=(const GpuMuxSwitch&) = delete;

  // The driver of one side (not owned; must outlive the switcher or be detached with nullptr). Waits for a switch in flight to end.
  void attach_output(GpuMuxTarget side, IPanelOutput* output);

  // Moves the panel to `target` (see above). Never blocks on another switch: that one's caller gets Busy.
  [[nodiscard]] GpuMuxSwitchReport switch_to(GpuMuxTarget target);
  // Whether switch_to(target) would be tried now (a settings page disables the choice otherwise); `why` (may be null) gets the reason.
  // False while a switch is in flight. Calls only the outputs' const queries.
  [[nodiscard]] bool can_switch_to(GpuMuxTarget target, const char** why) const;

  // Where the switcher believes the panel is (the last switch, or the boot side). False while that is unknown after a failed rollback (a mux
  // that can say where the panel is settles it at the next switch_to()).
  [[nodiscard]] bool active(GpuMuxTarget* out) const;
  // After a failed rollback, once someone has checked where the panel is.
  void set_known_active(GpuMuxTarget target);
  bool in_flight() const { return __atomic_load_n(&in_flight_, __ATOMIC_ACQUIRE); }
  uint32_t switches() const { return __atomic_load_n(&switches_, __ATOMIC_RELAXED); }
  // Whether the switcher holds a lease on that side's GPU (the side the panel moved to, after a switch).
  bool holds_lease(GpuMuxTarget side) const;

  // The machine's switcher, for a display settings page: set once at boot (main.cpp), null when the machine has no mux. Cleared when that
  // switcher is destroyed.
  static void publish(GpuMuxSwitch* system_switch);
  static GpuMuxSwitch* published();

 private:
  static uint32_t slot(GpuMuxTarget side) { return side == GpuMuxTarget::Discrete ? 1 : 0; }
  static GpuMuxTarget other(GpuMuxTarget side) { return side == GpuMuxTarget::Discrete ? GpuMuxTarget::Integrated : GpuMuxTarget::Discrete; }
  // Checks of switch_to / can_switch_to, made with `lock_` held. Sets `report` and returns false when the switch cannot be tried.
  bool preconditions(GpuMuxTarget target, GpuMuxTarget from, GpuMuxSwitchReport& report) const;
  GpuMuxSwitchReport run(GpuMuxTarget target);
  void give_back(GpuMuxTarget side);

  mutable kernel::Mutex lock_;       // held for a whole switch, and for attach_output()
  IGpuMux& mux_;
  IGpuLeases* leases_;
  int32_t index_[2] = {-1, -1};
  IPanelOutput* output_[2] = {};
  bool held_[2] = {};
  // (read without the lock by active(); written with it held)
  uint8_t active_;                   // a GpuMuxTarget
  bool known_ = true;
  bool in_flight_ = false;
  uint32_t switches_ = 0;
};

}  // namespace hardware
