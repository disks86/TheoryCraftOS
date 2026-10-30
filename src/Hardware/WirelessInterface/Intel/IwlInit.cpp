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


#include "Hardware/WirelessInterface/Intel/IwlInit.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
struct InitWatcher final : IIwlNotificationHandler {
  bool complete = false;
  void on_notification(const IwlRxPacket& packet) override {
    if (packet.group == iwl_cmd::kSystemGroup && packet.command == iwl_cmd::kInitCompleteNotification) complete = true;
  }
};

void put_le32(uint8_t* p, uint32_t v) {
  for (uint32_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
}  // namespace

bool iwl_run_unified_init(IwlTransport& transport, const IwlFirmware& firmware, IwlNvmData* nvm, uint32_t timeout_us, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (!transport.running()) {
    *error = "the firmware is not running";
    return false;
  }
  InitWatcher watcher;
  transport.set_handler(&watcher);
  struct Restore {
    IwlTransport& t;
    ~Restore() { t.set_handler(nullptr); }
  } restore{transport};
  uint8_t response[16];

  uint8_t cfg[4];
  put_le32(cfg, iwl_cmd::kInitFlagNvm);                              // the unified image: NVM only (no PHY configuration command)
  IwlHostCommand command;
  command.group = iwl_cmd::kSystemGroup;
  command.opcode = iwl_cmd::kInitExtendedConfig;
  command.data = ConstByteView(cfg, sizeof cfg);
  if (transport.send_sync(command, ByteView(response, sizeof response), timeout_us, error) < 0) return false;

  uint8_t complete[4] = {};
  command.group = iwl_nvm::kRegulatoryGroup;
  command.opcode = iwl_nvm::kAccessComplete;
  command.data = ConstByteView(complete, sizeof complete);
  if (transport.send_sync(command, ByteView(response, sizeof response), timeout_us, error) < 0) return false;

  // the firmware says when its own initialisation is over
  uint32_t waited = 0;
  while (!watcher.complete) {
    transport.poll(&watcher);
    if (watcher.complete) break;
    if (waited >= timeout_us) {
      *error = "the firmware did not finish its initialisation";
      return false;
    }
    delay_us(100);
    waited += 100;
  }

  uint8_t info_request[4] = {};
  uint8_t info[640];
  command.opcode = iwl_nvm::kGetInfo;
  command.data = ConstByteView(info_request, sizeof info_request);
  const int32_t n = transport.send_sync(command, ByteView(info, sizeof info), timeout_us, error);
  if (n < 0) return false;
  if (static_cast<uint32_t>(n) > sizeof info) {
    *error = "the NVM info response is larger than expected";
    return false;
  }
  if (!iwl_parse_nvm_get_info(ConstByteView(info, static_cast<uint32_t>(n)), iwl_phy_tx_antennas(firmware.phy_config()), iwl_phy_rx_antennas(firmware.phy_config()),
                              firmware.has_capability(iwl_cmd::kCapabilityLarSupport), nvm, error))
    return false;
  if (!iwl_read_csr_hw_address(transport.device(), nvm->hw_address)) {
    *error = "no valid hardware address in the chip";
    return false;
  }
  return true;
}

}  // namespace hardware
