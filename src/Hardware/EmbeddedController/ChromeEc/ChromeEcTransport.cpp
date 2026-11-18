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

#include "Hardware/EmbeddedController/ChromeEc/ChromeEcTransport.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace chromeec;

const char* ec_transfer_status_name(EcTransferStatus status) {
  switch (status) {
    case EcTransferStatus::Ok: return "ok";
    case EcTransferStatus::BusyBefore: return "the controller was busy";
    case EcTransferStatus::Timeout: return "the controller did not answer in time";
    case EcTransferStatus::RequestTooLarge: return "request too large";
    case EcTransferStatus::BadResponse: return "malformed response";
    case EcTransferStatus::ResponseTooLarge: return "response too large";
    case EcTransferStatus::BadChecksum: return "response checksum wrong";
  }
  return "?";
}

// --- Shared: the command/status ports --------------------------------------------------------------------------------------------------------

bool ChromeEcTransport::wait_idle() {
  for (uint32_t i = 0; i < kPollLimit; ++i) {
    ++polls_used_;
    if ((ports_.in8(kPortHostCommand) & kStatusBusyMask) == 0) return true;
    ports_.delay_us(kPollDelayUs);
  }
  return false;
}

bool ChromeEcTransport::read_memmap(uint32_t offset, ByteView out) {
  if (offset > kMemmapSize || out.size() > kMemmapSize - offset) return false;
  if (!out.empty()) read_map(offset, out);
  return true;
}

EcTransfer ChromeEcTransport::transfer(ConstByteView request, ByteView data) {
  EcTransfer t;
  polls_used_ = 0;
  if (request.size() < kRequestHeaderBytes || request.size() > kHostPacketSize) {
    t.status = EcTransferStatus::RequestTooLarge;
    return t;
  }
  if (!wait_idle()) {
    t.status = EcTransferStatus::BusyBefore;
    return t;
  }
  write_packet(0, request);
  ports_.out8(kPortHostCommand, kCommandProtocol3);
  if (!wait_idle()) {
    t.status = EcTransferStatus::Timeout;
    return t;
  }
  // The result byte: non-zero when the EC refused the packet itself (bad header, bad checksum); the response buffer is then not a response.
  const uint8_t refused = ports_.in8(kPortHostData);
  if (refused != 0) {
    t.status = EcTransferStatus::Ok;
    t.result = refused;
    return t;
  }
  uint8_t header_bytes[kResponseHeaderBytes];
  read_packet(0, ByteView(header_bytes));
  ResponseHeader header;
  if (!parse_response_header(ConstByteView(header_bytes), &header) || header.struct_version != kPacketVersion) {
    t.status = EcTransferStatus::BadResponse;
    return t;
  }
  if (header.data_length > kMaxResponseDataBytes || header.data_length > data.size()) {
    t.status = EcTransferStatus::ResponseTooLarge;
    t.result = header.result;
    return t;
  }
  ByteView body = data.first(header.data_length);
  if (!body.empty()) read_packet(kResponseHeaderBytes, body);
  if (static_cast<uint8_t>(byte_sum(ConstByteView(header_bytes)) + byte_sum(body)) != 0) {
    body.fill(0);   // (nothing of a corrupt answer is left for a careless caller)
    t.status = EcTransferStatus::BadChecksum;
    return t;
  }
  t.status = EcTransferStatus::Ok;
  t.result = header.result;
  t.data_bytes = header.data_length;
  return t;
}

// --- Plain LPC I/O ------------------------------------------------------------------------------------------------------------------------------

void LpcTransport::read_packet(uint32_t offset, ByteView out) {
  uint16_t port = static_cast<uint16_t>(kPortHostPacket + offset);
  for (uint8_t& b : out) b = ports_.in8(port++);
}

void LpcTransport::write_packet(uint32_t offset, ConstByteView bytes) {
  uint16_t port = static_cast<uint16_t>(kPortHostPacket + offset);
  for (uint8_t b : bytes) ports_.out8(port++, b);
}

void LpcTransport::read_map(uint32_t offset, ByteView out) {
  uint16_t port = static_cast<uint16_t>(memmap_base_ + offset);
  for (uint8_t& b : out) b = ports_.in8(port++);
}

// --- Microchip EMI ------------------------------------------------------------------------------------------------------------------------------
// As cros_ec_lpc_io_bytes_mec: byte access for a short or misaligned start, otherwise 32-bit auto-increment, where reading data byte 0 loads
// the next dword and touching data byte 3 moves the address on. A write's last partial dword is done in byte mode (writing byte 3 in
// long mode would flush the whole dword).

bool MecEmi::fits(uint32_t address, uint64_t size) const {
  constexpr uint32_t window = kMecWindowEnd - kMecWindowStart;
  return address <= window && size <= window - address;
}

void MecEmi::set_address(uint32_t address, uint8_t access) {
  ports_.out8(static_cast<uint16_t>(base_ + kMecEmiAddressB0), static_cast<uint8_t>((address & 0xFC) | access));
  ports_.out8(static_cast<uint16_t>(base_ + kMecEmiAddressB1), static_cast<uint8_t>((address >> 8) & 0x7F));
}

bool MecEmi::read(uint32_t address, ByteView out) {
  if (!fits(address, out.size())) return false;
  if (out.empty()) return true;
  uint8_t access = ((address & 3) || out.size() < 4) ? kMecAccessByte : kMecAccessLongAutoIncrement;
  set_address(address, access);
  uint32_t lane = address & 3;
  uint64_t i = 0;
  while (i < out.size()) {
    while (lane < 4 && i < out.size()) {
      out[i++] = ports_.in8(static_cast<uint16_t>(base_ + kMecEmiData + lane));
      ++lane;
      ++address;
    }
    if (i == out.size()) break;
    // The next dword: auto-increment already moved there unless this pass was in byte mode (reads stay in long mode to the end).
    if (access != kMecAccessLongAutoIncrement) {
      access = kMecAccessLongAutoIncrement;
      set_address(address, access);
    }
    lane = 0;
  }
  return true;
}

bool MecEmi::write(uint32_t address, ConstByteView bytes) {
  if (!fits(address, bytes.size())) return false;
  if (bytes.empty()) return true;
  uint8_t access = ((address & 3) || bytes.size() < 4) ? kMecAccessByte : kMecAccessLongAutoIncrement;
  set_address(address, access);
  uint32_t lane = address & 3;
  uint64_t i = 0;
  while (i < bytes.size()) {
    while (lane < 4 && i < bytes.size()) {
      ports_.out8(static_cast<uint16_t>(base_ + kMecEmiData + lane), bytes[i++]);
      ++lane;
      ++address;
    }
    if (i == bytes.size()) break;
    const uint8_t next = (bytes.size() - i >= 4) ? kMecAccessLongAutoIncrement : kMecAccessByte;
    if (next != access || access != kMecAccessLongAutoIncrement) {
      access = next;
      set_address(address, access);
    }
    lane = 0;
  }
  return true;
}

void MecTransport::read_packet(uint32_t offset, ByteView out) {
  if (!emi_.read(kPortHostPacket - kMecWindowStart + offset, out)) out.fill(0xFF);
}

void MecTransport::write_packet(uint32_t offset, ConstByteView bytes) { (void)emi_.write(kPortHostPacket - kMecWindowStart + offset, bytes); }

void MecTransport::read_map(uint32_t offset, ByteView out) {
  if (plain_memmap_base_) {
    uint16_t port = static_cast<uint16_t>(plain_memmap_base_ + offset);
    for (uint8_t& b : out) b = ports_.in8(port++);
    return;
  }
  if (!emi_.read(kPortMemmap - kMecWindowStart + offset, out)) out.fill(0xFF);
}

// --- Packet helpers -----------------------------------------------------------------------------------------------------------------------------

namespace chromeec {

uint32_t build_request(uint16_t command, uint8_t version, ConstByteView params, ByteView out) {
  if (params.size() > kMaxParamBytes || out.size() < kRequestHeaderBytes || params.size() > out.size() - kRequestHeaderBytes) return 0;
  ByteWriter w(out);
  w.u8(kPacketVersion);
  w.u8(0);   // checksum, below
  w.le16(command);
  w.u8(version);
  w.u8(0);
  w.le16(static_cast<uint16_t>(params.size()));
  w.bytes(params);
  if (!w.ok()) return 0;
  ByteView packet = w.written();
  packet[1] = checksum_complement(ConstByteView(packet));
  return static_cast<uint32_t>(packet.size());
}

bool parse_response_header(ConstByteView bytes, ResponseHeader* out) {
  ByteReader r(bytes);
  out->struct_version = r.u8();
  r.u8();   // checksum (covered by the sum over the packet)
  out->result = r.le16();
  out->data_length = r.le16();
  r.le16();
  return r.ok();
}

}  // namespace chromeec

}  // namespace hardware
