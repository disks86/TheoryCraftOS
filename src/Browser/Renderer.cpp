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
  if (tag_in(n, {"pre", "listing", "xmp"})) s->pre = true;
  if (tag_in(n, {"u", "ins"})) s->underline = true;
  if (tag_in(n, {"s", "strike", "del"})) s->strike = true;
  if (tag_in(n, {"center", "caption", "th"})) s->align = 1;
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
  DocumentRenderer renderer(*out, sheet, options);
  return renderer.run(doc);
}

}  // namespace browser
