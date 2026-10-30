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

// XDomain: another host on the end of a Thunderbolt cable (host to host). The connection manager firmware reports it as an XDomain connection; the hosts then introduce themselves with
// the XDomain discovery protocol (XDP, xdomain.c of the Linux thunderbolt driver) over the control channel: a UUID exchange (who is on the other end) and an exchange of property blocks (a
// small tree of named values: vendor, device, and the services each side offers, e.g. IP networking), read in pieces of up to 45 words. This file has the messages and the property block
// format (property.c); ThunderboltIcm keeps the list and does the exchange. The messages are in the layout of the Linux structures, as for ICM: the words little endian in memory.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace tb_xdp {
// xdomain header (route, length and sequence number), the protocol UUID and the type: 32 bytes
constexpr uint32_t kHeaderBytes = 32, kUuidRequestBytes = 32, kUuidResponseBytes = 56, kPropertiesRequestBytes = 68, kPropertiesResponseHeaderBytes = 72, kErrorResponseBytes = 36;
constexpr uint32_t kMaxDataWords = (256 - 4 - kPropertiesResponseHeaderBytes) / 4;        // words of properties in one response
constexpr uint32_t kMaxBlockWords = 500;
// tb_xdp_type
constexpr uint32_t kUuidResponse = 2, kPropertiesRequest = 3, kPropertiesResponse = 4, kPropertiesChangedRequest = 5, kPropertiesChangedResponse = 6, kErrorResponse = 7, kUuidRequest = 12,
                   kLinkStateStatusRequest = 15, kLinkStateChangeRequest = 17;
// tb_xdp_error
constexpr uint32_t kErrorUnknownPacket = 1, kErrorUnknownDomain = 2, kErrorNotSupported = 3, kErrorNotReady = 4;
extern const uint8_t kProtocolUuid[16];       // b638d70e-42ff-40bb-97c2-90e2c0b2ff07 in the order the structures hold it

// Writes a header: the route, the length (words after the 12 byte route / length part) with the 2 bit sequence number, the protocol UUID and the type. Returns bytes written.
uint32_t build_uuid_request(uint64_t route, uint8_t sequence, ByteView out);
uint32_t build_uuid_response(uint64_t route, uint8_t sequence, const uint8_t uuid[16], ByteView out);
uint32_t build_properties_request(uint64_t route, uint8_t sequence, const uint8_t source[16], const uint8_t destination[16], uint16_t offset, ByteView out);
uint32_t build_error_response(uint64_t route, uint8_t sequence, uint32_t error, ByteView out);
uint32_t build_properties_changed_response(uint64_t route, uint8_t sequence, ByteView out);
// A properties response carrying words [offset, offset + count) of `block` (count at most kMaxDataWords).
uint32_t build_properties_response(uint64_t route, uint8_t sequence, const uint8_t source[16], const uint8_t destination[16], ArrayView<const uint32_t> block, uint32_t generation, uint16_t offset,
                                   ByteView out);

struct Header {
  uint64_t route = 0;
  uint8_t sequence = 0;
  uint32_t length_words = 0, type = 0;
  uint8_t uuid[16] = {};
};
// The header of an XDP packet; false if it is too short, the length field says more than the packet has, or the protocol UUID is not XDP's.
bool parse_header(ConstByteView packet, Header* out);

struct UuidResponse {
  uint8_t uuid[16] = {};
  uint64_t route = 0;
};
// Results of a response: ok, an XDP error response (`error` set), or malformed.
enum class Parsed : uint8_t { Ok, Error, Malformed };
Parsed parse_uuid_response(ConstByteView packet, UuidResponse* out, uint32_t* error);
struct PropertiesResponse {
  uint16_t offset = 0, data_length = 0;
  uint32_t generation = 0;
  uint32_t words = 0;                         // how many words of data this packet carries
};
// The data itself is copied into `data` at `offset` words when it fits the block of `data_length` words.
Parsed parse_properties_response(ConstByteView packet, PropertiesResponse* out, uint32_t* error);
struct PropertiesRequest {
  uint8_t source[16] = {}, destination[16] = {};
  uint16_t offset = 0;
};
bool parse_properties_request(ConstByteView packet, PropertiesRequest* out);
}  // namespace tb_xdp

// ---- property blocks

enum class TbPropertyType : uint8_t { Unknown = 0, Directory = 0x44, Data = 0x64, Text = 0x74, Value = 0x76 };

// One entry of a parsed property tree, flattened: a directory is followed by its own entries at depth + 1.
struct TbPropertyEntry {
  char key[9] = {};
  TbPropertyType type = TbPropertyType::Unknown;
  uint8_t depth = 0;
  uint32_t value = 0;               // a Value's number; for a Directory, Data or Text the length in words
  char text[48] = {};               // a Text's characters (cut to fit)
};

// Parses a property block (the root directory) of `block` words, as received. Bounded: at most 8 levels and 128 entries, every length and offset checked against the block. False if the
// block is not a root directory (bad magic, a length beyond the block) or an entry does not make sense.
bool tb_properties_parse(ArrayView<const uint32_t> block, List<TbPropertyEntry>& out);
// The root-level entry with this key and type, or nullptr.
const TbPropertyEntry* tb_properties_find(const List<TbPropertyEntry>& entries, const char* key, TbPropertyType type);

// A block for this host: the root directory with vendorid, deviceid and devicerv as values and the vendor's and device's names as text under the keys vendorid and deviceid (a value and a text
// may share a key; the type tells them apart).
bool tb_properties_build_local(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name, List<uint32_t>& out);
// The same with the network service (USB4NET / ThunderboltIP) offered: a directory "network" (UUID c66189ca-1cce-4195-bdb8-49592e5f5a4f) with prtcid 1, prtcvers 1, prtcrevs 1 and
// prtcstns = `network_flags` (the TbNet flags of ThunderboltNet.hpp).
bool tb_properties_build_local(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name, bool offer_network, uint32_t network_flags,
                               List<uint32_t>& out);

}  // namespace hardware
