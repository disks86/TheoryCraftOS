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
#include "Network/INetworkStack.hpp"
#include "Network/Ip/IIpLayer.hpp"
#include "Network/Tcp/TcpManager.hpp"
#include "Network/Udp/UdpManager.hpp"
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

  // The interface this stack drives.
  hardware::INetworkInterface& interface() const { return nic_; }

  // --- INetworkStack
  kernel::RecursiveMutex& mutex() override { return mutex_; }
  void poll() override;
  IpConfiguration configuration() const override;
  void configure(const IpConfiguration& configuration) override;
  void start_dhcp() override;
  const IDhcpClient& dhcp() const override { return *dhcp_; }
  bool wait_for_address(uint32_t timeout_ms) override;
  bool resolve(StringView name, uint32_t timeout_ms, Ipv4Address* address) override;
  IDnsResolver& dns() override { return *resolver_; }
  bool ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) override;
  SharedPointer<IUdpSocket> udp_open(uint16_t port) override;
  SharedPointer<ITcpSocket> tcp_open() override;
  bool join_multicast(Ipv4Address group) override;
  void leave_multicast(Ipv4Address group) override;

  // --- IIpLayer
  Ipv4Address local_address() const override { return config_.address; }
  uint32_t max_payload() const override { return kMaxIpPayload; }
  uint64_t now_ms() const override { return clock_.now_ms(); }
  bool send_ip(Ipv4Address destination, uint8_t protocol, ConstByteView payload) override;
  bool send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, ConstByteView payload,
                    bool link_broadcast) override;

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

  // --- IDhcpListener
  void on_lease(const IpConfiguration& configuration) override;
  void on_lease_lost() override;

  void handle_frame(ConstByteView frame);
  void handle_arp(ConstByteView arp);
  void handle_ipv4(ConstByteView packet);
  void handle_icmp(Ipv4Address source, ConstByteView icmp);
  bool send_frame(const hardware::MacAddress& destination, uint16_t ether_type, ConstByteView payload);
  void send_arp(uint16_t operation, const hardware::MacAddress& target_mac, Ipv4Address target_ip);
  void send_arp_from(uint16_t operation, Ipv4Address sender_ip, const hardware::MacAddress& target_mac, Ipv4Address target_ip);
  void service_link_local();
  void link_local_conflict();
  void check_link_local_claim(uint16_t operation, const hardware::MacAddress& sender_mac, Ipv4Address sender_ip, Ipv4Address target_ip);
  void learn(Ipv4Address address, const hardware::MacAddress& mac);
  void flush_pending(Ipv4Address resolved);
  void service_pending();
  bool accepts_destination(Ipv4Address destination) const;
  bool is_member(Ipv4Address group) const;
  void handle_igmp(ConstByteView igmp);
  void send_igmp_report(Ipv4Address group);

  kernel::RecursiveMutex mutex_;  // guards everything below, and the sockets and resolver
  kernel::Mutex ping_mutex_;      // one ping at a time (they share the ping_* state)
  hardware::INetworkInterface& nic_;
  hardware::IClock& clock_;
  allocators::IAllocator& allocator_;
  bool follow_link_ = false, link_was_up_ = false;
  enum class LinkLocalState : uint8_t { Off, Probing, Announcing, Bound };
  bool link_local_enabled_ = false;
  LinkLocalState link_local_state_ = LinkLocalState::Off;
  Ipv4Address link_local_address_ = kAnyAddress;    // the candidate being probed, or the one in use
  uint32_t link_local_attempt_ = 0, link_local_step_ = 0;   // addresses tried so far; probes or announcements sent for this one
  uint64_t link_local_next_ms_ = 0, link_local_defended_ms_ = 0, link_local_dhcp_retry_ms_ = 0;
  IpConfiguration config_;
  Dictionary<uint32_t, ArpEntry> arp_;
  Queue<PendingPacket> pending_;
  UdpManager udp_;
  TcpManager tcp_;
  SharedPointer<UdpSocket> dhcp_socket_;
  UniquePointer<DhcpClient> dhcp_;
  UniquePointer<DnsResolver> resolver_;
  uint16_t ip_id_ = 1;
  static constexpr uint32_t kMaxGroups = 8;
  Ipv4Address groups_[kMaxGroups];
  uint32_t group_count_ = 0;
  // The ping in progress.
  bool ping_active_ = false;
  bool ping_answered_ = false;
  uint16_t ping_id_ = 0x7C05, ping_sequence_ = 0;
  uint64_t ping_sent_ms_ = 0, ping_rtt_ms_ = 0;
  Ipv4Address ping_target_ = kAnyAddress;
};

}  // namespace network
