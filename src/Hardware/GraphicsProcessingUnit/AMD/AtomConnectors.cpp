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

#include "Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.hpp"

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): amdgpu_atombios.c (amdgpu_atombios_get_connector_info_from_object_table,
// amdgpu_atombios_lookup_gpio, amdgpu_atombios_get_hpd_info_from_gpio, object_connector_convert), atombios.h and ObjectID.h. scripts/check_connectors_port.py checks the numbers against them.

namespace hardware {

namespace {
constexpr uint32_t kBiosMagic = 0xAA55, kRomTablePtr = 0x48, kRomMagicPtr = 4, kRomDataPtr = 0x20;
constexpr uint8_t kRomMagic[4] = {'A', 'T', 'O', 'M'};
constexpr uint32_t kDataGpioPinLut = 12, kDataObjectHeader = 22;   // indexes in the master list of data tables
constexpr uint32_t kTableHeaderBytes = 4;                          // ATOM_COMMON_TABLE_HEADER
// ATOM_OBJECT_HEADER: the header, the device vector, then the offsets of the connector, router, encoder and protection object tables and of the display path table
constexpr uint32_t kObjDeviceSupport = 4, kObjConnectorTable = 6, kObjPathTable = 14;
constexpr uint32_t kPathTableHeaderBytes = 4;                      // ATOM_DISPLAY_OBJECT_PATH_TABLE: count, version, 2 padding
constexpr uint32_t kPathHeaderBytes = 8;                           // ATOM_DISPLAY_OBJECT_PATH without its object ids: tag, size, connector, GPU
constexpr uint32_t kObjectTableHeaderBytes = 4, kObjectBytes = 8;  // ATOM_OBJECT_TABLE, ATOM_OBJECT
constexpr uint8_t kI2cRecord = 1, kHpdIntRecord = 2, kMaxRecordType = 22;          // ATOM_HPD_INT_RECORD_TYPE, ATOM_MAX_OBJECT_RECORD_NUMBER
constexpr uint32_t kGpioPinBytes = 4;                              // ATOM_GPIO_PIN_ASSIGNMENT
constexpr uint32_t kDeviceTv1 = 1u << 2, kDeviceCv = 1u << 8;   // ATOM_DEVICE_TV1_SUPPORT, ATOM_DEVICE_CV_SUPPORT
constexpr uint32_t kObjectIdMask = 0x00FF, kObjectIdShift = 0, kEnumIdMask = 0x0700, kEnumIdShift = 8;
constexpr uint32_t kHpdGpioRegister = 0x488d;                      // mmDC_GPIO_HPD_A (dce_v11_0_hpd_get_gpio_reg)
constexpr uint32_t kMaxPaths = 255, kMaxRecords = 64;
constexpr uint32_t kObjectTypeMask = 0x7000, kObjectTypeShift = 12, kGraphObjectTypeEncoder = 2;
constexpr uint32_t kMaxGraphObjects = 8;                           // the object ids of a path walked (a path has the encoders from the GPU to the connector: one to three)

// object_connector_convert, by connector object id; kUnknown for the ones that are not a connector with a name
constexpr int kUnknown = -1;
constexpr int kConnectorOfObject[] = {
    kUnknown, 0 /*DVI-I*/, 0, 1 /*DVI-D*/, 1, 2 /*VGA*/, 3 /*Composite*/, 4 /*SVideo*/, kUnknown, kUnknown, 5 /*DIN*/, kUnknown,
    6 /*HDMI-A*/, 7 /*HDMI-B*/, 8 /*LVDS*/, 5 /*DIN*/, kUnknown, kUnknown, kUnknown, 9 /*DP*/, 10 /*eDP*/, kUnknown,
};
constexpr uint32_t kConnectorCount = sizeof kConnectorOfObject / sizeof kConnectorOfObject[0];

struct Image {
  ConstByteView bytes;
  bool u8(uint64_t at, uint8_t* out) const { return bytes.try_get(at, out); }
  bool u16(uint64_t at, uint16_t* out) const {
    uint8_t lo = 0, hi = 0;
    if (at > UINT64_MAX - 1 || !bytes.try_get(at, &lo) || !bytes.try_get(at + 1, &hi)) return false;
    *out = static_cast<uint16_t>(lo | (hi << 8));
    return true;
  }
};

// amdgpu_atom_parse_data_header: where data table `index` starts and how big it says it is; false if the master list has no such table
bool data_table(const Image& image, uint32_t data_table_at, uint32_t index, uint16_t* start, uint16_t* size) {
  uint16_t at = 0;
  if (!image.u16(data_table_at + index * 2 + 4, &at) || at == 0) return false;
  *start = at;
  return image.u16(at, size);
}

// amdgpu_atombios_lookup_gpio and _get_hpd_info_from_gpio: the hot plug pin of GPIO `id`, or kNoPin
uint8_t hpd_pin_of_gpio(const Image& image, uint32_t data_table_at, uint8_t id) {
  uint16_t start = 0, size = 0;
  if (!data_table(image, data_table_at, kDataGpioPinLut, &start, &size)) return AtomConnectors::kNoPin;
  const uint32_t pins = size >= kTableHeaderBytes ? (size - kTableHeaderBytes) / kGpioPinBytes : 0;
  for (uint32_t i = 0; i < pins; ++i) {
    const uint32_t at = start + kTableHeaderBytes + i * kGpioPinBytes;
    uint16_t reg = 0;
    uint8_t shift = 0, gpio_id = 0;
    if (!image.u16(at, &reg) || !image.u8(at + 2, &shift) || !image.u8(at + 3, &gpio_id)) return AtomConnectors::kNoPin;
    if (gpio_id != id) continue;
    if (reg != kHpdGpioRegister || shift >= 32) return AtomConnectors::kNoPin;
    switch (1u << shift) {
      case 1u << 0: return 0;
      case 1u << 8: return 1;
      case 1u << 16: return 2;
      case 1u << 24: return 3;
      case 1u << 26: return 4;
      case 1u << 28: return 5;
      default: return AtomConnectors::kNoPin;
    }
  }
  return AtomConnectors::kNoPin;
}

// The records of a connector object: the DDC line of its I2C record and the hot plug interrupt record, the last of each as the driver's loop leaves them
struct ConnectorRecords {
  bool has_hpd = false, has_ddc = false;
  uint8_t gpio_id = 0, plugged_state = 0, ddc_id = 0;
};
void walk_records(const Image& image, uint32_t records, ConnectorRecords* out) {
  uint32_t at = records;
  for (uint32_t i = 0; i < kMaxRecords; ++i) {
    uint8_t type = 0, size = 0;
    if (!image.u8(at, &type) || !image.u8(at + 1, &size)) return;
    if (size == 0 || type == 0 || type > kMaxRecordType) return;
    if (type == kI2cRecord) {
      uint8_t id = 0;
      if (image.u8(at + 2, &id)) {
        out->ddc_id = id;
        out->has_ddc = true;
      }
    } else if (type == kHpdIntRecord) {
      uint8_t gpio = 0, plugged = 0;
      if (image.u8(at + 2, &gpio) && image.u8(at + 3, &plugged)) {
        out->gpio_id = gpio;
        out->plugged_state = plugged;
        out->has_hpd = true;
      }
    }
    at += size;
  }
}
}  // namespace

const char* AtomConnectors::type_name(Type type) {
  switch (type) {
    case Type::DviI: return "DVI-I";
    case Type::DviD: return "DVI-D";
    case Type::Vga: return "VGA";
    case Type::Composite: return "Composite";
    case Type::SVideo: return "SVIDEO";
    case Type::DIN: return "DIN";
    case Type::HdmiA: return "HDMI-A";
    case Type::HdmiB: return "HDMI-B";
    case Type::Lvds: return "LVDS";
    case Type::DisplayPort: return "DP";
    case Type::EmbeddedDisplayPort: return "eDP";
  }
  return "Unknown";
}

uint32_t AtomConnectors::name(const Connector& connector, ArrayView<char> buffer) {
  if (buffer.size() == 0) return 0;
  uint32_t used = 0;
  auto put = [&](char c) {
    if (used + 1 < buffer.size()) buffer[used++] = c;
  };
  for (const char* p = type_name(connector.type); *p; ++p) put(*p);
  put('-');
  if (connector.number >= 100) put(static_cast<char>('0' + connector.number / 100));
  if (connector.number >= 10) put(static_cast<char>('0' + connector.number / 10 % 10));
  put(static_cast<char>('0' + connector.number % 10));
  buffer[used] = 0;
  return used;
}

uint8_t AtomConnectors::dig_encoder(const Connector& c) {
  switch (encoder_id(c)) {
    case kEncoderUniphy: return link_b(c) ? 1 : 0;
    case kEncoderUniphy1: return link_b(c) ? 3 : 2;
    case kEncoderUniphy2: return link_b(c) ? 5 : 4;
    case kEncoderUniphy3: return 6;
    default: return kNoDig;
  }
}

uint32_t AtomConnectors::parse(ConstByteView bios, ArrayView<Connector> out) {
  const Image image{bios};
  uint16_t magic = 0;
  if (!image.u16(0, &magic) || magic != kBiosMagic) return 0;
  uint16_t rom_table = 0;
  if (!image.u16(kRomTablePtr, &rom_table)) return 0;
  for (uint32_t i = 0; i < sizeof kRomMagic; ++i) {
    uint8_t c = 0;
    if (!image.u8(uint32_t{rom_table} + kRomMagicPtr + i, &c) || c != kRomMagic[i]) return 0;
  }
  uint16_t data_table_at = 0;
  if (!image.u16(uint32_t{rom_table} + kRomDataPtr, &data_table_at)) return 0;

  uint16_t header = 0, header_size = 0;
  if (!data_table(image, data_table_at, kDataObjectHeader, &header, &header_size)) return 0;
  uint8_t crev = 0;
  if (!image.u8(uint32_t{header} + 3, &crev) || crev < 2) return 0;
  uint16_t device_support = 0, connector_table = 0, path_table = 0;
  if (!image.u16(uint32_t{header} + kObjDeviceSupport, &device_support) || !image.u16(uint32_t{header} + kObjConnectorTable, &connector_table) ||
      !image.u16(uint32_t{header} + kObjPathTable, &path_table))
    return 0;

  const uint32_t paths_at = uint32_t{header} + path_table;
  uint8_t path_count = 0;
  if (!image.u8(paths_at, &path_count)) return 0;
  const uint32_t connectors_at = uint32_t{header} + connector_table;
  uint8_t object_count = 0;
  const bool have_objects = image.u8(connectors_at, &object_count);

  const uint32_t capacity = out.size() < kMaxConnectors ? static_cast<uint32_t>(out.size()) : kMaxConnectors;
  uint16_t conn_ids[kMaxConnectors] = {};
  uint32_t count = 0, path_at = paths_at + kPathTableHeaderBytes;
  for (uint32_t p = 0; p < path_count && p < kMaxPaths; ++p) {
    uint16_t tag = 0, size = 0, conn_id = 0;
    if (!image.u16(path_at, &tag) || !image.u16(path_at + 2, &size) || !image.u16(path_at + 4, &conn_id)) break;
    if (size < kPathHeaderBytes) break;   // (the driver would parse the same path again)
    const uint32_t this_path = path_at;
    path_at += size;
    if (!(device_support & tag)) continue;
    const uint32_t object_id = (conn_id & kObjectIdMask) >> kObjectIdShift;
    if (tag == kDeviceTv1 || tag == kDeviceCv) continue;   // Skip TV/CV support
    if (object_id >= kConnectorCount) continue;
    const int type = kConnectorOfObject[object_id];
    if (type == kUnknown) continue;

    uint8_t pin = kNoPin, plugged = 0, ddc_id = 0;
    bool has_ddc = false;
    if ((tag & (kDeviceTv1 | kDeviceCv)) == 0 && have_objects) {
      for (uint32_t j = 0; j < object_count; ++j) {
        const uint32_t object = connectors_at + kObjectTableHeaderBytes + j * kObjectBytes;
        uint16_t id = 0, record_offset = 0;
        if (!image.u16(object, &id) || !image.u16(object + 4, &record_offset)) break;
        if (id != conn_id) continue;
        ConnectorRecords records;
        walk_records(image, uint32_t{header} + record_offset, &records);
        if (records.has_hpd) {
          pin = hpd_pin_of_gpio(image, data_table_at, records.gpio_id);
          plugged = records.plugged_state;
        }
        ddc_id = records.ddc_id;
        has_ddc = records.has_ddc;
        break;
      }
    }

    // The encoders of the path (amdgpu_atombios_get_connector_info_from_object_table: usGraphicObjIds, the objects of the encoder type): the first is the GPU's, a second one a bridge
    uint16_t encoder = 0, bridge = 0;
    const uint32_t objects_in_path = (size - kPathHeaderBytes) / 2 < kMaxGraphObjects ? (size - kPathHeaderBytes) / 2 : kMaxGraphObjects;
    for (uint32_t k = 0; k < objects_in_path; ++k) {
      uint16_t graph = 0;
      if (!image.u16(this_path + kPathHeaderBytes + k * 2, &graph)) break;
      if (((graph & kObjectTypeMask) >> kObjectTypeShift) != kGraphObjectTypeEncoder) continue;
      if (!encoder) encoder = graph;
      else if (!bridge) bridge = graph;
    }

    uint32_t existing = 0;
    while (existing < count && conn_ids[existing] != conn_id) ++existing;
    if (existing < count) {   // amdgpu_connector_add: the connector is there already, with one more device
      out[existing].devices |= tag;
      if (!out[existing].encoder_object) {
        out[existing].encoder_object = encoder;
        out[existing].bridge_object = bridge;
      }
      if (out[existing].hpd_pin == kNoPin && pin != kNoPin) {
        out[existing].hpd_pin = pin;
        out[existing].plugged_state = plugged;
      }
      if (!out[existing].has_ddc && has_ddc) {
        out[existing].has_ddc = true;
        out[existing].ddc_id = ddc_id;
      }
      continue;
    }
    if (count >= capacity) continue;
    Connector c;
    c.type = static_cast<Type>(type);
    c.object_id = static_cast<uint8_t>(object_id);
    c.enum_id = static_cast<uint8_t>((conn_id & kEnumIdMask) >> kEnumIdShift);
    c.hpd_pin = pin;
    c.plugged_state = plugged;
    c.has_ddc = has_ddc;
    c.ddc_id = ddc_id;
    c.devices = tag;
    c.encoder_object = encoder;
    c.bridge_object = bridge;
    c.number = 1;
    for (uint32_t i = 0; i < count; ++i)
      if (out[i].type == c.type) ++c.number;
    conn_ids[count] = conn_id;
    out[count++] = c;
  }
  return count;
}

const AtomConnectors::Connector* AtomConnectors::find_pin(ArrayView<const Connector> connectors, uint32_t pin) {
  for (const Connector& c : connectors)
    if (c.hpd_pin == pin) return &c;
  return nullptr;
}

}  // namespace hardware
