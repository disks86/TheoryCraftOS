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

#include "Hardware/Bus/Thunderbolt/ThunderboltXdomainHost.hpp"

namespace hardware {

namespace {
bool uuid_equal(const uint8_t* a, const uint8_t* b) {
  bool same = true;
  for (uint32_t k = 0; k < 16; ++k) same = same && a[k] == b[k];
  return same;
}
bool uuid_zero(const uint8_t* a) {
  bool zero = true;
  for (uint32_t k = 0; k < 16; ++k) zero = zero && a[k] == 0;
  return zero;
}
constexpr uint32_t kXdpTimeoutUs = 2000000, kXdpGiveUpAfter = 5, kXdpRetryPolls = 50;
}  // namespace

bool TbXdomainHost::set_identity(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name) {
  identity_[0] = vendor_id;
  identity_[1] = device_id;
  identity_[2] = device_revision;
  char vendor[sizeof vendor_name_] = {}, device[sizeof device_name_] = {};
  for (uint32_t i = 0; vendor_name && vendor_name[i] != 0 && i + 1 < sizeof vendor; ++i) vendor[i] = vendor_name[i];
  for (uint32_t i = 0; device_name && device_name[i] != 0 && i + 1 < sizeof device; ++i) device[i] = device_name[i];
  for (uint32_t i = 0; i < sizeof vendor_name_; ++i) vendor_name_[i] = vendor[i];
  for (uint32_t i = 0; i < sizeof device_name_; ++i) device_name_[i] = device[i];
  if (!tb_properties_build_local(identity_[0], identity_[1], identity_[2], vendor_name_, device_name_, offer_network_, network_flags_, local_block_)) return false;
  ++local_generation_;
  return true;
}

bool TbXdomainHost::offer_network(uint32_t flags) {
  offer_network_ = true;
  network_flags_ = flags;
  return set_identity(identity_[0], identity_[1], identity_[2], vendor_name_, device_name_);
}

bool TbXdomainHost::ensure_identity() { return local_block_.count() != 0 || set_identity(identity_[0], identity_[1], identity_[2], vendor_name_, device_name_); }

int TbXdomainHost::find(uint64_t route) const {
  for (uint64_t i = 0; i < list_.count(); ++i)
    if (live_[i] && list_[i].route == route) return static_cast<int>(i);
  return -1;
}

int TbXdomainHost::add(uint64_t route, const uint8_t local_uuid[16]) {
  const int existing = find(route);
  if (existing >= 0) return existing;
  TbXDomain x;
  x.route = route;
  for (uint32_t k = 0; k < 16; ++k) x.local_uuid[k] = local_uuid[k];
  for (uint64_t i = 0; i < list_.count(); ++i) {
    if (live_[i]) continue;                                  // (a free entry)
    list_[i] = x;
    live_[i] = true;
    return static_cast<int>(i);
  }
  if (list_.count() >= kMaxHosts || !list_.push_back(x)) return -1;
  live_[list_.count() - 1] = true;
  return static_cast<int>(list_.count() - 1);
}

bool TbXdomainHost::remove(uint64_t route) {
  const int index = find(route);
  if (index < 0) return false;
  if (list_[index].tunnel) paths_.close_paths(list_[index]);
  list_[index] = TbXDomain();
  live_[index] = false;
  return true;
}

void TbXdomainHost::clear() {
  for (uint64_t i = 0; i < list_.count(); ++i)
    if (live_[i]) remove(list_[i].route);
}

bool TbXdomainHost::approve(uint64_t index, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (index >= list_.count() || !live_[index]) {
    *error = "no such host";
    return false;
  }
  if (list_[index].tunnel) {
    *error = "a path is open already";
    return false;
  }
  if (!paths_.open_paths(list_[index], transmit_path, transmit_ring, receive_path, receive_ring, error)) return false;
  list_[index].tunnel = true;
  return true;
}

bool TbXdomainHost::disconnect(uint64_t index) {
  if (index >= list_.count() || !live_[index] || !list_[index].tunnel) return false;
  paths_.close_paths(list_[index]);
  list_[index].tunnel = false;
  return true;
}

bool TbXdomainHost::handle(TbPacketType type, ConstByteView body) {
  if (type == TbPacketType::XdomainRequest) {
    answer(body);
    return true;
  }
  if (type == TbPacketType::XdomainResponse) return ip_handler_ && ip_handler_->handle_ip_packet(body);
  return false;
}

// What the other host asks of us: who we are, and our properties.
void TbXdomainHost::answer(ConstByteView body) {
  tb_xdp::Header h;
  if (!tb_xdp::parse_header(body, &h) || !ensure_identity()) return;
  const int found = find(h.route);
  if (found < 0) return;
  const uint64_t route = h.route;
  uint8_t local_uuid[16];
  for (uint32_t k = 0; k < 16; ++k) local_uuid[k] = list_[found].local_uuid[k];
  uint8_t out[tb_frame::kMaxBody];
  uint32_t n = 0;
  switch (h.type) {
    case tb_xdp::kUuidRequest:
      n = tb_xdp::build_uuid_response(route, h.sequence, local_uuid, ByteView(out, sizeof out));
      break;
    case tb_xdp::kPropertiesRequest: {
      tb_xdp::PropertiesRequest q;
      if (!tb_xdp::parse_properties_request(body, &q)) return;
      if (!uuid_equal(q.destination, local_uuid)) n = tb_xdp::build_error_response(route, h.sequence, tb_xdp::kErrorUnknownDomain, ByteView(out, sizeof out));
      else n = tb_xdp::build_properties_response(route, h.sequence, local_uuid, q.source, ArrayView<const uint32_t>(&local_block_[0], local_block_.count()), local_generation_, q.offset, ByteView(out, sizeof out));
      break;
    }
    case tb_xdp::kPropertiesChangedRequest:
      n = tb_xdp::build_properties_changed_response(route, h.sequence, ByteView(out, sizeof out));
      list_[found].discovered = false;                       // (their properties changed: read them again)
      list_[found].gave_up = false;
      list_[found].attempts = 0;
      break;
    default:
      n = tb_xdp::build_error_response(route, h.sequence, tb_xdp::kErrorNotSupported, ByteView(out, sizeof out));
      break;
  }
  if (n != 0) control_.send(TbPacketType::XdomainResponse, ConstByteView(out, n));
}

void TbXdomainHost::service() {
  ++polls_;
  for (uint64_t i = 0; i < list_.count(); ++i) {
    if (!live_[i]) continue;
    const TbXDomain x = list_[i];                            // (a copy: the list can change during the exchange)
    if (x.discovered || x.gave_up) continue;
    if (x.attempts != 0 && polls_ % kXdpRetryPolls != 0) continue;
    const bool ok = discover(i);
    const int again = find(x.route);
    if (again >= 0 && !ok && !list_[again].discovered && ++list_[again].attempts >= kXdpGiveUpAfter) list_[again].gave_up = true;
    return;
  }
}

// The exchange with a host that was found on a port: its UUID, then its properties in pieces.
bool TbXdomainHost::discover(uint64_t index) {
  if (!ensure_identity()) return false;
  const TbXDomain xd = list_[index];
  uint8_t request[tb_frame::kMaxBody], response[tb_frame::kFrameBytes];
  uint32_t got = 0;
  uint32_t n = tb_xdp::build_uuid_request(xd.route, xd.attempts % 4, ByteView(request, sizeof request));
  if (n == 0 || control_.xdomain_request(ConstByteView(request, n), ByteView(response, sizeof response), &got, kXdpTimeoutUs, sink_) != TbControl::Result::Ok) return false;
  tb_xdp::UuidResponse who;
  uint32_t error = 0;
  if (tb_xdp::parse_uuid_response(ConstByteView(response, got < sizeof response ? got : sizeof response), &who, &error) != tb_xdp::Parsed::Ok) return false;
  uint8_t remote[16];
  for (uint32_t k = 0; k < 16; ++k) remote[k] = who.uuid[k];
  if (uuid_zero(remote)) return false;
  if (!uuid_zero(xd.remote_uuid) && !uuid_equal(remote, xd.remote_uuid)) return false;      // another host than before: it will be found again

  List<uint32_t> block(allocator_);
  uint32_t length = 0, offset = 0, generation = 0;
  for (uint32_t round = 0; round < tb_xdp::kMaxBlockWords; ++round) {
    n = tb_xdp::build_properties_request(xd.route, xd.attempts % 4, xd.local_uuid, remote, static_cast<uint16_t>(offset), ByteView(request, sizeof request));
    if (n == 0 || control_.xdomain_request(ConstByteView(request, n), ByteView(response, sizeof response), &got, kXdpTimeoutUs, sink_) != TbControl::Result::Ok) return false;
    const ConstByteView packet(response, got < sizeof response ? got : sizeof response);
    tb_xdp::PropertiesResponse p;
    if (tb_xdp::parse_properties_response(packet, &p, &error) != tb_xdp::Parsed::Ok || p.offset != offset) return false;
    if (length == 0) {
      length = p.data_length;
      generation = p.generation;
      if (length == 0 || length > tb_xdp::kMaxBlockWords) return false;
      for (uint32_t i = 0; i < length; ++i)
        if (!block.push_back(0)) return false;
    } else if (p.data_length != length) {
      return false;
    }
    if (p.words == 0) return false;
    ByteReader r(packet);
    r.skip(tb_xdp::kPropertiesResponseHeaderBytes);
    for (uint32_t i = 0; i < p.words && offset + i < length; ++i) block[offset + i] = r.le32();
    if (!r.ok()) return false;
    offset += p.words;
    if (offset >= length) break;
  }
  if (offset < length) return false;
  List<TbPropertyEntry> entries(allocator_);
  if (!tb_properties_parse(ArrayView<const uint32_t>(&block[0], block.count()), entries)) return false;

  const int at = find(xd.route);
  if (at < 0) return true;                                   // (it went away meanwhile)
  TbXDomain& x = list_[at];
  for (uint32_t k = 0; k < 16; ++k) x.remote_uuid[k] = remote[k];
  x.generation = generation;
  const TbPropertyEntry* vendor = tb_properties_find(entries, "vendorid", TbPropertyType::Value);
  const TbPropertyEntry* device = tb_properties_find(entries, "deviceid", TbPropertyType::Value);
  x.vendor_id = vendor ? vendor->value : 0;
  x.device_id = device ? device->value : 0;
  const TbPropertyEntry* vendor_name = tb_properties_find(entries, "vendorid", TbPropertyType::Text);
  const TbPropertyEntry* device_name = tb_properties_find(entries, "deviceid", TbPropertyType::Text);
  for (uint32_t k = 0; k < sizeof x.vendor_name; ++k) x.vendor_name[k] = vendor_name && k + 1 < sizeof x.vendor_name ? vendor_name->text[k] : 0;
  for (uint32_t k = 0; k < sizeof x.device_name; ++k) x.device_name[k] = device_name && k + 1 < sizeof x.device_name ? device_name->text[k] : 0;
  // the services: a directory of the root holds the protocol's id (prtcid), its settings (prtcstns) and more, at depth 1
  x.protocol_count = 0;
  x.has_network = false;
  x.network_flags = 0;
  uint32_t id = 0, flags = 0;
  bool have_id = false;
  auto close = [&]() {
    if (!have_id) return;
    if (x.protocol_count < 8) x.protocols[x.protocol_count++] = id;
    if (id == 1) {
      x.has_network = true;
      x.network_flags = flags;
    }
  };
  auto is = [](const TbPropertyEntry& e, const char* key) {
    for (uint32_t k = 0; k < 9; ++k) {
      if (e.key[k] != key[k]) return false;
      if (key[k] == 0) return true;
    }
    return true;
  };
  for (uint64_t e = 0; e < entries.count(); ++e) {
    const TbPropertyEntry& entry = entries[e];
    if (entry.depth == 0) {
      if (entry.type == TbPropertyType::Directory) {
        close();
        have_id = false;
        id = flags = 0;
      }
    } else if (entry.depth == 1 && entry.type == TbPropertyType::Value) {
      if (is(entry, "prtcid")) { id = entry.value; have_id = true; }
      else if (is(entry, "prtcstns")) flags = entry.value;
    }
  }
  close();
  x.discovered = true;
  return true;
}

}  // namespace hardware
