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

// A web page reduced to what a text-mode browser shows: runs of text, each with the style it is shown in and how it follows the one before it
// (on the same line, on a new line, after a blank line), the link targets some of them lead to, and the document node each came from. An HtmlDocument
// is what render_document() (Renderer.hpp) makes of a document tree: HTML is read into the tree (src/Dom), the style sheets and defaults give every
// element its Style, and the text and boxes become runs - headings, paragraphs, line breaks, rules, lists (bulleted and numbered), block quotes,
// preformatted text, bold text, code, tables (their cells side by side, divided by '|'), links, colours, alignment and spacing. An image shows as its
// alternative text. Only the printable ASCII characters can be shown, so other characters come out as what they look like in ASCII ("e" for "é", "-" for
// a dash) or '?'. parse_html() does it all for HTML text; parse_text() takes plain text as it is.
//
// All of the text lives in one pool and the runs point into it, so a document is a few allocations however long the page is.
#pragma once
#include "Browser/Css.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace browser {

enum RunFlag : uint8_t {
  kRunHeading = 1,   // a heading (Run::level says which)
  kRunBold = 2,
  kRunCode = 4,      // code: shown on a shaded background
  kRunPre = 8,       // preformatted: line breaks and spaces stay as they are
  kRunRule = 16,     // a horizontal rule (no text)
  kRunUnderline = 32,
  kRunStrike = 64,   // struck through
  kRunBlockBg = 128, // the background (Run::background) belongs to a block: it fills the lines the text is on
};

struct Run {
  uint32_t offset = 0, length = 0;   // the text, in the pool
  uint8_t flags = 0;                 // RunFlag bits
  uint8_t level = 0;                 // 1..6 for headings
  uint8_t indent = 0;                // the column that lines of this run start at
  uint8_t breaks = 0;                // line breaks before it: 0 follows the text before it, 1 starts a line, 2 leaves a blank line, 3 two blank lines...
  uint8_t align = 0;                 // 0 left, 1 centred, 2 right (of the line it is on)
  uint32_t color = 0;                // text colour 0xAARRGGBB, 0: the default for the kind of text
  uint32_t background = 0;           // 0: none
  int32_t link = -1;                 // which link it leads to, or -1
  uint32_t node = 0;                 // the serial of the element the text is in (dom::Node::serial), 0 for none
};

// What runs the scripts of a page while it is read (see dom::IScriptRunner).
using IScriptRunner = dom::IScriptRunner;

struct RenderOptions;

class HtmlDocument {
 public:
  static constexpr uint64_t kMaxText = 4u << 20;   // the most text kept; a longer page is cut off there
  static constexpr uint64_t kMaxRuns = 250000;

  explicit HtmlDocument(allocators::IAllocator& allocator) : allocator_(allocator), runs_(allocator), links_(allocator), pool_(allocator), title_(allocator) {}
  HtmlDocument(const HtmlDocument&) = delete;
  HtmlDocument& operator=(const HtmlDocument&) = delete;

  // Replaces the contents with the page in `html`, styled by the style sheets in it (<style>, style attributes) and by `extra_css` (read before the
  // page's own). With `scripts`, the page's scripts are run as they are met. False if memory ran out (the document is then empty).
  bool parse_html(StringView html, StringView extra_css = StringView(), IScriptRunner* scripts = nullptr);
  // Replaces the contents with `text`, shown as it is. False if memory ran out.
  bool parse_text(StringView text);
  void clear();

  StringView title() const { return title_.view(); }
  uint64_t run_count() const { return runs_.count(); }
  const Run& run(uint64_t index) const { return runs_[index]; }
  StringView run_text(uint64_t index) const { return pool_.view().substr(runs_[index].offset, runs_[index].length); }
  uint32_t link_count() const { return static_cast<uint32_t>(links_.count()); }
  // Where link `index` leads, as written in the page (it may be relative).
  StringView link(uint32_t index) const { return index < links_.count() ? pool_.view().substr(links_[index].offset, links_[index].length) : StringView(); }
  allocators::IAllocator& allocator() const { return allocator_; }
  // The page's own background (<body> or <html>), 0 if it has none.
  uint32_t page_background() const { return page_background_; }
  // True if the page was longer than the limits and the end is missing.
  bool truncated() const { return truncated_; }

 private:
  friend class DocumentRenderer;
  friend bool render_document(dom::Document&, const RenderOptions&, HtmlDocument*);
  struct LinkRef {
    uint32_t offset, length;
  };
  bool add_run(StringView text, uint8_t flags, uint8_t level, uint8_t indent, uint8_t breaks, int32_t link, uint8_t align, uint32_t color, uint32_t background, uint32_t node);
  int32_t add_link(StringView target);

  allocators::IAllocator& allocator_;
  List<Run> runs_;
  List<LinkRef> links_;
  uint32_t page_background_ = 0;
  String pool_, title_;
  bool truncated_ = false;
};

}  // namespace browser
