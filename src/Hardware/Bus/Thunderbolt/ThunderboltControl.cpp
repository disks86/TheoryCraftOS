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

#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "FileSystem/Ext/ExtChecksum.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPollStepUs = 50, kConfigRetries = 4;
constexpr uint32_t kConfigHeaderBytes = 12, kMaxConfigWords = (tb_frame::kMaxBody - kConfigHeaderBytes) / 4;
constexpr uint32_t kReplyMarker = 1u << 9;       // the "unknown" field of the header, set in replies

uint32_t wire_crc(ConstByteView wire_body) { return ~filesystem::ext::crc32c(~0u, wire_body); }

// cfg_error codes that are not answers to a request but events (tb_async_error)
bool async_error(uint8_t code) {
  switch (code) {
    case 1: case 12: case 13: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: return true;
    default: return false;
  }
}

uint32_t load_le32(ConstByteView bytes, uint64_t offset) {
  ByteReader reader(bytes);
  if (!reader.seek(offset)) return 0;
  const uint32_t v = reader.le32();
  return reader.ok() ? v : 0;
}
uint64_t route_of(ConstByteView body) { return (static_cast<uint64_t>(load_le32(body, 0) & 0x3FFFFF) << 32) | load_le32(body, 4); }
}  // namespace

namespace tb_frame {

uint32_t encode(ConstByteView body, ByteView out) {
  const uint64_t n = body.size();
  if (n == 0 || n % 4 != 0 || n > kMaxBody || out.size() < n + kCrcBytes) return 0;
  for (uint64_t i = 0; i < n; i += 4) {
    out[i + 0] = body[i + 3];
    out[i + 1] = body[i + 2];
    out[i + 2] = body[i + 1];
    out[i + 3] = body[i + 0];
  }
  const uint32_t crc = wire_crc(ConstByteView(out.data(), n));
  out[n + 0] = static_cast<uint8_t>(crc >> 24);
  out[n + 1] = static_cast<uint8_t>(crc >> 16);
  out[n + 2] = static_cast<uint8_t>(crc >> 8);
  out[n + 3] = static_cast<uint8_t>(crc);
  return static_cast<uint32_t>(n + kCrcBytes);
}

uint32_t decode(ByteView frame, bool check_crc) {
  const uint64_t size = frame.size();
  if (size < kCrcBytes + 4 || size % 4 != 0 || size > kFrameBytes) return 0;
  const uint64_t n = size - kCrcBytes;
  if (check_crc) {
    const uint32_t crc = wire_crc(ConstByteView(frame.data(), n));
    const uint32_t stored = (static_cast<uint32_t>(frame[n]) << 24) | (static_cast<uint32_t>(frame[n + 1]) << 16) | (static_cast<uint32_t>(frame[n + 2]) << 8) | frame[n + 3];
    if (crc != stored) return 0;
  }
  for (uint64_t i = 0; i < n; i += 4) {
    const uint8_t a = frame[i], b = frame[i + 1], c = frame[i + 2], d = frame[i + 3];
    frame[i + 0] = d;
    frame[i + 1] = c;
    frame[i + 2] = b;
    frame[i + 3] = a;
  }
  return static_cast<uint32_t>(n);
}

}  // namespace tb_frame

bool TbControl::start() {
  stop();
  buffers_ = allocate_dma_view<uint8_t>(allocator_, static_cast<uint64_t>(kReceiveBuffers + kTransmitBuffers) * tb_frame::kFrameBytes, 256);
  if (buffers_.size() == 0) return false;
  buffers_.fill(0);
  // the transmit ring first: it acknowledges plug events
  if (!tx_.start(0, 0, 0) || !rx_.start(tb_frame::kFrameBytes, 0xFFFF, 0xFFFF)) {
    stop();
    return false;
  }
  rx_next_ = tx_next_ = tx_in_flight_ = 0;
  for (uint32_t i = 0; i < kReceiveBuffers; ++i)
    if (!rx_.post_receive(tb_bus_address(receive_buffer(i).data()))) {
      stop();
      return false;
    }
  pending_ = Pending();
  running_ = true;
  return true;
}

void TbControl::stop() {
  running_ = false;
  rx_.stop();
  tx_.stop();
  release_view(allocator_, buffers_);
  pending_ = Pending();
}

void TbControl::reap_transmit() {
  TbCompletion done;
  while (tx_in_flight_ != 0 && tx_.poll(&done)) --tx_in_flight_;
}

bool TbControl::send(TbPacketType type, ConstByteView body) {
  if (!running_) return false;
  reap_transmit();
  if (tx_in_flight_ >= kTransmitBuffers || tx_.free_slots() == 0) return false;
  ByteView buffer = transmit_buffer(tx_next_);
  const uint32_t bytes = tb_frame::encode(body, buffer);
  if (bytes == 0) return false;
  if (!tx_.post_transmit(tb_bus_address(buffer.data()), bytes, static_cast<uint8_t>(type), static_cast<uint8_t>(type))) return false;
  tx_next_ = (tx_next_ + 1) % kTransmitBuffers;
  ++tx_in_flight_;
  return true;
}

bool TbControl::matches(TbPacketType type, ConstByteView body) const {
  if (!pending_.active || pending_.done) return false;
  if (pending_.xdomain) {
    // the route (the top bit of the first word is set in answers) and the sequence number (bits 27 and 28 of the third word) are the request's
    return type == TbPacketType::XdomainResponse && body.size() >= 12 && (load_le32(body, 0) & 0x7FFFFFFFu) == (pending_.route >> 32) && load_le32(body, 4) == (pending_.route & 0xFFFFFFFFu) &&
           ((load_le32(body, 8) >> 27) & 3) == pending_.seq;
  }
  if (!pending_.config) {
    uint8_t code = 0;
    return type == TbPacketType::IcmResponse && body.try_get(0, &code) && code == pending_.code && body.size() >= pending_.expected_bytes;
  }
  if (route_of(body) != pending_.route) return false;
  if (type == TbPacketType::Error) return true;
  if (type != pending_.request_type || body.size() != pending_.expected_bytes) return false;
  // reads and writes carry the sequence number of the request (bits 27 and 28 of the address word)
  return ((load_le32(body, 8) >> 27) & 3) == pending_.seq;
}

void TbControl::complete(TbPacketType type, ConstByteView body) {
  pending_.done = true;
  pending_.result = Result::Ok;
  if (pending_.xdomain) {
    pending_.received = static_cast<uint32_t>(body.size());
    pending_.out.first(body.size() < pending_.out.size() ? body.size() : pending_.out.size()).copy_from(body.first(body.size() < pending_.out.size() ? body.size() : pending_.out.size()));
    return;
  }
  if (!pending_.config) {
    // packet `id` of `total` goes to its place; the request is done when all of them are there
    const uint32_t id = load_le32(body, 0) >> 16 & 0xFF, total = load_le32(body, 0) >> 24;
    const uint32_t packet_bytes = pending_.expected_bytes;
    if (total == 0 || total > pending_.packets || id >= total || (pending_.total != 0 && total != pending_.total) || (pending_.received_mask >> id & 1)) {
      pending_.done = false;                     // not one of ours after all
      return;
    }
    pending_.total = total;
    pending_.received_mask |= 1u << id;
    pending_.out.subview(static_cast<uint64_t>(id) * packet_bytes, packet_bytes).copy_from(body.first(packet_bytes));
    pending_.done = pending_.received_mask == (total == 32 ? ~0u : (1u << total) - 1);
    return;
  }
  if (type == TbPacketType::Error) {
    const uint32_t word = load_le32(body, 8);
    pending_.error.reported = true;
    pending_.error.code = static_cast<uint8_t>(word & 0xFF);
    pending_.error.port = static_cast<uint8_t>((word >> 8) & 0x3F);
    pending_.result = Result::Failed;
    return;
  }
  if ((load_le32(body, 0) >> 22) != kReplyMarker) {
    pending_.result = Result::Failed;
    return;
  }
  pending_.out.first(body.size()).copy_from(body);
}

void TbControl::handle(const TbCompletion& completion, IThunderboltEvents* events) {
  ByteView buffer = receive_buffer(rx_next_);
  rx_next_ = (rx_next_ + 1) % kReceiveBuffers;
  // the buffer goes back to the chip whatever happens to the frame
  struct Repost {
    TbRing& ring;
    ByteView buffer;
    ~Repost() { ring.post_receive(tb_bus_address(buffer.data())); }
  } repost{rx_, buffer};
  if ((completion.flags & (TbRing::kDescCrcError | TbRing::kDescOverrun)) || completion.length > tb_frame::kFrameBytes) return;
  if (completion.eof < static_cast<uint8_t>(TbPacketType::Read) || completion.eof > static_cast<uint8_t>(TbPacketType::IcmResponse)) return;
  const TbPacketType type = static_cast<TbPacketType>(completion.eof);
  const bool icm = type == TbPacketType::IcmEvent || type == TbPacketType::IcmResponse || type == TbPacketType::IcmCommand;
  ByteView frame = buffer.first(completion.length);
  const uint32_t body_bytes = tb_frame::decode(frame, !icm);
  if (body_bytes == 0) return;
  ConstByteView body(frame.data(), body_bytes);
  if (type == TbPacketType::Error) {
    const uint32_t word = load_le32(body, 8);
    if (async_error(static_cast<uint8_t>(word & 0xFF))) {
      if (events) events->on_event(type, body);
      return;
    }
  }
  switch (type) {
    case TbPacketType::Event:
    case TbPacketType::XdomainRequest:
    case TbPacketType::IcmEvent:
      if (events) events->on_event(type, body);
      return;
    case TbPacketType::XdomainResponse:
      // (a service's own messages come in as responses too: its handler first, then the request that waits)
      if (events && events->on_event(type, body)) return;
      break;
    default:
      break;
  }
  // (a packet that answers nothing that is waiting is a late reply: dropped)
  if (matches(type, body)) complete(type, body);
}

uint32_t TbControl::poll(IThunderboltEvents* events) {
  if (!running_) return 0;
  reap_transmit();
  uint32_t n = 0;
  TbCompletion completion;
  while (rx_.poll(&completion)) {
    handle(completion, events);
    ++n;
  }
  return n;
}

TbControl::Result TbControl::wait(uint32_t timeout_us, IThunderboltEvents* events) {
  for (uint32_t waited = 0;; waited += kPollStepUs) {
    poll(events);
    if (pending_.done) return pending_.result;
    if (waited >= timeout_us) return Result::Timeout;
    delay_us(kPollStepUs);
  }
}

TbControl::Result TbControl::xdomain_request(ConstByteView request, ByteView response, uint32_t* received, uint32_t timeout_us, IThunderboltEvents* events, TbPacketType send_as) {
  if (!running_) return Result::NotRunning;
  if (request.size() < 12 || request.size() % 4 != 0) return Result::Failed;
  const uint64_t route = (static_cast<uint64_t>(load_le32(request, 0)) << 32) | load_le32(request, 4);
  pending_ = Pending();
  pending_.active = true;
  pending_.xdomain = true;
  pending_.route = route & ~(1ull << 63);
  pending_.seq = static_cast<uint8_t>((load_le32(request, 8) >> 27) & 3);
  pending_.out = response;
  if (!send(send_as, request)) {
    pending_ = Pending();
    return Result::Failed;
  }
  const Result result = wait(timeout_us, events);
  if (received) *received = pending_.received;
  pending_ = Pending();
  return result;
}

TbControl::Result TbControl::icm_request(ConstByteView request, ByteView response, uint32_t timeout_us, IThunderboltEvents* events) {
  return icm_request_packets(request, response, static_cast<uint32_t>(response.size()), 1, timeout_us, events);
}

TbControl::Result TbControl::icm_request_packets(ConstByteView request, ByteView responses, uint32_t packet_bytes, uint32_t packets, uint32_t timeout_us, IThunderboltEvents* events,
                                                         uint32_t* total_packets) {
  if (!running_) return Result::NotRunning;
  uint8_t code = 0;
  if (!request.try_get(0, &code) || packets == 0 || packets > 32 || packet_bytes == 0 || packet_bytes > tb_frame::kMaxBody || responses.size() / packets < packet_bytes) return Result::Failed;
  pending_ = Pending();
  pending_.active = true;
  pending_.config = false;
  pending_.code = code;
  pending_.expected_bytes = packet_bytes;
  pending_.packets = packets;
  pending_.out = responses;
  if (!send(TbPacketType::IcmCommand, request)) {
    pending_ = Pending();
    return Result::Failed;
  }
  const Result result = wait(timeout_us, events);
  if (total_packets) *total_packets = pending_.total;
  pending_ = Pending();
  return result;
}

TbControl::Result TbControl::config_request(bool write, uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, uint32_t count, ArrayView<const uint32_t> data,
                                            ArrayView<uint32_t> out, uint32_t timeout_us, ConfigError* error, IThunderboltEvents* events) {
  if (!running_) return Result::NotRunning;
  ConfigError local;
  if (!error) error = &local;
  *error = ConfigError();
  if (count == 0 || count > kMaxConfigWords || offset > 0x1FFF || port > 0x3F || route >> 54 != 0 || (write && data.size() < count) || (!write && out.size() < count)) return Result::Failed;
  Result result = Result::Timeout;
  for (uint32_t attempt = 0; attempt < kConfigRetries && result == Result::Timeout; ++attempt) {
    uint8_t request[tb_frame::kMaxBody];
    ByteWriter writer{ByteView(request, sizeof request)};
    writer.le32(static_cast<uint32_t>(route >> 32));
    writer.le32(static_cast<uint32_t>(route));
    writer.le32(offset | (count << 13) | (port << 19) | (static_cast<uint32_t>(space) << 25) | (attempt << 27));
    if (write)
      for (uint32_t i = 0; i < count; ++i) writer.le32(data[i]);
    if (!writer.ok()) return Result::Failed;
    uint8_t reply[tb_frame::kMaxBody];
    pending_ = Pending();
    pending_.active = true;
    pending_.config = true;
    pending_.request_type = write ? TbPacketType::Write : TbPacketType::Read;
    pending_.route = route;
    pending_.seq = static_cast<uint8_t>(attempt);
    pending_.expected_bytes = write ? kConfigHeaderBytes : kConfigHeaderBytes + 4 * count;
    pending_.out = ByteView(reply, sizeof reply);
    if (!send(pending_.request_type, writer.written())) {
      pending_ = Pending();
      return Result::Failed;
    }
    result = wait(timeout_us, events);
    *error = pending_.error;
    pending_ = Pending();
    if (result != Result::Ok) continue;
    // the reply names the same words
    const uint32_t address = load_le32(ConstByteView(reply, sizeof reply), 8);
    if ((address & 0x1FFF) != offset || ((address >> 13) & 0x3F) != count || ((address >> 25) & 3) != static_cast<uint32_t>(space)) return Result::Failed;
    error->response_port = static_cast<uint8_t>((address >> 19) & 0x3F);
    if (!write)
      for (uint32_t i = 0; i < count; ++i) out[i] = load_le32(ConstByteView(reply, sizeof reply), kConfigHeaderBytes + 4ull * i);
  }
  return result;
}

TbControl::Result TbControl::read_config(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<uint32_t> words, uint32_t timeout_us, ConfigError* error,
                                         IThunderboltEvents* events) {
  return config_request(false, route, port, space, offset, static_cast<uint32_t>(words.size()), ArrayView<const uint32_t>(), words, timeout_us, error, events);
}

TbControl::Result TbControl::write_config(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<const uint32_t> words, uint32_t timeout_us, ConfigError* error,
                                          IThunderboltEvents* events) {
  return config_request(true, route, port, space, offset, static_cast<uint32_t>(words.size()), words, ArrayView<uint32_t>(), timeout_us, error, events);
}

}  // namespace hardware
