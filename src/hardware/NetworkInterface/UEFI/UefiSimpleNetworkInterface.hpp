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
#include "hardware/NetworkInterface/INetworkInterface.hpp"
#include "uefi.hpp"

namespace hardware {

// Fallback for NICs this OS has no driver for: borrows the firmware's own driver
// through the UEFI Simple Network Protocol. Works with any NIC the firmware
// supports and needs no hardware knowledge, but only while boot services are
// active, and it is only as fast as the firmware's driver.
class UefiSimpleNetworkInterface final : public INetworkInterface {
 public:
  explicit UefiSimpleNetworkInterface(efi::SimpleNetwork* snp, efi::SystemTable* st) : snp_(snp), st_(st) {}

  const char* name() const override { return "UEFI network (firmware driver)"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  bool transmit(const uint8_t* frame, uint32_t length) override;
  bool receive(uint8_t* buffer, uint32_t capacity, uint32_t* length) override;
  NetworkStatistics statistics() const override { return stats_; }

 private:
  efi::SimpleNetwork* snp_;
  efi::SystemTable* st_;
  MacAddress mac_{};
  bool started_ = false;
  bool we_initialized_ = false;
  // The firmware keeps a pointer to the frame being sent until GetStatus returns
  // it, so frames are staged here, one at a time.
  alignas(64) uint8_t staging_[kEthernetMaxFrame + 64] = {};
  NetworkStatistics stats_;
};

}  // namespace hardware
