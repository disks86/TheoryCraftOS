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
/* Finding hosts and services by name through the C library: /etc/hosts, /etc/services and /etc/protocols, and DNS for names that are in no file. The names
 * asked of DNS are ones a resolver answers by itself, without the Internet: "<anything>.localhost" is the loopback and ".invalid" has nothing in it
 * (RFC 6761), which the machine's own resolver (systemd-resolved) knows; under QEMU the question goes to QEMU's DNS server, which hands it to that
 * resolver. Built static with glibc and run on the real kernel by tests/fork_qemu.sh; what it prints must be what it prints on Linux. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* getaddrinfo for one family: the first address and its port, or the error. */
static void look_up(const char* host, const char* service, int family, int flags) {
  struct addrinfo hints, *found = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = flags;
  const int r = getaddrinfo(host, service, &hints, &found);
  printf("getaddrinfo %s %s (%s):", host, service ? service : "-", family == AF_INET ? "IPv4" : "IPv6");
  if (r != 0) {
    printf(" error %d\n", r);
    return;
  }
  int count = 0;
  for (const struct addrinfo* a = found; a; a = a->ai_next) ++count;
  char text[INET6_ADDRSTRLEN] = {0};
  int port = 0;
  if (found->ai_family == AF_INET) {
    const struct sockaddr_in* in = (const struct sockaddr_in*)found->ai_addr;
    inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
    port = ntohs(in->sin_port);
  } else {
    const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)found->ai_addr;
    inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text));
    port = ntohs(in6->sin6_port);
  }
  printf(" %d address(es), the first %s port %d, protocol %d\n", count, text, port, found->ai_protocol);
  freeaddrinfo(found);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  /* from /etc/hosts */
  look_up("localhost", 0, AF_INET, 0);
  look_up("localhost", "https", AF_INET6, 0);
  /* from DNS, with the port from /etc/services */
  look_up("demo.localhost", "http", AF_INET, 0);
  look_up("demo.localhost", "22", AF_INET6, 0);
  look_up("another-name.demo.localhost", 0, AF_INET, 0);
  look_up("no-such-name.invalid", 0, AF_INET, 0);
  look_up("no-such-name.invalid", 0, AF_INET6, 0);
  /* no lookup at all */
  look_up("192.0.2.7", "8080", AF_INET, AI_NUMERICHOST | AI_NUMERICSERV);
  look_up("2001:db8::7", "imaps", AF_INET6, AI_NUMERICHOST);
  look_up("not-a-number", 0, AF_INET, AI_NUMERICHOST);
  look_up("localhost", "no-such-service", AF_INET, 0);

  const struct hostent* host = gethostbyname("localhost");
  printf("gethostbyname localhost: %s\n", host && host->h_addrtype == AF_INET ? inet_ntoa(*(struct in_addr*)host->h_addr_list[0]) : "not found");
  host = gethostbyname("demo.localhost");
  printf("gethostbyname demo.localhost: %s\n", host && host->h_addrtype == AF_INET ? inet_ntoa(*(struct in_addr*)host->h_addr_list[0]) : "not found");

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(443);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  char name[NI_MAXHOST] = {0}, service[NI_MAXSERV] = {0};
  int r = getnameinfo((struct sockaddr*)&address, sizeof(address), name, sizeof(name), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
  printf("getnameinfo, numbers: %d %s %s\n", r, name, service);
  r = getnameinfo((struct sockaddr*)&address, sizeof(address), name, sizeof(name), service, sizeof(service), NI_NUMERICHOST);
  printf("getnameinfo, the service by name: %d %s %s\n", r, name, service);
  r = getnameinfo((struct sockaddr*)&address, sizeof(address), name, sizeof(name), 0, 0, NI_NAMEREQD);
  printf("getnameinfo, the host by name: %d %s\n", r, r == 0 ? name : "-");

  const struct servent* ssh = getservbyname("ssh", "tcp");
  const struct servent* dns = getservbyport(htons(53), "udp");
  const struct protoent* udp = getprotobyname("udp");
  const struct protoent* six = getprotobynumber(6);
  printf("services and protocols: ssh %d, port 53/udp %s, udp %d, protocol 6 %s\n", ssh ? ntohs((unsigned short)ssh->s_port) : -1, dns ? dns->s_name : "-", udp ? udp->p_proto : -1,
         six ? six->p_name : "-");
  printf("resolve ok\n");
  return 0;
}
