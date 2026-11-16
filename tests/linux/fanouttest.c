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
/* PACKET_FANOUT over the loopback: packet sockets that share one stream of frames, each frame going to one of them, in turn, by flow, by processor or
 * as a program says. Built static with glibc and run on the real kernel by tests/fork_qemu.sh; what it prints must be what it prints on Linux, where it
 * needs the right to open packet sockets (the script runs it in a network namespace of its own for that). */
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
#include <sys/socket.h>
#include <unistd.h>

#define PORT 24990

static int udp;

static void say(int count) {
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(PORT);
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  for (int i = 0; i < count; ++i) {
    sendto(udp, "frame", 5, 0, (struct sockaddr*)&to, sizeof(to));
    usleep(5000);
  }
  usleep(100000);
}

static int open_on_lo(void) {
  const int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_IP));
  struct sockaddr_ll where;
  memset(&where, 0, sizeof(where));
  where.sll_family = AF_PACKET;
  where.sll_ifindex = (int)if_nametoindex("lo");
  bind(fd, (struct sockaddr*)&where, sizeof(where));
  return fd;
}

static int join(int fd, int id, int type) {
  const int argument = id | type << 16;
  return setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &argument, sizeof(argument)) == 0 ? 0 : -errno;
}

/* How many frames wait at a socket (they are read away). */
static int count(int fd) {
  char frame[128];
  int n = 0;
  struct pollfd wait = {fd, POLLIN, 0};
  while (poll(&wait, 1, 20) == 1 && recv(fd, frame, sizeof(frame), 0) > 0) ++n;
  return n;
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  udp = socket(AF_INET, SOCK_DGRAM, 0);
  /* (somebody has to listen on the port, or each datagram is answered with an ICMP message, which the sockets would hear too) */
  const int listener = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at;
  memset(&at, 0, sizeof(at));
  at.sin_family = AF_INET;
  at.sin_port = htons(PORT);
  at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(listener, (struct sockaddr*)&at, sizeof(at));

  /* each in turn */
  const int a = open_on_lo(), b = open_on_lo(), alone = open_on_lo();
  int value = -1;
  socklen_t size = sizeof(value);
  getsockopt(a, SOL_PACKET, PACKET_FANOUT, &value, &size);
  const int j1 = join(a, 7, PACKET_FANOUT_LB), j2 = join(b, 7, PACKET_FANOUT_LB);
  int joined = -1;
  size = sizeof(joined);
  getsockopt(b, SOL_PACKET, PACKET_FANOUT, &joined, &size);
  printf("in turn: in no group %d; joining %d %d; in group %#x\n", value, j1, j2, joined);
  say(6);
  const int ca = count(a), cb = count(b), cn = count(alone);
  printf(" six frames: %d and %d; a socket in no group: %d\n", ca, cb, cn);
  /* what does not fit a group */
  const int other = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP)), nothing = socket(AF_PACKET, SOCK_RAW, 0);
  struct sockaddr_ll where;
  memset(&where, 0, sizeof(where));
  where.sll_family = AF_PACKET;
  where.sll_ifindex = (int)if_nametoindex("lo");
  bind(other, (struct sockaddr*)&where, sizeof(where));
  const int rebind = bind(a, (struct sockaddr*)&where, sizeof(where)) == 0 ? 0 : -errno;
  printf(" another way of sharing: %d; another protocol: %d; a second group: %d; a socket that listens for nothing: %d; a way there is not: %d; binding a member: %d\n",
         join(alone, 7, PACKET_FANOUT_HASH), join(other, 7, PACKET_FANOUT_LB), join(a, 8, PACKET_FANOUT_LB), join(nothing, 9, PACKET_FANOUT_LB), join(alone, 9, 9), rebind);
  /* a member that closes leaves the rest everything */
  close(b);
  say(4);
  printf(" with one member gone: %d\n", count(a));
  close(a);
  count(alone);

  /* by flow: one conversation, one socket */
  const int h1 = open_on_lo(), h2 = open_on_lo();
  const int k1 = join(h1, 20, PACKET_FANOUT_HASH), k2 = join(h2, 20, PACKET_FANOUT_HASH);
  say(6);
  const int c1 = count(h1), c2 = count(h2);
  printf("by flow: joining %d %d; six frames of one flow: all at one socket: %s (%d in all)\n", k1, k2, (c1 == 6 && c2 == 0) || (c1 == 0 && c2 == 6) ? "yes" : "no", c1 + c2);
  close(h1);
  close(h2);

  /* by processor: here, wherever the program runs, they all come by the same one */
  const int p1 = open_on_lo(), p2 = open_on_lo();
  join(p1, 21, PACKET_FANOUT_CPU);
  join(p2, 21, PACKET_FANOUT_CPU);
  say(4);
  const int d1 = count(p1), d2 = count(p2);
  printf("by processor: %d frames in all, at one socket or the other\n", d1 + d2);
  close(p1);
  close(p2);

  /* as a program says: one that always answers 1, the second member */
  const int g1 = open_on_lo(), g2 = open_on_lo();
  struct sock_filter code[] = {{0x06, 0, 0, 1}};
  struct sock_fprog program = {1, code};
  const int early = setsockopt(g1, SOL_PACKET, PACKET_FANOUT_DATA, &program, sizeof(program)) == 0 ? 0 : -errno;
  const int m1 = join(g1, 22, PACKET_FANOUT_CBPF), m2 = join(g2, 22, PACKET_FANOUT_CBPF);
  const int given = setsockopt(g1, SOL_PACKET, PACKET_FANOUT_DATA, &program, sizeof(program)) == 0 ? 0 : -errno;
  count(g1);
  count(g2);
  say(5);
  const int e1 = count(g1), e2 = count(g2);
  printf("by a program: before joining %d; joining %d %d; the program %d; five frames: %d and %d\n", early, m1, m2, given, e1, e2);
  close(g1);
  close(g2);

  /* a number of the kernel's choosing */
  const int u = open_on_lo();
  const int wrong = join(u, 5, PACKET_FANOUT_LB | PACKET_FANOUT_FLAG_UNIQUEID), right = join(u, 0, PACKET_FANOUT_LB | PACKET_FANOUT_FLAG_UNIQUEID);
  size = sizeof(joined);
  getsockopt(u, SOL_PACKET, PACKET_FANOUT, &joined, &size);
  printf("a number of the kernel's choosing: with one named %d, with none %d; the group shares in turn: %s\n", wrong, right, ((joined >> 16) & 0xFF) == PACKET_FANOUT_LB ? "yes" : "no");
  close(u);
  printf("fanout ok\n");
  return 0;
}
