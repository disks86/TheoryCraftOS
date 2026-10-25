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

// STUN (RFC 8489) with the attributes ICE uses (RFC 8445): binding requests, answers, errors and indications; USERNAME, PRIORITY, USE-CANDIDATE, ICE-CONTROLLING /
// ICE-CONTROLLED, XOR-MAPPED-ADDRESS, ERROR-CODE, SOFTWARE; the short-term credential check (MESSAGE-INTEGRITY, HMAC-SHA1) and FINGERPRINT.
#pragma once
#include "Network/NetworkTypes.hpp"
#include "Rtc/RtcBytes.hpp"
#include "Utility/String.hpp"

namespace rtc {

constexpr uint16_t kStunBindingRequest = 0x0001, kStunBindingSuccess = 0x0101, kStunBindingError = 0x0111, kStunBindingIndication = 0x0011;
constexpr uint32_t kStunCookie = 0x2112A442, kStunHeader = 20;
constexpr uint16_t kStunUsername = 0x0006, kStunIntegrity = 0x0008, kStunErrorCode = 0x0009, kStunXorMapped = 0x0020, kStunPriority = 0x0024, kStunUseCandidate = 0x0025,
                   kStunSoftware = 0x8022, kStunFingerprint = 0x8028, kStunIceControlled = 0x8029, kStunIceControlling = 0x802A, kStunRealm = 0x0014, kStunNonce = 0x0015,
                   kStunXorRelayed = 0x0016, kStunLifetime = 0x000D, kStunRequestedTransport = 0x0019, kStunData = 0x0013, kStunXorPeer = 0x0012, kStunChannelNumber = 0x000C;

// Does the datagram look like a STUN message (first two bits 0, the cookie, a length that fits)? Demultiplexing of RFC 7983 starts with this.
bool stun_looks_like(ConstByteView datagram);

struct StunMessage {
  explicit StunMessage(allocators::IAllocator& a) : username(a), software(a) {}
  uint16_t type = 0;
  uint8_t transaction[12] = {};
  bool has_mapped = false, use_candidate = false, has_priority = false, has_controlling = false, has_controlled = false, has_error = false, has_integrity = false;
  bool fingerprint_ok = false;   // there was a FINGERPRINT and it is right
  network::Ipv4Address mapped{};
  uint16_t mapped_port = 0, error_code = 0;
  uint32_t priority = 0;
  uint64_t tie_breaker = 0;
  String username, software;
  uint64_t integrity_offset = 0;   // where MESSAGE-INTEGRITY starts in the datagram (for stun_check_integrity)
};

// Parses a datagram. False for anything malformed (a cut attribute, a length that does not match, a bad cookie). Unknown attributes are skipped.
bool stun_parse(ConstByteView datagram, StunMessage* out);
// Does the MESSAGE-INTEGRITY of the datagram match the key (the short-term credential: the password as it is)?
bool stun_check_integrity(ConstByteView datagram, const StunMessage& parsed, ConstByteView key);

// Building: stun_begin(), any number of attributes, stun_finish().
bool stun_begin(ByteBuffer* out, uint16_t type, const uint8_t transaction[12]);
bool stun_attribute(ByteBuffer* out, uint16_t type, ConstByteView value);
bool stun_text_attribute(ByteBuffer* out, uint16_t type, StringView text);
bool stun_xor_address(ByteBuffer* out, uint16_t type, network::Ipv4Address address, uint16_t port, const uint8_t transaction[12]);
bool stun_error_attribute(ByteBuffer* out, uint16_t code, StringView reason);
// Fixes the length, adds MESSAGE-INTEGRITY when `key` is not empty, and FINGERPRINT.
bool stun_finish(ByteBuffer* out, ConstByteView key, bool fingerprint);

}  // namespace rtc
