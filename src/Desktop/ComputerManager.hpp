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

// The computer manager: a window with four tabs. "Processes" is a table of the running kernel threads with their state,
// core and share of the processor; "Usermode" is the same for the user-mode programs (state, threads, share of the processor, memory); "Performance" shows the processor load (overall and per core) and memory use; "Hardware" is a tree of the
// devices of the machine grouped by kind (processors, memory, display adapters, displays, disk drives, ...), with a line of detail for the selected one.
// It reads the machine through an ISystemMonitor and an IHardwareInventory and refreshes itself when update() is called (the desktop does so once
// a second while the window is open). Right-clicking the Displays group of the hardware tree offers Display Settings, and right-clicking a network
// adapter offers Properties (its address, gateway, DNS server, link state and speed - another window, see NetworkProperties); a Wi-Fi adapter also offers Wi-Fi Properties (the network it is joined to, the networks in range and joining one - see WirelessProperties). The File menu
// closes the window; the View menu picks the tab. Every other device has a Properties item too (a display's also Display Settings): the same window for
// every device of a kind, whatever its driver - see DeviceProperties and IDeviceDetails.
#pragma once
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/LevelMeter.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ScrollPanel.hpp"
#include "Desktop/HardwareInventory.hpp"
#include "Gui/Controls/TabControl.hpp"
#include "Gui/Controls/TreeView.hpp"
#include "Utility/UniquePointer.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

enum class TaskState : uint8_t { Ready, Running, Blocked };

struct TaskThread {
  uint64_t id = 0;
  char name[24] = {};
  TaskState state = TaskState::Ready;
  bool idle = false;       // a core's idle thread
  int32_t core = -1;       // the core it runs on right now, or -1
  uint64_t cpu_ticks = 0;  // cumulative
};

// One user-mode program (a process, with all its threads).
struct TaskProcess {
  uint64_t id = 0;
  char name[24] = {};
  bool windows = false;        // otherwise Linux
  TaskState state = TaskState::Blocked;
  uint32_t threads = 0;
  uint64_t cpu_ticks = 0;      // cumulative, all threads
  uint64_t memory_bytes = 0;
};

struct TaskCore {
  bool online = false;
  bool efficiency = false;
  uint64_t busy_ticks = 0, idle_ticks = 0;  // cumulative
};

struct TaskMemory {
  uint64_t total_bytes = 0, free_bytes = 0;
};

// The graphics adapter's figures; a field the adapter cannot tell is left at "unknown".
struct TaskGpu {
  int32_t usage_percent = -1;          // 0-100, -1 unknown
  bool memory_known = false;           // memory_total_bytes 0: the used figure is known, the total is not
  uint64_t memory_total_bytes = 0, memory_used_bytes = 0;
  int32_t temperature_millicelsius = -1;   // -1 unknown
};

// The neural processing unit, if the machine has a hardware one. busy_us only grows: the time it has spent computing.
struct TaskNpu {
  bool present = false;
  uint64_t busy_us = 0;
};

// The battery, if the machine has one.
enum class ChargeState : uint8_t { Unknown, Charging, Discharging, Full, NotCharging };
struct TaskBattery {
  bool present = false;
  int32_t percent = -1;                  // 0-100, -1 unknown
  ChargeState state = ChargeState::Unknown;
};

// Where the computer manager gets its numbers (the kernel's, in the real system). All counters only grow.
class ISystemMonitor {
 public:
  // How many ticks a core counts per second (the unit of cpu_ticks, busy_ticks and idle_ticks).
  virtual uint32_t ticks_per_second() const = 0;
  virtual uint32_t core_count() const = 0;
  virtual bool core(uint32_t index, TaskCore* out) = 0;
  // Writes up to `capacity` threads; returns how many.
  virtual uint32_t threads(TaskThread* out, uint32_t capacity) = 0;
  // Writes up to `capacity` user-mode programs; returns how many.
  virtual uint32_t processes(TaskProcess* out, uint32_t capacity) { (void)out; (void)capacity; return 0; }
  virtual TaskMemory memory() = 0;
  // The processor's temperature in thousandths of a degree Celsius, or -1 if the machine cannot tell.
  virtual int32_t cpu_temperature_millicelsius() { return -1; }
  virtual TaskGpu gpu() { return TaskGpu(); }
  virtual TaskNpu npu() { return TaskNpu(); }
  virtual TaskBattery battery() { return TaskBattery(); }

 protected:
  constexpr ISystemMonitor() = default;
  ~ISystemMonitor() = default;
};

class ComputerManager : public gui::Frame {
 public:
  static constexpr uint32_t kMaxThreads = 256;
  static constexpr uint32_t kMaxProcesses = 128;
  static constexpr uint32_t kMaxCoresShown = 256;

  explicit ComputerManager(allocators::IAllocator& allocator);
  ~ComputerManager();

  // The source of the numbers (nullptr: none); it must outlive the computer manager.
  void set_monitor(ISystemMonitor* monitor);
  // The list of devices the Hardware tab shows (nullptr: none); it must outlive the computer manager.
  void set_hardware_inventory(IHardwareInventory* inventory);
  // Fired when Display Settings is chosen from the menu of the Displays group.
  void on_open_display_settings(gui::Handler handler) { open_display_settings_ = handler; }
  // Fired when Properties is chosen for a network adapter; network_adapter() is the adapter's number (HardwareEntry::id).
  void on_open_network_properties(gui::Handler handler) { open_network_properties_ = handler; }
  uint32_t network_adapter() const { return properties_adapter_; }
  // Fired when Wi-Fi Properties is chosen for a wireless adapter; wireless_adapter() is the adapter's number (HardwareEntry::id).
  void on_open_wireless_properties(gui::Handler handler) { open_wireless_properties_ = handler; }
  uint32_t wireless_adapter() const { return properties_adapter_; }
  // Fired when Properties is chosen for any other device; device_group() and device_id() say which (HardwareEntry::group and ::id).
  void on_open_device_properties(gui::Handler handler) { open_device_properties_ = handler; }
  HardwareGroup device_group() const { return device_group_; }
  uint32_t device_id() const { return properties_adapter_; }
  // Fired when Bluetooth Settings is chosen for a Bluetooth adapter; bluetooth_adapter() is the adapter's number (HardwareEntry::id).
  void on_open_bluetooth_properties(gui::Handler handler) { open_bluetooth_properties_ = handler; }
  uint32_t bluetooth_adapter() const { return properties_adapter_; }
  // Reads the monitor and refreshes everything. `now_us` is the uptime; load figures are over the time since
  // the previous call.
  void update(uint64_t now_us);

  gui::DataGrid& grid() { return grid_; }
  // The table of the Usermode tab.
  gui::DataGrid& process_grid() { return process_grid_; }
  // The CPU share (tenths of a percent of one core) of the program with this id, or -1 if it is not listed.
  int32_t process_load_tenths(uint64_t id) const;
  // The text of a cell of the Usermode table (PID, Program, Type, State, Thr, CPU %, Memory).
  bool process_cell(uint32_t row, uint32_t column, String* out) const { return process_model_.cell_text(row, column, out); }
  gui::LevelMeter& cpu_meter() { return cpu_meter_; }
  gui::LevelMeter& memory_meter() { return memory_meter_; }
  gui::Label& cpu_label() { return cpu_value_; }
  gui::Label& memory_label() { return memory_value_; }
  gui::Label& info_label() { return info_; }
  gui::TabControl& tabs() { return tabs_; }
  gui::TreeView& hardware_tree() { return tree_; }
  gui::ContextMenu& display_menu() { return display_menu_; }
  gui::ContextMenu& adapter_menu() { return adapter_menu_; }
  // The menu of a device that has no menu of its own (a sound card, a disk, ...): Properties. A display's also has Display Settings.
  gui::ContextMenu& device_menu() { return device_menu_; }
  gui::ContextMenu& monitor_menu() { return monitor_menu_; }
  // The menu of a Wi-Fi adapter: Properties and Wi-Fi Properties.
  gui::ContextMenu& wireless_menu() { return wireless_menu_; }
  gui::ContextMenu& bluetooth_menu() { return bluetooth_menu_; }
  gui::Label& hardware_info() { return hardware_info_; }
  // The tree node of a group (-1 if no device of the group is listed).
  int32_t hardware_group_node(HardwareGroup group) const;
  gui::MenuBar& menu_bar() { return menu_; }
  // The table's context menu (Copy Row).
  gui::ContextMenu& grid_menu() { return grid_menu_; }
  // The meter and label of core `index` (one per online core up to kMaxCoresShown; nullptr for others).
  gui::LevelMeter* core_meter(uint32_t index);
  // The GPU, video memory and temperature rows of the Performance tab.
  gui::LevelMeter& gpu_meter() { return gpu_row_.meter; }
  gui::Label& gpu_label() { return gpu_row_.value; }
  // The NPU row of the Performance tab; it shows only while the machine has an NPU.
  gui::LevelMeter& npu_meter() { return npu_row_.meter; }
  gui::Label& npu_label() { return npu_row_.value; }
  bool npu_row_visible() const { return npu_row_.row.visible(); }
  gui::LevelMeter& vram_meter() { return vram_row_.meter; }
  gui::Label& vram_label() { return vram_row_.value; }
  gui::LevelMeter& cpu_temp_meter() { return cpu_temp_row_.meter; }
  gui::Label& cpu_temp_label() { return cpu_temp_row_.value; }
  gui::LevelMeter& gpu_temp_meter() { return gpu_temp_row_.meter; }
  gui::Label& gpu_temp_label() { return gpu_temp_row_.value; }
  // The battery row: charge level (meter) and "85% - Charging" (label).
  // The scrolling area that holds the core rows.
  gui::ScrollPanel& core_scroll() { return core_scroll_; }
  gui::LevelMeter& battery_meter() { return battery_row_.meter; }
  gui::Label& battery_label() { return battery_row_.value; }
  gui::Label* core_label(uint32_t index);
  // The CPU share (tenths of a percent of one core) of the thread with this id, or -1 if it is not listed.
  int32_t thread_load_tenths(uint64_t id) const;

 private:
  struct Row {
    uint64_t id = 0;
    char name[24] = {};
    TaskState state = TaskState::Ready;
    bool idle = false;
    int32_t core = -1;
    uint32_t tenths = 0;  // share of one core, in tenths of a percent
    uint64_t ticks = 0;
  };
  struct Previous {
    uint64_t id, ticks;
  };

  class Model final : public gui::IGridModel {
   public:
    explicit Model(allocators::IAllocator& allocator) : rows(allocator) {}
    uint32_t row_count() const override { return static_cast<uint32_t>(rows.count()); }
    bool cell_text(uint32_t row, uint32_t column, String* out) const override;
    List<Row> rows;
    uint32_t ticks_per_second = 1000;
  };

  struct ProcessRow {
    uint64_t id = 0;
    char name[24] = {};
    bool windows = false;
    TaskState state = TaskState::Blocked;
    uint32_t threads = 0;
    uint32_t tenths = 0;
    uint64_t memory = 0;
  };

  class ProcessModel final : public gui::IGridModel {
   public:
    explicit ProcessModel(allocators::IAllocator& allocator) : rows(allocator) {}
    uint32_t row_count() const override { return static_cast<uint32_t>(rows.count()); }
    bool cell_text(uint32_t row, uint32_t column, String* out) const override;
    List<ProcessRow> rows;
  };

  // The row of one core on the Performance tab.
  struct CoreRow {
    explicit CoreRow(allocators::IAllocator& allocator) : row(allocator), title(allocator), value(allocator), meter(allocator) {}
    gui::Panel row;
    gui::Label title, value;
    gui::LevelMeter meter;
  };

  // Fills `row`'s three parts and docks it at the top of the Performance tab.
  void setup_stat_row(CoreRow& row, const char* title);

  static void on_show_processes(void* self, gui::Control&);
  static void on_show_performance(void* self, gui::Control&);
  static void on_close_window(void* self, gui::Control&);
  static void on_copy_row(void* self, gui::Control&);
  static void on_grid_menu_opening(void* self, gui::Control&);
  static void on_tab_changed(void* self, gui::Control&);
  static void on_show_hardware(void* self, gui::Control&);
  static void on_show_usermode(void* self, gui::Control&);
  static void on_hardware_selection(void* self, gui::Control&);
  static void on_display_settings_item(void* self, gui::Control&);
  static void on_properties_item(void* self, gui::Control&);
  static void on_wireless_item(void* self, gui::Control&);
  static void on_device_item(void* self, gui::Control&);
  static void on_bluetooth_item(void* self, gui::Control&);
  bool selected_adapter(uint32_t* id) const;
  bool selected_entry(HardwareEntry* out) const;
  void rebuild_hardware();

  gui::MenuBar menu_;
  gui::ContextMenu grid_menu_;
  gui::MenuItem* copy_row_item_ = nullptr;
  gui::MenuItem *processes_item_ = nullptr, *performance_item_ = nullptr, *hardware_item_ = nullptr, *usermode_item_ = nullptr;
  gui::TabControl tabs_;
  gui::ScrollPanel core_scroll_;
  gui::Panel processes_page_, performance_page_, hardware_page_, usermode_page_, cpu_row_, memory_row_;
  gui::TreeView tree_;
  gui::ContextMenu display_menu_, adapter_menu_, wireless_menu_, device_menu_, monitor_menu_, bluetooth_menu_;
  gui::MenuItem *device_properties_item_ = nullptr, *monitor_properties_item_ = nullptr, *monitor_settings_item_ = nullptr, *bluetooth_properties_item_ = nullptr, *bluetooth_settings_item_ = nullptr;
  gui::MenuItem *display_settings_item_ = nullptr, *properties_item_ = nullptr, *wireless_properties_item_ = nullptr, *wireless_adapter_item_ = nullptr;
  gui::Label hardware_info_;
  IHardwareInventory* inventory_ = nullptr;
  gui::Handler open_display_settings_, open_network_properties_, open_wireless_properties_, open_device_properties_, open_bluetooth_properties_;
  HardwareGroup device_group_ = HardwareGroup::System;
  uint32_t properties_adapter_ = 0;
  int32_t group_nodes_[kHardwareGroupCount] = {};
  gui::Label cpu_title_, cpu_value_, memory_title_, memory_value_, info_;
  gui::LevelMeter cpu_meter_, memory_meter_;
  gui::DataGrid grid_, process_grid_;
  CoreRow gpu_row_, npu_row_, vram_row_, cpu_temp_row_, gpu_temp_row_, battery_row_;
  List<UniquePointer<CoreRow>> core_rows_;
  Model model_;
  ProcessModel process_model_;
  ISystemMonitor* monitor_ = nullptr;
  List<TaskCore> previous_cores_;
  List<Previous> previous_threads_, previous_processes_;
  ArrayView<TaskThread> buffer_;
  ArrayView<TaskProcess> process_buffer_;
  uint64_t last_us_ = 0;
  uint64_t previous_npu_busy_us_ = 0;
  bool had_npu_ = false;
  bool have_previous_ = false;
};

}  // namespace desktop
