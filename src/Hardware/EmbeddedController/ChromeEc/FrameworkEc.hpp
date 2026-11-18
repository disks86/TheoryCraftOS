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

// What Framework added to its fork of the Chrome EC (Framework Laptop 13 Intel and AMD, Laptop 16, Laptop 12): the battery charge limit, the
// camera and microphone privacy switches, the chassis intrusion counters, the fingerprint reader's LED, and on the Laptop 16 the input deck
// and the expansion bay. Each is one host command (numbers and layouts in ChromeEcProtocol.hpp, several marked [unsure]); a board without
// the feature answers INVALID_COMMAND and the call returns empty. Read only, except the charge limit and the LED level.
//
// Thread-safe (ChromeEc serializes the commands).
#pragma once
#include <stdint.h>
#include "Hardware/EmbeddedController/ChromeEc/ChromeEc.hpp"

namespace hardware {

struct FrameworkChargeLimit {
  uint8_t max_percent = 100;   // the EC stops charging here (100: no limit)
  uint8_t min_percent = 0;
};

// What the privacy switches let through. (framework_tool shows 1 as "connected"; 0 means the switch has cut the device off.)
struct FrameworkPrivacySwitches {
  bool microphone_connected = false;
  bool camera_connected = false;
};

struct FrameworkChassisIntrusion {
  bool ever_opened = false;
  bool coin_cell_ever_removed = false;
  uint8_t open_count = 0;           // with the machine on mains or battery
  uint8_t open_count_powered_off = 0;
};

struct FrameworkFingerprintLed {
  uint8_t percent = 0;
  uint8_t level = 0;                // chromeec::FpLedLevel
};

// Laptop 16 input deck (the keyboard, the touchpad and the spacers / LED matrix modules above them).
struct FrameworkInputDeck {
  uint8_t state = 0;                // as the EC reports it (meaning not decoded here: see docs/FrameworkLaptop.md)
  uint8_t board_ids[8] = {};        // the id of the module in each of the deck's positions
};

// Laptop 16 expansion bay (the graphics module or the fan-only shell).
struct FrameworkExpansionBay {
  bool enabled = false, fault = false, door_closed = false;
  uint8_t board_id_0 = 0, board_id_1 = 0;
};

class FrameworkEc {
 public:
  static constexpr uint8_t kMinChargeLimit = 20;   // (our own floor: below this a laptop on mains would sit nearly empty)

  explicit FrameworkEc(ChromeEc& ec) : ec_(ec) {}

  [[nodiscard]] Optional<FrameworkChargeLimit> charge_limit();
  // Stops charging at `max_percent` (kMinChargeLimit..100; 100 removes the limit). False outside that range or when the EC refused.
  [[nodiscard]] bool set_charge_limit(uint8_t max_percent);

  [[nodiscard]] Optional<FrameworkPrivacySwitches> privacy_switches();
  [[nodiscard]] Optional<FrameworkChassisIntrusion> chassis_intrusion();
  [[nodiscard]] Optional<FrameworkFingerprintLed> fingerprint_led();
  [[nodiscard]] bool set_fingerprint_led(chromeec::FpLedLevel level);
  [[nodiscard]] Optional<FrameworkInputDeck> input_deck();
  [[nodiscard]] Optional<FrameworkExpansionBay> expansion_bay();

 private:
  ChromeEc& ec_;
};

}  // namespace hardware
