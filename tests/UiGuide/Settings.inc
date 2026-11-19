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

// The management tools, property sheets and system dialogs against the guidelines: the sign-in, colour and print dialogs (11.2 - 11.6), the
// property sheets and the Wi-Fi and Bluetooth windows (11.5, 6.3), Display Settings (6.3, 11.5, 17.1), the computer manager and the app manager
// (8, 11.2, 11.8). (included by tests/UiGuideTest.cpp)
#include <map>
#include "../FakeMonitors.hpp"
#include "Desktop/AppManager.hpp"
#include "Desktop/BluetoothProperties.hpp"
#include "Desktop/ColorDialog.hpp"
#include "Desktop/ComputerManager.hpp"
#include "Desktop/CredentialsDialog.hpp"
#include "Desktop/Desktop.hpp"
#include "Desktop/DeviceProperties.hpp"
#include "Desktop/DisplaySettings.hpp"
#include "Desktop/NetworkProperties.hpp"
#include "Desktop/PrintDialog.hpp"
#include "Desktop/WirelessProperties.hpp"

namespace settings_test {

struct Counter {
  int count = 0;
  static void fn(void* context, Control&) { ++static_cast<Counter*>(context)->count; }
  Handler handler() { return {&Counter::fn, this}; }
};

static void put(char* out, size_t capacity, const char* text) {
  strncpy(out, text, capacity - 1);
  out[capacity - 1] = 0;
}

struct PrintService final : desktop::IPrintService {
  uint32_t count = 2;
  uint32_t printer_count() override { return count; }
  bool printer(uint32_t i, desktop::PrinterListing* out) override {
    if (i >= count) return false;
    *out = desktop::PrinterListing();
    put(out->name, sizeof out->name, i == 0 ? "Office laser" : "Color jet");
    out->color = i == 1;
    out->paper_sizes = 0x03;
    return true;
  }
  uint32_t default_printer() override { return 0; }
  bool print_text(uint32_t, StringView, StringView, const desktop::PrintOptions&, String*) override { return true; }
  bool print_picture(uint32_t, const graphics::Surface&, StringView, const desktop::PrintOptions&, String*) override { return true; }
};

struct DeviceDetails final : desktop::IDeviceDetails {
  bool describe(desktop::HardwareGroup, uint32_t, desktop::DevicePropertyList* out) override {
    out->clear();
    put(out->title, sizeof out->title, "USB Headset");
    return out->add("Kind", "Sound device") && out->add("Channels", "2");
  }
};

struct NetworkDetails final : desktop::INetworkDetails {
  bool adapter(uint32_t, desktop::NetworkAdapterInfo* out) override {
    *out = desktop::NetworkAdapterInfo();
    put(out->name, sizeof out->name, "Fake NIC");
    out->link_up = true;
    return true;
  }
};

struct Wireless final : desktop::IWirelessDetails {
  desktop::WirelessState state = desktop::WirelessState::Disconnected;
  int connects = 0, disconnects = 0;
  std::string ssid, passphrase;
  bool is_wireless(uint32_t) override { return true; }
  bool adapter(uint32_t, desktop::WirelessAdapterInfo* out) override {
    *out = desktop::WirelessAdapterInfo();
    put(out->name, sizeof out->name, "Fake Wi-Fi");
    out->state = state;
    if (state == desktop::WirelessState::Connected) {
      put(out->network.ssid, sizeof out->network.ssid, ssid.c_str());
      out->network.security = desktop::WirelessSecurity::Wpa2;
      out->network.channel = 6;
      out->network.signal_percent = 80;
    }
    return true;
  }
  bool scan(uint32_t) override { return true; }
  uint32_t networks(uint32_t, desktop::WirelessNetworkInfo* out, uint32_t capacity) override {
    if (capacity < 2) return 0;
    out[0] = desktop::WirelessNetworkInfo();
    put(out[0].ssid, sizeof out[0].ssid, "Home");
    out[0].security = desktop::WirelessSecurity::Wpa2;
    out[0].channel = 6;
    out[0].signal_percent = 80;
    out[0].bssid[0] = 1;
    out[1] = desktop::WirelessNetworkInfo();
    put(out[1].ssid, sizeof out[1].ssid, "Cafe");
    out[1].channel = 11;
    out[1].signal_percent = 40;
    out[1].bssid[0] = 2;
    return 2;
  }
  desktop::WirelessResult connect(uint32_t, StringView name, StringView pass) override {
    ++connects;
    ssid.assign(name.data(), name.size());
    passphrase.assign(pass.data(), pass.size());
    state = desktop::WirelessState::Connected;
    return desktop::WirelessResult::Ok;
  }
  void disconnect(uint32_t) override { ++disconnects; state = desktop::WirelessState::Disconnected; }
};

struct Bluetooth final : desktop::IBluetoothDetails {
  desktop::BluetoothDeviceRow row;
  desktop::BluetoothPrompt question;
  int forgets = 0, connects = 0;
  Bluetooth() {
    put(row.name, sizeof row.name, "Keyboard");
    put(row.kind, sizeof row.kind, "Keyboard");
    row.key[0] = 0xC1;
    row.paired = true;
  }
  bool is_bluetooth(uint32_t) override { return true; }
  bool adapter(uint32_t, desktop::BluetoothAdapterInfo* out) override {
    *out = desktop::BluetoothAdapterInfo();
    put(out->name, sizeof out->name, "Fake Bluetooth");
    out->ready = true;
    return true;
  }
  void set_auto_reconnect(uint32_t, bool) override {}
  bool set_discovery(uint32_t, bool) override { return true; }
  uint32_t devices(uint32_t, desktop::BluetoothDeviceRow* out, uint32_t capacity) override {
    if (capacity < 1) return 0;
    out[0] = row;
    return 1;
  }
  desktop::BluetoothResult connect(uint32_t, const uint8_t*) override { ++connects; return desktop::BluetoothResult::Ok; }
  void disconnect(uint32_t, const uint8_t*) override {}
  void forget(uint32_t, const uint8_t*) override { ++forgets; row.paired = false; }
  bool prompt(uint32_t, desktop::BluetoothPrompt* out) override { *out = question; return true; }
  void answer_confirm(uint32_t, bool) override {}
  void answer_passkey(uint32_t, uint32_t) override {}
};

struct Inventory final : desktop::IHardwareInventory {
  desktop::HardwareEntry entries[5];
  Inventory() {
    auto set = [&](uint32_t i, desktop::HardwareGroup group, const char* name, uint32_t id, bool wireless = false, bool bluetooth = false) {
      entries[i].group = group;
      put(entries[i].name, sizeof entries[i].name, name);
      entries[i].id = id;
      entries[i].wireless = wireless;
      entries[i].bluetooth = bluetooth;
    };
    set(0, desktop::HardwareGroup::Displays, "Monitor A", 0);
    set(1, desktop::HardwareGroup::Network, "Fake NIC", 7);
    set(2, desktop::HardwareGroup::Network, "Fake Wi-Fi", 5, true);
    set(3, desktop::HardwareGroup::Audio, "USB Headset", 2);
    set(4, desktop::HardwareGroup::Bluetooth, "Fake Bluetooth", 0, false, true);
  }
  bool refresh() override { return false; }
  uint32_t count() const override { return 5; }
  bool entry(uint32_t index, desktop::HardwareEntry* out) const override {
    if (index >= 5) return false;
    *out = entries[index];
    return true;
  }
};

struct AppNetwork final : desktop::appman::INetwork {
  std::map<std::string, std::string> pages;
  bool fetch(StringView url, uint64_t max_bytes, String* body) override {
    auto it = pages.find(std::string(url.data(), url.size()));
    return it != pages.end() && it->second.size() <= max_bytes && body->assign(StringView(it->second.data(), it->second.size()));
  }
  bool download(StringView, Sink, void*, uint64_t*) override { return false; }
};

static void make_file(filesystem::IFileSystem& fs, const char* path, const char* text) {
  UniquePointer<filesystem::IFile> file;
  uint64_t written = 0;
  CHECK(fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
  if (file) CHECK(file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text)), &written) == filesystem::Status::Ok);
}

static int32_t tree_node(TreeView& tree, const char* text) {
  for (uint32_t n = 0; n < tree.node_count(); ++n)
    if (tree.node_text(static_cast<int32_t>(n)).equals(text)) return static_cast<int32_t>(n);
  return -1;
}

// A context menu holds the commands of its object with Properties last, and nothing in it ends in "..." (each opens a window and asks nothing first).
static bool ends_in_properties(ContextMenu& menu) {
  Menu& m = menu.menu();
  if (m.item_count() == 0 || !m.item(m.item_count() - 1)->text.view().equals("Properties")) return false;
  for (uint32_t i = 0; i < m.item_count(); ++i)
    if (m.item(i)->text.view().ends_with("...")) return false;
  return true;
}

}  // namespace settings_test

static void ui_guide_settings() {
  namespace st = settings_test;
  TestAllocator a;

  // --- Sign In (11.1 - 11.4): the Tab order is the reading order, the labels carry the access keys of their boxes, OK is the default button, and
  // an answer that cannot be right keeps the dialog open with the focus in the box at fault.
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    desktop::CredentialsDialog dlg(a);
    st::Counter done;
    CHECK(dlg.show(mgr, "srv", "", "", done.handler()));
    w.layout();
    CHECK(dlg.title().equals("Sign In") && dlg.heading_label().text().equals("Sign in to srv.") && dlg.ok_button().is_default() && !dlg.cancel_button().is_default());
    CHECK(w.focus() == &dlg.user_box());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.password_box());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.ok_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.cancel_button());
    alt(w, 'p');
    CHECK(w.focus() == &dlg.password_box());
    alt(w, 'u');
    CHECK(w.focus() == &dlg.user_box());
    // Enter with no user name: the dialog stays, says what is missing, and the focus is in the user name box.
    w.set_focus(&dlg.password_box());
    press(w, key::kEnter);
    CHECK(dlg.showing() && done.count == 0 && w.focus() == &dlg.user_box() && dlg.note_label().text().find(StringView("user name")) != StringView::npos);
    type_text(w, "alice");
    press(w, key::kEnter);
    CHECK(!dlg.showing() && dlg.accepted() && dlg.user().equals("alice") && done.count == 1);
    // Escape is Cancel.
    CHECK(dlg.show(mgr, "srv", "", "alice", done.handler()) && w.focus() == &dlg.password_box());
    press(w, key::kEscape);
    CHECK(!dlg.showing() && !dlg.accepted() && done.count == 2);
    // Ids are numbers: anything else is asked for again, in its box.
    CHECK(dlg.show(mgr, "nfs", "", "", done.handler(), desktop::CredentialsDialog::Style::UnixIds));
    CHECK(dlg.heading_label().text().equals("User and group IDs to use on nfs.") && dlg.user().equals("65534"));
    dlg.password_box().set_text("staff");
    click_center(w, dlg.ok_button());
    CHECK(dlg.showing() && w.focus() == &dlg.password_box() && dlg.note_label().text().find(StringView("number")) != StringView::npos);
    dlg.password_box().set_text("100");
    click_center(w, dlg.ok_button());
    CHECK(!dlg.showing() && dlg.accepted() && dlg.password().equals("100"));
    dlg.clear();
  }
  CHECK(a.live == 0);

  // --- Color (11.6): titled by its command, the labels end in a colon and reach their boxes, a sample shows the choice, OK is the default button
  // and comes before Cancel, and a code that is not a color keeps the dialog open with the focus in the box.
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    desktop::ColorDialog dlg(a);
    st::Counter done;
    CHECK(dlg.show(mgr, "Primary Color", 0xFF336699, done.handler()));
    w.layout();
    CHECK(dlg.title().equals("Primary Color") && dlg.ok_button().is_default() && w.focus() == &dlg.spectrum());
    CHECK(dlg.preview_color() == 0xFF336699 && dlg.original_color() == 0xFF336699);      // the sample, beside what it replaces
    alt(w, 'c');
    CHECK(w.focus() == &dlg.hex_box());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.ok_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.cancel_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.spectrum());                                                  // (round again: the main field is first)
    alt(w, 'g');
    press(w, key::kUp);                                                                   // the box the Green label names takes the key
    CHECK(dlg.green_box().value() == 0x67 && dlg.preview_color() == 0xFF336799);
    // A half-typed code: OK keeps the dialog, says what a code looks like, and the focus goes to the box.
    dlg.hex_box().set_text("#12");
    click_center(w, dlg.ok_button());
    CHECK(dlg.showing() && !dlg.accepted() && w.focus() == &dlg.hex_box() && !dlg.message_label().text().empty() && done.count == 0);
    dlg.hex_box().set_text("#0A0B0C");
    CHECK(dlg.message_label().text().empty());
    press(w, key::kEnter);
    CHECK(!dlg.showing() && dlg.accepted() && dlg.color() == 0xFF0A0B0C && done.count == 1);
    CHECK(dlg.show(mgr, "Primary Color", 0xFF000000, done.handler()));
    press(w, key::kEscape);
    CHECK(!dlg.showing() && !dlg.accepted());
  }
  CHECK(a.live == 0);

  // --- Print (11.6): what to print, the copies, the printer and a button to the printer's properties; Print is the default button; every choice
  // has an access key; Tab ends at the transaction buttons.
  {
    Screen screen(a, 900, 700);
    Theme theme;
    Window w(a, screen.renderer, theme, {900, 700});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    st::PrintService service;
    desktop::PrintDialog dlg(a);
    dlg.set_print_service(&service);
    dlg.set_text("one\ntwo\n");
    st::Counter done, properties;
    CHECK(dlg.show(mgr, desktop::PrintDialog::Kind::Text, "Print", done.handler()));
    w.layout();
    CHECK(dlg.title().equals("Print") && dlg.print_button().is_default() && dlg.properties_button().text().equals("Properties"));
    CHECK(!dlg.properties_button().enabled());                    // (nobody to show the sheet: unavailable, not a button that does nothing)
    dlg.on_printer_properties(properties.handler());
    CHECK(dlg.properties_button().enabled());
    dlg.printer_box().set_selected_index(1);
    click_center(w, dlg.properties_button());
    CHECK(properties.count == 1 && dlg.printer_index() == 1 && dlg.showing());
    // Every labelled choice has an access key of its own.
    dlg.assign_access_keys();
    Control* const labelled[] = {&dlg.properties_button(), &dlg.portrait_button(), &dlg.landscape_button(), &dlg.all_pages_button(), &dlg.page_range_button(),
                                 &dlg.toner_box(), &dlg.previous_page_button(), &dlg.next_page_button(), &dlg.print_button()};
    uint32_t used = 0;
    for (Control* c : labelled) {
      const char k = AccessKeys::key_of(c->access_label(0), c->access_index(0));
      CHECK(k != 0 && !(used & (1u << (k - 'a'))));
      if (k) used |= 1u << (k - 'a');
    }
    // The Tab order: the options from the printer on, the transaction buttons last.
    w.set_focus(&dlg.cancel_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.printer_box());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.properties_button());
    w.set_focus(&dlg.print_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dlg.cancel_button());
    w.set_focus(&dlg.copies_box());
    press(w, key::kEnter);                                        // Enter is Print from a field
    CHECK(!dlg.showing() && dlg.accepted() && done.count == 1);
    // No printer: Print and Properties are unavailable and the dialog says what to do.
    service.count = 0;
    CHECK(dlg.show(mgr, desktop::PrintDialog::Kind::Text, "Print", done.handler()));
    CHECK(!dlg.print_button().enabled() && !dlg.properties_button().enabled() && dlg.status_label().text().find(StringView("Connect one")) != StringView::npos);
    press(w, key::kEscape);
    CHECK(!dlg.showing() && !dlg.accepted());
  }
  CHECK(a.live == 0);

  // --- The printer's properties from the print dialog, in the desktop: the sheet of that printer opens on top of the (modal) print dialog, and
  // closing it comes back to the dialog.
  {
    Screen screen(a, 1000, 700);
    st::PrintService service;
    st::DeviceDetails details;
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_print_service(&service);
    d.set_device_details(&details);
    Window& w = d.window();
    st::Counter done;
    desktop::PrintDialog& dlg = d.print_dialog();
    CHECK(dlg.show(d.windows(), desktop::PrintDialog::Kind::Text, "Print", done.handler()));
    w.layout();
    CHECK(dlg.properties_button().enabled());
    click_center(w, dlg.properties_button());
    desktop::DeviceProperties& sheet = d.device_properties();
    CHECK(sheet.manager() && sheet.modal() && sheet.group() == desktop::HardwareGroup::Printers && sheet.device() == 0 && d.windows().active() == &sheet);
    press(w, key::kEscape);
    CHECK(!sheet.manager() && dlg.showing() && d.windows().active() == &dlg);
    press(w, key::kEscape);
    CHECK(!dlg.showing());
  }
  CHECK(a.live == 0);

  // --- Property sheets (11.5): "<Object name> Properties", no Minimize or Maximize, Escape and Enter close, the table has a context menu, and
  // Tab goes from the table to the buttons.
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    st::DeviceDetails details;
    desktop::DeviceProperties props(a);
    props.set_details(&details);
    props.show_device(desktop::HardwareGroup::Audio, 2);
    CHECK(mgr.add_window(&props));
    w.layout();
    CHECK(props.title().equals("USB Headset Properties") && !props.minimize_enabled() && !props.maximize_enabled() && props.close_button().is_default());
    CHECK(w.focus() == &props.grid());
    press(w, key::kTab);
    CHECK(w.focus() == &props.copy_button());
    press(w, key::kTab);
    CHECK(w.focus() == &props.close_button());
    // The context menu from the keyboard: Copy Row is unavailable until a row is selected.
    w.set_focus(&props.grid());
    press(w, static_cast<uint8_t>(key::kF1 + 9), modifier::kLeftShift);
    CHECK(props.grid_menu().open() && props.grid_menu().find_item("Copy Row") && !props.grid_menu().find_item("Copy Row")->enabled());
    w.close_popup();
    props.grid().select_row(0);
    press(w, static_cast<uint8_t>(key::kF1 + 9), modifier::kLeftShift);
    CHECK(props.grid_menu().open() && props.grid_menu().find_item("Copy Row")->enabled());
    w.close_popup();
    press(w, key::kEscape);
    CHECK(!props.manager());
    CHECK(mgr.add_window(&props));
    w.layout();
    w.set_focus(&props.grid());
    press(w, key::kEnter);                                        // the default button: Close
    CHECK(!props.manager());

    st::NetworkDetails network;
    desktop::NetworkProperties adapter(a);
    adapter.set_details(&network);
    adapter.show_adapter(7);
    CHECK(mgr.add_window(&adapter));
    w.layout();
    CHECK(adapter.title().equals("Fake NIC Properties") && !adapter.maximize_enabled() && adapter.close_button().is_default());
    press(w, key::kEscape);
    CHECK(!adapter.manager());
  }
  CHECK(a.live == 0);

  // --- Wi-Fi Networks: titled by its command and its adapter, Connect is the default button, a missing password puts the focus in the password
  // box, the labels reach the list and the box, and the list has the commands in a context menu.
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    st::Wireless wifi;
    desktop::WirelessProperties props(a);
    props.set_details(&wifi);
    CHECK(mgr.add_window(&props));
    props.show_adapter(5);
    w.layout();
    CHECK(props.title().equals("Wi-Fi Networks - Fake Wi-Fi") && props.connect_button().is_default() && !props.connect_button().enabled());
    alt(w, 'n');
    CHECK(w.focus() == &props.grid());
    press(w, key::kDown);                                         // the first key selects the first network
    CHECK(props.grid().selected_row() == 0 && props.connect_button().enabled());
    press(w, key::kEnter);                                        // Home wants a password
    CHECK(wifi.connects == 0 && w.focus() == &props.password_box() && props.status_label().text().find(StringView("password")) != StringView::npos);
    type_text(w, "secret");
    press(w, key::kEnter);
    CHECK(wifi.connects == 1 && wifi.ssid == "Home" && wifi.passphrase == "secret" && props.status_label().text().equals("Connected to Home."));
    CHECK(props.connection_label().text().equals("Connected to Home - WPA2, channel 6, signal 80%"));
    alt(w, 'p');
    CHECK(w.focus() == &props.password_box());
    CHECK(props.grid_menu().find_item("Connect") && props.grid_menu().find_item("Disconnect") && props.grid_menu().find_item("Disconnect")->enabled());
    alt(w, 'd');                                                  // Disconnect, by its access key
    CHECK(wifi.disconnects == 1 && !props.grid_menu().find_item("Disconnect")->enabled());
    w.set_focus(&props.grid());
    press(w, key::kEscape);
    CHECK(!props.manager());
  }
  CHECK(a.live == 0);

  // --- Bluetooth Devices: Forget deletes a pairing, so it asks and Cancel is the default answer; Escape cancels a pairing question before it
  // closes the window.
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    st::Bluetooth bt;
    desktop::BluetoothProperties props(a);
    props.set_details(&bt);
    CHECK(mgr.add_window(&props));
    props.show_adapter(0);
    w.layout();
    CHECK(props.title().equals("Bluetooth Devices - Fake Bluetooth") && !props.maximize_enabled());
    props.grid().select_row(0);
    CHECK(props.forget_button().enabled() && props.grid_menu().find_item("Forget")->enabled());
    click_center(w, props.forget_button());
    MessageDialog& ask = props.forget_dialog();
    CHECK(ask.showing() && bt.forgets == 0 && ask.default_choice() == MessageDialog::Choice::Decline && w.focus() == &ask.decline_button());
    CHECK(ask.accept_button().text().equals("Forget") && ask.decline_button().text().equals("Cancel") && !ask.accept_button().is_default());
    press(w, key::kEnter);                                        // Enter is the safe answer
    CHECK(!ask.showing() && bt.forgets == 0);
    click_center(w, props.forget_button());
    press(w, key::kEscape);
    CHECK(!ask.showing() && bt.forgets == 0 && props.manager());
    click_center(w, props.forget_button());
    alt(w, 'f');                                                  // the deliberate choice
    CHECK(!ask.showing() && bt.forgets == 1 && !props.forget_button().enabled());
    // A pairing question: its label names the passkey box; a wrong passkey keeps the focus there; Escape cancels the pairing, not the window.
    bt.question.kind = desktop::BluetoothPromptKind::EnterPasskey;
    st::put(bt.question.device, sizeof bt.question.device, "Keyboard");
    props.update();
    w.layout();
    CHECK(props.prompt_row().visible() && props.prompt_label().text().equals("Type the passkey Keyboard shows:"));
    w.set_focus(&props.grid());
    press(w, key::kEnter);                                        // Enter answers the question: nothing typed yet
    CHECK(w.focus() == &props.passkey_box() && props.status_label().text().find(StringView("passkey")) != StringView::npos);
    press(w, key::kEscape);
    CHECK(props.manager() && props.status_label().text().equals("Pairing cancelled."));
    bt.question.kind = desktop::BluetoothPromptKind::None;
    props.update();
    w.layout();
    w.set_focus(&props.grid());
    press(w, key::kEscape);
    CHECK(!props.manager());
  }
  CHECK(a.live == 0);

  // --- Display Settings (6.3, 11.5, 17.1): OK, Cancel and Apply; a new mode is kept only by a deliberate choice - Enter and Escape both bring
  // the old one back, the time limit is a net that More Time extends - and a kept one can still be taken back (Restore Previous, Ctrl+Z).
  {
    fakemon::FakeSource source;
    source.add("DP-1", fakemon::dell_edid());
    source.add("HDMI-1", fakemon::small_edid(), true, hardware::ConnectorType::Hdmi);
    fakemon::FakeGpu gpu;
    gpu.source = &source;
    hardware::SystemMonitors monitors(&gpu, a);
    monitors.initialize();
    CHECK(monitors.apply());
    Screen screen(a, 900, 700);
    Theme theme;
    Window w(a, screen.renderer, theme, {900, 700});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    MessageDialog confirm(a);
    desktop::DisplaySettings ds(a);
    ds.set_monitors(&monitors);
    ds.set_confirm_dialog(&confirm);
    CHECK(mgr.add_window(&ds));
    w.layout();
    CHECK(ds.title().equals("Display Settings") && ds.ok_button().is_default() && !ds.minimize_enabled() && !ds.maximize_enabled());
    CHECK(!ds.pending() && !ds.apply_button().enabled() && !ds.restore_button().enabled());      // nothing to apply, nothing to take back
    const uint32_t first_width = ds.selected_monitor()->current_mode().width;
    auto choose_other_resolution = [&] {      // with the keyboard: the label's access key, then an arrow
      alt(w, 'r');
      CHECK(w.focus() == &ds.resolution_list());
      press(w, ds.resolution_list().selected_index() == 0 ? key::kDown : key::kUp);
    };
    choose_other_resolution();
    CHECK(ds.pending() && ds.apply_button().enabled());
    alt(w, 'a');                              // Apply
    CHECK(ds.asking() && confirm.showing() && ds.selected_monitor()->current_mode().width != first_width && ds.manager());
    // The question: Revert is the default button and has the focus; Keep is a deliberate choice; More Time is between them.
    CHECK(confirm.default_choice() == MessageDialog::Choice::Decline && w.focus() == &confirm.decline_button() && confirm.decline_button().is_default());
    CHECK(confirm.accept_button().text().equals("Keep") && confirm.alternate_button().visible() && confirm.alternate_button().text().equals("More Time") &&
          confirm.decline_button().text().equals("Revert"));
    press(w, key::kEnter);                    // (somebody who sees nothing presses Enter...)
    CHECK(!ds.asking() && !confirm.showing() && ds.selected_monitor()->current_mode().width == first_width && ds.manager() && !ds.restore_button().enabled());
    choose_other_resolution();
    alt(w, 'a');
    CHECK(ds.asking());
    press(w, key::kEscape);                   // (...or Escape: both go back)
    CHECK(!ds.asking() && ds.selected_monitor()->current_mode().width == first_width);
    // More Time starts the countdown again, longer; running out reverts (the net, not the route).
    choose_other_resolution();
    alt(w, 'a');
    CHECK(ds.asking() && ds.seconds_left() == desktop::DisplaySettings::kConfirmSeconds);
    ds.tick();
    alt(w, 'm');
    CHECK(ds.asking() && confirm.showing() && ds.seconds_left() == desktop::DisplaySettings::kMoreSeconds && w.focus() == &confirm.decline_button());
    for (uint32_t i = 0; i < desktop::DisplaySettings::kMoreSeconds; ++i) ds.tick();
    CHECK(!ds.asking() && !confirm.showing() && ds.selected_monitor()->current_mode().width == first_width);
    // Keep, then take it back after all: Restore Previous, by Ctrl+Z.
    choose_other_resolution();
    alt(w, 'a');
    alt(w, 'k');
    CHECK(!ds.asking() && !confirm.showing() && ds.selected_monitor()->current_mode().width != first_width && ds.restore_button().enabled() && !ds.pending());
    ctrl(w, 'z');
    CHECK(ds.selected_monitor()->current_mode().width == first_width);
    // The close button is not Cancel: with choices that were not applied it asks. Cancel there keeps the window and the choices.
    choose_other_resolution();
    ds.close();
    CHECK(ds.manager() && confirm.showing() && confirm.accept_button().text().equals("Apply") && confirm.alternate_button().text().equals("Don't Apply"));
    press(w, key::kEscape);
    CHECK(ds.manager() && !confirm.showing() && ds.pending());
    ds.close();
    alt(w, 'd');                              // Don't Apply: closed, nothing changed
    CHECK(!ds.manager() && ds.selected_monitor()->current_mode().width == first_width);
    // Cancel (Escape) drops what was not applied without asking; OK applies, asks to keep, and closes when kept.
    CHECK(mgr.add_window(&ds));
    w.layout();
    CHECK(!ds.pending());
    choose_other_resolution();
    press(w, key::kEscape);
    CHECK(!ds.manager() && !confirm.showing() && ds.selected_monitor()->current_mode().width == first_width);
    CHECK(mgr.add_window(&ds));
    w.layout();
    choose_other_resolution();
    click_center(w, ds.ok_button());
    CHECK(ds.asking() && ds.manager());
    alt(w, 'k');
    CHECK(!ds.asking() && !ds.manager() && ds.selected_monitor()->current_mode().width != first_width);
    // A monitor is moved without the pointer: Ctrl+arrow in the picture (and the same commands are in its context menu).
    CHECK(mgr.add_window(&ds));
    w.layout();
    w.set_focus(&ds.map());
    press(w, key::kRight);
    CHECK(ds.map().selected() == 1);
    const int32_t slot0 = monitors.layout().find(0), slot1 = monitors.layout().find(1);
    CHECK(slot0 >= 0 && slot1 >= 0 && monitors.layout().at(static_cast<uint32_t>(slot1)).rect.x > monitors.layout().at(static_cast<uint32_t>(slot0)).rect.x);
    press(w, key::kLeft, modifier::kLeftControl);
    CHECK(monitors.layout().at(static_cast<uint32_t>(monitors.layout().find(1))).rect.x < monitors.layout().at(static_cast<uint32_t>(monitors.layout().find(0))).rect.x);
    CHECK(ds.map_menu().find_item("Move Left") && ds.map_menu().find_item("Move Right") && ds.map_menu().find_item("Make Primary"));
    press(w, static_cast<uint8_t>(key::kF1 + 9), modifier::kLeftShift);
    CHECK(ds.map_menu().open() && ds.map_menu().find_item("Move Right")->enabled());
    w.close_popup();
    ds.cancel();
    CHECK(!ds.manager());
  }
  CHECK(a.live == 0);

  // --- Display Settings, Graphics (9.3, 11.2, 11.8, 4, 1.5): on a laptop with a display switch the built-in screen's GPU is shown and the other
  // one offered by a button with an access key; the switch is confirmed first with Cancel as the default (the screen goes dark for a moment),
  // drawing stops while it runs, the line under the button says how it went, a failure is an error notice that says what happened, why and
  // where the screen is; the button is unavailable, with the reason shown, when the switch cannot be made (and while new settings wait).
  {
    struct FakeSwitch final : hardware::IBuiltInScreenSwitch {
      hardware::GpuMuxTarget at = hardware::GpuMuxTarget::Integrated;
      bool can = true, ok = true, back_ok = true, known = true;
      const char* why = "the discrete GPU would not wake";
      int switches = 0;
      std::vector<hardware::GpuMuxTarget> asked;
      bool active(hardware::GpuMuxTarget* out) const override { if (known) *out = at; return known; }
      const char* gpu_name(hardware::GpuMuxTarget side) const override { return side == hardware::GpuMuxTarget::Discrete ? "GeForce GTX 1650" : "UHD Graphics 630"; }
      bool can_switch_to(hardware::GpuMuxTarget, const char** out) const override { if (!can && out) *out = why; return can; }
      Result switch_to(hardware::GpuMuxTarget side) override {
        asked.push_back(side);
        Result r;
        if (!ok && asked.size() == 1) {
          r.reason = "that GPU would not wake";
          r.rolled_back = back_ok;
          return r;
        }
        at = side;
        ++switches;
        r.ok = true;
        return r;
      }
    };
    struct Pauses {
      std::vector<bool> calls;
      static void fn(void* self, bool stop) { static_cast<Pauses*>(self)->calls.push_back(stop); }
    };
    struct Applier {
      int calls = 0, fail_first = 0;
      static bool fn(void* self) { auto* a = static_cast<Applier*>(self); ++a->calls; return a->calls > a->fail_first; }
    };
    fakemon::FakeSource source;
    source.add("eDP-1", fakemon::small_edid(), true, hardware::ConnectorType::Internal);
    fakemon::FakeGpu gpu;
    gpu.source = &source;
    hardware::SystemMonitors monitors(&gpu, a);
    monitors.initialize();
    CHECK(monitors.apply());
    Screen screen(a, 900, 760);
    Theme theme;
    Window w(a, screen.renderer, theme, {900, 760});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    MessageDialog confirm(a);
    NoticeDialog notices(a);
    FakeSwitch sw;
    Pauses pauses;
    Applier applier;
    desktop::DisplaySettings ds(a);
    ds.set_monitors(&monitors);
    ds.set_confirm_dialog(&confirm);
    ds.set_notice_dialog(&notices);
    ds.set_pauser(&Pauses::fn, &pauses);
    ds.set_applier(&Applier::fn, &applier);
    CHECK(!ds.graphics_shown());      // (no display switch: no group)
    const uint32_t plain_height = ds.bounds().height;
    ds.set_graphics_switch(&sw);
    CHECK(mgr.add_window(&ds));
    w.layout();
    CHECK(ds.graphics_shown() && ds.bounds().height > plain_height);
    CHECK(ds.graphics_line().text().equals("The built-in screen is driven by UHD Graphics 630 (the integrated GPU)."));
    CHECK(ds.graphics_button().text().equals("Use GeForce GTX 1650 for the Built-in Screen") && ds.graphics_button().enabled() && !ds.graphics_button().is_default());
    CHECK(ds.graphics_note().text().equals("The built-in screen goes dark for a moment while it switches."));
    // Its access key: Alt+letter asks first. Cancel is the default and has the focus, so Enter and Escape change nothing.
    ds.assign_access_keys();
    const char key_letter = AccessKeys::key_of(ds.graphics_button().access_label(0), ds.graphics_button().access_index(0));
    CHECK(key_letter != 0);
    alt(w, key_letter);
    CHECK(confirm.showing() && confirm.default_choice() == MessageDialog::Choice::Decline && w.focus() == &confirm.decline_button() &&
          confirm.accept_button().text().equals("Switch") && confirm.decline_button().text().equals("Cancel") && !confirm.alternate_button().visible());
    press(w, key::kEnter);
    CHECK(!confirm.showing() && sw.asked.empty() && pauses.calls.empty() && applier.calls == 0);
    // With a single click (the button), then Switch: drawing stops, the panel moves, the monitors are made real, drawing goes on.
    click_center(w, ds.graphics_button());
    CHECK(confirm.showing());
    click_center(w, confirm.accept_button());
    CHECK(!confirm.showing() && sw.switches == 1 && sw.asked.size() == 1 && sw.asked[0] == hardware::GpuMuxTarget::Discrete && applier.calls == 1);
    CHECK(pauses.calls.size() == 2 && pauses.calls[0] && !pauses.calls[1] && !notices.showing());
    CHECK(ds.graphics_line().text().equals("The built-in screen is driven by GeForce GTX 1650 (the discrete GPU)."));
    CHECK(ds.graphics_button().text().equals("Use UHD Graphics 630 for the Built-in Screen") && ds.graphics_button().enabled());
    CHECK(ds.graphics_note().text().equals("The built-in screen is now driven by GeForce GTX 1650."));
    // From the keyboard alone: Space on the focused button, then Alt+S (Switch) - and back it goes.
    w.set_focus(&ds.graphics_button());
    press(w, key::kSpace);
    CHECK(confirm.showing());
    alt(w, 's');
    CHECK(!confirm.showing() && sw.at == hardware::GpuMuxTarget::Integrated && sw.switches == 2);
    // A switch that fails (undone by the switcher): an error notice says what happened, why and where the screen is; the line says so too.
    sw.ok = false;
    sw.asked.clear();
    CHECK(ds.switch_graphics() == false && sw.at == hardware::GpuMuxTarget::Integrated);
    CHECK(notices.showing() && notices.kind() == NoticeKind::Error && notices.title().equals("Display Settings"));
    CHECK(notices.message().equals("The built-in screen could not be switched to GeForce GTX 1650 because that GPU would not wake. It stays on UHD Graphics 630."));
    CHECK(ds.graphics_note().text().equals(notices.message()));
    press(w, key::kEnter);
    CHECK(!notices.showing());
    // ... one that could not be undone either: the notice says the screen may stay dark.
    sw.asked.clear();
    sw.back_ok = false;
    CHECK(!ds.switch_graphics() && notices.showing() && strstr(std::string(notices.message().data(), notices.message().size()).c_str(), "If it stays dark, restart the computer."));
    press(w, key::kEnter);
    // The desktop cannot be shown on the new GPU (the applier fails): the panel is switched back and the user is told.
    sw.ok = true;
    sw.back_ok = true;
    sw.asked.clear();
    applier.calls = 0;
    applier.fail_first = 1;
    CHECK(!ds.switch_graphics() && sw.asked.size() == 2 && sw.asked[1] == hardware::GpuMuxTarget::Integrated && sw.at == hardware::GpuMuxTarget::Integrated && applier.calls == 2);
    CHECK(notices.showing() && strstr(std::string(notices.message().data(), notices.message().size()).c_str(), "the desktop could not be shown on it"));
    press(w, key::kEnter);
    applier.fail_first = 0;
    // Unavailable, not failing: the reason is shown, and the button does nothing.
    sw.can = false;
    ds.refresh();
    CHECK(!ds.graphics_button().enabled() && ds.graphics_note().text().equals("Not available because the discrete GPU would not wake."));
    sw.asked.clear();
    CHECK(!ds.ask_switch_graphics() && !confirm.showing() && sw.asked.empty());
    sw.can = true;
    sw.known = false;
    ds.refresh();
    CHECK(!ds.graphics_button().enabled() && ds.graphics_line().text().equals("Which GPU drives the built-in screen is not known."));
    sw.known = true;
    ds.refresh();
    CHECK(ds.graphics_button().enabled());
    // While new display settings wait for Keep or Revert, the switch waits too.
    alt(w, 'r');
    press(w, ds.resolution_list().selected_index() == 0 ? key::kDown : key::kUp);
    if (ds.pending()) {
      alt(w, 'a');
      CHECK(ds.asking() && !ds.graphics_button().enabled());
      press(w, key::kEscape);
      CHECK(!ds.asking() && ds.graphics_button().enabled());
    }
    ds.cancel();
    CHECK(!ds.manager());
  }
  CHECK(a.live == 0);

  // --- Computer Manager (8): the menus in the standard order with access keys, Help ending in About; every command of the context menus is
  // also in the Device menu and unavailable where it does not apply; Alt+Enter is Properties; the context menus end in Properties.
  {
    Screen screen(a, 800, 700);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 700});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    st::Inventory inventory;
    NoticeDialog notices(a);
    desktop::ComputerManager cm(a);
    cm.set_hardware_inventory(&inventory);
    cm.set_notices(&notices);
    st::Counter display, network, wireless, device, bluetooth;
    cm.on_open_display_settings(display.handler());
    cm.on_open_network_properties(network.handler());
    cm.on_open_wireless_properties(wireless.handler());
    cm.on_open_device_properties(device.handler());
    cm.on_open_bluetooth_properties(bluetooth.handler());
    CHECK(mgr.add_window(&cm));
    w.layout();
    MenuBar& bar = cm.menu_bar();
    CHECK(cm.title().equals("Computer Manager") && standard_menu_order(bar) && menus_have_access_keys(cm, bar) && help_menu_ends_in_about(bar, "Computer Manager"));
    CHECK(bar.menu(0)->item(bar.menu(0)->item_count() - 1)->text.view().equals("Exit"));
    // The Threads tab: nothing selected, no device: the commands are unavailable, and choosing one does nothing.
    CHECK(!bar.find_item("Edit", "Copy Row")->enabled() && !bar.find_item("Device", "Properties")->enabled() && !bar.find_item("Device", "Display Settings")->enabled());
    CHECK(!choose(w, bar, "Device", "Properties") && device.count == 0 && network.count == 0);
    CHECK(bar.find_item("View", "Threads")->checked() && choose(w, bar, "View", "Hardware") && cm.tabs().selected_page() == 2 && bar.find_item("View", "Hardware")->checked());
    w.layout();
    TreeView& tree = cm.hardware_tree();
    // A wired adapter: Properties, by the menu and by Alt+Enter.
    tree.select(st::tree_node(tree, "Fake NIC"));
    CHECK(bar.find_item("Device", "Properties")->enabled() && !bar.find_item("Device", "Wi-Fi Networks")->enabled() && !bar.find_item("Device", "Display Settings")->enabled());
    w.set_focus(&tree);
    press(w, key::kEnter, modifier::kLeftAlt);
    CHECK(network.count == 1 && cm.network_adapter() == 7);
    CHECK(choose(w, bar, "Device", "Properties") && network.count == 2);
    // A Wi-Fi adapter, a Bluetooth adapter, a display, any other device.
    tree.select(st::tree_node(tree, "Fake Wi-Fi"));
    CHECK(choose(w, bar, "Device", "Wi-Fi Networks") && wireless.count == 1 && cm.wireless_adapter() == 5);
    tree.select(st::tree_node(tree, "Fake Bluetooth"));
    CHECK(!bar.find_item("Device", "Wi-Fi Networks")->enabled() && choose(w, bar, "Device", "Bluetooth Devices") && bluetooth.count == 1);
    tree.select(st::tree_node(tree, "Monitor A"));
    CHECK(choose(w, bar, "Device", "Display Settings") && display.count == 1);
    tree.select(st::tree_node(tree, "USB Headset"));
    w.set_focus(&tree);
    press(w, key::kEnter, modifier::kLeftAlt);
    CHECK(device.count == 1 && cm.device_group() == desktop::HardwareGroup::Audio && cm.device_id() == 2);
    // A group has no properties.
    tree.select(cm.hardware_group_node(desktop::HardwareGroup::Audio));
    CHECK(!bar.find_item("Device", "Properties")->enabled());
    press(w, key::kEnter, modifier::kLeftAlt);
    CHECK(device.count == 1);
    CHECK(cm.hardware_info().text().equals("1 device"));
    // The context menus: the device's commands, Properties last, no "...".
    CHECK(st::ends_in_properties(cm.adapter_menu()) && st::ends_in_properties(cm.wireless_menu()) && st::ends_in_properties(cm.device_menu()) &&
          st::ends_in_properties(cm.monitor_menu()) && st::ends_in_properties(cm.bluetooth_menu()));
    CHECK(cm.wireless_menu().menu().item(0)->text.view().equals("Wi-Fi Networks") && cm.display_menu().find_item("Display Settings"));
    // About names the application in its title.
    CHECK(choose(w, bar, "Help", "About Computer Manager") && notices.showing() && notices.title().equals("About Computer Manager") &&
          notices.kind() == NoticeKind::Information);
    press(w, key::kEnter);
    CHECK(!notices.showing());
    CHECK(choose(w, bar, "File", "Exit") && !cm.manager());
  }
  CHECK(a.live == 0);

  // --- App Manager (8, 11.2, 11.8): a menu bar with every command; Remove asks and Cancel is the default answer; a failure is a message with
  // what happened, why and what to do; what does not apply is unavailable.
  {
    Screen screen(a, 1000, 700);
    Theme theme;
    Window w(a, screen.renderer, theme, {1000, 700});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok && fs.create_directory("/Linux/opt") == filesystem::Status::Ok &&
          fs.create_directory("/Linux/opt/notes") == filesystem::Status::Ok);
    st::make_file(fs, "/Linux/opt/notes/version", "1.0\n");       // (an application installed earlier)
    st::make_file(fs, "/Linux/opt/notes/remove", "#!/bin/sh\n");
    st::AppNetwork net;
    const std::string bullet = "\xE2\x97\x86 ";
    net.pages[std::string(desktop::appman::kCatalogUrl.data(), desktop::appman::kCatalogUrl.size())] =
        bullet + "notes : Take notes in Markdown. #itsdesktopapp\n" + bullet + "paint-x : A painting program. #itsdesktopapp\n";
    NoticeDialog notices(a);
    {
      desktop::AppManager m(a);
      m.set_notices(&notices);
      CHECK(mgr.add_window(&m));
      m.set_services(&fs, &net);
      w.layout();
      MenuBar& bar = m.menu_bar();
      CHECK(m.title().equals("App Manager") && standard_menu_order(bar) && menus_have_access_keys(m, bar) && help_menu_ends_in_about(bar, "App Manager"));
      CHECK(bar.menu(0)->item(bar.menu(0)->item_count() - 1)->text.view().equals("Exit"));
      CHECK(m.catalog_count() == 2 && m.list().item_count() == 2 && m.status().equals("2 of 2 applications"));
      alt(w, 's');                              // "Search:" carries the access key of its box
      CHECK(w.focus() == &m.search_box());
      // Nothing selected: the commands are unavailable everywhere they are.
      CHECK(!m.install_button().enabled() && !bar.find_item("Application", "Install")->enabled() && !bar.find_item("Application", "Remove")->enabled() &&
            !m.list_menu().find_item("Remove")->enabled() && !bar.find_item("Application", "Cancel")->enabled());
      m.select_row(0);                          // notes: installed
      CHECK(m.remove_button().enabled() && bar.find_item("Application", "Remove")->enabled() && m.list_menu().find_item("Remove")->enabled() &&
            !bar.find_item("Application", "Install")->enabled());
      // Remove asks first; Enter and Escape leave the application where it is.
      filesystem::DirectoryEntry e;
      click_center(w, m.remove_button());
      MessageDialog& ask = m.remove_dialog();
      CHECK(ask.showing() && ask.default_choice() == MessageDialog::Choice::Decline && w.focus() == &ask.decline_button() && ask.accept_button().text().equals("Remove"));
      press(w, key::kEnter);
      CHECK(!ask.showing() && fs.stat("/Linux/opt/notes/version", &e) == filesystem::Status::Ok);
      w.set_focus(&m.list());
      press(w, key::kDelete);                   // Del is Remove, and asks the same
      CHECK(ask.showing());
      press(w, key::kEscape);
      CHECK(!ask.showing() && fs.stat("/Linux/opt/notes/version", &e) == filesystem::Status::Ok);
      CHECK(choose(w, bar, "Application", "Remove") && ask.showing());
      alt(w, 'r');                              // the deliberate choice
      CHECK(!ask.showing() && fs.stat("/Linux/opt/notes", &e) == filesystem::Status::NotFound && m.status().equals("\"notes\" was removed."));
      // A failure: a message that says what happened, why and what to do, titled by the application; the status line has it too.
      m.select_row(1);
      CHECK(bar.find_item("Application", "Install")->enabled() && choose(w, bar, "Application", "Install"));
      CHECK(notices.showing() && notices.kind() == NoticeKind::Error && notices.title().equals("App Manager"));
      CHECK(notices.message().starts_with("\"paint-x\" could not be installed because ") && notices.message().find(StringView("try again")) != StringView::npos);
      CHECK(m.status().equals(notices.message()));
      press(w, key::kEnter);
      CHECK(!notices.showing());
      // View > Installed Only is a toggle with a check mark, in step with the list beside the search box.
      CHECK(!bar.find_item("View", "Installed Only")->checked() && choose(w, bar, "View", "Installed Only"));
      CHECK(bar.find_item("View", "Installed Only")->checked() && m.show_box().selected_index() == desktop::AppManager::kShowInstalled && m.list().item_count() == 0);
      CHECK(choose(w, bar, "Help", "About App Manager") && notices.showing() && notices.title().equals("About App Manager"));
      press(w, key::kEnter);
      CHECK(choose(w, bar, "File", "Exit") && !m.manager());
    }
  }
  CHECK(a.live == 0);
}
