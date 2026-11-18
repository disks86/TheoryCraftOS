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

#include "Hardware/Bluetooth/HidpHost.hpp"
#include "Hardware/Bluetooth/Sdp.hpp"
#include "Hardware/GameController/GameControllerService.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

namespace {
void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text && text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

void announce(IInputSink& sink, const char* label, const char* what) {
  char text[96];
  uint32_t at = 0;
  for (const char* p = label; *p && at + 1 < sizeof text; ++p) text[at++] = *p;
  for (const char* p = what; *p && at + 1 < sizeof text; ++p) text[at++] = *p;
  sink.notify(StringView(text, at));
}

bool channel_open(const L2capChannel* channel, uint16_t handle, uint16_t cid) {
  return channel && channel->state == L2capState::Open && channel->handle == handle && channel->local_cid == cid;
}
}

// ------------------------------------------------------------------ the report link (any thread)

bool HidpReportLink::send_output(ConstByteView report) {
  if (report.empty() || report.size() > kMaxReport) return false;
  kernel::ScopedMutex lock(mutex_);
  if (closing_) return false;
  if (count_ == kQueue) {                                   // full: the newest replaces the last (an output report is a whole state: rumble, lights)
    Slot& last = outputs_[(head_ + count_ - 1) % kQueue];
    ByteView(last.data).first(report.size()).copy_from(report);
    last.length = static_cast<uint32_t>(report.size());
    return true;
  }
  Slot& s = outputs_[(head_ + count_) % kQueue];
  ByteView(s.data).first(report.size()).copy_from(report);
  s.length = static_cast<uint32_t>(report.size());
  ++count_;
  return true;
}

bool HidpReportLink::get_feature(uint8_t id, ByteView out, uint32_t* length) {
  *length = 0;
  uint32_t generation = 0;
  {
    kernel::ScopedMutex lock(mutex_);
    if (closing_ || feature_ != Feature::Idle) return false;
    feature_ = Feature::Queued;
    feature_id_ = id;
    generation = ++feature_generation_;
  }
  // The adapter's thread sends it on its next poll and completes it when the answer comes or 10 polls pass; this thread waits. With threads running the wait has its own limit
  // too (the adapter's thread may itself be waiting for something this thread holds); before then the adapter's limit is the only one.
  for (uint32_t waited = 0;; ++waited) {
    {
      kernel::ScopedMutex lock(mutex_);
      if (feature_generation_ != generation) return false;
      if (feature_ == Feature::Done) {
        const ConstByteView answer = ConstByteView(feature_data_).first(feature_length_).at_most(out.size());
        out.first(answer.size()).copy_from(answer);
        *length = static_cast<uint32_t>(answer.size());
        feature_ = Feature::Idle;
        return true;
      }
      if (feature_ == Feature::Failed || closing_) {
        feature_ = Feature::Idle;
        return false;
      }
      if (kernel::running() && waited >= kFeatureWaitMs) {
        feature_ = Feature::Idle;                           // (given up: an answer that comes later finds nobody waiting and is dropped)
        return false;
      }
    }
    if (kernel::running()) kernel::sleep_us(1000);
    else kernel::yield();
  }
}

bool HidpReportLink::set_report(uint8_t type, ConstByteView report) {
  if (type < 1 || type > 3 || report.empty() || report.size() > kMaxReport) return false;
  kernel::ScopedMutex lock(mutex_);
  if (closing_ || set_pending_) return false;
  set_pending_ = true;
  set_type_ = type;
  ByteView(set_data_).first(report.size()).copy_from(report);
  set_length_ = static_cast<uint32_t>(report.size());
  return true;
}

uint32_t HidpReportLink::set_reports_failed() const {
  kernel::ScopedMutex lock(mutex_);
  return set_failed_;
}

bool HidpReportLink::take_output(ByteView out, uint32_t* length) {
  kernel::ScopedMutex lock(mutex_);
  if (count_ == 0) return false;
  const Slot& s = outputs_[head_];
  const ConstByteView data = ConstByteView(s.data).first(s.length).at_most(out.size());
  out.first(data.size()).copy_from(data);
  *length = static_cast<uint32_t>(data.size());
  head_ = (head_ + 1) % kQueue;
  --count_;
  return true;
}

HidpReportLink::Control HidpReportLink::next_control(uint8_t* header, ByteView out, uint32_t* length) {
  kernel::ScopedMutex lock(mutex_);
  *length = 0;
  if (in_flight_ != Control::None || closing_) return Control::None;
  if (feature_ == Feature::Queued) {
    feature_ = Feature::Sent;
    in_flight_ = Control::GetFeature;
    *header = static_cast<uint8_t>(hidp::kGetReport | hidp::kReportFeature);
    if (feature_id_ != 0 && !out.empty()) {               // (the id only when the device numbers its reports; 0 is no id)
      out[0] = feature_id_;
      *length = 1;
    }
    return in_flight_;
  }
  if (set_pending_ && set_length_ <= out.size()) {
    set_pending_ = false;
    in_flight_ = Control::SetReport;
    *header = static_cast<uint8_t>(hidp::kSetReport | set_type_);
    out.first(set_length_).copy_from(ConstByteView(set_data_).first(set_length_));
    *length = set_length_;
    return in_flight_;
  }
  return Control::None;
}

bool HidpReportLink::control_in_flight() const {
  kernel::ScopedMutex lock(mutex_);
  return in_flight_ != Control::None;
}

void HidpReportLink::control_answer(bool ok, ConstByteView data) {
  kernel::ScopedMutex lock(mutex_);
  if (in_flight_ == Control::GetFeature && feature_ == Feature::Sent) {
    feature_ = ok ? Feature::Done : Feature::Failed;
    const ConstByteView kept = data.at_most(kMaxReport);
    ByteView(feature_data_).first(kept.size()).copy_from(kept);
    feature_length_ = static_cast<uint32_t>(kept.size());
  } else if (in_flight_ == Control::SetReport && !ok) {
    ++set_failed_;
  }
  in_flight_ = Control::None;
}

void HidpReportLink::handshake(uint8_t result) {
  kernel::ScopedMutex lock(mutex_);
  if (in_flight_ == Control::GetFeature && feature_ == Feature::Sent) feature_ = Feature::Failed;      // (a feature read is answered with data; a handshake is a refusal)
  else if (in_flight_ == Control::SetReport && result != hidp::kHandshakeSuccessful) ++set_failed_;
  else if (in_flight_ == Control::None) return;                                                       // (unasked: ignored)
  in_flight_ = Control::None;
}

void HidpReportLink::close() {
  kernel::ScopedMutex lock(mutex_);
  closing_ = true;
  count_ = 0;
  set_pending_ = false;
  if (feature_ == Feature::Queued || feature_ == Feature::Sent) feature_ = Feature::Failed;
  in_flight_ = Control::None;
}

// ------------------------------------------------------------------ a device (the adapter's thread)

struct HidpHost::Device final : IL2capChannelHandler {
  enum class State : uint8_t { WaitIncoming, Opening, Up, Down };
  enum class Kind : uint8_t { Keyboard, Consumer, Pointer };
  struct Route {
    uint8_t id = 0;
    Kind kind = Kind::Keyboard;
    hid::KeyboardDecoder keyboard;
    hid::ConsumerDecoder consumer;
    hid::PointerDecoder pointer;
  };

  Device(HidpHost& owner, allocators::IAllocator& allocator) : host(owner), descriptor(allocator) {}

  HidpHost& host;
  uint32_t remote_id = 0;
  uint16_t acl = 0;
  IBluetoothClassicRemote* classic = nullptr;     // (valid between attach and detach)
  State state = State::WaitIncoming;
  uint32_t ticks = 0, control_ticks = 0;
  L2capChannel* control = nullptr;
  L2capChannel* interrupt = nullptr;
  uint16_t control_cid = 0, interrupt_cid = 0;
  ByteBuffer descriptor;
  UniquePointer<hid::ReportMap> map;
  bool numbered = false, pad = false, session = false, tearing_down = false;
  char name[40] = {};
  BluetoothPadIdentity identity;
  UniquePointer<HidpReportLink> link;
  SharedPointer<HidPadChannel> channel;
  int32_t record = -1;
  Route routes[kMaxRoutes];
  uint32_t route_count = 0;

  bool is_control(const L2capChannel& c) const { return c.handle == acl && control_cid != 0 && c.local_cid == control_cid; }
  bool is_interrupt(const L2capChannel& c) const { return c.handle == acl && interrupt_cid != 0 && c.local_cid == interrupt_cid; }

  void on_open(L2capChannel& c) override {
    if (is_control(c) && state == State::Opening && !interrupt) {           // control first, then interrupt (HID Profile 1.1, 7.2)
      interrupt = classic ? classic->l2cap().connect(acl, hidp::kPsmInterrupt, this) : nullptr;
      interrupt_cid = interrupt ? interrupt->local_cid : 0;
    }
  }
  void on_data(L2capChannel& c, ConstByteView sdu) override {
    if (is_control(c)) host.on_control(*this, sdu);
    else if (is_interrupt(c)) host.on_interrupt(*this, sdu);
  }
  void on_close(L2capChannel& c) override {
    const bool was_control = is_control(c), was_interrupt = is_interrupt(c);
    if (!was_control && !was_interrupt) return;
    if (was_control) { control = nullptr; control_cid = 0; }
    if (was_interrupt) { interrupt = nullptr; interrupt_cid = 0; }
    if (tearing_down) return;
    // The device closed a channel of the session (it is switching off, or it refused one): the session is over, and so is the link.
    if (session) host.end_session(*this, true);
    state = State::Down;
    if (classic) classic->request_disconnect(false);
  }
};

HidpHost::~HidpHost() {
  for (UniquePointer<Device>& d : devices_) {
    if (!d) continue;
    d->tearing_down = true;
    if (d->link) d->link->close();
    if (d->channel) d->channel->detach();
  }
}

HidpHost::Device* HidpHost::device_for(uint32_t remote_id) {
  for (UniquePointer<Device>& d : devices_)
    if (d && d->remote_id == remote_id) return d.get();
  return nullptr;
}

HidpReportLink* HidpHost::link(uint32_t remote_id) {
  Device* d = device_for(remote_id);
  return d ? d->link.get() : nullptr;
}

HidpReportLink* HidpHost::link_at(uint32_t index) {
  return index < kMaxDevices && devices_[index] ? devices_[index]->link.get() : nullptr;
}

bool HidpHost::matches(const BluetoothDeviceInfo& info, const GattDatabase&) const { return !info.le; }

uint32_t HidpHost::incoming_psms(ArrayView<uint16_t> out) const {
  if (out.size() < 2) return 0;
  out[0] = hidp::kPsmControl;
  out[1] = hidp::kPsmInterrupt;
  return 2;
}

// The HID record's report map and the PnP record's ids (the second is optional: many pads have it, keyboards often not).
bool HidpHost::read_records(IBluetoothClassicRemote& classic, Device& device) {
  SdpClient sdp(classic.l2cap(), classic.controller());
  ByteBuffer answer(allocator_);
  if (sdp.search_attributes(classic.acl_handle(), sdp::kServiceClassHid, answer) != SdpResult::Ok) return false;
  sdp::Element list;
  ConstByteView map;
  if (!sdp::find_in_records(answer.view(), sdp::kAttributeHidDescriptorList, &list) || !sdp::hid_report_descriptor(list, &map) || map.size() > kMaxMap) return false;
  if (!device.descriptor.append(map)) return false;
  if (sdp.search_attributes(classic.acl_handle(), sdp::kServiceClassPnp, answer) == SdpResult::Ok) {
    sdp::Element value;
    uint32_t number = 0;
    if (sdp::find_in_records(answer.view(), sdp::kAttributeVendorId, &value) && sdp::to_uint(value, &number)) device.identity.vendor_id = static_cast<uint16_t>(number);
    if (sdp::find_in_records(answer.view(), sdp::kAttributeProductId, &value) && sdp::to_uint(value, &number)) device.identity.product_id = static_cast<uint16_t>(number);
  }
  return true;
}

// Which of the device's input reports become keys and pointer movements: the first keyboard, consumer control and pointer report of its map.
bool HidpHost::plan_input(Device& d) {
  bool keyboard = false, consumer = false, pointer = false;
  for (uint32_t i = 0; i < d.map->report_count && d.route_count < kMaxRoutes; ++i) {
    const hid::InputReport& report = d.map->reports[i];
    Device::Route& route = d.routes[d.route_count];
    route = Device::Route();
    route.id = report.id;
    if (report.app_page == hid::kPageGenericDesktop && report.app_usage == hid::kUsageKeyboard && !keyboard) {
      if (!route.keyboard.init(report)) continue;
      route.kind = Device::Kind::Keyboard;
      keyboard = true;
    } else if (report.app_page == hid::kPageConsumer && report.app_usage == 1 && !consumer) {
      if (!route.consumer.init(report)) continue;
      route.kind = Device::Kind::Consumer;
      consumer = true;
    } else if (report.app_page == hid::kPageGenericDesktop && (report.app_usage == hid::kUsageMouse || report.app_usage == hid::kUsagePointer) && !pointer) {
      if (!route.pointer.init(report)) continue;
      route.kind = Device::Kind::Pointer;
      pointer = true;
    } else {
      continue;
    }
    ++d.route_count;
  }
  if (d.route_count == 0) return false;
  kernel::ScopedMutex lock(mutex_);
  for (uint32_t i = 0; i < kMaxDevices; ++i) {
    InputRecord& r = records_[i];
    if (r.used) continue;
    r.clear();
    r.used = true;
    copy_text(r.label, sizeof r.label, d.name[0] ? d.name : "Bluetooth device");
    if (keyboard || consumer) r.keyboard = UniquePointer<BluetoothHidKeyboard>::make(allocator_);
    if (pointer) {
      r.mouse = UniquePointer<BluetoothHidMouse>::make(allocator_);
      for (uint32_t k = 0; k < d.route_count && r.mouse; ++k)
        if (d.routes[k].kind == Device::Kind::Pointer) r.mouse->configure(d.routes[k].pointer.absolute(), d.routes[k].pointer.max_x(), d.routes[k].pointer.max_y());
    }
    if ((keyboard || consumer) != (r.keyboard != nullptr) || pointer != (r.mouse != nullptr)) { r.clear(); return false; }
    d.record = static_cast<int32_t>(i);
    return true;
  }
  return false;
}

bool HidpHost::attach(IBluetoothRemote& remote) {
  IBluetoothClassicRemote* classic = remote.classic();
  if (!classic || device_for(remote.id())) return false;
  UniquePointer<Device>* slot = nullptr;
  for (UniquePointer<Device>& d : devices_)
    if (!d) { slot = &d; break; }
  if (!slot) return false;
  UniquePointer<Device> device = UniquePointer<Device>::make(allocator_, *this, allocator_);
  if (!device) return false;
  Device& d = *device;
  d.remote_id = remote.id();
  d.acl = classic->acl_handle();
  d.classic = classic;
  const char* name = remote.info().name;
  if (!name[0])
    if (const HciDevice* known = classic->controller().find_device(remote.info().address)) name = known->name;   // (the name request may have been answered since)
  copy_text(d.name, sizeof d.name, name);
  d.identity.name = d.name;
  d.identity.le = false;
  d.map = UniquePointer<hid::ReportMap>::make(allocator_);
  d.link = UniquePointer<HidpReportLink>::make(allocator_);
  if (!d.map || !d.link) return false;
  if (!read_records(*classic, d)) return false;                      // (no HID record: not a HID device, or its record cannot be read)
  if (!hid::parse_report_map(d.descriptor.view(), d.map.get())) return false;
  for (uint32_t i = 0; i < d.map->report_count; ++i) d.numbered = d.numbered || d.map->reports[i].id != 0;
  d.pad = game_controllers_ && is_bluetooth_gamepad(d.identity, d.descriptor.view());
  if (!d.pad && !plan_input(d)) return false;                                 // (neither a pad nor keys or a pointer: nothing to serve)
  if (classic->incoming()) {
    d.state = Device::State::WaitIncoming;                                    // (the device opens the channels itself; service() takes them)
  } else {
    d.state = Device::State::Opening;
    d.control = classic->l2cap().connect(d.acl, hidp::kPsmControl, &d);
    d.control_cid = d.control ? d.control->local_cid : 0;
    if (!d.control) {
      if (d.record >= 0) { kernel::ScopedMutex lock(mutex_); records_[d.record].clear(); }
      return false;
    }
  }
  *slot = util::move(device);
  return true;
}

void HidpHost::start_session(Device& d) {
  d.session = true;
  d.state = Device::State::Up;
  ++sessions_;
  if (d.pad) {
    d.channel = SharedPointer<HidPadChannel>::make(allocator_);
    if (!d.channel) return;
    d.channel->attach(d.link.get());
    UniquePointer<IGameController> pad = make_bluetooth_gamepad(allocator_, d.channel, d.identity, d.descriptor.view());
    if (pad && game_controllers_) {
      game_controllers_->add(util::move(pad));                               // (-1: every slot taken; the service keeps it waiting or lets it go, and present() tells)
      ++pads_;
    } else {
      d.channel->detach();
      d.channel = SharedPointer<HidPadChannel>();
    }
    return;
  }
  kernel::ScopedMutex lock(mutex_);
  if (d.record >= 0) records_[d.record].ready = true;
}

// The pad's channel detached (after the link is closed, so a feature read in progress returns and nothing calls in again) and the input devices sent away. With `device_left`
// the device ended it.
void HidpHost::end_session(Device& d, bool device_left) {
  const bool was = d.session;
  d.session = false;
  if (was && sessions_ > 0) --sessions_;
  if (d.link) d.link->close();
  if (d.channel) {
    d.channel->detach();
    d.channel = SharedPointer<HidPadChannel>();
    if (pads_ > 0) --pads_;
  }
  kernel::ScopedMutex lock(mutex_);
  if (d.record >= 0) {
    if (records_[d.record].used) records_[d.record].gone = true;
    d.record = -1;
  }
  (void)device_left;
}

void HidpHost::close_channels(Device& d) {
  d.tearing_down = true;
  if (!d.classic) return;
  L2cap& l2 = d.classic->l2cap();
  // Interrupt first, then control (the order the profile gives). A channel still being opened is closed too.
  if (d.interrupt && d.interrupt->state != L2capState::Closed && d.interrupt->handle == d.acl && d.interrupt->local_cid == d.interrupt_cid) l2.close(*d.interrupt);
  if (d.control && d.control->state != L2capState::Closed && d.control->handle == d.acl && d.control->local_cid == d.control_cid) l2.close(*d.control);
  d.interrupt = d.control = nullptr;
  d.interrupt_cid = d.control_cid = 0;
}

void HidpHost::detach(IBluetoothRemote& remote) {
  for (UniquePointer<Device>& slot : devices_) {
    if (!slot || slot->remote_id != remote.id()) continue;
    Device& d = *slot;
    d.tearing_down = true;
    end_session(d, false);
    {
      kernel::ScopedMutex lock(mutex_);
      if (d.record >= 0 && records_[d.record].used) records_[d.record].gone = true;      // (a device whose session never came up)
      d.record = -1;
    }
    if (remote.connected()) close_channels(d);
    d.classic = nullptr;
    slot.reset();
  }
}

// ------------------------------------------------------------------ the adapter's thread, every poll

void HidpHost::service(IBluetoothRemote& remote) {
  Device* found = device_for(remote.id());
  if (!found || !found->classic) return;
  Device& d = *found;
  L2cap& l2 = d.classic->l2cap();
  switch (d.state) {
    case Device::State::WaitIncoming: {
      if (!d.control) {
        d.control = d.classic->take_incoming(hidp::kPsmControl, &d);
        d.control_cid = d.control ? d.control->local_cid : 0;
      }
      if (!d.interrupt) {
        d.interrupt = d.classic->take_incoming(hidp::kPsmInterrupt, &d);
        d.interrupt_cid = d.interrupt ? d.interrupt->local_cid : 0;
      }
      if (d.control && d.interrupt) { start_session(d); return; }
      if (++d.ticks <= kIncomingWaitTicks) return;
      d.ticks = 0;                                                                     // (it has not opened them: we do, from where it got to)
      d.state = Device::State::Opening;
      if (!d.control) {
        d.control = l2.connect(d.acl, hidp::kPsmControl, &d);
        d.control_cid = d.control ? d.control->local_cid : 0;
      } else {
        d.interrupt = l2.connect(d.acl, hidp::kPsmInterrupt, &d);
        d.interrupt_cid = d.interrupt ? d.interrupt->local_cid : 0;
      }
      if (!d.control && !d.interrupt) { d.state = Device::State::Down; d.classic->request_disconnect(false); }
      return;
    }
    case Device::State::Opening:
      if (channel_open(d.control, d.acl, d.control_cid) && channel_open(d.interrupt, d.acl, d.interrupt_cid)) { start_session(d); return; }
      if (++d.ticks > kOpenTicks) {
        close_channels(d);
        d.state = Device::State::Down;
        d.classic->request_disconnect(false);
      }
      return;
    case Device::State::Down:
      return;
    case Device::State::Up:
      break;
  }
  // Output reports on the interrupt channel.
  uint8_t packet[1 + HidpReportLink::kMaxReport];
  uint32_t length = 0;
  for (uint32_t i = 0; i < HidpReportLink::kQueue && d.link->take_output(ByteView(packet).subview(1), &length); ++i) {
    packet[0] = static_cast<uint8_t>(hidp::kData | hidp::kReportOutput);
    if (channel_open(d.interrupt, d.acl, d.interrupt_cid)) (void)l2.send(*d.interrupt, ConstByteView(packet).first(1 + length));
  }
  // One control transaction at a time, given up after kControlTicks.
  if (d.link->control_in_flight()) {
    if (++d.control_ticks > kControlTicks) d.link->control_answer(false, ConstByteView());
    return;
  }
  uint8_t header = 0;
  if (d.link->next_control(&header, ByteView(packet).subview(1), &length) == HidpReportLink::Control::None) return;
  d.control_ticks = 0;
  packet[0] = header;
  if (!channel_open(d.control, d.acl, d.control_cid) || l2.send(*d.control, ConstByteView(packet).first(1 + length)) != HciResult::Ok) d.link->control_answer(false, ConstByteView());
}

void HidpHost::on_control(Device& d, ConstByteView sdu) {
  if (sdu.empty()) { ++malformed_; return; }
  const uint8_t type = sdu[0] & 0xF0, parameter = sdu[0] & 0x0F;
  switch (type) {
    case hidp::kHandshake:
      if (d.link) d.link->handshake(parameter);
      return;
    case hidp::kHidControl:
      if (parameter == hidp::kControlVirtualCableUnplug) {                             // the device asks to be forgotten (it was paired with something else, or reset)
        ++unplugged_;
        if (d.session) end_session(d, true);
        d.state = Device::State::Down;
        if (d.classic) d.classic->request_disconnect(true);
      }
      return;
    case hidp::kData:
      if (parameter == hidp::kReportFeature && d.link) d.link->control_answer(true, sdu.subview(1));
      return;
    default:
      return;
  }
}

void HidpHost::on_interrupt(Device& d, ConstByteView sdu) {
  if (sdu.empty() || sdu[0] != (hidp::kData | hidp::kReportInput)) { ++malformed_; return; }
  if (!d.session) return;
  const ConstByteView report = sdu.subview(1);
  ++input_reports_;
  if (d.channel) { d.channel->deliver(report); return; }
  uint8_t id = 0;
  ConstByteView data = report;
  if (d.numbered) {
    if (report.empty()) { ++malformed_; return; }
    id = report[0];
    data = report.subview(1);
  }
  kernel::ScopedMutex lock(mutex_);
  if (d.record < 0) return;
  InputRecord& r = records_[d.record];
  if (r.gone) return;
  for (uint32_t i = 0; i < d.route_count; ++i) {
    Device::Route& route = d.routes[i];
    if (route.id != id) continue;
    if (route.kind == Device::Kind::Keyboard) {
      hid::KeyboardState state;
      if (r.keyboard && route.keyboard.decode(data, &state)) r.keyboard->deliver(state);
    } else if (route.kind == Device::Kind::Consumer) {
      hid::KeyboardState state;
      if (r.keyboard && route.consumer.decode(data, &state)) r.keyboard->deliver_consumer(state);
    } else {
      MouseEvent event;
      if (r.mouse && route.pointer.decode(data, &event)) r.mouse->deliver(event);
    }
    return;
  }
}

// ------------------------------------------------------------------ the input system's thread

uint32_t HidpHost::poll(IInputSink& sink) {
  uint32_t changes = 0;
  kernel::ScopedMutex lock(mutex_);
  for (InputRecord& r : records_) {
    if (!r.used || (!r.ready && !r.gone)) continue;
    if (r.gone) {
      if (r.keyboard_added && r.keyboard) { r.keyboard->release_all(); sink.remove_keyboard(r.keyboard.get()); }
      if (r.mouse_added && r.mouse) { r.mouse->release_all(); sink.remove_mouse(r.mouse.get()); }
      if (r.keyboard_added || r.mouse_added) { announce(sink, r.label, ": disconnected"); ++changes; }
      r.clear();
      continue;
    }
    if (r.keyboard && !r.keyboard_added) r.keyboard_added = sink.add_keyboard(r.keyboard.get());
    if (r.mouse && !r.mouse_added) r.mouse_added = sink.add_mouse(r.mouse.get());
    if ((r.keyboard_added || r.mouse_added) && !r.announced) {
      r.announced = true;
      announce(sink, r.label, ": connected");
      ++changes;
    }
  }
  return changes;
}

void HidpHost::detach_all(IInputSink& sink) {
  {
    kernel::ScopedMutex lock(mutex_);
    for (InputRecord& r : records_)
      if (r.used) r.gone = true;
  }
  poll(sink);
}

}  // namespace hardware
