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

// Command-line client for trying TlsSocket against real servers (`openssl s_server`, or anything on the network):
//   TlsLive host port [--ipps-print TEXT] [--name N] [--ca file.pem] [--insecure] [--tls12] [--tls13] [--alpn p] [--request TEXT] [--no-clock] [--ems] [--no-cbc]
// Connects, shuts the handshake through, sends the request (default "GET / HTTP/1.0"), prints what came back. Exit 0 on success,
// 1 if the handshake or the exchange failed (the TLS error is printed), 2 for bad arguments.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "PosixNetwork.hpp"
#include "TlsDeterministic.hpp"
#include "Crypto/Crypto.hpp"
#include "Hardware/Printer/IppPrinter.hpp"
#include "Network/Tls/TlsSocket.hpp"

using namespace network;

struct HeapAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? malloc(size) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t) override { return size ? malloc(size) : nullptr; }
  void deallocate(void* p) override { free(p); }
};

struct UrandomEntropy final : crypto::IEntropySource {
  bool read(ByteView out) override {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
  }
};

static uint64_t g_fixed_now = 0;
static uint64_t wall_clock(void*) { return g_fixed_now ? g_fixed_now : static_cast<uint64_t>(time(nullptr)); }

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: TlsLive host port [options]\n"); return 2; }
  HeapAllocator alloc;
  PosixNetworkStack stack(alloc);
  UrandomEntropy urandom;
  bool seeded = false;
  uint32_t seed = 0;
  for (int i = 3; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--seed") { seeded = true; seed = static_cast<uint32_t>(strtoul(argv[i + 1], nullptr, 10)); }
  SeededEntropy deterministic(seed);
  crypto::IEntropySource& entropy = seeded ? static_cast<crypto::IEntropySource&>(deterministic) : static_cast<crypto::IEntropySource&>(urandom);
  crypto::DrbgRandom rng(entropy);
  if (!rng.initialize()) { fprintf(stderr, "no entropy\n"); return 2; }
  TlsConfig config;
  config.random = &rng;
  config.clock = wall_clock;
  const char* plain_file = nullptr;
  std::string request = "GET / HTTP/1.0\r\n\r\n", ca_file;
  const char* name = nullptr;
  size_t want_bytes = 0, echo_bytes = 0, hold_seconds = 0;
  const char* ipps_text = nullptr;
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--name" && i + 1 < argc) name = argv[++i];
    else if (a == "--ca" && i + 1 < argc) ca_file = argv[++i];
    else if (a == "--insecure") config.verify_certificates = false;
    else if (a == "--tls12") config.allow_tls13 = false;
    else if (a == "--tls13") config.allow_tls12 = false;
    else if (a == "--alpn" && i + 1 < argc) config.alpn = argv[++i];
    else if (a == "--request" && i + 1 < argc) { request = argv[++i]; size_t p; while ((p = request.find("\\r\\n")) != std::string::npos) request.replace(p, 4, "\r\n"); }
    else if (a == "--get" && i + 1 < argc) request = std::string("GET ") + argv[++i] + " HTTP/1.0\r\n\r\n";
    else if (a == "--seed" && i + 1 < argc) ++i;
    else if (a == "--now" && i + 1 < argc) g_fixed_now = strtoull(argv[++i], nullptr, 10);
    else if (a == "--dump-plain" && i + 1 < argc) plain_file = argv[++i];
    else if (a == "--no-clock") config.clock = nullptr;
    else if (a == "--ems") config.require_extended_master_secret = true;
    else if (a == "--no-cbc") config.allow_cbc_suites = false;
    else if (a == "--echo" && i + 1 < argc) echo_bytes = strtoul(argv[++i], nullptr, 10);
    else if (a == "--ipps-print" && i + 1 < argc) ipps_text = argv[++i];
    else if (a == "--hold" && i + 1 < argc) hold_seconds = strtoul(argv[++i], nullptr, 10);
    else if (a == "--bytes" && i + 1 < argc) want_bytes = strtoul(argv[++i], nullptr, 10);
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  config.server_name = name;
  crypto::TrustStore trust(alloc);
  if (!ca_file.empty()) {
    FILE* f = fopen(ca_file.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", ca_file.c_str()); return 2; }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    if (trust.add_pem(text.data(), text.size()) == 0) { fprintf(stderr, "no certificates in %s\n", ca_file.c_str()); return 2; }
  }
  config.trust = &trust;
  if (ipps_text) {   // print a job and ask for the status over IPPS with the printer driver code, instead of raw TLS
    in_addr ia;
    if (!inet_pton(AF_INET, argv[1], &ia)) return 2;
    const uint32_t ip = ntohl(ia.s_addr);
    hardware::PrinterInfo info;
    info.model = "live test";
    hardware::IppPrinter printer(stack, Ipv4Address::make(ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255), info, alloc, "/ipp/print", static_cast<uint16_t>(atoi(argv[2])));
    printer.set_tls(config);
    printer.set_document_format("application/octet-stream");
    printer.submit(ConstByteView(reinterpret_cast<const uint8_t*>(ipps_text), strlen(ipps_text)), "live job");
    const uint64_t p0 = stack.now_ms();
    while (printer.pending_jobs() && stack.now_ms() - p0 < 30000) { printer.poll(); usleep(1000); }
    printf("job: %s, ipp status 0x%04x, http %u, job-id %u, encrypted %d\n", printer.pending_jobs() ? "NOT DELIVERED" : printer.statistics().jobs_sent ? "delivered" : "FAILED", printer.last_status(), printer.http_status(), printer.last_job_id(), (int)printer.encrypted());
    printer.request_report();
    const uint64_t r0 = stack.now_ms();
    while (printer.report_pending() && stack.now_ms() - r0 < 30000) { printer.poll(); usleep(1000); }
    const hardware::PrinterReport* rep = printer.report();
    printf("report: %s, engine %d, %u supplies, %u alerts\n", rep ? "ok" : "none", rep ? (int)rep->engine : -1, rep ? rep->supply_count : 0, rep ? rep->alert_count : 0);
    return printer.statistics().jobs_sent == 1 && rep ? 0 : 1;
  }
  SharedPointer<ITcpSocket> socket = SharedPointer<ITcpSocket>::make_as<TlsSocket>(alloc, stack, alloc, config);
  TlsSocket& tls = static_cast<TlsSocket&>(*socket);
  in_addr addr;
  if (!inet_pton(AF_INET, argv[1], &addr)) { fprintf(stderr, "host must be a dotted address\n"); return 2; }
  if (!socket->connect(Ipv4Address::make(ntohl(addr.s_addr) >> 24, (ntohl(addr.s_addr) >> 16) & 255, (ntohl(addr.s_addr) >> 8) & 255, ntohl(addr.s_addr) & 255), static_cast<uint16_t>(atoi(argv[2])))) { fprintf(stderr, "connect failed\n"); return 1; }
  const uint64_t start = stack.now_ms();
  while (!socket->is_connected() && !socket->failed() && stack.now_ms() - start < 20000) usleep(500);
  if (!socket->is_connected()) {
    printf("FAILED handshake: tls error %d, alert %u, certificate status \"%s\"\n", (int)tls.error(), tls.alert_received(), crypto::cert_status_name(tls.certificate_status()));
    return 1;
  }
  printf("handshake ok: version 0x%04x, %s, alpn \"%s\"%s%s%s\n", tls.protocol_version(), tls.cipher_suite_name(), tls.alpn_selected(), tls.extended_master_secret() ? ", ems" : "",
         tls.encrypt_then_mac() ? ", etm" : "", tls.resumed() ? ", resumed" : "");
  if (echo_bytes) {   // send a pattern, expect the same back (interleaved, as an echo server needs)
    std::vector<uint8_t> data(echo_bytes), back;
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>((i * 131 + (i >> 8)) & 0xff);
    size_t sent = 0;
    const uint64_t e0 = stack.now_ms();
    while (back.size() < data.size() && stack.now_ms() - e0 < 30000) {
      if (sent < data.size()) sent += socket->send(ConstByteView(data.data() + sent, data.size() - sent < 20000 ? data.size() - sent : 20000));
      uint8_t buf[8192];
      const uint32_t n = socket->receive(ByteView(buf, sizeof buf));
      back.insert(back.end(), buf, buf + n);
      if (socket->failed()) { printf("FAILED echo: tls error %d alert %u\n", (int)tls.error(), tls.alert_received()); return 1; }
      if (!n) usleep(100);
    }
    socket->close();
    if (back == data) { printf("echo ok: %zu bytes each way, %s\n", data.size(), tls.cipher_suite_name()); return 0; }
    printf("FAILED echo: sent %zu, got %zu bytes%s\n", sent, back.size(), back.size() >= data.size() ? " (different)" : "");
    return 1;
  }
  if (hold_seconds) {   // talk once a second and print what comes in: for key updates and other things that happen mid-connection
    std::string got;
    const uint64_t h0 = stack.now_ms();
    uint64_t next = 0;
    int line = 0;
    while (stack.now_ms() - h0 < hold_seconds * 1000) {
      if (stack.now_ms() - h0 >= next) {
        const std::string text = "hello" + std::to_string(++line) + "\n";
        socket->send(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
        next += 1000;
      }
      uint8_t buf[1024];
      const uint32_t n = socket->receive(ByteView(buf, sizeof buf));
      got.append(reinterpret_cast<char*>(buf), n);
      if (socket->failed()) { printf("FAILED while holding: tls error %d alert %u\n", (int)tls.error(), tls.alert_received()); return 1; }
      if (!n) usleep(1000);
    }
    socket->close();
    if (plain_file) { FILE* f = fopen(plain_file, "wb"); if (f) { fwrite(got.data(), 1, got.size(), f); fclose(f); } }
    for (char& ch : got) if (ch == '\n') ch = ',';
    printf("held %zu s, received [%s]\n", hold_seconds, got.c_str());
    return 0;
  }
  ConstByteView out(reinterpret_cast<const uint8_t*>(request.data()), request.size());
  while (!out.empty()) {
    const uint32_t n = socket->send(out);
    out = out.subview(n);
    if (!n) usleep(500);
    if (socket->failed()) { printf("FAILED sending\n"); return 1; }
  }
  std::string body;
  const uint64_t t0 = stack.now_ms();
  while (stack.now_ms() - t0 < 10000) {
    uint8_t buf[4096];
    const uint32_t n = socket->receive(ByteView(buf, sizeof buf));
    body.append(reinterpret_cast<char*>(buf), n);
    if (socket->failed()) { printf("FAILED receiving: tls error %d alert %u\n", (int)tls.error(), tls.alert_received()); return 1; }
    if (socket->peer_closed()) break;
    if (want_bytes && body.size() >= want_bytes) break;
    if (!n) usleep(500);
  }
  socket->close();
  if (plain_file) { FILE* f = fopen(plain_file, "wb"); if (f) { fwrite(body.data(), 1, body.size(), f); fclose(f); } }
  const size_t eol = body.find("\r\n");
  printf("received %zu bytes, first line \"%s\", end: %s\n", body.size(), body.substr(0, eol).c_str(), tls.error() == TlsError::None ? "close_notify" : "no close_notify");
  if (getenv("DUMP")) fwrite(body.data(), 1, body.size(), stdout);
  return body.empty() ? 1 : 0;
}
