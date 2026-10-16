#pragma once
#include "Network/NetworkStackSet.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

// Exercises the network stack on every network interface at once, against QEMU's
// user-mode network (or any network laid out the same way): DHCP, ICMP ping, DNS,
// UDP and TCP. Where a result depends on a service that may not exist - an echo
// server on the host - a missing service is reported but not treated as a failure.
//
// start() runs the one-time checks and opens a small TCP server on port 8080 on
// each interface that answers each request with a short HTTP response; service()
// must then be called regularly (it polls the stacks too) to keep serving.
class NetworkStackDemo {
 public:
  static constexpr uint16_t kHostTcpEchoPort = 7777;  // optional echo servers on the host, reached at the gateway
  static constexpr uint16_t kHostUdpEchoPort = 7778;
  static constexpr uint16_t kServerPort = 8080;

  NetworkStackDemo(network::NetworkStackSet& stacks, allocators::IAllocator& allocator)
      : stacks_(stacks), allocator_(allocator), endpoints_(allocator) {}

  // Returns false if some interface got no lease or could not reach its gateway.
  bool start();
  // Polls the stacks and serves the TCP servers' connections.
  void service();

 private:
  struct Endpoint {
    explicit Endpoint(allocators::IAllocator& allocator) : connections(allocator) {}
    SharedPointer<network::ITcpSocket> listener;
    List<SharedPointer<network::ITcpSocket>> connections;
  };

  bool check_interface(uint32_t index);
  void serve(Endpoint& endpoint);

  network::NetworkStackSet& stacks_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Endpoint>> endpoints_;
};
