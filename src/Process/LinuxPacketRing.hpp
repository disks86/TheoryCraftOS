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
#pragma once

#include <stdint.h>

#include "Utility/ArrayView.hpp"

// The memory-mapped rings of AF_PACKET sockets (PACKET_RX_RING, PACKET_TX_RING; Linux's Documentation/networking/packet_mmap.rst): memory a program and
// the kernel share, in which the kernel leaves received frames with a header each and the program leaves frames to send. Nothing here knows of sockets
// or mappings (LinuxPacket.cpp does): a ring is a run of bytes, and these put frames into it and take them out of it in one of the three layouts.
//
// The other side of that memory is a program, which may write anything there at any time: every offset used here is worked out here, a length a
// program wrote is checked against the room there is, and a status word is read and written in one piece.
namespace process::linuxabi::ring {

enum class Version : uint8_t { V1 = 0, V2 = 1, V3 = 2 };      // PACKET_VERSION: TPACKET_V1, TPACKET_V2, TPACKET_V3
constexpr uint32_t kNameSize = 20;                             // struct sockaddr_ll, which follows each received frame's header

// struct tpacket_req, and what struct tpacket_req3 adds to it.
struct Request {
  uint32_t block_size = 0, block_count = 0, frame_size = 0, frame_count = 0;
  uint32_t retire_ms = 0, private_size = 0, features = 0;
  uint64_t bytes() const { return uint64_t{block_size} * block_count; }
};

// The size of a frame's header in a layout (what PACKET_HDRLEN answers); 0 for a version there is not.
uint32_t header_size(uint32_t version);
// May a ring be made as `request` asks? (Linux's rules: whole pages per block, frames that hold at least their header, a frame count that is what the
// blocks hold.) `reserve` is PACKET_RESERVE.
[[nodiscard]] bool valid(Version version, const Request& request, uint32_t reserve);

// A received frame, to be put into a ring.
struct Frame {
  ConstByteView link;          // its link header, if the socket hands those over (SOCK_RAW); empty otherwise
  ConstByteView data;          // what follows the link header
  uint32_t kept = 0;           // how many bytes of link and data together the socket's filter wants
  ConstByteView name;          // its struct sockaddr_ll (kNameSize bytes)
  uint64_t time_ns = 0;        // when it came (since 1970)
};

// The receiving side of a ring: where the kernel is in it.
class Receiver {
 public:
  void start(Version version, const Request& request, uint32_t reserve);
  // Puts a frame into the ring. False if there is no room (the program has not given back the place it would go): the frame is lost.
  bool put(ByteView ring, const Frame& frame, uint64_t now_ns);
  // TPACKET_V3: hands the block being filled to the program if it has waited long enough with frames in it.
  void tick(ByteView ring, uint64_t now_ns);
  // Is there something in the ring the program has not given back yet? (What poll() reports.)
  bool readable(ConstByteView ring) const;
  uint32_t frozen() const { return frozen_; }      // TPACKET_V3: how often frames had nowhere to go (tp_freeze_q_cnt)

 private:
  void open_block(ByteView ring, uint64_t now_ns);
  void close_block(ByteView ring, uint32_t status);

  Version version_ = Version::V1;
  Request request_;
  uint32_t reserve_ = 0;
  uint32_t head_ = 0;              // the frame (V1, V2) or block (V3) to fill next
  // TPACKET_V3: the block being filled
  bool open_ = false;
  uint32_t used_ = 0, packets_ = 0, last_ = 0;      // bytes used in it, frames in it, where the last frame's header is
  uint64_t opened_ns_ = 0, first_ns_ = 0, last_ns_ = 0, sequence_ = 0;
  uint32_t frozen_ = 0;
  bool was_frozen_ = false;
};

// The sending side: frames a program left to be sent.
class Transmitter {
 public:
  void start(Version version, const Request& request) { version_ = version; request_ = request; head_ = 0; }
  enum class Wrong : uint8_t { No, TooLong, Malformed };
  // Takes the next frame the program asked to have sent: its bytes in `out` (a view into the ring, which the program may fill again from now on:
  // copy it at once), of at most `most` bytes. False if the frame at the head is not one to send. One that makes no sense (*wrong says how) is
  // marked so and stays at the head, as on Linux, or with `skip_wrong` (PACKET_LOSS) is passed over. `use_offset`: the header says where the frame
  // starts (PACKET_TX_HAS_OFF); otherwise it follows the header.
  bool next(ByteView ring, bool use_offset, uint32_t most, bool skip_wrong, ConstByteView* out, Wrong* wrong);

 private:
  Version version_ = Version::V1;
  Request request_;
  uint32_t head_ = 0;
};

}  // namespace process::linuxabi::ring
