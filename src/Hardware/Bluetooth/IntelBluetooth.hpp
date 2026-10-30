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

// Intel Bluetooth controllers (the ones on the Wireless-AC 9560 and its relatives, 8087:0aaa and friends): the controller starts in a bootloader and runs nothing until the host sends
// it a signed firmware image (intel/ibt-<variant>-<revision>-<firmware revision>.sfi). The steps are those of the Linux driver's legacy bootloader path (btintel_bootloader_setup):
//
//   identify()       Read Version (vendor command 0xFC05). fw_variant 0x23 is operational firmware already; anything else is the bootloader, and the hardware variant, revision and
//                    firmware revision say which file is needed (firmware_name()).
//   load_firmware()  The boot parameters are read, the image's header (CSS header, public key, signature) goes in as secure-send fragments, then the payload, a few commands at a time;
//                    the bootloader says when it has taken everything (a vendor event) and Intel Reset boots the new image, which says so with another vendor event.
//
// The device configuration file (.ddc) the Linux driver applies afterwards is not loaded; the controller works without it.
#pragma once
#include "Hardware/Bluetooth/HciController.hpp"

namespace hardware {

enum class IntelSetup : uint8_t {
  Ok,                  // the firmware was loaded and runs
  AlreadyRunning,      // operational firmware was there
  NeedFirmware,        // the bootloader is waiting for an image: firmware_name() is the file
  UnsupportedVariant,  // not a legacy bootloader controller (the newer TLV ones are not handled)
  BadFirmware,
  Failed,
  Timeout,
};

struct IntelVersion {
  uint8_t hw_platform = 0, hw_variant = 0, hw_revision = 0, fw_variant = 0, fw_revision = 0, fw_build_num = 0, fw_build_ww = 0, fw_build_yy = 0, fw_patch_num = 0;
};

class IntelBluetooth {
 public:
  explicit IntelBluetooth(HciController& controller) : controller_(controller) {}
  ~IntelBluetooth() { controller_.set_event_hook(nullptr, nullptr); }
  IntelBluetooth(const IntelBluetooth&) = delete;
  IntelBluetooth& operator=(const IntelBluetooth&) = delete;

  IntelSetup identify();
  const IntelVersion& version() const { return version_; }
  bool operational() const { return version_.fw_variant == 0x23; }
  // The image the bootloader needs, after identify() said NeedFirmware.
  const char* firmware_name() const { return name_; }
  IntelSetup load_firmware(ConstByteView image);
  // The address in the bootloader's one-time-programmable memory (or the controller's, once running) is zero or Intel's default: the machine has no address of its own to use.
  bool address_valid() const { return address_valid_; }

 private:
  static bool hook(void* self, const hci::Event& event);
  bool secure_send(uint8_t fragment_type, ConstByteView data);
  bool wait_flag(const volatile bool* flag, uint32_t timeout_ms);
  HciController& controller_;
  IntelVersion version_;
  char name_[64] = {};
  uint32_t dev_revid_ = 0;
  bool address_valid_ = true;
  bool download_done_ = false, download_failed_ = false, booted_ = false;
};

}  // namespace hardware
