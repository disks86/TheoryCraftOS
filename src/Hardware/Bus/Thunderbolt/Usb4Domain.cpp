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

#include "Hardware/Bus/Thunderbolt/Usb4Domain.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace usb4;

namespace {
constexpr uint8_t kPathSource = 1, kPathInternal = 2, kPathDestination = 4, kPathAll = 7;
constexpr uint32_t kHopIdLimit = kHopWords * 64;            // HopIDs above this are not tracked (no adapter has that many)
constexpr uint8_t kErrorInvalidConfig = 2, kErrorNoSuchPort = 6, kErrorAckPlugEvent = 7;

uint32_t route_length(uint64_t route) {
  uint32_t length = 0;
  while (route != 0) {
    ++length;
    route >>= 8;
  }
  return length;
}

bool bit_test(const uint64_t* bits, uint32_t index) { return (bits[index / 64] >> (index % 64)) & 1; }
void bit_set(uint64_t* bits, uint32_t index) { bits[index / 64] |= 1ull << (index % 64); }
void bit_clear(uint64_t* bits, uint32_t index) { bits[index / 64] &= ~(1ull << (index % 64)); }
}  // namespace

// ------------------------------------------------------------------ configuration space

bool Usb4Domain::read(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<uint32_t> words, uint8_t* response_port) {
  TbControl::ConfigError error;
  if (control_.read_config(route, port, space, offset, words, kTimeoutUs, &error, this) != TbControl::Result::Ok) return false;
  if (response_port) *response_port = error.response_port;
  return true;
}

bool Usb4Domain::write(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<const uint32_t> words) {
  TbControl::ConfigError error;
  return control_.write_config(route, port, space, offset, words, kTimeoutUs, &error, this) == TbControl::Result::Ok;
}

// The capability of a port with this id (a linked list from the first offset in the port header), or -1.
int Usb4Domain::find_port_cap(const Usb4Router& r, uint32_t port, uint8_t first, uint8_t id) {
  uint32_t offset = first;
  for (uint32_t i = 0; i < 64 && offset != 0; ++i) {
    uint32_t word = 0;
    if (!port_read(r, port, offset, ArrayView<uint32_t>(&word, 1))) return -1;
    if (((word >> 8) & 0xFF) == id) return static_cast<int>(offset);
    offset = word & 0xFF;
  }
  return -1;
}

int Usb4Domain::find_router(uint64_t route) const {
  for (uint64_t i = 0; i < routers_.count(); ++i)
    if (routers_[i] && routers_[i]->present && routers_[i]->route == route) return static_cast<int>(i);
  return -1;
}

uint32_t Usb4Domain::present_count() const {
  uint32_t n = 0;
  for (uint64_t i = 0; i < routers_.count(); ++i) n += routers_[i] && routers_[i]->present;
  return n;
}

int Usb4Domain::allocate_router() {
  for (uint64_t i = 0; i < routers_.count(); ++i)
    if (routers_[i] && !routers_[i]->present) {
      *routers_[i] = Usb4Router();
      return static_cast<int>(i);
    }
  if (routers_.count() >= kMaxRouters) return -1;
  UniquePointer<Usb4Router> router = UniquePointer<Usb4Router>::make(allocator_);
  if (!router || !routers_.push_back(util::move(router))) return -1;
  return static_cast<int>(routers_.count() - 1);
}

// ------------------------------------------------------------------ routers

// Reads the router at `route` (the header and the upstream port), writes its configuration and reads its ports. Returns its index.
bool Usb4Domain::read_router(uint64_t route, int16_t parent, uint8_t parent_port, int* index_out, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  uint32_t header[5];
  uint8_t upstream = 0;
  if (!read(route, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(header, 5), &upstream)) {
    *error = "the router does not answer";
    return false;
  }
  const uint32_t depth = route_length(route);
  const uint8_t version = static_cast<uint8_t>((header[4] >> 24) >> 5 & 0x7);
  if (depth > kMaxDepth) {
    *error = "too deep";
    return false;
  }
  if (version == 0) {
    *error = "not a USB4 router (a Thunderbolt 3 device router is not supported by the software connection manager)";
    return false;
  }
  const uint32_t max_port = (header[1] >> 14) & 0x3F;
  if (max_port == 0 || upstream > max_port) {
    *error = "the router's ports make no sense";
    return false;
  }
  const int index = allocate_router();
  if (index < 0) {
    *error = "too many routers";
    return false;
  }
  Usb4Router& r = *routers_[index];
  r.present = true;
  r.route = route;
  r.depth = static_cast<uint8_t>(depth);
  r.upstream_port = upstream;
  r.max_port = static_cast<uint8_t>(max_port);
  r.vendor_id = static_cast<uint16_t>(header[0]);
  r.device_id = static_cast<uint16_t>(header[0] >> 16);
  r.usb4_version = version;
  r.parent = parent;
  r.parent_port = parent_port;
  // configure it (tb_switch_configure): its route, depth, upstream port, enabled; the connection manager's version and the pause between plug events
  uint32_t config[4];
  config[0] = (header[1] & ~((0x3Fu << 8) | (0x7u << 20))) | (static_cast<uint32_t>(upstream) << 8) | (depth << 20);
  config[1] = static_cast<uint32_t>(route);
  config[2] = (static_cast<uint32_t>(route >> 32) & 0x3FFFFF) | (1u << 31);
  config[3] = (header[4] & 0xFFFF0000u) | 0xA | ((version < 2 ? 0x10u : 0x20u) << 8);
  if (!write(route, 0, TbConfigSpace::Switch, kRouterCs1, ArrayView<const uint32_t>(config, 4))) {
    r.present = false;
    *error = "could not configure the router";
    return false;
  }
  uint32_t uid[2] = {0, 0};
  if (read(route, 0, TbConfigSpace::Switch, kRouterCs7, ArrayView<uint32_t>(uid, 2))) r.uid = uid[0] | (static_cast<uint64_t>(uid[1]) << 32);
  r.configured = true;
  if (!init_ports(r)) {
    r.present = false;
    *error = "could not read the router's ports";
    return false;
  }
  link_lane_adapters(r);
  *index_out = index;
  return true;
}

bool Usb4Domain::init_ports(Usb4Router& r) {
  for (uint32_t p = 1; p <= r.max_port; ++p) {
    Usb4Port& port = r.ports[p];
    port = Usb4Port();
    port.number = static_cast<uint8_t>(p);
    uint32_t h[8];
    TbControl::ConfigError error;
    const TbControl::Result result = control_.read_config(r.route, p, TbConfigSpace::Port, 0, ArrayView<uint32_t>(h, 8), kTimeoutUs, &error, this);
    if (result != TbControl::Result::Ok) {
      if (result == TbControl::Result::Failed && error.reported && (error.code == kErrorNoSuchPort || error.code == kErrorInvalidConfig)) {
        port.disabled = true;                            // (a port number the router does not implement)
        continue;
      }
      return false;
    }
    port.type = h[2] & 0xFFFFFF;
    port.max_in_hop = static_cast<uint16_t>(h[5] & 0x7FF);
    port.max_out_hop = static_cast<uint16_t>((h[5] >> 11) & 0x7FF);
    const uint8_t first = static_cast<uint8_t>(h[1] & 0xFF);
    if (port.type == kTypePort) {
      const int phy = find_port_cap(r, p, first, kPortCapPhy);
      if (phy > 0) port.cap_phy = static_cast<uint8_t>(phy);
      const int u4 = find_port_cap(r, p, first, kPortCapUsb4);
      if (u4 > 0) port.cap_usb4 = static_cast<uint8_t>(u4);
    } else {
      const int adapter = find_port_cap(r, p, first, kPortCapAdapter);
      if (adapter > 0) port.cap_adapter = static_cast<uint8_t>(adapter);
    }
  }
  return true;
}

// Consecutive lane adapters are the two lanes of one link (tb_switch_default_link_ports).
void Usb4Domain::link_lane_adapters(Usb4Router& r) {
  for (uint32_t p = 1; p < r.max_port; ++p) {
    Usb4Port& a = r.ports[p];
    Usb4Port& b = r.ports[p + 1];
    if (!a.is_lane() || !b.is_lane() || a.dual_link >= 0 || b.dual_link >= 0) continue;
    a.link_nr = 0;
    a.dual_link = static_cast<int8_t>(b.number);
    b.link_nr = 1;
    b.dual_link = static_cast<int8_t>(a.number);
  }
}

bool Usb4Domain::port_up(const Usb4Router& r, uint32_t port) {
  const Usb4Port& p = r.ports[port];
  if (p.cap_phy == 0) return false;
  uint32_t words[2];
  if (!port_read(r, port, p.cap_phy, ArrayView<uint32_t>(words, 2))) return false;
  const uint32_t state = (words[1] >> 26) & 0xF;
  return state == kPortUp || state == kPortTxCl0s || state == kPortRxCl0s || state == kPortCl1 || state == kPortCl2;
}

bool Usb4Domain::unlock_downstream(const Usb4Router& r, uint32_t port) {
  uint32_t word = 0;
  if (!port_read(r, port, kAdpCs4, ArrayView<uint32_t>(&word, 1))) return false;
  word &= ~kAdpCs4Lck;
  return port_write(r, port, kAdpCs4, ArrayView<const uint32_t>(&word, 1));
}

bool Usb4Domain::configure_link(Usb4Router& child, Usb4Router& parent, uint32_t down_port) {
  // both ends of the link are marked configured (usb4_port_configure: PORT_CS_19.PC)
  const uint32_t ends[2] = {down_port, child.upstream_port};
  Usb4Router* routers[2] = {&parent, &child};
  for (uint32_t i = 0; i < 2; ++i) {
    const Usb4Port& p = routers[i]->ports[ends[i]];
    if (p.cap_usb4 == 0) return false;
    uint32_t word = 0;
    if (!port_read(*routers[i], ends[i], p.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1))) return false;
    word |= kPortCs19Pc;
    if (!port_write(*routers[i], ends[i], p.cap_usb4 + kPortCs19, ArrayView<const uint32_t>(&word, 1))) return false;
  }
  return true;
}

// usb4_switch_setup: CNS cleared (this connection manager does Thunderbolt 3 tunnelling too), PCIe tunnelling allowed when the parent has a PCIe down adapter (and the xHCI over it).
bool Usb4Domain::switch_setup(Usb4Router& child, const Usb4Router& parent) {
  uint32_t cs6 = 0, cs5 = 0;
  if (!read(child.route, 0, TbConfigSpace::Switch, kRouterCs6, ArrayView<uint32_t>(&cs6, 1)) || !read(child.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<uint32_t>(&cs5, 1))) return false;
  bool parent_has_pcie = false;
  for (uint32_t p = 1; p <= parent.max_port; ++p) parent_has_pcie = parent_has_pcie || parent.ports[p].type == kTypePcieDown;
  if (parent_has_pcie) {
    cs5 |= kCs5Pto;
    if (cs6 & kCs6Hci) cs5 |= kCs5Hco;
  }
  cs5 &= ~kCs5Cns;
  return write(child.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<const uint32_t>(&cs5, 1));
}

bool Usb4Domain::configuration_valid(Usb4Router& r) {
  uint32_t cs5 = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<uint32_t>(&cs5, 1))) return false;
  cs5 |= kCs5Cv;
  if (!write(r.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<const uint32_t>(&cs5, 1))) return false;
  for (uint32_t waited = 0; waited < 50000; waited += 1000) {
    uint32_t cs6 = 0;
    if (read(r.route, 0, TbConfigSpace::Switch, kRouterCs6, ArrayView<uint32_t>(&cs6, 1)) && (cs6 & kCs6Cr)) return true;
    delay_us(1000);
  }
  return false;
}

// ------------------------------------------------------------------ the tree

bool Usb4Domain::start(const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (started_) return true;
  int root = -1;
  if (!read_router(0, -1, 0, &root, error)) return false;
  started_ = true;
  scan_router(root);
  close_preboot_tunnels();
  return true;
}

void Usb4Domain::scan_router(int index) {
  for (uint32_t p = 1; p <= routers_[index]->max_port; ++p) scan_port(index, p);
}

void Usb4Domain::scan_port(int index, uint32_t port_number) {
  {
    Usb4Router& r = *routers_[index];
    const Usb4Port& port = r.ports[port_number];
    if (port_number == r.upstream_port && r.parent >= 0) return;
    if (port.disabled || !port.is_lane() || port.link_nr != 0 || port.remote_router >= 0) return;
  }
  // wait for the link: up to a second while it is connecting
  bool up = false;
  for (uint32_t tries = 0; tries < 10 && !up; ++tries) {
    Usb4Router& r = *routers_[index];
    const Usb4Port& port = r.ports[port_number];
    uint32_t words[2];
    if (port.cap_phy == 0 || !port_read(r, port_number, port.cap_phy, ArrayView<uint32_t>(words, 2))) return;
    const uint32_t state = (words[1] >> 26) & 0xF;
    if (state == kPortDisabled || state == kPortUnplugged) return;
    up = state == kPortUp || state == kPortTxCl0s || state == kPortRxCl0s || state == kPortCl1 || state == kPortCl2;
    if (!up) delay_us(100000);
  }
  if (!up) return;
  Usb4Router& parent_ref = *routers_[index];
  if (!unlock_downstream(parent_ref, port_number)) return;
  const uint64_t route = parent_ref.route | (static_cast<uint64_t>(port_number) << (8 * parent_ref.depth));
  int child_index = -1;
  const char* why = nullptr;
  if (!read_router(route, static_cast<int16_t>(index), static_cast<uint8_t>(port_number), &child_index, &why)) {
    ++unreachable_;                                       // (another host on the cable, a router we cannot run, or nothing)
    return;
  }
  Usb4Router& parent = *routers_[index];
  Usb4Router& child = *routers_[child_index];
  if (!configure_link(child, parent, port_number) || !switch_setup(child, parent)) {
    remove_router(child_index);
    return;
  }
  parent.ports[port_number].remote_router = static_cast<int16_t>(child_index);
  child.ports[child.upstream_port].remote_router = static_cast<int16_t>(index);
  bond_link(child, parent, port_number);                 // (two lanes as one when both ends can; the link stays as it was when not)
  configuration_valid(child);
  scan_router(child_index);
}

void Usb4Domain::remove_below(int index, uint32_t port) {
  Usb4Port& p = routers_[index]->ports[port];
  if (p.remote_router < 0) return;
  const int child = p.remote_router;
  p.remote_router = -1;
  if (p.dual_link >= 0) routers_[index]->ports[p.dual_link].remote_router = -1;
  remove_router(child);
}

void Usb4Domain::remove_router(int index) {
  Usb4Router& r = *routers_[index];
  for (uint32_t p = 1; p <= r.max_port; ++p) {
    Usb4Port& port = r.ports[p];
    if (port.remote_router >= 0 && port.remote_router != r.parent) {
      const int child = port.remote_router;
      port.remote_router = -1;
      remove_router(child);
    }
  }
  for (uint32_t k = 0; k < 2; ++k) {
    const int tunnel = find_tunnel(index, k == 0 ? Usb4TunnelKind::Pcie : Usb4TunnelKind::Usb3);
    if (tunnel >= 0) remove_tunnel(tunnel);                // (the device is gone: only the host side can be undone)
  }
  r.present = false;
}

void Usb4Domain::close_preboot_tunnels() {
  for (uint64_t i = 0; i < routers_.count(); ++i) {
    if (!routers_[i] || !routers_[i]->present) continue;
    Usb4Router& r = *routers_[i];
    for (uint32_t p = 1; p <= r.max_port; ++p) {
      const Usb4Port& port = r.ports[p];
      if ((port.type != kTypePcieDown && port.type != kTypePcieUp && port.type != kTypeUsb3Down && port.type != kTypeUsb3Up) || port.cap_adapter == 0) continue;
      uint32_t word = 0;
      if (!port_read(r, p, port.cap_adapter + kAdpPcieCs0, ArrayView<uint32_t>(&word, 1)) || !(word & kAdpPcieCs0Pe)) continue;       // (PE is bit 31 of the first dword of the PCIe and the USB 3 adapters alike)
      if (find_tunnel(static_cast<int>(i), Usb4TunnelKind::Pcie) >= 0 || find_tunnel(static_cast<int>(i), Usb4TunnelKind::Usb3) >= 0) continue;     // (ours)
      const Usb4TunnelKind kind = (port.type == kTypePcieDown || port.type == kTypePcieUp) ? Usb4TunnelKind::Pcie : Usb4TunnelKind::Usb3;
      if (set_adapter_enable(r, p, kind, false)) ++preboot_closed_;
    }
  }
}

// ------------------------------------------------------------------ events

bool Usb4Domain::on_event(TbPacketType type, ConstByteView body) {
  if (type != TbPacketType::Event || body.size() < 12) return false;
  ByteReader r(body);
  const uint32_t hi = r.le32(), lo = r.le32(), word = r.le32();
  if (!r.ok()) return false;
  ++events_seen_;
  if (plug_count_ >= kPlugQueue) return true;                  // (full: the next scan finds what was missed)
  Plug& plug = plugs_[(plug_head_ + plug_count_) % kPlugQueue];
  plug.route = (static_cast<uint64_t>(hi & 0x3FFFFF) << 32) | lo;
  plug.port = static_cast<uint8_t>(word & 0x3F);
  plug.unplug = (word >> 31) != 0;
  ++plug_count_;
  return true;
}

void Usb4Domain::ack_plug(const Plug& plug) {
  uint8_t packet[12];
  ByteWriter w{ByteView(packet)};
  w.le32(static_cast<uint32_t>(plug.route >> 32));
  w.le32(static_cast<uint32_t>(plug.route));
  w.le32(kErrorAckPlugEvent | (static_cast<uint32_t>(plug.port) << 8) | ((plug.unplug ? 3u : 2u) << 30));
  if (w.ok()) control_.send(TbPacketType::Error, w.written());
}

void Usb4Domain::process_plug(const Plug& plug) {
  ack_plug(plug);
  const int index = find_router(plug.route);
  if (index < 0 || plug.port == 0 || plug.port > routers_[index]->max_port) return;
  if (plug.unplug) remove_below(index, plug.port);
  else scan_port(index, plug.port);
}

void Usb4Domain::poll() {
  control_.poll(this);
  while (plug_count_ != 0) {
    const Plug plug = plugs_[plug_head_];
    plug_head_ = (plug_head_ + 1) % kPlugQueue;
    --plug_count_;
    process_plug(plug);
    control_.poll(this);
  }
}

// ------------------------------------------------------------------ hop tables and paths

int Usb4Domain::allocate_hop(Usb4Router& r, uint32_t port, bool in, int min_hop, int max_hop) {
  Usb4Port& p = r.ports[port];
  uint64_t* bits = in ? p.in_hops : p.out_hops;
  const uint32_t limit = in ? p.max_in_hop : p.max_out_hop;
  uint32_t lo = min_hop < static_cast<int>(kMinPathHop) ? kMinPathHop : static_cast<uint32_t>(min_hop);
  uint32_t hi = max_hop < 0 || static_cast<uint32_t>(max_hop) > limit ? limit : static_cast<uint32_t>(max_hop);
  if (hi >= kHopIdLimit) hi = kHopIdLimit - 1;
  for (uint32_t h = lo; h <= hi; ++h) {
    if (bit_test(bits, h)) continue;
    bit_set(bits, h);
    return static_cast<int>(h);
  }
  return -1;
}

void Usb4Domain::release_hop(Usb4Router& r, uint32_t port, bool in, uint32_t hop) {
  if (hop >= kHopIdLimit) return;
  bit_clear(in ? r.ports[port].in_hops : r.ports[port].out_hops, hop);
}

void Usb4Domain::release_path(Usb4Path* path) {
  for (uint32_t i = 0; i < path->length; ++i) {
    const Usb4Hop& hop = path->hops[i];
    if (hop.router < 0 || !routers_[hop.router]->present) continue;
    release_hop(*routers_[hop.router], hop.in_port, true, hop.in_hop);
    release_hop(*routers_[hop.router], hop.out_port, false, hop.out_hop);
  }
  *path = Usb4Path();
}

// A path over one link, from `start_port` of the router `start` to `end_port` of the router `end` that hangs from it (or the other way round): two hops, as tb_path_alloc makes of a PCIe path.
// The HopID on the link is the lowest that is free as the output of the first router's lane adapter and as the input of the second router's.
bool Usb4Domain::build_path(Usb4Path* path, int first_router, uint32_t first_port, int second_router, uint32_t second_port, Usb4TunnelKind kind) {
  *path = Usb4Path();
  path->priority = kind == Usb4TunnelKind::Usb3 ? kUsb3Priority : kPciePriority;
  path->weight = kind == Usb4TunnelKind::Usb3 ? kUsb3Weight : kPcieWeight;
  Usb4Router& a = *routers_[first_router];
  Usb4Router& b = *routers_[second_router];
  // which of them is the parent: its lane adapter toward the other, and the other's upstream lane adapter
  const bool a_is_parent = b.parent == first_router;
  Usb4Router& parent = a_is_parent ? a : b;
  Usb4Router& child = a_is_parent ? b : a;
  const uint32_t parent_lane = child.parent_port;
  uint32_t child_lane = child.upstream_port;
  if (child.ports[child_lane].link_nr != 0 && child.ports[child_lane].dual_link >= 0) child_lane = static_cast<uint32_t>(child.ports[child_lane].dual_link);   // (the primary lane carries the traffic)
  const int first_in = allocate_hop(a, first_port, true, kPcieHop, kPcieHop);        // the adapter end of a PCIe path uses HopID 8 exactly
  if (first_in < 0) return false;
  // the HopID on the link
  Usb4Router& out_router = a_is_parent ? parent : child;
  Usb4Router& in_router = a_is_parent ? child : parent;
  const uint32_t out_lane = a_is_parent ? parent_lane : child_lane, in_lane = a_is_parent ? child_lane : parent_lane;
  int link_hop = -1;
  for (uint32_t h = kMinPathHop; h < kHopIdLimit && link_hop < 0; ++h) {
    if (h > out_router.ports[out_lane].max_out_hop || h > in_router.ports[in_lane].max_in_hop) break;
    if (!bit_test(out_router.ports[out_lane].out_hops, h) && !bit_test(in_router.ports[in_lane].in_hops, h)) link_hop = static_cast<int>(h);
  }
  const int last_out = allocate_hop(b, second_port, false, kPcieHop, kPcieHop);
  if (link_hop < 0 || last_out < 0) {
    release_hop(a, first_port, true, static_cast<uint32_t>(first_in));
    if (last_out >= 0) release_hop(b, second_port, false, static_cast<uint32_t>(last_out));
    return false;
  }
  bit_set(out_router.ports[out_lane].out_hops, static_cast<uint32_t>(link_hop));
  bit_set(in_router.ports[in_lane].in_hops, static_cast<uint32_t>(link_hop));
  path->length = 2;
  path->hops[0] = Usb4Hop{static_cast<int16_t>(first_router), static_cast<uint8_t>(first_port), static_cast<uint8_t>(out_lane), static_cast<uint16_t>(first_in), static_cast<uint16_t>(link_hop), 0};
  path->hops[1] = Usb4Hop{static_cast<int16_t>(second_router), static_cast<uint8_t>(in_lane), static_cast<uint8_t>(second_port), static_cast<uint16_t>(link_hop), static_cast<uint16_t>(last_out), 0};
  for (uint32_t i = 0; i < path->length; ++i) {
    const Usb4Router& router = *routers_[path->hops[i].router];
    const Usb4Port& in = router.ports[path->hops[i].in_port];
    path->hops[i].credits = in.is_lane() ? (in.bonded ? 2 * kPcieCredits : kPcieCredits) : 7;     // (no credit allocation: 16 on a lane adapter, 32 when bonded, 7 on a protocol adapter)
  }
  return true;
}

// One entry of a hop table (tb_path_activate). PCIe paths: flow control out of the source and internal hops and into every hop, no shared buffers, priority 3, weight 1.
bool Usb4Domain::write_hop(const Usb4Router& r, uint32_t port, uint32_t hop_index, const Usb4Hop& hop, bool first, bool last, const Usb4Path& path) {
  const uint8_t in_mask = first ? kPathSource : kPathInternal, out_mask = last ? kPathDestination : kPathInternal;
  const bool ingress_fc = (kPathAll & in_mask) != 0, egress_fc = ((kPathSource | kPathInternal) & out_mask) != 0;
  uint32_t words[2];
  words[0] = hop.out_hop | (static_cast<uint32_t>(hop.out_port) << 11) | (static_cast<uint32_t>(hop.credits) << 17) | (1u << 31);
  words[1] = path.weight | (static_cast<uint32_t>(path.priority) << 8) | (0x7FFu << 12) | (ingress_fc ? 1u << 24 : 0) | (egress_fc ? 1u << 25 : 0);   // (counter field all ones, counter disabled: what Linux writes)
  return write(r.route, port, TbConfigSpace::Hops, 2 * hop_index, ArrayView<const uint32_t>(words, 2));
}

// Disables a hop entry and waits for what it holds to drain (__tb_path_deactivate_hop).
bool Usb4Domain::disable_hop(const Usb4Router& r, uint32_t port, uint32_t hop_index) {
  uint32_t words[2];
  if (!read(r.route, port, TbConfigSpace::Hops, 2 * hop_index, ArrayView<uint32_t>(words, 2))) return false;
  if (!(words[0] & (1u << 31))) return true;
  words[0] &= ~(1u << 31);
  if (!write(r.route, port, TbConfigSpace::Hops, 2 * hop_index, ArrayView<const uint32_t>(words, 2))) return false;
  for (uint32_t waited = 0; waited < 500000; waited += 20) {
    if (!read(r.route, port, TbConfigSpace::Hops, 2 * hop_index, ArrayView<uint32_t>(words, 2))) return false;
    if (!(words[1] & (1u << 28))) {                       // pending
      words[1] &= ~((1u << 25) | (1u << 27));              // egress flow control and shared buffer off (the ingress ones are the adapter's own on USB4)
      return write(r.route, port, TbConfigSpace::Hops, 2 * hop_index, ArrayView<const uint32_t>(words, 2));
    }
    delay_us(20);
  }
  return false;
}

// From the last hop back to the first, so that nothing is sent into a hop that is not there yet.
bool Usb4Domain::activate_path(Usb4Path* path) {
  for (int i = static_cast<int>(path->length) - 1; i >= 0; --i) {
    const Usb4Hop& hop = path->hops[i];
    const Usb4Router& r = *routers_[hop.router];
    disable_hop(r, hop.in_port, hop.in_hop);              // (left over from before)
    if (!write_hop(r, hop.in_port, hop.in_hop, hop, i == 0, i == static_cast<int>(path->length) - 1, *path)) {
      for (uint32_t k = static_cast<uint32_t>(i) + 1; k < path->length; ++k) disable_hop(*routers_[path->hops[k].router], path->hops[k].in_port, path->hops[k].in_hop);
      return false;
    }
  }
  path->activated = true;
  return true;
}

void Usb4Domain::deactivate_path(Usb4Path* path) {
  if (!path->activated) return;
  for (uint32_t i = 0; i < path->length; ++i) {
    const Usb4Hop& hop = path->hops[i];
    if (hop.router >= 0 && routers_[hop.router]->present) disable_hop(*routers_[hop.router], hop.in_port, hop.in_hop);
  }
  path->activated = false;
}

bool Usb4Domain::set_adapter_enable(const Usb4Router& r, uint32_t port, Usb4TunnelKind kind, bool enable) {
  (void)kind;                                                       // (the enable bit is bit 31 of the first dword for both kinds)
  const Usb4Port& p = r.ports[port];
  if (p.cap_adapter == 0) return false;
  const uint32_t word = enable ? kAdpPcieCs0Pe : 0;
  return port_write(r, port, p.cap_adapter + kAdpPcieCs0, ArrayView<const uint32_t>(&word, 1));
}

bool Usb4Domain::set_pcie_enable(const Usb4Router& r, uint32_t port, bool enable) { return set_adapter_enable(r, port, Usb4TunnelKind::Pcie, enable); }

int Usb4Domain::find_tunnel(int router_index, Usb4TunnelKind kind) const {
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used && tunnels_[i].device_router == router_index && tunnels_[i].kind == kind) return static_cast<int>(i);
  return -1;
}

bool Usb4Domain::pcie_tunnelled(uint32_t router_index) const { return find_tunnel(static_cast<int>(router_index), Usb4TunnelKind::Pcie) >= 0; }
bool Usb4Domain::usb3_tunnelled(uint32_t router_index) const { return find_tunnel(static_cast<int>(router_index), Usb4TunnelKind::Usb3) >= 0; }
bool Usb4Domain::link_bonded(uint32_t router_index) const {
  const Usb4Router* r = router(router_index);
  return r && r->parent >= 0 && r->ports[r->upstream_port].bonded;
}

// The down adapter of `type` that belongs to a lane adapter of the host: the n-th down adapter for the n-th primary downstream lane adapter (usb4_switch_map_pcie_down and its USB 3 twin).
int Usb4Domain::port_pcie_down_for(const Usb4Router& host, uint32_t lane_port, uint32_t type) {
  uint32_t index = 0;
  for (uint32_t p = 1; p <= host.max_port; ++p) {
    const Usb4Port& port = host.ports[p];
    if (!port.is_lane() || (p == host.upstream_port && host.parent >= 0) || port.link_nr != 0) continue;
    if (p == lane_port) break;
    ++index;
  }
  uint32_t seen = 0;
  for (uint32_t p = 1; p <= host.max_port; ++p) {
    if (host.ports[p].type != type) continue;
    if (seen == index) return static_cast<int>(p);
    ++seen;
  }
  return -1;
}

// USB 3 bandwidth of the host router's down adapter (usb4_usb3_port_allocate_bandwidth): the adapter is asked for control (CMR, waiting for HCA), the bandwidth already consumed is read, and the
// larger of that and the wanted rate is written with a scale that makes it fit 12 bits; the request is withdrawn again.
bool Usb4Domain::usb3_allocate_bandwidth(const Usb4Router& host, uint32_t down_port, const Usb4Router& device, uint32_t up_port) {
  const Usb4Port& down = host.ports[down_port];
  const Usb4Port& up = device.ports[up_port];
  uint32_t speed_down = 0, speed_up = 0;
  if (!port_read(host, down_port, down.cap_adapter + kUsb3Cs4, ArrayView<uint32_t>(&speed_down, 1)) || !port_read(device, up_port, up.cap_adapter + kUsb3Cs4, ArrayView<uint32_t>(&speed_up, 1))) return false;
  const uint32_t rate_down = ((speed_down >> kUsb3Cs4MslrShift) & kUsb3Cs4MslrMask) == kUsb3Cs4Mslr20G ? 20000 : 10000;
  const uint32_t rate_up = ((speed_up >> kUsb3Cs4MslrShift) & kUsb3Cs4MslrMask) == kUsb3Cs4Mslr20G ? 20000 : 10000;
  const uint32_t wanted = (rate_down < rate_up ? rate_down : rate_up) * 90 / 100;
  auto set_request = [&](bool on) {
    uint32_t cs2 = 0;
    if (!port_read(host, down_port, down.cap_adapter + kUsb3Cs2, ArrayView<uint32_t>(&cs2, 1))) return false;
    cs2 = on ? (cs2 | kUsb3Cs2Cmr) : (cs2 & ~kUsb3Cs2Cmr);
    if (!port_write(host, down_port, down.cap_adapter + kUsb3Cs2, ArrayView<const uint32_t>(&cs2, 1))) return false;
    for (uint32_t waited = 0; waited < 1500000; waited += 1000) {
      uint32_t cs1 = 0;
      if (!port_read(host, down_port, down.cap_adapter + kUsb3Cs1, ArrayView<uint32_t>(&cs1, 1))) return false;
      if (((cs1 & kUsb3Cs1Hca) != 0) == on) return true;
      delay_us(1000);
    }
    return false;
  };
  if (!set_request(true)) return false;
  bool ok = false;
  uint32_t cs1 = 0, scale = 0;
  if (port_read(host, down_port, down.cap_adapter + kUsb3Cs1, ArrayView<uint32_t>(&cs1, 1)) && port_read(host, down_port, down.cap_adapter + kUsb3Cs3, ArrayView<uint32_t>(&scale, 1))) {
    scale &= 0x3F;
    auto to_mbps = [](uint32_t bw, uint32_t sc) { return static_cast<uint32_t>((static_cast<uint64_t>(bw) * 512 << sc) * 8000 / 1000000); };
    const uint32_t consumed_up = to_mbps(cs1 & 0xFFF, scale), consumed_down = to_mbps((cs1 >> 12) & 0xFFF, scale);
    const uint32_t up_bw = wanted > consumed_up ? wanted : consumed_up, down_bw = wanted > consumed_down ? wanted : consumed_down;
    const uint32_t max_bw = up_bw > down_bw ? up_bw : down_bw;
    auto to_bw = [](uint32_t mbps, uint32_t sc) { const uint64_t uframes = static_cast<uint64_t>(mbps) * 1000000 / 8000; const uint64_t unit = 512ull << sc; return static_cast<uint32_t>((uframes + unit - 1) / unit); };
    uint32_t new_scale = 0;
    while (new_scale < 64 && to_bw(max_bw, new_scale) >= 4096) ++new_scale;
    uint32_t cs2 = 0;
    if (new_scale < 64 && port_write(host, down_port, down.cap_adapter + kUsb3Cs3, ArrayView<const uint32_t>(&new_scale, 1)) &&
        port_read(host, down_port, down.cap_adapter + kUsb3Cs2, ArrayView<uint32_t>(&cs2, 1))) {
      cs2 = (cs2 & ~0x00FFFFFFu) | to_bw(up_bw, new_scale) | (to_bw(down_bw, new_scale) << 12);
      ok = port_write(host, down_port, down.cap_adapter + kUsb3Cs2, ArrayView<const uint32_t>(&cs2, 1));
    }
  }
  set_request(false);
  return ok;
}

bool Usb4Domain::remove_tunnel(int slot) {
  Usb4Tunnel& t = tunnels_[slot];
  // the adapters first (down, then up), then the paths; the device may be gone already
  if (t.host_router >= 0 && routers_[t.host_router]->present) set_adapter_enable(*routers_[t.host_router], t.down_port, t.kind, false);
  if (t.device_router >= 0 && routers_[t.device_router]->present) {
    set_adapter_enable(*routers_[t.device_router], t.up_port, t.kind, false);
    if (t.kind == Usb4TunnelKind::Pcie) routers_[t.device_router]->pcie_tunnel = false;
    else routers_[t.device_router]->usb3_tunnel = false;
  }
  deactivate_path(&t.down);
  deactivate_path(&t.up);
  release_path(&t.down);
  release_path(&t.up);
  t = Usb4Tunnel();
  return true;
}

bool Usb4Domain::make_tunnel(uint32_t router_index, Usb4TunnelKind kind, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  const bool pcie = kind == Usb4TunnelKind::Pcie;
  const uint32_t down_type = pcie ? kTypePcieDown : kTypeUsb3Down, up_type = pcie ? kTypePcieUp : kTypeUsb3Up;
  if (!started_ || router_index >= routers_.count() || !routers_[router_index]->present) {
    *error = "no such router";
    return false;
  }
  Usb4Router& device = *routers_[router_index];
  if (device.parent < 0) {
    *error = "the host router";
    return false;
  }
  if (find_tunnel(static_cast<int>(router_index), kind) >= 0) {
    *error = "already tunnelled";
    return false;
  }
  Usb4Router& host = *routers_[device.parent];
  if (!host.present) {
    *error = "its parent is gone";
    return false;
  }
  // the parent must be tunnelled to its own parent first (the device's link hangs below the parent's bridge or hub)
  if (host.parent >= 0 && find_tunnel(device.parent, kind) < 0) {
    *error = "the router above it has no such tunnel yet";
    return false;
  }
  int up = -1;
  for (uint32_t p = 1; p <= device.max_port && up < 0; ++p)
    if (device.ports[p].type == up_type && device.ports[p].cap_adapter != 0) up = static_cast<int>(p);
  if (up < 0) {
    *error = pcie ? "the router has no PCIe up adapter" : "the router has no USB 3 up adapter";
    return false;
  }
  int down = port_pcie_down_for(host, device.parent_port, down_type);
  auto in_use = [&](int port) {
    for (uint32_t i = 0; i < kMaxTunnels; ++i)
      if (tunnels_[i].used && tunnels_[i].kind == kind && tunnels_[i].host_router == device.parent && tunnels_[i].down_port == port) return true;
    uint32_t word = 0;
    return !port_read(host, static_cast<uint32_t>(port), host.ports[port].cap_adapter + kAdpPcieCs0, ArrayView<uint32_t>(&word, 1)) || (word & kAdpPcieCs0Pe) != 0;
  };
  if (down < 0 || host.ports[down].cap_adapter == 0 || in_use(down)) {
    down = -1;
    for (uint32_t p = 1; p <= host.max_port && down < 0; ++p)
      if (host.ports[p].type == down_type && host.ports[p].cap_adapter != 0 && !in_use(static_cast<int>(p))) down = static_cast<int>(p);
  }
  if (down < 0) {
    *error = pcie ? "no free PCIe down adapter" : "no free USB 3 down adapter";
    return false;
  }
  int slot = -1;
  for (uint32_t i = 0; i < kMaxTunnels && slot < 0; ++i)
    if (!tunnels_[i].used) slot = static_cast<int>(i);
  if (slot < 0) {
    *error = "too many tunnels";
    return false;
  }
  Usb4Tunnel& t = tunnels_[slot];
  t = Usb4Tunnel();
  t.used = true;
  t.kind = kind;
  t.device_router = static_cast<int16_t>(router_index);
  t.host_router = device.parent;
  t.down_port = static_cast<uint8_t>(down);
  t.up_port = static_cast<uint8_t>(up);
  auto fail = [&](const char* why) {
    remove_tunnel(slot);
    *error = why;
    return false;
  };
  if (!build_path(&t.down, device.parent, static_cast<uint32_t>(down), static_cast<int>(router_index), static_cast<uint32_t>(up), kind) ||
      !build_path(&t.up, static_cast<int>(router_index), static_cast<uint32_t>(up), device.parent, static_cast<uint32_t>(down), kind))
    return fail("no HopIDs left for the paths");
  // USB 3 over the host router's adapter has its bandwidth allocated first
  if (!pcie && host.parent < 0 && !usb3_allocate_bandwidth(host, static_cast<uint32_t>(down), device, static_cast<uint32_t>(up))) return fail("the USB 3 bandwidth could not be allocated");
  if (!activate_path(&t.down) || !activate_path(&t.up)) return fail("the paths could not be written");
  // PCIe: the far adapter first; USB 3: the near one first (tb_pci_activate / tb_usb3_activate)
  if (pcie) {
    if (!set_adapter_enable(device, static_cast<uint32_t>(up), kind, true) || !set_adapter_enable(host, static_cast<uint32_t>(down), kind, true)) return fail("the adapters could not be enabled");
  } else {
    if (!set_adapter_enable(host, static_cast<uint32_t>(down), kind, true) || !set_adapter_enable(device, static_cast<uint32_t>(up), kind, true)) return fail("the adapters could not be enabled");
  }
  t.active = true;
  if (pcie) device.pcie_tunnel = true;
  else device.usb3_tunnel = true;
  return true;
}

bool Usb4Domain::approve_pcie(uint32_t router_index, const char** error) { return make_tunnel(router_index, Usb4TunnelKind::Pcie, error); }
bool Usb4Domain::approve_usb3(uint32_t router_index, const char** error) { return make_tunnel(router_index, Usb4TunnelKind::Usb3, error); }

bool Usb4Domain::disconnect_pcie(uint32_t router_index) {
  const int slot = find_tunnel(static_cast<int>(router_index), Usb4TunnelKind::Pcie);
  return slot >= 0 && remove_tunnel(slot);
}

bool Usb4Domain::disconnect_usb3(uint32_t router_index) {
  const int slot = find_tunnel(static_cast<int>(router_index), Usb4TunnelKind::Usb3);
  return slot >= 0 && remove_tunnel(slot);
}

// ------------------------------------------------------------------ lane bonding

int Usb4Domain::lane_width(const Usb4Router& r, uint32_t port) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  if (p.cap_phy == 0 || !port_read(r, port, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return -1;
  return static_cast<int>((word & kLaneCs1CurrentWidthMask) >> kLaneCs1CurrentWidthShift);
}

bool Usb4Domain::set_lane_width(const Usb4Router& r, uint32_t port, uint32_t target) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  if (p.cap_phy == 0 || !port_read(r, port, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return false;
  word = (word & ~kLaneCs1TargetWidthMask) | target;
  return port_write(r, port, p.cap_phy + kLaneCs1, ArrayView<const uint32_t>(&word, 1));
}

// Bonds the two lanes of the link between `parent`'s downstream adapter and `child`'s upstream one (tb_switch_lane_bonding_enable, USB4 generation 3 links): when both ends can (the
// upstream USB4 port says BE, both lane adapters support two lanes) each end's lane 0 and lane 1 adapters are set to the dual width, the downstream end is told to bond, and the link must
// report two lanes within 100 ms. Anything short of that leaves the link as it was.
bool Usb4Domain::bond_link(Usb4Router& child, Usb4Router& parent, uint32_t down_port) {
  Usb4Port& up = child.ports[child.upstream_port];
  Usb4Port& down = parent.ports[down_port];
  if (up.dual_link < 0 || down.dual_link < 0 || up.cap_usb4 == 0) return false;
  uint32_t cs18 = 0;
  if (!port_read(child, child.upstream_port, up.cap_usb4 + kPortCs18, ArrayView<uint32_t>(&cs18, 1)) || !(cs18 & kPortCs18Be)) return false;
  for (int end = 0; end < 2; ++end) {
    const Usb4Router& r = end == 0 ? child : parent;
    const uint32_t lane0 = end == 0 ? child.upstream_port : down_port;
    uint32_t word = 0;
    const Usb4Port& p = r.ports[lane0];
    if (p.cap_phy == 0 || !port_read(r, lane0, p.cap_phy + kLaneCs0, ArrayView<uint32_t>(&word, 1))) return false;
    if (((word >> kLaneCs0WidthShift) & kLaneCs0WidthMask) != kLaneCs0WidthDual) return false;     // (does not support two lanes)
  }
  if (!port_up(parent, static_cast<uint32_t>(down.dual_link))) return false;
  // each end, upstream first: lane 0 to dual, lane 1 to dual, and the downstream end's lane 0 is told to bond
  struct End { Usb4Router* router; uint32_t lane0; bool upstream; };
  const End ends[2] = {{&child, child.upstream_port, true}, {&parent, down_port, false}};
  bool done[2] = {false, false};
  auto undo = [&]() {
    for (int i = 0; i < 2; ++i) {
      if (!done[i]) continue;
      const Usb4Router& r = *ends[i].router;
      set_lane_width(r, ends[i].lane0, kLaneCs1TargetSingle);
      set_lane_width(r, static_cast<uint32_t>(r.ports[ends[i].lane0].dual_link), kLaneCs1TargetSingle);
    }
  };
  for (int i = 0; i < 2; ++i) {
    const End& e = ends[i];
    const uint32_t lane1 = static_cast<uint32_t>(e.router->ports[e.lane0].dual_link);
    if (lane_width(*e.router, e.lane0) == 1 && !set_lane_width(*e.router, e.lane0, kLaneCs1TargetDual)) { undo(); return false; }
    const int width1 = lane_width(*e.router, lane1);
    if (width1 == 1 && !set_lane_width(*e.router, lane1, kLaneCs1TargetDual)) { undo(); return false; }
    if (width1 == 1 && !e.upstream) {
      const Usb4Port& p = e.router->ports[e.lane0];
      uint32_t word = 0;
      if (!port_read(*e.router, e.lane0, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) { undo(); return false; }
      word |= kLaneCs1Lb;
      if (!port_write(*e.router, e.lane0, p.cap_phy + kLaneCs1, ArrayView<const uint32_t>(&word, 1))) { undo(); return false; }
    }
    done[i] = true;
  }
  bool dual = false;
  for (uint32_t waited = 0; waited < 100000 && !dual; waited += 1000) {
    dual = lane_width(parent, down_port) == static_cast<int>(kLaneCs0WidthDual);
    if (!dual) delay_us(1000);
  }
  if (!dual) { undo(); return false; }
  up.bonded = down.bonded = true;
  child.ports[up.dual_link].bonded = parent.ports[down.dual_link].bonded = true;
  return true;
}

}  // namespace hardware
