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
/* AF_NETLINK the way programs use it: the C library's getifaddrs, if_nametoindex and if_indextoname, and requests made by hand (lists of interfaces,
 * addresses and routes, the route to one place, errors, acknowledgements). Built static with glibc and run on the real kernel by tests/fork_qemu.sh;
 * what it prints must be what it prints on Linux, so of the machine's own network only what every Linux machine has is printed (the loopback), and of
 * the rest only that it makes sense. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void by_the_library(void) {
  struct ifaddrs* list = 0;
  const int r = getifaddrs(&list);
  int entries = 0, named = 0, mapped = 0, packet = 0;
  printf("getifaddrs: %d\n", r);
  for (struct ifaddrs* a = list; a; a = a->ifa_next) {
    ++entries;
    if (a->ifa_name && a->ifa_name[0]) ++named;
    char back[IF_NAMESIZE] = "";
    const unsigned index = if_nametoindex(a->ifa_name);
    if (index != 0 && if_indextoname(index, back) && strcmp(back, a->ifa_name) == 0) ++mapped;
    if (a->ifa_addr && a->ifa_addr->sa_family == AF_PACKET) ++packet;
    if (strcmp(a->ifa_name, "lo") != 0 || !a->ifa_addr) continue;
    char text[64] = "", mask[64] = "";
    if (a->ifa_addr->sa_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, text, sizeof(text));
      if (a->ifa_netmask) inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_netmask)->sin_addr, mask, sizeof(mask));
      printf(" lo: %s mask %s%s%s%s\n", text, mask, a->ifa_flags & IFF_UP ? " up" : "", a->ifa_flags & IFF_LOOPBACK ? " loopback" : "", a->ifa_flags & IFF_RUNNING ? " running" : "");
    } else if (a->ifa_addr->sa_family == AF_INET6) {
      inet_ntop(AF_INET6, &((struct sockaddr_in6*)a->ifa_addr)->sin6_addr, text, sizeof(text));
      if (a->ifa_netmask) inet_ntop(AF_INET6, &((struct sockaddr_in6*)a->ifa_netmask)->sin6_addr, mask, sizeof(mask));
      printf(" lo: %s mask %s\n", text, mask);
    } else if (a->ifa_addr->sa_family == AF_PACKET) {
      printf(" lo: the interface itself, index %u, data %s\n", if_nametoindex("lo"), a->ifa_data ? "given" : "missing");
    }
  }
  printf(" %s; every entry named: %s; every name maps to a number and back: %s; an entry for each interface itself: %s\n", entries >= 3 ? "three entries or more" : "too few entries",
         named == entries ? "yes" : "no", mapped == entries ? "yes" : "no", packet >= 1 ? "yes" : "no");
  freeifaddrs(list);
  char name[IF_NAMESIZE] = "";
  const unsigned none = if_nametoindex("nosuchif0");
  const int none_error = errno;
  printf(" if_nametoindex(lo) %u, if_indextoname(1) %s, a name there is not: %u errno %d\n", if_nametoindex("lo"), if_indextoname(1, name) ? name : "(none)", none, none_error);
  struct if_nameindex* all = if_nameindex();
  int listed = 0, lo_listed = 0;
  for (struct if_nameindex* i = all; i && i->if_index != 0; ++i) {
    ++listed;
    if (i->if_index == 1 && strcmp(i->if_name, "lo") == 0) lo_listed = 1;
  }
  printf(" if_nameindex: %s, lo among them: %s\n", listed >= 1 ? "a list" : "nothing", lo_listed ? "yes" : "no");
  if (all) if_freenameindex(all);
}

static int netlink_open(int protocol) {
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
  struct sockaddr_nl me;
  memset(&me, 0, sizeof(me));
  me.nl_family = AF_NETLINK;
  if (fd >= 0) bind(fd, (struct sockaddr*)&me, sizeof(me));
  return fd;
}

struct tally { int datagrams, messages, done, errors, error, bad, lo, other_sender, wrong_sequence, wrong_port; };

/* Reads the answers to request `sequence` until the list ends (or the one message that is not a list has come); `each` sees every message. */
static struct tally answers(int fd, unsigned sequence, unsigned port, void (*each)(const struct nlmsghdr*, struct tally*)) {
  struct tally t;
  memset(&t, 0, sizeof(t));
  char buffer[16384];
  for (;;) {
    struct sockaddr_nl from;
    socklen_t size = sizeof(from);
    struct pollfd wait = {fd, POLLIN, 0};
    if (poll(&wait, 1, 2000) != 1) break;
    const long n = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&from, &size);
    if (n <= 0) break;
    ++t.datagrams;
    if (from.nl_pid != 0 || from.nl_family != AF_NETLINK || size != sizeof(from)) ++t.other_sender;
    int more = 0;
    long left = n;
    const struct nlmsghdr* h = (const struct nlmsghdr*)buffer;
    for (; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
      ++t.messages;
      if (h->nlmsg_seq != sequence) ++t.wrong_sequence;
      if (h->nlmsg_pid != port) ++t.wrong_port;
      if (h->nlmsg_type == NLMSG_DONE) { ++t.done; continue; }
      if (h->nlmsg_type == NLMSG_ERROR) { ++t.errors; t.error = ((const struct nlmsgerr*)NLMSG_DATA(h))->error; continue; }
      if (h->nlmsg_flags & NLM_F_MULTI) more = 1;
      if (each) each(h, &t);
    }
    if (left != 0) ++t.bad;
    if (!more || t.done) break;
  }
  return t;
}

static unsigned own_port(int fd) {
  struct sockaddr_nl me;
  socklen_t size = sizeof(me);
  memset(&me, 0, sizeof(me));
  getsockname(fd, (struct sockaddr*)&me, &size);
  return me.nl_pid;
}

static long ask(int fd, unsigned short type, unsigned short flags, unsigned sequence, const void* payload, size_t size) {
  char buffer[256];
  struct nlmsghdr* h = (struct nlmsghdr*)buffer;
  memset(buffer, 0, sizeof(buffer));
  h->nlmsg_len = NLMSG_LENGTH(size);
  h->nlmsg_type = type;
  h->nlmsg_flags = flags;
  h->nlmsg_seq = sequence;
  memcpy(NLMSG_DATA(h), payload, size);
  struct sockaddr_nl kernel;
  memset(&kernel, 0, sizeof(kernel));
  kernel.nl_family = AF_NETLINK;
  return sendto(fd, buffer, h->nlmsg_len, 0, (struct sockaddr*)&kernel, sizeof(kernel));
}

static void a_link(const struct nlmsghdr* h, struct tally* t) {
  const struct ifinfomsg* info = NLMSG_DATA(h);
  if (h->nlmsg_type != RTM_NEWLINK) { ++t->bad; return; }
  long left = (long)h->nlmsg_len - (long)NLMSG_LENGTH(sizeof(*info));
  for (const struct rtattr* a = IFLA_RTA(info); RTA_OK(a, left); a = RTA_NEXT(a, left)) {
    if (a->rta_type != IFLA_IFNAME || strcmp(RTA_DATA(a), "lo") != 0) continue;
    ++t->lo;
    printf(" lo: index %d, type %d,%s%s mtu", info->ifi_index, info->ifi_type, info->ifi_flags & IFF_UP ? " up" : "", info->ifi_flags & IFF_LOOPBACK ? " loopback" : "");
  }
  left = (long)h->nlmsg_len - (long)NLMSG_LENGTH(sizeof(*info));
  for (const struct rtattr* a = IFLA_RTA(info); RTA_OK(a, left); a = RTA_NEXT(a, left))
    if (a->rta_type == IFLA_MTU && info->ifi_index == 1) printf(" %u\n", *(const unsigned*)RTA_DATA(a));
}

static void an_address(const struct nlmsghdr* h, struct tally* t) {
  const struct ifaddrmsg* info = NLMSG_DATA(h);
  if (h->nlmsg_type != RTM_NEWADDR) { ++t->bad; return; }
  if (info->ifa_index != 1) return;
  long left = (long)h->nlmsg_len - (long)NLMSG_LENGTH(sizeof(*info));
  for (const struct rtattr* a = IFA_RTA(info); RTA_OK(a, left); a = RTA_NEXT(a, left)) {
    if (a->rta_type != IFA_ADDRESS) continue;
    char text[64] = "";
    inet_ntop(info->ifa_family, RTA_DATA(a), text, sizeof(text));
    printf(" lo: %s/%d scope %d\n", text, info->ifa_prefixlen, info->ifa_scope);
    ++t->lo;
  }
}

static void a_route(const struct nlmsghdr* h, struct tally* t) {
  const struct rtmsg* info = NLMSG_DATA(h);
  if (h->nlmsg_type != RTM_NEWROUTE) { ++t->bad; return; }
  long left = (long)h->nlmsg_len - (long)NLMSG_LENGTH(sizeof(*info));
  for (const struct rtattr* a = RTM_RTA(info); RTA_OK(a, left); a = RTA_NEXT(a, left)) {
    char text[64] = "";
    if (a->rta_type == RTA_DST) printf(" to %s/%d", inet_ntop(info->rtm_family, RTA_DATA(a), text, sizeof(text)), info->rtm_dst_len);
    if (a->rta_type == RTA_OIF) printf(" by interface %d", *(const int*)RTA_DATA(a));
    if (a->rta_type == RTA_PREFSRC) printf(" from %s", inet_ntop(info->rtm_family, RTA_DATA(a), text, sizeof(text)));
  }
  printf(" type %d\n", info->rtm_type);
  ++t->lo;
}

static void show(const char* what, struct tally t) {
  printf(" %s: %s, ended %s, errors %d (%d), badly formed %d, not from the kernel %d, wrong sequence %d, wrong port %d\n", what, t.messages > 0 ? "answered" : "no answer",
         t.done ? "with NLMSG_DONE" : "without NLMSG_DONE", t.errors, t.error, t.bad, t.other_sender, t.wrong_sequence, t.wrong_port);
}

static void by_hand(void) {
  printf("by hand:\n");
  const int fd = netlink_open(NETLINK_ROUTE);
  const unsigned port = own_port(fd);
  int type = 0, domain = 0, protocol = -1;
  socklen_t size = sizeof(type);
  getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size);
  size = sizeof(domain);
  getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &size);
  size = sizeof(protocol);
  getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &size);
  printf(" socket %s, bound to the process's number: %s, type %d domain %d protocol %d\n", fd >= 0 ? "made" : "refused", port == (unsigned)getpid() ? "yes" : "no", type, domain, protocol);
  struct rtgenmsg family = {AF_UNSPEC};
  ask(fd, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 1, &family, sizeof(family));
  show("interfaces", answers(fd, 1, port, a_link));
  ask(fd, RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 2, &family, sizeof(family));
  show("addresses", answers(fd, 2, port, an_address));
  struct rtmsg route;
  memset(&route, 0, sizeof(route));
  route.rtm_family = AF_INET;
  ask(fd, RTM_GETROUTE, NLM_F_REQUEST | NLM_F_DUMP, 3, &route, sizeof(route));
  show("routes", answers(fd, 3, port, 0));
  /* the route to the loopback: what every machine answers the same */
  struct { struct rtmsg head; struct rtattr attribute; unsigned char to[4]; } where;
  memset(&where, 0, sizeof(where));
  where.head.rtm_family = AF_INET;
  where.head.rtm_dst_len = 32;
  where.attribute.rta_type = RTA_DST;
  where.attribute.rta_len = RTA_LENGTH(4);
  where.to[0] = 127; where.to[3] = 1;
  ask(fd, RTM_GETROUTE, NLM_F_REQUEST, 4, &where, sizeof(where));
  show("the route to 127.0.0.1", answers(fd, 4, port, a_route));
  /* one interface by number; one there is not; with an acknowledgement asked for */
  struct ifinfomsg link;
  memset(&link, 0, sizeof(link));
  link.ifi_index = 1;
  ask(fd, RTM_GETLINK, NLM_F_REQUEST, 5, &link, sizeof(link));
  show("interface 1", answers(fd, 5, port, a_link));
  link.ifi_index = 987654;
  ask(fd, RTM_GETLINK, NLM_F_REQUEST, 6, &link, sizeof(link));
  show("an interface there is not", answers(fd, 6, port, 0));
  /* what the kernel does not know */
  ask(fd, 200, NLM_F_REQUEST, 7, &link, sizeof(link));
  show("a request nobody knows", answers(fd, 7, port, 0));
  ask(fd, NLMSG_NOOP, NLM_F_REQUEST | NLM_F_ACK, 8, &link, 0);
  show("nothing, to be acknowledged", answers(fd, 8, port, 0));
  /* a datagram is sized before it is read: MSG_PEEK | MSG_TRUNC, as the C library does */
  link.ifi_index = 987654;
  ask(fd, RTM_GETLINK, NLM_F_REQUEST, 9, &link, sizeof(link));
  char small[4];
  const long sized = recv(fd, small, sizeof(small), MSG_PEEK | MSG_TRUNC);
  int waiting = -1;
  char whole[256];
  const long got = recv(fd, whole, sizeof(whole), 0);
  const long after = recv(fd, whole, sizeof(whole), MSG_DONTWAIT);
  printf(" sized %ld, read %ld, then %ld errno %d\n", sized, got, after, errno);
  (void)waiting;
  /* another socket of the same process: another address; one that is taken; what a netlink socket cannot do */
  const int second = netlink_open(NETLINK_ROUTE);
  struct sockaddr_nl taken;
  memset(&taken, 0, sizeof(taken));
  taken.nl_family = AF_NETLINK;
  taken.nl_pid = port;
  const int third = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  const int in_use = bind(third, (struct sockaddr*)&taken, sizeof(taken)), in_use_error = errno;
  const int listening = listen(fd, 1), listening_error = errno;
  printf(" a second socket: its own number: %s; a number that is taken: %d errno %d; listen: %d errno %d; SOCK_STREAM: %d\n", own_port(second) != 0 && own_port(second) != port ? "yes" : "no",
         in_use, in_use_error, listening, listening_error, socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE));
  close(third);
  close(second);
  /* listening for changes: bound, and quiet while nothing changes */
  const int watcher = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
  struct sockaddr_nl groups;
  memset(&groups, 0, sizeof(groups));
  groups.nl_family = AF_NETLINK;
  groups.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
  const int watching = bind(watcher, (struct sockaddr*)&groups, sizeof(groups));
  struct pollfd wait = {watcher, POLLIN, 0};
  const int news = poll(&wait, 1, 300);
  size = sizeof(groups);
  getsockname(watcher, (struct sockaddr*)&groups, &size);
  printf(" a listener: bind %d, groups %#x, news in 300 ms: %d\n", watching, groups.nl_groups, news);
  close(watcher);
  close(fd);
  /* the generic controller: a family nobody has */
  const int generic = netlink_open(NETLINK_GENERIC);
  struct { struct genlmsghdr head; struct nlattr attribute; char name[12]; } find;
  memset(&find, 0, sizeof(find));
  find.head.cmd = CTRL_CMD_GETFAMILY;
  find.head.version = 1;
  find.attribute.nla_type = CTRL_ATTR_FAMILY_NAME;
  find.attribute.nla_len = NLA_HDRLEN + 10;
  memcpy(find.name, "tcos_none", 10);
  ask(generic, GENL_ID_CTRL, NLM_F_REQUEST, 1, &find, sizeof(find));
  show("a generic family there is not", answers(generic, 1, own_port(generic), 0));
  close(generic);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  by_the_library();
  by_hand();
  printf("netlink ok\n");
  return 0;
}
