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
#include "Allocators/IAllocator.hpp"
#include "Network/Dhcp/DhcpClient.hpp"
#include "Network/Dns/DnsResolver.hpp"
#include "Network/EgressQueue.hpp"
#include "Network/INetworkStack.hpp"
#include "Network/Ip/IIpLayer.hpp"
#include "Network/Tcp/TcpCoalescer.hpp"
#include "Network/Tcp/TcpManager.hpp"
#include "Network/Udp/UdpManager.hpp"
#include "ObjectContainer/Array.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"

namespace network {

// The default network stack. It speaks Ethernet (framing and ARP), IPv4 and ICMP
// echo itself and hands UDP and TCP to their managers and DHCP to its client.
//   - ARP: answers requests for our address, caches replies for two minutes, and
//     holds up to 8 outgoing packets while their next hop's address is looked up.
//   - IPv4: header checksums verified and generated, TTL 64, "don't fragment"
//     set; incoming fragments are dropped, outgoing packets over the MTU refused.
//   - ICMP: replies to echo requests and sends them for ping().
//   - Multicast: join_multicast() receives a group's datagrams (IGMPv2 reports, answers to queries); multicast is sent to the
//     group's Ethernet address.
class NetworkStack final : public INetworkStack, public IIpLayer, private IDhcpListener {
 public:
  // The stack drives `nic` and takes time from `clock`; both must outlive it.
  NetworkStack(hardware::INetworkInterface& nic, hardware::IClock& clock, allocators::IAllocator& allocator);
  ~NetworkStack();

  // Reserves the stack's memory. False if the allocator ran out.
  bool initialize();

  // For an interface whose link comes and goes (Wi-Fi): start DHCP when the link comes up and drop the address when it goes down. Off by default (a wired link is assumed up).
  void set_dhcp_follows_link(bool follow) { follow_link_ = follow; }

  // For a cable to one other host: when DHCP gets no answer, take an address from 169.254.0.0/16 (RFC 3927). The address is probed with ARP before use (three probes, a conflict picks another), announced, and
  // defended once against a later claim; DHCP is tried again every minute and a lease replaces it. Off by default.
  void set_link_local_fallback(bool on) { link_local_enabled_ = on; }
  bool using_link_local() const { return link_local_state_ == LinkLocalState::Announcing || link_local_state_ == LinkLocalState::Bound; }

  // The address the stack tries on its `attempt`-th try (0 first): from the MAC address and the attempt, in 169.254.1.0 - 169.254.254.255 as the RFC reserves the first and last 256.
  static Ipv4Address link_local_candidate(const hardware::MacAddress& mac, uint32_t attempt);

  // Who runs this stack (NetworkStackSet's service thread for it): `wake` is called when the stack has work that its next poll() should do
  // now - a packet to itself, frames waiting for room in the interface - from whatever thread made it (an application's send(), say);
  // `progress` after a poll() that changed something sockets may be waiting for (frames received or delivered, frames sent from the
  // transmit queue). Either may be null. Both run with the stack's lock held: they must only signal.
  struct ServiceHooks {
    void (*wake)(void* context) = nullptr;
    void (*progress)(void* context) = nullptr;
    void* context = nullptr;
  };
  void set_service_hooks(const ServiceHooks& hooks) { hooks_ = hooks; }

  // The interface this stack drives.
  hardware::INetworkInterface& interface() const { return nic_; }

  // --- INetworkStack
  kernel::RecursiveMutex& mutex() override { return mutex_; }
  // Receives and runs everything (every receive queue, then poll_control()). A thread per receive queue can instead call poll_receive() for
  // its queue while one calls poll_control() (NetworkStackSet does).
  void poll() override;
  // The interface's receive queues (each with its own receive offload run and lock; see INetworkInterface::receive_frame_from).
  uint32_t receive_queues() const { return queue_count_; }
  // Takes and handles what queue `queue` has received (up to a batch). True if it stopped at the limit (more are probably waiting). Takes no
  // stack-wide lock: TCP and UDP go to their sockets under the sockets' own locks, so several queues are handled at once.
  bool poll_receive(uint32_t queue);
  // Everything that is not receiving: the loopback, DHCP, IPv6 autoconfiguration, link-local addresses, DNS, ARP retries, TCP timers and what
  // waits in the transmit queue.
  void poll_control();
  // The last poll() stopped at its limit of frames: more are probably waiting (an interrupt will not announce those again).
  // (read by the service thread while another thread's poll may be setting it: a hint, accessed atomically)
  bool saturated() const { return __atomic_load_n(&saturated_, __ATOMIC_RELAXED); }
  // Frames are waiting for room in the interface's transmit queue (EgressQueue): the stack should be polled again soon, whether or not the
  // interface interrupts when it has sent some.
  bool backlogged() const;
  uint64_t progress() const override { return __atomic_load_n(&progress_, __ATOMIC_ACQUIRE); }
  bool wait_for_progress(uint64_t seen, uint64_t timeout_us) override;
  bool serviced() const override { return hooks_.wake != nullptr; }
  EgressQueue::Stats egress_stats() const;
  IpConfiguration configuration() const override;
  void configure(const IpConfiguration& configuration) override;
  void start_dhcp() override;
  const IDhcpClient& dhcp() const override { return *dhcp_; }
  bool wait_for_address(uint32_t timeout_ms) override;
  bool resolve(StringView name, uint32_t timeout_ms, Ipv4Address* address) override;
  IDnsResolver& dns() override { return *resolver_; }
  bool ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) override;
  SharedPointer<IUdpSocket> udp_open(uint16_t port) override { return udp_open_families(port, IpFamilies::Both); }
  SharedPointer<IUdpSocket> udp_open_families(uint16_t port, IpFamilies families) override;
  SharedPointer<ITcpSocket> tcp_open() override;
  bool join_multicast(Ipv4Address group) override;
  void leave_multicast(Ipv4Address group) override;

  // --- IIpLayer
  Ipv4Address local_address() const override {
    kernel::ScopedRecursiveMutex guard(ip_lock_);
    return config_.address;
  }
  uint32_t max_payload() const override { return kMaxIpPayload; }
  uint64_t now_ms() const override { return clock_.now_ms(); }
  bool send_ip(Ipv4Address destination, uint8_t protocol, ConstByteView payload) override;
  bool send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, ConstByteView payload,
                    bool link_broadcast) override;
  uint32_t max_segmented_payload(const IpAddress& destination) const override;
  uint32_t queued_for(const IpAddress& remote, uint16_t local_port, uint16_t remote_port) const override;
  bool send_tcp_segmented(const IpAddress& destination, ConstByteView segment, uint16_t mss, uint8_t ecn = kEcnNotEct) override;
  IpAddress source_for_ip(const IpAddress& destination) const override;
  bool send_ip_to(const IpAddress& destination, uint8_t protocol, ConstByteView payload, uint8_t ecn = kEcnNotEct) override;

  // --- IPv6 (NetworkStackIpv6.cpp)
  Ipv6Configuration configuration6() const override;
  bool ping6(const Ipv6Address& address, uint32_t timeout_ms, uint32_t* rtt_ms) override;
  bool resolve_ip(StringView name, uint32_t timeout_ms, IpAddress* address, bool prefer_ipv6 = false) override;
  // The address an IPv6 packet to `destination` goes out from: ::1 to itself, the link-local one on the link, else the global one (:: when
  // there is none that fits yet).
  Ipv6Address source_for6(const Ipv6Address& destination) const;

  // Receive offload: TCP segments put together before TCP saw them (0 without), see TcpCoalescer.
  uint64_t coalesced_segments() const;
  // The most receive queues seen being handled at the same moment (for tests: two flows on two queues were handled concurrently).
  uint32_t concurrent_receives_seen() const { return __atomic_load_n(&concurrent_peak_, __ATOMIC_RELAXED); }

 private:
  struct ArpEntry {
    hardware::MacAddress mac;
    uint64_t expires_ms;
  };
  struct PendingPacket {  // an IP packet waiting for its next hop's MAC address
    Ipv4Address next_hop;
    uint64_t next_request_ms;
    uint32_t requests;
    uint16_t length;
    uint8_t ip_packet[kMtu];
  };
  struct PendingPacket6 {  // an IPv6 packet (complete) waiting for its next hop's MAC address
    Ipv6Address next_hop;
    uint64_t next_request_ms;
    uint32_t requests;
    uint16_t length;
    uint8_t ip_packet[kMtu];
  };
  struct NeighborEntry {  // the neighbor cache (RFC 4861 7.3, simplified: reachable for a fixed time, then asked again)
    Ipv6Address address;
    hardware::MacAddress mac;
    uint64_t expires_ms;
    bool valid;
  };
  enum class AddressState : uint8_t { None, Tentative, Preferred, Duplicate };
  struct LoopedPacket {  // an IP packet (either version) this stack sent to itself, delivered by poll()
    uint16_t length;
    uint8_t ip_packet[kMtu];
  };

  // --- IDhcpListener
  void on_lease(const IpConfiguration& configuration) override;
  void on_lease_lost() override;

  struct RxQueue {   // one receive queue's state: taken only by the thread handling the queue (under `lock`)
    kernel::Mutex lock;
    ByteView storage;
    TcpCoalescer coalescer;
  };
  static constexpr uint32_t kMaxQueues = 8;
  void handle_frame(ConstByteView frame, const hardware::ReceivedFrame& checks = hardware::ReceivedFrame{});
  void flush_coalesced(RxQueue& rx);
  bool next_hop_for(Ipv4Address destination, hardware::MacAddress* mac) const;
  void handle_arp(ConstByteView arp);
  void handle_ipv4(ConstByteView packet, bool looped = false, const hardware::ReceivedFrame& checks = hardware::ReceivedFrame{});
  void handle_icmp(Ipv4Address source, ConstByteView icmp);
  // --- IPv6 (NetworkStackIpv6.cpp)
  void start_ipv6();
  void stop_ipv6();
  void service_ipv6();
  void handle_ipv6(ConstByteView packet, bool looped = false, const hardware::ReceivedFrame& checks = hardware::ReceivedFrame{});
  void handle_icmpv6(const Ipv6Address& source, const Ipv6Address& destination, uint8_t hop_limit, ConstByteView icmp, bool looped);
  void handle_router_advertisement(const Ipv6Address& source, ConstByteView message);
  void handle_neighbor_solicitation(const Ipv6Address& source, const Ipv6Address& destination, ConstByteView message);
  void handle_neighbor_advertisement(ConstByteView message);
  bool accepts_destination6(const Ipv6Address& destination) const;
  bool is_my_ipv6(const Ipv6Address& address) const;
  // Builds and sends an IPv6 packet (TCP/UDP: checksum as for IPv4; ICMPv6: computed here). `router_alert`: a hop-by-hop header with the
  // router alert option in front (MLD).
  bool send_ip6(const Ipv6Address& source, const Ipv6Address& destination, uint8_t next_header, ConstByteView payload, uint8_t hop_limit,
                bool router_alert = false);
  bool send_icmpv6(const Ipv6Address& source, const Ipv6Address& destination, ConstByteView message, uint8_t hop_limit, bool router_alert = false);
  void send_neighbor_solicitation(const Ipv6Address& target, bool duplicate_detection);
  void send_neighbor_advertisement(const Ipv6Address& to, const Ipv6Address& target, bool solicited);
  void send_router_solicitation();
  void send_mld_report();
  void learn6(const Ipv6Address& address, const hardware::MacAddress& mac, bool create);
  const NeighborEntry* neighbor(const Ipv6Address& address) const;
  bool next_hop6_for(const Ipv6Address& destination, Ipv6Address* next_hop) const;
  bool link_address6_for(const Ipv6Address& destination, hardware::MacAddress* mac) const;
  void flush_pending6(const Ipv6Address& resolved);
  void service_pending6();
  void begin_duplicate_detection(bool global);
  // `checksum` (offsets into the payload: an IP packet) is a transport checksum still to finish: by the interface when it offloads them.
  // Hands a frame to the interface, or, when it has no room (or frames are waiting already), to the transmit queue. False if neither took it.
  void wake_service() { if (hooks_.wake) hooks_.wake(hooks_.context); }
  uint32_t deliver_looped();
  bool transmit(EgressQueue::Kind kind, ConstByteView frame, hardware::TransmitChecksum checksum = hardware::TransmitChecksum{}, uint16_t mss = 0);
  bool send_frame(const hardware::MacAddress& destination, uint16_t ether_type, ConstByteView payload,
                  hardware::TransmitChecksum checksum = hardware::TransmitChecksum{});
  void send_arp(uint16_t operation, const hardware::MacAddress& target_mac, Ipv4Address target_ip);
  void send_arp_from(uint16_t operation, Ipv4Address sender_ip, const hardware::MacAddress& target_mac, Ipv4Address target_ip);
  bool service_link_local();   // true: DHCP should be tried again (the caller does it, with the IP lock let go)
  void link_local_conflict();
  void check_link_local_claim(uint16_t operation, const hardware::MacAddress& sender_mac, Ipv4Address sender_ip, Ipv4Address target_ip);
  void learn(Ipv4Address address, const hardware::MacAddress& mac);
  void flush_pending(Ipv4Address resolved);
  void service_pending();
  bool accepts_destination(Ipv4Address destination) const;
  bool is_member(Ipv4Address group) const;
  void handle_igmp(ConstByteView igmp);
  void send_igmp_report(Ipv4Address group);

  // The locks, outermost first (a thread holding one may take only those after it):
  //   1. a receive queue's lock (RxQueue::lock: the thread handling that queue)
  //   2. mutex_, the control lock: DHCP, the resolver, the orchestration of poll_control(), and what applications ask of the stack itself
  //      (configure, start_dhcp). Fair (handed to waiters in turn).
  //   3. a TLS connection's lock, 4. a TCP or UDP socket's (a listener's before one of its new connections'), 5. the socket tables' and a
  //      listener's accept queue (TcpManager, UdpManager)
  //   6. ip_lock_: everything of the IP layer below - addresses (IPv4 and IPv6, and their autoconfiguration), ARP and neighbor caches,
  //      the packets waiting for them and for the loopback, the transmit queue and the interface's transmit path, the ping, multicast groups.
  //      Held only while a packet is built and sent or the state looked at; nothing under it calls into a socket. Fair too: receive threads
  //      take it for a moment per frame, and under a flood an application's send would otherwise keep losing the race for it (10 ms a datagram).
  // So receiving takes no stack-wide lock (TCP and UDP go straight to their sockets, ARP, ICMP and neighbor discovery take the IP lock), and
  // sockets on different queues are handled at the same time.
  kernel::RecursiveMutex mutex_;
  mutable kernel::RecursiveMutex ip_lock_;
  kernel::Mutex ping_mutex_;      // one ping at a time (they share the ping_* state)
  hardware::INetworkInterface& nic_;
  hardware::IClock& clock_;
  allocators::IAllocator& allocator_;
  bool follow_link_ = false, link_was_up_ = false;
  bool saturated_ = false;
  enum class LinkLocalState : uint8_t { Off, Probing, Announcing, Bound };
  bool link_local_enabled_ = false;
  LinkLocalState link_local_state_ = LinkLocalState::Off;
  Ipv4Address link_local_address_ = kAnyAddress;    // the candidate being probed, or the one in use
  uint32_t link_local_attempt_ = 0, link_local_step_ = 0;   // addresses tried so far; probes or announcements sent for this one
  uint64_t link_local_next_ms_ = 0, link_local_defended_ms_ = 0, link_local_dhcp_retry_ms_ = 0;
  IpConfiguration config_;
  Dictionary<uint32_t, ArpEntry> arp_;
  Queue<PendingPacket> pending_;
  Queue<LoopedPacket> looped_;
  EgressQueue egress_;   // frames the interface had no room for, sent fairly as it makes room
  ServiceHooks hooks_;
  uint64_t progress_ = 0;            // (see progress(); with progress_mutex_ and progress_cv_ for wait_for_progress)
  kernel::Mutex progress_mutex_;
  kernel::ConditionVariable progress_cv_;
  void note_progress();
  uint8_t outgoing_ecn_ = kEcnNotEct;   // the ECN field of the IP packet being built (set by send_ip_to for its call, under ip_lock_)
  UdpManager udp_;
  TcpManager tcp_;
  SharedPointer<UdpSocket> dhcp_socket_;
  UniquePointer<DhcpClient> dhcp_;
  UniquePointer<DnsResolver> resolver_;
  uint16_t ip_id_ = 1;
  // Receive offload and the receive queues: each queue's run of TCP segments being put together, and its memory.
  static constexpr uint32_t kCoalesceBytes = 24 * 1024;
  Array<UniquePointer<RxQueue>, kMaxQueues> rx_;
  uint32_t queue_count_ = 1;
  uint32_t receiving_now_ = 0, concurrent_peak_ = 0;   // (queues being handled at this moment, and the most seen)
  // Segmentation offload: where a large TCP segment's frame is built (allocated with the first one).
  ByteView segment_frame_;
  static constexpr uint32_t kMaxGroups = 8;
  Ipv4Address groups_[kMaxGroups];
  uint32_t group_count_ = 0;
  // The ping in progress.
  bool ping_active_ = false;
  bool ping_answered_ = false;
  uint16_t ping_id_ = 0x7C05, ping_sequence_ = 0;
  uint64_t ping_sent_ms_ = 0, ping_rtt_ms_ = 0;
  Ipv4Address ping_target_ = kAnyAddress;
  bool ping_v6_ = false;
  Ipv6Address ping_target6_ = kIpv6Any;

  // --- IPv6 state
  bool ipv6_started_ = false;
  Ipv6Configuration config6_;
  AddressState link_local_state_6_ = AddressState::None, global_state_ = AddressState::None;
  Ipv6Address tentative_link_local_ = kIpv6Any, tentative_global_ = kIpv6Any;
  uint64_t link_local_dad_ms_ = 0, global_dad_ms_ = 0;   // when duplicate detection of each finishes
  uint32_t router_solicitations_ = 0;
  uint64_t next_router_solicitation_ms_ = 0;
  uint64_t router_expires_ms_ = 0, prefix_expires_ms_ = 0, dns6_expires_ms_ = 0;
  bool prefix_on_link_ = false;
  static constexpr uint32_t kNeighbors = 32;
  NeighborEntry neighbors_[kNeighbors] = {};
  uint32_t next_neighbor_slot_ = 0;
  Queue<PendingPacket6> pending6_;
  uint32_t ipv6_flow_ = 0;
};

}  // namespace network
