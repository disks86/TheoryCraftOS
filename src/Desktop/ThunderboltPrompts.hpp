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

// The desktop's question when a USB4 or Thunderbolt device asks for PCIe, which gives it direct access to the computer's memory: one device at
// a time, in a dialog that waits for the user. "Don't allow" changes nothing and is the default; "Always allow" lets the device in now and
// whenever it is plugged in again; "Allow once" only now. The decision is the connection manager's to carry out (IThunderboltApprovals,
// implemented over hardware::Usb4Policy by whoever wires it up), so the desktop knows nothing of the bus.
//
// pump() runs on the desktop's loop: it takes the answer of the dialog and asks the next question. A device that is pulled out while it is
// being asked about leaves the dialog open; its answer then does nothing.
#pragma once
#include <stdint.h>
#include "Gui/MessageDialog.hpp"
#include "Utility/String.hpp"

namespace desktop {

class IThunderboltApprovals {
 public:
  struct Request {
    uint64_t id = 0;
    char vendor[48] = {}, name[48] = {};
  };
  // The next device waiting for an answer; false if none.
  virtual bool next_request(Request* out) = 0;
  virtual void approve(uint64_t id, bool always) = 0;
  virtual void deny(uint64_t id) = 0;

 protected:
  IThunderboltApprovals() = default;
  ~IThunderboltApprovals() = default;
};

class ThunderboltPrompts {
 public:
  ThunderboltPrompts(allocators::IAllocator& allocator, gui::WindowManager& windows, IThunderboltApprovals& approvals);
  ThunderboltPrompts(const ThunderboltPrompts&) = delete;
  ThunderboltPrompts& operator=(const ThunderboltPrompts&) = delete;

  void pump();
  bool asking() const { return asking_ != 0; }
  gui::MessageDialog& dialog() { return dialog_; }

 private:
  allocators::IAllocator& allocator_;
  gui::WindowManager& windows_;
  IThunderboltApprovals& approvals_;
  gui::MessageDialog dialog_;
  uint64_t asking_ = 0;        // the device the dialog is about (0: none)
};

}  // namespace desktop
