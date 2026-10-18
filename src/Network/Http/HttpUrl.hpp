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

// http:// and https:// URLs for the HTTP client: taking one apart, and working out where a redirect's Location leads.
#pragma once
#include "Utility/String.hpp"

namespace network {

struct HttpUrl {
  explicit HttpUrl(allocators::IAllocator& allocator) : allocator_(&allocator), host(allocator), path(allocator) {}
  allocators::IAllocator& allocator() const { return *allocator_; }

  HttpUrl(HttpUrl&&) = default;
  HttpUrl& operator=(HttpUrl&&) = default;

  allocators::IAllocator* allocator_;
  bool https = false;
  String host;            // as written, without a port; a dotted-quad address is allowed (IPv6 literals are not)
  uint16_t port = 80;     // the default for the scheme if the URL has none
  String path;            // the path and the query, always starting with '/'; the fragment is dropped

  bool default_port() const { return port == (https ? 443 : 80); }
  // "http://host[:port]/path" again.
  bool assign_text(String* out) const;
  bool same_origin(const HttpUrl& other) const { return https == other.https && port == other.port && host.view().equals_ignore_case(other.host.view()); }
};

// Parses an absolute http or https URL. False for any other scheme, an empty host, a port outside 1..65535, user information
// ("user:pass@"), an IPv6 literal, a host over 253 characters, or characters that cannot be sent in a request line (controls, blanks,
// anything but printable ASCII). Memory shortage also fails.
bool parse_http_url(StringView text, HttpUrl* out);

// Resolves a reference (a redirect's Location) against the URL it came with (RFC 3986 section 5): absolute URLs, "//host/path",
// "/path", "?query" and "relative/path", with "." and ".." segments removed. False if the result is not a valid http(s) URL.
bool resolve_http_reference(const HttpUrl& base, StringView reference, HttpUrl* out);

}  // namespace network
