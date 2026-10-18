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

// Command-line HTTP(S) client for trying HttpClient against real servers, on the machine's own sockets:
//   HttpLive URL [--ca file.pem] [--insecure] [--head] [--post TEXT] [--header "Name: value"] [--max-redirects N] [--max-body N] [--sink] [--quiet]
// Fetches the URL and prints "status NNN, N body bytes, M redirects, final URL", then the body (unless --quiet). Exit 0 if the request
// completed (whatever the status), 1 if it failed (the error, and for TLS the details, are printed), 2 for bad arguments.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include "PosixNetwork.hpp"
#include "Crypto/Crypto.hpp"
#include "Network/Http/HttpClient.hpp"

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

static uint64_t wall_clock(void*) { return static_cast<uint64_t>(time(nullptr)); }

static bool count_sink(void* context, ConstByteView data) {
  *static_cast<uint64_t*>(context) += data.size();
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: HttpLive URL [options]\n"); return 2; }
  HeapAllocator alloc;
  PosixNetworkStack stack(alloc);
  UrandomEntropy urandom;
  crypto::DrbgRandom rng(urandom);
  if (!rng.initialize()) { fprintf(stderr, "no entropy\n"); return 2; }
  TlsConfig tls;
  tls.random = &rng;
  tls.clock = wall_clock;
  crypto::TrustStore trust(alloc);
  HttpClient http(stack, alloc);
  HttpMethod method = HttpMethod::Get;
  std::string post;
  bool quiet = false, sink = false;
  uint64_t sunk = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--ca" && i + 1 < argc) {
      FILE* f = fopen(argv[++i], "rb");
      if (!f) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
      std::string text;
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
      fclose(f);
      if (trust.add_pem(text.data(), text.size()) == 0) { fprintf(stderr, "no certificates in %s\n", argv[i]); return 2; }
    } else if (a == "--insecure") {
      tls.verify_certificates = false;
    } else if (a == "--head") {
      method = HttpMethod::Head;
    } else if (a == "--post" && i + 1 < argc) {
      method = HttpMethod::Post;
      post = argv[++i];
    } else if (a == "--header" && i + 1 < argc) {
      const std::string h = argv[++i];
      const size_t colon = h.find(':');
      if (colon == std::string::npos || !http.add_header(StringView(h.data(), colon), StringView(h.data() + colon + 1, h.size() - colon - 1))) { fprintf(stderr, "bad header\n"); return 2; }
    } else if (a == "--max-redirects" && i + 1 < argc) {
      http.set_max_redirects(static_cast<uint32_t>(atoi(argv[++i])));
    } else if (a == "--max-body" && i + 1 < argc) {
      http.set_max_body(strtoull(argv[++i], nullptr, 10));
    } else if (a == "--sink") {
      sink = true;
    } else if (a == "--quiet") {
      quiet = true;
    } else {
      fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  tls.trust = &trust;
  http.set_tls(&tls);
  if (sink) http.set_body_sink(count_sink, &sunk);
  const bool ok = http.start(method, argv[1], ConstByteView(reinterpret_cast<const uint8_t*>(post.data()), post.size()), "text/plain") && http.run(30000);
  if (!ok) {
    printf("FAILED: %s", http_error_name(http.error()));
    if (http.error() == HttpError::Tls) printf(" (tls error %d, alert %u, certificate \"%s\")", (int)http.tls_error(), http.tls_alert(), crypto::cert_status_name(http.certificate_status()));
    printf("\n");
    return 1;
  }
  String final_url(alloc);
  http.url().assign_text(&final_url);
  printf("status %u, %llu body bytes, %u redirects, %s%s\n", http.status(), (unsigned long long)(sink ? sunk : http.body().size()), http.redirects(), final_url.c_str(),
         http.body_was_decoded() ? ", decoded" : "");
  if (!quiet && !sink) fwrite(http.body().data(), 1, http.body().size(), stdout);
  return 0;
}
