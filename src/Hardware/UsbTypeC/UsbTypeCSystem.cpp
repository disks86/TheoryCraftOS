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

#include "Hardware/UsbTypeC/UsbTypeCSystem.hpp"
#include <new>

namespace hardware {

void UsbTypeCSystem::FreePublished::operator()(Published* p) const {
  allocators::IAllocator* allocator = p->allocator;
  p->~Published();
  allocator->deallocate(p);
}

UsbTypeCSystem::~UsbTypeCSystem() {
  Published* last = current_;
  current_ = nullptr;
  if (last) last->retire(FreePublished{}, domain_);
  domain_.barrier();   // (every retired copy is freed before the domain and the allocator its deleter uses go away)
}

void UsbTypeCSystem::publish() {
  // (placement new into allocator memory: RCU ends the copy's life after the grace period, not an owner - see FreePublished)
  void* memory = allocator_.allocate(sizeof(Published));
  if (!memory) {
    dirty_ = true;
    return;
  }
  Published* next = new (memory) Published();
  work_.generation = ++generation_;
  next->snapshot = work_;
  next->allocator = &allocator_;
  Published* old = current_;
  kernel::rcu_assign_pointer(&current_, next);
  if (old) old->retire(FreePublished{}, domain_);
  domain_.poll();
  dirty_ = false;
  __atomic_fetch_add(&publications_, 1u, __ATOMIC_RELAXED);
}

bool UsbTypeCSystem::start(uint64_t now_ms) {
  ++attempts_;
  const UcsiCore::Result r = core_.initialize(&work_);
  if (r != UcsiCore::Result::Ok) {
    work_.present = false;
    retry_at_ms_ = now_ms + options_.retry_ms;
    publish();
    return false;
  }
  failures_ = 0;
  last_check_ms_ = last_full_ms_ = now_ms;
  __atomic_store_n(&ready_, 1u, __ATOMIC_RELEASE);
  publish();
  return true;
}

void UsbTypeCSystem::failed(UcsiCore::Result result, uint64_t now_ms) {
  if (result != UcsiCore::Result::Timeout && result != UcsiCore::Result::Busy && result != UcsiCore::Result::Transport) {
    failures_ = 0;
    return;
  }
  if (++failures_ < options_.max_failures) return;
  // The policy manager stopped answering: what is known about the connectors is not current any more; it is started again later.
  __atomic_store_n(&ready_, 0u, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < work_.connector_count && i < kUsbTypeCMaxConnectors; ++i) work_.connectors[i].status_valid = false;
  attempts_ = 0;
  retry_at_ms_ = now_ms + options_.retry_ms;
  publish();
}

void UsbTypeCSystem::poll(uint64_t now_ms) {
  if (!started()) {
    if (attempts_ >= options_.max_start_attempts || now_ms < retry_at_ms_) return;
    (void)start(now_ms);
    return;
  }
  bool changed = dirty_;
  const bool notified = __atomic_exchange_n(&notify_pending_, 0u, __ATOMIC_ACQ_REL) != 0;
  if (notified || now_ms - last_check_ms_ >= options_.idle_check_ms) {
    last_check_ms_ = now_ms;
    bool one = false;
    const UcsiCore::Result r = core_.service(&work_, &one);
    changed = changed || one;
    failed(r, now_ms);
    if (!started()) return;
  }
  if (now_ms - last_full_ms_ >= options_.full_refresh_ms) {
    last_full_ms_ = now_ms;
    for (uint32_t i = 0; i < work_.connector_count; ++i) {
      bool one = false;
      const UcsiCore::Result r = core_.refresh_connector(&work_, i, &one);
      changed = changed || one;
      failed(r, now_ms);
      if (!started()) return;
    }
  }
  if (changed) publish();
}

void UsbTypeCSystem::run() {
  __atomic_store_n(&running_, 1u, __ATOMIC_RELEASE);
  constexpr uint32_t kSliceUs = 10000, kSlices = 10;   // (looks at the notification and the stop request every 10 ms, polls at least every 100 ms)
  while (!__atomic_load_n(&stop_, __ATOMIC_ACQUIRE)) {
    poll(clock_.now_us() / 1000);
    for (uint32_t i = 0; i < kSlices && !__atomic_load_n(&stop_, __ATOMIC_ACQUIRE) && !__atomic_load_n(&notify_pending_, __ATOMIC_ACQUIRE); ++i)
      clock_.sleep_us(kSliceUs);
  }
  __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);
}

void UsbTypeCSystem::on_notify(acpi::Node* device, uint32_t value) {
  if (device_ == nullptr || device != device_ || value != kNotifyValue) return;
  __atomic_fetch_add(&notifications_, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&notify_pending_, 1u, __ATOMIC_RELEASE);
}

bool UsbTypeCSystem::snapshot(UsbTypeCSnapshot* out) const {
  kernel::RcuReader reader(domain_);
  const Published* p = kernel::rcu_dereference(&current_);
  if (!p) {
    *out = UsbTypeCSnapshot();
    return false;
  }
  *out = p->snapshot;
  return true;
}

bool UsbTypeCSystem::present() const {
  kernel::RcuReader reader(domain_);
  const Published* p = kernel::rcu_dereference(&current_);
  return p && p->snapshot.present;
}

uint32_t UsbTypeCSystem::connector_count() const {
  kernel::RcuReader reader(domain_);
  const Published* p = kernel::rcu_dereference(&current_);
  return p && p->snapshot.present ? p->snapshot.connector_count : 0;
}

bool UsbTypeCSystem::connector(uint32_t index, UsbTypeCConnector* out) const {
  kernel::RcuReader reader(domain_);
  const Published* p = kernel::rcu_dereference(&current_);
  if (!p || !p->snapshot.present || index >= p->snapshot.connector_count || index >= kUsbTypeCMaxConnectors) return false;
  *out = p->snapshot.connectors[index];
  return true;
}

uint32_t UsbTypeCSystem::generation() const {
  kernel::RcuReader reader(domain_);
  const Published* p = kernel::rcu_dereference(&current_);
  return p ? p->snapshot.generation : 0;
}

}  // namespace hardware
