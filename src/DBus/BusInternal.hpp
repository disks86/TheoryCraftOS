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

// What Bus.cpp and BusDriver.cpp share: a connection, a name's queue of owners, and the driver (the methods of org.freedesktop.DBus).
#pragma once
#include "DBus/Bus.hpp"

namespace dbus {

struct Bus::Connection {
  explicit Connection(allocators::IAllocator& allocator)
      : unique(allocator), inbox(allocator), inbox_fds(allocator), out(allocator), out_fds(allocator), matches(allocator) {}
  uint32_t id = 0;
  String unique;
  uint64_t pid = 0;                    // 0: not known
  uint32_t uid = ~0u;                  // ~0: not known
  LocalPeer* local = nullptr;          // a connection inside the kernel
  UniquePointer<AuthServer> auth;      // (none for a local one)
  bool hello = false, monitor = false, closing = false;
  ByteBuffer inbox;
  Queue<FdRef> inbox_fds;
  ByteBuffer out;
  List<OutFd> out_fds;
  List<UniquePointer<MatchRule>> matches;      // a monitor's: what it watches (none: everything)
  uint32_t names = 0;                  // names it owns or waits for
  uint32_t pending_calls = 0;          // its calls waiting for replies
  uint32_t next_serial = 1;            // (a local connection's messages)
};

struct Bus::NameEntry {
  explicit NameEntry(allocators::IAllocator& allocator) : name(allocator), queue(allocator) {}
  String name;
  struct Owner {
    uint32_t client;
    uint32_t flags;
  };
  List<Owner> queue;      // [0] the primary owner
};

class BusDriver {
 public:
  static void hello(Bus& bus, Bus::Connection& from, const Message& call);
  static void handle(Bus& bus, Bus::Connection& from, const Message& call);
};

}  // namespace dbus
