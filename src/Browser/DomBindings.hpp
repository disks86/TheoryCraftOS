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

// The DOM as scripts see it: window, document, nodes and elements (with the properties and methods pages use - tree walking and changing, attributes, class
// lists, dataset, inline style, innerHTML / outerHTML / textContent, querySelector and the other lookups, getComputedStyle, matchMedia), events (listeners, the capture
// and bubble phases, preventDefault, stopPropagation, the Event constructors, on* handlers, the standard page events), location and history, storage
// (localStorage, sessionStorage), timers, DOMParser and the small parts of the platform pages expect (navigator, screen, performance, btoa, URI coding).
//
// Nodes are wrapped in host objects (see script::HostClass): a wrapper stands for one node for as long as the page lives, so `a === b` works. The
// collections that the DOM calls live (childNodes, getElementsByTagName, ...) are arrays made when asked for. What is not here: layout (sizes and positions
// are zero), the network (XMLHttpRequest, fetch), observers that report (MutationObserver and the like exist but never fire), forms as controls, canvas.
#pragma once
#include "Browser/Css.hpp"
#include "Browser/Media.hpp"
#include "Dom/Dom.hpp"
#include "Script/Vm.hpp"

namespace browser {

// A set of named strings: localStorage and sessionStorage.
class StorageArea {
 public:
  explicit StorageArea(allocators::IAllocator& allocator) : allocator_(allocator), keys_(allocator), values_(allocator) {}
  uint32_t count() const { return static_cast<uint32_t>(keys_.count()); }
  StringView key(uint32_t i) const { return i < keys_.count() ? keys_[i].view() : StringView(); }
  bool get(StringView key, StringView* value) const;
  // False if memory ran out or the storage limit (about 5 MB) was passed.
  bool set(StringView key, StringView value);
  bool remove(StringView key);
  void clear() {
    keys_.clear();
    values_.clear();
  }

 private:
  allocators::IAllocator& allocator_;
  List<String> keys_, values_;
};

// What the bindings need from the page they live in.
class IDomHost {
 public:
  virtual const MediaEnvironment& media() = 0;
  virtual StringView address() = 0;                            // where the page is from (with any #hash set by script)
  virtual void navigate(StringView address, bool replace) = 0; // location.assign / href = / replace
  virtual void set_hash(StringView hash) = 0;                  // location.hash = : no new page
  virtual void reload() = 0;
  virtual void history_go(int32_t delta) = 0;
  virtual uint32_t history_length() = 0;
  virtual void push_state(StringView address, bool replace) = 0;
  virtual void alert(StringView text) = 0;
  virtual bool confirm(StringView text) = 0;
  virtual void scroll_to(int32_t line) = 0;
  // The style of an element as it is drawn now (false if the page cannot say).
  virtual bool computed_style(const dom::Node* element, Style* out) = 0;
  // A new document made from html text (DOMParser); owned by the host. nullptr if memory ran out.
  virtual dom::Document* parse_document(StringView html) = 0;
  virtual StorageArea& local_storage() = 0;
  virtual StorageArea& session_storage() = 0;
  virtual String& cookie() = 0;
  // Compiles the code of an on* attribute (or a javascript: address) into a function taking `event`. False (with a message sent to console) if it does not
  // compile; the function is not kept alive by this: the caller stores it where the script engine can see it.
  virtual bool compile_handler(StringView code, script::Value* function) = 0;

 protected:
  IDomHost() = default;
  ~IDomHost() = default;
};

struct EventInit {
  bool bubbles = true, cancelable = true;
  double detail = 0, client_x = 0, client_y = 0;
  int button = 0;
};

class DomBindings {
 public:
  DomBindings(script::Vm& vm, dom::Document& document, IDomHost& host);
  ~DomBindings();
  DomBindings(const DomBindings&) = delete;
  DomBindings& operator=(const DomBindings&) = delete;

  // Defines everything on the global object. False if memory ran out.
  bool install();

  // Fires an event of that type at a node (nullptr: the window); true unless a listener called preventDefault(). Listeners that throw are counted and
  // the first message kept (take_error()).
  bool dispatch(dom::Node* target, StringView type, const EventInit& init = EventInit());
  // What a click on a node does when no listener stopped it: follows a link, toggles a check box. (Called by element.click() and by the browser after
  // dispatch(node, "click") returned true.)
  void activate(dom::Node* node);
  // The element whose script is running now (document.currentScript).
  void set_current_script(dom::Node* script);
  // document.readyState, and the window's numbers (innerWidth ...) from the media environment.
  void set_ready_state(StringView state);
  void update_window_metrics();
  // Looks at the media queries that matchMedia gave out: those whose answer changed get their change listeners called. Returns how many changed.
  uint32_t media_changed();
  // Text that listeners (and callbacks of timers) failed with.
  uint32_t errors() const;
  void take_error(String* message);
  // The wrapper of a node (made on first use).
  script::JsObject* wrap(dom::Node* node);
  // The node a wrapper stands for, or nullptr.
  static dom::Node* unwrap(const script::Value& value);

 // (the state of one page's bindings: see DomBindingsInternal.hpp)
  struct Impl;

 private:
  script::Vm& vm_;
  Impl* impl_;
};

}  // namespace browser
