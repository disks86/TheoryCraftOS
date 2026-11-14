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

#include "Desktop/AppManager.hpp"
#include "Desktop/Format.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;

namespace {

// `to` gets the items of `from` (lists cannot be assigned).
template <class T>
void move_list(List<T>& to, List<T>& from) {
  to.clear();
  for (T& item : from.view())
    if (!to.push_back(util::move(item))) break;
  from.clear();
}

constexpr int32_t kGap = 6, kRowHeight = 24, kLabelHeight = 22, kButtonHeight = 26, kMenuHeight = 22;

Rect2D rect(int32_t x, int32_t y, int32_t width, int32_t height) {
  return {x, y, static_cast<uint32_t>(width > 0 ? width : 0), static_cast<uint32_t>(height > 0 ? height : 0)};
}

// "\"name\" could not be installed because <why>." - one whole sentence per job, with what to try where that is known.
const char* failure_pattern(bool install, bool update, appman::Result result) {
  const bool network = result == appman::Result::NoScript || result == appman::Result::NoDownload || result == appman::Result::DownloadFailed;
  if (install) return network ? "\"%1\" could not be installed because %2. Check the network connection and try again." : "\"%1\" could not be installed because %2.";
  if (update) return network ? "\"%1\" could not be updated because %2. Check the network connection and try again." : "\"%1\" could not be updated because %2.";
  return "\"%1\" could not be removed because %2.";
}

// "12.3 MB"
bool append_megabytes(String* out, uint64_t bytes) {
  const uint64_t tenths = bytes / 104858;      // (1 MiB / 10, rounded)
  return out->append_uint(tenths / 10) && out->push_back('.') && out->append_uint(tenths % 10) && out->append(" MB");
}

}  // namespace

AppManager::AppManager(allocators::IAllocator& allocator)
    : Frame(allocator), allocator_(allocator), menu_(allocator), list_menu_(allocator), ask_(allocator), remove_name_(allocator), search_label_(allocator), show_label_(allocator), details_(allocator), status_(allocator), search_(allocator),
      show_(allocator), refresh_(allocator), install_(allocator), update_(allocator), remove_(allocator), cancel_(allocator), list_(allocator), progress_(allocator),
      catalog_(allocator), installed_(allocator), rows_(allocator), query_(allocator), job_name_(allocator), worker_(allocator) {
  set_title("App Manager");
  set_bounds({120, 50, 760, 540});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  // The menu bar: every command of the window (the buttons are the quick way to the common ones).
  if (Menu* file = menu_.add_menu("File")) file->add_item("Exit", {&AppManager::on_exit, this});
  if (Menu* view = menu_.add_menu("View")) {
    installed_only_item_ = view->add_item("Installed Only", {&AppManager::on_installed_only, this});
    view->add_separator();
    refresh_item_ = view->add_item("Refresh List", {&AppManager::on_refresh, this}, "F5");
  }
  if (Menu* app = menu_.add_menu("Application")) {
    install_item_ = app->add_item("Install", {&AppManager::on_install, this});
    update_item_ = app->add_item("Update", {&AppManager::on_update, this});
    remove_item_ = app->add_item("Remove", {&AppManager::on_remove, this}, "Del");
    app->add_separator();
    cancel_item_ = app->add_item("Cancel", {&AppManager::on_cancel, this});
  }
  if (Menu* help = menu_.add_menu("Help")) about_item_ = help->add_item("About App Manager", {&AppManager::on_about, this});
  // The commands of an application, on the application.
  list_install_item_ = list_menu_.add_item("Install", {&AppManager::on_install, this});
  list_update_item_ = list_menu_.add_item("Update", {&AppManager::on_update, this});
  list_remove_item_ = list_menu_.add_item("Remove", {&AppManager::on_remove, this});
  list_.set_context_menu(&list_menu_);
  ask_.on_result({&AppManager::on_remove_answer, this});

  search_label_.set_text("Search:");      // (a colon: the label carries the access key of the box after it)
  show_label_.set_text("Show:");
  show_.add_item("All applications");
  show_.add_item("Installed");
  show_.set_selected_index(kShowAll);
  refresh_.set_text("Refresh List");
  install_.set_text("Install");
  update_.set_text("Update");
  remove_.set_text("Remove");
  cancel_.set_text("Cancel");
  search_.set_max_length(200);
  list_.add_column("Application", 170);
  list_.add_column("Description", 420);
  list_.add_column("Status", 100);
  list_.set_view_mode(ViewMode::Details);
  list_.set_copy_enabled(false);
  progress_.set_maximum(100);

  search_.on_changed({&AppManager::on_search, this});
  show_.on_selection_changed({&AppManager::on_show, this});
  list_.on_selection_changed({&AppManager::on_pick, this});
  list_.on_item_activated({&AppManager::on_activate, this});
  install_.on_click({&AppManager::on_install, this});
  update_.on_click({&AppManager::on_update, this});
  remove_.on_click({&AppManager::on_remove, this});
  cancel_.on_click({&AppManager::on_cancel, this});
  refresh_.on_click({&AppManager::on_refresh, this});

  // (in reading order: that is the order Tab takes)
  Control* const parts[] = {&menu_, &search_label_, &search_, &show_label_, &show_, &refresh_, &list_, &details_, &install_, &update_, &remove_, &cancel_, &progress_, &status_};
  for (Control* part : parts) add(part);
  say("Applications for Linux from the AppMan list. The list is fetched when the network is available.");
  update_controls();
}

AppManager::~AppManager() {
  __atomic_store_n(&progress_state_.cancel, true, __ATOMIC_RELAXED);      // (a download in progress stops, and the worker ends)
  worker_.stop();
}

void AppManager::set_services(filesystem::IFileSystem* files, appman::INetwork* network) {
  {
    UiGuard lock(this);
    files_ = files;
    network_ = network;
    installer_ = files && network ? UniquePointer<appman::Installer>::make(allocator_, allocator_, *files, *network) : UniquePointer<appman::Installer>();
  }
  reload_installed();
  if (!load_cached_catalog() && network) refresh_catalog();
  rebuild();
}

uint64_t AppManager::catalog_count() const {
  kernel::ScopedMutex lock(state_mutex_);
  return catalog_.count();
}

bool AppManager::row_name(int32_t row, String* out) const {
  kernel::ScopedMutex lock(state_mutex_);
  if (row < 0 || uint64_t(row) >= rows_.count()) return false;
  return out->assign(rows_.view()[uint64_t(row)].view());
}

bool AppManager::is_installed(StringView name) const {
  for (const appman::InstalledApp& app : installed_.view())
    if (app.name.view().equals(name)) return true;
  return false;
}

bool AppManager::load_cached_catalog() {
  if (!files_) return false;
  UniquePointer<filesystem::IFile> file;
  if (files_->open_file(filesystem::layout::kAppManCatalog, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > appman::kMaxCatalogBytes) return false;
  ByteBuffer bytes(allocator_);
  if (!bytes.put_zeros(size)) return false;
  uint64_t done = 0;
  while (done < size) {
    uint64_t got = 0;
    if (file->read(done, bytes.writable_view().subview(done), &got) != filesystem::Status::Ok || got == 0) return false;
    done += got;
  }
  appman::Catalog fresh(allocator_);
  if (!fresh.parse(StringView(bytes.view()))) return false;
  kernel::ScopedMutex lock(state_mutex_);
  return catalog_.take(fresh);
}

void AppManager::reload_installed() {
  List<appman::InstalledApp> found(allocator_);
  if (installer_ && !installer_->list_installed(&found)) return;
  kernel::ScopedMutex lock(state_mutex_);
  move_list(installed_, found);
}

namespace {
// A row of the list, gathered under the state lock and shown after it (a control's handlers may run at once and take the lock themselves).
struct Row {
  explicit Row(allocators::IAllocator& allocator) : name(allocator), description(allocator) {}
  String name, description;
  bool installed = false;
};
}  // namespace

void AppManager::rebuild() {
  UiGuard lock(this);
  List<String> names(allocator_);
  List<Row> rows(allocator_);
  uint64_t total = 0;
  const bool only_installed = show_.selected_index() == kShowInstalled;
  {
    kernel::ScopedMutex state(state_mutex_);
    auto add_row = [&](StringView name, StringView description) {
      Row row(allocator_);
      String copy(allocator_);
      if (!row.name.assign(name) || !row.description.assign(description) || !copy.assign(name)) return false;
      row.installed = is_installed(name);
      return rows.push_back(util::move(row)) && names.push_back(util::move(copy));
    };
    if (only_installed) {
      for (const appman::InstalledApp& app : installed_.view()) {
        const appman::CatalogEntry* entry = catalog_.find(app.name.view());
        const StringView description = entry ? entry->description.view() : StringView("(not in the list any more)");
        bool matches = true;
        StringView words = query_.view(), word;
        while (matches && words.next_token(' ', &word)) {
          if (word.empty()) continue;
          matches = app.name.view().contains(word) || description.contains(word);
        }
        if (!matches) continue;
        ++total;
        if (!add_row(app.name.view(), description)) break;
      }
    } else {
      List<uint64_t> found(allocator_);
      if (catalog_.search(query_.view(), kMaxRows, &found, &total)) {
        for (const uint64_t i : found.view())
          if (!add_row(catalog_.at(i).name.view(), catalog_.at(i).description.view())) break;
      }
    }
    move_list(rows_, names);
  }
  list_.clear_items();
  for (const Row& row : rows.view()) {
    const StringView cells[3] = {row.name.view(), row.description.view(), row.installed ? StringView("Installed") : StringView()};
    list_.add_item(cells, 3, ItemIcon::File);
  }
  if (!busy()) {
    String text(allocator_), total_text(allocator_), count_text(allocator_), shown_text(allocator_);
    const uint64_t shown = list_.item_count();
    const uint64_t count = catalog_count();
    total_text.append_uint(total);
    count_text.append_uint(count);
    shown_text.append_uint(shown);
    if (show_.selected_index() == kShowInstalled) {
      format_message(&text, total == 1 ? "%1 application installed" : "%1 applications installed", total_text.view());
    } else if (count == 0) {
      text.assign(network_ ? "There is no list of applications yet. Choose Refresh List to fetch it." : "The list of applications cannot be fetched because there is no network.");
    } else if (shown < total) {
      format_message(&text, "%1 of %2 applications; the first %3 are shown. Type in Search to narrow the list.", total_text.view(), count_text.view(), shown_text.view());
    } else {
      format_message(&text, "%1 of %2 applications", total_text.view(), count_text.view());
    }
    say(text.view());
  }
  update_controls();
}

void AppManager::update_controls() {
  UiGuard lock(this);
  String name(allocator_), text(allocator_);
  const bool have = row_name(list_.selected_item(), &name);
  bool installed = false;
  {
    kernel::ScopedMutex state(state_mutex_);
    installed = have && is_installed(name.view());
    if (have) {
      const appman::CatalogEntry* entry = catalog_.find(name.view());
      if (!entry) text.assign(name.view());
      else format_message(&text, entry->command_line ? "%1: %2 (a program for the terminal)" : "%1: %2", name.view(), entry->description.view());
      for (const appman::InstalledApp& app : installed_.view())
        if (app.name.view().equals(name.view())) {
          String with(allocator_);
          format_message(&with, "%1  -  installed from %2", text.view(), app.version.view());
          text.assign(with.view());
        }
    }
  }
  details_.set_text(text.view());
  const bool ready = installer_ && !busy();
  install_.set_enabled(ready && have && !installed);
  update_.set_enabled(ready && have && installed);
  remove_.set_enabled(ready && have && installed);
  refresh_.set_enabled(network_ && !busy());
  cancel_.set_enabled(busy());
  // The menus follow the buttons: one state, wherever the command is chosen.
  if (install_item_) install_item_->set_enabled(install_.enabled());
  if (update_item_) update_item_->set_enabled(update_.enabled());
  if (remove_item_) remove_item_->set_enabled(remove_.enabled());
  if (cancel_item_) cancel_item_->set_enabled(cancel_.enabled());
  if (refresh_item_) refresh_item_->set_enabled(refresh_.enabled());
  if (list_install_item_) list_install_item_->set_enabled(install_.enabled());
  if (list_update_item_) list_update_item_->set_enabled(update_.enabled());
  if (list_remove_item_) list_remove_item_->set_enabled(remove_.enabled());
  if (installed_only_item_) installed_only_item_->set_checked(show_.selected_index() == kShowInstalled);
  if (about_item_) about_item_->set_enabled(notices_ != nullptr);
}

// The message goes to the status line, and - while this is the window the user is working in - into a message box as well, so a failure is not
// only in a line at the bottom. A window in the background never puts a message on top of the user's work: the line is there when they come back.
void AppManager::report(NoticeKind kind, StringView text) {
  say(text);
  if (active()) NoticeDialog::post(notices_, manager(), kind, text);
}

void AppManager::set_query(StringView text) {
  {
    kernel::ScopedMutex lock(state_mutex_);
    query_.assign(text);
  }
  rebuild();
}

void AppManager::set_filter(Filter filter) {
  show_.set_selected_index(filter);
  rebuild();
}

void AppManager::select_row(int32_t row) {
  list_.select_item(row);
  update_controls();
}

// ---------------------------------------------------------------- jobs
bool AppManager::start(Job job, StringView name) {
  if (!network_ || (job != Job::Catalog && !installer_)) {
    say("Applications cannot be installed because the network or the disk is not available.");
    return false;
  }
  if (__atomic_exchange_n(&busy_, true, __ATOMIC_ACQ_REL)) {
    say("Wait until the work in progress has finished, or cancel it.");
    return false;
  }
  {
    kernel::ScopedMutex lock(state_mutex_);
    job_ = job;
    job_name_.assign(name);
  }
  __atomic_store_n(&progress_state_.cancel, false, __ATOMIC_RELAXED);
  progress_state_.changed = &AppManager::on_progress;
  progress_state_.context = this;
  progress_.set_value(0);
  update_controls();
  String text(allocator_);
  switch (job) {
    case Job::Catalog: text.assign("Fetching the list of applications..."); break;
    case Job::Install: format_message(&text, "Installing \"%1\"...", name); break;
    case Job::Update: format_message(&text, "Looking for a newer \"%1\"...", name); break;
    case Job::Remove: format_message(&text, "Removing \"%1\"...", name); break;
    case Job::None: break;
  }
  say(text.view());
  worker_.post({&AppManager::run_job, this}, *this, false);
  return true;
}

bool AppManager::refresh_catalog() { return start(Job::Catalog, StringView()); }

bool AppManager::install_selected() {
  String name(allocator_);
  if (!row_name(list_.selected_item(), &name)) {
    say("Select an application in the list first.");
    return false;
  }
  return start(Job::Install, name.view());
}

bool AppManager::update_selected() {
  String name(allocator_);
  if (!row_name(list_.selected_item(), &name)) {
    say("Select an application in the list first.");
    return false;
  }
  return start(Job::Update, name.view());
}

bool AppManager::remove_selected() {
  String name(allocator_);
  if (!row_name(list_.selected_item(), &name)) {
    say("Select an application in the list first.");
    return false;
  }
  return start(Job::Remove, name.view());
}

// Removing deletes the application's files, and nothing here brings them back, so the question comes first; the answer that leaves everything as it
// was is the default one. Without a window manager (nowhere to ask) it removes at once.
bool AppManager::ask_remove() {
  if (!remove_.enabled()) return false;
  if (!manager()) return remove_selected();
  if (!row_name(list_.selected_item(), &remove_name_)) return false;
  String line(allocator_);
  const StringView name = remove_name_.view();
  format_message(&line, "Remove \"%1\"?", name.size() > 30 ? name.first(30) : name);
  ask_.set_texts(title(), line.view(), "Its files are deleted from this computer.", "Remove", "Cancel");
  ask_.set_default_choice(MessageDialog::Choice::Decline);
  return ask_.show(*manager());
}

void AppManager::on_remove_answer(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  if (m->ask_.accepted() && !m->remove_name_.empty()) m->start(Job::Remove, m->remove_name_.view());
}

void AppManager::cancel() {
  if (busy()) {
    __atomic_store_n(&progress_state_.cancel, true, __ATOMIC_RELAXED);
    say("Cancelling...");
  }
}

void AppManager::on_progress(void* self, const appman::Progress& progress) {
  auto* m = static_cast<AppManager*>(self);
  const uint64_t done = __atomic_load_n(&progress.done, __ATOMIC_RELAXED), total = __atomic_load_n(&progress.total, __ATOMIC_RELAXED);
  String text(m->allocator_), name(m->allocator_);
  {
    kernel::ScopedMutex lock(m->state_mutex_);
    name.assign(m->job_name_.view());
  }
  String done_text(m->allocator_), total_text(m->allocator_);
  append_megabytes(&done_text, done);
  if (total) {
    append_megabytes(&total_text, total);
    format_message(&text, "Downloading \"%1\": %2 of %3", name.view(), done_text.view(), total_text.view());
    m->progress_.set_value(done * 100 / total);
  } else {
    format_message(&text, "Downloading \"%1\": %2", name.view(), done_text.view());
  }
  m->say(text.view());
}

// On the worker thread: the job, then what the window shows after it.
void AppManager::run_job(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  Job job;
  String name(m->allocator_), detail(m->allocator_), text(m->allocator_);
  {
    kernel::ScopedMutex lock(m->state_mutex_);
    job = m->job_;
    name.assign(m->job_name_.view());
  }
  appman::Result result = appman::Result::Ok;
  NoticeKind kind = NoticeKind::Information;
  bool tell = false;      // a failure: said in a message, not only in the status line
  if (job == Job::Catalog) {
    String list(m->allocator_);
    appman::Catalog fresh(m->allocator_);
    if (!m->network_->fetch(appman::kCatalogUrl, appman::kMaxCatalogBytes, &list) || !fresh.parse(list.view())) {
      text.assign("The list of applications could not be fetched. Check the network connection, then choose Refresh List.");
      kind = NoticeKind::Error;
      tell = true;
    } else {
      String count(m->allocator_);
      count.append_uint(fresh.count());
      format_message(&text, "The list has %1 applications.", count.view());
      if (m->files_) {      // kept for next time (best effort)
        (void)filesystem::layout::create_app_folder(*m->files_, "AppMan", m->allocator_);
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        if (m->files_->open_file(filesystem::layout::kAppManCatalog, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok)
          (void)file->write(0, list.view().bytes(), &written);
      }
      kernel::ScopedMutex lock(m->state_mutex_);
      if (!m->catalog_.take(fresh)) {
        text.assign("The list of applications could not be kept because there is not enough memory. Close other programs and try again.");
        kind = NoticeKind::Error;
        tell = true;
      }
    }
  } else {
    switch (job) {
      case Job::Install: result = m->installer_->install(name.view(), &m->progress_state_, &detail); break;
      case Job::Update: result = m->installer_->update(name.view(), &m->progress_state_, &detail); break;
      case Job::Remove: result = m->installer_->remove(name.view(), &detail); break;
      default: break;
    }
    if (result == appman::Result::Ok) {
      format_message(&text, job == Job::Remove ? "\"%1\" was removed." : job == Job::Update ? "\"%1\" was updated. The Start menu starts the new version." : "\"%1\" is installed. It is in the Start menu.",
                     name.view());
    } else if (result == appman::Result::UpToDate) {
      format_message(&text, "\"%1\" is up to date.", name.view());
    } else if (result == appman::Result::Cancelled) {      // (what the user asked for: not a failure)
      format_message(&text, job == Job::Update ? "Updating \"%1\" was cancelled." : "Installing \"%1\" was cancelled. Nothing was installed.", name.view());
    } else {
      format_message(&text, failure_pattern(job == Job::Install, job == Job::Update, result), name.view(), appman::result_name(result));
      if (!detail.empty()) {      // (the technical detail goes at the end, after the sentence)
        String with(m->allocator_);
        format_message(&with, "%1 (%2)", text.view(), detail.view());
        text.assign(with.view());
      }
      kind = NoticeKind::Error;
      tell = true;
    }
  }
  m->progress_.set_value(result == appman::Result::Ok && job != Job::Remove && job != Job::Catalog ? 100 : 0);
  m->reload_installed();
  {
    kernel::ScopedMutex lock(m->state_mutex_);
    m->job_ = Job::None;
  }
  __atomic_store_n(&m->busy_, false, __ATOMIC_RELEASE);
  String keep(m->allocator_);
  const bool had = m->row_name(m->list_.selected_item(), &keep);
  m->rebuild();
  if (had) {      // the same application stays chosen (found under the state lock, chosen after it: the UI lock always comes first)
    int32_t row = -1;
    {
      kernel::ScopedMutex lock(m->state_mutex_);
      for (uint64_t i = 0; i < m->rows_.count() && row < 0; ++i)
        if (m->rows_.view()[i].view().equals(keep.view())) row = static_cast<int32_t>(i);
    }
    if (row >= 0) m->list_.select_item(row);
  }
  m->update_controls();
  if (tell) m->report(kind, text.view());
  else m->say(text.view());
}

// ---------------------------------------------------------------- controls
void AppManager::on_search(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  m->set_query(m->search_.text());
}
void AppManager::on_show(void* self, Control&) { static_cast<AppManager*>(self)->rebuild(); }
void AppManager::on_exit(void* self, Control&) { static_cast<AppManager*>(self)->close(); }
void AppManager::on_installed_only(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  m->set_filter(m->show_.selected_index() == kShowInstalled ? kShowAll : kShowInstalled);
}
void AppManager::on_about(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  NoticeDialog::post(m->notices_, m->manager(), NoticeKind::Information,
                     "App Manager installs, updates and removes applications for Linux from the AppMan list (github.com/ivan-hc/AM).", "About App Manager");
}

// F5: Refresh List. Del: Remove (the selected application; a text box takes Del for itself first).
bool AppManager::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kF1 + 4) {
    if (refresh_.enabled()) refresh_catalog();
    return true;
  }
  if (args.usage == hardware::key::kDelete && list_.focused()) {
    ask_remove();
    return true;
  }
  return Frame::on_key(args);
}
void AppManager::on_pick(void* self, Control&) { static_cast<AppManager*>(self)->update_controls(); }
void AppManager::on_activate(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  if (m->install_.enabled()) m->install_selected();
}
void AppManager::on_install(void* self, Control&) { static_cast<AppManager*>(self)->install_selected(); }
void AppManager::on_update(void* self, Control&) { static_cast<AppManager*>(self)->update_selected(); }
void AppManager::on_remove(void* self, Control&) { static_cast<AppManager*>(self)->ask_remove(); }
void AppManager::on_cancel(void* self, Control&) { static_cast<AppManager*>(self)->cancel(); }
void AppManager::on_refresh(void* self, Control&) { static_cast<AppManager*>(self)->refresh_catalog(); }

void AppManager::layout() {
  Frame::layout();
  const Rect2D area = client_rect();
  const int32_t left = area.x, top = area.y, width = static_cast<int32_t>(area.width), height = static_cast<int32_t>(area.height);
  int32_t y = top;
  place(menu_, rect(left, y, width, kMenuHeight));
  menu_.layout();
  y += kMenuHeight + kGap;
  // Search: [..........]  Show: [list]  [Refresh List]
  const int32_t search_label = 64, show_label = 48, show_list = 150, refresh = 116;
  const int32_t search = width - search_label - show_label - show_list - refresh - 3 * kGap;
  int32_t x = left;
  place(search_label_, rect(x, y, search_label, kRowHeight));
  x += search_label;
  place(search_, rect(x, y, search, kRowHeight));
  x += (search > 0 ? search : 0) + kGap;
  place(show_label_, rect(x, y, show_label, kRowHeight));
  x += show_label;
  place(show_, rect(x, y, show_list, kRowHeight));
  x += show_list + kGap;
  place(refresh_, rect(x, y, refresh, kRowHeight));
  y += kRowHeight + kGap;
  const int32_t below = kLabelHeight + kGap + kButtonHeight + kGap + kLabelHeight + kGap;
  const int32_t list_height = height - (y - top) - below;      // (the buttons below are as wide as each other)
  place(list_, rect(left, y, width, list_height > 80 ? list_height : 80));
  y += (list_height > 80 ? list_height : 80) + kGap;
  place(details_, rect(left, y, width, kLabelHeight));
  y += kLabelHeight + kGap;
  place(install_, rect(left, y, 80, kButtonHeight));
  place(update_, rect(left + 86, y, 80, kButtonHeight));
  place(remove_, rect(left + 172, y, 80, kButtonHeight));
  place(cancel_, rect(left + 258, y, 80, kButtonHeight));
  place(progress_, rect(left + 350, y + 4, width - 350, kButtonHeight - 8));
  y += kButtonHeight + kGap;
  place(status_, rect(left, y, width, kLabelHeight));
}

}  // namespace desktop
