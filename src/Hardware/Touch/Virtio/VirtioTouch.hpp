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
#include "Hardware/Bus/Virtio/VirtioInputDevice.hpp"
#include "Hardware/Mouse/IMouse.hpp"
#include "Hardware/Touch/ITouch.hpp"
#include "Hardware/Touch/LinuxMultitouch.hpp"
#include "Hardware/Touch/TouchTracker.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

// QEMU's virtio-multitouch-pci: a touchscreen in the Linux multitouch protocol (LinuxMultitouch). It is a touch device, and through pointer() also a mouse (its first finger the left button) while pointer emulation is on.
class VirtioTouch final : public ITouch {
 public:
  VirtioTouch(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator, efi::SystemTable* st)
      : device_(pci, device, allocator, st), pointer_(*this) {}

  const char* name() const override { return "virtio touchscreen"; }
  bool initialize() override;
  void shutdown() override { device_.shutdown(); }

  bool poll(TouchEvent* out) override;
  void set_pointer_emulation(bool on) override;
  // The mouse side (add it to the mice): events only while pointer emulation is on.
  IMouse& pointer() { return pointer_; }


 private:
  // The mouse side: its events in a queue of the device's, under the device's one lock (so that the two sides never take two locks).
  class Pointer final : public IMouse {
   public:
    explicit Pointer(VirtioTouch& owner) : owner_(owner) {}
    const char* name() const override { return "virtio touchscreen (pointer)"; }
    bool initialize() override { return true; }
    void shutdown() override {}
    bool poll(MouseEvent* out) override { return owner_.poll_mouse(out); }
    bool is_absolute() const override { return true; }
    uint32_t max_x() const override { return owner_.device_.absolute_max(0); }
    uint32_t max_y() const override { return owner_.device_.absolute_max(1); }
   private:
    VirtioTouch& owner_;
  };
  bool poll_mouse(MouseEvent* out);
  void emit(const MouseEvent& e);
  void read();      // (with mutex_ held)
  void frame();      // (a frame closed: the mouse side follows the first finger)

  VirtioInputDevice device_;
  Pointer pointer_;
  kernel::Mutex mutex_;         // the device and the state below: the touch and mouse sides read in turns
  TouchTracker tracker_;
  LinuxMultitouch protocol_;
  bool emulation_ = true;
  bool emulated_down_ = false;
  uint32_t emulated_x_ = 0, emulated_y_ = 0;
  static constexpr uint32_t kMouseQueue = 32;
  MouseEvent mouse_[kMouseQueue] = {};
  uint32_t mouse_head_ = 0, mouse_count_ = 0;
};

}  // namespace hardware
