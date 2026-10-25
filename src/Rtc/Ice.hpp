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

// ICE (RFC 8445), the part of WebRTC that finds a path between two peers: gathering candidates (the machine's address, and the address a STUN server sees), pairing them with
// the remote's, connectivity checks with the short-term credentials, nomination, and the consent checks that keep the path alive. One component (RTP and RTCP are
// multiplexed, and so is everything else - DTLS, SCTP - on the one UDP socket, as BUNDLE has it), IPv4 only. Polled like the network stack it runs over.
#pragma once
#include "Network/INetworkStack.hpp"
#include "ObjectContainer/List.hpp"
#include "Rtc/Stun.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace rtc {

// What the peer connection needs of the machine: the network, a clock and random bytes.
class IRtcNetwork {
 public:
  virtual SharedPointer<network::IUdpSocket> open_udp(uint16_t port) = 0;   // port 0: any
  virtual bool local_address(network::Ipv4Address* address) = 0;
  virtual bool resolve(StringView host, network::Ipv4Address* address) = 0;
  virtual uint64_t now_ms() = 0;
  virtual void poll() = 0;
  virtual bool random(ByteView out) = 0;

 protected:
  IRtcNetwork() = default;
  ~IRtcNetwork() = default;
};

enum class CandidateType : uint8_t { Host, ServerReflexive, PeerReflexive, Relay };
struct IceCandidate {
  CandidateType type = CandidateType::Host;
  network::Ipv4Address address{};
  uint16_t port = 0;
  uint32_t priority = 0, foundation = 1;
  network::Ipv4Address related_address{};
  uint16_t related_port = 0;
};
uint32_t ice_priority(CandidateType type, uint32_t local_preference, uint32_t component);
const char* candidate_type_name(CandidateType type);

enum class IceState : uint8_t { New, Checking, Connected, Completed, Failed, Disconnected, Closed };
enum class IceGathering : uint8_t { New, Gathering, Complete };

class IceAgent {
 public:
  static constexpr uint32_t kMaxRemote = 32, kMaxDatagrams = 64, kMaxStunServers = 4;
  IceAgent(allocators::IAllocator& allocator, IRtcNetwork& network);
  ~IceAgent();

  void set_local_credentials(StringView ufrag, StringView pwd);
  void set_remote_credentials(StringView ufrag, StringView pwd);
  void set_controlling(bool controlling) { controlling_ = controlling; }
  bool controlling() const { return controlling_; }
  // Opens the socket and starts gathering: the host candidate at once, a server reflexive one for every STUN server that answers.
  bool start_gathering(const StringView* stun_servers, uint32_t count);
  bool add_remote_candidate(const IceCandidate& candidate);
  void remote_candidates_done() { remote_done_ = true; }
  // Starts the connectivity checks (needs the remote credentials). Safe to call again.
  void start_checks();
  void close();

  // Drives everything: reads the socket, answers and sends checks. Call often.
  void poll();

  IceState state() const { return state_; }
  IceGathering gathering() const { return gathering_; }
  // The next local candidate not yet taken (false: none).
  bool take_local_candidate(IceCandidate* out);
  bool selected(IceCandidate* local, IceCandidate* remote) const;
  uint16_t local_port() const { return socket_ ? socket_->local_port() : 0; }
  bool state_changed(IceState* out);   // true once for every change

  // Sends a datagram over the selected pair (STUN and everything else alike). False if there is no pair yet.
  bool send(ConstByteView data);
  // The next datagram that was not STUN, from a peer we have a pair with. Returns its length (0: none).
  uint32_t receive(ByteView buffer);
  // How many STUN requests / responses were exchanged (for the stats).
  uint32_t requests_sent() const { return requests_sent_; }
  uint32_t responses_received() const { return responses_received_; }
  uint32_t requests_received() const { return requests_received_; }

 private:
  enum class PairState : uint8_t { Waiting, InProgress, Succeeded, Failed };
  struct Pair {
    uint32_t remote = 0;                   // index of the remote candidate
    uint64_t priority = 0;
    PairState state = PairState::Waiting;
    bool nominate = false;                 // our checks carry USE-CANDIDATE
    bool nominated = false;                // the pair is nominated (by us and answered, or by the other side)
    bool triggered = false;
    uint8_t transaction[12] = {};
    uint8_t attempts = 0;
    uint64_t sent_ms = 0;
  };
  struct Datagram {
    uint8_t data[1500];
    uint32_t length = 0;
    network::Ipv4Address from{};
    uint16_t port = 0;
  };
  struct Gather {
    network::Ipv4Address server{};
    uint16_t port = 0;
    uint8_t transaction[12] = {};
    uint8_t attempts = 0;
    bool done = false;
    uint64_t sent_ms = 0;
  };

  void handle_datagram(ConstByteView data, network::Ipv4Address from, uint16_t port);
  void handle_request(ConstByteView data, const StunMessage& m, network::Ipv4Address from, uint16_t port);
  void handle_response(ConstByteView data, const StunMessage& m, network::Ipv4Address from, uint16_t port);
  void send_check(Pair& pair);
  void run_checks();
  void run_gathering();
  void update_state();
  void set_state(IceState s);
  int32_t find_remote(network::Ipv4Address a, uint16_t port) const;
  int32_t find_pair(uint32_t remote) const;
  int32_t add_pair(uint32_t remote);
  void make_transaction(uint8_t out[12]);
  uint64_t pair_priority(uint32_t local, uint32_t remote_priority) const;
  void reply(const StunMessage& m, network::Ipv4Address to, uint16_t port, bool ok, uint16_t error);

  allocators::IAllocator& allocator_;
  IRtcNetwork& net_;
  SharedPointer<network::IUdpSocket> socket_;
  String local_ufrag_, local_pwd_, remote_ufrag_, remote_pwd_;
  bool controlling_ = true, remote_done_ = false, checks_started_ = false, closed_ = false;
  uint64_t tie_breaker_ = 0;
  IceState state_ = IceState::New, reported_ = IceState::New;
  IceGathering gathering_ = IceGathering::New;
  List<IceCandidate> local_, remote_, outgoing_;
  List<Pair> pairs_;
  List<Gather> gather_;
  List<UniquePointer<Datagram>> inbox_;
  int32_t selected_ = -1;
  network::Ipv4Address host_address_{};
  uint64_t last_check_ms_ = 0, last_rx_ms_ = 0, last_consent_ms_ = 0, started_ms_ = 0, gather_started_ms_ = 0, disconnected_ms_ = 0;
  uint32_t requests_sent_ = 0, responses_received_ = 0, requests_received_ = 0;
};

}  // namespace rtc
