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
  const bool network = result == appman::Result::NoScript || result == appman::Result::NoDownload || result == appman::Result::DownloadFailed ||
                       result == appman::Result::NoManifest;      // (what the network may be the reason of)
  if (install) return network ? "\"%1\" could not be installed because %2. Check the network connection and try again." : "\"%1\" could not be installed because %2.";
  if (update) return network ? "\"%1\" could not be updated because %2. Check the network connection and try again." : "\"%1\" could not be updated because %2.";
  return "\"%1\" could not be removed because %2.";
}

// "12.3 MB"
bool append_megabytes(String* out, uint64_t bytes) {
  const uint64_t tenths = bytes / 104858;      // (1 MiB / 10, rounded)
  return out->append_uint(tenths / 10) && out->push_back('.') && out->append_uint(tenths % 10) && out->append(" MB");
}

// The whole file at `path` (at most `limit` bytes). False if it is not there, is empty or larger, or cannot be read.
bool read_whole(filesystem::IFileSystem& files, StringView path, uint64_t limit, ByteBuffer* bytes) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > limit || !bytes->put_zeros(size)) return false;
  uint64_t done = 0;
  while (done < size) {
    uint64_t got = 0;
    if (file->read(done, bytes->writable_view().subview(done), &got) != filesystem::Status::Ok || got == 0) return false;
    done += got;
  }
  return true;
}

// Keeps `text` in the file at `path` of the App Manager's folder for next time (best effort).
void keep_file(filesystem::IFileSystem& files, allocators::IAllocator& allocator, const char* path, StringView text) {
  (void)filesystem::layout::create_app_folder(files, "AppMan", allocator);
  UniquePointer<filesystem::IFile> file;
  uint64_t written = 0;
  if (files.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok) (void)file->write(0, text.bytes(), &written);
}

}  // namespace

AppManager::AppManager(allocators::IAllocator& allocator)
    : Frame(allocator), allocator_(allocator), menu_(allocator), list_menu_(allocator), ask_(allocator), remove_name_(allocator), search_label_(allocator), show_label_(allocator), details_(allocator), status_(allocator), search_(allocator),
      show_(allocator), refresh_(allocator), install_(allocator), update_(allocator), remove_(allocator), cancel_(allocator), list_(allocator), progress_(allocator),
      catalog_(allocator), windows_catalog_(allocator), installed_(allocator), windows_installed_(allocator), rows_(allocator), query_(allocator), job_name_(allocator), worker_(allocator) {
  set_title("App Manager");
  set_bounds({120, 50, 760, 540});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);

  // The menu bar: every command of the window (the buttons are the quick way to the common ones).
  if (Menu* file = menu_.add_menu("File")) file->add_item("Exit", {&AppManager::on_exit, this});
  if (Menu* view = menu_.add_menu("View")) {
    installed_only_item_ = view->add_item("Installed Only", {&AppManager::on_installed_only, this});
    linux_only_item_ = view->add_item("For Linux Only", {&AppManager::on_linux_only, this});
    windows_only_item_ = view->add_item("For Windows Only", {&AppManager::on_windows_only, this});
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
  ask_.on_result({&AppManager::on_answer, this});

  search_label_.set_text("Search:");      // (a colon: the label carries the access key of the box after it)
  show_label_.set_text("Show:");
  show_.add_item("All applications");
  show_.add_item("Installed");
  show_.add_item("For Linux");
  show_.add_item("For Windows");
  show_.set_selected_index(kShowAll);
  refresh_.set_text("Refresh List");
  install_.set_text("Install");
  update_.set_text("Update");
  remove_.set_text("Remove");
  cancel_.set_text("Cancel");
  search_.set_max_length(200);
  list_.add_column("Application", 170);
  list_.add_column("Type", 70);
  list_.add_column("Description", 340);
  list_.add_column("Status", 110);
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
  say("Applications for Linux (the AppMan list) and for Windows (the winget list). The lists are fetched when the network is available.");
  update_controls();
}

AppManager::~AppManager() {
  __atomic_store_n(&progress_state_.cancel, true, __ATOMIC_RELAXED);      // (a download in progress stops, and the worker ends)
  worker_.stop();
}

void AppManager::set_services(filesystem::IFileSystem* files, appman::INetwork* network, appman::ISetupHost* setup) {
  {
    UiGuard lock(this);
    files_ = files;
    network_ = network;
    installer_ = files && network ? UniquePointer<appman::Installer>::make(allocator_, allocator_, *files, *network) : UniquePointer<appman::Installer>();
    windows_installer_ = files && network ? UniquePointer<appman::WingetInstaller>::make(allocator_, allocator_, *files, *network, setup) : UniquePointer<appman::WingetInstaller>();
  }
  reload_installed();
  if (!load_cached_catalog() && network) refresh_catalog();
  rebuild();
}

uint64_t AppManager::catalog_count() const {
  kernel::ScopedMutex lock(state_mutex_);
  return catalog_.count() + windows_catalog_.count();
}

uint64_t AppManager::linux_count() const {
  kernel::ScopedMutex lock(state_mutex_);
  return catalog_.count();
}

uint64_t AppManager::windows_count() const {
  kernel::ScopedMutex lock(state_mutex_);
  return windows_catalog_.count();
}

bool AppManager::row_name(int32_t row, String* out) const {
  kernel::ScopedMutex lock(state_mutex_);
  if (row < 0 || uint64_t(row) >= rows_.count()) return false;
  return out->assign(rows_.view()[uint64_t(row)].id.view());
}

bool AppManager::row_platform(int32_t row, AppPlatform* out) const {
  kernel::ScopedMutex lock(state_mutex_);
  if (row < 0 || uint64_t(row) >= rows_.count()) return false;
  *out = rows_.view()[uint64_t(row)].platform;
  return true;
}

bool AppManager::is_installed(AppPlatform platform, StringView name) const {
  if (platform == AppPlatform::Windows) {
    for (const appman::WingetInstalled& app : windows_installed_.view())
      if (appman::compare_folded(app.id.view(), name) == 0) return true;
    return false;
  }
  for (const appman::InstalledApp& app : installed_.view())
    if (app.name.view().equals(name)) return true;
  return false;
}

// The lists kept from last time. False unless both are there (the one that is missing is then fetched).
bool AppManager::load_cached_catalog() {
  if (!files_) return false;
  bool have_linux = false, have_windows = false;
  {
    ByteBuffer bytes(allocator_);
    appman::Catalog fresh(allocator_);
    if (read_whole(*files_, filesystem::layout::kAppManCatalog, appman::kMaxCatalogBytes, &bytes) && fresh.parse(StringView(bytes.view()))) {
      kernel::ScopedMutex lock(state_mutex_);
      have_linux = catalog_.take(fresh);
    }
  }
  {
    ByteBuffer bytes(allocator_);
    appman::WingetCatalog fresh(allocator_);
    if (read_whole(*files_, filesystem::layout::kWingetCatalog, appman::kMaxWingetListBytes, &bytes) && fresh.parse_list(StringView(bytes.view()))) {
      kernel::ScopedMutex lock(state_mutex_);
      have_windows = windows_catalog_.take(fresh);
    }
  }
  return have_linux && have_windows;
}

void AppManager::reload_installed() {
  List<appman::InstalledApp> found(allocator_);
  List<appman::WingetInstalled> windows_found(allocator_);
  const bool have = !installer_ || installer_->list_installed(&found);
  const bool have_windows = !windows_installer_ || windows_installer_->list_installed(&windows_found);
  kernel::ScopedMutex lock(state_mutex_);
  if (have) move_list(installed_, found);
  if (have_windows) move_list(windows_installed_, windows_found);
}

namespace {
// A row of the list, gathered under the state lock and shown after it (a control's handlers may run at once and take the lock themselves).
struct Row {
  explicit Row(allocators::IAllocator& allocator) : id(allocator), name(allocator), description(allocator) {}
  Row(Row&&) = default;
  Row& operator=(Row&&) = default;
  AppPlatform platform = AppPlatform::Linux;
  String id, name, description;
  StringView status;      // (a literal)
};
}  // namespace

void AppManager::rebuild() {
  UiGuard lock(this);
  List<RowKey> keys(allocator_);
  List<Row> rows(allocator_);
  uint64_t total = 0, count = 0;
  const int32_t filter = show_.selected_index();
  const bool only_installed = filter == kShowInstalled, want_linux = filter != kShowWindows, want_windows = filter != kShowLinux;
  {
    kernel::ScopedMutex state(state_mutex_);
    count = (want_linux ? catalog_.count() : 0) + (want_windows ? windows_catalog_.count() : 0);
    auto add_row = [&](AppPlatform platform, StringView id, StringView name, StringView description, StringView status) {
      Row row(allocator_);
      RowKey key(allocator_);
      row.platform = key.platform = platform;
      row.status = status;
      if (!row.id.assign(id) || !row.name.assign(name) || !row.description.assign(description) || !key.id.assign(id)) return false;
      return rows.push_back(util::move(row)) && keys.push_back(util::move(key));
    };
    auto add_linux = [&](const appman::CatalogEntry& entry) {
      return add_row(AppPlatform::Linux, entry.name.view(), entry.name.view(), entry.description.view(), is_installed(AppPlatform::Linux, entry.name.view()) ? "Installed" : "");
    };
    // "jqlang.jq, version 1.8.2": winget's list has no descriptions, so the row says which package it is.
    auto add_windows = [&](const appman::WingetPackage& package) {
      String description(allocator_);
      format_message(&description, "%1, version %2", package.id.view(), package.version.view());
      StringView status;
      for (const appman::WingetInstalled& app : windows_installed_.view())
        if (appman::compare_folded(app.id.view(), package.id.view()) == 0) status = app.version.view().equals(package.version.view()) ? "Installed" : "Update available";
      return add_row(AppPlatform::Windows, package.id.view(), package.name.view(), description.view(), status);
    };
    auto matches = [&](StringView name, StringView more) {
      StringView words = query_.view(), word;
      while (words.next_token(' ', &word))
        if (!word.empty() && !appman::contains_folded(name, word) && !appman::contains_folded(more, word)) return false;
      return true;
    };
    if (only_installed) {
      // Both kinds by name, as the two lists are.
      uint64_t l = 0, w = 0;
      const ArrayView<const appman::InstalledApp> linux_apps = static_cast<const List<appman::InstalledApp>&>(installed_).view();
      const ArrayView<const appman::WingetInstalled> windows_apps = static_cast<const List<appman::WingetInstalled>&>(windows_installed_).view();
      while (l < linux_apps.size() || w < windows_apps.size()) {
        const bool take_linux = w >= windows_apps.size() || (l < linux_apps.size() && appman::compare_folded(linux_apps[l].name.view(), windows_apps[w].name.view()) <= 0);
        if (take_linux) {
          const appman::InstalledApp& app = linux_apps[l++];
          const appman::CatalogEntry* entry = catalog_.find(app.name.view());
          const StringView description = entry ? entry->description.view() : StringView("(not in the list any more)");
          if (!matches(app.name.view(), description)) continue;
          ++total;
          if (!add_row(AppPlatform::Linux, app.name.view(), app.name.view(), description, "Installed")) break;
        } else {
          const appman::WingetInstalled& app = windows_apps[w++];
          const appman::WingetPackage* package = windows_catalog_.find(app.id.view());
          String description(allocator_);
          format_message(&description, "%1, version %2", app.id.view(), app.version.view());
          if (!matches(app.name.view(), description.view())) continue;
          ++total;
          if (!add_row(AppPlatform::Windows, app.id.view(), app.name.view(), description.view(), package && !package->version.view().equals(app.version.view()) ? "Update available" : "Installed"))
            break;
        }
      }
    } else {
      // The best matches of each catalog, merged: better matches first, by name among equals.
      List<uint64_t> linux_found(allocator_), windows_found(allocator_);
      List<uint8_t> linux_tiers(allocator_), windows_tiers(allocator_);
      uint64_t linux_total = 0, windows_total = 0;
      const bool ok = (!want_linux || catalog_.search(query_.view(), kMaxRows, &linux_found, &linux_total, &linux_tiers)) &&
                      (!want_windows || windows_catalog_.search(query_.view(), kMaxRows, &windows_found, &windows_total, &windows_tiers));
      total = linux_total + windows_total;
      uint64_t l = 0, w = 0;
      while (ok && rows.count() < kMaxRows && (l < linux_found.count() || w < windows_found.count())) {
        bool take_linux = w >= windows_found.count();
        if (!take_linux && l < linux_found.count()) {
          const uint8_t lt = linux_tiers[l], wt = windows_tiers[w];
          take_linux = lt != wt ? lt < wt : appman::compare_folded(catalog_.at(linux_found[l]).name.view(), windows_catalog_.at(windows_found[w]).name.view()) <= 0;
        }
        if (!(take_linux ? add_linux(catalog_.at(linux_found[l++])) : add_windows(windows_catalog_.at(windows_found[w++])))) break;
      }
    }
    move_list(rows_, keys);
  }
  list_.clear_items();
  for (const Row& row : rows.view()) {
    const StringView cells[4] = {row.name.view(), app_platform_name(row.platform), row.description.view(), row.status};
    list_.add_item(cells, 4, ItemIcon::File);
  }
  if (!busy()) {
    String text(allocator_), total_text(allocator_), count_text(allocator_), shown_text(allocator_);
    const uint64_t shown = list_.item_count();
    total_text.append_uint(total);
    count_text.append_uint(count);
    shown_text.append_uint(shown);
    if (only_installed) {
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
  AppPlatform platform = AppPlatform::Linux;
  const bool have = row_name(list_.selected_item(), &name) && row_platform(list_.selected_item(), &platform);
  bool installed = false;
  {
    kernel::ScopedMutex state(state_mutex_);
    installed = have && is_installed(platform, name.view());
    if (have && platform == AppPlatform::Windows) {
      const appman::WingetPackage* package = windows_catalog_.find(name.view());
      if (!package) text.assign(name.view());
      else format_message(&text, "%1 (%2), version %3, for Windows", package->name.view(), package->id.view(), package->version.view());
      for (const appman::WingetInstalled& app : windows_installed_.view())
        if (appman::compare_folded(app.id.view(), name.view()) == 0) {
          String with(allocator_);
          format_message(&with, "%1  -  version %2 is installed", text.view(), app.version.view());
          text.assign(with.view());
        }
    } else if (have) {
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
  const bool ready = installer_ && windows_installer_ && !busy();
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
  if (linux_only_item_) linux_only_item_->set_checked(show_.selected_index() == kShowLinux);
  if (windows_only_item_) windows_only_item_->set_checked(show_.selected_index() == kShowWindows);
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
bool AppManager::start(Job job, AppPlatform platform, StringView name, bool ignore_checksum) {
  if (!network_ || (job != Job::Catalog && (!installer_ || !windows_installer_))) {
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
    job_platform_ = platform;
    job_name_.assign(name);
    job_ignore_checksum_ = ignore_checksum;
  }
  __atomic_store_n(&progress_state_.cancel, false, __ATOMIC_RELAXED);
  __atomic_store_n(&progress_state_.running_setup, false, __ATOMIC_RELAXED);
  progress_state_.changed = &AppManager::on_progress;
  progress_state_.context = this;
  progress_.set_value(0);
  update_controls();
  String text(allocator_);
  switch (job) {
    case Job::Catalog: text.assign("Fetching the lists of applications..."); break;
    case Job::Install: format_message(&text, "Installing \"%1\"...", name); break;
    case Job::Update: format_message(&text, "Looking for a newer \"%1\"...", name); break;
    case Job::Remove: format_message(&text, "Removing \"%1\"...", name); break;
    case Job::None: break;
  }
  say(text.view());
  worker_.post({&AppManager::run_job, this}, *this, false);
  return true;
}

bool AppManager::refresh_catalog() { return start(Job::Catalog, AppPlatform::Linux, StringView()); }

bool AppManager::start_selected(Job job) {
  String name(allocator_);
  AppPlatform platform = AppPlatform::Linux;
  if (!row_name(list_.selected_item(), &name) || !row_platform(list_.selected_item(), &platform)) {
    say("Select an application in the list first.");
    return false;
  }
  return start(job, platform, name.view());
}

bool AppManager::install_selected() { return start_selected(Job::Install); }
bool AppManager::update_selected() { return start_selected(Job::Update); }
bool AppManager::remove_selected() { return start_selected(Job::Remove); }

// Removing deletes the application's files, and nothing here brings them back, so the question comes first; the answer that leaves everything as it
// was is the default one. Without a window manager (nowhere to ask) it removes at once.
bool AppManager::ask_remove() {
  if (!remove_.enabled()) return false;
  if (!manager()) return remove_selected();
  if (!row_name(list_.selected_item(), &remove_name_) || !row_platform(list_.selected_item(), &remove_platform_)) return false;
  String line(allocator_);
  const StringView name = remove_name_.view();
  format_message(&line, "Remove \"%1\"?", name.size() > 30 ? name.first(30) : name);
  question_ = Question::Remove;
  asked_job_ = Job::Remove;
  ask_.set_texts(title(), line.view(), "Its files are deleted from this computer.", "Remove", "Cancel");
  ask_.set_default_choice(MessageDialog::Choice::Decline);
  return ask_.show(*manager());
}

// A publisher that serves its newest file at a fixed link (Google Chrome) changes that file before winget's list catches up, so the download no longer
// has the SHA-256 the list names. It may also be a file someone changed on the way, so the list's answer stands unless the user says otherwise; the
// default answer installs nothing. It is asked even if another window is in front by now (unlike a notice): without an answer the install the user
// started can go no further. (On the worker thread: the UI lock keeps the question's fields from the Remove question.)
bool AppManager::ask_ignore_checksum(Job job, StringView id, StringView name) {
  UiGuard lock(this);
  if (!manager() || ask_.showing()) return false;
  if (!remove_name_.assign(id)) return false;
  remove_platform_ = AppPlatform::Windows;
  question_ = Question::IgnoreChecksum;
  asked_job_ = job;
  String line(allocator_);
  format_message(&line, "\"%1\" is not the file in the list.", name.size() > 24 ? name.first(24) : name);
  ask_.set_texts(title(), line.view(), "It may be newer, or altered. Use it anyway?", job == Job::Update ? "Update Anyway" : "Install Anyway", "Cancel");
  ask_.set_default_choice(MessageDialog::Choice::Decline);
  return ask_.show(*manager());
}

void AppManager::on_answer(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  if (!m->ask_.accepted() || m->remove_name_.empty()) return;
  m->start(m->asked_job_, m->remove_platform_, m->remove_name_.view(), m->question_ == Question::IgnoreChecksum);
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
  if (__atomic_load_n(&progress.running_setup, __ATOMIC_RELAXED)) {      // (the download is done: how long the setup program takes is not known)
    format_message(&text, "Running the setup program of \"%1\"...", name.view());
    m->progress_.set_value(100);
    m->say(text.view());
    return;
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
  AppPlatform platform;
  String name(m->allocator_), detail(m->allocator_), text(m->allocator_);
  appman::WingetPackage package(m->allocator_);      // (a copy: the catalog may be replaced while the job runs)
  bool have_package = false, ignore_checksum = false;
  {
    kernel::ScopedMutex lock(m->state_mutex_);
    job = m->job_;
    ignore_checksum = m->job_ignore_checksum_;
    platform = m->job_platform_;
    name.assign(m->job_name_.view());
    if (platform == AppPlatform::Windows && job != Job::Catalog) {
      const appman::WingetPackage* found = m->windows_catalog_.find(name.view());
      have_package = found && package.copy_from(*found);
    }
  }
  appman::Result result = appman::Result::Ok;
  NoticeKind kind = NoticeKind::Information;
  bool tell = false;      // a failure: said in a message, not only in the status line
  if (job == Job::Catalog) {
    // Both lists; one that cannot be fetched leaves the one from last time in place.
    bool have_linux = false, have_windows = false, memory = true;
    {
      String list(m->allocator_);
      appman::Catalog fresh(m->allocator_);
      if (m->network_->fetch(appman::kCatalogUrl, appman::kMaxCatalogBytes, &list) && fresh.parse(list.view())) {
        if (m->files_) keep_file(*m->files_, m->allocator_, filesystem::layout::kAppManCatalog, list.view());
        kernel::ScopedMutex lock(m->state_mutex_);
        have_linux = m->catalog_.take(fresh);
        memory = memory && have_linux;
      }
    }
    {
      String index(m->allocator_), list(m->allocator_);
      appman::WingetCatalog fresh(m->allocator_);
      if (m->network_->fetch(appman::kWingetIndexUrl, appman::kMaxWingetIndexBytes, &index) && fresh.parse_index(index.view().bytes())) {
        if (m->files_ && fresh.write_list(&list)) keep_file(*m->files_, m->allocator_, filesystem::layout::kWingetCatalog, list.view());
        kernel::ScopedMutex lock(m->state_mutex_);
        have_windows = m->windows_catalog_.take(fresh);
        memory = memory && have_windows;
      }
    }
    String linux_text(m->allocator_), windows_text(m->allocator_);
    linux_text.append_uint(m->linux_count());
    windows_text.append_uint(m->windows_count());
    if (!memory) {
      text.assign("The list of applications could not be kept because there is not enough memory. Close other programs and try again.");
      kind = NoticeKind::Error;
      tell = true;
    } else if (have_linux && have_windows) {
      format_message(&text, "The list has %1 applications for Linux and %2 for Windows.", linux_text.view(), windows_text.view());
    } else if (have_linux) {
      format_message(&text, "The list has %1 applications for Linux. The list of applications for Windows could not be fetched.", linux_text.view());
    } else if (have_windows) {
      format_message(&text, "The list has %1 applications for Windows. The list of applications for Linux could not be fetched.", windows_text.view());
    } else {
      text.assign("The list of applications could not be fetched. Check the network connection, then choose Refresh List.");
      kind = NoticeKind::Error;
      tell = true;
    }
  } else {
    if (platform == AppPlatform::Windows) {
      switch (job) {
        case Job::Install: result = have_package ? m->windows_installer_->install(package, &m->progress_state_, &detail, ignore_checksum) : appman::Result::NoManifest; break;
        case Job::Update: result = have_package ? m->windows_installer_->update(package, &m->progress_state_, &detail, ignore_checksum) : appman::Result::NoManifest; break;
        case Job::Remove: result = m->windows_installer_->remove(name.view(), &detail); break;
        default: break;
      }
      if (have_package) name.assign(package.name.view());      // (the messages name the application, not its identifier)
    } else {
      switch (job) {
        case Job::Install: result = m->installer_->install(name.view(), &m->progress_state_, &detail); break;
        case Job::Update: result = m->installer_->update(name.view(), &m->progress_state_, &detail); break;
        case Job::Remove: result = m->installer_->remove(name.view(), &detail); break;
        default: break;
      }
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
  AppPlatform keep_platform = AppPlatform::Linux;
  const bool had = m->row_name(m->list_.selected_item(), &keep) && m->row_platform(m->list_.selected_item(), &keep_platform);
  m->rebuild();
  if (had) {      // the same application stays chosen (found under the state lock, chosen after it: the UI lock always comes first)
    int32_t row = -1;
    {
      kernel::ScopedMutex lock(m->state_mutex_);
      for (uint64_t i = 0; i < m->rows_.count() && row < 0; ++i)
        if (m->rows_.view()[i].platform == keep_platform && m->rows_.view()[i].id.view().equals(keep.view())) row = static_cast<int32_t>(i);
    }
    if (row >= 0) m->list_.select_item(row);
  }
  m->update_controls();
  // A download that is not the file the list names, over HTTPS: the user may install it anyway (asked instead of the notice).
  const bool may_override = platform == AppPlatform::Windows && have_package && result == appman::Result::BadChecksum && !ignore_checksum &&
                            (job == Job::Install || job == Job::Update) && appman::WingetInstaller::may_ignore_checksum(detail.view());
  if (may_override && m->ask_ignore_checksum(job, package.id.view(), name.view())) m->say(text.view());
  else if (tell) m->report(kind, text.view());
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
  m->set_filter(m->show_.selected_index() == kShowInstalled ? kShowAll : kShowInstalled);      // (from one of the two kinds as well: Installed is of both)
}
void AppManager::on_linux_only(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  m->set_filter(m->show_.selected_index() == kShowLinux ? kShowAll : kShowLinux);
}
void AppManager::on_windows_only(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  m->set_filter(m->show_.selected_index() == kShowWindows ? kShowAll : kShowWindows);
}
void AppManager::on_about(void* self, Control&) {
  auto* m = static_cast<AppManager*>(self);
  NoticeDialog::post(m->notices_, m->manager(), NoticeKind::Information,
                     "App Manager installs, updates and removes applications for Linux from the AppMan list (github.com/ivan-hc/AM) and applications for Windows from the winget list (github.com/microsoft/winget-pkgs).",
                     "About App Manager");
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
