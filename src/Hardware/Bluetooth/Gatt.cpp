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

#include "Hardware/Bluetooth/Gatt.hpp"

namespace hardware {

using namespace att;

namespace {
const uint8_t kBase[12] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
}

Uuid Uuid::from16(uint16_t value) {
  Uuid u;
  for (int i = 0; i < 12; ++i) u.b[i] = kBase[i];
  u.b[12] = static_cast<uint8_t>(value);
  u.b[13] = static_cast<uint8_t>(value >> 8);
  return u;
}

Uuid Uuid::from_bytes(ConstByteView bytes) {
  Uuid u;
  if (bytes.size() == 2) return from16(static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)));
  if (bytes.size() == 16)
    for (int i = 0; i < 16; ++i) u.b[i] = bytes[i];
  return u;
}

bool Uuid::is16() const {
  for (int i = 0; i < 12; ++i)
    if (b[i] != kBase[i]) return false;
  return b[14] == 0 && b[15] == 0;
}

int GattDatabase::find_service(const Uuid& uuid) const {
  for (uint32_t i = 0; i < service_count; ++i)
    if (services[i].uuid == uuid) return static_cast<int>(i);
  return -1;
}

int GattDatabase::find_characteristic(const Uuid& uuid, const Uuid& service) const {
  for (uint32_t i = 0; i < characteristic_count; ++i) {
    if (characteristics[i].uuid != uuid) continue;
    if (!service.empty() && services[characteristics[i].service].uuid != service) continue;
    return static_cast<int>(i);
  }
  return -1;
}

AttStatus GattClient::discover(uint16_t link, GattDatabase* out) {
  *out = GattDatabase();
  // the services: Read By Group Type for the primary service declarations, from where the last answer ended
  uint32_t start = 1;
  for (uint32_t round = 0; round < 64 && start <= 0xFFFF; ++round) {
    uint8_t pdu[7];
    ByteWriter w{ByteView(pdu)};
    w.u8(kReadByGroupTypeRequest);
    w.le16(static_cast<uint16_t>(start));
    w.le16(0xFFFF);
    w.le16(gatt::kPrimaryService);
    const AttStatus status = att_.request(link, w.written(), kReadByGroupTypeResponse);
    if (status == AttStatus::Error && att_.error_code() == kAttributeNotFound) break;
    if (status != AttStatus::Ok) return status;
    ByteReader in(att_.response());
    in.skip(1);
    const uint8_t item = in.u8();
    if (!in.ok() || (item != 6 && item != 20)) return AttStatus::Malformed;
    uint32_t last_end = 0;
    while (in.remaining() >= item) {
      const uint16_t first = in.le16();
      const uint16_t last = in.le16();
      const ConstByteView uuid = in.take(item - 4);
      if (first == 0 || last < first || first < start) return AttStatus::Malformed;      // (a handle that goes backwards would loop forever)
      if (out->service_count < GattDatabase::kMaxServices) {
        GattService& s = out->services[out->service_count++];
        s.start = first;
        s.end = last;
        s.uuid = Uuid::from_bytes(uuid);
      } else {
        out->truncated = true;
      }
      last_end = last;
    }
    if (last_end == 0) return AttStatus::Malformed;
    start = last_end + 1;
  }
  // the characteristics of each service: Read By Type for the declarations
  for (uint32_t s = 0; s < out->service_count; ++s) {
    uint32_t from = out->services[s].start;
    for (uint32_t round = 0; round < 64 && from <= out->services[s].end; ++round) {
      uint8_t pdu[7];
      ByteWriter w{ByteView(pdu)};
      w.u8(kReadByTypeRequest);
      w.le16(static_cast<uint16_t>(from));
      w.le16(out->services[s].end);
      w.le16(gatt::kCharacteristic);
      const AttStatus status = att_.request(link, w.written(), kReadByTypeResponse);
      if (status == AttStatus::Error && att_.error_code() == kAttributeNotFound) break;
      if (status != AttStatus::Ok) return status;
      ByteReader in(att_.response());
      in.skip(1);
      const uint8_t item = in.u8();
      if (!in.ok() || (item != 7 && item != 21)) return AttStatus::Malformed;
      uint32_t last_declaration = 0;
      while (in.remaining() >= item) {
        const uint16_t declaration = in.le16();
        const uint8_t properties = in.u8();
        const uint16_t value_handle = in.le16();
        const ConstByteView uuid = in.take(item - 5);
        if (declaration < from || value_handle <= declaration) return AttStatus::Malformed;
        if (out->characteristic_count < GattDatabase::kMaxCharacteristics) {
          GattCharacteristic& c = out->characteristics[out->characteristic_count++];
          c.declaration = declaration;
          c.value_handle = value_handle;
          c.properties = properties;
          c.service = static_cast<uint8_t>(s);
          c.uuid = Uuid::from_bytes(uuid);
        } else {
          out->truncated = true;
        }
        last_declaration = declaration;
      }
      if (last_declaration == 0) return AttStatus::Malformed;
      from = last_declaration + 1;
    }
  }
  // where each characteristic ends: before the next declaration in its service, or with the service
  for (uint32_t i = 0; i < out->characteristic_count; ++i) {
    GattCharacteristic& c = out->characteristics[i];
    c.end_handle = out->services[c.service].end;
    if (i + 1 < out->characteristic_count && out->characteristics[i + 1].service == c.service && out->characteristics[i + 1].declaration > c.declaration)
      c.end_handle = static_cast<uint16_t>(out->characteristics[i + 1].declaration - 1);
  }
  // the descriptors: Find Information between the value and the end of each characteristic
  for (uint32_t i = 0; i < out->characteristic_count; ++i) {
    GattCharacteristic& c = out->characteristics[i];
    uint32_t from = static_cast<uint32_t>(c.value_handle) + 1;
    for (uint32_t round = 0; round < 16 && from <= c.end_handle; ++round) {
      uint8_t pdu[5];
      ByteWriter w{ByteView(pdu)};
      w.u8(kFindInformationRequest);
      w.le16(static_cast<uint16_t>(from));
      w.le16(c.end_handle);
      const AttStatus status = att_.request(link, w.written(), kFindInformationResponse);
      if (status == AttStatus::Error && att_.error_code() == kAttributeNotFound) break;
      if (status != AttStatus::Ok) return status;
      ByteReader in(att_.response());
      in.skip(1);
      const uint8_t format = in.u8();
      if (!in.ok() || (format != 1 && format != 2)) return AttStatus::Malformed;
      const uint32_t uuid_size = format == 1 ? 2 : 16;
      uint32_t last = 0;
      while (in.remaining() >= 2 + uuid_size) {
        const uint16_t handle = in.le16();
        const ConstByteView uuid = in.take(uuid_size);
        if (handle < from) return AttStatus::Malformed;
        if (out->descriptor_count < GattDatabase::kMaxDescriptors) {
          GattDescriptor& d = out->descriptors[out->descriptor_count++];
          d.handle = handle;
          d.characteristic = static_cast<uint8_t>(i);
          d.uuid = Uuid::from_bytes(uuid);
          if (d.uuid == Uuid::from16(gatt::kClientConfiguration)) c.cccd = handle;
        } else {
          out->truncated = true;
        }
        last = handle;
      }
      if (last == 0) return AttStatus::Malformed;
      from = last + 1;
    }
  }
  return AttStatus::Ok;
}

AttStatus GattClient::read(uint16_t link, const GattDatabase& db, int characteristic, ByteView out, uint32_t* length) {
  if (characteristic < 0 || static_cast<uint32_t>(characteristic) >= db.characteristic_count) return AttStatus::Malformed;
  return att_.read(link, db.characteristics[characteristic].value_handle, out, length);
}

AttStatus GattClient::write(uint16_t link, const GattDatabase& db, int characteristic, ConstByteView value, bool with_response) {
  if (characteristic < 0 || static_cast<uint32_t>(characteristic) >= db.characteristic_count) return AttStatus::Malformed;
  const uint16_t handle = db.characteristics[characteristic].value_handle;
  return with_response ? att_.write(link, handle, value) : att_.write_without_response(link, handle, value);
}

AttStatus GattClient::subscribe(uint16_t link, const GattDatabase& db, int characteristic, bool notifications, bool indications) {
  if (characteristic < 0 || static_cast<uint32_t>(characteristic) >= db.characteristic_count) return AttStatus::Malformed;
  const GattCharacteristic& c = db.characteristics[characteristic];
  if (c.cccd == 0) return AttStatus::Malformed;
  if ((notifications && !(c.properties & gatt::kPropNotify)) || (indications && !(c.properties & gatt::kPropIndicate))) return AttStatus::Malformed;
  const uint8_t value[2] = {static_cast<uint8_t>((notifications ? 1 : 0) | (indications ? 2 : 0)), 0};
  return att_.write(link, c.cccd, ConstByteView(value, 2));
}

}  // namespace hardware
