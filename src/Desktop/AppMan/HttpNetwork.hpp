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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Desktop/AppMan/AppManInstaller.hpp"
#include "Network/Tls/TlsSocket.hpp"

namespace desktop::appman {

// The App Manager's way onto the network: the kernel's HTTP client over the first network interface that has an address (network::system_network_stacks()),
// with the trust store of the browser for https. Answers are fetched whole (at most what the caller allows); downloads are streamed, with no size limit,
// and end when the connection has been idle for a minute. GitHub's API refuses requests without a User-Agent: this one says "TheoryCraftOS-AppMan".
// Each call is a request of its own; calls from several threads at once are fine.
class HttpNetwork final : public INetwork {
 public:
  // `tls` (nullptr: https cannot be used) must outlive this.
  HttpNetwork(allocators::IAllocator& allocator, const network::TlsConfig* tls) : allocator_(allocator), tls_(tls) {}

  bool fetch(StringView url, uint64_t max_bytes, String* body) override;
  bool download(StringView url, Sink sink, void* context, uint64_t* total) override;
  // Why the last call failed ("" if it did not), for the status line.
  StringView last_error() const { return __atomic_load_n(&last_error_, __ATOMIC_RELAXED); }

 private:
  void set_error(const char* text) { __atomic_store_n(&last_error_, text, __ATOMIC_RELAXED); }

  allocators::IAllocator& allocator_;
  const network::TlsConfig* tls_;
  const char* last_error_ = "";
};

}  // namespace desktop::appman
