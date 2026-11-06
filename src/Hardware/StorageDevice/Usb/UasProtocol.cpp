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

#include "Hardware/StorageDevice/Usb/UasProtocol.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware::uas {

namespace {
constexpr uint8_t kClassMassStorage = 8, kSubclassScsi = 6;
}  // namespace

bool build_command_iu(ByteView out, uint16_t tag, ConstByteView cdb) {
  if (out.size() < kCommandIuBytes || cdb.size() > 16 || tag == 0) return false;
  ByteWriter w(out.first(kCommandIuBytes));
  w.u8(kIuCommand);
  w.u8(0);
  w.be16(tag);
  w.u8(0);        // simple task attribute, priority 0
  w.u8(0);
  w.u8(0);        // no additional CDB bytes
  w.u8(0);
  w.zeros(8);     // LUN 0
  w.bytes(cdb);
  w.zeros(16 - cdb.size());
  return w.ok();
}

bool parse_status_iu(ConstByteView data, StatusIu* out) {
  ByteReader in(data);
  *out = StatusIu{};
  out->id = in.u8();
  in.skip(1);
  out->tag = in.be16();
  if (!in.ok() || out->tag == 0) return false;
  switch (out->id) {
    case kIuReadReady:
    case kIuWriteReady:
      return true;
    case kIuResponse:
      in.skip(3);   // additional response information
      out->response_code = in.u8();
      return in.ok();
    case kIuSense: {
      in.skip(2);   // status qualifier
      out->status = in.u8();
      in.skip(7);
      const uint16_t length = in.be16();
      out->sense = in.take(length);
      return in.ok();
    }
    default:
      return false;
  }
}

// The alternate setting of a mass storage interface with protocol UAS: its endpoints, each followed (on SuperSpeed) by a companion
// descriptor and then a Pipe Usage descriptor that says which pipe it is.
bool find_uas_interface(ConstByteView configuration, UasInterface* out) {
  UsbDescriptorWalker walker(configuration);
  ConstByteView descriptor;
  bool inside = false, found = false;
  int pending = -1;            // the endpoint index (in `candidate`) the next companion / pipe usage belongs to
  uint8_t last_address = 0;
  uint16_t last_packet = 0;
  uint32_t last_streams = 0;
  UasInterface candidate;
  uint32_t streams[4] = {};
  auto finish = [&] {
    if (!inside) return;
    inside = false;
    for (uint8_t e : candidate.endpoint)
      if (!e) return;
    // (the command pipe has no streams; the other three must agree on how many they can take)
    uint32_t least = streams[1];
    if (streams[2] < least) least = streams[2];
    if (streams[3] < least) least = streams[3];
    candidate.streams = least;
    *out = candidate;
    found = true;
  };
  while (!found && walker.next(&descriptor)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    if (parse_usb_interface(descriptor, &interface)) {
      finish();
      if (found) break;
      inside = interface.device_class == kClassMassStorage && interface.subclass == kSubclassScsi && interface.protocol == kProtocolUas;
      candidate = UasInterface{};
      candidate.number = interface.number;
      candidate.alternate = interface.alternate_setting;
      for (uint32_t& s : streams) s = 0;
      pending = -1;
      last_address = 0;
    } else if (inside && parse_usb_endpoint(descriptor, &endpoint)) {
      last_address = endpoint.transfer_type() == 2 ? endpoint.address : 0;
      last_packet = endpoint.max_packet;
      last_streams = 0;
      pending = 0;
    } else if (inside && descriptor.size() >= 4 && descriptor[1] == kDescriptorSuperSpeedCompanion && pending == 0) {
      const uint32_t exponent = descriptor[3] & 0x1F;   // bmAttributes: MaxStreams for a bulk endpoint
      last_streams = exponent ? 1u << (exponent > 16 ? 16 : exponent) : 0;
    } else if (inside && descriptor.size() >= 4 && descriptor[1] == kDescriptorPipeUsage && pending == 0 && last_address) {
      const uint8_t pipe = descriptor[2];
      const bool in = last_address & 0x80;
      const bool direction_ok = pipe == kPipeCommand ? !in : pipe == kPipeDataOut ? !in : in;
      if (pipe >= kPipeCommand && pipe <= kPipeDataOut && direction_ok && !candidate.endpoint[pipe - 1]) {
        candidate.endpoint[pipe - 1] = last_address;
        candidate.max_packet[pipe - 1] = last_packet;
        streams[pipe - 1] = last_streams;
      }
      pending = -1;
    }
  }
  finish();
  return found;
}

}  // namespace hardware::uas
