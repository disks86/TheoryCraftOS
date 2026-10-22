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

// Host-side tests for the HTTP client: URL parsing and redirect resolution, requests as they go out, every way an answer can be framed
// (Content-Length, chunked in tiny pieces, until close), redirects, compression, limits and failures - against a scripted stack - and
// a whole exchange over two real stacks joined by a fake wire. HTTPS is exercised against real servers by tests/http_live.sh.
#include <functional>
#include <map>
#include <stdio.h>
#include <string>
#include <vector>
#include "Media/Zlib.hpp"
#include "Network/Http/HttpClient.hpp"
#include "NetworkHarness.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static std::string text_of(ConstByteView v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size()); }
static std::string text_of(StringView v) { return std::string(v.data(), v.size()); }

// ------------------------------------------------------------------------------------------------ URLs

static void test_urls() {
  TestAllocator a;
  {
    HttpUrl u(a);
    CHECK(parse_http_url("http://example.com", &u) && !u.https && u.port == 80 && text_of(u.host.view()) == "example.com" && text_of(u.path.view()) == "/");
    CHECK(parse_http_url("HTTPS://Example.com:8443/a/b?x=1&y=2#frag", &u) && u.https && u.port == 8443 && text_of(u.host.view()) == "Example.com" && text_of(u.path.view()) == "/a/b?x=1&y=2");
    CHECK(parse_http_url("http://10.0.0.5:80/", &u) && u.default_port() && text_of(u.host.view()) == "10.0.0.5");
    CHECK(parse_http_url("http://example.com?q=1", &u) && text_of(u.path.view()) == "/?q=1");
    CHECK(parse_http_url("http://example.com/a/./b/../c//d/", &u) && text_of(u.path.view()) == "/a/c//d/");
    CHECK(parse_http_url("http://example.com/../../x", &u) && text_of(u.path.view()) == "/x");
    String text(a);
    CHECK(parse_http_url("https://h.example:444/p?q", &u) && u.assign_text(&text) && text_of(text.view()) == "https://h.example:444/p?q");
    CHECK(parse_http_url("http://h.example/", &u) && u.assign_text(&text) && text_of(text.view()) == "http://h.example/");
    // refused
    const char* bad[] = {"", "example.com", "ftp://example.com/", "http://", "http:///path", "http://user@example.com/", "http://example.com:0/", "http://example.com:65536/",
                         "http://example.com:/", "http://example.com:abc/", "http://[::1]/", "http://exa mple.com/", "http://example.com/a b", "http://example.com/\x01"};
    for (const char* s : bad) CHECK(!parse_http_url(s, &u));
    std::string long_host = "http://" + std::string(254, 'a') + "/";
    CHECK(!parse_http_url(long_host.c_str(), &u));

    // references
    HttpUrl base(a);
    CHECK(parse_http_url("http://example.com:8080/dir/page.html?old=1", &base));
    HttpUrl r(a);
    CHECK(resolve_http_reference(base, "http://other.org/x", &r) && text_of(r.host.view()) == "other.org" && r.port == 80 && text_of(r.path.view()) == "/x");
    CHECK(resolve_http_reference(base, "https://other.org/x", &r) && r.https && r.port == 443);
    CHECK(resolve_http_reference(base, "//cdn.example.net/lib.js", &r) && !r.https && text_of(r.host.view()) == "cdn.example.net" && text_of(r.path.view()) == "/lib.js");
    CHECK(resolve_http_reference(base, "/top?x=1#f", &r) && text_of(r.host.view()) == "example.com" && r.port == 8080 && text_of(r.path.view()) == "/top?x=1");
    CHECK(resolve_http_reference(base, "other.html", &r) && text_of(r.path.view()) == "/dir/other.html");
    CHECK(resolve_http_reference(base, "../up/other.html?z", &r) && text_of(r.path.view()) == "/up/other.html?z");
    CHECK(resolve_http_reference(base, "./sub/", &r) && text_of(r.path.view()) == "/dir/sub/");
    CHECK(resolve_http_reference(base, "?new=2", &r) && text_of(r.path.view()) == "/dir/page.html?new=2");
    CHECK(resolve_http_reference(base, "#only", &r) && text_of(r.path.view()) == "/dir/page.html?old=1");
    CHECK(resolve_http_reference(base, "  /padded  ", &r) && text_of(r.path.view()) == "/padded");
    CHECK(!resolve_http_reference(base, "", &r) && !resolve_http_reference(base, "   ", &r));
    CHECK(!resolve_http_reference(base, "a b", &r) && !resolve_http_reference(base, "ftp://x/y", &r));
  }
  CHECK(g_live == 0);
}

// ------------------------------------------------------------------------------------------------ a scripted stack

// A TCP socket that is the "server": once a whole request has been sent it produces the answer, handing it out `piece` bytes at a time.
struct ScriptSocket final : ITcpSocket {
  std::function<std::string(const std::string&)>* server = nullptr;
  std::vector<std::string>* requests = nullptr;
  std::string sent, answer;
  size_t pos = 0, piece = 1 << 20;
  bool connected = false, refuse = false, answered = false, never_answer = false, keep_open = false;
  const uint64_t* polls = nullptr;   // when set, only one piece is handed out for each poll of the stack
  uint64_t served_at = ~uint64_t{0};
  uint16_t port = 0;
  Ipv4Address remote;
  TcpState state() const override { return connected ? TcpState::Established : TcpState::SynSent; }
  bool connect(Ipv4Address a, uint16_t p) override { remote = a; port = p; connected = !refuse; return true; }
  bool listen(uint16_t) override { return false; }
  SharedPointer<ITcpSocket> accept() override { return SharedPointer<ITcpSocket>(); }
  uint32_t send(ConstByteView data) override {
    if (!connected) return 0;
    sent.append(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<uint32_t>(data.size());
  }
  void maybe_answer() {
    if (answered || never_answer) return;
    const size_t end = sent.find("\r\n\r\n");
    if (end == std::string::npos) return;
    size_t want = end + 4;
    const size_t cl = sent.find("Content-Length: ");
    if (cl != std::string::npos && cl < end) want += strtoul(sent.c_str() + cl + 16, nullptr, 10);
    if (sent.size() < want) return;
    answered = true;
    if (requests) requests->push_back(sent);
    answer = (*server)(sent);
  }
  uint32_t receive(ByteView buffer) override {
    maybe_answer();
    if (pos >= answer.size()) return 0;
    if (polls) {
      if (served_at == *polls) return 0;
      served_at = *polls;
    }
    size_t n = answer.size() - pos;
    if (n > piece) n = piece;
    if (n > buffer.size()) n = buffer.size();
    memcpy(buffer.data(), answer.data() + pos, n);
    pos += n;
    return static_cast<uint32_t>(n);
  }
  uint32_t available() const override { const_cast<ScriptSocket*>(this)->maybe_answer(); return static_cast<uint32_t>(answer.size() - pos); }
  uint32_t unsent_or_unacknowledged() const override { return 0; }
  bool is_connected() const override { return connected; }
  bool peer_closed() const override { const_cast<ScriptSocket*>(this)->maybe_answer(); return answered && pos >= answer.size() && !keep_open; }
  bool failed() const override { return refuse; }
  void close() override {}
  void abort() override { connected = false; }
  uint16_t local_port() const override { return 40000; }
  Ipv4Address remote_address() const override { return remote; }
  uint16_t remote_port() const override { return port; }
};

struct ScriptQuery final : IDnsQuery {
  DnsStatus s; Ipv4Address a;
  DnsStatus status() const override { return s; }
  Ipv4Address address() const override { return a; }
  uint32_t ttl_seconds() const override { return 60; }
};

struct ScriptResolver final : IDnsResolver {
  allocators::IAllocator& alloc;
  std::map<std::string, Ipv4Address> names;
  bool pending_first = false;
  explicit ScriptResolver(allocators::IAllocator& al) : alloc(al) {}
  SharedPointer<IDnsQuery> query(StringView name) override {
    auto q = SharedPointer<ScriptQuery>::make(alloc);
    auto it = names.find(text_of(name));
    q->s = it == names.end() ? DnsStatus::NotFound : DnsStatus::Resolved;
    if (it != names.end()) q->a = it->second;
    return SharedPointer<IDnsQuery>(q);
  }
  void clear_cache() override {}
  uint32_t cache_size() const override { return 0; }
};

struct ScriptStack final : INetworkStack {
  allocators::IAllocator& alloc;
  kernel::RecursiveMutex m;
  ScriptResolver resolver;
  std::function<std::string(const std::string&)> server;
  std::vector<std::string> requests;
  size_t piece = 1 << 20;
  bool refuse = false, never_answer = false, keep_open = false;
  mutable uint64_t ms = 1000;
  uint64_t polls = 0;
  bool one_per_poll = false;
  bool ticking = false;   // every look at the clock moves it on (a clock that changes while a poll is in progress)
  uint32_t sockets = 0;
  explicit ScriptStack(allocators::IAllocator& a) : alloc(a), resolver(a) { resolver.names["example.com"] = Ipv4Address::make(93, 184, 216, 34); resolver.names["other.org"] = Ipv4Address::make(10, 9, 8, 7); }
  kernel::RecursiveMutex& mutex() override { return m; }
  void poll() override {
    ms += 1;
    ++polls;
  }
  uint64_t now_ms() const override { return ticking ? ++ms : ms; }
  IpConfiguration configuration() const override { return {}; }
  void configure(const IpConfiguration&) override {}
  void start_dhcp() override {}
  const IDhcpClient& dhcp() const override { return *static_cast<IDhcpClient*>(nullptr); }
  bool wait_for_address(uint32_t) override { return true; }
  bool resolve(StringView, uint32_t, Ipv4Address*) override { return false; }
  IDnsResolver& dns() override { return resolver; }
  bool ping(Ipv4Address, uint32_t, uint32_t*) override { return false; }
  SharedPointer<IUdpSocket> udp_open(uint16_t) override { return SharedPointer<IUdpSocket>(); }
  SharedPointer<ITcpSocket> tcp_open() override {
    auto s = SharedPointer<ScriptSocket>::make(alloc);
    s->server = &server;
    s->requests = &requests;
    s->piece = piece;
    s->refuse = refuse;
    s->never_answer = never_answer;
    s->keep_open = keep_open;
    if (one_per_poll) s->polls = &polls;
    ++sockets;
    return SharedPointer<ITcpSocket>(s);
  }
};

static std::string response(const char* status, const char* headers, const std::string& body, bool length = true) {
  std::string r = std::string("HTTP/1.1 ") + status + "\r\n" + headers;
  if (length) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  return r + "\r\n" + body;
}

static std::string chunked(const std::string& body, size_t size) {
  std::string out;
  char hex[32];
  for (size_t at = 0; at < body.size(); at += size) {
    const size_t n = std::min(size, body.size() - at);
    snprintf(hex, sizeof hex, "%zX", n);   // (upper case digits)
    out += std::string(hex) + (at == 0 ? ";ext=1" : "") + "\r\n" + body.substr(at, n) + "\r\n";
  }
  return out + "0\r\nX-Trailer: yes\r\n\r\n";
}

static bool fetch(ScriptStack& stack, HttpClient& http, HttpMethod m, const char* url, ConstByteView body = ConstByteView(), const char* type = "") {
  return http.start(m, url, body, type) && http.run(60000);
}

// The time of the last progress is taken later than the `now` of the poll that takes it; the stall check must not read that as a huge stall.
static void test_clock_moves_during_poll() {
  TestAllocator a;
  {
    ScriptStack stack(a);
    stack.ticking = true;
    stack.one_per_poll = true;
    stack.piece = 700;
    std::string body(60000, 'x');
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Type: text/plain\r\n", body); };
    HttpClient http(stack, a);
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/big") && http.status() == 200 && http.body().size() == body.size());
  }
  CHECK(g_live == 0);
}

static void test_requests_and_framing() {
  TestAllocator a;
  {
    ScriptStack stack(a);
    HttpClient http(stack, a);
    stack.server = [](const std::string&) { return response("200 OK", "Content-Type: text/plain\r\nX-Multi: one\r\nx-multi: two\r\n", "hello world"); };
    // the request as it goes out
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/a/b?x=1#frag"));
    CHECK(http.status() == 200 && text_of(http.reason()) == "OK" && text_of(http.body()) == "hello world");
    CHECK(stack.requests.size() == 1);
    const std::string& req = stack.requests[0];
    CHECK(req.rfind("GET /a/b?x=1 HTTP/1.1\r\n", 0) == 0);
    CHECK(req.find("\r\nHost: example.com\r\n") != std::string::npos && req.find("\r\nConnection: close\r\n") != std::string::npos);
    CHECK(req.find("User-Agent: TheoryCraftOS") != std::string::npos && req.find("Accept: */*") != std::string::npos);
    CHECK(req.size() > 4 && req.substr(req.size() - 4) == "\r\n\r\n" && req.find("Content-Length") == std::string::npos);
    // headers
    CHECK(text_of(http.header("content-type")) == "text/plain" && text_of(http.header("CONTENT-TYPE")) == "text/plain" && text_of(http.header("x-multi")) == "one");
    CHECK(http.header("missing").empty() && http.content_length() == 11 && http.bytes_received() == 11 && http.redirects() == 0);
    // a port in the Host header; an address instead of a name needs no lookup
    CHECK(fetch(stack, http, HttpMethod::Get, "http://10.0.0.7:8080/"));
    CHECK(stack.requests.back().find("\r\nHost: 10.0.0.7:8080\r\n") != std::string::npos);
    // extra headers replace defaults and are validated
    CHECK(http.add_header("User-Agent", "Tester/1") && http.add_header("X-Token", "abc") && !http.add_header("Bad Name", "x") && !http.add_header("X", "a\r\nInjected: 1") && !http.add_header("", "x"));
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/"));
    CHECK(stack.requests.back().find("User-Agent: Tester/1\r\n") != std::string::npos && stack.requests.back().find("TheoryCraftOS") == std::string::npos && stack.requests.back().find("X-Token: abc\r\n") != std::string::npos);
    http.clear_headers();

    // POST with a body
    const uint8_t payload[5] = {'a', '=', 'b', '&', 'c'};
    CHECK(fetch(stack, http, HttpMethod::Post, "http://example.com/form", ConstByteView(payload, 5), "application/x-www-form-urlencoded"));
    CHECK(stack.requests.back().rfind("POST /form HTTP/1.1\r\n", 0) == 0 && stack.requests.back().find("Content-Type: application/x-www-form-urlencoded\r\n") != std::string::npos);
    CHECK(stack.requests.back().find("Content-Length: 5\r\n") != std::string::npos && stack.requests.back().substr(stack.requests.back().size() - 5) == "a=b&c");

    // an unknown name; a bad URL
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://nowhere.invalid/") && http.state() == HttpState::Failed && http.error() == HttpError::Resolve);
    CHECK(!http.start(HttpMethod::Get, "gopher://example.com/") && http.error() == HttpError::BadUrl);
    CHECK(!http.start(HttpMethod::Get, "https://example.com/") && http.error() == HttpError::NoTlsConfiguration);

    // framing, with the answer handed over in pieces from the whole thing down to single bytes
    const std::string body = "The quick brown fox jumps over the lazy dog. 0123456789 0123456789 0123456789";
    for (size_t piece : {size_t{1 << 20}, size_t{7}, size_t{1}}) {
      stack.piece = piece;
      stack.server = [&](const std::string&) { return response("200 OK", "", body); };
      CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == body);
      stack.server = [&](const std::string&) { return response("200 OK", "Transfer-Encoding: chunked\r\n", chunked(body, 10), false); };
      CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == body && http.content_length() == ~uint64_t{0});
      stack.server = [&](const std::string&) { return response("200 OK", "Transfer-Encoding: chunked\r\nContent-Length: 3\r\n", chunked(body, 1000), false); };
      CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == body);   // (chunked wins over a length)
      stack.server = [&](const std::string&) { return response("200 OK", "Connection: close\r\n", body, false); };
      CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == body);
      stack.server = [&](const std::string&) { return std::string("HTTP/1.1 100 Continue\r\n\r\n") + response("200 OK", "", body); };
      CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.status() == 200 && text_of(http.body()) == body);
      stack.server = [&](const std::string&) { return response("200 OK", "", body); };
      CHECK(fetch(stack, http, HttpMethod::Head, "http://example.com/") && http.status() == 200 && http.body().size() == 0 && stack.requests.back().rfind("HEAD / ", 0) == 0);
    }
    stack.piece = 1 << 20;
    stack.server = [&](const std::string&) { return response("204 No Content", "", "", false); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.status() == 204 && http.body().size() == 0);
    stack.server = [&](const std::string&) { return std::string("HTTP/1.1 304 Not Modified\r\nContent-Length: 50\r\n\r\n"); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.status() == 304 && http.body().size() == 0);
    stack.server = [&](const std::string&) { return response("404 Not Found", "", "nothing here"); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.status() == 404 && text_of(http.body()) == "nothing here");   // (a status is not an error)
    stack.server = [&](const std::string&) { return std::string("HTTP/1.0 200 OK\r\n\r\nold style"); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == "old style");
    stack.server = [&](const std::string&) { return response("200", "", "no reason phrase"); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.status() == 200 && http.reason().empty());
  }
  CHECK(g_live == 0);
}

static void test_errors_and_limits() {
  TestAllocator a;
  {
    ScriptStack stack(a);
    HttpClient http(stack, a);
    auto fails_with = [&](HttpError e) { return http.state() == HttpState::Failed && http.error() == e; };
    // not HTTP at all, or damaged
    stack.server = [](const std::string&) { return std::string("SSH-2.0-OpenSSH\r\n\r\n"); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 2x0 OK\r\n\r\n"); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/2 200\r\n\r\n"); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 12abc\r\n\r\nxx"); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\ntruncated"); };   // the connection ends early
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"); };   // no last chunk
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n"); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n"); };   // no CRLF after the data
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 200 OK\r\n") + std::string(70000, 'x'); };   // a head that never ends
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));
    stack.server = [](const std::string&) { return std::string("HTTP/1.1 101 Switching Protocols\r\n\r\n"); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Protocol));

    // size limits
    const std::string big(5000, 'z');
    http.set_max_body(1000);
    stack.server = [&](const std::string&) { return response("200 OK", "", big); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::TooLarge));
    stack.server = [&](const std::string&) { return response("200 OK", "Transfer-Encoding: chunked\r\n", chunked(big, 700), false); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::TooLarge));
    stack.server = [&](const std::string&) { return response("200 OK", "", big, false); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::TooLarge));
    http.set_max_body(16u << 20);

    // a body sink gets everything, in pieces, and keeps memory out of it; refusing stops the download
    struct Sink { std::string got; int calls = 0; size_t stop_after = ~size_t{0}; } sink;
    http.set_body_sink([](void* c, ConstByteView d) { auto* s = static_cast<Sink*>(c); s->got += text_of(d); ++s->calls; return s->got.size() < s->stop_after; }, &sink);
    stack.piece = 100;
    stack.server = [&](const std::string&) { return response("200 OK", "", big); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && sink.got == big && sink.calls > 10 && http.body().size() == 0 && http.bytes_received() == big.size());
    CHECK(stack.requests.back().find("Accept-Encoding: identity") != std::string::npos);
    sink.got.clear();
    sink.stop_after = 1000;
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Refused));
    http.set_body_sink(nullptr, nullptr);
    stack.piece = 1 << 20;

    // connections that go wrong
    stack.refuse = true;
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Connect));
    stack.refuse = false;
    stack.never_answer = true;
    http.set_timeouts(15000, 2000);
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && fails_with(HttpError::Timeout));
    CHECK(http.start(HttpMethod::Get, "http://example.com/") && http.busy());
    http.cancel();
    CHECK(fails_with(HttpError::Cancelled));
    stack.never_answer = false;
    // run() gives up after its own limit too
    stack.never_answer = true;
    http.set_timeouts(15000, 1000000);
    CHECK(http.start(HttpMethod::Get, "http://example.com/") && !http.run(500) && fails_with(HttpError::Timeout));
    stack.never_answer = false;
    // and the client is usable again afterwards
    stack.server = [&](const std::string&) { return response("200 OK", "", "fine"); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == "fine" && http.error() == HttpError::None);
  }
  CHECK(g_live == 0);
}

static void test_redirects() {
  TestAllocator a;
  {
    ScriptStack stack(a);
    HttpClient http(stack, a);
    auto path_of = [](const std::string& request) { const size_t sp = request.find(' '); return request.substr(sp + 1, request.find(' ', sp + 1) - sp - 1); };
    stack.server = [&](const std::string& request) {
      const std::string path = path_of(request);
      if (path == "/start") return response("301 Moved Permanently", "Location: /middle\r\n", "");
      if (path == "/middle") return response("302 Found", "Location: http://other.org:8081/final?x=1\r\n", "");
      if (path == "/final?x=1") return response("200 OK", "", "arrived");
      if (path == "/rel") return response("302 Found", "Location: sub/page\r\n", "");
      if (path == "/loop") return response("302 Found", "Location: /loop\r\n", "");
      if (path == "/post") return response("303 See Other", "Location: /result\r\n", "");
      if (path == "/post307") return response("307 Temporary Redirect", "Location: /echo\r\n", "");
      if (path == "/post302") return response("302 Found", "Location: /result\r\n", "");
      if (path == "/nowhere") return response("302 Found", "", "");
      if (path == "/bad") return response("302 Found", "Location: ftp://x/y\r\n", "");
      if (path == "/secure") return response("302 Found", "Location: https://example.com/\r\n", "");
      if (path == "/other-origin") return response("302 Found", "Location: http://other.org/x\r\n", "");
      return response("200 OK", "", "path " + path);
    };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/start") && http.status() == 200 && text_of(http.body()) == "arrived" && http.redirects() == 2);
    CHECK(stack.requests.size() == 3 && stack.requests[2].find("Host: other.org:8081\r\n") != std::string::npos && text_of(http.url().host.view()) == "other.org" && http.url().port == 8081);
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/rel") && text_of(http.body()) == "path /sub/page");
    // a loop gives up
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/loop") && http.error() == HttpError::TooManyRedirects && stack.sockets > 5);
    http.set_max_redirects(0);   // none followed: the redirect is the answer
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/start") && http.status() == 301 && text_of(http.header("location")) == "/middle");
    http.set_max_redirects(1);
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/start") && http.error() == HttpError::TooManyRedirects);
    http.set_max_redirects(5);
    // a redirect without a Location is an answer like any other; a bad one is an error
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/nowhere") && http.status() == 302);
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/bad") && http.error() == HttpError::BadUrl);
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/secure") && http.error() == HttpError::NoTlsConfiguration);   // (https: needs the TLS configuration)
    // methods and bodies across redirects
    const uint8_t payload[3] = {'x', 'y', 'z'};
    CHECK(fetch(stack, http, HttpMethod::Post, "http://example.com/post", ConstByteView(payload, 3), "text/plain") && text_of(http.body()) == "path /result");
    CHECK(stack.requests[stack.requests.size() - 2].rfind("POST /post ", 0) == 0 && stack.requests.back().rfind("GET /result ", 0) == 0 && stack.requests.back().find("Content-") == std::string::npos);
    CHECK(fetch(stack, http, HttpMethod::Post, "http://example.com/post302", ConstByteView(payload, 3), "text/plain") && stack.requests.back().rfind("GET /result ", 0) == 0);
    CHECK(fetch(stack, http, HttpMethod::Post, "http://example.com/post307", ConstByteView(payload, 3), "text/plain") && stack.requests.back().rfind("POST /echo ", 0) == 0 && stack.requests.back().substr(stack.requests.back().size() - 3) == "xyz");
    // credentials stay with their origin
    http.add_header("Authorization", "Bearer secret");
    http.add_header("X-Keep", "yes");
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/other-origin"));
    CHECK(stack.requests[stack.requests.size() - 2].find("Authorization: Bearer secret") != std::string::npos);
    CHECK(stack.requests.back().find("Authorization") == std::string::npos && stack.requests.back().find("X-Keep: yes") != std::string::npos && stack.requests.back().find("Host: other.org\r\n") != std::string::npos);
  }
  CHECK(g_live == 0);
}

static void test_compression() {
  TestAllocator a;
  {
    ScriptStack stack(a);
    HttpClient http(stack, a);
    std::string text;
    for (int i = 0; i < 400; ++i) text += "compressible line number " + std::to_string(i) + "\n";
    ByteBuffer raw(a), zlib(a);
    const ConstByteView input(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    CHECK(media::deflate(input, &raw, 6, a, true) && media::deflate(input, &zlib, 6, a, false));
    // gzip: header, raw deflate data, CRC-32 (unchecked) and length
    std::string gz = std::string("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\x03", 10) + text_of(raw.view());
    uint32_t n = static_cast<uint32_t>(text.size());
    gz += std::string("\0\0\0\0", 4) + std::string(reinterpret_cast<const char*>(&n), 4);
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: gzip\r\n", gz); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == text && http.body_was_decoded());
    CHECK(stack.requests.back().find("Accept-Encoding: gzip, deflate") != std::string::npos);
    stack.piece = 50;
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: deflate\r\n", text_of(zlib.view())); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == text && http.body_was_decoded());
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: deflate\r\n", text_of(raw.view())); };   // (bare deflate data)
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == text);
    stack.piece = 1 << 20;
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n", chunked(gz, 33), false); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == text);
    // damaged data and a bomb
    std::string damaged = gz;
    damaged[20] ^= 0x5A;
    damaged[damaged.size() - 4] ^= 0x01;   // (the length)
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: gzip\r\n", damaged); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.error() == HttpError::Protocol);
    http.set_max_body(5000);   // (more than the compressed data, less than what it inflates to)
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: gzip\r\n", gz); };
    CHECK(!fetch(stack, http, HttpMethod::Get, "http://example.com/") && http.error() == HttpError::Protocol);   // (inflates past the limit)
    http.set_max_body(1u << 20);
    // an encoding nobody here knows is left as it came
    stack.server = [&](const std::string&) { return response("200 OK", "Content-Encoding: br\r\n", "opaque"); };
    CHECK(fetch(stack, http, HttpMethod::Get, "http://example.com/") && text_of(http.body()) == "opaque" && !http.body_was_decoded());
  }
  CHECK(g_live == 0);
}

// ------------------------------------------------------------------------------------------------ over real stacks

static void test_over_the_network() {
  Lan lan;
  lan.configure_static();
  {
    Server server(lan, 80);
    std::string big;
    for (int i = 0; i < 3000; ++i) big += "row " + std::to_string(i) + " of the table\n";   // ~60 KB: many TCP segments
    int answered = 0;
    std::string out;   // what is still to be sent to the client; the server hands it over as the client takes it
    bool closing = false;
    server.app = [&](Server& s) {
      if (!s.conn) return;
      if (!out.empty()) {
        const uint32_t n = s.conn->send(ConstByteView(reinterpret_cast<const uint8_t*>(out.data()), out.size()));
        out.erase(0, n);
        return;
      }
      if (closing) {   // all sent: close, then wait for the client to go away before taking the next one
        s.conn->close();
        closing = false;
        return;
      }
      if (answered > 0 && s.conn->peer_closed() && s.conn->unsent_or_unacknowledged() == 0) {
        s.conn = SharedPointer<ITcpSocket>();
        s.received.clear();
        return;
      }
      if (s.received.find("\r\n\r\n") == std::string::npos || answered >= 2) return;
      s.received.clear();
      if (answered++ == 0) {
        std::string body;
        for (size_t at = 0; at < big.size(); at += 4000) {
          const size_t n = std::min<size_t>(4000, big.size() - at);
          char hex[16];
          snprintf(hex, sizeof hex, "%zx", n);
          body += std::string(hex) + "\r\n" + big.substr(at, n) + "\r\n";
        }
        out = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n" + body + "0\r\n\r\n";
      } else {
        out = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
      }
      closing = true;
    };
    HttpClient http(lan.a, lan.alloc);
    CHECK(http.start(HttpMethod::Get, "http://10.0.0.2/table.txt"));
    CHECK(lan.run_until([&] { lan.a.poll(); http.poll(); return !http.busy(); }, 20000));
    CHECK(http.state() == HttpState::Done && http.status() == 200 && text_of(http.body()) == big && text_of(http.header("content-type")) == "text/plain");
    // a second request opens a fresh connection
    CHECK(http.start(HttpMethod::Get, "http://10.0.0.2/small"));
    CHECK(lan.run_until([&] { lan.a.poll(); http.poll(); return !http.busy(); }, 20000));
    CHECK(http.state() == HttpState::Done && text_of(http.body()) == "ok");
    server.running = true;   // (the server is not asked to run again)
  }
}

int main() {
  test_urls();
  test_requests_and_framing();
  test_clock_moves_during_poll();
  test_errors_and_limits();
  test_redirects();
  test_compression();
  test_over_the_network();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("http tests passed\n");
  return 0;
}
