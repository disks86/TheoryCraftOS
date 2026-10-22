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

#include "Browser/HttpPageLoader.hpp"
#include "Browser/Fetcher.hpp"
#include "Utility/ByteText.hpp"

namespace browser {

namespace {

// A WebSocket over an HttpClient that is driven by poll().
class HttpWebSocket final : public IWebSocket {
 public:
  HttpWebSocket(network::INetworkStack& stack, allocators::IAllocator& allocator) : http_(stack, allocator), allocator_(allocator) {}
  network::HttpClient& client() { return http_; }

  void poll() override {
    if (finished_) return;
    http_.poll();
    if (http_.state() == network::HttpState::Upgraded) opened_ = true;
    if (http_.state() == network::HttpState::Failed || http_.ws_closed()) finished_ = true;
  }
  State state() const override {
    if (finished_) return State::Closed;
    if (http_.state() == network::HttpState::Upgraded) return closing_ ? State::Closing : State::Open;
    return State::Connecting;
  }
  bool send(bool text, ConstByteView data) override {
    if (state() != State::Open) return false;
    if (text) return http_.ws_send_text(bytes_as_text(data));
    return http_.ws_send_binary(data);
  }
  void close(uint16_t code, StringView reason) override {
    if (finished_ || closing_) return;
    closing_ = true;
    if (http_.state() == network::HttpState::Upgraded) {
      http_.ws_close(code, reason);
    } else {
      http_.cancel();
      finished_ = true;
    }
  }
  bool receive(bool* text, ByteBuffer* data) override {
    network::WebSocketMessage m(allocator_);
    if (!http_.ws_receive(&m)) return false;
    *text = m.text;
    data->clear();
    data->append(m.data.view());
    return true;
  }
  StringView protocol() const override { return http_.ws_protocol(); }
  uint16_t close_code() const override { return opened_ && http_.ws_closed() ? http_.ws_close_code() : static_cast<uint16_t>(1006); }
  StringView close_reason() const override { return opened_ ? http_.ws_close_reason() : StringView(); }
  bool opened() const override { return opened_; }

 private:
  network::HttpClient http_;
  allocators::IAllocator& allocator_;
  bool opened_ = false, closing_ = false, finished_ = false;
};

}  // namespace

UniquePointer<IWebSocket> HttpPageLoader::open_websocket(StringView url, StringView protocols, StringView origin, String* error) {
  network::INetworkStack* stack = nullptr;
  for (uint32_t i = 0; i < stacks_.count() && !stack; ++i)
    if (!stacks_.at(i)->configuration().address.is_unspecified()) stack = stacks_.at(i);
  if (!stack) {
    error->assign("There is no network connection.");
    return UniquePointer<IWebSocket>();
  }
  auto socket = UniquePointer<HttpWebSocket>::make(allocator_, *stack, allocator_);
  if (!socket) {
    error->assign("Out of memory.");
    return UniquePointer<IWebSocket>();
  }
  network::HttpClient& http = socket->client();
  if (has_tls_) http.set_tls(&tls_);
  http.set_session_cache(&sessions_);
  http.set_websocket_compression(true);
  http.add_header("User-Agent", "Mozilla/5.0 (compatible; TheoryCraftOS-Browser/1.0)");
  if (!origin.empty()) http.add_header("Origin", origin);
  if (!http.start_websocket(url, protocols)) {
    error->assign(StringView(network::http_error_name(http.error())));
    return UniquePointer<IWebSocket>();
  }
  return UniquePointer<IWebSocket>(static_cast<UniquePointer<HttpWebSocket>&&>(socket));
}

bool HttpPageLoader::send(network::HttpMethod method, StringView address, StringView content_type, StringView body, PageResult* result) {
  network::INetworkStack* stack = nullptr;
  for (uint32_t i = 0; i < stacks_.count() && !stack; ++i)
    if (!stacks_.at(i)->configuration().address.is_unspecified()) stack = stacks_.at(i);
  if (!stack) {
    result->error.assign("There is no network connection.");
    return false;
  }
  network::HttpClient http(*stack, allocator_);
  if (has_tls_) http.set_tls(&tls_);
  http.set_alt_svc_cache(&alt_svc_);
  http.set_session_cache(&sessions_);
  http.set_max_body(kMaxPageBytes);
  http.add_header("User-Agent", "TheoryCraftOS-Browser/1.0");
  http.add_header("Accept", "text/html,text/plain;q=0.9,*/*;q=0.1");
  if (!http.start(method, address, text_as_bytes(body), content_type) || !http.run(timeout_ms_)) {
    result->error.assign(http.error() == network::HttpError::NoTlsConfiguration ? StringView("https needs certificates, which are not installed.")
                                                                                 : StringView(network::http_error_name(http.error())));
    return false;
  }
  result->status = http.status();
  http.url().assign_text(&result->final_address);
  result->content_type.assign(http.header("content-type"));
  result->body.clear();
  return result->body.append(http.body());
}

bool HttpPageLoader::exchange(const NetRequest& request, NetResponse* response) {
  network::HttpMethod method;
  if (request.method.equals("GET")) method = network::HttpMethod::Get;
  else if (request.method.equals("HEAD")) method = network::HttpMethod::Head;
  else if (request.method.equals("POST")) method = network::HttpMethod::Post;
  else if (request.method.equals("PUT")) method = network::HttpMethod::Put;
  else if (request.method.equals("DELETE")) method = network::HttpMethod::Delete;
  else if (request.method.equals("PATCH")) method = network::HttpMethod::Patch;
  else if (request.method.equals("OPTIONS")) method = network::HttpMethod::Options;
  else {
    response->error.assign("This browser cannot send that kind of request.");
    return false;
  }
  network::INetworkStack* stack = nullptr;
  for (uint32_t i = 0; i < stacks_.count() && !stack; ++i)
    if (!stacks_.at(i)->configuration().address.is_unspecified()) stack = stacks_.at(i);
  if (!stack) {
    response->error.assign("There is no network connection.");
    return false;
  }
  network::HttpClient http(*stack, allocator_);
  if (has_tls_) http.set_tls(&tls_);
  http.set_alt_svc_cache(&alt_svc_);
  http.set_session_cache(&sessions_);
  http.set_max_body(kMaxFetchBytes);
  http.set_max_redirects(0);   // (the caller follows redirects: it has to send the right cookies on each hop)
  http.add_header("User-Agent", "Mozilla/5.0 (compatible; TheoryCraftOS-Browser/1.0)");
  StringView content_type;
  for_each_header(request.headers, [&](StringView name, StringView value) {
    if (name.equals_ignore_case("content-type")) content_type = value;
    else http.add_header(name, value);
  });
  const uint32_t timeout = request.timeout_ms ? request.timeout_ms : timeout_ms_;
  if (!http.start(method, request.url, request.body, content_type) || !http.run(timeout)) {
    response->error.assign(http.error() == network::HttpError::NoTlsConfiguration ? StringView("https needs certificates, which are not installed.")
                                                                                   : StringView(network::http_error_name(http.error())));
    return false;
  }
  response->status = http.status();
  response->reason.assign(http.reason());
  response->url.clear();
  http.url().assign_text(&response->url);
  response->headers.assign(http.header_block());
  response->body.clear();
  return response->body.append(http.body());
}

bool HttpPageLoader::fetch(StringView address, PageResult* result) { return send(network::HttpMethod::Get, address, StringView(), StringView(), result); }

bool HttpPageLoader::post(StringView address, StringView content_type, StringView body, PageResult* result) {
  return send(network::HttpMethod::Post, address, content_type, body, result);
}

}  // namespace browser
