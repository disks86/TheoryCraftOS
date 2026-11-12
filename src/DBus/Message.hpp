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

// A D-Bus message: the fixed header, the header fields the specification names, the body (kept as the bytes that came, in the sender's byte order) and
// the descriptors that go with it (UNIX_FDS).
//
// decode() checks everything: the fixed header (protocol 1, a known type, a serial), each header field's type and value, the fields each type of message
// needs, and the body against its signature. Header fields of codes the specification does not name are dropped, so a forwarded message carries only
// what the bus has checked (the bus says so with the "HeaderFiltering" feature). encode() writes the header afresh in the message's own byte order, then
// the body as it is.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace dbus {

enum MessageType : uint8_t { kMethodCall = 1, kMethodReturn = 2, kError = 3, kSignal = 4 };
enum MessageFlags : uint8_t { kNoReplyExpected = 0x1, kNoAutoStart = 0x2, kAllowInteractiveAuthorization = 0x4 };
enum HeaderField : uint8_t { kFieldPath = 1, kFieldInterface = 2, kFieldMember = 3, kFieldErrorName = 4, kFieldReplySerial = 5, kFieldDestination = 6,
                             kFieldSender = 7, kFieldSignature = 8, kFieldUnixFds = 9 };

// A descriptor passed with a message. What it is belongs to the transport that received it (an SCM_RIGHTS entry of the kernel's sockets, a host
// descriptor in a test); the bus only holds it and hands it on. Each transport makes its own kind with SharedPointer::make_as.
class Fd {
 protected:
  Fd() = default;
  ~Fd() = default;      // (made with SharedPointer::make_as, which destroys the real type)
};
using FdRef = SharedPointer<Fd>;

struct Message {
  explicit Message(allocators::IAllocator& allocator)
      : path(allocator), interface(allocator), member(allocator), error_name(allocator), destination(allocator), sender(allocator), signature(allocator),
        body(allocator), fds(allocator) {}
  Message(const Message&) = delete;
  Message& operator=(const Message&) = delete;

  bool big_endian = false;
  uint8_t type = 0;
  uint8_t flags = 0;
  uint32_t serial = 0;
  String path, interface, member, error_name, destination, sender, signature;
  bool has_reply_serial = false;
  uint32_t reply_serial = 0;
  uint32_t unix_fds = 0;      // the header's UNIX_FDS (the descriptors themselves are `fds`)
  ByteBuffer body;
  List<FdRef> fds;

  bool expects_reply() const { return type == kMethodCall && (flags & kNoReplyExpected) == 0; }
  // A copy (the same descriptors). False when memory ran out.
  bool copy_to(Message* out) const;
};

// The size of the message whose first bytes are `prefix`: 0 while fewer than 16 bytes are there, -1 when the fixed header is invalid or the message would
// be bigger than kMaxMessageBytes.
int64_t framed_size(ConstByteView prefix);

enum class DecodeStatus : uint8_t { Ok, Invalid, NoMemory };
// Decodes one whole message (exactly framed_size() bytes). The descriptors are not set (the caller attaches unix_fds of them).
DecodeStatus decode(ConstByteView bytes, Message* out);
// Appends the message (header in its byte order, body as it is) to `out`. False when memory ran out or it would be too big.
bool encode(const Message& message, ByteBuffer* out);

// Messages of the kernel's own services (each sets everything but the body and its signature, which the caller adds; serial and SENDER are set by
// Bus::send). False when memory ran out.
bool make_reply(const Message& call, Message* out);
bool make_error(const Message& call, StringView error_name, StringView text, Message* out);
bool make_signal(StringView path, StringView interface, StringView member, Message* out);

}  // namespace dbus
