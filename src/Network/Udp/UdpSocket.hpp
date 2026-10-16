#pragma once
#include "Allocators/IAllocator.hpp"
#include "Network/Ip/IIpLayer.hpp"
#include "Network/Udp/IUdpSocket.hpp"
#include "ObjectContainer/Queue.hpp"

namespace network {

// The default IUdpSocket. Received datagrams wait in a queue (up to kQueueLimit;
// later ones are dropped, as UDP allows).
class UdpSocket final : public IUdpSocket {
 public:
  static constexpr uint32_t kMaxDatagram = kMaxIpPayload - 8;  // 1472: IP payload minus the UDP header
  static constexpr uint32_t kQueueLimit = 32;

  UdpSocket(IIpLayer& ip, allocators::IAllocator& allocator, uint16_t port)
      : ip_(ip), port_(port), queue_(allocator) {}

  uint16_t local_port() const override { return port_; }
  bool send_to(Ipv4Address address, uint16_t port, const uint8_t* data, uint32_t length) override;
  bool receive_from(uint8_t* buffer, uint32_t capacity, Ipv4Address* from_address, uint16_t* from_port,
                    uint32_t* length) override;
  uint32_t pending() const override { return static_cast<uint32_t>(queue_.count()); }
  void close() override { closed_ = true; }

  // Like send_to() but with an explicit source address, which may be 0.0.0.0, and
  // optionally as a link-layer broadcast. DHCP uses it before it has an address.
  bool send_from(Ipv4Address source, Ipv4Address address, uint16_t port, const uint8_t* data, uint32_t length,
                 bool link_broadcast);

  // The UDP manager hands arriving datagrams in here.
  void deliver(Ipv4Address from_address, uint16_t from_port, const uint8_t* data, uint32_t length);
  bool closed() const { return closed_; }

 private:
  struct Datagram {
    Ipv4Address from_address;
    uint16_t from_port;
    uint16_t length;
    uint8_t data[kMaxDatagram];
  };

  IIpLayer& ip_;
  uint16_t port_;
  bool closed_ = false;
  Queue<Datagram> queue_;
};

}  // namespace network
