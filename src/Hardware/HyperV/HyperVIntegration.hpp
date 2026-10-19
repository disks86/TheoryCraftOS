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
#include <stdint.h>
#include "Hardware/HyperV/VmBus.hpp"

namespace hardware {

// Hyper-V's integration components, the services the host manager expects a guest to run, each on a VMBus channel of its own: the
// heartbeat (the host sees the guest is alive), shutdown (the host's "Shut down" / "Restart" request) and time synchronization (the
// host's clock). Key-value exchange, volume shadow copy, file copy and guest services are not answered; the host then lists them as
// not running.
class HyperVIntegration {
 public:
  HyperVIntegration(VmBus& bus) : bus_(bus) {}
  ~HyperVIntegration() { stop(); }
  HyperVIntegration(const HyperVIntegration&) = delete;
  HyperVIntegration& operator=(const HyperVIntegration&) = delete;

  // Opens the channels of the services the host offers. Returns how many.
  uint32_t start();
  void stop();
  // Answers whatever the host has sent. Call regularly (every few tens of milliseconds) from the VMBus thread.
  void service();

  bool heartbeat_running() const { return channels_[0] != nullptr; }
  bool shutdown_running() const { return channels_[1] != nullptr; }
  bool timesync_running() const { return channels_[2] != nullptr; }
  uint64_t heartbeats() const { return heartbeats_; }

  // The host asked for a shutdown or restart (true once per request).
  bool take_shutdown_request(bool* restart);
  // The latest host time as a FILETIME (100 ns since 1601, UTC); false if none arrived. `ticks` is when it was taken (uptime_us).
  bool host_time(uint64_t* filetime, uint64_t* taken_us) const;

 private:
  enum Service { Heartbeat, Shutdown, TimeSync, kServices };
  void handle(Service service, uint8_t* packet, uint32_t length, uint64_t transaction);
  bool negotiate(Service service, uint8_t* packet, uint32_t length);

  VmBus& bus_;
  VmBusChannel* channels_[kServices] = {};
  uint64_t heartbeats_ = 0;
  bool shutdown_pending_ = false, restart_ = false;
  bool have_time_ = false;
  uint64_t host_filetime_ = 0, host_time_taken_us_ = 0;
};

}  // namespace hardware
