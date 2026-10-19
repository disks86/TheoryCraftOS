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
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/IHardware.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "uefi.hpp"

namespace hardware {

// VirtualBox's guest interface: the VMMDev PCI device (80EE:CAFE) that VirtualBox's own Guest Additions talk to. A request is a
// block of memory (a header and a body) whose physical address is written to the device's I/O port; the host fills in the answer
// before the write returns. This driver does the parts that need no HGCM (the host-service channel behind shared folders and the
// clipboard): it identifies the guest, enables mouse integration (the host sends the pointer's absolute position), follows the
// host's display-size requests, answers the memory balloon, and sends the heartbeat. The host is polled - no interrupt is used.
//
// Written from the interface as documented in VirtualBox's VMMDev.h and checked only against what could be tested: the request
// plumbing is the same on every host, but nothing here has run under VirtualBox itself.
class VmmDev final : public IHardware {
 public:
  static constexpr uint16_t kPciVendor = 0x80EE;
  static constexpr uint16_t kPciDevice = 0xCAFE;
  static constexpr uint32_t kPointerMax = 0xFFFF;   // absolute positions run 0..0xFFFF on both axes
  static constexpr uint32_t kBalloonChunkPages = 256;
  static constexpr uint32_t kMaxBalloonChunks = 1024;   // 1 GiB

  VmmDev(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator) { (void)st; }

  const char* name() const override { return "VirtualBox guest device"; }
  bool initialize() override;
  void shutdown() override;

  // Looks at what the host wants and does it: pointer position, display size, balloon, heartbeat. Call every ~10 ms from one thread.
  void service();

  // --- Mouse integration. The host has the pointer when the pointer is over the VM window with integration on.
  bool host_wants_absolute() const { return __atomic_load_n(&wants_absolute_, __ATOMIC_ACQUIRE); }
  // The latest absolute position (0..kPointerMax) if it moved since the last call.
  bool take_pointer(uint32_t* x, uint32_t* y);

  // --- Display. The size the host window asks for, once per request.
  bool take_display_request(uint32_t* width, uint32_t* height);

  // The host's clock, milliseconds since 1970 (UTC); false if the host did not answer.
  bool host_time_ms(uint64_t* out);

  uint32_t balloon_mib() const { return balloon_chunks_; }
  uint32_t requested_balloon_mib() const { return requested_chunks_; }

 private:
  struct Header;
  bool call(uint32_t type, uint32_t body_bytes);   // with request_ filled in; true if the host accepted it (rc >= 0)
  uint8_t* body() const;
  bool report_guest();
  void handle_events(uint32_t events);
  void refresh_mouse();
  void follow_balloon();
  bool balloon_exchange(uint8_t* chunk, bool inflate);

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  uint16_t port_ = 0;
  ArrayView<uint8_t> request_;       // one request at a time, in memory below 4 GiB
  ArrayView<uint8_t*> chunk_base_;   // ballooned chunks
  kernel::Mutex mutex_;          // one request at a time
  kernel::Mutex pointer_lock_;   // the values below
  bool started_ = false;

  // Written by service(), read by the mouse and the desktop.
  bool wants_absolute_ = false;
  uint32_t pointer_x_ = 0, pointer_y_ = 0;
  bool pointer_dirty_ = false;
  uint32_t display_w_ = 0, display_h_ = 0;
  bool display_pending_ = false;
  uint32_t balloon_chunks_ = 0, requested_chunks_ = 0;
  uint64_t heartbeat_interval_us_ = 0, last_heartbeat_us_ = 0, last_balloon_check_us_ = 0;
};

}  // namespace hardware
