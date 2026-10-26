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

#include "Dom/HtmlBuilder.hpp"
#include <initializer_list>
#include "Dom/Entities.hpp"
#include "Utility/Move.hpp"

namespace dom {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool name_in(StringView name, std::initializer_list<const char*> names) {
  for (const char* n : names)
    if (name.equals(n)) return true;
  return false;
}

bool is_head_tag(StringView t) { return name_in(t, {"title", "meta", "link", "base", "style", "script", "noscript", "template", "basefont", "bgsound"}); }
bool is_raw_text(StringView t) { return name_in(t, {"script", "style", "xmp", "iframe", "noembed", "noframes", "noscript"}); }
bool is_rcdata(StringView t) { return name_in(t, {"title", "textarea"}); }
bool is_scope_boundary(StringView t) { return name_in(t, {"html", "body", "table", "td", "th", "caption", "template", "object", "button", "marquee", "applet"}); }

class TreeBuilder {
 public:
  static constexpr uint32_t kMaxDepth = 400;

  TreeBuilder(Document& doc, IScriptRunner* scripts, Node* fragment, StringView context)
      : doc_(doc), scripts_(scripts), fragment_(fragment), context_(context), open_(doc.allocator()), chunks_(doc.allocator()), text_(doc.allocator()) {}

  bool run(StringView html) {
    if (fragment_ && (is_raw_text(context_) || is_rcdata(context_))) {   // the content of such an element is text
      String data(doc_.allocator());
      if (is_rcdata(context_)) decode_text(html, &data);
      else data.assign(html);
      add_text(data.view());
      flush_text();
      return !failed_;
    }
    feed(html);
    flush_text();
    if (!fragment_) {
      ensure_html();   // (an empty page still has <html>, <head> and <body>)
      ensure_head();
      close_head();
      ensure_body();
    }
    return !failed_;
  }

 private:
  // --- The tree
  Node* current() const { return open_.count() ? open_[open_.count() - 1] : (fragment_ ? fragment_ : doc_.root()); }

  Node* make_element(StringView name) {
    Node* n = doc_.create_element(name);
    if (!n) (doc_.full() ? stopped_ : failed_) = true;   // (a page too big for the node limit is cut off where it stands)
    return n;
  }
  void insert(Node* n) {
    if (append_child(current(), n) != DomError::None) failed_ = true;
  }
  bool push(Node* n) {
    if (open_.count() >= kMaxDepth) return false;
    if (!open_.push_back(n)) {
      failed_ = true;
      return false;
    }
    return true;
  }
  void pop() {
    if (open_.count()) open_.remove_at(open_.count() - 1);
  }
  // Pops the stack down to (not including) index `depth`.
  void pop_to(uint64_t depth) {
    while (open_.count() > depth) pop();
    if (head_ && !on_stack(head_)) head_open_ = false;
  }
  bool on_stack(const Node* n) const {
    for (uint64_t i = 0; i < open_.count(); ++i)
      if (open_[i] == n) return true;
    return false;
  }

  void ensure_html() {
    if (fragment_ || html_) return;
    html_ = make_element("html");
    if (!html_) return;
    append_child(doc_.root(), html_);
    push(html_);
  }
  void ensure_head() {
    if (fragment_ || head_) return;
    ensure_html();
    if (!html_) return;
    head_ = make_element("head");
    if (!head_) return;
    append_child(html_, head_);
    push(head_);
    head_open_ = true;
  }
  void close_head() {
    if (fragment_ || !head_open_) return;
    for (uint64_t i = open_.count(); i > 0; --i) {
      if (open_[i - 1] == head_) {
        pop_to(i - 1);
        break;
      }
    }
    head_open_ = false;
  }
  void ensure_body() {
    if (fragment_) return;
    ensure_html();
    close_head();
    if (!html_) return;
    if (!head_) ensure_head(), close_head();
    if (!body_) {
      body_ = make_element("body");
      if (!body_) return;
      append_child(html_, body_);
      push(body_);
    }
  }

  // --- Text
  void add_text(StringView s) {
    if (s.empty()) return;
    text_.append(s);
  }
  void flush_text() {
    if (text_.empty()) return;
    bool blank = true;
    for (uint64_t i = 0; i < text_.size() && blank; ++i) blank = is_space(text_[i]);
    if (!fragment_) {
      if (blank && !body_ && !head_open_) {   // white space between the head and the body (or before the page) is not part of it
        text_.clear();
        return;
      }
      if (!blank && !head_open_) ensure_body();
      else if (!blank && head_open_ && current() == head_) {
        close_head();
        ensure_body();
      }
    }
    Node* parent = current();
    if (parent->last_child && parent->last_child->is_text()) {
      parent->last_child->data.append(text_.view());
      parent->owner->touch();
    } else {
      Node* t = doc_.create_text(text_.view());
      if (t) insert(t);
      else (doc_.full() ? stopped_ : failed_) = true;
    }
    text_.clear();
  }
  // Text with its references decoded.
  void decode_text(StringView s, String* out) {
    for (uint64_t i = 0; i < s.size();) {
      if (s[i] == '&') i += decode_reference(s, i, false, out);
      else out->push_back(s[i++]);
    }
  }

  // --- Implied end tags
  void close_p_in_scope() {
    for (uint64_t i = open_.count(); i > 0; --i) {
      const Node* n = open_[i - 1];
      if (n->is_tag("p")) {
        pop_to(i - 1);
        return;
      }
      if (is_scope_boundary(n->name.view())) return;
    }
  }
  // The innermost open element called one of `names`, searching no further than the nearest of `stops`; closes it and what is inside.
  void close_open(std::initializer_list<const char*> names, std::initializer_list<const char*> stops) {
    for (uint64_t i = open_.count(); i > 0; --i) {
      const StringView n = open_[i - 1]->name.view();
      if (name_in(n, stops)) return;
      if (name_in(n, names)) {
        pop_to(i - 1);
        return;
      }
    }
  }
  void close_for_start(StringView name) {
    if (name_in(name, {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "dl", "table", "pre", "blockquote", "form", "hr", "section", "article", "header",
                       "footer", "nav", "aside", "address", "figure", "fieldset", "main", "details", "menu", "dir", "center", "listing", "xmp", "plaintext"}))
      close_p_in_scope();
    if (name_in(name, {"h1", "h2", "h3", "h4", "h5", "h6"}) && open_.count() && name_in(current()->name.view(), {"h1", "h2", "h3", "h4", "h5", "h6"})) pop_to(open_.count() - 1);
    if (name.equals("li")) close_open({"li"}, {"ul", "ol", "menu", "dir", "table", "body"});
    else if (name_in(name, {"dt", "dd"})) close_open({"dt", "dd"}, {"dl", "table", "body"});
    else if (name.equals("tr")) close_open({"tr", "caption"}, {"table"});
    else if (name_in(name, {"td", "th"})) close_open({"td", "th"}, {"tr", "table"});
    else if (name_in(name, {"thead", "tbody", "tfoot", "caption", "colgroup"})) {   // these start directly in the table: whatever is open inside it ends
      for (uint64_t i = open_.count(); i > 0; --i) {
        if (open_[i - 1]->is_tag("table")) {
          pop_to(i);
          break;
        }
        if (name_in(open_[i - 1]->name.view(), {"html", "body"})) break;
      }
    }
    else if (name.equals("option")) close_open({"option"}, {"select", "datalist", "optgroup"});
    else if (name.equals("optgroup")) close_open({"option", "optgroup"}, {"select"});
    else if (name.equals("a")) close_open({"a"}, {"table", "td", "th", "body", "html", "template"});
    else if (name.equals("button")) close_open({"button"}, {"table", "td", "th", "body", "html"});
  }

  // --- Tokens
  // The attributes in the text of a tag (after its name): decoded, names lowercased, the first of a name wins.
  void read_attributes(StringView attrs, Node* element) {
    uint64_t i = 0;
    const uint64_t n = attrs.size();
    while (i < n) {
      while (i < n && (is_space(attrs[i]) || attrs[i] == '/')) ++i;
      const uint64_t start = i;
      while (i < n && !is_space(attrs[i]) && attrs[i] != '=' && attrs[i] != '/') ++i;
      const StringView name = attrs.substr(start, i - start);
      while (i < n && is_space(attrs[i])) ++i;
      String value(doc_.allocator());
      if (i < n && attrs[i] == '=') {
        ++i;
        while (i < n && is_space(attrs[i])) ++i;
        StringView raw;
        if (i < n && (attrs[i] == '"' || attrs[i] == '\'')) {
          const char q = attrs[i++];
          const uint64_t s = i;
          while (i < n && attrs[i] != q) ++i;
          raw = attrs.substr(s, i - s);
          if (i < n) ++i;
        } else {
          const uint64_t s = i;
          while (i < n && !is_space(attrs[i])) ++i;
          raw = attrs.substr(s, i - s);
        }
        for (uint64_t k = 0; k < raw.size();) {
          if (raw[k] == '&') k += decode_reference(raw, k, true, &value);
          else value.push_back(raw[k++]);
        }
      }
      if (name.empty()) continue;
      if (!has_attribute(element, name) && set_attribute(element, name, value.view()) != DomError::None) failed_ = true;
    }
  }

  static uint64_t find_end_tag(StringView html, uint64_t from, StringView name) {
    for (uint64_t k = from; k + 2 + name.size() <= html.size(); ++k) {
      if (html[k] != '<' || html[k + 1] != '/') continue;
      bool same = true;
      for (uint64_t j = 0; j < name.size() && same; ++j) same = lower(html[k + 2 + j]) == name[j];
      if (!same) continue;
      const uint64_t after = k + 2 + name.size();
      if (after >= html.size() || is_space(html[after]) || html[after] == '>' || html[after] == '/') return k;
    }
    return html.size();
  }

  void feed(StringView html) {
    const uint64_t n = html.size();
    uint64_t i = 0;
    while (i < n && !failed_ && !stopped_) {
      const char c = html[i];
      if (c == '<') {
        i = markup(html, i);
      } else if (c == '&') {
        String decoded(doc_.allocator());
        i += decode_reference(html, i, false, &decoded);
        add_text_checked(decoded.view());
      } else {
        const uint64_t start = i;
        while (i < n && html[i] != '<' && html[i] != '&') ++i;
        add_text_checked(html.substr(start, i - start));
      }
    }
  }

  // Text from the page: its first newline right after <pre> / <textarea> goes.
  void add_text_checked(StringView s) {
    if (skip_newline_ && !s.empty()) {
      skip_newline_ = false;
      if (s[0] == '\n') s = s.substr(1);
      else if (s[0] == '\r') s = s.substr(s.size() > 1 && s[1] == '\n' ? 2 : 1);
    }
    add_text(s);
  }

  uint64_t markup(StringView html, uint64_t at) {
    const uint64_t n = html.size();
    if (html.substr(at).starts_with("<!--")) {
      const uint64_t end = html.find("-->", at + 4);
      const uint64_t data_end = end == StringView::npos ? n : end;
      flush_text();
      Node* c = doc_.create_comment(html.substr(at + 4, data_end - at - 4));
      if (c) {
        if (!fragment_ && !html_) append_child(doc_.root(), c);
        else insert(c);
      }
      return end == StringView::npos ? n : end + 3;
    }
    if (at + 1 < n && (html[at + 1] == '!' || html[at + 1] == '?')) {   // a doctype or a bogus comment
      const uint64_t end = html.find('>', at);
      return end == StringView::npos ? n : end + 1;
    }
    uint64_t p = at + 1;
    const bool closing = p < n && html[p] == '/';
    if (closing) ++p;
    if (p >= n || !is_letter(html[p])) {
      add_text("<");
      return at + 1;
    }
    const uint64_t name_start = p;
    while (p < n && !is_space(html[p]) && html[p] != '/' && html[p] != '>') ++p;
    String name(doc_.allocator());
    for (uint64_t k = name_start; k < p; ++k) name.push_back(lower(html[k]));
    const uint64_t attrs_start = p;
    char quote = 0;
    while (p < n && (quote || html[p] != '>')) {
      if (quote) {
        if (html[p] == quote) quote = 0;
      } else if (html[p] == '"' || html[p] == '\'') {
        quote = html[p];
      }
      ++p;
    }
    const StringView attrs = html.substr(attrs_start, p - attrs_start);
    const uint64_t after = p < n ? p + 1 : n;
    flush_text();
    if (closing) {
      end_tag(name.view());
      return after;
    }
    return start_tag(name.view(), attrs, html, after);
  }

  void end_tag(StringView name) {
    if (name.equals("head")) {
      close_head();
      return;
    }
    if (name_in(name, {"html", "body"})) return;
    if (name.equals("br")) {
      Node* br = make_element("br");
      if (br) {
        ensure_body();
        insert(br);
      }
      return;
    }
    if (name.equals("p")) {
      bool open = false;
      for (uint64_t i = open_.count(); i > 0 && !open; --i) {
        if (open_[i - 1]->is_tag("p")) open = true;
        else if (is_scope_boundary(open_[i - 1]->name.view())) break;
      }
      if (!open) return;   // (a stray </p>)
    }
    if (name_in(name, {"thead", "tbody", "tfoot", "tr", "td", "th", "caption", "colgroup"})) {   // a part of a table ends with what is open inside it (a </thead> closes the row and cell)
      for (uint64_t i = open_.count(); i > 0; --i) {
        const StringView open_name = open_[i - 1]->name.view();
        if (open_name.equals(name)) {
          pop_to(i - 1);
          return;
        }
        if (name_in(open_name, {"table", "template", "html", "body"})) return;
        if (name_in(name, {"td", "th"}) && name_in(open_name, {"tr", "tbody", "thead", "tfoot"})) return;
        if (name.equals("tr") && name_in(open_name, {"tbody", "thead", "tfoot"})) return;
      }
      return;
    }
    for (uint64_t i = open_.count(); i > 0; --i) {
      const Node* n = open_[i - 1];
      if (n->name.view().equals(name)) {
        pop_to(i - 1);
        return;
      }
      if (is_scope_boundary(n->name.view()) && !is_scope_boundary(name) && !name_in(name, {"div", "span", "a", "b", "i", "em", "strong", "p", "ul", "ol", "li", "form", "section"})) return;
      if (name_in(n->name.view(), {"html", "body", "head"})) return;
    }
  }

  uint64_t start_tag(StringView name, StringView attrs, StringView html, uint64_t after) {
    const uint64_t n = html.size();
    if (name.equals("html")) {
      ensure_html();
      if (html_) read_attributes(attrs, html_);
      return after;
    }
    if (name.equals("head")) {
      ensure_html();
      if (!head_ && !body_) ensure_head();
      if (head_) read_attributes(attrs, head_);
      return after;
    }
    if (name.equals("body")) {
      ensure_body();
      if (body_) read_attributes(attrs, body_);
      return after;
    }
    // Where it goes: the head (before the body has started), or the body.
    const bool head_thing = !fragment_ && !body_ && is_head_tag(name);
    if (!fragment_) {
      if (head_thing) {
        ensure_html();
        if (!head_) ensure_head();
        else if (!head_open_) {
          head_open_ = true;   // (more head content after the head was closed, before the body: back into the head)
          if (head_ && !on_stack(head_)) push(head_);
        }
      } else if (!(body_ && !head_open_)) {
        ensure_body();
      }
    }
    if (!head_thing) {
      close_for_start(name);
      if (name_in(name, {"tr", "td", "th"})) {   // rows go in a table body and cells in a row, which the page may leave out
        if (current() && current()->is_tag("table")) {
          if (Node* body = make_element("tbody")) {
            insert(body);
            push(body);
          }
        }
        if (!name.equals("tr") && current() && name_in(current()->name.view(), {"tbody", "thead", "tfoot"})) {
          if (Node* row = make_element("tr")) {
            insert(row);
            push(row);
          }
        }
      }
    }
    Node* el = make_element(name);
    if (!el) return after;
    read_attributes(attrs, el);
    set_namespace(el, name);
    insert(el);
    bool is_void = is_void_tag(name);
    if (!is_void && attrs.trimmed().ends_with('/')) {   // <rect ... /> in an svg is closed in place
      bool in_svg = name.equals("svg");
      for (uint64_t i = 0; i < open_.count() && !in_svg; ++i) in_svg = open_[i]->is_tag("svg");
      is_void = in_svg;
    }
    // raw text and character data elements: their content, then the end tag
    if (!is_void && (is_raw_text(name) || is_rcdata(name))) {
      const uint64_t end = find_end_tag(html, after, name);
      StringView raw = html.substr(after, end - after);
      if (name.equals("textarea") && !raw.empty() && raw[0] == '\n') raw = raw.substr(1);
      String data(doc_.allocator());
      if (is_rcdata(name)) decode_text(raw, &data);
      else data.assign(raw);
      if (!data.empty()) {
        Node* t = doc_.create_text(data.view());
        if (t) append_child(el, t);
      }
      uint64_t next = n;
      if (end < n) {
        const uint64_t close = html.find('>', end);
        next = close == StringView::npos ? n : close + 1;
      }
      if (name.equals("script")) script_done(el);
      return next;
    }
    if (!is_void) {
      push(el);
      if (name_in(name, {"pre", "listing"})) skip_newline_ = true;
    }
    return after;
  }

  // The elements of an <svg> or <math> are not HTML: they get that namespace (and, for svg, the spelling of their names). What is inside a <foreignObject> is HTML again.
  void set_namespace(Node* el, StringView name) {
    static const char* const kSvg = "http://www.w3.org/2000/svg";
    static const char* const kMath = "http://www.w3.org/1998/Math/MathML";
    const char* ns = nullptr;
    if (name.equals("svg")) ns = kSvg;
    else if (name.equals("math")) ns = kMath;
    else {
      const Node* p = current();
      if (p && p->is_element() && p->ns_set && !p->ns.empty() && !p->name.view().equals("foreignobject") && !p->name.view().equals("annotation-xml")) ns = p->ns.view().equals(kSvg) ? kSvg : kMath;
    }
    if (!ns) return;
    el->ns_set = true;
    el->ns.assign(ns);
    if (ns != kSvg) return;
    static const char* const kCamel[] = {"altGlyph", "altGlyphDef", "altGlyphItem", "animateColor", "animateMotion", "animateTransform", "clipPath", "feBlend", "feColorMatrix", "feComponentTransfer",
                                         "feComposite", "feConvolveMatrix", "feDiffuseLighting", "feDisplacementMap", "feDistantLight", "feDropShadow", "feFlood", "feFuncA", "feFuncB", "feFuncG",
                                         "feFuncR", "feGaussianBlur", "feImage", "feMerge", "feMergeNode", "feMorphology", "feOffset", "fePointLight", "feSpecularLighting", "feSpotLight", "feTile",
                                         "feTurbulence", "foreignObject", "glyphRef", "linearGradient", "radialGradient", "textPath"};
    for (const char* camel : kCamel)
      if (name.equals_ignore_case(camel)) {
        el->qname.assign(camel);
        break;
      }
  }

  // A <script> has been read (it is in the tree): run it, and read what it wrote.
  void script_done(Node* script) {
    StringView type, src;
    const bool has_type = get_attribute(script, "type", &type);
    const bool has_src = get_attribute(script, "src", &src) && !src.empty();
    uint32_t index = 0;
    if (has_src) index = external_scripts_++;
    const bool javascript = !has_type || type.empty() || type.equals_ignore_case("text/javascript") || type.equals_ignore_case("application/javascript") ||
                            type.equals_ignore_case("text/ecmascript") || type.equals_ignore_case("application/ecmascript");
    if (!scripts_ || fragment_ || !javascript || script_depth_ > 3) return;
    for (const Node* p = script->parent; p; p = p->parent)
      if (p->is_tag("template")) return;   // (what is in a template is inert)
    String inline_text(doc_.allocator());
    text_content(script, &inline_text);
    const StringView source = has_src ? scripts_->external(index, src) : inline_text.view();
    if (source.empty()) return;
    String written(doc_.allocator());
    ++script_depth_;
    scripts_->run(script, source, &written);
    if (!written.empty() && chunks_.push_back(util::move(written))) {
      // (kept for as long as the page is read: the nodes made from it do not point into it, but the next chunk may continue its markup)
      const StringView text = chunks_[chunks_.count() - 1].view();
      feed(text);
      flush_text();
    }
    --script_depth_;
  }

  Document& doc_;
  IScriptRunner* scripts_;
  Node* fragment_;
  StringView context_;
  List<Node*> open_;
  List<String> chunks_;
  String text_;
  Node *html_ = nullptr, *head_ = nullptr, *body_ = nullptr;
  bool head_open_ = false, skip_newline_ = false, failed_ = false, stopped_ = false;
  uint32_t external_scripts_ = 0, script_depth_ = 0;
};

}  // namespace

static void move_into_content(Node* t) {
  while (t->first_child) {
    Node* c = t->first_child;
    remove_child(t, c);
    append_child(t->content, c);
  }
}

Node* template_content(Node* tmpl) {
  if (!tmpl->content) {
    tmpl->content = tmpl->owner->create_fragment();
    if (!tmpl->content) return nullptr;
    tmpl->content->template_content = true;
  }
  move_into_content(tmpl);   // (what has been read into the template so far, when a script asks while the page is being read)
  return tmpl->content;
}

void adopt_template_contents(Document& doc) {
  for (uint32_t serial = 1; serial <= doc.node_count(); ++serial) {
    Node* t = doc.by_serial(serial);
    if (t && t->is_tag("template")) template_content(t);
  }
}

// True if the page starts with a doctype (white space and comments may come before it).
static bool starts_with_doctype(StringView html) {
  uint64_t at = 0;
  for (;;) {
    while (at < html.size() && is_space(html[at])) ++at;
    if (html.substr(at).starts_with("<!--")) {
      const uint64_t end = html.find("-->", at + 4);
      if (end == StringView::npos) return false;
      at = end + 3;
      continue;
    }
    break;
  }
  const StringView rest = html.substr(at);
  return rest.size() >= 9 && rest.substr(0, 9).equals_ignore_case("<!doctype");
}

bool build_document(Document& doc, StringView html, IScriptRunner* scripts) {
  doc.quirks_mode = !starts_with_doctype(html);
  TreeBuilder builder(doc, scripts, nullptr, StringView());
  const bool ok = builder.run(html);
  adopt_template_contents(doc);
  return ok;
}

Node* parse_fragment(Document& doc, StringView html, StringView context_tag) {
  Node* fragment = doc.create_fragment();
  if (!fragment) return nullptr;
  TreeBuilder builder(doc, nullptr, fragment, context_tag);
  const bool ok = builder.run(html);
  adopt_template_contents(doc);
  return ok ? fragment : nullptr;
}

DomError set_inner_html(Node* element, StringView html) {
  if (element->type != NodeType::Element && element->type != NodeType::Fragment) return DomError::HierarchyRequest;
  Node* fragment = parse_fragment(*element->owner, html, element->name.view());
  if (!fragment) return DomError::OutOfMemory;
  Node* target = element->is_tag("template") ? template_content(element) : element;   // (a template's markup is its content)
  if (!target) return DomError::OutOfMemory;
  while (target->first_child) remove_child(target, target->first_child);
  return append_child(target, fragment);
}

}  // namespace dom
