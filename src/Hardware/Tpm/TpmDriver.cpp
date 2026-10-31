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

#include "Hardware/Tpm/TpmDriver.hpp"

namespace hardware {

const char* TpmDriver::outcome_name(Outcome o) {
  switch (o) {
    case Outcome::NotStarted: return "not started";
    case Outcome::Started: return "started";
    case Outcome::NoTpm: return "none found";
    case Outcome::UnsupportedStartMethod: return "the firmware's start method is not supported";
    case Outcome::MapFailed: return "the registers could not be mapped";
    case Outcome::OutOfMemory: return "out of memory";
    case Outcome::NoAnswer: return "it does not answer";
  }
  return "";
}

TpmDriver::Outcome TpmDriver::start(ConstByteView table, IRegisterMapper& mapper) {
  stop();
  Tpm2Table info;
  bool crb = false;
  uint64_t base = kTisBase;
  if (!table.empty() && tpm2_parse_table(table, &info)) {
    if (info.start_method == Tpm2Table::kStartCrb) {
      crb = true;
      base = info.control_area & ~uint64_t{0xFFF};
    } else if (info.start_method != Tpm2Table::kStartMmio) {
      return outcome_ = Outcome::UnsupportedStartMethod;
    }
  }
  MmioRegion region;
  if (!mapper.map_registers(base, 0x1000, &region)) return outcome_ = Outcome::MapFailed;
  mmio_ = UniquePointer<MmioTpmRegisters>::make(allocator_, region);
  if (!mmio_) return outcome_ = Outcome::OutOfMemory;
  return start_registers(*mmio_, base, crb);
}

TpmDriver::Outcome TpmDriver::start_registers(ITpmRegisters& registers, uint64_t base, bool crb) {
  ITpmTransport* transport = nullptr;
  if (crb) {
    crb_ = UniquePointer<TpmCrb>::make(allocator_, registers, base);
    if (!crb_) return outcome_ = Outcome::OutOfMemory;
    if (!crb_->probe()) return outcome_ = Outcome::NoTpm;
    transport = crb_.get();
  } else {
    tis_ = UniquePointer<TpmTis>::make(allocator_, registers);
    if (!tis_) return outcome_ = Outcome::OutOfMemory;
    if (!tis_->probe()) return outcome_ = Outcome::NoTpm;
    transport = tis_.get();
  }
  device_ = UniquePointer<TpmDevice>::make(allocator_, *transport, allocator_);
  if (!device_) return outcome_ = Outcome::OutOfMemory;
  if (!device_->startup()) {
    device_.reset();
    return outcome_ = Outcome::NoAnswer;
  }
  return outcome_ = Outcome::Started;
}

void TpmDriver::stop() {
  device_.reset();
  tis_.reset();
  crb_.reset();
  mmio_.reset();
  outcome_ = Outcome::NotStarted;
}

}  // namespace hardware
