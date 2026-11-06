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

#include "Hardware/Chipset/Smbus/Spd.hpp"

namespace hardware {

namespace {
constexpr uint8_t kTypeDdr3 = 0x0B, kTypeDdr4 = 0x0C, kTypeDdr5 = 0x12, kTypeLpddr5 = 0x13, kTypeLpddr5x = 0x15;

// Reads byte `index` of the image; false (and 0) when the image does not reach it.
bool byte_at(ConstByteView spd, uint64_t index, uint8_t* out) {
  *out = 0;
  return spd.try_get(index, out);
}

MemoryModuleForm form_of(uint8_t key, bool ddr3) {
  switch (key & 0x0F) {
    case 0x1: return MemoryModuleForm::Rdimm;
    case 0x2: return MemoryModuleForm::Udimm;
    case 0x3: return MemoryModuleForm::SoDimm;
    case 0x4: return ddr3 ? MemoryModuleForm::Other : MemoryModuleForm::Lrdimm;   // DDR3: Micro-DIMM
    case 0xB: return ddr3 ? MemoryModuleForm::Lrdimm : MemoryModuleForm::Other;
    case 0x0: return MemoryModuleForm::Unknown;
    default: return MemoryModuleForm::Other;
  }
}

// The data rate for a clock period in femtoseconds, snapped to the JEDEC speed bin it rounds to (DDR4-2666 has a 750 ps clock: 2666.7 MT/s).
uint32_t rate_of(uint64_t tck_fs) {
  if (tck_fs < 100000 || tck_fs > 10000000) return 0;   // outside 0.1..10 ns: not a real clock
  const uint32_t mts = static_cast<uint32_t>((2000000000ull + tck_fs / 2) / tck_fs);
  constexpr uint32_t kBins[] = {800,  1066, 1333, 1600, 1866, 2133, 2400, 2666, 2933, 3200, 3600, 4000,
                                4400, 4800, 5200, 5600, 6000, 6400, 6800, 7200, 7600, 8000, 8400, 8800};
  for (uint32_t bin : kBins) {
    const uint32_t difference = mts > bin ? mts - bin : bin - mts;
    if (difference * 200 <= bin) return bin;
  }
  return mts;
}

uint16_t manufacturer_at(ConstByteView spd, uint64_t index) {
  uint8_t continuation = 0, code = 0;
  if (!byte_at(spd, index, &continuation) || !byte_at(spd, index + 1, &code)) return 0;
  return static_cast<uint16_t>((continuation << 8) | code);
}

void decode_ddr3(ConstByteView spd, SpdModule* m) {
  uint8_t key, density, organization, bus, ftb, mtb_dividend, mtb_divisor, tck, fine;
  byte_at(spd, 3, &key);
  m->form = form_of(key, true);
  if (byte_at(spd, 4, &density) && byte_at(spd, 7, &organization) && byte_at(spd, 8, &bus) && (density & 0x0F) <= 6 && (organization & 7) <= 3 &&
      (bus & 7) <= 3) {
    const uint64_t die_bytes = (uint64_t{256} << (density & 0x0F)) * 1024 * 1024 / 8;
    m->device_width = static_cast<uint8_t>(4u << (organization & 7));
    m->ranks = static_cast<uint8_t>(((organization >> 3) & 7) + 1);
    m->ecc = ((bus >> 3) & 3) == 1;
    const uint32_t bus_width = 8u << (bus & 7);
    m->capacity_bytes = die_bytes * (bus_width / m->device_width) * m->ranks;
  }
  // tCKmin in medium timebase units (MTB = dividend / divisor ns) plus a signed fine correction (FTB = upper / lower nibble ps).
  if (byte_at(spd, 9, &ftb) && byte_at(spd, 10, &mtb_dividend) && byte_at(spd, 11, &mtb_divisor) && byte_at(spd, 12, &tck) && mtb_divisor != 0) {
    byte_at(spd, 34, &fine);
    const int64_t mtb_fs = int64_t{mtb_dividend} * 1000000 / mtb_divisor;
    const int64_t ftb_fs = (ftb & 0x0F) ? int64_t{ftb >> 4} * 1000 / (ftb & 0x0F) : 0;
    const int64_t tck_fs = int64_t{tck} * mtb_fs + int64_t{static_cast<int8_t>(fine)} * ftb_fs;
    if (tck_fs > 0) m->speed_mts = rate_of(static_cast<uint64_t>(tck_fs));
  }
  m->manufacturer = manufacturer_at(spd, 117);
}

void decode_ddr4(ConstByteView spd, SpdModule* m) {
  constexpr uint32_t kDensityMbit[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 12288, 24576};
  uint8_t key, density, package, organization, bus, timebases, tck, fine;
  byte_at(spd, 3, &key);
  m->form = form_of(key, false);
  if (byte_at(spd, 4, &density) && byte_at(spd, 6, &package) && byte_at(spd, 12, &organization) && byte_at(spd, 13, &bus) &&
      (density & 0x0F) < sizeof(kDensityMbit) / sizeof(kDensityMbit[0]) && (organization & 7) <= 3 && (bus & 7) <= 3) {
    const uint64_t die_bytes = uint64_t{kDensityMbit[density & 0x0F]} * 1024 * 1024 / 8;
    m->device_width = static_cast<uint8_t>(4u << (organization & 7));
    const uint32_t package_ranks = ((organization >> 3) & 7) + 1;
    const uint32_t dies = ((package >> 4) & 7) + 1;
    const bool stacked_3ds = (package & 0x80) && (package & 3) == 2;   // a 3DS stack: every die is a logical rank
    m->ranks = static_cast<uint8_t>(stacked_3ds ? package_ranks * dies : package_ranks);
    m->ecc = ((bus >> 3) & 3) == 1;
    const uint32_t bus_width = 8u << (bus & 7);
    m->capacity_bytes = die_bytes * (bus_width / m->device_width) * m->ranks;
  }
  // Both timebases are fixed by the standard (MTB 125 ps, FTB 1 ps); any other encoding is not a valid DDR4 SPD.
  if (byte_at(spd, 17, &timebases) && timebases == 0 && byte_at(spd, 18, &tck) && tck != 0) {
    byte_at(spd, 125, &fine);
    const int64_t tck_ps = int64_t{tck} * 125 + static_cast<int8_t>(fine);
    if (tck_ps > 0) m->speed_mts = rate_of(static_cast<uint64_t>(tck_ps) * 1000);
  }
  m->manufacturer = manufacturer_at(spd, 320);
}

void decode_ddr5(ConstByteView spd, SpdModule* m) {
  constexpr uint32_t kDensityMbit[] = {0, 4096, 8192, 12288, 16384, 24576, 32768, 49152, 65536};
  constexpr uint32_t kDies[] = {1, 0, 2, 4, 8, 16};   // encoding 1 is reserved
  uint8_t key, density, width, organization, bus, tck_low, tck_high;
  byte_at(spd, 3, &key);
  m->form = form_of(key, false);
  if (byte_at(spd, 4, &density) && byte_at(spd, 6, &width) && (density & 0x1F) < sizeof(kDensityMbit) / sizeof(kDensityMbit[0]) &&
      kDensityMbit[density & 0x1F] != 0 && (density >> 5) < sizeof(kDies) / sizeof(kDies[0]) && kDies[density >> 5] != 0 && (width >> 5) <= 3) {
    m->device_width = static_cast<uint8_t>(4u << (width >> 5));
    // The organisation of the module (ranks, bus width, sub-channels) is in the module-specific part of the SPD, page 1.
    if (byte_at(spd, 234, &organization) && byte_at(spd, 235, &bus) && (bus & 7) <= 3) {
      const uint64_t die_bytes = uint64_t{kDensityMbit[density & 0x1F]} * 1024 * 1024 / 8;
      m->ranks = static_cast<uint8_t>(((organization >> 3) & 7) + 1);
      m->ecc = ((bus >> 3) & 3) != 0;
      const uint32_t channels = ((bus >> 5) & 3) + 1;
      const uint32_t bus_width = 8u << (bus & 7);
      if (bus_width >= m->device_width) m->capacity_bytes = channels * (bus_width / m->device_width) * kDies[density >> 5] * die_bytes * m->ranks;
    }
  }
  if (byte_at(spd, 20, &tck_low) && byte_at(spd, 21, &tck_high)) {
    const uint32_t tck_ps = tck_low | (tck_high << 8);
    if (tck_ps) m->speed_mts = rate_of(uint64_t{tck_ps} * 1000);
  }
  m->manufacturer = manufacturer_at(spd, 512);
}

// SMBus side.
constexpr uint8_t kSpdAddress = 0x50, kEe1004SetPage0 = 0x36;
constexpr uint8_t kHubMr0 = 0x00, kHubMr1 = 0x01, kHubLegacyMode = 0x0B, kHubNvmWindow = 0x80;
constexpr uint8_t kHubLegacyPageMask = 0x07, kHubTwoByteAddressing = 0x08;

void store(SpdImage* image, uint16_t offset, uint8_t value) {
  if (offset >= SpdImage::kCapacity) return;
  image->bytes[offset] = value;
  if (offset >= image->size) image->size = static_cast<uint16_t>(offset + 1);
}

// Reads `count` bytes with byte-data commands first..first+count-1, stored from image offset `base` + first.
SmbusStatus read_range(ISmbusController& smbus, uint8_t address, uint8_t first, uint8_t count, uint16_t base, SpdImage* image) {
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t value = 0;
    const SmbusStatus status = smbus.read_byte_data(address, static_cast<uint8_t>(first + i), &value);
    if (status != SmbusStatus::Ok) return status;
    store(image, static_cast<uint16_t>(base + first + i), value);
  }
  return SmbusStatus::Ok;
}

// EE1004 (DDR4): the two 256-byte pages are chosen for every module on the bus at once by addressing 0x36 (page 0) or 0x37 (page 1). A read from 0x36
// is acknowledged only while page 0 is selected (Linux's ee1004 does the same).
SmbusStatus ee1004_page(ISmbusController& smbus, uint8_t* page) {
  uint8_t ignored = 0;
  const SmbusStatus status = smbus.read_byte(kEe1004SetPage0, &ignored);
  if (status == SmbusStatus::Ok) *page = 0;
  else if (status == SmbusStatus::NoDevice) *page = 1;
  else return status;
  return SmbusStatus::Ok;
}

bool ee1004_select(ISmbusController& smbus, uint8_t page) {
  // Some EEPROMs do not acknowledge the data byte of the set-page command, so the write's own result says little: ask which page is selected.
  (void)smbus.write_byte_data(static_cast<uint8_t>(kEe1004SetPage0 + page), 0, 0);
  uint8_t now = 0xFF;
  return ee1004_page(smbus, &now) == SmbusStatus::Ok && now == page;
}

SmbusStatus read_ddr4(ISmbusController& smbus, uint8_t address, uint8_t original_page, SpdImage* image) {
  SmbusStatus status = read_range(smbus, address, 0, 19, 0, image);
  if (status == SmbusStatus::Ok) status = read_range(smbus, address, 125, 1, 0, image);
  if (status == SmbusStatus::Ok && ee1004_select(smbus, 1)) {
    (void)read_range(smbus, address, 320 - 256, 2, 256, image);   // the module maker; best effort
    ee1004_select(smbus, 0);
  }
  if (original_page != 0) ee1004_select(smbus, original_page);
  return status;
}

// SPD5118 hub (DDR5): registers MR0..MR127 at 0x00..0x7F, and a 128-byte window at 0x80..0xFF onto the page of the 1 KiB EEPROM that MR11 selects.
class HubReader {
 public:
  HubReader(ISmbusController& smbus, uint8_t address, uint8_t legacy_mode) : smbus_(smbus), address_(address), mode_(legacy_mode) {}
  ~HubReader() {
    if (page_ != original()) select(original());
  }

  SmbusStatus read(uint16_t offset, uint8_t count, SpdImage* image) {
    for (uint16_t i = 0; i < count; ++i) {
      const uint16_t at = static_cast<uint16_t>(offset + i);
      if (!select(static_cast<uint8_t>(at / 128))) return SmbusStatus::Failed;
      uint8_t value = 0;
      const SmbusStatus status = smbus_.read_byte_data(address_, static_cast<uint8_t>(kHubNvmWindow | (at & 0x7F)), &value);
      if (status != SmbusStatus::Ok) return status;
      store(image, at, value);
    }
    return SmbusStatus::Ok;
  }

 private:
  uint8_t original() const { return mode_ & kHubLegacyPageMask; }
  bool select(uint8_t page) {
    if (page == page_) return true;
    if (smbus_.write_byte_data(address_, kHubLegacyMode, static_cast<uint8_t>((mode_ & ~kHubLegacyPageMask) | page)) != SmbusStatus::Ok) return false;
    page_ = page;
    return true;
  }

  ISmbusController& smbus_;
  uint8_t address_;
  uint8_t mode_;
  uint8_t page_ = original();
};
}  // namespace

bool spd_decode(ConstByteView spd, SpdModule* out) {
  *out = SpdModule();
  uint8_t type = 0;
  if (!byte_at(spd, 2, &type)) return false;
  switch (type) {
    case kTypeDdr3: out->type = MemoryType::Ddr3; decode_ddr3(spd, out); return true;
    case kTypeDdr4: out->type = MemoryType::Ddr4; decode_ddr4(spd, out); return true;
    case kTypeDdr5: out->type = MemoryType::Ddr5; decode_ddr5(spd, out); return true;
    case kTypeLpddr5:
    case kTypeLpddr5x: out->type = MemoryType::Lpddr5; return true;   // soldered memory: the SPD (if any) only names the type here
    default: return false;
  }
}

SmbusStatus spd_read(ISmbusController& smbus, uint8_t slot, SpdImage* image) {
  *image = SpdImage();
  if (slot > 7) return SmbusStatus::BadArgument;
  const uint8_t address = static_cast<uint8_t>(kSpdAddress + slot);

  uint8_t mr0 = 0, mr1 = 0;
  SmbusStatus status = smbus.read_byte_data(address, kHubMr0, &mr0);
  if (status != SmbusStatus::Ok) return status;
  status = smbus.read_byte_data(address, kHubMr1, &mr1);
  if (status != SmbusStatus::Ok) return status;

  if (mr0 == 0x51 && mr1 == 0x18) {   // an SPD5118 hub's device type
    uint8_t mode = 0;
    status = smbus.read_byte_data(address, kHubLegacyMode, &mode);
    if (status != SmbusStatus::Ok) return status;
    if (mode & kHubTwoByteAddressing) return SmbusStatus::Failed;   // left in I3C-style 2-byte addressing: byte-data reads cannot reach the EEPROM
    HubReader hub(smbus, address, mode);
    status = hub.read(0, 22, image);
    if (status != SmbusStatus::Ok) return status;
    (void)hub.read(234, 2, image);   // organisation (page 1) and maker (page 4): best effort, a write-protected hub keeps its page
    (void)hub.read(512, 2, image);
    return SmbusStatus::Ok;
  }

  // An EEPROM: byte 2 is the memory type. A DDR4 module may have been left on page 1 (bytes 256..511), where byte 2 is not the type.
  uint8_t type = 0;
  status = smbus.read_byte_data(address, 2, &type);
  if (status != SmbusStatus::Ok) return status;
  uint8_t original_page = 0;
  if (type != kTypeDdr3 && type != kTypeDdr4) {
    if (ee1004_page(smbus, &original_page) == SmbusStatus::Ok && original_page == 1 && ee1004_select(smbus, 0)) {
      status = smbus.read_byte_data(address, 2, &type);
      if (status != SmbusStatus::Ok) {
        ee1004_select(smbus, 1);
        return status;
      }
    } else {
      original_page = 0;
    }
  }
  if (type == kTypeDdr4) return read_ddr4(smbus, address, original_page, image);
  if (original_page != 0) ee1004_select(smbus, original_page);
  status = read_range(smbus, address, 0, 15, 0, image);
  if (status == SmbusStatus::Ok && type == kTypeDdr3) {
    status = read_range(smbus, address, 34, 1, 0, image);
    if (status == SmbusStatus::Ok) status = read_range(smbus, address, 117, 2, 0, image);
  }
  return status;
}

const char* memory_type_name(MemoryType type) {
  switch (type) {
    case MemoryType::Ddr3: return "DDR3";
    case MemoryType::Ddr4: return "DDR4";
    case MemoryType::Ddr5: return "DDR5";
    case MemoryType::Lpddr5: return "LPDDR5";
    case MemoryType::Unknown: break;
  }
  return "unknown memory";
}

const char* memory_module_form_name(MemoryModuleForm form) {
  switch (form) {
    case MemoryModuleForm::Rdimm: return "RDIMM";
    case MemoryModuleForm::Udimm: return "UDIMM";
    case MemoryModuleForm::SoDimm: return "SO-DIMM";
    case MemoryModuleForm::Lrdimm: return "LRDIMM";
    case MemoryModuleForm::Other: return "module";
    case MemoryModuleForm::Unknown: break;
  }
  return "module";
}

const char* jedec_manufacturer_name(uint16_t code) {
  struct Maker {
    uint16_t code;
    const char* name;
  };
  // Bank (continuation count) in the high byte, the code in the low byte; the parity bits (bit 7 of each) are ignored when comparing.
  constexpr Maker kMakers[] = {
      {0x00CE, "Samsung"}, {0x00AD, "SK hynix"}, {0x002C, "Micron"}, {0x0198, "Kingston"}, {0x029E, "Corsair"}, {0x030B, "Nanya"},
      {0x04CD, "G.Skill"}, {0x04CB, "ADATA"},    {0x04EF, "Team Group"}, {0x059B, "Crucial"},
  };
  const uint16_t key = code & 0x7F7F;
  for (const Maker& maker : kMakers)
    if ((maker.code & 0x7F7F) == key) return maker.name;
  return nullptr;
}

void spd_describe(const SpdModule& module, FixedString<128>& out) {
  out.clear();
  out.append(memory_type_name(module.type));
  out.push_back(' ');
  out.append(memory_module_form_name(module.form));
  if (module.capacity_bytes) {
    constexpr uint64_t kGiB = uint64_t{1} << 30, kMiB = uint64_t{1} << 20;
    out.append(", ");
    if (module.capacity_bytes % kGiB == 0) {
      out.append_uint(module.capacity_bytes / kGiB);
      out.append(" GiB");
    } else {
      out.append_uint(module.capacity_bytes / kMiB);
      out.append(" MiB");
    }
  }
  if (module.speed_mts) {
    out.append(", ");
    out.append_uint(module.speed_mts);
    out.append(" MT/s");
  }
  if (module.ranks && module.device_width) {
    out.append(", ");
    out.append_uint(module.ranks);
    out.append(module.ranks == 1 ? " rank of x" : " ranks of x");
    out.append_uint(module.device_width);
  }
  if (module.ecc) out.append(", ECC");
  if (module.manufacturer) {
    out.append(", ");
    const char* maker = jedec_manufacturer_name(module.manufacturer);
    if (maker) {
      out.append(maker);
    } else {
      out.append("JEDEC maker 0x");
      constexpr char kHex[] = "0123456789ABCDEF";
      const uint16_t code = module.manufacturer & 0x7F7F;
      for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(code >> shift) & 0xF]);
    }
  }
}

}  // namespace hardware
