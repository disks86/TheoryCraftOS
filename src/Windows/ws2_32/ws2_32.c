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

/* Event select (below, with the overlapped calls): the plain calls tell it what they did, so that a network event it already recorded can be recorded again (FD_READ after a
 * recv, FD_ACCEPT after an accept, FD_WRITE after a send that would have blocked), a connect is watched for FD_CONNECT, and a closed socket is forgotten. */
#define FD_READ_ 0x01
#define FD_WRITE_ 0x02
#define FD_OOB_ 0x04
#define FD_ACCEPT_ 0x08
#define FD_CONNECT_ 0x10
#define FD_CLOSE_ 0x20
static volatile LONG g_watched = 0;      /* how many sockets event select knows (the plain calls skip the table while there are none) */
static void select_rearm(SOCKET s, LONG events);
static void select_connecting(SOCKET s);
static void select_forget(SOCKET s);
static void select_inherit(SOCKET listener, SOCKET accepted);

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
  if (g_watched) select_forget(s);
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
  if (NT_SUCCESS(status) || wsa_error_for(status) == WSAEWOULDBLOCK_) select_connecting(s);      /* (FD_CONNECT follows either way, as on Windows) */
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
  if (g_watched) select_rearm(s, FD_ACCEPT_);
  if (!NT_SUCCESS(status)) return fail_nt(status, INVALID_SOCKET);
  if (!give_address(&peer, peer_length, addr, addrlen)) { NtClose(out); return INVALID_SOCKET; }
  if (g_watched) select_inherit(s, (SOCKET)(UINT_PTR)out);
  return (SOCKET)(UINT_PTR)out;
}

EXPORT int WSAAPI send(SOCKET s, const char *buf, int len, int flags) {
  (void)flags;
  ULONG sent = 0;
  const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, NULL, 0);
  if (g_watched && !NT_SUCCESS(status) && wsa_error_for(status) == WSAEWOULDBLOCK_) select_rearm(s, FD_WRITE_);
  return NT_SUCCESS(status) ? (int)sent : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI sendto(SOCKET s, const char *buf, int len, int flags, const SOCKADDR *to, int tolen) {
  (void)flags;
  if (!to) return send(s, buf, len, flags);      /* (sendto with no address is send, as on Windows) */
  if (!check_address(to, tolen)) return SOCKET_ERROR;
  ULONG sent = 0;
  const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, (PVOID)(UINT_PTR)buf, (ULONG)len, &sent, to, (ULONG)address_size(to, tolen));
  if (g_watched && !NT_SUCCESS(status) && wsa_error_for(status) == WSAEWOULDBLOCK_) select_rearm(s, FD_WRITE_);
  return NT_SUCCESS(status) ? (int)sent : (int)fail_nt(status, SOCKET_ERROR);
}
EXPORT int WSAAPI recv(SOCKET s, char *buf, int len, int flags) {
  (void)flags;
  ULONG got = 0;
  const NTSTATUS status = NtReceiveSocket((HANDLE)(UINT_PTR)s, buf, (ULONG)len, &got, NULL, NULL, NULL);
  if (g_watched) select_rearm(s, FD_READ_);
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
  if (g_watched) select_rearm(s, FD_READ_);
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

/* The services take the handles as 32-bit values (a handle fits) and compact each array to the ready ones in place, while an fd_set holds 64-bit SOCKETs: each set goes
 * through an array of ULONGs of its own and comes back as the ready SOCKETs. */
static void pack_set(const fd_set_ *set, ULONG *count, ULONG *handles) {
  *count = 0;
  if (!set) return;
  const UINT n = set->fd_count > 64 ? 64 : set->fd_count;
  for (UINT i = 0; i < n; ++i) handles[(*count)++] = (ULONG)set->fd_array[i];
}
static void unpack_set(fd_set_ *set, ULONG count, const ULONG *handles) {
  if (!set) return;
  for (ULONG i = 0; i < count && i < 64; ++i) set->fd_array[i] = (SOCKET)handles[i];
  set->fd_count = count;
}
EXPORT int WSAAPI select(int nfds, fd_set_ *readfds, fd_set_ *writefds, fd_set_ *exceptfds, const TIMEVAL_ *timeout) {
  (void)nfds;
  LARGE_INTEGER time_limit;
  const PLARGE_INTEGER time_arg = timeout ? &time_limit : NULL;
  if (timeout) time_limit.QuadPart = (LONGLONG)timeout->tv_sec * 10000000LL + (LONGLONG)timeout->tv_usec * 10LL;
  ULONG read_count, write_count, except_count, read_handles[64], write_handles[64], except_handles[64];
  pack_set(readfds, &read_count, read_handles);
  pack_set(writefds, &write_count, write_handles);
  pack_set(exceptfds, &except_count, except_handles);
  ULONG ready = 0;
  const NTSTATUS status = NtSelectSockets(readfds ? &read_count : NULL, readfds ? read_handles : NULL, writefds ? &write_count : NULL, writefds ? write_handles : NULL,
                                          exceptfds ? &except_count : NULL, exceptfds ? except_handles : NULL, time_arg, &ready);
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  /* Each count came back as how many of its handles are ready, the array compacted to them. */
  unpack_set(readfds, read_count, read_handles);
  unpack_set(writefds, write_count, write_handles);
  unpack_set(exceptfds, except_count, except_handles);
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

/* ---- events, event select and the overlapped calls ----------------------------------------------------------------------------------------------------------------------
 * WSA events are kernel32 manual-reset events. WSAEventSelect keeps a record per socket (the event, the FD_* bits asked for, which of them may be recorded again, what is
 * recorded and not yet enumerated, and the error of each) and makes the socket non-blocking, as on Windows. A poller thread of this DLL (started by the first WSAEventSelect)
 * asks NtSelectSockets about every socket that has something left to record - readable, writable, failed - and records and signals what it finds; the conditions are
 * re-armed the way Windows re-enables them (FD_READ by a receive, FD_ACCEPT by an accept, FD_WRITE by a send that would block). The services have no wake-up for anything but
 * the network, so the poller waits at most POLL_100NS in each select and re-reads the table; with nothing to watch it sleeps on an event of its own.
 * Overlapped WSARecv/WSARecvFrom complete at once when data is waiting; otherwise they are left with the poller, which receives into the caller's buffers when the socket
 * becomes readable and completes the OVERLAPPED (Internal: the NTSTATUS, InternalHigh: the bytes), its event and its completion routine (an APC of the thread that asked).
 * Sends never wait (the stack takes the data or fails), so the send calls always complete inline. Not done: completion ports for sockets, WSAAsyncSelect, FD_OOB/QOS. */
IMPORT HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES *attributes, BOOL manual, BOOL initial, const WCHAR *name);
IMPORT BOOL WINAPI SetEvent(HANDLE event);
IMPORT BOOL WINAPI ResetEvent(HANDLE event);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
IMPORT DWORD WINAPI WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds, BOOL alertable);
IMPORT HANDLE WINAPI CreateThread(SECURITY_ATTRIBUTES *attributes, size_t_ stack, LPTHREAD_START_ROUTINE start, PVOID parameter, DWORD flags, DWORD *id);
IMPORT void WINAPI AcquireSRWLockExclusive(SRWLOCK *lock);
IMPORT void WINAPI ReleaseSRWLockExclusive(SRWLOCK *lock);
IMPORT DWORD WINAPI QueueUserAPC(void (WINAPI *function)(ULONG_PTR), HANDLE thread, ULONG_PTR parameter);
IMPORT BOOL WINAPI DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, HANDLE *target, DWORD access, BOOL inherit, DWORD options);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT HANDLE WINAPI GetCurrentThread(void);
IMPORT void WINAPI Sleep(DWORD milliseconds);

#define WSA_IO_PENDING_ 997
#define WSA_IO_INCOMPLETE_ 996
#define WSA_INVALID_HANDLE_ 6
#define WSA_INVALID_PARAMETER_ 87
#define WSA_OPERATION_ABORTED_ 995
#define WSA_WAIT_FAILED_ 0xFFFFFFFFu
#define WSA_MAXIMUM_WAIT_EVENTS_ 64
#define STATUS_PENDING_ 0x00000103
#define STATUS_CANCELLED_ ((NTSTATUS)0xC0000120u)
#define STATUS_BUFFER_TOO_SMALL_ ((NTSTATUS)0xC0000023u)
#define STATUS_INVALID_PARAMETER_ ((NTSTATUS)0xC000000Du)
#define STATUS_NO_MEMORY_ ((NTSTATUS)0xC000009Au)
#define FD_MAX_EVENTS_ 10
#define POLL_100NS 100000      /* 10 ms: how long a change of the table (a new socket, a re-armed event) can wait to be seen */

typedef struct { ULONG len; char *buf; } WSABUF_;
typedef struct { LONG lNetworkEvents; int iErrorCode[FD_MAX_EVENTS_]; } WSANETWORKEVENTS_;
typedef void (WINAPI *COMPLETION_ROUTINE_)(DWORD error, DWORD bytes, OVERLAPPED *overlapped, DWORD flags);

static int bit_index(LONG event) { int i = 0; while (i < FD_MAX_EVENTS_ - 1 && !(event & (1 << i))) ++i; return i; }

typedef struct {
  SOCKET socket;              /* 0: a free record */
  HANDLE event;
  LONG mask, armed, recorded;
  int errors[FD_MAX_EVENTS_];
  BOOL tcp, connecting, closed;      /* closed: FD_CLOSE (or a failed connect) was found; the socket is not read again for it until it is selected again */
} SelectRecord;
/* An overlapped receive that had nothing to receive yet. */
#define MAX_BUFFERS 16
typedef struct {
  SOCKET socket;              /* 0: free */
  OVERLAPPED *overlapped;
  WSABUF_ buffers[MAX_BUFFERS];
  DWORD buffer_count;
  SOCKADDR *from;
  int *from_length;
  COMPLETION_ROUTINE_ routine;
  HANDLE thread;              /* the thread to run the routine in (a handle of our own), or NULL */
} PendingReceive;
#define MAX_RECORDS 512
#define MAX_PENDING 256
static SelectRecord g_records[MAX_RECORDS];
static PendingReceive g_pending[MAX_PENDING];
static SRWLOCK g_select_lock = {0};
static HANDLE g_wake = NULL;                  /* set when the table changed: the poller looks again */
static volatile LONG g_poller_state = 0;      /* 0 not started, 1 starting, 2 running */
static UINT g_rotate = 0;                     /* where the next pass starts (more than 64 sockets are polled in turns) */

static SelectRecord *find_record(SOCKET s) {
  for (UINT i = 0; i < MAX_RECORDS; ++i) if (g_records[i].socket == s) return &g_records[i];
  return NULL;
}
static SelectRecord *new_record(SOCKET s, BOOL tcp) {
  for (UINT i = 0; i < MAX_RECORDS; ++i) {
    if (g_records[i].socket) continue;
    memset(&g_records[i], 0, sizeof g_records[i]);
    g_records[i].socket = s;
    g_records[i].tcp = tcp;
    __atomic_add_fetch(&g_watched, 1, __ATOMIC_RELAXED);
    return &g_records[i];
  }
  return NULL;
}
static void wake_poller(void) { if (g_wake) SetEvent(g_wake); }

static void select_rearm(SOCKET s, LONG events) {
  AcquireSRWLockExclusive(&g_select_lock);
  SelectRecord *r = find_record(s);
  BOOL changed = FALSE;
  if (r && (r->mask & events & ~r->armed)) { r->armed |= r->mask & events; changed = TRUE; }
  ReleaseSRWLockExclusive(&g_select_lock);
  if (changed) wake_poller();
}
static void select_connecting(SOCKET s) {
  SOCKET_INFO_ info;
  if (!socket_info(s, &info) || info.type != SOCK_STREAM_) return;
  AcquireSRWLockExclusive(&g_select_lock);
  SelectRecord *r = find_record(s);
  if (!r) r = new_record(s, TRUE);
  if (r) { r->connecting = TRUE; r->closed = FALSE; }
  ReleaseSRWLockExclusive(&g_select_lock);
  wake_poller();
}

/* An accepted socket has the event selection of its listener (and is non-blocking with it), as on Windows. */
static void select_inherit(SOCKET listener, SOCKET accepted) {
  AcquireSRWLockExclusive(&g_select_lock);
  const SelectRecord *l = find_record(listener);
  const HANDLE event = l ? l->event : NULL;
  const LONG mask = l ? l->mask : 0;
  SelectRecord *r = mask ? new_record(accepted, l->tcp) : NULL;
  if (r) { r->event = event; r->mask = r->armed = mask & ~FD_ACCEPT_; }      /* (FD_ACCEPT means nothing for a connected socket: its readability is data) */
  ReleaseSRWLockExclusive(&g_select_lock);
  if (!mask) return;
  const UCHAR on = 1;
  NtSocketControl((HANDLE)(UINT_PTR)accepted, 1 /*SetNonBlocking*/, (PVOID)&on, 1, NULL, 0);
  wake_poller();
}

static void WINAPI completion_apc(ULONG_PTR parameter);
typedef struct { COMPLETION_ROUTINE_ routine; DWORD error, bytes; OVERLAPPED *overlapped; } CompletionCall;
static DWORD wsa_error_of_status(NTSTATUS status) {
  if (NT_SUCCESS(status)) return 0;
  if (status == STATUS_CANCELLED_) return WSA_OPERATION_ABORTED_;
  if (status == STATUS_BUFFER_TOO_SMALL_) return WSAEFAULT_;
  return (DWORD)wsa_error_for(status);
}
/* Completes an overlapped call: the status block, the event, and the routine (queued to `thread`, which is closed here). */
static void complete_overlapped(OVERLAPPED *ov, NTSTATUS status, DWORD bytes, COMPLETION_ROUTINE_ routine, HANDLE thread) {
  ov->InternalHigh = bytes;
  __atomic_store_n(&ov->Internal, (ULONG_PTR)(ULONG)status, __ATOMIC_RELEASE);
  HANDLE event = (HANDLE)((ULONG_PTR)ov->hEvent & ~(ULONG_PTR)1);      /* (the low bit only says "no completion port") */
  if (event) SetEvent(event);
  if (routine && thread) {
    CompletionCall *call = HeapAlloc(GetProcessHeap(), 0, sizeof *call);
    if (call) {
      call->routine = routine; call->bytes = bytes; call->overlapped = ov; call->error = wsa_error_of_status(status);
      if (!QueueUserAPC(completion_apc, thread, (ULONG_PTR)call)) HeapFree(GetProcessHeap(), 0, call);
    }
  }
  if (thread) CloseHandle(thread);
}
static void WINAPI completion_apc(ULONG_PTR parameter) {
  CompletionCall call = *(CompletionCall *)parameter;
  HeapFree(GetProcessHeap(), 0, (void *)parameter);
  call.routine(call.error, call.bytes, call.overlapped, 0);
}
static HANDLE own_thread_handle(void) {
  HANDLE h = NULL;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */)) return NULL;
  return h;
}

/* Receives into a list of buffers in one receive (a datagram is not split between calls; more than one buffer goes through a block of the heap). */
static NTSTATUS receive_into(SOCKET s, const WSABUF_ *buffers, DWORD count, DWORD *bytes, SOCKADDR *from, int *from_length) {
  ANY_SOCKADDR_ sender;
  ULONG sender_length = sizeof sender, got = 0;
  BOOLEAN closed = FALSE;
  size_t_ total = 0;
  for (DWORD i = 0; i < count; ++i) { if (buffers[i].len > 0x7FFFFFFFu - total) return STATUS_INVALID_PARAMETER_; total += buffers[i].len; }
  char *block = count == 1 ? buffers[0].buf : total ? HeapAlloc(GetProcessHeap(), 0, total) : NULL;
  if (count != 1 && total && !block) return STATUS_NO_MEMORY_;
  const NTSTATUS status = NtReceiveSocket((HANDLE)(UINT_PTR)s, block, (ULONG)total, &got, from ? &sender : NULL, from ? &sender_length : NULL, &closed);
  if (NT_SUCCESS(status) && count != 1) {
    size_t_ done = 0;
    for (DWORD i = 0; i < count && done < got; ++i) {
      const size_t_ n = got - done < buffers[i].len ? got - done : buffers[i].len;
      memcpy(buffers[i].buf, block + done, n);
      done += n;
    }
  }
  if (count != 1 && block) HeapFree(GetProcessHeap(), 0, block);
  if (g_watched) select_rearm(s, FD_READ_);
  if (!NT_SUCCESS(status)) return status;
  if (from && from_length && !closed) {
    if (*from_length < (int)sender_length) return STATUS_BUFFER_TOO_SMALL_;
    memcpy(from, &sender, sender_length);
    *from_length = (int)sender_length;
  }
  *bytes = got;
  return 0;
}

/* Whether a receive would not block now (a zero-timeout select of the one socket). */
static BOOL readable_now(SOCKET s) {
  ULONG count = 1, handle = (ULONG)s, ready = 0;
  LARGE_INTEGER zero; zero.QuadPart = 0;
  return NT_SUCCESS(NtSelectSockets(&count, &handle, NULL, NULL, NULL, NULL, &zero, &ready)) && ready > 0;
}

static void record_event(SelectRecord *r, LONG event, int error, BOOL *signal) {
  if (!(r->mask & r->armed & event)) return;
  r->armed &= ~event;
  r->recorded |= event;
  r->errors[bit_index(event)] = error;
  *signal = TRUE;
}
static BOOL is_connected(SOCKET s) {
  ANY_SOCKADDR_ peer;
  return NT_SUCCESS(NtSocketControl((HANDLE)(UINT_PTR)s, 3 /*GetPeerName*/, NULL, 0, &peer, sizeof peer));
}
/* What a socket the services found readable / writable / failed means for its record; TRUE if the record changed. */
static BOOL evaluate(SelectRecord *r, BOOL readable, BOOL writable, BOOL failed) {
  BOOL signal = FALSE;
  const LONG before_armed = r->armed;
  const BOOL before_closed = r->closed, before_connecting = r->connecting;
  if (r->connecting && (writable || failed)) {
    const BOOL connected = !failed && is_connected(r->socket);
    if (connected || failed) {
      r->connecting = FALSE;
      record_event(r, FD_CONNECT_, failed ? WSAECONNREFUSED_ : 0, &signal);
      if (failed) r->closed = TRUE;      /* (a connection that never was is not also "closed") */
    }
  }
  if (!r->connecting && !r->closed) {
    if (readable || failed) {
      if (r->mask & FD_ACCEPT_) record_event(r, FD_ACCEPT_, 0, &signal);
      else if (!r->tcp) record_event(r, FD_READ_, 0, &signal);
      else {
        ULONG available = 0;
        NtSocketControl((HANDLE)(UINT_PTR)r->socket, 4 /*BytesReadable*/, NULL, 0, &available, sizeof available);
        if (available > 0) record_event(r, FD_READ_, 0, &signal);
        else {      /* readable with nothing to read: the peer closed (or the connection failed). FD_CLOSE is recorded once, whatever was enumerated before. */
          r->closed = TRUE;
          r->armed |= r->mask & FD_CLOSE_;
          record_event(r, FD_CLOSE_, failed ? WSAECONNRESET_ : 0, &signal);
        }
      }
    }
    if (writable && (r->armed & FD_WRITE_) && (!r->tcp || is_connected(r->socket))) record_event(r, FD_WRITE_, 0, &signal);
  }
  if (signal && r->event) SetEvent(r->event);
  return signal || r->armed != before_armed || r->closed != before_closed || r->connecting != before_connecting;
}

static BOOL in_list(ULONG handle, const ULONG *list, ULONG count) { for (ULONG i = 0; i < count; ++i) if (list[i] == handle) return TRUE; return FALSE; }
static void add_handle(ULONG handle, ULONG *list, ULONG *count) {
  if (*count < 64 && !in_list(handle, list, *count)) list[(*count)++] = handle;
}

/* One pass: which sockets have something to record or a receive waiting, a select of them (at most `timeout` long), and what it found. `only`: just that socket (0: all).
 * Returns -1 if there was nothing to ask about, 0 if sockets were ready but nothing changed, 1 otherwise. */
static int poll_pass(SOCKET only, LONGLONG timeout) {
  ULONG reads[64], writes[64], excepts[64], read_count = 0, write_count = 0, except_count = 0;
  AcquireSRWLockExclusive(&g_select_lock);
  const UINT start = only ? 0 : g_rotate++ % MAX_RECORDS;
  for (UINT k = 0; k < MAX_RECORDS; ++k) {
    const SelectRecord *r = &g_records[(start + k) % MAX_RECORDS];
    if (!r->socket || (only && r->socket != only)) continue;
    const ULONG h = (ULONG)r->socket;
    const BOOL open = !r->connecting && !r->closed;
    /* (a listener is read for FD_ACCEPT only: its readability is the waiting connection, which stays until it is accepted) */
    const BOOL want_read = open && (r->armed & ((r->mask & FD_ACCEPT_) ? FD_ACCEPT_ : (FD_READ_ | FD_CLOSE_)));
    /* (a connect still under way is followed for any selection, so that the record knows when the socket can be read) */
    const BOOL want_write = (r->connecting && r->mask) || (open && (r->armed & FD_WRITE_));
    if (want_read) add_handle(h, reads, &read_count);
    if (want_write) add_handle(h, writes, &write_count);
    if (r->tcp && (want_read || want_write)) add_handle(h, excepts, &except_count);      /* (a failure is only news for a socket watched for something) */
  }
  for (UINT i = 0; i < MAX_PENDING; ++i) if (g_pending[i].socket && (!only || g_pending[i].socket == only)) add_handle((ULONG)g_pending[i].socket, reads, &read_count);
  ReleaseSRWLockExclusive(&g_select_lock);
  if (!read_count && !write_count && !except_count) return -1;
  ULONG ready = 0;
  LARGE_INTEGER limit; limit.QuadPart = timeout;
  const NTSTATUS status = NtSelectSockets(&read_count, reads, &write_count, writes, &except_count, excepts, &limit, &ready);
  if (!NT_SUCCESS(status)) return 0;
  if (ready == 0) return 1;      /* (the time is up: nothing to wait for before the next pass) */
  int changed = 0;
  PendingReceive done[32];      /* (at most this many receives a pass: the rest go in the next one) */
  UINT done_count = 0;
  AcquireSRWLockExclusive(&g_select_lock);
  for (UINT i = 0; i < MAX_RECORDS; ++i) {
    SelectRecord *r = &g_records[i];
    if (!r->socket || (only && r->socket != only)) continue;
    const ULONG h = (ULONG)r->socket;
    const BOOL readable = in_list(h, reads, read_count), writable = in_list(h, writes, write_count), failed = in_list(h, excepts, except_count);
    if ((readable || writable || failed) && evaluate(r, readable, writable, failed)) changed = 1;
  }
  /* The receives that can go now leave the table (receive_into takes the lock itself); one that finds nothing after all goes back. */
  for (UINT i = 0; i < MAX_PENDING; ++i) {
    PendingReceive *p = &g_pending[i];
    if (!p->socket || (only && p->socket != only) || !in_list((ULONG)p->socket, reads, read_count) || done_count == 32) continue;
    done[done_count++] = *p;
    p->socket = 0;
  }
  ReleaseSRWLockExclusive(&g_select_lock);
  for (UINT i = 0; i < done_count; ++i) {
    PendingReceive *p = &done[i];
    DWORD bytes = 0;
    const NTSTATUS result = receive_into(p->socket, p->buffers, p->buffer_count, &bytes, p->from, p->from_length);
    if (!NT_SUCCESS(result) && wsa_error_for(result) == WSAEWOULDBLOCK_) {      /* (another receive took the data first) */
      AcquireSRWLockExclusive(&g_select_lock);
      UINT slot = 0;
      while (slot < MAX_PENDING && g_pending[slot].socket) ++slot;
      if (slot < MAX_PENDING) g_pending[slot] = *p;
      ReleaseSRWLockExclusive(&g_select_lock);
      if (slot < MAX_PENDING) continue;
    }
    complete_overlapped(p->overlapped, result, bytes, p->routine, p->thread);
    changed = 1;
  }
  return changed;
}

static DWORD WINAPI poller_thread(PVOID unused) {
  (void)unused;
  for (;;) {
    const int result = poll_pass(0, POLL_100NS);
    /* (a socket that is ready without news - a fresh TCP socket reads as writable - must not spin the pass: then wait a little, or for a change of the table) */
    if (result < 0) WaitForSingleObject(g_wake, INFINITE);
    else if (result == 0) WaitForSingleObject(g_wake, 5);
    ResetEvent(g_wake);
  }
  return 0;
}
static BOOL start_poller(void) {
  for (;;) {
    LONG expected = 0;
    if (__atomic_compare_exchange_n(&g_poller_state, &expected, 1, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    if (expected == 2) return TRUE;
    Sleep(1);      /* (another thread is starting it) */
  }
  g_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
  HANDLE thread = g_wake ? CreateThread(NULL, 0, poller_thread, NULL, 0, NULL) : NULL;
  if (!thread) {
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    __atomic_store_n(&g_poller_state, 0, __ATOMIC_RELEASE);
    return FALSE;
  }
  CloseHandle(thread);
  __atomic_store_n(&g_poller_state, 2, __ATOMIC_RELEASE);
  return TRUE;
}

/* A socket being closed: its record goes, and its waiting receives complete as cancelled (WSA_OPERATION_ABORTED). */
static void select_forget(SOCKET s) {
  AcquireSRWLockExclusive(&g_select_lock);
  SelectRecord *r = find_record(s);
  if (r) { r->socket = 0; __atomic_sub_fetch(&g_watched, 1, __ATOMIC_RELAXED); }
  ReleaseSRWLockExclusive(&g_select_lock);
  for (;;) {
    PendingReceive cancelled[16];
    UINT n = 0;
    AcquireSRWLockExclusive(&g_select_lock);
    for (UINT i = 0; i < MAX_PENDING && n < 16; ++i) if (g_pending[i].socket == s) { cancelled[n++] = g_pending[i]; g_pending[i].socket = 0; }
    ReleaseSRWLockExclusive(&g_select_lock);
    if (!n) return;
    for (UINT i = 0; i < n; ++i) complete_overlapped(cancelled[i].overlapped, STATUS_CANCELLED_, 0, cancelled[i].routine, cancelled[i].thread);
  }
}

EXPORT HANDLE WSAAPI WSACreateEvent(void) {
  HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!event) set_wsa_error(WSA_NOT_ENOUGH_MEMORY_);
  return event;
}
EXPORT BOOL WSAAPI WSACloseEvent(HANDLE event) { if (!CloseHandle(event)) { set_wsa_error(WSA_INVALID_HANDLE_); return FALSE; } return TRUE; }
EXPORT BOOL WSAAPI WSASetEvent(HANDLE event) { if (!SetEvent(event)) { set_wsa_error(WSA_INVALID_HANDLE_); return FALSE; } return TRUE; }
EXPORT BOOL WSAAPI WSAResetEvent(HANDLE event) { if (!ResetEvent(event)) { set_wsa_error(WSA_INVALID_HANDLE_); return FALSE; } return TRUE; }
/* The WSA_WAIT_* values are the WAIT_* ones (WSA_WAIT_EVENT_0 = WAIT_OBJECT_0, WSA_WAIT_TIMEOUT = WAIT_TIMEOUT, WSA_WAIT_IO_COMPLETION = WAIT_IO_COMPLETION). */
EXPORT DWORD WSAAPI WSAWaitForMultipleEvents(DWORD count, const HANDLE *events, BOOL wait_all, DWORD timeout, BOOL alertable) {
  if (!count || count > WSA_MAXIMUM_WAIT_EVENTS_ || !events) { set_wsa_error(WSA_INVALID_PARAMETER_); return WSA_WAIT_FAILED_; }
  const DWORD result = WaitForMultipleObjectsEx(count, events, wait_all, timeout, alertable);
  if (result == WSA_WAIT_FAILED_) set_wsa_error(WSA_INVALID_HANDLE_);
  return result;
}

EXPORT int WSAAPI WSAEventSelect(SOCKET s, HANDLE event, LONG events) {
  SOCKET_INFO_ info;
  if (!require_started() || !socket_info(s, &info)) return SOCKET_ERROR;
  if (events && !event) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  const UCHAR on = 1;      /* (WSAEventSelect makes the socket non-blocking, and it stays so) */
  const NTSTATUS status = NtSocketControl((HANDLE)(UINT_PTR)s, 1 /*SetNonBlocking*/, (PVOID)&on, 1, NULL, 0);
  if (!NT_SUCCESS(status)) return (int)fail_nt(status, SOCKET_ERROR);
  if (events && !start_poller()) { set_wsa_error(WSAENOBUFS_); return SOCKET_ERROR; }
  AcquireSRWLockExclusive(&g_select_lock);
  SelectRecord *r = find_record(s);
  if (!r && events) r = new_record(s, info.type == SOCK_STREAM_);
  if (!r && events) { ReleaseSRWLockExclusive(&g_select_lock); set_wsa_error(WSAENOBUFS_); return SOCKET_ERROR; }
  if (r) {
    /* A new selection clears what was recorded, and every condition is looked at afresh (an FD_CLOSE that was found before is found again). */
    r->event = events ? event : NULL;
    r->mask = events & (FD_READ_ | FD_WRITE_ | FD_ACCEPT_ | FD_CONNECT_ | FD_CLOSE_);
    r->armed = r->mask;
    r->recorded = 0;
    memset(r->errors, 0, sizeof r->errors);
    if (!r->connecting) r->closed = FALSE;
  }
  ReleaseSRWLockExclusive(&g_select_lock);
  wake_poller();
  return 0;
}

EXPORT int WSAAPI WSAEnumNetworkEvents(SOCKET s, HANDLE event, WSANETWORKEVENTS_ *out) {
  SOCKET_INFO_ info;
  if (!socket_info(s, &info)) return SOCKET_ERROR;
  if (!out) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  if (g_watched) poll_pass(s, 0);      /* (what is true right now, without waiting for the poller) */
  memset(out, 0, sizeof *out);
  AcquireSRWLockExclusive(&g_select_lock);
  SelectRecord *r = g_watched ? find_record(s) : NULL;
  if (r) {
    out->lNetworkEvents = r->recorded;
    for (int i = 0; i < FD_MAX_EVENTS_; ++i) if (r->recorded & (1 << i)) out->iErrorCode[i] = r->errors[i];
    r->recorded = 0;
    memset(r->errors, 0, sizeof r->errors);
  }
  if (event) ResetEvent(event);
  ReleaseSRWLockExclusive(&g_select_lock);
  return 0;
}

/* Sends a list of buffers: a stream socket sends them in turn, a datagram one as one datagram (gathered in a block of the heap). */
static NTSTATUS send_from(SOCKET s, const WSABUF_ *buffers, DWORD count, DWORD *bytes, const SOCKADDR *to, int to_length) {
  SOCKET_INFO_ info;
  *bytes = 0;
  const ULONG to_size = to ? (ULONG)address_size(to, to_length) : 0;
  if (count == 1 || (socket_info(s, &info) && info.type == SOCK_STREAM_)) {
    for (DWORD i = 0; i < count; ++i) {
      ULONG sent = 0;
      const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, buffers[i].buf, buffers[i].len, &sent, to, to_size);
      if (!NT_SUCCESS(status)) {
        if (g_watched && wsa_error_for(status) == WSAEWOULDBLOCK_) select_rearm(s, FD_WRITE_);
        return *bytes ? 0 : status;      /* (what went is reported; the error comes with the next call) */
      }
      *bytes += sent;
      if (sent < buffers[i].len) break;
    }
    return 0;
  }
  size_t_ total = 0;
  for (DWORD i = 0; i < count; ++i) { if (buffers[i].len > 0x7FFFFFFFu - total) return STATUS_INVALID_PARAMETER_; total += buffers[i].len; }
  char *block = HeapAlloc(GetProcessHeap(), 0, total ? total : 1);
  if (!block) return STATUS_NO_MEMORY_;
  size_t_ at = 0;
  for (DWORD i = 0; i < count; ++i) { memcpy(block + at, buffers[i].buf, buffers[i].len); at += buffers[i].len; }
  ULONG sent = 0;
  const NTSTATUS status = NtSendSocket((HANDLE)(UINT_PTR)s, block, (ULONG)total, &sent, to, to_size);
  HeapFree(GetProcessHeap(), 0, block);
  if (!NT_SUCCESS(status)) return status;
  *bytes = sent;
  return 0;
}

/* The end of a call that is done, overlapped or not: the OVERLAPPED (and the routine) of an overlapped one, the result for both. */
static int finish_call(NTSTATUS status, DWORD bytes, DWORD *bytes_out, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  if (ov) complete_overlapped(ov, status, bytes, routine, routine ? own_thread_handle() : NULL);
  if (!NT_SUCCESS(status)) { set_wsa_error((int)wsa_error_of_status(status)); return SOCKET_ERROR; }
  if (bytes_out) *bytes_out = bytes;
  return 0;
}

static int receive_call(SOCKET s, WSABUF_ *buffers, DWORD count, DWORD *bytes_out, DWORD *flags, SOCKADDR *from, int *from_length, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  if (!buffers || !count) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  if ((from && !from_length) || (!ov && !bytes_out)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  if (flags) *flags = 0;
  for (int attempt = 0; ov && attempt < 4; ++attempt) {
    if (!readable_now(s)) {
      /* Nothing to receive yet: the poller receives it when it comes. */
      SOCKET_INFO_ info;
      if (!socket_info(s, &info)) return SOCKET_ERROR;
      if (count > MAX_BUFFERS || !start_poller()) { set_wsa_error(WSAENOBUFS_); return SOCKET_ERROR; }
      HANDLE thread = routine ? own_thread_handle() : NULL;
      ov->Internal = STATUS_PENDING_;
      ov->InternalHigh = 0;
      AcquireSRWLockExclusive(&g_select_lock);
      PendingReceive *p = NULL;
      for (UINT i = 0; i < MAX_PENDING && !p; ++i) if (!g_pending[i].socket) p = &g_pending[i];
      if (p) {
        p->socket = s; p->overlapped = ov; p->buffer_count = count; p->from = from; p->from_length = from_length; p->routine = routine; p->thread = thread;
        for (DWORD i = 0; i < count; ++i) p->buffers[i] = buffers[i];
      }
      ReleaseSRWLockExclusive(&g_select_lock);
      if (!p) { if (thread) CloseHandle(thread); ov->Internal = 0; set_wsa_error(WSAENOBUFS_); return SOCKET_ERROR; }
      wake_poller();
      set_wsa_error(WSA_IO_PENDING_);
      return SOCKET_ERROR;
    }
    DWORD bytes = 0;
    const NTSTATUS status = receive_into(s, buffers, count, &bytes, from, from_length);
    if (!NT_SUCCESS(status) && wsa_error_for(status) == WSAEWOULDBLOCK_) continue;      /* (the data went to another receive in the meantime: wait for more) */
    return finish_call(status, bytes, bytes_out, ov, routine);
  }
  DWORD bytes = 0;
  const NTSTATUS status = receive_into(s, buffers, count, &bytes, from, from_length);
  return finish_call(status, bytes, bytes_out, ov, routine);
}
EXPORT int WSAAPI WSARecv(SOCKET s, WSABUF_ *buffers, DWORD count, DWORD *bytes, DWORD *flags, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  return receive_call(s, buffers, count, bytes, flags, NULL, NULL, ov, routine);
}
EXPORT int WSAAPI WSARecvFrom(SOCKET s, WSABUF_ *buffers, DWORD count, DWORD *bytes, DWORD *flags, SOCKADDR *from, int *from_length, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  return receive_call(s, buffers, count, bytes, flags, from, from_length, ov, routine);
}
EXPORT int WSAAPI WSASend(SOCKET s, WSABUF_ *buffers, DWORD count, DWORD *bytes_out, DWORD flags, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  (void)flags;
  if (!buffers || !count) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  DWORD bytes = 0;
  const NTSTATUS status = send_from(s, buffers, count, &bytes, NULL, 0);
  return finish_call(status, bytes, bytes_out, ov, routine);
}
EXPORT int WSAAPI WSASendTo(SOCKET s, WSABUF_ *buffers, DWORD count, DWORD *bytes_out, DWORD flags, const SOCKADDR *to, int to_length, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  (void)flags;
  if (!buffers || !count) { set_wsa_error(WSAEINVAL_); return SOCKET_ERROR; }
  if (to && !check_address(to, to_length)) return SOCKET_ERROR;
  DWORD bytes = 0;
  const NTSTATUS status = send_from(s, buffers, count, &bytes, to, to_length);
  return finish_call(status, bytes, bytes_out, ov, routine);
}

EXPORT BOOL WSAAPI WSAGetOverlappedResult(SOCKET s, OVERLAPPED *ov, DWORD *bytes, BOOL wait, DWORD *flags) {
  (void)s;
  if (!ov || !bytes || !flags) { set_wsa_error(WSAEFAULT_); return FALSE; }
  if (__atomic_load_n(&ov->Internal, __ATOMIC_ACQUIRE) == STATUS_PENDING_) {
    HANDLE event = (HANDLE)((ULONG_PTR)ov->hEvent & ~(ULONG_PTR)1);
    if (!wait || !event) { set_wsa_error(WSA_IO_INCOMPLETE_); return FALSE; }
    while (__atomic_load_n(&ov->Internal, __ATOMIC_ACQUIRE) == STATUS_PENDING_) WaitForSingleObject(event, INFINITE);      /* (the event may be another call's too) */
  }
  *bytes = (DWORD)ov->InternalHigh;
  *flags = 0;
  const NTSTATUS status = (NTSTATUS)(ULONG)ov->Internal;
  if (NT_SUCCESS(status)) return TRUE;
  set_wsa_error((int)wsa_error_of_status(status));
  return FALSE;
}

/* ---- WSAIoctl ---------------------------------------------------------------------------------------------------------------------------------------------------------
 * FIONBIO / FIONREAD as ioctlsocket; the keep-alive and UDP reset settings are accepted (there are no keep-alives, and an ICMP port-unreachable never fails a UDP receive here);
 * the base handle of a socket is itself (there are no layered providers). No extension functions (AcceptEx, ConnectEx, ...: mswsock.dll has only stubs of them): WSAEINVAL,
 * which callers take for "use the plain calls". Anything else is WSAEOPNOTSUPP. */
#define SIO_GET_EXTENSION_FUNCTION_POINTER_ 0xC8000006u
#define SIO_KEEPALIVE_VALS_ 0x98000004u
#define SIO_UDP_CONNRESET_ 0x9800000Cu
#define SIO_UDP_NETRESET_ 0x9800000Fu
#define SIO_BASE_HANDLE_ 0x48000022u
#define SIO_BSP_HANDLE_ 0x4800001Bu
#define SIO_BSP_HANDLE_SELECT_ 0x4800001Cu
#define SIO_BSP_HANDLE_POLL_ 0x4800001Du
EXPORT int WSAAPI WSAIoctl(SOCKET s, DWORD code, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *returned, OVERLAPPED *ov, COMPLETION_ROUTINE_ routine) {
  SOCKET_INFO_ info;
  if (!socket_info(s, &info)) return SOCKET_ERROR;
  DWORD bytes = 0;
  switch (code) {
    case FIONBIO_: {
      if (!in || in_size < sizeof(ULONG)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
      u_long_ value; memcpy(&value, in, sizeof value);
      if (ioctlsocket(s, (LONG)FIONBIO_, &value) != 0) return SOCKET_ERROR;
      break;
    }
    case FIONREAD_: {
      if (!out || out_size < sizeof(ULONG)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
      u_long_ value = 0;
      if (ioctlsocket(s, (LONG)FIONREAD_, &value) != 0) return SOCKET_ERROR;
      memcpy(out, &value, sizeof value);
      bytes = sizeof value;
      break;
    }
    case SIO_KEEPALIVE_VALS_:
      if (!in || in_size < 12) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }      /* (struct tcp_keepalive: onoff, time, interval) */
      break;
    case SIO_UDP_CONNRESET_: case SIO_UDP_NETRESET_:
      if (!in || in_size < sizeof(BOOL)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
      break;
    case SIO_BASE_HANDLE_: case SIO_BSP_HANDLE_: case SIO_BSP_HANDLE_SELECT_: case SIO_BSP_HANDLE_POLL_:
      if (!out || out_size < sizeof(SOCKET)) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
      memcpy(out, &s, sizeof s);
      bytes = sizeof s;
      break;
    case SIO_GET_EXTENSION_FUNCTION_POINTER_:
      if (out && out_size >= sizeof(void *)) memset(out, 0, sizeof(void *));
      set_wsa_error(WSAEINVAL_);
      return SOCKET_ERROR;
    default:
      set_wsa_error(WSAEOPNOTSUPP_);
      return SOCKET_ERROR;
  }
  if (returned) *returned = bytes;
  if (ov) complete_overlapped(ov, 0, bytes, routine, routine ? own_thread_handle() : NULL);
  return 0;
}

/* ---- the protocol catalogue -------------------------------------------------------------------------------------------------------------------------------------------
 * Four entries, like the base MSAFD providers of a fresh Windows: TCP and UDP over IPv4 and IPv6 (no layered providers). */
typedef struct { ULONG Data1; USHORT Data2, Data3; UCHAR Data4[8]; } GUID_;
typedef struct {
  DWORD dwServiceFlags1, dwServiceFlags2, dwServiceFlags3, dwServiceFlags4, dwProviderFlags;
  GUID_ ProviderId;
  DWORD dwCatalogEntryId;
  struct { int ChainLen; DWORD ChainEntries[7]; } ProtocolChain;
  int iVersion, iAddressFamily, iMaxSockAddr, iMinSockAddr, iSocketType, iProtocol, iProtocolMaxOffset, iNetworkByteOrder, iSecurityScheme;
  DWORD dwMessageSize, dwProviderReserved;
  WCHAR szProtocol[256];
} WSAPROTOCOL_INFOW_;
_Static_assert(sizeof(WSAPROTOCOL_INFOW_) == 628, "WSAPROTOCOL_INFOW is 628 bytes");
static const GUID_ kTcpipProvider = {0xE70F1AA0, 0xAB8B, 0x11CF, {0x8C, 0xA3, 0x00, 0x80, 0x5F, 0x48, 0xA1, 0x92}};
static const GUID_ kTcpip6Provider = {0xF9EAB0C0, 0x26D4, 0x11D0, {0xBB, 0xBF, 0x00, 0xAA, 0x00, 0x6C, 0x34, 0xE4}};
static const struct { int family, type, protocol; const char *name; } kProtocols[] = {
  {AF_INET_, SOCK_STREAM_, IPPROTO_TCP_, "MSAFD Tcpip [TCP/IP]"},
  {AF_INET_, SOCK_DGRAM_, IPPROTO_UDP_, "MSAFD Tcpip [UDP/IP]"},
  {AF_INET6_, SOCK_STREAM_, IPPROTO_TCP_, "MSAFD Tcpip [TCP/IPv6]"},
  {AF_INET6_, SOCK_DGRAM_, IPPROTO_UDP_, "MSAFD Tcpip [UDP/IPv6]"},
};
#define PROTOCOL_COUNT (sizeof kProtocols / sizeof kProtocols[0])
static BOOL same_guid(const GUID_ *a, const GUID_ *b) {
  if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return FALSE;
  for (int i = 0; i < 8; ++i) if (a->Data4[i] != b->Data4[i]) return FALSE;
  return TRUE;
}
static void fill_protocol(WSAPROTOCOL_INFOW_ *p, UINT index) {
  memset(p, 0, sizeof *p);
  const BOOL stream = kProtocols[index].type == SOCK_STREAM_, v6 = kProtocols[index].family == AF_INET6_;
  p->dwServiceFlags1 = stream ? 0x00020066u /* GUARANTEED_DELIVERY|GUARANTEED_ORDER|GRACEFUL_CLOSE|EXPEDITED_DATA|IFS_HANDLES */
                              : 0x00020609u /* CONNECTIONLESS|MESSAGE_ORIENTED|SUPPORT_BROADCAST|SUPPORT_MULTIPOINT|IFS_HANDLES */;
  p->dwProviderFlags = 0x8;      /* PFL_MATCHES_PROTOCOL_ZERO */
  p->ProviderId = v6 ? kTcpip6Provider : kTcpipProvider;
  p->dwCatalogEntryId = 1001 + index;
  p->ProtocolChain.ChainLen = 1;      /* BASE_PROTOCOL */
  p->iVersion = 2;
  p->iAddressFamily = kProtocols[index].family;
  p->iMaxSockAddr = p->iMinSockAddr = v6 ? (int)sizeof(SOCKADDR_IN6_) : (int)sizeof(SOCKADDR_IN);
  p->iSocketType = kProtocols[index].type;
  p->iProtocol = kProtocols[index].protocol;
  p->dwMessageSize = stream ? 0 : 65467;
  for (int i = 0; kProtocols[index].name[i] && i < 255; ++i) p->szProtocol[i] = (WCHAR)kProtocols[index].name[i];
}
/* The entries for the protocol numbers in `wanted` (0-terminated; NULL: all): the count, or SOCKET_ERROR with the error (WSAENOBUFS and the size needed for a short buffer). */
static int enum_protocols(const int *wanted, WSAPROTOCOL_INFOW_ *buffer, DWORD *length, int *error) {
  if (!length) { *error = WSAEFAULT_; return SOCKET_ERROR; }
  UINT chosen[PROTOCOL_COUNT], n = 0;
  for (UINT i = 0; i < PROTOCOL_COUNT; ++i) {
    BOOL take = !wanted;
    for (const int *w = wanted; w && *w && !take; ++w) take = *w == kProtocols[i].protocol;
    if (take) chosen[n++] = i;
  }
  const DWORD need = n * (DWORD)sizeof(WSAPROTOCOL_INFOW_);
  if (!buffer || *length < need) { *length = need; *error = WSAENOBUFS_; return SOCKET_ERROR; }
  for (UINT i = 0; i < n; ++i) fill_protocol(&buffer[i], chosen[i]);
  return (int)n;
}
EXPORT int WSAAPI WSAEnumProtocolsW(int *protocols, WSAPROTOCOL_INFOW_ *buffer, DWORD *length) {
  int error = 0;
  const int n = enum_protocols(protocols, buffer, length, &error);
  if (n < 0) set_wsa_error(error);
  return n;
}
EXPORT int WSAAPI WSCEnumProtocols(int *protocols, WSAPROTOCOL_INFOW_ *buffer, DWORD *length, int *error) {
  int e = 0;
  const int n = enum_protocols(protocols, buffer, length, &e);
  if (n < 0 && error) *error = e;
  return n;
}
EXPORT int WSAAPI WSCGetProviderPath(GUID_ *provider, WCHAR *path, int *length, int *error) {
  static const char kPath[] = "%SystemRoot%\\system32\\mswsock.dll";
  int e = 0;
  if (!provider || !path || !length) e = WSAEFAULT_;
  else if (!same_guid(provider, &kTcpipProvider) && !same_guid(provider, &kTcpip6Provider)) e = WSAEINVAL_;
  else if (*length < (int)sizeof kPath) { *length = (int)sizeof kPath; e = WSAEFAULT_; }
  if (e) { if (error) *error = e; return SOCKET_ERROR; }
  for (int i = 0; i < (int)sizeof kPath; ++i) path[i] = (WCHAR)kPath[i];
  *length = (int)sizeof kPath - 1;
  return 0;
}
/* No name space providers are installed (names go to the kernel's resolver directly): an empty list. */
EXPORT int WSAAPI WSAEnumNameSpaceProvidersW(DWORD *length, void *buffer) {
  (void)buffer;
  if (!length) { set_wsa_error(WSAEFAULT_); return SOCKET_ERROR; }
  return 0;
}
EXPORT int WSAAPI WSASetServiceW(void *registration, int operation, DWORD flags) {
  (void)registration; (void)operation; (void)flags;
  set_wsa_error(WSAEOPNOTSUPP_);
  return SOCKET_ERROR;
}
/* A socket is a handle of this process's table; there is no way to hand it to another process. */
EXPORT int WSAAPI WSADuplicateSocketW(SOCKET s, DWORD process, WSAPROTOCOL_INFOW_ *info) {
  (void)s; (void)process;
  if (info) memset(info, 0, sizeof *info);
  set_wsa_error(WSAEOPNOTSUPP_);
  return SOCKET_ERROR;
}

/* ---- GetAddrInfoExW ---------------------------------------------------------------------------------------------------------------------------------------------------
 * GetAddrInfoW's answer in ADDRINFOEXW entries (one heap block each, like the others). Asked with an OVERLAPPED, the lookup runs in a thread of its own: the call returns
 * WSA_IO_PENDING, and the result, the OVERLAPPED's Internal (the error), its event and the completion routine (called in that thread, as Windows calls it outside the caller)
 * follow when it is done. The timeout is not used (the resolver has its own); a lookup cannot be cancelled. */
typedef struct addrinfoexW_ {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  size_t_ ai_addrlen;
  WCHAR *ai_canonname;
  SOCKADDR *ai_addr;
  void *ai_blob;
  size_t_ ai_bloblen;
  GUID_ *ai_provider;
  struct addrinfoexW_ *ai_next;
} ADDRINFOEXW_;
typedef void (WINAPI *LOOKUP_ROUTINE_)(DWORD error, DWORD bytes, OVERLAPPED *overlapped);

EXPORT void WSAAPI FreeAddrInfoExW(ADDRINFOEXW_ *ai) {
  while (ai) { ADDRINFOEXW_ *next = ai->ai_next; HeapFree(GetProcessHeap(), 0, ai); ai = next; }
}
static size_t_ wide_length(const WCHAR *s) { size_t_ n = 0; while (s[n]) ++n; return n; }
static int lookup_ex(const WCHAR *name, const WCHAR *service, const ADDRINFOEXW_ *hints, ADDRINFOEXW_ **result) {
  ADDRINFOW_ plain_hints, *found = NULL;
  memset(&plain_hints, 0, sizeof plain_hints);
  if (hints) { plain_hints.ai_flags = hints->ai_flags; plain_hints.ai_family = hints->ai_family; plain_hints.ai_socktype = hints->ai_socktype; plain_hints.ai_protocol = hints->ai_protocol; }
  const int error = GetAddrInfoW(name, service, hints ? &plain_hints : NULL, &found);
  if (error) return error;
  ADDRINFOEXW_ *head = NULL, **tail = &head;
  for (const ADDRINFOW_ *ai = found; ai; ai = ai->ai_next) {
    const size_t_ name_bytes = ai->ai_canonname ? (wide_length(ai->ai_canonname) + 1) * sizeof(WCHAR) : 0;
    BYTE *block = HeapAlloc(GetProcessHeap(), 0, sizeof(ADDRINFOEXW_) + ai->ai_addrlen + name_bytes);
    if (!block) { FreeAddrInfoExW(head); FreeAddrInfoW(found); return WSA_NOT_ENOUGH_MEMORY_; }
    ADDRINFOEXW_ *e = (ADDRINFOEXW_ *)block;
    memset(e, 0, sizeof *e);
    e->ai_flags = ai->ai_flags; e->ai_family = ai->ai_family; e->ai_socktype = ai->ai_socktype; e->ai_protocol = ai->ai_protocol;
    e->ai_addrlen = ai->ai_addrlen;
    e->ai_addr = (SOCKADDR *)(block + sizeof(ADDRINFOEXW_));
    memcpy(e->ai_addr, ai->ai_addr, ai->ai_addrlen);
    if (name_bytes) { e->ai_canonname = (WCHAR *)(block + sizeof(ADDRINFOEXW_) + ai->ai_addrlen); memcpy(e->ai_canonname, ai->ai_canonname, name_bytes); }
    *tail = e;
    tail = &e->ai_next;
  }
  FreeAddrInfoW(found);
  *result = head;
  return 0;
}
static WCHAR *wide_copy(const WCHAR *s) {
  if (!s) return NULL;
  const size_t_ bytes = (wide_length(s) + 1) * sizeof(WCHAR);
  WCHAR *out = HeapAlloc(GetProcessHeap(), 0, bytes);
  if (out) memcpy(out, s, bytes);
  return out;
}
typedef struct { WCHAR *name, *service; ADDRINFOEXW_ hints; BOOL has_hints; ADDRINFOEXW_ **result; OVERLAPPED *overlapped; LOOKUP_ROUTINE_ routine; } Lookup;
static void free_lookup(Lookup *l) {
  if (l->name) HeapFree(GetProcessHeap(), 0, l->name);
  if (l->service) HeapFree(GetProcessHeap(), 0, l->service);
  HeapFree(GetProcessHeap(), 0, l);
}
static DWORD WINAPI lookup_thread(PVOID parameter) {
  Lookup *l = parameter;
  const int error = lookup_ex(l->name, l->service, l->has_hints ? &l->hints : NULL, l->result);
  OVERLAPPED *ov = l->overlapped;
  const LOOKUP_ROUTINE_ routine = l->routine;
  free_lookup(l);
  ov->InternalHigh = 0;
  __atomic_store_n(&ov->Internal, (ULONG_PTR)(DWORD)error, __ATOMIC_RELEASE);
  if (ov->hEvent) SetEvent(ov->hEvent);
  if (routine) routine((DWORD)error, 0, ov);
  return 0;
}
EXPORT int WSAAPI GetAddrInfoExW(const WCHAR *name, const WCHAR *service, DWORD name_space, GUID_ *provider, const ADDRINFOEXW_ *hints, ADDRINFOEXW_ **result,
                                 void *timeout, OVERLAPPED *ov, LOOKUP_ROUTINE_ routine, HANDLE *handle) {
  (void)provider; (void)timeout;
  if (!result) { set_wsa_error(WSA_INVALID_PARAMETER_); return WSA_INVALID_PARAMETER_; }
  *result = NULL;
  if (handle) *handle = NULL;
  /* NS_ALL, NS_DNS, NS_NETBT, NS_WINS, NS_NLA: all answered by the one resolver */
  if (name_space != 0 && (name_space < 12 || name_space > 15)) { set_wsa_error(WSAEINVAL_); return WSAEINVAL_; }
  if (routine && !ov) { set_wsa_error(WSA_INVALID_PARAMETER_); return WSA_INVALID_PARAMETER_; }
  if (!ov) {
    const int error = lookup_ex(name, service, hints, result);
    set_wsa_error(error);
    return error;
  }
  Lookup *l = HeapAlloc(GetProcessHeap(), 0, sizeof *l);
  if (!l) { set_wsa_error(WSA_NOT_ENOUGH_MEMORY_); return WSA_NOT_ENOUGH_MEMORY_; }
  memset(l, 0, sizeof *l);
  l->name = wide_copy(name); l->service = wide_copy(service);
  if (hints) { l->hints = *hints; l->has_hints = TRUE; }
  l->result = result; l->overlapped = ov; l->routine = routine;
  if ((name && !l->name) || (service && !l->service)) { free_lookup(l); set_wsa_error(WSA_NOT_ENOUGH_MEMORY_); return WSA_NOT_ENOUGH_MEMORY_; }
  ov->Internal = WSA_IO_PENDING_;
  ov->InternalHigh = 0;
  HANDLE thread = CreateThread(NULL, 0, lookup_thread, l, 0, NULL);
  if (!thread) { free_lookup(l); ov->Internal = 0; set_wsa_error(WSA_NOT_ENOUGH_MEMORY_); return WSA_NOT_ENOUGH_MEMORY_; }
  CloseHandle(thread);
  if (handle) *handle = (HANDLE)ov;      /* (a value for GetAddrInfoExCancel, which cannot cancel anything) */
  set_wsa_error(WSA_IO_PENDING_);
  return WSA_IO_PENDING_;
}
/* The error of an overlapped GetAddrInfoExW: WSA_IO_PENDING (997) while it runs. */
EXPORT int WSAAPI GetAddrInfoExOverlappedResult(OVERLAPPED *ov) {
  if (!ov) return WSAEINVAL_;
  return (int)(DWORD)__atomic_load_n(&ov->Internal, __ATOMIC_ACQUIRE);
}
EXPORT int WSAAPI GetAddrInfoExCancel(HANDLE *handle) {
  (void)handle;
  set_wsa_error(WSA_INVALID_HANDLE_);
  return WSA_INVALID_HANDLE_;
}
