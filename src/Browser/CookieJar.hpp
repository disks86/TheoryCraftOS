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

// The cookie store of a browser (RFC 6265bis): Set-Cookie headers and `document.cookie = ...` put cookies in, requests and `document.cookie` take them out.
// A cookie has a name, value, domain (host-only unless a Domain attribute widened it), path, expiry (Expires or Max-Age; otherwise it lasts for the session),
// and the Secure, HttpOnly and SameSite flags. Domain and path matching, the `__Secure-` and `__Host-` prefixes, SameSite=Lax by default, the limits of the
// standard (4096 bytes per cookie, 180 per host, 3000 in all; the least recently used go first) and cookies that expire while they are stored are all handled
// here, with the public suffixes of PublicSuffix.hpp (a Domain attribute naming one is refused unless it is the host itself), the Partitioned attribute, a version
// number that changes with every change (so that whoever saves the cookies knows when to), and the list of the persistent ones for saving and putting back.
//
//   CookieJar jar(allocator);
//   jar.store("sid=abc; Path=/; HttpOnly; Max-Age=3600", url, now_ms, /*from_script*/ false);
//   String header(allocator);
//   jar.header_for(url, now_ms, CookieJar::Site::SameSite, /*for_script*/ false, &header);   // "sid=abc"
#pragma once
#include "Browser/WebUrl.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace browser {

class CookieJar {
 public:
  static constexpr uint32_t kMaxCookies = 3000, kMaxPerHost = 180;
  static constexpr uint64_t kMaxCookieBytes = 4096, kMaxAttributeBytes = 1024;

  // Where a request comes from, for SameSite: the same site, a top-level navigation from another site (a link, a form), or anything else from another site
  // (an image, a script, fetch()).
  enum class Site : uint8_t { SameSite, CrossSiteNavigation, CrossSiteRequest };

  struct Cookie {
    explicit Cookie(allocators::IAllocator& a) : name(a), value(a), domain(a), path(a) {}
    String name, value, domain, path;
    bool host_only = true, secure = false, http_only = false, partitioned = false;
    uint8_t same_site = 0;            // 0 not given (Lax), 1 Lax, 2 Strict, 3 None
    int64_t expires_ms = -1;          // milliseconds since 1970; -1: a session cookie
    uint64_t created = 0, accessed = 0;
    uint64_t order = 0;               // the order they were created in
  };

  explicit CookieJar(allocators::IAllocator& allocator) : allocator_(allocator), cookies_(allocator) {}
  CookieJar(const CookieJar&) = delete;
  CookieJar& operator=(const CookieJar&) = delete;

  // Takes a Set-Cookie header (or the string given to document.cookie) received from `url`. `from_script`: it came from document.cookie, so HttpOnly cookies
  // are refused and cannot be replaced. False if the cookie was refused (it can still have removed one).
  bool store(StringView set_cookie, const WebUrl& url, uint64_t now_ms, bool from_script);
  // The "Cookie" header value for a request to `url` ("a=1; b=2"); `for_script`: what document.cookie reads (no HttpOnly ones). Empty if there are none.
  void header_for(const WebUrl& url, uint64_t now_ms, Site site, bool for_script, String* out);
  // Goes up whenever a cookie is added, replaced or removed.
  uint64_t version() const { return __atomic_load_n(&version_, __ATOMIC_ACQUIRE); }
  // Copies the cookies that outlive the session (and have not expired) for saving.
  void snapshot_persistent(uint64_t now_ms, List<Cookie>* out);
  // Puts back a saved cookie (no checks beyond the limits: it was valid when it was saved).
  bool restore(Cookie&& cookie);
  // The cookies a script of the page at `url` can see (what document.cookie reads), with their attributes, in the order of the Cookie header.
  void list_for_script(const WebUrl& url, uint64_t now_ms, List<Cookie>* out);
  // Removes the cookie with these identifying parts. True if there was one.
  bool remove_cookie(StringView name, StringView domain, StringView path, bool host_only);
  // Throws out the cookies that have expired.
  void remove_expired(uint64_t now_ms);
  void clear() {
    kernel::ScopedMutex lock(mutex_);
    cookies_.clear();
    bump();
  }
  // count() and at() are for the thread that shows the cookies; store(), header_for(), remove_expired(), remove_host() and clear() may be called from several threads
  // at once (the browser's download threads set and send cookies while the page runs).
  uint32_t count() const { return static_cast<uint32_t>(cookies_.count()); }
  const Cookie& at(uint32_t index) const { return cookies_[index]; }
  // Removes all cookies of a host (and its subdomains).
  void remove_host(StringView host);

  // Reads an HTTP date ("Wed, 21 Oct 2026 07:28:00 GMT" and the older forms) as milliseconds since 1970. False if it is not one.
  static bool parse_http_date(StringView text, int64_t* ms);
  // Formats milliseconds since 1970 as an HTTP date.
  static void format_http_date(int64_t ms, String* out);

 private:
  static bool domain_matches(StringView host, StringView domain);
  static bool path_matches(StringView request_path, StringView cookie_path);
  void enforce_limits(StringView host);
  void remove_expired_locked(uint64_t now_ms);
  void bump() { __atomic_add_fetch(&version_, 1, __ATOMIC_ACQ_REL); }

  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;   // guards cookies_ and next_order_
  List<Cookie> cookies_;
  uint64_t next_order_ = 1;
  uint64_t version_ = 0;
};

}  // namespace browser
