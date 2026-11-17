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
/* Promiscuous mode and all-multicast as programs ask for them: PACKET_ADD_MEMBERSHIP on a packet socket, and the interface's flags (SIOCGIFFLAGS,
 * SIOCSIFFLAGS). Over the loopback, which has no hardware to ask but keeps the flags all the same. Built static with glibc and run on the real kernel
 * by tests/fork_qemu.sh; what it prints must be what it prints on Linux, where it needs the rights to do this (the script runs it in a network
 * namespace of its own for that). */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int lo;

static const char* flags(void) {
  static char text[64];
  struct ifreq request;
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&request, 0, sizeof(request));
  strcpy(request.ifr_name, "lo");
  ioctl(fd, SIOCGIFFLAGS, &request);
  close(fd);
  snprintf(text, sizeof(text), "%s%s", request.ifr_flags & IFF_PROMISC ? "promiscuous" : "not promiscuous", request.ifr_flags & IFF_ALLMULTI ? ", all multicast" : "");
  return text;
}

static int member(int fd, int option, int interface, int type, int size) {
  struct packet_mreq request;
  memset(&request, 0, sizeof(request));
  request.mr_ifindex = interface;
  request.mr_type = (unsigned short)type;
  request.mr_alen = (unsigned short)size;
  request.mr_address[0] = 1;
  return setsockopt(fd, SOL_PACKET, option, &request, sizeof(request)) == 0 ? 0 : -errno;
}

static int set_flag(int flag, int on) {
  struct ifreq request;
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&request, 0, sizeof(request));
  strcpy(request.ifr_name, "lo");
  ioctl(fd, SIOCGIFFLAGS, &request);
  request.ifr_flags = (short)(on ? request.ifr_flags | flag : request.ifr_flags & ~flag);
  const int r = ioctl(fd, SIOCSIFFLAGS, &request) == 0 ? 0 : -errno;
  close(fd);
  return r;
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  lo = (int)if_nametoindex("lo");
  const int a = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)), b = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  printf("at first: %s\n", flags());
  /* asked for by a socket; twice is dropped twice */
  const int a1 = member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  printf("a socket asks: %d: %s\n", a1, flags());
  const int a2 = member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0), d1 = member(a, PACKET_DROP_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  printf("asks again and lets go once: %d %d: %s\n", a2, d1, flags());
  const int d2 = member(a, PACKET_DROP_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  printf("lets go again: %d: %s\n", d2, flags());
  printf("lets go of what it does not have: %d; an interface there is not: %d; a kind there is not: %d\n", member(a, PACKET_DROP_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0),
         member(a, PACKET_ADD_MEMBERSHIP, 987654, PACKET_MR_PROMISC, 0), member(a, PACKET_ADD_MEMBERSHIP, lo, 9, 0));
  /* two sockets; one of them closes */
  member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  member(b, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  member(a, PACKET_DROP_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  printf("two ask, one lets go: %s\n", flags());
  close(b);
  printf("the other closes: %s\n", flags());
  /* every multicast frame, and one group */
  const int m1 = member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_ALLMULTI, 0);
  printf("all multicast: %d: %s\n", m1, flags());
  const int m2 = member(a, PACKET_DROP_MEMBERSHIP, lo, PACKET_MR_ALLMULTI, 0);
  printf("and not: %d: %s\n", m2, flags());
  const int g1 = member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_MULTICAST, 6), g2 = member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_MULTICAST, 4);
  const int g3 = member(a, PACKET_DROP_MEMBERSHIP, lo, PACKET_MR_MULTICAST, 6);
  printf("one group: %d; an address of the wrong size: %d; let go: %d: %s\n", g1, g2, g3, flags());
  /* the flag itself, which stays whoever set it */
  const int s1 = set_flag(IFF_PROMISC, 1);
  printf("the flag set: %d: %s\n", s1, flags());
  member(a, PACKET_ADD_MEMBERSHIP, lo, PACKET_MR_PROMISC, 0);
  const int s2 = set_flag(IFF_PROMISC, 0);
  printf("taken back while a socket still asks: %d: %s\n", s2, flags());
  close(a);
  printf("the socket closes: %s\n", flags());
  const int s3 = set_flag(IFF_ALLMULTI, 1);
  printf("all multicast as a flag: %d: %s\n", s3, flags());
  const int s4 = set_flag(IFF_ALLMULTI, 0);
  printf("and back: %d: %s\n", s4, flags());
  printf("promiscuous ok\n");
  return 0;
}
