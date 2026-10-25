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

// How the browser gets a web page: an IPageLoader fetches the address it is given and says what came back. The browser does not know how (the
// system's loader speaks HTTP over the network stacks; a test hands out canned pages). fetch() waits for the answer, so it is called from the
// handler thread and not the input thread.
#pragma once
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace rtc {
class IRtcNetwork;
}

namespace browser {

struct PageResult {
  explicit PageResult(allocators::IAllocator& allocator) : final_address(allocator), content_type(allocator), error(allocator), body(allocator) {}

  uint32_t status = 0;        // the HTTP status (200, 404, ...)
  String final_address;       // where the page really came from, after redirects
  String content_type;        // as the server said it ("text/html; charset=utf-8"), may be empty
  String error;               // when fetch() returns false: what went wrong
  ByteBuffer body;
};

// One HTTP exchange, as scripts and the browser ask for it (see Fetcher for what is added around it: cookies, redirects, the cross-origin rules).
struct NetRequest {
  StringView method = "GET";
  StringView url;
  StringView headers;       // "Name: value\r\n" lines
  ConstByteView body;       // for POST, PUT and PATCH
  uint32_t timeout_ms = 0;  // 0: the loader's own
};

struct NetResponse {
  explicit NetResponse(allocators::IAllocator& a) : allocator(a), reason(a), url(a), headers(a), error(a), body(a) {}
  allocators::IAllocator& allocator;
  uint32_t status = 0;      // 0 for an opaque answer (a cross-origin request made without CORS)
  String reason;
  String url;               // where the answer came from (after redirects)
  String headers;           // "Name: value\r\n" lines, one for each header the server sent (so several Set-Cookie lines stay several)
  String error;             // when the request failed: why
  ByteBuffer body;
  bool redirected = false;  // a redirect was followed
  bool opaque = false;
  // What a Clear-Site-Data header asked to be cleared (fetch_resource() already did the cookies): 1 cookies, 2 storage, 4 cache.
  uint8_t clear_site_data = 0;
};

// A WebSocket connection made for a page (see IPageLoader::open_websocket). The page calls poll() often and reads what has arrived.
class IWebSocket {
 public:
  enum class State : uint8_t { Connecting, Open, Closing, Closed };
  virtual void poll() = 0;
  virtual State state() const = 0;
  virtual bool send(bool text, ConstByteView data) = 0;
  virtual void close(uint16_t code, StringView reason) = 0;
  // The next message (text: UTF-8 text, otherwise binary); false when none has arrived.
  virtual bool receive(bool* text, ByteBuffer* data) = 0;
  virtual StringView protocol() const = 0;   // the subprotocol the server chose
  virtual uint16_t close_code() const = 0;   // 1006 when the connection ended without a close handshake
  virtual StringView close_reason() const = 0;
  virtual bool opened() const = 0;           // the upgrade succeeded at some time

 protected:
  IWebSocket() = default;
  ~IWebSocket() = default;
};

class IPageLoader {
 public:
  // The network WebRTC uses (UDP, the machine's address); nullptr: none.
  virtual rtc::IRtcNetwork* rtc_network() { return nullptr; }
  // Opens a WebSocket to `url` (ws://, wss://, http://, https://) offering the comma separated `protocols`; `origin` is sent as the Origin header. Null with
  // *error set when it cannot be done. The connection is made while the page polls it.
  virtual UniquePointer<IWebSocket> open_websocket(StringView url, StringView protocols, StringView origin, String* error) {
    (void)url;
    (void)protocols;
    (void)origin;
    error->assign("This browser cannot open WebSockets.");
    return UniquePointer<IWebSocket>();
  }

  // One exchange: the request goes out and the answer that comes back is returned as it is - a redirect is not followed (the caller does that, with the right
  // cookies and checks). True if an answer arrived, whatever its status. The default asks fetch() / post(), which follow redirects themselves.
  virtual bool exchange(const NetRequest& request, NetResponse* response) {
    PageResult page(response->allocator);
    const bool post_method = request.method.equals("POST");
    if (!post_method && !request.method.equals("GET")) {
      response->error.assign("This browser cannot send that kind of request.");
      return false;
    }
    StringView content_type;   // the Content-Type line of the request, for a POST
    for (uint64_t at = 0; at < request.headers.size();) {
      uint64_t end = at;
      while (end < request.headers.size() && request.headers[end] != '\n') ++end;
      const StringView line = request.headers.substr(at, end - at);
      if (line.size() > 13 && line.first(13).equals_ignore_case("content-type:")) content_type = line.substr(13).trimmed();
      at = end + 1;
    }
    const bool ok = post_method ? post(request.url, content_type, StringView(request.body), &page) : fetch(request.url, &page);
    if (!ok) {
      response->error.assign(page.error.view());
      return false;
    }
    response->status = page.status;
    response->url.assign(page.final_address.view());
    response->headers.assign("Content-Type: ");
    response->headers.append(page.content_type.view());
    response->headers.append("\r\n");
    response->body.clear();
    return response->body.append(page.body.view());
  }
  // Fetches `address` (an http:// or https:// address). True if an answer arrived - whatever its status; false (with `error` set) if not.
  virtual bool fetch(StringView address, PageResult* result) = 0;
  // Sends `body` (of that content type) to `address` with POST - a form with method=post - and takes the page that comes back. The same answer as fetch().
  virtual bool post(StringView address, StringView content_type, StringView body, PageResult* result) {
    (void)address; (void)content_type; (void)body;
    result->error.assign("This browser cannot send forms with POST.");
    return false;
  }

 protected:
  IPageLoader() = default;
  ~IPageLoader() = default;
};

}  // namespace browser
