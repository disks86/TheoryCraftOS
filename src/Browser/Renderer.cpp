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

#include "Browser/Renderer.hpp"
#include <initializer_list>
#include "Dom/Entities.hpp"

namespace browser {

namespace {

constexpr uint32_t kMaxIndent = 60;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool tag_in(const dom::Node* n, std::initializer_list<const char*> names) {
  for (const char* t : names)
    if (n->name.view().equals(t)) return true;
  return false;
}

StringView attr(const dom::Node* n, const char* name) {
  StringView v;
  return dom::get_attribute(n, name, &v) ? v : StringView();
}

// The defaults of an element's kind (what a browser's own style sheet and the page's old-style attributes say).
void ua_defaults(const dom::Node* n, Style* s) {
  StringView value;
  if (tag_in(n, {"head", "title", "meta", "link", "base", "script", "style", "noscript", "template", "textarea", "select", "datalist", "iframe", "object", "svg",
                 "area", "param", "source", "track", "dialog", "noembed", "noframes"}))
    s->display = Display::None;
  if (tag_in(n, {"div", "section", "article", "header", "footer", "nav", "main", "aside", "address", "center", "details", "summary", "caption", "thead", "tbody",
                 "tfoot", "fieldset", "dt", "dd", "legend", "tr", "body", "html", "menu", "dir", "figcaption", "hgroup", "search", "optgroup"}))
    s->display = Display::Block;
  if (tag_in(n, {"p", "table", "form", "figure", "dl", "ul", "ol", "pre", "blockquote", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "listing", "xmp"})) {
    s->display = Display::Block;
    s->margin_top = s->margin_bottom = 1;
  }
  if (tag_in(n, {"li"})) s->display = Display::ListItem;
  const StringView name = n->name.view();
  if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
    s->heading = static_cast<uint8_t>(name[1] - '0');
    s->bold = true;
  }
  if (tag_in(n, {"ul", "ol", "menu", "dir"})) {
    if (s->in_list) s->margin_top = s->margin_bottom = 0;
    s->in_list = true;
    s->padding_left = 3;
  }
  if (tag_in(n, {"blockquote", "dd"})) s->margin_left = 4;
  if (tag_in(n, {"b", "strong", "th"})) s->bold = true;
  if (tag_in(n, {"code", "tt", "kbd", "samp"})) s->code = true;
  if (tag_in(n, {"pre", "listing", "xmp"})) s->pre = true;
  if (tag_in(n, {"u", "ins"})) s->underline = true;
  if (tag_in(n, {"s", "strike", "del"})) s->strike = true;
  if (tag_in(n, {"center"})) s->align = 1;
  if (tag_in(n, {"a"}) && dom::has_attribute(n, "href")) s->underline = true;
  if (dom::has_attribute(n, "hidden")) s->display = Display::None;
  if (dom::get_attribute(n, "align", &value)) {
    if (value.equals_ignore_case("center") || value.equals_ignore_case("middle")) s->align = 1;
    else if (value.equals_ignore_case("right")) s->align = 2;
    else if (value.equals_ignore_case("left")) s->align = 0;
  }
  uint32_t color;
  if (tag_in(n, {"font"}) && dom::get_attribute(n, "color", &value) && parse_css_color(value, &color) && color) s->color = color;
  if (tag_in(n, {"body", "html", "table", "tr", "td", "th"}) && dom::get_attribute(n, "bgcolor", &value) && parse_css_color(value, &color)) {
    s->own_background = true;
    s->own_background_color = color;
  }
}

// The style of `n` given its parent's.
Style derive_style(const Style& parent, const dom::Node* n, const StyleSheet& sheet) {
  Style s = parent.inherit();
  ua_defaults(n, &s);
  sheet.apply(n, attr(n, "style"), &s);
  const uint32_t indent = static_cast<uint32_t>(parent.indent) + s.margin_left + s.padding_left;
  s.indent = static_cast<uint8_t>(indent < kMaxIndent ? indent : kMaxIndent);
  return s;
}

// The text of the document's style sheets in order: <style> elements and linked sheets that apply.
bool media_applies(const dom::Node* n, const MediaEnvironment& env) {
  StringView media;
  return !dom::get_attribute(n, "media", &media) || media_matches(media, env);
}

}  // namespace

class DocumentRenderer {
 public:
  DocumentRenderer(HtmlDocument& out, const StyleSheet& sheet, IObjectSource* objects) : out_(out), sheet_(sheet), objects_(objects), chunk_(out.allocator()), stack_(out.allocator()) {}

  bool run(const dom::Document& doc) {
    const dom::Node* root = doc.root();
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
    const bool is_body = tag_in(n, {"body", "html"});
    if (s.own_background) {
      if (is_body) {
        if (s.own_background_color) out_.page_background_ = s.own_background_color;
      } else {
        s.background = s.own_background_color;
        s.background_block = s.display != Display::Inline;
      }
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
    if (objects_ && !hidden() && tag_in(n, {"img", "video", "canvas"})) {
      ObjectInfo info;
      if (objects_->object_info(n, &info) && info.kind != ObjectKind::None && (info.ready || info.kind != ObjectKind::Image)) {
        object_run(n, info);
        return false;   // (a video's or canvas's fallback content is not shown)
      }
    }
    if (dom::is_void_tag(n->name.view())) void_action(n);
    return true;
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
  String chunk_;
  List<Element> stack_;
  Style root_;
  bool failed_ = false, ended_space_ = true;
  uint8_t pending_ = 0;
};

bool collect_style_sheets(dom::Document& doc, const RenderOptions& options, StyleSheet* sheet) {
  if (!options.extra_css.empty() && !sheet->parse(options.extra_css, options.media)) return false;
  for (const dom::Node* n = doc.root(); n; n = dom::next_in_tree(n, doc.root())) {
    if (!n->is_element()) continue;
    if (n->is_tag("style")) {
      StringView type;
      if (!media_applies(n, options.media)) continue;
      if (dom::get_attribute(n, "type", &type) && !type.empty() && !type.equals_ignore_case("text/css")) continue;
      String css(doc.allocator());
      dom::text_content(n, &css);
      if (!sheet->parse(css.view(), options.media)) return false;
    } else if (n->is_tag("link") && options.styles) {
      StringView rel;
      if (!dom::get_attribute(n, "rel", &rel) || !media_applies(n, options.media)) continue;
      bool stylesheet = false, alternate = false;
      uint64_t i = 0;
      while (i < rel.size()) {
        while (i < rel.size() && is_space(rel[i])) ++i;
        const uint64_t start = i;
        while (i < rel.size() && !is_space(rel[i])) ++i;
        const StringView word = rel.substr(start, i - start);
        stylesheet = stylesheet || word.equals_ignore_case("stylesheet");
        alternate = alternate || word.equals_ignore_case("alternate");
      }
      if (!stylesheet || alternate) continue;
      const StringView css = options.styles->linked(n);
      if (!css.empty() && !sheet->parse(css, options.media)) return false;
    }
  }
  return true;
}

void document_title(const dom::Document& doc, String* out) {
  out->clear();
  for (const dom::Node* n = doc.root(); n; n = dom::next_in_tree(n, doc.root())) {
    if (!n->is_tag("title")) continue;
    String raw(doc.allocator());
    dom::text_content(n, &raw);
    bool space = true;
    for (uint64_t i = 0; i < raw.size();) {
      const char c = raw[i];
      if (is_space(c)) {
        if (!space) out->push_back(' ');
        space = true;
        ++i;
      } else if (static_cast<uint8_t>(c) < 0x80) {
        out->push_back(c >= 32 ? c : ' ');
        space = false;
        ++i;
      } else {
        const char* look = dom::ascii_look_alike(dom::next_code_point(raw.view(), &i));
        out->append(look ? StringView(look) : StringView("?"));
        space = false;
      }
    }
    while (out->size() && (*out)[out->size() - 1] == ' ') out->truncate(out->size() - 1);
    return;
  }
}

Style computed_style(const StyleSheet& sheet, const dom::Node* element) {
  const dom::Node* chain[512];
  uint32_t depth = 0;
  for (const dom::Node* n = element; n && n->is_element() && depth < 512; n = n->parent) chain[depth++] = n;
  Style s;
  for (uint32_t i = depth; i > 0; --i) {
    const dom::Node* n = chain[i - 1];
    s = derive_style(s, n, sheet);
    if (s.own_background) {
      s.background = s.own_background_color;
      s.background_block = s.display != Display::Inline;
    }
  }
  return s;
}

bool render_document(dom::Document& doc, const RenderOptions& options, HtmlDocument* out) {
  out->clear();
  StyleSheet sheet(doc.allocator());
  if (!collect_style_sheets(doc, options, &sheet)) return false;
  String title(doc.allocator());
  document_title(doc, &title);
  out->title_.assign(title.view());
  out->truncated_ = doc.full();
  DocumentRenderer renderer(*out, sheet, options.objects);
  return renderer.run(doc);
}

}  // namespace browser
