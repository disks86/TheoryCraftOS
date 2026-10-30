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

// Networking between two hosts over a Thunderbolt cable (ThunderboltIP, now USB4NET; drivers/net/thunderbolt of Linux). The other host offers the network service in its properties; the
// two hosts log in to each other with messages of the service (XDomain messages that start with the service's UUID), each tells the other which HopID it will send on, and then the
// firmware is asked to open the data path (ICM_APPROVE_XDOMAIN) between a transmit ring of each host and a receive ring of the other. Ethernet frames go in the rings, each in one or more
// frames of up to 4096 bytes with a 12 byte header (the size, the index in the packet, an id that ties the frames of a packet together, and the frame count).
// The class is an Ethernet interface: transmit() and receive() move frames, link_up() says whether the connection is up. poll() drives the login, which blocks for the time of a request.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Crypto/Random.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltIcm.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace tb_ip {
// the service's flags (the other host's prtcstns says which it has)
constexpr uint32_t kFlagE2e = 1u << 0, kFlagMatchFragsId = 1u << 1, kFlag64kFrames = 1u << 2;
constexpr uint32_t kFrameBytes = 4096, kFrameHeaderBytes = 12, kMaxPayload = kFrameBytes - kFrameHeaderBytes, kMaxPacket = 65536, kMaxFramesPerPacket = 17;
constexpr uint32_t kRingSize = 64;
// thunderbolt_ip_type
constexpr uint32_t kLogin = 0, kLoginResponse = 1, kLogout = 2, kStatus = 3;
constexpr uint32_t kHeaderBytes = 68, kLoginBytes = 92, kLoginResponseBytes = 100, kLogoutBytes = 68, kStatusBytes = 72, kProtoVersion = 1;
// the PDFs of the data frames: first and last of a packet
constexpr uint8_t kPdfFrameStart = 1, kPdfFrameEnd = 2;
extern const uint8_t kServiceUuid[16];            // 798f589e-3616-8a47-97c6-5664a920c8dd

struct Header {
  uint64_t route = 0;
  uint8_t sequence = 0;
  uint8_t initiator[16] = {}, target[16] = {};
  uint32_t type = 0, command_id = 0;
};
// The header of a service message (false if it is short, claims more than it has, or the UUID is not the service's).
bool parse_header(ConstByteView packet, Header* out);
uint32_t build_login(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, uint32_t transmit_path, ByteView out);
uint32_t build_login_response(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, const uint8_t mac[6], ByteView out);
uint32_t build_logout(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, ByteView out);
uint32_t build_status(uint64_t route, uint8_t sequence, const uint8_t initiator[16], const uint8_t target[16], uint32_t command_id, uint32_t status, ByteView out);
bool parse_login(ConstByteView packet, uint32_t* proto_version, uint32_t* transmit_path);
bool parse_login_response(ConstByteView packet, uint32_t* status, uint8_t mac[6]);
}  // namespace tb_ip

class ThunderboltNet final : public INetworkInterface, public IThunderboltIpHandler {
 public:
  struct Config {
    uint32_t login_delay_polls = 1125;     // polls between login tries (4.5 s at the desktop loop's 4 ms)
    uint32_t login_retries = 60;
    uint32_t login_timeout_us = 500000;
    bool e2e = true;                       // end to end flow control, if the other host does it too
  };
  static constexpr uint32_t kFirstPath = 8;     // TB_PATH_MIN_HOPID: the lowest HopID of a path on the cable

  ThunderboltNet(ThunderboltIcm& icm, allocators::IAllocator& allocator, crypto::IRandom& random) : icm_(icm), allocator_(allocator), random_(random), packet_(allocator) {}
  ThunderboltNet(ThunderboltIcm& icm, allocators::IAllocator& allocator, crypto::IRandom& random, const Config& config)
      : icm_(icm), allocator_(allocator), random_(random), config_(config), packet_(allocator) {}
  ~ThunderboltNet() { shutdown(); }
  ThunderboltNet(const ThunderboltNet&) = delete;
  ThunderboltNet& operator=(const ThunderboltNet&) = delete;

  // The flags this host offers (to hand to ThunderboltIcm::offer_network before the other host reads the properties).
  static uint32_t offered_flags(const Config& config) { return tb_ip::kFlagMatchFragsId | (config.e2e ? tb_ip::kFlagE2e : 0); }

  // IHardware: makes the hardware address and starts watching for a host that offers the service.
  const char* name() const override { return "Thunderbolt networking"; }
  bool link_comes_and_goes() const override { return true; }
  bool initialize() override;
  void shutdown() override;

  // Drives the connection: finds a host, logs in, opens the path; tears down when the host is gone. Call it from the loop that polls the controller.
  void poll();

  bool connected() const { return state_ == State::Connected; }
  bool bound() const { return state_ != State::Idle; }
  const uint8_t* peer_mac() const { return peer_mac_; }
  uint32_t peer_flags() const { return peer_flags_; }

  // INetworkInterface
  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override { return state_ == State::Connected; }
  uint32_t link_speed_mbps() const override { return state_ == State::Connected ? 10000 : 0; }
  LinkDuplex link_duplex() const override { return state_ == State::Connected ? LinkDuplex::Full : LinkDuplex::Unknown; }
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  NetworkStatistics statistics() const override;

  // IThunderboltIpHandler: the other host's login and logout
  bool handle_ip_packet(ConstByteView body) override;

 private:
  enum class State : uint8_t { Idle, LoggingIn, Connected };
  bool find_host(uint64_t* index);
  bool open(const TbXDomain& xd);
  void close(bool logout);
  void try_login();
  void try_connect();
  void drain_transmit();
  void receive_frames();
  bool take_frame(const uint8_t* frame, uint32_t size);
  bool queue_packet(ConstByteView packet);
  bool respond(ConstByteView packet);

  ThunderboltIcm& icm_;
  allocators::IAllocator& allocator_;
  crypto::IRandom& random_;
  Config config_;
  mutable kernel::RecursiveMutex mutex_;
  MacAddress mac_{};
  bool initialized_ = false;
  State state_ = State::Idle;
  // the host it talks to
  uint8_t remote_uuid_[16] = {}, local_uuid_[16] = {};
  uint64_t route_ = 0;
  uint32_t peer_flags_ = 0;
  uint8_t peer_mac_[6] = {};
  // the connection
  bool login_sent_ = false, login_received_ = false, rings_ready_ = false, disconnect_pending_ = false;
  uint32_t local_path_ = kFirstPath, remote_path_ = 0, command_id_ = 0, login_tries_ = 0, wait_polls_ = 0;
  int tx_hop_ = -1, rx_hop_ = -1;
  UniquePointer<TbRing> tx_ring_, rx_ring_;
  ArrayView<uint8_t> tx_buffers_, rx_buffers_;
  uint32_t tx_next_ = 0, tx_in_flight_ = 0, rx_next_ = 0;
  uint16_t frame_id_ = 0;
  // the packet being put together from frames, and the packets waiting for receive()
  ByteBuffer packet_;
  uint32_t packet_frames_ = 0, packet_index_ = 0, packet_id_ = 0;
  ArrayView<uint8_t> queue_;
  uint64_t queue_used_ = 0;
  NetworkStatistics stats_;
};

// The way the network interface list holds the connection: it can outlive the controller's driver. detach() ends its use of the ThunderboltNet (before that is destroyed); after it the link is
// down and nothing moves.
class ThunderboltNetLink final : public INetworkInterface {
 public:
  explicit ThunderboltNetLink(ThunderboltNet* net) : net_(net), mac_(net ? net->mac_address() : MacAddress{}) {}
  void detach() {
    kernel::ScopedRecursiveMutex guard(mutex_);
    net_ = nullptr;
  }
  const char* name() const override { return "Thunderbolt networking"; }
  bool initialize() override { return true; }
  void shutdown() override { detach(); }
  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override { return with([](ThunderboltNet& n) { return n.link_up(); }, false); }
  bool link_comes_and_goes() const override { return true; }
  uint32_t link_speed_mbps() const override { return with([](ThunderboltNet& n) { return n.link_speed_mbps(); }, 0u); }
  LinkDuplex link_duplex() const override { return with([](ThunderboltNet& n) { return n.link_duplex(); }, LinkDuplex::Unknown); }
  bool transmit(ConstByteView frame) override { return with([&](ThunderboltNet& n) { return n.transmit(frame); }, false); }
  bool receive(ByteView buffer, uint32_t* length) override { return with([&](ThunderboltNet& n) { return n.receive(buffer, length); }, false); }
  NetworkStatistics statistics() const override { return with([](ThunderboltNet& n) { return n.statistics(); }, NetworkStatistics()); }

 private:
  template <class F, class T>
  T with(F f, T otherwise) const {
    kernel::ScopedRecursiveMutex guard(mutex_);
    return net_ ? f(*net_) : otherwise;
  }
  mutable kernel::RecursiveMutex mutex_;
  ThunderboltNet* net_;
  MacAddress mac_;
};

}  // namespace hardware
