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

#include "Layout/BoxBuilder.hpp"
#include "Layout/Table.hpp"
#include <initializer_list>

namespace browser {
// The style of an element given its parent's (Renderer.cpp: the defaults of its kind, then the cascade and the style attribute).
Style derive_style(const Style& parent, const dom::Node* n, const StyleSheet& sheet);
}  // namespace browser

namespace layout {

using browser::Display;
using browser::Float;
using browser::Layout;
using browser::Length;
using browser::ObjectKind;
using browser::Overflow;
using browser::Position;
using browser::Style;

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool tag_in(const dom::Node* n, std::initializer_list<const char*> tags) {
  for (const char* t : tags)
    if (n->is_tag(t)) return true;
  return false;
}

// Elements that make nothing of their own: they are not content (script is run elsewhere, noscript is for browsers that run none).
bool makes_no_box(const dom::Node* n) { return tag_in(n, {"head", "script", "style", "template", "title", "meta", "link", "base", "noscript", "datalist"}); }

// The text of a whole-number attribute (plain digits, as width="120" is written): false for anything else, including percentages.
bool pixel_attribute(const dom::Node* n, const char* name, int64_t* out) {
  StringView v;
  if (!dom::get_attribute(n, name, &v)) return false;
  v = v.trimmed();
  if (v.empty() || v.size() > 6) return false;
  int64_t value = 0;
  for (uint64_t i = 0; i < v.size(); ++i) {
    if (v[i] < '0' || v[i] > '9') return false;
    value = value * 10 + (v[i] - '0');
  }
  *out = value;
  return true;
}

// The size a form control has before anything else says: the typical size browsers give it.
bool control_size(const dom::Node* n, int64_t* w, int64_t* h) {
  if (n->is_tag("input")) {
    StringView type;
    dom::get_attribute(n, "type", &type);
    if (type.equals_ignore_case("checkbox") || type.equals_ignore_case("radio")) { *w = 13; *h = 13; return true; }
    if (type.equals_ignore_case("range")) { *w = 129; *h = 16; return true; }
    if (type.equals_ignore_case("color")) { *w = 50; *h = 27; return true; }
    if (type.equals_ignore_case("file")) { *w = 250; *h = 21; return true; }
    if (type.equals_ignore_case("image")) { *w = 0; *h = 0; return true; }
    int64_t size = 20;
    if (pixel_attribute(n, "size", &size) && size < 1) size = 20;
    *w = size * 7 + 3 + 4;
    *h = 21;
    return true;
  }
  if (n->is_tag("textarea")) { *w = 182; *h = 36; return true; }
  if (n->is_tag("select")) { *w = 100; *h = 21; return true; }
  if (n->is_tag("progress")) { *w = 160; *h = 16; return true; }
  if (n->is_tag("meter")) { *w = 80; *h = 16; return true; }
  return false;
}

void format_roman(uint32_t v, bool upper, String* out) {
  static const struct { uint32_t v; const char* s; } kRoman[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
                                                                  {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
  for (const auto& r : kRoman)
    while (v >= r.v) {
      for (const char* c = r.s; *c; ++c) out->push_back(upper ? static_cast<char>(*c - 32) : *c);
      v -= r.v;
    }
}

// The marker of a list item: a bullet, or its number in the list's counting style, then a full stop and a space.
void marker_text(uint8_t type, bool ordered, int64_t number, String* out) {
  if (type == 0 || type == 1 || type == 2 || type == 8) type = ordered ? 3 : type;
  if (type == 8) return;
  if (!ordered || type < 3 || type == 8) {
    out->append(type == 1 ? "\xE2\x97\xA6" : type == 2 ? "\xE2\x96\xAA" : "\xE2\x80\xA2");   // circle, square, disc
    out->push_back(' ');
    return;
  }
  const uint64_t magnitude = static_cast<uint64_t>(number < 0 ? -number : number);
  if ((type == 4 || type == 5) && number > 0) {
    char letters[16];
    uint32_t n = 0;
    uint64_t v = magnitude;
    while (v > 0 && n < sizeof(letters)) {
      --v;
      letters[n++] = static_cast<char>((type == 4 ? 'a' : 'A') + v % 26);
      v /= 26;
    }
    while (n > 0) out->push_back(letters[--n]);
  } else if ((type == 6 || type == 7) && number > 0 && number < 4000) {
    format_roman(static_cast<uint32_t>(magnitude), type == 7, out);
  } else {
    if (number < 0) out->push_back('-');
    if (type == 9 && magnitude < 10) out->push_back('0');
    out->append_uint(magnitude);
  }
  out->append(". ");
}

class Builder {
 public:
  Builder(dom::Document& doc, const browser::StyleSheet& sheet, const BuildOptions& options, BoxTree* tree) : doc_(doc), sheet_(sheet), options_(options), tree_(*tree) {}

  bool run() {
    tree_.clear();
    const dom::Node* html = doc_.document_element();
    if (!html) return false;
    const Style root_style;
    const BoxId root = tree_.create(BoxKind::Root, 0, root_style);
    if (root == kNoBox) return false;
    tree_.box(root).establishes_bfc = true;
    build_node(html, root_style, root, 1);
    if (failed_) return false;
    finalize(root);
    return !failed_;
  }

 private:
  allocators::IAllocator& allocator() const { return tree_.allocator(); }

  // The nodes an element shows as its children: a shadow host shows its shadow tree, a slot the nodes assigned to it (or its own children).
  void effective_children(const dom::Node* container, List<const dom::Node*>* out, uint32_t depth) {
    const dom::Node* first = container->shadow ? container->shadow->first_child : container->first_child;
    for (const dom::Node* c = first; c && out->count() < 20000; c = c->next) {
      if (c->is_tag("slot") && depth < 8) {
        List<dom::Node*> assigned(allocator());
        if (dom::slot_assigned_nodes(c, &assigned) && assigned.count() > 0) {
          for (uint64_t i = 0; i < assigned.count(); ++i) failed_ |= !out->push_back(assigned[i]);
        } else {
          effective_children(c, out, depth + 1);
        }
        continue;
      }
      failed_ |= !out->push_back(c);
    }
  }

  void build_children(const dom::Node* n, const Style& style, BoxId parent, uint32_t depth) {
    List<const dom::Node*> kids(allocator());
    effective_children(n, &kids, 0);
    for (uint64_t i = 0; i < kids.count() && !failed_; ++i) build_node(kids[i], style, parent, depth);
  }

  void build_node(const dom::Node* n, const Style& parent_style, BoxId parent, uint32_t depth) {
    if (failed_) return;
    if (n->type == dom::NodeType::Text) {
      if (n->data.size() == 0) return;
      const BoxId id = tree_.create(BoxKind::Text, n->serial, parent_style.inherit());
      if (id == kNoBox) { failed_ = true; return; }
      tree_.box(id).text.assign(n->data.view());
      failed_ |= !tree_.append(parent, id);
      return;
    }
    if (!n->is_element() || makes_no_box(n)) return;
    Style s = browser::derive_style(parent_style, n, sheet_);
    if (s.display == Display::None) return;
    apply_table_presentation(n, parent_style, sheet_, &s);   // (<table>, <tr>, <td> ... are table boxes: TableFixup.cpp)
    if (s.contents) {   // display: contents: no box of its own, its children are the parent's
      build_children(n, s, parent, depth);
      return;
    }
    if (depth >= options_.max_depth || tree_.count() >= options_.max_boxes) return;

    if (n->is_tag("br")) {
      const BoxId id = tree_.create(BoxKind::LineBreak, n->serial, s);
      if (id == kNoBox) { failed_ = true; return; }
      failed_ |= !tree_.append(parent, id);
      return;
    }
    if (n->is_tag("input")) {
      StringView type;
      if (dom::get_attribute(n, "type", &type) && type.equals_ignore_case("hidden")) return;
    }
    if (build_replaced(n, s, parent)) return;

    const bool out_of_flow = s.position == Position::Absolute || s.position == Position::Fixed;
    const bool floated = s.float_side != Float::None && !out_of_flow;   // (a positioned box is not floated)
    BoxKind kind = BoxKind::Block;
    if (out_of_flow || floated) kind = BoxKind::Block;                      // (blockified)
    else if (s.inline_box || tag_in(n, {"button", "select", "textarea"})) kind = BoxKind::InlineBlock;
    else if (s.display == Display::Inline) kind = BoxKind::Inline;
    const BoxId id = tree_.create(kind, n->serial, s);
    if (id == kNoBox) { failed_ = true; return; }
    Box& b = tree_.box(id);
    b.out_of_flow = out_of_flow;
    b.floated = floated;
    b.establishes_bfc = out_of_flow || floated || kind == BoxKind::InlineBlock || s.overflow_x != Overflow::Visible || s.overflow_y != Overflow::Visible ||
                        s.layout == Layout::Flex || s.layout == Layout::Grid || s.layout == Layout::Table || s.column_count > 0 || s.flow_root;
    failed_ |= !tree_.append(parent, id);
    read_table_attributes(n, &b);
    if (n->is_tag("hr")) {   // (the line of a rule: a box with a border and nothing in it)
      Style& hs = tree_.box(id).style;
      hs.display = Display::Block;
      tree_.box(id).kind = BoxKind::Block;
      if (hs.border_style[0] == browser::BorderStyle::None) {
        hs.border_px100[0] = hs.border_px100[2] = 100;
        hs.border_style[0] = hs.border_style[2] = browser::BorderStyle::Solid;
        hs.border_color[0] = hs.border_color[2] = 0xFF9A9A9Au;
      }
      if (hs.margin[0].px100 == 0) hs.margin[0] = hs.margin[2] = Length::px(800);
      return;
    }
    if (s.display == Display::ListItem && !s.list_none) add_marker(n, s, id);
    build_children(n, s, id, depth + 1);
    finalize(id);
    failed_ |= !finish_table(tree_, id);
  }

  // A picture, video, canvas, svg, iframe or form control: a box with a size of its own and nothing inside it. False for anything else.
  bool build_replaced(const dom::Node* n, const Style& s, BoxId parent) {
    ObjectKind kind = ObjectKind::None;
    int64_t natural_w = 0, natural_h = 0;
    bool has_w = false, has_h = false, shape_only = false;
    if (tag_in(n, {"img", "video", "audio", "canvas", "svg"}) && options_.objects) {
      browser::ObjectInfo info;
      if (options_.objects->object_info(n, &info) && info.kind != ObjectKind::None && (info.ready || info.kind != ObjectKind::Image)) {
        kind = info.kind;
        natural_w = info.width;
        natural_h = info.height;
        has_w = info.width != 0;
        has_h = info.height != 0;
        shape_only = info.shape_only;
      } else if (n->is_tag("img")) {   // a picture that is not there: its alternative text, if it has some
        StringView alt;
        if (dom::get_attribute(n, "alt", &alt) && !alt.empty()) {
          const BoxId id = tree_.create(BoxKind::Inline, n->serial, s);
          if (id == kNoBox) { failed_ = true; return true; }
          const BoxId text = tree_.create(BoxKind::Text, 0, s.inherit());
          if (text == kNoBox) { failed_ = true; return true; }
          tree_.box(text).text.assign(alt);
          failed_ |= !tree_.append(id, text) || !tree_.append(parent, id);
          tree_.box(id).inline_content = true;
          return true;
        }
        kind = ObjectKind::Image;   // (no alternative text: the size the page gives it is kept, empty)
      }
    }
    bool is_replaced = kind != ObjectKind::None || n->is_tag("img") || n->is_tag("iframe") || n->is_tag("embed") || n->is_tag("object");
    if (n->is_tag("iframe")) {
      natural_w = 300;
      natural_h = 150;
      has_w = has_h = true;
    }
    int64_t cw = 0, ch = 0;
    if (control_size(n, &cw, &ch)) {
      is_replaced = true;
      natural_w = cw;
      natural_h = ch;
      has_w = has_h = true;
    }
    if (!is_replaced) return false;
    const bool out_of_flow = s.position == Position::Absolute || s.position == Position::Fixed;
    const bool floated = s.float_side != Float::None && !out_of_flow;   // (a positioned box is not floated)
    const BoxId id = tree_.create(BoxKind::Replaced, n->serial, s);
    if (id == kNoBox) { failed_ = true; return true; }
    Box& b = tree_.box(id);
    b.out_of_flow = out_of_flow;
    b.floated = floated;
    b.establishes_bfc = true;
    b.object_kind = static_cast<uint8_t>(kind);
    b.natural_width = LayoutUnit::from_px(natural_w);
    b.natural_height = LayoutUnit::from_px(natural_h);
    b.has_natural_width = has_w;
    b.has_natural_height = has_h;
    b.fill_width = shape_only;
    // the width and height attributes say what the style does not
    int64_t attr = 0;
    if (b.style.width.is_auto() && pixel_attribute(n, "width", &attr)) b.style.width = Length::px(static_cast<int32_t>(attr > 100000 ? 100000 : attr) * 100);
    if (b.style.height.is_auto() && pixel_attribute(n, "height", &attr)) b.style.height = Length::px(static_cast<int32_t>(attr > 100000 ? 100000 : attr) * 100);
    failed_ |= !tree_.append(parent, id);
    return true;
  }

  // The bullet or number of a list item: inside the item's first line (list-style-position: inside) or out in the margin.
  void add_marker(const dom::Node* li, const Style& s, BoxId item) {
    const dom::Node* list = li->parent;
    const bool ordered = list && list->is_tag("ol");
    int64_t number = 1;
    if (ordered) {
      int64_t first = 1;
      StringView start;
      if (dom::get_attribute(list, "start", &start)) {
        uint64_t v = 0;
        if (start.trimmed().parse_uint(&v)) first = static_cast<int64_t>(v);
      }
      int64_t index = 0;
      for (const dom::Node* c = list->first_child; c && c != li; c = c->next)
        if (c->is_tag("li")) ++index;
      number = dom::has_attribute(list, "reversed") ? first - index : first + index;
    }
    String text(allocator());
    marker_text(s.list_type, ordered, number, &text);
    if (text.size() == 0) return;
    const BoxId id = tree_.create(s.list_inside ? BoxKind::Text : BoxKind::Marker, 0, s.inherit());
    if (id == kNoBox) { failed_ = true; return; }
    tree_.box(id).text.assign(text.view());
    failed_ |= !tree_.append(item, id);
  }

  static bool only_collapsible_space(const Box& b) {
    if (b.kind != BoxKind::Text || b.style.pre || b.style.white_space == 2 || b.style.white_space == 3 || b.style.white_space == 5) return false;
    for (uint64_t i = 0; i < b.text.size(); ++i)
      if (!is_space(b.text[i])) return false;
    return true;
  }

  // Arranges the children of a box that has all of them: block containers hold blocks or inline content, not both.
  void finalize(BoxId id) {
    List<BoxId> table_cells(allocator());   // anonymous cells made for loose content in a table (TableFixup.cpp)
    failed_ |= !fixup_table_parts(tree_, id, &table_cells);
    for (uint64_t i = 0; i < table_cells.count(); ++i) finalize(table_cells[i]);
    // The items of a flex container are blockified (an inline, inline-block or picture child is an item of its own; a float does not apply),
    // and text becomes anonymous items, so a flex container never lays out inline content itself.
    const bool flex = (tree_.box(id).style.layout == Layout::Flex || tree_.box(id).style.layout == Layout::Grid) && tree_.box(id).kind != BoxKind::Inline;   // (and a grid container's)
    if (flex) {
      for (uint64_t i = 0; i < tree_.box(id).children.count(); ++i) {
        Box& c = tree_.box(tree_.box(id).children[i]);
        if (c.kind == BoxKind::Marker || c.kind == BoxKind::Text || c.kind == BoxKind::LineBreak || c.out_of_flow) continue;
        c.floated = false;
        if (c.kind == BoxKind::Inline || c.kind == BoxKind::InlineBlock) c.kind = BoxKind::Block;
        if (c.kind == BoxKind::Replaced) c.style.display = Display::Block;
        c.establishes_bfc = true;
      }
    }
    bool has_block = false;
    for (uint64_t i = 0; i < tree_.box(id).children.count(); ++i) {
      const Box& c = tree_.box(tree_.box(id).children[i]);
      if (c.is_block_level() && !c.out_of_flow && !c.floated) has_block = true;
    }
    Box& b = tree_.box(id);
    if (!has_block && !flex) {
      b.inline_content = true;
      return;
    }
    if (b.kind == BoxKind::Inline) b.kind = BoxKind::Block;   // (an inline with blocks inside: see the note in BoxBuilder.hpp)
    b.inline_content = false;
    List<BoxId> arranged(allocator()), run(allocator());
    auto flush_run = [&]() {
      if (run.count() == 0) return;
      bool blank = true;
      for (uint64_t i = 0; i < run.count() && blank; ++i) blank = only_collapsible_space(tree_.box(run[i]));
      if (!blank) {
        const BoxId anon = tree_.create(BoxKind::Anonymous, 0, tree_.box(id).style.inherit());
        if (anon == kNoBox) { failed_ = true; return; }
        Style& as = tree_.box(anon).style;
        as.display = Display::Block;
        tree_.box(anon).inline_content = true;
        tree_.box(anon).establishes_bfc = flex;   // (an item of a flex or grid container)
        tree_.box(anon).parent = id;
        for (uint64_t i = 0; i < run.count(); ++i) failed_ |= !tree_.append(anon, run[i]);
        failed_ |= !arranged.push_back(anon);
      }
      run.clear();
    };
    for (uint64_t i = 0; i < tree_.box(id).children.count() && !failed_; ++i) {
      const BoxId cid = tree_.box(id).children[i];
      const Box& c = tree_.box(cid);
      if (c.kind == BoxKind::Marker) {
        failed_ |= !arranged.push_back(cid);
      } else if (c.is_block_level() && !c.out_of_flow && !c.floated) {
        flush_run();
        failed_ |= !arranged.push_back(cid);
      } else if (c.out_of_flow || c.floated) {
        if (run.count() > 0) failed_ |= !run.push_back(cid);
        else failed_ |= !arranged.push_back(cid);
      } else {
        failed_ |= !run.push_back(cid);
      }
    }
    flush_run();
    Box& again = tree_.box(id);
    again.children.clear();
    for (uint64_t i = 0; i < arranged.count(); ++i) {
      failed_ |= !again.children.push_back(arranged[i]);
      tree_.box(arranged[i]).parent = id;
    }
  }

  dom::Document& doc_;
  const browser::StyleSheet& sheet_;
  const BuildOptions& options_;
  BoxTree& tree_;
  bool failed_ = false;
};

}  // namespace

bool build_box_tree(dom::Document& doc, const browser::StyleSheet& sheet, const BuildOptions& options, BoxTree* out) {
  Builder builder(doc, sheet, options, out);
  return builder.run();
}

}  // namespace layout
