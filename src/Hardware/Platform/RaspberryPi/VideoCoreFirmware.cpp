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

#include "Hardware/Platform/RaspberryPi/VideoCoreFirmware.hpp"

namespace hardware {

bool VideoCoreFirmware::get1(uint32_t tag, uint32_t id, uint32_t* out) {
  const uint32_t request[1] = {id};
  uint32_t answer[2] = {};
  if (!mailbox_.call(tag, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer))) return false;
  if (answer[0] != id) return false;
  *out = answer[1];
  return true;
}

bool VideoCoreFirmware::get_pair(uint32_t tag, uint32_t* a, uint32_t* b) {
  uint32_t answer[2] = {};
  if (!mailbox_.call(tag, ArrayView<const uint32_t>(), ArrayView<uint32_t>(answer))) return false;
  *a = answer[0];
  *b = answer[1];
  return true;
}

bool VideoCoreFirmware::firmware_revision(uint32_t* revision) {
  uint32_t answer[1] = {};
  if (!mailbox_.call(vc_tag::kFirmwareRevision, ArrayView<const uint32_t>(), ArrayView<uint32_t>(answer))) return false;
  *revision = answer[0];
  return true;
}

bool VideoCoreFirmware::board_revision(uint32_t* revision) {
  uint32_t answer[1] = {};
  if (!mailbox_.call(vc_tag::kBoardRevision, ArrayView<const uint32_t>(), ArrayView<uint32_t>(answer))) return false;
  *revision = answer[0];
  return true;
}

bool VideoCoreFirmware::board(PiBoard* out) {
  uint32_t code = 0;
  if (!board_revision(&code)) return false;
  *out = pi_decode_revision(code);
  return out->valid;
}

bool VideoCoreFirmware::serial(uint64_t* serial) {
  uint32_t answer[2] = {};
  if (!mailbox_.call(vc_tag::kBoardSerial, ArrayView<const uint32_t>(), ArrayView<uint32_t>(answer))) return false;
  *serial = answer[0] | (static_cast<uint64_t>(answer[1]) << 32);
  return true;
}

bool VideoCoreFirmware::arm_memory(uint32_t* base, uint32_t* bytes) { return get_pair(vc_tag::kArmMemory, base, bytes); }
bool VideoCoreFirmware::gpu_memory(uint32_t* base, uint32_t* bytes) { return get_pair(vc_tag::kVcMemory, base, bytes); }

bool VideoCoreFirmware::clock_rate(uint32_t clock, uint32_t* hz) { return get1(vc_tag::kGetClockRate, clock, hz); }
bool VideoCoreFirmware::measured_clock_rate(uint32_t clock, uint32_t* hz) { return get1(vc_tag::kGetMeasuredClockRate, clock, hz); }
bool VideoCoreFirmware::max_clock_rate(uint32_t clock, uint32_t* hz) { return get1(vc_tag::kGetMaxClockRate, clock, hz); }
bool VideoCoreFirmware::min_clock_rate(uint32_t clock, uint32_t* hz) { return get1(vc_tag::kGetMinClockRate, clock, hz); }

bool VideoCoreFirmware::set_clock_rate(uint32_t clock, uint32_t hz, uint32_t* actual_hz) {
  const uint32_t request[3] = {clock, hz, 0};  // third word: 1 would skip turbo
  uint32_t answer[2] = {};
  if (!mailbox_.call(vc_tag::kSetClockRate, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer))) return false;
  if (answer[0] != clock) return false;
  if (actual_hz) *actual_hz = answer[1];
  return true;
}

bool VideoCoreFirmware::set_clock_enabled(uint32_t clock, bool on) {
  const uint32_t request[2] = {clock, on ? 1u : 0u};  // bit 0: on; bit 1 would say "the clock does not exist"
  uint32_t answer[2] = {};
  if (!mailbox_.call(vc_tag::kSetClockState, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer))) return false;
  return answer[0] == clock && (answer[1] & 2) == 0 && ((answer[1] & 1) != 0) == on;
}

bool VideoCoreFirmware::clock_enabled(uint32_t clock, bool* on) {
  uint32_t state = 0;
  if (!get1(vc_tag::kGetClockState, clock, &state) || (state & 2)) return false;
  *on = (state & 1) != 0;
  return true;
}

bool VideoCoreFirmware::temperature(uint32_t* millicelsius) { return get1(vc_tag::kGetTemperature, 0, millicelsius); }
bool VideoCoreFirmware::max_temperature(uint32_t* millicelsius) { return get1(vc_tag::kGetMaxTemperature, 0, millicelsius); }

bool VideoCoreFirmware::throttled(uint32_t* bits) {
  const uint32_t read_only[1] = {0};  // the mask of sticky bits to clear: none
  uint32_t answer[1] = {};
  if (!mailbox_.call(vc_tag::kGetThrottled, ArrayView<const uint32_t>(read_only), ArrayView<uint32_t>(answer))) return false;
  *bits = answer[0];
  return true;
}

bool VideoCoreFirmware::allocate_memory(uint32_t bytes, uint32_t alignment, uint32_t flags, uint32_t* handle) {
  const uint32_t request[3] = {bytes, alignment, flags};
  uint32_t answer[1] = {};
  if (!mailbox_.call(vc_tag::kAllocateMemory, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer)) || answer[0] == 0) return false;
  *handle = answer[0];
  return true;
}

bool VideoCoreFirmware::lock_memory(uint32_t handle, uint32_t* bus_address) {
  const uint32_t request[1] = {handle};
  uint32_t answer[1] = {};
  if (!mailbox_.call(vc_tag::kLockMemory, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer)) || answer[0] == 0) return false;
  *bus_address = answer[0];
  return true;
}

bool VideoCoreFirmware::unlock_memory(uint32_t handle) {
  const uint32_t request[1] = {handle};
  uint32_t answer[1] = {};
  return mailbox_.call(vc_tag::kUnlockMemory, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer)) && answer[0] == 0;
}

bool VideoCoreFirmware::release_memory(uint32_t handle) {
  const uint32_t request[1] = {handle};
  uint32_t answer[1] = {};
  return mailbox_.call(vc_tag::kReleaseMemory, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer)) && answer[0] == 0;
}

bool VideoCoreFirmware::edid_block(uint32_t block, ArrayView<uint8_t> out) {
  if (out.size() < 128) return false;
  const uint32_t request[1] = {block};
  uint32_t answer[34] = {};  // block number, status, then 128 bytes
  if (!mailbox_.call(vc_tag::kGetEdidBlock, ArrayView<const uint32_t>(request), ArrayView<uint32_t>(answer))) return false;
  if (answer[0] != block || answer[1] != 0) return false;
  for (uint64_t i = 0; i < 128; ++i) out[i] = static_cast<uint8_t>(answer[2 + i / 4] >> (8 * (i % 4)));
  return true;
}

}  // namespace hardware
