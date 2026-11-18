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

// The OS policy manager half of UCSI (shaped after Linux drivers/usb/typec/ucsi/ucsi.c): it resets the platform policy manager, enables its
// notifications, reads the capabilities and, for every connector, what it can do, its status, the source's PDOs, the partner's alternate modes
// and the cable. A command is CONTROL written, CCI watched until the PPM says it completed (BUSY means wait; ERROR is followed by
// GET_ERROR_STATUS; NOT SUPPORTED is an answer too), MESSAGE IN read and the completion acknowledged (ACK_CC_CI) - with a timeout at every wait,
// a CANCEL for a command the PPM stays busy with, and a bound on the number of looks even if the clock stands still. A connector change shows
// in CCI (its connector number); the core reads that connector's status, acknowledges the change and reads the details again.
//
// Single-threaded: one owner calls it (UsbTypeCSystem's poll). What it reads goes into a UsbTypeCSnapshot the owner publishes.
#pragma once
#include <stdint.h>
#include "Hardware/UsbTypeC/IUcsiTransport.hpp"
#include "Hardware/UsbTypeC/IUsbTypeC.hpp"
#include "Hardware/UsbTypeC/UcsiProtocol.hpp"

namespace hardware {

class UcsiCore {
 public:
  struct Timing {
    uint32_t command_timeout_us = 2000000;   // a PPM behind an embedded controller can take a few hundred milliseconds for GET_PDOS
    uint32_t reset_timeout_us = 5000000;
    uint32_t cancel_timeout_us = 200000;
    uint32_t first_poll_us = 500;            // the first wait between looks at CCI; it doubles up to max_poll_us
    uint32_t max_poll_us = 10000;
    uint32_t reset_poll_us = 20000;
  };
  enum class Result : uint8_t { Ok, Timeout, Busy, Error, NotSupported, Transport, Malformed };

  UcsiCore(IUcsiTransport& transport, IUcsiClock& clock) : UcsiCore(transport, clock, Timing()) {}
  UcsiCore(IUcsiTransport& transport, IUcsiClock& clock, const Timing& timing) : transport_(transport), clock_(clock), timing_(timing) {}

  // PPM_RESET, the notifications, GET_CAPABILITY and every connector into `out` (its generation is left as it is).
  [[nodiscard]] Result initialize(UsbTypeCSnapshot* out);
  // Handles the connector changes CCI shows (and those noticed while other commands ran). `changed` says whether `out` changed.
  [[nodiscard]] Result service(UsbTypeCSnapshot* out, bool* changed);
  // Reads connector `index` (0-based) again: its status, and the details when the partner or the contract changed.
  [[nodiscard]] Result refresh_connector(UsbTypeCSnapshot* snapshot, uint32_t index, bool* changed);

  // One command. MESSAGE IN goes to `data` (cut to its size); `length` gets the length the PPM reported. `ack_connector` (a connector number, 0 for
  // none) acknowledges that connector's change with the command, as GET_CONNECTOR_STATUS does for the connector that changed.
  [[nodiscard]] Result execute(uint64_t control, ByteView data, uint32_t* length, uint32_t ack_connector = 0);
  [[nodiscard]] Result reset();

  uint16_t version() const { return version_; }
  uint16_t last_error() const { return last_error_; }
  uint32_t pending_changes() const { return pending_; }
  uint32_t commands() const { return commands_; }
  uint32_t timeouts() const { return timeouts_; }
  uint32_t cancels() const { return cancels_; }

  static const char* result_name(Result result);

 private:
  [[nodiscard]] Result wait_for(uint32_t bits, uint32_t timeout_us, uint32_t* cci);
  [[nodiscard]] Result acknowledge(bool command, uint32_t connector);
  void cancel();
  void note(uint32_t cci);
  [[nodiscard]] Result read_status(UsbTypeCSnapshot* snapshot, uint32_t index, uint32_t ack_connector, bool* changed);
  [[nodiscard]] Result read_details(UsbTypeCConnector* connector);
  [[nodiscard]] Result read_pdos(UsbTypeCConnector* connector);
  [[nodiscard]] Result read_alt_modes(UsbTypeCConnector* connector);
  static bool fatal(Result r) { return r == Result::Timeout || r == Result::Busy || r == Result::Transport; }

  IUcsiTransport& transport_;
  IUcsiClock& clock_;
  Timing timing_;
  uint16_t version_ = 0;
  uint32_t features_ = 0;
  uint16_t last_error_ = 0;
  uint32_t pending_ = 0;            // bit n: connector n + 1 reported a change
  uint32_t acknowledging_ = 0;      // the connector whose change is being acknowledged: not noted again meanwhile
  bool in_error_status_ = false;
  uint32_t commands_ = 0, timeouts_ = 0, cancels_ = 0;
};

// Whether two connectors read the same (what a reader would see differ).
bool usb_type_c_connector_equal(const UsbTypeCConnector& a, const UsbTypeCConnector& b);

}  // namespace hardware
