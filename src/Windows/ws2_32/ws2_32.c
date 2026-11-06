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
 * are small, dense integers already, so there is no separate table to keep in sync - see advapi32's registry-is-a-path comment for the same idea applied to HKEYs). Sockets
 * are AF_INET or AF_INET6 (IPv6-only by default, dual-stack once IPV6_V6ONLY is turned off, as on Windows). The socket address a program hands in or reads back out
 * (SOCKADDR_IN or SOCKADDR_IN6) goes to and comes from the services as its own bytes; the kernel reads and checks it (src/Process/SocketAddress.hpp). Name resolution
 * (getaddrinfo and friends) knows numeric addresses, localhost and this machine's own name, and asks the kernel's DNS resolver (NtSocketControl ResolveName) for the
 * rest. Built freestanding with the OS's mingw compiler, directly over kernel32.dll and ntdll.dll, the way a real ws2_32.dll sits over ntdll for everything but the socket
 * calls themselves. */
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
typedef struct { SHORT sin6_family; USHORT sin6_port; ULONG sin6_flowinfo; UCHAR sin6_addr[16]; ULONG sin6_scope_id; } SOCKADDR_IN6_;
/* Room for either kind of address (a real SOCKADDR_STORAGE is 128 bytes; these services never give more than a SOCKADDR_IN6). */
typedef union { SOCKADDR sa; SOCKADDR_IN in; SOCKADDR_IN6_ in6; } ANY_SOCKADDR_;
/* _WIN64's WSADATA orders its fields differently from the 32-bit one (iMaxSockets/iMaxUdpDg/lpVendorInfo before the description strings, not after) - this OS is
 * x86-64 only, so this is the one layout that matters, but getting it backwards would silently corrupt a real mingw-built program's WSAStartup call. */
typedef struct { WORD wVersion, wHighVersion; USHORT iMaxSockets, iMaxUdpDg; PSTR lpVendorInfo; CHAR szDescription[257], szSystemStatus[129]; } WSADATA, *LPWSADATA;
typedef struct { UINT fd_count; SOCKET fd_array[64]; } fd_set_;
typedef struct { LONG tv_sec, tv_usec; } TIMEVAL_, *PTIMEVAL_;

#define AF_INET_ 2
#define AF_INET6_ 23
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
#define WSAENOPROTOOPT_ 10042
#define WSAEAFNOSUPPORT_ 10047
#define WSAESOCKTNOSUPPORT_ 10044
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
    case 0xC0000141u: return WSAEAFNOSUPPORT_;      /* STATUS_INVALID_ADDRESS: the address is not one this socket's family can use (IPv4 on an IPv6-only socket) */
    case 0xC0000023u: return WSAEFAULT_;            /* STATUS_BUFFER_TOO_SMALL: an address buffer too small for the address */
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

/* The size of the socket address `name` (namelen bytes) must have for its family; 0 for a family no socket has. */
static int address_size(const SOCKADDR *name, int namelen) {
  if (!name || namelen < (int)sizeof(SHORT)) return 0;
  if (name->sa_family == AF_INET_) return (int)sizeof(SOCKADDR_IN);
  if (name->sa_family == AF_INET6_) return (int)sizeof(SOCKADDR_IN6_);
  return 0;
}
/* Checks an address a program hands in: WSAEFAULT for a missing or short one, WSAEAFNOSUPPORT for an unknown family. */
static BOOL check_address(const SOCKADDR *name, int namelen) {
  if (!name || namelen < (int)sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return FALSE; }
  const int need = address_size(name, namelen);
  if (need == 0) { set_wsa_error(WSAEAFNOSUPPORT_); return FALSE; }
  if (namelen < need) { set_wsa_error(WSAEFAULT_); return FALSE; }
  return TRUE;
}
/* Copies an address the services gave back (`got` bytes) to the program's buffer, if it has one: WSAEFAULT if it is too small, like the real calls. */
static BOOL give_address(const ANY_SOCKADDR_ *got, ULONG got_length, SOCKADDR *out, int *outlen) {
  if (!out || !outlen) return TRUE;
  if (*outlen < (int)got_length) { set_wsa_error(WSAEFAULT_); return FALSE; }
  memcpy(out, got, got_length);
  *outlen = (int)got_length;
  return TRUE;
}

static BOOL require_started(void) { if (g_started <= 0) { set_wsa_error(WSANOTINITIALISED_); return FALSE; } return TRUE; }

EXPORT SOCKET WSAAPI socket(int af, int type, int protocol) {
  (void)protocol;
  if (!require_started()) return INVALID_SOCKET;
  if (af != AF_INET_ && af != AF_INET6_) { set_wsa_error(WSAEAFNOSUPPORT_); return INVALID_SOCKET; }
  if (type != SOCK_STREAM_ && type != SOCK_DGRAM_) { set_wsa_error(WSAESOCKTNOSUPPORT_); return INVALID_SOCKET; }
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
  if (!check_address(name, namelen)) return SOCKET_ERROR;
  const NTSTATUS status = NtBindSocket((HANDLE)(UINT_PTR)s, name, (ULONG)address_size(name, namelen));
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI connect(SOCKET s, const SOCKADDR *name, int namelen) {
  if (!check_address(name, namelen)) return SOCKET_ERROR;
  const NTSTATUS status = NtConnectSocket((HANDLE)(UINT_PTR)s, name, (ULONG)address_size(name, namelen));
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI listen(SOCKET s, int backlog) {
  const NTSTATUS status = NtListenSocket((HANDLE)(UINT_PTR)s, (ULONG)backlog);
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT SOCKET WSAAPI accept(SOCKET s, SOCKADDR *addr, int *addrlen) {
  HANDLE out = NULL;
  ANY_SOCKADDR_ peer;
  ULONG peer_length = sizeof peer;
  if (addr && !addrlen) { set_wsa_error(WSAEFAULT_); return INVALID_SOCKET; }
  const NTSTATUS status = NtAcceptSocket((HANDLE)(UINT_PTR)s, &out, &peer, &peer_length);
  if (!NT_SUCCESS(status)) return fail_nt(status, INVALID_SOCKET);
  if (!give_address(&peer, peer_length, addr, addrlen)) { NtClose(out); return INVALID_SOCKET; }
  return (SOCKET)(UINT_PTR)out;
}

EXPORT int WSAAPI send(SOCKET s, const char *buf, int len, int flags) {
  (void)flags;
  ULONG sent = 0;
  const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, NULL, 0);
  return NT_SUCCESS(status) ? (int)sent : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI sendto(SOCKET s, const char *buf, int len, int flags, const SOCKADDR *to, int tolen) {
  (void)flags;
  if (!to) return send(s, buf, len, flags);      /* (sendto with no address is send, as on Windows) */
  if (!check_address(to, tolen)) return SOCKET_ERROR;
  ULONG sent = 0;
  const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, to, (ULONG)address_size(to, tolen));
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
  ULONG got = 0;
  ANY_SOCKADDR_ sender;
  ULONG sender_length = sizeof sender;
  BOOLEAN closed = FALSE;
  if (from && !fromlen) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const NTSTATUS status = NtReceiveSocket((HANDLE)(UINT_PTR)s, buf, (ULONG)len, &got, from ? &sender : NULL, from ? &sender_length : NULL, &closed);
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  if (!closed && !give_address(&sender, sender_length, from, fromlen)) return SOCKET_ERROR;
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
/* GetSockName / GetPeerName: the services write the socket address itself; its size follows from its family. */
static int get_name(SOCKET s, ULONG operation, SOCKADDR *out, int *outlen) {
  if (!out || !outlen) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  ANY_SOCKADDR_ name;
  memset(&name, 0, sizeof name);
  const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, operation, NULL, 0, &name, sizeof name);
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  return give_address(&name, (ULONG)address_size(&name.sa, sizeof name), out, outlen) ? 0 : SOCKET_ERROR;
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
  return octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);      /* network byte order: the first octet first in memory, as in a real in_addr */
}
/* inet_ntoa's buffer is the calling thread's, as on Windows (another thread's call does not overwrite it): a TLS slot, filled on first use, freed at thread detach. */
IMPORT DWORD WINAPI TlsAlloc(void);
IMPORT PVOID WINAPI TlsGetValue(DWORD slot);
IMPORT BOOL WINAPI TlsSetValue(DWORD slot, PVOID value);
IMPORT BOOL WINAPI TlsFree(DWORD slot);
IMPORT void *WINAPI GetProcessHeap(void);
IMPORT void *WINAPI HeapAlloc(void *heap, DWORD flags, size_t_ bytes);
IMPORT BOOL WINAPI HeapFree(void *heap, DWORD flags, void *memory);
static DWORD g_ntoa_slot = 0xFFFFFFFFu;
static char g_ntoa_fallback[16];      /* (if the slot or the memory could not be had) */
static char *ntoa_buffer(void) {
  if (g_ntoa_slot == 0xFFFFFFFFu) return g_ntoa_fallback;
  char *buffer = TlsGetValue(g_ntoa_slot);
  if (!buffer && (buffer = HeapAlloc(GetProcessHeap(), 0, 16)) != NULL && !TlsSetValue(g_ntoa_slot, buffer)) { HeapFree(GetProcessHeap(), 0, buffer); buffer = NULL; }
  return buffer ? buffer : g_ntoa_fallback;
}
EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved;
  if (reason == 1 /* DLL_PROCESS_ATTACH */) g_ntoa_slot = TlsAlloc();
  else if (reason == 3 /* DLL_THREAD_DETACH */ && g_ntoa_slot != 0xFFFFFFFFu) {
    char *buffer = TlsGetValue(g_ntoa_slot);
    if (buffer) { HeapFree(GetProcessHeap(), 0, buffer); TlsSetValue(g_ntoa_slot, NULL); }
  } else if (reason == 0 /* DLL_PROCESS_DETACH */ && g_ntoa_slot != 0xFFFFFFFFu) TlsFree(g_ntoa_slot);
  return TRUE;
}

EXPORT char *WSAAPI inet_ntoa(IN_ADDR in) {
  char *const buffer = ntoa_buffer();
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

/* ---- addresses as text and name resolution ---------------------------------------------------------------------------------------------------------------------------
 * inet_pton / inet_ntop (IPv4 and IPv6 text), gethostname, and getaddrinfo / getnameinfo: numeric addresses, a NULL node (the wildcard or the loopback), "localhost" and
 * this machine's own name are answered here; any other name goes to the kernel's DNS resolver (NtSocketControl ResolveName), AAAA and A records both for AF_UNSPEC (AAAA only
 * when some interface has an IPv6 route, as Windows' default address-configuration check does), IPv6 results first (RFC 6724's preference for native IPv6). */
IMPORT BOOL WINAPI GetComputerNameA(char *name, DWORD *size);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, const char *source, int source_length, WCHAR *destination, int destination_length);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, const WCHAR *source, int source_length, char *destination, int destination_length, const char *default_char, BOOL *used_default);

#define AF_UNSPEC_ 0
#define WSATYPE_NOT_FOUND_ 10109
#define WSAHOST_NOT_FOUND_ 11001
#define WSA_NOT_ENOUGH_MEMORY_ 8
#define AI_PASSIVE_ 0x1
#define AI_CANONNAME_ 0x2
#define AI_NUMERICHOST_ 0x4
#define NI_NUMERICHOST_ 0x2
#define NI_NAMEREQD_ 0x4
#define NI_NUMERICSERV_ 0x8
#define NI_DGRAM_ 0x10
#define INET6_TEXT_ 46
#define INET4_TEXT_ 16

typedef struct addrinfo_ { int ai_flags, ai_family, ai_socktype, ai_protocol; size_t_ ai_addrlen; char *ai_canonname; SOCKADDR *ai_addr; struct addrinfo_ *ai_next; } ADDRINFOA_;
typedef struct addrinfoW_ { int ai_flags, ai_family, ai_socktype, ai_protocol; size_t_ ai_addrlen; WCHAR *ai_canonname; SOCKADDR *ai_addr; struct addrinfoW_ *ai_next; } ADDRINFOW_;

static int lower_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static BOOL same_name(const char *a, const char *b) {
  while (*a && lower_ascii((UCHAR)*a) == lower_ascii((UCHAR)*b)) { ++a; ++b; }
  return *a == 0 && *b == 0;
}
static size_t_ text_length(const char *s) { size_t_ n = 0; while (s[n]) ++n; return n; }

/* Dotted decimal as inet_pton reads it (RtlIpv4StringToAddress, strict): four parts of 0..255 with no leading zeros. The address is written once the four parts are read,
 * even when text follows them (the call still fails then); `end` is where the parts end. FALSE (nothing written) for a bad part. */
static BOOL parse_ipv4_parts(const char *text, UCHAR out[4], const char **end, BOOL leading_zeros) {
  UCHAR parts[4];
  const char *p = text;
  for (int i = 0; i < 4; ++i) {
    if (*p < '0' || *p > '9') return FALSE;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return FALSE;      /* (hexadecimal is inet_addr's, not this) */
    if (!leading_zeros && p[0] == '0' && p[1] >= '0' && p[1] <= '9') return FALSE;      /* (a leading zero would be octal elsewhere) */
    unsigned v = 0, digits = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); ++p; if (++digits > 3 || v > 255) return FALSE; }
    parts[i] = (UCHAR)v;
    if (i < 3 && *p++ != '.') return FALSE;
  }
  memcpy(out, parts, 4);
  *end = p;
  return TRUE;
}
static BOOL parse_ipv4(const char *text, UCHAR out[4]) {
  UCHAR parts[4];
  const char *end;
  if (!parse_ipv4_parts(text, parts, &end, FALSE) || *end) return FALSE;
  memcpy(out, parts, 4);
  return TRUE;
}
static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* RFC 4291 text: groups of up to four hex digits, one "::" for a run of zero groups, and an IPv4 tail. */
static BOOL parse_ipv6(const char *text, UCHAR out[16]) {
  USHORT groups[8];
  int count = 0, gap = -1;
  const char *p = text;
  if (p[0] == ':' && p[1] == ':') { gap = 0; p += 2; if (!*p) goto done; }
  else if (*p == ':') return FALSE;
  for (;;) {
    const char *start = p;
    unsigned v = 0;
    int digits = 0;
    while (hex_value(*p) >= 0 && digits < 5) { v = v << 4 | (unsigned)hex_value(*p); ++p; ++digits; }
    if (*p == '.') {      /* the last 32 bits as dotted decimal */
      UCHAR v4[4];
      const char *end;
      if (count > 6 || !parse_ipv4_parts(start, v4, &end, TRUE) || *end) return FALSE;      /* (the tail of an IPv6 address may have leading zeros) */
      groups[count++] = (USHORT)(v4[0] << 8 | v4[1]);
      groups[count++] = (USHORT)(v4[2] << 8 | v4[3]);
      break;
    }
    if (digits == 0 || digits > 4 || count == 8) return FALSE;
    groups[count++] = (USHORT)v;
    if (!*p) break;
    if (*p != ':') return FALSE;
    ++p;
    if (*p == ':') { if (gap >= 0) return FALSE; gap = count; ++p; if (!*p) break; }
  }
done:
  if (gap < 0 ? count != 8 : count > 7) return FALSE;
  for (int i = 0; i < 16; ++i) out[i] = 0;
  const int tail = gap < 0 ? 0 : count - gap;
  for (int i = 0; i < count - tail; ++i) { out[2 * i] = (UCHAR)(groups[i] >> 8); out[2 * i + 1] = (UCHAR)groups[i]; }
  for (int i = 0; i < tail; ++i) { const int at = 8 - tail + i; out[2 * at] = (UCHAR)(groups[gap + i] >> 8); out[2 * at + 1] = (UCHAR)groups[gap + i]; }
  return TRUE;
}

static int format_ipv4(const UCHAR *a, char *out) {
  int o = 0;
  for (int i = 0; i < 4; ++i) {
    const unsigned v = a[i];
    if (v >= 100) out[o++] = (char)('0' + v / 100);
    if (v >= 10) out[o++] = (char)('0' + v / 10 % 10);
    out[o++] = (char)('0' + v % 10);
    if (i < 3) out[o++] = '.';
  }
  out[o] = 0;
  return o;
}
/* The canonical form: lower-case hex, the longest run of two or more zero groups as "::", IPv4-mapped and -compatible addresses with a dotted tail, and so ISATAP
 * ones (the interface identifier 0:5efe or 200:5efe and then the IPv4 address), as RtlIpv6AddressToString writes them. */
static int format_ipv6(const UCHAR *a, char *out) {
  static const char hex[] = "0123456789abcdef";
  USHORT g[8];
  for (int i = 0; i < 8; ++i) g[i] = (USHORT)(a[2 * i] << 8 | a[2 * i + 1]);
  int best = -1, best_length = 0;
  for (int i = 0; i < 8;) {
    if (g[i]) { ++i; continue; }
    int j = i;
    while (j < 8 && !g[j]) ++j;
    if (j - i > best_length) { best = i; best_length = j - i; }
    i = j;
  }
  const BOOL isatap = (g[4] & 0xFDFF) == 0 && g[5] == 0x5EFE;
  const BOOL v4_tail = (best == 0 && (best_length == 6 || (best_length == 5 && g[5] == 0xFFFF))) || isatap;
  if (isatap && best >= 0 && best + best_length > 6) best_length = 6 - best;      /* (a run of zeros stops where the dotted tail begins) */
  if (best_length < 2) best = -1;
  int o = 0;
  for (int i = 0; i < 8; ++i) {
    if (i == best) { out[o++] = ':'; if (i == 0) out[o++] = ':'; i += best_length - 1; continue; }
    if (v4_tail && i == 6) { o += format_ipv4(a + 12, out + o); return o; }
    int shift = 12;
    while (shift > 0 && !(g[i] >> shift & 0xF)) shift -= 4;
    for (; shift >= 0; shift -= 4) out[o++] = hex[g[i] >> shift & 0xF];
    if (i < 7) out[o++] = ':';
  }
  out[o] = 0;
  return o;
}

EXPORT int WSAAPI inet_pton(int family, const char *text, void *out) {
  if (!text || !out) { set_wsa_error(WSAEFAULT_); return -1; }
  if (family == AF_INET_) { const char *end; return parse_ipv4_parts(text, out, &end, FALSE) && !*end ? 1 : 0; }
  if (family == AF_INET6_) return parse_ipv6(text, out) ? 1 : 0;
  set_wsa_error(WSAEAFNOSUPPORT_);
  return -1;
}
EXPORT const char *WSAAPI inet_ntop(int family, const void *address, char *out, size_t_ size) {
  char text[INET6_TEXT_];
  if (!address || !out) { set_wsa_error(WSAEINVAL_); return NULL; }
  if (family == AF_INET_) format_ipv4(address, text);
  else if (family == AF_INET6_) format_ipv6(address, text);
  else { set_wsa_error(WSAEAFNOSUPPORT_); return NULL; }
  const size_t_ n = text_length(text);
  if (size <= n) { set_wsa_error(WSAEINVAL_); return NULL; }
  memcpy(out, text, n + 1);
  return out;
}
EXPORT int WSAAPI InetPtonW(int family, const WCHAR *text, void *out) {
  char narrow[INET6_TEXT_ + 8];
  if (!text || !out) { set_wsa_error(WSAEFAULT_); return -1; }
  int n = 0;
  for (; text[n] && n < (int)sizeof narrow - 1; ++n) { if (text[n] > 0x7F) { set_wsa_error(WSAEINVAL_); return 0; } narrow[n] = (char)text[n]; }
  if (text[n]) { set_wsa_error(WSAEINVAL_); return 0; }
  narrow[n] = 0;
  const int ret = inet_pton(family, narrow, out);
  if (ret == 0) set_wsa_error(WSAEINVAL_);      /* (the W form says why, the A form does not) */
  return ret;
}
EXPORT const WCHAR *WSAAPI InetNtopW(int family, const void *address, WCHAR *out, size_t_ size) {
  char narrow[INET6_TEXT_];
  if (!out) { set_wsa_error(87); return NULL; }
  if (!inet_ntop(family, address, narrow, sizeof narrow)) return NULL;
  const size_t_ n = text_length(narrow);
  if (size <= n) { set_wsa_error(87); return NULL; }
  for (size_t_ i = 0; i <= n; ++i) out[i] = (WCHAR)(UCHAR)narrow[i];
  return out;
}

EXPORT int WSAAPI gethostname(char *name, int size) {
  char computer[64];
  DWORD length = sizeof computer;
  if (!require_started()) return SOCKET_ERROR;
  if (!name) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  if (!GetComputerNameA(computer, &length)) { memcpy(computer, "localhost", 10); length = 9; }
  if (size <= (int)length) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  memcpy(name, computer, length + 1);
  return 0;
}

/* The address of this machine: the first configured interface's, else the loopback. */
static void own_address(UCHAR out[4]) {
  out[0] = 127; out[1] = 0; out[2] = 0; out[3] = 1;
  HANDLE probe = NULL;
  if (NtCreateSocket(AF_INET_, SOCK_DGRAM_, IPPROTO_UDP_, &probe) < 0) return;
  for (ULONG i = 0; i < 8; ++i) {
    UCHAR config[16];
    if (NtSocketControl(probe, 5 /* InterfaceConfiguration */, &i, sizeof i, config, sizeof config) != 0) break;
    if (config[0] | config[1] | config[2] | config[3]) { memcpy(out, config, 4); break; }
  }
  NtClose(probe);
}
static BOOL is_own_name(const char *node) {
  char computer[64];
  DWORD length = sizeof computer;
  return GetComputerNameA(computer, &length) && same_name(node, computer);
}

/* A service: a port number, or one of the well-known names. */
static BOOL parse_service(const char *service, int socktype, USHORT *port) {
  static const struct { const char *name; USHORT port; } kServices[] = {
      {"echo", 7}, {"ftp-data", 20}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"domain", 53}, {"http", 80}, {"www", 80}, {"pop3", 110}, {"ntp", 123},
      {"imap", 143}, {"snmp", 161}, {"ldap", 389}, {"https", 443}, {"imaps", 993}, {"pop3s", 995}};
  (void)socktype;
  *port = 0;
  if (!service || !*service) return TRUE;      /* (no service, or an empty one: port 0) */
  if (*service >= '0' && *service <= '9') {
    unsigned v = 0;
    for (const char *p = service; *p; ++p) { if (*p < '0' || *p > '9') return FALSE; v = v * 10 + (unsigned)(*p - '0'); if (v > 65535) return FALSE; }
    *port = (USHORT)v;
    return TRUE;
  }
  for (size_t_ i = 0; i < sizeof kServices / sizeof kServices[0]; ++i) if (same_name(service, kServices[i].name)) { *port = kServices[i].port; return TRUE; }
  return FALSE;
}

/* One entry: the addrinfo, its sockaddr and its canonical name in one block (wide: the name as UTF-16 for ADDRINFOW). freeaddrinfo frees the blocks one by one. */
static ADDRINFOA_ *new_entry(int family, const UCHAR *address, USHORT port, const ADDRINFOA_ *hints, const char *canonical, BOOL wide) {
  const size_t_ sockaddr_size = family == AF_INET6_ ? sizeof(SOCKADDR_IN6_) : sizeof(SOCKADDR_IN);
  const size_t_ name_chars = canonical ? text_length(canonical) + 1 : 0;
  const size_t_ name_bytes = name_chars * (wide ? sizeof(WCHAR) : 1);
  BYTE *block = HeapAlloc(GetProcessHeap(), 0, sizeof(ADDRINFOA_) + sockaddr_size + name_bytes);
  if (!block) return NULL;
  memset(block, 0, sizeof(ADDRINFOA_) + sockaddr_size + name_bytes);
  ADDRINFOA_ *ai = (ADDRINFOA_ *)block;
  ai->ai_family = family;
  ai->ai_socktype = hints ? hints->ai_socktype : 0;
  ai->ai_protocol = hints ? hints->ai_protocol : 0;
  ai->ai_addrlen = sockaddr_size;
  ai->ai_addr = (SOCKADDR *)(block + sizeof(ADDRINFOA_));
  if (family == AF_INET6_) {
    SOCKADDR_IN6_ *in6 = (SOCKADDR_IN6_ *)ai->ai_addr;
    in6->sin6_family = AF_INET6_;
    in6->sin6_port = htons(port);
    memcpy(in6->sin6_addr, address, 16);
  } else {
    SOCKADDR_IN *in = (SOCKADDR_IN *)ai->ai_addr;
    in->sin_family = AF_INET_;
    in->sin_port = htons(port);
    memcpy(&in->sin_addr, address, 4);
  }
  if (canonical) {
    BYTE *name = block + sizeof(ADDRINFOA_) + sockaddr_size;
    if (wide) for (size_t_ i = 0; i < name_chars; ++i) ((WCHAR *)name)[i] = (WCHAR)(UCHAR)canonical[i];
    else memcpy(name, canonical, name_chars);
    ai->ai_canonname = (char *)name;
  }
  return ai;
}

/* Asks the kernel's resolver for `node`'s A (AF_INET) or AAAA (AF_INET6) record, through a throw-away socket handle (the services hang off one). */
static BOOL dns_lookup(const char *node, int family, UCHAR out[16]) {
  UCHAR in[4 + 253];
  const size_t_ n = text_length(node);
  if (n == 0 || n > 253) return FALSE;
  const ULONG f = (ULONG)family;
  memcpy(in, &f, 4);
  memcpy(in + 4, node, n);
  HANDLE probe = NULL;
  if (NtCreateSocket(AF_INET_, SOCK_DGRAM_, IPPROTO_UDP_, &probe) < 0) return FALSE;
  const NTSTATUS status = NtSocketControl(probe, 9 /* ResolveName */, in, (ULONG)(4 + n), out, family == AF_INET6_ ? 16 : 4);
  NtClose(probe);
  return NT_SUCCESS(status);
}
static BOOL has_ipv6_route(void) {
  HANDLE probe = NULL;
  UCHAR routed = 0;
  if (NtCreateSocket(AF_INET_, SOCK_DGRAM_, IPPROTO_UDP_, &probe) < 0) return FALSE;
  if (!NT_SUCCESS(NtSocketControl(probe, 10 /* HasIpv6Route */, NULL, 0, &routed, 1))) routed = 0;
  NtClose(probe);
  return routed != 0;
}
/* This machine's global IPv6 address, if an interface has one. */
static BOOL own_address6(UCHAR out[16]) {
  HANDLE probe = NULL;
  BOOL found = FALSE;
  if (NtCreateSocket(AF_INET_, SOCK_DGRAM_, IPPROTO_UDP_, &probe) < 0) return FALSE;
  for (ULONG i = 0; i < 8 && !found; ++i) {
    UCHAR config[72];
    if (NtSocketControl(probe, 8 /* InterfaceConfiguration6 */, &i, sizeof i, config, sizeof config) != 0) break;
    if (config[65] & 2) { memcpy(out, config + 16, 16); found = TRUE; }      /* (flags bit 1: the global address is ready) */
  }
  NtClose(probe);
  return found;
}

static void free_entries(ADDRINFOA_ *ai);

#define AI_V4MAPPED_ 0x800
#define AI_ALL_ 0x100
#define MAX_RESULTS_ 4

static int resolve(const char *node, const char *service, const ADDRINFOA_ *hints, ADDRINFOA_ **result, BOOL wide) {
  if (!result) return WSAEFAULT_;      /* (not a code of its own on Windows either) */
  *result = NULL;
  if (!node && !service) return WSAHOST_NOT_FOUND_;
  const int family = hints ? hints->ai_family : AF_UNSPEC_, flags = hints ? hints->ai_flags : 0;
  if (family != AF_UNSPEC_ && family != AF_INET_ && family != AF_INET6_) return WSAEAFNOSUPPORT_;
  if (hints && (hints->ai_socktype < 0 || hints->ai_socktype > 5)) return WSAESOCKTNOSUPPORT_;
  USHORT port = 0;
  if (!parse_service(service, hints ? hints->ai_socktype : 0, &port)) return WSATYPE_NOT_FOUND_;
  /* Up to MAX_RESULTS_ addresses, in the order they are given back. */
  UCHAR addresses[MAX_RESULTS_][16];
  int families[MAX_RESULTS_];
  int count = 0;
  const char *canonical = node;
#define ADD_(f, bytes) do { if (count < MAX_RESULTS_) { families[count] = (f); memcpy(addresses[count], (bytes), (f) == AF_INET6_ ? 16 : 4); ++count; } } while (0)
  static const UCHAR kLoop4[4] = {127, 0, 0, 1}, kAny4[4] = {0, 0, 0, 0};
  static const UCHAR kLoop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, kAny6[16] = {0};
  UCHAR address[16];
  if (!node) {
    /* The wildcard (AI_PASSIVE) or the loopback, of the family asked for (AF_UNSPEC: IPv4, which every program can use, as Windows answers it). */
    if (family == AF_INET6_) ADD_(AF_INET6_, (flags & AI_PASSIVE_) ? kAny6 : kLoop6);
    else ADD_(AF_INET_, (flags & AI_PASSIVE_) ? kAny4 : kLoop4);
  } else if (parse_ipv4(node, address)) {
    if (family == AF_INET6_) {
      if (!(flags & AI_V4MAPPED_)) return WSAHOST_NOT_FOUND_;
      UCHAR mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
      memcpy(mapped + 12, address, 4);
      ADD_(AF_INET6_, mapped);
    } else {
      ADD_(AF_INET_, address);
    }
  } else if (parse_ipv6(node, address)) {
    if (family == AF_INET_) return WSAHOST_NOT_FOUND_;
    ADD_(AF_INET6_, address);
  } else if (flags & AI_NUMERICHOST_) {
    return WSAHOST_NOT_FOUND_;
  } else if (same_name(node, "localhost")) {
    if (family != AF_INET_) ADD_(AF_INET6_, kLoop6);
    if (family != AF_INET6_) ADD_(AF_INET_, kLoop4);
  } else if (is_own_name(node) || !*node) {
    if (family != AF_INET_ && own_address6(address)) ADD_(AF_INET6_, address);
    if (family != AF_INET6_) { own_address(address); ADD_(AF_INET_, address); }
    if (count == 0) ADD_(AF_INET6_, kLoop6);
  } else {
    /* DNS. AF_UNSPEC asks for AAAA only when an IPv6 route exists (an address nobody can reach is worse than none); AF_INET6 with AI_V4MAPPED falls back to A
     * records, mapped (AI_ALL: both). */
    const BOOL want6 = family == AF_INET6_ || (family == AF_UNSPEC_ && has_ipv6_route());
    if (want6 && dns_lookup(node, AF_INET6_, address)) ADD_(AF_INET6_, address);
    if (family == AF_INET6_ && (flags & AI_V4MAPPED_) && (count == 0 || (flags & AI_ALL_)) && dns_lookup(node, AF_INET_, address)) {
      UCHAR mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
      memcpy(mapped + 12, address, 4);
      ADD_(AF_INET6_, mapped);
    }
    if (family != AF_INET6_ && dns_lookup(node, AF_INET_, address)) ADD_(AF_INET_, address);
    if (count == 0) return WSAHOST_NOT_FOUND_;
  }
#undef ADD_
  ADDRINFOA_ **tail = result;
  for (int i = 0; i < count; ++i) {
    /* (only the first entry carries the canonical name, as on Windows) */
    ADDRINFOA_ *ai = new_entry(families[i], addresses[i], port, hints, i == 0 && (flags & AI_CANONNAME_) ? (canonical ? canonical : "localhost") : NULL, wide);
    if (!ai) { free_entries(*result); *result = NULL; return WSA_NOT_ENOUGH_MEMORY_; }
    ai->ai_flags = flags;
    *tail = ai;
    tail = &ai->ai_next;
  }
  return 0;
}

static void free_entries(ADDRINFOA_ *ai) {
  while (ai) {
    ADDRINFOA_ *next = ai->ai_next;
    HeapFree(GetProcessHeap(), 0, ai);
    ai = next;
  }
}

EXPORT int WSAAPI getaddrinfo(const char *node, const char *service, const ADDRINFOA_ *hints, ADDRINFOA_ **result) {
  const int error = resolve(node, service, hints, result, FALSE);
  set_wsa_error(error);
  return error;
}
EXPORT void WSAAPI freeaddrinfo(ADDRINFOA_ *ai) { free_entries(ai); }

/* The W form: the names in UTF-8 for the resolver, the canonical name back as UTF-16. */
static char *narrow_copy(const WCHAR *text) {
  if (!text) return NULL;
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
  char *out = n > 0 ? HeapAlloc(GetProcessHeap(), 0, (size_t_)n) : NULL;
  if (out) WideCharToMultiByte(CP_UTF8, 0, text, -1, out, n, NULL, NULL);
  return out;
}
EXPORT int WSAAPI GetAddrInfoW(const WCHAR *node, const WCHAR *service, const ADDRINFOW_ *hints, ADDRINFOW_ **result) {
  char *narrow_node = narrow_copy(node), *narrow_service = narrow_copy(service);
  int error;
  if ((node && !narrow_node) || (service && !narrow_service)) error = WSA_NOT_ENOUGH_MEMORY_;
  else error = resolve(narrow_node, narrow_service, (const ADDRINFOA_ *)hints, (ADDRINFOA_ **)result, TRUE);      /* (the same layout but for the name's type) */
  if (narrow_node) HeapFree(GetProcessHeap(), 0, narrow_node);
  if (narrow_service) HeapFree(GetProcessHeap(), 0, narrow_service);
  set_wsa_error(error);
  return error;
}
EXPORT void WSAAPI FreeAddrInfoW(ADDRINFOW_ *ai) { free_entries((ADDRINFOA_ *)ai); }

/* getnameinfo: numeric text for the address (a name only for the loopback, "localhost"; NI_NAMEREQD fails otherwise) and the port. */
EXPORT int WSAAPI getnameinfo(const SOCKADDR *address, int length, char *host, DWORD host_size, char *service, DWORD service_size, int flags) {
  char text[INET6_TEXT_];
  USHORT port = 0;
  if (!address || length < (int)sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return WSAEFAULT_; }
  if (address->sa_family == AF_INET_) {
    const SOCKADDR_IN *in = (const SOCKADDR_IN *)address;
    format_ipv4((const UCHAR *)&in->sin_addr, text);
    port = ntohs(in->sin_port);
    if (!(flags & NI_NUMERICHOST_) && ((const UCHAR *)&in->sin_addr)[0] == 127) memcpy(text, "localhost", 10);
    else if (flags & NI_NAMEREQD_) { set_wsa_error(WSAHOST_NOT_FOUND_); return WSAHOST_NOT_FOUND_; }
  } else if (address->sa_family == AF_INET6_ && length >= (int)sizeof(SOCKADDR_IN6_)) {
    const SOCKADDR_IN6_ *in6 = (const SOCKADDR_IN6_ *)address;
    format_ipv6(in6->sin6_addr, text);
    port = ntohs(in6->sin6_port);
    if (flags & NI_NAMEREQD_) { set_wsa_error(WSAHOST_NOT_FOUND_); return WSAHOST_NOT_FOUND_; }
  } else {
    set_wsa_error(WSAEAFNOSUPPORT_);
    return WSAEAFNOSUPPORT_;
  }
  if (host && host_size) {
    const size_t_ n = text_length(text);
    if (host_size <= n) { set_wsa_error(WSAEFAULT_); return WSAEFAULT_; }
    memcpy(host, text, n + 1);
  }
  if (service && service_size) {
    char digits[8];
    int o = 0;
    char reversed[8];
    int r = 0;
    do { reversed[r++] = (char)('0' + port % 10); port = (USHORT)(port / 10); } while (port);
    while (r) digits[o++] = reversed[--r];
    digits[o] = 0;
    if (service_size <= (DWORD)o) { set_wsa_error(WSAEFAULT_); return WSAEFAULT_; }
    memcpy(service, digits, (size_t_)o + 1);
  }
  (void)NI_NUMERICSERV_; (void)NI_DGRAM_;      /* (services are always numeric here) */
  set_wsa_error(0);
  return 0;
}
EXPORT int WSAAPI GetNameInfoW(const SOCKADDR *address, int length, WCHAR *host, DWORD host_size, WCHAR *service, DWORD service_size, int flags) {
  char narrow_host[INET6_TEXT_ + 16], narrow_service[8];
  const int error = getnameinfo(address, length, host ? narrow_host : NULL, host ? sizeof narrow_host : 0, service ? narrow_service : NULL, service ? sizeof narrow_service : 0, flags);
  if (error) return error;
  if (host && host_size) {
    if (MultiByteToWideChar(CP_UTF8, 0, narrow_host, -1, host, (int)host_size) <= 0) { set_wsa_error(WSAEFAULT_); return WSAEFAULT_; }
  }
  if (service && service_size) {
    if (MultiByteToWideChar(CP_UTF8, 0, narrow_service, -1, service, (int)service_size) <= 0) { set_wsa_error(WSAEFAULT_); return WSAEFAULT_; }
  }
  return 0;
}

/* The name-space lookups (WSALookupService*): no name-space provider is installed, so a lookup finds nothing (a stub answered success, and callers looping over
 * WSALookupServiceNext until it fails never stopped). */
#define WSASERVICE_NOT_FOUND_ 10108
#define WSA_E_NO_MORE_ 10110
EXPORT int WSAAPI WSALookupServiceBeginA(void *query, DWORD flags, HANDLE *lookup) {
  (void)query; (void)flags;
  if (lookup) *lookup = NULL;
  set_wsa_error(WSASERVICE_NOT_FOUND_);
  return SOCKET_ERROR;
}
EXPORT int WSAAPI WSALookupServiceBeginW(void *query, DWORD flags, HANDLE *lookup) { return WSALookupServiceBeginA(query, flags, lookup); }
EXPORT int WSAAPI WSALookupServiceNextA(HANDLE lookup, DWORD flags, DWORD *length, void *results) {
  (void)lookup; (void)flags; (void)length; (void)results;
  set_wsa_error(WSA_E_NO_MORE_);
  return SOCKET_ERROR;
}
EXPORT int WSAAPI WSALookupServiceNextW(HANDLE lookup, DWORD flags, DWORD *length, void *results) { return WSALookupServiceNextA(lookup, flags, length, results); }
EXPORT int WSAAPI WSALookupServiceEnd(HANDLE lookup) { (void)lookup; return 0; }

/* ---- socket options -----------------------------------------------------------------------------------------------------------------------------------------------------
 * IPV6_V6ONLY is real (it decides whether an AF_INET6 socket also carries IPv4). The other common options are accepted and change nothing - the network stack has one
 * fixed behaviour for each (no Nagle delay to turn off, fixed 16 KiB buffers, no keep-alives) - and read back as that behaviour. Anything else is WSAENOPROTOOPT. */
#define SOL_SOCKET_ 0xFFFF
#define IPPROTO_IP_ 0
#define IPPROTO_IPV6_ 41
#define SO_REUSEADDR_ 0x0004
#define SO_KEEPALIVE_ 0x0008
#define SO_DONTROUTE_ 0x0010
#define SO_BROADCAST_ 0x0020
#define SO_LINGER_ 0x0080
#define SO_OOBINLINE_ 0x0100
#define SO_SNDBUF_ 0x1001
#define SO_RCVBUF_ 0x1002
#define SO_SNDTIMEO_ 0x1005
#define SO_RCVTIMEO_ 0x1006
#define SO_ERROR_ 0x1007
#define SO_TYPE_ 0x1008
#define SO_EXCLUSIVEADDRUSE_ ((int)(~SO_REUSEADDR_))
#define SO_DONTLINGER_ ((int)(~SO_LINGER_))
#define TCP_NODELAY_ 0x0001
#define IP_TOS_ 3
#define IP_TTL_ 4
#define IPV6_UNICAST_HOPS_ 4
#define IPV6_V6ONLY_ 27

typedef struct { ULONG family, type; UCHAR v6only, pad[3]; } SOCKET_INFO_;
static BOOL socket_info(SOCKET s, SOCKET_INFO_ *info) {
  const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, 7 /* GetSocketInfo */, NULL, 0, info, sizeof *info);
  if (!NT_SUCCESS(status)) { fail_nt(status, SOCKET_ERROR); return FALSE; }
  return TRUE;
}
/* The options that are accepted without changing anything. */
static BOOL inert_option(int level, int name) {
  if (level == SOL_SOCKET_)
    return name == SO_REUSEADDR_ || name == SO_KEEPALIVE_ || name == SO_DONTROUTE_ || name == SO_BROADCAST_ || name == SO_LINGER_ || name == SO_OOBINLINE_ ||
           name == SO_SNDBUF_ || name == SO_RCVBUF_ || name == SO_SNDTIMEO_ || name == SO_RCVTIMEO_ || name == SO_EXCLUSIVEADDRUSE_ || name == SO_DONTLINGER_;
  if (level == IPPROTO_TCP_) return name == TCP_NODELAY_;
  if (level == IPPROTO_IP_) return name == IP_TOS_ || name == IP_TTL_;
  if (level == IPPROTO_IPV6_) return name == IPV6_UNICAST_HOPS_;
  return FALSE;
}

EXPORT int WSAAPI setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen) {
  SOCKET_INFO_ info;
  if (!socket_info(s, &info)) return SOCKET_ERROR;
  if (level == IPPROTO_IPV6_ && optname == IPV6_V6ONLY_) {
    if (!optval || optlen < 1) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
    if (info.family != AF_INET6_) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
    DWORD value = 0;
    memcpy(&value, optval, optlen < 4 ? (size_t_)optlen : 4);
    const UCHAR on = value != 0;
    const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, 6 /* SetV6Only */, (PVOID)&on, 1, NULL, 0);
    return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
  }
  if (!inert_option(level, optname)) { set_wsa_error(WSAENOPROTOOPT_); return SOCKET_ERROR; }
  if (level == IPPROTO_IPV6_ && info.family != AF_INET6_) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  if (!optval && optlen > 0) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  return 0;
}

EXPORT int WSAAPI getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen) {
  SOCKET_INFO_ info;
  if (!socket_info(s, &info)) return SOCKET_ERROR;
  if (!optval || !optlen) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  DWORD value = 0;
  if (level == SOL_SOCKET_ && optname == SO_TYPE_) value = info.type;
  else if (level == SOL_SOCKET_ && optname == SO_ERROR_) value = 0;      /* (a failed connect is reported by the call itself, so there is nothing pending here) */
  else if (level == SOL_SOCKET_ && (optname == SO_SNDBUF_ || optname == SO_RCVBUF_)) value = 16384;
  else if (level == SOL_SOCKET_ && optname == SO_LINGER_) {
    if (*optlen < 4) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
    memset(optval, 0, 4);      /* struct linger { u_short l_onoff, l_linger; }: off */
    *optlen = 4;
    return 0;
  } else if (level == IPPROTO_IPV6_ && optname == IPV6_V6ONLY_) {
    if (info.family != AF_INET6_) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
    value = info.v6only;
  } else if (level == IPPROTO_IP_ && optname == IP_TTL_) value = 128;
  else if (level == IPPROTO_IPV6_ && optname == IPV6_UNICAST_HOPS_) value = 64;
  else if (inert_option(level, optname)) value = 0;
  else { set_wsa_error(WSAENOPROTOOPT_); return SOCKET_ERROR; }
  if (*optlen < 4) {
    /* (Windows accepts a one-byte buffer for the BOOL-valued ones) */
    if (*optlen < 1) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
    *optval = (char)(value != 0);
    *optlen = 1;
    return 0;
  }
  memcpy(optval, &value, 4);
  *optlen = 4;
  return 0;
}

/* WSASocket: socket() with protocol-info and flags. A protocol-info structure only repeats what af/type/protocol say for the protocols there are; WSA_FLAG_OVERLAPPED is
 * accepted (these sockets complete every call at once or block in it, which overlapped callers that wait for completion also handle). */
EXPORT SOCKET WSAAPI WSASocketW(int af, int type, int protocol, void *protocol_info, unsigned group, DWORD flags) {
  (void)protocol_info; (void)group; (void)flags;
  return socket(af, type, protocol);
}
EXPORT SOCKET WSAAPI WSASocketA(int af, int type, int protocol, void *protocol_info, unsigned group, DWORD flags) {
  return WSASocketW(af, type, protocol, protocol_info, group, flags);
}

/* ---- addresses as text with their ports ---------------------------------------------------------------------------------------------------------------------------------
 * WSAAddressToString: "a.b.c.d:port", "[v6%scope]:port", or without the port when it is 0 (and without brackets then). WSAStringToAddress reads the same forms back. */
static int append_decimal(char *out, ULONG v) {
  char reversed[12];
  int r = 0, o = 0;
  do { reversed[r++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (r) out[o++] = reversed[--r];
  out[o] = 0;
  return o;
}
static int address_text(const SOCKADDR *address, DWORD length, char *text) {
  if (!address || length < sizeof(SOCKADDR_IN)) return -1;
  int o = 0;
  if (address->sa_family == AF_INET_) {
    const SOCKADDR_IN *in = (const SOCKADDR_IN *)address;
    o = format_ipv4((const UCHAR *)&in->sin_addr, text);
    if (in->sin_port) { text[o++] = ':'; o += append_decimal(text + o, ntohs(in->sin_port)); }
    return o;
  }
  if (address->sa_family == AF_INET6_ && length >= sizeof(SOCKADDR_IN6_)) {
    const SOCKADDR_IN6_ *in6 = (const SOCKADDR_IN6_ *)address;
    if (in6->sin6_port) text[o++] = '[';
    o += format_ipv6(in6->sin6_addr, text + o);
    if (in6->sin6_scope_id) { text[o++] = '%'; o += append_decimal(text + o, in6->sin6_scope_id); }
    if (in6->sin6_port) { text[o++] = ']'; text[o++] = ':'; o += append_decimal(text + o, ntohs(in6->sin6_port)); }
    return o;
  }
  return -1;
}
EXPORT int WSAAPI WSAAddressToStringA(SOCKADDR *address, DWORD length, void *protocol_info, char *out, DWORD *out_length) {
  (void)protocol_info;
  char text[INET6_TEXT_ + 24];
  if (!out_length) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const int n = address_text(address, length, text);
  if (n < 0) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  if (!out || *out_length < (DWORD)n + 1) { *out_length = (DWORD)n + 1; set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  memcpy(out, text, (size_t_)n + 1);
  *out_length = (DWORD)n + 1;
  return 0;
}
EXPORT int WSAAPI WSAAddressToStringW(SOCKADDR *address, DWORD length, void *protocol_info, WCHAR *out, DWORD *out_length) {
  (void)protocol_info;
  char text[INET6_TEXT_ + 24];
  if (!out_length) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const int n = address_text(address, length, text);
  if (n < 0) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  if (!out || *out_length < (DWORD)n + 1) { *out_length = (DWORD)n + 1; set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  for (int i = 0; i <= n; ++i) out[i] = (WCHAR)(UCHAR)text[i];
  *out_length = (DWORD)n + 1;
  return 0;
}

/* A decimal port after a ':' (1..5 digits, at most 65535). */
static BOOL parse_port_text(const char *p, USHORT *port) {
  unsigned v = 0, digits = 0;
  for (; *p; ++p) { if (*p < '0' || *p > '9' || ++digits > 5) return FALSE; v = v * 10 + (unsigned)(*p - '0'); }
  if (digits == 0 || v > 65535) return FALSE;
  *port = (USHORT)v;
  return TRUE;
}
static int string_to_address(const char *text, int family, SOCKADDR *out, int *out_length) {
  char copy[INET6_TEXT_ + 24];
  if (!text || !out_length) return WSAEFAULT_;
  /* what does not parse leaves a zeroed address (family 0), as on Windows */
  const int size = family == AF_INET6_ ? (int)sizeof(SOCKADDR_IN6_) : (int)sizeof(SOCKADDR_IN);
  if (out && *out_length >= size && (family == AF_INET_ || family == AF_INET6_)) memset(out, 0, (size_t_)size);
  const size_t_ n = text_length(text);
  if (n == 0 || n >= sizeof copy) return WSAEINVAL_;
  memcpy(copy, text, n + 1);
  USHORT port = 0;
  ULONG scope = 0;
  if (family == AF_INET_) {
    char *colon = NULL;
    for (char *p = copy; *p; ++p) if (*p == ':') colon = p;
    if (colon) { *colon = 0; if (!parse_port_text(colon + 1, &port)) return WSAEINVAL_; }
    UCHAR a[4];
    if (!parse_ipv4(copy, a)) return WSAEINVAL_;
    if (!out || *out_length < (int)sizeof(SOCKADDR_IN)) { *out_length = sizeof(SOCKADDR_IN); return WSAEFAULT_; }
    SOCKADDR_IN *in = (SOCKADDR_IN *)out;
    memset(in, 0, sizeof *in);
    in->sin_family = AF_INET_;
    in->sin_port = htons(port);
    memcpy(&in->sin_addr, a, 4);
    *out_length = sizeof(SOCKADDR_IN);
    return 0;
  }
  if (family != AF_INET6_) return WSAEINVAL_;
  char *start = copy;
  if (*start == '[') {
    char *close = start + 1;
    while (*close && *close != ']') ++close;
    if (*close != ']') return WSAEINVAL_;
    *close = 0;
    if (close[1] == ':') { if (!parse_port_text(close + 2, &port)) return WSAEINVAL_; }
    else if (close[1]) return WSAEINVAL_;
    ++start;
  }
  for (char *p = start; *p; ++p)
    if (*p == '%') {
      *p = 0;
      unsigned v = 0, digits = 0;
      for (const char *q = p + 1; *q; ++q) { if (*q < '0' || *q > '9' || ++digits > 9) return WSAEINVAL_; v = v * 10 + (unsigned)(*q - '0'); }
      if (digits == 0) return WSAEINVAL_;
      scope = v;
      break;
    }
  UCHAR a[16];
  if (!parse_ipv6(start, a)) return WSAEINVAL_;
  if (!out || *out_length < (int)sizeof(SOCKADDR_IN6_)) { *out_length = sizeof(SOCKADDR_IN6_); return WSAEFAULT_; }
  SOCKADDR_IN6_ *in6 = (SOCKADDR_IN6_ *)out;
  memset(in6, 0, sizeof *in6);
  in6->sin6_family = AF_INET6_;
  in6->sin6_port = htons(port);
  memcpy(in6->sin6_addr, a, 16);
  in6->sin6_scope_id = scope;
  *out_length = sizeof(SOCKADDR_IN6_);
  return 0;
}
EXPORT int WSAAPI WSAStringToAddressA(char *text, int family, void *protocol_info, SOCKADDR *out, int *out_length) {
  (void)protocol_info;
  const int error = string_to_address(text, family, out, out_length);
  if (error) { set_wsa_error(error); return SOCKET_ERROR; }
  return 0;
}
EXPORT int WSAAPI WSAStringToAddressW(WCHAR *text, int family, void *protocol_info, SOCKADDR *out, int *out_length) {
  (void)protocol_info;
  char narrow[INET6_TEXT_ + 24];
  if (!text) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  int n = 0;
  for (; text[n] && n < (int)sizeof narrow - 1; ++n) { if (text[n] > 0x7F) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; } narrow[n] = (char)text[n]; }
  if (text[n]) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  narrow[n] = 0;
  return WSAStringToAddressA(narrow, family, protocol_info, out, out_length);
}
