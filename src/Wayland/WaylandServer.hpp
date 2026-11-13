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
// zxdg_decoration_manager_v1 1, and zwp_linux_dmabuf_v1 4 when the transport can take GPU images (set_dmabuf; the same four formats, linear; feedback
// names /dev/dxg as the device).
//
// GPU pictures. A zwp_linux_dmabuf_v1 buffer is a GPU image a client passed as a descriptor (on this system an export of the kernel's Direct3D layer, made with
// /dev/dxg: Process/DxgBufferFile.hpp). The desktop draws it where it is: on the GPU, with no copy at all, when the desktop's renderer draws on the device the
// image is on (IRenderer2D::draw_device_image), else from the CPU's view of it (one copy, as for wl_shm).
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
#include "Kernel/Clipboard.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "Wayland/WaylandYuv.hpp"

namespace wayland {

// A descriptor that came with a client's message (a wl_shm pool's file) or goes out with an event (the keymap). The transport makes them.
// A GPU image a client passed (a dmabuf): what the desktop draws a zwp_linux_dmabuf_v1 buffer from. Pixels it reads are 0xAARRGGBB whatever order the
// image keeps them in. The transport makes them (WaylandDxgImage.hpp over the kernel's Direct3D layer).
class GpuImage {
 public:
  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;
  virtual uint32_t pitch() const = 0;      // bytes per row of its memory
  // Draws its rectangle `source` at (x, y) with the renderer's clip and blend mode: in place when the renderer draws on the image's device, else from the CPU's
  // view of it. False when nothing could be drawn (the GPU still works on it, or it is lost).
  virtual bool draw(graphics::IRenderer2D& renderer, const graphics::Rect2D& source, int32_t x, int32_t y) = 0;
  // Its pixels in `source` (inside the image), row after row, into `out` (source.width * source.height words). False when it cannot be read now.
  virtual bool read(const graphics::Rect2D& source, ArrayView<uint32_t> out) = 0;
  // A buffer of it was committed: what the client wrote through a CPU mapping of the descriptor goes to the image (a dmabuf mapped with mmap,
  // Process/DxgBufferFile.hpp). False when that could not be done now.
  virtual bool committed() { return true; }

 protected:
  GpuImage() = default;
  ~GpuImage() = default;
};

// A timeline a client passed (a DRM sync object; on this system a fence exported with /dev/dxg): wp_linux_drm_syncobj_v1's acquire and release points.
class GpuTimeline {
 public:
  virtual uint64_t value() = 0;                // the point reached
  virtual void signal(uint64_t point) = 0;     // raises it to `point` (nothing when it is there already)

 protected:
  GpuTimeline() = default;
  ~GpuTimeline() = default;
};

class Memory {
 public:
  // At least the first `size` bytes of it, which the server may read (and the client write meanwhile); empty when it is smaller or no memory at all.
  // The view stays valid until the next call or until the object goes.
  virtual ByteView map(uint64_t size) = 0;
  // The GPU image it is (a dmabuf), or null: it is not one. It lives as long as this object.
  virtual GpuImage* gpu() { return nullptr; }
  // The timeline it is (a sync object), or null. It lives as long as this object.
  virtual GpuTimeline* timeline() { return nullptr; }

 protected:
  Memory() = default;
  ~Memory() = default;      // (an Fd is made with SharedPointer::make_as, which destroys the real type)
};
using Fd = SharedPointer<Memory>;
// Makes a descriptor for the server to send: a file in memory holding `contents`, sealed against changes. Null on failure.
using MakeFd = Fd (*)(void* context, ConstByteView contents);

// What the server asks of its transport (take_requests): a pipe to read a client's data from (MakePipe: the transport makes it, hands the server the write
// end with pipe_ready and the bytes, once the client closed its end, with pipe_read_done), or bytes to write into a descriptor a client passed and then let
// go of (Write: the desktop's clipboard pasted into a client). Carried out outside the server's lock.
struct TransportRequest {
  enum class Kind : uint8_t { MakePipe, Write };
  Kind kind = Kind::MakePipe;
  uint32_t job = 0;
  Fd fd;                             // Write: where to
  SharedPointer<ByteBuffer> data;    // Write: what
};

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
  // The desktop's clipboard, bridged to the Wayland selection (null: none). It must outlive the server.
  void set_clipboard(kernel::Clipboard* clipboard);
  // Pictures on the clipboard as PNG files (image/png), both ways: the clipboard's picture encoded (false: none, or it could not be), and a PNG file put
  // on the clipboard as its picture (false: not a picture). Given by main.cpp (Wayland/Media: the kernel's image codecs), so that the compositor does not
  // depend on them; without them only text is bridged. They are called outside the server's lock.
  using ClipboardToPng = bool (*)(allocators::IAllocator& allocator, kernel::Clipboard& clipboard, ByteBuffer* png);
  using PngToClipboard = bool (*)(allocators::IAllocator& allocator, kernel::Clipboard& clipboard, ConstByteView png);
  void set_clipboard_images(ClipboardToPng to_png, PngToClipboard from_png);
  // --- what the transport does for the server (see TransportRequest)
  void take_requests(List<TransportRequest>* out);
  void pipe_ready(uint32_t job, Fd write_end);
  void pipe_read_done(uint32_t job, ConstByteView data);
  // Now and then (the transport's rounds): commits waiting for an acquire point, the desktop's clipboard.
  void tick();
  // Whether zwp_linux_dmabuf_v1 is offered (the transport makes GPU images of the descriptors clients pass). Only clients that bind after it see the change.
  void set_dmabuf(bool offered);

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
  uint32_t take_window_menu(int32_t* x, int32_t* y) override;
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
  // Touches (wl_touch): a Down over a surface whose client has a wl_touch is taken (down, then the contact's motion and up go to that client, each with
  // a frame); else refused (the desktop moves the pointer with it). Cancel: the client is told its touches are cancelled.
  bool touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) override;
  // The pen (zwp_tablet_v2): over a surface whose client has a tablet seat it is taken (proximity_in, then motion, pressure, tilt, down / up, buttons,
  // each report a frame; the surface keeps it while the tip touches); elsewhere refused (the desktop moves the pointer). Out of range: proximity_out.
  bool pen(const desktop::PenInput& input) override;
  bool text_input(TextInputState* out) override;
  bool text_preedit(StringView text, uint32_t cursor) override;
  bool text_commit(StringView text) override;
  bool desktop_drag(uint32_t window, int32_t x, int32_t y, StringView text) override;
  bool desktop_drop() override;
  void desktop_drag_end() override;
  bool take_program_drop(String* text, int32_t* x, int32_t* y) override;
  void program_drop_done(bool taken) override;
  void cursor(int32_t* x, int32_t* y) override;
  bool cursor_confined() override;
  uint32_t capture_window() override;

  // Whether a client keeps the screen from going idle (zwp_idle_inhibit_manager_v1: a visible surface with an inhibitor).
  bool idle_inhibited();
  // Tests: the pixel (0xAARRGGBB as the client stored it; a GPU buffer's as GpuImage::read gives it) of a window's committed buffer at (x, y) of the window's
  // surface; 0 when outside or none.
  uint32_t window_pixel(uint32_t window, int32_t x, int32_t y);
  // The evdev key code of a HID usage (0: none), and the keymap the keyboard gets (xkb_v1 text, NUL-terminated).
  static uint32_t evdev_code(uint8_t hid_usage);
  static ConstByteView keymap_text();

 private:
  enum class Iface : uint8_t {
    Display, Registry, Callback, Compositor, Surface, Region, Subcompositor, Subsurface, Shm, ShmPool, Buffer, Seat, Pointer, Keyboard, Touch, Output,
    DataDeviceManager, DataSource, DataDevice, WmBase, Positioner, XdgSurface, Toplevel, Popup, DecorationManager, Decoration, LinuxDmabuf, BufferParams, DmabufFeedback, SyncobjManager, SyncobjTimeline, SyncobjSurface, DataOffer, Viewporter, Viewport, FractionalScaleManager, FractionalScale,
    Activation, ActivationToken, CursorShapeManager, CursorShapeDevice, PrimaryManager, PrimarySource, PrimaryDevice, PrimaryOffer, RelativePointerManager,
    RelativePointer, PointerConstraints, LockedPointer, ConfinedPointer, Presentation, PresentationFeedback, IdleInhibitManager, IdleInhibitor, Exporter, Exported,
    Importer, Imported, TabletManager, TabletSeat, Tablet, TabletTool, LayerShell, LayerSurface, TextInputManager,
    TextInput,
  };
  struct Surface;
  struct Client;
  // A pool's memory, shared by the pool and its buffers (a buffer outlives the pool it was made from).
  struct PoolMemory {
    Fd fd;
    ByteView bytes;
    uint64_t size = 0;
  };
  // A YUV buffer (WaylandYuv.hpp): where its planes are, and its picture converted at its last commit (rows of `width` words 0xFFRRGGBB), which is what is
  // drawn. Shared by every copy of the buffer's BufferInfo.
  struct YuvPicture {
    explicit YuvPicture(allocators::IAllocator& allocator) : rgb(allocator) {}
    YuvFormat format;
    SharedPointer<PoolMemory> pool;      // wl_shm: the pool (the planes at offsets in it)
    Fd planes[3];                        // dmabuf: each plane's descriptor (files in memory; several planes may share one)
    uint64_t offsets[3] = {0, 0, 0};
    uint32_t strides[3] = {0, 0, 0};
    bool dmabuf = false;
    List<uint32_t> rgb;
  };
  struct BufferInfo {
    SharedPointer<PoolMemory> memory;      // a wl_shm buffer's pool
    Fd gpu;                                // a dmabuf's plane: its descriptor's gpu() is the picture (the top left width x height of it)
    Fd release_timeline;                   // explicit sync: signalled at release_point when the compositor is done with this use of the buffer
    uint64_t release_point = 0;
    int32_t offset = 0, width = 0, height = 0, stride = 0;
    uint32_t format = 0;      // a wl_shm format code (a dmabuf's DRM code becomes the wl_shm one; a YUV format's code is the same in both)
    uint32_t id = 0;      // the wl_buffer (0: none, or it is gone)
    SharedPointer<YuvPicture> yuv;         // a YUV buffer (wl_shm or dmabuf): memory and gpu are null
    bool valid() const { return (memory || gpu || yuv) && width > 0; }
    uint32_t pitch() const { return yuv ? uint32_t(width) : uint32_t(stride / 4); }      // words between rows of buffer_pixels
    GpuImage* image() const { return gpu ? gpu->gpu() : nullptr; }
  };
  // A zwp_text_input_v3: its double-buffered state, what the client last committed, the surface it was entered on.
  struct TextInputInfo {
    explicit TextInputInfo(allocators::IAllocator& allocator) : pending(allocator), current(allocator) {}
    struct State {
      explicit State(allocators::IAllocator& allocator) : surrounding(allocator) {}
      bool enabled = false, enable_seen = false, surrounding_set = false, rect_set = false;
      String surrounding;
      int32_t cursor = 0, anchor = 0;
      uint32_t hint = 0, purpose = 0;
      Rect rect;      // the text cursor, surface coordinates
    };
    State pending, current;
    bool enabled = false;
    bool preedit_shown = false;
    uint32_t commits = 0;      // (the serial of done)
    uint32_t entered = 0;      // the surface it was entered on (0: none)
  };
  // A wl_data_source: what it offers.
  struct DataSourceInfo {
    explicit DataSourceInfo(allocators::IAllocator& allocator) : mimes(allocator) {}
    List<String> mimes;
    uint32_t actions = 0;      // (version 3: what a drag may do: 1 copy, 2 move, 4 ask)
    bool used = false;         // given to set_selection or start_drag (it cannot be given again)
  };
  // A wl_data_offer: whose data it is.
  struct OfferInfo {
    uint8_t source_client = 0;
    uint32_t source_id = 0;      // the wl_data_source (0: the desktop's clipboard)
    bool dnd = false;
    uint32_t actions = 0, preferred = 0;      // the destination's (set_actions)
    bool accepted = false;                    // a drag's destination accepted a type
  };
  // A zwp_linux_buffer_params_v1: the planes added so far (one for the RGB formats, up to three for YUV).
  struct DmabufParams {
    Fd planes[4];
    uint32_t offsets[4] = {0, 0, 0, 0}, strides[4] = {0, 0, 0, 0};
    uint64_t modifiers[4] = {0, 0, 0, 0};
    bool used = false;      // create or create_immed was called
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
    SharedPointer<DmabufParams> params;         // zwp_linux_buffer_params_v1
    Fd timeline;                                // wp_linux_drm_syncobj_timeline_v1
    SharedPointer<DataSourceInfo> source;       // wl_data_source, zwp_primary_selection_source_v1
    SharedPointer<OfferInfo> offer;             // wl_data_offer, zwp_primary_selection_offer_v1
    SharedPointer<TextInputInfo> text_input;    // zwp_text_input_v3
    SharedPointer<String> text;                 // xdg_activation_token_v1's token (once committed), zxdg_exported_v2's / zxdg_imported_v2's handle
    uint32_t tablet = 0, pen_tool = 0, eraser_tool = 0;      // zwp_tablet_seat_v2: the tablet and tools the server made for it
  };
  enum class Role : uint8_t { None, Toplevel, Popup, Subsurface, Cursor, Layer };
  // A layer surface's double-buffered state (zwlr_layer_surface_v1).
  struct LayerConfig {
    uint32_t width = 0, height = 0;          // (0: as wide / high as the anchors make it)
    uint32_t anchor = 0;                     // 1 top, 2 bottom, 4 left, 8 right
    int32_t zone = 0;                        // the exclusive zone (-1: ignores the others' zones)
    int32_t margin[4] = {0, 0, 0, 0};        // top, right, bottom, left
    uint32_t keyboard = 0;                   // 0 none, 1 exclusive, 2 on demand
    uint32_t layer = 0;                      // 0 background, 1 bottom, 2 top, 3 overlay
    bool operator==(const LayerConfig& o) const {
      return width == o.width && height == o.height && anchor == o.anchor && zone == o.zone && margin[0] == o.margin[0] && margin[1] == o.margin[1] &&
             margin[2] == o.margin[2] && margin[3] == o.margin[3] && keyboard == o.keyboard && layer == o.layer;
    }
  };
  struct SyncPoint {      // wp_linux_drm_syncobj_surface_v1: a timeline and a point on it
    Fd timeline;
    uint64_t point = 0;
    bool set() const { return timeline.get() != nullptr; }
  };
  struct State {      // a surface's double-buffered state
    bool attached = false;      // attach() since the last commit (buffer is what it set; invalid: none)
    BufferInfo buffer;
    bool damaged = false;
    Rect damage;                // surface coordinates (both kinds of damage: the scale is 1)
    bool opaque_set = false, input_set = false;
    SharedPointer<RegionRects> opaque, input;   // (input null after set: everywhere)
    bool geometry_set = false;
    Rect geometry;
    SyncPoint acquire, release;  // explicit sync (wp_linux_drm_syncobj_surface_v1)
    // how the buffer maps onto the surface (0 / -1: not set in this commit)
    int32_t scale = 0, transform = -1;
    bool source_set = false, destination_set = false;      // wp_viewport (set_source / set_destination, -1 = unset)
    int32_t source[4] = {-1, -1, -1, -1};                   // x, y, w, h in 24.8 fixed point
    int32_t destination[2] = {-1, -1};
  };
  // How a surface's buffer maps onto it: buffer_transform, buffer_scale, then the viewport's crop and size.
  struct BufferMapping {
    int32_t scale = 1, transform = 0;
    int32_t source[4] = {-1, -1, -1, -1};      // (24.8; -1: the whole of it)
    int32_t destination[2] = {-1, -1};
    bool identity() const { return scale == 1 && transform == 0 && source[0] == -1 && destination[0] == -1; }
  };
  struct Surface {
    explicit Surface(allocators::IAllocator& allocator)
        : pending_callbacks(allocator), callbacks(allocator), pending_feedback(allocator), feedback(allocator), stack(allocator), title(allocator) {}
    uint8_t client = 0;
    uint32_t id = 0;                            // the wl_surface
    Role role = Role::None;
    uint32_t xdg_id = 0, role_id = 0, decoration_id = 0;      // xdg_surface; toplevel / popup / subsurface; zxdg_toplevel_decoration_v1
    uint32_t syncobj_id = 0;                    // wp_linux_drm_syncobj_surface_v1 (0: implicit sync)
    uint32_t viewport_id = 0, fractional_id = 0;      // wp_viewport, wp_fractional_scale_v1
    BufferMapping mapping;
    bool waiting = false;                       // a commit waits for its acquire point (explicit sync): `waiting_state`
    State waiting_state;
    State pending, cached;                      // (cached: a synchronized subsurface's commit, applied with its parent's)
    bool has_cached = false;
    BufferInfo current;
    SharedPointer<RegionRects> input;           // null: everywhere
    List<uint32_t> pending_callbacks, callbacks;
    List<uint32_t> pending_feedback, feedback;  // wp_presentation_feedback: of the next commit, of the picture shown
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
    // a layer surface (WaylandLayerShell.cpp): its state, the size the last configure asked for, the edge it keeps (Frame::Edge numbers) and how much
    LayerConfig layer_pending, layer_current;
    int32_t layer_w = 0, layer_h = 0;
    uint8_t reserve_edge = 0;
    uint32_t reserve = 0;
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
  Resource* new_server_object(Client& c, Iface iface, uint32_t version, uint32_t* id);      // (an object the server makes: ids from 0xFF000000)
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
  // --- the selection, the desktop's clipboard, drag and drop (WaylandData.cpp)
  void data_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void set_selection(Client* c, uint32_t source);
  void send_selection(Client& c);
  uint32_t make_offer(Client& c, uint32_t device, uint8_t source_client, uint32_t source_id, bool dnd);
  DataSourceInfo* source_of(uint8_t client, uint32_t source);
  void source_gone(Client& c, uint32_t source);
  void poll_clipboard();
  void start_drag(Client& c, uint32_t source, Surface* origin);
  void drag_moved();
  void drag_focus(Surface* s, int32_t lx, int32_t ly);
  void drag_button_released();
  void end_drag(bool dropped);
  uint32_t drag_action(const OfferInfo& o) const;
  void request(TransportRequest r);
  static void devices_of(Client& c, List<uint32_t>* out);      // (the client's wl_data_devices)
  // --- wp_linux_drm_syncobj_v1 (WaylandDmabuf.cpp)
  void syncobj_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  bool check_sync_points(Client& c, Surface& s);       // (commit: the protocol's rules; false: an error was posted)
  void signal_release(BufferInfo& b);
  bool acquired(const State& state);
  void finish_waiting(Surface& s, bool force);           // applies a commit whose acquire point was reached (or drops it: force)
  void poll_waiting();
  // --- zwp_linux_dmabuf_v1 (WaylandDmabuf.cpp)
  void dmabuf_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void send_dmabuf_feedback(Client& c, uint32_t feedback);
  Fd format_table_fd();
  bool make_dmabuf_buffer(Client& c, uint32_t params_id, DmabufParams& p, int32_t width, int32_t height, uint32_t format, uint32_t flags, bool immediate, BufferInfo* out);
  bool make_yuv_dmabuf(Client& c, uint32_t params_id, DmabufParams& p, const YuvFormat& f, int32_t width, int32_t height, uint32_t flags, bool immediate,
                       BufferInfo* out);
  // Converts a YUV buffer's picture as it is now into its rgb words (at a commit of it). Black when its memory cannot be read.
  void convert_yuv(BufferInfo& b);
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
  // The surface's size from its buffer and mapping (0 x 0: no buffer), and where a surface point is in the buffer (false: outside it).
  static void surface_size(const BufferInfo& b, const BufferMapping& m, int32_t* w, int32_t* h);
  static void unviewported(const BufferInfo& b, const BufferMapping& m, float* w, float* h);
  static bool surface_to_buffer(const BufferInfo& b, const BufferMapping& m, float sx, float sy, float* bx, float* by);
  bool check_mapping(Client& c, Surface& s, const State& state);      // (commit: the scale and viewport rules; false: an error was posted)
  void draw_mapped(Surface& s, graphics::IRenderer2D& renderer, int32_t x, int32_t y);
  void viewport_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  // --- the other protocols (WaylandExtras.cpp): activation, cursor shapes, the primary selection, relative pointer and constraints, presentation time,
  // idle inhibitors, xdg-foreign; and the window manager's pings
  void extras_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void activate_locked(Surface* s);
  void send_primary_selection(Client& c);
  void update_constraints();
  void constraint_gone(Surface& s);
  struct Constraint;
  Constraint* active_constraint();
  void relative_motion(int32_t dx, int32_t dy);
  void ping(Client& c);
  void check_pings();
  void presented(Surface& s, bool shown);
  void forget_exports(Surface& s);
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
  bool dmabuf_ = false;
  Fd keymap_;
  Fd format_table_;      // zwp_linux_dmabuf_feedback_v1's format table (made once, sealed)
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
    Rect rect, work, panel;      // (panel: where layer surfaces anchor - the work area before the windows' reservations)
  };
  WorkArea monitors_[8];
  uint32_t monitor_count_ = 1;
  bool attached_ = false;
  int32_t frame_border_ = 0, frame_title_ = 0;
  uint32_t place_counter_ = 0;
  bool draw_missed_ = false;
  // the other protocols (WaylandExtras.cpp)
  uint32_t cursor_shape_ = 0;               // wp_cursor_shape_v1: the system cursor (OCR_*) the client under the pointer chose; 0: none
  uint8_t psel_client_ = 0;                 // the primary selection's source
  uint32_t psel_source_ = 0;
  uint8_t psel_sent_client_ = 0;
  struct Constraint {
    uint8_t client = 0;
    uint32_t id = 0;
    Surface* surface = nullptr;
    bool lock = false, persistent = false, active = false, defunct = false;
    SharedPointer<RegionRects> region;      // (null: the whole surface)
    bool hint_set = false;
    int32_t hint_x = 0, hint_y = 0;         // a lock's cursor position hint (surface pixels)
  };
  List<Constraint> constraints_;
  uint64_t present_seq_ = 0;
  List<String> tokens_;                     // xdg_activation_v1 tokens handed out and not used yet
  uint32_t next_token_ = 1;
  struct Exported {
    uint8_t client = 0;
    uint32_t id = 0;
    Surface* surface = nullptr;
    String handle;
    explicit Exported(allocators::IAllocator& allocator) : handle(allocator) {}
  };
  List<UniquePointer<Exported>> exports_;
  uint32_t next_export_ = 1;
  uint32_t idle_inhibitors_ = 0;
  // the selection: a client's wl_data_source, or the desktop's clipboard (sel_client_ 0, sel_desktop_)
  kernel::Clipboard* clipboard_ = nullptr;
  uint64_t clipboard_seen_ = 0;             // the clipboard's sequence the server last took in or set itself
  uint8_t sel_client_ = 0;
  uint32_t sel_source_ = 0;
  bool sel_desktop_ = false;
  uint8_t sel_sent_client_ = 0;             // the client the selection was last offered to (the keyboard focus's)
  uint32_t fetch_job_ = 0;                  // the clipboard's copy of a client's selection being read (0: none)
  String fetch_mime_;
  bool fetch_image_ = false;                // ... it is image/png (decoded onto the clipboard as a picture)
  enum class DesktopData : uint8_t { None, Text, Image };
  DesktopData desktop_kind_ = DesktopData::None;      // what the clipboard holds that the selection offers, as tick() last read it
  String desktop_text_;                     // the clipboard's text, as tick() last read it (what a paste of the desktop's selection gets)
  SharedPointer<ByteBuffer> desktop_png_;   // ... or its picture as a PNG file (shared with the transport's writes of it)
  ClipboardToPng to_png_ = nullptr;
  PngToClipboard from_png_ = nullptr;
  // What the clipboard holds now (outside the lock): its text, or its picture encoded.
  DesktopData read_clipboard(kernel::Clipboard& clipboard, String* text, SharedPointer<ByteBuffer>* png);
  void fetch_selection();
  List<TransportRequest> requests_;
  uint32_t next_job_ = 1;
  // a drag (wl_data_device.start_drag)
  struct Drag {
    bool active = false;
    uint8_t client = 0;
    uint32_t source = 0;                    // its wl_data_source (0: a drag inside one client, without data)
    Surface* origin = nullptr;
    Surface* icon = nullptr;
    Surface* focus = nullptr;               // the surface under it
    uint32_t offer = 0;                     // the offer the focus's client was given
    int32_t lx = 0, ly = 0;
    bool dropped = false;                   // the drop happened; the source waits for the destination's finish
    bool desktop = false;                   // the desktop's drag (desktop_drag): no client source, the data is desktop_drag_text_
  } drag_;
  // The desktop's drag (WaylandDragBridge.cpp): what it carries, and as a file URI list when it is a file a Linux program can see (else empty).
  String desktop_drag_text_, desktop_drag_uri_, desktop_drag_linux_;
  // A client's drag let go over the desktop: its data is read from the source (a pipe, `job`), then handed to the desktop (take_program_drop).
  struct ProgramDrop {
    explicit ProgramDrop(allocators::IAllocator& allocator) : mime(allocator), text(allocator) {}
    uint32_t job = 0;
    uint8_t client = 0;
    uint32_t source = 0;
    String mime;
    bool uri = false;              // the data is a text/uri-list
    int32_t x = 0, y = 0;          // where it was let go
    bool ready = false, taken = false;      // read (and given to the desktop)
    String text;                   // the items, one per line (files as system paths)
  } program_drop_;
  void drag_at(int32_t x, int32_t y);      // the drag (a client's or the desktop's) is at (x, y) of the screen
  bool drag_taken();                       // the surface under the drag accepted a type and an action
  bool start_program_drop();               // a client's drag let go outside every surface: to the desktop (false: nothing it carries is for it)
  void reset_program_drop();
  void program_drop_read(ConstByteView data);      // the source's data came
  uint32_t waiting_count_ = 0;              // surfaces with a commit waiting for its acquire point                // (draw_window: a GPU picture of the tree could not be drawn)
  uint32_t drag_window_ = 0, drag_edge_ = 0;
  struct TouchContact {
    uint32_t id = 0;              // the desktop's contact (the client sees it as its touch id)
    uint8_t client = 0;
    uint32_t surface = 0;         // the surface it went down on (its object id)
    int32_t origin_x = 0, origin_y = 0;      // where that surface's (0, 0) was on the screen at the time
  };
  List<TouchContact> touches_;    // the contacts clients took, until they lift
  struct PenFocus {
    uint8_t client = 0;           // 0: the pen is in nobody's proximity
    uint32_t surface = 0, tool = 0;
    bool eraser = false, tip = false;
    uint8_t buttons = 0;
    int32_t origin_x = 0, origin_y = 0;
  };
  PenFocus pen_;
  void pen_leave();               // proximity_out (and up first, if the tip touched) to the client that has the pen
  void tablet_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void layer_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);
  void text_input_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in);      // (WaylandTextInput.cpp)
  TextInputInfo* focused_text_input(Client** client, uint32_t* id);
  void text_input_focus(Surface* old, Surface* now);
  // A layer surface's commit (from apply): checks, the first configure, mapping. False: a protocol error was posted.
  bool commit_layer(Client& c, Surface& s, bool had_buffer);
  // Places every layer surface again (anchors, margins, the others' exclusive zones) and asks those whose size changed for the new one.
  void arrange_layers();
  void announce_tablet(Client& c, uint32_t seat_id, Resource& seat);
  uint32_t menu_window_ = 0;                // a toplevel's show_window_menu, until the desktop takes it
  int32_t menu_x_ = 0, menu_y_ = 0;
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
