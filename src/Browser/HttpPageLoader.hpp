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

// The system's page loader: HTTP and HTTPS over the machine's network stacks (see network::HttpClient). A request goes out of the first interface
// that has an address. https:// needs a TLS configuration with a trust store (set_tls()); without one an https address fails, saying so. Servers that announce
// HTTP/3 are reached over QUIC from then on, and TLS sessions are resumed from tickets (both are remembered by the loader).
#pragma once
#include "Browser/IPageLoader.hpp"
#include "Network/Http/HttpClient.hpp"
#include "Network/NetworkStackSet.hpp"
#include "Browser/StackRtcNetwork.hpp"

namespace browser {

class HttpPageLoader final : public IPageLoader {
 public:
  static constexpr uint64_t kMaxPageBytes = 4u << 20, kMaxFetchBytes = 16u << 20;

  HttpPageLoader(network::NetworkStackSet& stacks, allocators::IAllocator& allocator) : stacks_(stacks), allocator_(allocator), sessions_(allocator), rtc_network_(stacks), alt_svc_(allocator) {}
  rtc::IRtcNetwork* rtc_network() override { return &rtc_network_; }

  // Lets https:// addresses through (copied; what it points to must outlive the loader). nullptr: plain http only.
  void set_tls(const network::TlsConfig* config) {
    has_tls_ = config != nullptr;
    if (config) tls_ = *config;
  }
  void set_timeout_ms(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }

  bool exchange(const NetRequest& request, NetResponse* response) override;
  UniquePointer<IWebSocket> open_websocket(StringView url, StringView protocols, StringView origin, String* error) override;
  bool fetch(StringView address, PageResult* result) override;
  bool post(StringView address, StringView content_type, StringView body, PageResult* result) override;

 private:
  bool send(network::HttpMethod method, StringView address, StringView content_type, StringView body, PageResult* result);
  network::NetworkStackSet& stacks_;
  allocators::IAllocator& allocator_;
  network::TlsConfig tls_;
  bool has_tls_ = false;
  uint32_t timeout_ms_ = 30000;
  network::TlsSessionCache sessions_;   // TLS tickets: later requests to the same server resume the session
  rtc::StackRtcNetwork rtc_network_;
  network::AltSvcCache alt_svc_;   // servers that announced HTTP/3: later requests to them go over QUIC (see HttpClient)
};

}  // namespace browser
