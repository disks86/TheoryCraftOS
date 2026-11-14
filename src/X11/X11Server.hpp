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

// The kernel's X server: the X11 core protocol (and BIG-REQUESTS) for Linux and Windows programs, with the desktop as its window manager.
//
// One screen, the desktop's window manager area: its top-level windows (the children of the root) are the desktop's windows, shown through the same
// desktop::IWindowsSession the Windows programs' windows use (Win32Windows.hpp), so an X window and a Win32 window get the same frame, title bar, buttons,
// taskbar button and Alt+Tab place, and are moved, sized, minimized, maximized, activated and closed the same way. The server is an ICCCM / EWMH window
// manager on the inside: a top-level's requested position is its frame's (NorthWest gravity), its title comes from _NET_WM_NAME or WM_NAME, WM_DELETE_WINDOW
// closes it politely, _MOTIF_WM_HINTS without decorations and override-redirect windows get no frame, _NET_WM_MOVERESIZE starts a drag of the frame, _GTK_SHOW_WINDOW_MENU opens its window menu, and
// _NET_WM_STATE / WM_CHANGE_STATE maximize and minimize it. Other window managers cannot run (selecting SubstructureRedirect on the root is BadAccess).
//
// Pictures. Every top-level window has one picture of its inside (32-bit 0xAARRGGBB pixels); its subwindows draw into the part of it that they cover,
// clipped by their ancestors, the siblings stacked above them and (ClipByChildren) their own children, as on a server without backing store: what a window
// loses when it is uncovered is painted with its background and the client gets an Expose. The desktop draws that picture into its frame (draw_window), so
// a top-level costs one copy, like a Win32 window. Visuals: TrueColor depth 24 (the root's) and depth 32 (ARGB); pixmaps of depth 1, 8, 24 and 32.
// Text: one core font (8x16 cells, whatever the name asked for), the "cursor" font for glyph cursors.
//
// Transport. The server knows nothing of sockets: a connection is a client (open_client), its bytes come in through receive() and the replies, events and
// errors it is owed come out through take_output(). X11SocketServer.hpp carries them over the system's AF_UNIX sockets, where Linux and Windows programs reach
// the server with DISPLAY=:0.
//
// Locking: one mutex guards everything (a sleeping kernel::Mutex rather than a spin lock, because a big PutImage or the desktop drawing a large picture
// can take a while). Every public call takes it; none calls out while holding it except draw_window, which draws into the caller's renderer (the desktop
// calls it under its UI lock: UI lock -> server lock is the order; the server never takes the UI lock). The wake-up callback runs after the lock is let go.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Desktop/WindowsSession.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "X11/X11Protocol.hpp"
#include "X11/X11Wire.hpp"
#include "Graphics/DeviceScale.hpp"

namespace x11 {

struct Rect {
  int32_t x = 0, y = 0, w = 0, h = 0;      // (w, h >= 0)
  bool empty() const { return w <= 0 || h <= 0; }
};

// A set of rectangles that do not overlap (a clip list).
class Region {
 public:
  explicit Region(allocators::IAllocator& allocator) : rects_(allocator) {}
  void set(const Rect& r) { rects_.clear(); if (!r.empty()) rects_.push_back(r); }
  void assign(ArrayView<const Rect> disjoint) { rects_.clear(); for (const Rect& r : disjoint) if (!r.empty()) rects_.push_back(r); }
  void intersect(const Rect& r);
  void subtract(const Rect& r);
  bool empty() const { return rects_.count() == 0; }
  Rect bounds() const;
  ArrayView<const Rect> rects() const { return rects_.view(); }

 private:
  List<Rect> rects_;
};

// The pixels of a pixmap (shared: a GC's tile, stipple or clip mask, or a window's background keeps them after FreePixmap).
struct Picture {
  explicit Picture(allocators::IAllocator& allocator) : pixels(allocator) {}
  uint32_t width = 0, height = 0;
  uint8_t depth = 24;
  List<uint32_t> pixels;      // one value per pixel (depth 1: 0/1; depth 24: 0xFF000000 | rgb)
};

struct Ink;

class Server final : public desktop::IWindowsSession {
 public:
  static constexpr uint32_t kMaxClients = 128;
  static constexpr uint32_t kIdBits = 21;                                      // resource-id-mask = 2^21 - 1; client n's base = n << 21
  static constexpr uint32_t kRootWindow = 0x2B, kDefaultColormap = 0x20, kVisual24 = 0x21, kVisual32 = 0x22, kWmCheckWindow = 0x2C;
  static constexpr uint64_t kMaxRequestBytes = 16u * 1024 * 1024;              // BIG-REQUESTS: the largest request (4 Mi words)
  static constexpr uint64_t kMaxOutputBytes = 32u * 1024 * 1024;               // a client that lets more pile up is dropped
  static constexpr uint64_t kMaxPicturePixels = 32u * 1024 * 1024;             // one picture or pixmap (128 MiB)
  static constexpr uint64_t kMaxPropertyBytes = 16u * 1024 * 1024;

  using Clock = uint64_t (*)();      // milliseconds since some start (X timestamps)
  using Wake = void (*)(void* context);

  explicit Server(allocators::IAllocator& allocator, Clock clock = nullptr);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Called (outside the lock) whenever output for a client was produced by something other than its own request (input, another client's request), so
  // the transport sends it.
  void set_wake(Wake wake, void* context);

  // --- the transport
  // A new connection: its client number (1..kMaxClients-1), 0 when the table is full. `pid`: the process at the other end (0 unknown).
  uint32_t open_client(uint64_t pid);
  // Bytes that came from the client: every complete request is carried out.
  void receive(uint32_t client, ConstByteView bytes);
  // Moves what the client is owed to the end of `out`. True when the client is to be disconnected once that is sent (a failed setup, KillClient, a
  // protocol it broke, output it let pile up).
  bool take_output(uint32_t client, ByteBuffer* out);
  bool has_output(uint32_t client);
  // The connection went away: its resources are freed (windows destroyed, grabs and selections let go).
  void close_client(uint32_t client);
  uint32_t client_count();

  // --- desktop::IWindowsSession (the desktop's view of the X windows; see WindowsSession.hpp)
  void attach(int32_t frame_border, int32_t frame_title) override;
  void set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) override;
  void take_changes(List<uint32_t>* changed, List<Repaint>* repainted) override;
  uint32_t take_drag(uint32_t* edge) override;
  uint32_t take_window_menu(int32_t* x, int32_t* y) override;
  void windows(List<uint32_t>* out) override;
  bool window_info(uint32_t window, desktop::WindowsWindowInfo* out, String* title) override;
  bool draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) override;
  bool draw_window_scaled(uint32_t window, graphics::IRenderer2D& device, int32_t x, int32_t y, uint32_t scale_percent, uint32_t* width, uint32_t* height) override;
  uint32_t scale() { kernel::ScopedMutex lock(mutex_); return scale_; }      // (tests: the device scale the server works at)
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
  void cursor(int32_t* x, int32_t* y) override;
  bool cursor_confined() override;
  uint32_t capture_window() override;
  // Drag and drop with the desktop (XDND, X11DragAndDrop.cpp)
  bool desktop_drag(uint32_t window, int32_t x, int32_t y, StringView text) override;
  bool desktop_drop() override;
  void desktop_drag_end() override;
  bool take_program_drop(String* text, int32_t* x, int32_t* y) override;
  void program_drop_done(bool taken) override;
  bool program_drag(String* text, int32_t* x, int32_t* y) override;
  void program_drag_answer(bool would_take) override;
  // A finger: taken (XInput 2.2 TouchBegin / TouchUpdate / TouchEnd, X11Input2.cpp) when a window under it selected touch events; else the desktop
  // moves the pointer with it.
  bool touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) override;

  // Tests: the pixel of a top-level's picture (0 when outside or none).
  uint32_t picture_pixel(uint32_t window, int32_t x, int32_t y);

 private:
  struct Client;
  struct Window;
  struct Gc;
  struct Pixmap;
  struct Cursor;
  struct Font;
  struct Canvas;
  enum class Kind : uint8_t { Window, Pixmap, Gc, Cursor, Font, Colormap };
  struct Resource {
    Kind kind = Kind::Window;
    uint8_t owner = 0;
    Window* window = nullptr;
    Pixmap* pixmap = nullptr;
    Gc* gc = nullptr;
    Cursor* cursor = nullptr;
    Font* font = nullptr;
  };
  struct Property {
    explicit Property(allocators::IAllocator& allocator) : data(allocator) {}
    uint32_t name = 0, type = 0;
    uint8_t format = 8;
    ByteBuffer data;      // 16- and 32-bit units little-endian
  };
  struct Selection {
    uint8_t client = 0;
    uint32_t mask = 0;
  };
  struct XiSelection {      // XISelectEvents: a client's XI2 event mask (the first 32 events) for one device on a window
    uint8_t client = 0;
    uint16_t device = 0;
    uint32_t mask = 0;
  };
  struct Window {
    explicit Window(allocators::IAllocator& allocator) : children(allocator), selections(allocator), properties(allocator), xi(allocator) {}
    uint32_t id = 0;
    uint8_t owner = 0;
    Window* parent = nullptr;
    List<Window*> children;                 // bottom of the stack first
    int32_t x = 0, y = 0;                   // the outer top left in the parent (a top-level: its inside's top left in the root, see the header)
    uint32_t width = 1, height = 1, border = 0;
    bool input_only = false, mapped = false, override_redirect = false;
    uint8_t depth = 24;
    uint32_t visual = kVisual24;
    uint8_t background = 1;                 // 0 none, 1 pixel, 2 pixmap, 3 parent relative
    uint32_t background_pixel = 0xFF000000u;
    SharedPointer<Picture> background_picture;
    uint32_t border_pixel = 0xFF000000u;
    uint8_t bit_gravity = 0, win_gravity = 1;
    uint32_t do_not_propagate = 0, colormap = kDefaultColormap, cursor = 0;
    List<Selection> selections;
    List<UniquePointer<Property>> properties;
    // a top-level: its picture, and what the window manager knows of it
    SharedPointer<Picture> picture;         // (null: none yet, or too big)
    bool placed = false, iconic = false, maximized = false, fullscreen = false, above = false;
    Rect restore;                           // the inside before it was maximized
    uint32_t icon = 0;                      // the icon handle the desktop is given (0: none)
    List<XiSelection> xi;                   // XInput 2 selections (X11Input2.cpp)
  };
  struct Gc {
    uint32_t id = 0;
    uint8_t owner = 0, depth = 24;
    uint8_t function = 3, line_style = 0, cap_style = 1, join_style = 0, fill_style = 0, fill_rule = 0, arc_mode = 1, subwindow_mode = 0;
    uint32_t plane_mask = 0xFFFFFFFFu, foreground = 0, background = 1, line_width = 0;
    SharedPointer<Picture> tile, stipple, clip_mask;
    int32_t ts_x = 0, ts_y = 0, clip_x = 0, clip_y = 0;
    uint32_t font = 0;
    bool graphics_exposures = true;
    UniquePointer<List<Rect>> clip_rects;   // SetClipRectangles (relative to clip_x/y); null: none set
  };
  struct Pixmap {
    uint32_t id = 0;
    uint8_t owner = 0;
    SharedPointer<Picture> picture;
  };
  struct Cursor {
    explicit Cursor(allocators::IAllocator& allocator) : argb(allocator) {}
    uint32_t id = 0;
    uint8_t owner = 0;
    uint32_t system_id = 32512;             // a glyph of the cursor font: the desktop's shape; else argb is the picture
    List<uint32_t> argb;
    uint32_t width = 0, height = 0;
    int32_t hot_x = 0, hot_y = 0;
  };
  struct Font {
    uint32_t id = 0;
    uint8_t owner = 0;
    bool cursor_font = false;
  };
  struct Grab {
    bool active = false, implicit = false, owner_events = false;
    uint8_t client = 0;
    uint32_t window = 0, mask = 0, cursor = 0;
  };
  struct Change {
    uint32_t window = 0;
    bool changed = false, repaint = false;
    Rect dirty;
  };

  // --- requests (X11Requests.cpp)
  void process(Client& c);
  bool setup(Client& c);
  void dispatch(Client& c, uint8_t opcode, uint8_t data, WireReader& r, uint64_t length);
  void error(Client& c, uint8_t code, uint32_t value, uint8_t major, uint16_t minor = 0);
  WireWriter reply(Client& c, uint8_t data, uint32_t extra_words);       // writes the 8-byte reply head; the caller writes 24 + 4 * extra_words bytes more
  void send(Client& c, const Packet& p, bool with_sequence = true);
  bool new_id(Client& c, uint32_t id, uint8_t major);
  Window* window_of(Client& c, uint32_t id, uint8_t major);
  bool drawable_of(Client& c, uint32_t id, uint8_t major, Window** window, Pixmap** pixmap);
  Gc* gc_of(Client& c, uint32_t id, uint8_t major);
  void create_window(Client& c, uint8_t depth, WireReader& r);
  void change_window_attributes(Client& c, Window& w, uint32_t mask, WireReader& r, uint8_t major);
  void get_window_attributes(Client& c, Window& w);
  void configure_window(Client& c, Window& w, WireReader& r);
  void reparent_window(Client& c, Window& w, Window& parent, int32_t x, int32_t y);
  void change_property(Client& c, uint8_t mode, WireReader& r);
  void get_property(Client& c, bool remove, WireReader& r);
  void change_gc(Client& c, Gc& gc, uint32_t mask, WireReader& r, uint8_t major);
  void send_event_request(Client& c, bool propagate, WireReader& r);
  void query_font(Client& c, bool with_infos, StringView name, bool last);
  void put_image(Client& c, uint8_t format, WireReader& r, uint64_t length);
  void get_image(Client& c, uint8_t format, WireReader& r);
  void create_cursor(Client& c, WireReader& r);
  void create_glyph_cursor(Client& c, WireReader& r);
  void kill_client(Client& c, uint32_t resource);
  void free_resource(uint32_t id);

  // --- windows and the window manager (X11Windows.cpp)
  Window* find_window(uint32_t id);
  Window* top_of(Window* w);
  bool viewable(const Window* w) const;
  bool is_ancestor(const Window* ancestor, const Window* w) const;
  void inside_origin(const Window* w, int32_t* x, int32_t* y) const;      // the inside's top left in root coordinates
  void map_window(Window& w);
  void unmap_window(Window& w, bool from_destroy);
  void destroy_window(Window& w);
  void destroy_tree(Window& w);
  void restack(Window& w, Window* sibling, uint32_t mode);
  void move_resize(Window& w, int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t border, bool from_desktop);
  void send_configure_notify(Window& w);
  void structure_event(Window& w, Packet& p, uint32_t event_offset);      // to the window's StructureNotify and its parent's SubstructureNotify
  void deliver(Window& w, uint32_t event_mask, const Packet& p, uint8_t only_client = 0);
  bool selected(const Window& w, uint32_t event_mask) const;
  uint32_t event_mask_of(const Window& w, uint8_t client) const;
  void property_changed(Window& w, uint32_t atom, bool deleted);
  Property* find_property(Window& w, uint32_t atom);
  void set_property(Window& w, uint32_t atom, uint32_t type, uint8_t format, ConstByteView data);      // (the server's own, little-endian units)
  void set_property32(Window& w, uint32_t atom, uint32_t type, ArrayView<const uint32_t> values);
  void delete_property(Window& w, uint32_t atom);
  void window_manager_message(uint32_t window, uint32_t type, const uint32_t (&data)[5]);
  Change* change_of(uint32_t window);
  void note_changed(Window& top);
  void note_repaint(Window& top, const Rect& dirty);
  void place(Window& top);
  void frame_margins(Window& top, int32_t* left, int32_t* top_margin, int32_t* right, int32_t* bottom);      // (device pixels)
  void frame_margins_units(Window& top, int32_t* left, int32_t* top_margin, int32_t* right, int32_t* bottom);      // (the desktop's units)
  // HiDPI: the server works in device pixels (the root, windows, pointer, pictures); the desktop speaks units. These convert at that boundary.
  int32_t up(int32_t v) const { return graphics::units_to_device(v, scale_); }
  int32_t down(int32_t v) const { return graphics::device_to_units(v, scale_); }
  int32_t down_up(int32_t v) const { return graphics::device_to_units_up(v, scale_); }
  Rect up(const Rect& r) const { return {up(r.x), up(r.y), up(r.x + r.w) - up(r.x), up(r.y + r.h) - up(r.y)}; }
  Rect cover(const Rect& r) const { return {down(r.x), down(r.y), down_up(r.x + r.w) - down(r.x), down_up(r.y + r.h) - down(r.y)}; }      // (device -> units, every pixel)
  void rescale_top(Window& top, uint32_t before, uint32_t after);      // a top-level keeps its place in units when the scale changes
  void publish_dpi();      // RESOURCE_MANAGER's Xft.dpi and Xcursor.size on the root, for the scale
  bool decorated(Window& top);
  bool property32(Window& w, uint32_t atom, uint32_t index, uint32_t* out);
  uint32_t window_type(Window& w);
  void strut(Window& w, uint8_t* edge, uint32_t* size);
  Rect work_area_for(Window& top);
  void set_maximized(Window& top, bool on);
  void set_fullscreen(Window& top, bool on);
  void set_iconic(Window& top, bool on);
  void close_request(Window& top);
  void free_client_resources(uint8_t client);
  void update_wm_state(Window& top);
  void update_root_lists();
  void set_focus(uint32_t focus, uint8_t revert_to);
  void ensure_picture(Window& top);
  void send_client_message(Window& w, uint32_t type, uint32_t d0, uint32_t d1);
  bool has_protocol(Window& w, uint32_t protocol);
  uint32_t intern(StringView name);       // (never fails for the server's own names: they fit)

  // --- XInput 2 (X11Input2.cpp)
  void xinput_request(Client& c, uint8_t minor, WireReader& r);
  void xi_client_gone(uint8_t client);

  // --- drag and drop (X11DragAndDrop.cpp)
  void xdnd_setup();                                                          // the atoms, the root's XdndAware
  bool xdnd_client_message(Window& target, const Packet& p);                  // an XDND message to the root or the source window: handled (true)
  bool xdnd_selection_notify(Window& target, const Packet& p);                // the answer to the server's own request for a client's drag data
  bool xdnd_convert(Client& c, Window& requestor, uint32_t selection, uint32_t target, uint32_t property, uint32_t time);      // ConvertSelection of the server's drag
  void xdnd_client_gone(uint8_t client);
  void send_client_message5(Window& w, uint32_t type, const uint32_t (&data)[5]);
  void out_drag_position();
  void out_drag_leave();
  void out_drag_reset();
  void in_drag_reset();
  void in_drag_status();
  void in_drag_finish(bool taken);
  void in_drag_read(ConstByteView data);
  StringView atom_name(uint32_t atom) const;
  uint32_t now();

  // --- drawing (X11Drawing.cpp)
  bool canvas_of(Window* w, Pixmap* p, Gc* gc, bool include_inferiors, Canvas* out);
  Ink ink_of(const Gc& gc) const;
  void paint_background(Window& w, const Rect& area);       // window coordinates
  void paint_border(Window& w);
  void send_expose(Window& w, const Rect& r, uint16_t count);
  void expose_tree(Window& w);                                // w and its viewable descendants: painted and exposed whole
  void expose_area(Window& w, const Rect& area);              // the part of w and its descendants under `area` (w's coordinates)
  void fill_span(Canvas& cv, const Gc* gc, const Ink& ink, int32_t y, int32_t x0, int32_t x1, ArrayView<const uint32_t> source = {});
  void fill_rect(Canvas& cv, const Gc* gc, const Ink& ink, const Rect& r);
  void draw_line(Canvas& cv, const Gc& gc, int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool last);
  void fill_polygon(Canvas& cv, const Gc& gc, ArrayView<const int32_t> xs, ArrayView<const int32_t> ys, bool winding);
  void draw_arc(Canvas& cv, const Gc& gc, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t a1, int32_t a2, bool fill);
  void draw_text(Canvas& cv, const Gc& gc, int32_t x, int32_t y, ConstByteView chars, bool sixteen, bool image);
  void finish(Canvas& cv);                                    // the drawn part of a top-level's picture goes to the desktop
  void copy_area(Client& c, WireReader& r, bool plane);
  void poly_request(Client& c, uint8_t opcode, uint8_t data, WireReader& r);

  // --- input (X11Input.cpp)
  Window* window_at(int32_t x, int32_t y);
  void pointer_moved();                                       // the pointer's window again (Enter / Leave when it changed)
  void motion();                                              // MotionNotify where the pointer is
  void press(uint8_t xbutton, bool down);
  void crossing(Window* from, Window* to, uint8_t mode);
  void crossing_event(Window& w, uint8_t type, uint8_t detail, uint8_t mode, Window* child);
  bool propagate(uint8_t type, uint8_t detail, uint32_t wanted, Window* source, Window* stop, uint8_t only_client);
  void device_event(uint8_t type, uint8_t detail, uint32_t wanted, Window* source, Window* stop, bool keyboard);
  void fill_device_event(Packet& p, uint8_t type, uint8_t detail, Window& event, Window* child);
  Window* child_toward(Window& w, Window* source);
  uint16_t key_state() const;
  void release_grabs_of(uint8_t client);
  void wake();

  allocators::IAllocator& allocator_;
  Clock clock_;
  kernel::Mutex mutex_;
  Wake wake_ = nullptr;
  void* wake_context_ = nullptr;
  bool wake_wanted_ = false;
  UniquePointer<Client> clients_[kMaxClients];
  Dictionary<uint32_t, Resource> resources_;
  Hive<Window> windows_;
  Hive<Gc> gcs_;
  Hive<Pixmap> pixmaps_;
  Hive<Cursor> cursors_;
  Hive<Font> fonts_;
  Window* root_ = nullptr;
  Window* wm_check_ = nullptr;
  List<String> atoms_;      // atom n is atoms_[n - 1]
  struct SelectionOwner {
    uint32_t selection = 0, window = 0, time = 0;
    uint8_t client = 0;
  };
  List<SelectionOwner> selection_owners_;
  // the screen and the desktop
  uint32_t screen_w_ = 1024, screen_h_ = 768;      // (device pixels)
  uint32_t scale_ = 100;                            // the desktop's device scale (percent): device pixels per unit
  struct WorkArea {
    Rect rect, work, panel;      // panel: the work area without what windows reserve (where panels are placed)
  };
  WorkArea monitors_[8];
  uint32_t monitor_count_ = 0;
  bool attached_ = false;
  int32_t frame_border_ = 0, frame_title_ = 0;
  List<Change> changes_;
  bool changes_overflow_ = false;
  uint32_t drag_window_ = 0, drag_edge_ = 0;
  uint32_t menu_window_ = 0;      // _GTK_SHOW_WINDOW_MENU of a top-level, until the desktop takes it
  int32_t menu_x_ = 0, menu_y_ = 0;
  uint32_t place_counter_ = 0;
  uint32_t next_icon_ = 0x80000001u;
  // focus and input
  uint32_t focus_ = 1;                      // 0 None, 1 PointerRoot, else a window
  uint8_t focus_revert_ = 1;
  int32_t pointer_x_ = 0, pointer_y_ = 0;
  uint32_t pointer_window_ = kRootWindow;   // the deepest window the pointer is in
  uint16_t buttons_ = 0;                    // state bits of the buttons held
  uint8_t keys_[32] = {};
  bool caps_lock_ = false, num_lock_ = false;
  uint8_t modifiers_ = 0;                   // hardware::modifier bits as the desktop last said
  Grab pointer_grab_, keyboard_grab_;
  uint32_t last_time_ = 0;
  uint64_t fake_clock_ = 0;
  // XInput 2 touches the desktop handed the server (its contact id, the XI touch id, the window and client that own it)
  struct XiTouchSlot {
    bool live = false;
    uint32_t desktop_id = 0, touch_id = 0, window = 0;
    uint8_t client = 0;
    uint16_t device = 0;
  };
  static constexpr uint32_t kMaxXiTouches = 16;
  XiTouchSlot xi_touches_[kMaxXiTouches];
  uint32_t next_xi_touch_ = 1;
  void xi_touch_event(uint16_t type, const XiTouchSlot& t, int32_t x, int32_t y);
  // drag and drop (XDND): its atoms
  struct XdndAtoms {
    uint32_t aware = 0, enter = 0, position = 0, status = 0, leave = 0, drop = 0, finished = 0, selection = 0, action_copy = 0, type_list = 0;
    uint32_t uri_list = 0, text_plain = 0, text_utf8 = 0, targets = 0, data = 0, incr = 0, utf8 = 0;
  } xa_;
  // the desktop's drag into a client (the server is the source; its window is wm_check_)
  struct OutDrag {
    bool active = false, accepted = false, waiting = false, pending = false, dropped = false;
    uint32_t target = 0, version = 0;
    int32_t x = 0, y = 0;
  } out_drag_;
  String out_text_, out_uri_, out_linux_;
  // a client's drag over the desktop (the root is its target)
  struct InDrag {
    bool active = false, uri = false, asked = false, read = false, dropped = false, ready = false, given = false;
    uint8_t client = 0;
    uint32_t source = 0, version = 0, type = 0;
    int32_t x = 0, y = 0;
    int8_t answer = -1;           // the desktop's last answer (-1 none)
  } in_drag_;
  String in_text_;
};

}  // namespace x11
