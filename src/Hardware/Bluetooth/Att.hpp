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

// The attribute protocol client (Core spec vol 3 part F) on the fixed channel 4 of a link: requests are sent one at a time per link and answered by the poll loop (`request()` waits for
// the answer, running the controller meanwhile), notifications and indications are handed to a handler (indications are confirmed). Atomic reads of long values (read blob) and the
// MTU exchange are here; discovery and characteristics are in GattClient. An answer of "insufficient authentication / encryption" is passed to the security handler, which may bring the
// link up to the level asked for (pairing or encryption), and the request is then sent once more.
#pragma once
#include "Hardware/Bluetooth/L2cap.hpp"

namespace hardware {

namespace att {
constexpr uint8_t kErrorResponse = 0x01, kMtuRequest = 0x02, kMtuResponse = 0x03, kFindInformationRequest = 0x04, kFindInformationResponse = 0x05, kFindByTypeValueRequest = 0x06,
                  kFindByTypeValueResponse = 0x07, kReadByTypeRequest = 0x08, kReadByTypeResponse = 0x09, kReadRequest = 0x0A, kReadResponse = 0x0B, kReadBlobRequest = 0x0C,
                  kReadBlobResponse = 0x0D, kReadByGroupTypeRequest = 0x10, kReadByGroupTypeResponse = 0x11, kWriteRequest = 0x12, kWriteResponse = 0x13, kWriteCommand = 0x52,
                  kHandleValueNotification = 0x1B, kHandleValueIndication = 0x1D, kHandleValueConfirmation = 0x1E;
constexpr uint8_t kInvalidHandle = 0x01, kReadNotPermitted = 0x02, kWriteNotPermitted = 0x03, kInvalidPdu = 0x04, kInsufficientAuthentication = 0x05, kRequestNotSupported = 0x06,
                  kInvalidOffset = 0x07, kInsufficientAuthorization = 0x08, kAttributeNotFound = 0x0A, kAttributeNotLong = 0x0B, kInsufficientKeySize = 0x0C, kInsufficientEncryption = 0x0F;
constexpr uint16_t kDefaultMtu = 23, kMaxMtu = 517;
}  // namespace att

enum class AttStatus : uint8_t { Ok, Timeout, NoLink, Malformed, Error, Busy };

using AttNotificationHandler = void (*)(void* context, uint16_t link, uint16_t handle, ConstByteView value, bool indication);
// The answer said the link needs more security (`error` is the ATT error code): make it so and return true, or return false to give up.
using AttSecurityHandler = bool (*)(void* context, uint16_t link, uint8_t error);

class AttClient final : public IL2capFixedHandler {
 public:
  AttClient(HciController& controller, L2cap& l2cap);
  ~AttClient();
  AttClient(const AttClient&) = delete;
  AttClient& operator=(const AttClient&) = delete;

  void set_notification_handler(AttNotificationHandler handler, void* context) { notify_ = handler; notify_context_ = context; }
  void set_security_handler(AttSecurityHandler handler, void* context) { security_ = handler; security_context_ = context; }

  // Sends `pdu` (opcode first) and waits for the answer with opcode `expected`; the answer's PDU is in response(). An error response gives AttStatus::Error with error_code() and
  // error_handle().
  AttStatus request(uint16_t link, ConstByteView pdu, uint8_t expected, uint32_t timeout_ms = 3000);
  AttStatus command(uint16_t link, ConstByteView pdu);              // (a write command: no answer)
  ConstByteView response() const { return ConstByteView(response_).first(response_size_); }
  uint8_t error_code() const { return error_code_; }
  uint16_t error_handle() const { return error_handle_; }

  // Exchange MTU: asks for `mtu` (at most kMaxMtu); the agreed value is mtu(link).
  AttStatus exchange_mtu(uint16_t link, uint16_t mtu = 247);
  uint16_t mtu(uint16_t link) const;
  // Reads the value of an attribute, in as many blob reads as it takes, into `out`; `*length` is the size (at most out.size(); a longer value is cut).
  AttStatus read(uint16_t link, uint16_t handle, ByteView out, uint32_t* length);
  AttStatus write(uint16_t link, uint16_t handle, ConstByteView value);
  AttStatus write_without_response(uint16_t link, uint16_t handle, ConstByteView value);

  void on_data(const HciLink& link, ConstByteView sdu) override;
  void on_link(const HciLink& link, bool up) override;

 private:
  struct State { bool used = false; uint16_t link = 0, mtu = att::kDefaultMtu; };
  State* state_for(uint16_t link, bool create);
  AttStatus send_and_wait(uint16_t link, ConstByteView pdu, uint8_t expected, uint32_t timeout_ms);
  HciController& controller_;
  L2cap& l2cap_;
  State states_[HciController::kMaxLinks];
  // the request in flight
  bool waiting_ = false, answered_ = false, errored_ = false;
  uint16_t waiting_link_ = 0;
  uint8_t waiting_expected_ = 0, waiting_request_opcode_ = 0, error_code_ = 0;
  uint16_t error_handle_ = 0;
  uint8_t response_[att::kMaxMtu + 8] = {};
  uint32_t response_size_ = 0;
  AttNotificationHandler notify_ = nullptr;
  void* notify_context_ = nullptr;
  AttSecurityHandler security_ = nullptr;
  void* security_context_ = nullptr;
};

}  // namespace hardware
