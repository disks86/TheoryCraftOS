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

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  stream_pair();
  without_waiting();
  datagrams();
  version_six();
  options();
  printf("net ok\n");
  return 0;
}
