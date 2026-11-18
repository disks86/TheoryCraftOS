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

// What the software connection manager does with the devices it finds (the policy of Linux's tb.c and of boltd, which Usb4Domain leaves to
// its caller):
//
//   - USB 3 and DisplayPort tunnels are made for every device router as it appears, as Linux does: neither gives the device access to the
//     machine's memory, and a dock's USB ports and displays should simply work. The displays are looked for again whenever the topology
//     changes and now and then (a monitor plugged into a dock that was already there), and bandwidth requests of DP adapters are answered.
//   - A PCIe tunnel gives the device DMA access to memory, so it is made only for a device the user allowed: always (its router's unique id is
//     kept in the approval store and it is let in at once the next time) or this once. Any other device router waits as a request until the
//     user answers (approve/deny), which the desktop asks for (src/Desktop/ThunderboltPrompts). A denied device is not asked about again until
//     it is plugged in again.
//
// step() does the work and must run on the thread that polls the domain; the other calls may come from any thread (the desktop's): they read a
// snapshot or queue an answer that the next step() carries out.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/Bus/Thunderbolt/Usb4Domain.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

// Where "always allow" answers are kept.
class IUsb4ApprovalStore {
 public:
  virtual bool contains(uint64_t uid) = 0;
  [[nodiscard]] virtual bool add(uint64_t uid) = 0;
  virtual void remove(uint64_t uid) = 0;

 protected:
  IUsb4ApprovalStore() = default;
  ~IUsb4ApprovalStore() = default;
};

// The store as a file (filesystem::layout::kThunderboltApproved): one hex id per line. At most kMaxApproved devices.
class FileUsb4ApprovalStore final : public IUsb4ApprovalStore {
 public:
  static constexpr uint32_t kMaxApproved = 64;
  FileUsb4ApprovalStore(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator) : file_system_(file_system), allocator_(allocator) {}
  bool contains(uint64_t uid) override;
  [[nodiscard]] bool add(uint64_t uid) override;
  void remove(uint64_t uid) override;

  // The file's text parsed into ids (lines that are not a hex id are skipped); the ids as text. For the store and its tests.
  static uint32_t parse(ConstByteView text, uint64_t* ids, uint32_t capacity);
  static uint32_t format(const uint64_t* ids, uint32_t count, ByteView out);

 private:
  uint32_t load(uint64_t* ids);
  bool save(const uint64_t* ids, uint32_t count);
  filesystem::IFileSystem& file_system_;
  allocators::IAllocator& allocator_;
};

enum class Usb4DeviceState : uint8_t {
  Connected,     // USB 3 / DisplayPort as far as the device has them; PCIe not asked for (no PCIe adapter) or not yet decided
  Waiting,       // PCIe asked for: the user has not answered
  Allowed,       // a PCIe tunnel is up
  Denied,        // the user said no (or the tunnel could not be made)
};

struct Usb4DeviceInfo {
  uint64_t uid = 0;
  uint64_t parent_uid = 0;   // the device router it hangs from (0: the host): its PCIe tunnel has to be there first
  char vendor[48] = {}, name[48] = {};
  uint8_t depth = 0;
  bool usb3 = false, pcie_capable = false;
  bool remembered = false;   // allowed always
  Usb4DeviceState state = Usb4DeviceState::Connected;
};

class Usb4Policy {
 public:
  static constexpr uint32_t kMaxDevices = usb4::kMaxRouters;
  static constexpr uint32_t kDisplayScanSteps = 50;   // how often (in steps) the displays are looked for when nothing changed

  // `store` may be null: then nothing is remembered and every device is asked about each time.
  Usb4Policy(Usb4Domain& domain, IUsb4ApprovalStore* store) : domain_(domain), store_(store) {}

  // Follows the domain's routers: tunnels for new ones, requests, answers, displays and bandwidth. Returns how many tunnels it made or took down.
  uint32_t step();

  // --- Any thread.
  uint32_t device_count() const;
  [[nodiscard]] bool device(uint32_t index, Usb4DeviceInfo* out) const;
  // The first device waiting for an answer whose parent is the host or already has PCIe (a device behind a dock is asked about once the
  // dock is allowed); false if none.
  [[nodiscard]] bool next_request(Usb4DeviceInfo* out) const;
  // The user's answer about the device with this id: allow (always: remembered), or deny. Carried out by the next step().
  void approve(uint64_t uid, bool always);
  void deny(uint64_t uid);
  // No longer allowed automatically (an allowed device keeps its tunnel until it is unplugged).
  void forget(uint64_t uid);
  uint32_t changes() const { return __atomic_load_n(&changes_, __ATOMIC_ACQUIRE); }   // grows whenever the list changes (for a UI to refresh)

 private:
  struct Entry {
    bool used = false, seen = false;
    Usb4DeviceInfo info;
  };
  struct Answer {
    uint64_t uid = 0;
    enum : uint8_t { None, Approve, ApproveAlways, Deny, Forget } kind = None;
  };
  static constexpr uint32_t kMaxAnswers = 16;
  Entry* entry_for(uint64_t uid);                     // (locked)
  void take_up(uint32_t router_index, const Usb4Router& r, uint32_t* made);
  void carry_out(const Answer& a, uint32_t* made);

  Usb4Domain& domain_;
  IUsb4ApprovalStore* store_;
  mutable kernel::Mutex mutex_;                        // entries_ and answers_
  Entry entries_[kMaxDevices];
  Answer answers_[kMaxAnswers];
  uint32_t answer_count_ = 0;
  uint32_t steps_ = 0;
  uint32_t last_routers_ = 0;
  uint32_t changes_ = 0;
};

}  // namespace hardware
