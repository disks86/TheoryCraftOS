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

/* iphlpapi.dll for TheoryCraftOS: the adapter lists. The adapters are the software loopback (index 1, 127.0.0.1/8) and one Ethernet adapter per network interface of the
 * kernel (index 2, 3, ...), whose IPv4 address, netmask, gateway and DNS server come from the socket service NtSocketControl (operation 5, src/Process/WindowsSockets.cpp)
 * and whose IPv6 link-local and global addresses, router and DNS server from operation 8 (the loopback has ::1).
 * GetAdaptersAddresses lays them out as Windows does (adapters, unicast addresses, prefixes, gateways and DNS servers with their sockaddrs, then the strings) in one buffer
 * whose size does not depend on the flags that only leave parts out; GetAdaptersInfo lists the Ethernet adapters. The interface LUID / GUID / index conversions follow the
 * same numbering, and so do the MIB tables of the interfaces (GetIfTable, GetIfEntry(2), GetIpAddrTable, GetAdapterIndex, GetBestInterface). Not done: IPv6 in anything but
 * GetAdaptersAddresses, the MAC address (the kernel does not report it: PhysicalAddressLength is 0), the routing / neighbour / connection tables (they answer
 * ERROR_NOT_SUPPORTED) and notifications (still stubs). Built freestanding over kernel32.dll and ntdll.dll. */
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

__declspec(dllimport) LONG NTAPI NtCreateSocket(ULONG family, ULONG type, ULONG protocol, PHANDLE result);
__declspec(dllimport) LONG NTAPI NtSocketControl(HANDLE socket, ULONG operation, PVOID in, ULONG in_length, PVOID out, ULONG out_length);
__declspec(dllimport) LONG NTAPI NtClose(HANDLE handle);

#define MAX_ADAPTERS 8
#define LOOPBACK_INDEX 1
#define IF_TYPE_ETHERNET_ 6
#define IF_TYPE_LOOPBACK_ 24

typedef struct {
  ULONG index, type;
  BYTE address[4], netmask[4], gateway[4], dns[4];      /* network order; address 0.0.0.0: not configured */
  BYTE link_local6[16], global6[16], router6[16], dns6[16], prefix6[16];      /* (all zero: none) */
  BYTE prefix_length6;
  ULONG mtu;
} Adapter;

static BOOL zero4(const BYTE *a) { return !a[0] && !a[1] && !a[2] && !a[3]; }
static BOOL zero16(const BYTE *a) { for (int i = 0; i < 16; ++i) if (a[i]) return FALSE; return TRUE; }
static void put4(BYTE *out, BYTE a, BYTE b, BYTE c, BYTE d) { out[0] = a; out[1] = b; out[2] = c; out[3] = d; }
static void zero(void *p, SIZE_T n) { for (SIZE_T i = 0; i < n; ++i) ((BYTE *)p)[i] = 0; }
static void copy(void *to, const void *from, SIZE_T n) { for (SIZE_T i = 0; i < n; ++i) ((BYTE *)to)[i] = ((const BYTE *)from)[i]; }
static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }

/* The adapters: the loopback, then each interface the kernel's sockets know. */
static ULONG list_adapters(Adapter *out) {
  ULONG n = 0;
  zero(&out[0], sizeof out[0]);
  out[0].index = LOOPBACK_INDEX;
  out[0].type = IF_TYPE_LOOPBACK_;
  put4(out[0].address, 127, 0, 0, 1);
  put4(out[0].netmask, 255, 0, 0, 0);
  out[0].global6[15] = 1;      /* ::1 */
  out[0].prefix6[15] = 1;
  out[0].prefix_length6 = 128;
  out[0].mtu = 0xFFFFFFFFu;
  n = 1;
  HANDLE socket = NULL;
  if (NtCreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &socket) < 0) return n;
  for (ULONG i = 0; n < MAX_ADAPTERS; ++i) {
    BYTE config[16];
    if (NtSocketControl(socket, 5 /* InterfaceConfiguration */, &i, sizeof i, config, sizeof config) != 0) break;
    Adapter *a = &out[n++];
    a->index = LOOPBACK_INDEX + 1 + i;
    a->type = IF_TYPE_ETHERNET_;
    copy(a->address, config, 4);
    copy(a->netmask, config + 4, 4);
    copy(a->gateway, config + 8, 4);
    copy(a->dns, config + 12, 4);
    a->mtu = 1500;
    BYTE config6[72];
    if (NtSocketControl(socket, 8 /* InterfaceConfiguration6 */, &i, sizeof i, config6, sizeof config6) == 0) {
      const BYTE ready = config6[65];      /* 1: link-local ready, 2: global ready */
      if (ready & 1) copy(a->link_local6, config6, 16);
      if (ready & 2) { copy(a->global6, config6 + 16, 16); copy(a->prefix6, config6 + 16, 8); a->prefix_length6 = config6[64]; }
      copy(a->router6, config6 + 32, 16);
      copy(a->dns6, config6 + 48, 16);
      const ULONG mtu = (ULONG)config6[68] | (ULONG)config6[69] << 8 | (ULONG)config6[70] << 16 | (ULONG)config6[71] << 24;
      if (mtu >= 1280 && mtu < a->mtu) a->mtu = mtu;
    }
  }
  NtClose(socket);
  return n;
}

static int prefix_length(const BYTE *mask) {
  int bits = 0;
  for (int i = 0; i < 4; ++i) for (int b = 7; b >= 0; --b) if (mask[i] >> b & 1) ++bits;
  return bits;
}

/* The GUID of an adapter's index: {2E6F7D90-0001-4C0D-8000-0000000000xx}. */
static void adapter_guid(ULONG index, GUID *guid) {
  static const GUID base = {0x2E6F7D90, 0x0001, 0x4C0D, {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  *guid = base;
  guid->Data4[7] = (BYTE)index;
}
static void guid_text(const GUID *g, char *out) {      /* {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}: 38 characters and the terminator */
  static const char hex[] = "0123456789ABCDEF";
  BYTE bytes[16];
  for (int i = 0; i < 4; ++i) bytes[i] = (BYTE)(g->Data1 >> (8 * (3 - i)));
  bytes[4] = (BYTE)(g->Data2 >> 8); bytes[5] = (BYTE)g->Data2;
  bytes[6] = (BYTE)(g->Data3 >> 8); bytes[7] = (BYTE)g->Data3;
  copy(bytes + 8, g->Data4, 8);
  int o = 0;
  out[o++] = '{';
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = hex[bytes[i] >> 4];
    out[o++] = hex[bytes[i] & 15];
  }
  out[o++] = '}';
  out[o] = 0;
}
static void luid_of(const Adapter *a, NET_LUID *luid) {
  luid->Value = 0;
  luid->Info.NetLuidIndex = a->index;
  luid->Info.IfType = a->type;
}
static const WCHAR *friendly_name(const Adapter *a) { return a->type == IF_TYPE_LOOPBACK_ ? L"Loopback Pseudo-Interface 1" : L"Ethernet"; }
static const WCHAR *description(const Adapter *a) { return a->type == IF_TYPE_LOOPBACK_ ? L"Software Loopback Interface 1" : L"TheoryCraft Network Adapter"; }

static SIZE_T align8(SIZE_T n) { return (n + 7) & ~(SIZE_T)7; }

/* The addresses one adapter lists, in each of GetAdaptersAddresses' lists: unicast addresses (IPv4, then IPv6 link-local and global), prefixes, gateways, DNS servers. */
typedef struct { BYTE family_v6; BYTE bytes[16]; ULONG length; BYTE origin; } Entry;      /* length: a prefix's length or a unicast address's on-link prefix */
typedef struct { Entry unicast[3], prefix[5], gateway[2], dns[2]; ULONG unicasts, prefixes, gateways, dnses; } Entries;
static void add(Entry *list, ULONG *count, BOOL v6, const BYTE *bytes, ULONG length, BYTE origin) {
  Entry *e = &list[(*count)++];
  zero(e, sizeof *e);
  e->family_v6 = (BYTE)v6;
  copy(e->bytes, bytes, v6 ? 16 : 4);
  e->length = length;
  e->origin = origin;
}
#define ORIGIN_DHCP_ 0
#define ORIGIN_WELL_KNOWN_ 1
#define ORIGIN_LINK_LOCAL_ 2
#define ORIGIN_ROUTER_ 3
static void entries_of(const Adapter *a, BOOL ipv4, BOOL ipv6, Entries *e) {
  zero(e, sizeof *e);
  const BOOL loop = a->type == IF_TYPE_LOOPBACK_;
  if (ipv4 && !zero4(a->address)) {
    const ULONG bits = (ULONG)prefix_length(a->netmask);
    add(e->unicast, &e->unicasts, FALSE, a->address, bits, loop ? ORIGIN_WELL_KNOWN_ : ORIGIN_DHCP_);
    /* the prefixes: the network, the address itself and the broadcast address of the network */
    BYTE network[4], broadcast[4];
    for (int i = 0; i < 4; ++i) { network[i] = a->address[i] & a->netmask[i]; broadcast[i] = network[i] | (BYTE)~a->netmask[i]; }
    add(e->prefix, &e->prefixes, FALSE, network, bits, 0);
    add(e->prefix, &e->prefixes, FALSE, a->address, 32, 0);
    add(e->prefix, &e->prefixes, FALSE, broadcast, 32, 0);
    if (!zero4(a->gateway)) add(e->gateway, &e->gateways, FALSE, a->gateway, 0, 0);
    if (!zero4(a->dns)) add(e->dns, &e->dnses, FALSE, a->dns, 0, 0);
  }
  if (ipv6) {
    if (!zero16(a->link_local6)) {
      static const BYTE kLinkLocal[16] = {0xFE, 0x80};
      add(e->unicast, &e->unicasts, TRUE, a->link_local6, 64, ORIGIN_LINK_LOCAL_);
      add(e->prefix, &e->prefixes, TRUE, kLinkLocal, 64, 0);
    }
    if (!zero16(a->global6)) {
      add(e->unicast, &e->unicasts, TRUE, a->global6, a->prefix_length6, loop ? ORIGIN_WELL_KNOWN_ : ORIGIN_ROUTER_);
      add(e->prefix, &e->prefixes, TRUE, a->prefix6, a->prefix_length6, 0);
    }
    if (!zero16(a->router6) && !loop) add(e->gateway, &e->gateways, TRUE, a->router6, 0, 0);
    if (!zero16(a->dns6) && !loop) add(e->dns, &e->dnses, TRUE, a->dns6, 0, 0);
  }
}
static SIZE_T sockaddr_size(const Entry *e) { return align8(e->family_v6 ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN)); }

/* The bytes one adapter takes; the same for every flag that only leaves a part out. */
static SIZE_T adapter_size(const Adapter *a, BOOL ipv4, BOOL ipv6) {
  Entries e;
  entries_of(a, ipv4, ipv6, &e);
  SIZE_T n = align8(sizeof(IP_ADAPTER_ADDRESSES_LH));
  for (ULONG i = 0; i < e.unicasts; ++i) n += align8(sizeof(IP_ADAPTER_UNICAST_ADDRESS_LH)) + sockaddr_size(&e.unicast[i]);
  for (ULONG i = 0; i < e.prefixes; ++i) n += align8(sizeof(IP_ADAPTER_PREFIX_XP)) + sockaddr_size(&e.prefix[i]);
  for (ULONG i = 0; i < e.gateways; ++i) n += align8(sizeof(IP_ADAPTER_GATEWAY_ADDRESS_LH)) + sockaddr_size(&e.gateway[i]);
  for (ULONG i = 0; i < e.dnses; ++i) n += align8(sizeof(IP_ADAPTER_DNS_SERVER_ADDRESS_XP)) + sockaddr_size(&e.dns[i]);
  n += align8(39);                                         /* AdapterName */
  n += align8(sizeof(WCHAR));                              /* DnsSuffix */
  n += align8((wlen(description(a)) + 1) * sizeof(WCHAR));
  n += align8((wlen(friendly_name(a)) + 1) * sizeof(WCHAR));
  return n;
}

/* A bump allocator over the caller's buffer (sized beforehand, so it cannot run out). */
typedef struct { BYTE *next; } Cursor;
static void *take(Cursor *c, SIZE_T bytes) {
  void *p = c->next;
  zero(p, align8(bytes));
  c->next += align8(bytes);
  return p;
}
static void socket_address(Cursor *c, const Entry *e, SOCKET_ADDRESS *out) {
  if (e->family_v6) {
    SOCKADDR_IN6 *in6 = take(c, sizeof *in6);
    in6->sin6_family = AF_INET6;
    copy(&in6->sin6_addr, e->bytes, 16);
    out->lpSockaddr = (SOCKADDR *)in6;
    out->iSockaddrLength = sizeof *in6;
  } else {
    SOCKADDR_IN *in = take(c, sizeof *in);
    in->sin_family = AF_INET;
    copy(&in->sin_addr, e->bytes, 4);
    out->lpSockaddr = (SOCKADDR *)in;
    out->iSockaddrLength = sizeof *in;
  }
}
static WCHAR *wide_copy(Cursor *c, const WCHAR *text) {
  const SIZE_T n = wlen(text) + 1;
  WCHAR *w = take(c, n * sizeof(WCHAR));
  copy(w, text, n * sizeof(WCHAR));
  return w;
}
/* Space the left-out entries of one list would have taken. */
static void skip(Cursor *c, const Entry *list, ULONG count, SIZE_T node) {
  for (ULONG i = 0; i < count; ++i) c->next += align8(node) + sockaddr_size(&list[i]);
}

static void fill_adapter(Cursor *c, const Adapter *a, ULONG flags, BOOL ipv4, BOOL ipv6, IP_ADAPTER_ADDRESSES_LH *aa) {
  const BOOL loop = a->type == IF_TYPE_LOOPBACK_;
  Entries e;
  entries_of(a, ipv4, ipv6, &e);
  aa->Length = sizeof *aa;
  aa->IfIndex = a->index;
  aa->IfType = a->type;
  aa->Mtu = a->mtu;
  aa->OperStatus = loop || !zero4(a->address) || !zero16(a->link_local6) ? IfOperStatusUp : IfOperStatusDown;
  aa->Ipv4Enabled = TRUE;
  aa->Ipv6Enabled = loop || !zero16(a->link_local6);
  aa->Ipv4Metric = loop ? 75 : 25;
  aa->Ipv6Metric = aa->Ipv4Metric;
  aa->Ipv6IfIndex = aa->Ipv6Enabled ? a->index : 0;
  aa->TransmitLinkSpeed = aa->ReceiveLinkSpeed = 1000000000ull;
  aa->ConnectionType = NET_IF_CONNECTION_DEDICATED;
  aa->TunnelType = TUNNEL_TYPE_NONE;
  if (!loop) aa->Dhcpv4Enabled = TRUE;
  luid_of(a, &aa->Luid);
  for (int z = 0; z < 16; ++z) aa->ZoneIndices[z] = z == 0 ? 0 : z <= 2 ? a->index : 1;      /* (the interface and link scopes are the interface; the wider ones zone 1) */
  if (!(flags & GAA_FLAG_SKIP_UNICAST)) {
    IP_ADAPTER_UNICAST_ADDRESS_LH **link = (IP_ADAPTER_UNICAST_ADDRESS_LH **)&aa->FirstUnicastAddress;
    for (ULONG i = 0; i < e.unicasts; ++i) {
      const Entry *u = &e.unicast[i];
      IP_ADAPTER_UNICAST_ADDRESS_LH *ua = take(c, sizeof *ua);
      ua->Length = sizeof *ua;
      ua->Flags = loop || u->origin == ORIGIN_LINK_LOCAL_ ? 0 : IP_ADAPTER_ADDRESS_DNS_ELIGIBLE;
      socket_address(c, u, &ua->Address);
      ua->PrefixOrigin = u->origin == ORIGIN_WELL_KNOWN_ ? IpPrefixOriginWellKnown : u->origin == ORIGIN_DHCP_ ? IpPrefixOriginDhcp
                         : u->origin == ORIGIN_ROUTER_ ? IpPrefixOriginRouterAdvertisement : IpPrefixOriginWellKnown;
      ua->SuffixOrigin = u->origin == ORIGIN_WELL_KNOWN_ ? IpSuffixOriginWellKnown : u->origin == ORIGIN_DHCP_ ? IpSuffixOriginDhcp : IpSuffixOriginLinkLayerAddress;
      ua->DadState = IpDadStatePreferred;
      ua->ValidLifetime = ua->PreferredLifetime = ua->LeaseLifetime = 0xFFFFFFFFu;
      ua->OnLinkPrefixLength = (UINT8)u->length;
      *link = ua;
      link = &ua->Next;
    }
  } else skip(c, e.unicast, e.unicasts, sizeof(IP_ADAPTER_UNICAST_ADDRESS_LH));
  if (flags & GAA_FLAG_INCLUDE_PREFIX) {
    IP_ADAPTER_PREFIX_XP **link = &aa->FirstPrefix;
    for (ULONG i = 0; i < e.prefixes; ++i) {
      IP_ADAPTER_PREFIX_XP *p = take(c, sizeof *p);
      p->Length = sizeof *p;
      socket_address(c, &e.prefix[i], &p->Address);
      p->PrefixLength = e.prefix[i].length;
      *link = p;
      link = &p->Next;
    }
  } else skip(c, e.prefix, e.prefixes, sizeof(IP_ADAPTER_PREFIX_XP));
  if (flags & GAA_FLAG_INCLUDE_GATEWAYS) {
    IP_ADAPTER_GATEWAY_ADDRESS_LH **link = &aa->FirstGatewayAddress;
    for (ULONG i = 0; i < e.gateways; ++i) {
      IP_ADAPTER_GATEWAY_ADDRESS_LH *g = take(c, sizeof *g);
      g->Length = sizeof *g;
      socket_address(c, &e.gateway[i], &g->Address);
      *link = g;
      link = &g->Next;
    }
  } else skip(c, e.gateway, e.gateways, sizeof(IP_ADAPTER_GATEWAY_ADDRESS_LH));
  if (!(flags & GAA_FLAG_SKIP_DNS_SERVER)) {
    IP_ADAPTER_DNS_SERVER_ADDRESS_XP **link = &aa->FirstDnsServerAddress;
    for (ULONG i = 0; i < e.dnses; ++i) {
      IP_ADAPTER_DNS_SERVER_ADDRESS_XP *d = take(c, sizeof *d);
      d->Length = sizeof *d;
      socket_address(c, &e.dns[i], &d->Address);
      *link = d;
      link = &d->Next;
    }
  } else skip(c, e.dns, e.dnses, sizeof(IP_ADAPTER_DNS_SERVER_ADDRESS_XP));
  GUID guid;
  adapter_guid(a->index, &guid);
  char *name = take(c, 39);
  guid_text(&guid, name);
  aa->AdapterName = name;
  copy(&aa->NetworkGuid, &guid, sizeof guid);
  aa->DnsSuffix = take(c, sizeof(WCHAR));
  aa->Description = wide_copy(c, description(a));
  if (flags & GAA_FLAG_SKIP_FRIENDLY_NAME) {      /* an empty name in the space the name would take */
    WCHAR *empty = take(c, (wlen(friendly_name(a)) + 1) * sizeof(WCHAR));
    aa->FriendlyName = empty;
  } else aa->FriendlyName = wide_copy(c, friendly_name(a));
}

EXPORT ULONG WINAPI GetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES addresses, PULONG size) {
  (void)reserved;
  if (!size) return ERROR_INVALID_PARAMETER;
  if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  const BOOL ipv4 = family != AF_INET6, ipv6 = family != AF_INET;
  SIZE_T need = 0;
  for (ULONG i = 0; i < count; ++i) need += adapter_size(&list[i], ipv4, ipv6);
  if (!addresses || *size < need) { *size = (ULONG)need; return ERROR_BUFFER_OVERFLOW; }
  zero(addresses, need);
  Cursor c = {(BYTE *)addresses};
  IP_ADAPTER_ADDRESSES_LH *previous = NULL;
  for (ULONG i = 0; i < count; ++i) {
    BYTE *start = c.next;
    IP_ADAPTER_ADDRESSES_LH *aa = take(&c, sizeof *aa);
    fill_adapter(&c, &list[i], flags, ipv4, ipv6, aa);
    c.next = start + adapter_size(&list[i], ipv4, ipv6);      /* (the parts left out keep their place) */
    if (previous) previous->Next = aa;
    previous = aa;
  }
  *size = (ULONG)need;
  return ERROR_SUCCESS;
}

/* IP_ADAPTER_INFO: the Ethernet adapters only (Windows leaves the loopback out), addresses as dotted text. */
static void dotted(const BYTE *a, char *out) {
  int o = 0;
  for (int i = 0; i < 4; ++i) {
    const BYTE v = a[i];
    if (v >= 100) out[o++] = (char)('0' + v / 100);
    if (v >= 10) out[o++] = (char)('0' + v / 10 % 10);
    out[o++] = (char)('0' + v % 10);
    if (i < 3) out[o++] = '.';
  }
  out[o] = 0;
}
static void ascii_copy(char *out, SIZE_T room, const WCHAR *text) {
  SIZE_T i = 0;
  for (; text[i] && i + 1 < room; ++i) out[i] = (char)text[i];
  out[i] = 0;
}
EXPORT DWORD WINAPI GetAdaptersInfo(PIP_ADAPTER_INFO info, PULONG size) {
  if (!size) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  ULONG ethernet = 0;
  for (ULONG i = 0; i < count; ++i) if (list[i].type != IF_TYPE_LOOPBACK_) ++ethernet;
  if (ethernet == 0) return ERROR_NO_DATA;
  const ULONG need = ethernet * (ULONG)sizeof(IP_ADAPTER_INFO);
  if (!info || *size < need) { *size = need; return ERROR_BUFFER_OVERFLOW; }
  zero(info, need);
  IP_ADAPTER_INFO *out = info;
  for (ULONG i = 0; i < count; ++i) {
    const Adapter *a = &list[i];
    if (a->type == IF_TYPE_LOOPBACK_) continue;
    GUID guid;
    adapter_guid(a->index, &guid);
    guid_text(&guid, out->AdapterName);
    ascii_copy(out->Description, sizeof out->Description, description(a));
    out->Index = out->ComboIndex = a->index;
    out->Type = MIB_IF_TYPE_ETHERNET;
    out->DhcpEnabled = TRUE;
    dotted(a->address, out->IpAddressList.IpAddress.String);
    dotted(a->netmask, out->IpAddressList.IpMask.String);
    dotted(a->gateway, out->GatewayList.IpAddress.String);
    out->Next = out + 1 < info + ethernet ? out + 1 : NULL;
    ++out;
  }
  *size = need;
  return ERROR_SUCCESS;
}

EXPORT DWORD WINAPI GetNumberOfInterfaces(PDWORD count) {
  if (!count) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  *count = list_adapters(list);
  return NO_ERROR;
}

/* LUIDs, GUIDs and indices of the same adapters. */
static const Adapter *find_by_index(Adapter *list, ULONG count, ULONG index) {
  for (ULONG i = 0; i < count; ++i) if (list[i].index == index) return &list[i];
  return NULL;
}
EXPORT DWORD WINAPI ConvertInterfaceIndexToLuid(NET_IFINDEX index, PNET_LUID luid) {
  if (!luid) return ERROR_INVALID_PARAMETER;
  luid->Value = 0;
  Adapter list[MAX_ADAPTERS];
  const Adapter *a = find_by_index(list, list_adapters(list), index);
  if (!a) return ERROR_FILE_NOT_FOUND;
  luid_of(a, luid);
  return NO_ERROR;
}
EXPORT DWORD WINAPI ConvertInterfaceLuidToIndex(const NET_LUID *luid, PNET_IFINDEX index) {
  if (!luid || !index) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  for (ULONG i = 0; i < count; ++i) {
    NET_LUID l;
    luid_of(&list[i], &l);
    if (l.Value == luid->Value) { *index = list[i].index; return NO_ERROR; }
  }
  *index = 0;
  return ERROR_FILE_NOT_FOUND;
}
EXPORT DWORD WINAPI ConvertInterfaceLuidToGuid(const NET_LUID *luid, GUID *guid) {
  NET_IFINDEX index = 0;
  if (!luid || !guid) return ERROR_INVALID_PARAMETER;
  const DWORD status = ConvertInterfaceLuidToIndex(luid, &index);
  if (status != NO_ERROR) { zero(guid, sizeof *guid); return status; }
  adapter_guid(index, guid);
  return NO_ERROR;
}
EXPORT DWORD WINAPI ConvertInterfaceGuidToLuid(const GUID *guid, PNET_LUID luid) {
  if (!guid || !luid) return ERROR_INVALID_PARAMETER;
  luid->Value = 0;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  for (ULONG i = 0; i < count; ++i) {
    GUID g;
    adapter_guid(list[i].index, &g);
    const BYTE *x = (const BYTE *)&g, *y = (const BYTE *)guid;
    BOOL same = TRUE;
    for (SIZE_T b = 0; b < sizeof g; ++b) if (x[b] != y[b]) same = FALSE;
    if (same) { luid_of(&list[i], luid); return NO_ERROR; }
  }
  return ERROR_FILE_NOT_FOUND;
}

/* ---- the MIB tables of the interfaces and their addresses ---------------------------------------------------------------------------------------------------------- */

static ULONG be32(const BYTE *a) { return (ULONG)a[0] | (ULONG)a[1] << 8 | (ULONG)a[2] << 16 | (ULONG)a[3] << 24; }      /* (an IPAddr: the bytes in network order) */
static BOOL adapter_up(const Adapter *a) { return a->type == IF_TYPE_LOOPBACK_ || !zero4(a->address); }

/* \DEVICE\TCPIP_{GUID}: the name of an interface in the MIB_IFROW. */
static void device_name(const Adapter *a, WCHAR *out, SIZE_T room) {
  static const char prefix[] = "\\DEVICE\\TCPIP_";
  char guid_name[40];
  GUID guid;
  adapter_guid(a->index, &guid);
  guid_text(&guid, guid_name);
  SIZE_T o = 0;
  for (SIZE_T i = 0; prefix[i] && o + 1 < room; ++i) out[o++] = (WCHAR)prefix[i];
  for (SIZE_T i = 0; guid_name[i] && o + 1 < room; ++i) out[o++] = (WCHAR)guid_name[i];
  out[o] = 0;
}

static void fill_if_row(const Adapter *a, MIB_IFROW *row) {
  zero(row, sizeof *row);
  device_name(a, row->wszName, MAX_INTERFACE_NAME_LEN);
  row->dwIndex = a->index;
  row->dwType = a->type;
  row->dwMtu = a->type == IF_TYPE_LOOPBACK_ ? 0xFFFFFFFFu : 1500;
  row->dwSpeed = 1000000000u;
  row->dwAdminStatus = 1;      /* up */
  row->dwOperStatus = adapter_up(a) ? MIB_IF_OPER_STATUS_OPERATIONAL : MIB_IF_OPER_STATUS_NON_OPERATIONAL;
  const WCHAR *d = description(a);
  SIZE_T n = 0;
  for (; d[n] && n + 1 < MAXLEN_IFDESCR; ++n) row->bDescr[n] = (BYTE)d[n];
  row->bDescr[n] = 0;
  row->dwDescrLen = (DWORD)n + 1;
}

EXPORT DWORD WINAPI GetIfTable(PMIB_IFTABLE table, PULONG size, BOOL order) {
  (void)order;      /* (the rows are in index order already) */
  if (!size) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  const ULONG need = (ULONG)(FIELD_OFFSET(MIB_IFTABLE, table) + count * sizeof(MIB_IFROW));
  if (!table || *size < need) { *size = need; return ERROR_INSUFFICIENT_BUFFER; }
  table->dwNumEntries = count;
  for (ULONG i = 0; i < count; ++i) fill_if_row(&list[i], &table->table[i]);
  *size = need;
  return NO_ERROR;
}

EXPORT DWORD WINAPI GetIfEntry(PMIB_IFROW row) {
  if (!row) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const Adapter *a = find_by_index(list, list_adapters(list), row->dwIndex);
  if (!a) return ERROR_INVALID_DATA;
  fill_if_row(a, row);
  return NO_ERROR;
}

EXPORT DWORD WINAPI GetIfEntry2(PMIB_IF_ROW2 row) {
  if (!row) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  const Adapter *a = NULL;
  for (ULONG i = 0; i < count && !a; ++i) {
    NET_LUID l;
    luid_of(&list[i], &l);
    if (row->InterfaceLuid.Value ? l.Value == row->InterfaceLuid.Value : list[i].index == row->InterfaceIndex) a = &list[i];
  }
  if (!a) return ERROR_FILE_NOT_FOUND;
  zero(row, sizeof *row);
  luid_of(a, &row->InterfaceLuid);
  row->InterfaceIndex = a->index;
  adapter_guid(a->index, &row->InterfaceGuid);
  const WCHAR *alias = friendly_name(a), *d = description(a);
  for (SIZE_T i = 0; alias[i] && i + 1 < IF_MAX_STRING_SIZE + 1; ++i) row->Alias[i] = alias[i];
  for (SIZE_T i = 0; d[i] && i + 1 < IF_MAX_STRING_SIZE + 1; ++i) row->Description[i] = d[i];
  row->Mtu = a->type == IF_TYPE_LOOPBACK_ ? 0xFFFFFFFFu : 1500;
  row->Type = a->type;
  row->MediaType = NdisMedium802_3;
  row->PhysicalMediumType = NdisPhysicalMediumUnspecified;
  row->AccessType = a->type == IF_TYPE_LOOPBACK_ ? NET_IF_ACCESS_LOOPBACK : NET_IF_ACCESS_BROADCAST;
  row->DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
  row->InterfaceAndOperStatusFlags.HardwareInterface = a->type != IF_TYPE_LOOPBACK_;
  row->InterfaceAndOperStatusFlags.ConnectorPresent = a->type != IF_TYPE_LOOPBACK_;
  row->OperStatus = adapter_up(a) ? IfOperStatusUp : IfOperStatusDown;
  row->AdminStatus = NET_IF_ADMIN_STATUS_UP;
  row->MediaConnectState = adapter_up(a) ? MediaConnectStateConnected : MediaConnectStateDisconnected;
  row->ConnectionType = NET_IF_CONNECTION_DEDICATED;
  row->TransmitLinkSpeed = row->ReceiveLinkSpeed = 1000000000ull;
  return NO_ERROR;
}

EXPORT DWORD WINAPI GetAdapterIndex(LPWSTR name, PULONG index) {
  if (!name || !index) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  for (ULONG i = 0; i < count; ++i) {
    WCHAR candidate[64];
    device_name(&list[i], candidate, 64);
    SIZE_T k = 0;
    while (candidate[k] && (name[k] | 0x20) == (candidate[k] | 0x20)) ++k;      /* (case-insensitive: the GUID's hex digits and the prefix) */
    if (!candidate[k] && !name[k]) { *index = list[i].index; return NO_ERROR; }
  }
  return ERROR_INVALID_PARAMETER;
}

EXPORT DWORD WINAPI GetIpAddrTable(PMIB_IPADDRTABLE table, PULONG size, BOOL order) {
  if (!size) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  ULONG rows = 0;
  for (ULONG i = 0; i < count; ++i) if (!zero4(list[i].address)) ++rows;
  const ULONG need = (ULONG)(FIELD_OFFSET(MIB_IPADDRTABLE, table) + rows * sizeof(MIB_IPADDRROW));
  if (!table || *size < need) { *size = need; return ERROR_INSUFFICIENT_BUFFER; }
  table->dwNumEntries = rows;
  ULONG r = 0;
  for (ULONG i = 0; i < count; ++i) {
    if (zero4(list[i].address)) continue;
    MIB_IPADDRROW *row = &table->table[r++];
    zero(row, sizeof *row);
    row->dwAddr = be32(list[i].address);
    row->dwIndex = list[i].index;
    row->dwMask = be32(list[i].netmask);
    row->dwBCastAddr = 1;
    row->dwReasmSize = 0xFFFF;
    row->wType = MIB_IPADDR_PRIMARY;
  }
  if (order)      /* by address, as unsigned values (insertion sort: a handful of rows) */
    for (ULONG i = 1; i < rows; ++i)
      for (ULONG j = i; j > 0 && table->table[j - 1].dwAddr > table->table[j].dwAddr; --j) {
        MIB_IPADDRROW t = table->table[j];
        table->table[j] = table->table[j - 1];
        table->table[j - 1] = t;
      }
  *size = need;
  return NO_ERROR;
}

/* The interface a packet to `destination` leaves by: the one whose network holds it, the loopback for 127/8, else the first configured Ethernet adapter. */
EXPORT DWORD WINAPI GetBestInterface(IPAddr destination, PDWORD index) {
  if (!index) return ERROR_INVALID_PARAMETER;
  Adapter list[MAX_ADAPTERS];
  const ULONG count = list_adapters(list);
  const BYTE *d = (const BYTE *)&destination;
  const Adapter *fallback = NULL;
  for (ULONG i = 0; i < count; ++i) {
    const Adapter *a = &list[i];
    if (zero4(a->address)) continue;
    BOOL inside = TRUE;
    for (int b = 0; b < 4; ++b) if ((d[b] & a->netmask[b]) != (a->address[b] & a->netmask[b])) inside = FALSE;
    if (inside) { *index = a->index; return NO_ERROR; }
    if (!fallback && a->type != IF_TYPE_LOOPBACK_) fallback = a;
  }
  if (!fallback) return ERROR_CAN_NOT_COMPLETE;
  *index = fallback->index;
  return NO_ERROR;
}

/* The routing, neighbour and connection tables are not kept: they answer ERROR_NOT_SUPPORTED (and clear the table pointer of the *2 forms) instead of a success with nothing
 * written. */
#define NOT_SUPPORTED_TABLE(name, ...) EXPORT DWORD WINAPI name(__VA_ARGS__)
NOT_SUPPORTED_TABLE(GetIpForwardTable, PMIB_IPFORWARDTABLE table, PULONG size, BOOL order) { (void)table; (void)size; (void)order; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetIpNetTable, PMIB_IPNETTABLE table, PULONG size, BOOL order) { (void)table; (void)size; (void)order; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetTcpTable, PMIB_TCPTABLE table, PULONG size, BOOL order) { (void)table; (void)size; (void)order; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetUdpTable, PMIB_UDPTABLE table, PULONG size, BOOL order) { (void)table; (void)size; (void)order; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetExtendedTcpTable, PVOID table, PDWORD size, BOOL order, ULONG family, TCP_TABLE_CLASS kind, ULONG reserved) {
  (void)table; (void)size; (void)order; (void)family; (void)kind; (void)reserved;
  return ERROR_NOT_SUPPORTED;
}
NOT_SUPPORTED_TABLE(GetExtendedUdpTable, PVOID table, PDWORD size, BOOL order, ULONG family, UDP_TABLE_CLASS kind, ULONG reserved) {
  (void)table; (void)size; (void)order; (void)family; (void)kind; (void)reserved;
  return ERROR_NOT_SUPPORTED;
}
NOT_SUPPORTED_TABLE(GetIfTable2, PMIB_IF_TABLE2 *table) { if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetIfTable2Ex, MIB_IF_TABLE_LEVEL level, PMIB_IF_TABLE2 *table) { (void)level; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetIpForwardTable2, ADDRESS_FAMILY family, PMIB_IPFORWARD_TABLE2 *table) { (void)family; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetIpNetTable2, ADDRESS_FAMILY family, PMIB_IPNET_TABLE2 *table) { (void)family; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetIpInterfaceTable, ADDRESS_FAMILY family, PMIB_IPINTERFACE_TABLE *table) { (void)family; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetUnicastIpAddressTable, ADDRESS_FAMILY family, PMIB_UNICASTIPADDRESS_TABLE *table) { (void)family; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetAnycastIpAddressTable, ADDRESS_FAMILY family, PMIB_ANYCASTIPADDRESS_TABLE *table) { (void)family; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetMulticastIpAddressTable, ADDRESS_FAMILY family, PMIB_MULTICASTIPADDRESS_TABLE *table) { (void)family; if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetIfStackTable, PMIB_IFSTACK_TABLE *table) { if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_TABLE(GetInvertedIfStackTable, PMIB_INVERTEDIFSTACK_TABLE *table) { if (table) *table = NULL; return ERROR_NOT_SUPPORTED; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reason; (void)reserved;
  return TRUE;
}
