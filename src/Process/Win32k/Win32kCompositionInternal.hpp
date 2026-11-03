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
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Win32k/Win32k.hpp"
#include "Process/Win32k/Win32kComposition.hpp"
#include "Utility/SharedPointer.hpp"

// Shared by the files of the composition module (Win32kComposition*.cpp): the object model, the handle table, the state, and the helpers every service family uses.
//
// Locking. All state here is guarded by the system lock. The handlers are registered with locked = false and wrapped by thunk<>(), which takes the lock itself, because a
// handler often needs ObjectManager calls (set an event, add a reference, make a section) and those take the lock themselves: the lock is not recursive. A body that needs
// one gives the lock up with Ctx::Unlocked and looks again at what it needs afterwards. The ObjectManager events the objects hold are never touched under the lock: an
// object that lets go of an event, or must signal one, queues the operation (State::defer_event), and the end of every call runs the queue with the lock given up.
namespace process::win32k::composition {

enum class Kind : uint8_t { Channel, Connection, SyncObject, SharedResource, Surface, InputSink, FlipEndpoint, Token, RimRegistration, RimObserver, WindowTarget };

// Rights of the handles of this module (the specific ones are the low bits as for events; the standard ones are those of every NT object).
constexpr uint32_t kRightQuery = 0x1, kRightModify = 0x2, kRightAll = 0x1F0003u, kStandardRead = 0x20000u, kDelete = 0x10000u;
constexpr uint32_t kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u, kGenericAll = 0x10000000u, kMaximumAllowed = 0x02000000u;
constexpr uint32_t kHandleTag = 0x40000000u;      // handles of this module have this bit (the NT handles of the process table never get that high)

// Every object a handle can name. No virtual functions: SharedPointer remembers the concrete type it was made with.
struct Object {
  Kind kind;
  explicit Object(Kind k) : kind(k) {}
};

// Memory that is mapped into address spaces: a channel's command buffer. Page aligned; freed with its last mapping.
class MappedMemory final : public SharedMemory {
 public:
  // Null if memory ran out. The creator owns one reference.
  static MappedMemory* create(allocators::IAllocator& allocator, uint64_t size);
  void retain() override { __atomic_fetch_add(&refs_, 1, __ATOMIC_RELAXED); }
  void release() override;
  uint8_t* data() const { return storage_; }
  uint64_t size() const { return size_; }

 private:
  MappedMemory(allocators::IAllocator& a, uint8_t* s, uint64_t n) : allocator_(a), storage_(s), size_(n) {}
  allocators::IAllocator& allocator_;
  uint8_t* storage_;
  uint64_t size_;
  uint32_t refs_ = 1;
};

// ---- DirectComposition ------------------------------------------------------------------------------------------------------------------------------------------------

struct Resource { uint32_t id = 0, type = 0, incoming = 0; bool released = false; };      // incoming: how many other resources refer to it
struct Property { uint32_t resource = 0, property = 0; uint64_t value = 0; };
struct Edge { uint32_t parent = 0, child = 0; };
struct BatchFrame { uint32_t batch = 0; uint64_t frame = 0; };

struct Channel : Object {
  explicit Channel(allocators::IAllocator& a) : Object(Kind::Channel), resources(a), properties(a), edges(a), deleted(a), frames(a) {}
  ~Channel();
  uint64_t owner = 0;
  bool dwm = false;
  uint64_t connection_id = 0;
  uint64_t serial = 0;      // identifies the channel for the objects that refer to it without owning it
  MappedMemory* memory = nullptr;      // one reference
  uint64_t mapped_base = 0;            // where the creator has it mapped
  uint32_t completion_event = 0;       // an ObjectManager event (one reference), 0 = none
  uint32_t next_batch = 1, committed_batch = 0;
  uint32_t pending_commands = 0;       // commands processed since the last commit
  uint64_t processed_bytes = 0;
  uint32_t notifications = 0;          // batches committed that WaitForChannel has not reported
  bool animations_suspended = false, mmcss = false;
  List<Resource> resources;            // sorted by id
  List<Property> properties;
  List<Edge> edges;
  List<uint32_t> deleted;              // ids the compositor deleted, for GetDeletedResources
  List<BatchFrame> frames;
};

struct Connection : Object {
  Connection() : Object(Kind::Connection) {}
  uint64_t owner = 0, id = 0;
  uint32_t batch = 0;      // the connection's batch counter: grows with every commit of a channel attached to it
};

struct SyncObject : Object {
  SyncObject() : Object(Kind::SyncObject) {}
  uint64_t committed = 0, consumed = 0;      // CommitSynchronizationObject count / how many of them SyncWait has taken
  uint32_t flags = 0;
};

struct SharedResource : Object {
  SharedResource() : Object(Kind::SharedResource) {}
  uint32_t flags = 0, resource_id = 0;
  uint64_t dwm_channel_serial = 0;      // the DWM channel it was referenced on (0: not yet)
};

// ---- Composition surfaces, input sinks, tokens, window targets ----------------------------------------------------------------------------------------------------------

struct Surface : Object {
  Surface() : Object(Kind::Surface) {}
  uint64_t owner = 0;
  uint64_t bound_channel = 0;      // serial of the channel it is bound to (0: unbound)
  uint32_t bound_resource = 0;
  uint32_t frame_num = 0, frame_den = 0;      // frame rate (0/0: not set)
  uint32_t hdr[6] = {};                        // HDR metadata (primaries, white point, luminance), as set
  bool hdr_set = false;
  uint32_t stats_flags = 0;
  uint64_t stats_present_count = 0, stats_dirty = 0;
  uint32_t buffer_usage = 0, direct_flip = 0, analog_exclusive = 0;
  uint32_t independent_flip[4] = {};
  uint32_t realization = 0;                   // rendering realization (0: none, 1: hardware, 2: software)
  uint64_t section_size = 0;                  // the section behind it (see token manager OpenSectionAndEvents); 0 until made
  uint32_t section_id = 0, event_ready = 0, event_present = 0;      // ObjectManager objects (one reference each, 0 = none)
  uint64_t present_notifications = 0;
  uint32_t dirty[4] = {};                     // the last dirty rectangle (left, top, right, bottom)
  bool has_dirty = false;
  ~Surface();
};

struct InputSinkObject : Object {
  InputSinkObject() : Object(Kind::InputSink) {}
  uint64_t owner = 0, luid = 0;
  uint32_t view_id = 0, queue = 0, drop_target = 0;
  bool implicit = false;
  uint32_t transform[6] = {};      // 2x3 matrix, float bits (identity until set)
  bool closed = false;
};

struct FlipContent { uint32_t id = 0, buffer = 0, flags = 0; };
struct FlipBuffer { uint32_t id = 0, size = 0, flags = 0, usage = 0; uint32_t section = 0; };      // section: an ObjectManager section the buffer lives in (one reference, 0 = none)
struct FlipPresent { uint64_t id = 0; uint32_t content = 0, buffer = 0, flags = 0; uint8_t state = 0; };      // state: 0 queued, 1 acquired, 2 processing
struct FlipMessage { uint32_t type = 0, size = 0; uint8_t data[56] = {}; };

constexpr uint32_t kFlipMaxMessage = 56, kFlipMaxBuffers = 64, kFlipMaxContents = 256, kFlipMaxPresents = 256;

struct FlipObject {
  explicit FlipObject(allocators::IAllocator& a) : buffers(a), contents(a), presents(a), messages(a), events(a), lost_for_producer(a), lost_for_consumer(a), usage_refs(a) {}
  ~FlipObject();
  uint64_t owner = 0;
  bool producer_open = true, consumer_open = false, consumer_ever = false;
  uint32_t max_backchannel = 16;
  uint32_t next_buffer = 1, next_content = 1;
  uint32_t stats_types = 0;
  uint64_t completed = 0;
  List<FlipBuffer> buffers;
  List<FlipContent> contents;
  List<FlipPresent> presents;      // queued in order
  List<FlipMessage> messages;      // consumer to producer
  List<uint32_t> events;           // buffer-available events handed out (ObjectManager events, one reference each)
  List<uint32_t> lost_for_producer;      // lost events handed to the producer (signalled when the consumer goes)
  List<uint32_t> lost_for_consumer;      // ... to the consumer (signalled when the producer goes)
  bool available = false;                // the buffer-available events are signalled
  List<uint32_t> usage_refs;       // pairs (buffer id, count) flattened
};

struct FlipEndpoint : Object {
  FlipEndpoint(SharedPointer<FlipObject> f, bool c) : Object(Kind::FlipEndpoint), flip(static_cast<SharedPointer<FlipObject>&&>(f)), consumer(c) {}
  ~FlipEndpoint();
  SharedPointer<FlipObject> flip;
  bool consumer;
  bool disconnected = false;
};

struct Token : Object {
  enum Type : uint8_t { Flip, FlipReturn, Composition };
  explicit Token(Type t) : Object(Kind::Token), type(t) {}
  Type type;
  SharedPointer<FlipObject> flip;
  SharedPointer<Object> surface;      // (a Surface)
  uint64_t owner = 0;
};

struct WindowTarget : Object {
  WindowTarget() : Object(Kind::WindowTarget) {}
  uint32_t hwnd = 0, content_visual = 0, navigation_visual = 0;
  bool dcomp = false;
  uint64_t owner = 0;
};

// ---- Raw input manager ------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kRimTypeMouse = 0, kRimTypeKeyboard = 1;
struct RimPacket {
  uint32_t size = 40, type = 0, device = 0, flags = 0;
  uint64_t time_ns = 0;
  int32_t a = 0, b = 0, c = 0, d = 0;      // mouse: dx, dy, wheel, 0; keyboard: virtual key, scan code, 0, 0
};
static_assert(sizeof(RimPacket) == 40, "RimPacket is the layout RIMReadInput writes");

struct RimQueue {
  explicit RimQueue(allocators::IAllocator& a) : packets(a) {}
  List<RimPacket> packets;
  uint32_t overflow = 0;      // packets dropped because the queue was full
  void push(const RimPacket& p);
  bool pop(RimPacket* out);
};

struct RimRegistration : Object {
  RimRegistration(allocators::IAllocator& a) : Object(Kind::RimRegistration), queue(a) {}
  ~RimRegistration();
  uint64_t owner = 0;
  uint32_t device_mask = 0, flags = 0, hwnd = 0;
  RimQueue queue;
};
struct RimObserver : Object {
  RimObserver(allocators::IAllocator& a) : Object(Kind::RimObserver), queue(a) {}
  ~RimObserver();
  uint64_t owner = 0;
  uint32_t device_mask = 0, flags = 0;
  bool active = true;
  RimQueue queue;
};

// ---- minuser / input spaces -------------------------------------------------------------------------------------------------------------------------------------------

struct MinuserThread {
  uint64_t pid = 0, tid = 0;
  bool input_active = false;
  uint32_t wake_mask = 0, suspended = 0;
  int32_t offset_x = 0, offset_y = 0;
};
struct InputSpace { uint32_t id = 0, flags = 0; int32_t left = 0, top = 0, right = 0, bottom = 0; int32_t offset_x = 0, offset_y = 0; uint64_t owner = 0; };
struct MaterialValue { uint64_t owner = 0; uint32_t material = 0, property = 0; uint64_t value = 0; };
struct Frame { uint64_t id = 0; uint8_t state = 0; uint64_t owner = 0; };      // state: 0 begun, 1 confirmed, 2 retired, 3 discarded
struct Navigation { uint32_t hwnd = 0; bool enhanced = false; uint64_t owner = 0; };
struct ResizeSync { uint32_t hwnd = 0; bool enabled = false; SharedPointer<Object> sync; uint64_t owner = 0; };      // (sync: a SyncObject)
struct OrphanEvent { uint32_t id = 0; bool signal = false, release = false, reset = false; };

// ---- The handle table -------------------------------------------------------------------------------------------------------------------------------------------------

class HandleTable {
 public:
  static constexpr uint32_t kMaxPerProcess = 4096, kMaxTotal = 65536;
  explicit HandleTable(allocators::IAllocator& a) : entries_(a), free_(a) {}
  // A new handle owned by `pid` for `object` with `access`; 0 if the limits are hit or memory ran out.
  uint64_t create(uint64_t pid, SharedPointer<Object> object, uint32_t access);
  // The object `handle` names for `pid`. Status: kStatusInvalidHandle (no such handle / another process's / stale), kStatusObjectTypeMismatch, kStatusAccessDenied.
  Object* find(uint64_t handle, uint64_t pid, Kind kind, uint32_t required, uint32_t* status, uint32_t* access = nullptr);
  SharedPointer<Object> share(uint64_t handle, uint64_t pid, Kind kind, uint32_t required, uint32_t* status);
  bool close(uint64_t handle, uint64_t pid);
  // A new handle in `to_pid` for the object of `handle` (of `from_pid`), with `access` (a subset of the original's, unless `same_access`). Status as find().
  uint64_t duplicate(uint64_t handle, uint64_t from_pid, uint64_t to_pid, uint32_t access, bool same_access, uint32_t* status);
  uint32_t close_process(uint64_t pid);
  uint32_t count_for(uint64_t pid) const;
  uint32_t count() const { return live_; }
  void clear();      // every handle goes (the objects with them)
  uint32_t kind_count(uint64_t pid, Kind kind) const;
  // Handles of one kind owned by `pid`, appended to `out`.
  void handles_of(uint64_t pid, Kind kind, List<uint64_t>* out) const;
  struct Info { uint64_t handle = 0, pid = 0; Object* object = nullptr; uint32_t access = 0; };
  // Every handle of one kind of any process.
  void list(Kind kind, List<Info>* out) const;

 private:
  struct Entry { SharedPointer<Object> object; uint64_t pid = 0; uint32_t access = 0; uint32_t generation = 1; };
  Entry* entry_of(uint64_t handle);
  const Entry* entry_of(uint64_t handle) const;
  List<Entry> entries_;
  List<uint32_t> free_;
  uint32_t live_ = 0;
};

// ---- The state --------------------------------------------------------------------------------------------------------------------------------------------------------

struct State {
  explicit State(allocators::IAllocator& a);
  ~State();
  allocators::IAllocator& allocator;
  ObjectManager* objects;
  HandleTable handles;
  uint64_t next_serial = 1;
  uint64_t dwm_pid = 0;      // the process that owns the DWM channel (0: none)
  // events whose reference an object let go of, or that must be signalled: run with the lock given up at the end of a call
  List<OrphanEvent> deferred;
  bool flushing = false;
  // DirectComposition
  uint64_t next_frame = 1, next_connection = 1;
  List<Frame> frames;
  bool compositor_boosted = false;
  uint64_t debug_counters[32] = {};
  List<MaterialValue> materials;
  // RIM
  List<RimRegistration*> rim_registrations;
  List<RimObserver*> rim_observers;
  uint32_t rim_test_mode = 0;
  // minuser, input spaces, modern core
  List<MinuserThread> minuser_threads;
  List<InputSpace> input_spaces;
  uint32_t cursor_space = 0, pointer_space = 0;
  uint64_t mouse_intercept_pid = 0;
  int32_t cursor_x = 0, cursor_y = 0;
  bool cursor_known = false;
  List<Navigation> minuser_windows;      // windows the minuser layer knows (hwnd, owner)
  uint32_t next_visual = 0x100000;
  bool onecore_transform = false;
  uint32_t shell_cursor_state = 0;
  List<Navigation> navigation;
  List<ResizeSync> resize_syncs;
  List<uint8_t> navigation_sid;
  uint64_t service_pid[8] = {}, service_tid[8] = {};      // per service-thread kind: the thread that runs it (0: none)
  uint32_t telemetry_app_id = 0;
  uint64_t animation_scenarios = 0;
  List<uint64_t> touch_active;      // telemetry: touch interactions that began and did not end
  List<uint64_t> scenarios;         // telemetry: animation scenario references (id, count) pairs, flattened
  InputSink input;
  bool input_set = false;
  // Takes the reference of event `id` (and signals the event first if `signal`).
  void defer_event(uint32_t id, bool signal, bool release, bool reset = false);
};

// The state, made on first use from the system's allocator; null if the system has none.
State* state();

// ---- Calls --------------------------------------------------------------------------------------------------------------------------------------------------------------

class Ctx {
 public:
  Ctx(Call& c, State& s, ISystemSync& sync) : call(c), st(s), sync_(sync) {}
  Call& call;
  State& st;
  // Gives the system lock up for a stretch (ObjectManager calls); everything looked up before must be looked up again.
  class Unlocked {
   public:
    explicit Unlocked(Ctx& x) : sync_(x.sync_) { sync_.unlock(); }
    ~Unlocked() { sync_.lock(); }
    Unlocked(const Unlocked&) = delete;
    Unlocked& operator=(const Unlocked&) = delete;

   private:
    ISystemSync& sync_;
  };
  void signal(uint32_t event_id) { if (event_id) st.defer_event(event_id, true, false); }
  ISystemSync& sync() { return sync_; }
  void finish();      // (the lock held) runs the deferred event operations

 private:
  ISystemSync& sync_;
};

using Body = uint64_t (*)(Call&, Ctx&);
uint64_t run_locked(Call& c, Body body);
template <Body F>
uint64_t thunk(Call& c) { return run_locked(c, F); }
#define COMPOSITION_REGISTER(service, fn) register_handler(WindowsSyscallNumber::service, &thunk<fn>, false)

// ---- Helpers ----------------------------------------------------------------------------------------------------------------------------------------------------------

// The access mask a create call asks for, with the generic rights mapped; false if it has bits no object of this module has.
bool map_access(uint32_t requested, uint32_t* granted);

// Writes a HANDLE (64 bits) to the caller.
bool write_handle(Call& c, uint64_t address, uint64_t handle);
bool read_handle(Call& c, uint64_t address, uint64_t* handle);
// An object of the module for the caller: a typed lookup with the status.
template <class T>
T* lookup(Ctx& x, uint64_t handle, Kind kind, uint32_t right, uint32_t* status) {
  return static_cast<T*>(x.st.handles.find(handle, x.call.pid(), kind, right, status));
}
// Makes an object and a handle for the caller in one go. Returns the handle (0 on failure; status says why).
template <class T, class... Args>
SharedPointer<T> make_object(Ctx& x, Args&&... args) {
  return SharedPointer<T>::make(x.st.allocator, static_cast<Args&&>(args)...);
}
uint64_t new_handle(Ctx& x, SharedPointer<Object> object, uint32_t access, uint32_t* status);

// An event of the caller's: its ObjectManager id (checked to be an event handle with the modify right), with one reference taken for the module. 0 + status on failure.
uint32_t take_event(Ctx& x, uint64_t handle, uint32_t* status);
// Takes a reference of an object of the caller's table of descriptor kind `kind` (checking the handle's denied rights against `right`).
uint32_t take_object(Ctx& x, uint64_t handle, DescriptorKind kind, uint32_t right, uint32_t* status);
// The endpoint goes (or is disconnected): the peer's lost events are signalled.
void flip_disconnect(State& s, FlipEndpoint& e);
// Makes an ObjectManager event the module holds one reference of (and `extra` more).
uint32_t make_event(Ctx& x, bool manual_reset, bool signaled, uint32_t extra_references);
// Gives the caller a handle to an ObjectManager object whose reference the call hands over (kind: the descriptor kind).
bool give_descriptor(Call& c, DescriptorKind kind, uint32_t object, uint64_t* handle);
// The process a process handle (or the pseudo handle -1) names: its id; status kStatusInvalidHandle / process terminating.
uint32_t process_of_handle(Ctx& x, uint64_t handle, uint64_t* pid);
// Closes a descriptor made by give_descriptor (the object's reference goes with it); gives the lock up meanwhile.
void drop_descriptor(Ctx& x, uint64_t handle);
// Maps `memory` into the caller's address space; false if it has none or no room.
bool map_into_caller(Call& c, MappedMemory& memory, uint64_t* base);
void unmap_from_caller(Call& c, uint64_t base, uint64_t size);

// The window manager's functions as far as they are linked in.
InputSink default_input_sink();
void rim_dispatch_key(State& s, uint32_t virtual_key, uint32_t scan_code, bool down, bool extended, uint64_t time_ns);
void rim_dispatch_mouse(State& s, int32_t dx, int32_t dy, uint32_t button_flags, int32_t wheel, uint64_t time_ns);
const InputSink& input_sink(Ctx& x);

// Milliseconds of a timeout argument (0xFFFFFFFF: forever) as nanoseconds for wait_until.
uint64_t timeout_ns_of_ms(uint32_t ms);

void install_dcomp();
void install_surface();
void install_flip();
void install_rim();
void install_mit();

constexpr uint32_t kStatusProcessTerminating = 0xC000010Au, kStatusPrivilegeNotHeld = 0xC0000061u, kStatusDeviceBusy = 0x80000011u, kStatusInvalidBufferSize = 0xC0000206u;
constexpr uint32_t kStatusNotFoundSoft = 0xC0000225u, kStatusPortDisconnected = 0xC0000037u, kStatusAlreadyCommitted = 0xC0000021u, kStatusObjectNameExists = 0x40000000u;
constexpr uint32_t kStatusNoMoreEntriesSoft = 0x8000001Au, kStatusBufferOverflowSoft = 0x80000005u, kStatusInvalidDeviceRequest = 0xC0000010u, kStatusDeleted = 0xC0000120u;
constexpr uint32_t kStatusInvalidParameter1 = 0xC00000EFu, kStatusInvalidParameter2 = 0xC00000F0u, kStatusInvalidParameter3 = 0xC00000F1u;
constexpr uint32_t kStatusInvalidParameter4 = 0xC00000F2u, kStatusThreadIsTerminating = 0xC000004Bu, kStatusQuotaExceeded = 0xC0000044u;

}  // namespace process::win32k::composition
