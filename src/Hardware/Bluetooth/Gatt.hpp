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

// GATT client (Core spec vol 3 part G): finds the services, characteristics and descriptors of a peripheral over the attribute protocol, reads and writes characteristic values, and
// switches notifications on. The tables are fixed in size; what does not fit is left out and `truncated` says so.
#pragma once
#include "Hardware/Bluetooth/Att.hpp"

namespace hardware {

// A UUID in full (128 bits, least significant byte first, as it is sent). The 16 bit ones are the Bluetooth base UUID with the short value in bytes 12 and 13.
struct Uuid {
  uint8_t b[16] = {};
  static Uuid from16(uint16_t value);
  // From the 2 or 16 bytes of a PDU; an empty UUID (all zero) for any other length.
  static Uuid from_bytes(ConstByteView bytes);
  bool is16() const;
  uint16_t value16() const { return static_cast<uint16_t>(b[12] | (b[13] << 8)); }
  bool operator==(const Uuid& o) const {
    for (int i = 0; i < 16; ++i)
      if (b[i] != o.b[i]) return false;
    return true;
  }
  bool operator!=(const Uuid& o) const { return !(*this == o); }
  bool empty() const {
    for (int i = 0; i < 16; ++i)
      if (b[i]) return false;
    return true;
  }
};

namespace gatt {
constexpr uint16_t kPrimaryService = 0x2800, kSecondaryService = 0x2801, kCharacteristic = 0x2803, kClientConfiguration = 0x2902;
constexpr uint8_t kPropBroadcast = 0x01, kPropRead = 0x02, kPropWriteWithoutResponse = 0x04, kPropWrite = 0x08, kPropNotify = 0x10, kPropIndicate = 0x20;
}  // namespace gatt

struct GattService {
  uint16_t start = 0, end = 0;
  Uuid uuid;
};
struct GattCharacteristic {
  uint16_t declaration = 0, value_handle = 0, end_handle = 0, cccd = 0;     // end_handle: the last handle before the next declaration or the end of the service
  uint8_t properties = 0;
  uint8_t service = 0;
  Uuid uuid;
};
struct GattDescriptor {
  uint16_t handle = 0;
  uint8_t characteristic = 0;
  Uuid uuid;
};

struct GattDatabase {
  static constexpr uint32_t kMaxServices = 16, kMaxCharacteristics = 48, kMaxDescriptors = 64;
  GattService services[kMaxServices];
  GattCharacteristic characteristics[kMaxCharacteristics];
  GattDescriptor descriptors[kMaxDescriptors];
  uint32_t service_count = 0, characteristic_count = 0, descriptor_count = 0;
  bool truncated = false;
  // The first characteristic with this UUID in a service with `service` (or in any service when `service` is empty); -1 if none.
  int find_characteristic(const Uuid& uuid, const Uuid& service = Uuid()) const;
  int find_service(const Uuid& uuid) const;
};

class GattClient {
 public:
  explicit GattClient(AttClient& att) : att_(att) {}
  // Discovers everything on the link. Ok when the walk completed (even if some of it did not fit).
  AttStatus discover(uint16_t link, GattDatabase* out);
  AttStatus read(uint16_t link, const GattDatabase& db, int characteristic, ByteView out, uint32_t* length);
  AttStatus write(uint16_t link, const GattDatabase& db, int characteristic, ConstByteView value, bool with_response = true);
  // Writes the client characteristic configuration descriptor: notifications and/or indications on or off.
  AttStatus subscribe(uint16_t link, const GattDatabase& db, int characteristic, bool notifications, bool indications = false);

 private:
  AttClient& att_;
};

}  // namespace hardware
