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

// What a serial line is, whatever it hangs off (a 16550 UART, a USB CDC-ACM device): its framing, its modem lines and its counters.
#pragma once
#include <stdint.h>

namespace hardware {

enum class SerialParity : uint8_t { None, Odd, Even, Mark, Space };
enum class SerialStopBits : uint8_t { One, Two };   // (two means 1.5 for five data bits, as on a UART)
enum class SerialFlowControl : uint8_t { None, RtsCts };

struct SerialConfig {
  uint32_t baud = 115200;
  uint8_t data_bits = 8;   // 5 to 8
  SerialParity parity = SerialParity::None;
  SerialStopBits stop_bits = SerialStopBits::One;
  SerialFlowControl flow_control = SerialFlowControl::None;

  bool valid() const { return baud != 0 && data_bits >= 5 && data_bits <= 8; }
};

// The modem lines as they are now, one bit each.
struct SerialLines {
  static constexpr uint8_t kCts = 1, kDsr = 2, kRing = 4, kCarrier = 8;   // inputs (from the other end)
  static constexpr uint8_t kRts = 16, kDtr = 32;                          // outputs (what this end drives)
};

enum class SerialState : uint8_t {
  Closed,   // not configured; no bytes move
  Open,
  Error,    // the hardware stopped answering
};

struct SerialStatistics {
  uint64_t bytes_received = 0;
  uint64_t bytes_sent = 0;
  uint64_t overruns = 0;       // received bytes lost, by the hardware or because the receive queue was full
  uint64_t parity_errors = 0;
  uint64_t framing_errors = 0;
  uint64_t breaks = 0;
};

}  // namespace hardware
