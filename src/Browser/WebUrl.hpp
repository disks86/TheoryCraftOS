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

// URLs as the web platform defines them (the WHATWG URL Standard): what `new URL(text, base)` does, what the address of a page splits into, and what origin,
// cookie and cross-origin checks compare. WebUrl::parse() reads the special schemes (http, https, ws, wss, ftp, file) with their authority, host (domain names
// lower-cased and punycoded, IPv4 in any notation, IPv6), port, path with its "." and ".." segments, query and fragment, and the other schemes (data:,
// mailto:, javascript:, blob: ...) with an opaque path; percent-encoding follows the standard's encode sets. The setters change one part the way the
// standard's setters do (and leave the URL alone when the new value is not acceptable). Not here: the IDNA mapping (host names are not case-folded beyond
// ASCII), public suffixes.
#pragma once
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace browser {

class WebUrl {
 public:
  explicit WebUrl(allocators::IAllocator& allocator)
      : scheme(allocator), username(allocator), password(allocator), host(allocator), path(allocator), query(allocator), fragment(allocator), allocator_(&allocator) {}

  enum class Part : uint8_t { Href, Protocol, Username, Password, Host, Hostname, Port, Pathname, Search, Hash };

  // Reads `input`, relative to `base` when it is not absolute. False if it is not a URL.
  static bool parse(allocators::IAllocator& allocator, StringView input, const WebUrl* base, WebUrl* out);
  static bool can_parse(allocators::IAllocator& allocator, StringView input, const WebUrl* base);

  bool is_special() const;
  bool is_file() const { return scheme.equals("file"); }
  uint16_t default_port() const;   // 0 for schemes without one

  // The URL as text (the `href`); with the fragment left out for comparisons and requests.
  void serialize(String* out, bool with_fragment = true) const;
  // The origin ("https://example.com:8443"), or "null" for the schemes that have none.
  void origin(String* out) const;
  // Are the two URLs of the same origin? (Opaque origins are never equal to anything.)
  bool same_origin(const WebUrl& other) const;

  // The parts as the URL API names them ("https:", "user", "example.com:8443", "/a/b", "?q=1", "#top" ...).
  void get(Part part, String* out) const;
  // Changes one part. False (nothing changed) if the value cannot be used there.
  bool set(Part part, StringView value);

  // The query as the list of name / value pairs of application/x-www-form-urlencoded data, and back.
  static void form_encode(StringView text, String* out);          // "a b&c" -> "a+b%26c"
  static bool form_decode(StringView text, String* out);          // "a+b%26c" -> "a b&c"
  static bool percent_decode(StringView text, String* out);

  String scheme;                // "https" (lower case, no colon)
  String username, password;
  bool has_host = false;        // there is an authority part ("//...")
  String host;                  // serialized: "example.com", "xn--...", "1.2.3.4", "[::1]", "" (file:///)
  int32_t port = -1;            // -1: none
  bool opaque_path = false;     // "mailto:a@b", "data:text/plain,x"
  String path;                  // "/a/b/c" (for a path with segments), or the whole opaque path
  bool has_query = false;
  String query;                 // without the "?"
  bool has_fragment = false;
  String fragment;              // without the "#"

 private:
  allocators::IAllocator* allocator_;
};

}  // namespace browser
