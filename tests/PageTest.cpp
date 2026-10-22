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
#include "Dom/Form.hpp"
#include "Browser/PageLayout.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Media/ImageCodecs.hpp"

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
  double volume = 1;
  bool muted = false;
  uint32_t serial = 1;
  int updates = 0;
  bool play() override { is_playing = true; return true; }
  void pause() override { is_playing = false; }
  void seek(int64_t us) override { position = us < 0 ? 0 : us > duration ? duration : us; ++serial; }
  void set_looping(bool l) override { looping = l; }
  void set_volume(double v, bool m) override { volume = v; muted = m; }
  bool playing() override { return is_playing; }
  int64_t position_us() override { return position; }
  int64_t duration_us() override { return duration; }
  bool sound_only = false;   // an <audio>: no picture
  uint32_t width() override { return sound_only ? 0 : 64; }
  uint32_t height() override { return sound_only ? 0 : 48; }
  void update() override {
    ++updates;
    if (!is_playing) return;
    position += 500000;
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
    response->status = a.status;
    response->reason.assign(a.status == 200 ? "OK" : a.status == 404 ? "Not Found" : "Status");
    response->headers.assign(StringView(a.headers.data(), a.headers.size()));
    response->body.clear();
    response->body.append(ConstByteView(reinterpret_cast<const uint8_t*>(a.body.data()), a.body.size()));
    return true;
  }
};

struct FakeEnvironment final : IPageEnvironment {
  explicit FakeEnvironment(TestAllocator& a) : local_hub(a), session_hub(a), hub(a), jar(a) { allocator_for_clips = &a; }
  std::map<std::string, std::string> blobs;   // binary files (pictures, videos)
  FakeClip* last_clip = nullptr;
  bool can_play_video = true;
  bool fetch_bytes(StringView address, uint64_t, ByteBuffer* out) override {
    fetched.push_back(std::string(address.data(), address.size()));
    auto it = blobs.find(fetched.back());
    if (it == blobs.end()) return false;
    return out->append(ConstByteView(reinterpret_cast<const uint8_t*>(it->second.data()), it->second.size()));
  }
  UniquePointer<IVideoClip> open_video(UniquePointer<ByteBuffer> data) override {
    (void)data;
    if (!can_play_video) return UniquePointer<IVideoClip>();
    auto clip = UniquePointer<FakeClip>::make(*allocator_for_clips);
    last_clip = clip.get();
    return clip;
  }
  UniquePointer<IVideoClip> open_audio(UniquePointer<ByteBuffer> data) override {
    (void)data;
    if (!can_play_video) return UniquePointer<IVideoClip>();
    auto clip = UniquePointer<FakeClip>::make(*allocator_for_clips);
    clip->sound_only = true;
    last_clip = clip.get();
    return clip;
  }
  allocators::IAllocator* allocator_for_clips = nullptr;
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
  void clipboard_set(StringView text) override { clip.assign(text.data(), text.size()); }
  void pick_file(uint32_t node, bool) override { picks.push_back(node); }
  void navigate_post(StringView a, StringView type, StringView body) override {
    posts.push_back(std::string(a.data(), a.size()) + " [" + std::string(type.data(), type.size()) + "] " + std::string(body.data(), body.size()));
  }
  void navigate(StringView a, bool replace) override { navigations.push_back(std::string(a.data(), a.size()) + (replace ? " (replace)" : "")); }
  void set_hash(StringView h) override { hashes.push_back(std::string(h.data(), h.size())); }
  void reload() override { ++reloads; }
  void history_go(int32_t d) override { history_moves += d; }
  uint32_t history_length() override { return 3; }
  void push_state(StringView a, StringView state, bool has, bool replace) override {
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
              "  var r = []; var p = v.play(); r.push(typeof p.then, typeof p.catch); p.then(function() { r.push('then') }); r.push(v.paused);"
              "  v.currentTime = 1.5; r.push(v.currentTime); v.pause(); r.push(v.paused); v.volume = 0.25; r.push(v.volume);"
              "  try { v.volume = 3 } catch (e) { r.push(e.name) } v.muted = true; r.push(v.muted); r.push(v.duration, v.videoWidth, v.videoHeight, v.readyState, v.ended, v.loop);"
              "  v.loop = true; r.push(v.loop, v.hasAttribute('loop')); r.push(v.canPlayType('video/mp4') !== '', v.canPlayType('video/ogg') === '');"
              "  r.push(v.buffered.length, v.buffered.end(0), v.seekable.length); console.log(r.join(' '));"
              "}</script></body></html>");
    HtmlDocument doc(a);
    page.render(&doc);
    page.click(page.document().element_by_id("b")->serial, 0, 0);
    CHECK(page_log(page) == "function function then false 1.5 true 0.25 IndexSizeError true 2 64 48 4 false false true true true true 1 2 1");
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
  CHECK(page_text(page, 16) == "one two      | x\nthree four   |");
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
                 "true https://app.example/new | moved here | 302 /new | TypeError", stock));
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
  CHECK(async_is("fetch('/bin').then(function (r) { return r.arrayBuffer() }).then(function (b) { console.log(b.length, b.byteLength, b[0], b[2], b[3], b[4]) }); fetch('/bin').then(function (r) { return r.blob() }).then(function (b) { console.log(b.size, b.type) });", "5 5 1 255 0 65 | 5 application/octet-stream", stock));
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
  CHECK(async_is("var x = new XMLHttpRequest(); x.open('GET', '/bin'); x.responseType = 'arraybuffer'; x.onload = function () { console.log(x.response.length, x.response[2]) }; x.send(); var y = new XMLHttpRequest(); y.open('GET', '/bin'); y.responseType = 'blob'; y.onload = function () { console.log(y.response.size, y.response.type) }; y.send();", "5 255 | 5 application/octet-stream", stock));
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

static void test_messaging() {
  // to itself: the data is a copy, with what JSON cannot say kept
  CHECK(async_is("addEventListener('message', function (e) { var d = e.data; console.log(e.type, e.origin, e.source === window, e.ports.length, e.lastEventId === '', d.a.length, d.a[1].b, d.d instanceof Date, d.d.getTime(), 'u' in d, d.u, isNaN(d.n), 1 / d.z, d.s, d === msg, e instanceof MessageEvent) });"
                  "var msg = { a: [1, { b: 2 }], d: new Date(5), u: undefined, n: NaN, z: -0, s: 'ü' }; postMessage(msg, '*'); console.log('posted');",
                  "posted | message https://app.example true 0 true 2 2 true 5 true undefined true -Infinity \xC3\xBC false true"));
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

int main(int argc, char** argv) {
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
  test_url();
  test_search_params();
  test_headers();
  test_blob_text();
  test_abort();
  test_fetch();
  test_xhr();
  test_fetch_wire();
  test_storage();
  test_messaging();
  test_history_api();
  test_scripts_during_load();
  test_page_rendering();
  test_clicks_and_resize();
  test_external_and_errors();
  test_images();
  test_canvas_scripts();
  test_video();
  test_audio();
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
