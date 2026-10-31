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

// Finds the TPM the firmware presents and opens a transport to it: the ACPI TPM2 table's start method 6 is a CRB at the page of its control area; no table (or start method 0) is the FIFO
// interface at the fixed address 0xFED40000 (probed). The other start methods need the firmware's help (an ACPI method, a secure monitor call) and are reported as unsupported.
#pragma once
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/Tpm/TpmCrb.hpp"
#include "Hardware/Tpm/TpmDevice.hpp"
#include "Hardware/Tpm/TpmTis.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class MmioTpmRegisters final : public ITpmRegisters {
 public:
  explicit MmioTpmRegisters(const MmioRegion& region) : region_(region) {}
  uint8_t read8(uint32_t offset) override { return region_.read8(offset); }
  void write8(uint32_t offset, uint8_t value) override { region_.write8(offset, value); }
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

class TpmDriver {
 public:
  enum class Outcome : uint8_t { NotStarted, Started, NoTpm, UnsupportedStartMethod, MapFailed, OutOfMemory, NoAnswer };
  static constexpr uint64_t kTisBase = 0xFED40000;
  explicit TpmDriver(allocators::IAllocator& allocator) : allocator_(allocator) {}
  static const char* outcome_name(Outcome outcome);

  // `table` is the TPM2 table (empty if the firmware has none). Starts the TPM (Startup(CLEAR), which the firmware has done already).
  Outcome start(ConstByteView table, IRegisterMapper& mapper);
  Outcome start_registers(ITpmRegisters& registers, uint64_t base, bool crb);
  void stop();
  TpmDevice* device() { return device_.get(); }
  Outcome outcome() const { return outcome_; }

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<MmioTpmRegisters> mmio_;
  UniquePointer<TpmTis> tis_;
  UniquePointer<TpmCrb> crb_;
  UniquePointer<TpmDevice> device_;
  Outcome outcome_ = Outcome::NotStarted;
};

}  // namespace hardware
