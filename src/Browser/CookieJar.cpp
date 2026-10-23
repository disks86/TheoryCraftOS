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

#include "Browser/CookieJar.hpp"
#include "Utility/Move.hpp"

namespace browser {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

void append_lower(StringView text, String* out) {
  for (const char c : text) out->push_back(lower(c));
}

bool is_ip_address(StringView host) {
  if (host.starts_with('[')) return true;
  uint32_t dots = 0;
  for (const char c : host) {
    if (c == '.') ++dots;
    else if (!is_digit(c)) return false;
  }
  return dots == 3;
}

// Days from 1970-01-01 to the civil date (proleptic Gregorian).
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t* y, int64_t* m, int64_t* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  *y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y += *m <= 2;
}

const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

bool secure_url(const WebUrl& url) {
  return url.scheme.equals("https") || url.scheme.equals("wss") || url.host.equals("localhost") || url.host.equals("127.0.0.1") || url.host.equals("[::1]");
}

// The default path of a cookie: the request path up to its last slash.
void default_path(const WebUrl& url, String* out) {
  const StringView path = url.path.view();
  const uint64_t slash = path.rfind('/');
  if (path.empty() || path[0] != '/' || slash == 0 || slash == StringView::npos) out->assign("/");
  else out->assign(path.first(slash));
}

bool parse_integer(StringView text, int64_t* out) {
  text = text.trimmed();
  bool negative = false;
  if (text.starts_with('-')) {
    negative = true;
    text = text.without_prefix(1);
  }
  if (text.empty()) return false;
  int64_t v = 0;
  for (const char c : text) {
    if (!is_digit(c)) return false;
    if (v > (INT64_MAX - 9) / 10) v = (INT64_MAX - 9) / 10;   // (huge values just saturate)
    else v = v * 10 + (c - '0');
  }
  *out = negative ? -v : v;
  return true;
}

}  // namespace

bool CookieJar::parse_http_date(StringView text, int64_t* ms) {
  int hour = -1, minute = 0, second = 0, day = 0, month = -1;
  int64_t year = -1;
  bool have_time = false, have_day = false, have_year = false;
  uint64_t i = 0;
  auto delimiter = [](char c) { return c == '\t' || (c >= ' ' && c <= '/') || (c >= ';' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'); };
  while (i < text.size()) {
    while (i < text.size() && delimiter(text[i])) ++i;
    uint64_t end = i;
    while (end < text.size() && !delimiter(text[end])) ++end;
    const StringView token = text.substr(i, end - i);
    i = end;
    if (token.empty()) continue;
    if (!have_time && token.find(':') != StringView::npos) {
      StringView rest = token, a, b, c;
      if (rest.split_once(':', &a, &rest) && rest.split_once(':', &b, &c)) {
        int64_t h, m, s;
        if (parse_integer(a, &h) && parse_integer(b, &m) && parse_integer(c, &s) && a.size() <= 2 && b.size() <= 2) {
          hour = static_cast<int>(h);
          minute = static_cast<int>(m);
          second = static_cast<int>(s);
          have_time = true;
          continue;
        }
      }
    }
    int64_t number;
    if (!have_day && token.size() <= 2 && parse_integer(token, &number)) {
      day = static_cast<int>(number);
      have_day = true;
      continue;
    }
    if (month < 0 && token.size() >= 3) {
      bool found = false;
      for (int m = 0; m < 12 && !found; ++m) {
        if (token.first(3).equals_ignore_case(kMonths[m])) {
          month = m;
          found = true;
        }
      }
      if (found) continue;
    }
    if (!have_year && token.size() >= 2 && token.size() <= 4 && parse_integer(token, &number)) {
      year = number;
      have_year = true;
    }
  }
  if (!have_time || !have_day || month < 0 || !have_year) return false;
  if (year >= 70 && year <= 99) year += 1900;
  else if (year >= 0 && year <= 69) year += 2000;
  if (day < 1 || day > 31 || year < 1601 || hour > 23 || minute > 59 || second > 59) return false;
  *ms = (days_from_civil(year, month + 1, day) * 86400 + hour * 3600 + minute * 60 + second) * 1000;
  return true;
}

void CookieJar::format_http_date(int64_t ms, String* out) {
  int64_t seconds = ms / 1000;
  if (ms < 0 && ms % 1000 != 0) --seconds;
  int64_t days = seconds / 86400;
  int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  int64_t y, m, d;
  civil_from_days(days, &y, &m, &d);
  const int64_t weekday = ((days % 7) + 11) % 7;   // (1970-01-01 was a Thursday)
  out->append(kDays[weekday]);
  out->append(", ");
  if (d < 10) out->push_back('0');
  out->append_uint(static_cast<uint64_t>(d));
  out->push_back(' ');
  out->append(kMonths[m - 1]);
  out->push_back(' ');
  out->append_uint(static_cast<uint64_t>(y));
  out->push_back(' ');
  const int64_t parts[3] = {rest / 3600, (rest / 60) % 60, rest % 60};
  for (int k = 0; k < 3; ++k) {
    if (parts[k] < 10) out->push_back('0');
    out->append_uint(static_cast<uint64_t>(parts[k]));
    if (k < 2) out->push_back(':');
  }
  out->append(" GMT");
}

bool CookieJar::domain_matches(StringView host, StringView domain) {
  if (host.equals(domain)) return true;
  return host.size() > domain.size() && host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.' && !is_ip_address(host);
}

bool CookieJar::path_matches(StringView request_path, StringView cookie_path) {
  if (request_path.equals(cookie_path)) return true;
  if (!request_path.starts_with(cookie_path)) return false;
  return cookie_path.ends_with('/') || request_path[cookie_path.size()] == '/';
}

bool CookieJar::store(StringView set_cookie, const WebUrl& url, uint64_t now_ms, bool from_script) {
  kernel::ScopedMutex lock(mutex_);
  if (set_cookie.size() > kMaxCookieBytes + kMaxAttributeBytes * 4) return false;
  StringView rest = set_cookie, pair;
  const uint64_t semi = set_cookie.find(';');
  if (semi == StringView::npos) {
    pair = set_cookie;
    rest = StringView();
  } else {
    pair = set_cookie.first(semi);
    rest = set_cookie.substr(semi + 1);
  }
  Cookie cookie(allocator_);
  const uint64_t eq = pair.find('=');
  if (eq == StringView::npos) {
    cookie.value.assign(pair.trimmed());   // (a cookie with no "=" has no name)
  } else {
    cookie.name.assign(pair.first(eq).trimmed());
    cookie.value.assign(pair.substr(eq + 1).trimmed());
  }
  if (cookie.name.empty() && cookie.value.empty()) return false;
  if (cookie.name.size() + cookie.value.size() > kMaxCookieBytes) return false;
  for (const char c : cookie.name.view())
    if (c == '\0' || c == '\r' || c == '\n') return false;
  for (const char c : cookie.value.view())
    if (c == '\0' || c == '\r' || c == '\n') return false;

  bool has_domain = false, has_expires = false, has_max_age = false;
  int64_t expires = -1, max_age = 0;
  String path_attr(allocator_), domain_attr(allocator_);
  bool has_path = false;
  StringView attr;
  while (rest.next_token(';', &attr)) {
    attr = attr.trimmed();
    if (attr.empty()) continue;
    StringView name = attr, value;
    const uint64_t e = attr.find('=');
    if (e != StringView::npos) {
      name = attr.first(e).trimmed();
      value = attr.substr(e + 1).trimmed();
    }
    if (value.size() > kMaxAttributeBytes) continue;
    if (name.equals_ignore_case("Expires")) {
      int64_t t;
      if (parse_http_date(value, &t)) {
        has_expires = true;
        expires = t;
      }
    } else if (name.equals_ignore_case("Max-Age")) {
      int64_t t;
      if (parse_integer(value, &t)) {
        has_max_age = true;
        max_age = t;
      }
    } else if (name.equals_ignore_case("Domain")) {
      if (value.starts_with('.')) value = value.without_prefix(1);
      domain_attr.clear();
      append_lower(value, &domain_attr);
      has_domain = !domain_attr.empty();
    } else if (name.equals_ignore_case("Path")) {
      has_path = value.starts_with('/');
      if (has_path) path_attr.assign(value);
    } else if (name.equals_ignore_case("Secure")) {
      cookie.secure = true;
    } else if (name.equals_ignore_case("HttpOnly")) {
      cookie.http_only = true;
    } else if (name.equals_ignore_case("SameSite")) {
      cookie.same_site = value.equals_ignore_case("Strict") ? 2 : value.equals_ignore_case("None") ? 3 : 1;
    }
  }

  const bool secure_origin = secure_url(url);
  const StringView host = url.host.view();
  if (cookie.http_only && from_script) return false;
  if (cookie.secure && !secure_origin) return false;
  if (cookie.same_site == 3 && !cookie.secure) return false;
  const StringView prefixed = cookie.name.view();
  if (prefixed.starts_with("__Secure-") && !cookie.secure) return false;
  if (prefixed.starts_with("__Host-") && (!cookie.secure || has_domain || !has_path || !path_attr.equals("/"))) return false;
  if (has_domain) {
    if (is_ip_address(host)) {
      if (!domain_attr.view().equals(host)) return false;
    } else {
      if (!domain_matches(host, domain_attr.view())) return false;
      if (domain_attr.view().find('.') == StringView::npos && !domain_attr.view().equals(host)) return false;   // ("com": no public suffix list, so a bare label is refused)
    }
    cookie.host_only = false;   // (a Domain attribute makes it a domain cookie, even when it names the host itself)
    cookie.domain.assign(domain_attr.view());
  } else {
    cookie.host_only = true;
    cookie.domain.assign(host);
  }
  if (has_path) cookie.path.assign(path_attr.view());
  else default_path(url, &cookie.path);

  if (has_max_age) cookie.expires_ms = max_age <= 0 ? 0 : static_cast<int64_t>(now_ms) + (max_age > 400ll * 86400 ? 400ll * 86400 : max_age) * 1000;
  else if (has_expires) cookie.expires_ms = expires <= 0 ? 0 : expires;
  // (cookies live at most 400 days)
  if (cookie.expires_ms > static_cast<int64_t>(now_ms) + 400ll * 86400 * 1000) cookie.expires_ms = static_cast<int64_t>(now_ms) + 400ll * 86400 * 1000;
  const bool expired = cookie.expires_ms >= 0 && cookie.expires_ms <= static_cast<int64_t>(now_ms);

  // the cookie it replaces
  for (uint64_t i = 0; i < cookies_.count(); ++i) {
    Cookie& old = cookies_[i];
    if (!old.name.equals(cookie.name.view()) || !old.domain.equals(cookie.domain.view()) || !old.path.equals(cookie.path.view()) || old.host_only != cookie.host_only) continue;
    if (old.http_only && from_script) return false;
    if (old.secure && !secure_origin) return false;   // (an insecure site cannot overwrite a secure cookie)
    if (!expired) {
      cookie.created = old.created;
      cookie.order = old.order;
    }
    cookies_.remove_at(i);
    break;
  }
  if (expired) return true;
  if (cookie.created == 0) {
    cookie.created = now_ms ? now_ms : 1;
    cookie.order = next_order_++;
  }
  cookie.accessed = now_ms;
  if (!cookies_.push_back(util::move(cookie))) return false;
  enforce_limits(host);
  return true;
}

void CookieJar::enforce_limits(StringView host) {
  // per host (domain), then in all: the least recently used cookie goes first
  auto drop_oldest = [&](bool only_host) {
    int64_t victim = -1;
    for (uint64_t i = 0; i < cookies_.count(); ++i) {
      if (only_host && !cookies_[i].domain.equals(host)) continue;
      if (victim < 0 || cookies_[i].accessed < cookies_[static_cast<uint64_t>(victim)].accessed) victim = static_cast<int64_t>(i);
    }
    if (victim >= 0) cookies_.remove_at(static_cast<uint64_t>(victim));
  };
  for (;;) {
    uint32_t same = 0;
    for (uint64_t i = 0; i < cookies_.count(); ++i)
      if (cookies_[i].domain.equals(host)) ++same;
    if (same <= kMaxPerHost) break;
    drop_oldest(true);
  }
  while (cookies_.count() > kMaxCookies) drop_oldest(false);
}

void CookieJar::remove_expired(uint64_t now_ms) {
  kernel::ScopedMutex lock(mutex_);
  remove_expired_locked(now_ms);
}

void CookieJar::remove_expired_locked(uint64_t now_ms) {
  for (uint64_t i = cookies_.count(); i > 0; --i)
    if (cookies_[i - 1].expires_ms >= 0 && cookies_[i - 1].expires_ms <= static_cast<int64_t>(now_ms)) cookies_.remove_at(i - 1);
}

void CookieJar::remove_host(StringView host) {
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = cookies_.count(); i > 0; --i)
    if (domain_matches(host, cookies_[i - 1].domain.view()) || domain_matches(cookies_[i - 1].domain.view(), host)) cookies_.remove_at(i - 1);
}

void CookieJar::header_for(const WebUrl& url, uint64_t now_ms, Site site, bool for_script, String* out) {
  kernel::ScopedMutex lock(mutex_);
  remove_expired_locked(now_ms);
  const bool secure_origin = secure_url(url);
  const StringView host = url.host.view();
  const StringView path = url.path.empty() ? StringView("/") : url.path.view();
  // the matching cookies: longer paths first, then the older ones
  List<uint32_t> chosen(allocator_);
  for (uint64_t i = 0; i < cookies_.count(); ++i) {
    Cookie& c = cookies_[i];
    if (c.host_only ? !c.domain.equals(host) : !domain_matches(host, c.domain.view())) continue;
    if (!path_matches(path, c.path.view())) continue;
    if (c.secure && !secure_origin) continue;
    if (c.http_only && for_script) continue;
    if (site != Site::SameSite) {
      const uint8_t mode = c.same_site == 0 ? 1 : c.same_site;
      if (mode == 2) continue;
      if (mode == 1 && site != Site::CrossSiteNavigation) continue;
    }
    chosen.push_back(static_cast<uint32_t>(i));
  }
  for (uint64_t a = 1; a < chosen.count(); ++a) {   // (insertion sort: there are few)
    uint32_t key = chosen[a];
    uint64_t b = a;
    while (b > 0) {
      const Cookie& x = cookies_[chosen[b - 1]];
      const Cookie& y = cookies_[key];
      const bool before = y.path.size() > x.path.size() || (y.path.size() == x.path.size() && y.order < x.order);
      if (!before) break;
      chosen[b] = chosen[b - 1];
      --b;
    }
    chosen[b] = key;
  }
  for (uint64_t i = 0; i < chosen.count(); ++i) {
    Cookie& c = cookies_[chosen[i]];
    c.accessed = now_ms;
    if (!out->empty()) out->append("; ");
    if (!c.name.empty()) {
      out->append(c.name.view());
      out->push_back('=');
    }
    out->append(c.value.view());
  }
}

}  // namespace browser
