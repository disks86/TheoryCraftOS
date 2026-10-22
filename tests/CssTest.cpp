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
  Page(const char* html, int width = 640, int height = 480) : doc(g_alloc), sheet(g_alloc) {
    dom::build_document(doc, html, nullptr);
    RenderOptions options;
    options.media.width = width;
    options.media.height = height;
    collect_style_sheets(doc, options, &sheet);
    sheet.set_viewport(width, height);
  }
  Style style(const char* id) {
    const dom::Node* n = doc.element_by_id(id);
    return n ? computed_style(sheet, n) : Style();
  }
};

static int px(const Length& l) { return l.kind == Length::Px ? l.px100 / 100 : -999999; }

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

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_units_and_math();
  test_vars();
  test_shorthands();
  test_keywords_and_cascade();
  test_display_and_boxes();
  test_selectors_and_at_rules();
  test_garbage();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("css tests passed\n");
  return 0;
}
