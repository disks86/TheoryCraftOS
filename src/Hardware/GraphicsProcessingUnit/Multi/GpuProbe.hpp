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

// A GPU this OS cannot show or render on yet but can identify and watch: a hybrid laptop's discrete GPU, which has no display driver here (the
// vendor drivers run the boot display through the firmware's framebuffer). A probe reads the chip, its memory size and its sensors while the GPU
// is awake, and copes with the GPU losing its power between uses: it keeps no state in the device that it cannot set up again, which is what lets
// the GpuManager take the GPU to D3cold.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class IGpuProbe {
 public:
  // The chip ("NVIDIA GA107 (Ampere)"); the adapter's table name until start() has read it.
  virtual const char* name() const = 0;
  // What went wrong, or "identified".
  virtual const char* status() const = 0;
  // First contact. The GPU is awake (D0). False if it does not answer or is not a chip the probe knows; the probe is then dropped.
  [[nodiscard]] virtual bool start() = 0;
  // After a wake that may have followed a power cut: brings the probe's view back (the sensors are set up again). The GPU is awake.
  [[nodiscard]] virtual bool resume() = 0;
  // The video memory on the card, 0 if unknown.
  virtual uint64_t vram_bytes() const = 0;
  // The chip's temperature in thousandths of a degree, -1 if unknown. The GPU must be awake.
  virtual int32_t temperature_millicelsius() = 0;
  // Monitors plugged into the GPU's own outputs (a laptop's HDMI or USB-C port is often wired to the discrete GPU), as far as it can tell.
  virtual uint32_t connected_outputs() const { return 0; }
  // Whether refresh_outputs() can see monitors come and go (the probe reads the GPU's own hot-plug lines and EDIDs). False: it cannot tell.
  virtual bool reads_outputs() const { return false; }
  // Looks at the GPU's outputs again (a monitor may have been plugged in while the GPU slept and its hot-plug detection was dark): true if
  // what connected_outputs() and the EDIDs say changed since the last look. The GPU is awake (and resume() has run after a wake). Multi/GpuHotplug
  // calls it through GpuPool::refresh_outputs().
  [[nodiscard]] virtual bool refresh_outputs() { return false; }

 protected:
  IGpuProbe() = default;
  ~IGpuProbe() = default;
};

// Builds the probe for a GPU, or an empty pointer when there is none for it. The pool calls it with the GPU awake.
using GpuProbeFactory = UniquePointer<IGpuProbe> (*)(const GpuContext& context, allocators::IAllocator& allocator);

// The kernel's probes (GpuProbes.cpp): NVIDIA through the native driver's read-only start (chip, VRAM, VBIOS, sensors, connectors).
UniquePointer<IGpuProbe> make_vendor_gpu_probe(const GpuContext& context, allocators::IAllocator& allocator);

}  // namespace hardware
