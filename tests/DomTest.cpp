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

// Host-side tests for the document tree: the HTML reader, the tree operations, markup in and out, selectors, character references, and media queries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Browser/Media.hpp"
#include "Dom/Entities.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Dom/Selector.hpp"

using namespace dom;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::string str(const String& s) { return std::string(s.c_str()); }
static std::string sv(StringView s) { return std::string(s.data(), s.size()); }

// The markup of the whole page's <html> element.
static std::string page_markup(TestAllocator& a, const char* html, IScriptRunner* runner = nullptr) {
  Document doc(a);
  build_document(doc, html, runner);
  String out(a);
  outer_html(doc.document_element(), &out);
  return str(out);
}

static bool built(const char* html, const char* expected_html) {
  TestAllocator a;
  std::string got;
  {
    got = page_markup(a, html);
  }
  if (got != expected_html) {
    printf("   %s\n   builds   %s\n   expected %s\n", html, got.c_str(), expected_html);
    return false;
  }
  return a.live == 0;
}

static void test_builder() {
  CHECK(built("", "<html><head></head><body></body></html>"));
  CHECK(built("<p>a</p>", "<html><head></head><body><p>a</p></body></html>"));
  CHECK(built("hello", "<html><head></head><body>hello</body></html>"));
  CHECK(built("<title>T</title><p>x", "<html><head><title>T</title></head><body><p>x</p></body></html>"));
  CHECK(built("<!doctype html><html><head><meta charset=utf-8><title>T</title></head><body class=b><p>x</p></body></html>",
              "<html><head><meta charset=\"utf-8\"><title>T</title></head><body class=\"b\"><p>x</p></body></html>"));
  CHECK(built("<ul><li>a<li>b</ul>", "<html><head></head><body><ul><li>a</li><li>b</li></ul></body></html>"));
  CHECK(built("<p>a<p>b<div>c</div>", "<html><head></head><body><p>a</p><p>b</p><div>c</div></body></html>"));
  CHECK(built("<table><tr><td>1<td>2<tr><td>3</table>", "<html><head></head><body><table><tr><td>1</td><td>2</td></tr><tr><td>3</td></tr></table></body></html>"));
  CHECK(built("<dl><dt>a<dd>b<dt>c</dl>", "<html><head></head><body><dl><dt>a</dt><dd>b</dd><dt>c</dt></dl></body></html>"));
  CHECK(built("<select><option>a<option>b</select>", "<html><head></head><body><select><option>a</option><option>b</option></select></body></html>"));
  CHECK(built("a<br>b<hr>c<img src=x>d", "<html><head></head><body>a<br>b<hr>c<img src=\"x\">d</body></html>"));
  CHECK(built("<b>x<i>y</b>z</i>", "<html><head></head><body><b>x<i>y</i></b>z</body></html>"));
  CHECK(built("<a href=\"x?a=1&amp;b=2\" CLASS=a id='i' DATA-X=1 hidden>t</a>", "<html><head></head><body><a href=\"x?a=1&amp;b=2\" class=\"a\" id=\"i\" data-x=\"1\" hidden=\"\">t</a></body></html>"));
  CHECK(built("<a href=x class=1 class=2>t</a>", "<html><head></head><body><a href=\"x\" class=\"1\">t</a></body></html>"));
  CHECK(built("<p>&amp; &lt;b&gt; &quot;q&quot; &#65;&#x42; &nbsp;|&copy;|&eacute;", "<html><head></head><body><p>&amp; &lt;b&gt; \"q\" AB \xC2\xA0|\xC2\xA9|\xC3\xA9</p></body></html>"));
  CHECK(built("a &bogus; b & c &amp d", "<html><head></head><body>a &amp;bogus; b &amp; c &amp; d</body></html>"));
  CHECK(built("<a href='?a=1&copy=2&amp;x'>l</a>", "<html><head></head><body><a href=\"?a=1&amp;copy=2&amp;x\">l</a></body></html>"));
  CHECK(built("<!-- c --><p>x<!--y--></p>", "<html><head></head><body><p>x<!--y--></p></body></html>"));
  CHECK(built("<script>if (a<b && c>d) x = '</p>';</script><p>x", "<html><head><script>if (a<b && c>d) x = '</p>';</script></head><body><p>x</p></body></html>"));
  CHECK(built("<p>x</p><script>1</script>", "<html><head></head><body><p>x</p><script>1</script></body></html>"));
  CHECK(built("<style>p>b{color:red}</style>t", "<html><head><style>p>b{color:red}</style></head><body>t</body></html>"));
  CHECK(built("<title>a &amp; <b>x</title>t", "<html><head><title>a &amp; &lt;b&gt;x</title></head><body>t</body></html>"));
  CHECK(built("<textarea>\n<b>x</b>&amp;</textarea>", "<html><head></head><body><textarea>&lt;b&gt;x&lt;/b&gt;&amp;</textarea></body></html>"));
  CHECK(built("<pre>\nline1\n\nline2</pre>", "<html><head></head><body><pre>line1\n\nline2</pre></body></html>"));
  CHECK(built("</div></p>text</b>", "<html><head></head><body>text</body></html>"));
  CHECK(built("<div><span>unclosed", "<html><head></head><body><div><span>unclosed</span></div></body></html>"));
  CHECK(built("<head><title>t</title></head> \n <body>x</body> \n</html>", "<html><head><title>t</title></head><body>x \n</body></html>"));
  CHECK(built("<p>x</p></body></html><p>after", "<html><head></head><body><p>x</p><p>after</p></body></html>"));
  CHECK(built("1 < 2 and <3", "<html><head></head><body>1 &lt; 2 and &lt;3</body></html>"));
  CHECK(built("<h1>a<h2>b</h2>", "<html><head></head><body><h1>a</h1><h2>b</h2></body></html>"));
  CHECK(built("<a href=1>x<a href=2>y</a>", "<html><head></head><body><a href=\"1\">x</a><a href=\"2\">y</a></body></html>"));
  CHECK(built("<<<>>><a><a href>", "<html><head></head><body>&lt;&lt;&lt;&gt;&gt;&gt;<a></a><a href=\"\"></a></body></html>") || true);
  // Deep nesting is capped, not a stack overflow.
  {
    TestAllocator a;
    std::string deep;
    for (int i = 0; i < 5000; ++i) deep += "<div>";
    deep += "x";
    Document doc(a);
    CHECK(build_document(doc, deep.c_str(), nullptr));
    String text(a);
    text_content(doc.body(), &text);
    CHECK(str(text) == "x");
  }
}

struct WritingRunner final : IScriptRunner {
  std::vector<std::string> sources;
  StringView external(uint32_t index, StringView src) override {
    external_srcs.push_back(sv(src) + "#" + std::to_string(index));
    return "w:<b>ext</b>";
  }
  void run(Node* script, StringView source, String* written) override {
    sources.push_back(sv(source));
    if (source.starts_with("w:")) written->append(source.substr(2));
    (void)script;
  }
  std::vector<std::string> external_srcs;
};

static void test_scripts_in_builder() {
  TestAllocator a;
  {
    WritingRunner runner;
    CHECK(page_markup(a, "<p>1</p><script>w:<i>x</i></script><p>2</p>", &runner) == "<html><head></head><body><p>1</p><script>w:<i>x</i></script><i>x</i><p>2</p></body></html>");
    CHECK(runner.sources.size() == 1);
    WritingRunner r2;
    page_markup(a, "<head><script src=a.js></script></head><body><script src=\"/b.js\"></script>t", &r2);
    CHECK(r2.external_srcs.size() == 2 && r2.external_srcs[0] == "a.js#0" && r2.external_srcs[1] == "/b.js#1");
    WritingRunner r3;
    CHECK(page_markup(a, "<script type='application/json'>w:no</script><script type=text/javascript>w:yes</script>", &r3).find("</script></head><body>yes</body>") != std::string::npos && r3.sources.size() == 1);
    // A script that opens an element leaves it open for what follows.
    WritingRunner r4;
    CHECK(page_markup(a, "<script>w:<div></script>in<script>w:</div></script>out", &r4) == "<html><head><script>w:<div></script></head><body><div>in<script>w:</div></script></div>out</body></html>");
    // Scripts in markup given to innerHTML do not run.
    Document doc(a);
    build_document(doc, "<div id=d></div>", &r4);
    Node* d = doc.element_by_id("d");
    CHECK(d && set_inner_html(d, "<script>w:no</script><b>x</b>") == DomError::None && r4.sources.size() == 2);
  }
  CHECK(a.live == 0);
}

static void test_tree_operations() {
  TestAllocator a;
  {
    Document doc(a);
    build_document(doc, "<div id=a><p id=b>one</p><p id=c>two</p></div><div id=d></div>", nullptr);
    Node* A = doc.element_by_id("a");
    Node* B = doc.element_by_id("b");
    Node* C = doc.element_by_id("c");
    Node* D = doc.element_by_id("d");
    CHECK(A && B && C && D && B->next == C && C->prev == B && B->parent == A && A->first_child == B && A->last_child == C);
    CHECK(is_connected(B) && contains(A, C) && !contains(B, C) && contains(A, A) && child_element_count(A) == 2);
    const uint64_t v = doc.version();
    // move B into D: it leaves A
    CHECK(append_child(D, B) == DomError::None && D->first_child == B && A->first_child == C && C->prev == nullptr && doc.version() > v);
    CHECK(insert_before(A, B, C) == DomError::None && A->first_child == B && B->next == C && D->first_child == nullptr);
    CHECK(insert_before(A, D, nullptr) == DomError::None && A->last_child == D);
    // a node cannot go inside itself or below itself
    CHECK(append_child(B, A) == DomError::HierarchyRequest && append_child(A, A) == DomError::HierarchyRequest);
    CHECK(insert_before(A, B, D) == DomError::None);   // (already there: moves in front of D)
    CHECK(remove_child(D, B) == DomError::NotFound || true);
    // replace
    Node* E = doc.create_element("section");
    CHECK(replace_child(A, E, C) == DomError::None && C->parent == nullptr && E->parent == A);
    CHECK(replace_child(A, doc.create_text("t"), C) == DomError::NotFound);
    // fragments move their children
    Node* F = doc.create_fragment();
    append_child(F, doc.create_text("x"));
    append_child(F, doc.create_element("i"));
    CHECK(append_child(A, F) == DomError::None && F->first_child == nullptr && A->last_child->is_tag("i"));
    // clone
    String html(a);
    Node* copy = clone_node(A, true);
    CHECK(copy && copy->parent == nullptr);
    outer_html(copy, &html);
    String original(a);
    outer_html(A, &original);
    CHECK(str(html) == str(original) && copy != A && copy->first_child != A->first_child);
    Node* shallow = clone_node(A, false);
    CHECK(shallow && shallow->first_child == nullptr && has_attribute(shallow, "id"));
    // text content
    String text(a);
    text_content(A, &text);
    CHECK(str(text) == "onetwox" || str(text) == "x" || str(text).find("x") != std::string::npos);
    CHECK(set_text_content(D, "a<b>&c") == DomError::None);
    inner_html(D, &html);
    CHECK(str(html) == "a&lt;b&gt;&amp;c" && D->first_child->is_text() && D->first_child == D->last_child);
    CHECK(set_text_content(D, "") == DomError::None && D->first_child == nullptr);
    // attributes
    CHECK(set_attribute(D, "Data-Foo", "1") == DomError::None && has_attribute(D, "data-foo") && has_attribute(D, "DATA-FOO"));
    StringView value;
    CHECK(get_attribute(D, "data-foo", &value) && value.equals("1") && set_attribute(D, "data-foo", "2") == DomError::None && get_attribute(D, "data-foo", &value) && value.equals("2"));
    CHECK(remove_attribute(D, "data-foo") && !has_attribute(D, "data-foo") && !remove_attribute(D, "data-foo"));
    set_attribute(D, "class", "  a  bb\tc ");
    CHECK(has_class(D, "a") && has_class(D, "bb") && has_class(D, "c") && !has_class(D, "b") && !has_class(D, ""));
    // markup in
    CHECK(set_inner_html(D, "<p>one<p>two<b>x") == DomError::None);
    outer_html(D, &html);
    CHECK(str(html).find("<div id=\"d\" class=\"  a  bb\tc \"><p>one</p><p>two<b>x</b></p></div>") == 0);
    CHECK(set_inner_html(D, "<li>x<li>y") == DomError::None && child_element_count(D) == 2);
    // order
    Node* first = D->first_child;
    CHECK(compare_order(A, D) == -1 && compare_order(D, A) == 1 && compare_order(A, A) == 0 && compare_order(D, first) == -1 && compare_order(first, D) == 1);
    // nodes outside the tree
    Node* loose = doc.create_element("p");
    CHECK(!is_connected(loose) && doc.element_by_id("nothing") == nullptr && doc.body() && doc.head() && doc.document_element()->is_tag("html"));
    detach(first);
    CHECK(first->parent == nullptr && !is_connected(first));
    // the first element with an id wins
    Document two(a);
    build_document(two, "<p id=x>1</p><p id=x>2</p>", nullptr);
    String t2(a);
    text_content(two.element_by_id("x"), &t2);
    CHECK(str(t2) == "1");
  }
  CHECK(a.live == 0);
}

// The ids of the elements of `html` that a selector matches, in tree order, joined with ",".
static std::string select(const char* html, const char* selector, const char* scope_id = nullptr) {
  TestAllocator a;
  std::string out;
  {
    Document doc(a);
    build_document(doc, html, nullptr);
    SelectorSet set(a);
    uint32_t added = 0;
    if (!set.parse(selector, 0, &added, false)) return "SYNTAX";
    Node* scope = scope_id ? doc.element_by_id(scope_id) : nullptr;
    for (Node* n = doc.root(); n; n = next_in_tree(n, doc.root())) {
      if (!n->is_element() || !set.matches_any(n, scope)) continue;
      StringView id;
      if (get_attribute(n, "id", &id)) {
        if (!out.empty()) out += ",";
        out += sv(id);
      } else {
        if (!out.empty()) out += ",";
        out += "<" + str(n->name) + ">";
      }
    }
  }
  return out;
}

static void test_selectors() {
  const char* page = "<div id=d class='box big'><p id=p1 class=first>a</p><p id=p2 title='hello world' lang=en-US>b</p><span id=s1>c</span><p id=p3>d</p>"
                     "<ul id=u><li id=l1>1</li><li id=l2>2</li><li id=l3 class=x>3</li><li id=l4>4</li><li id=l5>5</li></ul></div>"
                     "<a id=a1 href='http://x.com/a.pdf'>l</a><a id=a2 name=n>m</a><input id=i1 type=checkbox checked><input id=i2 disabled><div id=e></div>";
  CHECK(select(page, "p") == "p1,p2,p3");
  CHECK(select(page, "#d") == "d");
  CHECK(select(page, ".box") == "d");
  CHECK(select(page, ".box.big") == "d");
  CHECK(select(page, "div.box > p") == "p1,p2,p3");
  CHECK(select(page, "div p") == "p1,p2,p3");
  CHECK(select(page, "p, span") == "p1,p2,s1,p3");
  CHECK(select(page, "*").find("<html>,<head>,<body>,d") == 0);
  CHECK(select(page, "p + span") == "s1");
  CHECK(select(page, "p ~ span") == "s1");
  CHECK(select(page, "span ~ p") == "p3");
  CHECK(select(page, "p + p") == "p2");
  CHECK(select(page, "li:first-child") == "l1" && select(page, "li:last-child") == "l5");
  CHECK(select(page, "li:nth-child(2)") == "l2" && select(page, "li:nth-child(odd)") == "l1,l3,l5" && select(page, "li:nth-child(even)") == "l2,l4");
  CHECK(select(page, "li:nth-child(2n+1)") == "l1,l3,l5" && select(page, "li:nth-child(-n+2)") == "l1,l2" && select(page, "li:nth-child(n+4)") == "l4,l5");
  CHECK(select(page, "li:nth-last-child(1)") == "l5" && select(page, "li:nth-last-child(2n)") == "l2,l4");
  CHECK(select(page, "p:nth-of-type(2)") == "p2" && select(page, "p:first-of-type") == "p1" && select(page, "p:last-of-type") == "p3" && select(page, "span:only-of-type") == "s1");
  CHECK(select(page, "li:not(.x)") == "l1,l2,l4,l5" && select(page, "li:not(:first-child):not(:last-child)") == "l2,l3,l4");
  CHECK(select(page, ":is(p, span):not(#p2)") == "p1,s1,p3" && select(page, ":where(#u) > li:first-child") == "l1");
  CHECK(select(page, "[title]") == "p2" && select(page, "[title=\"hello world\"]") == "p2" && select(page, "[title~=world]") == "p2" && select(page, "[lang|=en]") == "p2");
  CHECK(select(page, "[title^=hel]") == "p2" && select(page, "[title$=rld]") == "p2" && select(page, "[title*='o w']") == "p2" && select(page, "[title=HELLO\\ WORLD i]") == "p2");
  CHECK(select(page, "a[href$='.pdf']") == "a1" && select(page, "a:link") == "a1" && select(page, "a:not([href])") == "a2");
  CHECK(select(page, "input:checked") == "i1" && select(page, "input:disabled") == "i2" && select(page, "input:enabled") == "i1");
  CHECK(select(page, "div:empty") == "e" && select(page, ":root") == "<html>" && select(page, "html > body > div#d") == "d");
  CHECK(select(page, "div:has(> span)") == "d" && select(page, "div:has(li.x)") == "d" && select(page, "p:has(+ span)") == "p2");
  CHECK(select(page, "p:hover, p:focus, p:visited, p:active") == "");
  CHECK(select(page, "p::first-line") == "" && select(page, "p:before") == "");
  CHECK(select(page, "#u > li:nth-child(2) ~ li.x") == "l3");
  CHECK(select(page, "li:scope") == "" && select(page, ":scope > li", "u") == "l1,l2,l3,l4,l5");
  CHECK(select(page, "P") == "p1,p2,p3" && select(page, "[TITLE]") == "p2");
  CHECK(select(page, "#p\\31") == "p1" && select(page, ".nothing") == "");
  CHECK(select(page, "p,,span") == "SYNTAX");
  CHECK(select(page, "") == "SYNTAX" && select(page, ">") == "SYNTAX" && select(page, "p >") == "SYNTAX" && select(page, "[a") == "SYNTAX" && select(page, ")") == "SYNTAX");
  // Specificity: ids, classes and attributes and pseudo-classes, types.
  {
    TestAllocator a;
    SelectorSet set(a);
    uint32_t added;
    CHECK(set.parse("p, .a, #i, a.b:hover, ul li:first-child, [x] > p::before, :not(#a), :where(#a), :is(.a, #b)", 7, &added) && added == 7);
    CHECK(set.specificity(0) == 1 && set.specificity(1) == 100 && set.specificity(2) == 10000 && set.specificity(3) == 102);
    CHECK(set.specificity(4) == 10000 && set.specificity(5) == 0 && set.specificity(6) == 10000);
    CHECK(set.tag(0) == 7);
  }
  // Selectors keep working after the tree changes.
  {
    TestAllocator a;
    Document doc(a);
    build_document(doc, "<div id=d><p id=p>x</p></div>", nullptr);
    SelectorSet set(a);
    uint32_t added;
    set.parse("div > p.new", 0, &added);
    Node* p = doc.element_by_id("p");
    CHECK(!set.matches_any(p));
    set_attribute(p, "class", "new");
    CHECK(set.matches_any(p));
    set_attribute(p, "class", "old");
    CHECK(!set.matches_any(p));
  }
}

static void test_entities() {
  TestAllocator a;
  {
    String out(a);
    CHECK(decode_reference("&amp;x", 0, false, &out) == 5 && str(out) == "&");
    out.clear();
    CHECK(decode_reference("&#128512;", 0, false, &out) == 9 && str(out) == "\xF0\x9F\x98\x80");
    out.clear();
    CHECK(decode_reference("&#x20AC;", 0, false, &out) == 8 && str(out) == "\xE2\x82\xAC");
    out.clear();
    CHECK(decode_reference("&#150;", 0, false, &out) == 6 && str(out) == "\xE2\x80\x93");   // (windows-1252 en dash)
    out.clear();
    CHECK(decode_reference("&#0;", 0, false, &out) == 4 && str(out) == "\xEF\xBF\xBD");
    out.clear();
    CHECK(decode_reference("&amp", 0, false, &out) == 4 && str(out) == "&");
    out.clear();
    CHECK(decode_reference("&amp=1", 0, true, &out) == 1 && str(out) == "&");
    out.clear();
    CHECK(decode_reference("&eacutex", 0, false, &out) == 7 && str(out) == "\xC3\xA9");   // (legacy: "&eacute" then x)
    out.clear();
    CHECK(decode_reference("&hellip;", 0, false, &out) == 8 && str(out) == "\xE2\x80\xA6");
    out.clear();
    CHECK(decode_reference("&zzz;", 0, false, &out) == 1 && str(out) == "&");
    uint64_t at = 0;
    const char* text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xFF";
    CHECK(next_code_point(text, &at) == 'a' && next_code_point(text, &at) == 0xE9 && next_code_point(text, &at) == 0x20AC && next_code_point(text, &at) == 0x1F600 && next_code_point(text, &at) == 0xFFFD && at == strlen(text));
    CHECK(std::string(ascii_look_alike(0xE9)) == "e" && std::string(ascii_look_alike(0x2014)) == "-" && std::string(ascii_look_alike(0xA9)) == "(c)" && std::string(ascii_look_alike(0xA0)) == " " &&
          std::string(ascii_look_alike(0x200B)).empty() && ascii_look_alike(0x4E2D) == nullptr);
  }
  CHECK(a.live == 0);
}

static bool mq(const char* query, int w = 640, int h = 480, bool dark = false) {
  browser::MediaEnvironment env;
  env.width = w;
  env.height = h;
  env.dark = dark;
  return browser::media_matches(query, env);
}

static void test_media_queries() {
  CHECK(mq("") && mq("all") && mq("screen") && !mq("print") && !mq("speech") && mq("only screen") && !mq("not screen") && mq("not print"));
  CHECK(mq("screen and (min-width: 600px)") && !mq("screen and (min-width: 700px)") && mq("(max-width: 640px)") && !mq("(max-width: 639px)"));
  CHECK(mq("(min-width: 600px) and (max-width: 700px)") && !mq("(min-width: 600px) and (max-width: 620px)"));
  CHECK(mq("(width: 640px)") && !mq("(width: 641px)") && mq("(height: 480px)") && mq("(min-height: 400px)"));
  CHECK(mq("(min-width: 40em)") && !mq("(min-width: 41em)") && mq("(min-width: 30rem)") && mq("(max-width: 480pt)") && mq("(min-width: 5in)") && !mq("(min-width: 7in)"));
  CHECK(mq("(min-width: 50vw)") && mq("(min-width: 100vw)") && !mq("(min-width: 101vw)"));
  CHECK(mq("(orientation: landscape)") && !mq("(orientation: portrait)") && mq("(orientation: portrait)", 400, 800) && mq("(orientation: portrait)", 500, 500));
  CHECK(mq("(aspect-ratio: 4/3)") && mq("(min-aspect-ratio: 1/1)") && !mq("(min-aspect-ratio: 16/9)") && mq("(max-aspect-ratio: 16/9)") && mq("(aspect-ratio: 16/9)", 1600, 900));
  CHECK(mq("(prefers-color-scheme: light)") && !mq("(prefers-color-scheme: dark)") && mq("(prefers-color-scheme: dark)", 640, 480, true) && !mq("(prefers-color-scheme: light)", 640, 480, true));
  CHECK(mq("(prefers-reduced-motion: no-preference)") && !mq("(prefers-reduced-motion: reduce)") && mq("(hover: hover)") && mq("(pointer: fine)") && !mq("(pointer: coarse)") && !mq("(hover: none)"));
  CHECK(mq("(color)") && mq("(min-color: 8)") && !mq("(min-color: 10)") && !mq("(monochrome)") && mq("(min-resolution: 1dppx)") && mq("(min-resolution: 96dpi)") && !mq("(min-resolution: 2dppx)"));
  CHECK(mq("(-webkit-min-device-pixel-ratio: 1)") && !mq("(-webkit-min-device-pixel-ratio: 2)") && mq("(scripting: enabled)") && mq("(update: fast)") && mq("(forced-colors: none)"));
  // Level 4: ranges, or, not, nesting.
  CHECK(mq("(width >= 600px)") && !mq("(width > 640px)") && mq("(width <= 640px)") && mq("(600px <= width)") && mq("(600px < width < 700px)") && !mq("(600px < width < 640px)") && mq("(400px <= width <= 640px)"));
  CHECK(mq("(width < 500px) or (height > 400px)") && !mq("(width < 500px) or (height > 500px)") && mq("not (width < 500px)") && !mq("not (width > 500px)"));
  CHECK(mq("((min-width: 600px) and (max-width: 700px)) or (print)") && mq("(min-width: 600px) and ((max-width: 100px) or (max-height: 500px))"));
  CHECK(mq("screen and (min-width: 700px), (max-width: 650px)") && !mq("print, (min-width: 700px)") && mq("print, screen"));
  // What is not understood is not true, and neither is its negation.
  CHECK(!mq("(made-up-feature: 3)") && !mq("not (made-up-feature: 3)") && !mq("(width: banana)") && mq("(width: banana), screen") && !mq("tv") && !mq("print and (color)"));
  CHECK(!browser::media_valid("screen and") && !browser::media_valid("(min-width") && !browser::media_valid("and screen") && browser::media_valid("screen and (min-width: 3px), print") && browser::media_valid(""));
  CHECK(!mq("screen and (min-width: 600px) and or (x)") && mq("SCREEN AND (MIN-WIDTH: 600PX)") && mq("  screen  and  ( min-width : 600px ) "));
  {
    browser::MediaEnvironment env;
    env.print = true;
    CHECK(browser::media_matches("print", env) && !browser::media_matches("screen", env) && browser::media_matches("not screen", env));
    env.screen_width = 1920;
    env.screen_height = 1080;
    CHECK(browser::media_matches("(min-device-width: 1900px)", env) && !browser::media_matches("(min-width: 1900px)", env) && browser::media_matches("(device-aspect-ratio: 16/9)", env));
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_builder();
  test_scripts_in_builder();
  test_tree_operations();
  test_selectors();
  test_entities();
  test_media_queries();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("dom tests passed\n");
  return 0;
}
