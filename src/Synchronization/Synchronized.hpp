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
#include "Synchronization/SpinLock.hpp"
#include "Utility/Move.hpp"

// A T that several cores may use at once: every access takes the lock. Works
// with any type - a container, an Optional, a Variant, a plain struct.
//
//   Synchronized<Optional<int>> slot(cpu);
//   slot.with([](Optional<int>& o) { o = 5; });         // one locked operation
//   { auto a = slot.access(); if (*a) use(**a); }        // lock held until `a` ends
//
// A reference or pointer into the value must not be used after the lock is
// released; copy what you need out first. As with SpinLock, never call back into
// the same Synchronized from inside `with` or while an Access is alive.
template <class T>
class Synchronized {
 public:
  // Constructs the T from `args`; `cpu` is used by the lock to pause while waiting.
  template <class... Args>
  explicit Synchronized(const hardware::ICentralProcessingUnit& cpu, Args&&... args)
      : lock_(cpu), value_(util::forward<Args>(args)...) {}
  Synchronized(const Synchronized&) = delete;
  Synchronized& operator=(const Synchronized&) = delete;

  // Holds the lock and gives access to the value until it is destroyed. If the
  // value supports begin()/end() (every IObjectContainer does) it can be used in
  // range-for: `for (auto& item : container.access())`.
  class Access {
   public:
    Access(Access&& other) : owner_(other.owner_) { other.owner_ = nullptr; }
    ~Access() {
      if (owner_) owner_->lock_.unlock();
    }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;

    T& operator*() const { return owner_->value_; }
    T* operator->() const { return &owner_->value_; }
    T& get() const { return owner_->value_; }
    auto begin() const { return owner_->value_.begin(); }
    auto end() const { return owner_->value_.end(); }

   private:
    friend class Synchronized;
    explicit Access(Synchronized* owner) : owner_(owner) {}
    Synchronized* owner_;
  };

  Access access() {
    lock_.lock();
    return Access(this);
  }

  // Runs `operation(T&)` with the lock held and returns what it returns.
  template <class Operation>
  decltype(auto) with(Operation&& operation) {
    ScopedLock guard(lock_);
    return operation(value_);
  }
  template <class Operation>
  decltype(auto) with(Operation&& operation) const {
    ScopedLock guard(lock_);
    return operation(static_cast<const T&>(value_));
  }

 private:
  mutable SpinLock lock_;
  T value_;
};
