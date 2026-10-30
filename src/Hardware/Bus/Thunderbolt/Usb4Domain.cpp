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
constexpr uint8_t kErrorInvalidConfig = 2, kErrorNoSuchPort = 4, kErrorAckPlugEvent = 7;

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
bool Usb4Domain::read_router(uint64_t route, int16_t parent, uint8_t parent_port, int* index_out, const char** error, bool* silent) {
  const char* local = nullptr;
  if (!error) error = &local;
  uint32_t header[5];
  uint8_t upstream = 0;
  if (!read(route, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(header, 5), &upstream)) {
    *error = "the router does not answer";
    if (silent) *silent = true;                              // (nothing there to be a router: another host, perhaps)
    return false;
  }
  const uint32_t depth = route_length(route);
  const uint8_t version = static_cast<uint8_t>((header[4] >> 24) >> 5 & 0x7);   // (the USB4 major version, zero for Thunderbolt 3)
  if (depth > kMaxDepth) {
    *error = "too deep";
    return false;
  }
  // (a Thunderbolt 3 router has no USB4 version: it is run through its vendor specific capabilities, and must have the plug events one)
  bool tbt3 = false;
  int plug_cap = -1, lc_cap = -1;
  if (version == 0) {
    Usb4Router probe;
    probe.route = route;
    plug_cap = find_router_vse(probe, kVsecPlugEvents);
    lc_cap = find_router_vse(probe, kVsecLinkController);
    if (plug_cap <= 0 || (header[0] & 0xFFFF) != 0x8086) {
      *error = "a Thunderbolt 3 router without the plug events capability, or not Intel's";
      return false;
    }
    tbt3 = true;
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
  r.tbt3 = tbt3;
  r.cap_plug_events = tbt3 ? static_cast<uint8_t>(plug_cap) : 0;
  r.cap_lc = tbt3 && lc_cap > 0 ? static_cast<uint8_t>(lc_cap) : 0;
  r.parent = parent;
  r.parent_port = parent_port;
  // configure it (tb_switch_configure): its route, depth, upstream port, enabled; the connection manager's version and the pause between plug events
  uint32_t config[4];
  config[0] = (header[1] & ~((0x3Fu << 8) | (0x7u << 20))) | (static_cast<uint32_t>(upstream) << 8) | (depth << 20);
  config[1] = static_cast<uint32_t>(route);
  config[2] = (static_cast<uint32_t>(route >> 32) & 0x3FFFFF) | (1u << 31);
  config[3] = (header[4] & 0xFFFF0000u) | 0xA | ((version < 2 ? 0x10u : 0x20u) << 8);
  // (the Thunderbolt 3 kind has no connection manager version word: three words, then the plug events)
  if (!write(route, 0, TbConfigSpace::Switch, kRouterCs1, ArrayView<const uint32_t>(config, tbt3 ? 3 : 4)) || (tbt3 && !plug_events_active(r))) {
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
  {
    const int tmu = find_router_cap(r, kSwitchCapTmu);
    if (tmu > 0) r.cap_tmu = static_cast<uint8_t>(tmu);
  }
  read_buffer_allocation(r);
  read_drom(r);                                            // (before the default pairing, which leaves what it says alone)
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
    port.total_credits = static_cast<uint16_t>((h[4] >> 20) & 0x3FF);
    const uint8_t first = static_cast<uint8_t>(h[1] & 0xFF);
    if (port.type == kTypePort) {
      const int phy = find_port_cap(r, p, first, kPortCapPhy);
      if (phy > 0) port.cap_phy = static_cast<uint8_t>(phy);
      const int u4 = find_port_cap(r, p, first, kPortCapUsb4);
      const int time1 = find_port_cap(r, p, first, kPortCapTime1);
      if (time1 > 0) port.cap_tmu = static_cast<uint8_t>(time1);
      if (u4 > 0) {
        port.cap_usb4 = static_cast<uint8_t>(u4);
        uint32_t hop0[2];                                   // (the buffers the control path uses are in its hop entry)
        if (read(r.route, p, TbConfigSpace::Hops, 0, ArrayView<uint32_t>(hop0, 2))) port.ctl_credits = static_cast<uint16_t>((hop0[0] >> 17) & 0x7F);
      }
      if (port.ctl_credits == 0) port.ctl_credits = 2;
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

bool Usb4Domain::configure_port(const Usb4Router& r, uint32_t port, bool upstream) {
  if (r.tbt3) return lc_set_configured(r, port, upstream);
  const Usb4Port& p = r.ports[port];
  if (p.cap_usb4 == 0) return false;
  uint32_t word = 0;
  if (!port_read(r, port, p.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1))) return false;
  word |= kPortCs19Pc;
  return port_write(r, port, p.cap_usb4 + kPortCs19, ArrayView<const uint32_t>(&word, 1));
}

// The link controller's registers of the physical port a lane adapter belongs to (find_port_lc_cap): its descriptor says where the per-port blocks start and how big they are.
bool Usb4Domain::lc_set_configured(const Usb4Router& r, uint32_t port, bool upstream) {
  if (r.cap_lc == 0) return false;
  uint32_t desc = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_lc + kLcDesc, ArrayView<uint32_t>(&desc, 1))) return false;
  const uint32_t start = (desc >> 8) & 0xFF, size = (desc >> 16) & 0xFFF, phys = (port - 1) / 2;
  const uint32_t cap = r.cap_lc + start + phys * size;
  if (cap + kLcSxCtrl >= 0x1FFF) return false;
  uint32_t ctrl = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, cap + kLcSxCtrl, ArrayView<uint32_t>(&ctrl, 1))) return false;
  ctrl |= (port % 2) ? kLcSxCtrlL1c : kLcSxCtrlL2c;
  if (upstream) ctrl |= kLcSxCtrlUpstream;
  return write(r.route, 0, TbConfigSpace::Switch, cap + kLcSxCtrl, ArrayView<const uint32_t>(&ctrl, 1));
}

bool Usb4Domain::bonding_possible(const Usb4Router& child) {
  uint32_t word = 0;
  if (!child.tbt3) {
    const Usb4Port& up = child.ports[child.upstream_port];
    return up.cap_usb4 != 0 && port_read(child, child.upstream_port, up.cap_usb4 + kPortCs18, ArrayView<uint32_t>(&word, 1)) && (word & kPortCs18Be);
  }
  if (child.cap_lc == 0) return false;
  uint32_t desc = 0;
  if (!read(child.route, 0, TbConfigSpace::Switch, child.cap_lc + kLcDesc, ArrayView<uint32_t>(&desc, 1))) return false;
  const uint32_t cap = child.cap_lc + ((desc >> 8) & 0xFF) + ((child.upstream_port - 1) / 2) * ((desc >> 16) & 0xFFF);
  return cap + kLcPortAttr < 0x1FFF && read(child.route, 0, TbConfigSpace::Switch, cap + kLcPortAttr, ArrayView<uint32_t>(&word, 1)) && (word & kLcPortAttrBe);
}

bool Usb4Domain::configure_link(Usb4Router& child, Usb4Router& parent, uint32_t down_port) {
  // both ends of the link are marked configured (usb4_port_configure: PORT_CS_19.PC; the link controller for a Thunderbolt 3 router)
  return configure_port(child, child.upstream_port, true) && configure_port(parent, down_port, false);
}

// usb4_switch_setup: CNS cleared (this connection manager does Thunderbolt 3 tunnelling too), PCIe tunnelling allowed when the parent has a PCIe down adapter (and the xHCI over it).
bool Usb4Domain::switch_setup(Usb4Router& child, const Usb4Router& parent) {
  uint32_t cs6 = 0, cs5 = 0;
  if (child.tbt3) return true;                              // (nothing of this for a Thunderbolt 3 router)
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
  if (r.tbt3) return true;
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

// ------------------------------------------------------------------ router operations and the DROM

bool Usb4Domain::router_op(const Usb4Router& r, uint32_t opcode, uint32_t* metadata, uint8_t* status, const uint32_t* tx, uint32_t tx_dwords, uint32_t* rx, uint32_t rx_dwords) {
  if (tx_dwords > kOpDataDwords || rx_dwords > kOpDataDwords) return false;
  if (metadata && !write(r.route, 0, TbConfigSpace::Switch, kRouterCs25, ArrayView<const uint32_t>(metadata, 1))) return false;
  if (tx_dwords && !write(r.route, 0, TbConfigSpace::Switch, kRouterCs9, ArrayView<const uint32_t>(tx, tx_dwords))) return false;
  op_unsupported_ = false;
  uint32_t word = opcode | kCs26Ov;
  if (!write(r.route, 0, TbConfigSpace::Switch, kRouterCs26, ArrayView<const uint32_t>(&word, 1))) return false;
  bool done = false;
  for (uint32_t waited = 0; waited < 500000 && !done; waited += 1000) {
    if (!read(r.route, 0, TbConfigSpace::Switch, kRouterCs26, ArrayView<uint32_t>(&word, 1))) return false;
    done = (word & kCs26Ov) == 0;
    if (!done) delay_us(1000);
  }
  op_unsupported_ = done && (word & kCs26Ons);
  if (!done || (word & kCs26Ons)) return false;
  if (status) *status = static_cast<uint8_t>((word & kCs26StatusMask) >> kCs26StatusShift);
  if (metadata && !read(r.route, 0, TbConfigSpace::Switch, kRouterCs25, ArrayView<uint32_t>(metadata, 1))) return false;
  if (rx_dwords && !read(r.route, 0, TbConfigSpace::Switch, kRouterCs9, ArrayView<uint32_t>(rx, rx_dwords))) return false;
  return true;
}

// DROM_READ in blocks of up to 16 dwords, the address and the length in the metadata (usb4_switch_drom_read_block, and tb_nvm_read_data's handling of unaligned starts).
bool Usb4Domain::drom_read(const Usb4Router& r, uint32_t address, ByteView out) {
  uint64_t done = 0;
  while (done < out.size()) {
    const uint32_t offset = address & 3;
    const uint64_t want = out.size() - done;
    const uint32_t nbytes = static_cast<uint32_t>(want + offset < 64 ? want + offset : 64);
    const uint32_t dwords = (nbytes + 3) / 4, dwaddress = address / 4;
    if (dwaddress > 0x1FFF) return false;
    uint32_t block[kOpDataDwords];
    bool ok = false;
    for (uint32_t attempt = 0; attempt < 3 && !ok; ++attempt) {
      uint32_t metadata = ((dwords << 15) & 0xF8000u) | ((dwaddress << 2) & 0x7FFCu);
      uint8_t status = 0;
      ok = router_op(r, kOpDromRead, &metadata, &status, nullptr, 0, block, dwords) && status == 0;
    }
    if (!ok) return false;
    uint8_t bytes[64];
    for (uint32_t i = 0; i < dwords; ++i)
      for (uint32_t k = 0; k < 4; ++k) bytes[i * 4 + k] = static_cast<uint8_t>(block[i] >> (8 * k));
    const uint32_t take = nbytes - offset;
    for (uint32_t i = 0; i < take; ++i) out[done + i] = bytes[offset + i];
    done += take;
    address += take;
  }
  return true;
}

// The router's preferred buffer allocation (usb4_switch_credits_init): up to 16 (index, value) entries; the preferences are used only if they are complete for the adapters the router has.
bool Usb4Domain::read_buffer_allocation(Usb4Router& r) {
  r.credit_allocation = false;
  uint32_t data[kOpDataDwords] = {};
  uint32_t metadata = 0;
  uint8_t status = 0;
  if (!router_op(r, kOpBufferAlloc, &metadata, &status, nullptr, 0, data, kOpDataDwords) || status != 0) return false;
  const uint32_t length = metadata & 0xFF;
  if (length > kOpDataDwords) return false;
  int usb3 = -1, aux = -1, main = -1, pcie = -1, dma = -1;
  for (uint32_t i = 0; i < length; ++i) {
    const uint32_t index = data[i] & 0xFFFF, value = data[i] >> 16;
    if (index == 1) usb3 = static_cast<int>(value);
    else if (index == 2) aux = static_cast<int>(value);
    else if (index == 3) main = static_cast<int>(value);
    else if (index == 4) pcie = static_cast<int>(value);
    else if (index == 5) dma = static_cast<int>(value);
  }
  if (r.parent < 0 && dma < 0) return false;                // (the host router must say how much the host interface may have)
  uint32_t lanes = 0;
  for (uint32_t p = 1; p <= r.max_port; ++p) lanes += r.ports[p].is_lane();
  if (lanes > 2 && (aux < 0 || main < 0)) return false;
  for (uint32_t p = 1; p <= r.max_port; ++p) {
    const uint32_t type = r.ports[p].type;
    if (type == kTypeDpOut && main < 0) return false;
    if ((type == kTypeDpIn || type == kTypeDpOut) && aux < 0) return false;
    if ((type == kTypeUsb3Down || type == kTypeUsb3Up) && usb3 < 0) return false;
    if ((type == kTypePcieDown || type == kTypePcieUp) && pcie < 0) return false;
  }
  r.credit_allocation = true;
  r.max_usb3_credits = usb3 > 0 ? static_cast<uint16_t>(usb3) : 0;
  r.min_dp_aux_credits = aux > 0 ? static_cast<uint16_t>(aux) : 0;
  r.min_dp_main_credits = main > 0 ? static_cast<uint16_t>(main) : 0;
  r.max_pcie_credits = pcie > 0 ? static_cast<uint16_t>(pcie) : 0;
  r.max_dma_credits = dma > 0 ? static_cast<uint16_t>(dma) : 0;
  return true;
}

// What a lane adapter has for PCIe and DMA paths: its usable buffers (the total less the control path's) less the USB 3 share. (No DisplayPort streams are reserved for: none are made here.)
uint32_t Usb4Domain::available_credits(const Usb4Router& r, const Usb4Port& lane) const {
  const int usable = static_cast<int>(lane.total_credits) - static_cast<int>(lane.ctl_credits);
  const int credits = usable - r.max_usb3_credits;
  return credits > 0 ? static_cast<uint32_t>(credits) : 0;
}

// ---- the EEPROM of a Thunderbolt 3 router (eeprom.c): its DROM is read by driving the serial flash's pins from the plug events capability, one bit at a time.

bool Usb4Domain::eeprom_ctl(const Usb4Router& r, uint32_t* word, bool write_it) {
  if (write_it) return write(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + kPlugEventsEepromCtl, ArrayView<const uint32_t>(word, 1));
  return read(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + kPlugEventsEepromCtl, ArrayView<uint32_t>(word, 1));
}

// Access on: bit banging enabled and the chip selected; off: deselected first, then bit banging off (the controller does not probe again otherwise).
bool Usb4Domain::eeprom_active(const Usb4Router& r, bool enable) {
  uint32_t ctl = 0;
  if (!eeprom_ctl(r, &ctl, false)) return false;
  if (enable) {
    ctl |= kEepromBitBang;
    if (!eeprom_ctl(r, &ctl, true)) return false;
    ctl &= ~kEepromCs;
    return eeprom_ctl(r, &ctl, true);
  }
  ctl |= kEepromCs;
  if (!eeprom_ctl(r, &ctl, true)) return false;
  ctl &= ~kEepromBitBang;
  return eeprom_ctl(r, &ctl, true);
}

bool Usb4Domain::eeprom_out(const Usb4Router& r, uint8_t value) {
  uint32_t ctl = 0;
  if (!eeprom_ctl(r, &ctl, false)) return false;
  for (uint32_t i = 0; i < 8; ++i) {
    ctl = (value & 0x80) ? (ctl | kEepromDi) : (ctl & ~kEepromDi);
    if (!eeprom_ctl(r, &ctl, true)) return false;           // (the bit first, then the clock up and down)
    ctl |= kEepromSk;
    if (!eeprom_ctl(r, &ctl, true)) return false;
    ctl &= ~kEepromSk;
    if (!eeprom_ctl(r, &ctl, true)) return false;
    value = static_cast<uint8_t>(value << 1);
  }
  return true;
}

bool Usb4Domain::eeprom_in(const Usb4Router& r, uint8_t* value) {
  uint32_t ctl = 0;
  if (!eeprom_ctl(r, &ctl, false)) return false;
  *value = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    ctl |= kEepromSk;
    if (!eeprom_ctl(r, &ctl, true) || !eeprom_ctl(r, &ctl, false)) return false;       // (the chip's bit is there with the clock up)
    *value = static_cast<uint8_t>((*value << 1) | ((ctl & kEepromDo) ? 1 : 0));
    ctl &= ~kEepromSk;
    if (!eeprom_ctl(r, &ctl, true)) return false;
  }
  return true;
}

// The read command (3) and the 16 bit address, then the bytes; the DROM starts at the offset the plug events capability names.
bool Usb4Domain::eeprom_read_n(const Usb4Router& r, uint32_t offset, uint8_t* out, uint32_t count) {
  if (r.cap_plug_events == 0) return false;
  uint32_t ctl = 0, drom_offset = 0;
  if (!eeprom_ctl(r, &ctl, false) || !(ctl & kEepromPresent) || (ctl & kEepromNotPresent)) return false;
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + kPlugEventsDromOffset, ArrayView<uint32_t>(&drom_offset, 1)) || drom_offset > 0xFFFF) return false;
  offset += drom_offset;
  if (offset > 0xFFFF) return false;
  if (!eeprom_active(r, true)) return false;
  bool ok = eeprom_out(r, 3) && eeprom_out(r, static_cast<uint8_t>(offset >> 8)) && eeprom_out(r, static_cast<uint8_t>(offset));
  for (uint32_t i = 0; ok && i < count; ++i) ok = eeprom_in(r, out + i);
  return eeprom_active(r, false) && ok;
}

static uint8_t tb_crc8(const uint8_t* data, uint32_t length) {
  uint8_t value = 0xFF;
  for (uint32_t i = 0; i < length; ++i) {
    value ^= data[i];
    for (uint32_t j = 0; j < 8; ++j) value = static_cast<uint8_t>((value << 1) ^ ((value & 0x80) ? 7 : 0));
  }
  return value;
}

bool Usb4Domain::read_drom(Usb4Router& r) {
  r.drom = false;
  uint8_t size_bytes[2];
  uint8_t rom[4096];
  uint32_t total;
  if (r.tbt3) {
    if (!eeprom_read_n(r, 14, size_bytes, 2)) return false;
    total = ((size_bytes[0] | (uint32_t{size_bytes[1]} << 8)) & 0x3FF) + 13;
    if (total < 22 || total > sizeof rom || !eeprom_read_n(r, 0, rom, total)) return false;
  } else {
    if (!drom_read(r, 14, ByteView(size_bytes, 2))) return false;
    total = ((size_bytes[0] | (uint32_t{size_bytes[1]} << 8)) & 0xFFF) + 13;
    if (total < 16 || total > sizeof rom || !drom_read(r, 0, ByteView(rom, total))) return false;
  }
  // revision 3 is the USB4 layout (16 byte header, the CRC32 not enforced); anything else is read as the Thunderbolt layout (22 byte header, the unique id's CRC8 has to match)
  uint32_t pos = 16;
  if (rom[13] != 3) {
    if (total < 22 || tb_crc8(rom + 1, 8) != rom[0]) return false;
    pos = 22;
  }
  while (pos < total) {
    const uint8_t len = rom[pos];
    if (pos + 1 == total || len == 0 || pos + len > total || len < 2) return false;     // (an entry that runs past the end)
    const uint8_t index = rom[pos + 1] & 0x3F;
    const bool disabled = (rom[pos + 1] & 0x40) != 0, port_entry = (rom[pos + 1] & 0x80) != 0;
    if (!port_entry) {
      if (index == 1 || index == 2) {
        char* name = index == 1 ? r.vendor_name : r.device_name;
        uint32_t n = 0;
        for (; n + 2 < len && n + 1 < sizeof r.vendor_name && rom[pos + 2 + n] != 0; ++n) name[n] = static_cast<char>(rom[pos + 2 + n]);
        name[n] = 0;
      }
    } else if (index >= 1 && index <= r.max_port) {
      Usb4Port& port = r.ports[index];
      port.disabled = disabled;
      if (!disabled && port.type == kTypePort) {
        if (len != 8) return false;
        port.link_nr = (rom[pos + 2] >> 4) & 1;
        if (rom[pos + 2] & 0x80) {
          const uint32_t other = rom[pos + 3] & 0x3F;
          if (other >= 1 && other <= r.max_port && other != index) port.dual_link = static_cast<int8_t>(other);
        }
      }
    }
    pos += len;
  }
  r.drom = true;
  return true;
}

// ------------------------------------------------------------------ Thunderbolt 3 routers

// A vendor specific capability of the router's space (tb_switch_find_vse_cap): capability id 5, the VSEC id in byte 2; the short form has a length in byte 3 and the next offset in byte 0,
// the long form (length zero) has the next offset in the upper half of the second dword.
int Usb4Domain::find_router_vse(const Usb4Router& r, uint8_t vsec) {
  uint32_t header = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, 1, ArrayView<uint32_t>(&header, 1))) return -1;
  uint32_t offset = header & 0xFF;
  for (uint32_t i = 0; i < 64 && offset != 0 && offset < 0x1F00; ++i) {
    uint32_t word = 0;
    if (!read(r.route, 0, TbConfigSpace::Switch, offset, ArrayView<uint32_t>(&word, 1))) return -1;
    const uint32_t cap = (word >> 8) & 0xFF;
    uint32_t next = word & 0xFF;
    if (cap == kSwitchCapVse) {
      if (((word >> 16) & 0xFF) == vsec) return static_cast<int>(offset);
      if (((word >> 24) & 0xFF) == 0) {                      // the long form
        uint32_t second = 0;
        if (!read(r.route, 0, TbConfigSpace::Switch, offset + 1, ArrayView<uint32_t>(&second, 1))) return -1;
        next = second & 0xFFFF;
      }
    }
    offset = next;
  }
  return -1;
}

// tb_plug_events_active(true) for the Thunderbolt 3 kind: the pause between events set to its longest, and the USB hot plug event disabled (Alpine Ridge needs it for its xHCI).
bool Usb4Domain::plug_events_active(const Usb4Router& r) {
  uint32_t word = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, 4, ArrayView<uint32_t>(&word, 1))) return false;
  word = (word & ~0xFFu) | 0xFF;
  if (!write(r.route, 0, TbConfigSpace::Switch, 4, ArrayView<const uint32_t>(&word, 1))) return false;
  uint32_t events = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + 1, ArrayView<uint32_t>(&events, 1))) return false;
  events &= 0xFFFFFF83u;
  const uint16_t id = r.device_id;
  const bool alpine = r.vendor_id == 0x8086 && (id == 0x1576 || id == 0x1578 || id == 0x15C0 || id == 0x15D3 || id == 0x15DA);
  if (!alpine) events |= kPlugEventsUsbDisable;
  return write(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + 1, ArrayView<const uint32_t>(&events, 1));
}

// The link controller block of a lane adapter's physical port (find_port_lc_cap).
bool Usb4Domain::lc_port_block(const Usb4Router& r, uint32_t port, uint32_t* cap) {
  if (r.cap_lc == 0) return false;
  uint32_t desc = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_lc + kLcDesc, ArrayView<uint32_t>(&desc, 1))) return false;
  *cap = r.cap_lc + ((desc >> 8) & 0xFF) + ((port - 1) / 2) * ((desc >> 16) & 0xFFF);
  return *cap + kLcLinkReq < 0x1F00;
}

// tb_switch_pcie_bridge_write: a write to one of the router's PCIe bridges through the plug events capability, acknowledged within 100 ms.
bool Usb4Domain::pcie_bridge_write(const Usb4Router& r, uint32_t bridge, uint32_t offset, uint32_t value) {
  if (r.cap_plug_events == 0) return false;
  if (!write(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + kPlugEventsPcieWrData, ArrayView<const uint32_t>(&value, 1))) return false;
  const uint32_t command = (offset & 0x3FF) | (1u << (bridge + kPcieCmdBridgeShift)) | kPcieCmdWrite | kPcieCmdValue | kPcieCmdReqAck;
  if (!write(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + kPlugEventsPcieCmd, ArrayView<const uint32_t>(&command, 1))) return false;
  for (uint32_t waited = 0; waited < 100000; waited += 1000) {
    uint32_t word = 0;
    if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_plug_events + kPlugEventsPcieCmd, ArrayView<uint32_t>(&word, 1))) return false;
    if (!(word & kPcieCmdReqAck)) return !(word & kPcieCmdTimeout);
    delay_us(1000);
  }
  return false;
}

// What Linux does for a Thunderbolt 3 router once its PCIe tunnel is up: a Titan Ridge one directly below the host gets the PCIe L1 setting of its two bridges, and the xHCI of the router is
// connected to the link controller of ports 1 and 3 (Titan Ridge: both; Alpine Ridge: those with a USB device plugged in). A failure is not an error of the tunnel.
void Usb4Domain::tbt3_after_pcie(const Usb4Router& host, const Usb4Router& device) {
  if (!device.tbt3 || device.vendor_id != 0x8086) return;
  const bool titan = device.device_id == 0x15E7 || device.device_id == 0x15EA || device.device_id == 0x15EF;
  const bool alpine = device.device_id == 0x1576 || device.device_id == 0x1578 || device.device_id == 0x15C0 || device.device_id == 0x15D3 || device.device_id == 0x15DA;
  if (titan && host.parent < 0) {
    if (pcie_bridge_write(device, 5, 0x143, 0x0C7806B1u)) pcie_bridge_write(device, 0, 0x143, 0x0C5806B1u);
  }
  if (!titan && !alpine) return;
  const uint32_t ports[2] = {1, 3};
  for (uint32_t port : ports) {
    if (port > device.max_port) continue;
    uint32_t cap = 0, word = 0;
    if (!lc_port_block(device, port, &cap)) continue;
    if (alpine) {
      uint32_t status = 0;
      if (!read(device.route, 0, TbConfigSpace::Switch, cap + kLcCs42, ArrayView<uint32_t>(&status, 1)) || !(status & kLcCs42UsbPlugged)) continue;
    }
    if (!read(device.route, 0, TbConfigSpace::Switch, cap + kLcLinkReq, ArrayView<uint32_t>(&word, 1)) || (word & kLcLinkReqXhciConnect)) continue;
    word |= kLcLinkReqXhciConnect;
    write(device.route, 0, TbConfigSpace::Switch, cap + kLcLinkReq, ArrayView<const uint32_t>(&word, 1));
  }
}

// tb_lc_set_wake and tb_lc_set_sleep: in each link controller's block, the wake bits (all of them for a runtime sleep: connect and disconnect, USB4, PCIe, DisplayPort; none for a system sleep, and
// none at all for the host router) and then the sleep bit.
bool Usb4Domain::lc_sleep(const Usb4Router& r, bool runtime) {
  if (r.cap_lc == 0) return false;
  uint32_t desc = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_lc + kLcDesc, ArrayView<uint32_t>(&desc, 1))) return false;
  const uint32_t count = desc & 0xF, start = (desc >> 8) & 0xFF, size = (desc >> 16) & 0xFFF;
  for (uint32_t i = 0; i < count && i < 8; ++i) {
    const uint32_t cap = r.cap_lc + start + i * size;
    if (cap + kLcSxCtrl >= 0x1FFF) return false;
    uint32_t ctrl = 0;
    if (!read(r.route, 0, TbConfigSpace::Switch, cap + kLcSxCtrl, ArrayView<uint32_t>(&ctrl, 1))) return false;
    ctrl &= ~(kLcSxCtrlWoc | kLcSxCtrlWod | kLcSxCtrlWodpc | kLcSxCtrlWodpd | kLcSxCtrlWop | kLcSxCtrlWou4);
    if (runtime && r.parent >= 0) ctrl |= kLcSxCtrlWoc | kLcSxCtrlWod | kLcSxCtrlWou4 | kLcSxCtrlWop | kLcSxCtrlWodpc | kLcSxCtrlWodpd;
    ctrl |= kLcSxCtrlSlp;
    if (!write(r.route, 0, TbConfigSpace::Switch, cap + kLcSxCtrl, ArrayView<const uint32_t>(&ctrl, 1))) return false;
  }
  return true;
}

// ------------------------------------------------------------------ time management unit

// A capability of the router's own space: the list starts at the first offset of the header (dword 1, low byte), each entry has its id in byte 1 and the next offset in byte 0.
int Usb4Domain::find_router_cap(const Usb4Router& r, uint8_t id) {
  uint32_t header = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, 1, ArrayView<uint32_t>(&header, 1))) return -1;
  uint32_t offset = header & 0xFF;
  for (uint32_t i = 0; i < 64 && offset != 0 && offset < 0x1F00; ++i) {
    uint32_t word = 0;
    if (!read(r.route, 0, TbConfigSpace::Switch, offset, ArrayView<uint32_t>(&word, 1))) return -1;
    if (((word >> 8) & 0xFF) == id) return static_cast<int>(offset);
    uint32_t next = word & 0xFF;
    if (((word >> 8) & 0xFF) == kSwitchCapVse && ((word >> 24) & 0xFF) == 0) {      // (a vendor specific capability in the long form keeps its next offset in the second dword)
      uint32_t second = 0;
      if (!read(r.route, 0, TbConfigSpace::Switch, offset + 1, ArrayView<uint32_t>(&second, 1))) return -1;
      next = second & 0xFFFF;
    }
    offset = next;
  }
  return -1;
}

bool Usb4Domain::tmu_set_time_disruption(const Usb4Router& r, bool set) {
  uint32_t word = 0;
  if (r.tbt3) {                                              // (a Thunderbolt 3 router has the bit in its time vendor capability)
    const int cap = find_router_vse(r, kVsecTime2);
    if (cap < 0 || static_cast<uint32_t>(cap) + kTime2Cs26 >= 0x1F00 || !read(r.route, 0, TbConfigSpace::Switch, cap + kTime2Cs26, ArrayView<uint32_t>(&word, 1))) return false;
    word = set ? (word | kTime2Cs26Td) : (word & ~kTime2Cs26Td);
    return write(r.route, 0, TbConfigSpace::Switch, cap + kTime2Cs26, ArrayView<const uint32_t>(&word, 1));
  }
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_tmu + kTmuRtrCs0, ArrayView<uint32_t>(&word, 1))) return false;
  word = set ? (word | kTmuRtrCs0Td) : (word & ~kTmuRtrCs0Td);
  return write(r.route, 0, TbConfigSpace::Switch, r.cap_tmu + kTmuRtrCs0, ArrayView<const uint32_t>(&word, 1));
}

bool Usb4Domain::tmu_rate_write(const Usb4Router& r, uint32_t rate) {
  uint32_t word = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, r.cap_tmu + kTmuRtrCs3, ArrayView<uint32_t>(&word, 1))) return false;
  word = (word & 0xFFFFu) | (rate << kTmuRtrCs3IntervalShift);
  return write(r.route, 0, TbConfigSpace::Switch, r.cap_tmu + kTmuRtrCs3, ArrayView<const uint32_t>(&word, 1));
}

bool Usb4Domain::tmu_port_write(const Usb4Router& r, uint32_t port, uint32_t offset, uint32_t mask, uint32_t value) {
  const Usb4Port& p = r.ports[port];
  if (p.cap_tmu == 0) return false;
  uint32_t word = 0;
  if (!port_read(r, port, p.cap_tmu + offset, ArrayView<uint32_t>(&word, 1))) return false;
  word = (word & ~mask) | value;
  return port_write(r, port, p.cap_tmu + offset, ArrayView<const uint32_t>(&word, 1));
}

// The host router's time, three dwords from TMU_RTR_CS_1, goes to the child's local time registers and the post-time trigger is waited for (tb_switch_tmu_post_time).
bool Usb4Domain::tmu_post_time(const Usb4Router& child, const Usb4Router& host) {
  if (host.cap_tmu == 0 || child.cap_tmu == 0) return false;
  uint32_t gm[3];
  if (!read(host.route, 0, TbConfigSpace::Switch, host.cap_tmu + kTmuRtrCs1, ArrayView<uint32_t>(gm, 3))) return false;
  const uint64_t hi = gm[2] & 0xFFFFu, mid = gm[1], lo = (gm[0] & 0xFFFF0000u) >> 16;
  const uint64_t local_time = hi << 48 | mid << 16 | lo;
  if (!tmu_set_time_disruption(child, true)) return false;
  bool ok = false;
  const uint32_t local[2] = {static_cast<uint32_t>(local_time), static_cast<uint32_t>(local_time >> 32)};
  const uint32_t post[2] = {0x00000001u, 0xFFFFFFFFu}, high = 0;
  if (write(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs22, ArrayView<const uint32_t>(local, 2)) &&
      write(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs24, ArrayView<const uint32_t>(post, 2)) &&
      write(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs25, ArrayView<const uint32_t>(&high, 1))) {
    for (uint32_t tries = 0; tries < 100 && !ok; ++tries) {
      uint32_t now[2];
      if (!read(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs24, ArrayView<uint32_t>(now, 2))) break;
      ok = now[0] == 0 && now[1] == 0;
      if (!ok) delay_us(5);
    }
  }
  tmu_set_time_disruption(child, false);
  return ok;
}

bool Usb4Domain::tmu_enable_root(Usb4Router& root) {
  if (root.cap_tmu == 0) return false;
  uint32_t cs0 = 0;
  if (!read(root.route, 0, TbConfigSpace::Switch, root.cap_tmu + kTmuRtrCs0, ArrayView<uint32_t>(&cs0, 1)) || !(cs0 & kTmuRtrCs0Ucap)) return false;      // (no uni-directional mode)
  if (!tmu_set_time_disruption(root, true)) return false;
  const bool ok = tmu_rate_write(root, kTmuLowResRate);
  tmu_set_time_disruption(root, false);
  root.tmu_lowres = ok;
  return ok;
}

// Titan Ridge objects to CL states while the time is synchronized; the objections of the time capability and of the upstream adapter are switched off first (tb_switch_tmu_disable_objections).
bool Usb4Domain::tmu_disable_objections(const Usb4Router& r) {
  if (!is_titan_ridge(r)) return true;
  const int cap = find_router_vse(r, kVsecTime2);
  uint32_t word = 0;
  if (cap < 0 || static_cast<uint32_t>(cap) + kTime2Cs9 >= 0x1F00 || !read(r.route, 0, TbConfigSpace::Switch, cap + kTime2Cs9, ArrayView<uint32_t>(&word, 1))) return false;
  word &= ~kTime2Cs9ObjMask;
  return write(r.route, 0, TbConfigSpace::Switch, cap + kTime2Cs9, ArrayView<const uint32_t>(&word, 1)) && tmu_port_write(r, r.upstream_port, kTmuAdpCs6, kTmuAdpCs6ObjMask, kTmuAdpCs6ObjMask);
}

bool Usb4Domain::tmu_enable(Usb4Router& child, Usb4Router& parent, const Usb4Router& root, uint32_t down_port) {
  if (child.cap_tmu == 0 || !parent.tmu_lowres) return false;
  uint32_t cs0 = 0;
  if (!read(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs0, ArrayView<uint32_t>(&cs0, 1)) || !(cs0 & kTmuRtrCs0Ucap)) return false;
  if (child.tbt3 && !tmu_disable_objections(child)) return false;
  if (!child.tbt3) tmu_post_time(child, root);              // (the time is posted to USB4 routers only)
  if (!tmu_set_time_disruption(child, true)) return false;
  bool ok = tmu_rate_write(parent, kTmuLowResRate);
  // the parameters of the mode: the frequency measurement window and the averaging constants
  if (ok) {
    uint32_t word = 0;
    ok = read(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs0, ArrayView<uint32_t>(&word, 1));
    word = (word & ~kTmuRtrCs0FreqWindMask) | (kTmuLowResFreqWindow << kTmuRtrCs0FreqWindShift);
    ok = ok && write(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs0, ArrayView<const uint32_t>(&word, 1));
    ok = ok && read(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs15, ArrayView<uint32_t>(&word, 1));
    const uint32_t avg = kTmuLowResAverage;
    word = (word & ~0x00FFFFFFu) | avg | (avg << 6) | (avg << 12) | (avg << 18);
    ok = ok && write(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs15, ArrayView<const uint32_t>(&word, 1));
    if (ok && child.usb4_version > 1) {                    // (the delta averaging constant of version 2 routers: zero for this mode)
      ok = read(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs18, ArrayView<uint32_t>(&word, 1));
      word &= ~(0xFFu << 16);
      ok = ok && write(child.route, 0, TbConfigSpace::Switch, child.cap_tmu + kTmuRtrCs18, ArrayView<const uint32_t>(&word, 1));
    }
  }
  // both ends of the link: uni-directional, and the time sync that was disabled switched on (DTS cleared)
  if (ok) {
    ok = tmu_port_write(child, child.upstream_port, kTmuAdpCs3, kTmuAdpCs3Udm, kTmuAdpCs3Udm) && tmu_port_write(child, child.upstream_port, kTmuAdpCs6, kTmuAdpCs6Dts, 0) &&
         tmu_port_write(parent, down_port, kTmuAdpCs3, kTmuAdpCs3Udm, kTmuAdpCs3Udm) && tmu_port_write(parent, down_port, kTmuAdpCs6, kTmuAdpCs6Dts, 0);
  }
  tmu_set_time_disruption(child, false);
  child.tmu_lowres = ok;
  return ok;
}

// ------------------------------------------------------------------ retimers

// One transaction on the sideband channel (usb4_port_sb_read / sb_write): the data first for a write, then the command with its pending bit, which clears when it is done; the response
// code and no-response bits say how it went. Returns 0, -1 (no answer from the target: no such retimer) or -2 (any other failure).
int Usb4Domain::sb_transfer(const Usb4Router& r, uint32_t port, uint32_t target, uint32_t index, uint32_t reg, bool write_it, uint32_t* data, uint32_t bytes) {
  const Usb4Port& p = r.ports[port];
  const uint32_t dwords = (bytes + 3) / 4;
  if (p.cap_usb4 == 0 || dwords == 0 || dwords > kOpDataDwords) return -2;
  if (write_it && !port_write(r, port, p.cap_usb4 + kPortCs2, ArrayView<const uint32_t>(data, dwords))) return -2;
  uint32_t command = reg | (bytes << kSbLengthShift) | ((target << kSbTargetShift) & (7u << kSbTargetShift)) | kSbPending;
  if (target == kSbTargetRetimer) command |= index << kSbRetimerShift;
  if (write_it) command |= kSbWrite;
  if (!port_write(r, port, p.cap_usb4 + kPortCs1, ArrayView<const uint32_t>(&command, 1))) return -2;
  uint32_t word = 0;
  bool done = false;
  for (uint32_t waited = 0; waited < 500000 && !done; waited += 50) {
    if (!port_read(r, port, p.cap_usb4 + kPortCs1, ArrayView<uint32_t>(&word, 1))) return -2;
    done = (word & kSbPending) == 0;
    if (!done) delay_us(50);
  }
  if (!done) return -2;
  if (word & kSbNoResponse) return -1;
  if (word & kSbResponseCode) return -2;
  if (!write_it && !port_read(r, port, p.cap_usb4 + kPortCs2, ArrayView<uint32_t>(data, dwords))) return -2;
  return 0;
}

// usb4_port_sb_op: the opcode is written to the target's opcode register and read back until it changes; zero is success, the error and not-supported codes are failures.
int Usb4Domain::sb_op(const Usb4Router& r, uint32_t port, uint32_t target, uint32_t index, uint32_t opcode, uint32_t timeout_us) {
  uint32_t value = opcode;
  int result = sb_transfer(r, port, target, index, kSbOpcode, true, &value, 4);
  if (result != 0) return result;
  for (uint32_t waited = 0; waited < timeout_us; waited += 1000) {
    result = sb_transfer(r, port, target, index, kSbOpcode, false, &value, 4);
    if (result != 0) return result;
    if (value != opcode) return value == 0 ? 0 : -2;
    delay_us(1000);
  }
  return -2;
}

void Usb4Domain::scan_retimers(Usb4Router& r, uint32_t port) {
  Usb4Port& p = r.ports[port];
  p.retimer_count = 0;
  if (p.cap_usb4 == 0) return;
  uint32_t value = kSbOpEnumerate;
  if (sb_transfer(r, port, kSbTargetRouter, 0, kSbOpcode, true, &value, 4) != 0) return;      // (a broadcast: the indices facing this port are set)
  uint32_t last = 0;
  for (uint32_t i = 1; i <= kMaxRetimers; ++i) {
    const int result = sb_op(r, port, kSbTargetRetimer, i, kSbOpQueryLast, 500000);
    if (result != 0) break;
    uint32_t metadata = 0;
    if (sb_transfer(r, port, kSbTargetRetimer, i, kSbMetadata, false, &metadata, 4) != 0) break;
    if (metadata & 1) last = i;
  }
  for (uint32_t i = 1; i <= last; ++i) {
    uint32_t vendor = 0, product = 0, metadata = 0;
    if (sb_transfer(r, port, kSbTargetRetimer, i, kSbVendorId, false, &vendor, 4) != 0 || sb_transfer(r, port, kSbTargetRetimer, i, kSbProductId, false, &product, 4) != 0) break;
    // (a retimer that has no NVM operations is not taken)
    if (sb_op(r, port, kSbTargetRetimer, i, kSbOpGetNvmSectorSize, 500000) != 0 || sb_transfer(r, port, kSbTargetRetimer, i, kSbMetadata, false, &metadata, 4) != 0) break;
    Usb4Port::Retimer& rt = p.retimers[p.retimer_count++];
    rt.vendor = vendor;
    rt.product = product;
    rt.sector_size = metadata & 0xFFFFFF;
  }
}

const Usb4Port::Retimer* Usb4Domain::find_retimer(uint32_t router_index, uint32_t port, uint32_t index, const Usb4Router** router) const {
  const Usb4Router* r = this->router(router_index);
  if (!r || port == 0 || port >= usb4::kMaxPorts || index >= r->ports[port].retimer_count) return nullptr;
  *router = r;
  return &r->ports[port].retimers[index];
}

// NVM_SET_OFFSET: the dword address in the metadata (bits 23:2), then the operation.
bool Usb4Domain::retimer_set_offset(const Usb4Router& r, uint32_t port, uint32_t sb_index, uint32_t address) {
  uint32_t metadata = ((address / 4) << 2) & 0xFFFFFCu;
  return sb_transfer(r, port, kSbTargetRetimer, sb_index, kSbMetadata, true, &metadata, 4) == 0 && sb_op(r, port, kSbTargetRetimer, sb_index, kSbOpNvmSetOffset, 500000) == 0;
}

Usb4Domain::RetimerNvm Usb4Domain::retimer_nvm_write(uint32_t router_index, uint32_t port, uint32_t index, ConstByteView image) {
  const Usb4Router* r = nullptr;
  const Usb4Port::Retimer* rt = find_retimer(router_index, port, index, &r);
  if (!rt) return RetimerNvm::NoSuchRetimer;
  if (rt->vendor != kRetimerVendorIntel) return RetimerNvm::Unsupported;
  uint32_t start = 0;
  if (!tb_nvm_validate_image(image, static_cast<uint16_t>(rt->product), false, &start) || (image.size() - start) % 4 != 0) return RetimerNvm::BadImage;
  const uint32_t sb_index = index + 1;
  if (!retimer_set_offset(*r, port, sb_index, 0)) return RetimerNvm::Failed;
  const uint64_t total = image.size() - start;
  for (uint64_t done = 0; done < total;) {
    const uint32_t bytes = static_cast<uint32_t>(total - done < 64 ? total - done : 64);
    uint32_t block[kOpDataDwords] = {};
    for (uint32_t i = 0; i < bytes; ++i) block[i / 4] |= uint32_t{image[start + done + i]} << (8 * (i % 4));
    bool ok = false;
    for (uint32_t attempt = 0; attempt < 4 && !ok; ++attempt)        // (a timeout is tried again, three times)
      ok = sb_transfer(*r, port, kSbTargetRetimer, sb_index, kSbData, true, block, bytes) == 0 && sb_op(*r, port, kSbTargetRetimer, sb_index, kSbOpNvmBlockWrite, 1000000) == 0;
    if (!ok) return RetimerNvm::Failed;
    done += bytes;
  }
  return RetimerNvm::Ok;
}

// The authentication starts with a raw write of the opcode: once it runs the retimer's index is gone, so there is no answer to wait for. A status that can still be read is an error code
// with the reason in the metadata; no answer at all is what it looks like when it works. `auth_only` authenticates what is in the flash already (the offset set to the start).
Usb4Domain::RetimerNvm Usb4Domain::retimer_nvm_authenticate(uint32_t router_index, uint32_t port, uint32_t index, bool auth_only, uint32_t* status) {
  const Usb4Router* r = nullptr;
  const Usb4Port::Retimer* rt = find_retimer(router_index, port, index, &r);
  if (!rt) return RetimerNvm::NoSuchRetimer;
  if (rt->vendor != kRetimerVendorIntel) return RetimerNvm::Unsupported;
  const uint32_t sb_index = index + 1;
  if (auth_only && !retimer_set_offset(*r, port, sb_index, 0)) return RetimerNvm::Failed;
  uint32_t opcode = kSbOpNvmAuthWrite;
  if (sb_transfer(*r, port, kSbTargetRetimer, sb_index, kSbOpcode, true, &opcode, 4) != 0) return RetimerNvm::Failed;
  delay_us(150);
  if (status) *status = 0;
  uint32_t value = 0;
  if (sb_transfer(*r, port, kSbTargetRetimer, sb_index, kSbOpcode, false, &value, 4) != 0) return RetimerNvm::Ok;        // (gone: it restarts)
  if (value == 0) return RetimerNvm::Ok;
  if (value != kSbOpErr) return RetimerNvm::Failed;
  uint32_t metadata = 0;
  if (sb_transfer(*r, port, kSbTargetRetimer, sb_index, kSbMetadata, false, &metadata, 4) != 0) return RetimerNvm::Failed;
  if (status) *status = metadata & kSbMetadataAuthMask;
  return (metadata & kSbMetadataAuthMask) != 0 ? RetimerNvm::AuthFailed : RetimerNvm::Ok;
}

// NVM_READ in blocks of up to 16 dwords from a dword aligned address (tb_nvm_read_data); a block shorter than 16 dwords says its length in the metadata.
Usb4Domain::RetimerNvm Usb4Domain::retimer_nvm_read(uint32_t router_index, uint32_t port, uint32_t index, uint32_t address, ByteView out) {
  const Usb4Router* r = nullptr;
  const Usb4Port::Retimer* rt = find_retimer(router_index, port, index, &r);
  if (!rt) return RetimerNvm::NoSuchRetimer;
  if (address % 4 != 0 || out.size() % 4 != 0 || address > 0x3FFFFFu) return RetimerNvm::Failed;
  const uint32_t sb_index = index + 1;
  for (uint64_t done = 0; done < out.size();) {
    const uint32_t bytes = static_cast<uint32_t>(out.size() - done < 64 ? out.size() - done : 64), dwords = bytes / 4;
    uint32_t metadata = (((address + static_cast<uint32_t>(done)) / 4) << 2) & 0xFFFFFCu;
    if (dwords < kOpDataDwords) metadata |= dwords << 24;
    uint32_t block[kOpDataDwords] = {};
    if (sb_transfer(*r, port, kSbTargetRetimer, sb_index, kSbMetadata, true, &metadata, 4) != 0 || sb_op(*r, port, kSbTargetRetimer, sb_index, kSbOpNvmRead, 500000) != 0 ||
        sb_transfer(*r, port, kSbTargetRetimer, sb_index, kSbData, false, block, bytes) != 0)
      return RetimerNvm::Failed;
    for (uint32_t i = 0; i < bytes; ++i) out[done + i] = static_cast<uint8_t>(block[i / 4] >> (8 * (i % 4)));
    done += bytes;
  }
  return RetimerNvm::Ok;
}

// ------------------------------------------------------------------ the tree

bool Usb4Domain::start(const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (started_) return true;
  int root = -1;
  if (!read_router(0, -1, 0, &root, error)) return false;
  started_ = true;
  tmu_enable_root(*routers_[root]);
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
  scan_retimers(parent_ref, port_number);                  // (the cable's retimers, before the router behind it)
  if (!unlock_downstream(parent_ref, port_number)) return;
  const uint64_t route = parent_ref.route | (static_cast<uint64_t>(port_number) << (8 * parent_ref.depth));
  int child_index = -1;
  const char* why = nullptr;
  bool silent = false;
  if (!read_router(route, static_cast<int16_t>(index), static_cast<uint8_t>(port_number), &child_index, &why, &silent)) {
    if (silent) add_xdomain(index, port_number, route);   // (no router there: another host on the cable)
    else ++unreachable_;                                  // (a router we cannot run)
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
  {
    int root = index;
    while (routers_[root]->parent >= 0) root = routers_[root]->parent;
    clx_enable_first_link(child_index);
    tmu_enable(child, parent, *routers_[root], port_number);   // (a router that cannot keep time with the host's works without it)
  }
  configuration_valid(child);
  scan_router(child_index);
}

// Another host behind a lane adapter: it is added to the hosts to discover, with our UUID taken from the host router's unique id (uid, uid, ones, ones), and the adapter is marked as
// having one (PORT_CS_19.PID).
void Usb4Domain::add_xdomain(int router_index, uint32_t port, uint64_t route) {
  Usb4Router& r = *routers_[router_index];
  Usb4Port& p = r.ports[port];
  int root = 0;
  for (uint64_t i = 0; i < routers_.count(); ++i)
    if (routers_[i]->present && routers_[i]->parent < 0) root = static_cast<int>(i);
  uint8_t uuid[16];
  const uint64_t uid = routers_[root]->uid;
  for (uint32_t k = 0; k < 8; ++k) uuid[k] = static_cast<uint8_t>(uid >> (8 * k));
  for (uint32_t k = 8; k < 16; ++k) uuid[k] = 0xFF;
  if (xd_.add(route, uuid) < 0) return;
  p.xdomain = true;
  if (p.cap_usb4 != 0) {
    uint32_t word = 0;
    if (port_read(r, port, p.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1))) {
      word |= kPortCs19Pid;
      port_write(r, port, p.cap_usb4 + kPortCs19, ArrayView<const uint32_t>(&word, 1));
    }
  }
}

void Usb4Domain::remove_xdomain(int router_index, uint32_t port) {
  Usb4Router& r = *routers_[router_index];
  Usb4Port& p = r.ports[port];
  if (!p.xdomain) return;
  xd_.remove(r.route | (static_cast<uint64_t>(port) << (8 * r.depth)));
  p.xdomain = false;
  if (p.cap_usb4 != 0 && r.present) {
    uint32_t word = 0;
    if (port_read(r, port, p.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1))) {
      word &= ~kPortCs19Pid;
      port_write(r, port, p.cap_usb4 + kPortCs19, ArrayView<const uint32_t>(&word, 1));
    }
  }
}

void Usb4Domain::remove_below(int index, uint32_t port) {
  if (routers_[index]->ports[port].xdomain) remove_xdomain(index, port);
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
    if (port.xdomain) remove_xdomain(index, p);
    if (port.remote_router >= 0 && port.remote_router != r.parent) {
      const int child = port.remote_router;
      port.remote_router = -1;
      remove_router(child);
    }
  }
  for (uint32_t k = 0; k < 3; ++k) {
    const int tunnel = find_tunnel(index, k == 0 ? Usb4TunnelKind::Pcie : k == 1 ? Usb4TunnelKind::Usb3 : Usb4TunnelKind::Dp);
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
      const bool dp = port.type == kTypeDpIn || port.type == kTypeDpOut;
      if ((port.type != kTypePcieDown && port.type != kTypePcieUp && port.type != kTypeUsb3Down && port.type != kTypeUsb3Up && !dp) || port.cap_adapter == 0) continue;
      uint32_t word = 0;
      if (!port_read(r, p, port.cap_adapter + kAdpPcieCs0, ArrayView<uint32_t>(&word, 1)) || !(word & (dp ? (kDpCs0Ve | kDpCs0Ae) : kAdpPcieCs0Pe))) continue;       // (PE is bit 31 of the first dword of the PCIe and the USB 3 adapters alike)
      if (find_tunnel(static_cast<int>(i), Usb4TunnelKind::Pcie) >= 0 || find_tunnel(static_cast<int>(i), Usb4TunnelKind::Usb3) >= 0 || find_tunnel(static_cast<int>(i), Usb4TunnelKind::Dp) >= 0) continue;     // (ours)
      const Usb4TunnelKind kind = (port.type == kTypePcieDown || port.type == kTypePcieUp) ? Usb4TunnelKind::Pcie : Usb4TunnelKind::Usb3;
      if (dp ? set_dp_enable(r, p, false) : set_adapter_enable(r, p, kind, false)) ++preboot_closed_;
    }
  }
}

// ------------------------------------------------------------------ sleep

// usb4_switch_set_sleep: the sleep bit, then wait for the router to say it is ready.
// usb4_switch_set_wake: every lane adapter with the USB4 port capability (the host router's upstream one is the NHI and has none) is set to wake the host: the upstream one for a USB4 link,
// a downstream one that is configured for a disconnect and for a link that comes up as USB4, an unconfigured one for a connect; the router itself for PCIe, USB 3 and DisplayPort activity.
bool Usb4Domain::set_wake(Usb4Router& r, bool runtime) {
  if (!runtime) return true;                               // (a wake from system sleep is the platform's: nothing is armed here)
  for (uint32_t p = 1; p <= r.max_port; ++p) {
    const Usb4Port& port = r.ports[p];
    if (!port.is_lane() || port.cap_usb4 == 0 || (r.parent < 0 && p == r.upstream_port)) continue;
    uint32_t word = 0;
    if (!port_read(r, p, port.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1))) return false;
    word &= ~(kPortCs19Woc | kPortCs19Wod | kPortCs19Wou4);
    if (r.parent >= 0 && p == r.upstream_port) {
      word |= kPortCs19Wou4;
    } else {
      const bool configured = (word & kPortCs19Pc) != 0;
      if (!configured) word |= kPortCs19Woc;
      else word |= kPortCs19Wod | kPortCs19Wou4;
    }
    if (!port_write(r, p, port.cap_usb4 + kPortCs19, ArrayView<const uint32_t>(&word, 1))) return false;
  }
  if (r.parent >= 0) {
    uint32_t cs5 = 0;
    if (!read(r.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<uint32_t>(&cs5, 1))) return false;
    cs5 |= kCs5Wop | kCs5Wou | kCs5Wod;
    if (!write(r.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<const uint32_t>(&cs5, 1))) return false;
  }
  return true;
}

bool Usb4Domain::put_to_sleep(Usb4Router& r, bool runtime) {
  if (r.tbt3) return lc_sleep(r, runtime);                  // (a Thunderbolt 3 router sleeps through its link controller)
  if (!set_wake(r, runtime)) return false;
  uint32_t cs5 = 0;
  if (!read(r.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<uint32_t>(&cs5, 1))) return false;
  cs5 |= kCs5Slp;
  if (!write(r.route, 0, TbConfigSpace::Switch, kRouterCs5, ArrayView<const uint32_t>(&cs5, 1))) return false;
  for (uint32_t waited = 0; waited < 500000; waited += 1000) {
    uint32_t cs6 = 0;
    if (read(r.route, 0, TbConfigSpace::Switch, kRouterCs6, ArrayView<uint32_t>(&cs6, 1)) && (cs6 & kCs6Slpr)) return true;
    delay_us(1000);
  }
  return false;
}

bool Usb4Domain::runtime_allowed() const {
  if (!started_ || suspended_ || xd_.live() != 0) return false;
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used) return false;
  return true;
}

bool Usb4Domain::suspend(bool runtime) {
  if (!started_ || suspended_ || (runtime && !runtime_allowed())) return false;
  plug_count_ = 0;                                         // (events from before the sleep mean nothing after it)
  dp_request_count_ = 0;
  xd_.clear();                                              // (the other hosts and their paths)
  for (uint64_t i = 0; i < routers_.count(); ++i)
    if (routers_[i]->present) clx_disable(*routers_[i]);     // (the low power states go before the links do)
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used) remove_tunnel(static_cast<int>(i));
  // deepest first; a router that will not sleep is left awake and dropped with the rest, the host router's refusal is what the answer reports
  bool host_asleep = true;
  for (int depth = static_cast<int>(kMaxDepth); depth >= 0; --depth) {
    for (uint64_t i = 0; i < routers_.count(); ++i) {
      Usb4Router& r = *routers_[i];
      if (!r.present || r.depth != depth) continue;
      if (!put_to_sleep(r, runtime) && depth == 0) host_asleep = false;
    }
  }
  for (uint64_t i = 0; i < routers_.count(); ++i) routers_[i]->present = false;
  started_ = false;
  suspended_ = true;
  return host_asleep;
}

bool Usb4Domain::resume(const char** error) {
  if (!suspended_) return false;
  suspended_ = false;
  preboot_closed_ = 0;
  return start(error);
}

// ------------------------------------------------------------------ events

bool Usb4Domain::on_event(TbPacketType type, ConstByteView body) {
  if (type == TbPacketType::XdomainRequest || type == TbPacketType::XdomainResponse) return xd_.handle(type, body);
  if (type == TbPacketType::Error && body.size() >= 12) {                 // the DisplayPort bandwidth request error: the adapter wants more or gives some back
    ByteReader e(body);
    const uint32_t hi = e.le32(), lo = e.le32(), word = e.le32();
    if (e.ok() && (word & 0xFF) == kErrorDpBandwidth) {
      if (dp_request_count_ < kPlugQueue) {
        Plug& request = dp_requests_[(dp_request_head_ + dp_request_count_) % kPlugQueue];
        request.route = (static_cast<uint64_t>(hi & 0x3FFFFF) << 32) | lo;
        request.port = static_cast<uint8_t>((word >> 8) & 0x3F);
        ++dp_request_count_;
      }
      return true;
    }
    return false;
  }
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
  while (dp_request_count_ != 0) {
    const Plug request = dp_requests_[dp_request_head_];
    dp_request_head_ = (dp_request_head_ + 1) % kPlugQueue;
    --dp_request_count_;
    const int router = find_router(request.route);
    const int slot = router >= 0 ? dp_find(static_cast<uint32_t>(router), request.port) : -1;
    if (slot >= 0 && tunnels_[slot].bw_mode && dp_serve_request(slot)) dp_recalc_estimates();
  }
  xd_.service();
}

bool Usb4Domain::use_interrupts(bool on, int msix_vector, int data_vector) {
  if (msix_vector > 15 || data_vector > 15) return false;
  interrupts_ = on;
  if (on) {
    msix_vector_ = msix_vector;
    data_vector_ = data_vector;
    nhi_.set_control_interrupts(true, msix_vector);
  } else {
    nhi_.set_control_interrupts(false, msix_vector >= 0 ? msix_vector : msix_vector_);
    nhi_.disable_interrupts();
    msix_vector_ = data_vector_ = -1;
  }
  return true;
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
    if (path->dma) {                                        // (the credits the path held on a lane adapter go back)
      Usb4Port& in = routers_[hop.router]->ports[hop.in_port];
      if (lane_uses_allocation(*routers_[hop.router], in)) in.dma_credits = in.dma_credits >= hop.credits ? static_cast<uint16_t>(in.dma_credits - hop.credits) : 0;
    }
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
    uint32_t credits = in.is_lane() ? (in.bonded ? 2 * kPcieCredits : kPcieCredits) : 7;       // (no credit allocation: 16 on a lane adapter, 32 when bonded, 7 on a protocol adapter)
    if (lane_uses_allocation(router, in)) {
      if (kind == Usb4TunnelKind::Usb3) {
        credits = router.max_usb3_credits;
      } else {
        const uint32_t available = available_credits(router, in);
        credits = router.max_pcie_credits < available ? router.max_pcie_credits : available;
        if (credits < 6) {                                   // (not enough buffers for a PCIe path)
          release_path(path);
          return false;
        }
      }
    }
    path->hops[i].credits = static_cast<uint8_t>(credits);
  }
  return true;
}

// One entry of a hop table (tb_path_activate). PCIe paths: flow control out of the source and internal hops and into every hop, no shared buffers, priority 3, weight 1.
bool Usb4Domain::write_hop(const Usb4Router& r, uint32_t port, uint32_t hop_index, const Usb4Hop& hop, bool first, bool last, const Usb4Path& path) {
  const uint8_t in_mask = first ? kPathSource : kPathInternal, out_mask = last ? kPathDestination : kPathInternal;
  const bool ingress_fc = (path.ingress_fc_enable & in_mask) != 0, egress_fc = (path.egress_fc_enable & out_mask) != 0;
  uint32_t words[2];
  words[0] = hop.out_hop | (static_cast<uint32_t>(hop.out_port) << 11) | (static_cast<uint32_t>(hop.credits) << 17) | (path.pm_support ? 1u << 24 : 0) | (1u << 31);
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
// Buffers not under flow control are added to the input adapter's count (ADP_CS_4, low ten bits) before the hop is written (tb_port_add_nfc_credits).
bool Usb4Domain::add_nfc(const Usb4Router& r, uint32_t port, int delta) {
  if (delta == 0) return true;
  uint32_t word = 0;
  if (!port_read(r, port, kAdpCs4, ArrayView<uint32_t>(&word, 1))) return false;
  const int now = static_cast<int>(word & kAdpCs4NfcMask), next = now + delta;
  if (next < 0 || next > static_cast<int>(kAdpCs4NfcMask)) return false;
  word = (word & ~kAdpCs4NfcMask) | static_cast<uint32_t>(next);
  return port_write(r, port, kAdpCs4, ArrayView<const uint32_t>(&word, 1));
}

bool Usb4Domain::activate_path(Usb4Path* path) {
  for (int i = static_cast<int>(path->length) - 1; i >= 0; --i) {
    const Usb4Hop& hop = path->hops[i];
    if (!add_nfc(*routers_[hop.router], hop.in_port, hop.nfc_credits)) {
      for (uint32_t k = static_cast<uint32_t>(i) + 1; k < path->length; ++k) add_nfc(*routers_[path->hops[k].router], path->hops[k].in_port, -static_cast<int>(path->hops[k].nfc_credits));
      return false;
    }
  }
  for (int i = static_cast<int>(path->length) - 1; i >= 0; --i) {
    const Usb4Hop& hop = path->hops[i];
    const Usb4Router& r = *routers_[hop.router];
    disable_hop(r, hop.in_port, hop.in_hop);              // (left over from before)
    if (!write_hop(r, hop.in_port, hop.in_hop, hop, i == 0, i == static_cast<int>(path->length) - 1, *path)) {
      for (uint32_t k = static_cast<uint32_t>(i) + 1; k < path->length; ++k) disable_hop(*routers_[path->hops[k].router], path->hops[k].in_port, path->hops[k].in_hop);
      for (uint32_t k = 0; k < path->length; ++k) add_nfc(*routers_[path->hops[k].router], path->hops[k].in_port, -static_cast<int>(path->hops[k].nfc_credits));
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
    if (hop.router >= 0 && routers_[hop.router]->present) {
      disable_hop(*routers_[hop.router], hop.in_port, hop.in_hop);
      add_nfc(*routers_[hop.router], hop.in_port, -static_cast<int>(hop.nfc_credits));
    }
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
bool Usb4Domain::usb3_allocate_bandwidth(const Usb4Router& host, uint32_t down_port, const Usb4Router& device, uint32_t up_port, uint32_t* allocated) {
  const Usb4Port& down = host.ports[down_port];
  const Usb4Port& up = device.ports[up_port];
  uint32_t speed_down = 0, speed_up = 0;
  if (!port_read(host, down_port, down.cap_adapter + kUsb3Cs4, ArrayView<uint32_t>(&speed_down, 1)) || !port_read(device, up_port, up.cap_adapter + kUsb3Cs4, ArrayView<uint32_t>(&speed_up, 1))) return false;
  const uint32_t rate_down = ((speed_down >> kUsb3Cs4MslrShift) & kUsb3Cs4MslrMask) == kUsb3Cs4Mslr20G ? 20000 : 10000;
  const uint32_t rate_up = ((speed_up >> kUsb3Cs4MslrShift) & kUsb3Cs4MslrMask) == kUsb3Cs4Mslr20G ? 20000 : 10000;
  const uint32_t wanted = (rate_down < rate_up ? rate_down : rate_up) * 90 / 100;
  return usb3_set_allocation(host, down_port, wanted, wanted, false, allocated);
}

// usb4_usb3_port_allocate_bandwidth / release_bandwidth: the adapter is asked for control (CMR, waiting for HCA), the bandwidth already consumed is read, and the larger of that and the wanted
// rate (for a release: just what is consumed) is written with a scale that makes it fit 12 bits; the request is withdrawn again.
bool Usb4Domain::usb3_set_allocation(const Usb4Router& host, uint32_t down_port, uint32_t wanted_up, uint32_t wanted_down, bool release, uint32_t* allocated) {
  const Usb4Port& down = host.ports[down_port];
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
    const uint32_t up_bw = release ? consumed_up : (wanted_up > consumed_up ? wanted_up : consumed_up), down_bw = release ? consumed_down : (wanted_down > consumed_down ? wanted_down : consumed_down);
    const uint32_t max_bw = up_bw > down_bw ? up_bw : down_bw;
    auto to_bw = [](uint32_t mbps, uint32_t sc) { const uint64_t uframes = static_cast<uint64_t>(mbps) * 1000000 / 8000; const uint64_t unit = 512ull << sc; return static_cast<uint32_t>((uframes + unit - 1) / unit); };
    uint32_t new_scale = 0;
    while (new_scale < 64 && to_bw(max_bw, new_scale) >= 4096) ++new_scale;
    uint32_t cs2 = 0;
    if (new_scale < 64 && port_write(host, down_port, down.cap_adapter + kUsb3Cs3, ArrayView<const uint32_t>(&new_scale, 1)) &&
        port_read(host, down_port, down.cap_adapter + kUsb3Cs2, ArrayView<uint32_t>(&cs2, 1))) {
      cs2 = (cs2 & ~0x00FFFFFFu) | to_bw(up_bw, new_scale) | (to_bw(down_bw, new_scale) << 12);
      ok = port_write(host, down_port, down.cap_adapter + kUsb3Cs2, ArrayView<const uint32_t>(&cs2, 1));
      if (ok && allocated) *allocated = down_bw;
    }
  }
  set_request(false);
  return ok;
}

// A USB 3 stream whose bandwidth the host router's adapter manages and that crosses the link above `r` (the first link below the host): the tunnel, or -1.
int Usb4Domain::usb3_on_link(int r) const {
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Usb3 && tunnels_[i].device_router == r && routers_[tunnels_[i].host_router]->parent < 0) return static_cast<int>(i);
  return -1;
}

bool Usb4Domain::remove_tunnel(int slot) {
  Usb4Tunnel& t = tunnels_[slot];
  // the adapters first (down, then up), then the paths; the device may be gone already
  if ((t.kind == Usb4TunnelKind::Pcie || t.kind == Usb4TunnelKind::Usb3) && t.host_router >= 0 && routers_[t.host_router]->present) set_adapter_enable(*routers_[t.host_router], t.down_port, t.kind, false);
  if ((t.kind == Usb4TunnelKind::Pcie || t.kind == Usb4TunnelKind::Usb3) && t.device_router >= 0 && routers_[t.device_router]->present) {
    set_adapter_enable(*routers_[t.device_router], t.up_port, t.kind, false);
    if (t.kind == Usb4TunnelKind::Pcie) routers_[t.device_router]->pcie_tunnel = false;
    else routers_[t.device_router]->usb3_tunnel = false;
  }
  if (t.kind == Usb4TunnelKind::Dp) {                       // (the video and AUX enables, and a pending hot plug detect, undone first)
    if (t.host_router >= 0 && routers_[t.host_router]->present) {
      uint32_t hpd = 0;
      const Usb4Router& in = *routers_[t.host_router];
      if (port_read(in, t.down_port, in.ports[t.down_port].cap_adapter + kDpCs3, ArrayView<uint32_t>(&hpd, 1))) {
        hpd |= kDpCs3Hpdc;
        port_write(in, t.down_port, in.ports[t.down_port].cap_adapter + kDpCs3, ArrayView<const uint32_t>(&hpd, 1));
      }
      set_dp_enable(in, t.down_port, false);
      dp_resource(in, kOpDeallocDpResource, t.down_port);     // (and the input is free for the other managers again)
    }
    if (t.device_router >= 0 && routers_[t.device_router]->present) set_dp_enable(*routers_[t.device_router], t.up_port, false);
    deactivate_path(&t.third);
    release_path(&t.third);
    t.bw_mode = false;
  }
  deactivate_path(&t.down);
  deactivate_path(&t.up);
  release_path(&t.down);
  release_path(&t.up);
  const uint8_t group = t.bw_mode || t.kind == Usb4TunnelKind::Dp ? t.group : 0;
  t = Usb4Tunnel();
  if (group != 0 && group < 8) {                            // (the last stream of a group takes the group's keep with it)
    bool others = false;
    for (uint32_t i = 0; i < kMaxTunnels; ++i) others = others || (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp && tunnels_[i].bw_mode && tunnels_[i].group == group);
    if (!others) group_reserved_[group] = group_timer_[group] = 0;
  }
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
  if (!pcie && host.parent < 0) {
    uint32_t allocated = 0;
    if (!usb3_allocate_bandwidth(host, static_cast<uint32_t>(down), device, static_cast<uint32_t>(up), &allocated)) return fail("the USB 3 bandwidth could not be allocated");
    t.allocated_bw = allocated;
  }
  if (!activate_path(&t.down) || !activate_path(&t.up)) return fail("the paths could not be written");
  // PCIe: the far adapter first; USB 3: the near one first (tb_pci_activate / tb_usb3_activate)
  if (pcie) {
    if (!set_adapter_enable(device, static_cast<uint32_t>(up), kind, true) || !set_adapter_enable(host, static_cast<uint32_t>(down), kind, true)) return fail("the adapters could not be enabled");
  } else {
    if (!set_adapter_enable(host, static_cast<uint32_t>(down), kind, true) || !set_adapter_enable(device, static_cast<uint32_t>(up), kind, true)) return fail("the adapters could not be enabled");
  }
  t.active = true;
  if (pcie) {
    device.pcie_tunnel = true;
    tbt3_after_pcie(host, device);
  } else {
    device.usb3_tunnel = true;
  }
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

// ------------------------------------------------------------------ data paths to another host

bool Usb4Domain::take_hop(Usb4Router& r, uint32_t port, bool in, uint32_t hop) {
  Usb4Port& p = r.ports[port];
  const uint32_t limit = in ? p.max_in_hop : p.max_out_hop;
  uint64_t* bits = in ? p.in_hops : p.out_hops;
  if (hop == 0 || hop > limit || hop >= kHopIdLimit || bit_test(bits, hop)) return false;
  bit_set(bits, hop);
  return true;
}

// A path between the host interface and the lane adapter of the host on the cable, through the routers in `chain` (the one that holds that adapter first, the host router last):
// `toward_host` for the path we receive on, otherwise the one we send on. The first input and the last output HopIDs are given (the ring on the host interface, the HopID on the cable); the
// ones in between are the lowest free of both ends of each link (tb_path_alloc / tb_dma_init_*_path).
bool Usb4Domain::build_dma_path(Usb4Path* path, const int* chain, uint32_t n, bool toward_host, uint32_t xdomain_port, uint32_t nhi_port, uint32_t ring, uint32_t cable_hop) {
  *path = Usb4Path();
  if (n == 0 || n > kMaxHops) return false;
  path->priority = 5;
  path->weight = 1;
  path->dma = true;
  path->egress_fc_enable = toward_host ? kPathSource | kPathInternal : kPathAll;
  path->ingress_fc_enable = kPathAll;
  struct Step { int router; uint32_t in_port, out_port; };
  Step steps[kMaxHops];
  auto upstream_lane = [&](const Usb4Router& r) {
    uint32_t lane = r.upstream_port;
    if (r.ports[lane].link_nr != 0 && r.ports[lane].dual_link >= 0) lane = static_cast<uint32_t>(r.ports[lane].dual_link);
    return lane;
  };
  for (uint32_t i = 0; i < n; ++i) {
    // position along the direction of travel
    const uint32_t k = toward_host ? i : n - 1 - i;          // which router of the chain
    const Usb4Router& r = *routers_[chain[k]];
    const Usb4Router* below = k > 0 ? routers_[chain[k - 1]].get() : nullptr;
    const uint32_t down_lane = below ? below->parent_port : xdomain_port;           // toward the other host
    const uint32_t up_lane = k + 1 < n ? upstream_lane(r) : nhi_port;               // toward the host interface
    steps[i] = Step{chain[k], toward_host ? down_lane : up_lane, toward_host ? up_lane : down_lane};
  }
  uint32_t in_hop = toward_host ? cable_hop : ring;
  const uint32_t last_out = toward_host ? ring : cable_hop;
  bool ok = take_hop(*routers_[steps[0].router], steps[0].in_port, true, in_hop);
  for (uint32_t i = 0; ok && i < n; ++i) {
    Usb4Router& r = *routers_[steps[i].router];
    // the hop is recorded as soon as its input is held, so that a failure further on gives it back
    Usb4Hop& hop = path->hops[path->length++];
    hop = Usb4Hop{static_cast<int16_t>(steps[i].router), static_cast<uint8_t>(steps[i].in_port), static_cast<uint8_t>(steps[i].out_port), static_cast<uint16_t>(in_hop), 0, 0};
    uint32_t out_hop = 0;
    if (i + 1 == n) {
      out_hop = last_out;
      ok = take_hop(r, steps[i].out_port, false, out_hop);
    } else {
      Usb4Router& next = *routers_[steps[i + 1].router];
      for (uint32_t h = kMinPathHop; h < kHopIdLimit && out_hop == 0; ++h) {
        if (h > r.ports[steps[i].out_port].max_out_hop || h > next.ports[steps[i + 1].in_port].max_in_hop) break;
        if (!bit_test(r.ports[steps[i].out_port].out_hops, h) && !bit_test(next.ports[steps[i + 1].in_port].in_hops, h)) out_hop = h;
      }
      ok = out_hop != 0 && take_hop(r, steps[i].out_port, false, out_hop);
      if (ok && !take_hop(next, steps[i + 1].in_port, true, out_hop)) {
        release_hop(r, steps[i].out_port, false, out_hop);
        ok = false;
        out_hop = 0;
      }
    }
    if (!ok) break;
    hop.out_hop = static_cast<uint16_t>(out_hop);
    in_hop = out_hop;
  }
  if (!ok) {
    release_path(path);
    return false;
  }
  // credits (tb_dma_reserve_credits without credit allocation): 6 on a lane adapter, 14 when bonded, the adapter's buffers up to 14 on a protocol adapter; the first hop of the path we
  // receive on gets what the lane adapter has left after the control path
  // With the router's buffer allocation a lane adapter's DMA credits come out of what it has left after PCIe and the DMA paths already there (tb_dma_reserve_credits); at least one is needed.
  // Worked out for the whole path first, and taken only when every hop has its share.
  const Usb4Router& host_router = *routers_[chain[n - 1]];
  const uint32_t wanted = host_router.max_dma_credits != 0 && host_router.max_dma_credits < 14 ? host_router.max_dma_credits : 14;
  for (uint32_t i = 0; i < path->length; ++i) {
    Usb4Router& router = *routers_[path->hops[i].router];
    Usb4Port& in = router.ports[path->hops[i].in_port];
    uint32_t credits;
    if (toward_host && i == 0) {
      const uint32_t usable = in.total_credits > in.ctl_credits ? in.total_credits - in.ctl_credits : 0;
      credits = usable < wanted ? usable : wanted;
    } else if (lane_uses_allocation(router, in)) {
      const int available = static_cast<int>(available_credits(router, in)) - static_cast<int>(router.max_pcie_credits) - static_cast<int>(in.dma_credits);
      if (available < 1) {
        path->dma = false;                                  // (nothing was taken yet)
        release_path(path);
        return false;
      }
      credits = wanted;
      while (credits > static_cast<uint32_t>(available)) --credits;
    } else {
      credits = in.is_lane() ? (in.bonded ? 14 : 6) : (in.total_credits < wanted ? in.total_credits : wanted);
    }
    path->hops[i].credits = static_cast<uint8_t>(credits);
  }
  for (uint32_t i = 0; i < path->length; ++i) {
    Usb4Router& router = *routers_[path->hops[i].router];
    Usb4Port& in = router.ports[path->hops[i].in_port];
    if (lane_uses_allocation(router, in)) in.dma_credits = static_cast<uint16_t>(in.dma_credits + path->hops[i].credits);
  }
  return true;
}

bool Usb4Domain::open_paths(const TbXDomain& xd, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (transmit_ring == 0 || receive_ring == 0) {
    *error = "ring 0 is the control channel";
    return false;
  }
  // where the other host hangs: the router whose route is the host's without its last byte, and that byte is the lane adapter
  uint32_t length = route_length(xd.route);
  if (length == 0) {
    *error = "no route";
    return false;
  }
  const uint64_t parent_route = xd.route & ~(0xFFull << (8 * (length - 1)));
  const uint32_t xdomain_port = static_cast<uint32_t>((xd.route >> (8 * (length - 1))) & 0xFF);
  const int parent = find_router(parent_route);
  int root = -1;
  for (uint64_t i = 0; i < routers_.count(); ++i)
    if (routers_[i]->present && routers_[i]->parent < 0) root = static_cast<int>(i);
  if (parent < 0 || root < 0 || xdomain_port == 0 || xdomain_port > routers_[parent]->max_port || !routers_[parent]->ports[xdomain_port].xdomain) {
    *error = "the host is not where it was";
    return false;
  }
  int chain[kMaxHops];
  uint32_t n = 0;
  for (int r = parent; r >= 0 && n < kMaxHops; r = routers_[r]->parent) chain[n++] = r;
  if (chain[n - 1] != root) {
    *error = "no route to the host interface";
    return false;
  }
  const uint32_t nhi_port = routers_[root]->upstream_port;
  clx_disable_up(parent);                                   // (the low power states are off for as long as the path to the other host is)
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
  t.kind = Usb4TunnelKind::Dma;
  t.xdomain_route = xd.route;
  // we send on `transmit_path` from `transmit_ring`; the other host sends on `receive_path` into `receive_ring`
  if (!build_dma_path(&t.down, chain, n, false, xdomain_port, nhi_port, transmit_ring, transmit_path) || !build_dma_path(&t.up, chain, n, true, xdomain_port, nhi_port, receive_ring, receive_path)) {
    release_path(&t.down);
    release_path(&t.up);
    t = Usb4Tunnel();
    *error = "the HopIDs are in use";
    return false;
  }
  if (!activate_path(&t.down) || !activate_path(&t.up)) {
    deactivate_path(&t.down);
    deactivate_path(&t.up);
    release_path(&t.down);
    release_path(&t.up);
    t = Usb4Tunnel();
    *error = "the paths could not be written";
    return false;
  }
  t.active = true;
  return true;
}

void Usb4Domain::close_paths(const TbXDomain& xd) {
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dma && tunnels_[i].xdomain_route == xd.route) remove_tunnel(static_cast<int>(i));
  const uint32_t length = route_length(xd.route);
  const int parent = length ? find_router(xd.route & ~(0xFFull << (8 * (length - 1)))) : -1;
  if (parent >= 0 && routers_[parent]->present) clx_enable_first_link(parent);
}

// ------------------------------------------------------------------ DisplayPort

bool Usb4Domain::set_dp_enable(const Usb4Router& r, uint32_t port, bool enable) {
  const Usb4Port& p = r.ports[port];
  if (p.cap_adapter == 0) return false;
  uint32_t word = 0;
  if (!port_read(r, port, p.cap_adapter + kDpCs0, ArrayView<uint32_t>(&word, 1))) return false;
  word = enable ? (word | kDpCs0Ve | kDpCs0Ae) : (word & ~(kDpCs0Ve | kDpCs0Ae));
  return port_write(r, port, p.cap_adapter + kDpCs0, ArrayView<const uint32_t>(&word, 1));
}

// tb_dp_xchg_caps for two USB4 adapters: the connection manager handshake on the OUT adapter (UF and CMHS set, CMHS clears when it is done, three seconds), then the IN adapter's local
// capabilities become the OUT adapter's remote ones and the OUT adapter's the IN adapter's (no reduction: they are not limited here).
// The rate and bandwidth of a DisplayPort link in Mb/s (tb_dp_cap_get_rate, tb_dp_cap_get_rate_ext, tb_dp_bandwidth). The first knows the rates up to HBR3 (0 for the rest), the second the
// UHBR rates as well; tunnelling removes the 8b/10b (or the 128b/132b) encoding of the link.
static uint32_t dp_cap_rate(uint32_t cap) {
  switch ((cap & usb4::kDpCommonRateMask) >> usb4::kDpCommonRateShift) {
    case 0: return 1620;
    case 1: return 2700;
    case 2: return 5400;
    case 3: return 8100;
    default: return 0;
  }
}
static uint32_t dp_cap_rate_ext(uint32_t cap) {
  if (cap & usb4::kDpCommonUhbr20) return 20000;
  if (cap & usb4::kDpCommonUhbr13_5) return 13500;
  if (cap & usb4::kDpCommonUhbr10) return 10000;
  return dp_cap_rate(cap);
}
static uint32_t dp_cap_lanes(uint32_t cap) {
  switch ((cap & usb4::kDpCommonLanesMask) >> usb4::kDpCommonLanesShift) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    default: return 0;
  }
}
static uint32_t dp_bandwidth(uint32_t rate, uint32_t lanes) {
  return rate >= 10000 ? rate * lanes * 128 / 132 : rate * lanes * 8 / 10;
}

// DP resource arbitration between connection managers (usb4_switch_query/alloc/dealloc_dp_resource): the operation of the router with the DP IN adapter, the adapter in the metadata. A
// router that does not know the operations (ONS) leaves the arbitration out, and the adapter is ours; a status other than 0 says another manager holds it.
bool Usb4Domain::dp_resource(const Usb4Router& r, uint32_t opcode, uint32_t port) {
  uint32_t metadata = port;
  uint8_t status = 0;
  if (!router_op(r, opcode, &metadata, &status, nullptr, 0, nullptr, 0)) return op_unsupported_;
  return status == 0;
}

// DPRX done (DP_COMMON_CAP, tb_dp_wait_dprx): the receiver of the display has been read.
bool Usb4Domain::dp_dprx_done(uint32_t in_router, uint32_t in_port) {
  if (in_router >= routers_.count() || !routers_[in_router]->present || in_port == 0 || in_port > routers_[in_router]->max_port) return false;
  const Usb4Router& in = *routers_[in_router];
  uint32_t cap = 0;
  return in.ports[in_port].type == kTypeDpIn && in.ports[in_port].cap_adapter != 0 && port_read(in, in_port, in.ports[in_port].cap_adapter + kDpCommonCap, ArrayView<uint32_t>(&cap, 1)) && (cap & kDpCommonDprxDone);
}

bool Usb4Domain::dp_wait_dprx(uint32_t in_router, uint32_t in_port, uint32_t timeout_ms) {
  bool done = dp_dprx_done(in_router, in_port);
  for (uint32_t waited = 0; waited < timeout_ms * 1000 && !done; waited += 100) {
    delay_us(100);
    done = dp_dprx_done(in_router, in_port);
  }
  if (!done) return false;
  const int slot = dp_find(in_router, in_port);
  if (slot >= 0 && tunnels_[slot].bw_mode) dp_update_maximum(slot);
  return true;
}

// The stream's maximum once the display has told its capabilities: the DP IN adapter's local capability register now holds the lowest of what was read over AUX.
bool Usb4Domain::dp_update_maximum(int slot) {
  Usb4Tunnel& t = tunnels_[slot];
  const Usb4Router& in = *routers_[t.host_router];
  uint32_t cap = 0;
  if (t.granularity == 0 || !port_read(in, t.down_port, in.ports[t.down_port].cap_adapter + kDpLocalCap, ArrayView<uint32_t>(&cap, 1))) return false;
  const uint32_t bw = dp_bandwidth(dp_cap_rate_ext(cap), dp_cap_lanes(cap));
  if (bw == 0) return false;
  t.max_bw = (bw + t.granularity - 1) / t.granularity * t.granularity;
  return true;
}

// The capabilities of the two adapters, each copied to the other's remote capability register. A stream without the allocation mode runs at what the links on the way leave free
// (`max_bandwidth`, 0 for no limit): when the output's rate and lanes do not fit, the highest combination of both adapters' that does is written to the input instead
// (tb_dp_xchg_caps, tb_dp_reduce_bandwidth).
bool Usb4Domain::dp_exchange_caps(const Usb4Router& in, uint32_t in_port, const Usb4Router& out, uint32_t out_port, uint32_t max_bandwidth, const char** error) {
  const Usb4Port& i = in.ports[in_port];
  const Usb4Port& o = out.ports[out_port];
  uint32_t status = 0;
  if (!port_read(out, out_port, o.cap_adapter + kDpStatusCtrl, ArrayView<uint32_t>(&status, 1))) return false;
  status |= kDpStatusCtrlUf | kDpStatusCtrlCmhs;
  if (!port_write(out, out_port, o.cap_adapter + kDpStatusCtrl, ArrayView<const uint32_t>(&status, 1))) return false;
  bool done = false;
  for (uint32_t waited = 0; waited < 3000000 && !done; waited += 100) {
    if (!port_read(out, out_port, o.cap_adapter + kDpStatusCtrl, ArrayView<uint32_t>(&status, 1))) return false;
    done = (status & kDpStatusCtrlCmhs) == 0;
    if (!done) delay_us(100);
  }
  if (!done) return false;
  uint32_t in_cap = 0, out_cap = 0;
  if (!port_read(in, in_port, i.cap_adapter + kDpLocalCap, ArrayView<uint32_t>(&in_cap, 1)) || !port_read(out, out_port, o.cap_adapter + kDpLocalCap, ArrayView<uint32_t>(&out_cap, 1))) return false;
  if (max_bandwidth != 0) {
    const uint32_t in_rate = dp_cap_rate(in_cap), in_lanes = dp_cap_lanes(in_cap), out_rate = dp_cap_rate(out_cap), out_lanes = dp_cap_lanes(out_cap);
    if (dp_bandwidth(out_rate, out_lanes) > max_bandwidth) {
      static const uint32_t kReduced[][2] = {{8100, 4}, {5400, 4}, {8100, 2}, {2700, 4}, {5400, 2}, {8100, 1}, {1620, 4}, {5400, 1}, {2700, 2}, {1620, 2}, {2700, 1}, {1620, 1}};
      bool found = false;
      for (const auto& combination : kReduced) {
        if (combination[0] > out_rate || combination[1] > out_lanes || combination[0] > in_rate || combination[1] > in_lanes) continue;
        if (dp_bandwidth(combination[0], combination[1]) > max_bandwidth) continue;
        const uint32_t rate_index = combination[0] == 1620 ? 0 : combination[0] == 2700 ? 1 : combination[0] == 5400 ? 2 : 3;
        const uint32_t lanes_index = combination[1] == 1 ? 0 : combination[1] == 2 ? 1 : 2;
        out_cap = (out_cap & ~(kDpCommonRateMask | kDpCommonLanesMask)) | (rate_index << kDpCommonRateShift) | (lanes_index << kDpCommonLanesShift);
        found = true;
        break;
      }
      if (!found) {
        if (error) *error = "not enough bandwidth for the stream";
        return false;
      }
    }
  }
  return port_write(out, out_port, o.cap_adapter + kDpRemoteCap, ArrayView<const uint32_t>(&in_cap, 1)) && port_write(in, in_port, i.cap_adapter + kDpRemoteCap, ArrayView<const uint32_t>(&out_cap, 1));
}

// A path through the routers in `routers` (in the order of travel; each is the parent or a child of the next), from `first_port` of the first, entering on the HopID `first_hop`, to
// `last_port` of the last, leaving on `last_hop`. The HopIDs between are the lowest free of both ends of each link.
bool Usb4Domain::build_chain_path(Usb4Path* path, const int* routers, uint32_t n, uint32_t first_port, uint32_t first_hop, uint32_t last_port, uint32_t last_hop) {
  *path = Usb4Path();
  if (n == 0 || n > kMaxHops) return false;
  auto upstream_lane = [&](const Usb4Router& r) {
    uint32_t lane = r.upstream_port;
    if (r.ports[lane].link_nr != 0 && r.ports[lane].dual_link >= 0) lane = static_cast<uint32_t>(r.ports[lane].dual_link);
    return lane;
  };
  uint32_t in_port[kMaxHops], out_port[kMaxHops];
  for (uint32_t i = 0; i < n; ++i) {
    const Usb4Router& r = *routers_[routers[i]];
    // where the packet enters and leaves this router: the given adapters at the ends, the lane adapters of the links in between
    if (i == 0) in_port[i] = first_port;
    else in_port[i] = routers_[routers[i - 1]]->parent == routers[i] ? routers_[routers[i - 1]]->parent_port : upstream_lane(r);
    if (i + 1 == n) out_port[i] = last_port;
    else out_port[i] = r.parent == routers[i + 1] ? upstream_lane(r) : routers_[routers[i + 1]]->parent_port;
  }
  uint32_t in_hop = first_hop;
  bool ok = take_hop(*routers_[routers[0]], in_port[0], true, in_hop);
  for (uint32_t i = 0; ok && i < n; ++i) {
    Usb4Router& r = *routers_[routers[i]];
    Usb4Hop& hop = path->hops[path->length++];
    hop = Usb4Hop{static_cast<int16_t>(routers[i]), static_cast<uint8_t>(in_port[i]), static_cast<uint8_t>(out_port[i]), static_cast<uint16_t>(in_hop), 0, 0, 0};
    uint32_t out_hop = 0;
    if (i + 1 == n) {
      out_hop = last_hop;
      ok = take_hop(r, out_port[i], false, out_hop);
    } else {
      Usb4Router& next = *routers_[routers[i + 1]];
      for (uint32_t h = kMinPathHop; h < kHopIdLimit && out_hop == 0; ++h) {
        if (h > r.ports[out_port[i]].max_out_hop || h > next.ports[in_port[i + 1]].max_in_hop) break;
        if (!bit_test(r.ports[out_port[i]].out_hops, h) && !bit_test(next.ports[in_port[i + 1]].in_hops, h)) out_hop = h;
      }
      ok = out_hop != 0 && take_hop(r, out_port[i], false, out_hop);
      if (ok && !take_hop(next, in_port[i + 1], true, out_hop)) {
        release_hop(r, out_port[i], false, out_hop);
        ok = false;
        out_hop = 0;
      }
    }
    if (!ok) break;
    hop.out_hop = static_cast<uint16_t>(out_hop);
    in_hop = out_hop;
  }
  if (!ok) {
    release_path(path);
    return false;
  }
  return true;
}

bool Usb4Domain::tunnel_dp(uint32_t in_router, uint32_t in_port, uint32_t out_router, uint32_t out_port, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (!started_ || in_router >= routers_.count() || out_router >= routers_.count() || !routers_[in_router]->present || !routers_[out_router]->present) {
    *error = "no such router";
    return false;
  }
  Usb4Router& in = *routers_[in_router];
  Usb4Router& out = *routers_[out_router];
  if (in_port == 0 || in_port > in.max_port || out_port == 0 || out_port > out.max_port || in.ports[in_port].type != kTypeDpIn || out.ports[out_port].type != kTypeDpOut ||
      in.ports[in_port].cap_adapter == 0 || out.ports[out_port].cap_adapter == 0) {
    *error = "not a DisplayPort input and output";
    return false;
  }
  // the output is below the input: the routers from the output up to the input
  int chain_up[kMaxHops];
  uint32_t n = 0;
  for (int r = static_cast<int>(out_router); r >= 0 && n < kMaxHops; r = routers_[r]->parent) {
    chain_up[n++] = r;
    if (r == static_cast<int>(in_router)) break;
  }
  if (n == 0 || chain_up[n - 1] != static_cast<int>(in_router)) {
    *error = "the output is not below the input";
    return false;
  }
  for (uint32_t i = 0; i < kMaxTunnels; ++i) {
    if (!tunnels_[i].used || tunnels_[i].kind != Usb4TunnelKind::Dp) continue;
    if ((tunnels_[i].host_router == static_cast<int>(in_router) && tunnels_[i].down_port == in_port) || (tunnels_[i].device_router == static_cast<int>(out_router) && tunnels_[i].up_port == out_port)) {
      *error = "an adapter is in use already";
      return false;
    }
  }
  int slot = -1;
  for (uint32_t i = 0; i < kMaxTunnels && slot < 0; ++i)
    if (!tunnels_[i].used) slot = static_cast<int>(i);
  if (slot < 0) {
    *error = "too many tunnels";
    return false;
  }
  // another connection manager (the BIOS's, or the one of a router in between) may hold the input: ask before anything is written
  if (!dp_resource(in, kOpQueryDpResource, in_port) || !dp_resource(in, kOpAllocDpResource, in_port)) {
    *error = "the DisplayPort input belongs to another connection manager";
    return false;
  }
  Usb4Tunnel& t = tunnels_[slot];
  t = Usb4Tunnel();
  t.used = true;
  t.kind = Usb4TunnelKind::Dp;
  t.host_router = static_cast<int16_t>(in_router);
  t.device_router = static_cast<int16_t>(out_router);
  t.down_port = static_cast<uint8_t>(in_port);
  t.up_port = static_cast<uint8_t>(out_port);
  auto fail = [&](const char* why) {
    remove_tunnel(slot);
    *error = why;
    return false;
  };
  int chain_down[kMaxHops];
  for (uint32_t i = 0; i < n; ++i) chain_down[i] = chain_up[n - 1 - i];
  // video and AUX transmit from the input to the output, AUX receive back
  if (!build_chain_path(&t.down, chain_down, n, in_port, kDpVideoHop, out_port, kDpVideoHop) || !build_chain_path(&t.up, chain_down, n, in_port, kDpAuxTxHop, out_port, kDpAuxTxHop) ||
      !build_chain_path(&t.third, chain_up, n, out_port, kDpAuxRxHop, in_port, kDpAuxRxHop))
    return fail("no HopIDs left for the paths");
  const bool pm = in.usb4_version >= 2;
  // video: no flow control, buffers that are not flow controlled; AUX: flow control, one credit (the router's minimum with an allocation)
  t.down.priority = kDpVideoPriority; t.down.weight = kDpWeight; t.down.egress_fc_enable = 0; t.down.ingress_fc_enable = 0; t.down.pm_support = pm;
  t.up.priority = kDpAuxPriority; t.up.weight = kDpWeight; t.up.pm_support = pm;
  t.third.priority = kDpAuxPriority; t.third.weight = kDpWeight; t.third.pm_support = pm;
  Usb4Path* aux[2] = {&t.up, &t.third};
  for (Usb4Path* path : aux) {
    for (uint32_t i = 0; i < path->length; ++i) {
      const Usb4Router& r = *routers_[path->hops[i].router];
      const Usb4Port& p = r.ports[path->hops[i].in_port];
      path->hops[i].credits = lane_uses_allocation(r, p) ? static_cast<uint8_t>(r.min_dp_aux_credits) : 1;
    }
  }
  for (uint32_t i = 0; i < t.down.length; ++i) {
    const Usb4Router& r = *routers_[t.down.hops[i].router];
    const Usb4Port& p = r.ports[t.down.hops[i].in_port];
    if (lane_uses_allocation(r, p)) {
      t.down.hops[i].nfc_credits = static_cast<uint8_t>(r.min_dp_main_credits);
    } else {
      const uint32_t usable = p.total_credits > 2 ? p.total_credits - 2u : 0;
      t.down.hops[i].nfc_credits = static_cast<uint8_t>(usable < 12 ? usable : 12);
    }
  }
  // a stream without the allocation mode is cut down to what the links have free, if that is less than the output would send
  uint32_t input_cap = 0;
  const bool alloc_mode = port_read(in, in_port, in.ports[in_port].cap_adapter + kDpLocalCap, ArrayView<uint32_t>(&input_cap, 1)) && (input_cap & kDpCommonBwMode) && in.usb4_version != 0 && out.usb4_version != 0;
  int limit = 0;
  if (!alloc_mode) {
    limit = dp_available(slot);
    if (limit == 0) return fail("no bandwidth left on the links");
    if (limit < 0 || limit == 0x7FFFFFFF) limit = 0;
  }
  const char* why = "the adapters did not exchange their capabilities";
  if (!dp_exchange_caps(in, in_port, out, out_port, static_cast<uint32_t>(limit), &why)) return fail(why);
  if (!dp_enable_bandwidth_mode(slot)) return fail("the bandwidth allocation mode could not be set up");
  if (!activate_path(&t.down) || !activate_path(&t.up) || !activate_path(&t.third)) return fail("the paths could not be written");
  if (!set_dp_enable(in, in_port, true) || !set_dp_enable(out, out_port, true)) return fail("the DisplayPort adapters could not be enabled");
  t.active = true;
  dp_recalc_estimates();
  return true;
}

bool Usb4Domain::disconnect_dp(uint32_t in_router, uint32_t in_port) {
  for (uint32_t i = 0; i < kMaxTunnels; ++i) {
    if (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp && tunnels_[i].host_router == static_cast<int>(in_router) && tunnels_[i].down_port == in_port) {
      const bool ok = remove_tunnel(static_cast<int>(i));
      dp_recalc_estimates();                                 // (the others can have what it held)
      return ok;
    }
  }
  return false;
}

bool Usb4Domain::dp_find_output(uint32_t in_router, uint32_t in_port, uint32_t* out_router, uint32_t* out_port) {
  if (!started_ || in_router >= routers_.count() || !routers_[in_router]->present || in_port == 0 || in_port > routers_[in_router]->max_port || routers_[in_router]->ports[in_port].type != kTypeDpIn) return false;
  auto top_port = [&](uint32_t router) {                    // the host router's lane adapter that leads to the router (0 for the host router itself)
    int r = static_cast<int>(router);
    if (routers_[r]->parent < 0) return 0u;
    while (routers_[routers_[r]->parent]->parent >= 0) r = routers_[r]->parent;
    return static_cast<uint32_t>(routers_[r]->parent_port);
  };
  const uint32_t wanted = top_port(in_router);
  for (uint32_t r = 0; r < routers_.count(); ++r) {
    const Usb4Router& router = *routers_[r];
    if (!router.present || r == in_router || (wanted != 0 && top_port(r) != wanted)) continue;
    for (uint32_t p = 1; p <= router.max_port; ++p) {
      const Usb4Port& port = router.ports[p];
      if (port.type != kTypeDpOut || port.cap_adapter == 0) continue;
      bool used = false;
      for (uint32_t i = 0; i < kMaxTunnels; ++i) used = used || (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp && tunnels_[i].device_router == static_cast<int>(r) && tunnels_[i].up_port == p);
      uint32_t word = 0;
      if (used || !port_read(router, p, port.cap_adapter + kDpOutCs2, ArrayView<uint32_t>(&word, 1)) || !(word & kDpOutCs2Hpd)) continue;
      *out_router = r;
      *out_port = p;
      return true;
    }
  }
  return false;
}

uint32_t Usb4Domain::dp_connect_displays() {
  uint32_t made = 0;
  for (uint32_t r = 0; r < routers_.count(); ++r) {
    if (!routers_[r]->present) continue;
    for (uint32_t p = 1; p <= routers_[r]->max_port; ++p) {
      if (routers_[r]->ports[p].type != kTypeDpIn || dp_find(r, p) >= 0) continue;
      uint32_t out_router = 0, out_port = 0;
      if (dp_find_output(r, p, &out_router, &out_port) && tunnel_dp(r, p, out_router, out_port, nullptr)) ++made;
    }
  }
  return made;
}

uint32_t Usb4Domain::dp_tunnels() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < kMaxTunnels; ++i) n += tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp;
  return n;
}

// ------------------------------------------------------------------ CL states

bool Usb4Domain::is_titan_ridge(const Usb4Router& r) {
  return r.vendor_id == 0x8086 && (r.device_id == 0x15E7 || r.device_id == 0x15EA || r.device_id == 0x15EF);
}

// Of the Thunderbolt 3 routers only Titan Ridge has CL states (tb_switch_clx_is_supported).
bool Usb4Domain::clx_supported(const Usb4Router& r) const {
  if (r.tbt3 ? !is_titan_ridge(r) : r.usb4_version == 0) return false;
  if (r.vendor_id == 0x8086 && (r.device_id == 0x9A1B || r.device_id == 0x9A1D || r.device_id == 0x9A1F || r.device_id == 0x9A21)) return false;     // (tb_switch_is_tiger_lake)
  return true;
}

// tb_port_clx_supported: a bonded link, no other host, the port says it can (PORT_CS_18.CPS), and the lane adapter lists the states.
bool Usb4Domain::port_clx_supported(const Usb4Router& r, uint32_t port, uint8_t states) {
  const Usb4Port& p = r.ports[port];
  if (!p.bonded || p.xdomain || p.cap_phy == 0) return false;
  uint32_t word = 0;
  if (r.tbt3) {                                              // (the link controller says whether the link, an active cable included, supports it: tb_lc_is_clx_supported)
    uint32_t block = 0;
    if (!lc_port_block(r, port, &block) || block + kLcLinkAttr >= 0x1F00 || !read(r.route, 0, TbConfigSpace::Switch, block + kLcLinkAttr, ArrayView<uint32_t>(&word, 1)) || !(word & kLcLinkAttrCps)) return false;
  } else if (p.cap_usb4 == 0 || !port_read(r, port, p.cap_usb4 + kPortCs18, ArrayView<uint32_t>(&word, 1)) || !(word & kPortCs18Cps)) {
    return false;
  }
  uint32_t mask = 0;
  if (states & kClx0s) mask |= kLaneCs0Cl0sSupport;
  if (states & kClx1) mask |= kLaneCs0Cl1Support;
  if (states & kClx2) mask |= kLaneCs0Cl2Support;
  return port_read(r, port, p.cap_phy + kLaneCs0, ArrayView<uint32_t>(&word, 1)) && (word & mask) != 0;
}

bool Usb4Domain::port_clx_set(const Usb4Router& r, uint32_t port, uint8_t states, bool enable) {
  const Usb4Port& p = r.ports[port];
  uint32_t mask = 0;
  if (states & kClx0s) mask |= kLaneCs1Cl0sEnable;
  if (states & kClx1) mask |= kLaneCs1Cl1Enable;
  if (states & kClx2) mask |= kLaneCs1Cl2Enable;
  uint32_t word = 0;
  if (mask == 0 || p.cap_phy == 0 || !port_read(r, port, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return false;
  word = enable ? (word | mask) : (word & ~mask);
  return port_write(r, port, p.cap_phy + kLaneCs1, ArrayView<const uint32_t>(&word, 1));
}

// tb_switch_clx_enable: CL1 needs CL0s; CL2 needs version 2 routers on both sides; the upstream adapter of the router is made the power management secondary and the downstream one the primary; both
// ends must support the states; the upstream end is enabled first and undone if the downstream one fails.
bool Usb4Domain::clx_enable(Usb4Router& child, uint8_t states) {
  if (states == 0 || child.clx == states || ((states & kClx1) && !(states & kClx0s)) || child.parent < 0) return false;
  Usb4Router& parent = *routers_[child.parent];
  if (!clx_supported(child) || !clx_supported(parent)) return false;
  if ((states & kClx2) && (child.usb4_version < 2 || parent.usb4_version < 2)) return false;
  const uint32_t up = child.upstream_port, down = child.parent_port;
  const uint8_t both[2] = {0, 1};
  (void)both;
  {   // power management: the upstream adapter is the secondary
    const Usb4Port& u = child.ports[up];
    const Usb4Port& d = parent.ports[down];
    uint32_t word = 0;
    if (u.cap_phy == 0 || d.cap_phy == 0) return false;
    if (!port_read(child, up, u.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return false;
    word |= kLaneCs1Pms;
    if (!port_write(child, up, u.cap_phy + kLaneCs1, ArrayView<const uint32_t>(&word, 1))) return false;
    if (!port_read(parent, down, d.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return false;
    word &= ~kLaneCs1Pms;
    if (!port_write(parent, down, d.cap_phy + kLaneCs1, ArrayView<const uint32_t>(&word, 1))) return false;
  }
  if (!port_clx_supported(child, up, states) || !port_clx_supported(parent, down, states)) return false;
  if (!port_clx_set(child, up, states, true)) return false;
  if (!port_clx_set(parent, down, states, true)) {
    port_clx_set(child, up, states, false);
    return false;
  }
  if (!mask_clx_objections(child)) {
    port_clx_set(child, up, states, false);
    port_clx_set(parent, down, states, false);
    return false;
  }
  child.clx = static_cast<uint8_t>(child.clx | states);
  return true;
}

// A Titan Ridge below the host has two dual-lane ports (lanes 1 2 and 3 4): the objections of the port that is not the upstream one are masked in the low power capability, those of the
// upstream one unmasked (tb_switch_mask_clx_objections).
bool Usb4Domain::mask_clx_objections(const Usb4Router& child) {
  if (!child.tbt3 || !is_titan_ridge(child)) return true;
  const int cap = find_router_vse(child, kVsecLowPower);
  if (cap < 0) return false;
  const bool port_a = child.upstream_port == 1;
  const uint32_t mask = port_a ? kLowPwrPortBMask : kLowPwrPortAMask, unmask = port_a ? kLowPwrPortAMask : kLowPwrPortBMask, offset = static_cast<uint32_t>(cap) + (port_a ? kLowPwrC1Cl1 : kLowPwrC3Cl1);
  uint32_t val[2] = {};
  if (offset + 2 >= 0x1F00 || !read(child.route, 0, TbConfigSpace::Switch, offset, ArrayView<uint32_t>(val, 2))) return false;
  for (uint32_t& v : val) v = (v | mask) & ~unmask;
  return write(child.route, 0, TbConfigSpace::Switch, offset, ArrayView<const uint32_t>(val, 2));
}

bool Usb4Domain::clx_disable(Usb4Router& child) {
  if (child.clx == 0 || child.parent < 0) return false;
  Usb4Router& parent = *routers_[child.parent];
  const bool ok = port_clx_set(child, child.upstream_port, child.clx, false) && port_clx_set(parent, child.parent_port, child.clx, false);
  if (ok) child.clx = 0;
  return ok;
}

// Only the first link below the host router (tb_enable_clx): with CL2 if the routers allow, else CL0s and CL1.
void Usb4Domain::clx_enable_first_link(int router_index) {
  if (!clx_policy_) return;
  Usb4Router* r = routers_[router_index].get();
  while (r->parent >= 0 && routers_[r->parent]->parent >= 0) r = routers_[r->parent].get();
  if (r->parent < 0) return;
  for (uint32_t i = 0; i < kMaxTunnels; ++i) {               // (a path to another host through this link keeps them off)
    if (!tunnels_[i].used || tunnels_[i].kind != Usb4TunnelKind::Dma) continue;
    for (uint32_t h = 0; h < tunnels_[i].down.length; ++h)
      if (routers_[tunnels_[i].down.hops[h].router].get() == r) return;
  }
  if (!clx_enable(*r, kClx0s | kClx1 | kClx2)) clx_enable(*r, kClx0s | kClx1);
}

// tb_disable_clx: from this router up to the host router.
void Usb4Domain::clx_disable_up(int router_index) {
  for (Usb4Router* r = routers_[router_index].get(); r && r->parent >= 0; r = routers_[r->parent].get()) clx_disable(*r);
}

bool Usb4Domain::set_clx(uint32_t router_index, uint8_t states) {
  if (router_index >= routers_.count() || !routers_[router_index]->present) return false;
  Usb4Router& r = *routers_[router_index];
  if (states == 0) return r.clx == 0 || clx_disable(r);
  return clx_enable(r, states);
}

// ------------------------------------------------------------------ DisplayPort bandwidth

int Usb4Domain::dp_find(uint32_t in_router, uint32_t in_port) const {
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp && tunnels_[i].host_router == static_cast<int>(in_router) && tunnels_[i].down_port == in_port) return static_cast<int>(i);
  return -1;
}

bool Usb4Domain::dp_bandwidth_mode(uint32_t in_router, uint32_t in_port) const { const int s = dp_find(in_router, in_port); return s >= 0 && tunnels_[s].bw_mode; }
uint32_t Usb4Domain::dp_allocated_bandwidth(uint32_t in_router, uint32_t in_port) const { const int s = dp_find(in_router, in_port); return s >= 0 ? tunnels_[s].allocated_bw : 0; }
uint32_t Usb4Domain::dp_maximum_bandwidth(uint32_t in_router, uint32_t in_port) const { const int s = dp_find(in_router, in_port); return s >= 0 ? tunnels_[s].max_bw : 0; }

// The speed of one lane of the link in Gb/s (tb_port_get_link_speed): generation 4 40, generation 3 20, else 10.
int Usb4Domain::link_speed(const Usb4Router& r, uint32_t port) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  if (p.cap_phy == 0 || !port_read(r, port, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return -1;
  const uint32_t speed = (word & kLaneCs1CurrentSpeedMask) >> kLaneCs1CurrentSpeedShift;
  return speed == kLaneSpeedGen4 ? 40 : speed == kLaneSpeedGen3 ? 20 : 10;
}

// What the link above a router can carry from the host towards the router, in Mb/s (tb_maximum_bandwidth, the downstream direction): speed times the lanes that carry that way - two when bonded,
// one when not, one of an asymmetric link that transmits three towards the host and three when it transmits one.
int Usb4Domain::link_down_capacity(const Usb4Router& child) {
  const int speed = link_speed(child, child.upstream_port);
  const int width = lane_width(child, child.upstream_port);
  if (speed < 0 || width <= 0) return -1;
  const int lanes = width == static_cast<int>(Usb4LinkWidth::AsymTx) ? 1 : width == static_cast<int>(Usb4LinkWidth::AsymRx) ? 3 : width;
  return speed * lanes * 1000;
}

// What is left for the stream in slot `slot` on the links between its output and its input: each link's capacity less the allocations of the other DisplayPort streams that cross it and the
// 1500 Mb/s a PCIe tunnel between two version 2 routers keeps for itself (tb_available_bandwidth, downstream).
int Usb4Domain::dp_available(int slot) {
  const Usb4Tunnel& t = tunnels_[slot];
  int available = 0x7FFFFFFF;
  for (int r = t.device_router; r >= 0 && r != t.host_router; r = routers_[r]->parent) {
    const Usb4Router& router = *routers_[r];
    int capacity = link_down_capacity(router);
    if (capacity < 0) return -1;
    for (uint32_t i = 0; i < kMaxTunnels; ++i) {
      const Usb4Tunnel& other = tunnels_[i];
      if (static_cast<int>(i) == slot || !other.used || other.kind != Usb4TunnelKind::Dp || !other.bw_mode) continue;
      for (uint32_t h = 0; h + 1 < other.third.length; ++h)
        if (other.third.hops[h].router == r) { capacity -= static_cast<int>(other.allocated_bw); break; }
    }
    uint32_t groups = 0;                                     // (what is kept for the groups of the streams that cross the link, once per group, the stream's own included)
    for (uint32_t i = 0; i < kMaxTunnels; ++i) {
      const Usb4Tunnel& other = tunnels_[i];
      if (!other.used || other.kind != Usb4TunnelKind::Dp || !other.bw_mode || other.group >= 8) continue;
      for (uint32_t h = 0; h + 1 < other.third.length; ++h)
        if (other.third.hops[h].router == r) { groups |= 1u << other.group; break; }
    }
    for (uint32_t g = 1; g < 8; ++g)
      if (groups & (1u << g)) capacity -= static_cast<int>(group_reserved_[g]);
    if (router.usb4_version >= 2 && router.parent >= 0 && routers_[router.parent]->usb4_version >= 2 && find_tunnel(r, Usb4TunnelKind::Pcie) >= 0) capacity -= 1500;
    const int usb3 = usb3_on_link(r);                        // (what the USB 3 stream is allocated, times the PCIe weight on top of its own: 3 / 2)
    if (usb3 >= 0) capacity -= static_cast<int>(tunnels_[usb3].allocated_bw * 3 / 2);
    if (capacity < available) available = capacity;
  }
  return available < 0 ? 0 : available;
}

bool Usb4Domain::dp_write_estimate(int slot, uint32_t bandwidth) {
  const Usb4Tunnel& t = tunnels_[slot];
  const Usb4Router& in = *routers_[t.host_router];
  const Usb4Port& p = in.ports[t.down_port];
  uint32_t cs2 = 0;
  if (!port_read(in, t.down_port, p.cap_adapter + kDpCs2, ArrayView<uint32_t>(&cs2, 1)) || t.granularity == 0) return false;
  const uint32_t steps = bandwidth / t.granularity;
  cs2 = (cs2 & ~kDpCs2EstimatedMask) | ((steps > 0xFF ? 0xFFu : steps) << kDpCs2EstimatedShift);
  return port_write(in, t.down_port, p.cap_adapter + kDpCs2, ArrayView<const uint32_t>(&cs2, 1));
}

// tb_dp_bandwidth_alloc_mode_enable: the connection manager is declared (id, bandwidth mode supported, group), the non-reduced rate and lanes are the smaller of the two adapters', the
// granularity is the smallest of 250, 500 and 1000 Mb/s that lets the maximum fit 8 bits, the estimate is what the links have free, and nothing is allocated yet.
bool Usb4Domain::dp_enable_bandwidth_mode(int slot) {
  Usb4Tunnel& t = tunnels_[slot];
  Usb4Router& in = *routers_[t.host_router];
  Usb4Router& out = *routers_[t.device_router];
  const Usb4Port& i = in.ports[t.down_port];
  const Usb4Port& o = out.ports[t.up_port];
  uint32_t in_cap = 0, out_cap = 0, cs2 = 0;
  if (!port_read(in, t.down_port, i.cap_adapter + kDpLocalCap, ArrayView<uint32_t>(&in_cap, 1))) return false;
  if (!(in_cap & kDpCommonBwMode) || in.usb4_version == 0 || out.usb4_version == 0) return true;       // (not supported: the stream runs at the full rate, as before)
  if (!port_read(out, t.up_port, o.cap_adapter + kDpLocalCap, ArrayView<uint32_t>(&out_cap, 1))) return false;
  // the non-reduced rate and lanes are those of the lower of the two adapters (no UHBR: DisplayPort 2.0 and below); the maximum through the allocation mode counts the UHBR rates too
  const uint32_t in_rate = dp_cap_rate(in_cap), out_rate = dp_cap_rate(out_cap), in_lanes = dp_cap_lanes(in_cap), out_lanes = dp_cap_lanes(out_cap);
  if (in_rate == 0 || out_rate == 0 || in_lanes == 0 || out_lanes == 0) return false;
  const uint32_t rate_i = (in_rate < out_rate ? in_rate : out_rate) == 1620 ? 0 : (in_rate < out_rate ? in_rate : out_rate) == 2700 ? 1 : (in_rate < out_rate ? in_rate : out_rate) == 5400 ? 2 : 3;
  const uint32_t lanes_n = in_lanes < out_lanes ? in_lanes : out_lanes, lanes_i = lanes_n == 1 ? 0 : lanes_n == 2 ? 1 : 2;
  const uint32_t ext_in = dp_cap_rate_ext(in_cap), ext_out = dp_cap_rate_ext(out_cap);
  const uint32_t max_bw = dp_bandwidth(ext_in < ext_out ? ext_in : ext_out, lanes_n);
  uint32_t granularity = 250;
  while (max_bw / granularity > 255 && granularity <= 1000) granularity *= 2;
  if (granularity > 1000) return false;
  // the connection manager's declaration and the group of streams that share a link: one group per host lane adapter the stream leaves by
  if (!port_read(in, t.down_port, i.cap_adapter + kDpCs2, ArrayView<uint32_t>(&cs2, 1))) return false;
  const uint32_t lane = t.third.length ? t.third.hops[t.third.length - 1].in_port : 1;
  cs2 &= ~(kDpCs2CmIdMask | kDpCs2GroupMask | kDpCs2NrdRateMask | kDpCs2NrdLanesMask | kDpCs2GranularityMask);
  t.group = static_cast<uint8_t>(((lane - 1) / 2) % 7 + 1);
  cs2 |= kDpCs2Cmms | (0u << kDpCs2CmIdShift) | (static_cast<uint32_t>(t.group) << kDpCs2GroupShift) | (rate_i << kDpCs2NrdRateShift) | lanes_i | ((granularity == 250 ? 0u : granularity == 500 ? 1u : 2u) << kDpCs2GranularityShift);
  if (!port_write(in, t.down_port, i.cap_adapter + kDpCs2, ArrayView<const uint32_t>(&cs2, 1))) return false;
  t.granularity = granularity;
  t.max_bw = (max_bw + granularity - 1) / granularity * granularity;
  t.bw_mode = true;
  const int available = dp_available(slot);
  if (available < 0 || !dp_write_estimate(slot, static_cast<uint32_t>(available))) return false;
  return dp_allocate(slot, 0);
}

// usb4_dp_port_allocate_bandwidth: the amount in units of the granularity into DP_STATUS, the connection manager's acknowledge raised, then lowered while the request bit is waited for.
bool Usb4Domain::dp_allocate(int slot, uint32_t bandwidth) {
  Usb4Tunnel& t = tunnels_[slot];
  const Usb4Router& in = *routers_[t.host_router];
  const Usb4Port& p = in.ports[t.down_port];
  uint32_t status = 0, cs2 = 0;
  if (t.granularity == 0 || !port_read(in, t.down_port, p.cap_adapter + kDpStatusCtrl, ArrayView<uint32_t>(&status, 1))) return false;
  status = (status & ~kDpStatusAllocatedMask) | ((bandwidth / t.granularity) << kDpStatusAllocatedShift);
  if (!port_write(in, t.down_port, p.cap_adapter + kDpStatusCtrl, ArrayView<const uint32_t>(&status, 1))) return false;
  if (!port_read(in, t.down_port, p.cap_adapter + kDpCs2, ArrayView<uint32_t>(&cs2, 1))) return false;
  cs2 |= kDpCs2Ca;
  if (!port_write(in, t.down_port, p.cap_adapter + kDpCs2, ArrayView<const uint32_t>(&cs2, 1))) return false;
  cs2 &= ~kDpCs2Ca;
  if (!port_write(in, t.down_port, p.cap_adapter + kDpCs2, ArrayView<const uint32_t>(&cs2, 1))) return false;
  bool done = false;
  for (uint32_t waited = 0; waited < 500000 && !done; waited += 50) {
    uint32_t cs8 = 0;
    if (!port_read(in, t.down_port, p.cap_adapter + kDpCs8, ArrayView<uint32_t>(&cs8, 1))) return false;
    done = !(cs8 & kDpCs8Dr);
    if (!done) delay_us(50);
  }
  if (done) t.allocated_bw = bandwidth;
  return done;
}

// One request of a DP IN adapter: read (granularity times the 8 bit field), refused if the links cannot carry it next to the others, else allocated (never above the stream's maximum).
bool Usb4Domain::dp_serve_request(int slot) {
  Usb4Tunnel& t = tunnels_[slot];
  const Usb4Router& in = *routers_[t.host_router];
  const Usb4Port& p = in.ports[t.down_port];
  uint32_t cs8 = 0;
  if (!port_read(in, t.down_port, p.cap_adapter + kDpCs8, ArrayView<uint32_t>(&cs8, 1)) || !(cs8 & kDpCs8Dr)) return false;
  uint32_t requested = (cs8 & kDpCs8RequestedMask) * t.granularity;
  if (requested > t.max_bw) requested = t.max_bw;
  if (requested <= t.allocated_bw) {                         // (less or the same: what is given back is kept for the group for a while, tb_alloc_dp_bandwidth)
    if (t.group < 8 && t.allocated_bw > requested) {
      group_reserved_[t.group] += t.allocated_bw - requested;
      group_timer_[t.group] = kGroupReleaseMs;
    }
    return dp_allocate(slot, requested);
  }
  usb3_release_unused(slot);                                // (USB 3 gives back what it holds without using, to make room)
  const int available = dp_available(slot);
  const uint32_t reserved = t.group < 8 ? group_reserved_[t.group] : 0;
  if (available < 0 || requested > static_cast<uint32_t>(available) + reserved) {
    dp_allocate(slot, t.allocated_bw);                       // (not enough: the adapter is acknowledged with what it has, so that it stops asking and settles for that)
    usb3_reclaim();
    return false;
  }
  const bool ok = dp_allocate(slot, requested);
  if (ok && t.group < 8 && requested > static_cast<uint32_t>(available)) {     // (the part that came from the group's keep)
    const uint32_t used = requested - static_cast<uint32_t>(available);
    group_reserved_[t.group] -= used < group_reserved_[t.group] ? used : group_reserved_[t.group];
  }
  usb3_reclaim();
  return ok;
}

void Usb4Domain::tick(uint64_t now_ms) {
  if (ticked_ && now_ms > last_tick_ms_) {
    const uint64_t delta = now_ms - last_tick_ms_;
    advance_time(delta > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(delta));
  }
  ticked_ = true;
  last_tick_ms_ = now_ms;
}

// Time passes: a group's keep is let go when its 10 s are over, and the estimates of the streams are made again.
void Usb4Domain::advance_time(uint32_t milliseconds) {
  bool released = false;
  for (uint32_t g = 1; g < 8; ++g) {
    if (group_timer_[g] == 0) continue;
    group_timer_[g] = milliseconds >= group_timer_[g] ? 0 : group_timer_[g] - milliseconds;
    if (group_timer_[g] == 0 && group_reserved_[g] != 0) {
      group_reserved_[g] = 0;
      released = true;
    }
  }
  if (released) dp_recalc_estimates();
}

// tb_release_unused_usb3_bandwidth: the USB 3 streams on the links this stream crosses are cut down to what they consume.
void Usb4Domain::usb3_release_unused(int dp_slot) {
  for (int r = tunnels_[dp_slot].device_router; r >= 0 && r != tunnels_[dp_slot].host_router; r = routers_[r]->parent) {
    const int u = usb3_on_link(r);
    if (u < 0) continue;
    uint32_t allocated = 0;
    if (usb3_set_allocation(*routers_[tunnels_[u].host_router], tunnels_[u].down_port, 0, 0, true, &allocated)) tunnels_[u].allocated_bw = allocated;
  }
}

// tb_reclaim_usb3_bandwidth: each USB 3 stream at the host takes back what is free, up to 90% of its link rate, never less than it has.
void Usb4Domain::usb3_reclaim() {
  for (uint32_t i = 0; i < kMaxTunnels; ++i) {
    Usb4Tunnel& u = tunnels_[i];
    if (!u.used || u.kind != Usb4TunnelKind::Usb3 || routers_[u.host_router]->parent >= 0) continue;
    const Usb4Router& router = *routers_[u.device_router];
    int capacity = link_down_capacity(router);
    if (capacity < 0) continue;
    for (uint32_t k = 0; k < kMaxTunnels; ++k) {              // (less what the DisplayPort streams across it hold)
      const Usb4Tunnel& dp = tunnels_[k];
      if (!dp.used || dp.kind != Usb4TunnelKind::Dp || !dp.bw_mode) continue;
      for (uint32_t h = 0; h + 1 < dp.third.length; ++h)
        if (dp.third.hops[h].router == u.device_router) { capacity -= static_cast<int>(dp.allocated_bw); break; }
    }
    uint32_t max_rate = 9000;                                  // (the 10 Gb/s rate's 90%; a 20 Gb/s adapter's is more, read below)
    const Usb4Router& host = *routers_[u.host_router];
    uint32_t speed = 0;
    if (port_read(host, u.down_port, host.ports[u.down_port].cap_adapter + kUsb3Cs4, ArrayView<uint32_t>(&speed, 1)) && ((speed >> kUsb3Cs4MslrShift) & kUsb3Cs4MslrMask) == kUsb3Cs4Mslr20G) max_rate = 18000;
    uint32_t want = capacity > 0 ? static_cast<uint32_t>(capacity) : 0;                 // (what the link has free, the stream itself not counted)
    if (want > max_rate) want = max_rate;
    if (want <= u.allocated_bw) continue;
    uint32_t allocated = 0;
    if (usb3_set_allocation(host, u.down_port, want, want, false, &allocated)) u.allocated_bw = allocated;
  }
}

// After a change, every stream is told the most it could have now: what the links have free for it, up to its own maximum (tb_recalc_estimated_bandwidth).
void Usb4Domain::dp_recalc_estimates() {
  for (uint32_t i = 0; i < kMaxTunnels; ++i) {
    if (!tunnels_[i].used || tunnels_[i].kind != Usb4TunnelKind::Dp || !tunnels_[i].bw_mode) continue;
    const int available = dp_available(static_cast<int>(i));
    if (available < 0) continue;
    uint32_t estimate = static_cast<uint32_t>(available) + (tunnels_[i].group < 8 ? group_reserved_[tunnels_[i].group] : 0);     // (what the links have free for it, its own allocation not counted against it, the keep of its group added)
    if (estimate > tunnels_[i].max_bw) estimate = tunnels_[i].max_bw;
    dp_write_estimate(static_cast<int>(i), estimate);
  }
}

uint32_t Usb4Domain::dp_poll_requests() {
  uint32_t granted = 0;
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp && tunnels_[i].bw_mode && dp_serve_request(static_cast<int>(i))) ++granted;
  if (granted) dp_recalc_estimates();
  return granted;
}

// ------------------------------------------------------------------ lane bonding

int Usb4Domain::link_generation(const Usb4Router& r, uint32_t port) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  if (p.cap_phy == 0 || !port_read(r, port, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return 0;
  const uint32_t speed = (word & kLaneCs1CurrentSpeedMask) >> kLaneCs1CurrentSpeedShift;
  return speed == kLaneSpeedGen4 ? 4 : speed == kLaneSpeedGen3 ? 3 : speed == 8 ? 2 : 0;
}

bool Usb4Domain::asym_supported(const Usb4Router& r, uint32_t port) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  return p.cap_usb4 != 0 && port_read(r, port, p.cap_usb4 + kPortCs18, ArrayView<uint32_t>(&word, 1)) && (word & kPortCs18Csa);
}

// usb4_port_asym_set_link_width: the target of the asymmetric width bits of the lane adapter (dual is 0 there).
bool Usb4Domain::set_asym_width(const Usb4Router& r, uint32_t port, Usb4LinkWidth width) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  if (p.cap_phy == 0 || !port_read(r, port, p.cap_phy + kLaneCs1, ArrayView<uint32_t>(&word, 1))) return false;
  word &= ~kLaneCs1TargetAsymMask;
  if (width == Usb4LinkWidth::AsymTx) word |= kLaneCs1TargetAsymTx;
  else if (width == Usb4LinkWidth::AsymRx) word |= kLaneCs1TargetAsymRx;
  else if (width != Usb4LinkWidth::Dual) return false;
  return port_write(r, port, p.cap_phy + kLaneCs1, ArrayView<const uint32_t>(&word, 1));
}

// usb4_port_asym_start: START_ASYM is written and must clear within a second (the transition began), then TIP must clear within five (it is done).
bool Usb4Domain::asym_start(const Usb4Router& r, uint32_t port) {
  const Usb4Port& p = r.ports[port];
  uint32_t word = 0;
  if (p.cap_usb4 == 0 || !port_read(r, port, p.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1))) return false;
  word |= kPortCs19StartAsym;
  if (!port_write(r, port, p.cap_usb4 + kPortCs19, ArrayView<const uint32_t>(&word, 1))) return false;
  bool started = false;
  for (uint32_t waited = 0; waited < 1000000 && !started; waited += 1000) {
    started = port_read(r, port, p.cap_usb4 + kPortCs19, ArrayView<uint32_t>(&word, 1)) && !(word & kPortCs19StartAsym);
    if (!started) delay_us(1000);
  }
  if (!started) return false;
  for (uint32_t waited = 0; waited < 5000000; waited += 1000) {
    if (!port_read(r, port, p.cap_usb4 + kPortCs18, ArrayView<uint32_t>(&word, 1))) return false;
    if (!(word & kPortCs18Tip)) return true;
    delay_us(1000);
  }
  return false;
}

bool Usb4Domain::wait_link_width(const Usb4Router& r, uint32_t port, uint32_t widths, uint32_t timeout_us) {
  for (uint32_t waited = 0; waited < timeout_us; waited += 1000) {
    const int width = lane_width(r, port);
    if (width > 0 && (static_cast<uint32_t>(width) & widths)) return true;
    delay_us(1000);
  }
  return false;
}

Usb4LinkWidth Usb4Domain::link_width(uint32_t router_index) {
  const Usb4Router* r = router(router_index);
  if (!r || r->parent < 0) return static_cast<Usb4LinkWidth>(0);
  const int width = lane_width(*r, r->upstream_port);
  return width > 0 ? static_cast<Usb4LinkWidth>(width) : static_cast<Usb4LinkWidth>(0);
}

bool Usb4Domain::set_link_width(uint32_t router_index, Usb4LinkWidth width, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  Usb4Router* child = router_index < routers_.count() && routers_[router_index]->present ? routers_[router_index].get() : nullptr;
  if (!started_ || !child || child->parent < 0) {
    *error = "no such router";
    return false;
  }
  Usb4Router& parent = *routers_[child->parent];
  const uint32_t up = child->upstream_port, down = child->parent_port;
  if (child->usb4_version < 2 || parent.usb4_version < 2 || link_generation(*child, up) < 4 || !asym_supported(*child, up) || !asym_supported(parent, down)) {
    *error = "the link cannot be asymmetric";
    return false;
  }
  for (uint32_t i = 0; i < kMaxTunnels; ++i)
    if (tunnels_[i].used && tunnels_[i].kind == Usb4TunnelKind::Dp && (tunnels_[i].device_router == static_cast<int>(router_index) || tunnels_[i].host_router == static_cast<int>(router_index))) {
      *error = "a DisplayPort stream runs over the link";
      return false;
    }
  const int current = lane_width(*child, up);
  if (current <= 0) {
    *error = "the link width cannot be read";
    return false;
  }
  if (static_cast<uint32_t>(current) == static_cast<uint32_t>(width)) return true;
  bool ok;
  if (width == Usb4LinkWidth::Dual) {
    // tb_switch_asym_disable: both ends back to dual; the router that had three transmit lanes starts the change
    ok = set_asym_width(*child, up, Usb4LinkWidth::Dual) && set_asym_width(parent, down, Usb4LinkWidth::Dual);
    if (ok && current > static_cast<int>(Usb4LinkWidth::Dual)) ok = asym_start(current == static_cast<int>(Usb4LinkWidth::AsymTx) ? *child : parent, current == static_cast<int>(Usb4LinkWidth::AsymTx) ? up : down) &&
                                                                  wait_link_width(*child, up, static_cast<uint32_t>(Usb4LinkWidth::Dual), 100000);
  } else if (width == Usb4LinkWidth::AsymTx || width == Usb4LinkWidth::AsymRx) {
    // tb_switch_asym_enable: the device router's width as asked, the host's the opposite; the router that gains the third transmit lane starts the change
    const Usb4LinkWidth host_width = width == Usb4LinkWidth::AsymTx ? Usb4LinkWidth::AsymRx : Usb4LinkWidth::AsymTx;
    const bool start_on_host = width == Usb4LinkWidth::AsymTx;
    if (current == static_cast<int>(Usb4LinkWidth::AsymTx) || current == static_cast<int>(Usb4LinkWidth::AsymRx)) {
      *error = "go through the symmetric width first";
      return false;
    }
    ok = set_asym_width(*child, up, width) && set_asym_width(parent, down, host_width) && (start_on_host ? asym_start(parent, down) : asym_start(*child, up)) &&
         wait_link_width(*child, up, static_cast<uint32_t>(width), 100000);
  } else {
    *error = "a single lane cannot be asked for on this link";
    return false;
  }
  if (!ok) {
    *error = "the transition did not complete";
    return false;
  }
  child->ports[up].bonded = parent.ports[down].bonded = true;
  return true;
}

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
  if (up.dual_link < 0 || down.dual_link < 0 || !bonding_possible(child)) return false;
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
  const bool gen4 = link_generation(parent, down_port) >= 4;                 // (at generation 4 the width is set through the asymmetric bits: dual is zero there)
  auto set_dual = [&](const Usb4Router& r, uint32_t lane) { return gen4 ? set_asym_width(r, lane, Usb4LinkWidth::Dual) : set_lane_width(r, lane, kLaneCs1TargetDual); };
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
    if (lane_width(*e.router, e.lane0) == 1 && !set_dual(*e.router, e.lane0)) { undo(); return false; }
    const int width1 = lane_width(*e.router, lane1);
    if (width1 == 1 && !set_dual(*e.router, lane1)) { undo(); return false; }
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
    const int now = lane_width(parent, down_port);
    dual = now == static_cast<int>(kLaneCs0WidthDual) || (gen4 && (now == static_cast<int>(Usb4LinkWidth::AsymTx) || now == static_cast<int>(Usb4LinkWidth::AsymRx)));
    if (!dual) delay_us(1000);
  }
  if (!dual) { undo(); return false; }
  up.bonded = down.bonded = true;
  child.ports[up.dual_link].bonded = parent.ports[down.dual_link].bonded = true;
  return true;
}

}  // namespace hardware
