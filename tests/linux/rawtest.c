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
/* Raw and ICMP sockets the way ping and traceroute use them, over the loopback so that nothing outside the machine is needed: echo ("ping") sockets,
 * raw ICMP and ICMPv6 sockets with their filters, IP_TTL, IP_HDRINCL and IPPROTO_RAW. Built static with glibc and run on the real kernel by
 * tests/fork_qemu.sh; what it prints must be what it prints on Linux, where it needs the right to open raw sockets (the script runs it in a network
 * namespace of its own for that). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* (the C library has no header for these two: linux/icmp.h does) */
struct icmp_filter { unsigned data; };
#define ICMP_FILTER 1

static unsigned short checksum(const unsigned char* bytes, int size) {
  unsigned long sum = 0;
  for (int i = 0; i < size; i += 2) sum += (unsigned long)bytes[i] << 8 | (i + 1 < size ? bytes[i + 1] : 0);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (unsigned short)~sum;
}

/* An ICMP message into `out`: type, identifier, sequence, text; with its checksum if `sum`. Its size. */
static int icmp(unsigned char* out, int type, int identifier, int sequence, const char* text, int sum) {
  const int size = 8 + (int)strlen(text);
  memset(out, 0, 8);
  out[0] = (unsigned char)type;
  out[4] = (unsigned char)(identifier >> 8); out[5] = (unsigned char)identifier;
  out[6] = (unsigned char)(sequence >> 8); out[7] = (unsigned char)sequence;
  memcpy(out + 8, text, strlen(text));
  if (sum) { const unsigned short c = checksum(out, size); out[2] = (unsigned char)(c >> 8); out[3] = (unsigned char)c; }
  return size;
}

static struct sockaddr_in local4(int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

static int ready(int fd, int milliseconds) {
  struct pollfd wait = {fd, POLLIN, 0};
  return poll(&wait, 1, milliseconds) == 1;
}

/* One IPv4 packet from a raw socket, said in a line. */
static void show_packet(const char* what, int fd) {
  unsigned char packet[256];
  struct sockaddr_in from;
  socklen_t size = sizeof(from);
  if (!ready(fd, 1000)) { printf(" %s: nothing came\n", what); return; }
  const long n = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr*)&from, &size);
  if (n < 28) { printf(" %s: %ld errno %d\n", what, n, errno); return; }
  char source[32], destination[32];
  inet_ntop(AF_INET, packet + 12, source, sizeof(source));
  inet_ntop(AF_INET, packet + 16, destination, sizeof(destination));
  printf(" %s: %ld bytes, header %d, length %d, ttl %d, protocol %d, %s to %s, ICMP type %d id %#x sequence %d \"%.*s\", from %s port %d\n", what, n, packet[0], packet[2] << 8 | packet[3],
         packet[8], packet[9], source, destination, packet[20], packet[24] << 8 | packet[25], packet[26] << 8 | packet[27], (int)(n - 28), packet + 28, inet_ntoa(from.sin_addr),
         ntohs(from.sin_port));
}

static void echo_sockets(void) {
  printf("echo sockets:\n");
  unsigned char message[64], reply[64];
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  int type = 0, protocol = 0;
  socklen_t size = sizeof(type);
  getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size);
  size = sizeof(protocol);
  getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &size);
  struct sockaddr_in to = local4(0), from, own;
  int n = icmp(message, ICMP_ECHO, 0x1234, 7, "are you there?", 0);
  const long sent = sendto(fd, message, n, 0, (struct sockaddr*)&to, sizeof(to));
  size = sizeof(own);
  getsockname(fd, (struct sockaddr*)&own, &size);
  size = sizeof(from);
  const long got = ready(fd, 1000) ? recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr*)&from, &size) : -2;
  printf(" %s, type %d protocol %d; sent %ld, got %ld: type %d, the socket's own identifier (not the one written): %s, sequence %d, \"%.*s\", checksum %s, from %s\n",
         fd >= 0 ? "made" : "refused", type, protocol, sent, got, reply[0], (reply[4] << 8 | reply[5]) == ntohs(own.sin_port) && ntohs(own.sin_port) != 0x1234 ? "yes" : "no",
         reply[6] << 8 | reply[7], got > 8 ? (int)(got - 8) : 0, reply + 8, got > 0 && checksum(reply, (int)got) == 0 ? "good" : "bad", inet_ntoa(from.sin_addr));
  /* what is not an echo request */
  n = icmp(message, ICMP_ECHOREPLY, 1, 1, "a reply", 1);
  const long wrong = sendto(fd, message, n, 0, (struct sockaddr*)&to, sizeof(to)), wrong_error = errno;
  const long small = sendto(fd, message, 4, 0, (struct sockaddr*)&to, sizeof(to)), small_error = errno;
  printf(" not a request: %ld errno %ld; too short: %ld errno %ld\n", wrong, wrong_error, small, small_error);
  /* an identifier of the program's choosing */
  const int second = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  struct sockaddr_in chosen;
  memset(&chosen, 0, sizeof(chosen));
  chosen.sin_family = AF_INET;
  chosen.sin_port = htons(777);
  const int b1 = bind(second, (struct sockaddr*)&chosen, sizeof(chosen));
  n = icmp(message, ICMP_ECHO, 1, 2, "mine", 0);
  sendto(second, message, n, 0, (struct sockaddr*)&to, sizeof(to));
  const long mine = ready(second, 1000) ? recv(second, reply, sizeof(reply), 0) : -2;
  printf(" bound to 777: %d, reply %ld with identifier %d; the first socket hears of it: %s\n", b1, mine, reply[4] << 8 | reply[5], ready(fd, 100) ? "yes" : "no");
  close(second);
  /* connected: send() and recv() */
  connect(fd, (struct sockaddr*)&to, sizeof(to));
  n = icmp(message, ICMP_ECHO, 0, 9, "connected", 0);
  const long c1 = send(fd, message, n, 0);
  const long c2 = ready(fd, 1000) ? recv(fd, reply, sizeof(reply), 0) : -2;
  printf(" connected: sent %ld, got %ld sequence %d\n", c1, c2, reply[6] << 8 | reply[7]);
  close(fd);
  /* IPv6 */
  const int six = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
  struct sockaddr_in6 to6, from6;
  memset(&to6, 0, sizeof(to6));
  to6.sin6_family = AF_INET6;
  to6.sin6_addr = in6addr_loopback;
  n = icmp(message, ICMP6_ECHO_REQUEST, 0, 3, "six", 0);
  const long sent6 = sendto(six, message, n, 0, (struct sockaddr*)&to6, sizeof(to6));
  size = sizeof(from6);
  const long got6 = ready(six, 1000) ? recvfrom(six, reply, sizeof(reply), 0, (struct sockaddr*)&from6, &size) : -2;
  char text[64] = "";
  inet_ntop(AF_INET6, &from6.sin6_addr, text, sizeof(text));
  printf(" IPv6: %s, sent %ld, got %ld: type %d sequence %d \"%.*s\" from %s\n", six >= 0 ? "made" : "refused", sent6, got6, reply[0], reply[6] << 8 | reply[7], got6 > 8 ? (int)(got6 - 8) : 0, reply + 8,
         text);
  close(six);
}

static void raw_sockets(void) {
  printf("raw sockets:\n");
  unsigned char message[128];
  const int none = socket(AF_INET, SOCK_RAW, 0), none_error = errno;
  const int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  int type = 0, protocol = 0, ttl = 0;
  socklen_t size = sizeof(type);
  getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size);
  size = sizeof(protocol);
  getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &size);
  size = sizeof(ttl);
  getsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, &size);
  printf(" %s, type %d protocol %d ttl %d; without a protocol: %d errno %d\n", fd >= 0 ? "made" : "refused", type, protocol, ttl, none, none_error);
  struct sockaddr_in to = local4(0);
  /* over the loopback a raw socket hears its own request and the answer to it */
  int n = icmp(message, ICMP_ECHO, 0x5151, 1, "raw request", 1);
  const long sent = sendto(fd, message, n, 0, (struct sockaddr*)&to, sizeof(to));
  printf(" sent %ld\n", sent);
  show_packet("first", fd);
  show_packet("second", fd);
  /* ICMP_FILTER: the requests are kept away */
  struct icmp_filter filter;
  filter.data = 1u << ICMP_ECHO;
  const int f1 = setsockopt(fd, SOL_RAW, ICMP_FILTER, &filter, sizeof(filter));
  n = icmp(message, ICMP_ECHO, 0x5151, 2, "filtered", 1);
  sendto(fd, message, n, 0, (struct sockaddr*)&to, sizeof(to));
  printf(" filter %d\n", f1);
  show_packet("only", fd);
  printf(" more: %s\n", ready(fd, 100) ? "yes" : "no");
  /* IP_TTL: how far a packet may go; the answer comes with the usual one */
  const int three = 3;
  const int t1 = setsockopt(fd, IPPROTO_IP, IP_TTL, &three, sizeof(three));
  size = sizeof(ttl);
  getsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, &size);
  filter.data = 0;
  setsockopt(fd, SOL_RAW, ICMP_FILTER, &filter, sizeof(filter));
  n = icmp(message, ICMP_ECHO, 0x5151, 3, "short lived", 1);
  sendto(fd, message, n, 0, (struct sockaddr*)&to, sizeof(to));
  printf(" ttl %d: now %d\n", t1, ttl);
  show_packet("the request", fd);
  show_packet("the answer", fd);
  /* IP_HDRINCL: a whole packet, here a UDP datagram for a socket of this program (no UDP checksum, which IPv4 allows) */
  const int udp = socket(AF_INET, SOCK_DGRAM, 0), whole = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
  struct sockaddr_in at = local4(24950), from;
  const int on = 1;
  bind(udp, (struct sockaddr*)&at, sizeof(at));
  const int h1 = setsockopt(whole, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on));
  unsigned char packet[64];
  memset(packet, 0, sizeof(packet));
  packet[0] = 0x45; packet[8] = 9; packet[9] = IPPROTO_UDP;
  packet[16] = 127; packet[19] = 1;                                        /* to 127.0.0.1; from whoever the kernel says */
  packet[20] = 12345 >> 8; packet[21] = 12345 & 0xFF; packet[22] = 24950 >> 8; packet[23] = 24950 & 0xFF; packet[25] = 13;
  memcpy(packet + 28, "hello", 5);
  packet[3] = 33;
  const long h2 = sendto(whole, packet, 33, 0, (struct sockaddr*)&to, sizeof(to));
  char text[16] = "";
  size = sizeof(from);
  const long h3 = ready(udp, 1000) ? recvfrom(udp, text, sizeof(text) - 1, 0, (struct sockaddr*)&from, &size) : -2;
  const long h4 = sendto(whole, packet, 12, 0, (struct sockaddr*)&to, sizeof(to)), h4_error = errno;
  printf(" a whole packet: option %d, sent %ld, the UDP socket got %ld \"%s\" from %s port %d; half a header: %ld errno %ld\n", h1, h2, h3, text, inet_ntoa(from.sin_addr), ntohs(from.sin_port),
         h4, h4_error);
  close(whole);
  close(udp);
  /* IPPROTO_RAW: only sends, always whole packets; the raw ICMP socket hears what it sent and the answer */
  const int sender = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  int included = 0;
  size = sizeof(included);
  getsockopt(sender, IPPROTO_IP, IP_HDRINCL, &included, &size);
  setsockopt(fd, IPPROTO_IP, IP_TTL, &(int){64}, sizeof(int));
  memset(packet, 0, sizeof(packet));
  packet[0] = 0x45; packet[8] = 5; packet[9] = IPPROTO_ICMP;
  packet[16] = 127; packet[19] = 1;
  n = icmp(packet + 20, ICMP_ECHO, 0x6161, 1, "by hand", 1);
  packet[3] = (unsigned char)(20 + n);
  const long r1 = sendto(sender, packet, 20 + n, 0, (struct sockaddr*)&to, sizeof(to));
  printf(" IPPROTO_RAW: headers included %d, sent %ld\n", included, r1);
  show_packet("by hand", fd);
  show_packet("its answer", fd);
  printf(" the sender hears: %s\n", ready(sender, 100) ? "something" : "nothing");
  close(sender);
  close(fd);
  /* ICMPv6: no IP header in what comes, the checksum is the kernel's to write, and a filter that lets only echo replies through */
  const int six = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
  struct icmp6_filter filter6;
  ICMP6_FILTER_SETBLOCKALL(&filter6);
  ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter6);
  const int s1 = setsockopt(six, IPPROTO_ICMPV6, ICMP6_FILTER, &filter6, sizeof(filter6));
  struct sockaddr_in6 to6, from6;
  memset(&to6, 0, sizeof(to6));
  to6.sin6_family = AF_INET6;
  to6.sin6_addr = in6addr_loopback;
  n = icmp(message, ICMP6_ECHO_REQUEST, 0x7171, 4, "raw six", 0);
  const long s2 = sendto(six, message, n, 0, (struct sockaddr*)&to6, sizeof(to6));
  unsigned char reply[64];
  size = sizeof(from6);
  const long s3 = ready(six, 1000) ? recvfrom(six, reply, sizeof(reply), 0, (struct sockaddr*)&from6, &size) : -2;
  char source[64] = "";
  inet_ntop(AF_INET6, &from6.sin6_addr, source, sizeof(source));
  printf(" ICMPv6: filter %d, sent %ld, got %ld: type %d id %#x sequence %d \"%.*s\" from %s; more: %s\n", s1, s2, s3, reply[0], reply[4] << 8 | reply[5], reply[6] << 8 | reply[7],
         s3 > 8 ? (int)(s3 - 8) : 0, reply + 8, source, ready(six, 100) ? "yes" : "no");
  close(six);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  echo_sockets();
  raw_sockets();
  printf("raw ok\n");
  return 0;
}
