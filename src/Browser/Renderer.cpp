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

#include "Browser/RendererInternal.hpp"

namespace browser {

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

namespace {

// The defaults of an element's kind (what a browser's own style sheet and the page's old-style attributes say).
void ua_defaults(const dom::Node* n, Style* s) {
  StringView value;
  if (tag_in(n, {"head", "title", "meta", "link", "base", "script", "style", "noscript", "template", "datalist", "object",
                 "area", "param", "source", "track", "noembed", "noframes"}))
    s->display = Display::None;
  if (tag_in(n, {"div", "section", "article", "header", "footer", "nav", "main", "aside", "address", "center", "details", "summary", "caption", "thead", "tbody",
                 "tfoot", "fieldset", "dt", "dd", "legend", "tr", "body", "html", "menu", "dir", "figcaption", "hgroup", "search", "optgroup"}))
    s->display = Display::Block;
  if (tag_in(n, {"p", "table", "form", "figure", "dl", "ul", "ol", "pre", "blockquote", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "listing", "xmp"})) {
    s->display = Display::Block;
    s->margin[0] = s->margin[2] = Length::px(1600);   // (a line above and below)
  }
  if (tag_in(n, {"li"})) s->display = Display::ListItem;
  const StringView name = n->name.view();
  if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
    s->heading = static_cast<uint8_t>(name[1] - '0');
    s->bold = true;
  }
  if (tag_in(n, {"ul", "ol", "menu", "dir"})) {
    if (s->in_list) s->margin[0] = s->margin[2] = Length::px(0);
    s->in_list = true;
    s->padding[3] = Length::px(2400);   // (three columns)
  }
  if (tag_in(n, {"blockquote", "dd"})) s->margin[3] = Length::px(3200);   // (four columns)
  if (tag_in(n, {"b", "strong", "th"})) s->bold = true;
  if (tag_in(n, {"code", "tt", "kbd", "samp"})) s->code = true;
  if (tag_in(n, {"pre", "listing", "xmp"})) {
    s->pre = true;
    if (s->white_space == 0) s->white_space = 2;
  }
  if (tag_in(n, {"em", "i", "cite", "var", "dfn", "address"})) s->italic = true;
  if (n->is_tag("ol") && s->list_type == 0 && !s->list_none) s->list_type = 3;
  if (tag_in(n, {"ul", "menu", "dir"}) && s->list_type == 3) s->list_type = 0;
  if (n->is_tag("dialog") && !dom::has_attribute(n, "open")) s->display = Display::None;
  if (n->is_tag("mark")) {
    s->own_background = true;
    s->own_background_color = 0xFFFF00;
    s->background_argb = 0xFFFFFF00;
  }
  if (tag_in(n, {"u", "ins"})) s->underline = true;
  if (tag_in(n, {"s", "strike", "del"})) s->strike = true;
  if (tag_in(n, {"center", "caption", "th"})) s->align = 1;
  if (n->is_tag("table") && n->owner && n->owner->quirks_mode) s->align = 0;   // (in quirks mode a table does not pass on the text alignment it is in)
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
    s->background_argb = color;
  }
}

}  // namespace

// The style of `n` given its parent's.
Style derive_style(const Style& parent, const dom::Node* n, const StyleSheet& sheet) {
  Style s = parent.inherit();
  ua_defaults(n, &s);
  sheet.apply(n, attr(n, "style"), parent, &s);
  const uint32_t indent = static_cast<uint32_t>(parent.indent) + s.margin_left + s.padding_left;
  s.indent = static_cast<uint8_t>(indent < kMaxIndent ? indent : kMaxIndent);
  return s;
}

namespace {

// The text of the document's style sheets in order: <style> elements and linked sheets that apply.
bool media_applies(const dom::Node* n, const MediaEnvironment& env) {
  StringView media;
  return !dom::get_attribute(n, "media", &media) || media_matches(media, env);
}

}  // namespace

namespace {

// The next whitespace separated word of a declaration value.
bool next_value_word(StringView text, uint64_t* at, StringView* word) {
  uint64_t i = *at;
  while (i < text.size() && is_space(text[i])) ++i;
  if (i >= text.size()) return false;
  const uint64_t start = i;
  while (i < text.size() && !is_space(text[i])) ++i;
  *word = text.substr(start, i - start);
  *at = i;
  return true;
}

bool word_int(StringView w, int64_t* out) {
  if (w.empty()) return false;
  uint64_t i = 0;
  bool negative = false;
  if (w[0] == '-' || w[0] == '+') negative = w[i++] == '-';
  if (i >= w.size()) return false;
  int64_t v = 0;
  for (; i < w.size(); ++i) {
    if (w[i] < '0' || w[i] > '9') return false;
    v = v * 10 + (w[i] - '0');
    if (v > 1000000) v = 1000000;
  }
  *out = negative ? -v : v;
  return true;
}

uint8_t counter_style_of(StringView name) {
  if (name.equals_ignore_case("lower-alpha") || name.equals_ignore_case("lower-latin")) return 4;
  if (name.equals_ignore_case("upper-alpha") || name.equals_ignore_case("upper-latin")) return 5;
  if (name.equals_ignore_case("lower-roman")) return 6;
  if (name.equals_ignore_case("upper-roman")) return 7;
  if (name.equals_ignore_case("decimal-leading-zero")) return 9;
  return 3;
}

}  // namespace

// counter-reset and counter-increment of an element that was just opened.
void DocumentRenderer::update_counters(const dom::Node* n, const Style& s) {
  (void)n;
  if (hidden() || stack_.count() == 0) return;
  const uint64_t depth = stack_.count() - 1;
  for (int pass = 0; pass < 2; ++pass) {
    const StringView text = sheet_.text(pass == 0 ? s.counter_reset : s.counter_increment);
    uint64_t at = 0;
    StringView word;
    bool have = next_value_word(text, &at, &word);
    while (have && !failed_) {
      if (word.equals_ignore_case("none")) {
        have = next_value_word(text, &at, &word);
        continue;
      }
      StringView name = word;
      int64_t amount = pass == 0 ? 0 : 1;
      have = next_value_word(text, &at, &word);
      int64_t parsed;
      if (have && word_int(word, &parsed)) {
        amount = parsed;
        have = next_value_word(text, &at, &word);
      }
      if (pass == 0) {
        if (counters_.count() > 200) continue;
        Counter c(out_.allocator());
        c.name.assign(name);
        c.value = amount;
        c.depth = depth;
        failed_ |= !counters_.push_back(util::move(c));
        continue;
      }
      int64_t found = -1;
      for (uint64_t i = counters_.count(); i > 0 && found < 0; --i)
        if (counters_[i - 1].name.view().equals(name)) found = static_cast<int64_t>(i - 1);
      if (found < 0) {
        if (counters_.count() > 200) continue;
        Counter c(out_.allocator());
        c.name.assign(name);
        c.depth = depth;
        failed_ |= !counters_.push_back(util::move(c));
        found = static_cast<int64_t>(counters_.count() - 1);
      }
      counters_[static_cast<uint64_t>(found)].value += amount;
    }
  }
}

void DocumentRenderer::counter_text(StringView name, StringView style, StringView separator, bool all, String* out) const {
  const uint8_t type = counter_style_of(style);
  int64_t shown = 0;
  bool any = false;
  for (uint64_t i = 0; i < counters_.count(); ++i) {
    if (!counters_[i].name.view().equals(name)) continue;
    if (all) {
      if (any) out->append(separator);
      format_counter(counters_[i].value, type, out);
    }
    shown = counters_[i].value;
    any = true;
  }
  if (!all) format_counter(shown, type, out);
  else if (!any) format_counter(0, type, out);
}

// The text of a `content` value: strings, attr(), counter(), counters(), quotes.
bool DocumentRenderer::eval_content(const dom::Node* n, StringView content, String* out) {
  uint64_t i = 0;
  uint32_t guard = 0;
  while (i < content.size() && ++guard < 200) {
    while (i < content.size() && is_space(content[i])) ++i;
    if (i >= content.size()) break;
    const char c = content[i];
    if (c == '"' || c == '\'') {
      ++i;
      while (i < content.size() && content[i] != c) {
        if (content[i] != '\\') {
          out->push_back(content[i++]);
          continue;
        }
        ++i;
        if (i >= content.size()) break;
        uint32_t cp = 0, digits = 0;
        while (i < content.size() && digits < 6 && ((content[i] >= '0' && content[i] <= '9') || (content[i] >= 'a' && content[i] <= 'f') || (content[i] >= 'A' && content[i] <= 'F'))) {
          const char h = content[i++];
          cp = cp * 16 + static_cast<uint32_t>(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
          ++digits;
        }
        if (digits == 0) {
          if (content[i] == '\n') ++i;
          else out->push_back(content[i++]);
          continue;
        }
        if (i < content.size() && is_space(content[i])) ++i;
        if (cp == 0xA) out->push_back(' ');
        else if (cp < 0x80) out->push_back(static_cast<char>(cp));
        else if (const char* look = dom::ascii_look_alike(cp)) out->append(look);
        else out->push_back('?');
      }
      if (i < content.size()) ++i;
      continue;
    }
    if (c == '/') break;   // (alternative text for screen readers follows)
    uint64_t start = i;
    while (i < content.size() && !is_space(content[i]) && content[i] != '(') ++i;
    const StringView word = content.substr(start, i - start);
    if (i < content.size() && content[i] == '(') {
      ++i;
      const uint64_t arg_start = i;
      int depth = 1;
      char quote = 0;
      while (i < content.size() && depth > 0) {
        const char d = content[i];
        if (quote) {
          if (d == quote) quote = 0;
        } else if (d == '"' || d == '\'') {
          quote = d;
        } else if (d == '(') {
          ++depth;
        } else if (d == ')') {
          --depth;
        }
        ++i;
      }
      const StringView args = content.substr(arg_start, i - arg_start - (depth == 0 ? 1 : 0));
      // the comma separated arguments, quotes removed
      StringView parts[3];
      uint32_t count = 0;
      uint64_t k = 0, part_start = 0;
      quote = 0;
      for (; k <= args.size() && count < 3; ++k) {
        const char d = k < args.size() ? args[k] : ',';
        if (quote) {
          if (d == quote) quote = 0;
          continue;
        }
        if (d == '"' || d == '\'') quote = d;
        else if (d == ',') {
          StringView part = args.substr(part_start, k - part_start).trimmed();
          if (part.size() >= 2 && (part[0] == '"' || part[0] == '\'')) part = part.substr(1, part.size() - 2);
          parts[count++] = part;
          part_start = k + 1;
        }
      }
      if (word.equals_ignore_case("attr")) {
        StringView value;
        if (count > 0 && n && dom::get_attribute(n, parts[0], &value)) out->append(value);
      } else if (word.equals_ignore_case("counter")) {
        if (count > 0) counter_text(parts[0], count > 1 ? parts[1] : StringView(), StringView(), false, out);
      } else if (word.equals_ignore_case("counters")) {
        if (count > 1) counter_text(parts[0], count > 2 ? parts[2] : StringView(), parts[1], true, out);
      }
      continue;
    }
    if (word.equals_ignore_case("open-quote")) {
      out->push_back(quote_depth_ % 2 == 0 ? '"' : '\'');
      ++quote_depth_;
    } else if (word.equals_ignore_case("close-quote")) {
      if (quote_depth_ > 0) --quote_depth_;
      out->push_back(quote_depth_ % 2 == 0 ? '"' : '\'');
    } else if (word.equals_ignore_case("no-open-quote")) {
      ++quote_depth_;
    } else if (word.equals_ignore_case("no-close-quote")) {
      if (quote_depth_ > 0) --quote_depth_;
    }
    if (i == start) ++i;
  }
  return !out->empty() || content.size() > 0;
}

// ::before and ::after: the generated text is drawn in the style the rules for it give.
void DocumentRenderer::pseudo_content(const dom::Node* n, PseudoElement which) {
  if (!sheet_.has_pseudo(n, which)) return;
  Style ps = cur().inherit();
  sheet_.apply(n, StringView(), cur(), &ps, which);
  if (!ps.has_content || ps.display == Display::None) return;
  String text(out_.allocator());
  if (!eval_content(n, sheet_.text(ps.content), &text)) return;
  const bool blockish = ps.display != Display::Inline;
  if (text.empty() && !blockish) return;
  const uint32_t indent_here = static_cast<uint32_t>(cur().indent) + ps.margin_left + ps.padding_left;
  ps.indent = static_cast<uint8_t>(indent_here < kMaxIndent ? indent_here : kMaxIndent);
  if (ps.own_background) {
    ps.background = ps.own_background_color;
    ps.background_block = blockish;
  }
  Element e;
  e.style = ps;
  e.node = n;
  e.link = cur_link();
  failed_ |= !stack_.push_back(e);
  if (blockish) block(static_cast<uint8_t>(1 + ps.margin_top + ps.padding_top));
  if (!text.empty()) literal(text.view(), ps.indent);
  if (blockish) block(static_cast<uint8_t>(1 + ps.margin_bottom + ps.padding_bottom));
  stack_.remove_at(stack_.count() - 1);
}

bool collect_style_sheets(dom::Document& doc, const RenderOptions& options, StyleSheet* sheet) {
  if (!options.extra_css.empty() && !sheet->parse(options.extra_css, options.media)) return false;
  List<const dom::Node*> roots(doc.allocator());   // the document, then the shadow trees found in it (their style elements apply too)
  roots.push_back(doc.root());
  for (uint64_t r = 0; r < roots.count() && r < 200; ++r) {
  sheet->set_scope(r == 0 ? 0 : roots[r]->serial);   // (the styles of a shadow tree are for that tree)
  for (const dom::Node* n = roots[r]; n; n = dom::next_in_tree(n, roots[r])) {
    if (!n->is_element()) continue;
    if (n->shadow) roots.push_back(n->shadow);
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
  }
  sheet->set_scope(0);
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
  DocumentRenderer renderer(*out, sheet, options);
  return renderer.run(doc);
}

}  // namespace browser
