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

// What the platform (not the GPU) can do for the discrete GPU of a hybrid laptop: cut its power completely (D3cold) and, on a muxed machine,
// move the built-in panel between the GPUs. PCI power management alone only reaches D3hot, where a discrete GPU still draws a watt or more;
// the rest needs the firmware (ACPI power resources, NVIDIA's Optimus and JT _DSM methods, AMD's ATPX method) or a mux chip (Apple's gmux).
// Linux calls the same split vga_switcheroo: a handler per platform mechanism, the manager decides when.
#pragma once
#include <stdint.h>

namespace hardware {

enum class GpuPowerMechanism : uint8_t {
  None,
  AcpiPowerResources,  // the device's _PR3 power resources (_OFF/_ON), with _PS3/_PS0 around them (ACPI 6 7.3.11, PCI firmware spec D3cold)
  NvidiaOptimus,       // NVIDIA's Optimus _DSM (NVOP, function 0x1A: power off on D3), then the power resources
  NvidiaJt,            // NVIDIA's JT _DSM (GPS/JT, newer laptops), then the power resources
  NvidiaHybridDsm,     // NVIDIA's older hybrid _DSM (9D95A0A0-...: function 3 power, function 2 mux), from before Optimus
  AmdAtpx,             // AMD's ATPX method on the integrated GPU (function 2: power control)
  AppleGmux,           // Apple's gmux chip (MacBook Pro): switching and discrete power
};

const char* gpu_power_mechanism_name(GpuPowerMechanism mechanism);

// Turns the power of one discrete GPU off and on. The manager calls power_off() after it has put the GPU (and its other functions) in D3hot,
// and power_on() before it brings them back to D0; the GPU has lost all of its state when it comes back (it needs a driver that can set it up
// again, which is why the manager only does this to GPUs whose driver says so).
class IGpuPlatformPower {
 public:
  virtual GpuPowerMechanism mechanism() const = 0;
  // False if the firmware refused or the method faulted; the GPU is then left in D3hot (power_off) or not usable (power_on).
  [[nodiscard]] virtual bool power_off() = 0;
  [[nodiscard]] virtual bool power_on() = 0;
  // How long (milliseconds) the GPU needs after power_on() before its config space answers: the PCI firmware spec's D3cold-to-D0 delay
  // (_DSM function 9 of the root port can shorten it), 100 ms by default.
  virtual uint32_t power_on_delay_ms() const { return 100; }

 protected:
  IGpuPlatformPower() = default;
  ~IGpuPlatformPower() = default;
};

enum class GpuMuxTarget : uint8_t { Integrated, Discrete };

// A display multiplexer: a switch that connects the built-in panel (and on some machines its DDC lines, for the EDID) to one GPU or the other.
// A machine without one (most Optimus laptops) shows everything through the integrated GPU's panel connection, and the discrete GPU renders
// into memory the integrated one scans out.
class IGpuMux {
 public:
  // The GPU the panel is connected to now; false if the mux cannot tell.
  [[nodiscard]] virtual bool active(GpuMuxTarget* out) = 0;
  // Moves the panel. The GPU it moves to must be awake and have a driver that can light it up; the caller handles both.
  [[nodiscard]] virtual bool switch_to(GpuMuxTarget target) = 0;
  // Moves only the DDC lines (to read the panel's EDID through the other GPU before switching); false where the mux cannot.
  [[nodiscard]] virtual bool switch_ddc(GpuMuxTarget target) { (void)target; return false; }

 protected:
  IGpuMux() = default;
  ~IGpuMux() = default;
};

}  // namespace hardware
