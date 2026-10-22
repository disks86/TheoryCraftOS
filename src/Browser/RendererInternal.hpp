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

// The page renderer's internals, shared by Renderer.cpp (text, lists, tables, forms, objects) and RendererBoxes.cpp (boxes with borders and sizes, flexbox, grid).
#pragma once
#include <initializer_list>
#include "Browser/PageLayout.hpp"
#include "Browser/Renderer.hpp"
#include "Dom/Entities.hpp"
#include "Dom/Form.hpp"
#include "Utility/Move.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

constexpr uint32_t kMaxIndent = 60;

bool is_space(char c);
bool tag_in(const dom::Node* n, std::initializer_list<const char*> names);
StringView attr(const dom::Node* n, const char* name);
Style derive_style(const Style& parent, const dom::Node* n, const StyleSheet& sheet);

class DocumentRenderer {
 public:
  DocumentRenderer(HtmlDocument& out, const StyleSheet& sheet, const RenderOptions& options)
      : out_(out), sheet_(sheet), objects_(options.objects), options_(options), chunk_(out.allocator()), stack_(out.allocator()) {
    container_cols_ = options.media.width >= 64 ? static_cast<uint32_t>(options.media.width) / 8 : 80;
    root_.content_px = static_cast<int32_t>(container_cols_) * 8;
  }

  bool run(const dom::Document& doc) {
    const dom::Node* root = doc.root();
    modal_pass_ = true;   // open modal dialogs come first
    for (uint32_t serial = 1; serial <= doc.node_count() && !failed_; ++serial) {
      const dom::Node* d = doc.by_serial(serial);
      if (d && d->is_tag("dialog") && (d->form_state & dom::kFormModal) && dom::has_attribute(d, "open") && dom::is_connected(d)) walk(d);
    }
    flush();
    modal_pass_ = false;
    const dom::Node* n = root->first_child;
    while (n && !out_.truncated_ && !failed_) {
      const bool descend = enter(n);
      if (descend && n->first_child) {
        n = n->first_child;
        continue;
      }
      // leave this node and go on with the next one, leaving the parents when their last child is done
      const dom::Node* cur = n;
      n = nullptr;
      while (cur) {
        if (stack_.count() && stack_[stack_.count() - 1].node == cur) pop_element();
        if (cur->next) {
          n = cur->next;
          break;
        }
        cur = cur->parent;
        if (cur == root) break;
      }
    }
    flush();
    return !failed_;
  }

 private:
  struct Element {
    Style style;
    int32_t link = -1;
    uint32_t items = 0;   // list items (ul, ol) or cells (tr) so far
    const dom::Node* node = nullptr;
  };

  const Style& cur() const { return stack_.count() ? stack_[stack_.count() - 1].style : root_; }
  int32_t cur_link() const { return stack_.count() ? stack_[stack_.count() - 1].link : -1; }
  uint32_t cur_node() const { return stack_.count() ? stack_[stack_.count() - 1].node->serial : 0; }
  bool hidden() const { return cur().hidden; }
  uint8_t indent() const { return static_cast<uint8_t>(cur().indent < kMaxIndent ? cur().indent : kMaxIndent); }

  // One character of page text, with white space collapsed (unless preformatted).
  void put(char c) {
    if (hidden()) return;
    if (cur().pre) {
      if (c == '\r') return;
      if (c == '\t') {
        failed_ |= !chunk_.append("    ");
        ended_space_ = true;
        return;
      }
      if (c != '\n' && (c < 32 || c > 126)) return;
      failed_ |= !chunk_.push_back(c);
      ended_space_ = c == '\n' || c == ' ';
      return;
    }
    if (is_space(c)) {
      if (ended_space_) return;
      failed_ |= !chunk_.push_back(' ');
      ended_space_ = true;
      return;
    }
    if (c < 32 || c > 126) return;
    failed_ |= !chunk_.push_back(c);
    ended_space_ = false;
  }

  void text(const dom::Node* n) {
    const StringView s = n->data.view();
    for (uint64_t i = 0; i < s.size();) {
      if (static_cast<uint8_t>(s[i]) < 0x80) {
        put(s[i++]);
        continue;
      }
      const uint32_t cp = dom::next_code_point(s, &i);
      const char* look = dom::ascii_look_alike(cp);
      if (!look) put('?');
      else
        for (const char* p = look; *p; ++p) put(*p);
    }
  }

  uint32_t run_flags(const Style& s) const {
    uint32_t flags = 0;
    if (s.heading) flags |= kRunHeading;
    if (s.bold) flags |= kRunBold;
    if (s.code) flags |= kRunCode;
    if (s.pre) flags |= kRunPre;
    if (s.underline) flags |= kRunUnderline;
    if (s.strike) flags |= kRunStrike;
    if (s.background && s.background_block) flags |= kRunBlockBg;
    return flags;
  }

  // Turns the text collected since the last tag into a run in the current style.
  void flush() {
    if (chunk_.empty()) return;
    const Style& s = cur();
    StringView text = chunk_.view();
    const uint8_t breaks = out_.runs_.count() == 0 ? 0 : pending_;
    if (!s.pre && breaks > 0 && text[0] == ' ') text = text.substr(1);   // (no blank at the start of a line)
    String transformed(out_.allocator());
    if (s.transform && !text.empty()) {
      for (uint64_t k = 0; k < text.size(); ++k) {
        char c = text[k];
        if (s.transform == 1 && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (s.transform == 2 && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        failed_ |= !transformed.push_back(c);
      }
      text = transformed.view();
    }
    if (!text.empty()) {
      failed_ |= !out_.add_run(text, static_cast<uint8_t>(run_flags(s)), s.heading, indent(), breaks, cur_link(), s.align, s.color, s.background, cur_node());
      pending_ = 0;
    }
    chunk_.clear();
  }

  void block(uint8_t breaks) {
    flush();
    if (breaks > pending_) pending_ = breaks;
    ended_space_ = true;
  }

  // Text that is not in the page: a list marker, a table divider, an image's description.
  void literal(StringView text, uint8_t at_indent) {
    flush();
    if (text.empty() || hidden()) return;
    const Style& s = cur();
    const uint8_t breaks = out_.runs_.count() == 0 ? 0 : pending_;
    failed_ |= !out_.add_run(text, static_cast<uint8_t>(run_flags(s) & ~kRunPre), 0, at_indent, breaks, cur_link(), s.align, s.color, s.background, cur_node());
    pending_ = 0;
    ended_space_ = text[text.size() - 1] == ' ';
  }

  void pop_element() {
    flush();
    const uint64_t last = stack_.count() - 1;
    const Element e = stack_[last];
    stack_.remove_at(last);
    if (e.node->is_tag("dialog") && !hidden()) {
      block(2);
      failed_ |= !out_.add_run(StringView(), kRunRule, 0, 0, pending_, -1, 0, 0, 0, e.node->serial);
      pending_ = 0;
    }
    if (e.style.display != Display::Inline && !hidden()) block(static_cast<uint8_t>(1 + e.style.margin_bottom + e.style.padding_bottom));
  }

  // Starts a node; returns whether its children are to be visited (display none is not shown, children and all).
  bool enter(const dom::Node* n) {
    if (n->type == dom::NodeType::Text) {
      text(n);
      return false;
    }
    if (n->type != dom::NodeType::Element) return n->type == dom::NodeType::Fragment;
    flush();
    Style s = derive_style(cur(), n, sheet_);
    if (s.display == Display::None) return false;
    if (n->is_tag("iframe") && !options_.frames) return false;
    if (n->is_tag("dialog") && (!dom::has_attribute(n, "open") || ((n->form_state & dom::kFormModal) && !modal_pass_))) return false;   // (an open modal dialog is drawn first, apart)
    const bool is_body = tag_in(n, {"body", "html"});
    if (s.own_background) {
      if (is_body) {
        if (s.own_background_color) out_.page_background_ = s.own_background_color;
      } else {
        s.background = s.own_background_color;
        s.background_block = s.display != Display::Inline;
      }
    }
    if (n == box_root_ && box_style_) {
      s = *box_style_;   // (a box made by the caller: only what is inside it is drawn here)
    } else if (!hidden() && wants_box(n, s) && box_depth_ < kMaxBoxDepth && render_box(n, s)) {
      return false;
    }
    Element e;
    e.style = s;
    e.node = n;
    e.link = cur_link();
    if (tag_in(n, {"a"})) {
      StringView href;
      e.link = dom::get_attribute(n, "href", &href) && !href.empty() ? out_.add_link(href) : -1;
    }
    if (!hidden() && s.display != Display::Inline) block(static_cast<uint8_t>(1 + s.margin_top + s.padding_top));
    failed_ |= !stack_.push_back(e);
    if (n->is_tag("dialog") && !hidden()) {
      Style& ds = stack_[stack_.count() - 1].style;
      if (!ds.own_background) {
        ds.background = 0xFFEEEEEEu;
        ds.background_block = true;
      }
      ds.margin_left = static_cast<uint8_t>(ds.margin_left + 2);
      ds.indent = static_cast<uint8_t>(ds.indent + 2 < kMaxIndent ? ds.indent + 2 : kMaxIndent);
      block(2);
      failed_ |= !out_.add_run(StringView(), kRunRule, 0, 0, out_.runs_.count() == 0 ? 0 : pending_, -1, 0, 0, 0, n->serial);
      pending_ = 0;
      block(1);
    } else if (n->is_tag("details") && !hidden()) {
      if (!dom::has_attribute(n, "open")) {
        const dom::Node* summary = nullptr;
        for (const dom::Node* c = n->first_child; c && !summary; c = c->next)
          if (c->is_tag("summary")) summary = c;
        if (summary) walk(summary);
        else literal("> Details", indent());
        return false;
      }
    } else if (n->is_tag("summary") && !hidden() && n->parent && n->parent->is_tag("details")) {
      const dom::Node* first = nullptr;
      for (const dom::Node* c = n->parent->first_child; c && !first; c = c->next)
        if (c->is_tag("summary")) first = c;
      if (first == n) {
        if (!ended_space_) literal(" ", indent());
        literal(dom::has_attribute(n->parent, "open") ? "v " : "> ", indent());
      }
    }
    if (tag_in(n, {"tr"})) {
      stack_[stack_.count() - 1].items = 0;
    } else if (tag_in(n, {"td", "th"})) {
      flush();
      uint32_t cell = 0;
      for (uint64_t i = stack_.count() - 1; i > 0; --i)
        if (stack_[i - 1].node->is_tag("tr")) {
          cell = stack_[i - 1].items++;
          break;
        }
      if (cell > 0 && !hidden()) {
        chunk_.append(" | ");
        ended_space_ = true;
      }
    } else if (tag_in(n, {"li"}) && s.display == Display::ListItem && !s.list_none && !hidden()) {
      block(1);
      String marker(out_.allocator());
      uint32_t number = 0;
      bool ordered = false;
      for (uint64_t i = stack_.count() - 1; i > 0; --i) {
        const dom::Node* list = stack_[i - 1].node;
        if (list->is_tag("ul") || list->is_tag("ol") || list->is_tag("menu")) {
          ordered = list->is_tag("ol");
          number = ++stack_[i - 1].items;
          if (ordered) {
            StringView start;
            int64_t first = 1;
            if (dom::get_attribute(list, "start", &start)) {
              uint64_t v = 0;
              if (start.parse_uint(&v)) first = static_cast<int64_t>(v);
            }
            number = static_cast<uint32_t>(first + number - 1);
          }
          break;
        }
      }
      if (ordered) {
        marker.append_uint(number);
        marker.append(". ");
      } else {
        marker.append("* ");
      }
      const uint32_t here = s.indent;
      literal(marker.view(), static_cast<uint8_t>(here >= 3 ? here - 3 : 0));
    }
    if (objects_ && !hidden() && tag_in(n, {"img", "video", "audio", "canvas", "svg"})) {
      ObjectInfo info;
      if (objects_->object_info(n, &info) && info.kind != ObjectKind::None && (info.ready || info.kind != ObjectKind::Image)) {
        object_run(n, info);
        return false;   // (a video's or canvas's fallback content is not shown)
      }
      if (n->is_tag("audio")) return false;   // (nor is an audio's: without controls it shows nothing at all)
    }
    if (!hidden() && tag_in(n, {"input", "textarea", "select", "button", "progress", "meter"})) {
      control(n);
      return false;   // (what is inside a control is its value, not page text)
    }
    if (n->is_tag("iframe")) {
      if (!hidden()) frame_content(n);
      return false;   // (the text inside an iframe is for browsers that have none)
    }
    if (!hidden() && n->is_tag("table") && table_depth_ < 5 && render_table(n)) return false;   // (the table is drawn: its rows and cells are not visited again)
    if (dom::is_void_tag(n->name.view())) void_action(n);
    return true;
  }

  // An <iframe>: its page is drawn between two rules, a little in from the left.
  void frame_content(const dom::Node* n) {
    uint32_t tag = 0;
    const HtmlDocument* d = options_.frames->frame_document(n, &tag);
    if (!d) return;
    block(2);
    failed_ |= !out_.add_run(StringView(), kRunRule, 0, 0, out_.runs_.count() == 0 ? 0 : pending_, -1, 0, 0, 0, n->serial);
    pending_ = 0;
    List<int32_t> links(out_.allocator());
    for (uint32_t i = 0; i < d->link_count(); ++i) links.push_back(-2);
    bool first = true;
    for (uint64_t i = 0; i < d->run_count() && !failed_; ++i) {
      const Run& r = d->run(i);
      const uint32_t indent_here = r.indent + indent() + 2 < kMaxIndent ? r.indent + indent() + 2 : kMaxIndent;
      const uint8_t breaks = first ? 1 : r.breaks;
      first = false;
      const uint32_t node = r.node ? (tag << 24) | r.node : 0;
      if (r.flags & kRunRule) {
        failed_ |= !out_.add_run(StringView(), kRunRule, 0, static_cast<uint8_t>(indent_here), breaks, -1, 0, r.color, 0, node);
        continue;
      }
      int32_t link = -1;
      if (r.link >= 0 && static_cast<uint32_t>(r.link) < links.count()) {
        if (links[static_cast<uint32_t>(r.link)] == -2) links[static_cast<uint32_t>(r.link)] = out_.add_link(d->link(static_cast<uint32_t>(r.link)));
        link = links[static_cast<uint32_t>(r.link)];
      }
      if (r.object) {   // (a picture, video or canvas of the frame shows as what it is)
        failed_ |= !out_.add_run(r.object == 2 ? "[video]" : r.object == 3 ? "[canvas]" : r.object == 4 ? "[audio]" : "[image]", 0, 0, static_cast<uint8_t>(indent_here), breaks, link, 0, 0,
                                 r.background, node);
        continue;
      }
      const StringView text = d->run_text(i);
      if (text.empty()) continue;
      failed_ |= !out_.add_run(text, r.flags, r.level, static_cast<uint8_t>(indent_here), breaks, link, r.align, r.color, r.background, node);
      out_.runs_[out_.runs_.count() - 1].exact = r.exact;
    }
    block(2);
    failed_ |= !out_.add_run(StringView(), kRunRule, 0, 0, pending_, -1, 0, 0, 0, n->serial);
    pending_ = 0;
    block(1);
  }

  // Visits a node and what is below it (the cells of a table are drawn this way).
  void walk(const dom::Node* n) {
    const bool descend = enter(n);
    if (descend)
      for (const dom::Node* c = n->first_child; c && !failed_; c = c->next) walk(c);
    if (stack_.count() && stack_[stack_.count() - 1].node == n) pop_element();
  }

  // --- Tables
  // A table is a grid of cells that are drawn on their own, one after the other, then fitted together: the columns take the width of what is in them (all
  // of the page's width, if there is not enough, with the text of the cells wrapped), colspan and rowspan join cells, thead comes first and tfoot last, the
  // caption is above, and a border attribute draws the lines.
  struct Seg {
    explicit Seg(allocators::IAllocator& a) : text(a) {}
    String text;
    uint8_t flags = 0, level = 0;
    uint32_t color = 0, background = 0, node = 0;
    int32_t link = -1;
  };
  struct Cell {
    explicit Cell(allocators::IAllocator& a) : segs(a), line_end(a), link_map(a) {}
    const dom::Node* node = nullptr;
    uint32_t row = 0, col = 0, rowspan = 1, colspan = 1, natural = 0, min_word = 0, width = 0;
    Style style;
    uint8_t valign = 0;   // 0 top, 1 middle, 2 bottom
    UniquePointer<HtmlDocument> doc;
    List<Seg> segs;
    List<uint32_t> line_end;   // line i is segs[line_end[i - 1] .. line_end[i])
    List<int32_t> link_map;    // the cell's links, as links of the page (-2: not added yet)
    uint32_t lines() const { return static_cast<uint32_t>(line_end.count()); }
  };
  static constexpr uint32_t kMaxTableColumns = 24, kMaxTableRows = 400, kMaxTableCells = 2000;

  Style style_in_table(const dom::Node* n, const dom::Node* table, const Style& table_style) {
    Style s = derive_style(n->parent == table ? table_style : style_in_table(n->parent, table, table_style), n, sheet_);
    if (s.own_background) s.background = s.own_background_color;
    s.background_block = false;
    return s;
  }

  uint32_t seg_line_width(const Cell& c, uint32_t line) const {
    uint32_t w = 0;
    for (uint64_t i = line ? c.line_end[line - 1] : 0; i < c.line_end[line]; ++i) w += static_cast<uint32_t>(c.segs[i].text.size());
    return w;
  }

  // Fits the text of a cell (drawn into its own document) into lines `width` columns wide.
  bool wrap_cell(Cell& c, uint32_t width) {
    allocators::IAllocator& a = out_.allocator();
    c.segs.clear();
    c.line_end.clear();
    const HtmlDocument& d = *c.doc;
    uint64_t line_start = 0;
    uint32_t used = 0;
    bool first_content = true, carry_space = false;
    auto has = [&]() { return c.segs.count() > line_start; };
    auto close_line = [&]() {
      failed_ |= !c.line_end.push_back(static_cast<uint32_t>(c.segs.count()));
      line_start = c.segs.count();
      used = 0;
    };
    auto make_seg = [&](const Run& run, StringView text, bool plain) {
      Seg s(a);
      s.text.assign(text);
      if (!plain) {
        s.flags = static_cast<uint8_t>(run.flags & ~(kRunBlockBg | kRunPre));
        s.level = run.level;
        s.color = run.color;
        s.background = run.background;
        s.node = run.node;
        if (run.link >= 0) {
          const uint32_t l = static_cast<uint32_t>(run.link);
          if (l < c.link_map.count()) {
            if (c.link_map[l] == -2) c.link_map[l] = out_.add_link(d.link(l));
            s.link = c.link_map[l];
          }
        }
      } else {
        s.node = run.node;
      }
      failed_ |= !c.segs.push_back(util::move(s));
    };
    int64_t last_run = -1;
    for (uint64_t r = 0; r < d.run_count() && !failed_; ++r) {
      const Run& run = d.run(r);
      const StringView text = d.run_text(r);
      if (run.object || text.empty()) continue;
      if (run.breaks >= 1 && has()) close_line();
      if (run.breaks >= 2 && !first_content) close_line();
      first_content = false;
      if ((run.flags & kRunPre) || run.exact) {
        uint64_t i = 0;
        bool first = true;
        while (i <= text.size()) {
          uint64_t end = text.find('\n', i);
          if (end == StringView::npos) end = text.size();
          if (!first) close_line();
          StringView part = text.substr(i, end - i);
          if (part.size() > width) part = part.first(width);
          if (!part.empty()) make_seg(run, part, false);
          used += static_cast<uint32_t>(part.size());
          first = false;
          i = end + 1;
        }
        last_run = static_cast<int64_t>(r);
        carry_space = false;
        continue;
      }
      bool space_before = carry_space;
      uint64_t i = 0;
      while (i < text.size()) {
        if (text[i] == ' ') {
          space_before = true;
          ++i;
          continue;
        }
        uint64_t end = i;
        while (end < text.size() && text[end] != ' ') ++end;
        StringView word = text.substr(i, end - i);
        i = end;
        while (!word.empty()) {
          const uint32_t chunk = static_cast<uint32_t>(word.size() > width ? width : word.size());
          const bool gap = has() && space_before;
          if (has() && used + (gap ? 1 : 0) + chunk > width) {
            close_line();
            space_before = false;
          }
          if (has() && space_before) {
            if (last_run == static_cast<int64_t>(r)) {
              c.segs[c.segs.count() - 1].text.push_back(' ');
            } else {
              make_seg(run, " ", true);
            }
            ++used;
          }
          if (has() && last_run == static_cast<int64_t>(r)) c.segs[c.segs.count() - 1].text.append(word.first(chunk));
          else make_seg(run, word.first(chunk), false);
          used += chunk;
          last_run = static_cast<int64_t>(r);
          space_before = false;
          word = word.substr(chunk);
          if (!word.empty()) close_line();
        }
      }
      carry_space = text[text.size() - 1] == ' ' || (text.size() && space_before && has());
      last_run = static_cast<int64_t>(r);
    }
    if (has() || c.line_end.count() == 0) close_line();
    return !failed_;
  }

  // The pieces of the table line being made; when it is finished the spaces at its end are dropped and the pieces become runs.
  struct LinePiece {
    explicit LinePiece(allocators::IAllocator& a) : text(a) {}
    String text;
    uint8_t flags = 0, level = 0;
    uint32_t color = 0, background = 0, node = 0;
    int32_t link = -1;
  };

  void table_piece(List<LinePiece>& line, StringView text, uint8_t flags, uint8_t level, uint32_t color, uint32_t background, int32_t link, uint32_t node) {
    if (text.empty()) return;
    LinePiece p(out_.allocator());
    p.text.assign(text);
    p.flags = flags;
    p.level = level;
    p.color = color;
    p.background = background;
    p.link = link;
    p.node = node;
    failed_ |= !line.push_back(util::move(p));
  }

  void table_spaces(List<LinePiece>& line, uint32_t n, uint32_t background, uint32_t node) {
    if (n == 0) return;
    String pad(out_.allocator());
    for (uint32_t i = 0; i < n; ++i) pad.push_back(' ');
    table_piece(line, pad.view(), 0, 0, 0, background, -1, node);
  }

  void table_line(List<LinePiece>& line, bool trim) {
    while (trim && line.count() > 0) {
      LinePiece& last = line[line.count() - 1];
      while (last.text.size() > 0 && last.text[last.text.size() - 1] == ' ') last.text.truncate(last.text.size() - 1);
      if (last.text.size() > 0) break;
      line.remove_at(line.count() - 1);
    }
    for (uint64_t i = 0; i < line.count(); ++i) {
      const LinePiece& p = line[i];
      const uint8_t breaks = i == 0 ? (out_.runs_.count() == 0 ? 0 : pending_ > 1 ? pending_ : 1) : 0;
      failed_ |= !out_.add_run(p.text.view(), p.flags, p.level, indent(), breaks, p.link, 0, p.color, p.background, p.node);
      out_.runs_[out_.runs_.count() - 1].exact = 1;
    }
    if (line.count() == 0) {   // (an empty line still takes its place)
      failed_ |= !out_.add_run(" ", 0, 0, indent(), out_.runs_.count() == 0 ? 0 : pending_ > 1 ? pending_ : 1, -1, 0, 0, 0, 0);
      out_.runs_[out_.runs_.count() - 1].exact = 1;
    }
    pending_ = 0;
    ended_space_ = false;
    line.clear();
  }

  bool render_table(const dom::Node* table) {
    allocators::IAllocator& a = out_.allocator();
    const Style table_style = cur();
    // the rows, in the order they are drawn
    const dom::Node* caption = nullptr;
    List<const dom::Node*> head(a), body(a), foot(a), rows(a);
    for (const dom::Node* c = table->first_child; c; c = c->next) {
      if (!c->is_element()) continue;
      if (c->is_tag("caption") && !caption) caption = c;
      else if (c->is_tag("tr")) body.push_back(c);
      else if (c->is_tag("thead") || c->is_tag("tbody") || c->is_tag("tfoot")) {
        List<const dom::Node*>& into = c->is_tag("thead") ? head : c->is_tag("tfoot") ? foot : body;
        for (const dom::Node* r = c->first_child; r; r = r->next)
          if (r->is_tag("tr")) into.push_back(r);
      }
    }
    for (List<const dom::Node*>* list : {&head, &body, &foot})
      for (uint64_t i = 0; i < list->count(); ++i) rows.push_back((*list)[i]);
    if (rows.count() == 0 || rows.count() > kMaxTableRows) return false;
    // the cells, each in the first free place of its row
    List<UniquePointer<Cell>> cells(a);
    List<int32_t> occupied(a);
    for (uint64_t i = 0; i < rows.count() * kMaxTableColumns; ++i) occupied.push_back(0);
    uint32_t ncols = 0;
    for (uint32_t r = 0; r < rows.count(); ++r) {
      uint32_t col = 0;
      for (const dom::Node* c = rows[r]->first_child; c; c = c->next) {
        if (!(c->is_tag("td") || c->is_tag("th"))) continue;
        while (col < kMaxTableColumns && occupied[r * kMaxTableColumns + col] != 0) ++col;
        if (col >= kMaxTableColumns || cells.count() >= kMaxTableCells) break;
        auto cell = UniquePointer<Cell>::make(a, a);
        if (!cell) return false;
        cell->node = c;
        cell->row = r;
        cell->col = col;
        uint64_t v;
        if (attr(c, "colspan").trimmed().parse_uint(&v) && v > 1) cell->colspan = static_cast<uint32_t>(v > kMaxTableColumns ? kMaxTableColumns : v);
        if (attr(c, "rowspan").trimmed().parse_uint(&v) && v > 1) cell->rowspan = static_cast<uint32_t>(v > 64 ? 64 : v);
        if (col + cell->colspan > kMaxTableColumns) cell->colspan = kMaxTableColumns - col;
        if (r + cell->rowspan > rows.count()) cell->rowspan = static_cast<uint32_t>(rows.count()) - r;
        const int32_t id = static_cast<int32_t>(cells.count()) + 1;
        for (uint32_t y = 0; y < cell->rowspan; ++y)
          for (uint32_t x = 0; x < cell->colspan; ++x) occupied[(r + y) * kMaxTableColumns + col + x] = id;
        col += cell->colspan;
        if (col > ncols) ncols = col;
        const StringView va = attr(c, "valign").trimmed();
        cell->valign = va.equals_ignore_case("middle") ? 1 : va.equals_ignore_case("bottom") ? 2 : 0;
        cells.push_back(util::move(cell));
      }
    }
    if (cells.count() == 0 || ncols == 0) return false;
    // each cell is drawn into a document of its own
    for (uint64_t i = 0; i < cells.count(); ++i) {
      Cell& c = *cells[i];
      c.doc = UniquePointer<HtmlDocument>::make(a, a);
      if (!c.doc) return false;
      DocumentRenderer sub(*c.doc, sheet_, options_);
      sub.root_ = style_in_table(c.node->parent, table, table_style);
      sub.table_depth_ = table_depth_ + 1;
      sub.walk(c.node);
      sub.flush();
      if (sub.failed_) return false;
      c.style = style_in_table(c.node, table, table_style);
      for (uint64_t k = 0; k < c.doc->run_count(); ++k)
        if (c.doc->run(k).object) return false;   // (a picture in a cell cannot be fitted into text columns: the table is left to flow)
      for (uint32_t k = 0; k < c.doc->link_count(); ++k) c.link_map.push_back(-2);
      if (!wrap_cell(c, 0x7FFFFFFF)) return false;
      for (uint32_t l = 0; l < c.lines(); ++l) c.natural = seg_line_width(c, l) > c.natural ? seg_line_width(c, l) : c.natural;
      for (uint64_t k = 0; k < c.doc->run_count(); ++k) {
        const Run& run = c.doc->run(k);
        const StringView t = c.doc->run_text(k);
        uint64_t word = 0;
        for (uint64_t j = 0; j <= t.size(); ++j) {
          const bool cut = j == t.size() || t[j] == ' ' || ((run.flags & kRunPre) && t[j] == '\n') || (run.exact && t[j] == '\n');
          if (cut) {
            if (word > c.min_word) c.min_word = static_cast<uint32_t>(word);
            word = 0;
          } else {
            ++word;
          }
        }
      }
    }
    // the widths of the columns
    StringView border_value;
    const bool grid = dom::get_attribute(table, "border", &border_value) && !border_value.trimmed().empty() && !border_value.trimmed().equals("0");
    const uint32_t page_columns = container_cols_;
    const uint32_t frame = grid ? ncols * 3 + 1 : (ncols - 1) * 3;
    const uint32_t room = page_columns > indent() + frame + ncols ? page_columns - indent() - frame : ncols;
    List<uint32_t> natural(a), minimum(a), width(a);
    for (uint32_t c = 0; c < ncols; ++c) {
      natural.push_back(1);
      minimum.push_back(1);
      width.push_back(1);
    }
    for (uint64_t i = 0; i < cells.count(); ++i)
      if (cells[i]->colspan == 1) {
        const Cell& c = *cells[i];
        if (c.natural > natural[c.col]) natural[c.col] = c.natural;
        if (c.min_word > minimum[c.col]) minimum[c.col] = c.min_word;
      }
    for (uint64_t i = 0; i < cells.count(); ++i) {
      const Cell& c = *cells[i];
      if (c.colspan == 1) continue;
      uint32_t have_nat = 0, have_min = 0;
      for (uint32_t x = 0; x < c.colspan; ++x) have_nat += natural[c.col + x], have_min += minimum[c.col + x];
      have_nat += 3 * (c.colspan - 1);
      have_min += 3 * (c.colspan - 1);
      if (c.natural > have_nat) natural[c.col + c.colspan - 1] += c.natural - have_nat;
      if (c.min_word > have_min) minimum[c.col + c.colspan - 1] += c.min_word - have_min;
    }
    uint32_t total_nat = 0, total_min = 0;
    for (uint32_t c = 0; c < ncols; ++c) total_nat += natural[c], total_min += minimum[c];
    for (uint32_t c = 0; c < ncols; ++c) width[c] = natural[c];
    if (total_nat > room) {
      uint32_t extra = room > total_min ? room - total_min : 0, flexible = total_nat - total_min;
      for (uint32_t c = 0; c < ncols; ++c) {
        const uint32_t want = natural[c] - minimum[c];
        const uint32_t give = flexible ? static_cast<uint32_t>(static_cast<uint64_t>(extra) * want / flexible) : 0;
        width[c] = minimum[c] + (give > want ? want : give);
      }
    }
    // the cells are fitted into their widths and the rows get their heights
    List<uint32_t> height(a);
    for (uint64_t r = 0; r < rows.count(); ++r) height.push_back(1);
    for (uint64_t i = 0; i < cells.count(); ++i) {
      Cell& c = *cells[i];
      c.width = 3 * (c.colspan - 1);
      for (uint32_t x = 0; x < c.colspan; ++x) c.width += width[c.col + x];
      if (!wrap_cell(c, c.width ? c.width : 1)) return false;
      if (c.rowspan == 1 && c.lines() > height[c.row]) height[c.row] = c.lines();
    }
    for (uint64_t i = 0; i < cells.count(); ++i) {
      const Cell& c = *cells[i];
      if (c.rowspan == 1) continue;
      uint32_t have = grid ? c.rowspan - 1 : 0;
      for (uint32_t y = 0; y < c.rowspan; ++y) have += height[c.row + y];
      if (c.lines() > have) height[c.row + c.rowspan - 1] += c.lines() - have;
    }
    List<uint32_t> top(a);   // the first line of each row (a rule above it is not counted)
    uint32_t y = grid ? 1 : 0;
    for (uint64_t r = 0; r < rows.count(); ++r) {
      top.push_back(y);
      y += height[r] + (grid ? 1 : 0);
    }
    const uint32_t total_lines = y;
    // --- drawing
    if (caption) {
      walk(caption);
      block(1);
    }
    block(2);
    const uint32_t table_node = table->serial;
    List<LinePiece> line_pieces(a);
    for (uint32_t line = 0; line < total_lines && !failed_; ++line) {
      // is this line a rule (between rows)? `below` is the row under it
      bool rule = false;
      uint32_t row = 0, below = 0;
      for (uint32_t r = 0; r < rows.count(); ++r) {
        if (grid && line + 1 == top[r]) {
          rule = true;
          below = r;
        }
        if (line >= top[r] && line < top[r] + height[r]) row = r;
      }
      if (grid && line + 1 == total_lines) {
        rule = true;
        below = static_cast<uint32_t>(rows.count());
      }
      if (rule) {
        String text(a);
        text.push_back('+');
        for (uint32_t c = 0; c < ncols; ++c) {
          bool continues = false;   // (a cell that goes on across this rule)
          if (below > 0 && below < rows.count()) {
            const int32_t u = occupied[(below - 1) * kMaxTableColumns + c], d = occupied[below * kMaxTableColumns + c];
            continues = u != 0 && u == d;
          }
          for (uint32_t i = 0; i < width[c] + 2; ++i) text.push_back(continues ? ' ' : '-');
          text.push_back('+');
        }
        table_piece(line_pieces, text.view(), 0, 0, 0, 0, -1, table_node);
        table_line(line_pieces, false);
        continue;
      }
      if (grid) table_piece(line_pieces, "| ", 0, 0, 0, 0, -1, table_node);
      for (uint32_t c = 0; c < ncols && !failed_;) {
        const int32_t id = occupied[row * kMaxTableColumns + c];
        if (id == 0) {
          table_spaces(line_pieces, width[c], 0, table_node);
          ++c;
        } else {
          const Cell& cell = *cells[static_cast<uint64_t>(id - 1)];
          const uint32_t span_lines = top[cell.row + cell.rowspan - 1] + height[cell.row + cell.rowspan - 1] - top[cell.row];
          const uint32_t free_lines = span_lines > cell.lines() ? span_lines - cell.lines() : 0;
          const uint32_t offset = cell.valign == 1 ? free_lines / 2 : cell.valign == 2 ? free_lines : 0;
          const int64_t at = static_cast<int64_t>(line) - top[cell.row] - offset;
          const uint32_t bg = cell.style.background;
          if (at >= 0 && at < cell.lines()) {
            const uint32_t used = seg_line_width(cell, static_cast<uint32_t>(at));
            const uint32_t room_left = cell.width > used ? cell.width - used : 0;
            const uint32_t before = cell.style.align == 1 ? room_left / 2 : cell.style.align == 2 ? room_left : 0;
            table_spaces(line_pieces, before, bg, cell.node->serial);
            for (uint64_t i = at ? cell.line_end[static_cast<uint32_t>(at) - 1] : 0; i < cell.line_end[static_cast<uint32_t>(at)]; ++i) {
              const Seg& sg = cell.segs[i];
              table_piece(line_pieces, sg.text.view(), sg.flags, sg.level, sg.color, sg.background ? sg.background : bg, sg.link, sg.node ? sg.node : cell.node->serial);
            }
            table_spaces(line_pieces, room_left - before, bg, cell.node->serial);
          } else {
            table_spaces(line_pieces, cell.width, bg, cell.node->serial);
          }
          c += cell.colspan;
        }
        if (c < ncols) table_piece(line_pieces, " | ", 0, 0, 0, 0, -1, table_node);
      }
      if (grid) table_piece(line_pieces, " |", 0, 0, 0, 0, -1, table_node);
      table_line(line_pieces, !grid);
    }
    block(2);
    return !failed_;
  }

  // --- Forms
  static constexpr uint32_t kFieldBg = 0xFFF0F0F0u, kFocusBg = 0xFFDCE8FFu, kButtonBg = 0xFFD4D4D4u, kSelectionBg = 0xFF3060C0u, kInk = 0xFF202020u, kGrayInk = 0xFF808080u, kListBg = 0xFFF6F6F6u,
                            kPickedBg = 0xFFBCD4F8u;

  static uint32_t number_attr(const dom::Node* n, const char* name, uint32_t fallback, uint32_t lo, uint32_t hi) {
    uint64_t v;
    if (!attr(n, name).trimmed().parse_uint(&v) || v == 0) return fallback;
    return static_cast<uint32_t>(v < lo ? lo : v > hi ? hi : v);
  }

  // Printable ASCII of a value (the screen has no more), at most `limit` bytes.
  static void shown(StringView v, bool mask, String* out) {
    for (uint64_t i = 0; i < v.size();) {
      const uint8_t c = static_cast<uint8_t>(v[i]);
      if (c < 0x80) {
        out->push_back(mask ? '*' : c >= 32 && c < 127 ? static_cast<char>(c) : ' ');
        ++i;
        continue;
      }
      const char* look = mask ? "*" : dom::ascii_look_alike(dom::next_code_point(v, &i));
      if (mask) ++i;
      out->append(look ? StringView(look) : StringView("?"));
    }
  }

  // One character on the screen for each byte of a value (what the caret and the selection count in): the printable ones, '*' for a password, '?' for the rest.
  static void one_for_one(StringView v, bool mask, String* out) {
    for (uint64_t i = 0; i < v.size(); ++i) {
      const uint8_t c = static_cast<uint8_t>(v[i]);
      out->push_back(mask ? '*' : c >= 32 && c < 127 ? static_cast<char>(c) : '?');
    }
  }

  // The values of a <datalist> that fit what has been typed, as lines under the field (a click on one takes it).
  void suggestions(const dom::Node* input, StringView typed) {
    const dom::Node* list = input->owner->element_by_id(attr(input, "list"));
    if (!list || !list->is_tag("datalist")) return;
    uint32_t shown_count = 0;
    for (const dom::Node* o = list->first_child; o && shown_count < 6; o = dom::next_in_tree(o, list)) {
      if (!o->is_tag("option") || dom::is_disabled(o)) continue;
      String value(out_.allocator()), label(out_.allocator()), line(out_.allocator());
      dom::option_value(o, &value);
      StringView l;
      if (dom::get_attribute(o, "label", &l) && !l.empty()) label.assign(l);
      bool fits = typed.empty();
      for (uint64_t k = 0; !fits && k + typed.size() <= value.size(); ++k) fits = value.view().substr(k, typed.size()).equals_ignore_case(typed);
      for (uint64_t k = 0; !fits && k + typed.size() <= label.size(); ++k) fits = label.view().substr(k, typed.size()).equals_ignore_case(typed);
      if (!fits || value.view().equals(typed)) continue;
      line.append("  ");
      one_for_one(value.view(), false, &line);
      if (!label.empty()) {
        line.append(" - ");
        one_for_one(label.view(), false, &line);
      }
      pad(&line, 22);
      control_run(o, line.view(), kInk, kListBg, false, true);
      ++shown_count;
    }
  }

  static void pad(String* s, uint64_t width) {
    while (s->size() < width) s->push_back(' ');
  }

  // One piece of a control: a run of its own that a click on goes to `owner`.
  // `join`: the text goes on right after the one before it (the pieces of one field).
  void control_run(const dom::Node* owner, StringView text, uint32_t color, uint32_t background, bool bold, bool new_line, bool join = false) {
    flush();
    if (text.empty()) return;
    const Style& s = cur();
    if (!new_line && !join && !ended_space_) literal(" ", indent());
    const uint8_t breaks = join ? 0 : out_.runs_.count() == 0 ? 0 : new_line ? (pending_ > 1 ? pending_ : 1) : pending_;
    failed_ |= !out_.add_run(text, static_cast<uint8_t>(kRunPre | (bold ? kRunBold : 0)), 0, indent(), breaks, -1, s.align, color, background, owner->serial);
    pending_ = 0;
    ended_space_ = false;
  }

  void control(const dom::Node* n) {
    const bool focused = options_.focus == n->serial, disabled = dom::is_disabled(n);
    const uint32_t ink = disabled ? kGrayInk : kInk, field_bg = focused ? kFocusBg : kFieldBg;
    String text(out_.allocator()), value(out_.allocator());
    if (n->is_tag("input")) {
      const dom::InputType t = dom::input_type(n);
      switch (t) {
        case dom::InputType::Hidden: return;
        case dom::InputType::Checkbox:
          control_run(n, (n->form_state & dom::kFormIndeterminate) ? "[-]" : dom::is_checked(n) ? "[x]" : "[ ]", ink, focused ? kFocusBg : 0, false, false);
          break;
        case dom::InputType::Radio:
          control_run(n, dom::is_checked(n) ? "(*)" : "( )", ink, focused ? kFocusBg : 0, false, false);
          break;
        case dom::InputType::Submit:
        case dom::InputType::Reset:
        case dom::InputType::Button:
        case dom::InputType::Image: {
          dom::get_value(n, &value);
          if (t == dom::InputType::Image) value.assign(attr(n, "alt").empty() ? StringView("Submit") : attr(n, "alt"));
          else if (value.empty() && !dom::has_attribute(n, "value")) value.assign(t == dom::InputType::Submit ? "Submit" : t == dom::InputType::Reset ? "Reset" : "");
          text.append("[ ");
          shown(value.view(), false, &text);
          text.append(" ]");
          control_run(n, text.view(), ink, focused ? kFocusBg : kButtonBg, true, false);
          break;
        }
        case dom::InputType::File:
        {
          const uint32_t count = dom::file_count(n);
          control_run(n, "[ Choose File ]", ink, focused ? kFocusBg : kButtonBg, false, false);
          String names(out_.allocator());
          if (count == 0) names.append(" No file chosen");
          for (uint32_t k = 0; k < count; ++k) {
            StringView name, type, bytes;
            dom::file_at(n, k, &name, &type, &bytes);
            names.append(k ? ", " : " ");
            one_for_one(name, false, &names);
          }
          control_run(n, names.view(), count ? ink : kGrayInk, 0, false, false, true);
        }
          break;
        case dom::InputType::Color: {
          dom::get_value(n, &value);
          uint32_t rgb = 0;
          for (uint64_t i = 1; i < value.size(); ++i) {
            const char c = value[i];
            rgb = rgb << 4 | static_cast<uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10);
          }
          const uint32_t luma = (((rgb >> 16) & 255) * 299 + ((rgb >> 8) & 255) * 587 + (rgb & 255) * 114) / 1000;
          text.append("[");
          shown(value.view(), false, &text);
          text.append("]");
          control_run(n, text.view(), luma < 128 ? 0xFFFFFFFFu : 0xFF000000u, 0xFF000000u | rgb, false, false);
          break;
        }
        case dom::InputType::Range: {
          dom::get_value(n, &value);
          double lo = 0, hi = 100, v = 50;
          dom::parse_number(attr(n, "min").trimmed(), &lo);
          dom::parse_number(attr(n, "max").trimmed(), &hi);
          dom::parse_number(value.view(), &v);
          const uint32_t width = 14;
          uint32_t at = hi > lo ? static_cast<uint32_t>((v - lo) / (hi - lo) * (width - 1) + 0.5) : 0;
          if (at >= width) at = width - 1;
          text.append("[<");
          for (uint32_t i = 0; i < width; ++i) text.push_back(i == at ? 'o' : '-');
          text.append("> ");
          shown(value.view(), false, &text);
          text.append("]");
          control_run(n, text.view(), ink, field_bg, false, false);
          break;
        }
        default: {   // a field of text
          dom::get_value(n, &value);
          const uint32_t cols = number_attr(n, "size", 20, 4, 60);
          String content(out_.allocator());
          one_for_one(value.view(), t == dom::InputType::Password, &content);
          const bool placeholder = content.empty() && !focused && !attr(n, "placeholder").empty();
          if (placeholder) one_for_one(attr(n, "placeholder"), false, &content);
          uint32_t sel_start = 0, sel_end = 0, caret = 0;
          if (focused) dom::get_selection(n, &sel_start, &sel_end, &caret);
          const bool selection = focused && sel_start < sel_end;
          if (focused && !selection) {   // (the caret is a bar between two characters)
            String with(out_.allocator());
            with.append(content.view().first(caret));
            with.push_back('|');
            with.append(content.view().substr(caret));
            content.assign(with.view());
          }
          uint32_t shift = 0;
          if (content.size() > cols) {
            shift = focused && caret + 1 > cols ? caret + 1 - cols : 0;
            content.assign(content.view().substr(shift, cols));
          }
          pad(&content, cols);
          const uint32_t lo = selection ? (sel_start > shift ? sel_start - shift : 0) : 0, hi = selection ? (sel_end > shift ? sel_end - shift : 0) : 0;
          const uint32_t gray = placeholder ? kGrayInk : ink;
          if (selection && lo < hi && lo < content.size()) {   // the selected text is shown on its own colour
            const uint64_t end = hi < content.size() ? hi : content.size();
            control_run(n, "[", gray, field_bg, false, false);
            control_run(n, content.view().first(lo), gray, field_bg, false, false, true);
            control_run(n, content.view().substr(lo, end - lo), 0xFFFFFFFFu, kSelectionBg, false, false, true);
            control_run(n, content.view().substr(end), gray, field_bg, false, false, true);
            control_run(n, "]", gray, field_bg, false, false, true);
          } else {
            text.append("[");
            text.append(content.view());
            text.append("]");
            control_run(n, text.view(), gray, field_bg, false, false);
          }
          if (focused && n->is_tag("input") && !attr(n, "list").empty()) suggestions(n, value.view());
          break;
        }
      }
    } else if (n->is_tag("textarea")) {
      dom::get_value(n, &value);
      const uint32_t cols = number_attr(n, "cols", 24, 4, 80), rows = number_attr(n, "rows", 3, 1, 20);
      List<String> lines(out_.allocator());
      String line(out_.allocator());
      const bool placeholder = value.empty() && !focused && !attr(n, "placeholder").empty();
      const StringView source = placeholder ? attr(n, "placeholder") : value.view();
      uint32_t sel_start = 0, sel_end = 0, caret = 0;
      if (focused) dom::get_selection(n, &sel_start, &sel_end, &caret);
      uint64_t caret_line = 0, caret_col = 0;
      bool caret_seen = false;
      for (uint64_t i = 0; i <= source.size(); ++i) {
        if (focused && i == caret && !caret_seen) {
          caret_seen = true;
          if (line.size() >= cols && i < source.size() && source[i] != '\n') {   // (the caret starts the next line)
            failed_ |= !lines.push_back(util::move(line));
            line = String(out_.allocator());
          }
          caret_line = lines.count();
          caret_col = line.size();
        }
        if (i == source.size()) break;
        const char c = source[i];
        if (c == '\n' || line.size() >= cols) {
          failed_ |= !lines.push_back(util::move(line));
          line = String(out_.allocator());
          if (c == '\n') continue;
        }
        const uint8_t u = static_cast<uint8_t>(c);
        line.push_back(u >= 32 && u < 127 ? c : '?');
      }
      failed_ |= !lines.push_back(util::move(line));
      if (focused && caret_line < lines.count()) {
        String with(out_.allocator());
        with.append(lines[caret_line].view().first(caret_col));
        with.push_back('|');
        with.append(lines[caret_line].view().substr(caret_col));
        lines[caret_line].assign(with.view());
      }
      const uint64_t first = focused && caret_line >= rows ? caret_line - rows + 1 : 0;
      for (uint64_t r = 0; r < rows; ++r) {
        String row(out_.allocator());
        row.push_back('[');
        String body(out_.allocator());
        if (first + r < lines.count()) body.assign(lines[first + r].view());
        if (body.size() > cols + 1) body.assign(body.view().first(cols + 1));
        pad(&body, cols);
        row.append(body.view());
        row.push_back(']');
        control_run(n, row.view(), placeholder ? kGrayInk : ink, field_bg, false, r > 0);
      }
    } else if (n->is_tag("button")) {
      dom::text_content(n, &value);
      bool space = true;
      for (uint64_t i = 0; i < value.size();) {   // (the label, with white space collapsed)
        const char c = value[i];
        if (is_space(c)) {
          if (!space) text.push_back(' ');
          space = true;
          ++i;
        } else if (static_cast<uint8_t>(c) < 0x80) {
          text.push_back(c);
          space = false;
          ++i;
        } else {
          const char* look = dom::ascii_look_alike(dom::next_code_point(value.view(), &i));
          text.append(look ? StringView(look) : StringView("?"));
          space = false;
        }
      }
      while (text.size() && text[text.size() - 1] == ' ') text.truncate(text.size() - 1);
      String label(out_.allocator());
      label.append("[ ");
      label.append(text.view());
      label.append(" ]");
      control_run(n, label.view(), ink, focused ? kFocusBg : kButtonBg, true, false);
    } else if (n->is_tag("select")) {
      List<dom::Node*> options(out_.allocator());
      dom::select_options(n, &options);
      const int32_t chosen = dom::selected_index(n);
      const bool list = dom::select_is_list(n), open = options_.open_select == n->serial;
      uint64_t width = 4;
      List<String> labels(out_.allocator());
      for (uint64_t i = 0; i < options.count(); ++i) {
        String raw(out_.allocator()), label(out_.allocator());
        dom::option_text(options[i], &raw);
        shown(raw.view(), false, &label);
        if (label.size() > 40) label.truncate(40);
        if (label.size() > width) width = label.size();
        labels.push_back(util::move(label));
      }
      if (!list) {
        String box(out_.allocator());
        box.push_back('[');
        String shown_text(out_.allocator());
        if (chosen >= 0) shown_text.assign(labels[static_cast<uint64_t>(chosen)].view());
        pad(&shown_text, width);
        box.append(shown_text.view());
        box.append(open ? " ^]" : " v]");
        control_run(n, box.view(), ink, field_bg, false, false);
      }
      if (list || open) {
        const bool multiple = dom::has_attribute(n, "multiple");
        const uint64_t shown_rows = options.count();
        for (uint64_t i = 0; i < shown_rows && i < 24; ++i) {
          const bool on = multiple ? dom::is_checked(options[i]) : static_cast<int32_t>(i) == chosen;
          String row(out_.allocator());
          row.append(multiple ? (on ? "[x] " : "[ ] ") : (on ? "(*) " : "( ) "));
          row.append(labels[i].view());
          pad(&row, width + 4);
          control_run(options[i], row.view(), dom::is_disabled(options[i]) ? kGrayInk : ink, on ? kPickedBg : kListBg, false, true);
        }
      }
    } else if (n->is_tag("progress") || n->is_tag("meter")) {
      double v = 0, hi = 1, lo = 0;
      const bool known = dom::parse_number(attr(n, "value").trimmed(), &v);
      if (n->is_tag("meter")) {
        dom::parse_number(attr(n, "min").trimmed(), &lo);
        if (!dom::parse_number(attr(n, "max").trimmed(), &hi)) hi = 1;
      } else if (!dom::parse_number(attr(n, "max").trimmed(), &hi) || hi <= 0) {
        hi = 1;
      }
      const uint32_t width = 12;
      text.push_back('[');
      uint32_t filled = 0;
      if (known && hi > lo) filled = static_cast<uint32_t>((v < lo ? 0 : v > hi ? 1 : (v - lo) / (hi - lo)) * width + 0.5);
      for (uint32_t i = 0; i < width; ++i) text.push_back(!known && n->is_tag("progress") ? '.' : i < filled ? '#' : '-');
      text.push_back(']');
      if (known && n->is_tag("progress") && hi > lo) {
        text.push_back(' ');
        text.append_uint(static_cast<uint64_t>((v < 0 ? 0 : v > hi ? 1 : v / hi) * 100 + 0.5));
        text.push_back('%');
      }
      control_run(n, text.view(), ink, kFieldBg, false, false);
    }
    if (options_.invalid_node == n->serial && !options_.invalid_text.empty()) {
      String note(out_.allocator());
      note.append("! ");
      shown(options_.invalid_text, false, &note);
      control_run(n, note.view(), 0xFFC00000u, 0, true, false);
    }
  }

  // A width or height attribute: pixels, or a percentage.
  static void size_attribute(const dom::Node* n, const char* name, uint32_t* px, uint32_t* pct) {
    StringView v = attr(n, name).trimmed();
    if (v.empty()) return;
    uint64_t value = 0, i = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9' && value < 100000) value = value * 10 + static_cast<uint64_t>(v[i++] - '0');
    if (i == 0) return;
    if (i < v.size() && v[i] == '%') *pct = static_cast<uint32_t>(value > 100 ? 100 : value);
    else *px = static_cast<uint32_t>(value);
  }

  // A picture, video or canvas: one run that the layout gives a rectangle of the page.
  void object_run(const dom::Node* n, const ObjectInfo& info) {
    flush();
    const Style& s = cur();
    uint32_t w = 0, h = 0, pct = 0, ignored = 0;
    size_attribute(n, "width", &w, &pct);
    size_attribute(n, "height", &h, &ignored);
    if (s.width_px) {
      w = s.width_px;
      pct = 0;
    }
    if (s.width_pct) {
      pct = s.width_pct;
      w = 0;
    }
    if (s.height_px) h = s.height_px;
    const uint32_t nw = info.width ? info.width : 300, nh = info.height ? info.height : 150;
    if (pct) {
      w = nw;
      h = nh;
    } else if (w && h) {
    } else if (w) {
      h = static_cast<uint32_t>(static_cast<uint64_t>(nh) * w / nw);
    } else if (h) {
      w = static_cast<uint32_t>(static_cast<uint64_t>(nw) * h / nh);
    } else {
      w = nw;
      h = nh;
    }
    if (!ended_space_) literal(" ", indent());
    const uint8_t breaks = out_.runs_.count() == 0 ? 0 : pending_;
    failed_ |= !out_.add_object(static_cast<uint8_t>(info.kind), w, h, pct, indent(), breaks, cur_link(), s.align, s.background, cur_node());
    pending_ = 0;
    ended_space_ = false;
  }

  void void_action(const dom::Node* n) {
    if (hidden()) return;
    if (n->is_tag("br")) {
      flush();
      if (pending_ < 2) ++pending_;
      ended_space_ = true;
    } else if (n->is_tag("hr")) {
      block(2);
      const Style& s = cur();
      failed_ |= !out_.add_run(StringView(), kRunRule, 0, 0, out_.runs_.count() == 0 ? 0 : pending_, -1, 0, s.color, 0, cur_node());
      pending_ = 0;
      block(2);
    } else if (n->is_tag("img")) {
      StringView alt = attr(n, "alt");
      String shown(out_.allocator());
      shown.push_back('[');
      if (alt.empty()) {
        shown.append("image");
      } else {
        for (uint64_t k = 0; k < alt.size();) {
          if (static_cast<uint8_t>(alt[k]) < 0x80) {
            shown.push_back(alt[k] >= 32 ? alt[k] : ' ');
            ++k;
            continue;
          }
          const char* look = dom::ascii_look_alike(dom::next_code_point(alt, &k));
          shown.append(look ? StringView(look) : StringView("?"));
        }
      }
      shown.push_back(']');
      if (!ended_space_) literal(" ", indent());
      literal(shown.view(), indent());
    }
  }

  HtmlDocument& out_;
  const StyleSheet& sheet_;
  IObjectSource* objects_;
  const RenderOptions& options_;
  String chunk_;
  List<Element> stack_;
  Style root_;
  // --- Boxes (RendererBoxes.cpp): elements with borders, sizes, flexbox or grid layout are drawn into a box of their own - lines of cells - and put on the page.
  static constexpr uint32_t kMaxBoxDepth = 6;
  struct Raster {
    explicit Raster(allocators::IAllocator& a) : pieces(a), ends(a) {}
    List<LinePiece> pieces;
    List<uint32_t> ends;   // line i is pieces[ends[i - 1] .. ends[i])
    uint32_t width = 0;    // every line is this many columns wide
    uint32_t lines() const { return static_cast<uint32_t>(ends.count()); }
    void clear() {
      pieces.clear();
      ends.clear();
      width = 0;
    }
    // Takes over what another raster holds (which is left empty).
    bool take(Raster& other) {
      clear();
      width = other.width;
      bool ok = true;
      for (uint64_t i = 0; i < other.pieces.count(); ++i) ok = pieces.push_back(util::move(other.pieces[i])) && ok;
      for (uint64_t i = 0; i < other.ends.count(); ++i) ok = ends.push_back(other.ends[i]) && ok;
      other.clear();
      return ok;
    }
  };
  struct Edges {   // the borders and the padding of a box, in cells
    uint32_t border_left = 0, border_right = 0, border_top = 0, border_bottom = 0;
    uint32_t padding_left = 0, padding_right = 0, padding_top = 0, padding_bottom = 0;
    uint32_t horizontal() const { return border_left + border_right + padding_left + padding_right; }
    uint32_t vertical() const { return border_top + border_bottom + padding_top + padding_bottom; }
  };
  uint32_t container_cols_ = 80;             // the width of the box being drawn into, in columns
  const dom::Node* box_root_ = nullptr;      // the element whose box the caller made: it is drawn as a plain block with box_style_
  const Style* box_style_ = nullptr;
  uint32_t box_depth_ = 0;
  bool used_boxes_ = false;                  // a box was made while drawing: what was drawn depends on the width it was drawn for
  bool measuring_ = false;                   // widths are being measured: nothing grows to fill the room
  bool wants_box(const dom::Node* n, const Style& s) const;
  bool render_box(const dom::Node* n, const Style& s);
  Edges edges_of(const Style& s) const;
  // Draws what is in `n` (as a block of its own, with the style `inside`) into lines `content_cols` wide.
  bool draw_content(const dom::Node* n, const Style& inside, uint32_t content_cols, Raster* out);
  bool fit_document(const HtmlDocument& d, uint32_t width, uint32_t background, Raster* out);
  // The whole box of an element: content, padding, border, background; `outer_width` columns wide when it is not 0, else as wide as its content wants (at most `avail`).
  bool build_box(const dom::Node* n, const Style& s, uint32_t avail, uint32_t outer_width, uint32_t min_outer_lines, Raster* out);
  // Content measures of an element: the width of its longest line when nothing wraps (max) and of its longest word (min), in columns, for the content box.
  bool measure_box(const dom::Node* n, const Style& s, uint32_t avail, uint32_t* min_content, uint32_t* max_content);
  void decorate(Raster* r, const Style& s, const Edges& e, uint32_t min_outer_lines);
  void emit_raster(Raster& r, uint32_t left, uint32_t break_before);
  void pad_line(List<LinePiece>* row, uint32_t* used, uint32_t width, uint32_t background);
  Style inside_style(const Style& s, uint32_t content_cols) const;
  // One item of a flex or grid container.
  struct Item {
    explicit Item(allocators::IAllocator& a) : style(), box(a) {}
    const dom::Node* node = nullptr;   // an element, or a text node
    Style style;
    Edges edges;
    uint32_t margin_left = 0, margin_right = 0, margin_top = 0, margin_bottom = 0;
    bool auto_left = false, auto_right = false, auto_top = false, auto_bottom = false;
    uint32_t min_content = 0, max_content = 0;   // content box widths
    uint32_t basis = 0;                          // flex base size, outer (with padding and border)
    uint32_t main = 0;                           // the outer size along the main axis
    uint32_t min_main = 0, max_main = 0xFFFFFFFFu;
    uint32_t cross_offset = 0;
    int32_t grow = 0, shrink = 100;
    int32_t order = 0;
    int line = 0;
    // grid placement
    int32_t column = 0, row = 0, column_span = 1, row_span = 1;
    Raster box;
  };
  bool collect_items(const dom::Node* container, const Style& inside, List<UniquePointer<Item>>* items, List<const dom::Node*>* out_of_flow, uint32_t depth);
  bool prepare_item(Item* item, const Style& inside, uint32_t content_cols);
  bool draw_text_item(const dom::Node* text, const Style& inside, uint32_t width, Raster* out);
  uint32_t measured_min_ = 0, measured_max_ = 0;   // what a flex or grid layout found when it was measuring
  bool layout_flex(const dom::Node* n, const Style& s, uint32_t content_cols, uint32_t content_lines, Raster* out);
  bool layout_grid(const dom::Node* n, const Style& s, uint32_t content_cols, uint32_t content_lines, Raster* out);
  bool layout_columns(const dom::Node* n, const Style& s, uint32_t content_cols, Raster* out);
  bool failed_ = false, ended_space_ = true;
  uint32_t table_depth_ = 0;
  bool modal_pass_ = false;
  uint8_t pending_ = 0;
};

}  // namespace browser
