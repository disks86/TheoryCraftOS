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

#include "Hardware/WirelessInterface/Realtek/RtwEfuse.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {

// Copies the present words of one patch. `*phy` points at the first data byte and moves past the patch.
bool replay_words(ConstByteView physical, uint64_t limit, uint32_t block, uint8_t word_enable, uint64_t* phy, ByteView logical) {
  for (uint32_t i = 0; i < 4; ++i) {
    if (word_enable & (1u << i)) continue;
    const uint64_t log = uint64_t{block} * 8 + i * 2;
    if (*phy + 2 > limit || log + 2 > logical.size()) return false;
    logical[log] = physical[*phy];
    logical[log + 1] = physical[*phy + 1];
    *phy += 2;
  }
  return true;
}

}  // namespace

bool rtw88_efuse_logical_map(ConstByteView physical, uint32_t protect_size, ByteView logical) {
  logical.fill(0xFF);
  if (protect_size > physical.size()) return false;
  const uint64_t limit = physical.size() - protect_size;
  uint64_t phy = 0;
  while (phy < limit) {
    const uint8_t hdr1 = physical[phy];
    uint8_t hdr2 = 0xFF;
    (void)physical.try_get(phy + 1, &hdr2);
    if (hdr1 == 0xFF) break;
    uint32_t block;
    uint8_t word_enable;
    if ((hdr1 & 0x1F) == 0x0F) {
      if (hdr2 == 0xFF) break;
      if (phy + 2 > limit) return false;
      block = static_cast<uint32_t>((hdr2 & 0xF0) >> 1 | (hdr1 >> 5 & 0x07));
      word_enable = hdr2 & 0x0F;
      phy += 2;
    } else {
      block = (hdr1 & 0xF0) >> 4;
      word_enable = hdr1 & 0x0F;
      phy += 1;
    }
    if (!replay_words(physical, limit, block, word_enable, &phy, logical)) return false;
  }
  return true;
}

bool rtw89_efuse_logical_map(ConstByteView physical, uint32_t protect_size, ByteView logical) {
  logical.fill(0xFF);
  if (protect_size > physical.size()) return false;
  const uint64_t limit = physical.size() - protect_size;
  uint64_t phy = 0;
  while (phy + 1 < limit) {
    const uint8_t hdr1 = physical[phy];
    const uint8_t hdr2 = physical[phy + 1];
    if (hdr1 == 0xFF || hdr2 == 0xFF) break;
    const uint32_t block = static_cast<uint32_t>((hdr1 & 0x0F) << 4 | (hdr2 & 0xF0) >> 4);
    phy += 2;
    if (!replay_words(physical, limit, block, hdr2 & 0x0F, &phy, logical)) return false;
  }
  return true;
}

bool rtw_efuse_mac_address(ConstByteView logical, uint32_t offset, MacAddress* out) {
  ConstByteView bytes;
  if (!logical.try_subview(offset, 6, &bytes)) return false;
  bool all_ff = true, all_zero = true;
  for (uint32_t i = 0; i < 6; ++i) {
    out->bytes[i] = bytes[i];
    all_ff = all_ff && bytes[i] == 0xFF;
    all_zero = all_zero && bytes[i] == 0;
  }
  return !all_ff && !all_zero && (bytes[0] & 1) == 0;
}

bool rtw88_read_physical_efuse(INicRegisters& registers, ByteView physical) {
  constexpr uint32_t kFlag = 1u << 31, kAddressMask = 0x3FF, kAddressShift = 8;
  constexpr uint32_t kBankSelect = 3u << 8;            // BIT_MASK_EFUSE_BANK_SEL in REG_LDO_EFUSE_CTRL: bank 0 is the Wi-Fi bank
  constexpr uint8_t kLdo25Enable = 1u << 7;            // BIT_LDO25_EN in REG_LDO_EFUSE_CTRL + 3
  if (physical.size() > kAddressMask + 1) return false;
  registers.write8(kRtwRegEfuseAccess, kRtwEfuseAccessOn);     // rtw_chip_efuse_grant_on
  registers.write32(kRtwRegLdoEfuseCtrl, registers.read32(kRtwRegLdoEfuseCtrl) & ~kBankSelect);
  registers.write8(kRtwRegLdoEfuseCtrl + 3, static_cast<uint8_t>(registers.read8(kRtwRegLdoEfuseCtrl + 3) & ~kLdo25Enable));   // cfg_ldo25(false)
  uint32_t control = registers.read32(kRtwRegEfuseCtrl);
  bool ok = true;
  for (uint64_t address = 0; address < physical.size() && ok; ++address) {
    control &= ~(0xFFu | kAddressMask << kAddressShift);
    control |= (static_cast<uint32_t>(address) & kAddressMask) << kAddressShift;
    registers.write32(kRtwRegEfuseCtrl, control & ~kFlag);
    ok = false;
    for (uint32_t tries = 0; tries < 10000; ++tries) {
      delay_us(1);
      control = registers.read32(kRtwRegEfuseCtrl);
      if (control & kFlag) { ok = true; break; }
    }
    if (ok) physical[address] = static_cast<uint8_t>(control & 0xFF);
  }
  registers.write8(kRtwRegEfuseAccess, kRtwEfuseAccessOff);
  return ok;
}

}  // namespace hardware
