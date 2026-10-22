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

// Fetches a URL with the OS's own HTTP client over real sockets (tests/HostNetwork.hpp): a tool for trying the TLS, HTTP and WebSocket code against live
// servers from the development machine. Needs the network, so it is not part of the default host tests.
//   tests/run_host_tests.sh InteropTool            (builds it)
//   /tmp/tcos-host-tests/InteropTool https://www.google.com/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "HostNetwork.hpp"
#include "Crypto/Random.hpp"
#include "Network/Http/HttpClient.hpp"
#include "Network/Quic/QuicConnection.hpp"

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

struct UrandomSource final : crypto::IEntropySource {
  bool read(ByteView out) override {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
  }
};

static uint64_t unix_seconds(void*) { return static_cast<uint64_t>(time(nullptr)); }

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: InteropTool <url> [ca-bundle.pem]\n");
    return 2;
  }
  TestAllocator a;
  hostnet::HostNetworkStack stack(a);
  UrandomSource entropy;
  crypto::DrbgRandom random(entropy);
  if (!random.initialize()) { printf("no randomness\n"); return 1; }
  crypto::TrustStore trust(a);
  {
    FILE* f = fopen(argc > 2 ? argv[2] : "certs/ca-bundle.pem", "rb");
    if (!f) { printf("no CA bundle\n"); return 1; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = static_cast<char*>(malloc(static_cast<size_t>(size)));
    const size_t got = fread(text, 1, static_cast<size_t>(size), f);
    fclose(f);
    printf("%u trusted roots\n", trust.add_pem(text, got));
    free(text);
  }
  network::TlsConfig tls;
  tls.trust = &trust;
  tls.random = &random;
  tls.clock = unix_seconds;
  network::HttpClient http(stack, a);
  http.set_tls(&tls);
  http.add_header("User-Agent", "TheoryCraftOS-interop/1.0");
  network::AltSvcCache alt_svc(a);
  http.set_alt_svc_cache(&alt_svc);
  if (const char* mode = getenv("HTTP3")) http.set_http3(strcmp(mode, "off") == 0 ? network::Http3Mode::Off : strcmp(mode, "auto") == 0 ? network::Http3Mode::Auto : network::Http3Mode::Prefer);
  if (strncmp(argv[1], "quic://", 7) == 0) {   // a QUIC handshake with a server: quic://host[:port]
    std::string host = argv[1] + 7;
    uint16_t port = 443;
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
      port = static_cast<uint16_t>(atoi(host.c_str() + colon + 1));
      host.resize(colon);
    }
    network::Ipv4Address address;
    if (!stack.resolve(StringView(host.data(), host.size()), 5000, &address)) { printf("cannot resolve %s\n", host.c_str()); return 1; }
    network::QuicConfig config;
    config.tls = tls;
    config.tls.alpn = getenv("QUIC_ALPN") ? getenv("QUIC_ALPN") : "h3";
    network::QuicConnection quic(stack, a, config);
    if (!quic.connect(address, port, StringView(host.data(), host.size()))) { printf("connect failed: %s\n", network::quic_error_name(quic.error())); return 1; }
    const uint64_t start = stack.now_ms();
    while (!quic.established() && !quic.closed() && stack.now_ms() - start < 10000) { stack.poll(); quic.poll(); }
    const auto st = quic.stats();
    printf("%s: state %d, error %s (transport %llu, tls %d, cert %d, alert %u), alpn [%.*s], suite %s, rtt %u ms, cwnd %u, sent %llu received %llu lost %llu ptos %llu retry %d\n",
           quic.established() ? "established" : "FAILED", (int)quic.state(), network::quic_error_name(quic.error()), (unsigned long long)quic.transport_error(), (int)quic.tls_error(),
           (int)quic.certificate_status(), quic.tls_alert(), (int)quic.alpn().size(), quic.alpn().data(), quic.cipher_suite_name(), st.smoothed_rtt_ms, st.congestion_window,
           (unsigned long long)st.packets_sent, (unsigned long long)st.packets_received, (unsigned long long)st.packets_lost, (unsigned long long)st.ptos, (int)st.retry_seen);
    if (quic.error() == network::QuicError::PeerClosed)
      printf("peer closed with code %llu (%s) reason \"%.*s\"\n", (unsigned long long)quic.peer_close_code(), quic.peer_close_was_application() ? "application" : "transport", (int)quic.peer_close_reason().size(), quic.peer_close_reason().data());
    const bool ok = quic.established();
    // let the server's tickets and settings arrive, then close politely
    const uint64_t wait = stack.now_ms();
    while (!quic.closed() && stack.now_ms() - wait < 500) { stack.poll(); quic.poll(); }
    quic.close(0, "bye");
    return ok ? 0 : 1;
  }
  if (strncmp(argv[1], "ws://", 5) == 0 || strncmp(argv[1], "wss://", 6) == 0) {   // a WebSocket echo: send a message, print what comes back, close
    http.set_websocket_compression(getenv("WS_DEFLATE") != nullptr);
    if (!http.start_websocket(argv[1], getenv("WS_PROTOCOLS") ? getenv("WS_PROTOCOLS") : "") || !http.run(30000)) {
      printf("WebSocket FAILED: %s (status %u, tls %d, cert %d)\n", network::http_error_name(http.error()), http.ws_handshake_status(), (int)http.tls_error(), (int)http.certificate_status());
      return 1;
    }
    printf("WebSocket open (protocol [%.*s], compressed %d)\n", (int)http.ws_protocol().size(), http.ws_protocol().data(), (int)http.ws_compressed());
    http.ws_send_text("hello from TheoryCraftOS");
    network::WebSocketMessage message(a);
    const uint64_t start = stack.now_ms();
    int received = 0;
    while (stack.now_ms() - start < 6000 && !http.ws_closed()) {
      stack.poll();
      http.poll();
      while (http.ws_receive(&message)) {
        ++received;
        printf("message %d (%s, %llu bytes): %.*s\n", received, message.text ? "text" : "binary", (unsigned long long)message.data.size(), (int)(message.data.size() < 80 ? message.data.size() : 80),
               reinterpret_cast<const char*>(message.data.view().data()));
      }
      if (received >= 2) break;
    }
    http.ws_close(1000, "bye");
    for (int i = 0; i < 3000 && !http.ws_closed(); ++i) { stack.poll(); http.poll(); }
    printf("closed %d with code %u\n", (int)http.ws_closed(), http.ws_close_code());
    return received > 0 ? 0 : 1;
  }
  const bool ok = http.start(network::HttpMethod::Get, argv[1]) && http.run(30000);
  printf("http3: %d\n", (int)http.used_http3());
  printf("%s: status %u, error %s, tls error %d, cert status %d, alert %u, suite %s, %llu body bytes\n", ok ? "ok" : "FAILED", http.status(), network::http_error_name(http.error()),
         static_cast<int>(http.tls_error()), static_cast<int>(http.certificate_status()), http.tls_alert(), http.tls_cipher_suite(),
         static_cast<unsigned long long>(http.body().size()));
  printf("transfer-encoding [%.*s] content-length [%.*s] connection [%.*s] content-encoding [%.*s] state %d received %llu\n", (int)http.header("transfer-encoding").size(), http.header("transfer-encoding").data(), (int)http.header("content-length").size(), http.header("content-length").data(), (int)http.header("connection").size(), http.header("connection").data(), (int)http.header("content-encoding").size(), http.header("content-encoding").data(), (int)http.state(), (unsigned long long)http.bytes_received());
  if (ok) {
    printf("content-type: %.*s\n", static_cast<int>(http.header("content-type").size()), http.header("content-type").data());
    const ConstByteView b = http.body();
    printf("%.*s\n", static_cast<int>(b.size() < 300 ? b.size() : 300), reinterpret_cast<const char*>(b.data()));
  }
  return ok ? 0 : 1;
}
