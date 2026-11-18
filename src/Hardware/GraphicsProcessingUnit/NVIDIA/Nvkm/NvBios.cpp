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

// Follows nouveau (MIT license): nvkm/subdev/bios/base.c (finding BIT and BMP, the version), bit.c (bit_entry), dcb.c (dcb_table,
// dcb_outp_parse, dcb_outp_foreach), i2c.c (dcb_i2c_table, dcb_i2c_parse), gpio.c (dcb_gpio_table, dcb_gpio_parse), conn.c (nvbios_connTe,
// nvbios_connEp) and nvkm/engine/disp/conn.c (the HPD line to GPIO function map). Layouts nouveau's code does not pin down are listed in
// kUncertain at the end, and docs/Nvidia.md repeats them.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"

namespace hardware {
namespace nv {

namespace {

// nvbios_findstr: the first offset of `needle` in `haystack`, 0 if it is not there (0 is never a valid place for these structures: the ROM
// signature is there).
uint32_t find_string(ConstByteView haystack, ConstByteView needle) {
  if (needle.size() == 0 || haystack.size() < needle.size() || haystack.size() > 0xFFFFFFFFull) return 0;
  for (uint64_t i = 0; i + needle.size() <= haystack.size(); ++i)
    if (haystack.subview(i, needle.size()).equals(needle)) return static_cast<uint32_t>(i);
  return 0;
}

// Counts how many entries of a table at `start` with `count` entries of `length` bytes lie wholly inside the image.
uint32_t entries_inside(ConstByteView data, uint64_t start, uint32_t count, uint32_t length) {
  if (length == 0 || start >= data.size()) return 0;
  const uint64_t room = (data.size() - start) / length;
  return room < count ? static_cast<uint32_t>(room) : count;
}

}  // namespace

NvBios::NvBios(ConstByteView image) : data_(image) {
  static constexpr uint8_t kBit[5] = {0xff, 0xb8, 'B', 'I', 'T'};
  static constexpr uint8_t kBmp[5] = {0xff, 0x7f, 'N', 'V', 0x00};
  bit_offset_ = find_string(data_, ConstByteView(kBit, sizeof(kBit)));
  bmp_offset_ = find_string(data_, ConstByteView(kBmp, sizeof(kBmp)));
  if (bmp_offset_) {
    uint8_t major = 0, minor = 0;
    if (rd08(uint64_t{bmp_offset_} + 5, &major) && rd08(uint64_t{bmp_offset_} + 6, &minor)) bmp_version_ = static_cast<uint16_t>(major << 8 | minor);
  }
}

bool NvBios::rd08(uint64_t offset, uint8_t* out) const { return data_.try_get(offset, out); }
bool NvBios::rd16(uint64_t offset, uint16_t* out) const {
  uint8_t a = 0, b = 0;
  if (offset > UINT64_MAX - 1 || !rd08(offset, &a) || !rd08(offset + 1, &b)) return false;
  *out = static_cast<uint16_t>(a | (b << 8));
  return true;
}
bool NvBios::rd32(uint64_t offset, uint32_t* out) const {
  uint16_t lo = 0, hi = 0;
  if (offset > UINT64_MAX - 3 || !rd16(offset, &lo) || !rd16(offset + 2, &hi)) return false;
  *out = lo | (uint32_t{hi} << 16);
  return true;
}

bool NvBios::bit_entry(uint8_t id, BitEntry* out) const {
  if (!bit_offset_) return false;
  // The BIT header: ff b8 'B' 'I' 'T' 00, u16 version, u8 header size, u8 entry size, u8 entry count, u8 checksum; entries from +12
  // (nouveau uses 12 whatever the header size says).
  uint8_t entry_size = 0, entries = 0;
  if (!rd08(uint64_t{bit_offset_} + 9, &entry_size) || !rd08(uint64_t{bit_offset_} + 10, &entries) || entry_size < 6) return false;
  uint64_t entry = uint64_t{bit_offset_} + 12;
  for (uint32_t i = 0; i < entries; ++i, entry += entry_size) {
    BitEntry e;
    if (!rd08(entry, &e.id) || !rd08(entry + 1, &e.version) || !rd16(entry + 2, &e.length) || !rd16(entry + 4, &e.offset)) return false;
    if (e.id == id) {
      *out = e;
      return true;
    }
  }
  return false;
}

namespace {

struct DcbHeader {
  uint32_t at = 0;
  uint8_t version = 0, header = 0, count = 0, length = 0;
};

// dcb_table
const char* dcb_table(const NvBios& bios, const Chip& chip, DcbHeader* out) {
  uint16_t dcb = 0;
  if (chip.card_type != 0x04 && !bios.rd16(0x36, &dcb)) return "the image is too short for the DCB pointer";
  if (!dcb) return "no DCB table";
  DcbHeader h;
  h.at = dcb;
  if (!bios.rd08(dcb, &h.version)) return "the DCB pointer is outside the image";
  uint32_t signature = 0;
  if (h.version >= 0x42) return "unknown DCB version";
  if (h.version >= 0x30) {
    if (!bios.rd32(uint64_t{dcb} + 6, &signature) || signature != NvBios::kDcbSignature) return "DCB header validation failed";
    if (!bios.rd08(uint64_t{dcb} + 1, &h.header) || !bios.rd08(uint64_t{dcb} + 2, &h.count) || !bios.rd08(uint64_t{dcb} + 3, &h.length))
      return "the DCB header is cut short";
  } else if (h.version >= 0x20) {
    uint16_t i2c = 0;
    if (!bios.rd32(uint64_t{dcb} + 4, &signature) || signature != NvBios::kDcbSignature) return "DCB header validation failed";
    if (!bios.rd16(uint64_t{dcb} + 2, &i2c)) return "the DCB header is cut short";
    h.header = 8;
    h.length = 8;
    // The entries run up to the I2C table (nouveau: (i2c - dcb) / 8, in a u8).
    const uint32_t span = i2c > dcb ? (i2c - dcb) / 8u : 0u;
    h.count = static_cast<uint8_t>(span > 0xff ? 0xff : span);
  } else if (h.version >= 0x15) {
    static constexpr uint8_t kDevRec[7] = {'D', 'E', 'V', '_', 'R', 'E', 'C'};
    ConstByteView tag;
    if (dcb < 7 || !bios.data().try_subview(dcb - 7u, 7, &tag) || !tag.equals(ConstByteView(kDevRec, 7))) return "DCB header validation failed";
    uint16_t i2c = 0;
    if (!bios.rd16(uint64_t{dcb} + 2, &i2c)) return "the DCB header is cut short";
    h.header = 4;
    h.length = 10;
    const uint32_t span = i2c > dcb ? (i2c - dcb) / 10u : 0u;
    h.count = static_cast<uint8_t>(span > 0xff ? 0xff : span);
  } else {
    return "the DCB (before 1.5) has no useful data";
  }
  *out = h;
  return nullptr;
}

void parse_outputs(const NvBios& bios, const DcbHeader& dcb, BoardInfo* board) {
  if (dcb.version < 0x20) return;   // (DCB 1.5 entries: nouveau's dcb_outp_parse does not read them)
  if (dcb.length < 4) {
    board->truncated = true;
    return;
  }
  const uint64_t first = uint64_t{dcb.at} + dcb.header;
  const uint32_t inside = entries_inside(bios.data(), first, dcb.count, dcb.length);
  if (inside < dcb.count) board->truncated = true;
  for (uint32_t i = 0; i < inside; ++i) {
    const uint64_t entry = first + uint64_t{i} * dcb.length;
    uint32_t conn = 0;
    if (!bios.rd32(entry, &conn)) break;
    if (conn == 0x00000000 || conn == 0xffffffff) break;   // seen on an NV11 with DCB 1.5 / an NV17 with DCB 2.0
    const uint8_t type = conn & 0x0f;
    if (type == static_cast<uint8_t>(DcbOutputType::Eol)) break;
    if (type == static_cast<uint8_t>(DcbOutputType::Unused)) continue;
    if (board->output_count >= BoardInfo::kMaxOutputs) {
      board->truncated = true;
      break;
    }
    DcbOutput& o = board->outputs[board->output_count++];
    o = DcbOutput{};
    o.raw[0] = conn;
    o.type = static_cast<DcbOutputType>(type);
    o.or_mask = (conn & 0x0f000000) >> 24;
    o.location = (conn & 0x00300000) >> 20;
    o.bus = (conn & 0x000f0000) >> 16;
    o.connector = (conn & 0x0000f000) >> 12;
    o.heads = (conn & 0x00000f00) >> 8;
    o.i2c_index = (conn & 0x000000f0) >> 4;
    uint32_t conf = 0;
    const bool has_conf = dcb.length >= 8 && bios.rd32(entry + 4, &conf);
    if (has_conf) o.raw[1] = conf;
    uint8_t link = 0;
    if (dcb.version >= 0x40 && has_conf) {
      switch (o.type) {
        case DcbOutputType::Dp:
          switch (conf & 0x00e00000) {
            case 0x00000000: o.dp_max_link_rate = 0x06; break;   // 1.62 Gb/s
            case 0x00200000: o.dp_max_link_rate = 0x0a; break;   // 2.7
            case 0x00400000: o.dp_max_link_rate = 0x14; break;   // 5.4
            default: o.dp_max_link_rate = 0x1e; break;           // 8.1 (and what nouveau does not know)
          }
          switch ((conf & 0x0f000000) >> 24) {
            case 0xf:
            case 0x4: o.dp_max_lanes = 4; break;
            case 0x3:
            case 0x2: o.dp_max_lanes = 2; break;
            default: o.dp_max_lanes = 1; break;
          }
          [[fallthrough]];
        case DcbOutputType::Tmds:
        case DcbOutputType::Lvds:
          link = (conf & 0x00000030) >> 4;
          o.sorconf_link = link;
          o.extdev = o.location != 0 ? static_cast<uint8_t>((conf & 0x0000ff00) >> 8) : 0;
          break;
        default: break;
      }
    }
    o.hasht = static_cast<uint16_t>(o.extdev << 8 | o.location << 4 | type);
    o.hashm = static_cast<uint16_t>(o.heads << 8 | link << 6 | o.or_mask);
  }
}

// The connector table's HPD lines -> GPIO functions (nvkm/engine/disp/conn.c).
constexpr uint8_t kHpdFunctions[] = {0x07, 0x08, 0x51, 0x52, 0x5e, 0x5f, 0x60};

void parse_connectors(const NvBios& bios, const DcbHeader& dcb, BoardInfo* board) {
  if (dcb.version < 0x30 || dcb.header < 0x16) return;
  uint16_t table = 0;
  if (!bios.rd16(uint64_t{dcb.at} + 0x14, &table)) {
    board->truncated = true;
    return;
  }
  if (!table) return;
  uint8_t version = 0, header = 0, count = 0, length = 0;
  if (!bios.rd08(table, &version) || !bios.rd08(uint64_t{table} + 1, &header) || !bios.rd08(uint64_t{table} + 2, &count) ||
      !bios.rd08(uint64_t{table} + 3, &length)) {
    board->truncated = true;
    return;
  }
  if ((version != 0x30 && version != 0x40) || length < 2) return;
  const uint64_t first = uint64_t{table} + header;
  const uint32_t inside = entries_inside(bios.data(), first, count, length);
  if (inside < count) board->truncated = true;
  for (uint32_t i = 0; i < inside; ++i) {
    if (board->connector_count >= BoardInfo::kMaxConnectors) {
      board->truncated = true;
      break;
    }
    const uint64_t entry = first + uint64_t{i} * length;
    ConnectorEntry& c = board->connectors[board->connector_count++];
    c = ConnectorEntry{};
    c.index = static_cast<uint8_t>(i);
    uint8_t b[4] = {};
    const uint32_t n = length < 4 ? length : 4;
    for (uint32_t k = 0; k < n; ++k) (void)bios.rd08(entry + k, &b[k]);   // (inside the image: entries_inside)
    for (uint32_t k = 0; k < 4; ++k) c.raw[k] = b[k];
    c.type = b[0];
    c.location = b[1] & 0x0f;
    uint32_t hpd = (b[1] & 0x30) >> 4;
    uint32_t dp = (b[1] & 0xc0) >> 6;
    if (length >= 4) {
      hpd |= (b[2] & 0x03) << 2;
      dp |= b[2] & 0x0c;
      hpd |= (b[3] & 0x07) << 4;
    }
    c.hpd_mask = static_cast<uint8_t>(hpd);
    c.dp_mask = static_cast<uint8_t>(dp);
    c.hpd = 0xff;
    if (hpd) {
      const uint32_t line = static_cast<uint32_t>(__builtin_ctz(hpd));
      if (line < sizeof(kHpdFunctions)) c.hpd = kHpdFunctions[line];
    }
  }
}

void parse_i2c(const NvBios& bios, const DcbHeader* dcb, BoardInfo* board) {
  uint16_t table = 0;
  uint8_t version = 0, header = 0, count = 0, length = 0;
  if (dcb) {
    if (dcb->version >= 0x30) {
      if (!bios.rd16(uint64_t{dcb->at} + 4, &table)) board->truncated = true;
    } else if (dcb->version >= 0x15) {
      if (!bios.rd16(uint64_t{dcb->at} + 2, &table)) board->truncated = true;
    }
    version = dcb->version;
  }
  if (table && dcb && dcb->version >= 0x30) {
    if (!bios.rd08(table, &version) || !bios.rd08(uint64_t{table} + 1, &header) || !bios.rd08(uint64_t{table} + 2, &count) ||
        !bios.rd08(uint64_t{table} + 3, &length)) {
      board->truncated = true;
      return;
    }
    // UNSURE (see kUncertain): byte 4 of a 3.x/4.x I2C header with room for it holds the default ports, primary in the low nibble.
    uint8_t flags = 0;
    if (header >= 5 && bios.rd08(uint64_t{table} + 4, &flags)) {
      board->i2c_default[0] = flags & 0x0f;
      board->i2c_default[1] = flags >> 4;
    }
  } else if (table) {
    header = 0;
    count = 16;
    length = 4;
  }

  if (table) {
    if (length < 4) return;   // (nouveau reads four bytes of every entry)
    const uint64_t first = uint64_t{table} + header;
    const uint32_t inside = entries_inside(bios.data(), first, count, length);
    if (inside < count) board->truncated = true;
    for (uint32_t i = 0; i < inside; ++i) {
      if (board->i2c_count >= BoardInfo::kMaxI2c) {
        board->truncated = true;
        break;
      }
      const uint64_t entry = first + uint64_t{i} * length;
      uint8_t b[4] = {};
      for (uint32_t k = 0; k < 4; ++k) (void)bios.rd08(entry + k, &b[k]);
      I2cPort& p = board->i2c[board->i2c_count++];
      p = I2cPort{};
      for (uint32_t k = 0; k < 4; ++k) p.raw[k] = b[k];
      p.drive = p.sense = 0xff;
      p.share = p.aux = 0xff;
      uint8_t type = 0;
      if (version >= 0x41) {
        const uint32_t value = b[0] | (uint32_t{b[1]} << 8);
        type = ((value & 0x1f) == 0x1f && ((value >> 5) & 0x1f) == 0x1f) ? 0xff : 0x80;
      } else if (version >= 0x30) {
        type = b[3];
      } else {
        type = b[3] & 0x07;
        if (type == 0x07) type = 0xff;
      }
      switch (type) {
        case 0x00:   // DCB_I2C_NV04_BIT
          p.type = I2cPortType::Nv04Bit;
          p.drive = b[0];
          p.sense = b[1];
          break;
        case 0x04:   // DCB_I2C_NV4E_BIT
          p.type = I2cPortType::Nv4eBit;
          p.drive = b[1];
          break;
        case 0x05:   // DCB_I2C_NVIO_BIT
          p.type = I2cPortType::NvioBit;
          p.drive = b[0] & 0x0f;
          if (b[1] & 0x01) p.share = b[1] >> 1;
          break;
        case 0x06:   // DCB_I2C_NVIO_AUX
          p.type = I2cPortType::NvioAux;
          p.aux = b[0] & 0x0f;
          if (b[1] & 0x01) p.share = p.aux;
          break;
        case 0x80: {  // DCB_I2C_PMGR
          const uint32_t value = b[0] | (uint32_t{b[1]} << 8);
          p.type = I2cPortType::Pmgr;
          p.drive = (value & 0x01f) == 0x1f ? 0xff : static_cast<uint8_t>(value & 0x01f);
          p.aux = ((value & 0x3e0) >> 5) == 0x1f ? 0xff : static_cast<uint8_t>((value & 0x3e0) >> 5);
          p.share = p.aux;
          break;
        }
        default:     // DCB_I2C_UNUSED, and what nouveau warns about ("unknown i2c type") and treats as unused
          p.type = I2cPortType::Unused;
          break;
      }
    }
    return;
  }

  // No I2C table: the BMP's two fixed ports (in the BMP structure from BMP 4.0 on, at a fixed place of the image before).
  if (!bios.bmp_offset()) return;
  uint8_t bmp_major = 0;
  if (!bios.rd08(uint64_t{bios.bmp_offset()} + 5, &bmp_major)) return;
  const uint64_t at = bmp_major < 4 ? 0x0048 : 0x0036 + uint64_t{bios.bmp_offset()};
  static constexpr uint8_t kDefaults[2][2] = {{0x3f, 0x3e}, {0x37, 0x36}};
  for (uint32_t index = 0; index < 2; ++index) {
    uint8_t drive = 0, sense = 0;
    if (!bios.rd08(at + 4 + index * 2, &drive) || !bios.rd08(at + 5 + index * 2, &sense)) {
      board->truncated = true;
      return;
    }
    I2cPort& p = board->i2c[board->i2c_count++];
    p = I2cPort{};
    p.type = I2cPortType::Nv04Bit;
    p.drive = drive ? drive : kDefaults[index][0];
    p.sense = sense ? sense : kDefaults[index][1];
    p.share = p.aux = 0xff;
  }
}

void parse_gpio(const NvBios& bios, const DcbHeader& dcb, BoardInfo* board) {
  uint16_t table = 0;
  if (dcb.version >= 0x30 && dcb.header >= 0x0c) {
    if (!bios.rd16(uint64_t{dcb.at} + 0x0a, &table)) {
      board->truncated = true;
      return;
    }
  } else if (dcb.version >= 0x22 && dcb.at >= 0x0f) {
    uint8_t marker = 0;
    if (bios.rd08(dcb.at - 1u, &marker) && marker >= 0x13 && !bios.rd16(dcb.at - 0x0fu, &table)) table = 0;
  }
  if (!table) return;
  uint8_t version = 0, header = 0, count = 0, length = 0;
  if (!bios.rd08(table, &version)) {
    board->truncated = true;
    return;
  }
  bool ok = true;
  if (version < 0x30) {
    header = 3;
    ok = bios.rd08(uint64_t{table} + 2, &count) && bios.rd08(uint64_t{table} + 1, &length);
  } else if (version <= 0x41) {
    ok = bios.rd08(uint64_t{table} + 1, &header) && bios.rd08(uint64_t{table} + 2, &count) && bios.rd08(uint64_t{table} + 3, &length);
  } else {
    return;   // unknown version
  }
  if (!ok) {
    board->truncated = true;
    return;
  }
  const uint32_t need = version < 0x40 ? 2 : version < 0x41 ? 4 : 5;
  if (length < need) return;
  const uint64_t first = uint64_t{table} + header;
  const uint32_t inside = entries_inside(bios.data(), first, count, length);
  if (inside < count) board->truncated = true;
  for (uint32_t i = 0; i < inside; ++i) {
    if (board->gpio_count >= BoardInfo::kMaxGpio) {
      board->truncated = true;
      break;
    }
    const uint64_t entry = first + uint64_t{i} * length;
    GpioEntry& g = board->gpio[board->gpio_count++];
    g = GpioEntry{};
    if (version < 0x40) {
      uint16_t info = 0;
      (void)bios.rd16(entry, &info);
      g.line = info & 0x001f;
      g.function = static_cast<uint8_t>((info & 0x07e0) >> 5);
      g.log0 = static_cast<uint8_t>((info & 0x1800) >> 11);
      g.log1 = static_cast<uint8_t>((info & 0x6000) >> 13);
      g.param = (info & 0x8000) != 0;
    } else if (version < 0x41) {
      uint32_t info = 0;
      (void)bios.rd32(entry, &info);
      g.line = info & 0x1f;
      g.function = static_cast<uint8_t>((info & 0x0000ff00) >> 8);
      g.log0 = static_cast<uint8_t>((info & 0x18000000) >> 27);
      g.log1 = static_cast<uint8_t>((info & 0x60000000) >> 29);
      g.param = (info & 0x80000000) != 0;
    } else {
      uint32_t info = 0;
      uint8_t info1 = 0;
      (void)bios.rd32(entry, &info);
      (void)bios.rd08(entry + 4, &info1);
      g.line = info & 0x3f;
      g.function = static_cast<uint8_t>((info & 0x0000ff00) >> 8);
      g.log0 = (info1 & 0x18) >> 3;
      g.log1 = (info1 & 0x60) >> 5;
      g.param = (info1 & 0x80) != 0;
    }
  }
}

void parse_version(const NvBios& bios, BoardInfo* board) {
  BitEntry info;
  if (bios.bit_entry('i', &info) && info.length >= 4) {
    uint8_t v[5] = {};
    if (bios.rd08(uint64_t{info.offset} + 3, &v[0]) && bios.rd08(uint64_t{info.offset} + 2, &v[1]) &&
        bios.rd08(uint64_t{info.offset} + 1, &v[2]) && bios.rd08(info.offset, &v[3])) {
      if (info.length >= 5) (void)bios.rd08(uint64_t{info.offset} + 4, &v[4]);
      for (uint32_t i = 0; i < 5; ++i) board->bios_version[i] = v[i];
    } else {
      board->truncated = true;
    }
    return;
  }
  if (bios.bmp_offset()) {
    const uint64_t at = bios.bmp_offset();
    uint8_t v[4] = {};
    if (bios.rd08(at + 13, &v[0]) && bios.rd08(at + 12, &v[1]) && bios.rd08(at + 11, &v[2]) && bios.rd08(at + 10, &v[3])) {
      for (uint32_t i = 0; i < 4; ++i) board->bios_version[i] = v[i];
    }
  }
}

}  // namespace

BoardParseResult parse_board(const NvBios& bios, const Chip& chip, BoardInfo* board) {
  *board = BoardInfo{};
  BoardParseResult result;
  result.has_bit = bios.bit_offset() != 0;
  result.has_bmp = bios.bmp_offset() != 0;
  parse_version(bios, board);
  DcbHeader dcb;
  result.dcb_problem = dcb_table(bios, chip, &dcb);
  if (result.dcb_problem) {
    parse_i2c(bios, nullptr, board);   // (the BMP's fixed ports)
    return result;
  }
  result.has_dcb = true;
  board->dcb_version = dcb.version;
  parse_outputs(bios, dcb, board);
  parse_i2c(bios, &dcb, board);
  parse_gpio(bios, dcb, board);
  parse_connectors(bios, dcb, board);
  return result;
}

// UNCERTAIN: everything in NvChip.cpp, NvBiosShadow.cpp, NvBios.cpp and NvDevinit.cpp that was written from memory of nouveau / envytools /
// open-gpu-kernel-modules without a copy at hand and that nouveau's code does not fully pin down, kept in this one table (docs/Nvidia.md has
// the same list). Check these first against the sources (or a real board) when something does not match.
[[maybe_unused]] static constexpr const char* kUncertain[] = {
    // NvChip.cpp
    "BOOT_42 layout (architecture 29:24, implementation 23:20, revision major 19:16 / minor 15:12, as open-gpu-kernel-modules' dev_boot.h"
    " and nova read it); used only when BOOT_0 says Turing or later and BOOT_42 is neither 0 nor all ones",
    "GB102 = chipset 0x1a2 (and whether GB100/GB102 have a display engine: treated as not)",
    "GH100/GB10x/GB20x are in the table as nouveau's GSP-RM (r570) path knows them; the older nvkm switch has no case for 0x180/0x1a0/0x1b0",
    // NvBiosShadow.cpp
    "The PCI config mirror in BAR0 used for the ROM shadow bit: 0x001800 + 0x50 before NV40, 0x088000 + 0x50 from NV40 on (nv04_pci vs"
    " nv40_pci rd32/wr32), including Turing and later",
    "nvbios_shadow's skip values {0, 0, 0, 4, 1, 1} for PRAMIN, PROM, ACPI fast, ACPI slow, PCIROM, PLATFORM and its shadow_image scores"
    " (1 + 3 good checksum, 1 + 2 bad on an rw source, 1 + 1 bad on a read-only one, 0 when require_checksum)",
    "Which sources nouveau marks rw (PRAMIN, PCIROM, PLATFORM here) and require_checksum (ACPI fast only)",
    "PROM on Turing and later: the ROM may start with an IFR header nouveau does not skip; not handled",
    // NvBios.cpp
    "I2C table byte 4 (3.x/4.x header length >= 5): default DDC ports, primary in bits 3:0, secondary in 7:4 (from the old nouveau_bios.c"
    " i2c_default_indices; current nvkm does not read it)",
    "GPIO 4.1 entry: the second byte that holds log0/log1/param is read at +4 (nouveau: nvbios_rd32(bios, data + 4) truncated to u8)",
    "GpioEntry::output: not parsed (nouveau passes direction at drive time)",
    "DCB 2.x/3.x outputs: sorconf_link/extdev left 0 (nvkm reads the conf word only from DCB 4.0; old nouveau_bios.c derived dual link "
    "from the OR mask)",
    "DCB unused/EOL test on the type nibble (nouveau compares the whole low byte with 0x0f / 0x0e)",
    "Connector entries are kept in table order including unused (type 0xff) ones so DcbOutput::connector indexes them",
    "The BIT 'i' entry's patch byte (+4) is read only when the entry is at least 5 bytes long (nouveau reads it whenever length >= 4)",
    // NvDevinit.cpp
    "Turing+ devinit completion: 0x118128 bit 0 and 0x118234 bits 7:0 == 0xff (tu102_devinit_wait), read once here, not polled",
};

}  // namespace nv
}  // namespace hardware
