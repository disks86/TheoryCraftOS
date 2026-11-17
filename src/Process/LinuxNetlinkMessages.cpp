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
#include "Process/LinuxNetlinkMessages.hpp"

#include "Utility/ByteStream.hpp"
#include "Utility/StringView.hpp"

// Netlink messages (RFC 3549, and Linux's include/uapi/linux/{netlink,rtnetlink,if_link,if_addr,genetlink}.h). Everything is in the machine's byte
// order, which is little endian for every CPU Linux programs run on here; addresses inside are in network order.
//
// struct nlmsghdr: nlmsg_len (4: header and payload), nlmsg_type (2), nlmsg_flags (2), nlmsg_seq (4), nlmsg_pid (4). The next message starts at a
// multiple of 4. An attribute (struct nlattr / rtattr) is its length (2: header and data), its type (2), the data, padded the same way.
namespace process::linuxabi::netlink {

namespace {

constexpr uint64_t kHead = 16;
constexpr uint16_t kError = 2, kDone = 3, kMinType = 0x10;
constexpr uint16_t kFlagRequest = 1, kFlagMulti = 2, kFlagAck = 4, kFlagRoot = 0x100, kFlagMatch = 0x200, kFlagCapped = 0x100;
constexpr uint16_t kNewLink = 16, kDelLink = 17, kGetLink = 18, kNewAddr = 20, kDelAddr = 21, kGetAddr = 22, kNewRoute = 24, kDelRoute = 25, kGetRoute = 26,
                   kRtmMax = 127;
constexpr int32_t kEperm = 1, kEnoent = 2, kEnodev = 19, kEinval = 22, kEopnotsupp = 95, kEnetunreach = 101;
constexpr uint8_t kInet = 2, kInet6 = 10;
constexpr uint8_t kScopeUniverse = 0, kScopeLink = 253, kScopeHost = 254;
// the groups of NETLINK_ROUTE a notice goes to (RTNLGRP_*; bit n-1 of a socket's nl_groups)
constexpr uint32_t kGroupLink = 1, kGroupAddr4 = 5, kGroupRoute4 = 7, kGroupAddr6 = 9, kGroupRoute6 = 11;
constexpr uint64_t kDatagramBytes = 3072;      // a dump's answers go out in datagrams of about this size (a program's buffer is a page or more)

// Messages, appended to a buffer as datagrams for a socket's queue.
class Builder {
 public:
  explicit Builder(ByteBuffer& out) : out_(out) {}
  bool ok() const { return ok_; }

  void begin_datagram(uint32_t group) {
    end_datagram();
    datagram_ = out_.size();
    open_ = true;
    ok_ = ok_ && out_.put32(0) && out_.put32(group) && out_.put32(0);
  }
  void end_datagram() {
    if (!open_) return;
    open_ = false;
    if (out_.size() == datagram_ + kQueuedHead) out_.truncate(datagram_);      // (nothing in it: not sent)
    else ok_ = ok_ && out_.patch32(datagram_, uint32_t(out_.size() - datagram_ - kQueuedHead));
  }
  uint64_t datagram_size() const { return open_ ? out_.size() - datagram_ - kQueuedHead : 0; }

  void begin(uint16_t type, uint16_t flags, uint32_t sequence, uint32_t port) {
    message_ = out_.size();
    ok_ = ok_ && out_.put32(0) && out_.put16(type) && out_.put16(flags) && out_.put32(sequence) && out_.put32(port);
  }
  void end() { ok_ = ok_ && out_.patch32(message_, uint32_t(out_.size() - message_)); }

  void u8(uint8_t v) { ok_ = ok_ && out_.put8(v); }
  void u16(uint16_t v) { ok_ = ok_ && out_.put16(v); }
  void u32(uint32_t v) { ok_ = ok_ && out_.put32(v); }
  void bytes(ConstByteView data) { ok_ = ok_ && out_.append(data); }
  void zeros(uint64_t count) { ok_ = ok_ && out_.put_zeros(count); }

  void attribute(uint16_t type, ConstByteView data) {
    if (data.size() > 0xFFFF - 4) { ok_ = false; return; }
    ok_ = ok_ && out_.put16(uint32_t(4 + data.size())) && out_.put16(type) && out_.append(data) && out_.put_zeros((4 - data.size() % 4) % 4);
  }
  void attribute_u8(uint16_t type, uint8_t v) { attribute(type, ConstByteView(&v, 1)); }
  void attribute_u32(uint16_t type, uint32_t v) {
    uint8_t raw[4];
    ByteWriter w{ByteView(raw)};
    w.le32(v);
    attribute(type, ConstByteView(raw));
  }
  void attribute_zeros(uint16_t type, uint32_t count) { ok_ = ok_ && out_.put16(4 + count) && out_.put16(type) && out_.put_zeros((count + 3) & ~uint32_t{3}); }
  // a string with its final zero
  void attribute_text(uint16_t type, StringView text) {
    const uint64_t size = text.size() + 1;
    ok_ = ok_ && out_.put16(uint32_t(4 + size)) && out_.put16(type) && out_.append(text.bytes()) && out_.put_zeros(1 + (4 - size % 4) % 4);
  }

 private:
  ByteBuffer& out_;
  uint64_t datagram_ = 0, message_ = 0;
  bool open_ = false, ok_ = true;
};

// One message of a request, as far as its header says.
struct Message {
  uint16_t type = 0, flags = 0;
  uint32_t sequence = 0;
  ConstByteView all, payload;
};

// The data of the first attribute of `type` among `attributes`; false if there is none (or the run is broken before it).
bool find_attribute(ConstByteView attributes, uint16_t type, ConstByteView* data) {
  ByteReader in{attributes};
  while (in.remaining() >= 4) {
    const uint16_t length = in.le16(), kind = in.le16() & 0x3FFF;      // (the top two bits are flags: nested, network byte order)
    if (length < 4 || uint64_t{length} - 4 > in.remaining()) return false;
    const ConstByteView value = in.take(uint64_t{length} - 4);
    if (kind == type) { *data = value; return true; }
    const uint64_t pad = (4 - length % 4) % 4;
    if (!in.skip(in.remaining() < pad ? in.remaining() : pad)) return false;
  }
  return false;
}

// NLMSG_ERROR: the error (0: an acknowledgement) and the request it is for, the header alone for an acknowledgement or when the socket asked so.
void put_error(Builder& b, const Asker& asker, const Message& m, int32_t error) {
  const bool whole = error != 0 && !asker.cap_ack;
  b.begin_datagram(0);
  b.begin(kError, whole || error == 0 ? 0 : kFlagCapped, m.sequence, asker.port);
  b.u32(uint32_t(-error));
  b.bytes(whole ? m.all : m.all.first(kHead));
  b.zeros(whole ? (4 - m.all.size() % 4) % 4 : 0);
  b.end();
  b.end_datagram();
}

// --- what there is to tell ---------------------------------------------------------------------------------------------------------------------------------

struct Address {
  const Interface* on = nullptr;
  uint8_t family = 0, prefix = 0, scope = 0;
  uint8_t bytes[16] = {};
  bool same(const Address& o) const {
    return on->index == o.on->index && family == o.family && prefix == o.prefix && ConstByteView(bytes).equals(ConstByteView(o.bytes));
  }
};
constexpr uint32_t kMostAddresses = kMostInterfaces * 3;

void copy(ByteView to, ConstByteView from) { to.first(from.size()).copy_from(from); }

// Every address of one family, in Linux's order (by interface; a global IPv6 address before the link-local one).
uint32_t addresses_of(const Snapshot& network, uint8_t family, Address out[kMostAddresses]) {
  uint32_t count = 0;
  auto add = [&](const Interface& on, ConstByteView bytes, uint8_t prefix, uint8_t scope) {
    Address& a = out[count++];
    a = Address();
    a.on = &on;
    a.family = family;
    a.prefix = prefix;
    a.scope = scope;
    copy(ByteView(a.bytes), bytes);
  };
  static constexpr uint8_t kLoopback4[4] = {127, 0, 0, 1}, kLoopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  for (uint32_t i = 0; i < network.count && i < kMostInterfaces; ++i) {
    const Interface& on = network.interfaces[i];
    if (family == kInet) {
      if (on.loopback) add(on, ConstByteView(kLoopback4), 8, kScopeHost);
      else if (on.has4) add(on, ConstByteView(on.address4), on.prefix4, kScopeUniverse);
    } else {
      if (on.loopback) { add(on, ConstByteView(kLoopback6), 128, kScopeHost); continue; }
      if (on.has_global6) add(on, ConstByteView(on.global6), on.prefix6, kScopeUniverse);
      if (on.has_link6) add(on, ConstByteView(on.link6), 64, kScopeLink);
    }
  }
  return count;
}

struct Route {
  const Interface* out = nullptr;
  uint8_t family = 0, length = 0, scope = 0, protocol = 0, type = 1;
  uint8_t to[16] = {}, gateway[16] = {}, source[16] = {};
  bool has_gateway = false, has_source = false;
  uint32_t metric = 0;
  bool same(const Route& o) const {
    return out->index == o.out->index && family == o.family && length == o.length && has_gateway == o.has_gateway && ConstByteView(to).equals(ConstByteView(o.to)) &&
           ConstByteView(gateway).equals(ConstByteView(o.gateway)) && ConstByteView(source).equals(ConstByteView(o.source));
  }
};
constexpr uint32_t kMostRoutes = kMostInterfaces * 3;
constexpr uint8_t kProtocolKernel = 2, kProtocolRa = 9, kProtocolDhcp = 16, kTableMain = 254, kTypeUnicast = 1, kTypeLocal = 2;

bool all_zero(ConstByteView bytes) {
  for (const uint8_t b : bytes)
    if (b != 0) return false;
  return true;
}

// `bytes` with everything after the first `prefix` bits cleared.
void mask_to(ByteView bytes, uint32_t prefix) {
  uint32_t bit = 0;
  for (uint8_t& b : bytes) {
    if (bit + 8 <= prefix) {}
    else if (bit >= prefix) b = 0;
    else b = uint8_t(b & (0xFF00u >> (prefix - bit)));
    bit += 8;
  }
}

// The main routing table of one family: what a network interface's configuration amounts to (the default route first, as `ip route` lists it).
uint32_t routes_of(const Snapshot& network, uint8_t family, Route out[kMostRoutes]) {
  uint32_t count = 0;
  auto add = [&](const Interface& on, ConstByteView to, uint8_t length, uint8_t scope, uint8_t protocol, uint32_t metric) -> Route& {
    Route& r = out[count++];
    r = Route();
    r.out = &on;
    r.family = family;
    r.length = length;
    r.scope = scope;
    r.protocol = protocol;
    r.metric = metric;
    copy(ByteView(r.to), to);
    mask_to(ByteView(r.to), length);
    return r;
  };
  static constexpr uint8_t kNone[16] = {}, kLinkLocal[16] = {0xFE, 0x80}, kLoopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  for (uint32_t i = 0; i < network.count && i < kMostInterfaces; ++i) {
    const Interface& on = network.interfaces[i];
    if (family == kInet) {
      if (on.loopback || !on.has4) continue;
      if (!all_zero(ConstByteView(on.gateway4))) {
        Route& r = add(on, ConstByteView(kNone).first(4), 0, kScopeUniverse, kProtocolDhcp, 100);
        r.has_gateway = true;
        copy(ByteView(r.gateway), ConstByteView(on.gateway4));
        r.has_source = true;
        copy(ByteView(r.source), ConstByteView(on.address4));
      }
      Route& r = add(on, ConstByteView(on.address4), on.prefix4, kScopeLink, kProtocolKernel, 100);
      r.has_source = true;
      copy(ByteView(r.source), ConstByteView(on.address4));
    } else {
      if (on.loopback) { add(on, ConstByteView(kLoopback6), 128, kScopeUniverse, kProtocolKernel, 256); continue; }
      if (on.has_global6) add(on, ConstByteView(on.global6), on.prefix6, kScopeUniverse, kProtocolRa, 100);
      if (on.has_link6) add(on, ConstByteView(kLinkLocal), 64, kScopeUniverse, kProtocolKernel, 256);
      if (on.has_router6) {
        Route& r = add(on, ConstByteView(kNone), 0, kScopeUniverse, kProtocolRa, 100);
        r.has_gateway = true;
        copy(ByteView(r.gateway), ConstByteView(on.router6));
      }
    }
  }
  return count;
}

// --- the messages -------------------------------------------------------------------------------------------------------------------------------------------

// RTM_NEWLINK / RTM_DELLINK. struct ifinfomsg: ifi_family (1, and 1 of padding), ifi_type (2), ifi_index (4), ifi_flags (4), ifi_change (4).
void put_link(Builder& b, const Interface& on, uint16_t type, uint16_t flags, uint32_t sequence, uint32_t port) {
  constexpr uint32_t kUp = 1, kBroadcast = 2, kLoopback = 8, kRunning = 0x40, kMulticast = 0x1000, kLowerUp = 0x10000;
  static constexpr uint8_t kEveryone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, kNobody[6] = {};
  b.begin(type, flags, sequence, port);
  b.u8(0); b.u8(0);
  b.u16(on.loopback ? 772 : 1);      // ARPHRD_LOOPBACK, ARPHRD_ETHER
  b.u32(on.index);
  constexpr uint32_t kPromiscuous = 0x100, kAllMulticast = 0x200;
  b.u32((on.loopback ? (kUp | kLoopback | kRunning | kLowerUp) : (kUp | kBroadcast | kRunning | kMulticast | kLowerUp)) | (on.promiscuous ? kPromiscuous : 0) |
        (on.all_multicast ? kAllMulticast : 0));
  b.u32(0);
  b.attribute_text(3, StringView(on.name, on.name_size));      // IFLA_IFNAME
  b.attribute_u32(13, on.loopback ? 1000 : 1000);               // IFLA_TXQLEN
  b.attribute_u8(16, on.loopback ? 0 : 6);                      // IFLA_OPERSTATE: unknown (as Linux says of its loopback), up
  b.attribute_u8(17, 0);                                        // IFLA_LINKMODE
  b.attribute_u32(4, on.mtu);                                   // IFLA_MTU
  b.attribute_u32(27, 0);                                       // IFLA_GROUP
  b.attribute_u8(33, 1);                                        // IFLA_CARRIER
  b.attribute_text(6, on.loopback ? StringView("noqueue") : StringView("fq_codel"));      // IFLA_QDISC
  b.attribute(1, on.loopback ? ConstByteView(kNobody) : ConstByteView(on.mac));           // IFLA_ADDRESS
  b.attribute(2, on.loopback ? ConstByteView(kNobody) : ConstByteView(kEveryone));        // IFLA_BROADCAST
  b.attribute_zeros(23, 200);                                   // IFLA_STATS64: struct rtnl_link_stats64, nothing counted
  b.attribute_zeros(7, 96);                                     // IFLA_STATS: struct rtnl_link_stats
  b.end();
}

// RTM_NEWADDR / RTM_DELADDR. struct ifaddrmsg: ifa_family, ifa_prefixlen, ifa_flags, ifa_scope (1 each), ifa_index (4).
void put_address(Builder& b, const Address& a, uint16_t type, uint16_t flags, uint32_t sequence, uint32_t port) {
  constexpr uint8_t kPermanent = 0x80;
  const uint64_t size = a.family == kInet ? 4 : 16;
  const ConstByteView bytes = ConstByteView(a.bytes).first(size);
  const bool fixed = a.on->loopback || a.scope == kScopeLink;      // (an address the network handed out is not permanent)
  b.begin(type, flags, sequence, port);
  b.u8(a.family); b.u8(a.prefix); b.u8(fixed ? kPermanent : 0); b.u8(a.scope);
  b.u32(a.on->index);
  b.attribute(1, bytes);                                        // IFA_ADDRESS
  if (a.family == kInet) {
    b.attribute(2, bytes);                                      // IFA_LOCAL
    if (!a.on->loopback) {                                      // IFA_BROADCAST: the address with the host bits set
      uint8_t all[4];
      copy(ByteView(all), bytes);
      for (uint32_t bit = a.prefix; bit < 32; ++bit) all[bit / 8] = uint8_t(all[bit / 8] | (0x80u >> (bit % 8)));
      b.attribute(4, ConstByteView(all));
    }
    b.attribute_text(3, StringView(a.on->name, a.on->name_size));      // IFA_LABEL
  }
  b.attribute_u32(8, fixed ? kPermanent : 0);                   // IFA_FLAGS
  // IFA_CACHEINFO: preferred and valid lifetimes (for ever), when it was made and changed (hundredths of a second since boot)
  b.u16(20); b.u16(6);
  b.u32(0xFFFFFFFFu); b.u32(0xFFFFFFFFu); b.u32(0); b.u32(0);
  b.end();
}

// RTM_NEWROUTE / RTM_DELROUTE. struct rtmsg: rtm_family, rtm_dst_len, rtm_src_len, rtm_tos, rtm_table, rtm_protocol, rtm_scope, rtm_type (1 each),
// rtm_flags (4).
void put_route(Builder& b, const Route& r, uint16_t type, uint16_t flags, uint32_t sequence, uint32_t port, uint32_t route_flags = 0) {
  const uint64_t size = r.family == kInet ? 4 : 16;
  b.begin(type, flags, sequence, port);
  b.u8(r.family); b.u8(r.length); b.u8(0); b.u8(0);
  b.u8(kTableMain); b.u8(r.protocol); b.u8(r.scope); b.u8(r.type);
  b.u32(route_flags);
  b.attribute_u32(15, kTableMain);                                              // RTA_TABLE
  if (r.length != 0) b.attribute(1, ConstByteView(r.to).first(size));           // RTA_DST
  const bool one = route_flags != 0;      // (the answer for one destination lists the interface first, as Linux does)
  if (one) b.attribute_u32(4, r.out->index);                                    // RTA_OIF
  if (r.metric != 0) b.attribute_u32(6, r.metric);                              // RTA_PRIORITY
  if (r.has_source) b.attribute(7, ConstByteView(r.source).first(size));        // RTA_PREFSRC
  if (r.has_gateway) b.attribute(5, ConstByteView(r.gateway).first(size));      // RTA_GATEWAY
  if (!one) b.attribute_u32(4, r.out->index);                                   // RTA_OIF
  b.end();
}

// A dump: each of `count` things as a message with NLM_F_MULTI, in as many datagrams as it takes, then NLMSG_DONE in one of its own.
class Dump {
 public:
  Dump(Builder& b, const Asker& asker, const Message& m) : b_(b), asker_(asker), m_(m) { b_.begin_datagram(0); }
  // before each message
  void next() {
    if (b_.datagram_size() >= kDatagramBytes) b_.begin_datagram(0);
  }
  void done() {
    b_.begin_datagram(0);
    b_.begin(kDone, kFlagMulti, m_.sequence, asker_.port);
    b_.u32(0);
    b_.end();
    b_.end_datagram();
  }

 private:
  Builder& b_;
  const Asker& asker_;
  const Message& m_;
};

bool wanted(uint8_t asked, uint8_t family) { return asked == 0 || asked == family; }

// RTM_GETROUTE for one destination: the route a packet to it would take. 0, or the error.
int32_t route_to(Builder& b, const Asker& asker, const Message& m, const Snapshot& network) {
  if (m.payload.size() < 12) return kEinval;
  const uint8_t family = m.payload[0];
  if (family != kInet && family != kInet6) return kEopnotsupp;
  const uint64_t size = family == kInet ? 4 : 16;
  uint8_t to[16] = {};
  ConstByteView given;
  if (find_attribute(m.payload.subview(12), 1, &given)) {
    if (given.size() != size) return kEinval;
    copy(ByteView(to), given);
  }
  const ConstByteView target = ConstByteView(to).first(size);
  Route answer;
  bool found = false;
  const bool loopback = family == kInet ? to[0] == 127 : (all_zero(target.first(15)) && to[15] == 1);
  Address addresses[kMostAddresses];
  const uint32_t address_count = addresses_of(network, family, addresses);
  for (uint32_t i = 0; i < address_count && !found; ++i) {      // to the loopback, or to one of the machine's own addresses: delivered here
    const Address& a = addresses[i];
    if (!(a.on->loopback && loopback) && !ConstByteView(a.bytes).first(size).equals(target)) continue;
    answer.out = &network.interfaces[0];
    for (uint32_t k = 0; k < network.count && k < kMostInterfaces; ++k)
      if (network.interfaces[k].loopback) answer.out = &network.interfaces[k];
    answer.type = kTypeLocal;
    answer.scope = kScopeHost;
    answer.has_source = true;
    copy(ByteView(answer.source), target);
    found = true;
  }
  if (!found) {      // the longest prefix of the table that the destination is in
    Route routes[kMostRoutes];
    const uint32_t count = routes_of(network, family, routes);
    int best = -1;
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t masked[16];
      copy(ByteView(masked), ConstByteView(to));
      mask_to(ByteView(masked).first(size), routes[i].length);
      if (!ConstByteView(masked).first(size).equals(ConstByteView(routes[i].to).first(size))) continue;
      if (best < 0 || routes[i].length > routes[uint32_t(best)].length) best = int(i);
    }
    if (best < 0) return kEnetunreach;
    answer = routes[uint32_t(best)];
    if (!answer.has_source) {      // an address of the interface it leaves by (one of the destination's scope for IPv6)
      const bool link = family == kInet6 && to[0] == 0xFE && (to[1] & 0xC0) == 0x80;
      for (uint32_t i = 0; i < address_count; ++i) {
        const Address& a = addresses[i];
        if (a.on->index != answer.out->index || (family == kInet6 && link != (a.scope == kScopeLink))) continue;
        answer.has_source = true;
        copy(ByteView(answer.source), ConstByteView(a.bytes));
        break;
      }
    }
    answer.scope = kScopeUniverse;
  }
  answer.family = family;
  answer.length = uint8_t(size * 8);
  answer.metric = 0;
  answer.protocol = 0;
  copy(ByteView(answer.to), ConstByteView(to));
  b.begin_datagram(0);
  put_route(b, answer, kNewRoute, 0, m.sequence, asker.port, 0x200 /* RTM_F_CLONED */);
  b.end_datagram();
  return 0;
}

// One request of NETLINK_ROUTE. 0 if it was answered (or needs no answer), or the error to report.
int32_t route_request(Builder& b, const Asker& asker, const Message& m, const Snapshot& network, bool* dumped) {
  if (m.type > kRtmMax) return kEopnotsupp;
  if (m.payload.size() < 1) return 0;                    // (not even a family: nothing, as on Linux)
  const uint32_t kind = (m.type - kMinType) & 3;        // 0 new, 1 delete, 2 get, 3 set
  if (kind != 2) return kEperm;                         // (the network is not a Linux program's to configure here)
  const uint8_t family = m.payload[0];
  const bool dump = (m.flags & (kFlagRoot | kFlagMatch)) != 0;
  if (dump) {
    *dumped = true;
    Dump d(b, asker, m);
    if (m.type == kGetLink) {
      for (uint32_t i = 0; i < network.count && i < kMostInterfaces; ++i) {
        d.next();
        put_link(b, network.interfaces[i], kNewLink, kFlagMulti, m.sequence, asker.port);
      }
    } else if (m.type == kGetAddr || m.type == kGetRoute) {
      static constexpr uint8_t kFamilies[2] = {kInet, kInet6};
      for (const uint8_t each : kFamilies) {
        if (!wanted(family, each)) continue;
        if (m.type == kGetAddr) {
          Address addresses[kMostAddresses];
          const uint32_t count = addresses_of(network, each, addresses);
          for (uint32_t i = 0; i < count; ++i) {
            d.next();
            put_address(b, addresses[i], kNewAddr, kFlagMulti, m.sequence, asker.port);
          }
        } else {
          Route routes[kMostRoutes];
          const uint32_t count = routes_of(network, each, routes);
          for (uint32_t i = 0; i < count; ++i) {
            d.next();
            put_route(b, routes[i], kNewRoute, kFlagMulti, m.sequence, asker.port);
          }
        }
      }
    }
    // (neighbours, rules, queueing disciplines and whatever else can be listed: there are none)
    d.done();
    return 0;
  }
  if (m.type == kGetLink) {      // one interface, by its number or by its name (IFLA_IFNAME)
    if (m.payload.size() < 16) return kEinval;
    ByteReader in{m.payload.subview(4, 4)};
    const uint32_t index = in.le32();
    ConstByteView name;
    const bool named = find_attribute(m.payload.subview(16), 3, &name);
    if (index == 0 && !named) return kEinval;
    for (uint32_t i = 0; i < network.count && i < kMostInterfaces; ++i) {
      const Interface& on = network.interfaces[i];
      if (index != 0 ? on.index != index : !(name.size() == on.name_size + 1 && name.first(on.name_size).equals(StringView(on.name, on.name_size).bytes()))) continue;
      b.begin_datagram(0);
      put_link(b, on, kNewLink, 0, m.sequence, asker.port);
      b.end_datagram();
      return 0;
    }
    return kEnodev;
  }
  if (m.type == kGetRoute) return route_to(b, asker, m, network);
  return kEopnotsupp;
}

// One request of NETLINK_GENERIC. The only family there is is the controller itself ("nlctrl", number 16), which programs ask for the number of the
// family they want: CTRL_CMD_GETFAMILY (3) with CTRL_ATTR_FAMILY_NAME (2) or CTRL_ATTR_FAMILY_ID (1). struct genlmsghdr: cmd, version, reserved (2).
int32_t generic_request(Builder& b, const Asker& asker, const Message& m, bool* dumped) {
  constexpr uint16_t kController = 0x10;
  if (m.type != kController) return kEnoent;
  if (m.payload.size() < 4) return kEinval;
  if (m.payload[0] != 3) return kEopnotsupp;
  const bool dump = (m.flags & (kFlagRoot | kFlagMatch)) != 0;
  auto family = [&](uint16_t flags) {
    b.begin(kController, flags, m.sequence, asker.port);
    b.u8(1); b.u8(2); b.u16(0);                 // CTRL_CMD_NEWFAMILY, version 2
    b.attribute_text(2, StringView("nlctrl"));      // CTRL_ATTR_FAMILY_NAME
    b.u16(6); b.u16(1); b.u16(kController); b.u16(0);      // CTRL_ATTR_FAMILY_ID (a 16-bit number)
    b.attribute_u32(3, 2);                      // CTRL_ATTR_VERSION
    b.attribute_u32(4, 0);                      // CTRL_ATTR_HDRSIZE
    b.attribute_u32(5, 10);                     // CTRL_ATTR_MAXATTR
    b.end();
  };
  if (dump) {
    *dumped = true;
    Dump d(b, asker, m);
    d.next();
    family(kFlagMulti);
    d.done();
    return 0;
  }
  ConstByteView value;
  bool ours = false;
  if (find_attribute(m.payload.subview(4), 2, &value)) ours = value.size() == 7 && value[6] == 0 && value.first(6).equals(StringView("nlctrl").bytes());
  else if (find_attribute(m.payload.subview(4), 1, &value)) ours = value.size() == 2 && value[0] == kController && value[1] == 0;
  else return kEinval;
  if (!ours) return kEnoent;
  b.begin_datagram(0);
  family(0);
  b.end_datagram();
  return 0;
}

}  // namespace

bool answer(const Asker& asker, ConstByteView request, const Snapshot& network, ByteBuffer* out) {
  Builder b(*out);
  ByteReader in{request};
  while (in.remaining() >= kHead) {
    const uint64_t start = in.position();
    const uint32_t length = in.le32();
    Message m;
    m.type = in.le16();
    m.flags = in.le16();
    m.sequence = in.le32();
    in.skip(4);      // (nlmsg_pid: whoever it says it is from, the answer goes to the socket it came through)
    if (length < kHead || length > request.size() - start) break;      // (not a whole message: the rest of the datagram is not looked at)
    m.all = request.subview(start, length);
    m.payload = m.all.subview(kHead);
    const uint64_t padded = (uint64_t{length} + 3) & ~uint64_t{3};
    if (!in.seek(padded > request.size() - start ? request.size() : start + padded)) break;
    if (!(m.flags & kFlagRequest)) continue;
    int32_t error = 0;
    bool dumped = false;
    if (m.type >= kMinType) {
      if (asker.protocol == kProtocolRoute) error = route_request(b, asker, m, network, &dumped);
      else if (asker.protocol == kProtocolGeneric) error = generic_request(b, asker, m, &dumped);
      else error = kEopnotsupp;
    }
    // an error always comes back; success only when asked to acknowledge (and a dump's end is its own acknowledgement)
    if (error != 0 || ((m.flags & kFlagAck) && !dumped)) put_error(b, asker, m, error);
  }
  b.end_datagram();
  return b.ok();
}

bool changes(const Snapshot& before, const Snapshot& now, uint32_t groups, ByteBuffer* out) {
  Builder b(*out);
  auto subscribed = [&](uint32_t group) { return (groups & (uint32_t{1} << (group - 1))) != 0; };
  if (subscribed(kGroupLink)) {
    for (int pass = 0; pass < 2; ++pass) {      // the ones that went, then the ones that came
      const Snapshot& from = pass == 0 ? before : now;
      const Snapshot& other = pass == 0 ? now : before;
      for (uint32_t i = 0; i < from.count && i < kMostInterfaces; ++i) {
        bool there = false;
        for (uint32_t k = 0; k < other.count && k < kMostInterfaces; ++k) there = there || other.interfaces[k].index == from.interfaces[i].index;
        if (there) continue;
        b.begin_datagram(kGroupLink);
        put_link(b, from.interfaces[i], pass == 0 ? kDelLink : kNewLink, 0, 0, 0);
        b.end_datagram();
      }
    }
  }
  static constexpr uint8_t kFamilies[2] = {kInet, kInet6};
  for (const uint8_t family : kFamilies) {
    const uint32_t address_group = family == kInet ? kGroupAddr4 : kGroupAddr6, route_group = family == kInet ? kGroupRoute4 : kGroupRoute6;
    if (subscribed(address_group)) {
      Address was[kMostAddresses], is[kMostAddresses];
      const uint32_t was_count = addresses_of(before, family, was), is_count = addresses_of(now, family, is);
      for (int pass = 0; pass < 2; ++pass) {
        const Address* from = pass == 0 ? was : is;
        const Address* other = pass == 0 ? is : was;
        const uint32_t from_count = pass == 0 ? was_count : is_count, other_count = pass == 0 ? is_count : was_count;
        for (uint32_t i = 0; i < from_count; ++i) {
          bool there = false;
          for (uint32_t k = 0; k < other_count; ++k) there = there || other[k].same(from[i]);
          if (there) continue;
          b.begin_datagram(address_group);
          put_address(b, from[i], pass == 0 ? kDelAddr : kNewAddr, 0, 0, 0);
          b.end_datagram();
        }
      }
    }
    if (subscribed(route_group)) {
      Route was[kMostRoutes], is[kMostRoutes];
      const uint32_t was_count = routes_of(before, family, was), is_count = routes_of(now, family, is);
      for (int pass = 0; pass < 2; ++pass) {
        const Route* from = pass == 0 ? was : is;
        const Route* other = pass == 0 ? is : was;
        const uint32_t from_count = pass == 0 ? was_count : is_count, other_count = pass == 0 ? is_count : was_count;
        for (uint32_t i = 0; i < from_count; ++i) {
          bool there = false;
          for (uint32_t k = 0; k < other_count; ++k) there = there || other[k].same(from[i]);
          if (there) continue;
          b.begin_datagram(route_group);
          put_route(b, from[i], pass == 0 ? kDelRoute : kNewRoute, 0, 0, 0);
          b.end_datagram();
        }
      }
    }
  }
  b.end_datagram();
  return b.ok();
}

}  // namespace process::linuxabi::netlink
