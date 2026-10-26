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
#include <stdint.h>
#include "Hardware/Platform/RaspberryPi/RaspberryPiBoard.hpp"
#include "Hardware/Platform/RaspberryPi/VcMailbox.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// What the firmware's `kGetThrottled` word says (bits of the firmware's throttled status).
namespace vc_throttled {
constexpr uint32_t kUnderVoltage = 1u << 0, kFrequencyCapped = 1u << 1, kThrottled = 1u << 2, kSoftTemperatureLimit = 1u << 3;
}

// The firmware services a driver of the Raspberry Pi's media hardware needs, over a VcMailbox: who the board is, how much memory the GPU keeps, clocks, the
// temperature, and memory for the GPU that the firmware hands out and pins in place.
class VideoCoreFirmware {
 public:
  explicit VideoCoreFirmware(VcMailbox& mailbox) : mailbox_(mailbox) {}

  bool firmware_revision(uint32_t* revision);
  bool board_revision(uint32_t* revision);
  bool board(PiBoard* out);
  bool serial(uint64_t* serial);
  // Memory split between the ARM cores and the GPU: [base, base + bytes).
  bool arm_memory(uint32_t* base, uint32_t* bytes);
  bool gpu_memory(uint32_t* base, uint32_t* bytes);

  // Clocks (vc_clock numbers), in hertz.
  bool clock_rate(uint32_t clock, uint32_t* hz);
  bool measured_clock_rate(uint32_t clock, uint32_t* hz);
  bool max_clock_rate(uint32_t clock, uint32_t* hz);
  bool min_clock_rate(uint32_t clock, uint32_t* hz);
  // Sets the rate; the firmware answers with the rate it really set (turbo is not skipped). False if it did not take the request.
  bool set_clock_rate(uint32_t clock, uint32_t hz, uint32_t* actual_hz = nullptr);
  // Turns the clock on or off (the V3D and HEVC clocks are off until asked).
  bool set_clock_enabled(uint32_t clock, bool on);
  bool clock_enabled(uint32_t clock, bool* on);

  // The SoC temperature and the limit the firmware throttles at, in thousandths of a degree Celsius.
  bool temperature(uint32_t* millicelsius);
  bool max_temperature(uint32_t* millicelsius);
  // vc_throttled bits since boot (the upper half) and now (the lower half).
  bool throttled(uint32_t* bits);

  // Memory for the GPU: allocate gives a handle, lock pins it and returns its bus address, unlock lets it move, release frees it.
  bool allocate_memory(uint32_t bytes, uint32_t alignment, uint32_t flags, uint32_t* handle);
  bool lock_memory(uint32_t handle, uint32_t* bus_address);
  bool unlock_memory(uint32_t handle);
  bool release_memory(uint32_t handle);

  // One 128-byte block of the attached display's EDID (blocks count from 0).
  bool edid_block(uint32_t block, ArrayView<uint8_t> out);

 private:
  bool get1(uint32_t tag, uint32_t id, uint32_t* out);
  bool get_pair(uint32_t tag, uint32_t* a, uint32_t* b);
  VcMailbox& mailbox_;
};

}  // namespace hardware
