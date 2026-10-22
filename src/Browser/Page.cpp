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
#include "Browser/Url.hpp"
#include "Dom/Form.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Utility/Move.hpp"

namespace browser {

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
      session_(allocator),
      doc_(allocator),
      extra_documents_(allocator),
      scripts_(allocator),
      sheets_(allocator),
      objects_(allocator),
      focus_value_(allocator),
      invalid_text_(allocator),
      style_cache_(allocator) {
  address_.assign(address);
  style_cache_media_ = media;
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
      bindings_->set_ready_state(loading_ ? "loading" : "complete");
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

bool Page::load(StringView html) {
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
    if (bindings_->errors()) {
      String text(allocator_);
      bindings_->take_error(&text);
      note_error(text.view());
    }
  }
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
void Page::fetch_pending_styles() {
  for (const dom::Node* n = doc_.root(); n; n = dom::next_in_tree(n, doc_.root())) {
    if (!n->is_tag("link") || sheets_.count() >= kMaxStyleSheets) continue;
    StringView rel, href;
    if (!dom::get_attribute(n, "rel", &rel) || !dom::get_attribute(n, "href", &href) || href.empty()) continue;
    bool stylesheet = false;
    uint64_t i = 0;
    while (i < rel.size()) {
      while (i < rel.size() && rel[i] == ' ') ++i;
      const uint64_t start = i;
      while (i < rel.size() && rel[i] != ' ') ++i;
      if (rel.substr(start, i - start).equals_ignore_case("stylesheet")) stylesheet = true;
    }
    if (!stylesheet || fetched_already(n)) continue;
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
  fetch_pending_styles();
  objects_.set_now(env_.now_ms());
  objects_.sync(doc_, env_, address_.view(), media_);
  RenderOptions options;
  options.media = media_;
  options.styles = this;
  options.objects = &objects_;
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

// Fires what the pictures and videos have to say (load, error, loadedmetadata, ended ...) at their elements.
void Page::deliver_object_events() {
  PageObjects::Event e;
  uint32_t guard = 0;
  while (guard++ < 600 && objects_.take_event(&e)) {
    if (!bindings_) continue;
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
  objects_.update(static_cast<uint64_t>(now_ms));
  deliver_object_events();
  return objects_.take_dirty();
}

// --- After the page is up ------------------------------------------------------------------------------------------------------

void Page::set_media(const MediaEnvironment& media) {
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

void Page::click(uint32_t node_serial, int32_t x, int32_t y, uint8_t object, int32_t lx, int32_t ly, uint32_t w, uint32_t h) {
  dom::Node* n = doc_.by_serial(node_serial);
  if (!n) return;
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
  if (!engine_) return false;
  double due;
  if (!engine_->next_timer(&due) || due > now_ms) return false;
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

void Page::scroll_to(int32_t line) {
  scroll_request_ = line;
  env_.scroll_to(line);
}

void Page::set_hash(StringView hash) {
  String base(allocator_);
  const uint64_t at = address_.view().find('#');
  base.assign(at == StringView::npos ? address_.view() : address_.view().first(at));
  if (hash.size() > 1) base.append(hash);
  address_.assign(base.view());
  env_.set_hash(hash);
  if (bindings_) {
    EventInit plain;
    plain.cancelable = false;
    plain.bubbles = false;
    bindings_->dispatch(nullptr, "hashchange", plain);
  }
}

void Page::push_state(StringView address, bool replace) {
  address_.assign(address);
  env_.push_state(address, replace);
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
    if ((dom::is_listed_control(p) && !p->is_tag("fieldset") && !p->is_tag("output")) || p->is_tag("option") || p->is_tag("label")) {
      c = p;
      break;
    }
    if (p->is_tag("a")) break;
  }
  if (!c) {
    if (focus_) set_focus(nullptr);
    return false;
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
    if (t == dom::InputType::Checkbox) {
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
  if (method.trimmed().equals_ignore_case("dialog")) return;
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
  for (dom::Node* n = doc_.root(); n; n = dom::next_in_tree(n, doc_.root()))
    if (n->is_element() && dom::is_focusable(n)) order.push_back(n);
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
  dom::Node* n = focused();
  if (!n) {
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
  if (k == FormKey::Tab) {
    move_focus(shift ? -1 : 1);
    return true;
  }
  if (k == FormKey::Escape) {
    if (open_select_) {
      open_select_ = 0;
      doc_.touch();
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
      if (k == FormKey::Character && !ctrl && !alt) {
        if (!editable || static_cast<uint8_t>(ch) < 32 || static_cast<uint8_t>(ch) > 126) return true;
        if (t == dom::InputType::Number && !(ch >= '0' && ch <= '9') && ch != '.' && ch != '-' && ch != '+' && ch != 'e' && ch != 'E') return true;
        String v(allocator_);
        dom::get_value(n, &v);
        uint64_t max;
        StringView limit;
        if (dom::get_attribute(n, "maxlength", &limit) && limit.trimmed().parse_uint(&max) && v.size() >= max) return true;
        v.push_back(ch);
        dom::set_value(n, v.view(), false);
        n->form_state |= dom::kFormEdited;
        input_changed(n, false);
        return true;
      }
      if (k == FormKey::Backspace) {
        String v(allocator_);
        dom::get_value(n, &v);
        if (editable && v.size() > 0) {
          v.truncate(v.size() - 1);
          dom::set_value(n, v.view(), false);
          n->form_state |= dom::kFormEdited;
          input_changed(n, false);
        }
        return true;
      }
      if (k == FormKey::Enter) {
        if (n->is_tag("textarea")) {
          if (editable) {
            String v(allocator_);
            dom::get_value(n, &v);
            v.push_back('\n');
            dom::set_value(n, v.view(), false);
            n->form_state |= dom::kFormEdited;
            input_changed(n, false);
          }
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
        if (t == dom::InputType::Number) {
          if (editable && dom::step_value(n, k == FormKey::Up ? 1 : -1)) input_changed(n, true);
          return true;
        }
        return n->is_tag("textarea");
      }
      return k == FormKey::Delete || k == FormKey::Left || k == FormKey::Right || k == FormKey::Home || k == FormKey::End;
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

}  // namespace browser
