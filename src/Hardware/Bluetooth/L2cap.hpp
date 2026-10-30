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

// L2CAP (Core spec vol 3 part A) over the controller's ACL links: frames put back together from ACL fragments, fixed channels (the attribute protocol, the security manager and the
// signalling channels), and dynamic channels in basic mode for the classic profiles (PSM, configuration, disconnection). No retransmission or flow control modes, no credit based channels
// (a peer asking for one is told the PSM is not supported), no connectionless channel.
//
// Everything is driven by the controller's poll(): L2cap installs itself as the controller's ACL and link handler, so there can be only one per controller. Handlers are called from inside
// poll() and may send.
#pragma once
#include "Hardware/Bluetooth/HciController.hpp"

namespace hardware {

class IL2capFixedHandler {
 public:
  virtual void on_data(const HciLink& link, ConstByteView sdu) = 0;
  virtual void on_link(const HciLink& link, bool up) { (void)link; (void)up; }

 protected:
  IL2capFixedHandler() = default;
  ~IL2capFixedHandler() = default;
};

struct L2capChannel;
class IL2capChannelHandler {
 public:
  virtual void on_open(L2capChannel& channel) = 0;
  virtual void on_data(L2capChannel& channel, ConstByteView sdu) = 0;
  virtual void on_close(L2capChannel& channel) = 0;

 protected:
  IL2capChannelHandler() = default;
  ~IL2capChannelHandler() = default;
};

enum class L2capState : uint8_t { Closed, Connecting, Config, Open };

struct L2capChannel {
  L2capState state = L2capState::Closed;
  uint16_t handle = 0, local_cid = 0, remote_cid = 0, psm = 0;
  uint16_t mtu_out = 672;               // what we may send (the peer's MTU), what the peer may send us is mtu_in
  uint16_t mtu_in = 672;
  bool local_configured = false, remote_configured = false, we_connected = false;
  IL2capChannelHandler* handler = nullptr;
  uint8_t connect_id = 0;
};

class L2cap {
 public:
  static constexpr uint16_t kCidSignalling = 1, kCidAtt = 4, kCidLeSignalling = 5, kCidSmp = 6, kFirstDynamic = 0x40;
  static constexpr uint32_t kMaxFrame = 1032, kMaxChannels = 8, kMaxFixed = 8, kMaxListeners = 4;
  explicit L2cap(HciController& controller);
  ~L2cap();
  L2cap(const L2cap&) = delete;
  L2cap& operator=(const L2cap&) = delete;

  bool register_fixed(uint16_t cid, IL2capFixedHandler* handler);
  HciResult send_fixed(uint16_t handle, uint16_t cid, ConstByteView sdu);

  // Dynamic channels: a listener takes the channels the peer opens for a PSM; connect() opens one (Connecting until the peer answers and both sides have configured).
  bool listen(uint16_t psm, IL2capChannelHandler* handler);
  L2capChannel* connect(uint16_t handle, uint16_t psm, IL2capChannelHandler* handler);
  HciResult send(L2capChannel& channel, ConstByteView sdu);
  void close(L2capChannel& channel);
  L2capChannel* channel(uint32_t index) { return index < kMaxChannels && channels_[index].state != L2capState::Closed ? &channels_[index] : nullptr; }

  uint32_t frames_dropped() const { return dropped_; }
  uint32_t frames_received() const { return received_; }

 private:
  struct Assembly {
    bool used = false, discard = false;
    uint16_t handle = 0, have = 0, needed = 0;
    uint8_t data[kMaxFrame] = {};
  };
  static void on_acl(void* self, const HciLink& link, uint8_t boundary, ConstByteView data);
  static void on_link(void* self, const HciLink& link, bool up);
  Assembly* assembly_for(uint16_t handle, bool create);
  void frame(const HciLink& link, uint16_t cid, ConstByteView sdu);
  void signalling(const HciLink& link, ConstByteView sdu);
  void command(const HciLink& link, uint8_t code, uint8_t id, ConstByteView data);
  void send_signal(const HciLink& link, uint8_t code, uint8_t id, ConstByteView payload);
  void send_config_request(L2capChannel& channel);
  L2capChannel* find_by_local(uint16_t handle, uint16_t cid);
  L2capChannel* allocate(uint16_t handle);
  void finish_open(L2capChannel& channel);
  void drop_channel(L2capChannel& channel, bool notify);
  IL2capFixedHandler* fixed_for(uint16_t cid) const;

  HciController& controller_;
  Assembly assemblies_[HciController::kMaxLinks];
  struct Fixed { uint16_t cid = 0; IL2capFixedHandler* handler = nullptr; };
  Fixed fixed_[kMaxFixed];
  struct Listener { uint16_t psm = 0; IL2capChannelHandler* handler = nullptr; };
  Listener listeners_[kMaxListeners];
  L2capChannel channels_[kMaxChannels];
  uint8_t next_id_ = 1;
  uint8_t new_id() { const uint8_t id = next_id_++; if (next_id_ == 0) next_id_ = 1; return id; }
  uint32_t dropped_ = 0, received_ = 0;
};

}  // namespace hardware
