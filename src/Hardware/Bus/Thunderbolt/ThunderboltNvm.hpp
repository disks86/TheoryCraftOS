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

// Updating the controller's firmware (its NVM) through the DMA port, the mailbox in the NHI port of the host router's configuration space (dma_port.c and the Intel parts of nvm.c of the Linux
// thunderbolt driver). The controller has two flash areas: the active one it runs from and a second one an update is written to. When the new image is written the controller is asked to
// authenticate it; if the signature is good the areas swap and the controller restarts (it is gone from the bus for a moment), and a bad image leaves the old one running.
//
// NOTHING calls this on its own. A bad write cannot damage the active area, but the reset and the swap are what a wrong image makes fail, so an update is only ever started by an explicit
// call, with an image that passed validate_image(). Only the Intel controllers with the 16 KiB header (Alpine Ridge, Titan Ridge, Ice Lake) are covered.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class TbNvmResult : uint8_t {
  Ok,
  NoDmaPort,   // the host router has no NHI port with the mailbox
  Timeout,     // the controller did not answer
  Access,      // the mailbox refused (MAIL_OUT status 2)
  Auth,        // the controller did not accept the image (status 1)
  Failed,      // any other error
  BadImage,    // validate_image() said no
  BadRequest,  // an argument the mailbox cannot take
};

namespace tb_nvm {
// the registers of the mailbox, in dwords of the NHI port's configuration space
constexpr uint32_t kDmaPortCap = 0x3E, kMailData = 1, kMailDataDwords = 16, kMailIn = 17, kMailOut = 18;
constexpr uint32_t kCmdFlashWrite = 0, kCmdFlashUpdateAuth = 1, kCmdFlashRead = 2, kCmdPowerCycle = 4;
constexpr uint32_t kInCmdShift = 28, kInDwordsShift = 24, kInDwordsMask = 0xF, kInAddressShift = 2, kInAddressMask = 0x3FFFFF, kInCss = 1u << 1, kInRequest = 1u << 0;
constexpr uint32_t kOutResponse = 1u << 29, kOutCmdShift = 4, kOutCmdMask = 0xF, kOutStatusMask = 0xF, kStatusCompleted = 0, kStatusErrAuth = 1, kStatusErrAccess = 2;
constexpr uint32_t kTimeoutUs = 5000000, kAuthTimeoutUs = 150000, kRetries = 3, kPortType = 2;
// the flash image: where the version and the flash size are, the digital section after the header, the size limits
constexpr uint32_t kDeviceId = 0x05, kVersion = 0x08, kFlashSize = 0x45, kMinImage = 32 * 1024, kMaxImage = 1024 * 1024, kHeaderBytes = 16 * 1024, kBlockBytes = kMailDataDwords * 4;
}  // namespace tb_nvm

struct TbNvmVersion {
  uint8_t major = 0, minor = 0;
  uint32_t active_size = 0;    // the size of one flash area, in bytes (an image that is larger does not fit)
};

class TbDmaPort {
 public:
  TbDmaPort(TbControl& control, IThunderboltEvents* events, uint64_t route = 0) : control_(control), events_(events), route_(route) {}

  // Looks for the NHI port (3, 5 or 7 depending on the controller) and remembers it.
  TbNvmResult find();
  uint32_t port() const { return port_; }

  // The active flash area (read) and the other one (write), by byte address; a write needs `address` and `data.size()` to be multiples of 4.
  TbNvmResult flash_read(uint32_t address, ByteView out);
  TbNvmResult flash_write(uint32_t address, ConstByteView data);
  // Starts the authentication of what was written. The controller restarts when it succeeds, so Timeout is a likely answer and means nothing went wrong yet.
  TbNvmResult update_auth();
  // 0: no status from an update; 1: there is one, in `*status` (0 is success; the controller needs a power cycle in any case); -1: it could not be read.
  int update_auth_status(uint32_t* status);
  TbNvmResult power_cycle();

 private:
  TbNvmResult read_dwords(uint32_t offset, ArrayView<uint32_t> out, uint32_t timeout_us);
  TbNvmResult write_dwords(uint32_t offset, ArrayView<const uint32_t> data, uint32_t timeout_us);
  TbNvmResult request(uint32_t in, uint32_t timeout_us);
  TbNvmResult read_block(uint32_t dword_address, ArrayView<uint32_t> out);
  TbNvmResult write_block(uint32_t dword_address, ArrayView<const uint32_t> data);
  TbControl& control_;
  IThunderboltEvents* events_;
  uint64_t route_;
  uint32_t port_ = 0;
};

// Checks an image before anything is written (intel_switch_nvm_validate): the size, the pointer to the digital section (4 KiB aligned, inside the image, with the section's size inside too) and
// the device id in it against `device_id` (what the router reports in dword 0 of its switch configuration space, bits 31:16). `*data_start` is where the data to write starts.
bool tb_nvm_validate_image(ConstByteView image, uint16_t device_id, bool safe_mode, uint32_t* data_start);

// Reads the version out of the active area. The flash size says how large one area is: (1 MiB << (size & 7)) / 8, less the header, halved.
TbNvmResult tb_nvm_read_version(TbDmaPort& port, TbNvmVersion* out);

// Writes a validated image to the other area and asks the controller to authenticate it. `device_id` is what the router reports in its switch configuration space (dword 0, bits 31:16).
// The caller must have closed the paths first (ThunderboltIcm::disconnect_pcie_paths) and expects the controller to go away.
TbNvmResult tb_nvm_update(TbDmaPort& port, ConstByteView image, uint16_t device_id, bool safe_mode);

}  // namespace hardware
