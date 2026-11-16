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
#pragma once

#include <stdint.h>

#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

// What the kernel's side of an AF_NETLINK socket says: the answers to a datagram of requests, and the notices of what changed in the network. Nothing
// here knows about processes or descriptors (LinuxNetlink.cpp does): requests are bytes a program wrote, the network is a Snapshot, and what comes out
// is a run of datagrams for the socket's queue.
namespace process::linuxabi::netlink {

constexpr uint32_t kProtocolRoute = 0, kProtocolUevent = 15, kProtocolGeneric = 16;

// One network interface as Linux programs see it. Addresses are in network byte order. Interface 1 is the loopback.
struct Interface {
  uint32_t index = 0;
  char name[16] = {};
  uint32_t name_size = 0;
  bool loopback = false;
  uint8_t mac[6] = {};
  uint32_t mtu = 1500;
  bool has4 = false;
  uint8_t address4[4] = {}, gateway4[4] = {};      // (a gateway of zeros: none)
  uint8_t prefix4 = 0;
  bool has_link6 = false, has_global6 = false, has_router6 = false;
  uint8_t link6[16] = {}, global6[16] = {}, router6[16] = {};
  uint8_t prefix6 = 64;
};

constexpr uint32_t kMostInterfaces = 9;
struct Snapshot {
  Interface interfaces[kMostInterfaces];
  uint32_t count = 0;
};

// The socket a request came through: its protocol, the port its answers are addressed to, and whether errors carry only the request's header
// (NETLINK_CAP_ACK).
struct Asker {
  uint32_t protocol = kProtocolRoute;
  uint32_t port = 0;
  bool cap_ack = false;
};

// A datagram in a socket's queue: its length, the multicast group it was sent to (0: to the socket alone) and its sender's port (0: the kernel), four
// bytes each, then the bytes.
constexpr uint64_t kQueuedHead = 12;

// Answers every request in `request` (one datagram a program sent to the kernel), appending datagrams to `out`. False only without memory; a request
// that is cut short or makes no sense gets what Linux gives it (an error message, or nothing).
[[nodiscard]] bool answer(const Asker& asker, ConstByteView request, const Snapshot& network, ByteBuffer* out);

// What changed between two snapshots, as the notices Linux sends to the groups in `groups` (the RTMGRP_* bits a socket bound): addresses and routes
// that came and went, interfaces that appeared and disappeared. Appends datagrams to `out`. False only without memory.
[[nodiscard]] bool changes(const Snapshot& before, const Snapshot& now, uint32_t groups, ByteBuffer* out);

}  // namespace process::linuxabi::netlink
