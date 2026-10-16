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
class NetworkStack final : public INetworkStack, public IIpLayer, private IDhcpListener {
 public:
  // The stack drives `nic` and takes time from `clock`; both must outlive it.
  NetworkStack(hardware::INetworkInterface& nic, hardware::IClock& clock, allocators::IAllocator& allocator);
  ~NetworkStack();

  // Reserves the stack's memory. False if the allocator ran out.
  bool initialize();

  // The interface this stack drives.
  hardware::INetworkInterface& interface() const { return nic_; }

  // --- INetworkStack
  void poll() override;
  IpConfiguration configuration() const override { return config_; }
  void configure(const IpConfiguration& configuration) override;
  void start_dhcp() override;
  const IDhcpClient& dhcp() const override { return *dhcp_; }
  bool wait_for_address(uint32_t timeout_ms) override;
  bool resolve(const char* name, uint32_t timeout_ms, Ipv4Address* address) override;
  IDnsResolver& dns() override { return *resolver_; }
  bool ping(Ipv4Address address, uint32_t timeout_ms, uint32_t* rtt_ms) override;
  SharedPointer<IUdpSocket> udp_open(uint16_t port) override;
  SharedPointer<ITcpSocket> tcp_open() override;

  // --- IIpLayer
  Ipv4Address local_address() const override { return config_.address; }
  uint32_t max_payload() const override { return kMaxIpPayload; }
  uint64_t now_ms() const override { return clock_.now_ms(); }
  bool send_ip(Ipv4Address destination, uint8_t protocol, const uint8_t* payload, uint32_t length) override;
  bool send_ip_from(Ipv4Address source, Ipv4Address destination, uint8_t protocol, const uint8_t* payload,
                    uint32_t length, bool link_broadcast) override;

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

  void handle_frame(const uint8_t* frame, uint32_t length);
  void handle_arp(const uint8_t* arp, uint32_t length);
  void handle_ipv4(const uint8_t* packet, uint32_t length);
  void handle_icmp(Ipv4Address source, const uint8_t* icmp, uint32_t length);
  bool send_frame(const hardware::MacAddress& destination, uint16_t ether_type, const uint8_t* payload, uint32_t length);
  void send_arp(uint16_t operation, const hardware::MacAddress& target_mac, Ipv4Address target_ip);
  void learn(Ipv4Address address, const hardware::MacAddress& mac);
  void flush_pending(Ipv4Address resolved);
  void service_pending();
  bool accepts_destination(Ipv4Address destination) const;

  hardware::INetworkInterface& nic_;
  hardware::IClock& clock_;
  allocators::IAllocator& allocator_;
  IpConfiguration config_;
  Dictionary<uint32_t, ArpEntry> arp_;
  Queue<PendingPacket> pending_;
  UdpManager udp_;
  TcpManager tcp_;
  SharedPointer<UdpSocket> dhcp_socket_;
  UniquePointer<DhcpClient> dhcp_;
  UniquePointer<DnsResolver> resolver_;
  uint16_t ip_id_ = 1;
  // The ping in progress.
  bool ping_active_ = false;
  bool ping_answered_ = false;
  uint16_t ping_id_ = 0x7C05, ping_sequence_ = 0;
  uint64_t ping_sent_ms_ = 0, ping_rtt_ms_ = 0;
  Ipv4Address ping_target_ = kAnyAddress;
};

}  // namespace network
