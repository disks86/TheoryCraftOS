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

// A minimal SDP client (Core spec vol 3 part B): ServiceSearchAttributeRequest over an L2CAP channel on PSM 1, with the continuation state followed until the whole answer is in, and
// a reader for the data elements the answer is made of. Everything a device sends is untrusted: element sizes are checked against the bytes there, nesting is limited, the
// continuation state is at most 16 bytes, and the answer as a whole is limited in size and in the number of rounds it may take.
//
// What the classic HID host asks: the HID service record (0x1124) for its HIDDescriptorList (attribute 0x0206, the report map), and the PnP Information record (0x1200) for the
// vendor (0x0201) and product (0x0202) ids.
#pragma once
#include "Hardware/Bluetooth/L2cap.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace sdp {

constexpr uint16_t kPsm = 0x0001;
constexpr uint16_t kServiceClassHid = 0x1124, kServiceClassPnp = 0x1200;
constexpr uint16_t kAttributeHidDescriptorList = 0x0206, kAttributeVendorId = 0x0201, kAttributeProductId = 0x0202;
constexpr uint32_t kMaxDepth = 8, kMaxContinuation = 16;

enum class Type : uint8_t { Nil = 0, Uint = 1, Int = 2, Uuid = 3, Text = 4, Bool = 5, Sequence = 6, Alternative = 7, Url = 8 };

struct Element {
  Type type = Type::Nil;
  ConstByteView data;          // the value's bytes (big-endian numbers, the text, or a sequence's elements)
};

// Reads one data element at the reader's position; false (and the reader failed) if it is damaged: an unknown type, a size the type does not allow, or one that runs past the data.
[[nodiscard]] bool read_element(ByteReader& in, Element* out);
// Whether `data` is nothing but well-formed elements, sequences nested at most `max_depth` deep.
[[nodiscard]] bool valid(ConstByteView data, uint32_t max_depth = kMaxDepth);
// An unsigned integer element of 1, 2 or 4 bytes.
[[nodiscard]] bool to_uint(const Element& element, uint32_t* out);
// In an attribute list (a sequence of id and value pairs), the value of `id`.
[[nodiscard]] bool find_attribute(const Element& attribute_list, uint16_t id, Element* out);
// In a HIDDescriptorList value, the first descriptor of class 0x22 (the report descriptor).
[[nodiscard]] bool hid_report_descriptor(const Element& descriptor_list, ConstByteView* out);
// In the AttributeLists of a search answer (a sequence of attribute lists, one per record), the first record that has `id`, and that attribute's value.
[[nodiscard]] bool find_in_records(ConstByteView attribute_lists, uint16_t id, Element* out);

}  // namespace sdp

enum class SdpResult : uint8_t { Ok, NoLink, Refused, Timeout, Malformed, TooLarge, Error };

class SdpClient final : private IL2capChannelHandler {
 public:
  static constexpr uint32_t kMaxAnswer = 16 * 1024, kMaxRounds = 32;
  static constexpr uint16_t kMaxAttributeBytes = 512;     // asked for a round (it fits the 672 byte L2CAP MTU with the headers)

  SdpClient(L2cap& l2cap, HciController& controller) : l2cap_(l2cap), controller_(controller) {}
  ~SdpClient() { close(); }
  SdpClient(const SdpClient&) = delete;
  SdpClient& operator=(const SdpClient&) = delete;

  // Every attribute of the records of a 16-bit service class: the AttributeLists element of the answer (checked with sdp::valid), whole, in `out`. Opens the channel on the first
  // call (and keeps it for the next); runs the controller meanwhile. On Error, error_code() is the code the server answered with.
  [[nodiscard]] SdpResult search_attributes(uint16_t acl_handle, uint16_t service_class, ByteBuffer& out, uint32_t timeout_ms = 5000);
  uint16_t error_code() const { return error_code_; }
  void close();

 private:
  void on_open(L2capChannel& channel) override;
  void on_data(L2capChannel& channel, ConstByteView sdu) override;
  void on_close(L2capChannel& channel) override;
  static bool channel_settled(void* self);
  static bool answered(void* self);

  L2cap& l2cap_;
  HciController& controller_;
  L2capChannel* channel_ = nullptr;
  uint16_t handle_ = 0, cid_ = 0;
  bool open_ = false, closed_ = false;
  // the request in flight
  uint16_t transaction_ = 0;
  bool waiting_ = false, have_answer_ = false;
  uint8_t answer_[L2cap::kMaxFrame] = {};
  uint32_t answer_length_ = 0;
  uint16_t error_code_ = 0;
};

}  // namespace hardware
