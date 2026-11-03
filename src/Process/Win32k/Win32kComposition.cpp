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
#include <new>
#include "Process/Win32k/Win32kCompositionInternal.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Utility/UniquePointer.hpp"

// The window manager's input entry points are optional for this module (it must link without them): declared weak, they are null when that module is not in the build.
namespace process::win32k::user {
void inject_key(uint32_t, uint32_t, bool, bool) __attribute__((weak));
void inject_mouse_move(int32_t, int32_t) __attribute__((weak));
void inject_mouse_button(uint32_t, bool) __attribute__((weak));
void inject_mouse_wheel(int32_t, bool) __attribute__((weak));
bool post_message(uint32_t, uint32_t, uint64_t, uint64_t) __attribute__((weak));
bool post_thread_message(uint64_t, uint32_t, uint64_t, uint64_t) __attribute__((weak));
void inject_char(char32_t) __attribute__((weak));
void cursor_position(int32_t*, int32_t*) __attribute__((weak));
}  // namespace process::win32k::user
namespace process::win32k::gdi {
uint32_t screen_width() __attribute__((weak));
uint32_t screen_height() __attribute__((weak));
}  // namespace process::win32k::gdi

namespace process::win32k {
namespace composition {

namespace {
UniquePointer<State> g_state;

bool screen_size_of(uint32_t* width, uint32_t* height) {
  if (!gdi::screen_width || !gdi::screen_height) return false;
  *width = gdi::screen_width();
  *height = gdi::screen_height();
  return *width != 0 && *height != 0;
}
}  // namespace

// --- Memory ------------------------------------------------------------------------------------------------------------------------------------------------------------

MappedMemory* MappedMemory::create(allocators::IAllocator& allocator, uint64_t size) {
  if (size == 0 || size > (64ull << 20) || size % 4096 != 0) return nullptr;
  uint8_t* storage = static_cast<uint8_t*>(allocator.allocate_aligned(size, 4096));
  if (!storage) return nullptr;
  for (uint64_t i = 0; i < size; ++i) storage[i] = 0;
  void* self = allocator.allocate(sizeof(MappedMemory));
  if (!self) { allocator.deallocate(storage); return nullptr; }
  return new (self) MappedMemory(allocator, storage, size);      // (the one place the object is placed: it destroys itself in release())
}
void MappedMemory::release() {
  if (__atomic_fetch_sub(&refs_, 1, __ATOMIC_ACQ_REL) != 1) return;
  allocators::IAllocator& a = allocator_;
  uint8_t* s = storage_;
  this->~MappedMemory();
  a.deallocate(s);
  a.deallocate(this);
}

bool map_into_caller(Call& c, MappedMemory& memory, uint64_t* base) {
  AddressSpace* space = c.process().address_space();
  if (!space) return false;
  uint64_t at = 0;
  if (!space->find_free(memory.size(), 0x00007FF500000000ull, &at)) return false;
  if (!space->map_shared(at, memory.size(), kProtRead | kProtWrite, memory.data(), &memory)) return false;
  PageAttributes attributes;
  attributes.type = PageAttributes::Mapped;
  attributes.protect = attributes.allocation_protect = 0x04;      // PAGE_READWRITE
  space->set_attributes(at, memory.size(), attributes);
  *base = at;
  return true;
}
void unmap_from_caller(Call& c, uint64_t base, uint64_t size) {
  if (AddressSpace* space = c.process().address_space()) if (base != 0) space->unmap(base, size);
}

// --- Queues of raw input ----------------------------------------------------------------------------------------------------------------------------------------------

void RimQueue::push(const RimPacket& p) {
  constexpr uint64_t kLimit = 256;
  if (packets.count() >= kLimit) { packets.remove_at(0); ++overflow; }      // the oldest goes
  packets.push_back(p);
}
bool RimQueue::pop(RimPacket* out) {
  if (packets.count() == 0) return false;
  *out = packets[0];
  packets.remove_at(0);
  return true;
}

// --- Destructors: they run with the system lock held ------------------------------------------------------------------------------------------------------------------

Channel::~Channel() {
  if (memory) memory->release();
  if (completion_event) if (State* s = state()) s->defer_event(completion_event, false, true);
}
Surface::~Surface() {
  State* s = state();
  if (!s) return;
  if (section_id) s->defer_event(section_id, false, true);      // (an ObjectManager object: releasing is the same for every kind)
  if (event_ready) s->defer_event(event_ready, false, true);
  if (event_present) s->defer_event(event_present, false, true);
}
FlipObject::~FlipObject() {
  State* s = state();
  if (!s) return;
  for (uint32_t id : events) s->defer_event(id, false, true);
  for (uint32_t id : lost_for_producer) s->defer_event(id, true, true);
  for (uint32_t id : lost_for_consumer) s->defer_event(id, true, true);
  for (const FlipBuffer& b : buffers) if (b.section) s->defer_event(b.section, false, true);
}
void flip_disconnect(State& s, FlipEndpoint& e) {
  if (e.disconnected || !e.flip) return;
  e.disconnected = true;
  FlipObject& f = *e.flip;
  if (e.consumer) f.consumer_open = false; else f.producer_open = false;
  // the other side hears of it: its lost events fire, and the producer's buffer events too (nothing will take a buffer any more)
  for (uint32_t id : (e.consumer ? f.lost_for_producer : f.lost_for_consumer).view()) s.defer_event(id, true, false);
}
FlipEndpoint::~FlipEndpoint() {
  if (State* s = state()) flip_disconnect(*s, *this);
}
RimRegistration::~RimRegistration() {
  if (State* s = state()) for (uint64_t i = 0; i < s->rim_registrations.count(); ++i) if (s->rim_registrations[i] == this) { s->rim_registrations.remove_at(i); break; }
}
RimObserver::~RimObserver() {
  if (State* s = state()) for (uint64_t i = 0; i < s->rim_observers.count(); ++i) if (s->rim_observers[i] == this) { s->rim_observers.remove_at(i); break; }
}

// --- The state ---------------------------------------------------------------------------------------------------------------------------------------------------------

State::State(allocators::IAllocator& a)
    : allocator(a), objects(system_objects()), handles(a), deferred(a), frames(a), materials(a), rim_registrations(a), rim_observers(a), minuser_threads(a), input_spaces(a),
      minuser_windows(a), navigation(a), resize_syncs(a), navigation_sid(a), touch_active(a), scenarios(a) {
  input = default_input_sink();
}

State::~State() {
  // The objects go first (their destructors queue the events they hold), then the queue is run with nothing locked: the owner of a State shuts it down outside the lock.
  handles.clear();
  resize_syncs.clear();
  if (!objects) return;
  for (uint64_t i = 0; i < deferred.count(); ++i) {
    const OrphanEvent e = deferred[i];
    if (e.signal) objects->set_event(e.id, nullptr);
    if (e.reset) objects->reset_event(e.id, nullptr);
    if (e.release) objects->release(e.id);
  }
  deferred.clear();
}

void State::defer_event(uint32_t id, bool signal, bool release, bool reset) {
  OrphanEvent e;
  e.id = id; e.signal = signal; e.release = release; e.reset = reset;
  if (!deferred.push_back(e) && release) {
    // Out of memory: the reference is given up at once if the lock allows (it is held here, so release_locked).
    if (objects) objects->release_locked(id);
  }
}

State* state() {
  if (g_state) return g_state.get();
  allocators::IAllocator* allocator = system_allocator();
  if (!allocator || !system_objects()) return nullptr;
  g_state = UniquePointer<State>::make(*allocator, *allocator);
  return g_state.get();
}

void shutdown() { g_state.reset(); }

// --- The handle table -------------------------------------------------------------------------------------------------------------------------------------------------

HandleTable::Entry* HandleTable::entry_of(uint64_t handle) {
  if (handle >> 32 != 0 || (handle & 3) != 0 || (handle & kHandleTag) == 0) return nullptr;
  const uint32_t index = uint32_t((handle >> 2) & 0x3FFFF);
  const uint32_t generation = uint32_t((handle >> 20) & 0xFF);
  if (index == 0) return nullptr;
  Entry* e = entries_.get(index - 1);
  if (!e || !e->object || (e->generation & 0xFF) != generation) return nullptr;
  return e;
}
const HandleTable::Entry* HandleTable::entry_of(uint64_t handle) const { return const_cast<HandleTable*>(this)->entry_of(handle); }

uint64_t HandleTable::create(uint64_t pid, SharedPointer<Object> object, uint32_t access) {
  if (!object || live_ >= kMaxTotal || count_for(pid) >= kMaxPerProcess) return 0;
  uint32_t index;
  if (free_.count() != 0) {
    index = free_[free_.count() - 1];
    free_.remove_at(free_.count() - 1);
  } else {
    if (entries_.count() >= 0x3FFFF) return 0;
    Entry blank;
    if (!entries_.push_back(blank)) return 0;
    index = uint32_t(entries_.count() - 1);
  }
  Entry& e = entries_[index];
  e.object = static_cast<SharedPointer<Object>&&>(object);
  e.pid = pid;
  e.access = access;
  ++live_;
  return uint64_t(kHandleTag) | (uint64_t(e.generation & 0xFF) << 20) | (uint64_t(index + 1) << 2);
}

Object* HandleTable::find(uint64_t handle, uint64_t pid, Kind kind, uint32_t required, uint32_t* status, uint32_t* access) {
  Entry* e = entry_of(handle);
  if (!e || e->pid != pid) { *status = kStatusInvalidHandle; return nullptr; }
  if (e->object->kind != kind) { *status = kStatusObjectTypeMismatch; return nullptr; }
  if ((e->access & required) != required) { *status = kStatusAccessDenied; return nullptr; }
  if (access) *access = e->access;
  *status = kStatusSuccess;
  return e->object.get();
}

SharedPointer<Object> HandleTable::share(uint64_t handle, uint64_t pid, Kind kind, uint32_t required, uint32_t* status) {
  Object* o = find(handle, pid, kind, required, status);
  if (!o) return SharedPointer<Object>();
  return entry_of(handle)->object;
}

bool HandleTable::close(uint64_t handle, uint64_t pid) {
  Entry* e = entry_of(handle);
  if (!e || e->pid != pid) return false;
  const uint32_t index = uint32_t((handle >> 2) & 0x3FFFF) - 1;
  SharedPointer<Object> dying = static_cast<SharedPointer<Object>&&>(e->object);
  e->pid = 0;
  e->access = 0;
  e->generation = (e->generation + 1) & 0xFF;
  if (e->generation == 0) e->generation = 1;
  --live_;
  if (!free_.push_back(index)) { /* the slot is simply not reused */ }
  dying.reset();      // (a destructor may run: the table is consistent)
  return true;
}

uint64_t HandleTable::duplicate(uint64_t handle, uint64_t from_pid, uint64_t to_pid, uint32_t access, bool same_access, uint32_t* status) {
  Entry* e = entry_of(handle);
  if (!e || e->pid != from_pid) { *status = kStatusInvalidHandle; return 0; }
  uint32_t wanted = same_access ? e->access : access;
  if (!same_access && (wanted & ~e->access) != 0) { *status = kStatusAccessDenied; return 0; }
  SharedPointer<Object> object = e->object;
  const uint64_t result = create(to_pid, object, wanted);
  *status = result ? kStatusSuccess : kStatusNoMemory;
  return result;
}

uint32_t HandleTable::close_process(uint64_t pid) {
  uint32_t closed = 0;
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    Entry& e = entries_[i];
    if (!e.object || e.pid != pid) continue;
    SharedPointer<Object> dying = static_cast<SharedPointer<Object>&&>(e.object);
    e.pid = 0;
    e.access = 0;
    e.generation = (e.generation + 1) & 0xFF;
    if (e.generation == 0) e.generation = 1;
    --live_;
    free_.push_back(uint32_t(i));
    dying.reset();
    ++closed;
  }
  return closed;
}

void HandleTable::clear() {
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    Entry& e = entries_[i];
    SharedPointer<Object> dying = static_cast<SharedPointer<Object>&&>(e.object);
    e.pid = 0;
    dying.reset();
  }
  live_ = 0;
  entries_.clear();
  free_.clear();
}

void HandleTable::list(Kind kind, List<Info>* out) const {
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    const Entry& e = entries_[i];
    if (!e.object || e.object->kind != kind) continue;
    Info info;
    info.handle = uint64_t(kHandleTag) | (uint64_t(e.generation & 0xFF) << 20) | (uint64_t(i + 1) << 2);
    info.pid = e.pid;
    info.object = e.object.get();
    info.access = e.access;
    out->push_back(info);
  }
}

uint32_t HandleTable::count_for(uint64_t pid) const {
  uint32_t n = 0;
  for (const Entry& e : entries_.view()) if (e.object && e.pid == pid) ++n;
  return n;
}
uint32_t HandleTable::kind_count(uint64_t pid, Kind kind) const {
  uint32_t n = 0;
  for (const Entry& e : entries_.view()) if (e.object && e.pid == pid && e.object->kind == kind) ++n;
  return n;
}
void HandleTable::handles_of(uint64_t pid, Kind kind, List<uint64_t>* out) const {
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    const Entry& e = entries_[i];
    if (e.object && e.pid == pid && e.object->kind == kind) out->push_back(uint64_t(kHandleTag) | (uint64_t(e.generation & 0xFF) << 20) | (uint64_t(i + 1) << 2));
  }
}

// --- Calls ---------------------------------------------------------------------------------------------------------------------------------------------------------------

void Ctx::finish() {
  State& s = st;
  if (s.flushing || s.deferred.count() == 0) return;
  s.flushing = true;
  while (s.deferred.count() != 0) {
    // Take the whole queue, run it with the lock given up (in order: a signal queued before a release is done before it).
    List<OrphanEvent> batch(s.allocator);
    for (const OrphanEvent& e : s.deferred.view()) batch.push_back(e);
    s.deferred.clear();
    sync_.unlock();
    for (const OrphanEvent& e : batch.view()) {
      if (e.signal) s.objects->set_event(e.id, nullptr);
      if (e.reset) s.objects->reset_event(e.id, nullptr);
      if (e.release) s.objects->release(e.id);
    }
    sync_.lock();
  }
  s.flushing = false;
}

uint64_t run_locked(Call& c, Body body) {
  ObjectManager* objects = system_objects();
  if (!objects) return kStatusNotImplemented;
  ISystemSync& sync = objects->sync();
  sync.lock();
  uint64_t result;
  if (State* s = state()) {
    Ctx x(c, *s, sync);
    result = body(c, x);
    x.finish();
  } else {
    result = kStatusNoMemory;
  }
  sync.unlock();
  return result;
}

// --- Helpers ---------------------------------------------------------------------------------------------------------------------------------------------------------------

bool map_access(uint32_t requested, uint32_t* granted) {
  const uint32_t generics = kGenericRead | kGenericWrite | kGenericAll | kMaximumAllowed;
  if ((requested & ~(kRightAll | kDelete | kStandardRead | 0x40000u | 0x80000u | generics)) != 0) return false;
  uint32_t access = requested & ~generics;
  if (requested & kGenericRead) access |= kRightQuery | kStandardRead;
  if (requested & kGenericWrite) access |= kRightModify | kStandardRead;
  if (requested & (kGenericAll | kMaximumAllowed)) access |= kRightAll;
  if (requested == 0) access = kRightAll;
  *granted = access;
  return true;
}

bool write_handle(Call& c, uint64_t address, uint64_t handle) { return address == 0 || c.write_u64(address, handle); }
bool read_handle(Call& c, uint64_t address, uint64_t* handle) { return c.read_u64(address, handle); }

uint64_t new_handle(Ctx& x, SharedPointer<Object> object, uint32_t access, uint32_t* status) {
  const uint64_t h = x.st.handles.create(x.call.pid(), static_cast<SharedPointer<Object>&&>(object), access);
  *status = h ? kStatusSuccess : kStatusNoMemory;
  return h;
}

static bool slot_of(uint64_t handle, uint64_t* slot) {
  handle &= ~uint64_t{3};
  if (handle == 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

uint32_t take_event(Ctx& x, uint64_t handle, uint32_t* status) { return take_object(x, handle, DescriptorKind::Event, kRightModify, status); }

uint32_t take_object(Ctx& x, uint64_t handle, DescriptorKind kind, uint32_t right, uint32_t* status) {
  Process& p = x.call.process();
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) == DescriptorKind::Closed) { *status = kStatusInvalidHandle; return 0; }
  if (p.files().kind(slot) != kind) { *status = kStatusObjectTypeMismatch; return 0; }
  if (p.files().denied_access(slot) & right) { *status = kStatusAccessDenied; return 0; }
  const uint32_t id = uint32_t(p.files().token(slot));
  { Ctx::Unlocked u(x); x.st.objects->add_ref(id); }
  *status = kStatusSuccess;
  return id;
}

uint32_t make_event(Ctx& x, bool manual_reset, bool signaled, uint32_t extra_references) {
  Ctx::Unlocked u(x);
  const ObjectManager::Created c = x.st.objects->create(ObjectKind::Event, StringView(), manual_reset, signaled, 0, 0, 0);
  if (c.id == 0) return 0;
  for (uint32_t i = 0; i < extra_references; ++i) x.st.objects->add_ref(c.id);
  return c.id;
}

bool give_descriptor(Call& c, DescriptorKind kind, uint32_t object, uint64_t* handle) {
  const int64_t slot = c.process().files().open_token(kind, object);
  if (slot < 0) return false;
  *handle = (uint64_t(slot) + 1) * 4;
  return true;
}

void drop_descriptor(Ctx& x, uint64_t handle) {
  Ctx::Unlocked u(x);
  x.call.process().close_descriptor(handle / 4 - 1);
}

uint32_t process_of_handle(Ctx& x, uint64_t handle, uint64_t* pid) {
  Process& p = x.call.process();
  if (handle == ~uint64_t{0}) { *pid = p.id(); return kStatusSuccess; }
  uint64_t slot = 0;
  if (!slot_of(handle, &slot) || p.files().kind(slot) != DescriptorKind::Process) return kStatusInvalidHandle;
  const uint32_t id = uint32_t(p.files().token(slot));
  bool exited = false;
  uint32_t code = 0;
  bool found;
  { Ctx::Unlocked u(x); found = x.st.objects->query_process(id, &exited, &code, pid); }
  if (!found) return kStatusInvalidHandle;
  return exited ? kStatusProcessTerminating : kStatusSuccess;
}

InputSink default_input_sink() {
  InputSink sink;
  sink.key = user::inject_key;
  sink.mouse_move = user::inject_mouse_move;
  sink.mouse_button = user::inject_mouse_button;
  sink.mouse_wheel = user::inject_mouse_wheel;
  sink.character = user::inject_char;
  sink.post_message = user::post_message;
  sink.post_thread_message = user::post_thread_message;
  sink.cursor_position = user::cursor_position;
  sink.screen_size = (gdi::screen_width && gdi::screen_height) ? &screen_size_of : nullptr;
  return sink;
}
const InputSink& input_sink(Ctx& x) { return x.st.input; }
void set_input_sink(const InputSink& sink) {
  if (State* s = state()) { s->input = sink; s->input_set = true; }
}

uint64_t timeout_ns_of_ms(uint32_t ms) { return ms == 0xFFFFFFFFu ? ~uint64_t{0} : uint64_t{ms} * 1000000ull; }

// --- Process exit ---------------------------------------------------------------------------------------------------------------------------------------------------------

static void on_process_exit(uint64_t pid) {
  State* s = g_state.get();
  if (!s) return;
  s->handles.close_process(pid);
  if (s->dwm_pid == pid) s->dwm_pid = 0;
  if (s->mouse_intercept_pid == pid) s->mouse_intercept_pid = 0;
  for (uint64_t i = s->minuser_threads.count(); i-- > 0;) if (s->minuser_threads[i].pid == pid) s->minuser_threads.remove_at(i);
  for (uint64_t i = s->input_spaces.count(); i-- > 0;) if (s->input_spaces[i].owner == pid) s->input_spaces.remove_at(i);
  for (uint64_t i = s->materials.count(); i-- > 0;) if (s->materials[i].owner == pid) s->materials.remove_at(i);
  for (uint64_t i = s->navigation.count(); i-- > 0;) if (s->navigation[i].owner == pid) s->navigation.remove_at(i);
  for (uint64_t i = s->resize_syncs.count(); i-- > 0;) if (s->resize_syncs[i].owner == pid) s->resize_syncs.remove_at(i);
  for (uint64_t i = s->frames.count(); i-- > 0;) if (s->frames[i].owner == pid) s->frames.remove_at(i);
  for (uint32_t i = 0; i < 8; ++i) if (s->service_pid[i] == pid) { s->service_pid[i] = 0; s->service_tid[i] = 0; }
}

}  // namespace composition

void install_composition() {
  composition::install_dcomp();
  composition::install_surface();
  composition::install_flip();
  composition::install_rim();
  composition::install_mit();
  register_process_exit_hook(&composition::on_process_exit);
}

}  // namespace process::win32k
