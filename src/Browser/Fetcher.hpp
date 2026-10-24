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

// What happens between a page asking for a resource (fetch(), XMLHttpRequest, a navigation) and the loader that speaks HTTP: the request headers a page may not
// set are dropped, cookies go out and come back (CookieJar), redirects are followed with the rules of the Fetch Standard (a 303 becomes a GET, credentials
// are not sent along to other origins), mixed content is refused, data: URLs are decoded, and the cross-origin rules apply: a request to another origin that is
// not "simple" is preceded by a preflight (OPTIONS) request, and its answer is only given to the page if the server agrees (Access-Control-Allow-Origin,
// -Credentials, -Methods, -Headers), with only the headers the standard exposes. A cross-origin request in "no-cors" mode gets an opaque answer.
#pragma once
#include "Browser/CookieJar.hpp"
#include "Browser/IPageLoader.hpp"
#include "Browser/WebUrl.hpp"

namespace browser {

struct FetchOptions {
  // Navigate: a page being opened (no cross-origin checks; Lax cookies go along). Subresource: a picture, script or style sheet the browser itself loads for a page (no
  // cross-origin checks, the bytes are not hidden; cookies go along as for any cross-site request).
  enum class Mode : uint8_t { Cors, NoCors, SameOrigin, Navigate, Subresource };
  enum class Credentials : uint8_t { Omit, SameOrigin, Include };
  enum class Redirect : uint8_t { Follow, Error, Manual };

  StringView method = "GET";
  StringView url;            // absolute
  StringView headers;        // "Name: value\r\n" lines the page set
  ConstByteView body;
  Mode mode = Mode::Cors;
  Credentials credentials = Credentials::SameOrigin;
  Redirect redirect = Redirect::Follow;
  StringView page_address;   // the address of the page asking ("" for the address bar: nothing is cross-origin then)
  uint64_t now_ms = 0;       // for cookie expiry
  uint32_t timeout_ms = 0;
  uint32_t max_redirects = 20;
  // The Referer the page's Request asked for (its referrer and referrer policy, already applied: "" sends none). Unset: the default of the browser.
  bool block_third_party_cookies = false;   // cookies of other sites than the page's are neither sent nor kept
  bool referrer_given = false;
  StringView referrer;
  StringView referrer_policy;   // "" is the default (strict-origin-when-cross-origin): a redirect to another origin cuts the address down to the origin
  bool allow_data = true;
};

// Makes the request and fills `response`. False (with response->error set) when the request failed or the rules refused it; true for any answer, whatever
// its status.
bool fetch_resource(allocators::IAllocator& allocator, IPageLoader& loader, CookieJar& cookies, const FetchOptions& options, NetResponse* response);

// Helpers for header blocks ("Name: value\r\n" lines).
// The value of the first header called `name` (any case), blanks trimmed; false if there is none.
bool header_value(StringView block, StringView name, StringView* value);
// Calls `visit` for every header, in order.
template <class F>
void for_each_header(StringView block, F&& visit) {
  StringView rest = block, line;
  while (rest.next_token('\n', &line)) {
    if (line.ends_with('\r')) line = line.without_suffix(1);
    const uint64_t colon = line.find(':');
    if (colon == StringView::npos || colon == 0) continue;
    visit(line.first(colon).trimmed(), line.substr(colon + 1).trimmed());
  }
}

// Decodes a data: URL ("data:text/plain;base64,SGk=") into its media type and bytes. False if it is not a valid one.
bool decode_data_url(allocators::IAllocator& allocator, StringView url, String* media_type, ByteBuffer* data);

}  // namespace browser
