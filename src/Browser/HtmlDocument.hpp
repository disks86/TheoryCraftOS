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
// (on the same line, on a new line, after a blank line), and the link targets some of them lead to. HtmlDocument::parse_html() reads HTML the
// forgiving way a browser does - unknown and unclosed tags are ignored, entities are decoded, white space is collapsed (except in <pre>), scripts
// and styles are dropped - and knows headings, paragraphs, line breaks, rules, lists (bulleted and numbered), block quotes, preformatted text, bold
// text, code, tables (their cells side by side, divided by '|'), links and the page's title; an image shows as its alternative text. Only the
// printable ASCII characters can be shown, so other characters come out as '?' (a few common ones - quotes, dashes, a no-break space - as their
// ASCII look-alikes). parse_text() takes plain text as it is.
//
// All of the text lives in one pool and the runs point into it, so a document is a few allocations however long the page is.
#pragma once
#include "Browser/Css.hpp"
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
};

// What runs the scripts of a page while it is read: the parser hands over each <script> in the order they come, and reads what they wrote with
// document.write in their place. Implemented by the browser on top of the script engine.
class IScriptRunner {
 public:
  // The text of the index-th <script src=...> of the page (counting them in the order the page names them); empty if it was not fetched.
  virtual StringView external(uint32_t index) = 0;
  // Runs one script. What it wrote with document.write is appended to *written.
  virtual void run(StringView source, String* written) = 0;

 protected:
  IScriptRunner() = default;
  ~IScriptRunner() = default;
};

class HtmlDocument {
 public:
  static constexpr uint64_t kMaxText = 4u << 20;   // the most text kept; a longer page is cut off there
  static constexpr uint64_t kMaxRuns = 250000;

  explicit HtmlDocument(allocators::IAllocator& allocator) : allocator_(allocator), runs_(allocator), links_(allocator), sheets_(allocator), scripts_(allocator), pool_(allocator), title_(allocator) {}
  HtmlDocument(const HtmlDocument&) = delete;
  HtmlDocument& operator=(const HtmlDocument&) = delete;

  // Replaces the contents with the page in `html`, styled by the stylesheets in it (<style>, style attributes) and by `extra_css` (the stylesheets
  // it links to, fetched by the caller; read before the page's own). False if memory ran out (the document is then empty).
  // With `scripts`, the page's scripts are run as they are met (see IScriptRunner); without, they are only counted (stylesheet_count() style: script_count()).
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
  // The stylesheet files the page links to (<link rel=stylesheet href=...>), as written (they may be relative).
  uint32_t stylesheet_count() const { return static_cast<uint32_t>(sheets_.count()); }
  StringView stylesheet(uint32_t index) const { return index < sheets_.count() ? pool_.view().substr(sheets_[index].offset, sheets_[index].length) : StringView(); }
  // The scripts the page loads from files (<script src=...>) as written, and whether it has any script at all (inline ones too).
  uint32_t script_count() const { return static_cast<uint32_t>(scripts_.count()); }
  StringView script_source(uint32_t index) const { return index < scripts_.count() ? pool_.view().substr(scripts_[index].offset, scripts_[index].length) : StringView(); }
  bool has_scripts() const { return has_scripts_; }
  // True if the page was longer than the limits and the end is missing.
  bool truncated() const { return truncated_; }

 private:
  friend class HtmlParser;
  struct LinkRef {
    uint32_t offset, length;
  };
  bool add_run(StringView text, uint8_t flags, uint8_t level, uint8_t indent, uint8_t breaks, int32_t link, uint8_t align, uint32_t color, uint32_t background);
  int32_t add_link(StringView target);
  bool add_stylesheet(StringView target);
  bool add_script_ref(StringView target);

  allocators::IAllocator& allocator_;
  List<Run> runs_;
  List<LinkRef> links_, sheets_, scripts_;
  uint32_t page_background_ = 0;
  bool has_scripts_ = false;
  String pool_, title_;
  bool truncated_ = false;
};

}  // namespace browser
