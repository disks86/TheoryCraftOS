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

// The wire format of UCSI (USB Type-C Connector System Software Interface, Intel, revisions 1.2, 2.0 and 2.1): the data structure the OS and the
// platform policy manager (PPM) share - VERSION, CCI (command status and connector change indication), CONTROL (the command), MESSAGE IN (what
// the PPM answers) and MESSAGE OUT - the command codes and the layout of each answer. The decoders take MESSAGE IN as untrusted bytes: a field
// that lies beyond the data is read as zero, a structure too short for its required fields is refused, and values outside the encoding become
// "unknown". Field positions follow the spec's bit offsets (and Linux drivers/usb/typec/ucsi/ucsi.h).
#pragma once
#include <stdint.h>
#include "Hardware/UsbTypeC/IUsbTypeC.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware::ucsi {

// The shared data structure (byte offsets). From UCSI 2.0 on MESSAGE IN holds 256 bytes and MESSAGE OUT follows it.
constexpr uint32_t kOffsetVersion = 0x00, kOffsetCci = 0x04, kOffsetControl = 0x08, kOffsetMessageIn = 0x10;
constexpr uint32_t kOffsetMessageOutV1 = 0x20, kOffsetMessageOutV2 = 0x110;
constexpr uint32_t kMessageBytesV1 = 16, kMessageBytesV2 = 256;
constexpr uint32_t kMailboxBytesV1 = 0x30, kMailboxBytesV2 = 0x210;
constexpr uint16_t kVersion1_2 = 0x0120, kVersion2_0 = 0x0200, kVersion2_1 = 0x0210;

// CCI.
constexpr uint32_t kCciEndOfMessage = 1u << 0;     // (2.x; reserved before)
constexpr uint32_t kCciNotSupported = 1u << 25, kCciCancelComplete = 1u << 26, kCciResetComplete = 1u << 27, kCciBusy = 1u << 28,
                   kCciAckComplete = 1u << 29, kCciError = 1u << 30, kCciCommandComplete = 1u << 31;
constexpr uint32_t cci_connector(uint32_t cci) { return (cci >> 1) & 0x7F; }   // a connector that changed, 0 for none
constexpr uint32_t cci_length(uint32_t cci) { return (cci >> 8) & 0xFF; }

enum Command : uint8_t {
  kPpmReset = 0x01, kCancel = 0x02, kConnectorReset = 0x03, kAckCcCi = 0x04, kSetNotificationEnable = 0x05, kGetCapability = 0x06,
  kGetConnectorCapability = 0x07, kSetUom = 0x08, kSetUor = 0x09, kSetPdm = 0x0A, kSetPdr = 0x0B, kGetAlternateModes = 0x0C, kGetCamSupported = 0x0D,
  kGetCurrentCam = 0x0E, kSetNewCam = 0x0F, kGetPdos = 0x10, kGetCableProperty = 0x11, kGetConnectorStatus = 0x12, kGetErrorStatus = 0x13,
};
constexpr uint8_t control_command(uint64_t control) { return static_cast<uint8_t>(control); }

// ACK_CC_CI.
constexpr uint64_t kAckConnectorChange = 1ull << 16, kAckCommandComplete = 1ull << 17;
// SET_NOTIFICATION_ENABLE (the mask sits at bit 16 of CONTROL).
constexpr uint64_t kNotifyCommandComplete = 1ull << 16, kNotifyExternalSupply = 1ull << 17, kNotifyPowerMode = 1ull << 18, kNotifyAttention = 1ull << 19,
                   kNotifyCapabilityChange = 1ull << 21, kNotifyPowerLevel = 1ull << 22, kNotifyPdReset = 1ull << 23, kNotifyCamChange = 1ull << 24,
                   kNotifyBatteryCharging = 1ull << 25, kNotifyPartnerChange = 1ull << 27, kNotifyPowerDirection = 1ull << 28,
                   kNotifyConnectChange = 1ull << 30, kNotifyError = 1ull << 31;
constexpr uint64_t kNotifyAll = 0xDBE70000ull;      // what Linux enables once the connectors are known

// GET_CAPABILITY bmOptionalFeatures.
constexpr uint32_t kFeatureSetUom = 1u << 0, kFeatureSetPdm = 1u << 1, kFeatureAltModeDetails = 1u << 2, kFeatureAltModeOverride = 1u << 3,
                   kFeaturePdoDetails = 1u << 4, kFeatureCableDetails = 1u << 5, kFeatureExternalSupply = 1u << 6, kFeaturePdReset = 1u << 7;

// GET_CONNECTOR_STATUS connector status change bits.
constexpr uint16_t kChangeExternalSupply = 1u << 1, kChangePowerMode = 1u << 2, kChangeCapability = 1u << 5, kChangePowerLevel = 1u << 6,
                   kChangePdReset = 1u << 7, kChangeCam = 1u << 8, kChangeBatteryCharging = 1u << 9, kChangePartner = 1u << 11,
                   kChangePowerDirection = 1u << 12, kChangeConnect = 1u << 14, kChangeError = 1u << 15;

// GET_ERROR_STATUS error information bits.
constexpr uint16_t kErrorUnrecognizedCommand = 1u << 0, kErrorNonExistentConnector = 1u << 1, kErrorInvalidParameter = 1u << 2,
                   kErrorIncompatiblePartner = 1u << 3, kErrorCcCommunication = 1u << 4, kErrorDeadBattery = 1u << 5,
                   kErrorContractNegotiation = 1u << 6, kErrorOvercurrent = 1u << 7, kErrorUndefined = 1u << 8, kErrorPartnerRejectedSwap = 1u << 9,
                   kErrorHardReset = 1u << 10, kErrorPpmPolicyConflict = 1u << 11, kErrorSwapRejected = 1u << 12;

constexpr uint16_t kSvidDisplayPort = 0xFF01, kSvidThunderbolt = 0x8087;

// --- CONTROL words of the commands.
constexpr uint64_t control_word(uint8_t command, uint64_t specific = 0) { return uint64_t{command} | specific; }
constexpr uint64_t connector_field(uint32_t number) { return uint64_t{number & 0x7F} << 16; }
uint64_t control_get_pdos(uint32_t connector, bool partner, uint32_t offset, uint32_t count, bool source);
enum class AltModeRecipient : uint8_t { Connector = 0, Sop = 1, SopPrime = 2, SopDoublePrime = 3 };
uint64_t control_get_alternate_modes(AltModeRecipient recipient, uint32_t connector, uint32_t offset, uint32_t count);

// --- GET_CAPABILITY.
struct Capability {
  uint32_t attributes = 0;
  uint8_t connectors = 0;
  uint32_t features = 0;              // 8 bits in 1.x, 24 in 2.x
  uint8_t alt_modes = 0;
  uint16_t bc_version = 0, pd_version = 0, typec_version = 0;
};
[[nodiscard]] bool decode_capability(ConstByteView data, uint16_t version, Capability* out);

[[nodiscard]] bool decode_connector_capability(ConstByteView data, UsbTypeCConnectorCaps* out);

// --- GET_CONNECTOR_STATUS: 9 bytes before 1.2, 11 in 1.2, 12 in 2.0, 19 in 2.1. Fills the status fields of `out` (not the PDOs, modes or cable);
// `change` gets the change bits.
[[nodiscard]] bool decode_connector_status(ConstByteView data, uint16_t version, UsbTypeCConnector* out, uint16_t* change);

// --- GET_PDOS: whole objects only. Returns how many were decoded into `out`.
uint32_t decode_pdos(ConstByteView data, ArrayView<UsbPdo> out);
UsbPdo decode_pdo(uint32_t raw);
// The contract from a request data object and the source's objects (which may be empty: then only the current is known).
UsbPdContract decode_contract(uint32_t rdo, ArrayView<const UsbPdo> source_pdos);

// --- GET_ALTERNATE_MODES: up to two {SVID, MID} entries; an entry with SVID 0 ends the list. Returns how many were decoded.
uint32_t decode_alternate_modes(ConstByteView data, ArrayView<UsbAltMode> out);

// --- GET_CABLE_PROPERTY.
[[nodiscard]] bool decode_cable_property(ConstByteView data, UsbCable* out);

// --- GET_ERROR_STATUS.
[[nodiscard]] bool decode_error_status(ConstByteView data, uint16_t* error);

// A little-endian bit field of `data` (bit 0 = bit 0 of byte 0); bits past the end of the data read as zero. `width` at most 32.
uint32_t bit_field(ConstByteView data, uint32_t offset, uint32_t width);

}  // namespace hardware::ucsi
