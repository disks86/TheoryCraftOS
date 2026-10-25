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
// for the states the page tracks, and the pseudo-elements ::before, ::after and ::marker. Rules: @media and @supports blocks are read as if they stood where they
// are when their condition holds. @layer blocks give their rules a cascade layer (later layers win over earlier ones, unlayered rules over all layers; the order of
// !important is reversed), @scope blocks scope their selectors to the scoping root (and stop at its limit), @container rules apply when the size of the container
// (the viewport, or what an IContainerSizes says) satisfies the query. @import rules are listed (import_at()) for the page to fetch, @font-face rules are captured
// as FontFace descriptors, @keyframes as Keyframes, @property as registrations; @starting-style is skipped. The cascade orders by importance, layer,
// specificity and source order; the keywords inherit, initial, unset and revert work for every property; custom properties (--name) and var() with fallbacks
// are inherited like any value (registered non-inheriting ones are not); env() and attr() are replaced like var(); lengths take px, em, rem (the size of the root
// element), %, vw, vh, vmin, vmax, ch, ex, pt, pc, in, cm, mm and Q, and calc(), min(), max(), clamp(), round(), mod(), rem(), abs(), sign(), sin(), cos(),
// tan(), asin(), acos(), atan(), atan2(), pow(), sqrt(), hypot(), log() and exp() are worked out. Colours also take lab(), lch(), oklab(), oklch(), color(),
// color-mix() and light-dark().
//
// The pixel engine's properties: values that are one number or keyword are stored in Style fields (font_weight, letter_spacing_px100, opacity_x1000, radius[],
// cursor ...); values that are lists or functions (font-family, box-shadow, background layers, transform, transition, animation, filter, clip-path) are kept as
// text in a TextRef (with var() worked out) and read into structures by the readers of StyleSheet (font_families(), shadows(), background_layers(),
// transform_ops(), transitions(), animations(), filters(), clip_path()), the way grid templates are read when the layout needs them.
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

// Does the declaration make sense (a known property and a value it takes)? Properties whose values are not checked here accept any value.
bool css_declaration_valid(StringView name, StringView value);
// Does an @supports condition hold, "(display: grid) and (color: red)"?
bool css_supports(StringView text);

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
enum class Visibility : uint8_t { Visible, Hidden, Collapse };   // (hidden and collapse keep their space in the layout)
enum class Cursor : uint8_t {
  Auto, Default, None, ContextMenu, Help, Pointer, Progress, Wait, Cell, Crosshair, Text, VerticalText, Alias, Copy, Move, NoDrop, NotAllowed, Grab, Grabbing, AllScroll,
  ColResize, RowResize, NResize, EResize, SResize, WResize, NeResize, NwResize, SeResize, SwResize, EwResize, NsResize, NeswResize, NwseResize, ZoomIn, ZoomOut
};
enum class PointerEvents : uint8_t { Auto, None };
enum class UserSelect : uint8_t { Auto, None, Text, All, Contain };
enum class ObjectFit : uint8_t { Fill, Contain, Cover, None, ScaleDown };
enum class WritingMode : uint8_t { HorizontalTb, VerticalRl, VerticalLr, SidewaysRl, SidewaysLr };
enum class Direction : uint8_t { Ltr, Rtl };
enum class ColorScheme : uint8_t { Normal, Light, Dark, LightDark };
enum class OverscrollBehavior : uint8_t { Auto, Contain, None };

// font-variant (caps, numeric and ligature keywords): the bits of Style::font_variant.
constexpr uint16_t kVariantSmallCaps = 1, kVariantAllSmallCaps = 2, kVariantPetiteCaps = 4, kVariantAllPetiteCaps = 8, kVariantUnicase = 16, kVariantTitlingCaps = 32,
                   kVariantTabularNums = 64, kVariantProportionalNums = 128, kVariantLiningNums = 256, kVariantOldstyleNums = 512, kVariantSlashedZero = 1024,
                   kVariantNoLigatures = 2048, kVariantOrdinal = 4096, kVariantDiagonalFractions = 8192, kVariantStackedFractions = 16384;
constexpr uint16_t kVariantCapsMask = kVariantSmallCaps | kVariantAllSmallCaps | kVariantPetiteCaps | kVariantAllPetiteCaps | kVariantUnicase | kVariantTitlingCaps;
constexpr uint16_t kVariantNumericMask = kVariantTabularNums | kVariantProportionalNums | kVariantLiningNums | kVariantOldstyleNums | kVariantSlashedZero | kVariantOrdinal |
                                         kVariantDiagonalFractions | kVariantStackedFractions;

// A length or percentage of something not known yet (a radius or an offset that is a percentage of the box), or auto.
struct Dimension {
  enum Kind : uint8_t { Px, Percent, Auto };
  Kind kind = Px;
  int32_t x100 = 0;   // pixels x100, or percent x100
  static Dimension px(int32_t px100) {
    Dimension d;
    d.kind = Px;
    d.x100 = px100;
    return d;
  }
  static Dimension percent(int32_t x100) {
    Dimension d;
    d.kind = Percent;
    d.x100 = x100;
    return d;
  }
  static Dimension automatic() {
    Dimension d;
    d.kind = Auto;
    return d;
  }
  bool is_auto() const { return kind == Auto; }
  // The value in pixels given what 100% is.
  int32_t resolve(int32_t percent_of_px100) const {
    if (kind == Px) return x100;
    if (kind == Percent) return static_cast<int32_t>(static_cast<int64_t>(percent_of_px100) * x100 / 10000);
    return 0;
  }
};

// A place along an axis (background-position, object-position, transform-origin): `percent_x100` percent of the room there is, plus `px100` pixels
// ("right 10px" is 100% and -10px; "20%" is 20% and 0).
struct AxisPos {
  int32_t percent_x100 = 0;
  int32_t px100 = 0;
  int32_t resolve(int32_t room_px100) const { return static_cast<int32_t>(static_cast<int64_t>(room_px100) * percent_x100 / 10000) + px100; }
};

// The two radii of a corner (border-radius); a percentage is of the border box's width (h) or height (v).
struct CornerRadius {
  Dimension h, v;
};

// A size as computed: a number of pixels (x100), or a keyword.
struct Length {
  enum Kind : uint8_t { Auto, Px, MinContent, MaxContent, FitContent, None };
  int32_t px100 = 0;
  Kind kind = Auto;
  // The unresolved form, for the pixel layout (the old renderer ignores these). A length with a percentage keeps `kind` and `px100` as they always were (a
  // percentage of the box the style was worked out in, Style::content_px, is already in px100; a percentage height or inset on top/bottom is still Auto and
  // max-* of one is still None), and also says what it is made of:
  //   linear form:      value = abs100 + percent100 / 10000 * base      (calc(100% - 10px): percent100 = 10000, abs100 = -1000; 50%: 5000 and 0)
  //   expression form:  `expr` != 0 when the length mixes percentages and absolute lengths in a way that is not linear (min(50%, 400px), clamp(), round() ...).
  //                     The text is kept in the sheet; StyleSheet::resolve_length_in() works it out against a base. abs100 is its value at 0%.
  // So: if has_percent(), the new fields are the truth; else px100 is.
  int32_t percent100 = 0;   // hundredths of a percent
  int32_t abs100 = 0;       // the absolute part in pixels x100 (only meaningful when has_percent())
  uint32_t expr = 0;        // 0: none; otherwise a number for StyleSheet::length_expression()
  bool has_percent() const { return percent100 != 0 || expr != 0; }
  // The value in pixels x100 when 100% is `percent_base_px100`. For the expression form this is only the value at 0%: use StyleSheet::resolve_length_in().
  int32_t resolve(int32_t percent_base_px100) const {
    if (!has_percent()) return px100;
    return abs100 + static_cast<int32_t>(static_cast<int64_t>(percent_base_px100) * percent100 / 10000);
  }
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
// What border_color holds for `transparent` (0 means the text colour; no real colour has an alpha of 0).
constexpr uint32_t kTransparentBorder = 1;

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
  Length text_indent;               // the same as a Length: a percentage stays one (of the containing block's width) (inherited)
  int32_t content_px = 640;   // the width of the box the element is in, in pixels: what a percentage means (set by the renderer)
  uint32_t custom_scope = 0;  // the custom properties that apply (an index into the sheet's scopes; 0: none) (inherited)
  // the pixel engine's text properties (inherited)
  uint16_t font_weight = 400;       // 1..1000; `bold` follows it (>= 600) when it is given
  uint16_t font_stretch_x10 = 1000; // font-stretch as a percentage x10 (500..2000; normal is 1000, condensed 750, expanded 1250)
  uint16_t font_variant = 0;        // kVariant* bits
  TextRef font_family;              // the list as written (read it with StyleSheet::font_families()); empty: the default
  int32_t letter_spacing_px100 = 0, word_spacing_px100 = 0;   // normal is 0
  TextRef text_shadow;              // the list as written (StyleSheet::shadows())
  Cursor cursor = Cursor::Auto;
  PointerEvents pointer_events = PointerEvents::Auto;
  UserSelect user_select = UserSelect::Auto;
  Visibility visibility = Visibility::Visible;
  WritingMode writing_mode = WritingMode::HorizontalTb;
  Direction direction = Direction::Ltr;
  ColorScheme color_scheme = ColorScheme::Normal;
  TextRef list_style_image;         // list-style-image (url() or a gradient; empty: none)
  uint16_t root_font_px = 16;       // the font size of the root element: what rem means

  // --- not inherited
  Display display = Display::Inline;
  Layout layout = Layout::Flow;
  bool inline_box = false;    // inline-block, inline-flex, inline-grid, inline-table: a box that sits in a line
  bool contents = false;      // display: contents (the element has no box of its own)
  // the pixel engine's box properties (not inherited)
  CornerRadius radius[4];                      // border-radius: top-left, top-right, bottom-right, bottom-left
  TextRef box_shadow;                          // the list as written (StyleSheet::shadows())
  TextRef background_image, background_size, background_position, background_repeat, background_clip, background_origin, background_attachment;   // lists as written
  uint16_t opacity_x1000 = 1000;               // opacity 0..1 (opaque_zero is set when it is 0)
  TextRef transform_value, translate, rotate, scale;   // the transform property (`transform` is text-transform), translate, rotate and scale as written (StyleSheet::transform_ops())
  AxisPos transform_origin_x = {5000, 0}, transform_origin_y = {5000, 0};
  int32_t transform_origin_z_px100 = 0;
  int32_t outline_width_px100 = 300;
  BorderStyle outline_style = BorderStyle::None;
  bool outline_auto = false;                   // outline-style: auto
  uint32_t outline_color = 0;                  // 0: the text colour, kTransparentBorder: none
  int32_t outline_offset_px100 = 0;
  uint32_t aspect_ratio_x1000 = 0;             // width / height x1000 (0: no ratio)
  bool aspect_auto = false;                    // aspect-ratio: auto [ratio] (the natural ratio comes first)
  ObjectFit object_fit = ObjectFit::Fill;
  AxisPos object_position_x = {5000, 0}, object_position_y = {5000, 0};
  int32_t column_rule_width_px100 = 300;
  BorderStyle column_rule_style = BorderStyle::None;
  uint32_t column_rule_color = 0;              // 0: the text colour, kTransparentBorder: none
  TextRef transition_property, transition_duration, transition_timing, transition_delay, transition_behavior;   // lists as written (StyleSheet::transitions())
  TextRef animation_name, animation_duration, animation_timing, animation_delay, animation_iteration_count, animation_direction, animation_fill_mode, animation_play_state;   // (StyleSheet::animations())
  TextRef filter, backdrop_filter, clip_path;  // as written (StyleSheet::filters(), clip_path())
  TextRef scroll_snap_type, scroll_snap_align; // stored only
  bool scroll_snap_stop_always = false;        // stored only
  OverscrollBehavior overscroll_x = OverscrollBehavior::Auto, overscroll_y = OverscrollBehavior::Auto;   // stored only
  bool scroll_smooth = false;                  // scroll-behavior: smooth (stored only)
  TextRef container_name;                      // container-name
  uint8_t container_type = 0;                  // container-type: 0 normal, 1 inline-size, 2 size

  uint8_t margin_top = 0, margin_bottom = 0, padding_top = 0, padding_bottom = 0;   // blank lines (what the flow layout uses)
  uint8_t margin_left = 0, padding_left = 0;                                        // columns (what the flow layout uses)
  uint16_t width_px = 0, height_px = 0;        // width / height in pixels (0: auto), for pictures, videos and canvases
  uint8_t width_pct = 0;                        // width as a percentage of the line (0: not a percentage)
  bool own_background = false;                 // this element set the background
  uint32_t own_background_color = 0;           // what it set (for the page's background on <body>)
  uint32_t background_argb = 0;                // the same colour with its alpha kept (own_background_color shows a colour that is mostly transparent as none): what the pixel engine paints
  bool opaque_zero = false;                    // opacity: 0 (nothing is seen, the space is kept)

  // the box
  Length width, height, min_width, min_height, max_width, max_height;
  Length margin[4];           // top, right, bottom, left (auto or pixels)
  Length padding[4];
  int32_t border_px100[4] = {0, 0, 0, 0};
  BorderStyle border_style[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};
  uint32_t border_color[4] = {0, 0, 0, 0};     // 0: the text colour, kTransparentBorder: none (the border takes room but shows nothing)
  BoxSizing sizing = BoxSizing::ContentBox;
  Position position = Position::Static;
  Float float_side = Float::None;
  Clear clear = Clear::None;
  Overflow overflow_x = Overflow::Visible, overflow_y = Overflow::Visible;
  bool text_ellipsis = false;
  uint8_t vertical_align = 0;                  // 0 baseline, 1 top, 2 middle, 3 bottom
  int32_t z_index = 0;
  bool z_index_auto = true;                    // z-index: auto (a positioned box with an integer z-index, 0 included, makes a stacking context; with auto it does not)
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
    s.background_argb = 0;
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
    s.z_index_auto = true;
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
    for (CornerRadius& r : s.radius) r = CornerRadius();
    s.box_shadow = TextRef();
    s.background_image = s.background_size = s.background_position = s.background_repeat = s.background_clip = s.background_origin = s.background_attachment = TextRef();
    s.opacity_x1000 = 1000;
    s.transform_value = s.translate = s.rotate = s.scale = TextRef();
    s.transform_origin_x = s.transform_origin_y = AxisPos{5000, 0};
    s.transform_origin_z_px100 = 0;
    s.outline_width_px100 = 300;
    s.outline_style = BorderStyle::None;
    s.outline_auto = false;
    s.outline_color = 0;
    s.outline_offset_px100 = 0;
    s.aspect_ratio_x1000 = 0;
    s.aspect_auto = false;
    s.object_fit = ObjectFit::Fill;
    s.object_position_x = s.object_position_y = AxisPos{5000, 0};
    s.column_rule_width_px100 = 300;
    s.column_rule_style = BorderStyle::None;
    s.column_rule_color = 0;
    s.transition_property = s.transition_duration = s.transition_timing = s.transition_delay = s.transition_behavior = TextRef();
    s.animation_name = s.animation_duration = s.animation_timing = s.animation_delay = s.animation_iteration_count = TextRef();
    s.animation_direction = s.animation_fill_mode = s.animation_play_state = TextRef();
    s.filter = s.backdrop_filter = s.clip_path = TextRef();
    s.scroll_snap_type = s.scroll_snap_align = TextRef();
    s.scroll_snap_stop_always = false;
    s.overscroll_x = s.overscroll_y = OverscrollBehavior::Auto;
    s.scroll_smooth = false;
    s.container_name = TextRef();
    s.container_type = 0;
    return s;
  }

  Style() {
    margin[0] = margin[1] = margin[2] = margin[3] = Length::px(0);
    padding[0] = padding[1] = padding[2] = padding[3] = Length::px(0);
    max_width = max_height = Length::of(Length::None);
  }
};

// --- Structures the readers of StyleSheet fill in. Views of text (names, urls) point into the sheet and are valid until it parses again or is cleared.

// box-shadow and text-shadow entries.
struct Shadow {
  int32_t x_px100 = 0, y_px100 = 0, blur_px100 = 0, spread_px100 = 0;
  uint32_t color = 0;          // 0xAARRGGBB
  bool current_color = true;   // no colour given (or currentcolor): the text colour
  bool inset = false;
};

enum class GenericFamily : uint8_t { None, Serif, SansSerif, Monospace, Cursive, Fantasy, SystemUi, UiSerif, UiSansSerif, UiMonospace, UiRounded, Math, Emoji, Fangsong };
struct FontFamily {
  StringView name;             // a family name (quotes taken off; an unquoted name as written, with its words one space apart); empty for a generic family
  GenericFamily generic = GenericFamily::None;
};

enum class RepeatMode : uint8_t { Repeat, Space, Round, NoRepeat };
enum class BackgroundBox : uint8_t { BorderBox, PaddingBox, ContentBox, Text };
enum class BackgroundAttachment : uint8_t { Scroll, Fixed, Local };

struct ColorStop {
  uint32_t color = 0;
  bool current_color = false;
  Dimension position = Dimension::automatic();   // a length or percentage; (auto: not given). For a conic gradient an angle is kept as percent of the turn.
  bool hint = false;                             // an interpolation hint (a position only; no colour)
};
constexpr uint32_t kMaxGradientStops = 24;

struct Gradient {
  enum Kind : uint8_t { Linear, Radial, Conic };
  enum Extent : uint8_t { ClosestSide, ClosestCorner, FarthestSide, FarthestCorner, Explicit };
  Kind kind = Linear;
  bool repeating = false;
  int32_t angle_x100 = 18000;      // linear: direction in degrees x100 (180: to bottom); conic: the start angle
  uint8_t to_sides = 0;            // linear "to top right ...": bit 1 top, 2 right, 4 bottom, 8 left (the angle depends on the box then)
  bool circle = false;             // radial: circle (else ellipse)
  Extent extent = FarthestCorner;
  Dimension radius_x, radius_y;    // radial with Explicit extent (a circle has radius_x only)
  AxisPos center_x = {5000, 0}, center_y = {5000, 0};
  uint32_t stop_count = 0;
  ColorStop stops[kMaxGradientStops];
};

// One layer of the background (the lists of the background properties zipped; shorter lists repeat).
struct BackgroundLayer {
  enum Image : uint8_t { None, Url, GradientImage, Other };
  Image image = None;
  StringView url;                  // Url: the address, unquoted
  Gradient gradient;
  enum SizeMode : uint8_t { Auto, Cover, Contain, Explicit };
  SizeMode size_mode = Auto;
  Dimension size_w = Dimension::automatic(), size_h = Dimension::automatic();   // Explicit
  AxisPos position_x, position_y;
  RepeatMode repeat_x = RepeatMode::Repeat, repeat_y = RepeatMode::Repeat;
  BackgroundBox clip = BackgroundBox::BorderBox, origin = BackgroundBox::PaddingBox;
  BackgroundAttachment attachment = BackgroundAttachment::Scroll;
};

// One function of transform (or the whole of translate / rotate / scale).
struct TransformOp {
  enum Kind : uint8_t { Matrix, Translate, Scale, Rotate, Skew, Perspective };
  Kind kind = Matrix;
  Dimension x = Dimension::px(0), y = Dimension::px(0);   // Translate (percentages are of the box)
  double z_px = 0;                                       // Translate z, Perspective distance
  double sx = 1, sy = 1, sz = 1;                         // Scale
  double angle_deg = 0;                                  // Rotate, Skew (x)
  double angle_y_deg = 0;                                // Skew (y)
  double ax = 0, ay = 0, az = 1;                         // Rotate axis (z for a 2D rotation)
  double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // Matrix, column major like matrix3d()
};

struct FilterFunction {
  enum Kind : uint8_t { Blur, Brightness, Contrast, Grayscale, HueRotate, Invert, Opacity, Saturate, Sepia, DropShadow, Url };
  Kind kind = Blur;
  double amount = 0;       // Blur: pixels; HueRotate: degrees; the others a factor (1 = 100%)
  Shadow shadow;           // DropShadow
  StringView url;          // Url
};

struct ClipPath {
  enum Kind : uint8_t { None, Inset, Circle, Ellipse, Polygon, Path, Url, Box };
  Kind kind = None;
  Dimension inset[4];                    // Inset: top, right, bottom, left
  CornerRadius round[4];                 // Inset: the corners
  Dimension radius_x = Dimension::percent(5000), radius_y = Dimension::percent(5000);   // Circle (radius_x), Ellipse; auto: closest-side (farthest-side when radius_far)
  bool radius_far = false;
  AxisPos center_x = {5000, 0}, center_y = {5000, 0};
  static constexpr uint32_t kMaxPoints = 32;
  uint32_t point_count = 0;              // Polygon
  Dimension points_x[kMaxPoints], points_y[kMaxPoints];
  bool evenodd = false;
  StringView path_data;                  // Path
  StringView url;                        // Url
  BackgroundBox reference_box = BackgroundBox::BorderBox;   // the geometry box (margin-box counts as border-box)
};

struct TimingFunction {
  enum Kind : uint8_t { CubicBezier, Steps, Linear };
  Kind kind = CubicBezier;
  double x1 = 0.25, y1 = 0.1, x2 = 0.25, y2 = 1.0;   // CubicBezier ("ease" by default)
  int32_t steps = 1;                                  // Steps
  bool jump_start = false;                            // Steps: step-start / jump-start (else jump-end)
};

struct Transition {
  StringView property;            // a property name, "all" or "none"
  int32_t duration_ms = 0, delay_ms = 0;
  TimingFunction timing;
  bool allow_discrete = false;
};

struct Animation {
  enum Direction : uint8_t { Normal, Reverse, Alternate, AlternateReverse };
  enum Fill : uint8_t { FillNone, Forwards, Backwards, Both };
  StringView name;                // empty / "none": no animation
  int32_t duration_ms = 0, delay_ms = 0;
  TimingFunction timing;
  double iterations = 1;          // infinity: -1
  Direction direction = Normal;
  Fill fill = FillNone;
  bool paused = false;
};

// @import rules in order of appearance: the page fetches the url and reads the sheet in (with set_layer(layer) when has_layer).
struct ImportRule {
  StringView url;
  StringView media;               // the media query list after the url (empty: none)
  StringView supports;            // the condition inside supports(...) (empty: none)
  StringView layer;               // the name inside layer(...) (empty: an anonymous layer when has_layer)
  bool has_layer = false;
  bool applies = true;            // the media query and supports condition hold for the environment the sheet was read with
};

enum class FontFormat : uint8_t { None, Woff2, Woff, TrueType, OpenType, EmbeddedOpenType, Svg, Collection, Unknown };
enum class FontDisplay : uint8_t { Auto, Block, Swap, Fallback, Optional };
struct UnicodeRange {
  uint32_t first = 0, last = 0x10FFFF;
};
struct FontFaceSource {
  bool local = false;             // local(name) rather than url(...)
  StringView value;               // the url, or the font name
  FontFormat format = FontFormat::None;   // from format("..."); None when not given
  StringView format_text;         // as written (unquoted)
  bool supported_tech = true;     // tech(...) lists only technologies this engine knows (variations, features-opentype, color-COLRv0 ...)
};
constexpr uint32_t kMaxFontFaceSources = 8;
// An @font-face rule: what the font loading needs. Weight and stretch are ranges (a single value gives min == max).
struct FontFace {
  StringView family;
  FontFaceSource sources[kMaxFontFaceSources];
  uint32_t source_count = 0;
  uint16_t weight_min = 400, weight_max = 400;
  uint8_t style = 0;              // 0 normal, 1 italic, 2 oblique
  int32_t oblique_min_x100 = 1400, oblique_max_x100 = 1400;   // degrees x100 (style oblique)
  uint16_t stretch_min_x10 = 1000, stretch_max_x10 = 1000;    // percent x10
  ArrayView<const UnicodeRange> unicode_ranges;               // empty: every character
  FontDisplay display = FontDisplay::Auto;
  // Does the face cover the code point (its unicode-range)?
  bool covers(uint32_t code_point) const {
    if (unicode_ranges.size() == 0) return true;
    for (const UnicodeRange& r : unicode_ranges)
      if (code_point >= r.first && code_point <= r.last) return true;
    return false;
  }
};

// An @property rule.
struct PropertyRegistration {
  StringView name;
  StringView syntax;              // as written, unquoted ("<length>", "*")
  bool inherits = true;
  bool has_initial = false;
  StringView initial_value;
};

// Tells the style how big the container of an element is, for @container rules: the nearest ancestor with container-type (named `name` when not empty).
// False: no such container (the rule does not apply). Without one set, the viewport is the container.
class IContainerSizes {
 public:
  virtual bool container_size(const dom::Node* element, StringView name, int32_t* width_px, int32_t* height_px) = 0;
};

class StyleSheet {
 public:
  static constexpr uint32_t kMaxRules = 10000;

  explicit StyleSheet(allocators::IAllocator& allocator)
      : allocator_(allocator), pool_(allocator), selectors_(allocator), declarations_(allocator), rules_(allocator), scopes_(allocator), kept_(allocator),
        imports_(allocator), font_faces_(allocator), unicode_ranges_(allocator), keyframes_(allocator), stops_(allocator), registrations_(allocator), layers_(allocator),
        layer_rank_(allocator), containers_(allocator) {}
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
  // One custom property of a scope chain (Style::custom_scope): its name and value, and the scope that comes next (0 at the end); false for scope 0.
  bool custom_property_at(uint32_t scope, StringView* name, StringView* value, uint32_t* parent) const;
  TextRef keep_text(StringView s) const;
  // The size of the display, for vw / vh.
  void set_viewport(int32_t width_px, int32_t height_px) {
    viewport_w_ = width_px;
    viewport_h_ = height_px;
  }
  // Works out a CSS length expression ("2em", "calc(100% - 10px)", "min(50vw, 400px)") for the style of an element: false if it is not one. `percent_px` is what 100% is.
  bool resolve_length(StringView expression, const Style& style, int32_t percent_px, Length* out) const;
  // A length of the expression form (Length::expr != 0) worked out for the style of its element when 100% is `percent_base_px100` (pixels x100). Any other length
  // gives Length::resolve(). False if the text no longer works out.
  bool resolve_length_in(const Length& length, const Style& style, int32_t percent_base_px100, int32_t* out_px100) const;
  // The text of an expression form length ("min(50%, 400px)"); empty for other lengths.
  StringView length_expression(const Length& length) const;

  // --- reading the stored text of list valued properties (false: the allocator ran out; a list that is not valid is left empty)
  bool font_families(const Style& style, List<FontFamily>* out) const;
  bool shadows(TextRef list, const Style& style, List<Shadow>* out) const;                  // style.box_shadow or style.text_shadow
  bool background_layers(const Style& style, List<BackgroundLayer>* out) const;
  bool transform_ops(const Style& style, List<TransformOp>* out) const;                     // the translate, rotate, scale and transform properties in the order they apply
  // The matrix of the transform of a box of that size (column major like matrix3d()); with `around_origin` the transform-origin is taken into account (else it is the
  // matrix getComputedStyle() shows). False: no transform (the identity).
  bool transform_matrix(const Style& style, double box_width_px, double box_height_px, double out[16], bool around_origin = true) const;
  bool transitions(const Style& style, List<Transition>* out) const;
  bool animations(const Style& style, List<Animation>* out) const;
  bool filters(TextRef list, const Style& style, List<FilterFunction>* out) const;          // style.filter or style.backdrop_filter
  bool clip_path(const Style& style, ClipPath* out) const;

  // --- at-rules
  // The rules read from now on belong to this cascade layer (an @import ... layer(name)); empty: none.
  void set_layer(StringView layer_path);
  uint32_t import_count() const { return static_cast<uint32_t>(imports_.count()); }
  bool import_at(uint32_t index, ImportRule* out) const;
  uint32_t font_face_count() const { return static_cast<uint32_t>(font_faces_.count()); }
  bool font_face(uint32_t index, FontFace* out) const;
  uint32_t keyframes_count() const { return static_cast<uint32_t>(keyframes_.count()); }
  StringView keyframes_name(uint32_t rule) const;
  bool find_keyframes(StringView name, uint32_t* rule) const;   // the last @keyframes of that name
  uint32_t keyframe_count(uint32_t rule) const;
  int32_t keyframe_offset_x1000(uint32_t rule, uint32_t index) const;   // 0..1000, in ascending order
  // Applies the declarations of one keyframe to a style (which starts as the element's style at the time).
  void apply_keyframe(uint32_t rule, uint32_t index, const Style& parent, Style* style) const;
  // The declarations of a keyframe as property names and values (custom properties included).
  uint32_t keyframe_declaration_count(uint32_t rule, uint32_t index) const;
  bool keyframe_declaration(uint32_t rule, uint32_t index, uint32_t k, StringView* name, StringView* value) const;
  uint32_t property_registration_count() const { return static_cast<uint32_t>(registrations_.count()); }
  bool property_registration(uint32_t index, PropertyRegistration* out) const;
  uint32_t layer_count() const { return static_cast<uint32_t>(layers_.count()); }
  // The environment's colour scheme preference (light-dark() and color-scheme: normal follow it); parse() takes it from the MediaEnvironment.
  void set_dark(bool dark) { dark_ = dark; }
  void set_container_sizes(IContainerSizes* sizes) { container_sizes_ = sizes; }

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
    uint32_t layer = 0;       // 0: not in a layer; otherwise the index (from 1) in layers_
    uint32_t container = 0;   // 0: none; otherwise the index (from 1) in containers_ (the innermost @container it is in)
  };
  struct Span {
    uint32_t offset = 0, length = 0;   // in pool_
  };
  struct Layer {
    uint32_t parent;          // 0 or the index (from 1) of the layer it is in
    Span name;                // its own name (a dotted name is several layers)
    uint32_t ordinal;         // among the layers of its parent, in order of first mention
  };
  struct Container {
    uint32_t parent;          // the @container this one is in (from 1; 0: none)
    Span name, query;
  };
  struct KeyframeStop {
    int32_t offset_x1000;
    uint32_t first_declaration, declaration_count;
  };
  struct KeyframesRule {
    Span name;
    uint32_t first_stop, stop_count;
  };
  struct ImportData {
    Span url, media, supports, layer;
    bool has_layer, applies;
  };
  struct FontFaceData {
    Span family;
    struct Source {
      bool local = false, supported = true;
      Span value, format_text;
      FontFormat format = FontFormat::None;
    } sources[kMaxFontFaceSources];
    uint32_t source_count = 0;
    uint16_t weight_min = 400, weight_max = 400, stretch_min = 1000, stretch_max = 1000;
    uint8_t style = 0;
    int32_t oblique_min_x100 = 1400, oblique_max_x100 = 1400;
    uint32_t first_range = 0, range_count = 0;
    FontDisplay display = FontDisplay::Auto;
  };
  struct Registration {
    Span name, syntax, initial;
    bool inherits, has_initial;
  };
  struct Scope {
    uint32_t parent;           // the scope this one adds to
    uint32_t name_offset, name_length, value_offset, value_length;
  };

  bool parse_block(StringView css, uint64_t* at, bool nested);
  bool parse_rule(StringView prelude, StringView body);
  bool add_declarations(StringView body, uint32_t rule);
  bool add_declaration_run(StringView body, uint32_t* first, uint32_t* count);
  bool parse_at_rule(StringView name, StringView prelude, StringView body, bool has_body, bool* recurse);
  bool parse_import(StringView prelude);
  bool parse_font_face(StringView body);
  bool parse_keyframes(StringView prelude, StringView body);
  bool parse_property_rule(StringView prelude, StringView body);
  bool parse_layer_statement(StringView prelude);
  uint32_t enter_layer(StringView path);          // the layer for a (dotted) name inside the current one; 0 if the table is full
  void rank_layers();
  StringView span_text(Span span) const;
  bool container_applies(uint32_t container, const dom::Node* element) const;
  bool keep_span(StringView s, Span* out) {
    out->length = static_cast<uint32_t>(s.size());
    return keep(s, &out->offset);
  }
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
  bool dark_ = false;
  bool seen_rule_ = false;             // a rule other than @charset / @layer / @import has been read (an @import after it is ignored)
  uint32_t current_layer_ = 0;         // while reading: the layer the rules belong to
  uint32_t import_layer_ = 0;          // the layer set by set_layer()
  uint32_t current_container_ = 0;     // while reading: the innermost @container
  static constexpr uint32_t kMaxScopeDepth = 4;
  Span scope_root_[kMaxScopeDepth], scope_limit_[kMaxScopeDepth];
  uint32_t scope_depth_ = 0;           // while reading: how many @scope rules we are in
  uint32_t anonymous_layers_ = 0;
  List<ImportData> imports_;
  List<FontFaceData> font_faces_;
  List<UnicodeRange> unicode_ranges_;
  List<KeyframesRule> keyframes_;
  List<KeyframeStop> stops_;
  List<Registration> registrations_;
  List<Layer> layers_;
  List<uint32_t> layer_rank_;          // per layer: its place in the order (from 1)
  List<Container> containers_;
  IContainerSizes* container_sizes_ = nullptr;
  mutable uint32_t text_index_[1024] = {};   // keep_text(): the latest kept text by hash (index + 1)
  friend struct CssRead;
};

}  // namespace browser
