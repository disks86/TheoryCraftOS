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

/* IPv6 through ws2_32 and iphlpapi on TheoryCraftOS, in the real kernel (tests/windows_ipv6_qemu.sh runs it under QEMU; built with mingw against the real
 * winsock2.h / ws2tcpip.h / iphlpapi.h): address text (inet_pton/inet_ntop, WSAStringToAddress/WSAAddressToString), getaddrinfo for localhost and literals,
 * IPV6_V6ONLY, TCP and UDP over ::1, an AF_INET6 and an AF_INET listener on one port, a dual-stack listener that sees IPv4 clients as ::ffff:a.b.c.d,
 * GetAdaptersAddresses' IPv6 addresses (the loopback's ::1, the network card's link-local and SLAAC ones), and - when the network gave a global address -
 * a TCP echo to the host's [::1]:7777 reached through QEMU's [fec0::2].
 * Prints "sock6test: N checks, M failed"; the exit code is the number of failures. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (WSA error %d)\n", what, WSAGetLastError());
}

static int is_loopback6(const struct sockaddr_in6 *a) { return a->sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&a->sin6_addr); }
static int is_mapped_loopback(const struct sockaddr_in6 *a) {
  static const unsigned char mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 127, 0, 0, 1};
  return a->sin6_family == AF_INET6 && memcmp(&a->sin6_addr, mapped, 16) == 0;
}

/* Connects `client` (non-blocking) to `to`, accepts it on `listener` and returns the accepted socket; the peer address the accept reported is in *peer. */
static SOCKET connect_and_accept(SOCKET listener, SOCKET client, const struct sockaddr *to, int to_length, struct sockaddr_storage *peer, int *peer_length) {
  u_long on = 1;
  ioctlsocket(client, FIONBIO, &on);
  const int c = connect(client, to, to_length);
  if (c != 0 && WSAGetLastError() != WSAEWOULDBLOCK) return INVALID_SOCKET;
  *peer_length = sizeof *peer;
  SOCKET accepted = accept(listener, (struct sockaddr *)peer, peer_length);
  /* the client's side is connected once select() says it is writable */
  fd_set w;
  FD_ZERO(&w);
  FD_SET(client, &w);
  struct timeval tv = {5, 0};
  if (select(0, NULL, &w, NULL, &tv) != 1) { if (accepted != INVALID_SOCKET) closesocket(accepted); return INVALID_SOCKET; }
  on = 0;
  ioctlsocket(client, FIONBIO, &on);
  return accepted;
}

static void test_text(void) {
  struct in6_addr a;
  char text[64];
  check(inet_pton(AF_INET6, "2001:db8::1:0:0:1", &a) == 1, "inet_pton IPv6");
  check(inet_ntop(AF_INET6, &a, text, sizeof text) && strcmp(text, "2001:db8::1:0:0:1") == 0, "inet_ntop IPv6");
  check(inet_pton(AF_INET6, "1::2::3", &a) == 0, "inet_pton refuses two ::");
  struct sockaddr_in6 in6;
  int length = sizeof in6;
  check(WSAStringToAddressA((char *)"[fe80::1%3]:8080", AF_INET6, NULL, (struct sockaddr *)&in6, &length) == 0 && length == sizeof in6, "WSAStringToAddressA");
  check(ntohs(in6.sin6_port) == 8080 && in6.sin6_scope_id == 3 && in6.sin6_addr.s6_addr[0] == 0xFE && in6.sin6_addr.s6_addr[15] == 1, "WSAStringToAddressA fields");
  DWORD text_length = sizeof text;
  check(WSAAddressToStringA((struct sockaddr *)&in6, sizeof in6, NULL, text, &text_length) == 0 && strcmp(text, "[fe80::1%3]:8080") == 0, "WSAAddressToStringA IPv6");
  struct sockaddr_in in4;
  length = sizeof in4;
  check(WSAStringToAddressA((char *)"192.168.0.9:53", AF_INET, NULL, (struct sockaddr *)&in4, &length) == 0 && ntohs(in4.sin_port) == 53, "WSAStringToAddressA IPv4");
  text_length = 4;
  check(WSAAddressToStringA((struct sockaddr *)&in4, sizeof in4, NULL, text, &text_length) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT && text_length == 15,
        "WSAAddressToStringA says how much room it needs");
}

static void test_getaddrinfo(void) {
  struct addrinfo hints, *result = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  check(getaddrinfo("localhost", "80", &hints, &result) == 0 && result, "getaddrinfo localhost");
  if (result) {
    check(result->ai_family == AF_INET6 && is_loopback6((struct sockaddr_in6 *)result->ai_addr) && result->ai_addrlen == sizeof(struct sockaddr_in6), "localhost: ::1 first");
    check(result->ai_next && result->ai_next->ai_family == AF_INET && ((struct sockaddr_in *)result->ai_next->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "localhost: then 127.0.0.1");
    freeaddrinfo(result);
  }
  hints.ai_family = AF_INET6;
  hints.ai_flags = AI_NUMERICHOST;
  result = NULL;
  check(getaddrinfo("2001:db8::5", "443", &hints, &result) == 0 && result && result->ai_family == AF_INET6 &&
        ntohs(((struct sockaddr_in6 *)result->ai_addr)->sin6_port) == 443, "getaddrinfo IPv6 literal");
  if (result) freeaddrinfo(result);
  hints.ai_flags = AI_NUMERICHOST | AI_V4MAPPED;
  result = NULL;
  check(getaddrinfo("10.1.2.3", NULL, &hints, &result) == 0 && result && IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)result->ai_addr)->sin6_addr), "AI_V4MAPPED");
  if (result) freeaddrinfo(result);
  hints.ai_flags = AI_PASSIVE;
  result = NULL;
  check(getaddrinfo(NULL, "8080", &hints, &result) == 0 && result && IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6 *)result->ai_addr)->sin6_addr), "passive ::");
  if (result) freeaddrinfo(result);
}

static void test_loopback_tcp(void) {
  SOCKET listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  check(listener != INVALID_SOCKET, "socket(AF_INET6, SOCK_STREAM)");
  DWORD v6only = 0;
  int option_length = sizeof v6only;
  check(getsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, &option_length) == 0 && v6only == 1, "IPV6_V6ONLY is on by default");
  struct sockaddr_in6 any;
  memset(&any, 0, sizeof any);
  any.sin6_family = AF_INET6;
  any.sin6_port = htons(9601);
  check(bind(listener, (struct sockaddr *)&any, sizeof any) == 0 && listen(listener, 4) == 0, "bind [::]:9601 and listen");
  /* an AF_INET listener on the same port: each takes its own version */
  SOCKET listener4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in any4;
  memset(&any4, 0, sizeof any4);
  any4.sin_family = AF_INET;
  any4.sin_port = htons(9601);
  check(bind(listener4, (struct sockaddr *)&any4, sizeof any4) == 0 && listen(listener4, 4) == 0, "an AF_INET listener on the same port");

  SOCKET client = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in6 to = any;
  to.sin6_addr = in6addr_loopback;
  struct sockaddr_storage peer;
  int peer_length = 0;
  SOCKET accepted = connect_and_accept(listener, client, (struct sockaddr *)&to, sizeof to, &peer, &peer_length);
  check(accepted != INVALID_SOCKET && peer_length == sizeof(struct sockaddr_in6) && is_loopback6((struct sockaddr_in6 *)&peer), "accept over ::1");
  const char text[] = "over ::1";
  check(send(client, text, sizeof text, 0) == (int)sizeof text, "send over ::1");
  char back[32] = {0};
  check(recv(accepted, back, sizeof back, 0) == (int)sizeof text && strcmp(back, text) == 0, "recv over ::1");
  struct sockaddr_in6 name;
  int name_length = sizeof name;
  check(getsockname(client, (struct sockaddr *)&name, &name_length) == 0 && name_length == sizeof name && is_loopback6(&name), "getsockname over ::1");
  name_length = sizeof name;
  check(getpeername(client, (struct sockaddr *)&name, &name_length) == 0 && ntohs(name.sin6_port) == 9601, "getpeername over ::1");
  name_length = sizeof(struct sockaddr_in);
  check(getpeername(client, (struct sockaddr *)&name, &name_length) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "getpeername with no room for a SOCKADDR_IN6");
  closesocket(client);
  closesocket(accepted);
  /* the IPv4 client reaches the IPv4 listener */
  SOCKET client4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in to4 = any4;
  to4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  accepted = connect_and_accept(listener4, client4, (struct sockaddr *)&to4, sizeof to4, &peer, &peer_length);
  check(accepted != INVALID_SOCKET && peer_length == sizeof(struct sockaddr_in) && peer.ss_family == AF_INET, "the IPv4 client on the AF_INET listener");
  closesocket(client4);
  closesocket(accepted);
  closesocket(listener);
  closesocket(listener4);

  /* dual stack: IPV6_V6ONLY off, and IPv4 clients come in mapped */
  SOCKET dual = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  v6only = 0;
  check(setsockopt(dual, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof v6only) == 0, "IPV6_V6ONLY off");
  any.sin6_port = htons(9602);
  check(bind(dual, (struct sockaddr *)&any, sizeof any) == 0 && listen(dual, 4) == 0, "a dual-stack listener");
  client4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  to4.sin_port = htons(9602);
  accepted = connect_and_accept(dual, client4, (struct sockaddr *)&to4, sizeof to4, &peer, &peer_length);
  check(accepted != INVALID_SOCKET && peer_length == sizeof(struct sockaddr_in6) && is_mapped_loopback((struct sockaddr_in6 *)&peer), "an IPv4 client seen as ::ffff:127.0.0.1");
  closesocket(client4);
  if (accepted != INVALID_SOCKET) closesocket(accepted);
  v6only = 1;
  check(setsockopt(dual, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof v6only) == SOCKET_ERROR, "IPV6_V6ONLY cannot change once listening");
  closesocket(dual);
  SOCKET four = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  check(setsockopt(four, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof v6only) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "no IPV6_V6ONLY on AF_INET");
  check(connect(four, (struct sockaddr *)&to, sizeof to) == SOCKET_ERROR && WSAGetLastError() == WSAEAFNOSUPPORT, "an IPv6 address on an AF_INET socket");
  closesocket(four);
}

static void test_loopback_udp(void) {
  SOCKET a = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP), b = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in6 at;
  memset(&at, 0, sizeof at);
  at.sin6_family = AF_INET6;
  at.sin6_port = htons(9610);
  check(bind(a, (struct sockaddr *)&at, sizeof at) == 0, "bind UDP [::]:9610");
  at.sin6_addr = in6addr_loopback;
  check(sendto(b, "ping", 5, 0, (struct sockaddr *)&at, sizeof at) == 5, "sendto ::1");
  char buffer[16];
  struct sockaddr_in6 from;
  int from_length = sizeof from;
  fd_set r;
  FD_ZERO(&r);
  FD_SET(a, &r);
  struct timeval tv = {5, 0};
  check(select(0, &r, NULL, NULL, &tv) == 1, "select sees the datagram");
  check(recvfrom(a, buffer, sizeof buffer, 0, (struct sockaddr *)&from, &from_length) == 5 && from_length == sizeof from && is_loopback6(&from) && strcmp(buffer, "ping") == 0,
        "recvfrom ::1");
  closesocket(a);
  closesocket(b);
}

/* The IPv6 addresses GetAdaptersAddresses gives: ::1 on the loopback, and the network card's link-local one (and its global one, once the router's
 * advertisement has come: copied to *global). Waits up to 15 s for the link-local address. */
static int test_adapters(struct in6_addr *global) {
  int have_global = 0;
  for (int tries = 0; tries < 30; ++tries) {
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_GATEWAYS, NULL, NULL, &size);
    IP_ADAPTER_ADDRESSES *list = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!list || GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_GATEWAYS, NULL, list, &size) != NO_ERROR) { check(0, "GetAdaptersAddresses(AF_INET6)"); return 0; }
    int loop = 0, link_local = 0, gateway = 0;
    have_global = 0;
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
      for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)u->Address.lpSockaddr;
        if (in6->sin6_family != AF_INET6) continue;
        if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) loop = 1;
        else if (IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr)) link_local = 1;
        else { have_global = 1; *global = in6->sin6_addr; gateway = a->FirstGatewayAddress != NULL; }
      }
    HeapFree(GetProcessHeap(), 0, list);
    if (link_local && (have_global || tries >= 20)) {
      char text[64];
      if (have_global) printf("sock6test: global address %s\n", inet_ntop(AF_INET6, global, text, sizeof text));
      check(loop, "the loopback adapter has ::1");
      check(!have_global || gateway, "the adapter with a global address has its router as the gateway");
      return have_global;
    }
    Sleep(500);
  }
  check(0, "the network card has an IPv6 link-local address");
  return 0;
}

/* The host's echo server on its [::1]:7777, which QEMU's user network shows the guest as [fec0::2]:7777. */
static void test_echo_to_host(void) {
  struct addrinfo hints, *result = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  check(getaddrinfo("fec0::2", "7777", &hints, &result) == 0 && result, "getaddrinfo fec0::2");
  if (!result) return;
  SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  check(connect(s, result->ai_addr, (int)result->ai_addrlen) == 0, "connect [fec0::2]:7777");
  freeaddrinfo(result);
  static char out[8192], in[8192];
  for (int i = 0; i < (int)sizeof out; ++i) out[i] = (char)(i * 7 + 3);
  int sent = 0, got = 0;
  while (sent < (int)sizeof out) {
    const int n = send(s, out + sent, (int)sizeof out - sent, 0);
    if (n <= 0) break;
    sent += n;
  }
  while (got < sent) {
    const int n = recv(s, in + got, (int)sizeof in - got, 0);
    if (n <= 0) break;
    got += n;
  }
  check(sent == (int)sizeof out && got == sent && memcmp(in, out, sizeof out) == 0, "8 KiB echoed by the host over IPv6");
  struct sockaddr_in6 name;
  int name_length = sizeof name;
  check(getsockname(s, (struct sockaddr *)&name, &name_length) == 0 && !IN6_IS_ADDR_LINKLOCAL(&name.sin6_addr) && !IN6_IS_ADDR_UNSPECIFIED(&name.sin6_addr),
        "the connection's own address is the global one");
  closesocket(s);
  printf("sock6test: echo over IPv6 %s\n", got == sent && sent == (int)sizeof out ? "ok" : "failed");
}

int main(void) {
  WSADATA data;
  check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");
  test_text();
  test_getaddrinfo();
  test_loopback_tcp();
  test_loopback_udp();
  struct in6_addr global;
  if (test_adapters(&global)) test_echo_to_host();
  else printf("sock6test: no global IPv6 address, the echo to the host is skipped\n");
  WSACleanup();
  printf("sock6test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
