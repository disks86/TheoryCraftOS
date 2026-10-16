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
#include "Synchronization/Synchronized.hpp"
#include "Utility/SharedPointer.hpp"

// A SharedPointer variable that cores can read and replace at the same time.
// (A plain SharedPointer is safe to *copy* from many cores, but reading it while
// another core assigns to it is a race; this closes that gap.) load() hands out a
// private copy, so the object stays alive for as long as the caller keeps it,
// even if the cell is overwritten meanwhile.
template <class T>
class AtomicSharedPointer {
 public:
  explicit AtomicSharedPointer(const hardware::ICentralProcessingUnit& cpu) : cell_(cpu) {}
  AtomicSharedPointer(const hardware::ICentralProcessingUnit& cpu, SharedPointer<T> initial)
      : cell_(cpu, util::move(initial)) {}

  SharedPointer<T> load() const {
    return cell_.with([](const SharedPointer<T>& p) { return p; });
  }
  void store(SharedPointer<T> desired) {
    // The old value is released after the lock is dropped, so a destructor that
    // is slow (or touches other locks) never runs inside the critical section.
    SharedPointer<T> old = exchange(util::move(desired));
  }
  SharedPointer<T> exchange(SharedPointer<T> desired) {
    return cell_.with([&](SharedPointer<T>& p) {
      SharedPointer<T> old(util::move(p));
      p = util::move(desired);
      return old;
    });
  }

 private:
  Synchronized<SharedPointer<T>> cell_;
};
