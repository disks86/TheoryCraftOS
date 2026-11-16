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
/* TCP and UDP sockets the way programs use them, over the loopback so that nothing outside the machine is needed: a server and a client in two processes,
 * a connect that does not wait, datagrams, IPv6, select and poll. Built static with glibc and run on the real kernel by tests/fork_qemu.sh; what it prints
 * must be what it prints on Linux. (Each call is made before the line that prints it.) */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { PORT = 47211 };

static void nap(long milliseconds) {
  struct timespec t = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  nanosleep(&t, 0);
}
static int wait_code(pid_t pid) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}
static struct sockaddr_in loopback(int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}
static long read_all(int fd, char* out, long want) {      /* until `want` bytes or the end of the stream */
  long got = 0;
  while (got < want) {
    const long n = read(fd, out + got, (size_t)(want - got));
    if (n <= 0) break;
    got += n;
  }
  return got;
}

/* A server in this process, a client in a child: a request, an answer, and the end of the stream each way. */
static void stream_pair(void) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  const int reuse = setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in address = loopback(PORT);
  const int bound = bind(listener, (struct sockaddr*)&address, sizeof(address));
  const int listening = listen(listener, 4);
  struct sockaddr_in name;
  socklen_t size = sizeof(name);
  const int named = getsockname(listener, (struct sockaddr*)&name, &size);
  printf("tcp: socket %d, reuse %d, bind %d, listen %d, name %d: %s:%d (%d bytes)\n", listener >= 0, reuse, bound, listening, named, inet_ntoa(name.sin_addr),
         ntohs(name.sin_port) == PORT, (int)size);
  const pid_t child = fork();
  if (child == 0) {
    close(listener);
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) _exit(20);
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    if (getpeername(fd, (struct sockaddr*)&peer, &peer_size) != 0 || ntohs(peer.sin_port) != PORT || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) _exit(21);
    struct iovec parts[2] = {{"GET /hello ", 11}, {"please\n", 7}};
    if (writev(fd, parts, 2) != 18) _exit(22);
    char answer[64] = {0};
    if (read_all(fd, answer, 13) != 13 || strcmp(answer, "200 here it is") == 0) _exit(23);
    if (strncmp(answer, "200 here it i", 13) != 0) _exit(24);
    if (shutdown(fd, SHUT_WR) != 0) _exit(25);
    if (read(fd, answer, sizeof(answer)) != 1 || answer[0] != 's') _exit(26);      /* the last byte, sent after our end was closed */
    if (read(fd, answer, sizeof(answer)) != 0) _exit(27);                           /* then the end of the stream */
    close(fd);
    _exit(7);
  }
  struct sockaddr_in from;
  socklen_t from_size = sizeof(from);
  const int fd = accept(listener, (struct sockaddr*)&from, &from_size);
  char request[64] = {0};
  const long got = read_all(fd, request, 18);
  const long sent = send(fd, "200 here it i", 13, MSG_NOSIGNAL);
  char rest[8];
  const long end = read(fd, rest, sizeof(rest));      /* the client shut its side down: 0 */
  const long last = write(fd, "s", 1);
  const int closed = close(fd);
  const int code = wait_code(child);
  printf("tcp: accept %d from %s (%d bytes), request %ld \"%.17s\", answer %ld, end %ld, last %ld, close %d, client %d\n", fd >= 0, inet_ntoa(from.sin_addr),
         (int)from_size, got, request, sent, end, last, closed, code);
  close(listener);
}

/* A connect that does not wait: EINPROGRESS, poll for writability, SO_ERROR; and one to a port nobody listens on. */
static void without_waiting(void) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in address = loopback(PORT + 1);
  bind(listener, (struct sockaddr*)&address, sizeof(address));
  listen(listener, 4);
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  const int started = connect(fd, (struct sockaddr*)&address, sizeof(address));
  const int started_errno = started == 0 ? 0 : errno;
  struct pollfd poll_it = {fd, POLLOUT, 0};
  const int ready = poll(&poll_it, 1, 3000);
  int problem = -1;
  socklen_t size = sizeof(problem);
  const int asked = getsockopt(fd, SOL_SOCKET, SO_ERROR, &problem, &size);
  /* (over the loopback Linux may finish the handshake at once: both outcomes of the first call are fine, and are printed as one) */
  printf("nonblocking: connect %s, poll %d (writable %d), SO_ERROR %d: %d;", started == 0 || started_errno == EINPROGRESS ? "started" : "failed", ready,
         (poll_it.revents & POLLOUT) != 0, asked, problem);
  char byte;
  const long nothing = recv(fd, &byte, 1, 0);
  const int nothing_errno = errno;
  const int flags = fcntl(fd, F_GETFL);
  printf(" recv %ld (errno %d), O_NONBLOCK %d;", nothing, nothing_errno, (flags & O_NONBLOCK) != 0);
  const int accepted = accept(listener, 0, 0);
  const long wrote = write(accepted, "ping", 4);
  struct pollfd read_it = {fd, POLLIN, 0};
  const int readable = poll(&read_it, 1, 3000);
  int waiting = -1;
  const int counted = ioctl(fd, FIONREAD, &waiting);
  char four[8] = {0};
  const long got = recv(fd, four, sizeof(four), 0);
  printf(" write %ld, poll %d, FIONREAD %d: %d, recv %ld \"%s\";", wrote, readable, counted, waiting, got, four);
  close(accepted);
  close(fd);
  close(listener);
  /* nobody listens on this port */
  const int refused = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in nowhere = loopback(PORT + 2);
  const int result = connect(refused, (struct sockaddr*)&nowhere, sizeof(nowhere));
  printf(" no listener: %d (errno %d)\n", result, errno);
  close(refused);
}

static void datagrams(void) {
  const int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at_a = loopback(PORT + 3), at_b = loopback(PORT + 4);
  const int bound_a = bind(a, (struct sockaddr*)&at_a, sizeof(at_a)), bound_b = bind(b, (struct sockaddr*)&at_b, sizeof(at_b));
  const long sent = sendto(a, "datagram one", 12, 0, (struct sockaddr*)&at_b, sizeof(at_b));
  char text[64] = {0};
  struct sockaddr_in from;
  socklen_t size = sizeof(from);
  const long got = recvfrom(b, text, sizeof(text), 0, (struct sockaddr*)&from, &size);
  printf("udp: bind %d %d, sendto %ld, recvfrom %ld \"%s\" from %s:%d;", bound_a, bound_b, sent, got, text, inet_ntoa(from.sin_addr), ntohs(from.sin_port) == PORT + 3);
  /* a connected datagram socket: send and recv without addresses; a datagram cut to the buffer; nothing there for a call that does not wait */
  const int connected = connect(b, (struct sockaddr*)&at_a, sizeof(at_a));
  const long back = send(b, "and back again", 14, 0);
  char few[5] = {0};
  const long cut = recv(a, few, 4, 0);
  const long none = recv(a, text, sizeof(text), MSG_DONTWAIT);
  const int none_errno = errno;
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(a, &readable);
  FD_SET(b, &readable);
  struct timeval soon = {0, 20000};
  const int selected = select((a > b ? a : b) + 1, &readable, 0, 0, &soon);
  printf(" connect %d, send %ld, cut to %ld \"%s\", empty %ld (errno %d), select %d\n", connected, back, cut, few, none, none_errno, selected);
  close(a);
  close(b);
}

static void version_six(void) {
  const int listener = socket(AF_INET6, SOCK_STREAM, 0);
  if (listener < 0) { printf("ipv6: no socket (errno %d)\n", errno); return; }
  int one = 1, only = -1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  socklen_t size = sizeof(only);
  const int asked = getsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &only, &size);
  struct sockaddr_in6 address;
  memset(&address, 0, sizeof(address));
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(PORT + 5);
  address.sin6_addr = in6addr_any;
  const int bound = bind(listener, (struct sockaddr*)&address, sizeof(address));
  const int listening = listen(listener, 4);
  /* an IPv4 client reaches an IPv6 listener (IPV6_V6ONLY is off), and is seen with a mapped address */
  const pid_t child = fork();
  if (child == 0) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = loopback(PORT + 5);
    if (connect(fd, (struct sockaddr*)&to, sizeof(to)) != 0) _exit(30);
    if (write(fd, "four", 4) != 4) _exit(31);
    close(fd);
    const int six = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 to6;
    memset(&to6, 0, sizeof(to6));
    to6.sin6_family = AF_INET6;
    to6.sin6_port = htons(PORT + 5);
    to6.sin6_addr = in6addr_loopback;
    if (connect(six, (struct sockaddr*)&to6, sizeof(to6)) != 0) _exit(32);
    if (write(six, "six", 3) != 3) _exit(33);
    close(six);
    _exit(9);
  }
  char names[2][INET6_ADDRSTRLEN] = {{0}, {0}}, texts[2][8] = {{0}, {0}};
  long counts[2] = {0, 0};
  for (int i = 0; i < 2; ++i) {
    struct sockaddr_in6 from;
    socklen_t from_size = sizeof(from);
    const int fd = accept(listener, (struct sockaddr*)&from, &from_size);
    inet_ntop(AF_INET6, &from.sin6_addr, names[i], sizeof(names[i]));
    counts[i] = read_all(fd, texts[i], 4);
    close(fd);
  }
  const int code = wait_code(child);
  printf("ipv6: V6ONLY %d: %d, bind %d, listen %d; \"%s\" from %s (%ld), \"%s\" from %s (%ld); client %d\n", asked, only, bound, listening, texts[0], names[0], counts[0],
         texts[1], names[1], counts[1], code);
  close(listener);
}

static void options(void) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  int value = -1, on = 1;
  socklen_t size = sizeof(value);
  const int type_asked = getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &size);
  const int type = value;
  const int no_delay_set = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  value = -1;
  size = sizeof(value);
  const int no_delay_asked = getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size);
  const int no_delay = value != 0;
  const int keep_alive = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  struct timeval wait = {0, 50000};
  const int timeout_set = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
  char byte;
  const long unconnected = recv(fd, &byte, 1, 0);
  const int unconnected_errno = errno;
  const long written = send(fd, "x", 1, MSG_NOSIGNAL);
  const int written_errno = errno;
  const int raw = socket(AF_INET, SOCK_STREAM, IPPROTO_UDP);
  const int raw_errno = errno;
  printf("options: SO_TYPE %d: %d, TCP_NODELAY %d %d: %d, SO_KEEPALIVE %d, SO_RCVTIMEO %d, recv unconnected %ld (errno %d), send %ld (errno %d), wrong protocol %d (errno %d)\n",
         type_asked, type, no_delay_set, no_delay_asked, no_delay, keep_alive, timeout_set, unconnected, unconnected_errno, written, written_errno, raw, raw_errno);
  close(fd);
}

/* MSG_PEEK: what is looked at is still there for the next read, of a stream and of a datagram. */
static void looking(void) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in address = loopback(PORT + 6);
  bind(listener, (struct sockaddr*)&address, sizeof(address));
  listen(listener, 4);
  const int client = socket(AF_INET, SOCK_STREAM, 0);
  const int connected = connect(client, (struct sockaddr*)&address, sizeof(address));
  const int server = accept(listener, 0, 0);
  const long sent = send(client, "peek-a-boo", 10, 0);
  char text[32] = {0}, all[32] = {0}, first[32] = {0}, again[32] = {0}, rest[32] = {0};
  const long some = recv(server, text, 4, MSG_PEEK);
  const long whole = recv(server, all, sizeof(all) - 1, MSG_PEEK);
  int waiting = -1;
  ioctl(server, FIONREAD, &waiting);
  const long taken = recv(server, first, 5, 0);
  const long left = recv(server, again, sizeof(again) - 1, MSG_PEEK | MSG_DONTWAIT);
  const long last = recv(server, rest, sizeof(rest) - 1, 0);
  const long none = recv(server, rest, 1, MSG_PEEK | MSG_DONTWAIT);
  const int none_errno = errno;
  printf("peek: connect %d, send %ld; look %ld \"%s\", look %ld \"%s\", waiting %d, read %ld \"%s\", look %ld \"%s\", read %ld \"%s\", look %ld (errno %d);", connected, sent, some,
         text, whole, all, waiting, taken, first, left, again, last, rest, none, none_errno);
  close(client);
  char end;
  const long at_end = recv(server, &end, 1, MSG_PEEK);
  const long really = recv(server, &end, 1, 0);
  printf(" at the end %ld %ld;", at_end, really);
  close(server);
  close(listener);
  const int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at_b = loopback(PORT + 7);
  bind(b, (struct sockaddr*)&at_b, sizeof(at_b));
  sendto(a, "datagram", 8, 0, (struct sockaddr*)&at_b, sizeof(at_b));
  sendto(a, "next", 4, 0, (struct sockaddr*)&at_b, sizeof(at_b));
  char cut[8] = {0}, full[16] = {0}, second[16] = {0};
  struct sockaddr_in from;
  socklen_t size = sizeof(from);
  const long looked = recvfrom(b, cut, 4, MSG_PEEK, (struct sockaddr*)&from, &size);
  const long got = recv(b, full, sizeof(full) - 1, 0);
  const long after = recv(b, second, sizeof(second) - 1, 0);
  printf(" datagram: look %ld \"%s\" from %s, read %ld \"%s\", then %ld \"%s\"\n", looked, cut, inet_ntoa(from.sin_addr), got, full, after, second);
  close(a);
  close(b);
}

/* Control messages: what recvmsg() says about a datagram once the socket asked for it, and what sendmsg() takes. */
static void show_control(const char* what, int fd, size_t room, size_t control_room, int flags) {
  char data[64] = {0};
  union { char bytes[512]; struct cmsghdr align; } control;
  struct sockaddr_storage from;
  struct iovec iov = {data, room};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  memset(&control, 0, sizeof(control));
  m.msg_name = &from; m.msg_namelen = sizeof(from);
  m.msg_iov = &iov; m.msg_iovlen = 1;
  m.msg_control = control.bytes; m.msg_controllen = control_room;
  const long got = recvmsg(fd, &m, flags);
  const int error = got < 0 ? errno : 0;
  printf(" %s: %ld", what, got);
  if (got < 0) { printf(" errno %d\n", error); return; }
  printf(" \"%.*s\" control %lu%s%s:", (int)(got < (long)room ? got : (long)room), data, (unsigned long)m.msg_controllen, m.msg_flags & MSG_TRUNC ? " TRUNC" : "",
         m.msg_flags & MSG_CTRUNC ? " CTRUNC" : "");
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
    const unsigned char* d = CMSG_DATA(c);
    const long size = (long)c->cmsg_len - (long)CMSG_LEN(0);
    char text[64];
    if (c->cmsg_level == SOL_SOCKET && (c->cmsg_type == SO_TIMESTAMP || c->cmsg_type == SO_TIMESTAMPNS) && size == 16) {
      long long seconds;
      memcpy(&seconds, d, 8);
      const long long apart = seconds - (long long)time(0);
      printf(" [%s %s]", c->cmsg_type == SO_TIMESTAMP ? "time" : "time-ns", apart >= -5 && apart <= 5 ? "now" : "wrong");
    } else if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && size == (long)sizeof(struct in_pktinfo)) {
      struct in_pktinfo info;
      memcpy(&info, d, sizeof(info));
      printf(" [pktinfo if %d %s", info.ipi_ifindex, inet_ntop(AF_INET, &info.ipi_spec_dst, text, sizeof(text)));
      printf(" %s]", inet_ntop(AF_INET, &info.ipi_addr, text, sizeof(text)));
    } else if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL && size == 4) {
      int ttl;
      memcpy(&ttl, d, 4);
      printf(" [ttl %d]", ttl);
    } else if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS && size == 1) {
      printf(" [tos %d]", d[0]);
    } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO && size == (long)sizeof(struct in6_pktinfo)) {
      struct in6_pktinfo info;
      memcpy(&info, d, sizeof(info));
      printf(" [pktinfo6 if %u %s]", info.ipi6_ifindex, inet_ntop(AF_INET6, &info.ipi6_addr, text, sizeof(text)));
    } else if (c->cmsg_level == IPPROTO_IPV6 && (c->cmsg_type == IPV6_HOPLIMIT || c->cmsg_type == IPV6_TCLASS) && size == 4) {
      int value;
      memcpy(&value, d, 4);
      printf(" [%s %d]", c->cmsg_type == IPV6_HOPLIMIT ? "hoplimit" : "tclass", value);
    } else {
      printf(" [level %d type %d size %ld]", c->cmsg_level, c->cmsg_type, size);
    }
  }
  printf("\n");
}

static long send_control(int fd, const struct sockaddr* to, socklen_t to_size, int level, int type, const void* payload, size_t size, long length_override) {
  union { char bytes[128]; struct cmsghdr align; } control;
  struct iovec iov = {(void*)"with", 4};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  memset(&control, 0, sizeof(control));
  m.msg_name = (void*)to; m.msg_namelen = to_size;
  m.msg_iov = &iov; m.msg_iovlen = 1;
  m.msg_control = control.bytes; m.msg_controllen = CMSG_SPACE(size);
  struct cmsghdr* c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = level; c->cmsg_type = type; c->cmsg_len = length_override >= 0 ? (size_t)length_override : CMSG_LEN(size);
  memcpy(CMSG_DATA(c), payload, size);
  const long sent = sendmsg(fd, &m, 0);
  return sent < 0 ? -errno : sent;
}

static void control_messages(void) {
  const int one = 1, zero = 0;
  printf("control messages:\n");
  const int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at_b = loopback(PORT + 8);
  bind(b, (struct sockaddr*)&at_b, sizeof(at_b));
  const struct sockaddr* to = (struct sockaddr*)&at_b;
  sendto(a, "plain", 5, 0, to, sizeof(at_b));
  show_control("nothing asked", b, 64, 256, 0);
  const int s1 = setsockopt(b, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof(one)), s2 = setsockopt(b, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
  const int s3 = setsockopt(b, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one)), s4 = setsockopt(b, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one));
  int value = -1;
  socklen_t size = sizeof(value);
  const int g1 = getsockopt(b, IPPROTO_IP, IP_PKTINFO, &value, &size);
  printf(" asking: %d %d %d %d, read back %d %d\n", s1, s2, s3, s4, g1, value);
  sendto(a, "everything", 10, 0, to, sizeof(at_b));
  show_control("everything", b, 64, 256, 0);
  sendto(a, "small", 5, 0, to, sizeof(at_b));
  show_control("room for two", b, 64, CMSG_SPACE(16) + CMSG_SPACE(12), 0);
  sendto(a, "smaller", 7, 0, to, sizeof(at_b));
  show_control("room for one and a half", b, 64, CMSG_SPACE(16) + CMSG_LEN(4), 0);
  sendto(a, "tiny", 4, 0, to, sizeof(at_b));
  show_control("room for less than a header", b, 64, 8, 0);
  sendto(a, "none", 4, 0, to, sizeof(at_b));
  show_control("no room", b, 64, 0, 0);
  sendto(a, "0123456789", 10, 0, to, sizeof(at_b));
  show_control("cut", b, 4, 256, 0);
  sendto(a, "abcdefghij", 10, 0, to, sizeof(at_b));
  show_control("cut, asked its size", b, 4, 256, MSG_TRUNC);
  sendto(a, "ABCDEFGHIJ", 10, 0, to, sizeof(at_b));
  char few[4] = {0};
  const long real = recv(b, few, 3, MSG_TRUNC);
  printf(" recv with MSG_TRUNC: %ld \"%s\"\n", real, few);
  const int s5 = setsockopt(b, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
  const int s6 = setsockopt(b, IPPROTO_IP, IP_RECVTTL, &zero, sizeof(zero)), s7 = setsockopt(b, IPPROTO_IP, IP_RECVTOS, &zero, sizeof(zero));
  size = sizeof(value);
  getsockopt(b, SOL_SOCKET, SO_TIMESTAMP, &value, &size);
  printf(" nanoseconds instead: %d %d %d, SO_TIMESTAMP now %d\n", s5, s6, s7, value);
  sendto(a, "ns", 2, 0, to, sizeof(at_b));
  show_control("nanoseconds", b, 64, 256, 0);
  setsockopt(b, SOL_SOCKET, SO_TIMESTAMPNS, &zero, sizeof(zero));
  show_control("error queue", b, 64, 256, MSG_ERRQUEUE | MSG_DONTWAIT);
  /* what sendmsg takes with a datagram */
  struct in_pktinfo info;
  memset(&info, 0, sizeof(info));
  info.ipi_spec_dst.s_addr = htonl(INADDR_LOOPBACK);
  const int ttl = 9;
  const long c1 = send_control(a, to, sizeof(at_b), IPPROTO_IP, IP_PKTINFO, &info, sizeof(info), -1);
  const long c2 = send_control(a, to, sizeof(at_b), IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl), -1);
  const long c3 = send_control(a, to, sizeof(at_b), IPPROTO_IP, 99, &ttl, sizeof(ttl), -1);
  const long c4 = send_control(a, to, sizeof(at_b), IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl), 8);
  const long c5 = send_control(a, to, sizeof(at_b), IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl), 4096);
  const long c6 = send_control(a, to, sizeof(at_b), IPPROTO_IP, IP_PKTINFO, &info, 8, -1);
  const long c7 = send_control(a, to, sizeof(at_b), SOL_SOCKET, SCM_RIGHTS, &a, sizeof(a), -1);
  printf(" sending with: pktinfo %ld, ttl %ld, unknown %ld, short length %ld, long length %ld, short pktinfo %ld, rights %ld\n", c1, c2, c3, c4, c5, c6, c7);
  show_control("sent with pktinfo", b, 64, 256, 0);
  show_control("sent with ttl", b, 64, 256, 0);
  char rest[16];
  while (recv(b, rest, sizeof(rest), MSG_DONTWAIT) >= 0) {}
  close(a);
  close(b);
  /* IPv6, and an IPv4 datagram through an IPv6 socket */
  const int six = socket(AF_INET6, SOCK_DGRAM, 0), from6 = socket(AF_INET6, SOCK_DGRAM, 0), from4 = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in6 at_six;
  memset(&at_six, 0, sizeof(at_six));
  at_six.sin6_family = AF_INET6;
  at_six.sin6_port = htons(PORT + 9);
  bind(six, (struct sockaddr*)&at_six, sizeof(at_six));
  const int t1 = setsockopt(six, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)), t2 = setsockopt(six, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one));
  const int t3 = setsockopt(six, IPPROTO_IPV6, IPV6_RECVTCLASS, &one, sizeof(one));
  const int t4 = setsockopt(from4, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)), t4_error = t4 < 0 ? errno : 0;
  printf(" asking (IPv6): %d %d %d; of an IPv4 socket %d errno %d\n", t1, t2, t3, t4, t4_error);
  at_six.sin6_addr = in6addr_loopback;
  sendto(from6, "six", 3, 0, (struct sockaddr*)&at_six, sizeof(at_six));
  show_control("IPv6", six, 64, 256, 0);
  struct sockaddr_in at_four = loopback(PORT + 9);
  sendto(from4, "four", 4, 0, (struct sockaddr*)&at_four, sizeof(at_four));
  show_control("IPv4 through it", six, 64, 256, 0);
  const int t5 = setsockopt(six, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
  sendto(from4, "both", 4, 0, (struct sockaddr*)&at_four, sizeof(at_four));
  printf(" IP_PKTINFO too: %d\n", t5);
  show_control("IPv4 through it, both asked", six, 64, 256, 0);
  struct in6_pktinfo info6;
  memset(&info6, 0, sizeof(info6));
  const long d1 = send_control(from6, (struct sockaddr*)&at_six, sizeof(at_six), IPPROTO_IPV6, IPV6_PKTINFO, &info6, sizeof(info6), -1);
  const long d2 = send_control(from6, (struct sockaddr*)&at_six, sizeof(at_six), IPPROTO_IPV6, IPV6_HOPLIMIT, &ttl, sizeof(ttl), -1);
  const long d3 = send_control(from4, (struct sockaddr*)&at_four, sizeof(at_four), IPPROTO_IPV6, IPV6_PKTINFO, &info6, sizeof(info6), -1);
  printf(" sending with (IPv6): pktinfo %ld, hop limit %ld; IPv6 pktinfo on an IPv4 socket %ld\n", d1, d2, d3);
  close(six);
  close(from6);
  close(from4);
  /* a stream has none */
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in at = loopback(PORT + 10);
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  bind(listener, (struct sockaddr*)&at, sizeof(at));
  listen(listener, 1);
  const int client = socket(AF_INET, SOCK_STREAM, 0);
  connect(client, (struct sockaddr*)&at, sizeof(at));
  const int server = accept(listener, 0, 0);
  setsockopt(server, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
  write(client, "stream", 6);
  nap(50);
  show_control("stream", server, 4, 256, 0);
  show_control("stream, the rest", server, 64, 256, MSG_TRUNC);
  close(client);
  close(server);
  close(listener);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  stream_pair();
  without_waiting();
  datagrams();
  version_six();
  options();
  looking();
  control_messages();
  printf("net ok\n");
  return 0;
}
