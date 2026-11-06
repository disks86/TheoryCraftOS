/* Tests of TheoryCraftOS's ws2_32.dll: IPv4 addresses in the standard in_addr layout (the first octet first in memory) on every path, address text (inet_pton / inet_ntop),
 * and name resolution without DNS (numeric addresses, localhost, this machine's name), as Godot's IP layer uses them.
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */

#include <stdarg.h>
#include <stdio.h>

#include "winsock2.h"
#include "ws2tcpip.h"
#include "windef.h"
#include "winbase.h"
#include "iphlpapi.h"
#include "wine/test.h"

static void test_byte_order(void)
{
    struct in_addr in;
    const BYTE *b = (const BYTE *)&in;

    in.s_addr = inet_addr("10.0.2.5");
    ok(in.s_addr == htonl(0x0A000205), "inet_addr %#lx\n", in.s_addr);
    ok(b[0] == 10 && b[1] == 0 && b[2] == 2 && b[3] == 5, "the first octet comes first in memory: %u.%u.%u.%u\n", b[0], b[1], b[2], b[3]);
    ok(!strcmp(inet_ntoa(in), "10.0.2.5"), "inet_ntoa %s\n", inet_ntoa(in));
    in.s_addr = htonl(INADDR_LOOPBACK);
    ok(!strcmp(inet_ntoa(in), "127.0.0.1"), "htonl(INADDR_LOOPBACK) is %s\n", inet_ntoa(in));
}

static void test_text(void)
{
    static const struct { const char *text; BYTE bytes[16]; const char *canonical; } v6[] =
    {
        {"::1", {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, "::1"},
        {"::", {0}, "::"},
        {"2001:DB8::1", {0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1}, "2001:db8::1"},
        {"fe80:0:0:0:1:2:3:4", {0xfe,0x80,0,0,0,0,0,0,0,1,0,2,0,3,0,4}, "fe80::1:2:3:4"},
        {"::ffff:1.2.3.4", {0,0,0,0,0,0,0,0,0,0,0xff,0xff,1,2,3,4}, "::ffff:1.2.3.4"},
        {"1:0:0:2:0:0:0:3", {0,1,0,0,0,0,0,2,0,0,0,0,0,0,0,3}, "1:0:0:2::3"},
    };
    static const char *bad6[] = {":::", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "g::1", ":1", "1:2:3:4:5:6:7"};
    static const char *bad4[] = {"1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.", " 1.2.3.4", "1.2.3.4x", "0x1.2.3.4"};
    BYTE out[16];
    char text[64];
    WCHAR wtext[64];
    unsigned i;

    ok(inet_pton(AF_INET, "192.168.1.200", out) == 1 && out[0] == 192 && out[3] == 200, "inet_pton IPv4\n");
    for (i = 0; i < ARRAY_SIZE(bad4); i++) ok(inet_pton(AF_INET, bad4[i], out) == 0, "inet_pton accepted %s\n", bad4[i]);
    for (i = 0; i < ARRAY_SIZE(v6); i++)
    {
        ok(inet_pton(AF_INET6, v6[i].text, out) == 1 && !memcmp(out, v6[i].bytes, 16), "inet_pton %s\n", v6[i].text);
        ok(inet_ntop(AF_INET6, v6[i].bytes, text, sizeof(text)) && !strcmp(text, v6[i].canonical), "inet_ntop %s: %s\n", v6[i].canonical, text);
    }
    for (i = 0; i < ARRAY_SIZE(bad6); i++) ok(inet_pton(AF_INET6, bad6[i], out) == 0, "inet_pton accepted %s\n", bad6[i]);
    ok(inet_pton(AF_APPLETALK, "1.2.3.4", out) == -1 && WSAGetLastError() == WSAEAFNOSUPPORT, "an unknown family\n");
    out[0] = 10; out[1] = 0; out[2] = 2; out[3] = 15;
    ok(inet_ntop(AF_INET, out, text, sizeof(text)) && !strcmp(text, "10.0.2.15"), "inet_ntop IPv4 %s\n", text);
    ok(inet_ntop(AF_INET, out, text, 9) == NULL, "a buffer one short\n");
    ok(InetPtonW(AF_INET, L"8.8.4.4", out) == 1 && out[0] == 8 && out[2] == 4, "InetPtonW\n");
    ok(InetNtopW(AF_INET, out, wtext, ARRAY_SIZE(wtext)) && !lstrcmpW(wtext, L"8.8.4.4"), "InetNtopW %s\n", wine_dbgstr_w(wtext));
}

static const BYTE *address_of(const ADDRINFOA *ai) { return (const BYTE *)&((const SOCKADDR_IN *)ai->ai_addr)->sin_addr; }
static USHORT port_of(const ADDRINFOA *ai) { return ntohs(((const SOCKADDR_IN *)ai->ai_addr)->sin_port); }

static void test_getaddrinfo(void)
{
    ADDRINFOA hints, *ai = (ADDRINFOA *)0xdeadbeef;
    ADDRINFOW whints, *wai = NULL;
    char host[64], service[16];
    SOCKADDR_IN sin;
    int ret;

    ret = getaddrinfo("127.0.0.1", "80", NULL, &ai);
    ok(!ret && ai, "numeric address: %d\n", ret);
    if (ai && ai != (ADDRINFOA *)0xdeadbeef)
    {
        ok(ai->ai_family == AF_INET && ai->ai_addrlen == sizeof(SOCKADDR_IN) && !ai->ai_next, "family %d length %Iu\n", ai->ai_family, ai->ai_addrlen);
        ok(address_of(ai)[0] == 127 && address_of(ai)[3] == 1 && port_of(ai) == 80, "127.0.0.1:80 -> %u...%u:%u\n", address_of(ai)[0], address_of(ai)[3], port_of(ai));
        freeaddrinfo(ai);
    }
    ai = NULL;
    ok(!getaddrinfo("localhost", NULL, NULL, &ai) && ai && address_of(ai)[0] == 127 && address_of(ai)[3] == 1, "localhost\n");
    freeaddrinfo(ai);
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ai = NULL;
    ok(!getaddrinfo(NULL, "8080", &hints, &ai) && ai, "passive\n");
    if (ai)
    {
        ok(!*(const DWORD *)address_of(ai) && port_of(ai) == 8080 && ai->ai_socktype == SOCK_STREAM, "the wildcard\n");
        freeaddrinfo(ai);
    }
    ai = (ADDRINFOA *)0xdeadbeef;
    ret = getaddrinfo("no-such-host.invalid", NULL, NULL, &ai);
    ok(ret == WSAHOST_NOT_FOUND && ai == NULL, "an unknown name: %d %p\n", ret, ai);
    hints.ai_flags = AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;
    ok(getaddrinfo("localhost", NULL, &hints, &ai) == WSAHOST_NOT_FOUND, "AI_NUMERICHOST with a name\n");
    hints.ai_flags = 0;
    hints.ai_family = AF_INET6;
    ok(getaddrinfo("127.0.0.1", NULL, &hints, &ai) == WSAHOST_NOT_FOUND, "an IPv4 address for AF_INET6\n");
    ai = NULL;
    ok(!getaddrinfo("::1", "443", NULL, &ai) && ai && ai->ai_family == AF_INET6 && ai->ai_addrlen == sizeof(SOCKADDR_IN6), "::1\n");
    freeaddrinfo(ai);
    ai = NULL;
    ok(!getaddrinfo("127.0.0.1", "https", NULL, &ai) && ai && port_of(ai) == 443, "a named service\n");
    freeaddrinfo(ai);
    ok(getaddrinfo("127.0.0.1", "no-such-service", NULL, &ai) == WSATYPE_NOT_FOUND, "an unknown service\n");
    ok(getaddrinfo(NULL, NULL, NULL, &ai) == WSAHOST_NOT_FOUND, "neither node nor service\n");

    memset(&whints, 0, sizeof(whints));
    whints.ai_flags = AI_CANONNAME;
    ok(!GetAddrInfoW(L"localhost", L"21", &whints, &wai) && wai, "GetAddrInfoW\n");
    if (wai)
    {
        ok(wai->ai_canonname && !lstrcmpW(wai->ai_canonname, L"localhost"), "canonical name %s\n", wine_dbgstr_w(wai->ai_canonname));
        ok(ntohs(((SOCKADDR_IN *)wai->ai_addr)->sin_port) == 21, "port\n");
        FreeAddrInfoW(wai);
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(6007);
    sin.sin_addr.s_addr = inet_addr("10.0.2.2");
    ok(!getnameinfo((SOCKADDR *)&sin, sizeof(sin), host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV), "getnameinfo\n");
    ok(!strcmp(host, "10.0.2.2") && !strcmp(service, "6007"), "getnameinfo %s %s\n", host, service);
    ok(getnameinfo((SOCKADDR *)&sin, sizeof(sin), host, sizeof(host), NULL, 0, NI_NAMEREQD) == WSAHOST_NOT_FOUND, "NI_NAMEREQD without DNS\n");
}

static void test_own_name(void)
{
    char name[256];
    ADDRINFOA *ai = NULL;

    ok(!gethostname(name, sizeof(name)) && name[0], "gethostname %s\n", name);
    ok(gethostname(name, 1) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "a short buffer\n");
    ok(!getaddrinfo(name, NULL, NULL, &ai) && ai && ai->ai_family == AF_INET, "this machine's name resolves\n");
    freeaddrinfo(ai);
}

/* The address getsockname gives (the socket service's path) is the one the adapter list has (iphlpapi's path): both in the in_addr layout. */
static void test_socket_address(void)
{
    IP_ADAPTER_ADDRESSES *list, *aa;
    SOCKADDR_IN sin;
    ULONG size = 0;
    int length = sizeof(sin);
    SOCKET s;
    DWORD expected = 0;

    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &size);
    list = HeapAlloc(GetProcessHeap(), 0, size);
    if (!GetAdaptersAddresses(AF_INET, 0, NULL, list, &size))
        for (aa = list; aa && !expected; aa = aa->Next)
            if (aa->IfType != IF_TYPE_SOFTWARE_LOOPBACK && aa->FirstUnicastAddress)
                expected = ((SOCKADDR_IN *)aa->FirstUnicastAddress->Address.lpSockaddr)->sin_addr.s_addr;
    HeapFree(GetProcessHeap(), 0, list);
    if (!expected)
    {
        skip("no configured network interface\n");
        return;
    }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(s != INVALID_SOCKET, "socket %d\n", WSAGetLastError());
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    ok(!bind(s, (SOCKADDR *)&sin, sizeof(sin)), "bind %d\n", WSAGetLastError());
    ok(!getsockname(s, (SOCKADDR *)&sin, &length), "getsockname %d\n", WSAGetLastError());
    ok(sin.sin_addr.s_addr == expected, "getsockname %s, adapter %#lx\n", inet_ntoa(sin.sin_addr), expected);
    ok(sin.sin_port != 0, "a port was picked\n");
    closesocket(s);
}

/* A server and a client in this process over 127.0.0.1, as Godot's game connects to its editor's debugger. */
static void test_loopback(void)
{
    SOCKADDR_IN address, peer;
    SOCKET listener, client, server, udp_a, udp_b;
    int length;
    char buffer[64];

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(6007);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(!bind(listener, (SOCKADDR *)&address, sizeof(address)), "bind %d\n", WSAGetLastError());
    ok(!listen(listener, 4), "listen %d\n", WSAGetLastError());
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(!connect(client, (SOCKADDR *)&address, sizeof(address)), "connect to 127.0.0.1: %d\n", WSAGetLastError());
    length = sizeof(peer);
    server = accept(listener, (SOCKADDR *)&peer, &length);
    ok(server != INVALID_SOCKET, "accept %d\n", WSAGetLastError());
    ok(peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "the client is at %s\n", inet_ntoa(peer.sin_addr));
    ok(send(client, "hello editor", 13, 0) == 13, "send %d\n", WSAGetLastError());
    memset(buffer, 0, sizeof(buffer));
    ok(recv(server, buffer, sizeof(buffer), 0) == 13 && !strcmp(buffer, "hello editor"), "recv %s\n", buffer);
    ok(send(server, "hello game", 11, 0) == 11, "send back %d\n", WSAGetLastError());
    memset(buffer, 0, sizeof(buffer));
    ok(recv(client, buffer, sizeof(buffer), 0) == 11 && !strcmp(buffer, "hello game"), "recv back %s\n", buffer);
    length = sizeof(peer);
    ok(!getsockname(client, (SOCKADDR *)&peer, &length) && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "client end %s\n", inet_ntoa(peer.sin_addr));
    length = sizeof(peer);
    ok(!getpeername(client, (SOCKADDR *)&peer, &length) && peer.sin_port == htons(6007), "server end port %u\n", ntohs(peer.sin_port));
    closesocket(client);
    ok(recv(server, buffer, sizeof(buffer), 0) == 0, "the close reaches the server\n");
    closesocket(server);
    closesocket(listener);

    udp_a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    udp_b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    address.sin_port = htons(6008);
    ok(!bind(udp_a, (SOCKADDR *)&address, sizeof(address)), "udp bind %d\n", WSAGetLastError());
    ok(sendto(udp_b, "datagram", 9, 0, (SOCKADDR *)&address, sizeof(address)) == 9, "sendto %d\n", WSAGetLastError());
    length = sizeof(peer);
    memset(buffer, 0, sizeof(buffer));
    ok(recvfrom(udp_a, buffer, sizeof(buffer), 0, (SOCKADDR *)&peer, &length) == 9 && !strcmp(buffer, "datagram"), "recvfrom %s\n", buffer);
    ok(peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "from %s\n", inet_ntoa(peer.sin_addr));
    closesocket(udp_a);
    closesocket(udp_b);
}

START_TEST(tcos_resolve)
{
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    test_byte_order();
    test_text();
    test_getaddrinfo();
    test_own_name();
    test_socket_address();
    test_loopback();
    WSACleanup();
}
