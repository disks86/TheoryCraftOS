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

// A network stack for host tests that talks to the real network through the operating system's sockets (TCP only): lets the code that takes
// an INetworkStack run against real servers such as `openssl s_server`. Not used by the kernel.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "Network/INetworkStack.hpp"

namespace network {

struct PosixTcpSocket final : ITcpSocket {
  int fd = -1;
  bool connected = false, failed_flag = false, closed_by_peer = false, shut = false;
  Ipv4Address remote;
  uint16_t remote_port_ = 0;

  PosixTcpSocket() { fd = ::socket(AF_INET, SOCK_STREAM, 0); if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK); }
  ~PosixTcpSocket() { if (fd >= 0) ::close(fd); }

  void update() const {
    PosixTcpSocket* self = const_cast<PosixTcpSocket*>(this);
    if (fd < 0 || connected || failed_flag) return;
    pollfd p{fd, POLLOUT, 0};
    if (::poll(&p, 1, 0) > 0) {
      int err = 0;
      socklen_t l = sizeof err;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
      if (err || (p.revents & (POLLERR | POLLHUP))) self->failed_flag = true;
      else self->connected = true;
    }
  }
  TcpState state() const override { update(); return failed_flag ? TcpState::Closed : connected ? TcpState::Established : TcpState::SynSent; }
  bool connect(Ipv4Address address, uint16_t port) override {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(address.value());
    remote = address;
    remote_port_ = port;
    const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    return r == 0 || errno == EINPROGRESS;
  }
  bool listen(uint16_t) override { return false; }
  SharedPointer<ITcpSocket> accept() override { return SharedPointer<ITcpSocket>(); }
  uint32_t send(ConstByteView data) override {
    update();
    if (!connected || shut) return 0;
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) failed_flag = true; return 0; }
    return static_cast<uint32_t>(n);
  }
  uint32_t receive(ByteView buffer) override {
    update();
    if (!connected || buffer.size() == 0) return 0;
    const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (n == 0) { closed_by_peer = true; return 0; }
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) failed_flag = true; return 0; }
    if (const char* dump = getenv("TLS_DUMP")) {   // record what the server sent (for tests/data/make_tls_vectors.py)
      if (FILE* f = fopen(dump, "ab")) { fwrite(buffer.data(), 1, static_cast<size_t>(n), f); fclose(f); }
    }
    return static_cast<uint32_t>(n);
  }
  uint32_t available() const override {
    int n = 0;
    if (fd >= 0) ioctl(fd, FIONREAD, &n);
    return n > 0 ? static_cast<uint32_t>(n) : 0;
  }
  uint32_t unsent_or_unacknowledged() const override {
    int n = 0;
    if (fd >= 0) ioctl(fd, TIOCOUTQ, &n);
    return n > 0 ? static_cast<uint32_t>(n) : 0;
  }
  bool is_connected() const override { update(); return connected && !failed_flag && !shut; }
  bool peer_closed() const override { return closed_by_peer; }
  bool failed() const override { update(); return failed_flag; }
  void close() override { if (fd >= 0 && !shut) { ::shutdown(fd, SHUT_WR); shut = true; } }
  void abort() override { if (fd >= 0) { ::close(fd); fd = -1; } failed_flag = true; }
  uint16_t local_port() const override { return 0; }
  Ipv4Address remote_address() const override { return remote; }
  uint16_t remote_port() const override { return remote_port_; }
};

// Only tcp_open() and now_ms() do anything.
struct PosixNetworkStack final : INetworkStack {
  kernel::RecursiveMutex m;
  allocators::IAllocator& allocator;
  explicit PosixNetworkStack(allocators::IAllocator& a) : allocator(a) {}
  kernel::RecursiveMutex& mutex() override { return m; }
  void poll() override {}
  uint64_t now_ms() const override { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000ull + t.tv_nsec / 1000000; }
  IpConfiguration configuration() const override { return {}; }
  void configure(const IpConfiguration&) override {}
  void start_dhcp() override {}
  const IDhcpClient& dhcp() const override { return *static_cast<IDhcpClient*>(nullptr); }
  bool wait_for_address(uint32_t) override { return true; }
  bool resolve(StringView, uint32_t, Ipv4Address*) override { return false; }
  IDnsResolver& dns() override { return *static_cast<IDnsResolver*>(nullptr); }
  bool ping(Ipv4Address, uint32_t, uint32_t*) override { return false; }
  SharedPointer<IUdpSocket> udp_open(uint16_t) override { return SharedPointer<IUdpSocket>(); }
  SharedPointer<ITcpSocket> tcp_open() override { return SharedPointer<ITcpSocket>::make_as<PosixTcpSocket>(allocator); }
};

}  // namespace network
