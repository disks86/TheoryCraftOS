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

/* ws2_32's event and overlapped calls on TheoryCraftOS (mingw gcc: x86_64-w64-mingw32-gcc -O1 -o ws2eventtest.exe tests/windows/ws2eventtest.c -lws2_32). Like socktest.c it
 * needs no peer (there is no loopback interface): WSA events, WSAEventSelect (FD_WRITE of a bound UDP socket comes by itself, a listening socket with nobody connecting
 * records nothing), an overlapped WSARecvFrom with nothing to receive (pending, incomplete, then aborted by closesocket), WSASendTo, WSAIoctl, select() of two sockets,
 * the protocol catalogue and GetAddrInfoExW for "localhost", plain and overlapped. Prints "ws2eventtest: N checks, M failed"; the exit code is the number of failures. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE _WSAIOR(IOC_WS2, 34)
#endif
#include <string.h>

/* (not in every mingw header set) */
int WSAAPI WSCGetProviderPath(LPGUID provider, WCHAR *path, LPINT length, LPINT error);
INT WSAAPI GetAddrInfoExOverlappedResult(LPOVERLAPPED overlapped);

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (WSA error %d)\n", what, WSAGetLastError());
}

static SOCKET bound_udp(void) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  if (s != INVALID_SOCKET && bind(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); s = INVALID_SOCKET; }
  return s;
}

int main(void) {
  WSADATA data;
  check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");

  /* events */
  WSAEVENT e = WSACreateEvent();
  check(e != WSA_INVALID_EVENT, "WSACreateEvent");
  check(WSAWaitForMultipleEvents(1, &e, FALSE, 0, FALSE) == WSA_WAIT_TIMEOUT, "a new event is not signalled");
  check(WSASetEvent(e), "WSASetEvent");
  check(WSAWaitForMultipleEvents(1, &e, FALSE, 0, FALSE) == WSA_WAIT_EVENT_0, "a set event is signalled");
  check(WSAWaitForMultipleEvents(1, &e, FALSE, 0, FALSE) == WSA_WAIT_EVENT_0, "and stays so (manual reset)");
  check(WSAResetEvent(e), "WSAResetEvent");
  check(WSAWaitForMultipleEvents(1, &e, FALSE, 0, FALSE) == WSA_WAIT_TIMEOUT, "a reset event is not signalled");
  check(WSAWaitForMultipleEvents(0, &e, FALSE, 0, FALSE) == WSA_WAIT_FAILED, "no events is WSA_WAIT_FAILED");

  /* event select: a bound UDP socket can be written at once, so FD_WRITE comes without anything else happening */
  SOCKET udp = bound_udp();
  check(udp != INVALID_SOCKET, "a bound UDP socket");
  check(WSAEventSelect(udp, e, FD_READ | FD_WRITE) == 0, "WSAEventSelect(FD_READ | FD_WRITE)");
  check(WSAWaitForMultipleEvents(1, &e, FALSE, 2000, FALSE) == WSA_WAIT_EVENT_0, "FD_WRITE signals the event");
  WSANETWORKEVENTS ne;
  memset(&ne, 0xCC, sizeof ne);
  check(WSAEnumNetworkEvents(udp, e, &ne) == 0, "WSAEnumNetworkEvents");
  check(ne.lNetworkEvents == FD_WRITE && ne.iErrorCode[FD_WRITE_BIT] == 0, "it reports FD_WRITE without an error");
  check(WSAWaitForMultipleEvents(1, &e, FALSE, 0, FALSE) == WSA_WAIT_TIMEOUT, "WSAEnumNetworkEvents reset the event");
  check(WSAEnumNetworkEvents(udp, NULL, &ne) == 0 && ne.lNetworkEvents == 0, "FD_WRITE is recorded once");
  char byte = 0;
  check(recv(udp, &byte, 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "WSAEventSelect made the socket non-blocking");
  check(WSAEventSelect(udp, NULL, 0) == 0, "WSAEventSelect(0) cancels");

  /* a listening TCP socket with no connection: nothing to record */
  SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in any;
  memset(&any, 0, sizeof any);
  any.sin_family = AF_INET;
  any.sin_port = htons(40123);      /* (the stack gives a TCP listener no ephemeral port) */
  check(listener != INVALID_SOCKET && bind(listener, (struct sockaddr *)&any, sizeof any) == 0 && listen(listener, 4) == 0, "a listening TCP socket");
  WSAEVENT le = WSACreateEvent();
  check(WSAEventSelect(listener, le, FD_ACCEPT | FD_CLOSE) == 0, "WSAEventSelect(FD_ACCEPT)");
  check(WSAWaitForMultipleEvents(1, &le, FALSE, 200, FALSE) == WSA_WAIT_TIMEOUT, "no connection, no FD_ACCEPT");
  check(WSAEnumNetworkEvents(listener, le, &ne) == 0 && ne.lNetworkEvents == 0, "nothing recorded for the listener");
  check(accept(listener, NULL, NULL) == INVALID_SOCKET && WSAGetLastError() == WSAEWOULDBLOCK, "accept() does not block after WSAEventSelect");
  closesocket(listener);
  WSACloseEvent(le);

  /* overlapped receive with nothing to receive: pending, incomplete, then aborted when the socket is closed */
  SOCKET udp2 = bound_udp();
  WSAOVERLAPPED ov;
  memset(&ov, 0, sizeof ov);
  ov.hEvent = WSACreateEvent();
  char buffer[64];
  WSABUF wb = {sizeof buffer, buffer};
  DWORD got = 0, flags = 0;
  struct sockaddr_in from;
  int from_length = sizeof from;
  const int rv = WSARecvFrom(udp2, &wb, 1, &got, &flags, (struct sockaddr *)&from, &from_length, &ov, NULL);
  check(rv == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING, "an overlapped WSARecvFrom with no data is pending");
  DWORD bytes = 0;
  check(!WSAGetOverlappedResult(udp2, &ov, &bytes, FALSE, &flags) && WSAGetLastError() == WSA_IO_INCOMPLETE, "WSAGetOverlappedResult: incomplete");
  closesocket(udp2);
  check(WSAWaitForMultipleEvents(1, &ov.hEvent, FALSE, 2000, FALSE) == WSA_WAIT_EVENT_0, "closing the socket completes the receive");
  check(!WSAGetOverlappedResult(udp2, &ov, &bytes, TRUE, &flags) && WSAGetLastError() == WSA_OPERATION_ABORTED, "as aborted");
  WSACloseEvent(ov.hEvent);

  /* sends complete inline, overlapped or not */
  struct sockaddr_in to;
  memset(&to, 0, sizeof to);
  to.sin_family = AF_INET;
  to.sin_port = htons(9);      /* discard, at QEMU's gateway */
  to.sin_addr.s_addr = htonl(0x0A000202u);
  char payload[2][4] = {"ab", "cd"};
  WSABUF two[2] = {{2, payload[0]}, {2, payload[1]}};
  DWORD sent = 0;
  check(WSASendTo(udp, two, 2, &sent, 0, (struct sockaddr *)&to, sizeof to, NULL, NULL) == 0 && sent == 4, "WSASendTo gathers two buffers into one datagram");
  memset(&ov, 0, sizeof ov);
  ov.hEvent = WSACreateEvent();
  sent = 0;
  check(WSASendTo(udp, two, 1, &sent, 0, (struct sockaddr *)&to, sizeof to, &ov, NULL) == 0 && sent == 2, "an overlapped WSASendTo completes at once");
  check(WSAWaitForMultipleEvents(1, &ov.hEvent, FALSE, 0, FALSE) == WSA_WAIT_EVENT_0, "and sets its event");
  check(WSAGetOverlappedResult(udp, &ov, &bytes, FALSE, &flags) && bytes == 2, "WSAGetOverlappedResult of the send");
  WSACloseEvent(ov.hEvent);
  check(WSASendTo(udp, two, 1, &sent, 0, (struct sockaddr *)&to, 3, NULL, NULL) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "a short address is WSAEFAULT");

  /* WSAIoctl */
  u_long on = 1;
  DWORD returned = 0;
  check(WSAIoctl(udp, FIONBIO, &on, sizeof on, NULL, 0, &returned, NULL, NULL) == 0, "WSAIoctl(FIONBIO)");
  u_long pending_bytes = 99;
  check(WSAIoctl(udp, FIONREAD, NULL, 0, &pending_bytes, sizeof pending_bytes, &returned, NULL, NULL) == 0 && pending_bytes == 0 && returned == 4, "WSAIoctl(FIONREAD)");
  struct tcp_keepalive keep = {1, 30000, 1000};
  SOCKET tcp = socket(AF_INET, SOCK_STREAM, 0);
  check(WSAIoctl(tcp, SIO_KEEPALIVE_VALS, &keep, sizeof keep, NULL, 0, &returned, NULL, NULL) == 0, "WSAIoctl(SIO_KEEPALIVE_VALS)");
  SOCKET base = 0;
  check(WSAIoctl(tcp, SIO_BASE_HANDLE, NULL, 0, &base, sizeof base, &returned, NULL, NULL) == 0 && base == tcp, "SIO_BASE_HANDLE is the socket");
  GUID connectex = WSAID_CONNECTEX;
  void *fn = (void *)1;
  check(WSAIoctl(tcp, SIO_GET_EXTENSION_FUNCTION_POINTER, &connectex, sizeof connectex, &fn, sizeof fn, &returned, NULL, NULL) == SOCKET_ERROR &&
        WSAGetLastError() == WSAEINVAL && fn == NULL, "no extension functions");
  check(WSAIoctl(tcp, 0x12345678, NULL, 0, NULL, 0, &returned, NULL, NULL) == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP, "an unknown code is WSAEOPNOTSUPP");

  /* select() of two sockets: both UDP sockets are writable, both come back */
  SOCKET udp3 = bound_udp();
  fd_set writable;
  FD_ZERO(&writable);
  FD_SET(udp, &writable);
  FD_SET(udp3, &writable);
  struct timeval zero = {0, 0};
  check(select(0, NULL, &writable, NULL, &zero) == 2, "select() finds both sockets writable");
  check(writable.fd_count == 2 && FD_ISSET(udp, &writable) && FD_ISSET(udp3, &writable), "and returns both of them");
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(udp, &readable);
  FD_SET(udp3, &readable);
  check(select(0, &readable, NULL, NULL, &zero) == 0 && readable.fd_count == 0, "neither is readable");
  closesocket(udp3);
  closesocket(tcp);
  closesocket(udp);

  /* the protocol catalogue */
  DWORD length = 0;
  check(WSAEnumProtocolsW(NULL, NULL, &length) == SOCKET_ERROR && WSAGetLastError() == WSAENOBUFS && length == 4 * sizeof(WSAPROTOCOL_INFOW), "WSAEnumProtocolsW sizes");
  WSAPROTOCOL_INFOW protocols[4];
  length = sizeof protocols;
  check(WSAEnumProtocolsW(NULL, protocols, &length) == 4, "four protocols");
  check(protocols[0].iAddressFamily == AF_INET && protocols[0].iSocketType == SOCK_STREAM && protocols[0].iProtocol == IPPROTO_TCP, "the first is TCP/IPv4");
  check(protocols[3].iAddressFamily == AF_INET6 && protocols[3].iProtocol == IPPROTO_UDP && protocols[3].ProtocolChain.ChainLen == BASE_PROTOCOL, "the last is UDP/IPv6");
  int only_udp[] = {IPPROTO_UDP, 0};
  length = sizeof protocols;
  check(WSAEnumProtocolsW(only_udp, protocols, &length) == 2 && protocols[0].iProtocol == IPPROTO_UDP && protocols[1].iProtocol == IPPROTO_UDP, "filtered to UDP");
  WCHAR path[MAX_PATH];
  int path_length = MAX_PATH, error = 0;
  check(WSCGetProviderPath(&protocols[0].ProviderId, path, &path_length, &error) == 0 && path_length > 0, "WSCGetProviderPath of the base provider");

  /* GetAddrInfoExW */
  ADDRINFOEXW hints, *result = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  check(GetAddrInfoExW(L"localhost", L"80", NS_DNS, NULL, &hints, &result, NULL, NULL, NULL, NULL) == 0 && result, "GetAddrInfoExW(localhost)");
  if (result) {
    const struct sockaddr_in *a = (const struct sockaddr_in *)result->ai_addr;
    check(result->ai_family == AF_INET && result->ai_addrlen == sizeof *a && a->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && a->sin_port == htons(80), "127.0.0.1:80");
    FreeAddrInfoExW(result);
  }
  result = NULL;
  memset(&ov, 0, sizeof ov);
  ov.hEvent = WSACreateEvent();
  HANDLE lookup = NULL;
  const int pending = GetAddrInfoExW(L"localhost", NULL, NS_ALL, NULL, &hints, &result, NULL, &ov, NULL, &lookup);
  check(pending == WSA_IO_PENDING, "an overlapped GetAddrInfoExW is pending");
  check(WSAWaitForMultipleEvents(1, &ov.hEvent, FALSE, 5000, FALSE) == WSA_WAIT_EVENT_0, "and completes");
  check(GetAddrInfoExOverlappedResult(&ov) == 0 && result != NULL, "with the result");
  if (result) FreeAddrInfoExW(result);
  WSACloseEvent(ov.hEvent);

  WSACloseEvent(e);
  WSACleanup();
  printf("ws2eventtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
