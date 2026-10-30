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

// The IPC of the Intel Integrated Sensor Hub (ISH), the host's side of the doorbell and message registers (intel-ish-hid/ipc of Linux, for the Sunrise Point and Kaby Lake parts, which
// use the "BXT" interrupt registers): BAR 0 has a doorbell and a message area in each direction. A message is written to the message registers and announced by writing the doorbell
// (length in bits 0-9, protocol in bits 10-13, a management command in bits 16-19, busy in bit 31); the receiver clears its doorbell when it has read the message. The protocols are the
// management one (reset handshake) and ISHTP (a 4 byte ISHTP header, then the payload; see Ishtp.hpp). The firmware is started by the BIOS; the driver only tells it the host is ready and
// resets the IPC. Everything here is polled.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

class IIshRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

 protected:
  constexpr IIshRegisters() = default;
  ~IIshRegisters() = default;
};

namespace ish_reg {
constexpr uint32_t kPimr = 0x08, kPisr = 0x0C;                         // interrupt mask / status (the BXT layout)
constexpr uint32_t kFwStatus = 0x34, kHostComm = 0x38, kHostToIshDoorbell = 0x48, kIshToHostDoorbell = 0x54, kIshToHostMessage = 0x60, kHostToIshMessage = 0xE0, kRemap2 = 0x368;
constexpr uint32_t kDoorbellBusy = 1u << 31;
constexpr uint32_t kFwIlup = 1u << 0, kFwIshtpReady = 1u << 1;
constexpr uint32_t kHostCommReady = 0x81;                              // READY (bit 7) and ILUP (bit 0)
constexpr uint32_t kPimrInterruptEnable = 1u << 0, kPimrHostToIshBusyClear = 1u << 8;
constexpr uint32_t kProtocolIshtp = 1, kProtocolManagement = 3;
constexpr uint32_t kMngRxCompletion = 2, kMngResetNotify = 3, kMngResetNotifyAck = 4, kMngSyncFwClock = 5;
constexpr uint32_t kMaxMessage = 128;                                  // bytes of a message (the ISHTP header included)
}  // namespace ish_reg

struct IshMessage {
  uint32_t protocol = 0;
  uint32_t management_command = 0;
  uint32_t length = 0;                                                 // bytes in `data`
  uint8_t data[ish_reg::kMaxMessage] = {};
};

class IshIpc {
 public:
  explicit IshIpc(IIshRegisters& registers) : registers_(registers) {}

  // Tells the firmware the host is ready, enables its DMA window, resets the IPC (reset notify, waits for the acknowledgement) and waits for the firmware to say it is up and speaks
  // ISHTP. False if it never does.
  bool start();
  bool firmware_ready();

  // Sends an ISHTP message: the 4 byte header (fw_addr, host_addr, length 9 bits, msg_complete) then the payload (at most kMaxMessage - 4 bytes). False if the doorbell stays busy.
  bool send_ishtp(uint8_t fw_addr, uint8_t host_addr, bool complete, ConstByteView payload);
  bool send_management(uint32_t command, ConstByteView payload);
  // Takes the next message of the firmware, if there is one. The reset handshake of the management protocol is dealt with here (a reset notify is acknowledged) and reported through
  // reset_seen(); the other messages are given to the caller.
  bool receive(IshMessage* out);
  // Whether the firmware reset itself since the last call (all connections are gone then).
  bool take_reset_seen();

 private:
  bool wait_input_ready();
  bool write_message(uint32_t doorbell, ConstByteView first, ConstByteView second);
  void set_host_ready();
  IIshRegisters& registers_;
  bool reset_seen_ = false, reset_acked_ = false;
};

}  // namespace hardware
