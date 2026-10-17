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

#include "Hardware/Printer/Usb/Dot4.hpp"

namespace hardware {

namespace {
constexpr uint32_t kHeader = 6;
constexpr uint16_t kOurMaxPacket = 4096;  // offered both ways; the printer may lower it
constexpr uint32_t kReadAttempts = 8;      // empty transfers tolerated while waiting for a packet

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
}  // namespace

bool Dot4Session::write_packet(uint8_t psid, uint8_t ssid, uint8_t credit, ConstByteView payload) {
  const uint64_t total = kHeader + payload.size();
  if (total > out_.size() || total > 0xFFFF) return false;
  out_[0] = psid;
  out_[1] = ssid;
  put16(&out_[2], static_cast<uint16_t>(total));
  out_[4] = credit;
  out_[5] = 0;
  if (!payload.empty()) out_.subview(kHeader, payload.size()).copy_from(payload);
  uint32_t moved = 0;
  return host_.bulk_transfer(port_, out_ep_, out_.first(total), &moved) && moved == total;
}

// Takes the next whole packet out of the byte stream, reading from the pipe as needed. Its bytes are copied to packet_.
bool Dot4Session::read_packet(Packet* p) {
  for (uint32_t attempt = 0; attempt <= kReadAttempts;) {
    if (stream_length_ >= kHeader) {
      const uint16_t length = get16(&stream_[2]);
      if (length < kHeader || length > sizeof packet_) return false;  // not a packet: the stream is lost
      if (stream_length_ >= length) {
        p->psid = stream_[0];
        p->ssid = stream_[1];
        p->length = length;
        p->credit = stream_[4];
        p->control = stream_[5];
        for (uint32_t i = 0; i < length; ++i) packet_[i] = stream_[i];
        for (uint32_t i = length; i < stream_length_; ++i) stream_[i - length] = stream_[i];
        stream_length_ -= length;
        return true;
      }
    }
    uint32_t moved = 0;
    if (in_.size() > sizeof stream_ - stream_length_) return false;  // (cannot happen with the buffers the driver gives)
    if (!host_.bulk_transfer(port_, in_ep_, in_, &moved)) return false;
    if (moved == 0) {
      ++attempt;
      continue;
    }
    if (moved > in_.size() || stream_length_ + moved > sizeof stream_) return false;
    for (uint32_t i = 0; i < moved; ++i) stream_[stream_length_ + i] = in_[i];
    stream_length_ += moved;
  }
  return false;
}

// A packet that is not the awaited reply: data for our channel is kept, commands from the printer are answered.
void Dot4Session::absorb(const Packet& p) {
  const uint8_t* payload = packet_ + kHeader;
  const uint32_t size = p.length - kHeader;
  if (p.psid == kCommandSocket && p.ssid == kCommandSocket) {
    if (size < 1) return;
    const uint8_t command = payload[0];
    uint8_t reply[8];
    uint32_t n = 0;
    if (command == kCredit && size >= 5) {          // the printer grants us credit
      if (payload[1] == socket_ && payload[2] == socket_) h2p_credit_ += get16(payload + 3);
      reply[n++] = kCredit | 0x80;
      reply[n++] = 0;
      reply[n++] = payload[1];
      reply[n++] = payload[2];
    } else if (command == kCreditRequest && size >= 5) {  // it asks for credit for its replies: give a little
      reply[n++] = kCreditRequest | 0x80;
      reply[n++] = 0;
      reply[n++] = payload[1];
      reply[n++] = payload[2];
      put16(reply + n, 0);
      n += 2;
    } else if (command == kError) {
      error_ = true;  // the printer reports a protocol error: the session is over
      return;
    } else {
      reply[n++] = static_cast<uint8_t>(command | 0x80);  // a command we do not know: "unsupported"
      reply[n++] = 1;
    }
    write_packet(kCommandSocket, kCommandSocket, 1, ConstByteView(reply, n));
    return;
  }
  if (p.psid == socket_ && p.ssid == socket_) {
    h2p_credit_ += p.credit;  // piggy-backed credit
    if (p2h_credit_ > 0) --p2h_credit_;
    if (data_length_ + size <= sizeof data_) {
      for (uint32_t i = 0; i < size; ++i) data_[data_length_ + i] = payload[i];
      data_length_ += size;
    }
  }
}

bool Dot4Session::transact(uint8_t command, ConstByteView params) {
  uint8_t body[64];
  if (params.size() + 1 > sizeof body) return false;
  body[0] = command;
  for (uint64_t i = 0; i < params.size(); ++i) body[1 + i] = params[i];
  if (!write_packet(kCommandSocket, kCommandSocket, 1, ConstByteView(body, params.size() + 1))) return false;
  for (int guard = 0; guard < 16; ++guard) {
    Packet p;
    if (!read_packet(&p)) return false;
    const uint8_t* payload = packet_ + kHeader;
    const uint32_t size = p.length - kHeader;
    if (p.psid == kCommandSocket && p.ssid == kCommandSocket && size >= 2 && payload[0] == (command | 0x80)) {
      reply_length_ = size - 1 < sizeof reply_ ? size - 1 : sizeof reply_;
      for (uint32_t i = 0; i < reply_length_; ++i) reply_[i] = payload[1 + i];
      return reply_length_ >= 1 && reply_[0] == 0;  // the result code
    }
    absorb(p);
    if (error_) return false;
  }
  return false;
}

bool Dot4Session::open(const char* service) {
  if (session_) return open_;
  error_ = false;
  stream_length_ = data_length_ = 0;
  h2p_credit_ = p2h_credit_ = 0;
  const uint8_t revision = kRevision;
  if (!transact(kInit, ConstByteView(&revision, 1))) return false;
  session_ = true;
  uint32_t n = 0;
  uint8_t name[40];
  while (service[n] && n < sizeof name) {
    name[n] = static_cast<uint8_t>(service[n]);
    ++n;
  }
  if (!transact(kGetSocketId, ConstByteView(name, n)) || reply_length_ < 2) {
    close();
    return false;
  }
  socket_ = reply_[1];
  if (socket_ == 0) {
    close();
    return false;
  }
  uint8_t params[8];
  params[0] = socket_;
  params[1] = socket_;
  put16(params + 2, kOurMaxPacket);
  put16(params + 4, kOurMaxPacket);
  put16(params + 6, 0xFFFF);  // "unlimited credit" for the printer's replies
  if (!transact(kOpenChannel, ConstByteView(params, 8)) || reply_length_ < 11) {
    close();
    return false;
  }
  // reply_: result, psocket, ssocket, maxp2s(2), maxs2p(2), maxcredit(2), credit(2)
  max_h2p_ = get16(reply_ + 3);
  max_p2h_ = get16(reply_ + 5);
  h2p_credit_ = get16(reply_ + 9);
  if (max_h2p_ > kOurMaxPacket) max_h2p_ = kOurMaxPacket;
  if (max_h2p_ <= kHeader) {
    close();
    return false;
  }
  open_ = true;
  return true;
}

void Dot4Session::close() {
  if (!session_) return;
  if (open_) {
    const uint8_t params[2] = {socket_, socket_};
    transact(kCloseChannel, ConstByteView(params, 2));
    open_ = false;
  }
  transact(kExit, ConstByteView());
  session_ = false;
}

bool Dot4Session::request_credit() {
  uint8_t params[4] = {socket_, socket_, 0xFF, 0xFF};
  if (!transact(kCreditRequest, ConstByteView(params, 4)) || reply_length_ < 5) return false;
  h2p_credit_ += get16(reply_ + 3);
  return true;
}

bool Dot4Session::send(ConstByteView data, uint64_t* sent) {
  *sent = 0;
  if (!open_ || error_) return false;
  const uint32_t limit = max_h2p_ - kHeader;
  while (*sent < data.size()) {
    if (h2p_credit_ == 0) {
      if (!request_credit()) return false;
      if (h2p_credit_ == 0) return open_;  // the printer has no room right now
    }
    const uint64_t left = data.size() - *sent;
    const uint32_t n = left < limit ? static_cast<uint32_t>(left) : limit;
    if (!write_packet(socket_, socket_, 0, data.subview(*sent, n))) return false;
    --h2p_credit_;
    *sent += n;
    ++packets_sent_;
  }
  return true;
}

bool Dot4Session::receive(ByteView out, uint32_t* length) {
  *length = 0;
  if (!open_) return false;
  if (data_length_ == 0) {
    if (p2h_credit_ == 0) {
      uint8_t params[4] = {socket_, socket_, 0, 1};
      if (!transact(kCredit, ConstByteView(params, 4))) return false;
      p2h_credit_ += 1;
    }
    for (int guard = 0; guard < 16 && data_length_ == 0; ++guard) {
      Packet p;
      if (!read_packet(&p)) return false;
      absorb(p);
    }
  }
  const uint32_t n = data_length_ < out.size() ? data_length_ : static_cast<uint32_t>(out.size());
  for (uint32_t i = 0; i < n; ++i) out[i] = data_[i];
  for (uint32_t i = n; i < data_length_; ++i) data_[i - n] = data_[i];
  data_length_ -= n;
  *length = n;
  return true;
}

}  // namespace hardware
