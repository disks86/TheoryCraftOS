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


// HID over I2C (Microsoft's protocol, version 1.00): how the touchpad and the touchscreen of the Dell XPS 15 9575 talk. The device has a 30 byte HID descriptor at a register the
// firmware names (ACPI _DSM, see AcpiI2cHid.hpp), which gives the registers of the report descriptor, the input reports, the output reports, the commands and the data. Every
// access is a write of the 16 bit register (little endian) and, for a read, a repeated start and the read. Input reports are read without a register write: the device sends
// its input register's content, a 16 bit length (this field included; 0 is the answer to a reset) followed by the report (preceded by its report id if the device uses ids).
// Written after i2c-hid-core.c (the command encoding, the reset and the power sequence).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/I2c/II2cBus.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

struct I2cHidDescriptor {
  uint16_t length = 0;                 // 30
  uint16_t version = 0;                // 0x0100
  uint16_t report_descriptor_length = 0, report_descriptor_register = 0;
  uint16_t input_register = 0, max_input_length = 0;
  uint16_t output_register = 0, max_output_length = 0;
  uint16_t command_register = 0, data_register = 0;
  uint16_t vendor = 0, product = 0, revision = 0;
};

class I2cHidDevice {
 public:
  static constexpr uint32_t kDescriptorBytes = 30, kMaxReportDescriptor = 16384, kMaxInput = 4096;

  I2cHidDevice(allocators::IAllocator& allocator, II2cBus& bus, uint16_t address, uint16_t descriptor_register)
      : bus_(bus), address_(address), descriptor_register_(descriptor_register), report_descriptor_(allocator) {}

  // Reads the HID descriptor, powers the device on, resets it (the answer is waited for by polling the input register) and reads the report descriptor.
  bool initialize();

  const I2cHidDescriptor& descriptor() const { return descriptor_; }
  ConstByteView report_descriptor() const { return report_descriptor_.view(); }
  uint16_t address() const { return address_; }

  // One input report. `out` receives the length-prefix-free report (its report id byte first, if the device uses ids); *length is its size. False if there was none (a length of
  // 0 is the answer to a reset: *length is 0 then and the call succeeded) or the read failed (*length stays 0 and the function returns false).
  bool read_input(ByteView out, uint32_t* length);
  bool set_power(bool on);
  bool reset();
  // Output report with the report id `id` (0: none) to the output register (or through the command register's SET_REPORT if the device has none).
  bool send_output(uint8_t id, ConstByteView report);

 private:
  bool read_register(uint16_t reg, ByteView out);
  bool command(uint8_t opcode, uint8_t low_byte);
  II2cBus& bus_;
  uint16_t address_, descriptor_register_;
  I2cHidDescriptor descriptor_;
  ByteBuffer report_descriptor_;
  uint8_t input_[kMaxInput + 2] = {};
};

}  // namespace hardware
