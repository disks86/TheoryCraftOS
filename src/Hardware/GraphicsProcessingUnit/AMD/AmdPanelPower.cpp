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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdPanelPower.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

bool AmdPanelPower::run(AtomBios& bios, uint8_t action) {
  uint32_t params[2] = {uint32_t{action} << 24, 0};     // usPixelClock = 0, ucConfig = 0, ucAction, ucReserved[4]
  return bios.execute_table(kCommandTable, ArrayView<uint32_t>(params, 2)) == AtomBios::Status::Ok;
}

bool AmdPanelPower::power_on(IAtomHost& host, AtomBios& bios, IHpdSense& sense, uint8_t hpd) {
  if (!run(bios, kActionPowerOn)) return false;
  if (hpd == kNoHpd) return true;
  for (uint32_t i = 0; i < kSenseTries; ++i) {
    if (sense.hpd_sense(hpd)) return true;
    host.delay_ms(1);
  }
  return false;
}

}  // namespace hardware
