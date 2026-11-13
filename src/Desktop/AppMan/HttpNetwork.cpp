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

#include "Desktop/AppMan/HttpNetwork.hpp"
#include "Network/Http/HttpClient.hpp"
#include "Network/NetworkStackSet.hpp"

namespace desktop::appman {

namespace {

constexpr uint32_t kFetchTimeoutMs = 60000, kConnectTimeoutMs = 20000, kStallTimeoutMs = 60000;
constexpr uint32_t kDownloadTimeoutMs = 0xFFFFFFFFu;      // (no limit on the whole download: the stall timeout ends a dead one)

// The first interface that can reach beyond itself (an IPv4 address, or a global IPv6 one with a router).
network::INetworkStack* connected_stack() {
  network::NetworkStackSet* stacks = network::system_network_stacks();
  if (!stacks) return nullptr;
  for (uint32_t i = 0; i < stacks->count(); ++i) {
    network::INetworkStack* stack = stacks->at(i);
    if (!stack) continue;
    const network::Ipv6Configuration c6 = stack->configuration6();
    if (!stack->configuration().address.is_unspecified() || (c6.global_ready && !c6.router.is_unspecified())) return stack;
  }
  return nullptr;
}

bool prepare(network::HttpClient& http, const network::TlsConfig* tls) {
  if (tls) http.set_tls(tls);
  http.set_max_redirects(10);      // (GitHub's release links go through two)
  http.set_timeouts(kConnectTimeoutMs, kStallTimeoutMs);
  return http.add_header("User-Agent", "TheoryCraftOS-AppMan/1.0");
}

struct Download {
  network::HttpClient* http;
  INetwork::Sink sink;
  void* context;
  uint64_t* total;
  bool started = false;
};

bool forward(void* context, ConstByteView data) {
  Download& d = *static_cast<Download*>(context);
  if (!d.started) {      // (the head is in: the size, if the server said it)
    d.started = true;
    const uint64_t length = d.http->content_length();
    *d.total = length == ~uint64_t{0} ? 0 : length;
  }
  return d.sink(d.context, data);
}

}  // namespace

bool HttpNetwork::fetch(StringView url, uint64_t max_bytes, String* body) {
  network::INetworkStack* stack = connected_stack();
  if (!stack) {
    set_error("no network connection");
    return false;
  }
  network::HttpClient http(*stack, allocator_);
  http.set_max_body(max_bytes);
  if (!prepare(http, tls_) || !http.start(network::HttpMethod::Get, url) || !http.run(kFetchTimeoutMs)) {
    set_error(network::http_error_name(http.error()));
    return false;
  }
  if (http.status() < 200 || http.status() >= 300) {
    set_error("the server refused");
    return false;
  }
  set_error("");
  return body->assign(StringView(http.body()));
}

bool HttpNetwork::download(StringView url, Sink sink, void* context, uint64_t* total) {
  *total = 0;
  network::INetworkStack* stack = connected_stack();
  if (!stack) {
    set_error("no network connection");
    return false;
  }
  network::HttpClient http(*stack, allocator_);
  Download d{&http, sink, context, total};
  http.set_body_sink(&forward, &d);
  if (!prepare(http, tls_) || !http.start(network::HttpMethod::Get, url) || !http.run(kDownloadTimeoutMs)) {
    set_error(network::http_error_name(http.error()));
    return false;
  }
  if (http.status() < 200 || http.status() >= 300) {
    set_error("the server refused");
    return false;
  }
  set_error("");
  return true;
}

}  // namespace desktop::appman
