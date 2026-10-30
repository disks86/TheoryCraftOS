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

#include "Network/NetworkStackSet.hpp"
#include "Kernel/Thread.hpp"

namespace network {

namespace {
NetworkStackSet* g_system_stacks = nullptr;
}  // namespace

NetworkStackSet* system_network_stacks() { return g_system_stacks; }
void set_system_network_stacks(NetworkStackSet* stacks) { g_system_stacks = stacks; }

bool NetworkStackSet::add(hardware::INetworkInterface& nic) {
  UniquePointer<NetworkStack> stack = UniquePointer<NetworkStack>::make(allocator_, nic, clock_, allocator_);
  if (!stack || !stack->initialize()) return false;
  if (nic.wireless() || nic.link_comes_and_goes()) stack->set_dhcp_follows_link(true);       // the link of a Wi-Fi adapter is the connection: addresses come and go with it
  return stacks_.push_back(util::move(stack));
}

bool NetworkStackSet::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "network");
  return service_ != nullptr;
}

void NetworkStackSet::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void NetworkStackSet::service_main(void* self) {
  NetworkStackSet* set = static_cast<NetworkStackSet*>(self);
  while (!__atomic_load_n(&set->stop_, __ATOMIC_ACQUIRE)) {
    set->poll();
    kernel::sleep_us(500);
  }
}

void NetworkStackSet::poll() {
  for (UniquePointer<NetworkStack>& stack : stacks_) stack->poll();
}

void NetworkStackSet::start_dhcp() {
  for (UniquePointer<NetworkStack>& stack : stacks_) stack->start_dhcp();
}

uint32_t NetworkStackSet::wait_for_addresses(uint32_t timeout_ms) {
  const uint64_t deadline = clock_.now_ms() + timeout_ms;
  for (;;) {
    poll();
    uint32_t have = 0, giving_up = 0;
    for (UniquePointer<NetworkStack>& stack : stacks_) {
      if (stack->configuration().configured()) ++have;
      else if (stack->dhcp().state() == DhcpState::Failed) ++giving_up;  // will not get one: stop waiting for it
    }
    if (have + giving_up == stacks_.count() || clock_.now_ms() >= deadline) return have;
    kernel::sleep_us(200);
  }
}

bool NetworkStackSet::resolve(StringView name, uint32_t timeout_ms, Ipv4Address* address, uint32_t* interface_index) {
  // One query per usable interface, all in flight together.
  List<SharedPointer<IDnsQuery>> queries(allocator_);
  List<uint32_t> owners(allocator_);
  for (uint32_t i = 0; i < stacks_.count(); ++i) {
    NetworkStack& stack = *stacks_[i];
    if (!stack.configuration().configured() || stack.configuration().dns.is_unspecified()) continue;
    SharedPointer<IDnsQuery> query = stack.dns().query(name);
    if (!query) return false;  // not a valid name (the same for every interface) or out of memory
    if (!queries.push_back(query) || !owners.push_back(i)) return false;
  }

  const uint64_t deadline = clock_.now_ms() + timeout_ms;
  for (;;) {
    bool pending = false;
    for (uint64_t q = 0; q < queries.count(); ++q) {
      const DnsStatus status = queries[q]->status();
      if (status == DnsStatus::Resolved) {
        if (address) *address = queries[q]->address();
        if (interface_index) *interface_index = owners[q];
        return true;
      }
      pending |= status == DnsStatus::Pending;
    }
    if (!pending || clock_.now_ms() >= deadline) return false;
    poll();
    kernel::sleep_us(200);
  }
}

}  // namespace network
