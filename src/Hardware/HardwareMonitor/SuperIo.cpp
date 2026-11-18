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

#include "Hardware/HardwareMonitor/SuperIo.hpp"

namespace hardware {

namespace {

using V = SuperIoVendor;
using C = SuperIoChip;
constexpr SuperIoSupport kYes = SuperIoSupport::Supported, kNo = SuperIoSupport::IdentifiedOnly;
constexpr uint8_t kNctDevice = 0x0B, kIteDevice = 0x04, kFintekDevice = 0x04;

// Ids as Linux's drivers know them. Nuvoton parts keep a revision in the low bits: NCT6796 and NCT6798 differ only in bit 3, so those are matched
// with 0xFFF8 (as nct6775 does) and the NCT668x with 0xFFF0 (as nct6683 does).
constexpr SuperIoChipInfo kChips[] = {
    {0xB470, 0xFFF8, V::Nuvoton, C::Nct6775, "NCT6775", kNctDevice, kYes, false, nullptr},
    {0xC330, 0xFFF8, V::Nuvoton, C::Nct6776, "NCT6776", kNctDevice, kYes, false, nullptr},
    {0xC560, 0xFFF8, V::Nuvoton, C::Nct6779, "NCT6779", kNctDevice, kYes, false, nullptr},
    {0xC800, 0xFFF8, V::Nuvoton, C::Nct6791, "NCT6791", kNctDevice, kYes, true, nullptr},
    {0xC910, 0xFFF8, V::Nuvoton, C::Nct6792, "NCT6792", kNctDevice, kYes, true, nullptr},
    {0xD120, 0xFFF8, V::Nuvoton, C::Nct6793, "NCT6793", kNctDevice, kYes, true, nullptr},
    {0xD350, 0xFFF8, V::Nuvoton, C::Nct6795, "NCT6795", kNctDevice, kYes, true, nullptr},
    {0xD420, 0xFFF8, V::Nuvoton, C::Nct6796, "NCT6796", kNctDevice, kYes, true, nullptr},
    {0xD450, 0xFFF8, V::Nuvoton, C::Nct6797, "NCT6797", kNctDevice, kYes, true, nullptr},
    {0xD428, 0xFFF8, V::Nuvoton, C::Nct6798, "NCT6798", kNctDevice, kYes, true, nullptr},
    {0xD800, 0xFFF8, V::Nuvoton, C::Nct6799, "NCT6799", kNctDevice, kYes, true, nullptr},
    {0xC730, 0xFFF0, V::Nuvoton, C::Nct6683, "NCT6683", kNctDevice, kYes, false, nullptr},
    {0xD440, 0xFFF0, V::Nuvoton, C::Nct6686, "NCT6686", kNctDevice, kYes, false, nullptr},
    {0xD590, 0xFFF0, V::Nuvoton, C::Nct6687, "NCT6687", kNctDevice, kYes, false, nullptr},

    {0x8620, 0xFFFF, V::Ite, C::It8620, "IT8620", kIteDevice, kYes, false, nullptr},
    {0x8622, 0xFFFF, V::Ite, C::It8622, "IT8622", kIteDevice, kYes, false, nullptr},
    {0x8625, 0xFFFF, V::Ite, C::It8625, "IT8625", kIteDevice, kYes, false, nullptr},
    {0x8628, 0xFFFF, V::Ite, C::It8628, "IT8628", kIteDevice, kYes, false, nullptr},
    {0x8655, 0xFFFF, V::Ite, C::It8655, "IT8655", kIteDevice, kYes, false, nullptr},
    {0x8665, 0xFFFF, V::Ite, C::It8665, "IT8665", kIteDevice, kYes, false, nullptr},
    {0x8686, 0xFFFF, V::Ite, C::It8686, "IT8686", kIteDevice, kYes, false, nullptr},
    {0x8688, 0xFFFF, V::Ite, C::It8688, "IT8688", kIteDevice, kYes, false, nullptr},
    {0x8689, 0xFFFF, V::Ite, C::It8689, "IT8689", kIteDevice, kYes, false, nullptr},
    {0x8695, 0xFFFF, V::Ite, C::It87952, "IT87952 (IT8695)", kIteDevice, kYes, false, nullptr},
    {0x8705, 0xFFFF, V::Ite, C::It8705, "IT8705", kIteDevice, kYes, false, nullptr},
    {0x8712, 0xFFFF, V::Ite, C::It8712, "IT8712", kIteDevice, kYes, false, nullptr},
    {0x8716, 0xFFFF, V::Ite, C::It8716, "IT8716", kIteDevice, kYes, false, nullptr},
    {0x8718, 0xFFFF, V::Ite, C::It8718, "IT8718", kIteDevice, kYes, false, nullptr},
    {0x8720, 0xFFFF, V::Ite, C::It8720, "IT8720", kIteDevice, kYes, false, nullptr},
    {0x8721, 0xFFFF, V::Ite, C::It8721, "IT8721", kIteDevice, kYes, false, nullptr},
    {0x8726, 0xFFFF, V::Ite, C::It8726, "IT8726", kIteDevice, kYes, false, nullptr},
    {0x8728, 0xFFFF, V::Ite, C::It8728, "IT8728", kIteDevice, kYes, false, nullptr},
    {0x8771, 0xFFFF, V::Ite, C::It8771, "IT8771", kIteDevice, kYes, false, nullptr},
    {0x8772, 0xFFFF, V::Ite, C::It8772, "IT8772", kIteDevice, kYes, false, nullptr},
    {0x8733, 0xFFFF, V::Ite, C::It8792, "IT8792/IT8795", kIteDevice, kYes, false, nullptr},   // (the IT8792E reports 0x8733)

    {0x0507, 0xFFFF, V::Fintek, C::F71858, "F71858", kFintekDevice, kNo, false, "temperatures in a different 16-bit format, not implemented"},
    {0x0601, 0xFFFF, V::Fintek, C::F71862, "F71862/F71863", kFintekDevice, kYes, false, nullptr},
    {0x1106, 0xFFFF, V::Fintek, C::F71868, "F71868", kFintekDevice, kYes, false, nullptr},
    {0x0814, 0xFFFF, V::Fintek, C::F71869, "F71869", kFintekDevice, kYes, false, nullptr},
    {0x1007, 0xFFFF, V::Fintek, C::F71869A, "F71869A", kFintekDevice, kYes, false, nullptr},
    {0x0541, 0xFFFF, V::Fintek, C::F71882, "F71882", kFintekDevice, kYes, false, nullptr},
    {0x0723, 0xFFFF, V::Fintek, C::F71889, "F71889", kFintekDevice, kYes, false, nullptr},
    {0x0909, 0xFFFF, V::Fintek, C::F71889E, "F71889E", kFintekDevice, kYes, false, nullptr},
    {0x1005, 0xFFFF, V::Fintek, C::F71889A, "F71889A", kFintekDevice, kYes, false, nullptr},
    {0x0901, 0xFFFF, V::Fintek, C::F71808E, "F71808E", kFintekDevice, kNo, false, "fewer channels at other registers, not implemented"},
    {0x0704, 0xFFFF, V::Fintek, C::F81865, "F81865", kFintekDevice, kNo, false, "fewer channels at other registers, not implemented"},
};

bool nothing_there(uint16_t id) { return id == 0xFFFF || id == 0x0000; }

}  // namespace

ArrayView<const SuperIoChipInfo> superio_chip_table() { return ArrayView<const SuperIoChipInfo>(kChips); }

const char* superio_vendor_name(SuperIoVendor vendor) {
  switch (vendor) {
    case SuperIoVendor::Nuvoton: return "Nuvoton";
    case SuperIoVendor::Ite: return "ITE";
    case SuperIoVendor::Fintek: return "Fintek";
  }
  return "?";
}

const SuperIoChipInfo* superio_lookup(bool ite_key, uint16_t chip_id, uint16_t vendor_id) {
  const SuperIoVendor vendor = ite_key ? SuperIoVendor::Ite : vendor_id == superio::kFintekVendorId ? SuperIoVendor::Fintek : SuperIoVendor::Nuvoton;
  for (const SuperIoChipInfo& info : kChips) {
    if (info.vendor == vendor && (chip_id & info.id_mask) == info.id) return &info;
  }
  return nullptr;
}

void SuperIoConfig::enter(bool ite) {
  io_.write8(port_, 0x87);
  if (ite) {
    io_.write8(port_, 0x01);
    io_.write8(port_, 0x55);
    io_.write8(port_, port_ == 0x4E ? 0xAA : 0x55);
  } else {
    io_.write8(port_, 0x87);
  }
}

void SuperIoConfig::exit(SuperIoVendor vendor) {
  switch (vendor) {
    case SuperIoVendor::Ite:
      write(superio::kRegConfigControl, 0x02);
      break;
    case SuperIoVendor::Nuvoton:
      io_.write8(port_, 0xAA);
      write(superio::kRegConfigControl, 0x02);   // (nct6775's superio_exit; harmless once the key is out)
      break;
    case SuperIoVendor::Fintek:
      io_.write8(port_, 0xAA);
      break;
  }
}

void SuperIoConfig::exit_unmatched(bool ite) {
  if (ite) write(superio::kRegConfigControl, 0x02);
  else io_.write8(port_, 0xAA);
}

uint8_t SuperIoConfig::read(uint8_t reg) {
  io_.write8(port_, reg);
  return io_.read8(static_cast<uint16_t>(port_ + 1));
}

void SuperIoConfig::write(uint8_t reg, uint8_t value) {
  io_.write8(port_, reg);
  io_.write8(static_cast<uint16_t>(port_ + 1), value);
}

Optional<SuperIoChipFound> superio_probe(IPortIo& io, uint16_t config_port) {
  SuperIoConfig config(io, config_port);
  // Nuvoton/Fintek key first, then ITE's: neither chip takes the other's key (both need the exact sequence), so the order only decides which
  // chip a board with both at one port - which cannot happen - would report.
  constexpr bool kKeys[2] = {false, true};
  Optional<SuperIoChipFound> unknown;
  for (bool ite : kKeys) {
    config.enter(ite);
    const uint16_t id = config.read16(superio::kRegChipId);
    if (nothing_there(id)) {
      config.exit_unmatched(ite);
      continue;
    }
    SuperIoChipFound found;
    found.config_port = config_port;
    found.raw_id = id;
    found.revision = config.read(superio::kRegRevision);
    const uint16_t vendor_id = ite ? 0 : config.read16(superio::kRegVendorId);
    found.info = superio_lookup(ite, id, vendor_id);
    if (!found.info) {
      config.exit_unmatched(ite);
      found.problem = "unknown chip id";
      if (!unknown) unknown = found;   // (keep looking: the other key may find a known chip)
      continue;
    }
    config.select(found.info->sensor_device);
    found.sensor_active = (config.read(superio::kRegActivate) & 0x01) != 0;
    const uint16_t base = config.read16(superio::kRegBase) & static_cast<uint16_t>(~(superio::kWindowAlign - 1));
    if (found.info->unlock_io_space && found.sensor_active) {
      const uint8_t lock = config.read(superio::kRegNctIoLock);
      if (lock & superio::kNctIoLockBit) config.write(superio::kRegNctIoLock, static_cast<uint8_t>(lock & ~superio::kNctIoLockBit));
    }
    config.exit(found.info->vendor);
    // The window is 8 ports; one at 0 or wrapping past 0xFFFF (a floating bus reads 0xFFF8) is not set up.
    if (base == 0 || base > 0xFFFF - superio::kWindowAlign) found.problem = "sensor window not assigned by the firmware";
    else found.sensor_base = base;
    if (!found.sensor_active && !found.problem) found.problem = "sensor device not activated by the firmware";
    if (found.info->support != SuperIoSupport::Supported && !found.problem) found.problem = found.info->note;
    return found;
  }
  return unknown;
}

}  // namespace hardware
