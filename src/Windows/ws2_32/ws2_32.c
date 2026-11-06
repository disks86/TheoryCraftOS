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
 * and port a program hands in or reads back out are a real `struct sockaddr_in`'s bytes (network byte order). The services carry the port as those bytes in a USHORT, and the
 * address as one integer with the first octet in the top byte, so the address is byte-swapped at the boundary (kernel_address / program_address). Name resolution
 * (getaddrinfo and friends) knows numeric addresses, localhost and this machine's own name; there is no DNS client for Windows programs yet. Built freestanding with the OS's
 * mingw compiler, directly over kernel32.dll and ntdll.dll, the way a real ws2_32.dll sits over ntdll for everything but the socket calls themselves. */
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

/* An IPv4 address as a program keeps it (in_addr.S_addr: the octets in memory order, so the first octet is the low byte on this little-endian machine) and as the socket
 * services take and give it (one integer with the first octet in the top byte: network::Ipv4Address::value()). The two differ by a byte swap, made here at the boundary. */
static ULONG kernel_address(ULONG s_addr) { return s_addr >> 24 | (s_addr >> 8 & 0xFF00u) | (s_addr << 8 & 0xFF0000u) | s_addr << 24; }
static ULONG program_address(ULONG value) { return kernel_address(value); }

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
  const NTSTATUS status = NtBindSocket((HANDLE)(UINT_PTR)s, kernel_address(in->sin_addr.S_addr), (ULONG)in->sin_port);
  return NT_SUCCESS(status) ? 0 : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI connect(SOCKET s, const SOCKADDR *name, int namelen) {
  if (!name || (size_t_)namelen < sizeof(SOCKADDR_IN)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  const SOCKADDR_IN *in = (const SOCKADDR_IN *)name;
  const NTSTATUS status = NtConnectSocket((HANDLE)(UINT_PTR)s, kernel_address(in->sin_addr.S_addr), (ULONG)in->sin_port);
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
    in->sin_family = AF_INET_; in->sin_port = (USHORT)peer_port; in->sin_addr.S_addr = program_address(peer_address);
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
      NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, kernel_address(in->sin_addr.S_addr), (ULONG)in->sin_port, TRUE);
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
    in->sin_family = AF_INET_; in->sin_port = (USHORT)from_port; in->sin_addr.S_addr = program_address(from_address);
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
  in->sin_addr.S_addr = (ULONG)raw[0] | (ULONG)raw[1] << 8 | (ULONG)raw[2] << 16 | (ULONG)raw[3] << 24;      /* (the octets in memory order, as in a real in_addr) */
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
 * inet_pton / inet_ntop (IPv4 and IPv6 text), gethostname, and getaddrinfo / getnameinfo for what can be answered without a DNS client: numeric addresses, a NULL node (the
 * wildcard or the loopback), "localhost" and this machine's own name (its interface address, else the loopback). Any other name is WSAHOST_NOT_FOUND. An IPv6 literal resolves
 * to an AF_INET6 entry, though the sockets themselves are IPv4 only. */
IMPORT BOOL WINAPI GetComputerNameA(char *name, DWORD *size);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, const char *source, int source_length, WCHAR *destination, int destination_length);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, const WCHAR *source, int source_length, char *destination, int destination_length, const char *default_char, BOOL *used_default);

#define AF_UNSPEC_ 0
#define AF_INET6_ 23
#define WSAEAFNOSUPPORT_ 10047
#define WSAESOCKTNOSUPPORT_ 10044
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

typedef struct { SHORT sin6_family; USHORT sin6_port; ULONG sin6_flowinfo; UCHAR sin6_addr[16]; ULONG sin6_scope_id; } SOCKADDR_IN6_;
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
/* The canonical form: lower-case hex, the longest run of two or more zero groups as "::", IPv4-mapped and -compatible addresses with a dotted tail. */
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
  if (best_length < 2) best = -1;
  const BOOL v4_tail = best == 0 && (best_length == 6 || (best_length == 5 && g[5] == 0xFFFF));
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
  const size_t_ address_size = family == AF_INET6_ ? sizeof(SOCKADDR_IN6_) : sizeof(SOCKADDR_IN);
  const size_t_ name_chars = canonical ? text_length(canonical) + 1 : 0;
  const size_t_ name_bytes = name_chars * (wide ? sizeof(WCHAR) : 1);
  BYTE *block = HeapAlloc(GetProcessHeap(), 0, sizeof(ADDRINFOA_) + address_size + name_bytes);
  if (!block) return NULL;
  memset(block, 0, sizeof(ADDRINFOA_) + address_size + name_bytes);
  ADDRINFOA_ *ai = (ADDRINFOA_ *)block;
  ai->ai_family = family;
  ai->ai_socktype = hints ? hints->ai_socktype : 0;
  ai->ai_protocol = hints ? hints->ai_protocol : 0;
  ai->ai_addrlen = address_size;
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
    BYTE *name = block + sizeof(ADDRINFOA_) + address_size;
    if (wide) for (size_t_ i = 0; i < name_chars; ++i) ((WCHAR *)name)[i] = (WCHAR)(UCHAR)canonical[i];
    else memcpy(name, canonical, name_chars);
    ai->ai_canonname = (char *)name;
  }
  return ai;
}

static int resolve(const char *node, const char *service, const ADDRINFOA_ *hints, ADDRINFOA_ **result, BOOL wide) {
  if (!result) return WSAEFAULT_;      /* (not a code of its own on Windows either) */
  *result = NULL;
  if (!node && !service) return WSAHOST_NOT_FOUND_;
  const int family = hints ? hints->ai_family : AF_UNSPEC_, flags = hints ? hints->ai_flags : 0;
  if (family != AF_UNSPEC_ && family != AF_INET_ && family != AF_INET6_) return WSAEAFNOSUPPORT_;
  if (hints && (hints->ai_socktype < 0 || hints->ai_socktype > 5)) return WSAESOCKTNOSUPPORT_;
  USHORT port = 0;
  if (!parse_service(service, hints ? hints->ai_socktype : 0, &port)) return WSATYPE_NOT_FOUND_;
  UCHAR address[16];
  int result_family = AF_INET_;
  const char *canonical = node;
  if (!node) {
    if (flags & AI_PASSIVE_) memset(address, 0, sizeof address);
    else { address[0] = 127; address[1] = 0; address[2] = 0; address[3] = 1; }
    if (family == AF_INET6_) { memset(address, 0, 16); if (!(flags & AI_PASSIVE_)) address[15] = 1; result_family = AF_INET6_; }
  } else if (parse_ipv4(node, address)) {
    if (family == AF_INET6_) return WSAHOST_NOT_FOUND_;
  } else if (parse_ipv6(node, address)) {
    if (family == AF_INET_) return WSAHOST_NOT_FOUND_;
    result_family = AF_INET6_;
  } else if (flags & AI_NUMERICHOST_) {
    return WSAHOST_NOT_FOUND_;
  } else if (same_name(node, "localhost")) {
    if (family == AF_INET6_) { memset(address, 0, 16); address[15] = 1; result_family = AF_INET6_; }
    else { address[0] = 127; address[1] = 0; address[2] = 0; address[3] = 1; }
  } else if (is_own_name(node) || !*node) {
    if (family == AF_INET6_) return WSAHOST_NOT_FOUND_;
    own_address(address);
  } else {
    return WSAHOST_NOT_FOUND_;      /* (no DNS client for Windows programs yet) */
  }
  ADDRINFOA_ *ai = new_entry(result_family, address, port, hints, (flags & AI_CANONNAME_) ? (canonical ? canonical : "localhost") : NULL, wide);
  if (!ai) return WSA_NOT_ENOUGH_MEMORY_;
  ai->ai_flags = flags;
  *result = ai;
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
