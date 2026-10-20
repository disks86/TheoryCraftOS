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

// Host-side tests for the browser's model: reading HTML into runs (text, white space, entities, headings, lists, links, tables, preformatted text, things
// that are skipped, damaged markup), fitting the runs into lines, and the address helpers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Browser/HtmlDocument.hpp"
#include "Browser/PageLayout.hpp"
#include "Browser/Url.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// The page as plain text, the way the layout shows it in `columns` columns: lines joined with '\n', pieces placed at their columns.
static void render(TestAllocator& a, const HtmlDocument& doc, uint32_t columns, String* out) {
  PageLayout layout(a);
  layout.build(doc, columns);
  out->clear();
  for (uint32_t line = 0; line < layout.line_count(); ++line) {
    if (line > 0) out->push_back('\n');
    uint32_t at = 0;
    for (uint32_t p = layout.first_piece(line); p < layout.end_piece(line); ++p) {
      const Piece& piece = layout.piece(p);
      while (at < piece.column) { out->push_back(' '); ++at; }
      const Run& run = doc.run(piece.run);
      if (run.flags & kRunRule) { out->append("---"); at += 3; continue; }
      out->append(doc.run_text(piece.run).substr(piece.from, piece.length));
      at += piece.length;
    }
  }
}

static bool page_is(const char* html, uint32_t columns, const char* expected) {
  TestAllocator a;
  bool ok;
  {
    HtmlDocument doc(a);
    doc.parse_html(html);
    String out(a);
    render(a, doc, columns, &out);
    ok = out.view().equals(expected);
    if (!ok) printf("   page \"%s\" shows\n--\n%s\n--\nexpected\n--\n%s\n--\n", html, out.c_str(), expected);
  }
  return ok && a.live == 0;
}

static void test_text() {
  CHECK(page_is("hello", 40, "hello"));
  CHECK(page_is("  hello \n\t world  ", 40, "hello world"));
  CHECK(page_is("<p>one</p><p>two</p>", 40, "one\n\ntwo"));
  CHECK(page_is("one<br>two", 40, "one\ntwo"));
  CHECK(page_is("one<br><br>two", 40, "one\n\ntwo"));
  CHECK(page_is("<div>one</div><div>two</div>", 40, "one\ntwo"));
  CHECK(page_is("foo <b>bar</b> baz", 40, "foo bar baz"));
  CHECK(page_is("foo<b> bar</b>baz", 40, "foo barbaz"));
  CHECK(page_is("a &amp; b &lt;c&gt; &quot;d&quot; &#65;&#x42; &nbsp;x", 40, "a & b <c> \"d\" AB x"));
  CHECK(page_is("1 &bogus; 2 & 3", 40, "1 ? 2 & 3"));
  CHECK(page_is("caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D", 40, "caf? \"q\""));
  CHECK(page_is("a<!-- hidden -->b", 40, "ab"));
  CHECK(page_is("<!DOCTYPE html><html><head><title>T</title><style>p{}</style><script>var x = '<p>no</p>';</script></head><body>shown</body></html>", 40, "shown"));
  CHECK(page_is("1 < 2 and 3 > 2", 40, "1 < 2 and 3 > 2"));
  CHECK(page_is("<p>unclosed <b>bold <i>x", 40, "unclosed bold x"));
  CHECK(page_is("<img src=a.png alt=\"A logo\"> text", 40, "[A logo] text"));
  CHECK(page_is("", 40, ""));
  CHECK(page_is("<p></p>", 40, ""));
}

static void test_blocks() {
  CHECK(page_is("<h1>Title</h1>text", 40, "Title\n\ntext"));
  CHECK(page_is("<ul><li>one</li><li>two</li></ul>", 40, "* one\n* two"));
  CHECK(page_is("<ol><li>one</li><li>two</li></ol>", 40, "1. one\n2. two"));
  CHECK(page_is("<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>", 40, "* a\n   * b\n* c"));
  CHECK(page_is("<ul><li>one two three four five</li></ul>", 14, "* one two\n   three four\n   five"));
  CHECK(page_is("a<hr>b", 40, "a\n\n---\n\nb"));
  CHECK(page_is("a<blockquote>q</blockquote>b", 40, "a\n\n    q\n\nb"));
  CHECK(page_is("<pre>\n  x = 1;\n\n  y = 2;\n</pre>after", 40, "  x = 1;\n\n  y = 2;\n\nafter"));
  CHECK(page_is("<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>", 40, "A | B\n1 | 2"));
  CHECK(page_is("<p>x</p><script>1</script><noscript>no</noscript><p>y</p>", 40, "x\n\ny"));
}

static void test_wrapping() {
  CHECK(page_is("aaa bbb ccc ddd", 8, "aaa bbb\nccc ddd"));
  CHECK(page_is("aaa bbb ccc ddd", 11, "aaa bbb ccc\nddd"));
  CHECK(page_is("abcdefghijkl", 8, "abcdefgh\nijkl"));
  CHECK(page_is("ab abcdefghijkl", 8, "ab\nabcdefgh\nijkl"));
  CHECK(page_is("<pre>0123456789abc</pre>", 8, "01234567\n89abc"));
  CHECK(page_is("x <b>bold text</b> y", 9, "x bold\ntext y"));
}

static void test_links() {
  TestAllocator a;
  {
    HtmlDocument doc(a);
    doc.parse_html("see <a href=\"/next?a=1&amp;b=2\">the <b>next</b> page</a>, or <a href='x.html'>x</a> <a name=top>no</a>.");
    CHECK(doc.link_count() == 2 && doc.link(0).equals("/next?a=1&b=2") && doc.link(1).equals("x.html"));
    PageLayout layout(a);
    layout.build(doc, 60);
    CHECK(layout.line_count() == 1);
    // "see the next page, or x no."
    CHECK(layout.link_at(doc, 0, 0) == -1 && layout.link_at(doc, 0, 4) == 0 && layout.link_at(doc, 0, 16) == 0 && layout.link_at(doc, 0, 17) == -1);
    CHECK(layout.link_at(doc, 0, 22) == 1 && layout.link_at(doc, 0, 24) == -1 && layout.link_at(doc, 5, 0) == -1);
  }
  CHECK(a.live == 0);
}

static void test_title_and_limits() {
  TestAllocator a;
  {
    HtmlDocument doc(a);
    doc.parse_html("<html><head><title> My  &amp; page </title></head><body>x</body></html>");
    CHECK(doc.title().equals("My & page") && doc.run_count() == 1);
    CHECK(doc.parse_html("<title>one</title>") && doc.title().equals("one") && doc.run_count() == 0);
    CHECK(doc.parse_text("a\tb\r\nc\n") && doc.run_count() == 1 && doc.run_text(0).equals("a    b\nc") && (doc.run(0).flags & kRunPre));
    CHECK(doc.parse_text("") && doc.run_count() == 0);
    String out(a);
    doc.parse_text("l1\n\nl3");
    render(a, doc, 20, &out);
    CHECK(out.view().equals("l1\n\nl3"));
    // A page with a huge number of tags is cut off, not run out of memory.
    String big(a);
    for (int i = 0; i < 300000; ++i) big.append("<b>x</b> ");
    CHECK(doc.parse_html(big.view()) && doc.truncated() && doc.run_count() <= HtmlDocument::kMaxRuns);
    // Markup that is cut short or damaged.
    CHECK(doc.parse_html("<a href=\"unclosed") && doc.run_count() == 0);
    CHECK(doc.parse_html("text <b") && doc.run_count() == 1);
    CHECK(doc.parse_html("<script>never ends") && doc.run_count() == 0);
    CHECK(doc.parse_html("&#99999999999; &#x; &; &amp") && doc.run_count() == 1);
    CHECK(doc.parse_html("<<<>>><a><a href>") );
  }
  CHECK(a.live == 0);
}

static String resolved(TestAllocator& a, const char* base, const char* ref) {
  String out(a);
  if (!resolve_reference(a, base, ref, &out)) out.assign("(none)");
  return out;
}

static void test_addresses() {
  TestAllocator a;
  {
    String out(a);
    CHECK(normalize_address("  example.com/a  ", &out) && out.view().equals("http://example.com/a"));
    CHECK(normalize_address("https://x.org", &out) && out.view().equals("https://x.org"));
    CHECK(normalize_address("file:///docs/a.html", &out) && out.view().equals("/docs/a.html"));
    CHECK(normalize_address("/docs/a.html", &out) && out.view().equals("/docs/a.html"));
    CHECK(normalize_address("mailto:x@y", &out) && out.view().equals("mailto:x@y"));
    CHECK(!normalize_address("   ", &out));
    CHECK(is_web_address("HTTP://a") && is_web_address("https://a") && !is_web_address("ftp://a") && !is_web_address("/a"));
    CHECK(is_file_address("/a") && is_file_address("file:///a") && !is_file_address("http://a"));

    CHECK(resolved(a, "http://example.com/a/b.html", "c.html").view().equals("http://example.com/a/c.html"));
    CHECK(resolved(a, "http://example.com/a/b.html", "/c.html").view().equals("http://example.com/c.html"));
    CHECK(resolved(a, "http://example.com/a/b.html", "../c.html#top").view().equals("http://example.com/c.html"));
    CHECK(resolved(a, "http://example.com/a/b.html", "//other.org/x").view().equals("http://other.org/x"));
    CHECK(resolved(a, "http://example.com/a/b.html", "https://z.org/y#f").view().equals("https://z.org/y"));
    CHECK(resolved(a, "http://example.com/a/b.html", "#part").view().equals("http://example.com/a/b.html"));
    CHECK(resolved(a, "http://example.com/a/b.html", "mailto:x@y").view().equals("(none)"));
    CHECK(resolved(a, "http://example.com/a/b.html", "javascript:void(0)").view().equals("(none)"));
    CHECK(resolved(a, "http://example.com/a/b.html", "  ").view().equals("(none)"));
    CHECK(resolved(a, "/docs/a.html", "b.html").view().equals("/docs/b.html"));
    CHECK(resolved(a, "/docs/a.html", "sub/../c.html?x=1#y").view().equals("/docs/c.html"));
    CHECK(resolved(a, "/docs/a.html", "../../c.html").view().equals("/c.html"));
    CHECK(resolved(a, "/docs/a.html", "/top.html").view().equals("/top.html"));
    CHECK(resolved(a, "/docs/a.html", "file:///x/y.html").view().equals("/x/y.html"));
    CHECK(resolved(a, "/docs/a.html", "http://e.org/").view().equals("http://e.org/"));

    CHECK(address_name("/docs/a.html").equals("a.html") && address_name("/docs/").equals("docs") && address_name("/").equals("/") && address_name("").empty());
    CHECK(address_name("http://example.com/").equals("example.com") && address_name("http://example.com").equals("example.com"));
    CHECK(address_name("http://example.com/dir/page.html#x").equals("page.html"));
  }
  CHECK(a.live == 0);
}

int main() {
  test_text();
  test_blocks();
  test_wrapping();
  test_links();
  test_title_and_limits();
  test_addresses();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("browser tests passed\n");
  return 0;
}
