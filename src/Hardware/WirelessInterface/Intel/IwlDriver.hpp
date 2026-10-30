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


// The Intel Wireless-AC 9000 series driver as the system uses it: binds the PCI function (memory decoding and bus mastering on, BAR 0 mapped), reads the hardware revision to name
// the firmware file, finds the newest "/firmware/iwlwifi-...-<api>.ucode" there, starts the firmware and runs the initialisation handshake, leaving the card with a hardware address,
// an SKU and a channel list. Scanning and joining a network are not here yet, so this does not present an IWirelessInterface.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/WirelessInterface/Intel/IwlDevices.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Hardware/WirelessInterface/Intel/IwlInit.hpp"
#include "Hardware/WirelessInterface/Intel/IwlNvm.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Where firmware files come from. The system's is the "/firmware" folder of the file system.
class IIwlFirmwareSource {
 public:
  // Replaces `out` with the whole file called `name`; false if there is no such file (or it is empty or too large).
  virtual bool read(const char* name, ByteBuffer& out) = 0;

 protected:
  constexpr IIwlFirmwareSource() = default;
  ~IIwlFirmwareSource() = default;
};

class FileSystemFirmwareSource final : public IIwlFirmwareSource {
 public:
  FileSystemFirmwareSource(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator) : file_system_(file_system), allocator_(allocator) {}
  bool read(const char* name, ByteBuffer& out) override;

 private:
  filesystem::IFileSystem& file_system_;
  allocators::IAllocator& allocator_;
};

class IwlDriver {
 public:
  enum class Outcome : uint8_t {
    NotStarted,
    Started,
    UnknownDevice,         // not a function of the 9000 series
    BarUnmapped,           // BAR 0 is not assigned or cannot be mapped
    UnsupportedHardware,   // a MAC or radio this driver has no firmware name for
    FirmwareMissing,       // no file of any supported API version in /firmware
    FirmwareInvalid,       // the file is not a whole firmware file
    StartFailed,           // the card or its firmware did not come up
    InitFailed,            // the initialisation handshake or the NVM failed
  };
  static constexpr uint32_t kRegisterBytes = 0x2000;
  static constexpr uint32_t kTimeoutUs = 2000000;

  explicit IwlDriver(allocators::IAllocator& allocator) : allocator_(allocator), file_(allocator), firmware_(allocator) {}
  ~IwlDriver() { stop(); }
  IwlDriver(const IwlDriver&) = delete;
  IwlDriver& operator=(const IwlDriver&) = delete;

  static bool supports(const PciDeviceInfo& device) { return iwl_find_pci_device(device.vendor_id, device.device_id) != nullptr; }
  static const char* outcome_name(Outcome outcome);

  // Whole start for a PCI function: bind, then start_device().
  Outcome start(IPciBus& pci, const PciDeviceInfo& device, IRegisterMapper& mapper, IIwlFirmwareSource& firmware);
  // The part after the registers are reachable (tests bring their own).
  Outcome start_device(IIwlRegisters& registers, bool integrated, IIwlFirmwareSource& firmware);
  void stop();

  Outcome outcome() const { return outcome_; }
  const char* message() const { return message_; }
  const char* firmware_name() const { return firmware_name_; }
  bool running() const { return transport_ && transport_->running(); }
  const IwlNvmData& nvm() const { return nvm_; }
  IwlTransport* transport() { return transport_.get(); }

 private:
  Outcome fail(Outcome outcome, const char* message);

  allocators::IAllocator& allocator_;
  UniquePointer<MmioIwlRegisters> mmio_registers_;
  UniquePointer<IwlTransport> transport_;
  ByteBuffer file_;
  IwlFirmware firmware_;
  IwlNvmData nvm_;
  Outcome outcome_ = Outcome::NotStarted;
  const char* message_ = "";
  char firmware_name_[64] = {};
};

}  // namespace hardware
