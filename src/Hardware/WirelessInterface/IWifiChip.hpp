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

// The lower half of a Wi-Fi driver: one chip, from power-on to "firmware running, hardware address known". Every vendor folder implements it for its families
// (RtwChip, Mt7921Chip, Ath10kChip, BrcmfChip, ...). It is what the chip-specific parts have in common, so the system can start any adapter the same way and show what
// happened; the 802.11 upper half (scan, join, data, which is IWirelessInterface) sits on top of a chip once its driver family has one.
//
// A chip reaches its registers through WifiChipBus: PCIe chips through their mapped BAR (INicRegisters, a host test hands in a register model), USB chips through
// control transfers on endpoint 0 (each vendor wraps them as INicRegisters, see the vendor's *UsbRegisters), SDIO chips through an IWifiSdio function.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/WirelessInterface/WifiAdapterModel.hpp"
#include "Hardware/WirelessInterface/WifiFirmwareSource.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// An SDIO function (CMD52 single bytes and CMD53 blocks). The machines here have no SDIO host driver yet; the interface is what Broadcom's and Realtek's SDIO parts
// need, so their code can be written and tested against a model.
class IWifiSdio {
 public:
  virtual bool read8(uint8_t function, uint32_t address, uint8_t* value) = 0;
  virtual bool write8(uint8_t function, uint32_t address, uint8_t value) = 0;
  // CMD53, incrementing address. False if the transfer failed.
  virtual bool read_block(uint8_t function, uint32_t address, ByteView data) = 0;
  virtual bool write_block(uint8_t function, uint32_t address, ConstByteView data) = 0;

 protected:
  constexpr IWifiSdio() = default;
  ~IWifiSdio() = default;
};

// How a chip reaches its hardware. Exactly one of `registers` (PCIe, or USB wrapped by the vendor), `usb` and `sdio` is what the chip uses; the others may be null.
struct WifiChipBus {
  INicRegisters* registers = nullptr;   // a mapped BAR (PCIe)
  IPciBus* pci = nullptr;               // configuration space (subsystem IDs for board files, power management); may be null in tests
  PciDeviceInfo pci_device{};
  IUsbHostController* usb = nullptr;    // the controller and root port of a USB adapter
  uint32_t usb_port = 0;
  IWifiSdio* sdio = nullptr;
};

enum class WifiChipState : uint8_t {
  Off,               // not started (or stopped)
  Identified,        // the chip answered: its ID and revision were read
  FirmwareLoaded,    // the firmware was downloaded and reported that it runs
  Ready,             // the firmware answered the first commands; the hardware address is known
  Failed,
};

enum class WifiChipResult : uint8_t {
  Ok,
  NoRegisters,         // the bus gave no way to reach the chip
  NoResponse,          // registers read as all ones / the chip never became ready
  UnknownRevision,     // a chip ID or revision this driver has no data (firmware, power sequence) for
  FirmwareMissing,     // a file was not in /firmware
  FirmwareInvalid,     // a file was there but not well-formed (or not for this chip)
  FirmwareRejected,    // the chip refused it (checksum, signature) or never said it was running
  Timeout,             // a command or a state change did not finish in time
  Unsupported,         // the step exists on this family but is not written yet
};

const char* wifi_chip_state_name(WifiChipState state);
const char* wifi_chip_result_name(WifiChipResult result);

// What the chip said about itself.
struct WifiChipIdentity {
  uint32_t chip_id = 0;        // the chip's own ID register (0x8822, 0x7961, 0x0050 for QCA6174 ...)
  uint32_t revision = 0;       // its revision / cut / ECO
  char firmware_version[48] = {};   // as the firmware reports it, or as its file header says ("" before FirmwareLoaded)
};

class IWifiChip {
 public:
  virtual const WifiAdapterModel& model() const = 0;
  // Powers the chip on, identifies it, loads its firmware from `firmware` and waits until it runs. Ok once state() is Ready, or as far as the driver family goes so far
  // (state() then tells where it stopped, and the result is Unsupported for the first missing step). Calling it again after stop() starts over.
  [[nodiscard]] virtual WifiChipResult start(IWifiFirmwareSource& firmware) = 0;
  // Stops the chip's DMA and powers it down. Safe in any state.
  virtual void stop() = 0;

  virtual WifiChipState state() const = 0;
  virtual const WifiChipIdentity& identity() const = 0;
  // The address burnt into the chip (eFuse/OTP/NVRAM/firmware), valid from Ready on - or earlier where the family reads it directly. All zeros when unknown.
  virtual MacAddress mac_address() const = 0;
  // The main firmware file the chip wants (after Identified, when it depends on the revision).
  virtual const char* firmware_name() const = 0;
  // A static line saying what happened last ("firmware 9.9.15 running", "eFuse read failed", ...).
  virtual const char* message() const = 0;

 protected:
  // (owned through UniquePointer, which destroys the concrete chip: no virtual destructor, so the kernel needs no operator delete)
  IWifiChip() = default;
  ~IWifiChip() = default;
};

}  // namespace hardware
