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

// The desktop shell: a full-screen root window holding the desktop surface - where the window manager
// stacks application windows - and a taskbar along the bottom with the Start button (its menu opens
// upwards), the window switcher in the space in between, and the clock. The Start menu offers "File Explorer", "Computer Manager", "Text Editor", "Terminal", "Paint", "Media Player", "3D Test",
// and "About" (ordinary windows; a second File Explorer, Text Editor or Paint opens a new window next to the first, as many as wanted,
// and files can be dragged from one explorer to another or onto an editor) and "Shut Down...", which asks for confirmation in a modal dialog and then only
// requests the shutdown (shutdown_requested()): the boot code owns the hardware and does the orderly
// power-off once the desktop loop returns.
#pragma once
#include "Desktop/DesktopSurface.hpp"
#include "Desktop/DisplaySettings.hpp"
#include "Desktop/NetworkProperties.hpp"
#include "Desktop/ColorDialog.hpp"
#include "Desktop/CopyProgressDialog.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/PrintDialog.hpp"
#include "Desktop/FileExplorer.hpp"
#include "Desktop/Graphics3DTest.hpp"
#include "Desktop/MediaPlayer.hpp"
#include "Desktop/Notepad.hpp"
#include "Database/DatabaseManager.hpp"
#include "Desktop/DatabaseQuery.hpp"
#include "Desktop/Spreadsheet.hpp"
#include "Desktop/Paint.hpp"
#include "Desktop/Terminal.hpp"
#include "Desktop/ComputerManager.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/MenuButton.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/InputPump.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/RenderLoop.hpp"
#include "Gui/Window.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/TabFrame.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Gui/Windows/WindowSwitcher.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

namespace desktop {

struct DateTime {
  uint16_t year = 0;
  uint8_t month = 0, day = 0;  // 1-based
  uint8_t hour = 0, minute = 0;  // 24-hour clock
};

// Where the taskbar clock gets the local time from; false if it cannot be read right now.
using ClockSource = bool (*)(void* context, DateTime* out);

// "MM/dd/yyyy hh:mm AM" - 12-hour clock, always 19 characters. `out` needs room for 20. Returns the length.
uint32_t format_date_time(const DateTime& time, char* out);

class Desktop {
 public:
  static constexpr int32_t kTaskbarHeight = 32;
  // What gui::Frame::app_kind() says about a program's windows.
  enum AppKind : uint8_t { kAppNone, kAppExplorer, kAppEditor, kAppPaint, kAppTerminal, kAppMedia, kAppSheet, kAppDatabase };

  // The devices may be null (a machine without a mouse still shows the desktop).
  Desktop(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, hardware::IKeyboard* keyboard,
          hardware::IMouse* mouse);
  Desktop(const Desktop&) = delete;
  Desktop& operator=(const Desktop&) = delete;
  // Stops a copy that is still running.
  ~Desktop();

  gui::Window& window() { return window_; }
  // The system clipboard that copy and paste in every window of the desktop use (nullptr: none).
  void set_clipboard(kernel::Clipboard* clipboard) { window_.set_clipboard(clipboard); }
  gui::MenuButton& start_button() { return start_; }
  gui::WindowManager& windows() { return windows_; }
  gui::WindowSwitcher& switcher() { return switcher_; }
  gui::MessageDialog& shutdown_dialog() { return shutdown_dialog_; }
  // The dialog the programs show warnings, information and errors in.
  gui::NoticeDialog& notice_dialog() { return notice_dialog_; }
  // The window that shows files being moved between volumes and remote shares.
  CopyProgressDialog& copy_progress() { return copy_progress_; }
  gui::Frame& about_window() { return about_; }
  gui::Frame& explorer_window() { return explorer_; }
  FileExplorer& explorer() { return explorer_; }
  // The filesystem the file explorer browses (nullptr: none). It must outlive the desktop.
  void set_file_system(filesystem::IFileSystem* fs);
  Paint& paint() { return paint_; }
  MediaPlayer& media_player() { return media_player_; }
  Graphics3DTest& graphics_test() { return graphics_test_; }
  // The machine's graphics for the 3D test window (nullptr: it says there is none). They must outlive the desktop.
  void set_graphics(graphics::SystemGraphics* graphics) { graphics_test_.set_graphics(graphics); }
  // Opens the 3D test window (a spinning cube with the frame rate and whether the GPU draws it), or brings it forward.
  void open_graphics_test();
  // The machine's sound mixer, video codecs and clock for the media player (any may be nullptr). They must outlive the desktop.
  void set_media_services(audio::AudioMixer* mixer, hardware::SystemVideoCodecs* codecs, const hardware::IClock* clock) {
    media_player_.set_services(mixer, codecs, clock);
  }
  // The machine's MIDI output hardware for the media player to play MIDI files on (nullptr: none, they are synthesized in software).
  void set_midi_output(midi::IMidiOutput* output) { media_player_.set_midi_output(output); }
  // Opens the media player, or brings it forward (restoring it if minimized).
  void open_media_player(gui::TabFrame* into = nullptr);
  // The Open and Save As dialog that the text editor and Paint share.
  FileDialog& file_dialog() { return file_dialog_; }
  // The color and print dialogs the programs share.
  ColorDialog& color_dialog() { return color_dialog_; }
  PrintDialog& print_dialog() { return print_dialog_; }
  // The printers the print dialog offers and sends jobs to (nullptr: none). It must outlive the desktop.
  void set_print_service(IPrintService* service) { print_dialog_.set_print_service(service); }
  // Opens a paint window: the first one is paint() (when that is closed), further ones are made as needed and go away when closed.
  // Returns the window (nullptr if memory ran out).
  Paint* open_paint(gui::TabFrame* into = nullptr);
  Notepad& notepad() { return notepad_; }
  // The databases programs may connect to (nullptr: none). It must outlive the desktop.
  void set_databases(database::DatabaseManager* databases);
  database::DatabaseManager* databases() const { return databases_; }
  // Opens a database query window: the first one is database_query() (when that is closed), further ones are made as needed and go away when closed.
  DatabaseQuery* open_database_query(gui::TabFrame* into = nullptr);
  DatabaseQuery& database_query() { return database_query_; }
  uint32_t database_query_count() const;
  // Opens a spreadsheet window: the first one is spreadsheet() (when that is closed), further ones are made as needed and go away when closed.
  Spreadsheet* open_spreadsheet(gui::TabFrame* into = nullptr);
  Spreadsheet& spreadsheet() { return spreadsheet_; }
  uint32_t spreadsheet_count() const;
  // Opens a terminal window: the first one is terminal() (when that is closed), further ones are made as needed and go away when closed.
  Terminal* open_terminal(gui::TabFrame* into = nullptr);
  Terminal& terminal() { return terminal_; }
  uint32_t terminal_count() const;
  // Opens a text editor window: the first one is notepad() (when that is closed), further ones are made as needed and go away when closed.
  Notepad* open_text_editor(gui::TabFrame* into = nullptr);
  ComputerManager& computer_manager() { return computer_manager_; }
  // The machine's figures for the computer manager (nullptr: none). It must outlive the desktop.
  void set_system_monitor(ISystemMonitor* monitor) { computer_manager_.set_monitor(monitor); }
  // Opens the computer manager, or brings it forward (restoring it if minimized); it refreshes once a second meanwhile.
  void open_computer_manager();
  // Opens a file explorer window: the first one is explorer() (when that is closed), further ones are made as needed and go away when closed.
  FileExplorer* open_explorer(gui::TabFrame* into = nullptr);
  // Opens what `path` names the way a double click in a file explorer would: a folder in a file explorer (an open one, else a new one), a
  // picture in Paint, a sound or video in the media player, anything else in a text editor. False if it does not exist.
  bool open_path(StringView path);
  // Reads the folder of every open file explorer again (files moved in one show up in the other).
  void refresh_explorers();
  // How many windows of each kind are open right now.
  uint32_t explorer_count() const;
  uint32_t text_editor_count() const;
  uint32_t paint_count() const;
  // Opens the About window, or brings it forward (restoring it if minimized).
  void open_about();
  // --- Monitors. Tells the desktop about the monitors it is shown on (nullptr: one screen, the default). The display must already show the layout
  // (SystemMonitors::apply()), so that the surface the desktop draws on is the size of the layout's bounds. `monitors` must outlive the desktop.
  void set_monitors(hardware::SystemMonitors* monitors);
  // How the desktop gets the screen changed: applies the monitors' layout to the display hardware and builds the renderer for the desktop's new
  // size, which it returns (nullptr: it failed and the old renderer stays). Without one, a change of the layout only moves the windows around.
  using DisplayChanger = graphics::IRenderer2D* (*)(void* context);
  void set_display_changer(DisplayChanger changer, void* context) { changer_ = changer; changer_context_ = context; }
  // Makes what was changed in the monitors (their places, resolutions, scales) real: drawing stops for a moment, the display changer runs, the window
  // takes the new renderer and its size, drawing goes on, and the taskbar and the work areas are redone. False if the changer failed.
  bool apply_monitor_changes();
  // The layout of `monitors` changed (a monitor came or went, or the order was changed): the taskbar, work areas and pointer limits are redone.
  void monitors_changed();
  DisplaySettings& display_settings() { return display_settings_; }
  // Where the pointer is (window coordinates).
  gui::Point pointer() const { return input_.cursor(); }
  // The devices of the machine for the computer manager's Hardware tab (nullptr: none). It must outlive the desktop.
  void set_hardware_inventory(IHardwareInventory* inventory) { computer_manager_.set_hardware_inventory(inventory); }
  // The network adapters' details for the properties window (nullptr: none). It must outlive the desktop.
  void set_network_details(INetworkDetails* details) { network_properties_.set_details(details); }
  // Opens the properties window of a network adapter (its number: HardwareEntry::id), or brings it forward showing that adapter. It is reached from
  // the menu of an adapter in the computer manager's hardware tree.
  void open_network_properties(uint32_t adapter);
  NetworkProperties& network_properties() { return network_properties_; }
  // Opens the display settings window, or brings it forward. It is reached from the Displays group of the computer manager's hardware tree.
  void open_display_settings();
  // Asks "shut down?" in a modal dialog; accepting it makes shutdown_requested() true.
  void ask_shut_down();
  StringView clock_text() const { return clock_.text(); }
  bool shutdown_requested() const { return __atomic_load_n(&shutdown_requested_, __ATOMIC_ACQUIRE); }
  void request_shutdown() { __atomic_store_n(&shutdown_requested_, true, __ATOMIC_RELEASE); }
  // The clock shows "--/--/---- --:-- --" until a source is set (and while it fails).
  void set_clock_source(ClockSource source, void* context) { clock_source_ = source; clock_context_ = context; shown_ = ~0ull; }

  // --- Running. The desktop uses three threads: the caller's loop (input and the clock: pump()), a render
  // thread (redraws whenever something changed) and the handler thread that runs what controls fire
  // (menu items, button clicks, dialog answers). A slow handler therefore cannot stop the screen
  // from updating or the mouse from moving; handlers and other threads may change controls freely
  // (every setter is thread-safe, see UiGuard).
  // Starts the render and handler threads; false if threads are unavailable (then use step()).
  bool start();
  // Ends both threads (what was queued still runs).
  void stop();
  // Where the desktop reports trouble it can see but not fix (no screen output: the screen may be what is stuck).
  void set_diagnostic_output(void (*write)(StringView text)) { diagnostic_ = write; }
  // Frames the render thread has drawn.
  uint32_t frames_drawn() const { return render_loop_.frames(); }
  // Delivers pending input and refreshes the clock (reads the source at most once per second of
  // `uptime_us`). Call in a loop from the thread that owns the input devices.
  void pump(uint64_t uptime_us);
  // Single-threaded alternative to start(): pump(), then draw if anything changed. Returns whether it drew.
  bool step(uint64_t uptime_us);

 private:
  static void on_paint(void* self, gui::Control&);
  static void on_media_player(void* self, gui::Control&);
  static void on_graphics_test(void* self, gui::Control&);
  static void on_text_editor(void* self, gui::Control&);
  static void on_spreadsheet(void* self, gui::Control&);
  static void on_database_query(void* self, gui::Control&);
  static bool on_shortcut(void* self, const gui::KeyArgs& args);
  static void on_terminal(void* self, gui::Control&);
  static void on_new_terminal(void* self, gui::Control&);
  static void on_explorer_file(void* self, gui::Control&);
  static void on_files_changed(void* self, gui::Control&);
  static void on_terminal_path(void* self, gui::Control&);
  void open_file(StringView file);
  static void on_copy_finished(void* self, gui::Control&);
  static void on_extra_closed(void* self, gui::Control&);
  static void on_computer_manager(void* self, gui::Control&);
  static void on_explorer(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_display_settings(void* self, gui::Control&);
  static void on_network_properties(void* self, gui::Control&);
  static bool apply_changes_thunk(void* self);
  static void confine_pointer(void* self, int32_t* x, int32_t* y);
  static void on_shut_down(void* self, gui::Control&);
  static void on_shutdown_answer(void* self, gui::Control&);

  // Windows beyond the first of their kind (the members below): made when asked for, destroyed from pump() once they are closed.
  template <class T>
  T* open_instance(T& first, List<UniquePointer<T>>& extras, void (Desktop::*setup)(T&), gui::TabFrame* into);
  gui::TabFrame* new_host();
  // Puts the program window `tab` in a tab of `into`, or of a new window.
  bool place(gui::Frame* tab, gui::TabFrame* into);
  static void on_new_tab(void* self, gui::Control& sender);
  static void on_tab_torn_off(void* self, gui::Control& sender);
  void setup_explorer(FileExplorer& explorer);
  void setup_notepad(Notepad& notepad);
  void setup_spreadsheet(Spreadsheet& sheet);
  void setup_database_query(DatabaseQuery& query);
  void setup_paint(Paint& paint);
  void setup_terminal(Terminal& terminal);
  void reap_closed();
  // The text editor / paint window to open a file in: the one that already shows it, an open empty one, or a new one.
  Notepad* editor_for(StringView path);
  Paint* paint_for(StringView path);
  Spreadsheet* spreadsheet_for(StringView path);
  DatabaseQuery* database_query_for(StringView path);

  allocators::IAllocator& allocator_;
  gui::Theme theme_;
  gui::Window window_;
  gui::InputPump input_;
  gui::RenderLoop render_loop_;
  DesktopSurface surface_;
  gui::Panel taskbar_;
  gui::MenuButton start_;
  gui::Panel gap_left_, gap_right_;
  gui::WindowManager windows_;
  gui::WindowSwitcher switcher_;
  gui::Panel tray_;
  gui::Label clock_;
  gui::MessageDialog shutdown_dialog_;
  gui::NoticeDialog notice_dialog_;
  FileDialog file_dialog_;
  CopyProgressDialog copy_progress_;
  ColorDialog color_dialog_;
  PrintDialog print_dialog_;
  FileExplorer explorer_;
  ComputerManager computer_manager_;
  Notepad notepad_;
  Paint paint_;
  Spreadsheet spreadsheet_;
  DatabaseQuery database_query_;
  Terminal terminal_;
  MediaPlayer media_player_;
  Graphics3DTest graphics_test_;
  gui::MessageDialog display_confirm_dialog_;   // "keep these display settings?"
  DisplaySettings display_settings_;
  NetworkProperties network_properties_;
  gui::Frame about_;
  gui::Label about_lines_[3];
  filesystem::IFileSystem* fs_ = nullptr;
  database::DatabaseManager* databases_ = nullptr;
  List<UniquePointer<FileExplorer>> extra_explorers_;
  List<UniquePointer<Notepad>> extra_notepads_;
  List<UniquePointer<Paint>> extra_paints_;
  List<UniquePointer<Spreadsheet>> extra_sheets_;
  List<UniquePointer<DatabaseQuery>> extra_queries_;
  List<UniquePointer<Terminal>> extra_terminals_;
  List<UniquePointer<gui::TabFrame>> hosts_;   // the tabbed windows
  bool reap_needed_ = false;
  hardware::SystemMonitors* monitors_ = nullptr;
  DisplayChanger changer_ = nullptr;
  void* changer_context_ = nullptr;
  bool taskbar_in_surface_ = false;
  kernel::Mutex pointer_lock_;
  hardware::MonitorRect monitor_rects_[hardware::MonitorLayout::kMaxMonitors];   // the monitors the pointer may be on (enabled ones)
  uint32_t monitor_rect_count_ = 0;
  uint64_t polled_monitors_second_ = ~0ull;
  ClockSource clock_source_ = nullptr;
  void* clock_context_ = nullptr;
  void (*diagnostic_)(StringView) = nullptr;
  bool watching_ = false;  // the watchdog has its first sample
  uint32_t last_frames_ = 0;
  uint64_t stall_since_us_ = 0, stall_reported_us_ = 0;
  uint64_t polled_second_ = ~0ull;  // the uptime second the source was last read in
  uint64_t shown_ = ~0ull;          // what the label shows, packed (see step())
  uint64_t now_us_ = 0;             // the latest uptime pump() was given (the explorer measures double clicks with it)
  bool shutdown_requested_ = false;
};

}  // namespace desktop
