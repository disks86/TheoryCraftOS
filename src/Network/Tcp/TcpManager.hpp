#pragma once
#include "Network/Tcp/TcpSocket.hpp"

namespace network {

// Owns every TCP socket (connections, listeners, connections in teardown) and
// routes arriving segments to them by port and peer.
class TcpManager {
 public:
  TcpManager(IIpLayer& ip, allocators::IAllocator& allocator) : ip_(ip), allocator_(allocator), sockets_(allocator) {}

  // A new, unconnected socket.
  SharedPointer<TcpSocket> create();
  // A connection spawned by `listener` for an incoming SYN. Null if out of memory.
  SharedPointer<TcpSocket> spawn(TcpSocket* listener, Ipv4Address remote_address, uint16_t remote_port,
                                 uint16_t local_port, const TcpSegment& syn);

  // A TCP segment arrived in an IP packet (`segment` = TCP header and data).
  void handle(Ipv4Address source, Ipv4Address destination, const uint8_t* segment, uint32_t length);
  // Runs timers, finishes accepted handshakes and forgets sockets that are done.
  void poll();

  bool listening_on(uint16_t port) const;
  uint16_t allocate_port();
  uint32_t next_initial_sequence();
  // Disowns (and resets) connections spawned by a listener that is going away.
  void detach_children(TcpSocket* listener);

  // Answers a segment for which there is no connection with a reset.
  void send_reset(Ipv4Address to, uint16_t to_port, uint16_t from_port, const TcpSegment& cause);

 private:
  bool port_in_use(uint16_t port) const;

  IIpLayer& ip_;
  allocators::IAllocator& allocator_;
  List<SharedPointer<TcpSocket>> sockets_;
  uint16_t next_port_ = 49152;
  uint32_t iss_counter_ = 0;
};

}  // namespace network
