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

// CSS for a text-mode browser. StyleSheet reads stylesheets (the text of <style> elements and linked files) into rules; the page renderer asks it which
// declarations apply to each element and gets a Style: what the text of the element looks like and the box it makes (sizes, spacing, borders, how its children are
// laid out - flow, flexbox, grid, table - and where). The screen is a grid of character cells: a column is 8 pixels wide and a line is 16 pixels high, and
// lengths are worked out in pixels and then rounded to cells.
//
// Selectors: everything in Dom/Selector.hpp (type, class, id, attribute, structural and logical pseudo-classes, combinators), :hover, :active and :focus-within
// for the states the page tracks, and the pseudo-elements ::before, ::after and ::marker. Rules: @media, @supports, @layer and @container blocks are read as
// if they stood where they are when their condition holds; @import, @font-face, @keyframes and the like are skipped. The cascade orders by importance,
// specificity and source order; the keywords inherit, initial, unset and revert work for every property; custom properties (--name) and var() with fallbacks
// are inherited like any value; lengths take px, em, rem, %, vw, vh, vmin, vmax, ch, ex, pt, pc, in, cm, mm and Q, and calc(), min(), max() and clamp() are worked out.
//
// Understood: colour, background-color / background, font-size, font-weight, font-style, font, line-height, text-align, text-indent, text-transform, text-decoration,
// white-space, word-break, overflow-wrap, text-overflow, vertical-align, list-style, visibility, opacity (0 hides), display (none, block, inline, list-item,
// inline-block, flex, inline-flex, grid, inline-grid, table and its parts, contents, flow-root), position, top / right / bottom / left / inset, z-index, float, clear,
// overflow, width / height with their min and max, box-sizing, margin, padding, border (width, style, colour, per side), the flexbox properties (flex, flex-direction,
// flex-wrap, flex-flow, justify-content, align-items, align-self, align-content, order, gap), the grid properties (grid-template-columns / -rows / -areas, grid-auto-*,
// grid-column / -row / -area, gap, justify-items, justify-self, place-*), content, counter-reset, counter-increment, columns and the table properties.
#pragma once
#include "Browser/Media.hpp"
#include "Dom/Selector.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace browser {

// Colours are 0xAARRGGBB; 0 means "not set". Reads a CSS colour (names, #rgb, #rrggbb, #rgba, #rrggbbaa, rgb(), rgba(), hsl(), hsla(), hwb(), transparent). A
// transparent colour comes back as true with *out == 0. False for anything else (also "inherit" and "currentcolor").
bool parse_css_color(StringView text, uint32_t* out);
// The same with the alpha kept (0xAARRGGBB; "transparent" is 0x00000000), for canvases. False for anything else.
bool parse_css_color_alpha(StringView text, uint32_t* out);

// The value of attribute `name` in the text of a tag (after its tag name), as written (entities are not decoded); false if it has none.
bool find_attribute(StringView attrs, StringView name, StringView* value);

enum class Display : uint8_t { Inline, Block, ListItem, None };
// How an element lays out what is in it (or, for the parts of a table, what part it is).
enum class Layout : uint8_t { Flow, Flex, Grid, Table, TableRowGroup, TableRow, TableCell, TableCaption };
enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
enum class Float : uint8_t { None, Left, Right };
enum class Clear : uint8_t { None, Left, Right, Both };
enum class Overflow : uint8_t { Visible, Hidden, Scroll, Auto };
enum class BoxSizing : uint8_t { ContentBox, BorderBox };
enum class BorderStyle : uint8_t { None, Hidden, Dotted, Dashed, Solid, Double, Groove, Ridge, Inset, Outset };
enum class PseudoElement : uint8_t { None, Before, After, Marker };
// justify-content, align-items and the like.
enum class Place : uint8_t { Normal, Start, End, Center, Stretch, Baseline, SpaceBetween, SpaceAround, SpaceEvenly, FlexStart, FlexEnd, Left, Right, Auto };
enum class FlexDirection : uint8_t { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap : uint8_t { NoWrap, Wrap, WrapReverse };

// A size as computed: a number of pixels (x100), or a keyword.
struct Length {
  enum Kind : uint8_t { Auto, Px, MinContent, MaxContent, FitContent, None };
  int32_t px100 = 0;
  Kind kind = Auto;
  static Length px(int32_t px100) {
    Length l;
    l.kind = Px;
    l.px100 = px100;
    return l;
  }
  static Length of(Kind k) {
    Length l;
    l.kind = k;
    return l;
  }
  bool is_auto() const { return kind == Auto; }
  bool is_px() const { return kind == Px; }
  bool is_none() const { return kind == None; }
  // The size in cells (8 pixels wide, 16 high), rounded; `fallback` for anything that is not a number of pixels.
  int32_t columns(int32_t fallback = 0) const { return kind == Px ? (px100 < 0 ? -((-px100 + 400) / 800) : (px100 + 400) / 800) : fallback; }
  int32_t lines(int32_t fallback = 0) const { return kind == Px ? (px100 < 0 ? -((-px100 + 800) / 1600) : (px100 + 800) / 1600) : fallback; }
};

// A stretch of the style sheet's text (a value that is parsed when the layout needs it: grid templates, content). Resolve it with StyleSheet::text().
struct TextRef {
  uint32_t offset = 0, length = 0;   // in the sheet's own text, or (top bit of offset set) in the text kept while styles were worked out
  bool empty() const { return length == 0; }
};

constexpr uint32_t kMaxCustomScopes = 20000;

// What the text of an element looks like and the box it makes. Children start from their parent's style (see inherit()).
struct Style {
  // --- inherited
  uint32_t color = 0;         // text colour (inherited)
  uint32_t background = 0;    // the background that text is shown on (inherited: text on a block's background is on it too)
  bool background_block = false;   // that background belongs to a block, so it fills the lines the text is on
  bool bold = false, underline = false, strike = false, code = false, pre = false, italic = false;   // (inherited)
  bool hidden = false;        // not shown: display none here or above, or visibility hidden (inherited)
  bool in_list = false;       // inside a list (inherited)
  bool list_none = false;     // list-style: none (inherited)
  uint8_t transform = 0;      // 1 uppercase, 2 lowercase, 3 capitalize (inherited)
  uint8_t align = 0;          // 0 left, 1 center, 2 right (inherited)
  bool justify = false;       // text-align: justify (inherited)
  uint8_t heading = 0;        // 1..6 inside a heading (inherited)
  uint8_t indent = 0;         // column the element's lines start at (inherited, and added to)
  uint8_t white_space = 0;    // 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line, 5 break-spaces (inherited; `pre` follows it)
  bool word_break = false;    // word-break: break-all / overflow-wrap: anywhere (inherited)
  uint8_t list_type = 0;      // 0 disc, 1 circle, 2 square, 3 decimal, 4 lower-alpha, 5 upper-alpha, 6 lower-roman, 7 upper-roman, 8 none, 9 decimal-leading-zero (inherited)
  bool list_inside = false;   // list-style-position: inside
  uint16_t font_px = 16;      // font-size in pixels (inherited; em and % are worked out against it)
  int32_t line_height_px100 = -1;   // -1: normal (inherited)
  int32_t text_indent_px100 = 0;    // (inherited)
  int32_t content_px = 640;   // the width of the box the element is in, in pixels: what a percentage means (set by the renderer)
  uint32_t custom_scope = 0;  // the custom properties that apply (an index into the sheet's scopes; 0: none) (inherited)

  // --- not inherited
  Display display = Display::Inline;
  Layout layout = Layout::Flow;
  bool inline_box = false;    // inline-block, inline-flex, inline-grid, inline-table: a box that sits in a line
  bool contents = false;      // display: contents (the element has no box of its own)
  uint8_t margin_top = 0, margin_bottom = 0, padding_top = 0, padding_bottom = 0;   // blank lines (what the flow layout uses)
  uint8_t margin_left = 0, padding_left = 0;                                        // columns (what the flow layout uses)
  uint16_t width_px = 0, height_px = 0;        // width / height in pixels (0: auto), for pictures, videos and canvases
  uint8_t width_pct = 0;                        // width as a percentage of the line (0: not a percentage)
  bool own_background = false;                 // this element set the background
  uint32_t own_background_color = 0;           // what it set (for the page's background on <body>)
  bool opaque_zero = false;                    // opacity: 0 (nothing is seen, the space is kept)

  // the box
  Length width, height, min_width, min_height, max_width, max_height;
  Length margin[4];           // top, right, bottom, left (auto or pixels)
  Length padding[4];
  int32_t border_px100[4] = {0, 0, 0, 0};
  BorderStyle border_style[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};
  uint32_t border_color[4] = {0, 0, 0, 0};     // 0: the text colour
  BoxSizing sizing = BoxSizing::ContentBox;
  Position position = Position::Static;
  Float float_side = Float::None;
  Clear clear = Clear::None;
  Overflow overflow_x = Overflow::Visible, overflow_y = Overflow::Visible;
  bool text_ellipsis = false;
  uint8_t vertical_align = 0;                  // 0 baseline, 1 top, 2 middle, 3 bottom
  int32_t z_index = 0;
  Length inset[4];            // top, right, bottom, left
  // flex and grid
  FlexDirection flex_direction = FlexDirection::Row;
  FlexWrap flex_wrap = FlexWrap::NoWrap;
  Place justify_content = Place::Normal, align_items = Place::Normal, align_content = Place::Normal, justify_items = Place::Normal;
  Place align_self = Place::Auto, justify_self = Place::Auto;
  int32_t order = 0;
  int32_t flex_grow_x100 = 0, flex_shrink_x100 = 100;
  Length flex_basis;                           // auto
  Length row_gap, column_gap;                  // normal -> auto
  bool grid_dense = false, grid_column_flow = false;
  TextRef grid_template_columns, grid_template_rows, grid_template_areas, grid_auto_columns, grid_auto_rows;
  TextRef grid_column_start, grid_column_end, grid_row_start, grid_row_end;
  TextRef content;                             // the `content` of a pseudo-element (or "none"/"normal" when empty)
  bool has_content = false;                    // content was given (and is not none / normal)
  TextRef counter_reset, counter_increment;
  uint16_t column_count = 0;                   // columns: 0 is auto
  Length column_width;
  // tables
  bool border_collapse = false;
  Length border_spacing;
  bool table_fixed = false;
  bool caption_bottom = false;

  // The style a child starts with.
  Style inherit() const {
    Style s = *this;
    s.display = Display::Inline;
    s.layout = Layout::Flow;
    s.inline_box = false;
    s.contents = false;
    s.margin_top = s.margin_bottom = s.padding_top = s.padding_bottom = s.margin_left = s.padding_left = 0;
    s.width_px = s.height_px = 0;
    s.width_pct = 0;
    s.own_background = false;
    s.own_background_color = 0;
    s.opaque_zero = false;
    s.width = s.height = s.min_width = s.min_height = Length();
    s.max_width = s.max_height = Length::of(Length::None);
    for (int i = 0; i < 4; ++i) {
      s.margin[i] = Length::px(0);
      s.padding[i] = Length::px(0);
      s.border_px100[i] = 0;
      s.border_style[i] = BorderStyle::None;
      s.border_color[i] = 0;
      s.inset[i] = Length();
    }
    s.sizing = BoxSizing::ContentBox;
    s.position = Position::Static;
    s.float_side = Float::None;
    s.clear = Clear::None;
    s.overflow_x = s.overflow_y = Overflow::Visible;
    s.text_ellipsis = false;
    s.vertical_align = 0;
    s.z_index = 0;
    s.flex_direction = FlexDirection::Row;
    s.flex_wrap = FlexWrap::NoWrap;
    s.justify_content = s.align_items = s.align_content = s.justify_items = Place::Normal;
    s.align_self = s.justify_self = Place::Auto;
    s.order = 0;
    s.flex_grow_x100 = 0;
    s.flex_shrink_x100 = 100;
    s.flex_basis = Length();
    s.row_gap = s.column_gap = Length();
    s.grid_dense = s.grid_column_flow = false;
    s.grid_template_columns = s.grid_template_rows = s.grid_template_areas = s.grid_auto_columns = s.grid_auto_rows = TextRef();
    s.grid_column_start = s.grid_column_end = s.grid_row_start = s.grid_row_end = TextRef();
    s.content = TextRef();
    s.has_content = false;
    s.counter_reset = s.counter_increment = TextRef();
    s.column_count = 0;
    s.column_width = Length();
    s.border_collapse = false;
    s.border_spacing = Length();
    s.table_fixed = false;
    s.caption_bottom = false;
    return s;
  }

  Style() {
    margin[0] = margin[1] = margin[2] = margin[3] = Length::px(0);
    padding[0] = padding[1] = padding[2] = padding[3] = Length::px(0);
    max_width = max_height = Length::of(Length::None);
  }
};

class StyleSheet {
 public:
  static constexpr uint32_t kMaxRules = 10000;

  explicit StyleSheet(allocators::IAllocator& allocator)
      : allocator_(allocator), pool_(allocator), selectors_(allocator), declarations_(allocator), rules_(allocator), scopes_(allocator), kept_(allocator) {}
  StyleSheet(const StyleSheet&) = delete;
  StyleSheet& operator=(const StyleSheet&) = delete;

  // Adds the rules of a stylesheet (later ones win over earlier ones of the same specificity); @media rules apply when `env` satisfies their query. False
  // if memory ran out.
  bool parse(StringView css, const MediaEnvironment& env = MediaEnvironment());
  // The rules read from now on belong to a shadow tree (the serial of its root; 0: the document): they apply to what is in that tree, and to its host through :host.
  void set_scope(uint32_t shadow_root_serial) { scope_ = shadow_root_serial; }
  void clear();
  uint32_t rule_count() const { return static_cast<uint32_t>(rules_.count()); }
  // Does a rule depend on the pointer (:hover, :active, :focus-within)? Then the page is drawn again when the pointer moves on to another element.
  bool uses_pointer_state() const { return pointer_state_; }

  // Applies what the sheet says about the element to `style` (which starts as the parent's inherited style with the defaults of the element's kind), then
  // `inline_style` (the style attribute) on top. `parent` is the parent's style: what the keyword `inherit` copies and the font size that em means. With a
  // pseudo-element the rules of that pseudo-element of the element are applied. Properties that are not understood are skipped.
  void apply(const dom::Node* element, StringView inline_style, const Style& parent, Style* style, PseudoElement pseudo = PseudoElement::None) const;
  // Does some rule give the element this pseudo-element?
  bool has_pseudo(const dom::Node* element, PseudoElement pseudo) const;
  // The text of a TextRef (valid for as long as the sheet is), and a place to keep text so that a Style can point to it.
  StringView text(TextRef ref) const;
  TextRef keep_text(StringView s) const;
  // The size of the display, for vw / vh.
  void set_viewport(int32_t width_px, int32_t height_px) {
    viewport_w_ = width_px;
    viewport_h_ = height_px;
  }
  // Works out a CSS length expression ("2em", "calc(100% - 10px)", "min(50vw, 400px)") for the style of an element: false if it is not one. `percent_px` is what 100% is.
  bool resolve_length(StringView expression, const Style& style, int32_t percent_px, Length* out) const;

 private:
  struct Declaration {
    uint16_t property;
    bool important;
    uint32_t value_offset, value_length;
    uint32_t name_offset, name_length;   // custom properties (--name)
  };
  struct Rule {
    uint32_t first_declaration, declaration_count, order;
    uint32_t scope = 0;   // 0: a rule of the document; otherwise the serial of the shadow root whose <style> it came from
  };
  struct Scope {
    uint32_t parent;           // the scope this one adds to
    uint32_t name_offset, name_length, value_offset, value_length;
  };

  bool parse_block(StringView css, uint64_t* at, bool nested);
  bool parse_rule(StringView prelude, StringView body);
  bool add_declarations(StringView body, uint32_t rule);
  uint32_t add_scope(const Scope& scope, uint32_t fallback) const;
  bool keep(StringView s, uint32_t* offset) {
    *offset = static_cast<uint32_t>(pool_.size());
    return pool_.append(s);
  }
  friend struct CssApply;

  allocators::IAllocator& allocator_;
  bool pointer_state_ = false;
  uint32_t scope_ = 0;
  bool rule_applies(uint32_t selector, uint32_t element_scope, uint32_t host_scope) const;
  String pool_;
  dom::SelectorSet selectors_;
  const MediaEnvironment* env_ = nullptr;
  List<Declaration> declarations_;
  List<Rule> rules_;
  mutable List<Scope> scopes_;     // the custom property scopes made while styles were worked out
  mutable List<String> kept_;      // text that styles point to (see keep_text); each lives in a place of its own, so views of it stay valid
  int32_t viewport_w_ = 640, viewport_h_ = 480;
};

}  // namespace browser
