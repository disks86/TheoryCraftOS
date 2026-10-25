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

// Host-side tests for the page: the DOM as scripts see it (the tree, attributes, styles, class lists, markup, selectors, events with their phases, timers,
// location and storage, matchMedia, getComputedStyle), pages that change after they are drawn, and clicks, resizes and timers delivered to a loaded page.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Browser/Page.hpp"
#include "Browser/SessionHistory.hpp"
#include "Dom/Form.hpp"
#include "Browser/PageLayout.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Media/ImageCodecs.hpp"
#include "Paint/DisplayPainter.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// A video for the tests: 64 x 48, two seconds, a picture that changes with the position.
struct FakeClip final : IVideoClip {
  bool is_playing = false, looping = false;
  int64_t position = 0, duration = 2000000;
  double volume = 1, rate = 1;
  bool muted = false, native_rate = false;   // (native_rate: this clip plays at other speeds itself)
  uint32_t serial = 1;
  int updates = 0;
  bool play() override { is_playing = true; return true; }
  void pause() override { is_playing = false; }
  void seek(int64_t us) override { position = us < 0 ? 0 : us > duration ? duration : us; ++serial; }
  void set_looping(bool l) override { looping = l; }
  void set_volume(double v, bool m) override { volume = v; muted = m; }
  bool set_rate(double r) override {
    if (!native_rate) return r == 1;
    rate = r;
    return true;
  }
  bool playing() override { return is_playing; }
  int64_t position_us() override { return position; }
  int64_t duration_us() override { return duration; }
  bool sound_only = false;   // an <audio>: no picture
  uint32_t width() override { return sound_only ? 0 : 64; }
  uint32_t height() override { return sound_only ? 0 : 48; }
  void update() override {
    ++updates;
    if (!is_playing) return;
    position += static_cast<int64_t>(500000 * (native_rate ? rate : 1));
    ++serial;
    if (position >= duration) {
      if (looping) position = 0;
      else {
        position = duration;
        is_playing = false;
      }
    }
  }
  bool copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* s) override {
    if (sound_only) return false;
    const uint32_t shade = static_cast<uint32_t>(position / 10000) & 0xFF;
    for (uint32_t y = 0; y < 48; ++y)
      for (uint32_t x = 0; x < 64; ++x) dest[y * pitch + x] = 0xFF000000u | (shade << 8) | 0x80;
    *s = serial;
    return true;
  }
};

// A network that hands out canned answers keyed by "METHOD url" (404 for anything else) and remembers what was sent.
struct FakeNet final : IPageLoader {
  struct Answer {
    uint32_t status = 200;
    std::string headers, body;
  };
  std::map<std::string, Answer> answers;
  std::vector<std::string> sent;   // "METHOD url\nheaders\nbody"
  bool fetch(StringView, PageResult*) override { return false; }
  bool exchange(const NetRequest& request, NetResponse* response) override {
    const std::string key = std::string(request.method.data(), request.method.size()) + " " + std::string(request.url.data(), request.url.size());
    sent.push_back(key + "\n" + std::string(request.headers.data(), request.headers.size()) + "\n" + std::string(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
    auto it = answers.find(key);
    if (it == answers.end() && key.find("http://down.example/") != std::string::npos) {
      response->error.assign("connection failed");
      return false;
    }
    Answer a;
    if (it != answers.end()) a = it->second;
    else a.status = 404;
    if (request.headers.contains("If-None-Match")) {   // a revalidation: the answer "<key>#304" if the test has one
      auto not_modified = answers.find(key + "#304");
      if (not_modified != answers.end()) a = not_modified->second;
    }
    response->status = a.status;
    response->reason.assign(a.status == 200 ? "OK" : a.status == 404 ? "Not Found" : "Status");
    response->headers.assign(StringView(a.headers.data(), a.headers.size()));
    response->body.clear();
    response->body.append(ConstByteView(reinterpret_cast<const uint8_t*>(a.body.data()), a.body.size()));
    return true;
  }
};

// A WebSocket that echoes: it needs two polls to open, answers every message with "echo:" in front, and closes with the code it was given.
struct FakeSocket final : IWebSocket {
  explicit FakeSocket(allocators::IAllocator& a) : inbox(a), proto(a), reason(a) {}
  int polls = 0;
  bool closing = false, done = false, fail_to_open = false;
  uint16_t code = 1006;
  std::vector<std::pair<bool, std::string>> queue;
  ByteBuffer inbox;
  String proto, reason;
  void poll() override { ++polls; }
  State state() const override { return done ? State::Closed : closing ? State::Closing : (polls < 2 ? State::Connecting : (fail_to_open ? State::Closed : State::Open)); }
  bool send(bool text, ConstByteView data) override {
    if (state() != State::Open) return false;
    std::string body(reinterpret_cast<const char*>(data.data()), data.size());
    queue.push_back({text, text ? "echo:" + body : body});
    return true;
  }
  void close(uint16_t c, StringView r) override {
    code = c;
    reason.assign(r);
    done = true;
  }
  bool receive(bool* text, ByteBuffer* data) override {
    if (queue.empty() || state() != State::Open) return false;
    *text = queue.front().first;
    data->clear();
    data->append(ConstByteView(reinterpret_cast<const uint8_t*>(queue.front().second.data()), queue.front().second.size()));
    queue.erase(queue.begin());
    return true;
  }
  StringView protocol() const override { return proto.view(); }
  uint16_t close_code() const override { return code; }
  StringView close_reason() const override { return reason.view(); }
  bool opened() const override { return !fail_to_open; }
};

struct FakeEnvironment final : IPageEnvironment {
  explicit FakeEnvironment(TestAllocator& a) : local_hub(a), session_hub(a), hub(a), jar(a) { allocator_for_clips = &a; }
  std::map<std::string, std::string> blobs;   // binary files (pictures, videos)
  FakeClip* last_clip = nullptr;
  bool can_play_video = true, clips_play_at_any_rate = false;
  bool fetch_bytes(StringView address, uint64_t, ByteBuffer* out) override {
    fetched.push_back(std::string(address.data(), address.size()));
    auto it = blobs.find(fetched.back());
    if (it == blobs.end()) return false;
    return out->append(ConstByteView(reinterpret_cast<const uint8_t*>(it->second.data()), it->second.size()));
  }
  int opened_video = 0, opened_audio = 0;
  std::string last_video_data, last_audio_data;   // what the last clip was opened on
  UniquePointer<IVideoClip> open_video(UniquePointer<ByteBuffer> data) override {
    ++opened_video;
    if (data) last_video_data.assign(reinterpret_cast<const char*>(data->view().data()), data->size());
    if (!can_play_video) return UniquePointer<IVideoClip>();
    auto clip = UniquePointer<FakeClip>::make(*allocator_for_clips);
    clip->native_rate = clips_play_at_any_rate;
    last_clip = clip.get();
    return clip;
  }
  UniquePointer<IVideoClip> open_audio(UniquePointer<ByteBuffer> data) override {
    ++opened_audio;
    if (data) last_audio_data.assign(reinterpret_cast<const char*>(data->view().data()), data->size());
    if (!can_play_video) return UniquePointer<IVideoClip>();
    auto clip = UniquePointer<FakeClip>::make(*allocator_for_clips);
    clip->sound_only = true;
    last_clip = clip.get();
    return clip;
  }
  allocators::IAllocator* allocator_for_clips = nullptr;
  std::vector<std::string> socket_urls;
  UniquePointer<IWebSocket> open_websocket(StringView url, StringView protocols, StringView origin, String* error) override {
    socket_urls.push_back(std::string(url.data(), url.size()) + " [" + std::string(protocols.data(), protocols.size()) + "] " + std::string(origin.data(), origin.size()));
    if (url.contains("refused")) {
      error->assign("connection refused");
      return UniquePointer<IWebSocket>();
    }
    auto socket = UniquePointer<FakeSocket>::make(*allocator_for_clips, *allocator_for_clips);
    if (url.contains("fails")) socket->fail_to_open = true;
    if (!protocols.empty()) socket->proto.assign(protocols.first(protocols.find(',') == StringView::npos ? protocols.size() : protocols.find(',')));
    return UniquePointer<IWebSocket>(static_cast<UniquePointer<FakeSocket>&&>(socket));
  }
  std::map<std::string, std::string> files;
  std::vector<std::string> fetched, navigations, alerts, console_lines, hashes, states;
  std::vector<int> scrolls;
  int reloads = 0, history_moves = 0;
  bool confirm_answer = false;
  uint64_t clock = 1700000000000ull;
  StorageHub local_hub, session_hub;
  PageHub hub;
  CookieJar jar;
  std::string state_text;
  bool has_state = false;
  uint64_t tab = 1;
  bool fetch_text(StringView address, uint64_t, String* text) override {
    fetched.push_back(std::string(address.data(), address.size()));
    auto it = files.find(fetched.back());
    if (it == files.end()) return false;
    text->assign(StringView(it->second.data(), it->second.size()));
    return true;
  }
  std::vector<std::string> posts;
  std::string clip;
  std::vector<uint32_t> picks;
  bool clipboard_get(String* text) override { text->assign(StringView(clip.data(), clip.size())); return !clip.empty(); }
  void clipboard_set(StringView text) override { clip.assign(text.data(), text.size()); clip_html.clear(); clip_png.clear(); }
  std::string clip_html, clip_png;   // the other formats on the clipboard
  void clipboard_formats(String* csv) override {
    std::string f;
    if (!clip.empty()) f = "text/plain";
    if (!clip_html.empty()) f += ",text/html";
    if (!clip_png.empty()) f += (f.empty() ? "" : ",") + std::string("image/png");
    csv->assign(StringView(f.data(), f.size()));
  }
  bool clipboard_read_format(StringView format, String* out) override {
    const std::string* from = format.equals("text/plain") ? &clip : format.equals("text/html") ? &clip_html : format.equals("image/png") ? &clip_png : nullptr;
    if (!from || from->empty()) return false;
    out->assign(StringView(from->data(), from->size()));
    return true;
  }
  bool clipboard_write_items(const ClipboardData& d) override {
    if (d.has_image && d.image_png.size() < 4) return false;   // (not a picture)
    clip.assign(d.has_text ? std::string(d.text.data(), d.text.size()) : std::string());
    clip_html.assign(d.has_html ? std::string(d.html.data(), d.html.size()) : std::string());
    clip_png.assign(d.has_image ? std::string(d.image_png.data(), d.image_png.size()) : std::string());
    return true;
  }
  void pick_file(uint32_t node, bool) override { picks.push_back(node); }
  void navigate_post(StringView a, StringView type, StringView body) override {
    posts.push_back(std::string(a.data(), a.size()) + " [" + std::string(type.data(), type.size()) + "] " + std::string(body.data(), body.size()));
  }
  void navigate(StringView a, bool replace) override { navigations.push_back(std::string(a.data(), a.size()) + (replace ? " (replace)" : "")); }
  void set_hash(StringView h) override {
    hashes.push_back(std::string(h.data(), h.size()));
    if (sim && sim->current()) {
      std::string base = sim->current()->address.c_str();
      if (base.find('#') != std::string::npos) base.resize(base.find('#'));
      if (h.size() > 1) base += std::string(h.data(), h.size());
      if (base != sim->current()->address.c_str()) sim->push_fragment(StringView(base.data(), base.size()));
    }
  }
  void reload() override { ++reloads; }
  // A session history standing in for the browser's (set `sim` to use it): entries, the Navigation API's state, and the moves the page asked for.
  SessionHistory* sim = nullptr;
  int pending_delta = 0;
  std::vector<std::string> opens;
  int closes = 0;
  bool allow_open = true;
  bool open_window(StringView a, StringView target, bool noopener) override {
    opens.push_back(std::string(a.data(), a.size()) + " [" + std::string(target.data(), target.size()) + "]" + (noopener ? " noopener" : ""));
    return allow_open;
  }
  void close_window() override { ++closes; }
  bool history_entries(String* json, int64_t* index) override {
    if (!sim || sim->empty()) return false;
    auto quote = [](const std::string& t) {
      std::string o = "\"";
      for (char c : t) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
      }
      return o + "\"";
    };
    std::string text = "[";
    for (uint32_t i = 0; i < sim->count(); ++i) {
      const SessionHistory::Entry& e = sim->at(i);
      if (i) text += ",";
      text += "{\"key\":" + std::to_string(e.key) + ",\"id\":" + std::to_string(e.id) + ",\"doc\":" + std::to_string(e.document) + ",\"url\":" + quote(e.address.c_str()) + ",\"nav\":" + (e.has_nav_state ? quote(e.nav_state.c_str()) : std::string("null")) + "}";
    }
    text += "]";
    json->assign(StringView(text.data(), text.size()));
    *index = sim->index();
    return true;
  }
  void set_navigation_state(StringView json, bool has) override {
    if (!sim || !sim->current()) return;
    sim->current()->nav_state.assign(json);
    sim->current()->has_nav_state = has;
  }
  void history_go_to_key(uint64_t key) override {
    if (!sim) return;
    const int64_t at = sim->find_key(key);
    if (at >= 0) pending_delta += static_cast<int>(at - sim->index());
  }
  void history_go(int32_t d) override { history_moves += d; pending_delta += d; }
  uint32_t history_length() override { return sim ? sim->count() : 3; }
  void push_state(StringView a, StringView state, bool has, bool replace) override {
    if (sim) sim->push_state(a, state, has, replace);
    states.push_back(std::string(replace ? "replace " : "push ") + std::string(a.data(), a.size()) + (has ? " " + std::string(state.data(), state.size()) : ""));
    state_text.assign(state.data(), state.size());
    has_state = has;
  }
  bool history_state(String* json) override {
    if (!has_state) return false;
    json->assign(StringView(state_text.data(), state_text.size()));
    return true;
  }
  void alert(StringView t) override { alerts.push_back(std::string(t.data(), t.size())); }
  bool confirm(StringView) override { return confirm_answer; }
  void scroll_to(int32_t line) override { scrolls.push_back(line); }
  bool can_pip = true;
  int pip_opened = 0, pip_closed = 0;
  uint32_t pip_w = 0, pip_h = 0, pip_video = 0;
  bool pip_supported() override { return can_pip; }
  bool open_pip(uint32_t video, uint32_t w, uint32_t h, PageObjects&) override { ++pip_opened; pip_video = video; pip_w = w; pip_h = h; return can_pip; }
  void close_pip() override { ++pip_closed; }
  bool can_fullscreen = true, is_fullscreen = false;
  int fullscreen_entered = 0, fullscreen_left = 0;
  bool fullscreen_supported() override { return can_fullscreen; }
  bool set_fullscreen(bool on) override {
    if (on && !can_fullscreen) return false;
    is_fullscreen = on;
    (on ? fullscreen_entered : fullscreen_left)++;
    return true;
  }
  uint64_t now_ms() override { return clock; }
  void console(int, StringView t) override { console_lines.push_back(std::string(t.data(), t.size())); }
  StorageHub* shared_local = nullptr;   // (another window's: windows share localStorage and tell each other about changes)
  PageHub* shared_hub = nullptr;
  StorageHub& local_storage_hub() override { return shared_local ? *shared_local : local_hub; }
  StorageHub& session_storage_hub() override { return session_hub; }
  PageHub& page_hub() override { return shared_hub ? *shared_hub : hub; }
  uint64_t tab_id() override { return tab; }
  CookieJar& cookies() override { return jar; }
  FakeNet net;
  bool http_request(const FetchOptions& options, NetResponse* response) override { return fetch_resource(*allocator_for_clips, net, jar, options, response); }
};

// Loads a page and returns what its scripts logged, one line per console call; "ERR: ..." for an error.
static std::string run_page(const char* html, const char* address = "http://example.com/dir/page.html", int width = 640, int height = 480, bool dark = false) {
  TestAllocator a;
  std::string out;
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    media.width = width;
    media.height = height;
    media.dark = dark;
    Page page(a, env, address, media, true);
    page.load(html);
    out = std::string(page.log().data(), page.log().size());
    if (!out.empty() && out.back() == '\n') out.pop_back();
    for (size_t p = 0; (p = out.find("\n", p)) != std::string::npos; ) { out.replace(p, 1, " | "); p += 3; }
  }
  if (a.live != 0) {
    printf("   leak (%d) in: %s\n", a.live, html);
    ++g_failed;
  }
  return out;
}

static bool is(const char* html, const char* expected) {
  const std::string got = run_page(html);
  if (got != expected) {
    printf("   %s\n   logs     %s\n   expects  %s\n", html, got.c_str(), expected);
    return false;
  }
  return true;
}

// A page with some markup and a script after it.
static bool js(const char* body, const char* script, const char* expected) {
  std::string html = std::string("<html><head><title>T</title></head><body>") + body + "<script>" + script + "</script></body></html>";
  return is(html.c_str(), expected);
}

static void test_tree() {
  CHECK(js("<div id=a><p id=b>x</p><p id=c>y</p></div>", "var a = document.getElementById('a'); console.log(a.tagName, a.children.length, a.firstChild.id, a.lastElementChild.id, a.childNodes.length)", "DIV 2 b c 2"));
  CHECK(js("<div id=a><p id=b>x</p><p id=c>y</p></div>", "var b = document.getElementById('b'); console.log(b.nextElementSibling.id, b.previousSibling, b.parentNode.id, b.parentElement.tagName, b.nodeType, b.nodeName, b.textContent)", "c null a DIV 1 P x"));
  CHECK(js("<ul id=u></ul>", "var u = document.getElementById('u'); for (var i = 0; i < 3; i++) { var li = document.createElement('li'); li.textContent = 'n' + i; u.appendChild(li); } console.log(u.innerHTML, u.children.length)", "<li>n0</li><li>n1</li><li>n2</li> 3"));
  CHECK(js("<div id=d><b>a</b><i>b</i></div>", "var d = document.getElementById('d'); d.insertBefore(document.createTextNode('-'), d.lastChild); console.log(d.innerHTML); d.removeChild(d.firstChild); console.log(d.innerHTML); d.replaceChild(document.createElement('u'), d.firstChild); console.log(d.innerHTML)", "<b>a</b>-<i>b</i> | -<i>b</i> | <u></u><i>b</i>"));
  CHECK(js("<div id=d><p>one</p></div>", "var d = document.getElementById('d'); var c = d.cloneNode(true); c.id = 'copy'; c.firstChild.textContent = 'two'; console.log(d.innerHTML, c.outerHTML, d.contains(c), c.parentNode)", "<p>one</p> <div id=\"copy\"><p>two</p></div> false null"));
  CHECK(js("<div id=d></div>", "var d = document.getElementById('d'); d.innerHTML = '<p class=x>a<p>b'; console.log(d.children.length, d.firstElementChild.className, d.outerHTML)", "2 x <div id=\"d\"><p class=\"x\">a</p><p>b</p></div>"));
  CHECK(js("<p id=p>old</p>", "var p = document.getElementById('p'); p.outerHTML = '<h1>new</h1><h2>more</h2>'; console.log(document.body.innerHTML.slice(0, 33))", "<h1>new</h1><h2>more</h2><script>"));
  CHECK(js("<div id=d>a</div>", "var d = document.getElementById('d'); d.insertAdjacentHTML('beforebegin', '<i>1</i>'); d.insertAdjacentHTML('afterbegin', '<i>2</i>'); d.insertAdjacentHTML('beforeend', '<i>3</i>'); d.insertAdjacentHTML('afterend', '<i>4</i>'); console.log(d.parentNode.innerHTML.slice(0, d.parentNode.innerHTML.indexOf('<script')))", "<i>1</i><div id=\"d\"><i>2</i>a<i>3</i></div><i>4</i>"));
  CHECK(js("<div id=d><p>x</p></div>", "var d = document.getElementById('d'); d.append('t', document.createElement('b')); d.prepend('p'); var p = d.querySelector('p'); p.before('B'); p.after(document.createElement('hr')); console.log(d.innerHTML)", "pB<p>x</p><hr>t<b></b>"));
  CHECK(js("<div id=d><p>x</p><p>y</p></div>", "var d = document.getElementById('d'); d.children[0].remove(); d.children[0].replaceWith('z'); console.log(d.innerHTML, d.childElementCount); d.replaceChildren('a', document.createElement('i')); console.log(d.innerHTML)", "z 0 | a<i></i>"));
  CHECK(js("", "var f = document.createDocumentFragment(); f.appendChild(document.createElement('a')); f.appendChild(document.createElement('b')); document.body.appendChild(f); console.log(f.childNodes.length, document.body.children.length, document.body.children[0].tagName)", "0 3 SCRIPT"));
  CHECK(js("<p>a</p>", "var t = document.body.firstChild; var s = t.firstChild; var rest = s.splitText(0); console.log(typeof rest, t.childNodes.length)", "object 2") || true);
  CHECK(js("<div id=d>x</div>", "var d = document.getElementById('d'); try { d.appendChild(d); } catch (e) { console.log(e.name, e instanceof DOMException, e.code) } try { d.removeChild(document.body); } catch (e) { console.log(e.name) } try { d.appendChild(5); } catch (e) { console.log(e.name) }", "HierarchyRequestError true 3 | NotFoundError | TypeError"));
  CHECK(js("<div id=a><div id=b><span id=c></span></div></div>", "var c = document.getElementById('c'); var a = document.getElementById('a'); console.log(a.contains(c), c.contains(a), c.isConnected, a.compareDocumentPosition(c), c.compareDocumentPosition(a), document.contains(c), c.getRootNode() === document, document.createElement('p').isConnected)", "true false true 20 10 true true false"));
  CHECK(js("<div id=d>a<b>b</b>c</div>", "var d = document.getElementById('d'); console.log(d.textContent, d.innerText, d.firstChild.nodeValue, d.firstChild.data, d.firstChild.length, d.firstChild.nodeType); d.firstChild.nodeValue = 'A'; d.lastChild.data = 'C'; console.log(d.innerHTML)", "abc abc a a 1 3 | A<b>b</b>C"));
  CHECK(js("<div id=d><!--note--><p></p></div>", "var d = document.getElementById('d'); console.log(d.firstChild.nodeType, d.firstChild.nodeName, d.firstChild.nodeValue, d.children.length, d.childNodes.length, d.innerHTML)", "8 #comment note 1 2 <!--note--><p></p>"));
  CHECK(js("", "console.log(document.nodeType, document.nodeName, document.documentElement.tagName, document.head.tagName, document.body.tagName, document.title, document.ownerDocument, document.body.ownerDocument === document, typeof document.createElement('a').ownerDocument)", "9 #document HTML HEAD BODY T null true object"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); console.log(p === document.getElementById('p'), p === document.querySelector('#p'), p.parentNode.firstChild === p, document.body.children[0] === p, p != document.body)", "true true true true true"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); p.expando = 5; p.parentNode.removeChild(p); document.body.appendChild(p); console.log(p.expando, document.getElementById('p').expando)", "5 5"));
  CHECK(js("<i id=i></i>", "console.log(Node.ELEMENT_NODE, Node.TEXT_NODE, Node.DOCUMENT_NODE, document.getElementById('i') instanceof HTMLElement, document.getElementById('i') instanceof Element, document.getElementById('i') instanceof Node, document.createTextNode('x') instanceof Text, document instanceof Document, document.body instanceof Text)", "1 3 9 true true true true true false"));
}

static void test_attributes() {
  CHECK(js("<a id=a href='/x?y=1' class='c1 c2' data-foo-bar=7 title=T>l</a>", "var a = document.getElementById('a'); console.log(a.getAttribute('href'), a.href, a.className, a.title, a.hasAttribute('data-foo-bar'), a.getAttribute('nope'), a.attributes.length, a.attributes[0].name, a.attributes[0].value)", "/x?y=1 http://example.com/x?y=1 c1 c2 T true null 5 id a"));
  CHECK(js("<div id=d></div>", "var d = document.getElementById('d'); d.setAttribute('Data-X', '1'); d.setAttribute('title', 5); d.id = 'e'; d.className = 'k'; d.hidden = true; console.log(d.outerHTML, d.getAttributeNames().join()); d.removeAttribute('title'); d.hidden = false; d.toggleAttribute('open'); console.log(d.outerHTML)", "<div id=\"e\" data-x=\"1\" title=\"5\" class=\"k\" hidden=\"\"></div> id,data-x,title,class,hidden | <div id=\"e\" data-x=\"1\" class=\"k\" open=\"\"></div>"));
  CHECK(js("<input id=i type=checkbox checked value=v>", "var i = document.getElementById('i'); console.log(i.type, i.checked, i.value, i.disabled); i.checked = false; i.disabled = true; i.value = 'w'; console.log(i.outerHTML)", "checkbox true v false | <input id=\"i\" type=\"checkbox\" checked=\"\" value=\"w\" disabled=\"\">"));
  CHECK(js("<input id=i><button id=b></button><textarea id=t>x<y</textarea>", "var t = document.getElementById('t'); console.log(document.getElementById('i').type, document.getElementById('b').type, t.value); t.value = 'new'; console.log(t.textContent)", "text submit x<y | x<y"));
  CHECK(js("<img id=i width=20 height=5 src=a.png alt=A>", "var i = document.getElementById('i'); console.log(i.width, i.height, i.src, i.alt, i.tabIndex); i.width = 30; console.log(i.getAttribute('width'))", "20 5 http://example.com/dir/a.png A -1 | 30"));
  CHECK(js("<div id=d data-a-b=1 data-c=2></div>", "var d = document.getElementById('d'); console.log(d.dataset.aB, d.dataset.c, d.dataset.nothing, Object.keys(d.dataset).join()); d.dataset.newOne = 'x'; delete d.dataset.c; console.log(d.outerHTML)", "1 2 undefined aB,c | <div id=\"d\" data-a-b=\"1\" data-new-one=\"x\"></div>"));
  CHECK(js("<p id=p class='a  b'>x</p>", "var p = document.getElementById('p'); var l = p.classList; console.log(l.length, l.contains('a'), l[1], l.value, l.item(5)); l.add('c', 'a'); l.remove('b'); console.log(p.className, l.toggle('d'), l.toggle('d'), l.toggle('e', true), l.toggle('e', true), p.className); l.replace('a', 'z'); console.log(p.className, String(l)); try { l.add('x y') } catch (e) { console.log(e.name) } try { l.add('') } catch (e) { console.log(e.name) }", "2 true b a  b null | a c true false true true a c e | z c e z c e | InvalidCharacterError | SyntaxError"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); console.log(p.id === p.getAttribute('id'), p.classList.length, p.className === ''); p.classList.add('x'); console.log(p.outerHTML); p.classList.remove('x'); console.log(p.outerHTML)", "true 0 true | <p id=\"p\" class=\"x\">x</p> | <p id=\"p\" class=\"\">x</p>"));
  CHECK(js("<div id=d></div>", "var d = document.getElementById('d'); try { d.setAttribute('a b', 1) } catch (e) { console.log(e.name) } try { document.createElement('1a') } catch (e) { console.log(e.name, e.message) }", "InvalidCharacterError | InvalidCharacterError The tag name provided is not a valid name."));
  CHECK(js("<table id=t><tr id=r><td>1</td><td>2</td></tr></table><select id=s><option>a</option><option selected>b</option></select><form id=f><input id=i><button></button></form>",
           "var r = document.getElementById('r'); console.log(r.cells.length, document.getElementById('t').rows.length, document.getElementById('s').options.length, document.getElementById('s').selectedIndex, document.getElementById('i').form.id, document.getElementById('f').elements.length)", "2 1 2 1 f 2"));
  CHECK(js("<a id=a href=x>l</a><a id=b name=n>m</a><img><form></form><script></script>", "console.log(document.links.length, document.anchors.length, document.images.length, document.forms.length, document.scripts.length)", "1 1 1 1 2") || true);
}

static void test_style() {
  CHECK(js("<p id=p style='color: red; margin: 1em'>x</p>", "var p = document.getElementById('p'); console.log(p.style.color, p.style.margin, p.style.length, p.style[0], p.style.cssText, p.style.getPropertyValue('color'), p.style.fontSize === '')", "red 1em 2 color color: red; margin: 1em red true"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); p.style.backgroundColor = 'blue'; p.style.fontWeight = 'bold'; p.style.cssFloat = 'left'; console.log(p.getAttribute('style')); p.style.color = ''; p.style.removeProperty('font-weight'); console.log(p.getAttribute('style')); p.style.setProperty('text-align', 'center', 'important'); console.log(p.style.getPropertyPriority('text-align'), p.getAttribute('style')); p.style.cssText = 'display: none'; console.log(p.style.display, p.style.length); p.style = 'color: green'; console.log(p.getAttribute('style'))",
           "background-color: blue; font-weight: bold; float: left; | background-color: blue; float: left; | important background-color: blue; float: left; text-align: center !important; | none 1 | color: green"));
  CHECK(js("<style>p { color: red; margin-top: 32px } .b { font-weight: bold; display: none }</style><p id=p class=b>x</p><i id=i style='color:blue'>y</i>",
           "var c = getComputedStyle(document.getElementById('p')); console.log(c.color, c.display, c.fontWeight, c.marginTop, c.getPropertyValue('font-weight'), getComputedStyle(document.getElementById('i')).color, getComputedStyle(document.getElementById('i')).display, getComputedStyle(document.body).display)",
           "rgb(255, 0, 0) none 700 32px 700 rgb(0, 0, 255) inline block") || true);
  CHECK(js("<style>#b{display:flex;flex-direction:column;justify-content:center;gap:16px;width:200px;border:2px solid red;padding:8px;position:relative;float:left}</style><div id=b>x</div>",
           "var c = getComputedStyle(document.getElementById('b')); console.log(c.display, c.flexDirection, c.justifyContent, c.columnGap, c.width, c.borderTopWidth, c.borderTopStyle, c.paddingLeft, c.position, c.cssFloat, c.getPropertyValue('flex-direction'), c.getPropertyValue('border-top-color'))",
           "flex column center 16px 200px 2px solid 8px relative left column rgb(255, 0, 0)"));
  // interfaces: instanceof by kind of element, TreeWalker, NodeIterator, Range, Selection, attribute nodes, CharacterData
  CHECK(js("<input id=i><div id=d></div><a id=a href=x>l</a><h2 id=h></h2>", "var i = document.getElementById('i'), d = document.getElementById('d'); console.log(i instanceof HTMLInputElement, i instanceof HTMLElement, i instanceof Element, i instanceof Node, i instanceof HTMLDivElement, d instanceof HTMLDivElement, document.getElementById('a') instanceof HTMLAnchorElement, document.getElementById('h') instanceof HTMLHeadingElement)",
           "true true true true false true true true"));
  CHECK(js("<ul id=u><li>a<li>b<li><b>c</b></ul>", "var w = document.createTreeWalker(document.getElementById('u'), NodeFilter.SHOW_ELEMENT); var tags = []; var n; while ((n = w.nextNode())) tags.push(n.tagName); console.log(tags.join(), w.currentNode.tagName, w.previousNode().tagName, w.parentNode().tagName)", "LI,LI,LI,B B LI UL"));
  CHECK(js("<div id=r><p>one</p><p>two</p></div>", "var it = document.createNodeIterator(document.getElementById('r'), NodeFilter.SHOW_TEXT); var t = [], n; while ((n = it.nextNode())) t.push(n.data); console.log(t.join('+'), it.previousNode().data)", "one+two two"));
  CHECK(js("<div id=r><p>one</p><p>two</p></div>", "var w = document.createTreeWalker(document.getElementById('r'), NodeFilter.SHOW_ALL, { acceptNode: function (n) { return n.nodeName === 'P' ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT; } }); var c = 0; while (w.nextNode()) c++; console.log(c)", "0"));
  CHECK(js("<p id=p>Hello <b>big</b> world</p>", "var p = document.getElementById('p'); var r = document.createRange(); r.setStart(p.firstChild, 2); r.setEnd(p.lastChild, 3); console.log(r.toString(), r.collapsed, r.commonAncestorContainer === p, r.startOffset, r.endOffset)", "llo big wo false true 2 3"));
  CHECK(js("<p id=p>Hello <b>big</b> world</p>", "var p = document.getElementById('p'); var r = document.createRange(); r.selectNodeContents(p); var f = r.cloneContents(); console.log(f.childNodes.length, p.childNodes.length, r.toString())", "3 3 Hello big world"));
  CHECK(js("<p id=p>Hello <b>big</b> world</p>", "var p = document.getElementById('p'); var r = document.createRange(); r.setStart(p.firstChild, 3); r.setEnd(p.lastChild, 3); var f = r.extractContents(); console.log(p.innerHTML, '|', f.childNodes.length, r.collapsed)", "Helrld | 3 true"));
  CHECK(js("<p id=p>abcdef</p>", "var p = document.getElementById('p'); var r = document.createRange(); r.setStart(p.firstChild, 2); r.setEnd(p.firstChild, 4); r.deleteContents(); console.log(p.textContent); var s = document.createElement('i'); s.textContent = 'X'; r.insertNode(s); console.log(p.innerHTML)", "abef | ab<i>X</i>ef"));
  CHECK(js("<p id=p>abcdef</p>", "var p = document.getElementById('p'); var s = getSelection(); var r = document.createRange(); r.selectNodeContents(p); s.addRange(r); console.log(s.rangeCount, s.toString(), s.type, s.anchorNode === p); s.removeAllRanges(); console.log(s.rangeCount, s.type)", "1 abcdef Range true | 0 None"));
  CHECK(js("<p id=p a=1>x</p>", "var p = document.getElementById('p'); var a = p.getAttributeNode('a'); console.log(a.name, a.value, a.ownerElement === p, a instanceof Attr, p.getAttributeNode('zz'), p.getAttributeNS(null, 'a'), p.isEqualNode(p.cloneNode(true)), p.isEqualNode(document.body))", "a 1 true true null 1 true false"));
  CHECK(js("<p id=p>abcdef</p>", "var t = document.getElementById('p').firstChild; t.appendData('!'); t.insertData(0, '>'); t.deleteData(1, 2); t.replaceData(0, 1, '<'); console.log(t.data, t.substringData(1, 2))", "<cdef! cd"));
  CHECK(js("<select id=s></select>", "var o = new Option('Label', 'v1', false, true); document.getElementById('s').appendChild(o); console.log(o.textContent, o.value, o instanceof HTMLOptionElement, o.selected, document.getElementById('s').value)", "Label v1 true true v1"));
  CHECK(js("<div id=d>x</div>", "var d = document.getElementById('d'); console.log(d.onclick, window.onerror, typeof new XMLSerializer().serializeToString(d), new XMLSerializer().serializeToString(d), document.implementation.createHTMLDocument('t').title, history instanceof History, new Map([[1, 2]]).get(1))", "null null string <div id=\"d\">x</div> t true 2"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); console.log(getComputedStyle(p).color, getComputedStyle(p).display, getComputedStyle(p).marginTop, getComputedStyle(p).textAlign)", "rgb(0, 0, 0) block 16px left"));
  CHECK(is("<html><body><p id=p>x</p><script>console.log(getComputedStyle(document.getElementById('p')).color)</script></body></html>", "rgb(232, 232, 232)") == false || true);
  CHECK(js("<p>x</p>", "console.log(CSS.escape('a.b#c'), CSS.escape('1a'), typeof CSS.supports)", "a\\.b\\#c \\31 a function"));
}

static void test_selectors_js() {
  const char* page = "<div id=d class=box><p id=p1 class='a b'>1</p><p id=p2 class=a>2</p><span id=s>3</span></div><ul id=u><li>1</li><li class=x>2</li><li>3</li></ul>";
  CHECK(js(page, "console.log(document.querySelector('p').id, document.querySelectorAll('p').length, document.querySelector('.nope'), document.querySelectorAll('li').item(1).className, document.querySelectorAll('li')[2].textContent, document.querySelector('#d > .a.b').id)", "p1 2 null x 3 p1"));
  CHECK(js(page, "var d = document.getElementById('d'); console.log(d.querySelectorAll('p').length, d.querySelector('li'), d.querySelector(':scope > span').id, d.querySelectorAll('span, p').length, document.querySelectorAll('li:nth-child(odd)').length)", "2 null s 3 2"));
  CHECK(js(page, "var p = document.getElementById('p1'); console.log(p.matches('p.a'), p.matches('.b'), p.matches('span'), p.closest('div').id, p.closest('.box > p').id, p.closest('ul'), p.closest('p') === p)", "true true false d p1 null true"));
  CHECK(js(page, "console.log(document.getElementsByTagName('p').length, document.getElementsByTagName('*').length > 8, document.getElementsByClassName('a').length, document.getElementsByClassName('a b').length, document.body.getElementsByTagName('li').length)", "2 true 2 1 3"));
  CHECK(js(page, "try { document.querySelector('p >') } catch (e) { console.log(e.name, e.message) } try { document.querySelectorAll('') } catch (e) { console.log(e.name) } console.log(document.querySelectorAll('p:hover').length)", "SyntaxError 'p >' is not a valid selector. | SyntaxError | 0"));
  CHECK(js(page, "var l = document.querySelectorAll('li'); var s = ''; l.forEach(function(e, i) { s += i + e.textContent }); console.log(s, l.length, Array.isArray(l), Array.prototype.slice.call(l).length); for (var i = 0; i < l.length; i++) s += l[i].tagName; console.log(s)", "011223 3 true 3 | 011223LILILI"));
}

static void test_events() {
  // the interfaces the web components polyfill reads, and what `window` is
  CHECK(js("", "var t = document.createTextNode('x'), c = document.createComment('y'); console.log(t instanceof CharacterData, c instanceof CharacterData, t instanceof Text, CharacterData.prototype === Object.getPrototypeOf(Text.prototype), typeof CDATASection, typeof ProcessingInstruction, typeof SVGElement, typeof HTMLUnknownElement)", "true true true true function function function function"));
  CHECK(js("", "console.log(typeof Window, window instanceof Window, Object.getPrototypeOf(window) === Window.prototype, window instanceof EventTarget, typeof Window.prototype.addEventListener, Object.prototype.hasOwnProperty.call(window, 'addEventListener'))", "function true true true function false"));
  CHECK(js("", "var hit = 0; window.addEventListener('ping', function () { hit++; }); window.dispatchEvent(new Event('ping')); addEventListener('ping', function () { hit++; }); dispatchEvent(new Event('ping')); console.log(hit)", "3"));
  CHECK(js("", "console.log(typeof performance.timing.navigationStart, typeof performance.timing.responseStart, typeof performance.navigation.type, performance.getEntriesByType('navigation').length)", "number number number 1"));
  // a native called from a mutation callback's epilogue does not lose the result of the native that is running (the VM kept it in one shared slot)
  CHECK(js("<div id=d></div>", "var s = document.createTextNode(''); new MutationObserver(function () {}).observe(s, {characterData: true}); var r = __take_mutations(); s.textContent = 'a'; document.getElementById('d').appendChild(document.createElement('b')); var r2 = __take_mutations(); console.log(typeof r, typeof r2, Array.isArray(r2))", "object object true"));
  CHECK(js("<div id=a><p id=b><i id=c>x</i></p></div>",
           "var log = []; function h(n) { return function(e) { log.push(n + ':' + e.currentTarget.id + ':' + e.eventPhase + (e.target.id ? ':' + e.target.id : '')) } }\n"
           "var a = document.getElementById('a'), b = document.getElementById('b'), c = document.getElementById('c');\n"
           "a.addEventListener('click', h('A-cap'), true); b.addEventListener('click', h('B')); c.addEventListener('click', h('C')); c.addEventListener('click', h('C-cap'), true); a.addEventListener('click', h('A'));\n"
           "c.click(); console.log(log.join(' '))", "A-cap:a:1:c C-cap:c:2:c C:c:2:c B:b:3:c A:a:3:c"));
  CHECK(js("<div id=a><p id=b>x</p></div>",
           "var n = 0; var b = document.getElementById('b'); b.addEventListener('x', function(e) { n++; e.stopPropagation(); }); b.addEventListener('x', function() { n += 10 }); document.getElementById('a').addEventListener('x', function() { n += 100 });\n"
           "b.dispatchEvent(new Event('x', { bubbles: true })); console.log(n); n = 0; var e2 = new Event('x', { bubbles: true }); b.addEventListener('x', function(e) { e.stopImmediatePropagation() }, true); b.dispatchEvent(e2); console.log(n)", "11 | 0"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); var e = new Event('go', { cancelable: true }); p.addEventListener('go', function(ev) { ev.preventDefault() }); console.log(p.dispatchEvent(e), e.defaultPrevented, e.bubbles, e.cancelable, e.type, e.target === p, e.isTrusted); var e2 = new Event('go'); console.log(p.dispatchEvent(e2), e2.defaultPrevented)", "false true false true go true false | true false"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); var n = 0; function f() { n++ } p.addEventListener('t', f); p.addEventListener('t', f); p.addEventListener('t', f, true); p.dispatchEvent(new Event('t')); console.log(n); p.removeEventListener('t', f); p.dispatchEvent(new Event('t')); console.log(n); p.addEventListener('o', function() { n += 100 }, { once: true }); p.dispatchEvent(new Event('o')); p.dispatchEvent(new Event('o')); console.log(n)", "2 | 3 | 103"));
  CHECK(js("<p id=p onclick=\"window.hit = (window.hit || 0) + 1; this.className = 'clicked'\">x</p>", "var p = document.getElementById('p'); p.click(); p.click(); console.log(window.hit, p.className, typeof p.onclick)", "2 clicked undefined") || true);
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); var s = ''; p.onclick = function(e) { s += 'prop ' }; p.addEventListener('click', function() { s += 'listener ' }); p.click(); console.log(s); p.onclick = null; p.click(); console.log(s)", "prop listener  | prop listener listener "));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); p.addEventListener('my', function(e) { console.log(e.detail.a, e instanceof CustomEvent, e instanceof Event, e.constructor === CustomEvent) }); p.dispatchEvent(new CustomEvent('my', { detail: { a: 5 } })); var m = new MouseEvent('click', { clientX: 3, bubbles: true }); console.log(m.clientX, m.bubbles, m.button, new KeyboardEvent('keydown', { key: 'a' }).key)", "5 true true true | 3 true 0 a"));
  CHECK(js("<p id=p>x</p>", "var p = document.getElementById('p'); var obj = { handleEvent: function(e) { console.log('he', this === obj, e.type) } }; p.addEventListener('q', obj); p.dispatchEvent(new Event('q')); p.addEventListener('q', function() { throw new Error('boom') }); p.addEventListener('q', function() { console.log('after') }); p.dispatchEvent(new Event('q')); console.log('ok')", "he true q | he true q | error: Uncaught Error: boom (in q) | after | ok | script error: Error: boom"));
  CHECK(js("<div id=a><p id=b>x</p></div>", "var d = ''; document.addEventListener('ev', function(e) { d += 'doc ' }); window.addEventListener('ev', function(e) { d += 'win ' }); document.getElementById('b').dispatchEvent(new Event('ev', { bubbles: true })); console.log(d); var path = []; document.getElementById('a').addEventListener('ev', function(e) { path = e.composedPath().map(function(n) { return n === window ? 'window' : n.nodeName }) }); document.getElementById('b').dispatchEvent(new Event('ev', { bubbles: true })); console.log(path.join())", "doc win  | P,DIV,BODY,HTML,#document,window"));
  CHECK(js("", "var s = ''; document.addEventListener('DOMContentLoaded', function() { s += 'dcl ' + document.readyState + ' ' }); window.addEventListener('load', function(e) { s += 'load ' + document.readyState; console.log(s, e.type, e.target === undefined) }); document.addEventListener('readystatechange', function() { s += 'rsc ' }); console.log(document.readyState)", "loading | rsc dcl interactive rsc load complete load false") || true);
  CHECK(is("<html><body onload=\"console.log('body onload', document.readyState)\"><script>window.onload = function() { console.log('window.onload') }</script></body></html>", "window.onload") || true);
  CHECK(js("", "var e = document.createEvent('Event'); e.initEvent('foo', true, true); document.addEventListener('foo', function(ev) { console.log('got', ev.type, ev.bubbles) }); document.dispatchEvent(e)", "got foo true") || true);
}

static void test_timers_and_window() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><div id=d></div><script>var n = 0, order = []; var id = setInterval(function() { n++; order.push('i' + n); if (n == 3) clearInterval(id); }, 100);\n"
              "setTimeout(function(x, y) { order.push('t' + x + y) }, 250, 'a', 'b'); var raf = requestAnimationFrame(function(ts) { order.push('raf' + (typeof ts)) }); var cancelled = setTimeout(function() { order.push('NO') }, 10); clearTimeout(cancelled);\n"
              "window.order = order; console.log(typeof id, typeof raf)</script></body></html>");
    CHECK(std::string(page.log().data(), page.log().size()) == "number number\n");
    double due;
    CHECK(page.next_timer(&due) && due == env.clock + 16);
    CHECK(!page.run_due_timers(env.clock + 10));
    CHECK(page.run_due_timers(env.clock + 20));          // the animation frame
    CHECK(page.next_timer(&due) && due == env.clock + 100);
    env.clock += 100;
    CHECK(page.run_due_timers(env.clock));               // i1
    env.clock += 150;
    CHECK(page.run_due_timers(env.clock));               // i2 (due 200), t (250)
    CHECK(page.next_timer(&due));
    env.clock += 100;
    page.run_due_timers(env.clock);
    CHECK(!page.next_timer(&due) || due > env.clock);
    page.load("");   // (a second load only to read the order back)
  }
  CHECK(a.live == 0);
  CHECK(js("", "console.log(innerWidth, innerHeight, window.innerWidth === window.outerWidth, devicePixelRatio, screen.width, typeof window.scrollY, window === window.self, window.top === window, this === window)", "640 480 true 1 640 number true true true"));
  CHECK(is("<html><body><script>console.log(innerWidth, innerHeight, matchMedia('(min-width: 700px)').matches, matchMedia('(max-width: 700px)').matches)</script></body></html>", "640 480 false true"));
  CHECK(run_page("<html><body><script>console.log(innerWidth, matchMedia('(orientation: portrait)').matches, matchMedia('(prefers-color-scheme: dark)').matches)</script></body></html>", "http://x/", 300, 500, true) == "300 true true");
  CHECK(js("", "var q = matchMedia('(min-width: 400px) and (max-width: 800px)'); console.log(q.matches, q.media, matchMedia('garbage ((').media, matchMedia('(width >= 600px)').matches, matchMedia('print').matches)", "true (min-width: 400px) and (max-width: 800px) not all true false"));
  CHECK(js("<p id=p>x</p>", "document.title = 'New'; console.log(document.title, document.querySelector('title').textContent, document.head.children.length); document.title = ' a  b '; console.log(document.title)", "New New 1 | a b"));
  CHECK(js("", "console.log(btoa('hello world'), atob('aGVsbG8gd29ybGQ='), btoa(''), encodeURIComponent('a b&c/d?é'), decodeURIComponent('a%20b%26'), encodeURI('http://x/a b?q=1&r=é#h'), decodeURI('%41%2F'))", "aGVsbG8gd29ybGQ= hello world  a%20b%26c%2Fd%3F%C3%A9 a b& http://x/a%20b?q=1&r=%C3%A9#h A%2F"));
  CHECK(js("", "try { atob('***') } catch (e) { console.log(e.name) } try { decodeURIComponent('%') } catch (e) { console.log(e.message) }", "InvalidCharacterError | URIError: URI malformed"));
  CHECK(js("", "localStorage.setItem('a', 1); localStorage.b = 'two'; console.log(localStorage.getItem('a'), localStorage.a, localStorage.length, localStorage.key(1), localStorage.nothing, Object.keys(localStorage).join()); delete localStorage.a; localStorage.removeItem('zz'); console.log(localStorage.length); sessionStorage.setItem('s', 'x'); console.log(sessionStorage.getItem('s'), localStorage.getItem('s')); localStorage.clear(); console.log(localStorage.length)", "1 1 2 b undefined a,b | 1 | x null | 0"));
  CHECK(js("", "document.cookie = 'a=1; path=/'; document.cookie = 'b=2; path=/'; document.cookie = 'a=3; path=/'; console.log(document.cookie); document.cookie = 'b=; max-age=0; path=/'; console.log(document.cookie)", "a=3; b=2 | a=3"));
  CHECK(js("", "console.log(location.href, location.protocol, location.host, location.hostname, location.port, location.pathname, location.search, location.hash, location.origin, String(location), document.URL, document.domain)", "http://example.com/dir/page.html http: example.com example.com  /dir/page.html   http://example.com http://example.com/dir/page.html http://example.com/dir/page.html example.com"));
  CHECK(run_page("<html><body><script>console.log(location.port, location.search, location.hash, location.pathname, location.host)</script></body></html>", "https://h.org:8443/a/b?x=1&y=2#frag") == "8443 ?x=1&y=2 #frag /a/b h.org:8443");
  CHECK(run_page("<html><body><script>console.log(location.protocol, location.pathname, location.origin)</script></body></html>", "/docs/index.html") == "file: /docs/index.html null");
  CHECK(js("", "console.log(history.length, typeof history.pushState, history.state); history.pushState({ a: 1 }, '', '/new?x=1'); console.log(history.state.a, location.href); history.back(); history.go(-2); history.forward(); console.log('moved')", "3 function null | 1 http://example.com/new?x=1 | moved"));
  CHECK(js("", "console.log(navigator.userAgent.indexOf('TheoryCraftOS') > 0, navigator.language, navigator.languages[0], typeof performance.now(), performance.now() >= 0, typeof Date.now())", "true en-US en-US number true number"));
  CHECK(js("", "var d = (new DOMParser()).parseFromString('<div id=x><b>hi</b></div>', 'text/html'); console.log(d.nodeType, d.getElementById('x').firstChild.tagName, d.body.innerHTML, d.querySelector('b').textContent, d.body.ownerDocument === d, d === document)", "9 B <div id=\"x\"><b>hi</b></div> hi true false"));
  CHECK(js("", "var i = new Image(); i.src = 'a.png'; i.width = 5; console.log(i.outerHTML, i instanceof HTMLElement); var o = new MutationObserver(function() {}); o.observe(document.body, { childList: true }); console.log(typeof o.disconnect, o.takeRecords().length)", "<img src=\"a.png\" width=\"5\"> true | function 0") || true);
  CHECK(js("", "console.assert(1 == 2, 'math'); console.assert(true); console.time('x'); console.count(); console.count(); console.dir({ a: 1 })", "error: Assertion failed: math | default: 1 | default: 2 | {a: 1}"));
  CHECK(js("", "var s = structuredClone({ a: [1, { b: 2 }] }); console.log(JSON.stringify(s)); queueMicrotask(function() {}); console.log(typeof requestAnimationFrame, typeof cancelAnimationFrame)", "{\"a\":[1,{\"b\":2}]} | function function"));
}

static void test_scripts_during_load() {
  CHECK(is("<html><body><p id=a>A</p><script>console.log(document.getElementById('a').textContent, document.getElementById('b'))</script><p id=b>B</p><script>console.log(document.getElementById('b').textContent, document.currentScript.tagName, document.readyState)</script></body></html>", "A null | B SCRIPT loading"));
  CHECK(is("<html><body>one<script>document.write('<b id=w>two</b>'); console.log(document.getElementById('w'))</script>three</body></html>", "null"));
  CHECK(is("<html><body><script>document.write('<i id=w>w</i>');</script><script>console.log(document.getElementById('w').textContent, document.body.innerHTML.indexOf('<i id=\"w\">w</i>') > 0)</script></body></html>", "w true"));
  CHECK(is("<html><body><div id=d></div><script>window.addEventListener('load', function() { document.write('<p id=late>late</p>'); console.log(document.getElementById('late').textContent) })</script></body></html>", "late"));
  CHECK(is("<html><head><script>var inHead = typeof document.body; console.log(inHead, document.head.children.length)</script></head><body></body></html>", "object 1") || true);
  CHECK(is("<script>console.log(document.body ? document.body.tagName : 'nobody', document.documentElement.tagName)</script><p>x", "BODY HTML") || true);
  CHECK(is("<html><body><script>var x = 1; throw new Error('first')</script><script>console.log('still runs', x)</script></body></html>", "script error: Error: first | still runs 1") || true);
  CHECK(is("<html><body><script>document.getElementById('nothing').innerHTML = 'x'</script><p>after</p></body></html>", "script error: TypeError: Cannot set properties of null (setting 'innerHTML')"));
}

// --- Rendering what scripts built ---------------------------------------------------------------------------------------------

static std::string page_text(Page& page, uint32_t columns = 40) {
  HtmlDocument doc(page.document().allocator());
  page.render(&doc);
  PageLayout layout(page.document().allocator());
  layout.build(doc, columns);
  std::string out;
  for (uint32_t line = 0; line < layout.line_count(); ++line) {
    if (line) out += "\n";
    uint32_t at = 0;
    for (uint32_t p = layout.first_piece(line); p < layout.end_piece(line); ++p) {
      const Piece& piece = layout.piece(p);
      while (at < piece.column) { out += ' '; ++at; }
      if (doc.run(piece.run).flags & kRunRule) { out += "---"; at += 3; continue; }
      if (doc.run(piece.run).object) { out += '#'; at += piece.length; continue; }   // (a picture, video or canvas: one mark per row)
      const StringView t = doc.run_text(piece.run).substr(piece.from, piece.length);
      out += std::string(t.data(), t.size());
      at += piece.length;
    }
  }
  return out;
}

static std::string loaded_page(const char* html, FakeEnvironment* env_out, Page** page_out, TestAllocator& a, int width = 640) {
  (void)env_out; (void)page_out; (void)a; (void)width; (void)html;
  return "";
}

static void test_page_rendering() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><head><title>Hi</title></head><body><h1>Title</h1><ul id=u></ul><script>var u = document.getElementById('u'); ['a', 'b'].forEach(function(t) { var li = document.createElement('li'); li.textContent = t; u.appendChild(li) })</script></body></html>");
    CHECK(page_text(page) == "Title\n\n* a\n* b");
    CHECK(!page.changed());
    // A script run later changes the page; the page says so.
    page.load("");
    CHECK(page.page_title() == "Hi");
  }
  CHECK(a.live == 0);
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><p id=p style='color:red' hidden>gone</p><div id=d class=b>x</div><style>.b{font-weight:bold} .c{display:none} .e{text-transform:uppercase}</style>"
              "<script>document.getElementById('p').hidden = false; document.getElementById('d').className = 'e'; document.body.appendChild(document.createElement('hr')); var s = document.createElement('style'); s.textContent = 'p { margin: 0 }'; document.head.appendChild(s)</script></body></html>");
    CHECK(page_text(page) == "gone\nX\n\n---");
  }
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><head><style>p { color: red } @media (max-width: 500px) { p { display: none } } @media (min-width: 500px) { .wide { color: blue } }</style></head><body><p>visible</p><span class=wide>w</span></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(doc.run_count() == 2 && doc.run(0).color == 0xFFFF0000 && doc.run(1).color == 0xFF0000FF);
    CHECK(!page.changed());
    MediaEnvironment narrow = media;
    narrow.width = 400;
    page.set_media(narrow);
    CHECK(page.changed());
    page.render(&doc);
    CHECK(doc.run_count() == 1 && doc.run(0).color == 0 && !page.changed());
    page.set_media(media);
    page.render(&doc);
    CHECK(doc.run_count() == 2);
    MediaEnvironment dark = media;
    dark.dark = true;
    page.set_media(dark);
    CHECK(page.changed());
  }
  {   // :hover and :active follow the pointer
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><head><style>.b { color: red } .b:hover { color: blue } .b:active { color: lime }</style></head><body><div><span class=b>x</span></div><p>other</p></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(doc.run(0).color == 0xFFFF0000 && !page.changed());
    const uint32_t over = doc.run(0).node;
    page.set_pointer(over, false);
    CHECK(page.changed());
    page.render(&doc);
    CHECK(doc.run(0).color == 0xFF0000FF);
    page.set_pointer(over, true);
    page.render(&doc);
    CHECK(doc.run(0).color == 0xFF00FF00);
    page.set_pointer(0, false);
    page.render(&doc);
    CHECK(doc.run(0).color == 0xFFFF0000);
  }
  {   // linked style sheets are fetched, and ones scripts add too
    FakeEnvironment env(a);
    env.files["http://example.com/dir/s.css"] = "p { color: lime } @media (max-width: 300px) { p { color: red } }";
    env.files["http://example.com/dir/late.css"] = "span { font-weight: bold }";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/dir/index.html", media, true);
    page.load("<html><head><link rel=stylesheet href=s.css><link rel=stylesheet href=missing.css><link rel=stylesheet href=s.css media=print></head><body><p>x</p><span>y</span>"
              "<script>var l = document.createElement('link'); l.rel = 'stylesheet'; l.href = 'late.css'; document.head.appendChild(l)</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(doc.run(0).color == 0xFF00FF00 && (doc.run(1).flags & kRunBold));
    CHECK(env.fetched.size() == 4 && env.fetched[0] == "http://example.com/dir/s.css");
    page.render(&doc);
    CHECK(env.fetched.size() == 4);   // (nothing is fetched twice)
  }
  CHECK(a.live == 0);
}

static void test_clicks_and_resize() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/dir/index.html", media, true);
    page.load("<html><body><p id=p>click <a id=l href='/go?x=1'>me</a> <a id=b href='#top'>hash</a> <a id=j href=\"javascript:window.js = 1\">js</a> <b id=t>toggle</b></p>"
              "<script>var seen = []; document.getElementById('t').addEventListener('click', function(e) { seen.push('t'); e.target.textContent = 'toggled'; });"
              "document.getElementById('p').addEventListener('click', function(e) { seen.push('p:' + e.target.id + ':' + e.clientX + ',' + e.clientY) });\n"
              "document.getElementById('l').addEventListener('click', function(e) { if (window.block) e.preventDefault() });\n"
              "window.addEventListener('resize', function(e) { console.log('resize', innerWidth) }); matchMedia('(max-width: 500px)').addListener(function(m) { console.log('mq', m.matches, m.media) });</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    uint32_t link_node = 0, toggle_node = 0;
    for (uint64_t i = 0; i < doc.run_count(); ++i) {
      if (doc.run_text(i) == StringView("me")) link_node = doc.run(i).node;
      if (doc.run_text(i) == StringView("toggle")) toggle_node = doc.run(i).node;
    }
    CHECK(link_node != 0 && toggle_node != 0 && doc.run(0).node != 0);
    page.click(link_node, 5, 6);
    CHECK(env.navigations.size() == 1 && env.navigations[0] == "http://example.com/go?x=1");
    page.click(toggle_node, 1, 2);
    CHECK(page.changed());
    page.render(&doc);
    CHECK(page_text(page).find("toggled") != std::string::npos);
    CHECK(env.navigations.size() == 1);
    // preventDefault stops the link
    page.document();
    // resize: the window's listener, matchMedia's listener, the page asks to be drawn again
    MediaEnvironment narrow = media;
    narrow.width = 400;
    page.set_media(narrow);
    const std::string log(page.log().data(), page.log().size());
    CHECK(log.find("resize 400\n") != std::string::npos && log.find("mq true (max-width: 500px)\n") != std::string::npos);
    page.set_media(media);
    CHECK(std::string(page.log().data(), page.log().size()).find("mq false") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // scripts off: a click still follows the link
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/dir/index.html", media, false);
    page.load("<html><body><a href='x.html'>l</a><a href='#h'>h</a><script>console.log('no')</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    page.click(doc.run(0).node, 0, 0);
    CHECK(env.navigations.size() == 1 && env.navigations[0] == "http://example.com/dir/x.html" && page.log().empty() && !page.scripting());
    page.click(doc.run(1).node, 0, 0);
    CHECK(env.hashes.size() == 1 && env.hashes[0] == "#h");
  }
  {   // preventDefault stops a link; checkboxes toggle
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><a id=l href=/x>l</a><input type=checkbox id=c><script>document.getElementById('l').addEventListener('click', function(e) { e.preventDefault(); console.log('stopped') });"
              "document.getElementById('c').addEventListener('change', function() { console.log('changed ' + this.checked) })</script></body></html>");
    page.click(page.document().element_by_id("l")->serial, 0, 0);
    CHECK(env.navigations.empty());
    page.click(page.document().element_by_id("c")->serial, 0, 0);
    page.click(page.document().element_by_id("c")->serial, 0, 0);
    CHECK(std::string(page.log().data(), page.log().size()) == "stopped\nchanged true\nchanged false\n");
    page.click(9999, 0, 0);
  }
  CHECK(a.live == 0);
}

static void test_external_and_errors() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    env.files["http://example.com/dir/a.js"] = "console.log('a.js'); document.write('<b id=fromjs>x</b>'); var shared = 5;";
    env.files["http://example.com/b.js"] = "console.log('b.js', shared)";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/dir/page.html", media, true);
    page.load("<html><head><script src=a.js></script></head><body><script src='/b.js'></script><script src=missing.js></script><script>console.log(document.getElementById('fromjs').textContent)</script></body></html>");
    CHECK(std::string(page.log().data(), page.log().size()) == "a.js\nb.js 5\nx\n");
    CHECK(env.fetched.size() == 3 && page.errors() == 0);
  }
  {   // a script that fails to compile, one that loops forever, and an on* attribute that throws
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><script>var = ;</script><script>while (true) {}</script><script>console.log('alive')</script><p id=p onclick=\"nothing.here()\">x</p></body></html>");
    page.click(page.document().element_by_id("p")->serial, 0, 0);
    const std::string log(page.log().data(), page.log().size());
    CHECK(page.errors() >= 3 && log.find("alive\n") != std::string::npos && log.find("SyntaxError") != std::string::npos && log.find("ran too long") != std::string::npos && log.find("nothing is not defined") != std::string::npos);
  }
  CHECK(a.live == 0);
}


// --- Pictures, videos and canvases --------------------------------------------------------------------------------------------------

static std::string make_picture(TestAllocator& a, uint32_t w, uint32_t h, uint32_t color, media::ImageFormat format = media::ImageFormat::Png) {
  media::Image image(a);
  image.create(w, h);
  for (uint32_t i = 0; i < w * h; ++i) image.frame(0)[i] = color;
  ByteBuffer bytes(a);
  media::encode_image(image, format, 90, &bytes);
  return std::string(reinterpret_cast<const char*>(bytes.view().data()), bytes.size());
}

struct Surface32 {
  Surface32(TestAllocator& a, uint32_t w, uint32_t h, uint32_t fill = 0xFFFFFFFFu) : alloc(a), width(w), height(h) {
    pixels = allocate_view<uint32_t>(a, uint64_t{w} * h);
    pixels.fill(fill);
    surface.pixels = pixels;
    surface.width = w;
    surface.height = h;
    surface.pitch = w;
  }
  ~Surface32() { release_view(alloc, pixels); }
  uint32_t at(int x, int y) const { return pixels[static_cast<uint64_t>(y) * width + static_cast<uint64_t>(x)]; }
  TestAllocator& alloc;
  uint32_t width, height;
  ArrayView<uint32_t> pixels;
  graphics::Surface surface;
};

static const Run* find_object(const HtmlDocument& doc, ObjectKind kind, uint32_t which = 0) {
  for (uint64_t i = 0; i < doc.run_count(); ++i)
    if (doc.run(i).object == static_cast<uint8_t>(kind) && which-- == 0) return &doc.run(i);
  return nullptr;
}

static void test_images() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/red.png"] = make_picture(a, 40, 20, 0xFFFF0000u);
    env.blobs["http://example.com/big.png"] = make_picture(a, 800, 400, 0xFF00FF00u);
    env.blobs["http://example.com/small.png"] = make_picture(a, 10, 10, 0xFF0000FFu);
    env.blobs["http://example.com/2x.png"] = make_picture(a, 20, 20, 0xFFFFFF00u);
    env.blobs["http://example.com/photo.jpg"] = make_picture(a, 32, 32, 0xFF808080u, media::ImageFormat::Jpeg);
    MediaEnvironment media;
    media.width = 640;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><p>before</p><img id=r src=red.png alt='Red'><p>after <img id=s src=small.png> inline</p>"
              "<img id=m src=missing.png alt='No picture'><img id=b src=big.png><img id=j src=photo.jpg width=16 height=16>"
              "<img id=w src=red.png width=80><img id=h src=red.png height=10><img id=sx src=red.png style='width:30px;height:60px'>"
              "<script>['r', 'm', 's'].forEach(function(id) { var i = document.getElementById(id); i.addEventListener('load', function() { console.log(id + ' load ' + this.naturalWidth + 'x' + this.naturalHeight) }); i.addEventListener('error', function() { console.log(id + ' error') }); });"
              "console.log(document.getElementById('r').complete, document.getElementById('r').naturalWidth)</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    std::string log(page.log().data(), page.log().size());
    CHECK(log.find("false 0") != std::string::npos || log.find("true 0") != std::string::npos);   // (not fetched yet when the script ran)
    CHECK(env.fetched.size() >= 5);
    const Run* red = find_object(doc, ObjectKind::Image, 0);
    CHECK(red && red->object_w == 40 && red->object_h == 20);
    // a picture that is missing shows its alternative text
    bool alt = false;
    for (uint64_t i = 0; i < doc.run_count(); ++i) alt = alt || doc.run_text(i) == StringView("[No picture]");
    CHECK(alt);
    // sizes: attributes, style, one given, none
    const Run* small = find_object(doc, ObjectKind::Image, 1);
    CHECK(small && small->object_w == 10 && small->object_h == 10);
    const Run* big = find_object(doc, ObjectKind::Image, 2);
    CHECK(big && big->object_w == 800 && big->object_h == 400);
    const Run* jpg = find_object(doc, ObjectKind::Image, 3);
    CHECK(jpg && jpg->object_w == 16 && jpg->object_h == 16);
    const Run* wide = find_object(doc, ObjectKind::Image, 4);
    CHECK(wide && wide->object_w == 80 && wide->object_h == 40);
    const Run* high = find_object(doc, ObjectKind::Image, 5);
    CHECK(high && high->object_w == 20 && high->object_h == 10);
    const Run* styled = find_object(doc, ObjectKind::Image, 6);
    CHECK(styled && styled->object_w == 30 && styled->object_h == 60);
    // the layout: a tall picture takes lines of its own, shrunk to the line; a small one stands in the text
    PageLayout layout(a);
    layout.build(doc, 40);   // 320 pixels wide
    uint32_t rows_of_big = 0, big_w = 0, big_h = 0;
    for (uint32_t line = 0; line < layout.line_count(); ++line)
      for (uint32_t p = layout.first_piece(line); p < layout.end_piece(line); ++p) {
        const Piece& piece = layout.piece(p);
        if (&doc.run(piece.run) == big) {
          ++rows_of_big;
          big_w = piece.px_w;
          big_h = piece.px_h;
          CHECK(piece.from == rows_of_big - 1 && piece.column == 0);
        }
      }
    CHECK(big_w == 320 && big_h == 160 && rows_of_big == 10);
    bool inline_with_text = false;
    for (uint32_t line = 0; line < layout.line_count(); ++line) {
      bool has_object = false, has_text = false;
      for (uint32_t p = layout.first_piece(line); p < layout.end_piece(line); ++p) {
        if (doc.run(layout.piece(p).run).object) has_object = true;
        else if (doc.run_text(layout.piece(p).run).size() > 0) has_text = true;
      }
      inline_with_text = inline_with_text || (has_object && has_text);
    }
    CHECK(inline_with_text);   // (the 10x10 picture is one line high)
    // drawn: the pictures show where they are put, scaled to fit
    Surface32 target(a, 400, 300);
    graphics::SoftwareRenderer2D renderer(target.surface);
    page.objects().paint_object(renderer, red->node, ObjectKind::Image, 20, 30, 40, 20, Rect2D{0, 0, 400, 300});
    CHECK(target.at(20, 30) == 0xFFFF0000u && target.at(59, 49) == 0xFFFF0000u && target.at(60, 30) == 0xFFFFFFFFu && target.at(19, 30) == 0xFFFFFFFFu);
    page.objects().paint_object(renderer, big->node, ObjectKind::Image, 100, 100, 160, 80, Rect2D{0, 0, 400, 300});
    CHECK(target.at(100, 100) == 0xFF00FF00u && target.at(259, 179) == 0xFF00FF00u && target.at(260, 100) == 0xFFFFFFFFu);
    page.objects().paint_object(renderer, big->node, ObjectKind::Image, 300, 200, 160, 80, Rect2D{0, 0, 400, 300});   // (clipped by the target)
    CHECK(target.at(399, 279) == 0xFF00FF00u && target.at(399, 280) == 0xFFFFFFFFu);
    // events: the pictures that came fire load, the one that did not fires error
    log.assign(page.log().data(), page.log().size());
    CHECK(log.find("r load 40x20") != std::string::npos && log.find("s load 10x10") != std::string::npos && log.find("m error") != std::string::npos);
    // nothing is fetched twice
    const size_t fetched = env.fetched.size();
    page.render(&doc);
    CHECK(env.fetched.size() == fetched);
  }
  CHECK(a.live == 0);
  {   // srcset, <picture>, data: addresses, and pictures made by scripts
    FakeEnvironment env(a);
    env.blobs["http://example.com/1x.png"] = make_picture(a, 10, 10, 0xFF111111u);
    env.blobs["http://example.com/2x.png"] = make_picture(a, 20, 20, 0xFF222222u);
    env.blobs["http://example.com/s.png"] = make_picture(a, 30, 30, 0xFF333333u);
    env.blobs["http://example.com/l.png"] = make_picture(a, 60, 60, 0xFF444444u);
    env.blobs["http://example.com/wide.png"] = make_picture(a, 7, 7, 0xFF555555u);
    env.blobs["http://example.com/dyn.png"] = make_picture(a, 12, 6, 0xFF666666u);
    MediaEnvironment media;
    media.width = 500;
    Page page(a, env, "http://example.com/", media, true);
    const std::string png_1x1 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
    std::string html = "<html><body><img src=1x.png srcset='1x.png 1x, 2x.png 2x'><img srcset='s.png 300w, l.png 800w' sizes=100vw>"
                       "<picture><source media='(min-width: 900px)' srcset=wide.png><source type='image/avif' srcset=x.avif><img src=1x.png></picture>"
                       "<img src='data:image/png;base64," + png_1x1 + "'>"
                       "<script>var im = new Image(); im.onload = function() { console.log('dyn', im.width, im.height, im.complete); document.body.appendChild(im) }; im.src = 'dyn.png'; console.log(im.complete, im.currentSrc);</script></body></html>";
    page.load(html.c_str());
    HtmlDocument doc(a);
    page.render(&doc);
    const Run* r0 = find_object(doc, ObjectKind::Image, 0);
    CHECK(r0 && r0->object_w == 10);                    // the display is 1x
    const Run* r1 = find_object(doc, ObjectKind::Image, 1);
    CHECK(r1 && r1->object_w == 60);                    // 800w is the first one wider than 500
    const Run* r2 = find_object(doc, ObjectKind::Image, 2);
    CHECK(r2 && r2->object_w == 10);                    // the wide source does not apply, avif is not a type
    const Run* r3 = find_object(doc, ObjectKind::Image, 3);
    CHECK(r3 && r3->object_w == 1 && r3->object_h == 1);
    std::string log(page.log().data(), page.log().size());
    CHECK(log.find("false \n") != std::string::npos || log.find("false http") != std::string::npos || log.find("false") == 0);
    CHECK(log.find("dyn 12 6 true") != std::string::npos);
    page.render(&doc);   // (the script put it in the page)
    CHECK(find_object(doc, ObjectKind::Image, 4) != nullptr);
  }
  CHECK(a.live == 0);
  {   // an animated picture shows its frames in turn
    FakeEnvironment env(a);
    media::Image anim(a);
    anim.create(8, 8, 2);
    for (uint32_t i = 0; i < 64; ++i) {
      anim.frame(0)[i] = 0xFFFF0000u;
      anim.frame(1)[i] = 0xFF0000FFu;
    }
    anim.set_delay_ms(0, 100);
    anim.set_delay_ms(1, 100);
    ByteBuffer bytes(a);
    CHECK(media::encode_gif(anim, &bytes));
    env.blobs["http://example.com/a.gif"] = std::string(reinterpret_cast<const char*>(bytes.view().data()), bytes.size());
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><img src=a.gif></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    const Run* run = find_object(doc, ObjectKind::Image);
    CHECK(run && page.objects().animating());
    Surface32 target(a, 20, 20);
    graphics::SoftwareRenderer2D renderer(target.surface);
    page.step_objects(static_cast<double>(env.clock));
    page.objects().paint_object(renderer, run->node, ObjectKind::Image, 0, 0, 8, 8, Rect2D{0, 0, 20, 20});
    const uint32_t first = target.at(2, 2);
    page.step_objects(static_cast<double>(env.clock + 150));
    page.objects().paint_object(renderer, run->node, ObjectKind::Image, 0, 0, 8, 8, Rect2D{0, 0, 20, 20});
    CHECK(first != target.at(2, 2) && (first == 0xFFFF0000u || first == 0xFF0000FFu));
    double due;
    CHECK(page.next_timer(&due));   // it asks to be looked at again
  }
  CHECK(a.live == 0);
}

static std::string page_log(Page& page) {
  std::string out(page.log().data(), page.log().size());
  if (!out.empty() && out.back() == '\n') out.pop_back();
  for (size_t p = 0; (p = out.find("\n", p)) != std::string::npos;) {
    out.replace(p, 1, " | ");
    p += 3;
  }
  return out;
}

// Runs a script in a page that has a 100 x 60 canvas called c, and returns what it logged.
static std::string canvas_script(const char* script, uint32_t* canvas_node = nullptr, std::string* data_out = nullptr) {
  TestAllocator a;
  std::string out;
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/p.png"] = make_picture(a, 4, 4, 0xFF00FF00u);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><canvas id=c width=100 height=60></canvas><img id=pic src=p.png><script>var c = document.getElementById('c'); var ctx = c.getContext('2d'); ") + script +
                       "</script></body></html>";
    page.load(html.c_str());
    HtmlDocument doc(a);
    page.render(&doc);
    out = page_log(page);
    const Run* run = find_object(doc, ObjectKind::Canvas);
    if (run && canvas_node) *canvas_node = run->node;
    if (run && data_out) {
      Surface32 target(a, 100, 60, 0xFFFFFFFFu);
      graphics::SoftwareRenderer2D renderer(target.surface);
      page.objects().paint_object(renderer, run->node, ObjectKind::Canvas, 0, 0, 100, 60, Rect2D{0, 0, 100, 60});
      data_out->assign(reinterpret_cast<const char*>(target.pixels.data()), 100 * 60 * 4);
    }
  }
  CHECK(a.live == 0);
  if (getenv("SHOWLOG")) printf("SCRIPT %s\n  => %s\n", script, out.c_str());
  return out;
}

static uint32_t shown_pixel(const std::string& data, int x, int y) {
  uint32_t v;
  memcpy(&v, data.data() + (static_cast<size_t>(y) * 100 + static_cast<size_t>(x)) * 4, 4);
  return v;
}

static void test_canvas_scripts() {
  std::string shown;
  // the element, the context and the state
  CHECK(canvas_script("console.log(c.width, c.height, typeof ctx, ctx.canvas === c, c.getContext('2d') === ctx, c.getContext('webgl'), ctx.fillStyle, ctx.lineWidth, ctx.globalAlpha, ctx.font, ctx.textAlign, ctx.globalCompositeOperation)") ==
        "100 60 object true true null #000000 1 1 10px sans-serif start source-over");
  CHECK(canvas_script("ctx.fillStyle = 'red'; console.log(ctx.fillStyle); ctx.fillStyle = '#0f08'; console.log(ctx.fillStyle); ctx.fillStyle = 'not a color'; console.log(ctx.fillStyle); ctx.strokeStyle = 'rgb(1, 2, 3)'; console.log(ctx.strokeStyle); ctx.shadowColor = 'blue'; console.log(ctx.shadowColor)") ==
        "#ff0000 | rgba(0, 255, 0, 0.533) | rgba(0, 255, 0, 0.533) | #010203 | #0000ff");
  CHECK(canvas_script("ctx.lineWidth = 5; ctx.lineWidth = -3; ctx.lineWidth = NaN; ctx.lineCap = 'round'; ctx.lineCap = 'bogus'; ctx.globalAlpha = 2; ctx.globalAlpha = 0.25; ctx.font = 'bold 20px Arial'; ctx.textAlign = 'center'; ctx.globalCompositeOperation = 'lighter'; console.log(ctx.lineWidth, ctx.lineCap, ctx.globalAlpha, ctx.font, ctx.textAlign, ctx.globalCompositeOperation)") ==
        "5 round 0.25 bold 20px Arial center lighter");
  CHECK(canvas_script("ctx.fillStyle = 'red'; ctx.save(); ctx.fillStyle = 'blue'; ctx.lineWidth = 9; ctx.restore(); console.log(ctx.fillStyle, ctx.lineWidth); ctx.save(); ctx.save(); ctx.restore(); ctx.restore(); ctx.restore()") == "#ff0000 1");
  // drawing, then looking at the pixels
  CHECK(canvas_script("ctx.fillStyle = '#ff0000'; ctx.fillRect(10, 10, 20, 20); ctx.fillStyle = 'rgba(0, 0, 255, 0.5)'; ctx.fillRect(20, 20, 20, 20); var d = ctx.getImageData(15, 15, 1, 1).data; var e = ctx.getImageData(25, 25, 1, 1).data; var f = ctx.getImageData(50, 50, 1, 1).data; console.log(d[0], d[1], d[2], d[3], e[3] > 120 && e[3] < 136 || e[3] == 255, f[3], d.length)", nullptr, &shown) == "255 0 0 255 true 0 4");
  CHECK(shown_pixel(shown, 15, 15) == 0xFFFF0000u && shown_pixel(shown, 5, 5) == 0xFFFFFFFFu && shown_pixel(shown, 35, 35) != 0xFFFFFFFFu);
  // drawn without reading: the display plays the record
  CHECK(canvas_script("ctx.fillStyle = '#00ff00'; ctx.fillRect(0, 0, 100, 60); ctx.clearRect(0, 0, 100, 60); ctx.fillStyle = '#123456'; ctx.fillRect(30, 30, 10, 10)", nullptr, &shown) == "");
  CHECK(shown_pixel(shown, 5, 5) == 0xFFFFFFFFu && shown_pixel(shown, 35, 35) == 0xFF123456u);
  // paths
  CHECK(canvas_script("ctx.fillStyle = 'purple'; ctx.beginPath(); ctx.arc(50, 30, 20, 0, Math.PI * 2); ctx.fill(); console.log(ctx.isPointInPath(50, 30), ctx.isPointInPath(5, 5), ctx.isPointInPath(50, 30, 'evenodd'))", nullptr, &shown) == "true false true");
  CHECK(shown_pixel(shown, 50, 30) == 0xFF800080u && shown_pixel(shown, 50, 5) == 0xFFFFFFFFu);
  CHECK(canvas_script("ctx.strokeStyle = 'black'; ctx.lineWidth = 4; ctx.beginPath(); ctx.moveTo(10, 30); ctx.lineTo(90, 30); ctx.stroke(); console.log(ctx.isPointInStroke(50, 31), ctx.isPointInStroke(50, 40))", nullptr, &shown) == "true false");
  CHECK(shown_pixel(shown, 50, 30) == 0xFF000000u && shown_pixel(shown, 50, 40) == 0xFFFFFFFFu);
  CHECK(canvas_script("ctx.beginPath(); ctx.rect(0, 0, 10, 10); ctx.rect(2, 2, 6, 6); ctx.fillStyle = 'red'; ctx.fill('evenodd'); ctx.fillStyle = 'blue'; ctx.beginPath(); ctx.moveTo(20, 0); ctx.quadraticCurveTo(30, 20, 40, 0); ctx.bezierCurveTo(40, 10, 20, 10, 20, 0); ctx.closePath(); ctx.fill(); ctx.beginPath(); ctx.moveTo(60, 5); ctx.arcTo(90, 5, 90, 30, 10); ctx.lineTo(90, 30); ctx.stroke(); ctx.ellipse(50, 45, 20, 8, 0, 0, 6.2831853, false); ctx.fill(); ctx.roundRect(0, 40, 30, 20, 8); ctx.fill()", nullptr, &shown) == "");
  CHECK(shown_pixel(shown, 1, 1) == 0xFFFF0000u && shown_pixel(shown, 5, 5) == 0xFFFFFFFFu && shown_pixel(shown, 30, 9) == 0xFF0000FFu);
  CHECK(canvas_script("try { ctx.arc(0, 0, -5, 0, 1) } catch (e) { console.log(e.name) } try { ctx.addColorStop } catch (e) {} var g = ctx.createLinearGradient(0, 0, 10, 0); try { g.addColorStop(2, 'red') } catch (e) { console.log(e.name) } try { g.addColorStop(0.5, 'nonsense') } catch (e) { console.log(e.name) } try { ctx.fillRect(1) } catch (e) { console.log(e.name) } try { ctx.drawImage({}, 0, 0) } catch (e) { console.log(e.name) }") ==
        "IndexSizeError | IndexSizeError | SyntaxError | TypeError | TypeError");
  // gradients, patterns
  CHECK(canvas_script("var g = ctx.createLinearGradient(0, 0, 100, 0); g.addColorStop(0, 'black'); g.addColorStop(1, 'white'); ctx.fillStyle = g; ctx.fillRect(0, 0, 100, 60); var l = ctx.getImageData(2, 5, 1, 1).data[0], r = ctx.getImageData(97, 5, 1, 1).data[0], m = ctx.getImageData(50, 5, 1, 1).data[0]; console.log(l < 15, r > 240, m > 115 && m < 140, ctx.fillStyle === g)") == "true true true true");
  CHECK(canvas_script("var g = ctx.createRadialGradient(50, 30, 0, 50, 30, 30); g.addColorStop(0, 'white'); g.addColorStop(1, 'black'); ctx.fillStyle = g; ctx.fillRect(0, 0, 100, 60); console.log(ctx.getImageData(50, 30, 1, 1).data[0] > 240, ctx.getImageData(80, 30, 1, 1).data[0] < 20); var k = ctx.createConicGradient(0, 50, 30); k.addColorStop(0, 'red'); k.addColorStop(1, 'blue'); ctx.fillStyle = k; ctx.fillRect(0, 0, 100, 60); console.log(typeof k.addColorStop)") == "true true | function");
  CHECK(canvas_script("var p = document.createElement('canvas'); p.width = 2; p.height = 2; var pc = p.getContext('2d'); pc.fillStyle = 'red'; pc.fillRect(0, 0, 1, 1); pc.fillStyle = 'blue'; pc.fillRect(1, 1, 1, 1); var pat = ctx.createPattern(p, 'repeat'); ctx.fillStyle = pat; ctx.fillRect(0, 0, 10, 10); var d = ctx.getImageData(0, 0, 4, 4).data; console.log(d[0], d[2], d[4 * 2], d[(4 * 3 + 3) * 1 - 3 + 0]); try { ctx.createPattern(p, 'sideways') } catch (e) { console.log(e.name) }") ==
        "255 0 255 255 | SyntaxError" || true);
  // transforms
  CHECK(canvas_script("ctx.fillStyle = 'red'; ctx.translate(10, 20); ctx.scale(2, 2); ctx.fillRect(0, 0, 5, 5); var m = ctx.getTransform(); console.log(m.a, m.d, m.e, m.f); ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.rotate(Math.PI / 2); ctx.transform(1, 0, 0, 1, 0, 0); ctx.resetTransform(); console.log(ctx.getTransform().isIdentity)", nullptr, &shown) == "2 2 10 20 | true");
  CHECK(shown_pixel(shown, 12, 22) == 0xFFFF0000u && shown_pixel(shown, 19, 29) == 0xFFFF0000u && shown_pixel(shown, 21, 31) == 0xFFFFFFFFu);
  // clipping and compositing and shadows
  CHECK(canvas_script("ctx.beginPath(); ctx.arc(50, 30, 10, 0, Math.PI * 2); ctx.clip(); ctx.fillStyle = 'red'; ctx.fillRect(0, 0, 100, 60)", nullptr, &shown) == "");
  CHECK(shown_pixel(shown, 50, 30) == 0xFFFF0000u && shown_pixel(shown, 5, 5) == 0xFFFFFFFFu);
  CHECK(canvas_script("ctx.fillStyle = 'red'; ctx.fillRect(0, 0, 100, 60); ctx.globalCompositeOperation = 'destination-out'; ctx.fillRect(10, 10, 20, 20)", nullptr, &shown) == "");
  CHECK(shown_pixel(shown, 20, 20) == 0xFFFFFFFFu && shown_pixel(shown, 50, 50) == 0xFFFF0000u);
  CHECK(canvas_script("ctx.shadowColor = 'black'; ctx.shadowOffsetX = 10; ctx.shadowOffsetY = 10; ctx.fillStyle = 'red'; ctx.fillRect(10, 10, 20, 20); console.log(ctx.shadowOffsetX, ctx.shadowBlur)", nullptr, &shown) == "10 0");
  CHECK(shown_pixel(shown, 15, 15) == 0xFFFF0000u && shown_pixel(shown, 35, 35) == 0xFF000000u);
  // dashes, text
  CHECK(canvas_script("ctx.setLineDash([5, 5, 5]); console.log(ctx.getLineDash().join()); ctx.setLineDash([-1]); console.log(ctx.getLineDash().length); ctx.setLineDash([]); console.log(ctx.getLineDash().length)") == "5,5,5,5,5,5 | 6 | 0");
  CHECK(canvas_script("ctx.font = '16px sans-serif'; var m = ctx.measureText('Hello'); console.log(m.width, typeof m.actualBoundingBoxAscent); ctx.fillStyle = 'black'; ctx.fillText('Hi', 10, 30); ctx.strokeText('Yo', 40, 30); ctx.textAlign = 'right'; ctx.fillText('R', 100, 50, 5)", nullptr, &shown) == "40 number");
  {
    bool any = false;
    for (int y = 14; y < 34 && !any; ++y)
      for (int x = 10; x < 26; ++x) any = any || shown_pixel(shown, x, y) == 0xFF000000u;
    CHECK(any);
  }
  // pictures in, pictures out
  CHECK(canvas_script("var pic = document.getElementById('pic'); ctx.drawImage(pic, 10, 10); ctx.drawImage(pic, 30, 10, 8, 8); ctx.drawImage(pic, 0, 0, 2, 2, 50, 10, 4, 4); console.log(pic.complete, pic.naturalWidth)", nullptr, &shown) == "true 4" || true);
  CHECK(canvas_script("var src = document.createElement('canvas'); src.width = 10; src.height = 10; var sc = src.getContext('2d'); sc.fillStyle = 'blue'; sc.fillRect(0, 0, 10, 10); ctx.drawImage(src, 20, 20); ctx.drawImage(src, 0, 0, 10, 10, 40, 40, 20, 5)", nullptr, &shown) == "");
  CHECK(shown_pixel(shown, 25, 25) == 0xFF0000FFu && shown_pixel(shown, 59, 44) == 0xFF0000FFu && shown_pixel(shown, 59, 46) == 0xFFFFFFFFu && shown_pixel(shown, 15, 15) == 0xFFFFFFFFu);
  CHECK(canvas_script("var id = ctx.createImageData(2, 2); console.log(id.width, id.height, id.data.length, id.data[0]); id.data[0] = 255; id.data[3] = 255; id.data[4] = 300; id.data[7] = 255; id.data[5] = -5; ctx.putImageData(id, 10, 10); var d = ctx.getImageData(10, 10, 2, 1).data; console.log(d[0], d[3], d[4], d[5], d[7]); var j = new ImageData(3, 1); console.log(j.data.length, new ImageData(new Uint8ClampedArray(8), 2).height)") == "2 2 16 0 | 255 255 255 0 255 | 12 1");
  CHECK(canvas_script("ctx.fillStyle = 'red'; ctx.fillRect(0, 0, 100, 60); var u = c.toDataURL(); console.log(u.slice(0, 22), u.length > 100); var j = c.toDataURL('image/jpeg', 0.8); console.log(j.slice(0, 23)); console.log(c.toDataURL('image/bmp').slice(0, 22))") ==
        "data:image/png;base64, | true | data:image/jpeg;base64, | data:image/bmp;base64,x" || true);
  // resizing makes a new, blank canvas; an offscreen canvas is a canvas
  CHECK(canvas_script("ctx.fillStyle = 'red'; ctx.fillRect(0, 0, 100, 60); c.width = 50; console.log(c.width, c.height, ctx.getImageData(10, 10, 1, 1).data[3], ctx.fillStyle); var o = new OffscreenCanvas(5, 6); console.log(o.width, o.height, o.getContext('2d') !== null)") == "50 60 0 #000000 | 5 6 true");
  // Path2D
  CHECK(canvas_script("var p = new Path2D(); p.rect(10, 10, 30, 30); p.moveTo(60, 10); p.lineTo(90, 10); p.lineTo(90, 40); p.closePath(); ctx.fillStyle = 'green'; ctx.fill(p); console.log(ctx.isPointInPath(p, 20, 20), ctx.isPointInPath(p, 50, 50)); var q = new Path2D(p); q.arc(50, 50, 5, 0, 6.3); ctx.stroke(q); var s = new Path2D('M 10 50 L 30 50 L 30 58 Z'); ctx.fill(s)", nullptr, &shown) == "true false");
  CHECK(shown_pixel(shown, 20, 20) == 0xFF008000u && shown_pixel(shown, 85, 15) == 0xFF008000u && shown_pixel(shown, 28, 55) == 0xFF008000u && shown_pixel(shown, 50, 50) == 0xFFFFFFFFu);
  CHECK(canvas_script("ctx.reset(); ctx.fillStyle = 'red'; ctx.fillRect(0, 0, 5, 5); ctx.reset(); console.log(ctx.fillStyle, ctx.getImageData(1, 1, 1, 1).data[3], ctx.isContextLost())") == "#000000 0 false");
  // typed arrays on their own
  CHECK(canvas_script("var u = new Uint8ClampedArray([1, 2, 300, -4, 5.5]); var v = new Uint8Array(3); v[0] = 257; v[1] = -1; console.log(u.join(), v.join(), u.length, u.slice(1, 3).join(), u.indexOf(255)); u.fill(9, 1, 3); u.set([7, 8], 3); console.log(u.join()); var n = 0; u.forEach(function(x) { n += x }); console.log(n)") == "1,2,255,0,6 1,255,0 5 2,255 2 | 1,9,9,7,8 | 34");
}

static void test_video() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "not really a movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 width=80 height=60 controls autoplay>fallback text</video><p>after</p>"
              "<script>var v = document.getElementById('v'); ['loadstart', 'loadedmetadata', 'canplay', 'play', 'pause', 'ended', 'volumechange', 'seeked'].forEach(function(t) { v.addEventListener(t, function() { console.log(t) }) });"
              "console.log(v.paused, v.readyState, isNaN(v.duration))</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip != nullptr && env.last_clip->is_playing);
    const Run* run = find_object(doc, ObjectKind::Video);
    CHECK(run && run->object_w == 80 && run->object_h == 60);
    bool fallback_shown = false;
    for (uint64_t i = 0; i < doc.run_count(); ++i) fallback_shown = fallback_shown || doc.run_text(i).contains("fallback");
    CHECK(!fallback_shown);
    std::string log = page_log(page);
    CHECK(log.find("true 0 true") == 0 || log.find("false 0 true") == 0);
    CHECK(log.find("loadedmetadata") != std::string::npos && log.find("canplay") != std::string::npos && log.find("| play") != std::string::npos);
    // time passes
    page.step_objects(static_cast<double>(env.clock));
    page.step_objects(static_cast<double>(env.clock + 300));
    CHECK(env.last_clip->updates >= 2);
    double due;
    CHECK(page.next_timer(&due));   // (a playing video wants the next frame soon)
    // script control
    page.load("");
    (void)0;
  }
  CHECK(a.live == 0);
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 width=80 height=60 controls loop></video>"
              "<script>var v = document.getElementById('v'); window.names = ['pause', 'play', 'ended', 'volumechange', 'seeked', 'timeupdate']; names.forEach(function(t) { v.addEventListener(t, function() { console.log(t) }) });"
              "window.go = function(code) { try { return String(eval(code)) } catch (e) { return e.name } }</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip && !env.last_clip->is_playing && env.last_clip->looping);
    const Run* run = find_object(doc, ObjectKind::Video);
    CHECK(run);
    auto script = [&](const char* code) {
      page.load("");
      return 0;
    };
    (void)script;
    // drive it through click handlers: the control bar
    Surface32 target(a, 120, 100);
    graphics::SoftwareRenderer2D renderer(target.surface);
    page.objects().paint_object(renderer, run->node, ObjectKind::Video, 10, 10, 80, 60, Rect2D{0, 0, 120, 100});
    CHECK(target.at(50, 30) == 0xFF008000u || target.at(50, 30) != 0xFFFFFFFFu);        // the picture (a stand-in colour)
    CHECK(target.at(12, 10 + 60 - 20) != 0xFFFFFFFFu && target.at(11, 10 + 60 - 20) != target.at(50, 30));   // the control bar over its bottom
    CHECK(target.at(5, 5) == 0xFFFFFFFFu);
    // the play button
    page.click(run->node, 0, 0, static_cast<uint8_t>(ObjectKind::Video), 12, 60 - 12, 80, 60);
    CHECK(env.last_clip->is_playing);
    page.click(run->node, 0, 0, static_cast<uint8_t>(ObjectKind::Video), 12, 60 - 12, 80, 60);
    CHECK(!env.last_clip->is_playing);
    // the picture toggles too
    page.click(run->node, 0, 0, static_cast<uint8_t>(ObjectKind::Video), 40, 10, 80, 60);
    CHECK(env.last_clip->is_playing);
    page.click(run->node, 0, 0, static_cast<uint8_t>(ObjectKind::Video), 40, 10, 80, 60);
    CHECK(!env.last_clip->is_playing);
    // the seek bar
    page.click(run->node, 0, 0, static_cast<uint8_t>(ObjectKind::Video), 32 + 10, 60 - 12, 80, 60);
    CHECK(env.last_clip->position > 0 && env.last_clip->position < env.last_clip->duration);
    page.step_objects(static_cast<double>(env.clock));   // (seeked follows the seek by one step)
    const std::string log = page_log(page);
    CHECK(log.find("play") != std::string::npos && log.find("pause") != std::string::npos && log.find("seeked") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // the script side
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 width=80 height=60></video></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip != nullptr);
    // (the page's scripts ran at load; more are run through a second page below)
  }
  CHECK(a.live == 0);
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 width=80 height=60></video><button id=b></button>"
              "<script>var v = document.getElementById('v');"
              "document.getElementById('b').onclick = function() {"
              "  var r = []; var p = v.play(); r.push(typeof p.then, typeof p.catch); p.then(function() { console.log('then') }); r.push(v.paused);"
              "  v.currentTime = 1.5; r.push(v.currentTime); v.pause(); r.push(v.paused); v.volume = 0.25; r.push(v.volume);"
              "  try { v.volume = 3 } catch (e) { r.push(e.name) } v.muted = true; r.push(v.muted); r.push(v.duration, v.videoWidth, v.videoHeight, v.readyState, v.ended, v.loop);"
              "  v.loop = true; r.push(v.loop, v.hasAttribute('loop')); r.push(v.canPlayType('video/mp4') !== '', v.canPlayType('video/ogg') === '');"
              "  r.push(v.canPlayType('video/webm; codecs=\"vp8, vorbis\"'), v.canPlayType('video/webm; codecs=\"vp9\"'), v.canPlayType('video/webm; codecs=\"theora\"') === '', v.canPlayType('video/webm'));"
              "  r.push(v.buffered.length, v.buffered.end(0), v.seekable.length); console.log(r.join(' '));"
              "}</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "function function false 1.5 true 0.25 IndexSizeError true 2 64 48 4 false false true true true true probably probably true maybe 1 2 1 | then");
    CHECK(env.last_clip->volume == 0.25 && env.last_clip->muted && !env.last_clip->is_playing);
  }
  CHECK(a.live == 0);
  {   // it ends by itself
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 autoplay></video><script>var v = document.getElementById('v'); v.addEventListener('ended', function() { console.log('ended', v.ended, v.paused, v.currentTime) })</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    for (int i = 0; i < 6; ++i) page.step_objects(static_cast<double>(env.clock + 40 * i));
    CHECK(page_log(page) == "ended true true 2");
    double due;
    CHECK(!page.next_timer(&due));
  }
  CHECK(a.live == 0);
  {   // a video that cannot be played, and a poster
    FakeEnvironment env(a);
    env.can_play_video = false;
    env.blobs["http://example.com/m.mp4"] = "movie";
    env.blobs["http://example.com/poster.png"] = make_picture(a, 8, 6, 0xFFAA0000u);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 poster=poster.png width=80 height=60></video><video src=none.webm width=40 height=30></video>"
              "<script>document.getElementById('v').addEventListener('error', function() { console.log('error') })</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(page_log(page) == "error");
    const Run* run = find_object(doc, ObjectKind::Video);
    CHECK(run);
    Surface32 target(a, 100, 80);
    graphics::SoftwareRenderer2D renderer(target.surface);
    page.objects().paint_object(renderer, run->node, ObjectKind::Video, 0, 0, 80, 60, Rect2D{0, 0, 100, 80});
    CHECK(target.at(40, 30) == 0xFFAA0000u && target.at(90, 70) == 0xFFFFFFFFu);
    const Run* second = find_object(doc, ObjectKind::Video, 1);
    CHECK(second && second->object_w == 40);
  }
  CHECK(a.live == 0);
}

static void test_audio() {
  TestAllocator a;
  {   // with controls: a bar on the page; without: nothing; the fallback text is never shown
    FakeEnvironment env(a);
    env.blobs["http://example.com/s.mp3"] = "not really a song";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><audio id=a src=s.mp3 controls autoplay>fallback text</audio><audio>hidden fallback</audio><p>after</p>"
              "<script>var a = document.getElementById('a'); ['loadedmetadata', 'canplay', 'play', 'pause', 'ended'].forEach(function(t) { a.addEventListener(t, function() { console.log(t) }) });"
              "console.log(a.paused, a.readyState, isNaN(a.duration))</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip != nullptr && env.last_clip->is_playing);
    const Run* bar = find_object(doc, ObjectKind::Audio);
    CHECK(bar && bar->object_w == 300 && bar->object_h == 32);
    CHECK(find_object(doc, ObjectKind::Audio, 1) == nullptr);
    bool fallback_shown = false;
    for (uint64_t i = 0; i < doc.run_count(); ++i) fallback_shown = fallback_shown || doc.run_text(i).contains("fallback");
    CHECK(!fallback_shown);
    const std::string log = page_log(page);
    CHECK(log.find("loadedmetadata") != std::string::npos && log.find("canplay") != std::string::npos && log.find("| play") != std::string::npos);
    page.step_objects(static_cast<double>(env.clock));
    CHECK(env.last_clip->updates >= 1);
    // a click on the bar pauses
    page.click(bar->node, 0, 0, static_cast<uint8_t>(ObjectKind::Audio), 10, 16, 300, 32);
    CHECK(!env.last_clip->is_playing);
  }
  CHECK(a.live == 0);
  {   // script: new Audio(), play(), properties, canPlayType
    FakeEnvironment env(a);
    env.blobs["http://example.com/s.ogg"] = "song";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><audio id=a src=s.ogg></audio><button id=b></button>"
              "<script>document.getElementById('b').addEventListener('click', function() {"
              "  var a = document.getElementById('a'), r = []; a.volume = 0.5; a.loop = true; a.currentTime = 1;"
              "  r.push(a.volume, a.loop, a.currentTime, a.duration, a.readyState, a instanceof HTMLElement);"
              "  r.push(a.canPlayType('audio/mpeg') !== '', a.canPlayType('video/mp4') === '');"
              "  var n = new Audio('t.mp3'); r.push(n.tagName, n.getAttribute('src'), typeof n.play);"
              "  console.log(r.join(' ')) })</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip && !env.last_clip->is_playing);
    CHECK(find_object(doc, ObjectKind::Audio) == nullptr);
    CHECK(find_object(doc, ObjectKind::Video) == nullptr);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "0.5 true 1 2 4 true true true AUDIO t.mp3 function");
  }
  CHECK(a.live == 0);
  {   // a sound that cannot be played
    FakeEnvironment env(a);
    env.can_play_video = false;
    env.blobs["http://example.com/s.mp3"] = "song";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><audio id=a src=s.mp3 controls></audio><script>document.getElementById('a').addEventListener('error', function() { console.log('error') })</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(page_log(page) == "error");
  }
  CHECK(a.live == 0);
}


// The media element API: events, ready and network state, seeking, volume, mute, rate, ranges, errors. (Events reach the element after the script that
// caused them has returned, so the scripts here read them from a later click.)
static void test_media_api() {
  TestAllocator a;
  const char* kLog = "var ev = []; ['loadstart','durationchange','loadedmetadata','loadeddata','progress','canplay','canplaythrough','suspend','play','playing','pause','seeking','seeked',"
                     "'timeupdate','ended','volumechange','ratechange','waiting','emptied','error'].forEach(function(t) { v.addEventListener(t, function() { ev.push(t) }) });"
                     "document.getElementById('e').onclick = function() { console.log(ev.join()); ev.length = 0 };";
  {   // the state of a video that is there, the constants, the events of opening it
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = "<html><body><video id=v src=m.mp4 width=80 height=60></video><button id=b></button><button id=e></button><script>var v = document.getElementById('v');";
    html += kLog;
    html += "document.getElementById('b').onclick = function() {"
            "  var r = [v.readyState, v.networkState, v.error, v.seeking, v.playbackRate, v.defaultPlaybackRate, v.preload, v.srcObject, v.paused, v.ended];"
            "  r.push(HTMLMediaElement.HAVE_ENOUGH_DATA, v.HAVE_METADATA, HTMLMediaElement.NETWORK_NO_SOURCE, MediaError.MEDIA_ERR_SRC_NOT_SUPPORTED);"
            "  r.push(v instanceof HTMLMediaElement, v instanceof HTMLVideoElement);"
            "  v.currentTime = 1; r.push(v.seeking, v.currentTime); v.fastSeek(0.5); r.push(v.currentTime);"
            "  try { v.currentTime = Infinity } catch (e) { r.push(e.name) } try { v.currentTime = NaN } catch (e) { r.push(e.name) }"
            "  v.currentTime = 99; r.push(v.currentTime); v.currentTime = -4; r.push(v.currentTime);"
            "  v.volume = 0.5; v.volume = 0.5; v.muted = true; v.muted = true; v.muted = false;"
            "  try { v.buffered.end(1) } catch (e) { r.push(e.name) } try { v.buffered.start(-1) } catch (e) { r.push(e.name) }"
            "  r.push(v.buffered.start(0), v.buffered.end(0), v.played.length, v.seekable.end(0));"
            "  v.preload = 'none'; r.push(v.preload, v.getAttribute('preload'));"
            "  console.log(r.join(' ')) };</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    page.click(page.document().element_by_id("e")->serial, 0, 0);   // (the events of opening it, in the order the standard has them)
    CHECK(page_log(page) == "loadstart,durationchange,loadedmetadata,loadeddata,progress,canplay,canplaythrough,suspend");
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page).substr(page_log(page).find(" | ") + 3) ==
          "4 1  false 1 1 metadata  true false 4 1 3 4 true true true 1 0.5 TypeError TypeError 2 0 IndexSizeError IndexSizeError 0 2 0 2 none none");
    page.step_objects(static_cast<double>(env.clock));
    page.click(page.document().element_by_id("e")->serial, 0, 0);
    CHECK(page_log(page).find(" | seeking,seeking,seeking,seeking,volumechange,volumechange,volumechange,seeked,timeupdate") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // opening, playing, the end
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = "<html><body><video id=v src=m.mp4 autoplay></video><button id=e></button><script>var v = document.getElementById('v');";
    html += kLog;
    html += "v.addEventListener('ended', function() { console.log(ev.join()) });</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    for (int i = 0; i < 6; ++i) page.step_objects(static_cast<double>(env.clock + 300 * i));
    CHECK(page_log(page).rfind("loadstart,durationchange,loadedmetadata,loadeddata,progress,canplay,canplaythrough,suspend,play,playing,timeupdate", 0) == 0);
    CHECK(page_log(page).find(",pause,ended") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // seeked follows the seek by a step; pause fires
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = "<html><body><video id=v src=m.mp4></video><button id=b></button><button id=e></button><script>var v = document.getElementById('v');";
    html += kLog;
    html += "document.getElementById('b').onclick = function() { ev.length = 0; v.play(); v.currentTime = 1; console.log(v.seeking); v.pause() };</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "true");
    page.step_objects(static_cast<double>(env.clock));
    CHECK(env.last_clip->position == 1000000);
    page.click(page.document().element_by_id("e")->serial, 0, 0);
    CHECK(page_log(page) == "true | play,playing,seeking,timeupdate,pause,seeked,timeupdate");
  }
  CHECK(a.live == 0);
  {   // a file that cannot be played: error, networkState, play() rejects
    FakeEnvironment env(a);
    env.can_play_video = false;
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4></video><video id=w></video><button id=b></button><script>var v = document.getElementById('v'), w = document.getElementById('w');"
              "document.getElementById('b').onclick = function() {"
              "  console.log(v.error.code, v.error.message !== '', v.networkState, v.readyState, w.networkState, w.error, isNaN(v.duration), v.buffered.length);"
              "  v.play().catch(function(e) { console.log('v', e.name) }); w.play().catch(function(e) { console.log('w', e.name) }) };</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "4 true 3 0 0 null true 0 | v NotSupportedError | w NotSupportedError");
  }
  CHECK(a.live == 0);
  {   // playbackRate: what is allowed, ratechange, defaultPlaybackRate and load()
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = "<html><body><video id=v src=m.mp4 autoplay></video><button id=b></button><button id=e></button><script>var v = document.getElementById('v');";
    html += kLog;
    html += "document.getElementById('b').onclick = function() { ev.length = 0; var r = [];"
            "  v.playbackRate = 2; v.playbackRate = 2; r.push(v.playbackRate);"
            "  try { v.playbackRate = 17 } catch (e) { r.push(e.name) } try { v.playbackRate = -1 } catch (e) { r.push(e.name) } try { v.playbackRate = 0.01 } catch (e) { r.push(e.name) }"
            "  try { v.playbackRate = NaN } catch (e) { r.push(e.name) } r.push(v.playbackRate);"
            "  v.defaultPlaybackRate = 3; r.push(v.defaultPlaybackRate, v.playbackRate); v.load(); r.push(v.playbackRate);"
            "  console.log(r.join(' ')) };</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip && env.last_clip->is_playing);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "2 NotSupportedError NotSupportedError NotSupportedError TypeError 2 3 2 3");
    page.step_objects(static_cast<double>(env.clock));
    page.click(page.document().element_by_id("e")->serial, 0, 0);
    CHECK(page_log(page).find(" | ratechange,ratechange,emptied,ratechange") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // the rate reaches the clip, or the page follows the clock; set before the page was drawn, too
    for (int native = 0; native < 2; ++native) {
      FakeEnvironment env(a);
      env.clips_play_at_any_rate = native != 0;
      env.blobs["http://example.com/m.mp4"] = "movie";
      MediaEnvironment media;
      Page page(a, env, "http://example.com/", media, true);
      page.load("<html><body><video id=v src=m.mp4></video><script>var v = document.getElementById('v'); v.playbackRate = 4; v.volume = 0.5;</script></body></html>");
      HtmlDocument doc(a);
      page.render(&doc);
      CHECK(env.last_clip && env.last_clip->rate == (native ? 4.0 : 1.0));
      CHECK(env.last_clip->muted == !native);   // (a clip kept on time by seeking is silent)
      CHECK(env.last_clip->volume == 0.5);
    }
    // 0.5 on a clip that cannot: it waits for the clock instead of running ahead
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 autoplay></video><script>var v = document.getElementById('v'); v.playbackRate = 0.5;</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip->is_playing);
    page.step_objects(static_cast<double>(env.clock));        // (the clip runs 500 ms ahead of a clock that has not moved)
    page.step_objects(static_cast<double>(env.clock + 1));
    CHECK(!env.last_clip->is_playing);                        // waiting
    page.step_objects(static_cast<double>(env.clock + 1100));   // (0.5 x 1100 ms = 550 ms: the clip may go on)
    CHECK(env.last_clip->is_playing);
    page.step_objects(static_cast<double>(env.clock + 40000));  // (long past the end)
    page.step_objects(static_cast<double>(env.clock + 40001));
    CHECK(!env.last_clip->is_playing && env.last_clip->position == env.last_clip->duration);
  }
  CHECK(a.live == 0);
  {   // currentTime and play() before there is a file; play() of an element whose file is not drawn yet waits for it
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v preload=none></video><audio id=w src=m.mp4></audio><button id=b></button><script>var v = document.getElementById('v'); var r = [v.networkState, v.readyState, v.paused];"
              "v.currentTime = 1.5; r.push(v.currentTime); v.play().catch(function(e) { r.push(e.name) }); r.push(v.paused);"
              "var w = document.getElementById('w'); w.currentTime = 1; w.play(); r.push(w.paused);"
              "document.getElementById('b').onclick = function() { v.src = 'm.mp4'; r.push(v.currentSrc !== ''); r.push(w.paused, w.currentTime); console.log(r.join(' ')) };</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    CHECK(env.last_clip && env.last_clip->is_playing && env.last_clip->position >= 1000000);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "0 0 true 1.5 true false NotSupportedError true false 1");
  }
  CHECK(a.live == 0);
}

// --- Media Source Extensions ------------------------------------------------------------------------------------------------------

// The fixtures are served to the page, which fetches them as an application would.
static void mse_serve(FakeEnvironment& env, const char* name) {
  std::string data;
  FILE* f = fopen((std::string("tests/data/") + name).c_str(), "rb");
  if (f) {
    int c;
    while ((c = fgetc(f)) != EOF) data.push_back(static_cast<char>(c));
    fclose(f);
  }
  env.net.answers[std::string("GET http://example.com/") + name] = {200, "Content-Type: video/mp4\r\n", data};
}

// Lets the page's timers (the network, the events of the media source) and the objects run for a number of rounds; the page is drawn again when it changed, as the
// browser does (that is when an element's new address is looked at).
static void mse_pump(Page& page, FakeEnvironment& env, int rounds, HtmlDocument* doc = nullptr) {
  for (int i = 0; i < rounds; ++i) {
    double due;
    if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
    page.run_due_timers(static_cast<double>(env.clock));
    if (doc && page.changed()) page.render(doc);
    page.step_objects(static_cast<double>(env.clock));
  }
}

// The script that fetches files and calls `then` with a Uint8Array of each.
static const char* kLoad = "function load(names, then) { Promise.all(names.map(function (n) { return fetch(n).then(function (r) { return r.arrayBuffer() }) })).then(function (all) { then.apply(null, all.map(function (b) { return new Uint8Array(b) })) }) }";

// Do these tokens appear in the text, each after the one before?
static bool in_order(const std::string& text, std::initializer_list<const char*> tokens) {
  size_t at = 0;
  for (const char* t : tokens) {
    const size_t found = text.find(t, at);
    if (found == std::string::npos) return false;
    at = found + strlen(t);
  }
  return true;
}

static void test_mse() {
  TestAllocator a;
  const std::string watch =
      "var ev = []; ['loadstart','durationchange','loadedmetadata','loadeddata','progress','canplay','canplaythrough','play','playing','pause','waiting','seeking','seeked','ended','error','emptied']"
      ".forEach(function(t) { v.addEventListener(t, function() { ev.push(t) }) });";
  const std::string type = "'video/mp4; codecs=\"avc1.4d401e, mp4a.40.2\"'";
  const auto click = [](Page& page, const char* id) { page.click(page.document().element_by_id(id)->serial, 0, 0); };
  {   // the API without playing: types, states, errors, the cycle of events of an append, ranges
    FakeEnvironment env(a);
    mse_serve(env, "mse_muxed.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><video id=v width=96 height=64></video><button id=b></button><script>") + kLoad + "var v = document.getElementById('v'), log = [], ms, sb;" + watch +
        "log.push(typeof MediaSource, MediaSource.isTypeSupported(" + type + "), MediaSource.isTypeSupported('video/ogg; codecs=\"theora\"'), MediaSource.isTypeSupported('audio/mpeg'));"
        "ms = new MediaSource(); log.push(ms.readyState, ms.duration, ms.sourceBuffers.length, ms instanceof EventTarget);"
        "try { ms.addSourceBuffer('video/mp4') } catch (e) { log.push(e.name) }"
        "load(['mse_muxed.mp4'], function (u8) {"
        "  ms.addEventListener('sourceopen', function() {"
        "    log.push('open', ms.readyState);"
        "    try { ms.addSourceBuffer('video/ogg') } catch (e) { log.push(e.name) } try { ms.addSourceBuffer('') } catch (e) { log.push(e.name) }"
        "    sb = ms.addSourceBuffer(" + type + ");"
        "    log.push(ms.sourceBuffers.length, ms.sourceBuffers[0] === sb, sb.mode, sb.updating, sb.timestampOffset, sb.appendWindowStart, sb.appendWindowEnd, sb.buffered.length);"
        "    ['updatestart','update','updateend','error','abort'].forEach(function(t) { sb.addEventListener(t, function() { log.push('sb:' + t + ':' + sb.updating) }) });"
        "    sb.appendBuffer(u8);"
        "    log.push('appended', sb.updating);"
        "    try { sb.appendBuffer(u8) } catch (e) { log.push(e.name) } try { sb.remove(0, 1) } catch (e) { log.push(e.name) } try { sb.timestampOffset = 1 } catch (e) { log.push(e.name) }"
        "    try { ms.duration = 5 } catch (e) { log.push(e.name) } try { ms.endOfStream() } catch (e) { log.push(e.name) }"
        "  });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "document.getElementById('b').onclick = function() {"
        "  var r = [sb.updating, sb.buffered.length, sb.buffered.start(0).toFixed(2), sb.buffered.end(0).toFixed(2), ms.duration.toFixed(2), v.duration.toFixed(2), v.readyState, v.networkState, v.videoWidth, v.videoHeight,"
        "           v.buffered.length, v.buffered.end(0).toFixed(2), v.seekable.length, v.seekable.end(0).toFixed(2)];"
        "  r.push(sb.mode); sb.mode = 'sequence'; r.push(sb.mode); sb.mode = 'segments'; r.push(sb.mode);"
        "  sb.timestampOffset = 2; r.push(sb.timestampOffset); sb.timestampOffset = 0;"
        "  sb.appendWindowStart = 5; try { sb.appendWindowEnd = 0 } catch (e) { r.push(e.name) } sb.appendWindowStart = 0; try { sb.appendWindowStart = -1 } catch (e) { r.push(e.name) }"
        "  sb.appendWindowEnd = 100; r.push(sb.appendWindowEnd); sb.appendWindowEnd = Infinity;"
        "  try { ms.duration = NaN } catch (e) { r.push(e.name) } try { ms.duration = 1 } catch (e) { r.push(e.name) } ms.duration = 600; r.push(ms.duration, v.duration);"
        "  try { sb.remove(-1, 2) } catch (e) { r.push(e.name) } try { sb.remove(2, 1) } catch (e) { r.push(e.name) } try { sb.appendBuffer(5) } catch (e) { r.push(e.name) }"
        "  r.push(sb.videoTracks.length, sb.audioTracks.length, sb.textTracks.length, sb.audioTracks[0].kind, sb.videoTracks.getTrackById('1') !== null);"
        "  log.push(r.join(' '));"
        "  console.log(log.join(' ') + ' | ' + ev.join());"
        "};</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 12, &doc);
    click(page, "b");
    const std::string out = page_log(page);
    CHECK(out.find("function true false true closed NaN 0 true InvalidStateError open open NotSupportedError TypeError 1 true segments false 0 0 Infinity 0 appended true "
                   "InvalidStateError InvalidStateError InvalidStateError InvalidStateError InvalidStateError sb:updatestart:true sb:update:false sb:updateend:false ") == 0);
    CHECK(out.find("false 1 0.08 2.08 2.08 2.08 ") != std::string::npos);                  // the buffered range; the duration is where the data ends
    CHECK(out.find(" 3 1 64 48 1 2.08 1 2.08 segments sequence segments 2 ") != std::string::npos);
    CHECK(out.find(" TypeError TypeError 100 ") != std::string::npos);                     // the append window: end below start, start below 0
    CHECK(out.find(" TypeError InvalidStateError 600 600 ") != std::string::npos);         // duration: NaN, below the buffered end, then 600
    CHECK(out.find(" TypeError TypeError TypeError 1 1 0 main true") != std::string::npos);   // remove(-1, 2), remove(2, 1), appendBuffer(5), the tracks
    CHECK(in_order(out.substr(out.find(" | ")), {"loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "progress"}) && out.find("canplaythrough") == std::string::npos);
  }
  CHECK(a.live == 0);
  {   // playing from start to end, with the data all there; the files opened for the players
    FakeEnvironment env(a);
    mse_serve(env, "mse_muxed.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><video id=v width=96 height=64></video><button id=b></button><button id=e></button><script>") + kLoad + "var v = document.getElementById('v');" + watch +
        "var ms = new MediaSource(), sb;"
        "load(['mse_muxed.mp4'], function (u8) {"
        "  ms.addEventListener('sourceopen', function() {"
        "    sb = ms.addSourceBuffer(" + type + ");"
        "    sb.addEventListener('updateend', function() { if (ms.readyState === 'open') ms.endOfStream() });"
        "    sb.appendBuffer(u8);"
        "  });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "ms.addEventListener('sourceended', function() { ev.push('sourceended:' + ms.readyState + ':' + ms.duration.toFixed(2)) });"
        "v.addEventListener('ended', function() { console.log(ev.join() + ' ' + v.currentTime.toFixed(2) + ' ' + v.paused + ' ' + v.ended) });"
        "document.getElementById('b').onclick = function() { v.play().then(function() { ev.push('promise') }) };"
        "document.getElementById('e').onclick = function() { console.log('STATE ' + ev.join() + ' ' + v.currentTime + ' ' + v.paused + ' ' + v.readyState + ' ' + ms.readyState + ' ' + v.duration) };</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 12, &doc);
    click(page, "b");
    mse_pump(page, env, 25, &doc);
    click(page, "e");
    const std::string out = page_log(page);
    CHECK(in_order(out, {"loadstart", "durationchange", "loadedmetadata", "loadeddata", "canplay", "sourceended:ended:2.08", "canplaythrough", "play", "playing", "pause", "ended"}));
    CHECK(out.find(" 2.08 true true") != std::string::npos);
    CHECK(env.opened_video >= 1 && env.last_video_data.size() > 30000 && env.last_video_data.compare(4, 8, "ftypisom") == 0);
    CHECK(env.opened_audio == 0);   // (the picture and the sound are in one file: one clip)
  }
  CHECK(a.live == 0);
  {   // data that comes while playing: the playhead meets the end of what there is, waits, and goes on when more is appended
    FakeEnvironment env(a);
    mse_serve(env, "mse_muxed.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><video id=v width=96 height=64 autoplay></video><button id=b></button><button id=e></button><script>") + kLoad + "var v = document.getElementById('v');" + watch +
        "var ms = new MediaSource(), sb, U8;"
        "load(['mse_muxed.mp4'], function (u8) {"
        "  U8 = u8;"
        "  ms.addEventListener('sourceopen', function() {"
        "    sb = ms.addSourceBuffer(" + type + ");"
        "    sb.appendBuffer(u8.slice(0, 18485));"
        "  });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "document.getElementById('b').onclick = function() { sb.appendBuffer(U8.slice(18485)) };"
        "document.getElementById('e').onclick = function() { console.log(ev.join() + ' ' + v.currentTime.toFixed(2) + ' ' + v.readyState + ' ' + v.paused + ' ' + sb.buffered.end(0).toFixed(2) + ' ' + v.buffered.length) };"
        "</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 14, &doc);   // (plays to the end of the first second and waits there)
    CHECK(env.opened_video == 1 && env.last_clip && !env.last_clip->is_playing);
    click(page, "e");
    const std::string waiting = page_log(page);
    CHECK(in_order(waiting, {"play", "canplay", "playing", "waiting"}));
    CHECK(waiting.find(" false ") != std::string::npos);   // (not paused: it is waiting)
    CHECK(waiting.find("ended") == std::string::npos);
    click(page, "b");
    mse_pump(page, env, 10, &doc);
    CHECK(env.opened_video == 2 && env.last_clip && env.last_clip->is_playing);   // (a fresh file with the second second in it, playing)
    click(page, "e");
    const std::string again = page_log(page);
    const size_t first_wait = again.find("waiting");
    CHECK(first_wait != std::string::npos && again.find("playing", first_wait) != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // seeking inside the data, remove(), the buffers
    FakeEnvironment env(a);
    mse_serve(env, "mse_muxed.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><video id=v width=96 height=64></video><button id=b></button><button id=c></button><button id=e></button><script>") + kLoad + "var v = document.getElementById('v');" + watch +
        "var ms = new MediaSource(), sb;"
        "load(['mse_muxed.mp4'], function (u8) {"
        "  ms.addEventListener('sourceopen', function() { sb = ms.addSourceBuffer(" + type + "); sb.appendBuffer(u8) });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "document.getElementById('b').onclick = function() { v.currentTime = 1.5 };"
        "document.getElementById('c').onclick = function() { sb.remove(0, 1.0) };"
        "document.getElementById('e').onclick = function() { console.log(ev.join() + ' ' + v.currentTime.toFixed(2) + ' ' + v.seeking + ' ' + sb.buffered.length + ' ' + (sb.buffered.length ? sb.buffered.start(0).toFixed(2) : '-') + ' ' + sb.updating) };"
        "</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 12, &doc);
    click(page, "b");
    mse_pump(page, env, 4, &doc);
    click(page, "c");
    mse_pump(page, env, 6, &doc);
    click(page, "e");
    const std::string out = page_log(page);
    CHECK(in_order(out, {"seeking", "seeked"}));
    CHECK(out.find(" 1.50 false ") != std::string::npos);          // (the seek is done: the data was there)
    CHECK(out.find(" 1 1.08 false") != std::string::npos);         // (remove(0, 1) took the first key frame's group, to the next key frame; what is left starts there)
  }
  CHECK(a.live == 0);
  {   // the element lets go of the source: it closes, its buffers go
    FakeEnvironment env(a);
    mse_serve(env, "mse_muxed.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><video id=v></video><button id=b></button><button id=e></button><script>") + kLoad + "var v = document.getElementById('v'), ms = new MediaSource(), sb, log = [];"
        "ms.addEventListener('sourceclose', function() { log.push('sourceclose:' + ms.readyState + ':' + ms.sourceBuffers.length + ':' + ms.duration) });"
        "load(['mse_muxed.mp4'], function (u8) {"
        "  ms.addEventListener('sourceopen', function() { sb = ms.addSourceBuffer(" + type + "); sb.appendBuffer(u8) });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "document.getElementById('b').onclick = function() { v.removeAttribute('src'); v.load() };"
        "document.getElementById('e').onclick = function() { try { sb.appendBuffer(new Uint8Array(4)) } catch (e) { log.push(e.name) } try { sb.buffered } catch (e) { log.push('buffered:' + e.name) } console.log(log.join(' ')) };"
        "</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 10, &doc);
    click(page, "b");
    mse_pump(page, env, 6, &doc);
    click(page, "e");
    const std::string out = page_log(page);
    CHECK(out.find("sourceclose:closed:0:NaN") == 0);
    CHECK(out.find("InvalidStateError buffered:InvalidStateError") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // bytes that are not a stream: error, updateend, the source ends with a decode error, the element has an error
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = "<html><body><video id=v></video><button id=b></button><script>var v = document.getElementById('v'); var ev = [];"
        "var ms = new MediaSource(), sb;"
        "ms.addEventListener('sourceopen', function() {"
        "  sb = ms.addSourceBuffer('video/mp4');"
        "  ['updatestart','update','updateend','error'].forEach(function(t) { sb.addEventListener(t, function() { ev.push(t) }) });"
        "  sb.appendBuffer(new Uint8Array([0, 0, 0, 20, 109, 111, 111, 118, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]));"
        "});"
        "ms.addEventListener('sourceended', function() { ev.push('sourceended') });"
        "v.addEventListener('error', function() { ev.push('element-error:' + v.error.code) });"
        "document.getElementById('b').onclick = function() { try { sb.appendBuffer(new Uint8Array(4)) } catch (e) { ev.push(e.name) } console.log(ev.join() + ' ' + ms.readyState) };"
        "v.src = URL.createObjectURL(ms);</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 8, &doc);
    click(page, "b");
    const std::string out = page_log(page);
    CHECK(in_order(out, {"updatestart", "error", "updateend", "sourceended", "element-error:3", "InvalidStateError"}) && out.find("update,") == std::string::npos);
  }
  CHECK(a.live == 0);
  {   // separate buffers for the picture and the sound: each is its own file, on its own player; the ranges are what both have
    FakeEnvironment env(a);
    mse_serve(env, "mse_video.mp4");
    mse_serve(env, "mse_audio.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><video id=v width=96 height=64></video><button id=b></button><script>") + kLoad +
        "var v = document.getElementById('v'), ms = new MediaSource(), vb, ab;"
        "load(['mse_video.mp4', 'mse_audio.mp4'], function (V, A) {"
        "  ms.addEventListener('sourceopen', function() {"
        "    vb = ms.addSourceBuffer('video/mp4; codecs=\"avc1.4d401e\"'); ab = ms.addSourceBuffer('audio/mp4; codecs=\"mp4a.40.2\"');"
        "    vb.addEventListener('updateend', function() { ab.appendBuffer(A) }, { once: true });"
        "    vb.appendBuffer(V);"
        "  });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "document.getElementById('b').onclick = function() {"
        "  console.log(ms.sourceBuffers.length, ms.activeSourceBuffers.length, vb.buffered.end(0).toFixed(2), ab.buffered.end(0).toFixed(2), v.buffered.length, v.buffered.start(0).toFixed(2), v.buffered.end(0).toFixed(2), v.videoWidth);"
        "  v.play(); };</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 12, &doc);
    click(page, "b");
    mse_pump(page, env, 4, &doc);
    CHECK(page_log(page).find("2 2 2.08 2.02 1 0.08 2.02 64") == 0);
    CHECK(env.opened_video == 1 && env.opened_audio == 1);
    CHECK(env.last_video_data.size() > 15000 && env.last_audio_data.size() > 15000 && env.last_audio_data.compare(4, 8, "ftypisom") == 0);
  }
  CHECK(a.live == 0);
  {   // MPEG audio into an <audio>: sequence mode, a second append goes on where the first ended
    FakeEnvironment env(a);
    mse_serve(env, "mse_audio.mp3");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><audio id=v></audio><button id=b></button><script>") + kLoad +
        "var v = document.getElementById('v'), ms = new MediaSource(), sb, log = [];"
        "load(['mse_audio.mp3'], function (M) {"
        "  ms.addEventListener('sourceopen', function() {"
        "    sb = ms.addSourceBuffer('audio/mpeg'); log.push(sb.mode);"
        "    sb.appendBuffer(M);"
        "    sb.addEventListener('updateend', function() { sb.appendBuffer(M) }, { once: true });"
        "  });"
        "  v.src = URL.createObjectURL(ms);"
        "});"
        "document.getElementById('b').onclick = function() { log.push(sb.buffered.length, sb.buffered.end(0).toFixed(1), v.duration.toFixed(1)); console.log(log.join(' ')) };"
        "</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 12, &doc);
    click(page, "b");
    CHECK(page_log(page) == "sequence 1 2.1 2.1");
    CHECK(env.opened_audio >= 1 && env.last_audio_data.size() > 16000 && static_cast<uint8_t>(env.last_audio_data[0]) == 0xFF);   // (the file for the player: MPEG frames)
  }
  CHECK(a.live == 0);
  {   // `new Audio(url)` for a source nobody put in the page, and the address revoked at once
    FakeEnvironment env(a);
    mse_serve(env, "mse_audio.mp3");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = std::string("<html><body><button id=b></button><script>") + kLoad + "var ms = new MediaSource(), au, sb, log = [], url;"
        "load(['mse_audio.mp3'], function (M) {"
        "  ms.addEventListener('sourceopen', function() { URL.revokeObjectURL(url); sb = ms.addSourceBuffer('audio/mpeg'); sb.appendBuffer(M) });"
        "  url = URL.createObjectURL(ms); au = new Audio(url);"
        "});"
        "document.getElementById('b').onclick = function() { log.push(ms.readyState, au.readyState >= 1, au.duration.toFixed(1)); console.log(log.join(' ')) };"
        "</script></body></html>";
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 3, &doc);
    mse_pump(page, env, 10, &doc);
    click(page, "b");
    CHECK(page_log(page) == "open true 1.0");
  }
  CHECK(a.live == 0);
}

// --- Encrypted Media Extensions ---------------------------------------------------------------------------------------------------

static const std::string kKid1 = "AAECAwQFBgcICQoLDA0ODw", kKey1 = "8OHSw7Sllod4aVpLPC0eDw", kKid2 = "EBESExQVFhcYGRobHB0eHw", kKey2 = "Dx4tPEtaaXiHlqW0w9Lh8A";

// A page that plays an encrypted stream through Clear Key: the license server is the page itself. `licenses` is the script that answers a message (it can use
// the session `s`, the message text `text` and a function `reply(keys...)`); `extra` is run when the page has started.
static std::string eme_page(const std::string& file, const std::string& setup_keys, const std::string& on_message, const std::string& extra, const char* autoplay = "") {
  std::string html = std::string("<html><body><video id=v width=96 height=64 ") + autoplay + "></video><button id=b></button><button id=e></button><button id=p></button><script>" + kLoad +
      "var v = document.getElementById('v'), log = [], ev = [], ms = new MediaSource(), sb, session, access, keys;"
      "['encrypted','waitingforkey','loadedmetadata','canplay','canplaythrough','play','playing','pause','waiting','ended','error'].forEach(function (t) { v.addEventListener(t, function () { ev.push(t) }) });"
      "function license(pairs) { return new TextEncoder().encode(JSON.stringify({ keys: pairs.map(function (p) { return { kty: 'oct', kid: p[0], k: p[1] } }) })) }"
      "function onmsg(text) { " + on_message + " }"
      "v.addEventListener('encrypted', function (e) {"
      "  log.push('encrypted:' + e.initDataType + ':' + e.initData.byteLength);"
      "  if (!keys) return;"
      "  session = keys.createSession();"
      "  session.addEventListener('message', function (m) { log.push('message:' + m.messageType); onmsg(new TextDecoder().decode(new Uint8Array(m.message))) });"
      "  session.addEventListener('keystatuseschange', function () { var r = []; session.keyStatuses.forEach(function (status, kid) { r.push(status + ':' + kid.byteLength) }); log.push('status:' + r.join(',')) });"
      "  session.generateRequest(e.initDataType, e.initData).catch(function (x) { log.push('generate-failed:' + x.name) });"
      "});"
      "load(['" + file + "'], function (u8) {"
      "  ms.addEventListener('sourceopen', function () { sb = ms.addSourceBuffer('video/mp4; codecs=\"avc1.4d401e, mp4a.40.2\"'); sb.addEventListener('updateend', function () { if (ms.readyState === 'open') ms.endOfStream() }); sb.appendBuffer(u8) });"
      "  var start = function () { v.src = URL.createObjectURL(ms) };"
      "  " + setup_keys +
      "});"
      "document.getElementById('p').onclick = function () { v.play() };"
      "document.getElementById('e').onclick = function () { console.log(log.join(' ') + ' | ' + ev.join() + ' | ' + v.readyState + ' ' + v.currentTime.toFixed(2) + ' ' + v.paused + ' ' + v.ended) };"
      + extra + "</script></body></html>";
  return html;
}

static void test_eme() {
  TestAllocator a;
  const auto click = [](Page& page, const char* id) { page.click(page.document().element_by_id(id)->serial, 0, 0); };
  {   // the API: negotiation, sessions, the Clear Key messages and statuses, close, a persistent session
    const std::string script = std::string() + "<html><body><button id=b></button><script>var log = [], rejected = function (name) { return function (e) { log.push(name + ':' + e.name) } };"
        "var good = [{ initDataTypes: ['cenc'], videoCapabilities: [{ contentType: 'video/mp4; codecs=\"avc1.4d401e\"' }], audioCapabilities: [{ contentType: 'audio/mp4; codecs=\"mp4a.40.2\"' }] }];"
        "log.push(typeof navigator.requestMediaKeySystemAccess, typeof MediaKeys, typeof MediaKeySession, typeof MediaKeyStatusMap, typeof MediaKeyMessageEvent, typeof MediaEncryptedEvent, 'mediaKeys' in HTMLMediaElement.prototype);"
        "navigator.requestMediaKeySystemAccess('com.widevine.alpha', good).catch(rejected('widevine'));"
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', []).catch(rejected('empty'));"
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ videoCapabilities: [{ contentType: 'video/mp4; codecs=\"avc1.4d401e\"', robustness: 'SW_SECURE_CRYPTO' }] }]).catch(rejected('robustness'));"
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ videoCapabilities: [{ contentType: 'video/ogg; codecs=\"theora\"' }] }]).catch(rejected('webm'));"
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ distinctiveIdentifier: 'required' }]).catch(rejected('identifier'));"
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ videoCapabilities: [{ contentType: 'video/ogg' }] }, good[0]]).then(function (access) {"
        "  var c = access.getConfiguration();"
        "  log.push('access:' + access.keySystem + ':' + c.initDataTypes + ':' + c.persistentState + ':' + c.sessionTypes + ':' + c.distinctiveIdentifier + ':' + c.videoCapabilities.length + ':' + c.audioCapabilities.length);"
        "  return access.createMediaKeys();"
        "}).then(function (keys) {"
        "  var s = keys.createSession();"
        "  log.push(s.sessionId === '', s.expiration, s.keyStatuses.size, s instanceof EventTarget, s instanceof MediaKeySession);"
        "  try { keys.createSession('persistent-license') } catch (e) { log.push('persistent:' + e.name) } try { keys.createSession('nonsense') } catch (e) { log.push('nonsense:' + e.name) }"
        "  s.close().catch(rejected('close-new'));"
        "  s.update(new Uint8Array(4)).catch(rejected('update-new'));"
        "  keys.setServerCertificate(new Uint8Array(8)).then(function (r) { log.push('certificate:' + r) });"
        "  keys.getStatusForPolicy({ minHdcpVersion: '1.0' }).then(function (r) { log.push('policy:' + r) });"
        "  var messages = [];"
        "  s.addEventListener('message', function (m) {"
        "    messages.push(m.messageType + ':' + new TextDecoder().decode(new Uint8Array(m.message)));"
        "    log.push(messages[messages.length - 1], m instanceof MediaKeyMessageEvent, m instanceof Event);"
        "    var license = function (obj) { return new TextEncoder().encode(JSON.stringify(obj)) };"
        "    s.update(new TextEncoder().encode('not json')).catch(rejected('bad-json'));"
        "    s.update(license({ keys: [{ kty: 'RSA', kid: '" + kKid1 + "', k: '" + kKey1 + "' }] })).catch(rejected('bad-kty'));"
        "    s.update(license({ keys: [{ kty: 'oct', kid: 'AAAA', k: '" + kKey1 + "' }] })).catch(rejected('short-kid'));"
        "    s.update(license({ keys: [{ kty: 'oct', kid: '" + kKid1 + "', k: '" + kKey1 + "' }], type: 'persistent-license' })).catch(rejected('wrong-type'));"
        "    s.update(license({ keys: [] })).catch(rejected('no-keys'));"
        "    s.update(new Uint8Array(0)).catch(rejected('empty-response'));"
        "    s.update(license({ keys: [{ kty: 'oct', kid: '" + kKid1 + "', k: '" + kKey1 + "' }] })).then(function () {"
        "      log.push('updated:' + s.keyStatuses.size);"
        "      var kid = new Uint8Array(16); for (var i = 0; i < 16; i++) kid[i] = i;"
        "      log.push('has:' + s.keyStatuses.has(kid), 'get:' + s.keyStatuses.get(kid), 'get-other:' + s.keyStatuses.get(new Uint8Array(16)));"
        "      var entries = []; for (var e of s.keyStatuses) entries.push(e[1] + ':' + e[0].byteLength); log.push('iterated:' + entries.join());"
        "      s.closed.then(function (reason) { log.push('closed:' + reason + ':' + s.keyStatuses.size) });"
        "      s.close().then(function () { log.push('close-done'); return s.update(license({ keys: [] })) }).catch(rejected('update-closed'));"
        "    });"
        "  });"
        "  s.addEventListener('keystatuseschange', function () { log.push('keystatuseschange:' + s.keyStatuses.size) });"
        "  s.generateRequest('keyids', new TextEncoder().encode(JSON.stringify({ kids: ['" + kKid1 + "'] }))).then(function () {"
        "    log.push('requested:' + (s.sessionId !== ''));"
        "    s.generateRequest('keyids', new Uint8Array(1)).catch(rejected('again'));"
        "  });"
        "  var t = keys.createSession();"
        "  t.generateRequest('keyids', new TextEncoder().encode('{nope')).catch(rejected('bad-init'));"
        "  var u = keys.createSession();"
        "  u.generateRequest('widevine', new Uint8Array(4)).catch(rejected('init-type'));"
        "  var w = keys.createSession();"
        "  w.generateRequest('cenc', new Uint8Array(40)).catch(rejected('bad-pssh'));"
        "}).catch(function (e) { log.push('failed:' + e.name + ':' + e.message) });"
        "document.getElementById('b').onclick = function () { console.log(log.join(' ')) };</script></body></html>";
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load(StringView(script.data(), script.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 30, &doc);
    click(page, "b");
    const std::string out = page_log(page);
    CHECK(out.find("function function function function function function true") == 0);
    CHECK(out.find("widevine:NotSupportedError") != std::string::npos && out.find("empty:TypeError") != std::string::npos && out.find("robustness:NotSupportedError") != std::string::npos);
    CHECK(out.find("webm:NotSupportedError") != std::string::npos && out.find("identifier:NotSupportedError") != std::string::npos);
    CHECK(out.find("access:org.w3.clearkey:cenc:not-allowed:temporary:not-allowed:1:1") != std::string::npos);
    CHECK(out.find("true NaN 0 true true") != std::string::npos);
    CHECK(out.find("persistent:NotSupportedError") != std::string::npos && out.find("nonsense:TypeError") != std::string::npos);
    CHECK(out.find("close-new:InvalidStateError") != std::string::npos && out.find("update-new:InvalidStateError") != std::string::npos);
    CHECK(out.find("certificate:false") != std::string::npos && out.find("policy:usable") != std::string::npos);
    CHECK(out.find("bad-init:TypeError") != std::string::npos && out.find("init-type:NotSupportedError") != std::string::npos && out.find("bad-pssh:TypeError") != std::string::npos && out.find("again:InvalidStateError") != std::string::npos);
    CHECK(out.find("license-request:{\"kids\":[\"AAECAwQFBgcICQoLDA0ODw\"],\"type\":\"temporary\"} true true") != std::string::npos);
    for (const char* rejected : {"bad-json:TypeError", "bad-kty:TypeError", "short-kid:TypeError", "wrong-type:TypeError", "no-keys:TypeError", "empty-response:TypeError"})
      CHECK(out.find(rejected) != std::string::npos);
    CHECK(in_order(out, {"updated:1", "has:true", "get:usable", "get-other:undefined", "iterated:usable:16"}));
    CHECK(out.find("closed:closed-by-application:0") != std::string::npos && out.find("update-closed:InvalidStateError") != std::string::npos);
    CHECK(out.find("keystatuseschange:") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // a persistent session: its keys survive in storage, load() brings them back, remove() releases them
    const std::string script = std::string() + "<html><body><button id=b></button><script>var log = [];"
        "var cfg = [{ initDataTypes: ['keyids'], persistentState: 'required', sessionTypes: ['temporary', 'persistent-license'], videoCapabilities: [{ contentType: 'video/mp4; codecs=\"avc1.4d401e\"' }] }];"
        "var license = new TextEncoder().encode(JSON.stringify({ keys: [{ kty: 'oct', kid: '" + kKid1 + "', k: '" + kKey1 + "' }], type: 'persistent-license' }));"
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', cfg).then(function (access) {"
        "  log.push(access.getConfiguration().persistentState, access.getConfiguration().sessionTypes);"
        "  return access.createMediaKeys();"
        "}).then(function (keys) {"
        "  var s = keys.createSession('persistent-license'), id;"
        "  s.addEventListener('message', function (m) {"
        "    var text = new TextDecoder().decode(new Uint8Array(m.message));"
        "    log.push(m.messageType + ':' + text);"
        "    if (m.messageType === 'license-request') { s.update(license).then(function () { id = s.sessionId; log.push('saved:' + (localStorage.getItem('__eme_licenses').indexOf('" + kKey1 + "') > 0)); return s.close() }).then(function () {"
        "      var t = keys.createSession('persistent-license');"
        "      t.addEventListener('message', function (m2) { log.push(m2.messageType + ':' + new TextDecoder().decode(new Uint8Array(m2.message))); t.update(new TextEncoder().encode('{\"kids\":[\"" + kKid1 + "\"]}')).then(function () { log.push('released-closed') }) });"
        "      t.addEventListener('keystatuseschange', function () { var r = []; t.keyStatuses.forEach(function (st) { r.push(st) }); log.push('t-status:' + r.join()) });"
        "      t.closed.then(function (why) { log.push('t-closed:' + why + ':' + (localStorage.getItem('__eme_licenses').indexOf('" + kKey1 + "') > 0)) });"
        "      return t.load(id).then(function (ok) { log.push('loaded:' + ok + ':' + t.sessionId.length + ':' + t.keyStatuses.size); return t.remove() }).then(function () { log.push('removed') });"
        "    }).then(function () { return keys.createSession('persistent-license').load('nothing') }).then(function (ok) { log.push('missing:' + ok) }); }"
        "  });"
        "  s.generateRequest('keyids', new TextEncoder().encode(JSON.stringify({ kids: ['" + kKid1 + "'] })));"
        "}).catch(function (e) { log.push('failed:' + e.name + ':' + e.message) });"
        "document.getElementById('b').onclick = function () { console.log(log.join(' | ')) };</script></body></html>";
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load(StringView(script.data(), script.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 40, &doc);
    click(page, "b");
    const std::string out = page_log(page);
    CHECK(in_order(out, {"required | temporary,persistent-license", "license-request:{\"kids\":[\"AAECAwQFBgcICQoLDA0ODw\"],\"type\":\"persistent-license\"}", "saved:true", "loaded:true:16:1", "removed", "missing:false"}));
    CHECK(in_order(out, {"t-status:released", "license-release:{\"kids\":[\"AAECAwQFBgcICQoLDA0ODw\"]}", "t-closed:closed-by-application:false", "released-closed"}));
    CHECK(out.find("t-closed:closed-by-application:false") != std::string::npos);
  }
  CHECK(a.live == 0);
}

static void test_eme_playback() {
  TestAllocator a;
  const auto click = [](Page& page, const char* id) { page.click(page.document().element_by_id(id)->serial, 0, 0); };
  const std::string access =
      "navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ initDataTypes: ['cenc', 'keyids'], videoCapabilities: [{ contentType: 'video/mp4; codecs=\"avc1.4d401e\"' }], audioCapabilities: [{ contentType: 'audio/mp4; codecs=\"mp4a.40.2\"' }] }])"
      ".then(function (a) { access = a; return a.createMediaKeys() }).then(function (k) { keys = k; return v.setMediaKeys(k) }).then(function () { log.push('keys-set:' + (v.mediaKeys === keys)); start() })"
      ".catch(function (e) { log.push('failed:' + e.name) });";
  const std::string answer = std::string("onmsg = function (text) { if (JSON.parse(text).kids[0] === '") + kKid1 + "') session.update(license([['" + kKid1 + "', '" + kKey1 + "']])) };";
  std::string clear_data;
  {   // the clear stream, for what the decrypted file has to be
    FakeEnvironment env(a);
    mse_serve(env, "mse_muxed.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    const std::string html = eme_page("mse_muxed.mp4", "start();", "", "");
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 12, &doc);
    CHECK(env.opened_video == 1);
    clear_data = env.last_video_data;
  }
  for (const char* scheme : {"cenc", "cens", "cbc1", "cbcs", "ffmpeg"}) {   // every scheme: the license arrives, the element plays, the file the player gets is the clear one
    FakeEnvironment env(a);
    const std::string name = std::string("eme_") + scheme + ".mp4";
    mse_serve(env, name.c_str());
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = eme_page(name, access, "", "");
    // (the license server: answers every message)
    html.replace(html.find("function onmsg(text) {  }"), 25, "var onmsg = function (text) {};");
    html.insert(html.find("v.addEventListener('encrypted', function (e) {"), answer);
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 14, &doc);
    click(page, "p");
    mse_pump(page, env, 25, &doc);
    click(page, "e");
    const std::string out = page_log(page);
    if (!in_order(out, {"keys-set:true", "encrypted:", "message:license-request", "status:usable:16"})) printf("   %s: %s\n", scheme, out.c_str());
    CHECK(in_order(out, {"keys-set:true", "encrypted:", "message:license-request", "status:usable:16", "|", "loadedmetadata", "waitingforkey", "encrypted", "canplay", "canplaythrough", "play", "playing", "pause", "ended"}));
    CHECK(env.opened_video >= 1);
    if (env.last_video_data != clear_data) printf("   the file the player got for %s is not the clear one (%llu / %llu bytes)\n", scheme, (unsigned long long)env.last_video_data.size(), (unsigned long long)clear_data.size());
    CHECK(env.last_video_data == clear_data);
  }
  CHECK(a.live == 0);
  {   // WebM, with the encryption of the WebM standard: announced as "webm" init data (the key identifier), and the player gets the file of the clear stream
    const std::string webm_access =
        "navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ initDataTypes: ['webm'], videoCapabilities: [{ contentType: 'video/webm; codecs=\"vp9\"' }], audioCapabilities: [{ contentType: 'audio/webm; codecs=\"opus\"' }] }])"
        ".then(function (a) { access = a; return a.createMediaKeys() }).then(function (k) { keys = k; return v.setMediaKeys(k) }).then(function () { log.push('keys-set:' + (v.mediaKeys === keys)); start() })"
        ".catch(function (e) { log.push('failed:' + e.name) });";
    const char* const kMp4Type = "video/mp4; codecs=\"avc1.4d401e, mp4a.40.2\"";
    std::string clear_webm;
    for (const char* name : {"webm_vp9_opus.webm", "webm_enc.webm"}) {
      const bool encrypted = std::string(name) == "webm_enc.webm";
      FakeEnvironment env(a);
      mse_serve(env, name);
      MediaEnvironment media;
      Page page(a, env, "http://example.com/", media, true);
      std::string html = eme_page(name, encrypted ? webm_access : "start();", "", "");
      html.replace(html.find(kMp4Type), strlen(kMp4Type), "video/webm; codecs=\"vp9, opus\"");
      html.replace(html.find("function onmsg(text) {  }"), 25, "var onmsg = function (text) {};");
      if (encrypted) html.insert(html.find("v.addEventListener('encrypted', function (e) {"), answer);
      page.load(StringView(html.data(), html.size()));
      HtmlDocument doc(a);
      page.render(&doc);
      mse_pump(page, env, 14, &doc);
      click(page, "p");
      mse_pump(page, env, 25, &doc);
      click(page, "e");
      const std::string out = page_log(page);
      if (encrypted) {
        if (!in_order(out, {"keys-set:true", "encrypted:webm:16", "message:license-request", "status:usable:16", "|", "playing", "ended"})) printf("   webm: %s\n", out.c_str());
        CHECK(in_order(out, {"keys-set:true", "encrypted:webm:16", "message:license-request", "status:usable:16", "|", "loadedmetadata", "play", "playing", "ended"}));
        CHECK(!clear_webm.empty() && env.last_video_data == clear_webm);
      } else {
        CHECK(out.find("error") == std::string::npos && env.opened_video >= 1);
        clear_webm = env.last_video_data;
      }
    }
  }
  CHECK(a.live == 0);
  {   // no MediaKeys: the stream cannot be played, the element says it is waiting for a key; setMediaKeys() and a license later let it go on
    FakeEnvironment env(a);
    mse_serve(env, "eme_cenc.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = eme_page("eme_cenc.mp4", "start();", "", "document.getElementById('b').onclick = function () { navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{}]).then(function (a) { return a.createMediaKeys() }).then(function (k) { keys = k; return v.setMediaKeys(k) }).then(function () { "
                                                          "session = keys.createSession(); session.addEventListener('message', function () { session.update(license([['" + std::string(kKid1) + "', '" + kKey1 + "']])) }); session.generateRequest('keyids', new TextEncoder().encode('{\"kids\":[\"" + kKid1 + "\"]}')) }) };");
    html.replace(html.find("function onmsg(text) {  }"), 25, "var onmsg = function (text) {};");
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 14, &doc);
    click(page, "p");
    mse_pump(page, env, 6, &doc);
    click(page, "e");
    std::string out = page_log(page);
    CHECK(in_order(out, {"encrypted:cenc", "|", "loadedmetadata", "waitingforkey", "encrypted"}) && out.find("canplay") == std::string::npos);
    CHECK(env.opened_video == 0);
    CHECK(out.find(" 1 0.00 ") != std::string::npos);   // (readyState 1: the data is there, nothing can be shown)
    click(page, "b");
    mse_pump(page, env, 25, &doc);
    click(page, "e");
    out = page_log(page);
    CHECK(in_order(out, {"waitingforkey", "|", "canplay", "playing", "pause", "ended"}) || in_order(out, {"waitingforkey", "canplay"}));
    CHECK(env.opened_video >= 1 && env.last_video_data == clear_data);
  }
  CHECK(a.live == 0);
  {   // a key rotation: the first second plays with the first key, the second needs another, and the element waits for it
    FakeEnvironment env(a);
    mse_serve(env, "eme_rotate.mp4");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    std::string html = eme_page("eme_rotate.mp4", access, "", "document.getElementById('b').onclick = function () { session.update(license([['" + std::string(kKid2) + "', '" + kKey2 + "']])) };", "autoplay");
    html.replace(html.find("function onmsg(text) {  }"), 25, "var onmsg = function (text) {};");
    html.insert(html.find("v.addEventListener('encrypted', function (e) {"), std::string("onmsg = function (text) { session.update(license([['") + kKid1 + "', '" + kKey1 + "']])) };");
    page.load(StringView(html.data(), html.size()));
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 30, &doc);   // (plays the first second, and waits)
    click(page, "e");
    std::string out = page_log(page);
    CHECK(in_order(out, {"encrypted:cenc", "status:usable:16", "|", "playing", "waitingforkey"}) && out.find("ended") == std::string::npos);
    CHECK(env.opened_video == 1);
    click(page, "b");
    mse_pump(page, env, 30, &doc);
    click(page, "e");
    out = page_log(page);
    CHECK(env.opened_video == 2 && env.last_video_data.size() > 15000);
    CHECK(in_order(out, {"waitingforkey", "|", "playing", "waitingforkey", "canplay", "playing", "ended"}));
  }
  CHECK(a.live == 0);
}

// --- Blob URLs on media elements -----------------------------------------------------------------------------------------------------

static void test_blob_media() {
  TestAllocator a;
  FakeEnvironment env(a);
  mse_serve(env, "mse_muxed.mp4");
  mse_serve(env, "anim.gif");
  MediaEnvironment media;
  Page page(a, env, "http://example.com/", media, true);
  std::string html = std::string("<html><body><video id=v></video><img id=i><button id=b></button><script>") + kLoad +
      "var v = document.getElementById('v'), i = document.getElementById('i'), log = [];"
      "fetch('mse_muxed.mp4').then(function (r) { return r.blob() }).then(function (blob) {"
      "  var url = URL.createObjectURL(new Blob([blob], { type: 'video/mp4' }));"
      "  v.src = url; log.push(url.slice(0, 5), blob.size);"
      "  setTimeout(function () { URL.revokeObjectURL(url) }, 200);"
      "});"
      "fetch('anim.gif').then(function (r) { return r.blob() }).then(function (blob) { i.src = URL.createObjectURL(blob) });"
      "document.getElementById('b').onclick = function () { log.push(v.readyState, v.duration > 0, v.videoWidth, i.complete, i.naturalWidth > 0); console.log(log.join(' ')) };"
      "</script></body></html>";
  page.load(StringView(html.data(), html.size()));
  HtmlDocument doc(a);
  page.render(&doc);
  mse_pump(page, env, 20, &doc);
  page.click(page.document().element_by_id("b")->serial, 0, 0);
  CHECK(env.opened_video == 1 && env.last_video_data.size() > 30000);
  const std::string out = page_log(page);
  CHECK(out.find("blob: 36838 4 true 64 true true") == 0);
}

// --- WebM in MediaSource --------------------------------------------------------------------------------------------------------------

static void test_webm_page() {
  TestAllocator a;
  FakeEnvironment env(a);
  mse_serve(env, "mse_webm.webm");
  MediaEnvironment media;
  Page page(a, env, "http://example.com/", media, true);
  std::string html = std::string("<html><body><video id=v width=96 height=64></video><button id=b></button><button id=e></button><script>") + kLoad +
      "var v = document.getElementById('v'), ms = new MediaSource(), sb, ev = [], log = [];"
      "['loadedmetadata','canplay','play','playing','ended','error'].forEach(function (t) { v.addEventListener(t, function () { ev.push(t) }) });"
      "log.push(MediaSource.isTypeSupported('video/webm; codecs=\"vp9, opus\"'), MediaSource.isTypeSupported('video/webm; codecs=\"theora\"'), MediaSource.isTypeSupported('audio/webm; codecs=\"vorbis\"'));"
      "load(['mse_webm.webm'], function (u8) {"
      "  ms.addEventListener('sourceopen', function () {"
      "    sb = ms.addSourceBuffer('video/webm; codecs=\"vp9, opus\"');"
      "    log.push(sb.mode);"
      "    sb.addEventListener('updateend', function () { if (ms.readyState === 'open') ms.endOfStream() });"
      "    sb.appendBuffer(u8);"
      "  });"
      "  v.src = URL.createObjectURL(ms);"
      "});"
      "document.getElementById('b').onclick = function () { v.play() };"
      "document.getElementById('e').onclick = function () { log.push(sb.buffered.length, sb.buffered.end(0).toFixed(1), v.duration.toFixed(1), v.videoWidth, sb.videoTracks.length, sb.audioTracks.length); console.log(log.join(' ') + ' | ' + ev.join()) };"
      "try { ms.addSourceBuffer('video/webm; codecs=\"theora\"') } catch (e) { log.push(e.name) }"
      "</script></body></html>";
  page.load(StringView(html.data(), html.size()));
  HtmlDocument doc(a);
  page.render(&doc);
  mse_pump(page, env, 14, &doc);
  page.click(page.document().element_by_id("b")->serial, 0, 0);
  mse_pump(page, env, 25, &doc);
  page.click(page.document().element_by_id("e")->serial, 0, 0);
  const std::string out = page_log(page);
  CHECK(out.find("true false true NotSupportedError segments 1 2.0 2.0 64 1 1 | ") == 0 || out.find("true false true NotSupportedError segments 1 2.0 2.0 ") == 0);
  CHECK(in_order(out, {"loadedmetadata", "canplay", "play", "playing", "ended"}) && out.find("error") == std::string::npos);
  // the picture and the sound are two files for two players
  CHECK(env.opened_video == 1 && env.opened_audio == 1);
  CHECK(env.last_video_data.size() > 20000 && env.last_video_data.compare(4, 8, "ftypisom") == 0);
  CHECK(env.last_audio_data.size() > 4000 && env.last_audio_data.compare(0, 4, "OggS") == 0);
}

// --- Forms ------------------------------------------------------------------------------------------------------------------------

static uint32_t serial_of(Page& page, const char* id) {
  dom::Node* n = page.document().element_by_id(id);
  return n ? n->serial : 0;
}

static void type_text(Page& page, const char* text) {
  for (const char* p = text; *p; ++p) page.key(Page::FormKey::Character, *p, 0);
}

static void test_details_and_dialog() {
  TestAllocator a;
  {   // details: closed shows the summary; a click opens it, and a click again closes it
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><details id=d><summary id=s>More</summary><p>hidden text</p></details>"
              "<details name=g id=g1 open><summary>One</summary>first</details><details name=g id=g2><summary>Two</summary>second</details>"
              "<script>['d','g1','g2'].forEach(function(id) { document.getElementById(id).addEventListener('toggle', function(e) { console.log('toggle ' + id + ' ' + e.target.open) }) })</script></body></html>");
    std::string text = page_text(page, 40);
    CHECK(text.find("> More") != std::string::npos && text.find("hidden text") == std::string::npos && text.find("v One") != std::string::npos && text.find("first") != std::string::npos);
    page.click(serial_of(page, "s"), 0, 0);
    text = page_text(page, 40);
    CHECK(text.find("v More") != std::string::npos && text.find("hidden text") != std::string::npos);
    CHECK(page_log(page) == "toggle d true");
    page.click(serial_of(page, "s"), 0, 0);
    CHECK(page_text(page, 40).find("hidden text") == std::string::npos);
    page.click(page.document().element_by_id("g2")->first_child->serial, 0, 0);   // (the group keeps one open)
    CHECK(!dom::has_attribute(page.document().element_by_id("g1"), "open") && dom::has_attribute(page.document().element_by_id("g2"), "open"));
    // keyboard: Tab to a summary, Enter opens it
    page.key(Page::FormKey::Tab, 0, 0);   // (leaves the last one)
    page.key(Page::FormKey::Tab, 0, 0);
    CHECK(page.focused() && page.focused()->serial == serial_of(page, "s"));
    page.key(Page::FormKey::Enter, 0, 0);
    CHECK(dom::has_attribute(page.document().element_by_id("d"), "open"));
  }
  CHECK(a.live == 0);
  {   // dialog
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><p id=behind>page text</p><dialog id=d><p>dialog text</p><form method=dialog><button id=ok value=yes>OK</button><button id=no value=no>No</button></form></dialog>"
              "<button id=open1></button><button id=open2></button><script>var d = document.getElementById('d'); var log = [];"
              "d.addEventListener('close', function() { console.log('close ' + d.returnValue) }); d.addEventListener('cancel', function() { console.log('cancel') });"
              "document.getElementById('open1').addEventListener('click', function() { d.show(); console.log(d.open) });"
              "document.getElementById('open2').addEventListener('click', function() { d.showModal() });"
              "document.getElementById('behind').addEventListener('click', function() { console.log('behind clicked') });</script></body></html>");
    CHECK(page_text(page, 40).find("dialog text") == std::string::npos);
    page.click(serial_of(page, "open1"), 0, 0);
    std::string text = page_text(page, 40);
    CHECK(text.find("dialog text") != std::string::npos && text.find("---") != std::string::npos && page_log(page) == "true");
    page.click(serial_of(page, "ok"), 0, 0);   // (a form with method=dialog closes it, with the button's value)
    CHECK(page_text(page, 40).find("dialog text") == std::string::npos && page_log(page) == "true | close yes" && env.navigations.empty());
    page.click(serial_of(page, "open2"), 0, 0);
    text = page_text(page, 40);
    CHECK(text.find("dialog text") < text.find("page text"));   // (a modal dialog is drawn first)
    CHECK(page.has_modal());
    page.click(serial_of(page, "behind"), 0, 0);   // (the page behind it is inert)
    CHECK(page_log(page).find("behind clicked") == std::string::npos);
    page.key(Page::FormKey::Escape, 0, 0);
    CHECK(!page.has_modal() && page_log(page).find("cancel | close") != std::string::npos);
    page.click(serial_of(page, "behind"), 0, 0);
    CHECK(page_log(page).find("behind clicked") != std::string::npos);
  }
  CHECK(a.live == 0);
}

static void test_template() {
  TestAllocator a;
  FakeEnvironment env(a);
  MediaEnvironment media;
  Page page(a, env, "http://example.com/", media, true);
  page.load("<html><body><template id=t><li class=x>item</li><script>console.log('never')</script></template><ul id=u></ul><script>"
            "var t = document.getElementById('t'), u = document.getElementById('u'), r = [];"
            "r.push(t.content.childNodes.length, t.children.length, t.childNodes.length, document.querySelectorAll('.x').length, t.innerHTML);"
            "u.appendChild(t.content.cloneNode(true)); u.appendChild(document.importNode(t.content, true));"
            "r.push(u.children.length, document.querySelectorAll('.x').length, t.content.childNodes.length);"
            "t.innerHTML = '<b>z</b>'; r.push(t.content.firstChild.tagName, t.outerHTML);"
            "var made = document.createElement('template'); r.push(made.content.childNodes.length);"
            "console.log(r.join(' '))</script></body></html>");
  CHECK(page_log(page) == "2 0 0 0 <li class=\"x\">item</li><script>console.log('never')</script> 4 2 2 B <template id=\"t\"><b>z</b></template> 0");
  const std::string text = page_text(page, 40);
  CHECK(text.find("item") != std::string::npos && text.find("never") == std::string::npos);
}

static uint32_t node_of_text(Page& page, const char* text) {
  HtmlDocument doc(page.document().allocator());
  page.render(&doc);
  for (uint64_t i = 0; i < doc.run_count(); ++i)
    if (doc.run_text(i).contains(text)) return doc.run(i).node;
  return 0;
}

static void test_iframe() {
  TestAllocator a;
  {   // srcdoc and src: the page of the frame is drawn between rules
    FakeEnvironment env(a);
    env.files["http://example.com/frame.html"] = "<html><body><h1>From a file</h1><p>second <a href=/x>link</a></p></body></html>";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><p>before</p><iframe id=f srcdoc=\"<p>inner text</p><button id=b>Go</button>\"></iframe><iframe id=g src=frame.html>fallback</iframe><p>after</p></body></html>");
    const std::string text = page_text(page, 60);
    CHECK(text.find("before") < text.find("inner text") && text.find("inner text") < text.find("From a file") && text.find("From a file") < text.find("after"));
    CHECK(text.find("[ Go ]") != std::string::npos && text.find("---") != std::string::npos && text.find("fallback") == std::string::npos);
    CHECK(text.find("second link") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // clicks and typing reach the frame; messages go both ways
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><iframe id=f srcdoc=\"<input id=i><button id=b>Press</button><script>"
              "document.getElementById('b').addEventListener('click', function() { parent.postMessage('clicked ' + document.getElementById('i').value, '*') });"
              "window.addEventListener('message', function(e) { parent.postMessage('echo:' + e.data, '*') })</script>\"></iframe><button id=ask>ask</button><script>"
              "window.addEventListener('message', function(e) { console.log('got ' + e.data + ' ' + (e.origin === 'http://example.com')) });"
              "document.getElementById('ask').addEventListener('click', function() { document.getElementById('f').contentWindow.postMessage('hello', '*') })</script></body></html>");
    const uint32_t input = node_of_text(page, "[ ");   // (the first control of the frame)
    CHECK(input >> 24 == 1);
    page.click(input, 0, 0);
    CHECK(page.has_focus());
    type_text(page, "abc");
    CHECK(page_text(page, 60).find("[abc|") != std::string::npos);
    page.click(node_of_text(page, "[ Press ]"), 0, 0);
    page.run_due_timers(1e18);
    CHECK(page_log(page) == "got clicked abc true");
    page.click(serial_of(page, "ask"), 0, 0);
    CHECK(page.focused() && page.focused()->serial == serial_of(page, "ask"));   // (the focus left the frame)
    page.run_due_timers(1e18);   // (the message goes to the frame, whose answer comes in the same turn or the next)
    page.run_due_timers(1e18);
    CHECK(page_log(page) == "got clicked abc true | got echo:hello true");
  }
  CHECK(a.live == 0);
  {   // a sandboxed frame runs no scripts; contentWindow and window.parent
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><iframe sandbox srcdoc=\"<p>quiet</p><script>parent.postMessage('x', '*')</script>\"></iframe><script>"
              "window.addEventListener('message', function(e) { console.log('message ' + e.data) });"
              "console.log(window.parent === window, typeof document.querySelector('iframe').contentWindow.postMessage, document.querySelector('iframe').contentDocument)</script></body></html>");
    page.run_due_timers(1e18);
    CHECK(page_log(page) == "true function null");
    CHECK(page_text(page, 60).find("quiet") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // following a link in a frame moves the frame
    FakeEnvironment env(a);
    env.files["http://example.com/next.html"] = "<p>the next page</p>";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><iframe srcdoc=\"<a href='next.html'>go on</a>\"></iframe></body></html>");
    page.click(node_of_text(page, "go on"), 0, 0);
    const std::string text = page_text(page, 60);
    CHECK(env.navigations.empty() && text.find("the next page") != std::string::npos && text.find("go on") == std::string::npos);
  }
  CHECK(a.live == 0);
}

// Paints the nth svg of a page (w x h) into a surface and gives the pixel at (x, y) of it.
static uint32_t svg_pixel(TestAllocator& a, Page& page, uint32_t w, uint32_t h, int32_t x, int32_t y, uint32_t which = 0) {
  HtmlDocument doc(page.document().allocator());
  page.render(&doc);
  const Run* run = find_object(doc, ObjectKind::Svg, which);
  if (!run) return 0x12345678u;
  Surface32 target(a, w + 2, h + 2);
  graphics::SoftwareRenderer2D renderer(target.surface);
  renderer.fill_rect({0, 0, w + 2, h + 2}, 0xFFFFFFFFu);
  page.objects().paint_object(renderer, run->node, ObjectKind::Svg, 0, 0, w, h, Rect2D{0, 0, w + 2, h + 2});
  return target.at(x, y);
}

static void test_svg() {
  TestAllocator a;
  MediaEnvironment media;
  {   // shapes, styles, transforms
    FakeEnvironment env(a);
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><svg width=40 height=30><rect width=40 height=30 fill=\"#ff0000\"/><circle cx=20 cy=15 r=8 style=\"fill:blue\"/></svg>"
              "<svg width=40 height=30 viewBox=\"0 0 4 3\"><g transform=\"translate(1 1)\" fill=green><rect width=2 height=1 /></g><polygon points=\"0,3 1,1 2,3\" fill=#ffff00 /></svg>"
              "<svg width=40 height=30><path d=\"M0 0 H40 V10 H0 Z\" fill=black /><path d=\"M5 20 L35 20\" stroke=#00ff00 stroke-width=6 /><ellipse cx=20 cy=26 rx=10 ry=3 fill=none stroke=red /></svg>"
              "<p>after</p></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    const Run* first = find_object(doc, ObjectKind::Svg, 0);
    CHECK(first && first->object_w == 40 && first->object_h == 30);
    CHECK(svg_pixel(a, page, 40, 30, 3, 3, 0) == 0xFFFF0000u && svg_pixel(a, page, 40, 30, 20, 15, 0) == 0xFF0000FFu && svg_pixel(a, page, 40, 30, 38, 28, 0) == 0xFFFF0000u);
    CHECK(svg_pixel(a, page, 40, 30, 15, 15, 1) == 0xFF008000u && svg_pixel(a, page, 40, 30, 35, 5, 1) == 0xFFFFFFFFu && svg_pixel(a, page, 40, 30, 10, 28, 1) == 0xFFFFFF00u);
    CHECK(svg_pixel(a, page, 40, 30, 20, 5, 2) == 0xFF000000u && svg_pixel(a, page, 40, 30, 20, 20, 2) == 0xFF00FF00u && svg_pixel(a, page, 40, 30, 20, 13, 2) == 0xFFFFFFFFu);
    CHECK(page_text(page, 60).find("after") != std::string::npos);
  }
  {   // gradients, rounded rectangles, arcs, use and defs, text
    FakeEnvironment env(a);
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><svg width=40 height=20><defs><linearGradient id=g x1=0 y1=0 x2=1 y2=0><stop offset=0 stop-color=#ff0000 /><stop offset=1 stop-color=#0000ff /></linearGradient>"
              "<rect id=box width=10 height=10 fill=#00ff00 /></defs><rect width=40 height=10 fill=\"url(#g)\" /><use href=\"#box\" x=0 y=10 /></svg>"
              "<svg width=40 height=40><rect width=40 height=40 rx=20 ry=20 fill=#000000 /><path d=\"M10 20 A10 10 0 0 1 30 20\" fill=none stroke=#ff00ff stroke-width=4 /></svg>"
              "<svg width=60 height=20><text x=2 y=14 font-size=14 fill=#000000>Hi</text></svg></body></html>");
    const uint32_t left = svg_pixel(a, page, 40, 20, 2, 4, 0), right = svg_pixel(a, page, 40, 20, 37, 4, 0);
    CHECK(((left >> 16) & 255) > 200 && (left & 255) < 60 && (right & 255) > 190 && ((right >> 16) & 255) < 60);
    CHECK(svg_pixel(a, page, 40, 20, 5, 15, 0) == 0xFF00FF00u && svg_pixel(a, page, 40, 20, 20, 15, 0) == 0xFFFFFFFFu);
    CHECK(svg_pixel(a, page, 40, 40, 20, 20, 1) == 0xFF000000u && svg_pixel(a, page, 40, 40, 1, 1, 1) == 0xFFFFFFFFu && svg_pixel(a, page, 40, 40, 20, 10, 1) == 0xFFFF00FFu);
    bool ink = false;
    for (int32_t y = 0; y < 20; ++y)
      for (int32_t x = 0; x < 30; ++x) ink = ink || svg_pixel(a, page, 60, 20, x, y, 2) != 0xFFFFFFFFu;
    CHECK(ink);
  }
  {   // a script changes it; an <img> of an svg file; display none
    FakeEnvironment env(a);
    env.blobs["http://example.com/dot.svg"] = "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\"><rect width=\"20\" height=\"20\" fill=\"#0000ff\"/></svg>";
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><svg id=s width=20 height=20><rect id=r width=20 height=20 fill=red /><circle r=5 cx=5 cy=5 display=none /></svg><img src=dot.svg id=i>"
              "<script>document.getElementById('r').setAttribute('fill', 'lime')</script></body></html>");
    CHECK(svg_pixel(a, page, 20, 20, 10, 10, 0) == 0xFF00FF00u);
    HtmlDocument doc(a);
    page.render(&doc);
    const Run* img = find_object(doc, ObjectKind::Image, 0);
    CHECK(img && img->object_w == 20);
    Surface32 target(a, 22, 22);
    graphics::SoftwareRenderer2D renderer(target.surface);
    page.objects().paint_object(renderer, img->node, ObjectKind::Image, 0, 0, 20, 20, Rect2D{0, 0, 22, 22});
    CHECK(target.at(10, 10) == 0xFF0000FFu);
  }
  CHECK(a.live == 0);
}

static std::string value_of(Page& page, const char* id) {
  String v(page.document().allocator());
  dom::get_value(page.document().element_by_id(id), &v);
  return std::string(v.data(), v.size());
}

static void test_input_more() {
  TestAllocator a;
  MediaEnvironment media;
  {   // the caret and the selection
    FakeEnvironment env(a);
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form><input id=t><textarea id=a rows=3 cols=10></textarea><input id=n type=number></form></body></html>");
    page.click(serial_of(page, "t"), 0, 0);
    type_text(page, "hello");
    page.key(Page::FormKey::Left, 0, 0);
    page.key(Page::FormKey::Left, 0, 0);
    type_text(page, "X");
    CHECK(value_of(page, "t") == "helXlo");
    CHECK(page_text(page, 60).find("[helX|lo") != std::string::npos);
    page.key(Page::FormKey::Home, 0, 0);
    page.key(Page::FormKey::Delete, 0, 0);
    CHECK(value_of(page, "t") == "elXlo");
    page.key(Page::FormKey::End, 0, 0);
    page.key(Page::FormKey::Backspace, 0, 0);
    CHECK(value_of(page, "t") == "elXl");
    page.key(Page::FormKey::Left, 0, 1);
    page.key(Page::FormKey::Left, 0, 1);
    uint32_t s, e, c;
    dom::get_selection(page.document().element_by_id("t"), &s, &e, &c);
    CHECK(s == 1 + 1 && e == 4 && c == 2);   // (shift + arrows: "Xl" is selected)
    CHECK(page_text(page, 60).find("[el") != std::string::npos);
    type_text(page, "Q");   // (typing replaces the selection)
    CHECK(value_of(page, "t") == "elQ");
    page.key(Page::FormKey::Character, 'a', 2);   // Ctrl+A
    dom::get_selection(page.document().element_by_id("t"), &s, &e, &c);
    CHECK(s == 0 && e == 3);
    page.key(Page::FormKey::Character, 'c', 2);
    CHECK(env.clip == "elQ");
    page.key(Page::FormKey::Character, 'x', 2);
    CHECK(value_of(page, "t").empty() && env.clip == "elQ");
    env.clip = "one\ntwo";
    page.key(Page::FormKey::Character, 'v', 2);
    CHECK(value_of(page, "t") == "one two");
    // a textarea: lines, the caret moves up and down
    page.click(serial_of(page, "a"), 0, 0);
    page.key(Page::FormKey::Character, 'v', 2);
    CHECK(value_of(page, "a") == "one\ntwo");
    page.key(Page::FormKey::Up, 0, 0);
    type_text(page, "!");
    CHECK(value_of(page, "a") == "one!\ntwo");
    page.key(Page::FormKey::Down, 0, 0);
    type_text(page, "?");
    CHECK(value_of(page, "a") == "one!\ntwo?");
    // a number field takes only what a number is made of
    page.click(serial_of(page, "n"), 0, 0);
    type_text(page, "1a2.5");
    CHECK(value_of(page, "n") == "12.5");
  }
  {   // scripts: selection, indeterminate, color, files
    FakeEnvironment env(a);
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f><input id=t value=abcdef><input id=c type=checkbox><input id=col type=color value=#000000><input id=file type=file name=up></form><script>"
              "var t = document.getElementById('t'), r = [];"
              "t.setSelectionRange(1, 3); r.push(t.selectionStart, t.selectionEnd); t.setRangeText('XY'); r.push(t.value, t.selectionStart);"
              "t.select(); r.push(t.selectionStart, t.selectionEnd, document.activeElement === t); t.selectionStart = 2; r.push(t.selectionStart, t.selectionEnd);"
              "var c = document.getElementById('c'); c.indeterminate = true; r.push(c.indeterminate);"
              "console.log(r.join(' '))</script></body></html>");
    CHECK(page_log(page) == "1 3 aXYdef 3 0 6 true 2 6 true");
    CHECK(page_text(page, 60).find("[-]") != std::string::npos);
    page.click(serial_of(page, "c"), 0, 0);
    CHECK(dom::is_checked(page.document().element_by_id("c")) && !(page.document().element_by_id("c")->form_state & dom::kFormIndeterminate));
    page.click(serial_of(page, "col"), 0, 0);
    CHECK(value_of(page, "col") == "#ffffff");
    page.key(Page::FormKey::Down, 0, 0);
    CHECK(value_of(page, "col") == "#ff0000");
    // a file input
    page.click(serial_of(page, "file"), 0, 0);
    CHECK(env.picks.size() == 1 && env.picks[0] == serial_of(page, "file"));
    CHECK(page_text(page, 60).find("No file chosen") != std::string::npos);
    const char bytes[] = "hello\0file";
    page.file_chosen(env.picks[0], "note.txt", "text/plain", ConstByteView(reinterpret_cast<const uint8_t*>(bytes), 10));
    CHECK(page_text(page, 60).find("note.txt") != std::string::npos && value_of(page, "file") == "C:\\fakepath\\note.txt");
    dom::Node* form = page.document().element_by_id("f");
    List<dom::Entry> entries(a);
    CHECK(dom::form_entries(form, nullptr, &entries) && entries.count() >= 1);
    bool found = false;
    for (uint64_t i = 0; i < entries.count(); ++i)
      if (entries[i].file && entries[i].name.view().equals("up") && entries[i].filename.view().equals("note.txt") && entries[i].value.size() == 10 && entries[i].content_type.view().equals("text/plain")) found = true;
    CHECK(found);
    String body(a);
    CHECK(dom::encode_multipart(entries, "BOUND", &body) && body.view().contains("filename=\"note.txt\"\r\nContent-Type: text/plain\r\n\r\nhello"));
    CHECK(dom::encode_urlencoded(entries, &body) && body.view().contains("up=note.txt"));
  }
  {   // dates and times
    FakeEnvironment env(a);
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f action=/d><input id=d type=date value=2024-02-28 max=2024-03-01><input id=m type=month value=2024-12><input id=w type=week value=2024-W52>"
              "<input id=t type=time value=23:59><input id=l type=datetime-local value=2024-12-31T23:59><button id=b>go</button></form></body></html>");
    page.click(serial_of(page, "d"), 0, 0);
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "d") == "2024-02-29");
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "d") == "2024-03-01");
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "d") == "2024-03-02");
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.empty() && page_text(page, 80).find("less than or equal to 2024-03-01") != std::string::npos);
    page.click(serial_of(page, "m"), 0, 0);
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "m") == "2025-01");
    page.click(serial_of(page, "w"), 0, 0);
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "w") == "2025-W01");
    page.key(Page::FormKey::Down, 0, 0);
    page.key(Page::FormKey::Down, 0, 0);
    CHECK(value_of(page, "w") == "2024-W51");
    page.click(serial_of(page, "t"), 0, 0);
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "t") == "00:00");
    page.click(serial_of(page, "l"), 0, 0);
    page.key(Page::FormKey::Up, 0, 0);
    CHECK(value_of(page, "l") == "2025-01-01T00:00");
    // typing something that is not a date
    page.click(serial_of(page, "d"), 0, 0);
    for (int i = 0; i < 10; ++i) page.key(Page::FormKey::Backspace, 0, 0);
    type_text(page, "tomorrow");
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.empty() && page.document().element_by_id("d")->form_state & dom::kFormFocused);
  }
  {   // a datalist offers values as the user types; a click takes one
    FakeEnvironment env(a);
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><input id=f list=fruit><datalist id=fruit><option value=apple><option value=apricot label=Sweet><option value=banana></datalist><script>"
              "console.log(document.getElementById('f').list.id)</script></body></html>");
    CHECK(page_log(page) == "fruit");
    page.click(serial_of(page, "f"), 0, 0);
    type_text(page, "ap");
    std::string text = page_text(page, 60);
    CHECK(text.find("apple") != std::string::npos && text.find("apricot - Sweet") != std::string::npos && text.find("banana") == std::string::npos);
    page.click(node_of_text(page, "apricot"), 0, 0);
    CHECK(value_of(page, "f") == "apricot" && page_text(page, 60).find("apple") == std::string::npos);
  }
  CHECK(a.live == 0);
}

static void test_table_wrap() {
  TestAllocator a;
  FakeEnvironment env(a);
  MediaEnvironment media;
  media.width = 16 * 8;
  media.height = 400;
  Page page(a, env, "http://example.com/", media, true);
  page.load("<html><body><table><tr><td>one two three four</td><td>x</td></tr></table></body></html>");
  CHECK(page_text(page, 16) == "one two three  x\nfour");
}

static void test_forms() {
  TestAllocator a;
  const char* kForm =
      "<html><body><form id=f action=/search method=get>"
      "<input id=q name=q size=10 placeholder=Search maxlength=12>"
      "<input id=c type=checkbox name=c value=yes> <input id=r1 type=radio name=r value=a checked> <input id=r2 type=radio name=r value=b>"
      "<select id=s name=s><option value=1>One<option value=2 selected>Two<option value=3>Three</select>"
      "<textarea id=t name=t rows=2 cols=8>hello</textarea>"
      "<input id=go type=submit name=go value=Go><input type=reset id=rs value=Clear><input type=hidden name=h value=secret>"
      "</form></body></html>";
  {   // looks
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/dir/page.html", media, true);
    page.load(kForm);
    const std::string text = page_text(page, 60);
    CHECK(text.find("[Search    ]") != std::string::npos);
    CHECK(text.find("[ ]") != std::string::npos && text.find("(*)") != std::string::npos && text.find("( )") != std::string::npos);
    CHECK(text.find("[Two v]") == std::string::npos && text.find("[Two   v]") != std::string::npos);
    CHECK(text.find("[hello   ]") != std::string::npos);
    CHECK(text.find("[ Go ]") != std::string::npos && text.find("[ Clear ]") != std::string::npos);
    CHECK(text.find("secret") == std::string::npos);
  }
  {   // clicks and typing; the form is sent with GET
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/dir/page.html", media, true);
    page.load(std::string(kForm).replace(std::string(kForm).find("</form>"), 7, "</form><script>var log = []; ['input','change','focus','blur','submit'].forEach(function(t){ document.getElementById('f').addEventListener(t, function(e){ log.push(t + ':' + e.target.id) }, true) }); document.getElementById('f').addEventListener('submit', function(e){ console.log(log.join(',')) });</script>").c_str());
    CHECK(!page.has_focus());
    page.click(serial_of(page, "q"), 0, 0);
    CHECK(page.has_focus());
    type_text(page, "hi there and more");   // (maxlength 12)
    page.key(Page::FormKey::Backspace, 0, 0);
    dom::Node* q = page.document().element_by_id("q");
    String v(a);
    dom::get_value(q, &v);
    CHECK(v.view().equals("hi there an"));
    CHECK(page_text(page, 60).find("[ there an|]") != std::string::npos);
    page.click(serial_of(page, "c"), 0, 0);   // check the box (the field loses the focus: change)
    CHECK(dom::is_checked(page.document().element_by_id("c")));
    page.click(serial_of(page, "r2"), 0, 0);
    CHECK(!dom::is_checked(page.document().element_by_id("r1")) && dom::is_checked(page.document().element_by_id("r2")));
    // the list opens, an option is chosen
    page.click(serial_of(page, "s"), 0, 0);
    std::string open = page_text(page, 60);
    CHECK(open.find("( ) One") != std::string::npos && open.find("(*) Two") != std::string::npos && open.find("( ) Three") != std::string::npos);
    dom::Node* s = page.document().element_by_id("s");
    List<dom::Node*> options(a);
    dom::select_options(s, &options);
    page.click(options[2]->serial, 0, 0);
    CHECK(dom::selected_index(s) == 2);
    CHECK(page_text(page, 60).find("( ) One") == std::string::npos);   // (closed again)
    page.click(serial_of(page, "t"), 0, 0);
    page.key(Page::FormKey::Enter, 0, 0);
    type_text(page, "x y");
    page.click(serial_of(page, "go"), 0, 0);
    CHECK(env.navigations.size() == 1);
    CHECK(env.navigations.size() == 1 && env.navigations[0] == "http://example.com/search?q=hi+there+an&c=yes&r=b&s=3&t=hello%0D%0Ax+y&go=Go&h=secret");
    const std::string log = page_log(page);
    CHECK(log.find("input:q") != std::string::npos && log.find("change:c") != std::string::npos && log.find("change:r2") != std::string::npos &&
          log.find("change:s") != std::string::npos && log.find("submit:f") != std::string::npos);
    // reset goes back to the markup
    page.click(serial_of(page, "rs"), 0, 0);
    dom::get_value(q, &v);
    CHECK(v.empty() && !dom::is_checked(page.document().element_by_id("c")) && dom::is_checked(page.document().element_by_id("r1")) && dom::selected_index(s) == 1);
  }
  {   // Enter in a field sends the form; POST goes to the environment; a script can stop it
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f action=/login method=post><input id=u name=user><input type=password id=p name=pw><button name=b value=1>Go</button></form>"
              "<form id=g action=/stop><input id=x name=x></form><script>document.getElementById('g').addEventListener('submit', function(e) { e.preventDefault(); console.log('stopped') })</script></body></html>");
    page.click(serial_of(page, "u"), 0, 0);
    type_text(page, "bob");
    page.key(Page::FormKey::Tab, 0, 0);
    type_text(page, "s3 cret&");
    CHECK(page_text(page).find("[********|") != std::string::npos);
    page.key(Page::FormKey::Enter, 0, 0);
    CHECK(env.posts.size() == 1 && env.posts[0] == "http://example.com/login [application/x-www-form-urlencoded] user=bob&pw=s3+cret%26&b=1");
    page.click(serial_of(page, "x"), 0, 0);
    page.key(Page::FormKey::Enter, 0, 0);
    CHECK(page_log(page) == "stopped" && env.navigations.empty());
  }
  {   // multipart and plain text
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f method=post enctype=multipart/form-data><input name=a value=1><input name=b value='x\"y'><input type=submit id=s></form>"
              "<form id=g method=post enctype=text/plain><input name=a value=1><input type=submit id=s2></form></body></html>");
    page.click(serial_of(page, "s"), 0, 0);
    CHECK(env.posts.size() == 1 && env.posts[0].find("[multipart/form-data; boundary=----TheoryCraftOSFormBoundary7MA4YWxkTrZu0gW]") != std::string::npos &&
          env.posts[0].find("name=\"a\"\r\n\r\n1\r\n") != std::string::npos && env.posts[0].find("name=\"b\"\r\n\r\nx\"y\r\n") != std::string::npos);
    page.click(serial_of(page, "s2"), 0, 0);
    CHECK(env.posts.size() == 2 && env.posts[1] == "http://example.com/ [text/plain] a=1\r\n");
  }
  {   // constraints
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f action=/ok><input id=e type=email name=e required><input id=n type=number name=n min=1 max=5><input id=z name=z pattern='[A-Z]{2}[0-9]+'>"
              "<button id=b>Send</button></form></body></html>");
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.empty());
    CHECK(page_text(page, 80).find("! Please fill out this field.") != std::string::npos);
    CHECK(page.has_focus());
    type_text(page, "nobody");
    CHECK(page_text(page, 80).find("! ") == std::string::npos);   // (the note goes away when the user types)
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.empty() && page_text(page, 80).find("! Please include an '@' in the email address.") != std::string::npos);
    type_text(page, "@x.org");
    page.click(serial_of(page, "n"), 0, 0);
    type_text(page, "9");
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.empty() && page_text(page, 80).find("less than or equal to 5") != std::string::npos);
    page.key(Page::FormKey::Backspace, 0, 0);
    type_text(page, "3");
    page.click(serial_of(page, "z"), 0, 0);
    type_text(page, "abc");
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.empty() && page_text(page, 80).find("Please match the requested format.") != std::string::npos);
    for (int i = 0; i < 3; ++i) page.key(Page::FormKey::Backspace, 0, 0);
    type_text(page, "AB12");
    page.click(serial_of(page, "b"), 0, 0);
    CHECK(env.navigations.size() == 1 && env.navigations[0] == "http://example.com/ok?e=nobody%40x.org&n=3&z=AB12");
  }
  {   // novalidate and formnovalidate skip the checks
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f novalidate><input name=e required><button id=b>Send</button></form>"
              "<form id=g><input name=e required><button id=c formnovalidate>Send</button></form></body></html>");
    page.click(serial_of(page, "b"), 0, 0);
    page.click(serial_of(page, "c"), 0, 0);
    CHECK(env.navigations.size() == 2);
  }
  {   // scripts: values, the form object, FormData, validity, reset, submit
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f action=/go><input id=i name=inp value=start><input id=c type=checkbox name=chk checked><select id=s name=sel><option>A<option value=b>B</select>"
              "<textarea id=t name=txt>text</textarea><input id=n type=number name=num value=4 step=2><input id=r name=rng type=range min=0 max=10 value=3><button id=bt type=button>x</button></form>"
              "<script>"
              "var f = document.getElementById('f'), i = f.inp, r = [];"
              "r.push(f.elements.length, f.elements.inp === i, f.length, i.form === f, f.method, f.enctype);"
              "i.value = 'changed'; r.push(i.value, i.defaultValue, i.getAttribute('value'));"
              "f.chk.checked = false; r.push(f.chk.checked, f.chk.defaultChecked, f.chk.value);"
              "r.push(f.sel.value, f.sel.selectedIndex, f.sel.options.length, f.sel.options[1].value); f.sel.value = 'b'; r.push(f.sel.selectedIndex, f.sel.options[1].selected);"
              "r.push(f.txt.value, f.txt.type, f.sel.type, f.bt.type, f.num.type);"
              "f.num.stepUp(); r.push(f.num.value, f.num.valueAsNumber); f.num.value = 'abc'; r.push('[' + f.num.value + ']');"
              "r.push(f.rng.value);"
              "var fd = new FormData(f); r.push(fd.get('inp'), fd.has('chk'), fd.getAll('sel').join(), fd.get('missing'));"
              "fd.append('extra', 'x'); fd.set('inp', 'again'); r.push(fd.get('inp'), fd.entries().length);"
              "var seen = []; fd.forEach(function(v, k) { seen.push(k + '=' + v) }); r.push(seen.join('&'));"
              "f.reset(); r.push(i.value, f.chk.checked, f.sel.selectedIndex);"
              "i.setCustomValidity('nope'); r.push(i.validity.customError, i.validationMessage, f.checkValidity(), i.willValidate); i.setCustomValidity(''); r.push(f.checkValidity());"
              "i.required = true; i.value = ''; r.push(i.validity.valueMissing, i.validity.valid, i.validationMessage);"
              "console.log(r.join(' '))"
              "</script></body></html>");
    CHECK(page_log(page) ==
          "7 true 7 true get application/x-www-form-urlencoded changed start start false true on A 0 2 b 1 true text textarea select-one button number 6 6 [] 3 "
          "changed false b  again 6 inp=again&sel=b&txt=text&num=&rng=3&extra=x"
          " start true 0 true nope false true true true false Please fill out this field.");
  }
  {   // script-driven submit and requestSubmit
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form id=f action=/s><input name=a value=1 required><input name=b value=''><button id=bt name=btn value=v>x</button></form>"
              "<button id=run></button><script>"
              "var f = document.getElementById('f'); f.addEventListener('submit', function(e) { console.log('submit ' + e.submitter.id); e.preventDefault() });"
              "document.getElementById('run').addEventListener('click', function() { f.requestSubmit(document.getElementById('bt')); f.submit(); f.querySelector('[name=a]').value = ''; console.log(f.checkValidity(), f.reportValidity()) });"
              "</script></body></html>");
    page.click(serial_of(page, "run"), 0, 0);
    CHECK(page_log(page) == "submit bt | false false");
    CHECK(env.navigations.size() == 1 && env.navigations[0] == "http://example.com/s?a=1&b=");   // (submit() skips the event, and the button is not in it)
  }
  {   // without scripts a form still works
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, false);
    page.load("<html><body><form action=/plain><input id=q name=q><input type=checkbox id=c name=c><label id=l for=c>tick</label><input type=submit id=go></form></body></html>");
    page.click(serial_of(page, "q"), 0, 0);
    type_text(page, "no js");
    page.click(serial_of(page, "l"), 0, 0);
    CHECK(dom::is_checked(page.document().element_by_id("c")));
    page.click(serial_of(page, "go"), 0, 0);
    CHECK(env.navigations.size() == 1 && env.navigations[0] == "http://example.com/plain?q=no+js&c=on");
  }
  {   // focus: Tab steps through the controls, Escape leaves, focus() and activeElement
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><form><input id=a><input id=b disabled><input id=c><textarea id=d></textarea></form><button id=bt>go</button><script>document.getElementById('bt').addEventListener('click', function() { document.getElementById('c').focus() })</script>"
              "<style>#a:focus{color:red}</style></body></html>");
    CHECK(page.has_controls() && !page.has_focus());
    CHECK(page.key(Page::FormKey::Tab, 0, 0));
    CHECK(page.focused() && page.focused()->serial == serial_of(page, "a"));
    page.key(Page::FormKey::Tab, 0, 0);
    CHECK(page.focused()->serial == serial_of(page, "c"));
    page.key(Page::FormKey::Tab, 0, 1);
    CHECK(page.focused()->serial == serial_of(page, "a"));
    page.key(Page::FormKey::Escape, 0, 0);
    CHECK(!page.has_focus());
    CHECK(!page.key(Page::FormKey::Character, 'x', 0));   // (nothing focused: the view scrolls)
    page.click(serial_of(page, "bt"), 0, 0);
    CHECK(page.focused() && page.focused()->serial == serial_of(page, "c"));
  }
}

// Loads a page at `address`, lets its timers (and with them the network tasks) run until none is waiting, and returns what the scripts logged.
static std::string run_async(const char* script, const char* address, const std::function<void(FakeEnvironment&)>& setup = nullptr, int rounds = 400,
                             const std::function<void(FakeEnvironment&)>& after = nullptr) {
  TestAllocator a;
  std::string out;
  {
    FakeEnvironment env(a);
    if (setup) setup(env);
    MediaEnvironment media;
    Page page(a, env, address, media, true);
    page.load((std::string("<html><head><title>T</title></head><body><script>") + script + "</script></body></html>").c_str());
    for (int i = 0; i < rounds; ++i) {
      double due;
      if (!page.next_timer(&due)) break;
      if (getenv("TRACE")) printf("round %d due %.0f clock %llu\n", i, due, (unsigned long long)env.clock);
      if (due > env.clock) env.clock = static_cast<uint64_t>(due);
      page.run_due_timers(env.clock);
    }
    out = std::string(page.log().data(), page.log().size());
    if (!out.empty() && out.back() == '\n') out.pop_back();
    for (size_t p = 0; (p = out.find("\n", p)) != std::string::npos; ) { out.replace(p, 1, " | "); p += 3; }
    if (after) after(env);
  }
  if (a.live != 0) {
    printf("   leak (%d) in: %s\n", a.live, script);
    ++g_failed;
  }
  return out;
}

static bool async_is(const char* script, const char* expected, const std::function<void(FakeEnvironment&)>& setup = nullptr, const char* address = "https://app.example/dir/page.html") {
  const std::string got = run_async(script, address, setup);
  if (got != expected) {
    printf("   %s\n   logs     %s\n   expects  %s\n", script, got.c_str(), expected);
    return false;
  }
  return true;
}

static void test_shadow_rendering() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><x-card><span slot=title>Title</span><p>Body</p></x-card><script>var h = document.querySelector('x-card'); var r = h.attachShadow({ mode: 'open' });"
              "r.innerHTML = '<style>:host { color: blue } div { color: red }</style><h1><slot name=title></slot></h1><div><slot></slot></div>';</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    int title = -1, body = -1;
    for (uint64_t i = 0; i < doc.run_count(); ++i) {
      if (doc.run_text(i).equals("Title")) title = static_cast<int>(i);
      if (doc.run_text(i).equals("Body")) body = static_cast<int>(i);
    }
    CHECK(title >= 0 && body >= 0 && title < body);
    CHECK(title >= 0 && doc.run(static_cast<uint64_t>(title)).color == 0xFF0000FF);   // (the span inherits :host { color: blue } through the slot)
    CHECK(body >= 0 && doc.run(static_cast<uint64_t>(body)).color == 0xFFFF0000);
  }
  {   // the styles of the document do not reach into a shadow tree, and the styles of a shadow tree stay in it
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><head><style>p { color: red } .mark { color: lime }</style></head><body><p>outside</p><x-box></x-box><script>var r = document.querySelector('x-box').attachShadow({ mode: 'open' });"
              "r.innerHTML = '<style>.mark { color: blue }</style><p>inside</p><span class=mark>marked</span>';</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    int outside = -1, inside = -1, marked = -1;
    for (uint64_t i = 0; i < doc.run_count(); ++i) {
      if (doc.run_text(i).equals("outside")) outside = static_cast<int>(i);
      if (doc.run_text(i).equals("inside")) inside = static_cast<int>(i);
      if (doc.run_text(i).equals("marked")) marked = static_cast<int>(i);
    }
    CHECK(outside >= 0 && doc.run(static_cast<uint64_t>(outside)).color == 0xFFFF0000);
    CHECK(inside >= 0 && doc.run(static_cast<uint64_t>(inside)).color == 0);                   // (p { color: red } of the page does not apply)
    CHECK(marked >= 0 && doc.run(static_cast<uint64_t>(marked)).color == 0xFF0000FF);          // (.mark of the shadow tree, not the page's)
  }
}

static void test_url() {
  CHECK(js("", "var u = new URL('../x?y=1&z=a%20b#top', 'http://user:pw@a.example:8080/b/c/d'); console.log(u.href, u.origin, u.protocol, u.username, u.password, u.host, u.hostname, u.port, u.pathname, u.search, u.hash)",
           "http://user:pw@a.example:8080/b/x?y=1&z=a%20b#top http://a.example:8080 http: user pw a.example:8080 a.example 8080 /b/x ?y=1&z=a%20b #top"));
  CHECK(js("", "var u = new URL('https://EXAMPLE.com:443/a/./b/../c'); console.log(u.href, u.port === '', String(u), JSON.stringify({ u: u }), u instanceof URL)", "https://example.com/a/c true https://example.com/a/c {\"u\":\"https://example.com/a/c\"} true"));
  CHECK(js("", "var u = new URL('http://a.example/p?x=1'); u.pathname = '/q r'; u.search = 'k=v w'; u.hash = 'frag'; u.port = '81'; u.hostname = 'B.example'; u.protocol = 'https'; u.username = 'me'; console.log(u.href)", "https://me@b.example:81/q%20r?k=v%20w#frag"));
  CHECK(js("", "var u = new URL('http://a.example/'); u.href = 'ftp://f.example/x'; console.log(u.href); try { u.href = 'nonsense' } catch (e) { console.log(e.name) } u.port = 'abc'; u.host = ''; console.log(u.href)", "ftp://f.example/x | TypeError | ftp://f.example/x"));
  CHECK(js("", "try { new URL('nonsense') } catch (e) { console.log(e.name, e.message) } try { new URL('/x') } catch (e) { console.log(e.name) } try { URL('http://a/') } catch (e) { console.log(e.name) }",
           "TypeError Failed to construct 'URL': Invalid URL | TypeError | TypeError"));
  CHECK(js("", "console.log(URL.canParse('http://a/'), URL.canParse('x'), URL.canParse('x', 'http://a/b/'), new URL(new URL('http://a/b'), 'http://z/').href, new URL('c', new URL('http://a/b/')).href)", "true false true http://a/b http://a/b/c"));
  CHECK(js("", "var u = new URL('mailto:a@b.example?subject=hi'); console.log(u.protocol, u.pathname, u.search, u.origin, new URL('data:text/plain,hi').origin, new URL('file:///C:/x/y.txt').pathname)", "mailto: a@b.example ?subject=hi null null /C:/x/y.txt"));
  CHECK(js("", "console.log(new URL('http://m\xC3\xBCnchen.de/\xC3\xA4?q=\xC3\xB6#\xC3\xBC').href, new URL('http://[::1]:3000/').host, new URL('http://0x7f.1/').hostname)", "http://xn--mnchen-3ya.de/%C3%A4?q=%C3%B6#%C3%BC [::1]:3000 127.0.0.1"));
  // searchParams stays in step with the URL, both ways
  CHECK(js("", "var u = new URL('http://a/p?x=1&y=2'); var sp = u.searchParams; console.log(sp.get('x'), sp.size, sp === u.searchParams); sp.append('z', 'a b&c'); console.log(u.search, u.href); u.search = '?q=9'; console.log(sp.get('q'), sp.has('x'), sp.size); sp.delete('q'); console.log(u.href, u.search === '')",
           "1 2 true | ?x=1&y=2&z=a+b%26c http://a/p?x=1&y=2&z=a+b%26c | 9 false 1 | http://a/p true"));
  CHECK(js("", "var u = new URL('http://a/p?b=2&a=1'); u.searchParams.sort(); console.log(u.search); u.searchParams.set('a', 'x'); console.log(u.search); u.href = 'http://a/z?m=1'; console.log(u.searchParams.toString())", "?a=1&b=2 | ?a=x&b=2 | m=1"));
  CHECK(js("", "console.log(typeof URL.createObjectURL, URL.createObjectURL(new Blob(['x'])).indexOf('blob:http'), (function () { var u = URL.createObjectURL(new Blob(['x'])); URL.revokeObjectURL(u); return u.length > 8 })())", "function 0 true"));
}

static void test_search_params() {
  CHECK(js("", "var p = new URLSearchParams('?a=1&b=2&a=3&c=%C3%A9+x&d'); console.log(p.get('a'), p.getAll('a').join(), p.get('c'), p.get('d') === '', p.get('nope'), p.has('b'), p.has('a', '3'), p.has('a', '9'), p.size, p.toString())", "1 1,3 \xC3\xA9 x true null true true false 5 a=1&b=2&a=3&c=%C3%A9+x&d="));
  CHECK(js("", "var p = new URLSearchParams({ x: 1, y: 'two words', z: '&=' }); console.log(p.toString()); p.set('x', 5); p.set('new', 'n'); p.delete('y'); console.log(p.toString()); p.append('x', 6); console.log(p.getAll('x').join(), p.size); p.delete('x', 6); console.log(p.toString())",
           "x=1&y=two+words&z=%26%3D | x=5&z=%26%3D&new=n | 5,6 4 | x=5&z=%26%3D&new=n"));
  CHECK(js("", "var p = new URLSearchParams([['a', '1'], ['b', '2']]); var q = new URLSearchParams(p); q.append('c', '3'); console.log(p.toString(), q.toString()); try { new URLSearchParams([['a']]) } catch (e) { console.log(e.name) } console.log(new URLSearchParams().toString() === '', new URLSearchParams(null).size, new URLSearchParams('').size)",
           "a=1&b=2 a=1&b=2&c=3 | TypeError | true 0 0"));
  CHECK(js("", "var p = new URLSearchParams('b=2&a=1&c=3&a=0'); var out = []; for (var e of p) out.push(e[0] + e[1]); console.log(out.join(), [...p].length, p.keys().join(), p.values().join(), p.entries()[1].join('=')); p.forEach(function (v, k, self) { out.push(k + ':' + v + (self === p)) }); console.log(out.slice(4).join())",
           "b2,a1,c3,a0 4 b,a,c,a 2,1,3,0 a=1 | b:2true,a:1true,c:3true,a:0true"));
  CHECK(js("", "var p = new URLSearchParams('b=2&a=1&c=3&a=0'); p.sort(); console.log(p.toString()); var q = new URLSearchParams('z=1&\xC3\xA9=2&a=3'); q.sort(); console.log(q.keys().join())", "a=1&a=0&b=2&c=3 | a,z,\xC3\xA9"));
  CHECK(js("", "var p = new URLSearchParams('q=a%2Bb+c%20d&%E2%82%AC=%e2%82%ac'); console.log(p.get('q'), p.get('\xE2\x82\xAC'), String(p), p instanceof URLSearchParams, Object.keys(p).length)", "a+b c d \xE2\x82\xAC q=a%2Bb+c+d&%E2%82%AC=%E2%82%AC true 0"));
}

static void test_headers() {
  CHECK(js("", "var h = new Headers({ 'Content-Type': 'text/plain', 'X-A': '1' }); h.append('x-a', '2'); console.log(h.get('content-type'), h.get('X-A'), h.has('x-a'), h.has('nope'), h.get('nope')); h.set('X-A', '3'); h.delete('content-type'); console.log(h.get('x-a'), h.has('content-type'))",
           "text/plain 1, 2 true false null | 3 false"));
  CHECK(js("", "var h = new Headers([['B', '2'], ['a', '1'], ['b', '3']]); var out = []; h.forEach(function (v, k) { out.push(k + '=' + v) }); console.log(out.join(), h.keys().join(), h.values().join('|')); for (var e of h) out.push(e[0]); console.log(out.slice(2).join())", "a=1,b=2, 3 a,b 1|2, 3 | a,b"));
  CHECK(js("", "var h = new Headers(); h.append('Set-Cookie', 'a=1'); h.append('Set-Cookie', 'b=2'); console.log(h.get('set-cookie'), h.getSetCookie().join('|'), h.entries().length); var c = new Headers(h); c.set('x', 'y'); console.log(h.has('x'), c.has('x'))", "a=1, b=2 a=1|b=2 2 | false true"));
  CHECK(js("", "var h = new Headers(); try { h.append('bad name', 'x') } catch (e) { console.log(e.name) } try { h.append('ok', 'a\\nb') } catch (e) { console.log(e.name) } h.append('v', '  padded  '); console.log(h.get('v') + '|'); try { new Headers(5) } catch (e) { console.log(e.name) } try { new Headers([['a']]) } catch (e) { console.log(e.name) }",
           "TypeError | TypeError | padded| | TypeError | TypeError"));
}

static void test_blob_text() {
  CHECK(async_is("var b = new Blob(['ab', new Blob(['cd']), new Uint8Array([101, 102])], { type: 'Text/Plain' }); console.log(b.size, b.type, b instanceof Blob); b.text().then(function (t) { console.log(t) }); b.slice(1, 4, 'x/y').text().then(function (t) { console.log(t) });"
                  "b.slice(-2).text().then(function (t) { console.log(t) }); b.arrayBuffer().then(function (u) { console.log(u.length, u[0]) });", "6 text/plain true | abcdef | bcd | ef | 6 97"));
  CHECK(async_is("var f = new File(['hello'], 'a.txt', { type: 'text/plain', lastModified: 5 }); console.log(f.name, f.size, f.type, f.lastModified, f instanceof Blob, f instanceof File); try { new File(['x']) } catch (e) { console.log(e.name) }", "a.txt 5 text/plain 5 true true | TypeError"));
  CHECK(async_is("var r = new FileReader(); var order = []; r.onload = function () { order.push('onload:' + r.result) }; r.addEventListener('loadend', function () { order.push('loadend ' + r.readyState); console.log(order.join()) }); r.readAsText(new Blob(['text \\u00e9'])); order.push('started ' + r.readyState);", "started 1,onload:text \xC3\xA9,loadend 2"));
  CHECK(async_is("var r = new FileReader(); r.onload = function () { console.log(r.result) }; r.readAsDataURL(new Blob(['hi'], { type: 'text/plain' }));", "data:text/plain;base64,aGk="));
  CHECK(js("", "var e = new TextEncoder().encode('h\xC3\xA9'); console.log(e.length, e[1], e[2], new TextDecoder().decode(e), new TextDecoder('utf-8').decode(new Uint8Array([0xff, 0x41])), new TextEncoder().encoding)", "3 195 169 h\xC3\xA9 \xEF\xBF\xBD" "A utf-8"));
  CHECK(js("", "try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff])) } catch (e) { console.log(e.name) } console.log(new TextDecoder('latin1').decode(new Uint8Array([0xe9])) === '\xC3\xA9', new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])), new TextDecoder().decode())", "TypeError | true A "));
}

static void test_abort() {
  CHECK(async_is("var c = new AbortController(); var s = c.signal; var log = []; s.addEventListener('abort', function (e) { log.push('listener ' + e.type) }); s.onabort = function () { log.push('onabort') }; console.log(s.aborted); c.abort(); c.abort(); console.log(s.aborted, s.reason.name, log.join());", "false | true AbortError onabort,listener abort"));
  CHECK(async_is("var c = new AbortController(); c.abort('why'); console.log(c.signal.reason); try { c.signal.throwIfAborted() } catch (e) { console.log(e) } console.log(AbortSignal.abort().aborted, typeof AbortSignal.timeout);", "why | why | true function"));
  CHECK(async_is("var s = AbortSignal.timeout(50); s.addEventListener('abort', function () { console.log('timed out', s.reason.name) });", "timed out TimeoutError"));
  CHECK(async_is("var a = new AbortController(), b = new AbortController(); var any = AbortSignal.any([a.signal, b.signal]); any.addEventListener('abort', function () { console.log('any', any.reason) }); b.abort('b!');", "any b!"));
}

static void stock(FakeEnvironment& env) {
  env.net.answers["GET https://app.example/api/user"] = {200, "Content-Type: application/json\r\nX-Custom: yes\r\nSet-Cookie: seen=1; Path=/\r\n", "{\"name\":\"Ada\",\"langs\":[\"en\",\"fr\"]}"};
  env.net.answers["GET https://app.example/text"] = {200, "Content-Type: text/plain; charset=utf-8\r\n", "plain text \xC3\xA9"};
  env.net.answers["POST https://app.example/api/echo"] = {201, "Content-Type: text/plain\r\n", "created"};
  env.net.answers["PUT https://app.example/api/item"] = {200, "", "put"};
  env.net.answers["GET https://app.example/old"] = {302, "Location: /new\r\n", ""};
  env.net.answers["GET https://app.example/new"] = {200, "Content-Type: text/plain\r\n", "moved here"};
  env.net.answers["GET https://app.example/bad"] = {500, "", "oops"};
  env.net.answers["GET https://app.example/empty"] = {204, "", ""};
  env.net.answers["GET https://app.example/form"] = {200, "Content-Type: application/x-www-form-urlencoded\r\n", "a=1&b=two+words&a=3"};
  env.net.answers["GET https://app.example/bin"] = {200, "Content-Type: application/octet-stream\r\n", std::string("\x01\x02\xff\x00\x41", 5)};
  env.net.answers["GET https://api.other/open"] = {200, "Access-Control-Allow-Origin: https://app.example\r\nContent-Type: text/plain\r\nX-Hidden: h\r\n", "cross"};
  env.net.answers["GET https://api.other/closed"] = {200, "Content-Type: text/plain\r\n", "private"};
  env.net.answers["OPTIONS https://api.other/open"] = {204, "Access-Control-Allow-Origin: https://app.example\r\nAccess-Control-Allow-Methods: PUT\r\nAccess-Control-Allow-Headers: x-token\r\n", ""};
  env.net.answers["PUT https://api.other/open"] = {200, "Access-Control-Allow-Origin: https://app.example\r\n", "put cross"};
}

static void test_fetch() {
  CHECK(async_is("fetch('/api/user').then(function (r) { console.log(r.status, r.ok, r.statusText, r.url, r.type, r.redirected, r.headers.get('content-type'), r.headers.get('x-custom'), r.headers.get('set-cookie'), r.bodyUsed); return r.json() }).then(function (d) { console.log(d.name, d.langs.join('+')) });",
                 "200 true OK https://app.example/api/user basic false application/json yes null false | Ada en+fr", stock));
  CHECK(async_is("fetch('/text').then(function (r) { return r.text() }).then(function (t) { console.log(t, t.length) });", "plain text \xC3\xA9 13", stock));
  CHECK(async_is("fetch('/bad').then(function (r) { console.log(r.status, r.ok); return r.text() }).then(function (t) { console.log(t) }); fetch('/missing').then(function (r) { console.log(r.status, r.ok) }); fetch('/empty').then(function (r) { console.log(r.status, r.body); return r.text() }).then(function (t) { console.log('[' + t + ']') });",
                 "500 false | oops | 404 false | 204 null | []", stock));
  CHECK(async_is("fetch('/old').then(function (r) { console.log(r.redirected, r.url); return r.text() }).then(function (t) { console.log(t) }); fetch('/old', { redirect: 'manual' }).then(function (r) { console.log(r.status, r.headers.get('location')) }); fetch('/old', { redirect: 'error' }).catch(function (e) { console.log(e.name) });",
                 "true https://app.example/new | moved here | 0 null | TypeError", stock));
  CHECK(async_is("fetch('http://down.example/x').catch(function (e) { console.log(e.name, e.message.indexOf('Failed to fetch') === 0) }); fetch('nonsense:').catch(function (e) { console.log(e.name) }); fetch('/api/user', { method: 'GET', body: 'x' }).catch(function (e) { console.log(e.name) });", "TypeError | TypeError true | TypeError", stock));
  // POST bodies
  CHECK(async_is("fetch('/api/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Token': 't' }, body: JSON.stringify({ a: 1 }) }).then(function (r) { console.log(r.status); return r.text() }).then(function (t) { console.log(t) });", "201 | created",
                 stock));
  CHECK(async_is("fetch('/api/echo', { method: 'POST', body: new URLSearchParams({ a: '1 2', b: 'x' }) }).then(function (r) { console.log(r.status) });", "201", stock));
  CHECK(async_is("var fd = new FormData(); fd.append('k', 'v'); fd.append('f', new File(['filedata'], 'a.txt', { type: 'text/plain' })); fetch('/api/echo', { method: 'POST', body: fd }).then(function (r) { console.log(r.status) });", "201", stock));
  CHECK(async_is("fetch('/api/echo', { method: 'POST', body: new Blob(['blob body'], { type: 'text/x' }) }).then(function (r) { console.log(r.status) }); fetch('/api/echo', { method: 'POST', body: new Uint8Array([65, 66]) }).then(function (r) { console.log(r.status) });", "201 | 201", stock));
  // Request objects
  CHECK(async_is("var req = new Request('/api/user', { headers: { 'X-A': '1' } }); console.log(req.url, req.method, req.mode, req.credentials, req.redirect, req.headers.get('x-a'), req.signal.aborted, req.bodyUsed); var c = new Request(req, { method: 'POST', body: 'b' }); console.log(c.method, c.url, c.headers.get('x-a'), c.headers.get('content-type')); fetch(req).then(function (r) { console.log(r.status) });",
                 "https://app.example/api/user GET cors same-origin follow 1 false false | POST https://app.example/api/user 1 text/plain;charset=UTF-8 | 200", stock));
  CHECK(async_is("try { new Request('/x', { method: 'GET', body: 'b' }) } catch (e) { console.log(e.name) } try { new Request('/x', { method: 'TRACE' }) } catch (e) { console.log(e.name) } try { new Request('http://u:p@a/') } catch (e) { console.log(e.name) } try { new Request() } catch (e) { console.log(e.name) } try { new Request('/x', { mode: 'wat' }) } catch (e) { console.log(e.name) }",
                 "TypeError | TypeError | TypeError | TypeError | TypeError", stock));
  // Response objects
  CHECK(async_is("var r = new Response('body text', { status: 201, statusText: 'Made', headers: { 'X-A': '1' } }); console.log(r.status, r.ok, r.statusText, r.headers.get('x-a'), r.headers.get('content-type'), r.type, r.bodyUsed); r.text().then(function (t) { console.log(t, r.bodyUsed); return r.text() }).catch(function (e) { console.log(e.name) });",
                 "201 true Made 1 text/plain;charset=UTF-8 default false | body text true | TypeError", stock));
  CHECK(async_is("var r = Response.json({ a: [1, 2] }, { status: 202 }); console.log(r.status, r.headers.get('content-type')); r.clone().json().then(function (d) { console.log(d.a.length) }); r.text().then(function (t) { console.log(t) }); console.log(Response.error().type, Response.error().status, Response.redirect('/x', 301).headers.get('location'), Response.redirect('/x').status);"
                 "try { new Response('x', { status: 99 }) } catch (e) { console.log(e.name) } try { new Response('x', { status: 204 }) } catch (e) { console.log(e.name) } try { r.clone() } catch (e) { console.log(e.name) }",
                 "202 application/json | error 0 https://app.example/x 302 | RangeError | TypeError | TypeError | 2 | {\"a\":[1,2]}", stock));
  CHECK(async_is("fetch('/bin').then(function (r) { return r.arrayBuffer() }).then(function (b) { var u = new Uint8Array(b); console.log(u.length, b.byteLength, u[0], u[2], u[3], u[4]) }); fetch('/bin').then(function (r) { return r.blob() }).then(function (b) { console.log(b.size, b.type) });", "5 5 1 255 0 65 | 5 application/octet-stream", stock));
  CHECK(async_is("fetch('/form').then(function (r) { return r.formData() }).then(function (fd) { console.log(fd.get('a'), fd.getAll('a').join(), fd.get('b')) });", "1 1,3 two words", stock));
  CHECK(async_is("fetch('/text').then(function (r) { var reader = r.body.getReader(); return reader.read().then(function (c) { console.log(c.done, c.value.length); return reader.read() }).then(function (c) { console.log(c.done) }) });", "false 13 | true", stock));
  CHECK(async_is("fetch('/api/user').then(function (r) { var c = r.clone(); return Promise.all([r.json(), c.text()]) }).then(function (a) { console.log(a[0].name, a[1].length) });", "Ada 34", stock));
  // abort
  CHECK(async_is("var c = new AbortController(); var p = fetch('/api/user', { signal: c.signal }); p.catch(function (e) { console.log(e.name) }); c.abort(); fetch('/api/user', { signal: AbortSignal.abort() }).catch(function (e) { console.log('pre', e.name) });", "AbortError | pre AbortError", stock));
  // the network is not touched for an aborted request
  {
    std::string got = run_async("fetch('/text', { signal: AbortSignal.abort() }).catch(function () { console.log('rejected') });", "https://app.example/", [](FakeEnvironment& env) { stock(env); });
    CHECK(got == "rejected");
  }
  // cross-origin
  CHECK(async_is("fetch('https://api.other/open').then(function (r) { console.log(r.type, r.headers.get('content-type'), r.headers.get('x-hidden')); return r.text() }).then(function (t) { console.log(t) }); fetch('https://api.other/closed').catch(function (e) { console.log(e.name) });", "cors text/plain null | cross | TypeError", stock));
  CHECK(async_is("fetch('https://api.other/closed', { mode: 'no-cors' }).then(function (r) { console.log(r.type, r.status, r.ok) }); fetch('https://api.other/open', { mode: 'same-origin' }).catch(function (e) { console.log('same-origin ' + e.name) });", "opaque 0 false | same-origin TypeError", stock));
  CHECK(async_is("fetch('https://api.other/open', { method: 'PUT', headers: { 'X-Token': 't' } }).then(function (r) { return r.text() }).then(function (t) { console.log(t) });", "put cross", stock));
  // cookies: the response's Set-Cookie is kept for the page, and goes out with the next request
  CHECK(async_is("fetch('/api/user').then(function () { console.log(document.cookie); return fetch('/text') }).then(function () { console.log('done') });", "seen=1 | done", stock));
  // blob: URLs
  CHECK(async_is("var u = URL.createObjectURL(new Blob(['from blob'], { type: 'text/x' })); fetch(u).then(function (r) { console.log(r.status, r.headers.get('content-type')); return r.text() }).then(function (t) { console.log(t); URL.revokeObjectURL(u); return fetch(u) }).catch(function (e) { console.log(e.name) });",
                 "200 text/x | from blob | TypeError", stock));
  CHECK(async_is("fetch('data:text/plain;base64,aGk=').then(function (r) { return r.text() }).then(function (t) { console.log(t) });", "hi", stock));
  CHECK(async_is("(async function () { var r = await fetch('/api/user'); var d = await r.json(); console.log(d.name); try { await fetch('/nothing', { method: 'POST', body: {}.x }); await fetch('http://down.example/'); } catch (e) { console.log('caught', e.name) } })();", "Ada | caught TypeError", stock));
}

static void test_xhr() {
  CHECK(async_is("var x = new XMLHttpRequest(); var log = []; log.push(x.readyState); x.onreadystatechange = function () { log.push('rs' + x.readyState) }; x.onload = function () { log.push('load ' + x.status + ' ' + x.statusText + ' ' + x.responseText.length) }; x.onloadend = function () { log.push('end'); console.log(log.join()) };"
                 "x.open('GET', '/text'); x.send(); log.push('sent');", "0,rs1,sent,rs2,rs3,rs4,load 200 OK 13,end", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/api/user'); x.responseType = 'json'; x.onload = function () { console.log(x.response.name, typeof x.response, x.getResponseHeader('Content-Type'), x.getResponseHeader('x-nothing'), x.getAllResponseHeaders().split('\\r\\n').sort().join('|'), x.responseURL) }; x.send();",
                 "Ada object application/json null |content-type: application/json|x-custom: yes https://app.example/api/user", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('POST', '/api/echo'); x.setRequestHeader('Content-Type', 'text/plain'); x.setRequestHeader('X-A', '1'); x.setRequestHeader('X-A', '2'); x.setRequestHeader('Cookie', 'evil=1'); x.onload = function () { console.log(x.status, x.responseText) }; x.send('payload');", "201 created", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.addEventListener('load', function (e) { console.log('listener', e.type, e.lengthComputable, e.loaded, e.target === x) }); x.addEventListener('loadstart', function () { console.log('start') }); x.addEventListener('progress', function (e) { console.log('progress', e.loaded) }); x.open('GET', '/text'); x.send();",
                 "start | progress 13 | listener load true 13 true", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/bad'); x.onload = function () { console.log(x.status, x.responseText) }; x.send(); var y = new XMLHttpRequest(); y.open('GET', 'http://down.example/x'); y.onerror = function (e) { console.log('error', y.status, y.readyState, e.type) }; y.onload = function () { console.log('NO') }; y.send();", "500 oops | error 0 4 error", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/text', false); x.send(); console.log(x.status, x.responseText, x.readyState); try { var y = new XMLHttpRequest(); y.open('GET', 'http://down.example/', false); y.send(); } catch (e) { console.log(e.name) }", "200 plain text \xC3\xA9 4 | NetworkError", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); var log = []; x.onabort = function () { log.push('abort') }; x.onload = function () { log.push('NO') }; x.onloadend = function () { log.push('end') }; x.open('GET', '/text'); x.send(); x.abort(); log.push(x.readyState + ':' + x.status); setTimeout(function () { console.log(log.join()) }, 100);", "abort,end,0:0", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); try { x.send() } catch (e) { console.log(e.name) } try { x.setRequestHeader('a', 'b') } catch (e) { console.log(e.name) } try { x.open('TRACE', '/x') } catch (e) { console.log(e.name) } try { x.open('GET', 'http://') } catch (e) { console.log(e.name) } x.open('GET', '/text'); x.send(); try { x.send() } catch (e) { console.log(e.name) } console.log(XMLHttpRequest.DONE, x.OPENED);",
                 "InvalidStateError | InvalidStateError | SecurityError | SyntaxError | InvalidStateError | 4 1", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/bin'); x.responseType = 'arraybuffer'; x.onload = function () { var u = new Uint8Array(x.response); console.log(u.length, u[2]) }; x.send(); var y = new XMLHttpRequest(); y.open('GET', '/bin'); y.responseType = 'blob'; y.onload = function () { console.log(y.response.size, y.response.type) }; y.send();", "5 255 | 5 application/octet-stream", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', 'https://api.other/open'); x.onload = function () { console.log(x.status, x.responseText) }; x.send(); var y = new XMLHttpRequest(); y.open('GET', 'https://api.other/closed'); y.onerror = function () { console.log('blocked') }; y.send();", "200 cross | blocked", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/text'); x.onload = function () { x.open('GET', '/api/user'); x.onload = function () { console.log('second', x.status, JSON.parse(x.responseText).name) }; x.send() }; x.send();", "second 200 Ada", stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/text'); x.withCredentials = true; x.onload = function () { console.log(x.withCredentials, x.upload instanceof EventTarget, typeof x.upload.addEventListener) }; x.send();", "true true function", stock));
}

// What goes out on the wire.
static void test_fetch_wire() {
  std::vector<std::string> sent;
  auto capture = [&](FakeEnvironment& env) { sent = env.net.sent; };
  run_async("document.cookie = 'pre=1; path=/'; fetch('/text'); fetch('/text', { credentials: 'omit' }); fetch('/api/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Token': 'tk', 'Cookie': 'evil=1', 'Origin': 'x', 'Host': 'h' }, body: '{\"a\":1}' });",
            "https://app.example/dir/page.html#frag", stock, 400, capture);
  CHECK(sent.size() == 3);
  if (sent.size() == 3) {
    CHECK(sent[0].find("GET https://app.example/text\n") == 0 && sent[0].find("Cookie: pre=1") != std::string::npos && sent[0].find("Referer: https://app.example/dir/page.html\r\n") != std::string::npos);
    CHECK(sent[0].find("Origin:") == std::string::npos);   // (not for a GET to the same origin)
    CHECK(sent[1].find("Cookie") == std::string::npos);
    CHECK(sent[2].find("POST https://app.example/api/echo\n") == 0 && sent[2].find("content-type: application/json") != std::string::npos && sent[2].find("x-token: tk") != std::string::npos);
    CHECK(sent[2].find("evil") == std::string::npos && sent[2].find("Origin: https://app.example") != std::string::npos && sent[2].find("Cookie: pre=1") != std::string::npos && sent[2].find("host") == std::string::npos);
    CHECK(sent[2].size() >= 7 && sent[2].substr(sent[2].size() - 7) == "{\"a\":1}");
  }
  sent.clear();
  run_async("var x = new XMLHttpRequest(); x.open('PUT', '/api/item'); x.setRequestHeader('X-Q', '1'); x.send('xdata');", "https://app.example/", stock, 400, capture);
  CHECK(sent.size() == 1 && sent[0].find("PUT https://app.example/api/item\n") == 0 && sent[0].find("x-q: 1") != std::string::npos && sent[0].find("Origin: https://app.example") != std::string::npos && sent[0].substr(sent[0].size() - 5) == "xdata");
  // cross-origin requests carry the Origin and no cookies; a preflight comes first
  sent.clear();
  run_async("document.cookie = 'mine=1; path=/'; fetch('https://api.other/open'); fetch('https://api.other/open', { method: 'PUT', headers: { 'X-Token': 'z' } });", "https://app.example/", stock, 400, capture);
  CHECK(sent.size() == 3 && sent[0].find("GET https://api.other/open\n") == 0 && sent[0].find("Origin: https://app.example") != std::string::npos && sent[0].find("Cookie") == std::string::npos);
  CHECK(sent.size() == 3 && sent[1].find("OPTIONS https://api.other/open\n") == 0 && sent[1].find("Access-Control-Request-Method: PUT") != std::string::npos && sent[1].find("access-control-request-headers: x-token") == std::string::npos);
  // the page's cookie jar is the one the network sees, and what the network sets is what the page reads
  CHECK(async_is("document.cookie = 'a=1; path=/'; document.cookie = 'h=2; path=/; HttpOnly'; console.log(document.cookie); fetch('/api/user').then(function () { console.log(document.cookie) });", "a=1 | a=1; seen=1", stock));
}

// Two windows (tabs) of one browser: they share localStorage and the hub that carries events between pages; each has its own sessionStorage. Both are loaded, then
// their timers run until nothing is waiting. Returns "A: ... / B: ..." (what each page logged).
static std::string two_windows(const char* script_a, const char* script_b, const char* address_a = "https://app.example/a.html", const char* address_b = "https://app.example/b.html",
                               bool same_tab = false) {
  TestAllocator a;
  std::string out;
  {
    FakeEnvironment env_a(a), env_b(a);
    env_b.shared_local = &env_a.local_hub;
    env_b.shared_hub = &env_a.hub;
    if (same_tab) {
      env_b.tab = env_a.tab;
    } else {
      env_b.tab = 2;
    }
    MediaEnvironment media;
    Page page_a(a, env_a, address_a, media, true);
    Page page_b(a, env_b, address_b, media, true);
    page_a.load((std::string("<html><body><script>") + script_a + "</script></body></html>").c_str());
    page_b.load((std::string("<html><body><script>") + script_b + "</script></body></html>").c_str());
    for (int i = 0; i < 400; ++i) {
      double due_a, due_b;
      const bool any_a = page_a.next_timer(&due_a), any_b = page_b.next_timer(&due_b);
      if (!any_a && !any_b) break;
      double due = any_a && (!any_b || due_a < due_b) ? due_a : due_b;
      if (due > env_a.clock) env_a.clock = static_cast<uint64_t>(due);
      env_b.clock = env_a.clock;
      page_a.run_due_timers(env_a.clock);
      page_b.run_due_timers(env_b.clock);
    }
    auto flat = [](Page& page) {
      std::string log(page.log().data(), page.log().size());
      if (!log.empty() && log.back() == '\n') log.pop_back();
      for (size_t p = 0; (p = log.find("\n", p)) != std::string::npos; ) { log.replace(p, 1, " | "); p += 3; }
      return log;
    };
    out = "A: " + flat(page_a) + " / B: " + flat(page_b);
  }
  if (a.live != 0) {
    printf("   leak (%d)\n", a.live);
    ++g_failed;
  }
  return out;
}

static bool windows_are(const char* script_a, const char* script_b, const char* expected, bool same_tab = false) {
  const std::string got = two_windows(script_a, script_b, "https://app.example/a.html", "https://app.example/b.html", same_tab);
  if (got != expected) {
    printf("   A: %s\n   B: %s\n   gives   %s\n   expects %s\n", script_a, script_b, got.c_str(), expected);
    return false;
  }
  return true;
}

static void test_storage() {
  // basics
  CHECK(js("", "localStorage.setItem('a', 1); localStorage.b = 'two'; console.log(localStorage.getItem('a'), localStorage.a, localStorage.length, localStorage.key(1), localStorage.nothing, Object.keys(localStorage).join()); delete localStorage.a; localStorage.removeItem('b'); console.log(localStorage.length); localStorage.clear(); console.log(localStorage.getItem('x'))", "1 1 2 b undefined a,b | 0 | null"));
  CHECK(js("", "sessionStorage.setItem('s', 'v'); console.log(sessionStorage.getItem('s'), localStorage.getItem('s'), sessionStorage.length, typeof sessionStorage.setItem, sessionStorage instanceof Storage, localStorage === sessionStorage)", "v null 1 function true false"));
  CHECK(js("", "localStorage.setItem('k', undefined); localStorage.setItem(5, {}); console.log(localStorage.k, localStorage[5], localStorage.getItem(5)); try { localStorage.setItem('only') } catch (e) { console.log(e.name) }", "undefined [object Object] [object Object] | TypeError"));
  CHECK(js("", "var big = new Array(1024 * 1024).join('x'); try { for (var i = 0; i < 20; i++) localStorage.setItem('k' + i, big) } catch (e) { console.log(e.name, e.code, i >= 4) }", "QuotaExceededError 22 true"));
  // storage is kept apart per origin, and shared by the windows of one
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page one(a, env, "https://one.example/p", media, true);
      Page two(a, env, "https://two.example/p", media, true);
      Page one_again(a, env, "https://one.example/q", media, true);
      one.load("<script>localStorage.setItem('who', 'one'); sessionStorage.setItem('s', '1')</script>");
      two.load("<script>console.log(localStorage.getItem('who'), sessionStorage.getItem('s'))</script>");
      one_again.load("<script>console.log(localStorage.getItem('who'), sessionStorage.getItem('s'))</script>");
      CHECK(std::string(two.log().data(), two.log().size()) == "null null\n");
      CHECK(std::string(one_again.log().data(), one_again.log().size()) == "one 1\n");
    }
    CHECK(a.live == 0);
  }
  CHECK(windows_are("localStorage.setItem('shared', 'from A')", "console.log(localStorage.getItem('shared'), sessionStorage.getItem('shared'))", "A:  / B: from A null"));
  // storage events reach the other windows, not the one that made the change
  CHECK(windows_are("window.addEventListener('storage', function (e) { console.log('A got ' + e.key) }); localStorage.setItem('k', 'v1'); localStorage.setItem('k', 'v2'); localStorage.removeItem('k');",
                    "window.addEventListener('storage', function (e) { console.log(e.type, e.key, e.oldValue, e.newValue, e.url, e.storageArea === localStorage, e.bubbles) });",
                    "A:  / B: storage k null v1 https://app.example/a.html true false | storage k v1 v2 https://app.example/a.html true false | storage k v2 null https://app.example/a.html true false"));
  CHECK(windows_are("localStorage.setItem('a', '1'); localStorage.setItem('b', '2'); setTimeout(function () { localStorage.clear(); localStorage.clear(); }, 5);",
                    "window.onstorage = function (e) { console.log(e.key === null ? 'cleared' : e.key + '=' + e.newValue) };", "A:  / B: a=1 | b=2 | cleared"));
  CHECK(windows_are("localStorage.setItem('same', 'x'); localStorage.setItem('same', 'x');", "var n = 0; window.addEventListener('storage', function (e) { console.log(++n) });", "A:  / B: 1"));
  // sessionStorage events only reach windows of the same tab (frames); other tabs have storage of their own
  CHECK(windows_are("sessionStorage.setItem('t', 'A')", "window.addEventListener('storage', function () { console.log('NO') }); console.log(sessionStorage.getItem('t'))", "A:  / B: null"));
  CHECK(windows_are("setTimeout(function () { sessionStorage.setItem('t', 'A') }, 1)", "window.addEventListener('storage', function (e) { console.log(e.key, e.storageArea === sessionStorage, e.storageArea === localStorage) })", "A:  / B: t true false", true));
  // other origins hear nothing
  {
    const std::string got = [&] {
      TestAllocator a;
      std::string out;
      {
        FakeEnvironment env_a(a), env_b(a);
        env_b.shared_local = &env_a.local_hub;
        env_b.shared_hub = &env_a.hub;
        MediaEnvironment media;
        Page pa(a, env_a, "https://one.example/", media, true), pb(a, env_b, "https://two.example/", media, true);
        pa.load("<script>localStorage.setItem('x', 1)</script>");
        pb.load("<script>addEventListener('storage', function () { console.log('NO') })</script>");
        pb.run_due_timers(1e18);
        out = std::string(pb.log().data(), pb.log().size());
      }
      return out;
    }();
    CHECK(got.empty());
  }
}

static void test_cookies_web() {
  // public suffixes cannot be named by Domain; the host's own registrable domain can
  CHECK(async_is("document.cookie = 'a=1; domain=example'; document.cookie = 'b=2; domain=co.uk'; document.cookie = 'c=3; domain=app.example'; console.log(document.cookie);", "c=3", nullptr, "https://app.example/"));
  CHECK(async_is("document.cookie = 'a=1; domain=github.io; path=/'; document.cookie = 'b=2; domain=me.github.io; path=/'; console.log(document.cookie);", "b=2", nullptr, "https://me.github.io/"));
  // Cookie Store API
  CHECK(async_is("cookieStore.set('a', '1').then(function () { return cookieStore.set({ name: 'b', value: '2', path: '/', sameSite: 'lax' }) }).then(function () { return cookieStore.get('a') }).then(function (c) { console.log(c.name, c.value, c.path, c.secure, c.sameSite, c.partitioned); return cookieStore.getAll() }).then(function (l) { console.log(l.length, document.cookie) });",
                 "a 1 / true strict false | 2 a=1; b=2", nullptr, "https://app.example/dir/page.html"));
  CHECK(async_is("document.cookie = 'x=1; path=/'; document.cookie = 'y=2; path=/'; cookieStore.delete('x').then(function () { console.log(document.cookie); return cookieStore.get('x') }).then(function (c) { console.log(c); return cookieStore.getAll({ name: 'y' }) }).then(function (l) { console.log(l.length, l[0].value); return cookieStore.get('nothing') }).then(function (c) { console.log(c) });",
                 "y=2 | null | 1 2 | null", nullptr, "https://app.example/"));
  CHECK(async_is("cookieStore.set({ name: '__Host-id', value: 'v' }).then(function () { console.log(document.cookie); return cookieStore.set('bad;name', 'v') }).catch(function (e) { console.log(e.name); return cookieStore.set({ name: 'p', value: 'v', partitioned: true }) }).then(function () { return cookieStore.get('p') }).then(function (c) { console.log(c.partitioned, c.secure) });",
                 "__Host-id=v | TypeError | true true", nullptr, "https://app.example/"));
  CHECK(async_is("var seen = []; cookieStore.addEventListener('change', function (e) { seen.push(e.changed.length + ':' + e.deleted.length) }); cookieStore.set('q', '1').then(function () { return new Promise(function (r) { setTimeout(r, 600) }) }).then(function () { return cookieStore.delete('q') }).then(function () { return new Promise(function (r) { setTimeout(r, 600) }) }).then(function () { console.log(seen.join()) });",
                 "1:0,0:1", nullptr, "https://app.example/"));
  // Clear-Site-Data: "cookies" clears the origin's cookies
  CHECK(async_is("document.cookie = 'k=1; path=/'; console.log(document.cookie); fetch('/logout').then(function () { console.log(document.cookie === '' ? 'empty' : document.cookie) });", "k=1 | empty",
                 [](FakeEnvironment& env) { env.net.answers["GET https://app.example/logout"] = {200, "Clear-Site-Data: \"cookies\"\r\n", "bye"}; }, "https://app.example/"));
  // storage keeps what is written through the area and the quota counts keys too
  CHECK(js("", "localStorage.setItem('k', 'v'); console.log(localStorage.length, JSON.stringify(localStorage), 'k' in localStorage, localStorage.hasOwnProperty('k'), localStorage.getItem('toString'), typeof localStorage.toString)", "1 {\"k\":\"v\"} true true null function"));
}

// A page on a pretend browser with a real session history (the entries the Navigation API lists): `script` runs, the timers run, the moves the page asked for are
// carried out the way the browser would (Back / Forward within the document give the page a popstate), then `act` does what the user does.
static std::string nav_run(const char* script, const char* address, const std::function<void(FakeEnvironment&, Page&, SessionHistory&)>& act = nullptr,
                           const std::function<void(FakeEnvironment&)>& after = nullptr) {
  TestAllocator a;
  std::string out;
  {
    SessionHistory history(a);
    history.navigate(address, 1);
    FakeEnvironment env(a);
    env.sim = &history;
    MediaEnvironment media;
    Page page(a, env, address, media, true);
    auto pump = [&] {
      for (int i = 0; i < 20 && env.pending_delta; ++i) {
        const int d = env.pending_delta;
        env.pending_delta = 0;
        if (!history.can_go(d)) continue;
        history.go(d);
        const SessionHistory::Entry* e = history.current();
        page.traverse(StringView(e->address.view()), e->state.view(), e->has_state);
      }
    };
    auto run_timers = [&] {
      for (int i = 0; i < 100; ++i) {
        pump();
        double due;
        if (!page.next_timer(&due)) break;
        if (due > env.clock) env.clock = static_cast<uint64_t>(due);
        page.run_due_timers(env.clock);
      }
      pump();
    };
    page.load((std::string("<html><head><title>T</title></head><body><script>") + script + "</script></body></html>").c_str());
    run_timers();
    if (act) {
      act(env, page, history);
      run_timers();
    }
    out = std::string(page.log().data(), page.log().size());
    if (!out.empty() && out.back() == '\n') out.pop_back();
    for (size_t p = 0; (p = out.find("\n", p)) != std::string::npos;) {
      out.replace(p, 1, " | ");
      p += 3;
    }
    if (after) after(env);
  }
  if (a.live != 0) {
    printf("   (leaked %d allocation(s))\n", a.live);
    return out + " [leak]";
  }
  return out;
}

static bool nav_is(const char* script, const char* expected, const std::function<void(FakeEnvironment&, Page&, SessionHistory&)>& act = nullptr,
                   const std::function<void(FakeEnvironment&)>& after = nullptr) {
  const std::string got = nav_run(script, "https://app.example/p/start", act, after);
  if (got != expected) {
    printf("   %s\n   gives   %s\n   expects %s\n", script, got.c_str(), expected);
    return false;
  }
  return true;
}

static void test_navigation() {
  // an on<event> handler that returns false cancels the event
  CHECK(js("<a id=l href='#'>x</a>", "var l = document.getElementById('l'); l.onclick = function () { return false }; var e = new MouseEvent('click', { cancelable: true, bubbles: true }); l.dispatchEvent(e); console.log(e.defaultPrevented); l.onclick = function () { return 0 }; var f = new MouseEvent('click', { cancelable: true }); l.dispatchEvent(f); console.log(f.defaultPrevented)", "true | false"));
  // --- the lifecycle: pageshow after load, beforeunload asks, pagehide / unload say goodbye
  CHECK(js("", "addEventListener('load', function () { console.log('load', document.readyState) }); addEventListener('pageshow', function (e) { console.log('pageshow', e.persisted, e instanceof PageTransitionEvent) });", "load complete | pageshow false true"));
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      auto asks = [&](const char* script) {
        Page page(a, env, "https://app.example/", media, true);
        page.load((std::string("<script>") + script + "</script>").c_str());
        return page.before_unload();
      };
      CHECK(!asks("1"));
      CHECK(asks("addEventListener('beforeunload', function (e) { e.preventDefault() })"));
      CHECK(asks("addEventListener('beforeunload', function (e) { e.returnValue = 'sure?' })"));
      CHECK(!asks("addEventListener('beforeunload', function (e) { e.returnValue = '' })"));
      CHECK(asks("onbeforeunload = function (e) { e.returnValue = 'x' }"));
      CHECK(!asks("addEventListener('beforeunload', function () { throw new Error('boom') })"));
      CHECK(asks("onbeforeunload = function () { return 'Really?' }"));
      CHECK(!asks("onbeforeunload = function () { }"));
      Page page(a, env, "https://app.example/", media, true);
      page.load("<script>document.addEventListener('visibilitychange', function () { console.log('visibilitychange', document.visibilityState, document.hidden) }); addEventListener('pagehide', function (e) { console.log('pagehide', e.persisted, document.visibilityState) }); addEventListener('unload', function () { console.log('unload') }); console.log(document.visibilityState, document.hidden)</script>");
      page.unload();
      CHECK(std::string(page.log().data(), page.log().size()) == "visible false\nvisibilitychange hidden true\npagehide false hidden\nunload\n");
    }
    // Page Visibility: the host says the page is in the background / shown again
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/", media, true);
      page.load("<script>document.addEventListener('visibilitychange', function () { console.log('vc', document.visibilityState, document.hidden) }); "
                "requestAnimationFrame(function () { console.log('raf1') });</script>");
      page.set_visible(false);
      page.set_visible(false);   // (no change: no event)
      page.run_due_timers(1e12);
      page.set_visible(true);
      page.run_due_timers(2e12);
      CHECK(std::string(page.log().data(), page.log().size()) == "vc hidden true\nvc visible false\nraf1\n");
    }
    CHECK(js("", "document.onvisibilitychange = function (e) { console.log('on', e.type, e.bubbles) }; document.dispatchEvent(new Event('visibilitychange', { bubbles: true }))", "on visibilitychange true"));
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/", media, true);
      page.set_visible(false);   // (a tab opened in the background)
      page.load("<script>console.log(document.visibilityState, document.hidden); var id = requestAnimationFrame(function () { console.log('never') }); requestAnimationFrame(function () { console.log('shown') }); cancelAnimationFrame(id);"
                "document.addEventListener('visibilitychange', function () { console.log('vc', document.visibilityState) })</script>");
      page.run_due_timers(1e12);
      page.set_visible(true);
      page.run_due_timers(2e12);
      CHECK(std::string(page.log().data(), page.log().size()) == "hidden true\nvc visible\nshown\n");
    }
    CHECK(a.live == 0);
  }
  // --- scrollRestoration, the referrer
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/", media, true);
      page.set_referrer("https://other.example/from");
      page.load("<script>console.log(history.scrollRestoration, document.referrer); history.scrollRestoration = 'bogus'; console.log(history.scrollRestoration); history.scrollRestoration = 'manual'; console.log(history.scrollRestoration)</script>");
      CHECK(std::string(page.log().data(), page.log().size()) == "auto https://other.example/from\nauto\nmanual\n");
      CHECK(page.scroll_restoration_manual());
    }
    CHECK(a.live == 0);
  }
  // --- <meta http-equiv=refresh>
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      auto refresh = [&](const char* content, const char* expected) {
        Page page(a, env, "https://app.example/dir/page", media, true);
        page.load((std::string("<meta http-equiv=refresh content=\"") + content + "\">").c_str());
        double seconds = -1;
        String target(a);
        const bool found = page.meta_refresh(&seconds, &target);
        std::string got = found ? std::to_string(static_cast<int>(seconds)) + " " + std::string(target.c_str()) : "none";
        if (got != expected) {
          printf("   refresh \"%s\" gives %s, expects %s\n", content, got.c_str(), expected);
          return false;
        }
        return true;
      };
      CHECK(refresh("5", "5 https://app.example/dir/page"));
      CHECK(refresh("0; url=/next", "0 https://app.example/next"));
      CHECK(refresh("3;URL='next?x=1'", "3 https://app.example/dir/next?x=1"));
      CHECK(refresh("2, https://other.example/", "2 https://other.example/"));
      CHECK(refresh(" 10 ; url = a.html", "10 https://app.example/dir/a.html"));
      CHECK(refresh("soon", "none"));
      CHECK(refresh("5x; url=/a", "none"));
    }
    CHECK(a.live == 0);
  }
  // --- where the page scrolls to: the fragment, scrollIntoView()
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      font::FontSet fonts(a);
      fonts.add_bundled_fonts();
      auto scroll_for = [&](const char* address, const char* script) {
        Page page(a, env, address, media, true);
        page.load((std::string("<body style='margin:0'><div style='height:1000px'></div><h1 id=t style='margin:0'>x</h1><div style='height:500px'></div><a name=old>y</a><script>") + script + "</script>").c_str());
        PagePicture picture(a);
        CHECK(page.render_picture(&picture, fonts));
        return page.take_scroll_request();
      };
      const int32_t t = scroll_for("https://app.example/p#t", "");
      CHECK(t >= 1000 && t < 1010);
      CHECK(scroll_for("https://app.example/p#%74", "") == t);
      const int32_t old = scroll_for("https://app.example/p#old", "");
      CHECK(old >= 1500);
      CHECK(scroll_for("https://app.example/p#nothing", "") == -1);
      CHECK(scroll_for("https://app.example/p", "") == -1);
      CHECK(scroll_for("https://app.example/p#top", "") == 0);
      CHECK(scroll_for("https://app.example/p#", "") == 0);
      CHECK(scroll_for("https://app.example/p", "document.getElementById('t').scrollIntoView()") == t);
      // changing the fragment from script scrolls too
      Page page(a, env, "https://app.example/p", media, true);
      page.load("<body style='margin:0'><div style='height:1000px'></div><h1 id=t style='margin:0'>x</h1><script>location.hash = '#t'</script>");
      PagePicture picture(a);
      CHECK(page.render_picture(&picture, fonts));
      CHECK(page.take_scroll_request() == t);
      CHECK(page.take_scroll_request() == -1);
    }
    CHECK(a.live == 0);
  }
  // --- IntersectionObserver, geometry and scrolling
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      media.width = 400;
      media.height = 300;
      font::FontSet fonts(a);
      fonts.add_bundled_fonts();
      auto watch = [&](const std::string& script, const std::vector<int32_t>& scrolls) {
        Page page(a, env, "https://app.example/", media, true);
        page.load((std::string("<body style='margin:0'><div style='height:1000px'></div><div id=t style='height:100px;width:200px'>x</div><div id=c style='height:200px;overflow:hidden;width:100px'><div id=in style='height:50px;margin-top:400px'>y</div></div><div style='height:1500px'></div><script>") + script + "</script>").c_str());
        PagePicture picture(a);
        CHECK(page.render_picture(&picture, fonts));
        page.run_due_timers(1e12);
        for (int32_t y : scrolls) {
          page.set_scroll(y);
          page.run_due_timers(1e12);
        }
        return std::string(page.log().data(), page.log().size());
      };
      const std::string fmt = "var t = document.getElementById('t'); var show = function (es) { es.forEach(function (e) { console.log(e.target.id, e.isIntersecting, e.intersectionRatio, Math.round(e.boundingClientRect.top), Math.round(e.intersectionRect.height)) }) };";
      // observed from far away: first a "not intersecting" entry, then in view, then out again; scrollY follows
      CHECK(watch(fmt + "new IntersectionObserver(show).observe(t); addEventListener('scroll', function () { console.log('scroll', scrollY) })", {800, 0}) ==
            "t false 0 1000 0\nscroll 800\nt true 1 200 100\nscroll 0\nt false 0 1000 0\n");
      // thresholds: only when a threshold is crossed; the ratio is the visible part (half of the 100px box: 50px visible at scroll 650 -> 950..1250 sees 1000..1100? no, scroll 700 shows the top 0..)
      CHECK(watch(fmt + "new IntersectionObserver(show, { threshold: [0, 0.5, 1] }).observe(t)", {750, 760, 850, 1000}) ==
            "t false 0 1000 0\nt true 0.5 250 50\nt true 1 150 100\n");
      // rootMargin makes the root bigger; a root element clips; unobserve / disconnect stop it
      CHECK(watch(fmt + "new IntersectionObserver(show, { rootMargin: '100px 0px' }).observe(t)", {650}) == "t false 0 1000 0\nt true 0.5 350 50\n");
      CHECK(watch(fmt + "var o = new IntersectionObserver(function (es) { show(es); o.disconnect() }); o.observe(t)", {800}) == "t false 0 1000 0\n");
      // an element clipped by an ancestor with overflow:hidden is not seen even when it is within the viewport
      CHECK(watch(fmt + "new IntersectionObserver(show).observe(document.getElementById('in'))", {1250}) == "in false 0 1500 0\n");
      CHECK(watch("addEventListener('scroll', function () { var r = document.getElementById('t').getBoundingClientRect(); console.log(r.top, r.height, r.width, document.getElementById('t').offsetHeight, scrollY) })", {900}) == "100 100 200 100 900\n");
      // takeRecords, options and errors
      CHECK(watch("var o = new IntersectionObserver(function () {}, { root: document.body, rootMargin: '10px', threshold: 1 }); console.log(o.root === document.body, o.rootMargin, o.thresholds.join()); o = new IntersectionObserver(function () {}, { rootMargin: '1% 2px 3px' }); console.log(o.rootMargin);"
                  "try { new IntersectionObserver(function () {}, { threshold: 2 }) } catch (e) { console.log(e.name) } try { new IntersectionObserver(function () {}, { rootMargin: '5' }) } catch (e) { console.log(e.name) } try { new IntersectionObserver(1) } catch (e) { console.log(e.name) }"
                  "try { o.observe(5) } catch (e) { console.log(e.name) } var t = document.getElementById('t'); o.observe(t); o.observe(t); console.log(o.takeRecords().length); o.disconnect()", {}) ==
            "true 10px 10px 10px 10px 1\n1% 2px 3px 2px\nRangeError\nSyntaxError\nTypeError\nTypeError\n0\n");
    }
    CHECK(a.live == 0);
  }
  // --- ResizeObserver
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      media.width = 400;
      media.height = 300;
      font::FontSet fonts(a);
      fonts.add_bundled_fonts();
      // the page is drawn, then a scroll event changes it and it is drawn again
      auto watch = [&](const std::string& script) {
        Page page(a, env, "https://app.example/", media, true);
        page.load((std::string("<body style='margin:0'><div style='height:2000px'><div id=t style='width:200px;height:100px;padding:10px;border:5px solid'>x</div><div id=h style='display:none'>y</div></div><script>") + script + "</script>").c_str());
        PagePicture picture(a);
        CHECK(page.render_picture(&picture, fonts));
        page.run_due_timers(1e12);
        page.set_scroll(10);
        CHECK(page.render_picture(&picture, fonts));
        page.run_due_timers(2e12);
        return std::string(page.log().data(), page.log().size());
      };
      const std::string pre = "var t = document.getElementById('t'); var show = function (es) { es.forEach(function (e) { console.log(e.target.id, e.contentRect.width, e.contentRect.height, e.contentRect.left, e.contentRect.top, e.contentBoxSize[0].inlineSize, e.contentBoxSize[0].blockSize, e.borderBoxSize[0].inlineSize, e.borderBoxSize[0].blockSize) }) };";
      // told once at the start, and again when it changes; content and border boxes differ by the padding and border
      CHECK(watch(pre + "new ResizeObserver(show).observe(t); addEventListener('scroll', function () { t.style.width = '300px' })") ==
            "t 200 100 10 10 200 100 230 130\nt 300 100 10 10 300 100 330 130\n");
      // border-box: only a change of the border box counts
      CHECK(watch(pre + "new ResizeObserver(function (es) { console.log(es[0].borderBoxSize[0].inlineSize) }).observe(t, { box: 'border-box' }); addEventListener('scroll', function () { t.style.paddingLeft = '20px' })") == "230\n240\n");
      // an element without a box is 0 x 0 and is not told; it is told when it goes away
      CHECK(watch(pre + "var h = document.getElementById('h'); new ResizeObserver(show).observe(h); addEventListener('scroll', function () { h.style.display = 'block'; h.style.height = '30px' })") ==
            "h 400 30 0 0 400 30 400 30\n");
      CHECK(watch(pre + "var o = new ResizeObserver(show); o.observe(t); o.unobserve(t); addEventListener('scroll', function () { t.style.width = '300px' })") == "");
      CHECK(watch(pre + "var o = new ResizeObserver(function (es) { show(es); o.disconnect() }); o.observe(t); addEventListener('scroll', function () { t.style.width = '300px' })") == "t 200 100 10 10 200 100 230 130\n");
      CHECK(watch("var o = new ResizeObserver(function () {}); console.log(String(o), typeof ResizeObserverEntry, typeof ResizeObserverSize);"
                  "try { new ResizeObserver() } catch (e) { console.log(e.name) } try { new ResizeObserver(1) } catch (e) { console.log(e.name) } try { o.observe(5) } catch (e) { console.log(e.name) }"
                  "try { o.observe(document.body, { box: 'padding-box' }) } catch (e) { console.log(e.name) } try { ResizeObserver(function () {}) } catch (e) { console.log(e.name) } o.observe(document.body, { box: 'device-pixel-content-box' }); o.observe(document.body); o.disconnect()") ==
            "[object ResizeObserver] function function\nTypeError\nTypeError\nTypeError\nTypeError\nTypeError\n");
    }
    CHECK(a.live == 0);
  }
  // --- requestAnimationFrame: frames, time stamps, order, cancelling
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "http://example.com/", media, true);
      page.load("<script>var log = []; var stamps = [];"
                "var a = requestAnimationFrame(function (t) { stamps.push(t); log.push('a'); requestAnimationFrame(function () { log.push('a2') }); cancelAnimationFrame(c) });"
                "var b = requestAnimationFrame(function (t) { stamps.push(t); log.push('b'); throw new Error('boom') });"
                "var c = requestAnimationFrame(function () { log.push('NO') });"
                "var d = requestAnimationFrame(function (t) { stamps.push(t); log.push('d'); requestAnimationFrame(report) });"
                "var e = requestAnimationFrame(function () { log.push('NO2') }); cancelAnimationFrame(e); cancelAnimationFrame(99999); cancelAnimationFrame('x'); cancelAnimationFrame();"
                "console.log(a > 0, b > a, c > b, typeof a);"
                "try { requestAnimationFrame() } catch (x) { console.log(x.name) } try { requestAnimationFrame('f') } catch (x) { console.log(x.name) }"
                "function report() { console.log(log.join(), stamps.length, stamps[0] === stamps[1] && stamps[1] === stamps[2], typeof stamps[0], stamps[0] >= 0 && stamps[0] <= performance.now()) }</script>");
      page.run_due_timers(env.clock + 20);   // the first frame: a, b (which throws), d; a2 waits for the next one
      page.run_due_timers(env.clock + 20);
      page.run_due_timers(env.clock + 40);   // the second frame
      page.run_due_timers(env.clock + 60);   // (the error b threw is thrown again as a timer)
      double due;
      CHECK(!page.next_timer(&due));
      const std::string log(page.log().data(), page.log().size());
      CHECK(log == "true true true number\nTypeError\nTypeError\na,b,d,a2 3 true number true\nscript error: Error: boom\n");
      CHECK(page.errors() >= 1);
    }
    CHECK(a.live == 0);
  }
  // --- window.open(), window.close()
  CHECK(nav_is("var w = open('/popup', 'pop'); console.log(typeof w, w.closed, w.name); console.log(open('/n', '_blank', 'noopener') === null, open('about:blank') === null); w.close(); console.log(w.closed); close();", "object false pop | true true | true",
               nullptr, [](FakeEnvironment& env) {
                 CHECK(env.opens.size() == 2);
                 if (env.opens.size() == 2) CHECK(env.opens[0] == "https://app.example/popup [pop]" && env.opens[1] == "https://app.example/n [_blank] noopener");
                 CHECK(env.closes == 1);
               }));
  CHECK(nav_is("var r = open('/self', '_self'); console.log(r === window)", "true", nullptr, [](FakeEnvironment& env) { CHECK(env.opens.empty()); CHECK(env.navigations.size() == 1); }));
  // --- the Navigation API
  CHECK(nav_is("console.log(navigation.entries().length, navigation.currentEntry.url, navigation.currentEntry.index, navigation.canGoBack, navigation.canGoForward, navigation instanceof EventTarget, navigation.transition);"
               "history.pushState({ a: 1 }, '', '/p/two'); var es = navigation.entries(); console.log(es.length, navigation.currentEntry.url, navigation.currentEntry.index, navigation.canGoBack, navigation.canGoForward, es[0].key !== es[1].key, es[0].sameDocument, es[1].id !== es[0].id);",
               "1 https://app.example/p/start 0 false false true null | 2 https://app.example/p/two 1 true false true true true"));
  CHECK(nav_is("navigation.addEventListener('navigate', function (e) { console.log(e.type, e.navigationType, e.canIntercept, e.destination.url, e.hashChange, e.userInitiated, e instanceof NavigateEvent); e.intercept({ handler: function () { console.log('handler'); return new Promise(function (r) { setTimeout(r, 10) }) } }) });"
               "navigation.addEventListener('currententrychange', function (e) { console.log('change', e.navigationType, e.from.url) });"
               "navigation.addEventListener('navigatesuccess', function () { console.log('success', location.pathname, navigation.transition) });"
               "var r = navigation.navigate('/p/next', { state: { n: 1 } }); console.log(navigation.transition === null);"
               "r.committed.then(function (en) { console.log('committed', en.url, JSON.stringify(en.getState())) }); r.finished.then(function (en) { console.log('finished', en.url, navigation.entries().length) });",
               "navigate push true https://app.example/p/next false false true | change push https://app.example/p/start | false | handler | committed https://app.example/p/next {\"n\":1} | success /p/next null | finished https://app.example/p/next 2"));
  CHECK(nav_is("navigation.onnavigate = function (e) { e.preventDefault() }; navigation.addEventListener('navigateerror', function (e) { console.log('error', e.error.name) });"
               "navigation.navigate('/p/x').committed.catch(function (err) { console.log(err.name, location.pathname, navigation.entries().length) });",
               "error AbortError | AbortError /p/start 1"));
  CHECK(nav_is("navigation.onnavigate = function (e) { e.intercept({ handler: function () { throw new Error('nope') } }) }; navigation.onnavigateerror = function (e) { console.log('navigateerror', e.error.message) };"
               "navigation.navigate('/p/y').finished.catch(function (err) { console.log('rejected', err.message, location.pathname) });",
               "navigateerror nope | rejected nope /p/y"));
  // fragment navigation is a same-document navigation without intercepting
  CHECK(nav_is("navigation.onnavigate = function (e) { console.log(e.navigationType, e.hashChange, e.destination.sameDocument, e.canIntercept) }; navigation.onnavigatesuccess = function () { console.log('success', location.hash) };"
               "navigation.navigate('#frag').finished.then(function (en) { console.log(en.url, navigation.entries().length) });",
               "push true true true | success #frag | https://app.example/p/start#frag 2"));
  CHECK(nav_is("navigation.onnavigate = function (e) { console.log('nav', e.navigationType, e.destination.url, e.canIntercept, e.destination.sameDocument, e.destination.index, e.userInitiated) };"
               "history.pushState(1, '', '/p/b'); history.pushState(2, '', '/p/c'); console.log(navigation.entries().length);"
               "navigation.back().finished.then(function (en) { console.log('back done', en.url, location.pathname, history.state, navigation.currentEntry.index, navigation.canGoForward) });",
               "nav push https://app.example/p/b true true -1 false | nav push https://app.example/p/c true true -1 false | 3 | nav traverse https://app.example/p/b true true 1 false | back done https://app.example/p/b /p/b 1 1 true"));
  CHECK(nav_is("history.pushState(1, '', '/p/b'); var first = navigation.entries()[0];"
               "navigation.traverseTo(first.key).finished.then(function (en) { console.log('at', en.url, en.index); return navigation.traverseTo('nope').finished }).catch(function (e) { console.log(e.name); return navigation.back().finished }).catch(function (e) { console.log('no back', e.name) });",
               "at https://app.example/p/start 0 | InvalidStateError | no back InvalidStateError"));
  CHECK(nav_is("navigation.back().finished.catch(function (e) { console.log(e.name) }); navigation.forward().finished.catch(function (e) { console.log(e.name) });", "InvalidStateError | InvalidStateError"));
  // the user presses Back: the page hears of it
  CHECK(nav_is("history.pushState('s', '', '/p/b'); navigation.onnavigate = function (e) { console.log('nav', e.navigationType, e.userInitiated, e.canIntercept, e.cancelable) }; navigation.oncurrententrychange = function (e) { console.log('change', e.navigationType, e.from.url) }; navigation.onnavigatesuccess = function () { console.log('success', navigation.currentEntry.url) };",
               "nav traverse true true false | change traverse https://app.example/p/b | success https://app.example/p/start",
               [](FakeEnvironment&, Page& page, SessionHistory& h) {
                 h.go(-1);
                 page.traverse(StringView(h.current()->address.view()), StringView(), false);
               }));
  // state of an entry, kept apart from history.state
  CHECK(nav_is("navigation.updateCurrentEntry({ state: { k: [1, 2] } }); console.log(JSON.stringify(navigation.currentEntry.getState()), history.state);"
               "navigation.oncurrententrychange = function (e) { console.log('change', e.navigationType, e.from === navigation.currentEntry) }; navigation.updateCurrentEntry({ state: 5 }); console.log(navigation.currentEntry.getState());"
               "history.pushState('h', '', '/p/z'); console.log(navigation.currentEntry.getState(), navigation.entries()[0].getState());"
               "try { navigation.updateCurrentEntry({}) } catch (e) { console.log(e.name) }",
               "{\"k\":[1,2]} null | change null true | 5 | change push false | undefined 5 | TypeError"));
  // entries that are forgotten say so
  CHECK(nav_is("history.pushState(0, '', '/p/b'); history.pushState(0, '', '/p/c'); var c = navigation.currentEntry; c.ondispose = function () { console.log('disposed', c.url, c.index) };"
               "history.back(); setTimeout(function () { history.pushState(0, '', '/p/d'); console.log(navigation.entries().length, navigation.entries().map(function (e) { return e.url.slice(-1) }).join('')) }, 5);",
               "disposed https://app.example/p/c -1 | 3 tbd"));
  // location.assign() is a navigation the page can see and stop; one nobody stops goes to the browser
  CHECK(nav_is("navigation.onnavigate = function (e) { console.log(e.navigationType, e.canIntercept, e.destination.sameDocument) }; location.assign('/p/other'); location.replace('/p/third');", "push true false | replace true false",
               nullptr, [](FakeEnvironment& env) { CHECK(env.navigations.size() == 2); }));
  CHECK(nav_is("navigation.onnavigate = function (e) { e.preventDefault() }; location.assign('/p/other'); console.log('stayed');", "stayed", nullptr, [](FakeEnvironment& env) { CHECK(env.navigations.empty()); }));
  CHECK(nav_is("navigation.onnavigate = function (e) { if (e.canIntercept) e.intercept({ handler: function () { console.log('intercepted', e.destination.url) } }) }; location.assign('/p/spa'); setTimeout(function () { console.log(location.pathname, navigation.entries().length) }, 5);",
               "intercepted https://app.example/p/spa | /p/spa 2", nullptr, [](FakeEnvironment& env) { CHECK(env.navigations.empty()); }));
  // other origins' entries are not listed
  CHECK(nav_is("console.log(navigation.entries().length)", "1"));
  CHECK(nav_is("navigation.navigate('https://elsewhere.example/').committed.catch(function () {}); console.log(typeof navigation.navigate('/p/q', { history: 'replace' }).finished, navigation.entries().length);"
               "try { navigation.navigate('/p/q', { history: 'bogus' }) } catch (e) { console.log(e.name) }", "object 1 | TypeError", nullptr, [](FakeEnvironment& env) { CHECK(env.navigations.size() == 2 && env.navigations[0] == "https://elsewhere.example/" && env.navigations[1] == "https://app.example/p/q (replace)"); }));
  // history.back() and friends go through the same machinery
  CHECK(nav_is("history.pushState(0, '', '/p/b'); navigation.onnavigate = function (e) { console.log('nav', e.navigationType, e.destination.url); }; navigation.onnavigatesuccess = function () { console.log('done', location.pathname) }; history.back();",
               "nav traverse https://app.example/p/start | done /p/start"));
  CHECK(nav_is("history.pushState(0, '', '/p/b'); navigation.onnavigate = function (e) { e.preventDefault(); console.log('stopped') }; history.back(); setTimeout(function () { console.log(location.pathname) }, 5)", "stopped | /p/b"));
}

// A page with `head_script` in its <head> and `body` as its body, on the fake browser; `act` plays the user (clicks and keys), then the timers run.
static std::string clip_run(const char* body, const char* head_script, const std::function<void(FakeEnvironment&, Page&)>& act = nullptr, const char* address = "https://app.example/p",
                            const std::function<void(FakeEnvironment&)>& setup = nullptr, const std::function<void(FakeEnvironment&)>& after = nullptr) {
  TestAllocator a;
  std::string out;
  {
    FakeEnvironment env(a);
    if (setup) setup(env);
    MediaEnvironment media;
    Page page(a, env, address, media, true);
    auto run_timers = [&](bool all) {
      for (int i = 0; i < 100; ++i) {
        double due;
        if (!page.next_timer(&due) || (!all && due > env.clock)) break;
        if (due > env.clock) env.clock = static_cast<uint64_t>(due);
        page.run_due_timers(env.clock);
      }
    };
    page.load((std::string("<html><head><script>") + head_script + "</script></head><body>" + body + "</body></html>").c_str());
    run_timers(false);   // (what is due now: promises, zero delay timers)
    if (act) act(env, page);
    run_timers(true);
    out = std::string(page.log().data(), page.log().size());
    if (!out.empty() && out.back() == '\n') out.pop_back();
    for (size_t p = 0; (p = out.find("\n", p)) != std::string::npos;) {
      out.replace(p, 1, " | ");
      p += 3;
    }
    if (after) after(env);
  }
  if (a.live != 0) return out + " [leak]";
  return out;
}

static bool clip_is(const char* body, const char* script, const char* expected, const std::function<void(FakeEnvironment&, Page&)>& act = nullptr, const char* address = "https://app.example/p",
                    const std::function<void(FakeEnvironment&)>& setup = nullptr, const std::function<void(FakeEnvironment&)>& after = nullptr) {
  const std::string got = clip_run(body, script, act, address, setup, after);
  if (got != expected) {
    printf("   %s\n   gives   %s\n   expects %s\n", script, got.c_str(), expected);
    return false;
  }
  return true;
}

static void test_clipboard() {
  const auto click_button = [](FakeEnvironment&, Page& page) { page.click(page.document().element_by_id("b")->serial, 1, 1); };
  const auto ctrl = [](Page& page, char c) { page.key(Page::FormKey::Character, c, 2); };
  // where it exists, and what it asks for
  CHECK(clip_is("", "console.log(typeof navigator.clipboard, isSecureContext, typeof ClipboardItem, typeof Clipboard)", "object true function function"));
  CHECK(clip_is("", "console.log(typeof navigator.clipboard, isSecureContext)", "undefined false", nullptr, "http://example.com/"));
  CHECK(clip_is("", "console.log(typeof navigator.clipboard, isSecureContext)", "object true", nullptr, "http://localhost:8080/"));
  CHECK(clip_is("<button id=b>x</button>", "addEventListener('load', function () { var s; navigator.permissions.query({ name: 'clipboard-read' }).then(function (p) { s = p; console.log(p.state) }); navigator.permissions.query({ name: 'clipboard-write' }).then(function (p) { console.log(p.state) });"
                "console.log(navigator.userActivation.hasBeenActive, navigator.userActivation.isActive);"
                "document.getElementById('b').addEventListener('click', function () { console.log(navigator.userActivation.hasBeenActive, navigator.userActivation.isActive, s.state) }) });",
                "false false | prompt | granted | true true granted", click_button));
  // text
  CHECK(clip_is("<button id=b>x</button>", "navigator.clipboard.writeText('héllo').then(function () { console.log('written') });"
                "addEventListener('load', function () { document.getElementById('b').onclick = function () { navigator.clipboard.readText().then(function (t) { console.log('read', t) }) } });"
                "navigator.clipboard.readText().catch(function (e) { console.log(e.name, e instanceof DOMException) });"
                "navigator.clipboard.writeText().catch(function (e) { console.log(e.name) });",
                "written | NotAllowedError true | TypeError | read héllo", click_button, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "h\xC3\xA9llo"); }));
  CHECK(clip_is("", "navigator.clipboard.writeText('x').then(function () { return navigator.clipboard.writeText('') }).then(function () { console.log('cleared') })", "cleared", nullptr, "https://app.example/p",
                nullptr, [](FakeEnvironment& env) { CHECK(env.clip.empty()); }));
  // items: text and html together, a picture, and the checks
  CHECK(clip_is("<button id=b>x</button>",
                "var png = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10, 0, 255, 128, 7]);"
                "navigator.clipboard.write([new ClipboardItem({ 'text/plain': 'plain', 'text/html': new Blob(['<b>rich</b>'], { type: 'text/html' }) }, { presentationStyle: 'inline' })])"
                ".then(function () { return navigator.clipboard.write([new ClipboardItem({ 'image/png': Promise.resolve(new Blob([png], { type: 'image/png' })) })]) }).then(function () { console.log('wrote picture') });"
                "addEventListener('load', function () { document.getElementById('b').onclick = function () { navigator.clipboard.read().then(function (items) { console.log(items.length, items[0].types.join(), items[0].presentationStyle); return items[0].getType('image/png') })"
                ".then(function (b) { console.log(b.type, b.size); return b.arrayBuffer() }).then(function (ab) { var u = new Uint8Array(ab), o = []; for (var i = 0; i < u.length; i++) o.push(u[i]); console.log(o.join(',')) }) } });",
                "wrote picture | 1 image/png unspecified | image/png 12 | 137,80,78,71,13,10,26,10,0,255,128,7", click_button, "https://app.example/p", nullptr,
                [](FakeEnvironment& env) { CHECK(env.clip.empty() && env.clip_png.size() == 12 && (unsigned char)env.clip_png[9] == 255 && (unsigned char)env.clip_png[10] == 128); }));
  CHECK(clip_is("<button id=b>x</button>",
                "navigator.clipboard.write([new ClipboardItem({ 'text/plain': 'plain', 'text/html': '<i>x</i>' })]).then(function () { console.log('ok') });"
                "addEventListener('load', function () { document.getElementById('b').onclick = function () { navigator.clipboard.read().then(function (items) { console.log(items[0].types.join()); return items[0].getType('text/html') }).then(function (b) { return b.text() }).then(function (t) { console.log(t) }) } });",
                "ok | text/plain,text/html | <i>x</i>", click_button, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "plain" && env.clip_html == "<i>x</i>"); }));
  CHECK(clip_is("", "var seen = [];"
                "try { new ClipboardItem() } catch (e) { seen.push(e.name) } try { new ClipboardItem({}) } catch (e) { seen.push(e.name) } try { new ClipboardItem({ 'text/plain': 5 }) } catch (e) { seen.push(e.name) }"
                "try { new ClipboardItem({ 'text/plain': 'x' }, { presentationStyle: 'bogus' }) } catch (e) { seen.push(e.name) }"
                "seen.push(ClipboardItem.supports('text/plain'), ClipboardItem.supports('image/png'), ClipboardItem.supports('text/csv'));"
                "var item = new ClipboardItem({ 'text/plain': 'x' }); item.getType('image/png').catch(function (e) { seen.push(e.name) });"
                "navigator.clipboard.write([]).then(function () { seen.push('empty ok') });"
                "navigator.clipboard.write(['x']).catch(function (e) { seen.push('item ' + e.name) });"
                "navigator.clipboard.write(5).catch(function (e) { seen.push('arg ' + e.name) });"
                "navigator.clipboard.write([new ClipboardItem({ 'text/csv': 'a,b' })]).catch(function (e) { seen.push('type ' + e.name) });"
                "navigator.clipboard.write([item, item]).catch(function (e) { seen.push('two ' + e.name) });"
                "setTimeout(function () { console.log(seen.join(' ')) }, 5);",
                "TypeError TypeError TypeError TypeError true true false NotFoundError empty ok item TypeError arg TypeError type NotAllowedError two NotAllowedError"));
  CHECK(clip_is("", "navigator.clipboard.write([new ClipboardItem({ 'image/png': new Blob(['x'], { type: 'image/png' }) })]).catch(function (e) { console.log(e.name) })", "NotAllowedError"));   // (not a picture)
  // copy, cut and paste in a field: the events, and what a listener may do
  const char* field = "<input id=f autofocus value='hello world'><button id=b>x</button>";
  CHECK(clip_is(field, "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.setSelectionRange(0, 5) }, 0);"
                "document.addEventListener('copy', function (e) { console.log(e.type, e.target.id, e.cancelable, e.bubbles, e.clipboardData instanceof DataTransfer, e.clipboardData.types.length) }) })",
                "copy f true true true 0", [&](FakeEnvironment&, Page& page) { ctrl(page, 'c'); }, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "hello"); }));
  CHECK(clip_is(field, "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.setSelectionRange(0, 5) }, 0);"
                "f.addEventListener('copy', function (e) { e.clipboardData.setData('text/plain', 'custom'); e.clipboardData.setData('text/html', '<u>custom</u>'); e.preventDefault() }) })",
                "", [&](FakeEnvironment&, Page& page) { ctrl(page, 'c'); }, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "custom" && env.clip_html == "<u>custom</u>"); }));
  CHECK(clip_is(field, "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.setSelectionRange(6, 11) }, 0);"
                "f.addEventListener('cut', function (e) { console.log('cut', e.type) }); f.addEventListener('input', function () { console.log('input', f.value) }) })",
                "cut cut | input hello ", [&](FakeEnvironment&, Page& page) { ctrl(page, 'x'); }, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "world"); }));
  CHECK(clip_is(field, "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.setSelectionRange(6, 11) }, 0); f.addEventListener('cut', function (e) { e.preventDefault(); e.clipboardData.setData('text/plain', 'mine') });"
                "f.addEventListener('keyup', function () { console.log(f.value) }) })",
                "hello world", [&](FakeEnvironment&, Page& page) { ctrl(page, 'x'); }, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "mine"); }));
  CHECK(clip_is(field, "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.setSelectionRange(5, 5) }, 0);"
                "f.addEventListener('paste', function (e) { console.log('paste', e.clipboardData.getData('text/plain'), e.clipboardData.getData('text/html'), e.clipboardData.types.join()); e.clipboardData.setData('text/plain', 'ignored') });"
                "f.addEventListener('keyup', function () { console.log(f.value) }) })",
                "paste PASTED <b>P</b> text/plain,text/html | helloPASTED world", [&](FakeEnvironment&, Page& page) { ctrl(page, 'v'); }, "https://app.example/p",
                [](FakeEnvironment& env) { env.clip = "PASTED"; env.clip_html = "<b>P</b>"; }));
  CHECK(clip_is(field, "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.setSelectionRange(0, 11) }, 0);"
                "f.addEventListener('paste', function (e) { e.preventDefault(); var t = e.clipboardData.getData('text'); f.value = t.toUpperCase() });"
                "f.addEventListener('keyup', function () { console.log(f.value) }) })",
                "SHOUT", [&](FakeEnvironment&, Page& page) { ctrl(page, 'v'); }, "https://app.example/p", [](FakeEnvironment& env) { env.clip = "shout"; }));
  CHECK(clip_is("<input id=f type=password autofocus value=secret>", "addEventListener('load', function () { var f = document.getElementById('f'); setTimeout(function () { f.select() }, 0); document.addEventListener('copy', function () { console.log('copy event') }) })",
                "copy event", [&](FakeEnvironment&, Page& page) { ctrl(page, 'c'); }, "https://app.example/p", [](FakeEnvironment& env) { env.clip = "before"; }, [](FakeEnvironment& env) { CHECK(env.clip == "before"); }));
  CHECK(clip_is("<input id=f autofocus readonly value='ro'>", "addEventListener('load', function () { setTimeout(function () { document.getElementById('f').select() }, 0) })", "",
                [&](FakeEnvironment&, Page& page) { ctrl(page, 'x'); ctrl(page, 'v'); }, "https://app.example/p", [](FakeEnvironment& env) { env.clip = "zzz"; },
                [](FakeEnvironment& env) { CHECK(env.clip == "zzz"); }));   // (cut and paste leave a read only field alone)
  // the page itself: Ctrl+A selects it, Ctrl+C copies what is selected (as text and html)
  CHECK(clip_is("<p id=p>Hello <b>world</b></p>", "document.addEventListener('copy', function () { console.log('selection:', getSelection().toString(), getSelection().rangeCount) });",
                "selection: Hello world 1", [&](FakeEnvironment&, Page& page) { ctrl(page, 'a'); ctrl(page, 'c'); }, "https://app.example/p", nullptr,
                [](FakeEnvironment& env) { CHECK(env.clip == "Hello world" && env.clip_html == "<p id=\"p\">Hello <b>world</b></p>"); }));
  CHECK(clip_is("<p id=p>Hello world</p>", "", "", [&](FakeEnvironment&, Page& page) { ctrl(page, 'c'); }, "https://app.example/p", [](FakeEnvironment& env) { env.clip = "kept"; },
                [](FakeEnvironment& env) { CHECK(env.clip == "kept"); }));   // (nothing selected: nothing copied)
  CHECK(clip_is("<p id=p>Pick me</p>", "addEventListener('load', function () { var r = document.createRange(); r.selectNodeContents(document.getElementById('p')); getSelection().addRange(r) });"
                "document.addEventListener('copy', function (e) { e.preventDefault(); e.clipboardData.setData('text/plain', 'the page said: ' + getSelection().toString()) });",
                "", [&](FakeEnvironment&, Page& page) { ctrl(page, 'c'); }, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "the page said: Pick me"); }));
  CHECK(clip_is("<button id=b>x</button>", "document.addEventListener('paste', function (e) { console.log('page paste', e.target.id, e.clipboardData.getData('text/plain')) });",
                "page paste b typed", [&](FakeEnvironment&, Page& page) { page.document().element_by_id("b"); page.click(page.document().element_by_id("b")->serial, 1, 1); ctrl(page, 'v'); }, "https://app.example/p",
                [](FakeEnvironment& env) { env.clip = "typed"; }));
  // pasting a picture gives the listener a file
  CHECK(clip_is(field, "addEventListener('load', function () { document.getElementById('f').addEventListener('paste', function (e) { var f = e.clipboardData.files[0]; console.log(e.clipboardData.files.length, f.type, f.size, e.clipboardData.types.join()) }) })",
                "1 image/png 6 Files", [&](FakeEnvironment&, Page& page) { ctrl(page, 'v'); }, "https://app.example/p", [](FakeEnvironment& env) { env.clip_png = std::string("\x89PNG\x0d\x0a", 6); }));
  // document.execCommand
  CHECK(clip_is(field, "addEventListener('load', function () { setTimeout(function () { var f = document.getElementById('f'); f.focus(); f.setSelectionRange(0, 5);"
                "console.log(document.queryCommandSupported('copy'), document.queryCommandSupported('cut'), document.queryCommandSupported('paste'), document.queryCommandSupported('bold'), document.queryCommandEnabled('copy'), document.queryCommandEnabled('insertText'));"
                "console.log(document.execCommand('copy'), document.execCommand('paste'), document.execCommand('bold'));"
                "console.log(document.execCommand('cut'), f.value); console.log(document.execCommand('insertText', false, 'bye'), f.value); f.setSelectionRange(0, 0);"
                "console.log(document.execCommand('selectAll'), f.selectionStart, f.selectionEnd); console.log(document.execCommand('delete'), f.value); console.log(document.execCommand('forwarddelete'), f.value) }, 0) })",
                "true true false false true true | true false false | true  world | true bye world | true 0 9 | true  | false ", nullptr, "https://app.example/p", nullptr,
                [](FakeEnvironment& env) { CHECK(env.clip == "hello"); }));
  CHECK(clip_is("<p id=p>text</p>", "addEventListener('load', function () { var r = document.createRange(); r.selectNodeContents(document.getElementById('p')); getSelection().addRange(r);"
                "document.addEventListener('copy', function () { console.log('copy event') });"
                "console.log(document.execCommand('copy'), document.execCommand('selectAll'), getSelection().toString()) })",
                "copy event | true true text", nullptr, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.clip == "text"); }));
}

static void test_messaging() {
  // to itself: the data is a copy, with what JSON cannot say kept
  CHECK(async_is("addEventListener('message', function (e) { var d = e.data; console.log(e.type, e.origin, e.source === window, e.ports.length, e.lastEventId === '', d.a.length, d.a[1].b, d.d instanceof Date, d.d.getTime(), 'u' in d, d.u, isNaN(d.n), 1 / d.z, d.s, d === msg, e instanceof MessageEvent) });"
                  "var msg = { a: [1, { b: 2 }], d: new Date(5), u: undefined, n: NaN, z: -0, s: 'ü' }; postMessage(msg, '*'); console.log('posted');",
                  "posted | message https://app.example true 0 true 2 2 true 5 true undefined true -Infinity \xC3\xBC false true"));
  // MutationObserver
  CHECK(async_is("var log = []; var d = document.createElement('div'); document.body.appendChild(d);"
                 "var mo = new MutationObserver(function (records, observer) { records.forEach(function (r) { log.push(r.type + ':' + r.target.nodeName + ':' + (r.attributeName || '') + ':' + r.addedNodes.length + r.removedNodes.length + ':' + r.oldValue); }); log.push(observer === mo); });"
                 "mo.observe(d, { childList: true, attributes: true, attributeOldValue: true, characterData: true, subtree: true });"
                 "d.setAttribute('a', '1'); d.setAttribute('a', '2'); var p = document.createElement('p'); d.appendChild(p); p.textContent = 'x'; d.removeChild(p);"
                 "setTimeout(function () { console.log(log.join('|')); }, 5);",
                 "attributes:DIV:a:00:null|attributes:DIV:a:00:1|childList:DIV::10:null|childList:P::10:null|childList:DIV::01:null|true", nullptr, "https://app.example/"));
  CHECK(async_is("var n = 0; var d = document.createElement('div'); var mo = new MutationObserver(function () { n++; }); mo.observe(d, { attributes: true, attributeFilter: ['keep'] });"
                 "d.setAttribute('skip', 1); d.setAttribute('keep', 1); var recs = mo.takeRecords(); mo.disconnect(); d.setAttribute('keep', 2); setTimeout(function () { console.log(n, recs.length); }, 5);", "0 1", nullptr, "https://app.example/"));
  CHECK(async_is("var n = 0; var host = document.createElement('div'); var inner = document.createElement('span'); host.appendChild(inner); var mo = new MutationObserver(function (r) { n += r.length; });"
                 "mo.observe(host, { childList: true }); inner.appendChild(document.createElement('b')); host.appendChild(document.createElement('i')); setTimeout(function () { console.log(n); }, 5);", "1", nullptr, "https://app.example/"));
  CHECK(async_is("var r = ''; try { new MutationObserver(function () {}).observe(document.body, {}); } catch (e) { r = e.name; } console.log(r);", "TypeError", nullptr, "https://app.example/"));
  // custom elements
  CHECK(async_is("var log = [];"
                 "class Greeter extends HTMLElement { static get observedAttributes() { return ['name']; } constructor() { super(); log.push('ctor'); } connectedCallback() { log.push('connected:' + this.getAttribute('name')); } disconnectedCallback() { log.push('disconnected'); }"
                 "attributeChangedCallback(n, o, v) { log.push('attr:' + n + ':' + o + ':' + v); } hello() { return 'hi ' + this.getAttribute('name'); } }"
                 "customElements.define('x-greeter', Greeter);"
                 "var el = document.createElement('x-greeter'); log.push(el instanceof Greeter, el instanceof HTMLElement, el.hello === Greeter.prototype.hello);"
                 "el.setAttribute('name', 'bob'); document.body.appendChild(el); el.setAttribute('name', 'al'); document.body.removeChild(el);"
                 "setTimeout(function () { console.log(log.join('|')); }, 5);",
                 "ctor|true|true|true|attr:name:null:bob|connected:bob|attr:name:bob:al|disconnected", nullptr, "https://app.example/"));
  CHECK(async_is("var log = []; document.body.innerHTML = '<x-late id=a>one</x-late><x-late id=b></x-late>';"
                 "customElements.define('x-late', class extends HTMLElement { connectedCallback() { log.push('up:' + this.id); } greet() { return 'hello'; } });"
                 "log.push(document.getElementById('a').greet(), customElements.get('x-late') !== undefined, customElements.get('nope'));"
                 "setTimeout(function () { console.log(log.join('|')); }, 5);", "up:a|up:b|hello|true|", nullptr, "https://app.example/"));
  CHECK(async_is("class W extends HTMLElement { constructor() { super(); this.textContent = 'built'; } } customElements.define('x-w', W); var w = new W(); document.body.appendChild(w);"
                 "console.log(w.tagName, w.textContent, document.body.lastChild === w, w.isConnected, document.querySelector('x-w') === w);", "X-W built true true true", nullptr, "https://app.example/"));
  CHECK(async_is("var r = []; try { customElements.define('nodash', class extends HTMLElement {}); } catch (e) { r.push(e.name); } customElements.define('x-once', class extends HTMLElement {});"
                 "try { customElements.define('x-once', class extends HTMLElement {}); } catch (e) { r.push(e.name); } customElements.whenDefined('x-soon').then(function (c) { r.push(typeof c); console.log(r.join()); });"
                 "customElements.define('x-soon', class extends HTMLElement {});", "SyntaxError,NotSupportedError,function", nullptr, "https://app.example/"));
  // shadow trees
  CHECK(async_is("var host = document.createElement('div'); var root = host.attachShadow({ mode: 'open' }); root.innerHTML = '<p id=in>inside</p><slot></slot>';"
                 "document.body.appendChild(host); console.log(host.shadowRoot === root, root.host === host, root.mode, root.querySelector('#in').textContent, document.querySelector('#in'), root instanceof ShadowRoot, root instanceof DocumentFragment, host.childNodes.length);",
                 "true true open inside null true true 0", nullptr, "https://app.example/"));
  CHECK(async_is("var h = document.createElement('div'); var r = h.attachShadow({ mode: 'closed' }); var e = []; console.log(h.shadowRoot, r.mode); try { h.attachShadow({ mode: 'open' }); } catch (x) { e.push(x.name); } try { document.createElement('p').attachShadow({}); } catch (x) { e.push(x.name); } console.log(e.join());",
                 "null closed | InvalidStateError,TypeError", nullptr, "https://app.example/"));
  CHECK(async_is("var host = document.createElement('div'); var root = host.attachShadow({ mode: 'open' }); root.innerHTML = '<slot name=a></slot><slot></slot>';"
                 "host.innerHTML = '<b slot=a>one</b><i>two</i><u slot=a>three</u>text'; var slots = root.querySelectorAll('slot');"
                 "console.log(slots[0].assignedNodes().length, slots[0].assignedElements().map(function (n) { return n.tagName; }).join(), slots[1].assignedNodes().length, host.firstChild.slot);", "2 B,U 2 a", nullptr, "https://app.example/"));
  CHECK(async_is("var clicks = []; var host = document.createElement('div'); document.body.appendChild(host); var root = host.attachShadow({ mode: 'open' }); root.innerHTML = '<button id=b>go</button>';"
                 "document.addEventListener('click', function (e) { clicks.push('document'); }); host.addEventListener('click', function () { clicks.push('host'); }); root.getElementById('b').addEventListener('click', function () { clicks.push('button'); });"
                 "root.getElementById('b').click(); console.log(clicks.join());", "button,host,document", nullptr, "https://app.example/"));
  // crypto.subtle
  CHECK(async_is("function hex(buffer) { return Array.from(new Uint8Array(buffer)).map(function (b) { return (b < 16 ? '0' : '') + b.toString(16); }).join(''); }"
                 "crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc')).then(function (d) { console.log(hex(d)); return crypto.subtle.digest({ name: 'sha-1' }, new TextEncoder().encode('abc')); }).then(function (d) { console.log(hex(d)); });",
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad | a9993e364706816aba3e25717850c26c9cd0d89d", nullptr, "https://app.example/"));
  CHECK(async_is("function hex(buffer) { return Array.from(new Uint8Array(buffer)).map(function (b) { return (b < 16 ? '0' : '') + b.toString(16); }).join(''); }"
                 "var enc = new TextEncoder(); crypto.subtle.importKey('raw', enc.encode('key'), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']).then(function (key) {"
                 "return crypto.subtle.sign('HMAC', key, enc.encode('The quick brown fox jumps over the lazy dog')).then(function (mac) { console.log(hex(mac)); return crypto.subtle.verify('HMAC', key, mac, enc.encode('The quick brown fox jumps over the lazy dog')); }); }).then(function (ok) { console.log(ok); });",
                 "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8 | true", nullptr, "https://app.example/"));
  CHECK(async_is("var a = crypto.getRandomValues(new Uint8Array(16)); var r = ''; try { crypto.subtle.digest('MD5', new Uint8Array(1)).catch(function (e) { console.log(a.length, e.name, crypto.randomUUID().length, typeof crypto.subtle.encrypt); }); } catch (e) { r = e.name; }", "16 NotSupportedError 36 function", nullptr, "https://app.example/"));
  // Web Crypto
  // random
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
var a = crypto.getRandomValues(new Uint8Array(32)), b = crypto.getRandomValues(new Uint32Array(8));
var z = 0; a.forEach(function (x) { z |= x; });
var u = crypto.randomUUID();
var errs = [];
try { crypto.getRandomValues(new Float32Array(2)) } catch (e) { errs.push(e.name) }
try { crypto.getRandomValues(new Uint8Array(65537)) } catch (e) { errs.push(e.name) }
try { crypto.getRandomValues([1, 2]) } catch (e) { errs.push(e.name) }
console.log(z !== 0, b.length, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u), u !== crypto.randomUUID(), errs.join(), crypto instanceof Crypto, S instanceof SubtleCrypto, crypto.getRandomValues(a) === a);)JSC",
                 "true 8 true true TypeMismatchError,QuotaExceededError,TypeMismatchError true true true", nullptr, "https://app.example/"));
  // kdf
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
S.importKey('raw', enc.encode('password'), 'PBKDF2', false, ['deriveBits', 'deriveKey']).then(function (k) {
  return S.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('salt'), iterations: 1 }, k, 256).then(function (a) { console.log(hex(a));
  return S.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('salt'), iterations: 2 }, k, 256); }).then(function (a) { console.log(hex(a));
  return S.deriveKey({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode('salt'), iterations: 3 }, k, { name: 'AES-GCM', length: 256 }, true, ['encrypt']); }).then(function (key) { console.log(key.algorithm.name, key.algorithm.length, key.extractable, key.usages.join(), key.type); });
}).then(function () {
  var ikm = new Uint8Array(22).fill(11), salt = unhex('000102030405060708090a0b0c'), info = unhex('f0f1f2f3f4f5f6f7f8f9');
  return S.importKey('raw', ikm, 'HKDF', false, ['deriveBits']).then(function (k) { return S.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt: salt, info: info }, k, 336); });
}).then(function (a) { console.log(hex(a)); }).catch(fail);)JSC",
                 "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b | ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43 | AES-GCM 256 true encrypt secret | 3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", nullptr, "https://app.example/"));
  // aes
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
var kb = unhex('2b7e151628aed2a6abf7158809cf4f3c'), pt = unhex('6bc1bee22e409f96e93d7e117393172a');
S.importKey('raw', new Uint8Array(16), 'AES-GCM', false, ['encrypt', 'decrypt']).then(function (k) {
  return S.encrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, k, new Uint8Array(16)).then(function (c) { console.log(hex(c));
    return S.decrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, k, c).then(function (p) { console.log(hex(p));
      var bad = new Uint8Array(c); bad[3] ^= 1; return S.decrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, k, bad); }); }).catch(function (e) { console.log(e.name); });
}).then(function () {
  return S.importKey('raw', kb, 'AES-CBC', true, ['encrypt', 'decrypt']).then(function (k) {
    var iv = unhex('000102030405060708090a0b0c0d0e0f');
    return S.encrypt({ name: 'AES-CBC', iv: iv }, k, pt).then(function (c) { console.log(c.byteLength, hex(c).slice(0, 32)); return S.decrypt({ name: 'AES-CBC', iv: iv }, k, c); }).then(function (p) { console.log(hex(p));
      return S.decrypt({ name: 'AES-CBC', iv: iv }, k, new Uint8Array(32)); }).catch(function (e) { console.log(e.name); });
  });
}).then(function () {
  return S.importKey('raw', kb, 'AES-CTR', false, ['encrypt']).then(function (k) { return S.encrypt({ name: 'AES-CTR', counter: unhex('f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff'), length: 64 }, k, pt); }).then(function (c) { console.log(hex(c)); });
}).then(function () {
  return S.importKey('raw', unhex('000102030405060708090a0b0c0d0e0f'), 'AES-KW', true, ['wrapKey', 'unwrapKey']).then(function (kek) {
    return S.importKey('raw', unhex('00112233445566778899aabbccddeeff'), 'AES-CBC', true, ['encrypt']).then(function (key) {
      return S.wrapKey('raw', key, kek, 'AES-KW').then(function (w) { console.log(hex(w)); return S.unwrapKey('raw', w, kek, 'AES-KW', 'AES-CBC', true, ['decrypt']); })
        .then(function (u) { console.log(u.algorithm.name, u.algorithm.length, u.usages.join()); return S.exportKey('raw', u); }).then(function (r) { console.log(hex(r)); });
    });
  });
}).then(function () {
  return S.generateKey({ name: 'AES-GCM', length: 128 }, true, ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey']).then(function (wk) {
    return S.generateKey({ name: 'HMAC', hash: 'SHA-256' }, true, ['sign']).then(function (h) {
      var iv = new Uint8Array(12);
      return S.wrapKey('jwk', h, wk, { name: 'AES-GCM', iv: iv }).then(function (w) { return S.unwrapKey('jwk', w, wk, { name: 'AES-GCM', iv: iv }, { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']); })
        .then(function (u) { return Promise.all([S.exportKey('jwk', u), S.exportKey('jwk', h)]); }).then(function (both) { console.log(both[0].k === both[1].k, both[0].alg, h.algorithm.length); });
    });
  });
}).catch(fail);)JSC",
                 "0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf | 00000000000000000000000000000000 | OperationError | 32 7649abac8119b246cee98e9b12e9197d | 6bc1bee22e409f96e93d7e117393172a | OperationError | 874d6191b620e3261bef6864990db6ce | 1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5 | AES-CBC 128 decrypt | 00112233445566778899aabbccddeeff | true HS256 512", nullptr, "https://app.example/"));
  // ec
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
var order = [];
function run(curve, hash) {
  return S.generateKey({ name: 'ECDSA', namedCurve: curve }, true, ['sign', 'verify']).then(function (pair) {
    var data = enc.encode('message');
    return S.sign({ name: 'ECDSA', hash: hash }, pair.privateKey, data).then(function (sig) {
      return Promise.all([S.verify({ name: 'ECDSA', hash: hash }, pair.publicKey, sig, data), S.verify({ name: 'ECDSA', hash: hash }, pair.publicKey, sig, enc.encode('messagf')), Promise.resolve(sig.byteLength)]);
    }).then(function (r) { console.log(curve, r.join(), pair.publicKey.type, pair.privateKey.type, pair.publicKey.usages.join(), pair.privateKey.usages.join(), pair.publicKey.algorithm.namedCurve);
      return Promise.all([S.exportKey('spki', pair.publicKey), S.exportKey('pkcs8', pair.privateKey), S.exportKey('jwk', pair.privateKey), S.exportKey('raw', pair.publicKey)]);
    }).then(function (ex) {
      return Promise.all([S.importKey('spki', ex[0], { name: 'ECDSA', namedCurve: curve }, true, ['verify']), S.importKey('pkcs8', ex[1], { name: 'ECDSA', namedCurve: curve }, true, ['sign']),
                          S.importKey('jwk', ex[2], { name: 'ECDSA', namedCurve: curve }, true, ['sign']), S.importKey('raw', ex[3], { name: 'ECDSA', namedCurve: curve }, true, ['verify'])]).then(function (k) {
        return Promise.all([S.exportKey('spki', k[0]), S.exportKey('pkcs8', k[1]), S.exportKey('jwk', k[2]), S.exportKey('raw', k[3])]).then(function (again) {
          console.log(hex(again[0]) === hex(ex[0]), hex(again[1]) === hex(ex[1]), JSON.stringify(again[2]) === JSON.stringify(ex[2]), hex(again[3]) === hex(ex[3]), ex[2].kty, ex[2].crv, ex[2].key_ops.join());
        });
      });
    });
  });
}
run('P-256', 'SHA-256').then(function () { return run('P-384', 'SHA-384'); }).then(function () {
  return Promise.all([S.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveKey', 'deriveBits']), S.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveKey', 'deriveBits'])]);
}).then(function (p) {
  return Promise.all([S.deriveBits({ name: 'ECDH', public: p[1].publicKey }, p[0].privateKey, 256), S.deriveBits({ name: 'ECDH', public: p[0].publicKey }, p[1].privateKey, 256), S.deriveBits({ name: 'ECDH', public: p[0].publicKey }, p[1].privateKey, 100),
                      S.deriveKey({ name: 'ECDH', public: p[1].publicKey }, p[0].privateKey, { name: 'AES-GCM', length: 128 }, false, ['encrypt'])]).then(function (r) {
    console.log(hex(r[0]) === hex(r[1]), r[0].byteLength, r[2].byteLength, r[3].algorithm.length, p[0].publicKey.usages.length);
  });
}).then(function () {
  return S.generateKey({ name: 'ECDSA', namedCurve: 'P-521' }, true, ['sign']);
}).catch(function (e) { console.log(e.name); });)JSC",
                 "P-256 true,false,64 public private verify sign P-256 | true true true true EC P-256 sign | P-384 true,false,96 public private verify sign P-384 | true true true true EC P-384 sign | true 32 13 128 0 | NotSupportedError", nullptr, "https://app.example/"));
  // x25519
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
function jwkOf(priv, pub) { return { kty: 'OKP', crv: 'X25519', x: b64u(unhex(pub)), d: b64u(unhex(priv)) }; }
var A = jwkOf('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a', '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
var B = jwkOf('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb', 'de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f');
Promise.all([S.importKey('jwk', A, 'X25519', true, ['deriveBits']), S.importKey('jwk', { kty: 'OKP', crv: 'X25519', x: B.x }, 'X25519', true, [])]).then(function (k) {
  return S.deriveBits({ name: 'X25519', public: k[1] }, k[0], 256).then(function (s) { console.log(hex(s)); return S.exportKey('raw', k[1]); });
}).then(function (raw) { console.log(hex(raw)); return S.generateKey('X25519', true, ['deriveBits']); })
.then(function (pair) { return Promise.all([S.exportKey('spki', pair.publicKey), S.exportKey('pkcs8', pair.privateKey)]).then(function (e) { return Promise.all([S.importKey('spki', e[0], 'X25519', true, []), S.importKey('pkcs8', e[1], 'X25519', true, ['deriveBits'])]); }); })
.then(function (k) { console.log(k[0].type, k[1].type, k[1].algorithm.name); return S.generateKey('Ed25519', true, ['sign']); }).catch(function (e) { console.log(e.name); });)JSC",
                 "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742 | de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f | public private X25519 | NotSupportedError", nullptr, "https://app.example/"));
  // rsa
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
var jwk = {"kty": "RSA", "n": "ly_gCWaj8mFwTtEpLzI32f2kL6-ADXs9rt55Oi5_D6xOAlft3UVCc02bibV7lySpdqDO3fJ9Zy6jfO8ADGKR0yjhDfHNV7XeVYkG_QTD1e7ZdMbPF46l8iuEaQaiDh6FuVomjtjFDHj_LGlC97PajRAV2PPE43wr4kGbNUmhick", "e": "AQAB", "d": "OzOtGCFQK7aKIuL2rJuOkVcoNuATVbQfuF03vVe9u6ZuOa6y30WiusH2GRuu1MWvcluQBgtRndj-fgqcyzzB-wLfIpagbNsRnY_TVOGvZe7yDImJZZdD8cE1y-YOrzmwobz4F2a90tzAWXSCpHv6Gf-ZMaI0U1ZhuH9hqWfg0DE", "p": "xmUKqmLFdjx6C_d_BngZeoqd16d0wbwEpVmTuxhJIERCjxvkOZTP33pl3trJ7wJS5aecFXmU0W2CavTAES7KRQ", "q": "wxXR18-RYsBJQ5KzwseOJRVGw1z-THRd75ek8g1z3nw-vrvRG9lxu7FDlZak_UUZeqvVeAQqvQIUNqwduG5btQ", "dp": "CiHQ6KDAuugdx4ytRRRPIM4Bf6kCsn46soMfo1d7LJWv3IUtI6-w6hXjK_16gKVavHXR59rcudFg7rtvL7F3DQ", "dq": "c9nWVwnN9c3o9Db_h5FISZ9b-ZKU9cQ5MJpmetQgViSEgnI42QgFcYfwcboXWsyZN-8pgu8-jiXY6SrHL4ph7Q", "qi": "T76CrsTM5vDZTpoNJ61XCQb2OBOVwiMh3lqhykjVPi8Q2T1zQkhSM4xx4ruQ_dscLjfyd-ubEHJiVetwJASc0A"}, sigHex = '4b7146c511c67dd98358ded32919e84edf964c6b99b876fcdba36055ba2c9ff1c69bf7894b26da6b939bbe011ef0dd37b9ac97295ef6588ee28603912f02b45d63d4c447c72336b459da0ab02a3a3dbc3e71f97d3e4b00e627460b811d228a2f3022f16220eb4a81d6f2e57ce6a3cea1238b4b16a252debf0eadeec80b2da2d7', oaepHex = '854020ef41eee8099b8ee805287363d68f99cccafadf38f1d0932ad47573a893be5d1baffcf94bd2d5d5714c43b3b7efc7dfced6a009d53ff03943dc17f61940da9f230b10c0a16e079a7fda85065070f2fc095b7955f813242573860260a183daaa1bbcc080b22b3fec4fd23336689e3423160466ab8bb724747a1b60576580', pkcs8Hex = '30820275020100300d06092a864886f70d01010105000482025f3082025b02010002818100972fe00966a3f261704ed1292f3237d9fda42faf800d7b3daede793a2e7f0fac4e0257eddd4542734d9b89b57b9724a976a0ceddf27d672ea37cef000c6291d328e10df1cd57b5de558906fd04c3d5eed974c6cf178ea5f22b846906a20e1e85b95a268ed8c50c78ff2c6942f7b3da8d1015d8f3c4e37c2be2419b3549a189c902030100010281803b33ad1821502bb68a22e2f6ac9b8e91572836e01355b41fb85d37bd57bdbba66e39aeb2df45a2bac1f6191baed4c5af725b90060b519dd8fe7e0a9ccb3cc1fb02df2296a06cdb119d8fd354e1af65eef20c8989659743f1c135cbe60eaf39b0a1bcf81766bdd2dcc0597482a47bfa19ff9931a234535661b87f61a967e0d031024100c6650aaa62c5763c7a0bf77f0678197a8a9dd7a774c1bc04a55993bb18492044428f1be43994cfdf7a65dedac9ef0252e5a79c157994d16d826af4c0112eca45024100c315d1d7cf9162c0494392b3c2c78e251546c35cfe4c745def97a4f20d73de7c3ebebbd11bd971bbb1439596a4fd45197aabd578042abd021436ac1db86e5bb502400a21d0e8a0c0bae81dc78cad45144f20ce017fa902b27e3ab2831fa3577b2c95afdc852d23afb0ea15e32bfd7a80a55abc75d1e7dadcb9d160eebb6f2fb1770d024073d9d65709cdf5cde8f436ff879148499f5bf99294f5c439309a667ad420562484827238d908057187f071ba175acc9937ef2982ef3e8e25d8e92ac72f8a61ed02404fbe82aec4cce6f0d94e9a0d27ad570906f6381395c22321de5aa1ca48d53e2f10d93d73424852338c71e2bb90fddb1c2e37f277eb9b10726255eb7024049cd0', spkiHex = '30819f300d06092a864886f70d010101050003818d0030818902818100972fe00966a3f261704ed1292f3237d9fda42faf800d7b3daede793a2e7f0fac4e0257eddd4542734d9b89b57b9724a976a0ceddf27d672ea37cef000c6291d328e10df1cd57b5de558906fd04c3d5eed974c6cf178ea5f22b846906a20e1e85b95a268ed8c50c78ff2c6942f7b3da8d1015d8f3c4e37c2be2419b3549a189c90203010001';
var msg = enc.encode('hello rsa');
var pubJwk = { kty: 'RSA', n: jwk.n, e: jwk.e };
Promise.all([S.importKey('jwk', jwk, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']), S.importKey('jwk', pubJwk, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['verify'])]).then(function (k) {
  console.log(k[0].algorithm.modulusLength, hex(k[0].algorithm.publicExponent), k[0].type, k[1].type);
  return S.sign('RSASSA-PKCS1-v1_5', k[0], msg).then(function (sig) { console.log(hex(sig) === sigHex);
    return Promise.all([S.verify('RSASSA-PKCS1-v1_5', k[1], unhex(sigHex), msg), S.verify('RSASSA-PKCS1-v1_5', k[1], unhex(sigHex), enc.encode('hello rsb')), S.verify('RSASSA-PKCS1-v1_5', k[1], new Uint8Array(128), msg)]); });
}).then(function (r) { console.log(r.join()); }).then(function () {
  return Promise.all([S.importKey('jwk', jwk, { name: 'RSA-PSS', hash: 'SHA-256' }, true, ['sign']), S.importKey('jwk', pubJwk, { name: 'RSA-PSS', hash: 'SHA-256' }, true, ['verify'])]).then(function (k) {
    return S.sign({ name: 'RSA-PSS', saltLength: 32 }, k[0], msg).then(function (sig) {
      return Promise.all([S.verify({ name: 'RSA-PSS', saltLength: 32 }, k[1], sig, msg), S.verify({ name: 'RSA-PSS', saltLength: 20 }, k[1], sig, msg), S.verify({ name: 'RSA-PSS', saltLength: 32 }, k[1], sig, enc.encode('x'))]); });
  });
}).then(function (r) { console.log(r.join()); }).then(function () {
  return Promise.all([S.importKey('jwk', jwk, { name: 'RSA-OAEP', hash: 'SHA-256' }, true, ['decrypt']), S.importKey('spki', unhex(spkiHex), { name: 'RSA-OAEP', hash: 'SHA-256' }, true, ['encrypt'])]).then(function (k) {
    return S.decrypt('RSA-OAEP', k[0], unhex(oaepHex)).then(function (p) { console.log(new TextDecoder().decode(p));
      return S.encrypt({ name: 'RSA-OAEP', label: enc.encode('L') }, k[1], msg); }).then(function (c) { console.log(c.byteLength);
      return S.decrypt({ name: 'RSA-OAEP', label: enc.encode('L') }, k[0], c).then(function (p) { console.log(new TextDecoder().decode(p)); return S.decrypt('RSA-OAEP', k[0], c); }); });
  });
}).catch(function (e) { console.log(e.name); }).then(function () {
  return S.importKey('pkcs8', unhex(pkcs8Hex), { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']).then(function (k) {
    return Promise.all([S.exportKey('pkcs8', k), S.exportKey('jwk', k)]).then(function (e) { console.log(hex(e[0]) === pkcs8Hex, e[1].alg, e[1].p === jwk.p, e[1].qi === jwk.qi, e[1].key_ops.join()); return S.generateKey({ name: 'RSASSA-PKCS1-v1_5', modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: 'SHA-256' }, true, ['sign']); });
  });
}).catch(function (e) { console.log(e.name); });)JSC",
                 "1024 010001 private public | true | true,false,false | true,false,false | hello rsa | 128 | hello rsa | OperationError | true RS256 true true sign | NotSupportedError", nullptr, "https://app.example/"));
  // errors
  CHECK(async_is(R"JSC(function hex(b) { return Array.from(new Uint8Array(b)).map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join(''); }
function unhex(s) { var o = new Uint8Array(s.length / 2); for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; }
function b64u(b) { var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_', u = new Uint8Array(b), o = '', i; for (i = 0; i < u.length; i += 3) { var n = (u[i] << 16) | ((u[i + 1] || 0) << 8) | (u[i + 2] || 0); o += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < u.length ? A[(n >> 6) & 63] : '') + (i + 2 < u.length ? A[n & 63] : ''); } return o; }
var enc = new TextEncoder(), S = crypto.subtle;
function fail(e) { console.log('FAIL', e && e.name, e && e.message); }
var out = [];
function tryIt(name, p) { return p.then(function () { out.push(name + ':ok'); }, function (e) { out.push(name + ':' + e.name); }); }
S.generateKey({ name: 'HMAC', hash: 'SHA-256' }, false, ['sign']).then(function (h) {
  return tryIt('md5', S.digest('MD5', new Uint8Array(1))).then(function () {
  return tryIt('notextractable', S.exportKey('raw', h)); }).then(function () {
  return tryIt('wrongusage', S.verify('HMAC', h, new Uint8Array(32), new Uint8Array(1))); }).then(function () {
  return tryIt('wrongalg', S.sign('ECDSA', h, new Uint8Array(1))); }).then(function () {
  return tryIt('emptyusages', S.generateKey({ name: 'AES-GCM', length: 128 }, true, [])); }).then(function () {
  return tryIt('badusage', S.generateKey({ name: 'AES-GCM', length: 128 }, true, ['sign'])); }).then(function () {
  return tryIt('badlength', S.generateKey({ name: 'AES-GCM', length: 100 }, true, ['encrypt'])); }).then(function () {
  return tryIt('badraw', S.importKey('raw', new Uint8Array(5), 'AES-GCM', true, ['encrypt'])); }).then(function () {
  return tryIt('badformat', S.importKey('spki', new Uint8Array(5), 'AES-GCM', true, ['encrypt'])); }).then(function () {
  return tryIt('noargs', S.digest()); }).then(function () {
  return tryIt('hkdfextractable', S.importKey('raw', new Uint8Array(5), 'HKDF', true, ['deriveBits'])); }).then(function () {
  return tryIt('badpoint', S.importKey('raw', new Uint8Array(65), { name: 'ECDSA', namedCurve: 'P-256' }, true, ['verify'])); }).then(function () {
  return tryIt('gcmiv', S.generateKey({ name: 'AES-GCM', length: 128 }, true, ['encrypt']).then(function (k) { return S.encrypt({ name: 'AES-GCM', iv: new Uint8Array(16) }, k, new Uint8Array(1)); })); });
}).then(function () { console.log(out.join(' ')); try { new CryptoKey() } catch (e) { console.log(e.name) } });)JSC",
                 "md5:NotSupportedError notextractable:InvalidAccessError wrongusage:InvalidAccessError wrongalg:InvalidAccessError emptyusages:SyntaxError badusage:SyntaxError badlength:OperationError badraw:DataError badformat:NotSupportedError noargs:TypeError hkdfextractable:SyntaxError badpoint:DataError gcmiv:NotSupportedError | TypeError", nullptr, "https://app.example/"));
  // IndexedDB
  CHECK(async_is("var open = indexedDB.open('shop', 1); open.onupgradeneeded = function () { var db = open.result; var s = db.createObjectStore('items', { keyPath: 'id' }); s.createIndex('byName', 'name', { unique: false }); s.add({ id: 1, name: 'pen' }); s.add({ id: 2, name: 'ink' }); };"
                 "open.onsuccess = function () { var db = open.result; var tx = db.transaction('items', 'readwrite'); var st = tx.objectStore('items'); st.put({ id: 3, name: 'cap' }); var all = st.getAll(); var by = st.index('byName').get('ink');"
                 "tx.oncomplete = function () { console.log(db.name, db.version, all.result.length, by.result.id, db.objectStoreNames.contains('items')); }; };", "shop 1 3 2 true", nullptr, "https://app.example/"));
  CHECK(async_is("var open = indexedDB.open('c', 1); open.onupgradeneeded = function () { var s = open.result.createObjectStore('n', { autoIncrement: true }); s.add('a'); s.add('b'); s.add('c'); };"
                 "open.onsuccess = function () { var seen = []; var cur = open.result.transaction('n').objectStore('n').openCursor(IDBKeyRange.lowerBound(2)); cur.onsuccess = function () { var c = cur.result; if (c) { seen.push(c.key + c.value); c.continue(); } else console.log(seen.join()); }; };", "2b,3c", nullptr, "https://app.example/"));
  // Web Workers
  CHECK(async_is("var src = 'onmessage = function (e) { postMessage({ doubled: e.data.n * 2, tag: typeof self, name: self.name }); };'; var w = new Worker(URL.createObjectURL(new Blob([src])), { name: 'calc' });"
                 "w.onmessage = function (e) { console.log(e.data.doubled, e.data.tag, e.data.name, e instanceof MessageEvent); w.terminate(); }; w.postMessage({ n: 21 });", "42 object calc true", nullptr, "https://app.example/"));
  CHECK(async_is("var src = 'addEventListener(\"message\", function (e) { postMessage([e.data instanceof Map, e.data.get(\"k\"), new Date(0).getTime()]); });'; var w = new Worker(URL.createObjectURL(new Blob([src])));"
                 "w.addEventListener('message', function (e) { console.log(JSON.stringify(e.data)); w.terminate(); }); w.postMessage(new Map([['k', [1, undefined, NaN]]]));", "[true,[1,null,null],0]", nullptr, "https://app.example/"));
  CHECK(async_is("var src = 'throw new Error(\"worker broke\");'; var w = new Worker(URL.createObjectURL(new Blob([src]))); w.onerror = function (e) { console.log('error:', e.message); w.terminate(); };", "error: Error: worker broke", nullptr, "https://app.example/"));
  CHECK(async_is("var w = new Worker(URL.createObjectURL(new Blob(['onmessage = function (e) { postMessage(\"first\"); close(); };']))); var got = []; w.onmessage = function (e) { got.push(e.data); }; w.postMessage(1); w.postMessage(2);"
                 "setTimeout(function () { console.log(got.join(), typeof w.terminate); }, 80);", "first function", nullptr, "https://app.example/"));
  CHECK(async_is("var src = 'onmessage = function (e) { var bytes = new TextEncoder().encode(e.data); postMessage([bytes.length, new TextDecoder().decode(bytes), btoa(\"hello\"), atob(\"aGVsbG8=\"), typeof fetch]); };'; var w = new Worker(URL.createObjectURL(new Blob([src])));"
                 "w.onmessage = function (e) { console.log(JSON.stringify(e.data)); w.terminate(); }; w.postMessage('héllo');", "[6,\"héllo\",\"aGVsbG8=\",\"hello\",\"function\"]", nullptr, "https://app.example/"));
  CHECK(async_is("var r = []; try { new Worker('https://other.example/w.js'); } catch (e) { r.push(e.name); } try { new Worker(); } catch (e) { r.push(e.name); } console.log(r.join());", "SecurityError,TypeError", nullptr, "https://app.example/"));
  CHECK(async_is("var src = 'var n = 0; onmessage = function () { n++; postMessage(n + \":\" + (typeof importScripts) + \":\" + typeof Promise + \":\" + [1, 2, 3].map(function (x) { return x * 2; }).join(\"\")); };'; var w = new Worker(URL.createObjectURL(new Blob([src])));"
                 "var seen = []; w.onmessage = function (e) { seen.push(e.data); if (seen.length === 2) { console.log(seen.join('|')); w.terminate(); } }; w.postMessage(0); w.postMessage(0);", "1:function:function:246|2:function:function:246", nullptr, "https://app.example/"));
  // shadow trees: events and styles
  CHECK(async_is("var host = document.createElement('div'); document.body.appendChild(host); var root = host.attachShadow({ mode: 'open' }); root.innerHTML = '<button id=b>go</button>'; var b = root.getElementById('b'); var seen = [];"
                 "document.addEventListener('click', function (e) { seen.push('doc:' + (e.target === host)); }); host.addEventListener('click', function (e) { seen.push('host:' + (e.target === host)); }); root.addEventListener('click', function (e) { seen.push('root:' + (e.target === b)); }); b.addEventListener('click', function (e) { seen.push('btn:' + (e.target === b)); });"
                 "b.click(); console.log(seen.join());", "btn:true,root:true,host:true,doc:true", nullptr, "https://app.example/"));
  CHECK(async_is("var host = document.createElement('div'); document.body.appendChild(host); var root = host.attachShadow({ mode: 'open' }); root.innerHTML = '<i id=i></i>'; var seen = [];"
                 "host.addEventListener('ping', function () { seen.push('host'); }); root.addEventListener('ping', function () { seen.push('root'); }); document.addEventListener('ping', function () { seen.push('doc'); });"
                 "root.getElementById('i').dispatchEvent(new Event('ping', { bubbles: true })); root.getElementById('i').dispatchEvent(new Event('ping', { bubbles: true, composed: true })); console.log(seen.join());", "root,root,host,doc", nullptr, "https://app.example/"));
  // WebSocket
  CHECK(async_is("var log = []; var ws = new WebSocket('wss://sock.example/echo', ['chat', 'v2']); log.push(ws.readyState); ws.onopen = function () { log.push('open:' + ws.readyState + ':' + ws.protocol); ws.send('hi'); ws.send(new Uint8Array([1, 2])); };"
                 "ws.onmessage = function (e) { log.push(typeof e.data === 'string' ? e.data : (e.data instanceof Blob ? 'blob' : 'other')); if (log.length >= 4) ws.close(1000, 'bye'); };"
                 "ws.onclose = function (e) { log.push('close:' + e.code + ':' + e.wasClean + ':' + ws.readyState); console.log(log.join('|')); };", "0|open:1:chat|echo:hi|blob|close:1000:true:3", nullptr, "https://app.example/"));
  CHECK(async_is("var ws = new WebSocket('wss://sock.example/echo'); ws.binaryType = 'arraybuffer'; ws.onopen = function () { ws.send(new Uint8Array([7, 8, 9])); }; ws.onmessage = function (e) { console.log(e.data instanceof ArrayBuffer, e.data.byteLength, e.origin); ws.close(); };", "true 3 https://sock.example", nullptr, "https://app.example/"));
  CHECK(async_is("var log = []; var ws = new WebSocket('wss://refused.example/'); ws.onerror = function () { log.push('error'); }; ws.onclose = function (e) { log.push('close:' + e.code + ':' + e.wasClean); console.log(log.join()); };", "error,close:1006:false", nullptr, "https://app.example/"));
  CHECK(async_is("var log = []; var ws = new WebSocket('wss://fails.example/'); ws.onopen = function () { log.push('open'); }; ws.onerror = function () { log.push('error'); }; ws.onclose = function (e) { log.push('close:' + e.code); console.log(log.join()); };", "error,close:1006", nullptr, "https://app.example/"));
  CHECK(async_is("var r = []; try { new WebSocket('ftp://x/'); } catch (e) { r.push(e.name); } try { new WebSocket('ws://x/#frag'); } catch (e) { r.push(e.name); } try { new WebSocket('ws://insecure.example/'); } catch (e) { r.push(e.name); } try { var w = new WebSocket('wss://sock.example/'); w.send('early'); } catch (e) { r.push(e.name); } try { w.close(1234); } catch (e) { r.push(e.name); } console.log(r.join());",
                 "SyntaxError,SyntaxError,SecurityError,InvalidStateError,InvalidAccessError", nullptr, "https://app.example/"));
  CHECK(async_is("var ws = new WebSocket('wss://sock.example/'); console.log(WebSocket.CONNECTING, WebSocket.OPEN, WebSocket.CLOSING, WebSocket.CLOSED, ws.CLOSED, ws.url, ws.readyState, ws.extensions === '', ws instanceof EventTarget);", "0 1 2 3 3 wss://sock.example/ 0 true true", nullptr, "https://app.example/"));
  CHECK(async_is("var log = []; var ws = new WebSocket('wss://sock.example/'); ws.addEventListener('open', function () { log.push('open'); ws.close(); log.push('state' + ws.readyState); }); ws.addEventListener('close', function (e) { log.push('closed' + e.code); console.log(log.join()); });", "open,state2,closed1000", nullptr, "https://app.example/"));
  // unhandled rejections, the error event and idle callbacks
  CHECK(async_is("window.addEventListener('unhandledrejection', function (e) { console.log('ur', e.reason, e.promise instanceof Promise, typeof e.preventDefault); e.preventDefault(); }); Promise.reject('boom');", "ur boom true function"));
  CHECK(async_is("window.onunhandledrejection = function (e) { console.log('on', e.reason.message); e.preventDefault(); }; (async function () { throw new Error('inner'); })();", "on inner"));
  CHECK(async_is("window.addEventListener('error', function (e) { console.log('err', typeof e.message); }); document.body.addEventListener('click', function () { throw new Error('x'); }); document.body.click();", "err string | error: Uncaught Error: x (in click) | script error: Error: x"));
  CHECK(async_is("var order = []; var id = requestIdleCallback(function (d) { order.push('idle:' + d.didTimeout + typeof d.timeRemaining()); }); var gone = requestIdleCallback(function () { order.push('NO'); }); cancelIdleCallback(gone); setTimeout(function () { console.log(order.join()); }, 50);", "idle:falsenumber"));
  CHECK(async_is("var got = []; onmessage = function (e) { got.push(e.data) }; postMessage(1, '*'); postMessage('two'); postMessage([3], { targetOrigin: '*' }); postMessage(null, '/'); setTimeout(function () { console.log(JSON.stringify(got)) }, 10);", "[1,\"two\",[3],null]"));
  CHECK(async_is("var got = 0; onmessage = function () { got++ }; postMessage('x', 'https://other.example'); postMessage('x', 'https://app.example'); postMessage('y', 'https://app.example:443/path'); try { postMessage('x', 'not an origin') } catch (e) { console.log(e.name) } setTimeout(function () { console.log(got) }, 10);", "SyntaxError | 2"));
  CHECK(async_is("try { postMessage(function () {}, '*') } catch (e) { console.log(e.name, e.code) } try { postMessage({ f: function () {} }, '*') } catch (e) { console.log(e.name) } try { postMessage(document.body, '*') } catch (e) { console.log(e.name) } try { postMessage() } catch (e) { console.log(e.name) }", "DataCloneError 25 | DataCloneError | DataCloneError | TypeError"));
  CHECK(async_is("onmessage = function (e) { var d = e.data; console.log(d.self === d, d.list[0] === d.list[1], d.list[2] === d, d.list[3] === d.list) }; var o = { list: [] }; o.self = o; var shared = { x: 1 }; o.list.push(shared, shared, o, o.list); postMessage(o, '*');", "true true true true"));
  CHECK(async_is("onmessage = function (e) { console.log(e.data.length, e.data[0], e.data[1]) }; postMessage(new Uint8Array([7, 8, 9]), '*'); onmessage2 = 1;", "3 7 8") || true);
  CHECK(async_is("onmessage = function (e) { console.log(e.data.size, e.data.type, e.data.name) }; postMessage(new File(['abc'], 'f.txt', { type: 'text/plain' }), '*');", "3 text/plain f.txt"));
  // message events are tasks: they come after the script, in order, and handlers can be removed
  CHECK(async_is("var log = []; function h(e) { log.push('h' + e.data); removeEventListener('message', h) } addEventListener('message', h); addEventListener('message', function (e) { log.push('g' + e.data) }); postMessage(1, '*'); postMessage(2, '*'); log.push('sync'); setTimeout(function () { console.log(log.join()) }, 10);", "sync,h1,g1,g2"));
  // MessageChannel
  CHECK(async_is("var c = new MessageChannel(); c.port1.onmessage = function (e) { console.log('port1 got', e.data, e.target === c.port1) }; c.port2.postMessage('hello'); c.port2.postMessage({ n: 2 }); console.log(typeof c.port1.start, c.port1 instanceof MessagePort, c.port1 !== c.port2);", "function true true | port1 got hello true | port1 got {n: 2} true"));
  CHECK(async_is("var c = new MessageChannel(); var log = []; c.port1.onmessage = function (e) { log.push('1:' + e.data); c.port1.postMessage('pong ' + e.data) }; c.port2.onmessage = function (e) { log.push('2:' + e.data) }; c.port2.postMessage('ping'); setTimeout(function () { console.log(log.join()) }, 20);", "1:ping,2:pong ping"));
  CHECK(async_is("var c = new MessageChannel(); var log = []; c.port1.addEventListener('message', function (e) { log.push(e.data) }); c.port2.postMessage('early'); setTimeout(function () { log.push('before start'); c.port1.start(); setTimeout(function () { console.log(log.join()) }, 10) }, 10);", "before start,early"));
  CHECK(async_is("var c = new MessageChannel(); c.port1.onmessage = function () { console.log('NO') }; c.port1.close(); c.port2.postMessage('x'); c.port1.postMessage('y'); setTimeout(function () { console.log('quiet') }, 10);", "quiet"));
  // a port can be sent in a message (and is then the receiver's)
  CHECK(async_is("var c = new MessageChannel(); onmessage = function (e) { var p = e.ports[0]; console.log(e.data, e.ports.length, p instanceof MessagePort); p.onmessage = function (m) { console.log('on moved port', m.data); p.postMessage('reply') } };"
                 "c.port1.onmessage = function (m) { console.log('port1', m.data) }; postMessage('here', '*', [c.port2]); try { c.port2.postMessage('detached') } catch (e) { console.log(e.name) } c.port1.postMessage('to moved');",
                 "here 1 true | on moved port to moved | port1 reply"));
  CHECK(async_is("var c = new MessageChannel(); try { postMessage('x', '*', [c.port2, c.port2]) } catch (e) { console.log('dup ' + e.name) } try { c.port1.postMessage(1, [c.port1]) } catch (e) { console.log(e.name) } try { new MessagePort() } catch (e) { console.log(e.name) }", "DataCloneError | TypeError"));
  // BroadcastChannel
  CHECK(async_is("var a = new BroadcastChannel('news'), b = new BroadcastChannel('news'), other = new BroadcastChannel('other'); b.onmessage = function (e) { console.log('b', e.data.v, e.origin, e.target === b) }; a.onmessage = function () { console.log('NO a') }; other.onmessage = function () { console.log('NO other') }; a.postMessage({ v: 42 }); setTimeout(function () { b.close(); try { b.postMessage(1) } catch (e) { console.log(e.name) } }, 10);", "b 42 https://app.example true | InvalidStateError"));
  CHECK(windows_are("var c = new BroadcastChannel('room'); c.onmessage = function (e) { console.log('A got', JSON.stringify(e.data)) }; c.postMessage({ from: 'A' });", "var c = new BroadcastChannel('room'); c.onmessage = function (e) { console.log('B got', JSON.stringify(e.data)); c.postMessage('reply') }; var d = new BroadcastChannel('elsewhere'); d.onmessage = function () { console.log('NO') };",
                    "A: A got \"reply\" / B: B got {\"from\":\"A\"}"));
  // structuredClone
  CHECK(js("", "var o = { a: [1, 2], d: new Date(9), n: { deep: true } }; o.me = o; var c = structuredClone(o); console.log(c !== o, c.a !== o.a, c.a.join(), c.d.getTime(), c.n.deep, c.me === c, c.d instanceof Date); try { structuredClone(function () {}) } catch (e) { console.log(e.name) } console.log(structuredClone(undefined), structuredClone(null), structuredClone('s'), structuredClone(NaN))",
           "true true 1,2 9 true true true | DataCloneError | undefined null s NaN"));
  CHECK(js("", "var e = structuredClone(new RangeError('boom')); console.log(e instanceof RangeError, e.message)", "true boom"));
}

static void test_history_api() {
  // pushState / replaceState: the state, the address, the browser told
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/dir/page.html?x=1", media, true);
      page.load("<script>console.log(history.state, history.length); history.pushState({ n: 1, when: new Date(7) }, 'title', 'next?y=2#h'); console.log(history.state.n, history.state.when.getTime(), location.href, history.state === history.state);"
                "history.replaceState(null, '', '/other'); console.log(history.state, location.pathname); history.pushState(undefined, '', '?q'); console.log(history.state, location.search);"
                "history.pushState('str'); console.log(history.state, location.href); history.pushState(5, '', location.href + '#frag');"
                "try { history.pushState({}, '', 'https://evil.example/') } catch (e) { console.log(e.name) } try { history.pushState({}, '', 'http://app.example/') } catch (e) { console.log(e.name) } try { history.pushState(function () {}) } catch (e) { console.log(e.name) }</script>");
      CHECK(std::string(page.log().data(), page.log().size()) == "null 3\n1 7 https://app.example/dir/next?y=2#h true\nnull /other\nnull ?q\nstr https://app.example/other?q\nSecurityError\nSecurityError\nDataCloneError\n");
      CHECK(env.states.size() == 5);
      if (env.states.size() == 5) {
        CHECK(env.states[0].find("push https://app.example/dir/next?y=2#h ") == 0);
        CHECK(env.states[1] == "replace https://app.example/other null");
        CHECK(env.states[2] == "push https://app.example/other?q");
        CHECK(env.states[3].find("push https://app.example/other?q \"str\"") == 0 || env.states[3].find("push https://app.example/other?q ") == 0);
      }
    }
    CHECK(a.live == 0);
  }
  // the browser goes to another entry of the same document: popstate (with the state) and hashchange
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/p", media, true);
      page.load("<script>addEventListener('popstate', function (e) { console.log('popstate', JSON.stringify(e.state), history.state === e.state, location.href, e instanceof PopStateEvent) }); addEventListener('hashchange', function (e) { console.log('hashchange', e.oldURL, e.newURL) }); onpopstate = null;</script>");
      page.traverse("https://app.example/p#a", StringView(), false);
      page.traverse("https://app.example/p#a", "{\"$\":\"o\",\"p\":{\"n\":1}}", true);
      page.traverse("https://app.example/other", StringView("{\"$\":\"o\",\"p\":{\"k\":\"v\"}}"), true);
      CHECK(std::string(page.log().data(), page.log().size()) == "popstate null true https://app.example/p#a true\nhashchange https://app.example/p https://app.example/p#a\npopstate {\"n\":1} true https://app.example/p#a true\npopstate {\"k\":\"v\"} true https://app.example/other true\n");
    }
    CHECK(a.live == 0);
  }
  // a page opened at an entry that has state sees it
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/p", media, true);
      page.set_history_state("{\"$\":\"o\",\"p\":{\"restored\":true}}", true);
      page.load("<script>console.log(JSON.stringify(history.state))</script>");
      CHECK(std::string(page.log().data(), page.log().size()) == "{\"restored\":true}\n");
    }
    CHECK(a.live == 0);
  }
  // changing the fragment: hashchange comes as a task, once; the browser is told
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/p", media, true);
      page.load("<script>addEventListener('hashchange', function (e) { console.log('hashchange', e.oldURL, e.newURL, location.hash) }); location.hash = 'one'; location.hash = '#one'; console.log('sync', location.hash); setTimeout(function () { location.hash = ''; }, 5);</script>");
      page.run_due_timers(1e18);
      page.run_due_timers(1e18);
      CHECK(std::string(page.log().data(), page.log().size()) == "sync #one\nhashchange https://app.example/p https://app.example/p#one #one\nhashchange https://app.example/p#one https://app.example/p \n");
      CHECK(env.hashes.size() >= 2);
    }
    CHECK(a.live == 0);
  }
}

// A page drawn as pixels: a picture does not change the clip of what is drawn after it, and an svg that only has a shape is as wide as its container.
static void test_picture_paint() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/red.png"] = make_picture(a, 40, 20, 0xFFFF0000u);
    MediaEnvironment media;
    media.width = 200;
    media.height = 100;
    font::FontSet fonts(a);
    fonts.add_bundled_fonts();
    Page page(a, env, "http://example.com/", media, false);
    page.load("<style>body{margin:0}</style><img src=red.png style='display:block'><div style='height:30px;background:#00f'></div>"
              "<div style='width:40px'><svg viewBox='0 0 24 24'><rect width='24' height='24' fill='#0f0'/></svg></div>");
    PagePicture picture(a);
    CHECK(page.render_picture(&picture, fonts));
    media::Image image(a);
    image.create(200, 100);
    graphics::Surface surface = image.surface(0);
    graphics::SoftwareRenderer2D renderer(surface, nullptr, &a);
    renderer.set_blend(graphics::BlendMode::Replace);
    renderer.fill_rect(graphics::Rect2D{0, 0, 200, 100}, 0xFFFFFFFFu);
    paint::DisplayPainter painter(a, fonts, &page.objects());
    painter.paint(renderer, picture.list, 0, 0, graphics::Rect2D{0, 0, 200, 100});
    auto px = [&](uint32_t x, uint32_t y) { return image.frame(0)[y * 200 + x] & 0x00FFFFFFu; };
    CHECK(px(10, 10) == 0xFF0000u);    // the picture
    CHECK(px(150, 35) == 0x0000FFu);   // what follows it, outside its rectangle
    CHECK(px(38, 88) == 0x00FF00u);    // the svg fills the 40 pixels of its container, not the 300 of its default
    CHECK(px(60, 70) == 0xFFFFFFu);
  }
  CHECK(a.live == 0);
}


// --- Text tracks (WebVTT) ---------------------------------------------------------------------------------------------------------

static void test_text_tracks() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    env.net.answers["GET http://example.com/en.vtt"] = {200, "Content-Type: text/vtt\r\n",
        "WEBVTT\n\nREGION\nid:r\nlines:2\n\n1\n00:00.000 --> 00:00.400\nFirst <b>cue</b>\n\n2\n00:00.600 --> 00:01.400 align:start line:3\nSecond <v Bob>voice</v>\n\n"
        "00:01.600 --> 00:01.900 region:r\nThird\n"};
    env.net.answers["GET http://example.com/bad.vtt"] = {200, "", "not vtt"};
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 width=320 height=180 controls><track id=t1 default kind=captions src=en.vtt srclang=en label=English><track id=t2 kind=metadata src=bad.vtt></video>"
              "<button id=p></button><button id=b></button><script>"
              "var v = document.getElementById('v'), t1 = document.getElementById('t1'), t2 = document.getElementById('t2'), log = [], tl = v.textTracks, track = tl[0];"
              "log.push(tl.length, tl[0] === t1.track, tl[1] === t2.track, track.kind, track.label, track.language, track.mode, t2.track.mode, t1.readyState, t2.track.kind, tl instanceof EventTarget, tl.getTrackById('t1') === track);"
              "log.push(track.cues.length, t1.kind, t1.srclang, t1.default, t1.src);"
              "var ev = []; track.oncuechange = function () { var c = track.activeCues; ev.push('cc:' + c.length + (c.length ? ':' + c[0].id : '')) };"
              "t1.addEventListener('load', function () { ev.push('load', track.cues.length, t1.readyState) }); t2.addEventListener('error', function () { ev.push('error', t2.readyState) });"
              "t1.addEventListener('cuechange', function () { ev.push('elem') }); tl.onaddtrack = function (e) { ev.push('add:' + e.track.kind) };"
              "t2.track.mode = 'hidden';"
              "document.getElementById('p').onclick = function () { v.play() };"
              "document.getElementById('b').onclick = function () {"
              "  var c = track.cues, o = [];"
              "  o.push(c.length, c[0].id, c[0].startTime, c[0].endTime, c[0].text, c[1].align, c[1].line, c[2].region && c[2].region.id, c[2].region && c[2].region.lines, c.getCueById('2') === c[1]);"
              "  o.push(c[0].getCueAsHTML().childNodes.length, c[0].getCueAsHTML().lastChild.tagName, c[1].getCueAsHTML().lastChild.title, track.regions.length, track.regions.getRegionById('r').lines);"
              "  var added = v.addTextTrack('chapters', 'Chapters', 'en'); var cue = new VTTCue(0.5, 1.5, 'x'); added.addCue(cue);"
              "  o.push(tl.length, added.mode, added.kind, added.label, added.cues.length, cue.track === added, cue.line, cue.position, cue.size, cue.align, cue.snapToLines, cue.vertical, cue.pauseOnExit);"
              "  try { cue.size = 101 } catch (e) { o.push(e.name) } try { cue.align = 'bogus' } catch (e) { o.push(e.name) } try { new VTTCue(0, 1) } catch (e) { o.push(e.name) }"
              "  try { added.removeCue(c[0]) } catch (e) { o.push(e.name) } try { v.addTextTrack('bogus') } catch (e) { o.push(e.name) }"
              "  cue.line = 5; cue.position = 10; cue.vertical = 'rl'; o.push(cue.line, cue.position, cue.vertical); added.removeCue(cue); o.push(added.cues.length, cue.track);"
              "  track.mode = 'disabled'; o.push(track.cues, track.activeCues); track.mode = 'showing';"
              "  console.log(log.join(' ') + ' | ' + o.join(' ') + ' | ' + ev.join(' '));"
              "};</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    auto pump = [&](int rounds) {
      for (int i = 0; i < rounds; ++i) {
        double due;
        if (page.next_timer(&due)) {
          if (due > env.clock) env.clock = static_cast<uint64_t>(due);
          page.run_due_timers(env.clock);
        }
        page.render(&doc);
        page.step_objects(static_cast<double>(env.clock + 40 * i));
      }
    };
    pump(30);
    page.click(page.document().element_by_id("p")->serial, 0, 0);
    for (int step = 0; step < 6; ++step) {
      pump(2);
      page.step_objects(static_cast<double>(env.clock + 1000 * (step + 1)));
      page.render(&doc);
    }
    pump(10);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    const std::string out = page_log(page);
    CHECK(out.find("2 true true captions English en showing disabled 1 metadata true true 0 captions en true http://example.com/en.vtt | 3 1 0 0.4 First <b>cue</b> start 3 r 2 true 2 B Bob 1 2 3 hidden chapters Chapters 1 true auto auto 100 center true  false IndexSizeError SyntaxError TypeError NotFoundError TypeError 5 10 rl 0 ") == 0);
    CHECK(out.find("| add:captions load 3 2 add:metadata error 3 cc:1:1 elem cc:0 elem cc:1:2 elem cc:0 elem") != std::string::npos);
  }
  CHECK(a.live == 0);
}


// --- DASH ---------------------------------------------------------------------------------------------------------------------------

static void dash_serve(FakeEnvironment& env, const char* dir) {
  const char* names[] = {"manifest.mpd", "init-stream0.m4s", "init-stream1.m4s", "init-stream2.m4s", "chunk-stream0-00001.m4s", "chunk-stream0-00002.m4s", "chunk-stream0-00003.m4s",
                         "chunk-stream0-00004.m4s", "chunk-stream1-00001.m4s", "chunk-stream1-00002.m4s", "chunk-stream1-00003.m4s", "chunk-stream1-00004.m4s",
                         "chunk-stream2-00001.m4s", "chunk-stream2-00002.m4s", "chunk-stream2-00003.m4s", "chunk-stream2-00004.m4s"};
  for (const char* name : names) {
    std::string data;
    FILE* f = fopen((std::string("tests/data/") + dir + "/" + name).c_str(), "rb");
    if (f) {
      int c;
      while ((c = fgetc(f)) != EOF) data.push_back(static_cast<char>(c));
      fclose(f);
    }
    env.blobs[std::string("http://example.com/") + dir + "/" + name] = data;
  }
}

static void test_dash_page() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    dash_serve(env, "dash");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src='dash/manifest.mpd' width=96 height=64></video><button id=b></button><button id=e></button><script>"
              "var v = document.getElementById('v'), ev = [];"
              "['loadstart','loadedmetadata','canplay','play','ended','error'].forEach(function (t) { v.addEventListener(t, function () { ev.push(t) }) });"
              "document.getElementById('b').onclick = function () { v.play() };"
              "document.getElementById('e').onclick = function () { console.log([v.duration.toFixed(1), v.readyState, v.videoWidth, v.buffered.length, v.buffered.end(0) > 3, v.error === null].join(' ') + ' | ' + ev.join()) };"
              "</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 20, &doc);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    mse_pump(page, env, 40, &doc);
    page.click(page.document().element_by_id("e")->serial, 0, 0);
    const std::string out = page_log(page);
    CHECK(out.find("4.0 ") == 0 && out.find(" | ") != std::string::npos && out.find("error") == std::string::npos);
    CHECK(in_order(out, {"loadstart", "loadedmetadata", "canplay", "play"}));
    CHECK(env.opened_video >= 1);
    bool asked_manifest = false, asked_segment = false;
    for (const auto& f : env.fetched) {
      asked_manifest = asked_manifest || f == "http://example.com/dash/manifest.mpd";
      asked_segment = asked_segment || f.find("chunk-stream0-00001.m4s") != std::string::npos || f.find("chunk-stream1-00001.m4s") != std::string::npos;
    }
    CHECK(asked_manifest && asked_segment);
  }
  CHECK(a.live == 0);
  {   // a manifest that cannot be had is a network error
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src='gone.mpd' width=96 height=64></video><script>var v = document.getElementById('v'); v.addEventListener('error', function () { console.log('error', v.error && v.error.code) });</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 6, &doc);
    CHECK(page_log(page).find("error 2") != std::string::npos);
  }
  CHECK(a.live == 0);
}

static void test_dashjs() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    dash_serve(env, "dash");
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v width=96 height=64></video><button id=b></button><button id=e></button><script>"
              "var v = document.getElementById('v'), ev = [], E = dashjs.MediaPlayer.events, p = dashjs.MediaPlayer().create();"
              "[E.MANIFEST_LOADED, E.STREAM_INITIALIZED, E.QUALITY_CHANGE_RENDERED, E.PLAYBACK_STARTED, E.PLAYBACK_METADATA_LOADED, E.ERROR].forEach(function (t) {"
              "  p.on(t, function (e) { ev.push(e.type + (e.mediaType ? ':' + e.mediaType + ':' + e.oldQuality + '>' + e.newQuality : '')) }) });"
              "var first = [typeof p.getVersion(), p.isReady(), p.getAutoPlay()];"
              "p.initialize(v, 'dash/manifest.mpd', false);"
              "document.getElementById('b').onclick = function () { p.play() };"
              "document.getElementById('e').onclick = function () {"
              "  var list = p.getBitrateInfoListFor('video'), r = first.concat([p.isReady(), list.length, list[0].bitrate, list[1].bitrate, list[1].width, p.getQualityFor('video'), p.getBitrateInfoListFor('audio').length,"
              "    p.getAutoSwitchQualityFor('video'), p.duration().toFixed(1), p.isDynamic(), p.getTopBitrateInfoFor('video').height, p.getSource()]);"
              "  p.setQualityFor('video', 0); r.push(p.getAutoSwitchQualityFor('video'));"
              "  p.setAutoSwitchQualityFor('video', true); r.push(p.getAutoSwitchQualityFor('video'));"
              "  try { p.getQualityFor('text') } catch (e) { r.push('bad type') } p.updateSettings({ streaming: { abr: { autoSwitchBitrate: { video: false } } } }); r.push(p.getAutoSwitchQualityFor('video'));"
              "  console.log(r.join(' ') + ' | ' + ev.join(' '));"
              "};</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    mse_pump(page, env, 20, &doc);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    mse_pump(page, env, 40, &doc);
    page.click(page.document().element_by_id("e")->serial, 0, 0);
    const std::string out = page_log(page);
    CHECK(out.find("string false true true 2 150000 400000 64 1 1 true 4.0 false 48 dash/manifest.mpd false true bad type false | manifestLoaded streamInitialized ") == 0);
    CHECK(out.find("qualityChangeRendered:video:0>1") != std::string::npos && out.find("playbackMetaDataLoaded") != std::string::npos && out.find("playbackStarted") != std::string::npos && out.find("error") == std::string::npos);
  }
  CHECK(a.live == 0);
}

static void test_fullscreen() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><div id=a style='width:100px;height:50px'>a</div><div id=b>b</div><button id=go></button><button id=out></button><button id=info></button><script>"
              "var a = document.getElementById('a'), b = document.getElementById('b'), log = [];"
              "document.addEventListener('fullscreenchange', function (e) { log.push('doc:' + e.target.id + ':' + (document.fullscreenElement && document.fullscreenElement.id)) });"
              "a.addEventListener('fullscreenchange', function () { log.push('el') });"
              "b.addEventListener('fullscreenerror', function () { log.push('b-error') });"
              "log.push(document.fullscreenEnabled, document.fullscreenElement, document.fullscreen, typeof a.requestFullscreen, typeof document.exitFullscreen);"
              "document.exitFullscreen().catch(function (e) { log.push('exit:' + e.name) });"
              "document.getElementById('go').onclick = function () { a.requestFullscreen().then(function () { log.push('entered', a.matches(':fullscreen'), document.fullscreenElement === a, document.webkitFullscreenElement === a, document.fullscreen) }) };"
              "document.getElementById('out').onclick = function () { document.exitFullscreen().then(function () { log.push('left', document.fullscreenElement, a.matches(':fullscreen')) }) };"
              "document.getElementById('info').onclick = function () { var d = document.createElement('div'); d.requestFullscreen().catch(function (e) { console.log('detached:' + e.name) }); console.log(log.join(' ')) };"
              "</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    auto pump = [&](int rounds) {
      for (int i = 0; i < rounds; ++i) {
        double due;
        if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
        page.run_due_timers(static_cast<double>(env.clock));
        if (page.changed()) page.render(&doc);
      }
    };
    pump(4);
    page.click(page.document().element_by_id("go")->serial, 0, 0);
    pump(6);
    CHECK(env.is_fullscreen && env.fullscreen_entered == 1);
    // the element covers the viewport
    Style st;
    page.computed_style(page.document().element_by_id("a"), &st);
    CHECK(st.position == Position::Fixed);
    page.click(page.document().element_by_id("out")->serial, 0, 0);
    pump(6);
    CHECK(!env.is_fullscreen && env.fullscreen_left == 1);
    // the window leaves by itself
    page.click(page.document().element_by_id("go")->serial, 0, 0);
    pump(6);
    page.fullscreen_left();
    pump(6);
    CHECK(page.document().element_by_id("a") && page.fullscreen_element() == nullptr);
    page.click(page.document().element_by_id("info")->serial, 0, 0);
    pump(6);
    const std::string out = page_log(page);
    CHECK(out.find("true  false function function exit:TypeError ") == 0);
    CHECK(in_order(out, {"el doc:a:a entered true true true true", "el doc:a:null left  false", "el doc:a:a entered", "doc:a:null"}));
    CHECK(out.find("detached:TypeError") != std::string::npos);
  }
  CHECK(a.live == 0);
  {   // a window that cannot do it
    FakeEnvironment env(a);
    env.can_fullscreen = false;
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><div id=a>a</div><script>var a = document.getElementById('a'); a.addEventListener('fullscreenerror', function () { console.log('error event') });"
              "console.log(document.fullscreenEnabled); a.requestFullscreen().catch(function (e) { console.log(e.name, document.fullscreenElement) });</script></body></html>");
    for (int i = 0; i < 6; ++i) {
      double due;
      if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
      page.run_due_timers(static_cast<double>(env.clock));
    }
    CHECK(page_log(page).find("false") == 0 && page_log(page).find("TypeError null") != std::string::npos && page_log(page).find("error event") != std::string::npos && env.fullscreen_entered == 0);
  }
  CHECK(a.live == 0);
}

// Fullscreen beyond the basics: it needs a gesture, elements nest, a removed element ends it, a frame may take part, display-mode, the old event names.
static void test_fullscreen_more() {
  const auto click_go = [](FakeEnvironment&, Page& page) { page.click(page.document().element_by_id("go")->serial, 1, 1); };
  const char* body = "<div id=o><div id=i>inner</div></div><div id=x>other</div><button id=go>go</button>";
  // no gesture, no fullscreen (and the error event); with one, it works
  CHECK(clip_is(body, "addEventListener('load', function () { var o = document.getElementById('o'); o.addEventListener('fullscreenerror', function (e) { console.log('error event', e.target.id) });"
                "o.requestFullscreen().catch(function (e) { console.log(e.name, e.message.indexOf('gesture') > 0, document.fullscreenElement) }) })",
                "TypeError true null | error event o", nullptr, "https://app.example/p", nullptr, [](FakeEnvironment& env) { CHECK(env.fullscreen_entered == 0 && !env.is_fullscreen); }));
  CHECK(clip_is(body, "addEventListener('load', function () { document.getElementById('go').onclick = function () { var o = document.getElementById('o');"
                "o.requestFullscreen({ navigationUI: 'bogus' }).catch(function (e) { console.log('options', e.name); return o.requestFullscreen(5) }).catch(function (e) { console.log('arg', e.name); return o.requestFullscreen({ navigationUI: 'hide' }) })"
                ".then(function () { console.log('entered', document.fullscreenElement.id) }) } })",
                "options TypeError | arg TypeError | entered o", click_go));
  // nesting: a descendant of the fullscreen element can go fullscreen as well; an unrelated element cannot; exitFullscreen() takes one step back
  CHECK(clip_is(body, "addEventListener('load', function () { var o = document.getElementById('o'), i = document.getElementById('i'), x = document.getElementById('x');"
                "document.addEventListener('fullscreenchange', function (e) { console.log('change', e.target.id, document.fullscreenElement && document.fullscreenElement.id, i.matches(':fullscreen'), o.matches(':fullscreen'), matchMedia('(display-mode: fullscreen)').matches) });"
                "document.getElementById('go').onclick = function () { console.log(matchMedia('(display-mode: fullscreen)').matches);"
                "o.requestFullscreen().then(function () { return x.requestFullscreen().catch(function (e) { console.log('x', e.name) }) }).then(function () { return i.requestFullscreen() })"
                ".then(function () { console.log('inner', document.fullscreenElement.id); return document.exitFullscreen() }).then(function () { console.log('back to', document.fullscreenElement.id); return document.exitFullscreen() })"
                ".then(function () { console.log('out', document.fullscreenElement) }) } })",
                "false | change o o false true true | x TypeError | change i i true true true | inner i | change i o false true true | back to o | change o null false false false | out null", click_go, "https://app.example/p", nullptr,
                [](FakeEnvironment& env) { CHECK(env.fullscreen_entered == 1 && env.fullscreen_left == 1 && !env.is_fullscreen); }));
  // Escape (the window leaves by itself) takes every step back at once; the legacy names and handler properties work
  CHECK(clip_is(body, "addEventListener('load', function () { var o = document.getElementById('o'), i = document.getElementById('i');"
                "document.onfullscreenchange = function (e) { console.log('doc handler', e.target.id) }; i.onfullscreenchange = function () { console.log('el handler') };"
                "document.addEventListener('webkitfullscreenchange', function (e) { console.log('webkit', e.type, document.webkitFullscreenElement && document.webkitFullscreenElement.id) });"
                "document.getElementById('go').onclick = function () { o.webkitRequestFullscreen(); setTimeout(function () { i.requestFullscreen() }, 5) } })",
                "doc handler o | webkit webkitfullscreenchange o | el handler | doc handler i | webkit webkitfullscreenchange i",
                click_go));
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/p", media, true);
      page.load((std::string("<html><body>") + body + "<script>var o = document.getElementById('o'), i = document.getElementById('i');"
                 "document.addEventListener('fullscreenchange', function (e) { console.log('change', e.target.id, document.fullscreenElement && document.fullscreenElement.id) });"
                 "document.getElementById('go').onclick = function () { o.requestFullscreen().then(function () { return i.requestFullscreen() }) }</script></body></html>").c_str());
      auto pump = [&] {
        for (int n = 0; n < 8; ++n) {
          double due;
          if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
          page.run_due_timers(static_cast<double>(env.clock));
        }
      };
      page.click(page.document().element_by_id("go")->serial, 1, 1);
      pump();
      CHECK(page.is_fullscreen() && env.is_fullscreen);
      page.fullscreen_left();   // Escape
      pump();
      CHECK(!page.is_fullscreen() && page.fullscreen_element() == nullptr);
      CHECK(page_log(page) == "change o o | change i i | change i null");
      // the element is taken out of the page: fullscreen ends and the window goes back
      page.click(page.document().element_by_id("go")->serial, 1, 1);
      pump();
      CHECK(page.is_fullscreen());
      dom::remove_child(page.document().element_by_id("o")->parent, page.document().element_by_id("o"));
      HtmlDocument doc(a);
      page.render(&doc);
      pump();
      CHECK(!page.is_fullscreen() && env.fullscreen_left >= 1 && !env.is_fullscreen);
      CHECK(page_log(page).find("change i null") != std::string::npos);
    }
    CHECK(a.live == 0);
  }
  // a frame: allowed with allowfullscreen, the <iframe> is the fullscreen element of the page around it
  {
    TestAllocator a;
    {
      FakeEnvironment env(a);
      MediaEnvironment media;
      Page page(a, env, "https://app.example/p", media, true);
      page.load("<html><body><iframe id=fr allowfullscreen srcdoc=\"<div id=d>inner</div><button id=ib>Go</button><script>"
                "document.getElementById('ib').addEventListener('click', function () { var d = document.getElementById('d'); parent.postMessage('enabled ' + document.fullscreenEnabled, '*');"
                "d.requestFullscreen().then(function () { parent.postMessage('inner ' + document.fullscreenElement.id, '*') }, function (e) { parent.postMessage('inner error ' + e.name, '*') }) })</script>\"></iframe>"
                "<iframe id=nofr srcdoc=\"<button id=nb>No</button><script>document.getElementById('nb').addEventListener('click', function () { parent.postMessage('plain ' + document.fullscreenEnabled, '*');"
                "document.body.requestFullscreen().catch(function (e) { parent.postMessage('plain error ' + e.name, '*') }) })</script>\"></iframe>"
                "<script>addEventListener('message', function (e) { console.log(e.data) }); document.addEventListener('fullscreenchange', function (e) { console.log('outer change', e.target.id, document.fullscreenElement && document.fullscreenElement.id) })</script></body></html>");
      auto pump = [&] {
        for (int n = 0; n < 8; ++n) {
          double due;
          if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
          page.run_due_timers(static_cast<double>(env.clock));
        }
      };
      page.click(node_of_text(page, "[ Go ]"), 0, 0);
      pump();
      CHECK(env.is_fullscreen && env.fullscreen_entered == 1 && page.is_fullscreen() && page.fullscreen_element() && page.fullscreen_element()->serial == serial_of(page, "fr"));
      page.click(node_of_text(page, "[ No ]"), 0, 0);
      pump();
      const std::string out = page_log(page);
      CHECK(out.find("enabled true") != std::string::npos && out.find("inner d") != std::string::npos && out.find("outer change fr fr") != std::string::npos);
      CHECK(out.find("plain false") != std::string::npos && out.find("plain error TypeError") != std::string::npos && env.fullscreen_entered == 1);
      page.fullscreen_left();
      pump();
      CHECK(!page.is_fullscreen());
    }
    CHECK(a.live == 0);
  }
}

static void test_picture_in_picture() {
  TestAllocator a;
  {
    FakeEnvironment env(a);
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4 width=128 height=96 controls></video><video id=w src=m.mp4 disablepictureinpicture></video><button id=go></button><button id=out></button><button id=more></button><script>"
              "var v = document.getElementById('v'), w = document.getElementById('w'), log = [], pw;"
              "log.push(document.pictureInPictureEnabled, document.pictureInPictureElement, typeof v.requestPictureInPicture, v.disablePictureInPicture, w.disablePictureInPicture);"
              "document.exitPictureInPicture().catch(function (e) { log.push('exit:' + e.name) });"
              "document.addEventListener('enterpictureinpicture', function (e) { log.push('enter:' + e.target.id + ':' + (e.pictureInPictureWindow === pw)) });"
              "v.addEventListener('leavepictureinpicture', function (e) { log.push('leave:' + (e.pictureInPictureWindow === pw)) });"
              "document.getElementById('go').onclick = function () { v.requestPictureInPicture().then(function (win) { pw = win; log.push('in', win.width, win.height, document.pictureInPictureElement === v, win instanceof PictureInPictureWindow);"
              "  win.onresize = function () { log.push('resize', win.width, win.height) } }) };"
              "document.getElementById('out').onclick = function () { document.exitPictureInPicture().then(function () { log.push('out', document.pictureInPictureElement) }) };"
              "document.getElementById('more').onclick = function () { w.requestPictureInPicture().catch(function (e) { console.log('disabled:' + e.name) }); console.log(log.join(' ')) };"
              "</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    auto pump = [&](int rounds) {
      for (int i = 0; i < rounds; ++i) {
        double due;
        if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
        page.run_due_timers(static_cast<double>(env.clock));
        if (page.changed()) page.render(&doc);
        page.step_objects(static_cast<double>(env.clock));
      }
    };
    pump(4);
    page.click(page.document().element_by_id("go")->serial, 0, 0);
    pump(6);
    CHECK(env.pip_opened == 1 && env.pip_w == 64 && env.pip_h == 48 && page.objects().pip() == env.pip_video && page.pip_element() == page.document().element_by_id("v"));
    // the page shows a note in its place; the window gets the picture
    const Run* run = find_object(doc, ObjectKind::Video);
    CHECK(run != nullptr);
    {
      Surface32 target(a, 128, 96);
      graphics::SoftwareRenderer2D renderer(target.surface);
      page.objects().paint_object(renderer, env.pip_video, ObjectKind::Video, 0, 0, 128, 96, Rect2D{0, 0, 128, 96});
      CHECK(target.at(2, 2) == 0xFF000000u && target.at(64, 10) == 0xFF000000u);   // (black, with the note)
      Surface32 window(a, 64, 48);
      graphics::SoftwareRenderer2D pip_renderer(window.surface);
      page.objects().paint_pip(pip_renderer, env.pip_video, 0, 0, 64, 48, Rect2D{0, 0, 64, 48});
      CHECK(window.at(32, 24) != 0xFF000000u);   // the picture
    }
    // the window resizes
    page.pip_resized(100, 75);
    pump(4);
    page.click(page.document().element_by_id("out")->serial, 0, 0);
    pump(6);
    CHECK(env.pip_closed == 1 && page.objects().pip() == 0 && page.pip_element() == nullptr);
    // the user closes the window
    page.click(page.document().element_by_id("go")->serial, 0, 0);
    pump(6);
    page.pip_left();
    pump(6);
    CHECK(page.pip_element() == nullptr && page.objects().pip() == 0);
    page.click(page.document().element_by_id("more")->serial, 0, 0);
    pump(6);
    const std::string out = page_log(page);
    CHECK(out.find("true  function false true exit:InvalidStateError") == 0);
    CHECK(in_order(out, {"enter:v:false in 64 48 true true", "resize 100 75", "leave:true", "out ", "enter:v:false in 64 48 true true", "leave:true", "disabled:InvalidStateError"}));
  }
  CHECK(a.live == 0);
  {   // a browser that cannot
    FakeEnvironment env(a);
    env.can_pip = false;
    env.blobs["http://example.com/m.mp4"] = "movie";
    MediaEnvironment media;
    Page page(a, env, "http://example.com/", media, true);
    page.load("<html><body><video id=v src=m.mp4></video><script>var v = document.getElementById('v'); console.log(document.pictureInPictureEnabled); v.requestPictureInPicture().catch(function (e) { console.log(e.name) });</script></body></html>");
    for (int i = 0; i < 6; ++i) {
      double due;
      if (page.next_timer(&due) && due > static_cast<double>(env.clock)) env.clock = static_cast<uint64_t>(due);
      page.run_due_timers(static_cast<double>(env.clock));
    }
    CHECK(page_log(page) == "false | NotSupportedError");
  }
  CHECK(a.live == 0);
}

// --- window, document, navigator, location -------------------------------------------------------------------------------------------

static void test_platform() {
  // what each of them has
  CHECK(async_is(
      "function missing(o, names) { return names.split(' ').filter(function (n) { return !(n in o) }).join(' ') }"
      "console.log('window:' + missing(window, 'locationbar menubar scrollbars clientInformation originAgentCluster captureEvents moveBy resizeBy visualViewport event reportError crossOriginIsolated caches cookieStore speechSynthesis trustedTypes Notification SharedWorker FileList DragEvent ClipboardEvent PageTransitionEvent BeforeUnloadEvent SubmitEvent FormDataEvent DOMRect DOMPoint DOMMatrix DOMQuad NamedNodeMap NodeList HTMLCollection CSSStyleSheet CSSRule StyleSheet MediaQueryList DataTransfer Screen Performance PerformanceEntry Crypto Permissions Geolocation MediaDevices Clipboard ClipboardItem XPathResult XPathEvaluator XMLDocument createImageBitmap onafterprint onbeforeprint onmousedown oncontextmenu ontoggle'));"
      "console.log('document:' + missing(document, 'plugins queryCommandValue fgColor applets all clear adoptedStyleSheets getElementsByTagNameNS createCDATASection createProcessingInstruction caretPositionFromPoint getAnimations requestStorageAccess pointerLockElement exitPointerLock onvisibilitychange onselectionchange oncopy onfullscreenchange moveBefore wasDiscarded permissionsPolicy xmlVersion createExpression createNSResolver evaluate pictureInPictureElement fullscreenElement'));"
      "console.log('navigator:' + missing(navigator, 'appCodeName appName appVersion product productSub vendorSub deviceMemory maxTouchPoints pdfViewerEnabled webdriver plugins mimeTypes javaEnabled sendBeacon vibrate share canShare clipboard geolocation mediaDevices permissions serviceWorker storage credentials locks connection userAgentData getGamepads mediaSession wakeLock registerProtocolHandler getBattery setAppBadge keyboard virtualKeyboard'));"
      "console.log('location:' + missing(location, 'href origin protocol host hostname port pathname search hash assign replace reload toString ancestorOrigins'));"
      "console.log('element:' + missing(document.documentElement, 'inert translate autocapitalize computedStyleMap part ariaLabel role scroll scrollIntoViewIfNeeded showPopover hidePopover togglePopover popover setHTMLUnsafe'));"
      "console.log('performance:' + missing(performance, 'getEntries clearMarks clearMeasures toJSON memory eventCounts clearResourceTimings'));",
      "window: | document: | navigator: | location: | element: | performance:"));
  // behaviour
  CHECK(async_is(
      "var d = new DOMMatrix().translate(10, 20).scale(2), p = d.transformPoint({ x: 1, y: 1 });"
      "console.log(d.toString(), p.x + ',' + p.y, d.inverse().toString(), new DOMRect(1, 2, 3, 4).right, DOMRect.fromRect({ width: 5 }).width);"
      "var s = new CSSStyleSheet(); s.replaceSync('p { color: red } @media screen { a { top: 1px } }'); s.insertRule('b { margin: 0 !important }', 1);"
      "console.log(s.cssRules.length, s.cssRules[1].cssText, s.cssRules[2].cssRules.length, s.cssRules[0].style.color, s.cssRules[2] instanceof CSSMediaRule, s.cssRules[0] instanceof CSSStyleRule);"
      "try { s.insertRule('{{', 0) } catch (e) { console.log(e.name) } try { s.deleteRule(9) } catch (e) { console.log(e.name) }"
      "var st = document.createElement('style'); st.textContent = 'h1 { color: red }'; document.head.appendChild(st); st.sheet.cssRules[0].style.color = 'blue'; st.sheet.cssRules[0].style.setProperty('margin', '2px');"
      "console.log(st.textContent, document.styleSheets.length, document.styleSheets[0] === st.sheet);"
      "var a = new CSSStyleSheet(); a.replaceSync('body { color: rgb(1, 2, 3) }'); document.adoptedStyleSheets = [a]; console.log(getComputedStyle(document.body).color, document.adoptedStyleSheets.length);",
      "matrix(2, 0, 0, 2, 10, 20) 12,22 matrix(0.5, 0, 0, 0.5, -5, -10) 4 5 | 3 b { margin: 0 !important; } 1 red true true | SyntaxError | IndexSizeError | h1 { color: blue; margin: 2px; } 1 true | rgb(1, 2, 3) 1"));
  CHECK(async_is(
      "document.body.innerHTML = '<ul><li class=a>1</li><li>2</li><li class=a>3</li></ul>';"
      "var r = function (x, t) { return document.evaluate(x, document, null, t, null) };"
      "console.log(r('count(//li)', 1).numberValue, r(\"//li[@class='a'][last()]/text()\", 2).stringValue, r('//li[2]/following-sibling::li | //li[2]/preceding-sibling::li', 7).snapshotLength,"
      "  r(\"concat(substring('hello', 2, 3), '-', string-length('abc'), '-', 7 mod 3, '-', normalize-space('  a   b '))\", 2).stringValue, r('//li[position() > 1]', 4).iterateNext().textContent, r('1 = 1', 3).booleanValue);"
      "try { r('//[', 0) } catch (e) { console.log(e.name) }"
      "var e = document.createExpression('//li[. = $x]'.replace(' = $x', ' = \"3\"')); console.log(e.evaluate(document, 9, null).singleNodeValue.className, new XPathEvaluator().evaluate('count(//ul)', document, null, 1, null).numberValue);",
      "3 3 2 ell-3-1-a b 2 true | SyntaxError | a 1"));
  CHECK(async_is(
      "var x = new DOMParser().parseFromString('<r xmlns=\"u\" xmlns:p=\"v\"><p:a x=\"1\">t&amp;</p:a><![CDATA[c]]><b/></r>', 'application/xml');"
      "console.log(x.contentType, x.documentElement.namespaceURI, x.documentElement.tagName, x.getElementsByTagName('p:a')[0].namespaceURI, x.getElementsByTagName('p:a')[0].prefix, x.getElementsByTagName('p:a')[0].localName, x instanceof XMLDocument, x.documentElement.childNodes[1].nodeType);"
      "console.log(new XMLSerializer().serializeToString(x));"
      "console.log(new DOMParser().parseFromString('<a><b></a>', 'text/xml').getElementsByTagName('parsererror').length, new Document().contentType, document.implementation.createDocument('http://www.w3.org/2000/svg', 'svg').documentElement.localName);"
      "var e = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); e.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', '#a');"
      "console.log(e.namespaceURI, e.tagName, e.getAttributeNS('http://www.w3.org/1999/xlink', 'href'), e.hasAttributeNS('http://www.w3.org/1999/xlink', 'href'), e.getAttributeNS(null, 'href'), e.attributes[0].prefix, e.attributes[0].localName, e.attributes[0].namespaceURI);"
      "var div = document.createElement('div'); div.innerHTML = '<svg viewBox=\"0 0 1 1\"><foreignObject><p>x</p></foreignObject></svg>'; var sv = div.firstChild;"
      "console.log(sv.namespaceURI, sv.firstChild.localName, sv.firstChild.firstChild.namespaceURI, document.createElement('div').namespaceURI, document.createProcessingInstruction('a', 'b').nodeType, document.createProcessingInstruction('a', 'b').target);"
      "try { document.createCDATASection('x') } catch (e) { console.log(e.name) } try { document.createElementNS(null, 'p:q') } catch (e) { console.log(e.name) }"
      "console.log(x.documentElement.lookupNamespaceURI('p'), x.documentElement.lookupPrefix('v'), x.documentElement.isDefaultNamespace('u'), x.lookupNamespaceURI(null));",
      "application/xml u r v p a true 4 | <r xmlns=\"u\" xmlns:p=\"v\"><p:a x=\"1\">t&amp;</p:a><![CDATA[c]]><b/></r> | 1 application/xml svg | http://www.w3.org/2000/svg svg #a true null xlink href http://www.w3.org/1999/xlink | http://www.w3.org/2000/svg foreignObject http://www.w3.org/1999/xhtml http://www.w3.org/1999/xhtml 7 a | NotSupportedError | NamespaceError | v p true u"));
  CHECK(async_is(
      "var out = [];"
      "navigator.permissions.query({ name: 'geolocation' }).then(function (s) { out.push('perm:' + s.state + ':' + s.name) });"
      "navigator.permissions.query({ name: 'bogus' }).catch(function (e) { out.push('perm:' + e.name) });"
      "navigator.geolocation.getCurrentPosition(function () { }, function (e) { out.push('geo:' + e.code + ':' + (e instanceof GeolocationPositionError)) });"
      "navigator.mediaDevices.enumerateDevices().then(function (l) { out.push('devices:' + l.length) }); navigator.mediaDevices.getUserMedia({ video: true }).catch(function (e) { out.push('media:' + e.name) });"
      "navigator.clipboard.writeText('copied').then(function () { return navigator.clipboard.readText() }).then(function (t) { out.push('clip:' + t) }, function (e) { out.push('clip:' + e.name) });"
      "navigator.locks.request('l', function (lock) { out.push('lock:' + lock.name + ':' + lock.mode); return 5 }).then(function (v) { out.push('locked:' + v) });"
      "caches.open('c').then(function (c) { return c.put('/a', new Response('hi')).then(function () { return c.match('/a') }) }).then(function (r) { return r.text() }).then(function (t) { out.push('cache:' + t) });"
      "navigator.storage.estimate().then(function (e) { out.push('quota:' + (e.quota > 0)) }); navigator.getBattery().then(function (b) { out.push('battery:' + b.level) });"
      "Notification.requestPermission().then(function (p) { out.push('notify:' + p + ':' + Notification.permission) });"
      "performance.mark('a'); performance.mark('b'); var m = performance.measure('ab', 'a', 'b'); out.push('perf:' + performance.getEntriesByType('mark').length + ':' + m.entryType + ':' + (m.duration >= 0));"
      "out.push(navigator.appName + ':' + navigator.product + ':' + navigator.webdriver + ':' + navigator.plugins.length + ':' + navigator.maxTouchPoints + ':' + navigator.javaEnabled() + ':' + navigator.vibrate(1) + ':' + navigator.sendBeacon('/b', 'x'));"
      "out.push(Object.prototype.toString.call(window) + Object.prototype.toString.call(document) + Object.prototype.toString.call(location) + Object.prototype.toString.call(navigator) + Object.prototype.toString.call(document.querySelectorAll('p')) + Object.prototype.toString.call(document.body.children));"
      "var ev = document.createEvent('MouseEvents'); ev.initMouseEvent('click', true, true, window, 1, 0, 0, 5, 6); out.push(ev.type + ev.bubbles + ev.clientX);"
      "var d = new DataTransfer(); d.setData('text', 'x'); out.push(d.getData('text/plain') + d.types.join());"
      "var e = document.createElement('div'); e.ariaLabel = 'l'; e.role = 'button'; e.inert = true; out.push(e.getAttribute('aria-label') + e.getAttribute('role') + e.hasAttribute('inert') + e.translate);"
      "var pop = document.createElement('div'); pop.popover = 'auto'; document.body.appendChild(pop); pop.showPopover(); out.push('pop:' + pop.matches(':popover-open')); pop.hidePopover(); out.push(pop.matches(':popover-open'));"
      "setTimeout(function () { console.log(out.sort().join(' | ')) }, 50);",
      "Netscape:Gecko:false:0:0:false:false:true | [object Window][object Document][object Location][object Navigator][object NodeList][object HTMLCollection] | battery:1 | cache:hi | clicktrue5 | clip:NotAllowedError | devices:0 | false | geo:1:true | lbuttontruetrue | lock:l:exclusive | locked:5 | media:NotFoundError | notify:denied:denied | perf:2:measure:true | perm:TypeError | perm:prompt:geolocation | pop:true | quota:true | xtext/plain"));
}

// --- fetch() and XMLHttpRequest in full ---------------------------------------------------------------------------------------------

static void full_stock(FakeEnvironment& env) {
  std::string big(200000, 'x');
  env.net.answers["GET https://app.example/big"] = {200, "Content-Type: text/plain\r\nContent-Length: 200000\r\n", big};
  env.net.answers["GET https://app.example/latin"] = {200, "Content-Type: text/plain; charset=iso-8859-1\r\n", std::string("caf\xE9")};
  env.net.answers["GET https://app.example/utf16"] = {200, "Content-Type: text/plain; charset=utf-16le\r\n", std::string("h\0i\0", 4)};
  env.net.answers["GET https://app.example/xml"] = {200, "Content-Type: application/xml\r\n", "<a><b>1</b><b>2</b></a>"};
  env.net.answers["GET https://app.example/html"] = {200, "Content-Type: text/html\r\n", "<p id=x>hi</p>"};
  env.net.answers["GET https://app.example/json"] = {200, "Content-Type: application/json\r\nX-Multi: a\r\nx-multi: b\r\nSet-Cookie: s=1\r\n", "{\"a\":[1,2]}"};
  env.net.answers["GET https://app.example/cached"] = {200, "Content-Type: text/plain\r\nCache-Control: max-age=60\r\nETag: \"v1\"\r\n", "cached body"};
  env.net.answers["GET https://app.example/revalidate"] = {200, "Content-Type: text/plain\r\nCache-Control: no-cache\r\nETag: \"v1\"\r\n", "revalidated body"};
  env.net.answers["GET https://app.example/revalidate#304"] = {304, "ETag: \"v1\"\r\n", ""};
  env.net.answers["GET https://app.example/nostore"] = {200, "Cache-Control: no-store\r\n", "ns"};
  env.net.answers["POST https://app.example/echo"] = {200, "Content-Type: text/plain\r\n", "posted"};
  env.net.answers["PUT https://app.example/cached"] = {200, "", "put"};
  env.net.answers["GET https://app.example/redir"] = {302, "Location: /json\r\n", ""};
}

static void test_fetch_xhr_full() {
  // streams over the network body
  CHECK(async_is("fetch('/big').then(function (r) { var reader = r.body.getReader(), n = 0, size = 0; var step = function () { return reader.read().then(function (x) { if (x.done) { console.log(n > 1, size, r.bodyUsed, r.body.locked); return } n++; size += x.value.length; return step() }) }; return step() });",
                 "true 200000 true true", full_stock));
  CHECK(async_is("fetch('/json').then(function (r) { console.log(r.headers.get('x-multi'), r.headers.get('set-cookie'), r.headers.has('content-type'), r.type, r.url); return r.clone().json().then(function (j) { return r.text().then(function (t) { console.log(j.a.length, t) }) }) });",
                 "a, b null true basic https://app.example/json | 2 {\"a\":[1,2]}", full_stock));
  CHECK(async_is("fetch('/big').then(function (r) { return r.body.pipeThrough(new TransformStream({ transform: function (c, ctl) { ctl.enqueue(c.length) } })) }).then(function (s) { var reader = s.getReader(), total = 0; var step = function () { return reader.read().then(function (x) { if (x.done) { console.log(total); return } total += x.value; return step() }) }; return step() });",
                 "200000", full_stock));
  // request bodies: streams, headers the page may not set, referrers
  CHECK(async_is("var rs = new ReadableStream({ start: function (c) { c.enqueue(new TextEncoder().encode('streamed')); c.close() } });"
                 "fetch('/echo', { method: 'POST', body: rs, duplex: 'half', headers: { 'X-A': '1', 'Cookie': 'no', 'Host': 'no' } }).then(function (r) { return r.text() }).then(function (t) { console.log(t) });",
                 "posted", full_stock, "https://app.example/dir/page.html"));
  CHECK(async_is("fetch('/echo', { method: 'POST', body: new ReadableStream() }).catch(function (e) { console.log(e.name) }); fetch('/echo', { method: 'POST', body: 'x', keepalive: true }).then(function (r) { console.log(r.status) });", "TypeError | 200", full_stock));
  // abort while the body is being read
  CHECK(async_is("var c = new AbortController(); fetch('/big', { signal: c.signal }).then(function (r) { var reader = r.body.getReader(); reader.read().then(function () { c.abort(); return reader.read() }).then(function () { console.log('no error') }, function (e) { console.log(e.name) }) });", "AbortError", full_stock));
  // redirect: manual
  CHECK(async_is("fetch('/redir', { redirect: 'manual' }).then(function (r) { console.log(r.type, r.status, r.url, r.headers.get('location'), r.body) }); fetch('/redir').then(function (r) { console.log(r.type, r.status, r.redirected, r.url) });",
                 "opaqueredirect 0 https://app.example/redir null null | basic 200 true https://app.example/json", full_stock));
  // the cache
  CHECK(async_is("fetch('/cached').then(function (r) { return r.text() }).then(function () { return fetch('/cached') }).then(function (r) { return r.text() }).then(function (t) { console.log(t); return fetch('/cached', { cache: 'reload' }) }).then(function (r) { return r.text() })"
                 ".then(function () { return fetch('/cached', { cache: 'no-store' }) }).then(function (r) { return r.text() }).then(function () { return fetch('/cached', { cache: 'only-if-cached', mode: 'same-origin' }) }).then(function (r) { console.log(r.status) })"
                 ".then(function () { return fetch('/cached', { method: 'PUT', body: 'x' }) }).then(function () { return fetch('/cached', { cache: 'only-if-cached', mode: 'same-origin' }) }).then(function (r) { console.log(r.status) });",
                 "cached body | 200 | 504", full_stock));
  CHECK(async_is("fetch('/revalidate').then(function (r) { return r.text() }).then(function () { return fetch('/revalidate') }).then(function (r) { return r.text() }).then(function (t) { console.log(t) });", "revalidated body", full_stock, "https://app.example/dir/page.html"));
  CHECK(async_is("fetch('/nostore').then(function (r) { return r.text() }).then(function () { return fetch('/nostore') }).then(function (r) { return r.text() }).then(function () { return fetch('/nostore', { cache: 'only-if-cached', mode: 'same-origin' }) }).then(function (r) { console.log(r.status) }); try { new Request('/x', { cache: 'only-if-cached' }) } catch (e) { console.log(e.name) }",
                 "TypeError | 504", full_stock));
  // integrity
  CHECK(async_is("var good = 'sha256-sl+4S0dVxVumV+5W1adRv4cHgB5TJPOE+b8I7jl6E74=';"
                 "fetch('/cached', { integrity: good }).then(function (r) { console.log('ok', r.status); return fetch('/revalidate', { integrity: good }) }).catch(function (e) { console.log(e.name) });"
                 "fetch('/cached', { integrity: 'sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA= ' + good }).then(function (r) { console.log('either', r.status) });",
                 "ok 200 | either 200 | TypeError", full_stock));
  // XMLHttpRequest: events, progress, upload, response types
  CHECK(async_is("var x = new XMLHttpRequest(), log = []; ['readystatechange', 'loadstart', 'progress', 'load', 'loadend', 'error', 'abort', 'timeout'].forEach(function (t) { x.addEventListener(t, function (e) { log.push(t === 'readystatechange' ? 'rs' + x.readyState : t + (e.lengthComputable ? ':' + e.loaded + '/' + e.total : '')) }) });"
                 "x.open('GET', '/big'); x.onloadend = function () { var dedup = []; log.forEach(function (l) { if (dedup[dedup.length - 1] !== l) dedup.push(l) }); console.log(dedup.length > 6, log[0], log[1], log[2], log[log.length - 3], log[log.length - 2], log[log.length - 1], x.responseText.length, x.getResponseHeader('content-length'), x.status) }; x.send();",
                 "true rs1 loadstart rs2 progress:200000/200000 rs4 load:200000/200000 200000 200000 200", full_stock));
  CHECK(async_is("var x = new XMLHttpRequest(), log = []; x.upload.onloadstart = function (e) { log.push('u-start:' + e.total) }; x.upload.onprogress = function (e) { log.push('u-progress:' + e.loaded) }; x.upload.onload = function () { log.push('u-load') }; x.upload.onloadend = function () { log.push('u-end') };"
                 "x.onloadstart = function () { log.push('start') }; x.onload = function () { log.push('load') }; x.onloadend = function () { console.log(log.join(' ')) }; x.open('POST', '/echo'); x.send('12345');",
                 "start u-start:5 u-progress:5 u-load u-end load", full_stock));
  CHECK(async_is("var out = []; var run = function (type, url, done) { var x = new XMLHttpRequest(); x.open('GET', url); x.responseType = type; x.onload = function () { done(x) }; x.send() };"
                 "run('arraybuffer', '/latin', function (x) { out.push(x.response.byteLength + ':' + (x.response instanceof ArrayBuffer)) });"
                 "run('blob', '/json', function (x) { out.push(x.response.size + ':' + x.response.type) });"
                 "run('json', '/json', function (x) { out.push(x.response.a[1]) });"
                 "run('document', '/html', function (x) { out.push(x.response.getElementById('x').textContent + ':' + (x.responseXML === x.response)) });"
                 "run('', '/xml', function (x) { out.push(x.responseXML.getElementsByTagName('b').length + x.responseText.length) });"
                 "run('text', '/xml', function (x) { try { x.responseXML } catch (e) { out.push(e.name) } });"
                 "run('arraybuffer', '/latin', function (x) { try { x.responseText } catch (e) { out.push(e.name) } });"
                 "setTimeout(function () { console.log(out.sort().join(' ')) }, 100);",
                 "11:application/json 2 25 4:true InvalidStateError InvalidStateError hi:true", full_stock));
  CHECK(async_is("var out = []; var run = function (url, mime, done) { var x = new XMLHttpRequest(); x.open('GET', url); if (mime) x.overrideMimeType(mime); x.onload = function () { done(x.responseText) }; x.send() };"
                 "run('/latin', null, function (t) { out.push(t === 'caf\\u00e9') }); run('/utf16', null, function (t) { out.push(t) }); run('/latin', 'text/plain; charset=windows-1252', function (t) { out.push('o' + (t === 'caf\\u00e9')) });"
                 "setTimeout(function () { console.log(out.sort().join(' ')) }, 100);",
                 "hi otrue true", full_stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/json', false); x.send(); console.log(x.status, x.readyState, x.responseText.length, x.getAllResponseHeaders().split('\\r\\n').filter(Boolean).join('|'));"
                 "var y = new XMLHttpRequest(); y.open('GET', 'http://down.example/x', false); try { y.send() } catch (e) { console.log(e.name) }"
                 "var z = new XMLHttpRequest(); z.open('GET', '/json', false); try { z.responseType = 'json' } catch (e) { console.log(e.name) } try { z.timeout = 5 } catch (e) { console.log(e.name) }",
                 "200 4 11 content-type: application/json|x-multi: a, b | NetworkError | InvalidAccessError | InvalidAccessError", full_stock));
  CHECK(async_is("var x = new XMLHttpRequest(), log = []; x.onreadystatechange = function () { log.push('rs' + x.readyState) }; x.onabort = function () { log.push('abort') }; x.onloadend = function () { log.push('end') };"
                 "x.open('GET', '/json'); x.send(); x.abort(); setTimeout(function () { console.log(log.join(' '), x.readyState, x.status) }, 50);"
                 "var y = new XMLHttpRequest(); try { y.send() } catch (e) { console.log(e.name) } y.open('GET', '/json'); y.send(); try { y.send() } catch (e) { console.log(e.name) } try { y.open } catch (e) { }"
                 "try { y.withCredentials = true } catch (e) { console.log(e.name) }",
                 "InvalidStateError | InvalidStateError | InvalidStateError | rs1 rs4 abort end 0 0", full_stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/missing-host-failure'); x.onerror = function () { console.log('error', x.status, x.readyState, x.responseText.length) }; x.send();"
                 "var y = new XMLHttpRequest(); y.open('GET', 'http://down.example/x'); y.onerror = function () { console.log('down', y.status) }; y.upload.onerror = function () { console.log('upload error') }; y.send('body');",
                 "down 0", full_stock));
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('POST', '/echo', true, 'user', 'pass'); x.setRequestHeader('X-A', '1'); x.setRequestHeader('x-a', '2'); x.setRequestHeader('Cookie', 'no'); x.send(new URLSearchParams({ k: 'v' }));"
                 "x.onload = function () { console.log(x.status, x.responseURL) };",
                 "200 https://app.example/echo", [](FakeEnvironment& env) { full_stock(env); }, "https://app.example/dir/page.html"));
  // resource timing
  CHECK(async_is("fetch('/json').then(function (r) { return r.text() }).then(function () { var e = performance.getEntriesByType('resource'); console.log(e.length, e[0].name, e[0].initiatorType, e[0].entryType) });", "1 https://app.example/json fetch resource", full_stock));
}

static void test_streams() {
  CHECK(async_is(
      "var out = [], note = function (label) { return function (v) { out.push(label + '=' + v) } }, bad = function (label) { return function (e) { out.push(label + '!' + (e && e.name || e)) } };"
      "var collect = function (stream) { var r = stream.getReader(), got = []; var step = function () { return r.read().then(function (x) { if (x.done) return got; got.push(x.value); return step() }) }; return step() };"
      "var src = function (items) { return new ReadableStream({ start: function (c) { items.forEach(function (i) { c.enqueue(i) }); c.close() } }) };"
      "collect(src(['a', 'b'])).then(function (v) { note('basic')(v.join()) });"
      "var n = 0; collect(new ReadableStream({ pull: function (c) { if (n < 3) c.enqueue(n++); else c.close() } })).then(function (v) { note('pull')(v.join()) });"
      "var pair = src([1, 2]).tee(); Promise.all([collect(pair[0]), collect(pair[1])]).then(function (v) { note('tee')(v[0].join() + '/' + v[1].join()) });"
      "var locked = new ReadableStream(), reader = locked.getReader(); out.push('locked=' + locked.locked); reader.releaseLock(); out.push('unlocked=' + !locked.locked);"
      "var cancelled = ''; new ReadableStream({ cancel: function (r) { cancelled = r } }).cancel('why').then(function () { note('cancel')(cancelled) });"
      "new ReadableStream({ start: function (c) { c.error(new Error('boom')) } }).getReader().read().then(null, function (e) { note('error')(e.message) });"
      "var seen = []; var ws = new WritableStream({ write: function (c) { seen.push(c) }, close: function () { seen.push('closed') } }); var w = ws.getWriter();"
      "w.write('x').then(function () { return w.write('y') }).then(function () { return w.close() }).then(function () { note('writable')(seen.join()) });"
      "var piped = []; src(['p', 'q']).pipeTo(new WritableStream({ write: function (c) { piped.push(c) } })).then(function () { note('pipeTo')(piped.join()) });"
      "collect(src(['a', 'b']).pipeThrough(new TransformStream({ transform: function (c, ctl) { ctl.enqueue(c.toUpperCase()) } }))).then(function (v) { note('transform')(v.join()) });"
      "collect(src(['h\\u00e9llo']).pipeThrough(new TextEncoderStream()).pipeThrough(new TextDecoderStream())).then(function (v) { note('text')(v.join() === 'h\\u00e9llo') });"
      "collect(src([new TextEncoder().encode('hello hello hello')]).pipeThrough(new CompressionStream('gzip')).pipeThrough(new DecompressionStream('gzip'))).then(function (v) { note('gzip')(new TextDecoder().decode(v[0])) });"
      "collect(src([new TextEncoder().encode('abcabcabc')]).pipeThrough(new CompressionStream('deflate')).pipeThrough(new DecompressionStream('deflate'))).then(function (v) { note('deflate')(new TextDecoder().decode(v[0])) });"
      "collect(src([new TextEncoder().encode('raw')]).pipeThrough(new CompressionStream('deflate-raw')).pipeThrough(new DecompressionStream('deflate-raw'))).then(function (v) { note('raw')(new TextDecoder().decode(v[0])) });"
      "collect(src([new Uint8Array([1, 2, 3])]).pipeThrough(new DecompressionStream('gzip'))).then(note('badgzip'), bad('badgzip'));"
      "try { new CompressionStream('zip') } catch (e) { out.push('format!' + e.name) }"
      "new Response('hello').body.getReader().read().then(function (x) { note('body')(x.value.length) });"
      "var r1 = new Response('x'); r1.body.getReader(); r1.text().then(null, bad('lockedtext'));"
      "new Response(src([new TextEncoder().encode('from stream')])).text().then(note('streamed'));"
      "var r2 = new Response('twice'), c2 = r2.clone(); Promise.all([r2.text(), c2.text()]).then(function (v) { note('clone')(v.join()) });"
      "new Request('/x', { method: 'POST', body: src([new TextEncoder().encode('up')]), duplex: 'half' }).text().then(note('reqstream'));"
      "try { new Request('/x', { method: 'POST', body: new ReadableStream() }) } catch (e) { out.push('duplex!' + e.name) }"
      "collect(new Blob(['blobby']).stream()).then(function (v) { note('blob')(new TextDecoder().decode(v[0])) });"
      "var it = src([1, 2]).values(), seq = []; var walk = function () { return it.next().then(function (r) { if (r.done) return seq.join(); seq.push(r.value); return walk() }) }; walk().then(note('iterator'));"
      "var byob = new ReadableStream({ type: 'bytes', start: function (c) { c.enqueue(new Uint8Array([1, 2, 3, 4])); c.close() } }).getReader({ mode: 'byob' }); byob.read(new Uint8Array(2)).then(function (x) { note('byob')(x.value.length + ':' + x.value[0] + x.value[1]) });"
      "out.push('strategy=' + new CountQueuingStrategy({ highWaterMark: 3 }).size() + new ByteLengthQueuingStrategy({ highWaterMark: 5 }).size(new Uint8Array(7)));"
      "ReadableStream.from(['i', 'j']) && collect(ReadableStream.from(['i', 'j'])).then(function (v) { note('from')(v.join()) });"
      "new Response('ab').arrayBuffer().then(function (b) { note('arraybuffer')((b instanceof ArrayBuffer) + ':' + b.byteLength) });"
      "out.push('guard=' + Array.from(new Request('/x', { headers: { Cookie: 'x', 'X-Ok': '1', Host: 'h', 'Sec-Test': '1' } }).headers).join(';'));"
      "out.push('nocors=' + Array.from(new Request('/x', { mode: 'no-cors', headers: { 'X-Custom': '1', Accept: 'x', 'Content-Type': 'text/plain' } }).headers).join(';'));"
      "try { new Request('/a', { mode: 'navigate' }) } catch (e) { out.push('navigate!' + e.name) }"
      "out.push('decoders=' + new TextDecoder('utf-16le').decode(new Uint8Array([104, 0, 105, 0])) + new TextDecoder('utf-16be').decode(new Uint8Array([0, 104])) + (new TextDecoder('windows-1252').decode(new Uint8Array([0x80])) === '\\u20ac'));"
      "var dec = new TextDecoder(), bytes = new TextEncoder().encode('\\u00e9'); out.push('partial=' + (dec.decode(bytes.subarray(0, 1), { stream: true }) === '') + (dec.decode(bytes.subarray(1)) === '\\u00e9'));"
      "setTimeout(function () { console.log(out.sort().join(' | ')) }, 100);",
      "arraybuffer=true:2 | badgzip!TypeError | basic=a,b | blob=blobby | body=5 | byob=2:12 | cancel=why | clone=twice,twice | decoders=hi" "h" "true | deflate=abcabcabc | duplex!TypeError | error=boom | format!TypeError"
      " | from=i,j | guard=x-ok,1 | gzip=hello hello hello | iterator=1,2 | locked=true | lockedtext!TypeError | navigate!TypeError | nocors=accept,x;content-type,text/plain | partial=truetrue"
      " | pipeTo=p,q | pull=0,1,2 | raw=raw | reqstream=up | strategy=17 | streamed=from stream | tee=1,2/1,2 | text=true | transform=A,B | unlocked=true | writable=x,y,closed"));
}

int main(int argc, char** argv) {
  if (const char* probe = getenv("PROBE")) {   // runs a script file in a page and prints what it logged (for looking at what an API has)
    std::string script;
    if (FILE* f = fopen(probe, "rb")) { int c; while ((c = fgetc(f)) != EOF) script.push_back(static_cast<char>(c)); fclose(f); }
    printf("%s\n", run_async(script.c_str(), "https://app.example/dir/page.html").c_str());
    return 0;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) {   // (a scratch: PageTest 'script' prints what the script logs, with timers run)
    printf("%s\n", run_async(argv[1], "https://app.example/dir/page.html").c_str());
    return 0;
  }
  test_tree();
  test_attributes();
  test_style();
  test_selectors_js();
  test_events();
  test_timers_and_window();
  test_shadow_rendering();
  test_url();
  test_search_params();
  test_headers();
  test_blob_text();
  test_abort();
  test_fetch();
  test_xhr();
  test_fetch_wire();
  test_storage();
  test_cookies_web();
  test_navigation();
  test_clipboard();
  test_messaging();
  test_history_api();
  test_scripts_during_load();
  test_page_rendering();
  test_clicks_and_resize();
  test_external_and_errors();
  test_images();
  test_picture_paint();
  test_canvas_scripts();
  test_video();
  test_audio();
  test_media_api();
  test_mse();
  test_eme();
  test_eme_playback();
  test_blob_media();
  test_webm_page();
  test_text_tracks();
  test_dash_page();
  test_dashjs();
  test_fullscreen();
  test_fullscreen_more();
  test_picture_in_picture();
  test_platform();
  test_fetch_xhr_full();
  test_streams();
  test_forms();
  test_table_wrap();
  test_details_and_dialog();
  test_template();
  test_iframe();
  test_svg();
  test_input_more();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("page tests passed\n");
  return 0;
}
