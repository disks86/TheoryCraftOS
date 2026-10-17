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
#include "Hardware/Bus/Usb/IUsbHostController.hpp"

namespace hardware {

// IEEE 1284.4 ("Dot4") multiplexing over a USB printer interface of protocol 3: one bulk pipe pair carries several logical
// channels (sockets), so a multifunction printer can print, answer status queries and scan at the same time. Every packet is a
// six-byte header - primary socket, secondary socket, length (big-endian, including the header), credit, control - and a payload.
// Socket 0 is the command channel: Init, GetSocketID (service name -> socket), OpenChannel, Credit, CreditRequest, CloseChannel,
// Exit and Error. A data packet may only be sent while the receiver has granted credit for its channel (one credit per packet), which
// is how a slow printer holds the host back.
//
// This is the host (primary) side with a single data channel - enough to print and to read the printer's replies. It follows the
// sequences HP's Linux driver uses with real printers: transactions are one at a time, commands carry transaction credit 1, the
// channel is opened with "unlimited credit" for the printer's replies, and the same socket number is used on both sides. All calls
// are synchronous (they wait for the printer's reply, bounded by the controller's transfer timeout) except that send() hands back
// what it could send when the printer has no credit to spare.
class Dot4Session {
 public:
  static constexpr uint8_t kRevision = 0x20;
  static constexpr uint8_t kCommandSocket = 0;
  // Command codes (the reply is the code with bit 7 set).
  enum Command : uint8_t { kInit = 0, kOpenChannel = 1, kCloseChannel = 2, kCredit = 3, kCreditRequest = 4, kExit = 8, kGetSocketId = 9, kGetServiceName = 10, kError = 0x7F };

  // `out_buffer` and `in_buffer` are the DMA-safe blocks of USB transfers (each within one 64 KiB window, at least 64 bytes).
  Dot4Session(IUsbHostController& host, uint32_t port, uint8_t out_endpoint, uint8_t in_endpoint, ByteView out_buffer, ByteView in_buffer)
      : host_(host), port_(port), out_ep_(out_endpoint), in_ep_(in_endpoint), out_(out_buffer), in_(in_buffer) {}

  // Init, then find the socket of `service` ("PRINT" for printing) and open a channel to it. False if the printer refuses or does not
  // answer.
  bool open(const char* service = "PRINT");
  bool is_open() const { return open_; }
  // Closes the channel and the session (best effort).
  void close();

  // Sends as much of `data` as the printer has credit for, in packets of at most max_packet() - 6 bytes. *sent receives the bytes
  // handed over (possibly 0: the printer has no credit just now - ask again later). False on a transfer error or a protocol failure.
  bool send(ConstByteView data, uint64_t* sent);
  // The next data the printer sent on the channel (one packet's worth) into `out`; grants the printer credit first if it has none.
  bool receive(ByteView out, uint32_t* length);

  uint8_t socket() const { return socket_; }
  uint16_t max_packet() const { return max_h2p_; }       // largest packet to the printer, header included
  uint16_t printer_max_packet() const { return max_p2h_; }
  uint32_t credit() const { return h2p_credit_; }
  uint32_t packets_sent() const { return packets_sent_; }

 private:
  struct Packet {
    uint8_t psid = 0, ssid = 0, credit = 0, control = 0;
    uint16_t length = 0;
    uint16_t payload_offset = 0;  // into packet_
  };
  bool write_packet(uint8_t psid, uint8_t ssid, uint8_t credit, ConstByteView payload);
  bool read_packet(Packet* p);
  // Sends a command and waits for its reply (handling other traffic meanwhile). The reply payload (after the command byte) is left in
  // reply_, its result byte at reply_[0].
  bool transact(uint8_t command, ConstByteView params);
  void absorb(const Packet& p);   // a data packet on the channel, or a command from the printer
  bool request_credit();

  IUsbHostController& host_;
  uint32_t port_;
  uint8_t out_ep_, in_ep_;
  ByteView out_, in_;
  uint8_t stream_[32768 + 64];  // bytes read from the pipe and not yet parsed
  uint32_t stream_length_ = 0;
  uint8_t data_[16384];         // data received on the channel, not yet handed out
  uint32_t data_length_ = 0;
  uint8_t packet_[8192 + 16];
  uint8_t reply_[64];
  uint32_t reply_length_ = 0;
  bool open_ = false, session_ = false, error_ = false;
  uint8_t socket_ = 0;
  uint16_t max_h2p_ = 0, max_p2h_ = 0;
  uint32_t h2p_credit_ = 0, p2h_credit_ = 0;
  uint32_t packets_sent_ = 0;
};

}  // namespace hardware
