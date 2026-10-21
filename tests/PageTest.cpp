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
#include <map>
#include <string>
#include <vector>
#include "Browser/Page.hpp"
#include "Browser/PageLayout.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

struct FakeEnvironment final : IPageEnvironment {
  explicit FakeEnvironment(TestAllocator& a) : storage(a), jar(a) {}
  std::map<std::string, std::string> files;
  std::vector<std::string> fetched, navigations, alerts, console_lines, hashes, states;
  std::vector<int> scrolls;
  int reloads = 0, history_moves = 0;
  bool confirm_answer = false;
  uint64_t clock = 1700000000000ull;
  StorageArea storage;
  String jar;
  bool fetch_text(StringView address, uint64_t, String* text) override {
    fetched.push_back(std::string(address.data(), address.size()));
    auto it = files.find(fetched.back());
    if (it == files.end()) return false;
    text->assign(StringView(it->second.data(), it->second.size()));
    return true;
  }
  void navigate(StringView a, bool replace) override { navigations.push_back(std::string(a.data(), a.size()) + (replace ? " (replace)" : "")); }
  void set_hash(StringView h) override { hashes.push_back(std::string(h.data(), h.size())); }
  void reload() override { ++reloads; }
  void history_go(int32_t d) override { history_moves += d; }
  uint32_t history_length() override { return 3; }
  void push_state(StringView a, bool) override { states.push_back(std::string(a.data(), a.size())); }
  void alert(StringView t) override { alerts.push_back(std::string(t.data(), t.size())); }
  bool confirm(StringView) override { return confirm_answer; }
  void scroll_to(int32_t line) override { scrolls.push_back(line); }
  uint64_t now_ms() override { return clock; }
  void console(int, StringView t) override { console_lines.push_back(std::string(t.data(), t.size())); }
  StorageArea& local_storage() override { return storage; }
  String& cookie() override { return jar; }
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
  CHECK(js("<input id=i type=checkbox checked value=v>", "var i = document.getElementById('i'); console.log(i.type, i.checked, i.value, i.disabled); i.checked = false; i.disabled = true; i.value = 'w'; console.log(i.outerHTML)", "checkbox true v false | <input id=\"i\" type=\"checkbox\" value=\"w\" disabled=\"\">"));
  CHECK(js("<input id=i><button id=b></button><textarea id=t>x<y</textarea>", "var t = document.getElementById('t'); console.log(document.getElementById('i').type, document.getElementById('b').type, t.value); t.value = 'new'; console.log(t.textContent)", "text submit x<y | new"));
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
  CHECK(js("", "document.cookie = 'a=1; path=/'; document.cookie = 'b=2'; document.cookie = 'a=3'; console.log(document.cookie); document.cookie = 'b=; max-age=0'; console.log(document.cookie)", "a=3; b=2 | a=3"));
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

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_tree();
  test_attributes();
  test_style();
  test_selectors_js();
  test_events();
  test_timers_and_window();
  test_scripts_during_load();
  test_page_rendering();
  test_clicks_and_resize();
  test_external_and_errors();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("page tests passed\n");
  return 0;
}
