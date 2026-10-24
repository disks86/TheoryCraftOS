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

#include "Browser/Fetcher.hpp"

namespace browser {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool name_is(StringView name, const char* expected) { return name.equals_ignore_case(StringView(expected)); }

bool forbidden_request_header(StringView name) {
  static const char* const kNames[] = {"accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method", "connection",
                                       "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin", "referer", "set-cookie",
                                       "te", "trailer", "transfer-encoding", "upgrade", "via"};
  for (const char* n : kNames)
    if (name_is(name, n)) return true;
  return name.size() >= 6 && (name.first(6).equals_ignore_case("proxy-") || (name.size() >= 4 && name.first(4).equals_ignore_case("sec-")));
}

// The media type of a Content-Type value, without parameters, lower-cased.
void essence(StringView value, String* out) {
  const uint64_t semi = value.find(';');
  const StringView type = (semi == StringView::npos ? value : value.first(semi)).trimmed();
  for (const char c : type) out->push_back(lower(c));
}

bool safelisted_request_header(allocators::IAllocator& allocator, StringView name, StringView value) {
  if (value.size() > 128) return false;
  for (const char c : value)
    if (static_cast<uint8_t>(c) < 0x20 && c != '\t') return false;
  if (name_is(name, "accept") || name_is(name, "accept-language") || name_is(name, "content-language")) {
    for (const char c : value)
      if (c == '"' || c == '(' || c == ')' || c == ':' || c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' || c == '{' || c == '}') return false;
    return true;
  }
  if (name_is(name, "content-type")) {
    String type(allocator);
    essence(value, &type);
    return type.equals("application/x-www-form-urlencoded") || type.equals("multipart/form-data") || type.equals("text/plain");
  }
  return false;
}

bool simple_method(StringView m) { return m.equals("GET") || m.equals("HEAD") || m.equals("POST"); }

bool is_redirect_status(uint32_t s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

// Does the list (comma separated, blanks around the items) hold `item`? "*" counts when `wildcard` is allowed.
bool list_has(StringView list, StringView item, bool wildcard, bool ignore_case) {
  StringView rest = list, token;
  while (rest.next_token(',', &token)) {
    token = token.trimmed();
    if (wildcard && token.equals("*")) return true;
    if (ignore_case ? token.equals_ignore_case(item) : token.equals(item)) return true;
  }
  return false;
}

// The part of the host that identifies the site (the last two labels, or three under a two-letter country domain such as co.uk).
void site_of(StringView host, String* out) {
  const bool numeric = [&] {
    for (const char c : host)
      if (!(is_digit(c) || c == '.' || c == '[' || c == ']' || c == ':' || (c >= 'a' && c <= 'f'))) return false;
    return host.starts_with('[') || host.find(':') != StringView::npos || host.find('.') != StringView::npos;
  }();
  if (host.find('.') == StringView::npos || (numeric && (host.starts_with('[') || is_digit(host[0])))) {
    out->assign(host);
    return;
  }
  uint32_t labels = 0;
  for (const char c : host)
    if (c == '.') ++labels;
  uint64_t keep = 2;
  const uint64_t last_dot = host.rfind('.');
  const StringView tld = host.substr(last_dot + 1);
  if (tld.size() == 2 && labels >= 2) {
    const StringView without = host.first(last_dot);
    const StringView second = without.substr(without.rfind('.') + 1);   // (rfind gives npos -> +1 wraps to 0 for a bare second level)
    if (second.size() <= 3) keep = 3;
  }
  uint64_t dots_seen = 0, start = host.size();
  while (start > 0) {
    --start;
    if (host[start] == '.' && ++dots_seen == keep) {
      ++start;
      break;
    }
  }
  if (dots_seen < keep) start = 0;
  out->assign(host.substr(start));
}

bool same_site(allocators::IAllocator& allocator, const WebUrl& a, const WebUrl& b) {
  if (!a.scheme.view().equals(b.scheme.view())) return false;
  String x(allocator), y(allocator);
  site_of(a.host.view(), &x);
  site_of(b.host.view(), &y);
  return x.view().equals(y.view());
}

bool is_local_host(StringView host) { return host.equals("localhost") || host.equals("127.0.0.1") || host.equals("[::1]"); }

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

bool base64_decode(StringView text, ByteBuffer* out) {
  uint32_t bits = 0, count = 0;
  for (const char c : text) {
    if (c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    const int v = base64_value(c);
    if (v < 0) return false;
    bits = (bits << 6) | static_cast<uint32_t>(v);
    count += 6;
    if (count >= 8) {
      count -= 8;
      const uint8_t byte = static_cast<uint8_t>((bits >> count) & 0xFF);
      if (!out->append(ConstByteView(&byte, 1))) return false;
    }
  }
  return true;
}

// Collects the headers a page may set. `unsafe` gets the names (lower case, comma separated) of those that need a preflight.
void sanitize_headers(allocators::IAllocator& allocator, StringView block, bool no_cors, String* out, String* unsafe) {
  for_each_header(block, [&](StringView name, StringView value) {
    if (forbidden_request_header(name)) return;
    const bool safe = safelisted_request_header(allocator, name, value);
    if (no_cors && !safe) return;
    out->append(name);
    out->append(": ");
    out->append(value);
    out->append("\r\n");
    if (!safe) {
      String lowered(allocator);
      for (const char c : name) lowered.push_back(lower(c));
      if (!list_has(unsafe->view(), lowered.view(), false, false)) {
        if (!unsafe->empty()) unsafe->push_back(',');
        unsafe->append(lowered.view());
      }
    }
  });
}

}  // namespace

bool header_value(StringView block, StringView name, StringView* value) {
  bool found = false;
  for_each_header(block, [&](StringView n, StringView v) {
    if (!found && n.equals_ignore_case(name)) {
      *value = v;
      found = true;
    }
  });
  return found;
}

bool decode_data_url(allocators::IAllocator& allocator, StringView url, String* media_type, ByteBuffer* data) {
  if (!url.starts_with("data:") && !url.starts_with("DATA:")) return false;
  StringView rest = url.without_prefix(5);
  const uint64_t hash = rest.find('#');
  if (hash != StringView::npos) rest = rest.first(hash);
  const uint64_t comma = rest.find(',');
  if (comma == StringView::npos) return false;
  StringView meta = rest.first(comma).trimmed();
  const StringView body = rest.substr(comma + 1);
  bool base64 = false;
  const uint64_t last_semi = meta.rfind(';');
  if (last_semi != StringView::npos && meta.substr(last_semi + 1).trimmed().equals_ignore_case("base64")) {
    base64 = true;
    meta = meta.first(last_semi);
  }
  media_type->clear();
  if (meta.empty() || meta.starts_with(';')) media_type->assign("text/plain;charset=US-ASCII");
  else media_type->assign(meta);
  data->clear();
  String decoded(allocator);
  if (!WebUrl::percent_decode(body, &decoded)) return false;
  if (base64) return base64_decode(decoded.view(), data);
  return data->append(decoded.view().bytes());
}

bool fetch_resource(allocators::IAllocator& allocator, IPageLoader& loader, CookieJar& cookies, const FetchOptions& opt, NetResponse* response) {
  response->status = 0;
  response->reason.clear();
  response->url.clear();
  response->headers.clear();
  response->error.clear();
  response->body.clear();
  response->redirected = false;
  response->opaque = false;
  auto fail = [&](const char* message) {
    response->error.assign(message);
    return false;
  };

  if (opt.url.starts_with("data:") || opt.url.starts_with("DATA:")) {
    if (!opt.allow_data) return fail("Unsupported URL scheme");
    String media(allocator);
    if (!decode_data_url(allocator, opt.url, &media, &response->body)) return fail("Failed to parse data URL");
    response->status = 200;
    response->reason.assign("OK");
    response->url.assign(opt.url);
    response->headers.assign("Content-Type: ");
    response->headers.append(media.view());
    response->headers.append("\r\n");
    return true;
  }

  WebUrl target(allocator);
  if (!WebUrl::parse(allocator, opt.url, nullptr, &target)) return fail("Failed to parse URL");
  if (!target.scheme.equals("http") && !target.scheme.equals("https")) return fail("Unsupported URL scheme");

  WebUrl page(allocator);
  const bool has_page = !opt.page_address.empty() && WebUrl::parse(allocator, opt.page_address, nullptr, &page) && page.is_special() && !page.is_file();
  const bool local_page = !opt.page_address.empty() && !has_page;   // (a file on this machine: its origin is opaque, so everything is cross-origin)
  String page_origin(allocator);
  if (has_page) page.origin(&page_origin);
  else if (local_page) page_origin.assign("null");
  const bool navigate = opt.mode == FetchOptions::Mode::Navigate;
  const bool unchecked = navigate || opt.mode == FetchOptions::Mode::Subresource;   // (the browser's own loads: no cross-origin rules)

  auto is_cross = [&](const WebUrl& u) {
    if (unchecked || (!has_page && !local_page)) return false;
    if (local_page) return true;
    return !page.same_origin(u);
  };

  if (has_page && page.scheme.equals("https") && target.scheme.equals("http") && !is_local_host(target.host.view()) && !unchecked) return fail("Mixed Content: the page is secure but the request is not");

  String method(allocator);
  for (const char c : opt.method) method.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  if (method.empty()) method.assign("GET");
  for (const char c : method.view())
    if (!is_alpha(c)) return fail("Invalid method");
  if (method.equals("CONNECT") || method.equals("TRACE") || method.equals("TRACK")) return fail("Forbidden method");
  if ((method.equals("GET") || method.equals("HEAD")) && !opt.body.empty()) return fail("Request with GET/HEAD method cannot have body.");

  const bool cross = is_cross(target);
  const bool no_cors = opt.mode == FetchOptions::Mode::NoCors && cross;
  if (cross && opt.mode == FetchOptions::Mode::SameOrigin) return fail("Failed to fetch: the request is cross-origin and the mode is same-origin");
  if (no_cors && !simple_method(method.view())) return fail("Failed to fetch: no-cors requests may only use GET, HEAD or POST");

  String user_headers(allocator), unsafe(allocator);
  sanitize_headers(allocator, opt.headers, no_cors, &user_headers, &unsafe);
  const bool credentials_sent = opt.credentials == FetchOptions::Credentials::Include;

  // preflight
  if (cross && opt.mode == FetchOptions::Mode::Cors && (!simple_method(method.view()) || !unsafe.empty())) {
    String headers(allocator);
    headers.append("Origin: ");
    headers.append(page_origin.view());
    headers.append("\r\nAccess-Control-Request-Method: ");
    headers.append(method.view());
    headers.append("\r\n");
    if (!unsafe.empty()) {
      headers.append("Access-Control-Request-Headers: ");
      headers.append(unsafe.view());
      headers.append("\r\n");
    }
    String target_text(allocator);
    target.serialize(&target_text, false);
    NetRequest preflight;
    preflight.method = "OPTIONS";
    preflight.url = target_text.view();
    preflight.headers = headers.view();
    preflight.timeout_ms = opt.timeout_ms;
    NetResponse answer(allocator);
    if (!loader.exchange(preflight, &answer)) return fail(answer.error.empty() ? "Failed to fetch" : answer.error.c_str());
    StringView allow_origin, allow_credentials, allow_methods, allow_headers;
    header_value(answer.headers.view(), "Access-Control-Allow-Origin", &allow_origin);
    header_value(answer.headers.view(), "Access-Control-Allow-Credentials", &allow_credentials);
    header_value(answer.headers.view(), "Access-Control-Allow-Methods", &allow_methods);
    header_value(answer.headers.view(), "Access-Control-Allow-Headers", &allow_headers);
    if (answer.status < 200 || answer.status > 299) return fail("Response to preflight request doesn't pass access control check: it does not have HTTP ok status");
    if (!(allow_origin.equals(page_origin.view()) || (allow_origin.equals("*") && !credentials_sent)))
      return fail("Response to preflight request doesn't pass access control check: no matching Access-Control-Allow-Origin header");
    if (credentials_sent && !allow_credentials.equals("true")) return fail("Response to preflight request doesn't pass access control check: Access-Control-Allow-Credentials is not 'true'");
    if (!simple_method(method.view()) && !list_has(allow_methods, method.view(), !credentials_sent, false))
      return fail("Response to preflight request doesn't pass access control check: the method is not allowed by Access-Control-Allow-Methods");
    StringView rest = unsafe.view(), name;
    while (rest.next_token(',', &name)) {
      if (!list_has(allow_headers, name, !credentials_sent && !name.equals("authorization"), true))
        return fail("Response to preflight request doesn't pass access control check: a request header is not allowed by Access-Control-Allow-Headers");
    }
  }

  // the request itself, and the redirects it meets
  WebUrl current(allocator);
  current = static_cast<WebUrl&&>(target);
  ConstByteView body = opt.body;
  bool keep_body = true;
  for (uint32_t hop = 0;; ++hop) {
    String headers(allocator);
    headers.append(user_headers.view());
    const bool hop_cross = is_cross(current);
    if (has_page || local_page) {
      if (hop_cross || !(method.equals("GET") || method.equals("HEAD"))) {
        headers.append("Origin: ");
        headers.append(page_origin.view());
        headers.append("\r\n");
      }
      if (opt.referrer_given) {   // what the page's Request said
        StringView value = opt.referrer;
        if (!value.empty() && hop_cross && (opt.referrer_policy.empty() || opt.referrer_policy.equals("strict-origin-when-cross-origin") || opt.referrer_policy.equals("origin-when-cross-origin"))) {
          const uint64_t scheme_end = value.find("://");
          const uint64_t path = scheme_end == StringView::npos ? StringView::npos : value.find('/', scheme_end + 3);
          if (path != StringView::npos) value = value.first(path + 1);   // (the origin and its slash)
        }
        if (!value.empty() && has_page && !(page.scheme.equals("https") && current.scheme.equals("http") && !opt.referrer_policy.equals("unsafe-url"))) {
          headers.append("Referer: ");
          headers.append(value);
          headers.append("\r\n");
        }
      } else if (has_page && !(page.scheme.equals("https") && current.scheme.equals("http"))) {   // the referrer: the whole address within the origin, the origin alone otherwise
        headers.append("Referer: ");
        if (page.same_origin(current)) {
          WebUrl bare(allocator);
          bare = WebUrl(allocator);
          String text(allocator);
          page.serialize(&text, false);
          if (!page.username.empty() || !page.password.empty()) {
            if (WebUrl::parse(allocator, text.view(), nullptr, &bare)) {
              bare.username.clear();
              bare.password.clear();
              text.clear();
              bare.serialize(&text, false);
            }
          }
          headers.append(text.view());
        } else {
          headers.append(page_origin.view());
          headers.push_back('/');
        }
        headers.append("\r\n");
      }
    }
    const bool third_party = opt.block_third_party_cookies && has_page && !navigate && !same_site(allocator, page, current);
    const bool send_cookies = !third_party && (navigate || opt.mode == FetchOptions::Mode::Subresource || opt.credentials == FetchOptions::Credentials::Include || (opt.credentials == FetchOptions::Credentials::SameOrigin && !hop_cross));
    if (send_cookies) {
      CookieJar::Site site = CookieJar::Site::SameSite;
      if (has_page && !same_site(allocator, page, current)) site = navigate && method.equals("GET") ? CookieJar::Site::CrossSiteNavigation : CookieJar::Site::CrossSiteRequest;
      else if (local_page) site = CookieJar::Site::CrossSiteRequest;
      String cookie(allocator);
      cookies.header_for(current, opt.now_ms, site, false, &cookie);
      if (!cookie.empty()) {
        headers.append("Cookie: ");
        headers.append(cookie.view());
        headers.append("\r\n");
      }
    }
    String url_text(allocator);
    current.serialize(&url_text, false);
    NetRequest request;
    request.method = method.view();
    request.url = url_text.view();
    request.headers = headers.view();
    request.body = keep_body ? body : ConstByteView();
    request.timeout_ms = opt.timeout_ms;
    NetResponse answer(allocator);
    if (!loader.exchange(request, &answer)) return fail(answer.error.empty() ? "Failed to fetch" : answer.error.c_str());

    if (send_cookies) {
      for_each_header(answer.headers.view(), [&](StringView name, StringView value) {
        if (name.equals_ignore_case("set-cookie")) cookies.store(value, current, opt.now_ms, false);
      });
    }
    if (!is_redirect_status(answer.status) && !answer.url.empty() && !answer.url.view().equals(url_text.view())) {   // a loader that followed a redirect itself
      WebUrl landed(allocator);
      String landed_text(allocator);
      if (WebUrl::parse(allocator, answer.url.view(), nullptr, &landed)) {
        landed.serialize(&landed_text, false);
        if (!landed_text.view().equals(url_text.view())) {
          current = static_cast<WebUrl&&>(landed);
          response->redirected = true;
        }
      }
    }
    StringView location;
    if (is_redirect_status(answer.status) && header_value(answer.headers.view(), "Location", &location) && !location.empty()) {
      if (opt.redirect == FetchOptions::Redirect::Error) return fail("Failed to fetch: the request was redirected");
      if (opt.redirect == FetchOptions::Redirect::Follow) {
        if (hop >= opt.max_redirects) return fail("Failed to fetch: too many redirects");
        WebUrl next(allocator);
        if (!WebUrl::parse(allocator, location, &current, &next)) return fail("Failed to fetch: the redirect address is not valid");
        if (!next.scheme.equals("http") && !next.scheme.equals("https")) return fail("Failed to fetch: the redirect is to an unsupported scheme");
        if (current.scheme.equals("https") && next.scheme.equals("http") && !is_local_host(next.host.view())) return fail("Failed to fetch: redirect from https to http");
        if (has_page && page.scheme.equals("https") && next.scheme.equals("http") && !is_local_host(next.host.view()) && !unchecked) return fail("Mixed Content: redirected to an insecure address");
        if (answer.status == 303 ? !method.equals("HEAD") : ((answer.status == 301 || answer.status == 302) && method.equals("POST"))) {
          method.assign("GET");
          keep_body = false;
        }
        if (!current.same_origin(next)) {   // credentials do not follow a redirect to another origin
          String kept(allocator);
          for_each_header(user_headers.view(), [&](StringView name, StringView value) {
            if (name.equals_ignore_case("authorization")) return;
            kept.append(name);
            kept.append(": ");
            kept.append(value);
            kept.append("\r\n");
          });
          user_headers = static_cast<String&&>(kept);
        }
        current = static_cast<WebUrl&&>(next);
        response->redirected = true;
        continue;
      }
    }

    // the answer
    const bool final_cross = is_cross(current);
    if (final_cross && opt.mode == FetchOptions::Mode::Cors) {
      StringView allow_origin, allow_credentials;
      header_value(answer.headers.view(), "Access-Control-Allow-Origin", &allow_origin);
      header_value(answer.headers.view(), "Access-Control-Allow-Credentials", &allow_credentials);
      if (!(allow_origin.equals(page_origin.view()) || (allow_origin.equals("*") && !credentials_sent)))
        return fail(allow_origin.empty() ? "Access to fetch has been blocked by CORS policy: no 'Access-Control-Allow-Origin' header is present on the requested resource"
                                         : "Access to fetch has been blocked by CORS policy: the 'Access-Control-Allow-Origin' header does not match the origin of the page");
      if (credentials_sent && !allow_credentials.equals("true")) return fail("Access to fetch has been blocked by CORS policy: credentials mode is 'include' but 'Access-Control-Allow-Credentials' is not 'true'");
      // only some headers are shown to the page
      StringView exposed;
      header_value(answer.headers.view(), "Access-Control-Expose-Headers", &exposed);
      static const char* const kSafe[] = {"cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma"};
      String shown(allocator);
      for_each_header(answer.headers.view(), [&](StringView name, StringView value) {
        bool ok = list_has(exposed, name, !credentials_sent, true);
        for (const char* s : kSafe) ok = ok || name_is(name, s);
        if (!ok) return;
        shown.append(name);
        shown.append(": ");
        shown.append(value);
        shown.append("\r\n");
      });
      answer.headers = static_cast<String&&>(shown);
    } else if (final_cross && opt.mode == FetchOptions::Mode::NoCors) {
      response->opaque = true;
      response->status = 0;
      response->url.clear();
      return true;
    }
    // (a page never sees Set-Cookie)
    String visible(allocator);
    for_each_header(answer.headers.view(), [&](StringView name, StringView value) {
      if (name_is(name, "set-cookie") || name_is(name, "set-cookie2")) return;
      visible.append(name);
      visible.append(": ");
      visible.append(value);
      visible.append("\r\n");
    });
    response->status = answer.status;
    response->reason.assign(answer.reason.view());
    response->headers = static_cast<String&&>(visible);
    response->body.clear();
    if (!response->body.append(answer.body.view())) return fail("out of memory");
    current.serialize(&response->url, false);
    StringView clear;
    if (header_value(answer.headers.view(), "Clear-Site-Data", &clear) && (current.scheme.equals("https") || current.host.equals("localhost") || current.host.equals("127.0.0.1"))) {
      // "cookies", "storage", "cache" or "*" (quoted, separated by commas)
      StringView rest = clear, token;
      while (rest.next_token(',', &token)) {
        token = token.trimmed();
        if (token.starts_with('"') && token.ends_with('"') && token.size() >= 2) token = token.substr(1, token.size() - 2);
        if (token.equals("cookies") || token.equals("*")) response->clear_site_data |= 1;
        if (token.equals("storage") || token.equals("*")) response->clear_site_data |= 2;
        if (token.equals("cache") || token.equals("*")) response->clear_site_data |= 4;
      }
      if (response->clear_site_data & 1) cookies.remove_host(current.host.view());
    }
    return true;
  }
}

}  // namespace browser
