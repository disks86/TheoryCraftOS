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

// CSS selectors over the document tree: parsing a selector list into a compact form, and matching an element against it. Used by the style rules (the
// cascade) and by querySelector, matches and closest.
//
// Understood: type, universal, #id, .class, attribute tests ([a], [a=b], ~=, |=, ^=, $=, *=, with an `i` flag), the descendant, child (>), adjacent (+)
// and general sibling (~) combinators, selector lists, and the pseudo-classes :first-child, :last-child, :only-child, :first-of-type, :last-of-type,
// :only-of-type, :nth-child(an+b), :nth-last-child, :nth-of-type, :nth-last-of-type, :empty, :root, :scope, :link, :any-link, :checked, :disabled,
// :enabled, :not(), :is(), :where() and :has() (which looks at descendants / children / following siblings, approximately), :hover and :active (the pointer
// states a page keeps in Node::ui_state), :focus and :focus-within. :visited, :target and the other states that need a history never match. The pseudo-elements
// ::before, ::after and ::marker (and the old :before, :after) are remembered with the selector (pseudo()), and such a selector matches the element they belong
// to only when asked to ignore that; the other pseudo-elements make a selector never match.
#pragma once
#include "Dom/Dom.hpp"

namespace dom {

class SelectorSet {
 public:
  explicit SelectorSet(allocators::IAllocator& allocator) : alloc_(allocator), pool_(allocator), tests_(allocator), compounds_(allocator), selectors_(allocator), tops_(allocator), refs_(allocator) {}
  SelectorSet(const SelectorSet&) = delete;
  SelectorSet& operator=(const SelectorSet&) = delete;

  // Adds the selectors of a list ("a, b > c"), each remembering `tag` (the style rule it belongs to). *added = how many that could apply were added; a
  // selector that is wrong or can never match here is left out. Returns false if the list was not valid selector syntax at all (nothing added;
  // querySelector throws for this) or memory ran out.
  // `forgiving` (what style rules do): a selector in the list that is not valid is left out and the others are kept; otherwise (querySelector) one
  // bad selector makes the whole list invalid.
  bool parse(StringView text, uint32_t tag, uint32_t* added, bool forgiving = true);
  void clear();

  // The selectors that were added directly (not the ones inside :not() and the like).
  uint32_t count() const { return static_cast<uint32_t>(tops_.count()); }
  // The i-th of them in the order added: its tag and its specificity (ids x 10000 + classes etc. x 100 + types).
  uint32_t tag(uint32_t i) const;
  uint32_t specificity(uint32_t i) const;
  // 0 for an ordinary selector, 1 for ::before, 2 for ::after, 3 for ::marker (the element it matches is the one the pseudo-element belongs to).
  uint8_t pseudo(uint32_t i) const;
  bool uses_host(uint32_t i) const;
  // Does the i-th selector match? A selector with a pseudo-element matches only with `with_pseudo` (the style rules ask this way).
  bool matches(uint32_t i, const Node* element, const Node* scope = nullptr, bool with_pseudo = false) const;
  // Does any selector match?
  bool matches_any(const Node* element, const Node* scope = nullptr) const;

 private:
  enum class Combinator : uint8_t { None, Descendant, Child, Adjacent, Sibling };
  enum class TestKind : uint8_t {
    Tag, Id, Class, Attr, FirstChild, LastChild, OnlyChild, FirstOfType, LastOfType, OnlyOfType, Nth, Empty, Root, Scope, Link, Checked, Disabled, Enabled, Focus, Required, Optional, ReadOnly, ReadWrite, Valid, Invalid, PlaceholderShown,
    Not, Is, Has, Never, Always, Hover, Active, FocusWithin, Host, HostIs, Fullscreen, Lang,
  };
  enum class AttrOp : uint8_t { Exists, Equals, Includes, Dash, Prefix, Suffix, Contains };
  struct Test {
    TestKind kind;
    AttrOp op;
    bool fold_case, from_end, of_type;
    uint32_t name_offset, name_length, value_offset, value_length;
    int32_t a, b;                        // Nth: matches position p when p = a*n + b for some n >= 0
    uint32_t first_ref, ref_count;       // Not / Is / Has: the selectors inside (a stretch of refs_)
  };
  struct Compound {
    Combinator combinator;   // how it relates to the compound before it (for the first compound of a :has() selector: to the anchor)
    uint32_t first_test, test_count;
  };
  struct Selector {
    uint32_t first_compound, compound_count, specificity, tag;
    bool inner;
    uint8_t pseudo;
    bool host = false;   // written with :host (it is meant for the host of a shadow tree)
  };
  class Reader;
  friend class Reader;

  bool match_chain(const Selector& s, uint32_t k, const Node* node, const Node* scope) const;
  bool match_compound(const Compound& c, const Node* node, const Node* scope) const;
  bool match_test(const Test& t, const Node* node, const Node* scope) const;
  bool match_inner(const Test& t, const Node* node, const Node* scope) const;
  StringView text(uint32_t offset, uint32_t length) const { return pool_.view().substr(offset, length); }

  allocators::IAllocator& alloc_;
  String pool_;
  List<Test> tests_;
  List<Compound> compounds_;
  List<Selector> selectors_;
  List<uint32_t> tops_;   // the selectors added directly, in order (indexes into selectors_)
  List<uint32_t> refs_;   // the selectors inside :not() and the like (indexes into selectors_), a list after another
};

}  // namespace dom
