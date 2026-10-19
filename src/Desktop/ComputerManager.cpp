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

ComputerManager::ComputerManager(allocators::IAllocator& allocator)
    : Frame(allocator),
      menu_(allocator),
      grid_menu_(allocator),
      tabs_(allocator),
      processes_page_(allocator),
      performance_page_(allocator),
      hardware_page_(allocator),
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
      hardware_info_(allocator),
      cpu_meter_(allocator),
      memory_meter_(allocator),
      grid_(allocator),
      core_rows_(allocator),
      model_(allocator),
      previous_cores_(allocator),
      previous_threads_(allocator) {
  buffer_ = allocate_view<TaskThread>(allocator, kMaxThreads);
  for (int32_t& node : group_nodes_) node = -1;
  set_title("Computer Manager");
  set_bounds({90, 40, 480, 430});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) file->add_item("Close", {&ComputerManager::on_close_window, this});
  if (Menu* view = menu_.add_menu("View")) {
    processes_item_ = view->add_item("Processes", {&ComputerManager::on_show_processes, this});
    performance_item_ = view->add_item("Performance", {&ComputerManager::on_show_performance, this});
    hardware_item_ = view->add_item("Hardware", {&ComputerManager::on_show_hardware, this});
  }
  copy_row_item_ = grid_menu_.add_item("Copy Row", {&ComputerManager::on_copy_row, this}, "Ctrl+C");
  grid_menu_.on_opening(Handler::immediate(&ComputerManager::on_grid_menu_opening, this));
  tabs_.set_dock(DockStyle::Fill);
  tabs_.on_selection_changed(Handler::immediate(&ComputerManager::on_tab_changed, this));
  processes_page_.set_style(PanelStyle::Transparent);
  processes_page_.set_padding(Insets::all(4));
  performance_page_.set_style(PanelStyle::Transparent);
  performance_page_.set_padding(Insets::all(6));

  struct RowParts {
    Panel& row;
    Label& title;
    Label& value;
    LevelMeter& meter;
    const char* text;
  };
  const RowParts rows[2] = {{cpu_row_, cpu_title_, cpu_value_, cpu_meter_, "CPU"}, {memory_row_, memory_title_, memory_value_, memory_meter_, "Memory"}};
  for (const RowParts& r : rows) {
    r.row.set_style(PanelStyle::Transparent);
    r.row.set_dock(DockStyle::Top);
    r.row.set_size(0, 24);
    r.title.set_text(r.text);
    r.title.set_dock(DockStyle::Left);
    r.title.set_size(64, 0);
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
  info_.set_dock(DockStyle::Top);
  info_.set_size(0, 20);
  performance_page_.add(&info_);

  grid_.add_column("ID", 44, Align::Right);
  grid_.add_column("Thread", 150);
  grid_.add_column("State", 70);
  grid_.add_column("Core", 44, Align::Right);
  grid_.add_column("CPU %", 56, Align::Right);
  grid_.add_column("CPU s", 70, Align::Right);
  grid_.set_model(&model_);
  grid_.set_dock(DockStyle::Fill);
  grid_.set_context_menu(&grid_menu_);

  // The Hardware tab: the tree, and a line about the selected device under it. The Displays group has a menu of its own.
  hardware_page_.set_style(PanelStyle::Transparent);
  hardware_page_.set_padding(Insets::all(4));
  hardware_info_.set_dock(DockStyle::Bottom);
  hardware_info_.set_size(0, 22);
  tree_.set_dock(DockStyle::Fill);
  tree_.on_selection_changed(Handler::immediate(&ComputerManager::on_hardware_selection, this));
  tree_.set_context_menu(&display_menu_);   // (the nodes that have a menu say which; see TreeView)
  display_settings_item_ = display_menu_.add_item("Display Settings...", {&ComputerManager::on_display_settings_item, this});
  properties_item_ = adapter_menu_.add_item("Properties...", {&ComputerManager::on_properties_item, this});
  hardware_page_.add(&hardware_info_);
  hardware_page_.add(&tree_);

  processes_page_.add(&grid_);
  tabs_.add_page("Processes", &processes_page_);
  tabs_.add_page("Performance", &performance_page_);
  tabs_.add_page("Hardware", &hardware_page_);
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
}
void ComputerManager::on_show_processes(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(0); }
void ComputerManager::on_show_performance(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(1); }
void ComputerManager::on_show_hardware(void* self, Control&) { static_cast<ComputerManager*>(self)->tabs_.select_page(2); }
void ComputerManager::on_display_settings_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  t->fire(t->open_display_settings_);
}
void ComputerManager::on_properties_item(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  const int32_t node = t->tree_.selected();
  HardwareEntry e;
  if (node < 0 || !t->inventory_ || (t->tree_.node_tag(node) & 0x10000u) || !t->inventory_->entry(t->tree_.node_tag(node), &e)) return;
  t->properties_adapter_ = e.id;
  t->fire(t->open_network_properties_);
}
void ComputerManager::on_copy_row(void* self, Control&) { static_cast<ComputerManager*>(self)->grid_.copy_selected_row(); }
void ComputerManager::on_grid_menu_opening(void* self, Control&) {
  auto* t = static_cast<ComputerManager*>(self);
  t->copy_row_item_->set_enabled(t->grid_.selected_row() >= 0);
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
      if (device >= 0 && g == static_cast<uint32_t>(HardwareGroup::Network)) tree_.set_node_menu(device, &adapter_menu_);   // (Properties)
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
  if (node < 0 || !t->inventory_) {
    t->hardware_info_.set_text(StringView());
    return;
  }
  const uint32_t tag = t->tree_.node_tag(node);
  String text(t->allocator());
  if (tag & 0x10000u) {
    text.append_uint(t->tree_.child_count(node));
    text.append(t->tree_.child_count(node) == 1 ? " device" : " devices");
    if ((tag & 0xFFFFu) == static_cast<uint32_t>(HardwareGroup::Displays)) text.append(" - right-click for Display Settings");
  } else {
    HardwareEntry e;
    if (t->inventory_->entry(tag, &e)) text.assign(e.detail[0] ? StringView(e.detail) : StringView(e.name));
  }
  t->hardware_info_.set_text(text.view());
}

ComputerManager::~ComputerManager() { release_view(allocator(), buffer_); }

void ComputerManager::set_monitor(ISystemMonitor* monitor) {
  monitor_ = monitor;
  have_previous_ = false;
  previous_cores_.clear();
  previous_threads_.clear();
  model_.ticks_per_second = monitor ? monitor->ticks_per_second() : 1000;
}

int32_t ComputerManager::thread_load_tenths(uint64_t id) const {
  for (const Row& r : model_.rows)
    if (r.id == id && !r.idle) return static_cast<int32_t>(r.tenths);
  return -1;
}

void ComputerManager::update(uint64_t now_us) {
  UiGuard lock(this);
  if (inventory_ && inventory_->refresh()) rebuild_hardware();
  if (!monitor_ || buffer_.empty()) {
    info_.set_text("No system monitor available");
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
      row->title.set_size(96, 0);
      row->value.set_dock(DockStyle::Right);
      row->value.set_size(50, 0);
      row->meter.set_peak_hold(false);
      row->meter.set_dock(DockStyle::Fill);
      row->row.add(&row->title);
      row->row.add(&row->value);
      row->row.add(&row->meter);
      row->row.set_visible(false);
      performance_page_.add(&row->row);
      core_rows_.push_back(util::move(row));
    }
    if (i >= core_rows_.count()) continue;
    CoreRow& row = *core_rows_[i];
    row.row.set_visible(core.online);
    if (!core.online) continue;
    const uint64_t percent = busy + idle ? busy * 100 / (busy + idle) : 0;
    text.assign("Core ");
    text.append_uint(i);
    if (core.efficiency) text.append(" (E)");
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
  previous_cores_.clear();
  for (const TaskCore& c : current) previous_cores_.push_back(c);
  last_us_ = now_us;
  have_previous_ = true;
  grid_.model_changed();
  grid_.select_row(reselect);

  // --- Summary line.
  text.assign("Threads: ");
  text.append_uint(shown_threads);
  text.append("   Cores: ");
  text.append_uint(online);
  text.append("   Uptime: ");
  const uint64_t seconds = now_us / 1000000;
  auto two = [&](uint64_t v) {
    text.push_back(static_cast<char>('0' + (v / 10) % 10));
    text.push_back(static_cast<char>('0' + v % 10));
  };
  two(seconds / 3600);
  text.push_back(':');
  two(seconds / 60 % 60);
  text.push_back(':');
  two(seconds % 60);
  info_.set_text(text.view());
}

}  // namespace desktop
