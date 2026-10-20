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

#include "Desktop/WebBrowser.hpp"
#include "Browser/Url.hpp"
#include "Desktop/FileStatus.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

using namespace gui;
using filesystem::IFile;
using filesystem::OpenMode;
using filesystem::Status;

// A page as it came: where from, what the server said it is, and its bytes.
struct WebBrowser::Fetched {
  explicit Fetched(allocators::IAllocator& allocator) : address(allocator), type(allocator), error(allocator), body(allocator) {}
  String address, type, error;
  ByteBuffer body;
  uint32_t status = 0;
};

namespace {

const char kWelcome[] =
    "<html><head><title>Welcome</title></head><body>"
    "<h1>TheoryCraftOS Web Browser</h1>"
    "<p>Type an address in the box above and press Enter. Web addresses (http:// and https://) and files on this computer "
    "(such as /docs/index.html) can be opened.</p>"
    "<hr>"
    "<h2>Keys</h2>"
    "<ul><li>Ctrl+L: go to the address box</li><li>Alt+Left, Alt+Right: back and forward</li><li>F5 or Ctrl+R: reload</li>"
    "<li>Ctrl+O: open a file</li><li>Space, Page Up, Page Down: scroll</li></ul>"
    "<p>This browser shows text, headings, lists, tables and links. It does not show pictures or run scripts.</p>"
    "<p>A place to start: <a href=\"http://example.com/\">http://example.com/</a></p>"
    "</body></html>";

bool contains_ignore_case(StringView text, const char* needle) {
  const StringView n(needle);
  for (uint64_t i = 0; i + n.size() <= text.size(); ++i)
    if (text.substr(i, n.size()).equals_ignore_case(n)) return true;
  return false;
}

// Is this HTML? By what the server said, else by the name, else by how it starts.
bool is_html(StringView type, StringView address, StringView body) {
  if (!type.empty() && contains_ignore_case(type, "html")) return true;
  if (!type.empty() && !contains_ignore_case(type, "text/plain") && contains_ignore_case(type, "/")) return false;
  if (WebBrowser::is_page_name(address)) return true;
  const StringView head = body.first(body.size() < 256 ? body.size() : 256).trimmed();
  return head.starts_with('<') && (contains_ignore_case(head, "<!doctype html") || contains_ignore_case(head, "<html") || contains_ignore_case(head, "<head") ||
                                    contains_ignore_case(head, "<body") || contains_ignore_case(head, "<p>") || contains_ignore_case(head, "<h1"));
}

// Can this be read as text? (no NUL bytes near the start, and not a type that is plainly something else)
bool is_text(StringView type, StringView body) {
  if (!type.empty() && (contains_ignore_case(type, "image/") || contains_ignore_case(type, "audio/") || contains_ignore_case(type, "video/") ||
                        contains_ignore_case(type, "octet-stream") || contains_ignore_case(type, "zip") || contains_ignore_case(type, "pdf")))
    return false;
  for (uint64_t i = 0; i < body.size() && i < 512; ++i)
    if (body[i] == 0) return false;
  return true;
}

}  // namespace

bool WebBrowser::is_page_name(StringView name) {
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos) return false;
  const StringView ext = name.substr(dot + 1);
  return ext.equals_ignore_case("html") || ext.equals_ignore_case("htm");
}

WebBrowser::WebBrowser(allocators::IAllocator& allocator)
    : Frame(allocator),
      menu_(allocator),
      toolbar_(allocator),
      status_bar_(allocator),
      back_(allocator),
      forward_(allocator),
      reload_(allocator),
      go_(allocator),
      address_(allocator),
      status_(allocator),
      page_(allocator),
      history_(allocator),
      current_(allocator),
      request_target_(allocator),
      status_text_(allocator) {
  set_bounds({100, 50, 680, 480});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);
  load_handler_ = {&WebBrowser::on_load, this};

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("Open File...", {&WebBrowser::on_open, this}, "Ctrl+O");
    file->add_separator();
    file->add_item("Close", {&WebBrowser::on_close, this});
  }
  if (Menu* go = menu_.add_menu("Go")) {
    go->add_item("Back", {&WebBrowser::on_back, this}, "Alt+Left");
    go->add_item("Forward", {&WebBrowser::on_forward, this}, "Alt+Right");
    go->add_item("Reload", {&WebBrowser::on_reload, this}, "F5");
  }

  toolbar_.set_style(PanelStyle::Transparent);
  toolbar_.set_dock(DockStyle::Top);
  toolbar_.set_size(0, 28);
  toolbar_.set_padding(Insets::all(1));
  back_.set_text("<");
  back_.set_dock(DockStyle::Left);
  back_.set_size(28, 0);
  back_.on_click({&WebBrowser::on_back, this});
  forward_.set_text(">");
  forward_.set_dock(DockStyle::Left);
  forward_.set_size(28, 0);
  forward_.on_click({&WebBrowser::on_forward, this});
  reload_.set_text("Reload");
  reload_.set_dock(DockStyle::Left);
  reload_.set_size(64, 0);
  reload_.on_click({&WebBrowser::on_reload, this});
  go_.set_text("Go");
  go_.set_dock(DockStyle::Right);
  go_.set_size(40, 0);
  go_.on_click({&WebBrowser::on_go, this});
  address_.set_dock(DockStyle::Fill);
  toolbar_.add(&back_);
  toolbar_.add(&forward_);
  toolbar_.add(&reload_);
  toolbar_.add(&go_);
  toolbar_.add(&address_);   // Fill goes last

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&status_);

  page_.set_dock(DockStyle::Fill);
  page_.on_link_clicked(Handler::immediate(&WebBrowser::on_link, this));
  page_.on_hover_changed(Handler::immediate(&WebBrowser::on_hover, this));

  add(&menu_);
  add(&toolbar_);
  add(&status_bar_);
  add(&page_);   // Fill goes last
  update_buttons();
  update_title(StringView());

  Fetched welcome(allocator);
  welcome.body.append(ConstByteView(reinterpret_cast<const uint8_t*>(kWelcome), sizeof kWelcome - 1));
  welcome.type.assign("text/html");
  show(welcome);
  say("Type an address and press Enter.");
}

void WebBrowser::say(StringView text) {
  UiGuard lock(this);
  status_text_.assign(text);
  status_.set_text(text);
}

void WebBrowser::update_buttons() {
  back_.set_enabled(can_go_back());
  forward_.set_enabled(can_go_forward());
}

void WebBrowser::update_title(StringView page_title) {
  String title(allocator());
  title.assign("Web Browser");
  const StringView name = page_title.empty() ? browser::address_name(current_.view()) : page_title;
  if (!name.empty()) {
    title.append(" - ");
    title.append(name);
  }
  set_title(title.view());
}

void WebBrowser::show_address(StringView address) { address_.set_text(address); }

// --- Commands ---------------------------------------------------------------------------------------

void WebBrowser::on_back(void* self, Control&) { static_cast<WebBrowser*>(self)->go_back(); }
void WebBrowser::on_forward(void* self, Control&) { static_cast<WebBrowser*>(self)->go_forward(); }
void WebBrowser::on_reload(void* self, Control&) { static_cast<WebBrowser*>(self)->reload(); }
void WebBrowser::on_go(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  b->navigate(b->address_.text());
}
void WebBrowser::on_open(void* self, Control&) { static_cast<WebBrowser*>(self)->ask_open(); }
void WebBrowser::on_close(void* self, Control&) { static_cast<WebBrowser*>(self)->close(); }
void WebBrowser::on_load(void* self, Control&) { static_cast<WebBrowser*>(self)->run_request(); }

void WebBrowser::on_open_picked(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (b->dialog_->accepted()) b->navigate(b->dialog_->selected_path());
}

void WebBrowser::on_link(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  String target(b->allocator());
  if (browser::resolve_reference(b->allocator(), b->current_.view(), b->page_.clicked_link(), &target)) {
    b->navigate(target.view());
    return;
  }
  String message(b->allocator());
  message.append("Cannot follow ");
  message.append(b->page_.clicked_link());
  b->say(message.view());
}

void WebBrowser::on_hover(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  const StringView link = b->page_.hovered_link();
  b->status_.set_text(link.empty() ? b->status_text_.view() : link);
}

namespace {
const FileDialog::Filter kPageFilters[] = {{"Web pages", "html;htm"}, {"Text files", "txt;md;log;cfg;ini;csv;json;xml"}, {"All files", "*"}};
}

void WebBrowser::ask_open() {
  if (!dialog_ || !manager()) {
    notify(NoticeKind::Error, "No file dialog available.");
    return;
  }
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kPageFilters, 3, current_.view().starts_with('/') ? current_.view() : StringView(), {&WebBrowser::on_open_picked, this}))
    notify(NoticeKind::Warning, "The file dialog is busy.");
}

bool WebBrowser::navigate(StringView typed) {
  String address(allocator());
  if (!browser::normalize_address(typed, &address)) {
    say("Type an address first.");
    return false;
  }
  if (!browser::is_web_address(address.view()) && !browser::is_file_address(address.view())) {
    String message(allocator());
    message.append("Cannot open ");
    message.append(address.view());
    message.append(": only web addresses (http:// and https://) and files are supported.");
    notify(NoticeKind::Warning, message.view());
    return false;
  }
  return post(Request::Go, address.view());
}

bool WebBrowser::go_back() { return can_go_back() && post(Request::Back, StringView()); }
bool WebBrowser::go_forward() { return can_go_forward() && post(Request::Forward, StringView()); }

bool WebBrowser::reload() {
  String address(allocator());
  if (current_.empty() && !browser::normalize_address(address_.text(), &address)) return false;
  return post(Request::Reload, address.view());
}

// Makes a request and has the handler thread carry it out (the latest request wins).
bool WebBrowser::post(Request kind, StringView target) {
  {
    UiGuard lock(this);
    request_ = kind;
    request_target_.assign(target);
  }
  fire(load_handler_);
  return true;
}

void WebBrowser::run_request() {
  Request kind;
  String target(allocator());
  {
    UiGuard lock(this);
    kind = request_;
    request_ = Request::None;
    target.assign(request_target_.view());
  }
  if (kind != Request::None) load(kind, target.view());
}

// --- Loading ----------------------------------------------------------------------------------------

void WebBrowser::load(Request kind, StringView target) {
  String address(allocator());
  {
    UiGuard lock(this);
    switch (kind) {
      case Request::Go: address.assign(target); break;
      case Request::Back:
        if (index_ <= 0) return;
        address.assign(history_[static_cast<uint64_t>(index_ - 1)].view());
        break;
      case Request::Forward:
        if (index_ + 1 >= static_cast<int64_t>(history_.count())) return;
        address.assign(history_[static_cast<uint64_t>(index_ + 1)].view());
        break;
      case Request::Reload: address.assign(current_.empty() ? target : current_.view()); break;
      default: return;
    }
    loading_ = true;
  }
  String message(allocator());
  message.append("Loading ");
  message.append(address.view());
  message.append("...");
  say(message.view());
  Fetched page(allocator());
  page.address.assign(address.view());
  const bool ok = fetch(address.view(), &page);
  if (!ok) {
    {
      UiGuard lock(this);
      loading_ = false;
      address_.set_text(current_.view());
    }
    say("Failed.");
    notify(NoticeKind::Error, page.error.view());
    return;
  }
  {
    UiGuard lock(this);
    remember(kind, page.address.view());
    loading_ = false;
  }
  show(page);
}

bool WebBrowser::fetch(StringView address, Fetched* page) {
  if (browser::is_file_address(address)) return fetch_file(address, page);
  if (!loader_) {
    page->error.assign("Web pages cannot be opened: there is no network connection.");
    return false;
  }
  browser::PageResult result(allocator());
  if (!loader_->fetch(address, &result)) {
    String message(allocator());
    message.append("Cannot open ");
    message.append(address.size() > 24 ? address.first(24) : address);   // (the notice dialog has room for four short lines)
    if (address.size() > 24) message.append("...");
    message.append(": ");
    message.append(result.error.empty() ? StringView("the page could not be fetched.") : result.error.view());
    page->error.assign(message.view());
    return false;
  }
  if (!result.final_address.empty()) page->address.assign(result.final_address.view());
  page->type.assign(result.content_type.view());
  page->status = result.status;
  if (!page->body.append(result.body.view())) {
    page->error.assign("Out of memory.");
    return false;
  }
  return true;
}

bool WebBrowser::fetch_file(StringView path, Fetched* page) {
  String name(allocator());
  name.assign(path.starts_with("file://") ? path.substr(7) : path);
  page->address.assign(name.view());
  String problem(allocator());
  problem.append("Cannot open ");
  problem.append(name.view());
  problem.append(": ");
  if (!fs_) {
    problem.append("no storage available.");
    page->error.assign(problem.view());
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(name.view(), OpenMode::Read, &file);
  if (status != Status::Ok) {
    problem.append(describe_status(status));
    page->error.assign(problem.view());
    return false;
  }
  const uint64_t size = file->size();
  if (size > kMaxFileBytes) {
    problem.append("the file is too large (limit 4 MiB).");
    page->error.assign(problem.view());
    return false;
  }
  ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator(), size ? size : 1);
  uint64_t got = 0;
  status = bytes.empty() ? Status::OutOfMemory : size ? file->read(0, bytes.first(size), &got) : Status::Ok;
  file.reset();
  const bool ok = status == Status::Ok && page->body.append(ConstByteView(bytes.data(), got));
  release_view(allocator(), bytes);
  if (!ok) {
    problem.append(status == Status::Ok ? "out of memory." : "the file could not be read.");
    page->error.assign(problem.view());
    return false;
  }
  return true;
}

// Keeps the page just loaded in the history (as a new entry, or as the one Back / Forward / Reload went to).
void WebBrowser::remember(Request kind, StringView address) {
  switch (kind) {
    case Request::Back:
      if (index_ > 0) --index_;
      break;
    case Request::Forward:
      if (index_ + 1 < static_cast<int64_t>(history_.count())) ++index_;
      break;
    default: {
      const bool same = index_ >= 0 && history_[static_cast<uint64_t>(index_)].view().equals(address);
      if (kind == Request::Reload && index_ >= 0) {
        history_[static_cast<uint64_t>(index_)].assign(address);
      } else if (!same) {
        while (static_cast<int64_t>(history_.count()) > index_ + 1) history_.remove_at(history_.count() - 1);
        String entry(allocator());
        entry.assign(address);
        if (history_.push_back(util::move(entry))) ++index_;
        if (history_.count() > kMaxHistory) {
          history_.remove_at(0);
          --index_;
        }
      }
      break;
    }
  }
  current_.assign(address);
}

// Shows a fetched page: as HTML, as text, or a note that it cannot be shown.
void WebBrowser::show(Fetched& page) {
  const StringView body(page.body.view());
  UniquePointer<browser::HtmlDocument> doc = UniquePointer<browser::HtmlDocument>::make(allocator(), allocator());
  if (!doc) {
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  bool ok = true;
  if (is_html(page.type.view(), page.address.view(), body)) {
    ok = doc->parse_html(body);
  } else if (is_text(page.type.view(), body)) {
    ok = doc->parse_text(body);
  } else {
    String text(allocator());
    text.append("This kind of file (");
    text.append(page.type.empty() ? StringView("unknown type") : page.type.view());
    text.append(") cannot be shown.");
    ok = doc->parse_text(text.view());
  }
  if (!ok) {
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  const StringView title = doc->title();
  String page_title(allocator());
  page_title.assign(title);
  const bool cut = doc->truncated();
  page_.set_document(util::move(doc));
  {
    UiGuard lock(this);
    address_.set_text(current_.view());
    update_buttons();
    update_title(page_title.view());
  }
  String message(allocator());
  if (page.status >= 400) {
    message.append("HTTP ");
    message.append_uint(page.status);
    message.append(": ");
  }
  message.append(cut ? "Done (the page is cut off)." : "Done.");
  say(message.view());
}

// --- Keys -------------------------------------------------------------------------------------------

bool WebBrowser::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  const bool control = args.modifiers & hardware::modifier::kControl;
  const bool alt = args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt);
  if (args.usage == key::kEnter && address_.focused()) {
    navigate(address_.text());
    return true;
  }
  if (alt && args.usage == key::kLeft) {
    go_back();
    return true;
  }
  if (alt && args.usage == key::kRight) {
    go_forward();
    return true;
  }
  if (args.usage == key::kF1 + 4) {   // F5
    reload();
    return true;
  }
  if (control) {
    switch (args.usage) {
      case key::kA + 17: reload(); return true;   // R
      case key::kA + 14: ask_open(); return true; // O
      case key::kA + 11:                          // L
        if (Window* w = window()) {
          w->set_focus(&address_);
          address_.select_all();
        }
        return true;
      default: break;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
