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

#include "Browser/Page.hpp"
#include "Layout/BoxBuilder.hpp"
#include "Layout/FontMeasurer.hpp"
#include "Layout/Layout.hpp"
#include "Paint/PaintBuilder.hpp"
#include "Browser/ResourceScan.hpp"
#include "Browser/Url.hpp"
#include "Browser/WebPrelude.hpp"
#include "Script/TypedArray.hpp"
#include "Utility/ByteText.hpp"
#include "Dom/Form.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Browser/Objects.hpp"
#include "Utility/Move.hpp"

namespace browser {

// What a page in a frame asks of the browser: what it can have it asks the page around it for; going somewhere moves the frame.
class Page::FrameEnv final : public IPageEnvironment {
 public:
  FrameEnv(Page& owner, uint32_t serial) : owner_(owner), serial_(serial) {}
  bool fetch_text(StringView address, uint64_t limit, String* text) override { return owner_.env_.fetch_text(address, limit, text); }
  bool fetch_bytes(StringView address, uint64_t limit, ByteBuffer* out) override { return owner_.env_.fetch_bytes(address, limit, out); }
  UniquePointer<IVideoClip> open_video(UniquePointer<ByteBuffer> data) override { return owner_.env_.open_video(util::move(data)); }
  UniquePointer<IVideoClip> open_audio(UniquePointer<ByteBuffer> data) override { return owner_.env_.open_audio(util::move(data)); }
  void navigate(StringView address, bool) override { owner_.frame_navigate(serial_, address); }
  void navigate_post(StringView address, StringView type, StringView body) override { owner_.env_.navigate_post(address, type, body); }
  bool clipboard_get(String* text) override { return owner_.env_.clipboard_get(text); }
  void clipboard_set(StringView text) override { owner_.env_.clipboard_set(text); }
  void pick_file(uint32_t node_serial, bool multiple) override { owner_.frame_pick_file(serial_, node_serial, multiple); }
  void set_hash(StringView) override {}
  void reload() override {}
  void history_go(int32_t) override {}
  uint32_t history_length() override { return 1; }
  void push_state(StringView, StringView, bool, bool) override {}
  void alert(StringView text) override { owner_.env_.alert(text); }
  bool confirm(StringView text) override { return owner_.env_.confirm(text); }
  void scroll_to(int32_t) override {}
  // A frame can go fullscreen when its <iframe> allows it: the <iframe> is then the fullscreen element of the page around it.
  bool fullscreen_supported() override { return owner_.env_.fullscreen_supported() && owner_.frame_allows_fullscreen(serial_); }
  bool set_fullscreen(bool on) override {
    if (on) {
      const bool ok = owner_.fullscreen_request(owner_.doc_.by_serial(serial_));
      if (ok) owner_.fire_fullscreen_change(owner_.doc_.by_serial(serial_));
      return ok;
    }
    dom::Node* frame = owner_.doc_.by_serial(serial_);
    if (frame && owner_.fullscreen_element() == frame) {
      owner_.fullscreen_exit();
      owner_.fire_fullscreen_change(frame);
    }
    return true;
  }
  uint64_t now_ms() override { return owner_.env_.now_ms(); }
  void console(int level, StringView text) override { owner_.env_.console(level, text); }
  StorageHub& local_storage_hub() override { return owner_.env_.local_storage_hub(); }
  StorageHub& session_storage_hub() override { return owner_.env_.session_storage_hub(); }
  PageHub& page_hub() override { return owner_.env_.page_hub(); }
  uint64_t tab_id() override { return owner_.env_.tab_id(); }
  CookieJar& cookies() override { return owner_.env_.cookies(); }
  bool http_request(const FetchOptions& options, NetResponse* response) override { return owner_.env_.http_request(options, response); }

 private:
  Page& owner_;
  uint32_t serial_;
};

struct Page::Frame {
  Frame(Page& owner, uint32_t node_serial, allocators::IAllocator& a) : serial(node_serial), key(a), doc(a), env(owner, node_serial) {}
  uint32_t serial;
  String key;
  HtmlDocument doc;
  FrameEnv env;
  UniquePointer<Page> page;
};

Page::Page(allocators::IAllocator& allocator, IPageEnvironment& environment, StringView address, const MediaEnvironment& media, bool scripts)
    : allocator_(allocator),
      env_(environment),
      scripts_enabled_(scripts),
      media_(media),
      rendered_media_(media),
      address_(allocator),
      log_(allocator),
      first_error_(allocator),
      title_text_(allocator),
      written_(allocator),
      doc_(allocator),
      extra_documents_(allocator),
      scripts_(allocator),
      sheets_(allocator),
      objects_(allocator),
      frames_(allocator),
      messages_(allocator),
      port_homes_(allocator),
      frame_windows_(allocator),
      storage_origin_(allocator),
      state_json_(allocator),
      referrer_(allocator),
      fullscreen_stack_(allocator),
      focus_value_(allocator),
      invalid_text_(allocator),
      style_cache_(allocator),
      sockets_(allocator),
      workers_(allocator),
      geometry_(allocator) {
  address_.assign(address);
  style_cache_media_ = media;
  hub_id_ = env_.page_hub().add_page();
  hub_seen_ = env_.page_hub().serial();
}

Page::~Page() {
  bindings_.reset();
  engine_.reset();
}

bool Page::start_engine() {
  if (engine_) return true;
  if (!scripts_enabled_ || engine_failed_) return false;
  engine_ = UniquePointer<script::Engine>::make(allocator_, allocator_, static_cast<script::IHost*>(this));
  if (engine_ && engine_->ok()) {
    engine_->set_limits(8000000, 48u << 20);
    bindings_ = UniquePointer<DomBindings>::make(allocator_, engine_->vm(), doc_, static_cast<IDomHost&>(*this));
    if (bindings_ && bindings_->install()) {
      String prelude_error(allocator_);
      if (!engine_->run_library(web_prelude(), &prelude_error)) {   // (fetch, XMLHttpRequest, URLSearchParams ... : the page still works without them)
        prelude_error.insert(0, "The web platform script failed: ");
        env_.console(3, prelude_error.view());
      }
      prelude_error.clear();
      if (!engine_->run_library(dom_prelude(), &prelude_error)) {
        prelude_error.insert(0, "The DOM script failed: ");
        env_.console(3, prelude_error.view());
      }
      bindings_->set_ready_state(loading_ ? "loading" : "complete");
      bindings_->set_hidden(hidden_);
      return true;
    }
  }
  bindings_.reset();
  engine_.reset();
  engine_failed_ = true;
  return false;
}

void Page::note_error(StringView text) {
  if (errors_++ == 0) first_error_.assign(text);
  if (log_.size() < 16 * 1024) {
    log_.append("script error: ");
    log_.append(text);
    log_.push_back('\n');
  }
}

bool Page::resolve(StringView reference, String* out) { return resolve_reference(allocator_, address_.view(), reference, out); }

// --- Loading -----------------------------------------------------------------------------------------------------------------

// Asks the environment to start downloading what the page's HTML names, so the scripts and style sheets arrive while the page is read.
void Page::prefetch_resources(StringView html) {
  scan_resources(html, [this](ResourceKind kind, StringView reference) {
    if (kind == ResourceKind::Script && !scripts_enabled_) return;
    String address(allocator_);
    if (resolve(reference, &address) && (address.view().starts_with("http://") || address.view().starts_with("https://"))) env_.prefetch(address.view());
  });
}

bool Page::load(StringView html) {
  prefetch_resources(html);
  loading_ = true;
  const bool ok = dom::build_document(doc_, html, scripts_enabled_ ? static_cast<dom::IScriptRunner*>(this) : nullptr);
  loading_ = false;
  if (!ok) return false;
  if (engine_ && bindings_) {
    bindings_->set_current_script(nullptr);
    bindings_->set_ready_state("interactive");
    EventInit state_change;
    state_change.cancelable = false;
    state_change.bubbles = false;
    bindings_->dispatch(doc_.root(), "readystatechange", state_change);
    EventInit bubbling;
    bubbling.cancelable = false;
    bindings_->dispatch(doc_.root(), "DOMContentLoaded", bubbling);
    bindings_->set_ready_state("complete");
    EventInit plain;
    plain.cancelable = false;
    plain.bubbles = false;
    bindings_->dispatch(doc_.root(), "readystatechange", plain);
    bindings_->dispatch(nullptr, "load", plain);   // (load is the window's)
    bindings_->call_hook("__fire_pageshow", nullptr, 0);
    if (bindings_->errors()) {
      String text(allocator_);
      bindings_->take_error(&text);
      note_error(text.view());
    }
  }
  if (address_.view().find('#') != StringView::npos) fragment_pending_ = true;
  for (dom::Node* n = doc_.root(); n; n = dom::next_in_tree(n, doc_.root())) {   // autofocus
    if (n->is_element() && dom::has_attribute(n, "autofocus") && dom::is_focusable(n)) {
      set_focus(n);
      break;
    }
  }
  return true;
}

StringView Page::external(uint32_t index, StringView src) {
  if (index >= kMaxExternalScripts) return StringView();
  for (uint64_t i = 0; i < scripts_.count(); ++i)
    if (scripts_[i].serial == index + 1) return scripts_[i].text.view();
  Fetched f(allocator_);
  f.serial = index + 1;
  if (resolve(src, &f.address)) env_.fetch_text(f.address.view(), kMaxScriptBytes, &f.text);
  if (!scripts_.push_back(util::move(f))) return StringView();
  return scripts_[scripts_.count() - 1].text.view();
}

void Page::run(dom::Node* script, StringView source, String* written) {
  if (!start_engine()) return;
  bindings_->set_current_script(script);
  written_.clear();
  String error(allocator_);
  if (!engine_->run(source, &error, nullptr)) note_error(error.view());
  if (bindings_->errors()) {
    String text(allocator_);
    bindings_->take_error(&text);
    note_error(text.view());
  }
  written->append(written_.view());
  written_.clear();
  bindings_->set_current_script(nullptr);
}

void Page::document_write(StringView text, bool newline) {
  written_.append(text);
  if (newline) written_.push_back('\n');
}

void Page::console(int level, StringView text) {
  if (log_.size() < 16 * 1024) {
    if (level == 2) log_.append("warning: ");
    if (level == 3) log_.append("error: ");
    log_.append(text);
    log_.push_back('\n');
  }
  env_.console(level, text);
}

StringView Page::page_title() {
  document_title(doc_, &title_text_);
  return title_text_.view();
}

void Page::set_title(StringView title) {
  dom::Node* t = nullptr;
  for (dom::Node* n = doc_.root(); n && !t; n = dom::next_in_tree(n, doc_.root()))
    if (n->is_tag("title")) t = n;
  if (!t && doc_.head()) {
    t = doc_.create_element("title");
    if (t) dom::append_child(doc_.head(), t);
  }
  if (t) dom::set_text_content(t, title);
}

// --- Styles -------------------------------------------------------------------------------------------------------------------

StringView Page::linked(const dom::Node* link_element) {
  for (uint64_t i = 0; i < sheets_.count(); ++i)
    if (sheets_[i].serial == link_element->serial) return sheets_[i].text.view();
  return StringView();
}

bool Page::fetched_already(const dom::Node* link) const {
  for (uint64_t i = 0; i < sheets_.count(); ++i)
    if (sheets_[i].serial == link->serial) return true;
  return false;
}

// Fetches the sheets that <link rel=stylesheet> elements name and that were not fetched yet (the ones scripts added too).
// The href of a <link rel=stylesheet> element that was not fetched yet (false for any other node).
bool Page::pending_style_link(const dom::Node* n, StringView* href) const {
  StringView rel;
  if (!n->is_tag("link") || !dom::get_attribute(n, "rel", &rel) || !dom::get_attribute(n, "href", href) || href->empty()) return false;
  bool stylesheet = false;
  uint64_t i = 0;
  while (i < rel.size()) {
    while (i < rel.size() && rel[i] == ' ') ++i;
    const uint64_t start = i;
    while (i < rel.size() && rel[i] != ' ') ++i;
    if (rel.substr(start, i - start).equals_ignore_case("stylesheet")) stylesheet = true;
  }
  return stylesheet && !fetched_already(n);
}

void Page::fetch_pending_styles() {
  // First ask for every sheet (they download side by side), then collect them in order.
  uint64_t requested = sheets_.count();
  for (const dom::Node* n = doc_.root(); n && requested < kMaxStyleSheets; n = dom::next_in_tree(n, doc_.root())) {
    StringView href;
    String address(allocator_);
    if (!pending_style_link(n, &href)) continue;
    ++requested;
    if (resolve(href, &address)) env_.prefetch(address.view());
  }
  for (const dom::Node* n = doc_.root(); n; n = dom::next_in_tree(n, doc_.root())) {
    StringView href;
    if (sheets_.count() >= kMaxStyleSheets || !pending_style_link(n, &href)) continue;
    Fetched f(allocator_);
    f.serial = n->serial;
    if (resolve(href, &f.address)) env_.fetch_text(f.address.view(), kMaxStyleSheetBytes, &f.text);
    sheets_.push_back(util::move(f));   // (even an empty answer: it is not asked for again)
  }
}

const StyleSheet& Page::style_sheet() {
  if (!style_cache_valid_ || style_cache_version_ != doc_.version() || style_cache_media_ != media_) {
    style_cache_.clear();
    RenderOptions options;
    options.media = media_;
    options.styles = this;
    // the element shown fullscreen covers the viewport (read first: the page's own rules, apart from !important ones, win)
    static const char kFullscreenCss[] =
        ":fullscreen { background-color: #000; color: #fff; }"
        ":fullscreen { position: fixed !important; top: 0 !important; left: 0 !important; right: 0 !important; bottom: 0 !important; width: 100% !important;"
        " height: 100% !important; max-width: none !important; max-height: none !important; margin: 0 !important; box-sizing: border-box !important; z-index: 2147483647 !important; }";
    if (!fullscreen_stack_.empty()) options.extra_css = StringView(kFullscreenCss);
    collect_style_sheets(doc_, options, &style_cache_);
    style_cache_version_ = doc_.version();
    style_cache_media_ = media_;
    style_cache_valid_ = true;
  }
  return style_cache_;
}

bool Page::computed_style(const dom::Node* element, Style* out) {
  *out = browser::computed_style(style_sheet(), element);
  return true;
}

bool Page::render(HtmlDocument* out) {
  prune_fullscreen();
  fetch_pending_styles();
  sync_frames();
  objects_.set_now(env_.now_ms());
  objects_.sync(doc_, env_, address_.view(), media_);
  RenderOptions options;
  options.media = media_;
  options.styles = this;
  options.objects = &objects_;
  options.frames = this;
  options.focus = focus_;
  options.open_select = open_select_;
  options.invalid_node = invalid_node_;
  options.invalid_text = invalid_text_.view();
  const bool ok = render_document(doc_, options, out);
  rendered_version_ = doc_.version();
  rendered_media_ = media_;
  deliver_object_events();
  return ok;
}

bool Page::render_picture(PagePicture* out, font::FontSet& fonts) {
  prune_fullscreen();
  fetch_pending_styles();
  sync_frames();
  objects_.set_now(env_.now_ms());
  objects_.sync(doc_, env_, address_.view(), media_);
  style_sheet();
  style_cache_.set_viewport(media_.width, media_.height);
  layout::BuildOptions build;
  build.objects = &objects_;
  layout::FontMeasurer measurer(fonts, &style_cache_);
  bool ok = layout::build_box_tree(doc_, style_cache_, build, &out->tree);
  if (ok) {
    layout::LayoutOptions layout_options;
    layout_options.viewport_width = layout::LayoutUnit::from_px(media_.width);
    layout_options.viewport_height = layout::LayoutUnit::from_px(media_.height);
    layout_options.measurer = &measurer;
    layout_options.sheet = &style_cache_;
    ok = layout::layout_tree(out->tree, layout_options);
  }
  if (ok) {
    paint::PaintOptions paint_options;
    paint_options.measurer = &measurer;
    paint_options.sheet = &style_cache_;
    paint_options.doc = &doc_;
    ok = paint::build_display_list(out->tree, paint_options, &out->list);
  }
  if (ok) resolve_scroll_target(out->tree);
  if (ok) record_geometry(out->tree);
  out->document = &doc_;
  out->viewport_width = media_.width;
  rendered_version_ = doc_.version();
  rendered_media_ = media_;
  deliver_object_events();
  if (ok && bindings_) {   // (what is on screen has changed: intersection observers look again)
    bindings_->call_hook("__io_run", nullptr, 0);
    flush_errors();
  }
  return ok;
}

// Fires what the pictures and videos have to say (load, error, loadedmetadata, ended ...) at their elements.
void Page::deliver_object_events() {
  PageObjects::Event e;
  uint32_t guard = 0;
  while (guard++ < 600 && objects_.take_event(&e)) {
    if (!bindings_) continue;
    if (e.source != 0) {   // a MediaSource (sourceopen, sourceclose): the built-in script holds the object
      const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(static_cast<double>(e.source)), DomBindings::HookArg::of(StringView(e.type))};
      bindings_->call_hook("__mse_event", args, 2);
      continue;
    }
    if (StringView(e.type).equals("__tt")) {   // the text tracks of a video: its <track> children changed, or the cues that are active did
      const DomBindings::HookArg arg = DomBindings::HookArg::of(static_cast<double>(e.serial));
      bindings_->call_hook("__tt_event", &arg, 1);
      continue;
    }
    if (StringView(e.type).starts_with("__dash")) {   // a DASH video: its player started or changed quality (script's dashjs-style players hear of it)
      const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(static_cast<double>(e.serial)), DomBindings::HookArg::of(StringView(e.type))};
      bindings_->call_hook("__dash_event", args, 2);
      continue;
    }
    dom::Node* n = doc_.by_serial(e.serial);
    if (!n) continue;
    EventInit init;
    init.bubbles = false;
    init.cancelable = false;
    bindings_->dispatch(n, e.type, init);
  }
  if (bindings_ && bindings_->errors()) {
    String text(allocator_);
    bindings_->take_error(&text);
    note_error(text.view());
  }
}

bool Page::step_objects(double now_ms) {
  if (pip_serial_ != 0) {   // the video was taken out of the page: it leaves the window
    dom::Node* pip = pip_element();
    if (!pip || !dom::is_connected(pip)) {
      env_.close_pip();
      pip_left();
    }
  }
  bool moved = false;
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page) moved |= frames_[i]->page->step_objects(now_ms);
  (void)moved;
  objects_.update(static_cast<uint64_t>(now_ms));
  deliver_object_events();
  return objects_.take_dirty();
}

// --- After the page is up ------------------------------------------------------------------------------------------------------

void Page::set_media(const MediaEnvironment& given) {
  MediaEnvironment media = given;
  media.fullscreen = !fullscreen_stack_.empty();
  if (media == media_) return;
  media_ = media;
  if (!engine_ || !bindings_) return;
  bindings_->update_window_metrics();
  EventInit plain;
  plain.cancelable = false;
  plain.bubbles = false;
  bindings_->dispatch(nullptr, "resize", plain);
  bindings_->media_changed();
  if (bindings_->errors()) {
    String text(allocator_);
    bindings_->take_error(&text);
    note_error(text.view());
  }
}

void Page::set_pointer(uint32_t node_serial, bool pressed) {
  if (node_serial >> 24) {   // in a frame: its page takes it
    const uint64_t frame = (node_serial >> 24) - 1;
    if (frame < frames_.count() && frames_[frame]->page) frames_[frame]->page->set_pointer(node_serial & 0xFFFFFF, pressed);
    node_serial = 0;
  }
  if (node_serial == pointer_node_ && pressed == pointer_pressed_) return;
  for (dom::Node* n = doc_.by_serial(pointer_node_); n; n = n->parent) n->ui_state = 0;
  pointer_node_ = node_serial;
  pointer_pressed_ = pressed;
  for (dom::Node* n = doc_.by_serial(node_serial); n; n = n->parent)
    n->ui_state = static_cast<uint8_t>(dom::kUiHover | (pressed ? dom::kUiActive : 0));
  if (style_sheet().uses_pointer_state()) doc_.touch();
}

void Page::click(uint32_t node_serial, int32_t x, int32_t y, uint8_t object, int32_t lx, int32_t ly, uint32_t w, uint32_t h) {
  if (node_serial >> 24) {   // in a frame: its page takes it
    const uint64_t frame = (node_serial >> 24) - 1;
    if (frame < frames_.count() && frames_[frame]->page) {
      if (focus_) set_focus(nullptr);
      for (uint64_t i = 0; i < frames_.count(); ++i)
        if (i != frame && frames_[i]->page && frames_[i]->page->has_focus()) frames_[i]->page->blur();
      frames_[frame]->page->click(node_serial & 0xFFFFFF, x, y, object, lx, ly, w, h);
    }
    return;
  }
  dom::Node* n = doc_.by_serial(node_serial);
  if (!n) return;
  if (dom::Node* modal = modal_dialog())
    if (!dom::contains(modal, n)) return;   // (the page behind a modal dialog does not take clicks)
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page && frames_[i]->page->has_focus()) frames_[i]->page->blur();
  press_focus(n);
  if (!bindings_) {   // no scripts: a click only follows a link
    if ((object == static_cast<uint8_t>(ObjectKind::Video) || object == static_cast<uint8_t>(ObjectKind::Audio)) && objects_.object_clicked(node_serial, lx, ly, w, h)) return;
    if (activate_control(n)) return;
    for (dom::Node* a = n; a && a->is_element(); a = a->parent) {
      StringView href;
      if (a->is_tag("a") && dom::get_attribute(a, "href", &href) && !href.empty()) {
        String target(allocator_);
        if (href.starts_with('#')) set_hash(href);
        else if (resolve(href, &target)) env_.navigate(target.view(), false);
        return;
      }
    }
    return;
  }
  EventInit init;
  init.client_x = x;
  init.client_y = y;
  init.detail = 1;
  if (bindings_->dispatch(n, "click", init)) {
    if ((object == static_cast<uint8_t>(ObjectKind::Video) || object == static_cast<uint8_t>(ObjectKind::Audio)) && objects_.object_clicked(node_serial, lx, ly, w, h)) {
      // (the control bar or the picture of a video took it)
    } else {
      bindings_->activate(n);
    }
  }
  deliver_object_events();
  if (bindings_->errors()) {
    String text(allocator_);
    bindings_->take_error(&text);
    note_error(text.view());
  }
}

bool Page::run_due_timers(double now_ms) {
  bool any_frame = false;
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page) any_frame |= frames_[i]->page->run_due_timers(now_ms);
  if (env_.page_hub().pending(hub_id_, storage_origin(), hub_seen_)) {
    poll_hub();
    any_frame = true;
  }
  if (!messages_.empty()) {
    deliver_messages();
    any_frame = true;
  }
  if (!engine_) return any_frame;
  double due;
  if (!engine_->next_timer(&due) || due > now_ms) return any_frame;
  String error(allocator_);
  engine_->run_due_timers(now_ms, 50, &error);
  if (!error.empty()) note_error(error.view());
  if (bindings_ && bindings_->errors()) {
    String text(allocator_);
    bindings_->take_error(&text);
    note_error(text.view());
  }
  return true;
}

bool Page::next_timer(double* due_ms) {
  bool any = engine_ && engine_->next_timer(due_ms);
  for (uint64_t i = 0; i < frames_.count(); ++i) {
    double due;
    if (frames_[i]->page && frames_[i]->page->next_timer(&due) && (!any || due < *due_ms)) {
      *due_ms = due;
      any = true;
    }
  }
  if (!messages_.empty() || env_.page_hub().pending(hub_id_, storage_origin(), hub_seen_)) {   // (a message waits to be delivered: now)
    *due_ms = 1;
    any = true;
  }
  if (objects_.animating()) {   // (a playing video, an animated picture: the next frame)
    const double frame = static_cast<double>(env_.now_ms()) + 33;
    if (!any || frame < *due_ms) *due_ms = frame;
    any = true;
  }
  return any;
}

int32_t Page::take_scroll_request() {
  const int32_t r = scroll_request_;
  scroll_request_ = -1;
  return r;
}

// --- the clipboard ----------------------------------------------------------------------------------------------------------------------

bool Page::fire_clipboard(dom::Node* target, StringView type) {
  if (!bindings_ || !target) return false;
  const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(target), DomBindings::HookArg::of(type)};
  String result(allocator_);
  bindings_->call_hook("__clip_event", args, 2, &result);
  flush_errors();
  doc_.touch();
  return result.view().equals("1");
}

void Page::page_clipboard_key(dom::Node* target, char key) {
  if (!bindings_) return;
  dom::Node* at = target ? target : doc_.body();
  if (!at) at = doc_.document_element();
  const char which = key >= 'A' && key <= 'Z' ? static_cast<char>(key + 32) : key;
  if (which == 'a') {
    bindings_->call_hook("__select_all", nullptr, 0);
  } else if (which == 'c') {
    if (!fire_clipboard(at, "copy")) bindings_->call_hook("__clip_copy_selection", nullptr, 0);
  } else if (which == 'x') {
    fire_clipboard(at, "cut");
  } else {
    fire_clipboard(at, "paste");
  }
  flush_errors();
  doc_.touch();
}

// --- leaving, and where the page scrolls to --------------------------------------------------------------------------------------------

bool Page::before_unload() {
  if (!bindings_) return false;
  String result(allocator_);
  bindings_->call_hook("__page_beforeunload", nullptr, 0, &result);
  flush_errors();
  return result.view().equals("1");
}

void Page::unload() {
  if (!bindings_) return;
  bindings_->call_hook("__page_unload", nullptr, 0);
  flush_errors();
}

void Page::set_visible(bool visible) {
  if (hidden_ == !visible) return;
  hidden_ = !visible;
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page) frames_[i]->page->set_visible(visible);
  if (!bindings_) return;
  const DomBindings::HookArg arg = DomBindings::HookArg::of(hidden_);
  bindings_->call_hook("__page_visibility", &arg, 1);
  flush_errors();
}

bool Page::scroll_restoration_manual() {
  if (!bindings_) return false;
  String result(allocator_);
  bindings_->call_hook("__scroll_manual", nullptr, 0, &result);
  return result.view().equals("1");
}

bool Page::meta_refresh(double* seconds, String* address) {
  for (dom::Node* n = doc_.root(); n; n = dom::next_in_tree(n, doc_.root())) {
    StringView equiv, content;
    if (!n->is_tag("meta") || !dom::get_attribute(n, "http-equiv", &equiv) || !equiv.equals_ignore_case("refresh") || !dom::get_attribute(n, "content", &content)) continue;
    // "5", "5; url=next.html", "0;next.html": the delay, then separator(s), then an optional "url=" and the address
    const StringView text = content.trimmed();
    uint64_t i = 0;
    double delay = 0;
    bool digits = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      if (delay < 1e9) delay = delay * 10 + (text[i] - '0');
      digits = true;
      ++i;
    }
    if (!digits) continue;
    if (i < text.size() && text[i] == '.') {   // (a fraction is dropped)
      ++i;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    }
    StringView rest = text.substr(i).trimmed();
    if (!rest.empty() && rest[0] != ';' && rest[0] != ',') continue;
    if (!rest.empty()) rest = rest.substr(1).trimmed();
    if (rest.size() >= 3 && rest.first(3).equals_ignore_case("url")) {
      rest = rest.substr(3).trimmed();
      if (!rest.empty() && rest[0] == '=') rest = rest.substr(1).trimmed();
    }
    if (!rest.empty() && (rest[0] == '"' || rest[0] == '\'')) {
      const char quote = rest[0];
      rest = rest.substr(1);
      const uint64_t end = rest.find(quote);
      if (end != StringView::npos) rest = rest.first(end);
    }
    *seconds = delay;
    if (rest.empty()) address->assign(address_.view());
    else if (!resolve(rest, address)) continue;
    return true;
  }
  return false;
}

void Page::scroll_node_into_view(dom::Node* node) {
  if (node) scroll_node_ = node->serial;
}

// Remembers where every element's box ended up, in pixels from the top left of the document (the first box of an element is its own; boxes are relative to
// their parent's border box, so the offsets of the ancestors are added on the way down).
void Page::record_geometry(const layout::BoxTree& tree) {
  geometry_.clear();
  geometry_ready_ = true;
  if (tree.empty()) return;
  struct Pending {
    layout::BoxId id;
    int64_t x100, y100;   // where the parent's border box is, in hundredths of a pixel
  };
  List<Pending> stack(allocator_);
  stack.push_back({tree.root(), 0, 0});
  while (stack.count() > 0) {
    const Pending p = stack[stack.count() - 1];
    stack.remove_at(stack.count() - 1);
    const layout::Box& b = tree.box(p.id);
    const int64_t x = p.x100 + b.x.to_px100(), y = p.y100 + b.y.to_px100();
    if (b.node != 0 && !geometry_.contains(b.node)) {
      BoxRect r;
      r.x = static_cast<double>(x) / 100.0;
      r.y = static_cast<double>(y) / 100.0;
      r.width = static_cast<double>(b.width.to_px100()) / 100.0;
      r.height = static_cast<double>(b.height.to_px100()) / 100.0;
      geometry_.put(b.node, r);
    }
    for (uint64_t i = 0; i < b.children.count(); ++i) stack.push_back({b.children[i], x, y});
  }
}

bool Page::node_rect(dom::Node* node, double* rect4) {
  if (!node || !geometry_ready_ || !dom::is_connected(node)) return false;
  const BoxRect* r = geometry_.find(node->serial);
  if (!r) return false;
  rect4[0] = r->x;
  rect4[1] = r->y;
  rect4[2] = r->width;
  rect4[3] = r->height;
  return true;
}

void Page::set_scroll(int32_t y) {
  if (y < 0) y = 0;
  if (y == scroll_y_) return;
  scroll_y_ = y;
  if (!bindings_) return;
  bindings_->set_scroll(y);
  EventInit bubbling;
  bubbling.cancelable = false;
  bindings_->dispatch(doc_.root(), "scroll", bubbling);
  bindings_->call_hook("__io_run", nullptr, 0);
  flush_errors();
}

// The vertical place the page should be scrolled to: the fragment of the address (an element with that id, or an <a name>; "" and "top" are the top), or the
// element scrollIntoView() was called on. Worked out from the layout just made.
void Page::resolve_scroll_target(const layout::BoxTree& tree) {
  dom::Node* target = nullptr;
  bool top = false;
  if (scroll_node_) {
    target = doc_.by_serial(scroll_node_);
  } else if (fragment_pending_) {
    const uint64_t at = address_.view().find('#');
    String name(allocator_);
    if (at != StringView::npos) {
      const StringView raw = address_.view().substr(at + 1);
      for (uint64_t i = 0; i < raw.size(); ++i) {   // (percent escapes are undone)
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (raw[i] == '%' && i + 2 < raw.size() + 0 && hex(raw[i + 1]) >= 0 && hex(raw[i + 2]) >= 0) {
          name.push_back(static_cast<char>(hex(raw[i + 1]) * 16 + hex(raw[i + 2])));
          i += 2;
        } else {
          name.push_back(raw[i]);
        }
      }
    }
    if (name.empty() || name.view().equals_ignore_case("top")) top = true;
    if (!name.empty()) {
      target = doc_.element_by_id(name.view());
      for (dom::Node* n = doc_.root(); !target && n; n = dom::next_in_tree(n, doc_.root())) {
        StringView value;
        if (n->is_tag("a") && dom::get_attribute(n, "name", &value) && value.equals(name.view())) target = n;
      }
      if (target) top = false;
    }
  }
  fragment_pending_ = false;
  scroll_node_ = 0;
  if (!target) {
    if (top) scroll_request_ = 0;
    return;
  }
  for (uint64_t i = 0; i < tree.count(); ++i) {
    const layout::Box& b = tree.box(static_cast<layout::BoxId>(i));
    if (b.node != target->serial) continue;
    int64_t y = 0;
    for (layout::BoxId id = static_cast<layout::BoxId>(i); id != layout::kNoBox; id = tree.box(id).parent) y += tree.box(id).y.round();
    scroll_request_ = y < 0 ? 0 : y > 100000000 ? 100000000 : static_cast<int32_t>(y);
    return;
  }
}

// --- Picture-in-Picture ------------------------------------------------------------------------------------------------------------

dom::Node* Page::pip_element() { return pip_serial_ ? doc_.by_serial(pip_serial_) : nullptr; }

int Page::pip_request(dom::Node* video, uint32_t* width, uint32_t* height) {
  if (!video || !video->is_tag("video") || !env_.pip_supported()) return 1;
  if (dom::has_attribute(video, "disablepictureinpicture")) return 3;
  PageObjects::VideoState st;
  objects_.video_state(video->serial, &st);
  if (!st.known || st.ready_state == 0 || !dom::is_connected(video)) return 2;
  uint32_t w = st.width ? st.width : 320, h = st.height ? st.height : 180;
  if (w > 320) {   // (a small window: at most 320 pixels wide)
    h = static_cast<uint32_t>(uint64_t{h} * 320 / w);
    w = 320;
  }
  if (h == 0) h = 1;
  if (pip_serial_ != 0 && pip_serial_ != video->serial) {   // one window: the video that was in it leaves
    pip_serial_ = 0;
    env_.close_pip();
  }
  if (pip_serial_ != video->serial && !env_.open_pip(video->serial, w, h, objects_)) return 1;
  pip_serial_ = video->serial;
  objects_.set_pip(video->serial);
  *width = w;
  *height = h;
  return 0;
}

void Page::pip_exit() {
  if (pip_serial_ == 0) return;
  pip_serial_ = 0;
  objects_.set_pip(0);
  env_.close_pip();
}

void Page::pip_left() {
  if (pip_serial_ == 0) return;
  pip_serial_ = 0;
  objects_.set_pip(0);
  if (bindings_) bindings_->call_hook("__pip_left", nullptr, 0);
}

void Page::pip_resized(uint32_t width, uint32_t height) {
  if (pip_serial_ == 0 || !bindings_) return;
  const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(static_cast<double>(width)), DomBindings::HookArg::of(static_cast<double>(height))};
  bindings_->call_hook("__pip_resized", args, 2);
}

// --- Fullscreen --------------------------------------------------------------------------------------------------------------------

dom::Node* Page::fullscreen_element() {
  while (!fullscreen_stack_.empty()) {
    dom::Node* n = doc_.by_serial(fullscreen_stack_[fullscreen_stack_.count() - 1]);
    if (n && dom::is_connected(n)) return n;
    break;   // (a removed element: prune_fullscreen() deals with it)
  }
  return nullptr;
}

// `fullscreenchange` at an element (the page's script hears it as for its own requests).
void Page::fire_fullscreen_change(dom::Node* target) {
  if (!bindings_ || !target) return;
  const DomBindings::HookArg args[1] = {DomBindings::HookArg::of(target)};
  bindings_->call_hook("__fs_fire", args, 1);
}

void Page::sync_fullscreen_media() {
  const bool on = !fullscreen_stack_.empty();
  if (media_.fullscreen == on) return;
  media_.fullscreen = on;
  doc_.touch();
  if (bindings_) bindings_->media_changed();   // (display-mode: no resize event: the window's size change brings that)
}

void Page::clear_fullscreen() {
  for (uint64_t i = 0; i < fullscreen_stack_.count(); ++i)
    if (dom::Node* n = doc_.by_serial(fullscreen_stack_[i])) n->form_state &= static_cast<uint8_t>(~dom::kFormFullscreen);
  fullscreen_stack_.clear();
  doc_.touch();
  sync_fullscreen_media();
}

bool Page::fullscreen_request(dom::Node* element) {
  if (!element || !element->is_element() || !dom::is_connected(element) || !env_.fullscreen_supported()) return false;
  if (dom::Node* top = fullscreen_element()) {
    if (top == element) return true;
    bool inside = false;   // (only an element inside the fullscreen one can go fullscreen as well)
    for (dom::Node* n = element->parent; n && !inside; n = n->parent) inside = n == top;
    if (!inside) return false;
  } else if (!env_.set_fullscreen(true)) {
    return false;
  }
  if (!fullscreen_stack_.push_back(element->serial)) return false;
  element->form_state |= dom::kFormFullscreen;
  doc_.touch();
  sync_fullscreen_media();
  return true;
}

// exitFullscreen(): the innermost element goes back; the window when it was the last.
void Page::fullscreen_exit() {
  if (fullscreen_stack_.empty()) return;
  if (dom::Node* n = doc_.by_serial(fullscreen_stack_[fullscreen_stack_.count() - 1])) n->form_state &= static_cast<uint8_t>(~dom::kFormFullscreen);
  fullscreen_stack_.remove_at(fullscreen_stack_.count() - 1);
  doc_.touch();
  if (fullscreen_stack_.empty()) env_.set_fullscreen(false);
  sync_fullscreen_media();
}

// The window left fullscreen by itself (Escape): every element goes back.
void Page::fullscreen_left() {
  if (fullscreen_stack_.empty()) return;
  dom::Node* top = doc_.by_serial(fullscreen_stack_[fullscreen_stack_.count() - 1]);
  clear_fullscreen();
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page) frames_[i]->page->fullscreen_left();
  if (bindings_) {
    const DomBindings::HookArg args[1] = {top && dom::is_connected(top) ? DomBindings::HookArg::of(top) : DomBindings::HookArg::null()};
    bindings_->call_hook("__fs_left", args, 1);
  }
}

void Page::prune_fullscreen() {
  if (fullscreen_stack_.empty()) return;
  dom::Node* gone = nullptr;
  bool changed = false;
  for (uint64_t i = fullscreen_stack_.count(); i-- > 0;) {
    dom::Node* n = doc_.by_serial(fullscreen_stack_[i]);
    if (n && dom::is_connected(n)) continue;
    if (n) n->form_state &= static_cast<uint8_t>(~dom::kFormFullscreen);
    gone = n;
    fullscreen_stack_.remove_at(i);
    changed = true;
  }
  if (!changed) return;
  if (fullscreen_stack_.empty()) env_.set_fullscreen(false);
  doc_.touch();
  sync_fullscreen_media();
  if (bindings_) {
    (void)gone;
    const DomBindings::HookArg args[1] = {DomBindings::HookArg::null()};
    bindings_->call_hook("__fs_left", args, 1);   // (the element is out of the document: the event goes to the document)
  }
}

// May the page in this <iframe> use the Fullscreen API? (allowfullscreen, or allow="fullscreen".)
bool Page::frame_allows_fullscreen(uint32_t frame_serial) {
  dom::Node* f = doc_.by_serial(frame_serial);
  if (!f) return false;
  StringView allow;
  return dom::has_attribute(f, "allowfullscreen") || (dom::get_attribute(f, "allow", &allow) && allow.contains("fullscreen"));
}

void Page::scroll_to(int32_t y) {
  scroll_request_ = y;
  env_.scroll_to(y);
}

void Page::set_hash(StringView hash) {
  String base(allocator_), before(allocator_);
  before.assign(address_.view());
  const uint64_t at = address_.view().find('#');
  base.assign(at == StringView::npos ? address_.view() : address_.view().first(at));
  if (hash.size() > 1) base.append(hash);
  if (hash.size() > 1) fragment_pending_ = true;
  if (base.view().equals(before.view())) {
    env_.set_hash(hash);
    return;
  }
  address_.assign(base.view());
  env_.set_hash(hash);
  if (bindings_) {   // hashchange comes as a task of its own, after the script that changed the fragment has finished
    const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(before.view()), DomBindings::HookArg::of(address_.view())};
    bindings_->call_hook("__nav_fragment", args, 2);
    bindings_->schedule_hook("__fire_hashchange", args, 2);
  }
}

void Page::push_state(StringView address, StringView state_json, bool has_state, bool replace) {
  address_.assign(address);
  state_json_.assign(state_json);
  has_state_ = has_state;
  env_.push_state(address, state_json, has_state, replace);
}

bool Page::history_state(String* json) {
  if (!has_state_) return false;
  json->assign(state_json_.view());
  return true;
}

void Page::set_history_state(StringView json, bool has_state) {
  state_json_.assign(json);
  has_state_ = has_state;
}

void Page::traverse(StringView address, StringView state_json, bool has_state) {
  String before(allocator_);
  before.assign(address_.view());
  address_.assign(address);
  state_json_.assign(state_json);
  has_state_ = has_state;
  if (!bindings_) return;
  {
    const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(before.view()), DomBindings::HookArg::of(address)};
    bindings_->call_hook("__nav_traversed", args, 2);
  }
  bindings_->call_hook("__fire_popstate", nullptr, 0);
  const StringView old_hash = before.view().find('#') == StringView::npos ? StringView() : before.view().substr(before.view().find('#'));
  const StringView new_hash = address.find('#') == StringView::npos ? StringView() : address.substr(address.find('#'));
  const StringView old_base = before.view().find('#') == StringView::npos ? before.view() : before.view().first(before.view().find('#'));
  const StringView new_base = address.find('#') == StringView::npos ? address : address.first(address.find('#'));
  if (old_base.equals(new_base) && !old_hash.equals(new_hash)) {
    const DomBindings::HookArg args[2] = {DomBindings::HookArg::of(before.view()), DomBindings::HookArg::of(address)};
    bindings_->call_hook("__fire_hashchange", args, 2);
  }
  flush_errors();
}

dom::Document* Page::parse_document(StringView html) {
  UniquePointer<dom::Document> d = UniquePointer<dom::Document>::make(allocator_, allocator_);
  if (!d || !dom::build_document(*d, html, nullptr)) return nullptr;
  dom::Document* raw = d.get();
  if (!extra_documents_.push_back(util::move(d))) return nullptr;
  return raw;
}

bool Page::compile_handler(StringView code, script::Value* function) {
  if (!engine_) return false;
  String source(allocator_), error(allocator_);
  source.append("(function(event) {\n");
  source.append(code);
  source.append("\n})");
  if (engine_->evaluate(source.view(), &error, function)) return true;
  console(3, error.view());
  note_error(error.view());
  return false;
}


// --- Forms -----------------------------------------------------------------------------------------------------------------------

// The open modal dialog (the first), or nullptr.
dom::Node* Page::modal_dialog() {
  for (uint32_t serial = 1; serial <= doc_.node_count(); ++serial) {
    dom::Node* d = doc_.by_serial(serial);
    if (d && d->is_tag("dialog") && (d->form_state & dom::kFormModal) && dom::has_attribute(d, "open") && dom::is_connected(d)) return d;
  }
  return nullptr;
}

void Page::show_dialog(dom::Node* dialog, bool modal) {
  if (!dialog->is_tag("dialog") || dom::has_attribute(dialog, "open")) return;
  dom::set_attribute(dialog, "open", "");
  if (modal) dialog->form_state |= dom::kFormModal;
  else dialog->form_state &= static_cast<uint8_t>(~dom::kFormModal);
  dom::Node* target = nullptr;
  for (dom::Node* d = dialog->first_child; d && !target; d = dom::next_in_tree(d, dialog))
    if (d->is_element() && dom::is_focusable(d) && (dom::has_attribute(d, "autofocus") || !target)) target = d;
  if (modal || target) set_focus(target);
  doc_.touch();
}

void Page::close_dialog_text(dom::Node* dialog, StringView value) {
  if (!dialog->is_tag("dialog") || !dom::has_attribute(dialog, "open")) return;
  dom::remove_attribute(dialog, "open");
  dialog->form_state &= static_cast<uint8_t>(~dom::kFormModal);
  if (!value.empty()) dialog->data.assign(value);
  if (dom::Node* f = focused())
    if (dom::contains(dialog, f)) set_focus(nullptr);
  doc_.touch();
  fire(dialog, "close", false);
  flush_errors();
}

void Page::close_dialog(dom::Node* dialog, const char* return_value) { close_dialog_text(dialog, return_value ? StringView(return_value) : StringView()); }

void Page::toggle_details(dom::Node* details) {
  const bool opening = !dom::has_attribute(details, "open");
  if (opening) {
    StringView name;
    if (dom::get_attribute(details, "name", &name) && !name.empty()) {   // (details of one group: one is open)
      for (uint32_t serial = 1; serial <= doc_.node_count(); ++serial) {
        dom::Node* d = doc_.by_serial(serial);
        StringView other;
        if (d && d != details && d->is_tag("details") && dom::has_attribute(d, "open") && dom::get_attribute(d, "name", &other) && other.equals(name)) {
          dom::remove_attribute(d, "open");
          fire(d, "toggle", false);
        }
      }
    }
    dom::set_attribute(details, "open", "");
  } else {
    dom::remove_attribute(details, "open");
  }
  doc_.touch();
  fire(details, "toggle", false);
  flush_errors();
}

// Text pasted into a field: one line for a single-line field, filtered for a number field.
void Page::paste_text(dom::Node* control, StringView text) {
  String clean(allocator_);
  const dom::InputType t = control->is_tag("input") ? dom::input_type(control) : dom::InputType::Text;
  for (uint64_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\r') continue;
    if (c == '\n' && !control->is_tag("textarea")) c = ' ';
    if (static_cast<uint8_t>(c) < 32 && c != '\n') continue;
    if (static_cast<uint8_t>(c) > 126) c = '?';
    if (t == dom::InputType::Number && !((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) continue;
    clean.push_back(c);
  }
  if (!clean.empty() && dom::replace_selection(control, clean.view(), true)) input_changed(control, false);
}

void Page::flush_errors() {
  if (bindings_ && bindings_->errors()) {
    String text(allocator_);
    bindings_->take_error(&text);
    note_error(text.view());
  }
}

// Fires an event at a node for the scripts; false if a listener cancelled it. (Without scripts nothing listens.)
bool Page::fire(dom::Node* target, StringView type, bool bubbles, bool cancelable, dom::Node* submitter) {
  if (!bindings_) return true;
  EventInit init;
  init.bubbles = bubbles;
  init.cancelable = cancelable;
  init.submitter = submitter;
  return bindings_->dispatch(target, type, init);
}

bool Page::custom_error(void* context, const dom::Node* control) {
  Page* page = static_cast<Page*>(context);
  return page->bindings_ && page->bindings_->custom_validity(control, nullptr);
}

dom::Node* Page::focused() {
  if (!focus_) return nullptr;
  dom::Node* n = doc_.by_serial(focus_);
  if (n && dom::is_connected(n) && dom::is_focusable(n)) return n;
  focus_ = 0;
  return nullptr;
}

bool Page::has_controls() {
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page && frames_[i]->page->has_controls()) return true;
  if (controls_version_ == doc_.version()) return controls_cached_;
  controls_version_ = doc_.version();
  controls_cached_ = false;
  for (const dom::Node* n = doc_.root(); n && !controls_cached_; n = dom::next_in_tree(n, doc_.root()))
    controls_cached_ = n->is_element() && dom::is_focusable(n);
  return controls_cached_;
}

void Page::set_focus(dom::Node* control) {
  dom::Node* next = control && dom::is_focusable(control) ? control : nullptr;
  dom::Node* old = focused();
  if (next == old) return;
  if (old) {
    old->form_state &= static_cast<uint8_t>(~dom::kFormFocused);
    if (open_select_ == old->serial) open_select_ = 0;
    String now(allocator_);
    dom::get_value(old, &now);
    const bool changed = !now.view().equals(focus_value_.view());
    focus_ = 0;
    if (changed) fire(old, "change", true);
    fire(old, "blur", false);
    fire(old, "focusout", true);
  }
  if (next) {
    next->caret = next->anchor = 0xFFFF;
    next->form_state |= dom::kFormFocused;
    focus_ = next->serial;
    dom::get_value(next, &focus_value_);
    fire(next, "focus", false);
    fire(next, "focusin", true);
  }
  doc_.touch();
  flush_errors();
}

// The value of a control changed: `input` fires, and `change` too when the user has finished (a box checked, an item chosen, Enter pressed).
void Page::input_changed(dom::Node* control, bool final_change) {
  if (invalid_node_) {
    invalid_node_ = 0;
    invalid_text_.clear();
  }
  fire(control, "input", true);
  if (final_change) {
    fire(control, "change", true);
    dom::get_value(control, &focus_value_);
  }
  doc_.touch();
}

void Page::toggle_list(dom::Node* select) {
  open_select_ = open_select_ == select->serial ? 0 : select->serial;
  doc_.touch();
}

void Page::choose_option(dom::Node* option) {
  if (option->parent && option->parent->is_tag("datalist")) {   // a suggestion: the field takes it
    dom::Node* input = focused();
    if (input) {
      String v(allocator_);
      dom::option_value(option, &v);
      dom::set_value(input, v.view(), true);
      input->form_state |= dom::kFormEdited;
      input_changed(input, true);
    }
    return;
  }
  dom::Node* select = option->parent;
  while (select && !select->is_tag("select")) select = select->parent;
  if (!select || dom::is_disabled(option) || dom::is_disabled(select)) return;
  List<dom::Node*> options(allocator_);
  dom::select_options(select, &options);
  if (dom::has_attribute(select, "multiple")) {
    dom::set_checked(option, !dom::is_checked(option));
    select->form_state |= dom::kFormValueDirty;
  } else {
    for (uint64_t i = 0; i < options.count(); ++i)
      if (options[i] == option) dom::set_selected_index(select, static_cast<int32_t>(i));
    if (!dom::select_is_list(select)) open_select_ = 0;
  }
  set_focus(select);
  input_changed(select, true);
}

// A click (or Space / Enter) on a control or label or option. False if the node is none of those (the focus then leaves the form).
// The press of the pointer on a node moves the focus to the control under it (or out of the form, if it is on something else); labels and options leave it.
void Page::press_focus(dom::Node* node) {
  for (dom::Node* p = node; p && p->is_element(); p = p->parent) {
    if (p->is_tag("option") || p->is_tag("label")) return;
    if (p->is_tag("summary") && dom::is_focusable(p)) {
      set_focus(p);
      return;
    }
    if (dom::is_listed_control(p) && !p->is_tag("fieldset") && !p->is_tag("output")) {
      set_focus(p);
      return;
    }
    if (p->is_tag("a")) break;
  }
  if (focus_) set_focus(nullptr);
}

bool Page::activate_control(dom::Node* node) {
  dom::Node* c = nullptr;
  for (dom::Node* p = node; p && p->is_element(); p = p->parent) {
    if ((dom::is_listed_control(p) && !p->is_tag("fieldset") && !p->is_tag("output")) || p->is_tag("option") || p->is_tag("label") ||
        (p->is_tag("summary") && p->parent && p->parent->is_tag("details"))) {
      c = p;
      break;
    }
    if (p->is_tag("a")) break;
  }
  if (!c) {
    if (focus_) set_focus(nullptr);
    return false;
  }
  if (c->is_tag("summary")) {
    toggle_details(c->parent);
    return true;
  }
  if (c->is_tag("label")) {
    dom::Node* target = nullptr;
    StringView f;
    auto labelable = [](const dom::Node* d) {
      return d->is_tag("button") || d->is_tag("select") || d->is_tag("textarea") || (d->is_tag("input") && dom::input_type(d) != dom::InputType::Hidden);
    };
    if (dom::get_attribute(c, "for", &f)) {
      dom::Node* e = doc_.element_by_id(f);
      if (e && labelable(e)) target = e;
    } else {
      for (dom::Node* d = c->first_child; d && !target; d = dom::next_in_tree(d, c))
        if (d->is_element() && labelable(d)) target = d;
    }
    if (target && target != node && !dom::is_disabled(target)) {
      set_focus(target);
      if (fire(target, "click", true, true)) activate_control(target);
    }
    return true;
  }
  if (dom::is_disabled(c)) return true;
  if (c->is_tag("option")) {
    choose_option(c);
    return true;
  }
  if (c->is_tag("select")) {
    if (!dom::select_is_list(c)) toggle_list(c);
    return true;
  }
  if (c->is_tag("input")) {
    const dom::InputType t = dom::input_type(c);
    if (t == dom::InputType::File) {
      env_.pick_file(c->serial, dom::has_attribute(c, "multiple"));
      return true;
    }
    if (t == dom::InputType::Color) {
      String v(allocator_), next(allocator_);
      dom::get_value(c, &v);
      dom::next_color(v.view(), 1, &next);
      dom::set_value(c, next.view(), true);
      input_changed(c, true);
      return true;
    }
    if (t == dom::InputType::Checkbox) {
      c->form_state &= static_cast<uint8_t>(~dom::kFormIndeterminate);
      dom::set_checked(c, !dom::is_checked(c));
      input_changed(c, true);
      return true;
    }
    if (t == dom::InputType::Radio) {
      if (!dom::is_checked(c)) {
        dom::set_checked(c, true);
        input_changed(c, true);
      }
      return true;
    }
  }
  if (dom::is_submit_button(c)) submit_form(dom::form_owner(c), c, true);
  else if (dom::is_reset_button(c)) reset_form(dom::form_owner(c));
  return true;
}

void Page::reset_form(dom::Node* form) {
  if (!form) return;
  if (!fire(form, "reset", true, true)) {
    flush_errors();
    return;
  }
  dom::reset_form(form);
  invalid_node_ = 0;
  invalid_text_.clear();
  if (dom::Node* f = focused()) dom::get_value(f, &focus_value_);
  doc_.touch();
  flush_errors();
}

bool Page::report_validity(dom::Node* control) {
  dom::ValidityHooks hooks;
  hooks.has_custom_error = &Page::custom_error;
  hooks.context = this;
  const uint32_t flags = dom::validity(control, &hooks);
  if (flags == 0) return true;
  if (fire(control, "invalid", false, true)) {
    String message(allocator_);
    if (!(flags & dom::kCustomError) || !bindings_ || !bindings_->custom_validity(control, &message)) dom::validation_message(control, flags, &message);
    invalid_node_ = control->serial;
    invalid_text_.assign(message.view());
    set_focus(control);
  }
  doc_.touch();
  flush_errors();
  return false;
}

void Page::submit_form(dom::Node* form, dom::Node* submitter, bool interactive) {
  if (!form) return;
  if (interactive) {
    const bool unchecked = dom::has_attribute(form, "novalidate") || (submitter && dom::has_attribute(submitter, "formnovalidate"));
    if (!unchecked) {
      dom::ValidityHooks hooks;
      hooks.has_custom_error = &Page::custom_error;
      hooks.context = this;
      List<dom::Node*> controls(allocator_);
      dom::form_controls(form, &controls);
      bool any = false;
      for (uint64_t i = 0; i < controls.count(); ++i) {
        const uint32_t flags = dom::validity(controls[i], &hooks);
        if (flags == 0) continue;
        if (!any) {
          any = true;
          report_validity(controls[i]);
        } else {
          fire(controls[i], "invalid", false, true);
        }
      }
      if (any) return;
    }
    invalid_node_ = 0;
    invalid_text_.clear();
    const bool go = fire(form, "submit", true, true, submitter);
    flush_errors();
    if (!go) return;
  }
  send_form(form, submitter);
}

void Page::send_form(dom::Node* form, dom::Node* submitter) {
  auto pick = [&](const char* own, const char* fallback, StringView* out) {
    StringView v;
    if (submitter && dom::get_attribute(submitter, own, &v) && !v.empty()) {
      *out = v;
      return true;
    }
    if (dom::get_attribute(form, fallback, &v)) {
      *out = v;
      return true;
    }
    return false;
  };
  StringView method, action, enctype;
  pick("formmethod", "method", &method);
  const bool has_action = pick("formaction", "action", &action) && !action.trimmed().empty();
  pick("formenctype", "enctype", &enctype);
  if (method.trimmed().equals_ignore_case("dialog")) {   // the dialog the form is in closes, with the value of the button that was pressed
    dom::Node* dialog = form->parent;
    while (dialog && !dialog->is_tag("dialog")) dialog = dialog->parent;
    String value(allocator_);
    if (submitter) dom::get_value(submitter, &value);
    if (dialog) close_dialog_text(dialog, value.view());
    return;
  }
  const bool post = method.trimmed().equals_ignore_case("post");
  List<dom::Entry> entries(allocator_);
  if (!dom::form_entries(form, submitter, &entries)) return;
  String target(allocator_);
  if (has_action) {
    if (action.trimmed().starts_with("javascript:") || action.trimmed().starts_with("mailto:")) return;
    if (!resolve(action.trimmed(), &target)) return;
  } else {
    target.assign(address_.view());
  }
  const uint64_t hash = target.view().find('#');
  if (hash != StringView::npos) target.truncate(hash);
  String body(allocator_);
  if (!post) {
    const uint64_t query = target.view().find('?');
    if (query != StringView::npos) target.truncate(query);
    if (!dom::encode_urlencoded(entries, &body)) return;
    target.push_back('?');
    target.append(body.view());
    env_.navigate(target.view(), false);
    return;
  }
  StringView type = "application/x-www-form-urlencoded";
  String content_type(allocator_);
  const StringView kind = enctype.trimmed();
  bool ok;
  if (kind.equals_ignore_case("multipart/form-data")) {
    static const char kBoundary[] = "----TheoryCraftOSFormBoundary7MA4YWxkTrZu0gW";
    ok = dom::encode_multipart(entries, kBoundary, &body);
    content_type.assign("multipart/form-data; boundary=");
    content_type.append(kBoundary);
    type = content_type.view();
  } else if (kind.equals_ignore_case("text/plain")) {
    ok = dom::encode_plain(entries, &body);
    type = "text/plain";
  } else {
    ok = dom::encode_urlencoded(entries, &body);
  }
  if (ok) env_.navigate_post(target.view(), type, body.view());
}

// Tab and Shift+Tab: the next control (or the one before), and out of the form after the last one.
void Page::move_focus(int32_t direction) {
  List<dom::Node*> order(allocator_);
  dom::Node* modal = modal_dialog();
  for (dom::Node* n = doc_.root(); n; n = dom::next_in_tree(n, doc_.root()))
    if (n->is_element() && dom::is_focusable(n) && (!modal || dom::contains(modal, n))) order.push_back(n);
  if (order.count() == 0) return;
  int64_t at = -1;
  for (uint64_t i = 0; i < order.count(); ++i)
    if (order[i]->serial == focus_) at = static_cast<int64_t>(i);
  int64_t next = at < 0 ? (direction > 0 ? 0 : static_cast<int64_t>(order.count()) - 1) : at + direction;
  if (next < 0 || next >= static_cast<int64_t>(order.count())) {
    set_focus(nullptr);
    return;
  }
  set_focus(order[static_cast<uint64_t>(next)]);
}

bool Page::key(FormKey k, char character, uint8_t modifiers) {
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page && frames_[i]->page->has_focus()) return frames_[i]->page->key(k, character, modifiers);
  dom::Node* n = focused();
  if (!n) {
    if (k == FormKey::Character && (modifiers & 2) && !(modifiers & 4) && (character == 'a' || character == 'c' || character == 'x' || character == 'v' || character == 'A' || character == 'C' || character == 'X' || character == 'V')) {
      page_clipboard_key(nullptr, character);
      return true;
    }
    if (k == FormKey::Escape) {
      if (dom::Node* modal = modal_dialog()) {
        if (fire(modal, "cancel", false, true)) close_dialog_text(modal, StringView());
        return true;
      }
      return false;
    }
    if (k != FormKey::Tab) return false;
    move_focus((modifiers & 1) ? -1 : 1);
    return true;
  }
  if (bindings_) {
    static const struct {
      FormKey key;
      const char* name;
      double code;
    } kNames[] = {{FormKey::Enter, "Enter", 13}, {FormKey::Backspace, "Backspace", 8}, {FormKey::Delete, "Delete", 46}, {FormKey::Tab, "Tab", 9},
                  {FormKey::Escape, "Escape", 27}, {FormKey::Left, "ArrowLeft", 37}, {FormKey::Up, "ArrowUp", 38}, {FormKey::Right, "ArrowRight", 39},
                  {FormKey::Down, "ArrowDown", 40}, {FormKey::Home, "Home", 36}, {FormKey::End, "End", 35}};
    EventInit init;
    init.shift = modifiers & 1;
    init.ctrl = modifiers & 2;
    init.alt = modifiers & 4;
    init.key[0] = 0;
    if (k == FormKey::Character) {
      init.key[0] = character;
      init.key[1] = 0;
      init.key_code = character >= 'a' && character <= 'z' ? character - 32 : character;
    } else {
      for (const auto& e : kNames)
        if (e.key == k) {
          const char* p = e.name;
          for (uint32_t i = 0; i < 15 && *p; ++i) init.key[i] = *p++;
          init.key_code = e.code;
        }
    }
    const bool go = bindings_->dispatch(n, "keydown", init);
    flush_errors();
    if (!go) return true;
    if (k == FormKey::Character || k == FormKey::Enter) {
      const bool press = bindings_->dispatch(n, "keypress", init);
      flush_errors();
      if (!press) return true;
    }
    const bool consumed = press_key(n, k, character, modifiers);
    bindings_->dispatch(focused() ? focused() : n, "keyup", init);
    flush_errors();
    return consumed;
  }
  return press_key(n, k, character, modifiers);
}

bool Page::press_key(dom::Node* n, FormKey k, char ch, uint8_t modifiers) {
  const bool shift = modifiers & 1, ctrl = modifiers & 2, alt = modifiers & 4;
  if (k == FormKey::Character && ctrl && !alt && (ch == 'a' || ch == 'c' || ch == 'x' || ch == 'v' || ch == 'A' || ch == 'C' || ch == 'X' || ch == 'V')) {
    const bool text_field = (n->is_tag("input") && dom::is_text_field(dom::input_type(n))) || n->is_tag("textarea");
    if (!text_field) {   // (a button or link has focus: the keys work on the page)
      page_clipboard_key(n, ch);
      return true;
    }
  }
  if (k == FormKey::Tab) {
    move_focus(shift ? -1 : 1);
    return true;
  }
  if (k == FormKey::Escape) {
    dom::Node* modal = open_select_ ? nullptr : modal_dialog();
    if (open_select_) {
      open_select_ = 0;
      doc_.touch();
    } else if (modal) {
      if (fire(modal, "cancel", false, true)) close_dialog_text(modal, StringView());
    } else {
      set_focus(nullptr);
    }
    return true;
  }
  const bool space = k == FormKey::Character && ch == ' ';
  if (n->is_tag("input") || n->is_tag("textarea")) {
    const dom::InputType t = n->is_tag("input") ? dom::input_type(n) : dom::InputType::Text;
    if (dom::is_text_field(t)) {
      const bool editable = !dom::is_read_only(n);
      const bool textarea = n->is_tag("textarea");
      auto filtered = [&](char c) { return t != dom::InputType::Number || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'; };
      if (k == FormKey::Character && ctrl && !alt) {   // the clipboard, and select all
        uint32_t s, e, caret;
        dom::get_selection(n, &s, &e, &caret);
        String v(allocator_);
        dom::get_value(n, &v);
        if (ch == 'a' || ch == 'A') {
          dom::set_selection(n, 0, static_cast<uint32_t>(v.size()));
        } else if (ch == 'c' || ch == 'C' || ch == 'x' || ch == 'X') {
          const bool cut = ch == 'x' || ch == 'X';
          const bool handled = fire_clipboard(n, cut ? "cut" : "copy");   // (a script may do it all itself)
          if (!handled && s < e && (!cut || editable)) {
            if (t != dom::InputType::Password) env_.clipboard_set(v.view().substr(s, e - s));
            if (cut && t != dom::InputType::Password && dom::delete_selection(n, 1)) input_changed(n, false);
          }
        } else if (ch == 'v' || ch == 'V') {
          const bool handled = fire_clipboard(n, "paste");
          String clip(allocator_);
          if (!handled && editable && env_.clipboard_get(&clip)) paste_text(n, clip.view());
        }
        doc_.touch();
        return true;
      }
      if (k == FormKey::Character && !alt) {
        if (!editable || static_cast<uint8_t>(ch) < 32 || static_cast<uint8_t>(ch) > 126 || !filtered(ch)) return true;
        if (dom::replace_selection(n, StringView(&ch, 1), true)) input_changed(n, false);
        return true;
      }
      if (k == FormKey::Backspace || k == FormKey::Delete) {
        if (editable && dom::delete_selection(n, k == FormKey::Backspace ? -1 : 1)) input_changed(n, false);
        return true;
      }
      if (k == FormKey::Left || k == FormKey::Right) {
        dom::move_caret(n, k == FormKey::Left ? -1 : 1, shift);
        return true;
      }
      if (k == FormKey::Home || k == FormKey::End) {
        dom::move_caret(n, k == FormKey::Home ? INT32_MIN : INT32_MAX, shift);
        return true;
      }
      if (k == FormKey::Enter) {
        if (textarea) {
          if (editable && dom::replace_selection(n, "\n", true)) input_changed(n, false);
          return true;
        }
        String now(allocator_);
        dom::get_value(n, &now);
        if (!now.view().equals(focus_value_.view())) {
          fire(n, "change", true);
          focus_value_.assign(now.view());
        }
        dom::Node* form = dom::form_owner(n);
        if (!form) return true;
        List<dom::Node*> controls(allocator_);
        dom::form_controls(form, &controls);
        dom::Node* button = nullptr;
        uint32_t fields = 0;
        for (uint64_t i = 0; i < controls.count(); ++i) {
          if (!button && dom::is_submit_button(controls[i]) && !dom::is_disabled(controls[i])) button = controls[i];
          if (controls[i]->is_tag("input") && dom::is_text_field(dom::input_type(controls[i]))) ++fields;
        }
        if (button) {
          if (fire(button, "click", true, true)) activate_control(button);
        } else if (fields <= 1) {
          submit_form(form, nullptr, true);
        }
        return true;
      }
      if (k == FormKey::Up || k == FormKey::Down) {
        if (textarea) {   // (a line up or down, at the same place in it)
          String v(allocator_);
          dom::get_value(n, &v);
          uint32_t s, e, caret;
          dom::get_selection(n, &s, &e, &caret);
          uint64_t line_start = caret;
          while (line_start > 0 && v[line_start - 1] != '\n') --line_start;
          const uint64_t column = caret - line_start;
          int64_t target = caret;
          if (k == FormKey::Up) {
            if (line_start == 0) target = 0;
            else {
              uint64_t prev_start = line_start - 1;
              while (prev_start > 0 && v[prev_start - 1] != '\n') --prev_start;
              const uint64_t prev_len = line_start - 1 - prev_start;
              target = static_cast<int64_t>(prev_start + (column < prev_len ? column : prev_len));
            }
          } else {
            uint64_t line_end = caret;
            while (line_end < v.size() && v[line_end] != '\n') ++line_end;
            if (line_end >= v.size()) target = static_cast<int64_t>(v.size());
            else {
              uint64_t next_end = line_end + 1;
              while (next_end < v.size() && v[next_end] != '\n') ++next_end;
              const uint64_t next_len = next_end - (line_end + 1);
              target = static_cast<int64_t>(line_end + 1 + (column < next_len ? column : next_len));
            }
          }
          dom::move_caret(n, static_cast<int32_t>(target - static_cast<int64_t>(caret)), shift);
          return true;
        }
        if (t == dom::InputType::Number || t == dom::InputType::Date || t == dom::InputType::Time || t == dom::InputType::DateTimeLocal || t == dom::InputType::Month ||
            t == dom::InputType::Week) {
          if (editable && dom::step_value(n, k == FormKey::Up ? 1 : -1)) input_changed(n, true);
          return true;
        }
        return false;
      }
      return false;
    }
    if (t == dom::InputType::Color) {
      if (space || k == FormKey::Enter || k == FormKey::Up || k == FormKey::Down || k == FormKey::Left || k == FormKey::Right) {
        String v(allocator_), next(allocator_);
        dom::get_value(n, &v);
        dom::next_color(v.view(), (k == FormKey::Up || k == FormKey::Left) ? -1 : 1, &next);
        dom::set_value(n, next.view(), true);
        input_changed(n, true);
        return true;
      }
      return false;
    }
    if (t == dom::InputType::File) {
      if (space || k == FormKey::Enter) {
        env_.pick_file(n->serial, dom::has_attribute(n, "multiple"));
        return true;
      }
      return false;
    }
    if (t == dom::InputType::Range) {
      int32_t steps = 0;
      if (k == FormKey::Left || k == FormKey::Down) steps = -1;
      else if (k == FormKey::Right || k == FormKey::Up) steps = 1;
      else if (k == FormKey::Home) steps = -100000;
      else if (k == FormKey::End) steps = 100000;
      if (steps == 0) return false;
      if (dom::step_value(n, steps)) input_changed(n, true);
      return true;
    }
    if (t == dom::InputType::Checkbox || t == dom::InputType::Radio) {
      if (space) {
        if (fire(n, "click", true, true)) activate_control(n);
        return true;
      }
      if (t == dom::InputType::Radio && (k == FormKey::Left || k == FormKey::Right || k == FormKey::Up || k == FormKey::Down)) {
        const int32_t dir = (k == FormKey::Left || k == FormKey::Up) ? -1 : 1;
        List<dom::Node*> group(allocator_);
        dom::Node* owner = dom::form_owner(n);
        const dom::Node* root = owner ? owner : doc_.root();
        StringView name;
        dom::get_attribute(n, "name", &name);
        for (dom::Node* d = root->first_child; d; d = dom::next_in_tree(d, root))
          if (d->is_tag("input") && dom::input_type(d) == dom::InputType::Radio && dom::form_owner(d) == owner && !dom::is_disabled(d)) {
            StringView other;
            dom::get_attribute(d, "name", &other);
            if (other.equals(name)) group.push_back(d);
          }
        for (uint64_t i = 0; i < group.count(); ++i)
          if (group[i] == n) {
            dom::Node* next = group[(i + group.count() + (dir > 0 ? 1 : group.count() - 1)) % group.count()];
            set_focus(next);
            if (!dom::is_checked(next)) {
              dom::set_checked(next, true);
              input_changed(next, true);
            }
            break;
          }
        return true;
      }
      return k == FormKey::Enter ? (submit_form(dom::form_owner(n), nullptr, true), true) : false;
    }
    if (dom::is_button(n)) {
      if (space || k == FormKey::Enter) {
        if (fire(n, "click", true, true)) activate_control(n);
        return true;
      }
      return false;
    }
    return false;
  }
  if (n->is_tag("button")) {
    if (space || k == FormKey::Enter) {
      if (fire(n, "click", true, true)) activate_control(n);
      return true;
    }
    return false;
  }
  if (n->is_tag("summary")) {
    if (space || k == FormKey::Enter) {
      if (fire(n, "click", true, true)) activate_control(n);
      return true;
    }
    return false;
  }
  if (n->is_tag("select")) {
    if (k == FormKey::Enter || space) {
      if (!dom::select_is_list(n)) toggle_list(n);
      return true;
    }
    if (k == FormKey::Up || k == FormKey::Down || k == FormKey::Left || k == FormKey::Right) {
      List<dom::Node*> options(allocator_);
      dom::select_options(n, &options);
      const int32_t dir = (k == FormKey::Up || k == FormKey::Left) ? -1 : 1;
      int32_t at = dom::selected_index(n);
      for (int32_t i = at + dir; i >= 0 && i < static_cast<int32_t>(options.count()); i += dir) {
        if (dom::is_disabled(options[static_cast<uint64_t>(i)])) continue;
        dom::set_selected_index(n, i);
        input_changed(n, true);
        break;
      }
      return true;
    }
    if (k == FormKey::Home || k == FormKey::End) return true;
  }
  return false;
}


// --- Frames ------------------------------------------------------------------------------------------------------------------------

bool Page::changed() const {
  if (doc_.version() != rendered_version_ || media_ != rendered_media_ || fragment_pending_ || scroll_node_) return true;
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page && frames_[i]->page->changed()) return true;
  return false;
}

bool Page::has_focus() const {
  if (focus_ != 0) return true;
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->page && frames_[i]->page->has_focus()) return true;
  return false;
}

const HtmlDocument* Page::frame_document(const dom::Node* iframe, uint32_t* tag) {
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->serial == iframe->serial && frames_[i]->page) {
      *tag = static_cast<uint32_t>(i) + 1;
      return &frames_[i]->doc;
    }
  return nullptr;
}

// A file input of a frame was clicked: the browser is asked with the serial tagged by the frame's number, so that the file comes back to it.
void Page::frame_pick_file(uint32_t frame_serial, uint32_t node_serial, bool multiple) {
  for (uint64_t i = 0; i < frames_.count(); ++i)
    if (frames_[i]->serial == frame_serial) env_.pick_file((static_cast<uint32_t>(i + 1) << 24) | node_serial, multiple);
}

void Page::file_chosen(uint32_t node_serial, StringView name, StringView type, ConstByteView bytes) {
  if (node_serial >> 24) {
    const uint64_t frame = (node_serial >> 24) - 1;
    if (frame < frames_.count() && frames_[frame]->page) frames_[frame]->page->file_chosen(node_serial & 0xFFFFFF, name, type, bytes);
    return;
  }
  dom::Node* n = doc_.by_serial(node_serial);
  if (!n || !n->is_tag("input") || dom::input_type(n) != dom::InputType::File) return;
  dom::add_file(n, name, type, bytes, !dom::has_attribute(n, "multiple"));
  input_changed(n, true);
  flush_errors();
}

// Following a link in a frame: the frame shows the page.
void Page::frame_navigate(uint32_t frame_serial, StringView address) {
  dom::Node* n = doc_.by_serial(frame_serial);
  if (!n) return;
  dom::remove_attribute(n, "srcdoc");
  dom::set_attribute(n, "src", address);
  doc_.touch();
}

// Makes the pages of the <iframe>s that are new or whose address changed, draws those that changed, forgets those that are gone.
void Page::sync_frames() {
  List<dom::Node*> found(allocator_);
  for (uint32_t serial = 1; serial <= doc_.node_count() && found.count() < 8; ++serial) {
    dom::Node* n = doc_.by_serial(serial);
    if (n && n->is_tag("iframe") && dom::is_connected(n)) found.push_back(n);
  }
  for (uint64_t i = frames_.count(); i > 0; --i) {
    bool still = false;
    for (uint64_t k = 0; k < found.count(); ++k) still = still || found[k]->serial == frames_[i - 1]->serial;
    if (!still) frames_.remove_at(i - 1);
  }
  List<uint32_t> loaded(allocator_);
  for (uint64_t k = 0; k < found.count(); ++k) {
    dom::Node* n = found[k];
    String key(allocator_), html(allocator_), address(allocator_);
    StringView srcdoc, src;
    if (dom::get_attribute(n, "srcdoc", &srcdoc)) {
      key.assign("srcdoc:");
      key.append(srcdoc);
    } else if (dom::get_attribute(n, "src", &src) && !src.trimmed().empty() && !src.trimmed().equals_ignore_case("about:blank")) {
      if (src.trimmed().starts_with("data:")) key.assign(src.trimmed());
      else if (!resolve(src.trimmed(), &key)) key.assign("about:blank");
    } else {
      key.assign("about:blank");
    }
    Frame* f = nullptr;
    for (uint64_t i = 0; i < frames_.count(); ++i)
      if (frames_[i]->serial == n->serial) f = frames_[i].get();
    if (f && f->key.view().equals(key.view())) {
      if (f->page && f->page->changed()) f->page->render(&f->doc);
      continue;
    }
    if (depth_ >= 3) continue;
    if (!f) {
      auto fresh = UniquePointer<Frame>::make(allocator_, *this, n->serial, allocator_);
      if (!fresh || !frames_.push_back(util::move(fresh))) continue;
      f = frames_[frames_.count() - 1].get();
    }
    f->page.reset();
    f->key.assign(key.view());
    if (key.view().starts_with("srcdoc:")) {
      html.assign(srcdoc);
      address.assign(address_.view());
    } else if (key.view().starts_with("data:")) {
      ByteBuffer bytes(allocator_);
      String type(allocator_);
      address.assign(address_.view());
      if (decode_data_url(allocator_, key.view(), &bytes, &type)) html.assign(StringView(bytes.view()));
    } else if (key.view().equals("about:blank")) {
      address.assign("about:blank");
    } else {
      address.assign(key.view());
      if (!env_.fetch_text(key.view(), 1u << 20, &html)) html.assign("<p>This frame could not be loaded.</p>");
    }
    StringView sandbox;
    const bool sandboxed = dom::get_attribute(n, "sandbox", &sandbox) && !sandbox.contains("allow-scripts");
    MediaEnvironment frame_media = media_;
    if (frame_media.width > 16 * 8 * 2) frame_media.width -= 6 * 8;
    f->page = UniquePointer<Page>::make(allocator_, allocator_, f->env, address.view(), frame_media, scripts_enabled_ && !sandboxed);
    if (!f->page) continue;
    f->page->set_parent(this, n->serial, depth_ + 1);
    f->page->set_visible(!hidden_);
    f->page->load(html.view());
    f->page->render(&f->doc);
    loaded.push_back(n->serial);
  }
  for (uint64_t i = 0; i < loaded.count(); ++i)
    if (dom::Node* n = doc_.by_serial(loaded[i])) fire(n, "load", false);
}

// --- Messages ----------------------------------------------------------------------------------------------------------------------

// --- Windows, message ports, broadcasts and storage events ---------------------------------------------------------------------------

namespace {

// Calls `visit` with each number of a comma separated list ("1,2,3").
template <class F>
void each_number(StringView list, F&& visit) {
  StringView rest = list, token;
  while (rest.next_token(',', &token)) {
    uint32_t v = 0;
    bool ok = !token.empty();
    for (const char c : token) {
      if (c < '0' || c > '9') ok = false;
      else v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    if (ok) visit(v);
  }
}

}  // namespace

Page* Page::find_window(uint32_t id) {
  if (window_id_ == id) return this;
  for (uint64_t i = 0; i < frames_.count(); ++i) {
    if (!frames_[i]->page) continue;
    if (Page* p = frames_[i]->page->find_window(id)) return p;
  }
  return nullptr;
}

uint32_t Page::frame_window_id(uint32_t iframe_serial) {
  for (uint64_t i = 0; i < frame_windows_.count(); ++i)
    if (frame_windows_[i].port == iframe_serial) return frame_windows_[i].window;   // (the same number each time, also before the frame has been loaded)
  const uint32_t id = root()->next_window_id_++;
  frame_windows_.push_back(PortHome{iframe_serial, id});
  return id;
}

const String& Page::storage_origin() {
  if (storage_origin_.empty()) {
    WebUrl url(allocator_);
    if (WebUrl::parse(allocator_, address_.view(), nullptr, &url) && !url.is_file()) {
      url.origin(&storage_origin_);
    } else {
      storage_origin_.assign("file://");   // (a page from the file system has an opaque origin; its storage is kept apart per file)
      storage_origin_.append(address_.view());
    }
  }
  return storage_origin_;
}

void Page::cookie_get(String* out) {
  WebUrl url(allocator_);
  if (!WebUrl::parse(allocator_, address_.view(), nullptr, &url) || !(url.scheme.equals("http") || url.scheme.equals("https"))) return;
  env_.cookies().header_for(url, env_.now_ms(), CookieJar::Site::SameSite, true, out);
}

void Page::cookie_set(StringView text) {
  WebUrl url(allocator_);
  if (!WebUrl::parse(allocator_, address_.view(), nullptr, &url) || !(url.scheme.equals("http") || url.scheme.equals("https"))) return;
  env_.cookies().store(text, url, env_.now_ms(), true);
}

void Page::cookie_details(StringView target, String* out) {
  WebUrl page_url(allocator_), url(allocator_);
  if (!WebUrl::parse(allocator_, address_.view(), nullptr, &page_url) || !(page_url.scheme.equals("http") || page_url.scheme.equals("https"))) return;
  if (target.empty()) url = static_cast<WebUrl&&>(page_url);
  else {
    if (!WebUrl::parse(allocator_, target, &page_url, &url) || !page_url.same_origin(url)) return;
  }
  List<CookieJar::Cookie> list(allocator_);
  env_.cookies().list_for_script(url, env_.now_ms(), &list);
  for (uint64_t i = 0; i < list.count(); ++i) {
    const CookieJar::Cookie& c = list[i];
    out->append(c.name.view());
    out->push_back('\t');
    out->append(c.value.view());
    out->push_back('\t');
    out->append(c.domain.view());
    out->push_back('\t');
    out->append(c.path.view());
    out->push_back('\t');
    out->append_int(c.expires_ms);
    out->push_back('\t');
    out->append_uint(c.secure ? 1 : 0);
    out->push_back('\t');
    out->append_uint(c.same_site);
    out->push_back('\t');
    out->append_uint(c.partitioned ? 1 : 0);
    out->push_back('\t');
    out->append_uint(c.host_only ? 1 : 0);
    out->push_back('\n');
  }
}

bool Page::cookie_write(StringView text) {
  WebUrl url(allocator_);
  if (!WebUrl::parse(allocator_, address_.view(), nullptr, &url) || !(url.scheme.equals("http") || url.scheme.equals("https"))) return false;
  return env_.cookies().store(text, url, env_.now_ms(), true);
}

// --- Web Workers: a script engine of its own, without the document

struct Page::WorkerRuntime final : script::IHost {
  struct Event {
    explicit Event(allocators::IAllocator& a) : text(a) {}
    uint32_t kind = 0;
    String text;
  };
  WorkerRuntime(Page& owner, uint32_t worker_id) : page(owner), id(worker_id), url(owner.allocator_), name(owner.allocator_), inbox(owner.allocator_), events(owner.allocator_) {}
  Page& page;
  uint32_t id;
  UniquePointer<script::Engine> engine;
  String url, name;
  List<String> inbox;      // messages for the worker, serialized
  List<Event> events;      // what it said: messages, errors, its closing
  bool closed = false;
  void add_event(uint32_t kind, StringView text) {
    Event e(page.allocator_);
    e.kind = kind;
    e.text.assign(text);
    events.push_back(util::move(e));
  }
  // script::IHost
  void console(int level, StringView text) override {
    String line(page.allocator_);
    line.append("[worker] ");
    line.append(text);
    page.env_.console(level, line.view());
  }
  void alert(StringView) override {}
  void document_write(StringView, bool) override {}
  uint64_t now_ms() override { return page.env_.now_ms(); }
  StringView location() override { return url.view(); }
  StringView title() override { return StringView(); }
  void set_title(StringView) override {}
  StringView user_agent() override { return "Mozilla/5.0 (compatible; TheoryCraftOS-Browser/1.0)"; }
};

uint32_t Page::worker_start(StringView source, StringView url, StringView name, String* error) {
  if (workers_.count() >= 8) {
    error->assign("Too many workers are running.");
    return 0;
  }
  auto runtime = UniquePointer<WorkerRuntime>::make(allocator_, *this, next_worker_);
  if (!runtime) {
    error->assign("Out of memory.");
    return 0;
  }
  runtime->url.assign(url);
  runtime->name.assign(name);
  runtime->engine = UniquePointer<script::Engine>::make(allocator_, allocator_, static_cast<script::IHost*>(runtime.get()));
  if (!runtime->engine || !runtime->engine->ok()) {
    error->assign("Out of memory.");
    return 0;
  }
  runtime->engine->set_limits(4000000, 24u << 20);
  script::Vm& vm = runtime->engine->vm();
  vm.set_user(runtime.get());
  using script::Value;
  vm.define_native(vm.global(), "__out", [](script::Vm& v, const Value&, const Value* args, uint32_t argc, Value* result) {
    (void)result;
    script::JsString* text;
    if (argc > 0 && v.to_string(args[0], &text)) static_cast<WorkerRuntime*>(v.user())->add_event(0, text->view());
    return true;
  }, 1);
  vm.define_native(vm.global(), "__worker_close", [](script::Vm& v, const Value&, const Value*, uint32_t, Value*) {
    WorkerRuntime* w = static_cast<WorkerRuntime*>(v.user());
    if (!w->closed) {
      w->closed = true;
      w->add_event(2, StringView());
    }
    return true;
  }, 0);
  vm.define_native(vm.global(), "__worker_fail", [](script::Vm& v, const Value&, const Value* args, uint32_t argc, Value*) {
    script::JsString* text;
    if (argc > 0 && v.to_string(args[0], &text)) static_cast<WorkerRuntime*>(v.user())->add_event(1, text->view());
    return true;
  }, 1);
  vm.define_native(vm.global(), "__worker_fetch", [](script::Vm& v, const Value&, const Value* args, uint32_t argc, Value* result) {
    script::JsString* address;
    *result = Value::make_null();
    if (argc == 0 || !v.to_string(args[0], &address)) return true;
    WorkerRuntime* w = static_cast<WorkerRuntime*>(v.user());
    String text(w->page.allocator_);
    if (w->page.env_.fetch_text(address->view(), 1u << 20, &text)) *result = Value::make_string(v.new_string(text.view()));
    return true;
  }, 1);
  vm.define_native(vm.global(), "__bytes_to_string", [](script::Vm& v, const Value&, const Value* args, uint32_t argc, Value* result) {   // (TextDecoder)
    ArrayView<uint8_t> bytes;
    if (argc == 0 || !script::typed_array_bytes(args[0], &bytes)) return v.throw_error(script::ErrorKind::TypeError, "The provided value is not a byte array.");
    *result = Value::make_string(v.new_string(bytes_as_text(ConstByteView(bytes.data(), bytes.size()))));
    return true;
  }, 1);
  vm.define_native(vm.global(), "__string_to_bytes", [](script::Vm& v, const Value&, const Value* args, uint32_t argc, Value* result) {   // (TextEncoder)
    script::JsString* text;
    if (argc == 0 || !v.to_string(args[0], &text)) return false;
    *result = Value::make_object(script::new_typed_array(v, script::ArrayKind::Uint8, text->view()));
    return true;
  }, 1);
  vm.define_named(vm.global(), "__worker_url", Value::make_string(vm.new_string(url)), 0);
  vm.define_named(vm.global(), "__worker_name", Value::make_string(vm.new_string(name)), 0);
  String problem(allocator_);
  if (!runtime->engine->run_library(worker_prelude(), &problem)) {
    error->assign(problem.view());
    return 0;
  }
  const uint32_t id = next_worker_++;
  runtime->id = id;
  if (!workers_.push_back(util::move(runtime))) {
    error->assign("Out of memory.");
    return 0;
  }
  WorkerRuntime* started = workers_[workers_.count() - 1].get();
  String result(allocator_);
  if (!started->engine->run(source, &problem, &result)) started->add_event(1, problem.view());   // (an error in the script is the worker's error event)
  return id;
}

void Page::worker_post(uint32_t id, StringView message) {
  for (uint64_t i = 0; i < workers_.count(); ++i) {
    WorkerRuntime* w = workers_[i].get();
    if (w->id != id || w->closed || w->inbox.count() > 1000) continue;
    String text(allocator_);
    text.assign(message);
    w->inbox.push_back(util::move(text));
  }
}

void Page::worker_terminate(uint32_t id) {
  for (uint64_t i = 0; i < workers_.count(); ++i)
    if (workers_[i]->id == id) {
      workers_.remove_at(i);
      return;
    }
}

void Page::worker_pump() {
  for (uint64_t i = 0; i < workers_.count(); ++i) {
    WorkerRuntime* w = workers_[i].get();
    if (w->closed) continue;
    String problem(allocator_), ignored(allocator_);
    for (uint32_t guard = 0; guard < 100 && w->inbox.count() > 0 && !w->closed; ++guard) {   // the messages for the worker
      script::Vm& vm = w->engine->vm();
      vm.define_named(vm.global(), "__inbox", script::Value::make_string(vm.new_string(w->inbox[0].view())), script::kWritable);
      w->inbox.remove_at(0);
      problem.clear();
      if (!w->engine->run("__deliver()", &problem, &ignored)) w->add_event(1, problem.view());
    }
    if (w->closed) continue;
    problem.clear();
    w->engine->run_due_timers(static_cast<double>(env_.now_ms()), 50, &problem);
    if (!problem.empty()) w->add_event(1, problem.view());
  }
}

bool Page::worker_next_event(uint32_t* id, uint32_t* kind, String* text) {
  for (uint64_t i = 0; i < workers_.count(); ++i) {
    WorkerRuntime* w = workers_[i].get();
    if (w->events.count() == 0) continue;
    *id = w->id;
    *kind = w->events[0].kind;
    text->assign(w->events[0].text.view());
    w->events.remove_at(0);
    return true;
  }
  return false;
}

IWebSocket* Page::socket_of(uint32_t id) {
  for (uint64_t i = 0; i < sockets_.count(); ++i)
    if (sockets_[i].id == id) return sockets_[i].socket.get();
  return nullptr;
}

uint32_t Page::ws_open(StringView url, StringView protocols, String* error) {
  if (sockets_.count() >= 64) {
    error->assign("Too many WebSockets are open.");
    return 0;
  }
  String origin(allocator_);
  page_origin(&origin);
  UniquePointer<IWebSocket> opened = env_.open_websocket(url, protocols, origin.view(), error);
  if (!opened) return 0;
  Socket s;
  s.id = next_socket_++;
  s.socket = util::move(opened);
  const uint32_t id = s.id;
  if (!sockets_.push_back(util::move(s))) {
    error->assign("Out of memory.");
    return 0;
  }
  return id;
}

int Page::ws_state(uint32_t id) {
  IWebSocket* ws = socket_of(id);
  if (!ws) return 3;
  ws->poll();
  switch (ws->state()) {
    case IWebSocket::State::Connecting: return 0;
    case IWebSocket::State::Open: return 1;
    case IWebSocket::State::Closing: return 2;
    default: return 3;
  }
}

bool Page::ws_send(uint32_t id, bool text, ConstByteView data) {
  IWebSocket* ws = socket_of(id);
  return ws && ws->send(text, data);
}

void Page::ws_close(uint32_t id, uint16_t code, StringView reason) {
  if (IWebSocket* ws = socket_of(id)) ws->close(code, reason);
}

bool Page::ws_receive(uint32_t id, bool* text, ByteBuffer* data) {
  IWebSocket* ws = socket_of(id);
  if (!ws) return false;
  ws->poll();
  return ws->receive(text, data);
}

void Page::ws_info(uint32_t id, String* protocol, uint16_t* code, String* reason, bool* opened) {
  IWebSocket* ws = socket_of(id);
  if (!ws) return;
  protocol->assign(ws->protocol());
  *code = ws->close_code();
  reason->assign(ws->close_reason());
  *opened = ws->opened();
}

void Page::ws_release(uint32_t id) {
  for (uint64_t i = 0; i < sockets_.count(); ++i)
    if (sockets_[i].id == id) {
      sockets_.remove_at(i);
      return;
    }
}

void Page::page_origin(String* out) {
  WebUrl url(allocator_);
  if (WebUrl::parse(allocator_, address_.view(), nullptr, &url)) url.origin(out);
  else out->append("null");
}

void Page::claim_ports(StringView ports, uint32_t window) {
  Page* top = root();
  each_number(ports, [&](uint32_t id) { top->move_port(id, window); });
}

bool Page::post_window_message(uint32_t target_window, StringView target_origin, StringView payload, StringView ports) {
  Page* target = root()->find_window(target_window);
  if (!target) return false;
  String mine(allocator_), theirs(allocator_);
  page_origin(&mine);
  target->page_origin(&theirs);
  if (target_origin.equals("/")) {
    if (!theirs.equals(mine.view())) return true;   // (a message for the wrong origin is dropped without a word)
  } else if (!target_origin.equals("*")) {
    WebUrl wanted(allocator_);
    String wanted_origin(allocator_);
    if (!WebUrl::parse(allocator_, target_origin, nullptr, &wanted)) return false;
    wanted.origin(&wanted_origin);
    if (!theirs.equals(wanted_origin.view())) return true;
  }
  claim_ports(ports, target->window_id_);
  target->queue_window_message(payload, mine.view(), window_id_, ports);
  return true;
}

void Page::queue_window_message(StringView payload, StringView origin, uint32_t source, StringView ports) {
  if (messages_.count() >= 256) return;
  Message m(allocator_);
  m.payload.assign(payload);
  m.origin.assign(origin);
  m.source = source;
  m.ports.assign(ports);
  messages_.push_back(util::move(m));
}

void Page::queue_port_message(uint32_t port, StringView payload, StringView ports) {
  if (messages_.count() >= 256) return;
  Message m(allocator_);
  m.to_port = true;
  m.port = port;
  m.payload.assign(payload);
  m.ports.assign(ports);
  messages_.push_back(util::move(m));
}

uint32_t Page::new_port_pair() {
  Page* top = root();
  const uint32_t first = top->next_port_;
  top->next_port_ += 2;
  top->port_homes_.push_back(PortHome{first, window_id_});
  top->port_homes_.push_back(PortHome{first + 1, window_id_});
  return first;
}

void Page::move_port(uint32_t port, uint32_t window) {
  Page* top = root();
  for (uint64_t i = 0; i < top->port_homes_.count(); ++i) {
    if (top->port_homes_[i].port == port) {
      top->port_homes_[i].window = window;
      return;
    }
  }
  top->port_homes_.push_back(PortHome{port, window});
}

void Page::post_port_message(uint32_t port, StringView payload, StringView ports) {
  Page* top = root();
  for (uint64_t i = 0; i < top->port_homes_.count(); ++i) {
    if (top->port_homes_[i].port != port) continue;
    Page* target = top->find_window(top->port_homes_[i].window);
    if (!target) return;
    claim_ports(ports, target->window_id_);
    target->queue_port_message(port, payload, ports);
    return;
  }
}

void Page::broadcast(StringView channel, StringView payload) {
  HubEvent e(allocator_);
  e.kind = HubEvent::Kind::Broadcast;
  e.source = hub_id_;
  e.origin.assign(storage_origin());
  e.name.assign(channel);
  e.first.assign(payload);
  env_.page_hub().post(util::move(e));
}

void Page::storage_changed(bool session, const String* key, const String* old_value, const String* new_value) {
  HubEvent e(allocator_);
  e.kind = HubEvent::Kind::Storage;
  e.source = hub_id_;
  e.group = session ? env_.tab_id() : 0;
  e.origin.assign(storage_origin());
  if (key) e.name.assign(key->view());
  e.key_null = key == nullptr;
  if (old_value) e.first.assign(old_value->view());
  e.old_null = old_value == nullptr;
  if (new_value) e.second.assign(new_value->view());
  e.new_null = new_value == nullptr;
  e.url.assign(address_.view());
  env_.page_hub().post(util::move(e));
}

// The events other pages made for this one: storage changes and broadcast messages.
void Page::poll_hub() {
  if (!bindings_) {
    hub_seen_ = env_.page_hub().serial();
    return;
  }
  HubEvent e(allocator_);
  while (env_.page_hub().next(hub_id_, storage_origin(), &hub_seen_, &e)) {
    using Arg = DomBindings::HookArg;
    if (e.kind == HubEvent::Kind::Storage) {
      if (e.group != 0 && e.group != env_.tab_id()) continue;   // (another tab's sessionStorage)
      const Arg args[5] = {Arg::of(e.group != 0), e.key_null ? Arg::null() : Arg::of(e.name.view()), e.old_null ? Arg::null() : Arg::of(e.first.view()),
                           e.new_null ? Arg::null() : Arg::of(e.second.view()), Arg::of(e.url.view())};
      bindings_->call_hook("__storage_event", args, 5);
    } else {
      const Arg args[2] = {Arg::of(e.name.view()), Arg::of(e.first.view())};
      bindings_->call_hook("__bc_incoming", args, 2);
    }
  }
  flush_errors();
}

void Page::deliver_messages() {
  while (messages_.count() > 0 && bindings_) {
    Message m = util::move(messages_[0]);
    messages_.remove_at(0);
    using Arg = DomBindings::HookArg;
    if (m.to_port) {
      const Arg args[3] = {Arg::of(static_cast<double>(m.port)), Arg::of(m.payload.view()), Arg::of(m.ports.view())};
      bindings_->call_hook("__port_deliver", args, 3);
    } else {
      const Arg args[4] = {Arg::of(m.payload.view()), Arg::of(m.origin.view()), Arg::of(static_cast<double>(m.source)), Arg::of(m.ports.view())};
      bindings_->call_hook("__fire_message", args, 4);
    }
  }
  messages_.clear();
  flush_errors();
}

}  // namespace browser
