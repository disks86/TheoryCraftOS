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

// Host-side tests for the CSS engine: how values are read (units, calc(), var(), shorthands, keywords) and what the cascade gives the style of an element.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/Renderer.hpp"
#include "Dom/Form.hpp"
#include "Dom/HtmlBuilder.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static TestAllocator g_alloc;

// The page, its style sheet and the computed style of the element with that id.
struct Page {
  dom::Document doc;
  StyleSheet sheet;
  Page(const char* html, int width = 640, int height = 480, bool dark = false) : doc(g_alloc), sheet(g_alloc) {
    dom::build_document(doc, html, nullptr);
    RenderOptions options;
    options.media.width = width;
    options.media.height = height;
    options.media.dark = dark;
    collect_style_sheets(doc, options, &sheet);
    sheet.set_viewport(width, height);
  }
  Style style(const char* id) {
    const dom::Node* n = doc.element_by_id(id);
    return n ? computed_style(sheet, n) : Style();
  }
};

static int px(const Length& l) { return l.kind == Length::Px ? l.px100 / 100 : -999999; }
static bool near(double a, double b, double tolerance = 0.01) { return (a > b ? a - b : b - a) <= tolerance; }
static bool channels_near(uint32_t argb, int r, int g, int b, int tolerance = 3) {
  auto d = [](int x, int y) { return x > y ? x - y : y - x; };
  return d(static_cast<int>((argb >> 16) & 0xFF), r) <= tolerance && d(static_cast<int>((argb >> 8) & 0xFF), g) <= tolerance && d(static_cast<int>(argb & 0xFF), b) <= tolerance;
}

static void test_units_and_math() {
  Page p("<style>#a{width:100px;height:10em;margin-left:2rem;padding-top:5pt;min-width:1in;max-width:2.54cm} #b{width:50%;font-size:20px;height:calc(2em + 4px)} "
         "#c{width:calc(100% - 40px);margin-top:min(10px, 2em);margin-bottom:max(10px, 2em);padding-left:clamp(10px, 50vw, 100px)} #d{width:10vw;height:10vh;margin-left:4ch}"
         "#e{width:calc(2 * (10px + 5px) / 3);margin-left:calc(-1 * 8px);padding-left:25.4mm;margin-top:calc(1em - 4px)} #f{width:auto;height:min-content;max-width:none;min-height:max-content}"
         "#g{width:fit-content;margin-right:auto;margin-left:auto} #h{width:-5px;height:abc;margin-top:1 2} #i{font-size:150%;width:10em}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div>");
  Style a = p.style("a");
  CHECK(px(a.width) == 100 && px(a.height) == 160 && px(a.margin[3]) == 32 && px(a.padding[0]) == 6 && px(a.min_width) == 96 && px(a.max_width) == 96);
  Style b = p.style("b");
  CHECK(px(b.width) == 320 && px(b.height) == 44);   // (50% of the 640 pixel page; 2em of 20px and 4px)
  Style c = p.style("c");
  CHECK(px(c.width) == 600 && px(c.margin[0]) == 10 && px(c.margin[2]) == 32 && px(c.padding[3]) == 100);
  Style d = p.style("d");
  CHECK(px(d.width) == 64 && px(d.height) == 48 && px(d.margin[3]) == 32);
  Style e = p.style("e");
  CHECK(px(e.width) == 10 && px(e.margin[3]) == -8 && px(e.padding[3]) == 96 && px(e.margin[0]) == 12);
  Style f = p.style("f");
  CHECK(f.width.is_auto() && f.height.kind == Length::MinContent && f.max_width.is_none() && f.min_height.kind == Length::MaxContent);
  Style g = p.style("g");
  CHECK(g.width.kind == Length::FitContent && g.margin[1].is_auto() && g.margin[3].is_auto());
  Style h = p.style("h");
  CHECK(h.width.is_auto() && h.height.is_auto() && px(h.margin[0]) == 0);   // (a negative width, an unknown height and a margin of two numbers are not understood)
  Style i = p.style("i");
  CHECK(i.font_px == 24 && px(i.width) == 240);
  // legacy cells
  CHECK(a.margin_left == 4 && a.padding_top == 0 && a.width_px == 100 && a.height_px == 160);
}

static void test_vars() {
  Page p("<style>:root{--gap: 16px; --color: red; --size: calc(var(--gap) * 2)} #a{margin-left:var(--gap); color:var(--color); width:var(--size)} #b{--gap:32px; margin-left:var(--gap)}"
         "#c{margin-left:var(--nope, 8px); padding-left:var(--nope2, var(--gap)); width:var(--nope3)} #d{color:var(--color, blue)}"
         "#e{--a: var(--b); --b: var(--a); width: var(--a, 12px)} #f{--x: 1px 2px; margin: var(--x)} #g{--v: 20px} #g p{margin-left:var(--v)} #h{margin-left: var( --gap ) }</style>"
         "<div id=a>a</div><div id=b><div id=b2 style='margin-left:var(--gap)'>b</div></div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div>"
         "<div id=g><p id=g2>g</p></div><div id=h>h</div>");
  Style a = p.style("a");
  CHECK(px(a.margin[3]) == 16 && a.color == 0xFFFF0000 && px(a.width) == 32);
  CHECK(px(p.style("b").margin[3]) == 32 && px(p.style("b2").margin[3]) == 32);   // (inherited, and redefined for the subtree)
  Style c = p.style("c");
  CHECK(px(c.margin[3]) == 8 && px(c.padding[3]) == 16 && c.width.is_auto());
  CHECK(p.style("d").color == 0xFFFF0000);
  CHECK(p.style("e").width.is_auto() || px(p.style("e").width) == 12);   // (a cycle gives the fallback or nothing)
  Style f = p.style("f");
  CHECK(px(f.margin[0]) == 1 && px(f.margin[1]) == 2);
  CHECK(px(p.style("g2").margin[3]) == 20);
  CHECK(px(p.style("h").margin[3]) == 16);
}

static void test_shorthands() {
  Page p("<style>#a{margin:1px} #b{margin:1px 2px} #c{margin:1px 2px 3px} #d{margin:1px 2px 3px 4px} #e{padding:4px 8px; padding-right:0}"
         "#f{border:2px solid red} #g{border-top:1px dashed blue; border-bottom-width:4px; border-bottom-style:solid} #h{border:none} #i{border-width:1px 2px; border-style:solid; border-color:red green}"
         "#j{background:#abc url(x.png) no-repeat} #k{background:linear-gradient(red, blue), #123456} #l{font:italic bold 20px/30px sans-serif} #m{font:12px serif}"
         "#n{overflow:hidden scroll} #o{inset:1px 2px 3px 4px} #p{flex:1} #q{flex:2 3 10px} #r{flex:none} #s{flex:auto} #t{flex:0 0 25%} #u{flex-flow:column wrap}"
         "#v{gap:4px 8px} #w{grid-column:2 / 4; grid-row:span 2} #x{place-items:center start} #y{list-style:square inside} #z{text-decoration:underline line-through red}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div>"
         "<div id=j>j</div><div id=k>k</div><div id=l>l</div><div id=m>m</div><div id=n>n</div><div id=o>o</div><div id=p>p</div><div id=q>q</div><div id=r>r</div><div id=s>s</div>"
         "<div id=t>t</div><div id=u>u</div><div id=v>v</div><div id=w>w</div><div id=x>x</div><div id=y>y</div><div id=z>z</div>");
  Style a = p.style("a");
  CHECK(px(a.margin[0]) == 1 && px(a.margin[1]) == 1 && px(a.margin[2]) == 1 && px(a.margin[3]) == 1);
  Style b = p.style("b");
  CHECK(px(b.margin[0]) == 1 && px(b.margin[1]) == 2 && px(b.margin[2]) == 1 && px(b.margin[3]) == 2);
  Style c = p.style("c");
  CHECK(px(c.margin[0]) == 1 && px(c.margin[1]) == 2 && px(c.margin[2]) == 3 && px(c.margin[3]) == 2);
  Style d = p.style("d");
  CHECK(px(d.margin[0]) == 1 && px(d.margin[1]) == 2 && px(d.margin[2]) == 3 && px(d.margin[3]) == 4);
  Style e = p.style("e");
  CHECK(px(e.padding[0]) == 4 && px(e.padding[1]) == 0 && px(e.padding[3]) == 8);
  Style f = p.style("f");
  CHECK(f.border_px100[0] == 200 && f.border_style[3] == BorderStyle::Solid && f.border_color[2] == 0xFFFF0000);
  Style g = p.style("g");
  CHECK(g.border_px100[0] == 100 && g.border_style[0] == BorderStyle::Dashed && g.border_color[0] == 0xFF0000FF && g.border_px100[2] == 400 && g.border_style[2] == BorderStyle::Solid && g.border_style[1] == BorderStyle::None);
  Style h = p.style("h");
  CHECK(h.border_style[0] == BorderStyle::None);
  Style i = p.style("i");
  CHECK(i.border_px100[0] == 100 && i.border_px100[1] == 200 && i.border_px100[2] == 100 && i.border_px100[3] == 200 && i.border_color[0] == 0xFFFF0000 && i.border_color[1] == 0xFF008000 && i.border_style[2] == BorderStyle::Solid);
  CHECK(p.style("j").own_background && p.style("j").own_background_color == 0xFFAABBCC);
  CHECK(p.style("k").own_background && p.style("k").own_background_color == 0xFF123456);
  Style l = p.style("l");
  CHECK(l.italic && l.bold && l.font_px == 20 && l.line_height_px100 == 3000);
  Style m = p.style("m");
  CHECK(!m.italic && !m.bold && m.font_px == 12);
  Style n = p.style("n");
  CHECK(n.overflow_x == Overflow::Hidden && n.overflow_y == Overflow::Scroll);
  Style o = p.style("o");
  CHECK(px(o.inset[0]) == 1 && px(o.inset[1]) == 2 && px(o.inset[2]) == 3 && px(o.inset[3]) == 4);
  Style pp = p.style("p");
  CHECK(pp.flex_grow_x100 == 100 && pp.flex_shrink_x100 == 100 && px(pp.flex_basis) == 0);
  Style q = p.style("q");
  CHECK(q.flex_grow_x100 == 200 && q.flex_shrink_x100 == 300 && px(q.flex_basis) == 10);
  Style r = p.style("r");
  CHECK(r.flex_grow_x100 == 0 && r.flex_shrink_x100 == 0 && r.flex_basis.is_auto());
  Style s = p.style("s");
  CHECK(s.flex_grow_x100 == 100 && s.flex_shrink_x100 == 100 && s.flex_basis.is_auto());
  Style t = p.style("t");
  CHECK(t.flex_grow_x100 == 0 && t.flex_shrink_x100 == 0 && px(t.flex_basis) == 160);
  Style u = p.style("u");
  CHECK(u.flex_direction == FlexDirection::Column && u.flex_wrap == FlexWrap::Wrap);
  Style v = p.style("v");
  CHECK(px(v.row_gap) == 4 && px(v.column_gap) == 8);
  Style w = p.style("w");
  CHECK(p.sheet.text(w.grid_column_start).equals("2") && p.sheet.text(w.grid_column_end).equals("4") && p.sheet.text(w.grid_row_start).equals("span 2"));
  Style x = p.style("x");
  CHECK(x.align_items == Place::Center && x.justify_items == Place::Start);
  Style y = p.style("y");
  CHECK(y.list_type == 2 && y.list_inside && !y.list_none);
  Style z = p.style("z");
  CHECK(z.underline && z.strike);
}

static void test_keywords_and_cascade() {
  Page p("<style>#a{color:red;margin-left:10px;width:5px} #a2{color:inherit;margin-left:inherit;width:inherit} #a3{color:initial;margin-left:initial;width:unset}"
         "#b{font-size:20px} #b2{font-size:inherit; width:2em} #b3{font-size:smaller} #b4{font-size:larger} #b5{font-size:xx-large} #b6{font-size:50%}"
         "p{color:red} p.x{color:blue} #i{color:lime} .y{color:green !important} p{color:black !important}"
         "#imp{width:1px !important} #imp{width:2px} .multi{margin:0 !important; margin-top:9px}"
         "#bad1{width:5px; width:wrong; height:3px} #bad2{unknown-property:1; color:blue} #bad3{color:#12; color:red;;;;} #bad4{;color:blue;} </style>"
         "<div id=a><div id=a2>x</div><div id=a3>y</div></div><div id=b><div id=b2>b</div><div id=b3>b</div><div id=b4>b</div><div id=b5>b</div><div id=b6>b</div></div>"
         "<p id=p1>p</p><p id=p2 class=x>p</p><p id=i class='x'>p</p><p id=p4 class=y>p</p>"
         "<div id=imp style='width:3px'>i</div><div id=m class=multi>m</div><div id=bad1>1</div><div id=bad2>2</div><div id=bad3>3</div><div id=bad4>4</div>");
  Style a2 = p.style("a2");
  CHECK(a2.color == 0xFFFF0000 && px(a2.margin[3]) == 10 && px(a2.width) == 5);   // (inherit copies what the parent has, also for properties that are not inherited)
  CHECK(px(p.style("a").margin[3]) == 10);
  Style a3 = p.style("a3");
  CHECK(a3.color == 0 && px(a3.margin[3]) == 0 && a3.width.is_auto());
  CHECK(p.style("b2").font_px == 20 && px(p.style("b2").width) == 40);
  CHECK(p.style("b3").font_px == 17 && p.style("b4").font_px == 24 && p.style("b5").font_px == 32 && p.style("b6").font_px == 10);
  CHECK(p.style("p1").color == 0xFF000000);   // (!important wins over the more specific rules)
  CHECK(p.style("p2").color == 0xFF000000 && p.style("p4").color == 0xFF008000);   // (among !important declarations specificity decides again)
  CHECK(px(p.style("imp").width) == 1);
  Style m = p.style("m");
  CHECK(px(m.margin[0]) == 0 && px(m.margin[1]) == 0);   // (both !important: the one that comes last of the important ones wins)
  CHECK(px(p.style("bad1").width) == 5 && px(p.style("bad1").height) == 3);
  CHECK(p.style("bad2").color == 0xFF0000FF && p.style("bad3").color == 0xFFFF0000 && p.style("bad4").color == 0xFF0000FF);
}

static void test_display_and_boxes() {
  Page p("<style>#a{display:flex} #b{display:inline-flex} #c{display:grid} #d{display:inline-grid} #e{display:inline-block} #f{display:table} #g{display:table-row}"
         "#h{display:table-cell} #i{display:contents} #j{display:none} #k{display:list-item} #l{display:block flow} #m{display:inline flex} #n{display:flow-root} #o{display:wat}"
         "#p{position:relative;float:left;clear:both;z-index:3} #q{position:absolute;top:5px;left:calc(50% - 10px)} #r{box-sizing:border-box;overflow-x:auto}"
         "#s{visibility:hidden} #t{opacity:0} #u{white-space:nowrap} #v{white-space:pre-wrap} #w{text-transform:capitalize;text-align:justify} #x{vertical-align:middle;text-overflow:ellipsis}</style>"
         "<span id=a>a</span><span id=b>b</span><span id=c>c</span><span id=d>d</span><span id=e>e</span><span id=f>f</span><span id=g>g</span><span id=h>h</span><span id=i>i</span>"
         "<span id=j>j</span><span id=k>k</span><span id=l>l</span><span id=m>m</span><span id=n>n</span><span id=o>o</span><span id=p>p</span><span id=q>q</span><span id=r>r</span>"
         "<span id=s>s</span><span id=t>t</span><span id=u>u</span><span id=v>v</span><span id=w>w</span><span id=x>x</span>");
  Style a = p.style("a");
  CHECK(a.display == Display::Block && a.layout == Layout::Flex && !a.inline_box);
  Style b = p.style("b");
  CHECK(b.display == Display::Inline && b.layout == Layout::Flex && b.inline_box);
  CHECK(p.style("c").layout == Layout::Grid && p.style("c").display == Display::Block);
  CHECK(p.style("d").layout == Layout::Grid && p.style("d").inline_box);
  Style e = p.style("e");
  CHECK(e.display == Display::Inline && e.inline_box && e.layout == Layout::Flow);
  CHECK(p.style("f").layout == Layout::Table && p.style("g").layout == Layout::TableRow && p.style("h").layout == Layout::TableCell);
  CHECK(p.style("i").contents && p.style("j").display == Display::None && p.style("k").display == Display::ListItem);
  CHECK(p.style("l").display == Display::Block && p.style("m").layout == Layout::Flex && p.style("m").inline_box && p.style("n").display == Display::Block);
  CHECK(p.style("o").display == Display::Block);
  Style pp = p.style("p");
  CHECK(pp.position == Position::Relative && pp.float_side == Float::Left && pp.clear == Clear::Both && pp.z_index == 3);
  Style q = p.style("q");
  CHECK(q.position == Position::Absolute && px(q.inset[0]) == 5 && px(q.inset[3]) == 310);
  Style r = p.style("r");
  CHECK(r.sizing == BoxSizing::BorderBox && r.overflow_x == Overflow::Auto && r.overflow_y == Overflow::Visible);
  CHECK(p.style("s").hidden && p.style("t").hidden && p.style("t").opaque_zero);
  CHECK(p.style("u").white_space == 1 && !p.style("u").pre && p.style("v").white_space == 3 && p.style("v").pre);
  Style w = p.style("w");
  CHECK(w.transform == 3 && w.justify && w.align == 0);
  Style x = p.style("x");
  CHECK(x.vertical_align == 2 && x.text_ellipsis);
}

static void test_selectors_and_at_rules() {
  Page p("<style>@media (max-width: 500px) { #a{color:red} } @media (min-width: 500px) { #a{color:blue} } @supports (display:grid) { #b{color:red} } @supports (wat:1) { #b{color:blue} }"
         "@supports not (wat:1) { #c{color:red} } @supports (display:flex) and (wat:1) { #c{color:blue} } @supports (wat:1) or (display:flex) { #d{color:red} }"
         "@layer base { #e{color:red} } @container (min-width: 1px) { #f{color:red} } @font-face { font-family: x; src: url(x) } @keyframes k { from { color: red } to { color: blue } } @import url(a.css); #g{color:red}"
         "#h:hover{color:red} #h:active{color:blue} #h:focus-within{color:lime} #i::before{content:'x';color:red} #i::after{content:'y'} #i{color:blue}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h tabindex=0><input id=h2></div><div id=i>i</div>");
  CHECK(p.style("a").color == 0xFF0000FF && p.style("b").color == 0xFFFF0000 && p.style("c").color == 0xFFFF0000 && p.style("d").color == 0xFFFF0000);
  CHECK(p.style("e").color == 0xFFFF0000 && p.style("f").color == 0xFFFF0000 && p.style("g").color == 0xFFFF0000);
  // states
  dom::Node* h = p.doc.element_by_id("h");
  CHECK(p.style("h").color == 0);
  h->ui_state = dom::kUiHover;
  CHECK(p.style("h").color == 0xFFFF0000);
  h->ui_state = dom::kUiHover | dom::kUiActive;
  CHECK(p.style("h").color == 0xFF0000FF);
  h->ui_state = 0;
  p.doc.element_by_id("h2")->form_state |= dom::kFormFocused;
  CHECK(p.style("h").color == 0xFF00FF00);
  // pseudo-elements
  dom::Node* i = p.doc.element_by_id("i");
  CHECK(p.sheet.has_pseudo(i, PseudoElement::Before) && p.sheet.has_pseudo(i, PseudoElement::After) && !p.sheet.has_pseudo(i, PseudoElement::Marker) && !p.sheet.has_pseudo(p.doc.element_by_id("a"), PseudoElement::Before));
  Style base = computed_style(p.sheet, i);
  Style before = base.inherit();
  p.sheet.apply(i, StringView(), base, &before, PseudoElement::Before);
  CHECK(before.has_content && p.sheet.text(before.content).equals("'x'") && before.color == 0xFFFF0000);
  Style plain = base.inherit();
  p.sheet.apply(i, StringView(), base, &plain, PseudoElement::None);
  CHECK(!plain.has_content && plain.color == 0xFF0000FF);
}

// Rules and values made of garbage must not crash anything.
static void test_garbage() {
  const char* sheets[] = {
      "a{b", "a{b:", "a{b:}", "{}", "}}}", "a{width:calc(}", "a{width:calc(1px +)}", "a{width:calc(1px 2px)}", "a{width:var(}", "a{width:var(--x,}", "a{--x:var(--x)} a{width:var(--x)}",
      "a{margin:1px 2px 3px 4px 5px 6px}", "a{font:}", "a{font:/}", "a{font:12px/}", "a{flex:1 2 3 4}", "a{grid-area:/ / /}", "a{grid:/}", "a{grid:\"a\" / \"b\"}",
      "@media{{{{", "@supports (", "@layer;@layer a{", "a{content:\"unterminated", "a{width:1e999px}", "a{width:1e-999px}", "a{margin:calc(1px/0)}", "a{width:min()}",
      "a{width:clamp(1px)}", "a{width:clamp(1px,2px)}", "a{width:max(1px,)}", "a{border:solid solid solid}", "a{background:,}", "a{background:a,b,c}",
  };
  for (const char* css : sheets) {
    std::string html = std::string("<style>") + css + "</style><a id=a>x</a>";
    Page page(html.c_str());
    (void)page.style("a");
  }
  // truncations and bit flips of a real sheet
  const std::string real = "div.c>p:first-child{color:#ff0000;margin:calc(1em + 2px) auto;width:min(50%,40em);font:italic bold 12px/1.5 serif;grid-template-columns:repeat(2,1fr);display:flex;"
                           "flex:1 1 var(--x,10px)} :root{--x:1px} p::before{content:'a'} @media (min-width:1px){a{color:red}}";
  for (size_t n = 0; n <= real.size(); ++n) {
    std::string html = "<style>" + real.substr(0, n) + "</style><div class=c><p id=a>x</p></div>";
    Page page(html.c_str());
    (void)page.style("a");
  }
  for (size_t i = 0; i < real.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string css = real;
      css[i] = static_cast<char>(css[i] ^ (1 << bit));
      std::string html = "<style>" + css + "</style><div class=c><p id=a>x</p></div>";
      Page page(html.c_str());
      (void)page.style("a");
    }
  }
}


// --- the pixel engine's properties --------------------------------------------------------------------------

static void test_fonts() {
  Page p("<style>html{font-size:20px} #a{font-family:'Helvetica Neue', Arial, sans-serif;font-weight:300;font-stretch:condensed;font-variant:small-caps;letter-spacing:0.1em;word-spacing:3px}"
         "#b{font-weight:bold} #b2{font-weight:bolder} #b3{font-weight:lighter} #c{font-weight:850;font-stretch:62.5%} #d{font:italic small-caps 700 expanded 12px/20px \"Fira Sans\", monospace}"
         "#e{font-family:serif,;font-weight:2000;font-stretch:wat;letter-spacing:5;font-variant:wat} #f{font-variant-numeric:tabular-nums slashed-zero;font-variant-ligatures:none}"
         "#g{font-weight:normal} #g2{font-weight:bolder} #h{font-family:monospace;letter-spacing:normal} #h2{font-family:inherit} #i{font-weight:calc(100 * 3)} #j{font-family:ui-monospace,system-ui}</style>"
         "<div id=a>a</div><div id=b><span id=b2>x</span></div><div id=b3x style='font-weight:900'><span id=b3>y</span></div><div id=c>c</div><div id=d>d</div><div id=e>e</div>"
         "<div id=f>f</div><div id=g><span id=g2>g</span></div><div id=h><span id=h2>h</span></div><div id=i>i</div><div id=j>j</div>");
  Style a = p.style("a");
  List<FontFamily> families(g_alloc);
  CHECK(p.sheet.font_families(a, &families) && families.count() == 3);
  CHECK(families[0].name.equals("Helvetica Neue") && families[0].generic == GenericFamily::None && families[1].name.equals("Arial") && families[2].generic == GenericFamily::SansSerif && families[2].name.empty());
  CHECK(a.font_weight == 300 && !a.bold && a.font_stretch_x10 == 750 && a.font_variant == kVariantSmallCaps);
  CHECK(a.letter_spacing_px100 == 200 && a.word_spacing_px100 == 300);   // (0.1em of 20px)
  Style b = p.style("b"), b2 = p.style("b2");
  CHECK(b.font_weight == 700 && b.bold && b2.font_weight == 900 && b2.bold);   // (bolder than 700 is 900)
  CHECK(p.style("b3").font_weight == 700);   // (lighter than 900 is 700)
  Style c = p.style("c");
  CHECK(c.font_weight == 850 && c.bold && c.font_stretch_x10 == 625);
  Style d = p.style("d");
  CHECK(d.italic && d.font_weight == 700 && d.font_stretch_x10 == 1250 && d.font_variant == kVariantSmallCaps && d.font_px == 12 && d.line_height_px100 == 2000);
  List<FontFamily> dfam(g_alloc);
  CHECK(p.sheet.font_families(d, &dfam) && dfam.count() == 2 && dfam[0].name.equals("Fira Sans") && dfam[1].generic == GenericFamily::Monospace);
  Style e = p.style("e");   // malformed values leave the defaults
  List<FontFamily> efam(g_alloc);
  CHECK(e.font_family.empty() && p.sheet.font_families(e, &efam) && efam.count() == 0);
  CHECK(e.font_weight == 400 && e.font_stretch_x10 == 1000 && e.letter_spacing_px100 == 0 && e.font_variant == 0);
  Style f = p.style("f");
  CHECK(f.font_variant == (kVariantTabularNums | kVariantSlashedZero | kVariantNoLigatures));
  CHECK(p.style("g2").font_weight == 700 && p.style("g").font_weight == 400);   // (bolder than 400 is 700)
  Style h2 = p.style("h2");   // inherited
  CHECK(!h2.font_family.empty() && p.sheet.text(h2.font_family).equals("monospace"));
  CHECK(p.style("i").font_weight == 300);
  List<FontFamily> j(g_alloc);
  CHECK(p.sheet.font_families(p.style("j"), &j) && j.count() == 2 && j[0].generic == GenericFamily::UiMonospace && j[1].generic == GenericFamily::SystemUi);
  // inheritance: letter-spacing and weight pass on, the radius does not
  Page q("<style>#a{font-weight:600;letter-spacing:2px;word-spacing:1px;border-radius:5px;font-stretch:expanded}</style><div id=a><span id=b>x</span></div>");
  Style qb = q.style("b");
  CHECK(qb.font_weight == 600 && qb.bold && qb.letter_spacing_px100 == 200 && qb.word_spacing_px100 == 100 && qb.font_stretch_x10 == 1250 && qb.radius[0].h.x100 == 0);
}

static void test_border_radius() {
  Page p("<style>#a{border-radius:10px} #b{border-radius:10px 20px} #c{border-radius:1px 2px 3px} #d{border-radius:1px 2px 3px 4px} #e{border-radius:10px 20px / 5px 15px 25px 35px}"
         "#f{border-radius:50%} #g{border-top-left-radius:8px 4px;border-bottom-right-radius:3px} #h{border-radius:5px;border-top-right-radius:0} #i{border-radius:-5px;border-top-left-radius:wat}"
         "#j{border-radius:1px 2px 3px 4px 5px} #k{border-start-start-radius:7px} #l{border-radius:2em} #m{border-radius:inherit}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div>"
         "<div id=k>k</div><div id=l>l</div><div id=m>m</div>");
  Style a = p.style("a");
  for (int i = 0; i < 4; ++i) CHECK(a.radius[i].h.kind == Dimension::Px && a.radius[i].h.x100 == 1000 && a.radius[i].v.x100 == 1000);
  Style b = p.style("b");
  CHECK(b.radius[0].h.x100 == 1000 && b.radius[1].h.x100 == 2000 && b.radius[2].h.x100 == 1000 && b.radius[3].h.x100 == 2000);
  Style c = p.style("c");
  CHECK(c.radius[0].h.x100 == 100 && c.radius[1].h.x100 == 200 && c.radius[2].h.x100 == 300 && c.radius[3].h.x100 == 200);
  Style d = p.style("d");
  CHECK(d.radius[0].h.x100 == 100 && d.radius[1].h.x100 == 200 && d.radius[2].h.x100 == 300 && d.radius[3].h.x100 == 400);
  Style e = p.style("e");   // elliptical: horizontal radii 10 20 10 20, vertical 5 15 25 35
  CHECK(e.radius[0].h.x100 == 1000 && e.radius[0].v.x100 == 500 && e.radius[1].h.x100 == 2000 && e.radius[1].v.x100 == 1500 && e.radius[2].v.x100 == 2500 && e.radius[3].v.x100 == 3500);
  Style f = p.style("f");
  CHECK(f.radius[0].h.kind == Dimension::Percent && f.radius[0].h.x100 == 5000 && f.radius[3].v.kind == Dimension::Percent);
  CHECK(f.radius[0].h.resolve(20000) == 10000);   // 50% of 200px
  Style g = p.style("g");
  CHECK(g.radius[0].h.x100 == 800 && g.radius[0].v.x100 == 400 && g.radius[2].h.x100 == 300 && g.radius[2].v.x100 == 300 && g.radius[1].h.x100 == 0);
  Style h = p.style("h");
  CHECK(h.radius[0].h.x100 == 500 && h.radius[1].h.x100 == 0 && h.radius[2].h.x100 == 500);
  Style i = p.style("i");
  CHECK(i.radius[0].h.x100 == 0);   // (negative and unknown values are ignored)
  CHECK(p.style("j").radius[0].h.x100 == 0);
  CHECK(p.style("k").radius[0].h.x100 == 700);
  CHECK(p.style("l").radius[0].h.x100 == 3200);
}

static void test_shadows() {
  Page p("<style>#a{color:#123456;box-shadow:1px 2px 3px 4px red, inset 0 0 5px rgba(0,0,0,.5), 0 1px blue} #b{text-shadow:1px 1px 2px #fff, -1px 0 green} #c{box-shadow:none}"
         "#d{box-shadow:1px} #e{box-shadow:1px 2px red red} #f{text-shadow:1px 2px 3px 4px red} #g{box-shadow:2px 2px 1em currentcolor} #h{box-shadow:red 1px 2px -3px} #i{box-shadow:1px 2px 3px red,}"
         "#j{box-shadow:0 0 0 1px}</style>"
         "<div id=a>a</div><div id=b><span id=b2>x</span></div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div>");
  Style a = p.style("a");
  List<Shadow> sh(g_alloc);
  CHECK(p.sheet.shadows(a.box_shadow, a, &sh) && sh.count() == 3);
  CHECK(sh[0].x_px100 == 100 && sh[0].y_px100 == 200 && sh[0].blur_px100 == 300 && sh[0].spread_px100 == 400 && sh[0].color == 0xFFFF0000 && !sh[0].current_color && !sh[0].inset);
  CHECK(sh[1].inset && sh[1].blur_px100 == 500 && (sh[1].color >> 24) == 128 && (sh[1].color & 0xFFFFFF) == 0);
  CHECK(sh[2].x_px100 == 0 && sh[2].y_px100 == 100 && sh[2].blur_px100 == 0 && sh[2].color == 0xFF0000FF);
  Style b = p.style("b2");   // text-shadow is inherited
  List<Shadow> ts(g_alloc);
  CHECK(p.sheet.shadows(b.text_shadow, b, &ts) && ts.count() == 2 && ts[0].color == 0xFFFFFFFF && ts[1].x_px100 == -100 && ts[1].color == 0xFF008000 && ts[1].blur_px100 == 0);
  Style c = p.style("c");
  CHECK(c.box_shadow.empty());
  CHECK(p.style("d").box_shadow.empty() && p.style("e").box_shadow.empty() && p.style("f").text_shadow.empty() && p.style("h").box_shadow.empty() && p.style("i").box_shadow.empty());   // (malformed)
  Style g = p.style("g");
  List<Shadow> gs(g_alloc);
  CHECK(p.sheet.shadows(g.box_shadow, g, &gs) && gs.count() == 1 && gs[0].current_color && gs[0].blur_px100 == 1600);
  List<Shadow> js(g_alloc);
  Style j = p.style("j");
  CHECK(p.sheet.shadows(j.box_shadow, j, &js) && js.count() == 1 && js[0].current_color && js[0].spread_px100 == 100);
  CHECK(p.style("b").box_shadow.empty());   // (not inherited)
  Page q("<style>#a{box-shadow:1px 1px red} #b{box-shadow:inherit}</style><div id=a><div id=b>x</div></div>");
  CHECK(!q.style("b").box_shadow.empty());
}

static void test_backgrounds() {
  Page p("<style>"
         "#a{background:url(a.png) no-repeat center / cover, linear-gradient(45deg, red 0%, blue 100%) 10px 20px / 30px auto repeat-x padding-box content-box fixed, #123456}"
         "#b{background-image:linear-gradient(to top right, red, #00f 50%, green);background-size:contain;background-position:right 10px bottom 20%;background-repeat:space round;background-clip:text;background-origin:content-box}"
         "#c{background-image:radial-gradient(circle 40px at 25% 75%, red, blue), conic-gradient(from 90deg at center, red 0deg, blue 180deg, red 1turn), repeating-linear-gradient(red 0 10px, blue 10px 20px)}"
         "#d{background:none} #e{background-image:wat(1)} #f{background-image:url(x.png),none;background-size:10px,20px 30%,auto;background-repeat:repeat-x} #g{background:url('q.png')}"
         "#h{background:red url(h.png)} #i{background-image:linear-gradient(red)} #j{background-position:left} #k{background-image:radial-gradient(ellipse closest-side, red, blue)}"
         "#l{background-image:linear-gradient(red, 20px, blue)} #m{background:inherit} #m0{background:url(z.png) 5px 6px}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div><div id=k>k</div>"
         "<div id=l>l</div><div id=m0><div id=m>m</div></div>");
  Style a = p.style("a");
  CHECK(a.own_background && a.own_background_color == 0xFF123456);   // (the colour is the last layer's)
  List<BackgroundLayer> layers(g_alloc);
  CHECK(p.sheet.background_layers(a, &layers) && layers.count() == 3 && layers[2].image == BackgroundLayer::None);   // (the colour is a layer of its own in the list)
  CHECK(layers[0].image == BackgroundLayer::Url && layers[0].url.equals("a.png") && layers[0].repeat_x == RepeatMode::NoRepeat && layers[0].repeat_y == RepeatMode::NoRepeat &&
        layers[0].size_mode == BackgroundLayer::Cover && layers[0].position_x.percent_x100 == 5000 && layers[0].position_y.percent_x100 == 5000);
  CHECK(layers[1].image == BackgroundLayer::GradientImage && layers[1].gradient.kind == Gradient::Linear && layers[1].gradient.angle_x100 == 4500 && layers[1].gradient.stop_count == 2 &&
        layers[1].gradient.stops[0].color == 0xFFFF0000 && layers[1].gradient.stops[0].position.kind == Dimension::Percent && layers[1].gradient.stops[1].color == 0xFF0000FF &&
        layers[1].gradient.stops[1].position.x100 == 10000);
  CHECK(layers[1].position_x.px100 == 1000 && layers[1].position_y.px100 == 2000 && layers[1].size_mode == BackgroundLayer::Explicit && layers[1].size_w.x100 == 3000 && layers[1].size_h.is_auto() &&
        layers[1].repeat_x == RepeatMode::Repeat && layers[1].repeat_y == RepeatMode::NoRepeat && layers[1].origin == BackgroundBox::PaddingBox && layers[1].clip == BackgroundBox::ContentBox &&
        layers[1].attachment == BackgroundAttachment::Fixed);
  Style b = p.style("b");
  List<BackgroundLayer> bl(g_alloc);
  CHECK(p.sheet.background_layers(b, &bl) && bl.count() == 1);
  const Gradient& bg = bl[0].gradient;
  CHECK(bg.to_sides == 3 && bg.stop_count == 3 && bg.stops[1].color == 0xFF0000FF && bg.stops[1].position.x100 == 5000 && bg.stops[2].position.is_auto());
  CHECK(bl[0].size_mode == BackgroundLayer::Contain && bl[0].position_x.percent_x100 == 10000 && bl[0].position_x.px100 == -1000 && bl[0].position_y.percent_x100 == 8000 &&
        bl[0].repeat_x == RepeatMode::Space && bl[0].repeat_y == RepeatMode::Round && bl[0].clip == BackgroundBox::Text && bl[0].origin == BackgroundBox::ContentBox);
  Style c = p.style("c");
  List<BackgroundLayer> cl(g_alloc);
  CHECK(p.sheet.background_layers(c, &cl) && cl.count() == 3);
  const Gradient& radial = cl[0].gradient;
  CHECK(radial.kind == Gradient::Radial && radial.circle && radial.extent == Gradient::Explicit && radial.radius_x.x100 == 4000 && radial.center_x.percent_x100 == 2500 && radial.center_y.percent_x100 == 7500 &&
        radial.stop_count == 2);
  const Gradient& conic = cl[1].gradient;
  CHECK(conic.kind == Gradient::Conic && conic.angle_x100 == 9000 && conic.stop_count == 3 && conic.stops[1].position.x100 == 5000 && conic.stops[2].position.x100 == 10000);
  const Gradient& rep = cl[2].gradient;
  CHECK(rep.kind == Gradient::Linear && rep.repeating && rep.stop_count == 4 && rep.stops[1].position.x100 == 1000 && rep.stops[2].position.x100 == 1000 && rep.stops[3].position.x100 == 2000);
  List<BackgroundLayer> dl(g_alloc);
  CHECK(p.style("d").background_image.empty() && p.sheet.background_layers(p.style("d"), &dl) && dl.count() == 0);
  CHECK(p.style("e").background_image.empty() && p.style("i").background_image.empty());   // (malformed)
  List<BackgroundLayer> ll(g_alloc);
  CHECK(p.sheet.background_layers(p.style("l"), &ll) && ll.count() == 1 && ll[0].gradient.stop_count == 3 && ll[0].gradient.stops[1].hint && ll[0].gradient.stops[1].position.x100 == 2000);   // (a hint)
  Style f = p.style("f");
  List<BackgroundLayer> fl(g_alloc);
  CHECK(p.sheet.background_layers(f, &fl) && fl.count() == 2 && fl[1].image == BackgroundLayer::None && fl[1].size_w.x100 == 2000 && fl[1].size_h.kind == Dimension::Percent && fl[0].size_w.x100 == 1000 &&
        fl[0].repeat_x == RepeatMode::Repeat && fl[0].repeat_y == RepeatMode::NoRepeat && fl[1].repeat_y == RepeatMode::NoRepeat);
  List<BackgroundLayer> gl(g_alloc);
  CHECK(p.sheet.background_layers(p.style("g"), &gl) && gl.count() == 1 && gl[0].url.equals("q.png"));
  Style h = p.style("h");
  CHECK(h.own_background_color == 0xFFFF0000 && !h.background_image.empty());
  CHECK(p.sheet.text(p.style("j").background_position).equals("left"));
  List<BackgroundLayer> kl(g_alloc);
  CHECK(p.sheet.background_layers(p.style("k"), &kl) && kl.count() == 1 && !kl[0].gradient.circle && kl[0].gradient.extent == Gradient::ClosestSide);
  CHECK(!p.style("m").background_image.empty() && p.sheet.text(p.style("m").background_position).equals("5px 6px"));   // (inherit copies it)
}

static void test_opacity_visibility() {
  Page p("<style>#a{opacity:.5} #b{opacity:50%} #c{opacity:0} #d{opacity:2} #e{opacity:-1} #f{opacity:wat} #g{opacity:.25} #g2{opacity:inherit} #h{visibility:hidden} #h2{visibility:visible} #i{visibility:collapse}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g><div id=g2>g</div></div><div id=h><span id=h2>x</span><span id=h3>y</span></div><div id=i>i</div>");
  CHECK(p.style("a").opacity_x1000 == 500 && !p.style("a").opaque_zero && p.style("b").opacity_x1000 == 500);
  CHECK(p.style("c").opacity_x1000 == 0 && p.style("c").opaque_zero && p.style("c").hidden);   // (opaque_zero keeps working)
  CHECK(p.style("d").opacity_x1000 == 1000 && p.style("e").opacity_x1000 == 0 && p.style("f").opacity_x1000 == 1000);
  CHECK(p.style("g").opacity_x1000 == 250 && p.style("g2").opacity_x1000 == 250);
  Style h = p.style("h");
  CHECK(h.hidden && h.visibility == Visibility::Hidden && p.style("h3").visibility == Visibility::Hidden);
  Style h2 = p.style("h2");
  CHECK(!h2.hidden && h2.visibility == Visibility::Visible);   // (a visible child of a hidden parent is shown)
  CHECK(p.style("i").visibility == Visibility::Collapse && p.style("i").hidden);
  CHECK(p.style("a").visibility == Visibility::Visible && p.style("a").display == Display::Block);
}

static void test_transforms() {
  Page p("<style>#a{transform:translate(10px, 20px) rotate(90deg) scale(2)} #b{translate:5px 6px;rotate:45deg;scale:2 3;transform:skewX(10deg)} #c{transform:none} #d{transform:wat(1)}"
         "#e{transform:matrix(1,0,0,1,30,40)} #f{transform-origin:left top} #g{transform-origin:10px 20% 5px} #h{transform:translate(50%, 25%);transform-origin:0 0} #i{rotate:x 30deg}"
         "#j{rotate:1 0 0 .5turn} #k{translate:1em} #l{transform:rotate(1deg" "} #m{transform:scale(1,2,3)} #n{transform:perspective(100px) translateZ(10px)}"
         "#o{transform:translate(10px) rotate(180deg);transform-origin:100px 50px}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div><div id=k>k</div>"
         "<div id=l>l</div><div id=m>m</div><div id=n>n</div><div id=o>o</div>");
  Style a = p.style("a");
  List<TransformOp> ops(g_alloc);
  CHECK(p.sheet.transform_ops(a, &ops) && ops.count() == 3);
  CHECK(ops[0].kind == TransformOp::Translate && ops[0].x.x100 == 1000 && ops[0].y.x100 == 2000 && ops[1].kind == TransformOp::Rotate && near(ops[1].angle_deg, 90) && ops[2].kind == TransformOp::Scale && ops[2].sx == 2 && ops[2].sy == 2);
  double m[16];
  CHECK(p.sheet.transform_matrix(a, 100, 100, m, false));
  // translate(10,20) * rotate(90deg) * scale(2): x axis goes to y, scaled by two
  CHECK(near(m[0], 0, 1e-6) && near(m[1], 2, 1e-6) && near(m[4], -2, 1e-6) && near(m[5], 0, 1e-6) && near(m[12], 10, 1e-6) && near(m[13], 20, 1e-6) && m[10] == 1 && m[15] == 1);
  Style b = p.style("b");
  List<TransformOp> bops(g_alloc);
  CHECK(p.sheet.transform_ops(b, &bops) && bops.count() == 4 && bops[0].kind == TransformOp::Translate && bops[1].kind == TransformOp::Rotate && bops[2].kind == TransformOp::Scale && bops[2].sx == 2 && bops[2].sy == 3 &&
        bops[3].kind == TransformOp::Skew && near(bops[3].angle_deg, 10));   // (translate, rotate, scale, then transform)
  Style c = p.style("c");
  List<TransformOp> cops(g_alloc);
  CHECK(c.transform_value.empty() && p.sheet.transform_ops(c, &cops) && cops.count() == 0 && !p.sheet.transform_matrix(c, 10, 10, m));
  CHECK(p.style("d").transform_value.empty() && p.style("l").transform_value.empty() && p.style("m").transform_value.empty());   // (malformed)
  Style e = p.style("e");
  CHECK(p.sheet.transform_matrix(e, 10, 10, m, true) && near(m[12], 30) && near(m[13], 40) && near(m[0], 1));
  Style f = p.style("f");
  CHECK(f.transform_origin_x.percent_x100 == 0 && f.transform_origin_y.percent_x100 == 0);
  Style g = p.style("g");
  CHECK(g.transform_origin_x.px100 == 1000 && g.transform_origin_y.percent_x100 == 2000 && g.transform_origin_z_px100 == 500);
  Style h = p.style("h");
  CHECK(p.sheet.transform_matrix(h, 200, 100, m, true) && near(m[12], 100) && near(m[13], 25));   // (percentages are of the box)
  Style i = p.style("i");
  List<TransformOp> iops(g_alloc);
  CHECK(p.sheet.transform_ops(i, &iops) && iops.count() == 1 && iops[0].ax == 1 && iops[0].az == 0 && near(iops[0].angle_deg, 30));
  List<TransformOp> jops(g_alloc);
  CHECK(p.sheet.transform_ops(p.style("j"), &jops) && jops.count() == 1 && near(jops[0].angle_deg, 180) && jops[0].ax == 1);
  List<TransformOp> kops(g_alloc);
  CHECK(p.sheet.transform_ops(p.style("k"), &kops) && kops.count() == 1 && kops[0].x.x100 == 1600);
  CHECK(p.sheet.transform_matrix(p.style("n"), 10, 10, m, false) && near(m[11], -0.01, 1e-9) && near(m[14], 10, 1e-6) && near(m[15], 0.9, 1e-9));
  // around the origin: rotate(180deg) about (100,50) maps the origin to (200,100), then the translation
  Style o = p.style("o");
  CHECK(p.sheet.transform_matrix(o, 200, 100, m, true) && near(m[12], 210, 1e-6) && near(m[13], 100, 1e-6) && near(m[0], -1, 1e-6));
  Page q("<style>#a{transform:rotate(10deg);translate:1px;opacity:.5;border-radius:3px}</style><div id=a><div id=b>x</div></div>");
  CHECK(q.style("b").transform_value.empty() && q.style("b").translate.empty() && q.style("b").opacity_x1000 == 1000);   // (not inherited)
}

static void test_misc_properties() {
  Page p("<style>#a{outline:2px dashed red;outline-offset:3px} #b{outline:auto} #c{outline-width:thick;outline-style:solid;outline-color:invert} #d{outline:wat} #e{cursor:url(a.cur) 2 2, url(b.cur), pointer}"
         "#f{cursor:not-allowed} #g{cursor:wat} #h{pointer-events:none} #i{user-select:none} #j{aspect-ratio:16 / 9} #k{aspect-ratio:auto 4/3} #l{aspect-ratio:2} #m{aspect-ratio:wat} #n{aspect-ratio:0 / 1}"
         "#o{object-fit:cover;object-position:left 10px top 20px} #p{object-fit:scale-down;object-position:25% bottom} #q{list-style-image:url(dot.png)} #r{list-style:square url(x.png) inside} #s{list-style:circle}"
         "#t{column-rule:3px dotted blue} #u{column-rule-width:thin;column-rule-style:solid} #v{writing-mode:vertical-rl;direction:rtl} #w{scroll-snap-type:x mandatory;scroll-snap-align:start center;scroll-snap-stop:always;"
         "overscroll-behavior:contain none;scroll-behavior:smooth} #x{container:sidebar / inline-size} #y{container-type:size;container-name:a b} #z{color-scheme:light dark}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e><span id=e2>x</span></div><div id=f>f</div><div id=g>g</div><div id=h><span id=h2>h</span></div><div id=i><span id=i2>i</span></div>"
         "<div id=j>j</div><div id=k>k</div><div id=l>l</div><div id=m>m</div><div id=n>n</div><div id=o>o</div><div id=p>p</div><div id=q><span id=q2>q</span></div><div id=r>r</div><div id=s>s</div><div id=t>t</div>"
         "<div id=u>u</div><div id=v><span id=v2>v</span></div><div id=w>w</div><div id=x>x</div><div id=y>y</div><div id=z><span id=z2>z</span></div>");
  Style a = p.style("a");
  CHECK(a.outline_width_px100 == 200 && a.outline_style == BorderStyle::Dashed && a.outline_color == 0xFFFF0000 && a.outline_offset_px100 == 300 && !a.outline_auto);
  Style b = p.style("b");
  CHECK(b.outline_auto && b.outline_style == BorderStyle::Solid);
  Style c = p.style("c");
  CHECK(c.outline_width_px100 == 500 && c.outline_style == BorderStyle::Solid && c.outline_color == 0);
  CHECK(p.style("d").outline_style == BorderStyle::None);
  CHECK(p.style("e").cursor == Cursor::Pointer && p.style("e2").cursor == Cursor::Pointer && p.style("f").cursor == Cursor::NotAllowed && p.style("g").cursor == Cursor::Auto);
  CHECK(p.style("h").pointer_events == PointerEvents::None && p.style("h2").pointer_events == PointerEvents::None && p.style("i2").user_select == UserSelect::None);
  CHECK(p.style("j").aspect_ratio_x1000 == 1778 && !p.style("j").aspect_auto);
  CHECK(p.style("k").aspect_auto && p.style("k").aspect_ratio_x1000 == 1333 && p.style("l").aspect_ratio_x1000 == 2000);
  CHECK(p.style("m").aspect_ratio_x1000 == 0 && !p.style("m").aspect_auto && p.style("n").aspect_ratio_x1000 == 0);
  Style o = p.style("o");
  CHECK(o.object_fit == ObjectFit::Cover && o.object_position_x.percent_x100 == 0 && o.object_position_x.px100 == 1000 && o.object_position_y.px100 == 2000);
  Style pp = p.style("p");
  CHECK(pp.object_fit == ObjectFit::ScaleDown && pp.object_position_x.percent_x100 == 2500 && pp.object_position_y.percent_x100 == 10000);
  CHECK(p.sheet.text(p.style("q").list_style_image).equals("url(dot.png)") && p.sheet.text(p.style("q2").list_style_image).equals("url(dot.png)"));
  Style r = p.style("r");
  CHECK(r.list_type == 2 && r.list_inside && p.sheet.text(r.list_style_image).equals("url(x.png)"));
  CHECK(p.style("s").list_style_image.empty() && p.style("s").list_type == 1);
  Style t = p.style("t");
  CHECK(t.column_rule_width_px100 == 300 && t.column_rule_style == BorderStyle::Dotted && t.column_rule_color == 0xFF0000FF);
  CHECK(p.style("u").column_rule_width_px100 == 100 && p.style("u").column_rule_style == BorderStyle::Solid);
  Style v = p.style("v2");
  CHECK(v.writing_mode == WritingMode::VerticalRl && v.direction == Direction::Rtl);
  Style w = p.style("w");
  CHECK(p.sheet.text(w.scroll_snap_type).equals("x mandatory") && p.sheet.text(w.scroll_snap_align).equals("start center") && w.scroll_snap_stop_always &&
        w.overscroll_x == OverscrollBehavior::Contain && w.overscroll_y == OverscrollBehavior::None && w.scroll_smooth);
  Style x = p.style("x");
  CHECK(x.container_type == 1 && p.sheet.text(x.container_name).equals("sidebar"));
  Style y = p.style("y");
  CHECK(y.container_type == 2 && p.sheet.text(y.container_name).equals("a b"));
  CHECK(p.style("z").color_scheme == ColorScheme::LightDark && p.style("z2").color_scheme == ColorScheme::LightDark);
}

static void test_transitions_and_animations() {
  Page p("<style>#a{transition:opacity .3s ease-in 0.1s, transform 1s linear} #b{transition-property:a, b, c;transition-duration:1s, 2s} #c{transition:1s -0.5s ease-out;transition-behavior:allow-discrete}"
         "#d{animation:spin 2s infinite alternate both paused cubic-bezier(.1,.7,1,.1) .5s} #e{animation-name:a, b;animation-duration:1s;animation-iteration-count:3, infinite;animation-direction:reverse, alternate-reverse}"
         "#f{transition:foo bar} #g{transition-duration:-1s} #h{animation-timing-function:cubic-bezier(2,0,0,0)} #i{animation:fade 1s steps(4, jump-start), slide 2s steps(2)} #j{transition:none} #k{animation:none}"
         "#l{transition:all 200ms} #m{animation-name:x;animation-duration:auto} #n{animation:x 1s step-end}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div><div id=k>k</div>"
         "<div id=l>l</div><div id=m>m</div><div id=n>n</div>");
  List<Transition> t(g_alloc);
  CHECK(p.sheet.transitions(p.style("a"), &t) && t.count() == 2);
  CHECK(t[0].property.equals("opacity") && t[0].duration_ms == 300 && t[0].delay_ms == 100 && t[0].timing.kind == TimingFunction::CubicBezier && near(t[0].timing.x1, 0.42) && near(t[0].timing.y2, 1));
  CHECK(t[1].property.equals("transform") && t[1].duration_ms == 1000 && t[1].timing.kind == TimingFunction::Linear && t[1].delay_ms == 0);
  List<Transition> t2(g_alloc);
  CHECK(p.sheet.transitions(p.style("b"), &t2) && t2.count() == 3 && t2[0].duration_ms == 1000 && t2[1].duration_ms == 2000 && t2[2].duration_ms == 1000 && t2[2].property.equals("c"));   // (the lists repeat)
  List<Transition> t3(g_alloc);
  CHECK(p.sheet.transitions(p.style("c"), &t3) && t3.count() == 1 && t3[0].property.equals("all") && t3[0].duration_ms == 1000 && t3[0].delay_ms == -500 && t3[0].allow_discrete && near(t3[0].timing.x1, 0));
  List<Animation> an(g_alloc);
  CHECK(p.sheet.animations(p.style("d"), &an) && an.count() == 1);
  CHECK(an[0].name.equals("spin") && an[0].duration_ms == 2000 && an[0].iterations == -1 && an[0].direction == Animation::Alternate && an[0].fill == Animation::Both && an[0].paused && an[0].delay_ms == 500 &&
        near(an[0].timing.x1, 0.1) && near(an[0].timing.y1, 0.7));
  List<Animation> an2(g_alloc);
  CHECK(p.sheet.animations(p.style("e"), &an2) && an2.count() == 2 && an2[0].iterations == 3 && an2[1].iterations == -1 && an2[0].direction == Animation::Reverse && an2[1].direction == Animation::AlternateReverse &&
        an2[1].duration_ms == 1000);
  List<Transition> t4(g_alloc);
  CHECK(p.sheet.transitions(p.style("f"), &t4) && t4.count() == 0);   // (two property names: not a transition)
  List<Transition> t5(g_alloc);
  CHECK(p.sheet.transitions(p.style("g"), &t5) && t5.count() == 0);
  CHECK(p.style("h").animation_timing.empty());
  List<Animation> an3(g_alloc);
  CHECK(p.sheet.animations(p.style("i"), &an3) && an3.count() == 2 && an3[0].timing.kind == TimingFunction::Steps && an3[0].timing.steps == 4 && an3[0].timing.jump_start && an3[1].timing.steps == 2 && !an3[1].timing.jump_start);
  List<Transition> t6(g_alloc);
  CHECK(p.sheet.transitions(p.style("j"), &t6) && t6.count() == 1 && t6[0].property.equals("none"));
  List<Animation> an4(g_alloc);
  CHECK(p.sheet.animations(p.style("k"), &an4) && an4.count() == 1 && an4[0].name.empty());
  List<Transition> t7(g_alloc);
  CHECK(p.sheet.transitions(p.style("l"), &t7) && t7.count() == 1 && t7[0].duration_ms == 200 && t7[0].property.equals("all"));
  List<Animation> an5(g_alloc);
  CHECK(p.sheet.animations(p.style("m"), &an5) && an5.count() == 1 && an5[0].duration_ms == 0);
  List<Animation> an6(g_alloc);
  CHECK(p.sheet.animations(p.style("n"), &an6) && an6.count() == 1 && an6[0].timing.kind == TimingFunction::Steps && an6[0].timing.steps == 1 && !an6[0].timing.jump_start);
  Page q("<style>#a{transition:opacity 1s} </style><div id=a><div id=b>x</div></div>");
  CHECK(q.style("b").transition_property.empty());   // (not inherited)
}

static void test_keyframes() {
  Page p("<style>@keyframes k { from { opacity: 0; transform: scale(1) } 50%, 60% { opacity: .5; color: red } to { opacity: 1 } bad { opacity: 1 } 120% { opacity: 1 } }"
         "@-webkit-keyframes w { to { width: 10px } } @keyframes k { from { opacity: .25 } to { opacity: .75 } } @keyframes { from { } } @keyframes none { from { } }</style><div id=a>a</div>");
  CHECK(p.sheet.keyframes_count() == 3);   // k, w, k (a later one of the same name wins)
  uint32_t rule = 99;
  CHECK(p.sheet.find_keyframes("k", &rule) && rule == 2 && p.sheet.keyframes_name(rule).equals("k") && p.sheet.keyframe_count(rule) == 2);
  CHECK(p.sheet.keyframe_offset_x1000(rule, 0) == 0 && p.sheet.keyframe_offset_x1000(rule, 1) == 1000);
  Style base;
  Style frame = base.inherit();
  p.sheet.apply_keyframe(rule, 0, base, &frame);
  CHECK(frame.opacity_x1000 == 250);
  CHECK(p.sheet.find_keyframes("w", &rule) && p.sheet.keyframe_count(rule) == 1);
  Style wframe = base.inherit();
  p.sheet.apply_keyframe(rule, 0, base, &wframe);
  CHECK(px(wframe.width) == 10);
  CHECK(!p.sheet.find_keyframes("nope", &rule));
  // the first k: offsets are sorted and the stops are kept apart
  CHECK(p.sheet.keyframes_name(0).equals("k") && p.sheet.keyframe_count(0) == 4);
  CHECK(p.sheet.keyframe_offset_x1000(0, 0) == 0 && p.sheet.keyframe_offset_x1000(0, 1) == 500 && p.sheet.keyframe_offset_x1000(0, 2) == 600 && p.sheet.keyframe_offset_x1000(0, 3) == 1000);
  Style mid = base.inherit();
  p.sheet.apply_keyframe(0, 2, base, &mid);
  CHECK(mid.opacity_x1000 == 500 && mid.color == 0xFFFF0000);
  CHECK(p.sheet.keyframe_declaration_count(0, 0) == 2);
  StringView name, value;
  CHECK(p.sheet.keyframe_declaration(0, 0, 1, &name, &value) && name.equals("transform") && value.equals("scale(1)"));
  CHECK(!p.sheet.keyframe_declaration(0, 0, 2, &name, &value));
}

static void test_filters_and_clip_path() {
  Page p("<style>#a{filter:blur(4px) brightness(150%) drop-shadow(1px 2px 3px red) hue-rotate(90deg) url(#f) grayscale()} #b{filter:blur(red)} #c{filter:none} #d{backdrop-filter:blur(2px) saturate(2)}"
         "#e{clip-path:circle(50% at 10px 20%)} #f{clip-path:inset(10px 20px round 5px)} #g{clip-path:polygon(evenodd, 0 0, 100% 0, 50% 100%)} #h{clip-path:path(\"M0 0L10 10\")} #i{clip-path:url(#c)}"
         "#j{clip-path:border-box} #k{clip-path:ellipse(10px 20px) content-box} #l{clip-path:circle(wat)} #m{clip-path:polygon(0 0, 1)} #n{clip-path:circle(farthest-side)} #o{filter:blur(-1px)}"
         "#p{filter:opacity(50%) invert(1) sepia(.5) contrast(2)}</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div><div id=k>k</div>"
         "<div id=l>l</div><div id=m>m</div><div id=n>n</div><div id=o>o</div><div id=p>p</div>");
  Style a = p.style("a");
  List<FilterFunction> f(g_alloc);
  CHECK(p.sheet.filters(a.filter, a, &f) && f.count() == 6);
  CHECK(f[0].kind == FilterFunction::Blur && near(f[0].amount, 4) && f[1].kind == FilterFunction::Brightness && near(f[1].amount, 1.5) && f[2].kind == FilterFunction::DropShadow &&
        f[2].shadow.x_px100 == 100 && f[2].shadow.blur_px100 == 300 && f[2].shadow.color == 0xFFFF0000 && f[3].kind == FilterFunction::HueRotate && near(f[3].amount, 90) &&
        f[4].kind == FilterFunction::Url && f[4].url.equals("#f") && f[5].kind == FilterFunction::Grayscale && near(f[5].amount, 1));
  CHECK(p.style("b").filter.empty() && p.style("o").filter.empty() && p.style("c").filter.empty());
  Style d = p.style("d");
  List<FilterFunction> bf(g_alloc);
  CHECK(p.sheet.filters(d.backdrop_filter, d, &bf) && bf.count() == 2 && bf[1].kind == FilterFunction::Saturate && near(bf[1].amount, 2));
  Style pp = p.style("p");
  List<FilterFunction> pf(g_alloc);
  CHECK(p.sheet.filters(pp.filter, pp, &pf) && pf.count() == 4 && near(pf[0].amount, 0.5) && pf[1].kind == FilterFunction::Invert && near(pf[3].amount, 2));
  ClipPath clip;
  CHECK(p.sheet.clip_path(p.style("e"), &clip) && clip.kind == ClipPath::Circle && clip.radius_x.kind == Dimension::Percent && clip.radius_x.x100 == 5000 && clip.center_x.px100 == 1000 && clip.center_y.percent_x100 == 2000);
  CHECK(p.sheet.clip_path(p.style("f"), &clip) && clip.kind == ClipPath::Inset && clip.inset[0].x100 == 1000 && clip.inset[1].x100 == 2000 && clip.inset[2].x100 == 1000 && clip.inset[3].x100 == 2000 &&
        clip.round[0].h.x100 == 500 && clip.round[3].v.x100 == 500);
  CHECK(p.sheet.clip_path(p.style("g"), &clip) && clip.kind == ClipPath::Polygon && clip.evenodd && clip.point_count == 3 && clip.points_x[1].kind == Dimension::Percent && clip.points_x[1].x100 == 10000 && clip.points_y[2].x100 == 10000);
  CHECK(p.sheet.clip_path(p.style("h"), &clip) && clip.kind == ClipPath::Path && clip.path_data.equals("M0 0L10 10"));
  CHECK(p.sheet.clip_path(p.style("i"), &clip) && clip.kind == ClipPath::Url && clip.url.equals("#c"));
  CHECK(p.sheet.clip_path(p.style("j"), &clip) && clip.kind == ClipPath::Box && clip.reference_box == BackgroundBox::BorderBox);
  CHECK(p.sheet.clip_path(p.style("k"), &clip) && clip.kind == ClipPath::Ellipse && clip.radius_x.x100 == 1000 && clip.radius_y.x100 == 2000 && clip.reference_box == BackgroundBox::ContentBox);
  CHECK(p.style("l").clip_path.empty() && p.style("m").clip_path.empty());
  CHECK(p.sheet.clip_path(p.style("n"), &clip) && clip.kind == ClipPath::Circle && clip.radius_far && clip.radius_x.is_auto());
}

static void test_math_and_functions() {
  Page p("<html style='font-size:20px'><head><style>"
         "#a{width:calc(round(7.4px, 2px))} #a2{width:calc(round(up, 7.1px, 2px))} #a3{width:calc(round(down, 7.9px, 2px))} #a4{margin-left:calc(round(to-zero, -7.9px, 2px))}"
         "#b{width:mod(-7px, 3px);margin-left:rem(-7px, 3px)} #c{width:calc(sin(90deg) * 10px);margin-left:calc(cos(0) * 10px);padding-left:calc(tan(45deg) * 10px)}"
         "#d{width:calc(pow(2, 3) * 1px);margin-left:calc(sqrt(16) * 1px);padding-left:hypot(3px, 4px)} #e{width:calc(log(e) * 10px);margin-left:calc(exp(0) * 3px);padding-left:calc(sign(-5px) * 3px)}"
         "#f{width:calc(pi * 100px)} #g{transform:rotate(atan2(1, 1))} #g2{transform:rotate(asin(1))} #g3{transform:rotate(calc(sin(90deg) * 10deg))} #h{width:2rem} #i{width:calc(1 / 0 * 1px)} #j{width:round(1px, 0px)}"
         "#k{width:calc(sqrt(-1) * 1px)} #l{width:min(10px, 20px, 5px, 40px)} #m{width:max(1px, 2px, 3px, 4px, 5px)} #n{margin-left:env(safe-area-inset-top, 5px);padding-left:env(unknown-thing, 7px)}"
         "#o{padding-left:calc(10px + env(safe-area-inset-left))} #p{width:attr(data-w px)} #p2{width:attr(data-missing px, 12px)} #p3{width:attr(data-missing px)} #p4{margin-left:attr(data-bad px, 3px)}"
         "#p5::before{content:attr(data-w)} #q{width:abs(-4px)} #r{width:calc(pow(-8, 1/3) * 1px)} #s{width:calc(pow(2, 0.5) * 100px)} #t{width:calc(log(100, 10) * 5px)} #u{width:1rem}"
         "</style></head><body><div id=a>a</div><div id=a2>a</div><div id=a3>a</div><div id=a4>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div>"
         "<div id=g>g</div><div id=g2>g</div><div id=g3>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div><div id=k>k</div><div id=l>l</div><div id=m>m</div><div id=n>n</div><div id=o>o</div>"
         "<div id=p data-w=30>p</div><div id=p2>p</div><div id=p3>p</div><div id=p4 data-bad=abc>p</div><div id=p5 data-w=hello>p</div><div id=q>q</div><div id=r>r</div><div id=s>s</div><div id=t>t</div>"
         "<div id=u>u</div></body></html>");
  CHECK(px(p.style("a").width) == 8 && px(p.style("a2").width) == 8 && px(p.style("a3").width) == 6 && px(p.style("a4").margin[3]) == -6);
  Style b = p.style("b");
  CHECK(px(b.width) == 2 && px(b.margin[3]) == -1);
  Style c = p.style("c");
  CHECK(px(c.width) == 10 && px(c.margin[3]) == 10 && px(c.padding[3]) == 10);
  Style d = p.style("d");
  CHECK(px(d.width) == 8 && px(d.margin[3]) == 4 && px(d.padding[3]) == 5);
  Style e = p.style("e");
  CHECK(px(e.width) == 10 && px(e.margin[3]) == 3 && e.padding[3].px100 == 0);   // (the padding would be negative: not understood)
  CHECK(near(p.style("f").width.px100, 31416, 1));
  {
    List<TransformOp> ops(g_alloc);
    CHECK(p.sheet.transform_ops(p.style("g"), &ops) && ops.count() == 1 && near(ops[0].angle_deg, 45, 1e-6));
    List<TransformOp> ops2(g_alloc);
    CHECK(p.sheet.transform_ops(p.style("g2"), &ops2) && ops2.count() == 1 && near(ops2[0].angle_deg, 90, 1e-6));
    List<TransformOp> ops3(g_alloc);
    CHECK(p.sheet.transform_ops(p.style("g3"), &ops3) && ops3.count() == 1 && near(ops3[0].angle_deg, 10, 1e-6));
  }
  CHECK(px(p.style("h").width) == 40);   // (the root's font size is 20px)
  CHECK(p.style("i").width.is_auto() && p.style("j").width.is_auto() && p.style("k").width.is_auto());   // (division by zero, a modulus of 0 and the root of a negative number are errors)
  CHECK(px(p.style("l").width) == 5 && px(p.style("m").width) == 5);
  Style n = p.style("n");
  CHECK(px(n.margin[3]) == 0 && px(n.padding[3]) == 7);
  CHECK(px(p.style("o").padding[3]) == 10);
  CHECK(px(p.style("p").width) == 30 && px(p.style("p2").width) == 12 && p.style("p3").width.is_auto() && px(p.style("p4").margin[3]) == 3);
  Style p5 = p.style("p5");
  Style before = p5.inherit();
  p.sheet.apply(p.doc.element_by_id("p5"), StringView(), p5, &before, PseudoElement::Before);
  CHECK(before.has_content && p.sheet.text(before.content).equals("attr(data-w)"));   // (attr() in content is left for the layout)
  CHECK(px(p.style("q").width) == 4);
  CHECK(p.style("r").width.is_auto());   // (the cube root of a negative number through pow() is not a number)
  CHECK(near(p.style("s").width.px100, 14142, 2) && px(p.style("t").width) == 10);
  CHECK(px(p.style("u").width) == 20);
  // rem follows the root element's font size, also when it is given in rem itself (against the initial 16px)
  Page q("<html style='font-size:2rem'><body><div id=a style='width:1rem'>a</div><div id=b style='font-size:10px;width:3rem'>b</div></body></html>");
  CHECK(q.style("a").font_px == 32 && px(q.style("a").width) == 32 && px(q.style("b").width) == 96 && q.style("a").root_font_px == 32);
}

static void test_color_functions() {
  uint32_t c;
  CHECK(parse_css_color_alpha("oklch(0.628 0.2577 29.23)", &c) && channels_near(c, 255, 0, 0, 6) && (c >> 24) == 255);
  CHECK(parse_css_color_alpha("oklch(100% 0 0)", &c) && channels_near(c, 255, 255, 255, 1));
  CHECK(parse_css_color_alpha("oklab(1 0 0)", &c) && channels_near(c, 255, 255, 255, 1));
  CHECK(parse_css_color_alpha("oklab(0 0 0 / 50%)", &c) && channels_near(c, 0, 0, 0, 1) && (c >> 24) >= 127 && (c >> 24) <= 128);
  CHECK(parse_css_color_alpha("lab(50% 0 0)", &c) && channels_near(c, 119, 119, 119, 2));
  CHECK(parse_css_color_alpha("lab(54.29 80.8 69.89)", &c) && channels_near(c, 255, 0, 0, 8));
  CHECK(parse_css_color_alpha("lch(50% 0 0)", &c) && channels_near(c, 119, 119, 119, 2));
  CHECK(parse_css_color_alpha("lch(54.29 106.84 40.85)", &c) && channels_near(c, 255, 0, 0, 8));
  CHECK(parse_css_color_alpha("color(srgb 1 0 0)", &c) && c == 0xFFFF0000);
  CHECK(parse_css_color_alpha("color(srgb 0.5 0.5 0.5 / 0.5)", &c) && channels_near(c, 128, 128, 128, 1) && (c >> 24) >= 127);
  CHECK(parse_css_color_alpha("color(display-p3 1 0 0)", &c) && channels_near(c, 255, 0, 0, 2));
  CHECK(parse_css_color_alpha("color(srgb-linear 1 1 1)", &c) && c == 0xFFFFFFFF);
  CHECK(parse_css_color_alpha("color(xyz-d65 0.9505 1 1.089)", &c) && channels_near(c, 255, 255, 255, 2));
  CHECK(parse_css_color_alpha("color(a98-rgb 1 1 1)", &c) && channels_near(c, 255, 255, 255, 2));
  CHECK(parse_css_color_alpha("color(prophoto-rgb 1 1 1)", &c) && channels_near(c, 255, 255, 255, 3));
  CHECK(parse_css_color_alpha("color(rec2020 1 1 1)", &c) && channels_near(c, 255, 255, 255, 3));
  CHECK(parse_css_color_alpha("color-mix(in srgb, red, blue)", &c) && c == 0xFF800080);
  CHECK(parse_css_color_alpha("color-mix(in srgb, red 25%, blue)", &c) && c == 0xFF4000BF);
  CHECK(parse_css_color_alpha("color-mix(in srgb, red 30%, blue 30%)", &c) && (c & 0xFFFFFF) == 0x800080 && (c >> 24) == 153);   // (the percentages add up to 60%: the result is 60% opaque)
  CHECK(parse_css_color_alpha("color-mix(in srgb, red, transparent)", &c) && channels_near(c, 255, 0, 0, 1) && (c >> 24) >= 127 && (c >> 24) <= 128);
  CHECK(parse_css_color_alpha("color-mix(in oklab, white, black)", &c) && channels_near(c, 99, 99, 99, 4));
  CHECK(parse_css_color_alpha("color-mix(in oklch, red, blue)", &c) && (c >> 24) == 255);
  CHECK(parse_css_color_alpha("color-mix(in oklch longer hue, red, blue)", &c) && (c >> 24) == 255);
  CHECK(parse_css_color_alpha("color-mix(in hsl, red, lime)", &c) && channels_near(c, 255, 255, 0, 3));
  CHECK(parse_css_color_alpha("color-mix(in lab, color-mix(in srgb, red, red), white 0%)", &c) && channels_near(c, 255, 0, 0, 6));
  CHECK(parse_css_color_alpha("color-mix(in srgb, currentcolor, white)", &c) && channels_near(c, 128, 128, 128, 1));
  CHECK(parse_css_color_alpha("light-dark(red, blue)", &c) && c == 0xFFFF0000);
  // malformed
  CHECK(!parse_css_color_alpha("oklch(0.5)", &c) && !parse_css_color_alpha("lab(wat 1 2)", &c) && !parse_css_color_alpha("color(wat 1 2 3)", &c) && !parse_css_color_alpha("color-mix(red, blue)", &c) &&
        !parse_css_color_alpha("color-mix(in srgb, red 10% 20%, blue)", &c) && !parse_css_color_alpha("color-mix(in srgb, red, blue, green)", &c) && !parse_css_color_alpha("light-dark(red)", &c) &&
        !parse_css_color_alpha("color-mix(in srgb, red -5%, blue)", &c) && !parse_css_color_alpha("oklab(1 2 3 4 5)", &c) && !parse_css_color_alpha("color-mix(in wat, red, blue)", &c) &&
        !parse_css_color_alpha("light-dark(red, wat)", &c));
  uint32_t plain;
  CHECK(parse_css_color("oklch(0.628 0.2577 29.23)", &plain) && channels_near(plain, 255, 0, 0, 6) && (plain >> 24) == 255);
  CHECK(parse_css_color("color(srgb 1 0 0 / 0.1)", &plain) && plain == 0);   // (mostly transparent is not shown, like the other colours)
  // in a style
  Page light("<style>#a{color:light-dark(red, blue)} #b{color-scheme:dark;color:light-dark(red, blue)} #c{background-color:color-mix(in srgb, red, blue)} #d{color:color-mix(in srgb, currentcolor, white)}"
             "#e{border-top-color:light-dark(#111, #eee);border-top-style:solid} #f{color-scheme:light dark;color:light-dark(red, blue)}</style><div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div>");
  Page dark("<style>#a{color:light-dark(red, blue)} #b{color-scheme:light;color:light-dark(red, blue)} #e{border-top-color:light-dark(#111, #eee);border-top-style:solid}</style><div id=a>a</div><div id=b>b</div><div id=e>e</div>",
            640, 480, true);
  CHECK(light.style("a").color == 0xFFFF0000 && light.style("b").color == 0xFF0000FF && light.style("c").own_background_color == 0xFF800080);
  CHECK(light.style("e").border_color[0] == 0xFF111111 && light.style("f").color == 0xFFFF0000);
  CHECK(dark.style("a").color == 0xFF0000FF && dark.style("b").color == 0xFFFF0000 && dark.style("e").border_color[0] == 0xFFEEEEEE);
}

static void test_cascade_layers() {
  Page p("<style>@layer base, theme; @layer theme { #a { color: blue } #c { color: blue !important } } @layer base { #a { color: red } #c { color: red !important } }"
         "#b { color: green } @layer theme { #b { color: lime } } #c { color: green !important }"
         "@layer outer { @layer inner { #d { color: red } } #d { color: blue } } @layer outer.inner { #e { color: red } } @layer outer { #e { color: blue } }"
         "@layer { #f { color: red } } @layer { #g { color: red } } #g { color: blue } @layer late { #h { color: red } } @layer early, late; @layer early { #h { color: blue } }"
         "@layer x { #i { color: red } #i { color: blue } } @layer a.b.c { #j { color: lime } } #k { color: red } @layer z { #k { color: blue !important } }</style>"
         "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g>g</div><div id=h>h</div><div id=i>i</div><div id=j>j</div><div id=k>k</div>");
  CHECK(p.style("a").color == 0xFF0000FF);   // later layers win over earlier ones
  CHECK(p.style("b").color == 0xFF008000);   // unlayered rules win over layers
  CHECK(p.style("c").color == 0xFFFF0000);   // for !important it is the other way round: the first layer wins, the unlayered rule loses
  CHECK(p.style("d").color == 0xFF0000FF);   // the rules of a layer itself come after its sub-layers
  CHECK(p.style("e").color == 0xFF0000FF);
  CHECK(p.style("f").color == 0xFFFF0000 && p.style("g").color == 0xFF0000FF);
  CHECK(p.style("h").color == 0xFF0000FF);   // (late was mentioned first: early comes after it and wins)
  CHECK(p.style("i").color == 0xFF0000FF && p.style("j").color == 0xFF00FF00);
  CHECK(p.style("k").color == 0xFF0000FF);   // (an !important rule in a layer beats an unlayered normal one)
  CHECK(p.sheet.layer_count() >= 10);
  // layer(name) set by the page for an @import
  Page q("<style>@layer base { #a { color: red } } #b { color: red }</style><div id=a>a</div><div id=b>b</div>");
  q.sheet.set_layer("later");
  MediaEnvironment env;
  q.sheet.parse("#a { color: blue } #b { color: blue }", env);
  q.sheet.set_layer(StringView());
  CHECK(q.style("a").color == 0xFF0000FF && q.style("b").color == 0xFFFF0000);   // (in a layer after base; unlayered stays on top)
}

// Every property of the pixel engine through inherit, initial and unset, and whether a child starts with the parent's value.
struct PropertyRow {
  const char* name;
  const char* value;
  bool inherited;
  std::string (*signature)(const StyleSheet&, const Style&);
};
static std::string sig_num(long long v) { return std::to_string(v); }
#define SIG(expr) [](const StyleSheet& sheet, const Style& s) -> std::string { (void)sheet; return (expr); }
#define TXT(ref) std::string(std::string(sheet.text(s.ref).data(), sheet.text(s.ref).size()))

static void test_property_matrix() {
  static const PropertyRow rows[] = {
      {"font-family", "'Q Font', serif", true, SIG(TXT(font_family))},
      {"font-weight", "800", true, SIG(sig_num(s.font_weight) + (s.bold ? "b" : ""))},
      {"font-stretch", "150%", true, SIG(sig_num(s.font_stretch_x10))},
      {"font-variant-caps", "small-caps", true, SIG(sig_num(s.font_variant & kVariantCapsMask))},
      {"font-variant-numeric", "tabular-nums", true, SIG(sig_num(s.font_variant & kVariantNumericMask))},
      {"font-variant-ligatures", "none", true, SIG(sig_num(s.font_variant & kVariantNoLigatures))},
      {"letter-spacing", "3px", true, SIG(sig_num(s.letter_spacing_px100))},
      {"word-spacing", "4px", true, SIG(sig_num(s.word_spacing_px100))},
      {"border-top-left-radius", "7px 8px", false, SIG(sig_num(s.radius[0].h.x100) + "," + sig_num(s.radius[0].v.x100))},
      {"border-bottom-left-radius", "9px", false, SIG(sig_num(s.radius[3].h.x100))},
      {"border-radius", "6px", false, SIG(sig_num(s.radius[1].h.x100) + sig_num(s.radius[2].v.x100))},
      {"box-shadow", "1px 1px red", false, SIG(TXT(box_shadow))},
      {"text-shadow", "2px 2px blue", true, SIG(TXT(text_shadow))},
      {"background-image", "url(a.png)", false, SIG(TXT(background_image))},
      {"background-size", "cover", false, SIG(TXT(background_size))},
      {"background-position", "10px 20px", false, SIG(TXT(background_position))},
      {"background-repeat", "no-repeat", false, SIG(TXT(background_repeat))},
      {"background-clip", "content-box", false, SIG(TXT(background_clip))},
      {"background-origin", "border-box", false, SIG(TXT(background_origin))},
      {"background-attachment", "fixed", false, SIG(TXT(background_attachment))},
      {"opacity", "0.4", false, SIG(sig_num(s.opacity_x1000))},
      {"transform", "rotate(3deg)", false, SIG(TXT(transform_value))},
      {"translate", "3px 4px", false, SIG(TXT(translate))},
      {"rotate", "9deg", false, SIG(TXT(rotate))},
      {"scale", "1.5", false, SIG(TXT(scale))},
      {"transform-origin", "10px 20px 30px", false, SIG(sig_num(s.transform_origin_x.px100) + "," + sig_num(s.transform_origin_y.px100) + "," + sig_num(s.transform_origin_z_px100))},
      {"outline-width", "5px", false, SIG(sig_num(s.outline_width_px100))},
      {"outline-style", "dotted", false, SIG(sig_num(static_cast<int>(s.outline_style)))},
      {"outline-color", "#abcdef", false, SIG(sig_num(s.outline_color))},
      {"outline-offset", "6px", false, SIG(sig_num(s.outline_offset_px100))},
      {"cursor", "grab", true, SIG(sig_num(static_cast<int>(s.cursor)))},
      {"pointer-events", "none", true, SIG(sig_num(static_cast<int>(s.pointer_events)))},
      {"user-select", "text", true, SIG(sig_num(static_cast<int>(s.user_select)))},
      {"aspect-ratio", "3 / 2", false, SIG(sig_num(s.aspect_ratio_x1000))},
      {"object-fit", "contain", false, SIG(sig_num(static_cast<int>(s.object_fit)))},
      {"object-position", "10px 20px", false, SIG(sig_num(s.object_position_x.px100) + "," + sig_num(s.object_position_y.px100))},
      {"list-style-image", "url(l.png)", true, SIG(TXT(list_style_image))},
      {"column-rule-width", "6px", false, SIG(sig_num(s.column_rule_width_px100))},
      {"column-rule-style", "double", false, SIG(sig_num(static_cast<int>(s.column_rule_style)))},
      {"column-rule-color", "#123", false, SIG(sig_num(s.column_rule_color))},
      {"transition-property", "color", false, SIG(TXT(transition_property))},
      {"transition-duration", "2s", false, SIG(TXT(transition_duration))},
      {"transition-timing-function", "linear", false, SIG(TXT(transition_timing))},
      {"transition-delay", "1s", false, SIG(TXT(transition_delay))},
      {"transition-behavior", "allow-discrete", false, SIG(TXT(transition_behavior))},
      {"animation-name", "foo", false, SIG(TXT(animation_name))},
      {"animation-duration", "3s", false, SIG(TXT(animation_duration))},
      {"animation-timing-function", "ease-in", false, SIG(TXT(animation_timing))},
      {"animation-delay", "2s", false, SIG(TXT(animation_delay))},
      {"animation-iteration-count", "4", false, SIG(TXT(animation_iteration_count))},
      {"animation-direction", "reverse", false, SIG(TXT(animation_direction))},
      {"animation-fill-mode", "both", false, SIG(TXT(animation_fill_mode))},
      {"animation-play-state", "paused", false, SIG(TXT(animation_play_state))},
      {"filter", "blur(3px)", false, SIG(TXT(filter))},
      {"backdrop-filter", "blur(4px)", false, SIG(TXT(backdrop_filter))},
      {"clip-path", "circle(40%)", false, SIG(TXT(clip_path))},
      {"writing-mode", "vertical-lr", true, SIG(sig_num(static_cast<int>(s.writing_mode)))},
      {"direction", "rtl", true, SIG(sig_num(static_cast<int>(s.direction)))},
      {"color-scheme", "dark", true, SIG(sig_num(static_cast<int>(s.color_scheme)))},
      {"scroll-snap-type", "y proximity", false, SIG(TXT(scroll_snap_type))},
      {"scroll-snap-align", "center", false, SIG(TXT(scroll_snap_align))},
      {"scroll-snap-stop", "always", false, SIG(sig_num(s.scroll_snap_stop_always))},
      {"overscroll-behavior-x", "contain", false, SIG(sig_num(static_cast<int>(s.overscroll_x)))},
      {"overscroll-behavior-y", "none", false, SIG(sig_num(static_cast<int>(s.overscroll_y)))},
      {"scroll-behavior", "smooth", false, SIG(sig_num(s.scroll_smooth))},
      {"container-type", "size", false, SIG(sig_num(s.container_type))},
      {"container-name", "side", false, SIG(TXT(container_name))},
      {"visibility", "hidden", true, SIG(sig_num(static_cast<int>(s.visibility)) + (s.hidden ? "h" : ""))},
  };
  for (const PropertyRow& row : rows) {
    std::string html = std::string("<style>#p{") + row.name + ":" + row.value + "} #i{" + row.name + ":inherit} #n{" + row.name + ":initial} #u{" + row.name + ":unset}</style>" +
                       "<div id=p><div id=i>i</div><div id=n>n</div><div id=u>u</div><div id=plain>x</div></div><div id=base>b</div>";
    Page page(html.c_str());
    const std::string parent = row.signature(page.sheet, page.style("p")), initial = row.signature(page.sheet, page.style("base"));
    const std::string inherited = row.signature(page.sheet, page.style("i")), reset = row.signature(page.sheet, page.style("n")), unset = row.signature(page.sheet, page.style("u")),
                      plain = row.signature(page.sheet, page.style("plain"));
    const bool ok = parent != initial && inherited == parent && reset == initial && unset == (row.inherited ? parent : initial) && plain == (row.inherited ? parent : initial);
    if (!ok) printf("property %s: parent=%s initial=%s inherit=%s initial=%s unset=%s plain=%s\n", row.name, parent.c_str(), initial.c_str(), inherited.c_str(), reset.c_str(), unset.c_str(), plain.c_str());
    CHECK(ok);
  }
  // the shorthands reach their longhands, also with inherit
  Page p("<style>#p{border-radius:3px;outline:1px solid red;column-rule:2px solid blue;transition:opacity 1s;animation:a 1s;overscroll-behavior:contain;font-variant:small-caps tabular-nums;container:n / size;background:url(a.png) red}"
         "#c{border-radius:inherit;outline:inherit;column-rule:inherit;transition:inherit;animation:inherit;overscroll-behavior:inherit;font-variant:inherit;container:inherit;background:inherit}"
         "#d{border-radius:initial;outline:initial;column-rule:initial;transition:initial;animation:initial;overscroll-behavior:initial;font-variant:initial;container:initial;background:initial}</style>"
         "<div id=p><div id=c>c</div><div id=d>d</div></div>");
  Style c = p.style("c"), d = p.style("d");
  CHECK(c.radius[2].h.x100 == 300 && c.outline_width_px100 == 100 && c.outline_color == 0xFFFF0000 && c.column_rule_color == 0xFF0000FF && !c.transition_property.empty() && !c.animation_name.empty() &&
        c.overscroll_x == OverscrollBehavior::Contain && c.font_variant == (kVariantSmallCaps | kVariantTabularNums) && c.container_type == 2 && !c.background_image.empty());
  CHECK(d.radius[2].h.x100 == 0 && d.outline_style == BorderStyle::None && d.column_rule_style == BorderStyle::None && d.transition_property.empty() && d.animation_name.empty() &&
        d.overscroll_x == OverscrollBehavior::Auto && d.font_variant == 0 && d.container_type == 0 && d.background_image.empty());
}

static void test_imports() {
  Page p("<style>@charset \"utf-8\"; @import url(\"a.css\"); @import 'b.css' layer(theme) supports(display:grid) screen and (min-width: 100px); @import url(c.css) layer; @import url(d.css) (max-width: 10px);"
         "@import url(e.css) supports(wat:1); @layer x; @import url(f.css); #a{color:red} @import url(late.css); @import; @import 5;</style><div id=a>a</div>");
  CHECK(p.sheet.import_count() == 6);
  ImportRule r;
  CHECK(p.sheet.import_at(0, &r) && r.url.equals("a.css") && r.media.empty() && !r.has_layer && r.applies);
  CHECK(p.sheet.import_at(1, &r) && r.url.equals("b.css") && r.has_layer && r.layer.equals("theme") && r.supports.equals("display:grid") && r.media.equals("screen and (min-width: 100px)") && r.applies);
  CHECK(p.sheet.import_at(2, &r) && r.url.equals("c.css") && r.has_layer && r.layer.empty());
  CHECK(p.sheet.import_at(3, &r) && r.url.equals("d.css") && r.media.equals("(max-width: 10px)") && !r.applies);
  CHECK(p.sheet.import_at(4, &r) && r.url.equals("e.css") && r.supports.equals("wat:1") && !r.applies);
  CHECK(p.sheet.import_at(5, &r) && r.url.equals("f.css"));
  CHECK(!p.sheet.import_at(6, &r));
  CHECK(p.style("a").color == 0xFFFF0000);
  Page two("<style>@import url(one.css);</style><style>@import url(two.css); p{color:red} @import url(three.css);</style>");   // (every sheet may begin with imports)
  CHECK(two.sheet.import_count() == 2 && two.sheet.import_at(1, &r) && r.url.equals("two.css"));
}

static void test_font_faces() {
  Page p("<style>@font-face { font-family: \"My Font\"; src: url(\"a.woff2\") format(\"woff2\") tech(variations), url(b.woff) format('woff'), local(\"Arial\"), url(c.ttf) tech(color-COLRv1); font-weight: 400 700;"
         "font-style: oblique 10deg 20deg; font-stretch: 75% 125%; unicode-range: U+0025-00FF, U+4??, U+26; font-display: swap }"
         "@font-face { font-family: x } @font-face { src: url(y.woff) } @font-face { font-family: Plain; src: url(p.otf) format(opentype); font-weight: bold; font-style: italic; font-stretch: condensed }"
         "@font-face { font-family: Bad; src: url(q.woff); unicode-range: U+ZZ; font-weight: 9000; font-display: wat } @media (min-width: 100px) { @font-face { font-family: Wide; src: url(w.eot) format(\"embedded-opentype\") } }"
         "@media (max-width: 10px) { @font-face { font-family: Narrow; src: url(n.woff) } }</style><div>x</div>");
  CHECK(p.sheet.font_face_count() == 4);
  FontFace f;
  CHECK(p.sheet.font_face(0, &f) && f.family.equals("My Font") && f.source_count == 4);
  CHECK(!f.sources[0].local && f.sources[0].value.equals("a.woff2") && f.sources[0].format == FontFormat::Woff2 && f.sources[0].format_text.equals("woff2") && f.sources[0].supported_tech);
  CHECK(f.sources[1].format == FontFormat::Woff && f.sources[2].local && f.sources[2].value.equals("Arial") && f.sources[2].format == FontFormat::None && f.sources[3].format == FontFormat::None && !f.sources[3].supported_tech);
  CHECK(f.weight_min == 400 && f.weight_max == 700 && f.style == 2 && f.oblique_min_x100 == 1000 && f.oblique_max_x100 == 2000 && f.stretch_min_x10 == 750 && f.stretch_max_x10 == 1250 && f.display == FontDisplay::Swap);
  CHECK(f.unicode_ranges.size() == 3 && f.unicode_ranges[0].first == 0x25 && f.unicode_ranges[0].last == 0xFF && f.unicode_ranges[1].first == 0x400 && f.unicode_ranges[1].last == 0x4FF &&
        f.unicode_ranges[2].first == 0x26 && f.unicode_ranges[2].last == 0x26);
  CHECK(f.covers(0x30) && f.covers(0x410) && !f.covers(0x500) && !f.covers(0x20));
  CHECK(p.sheet.font_face(1, &f) && f.family.equals("Plain") && f.sources[0].format == FontFormat::OpenType && f.weight_min == 700 && f.weight_max == 700 && f.style == 1 && f.stretch_min_x10 == 750 && f.unicode_ranges.size() == 0 && f.covers(0x1F600));
  CHECK(p.sheet.font_face(2, &f) && f.family.equals("Bad") && f.weight_min == 400 && f.display == FontDisplay::Auto && f.unicode_ranges.size() == 0);   // (bad descriptors are ignored)
  CHECK(p.sheet.font_face(3, &f) && f.family.equals("Wide") && f.sources[0].format == FontFormat::EmbeddedOpenType);
  CHECK(!p.sheet.font_face(4, &f));
}

static void test_property_rules() {
  Page p("<style>@property --gap { syntax: '<length>'; inherits: false; initial-value: 7px } @property --tone { syntax: '*'; inherits: true } @property --bad { syntax: '<length>' } @property nope { syntax: '*'; inherits: true }"
         "@property --keep { syntax: '<length>'; inherits: true; initial-value: 1px }"
         "#a{--gap:3px;--keep:9px;margin-left:var(--gap);padding-left:var(--keep)} #a2{margin-left:var(--gap);padding-left:var(--keep)} #b{margin-left:var(--gap)} #c{width:var(--bad, 5px)} #d{width:var(--keep)}</style>"
         "<div id=a><div id=a2>x</div></div><div id=b>b</div><div id=c>c</div><div id=d>d</div>");
  CHECK(p.sheet.property_registration_count() == 3);
  PropertyRegistration r;
  CHECK(p.sheet.property_registration(0, &r) && r.name.equals("--gap") && r.syntax.equals("<length>") && !r.inherits && r.has_initial && r.initial_value.equals("7px"));
  CHECK(p.sheet.property_registration(1, &r) && r.name.equals("--tone") && r.syntax.equals("*") && r.inherits && !r.has_initial);
  CHECK(px(p.style("a").margin[3]) == 3 && px(p.style("a").padding[3]) == 9);
  CHECK(px(p.style("a2").margin[3]) == 7 && px(p.style("a2").padding[3]) == 9);   // (--gap does not inherit: its initial value; --keep does)
  CHECK(px(p.style("b").margin[3]) == 7);
  CHECK(px(p.style("c").width) == 5 && px(p.style("d").width) == 1);
}

struct FixedContainers final : IContainerSizes {
  bool container_size(const dom::Node* element, StringView name, int32_t* width, int32_t* height) override {
    ++calls;
    if (name.equals("none-such")) return false;
    *width = name.equals("sidebar") ? 300 : 800;
    *height = 200;
    return true;
  }
  int calls = 0;
};

static void test_containers_and_scopes() {
  const char* html = "<style>@container (min-width: 400px) { #t{color:red} } @container sidebar (max-width: 350px) { #t{margin-left:7px} } @container (min-width: 400px) and (min-height: 300px) { #t{padding-left:5px} }"
                     "@container none-such (min-width: 1px) { #t{width:9px} } @container style(--x: 1) { #t{height:9px} } @container { #t{height:3px} } @container sidebar (min-width: 1px) { @container (max-height: 250px) { #t{padding-top:2px} } }"
                     "@starting-style { #t { color: blue; opacity: 0 } }</style><div id=t>t</div>";
  Page p(html);   // (no container sizes: the viewport is the container: 640x480)
  Style t = p.style("t");
  CHECK(t.color == 0xFFFF0000 && px(t.margin[3]) == 0 && px(t.padding[3]) == 5 && px(t.padding[0]) == 0 && t.opacity_x1000 == 1000);
  CHECK(px(t.width) == 9 && t.height.is_auto());   // (a named container is the viewport too; the viewport is 480 high: not at most 250; style() queries and empty ones do not apply)
  FixedContainers containers;
  Page q(html);
  q.sheet.set_container_sizes(&containers);
  Style u = q.style("t");
  CHECK(containers.calls > 0);
  CHECK(u.color == 0xFFFF0000 && px(u.margin[3]) == 7 && px(u.padding[3]) == 0 && px(u.padding[0]) == 2 && u.width.is_auto());   // (sidebar is 300 wide, the other container 800x200)
  q.sheet.set_container_sizes(nullptr);
  Page s("<style>@scope (.card) to (.content) { img { width: 10px } :scope { color: red } .title { height: 5px } & > a { width: 3px } }"
         "@scope (.x, .y) { p { margin-left: 4px } } @scope { b { color: blue } } @scope (.a) { @scope (.b) { i { height: 6px } } }</style>"
         "<div class=card id=card><img id=i1><span class=title id=t1>t</span><a id=a1>a</a><div class=content><img id=i2><span class=title id=t2>t</span></div></div><img id=i3><div class=y><p id=p1>x</p></div><p id=p2>y</p>"
         "<b id=bb>b</b><div class=a><div class=b><i id=i4>i</i></div><i id=i5>i</i></div>");
  CHECK(px(s.style("i1").width) == 10 && s.style("i2").width.is_auto() && s.style("i3").width.is_auto());
  CHECK(s.style("card").color == 0xFFFF0000 && px(s.style("t1").height) == 5 && s.style("t2").height.is_auto());
  CHECK(px(s.style("a1").width) == 3);
  CHECK(px(s.style("p1").margin[3]) == 4 && px(s.style("p2").margin[3]) == 0);
  CHECK(s.style("bb").color == 0xFF0000FF);   // (a scope without a root applies everywhere)
  CHECK(px(s.style("i4").height) == 6 && s.style("i5").height.is_auto());
}

static void test_new_garbage() {
  const char* sheets[] = {
      "a{font-family:}", "a{font-family:,}", "a{font-family:'x}", "a{font-weight:}", "a{font:italic}", "a{font:bold 12px}", "a{border-radius:/}", "a{border-radius:1px/}", "a{border-radius:/1px}",
      "a{box-shadow:,}", "a{box-shadow:inset}", "a{background:url(}", "a{background:linear-gradient(}", "a{background:linear-gradient(,)}", "a{background:/ /}", "a{background:center / / cover}",
      "a{background-image:conic-gradient(from)}", "a{background-image:radial-gradient(circle at)}", "a{transform:translate(}", "a{transform:rotate(1deg) )}", "a{transform:matrix(1,2)}",
      "a{transform-origin:left left}", "a{transform-origin:1px 2px 3px 4px}", "a{translate:}", "a{rotate:x}", "a{scale:}", "a{outline:solid solid}", "a{cursor:}", "a{cursor:url(}",
      "a{aspect-ratio:/}", "a{aspect-ratio:1/}", "a{aspect-ratio:1/2/3}", "a{object-position:left left left left left}", "a{transition:,}", "a{transition:1s,2s,}", "a{animation:,}",
      "a{animation:x 1s 2s 3s}", "a{filter:blur(}", "a{filter:drop-shadow()}", "a{clip-path:polygon(}", "a{clip-path:inset(round)}", "a{clip-path:path()}", "a{clip-path:circle(at)}",
      "a{color:color-mix(}", "a{color:oklch(}", "a{color:light-dark(,)}", "a{color:color(srgb)}", "a{width:calc(round())}", "a{width:calc(mod(1px))}", "a{width:calc(atan2(1))}", "a{width:calc(hypot())}",
      "a{width:env(}", "a{width:env()}", "a{width:attr(}", "a{width:attr()}", "a{width:calc(pow(1))}", "a{width:calc(log(1,2,3))}", "a{--x:attr(a);width:var(--x)}",
      "@layer a.{} @layer .a{} @layer ..{}", "@layer a,,b;", "@layer a { @layer b { @layer c {", "@import", "@import url(", "@import url(a.css", "@import \"a.css\" layer(", "@import url(a) supports(",
      "@font-face{", "@font-face{src:url(", "@font-face{font-family:'a';src:url(a) format(}", "@font-face{font-family:a;src:url(a);unicode-range:U+", "@font-face{font-family:a;src:url(a);unicode-range:U+1-}",
      "@font-face{font-family:a;src:local(", "@keyframes a{", "@keyframes a{from{", "@keyframes a{50%{opacity:1}", "@keyframes a{%{}}", "@keyframes a{,{}}", "@property --a{", "@property --a{syntax:'",
      "@container", "@container (", "@container a b c (min-width:1px){", "@scope (", "@scope (a) to (", "@scope (a) to {", "@starting-style{", "@media{@layer{@scope(a){@container(width>1px){a{color:red}",
      "a{color-scheme:}", "a{color-scheme:light light light}", "a{container:/}", "a{container-type:wat}", "a{writing-mode:}", "a{scroll-snap-type:}", "a{overscroll-behavior:}",
      "a{background-position:calc(}", "a{background-size:1px 2px 3px}", "a{background-repeat:repeat repeat repeat}", "a{background-clip:}",
  };
  for (const char* css : sheets) {
    std::string html = std::string("<style>") + css + "</style><a id=a data-x=y>x</a>";
    Page page(html.c_str());
    Style s = page.style("a");
    List<BackgroundLayer> layers(g_alloc);
    page.sheet.background_layers(s, &layers);
    List<TransformOp> ops(g_alloc);
    page.sheet.transform_ops(s, &ops);
    List<Shadow> shadows(g_alloc);
    page.sheet.shadows(s.box_shadow, s, &shadows);
    List<FontFamily> families(g_alloc);
    page.sheet.font_families(s, &families);
    List<Transition> transitions(g_alloc);
    page.sheet.transitions(s, &transitions);
    List<Animation> animations(g_alloc);
    page.sheet.animations(s, &animations);
    List<FilterFunction> filters(g_alloc);
    page.sheet.filters(s.filter, s, &filters);
    ClipPath clip;
    page.sheet.clip_path(s, &clip);
    double m[16];
    page.sheet.transform_matrix(s, 10, 10, m);
    FontFace face;
    for (uint32_t i = 0; i < page.sheet.font_face_count(); ++i) page.sheet.font_face(i, &face);
    ImportRule import_rule;
    for (uint32_t i = 0; i < page.sheet.import_count(); ++i) page.sheet.import_at(i, &import_rule);
    for (uint32_t i = 0; i < page.sheet.keyframes_count(); ++i) {
      Style base, frame = base.inherit();
      for (uint32_t k = 0; k < page.sheet.keyframe_count(i); ++k) page.sheet.apply_keyframe(i, k, base, &frame);
    }
  }
  // truncations and bit flips of a sheet using the new features
  const std::string real = "@import url(a.css) layer(x) supports(display:grid);@layer a,b;@font-face{font-family:'F';src:url(f.woff2) format('woff2'),local(F);unicode-range:U+0-FF,U+4??;font-weight:100 900}"
                           "@keyframes k{from{opacity:0}to{opacity:1;transform:rotate(1turn)}}@property --p{syntax:'<length>';inherits:false;initial-value:1px}"
                           "@layer a{@container (min-width:1px){@scope (div) to (p){p{box-shadow:inset 1px 2px 3px 4px color-mix(in oklch,red,blue);background:url(x) 1px 2px/cover no-repeat,linear-gradient(45deg,red,blue 50%);"
                           "transform:translate(1px,2%) rotate(1deg) scale(2);border-radius:1px 2px/3px;filter:blur(1px) drop-shadow(1px 1px red);clip-path:polygon(0 0,1px 1px,2px 0);"
                           "transition:opacity 1s ease-in .5s;animation:k 1s steps(2) infinite;width:calc(round(1px,2px) + mod(5px,3px) + env(safe-area-inset-top,1px) + attr(data-x px,2px));"
                           "font:italic 700 expanded 12px/1.5 'F',serif;color:light-dark(oklab(.5 .1 .1),lab(1 2 3));outline:2px solid;aspect-ratio:16/9;cursor:pointer}}}}";
  for (size_t n = 0; n <= real.size(); ++n) {
    std::string html = "<style>" + real.substr(0, n) + "</style><div><p id=a data-x=3>x</p></div>";
    Page page(html.c_str());
    Style s = page.style("a");
    List<BackgroundLayer> layers(g_alloc);
    page.sheet.background_layers(s, &layers);
    List<TransformOp> ops(g_alloc);
    page.sheet.transform_ops(s, &ops);
    List<Shadow> shadows(g_alloc);
    page.sheet.shadows(s.box_shadow, s, &shadows);
    List<Animation> animations(g_alloc);
    page.sheet.animations(s, &animations);
    FontFace face;
    for (uint32_t i = 0; i < page.sheet.font_face_count(); ++i) page.sheet.font_face(i, &face);
  }
  for (size_t i = 0; i < real.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string css = real;
      css[i] = static_cast<char>(css[i] ^ (1 << bit));
      std::string html = "<style>" + css + "</style><div><p id=a data-x=3>x</p></div>";
      Page page(html.c_str());
      Style s = page.style("a");
      List<BackgroundLayer> layers(g_alloc);
      page.sheet.background_layers(s, &layers);
      List<TransformOp> ops(g_alloc);
      page.sheet.transform_ops(s, &ops);
      List<Shadow> shadows(g_alloc);
      page.sheet.shadows(s.box_shadow, s, &shadows);
      List<Animation> animations(g_alloc);
      page.sheet.animations(s, &animations);
      FontFace face;
      for (uint32_t i2 = 0; i2 < page.sheet.font_face_count(); ++i2) page.sheet.font_face(i2, &face);
    }
  }
}

// Percentages stay resolvable at layout time, while the old fields are what they always were.
static void test_percent_lengths() {
  Page p("<style>#a{width:50%;height:25%;margin-left:10%;padding-left:5%;min-width:calc(100% - 20px);max-height:50%;top:30%;left:calc(50% + 10px);flex-basis:20%;gap:10%;text-indent:5%;"
         "margin-top:10%;min-height:10%;max-width:60%;bottom:calc(100% - 4px);right:12px} "
         "#b{width:min(50%, 400px);height:clamp(10px, 50%, 300px);margin-left:max(10%, 20px)} #c{width:100px;height:calc(2em + 4px);margin-left:calc(50% - 50%)} "
         "#d{width:calc(50% +);height:wat%} #e{width:calc(100% / 4 + 2px)}</style><div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div>");
  Style a = p.style("a");
  // the old view: pixels against the 640 pixel page, and heights and insets on top and bottom automatic
  CHECK(px(a.width) == 320 && a.height.is_auto() && px(a.margin[3]) == 64 && px(a.padding[3]) == 32 && px(a.min_width) == 620 && a.max_height.is_none() && a.inset[0].is_auto() && px(a.inset[3]) == 330);
  CHECK(px(a.flex_basis) == 128 && px(a.row_gap) == 64 && a.text_indent_px100 == 3200 && px(a.margin[0]) == 64 && px(a.max_width) == 384 && a.min_height.is_auto());
  // the new view
  CHECK(a.width.has_percent() && a.width.percent100 == 5000 && a.width.abs100 == 0 && a.width.resolve(20000) == 10000);
  CHECK(a.height.percent100 == 2500 && a.height.resolve(40000) == 10000 && a.height.kind == Length::Auto);
  CHECK(a.margin[3].percent100 == 1000 && a.padding[3].percent100 == 500 && a.margin[0].percent100 == 1000);
  CHECK(a.min_width.percent100 == 10000 && a.min_width.abs100 == -2000 && a.min_width.resolve(100000) == 98000);
  CHECK(a.max_height.percent100 == 5000 && a.max_height.kind == Length::None && a.min_height.percent100 == 1000 && a.max_width.percent100 == 6000);
  CHECK(a.inset[0].percent100 == 3000 && a.inset[0].kind == Length::Auto && a.inset[3].percent100 == 5000 && a.inset[3].abs100 == 1000 && a.inset[2].percent100 == 10000 && a.inset[2].abs100 == -400);
  CHECK(a.flex_basis.percent100 == 2000 && a.row_gap.percent100 == 1000 && a.column_gap.percent100 == 1000 && a.text_indent.percent100 == 500);
  CHECK(!a.inset[1].has_percent() && a.inset[1].resolve(99999) == 1200 && px(a.inset[1]) == 12);
  // calc, min, max and clamp that do not reduce to a pixels-and-percentage sum keep their text
  Style b = p.style("b");
  CHECK(px(b.width) == 320 && b.width.expr != 0 && b.width.has_percent() && p.sheet.length_expression(b.width).equals("min(50%, 400px)"));
  int32_t out = 0;
  CHECK(p.sheet.resolve_length_in(b.width, b, 100000, &out) && out == 40000);
  CHECK(p.sheet.resolve_length_in(b.width, b, 50000, &out) && out == 25000);
  CHECK(b.height.kind == Length::Auto && b.height.expr != 0 && p.sheet.resolve_length_in(b.height, b, 200000, &out) && out == 30000 && p.sheet.resolve_length_in(b.height, b, 1000, &out) && out == 1000);
  CHECK(p.sheet.resolve_length_in(b.margin[3], b, 100000, &out) && out == 10000 && p.sheet.resolve_length_in(b.margin[3], b, 400000, &out) && out == 40000);
  // plain lengths are unchanged
  Style c = p.style("c");
  CHECK(!c.width.has_percent() && c.width.resolve(12345) == 10000 && !c.height.has_percent() && px(c.height) == 36 && !c.margin[3].has_percent());
  CHECK(p.sheet.resolve_length_in(c.width, c, 5, &out) && out == 10000);
  Style d = p.style("d");
  CHECK(d.width.is_auto() && !d.width.has_percent() && d.height.is_auto() && !d.height.has_percent());
  Style e = p.style("e");
  CHECK(e.width.percent100 == 2500 && e.width.abs100 == 200 && px(e.width) == 162);
  // inherit and the children
  Page q("<style>#a{width:50%;text-indent:10%} #b{width:inherit}</style><div id=a><div id=b>x</div></div>");
  CHECK(q.style("b").width.percent100 == 5000 && q.style("b").text_indent.percent100 == 1000);
  Page r("<style>#a{width:50%}</style><div id=a><div id=b>x</div></div>");
  CHECK(!r.style("b").width.has_percent());
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_units_and_math();
  test_vars();
  test_shorthands();
  test_keywords_and_cascade();
  test_display_and_boxes();
  test_selectors_and_at_rules();
  test_garbage();
  test_fonts();
  test_border_radius();
  test_shadows();
  test_backgrounds();
  test_opacity_visibility();
  test_transforms();
  test_misc_properties();
  test_transitions_and_animations();
  test_keyframes();
  test_filters_and_clip_path();
  test_math_and_functions();
  test_color_functions();
  test_cascade_layers();
  test_property_matrix();
  test_imports();
  test_font_faces();
  test_property_rules();
  test_containers_and_scopes();
  test_percent_lengths();
  test_new_garbage();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("css tests passed\n");
  return 0;
}
