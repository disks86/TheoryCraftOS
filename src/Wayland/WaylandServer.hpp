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

// The kernel's Wayland compositor: the Wayland protocol for Linux programs, with the desktop as the compositor's window manager.
//
// Its windows are the desktop's windows, shown through the same desktop::IWindowsSession the Windows programs' and the X server's windows use
// (Win32Windows.hpp), so an xdg_toplevel, a Win32 window and an X window get the same frame, title bar, buttons, taskbar button and Alt+Tab place, and are
// moved, sized, minimized, maximized, activated and closed the same way. Decorations are the desktop's (xdg-decoration: the compositor always answers
// server_side); a client that does not ask (GTK) draws its own and moves or sizes its window with xdg_toplevel.move / resize, which start the desktop's drag
// of the frame like a Win32 program's custom title bar does. Popups (menus, tooltips) are frameless desktop windows placed by their xdg_positioner.
//
// Globals: wl_compositor 6, wl_subcompositor 1, wl_shm 2 (ARGB8888, XRGB8888, ABGR8888, XBGR8888), wl_seat 8 (pointer and keyboard; the keymap is a US
// xkb_v1 keymap in a sealed file in memory), wl_output 4 (the primary monitor), wl_data_device_manager 3 (no transfers yet), xdg_wm_base 5,
// zxdg_decoration_manager_v1 1.
//
// Pictures. A wl_shm pool is a file in memory (memfd) the client passed with SCM_RIGHTS; the desktop draws a window's committed buffer straight from that
// memory (draw_window), so a frame costs one copy, like a Win32 or an X window. Subsurfaces are drawn over their parent in their stacking order. A surface's
// frame callbacks are answered when the desktop has drawn it, which paces a client to the desktop's redraws (and stops one whose window is hidden).
//
// Transport. The server knows nothing of sockets: a connection is a client (open_client), its bytes and descriptors come in through receive() and what it
// is owed comes out through take_output(). WaylandSocketServer.hpp carries them over the system's AF_UNIX sockets ($XDG_RUNTIME_DIR/wayland-0).
//
// Locking: one mutex guards everything. Every public call takes it; none calls out while holding it except draw_window, which draws into the caller's
// renderer (the desktop calls it under its UI lock: UI lock -> server lock is the order; the server never takes the UI lock). The wake-up callback runs
// after the lock is let go.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Desktop/WindowsSession.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace wayland {

// A descriptor that came with a client's message (a wl_shm pool's file) or goes out with an event (the keymap). The transport makes them.
class Memory {
 public:
  // At least the first `size` bytes of it, which the server may read (and the client write meanwhile); empty when it is smaller or no memory at all.
  // The view stays valid until the next call or until the object goes.
  virtual ByteView map(uint64_t size) = 0;

 protected:
  Memory() = default;
  ~Memory() = default;      // (an Fd is made with SharedPointer::make_as, which destroys the real type)
};
using Fd = SharedPointer<Memory>;
// Makes a descriptor for the server to send: a file in memory holding `contents`, sealed against changes. Null on failure.
using MakeFd = Fd (*)(void* context, ConstByteView contents);

struct Rect {
  int32_t x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

class Server final : public desktop::IWindowsSession {
 public:
  static constexpr uint32_t kMaxClients = 128;
  static constexpr uint32_t kMaxMessage = 4096;                     // as libwayland
  static constexpr uint64_t kMaxOutputBytes = 8u * 1024 * 1024;     // a client that lets more pile up is dropped
  static constexpr uint32_t kMaxObjects = 32768;                    // objects of one client
  static constexpr uint32_t kMaxFds = 256;                          // descriptors a client sent that no request took yet
  static constexpr uint32_t kMaxRegionRects = 256;
  static constexpr int32_t kMaxSize = 16384;                        // a buffer's or a window's width and height

  using Clock = uint64_t (*)();      // milliseconds since some start (event times)
  using Wake = void (*)(void* context);

  explicit Server(allocators::IAllocator& allocator, Clock clock = nullptr);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Called (outside the lock) whenever output for a client was produced by something other than its own request (input, the desktop, another client).
  void set_wake(Wake wake, void* context);
  void set_fd_maker(MakeFd make, void* context);

  // --- the transport
  // A new connection: its number (1..kMaxClients-1), 0 when the table is full. `pid`: the process at the other end (0 unknown).
  uint32_t open_client(uint64_t pid);
  // What came from the client: descriptors (moved out of `fds`; they arrive no later than the bytes of the request that uses them) and bytes. Every
  // complete request is carried out.
  void receive(uint32_t client, ConstByteView bytes, ArrayView<Fd> fds);
  // Moves what the client is owed to the end of `out`, and the descriptors that go with it to `fds` (send them with the first of those bytes). True when the
  // client is to be disconnected once that is sent (a protocol error, output it let pile up).
  bool take_output(uint32_t client, ByteBuffer* out, List<Fd>* fds);
  bool has_output(uint32_t client);
  void close_client(uint32_t client);
  uint32_t client_count();

  // --- desktop::IWindowsSession (see WindowsSession.hpp)
  void attach(int32_t frame_border, int32_t frame_title) override;
  void set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) override;
  void take_changes(List<uint32_t>* changed, List<Repaint>* repainted) override;
  uint32_t take_drag(uint32_t* edge) override;
  void windows(List<uint32_t>* out) override;
  bool window_info(uint32_t window, desktop::WindowsWindowInfo* out, String* title) override;
  bool draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) override;
  void move_window(uint32_t window, const desktop::WindowsRect& rect) override;
  void activate_window(uint32_t window) override;
  void window_command(uint32_t window, Command command) override;
  void pointer(Pointer* out) override;
  bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) override;
  void key(uint8_t hid_usage, bool pressed, uint8_t modifiers) override;
  void mouse_motion(int32_t dx, int32_t dy) override;
  void mouse_position(int32_t x, int32_t y) override;
  void mouse_button(uint32_t button, bool down) override;
  void mouse_wheel(int32_t notches) override;
  void mouse_left() override;
  void cursor(int32_t* x, int32_t* y) override;
  bool cursor_confined() override;
  uint32_t capture_window() override;

  // Tests: the pixel (0xAARRGGBB as the client stored it) of a window's committed buffer at (x, y) of the window's surface; 0 when outside or none.
  uint32_t window_pixel(uint32_t window, int32_t x, int32_t y);
  // The evdev key code of a HID usage (0: none), and the keymap the keyboard gets (xkb_v1 text, NUL-terminated).
  static uint32_t evdev_code(uint8_t hid_usage);
  static ConstByteView keymap_text();

 private:
  enum class Iface : uint8_t {
    Display, Registry, Callback, Compositor, Surface, Region, Subcompositor, Subsurface, Shm, ShmPool, Buffer, Seat, Pointer, Keyboard, Touch, Output,
    DataDeviceManager, DataSource, DataDevice, WmBase, Positioner, XdgSurface, Toplevel, Popup, DecorationManager, Decoration,
  };
  struct Surface;
  struct Client;
  // A pool's memory, shared by the pool and its buffers (a buffer outlives the pool it was made from).
  struct PoolMemory {
    Fd fd;
    ByteView bytes;
    uint64_t size = 0;
  };
  struct BufferInfo {
    SharedPointer<PoolMemory> memory;
    int32_t offset = 0, width = 0, height = 0, stride = 0;
    uint32_t format = 0;
    uint32_t id = 0;      // the wl_buffer (0: none, or it is gone)
    bool valid() const { return memory && width > 0; }
  };
  struct RegionRects {
    explicit RegionRects(allocators::IAllocator& allocator) : rects(allocator), add(allocator) {}
    List<Rect> rects;      // added (true) and subtracted (false), in order
    List<bool> add;
    bool contains(int32_t x, int32_t y) const;
  };
  struct Positioner {
    int32_t width = 0, height = 0;
    Rect anchor_rect;
    uint32_t anchor = 0, gravity = 0, adjustment = 0;
    int32_t offset_x = 0, offset_y = 0;
  };
  struct Resource {
    Iface iface = Iface::Display;
    uint32_t version = 1;
    Surface* surface = nullptr;                 // wl_surface and the roles' objects (xdg_surface, toplevel, popup, subsurface, decoration)
    SharedPointer<PoolMemory> pool;             // wl_shm_pool
    BufferInfo buffer;                          // wl_buffer
    SharedPointer<RegionRects> region;          // wl_region
    Positioner positioner;                      // xdg_positioner
  };
  enum class Role : uint8_t { None, Toplevel, Popup, Subsurface, Cursor };
  struct State {      // a surface's double-buffered state
    bool attached = false;      // attach() since the last commit (buffer is what it set; invalid: none)
    BufferInfo buffer;
    bool damaged = false;
    Rect damage;                // surface coordinates (both kinds of damage: the scale is 1)
    bool opaque_set = false, input_set = false;
    SharedPointer<RegionRects> opaque, input;   // (input null after set: everywhere)
    bool geometry_set = false;
    Rect geometry;
  };
  struct Surface {
    explicit Surface(allocators::IAllocator& allocator) : pending_callbacks(allocator), callbacks(allocator), stack(allocator), title(allocator) {}
    uint8_t client = 0;
    uint32_t id = 0;                            // the wl_surface
    Role role = Role::None;
    uint32_t xdg_id = 0, role_id = 0, decoration_id = 0;      // xdg_surface; toplevel / popup / subsurface; zxdg_toplevel_decoration_v1
    State pending, cached;                      // (cached: a synchronized subsurface's commit, applied with its parent's)
    bool has_cached = false;
    BufferInfo current;
    SharedPointer<RegionRects> input;           // null: everywhere
    List<uint32_t> pending_callbacks, callbacks;
    // subsurfaces: the children with the surface itself (nullptr) among them, bottom first
    Surface* parent = nullptr;
    List<Surface*> stack;
    int32_t sub_x = 0, sub_y = 0, pending_sub_x = 0, pending_sub_y = 0;
    bool sync = true;
    // an xdg surface
    uint32_t window = 0;                        // its desktop window (a toplevel or a popup; 0: none)
    bool initial_configure = false;             // the configure the first commit asks for was sent
    bool mapped = false;
    Rect geometry;
    bool geometry_set = false;
    uint32_t last_serial = 0, acked_serial = 0;
    int32_t want_w = 0, want_h = 0;             // the size the last configure asked for (0: the client's choice); shown until the client commits after acking
    uint32_t want_serial = 0;
    // a toplevel
    String title;
    uint32_t parent_window = 0;                 // set_parent (the parent's window)
    int32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    bool server_decorated = false, maximized = false, fullscreen = false, iconic = false, activated = false, placed = false;
    int32_t x = 0, y = 0;                       // the window geometry's top left on the screen (a toplevel: inside its frame)
    Rect restore;
    // a popup
    Surface* popup_parent = nullptr;            // the xdg surface it belongs to
    Positioner positioner;
    Rect placement;                             // relative to the parent's window geometry
    bool grab = false;
  };
  struct Change {
    uint32_t window = 0;
    bool changed = false, repaint = false;
    Rect dirty;
  };
  struct Global {
    uint32_t name;
    Iface iface;
    uint32_t version;
  };
  class Reader;
  class Event;

  // --- messages (WaylandServer.cpp)
  void process(Client& c);
  void dispatch(Client& c, uint32_t object, Resource& r, uint16_t opcode, Reader& in);
  void post_error(Client& c, uint32_t object, uint32_t code, StringView message);
  void send(Client& c, const Event& e);
  void delete_id(Client& c, uint32_t id);
  Resource* new_object(Client& c, uint32_t id, Iface iface, uint32_t version, uint32_t parent);
  Resource* object_of(Client& c, uint32_t id, Iface iface, uint32_t from);      // (a protocol error when it is not one: nullptr)
  void destroy_object(Client& c, uint32_t id);
  void bind(Client& c, uint32_t name, StringView interface, uint32_t version, uint32_t id);
  void announce(Client& c, uint32_t object, Iface iface, uint32_t version);
  Fd keymap_fd();
  uint32_t next_serial();
  uint32_t now();
  void wake();
  Client* client_of(uint8_t number);

  // --- surfaces, shm and the shell (WaylandSurfaces.cpp)
  void surface_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void region_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void subsurface_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void shm_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void wm_base_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void positioner_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void xdg_surface_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void toplevel_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void popup_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void decoration_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void commit(Client& c, Surface& s);
  void apply(Client& c, Surface& s, State& state);
  bool synchronized(const Surface& s) const;
  void destroy_surface(Client& c, Surface& s);
  void unmap(Surface& s);
  void map_window(Surface& s);
  void place(Surface& s);
  void place_popup(Surface& s);
  void configure_toplevel(Surface& s);
  void configure_popup(Surface& s);
  void send_configure_serial(Surface& s);
  void dismiss_popups(Surface* keep_root);
  void dismiss_children(Surface& s);
  void shown_size(const Surface& s, int32_t* w, int32_t* h) const;
  void actual_size(const Surface& s, int32_t* w, int32_t* h) const;
  static ArrayView<uint32_t> buffer_pixels(const BufferInfo& b);
  void copy_region(Client& c, uint32_t region, SharedPointer<RegionRects>* out, uint32_t from);
  void dismiss_popup(Surface& s);
  void release_buffer(Client& c, const BufferInfo& b);
  bool decorated(const Surface& s) const;
  void frame_margins(const Surface& s, int32_t* left, int32_t* top, int32_t* right, int32_t* bottom) const;
  Rect content_rect(const Surface& s) const;               // the window geometry on the screen (the desktop's "surface" rectangle)
  void surface_origin(const Surface& s, int32_t* x, int32_t* y) const;      // where the surface's (0, 0) is on the screen
  Rect work_area_for(const Surface& s) const;
  Rect monitor_for(const Surface& s) const;
  void set_maximized(Surface& s, bool on);
  void set_fullscreen(Surface& s, bool on);
  Surface* window_surface(uint32_t window);
  Surface* root_toplevel(Surface* s);
  void raise(Surface& s);
  Change* change_of(uint32_t window);
  void note_changed(Surface& s);
  void note_repaint(Surface& s, const Rect& dirty);
  void draw_tree(Surface& s, graphics::IRenderer2D& renderer, int32_t x, int32_t y, bool top, List<uint32_t>* done);

  // --- input (WaylandInput.cpp)
  Surface* surface_at(int32_t x, int32_t y, int32_t* local_x, int32_t* local_y);
  Surface* hit(Surface& s, int32_t x, int32_t y, int32_t* local_x, int32_t* local_y);
  void pointer_moved(bool motion);
  void set_pointer_focus(Surface* s, int32_t lx, int32_t ly);
  void pointer_frame(Client& c);
  void set_keyboard_focus(Surface* s);
  void send_modifiers(Client& c);
  void keyboard_enter(Client& c, Resource& keyboard, uint32_t keyboard_id, Surface& s);
  void seat_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void pointer_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  uint32_t modifier_mask(bool locked) const;
  void forget_surface(Surface& s);      // input and the desktop let go of a surface that is going

  allocators::IAllocator& allocator_;
  Clock clock_;
  kernel::Mutex mutex_;
  Wake wake_ = nullptr;
  void* wake_context_ = nullptr;
  bool wake_wanted_ = false;
  MakeFd make_fd_ = nullptr;
  void* make_fd_context_ = nullptr;
  Fd keymap_;
  UniquePointer<Client> clients_[kMaxClients];
  Hive<Surface> surfaces_;
  List<Surface*> stack_;                    // the mapped windows (toplevels and popups), bottom first
  List<Change> changes_;
  bool changes_overflow_ = false;
  uint32_t next_window_ = 1;
  uint32_t serial_ = 0;
  uint64_t fake_clock_ = 0;
  // the screen and the desktop
  uint32_t screen_w_ = 1024, screen_h_ = 768;
  struct WorkArea {
    Rect rect, work;
  };
  WorkArea monitors_[8];
  uint32_t monitor_count_ = 1;
  bool attached_ = false;
  int32_t frame_border_ = 0, frame_title_ = 0;
  uint32_t place_counter_ = 0;
  uint32_t drag_window_ = 0, drag_edge_ = 0;
  uint32_t active_window_ = 0;
  // input
  int32_t pointer_x_ = 0, pointer_y_ = 0;
  Surface* pointer_focus_ = nullptr;
  int32_t pointer_lx_ = 0, pointer_ly_ = 0;      // in the focus surface (pixels)
  uint32_t buttons_ = 0;                        // held: bit 0 left, 1 right, 2 middle
  Surface* keyboard_focus_ = nullptr;
  uint8_t keys_[32] = {};                       // HID usages held (that the clients were told of)
  uint8_t modifiers_ = 0;                       // hardware::modifier bits
  bool caps_lock_ = false, num_lock_ = false;
  uint32_t last_mods_[3] = {~0u, ~0u, ~0u};     // what the keyboard focus's client was last told (depressed, locked, group)
  // the cursor a client set (wl_pointer.set_cursor)
  bool cursor_set_ = false;                     // false: the system arrow
  Surface* cursor_surface_ = nullptr;           // (null with cursor_set_: hidden)
  int32_t cursor_hot_x_ = 0, cursor_hot_y_ = 0;
  uint32_t cursor_handle_ = 0x40000000u;        // changes with each new picture, so the desktop fetches it again
};

}  // namespace wayland
