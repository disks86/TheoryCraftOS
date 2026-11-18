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

// The ChromeOS embedded controller's host interface: the ports, the host command numbers, the memory map and the protocol version 3 packet
// header, as the Chromium EC sources (include/ec_commands.h) and Linux (drivers/platform/chrome/cros_ec_lpc*.c) define them, plus the
// commands Framework added in its fork of the EC (framework_tool, the framework_laptop module).
//
// THE TABLE. Every number below was written down from memory of those sources, not copied from them; none has been checked against a real
// machine. Each carries a mark:
//   [sure]    the core every Chrome EC host driver uses (well remembered, but the tests run against a model written from the same memory)
//   [likely]  remembered with fair confidence (Linux's Framework quirks, less common commands)
//   [unsure]  Framework-specific numbers and layouts that should be checked against framework_tool / the EC source before relying on them
// When a number turns out wrong, this file is the only place to change it.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware::chromeec {

// --- Ports ----------------------------------------------------------------------------------------------------------------------------------
inline constexpr uint16_t kPortHostData = 0x200;          // [sure]   EC_LPC_ADDR_HOST_DATA: result byte of the last command
inline constexpr uint16_t kPortHostCommand = 0x204;       // [sure]   EC_LPC_ADDR_HOST_CMD: write the command, read the status
inline constexpr uint16_t kPortHostPacket = 0x800;        // [sure]   EC_LPC_ADDR_HOST_PACKET: the v3 request/response buffer
inline constexpr uint16_t kHostPacketSize = 0x100;        // [sure]   EC_LPC_HOST_PACKET_SIZE
inline constexpr uint16_t kPortMemmap = 0x900;            // [sure]   EC_LPC_ADDR_MEMMAP
inline constexpr uint16_t kPortFrameworkMemmap = 0xE00;   // [likely] cros_ec_lpc CROS_EC_LPC_QUIRK_REMAP_MEMORY base on Framework laptops
inline constexpr uint16_t kMemmapSize = 0xFF;             // [sure]   EC_MEMMAP_SIZE (255; the last byte of the 256 is not part of it)
// Microchip MEC "embedded memory interface": eight ports; the EC's 0x800..0x9FF window is reached through them, the EMI address being the
// port number less 0x800 (so the packet buffer is EMI 0x000 and the memory map EMI 0x100).
inline constexpr uint16_t kPortMecEmi = 0x800;            // [likely] MEC_EMI_BASE (cros_ec_lpc_mec_init(EC_HOST_CMD_REGION0, ...))
inline constexpr uint16_t kMecEmiAddressB0 = 2, kMecEmiAddressB1 = 3, kMecEmiData = 4;   // [sure]  offsets from the EMI base
inline constexpr uint16_t kMecWindowStart = 0x800, kMecWindowEnd = 0xA00;               // [likely] the part of the port space behind the EMI
inline constexpr uint8_t kMecAccessByte = 0, kMecAccessLongAutoIncrement = 3;          // [sure]   low bits of EMI address byte 0

// --- Status register (read from kPortHostCommand) --------------------------------------------------------------------------------------------
inline constexpr uint8_t kStatusToHost = 0x01;      // [sure]
inline constexpr uint8_t kStatusFromHost = 0x02;    // [sure]
inline constexpr uint8_t kStatusProcessing = 0x04;  // [sure]
inline constexpr uint8_t kStatusBusyMask = kStatusFromHost | kStatusProcessing;   // [sure] EC_LPC_STATUS_BUSY_MASK

inline constexpr uint8_t kCommandProtocol3 = 0xDA;  // [sure]   EC_COMMAND_PROTOCOL_3: "a v3 packet is in the buffer"

// --- Protocol version 3 packets -------------------------------------------------------------------------------------------------------------
// Request:  u8 struct_version (3), u8 checksum, u16 command, u8 command_version, u8 reserved, u16 data_length, then the parameters.
// Response: u8 struct_version (3), u8 checksum, u16 result, u16 data_length, u16 reserved, then the data.
// Both little-endian; the checksum byte makes the sum of every byte of header and data 0 (mod 256).                                     [sure]
inline constexpr uint8_t kPacketVersion = 3;
inline constexpr uint32_t kRequestHeaderBytes = 8, kResponseHeaderBytes = 8;
inline constexpr uint32_t kMaxParamBytes = kHostPacketSize - kRequestHeaderBytes;
inline constexpr uint32_t kMaxResponseDataBytes = kHostPacketSize - kResponseHeaderBytes;

// EC_RES_* (the result field of the response, also the byte at kPortHostData).                                                          [sure]
enum class EcResult : uint16_t {
  Success = 0, InvalidCommand = 1, Error = 2, InvalidParam = 3, AccessDenied = 4, InvalidResponse = 5, InvalidVersion = 6, InvalidChecksum = 7,
  InProgress = 8, Unavailable = 9, Timeout = 10, Overflow = 11, InvalidHeader = 12, RequestTruncated = 13, ResponseTooBig = 14, BusError = 15,
  Busy = 16,
};

// --- Host commands (EC_CMD_*) ---------------------------------------------------------------------------------------------------------------
namespace command {
inline constexpr uint16_t kHello = 0x0001;                  // [sure]   u32 in -> u32 in + 0x01020304
inline constexpr uint16_t kGetVersion = 0x0002;             // [sure]   char ro[32], rw[32], reserved[32], u32 current_image
inline constexpr uint16_t kGetBuildInfo = 0x0004;           // [sure]   text
inline constexpr uint16_t kGetChipInfo = 0x0005;            // [sure]   char vendor[32], name[32], revision[32]
inline constexpr uint16_t kGetCommandVersions = 0x0008;     // [sure]   v0: u8 command, v1: u16 command -> u32 version mask
inline constexpr uint16_t kGetProtocolInfo = 0x000B;        // [sure]   u32 versions, u16 max request, u16 max response, u32 flags
inline constexpr uint16_t kGetFeatures = 0x000D;            // [sure]   u32 flags[2]
inline constexpr uint16_t kPwmGetFanTargetRpm = 0x0020;     // [sure]   -> u32 rpm
inline constexpr uint16_t kPwmSetFanTargetRpm = 0x0021;     // [sure]   v0: u32 rpm (every fan), v1: u32 rpm, u8 fan
inline constexpr uint16_t kPwmGetKeyboardBacklight = 0x0022;// [sure]   -> u8 percent, u8 enabled
inline constexpr uint16_t kPwmSetKeyboardBacklight = 0x0023;// [sure]   u8 percent
inline constexpr uint16_t kPwmSetFanDuty = 0x0024;          // [sure]   v0: u32 percent (every fan), v1: u32 percent, u8 fan
inline constexpr uint16_t kLedControl = 0x0029;             // [sure]   v1: u8 led, u8 flags, u8 brightness[6] -> u8 range[6]
inline constexpr uint16_t kThermalAutoFanControl = 0x0052;  // [sure]   v0: (every fan), v1: u8 fan
inline constexpr uint16_t kTempSensorGetInfo = 0x0070;      // [sure]   u8 id -> char name[32], u8 type
inline constexpr uint16_t kChargeState = 0x00A0;            // [likely] v0 GET_STATE: u8 cmd(0) + 8 bytes -> u32 ac, voltage, current, input current, state of charge

// Framework's own commands (its EC fork; 0x3E00 and up).
inline constexpr uint16_t kFrameworkChargeLimitControl = 0x3E03;   // [likely] u8 modes, u8 max %, u8 min % -> u8 max %, u8 min %
inline constexpr uint16_t kFrameworkChassisIntrusion = 0x3E09;     // [unsure] u8 clear_magic, u8 clear -> u8 ever_opened, u8 coin_removed, u8 count, u8 vtr_count
inline constexpr uint16_t kFrameworkFpLedLevel = 0x3E0E;           // [unsure] u8 set_level, u8 get -> u8 percentage, u8 level
inline constexpr uint16_t kFrameworkPrivacySwitches = 0x3E14;      // [likely] (none) -> u8 microphone, u8 camera
inline constexpr uint16_t kFrameworkDeckState = 0x3E16;            // [unsure] Laptop 16 input deck: u8 mode (0 read only) -> u8 board_id[8], u8 state
inline constexpr uint16_t kFrameworkExpansionBayStatus = 0x3E1B;   // [unsure] Laptop 16 expansion bay: -> u8 state, u8 board_id_0, u8 board_id_1
}  // namespace command

// EC_CMD_LED_CONTROL                                                                                                                      [sure]
inline constexpr uint8_t kLedFlagQuery = 0x01, kLedFlagAuto = 0x02;
inline constexpr uint32_t kLedColorCount = 6;   // red, green, blue, yellow, white, amber
enum class EcLed : uint8_t { Battery = 0, Power = 1, Adapter = 2, Left = 3, Right = 4 };

// Framework charge limit modes                                                                                                         [likely]
inline constexpr uint8_t kChargeLimitDisable = 0x01, kChargeLimitSet = 0x02, kChargeLimitGet = 0x08, kChargeLimitOverride = 0x80;
// Framework fingerprint LED levels                                                                                                     [unsure]
enum class FpLedLevel : uint8_t { High = 0, Medium = 1, Low = 2 };
// Version 0 parameters: u8 set_level, u8 get (1: only report; the level byte is then ignored)                                          [unsure]
// Framework 16 expansion bay state bits                                                                                                [unsure]
inline constexpr uint8_t kBayEnabled = 0x01, kBayFault = 0x02, kBayDoorClosed = 0x04;

// EC_CMD_GET_FEATURES bit numbers (EC_FEATURE_*)                                                                                        [likely]
inline constexpr uint32_t kFeaturePwmFan = 2, kFeaturePwmKeyboard = 3, kFeatureLed = 5, kFeatureThermal = 10, kFeatureCharger = 16, kFeatureBattery = 17;

// --- The memory map (EC_MEMMAP_*, offsets into the 255-byte map) ---------------------------------------------------------------------------
namespace memmap {
inline constexpr uint8_t kTempSensor = 0x00;      // [sure]   16 bytes: sensors 0..15, kelvin - 200
inline constexpr uint8_t kFan = 0x10;             // [sure]   4 x u16 rpm
inline constexpr uint8_t kTempSensorB = 0x18;     // [sure]   8 bytes: sensors 16..23 (thermal version >= 2)
inline constexpr uint8_t kId = 0x20;              // [sure]   'E' 'C'
inline constexpr uint8_t kIdVersion = 0x22;       // [sure]
inline constexpr uint8_t kThermalVersion = 0x23;  // [sure]
inline constexpr uint8_t kBatteryVersion = 0x24;  // [sure]
inline constexpr uint8_t kSwitchesVersion = 0x25; // [sure]
inline constexpr uint8_t kHostCommandFlags = 0x27;// [sure]
inline constexpr uint8_t kSwitches = 0x30;        // [sure]
inline constexpr uint8_t kBatteryVoltage = 0x40;  // [sure]   u32 mV
inline constexpr uint8_t kBatteryRate = 0x44;     // [sure]   u32 mA
inline constexpr uint8_t kBatteryCapacity = 0x48; // [sure]   u32 mAh remaining
inline constexpr uint8_t kBatteryFlags = 0x4C;    // [sure]   u8
inline constexpr uint8_t kBatteryDesignCapacity = 0x50;   // [sure] u32 mAh
inline constexpr uint8_t kBatteryDesignVoltage = 0x54;    // [sure] u32 mV
inline constexpr uint8_t kBatteryLastFullCharge = 0x58;   // [sure] u32 mAh
inline constexpr uint8_t kBatteryCycleCount = 0x5C;       // [sure] u32
inline constexpr uint8_t kBatteryManufacturer = 0x60;     // [sure] 8 chars
inline constexpr uint8_t kBatteryModel = 0x68;            // [sure] 8 chars

inline constexpr uint32_t kTempSensorEntries = 16, kTempSensorEntriesB = 8, kFanEntries = 4;
inline constexpr uint8_t kTempNotPresent = 0xFF, kTempError = 0xFE, kTempNotPowered = 0xFD, kTempNotCalibrated = 0xFC;   // [sure]
inline constexpr int32_t kTempOffsetKelvin = 200;                                                                       // [sure]
inline constexpr uint16_t kFanNotPresent = 0xFFFF, kFanStalled = 0xFFFE;                                                // [sure]
inline constexpr uint8_t kHostCommandFlagVersion3 = 0x02;                                                               // [sure]
inline constexpr uint8_t kBatteryAcPresent = 0x01, kBatteryPresent = 0x02, kBatteryDischarging = 0x04, kBatteryCharging = 0x08, kBatteryCritical = 0x10;  // [sure]
inline constexpr uint8_t kSwitchLidOpen = 0x01, kSwitchPowerButton = 0x02, kSwitchWriteProtectDisabled = 0x04;          // [sure]
}  // namespace memmap

// --- The ACPI ids of the EC device ---------------------------------------------------------------------------------------------------------
// GOOG0004: Chromebooks (cros_ec_lpc's ACPI match). FRMW0004 / FRMW0005: Framework laptops (which board uses which is not known here).   [likely]
inline constexpr const char* kAcpiIds[] = {"GOOG0004", "FRMW0004", "FRMW0005"};

// --- Packet helpers --------------------------------------------------------------------------------------------------------------------------

// The byte that makes the sum of `bytes` plus itself 0.
inline uint8_t checksum_complement(ConstByteView bytes) {
  uint8_t sum = 0;
  for (uint8_t b : bytes) sum = static_cast<uint8_t>(sum + b);
  return static_cast<uint8_t>(-sum);
}
inline uint8_t byte_sum(ConstByteView bytes) {
  uint8_t sum = 0;
  for (uint8_t b : bytes) sum = static_cast<uint8_t>(sum + b);
  return sum;
}

// Builds a v3 request into `out` (header, then `params`). Returns its length, 0 when it does not fit.
[[nodiscard]] uint32_t build_request(uint16_t command, uint8_t version, ConstByteView params, ByteView out);

struct ResponseHeader {
  uint8_t struct_version = 0;
  uint16_t result = 0;
  uint16_t data_length = 0;
};
// Reads the header of a response; false when `bytes` is shorter than a header.
[[nodiscard]] bool parse_response_header(ConstByteView bytes, ResponseHeader* out);

}  // namespace hardware::chromeec
