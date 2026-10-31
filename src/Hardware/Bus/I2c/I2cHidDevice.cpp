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


#include "Hardware/Bus/I2c/I2cHidDevice.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {

constexpr uint8_t kOpcodeReset = 0x01, kOpcodeSetReport = 0x03, kOpcodeSetPower = 0x08;
constexpr uint32_t kResetPolls = 200;                // x 5 ms: the device answers a reset within a second (the kernel waits up to five)

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

bool I2cHidDevice::read_register(uint16_t reg, ByteView out) {
  const uint8_t r[2] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(reg >> 8)};
  return bus_.transfer(address_, ConstByteView(r, 2), out);
}

// A command: the command register, then the opcode in the high byte of the next 16 bits and the report type / id or power state in the low byte
bool I2cHidDevice::command(uint8_t opcode, uint8_t low_byte) {
  const uint8_t c[4] = {static_cast<uint8_t>(descriptor_.command_register), static_cast<uint8_t>(descriptor_.command_register >> 8), low_byte, opcode};
  return bus_.transfer(address_, ConstByteView(c, 4), ByteView());
}

bool I2cHidDevice::set_power(bool on) { return command(kOpcodeSetPower, on ? 0x00 : 0x01); }

bool I2cHidDevice::reset() {
  if (!command(kOpcodeReset, 0x00)) return false;
  // the device answers with a report of length 0 when it is ready; if it never does (no interrupt line seen here, a device that does not say), go on after the wait
  for (uint32_t i = 0; i < kResetPolls; ++i) {
    delay_us(5000);
    uint8_t reply[2] = {0xFF, 0xFF};
    if (bus_.transfer(address_, ConstByteView(), ByteView(reply, 2)) && le16(reply) == 0) return true;
  }
  return true;
}

bool I2cHidDevice::initialize() {
  uint8_t raw[kDescriptorBytes] = {};
  if (!read_register(descriptor_register_, ByteView(raw, kDescriptorBytes))) return false;
  I2cHidDescriptor& d = descriptor_;
  d.length = le16(raw + 0);
  d.version = le16(raw + 2);
  d.report_descriptor_length = le16(raw + 4);
  d.report_descriptor_register = le16(raw + 6);
  d.input_register = le16(raw + 8);
  d.max_input_length = le16(raw + 10);
  d.output_register = le16(raw + 12);
  d.max_output_length = le16(raw + 14);
  d.command_register = le16(raw + 16);
  d.data_register = le16(raw + 18);
  d.vendor = le16(raw + 20);
  d.product = le16(raw + 22);
  d.revision = le16(raw + 24);
  if (d.length != kDescriptorBytes || d.version != 0x0100 || d.report_descriptor_length == 0 || d.report_descriptor_length > kMaxReportDescriptor || d.max_input_length < 2 ||
      d.max_input_length > kMaxInput || d.input_register == 0 || d.command_register == 0)
    return false;
  if (!set_power(true)) return false;
  delay_us(1000);
  if (!reset()) return false;
  report_descriptor_.clear();
  if (!report_descriptor_.put_zeros(d.report_descriptor_length)) return false;
  return read_register(d.report_descriptor_register, report_descriptor_.writable_view());
}

bool I2cHidDevice::read_input(ByteView out, uint32_t* length) {
  *length = 0;
  const uint32_t want = descriptor_.max_input_length;
  if (want < 2 || want > kMaxInput) return false;
  if (!bus_.transfer(address_, ConstByteView(), ByteView(input_, want))) return false;
  const uint32_t total = le16(input_);
  if (total == 0) return true;                          // the answer to a reset
  if (total < 2 || total > want) return false;
  const uint32_t n = total - 2;
  if (n > out.size()) return false;
  for (uint32_t i = 0; i < n; ++i) out[i] = input_[2 + i];
  *length = n;
  return true;
}

bool I2cHidDevice::send_output(uint8_t id, ConstByteView report) {
  const uint32_t n = static_cast<uint32_t>(report.size());
  if (descriptor_.output_register == 0 || descriptor_.max_output_length < 2 || n + 2 > descriptor_.max_output_length || n + 6 > kMaxInput) return false;
  uint8_t message[kMaxInput + 8];
  // the output register, the length (itself included), the report (preceded by its id when there is one)
  message[0] = static_cast<uint8_t>(descriptor_.output_register);
  message[1] = static_cast<uint8_t>(descriptor_.output_register >> 8);
  const uint32_t total = n + 2 + (id ? 1 : 0);
  message[2] = static_cast<uint8_t>(total);
  message[3] = static_cast<uint8_t>(total >> 8);
  uint32_t at = 4;
  if (id) message[at++] = id;
  for (uint32_t i = 0; i < n; ++i) message[at++] = report[i];
  (void)kOpcodeSetReport;
  return bus_.transfer(address_, ConstByteView(message, at), ByteView());
}

bool I2cHidDevice::set_feature(uint8_t id, ConstByteView report) {
  const uint32_t n = static_cast<uint32_t>(report.size());
  if (descriptor_.command_register == 0 || descriptor_.data_register == 0 || n > kMaxInput - 16) return false;
  uint8_t message[kMaxInput + 8];
  uint32_t at = 0;
  message[at++] = static_cast<uint8_t>(descriptor_.command_register);
  message[at++] = static_cast<uint8_t>(descriptor_.command_register >> 8);
  constexpr uint8_t kFeature = 3;
  if (id < 0x0F) {
    message[at++] = static_cast<uint8_t>((kFeature << 4) | id);
    message[at++] = kOpcodeSetReport;
  } else {
    message[at++] = static_cast<uint8_t>((kFeature << 4) | 0x0F);
    message[at++] = kOpcodeSetReport;
    message[at++] = id;
  }
  message[at++] = static_cast<uint8_t>(descriptor_.data_register);
  message[at++] = static_cast<uint8_t>(descriptor_.data_register >> 8);
  const uint32_t length = n + 2 + (id ? 1 : 0);
  message[at++] = static_cast<uint8_t>(length);
  message[at++] = static_cast<uint8_t>(length >> 8);
  if (id) message[at++] = id;
  for (uint32_t i = 0; i < n; ++i) message[at++] = report[i];
  return bus_.transfer(address_, ConstByteView(message, at), ByteView());
}

}  // namespace hardware
