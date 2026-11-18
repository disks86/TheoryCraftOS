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

// UCSI over ACPI (as Linux drivers/usb/typec/ucsi/ucsi_acpi.c): the device with _HID or _CID "PNP0CA0" (often _HID "USBC000") describes the
// shared data structure as a memory range in its _CRS and has a _DSM (UUID 6f8398c2-7ca4-11e4-ad36-631042b5008f, revision 1) whose function 1
// hands what the OS wrote (CONTROL, MESSAGE OUT) to the policy manager - usually the embedded controller - and whose function 2 copies the
// policy manager's CCI and MESSAGE IN back into the memory. The firmware raises Notify(device, 0x80) when CCI changed.
//
// The memory is read and written through the AML host's SystemMemory accesses, byte by byte: the same path the firmware's own fields of that
// memory take (an OperationRegion over it), so both see the same bytes and the kernel maps the range once.
#pragma once
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/UsbTypeC/IUcsiTransport.hpp"

namespace hardware {

class AcpiUcsiTransport final : public IUcsiTransport {
 public:
  static constexpr uint8_t kDsmFunctionQuery = 0, kDsmFunctionWrite = 1, kDsmFunctionRead = 2;
  AcpiUcsiTransport(acpi::AmlInterpreter& vm, acpi::IAmlHost& memory, acpi::Node* device) : vm_(vm), memory_(memory), device_(device) {}

  // The UCSI devices of the namespace (present ones: _STA, where there is one, says so). Returns how many went into `out`.
  static uint32_t find_devices(acpi::AmlInterpreter& vm, ArrayView<acpi::Node*> out);

  // Reads _CRS (the mailbox), checks that _DSM offers the read and write functions and reads VERSION. False if any of that is missing.
  [[nodiscard]] bool initialize();
  acpi::Node* device() const { return device_; }
  uint64_t mailbox_address() const { return base_; }
  uint32_t mailbox_length() const { return length_; }
  uint16_t version() const { return version_; }   // (BCD, as read at initialize())
  uint32_t dsm_calls() const { return dsm_calls_; }

  bool read_version(uint16_t* version) override;
  bool read_cci(uint32_t* cci) override;
  bool read_message_in(ByteView out) override;
  bool write_message_out(ConstByteView data) override;
  bool write_control(uint64_t control) override;
  uint32_t message_capacity() const override { return capacity_; }

 private:
  [[nodiscard]] bool dsm(uint8_t function, acpi::Value* result = nullptr);
  bool fail();
  [[nodiscard]] bool read_bytes(uint32_t offset, ByteView out);
  [[nodiscard]] bool write_bytes(uint32_t offset, ConstByteView data);

  acpi::AmlInterpreter& vm_;
  acpi::IAmlHost& memory_;
  acpi::Node* device_;
  acpi::Node* dsm_node_ = nullptr;
  uint64_t base_ = 0;
  uint32_t length_ = 0;
  uint16_t version_ = 0;
  uint32_t capacity_ = 16;
  uint32_t message_out_ = 0x20;
  uint32_t dsm_calls_ = 0;
};

}  // namespace hardware
