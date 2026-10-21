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

// CSS for a text-mode browser. StyleSheet reads stylesheets (the text of <style> elements and linked files) into rules; the HTML reader asks it which
// declarations apply to each element and turns them into a Style, which says what the text of the element looks like and how its block is spaced.
//
// Understood: selectors with type, universal, class, id and attribute ([a], [a=b], ~=, ^=, $=, *=) tests, the descendant and child combinators, selector
// lists, :first-child, :root, :link and :visited; specificity, source order, !important and the style attribute; @media rules (those that do not name
// print, a maximum width, a colour scheme or hovering are applied, the others left out) and @supports; comments. Left out: sibling combinators,
// :hover, :focus, :not(), :nth-child() and the other pseudo-classes and all pseudo-elements (a rule using them never applies), var() values,
// @import, @font-face, @keyframes.
//
// Used by the screen of fixed-width characters: color, background-color (and the colour in background), font-weight (bold or not), text-decoration
// (underline, line-through), text-transform, text-align, display (none, block, inline, list-item), visibility, white-space (preformatted or not),
// margin and padding (top and bottom as blank lines - about 16 pixels or one em each - and left as columns of 8 pixels), list-style none. Everything
// else (sizes, borders, floats, positions, fonts) is read and ignored.
#pragma once
#include "Browser/Media.hpp"
#include "Dom/Selector.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace browser {

// Colours are 0xAARRGGBB; 0 means "not set". Reads a CSS colour (names, #rgb, #rrggbb, #rgba, #rrggbbaa, rgb(), rgba(), hsl(), transparent). A
// transparent colour comes back as true with *out == 0. False for anything else (also "inherit" and "currentcolor").
bool parse_css_color(StringView text, uint32_t* out);

// The value of attribute `name` in the text of a tag (after its tag name), as written (entities are not decoded); false if it has none.
bool find_attribute(StringView attrs, StringView name, StringView* value);

enum class Display : uint8_t { Inline, Block, ListItem, None };

// What the text of an element looks like. Children start from their parent's style (see inherit()).
struct Style {
  uint32_t color = 0;         // text colour (inherited)
  uint32_t background = 0;    // the background that text is shown on (inherited: text on a block's background is on it too)
  bool background_block = false;   // that background belongs to a block, so it fills the lines the text is on
  bool bold = false, underline = false, strike = false, code = false, pre = false;   // (inherited)
  bool hidden = false;        // not shown: display none here or above, or visibility hidden (inherited)
  bool in_list = false;       // inside a list (inherited)
  bool list_none = false;     // list-style: none (inherited)
  uint8_t transform = 0;      // 1 uppercase, 2 lowercase (inherited)
  uint8_t align = 0;          // 0 left, 1 center, 2 right (inherited)
  uint8_t heading = 0;        // 1..6 inside a heading (inherited)
  uint8_t indent = 0;         // column the element's lines start at (inherited, and added to)
  Display display = Display::Inline;
  uint8_t margin_top = 0, margin_bottom = 0, padding_top = 0, padding_bottom = 0;   // blank lines (not inherited)
  uint8_t margin_left = 0, padding_left = 0;                                        // columns (not inherited)
  bool own_background = false;                 // this element set the background (not inherited)
  uint32_t own_background_color = 0;           // what it set (for the page's background on <body>)

  // The style a child starts with.
  Style inherit() const {
    Style s = *this;
    s.display = Display::Inline;
    s.margin_top = s.margin_bottom = s.padding_top = s.padding_bottom = s.margin_left = s.padding_left = 0;
    s.own_background = false;
    s.own_background_color = 0;
    return s;
  }
};

class StyleSheet {
 public:
  static constexpr uint32_t kMaxRules = 10000;

  explicit StyleSheet(allocators::IAllocator& allocator) : allocator_(allocator), pool_(allocator), selectors_(allocator), declarations_(allocator), rules_(allocator) {}
  StyleSheet(const StyleSheet&) = delete;
  StyleSheet& operator=(const StyleSheet&) = delete;

  // Adds the rules of a stylesheet (later ones win over earlier ones of the same specificity); @media rules apply when `env` satisfies their query. False
  // if memory ran out.
  bool parse(StringView css, const MediaEnvironment& env = MediaEnvironment());
  void clear();
  uint32_t rule_count() const { return static_cast<uint32_t>(rules_.count()); }

  // Applies what the sheet says about the element to `style`, then `inline_style` (the style attribute) on top. Properties that are not understood are
  // skipped.
  void apply(const dom::Node* element, StringView inline_style, Style* style) const;
  // Reads declarations ("color: red; font-weight: bold") into `style`; `important` picks which of the normal and !important ones are applied.
  static void apply_declarations(StringView text, bool important, Style* style);

 private:
  struct Declaration {
    uint8_t property;
    bool important;
    uint32_t value_offset, value_length;
  };
  struct Rule {
    uint32_t first_declaration, declaration_count, order;
  };

  bool parse_block(StringView css, uint64_t* at, bool nested);
  bool parse_rule(StringView prelude, StringView body);
  bool add_declarations(StringView body, uint32_t rule);
  StringView text(uint32_t offset, uint32_t length) const { return pool_.view().substr(offset, length); }
  bool keep(StringView s, uint32_t* offset) {
    *offset = static_cast<uint32_t>(pool_.size());
    return pool_.append(s);
  }

  allocators::IAllocator& allocator_;
  String pool_;
  dom::SelectorSet selectors_;
  const MediaEnvironment* env_ = nullptr;
  List<Declaration> declarations_;
  List<Rule> rules_;
};

}  // namespace browser
