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

// A network stack for the development machine: the OS's INetworkStack interfaces over ordinary Linux sockets (non-blocking TCP and UDP, name lookup by
// getaddrinfo), so the TLS, HTTP, WebSocket and QUIC code can be run against real servers from the host tests and the interop tool. Not part of the OS image.
#pragma once
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "Network/INetworkStack.hpp"

namespace hostnet {

using namespace network;

inline uint64_t monotonic_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

class HostTcpSocket final : public ITcpSocket {
 public:
  HostTcpSocket() = default;
  ~HostTcpSocket() { if (fd_ >= 0) ::close(fd_); }

  TcpState state() const override {
    update();
    if (fd_ < 0) return failed_ || closed_ ? TcpState::Closed : TcpState::Closed;
    if (connecting_) return TcpState::SynSent;
    if (peer_closed_) return TcpState::CloseWait;
    return TcpState::Established;
  }
  bool connect(Ipv4Address address, uint16_t port) override {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address.value());
    remote_ = address;
    remote_port_ = port;
    const int r = ::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    if (r == 0) connecting_ = false;
    else if (errno == EINPROGRESS) connecting_ = true;
    else { ::close(fd_); fd_ = -1; failed_ = true; return false; }
    return true;
  }
  bool listen(uint16_t) override { return false; }
  SharedPointer<ITcpSocket> accept() override { return SharedPointer<ITcpSocket>(); }
  uint32_t send(ConstByteView data) override {
    update();
    if (fd_ < 0 || connecting_ || data.empty()) return 0;
    const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) { failed_ = true; }
      return 0;
    }
    return static_cast<uint32_t>(n);
  }
  uint32_t receive(ByteView buffer) override {
    update();
    if (fd_ < 0 || connecting_ || buffer.empty()) return 0;
    const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n > 0) return static_cast<uint32_t>(n);
    if (n == 0) peer_closed_ = true;
    else if (errno != EAGAIN && errno != EWOULDBLOCK) failed_ = true;
    return 0;
  }
  uint32_t available() const override {
    update();
    if (fd_ < 0 || connecting_) return 0;
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
      char peek;
      const ssize_t n = ::recv(fd_, &peek, 1, MSG_PEEK);
      if (n == 0) peer_closed_ = true;
      else if (n > 0) {
        int pending = 0;
        ioctl_pending(&pending);
        return pending > 0 ? static_cast<uint32_t>(pending) : 1;
      }
    }
    return 0;
  }
  uint32_t unsent_or_unacknowledged() const override { return 0; }
  bool is_connected() const override {
    update();
    return fd_ >= 0 && !connecting_ && !failed_;
  }
  bool peer_closed() const override {
    update();
    if (peer_closed_) return true;
    available();
    return peer_closed_;
  }
  bool failed() const override {
    update();
    return failed_;
  }
  void close() override {
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_WR); }
  }
  void abort() override {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    closed_ = true;
  }
  uint16_t local_port() const override { return 0; }
  Ipv4Address remote_address() const override { return remote_; }
  uint16_t remote_port() const override { return remote_port_; }

 private:
  void ioctl_pending(int* n) const;
  void update() const {
    if (fd_ < 0 || !connecting_) return;
    pollfd p{fd_, POLLOUT, 0};
    if (::poll(&p, 1, 0) > 0) {
      int err = 0;
      socklen_t len = sizeof err;
      getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err) { failed_ = true; ::close(fd_); fd_ = -1; }
      else connecting_ = false;
    }
  }
  mutable int fd_ = -1;
  mutable bool connecting_ = false, failed_ = false, peer_closed_ = false, closed_ = false;
  Ipv4Address remote_{};
  uint16_t remote_port_ = 0;
};

inline void HostTcpSocket::ioctl_pending(int* n) const {
  *n = 0;
  if (fd_ >= 0) { char buf[1]; (void)buf; }
}

class HostUdpSocket final : public IUdpSocket {
 public:
  explicit HostUdpSocket(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd_ >= 0 && ::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) { ::close(fd_); fd_ = -1; }
    if (fd_ >= 0) {
      socklen_t len = sizeof sa;
      getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
      port_ = ntohs(sa.sin_port);
    }
  }
  ~HostUdpSocket() { if (fd_ >= 0) ::close(fd_); }
  bool ok() const { return fd_ >= 0; }
  uint16_t local_port() const override { return port_; }
  bool send_to(Ipv4Address address, uint16_t port, ConstByteView data) override {
    if (fd_ < 0 || data.size() > 1472) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(address.value());
    return ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == static_cast<ssize_t>(data.size());
  }
  bool receive_from(ByteView buffer, Ipv4Address* from_address, uint16_t* from_port, uint32_t* length) override {
    if (fd_ < 0) return false;
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    const ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), MSG_TRUNC, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) return false;
    *from_address = Ipv4Address::from_value(ntohl(sa.sin_addr.s_addr));
    *from_port = ntohs(sa.sin_port);
    *length = static_cast<uint32_t>(n);
    return true;
  }
  uint32_t pending() const override {
    if (fd_ < 0) return 0;
    pollfd p{fd_, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLIN) ? 1 : 0;
  }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

 private:
  int fd_ = -1;
  uint16_t port_ = 0;
};

class HostDnsQuery final : public IDnsQuery {
 public:
  HostDnsQuery(DnsStatus status, Ipv4Address address) : status_(status), address_(address) {}
  DnsStatus status() const override { return status_; }
  Ipv4Address address() const override { return address_; }
  uint32_t ttl_seconds() const override { return 60; }

 private:
  DnsStatus status_;
  Ipv4Address address_;
};

class HostDns final : public IDnsResolver {
 public:
  explicit HostDns(allocators::IAllocator& a) : allocator_(a) {}
  SharedPointer<IDnsQuery> query(StringView name) override {
    char host[256];
    if (name.size() == 0 || name.size() >= sizeof host) return SharedPointer<IDnsQuery>();
    memcpy(host, name.data(), name.size());
    host[name.size()] = 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0 || !result) return SharedPointer<IDnsQuery>::make_as<HostDnsQuery>(allocator_, DnsStatus::NotFound, Ipv4Address{});
    const uint32_t v = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(result);
    return SharedPointer<IDnsQuery>::make_as<HostDnsQuery>(allocator_, DnsStatus::Resolved, Ipv4Address::from_value(v));
  }
  void clear_cache() override {}
  uint32_t cache_size() const override { return 0; }

 private:
  allocators::IAllocator& allocator_;
};

class HostDhcp final : public IDhcpClient {
 public:
  DhcpState state() const override { return DhcpState::Bound; }
  IpConfiguration lease() const override { return IpConfiguration{}; }
  uint32_t lease_seconds() const override { return 3600; }
  Ipv4Address server() const override { return Ipv4Address{}; }
};

class HostNetworkStack final : public INetworkStack {
 public:
  explicit HostNetworkStack(allocators::IAllocator& a) : allocator_(a), dns_(a) {}
  kernel::RecursiveMutex& mutex() override { return mutex_; }
  void poll() override { usleep(500); }
  uint64_t now_ms() const override { return monotonic_ms(); }
  IpConfiguration configuration() const override {
    IpConfiguration c;
    c.address = Ipv4Address::make(10, 0, 0, 2);
    return c;
  }
  void configure(const IpConfiguration&) override {}
  void start_dhcp() override {}
  const IDhcpClient& dhcp() const override { return dhcp_; }
  bool wait_for_address(uint32_t) override { return true; }
  bool resolve(StringView name, uint32_t, Ipv4Address* address) override {
    SharedPointer<IDnsQuery> q = dns_.query(name);
    if (!q || q->status() != DnsStatus::Resolved) return false;
    *address = q->address();
    return true;
  }
  IDnsResolver& dns() override { return dns_; }
  bool ping(Ipv4Address, uint32_t, uint32_t*) override { return false; }
  SharedPointer<IUdpSocket> udp_open(uint16_t port) override {
    auto s = SharedPointer<HostUdpSocket>::make(allocator_, port);
    if (!s || !s->ok()) return SharedPointer<IUdpSocket>();
    return SharedPointer<IUdpSocket>(s);
  }
  SharedPointer<ITcpSocket> tcp_open() override { return SharedPointer<ITcpSocket>::make_as<HostTcpSocket>(allocator_); }

 private:
  allocators::IAllocator& allocator_;
  kernel::RecursiveMutex mutex_;
  HostDns dns_;
  HostDhcp dhcp_;
};

}  // namespace hostnet
