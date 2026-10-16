#include "Network/Udp/UdpManager.hpp"
#include "Network/Checksum.hpp"

namespace network {

bool UdpManager::port_in_use(uint16_t port) const {
  for (const SharedPointer<UdpSocket>& s : sockets_)
    if (s->local_port() == port && !s->closed()) return true;
  return false;
}

SharedPointer<UdpSocket> UdpManager::open(uint16_t port) {
  if (port == 0) {
    for (uint32_t tries = 0; tries < 16384 && port == 0; ++tries) {
      const uint16_t candidate = next_ephemeral_;
      next_ephemeral_ = next_ephemeral_ == 65535 ? 49152 : next_ephemeral_ + 1;
      if (!port_in_use(candidate)) port = candidate;
    }
    if (port == 0) return SharedPointer<UdpSocket>();
  } else if (port_in_use(port)) {
    return SharedPointer<UdpSocket>();
  }
  SharedPointer<UdpSocket> socket = SharedPointer<UdpSocket>::make(allocator_, ip_, allocator_, port);
  if (!socket || !sockets_.push_back(socket)) return SharedPointer<UdpSocket>();
  return socket;
}

bool UdpManager::handle(Ipv4Address source, Ipv4Address destination, const uint8_t* segment, uint32_t length) {
  if (length < 8) return false;
  const uint16_t udp_length = load16(segment + 4);
  if (udp_length < 8 || udp_length > length) return false;
  if (load16(segment + 6) != 0) {  // a zero checksum means the sender did not compute one
    const uint32_t sum = checksum_add(pseudo_header_sum(source, destination, kProtocolUdp, udp_length), segment, udp_length);
    if (checksum_finish(sum) != 0) return false;
  }
  const uint16_t port = load16(segment + 2);
  for (SharedPointer<UdpSocket>& s : sockets_) {
    if (s->local_port() != port || s->closed()) continue;
    s->deliver(source, load16(segment), segment + 8, udp_length - 8u);
    return true;
  }
  return false;
}

void UdpManager::reap() {
  for (uint64_t i = sockets_.count(); i > 0; --i) {
    SharedPointer<UdpSocket>& s = sockets_[i - 1];
    if (s->closed() || s.use_count() == 1) sockets_.remove_at(i - 1);
  }
}

}  // namespace network
