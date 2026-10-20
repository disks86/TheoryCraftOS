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

#include "Database/ConnectionOptions.hpp"
#include "Database/NumberText.hpp"

namespace database {

namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equals_ignoring_case(StringView a, const char* b) {
  uint64_t i = 0;
  for (; b[i]; ++i)
    if (i >= a.size() || lower(a[i]) != b[i]) return false;
  return i == a.size();
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Appends `in` with its %XX escapes undone. False for a bad escape or memory shortage.
bool unescape(StringView in, String* out) {
  out->clear();
  for (uint64_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      if (!out->push_back(in[i])) return false;
      continue;
    }
    if (i + 2 >= in.size()) return false;   // (a % needs two hex digits after it)
    const int hi = hex_value(in[i + 1]), lo = hex_value(in[i + 2]);
    if (hi < 0 || lo < 0 || !out->push_back(static_cast<char>(hi * 16 + lo))) return false;
    i += 2;
  }
  return true;
}

bool fail(DbError* error, const char* why) {
  if (error) error->set(DbStatus::InvalidArgument, why);
  return false;
}
}  // namespace

bool parse_database_url(StringView url, ConnectionOptions* out, DbError* error) {
  const uint64_t scheme_end = url.find(StringView("://"));
  if (scheme_end == StringView::npos || scheme_end == 0) return fail(error, "the URL needs a scheme (postgres://...)");
  String scheme(out->allocator());
  for (uint64_t i = 0; i < scheme_end; ++i) scheme.push_back(lower(url[i]));
  out->driver.assign(scheme.view() == StringView("postgresql") ? StringView("postgres") : scheme.view());
  StringView rest = url.substr(scheme_end + 3);

  StringView query;
  const uint64_t question = rest.find('?');
  if (question != StringView::npos) {
    query = rest.substr(question + 1);
    rest = rest.substr(0, question);
  }
  StringView path;
  const uint64_t slash = rest.find('/');
  if (slash != StringView::npos) {
    path = rest.substr(slash + 1);
    rest = rest.substr(0, slash);
  }
  StringView user_info, host_port = rest;
  const uint64_t at = rest.rfind('@');
  if (at != StringView::npos) {
    user_info = rest.substr(0, at);
    host_port = rest.substr(at + 1);
  }
  if (!user_info.empty()) {
    const uint64_t colon = user_info.find(':');
    if (!unescape(colon == StringView::npos ? user_info : user_info.substr(0, colon), &out->user)) return fail(error, "bad escape in the user name");
    if (colon != StringView::npos && !unescape(user_info.substr(colon + 1), &out->password)) return fail(error, "bad escape in the password");
  }
  StringView host = host_port;
  const uint64_t colon = host_port.rfind(':');
  if (colon != StringView::npos) {
    host = host_port.substr(0, colon);
    int64_t port;
    if (!parse_int64(host_port.substr(colon + 1), &port) || port < 1 || port > 65535) return fail(error, "the port must be a number from 1 to 65535");
    out->port = static_cast<uint16_t>(port);
  }
  if (host.empty()) return fail(error, "the URL needs a host");
  out->host.assign(host);
  if (!unescape(path, &out->database)) return fail(error, "bad escape in the database name");

  // Options.
  uint64_t at_option = 0;
  String value(out->allocator());
  while (at_option < query.size()) {
    uint64_t end = query.find('&', at_option);
    if (end == StringView::npos) end = query.size();
    const StringView pair = query.substr(at_option, end - at_option);
    at_option = end + 1;
    if (pair.empty()) continue;
    const uint64_t eq = pair.find('=');
    const StringView key = eq == StringView::npos ? pair : pair.substr(0, eq);
    if (!unescape(eq == StringView::npos ? StringView() : pair.substr(eq + 1), &value)) return fail(error, "bad escape in an option");
    if (equals_ignoring_case(key, "sslmode")) {
      const StringView v = value.view();
      if (equals_ignoring_case(v, "disable")) out->tls = TlsMode::Disable;
      else if (equals_ignoring_case(v, "allow") || equals_ignoring_case(v, "prefer")) out->tls = TlsMode::Prefer;
      else if (equals_ignoring_case(v, "require") || equals_ignoring_case(v, "verify-ca") || equals_ignoring_case(v, "verify-full")) out->tls = TlsMode::Require;
      else return fail(error, "sslmode must be disable, allow, prefer, require, verify-ca or verify-full");
    } else if (equals_ignoring_case(key, "connect_timeout")) {
      int64_t seconds;
      if (!parse_int64(value.view(), &seconds) || seconds < 0 || seconds > 3600) return fail(error, "connect_timeout is a number of seconds");
      out->connect_timeout_ms = seconds ? static_cast<uint32_t>(seconds) * 1000 : ConnectionOptions::kDefaultConnectTimeoutMs;
    } else if (equals_ignoring_case(key, "application_name")) {
      out->application_name.assign(value.view());
    } else if (equals_ignoring_case(key, "max_result_bytes")) {
      int64_t bytes;
      if (!parse_int64(value.view(), &bytes) || bytes < 1024) return fail(error, "max_result_bytes is a number of bytes (at least 1024)");
      out->max_result_bytes = static_cast<uint64_t>(bytes);
    }
  }
  return true;
}

}  // namespace database
