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

// The network and text natives of the DOM bindings: URL (a host object over WebUrl), the requests behind fetch() and XMLHttpRequest (the page's JavaScript side is
// in WebPrelude.cpp), and the helpers for bytes and UTF-8 text that Blob, TextEncoder, TextDecoder and URLSearchParams use.
#include "Browser/DomBindingsInternal.hpp"
#include "Media/Zlib.hpp"
#include "Utility/Crc32.hpp"
#include "Crypto/Crypto.hpp"
#include "Script/TypedArray.hpp"
#include "Utility/ByteText.hpp"
#include "Browser/WebUrl.hpp"
#include "Utility/ByteBuffer.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)
#define IS(text) k.equals(text)

JsString* key_of(Vm& vm, const char* name) { return vm.intern(name); }

// --- Text and bytes ------------------------------------------------------------------------------------------------------------

// Is there a valid UTF-8 character at text[i]? How long is it (0: no)?
uint32_t utf8_length(StringView text, uint64_t i) {
  const uint8_t b = static_cast<uint8_t>(text[i]);
  if (b < 0x80) return 1;
  auto cont = [&](uint64_t k) { return k < text.size() && (static_cast<uint8_t>(text[k]) & 0xC0) == 0x80; };
  if (b >= 0xC2 && b <= 0xDF) return cont(i + 1) ? 2 : 0;
  if (b >= 0xE0 && b <= 0xEF) {
    if (!cont(i + 1) || !cont(i + 2)) return 0;
    const uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
    if (b == 0xE0 && b1 < 0xA0) return 0;   // (overlong)
    if (b == 0xED && b1 >= 0xA0) return 0;  // (a surrogate)
    return 3;
  }
  if (b >= 0xF0 && b <= 0xF4) {
    if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
    const uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
    if (b == 0xF0 && b1 < 0x90) return 0;
    if (b == 0xF4 && b1 >= 0x90) return 0;
    return 4;
  }
  return 0;
}

NATIVE(net_utf8_clean) {
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  String clean(vm.allocator());
  for (uint64_t i = 0; i < text.size();) {
    const uint32_t n = utf8_length(text.view(), i);
    if (n == 0) {
      clean.append("\xEF\xBF\xBD");
      ++i;
      while (i < text.size() && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80 && utf8_length(text.view(), i) == 0) ++i;   // (one replacement for a damaged sequence)
    } else {
      clean.append(text.view().substr(i, n));
      i += n;
    }
  }
  *result = str_value(vm, clean.view());
  return true;
}

NATIVE(net_bytes_to_string) {
  StringView bytes;
  const Value a = arg_at(args, argc, 0);
  if (a.is_string()) {
    *result = a;
    return true;
  }
  if (!byte_array_bytes(a, &bytes)) return vm.throw_error(ErrorKind::TypeError, "The provided value is not a byte array.");
  *result = str_value(vm, bytes);
  return true;
}

NATIVE(net_string_to_bytes) {
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  *result = Value::make_object(make_byte_array(impl(vm), text.view()));
  return true;
}

NATIVE(net_form_encode) {
  String text(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  WebUrl::form_encode(text.view(), &out);
  *result = str_value(vm, out.view());
  return true;
}

NATIVE(net_form_decode) {
  String text(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  WebUrl::form_decode(text.view(), &out);
  *result = str_value(vm, out.view());
  return true;
}

// --- Compression -----------------------------------------------------------------------------------------------------------------

// __zlib(format, compress, bytes): "gzip", "deflate" (zlib) or "deflate-raw" compressed or decompressed; null if the data is not valid.
NATIVE(net_zlib) {
  (void)self;
  String format(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &format)) return false;
  const bool compress = Vm::to_boolean(arg_at(args, argc, 1));
  String input(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 2), &input)) return false;
  const ConstByteView in = input.view().bytes();
  ByteBuffer out(vm.allocator());
  *result = Value::make_null();
  const bool gzip = format.equals("gzip"), raw = format.equals("deflate-raw");
  if (compress) {
    if (gzip) {
      static const uint8_t kHeader[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};
      if (!out.append(ConstByteView(kHeader, 10))) return true;
    }
    if (!media::deflate(in, &out, 6, vm.allocator(), gzip || raw)) return true;
    if (gzip) {
      const uint32_t crc = crc32(in), size = static_cast<uint32_t>(in.size());
      uint8_t trailer[8];
      for (int i = 0; i < 4; ++i) {
        trailer[i] = static_cast<uint8_t>(crc >> (8 * i));
        trailer[4 + i] = static_cast<uint8_t>(size >> (8 * i));
      }
      if (!out.append(ConstByteView(trailer, 8))) return true;
    }
  } else if (gzip) {
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return true;
    const uint8_t flags = in[3];
    uint64_t at = 10;
    if (flags & 4) { if (at + 2 > in.size()) return true; at += 2 + (in[at] | (in[at + 1] << 8)); }
    if (flags & 8) { while (at < in.size() && in[at] != 0) ++at; ++at; }
    if (flags & 16) { while (at < in.size() && in[at] != 0) ++at; ++at; }
    if (flags & 2) at += 2;
    if (at + 8 > in.size()) return true;
    uint64_t used = 0;
    if (!media::inflate(in.subview(at, in.size() - at - 8), &out, 256u << 20, true, &used)) return true;
    const ConstByteView tail = in.subview(at + used);
    if (tail.size() < 8) return true;
    uint32_t crc = 0, size = 0;
    for (int i = 0; i < 4; ++i) {
      crc |= static_cast<uint32_t>(tail[i]) << (8 * i);
      size |= static_cast<uint32_t>(tail[4 + i]) << (8 * i);
    }
    if (crc != crc32(out.view()) || size != static_cast<uint32_t>(out.size())) return true;
  } else if (!media::inflate(in, &out, 256u << 20, raw)) {
    return true;
  }
  *result = str_value(vm, StringView(out.view()));
  return true;
}

// --- Requests ------------------------------------------------------------------------------------------------------------------

bool opt_text(Vm& vm, const Value& opts, const char* name, String* out) {
  Value v;
  if (!vm.get_named(opts, name, &v)) return false;
  if (v.is_nullish()) return true;
  JsString* s;
  if (!vm.to_string(v, &s)) return false;
  return out->assign(s->view()) || true;
}

// Makes the request an options object describes and builds the answer object: { ok, status, statusText, url, redirected, opaque, headers, body } or { ok: false, error }.
bool perform(Vm& vm, const Value& opts, Value* result) {
  DomBindings::Impl& c = impl(vm);
  String method(vm.allocator()), url(vm.allocator()), headers(vm.allocator()), mode(vm.allocator()), credentials(vm.allocator()), redirect(vm.allocator());
  method.assign("GET");
  mode.assign("cors");
  credentials.assign("same-origin");
  redirect.assign("follow");
  if (!opts.is_object()) return vm.throw_error(ErrorKind::TypeError, "request options are missing");
  if (!opt_text(vm, opts, "method", &method) || !opt_text(vm, opts, "url", &url) || !opt_text(vm, opts, "headers", &headers) || !opt_text(vm, opts, "mode", &mode) ||
      !opt_text(vm, opts, "credentials", &credentials) || !opt_text(vm, opts, "redirect", &redirect))
    return false;
  String referrer(vm.allocator()), referrer_policy(vm.allocator());
  Value referrer_value;
  const bool referrer_given = vm.get_named(opts, "referrer", &referrer_value) && referrer_value.is_string();
  if (!opt_text(vm, opts, "referrer", &referrer) || !opt_text(vm, opts, "referrerPolicy", &referrer_policy)) return false;
  ByteBuffer body(vm.allocator());
  Value b;
  if (!vm.get_named(opts, "body", &b)) return false;
  StringView bytes;
  if (b.is_string()) {
    body.append(b.as_string()->view().bytes());
  } else if (byte_array_bytes(b, &bytes)) {
    body.append(bytes.bytes());
  } else if (!b.is_nullish()) {
    String text(vm.allocator());
    if (!arg_text(vm, b, &text)) return false;
    body.append(text.view().bytes());
  }
  double timeout = 0;
  Value t;
  if (vm.get_named(opts, "timeout", &t) && t.is_number()) timeout = t.u.number;

  FetchOptions fo;
  fo.method = method.view();
  fo.url = url.view();
  fo.headers = headers.view();
  fo.body = body.view();
  fo.mode = mode.equals("no-cors") ? FetchOptions::Mode::NoCors : mode.equals("same-origin") ? FetchOptions::Mode::SameOrigin : mode.equals("navigate") ? FetchOptions::Mode::Navigate : FetchOptions::Mode::Cors;
  fo.credentials = credentials.equals("include") ? FetchOptions::Credentials::Include : credentials.equals("omit") ? FetchOptions::Credentials::Omit : FetchOptions::Credentials::SameOrigin;
  fo.redirect = redirect.equals("error") ? FetchOptions::Redirect::Error : redirect.equals("manual") ? FetchOptions::Redirect::Manual : FetchOptions::Redirect::Follow;
  fo.referrer_given = referrer_given;
  fo.referrer = referrer.view();
  fo.referrer_policy = referrer_policy.view();
  fo.page_address = c.host.address();
  fo.now_ms = vm.host() ? vm.host()->now_ms() : 0;
  fo.timeout_ms = timeout > 0 && timeout < 3.6e6 ? static_cast<uint32_t>(timeout) : 0;
  NetResponse response(vm.allocator());
  const bool ok = c.host.http_request(fo, &response);
  JsObject* r = vm.new_plain();
  *result = Value::make_object(r);
  vm.define_named(r, "ok", bool_value(ok));
  if (!ok) {
    vm.define_named(r, "error", str_value(vm, response.error.empty() ? StringView("Failed to fetch") : response.error.view()));
    return true;
  }
  vm.define_named(r, "status", num_value(response.status));
  vm.define_named(r, "statusText", str_value(vm, response.reason.view()));
  vm.define_named(r, "url", str_value(vm, response.url.view()));
  vm.define_named(r, "redirected", bool_value(response.redirected));
  vm.define_named(r, "opaque", bool_value(response.opaque));
  vm.define_named(r, "headers", str_value(vm, response.headers.view()));
  vm.define_named(r, "body", Value::make_object(make_byte_array(c, StringView(response.body.view()))));
  return true;
}

NATIVE(net_send_sync) { return perform(vm, arg_at(args, argc, 0), result); }

// The timer task of an asynchronous request: makes it, then hands the answer to the callback.
NATIVE(net_run) {
  Value answer;
  if (!perform(vm, arg_at(args, argc, 0), &answer)) return false;
  const Value callback = arg_at(args, argc, 1);
  if (!Vm::is_callable(callback)) return true;
  return vm.call(callback, Value(), &answer, 1, result);
}

// __net_send(options, callback): the request is made as a task of its own (after the script that asked, as the network would answer); returns the timer id.
NATIVE(net_send) {
  if (!arg_at(args, argc, 0).is_object() || !Vm::is_callable(arg_at(args, argc, 1))) return vm.throw_error(ErrorKind::TypeError, "request options and a callback are needed");
  Value runner;
  if (!vm.get_named(Value::make_object(vm.global()), "__net_run", &runner)) return false;
  const Value task_args[2] = {args[0], args[1]};
  *result = num_value(vm.add_timer(runner, 0, -1, task_args, 2));
  return true;
}

// --- crypto.subtle: __crypto_hash(kind, bytes) and __crypto_hmac(kind, key, bytes) give the digest as a byte array (kind 1 SHA-1, 2 SHA-256, 3 SHA-384, 4 SHA-512)
bool crypto_kind(Vm& vm, const Value& v, crypto::HashKind* kind) {
  double d;
  if (!vm.to_number(v, &d)) return false;
  switch (static_cast<int>(d)) {
    case 1: *kind = crypto::HashKind::Sha1; return true;
    case 2: *kind = crypto::HashKind::Sha256; return true;
    case 3: *kind = crypto::HashKind::Sha384; return true;
    case 4: *kind = crypto::HashKind::Sha512; return true;
  }
  return vm.throw_error(ErrorKind::TypeError, "Unrecognized hash algorithm");
}
NATIVE(crypto_hash) {
  crypto::HashKind kind;
  ArrayView<uint8_t> data;
  if (!crypto_kind(vm, arg_at(args, argc, 0), &kind)) return false;
  if (!typed_array_bytes(arg_at(args, argc, 1), &data)) return vm.throw_error(ErrorKind::TypeError, "The data must be an ArrayBuffer or a view of one");
  uint8_t out[crypto::kMaxHashSize];
  crypto::hash(kind, ConstByteView(data.data(), data.size()), out);
  *result = Value::make_object(make_byte_array(impl(vm), bytes_as_text(ConstByteView(out, crypto::hash_size(kind)))));
  return true;
}
NATIVE(crypto_hmac) {
  crypto::HashKind kind;
  ArrayView<uint8_t> key, data;
  if (!crypto_kind(vm, arg_at(args, argc, 0), &kind)) return false;
  if (!typed_array_bytes(arg_at(args, argc, 1), &key) || !typed_array_bytes(arg_at(args, argc, 2), &data)) return vm.throw_error(ErrorKind::TypeError, "The key and the data must be ArrayBuffers or views of one");
  uint8_t out[crypto::kMaxHashSize];
  crypto::hmac(kind, ConstByteView(key.data(), key.size()), ConstByteView(data.data(), data.size()), out);
  *result = Value::make_object(make_byte_array(impl(vm), bytes_as_text(ConstByteView(out, crypto::hash_size(kind)))));
  return true;
}

// --- WebSockets: __ws_open(url, protocols) -> id (0: not made, with the reason in __ws_error), __ws_state(id), __ws_send(id, data), __ws_close(id, code, reason),
// __ws_receive(id) -> null | string | byte array, __ws_info(id) -> [protocol, code, reason, opened], __ws_release(id)
NATIVE(ws_open) {
  String url(vm.allocator()), protocols(vm.allocator()), error(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &url) || !arg_text(vm, arg_at(args, argc, 1), &protocols)) return false;
  const uint32_t id = impl(vm).host.ws_open(url.view(), protocols.view(), &error);
  vm.define_named(vm.global(), "__ws_error", str_value(vm, error.view()), 0);
  *result = num_value(id);
  return true;
}
NATIVE(ws_state) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  *result = num_value(impl(vm).host.ws_state(static_cast<uint32_t>(id)));
  return true;
}
NATIVE(ws_send) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  const Value data = arg_at(args, argc, 1);
  bool ok;
  if (data.is_string()) {
    const StringView text = data.as_string()->view();
    ok = impl(vm).host.ws_send(static_cast<uint32_t>(id), true, text_as_bytes(text));
  } else {
    ArrayView<uint8_t> bytes;
    if (!typed_array_bytes(data, &bytes)) return vm.throw_error(ErrorKind::TypeError, "WebSocket.send needs a string or binary data");
    ok = impl(vm).host.ws_send(static_cast<uint32_t>(id), false, ConstByteView(bytes.data(), bytes.size()));
  }
  *result = bool_value(ok);
  return true;
}
NATIVE(ws_close) {
  double id, code = 1000;
  String reason(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &id) || (argc > 1 && !vm.to_number(args[1], &code)) || (argc > 2 && !arg_text(vm, args[2], &reason))) return false;
  impl(vm).host.ws_close(static_cast<uint32_t>(id), static_cast<uint16_t>(code), reason.view());
  return true;
}
NATIVE(ws_receive) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  bool text = false;
  ByteBuffer data(vm.allocator());
  *result = Value::make_null();
  if (!impl(vm).host.ws_receive(static_cast<uint32_t>(id), &text, &data)) return true;
  const StringView bytes = bytes_as_text(data.view());
  *result = text ? str_value(vm, bytes) : Value::make_object(make_byte_array(impl(vm), bytes));
  return true;
}
NATIVE(ws_info) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  String protocol(vm.allocator()), reason(vm.allocator());
  uint16_t code = 1006;
  bool opened = false;
  impl(vm).host.ws_info(static_cast<uint32_t>(id), &protocol, &code, &reason, &opened);
  JsObject* a = vm.new_array(4);
  vm.array_push(a, str_value(vm, protocol.view()));
  vm.array_push(a, num_value(code));
  vm.array_push(a, str_value(vm, reason.view()));
  vm.array_push(a, bool_value(opened));
  *result = Value::make_object(a);
  return true;
}
NATIVE(ws_release) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  impl(vm).host.ws_release(static_cast<uint32_t>(id));
  return true;
}

// --- Web Workers: __worker_start(source, url, name) -> id (0: not started, the reason in __worker_error), __worker_post(id, text), __worker_terminate(id),
// __worker_pump() -> [[id, kind, text] ...] (kind 0 message, 1 error, 2 closed)
NATIVE(worker_start) {
  String source(vm.allocator()), url(vm.allocator()), name(vm.allocator()), error(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &source) || !arg_text(vm, arg_at(args, argc, 1), &url) || !arg_text(vm, arg_at(args, argc, 2), &name)) return false;
  const uint32_t id = impl(vm).host.worker_start(source.view(), url.view(), name.view(), &error);
  vm.define_named(vm.global(), "__worker_error", str_value(vm, error.view()), 0);
  *result = num_value(id);
  return true;
}
NATIVE(worker_post) {
  double id;
  String text(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &id) || !arg_text(vm, arg_at(args, argc, 1), &text)) return false;
  impl(vm).host.worker_post(static_cast<uint32_t>(id), text.view());
  return true;
}
NATIVE(worker_terminate) {
  double id;
  if (!vm.to_number(arg_at(args, argc, 0), &id)) return false;
  impl(vm).host.worker_terminate(static_cast<uint32_t>(id));
  return true;
}
NATIVE(worker_pump) {
  DomBindings::Impl& c = impl(vm);
  c.host.worker_pump();
  JsObject* out = vm.new_array();
  *result = Value::make_object(out);
  uint32_t id, kind, guard = 0;
  String text(vm.allocator());
  while (guard++ < 500 && c.host.worker_next_event(&id, &kind, &text)) {
    JsObject* entry = vm.new_array(3);
    vm.array_push(entry, num_value(id));
    vm.array_push(entry, num_value(kind));
    vm.array_push(entry, str_value(vm, text.view()));
    vm.array_push(out, Value::make_object(entry));
  }
  return true;
}

// __run_hook(name, ...arguments): calls the global function `name` (a timer task that DomBindings::schedule_hook makes).
NATIVE(run_hook) {
  Value fn;
  if (argc == 0 || !args[0].is_string() || !vm.get_named(Value::make_object(vm.global()), args[0].as_string()->view(), &fn) || !Vm::is_callable(fn)) return true;
  return vm.call(fn, Value(), args + 1, argc - 1, result);
}

// --- Windows, message ports and broadcasts: the JavaScript side (WebPrelude.cpp) keeps the objects; the page tree routes the messages ----------------------

NATIVE(win_window_id) { *result = num_value(impl(vm).host.window_id()); return true; }
NATIVE(win_parent_window_id) { *result = num_value(impl(vm).host.parent_window_id()); return true; }
NATIVE(win_top_window_id) { *result = num_value(impl(vm).host.top_window_id()); return true; }
NATIVE(win_frame_window_id) {
  double serial;
  if (!vm.to_number(arg_at(args, argc, 0), &serial)) return false;
  *result = num_value(impl(vm).host.frame_window_id(static_cast<uint32_t>(serial)));
  return true;
}
// __post_message(window, targetOrigin, payload, ports): true if the message was sent (the window exists and the origin matched)
NATIVE(win_post_message) {
  double target;
  String origin(vm.allocator()), payload(vm.allocator()), ports(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &target) || !arg_text(vm, arg_at(args, argc, 1), &origin) || !arg_text(vm, arg_at(args, argc, 2), &payload) || !arg_text(vm, arg_at(args, argc, 3), &ports)) return false;
  *result = bool_value(impl(vm).host.post_window_message(static_cast<uint32_t>(target), origin.view(), payload.view(), ports.view()));
  return true;
}
NATIVE(port_pair) { *result = num_value(impl(vm).host.new_port_pair()); return true; }
NATIVE(port_move) {
  double port, window;
  if (!vm.to_number(arg_at(args, argc, 0), &port) || !vm.to_number(arg_at(args, argc, 1), &window)) return false;
  impl(vm).host.move_port(static_cast<uint32_t>(port), static_cast<uint32_t>(window));
  return true;
}
NATIVE(port_post) {
  double port;
  String payload(vm.allocator()), ports(vm.allocator());
  if (!vm.to_number(arg_at(args, argc, 0), &port) || !arg_text(vm, arg_at(args, argc, 1), &payload) || !arg_text(vm, arg_at(args, argc, 2), &ports)) return false;
  impl(vm).host.post_port_message(static_cast<uint32_t>(port), payload.view(), ports.view());
  return true;
}
NATIVE(broadcast_post) {
  String channel(vm.allocator()), payload(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &channel) || !arg_text(vm, arg_at(args, argc, 1), &payload)) return false;
  impl(vm).host.broadcast(channel.view(), payload.view());
  return true;
}

// --- URL ----------------------------------------------------------------------------------------------------------------------

bool url_part(StringView k, WebUrl::Part* part) {
  if (IS("href")) *part = WebUrl::Part::Href;
  else if (IS("protocol")) *part = WebUrl::Part::Protocol;
  else if (IS("username")) *part = WebUrl::Part::Username;
  else if (IS("password")) *part = WebUrl::Part::Password;
  else if (IS("host")) *part = WebUrl::Part::Host;
  else if (IS("hostname")) *part = WebUrl::Part::Hostname;
  else if (IS("port")) *part = WebUrl::Part::Port;
  else if (IS("pathname")) *part = WebUrl::Part::Pathname;
  else if (IS("search")) *part = WebUrl::Part::Search;
  else if (IS("hash")) *part = WebUrl::Part::Hash;
  else return false;
  return true;
}

bool load_url(Vm& vm, JsObject* o, WebUrl* out) {
  Value v;
  if (!vm.get_plain(o, key_of(vm, "__href"), &v) || !v.is_string()) return false;
  return WebUrl::parse(vm.allocator(), v.as_string()->view(), nullptr, out);
}

void store_url(Vm& vm, JsObject* o, const WebUrl& url) {
  String text(vm.allocator());
  url.serialize(&text);
  vm.define(o, key_of(vm, "__href"), str_value(vm, text.view()), kWritable);
}

bool url_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  const StringView k = key->view();
  WebUrl::Part part;
  const bool is_origin = IS("origin"), is_params = IS("searchParams");
  if (!url_part(k, &part) && !is_origin && !is_params) return false;
  if (is_params) {
    Value sp;
    if (vm.get_plain(o, key_of(vm, "__sp"), &sp) && sp.is_object()) {
      *out = sp;
      return true;
    }
    Value make;
    if (!vm.get_named(Value::make_object(vm.global()), "__make_search_params", &make) || !Vm::is_callable(make)) return false;
    const Value arg = Value::make_object(o);
    if (!vm.call(make, Value(), &arg, 1, &sp)) {
      String ignored(vm.allocator());
      vm.take_exception(&ignored);
      return false;
    }
    vm.define(o, key_of(vm, "__sp"), sp, kWritable);
    *out = sp;
    return true;
  }
  WebUrl url(vm.allocator());
  if (!load_url(vm, o, &url)) return false;
  String text(vm.allocator());
  if (is_origin) url.origin(&text);
  else url.get(part, &text);
  *out = str_value(vm, text.view());
  return true;
}

HostClass::Set url_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  const StringView k = key->view();
  WebUrl::Part part;
  if (!url_part(k, &part)) return HostClass::Set::NotHandled;
  String text(vm.allocator());
  if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
  WebUrl url(vm.allocator());
  if (!load_url(vm, o, &url)) return HostClass::Set::Handled;
  if (!url.set(part, text.view())) {
    if (part == WebUrl::Part::Href) {
      vm.throw_error(ErrorKind::TypeError, "Failed to set the 'href' property on 'URL': Invalid URL");
      return HostClass::Set::Threw;
    }
    return HostClass::Set::Handled;
  }
  store_url(vm, o, url);
  if (part == WebUrl::Part::Href || part == WebUrl::Part::Search) {   // searchParams follows the query
    Value sp, reset;
    if (vm.get_plain(o, key_of(vm, "__sp"), &sp) && sp.is_object() && vm.get_named(sp, "__reset", &reset) && Vm::is_callable(reset)) {
      String search(vm.allocator());
      url.get(WebUrl::Part::Search, &search);
      const Value arg = str_value(vm, search.view());
      Value ignored;
      if (!vm.call(reset, sp, &arg, 1, &ignored)) return HostClass::Set::Threw;
    }
  }
  return HostClass::Set::Handled;
}

const HostClass kUrlClass = {"URL", url_get, url_set, nullptr, nullptr};

bool parse_with_base(Vm& vm, const Value& input, const Value& base, WebUrl* out, bool* threw) {
  *threw = false;
  String text(vm.allocator());
  if (!arg_text(vm, input, &text)) {
    *threw = true;
    return false;
  }
  if (base.is_nullish()) return WebUrl::parse(vm.allocator(), text.view(), nullptr, out);
  WebUrl base_url(vm.allocator());
  if (base.is_object() && base.as_object()->host == &kUrlClass) {
    if (!load_url(vm, base.as_object(), &base_url)) return false;
  } else {
    String base_text(vm.allocator());
    if (!arg_text(vm, base, &base_text)) {
      *threw = true;
      return false;
    }
    if (!WebUrl::parse(vm.allocator(), base_text.view(), nullptr, &base_url)) return false;
  }
  return WebUrl::parse(vm.allocator(), text.view(), &base_url, out);
}

NATIVE(url_constructor) {
  if (!self.is_object() || self.as_object() == vm.global() || self.as_object()->host) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'URL': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
  if (argc == 0) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'URL': 1 argument required, but only 0 present.");
  WebUrl url(vm.allocator());
  bool threw;
  if (!parse_with_base(vm, args[0], arg_at(args, argc, 1), &url, &threw)) {
    if (threw) return false;
    return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'URL': Invalid URL");
  }
  JsObject* o = self.as_object();
  o->host = &kUrlClass;
  store_url(vm, o, url);
  *result = self;
  return true;
}

NATIVE(url_can_parse) {
  WebUrl url(vm.allocator());
  bool threw;
  const bool ok = parse_with_base(vm, arg_at(args, argc, 0), arg_at(args, argc, 1), &url, &threw);
  if (threw) return false;
  *result = bool_value(ok);
  return true;
}

NATIVE(url_to_string) {
  Value href;
  if (!self.is_object() || self.as_object()->host != &kUrlClass || !vm.get_named(self, "href", &href)) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  *result = href;
  return true;
}

// __url_resolve(text, base): the address `text` leads to from `base`, or null.
NATIVE(url_resolve) {
  WebUrl url(vm.allocator());
  bool threw;
  if (!parse_with_base(vm, arg_at(args, argc, 0), arg_at(args, argc, 1), &url, &threw)) {
    if (threw) return false;
    *result = Value::make_null();
    return true;
  }
  String text(vm.allocator());
  url.serialize(&text);
  *result = str_value(vm, text.view());
  return true;
}

}  // namespace

bool install_net(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__net_send", net_send, 2);
  vm.define_native(g, "__zlib", net_zlib, 3);
  vm.define_native(g, "__net_run", net_run, 2);
  vm.define_native(g, "__net_send_sync", net_send_sync, 1);
  vm.define_native(g, "__utf8_clean", net_utf8_clean, 1);
  vm.define_native(g, "__bytes_to_string", net_bytes_to_string, 1);
  vm.define_native(g, "__string_to_bytes", net_string_to_bytes, 1);
  vm.define_native(g, "__form_encode", net_form_encode, 1);
  vm.define_native(g, "__form_decode", net_form_decode, 1);
  vm.define_native(g, "__url_resolve", url_resolve, 2);
  vm.define_native(g, "__run_hook", run_hook, 1);
  vm.define_native(g, "__worker_start", worker_start, 3);
  vm.define_native(g, "__worker_post", worker_post, 2);
  vm.define_native(g, "__worker_terminate", worker_terminate, 1);
  vm.define_native(g, "__worker_pump", worker_pump, 0);
  vm.define_native(g, "__crypto_hash", crypto_hash, 2);
  vm.define_native(g, "__crypto_hmac", crypto_hmac, 3);
  vm.define_native(g, "__ws_open", ws_open, 2);
  vm.define_native(g, "__ws_state", ws_state, 1);
  vm.define_native(g, "__ws_send", ws_send, 2);
  vm.define_native(g, "__ws_close", ws_close, 3);
  vm.define_native(g, "__ws_receive", ws_receive, 1);
  vm.define_native(g, "__ws_info", ws_info, 1);
  vm.define_native(g, "__ws_release", ws_release, 1);
  vm.define_native(g, "__window_id", win_window_id, 0);
  vm.define_native(g, "__parent_window_id", win_parent_window_id, 0);
  vm.define_native(g, "__top_window_id", win_top_window_id, 0);
  vm.define_native(g, "__frame_window_id", win_frame_window_id, 1);
  vm.define_native(g, "__post_message", win_post_message, 4);
  vm.define_native(g, "__port_pair", port_pair, 0);
  vm.define_native(g, "__port_move", port_move, 2);
  vm.define_native(g, "__port_post", port_post, 3);
  vm.define_native(g, "__broadcast", broadcast_post, 2);
  JsObject* proto = vm.new_plain();
  vm.define_native(proto, "toString", url_to_string, 0);
  vm.define_native(proto, "toJSON", url_to_string, 0);
  JsObject* ctor = vm.new_native(url_constructor, "URL", 1);
  vm.define(ctor, vm.prototype_name(), Value::make_object(proto), 0);
  vm.define(proto, vm.intern("constructor"), Value::make_object(ctor), kWritable);
  vm.define_native(ctor, "canParse", url_can_parse, 1);
  vm.define_named(g, "URL", Value::make_object(ctor), kWritable);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
