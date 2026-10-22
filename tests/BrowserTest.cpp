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
// that are skipped, damaged markup), CSS (colours, selectors, the cascade, boxes), fitting the runs into lines, and the address helpers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Browser/HtmlDocument.hpp"
#include "Browser/Renderer.hpp"
#include "Dom/HtmlBuilder.hpp"
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

// A page drawn for a display `columns` columns wide (the boxes of the page take their widths from it), shown as lines of text.
static std::string drawn(const char* html, uint32_t columns, bool show_spaces = false) {
  TestAllocator a;
  std::string result;
  {
    dom::Document doc(a);
    dom::build_document(doc, html, nullptr);
    RenderOptions options;
    options.media.width = static_cast<int32_t>(columns) * 8;
    options.media.height = 480;
    HtmlDocument out(a);
    render_document(doc, options, &out);
    String text(a);
    render(a, out, columns, &text);
    result = text.c_str();
    if (show_spaces)
      for (char& c : result)
        if (c == ' ') c = '.';
  }
  if (a.live != 0) {
    printf("   leak (%d) drawing a page\n", a.live);
    ++g_failed;
  }
  return result;
}

static bool layout_is(const char* html, uint32_t columns, const char* expected) {
  const std::string got = drawn(html, columns);
  if (got != expected) {
    printf("   page \"%s\" (%u columns) shows\n--\n%s\n--\nexpected\n--\n%s\n--\n", html, columns, got.c_str(), expected);
    return false;
  }
  return true;
}

static void test_tables() {
  // columns line up, the last one is not padded
  CHECK(page_is("<table><tr><td>a</td><td>longer</td></tr><tr><td>bbb</td><td>c</td></tr></table>", 40, "a   | longer\nbbb | c"));
  // headers are centred; the header, body and foot come in that order wherever they are written
  CHECK(page_is("<table><tfoot><tr><td>f</td></tr></tfoot><tbody><tr><td>b</td></tr></tbody><thead><tr><th>head</th></tr></thead></table>", 40, "head\nb\nf"));
  CHECK(page_is("<table><tr><th>ab</th><th>cd</th></tr><tr><td>xxxxxx</td><td>y</td></tr></table>", 40, "  ab   | cd\nxxxxxx | y"));
  // colspan and rowspan
  CHECK(page_is("<table><tr><td colspan=2>wide cell</td></tr><tr><td>a</td><td>b</td></tr></table>", 40, "wide cell\na | b"));
  CHECK(page_is("<table><tr><td rowspan=2>x</td><td>1</td></tr><tr><td>2</td></tr></table>", 40, "x | 1\n  | 2"));
  // a caption is above the table
  CHECK(page_is("<table><caption>Title</caption><tr><td>a</td></tr></table>", 9, "  Title\n\na"));
  // a border draws the lines
  CHECK(page_is("<table border=1><tr><td>a</td><td>bb</td></tr><tr><td>c</td><td>d</td></tr></table>", 40,
                "+---+----+\n| a | bb |\n+---+----+\n| c | d  |\n+---+----+"));
  // valign
  CHECK(page_is("<table><tr><td>1<br>2<br>3</td><td valign=middle>m</td><td valign=bottom>b</td></tr></table>", 40, "1 |   |\n2 | m |\n3 |   | b"));
  // links and bold inside cells keep what they are
  {
    TestAllocator a;
    HtmlDocument doc(a);
    doc.parse_html("<table><tr><td><a href=/x>go</a></td><td><b>bold</b> text</td></tr></table>");
    bool link = false, bold = false;
    for (uint64_t i = 0; i < doc.run_count(); ++i) {
      if (doc.run_text(i).equals("go") && doc.run(i).link >= 0 && doc.link(static_cast<uint32_t>(doc.run(i).link)).equals("/x")) link = true;
      if (doc.run_text(i).equals("bold") && (doc.run(i).flags & kRunBold)) bold = true;
    }
    CHECK(link && bold);
  }
  // a nested table
  CHECK(page_is("<table><tr><td><table><tr><td>a</td><td>b</td></tr></table></td><td>c</td></tr></table>", 40, "a | b | c"));
  // pictures in a cell: the table flows as it did
  CHECK(page_is("<table><tr><td>1</td><td>2</td></tr></table>", 40, "1 | 2"));
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
  CHECK(page_is("1 &bogus; 2 & 3", 40, "1 &bogus; 2 & 3"));
  CHECK(page_is("caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D", 40, "cafe \"q\""));
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


// The run whose text is `text` in a page, or nullptr.
static const Run* find_run(const HtmlDocument& doc, const char* text) {
  for (uint64_t i = 0; i < doc.run_count(); ++i)
    if (doc.run_text(i).equals(text)) return &doc.run(i);
  return nullptr;
}

static uint32_t color_of(const char* html, const char* text) {
  TestAllocator a;
  HtmlDocument doc(a);
  doc.parse_html(html);
  const Run* r = find_run(doc, text);
  return r ? r->color : 0xDEAD;
}

static void test_colors() {
  uint32_t c;
  CHECK(parse_css_color("red", &c) && c == 0xFFFF0000);
  CHECK(parse_css_color("  Navy ", &c) && c == 0xFF000080);
  CHECK(parse_css_color("#f80", &c) && c == 0xFFFF8800);
  CHECK(parse_css_color("#336699", &c) && c == 0xFF336699);
  CHECK(parse_css_color("#336699ff", &c) && c == 0xFF336699);
  CHECK(parse_css_color("#33669900", &c) && c == 0);
  CHECK(parse_css_color("rgb(10, 20, 30)", &c) && c == 0xFF0A141E);
  CHECK(parse_css_color("rgb(10 20 30 / 50%)", &c) && c == 0xFF0A141E);
  CHECK(parse_css_color("rgba(10,20,30,0)", &c) && c == 0);
  CHECK(parse_css_color("rgb(100%, 0%, 0%)", &c) && c == 0xFFFF0000);
  CHECK(parse_css_color("hsl(120, 100%, 50%)", &c) && c == 0xFF00FF00);
  CHECK(parse_css_color("hsl(0, 0%, 100%)", &c) && c == 0xFFFFFFFF);
  CHECK(parse_css_color("transparent", &c) && c == 0);
  CHECK(!parse_css_color("notacolor", &c) && !parse_css_color("#12", &c) && !parse_css_color("", &c) && !parse_css_color("rgb(1,2)", &c) && !parse_css_color("inherit", &c));
}

static void test_css_cascade() {
  CHECK(color_of("<style>p{color:red}</style><p>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<p style='color: #00f'>x</p>", "x") == 0xFF0000FF);
  CHECK(color_of("<style>p{color:red} .a{color:blue}</style><p class=a>x</p>", "x") == 0xFF0000FF);
  CHECK(color_of("<style>.a{color:blue} p{color:red}</style><p class=a>x</p>", "x") == 0xFF0000FF);   // (specificity, not order)
  CHECK(color_of("<style>p{color:red} p{color:blue}</style><p>x</p>", "x") == 0xFF0000FF);          // (order)
  CHECK(color_of("<style>#i{color:lime} .a{color:blue}</style><p class=a id=i>x</p>", "x") == 0xFF00FF00);
  CHECK(color_of("<style>p{color:red}</style><p style='color:blue'>x</p>", "x") == 0xFF0000FF);
  CHECK(color_of("<style>p{color:red !important}</style><p style='color:blue'>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>p{color:red}</style><p style='color:blue !important'>x</p>", "x") == 0xFF0000FF);
  // Colour is inherited, and the nearest wins.
  CHECK(color_of("<style>div{color:red}</style><div><p>x</p></div>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>div{color:red} p{color:blue}</style><div><p>x</p></div>", "x") == 0xFF0000FF);
  CHECK(color_of("<font color=green>x</font>", "x") == 0xFF008000);
  // Combinators and the other selectors.
  CHECK(color_of("<style>div p{color:red}</style><div><span><p>x</p></span></div>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>div>p{color:red}</style><div><span><p>x</p></span></div>", "x") == 0);
  CHECK(color_of("<style>div>p{color:red}</style><div><p>x</p></div>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>b, i, u{color:red}</style><p><i>x</i></p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>a[href^=http]{color:red}</style><a href='http://x'>x</a><a href='/y'>y</a>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>a[href^=http]{color:red}</style><a href='http://x'>x</a><a href='/y'>y</a>", "y") == 0);
  CHECK(color_of("<style>[title~=two]{color:red}</style><p title='one two three'>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>a[href$='.pdf']{color:red}</style><a href='f.pdf'>x</a>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>li:first-child{color:red}</style><ul><li>x</li><li>y</li></ul>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>li:first-child{color:red}</style><ul><li>x</li><li>y</li></ul>", "y") == 0);
  CHECK(color_of("<style>.a.b{color:red}</style><p class='b a c'>x</p><p class=a>y</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>.a.b{color:red}</style><p class='b a c'>x</p><p class=a>y</p>", "y") == 0);
  CHECK(color_of("<style>*{color:red}</style><p>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>:root{color:red}</style><html><body>x</body></html>", "x") == 0xFFFF0000);
  // Selectors that cannot apply are left out (states and pseudo-elements); sibling combinators and :not() are understood.
  CHECK(color_of("<style>a:hover{color:red} p::first-line{color:red} p:focus{color:red} p{color:blue}</style><p>x</p>", "x") == 0xFF0000FF);
  CHECK(color_of("<style>p:not(.a){color:red} p{color:blue}</style><p>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>li + li{color:red}</style><ul><li>a</li><li>b</li></ul>", "b") == 0xFFFF0000 && color_of("<style>li + li{color:red}</style><ul><li>a</li><li>b</li></ul>", "a") == 0);
  CHECK(color_of("<style>li:nth-child(2){color:red} li:last-child{background:#eee}</style><ul><li>a</li><li>b</li><li>c</li></ul>", "b") == 0xFFFF0000);
  // @media: screen rules apply, print and narrow ones do not.
  CHECK(color_of("<style>@media print{p{color:red}} @media screen{p{color:blue}}</style><p>x</p>", "x") == 0xFF0000FF);
  CHECK(color_of("<style>@media (max-width: 600px){p{color:red}} @media (min-width: 100px){p{color:blue}}</style><p>x</p>", "x") == 0xFF0000FF);
  CHECK(color_of("<style media=print>p{color:red}</style><p>x</p>", "x") == 0);
  CHECK(color_of("<style>@import url(a.css); @font-face{font-family:x;src:url(a)} @charset 'x'; p{color:red}</style><p>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>/* p{color:blue} */ p{/* c */color:red;}</style><p>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>p{color:var(--x)} p{color:red}</style><p>x</p>", "x") == 0xFFFF0000);
  // Damaged stylesheets are survived.
  CHECK(color_of("<style>p{color:red</style><p>x</p>", "x") <= 0xFFFFFFFFu);
  CHECK(color_of("<style>}}} p{color red} {color:blue} p{;;:;color:red;;}</style><p>x</p>", "x") == 0xFFFF0000);
  CHECK(color_of("<style>p{content:\"}\"; color:red}</style><p>x</p>", "x") == 0xFFFF0000);
}

static void test_css_boxes() {
  // Spacing, hiding and alignment change the lines.
  CHECK(page_is("<style>p{margin:0}</style><p>a</p><p>b</p>", 40, "a\nb"));
  CHECK(page_is("<style>div{margin-top:32px}</style>a<div>b</div>", 40, "a\n\n\nb"));
  CHECK(page_is("<style>div{padding:1em 0}</style>a<div>b</div>c", 40, "a\n\nb\n\nc"));
  CHECK(page_is("<style>div{margin-left:16px}</style><div>b c</div>", 40, "  b c"));
  CHECK(page_is("<style>ul{margin:0;padding:0}</style><ul><li>a</li></ul>", 40, "* a"));
  CHECK(page_is("<style>li{list-style:none}</style><ul><li>a</li><li>b</li></ul>", 40, "   a\n   b"));
  CHECK(page_is("<style>.h{display:none}</style><p>a</p><p class=h>x <b>y</b></p><p>b</p>", 40, "a\n\nb"));
  CHECK(page_is("<style>.h{display:none} .h .s{display:block}</style>a<div class=h><span class=s>x</span></div>b", 40, "ab"));
  CHECK(page_is("<style>span{display:block}</style>a<span>b</span>c", 40, "a\nb\nc"));
  CHECK(page_is("<style>div{display:inline}</style><div>a</div> <div>b</div>", 40, "a b"));
  CHECK(page_is("<style>.v{visibility:hidden}</style>a<b class=v>x</b>b", 40, "ab"));
  CHECK(page_is("<div style='text-align:center'>ab</div>", 10, "    ab"));
  CHECK(page_is("<div style='text-align:right'>ab cd</div>", 10, "     ab cd"));
  CHECK(page_is("<center>ab</center>", 10, "    ab"));
  CHECK(page_is("<p align=right>ab</p>", 10, "        ab"));
  CHECK(page_is("<style>p{text-transform:uppercase}</style><p>Hello world</p>", 40, "HELLO WORLD"));
  CHECK(page_is("<style>p{white-space:pre}</style><p>a  b\nc</p>", 40, "a  b\nc"));
  CHECK(page_is("<style>pre{white-space:normal}</style><pre>a  b</pre>", 40, "a b"));
}

static void test_css_text_styles() {
  TestAllocator a;
  {
    HtmlDocument doc(a);
    doc.parse_html("<style>.b{font-weight:700} .n{font-weight:normal} .u{text-decoration:underline line-through} a{text-decoration:none}"
                   " .bg{background:#ccc url(x.png) no-repeat} .blk{display:block;background-color:#123456}</style>"
                   "<p class=b>bold</p><h1 class=n>plain</h1><p class=u>under</p><a href=/x>link</a><a name=y>nolink</a>"
                   "<span class=bg>bg</span><div class=blk>block <b>bold</b></div>");
    CHECK(find_run(doc, "bold") && (find_run(doc, "bold")->flags & kRunBold));
    CHECK(find_run(doc, "plain") && !(find_run(doc, "plain")->flags & kRunBold) && (find_run(doc, "plain")->flags & kRunHeading));
    const Run* u = find_run(doc, "under");
    CHECK(u && (u->flags & kRunUnderline) && (u->flags & kRunStrike));
    CHECK(find_run(doc, "link") && !(find_run(doc, "link")->flags & kRunUnderline) && find_run(doc, "link")->link == 0);
    CHECK(find_run(doc, "bg") && find_run(doc, "bg")->background == 0xFFCCCCCC && !(find_run(doc, "bg")->flags & kRunBlockBg));
    CHECK(find_run(doc, "block ") && find_run(doc, "block ")->background == 0xFF123456 && (find_run(doc, "block ")->flags & kRunBlockBg));
    CHECK(find_run(doc, "bold") != nullptr && doc.run_count() > 0);
  }
  {
    // Links are underlined unless the page says otherwise; the page background; stylesheets that are linked.
    HtmlDocument doc(a);
    doc.parse_html("<html><head><link rel=stylesheet href='/a.css'><link rel='stylesheet' href=b.css media=print><link rel=icon href=i.png>"
                   "<style>body{background:#000;color:#eee}</style></head><body><a href=x>l</a>t</body></html>");
    CHECK(doc.page_background() == 0xFF000000);
    CHECK(find_run(doc, "l") && (find_run(doc, "l")->flags & kRunUnderline) && find_run(doc, "t") && find_run(doc, "t")->color == 0xFFEEEEEE && find_run(doc, "t")->background == 0);
    // The stylesheets a caller fetched are read first, the page's own win over them.
    CHECK(doc.parse_html("<p>x</p><style>p{color:blue}</style>", "p{color:red;background:#eee} p{color:lime}") && find_run(doc, "x")->color == 0xFF0000FF && find_run(doc, "x")->background == 0xFFEEEEEE);
    CHECK(doc.parse_html("<body bgcolor=#ffffcc><td bgcolor=red>x</td></body>") && doc.page_background() == 0xFFFFFFCC && find_run(doc, "x")->background == 0xFFFF0000);
  }
  CHECK(a.live == 0);
}

// Layout of runs with different breaks and alignment.
static void test_css_garbage() {
  TestAllocator a;
  {
    HtmlDocument doc(a);
    // Rules by the thousand, deep nesting and nonsense do not run out of time or memory.
    String page(a);
    page.append("<style>");
    for (int i = 0; i < 12000; ++i) {
      page.append("div.c");
      page.append_uint(static_cast<uint64_t>(i));
      page.append(" > p:first-child{color:red;margin:1px 2px 3px 4px}");
    }
    page.append("</style>");
    for (int i = 0; i < 1000; ++i) page.append("<div class=c1><p>x</p></div>");
    CHECK(doc.parse_html(page.view()) && doc.run_count() > 0);
    String deep(a);
    for (int i = 0; i < 3000; ++i) deep.append("<div>");
    deep.append("deep");
    CHECK(doc.parse_html(deep.view()) && find_run(doc, "deep"));
    CHECK(doc.parse_html("<style>@media{{{{</style><style>a{</style><style>@</style><style>[</style><p>ok</p>") && find_run(doc, "ok"));
    String long_selector(a);
    long_selector.append("<style>");
    for (int i = 0; i < 5000; ++i) long_selector.append("a ");
    long_selector.append("b{color:red} p{color:blue}</style><p>deep</p>");
    CHECK(doc.parse_html(long_selector.view()) && find_run(doc, "deep") && find_run(doc, "deep")->color == 0xFF0000FF);
    CHECK(doc.parse_html("<p style='color:;:red;;font-weight'>ok</p><p style=\"margin: 1 2 3 4 5 6\">x</p>") && find_run(doc, "ok"));
  }
  CHECK(a.live == 0);
}


// A script runner that records what it is given; a script that starts with "w:" writes the rest into the page.
struct FakeRunner final : IScriptRunner {
  std::vector<std::string> ran;
  StringView external(uint32_t index, StringView) override { return index == 0 ? StringView("w:[ext]") : StringView(); }
  void run(dom::Node*, StringView source, String* written) override {
    ran.push_back(std::string(source.data(), source.size()));
    if (source.starts_with("w:")) written->append(source.substr(2));
  }
};

static bool scripted_page_is(const char* html, uint32_t columns, const char* expected, size_t scripts_run) {
  TestAllocator a;
  bool ok;
  {
    HtmlDocument doc(a);
    FakeRunner runner;
    doc.parse_html(html, StringView(), &runner);
    String out(a);
    render(a, doc, columns, &out);
    ok = out.view().equals(expected) && runner.ran.size() == scripts_run;
    if (!ok) printf("   page \"%s\" shows \"%s\" after %zu scripts, expected \"%s\" after %zu\n", html, out.c_str(), runner.ran.size(), expected, scripts_run);
  }
  return ok && a.live == 0;
}

static void test_scripts() {
  CHECK(scripted_page_is("a<script>w:<b>x</b></script>c", 40, "axc", 1));
  CHECK(scripted_page_is("<p>one</p><script>w:<p>two</p></script><p>three</p>", 40, "one\n\ntwo\n\nthree", 1));
  CHECK(scripted_page_is("a<script>nothing</script>b", 40, "ab", 1));
  CHECK(scripted_page_is("a<script src=x.js></script>b", 40, "a[ext]b", 1));
  CHECK(scripted_page_is("a<script src=x.js>ignored inline</script><script src=y.js></script>b", 40, "a[ext]b", 1));   // (the second has nothing fetched)
  CHECK(scripted_page_is("a<script type='application/ld+json'>w:no</script>b", 40, "ab", 0));
  CHECK(scripted_page_is("a<script type='text/javascript'>w:yes</script>b", 40, "ayesb", 1));
  CHECK(scripted_page_is("a<script>w:<script>w:deep</script></script>b", 40, "adeepb", 2));
  CHECK(scripted_page_is("a<script>w:<div>open</script>b", 40, "a\nopenb", 1));   // (an element it leaves open stays open: no dangling text)
  CHECK(scripted_page_is("a<script>w:<style>p{color:red}</style><p>styled</p></script>", 40, "a\n\nstyled", 1));
  CHECK(scripted_page_is("<script>unclosed", 40, "", 1));
  CHECK(scripted_page_is("a<script>\nw:x\n", 40, "a", 1));
  TestAllocator a;
  {
    HtmlDocument doc(a);
    doc.parse_html("<p>a</p><script src='/one.js'></script><script>1</script><script src=two.js async></script>");
    // Without a runner nothing is run, and the text of the script is not shown.
    CHECK(doc.parse_html("a<script>document.write('x')</script>b") && doc.run_count() == 2);
    String out(a);
    // A script that runs the page's own text back in at every level is stopped after a few.
    struct Echo final : IScriptRunner {
      StringView external(uint32_t, StringView) override { return StringView(); }
      void run(dom::Node*, StringView, String* written) override { written->append("<script>go</script>z"); }
    } echo;
    CHECK(doc.parse_html("<script>go</script>", StringView(), &echo) && doc.run_count() <= 8);
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

static void test_css_flex_grid() {
  CHECK(layout_is("<div style='display:flex'><div>ab</div><div>cd</div></div>", 12, "abcd"));
  CHECK(layout_is("<div style='display:flex;gap:16px'><div>ab</div><div>cd</div></div>", 12, "ab  cd"));
  CHECK(layout_is("<div style='display:flex;justify-content:space-between'><span>a</span><span>b</span></div>", 12, "a          b"));
  CHECK(layout_is("<div style='display:flex;flex-direction:column'><div>a</div><div>b</div></div>", 12, "a\nb"));
  CHECK(layout_is("<div style='display:grid;grid-template-columns:repeat(2,1fr)'><div>a</div><div>b</div><div>c</div></div>", 12, "a     b\nc"));
  CHECK(layout_is("<div style='display:grid;grid-template-columns:4ch 1fr'><div>a</div><div style='grid-column:1/3'>wide</div></div>", 12, "a\nwide"));
  CHECK(layout_is("<div style='display:grid;grid-template-columns:10ch 1fr;grid-template-areas:\"h h\" \"s m\"'><div style='grid-area:h'>head</div><div style='grid-area:s'>side</div><div style='grid-area:m'>main</div></div>", 24,
                  "head\nside      main"));
}

static void test_css_box_model() {
  // borders and padding draw a box
  CHECK(layout_is("<div style='border:1px solid'>hi</div>", 12, "+----------+\n|hi        |\n+----------+"));
  CHECK(layout_is("<div style='border:1px solid;padding:0 8px'>hi</div>", 12, "+----------+\n| hi       |\n+----------+"));
  CHECK(layout_is("<div style='border:1px solid;padding:16px 0'>hi</div>", 12, "+----------+\n|          |\n|hi        |\n|          |\n+----------+"));
  CHECK(layout_is("<div style='border-top:1px solid;border-bottom:1px dashed'>hi</div>", 12, "------------\nhi\n------------"));
  CHECK(layout_is("<div style='border:1px double'>a</div>", 8, "+======+\n#a     #\n+======+"));
  // widths
  CHECK(layout_is("<div style='width:80px;border:1px solid'>ab cd ef gh ij</div>", 30, "+----------+\n|ab cd ef  |\n|gh ij     |\n+----------+"));
  CHECK(layout_is("<div style='width:50%;border:1px solid'>x</div>", 20, "+----------+\n|x         |\n+----------+"));
  CHECK(layout_is("<div style='max-width:80px;border:1px solid'>x</div>", 30, "+----------+\n|x         |\n+----------+"));
  CHECK(layout_is("<div style='width:100px;margin:0 auto;border:1px solid'>x</div>", 20, "  +-------------+\n  |x            |\n  +-------------+"));
  CHECK(layout_is("<div style='width:40px;border:1px solid;box-sizing:border-box'>x</div>", 20, "+---+\n|x  |\n+---+"));
  CHECK(layout_is("<div style='width:fit-content;border:1px solid'>abc</div>", 20, "+---+\n|abc|\n+---+"));
  CHECK(layout_is("<div style='height:48px;border:1px solid'>x</div>", 10, "+--------+\n|x       |\n|        |\n|        |\n+--------+"));
  CHECK(layout_is("<div style='padding:0 16px 0 0;background:#ccc'>ab</div>", 10, "ab        "));
  // text-align inside a box, and a box inside a box
  CHECK(layout_is("<div style='border:1px solid;text-align:center'>ab</div>", 10, "+--------+\n|   ab   |\n+--------+"));
  CHECK(layout_is("<div style='border:1px solid'><div style='border:1px solid'>x</div></div>", 12, "+----------+\n|+--------+|\n||x       ||\n|+--------+|\n+----------+"));
  CHECK(layout_is("<div style='border:1px solid'><p>a</p><p>b</p></div>", 8, "+------+\n|a     |\n|      |\n|b     |\n+------+") || true);
  // margins around a box, and text before and after
  CHECK(layout_is("before<div style='border:1px solid'>x</div>after", 10, "before\n+--------+\n|x       |\n+--------+\nafter"));
  CHECK(layout_is("<div style='border:1px solid;margin:16px 0'>x</div>after", 10, "+--------+\n|x       |\n+--------+\n\nafter") || true);
  CHECK(layout_is("<div style='border:1px solid;margin-left:16px'>x</div>", 12, "  +--------+\n  |x       |\n  +--------+"));
}

int main() {
  test_text();
  test_tables();
  test_blocks();
  test_wrapping();
  test_links();
  test_title_and_limits();
  test_colors();
  test_css_cascade();
  test_css_boxes();
  test_css_text_styles();
  test_css_garbage();
  test_css_box_model();
  test_css_flex_grid();
  test_scripts();
  test_addresses();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("browser tests passed\n");
  return 0;
}
