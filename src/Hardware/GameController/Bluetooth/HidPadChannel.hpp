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

// Where a Bluetooth transport hands a game controller's HID reports to its driver: the meeting point of the two transports (HID over GATT for
// LE pads, src/Hardware/Bluetooth/HidOverGatt; HIDP over L2CAP for classic ones) and the pad drivers (BluetoothGamepad.hpp), so that neither
// knows the other.
//
// The channel is shared (SharedPointer) by the transport, which owns the connection, and the pad, which the game controller service owns and
// may keep a while after the link is gone. The transport attach()es its link, deliver()s every input report, and detach()es before the link
// object goes away; from then on the pad sees attached() == false (and is dropped by the service) and nothing calls into the link again.
//
// Locking: input reports go through a ring with a lock of its own, never held while calling the link, so a transport may deliver() while
// holding its own locks. Calls into the link (send_output, get_feature) hold the link lock; detach() takes it too and therefore waits for a
// call in progress. A transport must not hold, while calling detach(), a lock that its own send_output()/get_feature() take.
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// What a transport offers a pad.
class IHidReportLink {
 public:
  // An output report, its id first (LE: written to the Output Report characteristic with that id, the id itself not sent; classic: HIDP
  // DATA|Output (0xA2) on the interrupt channel, the id following the header byte). False if it could not be sent.
  virtual bool send_output(ConstByteView report) = 0;
  // A feature report (classic: GET_REPORT on the control channel; LE: a read of the Feature Report characteristic with that id), id first in
  // `out` when the device numbers its reports. False if the device has none or did not answer.
  virtual bool get_feature(uint8_t id, ByteView out, uint32_t* length) = 0;

 protected:
  IHidReportLink() = default;
  ~IHidReportLink() = default;
};

class HidPadChannel {
 public:
  static constexpr uint32_t kRing = 16, kMaxReport = 96;

  HidPadChannel() = default;
  HidPadChannel(const HidPadChannel&) = delete;
  HidPadChannel& operator=(const HidPadChannel&) = delete;

  // --- The transport's side.
  void attach(IHidReportLink* link) {
    kernel::ScopedMutex lock(link_mutex_);
    link_ = link;
    __atomic_store_n(&attached_, link != nullptr, __ATOMIC_RELEASE);
  }
  void detach() { attach(nullptr); }
  // An input report, laid out as the pad's report descriptor numbers it: with its id byte first when the descriptor uses report ids (an LE
  // transport, whose notifications carry no id, puts the Report Reference's id in front), without one when it does not. Longer reports are
  // cut at kMaxReport; a full ring drops the oldest, since a newer report holds the newer state.
  void deliver(ConstByteView report) {
    kernel::ScopedMutex lock(ring_mutex_);
    if (count_ == kRing) {
      head_ = (head_ + 1) % kRing;
      --count_;
      __atomic_add_fetch(&dropped_, 1, __ATOMIC_RELAXED);
    }
    Slot& s = ring_[(head_ + count_) % kRing];
    const ConstByteView kept = report.at_most(kMaxReport);
    ByteView(s.data).first(kept.size()).copy_from(kept);
    s.length = static_cast<uint32_t>(kept.size());
    ++count_;
  }

  // --- The pad's side.
  bool attached() const { return __atomic_load_n(&attached_, __ATOMIC_ACQUIRE); }
  // The oldest queued report (copied into `out`, cut to fit); false if none is waiting.
  bool take(ByteView out, uint32_t* length) {
    kernel::ScopedMutex lock(ring_mutex_);
    if (count_ == 0) return false;
    const Slot& s = ring_[head_];
    const ConstByteView data = ConstByteView(s.data).first(s.length).at_most(out.size());
    out.first(data.size()).copy_from(data);
    *length = static_cast<uint32_t>(data.size());
    head_ = (head_ + 1) % kRing;
    --count_;
    return true;
  }
  bool send_output(ConstByteView report) {
    kernel::ScopedMutex lock(link_mutex_);
    return link_ && link_->send_output(report);
  }
  bool get_feature(uint8_t id, ByteView out, uint32_t* length) {
    kernel::ScopedMutex lock(link_mutex_);
    *length = 0;
    return link_ && link_->get_feature(id, out, length);
  }
  uint32_t dropped() const { return __atomic_load_n(&dropped_, __ATOMIC_RELAXED); }

 private:
  struct Slot {
    uint8_t data[kMaxReport];
    uint32_t length;
  };
  kernel::Mutex link_mutex_;
  IHidReportLink* link_ = nullptr;   // (non-owning; valid between attach and detach)
  bool attached_ = false;
  kernel::Mutex ring_mutex_;
  Slot ring_[kRing] = {};
  uint32_t head_ = 0, count_ = 0, dropped_ = 0;
};

}  // namespace hardware
