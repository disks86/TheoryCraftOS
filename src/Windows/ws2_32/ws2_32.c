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

/* ws2_32.dll for TheoryCraftOS: Winsock over this OS's own NT-ish socket services (src/Process/WindowsSockets.{hpp,cpp}; real Windows has none of its own, Winsock talks to
 * a device driver, afd.sys, through its own ioctl protocol, but there is no reason to copy that here). A SOCKET is the kernel HANDLE of the socket descriptor directly (both
 * are small, dense integers already, so there is no separate table to keep in sync - see advapi32's registry-is-a-path comment for the same idea applied to HKEYs). Address
 * and port a program hands in or reads back out are a real `struct sockaddr_in`'s bytes (network byte order); the NT services carry the same bytes as a plain ULONG/USHORT
 * (ws2_32.dll does no byte-swapping of its own - ntohs/htons etc. are for the *program* to use on values it keeps itself). Built freestanding with the OS's mingw compiler,
 * directly over kernel32.dll and ntdll.dll, the way a real ws2_32.dll sits over ntdll for everything but the socket calls themselves. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define WSAAPI

typedef UINT_PTR SOCKET;
typedef USHORT u_short_;
typedef ULONG u_long_;
#define INVALID_SOCKET ((SOCKET)(UINT_PTR)-1)
#define SOCKET_ERROR (-1)

typedef struct in_addr_ { union { struct { UCHAR s_b1, s_b2, s_b3, s_b4; }; ULONG S_addr; }; } IN_ADDR;
typedef struct sockaddr_ { USHORT sa_family; UCHAR sa_data[14]; } SOCKADDR, *LPSOCKADDR;
typedef struct sockaddr_in_ { SHORT sin_family; USHORT sin_port; IN_ADDR sin_addr; UCHAR sin_zero[8]; } SOCKADDR_IN, *LPSOCKADDR_IN;
/* _WIN64's WSADATA orders its fields differently from the 32-bit one (iMaxSockets/iMaxUdpDg/lpVendorInfo before the description strings, not after) - this OS is
 * x86-64 only, so this is the one layout that matters, but getting it backwards would silently corrupt a real mingw-built program's WSAStartup call. */
typedef struct { WORD wVersion, wHighVersion; USHORT iMaxSockets, iMaxUdpDg; PSTR lpVendorInfo; CHAR szDescription[257], szSystemStatus[129]; } WSADATA, *LPWSADATA;
typedef struct { UINT fd_count; SOCKET fd_array[64]; } fd_set_;
typedef struct { LONG tv_sec, tv_usec; } TIMEVAL_, *PTIMEVAL_;

#define AF_INET_ 2
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_ 2
#define IPPROTO_TCP_ 6
#define IPPROTO_UDP_ 17
#define INADDR_ANY_ 0u

#define SD_RECEIVE_ 0
#define SD_SEND_ 1
#define SD_BOTH_ 2

#define FIONBIO_ 0x8004667Eu
#define FIONREAD_ 0x4004667Fu

#define WSAEINTR_ 10004
#define WSAEFAULT_ 10014
#define WSAEINVAL_ 10022
#define WSAEWOULDBLOCK_ 10035
#define WSAENOTSOCK_ 10038
#define WSAEOPNOTSUPP_ 10045
#define WSAEADDRINUSE_ 10048
#define WSAEADDRNOTAVAIL_ 10049
#define WSAENETUNREACH_ 10051
#define WSAECONNRESET_ 10054
#define WSAENOBUFS_ 10055
#define WSAEISCONN_ 10056
#define WSAENOTCONN_ 10057
#define WSAETIMEDOUT_ 10060
#define WSAECONNREFUSED_ 10061
#define WSAEHOSTUNREACH_ 10065
#define WSASYSNOTREADY_ 10091
#define WSAVERNOTSUPPORTED_ 10092
#define WSANOTINITIALISED_ 10093

/* From kernel32.dll / ntdll.dll. */
IMPORT void *memset(void *, int, size_t_);
IMPORT void *memcpy(void *, const void *, size_t_);

static int g_started = 0;      /* WSAStartup/WSACleanup are reference counted, like the real ones (nested libraries each call their own pair) */

static void set_wsa_error(int error) { TEB_LAST_ERROR(NtCurrentTeb()) = (DWORD)error; }

/* What a failing NTSTATUS from one of our socket services means as a WSA error (its own numbering, separate from plain Win32 GetLastError codes: see WinSock2.h). */
static int wsa_error_for(NTSTATUS status) {
  switch ((ULONG)status) {
    case 0xC0000103u: return WSAEWOULDBLOCK_;      /* kStatusWouldBlock (shares its value with STATUS_NOT_A_DIRECTORY elsewhere - only ever this, on a socket handle) */
    case 0xC0000236u: return WSAECONNREFUSED_;
    case 0xC000023Du: return WSAEHOSTUNREACH_;
    case 0xC000020Au: return WSAEADDRINUSE_;
    case 0xC0000050u: return WSAECONNRESET_;
    case 0xC000000Eu: return WSAENETUNREACH_;
    case 0xC0000140u: return WSAENOTCONN_;
    case 0xC0000209u: return WSAEADDRNOTAVAIL_;
    case 0xC0000238u: return WSAENOTCONN_;          /* STATUS_NOT_CONNECTED */
    case 0xC0000120u: return WSAEINTR_;             /* STATUS_CANCELLED */
    case 0xC0000008u: return WSAENOTSOCK_;
    case 0xC000000Du: return WSAEINVAL_;
    case 0xC0000002u: return WSAEOPNOTSUPP_;
    case 0xC0000005u: return WSAEFAULT_;
    case 0xC000009Au: return WSAENOBUFS_;
    default: return WSAEINVAL_;
  }
}
/* Fails the call (returning `result`) with a WSA error made from an NTSTATUS; result is -1 for int-returning calls, INVALID_SOCKET for socket()/accept(). */
static SOCKET fail_nt(NTSTATUS status, SOCKET result) { set_wsa_error(wsa_error_for(status)); return result; }

EXPORT int WSAAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData) {
  (void)wVersionRequested;
  ++g_started;
  if (lpWSAData) {
    memset(lpWSAData, 0, sizeof(*lpWSAData));
    lpWSAData->wVersion = 0x0202; lpWSAData->wHighVersion = 0x0202;      /* Winsock 2.2, like every real implementation offers */
    lpWSAData->iMaxSockets = 32767; lpWSAData->iMaxUdpDg = 65467;
    const char desc[] = "TheoryCraftOS Winsock";
    memcpy(lpWSAData->szDescription, desc, sizeof(desc));
  }
  return 0;
}
EXPORT int WSAAPI WSACleanup(void) { if (g_started > 0) --g_started; return 0; }
EXPORT int WSAAPI WSAGetLastError(void) { return (int)TEB_LAST_ERROR(NtCurrentTeb()); }
EXPORT void WSAAPI WSASetLastError(int error) { set_wsa_error(error); }

static BOOL require_started(void) { if (g_started <= 0) { set_wsa_error(WSANOTINITIALISED_); return FALSE; } return TRUE; }

EXPORT SOCKET WSAAPI socket(int af, int type, int protocol) {
  (void)protocol;
  if (!require_started()) return INVALID_SOCKET;
  if (af != AF_INET_ || (type != SOCK_STREAM_ && type != SOCK_DGRAM_)) { set_wsa_error(WSAEINVAL_); return INVALID_SOCKET; }
  HANDLE h = NULL;
  const NTSTATUS status = NtCreateSocket((ULONG)af, (ULONG)type, (ULONG)(type == SOCK_STREAM_ ? IPPROTO_TCP_ : IPPROTO_UDP_), &h);
  if (!NT_SUCCESS(status)) return fail_nt(status, INVALID_SOCKET);
  return (SOCKET)(UINT_PTR)h;
}
EXPORT int WSAAPI closesocket(SOCKET s) {
  const NTSTATUS status = NtClose((HANDLE)(UINT_PTR)s);
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  return 0;
}

EXPORT int WSAAPI bind(SOCKET s, const SOCKADDR *name, int namelen) {
  if (!name || (size_t_)namelen < sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const SOCKADDR_IN *in = (const SOCKADDR_IN *)name;
  const NTSTATUS status = NtBindSocket((HANDLE)(UINT_PTR)s, in->sin_addr.S_addr, (ULONG)in->sin_port);
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI connect(SOCKET s, const SOCKADDR *name, int namelen) {
  if (!name || (size_t_)namelen < sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const SOCKADDR_IN *in = (const SOCKADDR_IN *)name;
  const NTSTATUS status = NtConnectSocket((HANDLE)(UINT_PTR)s, in->sin_addr.S_addr, (ULONG)in->sin_port);
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI listen(SOCKET s, int backlog) {
  const NTSTATUS status = NtListenSocket((HANDLE)(UINT_PTR)s, (ULONG)backlog);
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT SOCKET WSAAPI accept(SOCKET s, SOCKADDR *addr, int *addrlen) {
  HANDLE out = NULL;
  ULONG peer_address = 0, peer_port = 0;
  const NTSTATUS status = NtAcceptSocket((HANDLE)(UINT_PTR)s, &out, &peer_address, &peer_port);
  if (!NT_SUCCESS(status)) return fail_nt(status, INVALID_SOCKET);
  if (addr && addrlen && (size_t_)*addrlen >= sizeof(SOCKADDR_IN)) {
    SOCKADDR_IN *in = (SOCKADDR_IN *)addr;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET_; in->sin_port = (USHORT)peer_port; in->sin_addr.S_addr = peer_address;
    *addrlen = sizeof(SOCKADDR_IN);
  }
  return (SOCKET)(UINT_PTR)out;
}

EXPORT int WSAAPI send(SOCKET s, const char *buf, int len, int flags) {
  (void)flags;
  ULONG sent = 0;
  const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, 0, 0, FALSE);
  return NT_SUCCESS(status) ? (int)sent : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI sendto(SOCKET s, const char *buf, int len, int flags, const SOCKADDR *to, int tolen) {
  (void)flags;
  if (!to || (size_t_)tolen < sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const SOCKADDR_IN *in = (const SOCKADDR_IN *)to;
  ULONG sent = 0;
  const NTSTATUS status =
      NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, in->sin_addr.S_addr, (ULONG)in->sin_port, TRUE);
  return NT_SUCCESS(status) ? (int)sent : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI recv(SOCKET s, char *buf, int len, int flags) {
  (void)flags;
  ULONG got = 0;
  const NTSTATUS status = NtReceiveSocket((HANDLE)(UINT_PTR)s, buf, (ULONG)len, &got, NULL, NULL, NULL);
  return NT_SUCCESS(status) ? (int)got : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI recvfrom(SOCKET s, char *buf, int len, int flags, SOCKADDR *from, int *fromlen) {
  (void)flags;
  ULONG got = 0, from_address = 0, from_port = 0;
  const NTSTATUS status = NtReceiveSocket((HANDLE)(UINT_PTR)s, buf, (ULONG)len, &got, &from_address, &from_port, NULL);
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  if (from && fromlen && (size_t_)*fromlen >= sizeof(SOCKADDR_IN)) {
    SOCKADDR_IN *in = (SOCKADDR_IN *)from;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET_; in->sin_port = (USHORT)from_port; in->sin_addr.S_addr = from_address;
    *fromlen = sizeof(SOCKADDR_IN);
  }
  return (int)got;
}

EXPORT int WSAAPI shutdown(SOCKET s, int how) {
  const ULONG in = (ULONG)how;
  const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, 0 /*Shutdown*/, (PVOID)&in, sizeof(in), NULL, 0);
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI ioctlsocket(SOCKET s, LONG cmd, u_long_ *argp) {
  if (cmd == (LONG)FIONBIO_) {
    const UCHAR on = argp && *argp != 0;
    const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, 1 /*SetNonBlocking*/, (PVOID)&on, 1, NULL, 0);
    return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
  }
  if (cmd == (LONG)FIONREAD_) {
    ULONG available = 0;
    const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, 4 /*BytesReadable*/, NULL, 0, &available, sizeof(available));
    if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
    if (argp) *argp = available;
    return 0;
  }
  set_wsa_error(WSAEOPNOTSUPP_);
  return SOCKET_ERROR;
}
/* Reads an address+port pair back as an 8-byte [addr:4][port:2][pad:2] buffer (NtSocketControl's GetSockName/GetPeerName layout) into a SOCKADDR_IN. */
static int get_name(SOCKET s, ULONG operation, SOCKADDR *out, int *outlen) {
  if (!out || !outlen || (size_t_)*outlen < sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  UCHAR raw[8];
  const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, operation, NULL, 0, raw, sizeof(raw));
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  SOCKADDR_IN *in = (SOCKADDR_IN *)out;
  memset(in, 0, sizeof(*in));
  in->sin_family = AF_INET_;
  in->sin_addr.S_addr = (ULONG)raw[0] << 24 | (ULONG)raw[1] << 16 | (ULONG)raw[2] << 8 | raw[3];
  in->sin_port = (USHORT)((USHORT)raw[4] << 8 | raw[5]);
  *outlen = sizeof(SOCKADDR_IN);
  return 0;
}
EXPORT int WSAAPI getsockname(SOCKET s, SOCKADDR *name, int *namelen) { return get_name(s, 2, name, namelen); }
EXPORT int WSAAPI getpeername(SOCKET s, SOCKADDR *name, int *namelen) { return get_name(s, 3, name, namelen); }

EXPORT int WSAAPI select(int nfds, fd_set_ *readfds, fd_set_ *writefds, fd_set_ *exceptfds, const TIMEVAL_ *timeout) {
  (void)nfds;
  LARGE_INTEGER time_limit;
  const PLARGE_INTEGER time_arg = timeout ? &time_limit : NULL;
  if (timeout) time_limit.QuadPart = (LONGLONG)timeout->tv_sec * 10000000LL + (LONGLONG)timeout->tv_usec * 10LL;
  ULONG ready = 0;
  const NTSTATUS status = NtSelectSockets(
      readfds ? &readfds->fd_count : NULL, readfds ? (PULONG)readfds->fd_array : NULL,
      writefds ? &writefds->fd_count : NULL, writefds ? (PULONG)writefds->fd_array : NULL,
      exceptfds ? &exceptfds->fd_count : NULL, exceptfds ? (PULONG)exceptfds->fd_array : NULL,
      time_arg, &ready);
  /* Each fd_set's own fd_count is read/write, like the real one: NtSelectSockets already turned it from "how many handles I gave you" into "how many of them are
     ready" and compacted fd_array to match, so there is nothing left to do here. */
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  return (int)ready;
}

/* Not a macro on the real one either: winsock2.h's FD_ISSET expands to a call to this (exported by every real ws2_32.dll). */
EXPORT int WSAAPI __WSAFDIsSet(SOCKET s, fd_set_ *set) {
  if (!set) return 0;
  for (UINT i = 0; i < set->fd_count; ++i) if (set->fd_array[i] == s) return 1;
  return 0;
}

EXPORT u_short_ WSAAPI htons(u_short_ hostshort) { return (u_short_)(((hostshort & 0xFF) << 8) | (hostshort >> 8)); }
EXPORT u_short_ WSAAPI ntohs(u_short_ netshort) { return htons(netshort); }
EXPORT u_long_ WSAAPI htonl(u_long_ hostlong) {
  return ((hostlong & 0xFFu) << 24) | ((hostlong & 0xFF00u) << 8) | ((hostlong >> 8) & 0xFF00u) | ((hostlong >> 24) & 0xFFu);
}
EXPORT u_long_ WSAAPI ntohl(u_long_ netlong) { return htonl(netlong); }

EXPORT ULONG WSAAPI inet_addr(const char *cp) {
  if (!cp) return 0xFFFFFFFFu;
  ULONG octets[4] = {0, 0, 0, 0};
  int n = 0;
  const char *p = cp;
  for (; n < 4; ++n) {
    if (*p < '0' || *p > '9') return 0xFFFFFFFFu;
    ULONG v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (ULONG)(*p - '0'); ++p; if (v > 255) return 0xFFFFFFFFu; }
    octets[n] = v;
    if (n < 3) { if (*p != '.') return 0xFFFFFFFFu; ++p; } else if (*p != 0) return 0xFFFFFFFFu;
  }
  return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];      /* network byte order: the first octet is the top byte (matches a real in_addr) */
}
EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved; (void)reason;
  return TRUE;
}

EXPORT char *WSAAPI inet_ntoa(IN_ADDR in) {
  static char buffer[16];
  char *p = buffer;
  const UCHAR bytes[4] = {in.s_b1, in.s_b2, in.s_b3, in.s_b4};
  for (int i = 0; i < 4; ++i) {
    UCHAR v = bytes[i];
    char digits[4]; int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v = (UCHAR)(v / 10); } while (v);
    while (n > 0) *p++ = digits[--n];
    if (i < 3) *p++ = '.';
  }
  *p = 0;
  return buffer;
}
