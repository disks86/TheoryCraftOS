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
#include "Browser/WebRandom.hpp"
#include "Browser/CacheStore.hpp"
#include "Browser/ServiceWorker.hpp"
#include "Browser/Fetcher.hpp"
#include "Browser/Url.hpp"
#include "Browser/WebUrl.hpp"
#include "Media/ImageCodecs.hpp"
#include "Desktop/FileStatus.hpp"
#include "Gui/Window.hpp"
#include "Gui/Windows/TabFrame.hpp"
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

static void visible_referrer(allocators::IAllocator& allocator, StringView from, StringView to, String* out);

bool WebBrowser::is_page_name(StringView name) {
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos) return false;
  const StringView ext = name.substr(dot + 1);
  return ext.equals_ignore_case("html") || ext.equals_ignore_case("htm");
}

WebBrowser::WebBrowser(allocators::IAllocator& allocator)
    : Frame(allocator),
      rich_html_(allocator),
      menu_(allocator),
      back_menu_(allocator),
      forward_menu_(allocator),
      leave_dialog_(allocator),
      leave_target_(allocator),
      pending_referrer_(allocator),
      referrer_(allocator),
      refresh_target_(allocator),
      defer_opens_(allocator),
      toolbar_(allocator),
      status_bar_(allocator),
      back_(allocator),
      forward_(allocator),
      reload_(allocator),
      go_(allocator),
      address_(allocator),
      status_(allocator),
      page_(allocator),
      own_local_(allocator),
      own_hub_(allocator),
      env_(*this, allocator),
      pip_window_(allocator),
      defer_go_(allocator),
      defer_type_(allocator),
      defer_body_(allocator),
      request_type_(allocator),
      request_body_(allocator),
      script_log_(allocator),
      history_(allocator),
      own_jar_(allocator),
      current_(allocator),
      request_target_(allocator),
      status_text_(allocator),
      worker_(allocator),
      prefetcher_(allocator, &WebBrowser::prefetch_one, this) {
  set_bounds({100, 50, 680, 480});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);
  load_handler_ = {&WebBrowser::on_load, this};
  pip_handler_ = {&WebBrowser::on_pip_changed, this};
  pip_window_.on_closed(gui::Handler::immediate(&WebBrowser::route_pip, this));

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
    third_party_item_ = view->add_item("Block Third-Party Cookies", {&WebBrowser::on_toggle_third_party, this});
    view->add_separator();
    view->add_item("Clear Cookies and Site Data", {&WebBrowser::on_clear_site_data, this});
    if (scripts_item_) scripts_item_->set_checked(scripts_enabled_);
  }
  if (Menu* go = menu_.add_menu("Go")) {
    go->add_item("Back", {&WebBrowser::on_back, this}, "Alt+Left");
    go->add_item("Forward", {&WebBrowser::on_forward, this}, "Alt+Right");
    go->add_item("Reload", {&WebBrowser::on_reload, this}, "F5");
  }

  leave_dialog_.on_result({&WebBrowser::on_leave_answer, this});
  for (uint32_t i = 0; i < 2 * kHops; ++i) {
    hops_[i].owner = this;
    gui::ContextMenu& menu = i < kHops ? back_menu_ : forward_menu_;
    hop_items_[i] = menu.add_item("", {&WebBrowser::on_history_hop, &hops_[i]});
    if (hop_items_[i]) hop_items_[i]->set_enabled(false);
  }
  back_menu_.on_opening({&WebBrowser::on_history_opening, this});
  forward_menu_.on_opening({&WebBrowser::on_history_opening, this});

  toolbar_.set_style(PanelStyle::Transparent);
  toolbar_.set_dock(DockStyle::Top);
  toolbar_.set_size(0, 28);
  toolbar_.set_padding(Insets::all(1));
  back_.set_text("<");
  back_.set_dock(DockStyle::Left);
  back_.set_size(28, 0);
  back_.on_click({&WebBrowser::on_back, this});
  back_.set_context_menu(&back_menu_);
  forward_.set_text(">");
  forward_.set_dock(DockStyle::Left);
  forward_.set_size(28, 0);
  forward_.on_click({&WebBrowser::on_forward, this});
  forward_.set_context_menu(&forward_menu_);
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
  pointer_handler_ = {&WebBrowser::on_pointer, this};
  resize_handler_ = {&WebBrowser::on_view_resized, this};
  scroll_handler_ = {&WebBrowser::on_view_scrolled, this};
  timer_handler_ = {&WebBrowser::on_timers, this};
  visibility_handler_ = {&WebBrowser::on_visibility, this};
  key_handler_ = {&WebBrowser::on_form_key, this};
  page_.on_node_clicked(Handler::immediate(&WebBrowser::route_node, this));
  page_.on_pointer_changed(Handler::immediate(&WebBrowser::route_pointer, this));
  page_.on_key_pressed(Handler::immediate(&WebBrowser::route_key, this));
  page_.on_resized(Handler::immediate(&WebBrowser::route_resize, this));
  page_.on_scrolled(Handler::immediate(&WebBrowser::route_scroll, this));

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

bool WebBrowser::RtcRandom::fill(ByteView out) { return browser::system_random(out); }

browser::IRtcService* WebBrowser::rtc() {
  if (!rtc_service_ && loader_)
    if (rtc::IRtcNetwork* network = loader_->rtc_network()) {
      rtc_service_ = UniquePointer<rtc::RtcService>::make(allocator(), allocator(), *network, rtc_random_);
      if (rtc_service_) rtc_service_->set_devices(rtc_audio_);
    }
  return rtc_service_.get();
}

WebBrowser::~WebBrowser() {
  worker_.stop();   // (the page's thread ends first: it must not touch the controls below while they are taken down)
  pip_window_.dismiss();
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
                {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"mkv", "video/x-matroska"}, {"weba", "audio/webm"}, {"opus", "audio/opus"}};
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
    b->pending_referrer_.assign(b->current_.view());
    if (!b->navigate(target.view())) b->pending_referrer_.clear();
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

bool WebBrowser::go_back() {
  if (!can_go_back()) return false;
  request_delta_ = -1;
  return post(Request::Back, StringView());
}

bool WebBrowser::go_forward() {
  if (!can_go_forward()) return false;
  request_delta_ = 1;
  return post(Request::Forward, StringView());
}

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
  queue(load_handler_, *this, true);
  return true;
}

bool WebBrowser::post(Request kind, StringView target) {
  {
    UiGuard lock(this);
    request_ = kind;
    request_target_.assign(target);
  }
  queue(load_handler_, *this, true);
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
  bool in_place = false;
  {
    UiGuard lock(this);
    switch (kind) {
      case Request::Go: address.assign(target); break;
      case Request::Back:
      case Request::Forward: {
        if (!history_.can_go(request_delta_)) return;
        const browser::SessionHistory::Entry& entry = history_.at(static_cast<uint32_t>(history_.index() + request_delta_));
        if (live_ && entry.document != 0 && entry.document == document_) {   // an entry of the document that is shown: the page is not loaded again
          in_place = true;
          break;
        }
        address.assign(entry.address.view());
        break;
      }
      case Request::Reload: address.assign(current_.empty() ? target : current_.view()); break;
      case Request::Submit: address.assign(target); break;
      default: return;
    }
    if (!in_place) loading_ = true;
  }
  if (in_place) {
    traverse_in_place(request_delta_);
    return;
  }
  if (live_ && manager() && !leave_confirmed_ && live_->before_unload()) {   // the page wants the user to say they want to leave
    {
      UiGuard lock(this);
      loading_ = false;
    }
    ask_leave(kind, target);
    return;
  }
  leave_confirmed_ = false;
  leave_current();
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
    const uint64_t hash = address.view().find('#');   // (the fragment of the address asked for stays, as redirects keep it, unless they name one)
    if (hash != StringView::npos && page.address.view().find('#') == StringView::npos) page.address.append(address.view().substr(hash));
    remember(kind, page.address.view());
    loading_ = false;
    if (kind == Request::Go || kind == Request::Submit) referrer_.assign(pending_referrer_.view());
    else if (kind != Request::Reload) referrer_.clear();
    pending_referrer_.clear();
    const browser::SessionHistory::Entry* entry = history_.current();
    restore_scroll_ = (kind == Request::Back || kind == Request::Forward || kind == Request::Reload) && entry && entry->scroll_saved && !entry->manual_scroll ? entry->scroll : -1;
  }
  show(page);
}

bool WebBrowser::fetch(StringView address, Fetched* page, uint64_t limit, bool subresource) {
  if (browser::is_file_address(address)) return fetch_file(address, page, limit);
  if (!loader_) {
    page->error.assign("Web pages cannot be opened: there is no network connection.");
    return false;
  }
  browser::FetchOptions options;
  options.url = address;
  options.mode = subresource ? browser::FetchOptions::Mode::Subresource : browser::FetchOptions::Mode::Navigate;
  options.block_third_party_cookies = block_third_party_;
  options.credentials = browser::FetchOptions::Credentials::Include;
  options.headers = "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.1\r\n";
  options.now_ms = page_clock_ms();
  if (subresource) options.timeout_ms = kSubresourceTimeoutMs;
  browser::NetResponse result(allocator());
  // a service worker in charge of the address answers a page being opened first
  const browser::ServiceWorkerHub::Intercept verdict =
      !subresource && sw_hub_ ? sw_hub_->intercept(0, options, "document", true, &result) : browser::ServiceWorkerHub::Intercept::Pass;
  const bool fetched = verdict == browser::ServiceWorkerHub::Intercept::Responded ||
                       (verdict == browser::ServiceWorkerHub::Intercept::Pass && browser::fetch_resource(allocator(), *loader_, jar(), options, &result));
  if (!fetched) {
    String message(allocator());
    message.append("Cannot open ");
    message.append(address.size() > 24 ? address.first(24) : address);   // (the notice dialog has room for four short lines)
    if (address.size() > 24) message.append("...");
    message.append(": ");
    message.append(result.error.empty() ? StringView("the page could not be fetched.") : result.error.view());
    page->error.assign(message.view());
    return false;
  }
  if (!result.url.empty()) page->address.assign(result.url.view());
  StringView type;
  browser::header_value(result.headers.view(), "Content-Type", &type);
  page->type.assign(type);
  page->status = result.status;
  if (!page->body.append(result.body.view())) {
    page->error.assign("Out of memory.");
    return false;
  }
  return true;
}

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
  browser::FetchOptions options;
  String headers(allocator());
  headers.append("Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.1\r\nContent-Type: ");
  headers.append(type.view());
  headers.append("\r\n");
  options.method = "POST";
  options.url = address;
  options.headers = headers.view();
  options.body = body.view().bytes();
  options.mode = browser::FetchOptions::Mode::Navigate;
  options.credentials = browser::FetchOptions::Credentials::Include;
  options.now_ms = page_clock_ms();
  browser::NetResponse result(allocator());
  const browser::ServiceWorkerHub::Intercept verdict = sw_hub_ ? sw_hub_->intercept(0, options, "document", true, &result) : browser::ServiceWorkerHub::Intercept::Pass;
  const bool sent = verdict == browser::ServiceWorkerHub::Intercept::Responded ||
                    (verdict == browser::ServiceWorkerHub::Intercept::Pass && browser::fetch_resource(allocator(), *loader_, jar(), options, &result));
  if (!sent) {
    String message(allocator());
    message.append("Cannot send the form: ");
    message.append(result.error.empty() ? StringView("the server did not answer.") : result.error.view());
    page->error.assign(message.view());
    return false;
  }
  if (!result.url.empty()) page->address.assign(result.url.view());
  StringView result_type;
  browser::header_value(result.headers.view(), "Content-Type", &result_type);
  page->type.assign(result_type);
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
  ++document_;   // (a new document is on its way: the entries that belonged to the one it replaces answer to it from now on)
  switch (kind) {
    case Request::Back:
    case Request::Forward: {
      history_.go(request_delta_);
      const uint32_t arrived = history_.current() ? history_.current()->document : 0;   // (the document of the entry we came to is loaded again: its entries follow it)
      history_.replace_current(address, document_);
      if (arrived) history_.renumber_document(arrived, document_);
      break;
    }
    default: {
      const bool same = history_.current() && history_.current()->address.view().equals(address);
      if (kind == Request::Reload && history_.current()) {
        const uint32_t previous = history_.current()->document;
        history_.replace_current(address, document_);
        if (previous) history_.renumber_document(previous, document_);
      } else if (replace_next_ && history_.current()) {   // (a refresh without a delay: Back must not come back to the page that sent it on)
        history_.replace_navigation(address, document_);
      } else if (!same) {
        history_.navigate(address, document_);
      } else {
        history_.replace_navigation(address, document_);   // (going to the address that is shown replaces its entry)
      }
      replace_next_ = false;
      break;
    }
  }
  current_.assign(address);
}

// Back or Forward to an entry that belongs to the document on the screen (pushState() and fragment entries): the address changes, the page is told, nothing is fetched.
bool WebBrowser::traverse_in_place(int32_t delta) {
  String address(allocator()), state(allocator());
  bool has_state;
  const bool manual = live_ && live_->scroll_restoration_manual();
  {
    UiGuard lock(this);
    if (!live_ || !history_.can_go(delta)) return false;
    browser::SessionHistory::Entry& leaving = *history_.current();   // (where the page was scrolled to, for when it comes back)
    leaving.scroll = page_.scroll_y();
    leaving.scroll_saved = true;
    leaving.manual_scroll = manual;
    history_.go(delta);
    const browser::SessionHistory::Entry& entry = *history_.current();
    restore_scroll_ = entry.scroll_saved && !entry.manual_scroll ? entry.scroll : -1;
    address.assign(entry.address.view());
    state.assign(entry.state.view());
    has_state = entry.has_state;
    current_.assign(address.view());
    address_.set_text(current_.view());
    update_buttons();
  }
  live_->traverse(address.view(), state.view(), has_state);
  settle();
  return true;
}

font::FontSet& WebBrowser::layout_fonts() {
  if (!layout_fonts_) {
    layout_fonts_ = UniquePointer<font::FontSet>::make(allocator(), allocator());
    layout_fonts_->add_bundled_fonts();
  }
  return *layout_fonts_;
}

// A plain text file shown as a page: its text in a <pre>.
static void text_as_html(StringView text, String* out) {
  out->append("<!doctype html><meta charset=utf-8><body style='margin:8px'><pre style='margin:0;white-space:pre-wrap;overflow-wrap:break-word;font-size:13px'>");
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '<') out->append("&lt;");
    else if (c == '>') out->append("&gt;");
    else if (c == '&') out->append("&amp;");
    else out->push_back(c);
  }
  out->append("</pre>");
}

// Shows a fetched page: as HTML, as text, or a note that it cannot be shown.
void WebBrowser::show(Fetched& page) {
  const StringView body(page.body.view());
  UniquePointer<browser::PagePicture> picture = UniquePointer<browser::PagePicture>::make(allocator(), allocator());
  if (!picture) {
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  {
    UiGuard lock(&page_);
    page_.set_object_painter(nullptr);
  }
  prefetcher_.cancel_all();   // (what the old page was getting is of no use now)
  if (live_) live_->unload();   // (pagehide, unload)
  live_.reset();   // (the old page and its scripts go before the new one comes)
  __atomic_store_n(&refresh_armed_, false, __ATOMIC_RELEASE);
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
  // Text and files that cannot be shown are pages too: a <pre> of the text, a line of explanation
  String synthetic(allocator());
  StringView html = body;
  bool scripts = scripts_enabled_;
  if (!is_html(page.type.view(), page.address.view(), body)) {
    scripts = false;
    if (is_text(page.type.view(), body)) {
      text_as_html(body, &synthetic);
    } else {
      String note(allocator());
      note.append("This kind of file (");
      note.append(page.type.empty() ? StringView("unknown type") : page.type.view());
      note.append(") cannot be shown.");
      text_as_html(note.view(), &synthetic);
    }
    html = synthetic.view();
  }
  bool ok = true;
  media_ = current_media();
  apply_fullscreen(false);   // (a page that was fullscreen is gone)
  pip_window_.dismiss();     // (and a video that was in its own window)
  defer_fullscreen_ = 0;
  live_ = UniquePointer<browser::Page>::make(allocator(), allocator(), env_, page.address.view(), media_, scripts);
  if (live_) live_->set_visible(__atomic_load_n(&page_shown_, __ATOMIC_ACQUIRE));
  if (!live_) ok = false;
  else {
    if (history_.current() && history_.current()->has_state) live_->set_history_state(history_.current()->state.view(), true);
    String referrer(allocator());
    visible_referrer(allocator(), referrer_.view(), page.address.view(), &referrer);
    live_->set_referrer(referrer.view());
    if (scripts) say("Reading the page and running its scripts...");
    else say("Reading the page...");
    ok = live_->load(html) && live_->render_picture(picture.get(), layout_fonts());
    if (ok) page_.set_object_painter(&live_->objects());
    double seconds = 0;
    String target(allocator());
    if (ok && scripts_enabled_ && live_->meta_refresh(&seconds, &target) && seconds < 1e6) {   // <meta http-equiv=refresh>
      refresh_target_.assign(target.view());
      refresh_due_ms_ = __atomic_load_n(&uptime_ms_, __ATOMIC_ACQUIRE) + static_cast<uint64_t>(seconds * 1000);
      refresh_replace_ = seconds <= 1;
      __atomic_store_n(&refresh_armed_, true, __ATOMIC_RELEASE);
    }
  }
  if (!ok) {
    live_.reset();
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  present(util::move(picture), false, page.status);
  settle();
}

// Puts a drawn page on the screen, with its title and the state of its scripts in the status bar.
void WebBrowser::present(UniquePointer<browser::PagePicture> picture, bool keep_scroll, uint32_t http_status) {
  String page_title(allocator());
  if (live_) page_title.assign(live_->page_title());
  const bool cut = false;
  page_.set_picture(util::move(picture), keep_scroll);
  int32_t scroll = defer_scroll_;
  defer_scroll_ = -1;
  if (live_) {
    const int32_t asked = live_->take_scroll_request();
    if (asked >= 0) scroll = asked;
  }
  if (restore_scroll_ >= 0) {   // Back / Forward / Reload: where the page was left wins over what the page asks for
    scroll = restore_scroll_;
    restore_scroll_ = -1;
  }
  if (scroll >= 0) page_.scroll_to_y(scroll);
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
  UniquePointer<browser::PagePicture> picture = UniquePointer<browser::PagePicture>::make(allocator(), allocator());
  if (!picture || !live_->render_picture(picture.get(), layout_fonts())) {
    notify(NoticeKind::Error, "Out of memory.");
    return;
  }
  present(util::move(picture), keep_scroll, 0);
}

// After the page ran something (a click, a timer, a new window size): do what it asked of the browser, then draw it again if it changed.
void WebBrowser::settle() {
  if (defer_opens_.count() > 0) {   // window.open()
    List<String> opens(allocator());
    for (uint64_t i = 0; i < defer_opens_.count(); ++i) {
      String copy(allocator());
      copy.assign(defer_opens_[i].view());
      opens.push_back(util::move(copy));
    }
    defer_opens_.clear();
    for (uint64_t i = 0; i < opens.count(); ++i)
      if (open_fn_) open_fn_(open_ctx_, *this, opens[i].view());
  }
  if (want_close_) {   // window.close() of a window a script opened
    want_close_ = false;
    if (opened_by_script_) {
      if (live_) live_->unload();
      UiGuard lock(this);
      close();
      return;
    }
  }
  if (want_post_) {
    String target(allocator()), type(allocator()), body(allocator());
    target.assign(defer_go_.view());
    type.assign(defer_type_.view());
    body.assign(defer_body_.view());
    want_post_ = false;
    pending_referrer_.assign(current_.view());
    post_form(target.view(), type.view(), body.view());
    return;
  }
  if (want_go_) {
    String target(allocator());
    target.assign(defer_go_.view());
    want_go_ = false;
    pending_referrer_.assign(current_.view());
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
    if (!history_.can_go(delta)) return;
    request_delta_ = delta;
    post(delta < 0 ? Request::Back : Request::Forward, StringView());
    return;
  }
  if (defer_fullscreen_ != 0) {
    const int8_t what = defer_fullscreen_;
    defer_fullscreen_ = 0;
    apply_fullscreen(what == 1);
    if (what == 3 && live_) live_->fullscreen_left();
  }
  if (!live_) return;
  canvas_accelerated_ = live_->objects().accelerated();
  if (live_->take_visual_change()) {   // (a canvas was drawn on, a video has a new picture: only the view needs painting again)
    UiGuard lock(&page_);
    page_.invalidate();
  }
  update_key_capture();
  const uint64_t now_ms = __atomic_load_n(&uptime_ms_, __ATOMIC_ACQUIRE);
  const bool shown = __atomic_load_n(&page_shown_, __ATOMIC_ACQUIRE);
  const bool must = defer_scroll_ >= 0 || restore_scroll_ >= 0;
  if (live_->changed() || must) {
    if (!must && (!shown || (in_timers_ && now_ms < __atomic_load_n(&last_refresh_ms_, __ATOMIC_ACQUIRE) + kMinRefreshGapMs))) {   // (a timer made it too soon, or nobody looks: tick comes back for it; a click or key is drawn at once)
      __atomic_store_n(&refresh_deferred_, true, __ATOMIC_RELEASE);
    } else {
      __atomic_store_n(&refresh_deferred_, false, __ATOMIC_RELEASE);
      __atomic_store_n(&last_refresh_ms_, now_ms, __ATOMIC_RELEASE);
      refresh(true);
    }
  } else if (live_->errors() != reported_errors_) {
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
    if (args.usage == key::kEscape && b->fullscreen_) {   // (Escape leaves fullscreen first, wherever the focus is; the page does not get the key)
      b->apply_fullscreen(false);
      if (b->live_) b->live_->fullscreen_left();
      b->settle();
      continue;
    }
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
  if (page_.viewport_width() > 1) {   // (the view has been laid out: the page gets its size in CSS pixels)
    m.width = page_.viewport_width();
    m.height = page_.viewport_height();
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

// Is the page on show: its tab is the selected one of its window (or it has a window of its own) and that window is not minimized?
bool WebBrowser::window_shown() {
  gui::Frame* window = host() ? host() : this;
  if (window->state() == gui::Frame::State::Minimized) return false;
  const gui::TabFrame* tabs = window->as_tabs();
  return !host() || !tabs || tabs->selected() == this;
}

void WebBrowser::on_visibility(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (b->live_) b->live_->set_visible(__atomic_load_n(&b->page_shown_, __ATOMIC_ACQUIRE));
  b->settle();
}

void WebBrowser::tick(uint64_t uptime_us) {
  __atomic_store_n(&uptime_ms_, uptime_us / 1000, __ATOMIC_RELEASE);
  if (manager()) {   // (a window that is not open yet counts as shown)
    const bool shown = window_shown();
    if (shown != __atomic_load_n(&page_shown_, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&page_shown_, shown, __ATOMIC_RELEASE);
      queue(visibility_handler_, *this, true);
    }
  }
  if (pip_window_.showing()) {   // the picture moves on; a window that was resized is noticed
    pip_window_.refresh();
    if (pip_window_.picture_width() != pip_w_ || pip_window_.picture_height() != pip_h_) queue(pip_handler_, *this, true);
  }
  if (__atomic_load_n(&refresh_armed_, __ATOMIC_ACQUIRE) && uptime_us / 1000 >= refresh_due_ms_ && __atomic_exchange_n(&refresh_armed_, false, __ATOMIC_ACQ_REL)) {
    replace_next_ = refresh_replace_;   // <meta http-equiv=refresh> comes due: the page is opened again (or the address it names)
    pending_referrer_.assign(current_.view());
    post(refresh_target_.view().equals(current_.view()) ? Request::Reload : Request::Go, refresh_target_.view());
  }
  const bool shown_now = __atomic_load_n(&page_shown_, __ATOMIC_ACQUIRE);
  const uint64_t ms = uptime_us / 1000;
  const uint64_t due = __atomic_load_n(&next_due_ms_, __ATOMIC_ACQUIRE);
  if (__atomic_load_n(&refresh_deferred_, __ATOMIC_ACQUIRE) && shown_now && ms >= __atomic_load_n(&last_refresh_ms_, __ATOMIC_ACQUIRE) + kMinRefreshGapMs && !__atomic_exchange_n(&timer_pending_, true, __ATOMIC_ACQ_REL)) {
    queue(timer_handler_, *this, true);   // (a redraw was put off: the handler's settle() does it now)
    return;
  }
  if (due != 0 && !shown_now) {   // a page that is not shown: its timers run once a second
    if (ms < last_hidden_run_ms_ + kHiddenTimerGapMs) return;
    last_hidden_run_ms_ = ms;
  }
  if (due == 0 || page_clock_ms() < due || __atomic_exchange_n(&timer_pending_, true, __ATOMIC_ACQ_REL)) return;
  queue(timer_handler_, *this, true);
}

void WebBrowser::on_timers(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  __atomic_store_n(&b->timer_pending_, false, __ATOMIC_RELEASE);
  if (!b->live_) return;
  const double now = static_cast<double>(b->page_clock_ms());
  b->live_->run_due_timers(now);
  b->live_->step_objects(now);
  b->in_timers_ = true;
  b->settle();
  b->in_timers_ = false;
}

void WebBrowser::on_pointer(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (!b->live_) return;
  b->live_->set_pointer(b->page_.pointer_node(), b->page_.pointer_pressed());
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

void WebBrowser::on_view_scrolled(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (!b->live_) return;
  int32_t y;
  {
    UiGuard lock(&b->page_);
    y = b->page_.scroll_y();
  }
  b->live_->set_scroll(y);
  b->settle();
}

void WebBrowser::update_media() {
  if (defer_fullscreen_ != 0) settle();
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
  queue(resize_handler_, *this, true);
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

bool WebBrowser::Env::fetch_bytes(StringView address, uint64_t limit, ByteBuffer* out) { return owner_.fetch_subresource(address, limit, out); }

// The answer for a subresource: a download that was started ahead of time is used (and waited for, up to the time limit); anything else is fetched here.
bool WebBrowser::fetch_subresource(StringView address, uint64_t limit, ByteBuffer* body) {
  ByteBuffer got(allocator());
  switch (prefetcher_.take(address, kSubresourceTimeoutMs + 2000, &got)) {
    case browser::ResourcePrefetcher::Result::Hit: break;
    case browser::ResourcePrefetcher::Result::Failed: return false;
    case browser::ResourcePrefetcher::Result::Miss: {
      Fetched f(allocator());
      f.address.assign(address);
      if (!fetch(address, &f, kMaxFileBytes, true) || f.status >= 400) return false;
      if (!got.append(f.body.view())) return false;
      break;
    }
  }
  return got.size() <= limit && body->append(got.view());
}

// Runs on a download thread: fetches like fetch_subresource() would, without the prefetcher.
bool WebBrowser::prefetch_one(void* self, StringView address, ByteBuffer* body) {
  auto* b = static_cast<WebBrowser*>(self);
  Fetched f(b->allocator());
  f.address.assign(address);
  if (!b->fetch(address, &f, kMaxFileBytes, true) || f.status >= 400) return false;
  return body->append(f.body.view());
}

void WebBrowser::Env::prefetch(StringView address) {
  if (!owner_.loader_ || !(address.starts_with("http://") || address.starts_with("https://"))) return;
  owner_.page_clock_ms();   // (sets the clock up here, on this thread, before download threads read it)
  owner_.prefetcher_.request(address);
}

namespace {
uint64_t next_tab_id() {
  static uint64_t counter = 0;
  return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
}
}  // namespace

WebBrowser::Env::Env(WebBrowser& owner, allocators::IAllocator& allocator) : owner_(owner), session_(allocator), tab_(next_tab_id()) {}

browser::StorageHub& WebBrowser::Env::local_storage_hub() { return owner_.shared_local_ ? *owner_.shared_local_ : owner_.own_local_; }
browser::PageHub& WebBrowser::Env::page_hub() { return owner_.shared_hub_ ? *owner_.shared_hub_ : owner_.own_hub_; }

browser::CookieJar& WebBrowser::Env::cookies() { return owner_.jar(); }

bool WebBrowser::Env::http_request(const browser::FetchOptions& options, browser::NetResponse* response) {
  if (owner_.sw_hub_ && owner_.sw_hub_->push_handles(options.url)) {   // (the push service answers for itself)
    owner_.sw_hub_->push_request(options, response);
    return true;
  }
  if (!owner_.loader_) {
    response->error.assign("There is no network connection.");
    return false;
  }
  browser::FetchOptions mine = options;
  mine.block_third_party_cookies = owner_.block_third_party_;
  return browser::fetch_resource(owner_.allocator(), *owner_.loader_, owner_.jar(), mine, response);   // (may run on a worker thread)
}

void WebBrowser::Env::http_answered(const browser::NetResponse& response) { owner_.apply_clear_site_data(response); }

// A Clear-Site-Data header asked for the storage of the site that answered to go (the cookies were dealt with where the answer was read).
void WebBrowser::apply_clear_site_data(const browser::NetResponse& response) {
  if (!(response.clear_site_data & 2) || response.url.empty()) return;
  browser::WebUrl url(allocator());
  if (!browser::WebUrl::parse(allocator(), response.url.view(), nullptr, &url)) return;
  String origin(allocator());
  url.origin(&origin);
  // "storage": localStorage and sessionStorage, the caches of the Cache API and the service workers of the origin
  (shared_local_ ? *shared_local_ : own_local_).area(origin.view()).clear();
  env_.session_storage_hub().area(origin.view()).clear();
  if (sw_hub_) sw_hub_->clear_origin(origin.view());
  if (shared_caches_) shared_caches_->clear_origin(origin.view());
}

void WebBrowser::set_block_third_party_cookies(bool block) {
  block_third_party_ = block;
  if (third_party_item_) third_party_item_->set_checked(block);
}

void WebBrowser::clear_site_data() {
  jar().clear();
  (shared_local_ ? *shared_local_ : own_local_).clear_all();
  env_.session_storage_hub().clear_all();
  if (sw_hub_) sw_hub_->clear_all();
  if (shared_caches_) shared_caches_->clear_all();
}

void WebBrowser::on_toggle_third_party(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  b->set_block_third_party_cookies(!b->block_third_party_);
}

void WebBrowser::on_clear_site_data(void* self, Control&) { static_cast<WebBrowser*>(self)->clear_site_data(); }

UniquePointer<browser::IWebSocket> WebBrowser::Env::open_websocket(StringView url, StringView protocols, StringView origin, String* error) {
  if (!owner_.loader_) {
    error->assign("There is no network connection.");
    return UniquePointer<browser::IWebSocket>();
  }
  return owner_.loader_->open_websocket(url, protocols, origin, error);
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
  ByteBuffer body(owner_.allocator());
  if (!owner_.fetch_subresource(address, limit, &body)) return false;
  return text->assign(StringView(body.view()));
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


// The text of some html: the tags dropped, the common entities undone, blocks and <br> as line breaks (for a page that gave the clipboard html only).
static void html_to_text(StringView html, String* out) {
  out->clear();
  bool in_tag = false;
  for (uint64_t i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (in_tag) {
      if (c == '>') in_tag = false;
      continue;
    }
    if (c == '<') {
      const StringView rest = html.substr(i + 1);
      if (rest.starts_with("br") || rest.starts_with("/p") || rest.starts_with("/div") || rest.starts_with("/li") || rest.starts_with("/h")) {
        if (!out->empty() && out->view()[out->size() - 1] != '\n') out->push_back('\n');
      }
      in_tag = true;
    } else if (c == '&') {
      const StringView rest = html.substr(i);
      struct Entity { const char* name; char value; };
      static const Entity kEntities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
      bool done = false;
      for (const Entity& e : kEntities) {
        if (rest.starts_with(e.name)) {
          out->push_back(e.value);
          i += StringView(e.name).size() - 1;
          done = true;
          break;
        }
      }
      if (!done) out->push_back(c);
    } else {
      out->push_back(c);
    }
  }
}

void WebBrowser::Env::clipboard_formats(String* csv) {
  csv->clear();
  gui::Window* w = owner_.window();
  kernel::Clipboard* clip = w ? w->clipboard() : nullptr;
  if (!clip) return;
  if (clip->format() == kernel::ClipboardFormat::Text) {
    csv->assign("text/plain");
    if (owner_.rich_valid_ && clip->sequence() == owner_.rich_sequence_ && !owner_.rich_html_.empty()) csv->append(",text/html");
  } else if (clip->format() == kernel::ClipboardFormat::Image) {
    csv->assign("image/png");
  }
}

bool WebBrowser::Env::clipboard_read_format(StringView format, String* bytes) {
  gui::Window* w = owner_.window();
  kernel::Clipboard* clip = w ? w->clipboard() : nullptr;
  if (!clip) return false;
  if (format.equals("text/plain")) return clip->get_text(bytes);
  if (format.equals("text/html")) return clip->format() == kernel::ClipboardFormat::Text && owner_.rich_valid_ && clip->sequence() == owner_.rich_sequence_ && !owner_.rich_html_.empty() && bytes->assign(owner_.rich_html_.view());
  if (format.equals("image/png")) {
    uint32_t width = 0, height = 0;
    if (!clip->image_size(&width, &height)) return false;
    media::Image image(owner_.allocator());
    if (!image.create(width, height)) return false;
    ArrayView<uint32_t> pixels = image.frame(0);
    if (!clip->get_image(pixels.data(), pixels.size())) return false;
    ByteBuffer png(owner_.allocator());
    if (!media::encode_png(image, 6, &png)) return false;
    return bytes->assign(StringView(png.view()));
  }
  return false;
}

bool WebBrowser::Env::clipboard_write_items(const browser::ClipboardData& data) {
  gui::Window* w = owner_.window();
  kernel::Clipboard* clip = w ? w->clipboard() : nullptr;
  if (!clip) return false;
  bool ok = true;
  String derived(owner_.allocator());
  StringView text = data.text;
  if (data.has_image && !data.has_text && !data.has_html) {
    media::Image image(owner_.allocator());
    if (!media::decode_png(data.image_png.bytes(), &image) || image.empty()) return false;
    ArrayView<uint32_t> pixels = image.frame(0);
    ok = clip->set_image(pixels.data(), image.width(), image.height());
  } else if (data.has_text || data.has_html) {
    if (!data.has_text) {
      html_to_text(data.html, &derived);
      text = derived.view();
    }
    if (text.empty()) clip->clear();
    else ok = clip->set_text(text);
  } else {
    clip->clear();
  }
  owner_.rich_html_.assign(data.has_html ? data.html : StringView());
  owner_.rich_sequence_ = clip->sequence();
  owner_.rich_valid_ = ok;
  return ok;
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
uint32_t WebBrowser::Env::history_length() { return owner_.history_.count(); }

void WebBrowser::Env::push_state(StringView address, StringView state_json, bool has_state, bool replace) {
  UiGuard lock(&owner_);
  owner_.history_.push_state(address, state_json, has_state, replace);
  owner_.current_.assign(address);
  owner_.address_.set_text(owner_.current_.view());
  owner_.update_buttons();
}

void WebBrowser::Env::alert(StringView text) { owner_.notify(NoticeKind::Information, text); }
void WebBrowser::Env::scroll_to(int32_t line) { owner_.defer_scroll_ = line; }
bool WebBrowser::Env::open_pip(uint32_t video_serial, uint32_t width, uint32_t height, browser::PageObjects& objects) {
  gui::WindowManager* m = owner_.manager();
  if (!m) return false;
  owner_.pip_w_ = width;
  owner_.pip_h_ = height;
  return owner_.pip_window_.open(*m, &objects, video_serial, width, height);
}
void WebBrowser::Env::close_pip() { owner_.pip_window_.dismiss(); }

// The window of the video was closed, or its size changed: the page is told.
void WebBrowser::on_pip_changed(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (!b->live_ || b->live_->objects().pip() == 0) return;
  if (!b->pip_window_.showing()) {
    b->live_->pip_left();
  } else {
    const uint32_t w = b->pip_window_.picture_width(), h = b->pip_window_.picture_height();
    if (w != b->pip_w_ || h != b->pip_h_) {
      b->pip_w_ = w;
      b->pip_h_ = h;
      b->live_->pip_resized(w, h);
    }
  }
  b->settle();
}

bool WebBrowser::Env::set_fullscreen(bool on) {
  owner_.defer_fullscreen_ = on ? 1 : 2;
  return true;
}

void WebBrowser::apply_fullscreen(bool on) {
  if (on == fullscreen_) return;
  fullscreen_ = on;
  menu_.set_visible(!on);
  toolbar_.set_visible(!on);
  status_bar_.set_visible(!on);
  set_fullscreen(on);   // (the window: no title bar, border or tab strip, covering the desktop's area)
  UiGuard lock(this);
  invalidate();
}
uint64_t WebBrowser::Env::now_ms() { return owner_.page_clock_ms(); }

// Following a link to "#something" on the page shown: the address changes, the history entry too; "#" and "#top" scroll to the top.
void WebBrowser::apply_hash(StringView hash) {
  UiGuard lock(this);
  String address(allocator());
  const uint64_t at = current_.view().find('#');
  address.assign(at == StringView::npos ? current_.view() : current_.view().first(at));
  if (hash.size() > 1) address.append(hash);
  if (!address.view().equals(current_.view())) {   // (a new entry of the same document; the same fragment again changes nothing)
    history_.push_fragment(address.view());
    current_.assign(address.view());
  }
  address_.set_text(current_.view());
  update_buttons();
  if (hash.size() <= 1 || hash.equals_ignore_case("#top")) defer_scroll_ = 0;
}

// --- Keys -------------------------------------------------------------------------------------------

bool WebBrowser::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  const bool control = args.modifiers & hardware::modifier::kControl;
  const bool alt = args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt);
  if (args.usage == key::kEscape && fullscreen_) {   // the user leaves fullscreen; the page hears of it
    defer_fullscreen_ = 3;
    queue(resize_handler_, *this, true);
    return true;
  }
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


// --- Leaving a page, and the lists under Back and Forward ----------------------------------------------------------------------------

// What document.referrer says: the page that led here, as the default policy (strict-origin-when-cross-origin) lets it be seen - the whole address (without the
// fragment) within one origin, only the origin to another, nothing when going from https to http.
static void visible_referrer(allocators::IAllocator& allocator, StringView from, StringView to, String* out) {
  out->clear();
  if (from.empty()) return;
  browser::WebUrl a(allocator), b(allocator);
  if (!browser::WebUrl::parse(allocator, from, nullptr, &a) || !browser::WebUrl::parse(allocator, to, nullptr, &b)) return;
  if (!a.scheme.equals("http") && !a.scheme.equals("https")) return;
  if (a.scheme.equals("https") && !b.scheme.equals("https")) return;
  if (a.same_origin(b)) {
    a.serialize(out, false);
    return;
  }
  a.origin(out);
  out->push_back('/');
}

void WebBrowser::leave_current() {
  const bool manual = live_ && live_->scroll_restoration_manual();
  String title(allocator());
  if (live_) title.assign(live_->page_title());
  UiGuard lock(this);
  browser::SessionHistory::Entry* e = history_.current();
  if (!e) return;
  e->scroll = page_.scroll_y();
  e->scroll_saved = true;
  e->manual_scroll = manual;
  e->title.assign(title.view());
}

void WebBrowser::ask_leave(Request kind, StringView target) {
  UiGuard lock(this);
  leave_kind_ = kind;
  leave_target_.assign(target);
  if (!manager() || leave_dialog_.showing()) return;
  leave_dialog_.set_texts("Leave page?", "This page asks whether you want to leave it.", "Changes you made may not be saved.", "Leave", "Stay");
  leave_dialog_.show(*manager());
}

void WebBrowser::on_leave_answer(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  if (!b->leave_dialog_.accepted()) {
    UiGuard lock(b);
    b->address_.set_text(b->current_.view());
    return;
  }
  {
    UiGuard lock(b);
    b->leave_confirmed_ = true;
    b->request_ = b->leave_kind_;
    b->request_target_.assign(b->leave_target_.view());
  }
  b->queue(b->load_handler_, *b, true);
}

// The titles (or addresses) of the entries Back (direction -1) or Forward (+1) can go to, nearest first.
uint32_t WebBrowser::history_titles(int32_t direction, uint32_t limit, List<String>* out) {
  UiGuard lock(this);
  uint32_t n = 0;
  for (int32_t step = 1; n < limit && history_.can_go(direction * step); ++step) {
    const browser::SessionHistory::Entry& e = history_.at(static_cast<uint32_t>(history_.index() + direction * step));
    String text(allocator());
    text.assign(e.title.empty() ? e.address.view() : e.title.view());
    if (!out->push_back(util::move(text))) break;
    ++n;
  }
  return n;
}

void WebBrowser::on_history_opening(void* self, Control&) {
  auto* b = static_cast<WebBrowser*>(self);
  for (int32_t direction = -1; direction <= 1; direction += 2) {
    List<String> titles(b->allocator());
    b->history_titles(direction, kHops, &titles);
    const uint32_t base = direction < 0 ? 0 : kHops;
    for (uint32_t i = 0; i < kHops; ++i) {
      gui::MenuItem* item = b->hop_items_[base + i];
      if (!item) continue;
      const bool used = i < titles.count();
      {
        UiGuard lock(b);
        item->text.assign(used ? titles[i].view() : StringView());
        b->hops_[base + i].delta = direction * static_cast<int32_t>(i + 1);
      }
      item->set_enabled(used);
    }
  }
}

void WebBrowser::on_history_hop(void* context, Control&) {
  Hop* hop = static_cast<Hop*>(context);
  WebBrowser* b = hop->owner;
  if (!b->history_.can_go(hop->delta)) return;
  b->request_delta_ = hop->delta;
  b->post(hop->delta < 0 ? Request::Back : Request::Forward, StringView());
}

// --- The session history as the Navigation API sees it ----------------------------------------------------------------------------

static void append_json_string(String* out, StringView text) {
  out->push_back('"');
  for (const char c : text) {
    if (c == '"') out->append("\\\"");
    else if (c == '\\') out->append("\\\\");
    else if (static_cast<uint8_t>(c) < 0x20) {
      static const char digits[] = "0123456789abcdef";
      out->append("\\u00");
      out->push_back(digits[(static_cast<uint8_t>(c) >> 4) & 15]);
      out->push_back(digits[static_cast<uint8_t>(c) & 15]);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

bool WebBrowser::Env::history_entries(String* json, int64_t* index) {
  UiGuard lock(&owner_);
  const browser::SessionHistory& h = owner_.history_;
  if (h.empty()) return false;
  json->push_back('[');
  for (uint32_t i = 0; i < h.count(); ++i) {
    const browser::SessionHistory::Entry& e = h.at(i);
    if (i) json->push_back(',');
    json->append("{\"key\":");
    json->append_uint(e.key);
    json->append(",\"id\":");
    json->append_uint(e.id);
    json->append(",\"doc\":");
    json->append_uint(e.document);
    json->append(",\"url\":");
    append_json_string(json, e.address.view());
    json->append(",\"nav\":");
    if (e.has_nav_state) append_json_string(json, e.nav_state.view());
    else json->append("null");
    json->push_back('}');
  }
  json->push_back(']');
  *index = h.index();
  return true;
}

void WebBrowser::Env::set_navigation_state(StringView json, bool has_state) {
  UiGuard lock(&owner_);
  browser::SessionHistory::Entry* e = owner_.history_.current();
  if (!e) return;
  e->nav_state.assign(json);
  e->has_nav_state = has_state;
}

void WebBrowser::Env::history_go_to_key(uint64_t key) {
  const int64_t at = owner_.history_.find_key(key);
  if (at >= 0) owner_.defer_history_ += static_cast<int32_t>(at - owner_.history_.index());
}

bool WebBrowser::Env::open_window(StringView address, StringView, bool) {
  if (!owner_.open_fn_ || !(browser::is_web_address(address) || browser::is_file_address(address)) || owner_.defer_opens_.count() >= 8) return false;
  String copy(owner_.allocator());
  copy.assign(address);
  return owner_.defer_opens_.push_back(util::move(copy));
}

void WebBrowser::Env::close_window() { owner_.want_close_ = true; }

}  // namespace desktop
