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

#include "Hardware/Bus/Thunderbolt/ThunderboltNet.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace tb_ip {

// (tbnet_svc_uuid: 798f589e-3616-8a47-97c6-5664a920c8dd)
const uint8_t kServiceUuid[16] = {0x79, 0x8F, 0x58, 0x9E, 0x36, 0x16, 0x8A, 0x47, 0x97, 0xC6, 0x56, 0x64, 0xA9, 0x20, 0xC8, 0xDD};

namespace {
constexpr uint32_t kLengthMask = 0x3F, kSequenceMask = 3u << 27, kSequenceShift = 27;

void header(ByteWriter& w, uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t type, uint32_t size, uint32_t command_id) {
  w.le32(static_cast<uint32_t>(route >> 32));
  w.le32(static_cast<uint32_t>(route));
  w.le32(((size - 12) / 4 & kLengthMask) | ((static_cast<uint32_t>(sequence) << kSequenceShift) & kSequenceMask));
  w.bytes(ConstByteView(kServiceUuid, 16));
  w.bytes(ConstByteView(initiator, 16));
  w.bytes(ConstByteView(target, 16));
  w.le32(type);
  w.le32(command_id);
}
uint32_t finish(const ByteWriter& w) { return w.ok() ? static_cast<uint32_t>(w.position()) : 0; }
}  // namespace

bool parse_header(ConstByteView packet, Header* out) {
  if (packet.size() < kHeaderBytes || packet.size() % 4 != 0) return false;
  ByteReader r(packet);
  const uint32_t hi = r.le32(), lo = r.le32(), length_sn = r.le32();
  uint8_t uuid[16];
  Header h;
  const bool a = r.read_into(ByteView(uuid, 16)), b = r.read_into(ByteView(h.initiator, 16)), c = r.read_into(ByteView(h.target, 16));
  h.type = r.le32();
  h.command_id = r.le32();
  if (!r.ok() || !a || !b || !c) return false;
  if (12 + 4ull * (length_sn & kLengthMask) > packet.size()) return false;
  for (uint32_t i = 0; i < 16; ++i)
    if (uuid[i] != kServiceUuid[i]) return false;
  h.route = ((static_cast<uint64_t>(hi) << 32) | lo) & ~(1ull << 63);
  h.sequence = static_cast<uint8_t>((length_sn & kSequenceMask) >> kSequenceShift);
  *out = h;
  return true;
}

uint32_t build_login(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, uint32_t transmit_path, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, initiator, target, kLogin, kLoginBytes, command_id);
  w.le32(kProtoVersion);
  w.le32(transmit_path);
  w.zeros(16);
  return finish(w);
}

uint32_t build_login_response(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, const uint8_t mac[6], ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, initiator, target, kLoginResponse, kLoginResponseBytes, command_id);
  w.le32(0);                                          // status
  w.bytes(ConstByteView(mac, 6));
  w.zeros(2);
  w.le32(6);                                          // the address's length
  w.zeros(16);
  return finish(w);
}

uint32_t build_logout(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, initiator, target, kLogout, kLogoutBytes, command_id);
  return finish(w);
}

uint32_t build_status(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, uint32_t status, ByteView out) {
  ByteWriter w(out);
  header(w, route, sequence, initiator, target, kStatus, kStatusBytes, command_id);
  w.le32(status);
  return finish(w);
}

bool parse_login(ConstByteView packet, uint32_t* proto_version, uint32_t* transmit_path) {
  if (packet.size() < kLoginBytes) return false;
  ByteReader r(packet);
  r.skip(kHeaderBytes);
  const uint32_t version = r.le32(), path = r.le32();
  if (!r.ok()) return false;
  *proto_version = version;
  *transmit_path = path;
  return true;
}

bool parse_login_response(ConstByteView packet, uint32_t* status, uint8_t mac[6]) {
  if (packet.size() < kLoginResponseBytes) return false;
  ByteReader r(packet);
  r.skip(kHeaderBytes);
  const uint32_t s = r.le32();
  uint8_t m[6];
  const bool got = r.read_into(ByteView(m, 6));
  r.skip(2);
  const uint32_t length = r.le32();
  if (!r.ok() || !got || length != 6) return false;
  *status = s;
  for (uint32_t i = 0; i < 6; ++i) mac[i] = m[i];
  return true;
}

}  // namespace tb_ip

namespace {
constexpr uint64_t kQueueBytes = 128 * 1024;
bool uuid_same(const uint8_t* a, const uint8_t* b) {
  bool same = true;
  for (uint32_t i = 0; i < 16; ++i) same = same && a[i] == b[i];
  return same;
}
}  // namespace

bool ThunderboltNet::initialize() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (initialized_) return true;
  uint8_t mac[6];
  if (!random_.fill(ByteView(mac, 6))) return false;
  mac[0] = static_cast<uint8_t>((mac[0] & 0xFE) | 0x02);            // a unicast address that belongs to nobody in particular
  for (uint32_t i = 0; i < 6; ++i) mac_.bytes[i] = mac[i];
  queue_ = allocate_view<uint8_t>(allocator_, kQueueBytes);
  if (queue_.size() == 0) return false;
  queue_used_ = 0;
  initialized_ = true;
  icm_.set_ip_handler(this);
  return true;
}

void ThunderboltNet::shutdown() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (!initialized_) return;
  close(true);
  icm_.set_ip_handler(nullptr);
  release_view(allocator_, queue_);
  initialized_ = false;
}

bool ThunderboltNet::find_host(uint64_t* index) {
  for (uint64_t i = 0; i < icm_.xdomain_count(); ++i) {
    const TbXDomain& x = icm_.xdomain(i);
    if (x.discovered && x.has_network) {
      *index = i;
      return true;
    }
  }
  return false;
}

bool ThunderboltNet::open(const TbXDomain& xd) {
  for (uint32_t i = 0; i < 16; ++i) {
    remote_uuid_[i] = xd.remote_uuid[i];
    local_uuid_[i] = xd.local_uuid[i];
  }
  route_ = xd.route;
  peer_flags_ = xd.network_flags;
  TbNhi& nhi = icm_.nhi();
  tx_hop_ = nhi.allocate_hop(true);
  rx_hop_ = nhi.allocate_hop(false);
  if (tx_hop_ < 0 || rx_hop_ < 0) {
    if (tx_hop_ >= 0) nhi.release_hop(true, static_cast<uint32_t>(tx_hop_));
    if (rx_hop_ >= 0) nhi.release_hop(false, static_cast<uint32_t>(rx_hop_));
    tx_hop_ = rx_hop_ = -1;
    return false;
  }
  tx_ring_ = UniquePointer<TbRing>::make(allocator_, nhi.registers(), allocator_, static_cast<uint32_t>(tx_hop_), true, tb_ip::kRingSize);
  rx_ring_ = UniquePointer<TbRing>::make(allocator_, nhi.registers(), allocator_, static_cast<uint32_t>(rx_hop_), false, tb_ip::kRingSize);
  if (!tx_ring_ || !rx_ring_) {
    close(false);
    return false;
  }
  login_sent_ = login_received_ = rings_ready_ = false;
  login_tries_ = 0;
  wait_polls_ = 0;
  remote_path_ = 0;
  state_ = State::LoggingIn;
  return true;
}

void ThunderboltNet::close(bool logout) {
  if (state_ == State::Idle && !tx_ring_ && !rx_ring_) return;
  uint64_t index = icm_.xdomain_count();
  for (uint64_t i = 0; i < icm_.xdomain_count(); ++i)
    if (uuid_same(icm_.xdomain(i).remote_uuid, remote_uuid_)) index = i;
  if (login_sent_ && login_received_) {
    if (logout && index < icm_.xdomain_count()) {
      // (a few tries; the other host may already be gone)
      uint8_t request[tb_ip::kLogoutBytes], response[tb_frame::kFrameBytes];
      for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const uint32_t n = tb_ip::build_logout(route_, 0, local_uuid_, remote_uuid_, ++command_id_, ByteView(request, sizeof request));
        uint32_t got = 0;
        if (n != 0 && icm_.control().xdomain_request(ConstByteView(request, n), ByteView(response, sizeof response), &got, 200000, icm_.event_sink(), TbPacketType::XdomainResponse) != TbControl::Result::Timeout) break;
      }
    }
    // the path first, then the rings: the chip may still be moving frames
    if (index < icm_.xdomain_count()) icm_.disconnect_xdomain_paths(index);
  }
  if (rx_hop_ >= 0) icm_.ring_interrupts(false, static_cast<uint32_t>(rx_hop_), false);
  if (tx_hop_ >= 0) icm_.ring_interrupts(true, static_cast<uint32_t>(tx_hop_), false);
  if (rx_ring_) rx_ring_->stop();
  if (tx_ring_) tx_ring_->stop();
  rx_ring_ = UniquePointer<TbRing>();
  tx_ring_ = UniquePointer<TbRing>();
  release_view(allocator_, rx_buffers_);
  release_view(allocator_, tx_buffers_);
  if (tx_hop_ >= 0) icm_.nhi().release_hop(true, static_cast<uint32_t>(tx_hop_));
  if (rx_hop_ >= 0) icm_.nhi().release_hop(false, static_cast<uint32_t>(rx_hop_));
  tx_hop_ = rx_hop_ = -1;
  tx_next_ = tx_in_flight_ = rx_next_ = 0;
  login_sent_ = login_received_ = rings_ready_ = false;
  packet_.clear();
  packet_frames_ = 0;
  queue_used_ = 0;
  state_ = State::Idle;
}

void ThunderboltNet::poll() {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (!initialized_) return;
  if (state_ == State::Idle) {
    uint64_t index = 0;
    if (!find_host(&index)) return;
    open(icm_.xdomain(index));
    return;
  }
  // the host going away ends the connection (nothing to log out from)
  bool present = false;
  for (uint64_t i = 0; i < icm_.xdomain_count(); ++i) present = present || uuid_same(icm_.xdomain(i).remote_uuid, remote_uuid_);
  if (!present || disconnect_pending_) {
    disconnect_pending_ = false;
    close(false);
    return;
  }
  if (state_ == State::LoggingIn) {
    if (!login_sent_) {
      if (wait_polls_ > 0) --wait_polls_;
      else if (login_tries_ < config_.login_retries) try_login();
    }
    if (login_sent_ && login_received_) try_connect();
  } else {
    receive_frames();
    drain_transmit();
  }
}

void ThunderboltNet::try_login() {
  uint8_t request[tb_ip::kLoginBytes], response[tb_frame::kFrameBytes];
  const uint32_t n = tb_ip::build_login(route_, static_cast<uint8_t>(login_tries_ % 4), local_uuid_, remote_uuid_, ++command_id_, local_path_, ByteView(request, sizeof request));
  uint32_t got = 0;
  bool ok = n != 0 && icm_.control().xdomain_request(ConstByteView(request, n), ByteView(response, sizeof response), &got, config_.login_timeout_us, icm_.event_sink(), TbPacketType::XdomainResponse) ==
                          TbControl::Result::Ok;
  if (ok) {
    const ConstByteView packet(response, got < sizeof response ? got : sizeof response);
    tb_ip::Header h;
    uint32_t status = 0;
    // (the answer comes from the other host to us)
    ok = tb_ip::parse_header(packet, &h) && h.type == tb_ip::kLoginResponse && uuid_same(h.initiator, remote_uuid_) && uuid_same(h.target, local_uuid_) && tb_ip::parse_login_response(packet, &status, peer_mac_);
  }
  if (ok) {
    login_tries_ = 0;
    login_sent_ = true;
  } else {
    ++login_tries_;
    wait_polls_ = config_.login_delay_polls;
  }
}

bool ThunderboltNet::handle_ip_packet(ConstByteView body) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (state_ == State::Idle) return false;
  tb_ip::Header h;
  if (!tb_ip::parse_header(body, &h)) return false;
  // from the host we talk to, to us, on the route we know it by
  if (!uuid_same(h.initiator, remote_uuid_) || !uuid_same(h.target, local_uuid_) || h.route != route_) return false;
  uint8_t out[tb_ip::kLoginResponseBytes];
  if (h.type == tb_ip::kLogin) {
    uint32_t version = 0, path = 0;
    if (!tb_ip::parse_login(body, &version, &path)) return true;
    const uint32_t n = tb_ip::build_login_response(route_, h.sequence, local_uuid_, remote_uuid_, h.command_id, mac_.bytes, ByteView(out, sizeof out));
    if (n != 0 && respond(ConstByteView(out, n))) {
      login_received_ = true;
      remote_path_ = path;
      // (a login after the tries ran out, or after a logout, starts the tries again)
      if (login_tries_ >= config_.login_retries || !login_sent_) {
        login_tries_ = 0;
        wait_polls_ = 0;
      }
    }
    return true;
  }
  if (h.type == tb_ip::kLogout) {
    const uint32_t n = tb_ip::build_status(route_, h.sequence, local_uuid_, remote_uuid_, ++command_id_, 0, ByteView(out, sizeof out));
    if (n != 0) respond(ConstByteView(out, n));
    disconnect_pending_ = true;
    return true;
  }
  return false;
}

bool ThunderboltNet::respond(ConstByteView packet) { return icm_.control().send(TbPacketType::XdomainResponse, packet); }

void ThunderboltNet::try_connect() {
  uint64_t index = icm_.xdomain_count();
  for (uint64_t i = 0; i < icm_.xdomain_count(); ++i)
    if (uuid_same(icm_.xdomain(i).remote_uuid, remote_uuid_)) index = i;
  if (index >= icm_.xdomain_count()) return;
  const bool e2e = config_.e2e && (peer_flags_ & tb_ip::kFlagE2e) != 0;
  TbRing::Mode tx_mode, rx_mode;
  tx_mode.frame = rx_mode.frame = true;
  tx_mode.e2e = rx_mode.e2e = e2e;
  rx_mode.e2e_tx_hop = static_cast<uint32_t>(tx_hop_);
  auto fail = [&]() {
    icm_.ring_interrupts(false, static_cast<uint32_t>(rx_hop_), false);
    icm_.ring_interrupts(true, static_cast<uint32_t>(tx_hop_), false);
    rx_ring_->stop();
    tx_ring_->stop();
    release_view(allocator_, rx_buffers_);
    release_view(allocator_, tx_buffers_);
    login_sent_ = false;                       // (what the other host sent stays: only it can send it again)
    login_tries_ = 0;
    wait_polls_ = config_.login_delay_polls;
  };
  // the rings first, with every receive buffer posted before a frame can arrive; the path last
  tx_buffers_ = allocate_dma_view<uint8_t>(allocator_, static_cast<uint64_t>(tb_ip::kRingSize) * tb_ip::kFrameBytes, 4096);
  rx_buffers_ = allocate_dma_view<uint8_t>(allocator_, static_cast<uint64_t>(tb_ip::kRingSize) * tb_ip::kFrameBytes, 4096);
  if (tx_buffers_.size() == 0 || rx_buffers_.size() == 0 || !tx_ring_->start(tx_mode, 0, 0) || !rx_ring_->start(rx_mode, 1u << tb_ip::kPdfFrameStart, 1u << tb_ip::kPdfFrameEnd)) {
    fail();
    return;
  }
  rx_next_ = tx_next_ = tx_in_flight_ = 0;
  for (uint32_t i = 0; i + 1 < tb_ip::kRingSize; ++i)
    if (!rx_ring_->post_receive(tb_bus_address(rx_buffers_.subview(static_cast<uint64_t>(i) * tb_ip::kFrameBytes, tb_ip::kFrameBytes).data()))) {
      fail();
      return;
    }
  icm_.ring_interrupts(true, static_cast<uint32_t>(tx_hop_), true);          // (the data rings raise the same interrupt as the control channel, when it is in use)
  icm_.ring_interrupts(false, static_cast<uint32_t>(rx_hop_), true);
  const char* error = nullptr;
  if (!icm_.approve_xdomain_paths(index, static_cast<uint16_t>(local_path_), static_cast<uint16_t>(tx_hop_), static_cast<uint16_t>(remote_path_), static_cast<uint16_t>(rx_hop_), &error)) {
    fail();
    return;
  }
  rings_ready_ = true;
  state_ = State::Connected;
}

void ThunderboltNet::drain_transmit() {
  TbCompletion done;
  while (tx_in_flight_ != 0 && tx_ring_->poll(&done)) --tx_in_flight_;
}

bool ThunderboltNet::queue_packet(ConstByteView packet) {
  if (packet.size() < kEthernetHeaderSize || queue_used_ + 4 + packet.size() > queue_.size()) {
    ++stats_.frames_dropped;
    return false;
  }
  ByteWriter w(queue_.subview(queue_used_));
  w.le32(static_cast<uint32_t>(packet.size()));
  w.bytes(packet);
  queue_used_ += 4 + packet.size();
  return true;
}

// One frame of the receive ring (tbnet_check_frame and tbnet_poll): the header says how big this frame's data is and where it belongs in the packet.
bool ThunderboltNet::take_frame(const uint8_t* frame, uint32_t size) {
  if (size <= tb_ip::kFrameHeaderBytes) return false;
  ByteReader r(ConstByteView(frame, size));
  const uint32_t frame_size = r.le32();
  const uint32_t frame_index = r.le16(), frame_id = r.le16(), frame_count = r.le32();
  if (!r.ok() || frame_size == 0 || frame_size > size - tb_ip::kFrameHeaderBytes) return false;
  if (packet_frames_ != 0) {
    // the next frame of the packet that is being put together
    if (frame_count != packet_frames_ || frame_index != packet_index_ + 1 || frame_id != packet_id_ || packet_.size() + frame_size > tb_ip::kMaxPacket) return false;
  } else if (frame_count == 0 || frame_count > tb_ip::kMaxFramesPerPacket || frame_index != 0) {
    return false;
  }
  if (!packet_.append(ConstByteView(frame + tb_ip::kFrameHeaderBytes, frame_size))) return false;
  packet_frames_ = frame_count;
  packet_index_ = frame_index;
  packet_id_ = frame_id;
  if (frame_index == frame_count - 1) {
    ++stats_.frames_received;
    queue_packet(packet_.view());
    packet_.clear();
    packet_frames_ = 0;
  }
  return true;
}

void ThunderboltNet::receive_frames() {
  if (!rx_ring_ || !rx_ring_->running()) return;
  TbCompletion c;
  for (uint32_t budget = 0; budget < tb_ip::kRingSize && rx_ring_->poll(&c); ++budget) {
    ByteView buffer = rx_buffers_.subview(static_cast<uint64_t>(rx_next_) * tb_ip::kFrameBytes, tb_ip::kFrameBytes);
    rx_next_ = (rx_next_ + 1) % (tb_ip::kRingSize - 1);          // (one buffer fewer than the ring has slots: the buffers go round in the order they were posted)
    const bool bad = (c.flags & (TbRing::kDescCrcError | TbRing::kDescOverrun)) != 0;
    // (the overrun flag shares the bit of "posted": a completed descriptor with it set is an overrun)
    if (bad || !take_frame(buffer.data(), c.length)) {
      ++stats_.frames_dropped;
      packet_.clear();
      packet_frames_ = 0;
    }
    rx_ring_->post_receive(tb_bus_address(buffer.data()));
  }
}

bool ThunderboltNet::transmit(ConstByteView frame) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (state_ != State::Connected || frame.empty() || frame.size() > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  drain_transmit();
  if (tx_in_flight_ + 1 >= tb_ip::kRingSize) {
    ++stats_.send_failures;
    return false;
  }
  const uint32_t length = static_cast<uint32_t>(frame.size() < kEthernetMinFrame ? kEthernetMinFrame : frame.size());
  ByteView buffer = tx_buffers_.subview(static_cast<uint64_t>(tx_next_) * tb_ip::kFrameBytes, tb_ip::kFrameBytes);
  ByteWriter w(buffer);
  w.le32(length);
  w.le16(0);                                                         // the index of the frame in the packet
  w.le16((peer_flags_ & tb_ip::kFlagMatchFragsId) ? frame_id_ : 0);
  w.le32(1);                                                         // how many frames the packet has
  w.bytes(frame);
  w.zeros(length - frame.size());
  if (!w.ok() || !tx_ring_->post_transmit(tb_bus_address(buffer.data()), tb_ip::kFrameHeaderBytes + length, tb_ip::kPdfFrameStart, tb_ip::kPdfFrameEnd)) {
    ++stats_.send_failures;
    return false;
  }
  tx_next_ = (tx_next_ + 1) % tb_ip::kRingSize;
  ++tx_in_flight_;
  if (peer_flags_ & tb_ip::kFlagMatchFragsId) ++frame_id_;
  ++stats_.frames_sent;
  return true;
}

bool ThunderboltNet::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedRecursiveMutex guard(mutex_);
  if (state_ == State::Connected) receive_frames();
  if (queue_used_ < 4) return false;
  ByteReader r(ConstByteView(queue_.data(), queue_used_));
  const uint32_t size = r.le32();
  if (!r.ok() || 4ull + size > queue_used_) {                         // (cannot happen: only whole packets are queued)
    queue_used_ = 0;
    return false;
  }
  const uint64_t copy = size < buffer.size() ? size : buffer.size();
  if (copy != 0) buffer.first(copy).copy_from(ConstByteView(queue_.data() + 4, copy));
  *length = size;
  const uint64_t rest = queue_used_ - 4 - size;
  for (uint64_t i = 0; i < rest; ++i) queue_[i] = queue_[4 + size + i];
  queue_used_ = rest;
  return true;
}

NetworkStatistics ThunderboltNet::statistics() const {
  kernel::ScopedRecursiveMutex guard(mutex_);
  return stats_;
}

}  // namespace hardware
