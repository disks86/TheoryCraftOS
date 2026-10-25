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

#include "Rtc/Sdp.hpp"
#include "Utility/Move.hpp"

namespace rtc {

namespace {

bool name_matches(StringView line, StringView name, StringView* value) {
  if (!line.starts_with("a=") || line.size() < 2 + name.size()) return false;
  const StringView rest = line.substr(2);
  if (!rest.starts_with(name)) return false;
  if (rest.size() == name.size()) {
    *value = StringView();
    return true;
  }
  if (rest[name.size()] != ':') return false;
  *value = rest.substr(name.size() + 1);
  return true;
}

bool add_line(List<String>& lines, StringView text, allocators::IAllocator& a) {
  if (lines.count() >= kSdpMaxLines) return false;
  String line(a);
  return line.assign(text) && lines.push_back(util::move(line));
}

}  // namespace

bool SdpMedia::attribute(StringView name, StringView* value) const { return nth(name, 0, value); }

bool SdpMedia::nth(StringView name, uint32_t index, StringView* value) const {
  uint32_t seen = 0;
  for (const String& line : lines) {
    StringView v;
    if (!name_matches(line.view(), name, &v)) continue;
    if (seen++ == index) {
      *value = v;
      return true;
    }
  }
  return false;
}

bool SdpMedia::has_flag(StringView name) const {
  for (const String& line : lines)
    if (line.view().size() == 2 + name.size() && line.view().starts_with("a=") && line.view().substr(2).equals(name)) return true;
  return false;
}

uint32_t SdpMedia::count(StringView name) const {
  uint32_t n = 0;
  StringView v;
  for (const String& line : lines) n += name_matches(line.view(), name, &v) ? 1 : 0;
  return n;
}

bool Sdp::attribute(StringView name, StringView* value) const {
  for (const String& line : session)
    if (name_matches(line.view(), name, value)) return true;
  return false;
}

bool Sdp::add_session(StringView line) { return add_line(session, line, allocator_); }

SdpMedia* Sdp::add_media(StringView kind, uint16_t port, StringView protocol) {
  if (media.count() >= kSdpMaxMedia) return nullptr;
  UniquePointer<SdpMedia> m = UniquePointer<SdpMedia>::make(allocator_, allocator_);
  if (!m || !m->kind.assign(kind) || !m->protocol.assign(protocol)) return nullptr;
  m->port = port;
  SdpMedia* raw = m.get();
  return media.push_back(util::move(m)) ? raw : nullptr;
}

bool Sdp::parse(StringView text) {
  session.clear();
  media.clear();
  if (text.size() > kSdpMaxBytes || !text.starts_with("v=0")) return false;
  SdpMedia* current = nullptr;
  StringView rest = text, line;
  while (rest.next_token('\n', &line)) {
    if (line.ends_with('\r')) line = line.without_suffix(1);
    if (line.empty()) continue;   // (a trailing end of line)
    if (line.size() < 3 || line[1] != '=' || line[0] < 'a' || line[0] > 'z') return false;
    if (line[0] == 'v') continue;   // (the version: checked above, written again by write())
    if (line[0] == 'm') {
      // m=<kind> <port>[/<count>] <protocol> <format>...
      StringView fields = line.substr(2), kind, port, protocol, format;
      if (!fields.next_token(' ', &kind) || !fields.next_token(' ', &port) || !fields.next_token(' ', &protocol) || kind.empty() || protocol.empty()) return false;
      const uint64_t slash = port.find('/');
      uint64_t number = 0;
      if (!(slash == StringView::npos ? port : port.first(slash)).parse_uint(&number) || number > 65535) return false;
      current = add_media(kind, static_cast<uint16_t>(number), protocol);
      if (!current) return false;
      while (fields.next_token(' ', &format)) {
        if (format.empty()) continue;
        String f(allocator_);
        if (!f.assign(format) || !current->formats.push_back(util::move(f))) return false;
      }
      continue;
    }
    if (!add_line(current ? current->lines : session, line, allocator_)) return false;
  }
  return true;
}

bool Sdp::write(String* out) const {
  out->clear();
  if (!out->append("v=0\r\n")) return false;
  for (const String& line : session) {
    if (line.view().starts_with("v=")) continue;
    if (!out->append(line.view()) || !out->append("\r\n")) return false;
  }
  for (const UniquePointer<SdpMedia>& m : media) {
    if (!out->append("m=") || !out->append(m->kind.view()) || !out->append(" ") || !out->append_uint(m->port) || !out->append(" ") || !out->append(m->protocol.view())) return false;
    for (const String& f : m->formats)
      if (!out->append(" ") || !out->append(f.view())) return false;
    if (!out->append("\r\n")) return false;
    for (const String& line : m->lines)
      if (!out->append(line.view()) || !out->append("\r\n")) return false;
  }
  return out->size() <= kSdpMaxBytes;
}

bool sdp_inherited(const Sdp& sdp, const SdpMedia& media, StringView name, StringView* value) { return media.attribute(name, value) || sdp.attribute(name, value); }

bool parse_candidate(StringView text, IceCandidateInfo* out) {
  if (text.starts_with("a=")) text = text.substr(2);
  if (text.starts_with("candidate:")) text = text.substr(10);
  StringView foundation, component, protocol, priority, address, port, typ_word, type, word, value;
  uint64_t n = 0;
  if (!text.next_token(' ', &foundation) || !text.next_token(' ', &component) || !text.next_token(' ', &protocol) || !text.next_token(' ', &priority) ||
      !text.next_token(' ', &address) || !text.next_token(' ', &port) || !text.next_token(' ', &typ_word) || !text.next_token(' ', &type) || !typ_word.equals("typ"))
    return false;
  if (foundation.empty() || foundation.size() > 32 || !component.parse_uint(&n) || n < 1 || n > 256) return false;
  out->component = static_cast<uint32_t>(n);
  if (!priority.parse_uint(&n) || n > 0xFFFFFFFFull) return false;
  out->priority = static_cast<uint32_t>(n);
  if (!port.parse_uint(&n) || n > 65535 || address.empty()) return false;
  out->port = static_cast<uint16_t>(n);
  out->related_port = 0;
  out->related_address.clear();
  if (!out->foundation.assign(foundation) || !out->protocol.assign(protocol) || !out->address.assign(address) || !out->type.assign(type)) return false;
  while (text.next_token(' ', &word)) {   // raddr / rport / tcptype / generation ... : name value pairs
    if (!text.next_token(' ', &value)) break;
    if (word.equals("raddr")) out->related_address.assign(value);
    else if (word.equals("rport") && value.parse_uint(&n) && n <= 65535) out->related_port = static_cast<uint16_t>(n);
  }
  return true;
}

void write_candidate(const IceCandidateInfo& c, String* out) {
  out->assign("candidate:");
  out->append(c.foundation.view());
  out->append(" ");
  out->append_uint(c.component);
  out->append(" ");
  out->append(c.protocol.view());
  out->append(" ");
  out->append_uint(c.priority);
  out->append(" ");
  out->append(c.address.view());
  out->append(" ");
  out->append_uint(c.port);
  out->append(" typ ");
  out->append(c.type.view());
  if (!c.related_address.empty()) {
    out->append(" raddr ");
    out->append(c.related_address.view());
    out->append(" rport ");
    out->append_uint(c.related_port);
  }
}

bool parse_fingerprint(StringView value, String* algorithm, String* hex_colon) {
  StringView alg, hex;
  if (!value.next_token(' ', &alg) || !value.next_token(' ', &hex) || alg.empty() || hex.empty()) return false;
  for (const char c : hex)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || c == ':')) return false;
  return algorithm->assign(alg) && hex_colon->assign(hex);
}

}  // namespace rtc
