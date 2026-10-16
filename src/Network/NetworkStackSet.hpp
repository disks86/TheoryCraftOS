#pragma once
#include "Network/NetworkStack.hpp"
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
// Like a single stack it is polled from one core; poll() runs every stack.
class NetworkStackSet {
 public:
  NetworkStackSet(hardware::IClock& clock, allocators::IAllocator& allocator)
      : clock_(clock), allocator_(allocator), stacks_(allocator) {}

  // Builds a stack for `nic` (which must outlive the set). False if memory ran out.
  bool add(hardware::INetworkInterface& nic);

  uint32_t count() const { return static_cast<uint32_t>(stacks_.count()); }
  // The stack of interface `index` (in the order added), or null.
  NetworkStack* at(uint32_t index) const { return index < stacks_.count() ? stacks_[index].get() : nullptr; }

  void clear() { stacks_.clear(); }  // drops every stack (before the NICs go away)
  void poll();
  // Starts DHCP on every interface.
  void start_dhcp();
  // Polls every stack until all have an address or `timeout_ms` passes; returns
  // how many interfaces have one. A silent network does not hold up the others.
  uint32_t wait_for_addresses(uint32_t timeout_ms);

  // Looks `name` up on every interface that has an address and a DNS server, all
  // at the same time, and returns the first answer found. *interface_index tells
  // which interface's network gave it. False if none resolved it in time.
  bool resolve(const char* name, uint32_t timeout_ms, Ipv4Address* address, uint32_t* interface_index = nullptr);

 private:
  hardware::IClock& clock_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<NetworkStack>> stacks_;
};

}  // namespace network
