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

// The ChromeOS EC host command layer: one transaction at a time over a transport (LPC or MEC), the standard commands (hello, version,
// features, command versions, fans, keyboard backlight, LEDs, charge state) and the memory map (temperatures, fan speeds, battery).
//
// Thread-safe: every transaction (and every memory map read, which on a MEC moves the shared EMI address) holds a mutex, so two threads'
// commands never interleave on the ports. The firmware's ACPI code may issue host commands too (Framework's AMD boards guard them with the
// AML mutex ECMT); an IEcFirmwareLock, when set, is held around each transaction as well, after the mutex. A transaction can wait up to the
// transport's poll budget, which is why this is a sleeping mutex and not a spin lock: do not call it with a spin lock held.
#pragma once
#include <stdint.h>
#include "Hardware/EmbeddedController/ChromeEc/ChromeEcTransport.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

// Something else that talks to the EC and must be kept out while a transaction runs (the ACPI interpreter).
class IEcFirmwareLock {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;

 protected:
  constexpr IEcFirmwareLock() = default;
  ~IEcFirmwareLock() = default;
};

// The outcome of one host command.
struct EcReply {
  EcTransferStatus transfer = EcTransferStatus::Timeout;
  uint16_t result = 0;       // chromeec::EcResult (meaningful when transfer is Ok)
  uint32_t data_bytes = 0;
  bool ok() const { return transfer == EcTransferStatus::Ok && result == static_cast<uint16_t>(chromeec::EcResult::Success); }
};

enum class EcImage : uint8_t { Unknown = 0, ReadOnly = 1, ReadWrite = 2 };

struct EcVersionInfo {
  FixedString<33> read_only;
  FixedString<33> read_write;
  EcImage current = EcImage::Unknown;
};

struct EcChipInfo {
  FixedString<33> vendor, name, revision;
};

struct EcProtocolInfo {
  uint32_t versions = 0;            // bit n: protocol version n
  uint16_t max_request_bytes = 0;   // whole packet, header included
  uint16_t max_response_bytes = 0;
  uint32_t flags = 0;
};

struct EcKeyboardBacklight {
  uint8_t percent = 0;
  bool enabled = false;
};

// A memory map temperature: kelvin less 200, or a code for why there is none.
struct EcTemperatureReading {
  bool present = false;     // a sensor is wired at this index
  bool valid = false;       // and it gave a temperature
  int32_t millicelsius = 0;
};

struct EcBatteryInfo {
  bool present = false, ac_present = false, charging = false, discharging = false, critical = false;
  uint32_t voltage_mv = 0, rate_ma = 0, remaining_mah = 0, full_charge_mah = 0, design_mah = 0, design_mv = 0, cycle_count = 0;
  FixedString<9> manufacturer, model;
  uint8_t percent() const;   // remaining / last full charge, 0..100 (0 without a full-charge figure)
};

struct EcChargeState {
  bool ac = false;
  uint32_t charge_voltage_mv = 0, charge_current_ma = 0, input_current_ma = 0, state_of_charge = 0;
};

struct EcLedRange {
  uint8_t max[chromeec::kLedColorCount] = {};   // brightest level per colour (red, green, blue, yellow, white, amber); 0: no such colour
};

class ChromeEc {
 public:
  static constexpr uint32_t kMaxFans = chromeec::memmap::kFanEntries;
  static constexpr uint32_t kMaxTemperatureSensors = chromeec::memmap::kTempSensorEntries + chromeec::memmap::kTempSensorEntriesB;

  explicit ChromeEc(ChromeEcTransport& transport) : transport_(transport) {}
  ChromeEc(const ChromeEc&) = delete;
  ChromeEc& operator=(const ChromeEc&) = delete;

  // Checks the memory map ("EC", protocol version 3 advertised), says hello and reads what the EC is (version, chip, protocol, features).
  // False, and nothing else is sent, when any of it is wrong; why: problem().
  [[nodiscard]] bool start();
  bool started() const { return started_; }
  const char* problem() const { return problem_; }
  EcTransportKind transport_kind() const { return transport_.kind(); }

  void set_firmware_lock(IEcFirmwareLock* lock);

  // One host command. `response` receives the data (its size is the most the caller accepts). Parameters beyond what the EC accepts (its
  // protocol info) are refused before anything is sent.
  [[nodiscard]] EcReply command(uint16_t command, uint8_t version, ConstByteView params, ByteView response);
  // Bytes of the memory map.
  [[nodiscard]] bool read_memmap(uint32_t offset, ByteView out);

  // Cached by start().
  const EcVersionInfo& version() const { return version_; }
  const EcChipInfo& chip() const { return chip_; }
  const EcProtocolInfo& protocol() const { return protocol_; }
  uint64_t features() const { return features_; }
  bool has_feature(uint32_t bit) const { return bit < 64 && (features_ >> bit) & 1; }

  // --- Commands.
  [[nodiscard]] Optional<uint32_t> hello(uint32_t value);
  [[nodiscard]] Optional<EcVersionInfo> read_version();
  [[nodiscard]] Optional<EcChipInfo> read_chip_info();
  [[nodiscard]] Optional<EcProtocolInfo> read_protocol_info();
  [[nodiscard]] Optional<uint64_t> read_features();
  // The versions the EC supports of `command` (bit n: version n); empty when it does not know the command.
  [[nodiscard]] Optional<uint32_t> command_versions(uint16_t command);

  [[nodiscard]] Optional<uint32_t> fan_target_rpm();
  // `fan` empty: every fan (version 0). A fan index needs version 1, which is used when the EC has it.
  [[nodiscard]] bool set_fan_target_rpm(Optional<uint8_t> fan, uint32_t rpm);
  [[nodiscard]] bool set_fan_duty(Optional<uint8_t> fan, uint8_t percent);
  [[nodiscard]] bool set_fan_automatic(Optional<uint8_t> fan);

  [[nodiscard]] Optional<EcKeyboardBacklight> keyboard_backlight();
  [[nodiscard]] bool set_keyboard_backlight(uint8_t percent);

  [[nodiscard]] Optional<EcLedRange> led_range(chromeec::EcLed led);
  [[nodiscard]] bool set_led(chromeec::EcLed led, const uint8_t (&brightness)[chromeec::kLedColorCount]);
  [[nodiscard]] bool set_led_automatic(chromeec::EcLed led);

  [[nodiscard]] Optional<EcChargeState> charge_state();
  [[nodiscard]] bool temperature_sensor_name(uint8_t sensor, FixedString<33>* out);

  // --- The memory map.
  [[nodiscard]] Optional<EcTemperatureReading> temperature(uint32_t sensor);
  // RPM; 0 for a stalled fan; empty for no fan at that index.
  [[nodiscard]] Optional<uint32_t> fan_rpm(uint32_t fan);
  [[nodiscard]] Optional<EcBatteryInfo> battery();
  // EC_MEMMAP_SWITCHES (lid open, power button, write protect).
  [[nodiscard]] Optional<uint8_t> switches();

 private:
  struct Lock;
  EcReply command_locked(uint16_t command, uint8_t version, ConstByteView params, ByteView response);
  // Version 1 when the EC lists it for `command`, else 0. Cached per command.
  uint8_t best_version(uint16_t command, uint8_t highest);

  ChromeEcTransport& transport_;
  kernel::Mutex mutex_;
  IEcFirmwareLock* firmware_lock_ = nullptr;
  bool started_ = false;
  const char* problem_ = "not started";
  uint8_t thermal_version_ = 0;
  EcVersionInfo version_;
  EcChipInfo chip_;
  EcProtocolInfo protocol_;
  uint64_t features_ = 0;
  uint32_t max_params_ = chromeec::kMaxParamBytes, max_response_ = chromeec::kMaxResponseDataBytes;
  struct VersionCache {
    uint16_t command = 0;
    uint32_t mask = 0;
    bool used = false;
  };
  VersionCache versions_[8];   // (guarded by mutex_)
};

// --- Detection -----------------------------------------------------------------------------------------------------------------------------

struct EcDetection {
  bool found = false;
  EcTransportKind kind = EcTransportKind::Lpc;
  uint16_t lpc_memmap_base = 0;       // Lpc: where the map is
  uint16_t mec_plain_memmap_base = 0; // Mec: 0 = the map through the EMI
  const char* reason = "";
};

// Looks for a Chrome EC without writing to a port it has not identified. `acpi_id_present`: the ACPI tables name the EC device (GOOG0004,
// FRMW0004 or FRMW0005). Without that, only reads happen first: the "EC" signature at 0x920 / 0xE20. Only once one of the two identified the
// controller does it touch the MEC EMI address registers (0x802/0x803) to look for the map behind them. Says nothing about whether host
// commands work: ChromeEc::start() checks that.
[[nodiscard]] EcDetection detect_chrome_ec(IChromeEcPorts& ports, bool acpi_id_present);

}  // namespace hardware
