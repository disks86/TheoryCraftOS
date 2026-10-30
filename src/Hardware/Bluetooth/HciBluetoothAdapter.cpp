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

#include "Hardware/Bluetooth/HciBluetoothAdapter.hpp"

namespace hardware {

namespace {
void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

void append_text(char* out, uint32_t capacity, const char* text) {
  uint32_t at = 0;
  while (out[at] && at + 1 < capacity) ++at;
  if (at > 0 && at + 2 < capacity) out[at++] = ',';
  copy_text(out + at, capacity - at, text);
}

const char* pairing_failure(uint8_t reason) {
  switch (reason) {
    case 0x01: return "wrong passkey";
    case 0x02: case 0x03: return "the device needs a pairing method this system cannot do";
    case 0x04: return "the pairing confirmation did not match";
    case 0x05: return "the device does not support pairing";
    case 0x06: return "the device's key size is too small";
    case 0x0B: return "the pairing key exchange failed";
    case 0x0C: return "the numbers shown did not match";
    default: return "pairing failed";
  }
}
}

// ------------------------------------------------------------------ a connected device

class HciBluetoothAdapter::Remote final : public IBluetoothRemote {
 public:
  static constexpr uint32_t kRoutes = 16;
  Remote(HciBluetoothAdapter& owner, uint32_t id, uint16_t link, const BluetoothDeviceInfo& info) : owner_(owner), id_(id), link_(link), info_(info) {}

  const BluetoothDeviceInfo& info() const override { return info_; }
  uint32_t id() const override { return id_; }
  bool connected() const override { return owner_.controller_.link(link_) != nullptr; }
  bool encrypted() const override {
    const HciLink* l = owner_.controller_.link(link_);
    return l && l->encrypted;
  }
  const GattDatabase& database() const override { return db_; }
  AttStatus read(int characteristic, ByteView out, uint32_t* length) override { return owner_.gatt_.read(link_, db_, characteristic, out, length); }
  AttStatus read_descriptor(uint16_t handle, ByteView out, uint32_t* length) override { return owner_.att_.read(link_, handle, out, length); }
  AttStatus write(int characteristic, ConstByteView value, bool with_response) override { return owner_.gatt_.write(link_, db_, characteristic, value, with_response); }
  AttStatus subscribe(int characteristic, bool indications, IBluetoothNotificationSink* sink) override {
    if (characteristic < 0 || static_cast<uint32_t>(characteristic) >= db_.characteristic_count) return AttStatus::Malformed;
    const uint16_t handle = db_.characteristics[characteristic].value_handle;
    if (!sink) {
      for (Route& r : routes_)
        if (r.sink && r.handle == handle) r = Route();
      return owner_.gatt_.subscribe(link_, db_, characteristic, false, false);
    }
    Route* slot = nullptr;
    for (Route& r : routes_) {
      if (r.sink && r.handle == handle) { slot = &r; break; }
      if (!r.sink && !slot) slot = &r;
    }
    if (!slot) return AttStatus::Busy;
    slot->handle = handle;
    slot->sink = sink;
    const AttStatus status = owner_.gatt_.subscribe(link_, db_, characteristic, !indications, indications);
    if (status != AttStatus::Ok) *slot = Route();
    return status;
  }

  struct Route { uint16_t handle = 0; IBluetoothNotificationSink* sink = nullptr; };
  HciBluetoothAdapter& owner_;
  uint32_t id_;
  uint16_t link_;
  BluetoothDeviceInfo info_;
  GattDatabase db_;
  Route routes_[kRoutes];
  bool attached_[BluetoothProfileRegistry::kMaxProfiles] = {};
};

// ------------------------------------------------------------------ the adapter

HciBluetoothAdapter::HciBluetoothAdapter(HciController& controller, L2cap& l2cap, AttClient& att, GattClient& gatt, SecurityManager* security, SmBondStore& bonds,
                                         BluetoothProfileRegistry& profiles, allocators::IAllocator& allocator)
    : controller_(controller), l2cap_(l2cap), att_(att), gatt_(gatt), security_(security), bonds_(bonds), profiles_(profiles), allocator_(allocator) {
  att_.set_notification_handler(&HciBluetoothAdapter::on_notification, this);
}

HciBluetoothAdapter::~HciBluetoothAdapter() {
  shutdown();
  att_.set_notification_handler(nullptr, nullptr);
  if (security_) security_->set_user_interface(nullptr);
}

void HciBluetoothAdapter::shutdown() {
  for (UniquePointer<Remote>& r : remotes_)
    if (r) drop_remote(r.get());
  op_ = Operation();
}

uint32_t HciBluetoothAdapter::connected_count() const {
  uint32_t n = 0;
  for (const UniquePointer<Remote>& r : remotes_) n += r != nullptr;
  return n;
}

HciBluetoothAdapter::Remote* HciBluetoothAdapter::remote_for_link(uint16_t link) const {
  for (const UniquePointer<Remote>& r : remotes_)
    if (r && r->link_ == link) return r.get();
  return nullptr;
}

HciBluetoothAdapter::Remote* HciBluetoothAdapter::remote_for_address(const hci::BdAddr& address) const {
  for (const UniquePointer<Remote>& r : remotes_)
    if (r && r->info_.address == address) return r.get();
  return nullptr;
}

const hci::BdAddr* HciBluetoothAdapter::address_of_link(uint16_t link) const {
  const HciLink* l = controller_.link(link);
  return l ? &l->address : nullptr;
}

void HciBluetoothAdapter::remember_failure(const hci::BdAddr& address, const char* text) {
  Failure* slot = nullptr;
  for (Failure& f : failures_) {
    if (f.used && f.address == address) { slot = &f; break; }
    if (!f.used && !slot) slot = &f;
  }
  if (!slot) slot = &failures_[0];
  slot->used = true;
  slot->address = address;
  slot->text = text;
}

void HciBluetoothAdapter::forget_failure(const hci::BdAddr& address) {
  for (Failure& f : failures_)
    if (f.used && f.address == address) f.used = false;
}

void HciBluetoothAdapter::fail(const char* text) {
  remember_failure(op_.address, text);
  if (op_.link != 0 && controller_.link(op_.link)) controller_.disconnect(op_.link);
  op_ = Operation();
}

// ------------------------------------------------------------------ the list

void HciBluetoothAdapter::fill(const HciDevice& d, BluetoothDeviceInfo* out) const {
  *out = BluetoothDeviceInfo();
  out->address = d.address;
  out->address_type = d.address_type;
  out->le = d.le;
  copy_text(out->name, sizeof out->name, d.name);
  out->rssi = d.rssi;
  out->class_of_device = d.class_of_device;
  out->appearance = d.appearance;
  out->connectable = d.connectable;
  const SmBond* bond = d.le ? bonds_.find(d.address, d.address_type) : nullptr;
  out->paired = bond != nullptr;
  out->authenticated = bond && bond->authenticated;
  if (const Remote* r = remote_for_address(d.address)) {
    out->state = BluetoothLinkState::Connected;
    if (r->info_.name[0] && !out->name[0]) copy_text(out->name, sizeof out->name, r->info_.name);
    copy_text(out->profiles, sizeof out->profiles, r->info_.profiles);
    return;
  }
  if (op_.phase != Phase::Idle && op_.address == d.address) {
    out->state = op_.phase == Phase::Connecting ? BluetoothLinkState::Connecting : op_.phase == Phase::Setup ? BluetoothLinkState::Setup : BluetoothLinkState::Pairing;
    return;
  }
  for (const Failure& f : failures_)
    if (f.used && f.address == d.address) { out->state = BluetoothLinkState::Failed; out->failure = f.text; }
}

bool HciBluetoothAdapter::device(uint32_t index, BluetoothDeviceInfo* out) const {
  const HciDevice* d = controller_.device(index);
  if (!d) return false;
  fill(*d, out);
  return true;
}

bool HciBluetoothAdapter::find_device(const hci::BdAddr& address, BluetoothDeviceInfo* out) const {
  const HciDevice* d = controller_.find_device(address);
  if (!d) return false;
  fill(*d, out);
  return true;
}

// ------------------------------------------------------------------ discovery and operations

BluetoothResult HciBluetoothAdapter::start_discovery() {
  if (!ready()) return BluetoothResult::NotReady;
  if (op_.phase == Phase::Connecting) return BluetoothResult::Busy;
  const HciResult le = controller_.start_le_scan(true);
  if (controller_.info().classic_supported) controller_.start_inquiry(8);
  return le == HciResult::Ok || controller_.inquiring() ? BluetoothResult::Ok : BluetoothResult::Unsupported;
}

void HciBluetoothAdapter::stop_discovery() {
  if (controller_.le_scanning()) controller_.stop_le_scan();
  if (controller_.inquiring()) controller_.cancel_inquiry();
}

BluetoothResult HciBluetoothAdapter::connect(const hci::BdAddr& address, bool pair) {
  if (!ready()) return BluetoothResult::NotReady;
  const HciDevice* d = controller_.find_device(address);
  if (!d) return BluetoothResult::NoSuchDevice;
  if (!d->le) return BluetoothResult::Unsupported;
  if (remote_for_address(address)) return BluetoothResult::Ok;
  if (op_.phase != Phase::Idle) return BluetoothResult::Busy;
  if (connected_count() >= kMaxRemotes) return BluetoothResult::Busy;
  if (controller_.le_scanning()) controller_.stop_le_scan();            // (some controllers cannot connect while scanning)
  forget_failure(address);
  op_ = Operation();
  op_.address = address;
  op_.type = d->address_type;
  op_.pair = pair;
  if (controller_.connect_le(address, d->address_type) != HciResult::Ok) {
    remember_failure(address, "the controller refused to connect");
    return BluetoothResult::Ok;                                          // (the failure is in the device's state)
  }
  op_.phase = Phase::Connecting;
  return BluetoothResult::Ok;
}

BluetoothResult HciBluetoothAdapter::disconnect(const hci::BdAddr& address) {
  if (Remote* r = remote_for_address(address)) {
    const uint16_t link = r->link_;
    drop_remote(r);
    if (controller_.link(link)) controller_.disconnect(link);
    return BluetoothResult::Ok;
  }
  if (op_.phase != Phase::Idle && op_.address == address) {
    if (op_.phase == Phase::Connecting) controller_.cancel_connect_le();
    else if (op_.phase == Phase::Pairing && security_) security_->abort_pairing();
    if (op_.link != 0 && controller_.link(op_.link)) controller_.disconnect(op_.link);
    op_ = Operation();
    return BluetoothResult::Ok;
  }
  return BluetoothResult::NoSuchDevice;
}

BluetoothResult HciBluetoothAdapter::forget(const hci::BdAddr& address) {
  const BluetoothResult r = disconnect(address);
  const HciDevice* d = controller_.find_device(address);
  bool removed = false;
  if (d) removed = bonds_.remove(d->address, d->address_type);
  const SmBond* by_identity = nullptr;
  for (uint32_t i = 0; i < SmBondStore::kMaxBonds; ++i) {
    by_identity = bonds_.at(i);
    if (by_identity && by_identity->has_identity && by_identity->identity == address) { bonds_.remove(by_identity->address, by_identity->address_type); removed = true; }
  }
  forget_failure(address);
  return r == BluetoothResult::Ok || removed ? BluetoothResult::Ok : BluetoothResult::NoSuchDevice;
}

void HciBluetoothAdapter::set_auto_reconnect(bool on) {
  auto_reconnect_ = on;
  reconnect_seen_ = 0;
  for (uint32_t i = 0; i < controller_.device_count(); ++i)          // (only advertisements heard from now on count)
    if (controller_.device(i)->last_seen > reconnect_seen_) reconnect_seen_ = controller_.device(i)->last_seen;
}

void HciBluetoothAdapter::set_pairing_ui(IBluetoothPairingUi* ui) {
  ui_ = ui;
  if (!security_) return;
  SmConfig config;
  config.io_capability = ui ? 4 : 3;               // with a screen and a keyboard the adapter can display a passkey, compare numbers and take one typed
  config.mitm = ui != nullptr;
  security_->set_config(config);
  security_->set_user_interface(ui ? this : nullptr);
}

void HciBluetoothAdapter::passkey_entered(const hci::BdAddr& address, uint32_t passkey) {
  if (security_ && op_.phase != Phase::Idle && op_.address == address) security_->passkey_entered(op_.link, passkey);
}

void HciBluetoothAdapter::confirm(const hci::BdAddr& address, bool accepted) {
  if (security_ && op_.phase != Phase::Idle && op_.address == address) security_->confirm(op_.link, accepted);
}

void HciBluetoothAdapter::display_passkey(uint16_t link, uint32_t passkey) {
  if (ui_ && address_of_link(link)) ui_->display_passkey(*address_of_link(link), passkey);
}
void HciBluetoothAdapter::confirm_numeric(uint16_t link, uint32_t value) {
  if (ui_ && address_of_link(link)) ui_->confirm_numeric(*address_of_link(link), value);
}
void HciBluetoothAdapter::request_passkey(uint16_t link) {
  if (ui_ && address_of_link(link)) ui_->request_passkey(*address_of_link(link));
}

// ------------------------------------------------------------------ notifications

void HciBluetoothAdapter::on_notification(void* self, uint16_t link, uint16_t handle, ConstByteView value, bool indication) {
  HciBluetoothAdapter& a = *static_cast<HciBluetoothAdapter*>(self);
  Remote* remote = a.remote_for_link(link);
  if (!remote) return;
  for (Remote::Route& r : remote->routes_)
    if (r.sink && r.handle == handle) { r.sink->on_notification(*remote, handle, value, indication); return; }
}

// ------------------------------------------------------------------ running

void HciBluetoothAdapter::poll() {
  controller_.poll();
  advance();
  for (UniquePointer<Remote>& r : remotes_)
    if (r && !controller_.link(r->link_)) drop_remote(r.get());                // (the device went away)
  reconnect_bonded();
}

void HciBluetoothAdapter::advance() {
  switch (op_.phase) {
    case Phase::Idle:
      return;
    case Phase::Connecting: {
      const HciLink* l = controller_.find_link(op_.address);
      if (l && l->le) {
        op_.link = l->handle;
        op_.phase = Phase::Securing;
        op_.ticks = 0;
        return;
      }
      if (++op_.ticks > kConnectTicks) {
        controller_.cancel_connect_le();
        fail("could not connect");
      }
      return;
    }
    case Phase::Securing: {
      const HciLink* l = controller_.link(op_.link);
      if (!l) return fail("the link was lost");
      const SmBond* bond = security_ ? bonds_.find(l->address, l->address_type) : nullptr;
      if (security_ && bond && bond->has_ltk) {
        if (security_->encrypt(op_.link) == SmResult::Ok) { op_.phase = Phase::Setup; return; }
        if (!controller_.link(op_.link)) return fail("the link was lost");
        bonds_.remove(bond->address, bond->address_type);                       // (the device no longer honours the key)
      }
      if (security_ && op_.pair && !l->encrypted) {
        op_.phase = Phase::Pairing;
        op_.pairing_started = false;
        op_.ticks = 0;
        return;
      }
      op_.phase = Phase::Setup;
      return;
    }
    case Phase::Pairing: {
      if (!controller_.link(op_.link)) { security_->abort_pairing(); return fail("the link was lost"); }
      if (!op_.pairing_started) {
        if (security_->start_pairing(op_.link) != SmResult::Ok) return fail("pairing could not start");
        op_.pairing_started = true;
        return;
      }
      SmResult result;
      if (security_->pairing_finished(&result)) {
        if (result == SmResult::Ok) { op_.phase = Phase::Setup; return; }
        return fail(pairing_failure(security_->failure_reason()));
      }
      if (++op_.ticks > kPairTicks) {
        security_->abort_pairing();
        fail("pairing timed out");
      }
      return;
    }
    case Phase::Setup:
      setup_remote();
      return;
  }
}

void HciBluetoothAdapter::setup_remote() {
  const HciLink* l = controller_.link(op_.link);
  if (!l) return fail("the link was lost");
  UniquePointer<Remote>* slot = nullptr;
  for (UniquePointer<Remote>& r : remotes_)
    if (!r) { slot = &r; break; }
  if (!slot) return fail("too many devices connected");
  BluetoothDeviceInfo info;
  if (const HciDevice* d = controller_.find_device(op_.address)) fill(*d, &info);
  info.address = op_.address;
  info.address_type = op_.type;
  const uint16_t link = op_.link;
  const hci::BdAddr address = op_.address;
  att_.exchange_mtu(link, 247);                                             // (a device that does not answer keeps the default of 23)
  *slot = UniquePointer<Remote>::make(allocator_, *this, next_id_++, link, info);
  if (!*slot) return fail("out of memory");
  Remote& remote = **slot;
  if (gatt_.discover(link, &remote.db_) != AttStatus::Ok) {
    slot->reset();
    return fail("the device's services could not be read");
  }
  if (!remote.info_.name[0]) {                                              // a name from the device's Generic Access service when the advertisement had none
    const int name = remote.db_.find_characteristic(Uuid::from16(0x2A00));
    uint8_t text[sizeof remote.info_.name];
    uint32_t length = 0;
    if (name >= 0 && gatt_.read(link, remote.db_, name, ByteView(text), &length) == AttStatus::Ok) {
      uint32_t n = 0;
      for (; n < length && n + 1 < sizeof remote.info_.name; ++n) remote.info_.name[n] = static_cast<char>(text[n]);
      remote.info_.name[n] = 0;
    }
  }
  op_ = Operation();                                                        // (the operation is over: profiles may start their own traffic)
  remote.info_.state = BluetoothLinkState::Connected;
  for (uint32_t i = 0; i < BluetoothProfileRegistry::kMaxProfiles; ++i) {
    IBluetoothProfile* profile = profiles_.at(i);
    if (!profile || !profile->matches(remote.info_, remote.db_)) continue;
    if (!profile->attach(remote)) continue;
    remote.attached_[i] = true;
    append_text(remote.info_.profiles, sizeof remote.info_.profiles, profile->name());
  }
  (void)address;
}

void HciBluetoothAdapter::drop_remote(Remote* remote) {
  if (!remote) return;
  for (uint32_t i = 0; i < BluetoothProfileRegistry::kMaxProfiles; ++i) {
    if (!remote->attached_[i]) continue;
    remote->attached_[i] = false;
    if (IBluetoothProfile* p = profiles_.at(i)) p->detach(*remote);
  }
  for (UniquePointer<Remote>& r : remotes_)
    if (r.get() == remote) r.reset();
}

// ------------------------------------------------------------------ reconnecting

void HciBluetoothAdapter::reconnect_bonded() {
  if (!auto_reconnect_ || !ready() || op_.phase != Phase::Idle || bonds_.count() == 0) return;
  if (!controller_.le_scanning()) {
    if (controller_.start_le_scan(false) != HciResult::Ok) return;
  }
  uint32_t newest = reconnect_seen_;
  const hci::BdAddr* target = nullptr;
  uint8_t target_type = 0;
  for (uint32_t i = 0; i < controller_.device_count(); ++i) {
    const HciDevice* d = controller_.device(i);
    if (d->last_seen > newest) newest = d->last_seen;
    if (d->last_seen <= reconnect_seen_ || !d->le || !d->connectable || remote_for_address(d->address)) continue;
    if (bonds_.find(d->address, d->address_type)) { target = &d->address; target_type = d->address_type; }
  }
  reconnect_seen_ = newest;
  (void)target_type;
  if (target && connected_count() < kMaxRemotes) connect(*target, false);
}

}  // namespace hardware
