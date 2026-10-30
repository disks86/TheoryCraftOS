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

// Bluetooth host controller interface (Core spec vol 4 part E): the numbers the host and the controller speak in. Packets carry no transport framing here (that is the
// transport's: USB sends commands on the control endpoint, events on an interrupt endpoint and data on bulk ones).
#pragma once
#include <stdint.h>

namespace hardware {
namespace hci {

// Opcodes: OGF in the top six bits, OCF in the low ten.
constexpr uint16_t opcode(uint16_t ogf, uint16_t ocf) { return static_cast<uint16_t>((ogf << 10) | ocf); }
constexpr uint16_t kOgfLink = 0x01, kOgfPolicy = 0x02, kOgfController = 0x03, kOgfInfo = 0x04, kOgfStatus = 0x05, kOgfLe = 0x08, kOgfVendor = 0x3F;

constexpr uint16_t kOpInquiry = opcode(kOgfLink, 0x0001), kOpInquiryCancel = opcode(kOgfLink, 0x0002), kOpCreateConnection = opcode(kOgfLink, 0x0005),
                   kOpDisconnect = opcode(kOgfLink, 0x0006), kOpAcceptConnection = opcode(kOgfLink, 0x0009), kOpRejectConnection = opcode(kOgfLink, 0x000A),
                   kOpLinkKeyReply = opcode(kOgfLink, 0x000B), kOpLinkKeyNegativeReply = opcode(kOgfLink, 0x000C), kOpPinCodeReply = opcode(kOgfLink, 0x000D),
                   kOpPinCodeNegativeReply = opcode(kOgfLink, 0x000E), kOpAuthenticationRequested = opcode(kOgfLink, 0x0011), kOpSetConnectionEncryption = opcode(kOgfLink, 0x0013),
                   kOpRemoteNameRequest = opcode(kOgfLink, 0x0019), kOpReadRemoteFeatures = opcode(kOgfLink, 0x001B), kOpIoCapabilityReply = opcode(kOgfLink, 0x002B),
                   kOpUserConfirmationReply = opcode(kOgfLink, 0x002C), kOpUserConfirmationNegativeReply = opcode(kOgfLink, 0x002D),
                   kOpSetEventMask = opcode(kOgfController, 0x0001), kOpReset = opcode(kOgfController, 0x0003), kOpWriteLocalName = opcode(kOgfController, 0x0013),
                   kOpReadLocalName = opcode(kOgfController, 0x0014), kOpWriteScanEnable = opcode(kOgfController, 0x001A), kOpWriteClassOfDevice = opcode(kOgfController, 0x0024),
                   kOpWriteInquiryMode = opcode(kOgfController, 0x0045), kOpWriteSimplePairingMode = opcode(kOgfController, 0x0056), kOpWriteLeHostSupported = opcode(kOgfController, 0x006D),
                   kOpWriteExtendedInquiryResponse = opcode(kOgfController, 0x0052),
                   kOpReadLocalVersion = opcode(kOgfInfo, 0x0001), kOpReadLocalCommands = opcode(kOgfInfo, 0x0002), kOpReadLocalFeatures = opcode(kOgfInfo, 0x0003),
                   kOpReadBufferSize = opcode(kOgfInfo, 0x0005), kOpReadBdAddr = opcode(kOgfInfo, 0x0009),
                   kOpLeSetEventMask = opcode(kOgfLe, 0x0001), kOpLeReadBufferSize = opcode(kOgfLe, 0x0002), kOpLeReadLocalFeatures = opcode(kOgfLe, 0x0003),
                   kOpLeSetRandomAddress = opcode(kOgfLe, 0x0005), kOpLeSetAdvertisingParameters = opcode(kOgfLe, 0x0006), kOpLeSetAdvertisingData = opcode(kOgfLe, 0x0008),
                   kOpLeSetScanResponseData = opcode(kOgfLe, 0x0009), kOpLeSetAdvertisingEnable = opcode(kOgfLe, 0x000A), kOpLeSetScanParameters = opcode(kOgfLe, 0x000B),
                   kOpLeSetScanEnable = opcode(kOgfLe, 0x000C), kOpLeCreateConnection = opcode(kOgfLe, 0x000D), kOpLeCreateConnectionCancel = opcode(kOgfLe, 0x000E),
                   kOpLeClearFilterAcceptList = opcode(kOgfLe, 0x0010), kOpLeConnectionUpdate = opcode(kOgfLe, 0x0013), kOpLeStartEncryption = opcode(kOgfLe, 0x0019),
                   kOpLeLongTermKeyReply = opcode(kOgfLe, 0x001A), kOpLeLongTermKeyNegativeReply = opcode(kOgfLe, 0x001B), kOpLeRand = opcode(kOgfLe, 0x0018),
                   kOpLeEncrypt = opcode(kOgfLe, 0x0017), kOpLeReadFilterAcceptListSize = opcode(kOgfLe, 0x000F), kOpLeReadSupportedStates = opcode(kOgfLe, 0x001C),
                   kOpLeSetDefaultPhy = opcode(kOgfLe, 0x0031);

// Event codes.
constexpr uint8_t kEvInquiryComplete = 0x01, kEvInquiryResult = 0x02, kEvConnectionComplete = 0x03, kEvConnectionRequest = 0x04, kEvDisconnectionComplete = 0x05,
                  kEvAuthenticationComplete = 0x06, kEvRemoteNameRequestComplete = 0x07, kEvEncryptionChange = 0x08, kEvReadRemoteFeaturesComplete = 0x0B,
                  kEvCommandComplete = 0x0E, kEvCommandStatus = 0x0F, kEvHardwareError = 0x10, kEvNumberOfCompletedPackets = 0x13, kEvPinCodeRequest = 0x16,
                  kEvLinkKeyRequest = 0x17, kEvLinkKeyNotification = 0x18, kEvInquiryResultWithRssi = 0x22, kEvExtendedInquiryResult = 0x2F, kEvIoCapabilityRequest = 0x31,
                  kEvIoCapabilityResponse = 0x32, kEvUserConfirmationRequest = 0x33, kEvSimplePairingComplete = 0x36, kEvLeMeta = 0x3E, kEvVendor = 0xFF;
constexpr uint8_t kLeConnectionComplete = 0x01, kLeAdvertisingReport = 0x02, kLeConnectionUpdateComplete = 0x03, kLeLongTermKeyRequest = 0x05, kLeEnhancedConnectionComplete = 0x0A;

// Status codes that matter to the host.
constexpr uint8_t kStatusSuccess = 0x00, kStatusUnknownCommand = 0x01, kStatusUnknownConnection = 0x02, kStatusHardwareFailure = 0x03, kStatusPageTimeout = 0x04,
                  kStatusAuthenticationFailure = 0x05, kStatusPinOrKeyMissing = 0x06, kStatusMemoryFull = 0x07, kStatusConnectionTimeout = 0x08, kStatusCommandDisallowed = 0x0C,
                  kStatusInvalidParameters = 0x12, kStatusRemoteUserTerminated = 0x13, kStatusLocalHostTerminated = 0x16;

// A device address as the controller sends it: least significant byte first.
struct BdAddr {
  uint8_t b[6] = {};
  bool operator==(const BdAddr& o) const {
    for (int i = 0; i < 6; ++i)
      if (b[i] != o.b[i]) return false;
    return true;
  }
  bool operator!=(const BdAddr& o) const { return !(*this == o); }
  bool is_zero() const {
    for (int i = 0; i < 6; ++i)
      if (b[i]) return false;
    return true;
  }
};

constexpr uint8_t kAddrPublic = 0, kAddrRandom = 1;
constexpr uint32_t kMaxCommandParams = 255, kMaxEventParams = 255, kMaxAclData = 1021;

}  // namespace hci
}  // namespace hardware
