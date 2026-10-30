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

// A model of USB4 routers behind the control channel, for the software connection manager's tests: each router has a header, registers, ports with their spaces and capability lists, and hop tables;
// requests are routed by the route string through the links that are up. It stands in for what tb.c would find on a Tiger Lake machine with docks attached.
#pragma once
#include <map>
#include <memory>
#include <vector>
#include "FakeThunderbolt.hpp"
#include "Hardware/Bus/Thunderbolt/Usb4Domain.hpp"

namespace fake_usb4 {

using namespace hardware::usb4;

struct Port {
  uint32_t type = 0;
  bool implemented = true;
  std::vector<uint32_t> space = std::vector<uint32_t>(256, 0);
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> hops;      // hop index -> two dwords
  uint32_t state = kPortDisabled;                                // lanes: the link state
  bool locked = false;                                           // a downstream lane adapter stays locked until the host clears LCK
};

struct Router {
  uint64_t route = 0;
  bool connected = true;             // the link to it is up (the host's lane adapter says so)
  uint8_t version = 0x20;            // USB4 version 1 in the top three bits
  uint32_t vendor = 0x8086, device = 0x1234;
  uint32_t upstream = 1;
  uint32_t max_port = 0;
  uint32_t regs[512] = {};
  uint32_t first_cap = 0x20;
  std::vector<Port> ports;           // index 0 is the control adapter
  uint32_t cs5_writes = 0, post_times = 0;
  std::vector<uint32_t> buffer_alloc;  // BUFFER_ALLOC entries (index | value << 16); none: the operation is not supported
  std::vector<uint8_t> drom;         // what DROM_READ gives (none: the operation is not supported)
  uint32_t drom_status = 0;          // the status the operation ends with
  uint32_t drom_reads = 0;
};

class Fabric {
 public:
  explicit Fabric(fake_tb::FakeNhi& chip) : chip_(chip) {
    chip_.config_model = [this](bool write, uint64_t route, uint32_t port, uint32_t space, uint32_t offset, uint32_t length, const std::vector<uint32_t>& data) {
      return handle(write, route, port, space, offset, length, data);
    };
  }
  ~Fabric() { chip_.config_model = nullptr; }

  // A router with `lane_pairs` pairs of lane adapters (the first pair's first adapter is the upstream one unless it is the host), `pcie_down` PCIe down adapters and a PCIe up adapter if asked.
  Router& add(uint64_t route, uint32_t lane_pairs, uint32_t pcie_down, bool pcie_up, bool host = false, uint32_t usb3_down = 0, bool usb3_up = false, uint32_t dp_in = 0, uint32_t dp_out = 0) {
    std::unique_ptr<Router> r(new Router);
    r->route = route;
    r->upstream = 1;
    uint32_t n = 0;
    r->ports.push_back(Port());                                  // port 0: the control adapter (no space of its own)
    for (uint32_t i = 0; i < lane_pairs * 2; ++i) r->ports.push_back(lane(++n));
    for (uint32_t i = 0; i < pcie_down; ++i) r->ports.push_back(adapter(++n, kTypePcieDown));
    if (pcie_up) r->ports.push_back(adapter(++n, kTypePcieUp));
    if (host) {                                                  // the host router's upstream port is its NHI adapter
      r->ports.push_back(adapter(++n, kTypeNhi));
      r->upstream = n;
    }
    for (uint32_t i = 0; i < usb3_down; ++i) r->ports.push_back(usb3(++n, kTypeUsb3Down));
    if (usb3_up) r->ports.push_back(usb3(++n, kTypeUsb3Up));
    for (uint32_t i = 0; i < dp_in; ++i) { Port p = adapter(++n, kTypeDpIn); p.space[0x10 + kDpLocalCap] = (3u << kDpCommonRateShift) | (2u << kDpCommonLanesShift); r->ports.push_back(p); }      // HBR3, four lanes
    for (uint32_t i = 0; i < dp_out; ++i) { Port p = adapter(++n, kTypeDpOut); p.space[0x10 + kDpLocalCap] = (2u << kDpCommonRateShift) | (1u << kDpCommonLanesShift); r->ports.push_back(p); }     // HBR2, two lanes
    r->max_port = n;
    r->regs[6] = (1u << 18);                                     // ROUTER_CS_6.HCI: it has an xHCI
    r->regs[7] = 0xA0A0A0A0u + static_cast<uint32_t>(route);
    r->regs[0x20] = (kSwitchCapTmu << 8) | (tmu_ucap ? kTmuRtrCs0Ucap : 0);          // the TMU capability, last in the list
    r->regs[0x20 + kTmuRtrCs1] = 0x11110000u;                                           // the router's local time, for the host router to be read
    r->regs[0x20 + kTmuRtrCs1 + 1] = 0x22222222u;
    r->regs[0x20 + kTmuRtrCs1 + 2] = 0x00003333u;
    r->regs[8] = 0x0B0B0B0Bu;
    Router& ref = *r;
    routers_[route] = std::move(r);
    return ref;
  }
  // Makes the router at `route` a Thunderbolt 3 one: no USB4 version, no USB4 port capabilities, the plug events capability (at 0x28, short form) and the link controller (at 0x30, long form,
  // per-port blocks of 4 dwords from 4 dwords in), the link controller's bonding attribute set.
  Router& make_tbt3(uint64_t route, uint32_t device_id = 0x15EA) {
    Router& r = *routers_[route];
    r.version = 0;
    r.device = device_id;
    r.first_cap = 0x28;
    r.regs[0x28] = 0x30 | (kSwitchCapVse << 8) | (kVsecPlugEvents << 16) | (0x10u << 24);
    r.regs[0x30] = (kSwitchCapVse << 8) | (kVsecLinkController << 16);
    r.regs[0x31] = 0;                                                                    // next: none, length: left zero
    r.regs[0x30 + kLcDesc] = 4u | (4u << 8) | (4u << 16);
    for (uint32_t phys = 0; phys < 4; ++phys) r.regs[0x30 + 4 + phys * 4 + kLcPortAttr] |= kLcPortAttrBe;
    for (Port& p : r.ports) {
      if (p.type != kTypePort) continue;
      p.space[0x10] &= ~0xFFu;                                                           // the lane adapter capability is the last in the list
      p.space[0x20] = 0;
      p.space[0x20 + kPortCs18] = 0;
    }
    return r;
  }
  Router& at(uint64_t route) { return *routers_[route]; }
  bool has(uint64_t route) const { return routers_.count(route) != 0; }
  // Connects the downstream lane adapter `port` of the router at `parent_route` to the router at `route` (the link comes up).
  void link(uint64_t parent_route, uint32_t port, uint64_t route) {
    Router& parent = at(parent_route);
    parent.ports[port].state = kPortUp;
    parent.ports[port].locked = true;
    parent.ports[port].space[4] |= kAdpCs4Lck;
    parent.ports[port + 1].state = kPortUp;
    at(route).connected = true;
    for (uint32_t i : {1u, 2u}) at(route).ports[i].state = kPortUp;
  }
  // Another host on the cable behind the downstream lane adapter `port`: the link is up and no router answers.
  void link_host(uint64_t parent_route, uint32_t port) {
    Router& parent = at(parent_route);
    parent.ports[port].state = kPortUp;
    parent.ports[port].locked = true;
    parent.ports[port].space[4] |= kAdpCs4Lck;
    parent.ports[port + 1].state = kPortUp;
  }
  void unlink_host(uint64_t parent_route, uint32_t port) {
    Router& parent = at(parent_route);
    parent.ports[port].state = kPortUnplugged;
    parent.ports[port + 1].state = kPortUnplugged;
  }
  void unlink(uint64_t parent_route, uint32_t port, uint64_t route) {
    Router& parent = at(parent_route);
    parent.ports[port].state = kPortUnplugged;
    parent.ports[port + 1].state = kPortUnplugged;
    at(route).connected = false;
  }
  bool tmu_ucap = true;                                          // new routers support the uni-directional TMU mode
  bool refuse_sleep = false;                                     // ROUTER_CS_6.SLPR never comes up
  bool gen4_links = false;                                       // every link runs at generation 4 and reports asymmetric widths as asked
  bool asym_stuck = false;                                       // the transition never completes
  bool dp_handshake_never_done = false;                         // CMHS stays set
  bool no_usb3_ack = false;                                      // the USB 3 adapter never grants the connection manager its request
  bool bond_never_completes = false;                             // the link keeps reporting one lane
  uint32_t fail_hop_writes = 0;                                  // the next hop table writes get an error
  uint32_t reads = 0, writes = 0;
  std::vector<std::string> log;                                  // "W route port space offset" of the writes, for order checks
  bool random_garbage = false;                                   // answer reads with noise
  uint32_t noise_seed = 1;

 private:
  static Port lane(uint32_t number) {
    Port p;
    p.type = kTypePort;
    p.space[0] = 0x8086;
    p.space[1] = 0x10;                                           // first capability
    p.space[2] = kTypePort;
    p.space[3] = number << 20;
    p.space[5] = 0x7FF | (0x7FFu << 11);                         // plenty of HopIDs
    p.space[4] = 40u << 20;                                       // total buffers (ADP_CS_4)
    p.space[0x10] = 0x20 | (kPortCapPhy << 8) | (2u << 20) | kLaneCs0Cl0sSupport | kLaneCs0Cl1Support | kLaneCs0Cl2Support;       // the lane adapter capability, next at 0x20; it supports two lanes and the low power states
    p.space[0x20] = 0x40 | (kPortCapUsb4 << 8);                   // the USB4 capability, next the TMU one
    p.space[0x40] = (kPortCapTime1 << 8);
    p.space[0x20 + kPortCs18] = kPortCs18Be | kPortCs18Csa | kPortCs18Cps;       // bonding and asymmetric links supported
    return p;
  }
  static Port usb3(uint32_t number, uint32_t type) {
    Port p = adapter(number, type);
    p.space[0x10 + kUsb3Cs4] = 0;                                  // 10 Gb/s unless a test says 20
    return p;
  }
  static Port adapter(uint32_t number, uint32_t type) {
    Port p;
    p.type = type;
    p.space[0] = 0x8086;
    p.space[1] = 0x10;
    p.space[2] = type;
    p.space[3] = number << 20;
    p.space[5] = 31 | (31u << 11);
    p.space[4] = 40u << 20;
    p.space[0x10] = (kPortCapAdapter << 8);
    return p;
  }
  uint32_t noise() { noise_seed = noise_seed * 1664525u + 1013904223u; return noise_seed; }

  fake_tb::FakeNhi::ConfigAnswer fail(uint32_t code, uint32_t port = 0) {
    fake_tb::FakeNhi::ConfigAnswer a;
    a.handled = true;
    a.error = code;
    a.port = port;
    return a;
  }

  fake_tb::FakeNhi::ConfigAnswer handle(bool write, uint64_t route, uint32_t port, uint32_t space, uint32_t offset, uint32_t length, const std::vector<uint32_t>& data) {
    auto it = routers_.find(route & ((1ull << 54) - 1));
    if (it == routers_.end() || !it->second->connected) return fail(6);                   // nobody there
    Router& r = *it->second;
    // the downstream lane adapter that leads here must have been unlocked
    fake_tb::FakeNhi::ConfigAnswer a;
    a.handled = true;
    a.port = r.upstream;
    if (!write) ++reads; else ++writes;
    if (write) {
      char text[96];
      snprintf(text, sizeof text, "%c %llx %u %u %u", space == 0 ? 'H' : 'W', static_cast<unsigned long long>(route), port, space, offset);
      log.push_back(text);
    }
    if (route != 0) {
      // the parent's port on the way is the lowest byte after the parent's own bytes: find the parent router and its adapter
      uint64_t parent_route = route;
      uint32_t shift = 0;
      while ((parent_route >> (shift + 8)) != 0) shift += 8;
      const uint32_t parent_port = static_cast<uint32_t>((route >> shift) & 0xFF);
      parent_route &= ~(0xFFull << shift);
      auto pit = routers_.find(parent_route);
      if (pit != routers_.end() && parent_port < pit->second->ports.size() && pit->second->ports[parent_port].locked) return fail(6);
    }
    if (space == 2) {                                                               // the router's own space
      if (offset + length > 512) return fail(2);
      if (!write) {
        for (uint32_t i = 0; i < length; ++i) a.data.push_back(read_switch(r, offset + i));
      } else {
        for (uint32_t i = 0; i < length && i < data.size(); ++i) write_switch(r, offset + i, data[i]);
      }
      return a;
    }
    if (port == 0 || port > r.max_port) return fail(6);
    Port& p = r.ports[port];
    if (!p.implemented) return fail(2);
    if (space == 1) {                                                               // port space
      if (offset + length > 256) return fail(2);
      for (uint32_t i = 0; i < length; ++i) {
        if (!write) a.data.push_back(read_port(r, port, offset + i));
        else if (i < data.size()) write_port(r, port, offset + i, data[i]);
      }
      return a;
    }
    if (space == 0) {                                                               // hop table
      if (write && fail_hop_writes > 0) { --fail_hop_writes; return fail(2); }
      const uint32_t index = offset / 2;
      auto& hop = p.hops[index];
      if (!write) {
        for (uint32_t i = 0; i < length; ++i) a.data.push_back((offset + i) % 2 == 0 ? hop.first : hop.second);
      } else {
        for (uint32_t i = 0; i < length && i < data.size(); ++i) ((offset + i) % 2 == 0 ? p.hops[(offset + i) / 2].first : p.hops[(offset + i) / 2].second) = data[i];
      }
      return a;
    }
    return fail(2);
  }

  uint32_t read_switch(Router& r, uint32_t i) {
    if (random_garbage) return noise();
    switch (i) {
      case 0: return r.vendor | (r.device << 16);
      case 1: return r.first_cap | (r.upstream << 8) | (r.max_port << 14) | (r.regs[1] & 0x00700000u) | (0x02u << 24);
      case 2: return r.regs[2];
      case 3: return r.regs[3];
      case 4: return r.regs[4] | (uint32_t{r.version} << 24);
      default: return r.regs[i];
    }
  }
  void write_switch(Router& r, uint32_t i, uint32_t v) {
    if (i == 1) r.regs[1] = (r.regs[1] & ~0x00700000u) | (v & 0x00700000u) , r.upstream = (v >> 8) & 0x3F;
    else if (i == 4) r.regs[4] = v & 0x00FFFFFFu;
    else if (i == 0x20) r.regs[i] = (r.regs[i] & 0x4000FFFFu) | (v & ~0x4000FFFFu);              // the capability header and UCAP are read only
    else if (i == 0x20 + kTmuRtrCs24) { r.regs[i] = 0; r.regs[i + 1] = 0; ++r.post_times; }  // the post time trigger clears itself at once
    else r.regs[i] = v;
    if (i == 26 && (v & kCs26Ov)) {                                                    // a router operation
      if ((v & 0xFFFF) == kOpDromRead && !r.drom.empty()) {
        ++r.drom_reads;
        const uint32_t dwords = (r.regs[25] >> 15) & 0x1F, dwaddress = (r.regs[25] >> 2) & 0x1FFF;
        for (uint32_t d = 0; d < dwords && d < 16; ++d) {
          uint32_t word = 0;
          for (uint32_t k = 0; k < 4; ++k) {
            const size_t at = (dwaddress + d) * 4 + k;
            word |= uint32_t{at < r.drom.size() ? r.drom[at] : uint8_t{0}} << (8 * k);
          }
          r.regs[9 + d] = word;
        }
        r.regs[26] = (r.drom_status << kCs26StatusShift);
      } else if ((v & 0xFFFF) == kOpBufferAlloc && !r.buffer_alloc.empty()) {
        for (uint32_t d = 0; d < 16; ++d) r.regs[9 + d] = d < r.buffer_alloc.size() ? r.buffer_alloc[d] : 0;
        r.regs[25] = static_cast<uint32_t>(r.buffer_alloc.size());
        r.regs[26] = 0;
      } else {
        r.regs[26] = kCs26Ons;
      }
      return;
    }
    if (i == 5) {
      ++r.cs5_writes;
      if ((v & kCs5Slp) && !refuse_sleep) r.regs[6] |= kCs6Slpr;
      if (v & kCs5Cv) r.regs[6] |= kCs6Cr;
    }
  }
  uint32_t read_port(Router& r, uint32_t port, uint32_t i) {
    if (random_garbage) return noise();
    Port& p = r.ports[port];
    if (i == 0x11 && p.type == kTypePort) {                                            // the lane adapter's second dword (PHY capability at 0x10)
      const bool dual = (p.space[0x11] & kLaneCs1TargetWidthMask) == kLaneCs1TargetDual && !bond_never_completes;
      const uint32_t asym = p.space[0x11] & kLaneCs1TargetAsymMask;
      uint32_t width = dual ? 2u : 1u;
      if (gen4_links) width = asym == kLaneCs1TargetAsymTx ? 4u : asym == kLaneCs1TargetAsymRx ? 8u : (dual || width == 2 ? 2u : (bond_never_completes ? 1u : 2u));
      if (asym_stuck) width = 2u;
      return (p.space[0x11] & (kLaneCs1TargetWidthMask | kLaneCs1Lb | kLaneCs1TargetAsymMask | kLaneCs1Cl0sEnable | kLaneCs1Cl1Enable | kLaneCs1Cl2Enable | kLaneCs1Pms)) | (p.state << 26) | (width << kLaneCs1CurrentWidthShift) | ((gen4_links ? kLaneSpeedGen4 : kLaneSpeedGen3) << kLaneCs1CurrentSpeedShift);
    }
    return p.space[i];
  }
  void write_port(Router& r, uint32_t port, uint32_t i, uint32_t v) {
    Port& p = r.ports[port];
    if (i == 4 && p.type == kTypePort) {                                               // ADP_CS_4: the lock bit
      p.space[4] = v;
      if (!(v & kAdpCs4Lck)) p.locked = false;
      return;
    }
    if (p.type == kTypePort && i == 0x20 + 0x13 && (v & kPortCs19StartAsym)) {         // the transition starts and ends at once
      p.space[i] = v & ~kPortCs19StartAsym;
      return;
    }
    if (p.type == kTypeDpOut && i == 0x10 + kDpStatusCtrl) {                            // the connection manager handshake completes at once
      p.space[i] = dp_handshake_never_done ? v : (v & ~kDpStatusCtrlCmhs);
      return;
    }
    if (p.type == kTypeUsb3Down || p.type == kTypeUsb3Up) {
      if (i == 0x10 + kUsb3Cs2) {                                                      // the connection manager's request is acknowledged in HCA
        p.space[i] = v;
        if (no_usb3_ack) return;
        if (v & kUsb3Cs2Cmr) p.space[0x10 + kUsb3Cs1] |= kUsb3Cs1Hca;
        else p.space[0x10 + kUsb3Cs1] &= ~kUsb3Cs1Hca;
        return;
      }
    }
    if (i == 0x10 && p.type != kTypePort) {                                              // the adapter capability's first dword: the id and the link are read only, the enable bit is not
      p.space[i] = (p.space[i] & 0xFFFFu) | (v & 0xFFFF0000u);
      return;
    }
    p.space[i] = v;
  }

  fake_tb::FakeNhi& chip_;
  std::map<uint64_t, std::unique_ptr<Router>> routers_;
};

}  // namespace fake_usb4
