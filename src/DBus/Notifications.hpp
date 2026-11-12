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

// The desktop notifications service (the freedesktop.org Desktop Notifications Specification 1.2) the kernel itself serves on the session bus: name
// org.freedesktop.Notifications, object /org/freedesktop/Notifications. notify-send, libnotify and GLib's GNotification send Notify there and the desktop
// shows a toast (INotificationDisplay). The service tells when a notification goes (NotificationClosed: 1 expired, 3 closed by CloseNotification).
//
// Supported: Notify (replaces_id, expire_timeout: -1 the default kDefaultTimeoutMs, 0 never), CloseNotification, GetCapabilities ("body"),
// GetServerInformation, Introspect, Peer. Not: actions (no ActionInvoked), icons and images, markup, sounds; hints are read past.
#pragma once
#include <stdint.h>
#include "DBus/Bus.hpp"
#include "ObjectContainer/List.hpp"

namespace dbus {

// Where the notifications are shown (the desktop's toasts). Called on the bus's thread.
class INotificationDisplay {
 public:
  // Shows one; returns its number for close() (0: it could not be shown). `timeout_ms` 0: until closed.
  virtual uint32_t show(StringView app_name, StringView summary, StringView body, uint32_t timeout_ms) = 0;
  virtual void close(uint32_t shown) = 0;

 protected:
  INotificationDisplay() = default;
  ~INotificationDisplay() = default;
};

class NotificationService final : public LocalPeer {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 8000;
  static constexpr uint32_t kMaxShowing = 256;
  static constexpr uint64_t kMaxTextBytes = 4096;      // summary and body are cut there (at a character)

  NotificationService(Bus& bus, INotificationDisplay& display, allocators::IAllocator& allocator);
  ~NotificationService();
  // Connects to the bus and takes the name. False if it could not.
  bool start();
  uint32_t showing() const { return uint32_t(showing_.count()); }

  void deliver(const Message& message) override;
  void tick(uint64_t now_us) override;

 private:
  struct Shown {
    uint32_t id = 0, toast = 0;
    uint32_t timeout_ms = 0;      // 0: never
    uint64_t expires_us = 0;      // (0 with a timeout: from the next tick on)
  };
  void notify(const Message& call);
  void close_notification(const Message& call);
  void closed(uint32_t id, uint32_t reason);
  void reply(Message& m);

  Bus& bus_;
  INotificationDisplay& display_;
  allocators::IAllocator& allocator_;
  uint32_t client_ = 0;
  uint32_t next_id_ = 1;
  uint64_t now_us_ = 0;
  List<Shown> showing_;
};

}  // namespace dbus
