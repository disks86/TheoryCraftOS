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

// A web page that has been loaded: its document tree, its style sheets, its scripts (an Engine with the DOM installed), and everything that happens to it
// after the first drawing - clicks, timers, a window of another size, a changed colour scheme. The browser window creates a Page for each HTML page it
// shows, asks it to draw itself into an HtmlDocument whenever changed() says the tree or the display changed, and tells it what the user does. Pages
// reach the outside through an IPageEnvironment. All of it is used from one thread (the browser's handler thread).
//
//   Page page(allocator, environment, "http://example.com/", media_environment, /*scripts*/ true);
//   page.load(html);                    // reads the tree, runs the scripts, fires DOMContentLoaded and load
//   page.render(&document);             // fetches the style sheets it links to, and fills `document` with what the page looks like
//   page.click(serial, x, y);           // a click on the node a run came from (see Run::node)
//   if (page.changed()) page.render(&document);
#pragma once
#include "Browser/DomBindings.hpp"
#include "Browser/HtmlDocument.hpp"
#include "Browser/Media.hpp"
#include "Browser/Objects.hpp"
#include "Browser/Renderer.hpp"
#include "Dom/Dom.hpp"
#include "Script/Engine.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

// What a page asks of the browser it is shown in.
class IPageEnvironment : public IObjectEnvironment {
 public:
  // The text at an address (a script or a style sheet), at most `limit` bytes. False if it cannot be had.
  virtual bool fetch_text(StringView address, uint64_t limit, String* text) = 0;
  virtual void navigate(StringView address, bool replace) = 0;
  virtual void set_hash(StringView hash) = 0;
  virtual void reload() = 0;
  virtual void history_go(int32_t delta) = 0;
  virtual uint32_t history_length() = 0;
  virtual void push_state(StringView address, bool replace) = 0;
  virtual void alert(StringView text) = 0;
  virtual bool confirm(StringView text) = 0;
  virtual void scroll_to(int32_t line) = 0;
  virtual uint64_t now_ms() = 0;                 // milliseconds since 1970 (0 without a clock)
  virtual void console(int level, StringView text) = 0;
  virtual StorageArea& local_storage() = 0;
  virtual String& cookie() = 0;
  // (IObjectEnvironment: fetch_bytes() for pictures and videos, open_video(); pages without pictures need neither)
  bool fetch_bytes(StringView, uint64_t, ByteBuffer*) override { return false; }

 protected:
  IPageEnvironment() = default;
  ~IPageEnvironment() = default;
};

class Page final : public script::IHost, public IDomHost, public dom::IScriptRunner, public IStyleSource {
 public:
  static constexpr uint32_t kMaxExternalScripts = 12, kMaxStyleSheets = 8;
  static constexpr uint64_t kMaxScriptBytes = 1u << 20, kMaxStyleSheetBytes = 512u << 10;

  Page(allocators::IAllocator& allocator, IPageEnvironment& environment, StringView address, const MediaEnvironment& media, bool scripts);
  ~Page();
  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  // Reads the page: the tree, the scripts (when enabled), then readyState "interactive", DOMContentLoaded, "complete" and load. False if memory ran out.
  bool load(StringView html);
  // Fetches the style sheets the page links to that have not been fetched, and draws the page into `out`.
  bool render(HtmlDocument* out);
  // Has the tree changed, or the display (set_media()), since the last render()?
  bool changed() const { return doc_.version() != rendered_version_ || media_ != rendered_media_; }
  // Did something visible change that is not in the tree (a canvas drawn on, a video frame)? Then the view only needs painting again.
  bool take_visual_change() { return objects_.take_dirty(); }

  // The display changed (the window was resized, the colour scheme was switched): the page's numbers and its matchMedia() lists follow, and `resize`
  // fires. A style sheet's @media rules are read again by the next render().
  void set_media(const MediaEnvironment& media);
  // A click on a node (Run::node): the click event goes to it and up; if nothing stops it, a link is followed. Without scripts only the link is. When the
  // node is a picture, video or canvas, `object` is its ObjectKind and (lx, ly) where in its w x h rectangle the click was: a video with controls plays or
  // pauses.
  void click(uint32_t node_serial, int32_t x, int32_t y, uint8_t object = 0, int32_t lx = 0, int32_t ly = 0, uint32_t w = 0, uint32_t h = 0);
  // Moves pictures and videos along (animated pictures, playing videos) and fires the events that follow (load, error, timeupdate, ended ...); true if
  // anything visible changed or an event was fired. Called often while objects().animating().
  bool step_objects(double now_ms);
  PageObjects& objects() override { return objects_; }
  // Runs the timers that are due at `now_ms` (the host clock); true if any ran.
  bool run_due_timers(double now_ms);
  // When the next timer is due (host clock); false if none is waiting.
  bool next_timer(double* due_ms);
  // Where the page is scrolled to (a script called scrollTo): the line it asked for, or -1 if it did not since the last call.
  int32_t take_scroll_request();

  dom::Document& document() { return doc_; }
  StringView address() override { return address_.view(); }
  void set_address(StringView address) { address_.assign(address); }
  // The title: what <title> says now.
  StringView page_title();
  bool scripting() const { return scripts_enabled_; }
  // What scripts printed (one line each, errors included), how many errors they had and the first of them.
  StringView log() const { return log_.view(); }
  uint32_t errors() const { return errors_; }
  StringView first_error() const { return first_error_.view(); }

  // --- script::IHost
  void console(int level, StringView text) override;
  void alert(StringView text) override { env_.alert(text); }
  void document_write(StringView text, bool newline) override;
  uint64_t now_ms() override { return env_.now_ms(); }
  StringView location() override { return address_.view(); }
  StringView title() override { return page_title(); }
  void set_title(StringView title) override;
  StringView user_agent() override { return "Mozilla/5.0 (compatible; TheoryCraftOS-Browser/1.0)"; }

  // --- IDomHost
  const MediaEnvironment& media() override { return media_; }
  void navigate(StringView address, bool replace) override { env_.navigate(address, replace); }
  void set_hash(StringView hash) override;
  void reload() override { env_.reload(); }
  void history_go(int32_t delta) override { env_.history_go(delta); }
  uint32_t history_length() override { return env_.history_length(); }
  void push_state(StringView address, bool replace) override;
  bool confirm(StringView text) override { return env_.confirm(text); }
  void scroll_to(int32_t line) override;
  bool computed_style(const dom::Node* element, Style* out) override;
  dom::Document* parse_document(StringView html) override;
  StorageArea& local_storage() override { return env_.local_storage(); }
  StorageArea& session_storage() override { return session_; }
  String& cookie() override { return env_.cookie(); }
  bool compile_handler(StringView code, script::Value* function) override;

  // --- dom::IScriptRunner
  StringView external(uint32_t index, StringView src) override;
  void run(dom::Node* script, StringView source, String* written) override;

  // --- IStyleSource
  StringView linked(const dom::Node* link_element) override;

 private:
  struct Fetched {
    explicit Fetched(allocators::IAllocator& a) : address(a), text(a) {}
    uint32_t serial = 0;   // the link element (style sheets) or the position (scripts)
    String address, text;
  };

  bool start_engine();
  void deliver_object_events();
  void note_error(StringView text);
  void fetch_pending_styles();
  bool resolve(StringView reference, String* out);
  bool fetched_already(const dom::Node* link) const;
  const StyleSheet& style_sheet();

  allocators::IAllocator& allocator_;
  IPageEnvironment& env_;
  bool scripts_enabled_;
  MediaEnvironment media_, rendered_media_;
  String address_, log_, first_error_, title_text_, written_;
  StorageArea session_;
  dom::Document doc_;
  List<UniquePointer<dom::Document>> extra_documents_;
  List<Fetched> scripts_, sheets_;
  PageObjects objects_;
  uint32_t errors_ = 0;
  uint64_t rendered_version_ = 0;
  bool loading_ = true, engine_failed_ = false;
  int32_t scroll_request_ = -1;
  // the style sheets as of now, for getComputedStyle
  StyleSheet style_cache_;
  uint64_t style_cache_version_ = 0;
  MediaEnvironment style_cache_media_;
  bool style_cache_valid_ = false;
  // what the scripts run in (destroyed first)
  UniquePointer<script::Engine> engine_;
  UniquePointer<DomBindings> bindings_;
};

}  // namespace browser
