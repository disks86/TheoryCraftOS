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
  RenderOptions options;
  options.media = media_;
  options.styles = this;
  const bool ok = render_document(doc_, options, out);
  rendered_version_ = doc_.version();
  rendered_media_ = media_;
  return ok;
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

void Page::click(uint32_t node_serial, int32_t x, int32_t y) {
  dom::Node* n = doc_.by_serial(node_serial);
  if (!n) return;
  if (!bindings_) {   // no scripts: a click only follows a link
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
  if (bindings_->dispatch(n, "click", init)) bindings_->activate(n);
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

bool Page::next_timer(double* due_ms) { return engine_ && engine_->next_timer(due_ms); }

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

}  // namespace browser
