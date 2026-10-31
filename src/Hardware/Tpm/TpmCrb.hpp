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

// The TPM command / response buffer interface (CRB; Intel PTT and AMD's fTPM present it, start method 6 of the ACPI TPM2 table): a control area with the request and start registers and the
// addresses and sizes of a command buffer and a response buffer. Here those buffers must lie in the same 4 KiB as the control area (the usual place, 0xFED40080), which is what the
// register interface reaches. Start methods that need the firmware (ACPI start, SMC, FF-A) are not supported.
#pragma once
#include "Hardware/Tpm/ITpmTransport.hpp"

namespace hardware {

namespace crb {
constexpr uint32_t kLocState = 0x00, kLocCtrl = 0x08, kLocSts = 0x0C, kIntfId = 0x30, kCtrlReq = 0x40, kCtrlSts = 0x44, kCtrlCancel = 0x48, kCtrlStart = 0x4C;
constexpr uint32_t kCmdSize = 0x58, kCmdPaLow = 0x5C, kCmdPaHigh = 0x60, kRspSize = 0x64, kRspPa = 0x68;
constexpr uint32_t kReqCmdReady = 1, kReqGoIdle = 2, kStsFatal = 1, kStsIdle = 2;
}  // namespace crb

class TpmCrb final : public ITpmTransport {
 public:
  // `base` is the physical address the registers were mapped for (the buffers' addresses are compared against it).
  TpmCrb(ITpmRegisters& registers, uint64_t base) : registers_(registers), base_(base) {}
  bool probe();
  const char* name() const override { return "TPM 2.0 (CRB)"; }
  bool transmit(ConstByteView command, ByteBuffer* response) override;

 private:
  bool request_locality();
  bool wait(uint32_t reg, uint32_t mask, uint32_t value, uint32_t polls);
  ITpmRegisters& registers_;
  uint64_t base_;
};

}  // namespace hardware
