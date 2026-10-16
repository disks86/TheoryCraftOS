#pragma once
#include "Network/Dhcp/IDhcpClient.hpp"
#include "Network/Ip/IIpLayer.hpp"
#include "Network/Udp/UdpSocket.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// Told when a lease is obtained or lost, so the owner can (de)configure itself.
class IDhcpListener {
 public:
  virtual void on_lease(const IpConfiguration& configuration) = 0;
  virtual void on_lease_lost() = 0;

 protected:
  IDhcpListener() = default;
  ~IDhcpListener() = default;
};

// The default IDhcpClient (RFC 2131): DISCOVER, take the first OFFER, REQUEST it,
// and on the ACK hand the configuration to the listener. Half-way through the
// lease (T1) it asks the server to extend it; when the lease runs out without an
// answer it gives the address up and starts over. Messages are retried with
// growing delays. Driven by poll().
class DhcpClient final : public IDhcpClient {
 public:
  static constexpr uint16_t kServerPort = 67;
  static constexpr uint16_t kClientPort = 68;

  DhcpClient(IIpLayer& ip, SharedPointer<UdpSocket> socket, hardware::MacAddress mac, IDhcpListener& listener)
      : ip_(ip), socket_(socket), mac_(mac), listener_(listener) {}

  void start();
  void stop();
  void poll();

  DhcpState state() const override { return state_; }
  IpConfiguration lease() const override { return lease_; }
  uint32_t lease_seconds() const override { return lease_seconds_; }
  Ipv4Address server() const override { return server_; }

 private:
  uint32_t build(uint8_t* message, uint8_t type, bool broadcast_reply) const;
  void begin_discovery();
  void send_discover();
  void send_request(bool renewing);
  void handle(const uint8_t* message, uint32_t length);
  void bind(const IpConfiguration& configuration, uint32_t lease_seconds, uint32_t t1_seconds);
  void schedule_retry();

  IIpLayer& ip_;
  SharedPointer<UdpSocket> socket_;
  hardware::MacAddress mac_;
  IDhcpListener& listener_;

  DhcpState state_ = DhcpState::Stopped;
  uint32_t xid_ = 0;
  IpConfiguration offered_;     // what the server offered / granted
  IpConfiguration lease_;       // the lease in force
  Ipv4Address server_ = kAnyAddress;
  uint32_t lease_seconds_ = 0;
  uint32_t offered_lease_seconds_ = 0;
  uint32_t offered_t1_seconds_ = 0;
  uint64_t bound_at_ms_ = 0;
  uint64_t t1_ms_ = 0;          // when to start renewing, measured from bound_at_ms_
  uint64_t next_action_ms_ = 0; // when to retry / act next
  uint32_t attempts_ = 0;
  uint32_t renew_attempts_ = 0;
};

}  // namespace network
