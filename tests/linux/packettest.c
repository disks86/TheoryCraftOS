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
/* AF_PACKET sockets over the loopback, so that nothing outside the machine is needed: frames heard whole (SOCK_RAW) and without their link header
 * (SOCK_DGRAM), a socket filter, a frame sent by hand. Built static with glibc and run on the real kernel by tests/fork_qemu.sh; what it prints must be
 * what it prints on Linux, where it needs the right to open such sockets (the script runs it in a network namespace of its own for that). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 24970

static int ready(int fd, int milliseconds) {
  struct pollfd wait = {fd, POLLIN, 0};
  return poll(&wait, 1, milliseconds) == 1;
}

static struct sockaddr_in local(int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

static void say(int udp, int port, const char* text) {
  struct sockaddr_in to = local(port);
  sendto(udp, text, strlen(text), 0, (struct sockaddr*)&to, sizeof(to));
}

static void show_address(const struct sockaddr_ll* a, socklen_t size) {
  printf("address of %d bytes: family %d protocol %#x interface %d hardware type %d packet type %d address of %d bytes %02x:%02x:%02x:%02x:%02x:%02x", (int)size, a->sll_family,
         ntohs(a->sll_protocol), a->sll_ifindex, a->sll_hatype, a->sll_pkttype, a->sll_halen, a->sll_addr[0], a->sll_addr[1], a->sll_addr[2], a->sll_addr[3], a->sll_addr[4], a->sll_addr[5]);
}

/* One frame from a packet socket, said in a line; `header` is how many bytes of link header it starts with. */
static void show_frame(const char* what, int fd, int header) {
  unsigned char frame[256];
  struct sockaddr_ll from;
  socklen_t size = sizeof(from);
  memset(&from, 0, sizeof(from));
  if (!ready(fd, 1000)) { printf(" %s: nothing came\n", what); return; }
  const long n = recvfrom(fd, frame, sizeof(frame), 0, (struct sockaddr*)&from, &size);
  if (n < header + 28) { printf(" %s: %ld errno %d\n", what, n, errno); return; }
  printf(" %s: %ld bytes", what, n);
  if (header) printf(", to %02x:%02x:%02x from %02x:%02x:%02x type %#x", frame[0], frame[1], frame[5], frame[6], frame[7], frame[11], frame[12] << 8 | frame[13]);
  const unsigned char* ip = frame + header;
  printf(", IP version %d protocol %d, to port %d, \"%.*s\"; ", ip[0] >> 4, ip[9], ip[22] << 8 | ip[23], (int)(n - header - 28), ip + 28);
  show_address(&from, size);
  printf("\n");
}

static unsigned short checksum(const unsigned char* bytes, int size) {
  unsigned long sum = 0;
  for (int i = 0; i < size; i += 2) sum += (unsigned long)bytes[i] << 8 | bytes[i + 1];
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (unsigned short)~sum;
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  const int lo = (int)if_nametoindex("lo");
  const int udp = socket(AF_INET, SOCK_DGRAM, 0), listener = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at = local(PORT);
  bind(listener, (struct sockaddr*)&at, sizeof(at));

  /* whole frames of one protocol, on one interface */
  const int raw = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
  int type = 0, domain = 0;
  socklen_t size = sizeof(type);
  getsockopt(raw, SOL_SOCKET, SO_TYPE, &type, &size);
  size = sizeof(domain);
  getsockopt(raw, SOL_SOCKET, SO_DOMAIN, &domain, &size);
  struct sockaddr_ll where;
  memset(&where, 0, sizeof(where));
  where.sll_family = AF_PACKET;
  where.sll_ifindex = 987654;
  const int b0 = bind(raw, (struct sockaddr*)&where, sizeof(where)), b0_error = errno;
  where.sll_ifindex = lo;
  const int b1 = bind(raw, (struct sockaddr*)&where, sizeof(where));
  printf("raw: %s, type %d domain %d; bind to an interface there is not: %d errno %d; to lo (%d): %d\n", raw >= 0 ? "made" : "refused", type, domain, b0, b0_error, lo, b1);
  struct sockaddr_ll own;
  size = sizeof(own);
  memset(&own, 0, sizeof(own));
  getsockname(raw, (struct sockaddr*)&own, &size);
  printf(" its own ");
  show_address(&own, size);
  printf("\n");
  say(udp, PORT, "hello");
  show_frame("heard", raw, 14);
  printf(" more: %s\n", ready(raw, 100) ? "yes" : "no");
  /* looked at and sized before it is read */
  say(udp, PORT, "sized");
  unsigned char small[4];
  int waiting = -1;
  const int woke = ready(raw, 1000);
  ioctl(raw, FIONREAD, &waiting);
  const long looked = recv(raw, small, sizeof(small), MSG_PEEK | MSG_TRUNC);
  printf(" woke %d, waiting %d, looked %ld\n", woke, waiting, looked);
  show_frame("then read", raw, 14);

  /* without the link header */
  const int cooked = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
  bind(cooked, (struct sockaddr*)&where, sizeof(where));
  say(udp, PORT, "cooked");
  show_frame("without a link header", cooked, 0);
  show_frame("and whole", raw, 14);

  /* a filter: only UDP to PORT, and of that only 46 bytes */
  struct sock_filter code[] = {
      {0x28, 0, 0, 12}, {0x15, 0, 8, 0x0800}, {0x30, 0, 0, 23}, {0x15, 0, 6, 17}, {0x28, 0, 0, 20}, {0x45, 4, 0, 0x1FFF},
      {0xB1, 0, 0, 14}, {0x48, 0, 0, 16},     {0x15, 0, 1, PORT}, {0x06, 0, 0, 46}, {0x06, 0, 0, 0},
  };
  struct sock_fprog program = {sizeof(code) / sizeof(code[0]), code};
  struct sock_filter broken[] = {{0x15, 9, 9, 0}, {0x06, 0, 0, 0}};
  struct sock_fprog bad = {2, broken};
  const int f0 = setsockopt(raw, SOL_SOCKET, SO_ATTACH_FILTER, &bad, sizeof(bad)), f0_error = errno;
  const int f1 = setsockopt(raw, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program));
  while (ready(cooked, 50)) recv(cooked, small, sizeof(small), 0);
  say(udp, PORT + 1, "another port");
  say(udp, PORT, "filtered, and cut short");
  printf(" filter: a broken one %d errno %d, a good one %d\n", f0, f0_error, f1);
  show_frame("through the filter", raw, 14);
  printf(" more: %s\n", ready(raw, 100) ? "yes" : "no");
  const int nothing = 0;
  const int f2 = setsockopt(raw, SOL_SOCKET, SO_DETACH_FILTER, &nothing, sizeof(nothing));
  const int f3 = setsockopt(raw, SOL_SOCKET, SO_DETACH_FILTER, &nothing, sizeof(nothing)), f3_error = errno;
  printf(" taken off: %d, again: %d errno %d\n", f2, f3, f3_error);
  while (ready(cooked, 50)) recv(cooked, small, sizeof(small), 0);

  /* a frame made by hand and sent on the loopback: a UDP datagram for the listener (no UDP checksum, which IPv4 allows) */
  unsigned char frame[14 + 20 + 8 + 7];
  memset(frame, 0, sizeof(frame));
  frame[12] = 0x08;
  unsigned char* ip = frame + 14;
  ip[0] = 0x45; ip[3] = 20 + 8 + 7; ip[8] = 64; ip[9] = 17;
  ip[12] = 127; ip[15] = 1; ip[16] = 127; ip[19] = 1;
  const unsigned short sum = checksum(ip, 20);
  ip[10] = (unsigned char)(sum >> 8); ip[11] = (unsigned char)sum;
  ip[20] = 12345 >> 8; ip[21] = 12345 & 0xFF; ip[22] = PORT >> 8; ip[23] = PORT & 0xFF; ip[25] = 15;
  memcpy(ip + 28, "by hand", 7);
  /* (Whether the listener gets it is not said: Linux drops a packet to 127.0.0.1 that arrives as a frame, where this is the loopback's only way in.) */
  const long s0 = send(raw, frame, 10, 0), s0_error = errno;
  const long s1 = send(raw, frame, sizeof(frame), 0);
  printf(" by hand: too short %ld errno %ld; sent %ld\n", s0, s0_error, s1);
  show_frame("and the packet socket heard it", raw, 14);
  /* the same without a link header, to an address */
  struct sockaddr_ll to;
  memset(&to, 0, sizeof(to));
  to.sll_family = AF_PACKET;
  to.sll_protocol = htons(ETH_P_IP);
  to.sll_ifindex = lo;
  to.sll_halen = 6;
  memcpy(ip + 28, "no head", 7);
  const long s2 = sendto(cooked, ip, 20 + 8 + 7, 0, (struct sockaddr*)&to, sizeof(to));
  show_frame("sent without a link header", raw, 14);
  const long s3 = send(cooked, ip, 20 + 8 + 7, 0);
  printf(" without a link header: sent %ld; with no address (the loopback has none): %ld\n", s2, s3);
  show_frame("and that one", raw, 14);

  /* what a packet socket is not */
  const int l = listen(raw, 1), l_error = errno;
  const int stream = socket(AF_PACKET, SOCK_STREAM, htons(ETH_P_IP)), stream_error = errno;
  printf(" listen: %d errno %d; SOCK_STREAM: %d errno %d\n", l, l_error, stream, stream_error);
  close(cooked);
  close(raw);
  printf("packet ok\n");
  return 0;
}
