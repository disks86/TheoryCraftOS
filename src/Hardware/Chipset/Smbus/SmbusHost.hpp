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

// The SMBus host controller that Intel chipsets (ICH8 through the current PCHs and SoCs: Linux's i2c-i801) and AMD chipsets (the FCH of every Zen
// SoC and its SB800/Hudson predecessors: Linux's i2c-piix4) both implement: a small block of 8-bit I/O registers where the OS writes an address,
// a command and data, starts a protocol and polls the status register. The two differ only in who else may use the controller: Intel arbitrates
// with the INUSE_STS hardware semaphore, AMD with a host semaphore bit in the slave control register that the firmware's IMC/SMU also honours.
#pragma once
#include <stdint.h>
#include "Hardware/Chipset/Smbus/ISmbusController.hpp"

namespace hardware {

// The controller's registers (offsets from its I/O base) and a way to wait. The real machine uses SmbusIoPorts; a test hands out a model.
class ISmbusHostPorts {
 public:
  virtual uint8_t read8(uint8_t reg) = 0;
  virtual void write8(uint8_t reg, uint8_t value) = 0;
  virtual void pause_us(uint32_t us) = 0;

 protected:
  constexpr ISmbusHostPorts() = default;
  ~ISmbusHostPorts() = default;
};

enum class SmbusHostFlavor : uint8_t { IntelI801, AmdFch };

namespace smbus_reg {
constexpr uint8_t kHostStatus = 0x00, kHostControl = 0x02, kHostCommand = 0x03, kHostAddress = 0x04, kHostData0 = 0x05, kHostData1 = 0x06,
                  kBlockData = 0x07;
constexpr uint8_t kAmdSlaveControl = 0x08;                 // AMD: SMBSLVCNT, bit 4 requests the host semaphore, bit 5 releases it
constexpr uint8_t kIntelAuxControl = 0x0D;                 // Intel: SMBAUXCTL, bit 1 (E32B) turns on the 32-byte block buffer

constexpr uint8_t kStatusBusy = 0x01, kStatusInterrupt = 0x02, kStatusDeviceError = 0x04, kStatusBusError = 0x08, kStatusFailed = 0x10,
                  kStatusInUse = 0x40, kStatusByteDone = 0x80;
constexpr uint8_t kStatusErrors = kStatusDeviceError | kStatusBusError | kStatusFailed;
constexpr uint8_t kStatusClear = kStatusInterrupt | kStatusErrors | 0x20 /* SMBALERT */ | kStatusByteDone;

constexpr uint8_t kControlKill = 0x02, kControlStart = 0x40;
constexpr uint8_t kProtocolQuick = 0x00, kProtocolByte = 0x04, kProtocolByteData = 0x08, kProtocolWordData = 0x0C, kProtocolBlockData = 0x14;

constexpr uint8_t kAmdSemaphoreRequest = 0x10, kAmdSemaphoreRelease = 0x20;
constexpr uint8_t kIntelAuxE32b = 0x02;
}  // namespace smbus_reg

// One host controller. Not thread-safe by itself: several users share it through a lock (SystemChipset keeps it in a Synchronized).
class SmbusHost final : public ISmbusController {
 public:
  constexpr SmbusHost() = default;
  SmbusHost(const SmbusHost&) = delete;
  SmbusHost& operator=(const SmbusHost&) = delete;

  // Points the host at a controller (the ports object must outlive this). nullptr leaves it without one: every transaction says NoController.
  void attach(ISmbusHostPorts* ports, SmbusHostFlavor flavor) {
    ports_ = ports;
    flavor_ = flavor;
  }
  bool attached() const { return ports_ != nullptr; }
  SmbusHostFlavor flavor() const { return flavor_; }

  // How many status polls a transaction waits (each `kPollUs` apart) before it is killed.
  static constexpr uint32_t kPollUs = 100, kMaxPolls = 1000;

  SmbusStatus quick(uint8_t address, bool read) override;
  SmbusStatus read_byte(uint8_t address, uint8_t* value) override;
  SmbusStatus read_byte_data(uint8_t address, uint8_t command, uint8_t* value) override;
  SmbusStatus write_byte_data(uint8_t address, uint8_t command, uint8_t value) override;
  SmbusStatus read_word_data(uint8_t address, uint8_t command, uint16_t* value) override;
  SmbusStatus read_block_data(uint8_t address, uint8_t command, ArrayView<uint8_t> out, uint8_t* length) override;

 private:
  struct Request {
    uint8_t address;
    bool read;
    uint8_t command;
    uint8_t protocol;
    uint8_t data0, data1;
  };
  // Takes the controller from the firmware's side of the arbitration, runs the request, and gives it back.
  SmbusStatus run(const Request& request, uint8_t* data0, uint8_t* data1, ArrayView<uint8_t> block, uint8_t* block_length);
  SmbusStatus claim();
  void release();
  SmbusStatus execute(const Request& request);

  ISmbusHostPorts* ports_ = nullptr;
  SmbusHostFlavor flavor_ = SmbusHostFlavor::IntelI801;
};

}  // namespace hardware
