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
#include "Network/NetworkStack.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"

namespace network {

// One network stack per network interface, run together. Each interface gets its
// own address (usually from its own DHCP server) and its own DNS resolver, since
// each is typically a different network with its own DNS server.
//
//   NetworkStackSet stacks(clock, allocator);
//   for each NIC: stacks.add(*nic);
//   stacks.start_dhcp();
//   stacks.wait_for_addresses(8000);       // all interfaces at once
//   stacks.resolve("example.com", 3000, &address);   // asks every network
//
// Every stack is thread-safe. poll() may be called from any thread, or
// start_service() starts a thread that polls all of them, so that application
// threads only ever wait (sleep) for their sockets.
class NetworkStackSet {
 public:
  NetworkStackSet(hardware::IClock& clock, allocators::IAllocator& allocator)
      : clock_(clock), allocator_(allocator), stacks_(allocator) {}

  // Builds a stack for `nic` (which must outlive the set). False if memory ran out.
  bool add(hardware::INetworkInterface& nic);

  uint32_t count() const { return static_cast<uint32_t>(stacks_.count()); }
  // The stack of interface `index` (in the order added), or null.
  NetworkStack* at(uint32_t index) const { return index < stacks_.count() ? stacks_[index].get() : nullptr; }

  ~NetworkStackSet() { stop_service(); }
  NetworkStackSet(const NetworkStackSet&) = delete;
  NetworkStackSet& operator=(const NetworkStackSet&) = delete;

  // Runs a thread that keeps every stack polled (about every 500 microseconds when
  // idle, immediately again when there was traffic). Call after the last add().
  bool start_service();
  void stop_service();
  bool service_running() const { return service_ != nullptr; }
  void clear() {  // drops every stack (before the NICs go away)
    stop_service();
    stacks_.clear();
  }
  void poll();
  // Starts DHCP on every interface.
  void start_dhcp();
  // Polls every stack until all have an address or `timeout_ms` passes; returns
  // how many interfaces have one. A silent network does not hold up the others.
  uint32_t wait_for_addresses(uint32_t timeout_ms);

  // Looks `name` up on every interface that has an address and a DNS server, all
  // at the same time, and returns the first answer found. *interface_index tells
  // which interface's network gave it. False if none resolved it in time.
  bool resolve(StringView name, uint32_t timeout_ms, Ipv4Address* address, uint32_t* interface_index = nullptr);

 private:
  hardware::IClock& clock_;
  allocators::IAllocator& allocator_;
  static void service_main(void* self);

  List<UniquePointer<NetworkStack>> stacks_;
  kernel::Thread* service_ = nullptr;
  volatile bool stop_ = false;
};

}  // namespace network
