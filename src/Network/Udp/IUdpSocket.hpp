#pragma once
#include "Network/NetworkTypes.hpp"

namespace network {

// A UDP endpoint: unreliable, unordered datagrams to and from any host. Polled:
// receive_from() returns at once, and datagrams arrive only while the stack's
// poll() is being called.
class IUdpSocket {
 public:
  virtual uint16_t local_port() const = 0;

  // Sends one datagram of at most 1472 bytes (what fits an Ethernet frame without
  // fragmentation). False if it cannot be sent right now.
  virtual bool send_to(Ipv4Address address, uint16_t port, const uint8_t* data, uint32_t length) = 0;

  // Takes the oldest queued datagram: copies up to `capacity` bytes (the rest of
  // a longer datagram is discarded) and reports the sender and the datagram's
  // full length. False if none is waiting.
  virtual bool receive_from(uint8_t* buffer, uint32_t capacity, Ipv4Address* from_address, uint16_t* from_port,
                            uint32_t* length) = 0;
  // Datagrams waiting to be received.
  virtual uint32_t pending() const = 0;

  // Stops receiving; the port is free for others. (Dropping the last reference
  // does the same.)
  virtual void close() = 0;

 protected:
  IUdpSocket() = default;
  ~IUdpSocket() = default;
};

}  // namespace network
