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
/* The memory-mapped rings of AF_PACKET sockets (PACKET_RX_RING, PACKET_TX_RING) in their three layouts, over the loopback: frames found in the mapped
 * memory, given back, and sent from it. Built static with glibc and run on the real kernel by tests/fork_qemu.sh; what it prints must be what it prints
 * on Linux, where it needs the right to open packet sockets (the script runs it in a network namespace of its own for that). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 24980
#define BLOCK 4096
#define BLOCKS 4
#define FRAME 2048

static int udp;

static int ready(int fd, int milliseconds) {
  struct pollfd wait = {fd, POLLIN, 0};
  return poll(&wait, 1, milliseconds) == 1;
}

/* Waits until the 32-bit status word at `status` is the program's (bit 0). poll() is what moves frames into a ring where nothing else does, and it
 * returns at once while an earlier frame has not been given back, so it is asked again and again for up to a second. */
static int arrived(int fd, const volatile unsigned* status) {
  for (int tries = 0; tries < 500 && !(*status & TP_STATUS_USER); ++tries) {
    ready(fd, 2);
    usleep(2000);
  }
  return (*status & TP_STATUS_USER) != 0;
}

static void say(const char* text) {
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(PORT);
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sendto(udp, text, strlen(text), 0, (struct sockaddr*)&to, sizeof(to));
}

static int open_on_lo(int type) {
  const int fd = socket(AF_PACKET, type, htons(ETH_P_IP));
  struct sockaddr_ll where;
  memset(&where, 0, sizeof(where));
  where.sll_family = AF_PACKET;
  where.sll_ifindex = (int)if_nametoindex("lo");
  bind(fd, (struct sockaddr*)&where, sizeof(where));
  return fd;
}

static void show_address(const struct sockaddr_ll* a) {
  printf("family %d protocol %#x interface %d hardware type %d packet type %d", a->sll_family, ntohs(a->sll_protocol), a->sll_ifindex, a->sll_hatype, a->sll_pkttype);
}

/* The first two layouts: a ring of frames, each with a status the kernel sets and the program clears. */
static void frames(int version, int type) {
  const int fd = open_on_lo(type);
  int length = version;
  socklen_t size = sizeof(length);
  const int v = setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version));
  const int h = getsockopt(fd, SOL_PACKET, PACKET_HDRLEN, &length, &size);
  struct tpacket_req request = {BLOCK, BLOCKS, FRAME, BLOCKS * (BLOCK / FRAME)};
  struct tpacket_req wrong = {BLOCK, BLOCKS, FRAME, BLOCKS * (BLOCK / FRAME) + 1};
  const int r0 = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &wrong, sizeof(wrong)), r0_error = errno;
  const int r1 = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &request, sizeof(request));
  int other = version == 0 ? 1 : 0;
  const int v2 = setsockopt(fd, SOL_PACKET, PACKET_VERSION, &other, sizeof(other)), v2_error = errno;
  void* too_small = mmap(0, BLOCK, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  const int small_error = errno;
  unsigned char* ring = mmap(0, BLOCK * BLOCKS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  printf("version %d, %s: set %d, header %d bytes (%d); a ring that does not add up: %d errno %d; a ring: %d; another version now: %d errno %d; mapping part of it: %s errno %d; all of it: %s\n",
         version, type == SOCK_RAW ? "whole frames" : "without link headers", v, length, h, r0, r0_error, r1, v2, v2_error, too_small == MAP_FAILED ? "refused" : "mapped", small_error,
         ring == MAP_FAILED ? "refused" : "mapped");
  if (ring == MAP_FAILED) { close(fd); return; }
  const char* texts[3] = {"first", "second one", "third"};
  for (int i = 0; i < 3; ++i) {
    say(texts[i]);
    unsigned char* slot = ring + i * FRAME;
    const int woke = ready(fd, 1000) && arrived(fd, (const volatile unsigned*)slot);
    unsigned long status;
    unsigned len, snap, mac, net;
    if (version == 0) {
      const struct tpacket_hdr* head = (const struct tpacket_hdr*)slot;
      status = head->tp_status; len = head->tp_len; snap = head->tp_snaplen; mac = head->tp_mac; net = head->tp_net;
    } else {
      const struct tpacket2_hdr* head = (const struct tpacket2_hdr*)slot;
      status = head->tp_status; len = head->tp_len; snap = head->tp_snaplen; mac = head->tp_mac; net = head->tp_net;
    }
    const struct sockaddr_ll* from = (const struct sockaddr_ll*)(slot + TPACKET_ALIGN(length));
    const unsigned char* ip = slot + net;
    printf(" frame %d: woke %d, the program's %lu, length %u kept %u, link header at %u, network header at %u, IP version %d to port %d \"%.*s\"; ", i, woke, status & 1, len, snap, mac, net,
           ip[0] >> 4, ip[22] << 8 | ip[23], (int)(snap - (net - mac) - 28), ip + 28);
    show_address(from);
    printf("\n");
  }
  /* until the program gives a frame back, poll goes on saying there is something */
  const int before = ready(fd, 50);
  for (int i = 0; i < 3; ++i) *(unsigned*)(ring + i * FRAME) = TP_STATUS_KERNEL;
  if (version == 0) for (int i = 0; i < 3; ++i) ((struct tpacket_hdr*)(ring + i * FRAME))->tp_status = TP_STATUS_KERNEL;
  const int after = ready(fd, 50);
  char nothing[64];
  const long by_recv = recv(fd, nothing, sizeof(nothing), MSG_DONTWAIT);
  struct tpacket_stats stats = {0, 0};
  size = sizeof(stats);
  getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &stats, &size);
  /* a ring that is mapped stays; unmapped, it can be taken away */
  struct tpacket_req none = {0, 0, 0, 0};
  const int gone0 = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &none, sizeof(none)), gone0_error = errno;
  munmap(ring, BLOCK * BLOCKS);
  const int gone1 = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &none, sizeof(none));
  printf(" something to read before giving them back: %d, after: %d; recv: %ld; frames counted %u, lost %u; taking a mapped ring away: %d errno %d; unmapped: %d\n", before, after, by_recv,
         stats.tp_packets, stats.tp_drops, gone0, gone0_error, gone1);
  close(fd);
}

/* The third layout: blocks of frames, handed over when they are full or have waited long enough. */
static void blocks(void) {
  const int fd = open_on_lo(SOCK_RAW);
  int version = TPACKET_V3, length = TPACKET_V3;
  socklen_t size = sizeof(length);
  setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version));
  getsockopt(fd, SOL_PACKET, PACKET_HDRLEN, &length, &size);
  struct tpacket_req3 request;
  memset(&request, 0, sizeof(request));
  request.tp_block_size = BLOCK; request.tp_block_nr = BLOCKS; request.tp_frame_size = FRAME; request.tp_frame_nr = BLOCKS * (BLOCK / FRAME);
  request.tp_retire_blk_tov = 100;
  const int r0 = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &request, sizeof(struct tpacket_req)), r0_error = errno;
  const int r1 = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &request, sizeof(request));
  unsigned char* ring = mmap(0, BLOCK * BLOCKS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  printf("version 2: header %d bytes; the short request: %d errno %d; a ring: %d; %s\n", length, r0, r0_error, r1, ring == MAP_FAILED ? "not mapped" : "mapped");
  if (ring == MAP_FAILED) { close(fd); return; }
  say("one");
  say("two, a little longer");
  say("three");
  const int woke = ready(fd, 2000);
  const struct tpacket_block_desc* block = (const struct tpacket_block_desc*)ring;
  printf(" woke %d: block version %u, private part at %u, the program's %u, by the clock %u, frames %u, the first at %u, number %llu\n", woke, block->version, block->offset_to_priv,
         block->hdr.bh1.block_status & TP_STATUS_USER, (block->hdr.bh1.block_status & TP_STATUS_BLK_TMO) != 0, block->hdr.bh1.num_pkts, block->hdr.bh1.offset_to_first_pkt,
         (unsigned long long)block->hdr.bh1.seq_num);
  const struct tpacket3_hdr* frame = (const struct tpacket3_hdr*)(ring + block->hdr.bh1.offset_to_first_pkt);
  for (unsigned i = 0; i < block->hdr.bh1.num_pkts && i < 8; ++i) {
    const unsigned char* ip = (const unsigned char*)frame + frame->tp_net;
    const struct sockaddr_ll* from = (const struct sockaddr_ll*)((const unsigned char*)frame + TPACKET_ALIGN(length));
    printf(" frame %u: next in %u, length %u kept %u, link header at %u, network header at %u, \"%.*s\"; ", i, frame->tp_next_offset, frame->tp_len, frame->tp_snaplen, frame->tp_mac,
           frame->tp_net, (int)(frame->tp_snaplen - (frame->tp_net - frame->tp_mac) - 28), ip + 28);
    show_address(from);
    printf("\n");
    frame = (const struct tpacket3_hdr*)((const unsigned char*)frame + frame->tp_next_offset);
  }
  const int before = ready(fd, 50);
  ((struct tpacket_block_desc*)ring)->hdr.bh1.block_status = TP_STATUS_KERNEL;
  const int after = ready(fd, 50);
  /* the next block */
  say("four");
  const struct tpacket_block_desc* second = (const struct tpacket_block_desc*)(ring + BLOCK);
  const int again = ready(fd, 2000) && arrived(fd, (const volatile unsigned*)&second->hdr.bh1.block_status);
  struct tpacket_stats_v3 stats;
  memset(&stats, 0, sizeof(stats));
  size = sizeof(stats);
  getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &stats, &size);
  printf(" something to read before giving the block back: %d, after: %d; then woke %d: the second block the program's %u, frames %u, number %llu; frames counted %u, lost %u, full %u\n", before,
         after, again, second->hdr.bh1.block_status & TP_STATUS_USER, second->hdr.bh1.num_pkts, (unsigned long long)second->hdr.bh1.seq_num, stats.tp_packets, stats.tp_drops,
         stats.tp_freeze_q_cnt);
  munmap(ring, BLOCK * BLOCKS);
  close(fd);
}

/* A transmit ring: the program leaves frames in the mapped memory and send() sends them. Another packet socket hears them go by. */
static void transmit(void) {
  const int fd = open_on_lo(SOCK_RAW), listener = open_on_lo(SOCK_RAW);
  int version = TPACKET_V2;
  setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version));
  struct tpacket_req request = {BLOCK, 1, FRAME, 2};
  const int r = setsockopt(fd, SOL_PACKET, PACKET_TX_RING, &request, sizeof(request));
  unsigned char* ring = mmap(0, BLOCK, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  printf("a transmit ring: %d, %s\n", r, ring == MAP_FAILED ? "not mapped" : "mapped");
  if (ring == MAP_FAILED) return;
  const long idle = send(fd, 0, 0, 0);
  for (int i = 0; i < 2; ++i) {
    struct tpacket2_hdr* head = (struct tpacket2_hdr*)(ring + i * FRAME);
    unsigned char* frame = (unsigned char*)head + TPACKET_ALIGN(sizeof(*head));
    memset(frame, 0, 64);
    frame[12] = 0x08;
    unsigned char* ip = frame + 14;
    ip[0] = 0x45; ip[3] = 20 + 8 + 4; ip[8] = 64; ip[9] = 17;
    ip[12] = 127; ip[15] = 1; ip[16] = 127; ip[19] = 1;
    ip[22] = PORT >> 8; ip[23] = PORT & 0xFF; ip[25] = 12;
    memcpy(ip + 28, i == 0 ? "ring" : "RING", 4);
    head->tp_len = 14 + 20 + 8 + 4;
    head->tp_status = TP_STATUS_SEND_REQUEST;
  }
  const long sent = send(fd, 0, 0, 0);
  usleep(20000);
  printf(" with nothing left in it: %ld; with two frames: %ld; their status afterwards: %u %u\n", idle, sent, ((struct tpacket2_hdr*)ring)->tp_status,
         ((struct tpacket2_hdr*)(ring + FRAME))->tp_status);
  for (int i = 0; i < 2; ++i) {
    unsigned char heard[128];
    const long n = ready(listener, 1000) ? recv(listener, heard, sizeof(heard), 0) : -2;
    printf(" heard: %ld bytes \"%.*s\"\n", n, n >= 46 ? 4 : 0, heard + 42);
  }
  /* a frame longer than its place can hold */
  ((struct tpacket2_hdr*)ring)->tp_len = 5000;
  ((struct tpacket2_hdr*)ring)->tp_status = TP_STATUS_SEND_REQUEST;
  const long bad = send(fd, 0, 0, 0), bad_error = errno;
  printf(" a frame too long for its place: %ld errno %ld, status %u\n", bad, bad < 0 ? bad_error : 0, ((struct tpacket2_hdr*)ring)->tp_status);
  munmap(ring, BLOCK);
  close(listener);
  close(fd);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  udp = socket(AF_INET, SOCK_DGRAM, 0);
  /* (somebody has to listen on the port, or each datagram is answered with an ICMP message, which the rings would hold too) */
  const int listener = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in at;
  memset(&at, 0, sizeof(at));
  at.sin_family = AF_INET;
  at.sin_port = htons(PORT);
  at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(listener, (struct sockaddr*)&at, sizeof(at));
  frames(TPACKET_V1, SOCK_RAW);
  frames(TPACKET_V2, SOCK_RAW);
  frames(TPACKET_V2, SOCK_DGRAM);
  blocks();
  transmit();
  printf("rings ok\n");
  return 0;
}
