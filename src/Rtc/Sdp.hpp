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

// Session descriptions (RFC 8866, as WebRTC uses them in JSEP, RFC 9429). An Sdp keeps the lines of the description: the ones before the first media section
// (session level) and, for each "m=" section, its fields and the lines after it. Parsing checks the shape (every line "x=value", the version, the sizes) and nothing of
// the meaning; the helpers below read what the peer connection needs - attributes by name, the ICE parameters, the DTLS fingerprint and role, candidates, codecs.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace rtc {

constexpr uint32_t kSdpMaxBytes = 128 * 1024, kSdpMaxMedia = 16, kSdpMaxLines = 2048;

class SdpMedia {
 public:
  explicit SdpMedia(allocators::IAllocator& a) : kind(a), protocol(a), formats(a), lines(a) {}
  String kind, protocol;   // "audio" / "video" / "application"; "UDP/TLS/RTP/SAVPF", "UDP/DTLS/SCTP" ...
  uint16_t port = 9;
  List<String> formats;    // payload types, or "webrtc-datachannel"
  List<String> lines;      // after the m= line, without line ends ("c=IN IP4 ...", "a=mid:0", ...)

  // The value of the first "a=name:value" ("" for "a=name"); false if there is none.
  bool attribute(StringView name, StringView* value) const;
  bool has_flag(StringView name) const;                       // "a=name"
  uint32_t count(StringView name) const;                      // how many "a=name..." lines
  bool nth(StringView name, uint32_t index, StringView* value) const;
};

class Sdp {
 public:
  explicit Sdp(allocators::IAllocator& a) : allocator_(a), session(a), media(a) {}
  Sdp(const Sdp&) = delete;
  Sdp& operator=(const Sdp&) = delete;
  List<String> session;                       // the lines before the first m=
  List<UniquePointer<SdpMedia>> media;

  // False for text that is not a session description (or too large).
  bool parse(StringView text);
  bool write(String* out) const;

  // Session-level attributes.
  bool attribute(StringView name, StringView* value) const;
  bool add_session(StringView line);
  SdpMedia* add_media(StringView kind, uint16_t port, StringView protocol);
  allocators::IAllocator& allocator() const { return allocator_; }

 private:
  allocators::IAllocator& allocator_;
};

// An attribute of a media section, or of the session when the media section does not have it (the order the standard gives for ice-ufrag, ice-pwd, fingerprint, setup).
bool sdp_inherited(const Sdp& sdp, const SdpMedia& media, StringView name, StringView* value);

// "a=candidate:..." parsed (RFC 8839).
struct IceCandidateInfo {
  explicit IceCandidateInfo(allocators::IAllocator& a) : foundation(a), type(a), protocol(a), address(a), related_address(a) {}
  String foundation, type, protocol, address, related_address;
  uint32_t component = 1;
  uint32_t priority = 0;
  uint16_t port = 0, related_port = 0;
};
// `text` is the candidate with or without the "candidate:" prefix. False if it does not parse.
bool parse_candidate(StringView text, IceCandidateInfo* out);
void write_candidate(const IceCandidateInfo& candidate, String* out);   // "candidate:..." (no "a=")

// "a=fingerprint:sha-256 AB:CD:..." -> the algorithm and the bytes.
bool parse_fingerprint(StringView value, String* algorithm, String* hex_colon);

}  // namespace rtc
