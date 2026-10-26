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

// The document tree of a page: a Document with Elements, Text and Comment nodes, built by the HTML reader, changed by scripts, and read by the renderer
// and by selectors. The model follows the DOM standard in what matters to a page - nodes with a parent, first / last child and previous / next
// sibling; elements with a lowercase tag name and attributes; text and comments with data; document fragments; the tree operations (append, insert,
// remove, replace, clone); textContent, innerHTML and outerHTML in both directions - without what has no use here (namespaces, shadow trees, ranges).
//
// A Document owns all of its nodes, attached or not, and frees them together. Every change made through the functions here bumps the document's
// version(), which is how the browser knows the page has to be drawn again. The script engine hangs its wrapper object for a node on Node::script.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace dom {

class Document;

enum class NodeType : uint8_t { Element = 1, Text = 3, Comment = 8, Document = 9, Fragment = 11 };

struct Attribute {
  explicit Attribute(allocators::IAllocator& allocator) : name(allocator), value(allocator), ns(allocator) {}
  String name, value;
  String ns;   // the namespace of an attribute made with setAttributeNS (empty: none)
};

// Bits of Node::ui_state.
enum UiState : uint8_t { kUiHover = 1, kUiActive = 2 };

struct Node {
  Node(allocators::IAllocator& allocator, NodeType t, Document* d) : type(t), owner(d), name(allocator), data(allocator), attrs(allocator), ns(allocator), qname(allocator) {}

  NodeType type;
  Node *parent = nullptr, *first_child = nullptr, *last_child = nullptr, *prev = nullptr, *next = nullptr;
  Document* owner;
  uint32_t serial = 0;       // 1, 2, ... in the order the document made its nodes (0 never names a node)
  String name;               // elements: the lowercase tag name; others: "#text", "#comment", "#document", "#document-fragment"
  String data;               // text and comments
  List<Attribute> attrs;     // elements
  // elements: the namespace and the qualified name as the page wrote them. An element of HTML has neither (ns_set is false: its namespace is XHTML, its name `name`).
  String ns, qname;
  bool ns_set = false;
  uint8_t dom_type = 0;      // a comment or text node that stands for a node of another type: 7 a processing instruction (name is its target), 4 a CDATA section
  void* script = nullptr;    // the script engine's object for this node
  Node* content = nullptr;   // <template>: the fragment that holds what is inside it (it is not part of the page)
  Node* shadow = nullptr;    // elements: the shadow root (a fragment) that is drawn in place of the children
  Node* shadow_host = nullptr;   // a shadow root: the element it belongs to
  bool template_content = false;   // a fragment: it is the content of a <template> (its nodes belong to a document of their own as far as scripts can tell)
  bool shadow_closed = false;    // a shadow root made with mode "closed
  uint16_t caret = 0xFFFF, anchor = 0xFFFF;   // a text control: where the caret is and the other end of the selection (0xFFFF: at the end)
  uint8_t form_state = 0;    // elements: what the user (or a script) changed in a form control, and whether it is focused (FormState bits, Form.hpp)
  uint8_t ui_state = 0;      // elements: kUiHover while the pointer is over it (or a descendant), kUiActive while it is pressed

  bool is_element() const { return type == NodeType::Element; }
  bool is_text() const { return type == NodeType::Text; }
  bool is_tag(const char* tag) const { return type == NodeType::Element && name.view().equals(tag); }
};

// A change made to the tree, kept while someone wants to know (MutationObserver, custom elements): see Document::set_recording().
struct Mutation {
  enum class Kind : uint8_t { ChildList, Attributes, CharacterData };
  explicit Mutation(allocators::IAllocator& allocator) : attribute(allocator), old_value(allocator), watchers(allocator) {}
  Kind kind = Kind::ChildList;
  Node* target = nullptr;                 // the parent (child list), the element (attributes), the text or comment (character data)
  Node *previous = nullptr, *next = nullptr;   // child list: the siblings of the nodes that were added or removed
  Node *added = nullptr, *removed = nullptr;   // child list: the node that went in or out (one record for each)
  String attribute;                        // attributes: the (lowercase) name
  String old_value;                        // attributes and character data: what it was
  bool had_old_value = false;              // attributes: false if the attribute was added
  String watchers;                         // the serials (comma separated) of the watched nodes that were this target or above it when the change was made
};

// What the tree operations say when they refuse.
enum class DomError : uint8_t { None, HierarchyRequest, NotFound, OutOfMemory, TooManyNodes };

class Document {
 public:
  static constexpr uint32_t kMaxNodes = 400000;

  explicit Document(allocators::IAllocator& allocator);
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  allocators::IAllocator& allocator() const { return allocator_; }
  Node* root() const { return root_; }                  // the Document node
  // True for a page read without a doctype: it is laid out the way old pages expect (a table does not take its text alignment from what it is in).
  bool quirks_mode = false;
  // What contentType says when the document is not HTML ("application/xml" ...); empty for a page of HTML.
  String content_type;
  Node* document_element() const;                       // <html>
  Node* head() const;
  Node* body() const;
  // Makes a node that is not in the tree yet. nullptr when memory or the node limit ran out.
  Node* create_element(StringView tag);                 // (the name is lowercased)
  Node* create_text(StringView data);
  Node* create_comment(StringView data);
  Node* create_fragment();
  Node* by_serial(uint32_t serial) const { return serial >= 1 && serial <= nodes_.count() ? nodes_[serial - 1] : nullptr; }
  uint32_t node_count() const { return static_cast<uint32_t>(nodes_.count()); }
  // True when the node limit has been reached (the page is then cut off where it was).
  bool full() const { return nodes_.count() >= kMaxNodes; }
  // The first element in the tree with that id (the tree order), or nullptr.
  Node* element_by_id(StringView id) const;

  // Changes through the functions below bump this.
  uint64_t version() const { return version_; }
  void touch() { ++version_; }

  // While recording, every change to the tree is also written to a log (at most kMaxLog entries; later ones are lost). `hook` is called when the log goes
  // from empty to not empty. take_log() moves the log out.
  static constexpr uint32_t kMaxLog = 20000;
  void set_recording(bool on, void (*hook)(void*) = nullptr, void* context = nullptr) {
    recording_ = on;
    if (hook) hook_ = hook;
    if (context) hook_context_ = context;
  }
  bool recording() const { return recording_; }
  // Nodes somebody observes (a MutationObserver's targets): each record says which of them it happened in.
  void watch(Node* node) {
    for (uint64_t i = 0; i < watched_.count(); ++i)
      if (watched_[i] == node) return;
    watched_.push_back(node);
  }
  void unwatch_all() { watched_.clear(); }
  void record(Mutation&& m);
  List<Mutation>& log() { return log_; }

 private:
  Node* make(NodeType type, StringView name);

  allocators::IAllocator& allocator_;
  Node* root_ = nullptr;
  List<Node*> nodes_;
  uint64_t version_ = 1;
  List<Mutation> log_;
  List<Node*> watched_;
  bool recording_ = false;
  void (*hook_)(void*) = nullptr;
  void* hook_context_ = nullptr;
};

// --- Tree operations. They follow the standard: a node is first taken out of where it is; a fragment's children go in its place; nothing can be put
// inside itself or its descendants. `child` of insert_before may be nullptr (append).
DomError append_child(Node* parent, Node* child);
DomError insert_before(Node* parent, Node* child, Node* reference);
DomError remove_child(Node* parent, Node* child);
DomError replace_child(Node* parent, Node* child, Node* old_child);
// Takes `node` out of its parent (if it has one).
void detach(Node* node);
// A copy (with the children too if `deep`) that is in no tree. nullptr if memory ran out.
Node* clone_node(Node* node, bool deep);
// The same, made in `doc` (importNode).
Node* clone_into(Document& doc, Node* node, bool deep);
// Moves a node and what is under it into another document (adoptNode): a copy made there, `moved(old, new)` called for every pair. nullptr if memory ran out.
Node* adopt_into(Document& target, Node* node, void (*moved)(Node* from, Node* to, void* user), void* user);

// Is `node` `ancestor` or inside it?
bool contains(const Node* ancestor, const Node* node);
// Is the node in its document's tree (below the Document node; a shadow tree counts through its host)?
bool is_connected(const Node* node);
// The node an event goes to after this one: the parent, or the host for the top of a shadow tree.
inline Node* event_parent(const Node* node) { return node->parent ? node->parent : node->shadow_host; }
// The children of a shadow host that go in `slot` (a <slot> in a shadow tree): the ones whose slot attribute is the slot's name (text goes in the slot that has no
// name). False if the slot is not in a shadow tree.
bool slot_assigned_nodes(const Node* slot, List<Node*>* out);
Node* first_element_child(const Node* node);
Node* last_element_child(const Node* node);
Node* next_element_sibling(const Node* node);
Node* previous_element_sibling(const Node* node);
uint32_t child_element_count(const Node* node);
// The next node in tree order (depth first, children before siblings) not leaving `root`; nullptr at the end.
Node* next_in_tree(const Node* node, const Node* root);
// 1 if `a` comes after `b` in the tree, -1 if before, 0 if the same or in different trees.
int compare_order(const Node* a, const Node* b);

// --- Attributes (names compared in lowercase).
bool get_attribute(const Node* element, StringView name, StringView* value);
bool has_attribute(const Node* element, StringView name);
// Sets or adds. The attribute name is lowercased.
DomError set_attribute(Node* element, StringView name, StringView value);
bool remove_attribute(Node* element, StringView name);
// The whitespace separated words of the class attribute contain `word`?
bool has_class(const Node* element, StringView word);

// --- Text and markup
// All the text below a node (a text node's data), without the comments.
void text_content(const Node* node, String* out);
// Replaces the children of an element or fragment with one text node (none for ""); a text or comment node's data is replaced.
DomError set_text_content(Node* node, StringView text);
// The markup of the children (innerHTML) or of the node itself (outerHTML).
void inner_html(const Node* node, String* out);
void outer_html(const Node* node, String* out);
// Is this a tag that has no end tag and no children?
bool is_void_tag(StringView tag);
// Replaces the children of `element` with what `html` makes (without running scripts in it); an element's tag decides how its text is read (as a
// <textarea>'s or a <style>'s content is not markup). Returns DomError::None on success.
DomError set_inner_html(Node* element, StringView html);
// The content of a <template> (made empty if it has none yet): the nodes inside a template are not in the tree of the page, they are in this fragment.
Node* template_content(Node* tmpl);
// Moves what is inside every <template> of the document into its content (done once a page has been read).
void adopt_template_contents(Document& doc);
// Makes the nodes `html` describes as the children of a new fragment (scripts in it are not run). nullptr if memory ran out.
Node* parse_fragment(Document& doc, StringView html, StringView context_tag);

}  // namespace dom
