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

// The control channel of a Thunderbolt host interface (ctl.c of the Linux thunderbolt driver): hop 0 of the NHI, one TX and one RX ring of small frames. A frame is a packet type (in
// the descriptor's SOF / EOF), a body of whole 32 bit words, each sent most significant byte first, and a CRC-32C of those wire bytes (the complement of the running value, also
// big endian). The bodies in this API are in the layout of the Linux structures: the words little endian, as they sit in host memory. Over the channel go the router configuration
// space reads and writes, plug and error events, and the messages to and from the connection manager firmware (ICM): requests, responses and events.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltRing.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// tb_cfg_pkg_type
enum class TbPacketType : uint8_t {
  Read = 1,
  Write = 2,
  Error = 3,
  NotifyAck = 4,
  Event = 5,
  XdomainRequest = 6,
  XdomainResponse = 7,
  Override = 8,
  Reset = 9,
  IcmEvent = 10,
  IcmCommand = 11,
  IcmResponse = 12,
};

enum class TbConfigSpace : uint8_t { Hops = 0, Port = 1, Switch = 2, Counters = 3 };

namespace tb_frame {
constexpr uint32_t kFrameBytes = 256, kCrcBytes = 4, kMaxBody = kFrameBytes - kCrcBytes;
// Wire frame for a body: the words swapped to big endian and the CRC appended. `out` takes body.size() + 4 bytes; returns the frame's size, 0 if the body is not whole words or too long.
uint32_t encode(ConstByteView body, ByteView out);
// The inverse, in place: the words of `frame` go back to host order; returns the body's size (without the CRC), or 0 if the frame is not whole words, too short, or (when `check_crc`)
// its CRC does not match. The CRC bytes are left after the body.
uint32_t decode(ByteView frame, bool check_crc);
}  // namespace tb_frame

// Packets that nobody asked for: plug events, ICM events, XDomain requests.
class IThunderboltEvents {
 public:
  virtual void on_event(TbPacketType type, ConstByteView body) = 0;

 protected:
  constexpr IThunderboltEvents() = default;
  ~IThunderboltEvents() = default;
};

class TbControl {
 public:
  static constexpr uint32_t kRingSize = 10, kReceiveBuffers = 8, kTransmitBuffers = 8;
  enum class Result : uint8_t { Ok, Timeout, Failed, NotRunning };
  // cfg_error_pkg: the error the router reported when Result::Failed came from an error packet
  struct ConfigError {
    bool reported = false;
    uint8_t code = 0, port = 0;
  };

  TbControl(TbNhi& nhi, allocators::IAllocator& allocator) : nhi_(nhi), allocator_(allocator), tx_(nhi.registers(), allocator, 0, true, kRingSize), rx_(nhi.registers(), allocator, 0, false, kRingSize) {}
  ~TbControl() { stop(); }
  TbControl(const TbControl&) = delete;
  TbControl& operator=(const TbControl&) = delete;

  // Starts both rings and posts the receive buffers (tb_ctl_start).
  bool start();
  void stop();
  bool running() const { return running_; }

  // One packet out. The body is whole words, at most 252 bytes.
  bool send(TbPacketType type, ConstByteView body);
  // Reads what the chip finished: unasked packets go to `events` (dropped if null). Returns the number of packets received.
  uint32_t poll(IThunderboltEvents* events);

  // An ICM request: sent as an ICM command, answered by an ICM response with the same code (the first byte of the body). The response body is copied into `response`, which has to be
  // exactly as long as the response is expected to be. Events that arrive while waiting go to `events`.
  Result icm_request(ConstByteView request, ByteView response, uint32_t timeout_us, IThunderboltEvents* events);
  // The same for an answer in several packets (GET_TOPOLOGY): up to `packets` responses of `packet_bytes` each, the packet with id n copied to `responses` at n * packet_bytes. Done when
  // as many packets have come as the first of them says there are (they may arrive in any order); one that claims more than `packets`, or an id outside them, is not taken.
  Result icm_request_packets(ConstByteView request, ByteView responses, uint32_t packet_bytes, uint32_t packets, uint32_t timeout_us, IThunderboltEvents* events,
                              uint32_t* total_packets = nullptr);
  // Router configuration space (tb_cfg_read_raw / tb_cfg_write_raw): `words` 32 bit words at the word offset `offset` of `space` (of `port` for the port space) of the router at `route`.
  Result read_config(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<uint32_t> words, uint32_t timeout_us, ConfigError* error, IThunderboltEvents* events);
  Result write_config(uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, ArrayView<const uint32_t> words, uint32_t timeout_us, ConfigError* error, IThunderboltEvents* events);

 private:
  struct Pending {
    bool active = false, done = false;
    bool config = false;
    TbPacketType request_type = TbPacketType::Read;
    uint64_t route = 0;
    uint8_t seq = 0, code = 0;
    uint32_t expected_bytes = 0;
    uint32_t packets = 1, total = 0, received_mask = 0;     // ICM answers of several packets
    ByteView out;
    Result result = Result::Timeout;
    ConfigError error;
  };

  void reap_transmit();
  ByteView receive_buffer(uint32_t index) { return buffers_.subview(static_cast<uint64_t>(index) * tb_frame::kFrameBytes, tb_frame::kFrameBytes); }
  ByteView transmit_buffer(uint32_t index) { return buffers_.subview(static_cast<uint64_t>(kReceiveBuffers + index) * tb_frame::kFrameBytes, tb_frame::kFrameBytes); }
  void handle(const TbCompletion& completion, IThunderboltEvents* events);
  bool matches(TbPacketType type, ConstByteView body) const;
  void complete(TbPacketType type, ConstByteView body);
  Result config_request(bool write, uint64_t route, uint32_t port, TbConfigSpace space, uint32_t offset, uint32_t count, ArrayView<const uint32_t> data, ArrayView<uint32_t> out,
                        uint32_t timeout_us, ConfigError* error, IThunderboltEvents* events);
  Result wait(uint32_t timeout_us, IThunderboltEvents* events);

  TbNhi& nhi_;
  allocators::IAllocator& allocator_;
  TbRing tx_, rx_;
  ArrayView<uint8_t> buffers_;           // the receive buffers, then the transmit buffers
  bool running_ = false;
  uint32_t rx_next_ = 0, tx_next_ = 0, tx_in_flight_ = 0;
  Pending pending_;
};

}  // namespace hardware
