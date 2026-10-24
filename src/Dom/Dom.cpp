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

#include "Dom/Dom.hpp"
#include <new>
#include "Utility/Move.hpp"

namespace dom {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

void lowercase_into(StringView in, String* out) {
  out->clear();
  for (uint64_t i = 0; i < in.size(); ++i) out->push_back(lower(in[i]));
}

}  // namespace

// --- Document ----------------------------------------------------------------------------------------------------

Document::Document(allocators::IAllocator& allocator) : content_type(allocator), allocator_(allocator), nodes_(allocator), log_(allocator), watched_(allocator) { root_ = make(NodeType::Document, "#document"); }

Document::~Document() {
  for (uint64_t i = 0; i < nodes_.count(); ++i) {
    Node* n = nodes_[i];
    n->~Node();
    allocator_.deallocate(n);
  }
}

Node* Document::make(NodeType type, StringView name) {
  if (nodes_.count() >= kMaxNodes) return nullptr;
  void* mem = allocator_.allocate(sizeof(Node));
  if (!mem) return nullptr;
  Node* n = new (mem) Node(allocator_, type, this);
  if (!nodes_.push_back(n)) {
    n->~Node();
    allocator_.deallocate(mem);
    return nullptr;
  }
  n->serial = static_cast<uint32_t>(nodes_.count());
  if (type == NodeType::Element) lowercase_into(name, &n->name);
  else n->name.assign(name);
  return n;
}

Node* Document::create_element(StringView tag) { return make(NodeType::Element, tag); }

Node* Document::create_text(StringView data) {
  Node* n = make(NodeType::Text, "#text");
  if (n) n->data.assign(data);
  return n;
}

Node* Document::create_comment(StringView data) {
  Node* n = make(NodeType::Comment, "#comment");
  if (n) n->data.assign(data);
  return n;
}

Node* Document::create_fragment() { return make(NodeType::Fragment, "#document-fragment"); }

Node* Document::document_element() const {
  for (Node* c = root_->first_child; c; c = c->next)
    if (c->is_element()) return c;
  return nullptr;
}

Node* Document::head() const {
  Node* html = document_element();
  if (!html) return nullptr;
  for (Node* c = html->first_child; c; c = c->next)
    if (c->is_tag("head")) return c;
  return nullptr;
}

Node* Document::body() const {
  Node* html = document_element();
  if (!html) return nullptr;
  for (Node* c = html->first_child; c; c = c->next)
    if (c->is_tag("body") || c->is_tag("frameset")) return c;
  return nullptr;
}

Node* Document::element_by_id(StringView id) const {
  if (id.empty()) return nullptr;
  for (Node* n = root_; n; n = next_in_tree(n, root_)) {
    StringView value;
    if (n->is_element() && get_attribute(n, "id", &value) && value.equals(id)) return n;
  }
  return nullptr;
}

void Document::record(Mutation&& m) {
  if (!recording_ || log_.count() >= kMaxLog) return;
  for (uint64_t i = 0; i < watched_.count(); ++i) {
    if (!contains(watched_[i], m.target)) continue;
    if (!m.watchers.empty()) m.watchers.push_back(',');
    m.watchers.append_uint(watched_[i]->serial);
  }
  const bool first = log_.count() == 0;
  log_.push_back(util::move(m));
  if (first && hook_) hook_(hook_context_);
}

// --- Tree --------------------------------------------------------------------------------------------------------

namespace {

void touch(Node* n) {
  if (n && n->owner) n->owner->touch();
}

void unlink(Node* node) {
  Node* p = node->parent;
  if (!p) return;
  if (p->owner && p->owner->recording()) {
    Mutation m(p->owner->allocator());
    m.kind = Mutation::Kind::ChildList;
    m.target = p;
    m.previous = node->prev;
    m.next = node->next;
    m.removed = node;
    p->owner->record(util::move(m));
  }
  if (node->prev) node->prev->next = node->next;
  else p->first_child = node->next;
  if (node->next) node->next->prev = node->prev;
  else p->last_child = node->prev;
  node->parent = node->prev = node->next = nullptr;
}

// Puts `node` (not in a tree) before `reference` (a child of `parent`, or nullptr for the end).
void link_before(Node* parent, Node* node, Node* reference) {
  node->parent = parent;
  node->next = reference;
  node->prev = reference ? reference->prev : parent->last_child;
  if (node->prev) node->prev->next = node;
  else parent->first_child = node;
  if (reference) reference->prev = node;
  else parent->last_child = node;
  if (parent->owner && parent->owner->recording()) {
    Mutation m(parent->owner->allocator());
    m.kind = Mutation::Kind::ChildList;
    m.target = parent;
    m.previous = node->prev;
    m.next = node->next;
    m.added = node;
    parent->owner->record(util::move(m));
  }
}

bool can_have_children(const Node* n) { return n->type == NodeType::Element || n->type == NodeType::Document || n->type == NodeType::Fragment; }

}  // namespace

bool contains(const Node* ancestor, const Node* node) {
  for (const Node* n = node; n; n = n->parent)
    if (n == ancestor) return true;
  return false;
}

bool slot_assigned_nodes(const Node* slot, List<Node*>* out) {
  const Node* root = slot;
  while (root->parent) root = root->parent;
  if (!root->shadow_host) return false;
  StringView name;
  get_attribute(slot, "name", &name);
  for (Node* c = root->shadow_host->first_child; c; c = c->next) {
    StringView wanted;
    if (c->is_element()) get_attribute(c, "slot", &wanted);
    else if (!(c->is_text() && !c->data.empty())) continue;
    if (wanted.equals(name)) out->push_back(c);
  }
  return true;
}

bool is_connected(const Node* node) {
  const Node* n = node;
  for (uint32_t guard = 0; guard < 64; ++guard) {   // (a shadow tree is connected when its host is)
    while (n->parent) n = n->parent;
    if (n->type == NodeType::Document) return true;
    if (!n->shadow_host) return false;
    n = n->shadow_host;
  }
  return false;
}

void detach(Node* node) {
  if (!node->parent) return;
  touch(node);
  unlink(node);
}

DomError insert_before(Node* parent, Node* child, Node* reference) {
  if (!can_have_children(parent) || child->type == NodeType::Document) return DomError::HierarchyRequest;
  if (reference && reference->parent != parent) return DomError::NotFound;
  if (contains(child, parent)) return DomError::HierarchyRequest;   // (into itself or below itself)
  if (child->type == NodeType::Text && parent->type == NodeType::Document) return DomError::HierarchyRequest;
  if (reference == child) reference = child->next;
  if (child->type == NodeType::Fragment) {
    while (Node* c = child->first_child) {
      unlink(c);
      link_before(parent, c, reference);
    }
    touch(parent);
    return DomError::None;
  }
  unlink(child);
  link_before(parent, child, reference);
  touch(parent);
  return DomError::None;
}

DomError append_child(Node* parent, Node* child) { return insert_before(parent, child, nullptr); }

DomError remove_child(Node* parent, Node* child) {
  if (child->parent != parent) return DomError::NotFound;
  touch(parent);
  unlink(child);
  return DomError::None;
}

DomError replace_child(Node* parent, Node* child, Node* old_child) {
  if (old_child->parent != parent) return DomError::NotFound;
  if (child == old_child) return DomError::None;
  Node* after = old_child->next;
  if (after == child) after = child->next;
  const DomError e = insert_before(parent, child, old_child);
  if (e != DomError::None) return e;
  unlink(old_child);
  return DomError::None;
}

Node* clone_node(Node* node, bool deep) {
  Document& doc = *node->owner;
  Node* copy = nullptr;
  switch (node->type) {
    case NodeType::Element:
      copy = doc.create_element(node->name.view());
      if (copy) {
        copy->ns_set = node->ns_set;
        copy->ns.assign(node->ns.view());
        copy->qname.assign(node->qname.view());
        for (uint64_t i = 0; i < node->attrs.count(); ++i) {
          if (set_attribute(copy, node->attrs[i].name.view(), node->attrs[i].value.view()) != DomError::None) return nullptr;
          if (!node->attrs[i].ns.empty() && !copy->attrs[copy->attrs.count() - 1].ns.assign(node->attrs[i].ns.view())) return nullptr;
        }
      }
      if (copy && node->is_tag("template")) {
        copy->content = node->content ? clone_node(node->content, deep) : doc.create_fragment();
        if (!copy->content) return nullptr;
      }
      if (copy) {   // (a copy of a control keeps what was typed into it, not the focus)
        copy->form_state = node->form_state & ~0x08;
        if (node->form_state & 1) copy->data.assign(node->data.view());
      }
      break;
    case NodeType::Text: copy = doc.create_text(node->data.view()); if (copy) { copy->dom_type = node->dom_type; if (node->dom_type) copy->name.assign(node->name.view()); } break;
    case NodeType::Comment: copy = doc.create_comment(node->data.view()); if (copy) { copy->dom_type = node->dom_type; if (node->dom_type) copy->name.assign(node->name.view()); } break;
    case NodeType::Fragment: copy = doc.create_fragment(); break;
    case NodeType::Document: return nullptr;
  }
  if (!copy) return nullptr;
  if (deep) {
    for (Node* c = node->first_child; c; c = c->next) {
      Node* cc = clone_node(c, true);
      if (!cc) return nullptr;
      link_before(copy, cc, nullptr);
    }
  }
  return copy;
}

Node* first_element_child(const Node* node) {
  for (Node* c = node->first_child; c; c = c->next)
    if (c->is_element()) return c;
  return nullptr;
}

Node* last_element_child(const Node* node) {
  for (Node* c = node->last_child; c; c = c->prev)
    if (c->is_element()) return c;
  return nullptr;
}

Node* next_element_sibling(const Node* node) {
  for (Node* c = node->next; c; c = c->next)
    if (c->is_element()) return c;
  return nullptr;
}

Node* previous_element_sibling(const Node* node) {
  for (Node* c = node->prev; c; c = c->prev)
    if (c->is_element()) return c;
  return nullptr;
}

uint32_t child_element_count(const Node* node) {
  uint32_t n = 0;
  for (Node* c = node->first_child; c; c = c->next) n += c->is_element() ? 1 : 0;
  return n;
}

Node* next_in_tree(const Node* node, const Node* root) {
  if (node->first_child) return node->first_child;
  for (const Node* n = node; n && n != root; n = n->parent)
    if (n->next) return n->next;
  return nullptr;
}

int compare_order(const Node* a, const Node* b) {
  if (a == b) return 0;
  // the paths from the root
  const Node* pa[512];
  const Node* pb[512];
  uint32_t na = 0, nb = 0;
  for (const Node* n = a; n && na < 512; n = n->parent) pa[na++] = n;
  for (const Node* n = b; n && nb < 512; n = n->parent) pb[nb++] = n;
  if (pa[na - 1] != pb[nb - 1]) return 0;
  while (na > 0 && nb > 0 && pa[na - 1] == pb[nb - 1]) {
    --na;
    --nb;
  }
  if (na == 0) return -1;   // a contains b: a comes first
  if (nb == 0) return 1;
  for (const Node* s = pa[na - 1]->next; s; s = s->next)
    if (s == pb[nb - 1]) return -1;
  return 1;
}

// --- Attributes --------------------------------------------------------------------------------------------------

namespace {

int64_t find_attr(const Node* e, StringView name) {
  for (uint64_t i = 0; i < e->attrs.count(); ++i) {
    const StringView n = e->attrs[i].name.view();
    if (n.size() != name.size()) continue;
    bool same = true;
    for (uint64_t k = 0; k < n.size() && same; ++k) same = n[k] == lower(name[k]);
    if (same) return static_cast<int64_t>(i);
  }
  return -1;
}

}  // namespace

bool get_attribute(const Node* e, StringView name, StringView* value) {
  const int64_t i = find_attr(e, name);
  if (i < 0) return false;
  *value = e->attrs[static_cast<uint64_t>(i)].value.view();
  return true;
}

bool has_attribute(const Node* e, StringView name) { return find_attr(e, name) >= 0; }

namespace {
void record_attribute(Node* e, StringView name, const String* old_value) {
  if (!e->owner || !e->owner->recording()) return;
  Mutation m(e->owner->allocator());
  m.kind = Mutation::Kind::Attributes;
  m.target = e;
  lowercase_into(name, &m.attribute);
  if (old_value) {
    m.old_value.assign(old_value->view());
    m.had_old_value = true;
  }
  e->owner->record(util::move(m));
}
}  // namespace

DomError set_attribute(Node* e, StringView name, StringView value) {
  const int64_t i = find_attr(e, name);
  if (i >= 0) {
    String old(e->owner->allocator());
    old.assign(e->attrs[static_cast<uint64_t>(i)].value.view());
    if (!e->attrs[static_cast<uint64_t>(i)].value.assign(value)) return DomError::OutOfMemory;
    record_attribute(e, name, &old);
  } else {
    if (name.empty()) return DomError::HierarchyRequest;
    Attribute a(e->owner->allocator());
    lowercase_into(name, &a.name);
    if (!a.value.assign(value) || !e->attrs.push_back(util::move(a))) return DomError::OutOfMemory;
    record_attribute(e, name, nullptr);
  }
  touch(e);
  return DomError::None;
}

bool remove_attribute(Node* e, StringView name) {
  const int64_t i = find_attr(e, name);
  if (i < 0) return false;
  String old(e->owner->allocator());
  old.assign(e->attrs[static_cast<uint64_t>(i)].value.view());
  e->attrs.remove_at(static_cast<uint64_t>(i));
  record_attribute(e, name, &old);
  touch(e);
  return true;
}

bool has_class(const Node* e, StringView word) {
  StringView classes;
  if (word.empty() || !get_attribute(e, "class", &classes)) return false;
  uint64_t i = 0;
  while (i < classes.size()) {
    while (i < classes.size() && is_space(classes[i])) ++i;
    const uint64_t start = i;
    while (i < classes.size() && !is_space(classes[i])) ++i;
    if (i > start && classes.substr(start, i - start).equals(word)) return true;
  }
  return false;
}

// --- Text and markup ---------------------------------------------------------------------------------------------

void text_content(const Node* node, String* out) {
  out->clear();
  if (node->type == NodeType::Text || node->type == NodeType::Comment) {
    out->assign(node->data.view());
    return;
  }
  for (const Node* n = node->first_child ? node->first_child : nullptr; n; n = next_in_tree(n, node))
    if (n->type == NodeType::Text) out->append(n->data.view());
}

DomError set_text_content(Node* node, StringView text) {
  if (node->type == NodeType::Text || node->type == NodeType::Comment) {
    if (node->owner && node->owner->recording()) {
      Mutation m(node->owner->allocator());
      m.kind = Mutation::Kind::CharacterData;
      m.target = node;
      m.old_value.assign(node->data.view());
      m.had_old_value = true;
      node->owner->record(util::move(m));
    }
    if (!node->data.assign(text)) return DomError::OutOfMemory;
    touch(node);
    return DomError::None;
  }
  if (node->type == NodeType::Document) return DomError::None;
  while (node->first_child) unlink(node->first_child);
  if (!text.empty()) {
    Node* t = node->owner->create_text(text);
    if (!t) return DomError::OutOfMemory;
    link_before(node, t, nullptr);
  }
  touch(node);
  return DomError::None;
}

bool is_void_tag(StringView tag) {
  static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
  for (const char* v : kVoid)
    if (tag.equals(v)) return true;
  return false;
}

namespace {

void escape_into(StringView s, bool attribute, String* out) {
  for (uint64_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '&') out->append("&amp;");
    else if (c == '"' && attribute) out->append("&quot;");
    else if (c == '<' && !attribute) out->append("&lt;");
    else if (c == '>' && !attribute) out->append("&gt;");
    else out->push_back(c);
  }
}

bool is_raw_text_tag(StringView tag) { return tag.equals("script") || tag.equals("style") || tag.equals("xmp") || tag.equals("iframe") || tag.equals("noembed") || tag.equals("noframes"); }

void serialize(const Node* n, String* out, uint32_t depth) {
  if (depth > 400) return;
  switch (n->type) {
    case NodeType::Text: {
      const Node* p = n->parent;
      if (p && p->is_element() && is_raw_text_tag(p->name.view())) out->append(n->data.view());
      else escape_into(n->data.view(), false, out);
      return;
    }
    case NodeType::Comment:
      if (n->dom_type == 7) {
        out->append("<?");
        out->append(n->name.view());
        out->push_back(' ');
        out->append(n->data.view());
        out->push_back('>');
        return;
      }
      out->append("<!--");
      out->append(n->data.view());
      out->append("-->");
      return;
    case NodeType::Element: {
      const StringView tag = n->qname.empty() ? n->name.view() : n->qname.view();
      out->push_back('<');
      out->append(tag);
      for (uint64_t i = 0; i < n->attrs.count(); ++i) {
        out->push_back(' ');
        out->append(n->attrs[i].name.view());
        out->append("=\"");
        escape_into(n->attrs[i].value.view(), true, out);
        out->push_back('"');
      }
      out->push_back('>');
      if (is_void_tag(n->name.view())) return;
      for (const Node* c = n->content ? n->content->first_child : n->first_child; c; c = c->next) serialize(c, out, depth + 1);
      out->append("</");
      out->append(tag);
      out->push_back('>');
      return;
    }
    default:
      for (const Node* c = n->first_child; c; c = c->next) serialize(c, out, depth + 1);
      return;
  }
}

}  // namespace

void inner_html(const Node* node, String* out) {
  out->clear();
  if (node->type == NodeType::Text || node->type == NodeType::Comment) return;
  for (const Node* c = node->content ? node->content->first_child : node->first_child; c; c = c->next) serialize(c, out, 0);
}

void outer_html(const Node* node, String* out) {
  out->clear();
  serialize(node, out, 0);
}

}  // namespace dom
