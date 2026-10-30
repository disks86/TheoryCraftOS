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


// The firmware's start-up handshake and the NVM, after iwl_run_unified_mvm_ucode and iwl_get_nvm: with the regular image alive, tell it that NVM access follows (INIT_EXTENDED_CFG,
// system group), say that NVM access is complete (NVM_ACCESS_COMPLETE, regulatory group), wait for INIT_COMPLETE, then ask for the NVM information (NVM_GET_INFO) and parse it. The
// card's hardware address comes from the CSR block.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Hardware/WirelessInterface/Intel/IwlNvm.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"

namespace hardware {

namespace iwl_cmd {
constexpr uint8_t kSystemGroup = 0x02, kInitExtendedConfig = 0x03, kInitCompleteNotification = 0x04;
constexpr uint32_t kInitFlagNvm = 1u << 1;                       // IWL_INIT_NVM
constexpr uint32_t kCapabilityLarSupport = 1;
}  // namespace iwl_cmd

// Runs the handshake on a started transport and fills `nvm` (including the hardware address). `timeout_us` bounds each wait.
bool iwl_run_unified_init(IwlTransport& transport, const IwlFirmware& firmware, IwlNvmData* nvm, uint32_t timeout_us, const char** error);

}  // namespace hardware
