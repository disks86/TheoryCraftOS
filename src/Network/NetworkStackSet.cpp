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
  UniquePointer<Worker> worker = UniquePointer<Worker>::make(allocator_);
  if (!stack || !worker || !stack->initialize()) return false;
  if (nic.wireless() || nic.link_comes_and_goes()) stack->set_dhcp_follows_link(true);       // the link of a Wi-Fi adapter is the connection: addresses come and go with it
  if (nic.direct_link()) stack->set_link_local_fallback(true);
  worker->set = this;
  worker->stack = stack.get();
  NetworkStack::ServiceHooks hooks;
  hooks.wake = &NetworkStackSet::on_activity;
  hooks.progress = &NetworkStackSet::on_progress;
  hooks.context = worker.get();
  stack->set_service_hooks(hooks);
  if (!stacks_.push_back(util::move(stack))) return false;
  if (!workers_.push_back(util::move(worker))) {
    stacks_.remove_at(stacks_.count() - 1);
    return false;
  }
  nic.enable_interrupts(&NetworkStackSet::on_activity, workers_[workers_.count() - 1].get());  // (an interface that cannot stays polled)
  return true;
}

void NetworkStackSet::on_activity(void* worker) { static_cast<Worker*>(worker)->activity.set(); }

void NetworkStackSet::on_progress(void* worker) {
  NetworkStackSet* set = static_cast<Worker*>(worker)->set;
  __atomic_add_fetch(&set->progress_, 1, __ATOMIC_ACQ_REL);
  kernel::ScopedMutex guard(set->progress_mutex_);   // (taken so that a waiter between its check and its wait still gets the notification)
  set->progress_cv_.notify_all();
}

bool NetworkStackSet::wait_for_progress(uint64_t seen, uint64_t timeout_us) {
  if (timeout_us == 0) return true;   // (0 is "forever" to the condition variable: never that here)
  kernel::ScopedMutex guard(progress_mutex_);
  if (progress() == seen) progress_cv_.wait(progress_mutex_, timeout_us);
  return true;
}

void NetworkStackSet::clear() {
  stop_service();
  for (UniquePointer<NetworkStack>& stack : stacks_) {
    stack->interface().disable_interrupts();
    stack->set_service_hooks(NetworkStack::ServiceHooks{});
  }
  stacks_.clear();
  workers_.clear();
}

bool NetworkStackSet::interrupt_driven() const {
  if (stacks_.count() == 0) return false;
  for (const UniquePointer<NetworkStack>& stack : stacks_)
    if (!stack->interface().interrupts_enabled()) return false;
  return true;
}

bool NetworkStackSet::start_service() {
  if (service_) return true;
  stop_ = false;
  for (UniquePointer<Worker>& worker : workers_) {
    worker->thread = kernel::spawn(service_main, worker.get(), "network");
    if (!worker->thread) {   // (all or none)
      service_ = true;
      stop_service();
      return false;
    }
  }
  service_ = true;
  return true;
}

void NetworkStackSet::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  for (UniquePointer<Worker>& worker : workers_) {
    if (!worker->thread) continue;
    worker->activity.set();
    kernel::join(worker->thread);
    worker->thread = nullptr;
  }
  service_ = false;
}

void NetworkStackSet::service_main(void* self) {
  Worker* worker = static_cast<Worker*>(self);
  NetworkStack& stack = *worker->stack;
  while (!__atomic_load_n(&worker->set->stop_, __ATOMIC_ACQUIRE)) {
    stack.poll();
    if (stack.saturated()) continue;  // frames are still waiting: no interrupt will say so again
    // Everything was taken: the interface may interrupt again (an interrupt masks itself until then, so a burst costs one).
    stack.interface().rearm_interrupts();
    // An interface that interrupts wakes this thread when frames arrive; then it only has to wake up by itself for the stack's timers. One
    // that is polled keeps the short period, and so does a stack whose frames wait for room in the interface (it may not interrupt when it
    // has sent some).
    const bool soon = !stack.interface().interrupts_enabled() || stack.backlogged();
    worker->activity.wait(soon ? kPolledIdleUs : kInterruptIdleUs);
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
