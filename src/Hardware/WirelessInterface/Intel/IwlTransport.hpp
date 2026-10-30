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


// Bringing an Intel Wireless-AC 9000 series card up to a running firmware: the order of iwl_trans_pcie_start_fw (take ownership, interrupts off, the radio switch, the adapter's
// basic functions, the NIC configuration from the firmware's PHY SKU, the receive and command queues, the section transfer) followed by the wait for the firmware's ALIVE interrupt
// and notification and the scheduler's setup (iwl_trans_pcie_fw_alive). Then commands go to the firmware and their responses come back through the receive queue
// (send_sync); everything else the firmware says is passed to a handler. All polled, one thread.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFwLoad.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Hardware/WirelessInterface/Intel/IwlRx.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTx.hpp"

namespace hardware {

namespace iwl_cmd {
constexpr uint8_t kAliveNotification = 0x01;
constexpr uint16_t kAliveStatusOk = 0xCAFE, kAliveStatusError = 0xDEAD;
}  // namespace iwl_cmd

namespace iwl_csr {
constexpr uint32_t kMacShadowRegCtrl = 0x0A8, kMacShadowRegCtrlEnable = 0x800FFFFF;
constexpr uint32_t kHwIfMacStepDashMask = 0xF, kHwIfPhyTypeMask = 0xC00, kHwIfPhyDashMask = 0x3000, kHwIfPhyStepMask = 0xC000, kHwIfMacSi = 0x100, kHwIfRadioSi = 0x200;
constexpr uint32_t kHwIfPhyTypePos = 10, kHwIfPhyDashPos = 12, kHwIfPhyStepPos = 14;
}  // namespace iwl_csr

// What the firmware's ALIVE notification said (a prefix common to every version of the notification).
struct IwlAliveInfo {
  bool valid = false;
  uint16_t status = 0, flags = 0;
  uint32_t ucode_major = 0, ucode_minor = 0, scd_base = 0, error_event_table = 0;
};

class IIwlNotificationHandler {
 public:
  virtual void on_notification(const IwlRxPacket& packet) = 0;

 protected:
  constexpr IIwlNotificationHandler() = default;
  ~IIwlNotificationHandler() = default;
};

class IwlTransport final : private IIwlPacketSink {
 public:
  IwlTransport(IIwlRegisters& registers, allocators::IAllocator& allocator, bool integrated)
      : device_(registers), allocator_(allocator), integrated_(integrated), rx_(device_, allocator), tx_(device_, allocator), load_(device_, allocator) {}

  // Loads `image` of `firmware` and waits for the firmware to come alive. On failure the chip is stopped.
  bool start(const IwlFirmware& firmware, IwlImage image, uint32_t timeout_us, const char** error);
  void stop();
  bool running() const { return running_; }

  // Sends a command and waits for its response, which is copied to `response` (the part that fits); returns the response's length, or -1 on failure (error says why).
  int32_t send_sync(const IwlHostCommand& command, ByteView response, uint32_t timeout_us, const char** error);
  // Handles whatever the firmware has sent; notifications go to the handler. Returns the number of frames.
  uint32_t poll(IIwlNotificationHandler* handler);
  // Notifications that arrive while a command is waited for (or in a poll without a handler) go here.
  void set_handler(IIwlNotificationHandler* handler) { default_handler_ = handler; }

  const IwlAliveInfo& alive() const { return alive_; }
  IwlPcieDevice& device() { return device_; }
  IwlCommandQueue& command_queue() { return tx_; }
  allocators::IAllocator& allocator() { return allocator_; }

 private:
  void on_packet(const IwlRxPacket& packet) override;
  void configure_nic(uint32_t phy_config);

  IwlPcieDevice device_;
  allocators::IAllocator& allocator_;
  bool integrated_;
  IwlRxQueue rx_;
  IwlCommandQueue tx_;
  IwlFwLoad load_;
  IwlAliveInfo alive_;
  bool running_ = false;
  // the command being waited for
  bool waiting_ = false, answered_ = false;
  uint16_t waiting_sequence_ = 0;
  ByteView response_;
  uint32_t response_length_ = 0;
  IIwlNotificationHandler* handler_ = nullptr;
  IIwlNotificationHandler* default_handler_ = nullptr;
};

}  // namespace hardware
