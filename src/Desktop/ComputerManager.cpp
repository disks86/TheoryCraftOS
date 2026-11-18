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

#include "Desktop/ComputerManager.hpp"
#include "Desktop/Format.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

namespace {
constexpr int32_t kTitleWidth = 136;   // wide enough for "GPU temperature" in the 8-pixel font
constexpr char kNotAvailable[] = "Not available";   // a figure the machine cannot tell
}

using namespace gui;

bool ComputerManager::Model::cell_text(uint32_t row, uint32_t column, String* out) const {
  out->clear();
  if (row >= rows.count()) return true;
  const Row& r = rows[row];
  switch (column) {
    case 0: return r.idle ? out->assign("-") : out->append_uint(r.id);
    case 1: {
      uint64_t length = 0;
      while (length < sizeof r.name && r.name[length]) ++length;
      return out->assign(StringView(r.name, length));
    }
    case 2: return out->assign(r.state == TaskState::Running ? "Running" : r.state == TaskState::Blocked ? "Blocked" : "Ready");
    case 3: return r.core < 0 || out->append_uint(static_cast<uint64_t>(r.core));
    case 4: return append_tenths(out, r.tenths);
    case 5: return append_tenths(out, r.ticks * 10 / (ticks_per_second ? ticks_per_second : 1));
    default: return true;
  }
}

bool ComputerManager::ProcessModel::cell_text(uint32_t row, uint32_t column, String* out) const {
  out->clear();
  if (row >= rows.count()) return true;
  const ProcessRow& r = rows[row];
  switch (column) {
    case 0: return out->append_uint(r.id);
    case 1: {
      uint64_t length = 0;
      while (length < sizeof r.name && r.name[length]) ++length;
      return out->assign(StringView(r.name, length));
    }
    case 2: return out->assign(r.windows ? "Windows" : "Linux");
    case 3: return out->assign(r.state == TaskState::Running ? "Running" : r.state == TaskState::Blocked ? "Blocked" : "Ready");
    case 4: return out->append_uint(r.threads);
    case 5: return append_tenths(out, r.tenths);
    case 6: return format_size(r.memory, out);
    default: return true;
  }
}

ComputerManager::ComputerManager(allocators::IAllocator& allocator)
    : Frame(allocator),
      menu_(allocator),
      grid_menu_(allocator),
      process_grid_menu_(allocator),
      tabs_(allocator),
      core_scroll_(allocator),
      processes_page_(allocator),
      performance_page_(allocator),
      hardware_page_(allocator),
      usermode_page_(allocator),
      cpu_row_(allocator),
      memory_row_(allocator),
      cpu_title_(allocator),
      cpu_value_(allocator),
      memory_title_(allocator),
      memory_value_(allocator),
      info_(allocator),
      tree_(allocator),
      display_menu_(allocator),
      adapter_menu_(allocator),
      wireless_menu_(allocator),
      device_menu_(allocator),
      monitor_menu_(allocator),
      bluetooth_menu_(allocator),
      hardware_info_(allocator),
      cpu_meter_(allocator),
      memory_meter_(allocator),
      grid_(allocator),
      process_grid_(allocator),
      gpu_row_(allocator),
      npu_row_(allocator),
      vram_row_(allocator),
      cpu_temp_row_(allocator),
      gpu_temp_row_(allocator),
      fan_row_(allocator),
      battery_row_(allocator),
      core_rows_(allocator),
      model_(allocator),
      process_model_(allocator),
      previous_cores_(allocator),
      previous_threads_(allocator),
      previous_processes_(allocator) {
  buffer_ = allocate_view<TaskThread>(allocator, kMaxThreads);
  process_buffer_ = allocate_view<TaskProcess>(allocator, kMaxProcesses);
  for (int32_t& node : group_nodes_) node = -1;
  set_title("Computer Manager");
  set_bounds({90, 40, 480, 544});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) file->add_item("Exit", {&ComputerManager::on_close_window, this});
  if (Menu* edit = menu_.add_menu("Edit")) edit_copy_item_ = edit->add_item("Copy Row", {&ComputerManager::on_edit_copy, this}, "Ctrl+C");
  if (Menu* view = menu_.add_menu("View")) {      // (in the order of the tabs)
    processes_item_ = view->add_item("Threads", {&ComputerManager::on_show_processes, this});
    performance_item_ = view->add_item("Performance", {&ComputerManager::on_show_performance, this});
    hardware_item_ = view->add_item("Hardware", {&ComputerManager::on_show_hardware, this});
    usermode_item_ = view->add_item("Programs", {&ComputerManager::on_show_usermode, this});
  }
  // The commands of the selected device: what its context menu has, in the same order, Properties last.
  if (Menu* device = menu_.add_menu("Device")) {
    menu_display_item_ = device->add_item("Display Settings", {&ComputerManager::on_display_settings_item, this});
    menu_wireless_item_ = device->add_item("Wi-Fi Networks", {&ComputerManager::on_wireless_item, this});
    menu_bluetooth_item_ = device->add_item("Bluetooth Devices", {&ComputerManager::on_bluetooth_item, this});
    device->add_separator();
    menu_properties_item_ = device->add_item("Properties", {&ComputerManager::on_any_properties, this}, "Alt+Enter");
  }
  if (Menu* help = menu_.add_menu("Help")) about_item_ = help->add_item("About Computer Manager", {&ComputerManager::on_about, this});
  copy_row_item_ = grid_menu_.add_item("Copy Row", {&ComputerManager::on_copy_row, this});
  grid_menu_.on_opening(Handler::immediate(&ComputerManager::on_grid_menu_opening, this));
  copy_process_row_item_ = process_grid_menu_.add_item("Copy Row", {&ComputerManager::on_copy_process_row, this});
  process_grid_menu_.on_opening(Handler::immediate(&ComputerManager::on_process_menu_opening, this));
  tabs_.set_dock(DockStyle::Fill);
  tabs_.on_selection_changed(Handler::immediate(&ComputerManager::on_tab_changed, this));
  processes_page_.set_style(PanelStyle::Transparent);
  processes_page_.set_padding(Insets::all(4));
  performance_page_.set_style(PanelStyle::Transparent);
  performance_page_.set_padding(Insets::all(6));
  usermode_page_.set_style(PanelStyle::Transparent);
  usermode_page_.set_padding(Insets::all(4));

  struct RowParts {
    Panel& row;
    Label& title;
    Label& value;
    LevelMeter& meter;
    const char* text;
  };
  const RowParts rows[2] = {{cpu_row_, cpu_title_, cpu_value_, cpu_meter_, "Processor"}, {memory_row_, memory_title_, memory_value_, memory_meter_, "Memory"}};
  for (const RowParts& r : rows) {
    r.row.set_style(PanelStyle::Transparent);
    r.row.set_dock(DockStyle::Top);
    r.row.set_size(0, 24);
    r.title.set_text(r.text);
    r.title.set_dock(DockStyle::Left);
    r.title.set_size(kTitleWidth, 0);
    r.value.set_dock(DockStyle::Right);
    r.value.set_size(170, 0);
    r.value.set_text("-");
    r.meter.set_peak_hold(false);
    r.meter.set_dock(DockStyle::Fill);
    r.row.add(&r.title);
    r.row.add(&r.value);
    r.row.add(&r.meter);
    performance_page_.add(&r.row);
  }
  setup_stat_row(gpu_row_, "Graphics");
  setup_stat_row(npu_row_, "AI processor");
  npu_row_.row.set_visible(false);   // (until the machine turns out to have one)
  setup_stat_row(vram_row_, "Graphics memory");
  setup_stat_row(cpu_temp_row_, "CPU temperature");
  setup_stat_row(gpu_temp_row_, "GPU temperature");
  setup_stat_row(fan_row_, "Fan speed");
  fan_row_.row.set_visible(false);   // (until the board's sensor chip reports a fan)
  setup_stat_row(battery_row_, "Battery");
  cpu_temp_row_.meter.set_zones(ZoneDirection::HighIsBad, 70, 85);   // degrees Celsius
  gpu_temp_row_.meter.set_zones(ZoneDirection::HighIsBad, 70, 85);
  battery_row_.meter.set_zones(ZoneDirection::LowIsBad, 15, 40);     // percent: red when nearly empty, green when well charged
  info_.set_dock(DockStyle::Top);
  info_.set_size(0, 20);
  performance_page_.add(&info_);
  core_scroll_.set_dock(DockStyle::Fill);   // the cores, one row each, scrolled when there are more than fit
  performance_page_.add(&core_scroll_);

  grid_.add_column("ID", 44, Align::Right);
  grid_.add_column("Thread", 150);
  grid_.add_column("State", 70);
  grid_.add_column("Core", 44, Align::Right);
  grid_.add_column("CPU %", 56, Align::Right);
  grid_.add_column("CPU s", 70, Align::Right);
  grid_.set_model(&model_);
  grid_.set_dock(DockStyle::Fill);
  grid_.set_context_menu(&grid_menu_);
  grid_.on_selection_changed(Handler::immediate(&ComputerManager::on_grid_selection, this));

  process_grid_.add_column("PID", 40, Align::Right);
  process_grid_.add_column("Program", 106);
  process_grid_.add_column("Type", 54);
  process_grid_.add_column("State", 62);
  process_grid_.add_column("Threads", 62, Align::Right);
  process_grid_.add_column("CPU %", 48, Align::Right);
  process_grid_.add_column("Memory", 70, Align::Right);
  process_grid_.set_model(&process_model_);
  process_grid_.set_dock(DockStyle::Fill);
  process_grid_.set_context_menu(&process_grid_menu_);
  process_grid_.on_selection_changed(Handler::immediate(&ComputerManager::on_grid_selection, this));
  usermode_page_.add(&process_grid_);

  // The Hardware tab: the tree, and a line about the selected device under it. The Displays group has a menu of its own.
  hardware_page_.set_style(PanelStyle::Transparent);
  hardware_page_.set_padding(Insets::all(4));
  hardware_info_.set_dock(DockStyle::Bottom);
  hardware_info_.set_size(0, 22);
  tree_.set_dock(DockStyle::Fill);
  tree_.on_selection_changed(Handler::immediate(&ComputerManager::on_hardware_selection, this));
  tree_.set_context_menu(&display_menu_);   // (the nodes that have a menu say which; see TreeView)
  // The context menus: the commands of the device first, Properties last. None ends in "...": each opens a window and asks nothing first.
  display_settings_item_ = display_menu_.add_item("Display Settings", {&ComputerManager::on_display_settings_item, this});
  properties_item_ = adapter_menu_.add_item("Properties", {&ComputerManager::on_properties_item, this});
  wireless_properties_item_ = wireless_menu_.add_item("Wi-Fi Networks", {&ComputerManager::on_wireless_item, this});
  wireless_menu_.add_separator();
  wireless_adapter_item_ = wireless_menu_.add_item("Properties", {&ComputerManager::on_properties_item, this});
  device_properties_item_ = device_menu_.add_item("Properties", {&ComputerManager::on_device_item, this});
  monitor_settings_item_ = monitor_menu_.add_item("Display Settings", {&ComputerManager::on_display_settings_item, this});
  monitor_menu_.add_separator();
  monitor_properties_item_ = monitor_menu_.add_item("Properties", {&ComputerManager::on_device_item, this});
  bluetooth_settings_item_ = bluetooth_menu_.add_item("Bluetooth Devices", {&ComputerManager::on_bluetooth_item, this});
  bluetooth_menu_.add_separator();
  bluetooth_properties_item_ = bluetooth_menu_.add_item("Properties", {&ComputerManager::on_device_item, this});
  hardware_page_.add(&hardware_info_);
  hardware_page_.add(&tree_);

  processes_page_.add(&grid_);
  tabs_.add_page("Threads", &processes_page_);
  tabs_.add_page("Performance", &performance_page_);
  tabs_.add_page("Hardware", &hardware_page_);
  tabs_.add_page("Programs", &usermode_page_);
  add(&menu_);
  add(&tabs_);
  on_tab_changed(this, tabs_);
}

LevelMeter* ComputerManager::core_meter(uint32_t index) {
  return index < core_rows_.count() && core_rows_[index]->row.visible() ? &core_rows_[index]->meter : nullptr;
}

Label* ComputerManager::core_label(uint32_t index) {
  return index < core_rows_.count() && core_rows_[index]->row.visible() ? &core_rows_[index]->value : nullptr;
}

void ComputerManager::on_tab_changed(void* self, Control&) {  // the View menu shows which tab is up
  auto* t = static_cast<ComputerManager*>(self);
  if (t->processes_item_) t->processes_item_->set_checked(t->tabs_.selected_page() == 0);
  if (t->performance_item_) t->performance_item_->set_checked(t->tabs_.selected_page() == 1);
  if (t->hardware_item_) t->hardware_item_->set_checked(t->tabs_.selected_page() == 2);
  if (t->usermode_item_) t->usermode_item_->set_checked(t->tabs_.selected_page() == 3);
  t->update_commands();
}

DataGrid* ComputerManager::shown_grid() { return tabs_.selected_page() == 0 ? &grid_ : tabs_.selected_page() == 3 ? &process_grid_ : nullptr; }

// What cannot be done now is unavailable, not answered with a message: Copy Row needs a selected row of the table that is up, the Device commands
// the kind of device they are for, selected in the tree of the Hardware tab.
void ComputerManager::update_commands() {
  DataGrid* grid = shown_grid();
  if (edit_copy_item_) edit_copy_item_->set_enabled(grid && grid->selected_row() >= 0);
  if (about_item_) about_item_->set_enabled(notices_ != nullptr);
  const bool hardware = tabs_.selected_page() == 2;
  const int32_t node = hardware ? tree_.selected() : -1;
  HardwareEntry e;
  const bool device = hardware && selected_entry(&e);
  const bool displays_group = node >= 0 && tree_.node_tag(node) == (0x10000u | static_cast<uint32_t>(HardwareGroup::Displays));
  if (menu_display_item_) menu_display_item_->set_enabled(displays_group || (device && e.group == HardwareGroup::Displays));
  if (menu_wireless_item_) menu_wireless_item_->set_enabled(device && e.group == HardwareGroup::Network && e.wireless);
  if (menu_bluetooth_item_) menu_bluetooth_item_->set_enabled(device && e.group == HardwareGroup::Bluetooth && e.bluetooth);
  if (menu_properties_item_) menu_properties_item_->set_enabled(device);
}

void ComputerManager::on_grid_selection(void* self, Control&) { static_cast<ComputerManager*>(self)->update_commands(); }

void ComputerManager::on_edit_copy(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  if (DataGrid* grid = t->shown_grid()) grid->copy_selected_row();
}

// Properties of the selected device: a network adapter has a sheet of its own, every other device the common one.
void ComputerManager::on_any_properties(void* self, Control& sender) {
  auto* t = static_cast<ComputerManager*>(self);
  HardwareEntry e;
  if (t->tabs_.selected_page() != 2 || !t->selected_entry(&e)) return;
  if (e.group == HardwareGroup::Network) on_properties_item(self, sender);
  else on_device_item(self, sender);
}

void ComputerManager::on_about(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  NoticeDialog::post(t->notices_, t->manager(), NoticeKind::Information,
                     "Computer Manager shows what the computer is doing and what it is made of: the running threads and programs, the load, and the devices.",
                     "About Computer Manager");
}

bool ComputerManager::DeviceTree::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter && (args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt))) return false;
  return TreeView::on_key(args);
}

// Alt+Enter: Properties of the selected device.
bool ComputerManager::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter && (args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt))) {
    on_any_properties(this, *this);
    return true;
  }
  return Frame::on_key(args);
}
void ComputerManager::on_show_processes(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(0); }
void ComputerManager::setup_stat_row(CoreRow& r, const char* title) {
  r.row.set_style(PanelStyle::Transparent);
  r.row.set_dock(DockStyle::Top);
  r.row.set_size(0, 24);
  r.title.set_text(title);
  r.title.set_dock(DockStyle::Left);
  r.title.set_size(kTitleWidth, 0);
  r.value.set_dock(DockStyle::Right);
  r.value.set_size(170, 0);
  r.value.set_text(kNotAvailable);
  r.meter.set_peak_hold(false);
  r.meter.set_dock(DockStyle::Fill);
  r.row.add(&r.title);
  r.row.add(&r.value);
  r.row.add(&r.meter);
  performance_page_.add(&r.row);
}

void ComputerManager::on_show_performance(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(1); }
void ComputerManager::on_show_hardware(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(2); }
void ComputerManager::on_show_usermode(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(3); }
void ComputerManager::on_display_settings_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  t->fire(t->open_display_settings_);
}
bool ComputerManager::selected_entry(HardwareEntry* out) const {
  const int32_t node = tree_.selected();
  return node >= 0 && inventory_ && !(tree_.node_tag(node) & 0x10000u) && inventory_->entry(tree_.node_tag(node), out);
}
bool ComputerManager::selected_adapter(uint32_t* id) const {
  HardwareEntry e;
  if (!selected_entry(&e)) return false;
  *id = e.id;
  return true;
}
void ComputerManager::on_device_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  HardwareEntry e;
  if (!t->selected_entry(&e)) return;
  t->device_group_ = e.group;
  t->properties_adapter_ = e.id;
  t->fire(t->open_device_properties_);
}
void ComputerManager::on_properties_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  if (!t->selected_adapter(&t->properties_adapter_)) return;
  t->fire(t->open_network_properties_);
}
void ComputerManager::on_wireless_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  if (!t->selected_adapter(&t->properties_adapter_)) return;
  t->fire(t->open_wireless_properties_);
}
void ComputerManager::on_bluetooth_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  if (!t->selected_adapter(&t->properties_adapter_)) return;
  t->fire(t->open_bluetooth_properties_);
}
void ComputerManager::on_copy_row(void* self, Control&) { static_cast<ComputerManager*>(self)->grid_.copy_selected_row(); }
void ComputerManager::on_grid_menu_opening(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  t->copy_row_item_->set_enabled(t->grid_.selected_row() >= 0);
}
void ComputerManager::on_copy_process_row(void* self, Control&) { static_cast<ComputerManager*>(self)->process_grid_.copy_selected_row(); }
void ComputerManager::on_process_menu_opening(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  t->copy_process_row_item_->set_enabled(t->process_grid_.selected_row() >= 0);
}
void ComputerManager::on_close_window(void* self, Control&) { static_cast<ComputerManager*>(self)->close(); }

// --- The Hardware tab ---------------------------------------------------------------------------------

void ComputerManager::set_hardware_inventory(IHardwareInventory* inventory) {
  UiGuard lock(this);
  inventory_ = inventory;
  if (inventory_) inventory_->refresh();
  tree_.clear();
  rebuild_hardware();
}

int32_t ComputerManager::hardware_group_node(HardwareGroup group) const { return group_nodes_[static_cast<uint32_t>(group)]; }

// Fills the tree from the inventory: the groups that have devices, in a fixed order, each with its devices. What was open and what was selected
// stays so (the first time, every group is open).
void ComputerManager::rebuild_hardware() {
  UiGuard lock(this);
  const bool first = tree_.node_count() == 0;
  uint32_t open_mask = 0, had_mask = 0;   // the groups that were open, and those that were there at all
  for (uint32_t g = 0; g < kHardwareGroupCount; ++g)
    if (group_nodes_[g] >= 0) {
      had_mask |= 1u << g;
      if (tree_.expanded(group_nodes_[g])) open_mask |= 1u << g;
    }
  String selected_name(allocator());
  uint32_t selected_tag = ~0u;
  if (tree_.selected() >= 0) {
    selected_name.assign(tree_.node_text(tree_.selected()));
    selected_tag = tree_.node_tag(tree_.selected());
  }
  tree_.clear();
  for (int32_t& node : group_nodes_) node = -1;
  if (!inventory_) {
    hardware_info_.set_text("No hardware information is available.");
    return;
  }
  const uint32_t count = inventory_->count();
  for (uint32_t g = 0; g < kHardwareGroupCount; ++g) {
    int32_t node = -1;
    for (uint32_t i = 0; i < count; ++i) {
      HardwareEntry e;
      if (!inventory_->entry(i, &e) || static_cast<uint32_t>(e.group) != g) continue;
      if (node < 0) {
        node = tree_.add_node(-1, hardware_group_name(static_cast<HardwareGroup>(g)), 0x10000u | g, ItemIcon::Folder);
        if (node < 0) break;
        group_nodes_[g] = node;
      }
      const int32_t device = tree_.add_node(node, e.name, i, g == static_cast<uint32_t>(HardwareGroup::Storage) ? ItemIcon::Drive : ItemIcon::File);
      if (device < 0) continue;
      // Every device has Properties. A network adapter's is its own window (and a Wi-Fi adapter has Wi-Fi Properties too); a Bluetooth adapter also has Bluetooth Settings; a display also leads
      // to Display Settings; all the others share the generic one, whatever their driver.
      if (g == static_cast<uint32_t>(HardwareGroup::Network)) tree_.set_node_menu(device, e.wireless ? &wireless_menu_ : &adapter_menu_);
      else if (g == static_cast<uint32_t>(HardwareGroup::Bluetooth) && e.bluetooth) tree_.set_node_menu(device, &bluetooth_menu_);
      else tree_.set_node_menu(device, g == static_cast<uint32_t>(HardwareGroup::Displays) ? &monitor_menu_ : &device_menu_);
    }
    if (node < 0) continue;
    if (first || !(had_mask & (1u << g)) || (open_mask & (1u << g))) tree_.set_expanded(node, true);   // (a group that turns up is open)
    if (g == static_cast<uint32_t>(HardwareGroup::Displays)) tree_.set_node_menu(node, &display_menu_);   // (Display Settings)
  }
  // The selection: the same device, or the same group.
  for (uint32_t n = 0; selected_tag != ~0u && n < tree_.node_count(); ++n) {
    const bool group = (selected_tag & 0x10000u) != 0;
    if ((group && tree_.node_tag(static_cast<int32_t>(n)) == selected_tag) ||
        (!group && tree_.node_tag(static_cast<int32_t>(n)) < 0x10000u && tree_.node_text(static_cast<int32_t>(n)).equals(selected_name.view()))) {
      tree_.select(static_cast<int32_t>(n));
      break;
    }
  }
  if (tree_.selected() < 0 && tree_.node_count()) tree_.select(0);
  on_hardware_selection(this, tree_);
}

void ComputerManager::on_hardware_selection(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  const int32_t node = t->tree_.selected();
  t->update_commands();
  if (node < 0 || !t->inventory_) {
    t->hardware_info_.set_text(StringView());
    return;
  }
  const uint32_t tag = t->tree_.node_tag(node);
  String text(t->allocator());
  if (tag & 0x10000u) {
    String count(t->allocator());
    count.append_uint(t->tree_.child_count(node));
    const bool one = t->tree_.child_count(node) == 1;
    if ((tag & 0xFFFFu) == static_cast<uint32_t>(HardwareGroup::Displays))   // (where the command is, for whoever does not think of the context menu)
      format_message(&text, one ? "%1 device. Display Settings is in the Device menu." : "%1 devices. Display Settings is in the Device menu.", count.view());
    else
      format_message(&text, one ? "%1 device" : "%1 devices", count.view());
  } else {
    HardwareEntry e;
    if (t->inventory_->entry(tag, &e)) text.assign(e.detail[0] ? StringView(e.detail) : StringView(e.name));
  }
  t->hardware_info_.set_text(text.view());
}

ComputerManager::~ComputerManager() {
  release_view(allocator(), buffer_);
  release_view(allocator(), process_buffer_);
}

void ComputerManager::set_monitor(ISystemMonitor* monitor) {
  monitor_ = monitor;
  have_previous_ = false;
  previous_cores_.clear();
  previous_threads_.clear();
  previous_processes_.clear();
  model_.ticks_per_second = monitor ? monitor->ticks_per_second() : 1000;
}

int32_t ComputerManager::thread_load_tenths(uint64_t id) const {
  for (const Row& r : model_.rows)
    if (r.id == id && !r.idle) return static_cast<int32_t>(r.tenths);
  return -1;
}

int32_t ComputerManager::process_load_tenths(uint64_t id) const {
  for (const ProcessRow& r : process_model_.rows)
    if (r.id == id) return static_cast<int32_t>(r.tenths);
  return -1;
}

void ComputerManager::update(uint64_t now_us) {
  UiGuard lock(this);
  if (inventory_ && inventory_->refresh()) rebuild_hardware();
  if (!monitor_ || buffer_.empty()) {
    info_.set_text("No information about the system is available.");
    return;
  }
  const uint64_t hz = monitor_->ticks_per_second() ? monitor_->ticks_per_second() : 1;
  const uint64_t elapsed_us = have_previous_ && now_us > last_us_ ? now_us - last_us_ : 0;

  // --- Processor: busy share per core since the last call, and over all online cores.
  const uint32_t cores = monitor_->core_count();
  List<TaskCore> current(allocator());
  uint64_t all_busy = 0, all_total = 0;
  String text(allocator());
  uint32_t online = 0;
  for (uint32_t i = 0; i < cores; ++i) {
    TaskCore core;
    if (!monitor_->core(i, &core)) core = TaskCore();
    current.push_back(core);
    uint64_t busy = 0, idle = 0;
    if (core.online && have_previous_ && i < previous_cores_.count()) {
      busy = core.busy_ticks - previous_cores_[i].busy_ticks;
      idle = core.idle_ticks - previous_cores_[i].idle_ticks;
    }
    if (core.online) {
      ++online;
      all_busy += busy;
      all_total += busy + idle;
    }
    if (i >= kMaxCoresShown) continue;
    while (core_rows_.count() <= i) {  // the rows of the Performance tab are made as cores turn up
      UniquePointer<CoreRow> row = UniquePointer<CoreRow>::make(allocator(), allocator());
      if (!row) break;
      row->row.set_style(PanelStyle::Transparent);
      row->row.set_dock(DockStyle::Top);
      row->row.set_size(0, 22);
      row->title.set_dock(DockStyle::Left);
      row->title.set_size(kTitleWidth, 0);
      row->value.set_dock(DockStyle::Right);
      row->value.set_size(50, 0);
      row->meter.set_peak_hold(false);
      row->meter.set_dock(DockStyle::Fill);
      row->row.add(&row->title);
      row->row.add(&row->value);
      row->row.add(&row->meter);
      row->row.set_visible(false);
      core_scroll_.add(&row->row);
      core_rows_.push_back(util::move(row));
    }
    if (i >= core_rows_.count()) continue;
    CoreRow& row = *core_rows_[i];
    row.row.set_visible(core.online);
    if (!core.online) continue;
    const uint64_t percent = busy + idle ? busy * 100 / (busy + idle) : 0;
    String number(allocator());
    number.append_uint(i);
    format_message(&text, core.efficiency ? "Core %1 (efficiency)" : "Core %1", number.view());
    row.title.set_text(text.view());
    row.meter.set_level(static_cast<int32_t>(percent));
    text.assign("");
    text.append_uint(percent);
    text.append("%");
    row.value.set_text(text.view());
  }
  const uint64_t cpu_percent = all_total ? all_busy * 100 / all_total : 0;
  cpu_meter_.set_level(static_cast<int32_t>(cpu_percent));
  text.clear();
  text.append_uint(cpu_percent);
  text.append("%");
  cpu_value_.set_text(text.view());

  // --- Memory.
  const TaskMemory memory = monitor_->memory();
  const uint64_t used = memory.total_bytes > memory.free_bytes ? memory.total_bytes - memory.free_bytes : 0;
  memory_meter_.set_level(memory.total_bytes ? static_cast<int32_t>(used * 100 / memory.total_bytes) : 0);
  String used_text(allocator()), total_text(allocator());
  format_size(used, &used_text);
  format_size(memory.total_bytes, &total_text);
  text.assign(used_text.view());
  text.append(" / ");
  text.append(total_text.view());
  memory_value_.set_text(text.view());

  // --- Graphics adapter and temperatures; whatever the machine cannot tell shows as n/a.
  const TaskGpu gpu = monitor_->gpu();
  auto show_percent = [&](CoreRow& row, int32_t percent) {
    row.meter.set_level(percent < 0 ? 0 : percent > 100 ? 100 : percent);
    text.clear();
    if (percent < 0) text.assign(kNotAvailable);
    else { text.append_uint(static_cast<uint64_t>(percent)); text.append("%"); }
    row.value.set_text(text.view());
  };
  auto show_temperature = [&](CoreRow& row, int32_t millicelsius) {
    row.meter.set_level(millicelsius < 0 ? 0 : millicelsius >= 100000 ? 100 : millicelsius / 1000);
    text.clear();
    if (millicelsius < 0) {
      text.assign(kNotAvailable);
    } else {
      text.append_uint(static_cast<uint64_t>(millicelsius) / 1000);
      text.push_back('.');
      text.append_uint(static_cast<uint64_t>(millicelsius) / 100 % 10);
      text.append(" C");
    }
    row.value.set_text(text.view());
  };
  show_percent(gpu_row_, gpu.usage_percent);
  // The NPU: how much of the time since the last call it spent computing. The row is there only when the machine has one.
  const TaskNpu npu = monitor_->npu();
  npu_row_.row.set_visible(npu.present);
  if (npu.present) {
    int32_t npu_percent = -1;
    if (had_npu_ && elapsed_us > 0 && npu.busy_us >= previous_npu_busy_us_) {
      const uint64_t percent = (npu.busy_us - previous_npu_busy_us_) * 100 / elapsed_us;
      npu_percent = static_cast<int32_t>(percent > 100 ? 100 : percent);
    }
    show_percent(npu_row_, npu_percent);
  }
  had_npu_ = npu.present;
  previous_npu_busy_us_ = npu.busy_us;
  if (gpu.memory_known && !gpu.memory_total_bytes) {   // the adapter knows what is in use but not what there is
    vram_row_.meter.set_level(0);
    format_size(gpu.memory_used_bytes, &used_text);
    format_message(&text, "%1 used", used_text.view());
    vram_row_.value.set_text(text.view());
  } else if (gpu.memory_known) {
    vram_row_.meter.set_level(static_cast<int32_t>(gpu.memory_used_bytes * 100 / gpu.memory_total_bytes));
    format_size(gpu.memory_used_bytes, &used_text);
    format_size(gpu.memory_total_bytes, &total_text);
    text.assign(used_text.view());
    text.append(" / ");
    text.append(total_text.view());
    vram_row_.value.set_text(text.view());
  } else {
    vram_row_.meter.set_level(0);
    vram_row_.value.set_text(kNotAvailable);
  }
  show_temperature(cpu_temp_row_, monitor_->cpu_temperature_millicelsius());
  show_temperature(gpu_temp_row_, gpu.temperature_millicelsius);
  // The fan: the meter is full at 3000 RPM, about what a CPU cooler's fan runs at flat out.
  const int32_t rpm = monitor_->fan_rpm();
  fan_row_.row.set_visible(rpm >= 0);
  if (rpm >= 0) {
    fan_row_.meter.set_level(rpm >= 3000 ? 100 : rpm / 30);
    text.clear();
    text.append_uint(static_cast<uint64_t>(rpm));
    text.append(" RPM");
    fan_row_.value.set_text(text.view());
  }

  const TaskBattery battery = monitor_->battery();
  if (!battery.present) {
    battery_row_.meter.set_level(0);
    battery_row_.value.set_text("No battery");
  } else if (battery.percent < 0) {
    battery_row_.meter.set_level(0);
    battery_row_.value.set_text(kNotAvailable);
  } else {
    const int32_t percent = battery.percent > 100 ? 100 : battery.percent;
    battery_row_.meter.set_level(percent);
    String level(allocator());
    level.append_uint(static_cast<uint64_t>(percent));
    const char* pattern = battery.state == ChargeState::Charging      ? "%1% - Charging"
                          : battery.state == ChargeState::Discharging ? "%1% - Discharging"
                          : battery.state == ChargeState::Full        ? "%1% - Full"
                          : battery.state == ChargeState::NotCharging ? "%1% - Not charging"
                                                                      : "%1%";
    format_message(&text, pattern, level.view());
    battery_row_.value.set_text(text.view());
  }

  // --- Threads: load since the last call, in tenths of a percent of one core.
  const uint32_t count = monitor_->threads(buffer_.data(), kMaxThreads);
  const uint64_t selected_id = grid_.selected_row() >= 0 && static_cast<uint32_t>(grid_.selected_row()) < model_.rows.count()
                                   ? model_.rows[static_cast<uint32_t>(grid_.selected_row())].id
                                   : ~0ull;
  const uint64_t period_ticks = elapsed_us * hz / 1000000;  // ticks one core counts meanwhile
  model_.rows.clear();
  List<Previous> next_previous(allocator());
  int32_t reselect = -1;
  uint32_t shown_threads = 0;
  for (uint32_t i = 0; i < count && i < kMaxThreads; ++i) {
    const TaskThread& t = buffer_[i];
    Row row;
    row.id = t.id;
    __builtin_memcpy(row.name, t.name, sizeof row.name);
    row.name[sizeof row.name - 1] = 0;
    row.state = t.state;
    row.idle = t.idle;
    row.core = t.core;
    row.ticks = t.cpu_ticks;
    uint64_t before = t.cpu_ticks;
    bool known = false;
    for (const Previous& p : previous_threads_)
      if (p.id == t.id && !t.idle) {
        before = p.ticks;
        known = true;
        break;
      }
    if (have_previous_ && period_ticks && (known || t.idle)) {
      const uint64_t delta = t.idle ? 0 : t.cpu_ticks - before;
      row.tenths = static_cast<uint32_t>(delta * 1000 / period_ticks);
    }
    if (!t.idle) next_previous.push_back(Previous{t.id, t.cpu_ticks});
    if (!t.idle && t.id == selected_id) reselect = static_cast<int32_t>(model_.rows.count());
    model_.rows.push_back(row);
    ++shown_threads;
  }
  previous_threads_.clear();
  for (const Previous& p : next_previous) previous_threads_.push_back(p);

  // --- User-mode programs: the same, for all the threads of a program together.
  const uint32_t program_count = process_buffer_.empty() ? 0 : monitor_->processes(process_buffer_.data(), kMaxProcesses);
  const int32_t selected_program = process_grid_.selected_row();
  const uint64_t selected_pid = selected_program >= 0 && static_cast<uint32_t>(selected_program) < process_model_.rows.count()
                                    ? process_model_.rows[static_cast<uint32_t>(selected_program)].id
                                    : ~0ull;
  process_model_.rows.clear();
  List<Previous> next_processes(allocator());
  int32_t reselect_program = -1;
  for (uint32_t i = 0; i < program_count && i < kMaxProcesses; ++i) {
    const TaskProcess& p = process_buffer_[i];
    ProcessRow row;
    row.id = p.id;
    __builtin_memcpy(row.name, p.name, sizeof row.name);
    row.name[sizeof row.name - 1] = 0;
    row.windows = p.windows;
    row.state = p.state;
    row.threads = p.threads;
    row.memory = p.memory_bytes;
    if (have_previous_ && period_ticks)
      for (const Previous& before : previous_processes_)
        if (before.id == p.id) {
          row.tenths = static_cast<uint32_t>((p.cpu_ticks > before.ticks ? p.cpu_ticks - before.ticks : 0) * 1000 / period_ticks);
          break;
        }
    next_processes.push_back(Previous{p.id, p.cpu_ticks});
    if (p.id == selected_pid) reselect_program = static_cast<int32_t>(process_model_.rows.count());
    process_model_.rows.push_back(row);
  }
  previous_processes_.clear();
  for (const Previous& p : next_processes) previous_processes_.push_back(p);
  process_grid_.model_changed();
  process_grid_.select_row(reselect_program);
  previous_cores_.clear();
  for (const TaskCore& c : current) previous_cores_.push_back(c);
  last_us_ = now_us;
  have_previous_ = true;
  grid_.model_changed();
  grid_.select_row(reselect);
  update_commands();

  // --- Summary line.
  String threads_text(allocator()), cores_text(allocator()), uptime(allocator());
  threads_text.append_uint(shown_threads);
  cores_text.append_uint(online);
  const uint64_t seconds = now_us / 1000000;
  auto two = [&](uint64_t v) {
    uptime.push_back(static_cast<char>('0' + (v / 10) % 10));
    uptime.push_back(static_cast<char>('0' + v % 10));
  };
  two(seconds / 3600);
  uptime.push_back(':');
  two(seconds / 60 % 60);
  uptime.push_back(':');
  two(seconds % 60);
  format_message(&text, "Threads: %1   Cores: %2   Uptime: %3", threads_text.view(), cores_text.view(), uptime.view());
  info_.set_text(text.view());
}

}  // namespace desktop
