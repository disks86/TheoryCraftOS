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
#include "Audio/AudioPlayer.hpp"
#include "Media/AudioStreaming.hpp"
#include "Utility/AllocatedView.hpp"
#include "Video/VideoPlayer.hpp"

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

// A video of a page, played by a video::VideoPlayer (on the machine's video hardware when it has some; its sound goes through the mixer).
class BrowserVideoClip final : public browser::IVideoClip {
 public:
  BrowserVideoClip(allocators::IAllocator& allocator, const hardware::IClock& clock, hardware::SystemVideoCodecs& codecs, audio::AudioMixer* mixer, UniquePointer<ByteBuffer> data)
      : data_(util::move(data)), source_(data_->view()), player_(clock, codecs, allocator, mixer) {}
  bool open() { return player_.open_media(source_) == video::OpenResult::Ok; }

  bool play() override { return player_.play(); }
  void pause() override { player_.pause(); }
  void seek(int64_t microseconds) override { player_.seek(microseconds); }
  void set_looping(bool looping) override { player_.set_looping(looping); }
  void set_volume(double volume, bool muted) override {
    if (!player_.audio()) return;
    const double v = muted ? 0 : volume < 0 ? 0 : volume > 1 ? 1 : volume;
    player_.audio()->set_volume(static_cast<uint8_t>(v * 100 + 0.5));
  }
  bool playing() override { return player_.playing(); }
  int64_t position_us() override { return player_.position_us(); }
  int64_t duration_us() override { return player_.duration_us(); }
  uint32_t width() override { return player_.frame_width(); }
  uint32_t height() override { return player_.frame_height(); }
  void update() override { player_.update(); }
  bool copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* serial) override {
    if (pitch < player_.frame_width() || dest.size() < uint64_t{pitch} * player_.frame_height()) return false;
    return player_.copy_picture(dest.data(), pitch, serial);
  }

 private:
  UniquePointer<ByteBuffer> data_;
  media::MemoryByteSource source_;
  video::VideoPlayer player_;
};

// A sound of a page (<audio>): the file is decoded as it plays (media::open_audio_stream) and goes through an audio::AudioPlayer on the machine's mixer.
class BrowserAudioClip final : public browser::IVideoClip {
 public:
  BrowserAudioClip(allocators::IAllocator& allocator, audio::AudioMixer& mixer, UniquePointer<ByteBuffer> data)
      : data_(util::move(data)), source_(data_->view()), player_(mixer, allocator), allocator_(allocator) {}
  ~BrowserAudioClip() { player_.close(); }
  bool open() {
    stream_ = media::open_audio_stream(source_, allocator_);
    return stream_ && stream_->frame_count() > 0 && player_.open(stream_.get());
  }

  bool play() override { return player_.play(); }
  void pause() override { player_.pause(); }
  void seek(int64_t microseconds) override {
    if (microseconds < 0) microseconds = 0;
    player_.seek(static_cast<uint64_t>(microseconds) * player_.sample_rate() / 1000000);
  }
  void set_looping(bool looping) override { player_.set_looping(looping); }
  void set_volume(double volume, bool muted) override {
    player_.set_volume(static_cast<uint8_t>((volume < 0 ? 0 : volume > 1 ? 1 : volume) * 100 + 0.5));
    player_.set_muted(muted);
  }
  bool playing() override { return player_.playing(); }
  int64_t position_us() override { return player_.sample_rate() ? static_cast<int64_t>(player_.position() * 1000000 / player_.sample_rate()) : 0; }
  int64_t duration_us() override { return player_.sample_rate() ? static_cast<int64_t>(player_.duration() * 1000000 / player_.sample_rate()) : 0; }
  uint32_t width() override { return 0; }
  uint32_t height() override { return 0; }
  void update() override { player_.update(); }
  bool copy_picture(ArrayView<uint32_t>, uint32_t, uint32_t*) override { return false; }

 private:
  UniquePointer<ByteBuffer> data_;
  media::MemoryByteSource source_;
  UniquePointer<audio::IPcmSource> stream_;
  audio::AudioPlayer player_;
  allocators::IAllocator& allocator_;
};

const char kWelcome[] =
    "<html><head><title>Welcome</title></head><body>"
    "<h1>TheoryCraftOS Web Browser</h1>"
    "<p>Type an address in the box above and press Enter. Web addresses (http:// and https://) and files on this computer "
    "(such as /docs/index.html) can be opened.</p>"
    "<hr>"
    "<h2>Keys</h2>"
    "<ul><li>Ctrl+L: go to the address box</li><li>Alt+Left, Alt+Right: back and forward</li><li>F5 or Ctrl+R: reload</li>"
    "<li>Ctrl+O: open a file</li><li>Space, Page Up, Page Down: scroll</li></ul>"
    "<p>This browser shows text, headings, lists, tables and links, with the colours and spacing that stylesheets give them, and pictures, videos, sounds and canvases.</p>"
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
      env_(*this, allocator),
      defer_go_(allocator),
      defer_type_(allocator),
      defer_body_(allocator),
      request_type_(allocator),
      request_body_(allocator),
      script_log_(allocator),
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
  if (Menu* view = menu_.add_menu("View")) {
    scripts_item_ = view->add_item("Run Scripts", {&WebBrowser::on_toggle_scripts, this});
    dark_item_ = view->add_item("Dark Colors", {&WebBrowser::on_toggle_dark, this});
    if (scripts_item_) scripts_item_->set_checked(scripts_enabled_);
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
  node_handler_ = {&WebBrowser::on_node_click, this};
  resize_handler_ = {&WebBrowser::on_view_resized, this};
  timer_handler_ = {&WebBrowser::on_timers, this};
  key_handler_ = {&WebBrowser::on_form_key, this};
  page_.on_node_clicked(node_handler_);
  page_.on_key_pressed(key_handler_);
  page_.on_resized(resize_handler_);

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

WebBrowser::~WebBrowser() {
  UiGuard lock(&page_);
  page_.set_object_painter(nullptr);
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

namespace {
const FileDialog::Filter kAnyFile[] = {{"All files", "*"}};

// What kind of file a name says it is (for the type a form sends with it).
const char* type_of_name(StringView name) {
  const uint64_t dot = name.rfind('.');
  const StringView ext = dot == StringView::npos ? StringView() : name.substr(dot + 1);
  static const struct {
    const char* ext;
    const char* type;
  } kTypes[] = {{"txt", "text/plain"}, {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"}, {"json", "application/json"},
                {"xml", "application/xml"}, {"csv", "text/csv"}, {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
                {"bmp", "image/bmp"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"}, {"pdf", "application/pdf"}, {"zip", "application/zip"}, {"wav", "audio/wav"},
                {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"}};
  for (const auto& t : kTypes)
    if (ext.equals_ignore_case(t.ext)) return t.type;
  return "application/octet-stream";
}
}  // namespace

// A file input was clicked: the file dialog opens, and what is picked goes back to the input.
void WebBrowser::ask_file(uint32_t node_serial) {
  if (!dialog_ || !manager()) return;
  pending_file_node_ = node_serial;
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Choose File", kAnyFile, 1, StringView(), {&WebBrowser::on_file_picked, this})) notify(NoticeKind::Warning, "The file dialog is busy.");
}

void WebBrowser::on_file_picked(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (!b->dialog_->accepted() || !b->fs_ || !b->live_) return;
  String path(b->allocator());
  path.assign(b->dialog_->selected_path());
  UniquePointer<IFile> file;
  if (b->fs_->open_file(path.view(), OpenMode::Read, &file) != Status::Ok) return;
  const uint64_t size = file->size();
  if (size > (16u << 20)) {
    b->notify(NoticeKind::Warning, "That file is too large to send.");
    return;
  }
  ByteBuffer bytes(b->allocator());
  ArrayView<uint8_t> chunk = allocate_view<uint8_t>(b->allocator(), size ? size : 1);
  uint64_t got = 0;
  const Status status = chunk.empty() ? Status::OutOfMemory : size ? file->read(0, chunk.first(size), &got) : Status::Ok;
  file.reset();
  const bool ok = status == Status::Ok && bytes.append(ConstByteView(chunk.data(), got));
  release_view(b->allocator(), chunk);
  if (!ok) return;
  const uint64_t slash = path.view().rfind('/');
  const StringView name = slash == StringView::npos ? path.view() : path.view().substr(slash + 1);
  b->live_->file_chosen(b->pending_file_node_, name, type_of_name(name), bytes.view());
  b->settle();
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
bool WebBrowser::post_form(StringView target, StringView content_type, StringView body) {
  {
    UiGuard lock(this);
    request_ = Request::Submit;
    request_target_.assign(target);
    request_type_.assign(content_type);
    request_body_.assign(body);
  }
  fire(load_handler_);
  return true;
}

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
      case Request::Submit: address.assign(target); break;
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
  bool ok;
  if (kind == Request::Submit) ok = fetch_post(address.view(), &page);
  else ok = fetch(address.view(), &page);
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

bool WebBrowser::fetch(StringView address, Fetched* page, uint64_t limit) {
  if (browser::is_file_address(address)) return fetch_file(address, page, limit);
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

// A form sent with POST: the answer is the page that is shown (only web addresses can be posted to).
bool WebBrowser::fetch_post(StringView address, Fetched* page) {
  String type(allocator()), body(allocator());
  {
    UiGuard lock(this);
    type.assign(request_type_.view());
    body.assign(request_body_.view());
  }
  if (!browser::is_web_address(address) || !loader_) {
    page->error.assign(loader_ ? "A form can only be sent to a web address." : "Web pages cannot be opened: there is no network connection.");
    return false;
  }
  browser::PageResult result(allocator());
  if (!loader_->post(address, type.view(), body.view(), &result)) {
    String message(allocator());
    message.append("Cannot send the form: ");
    message.append(result.error.empty() ? StringView("the server did not answer.") : result.error.view());
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

bool WebBrowser::fetch_file(StringView path, Fetched* page, uint64_t limit) {
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
  if (size > limit) {
    problem.append("the file is too large.");
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
  {
    UiGuard lock(&page_);
    page_.set_object_painter(nullptr);
  }
  live_.reset();   // (the old page and its scripts go before the new one comes)
  {
    UiGuard lock(this);
    script_log_.clear();
    script_errors_ = 0;
    reported_errors_ = 0;
    defer_reload_ = want_go_ = want_post_ = false;
    defer_history_ = 0;
    defer_scroll_ = -1;
    next_due_ms_ = 0;
  }
  bool ok = true;
  if (is_html(page.type.view(), page.address.view(), body)) {
    media_ = current_media();
    live_ = UniquePointer<browser::Page>::make(allocator(), allocator(), env_, page.address.view(), media_, scripts_enabled_);
    if (!live_) ok = false;
    else {
      say(scripts_enabled_ ? "Reading the page and running its scripts..." : "Reading the page...");
      ok = live_->load(body) && live_->render(doc.get());
      if (ok) page_.set_object_painter(&live_->objects());
    }
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
    live_.reset();
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  present(util::move(doc), false, page.status);
  settle();
}

// Puts a drawn page on the screen, with its title and the state of its scripts in the status bar.
void WebBrowser::present(UniquePointer<browser::HtmlDocument> doc, bool keep_scroll, uint32_t http_status) {
  String page_title(allocator());
  page_title.assign(live_ && !live_->page_title().empty() ? live_->page_title() : doc->title());
  const bool cut = doc->truncated();
  page_.set_document(util::move(doc), keep_scroll);
  int32_t scroll = defer_scroll_;
  defer_scroll_ = -1;
  if (live_) {
    const int32_t asked = live_->take_scroll_request();
    if (asked >= 0) scroll = asked;
  }
  if (scroll >= 0) page_.scroll_to_line(static_cast<uint32_t>(scroll));
  uint32_t errors = 0;
  String first_error(allocator());
  {
    UiGuard lock(this);
    if (live_) {
      script_log_.assign(live_->log());
      script_errors_ = errors = live_->errors();
      first_error.assign(live_->first_error());
    }
    address_.set_text(current_.view());
    update_buttons();
    update_title(page_title.view());
  }
  if (keep_scroll && errors == reported_errors_) return;   // (drawn again: the status bar keeps what it said, unless there is news)
  reported_errors_ = errors;
  String message(allocator());
  if (http_status >= 400) {
    message.append("HTTP ");
    message.append_uint(http_status);
    message.append(": ");
  }
  message.append(cut ? "Done (the page is cut off)." : "Done.");
  if (errors > 0) {
    message.truncate(message.size() - 1);
    message.append(" - ");
    message.append_uint(errors);
    message.append(errors == 1 ? " script error: " : " script errors; first: ");
    message.append(first_error.view());
  }
  say(message.view());
}

// Draws the loaded page again (its tree changed, or the display did).
void WebBrowser::refresh(bool keep_scroll) {
  if (!live_) return;
  UniquePointer<browser::HtmlDocument> doc = UniquePointer<browser::HtmlDocument>::make(allocator(), allocator());
  if (!doc || !live_->render(doc.get())) {
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  present(util::move(doc), keep_scroll, 0);
}

// After the page ran something (a click, a timer, a new window size): do what it asked of the browser, then draw it again if it changed.
void WebBrowser::settle() {
  if (want_post_) {
    String target(allocator()), type(allocator()), body(allocator());
    target.assign(defer_go_.view());
    type.assign(defer_type_.view());
    body.assign(defer_body_.view());
    want_post_ = false;
    post_form(target.view(), type.view(), body.view());
    return;
  }
  if (want_go_) {
    String target(allocator());
    target.assign(defer_go_.view());
    want_go_ = false;
    post(Request::Go, target.view());
    return;
  }
  if (defer_reload_) {
    defer_reload_ = false;
    post(Request::Reload, StringView());
    return;
  }
  if (defer_history_ != 0) {
    const int32_t delta = defer_history_;
    defer_history_ = 0;
    if (delta < 0) go_back();
    else go_forward();
    return;
  }
  if (!live_) return;
  canvas_accelerated_ = live_->objects().accelerated();
  if (live_->take_visual_change()) {   // (a canvas was drawn on, a video has a new picture: only the view needs painting again)
    UiGuard lock(&page_);
    page_.invalidate();
  }
  update_key_capture();
  if (live_->changed() || defer_scroll_ >= 0) refresh(true);
  else if (live_->errors() != reported_errors_) {
    UiGuard lock(this);
    script_log_.assign(live_->log());
    script_errors_ = live_->errors();
  }
  schedule_timers();
}

// Keys go to the page while a control has the focus (and Tab is the page's whenever it has a control to step to).
void WebBrowser::update_key_capture() {
  if (!live_) {
    page_.set_capture_keys(PageView::kCaptureNone);
    return;
  }
  const uint8_t mode = live_->has_focus() || live_->has_modal() ? PageView::kCaptureAll : live_->has_controls() ? PageView::kCaptureTab : PageView::kCaptureNone;
  page_.set_capture_keys(mode);
}

void WebBrowser::on_form_key(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  namespace key = hardware::key;
  KeyArgs args;
  while (b->page_.take_key(&args)) {
    if (!b->live_) {
      b->page_.scroll_key(args);
      continue;
    }
    using Key = browser::Page::FormKey;
    Key k = Key::Character;
    switch (args.usage) {
      case key::kEnter: k = Key::Enter; break;
      case key::kBackspace: k = Key::Backspace; break;
      case key::kDelete: k = Key::Delete; break;
      case key::kTab: k = Key::Tab; break;
      case key::kEscape: k = Key::Escape; break;
      case key::kLeft: k = Key::Left; break;
      case key::kRight: k = Key::Right; break;
      case key::kUp: k = Key::Up; break;
      case key::kDown: k = Key::Down; break;
      case 0x4A: k = Key::Home; break;
      case 0x4D: k = Key::End; break;
      default: break;
    }
    const bool control = args.modifiers & hardware::modifier::kControl;
    char character = args.character;
    if (control && args.usage >= key::kA && args.usage < key::kA + 26) character = static_cast<char>('a' + (args.usage - key::kA));   // (Ctrl+A ... Ctrl+Z)
    const uint8_t mods = static_cast<uint8_t>((args.modifiers & hardware::modifier::kShift ? 1 : 0) | (control ? 2 : 0));
    bool taken;
    if (k == Key::Character && (character < 32 || character > 126)) taken = false;
    else taken = b->live_->key(k, character, mods);
    if (!taken) b->page_.scroll_key(args);
  }
  b->settle();
}

void WebBrowser::schedule_timers() {
  double due = 0;
  const bool any = live_ && live_->next_timer(&due);
  __atomic_store_n(&next_due_ms_, any ? static_cast<uint64_t>(due < 1 ? 1 : due) : 0, __ATOMIC_RELEASE);
}

browser::MediaEnvironment WebBrowser::current_media() {
  browser::MediaEnvironment m;
  UiGuard lock(&page_);
  if (page_.columns() > 0) {
    m.width = static_cast<int32_t>(page_.columns()) * Painter::kGlyphWidth;
    m.height = static_cast<int32_t>(page_.visible_lines()) * PageView::kLineHeight;
  }
  m.dark = dark_;
  m.scripting = scripts_enabled_;
  return m;
}

uint64_t WebBrowser::page_clock_ms() {
  const uint64_t up = __atomic_load_n(&uptime_ms_, __ATOMIC_ACQUIRE);
  if (!unix_clock_) return up + 1;
  if (!clock_based_) {
    clock_base_unix_ = unix_clock_(unix_clock_context_);
    clock_base_up_ = up;
    clock_based_ = true;
  }
  return clock_base_unix_ + (up - clock_base_up_);
}

void WebBrowser::tick(uint64_t uptime_us) {
  __atomic_store_n(&uptime_ms_, uptime_us / 1000, __ATOMIC_RELEASE);
  const uint64_t due = __atomic_load_n(&next_due_ms_, __ATOMIC_ACQUIRE);
  if (due == 0 || page_clock_ms() < due || __atomic_exchange_n(&timer_pending_, true, __ATOMIC_ACQ_REL)) return;
  fire(timer_handler_);
}

void WebBrowser::on_timers(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  __atomic_store_n(&b->timer_pending_, false, __ATOMIC_RELEASE);
  if (!b->live_) return;
  const double now = static_cast<double>(b->page_clock_ms());
  b->live_->run_due_timers(now);
  b->live_->step_objects(now);
  b->settle();
}

void WebBrowser::on_node_click(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (!b->live_) return;
  b->live_->click(b->page_.clicked_node(), b->page_.clicked_x(), b->page_.clicked_y(), b->page_.clicked_object(), b->page_.clicked_local_x(), b->page_.clicked_local_y(),
                  b->page_.clicked_width(), b->page_.clicked_height());
  b->settle();
}

// The window changed size (or the colour scheme was switched): the page is told the new viewport.
void WebBrowser::on_view_resized(void* self, Control&) { static_cast<WebBrowser*>(self)->update_media(); }

void WebBrowser::update_media() {
  const browser::MediaEnvironment m = current_media();
  if (m == media_ || !live_) {
    media_ = m;
    return;
  }
  media_ = m;
  live_->set_media(m);
  settle();
}

void WebBrowser::set_scripts_enabled(bool enabled) {
  scripts_enabled_ = enabled;
  if (scripts_item_) scripts_item_->set_checked(enabled);
}

void WebBrowser::set_dark(bool dark) {
  dark_ = dark;
  if (dark_item_) dark_item_->set_checked(dark);
  fire(resize_handler_);
}

void WebBrowser::on_toggle_scripts(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  b->set_scripts_enabled(!b->scripts_enabled_);
  b->reload();
}

void WebBrowser::on_toggle_dark(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  b->set_dark(!b->dark_);
}

// --- What a page asks of the window ------------------------------------------------------------------

bool WebBrowser::Env::fetch_bytes(StringView address, uint64_t limit, ByteBuffer* out) {
  Fetched f(owner_.allocator());
  f.address.assign(address);
  if (!owner_.fetch(address, &f, limit) || f.status >= 400 || f.body.size() > limit) return false;
  return out->append(f.body.view());
}

UniquePointer<browser::IVideoClip> WebBrowser::Env::open_video(UniquePointer<ByteBuffer> data) {
  if (!owner_.codecs_ || !owner_.clock_ || !data) return UniquePointer<browser::IVideoClip>();
  auto clip = UniquePointer<BrowserVideoClip>::make(owner_.allocator(), owner_.allocator(), *owner_.clock_, *owner_.codecs_, owner_.mixer_, util::move(data));
  if (!clip || !clip->open()) return UniquePointer<browser::IVideoClip>();
  return clip;
}

UniquePointer<browser::IVideoClip> WebBrowser::Env::open_audio(UniquePointer<ByteBuffer> data) {
  if (!owner_.mixer_ || !data) return UniquePointer<browser::IVideoClip>();
  auto clip = UniquePointer<BrowserAudioClip>::make(owner_.allocator(), owner_.allocator(), *owner_.mixer_, util::move(data));
  if (!clip || !clip->open()) return UniquePointer<browser::IVideoClip>();
  return clip;
}

bool WebBrowser::Env::fetch_text(StringView address, uint64_t limit, String* text) {
  Fetched f(owner_.allocator());
  f.address.assign(address);
  if (!owner_.fetch(address, &f) || f.status >= 400 || f.body.size() > limit) return false;
  return text->assign(StringView(f.body.view()));
}

void WebBrowser::Env::navigate(StringView address, bool) {
  owner_.defer_go_.assign(address);
  owner_.want_go_ = true;
}

bool WebBrowser::Env::clipboard_get(String* text) {
  gui::Window* w = owner_.window();
  kernel::Clipboard* clip = w ? w->clipboard() : nullptr;
  return clip && clip->get_text(text);
}

void WebBrowser::Env::clipboard_set(StringView text) {
  gui::Window* w = owner_.window();
  kernel::Clipboard* clip = w ? w->clipboard() : nullptr;
  if (clip) clip->set_text(text);
}

void WebBrowser::Env::pick_file(uint32_t node_serial, bool) { owner_.ask_file(node_serial); }

void WebBrowser::Env::navigate_post(StringView address, StringView content_type, StringView body) {
  owner_.defer_go_.assign(address);
  owner_.defer_type_.assign(content_type);
  owner_.defer_body_.assign(body);
  owner_.want_post_ = true;
}

void WebBrowser::Env::set_hash(StringView hash) { owner_.apply_hash(hash); }
void WebBrowser::Env::reload() { owner_.defer_reload_ = true; }
void WebBrowser::Env::history_go(int32_t delta) { owner_.defer_history_ += delta; }
uint32_t WebBrowser::Env::history_length() { return static_cast<uint32_t>(owner_.history_.count()); }

void WebBrowser::Env::push_state(StringView address, bool replace) {
  UiGuard lock(&owner_);
  owner_.remember(replace ? Request::Reload : Request::Go, address);
  owner_.address_.set_text(owner_.current_.view());
  owner_.update_buttons();
}

void WebBrowser::Env::alert(StringView text) { owner_.notify(NoticeKind::Information, text); }
void WebBrowser::Env::scroll_to(int32_t line) { owner_.defer_scroll_ = line; }
uint64_t WebBrowser::Env::now_ms() { return owner_.page_clock_ms(); }

// Following a link to "#something" on the page shown: the address changes, the history entry too; "#" and "#top" scroll to the top.
void WebBrowser::apply_hash(StringView hash) {
  UiGuard lock(this);
  String address(allocator());
  const uint64_t at = current_.view().find('#');
  address.assign(at == StringView::npos ? current_.view() : current_.view().first(at));
  if (hash.size() > 1) address.append(hash);
  remember(Request::Reload, address.view());
  address_.set_text(current_.view());
  if (hash.size() <= 1 || hash.equals_ignore_case("#top")) defer_scroll_ = 0;
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
