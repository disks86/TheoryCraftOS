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
  uint32_t regs[64] = {};
  std::vector<Port> ports;           // index 0 is the control adapter
  uint32_t cs5_writes = 0;
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
  Router& add(uint64_t route, uint32_t lane_pairs, uint32_t pcie_down, bool pcie_up, bool host = false, uint32_t usb3_down = 0, bool usb3_up = false) {
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
    r->max_port = n;
    r->regs[6] = (1u << 18);                                     // ROUTER_CS_6.HCI: it has an xHCI
    r->regs[7] = 0xA0A0A0A0u + static_cast<uint32_t>(route);
    r->regs[8] = 0x0B0B0B0Bu;
    Router& ref = *r;
    routers_[route] = std::move(r);
    return ref;
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
  bool refuse_sleep = false;                                     // ROUTER_CS_6.SLPR never comes up
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
    p.space[0x10] = 0x20 | (kPortCapPhy << 8) | (2u << 20);       // the lane adapter capability, next at 0x20; it supports two lanes
    p.space[0x20] = (kPortCapUsb4 << 8);
    p.space[0x20 + kPortCs18] = kPortCs18Be;                      // bonding enabled
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
      if (offset + length > 64) return fail(2);
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
      case 1: return 0x08 | (r.upstream << 8) | (r.max_port << 14) | (r.regs[1] & 0x00700000u) | (0x02u << 24);
      case 2: return r.regs[2];
      case 3: return r.regs[3];
      case 4: return r.regs[4] | (uint32_t{r.version} << 24);
      default: return r.regs[i];
    }
  }
  void write_switch(Router& r, uint32_t i, uint32_t v) {
    if (i == 1) r.regs[1] = (r.regs[1] & ~0x00700000u) | (v & 0x00700000u) , r.upstream = (v >> 8) & 0x3F;
    else if (i == 4) r.regs[4] = v & 0x00FFFFFFu;
    else r.regs[i] = v;
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
      return (p.space[0x11] & (kLaneCs1TargetWidthMask | kLaneCs1Lb)) | (p.state << 26) | ((dual ? 2u : 1u) << kLaneCs1CurrentWidthShift);
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
