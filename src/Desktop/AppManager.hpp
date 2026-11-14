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

// The App Manager: the AppImage package manager of the desktop, over AppMan's catalog (https://github.com/ivan-hc/AM). It lists the AppImages AppMan
// knows (about 9000), finds them by name or description as you type, and installs, updates and removes them in the Linux world, where the Start menu
// then lists them ("Name (Linux)") and they run as AppImages do (src/Program/AppImageLaunch.hpp). "Show" narrows the list to the installed programs.
//
// The work happens on a thread of its own (a download can take minutes; the window stays usable and Cancel stops it), one job at a time. The catalog
// is kept in /AppStore/AppMan (so the list is there at once next time; "Refresh list" fetches it again); the programs go where AppMan puts them
// (AppMan/AppManInstaller.hpp). Like every program window it can be a tab of a tabbed window.
//
// Every command is in the menu bar (File, View, Application, Help) as well as on a button, and the commands of an application are in the
// context menu of the list. What cannot be done now is unavailable. Remove deletes an application's files, so it asks first and Cancel is the
// default answer (Del in the list is Remove). F5 is Refresh List. A job that fails says so in a message when the window is the active one
// (never on top of other work), and in the status line either way.
#pragma once
#include "Desktop/AppMan/AppManCatalog.hpp"
#include "Desktop/AppMan/AppManInstaller.hpp"
#include "Desktop/HandlerThread.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Controls/ProgressBar.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Kernel/Sync.hpp"

namespace desktop {

class AppManager : public gui::Frame {
 public:
  enum Filter : int32_t { kShowAll, kShowInstalled };
  static constexpr uint64_t kMaxRows = 400;      // (the list shows the best matches; typing narrows it)

  explicit AppManager(allocators::IAllocator& allocator);
  ~AppManager();

  // Where programs are installed (the OS file system, which holds /Linux and /AppStore) and how the network is reached. Either may be null: nothing can be
  // installed then. Both must outlive the window. Loads the catalog kept from last time, or fetches it.
  void set_services(filesystem::IFileSystem* files, appman::INetwork* network);

  // The commands behind the controls (the jobs run on the window's thread, or at once where there are no threads: the tests). False when it cannot be
  // done now (nothing chosen, already installed, a job running); status() says why.
  bool refresh_catalog();
  bool install_selected();
  bool update_selected();
  bool remove_selected();
  // What the Remove button, menu item and Del do: asks "Remove <name>?" first (when the window is in a window manager), then removes.
  bool ask_remove();
  void cancel();
  // Where messages are shown (nullptr: in the status line only). It must outlive the window.
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; update_controls(); }
  void set_query(StringView text);
  void set_filter(Filter filter);
  void select_row(int32_t row);

  // What the window shows.
  StringView status() const { return status_.text(); }
  StringView details() const { return details_.text(); }
  uint64_t catalog_count() const;
  bool busy() const { return __atomic_load_n(&busy_, __ATOMIC_ACQUIRE); }
  // The application of a row of the list ("" if none).
  bool row_name(int32_t row, String* out) const;
  gui::ListView& list() { return list_; }
  gui::TextBox& search_box() { return search_; }
  gui::DropDown& show_box() { return show_; }
  gui::Button& install_button() { return install_; }
  gui::Button& update_button() { return update_; }
  gui::Button& remove_button() { return remove_; }
  gui::Button& cancel_button() { return cancel_; }
  gui::Button& refresh_button() { return refresh_; }
  gui::ProgressBar& progress_bar() { return progress_; }
  gui::MenuBar& menu_bar() { return menu_; }
  gui::ContextMenu& list_menu() { return list_menu_; }
  gui::MessageDialog& remove_dialog() { return ask_; }

  bool on_key(const gui::KeyArgs& args) override;

  void layout() override;

 private:
  enum class Job : uint8_t { None, Catalog, Install, Update, Remove };
  static void on_search(void* self, gui::Control&);
  static void on_show(void* self, gui::Control&);
  static void on_pick(void* self, gui::Control&);
  static void on_activate(void* self, gui::Control&);
  static void on_install(void* self, gui::Control&);
  static void on_update(void* self, gui::Control&);
  static void on_remove(void* self, gui::Control&);
  static void on_cancel(void* self, gui::Control&);
  static void on_refresh(void* self, gui::Control&);
  static void on_exit(void* self, gui::Control&);
  static void on_installed_only(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_remove_answer(void* self, gui::Control&);
  void report(gui::NoticeKind kind, StringView text);   // a result the user must not miss
  static void run_job(void* self, gui::Control&);
  static void on_progress(void* self, const appman::Progress& progress);

  bool start(Job job, StringView name);
  bool load_cached_catalog();
  void reload_installed();
  void rebuild();            // the list from the catalog, the query and the filter
  void update_controls();    // the details line and the buttons, for the chosen row
  bool is_installed(StringView name) const;      // (state_mutex_ held)
  void say(StringView text) { status_.set_text(text); }

  allocators::IAllocator& allocator_;
  gui::MenuBar menu_;
  gui::ContextMenu list_menu_;
  gui::MenuItem *installed_only_item_ = nullptr, *refresh_item_ = nullptr, *install_item_ = nullptr, *update_item_ = nullptr, *remove_item_ = nullptr,
                *cancel_item_ = nullptr, *about_item_ = nullptr, *list_install_item_ = nullptr, *list_update_item_ = nullptr, *list_remove_item_ = nullptr;
  gui::MessageDialog ask_;                      // "Remove <name>?"
  String remove_name_;                          // the application the question is about
  gui::NoticeDialog* notices_ = nullptr;
  gui::Label search_label_, show_label_, details_, status_;
  gui::TextBox search_;
  gui::DropDown show_;
  gui::Button refresh_, install_, update_, remove_, cancel_;
  gui::ListView list_;
  gui::ProgressBar progress_;

  filesystem::IFileSystem* files_ = nullptr;
  appman::INetwork* network_ = nullptr;
  UniquePointer<appman::Installer> installer_;

  // What the worker thread and the controls share. Lock order: the UI lock (UiGuard) before state_mutex_, never the other way round.
  mutable kernel::Mutex state_mutex_;
  appman::Catalog catalog_;                     // (state_mutex_)
  List<appman::InstalledApp> installed_;        // (state_mutex_) by name
  List<String> rows_;                           // (state_mutex_) the application of each row of the list
  String query_;                                // (state_mutex_)
  Job job_ = Job::None;                         // (state_mutex_) the job the worker is to run, and for what
  String job_name_;                             // (state_mutex_)
  appman::Progress progress_state_;
  bool busy_ = false;
  HandlerThread worker_;                        // (last: stopped first when the window goes)
};

}  // namespace desktop
