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
 * kernel (index 2, 3, ...), whose IPv4 address, netmask, gateway and DNS server come from the socket service NtSocketControl (operation 5, src/Process/WindowsSockets.cpp).
 * GetAdaptersAddresses lays them out as Windows does (adapters, unicast addresses, prefixes, gateways and DNS servers with their sockaddrs, then the strings) in one buffer
 * whose size does not depend on the flags that only leave parts out; GetAdaptersInfo lists the Ethernet adapters. The interface LUID / GUID / index conversions follow the
 * same numbering, and so do the MIB tables of the interfaces (GetIfTable, GetIfEntry(2), GetIpAddrTable, GetAdapterIndex, GetBestInterface). Not done: IPv6 (the kernel's
 * network stack has none), the MAC address (the kernel does not report it: PhysicalAddressLength is 0), the routing / neighbour / connection tables (they answer
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
} Adapter;

static BOOL zero4(const BYTE *a) { return !a[0] && !a[1] && !a[2] && !a[3]; }
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

/* The bytes one adapter takes; the same for every flag that only leaves a part out. */
static SIZE_T adapter_size(const Adapter *a, BOOL ipv4) {
  SIZE_T n = align8(sizeof(IP_ADAPTER_ADDRESSES_LH));
  if (ipv4 && !zero4(a->address)) {
    n += align8(sizeof(IP_ADAPTER_UNICAST_ADDRESS_LH)) + align8(sizeof(SOCKADDR_IN));
    n += 3 * (align8(sizeof(IP_ADAPTER_PREFIX_XP)) + align8(sizeof(SOCKADDR_IN)));
    if (!zero4(a->gateway)) n += align8(sizeof(IP_ADAPTER_GATEWAY_ADDRESS_LH)) + align8(sizeof(SOCKADDR_IN));
    if (!zero4(a->dns)) n += align8(sizeof(IP_ADAPTER_DNS_SERVER_ADDRESS_XP)) + align8(sizeof(SOCKADDR_IN));
  }
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
static SOCKADDR *sockaddr4(Cursor *c, const BYTE *address) {
  SOCKADDR_IN *in = take(c, sizeof *in);
  in->sin_family = AF_INET;
  copy(&in->sin_addr, address, 4);
  return (SOCKADDR *)in;
}
static WCHAR *wide_copy(Cursor *c, const WCHAR *text) {
  const SIZE_T n = wlen(text) + 1;
  WCHAR *w = take(c, n * sizeof(WCHAR));
  copy(w, text, n * sizeof(WCHAR));
  return w;
}

static void fill_adapter(Cursor *c, const Adapter *a, ULONG flags, BOOL ipv4, IP_ADAPTER_ADDRESSES_LH *aa) {
  aa->Length = sizeof *aa;
  aa->IfIndex = a->index;
  aa->IfType = a->type;
  aa->Mtu = a->type == IF_TYPE_LOOPBACK_ ? 0xFFFFFFFFu : 1500;
  aa->OperStatus = a->type == IF_TYPE_LOOPBACK_ || !zero4(a->address) ? IfOperStatusUp : IfOperStatusDown;
  aa->Ipv4Enabled = TRUE;
  aa->Ipv4Metric = a->type == IF_TYPE_LOOPBACK_ ? 75 : 25;
  aa->Ipv6Metric = aa->Ipv4Metric;
  aa->Ipv6IfIndex = 0;
  aa->TransmitLinkSpeed = aa->ReceiveLinkSpeed = 1000000000ull;
  aa->ConnectionType = NET_IF_CONNECTION_DEDICATED;
  aa->TunnelType = TUNNEL_TYPE_NONE;
  if (a->type != IF_TYPE_LOOPBACK_) aa->Dhcpv4Enabled = TRUE;
  luid_of(a, &aa->Luid);
  for (int z = 0; z < 16; ++z) aa->ZoneIndices[z] = z == 0 ? 0 : z <= 2 ? a->index : 1;      /* (the interface and link scopes are the interface; the wider ones zone 1) */
  if (ipv4 && !zero4(a->address)) {
    if (!(flags & GAA_FLAG_SKIP_UNICAST)) {
      IP_ADAPTER_UNICAST_ADDRESS_LH *ua = take(c, sizeof *ua);
      ua->Length = sizeof *ua;
      ua->Flags = a->type == IF_TYPE_LOOPBACK_ ? 0 : IP_ADAPTER_ADDRESS_DNS_ELIGIBLE;
      ua->Address.lpSockaddr = sockaddr4(c, a->address);
      ua->Address.iSockaddrLength = sizeof(SOCKADDR_IN);
      ua->PrefixOrigin = a->type == IF_TYPE_LOOPBACK_ ? IpPrefixOriginWellKnown : IpPrefixOriginDhcp;
      ua->SuffixOrigin = a->type == IF_TYPE_LOOPBACK_ ? IpSuffixOriginWellKnown : IpSuffixOriginDhcp;
      ua->DadState = IpDadStatePreferred;
      ua->ValidLifetime = ua->PreferredLifetime = ua->LeaseLifetime = 0xFFFFFFFFu;
      ua->OnLinkPrefixLength = (UINT8)prefix_length(a->netmask);
      aa->FirstUnicastAddress = ua;
    } else c->next += align8(sizeof(IP_ADAPTER_UNICAST_ADDRESS_LH)) + align8(sizeof(SOCKADDR_IN));
    /* the prefixes: the network, the address itself and the broadcast address of the network */
    if (flags & GAA_FLAG_INCLUDE_PREFIX) {
      BYTE network[4], broadcast[4];
      for (int i = 0; i < 4; ++i) { network[i] = a->address[i] & a->netmask[i]; broadcast[i] = network[i] | (BYTE)~a->netmask[i]; }
      const BYTE *addresses[3] = {network, a->address, broadcast};
      const ULONG lengths[3] = {(ULONG)prefix_length(a->netmask), 32, 32};
      IP_ADAPTER_PREFIX_XP **link = &aa->FirstPrefix;
      for (int i = 0; i < 3; ++i) {
        IP_ADAPTER_PREFIX_XP *p = take(c, sizeof *p);
        p->Length = sizeof *p;
        p->Address.lpSockaddr = sockaddr4(c, addresses[i]);
        p->Address.iSockaddrLength = sizeof(SOCKADDR_IN);
        p->PrefixLength = lengths[i];
        *link = p;
        link = &p->Next;
      }
    } else c->next += 3 * (align8(sizeof(IP_ADAPTER_PREFIX_XP)) + align8(sizeof(SOCKADDR_IN)));
    if (!zero4(a->gateway)) {
      if (flags & GAA_FLAG_INCLUDE_GATEWAYS) {
        IP_ADAPTER_GATEWAY_ADDRESS_LH *g = take(c, sizeof *g);
        g->Length = sizeof *g;
        g->Address.lpSockaddr = sockaddr4(c, a->gateway);
        g->Address.iSockaddrLength = sizeof(SOCKADDR_IN);
        aa->FirstGatewayAddress = g;
      } else c->next += align8(sizeof(IP_ADAPTER_GATEWAY_ADDRESS_LH)) + align8(sizeof(SOCKADDR_IN));
    }
    if (!zero4(a->dns)) {
      if (!(flags & GAA_FLAG_SKIP_DNS_SERVER)) {
        IP_ADAPTER_DNS_SERVER_ADDRESS_XP *d = take(c, sizeof *d);
        d->Length = sizeof *d;
        d->Address.lpSockaddr = sockaddr4(c, a->dns);
        d->Address.iSockaddrLength = sizeof(SOCKADDR_IN);
        aa->FirstDnsServerAddress = d;
      } else c->next += align8(sizeof(IP_ADAPTER_DNS_SERVER_ADDRESS_XP)) + align8(sizeof(SOCKADDR_IN));
    }
  }
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
  const BOOL ipv4 = family != AF_INET6;
  SIZE_T need = 0;
  for (ULONG i = 0; i < count; ++i) need += adapter_size(&list[i], ipv4);
  if (!addresses || *size < need) { *size = (ULONG)need; return ERROR_BUFFER_OVERFLOW; }
  zero(addresses, need);
  Cursor c = {(BYTE *)addresses};
  IP_ADAPTER_ADDRESSES_LH *previous = NULL;
  for (ULONG i = 0; i < count; ++i) {
    BYTE *start = c.next;
    IP_ADAPTER_ADDRESSES_LH *aa = take(&c, sizeof *aa);
    fill_adapter(&c, &list[i], flags, ipv4, aa);
    c.next = start + adapter_size(&list[i], ipv4);      /* (the parts left out keep their place) */
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
