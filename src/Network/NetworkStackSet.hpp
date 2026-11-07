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

// What a Windows socket (src/Process/Sockets.hpp) needs of a set of network stacks: how many there are, and each one's interface - not NetworkStackSet itself, so that
// something with its own stacks (a host test's, say, joined by a fake wire instead of real interfaces) can stand in for it without building one.
class IStackProvider {
 public:
  virtual uint32_t count() const = 0;
  virtual INetworkStack* at(uint32_t index) const = 0;
  // A number that goes up whenever some stack did something its sockets may be waiting for (frames received, delivered over the loopback,
  // or sent from a transmit queue). Read it before trying an operation; if the operation would block, wait_for_progress() with it.
  virtual uint64_t progress() const { return 0; }
  // Waits until progress() is no longer `seen`, or `timeout_us` passes. False if this provider cannot wait that way (the caller sleeps
  // instead, as it would have without it).
  virtual bool wait_for_progress(uint64_t seen, uint64_t timeout_us) {
    (void)seen;
    (void)timeout_us;
    return false;
  }

 protected:
  IStackProvider() = default;
  ~IStackProvider() = default;
};

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
// start_service() starts a thread per stack that polls it, so that application
// threads only ever wait for their sockets (wait_for_progress() wakes them as soon
// as a poll did something). Each stack's thread runs on its own: interfaces are
// serviced concurrently, on different cores, and a busy one does not delay another.
class NetworkStackSet final : public IStackProvider {
 public:
  NetworkStackSet(hardware::IClock& clock, allocators::IAllocator& allocator)
      : clock_(clock), allocator_(allocator), stacks_(allocator), workers_(allocator) {}

  // Builds a stack for `nic` (which must outlive the set). False if memory ran out.
  bool add(hardware::INetworkInterface& nic);

  uint32_t count() const override { return static_cast<uint32_t>(stacks_.count()); }
  // The stack of interface `index` (in the order added), or null.
  NetworkStack* at(uint32_t index) const override { return index < stacks_.count() ? stacks_[index].get() : nullptr; }

  ~NetworkStackSet() { clear(); }
  NetworkStackSet(const NetworkStackSet&) = delete;
  NetworkStackSet& operator=(const NetworkStackSet&) = delete;

  // Runs a thread per stack that keeps it polled: woken by its interface's interrupts
  // where it has them (and every 5 ms for timers), else about every 500 microseconds;
  // at once by the stack itself when it has work (a packet over the loopback, frames
  // waiting for room in the interface); immediately again while frames keep coming.
  // Call after the last add().
  bool start_service();
  void stop_service();
  bool service_running() const { return service_; }
  void clear();  // drops every stack (before the NICs go away); their interfaces stop interrupting
  // True when every interface wakes the service thread by interrupt.
  bool interrupt_driven() const;
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

  uint64_t progress() const override { return __atomic_load_n(&progress_, __ATOMIC_ACQUIRE); }
  bool wait_for_progress(uint64_t seen, uint64_t timeout_us) override;

 private:
  // One stack's service thread, and what wakes it.
  struct Worker {
    NetworkStackSet* set = nullptr;
    NetworkStack* stack = nullptr;
    kernel::Event activity{true};
    kernel::Thread* thread = nullptr;
  };
  hardware::IClock& clock_;
  allocators::IAllocator& allocator_;
  static void service_main(void* worker);
  static void on_activity(void* worker);  // from the interface's interrupt handler, and the stack's wake hook
  static void on_progress(void* worker);
  static constexpr uint64_t kPolledIdleUs = 500, kInterruptIdleUs = 5000;

  List<UniquePointer<NetworkStack>> stacks_;
  List<UniquePointer<Worker>> workers_;   // (one per stack, in the same order)
  bool service_ = false;
  volatile bool stop_ = false;
  uint64_t progress_ = 0;
  kernel::Mutex progress_mutex_;          // (with progress_cv_: wait_for_progress)
  kernel::ConditionVariable progress_cv_;
};

// The machine's one set of network stacks, for user-mode programs to reach (Windows sockets: see src/Process/Sockets.hpp). Set once, from main(), after the
// interfaces are added and start_service() is called; null before then or on a machine with no usable network interface.
NetworkStackSet* system_network_stacks();
void set_system_network_stacks(NetworkStackSet* stacks);

}  // namespace network
