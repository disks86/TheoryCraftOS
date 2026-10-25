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

// Threading tests for the GUI: run with real threads (tests/HostThreads.cpp) under ThreadSanitizer.
// They check that a slow button handler cannot stop painting or input, that handlers run in order on
// their own thread, that any thread may change controls while the window is painted and fed input (the
// stress test would trip ThreadSanitizer on any unlocked access), that run_async() works, and that
// destroying a control cancels or waits for its handlers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <atomic>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "Desktop/Desktop.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/RenderLoop.hpp"
#include "Gui/Window.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

using namespace gui;
namespace key = hardware::key;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::atomic<int> g_failed{0};
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

static uint64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t{static_cast<uint64_t>(ts.tv_sec)} * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}
static void sleep_ms(unsigned ms) { kernel::sleep_us(uint64_t{ms} * 1000); }

struct Screen {
  TestAllocator& a;
  graphics::Surface surface;
  graphics::SoftwareRenderer2D renderer;
  Screen(TestAllocator& alloc, uint32_t w, uint32_t h) : a(alloc), surface(make(alloc, w, h)), renderer(surface) {}
  ~Screen() { release_view(a, surface.pixels); }
  static graphics::Surface make(TestAllocator& a, uint32_t w, uint32_t h) {
    graphics::Surface s;
    s.pixels = allocate_view<uint32_t>(a, w * h);
    s.width = w; s.height = h; s.pitch = w;
    for (uint32_t& p : s.pixels) p = 0;
    return s;
  }
};

static void click(Window& w, int32_t x, int32_t y) {
  w.mouse_move(x, y);
  w.mouse_button(x, y, MouseButton::Left, true);
  w.mouse_button(x, y, MouseButton::Left, false);
}
static Point center(Control& c) {
  const Point o = c.window_origin();
  return {o.x + static_cast<int32_t>(c.bounds().width) / 2, o.y + static_cast<int32_t>(c.bounds().height) / 2};
}
static std::string read_text(Window& w, const Label& l) {
  std::string out;
  w.locked([&] { out.assign(l.text().data(), static_cast<size_t>(l.text().size())); });
  return out;
}

// --- 1. A slow handler stops neither painting nor input ---------------------------------

struct SlowState {
  Label* result;
  std::atomic<int> started{0}, finished{0};
  std::atomic<kernel::Thread*> ran_on{nullptr};
};

static void slow_handler(void* context, Control&) {
  auto* s = static_cast<SlowState*>(context);
  s->ran_on = kernel::current_thread();
  ++s->started;
  sleep_ms(400);
  s->result->set_text("handler done");  // a handler may change the UI from its own thread
  ++s->finished;
}

static void test_slow_handler() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    Button button(a);
    Label result(a), ticker(a);
    button.set_dock(DockStyle::Top); result.set_dock(DockStyle::Top); ticker.set_dock(DockStyle::Top);
    button.set_text("Slow");
    w.add(&button); w.add(&result); w.add(&ticker);
    SlowState state{&result};
    button.on_click({slow_handler, &state});
    w.layout();
    CHECK(w.start_handler_thread());
    CHECK(!w.start_handler_thread());  // only once
    RenderLoop loop(w);
    CHECK(loop.start());
    sleep_ms(50);
    const uint32_t frames_before = loop.frames();

    const uint64_t t0 = now_us();
    const Point p = center(button);
    click(w, p.x, p.y);
    const uint64_t click_us = now_us() - t0;
    CHECK(click_us < 100000);  // delivering the click did not wait for the 400 ms handler

    // While the handler sleeps the UI stays alive: input is processed, properties change, frames are drawn.
    while (state.started == 0) sleep_ms(1);
    CHECK(state.ran_on.load() != kernel::current_thread());
    for (int i = 0; i < 20; ++i) {
      char text[16];
      snprintf(text, sizeof(text), "tick %d", i);
      ticker.set_text(text);
      w.mouse_move(10 + i, 10 + i);
      sleep_ms(10);
    }
    CHECK(state.finished == 0);  // the handler is still busy ...
    CHECK(loop.frames() >= frames_before + 5);  // ... yet frames were drawn meanwhile
    CHECK(read_text(w, ticker) == "tick 19");
    CHECK(w.wait_for_handlers(3000000));
    CHECK(state.finished == 1 && read_text(w, result) == "handler done");
    loop.stop();
    CHECK(!loop.running());
    w.stop_handler_thread();
    CHECK(!w.has_handler_thread());
  }
  CHECK(a.live == 0);
}

// --- 1b. A slow handler in one application window does not hold up another -------------------

struct LaneState {
  std::atomic<bool> release{false}, slow_started{false}, slow_done{false}, fast_done{false};
  std::atomic<int> same_lane_order{0};
};
static void lane_slow(void* context, Control&) {
  auto* s = static_cast<LaneState*>(context);
  s->slow_started = true;
  for (int i = 0; i < 600 && !s->release; ++i) sleep_ms(5);
  s->slow_done = true;
}
static void lane_fast(void* context, Control&) { static_cast<LaneState*>(context)->fast_done = true; }
static void lane_second(void* context, Control&) {   // (same lane as the slow one: must come after it)
  auto* s = static_cast<LaneState*>(context);
  s->same_lane_order = s->slow_done ? 1 : -1;
}

static void test_lanes() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    Frame one(a), two(a);
    Button slow(a), second(a), fast(a);
    slow.set_dock(DockStyle::Top); second.set_dock(DockStyle::Top); fast.set_dock(DockStyle::Top);
    one.set_bounds({0, 0, 190, 200}); two.set_bounds({200, 0, 190, 200});
    one.add(&slow); one.add(&second); two.add(&fast);
    w.add(&one); w.add(&two);
    LaneState state;
    slow.on_click({lane_slow, &state});
    second.on_click({lane_second, &state});
    fast.on_click({lane_fast, &state});
    CHECK(w.start_handler_thread());
    slow.click();
    for (int i = 0; i < 400 && !state.slow_started; ++i) sleep_ms(5);
    CHECK(state.slow_started);
    second.click();  // queued behind the slow handler of the same window
    fast.click();    // another window: runs at once, on another thread
    for (int i = 0; i < 400 && !state.fast_done; ++i) sleep_ms(5);
    CHECK(state.fast_done);
    CHECK(!state.slow_done);
    CHECK(state.same_lane_order.load() == 0);
    state.release = true;
    CHECK(w.wait_for_handlers(3000000));
    CHECK(state.slow_done);
    CHECK(state.same_lane_order.load() == 1);
    w.stop_handler_thread();
    CHECK(!w.has_handler_thread());
  }
  CHECK(a.live == 0);
}

// --- 2. Handlers run in the order they were fired, one at a time ----------------------------

struct OrderState {
  std::atomic<int> concurrent{0}, max_concurrent{0};
  int seen[64];
  std::atomic<int> count{0};
};

struct Numbered {
  OrderState* state;
  int number;
};

static void order_handler(void* context, Control&) {
  auto* n = static_cast<Numbered*>(context);
  OrderState* s = n->state;
  const int now = ++s->concurrent;
  if (now > s->max_concurrent) s->max_concurrent = now;
  s->seen[s->count++] = n->number;
  if (n->number % 8 == 0) sleep_ms(1);
  --s->concurrent;
}

static void test_handler_order() {
  TestAllocator a;
  {
    Screen screen(a, 200, 100);
    Theme theme;
    Window w(a, screen.renderer, theme, {200, 100});
    OrderState state;
    Numbered numbers[50];
    Button* buttons = static_cast<Button*>(malloc(sizeof(Button) * 50));
    for (int i = 0; i < 50; ++i) {
      new (&buttons[i]) Button(a);
      numbers[i] = {&state, i};
      buttons[i].on_click({order_handler, &numbers[i]});
      w.add(&buttons[i]);
    }
    CHECK(w.start_handler_thread());
    for (int i = 0; i < 50; ++i) buttons[i].click();  // fired from the test thread, queued
    CHECK(w.wait_for_handlers(3000000));
    CHECK(state.count == 50 && state.max_concurrent == 1);
    bool ordered = true;
    for (int i = 0; i < 50; ++i) ordered = ordered && state.seen[i] == i;
    CHECK(ordered);
    // Without a handler thread a handler runs in the firing thread, before click() returns.
    w.stop_handler_thread();
    state.count = 0;
    buttons[7].click();
    CHECK(state.count == 1 && state.seen[0] == 7);
    for (int i = 0; i < 50; ++i) {
      w.remove(&buttons[i]);
      buttons[i].~Button();
    }
    free(buttons);
  }
  CHECK(a.live == 0);
}

// --- 3. Stress: many threads change the UI while it is drawn and fed input --------------------

static void test_stress() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    MenuBar bar(a);
    WindowManager mgr(a);
    Panel side(a);
    Label label(a);
    TextBox box(a);
    DropDown drop(a);
    Panel dynamic(a);
    bar.set_dock(DockStyle::Top);
    side.set_dock(DockStyle::Left); side.set_size(220, 0);
    mgr.set_dock(DockStyle::Fill);
    label.set_dock(DockStyle::Top); box.set_dock(DockStyle::Top); drop.set_dock(DockStyle::Top);
    dynamic.set_dock(DockStyle::Fill);
    Menu* menu = bar.add_menu("File");
    MenuItem* item_a = menu->add_item("Alpha");
    MenuItem* item_b = menu->add_item("Beta");
    side.add(&label); side.add(&box); side.add(&drop); side.add(&dynamic);
    w.add(&bar); w.add(&side); w.add(&mgr);
    w.layout();
    CHECK(w.start_handler_thread());
    RenderLoop loop(w);
    CHECK(loop.start());

    std::atomic<bool> stop{false};
    std::atomic<int> ops{0};
    auto worker = [&](int id) {
      unsigned n = static_cast<unsigned>(id) * 7919u;
      while (!stop) {
        ++n;
        char text[32];
        snprintf(text, sizeof(text), "w%d #%u", id, n);
        switch ((n + static_cast<unsigned>(id)) % 8) {
          case 0: label.set_text(text); break;
          case 1: box.set_text(text); break;
          case 2:
            drop.add_item(text);
            {
              int count = 0;
              w.locked([&] { count = drop.item_count(); });  // plain getters need the lock off the UI thread
              if (count > 6) drop.clear_items();
            }
            break;
          case 3: {  // controls come and go
            Button* b = new Button(a);
            b->set_text(text);
            b->set_dock(DockStyle::Top);
            dynamic.add(b);
            b->set_text("changed");
            dynamic.remove(b);
            delete b;
            break;
          }
          case 4:
            item_a->set_enabled(n % 2);
            item_b->set_checked(n % 3 == 0);
            break;
          case 5: {  // windows come and go
            Frame* f = new Frame(a);
            f->set_title(text);
            f->set_bounds({static_cast<int32_t>(n % 200), static_cast<int32_t>(n % 150), 240, 160});
            mgr.add_window(f);
            f->minimize();
            f->restore();
            f->maximize();
            f->maximize();
            mgr.activate(f);
            mgr.close_window(f);
            delete f;
            break;
          }
          case 6: w.set_title(text); break;
          default:
            side.set_visible(n % 5 != 0);
            side.set_enabled(true);
            break;
        }
        ++ops;
      }
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back(worker, i);

    // Meanwhile this thread is the input thread: moves, clicks (menu, drop-down, text box), keys.
    const uint64_t end = now_us() + 1500000;
    unsigned tick = 0;
    while (now_us() < end) {
      ++tick;
      w.mouse_move(static_cast<int32_t>(tick * 13 % 800), static_cast<int32_t>(tick * 7 % 600));
      if (tick % 9 == 0) click(w, 12, 10);                 // the File menu title
      if (tick % 11 == 0) click(w, 100, 60);
      if (tick % 13 == 0) { KeyArgs k; k.character = 'a'; k.usage = key::kA; w.key(k); }
      if (tick % 17 == 0) { KeyArgs k; k.usage = key::kEscape; w.key(k); }
      if (tick % 19 == 0) w.locked([&] { (void)label.text().size(); (void)drop.item_count(); });
      sleep_ms(1);
    }
    stop = true;
    for (std::thread& t : workers) t.join();
    CHECK(ops > 100);
    CHECK(loop.frames() > 10);
    loop.stop();
    CHECK(w.wait_for_handlers(2000000));
    w.stop_handler_thread();
    // Still consistent: render once more on this thread, no frames or windows left over.
    w.render();
    CHECK(mgr.window_count() == 0 && dynamic.child_count() == 0);
    printf("  stress: %d operations, %u frames\n", ops.load(), loop.frames());
  }
  CHECK(a.live == 0);
}

// --- 4. run_async -------------------------------------------------------------------------------

struct AsyncState {
  Label* progress;
  Label* done_label;
  std::atomic<kernel::Thread*> work_thread{nullptr}, done_thread{nullptr};
  std::atomic<int> works{0}, dones{0};
};

static void async_work(void* context) {
  auto* s = static_cast<AsyncState*>(context);
  s->work_thread = kernel::current_thread();
  for (int i = 0; i < 5; ++i) {
    char text[16];
    snprintf(text, sizeof(text), "step %d", i);
    s->progress->set_text(text);
    sleep_ms(20);
  }
  ++s->works;
}

static void async_done(void* context, Control&) {
  auto* s = static_cast<AsyncState*>(context);
  s->done_thread = kernel::current_thread();
  s->done_label->set_text("finished");
  ++s->dones;
}

static void test_run_async() {
  TestAllocator a;
  {
    Screen screen(a, 200, 100);
    Theme theme;
    Window w(a, screen.renderer, theme, {200, 100});
    Label progress(a), done(a);
    w.add(&progress); w.add(&done);
    CHECK(w.start_handler_thread());
    AsyncState state{&progress, &done};
    const uint64_t t0 = now_us();
    CHECK(w.run_async(async_work, &state, {async_done, &state}));
    CHECK(now_us() - t0 < 50000);  // returned at once, the work takes 100 ms
    CHECK(w.wait_for_background(3000000));
    CHECK(w.wait_for_handlers(3000000));
    CHECK(state.works == 1 && state.dones == 1);
    CHECK(read_text(w, progress) == "step 4" && read_text(w, done) == "finished");
    CHECK(state.work_thread.load() != kernel::current_thread());
    CHECK(state.done_thread.load() != state.work_thread.load());  // the completion runs on the handler thread
    w.stop_handler_thread();
  }
  CHECK(a.live == 0);
}

// --- 5. Destroying controls ------------------------------------------------------------------------

struct PurgeState {
  std::atomic<int> a_runs{0}, b_runs{0};
  std::atomic<int> c_started{0}, c_finished{0};
};

static void purge_a(void* context, Control&) { sleep_ms(250); ++static_cast<PurgeState*>(context)->a_runs; }
static void purge_b(void* context, Control&) { ++static_cast<PurgeState*>(context)->b_runs; }
static void purge_c(void* context, Control&) {
  auto* s = static_cast<PurgeState*>(context);
  ++s->c_started;
  sleep_ms(200);
  ++s->c_finished;
}

static void test_destroying_controls() {
  TestAllocator a;
  {
    Screen screen(a, 300, 200);
    Theme theme;
    Window w(a, screen.renderer, theme, {300, 200});
    Button first(a);
    first.set_dock(DockStyle::Top);
    w.add(&first);
    w.layout();
    CHECK(w.start_handler_thread());
    PurgeState state;
    first.on_click({purge_a, &state});

    // A handler queued for a control that is destroyed before it runs never runs.
    first.click();                       // occupies the handler thread for 250 ms
    {
      Button doomed(a);
      doomed.on_click({purge_b, &state});
      w.add(&doomed);
      doomed.click();                    // queued behind the slow one
      w.remove(&doomed);                 // taken out of the window ... and destroyed here
    }
    CHECK(w.wait_for_handlers(3000000));
    CHECK(state.a_runs == 1 && state.b_runs == 0);

    // A handler that is running right now is waited for.
    {
      Button busy(a);
      busy.on_click({purge_c, &state});
      w.add(&busy);
      busy.click();
      while (state.c_started == 0) sleep_ms(1);
      const uint64_t t0 = now_us();
    } // ~Button waits for the handler to finish before the control disappears
    CHECK(state.c_finished == 1);
    w.stop_handler_thread();
  }
  CHECK(a.live == 0);
}

// --- 6. The desktop with its threads ---------------------------------------------------------------------

struct FakeKeyboard final : hardware::IKeyboard {
  hardware::KeyEvent events[32]; int head = 0, tail = 0;
  const char* name() const override { return "fake keyboard"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(hardware::KeyEvent* out) override { if (head == tail) return false; *out = events[head++]; return true; }
  uint8_t modifiers() const override { return 0; }
  void push(uint8_t usage) { events[tail++] = {usage, true}; events[tail++] = {usage, false}; }
};

struct FakeMouse final : hardware::IMouse {
  hardware::MouseEvent events[32]; int head = 0, tail = 0;
  const char* name() const override { return "fake tablet"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(hardware::MouseEvent* out) override { if (head == tail) return false; *out = events[head++]; return true; }
  bool is_absolute() const override { return true; }
  uint32_t max_x() const override { return 32767; }
  uint32_t max_y() const override { return 32767; }
  void at(uint32_t x, uint32_t y, uint8_t buttons) { events[tail++] = {0, 0, 0, buttons, true, x, y, 32767, 32767}; }
};

static bool wait_until(desktop::Desktop& d, uint64_t timeout_us, bool (*condition)(desktop::Desktop&)) {
  const uint64_t end = now_us() + timeout_us;
  uint64_t uptime = 0;
  while (now_us() < end) {
    d.pump(uptime += 20000);
    if (condition(d)) return true;
    sleep_ms(5);
  }
  return false;
}

static void test_desktop_threads() {
  TestAllocator a;
  {
    Screen screen(a, 1024, 768);
    FakeKeyboard kb;
    FakeMouse mouse;
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    CHECK(d.start());
    CHECK(d.window().has_handler_thread());
    // Start > Shut Down... with the pointer; the menu and the dialog appear while the loop pumps input.
    auto at = [&](int32_t x, int32_t y) { return std::pair<uint32_t, uint32_t>{static_cast<uint32_t>(x * 32768 / 1024), static_cast<uint32_t>(y * 32768 / 768)}; };
    auto press = [&](int32_t x, int32_t y) {
      const auto p = at(x, y);
      mouse.at(p.first, p.second, 0);
      mouse.at(p.first, p.second, hardware::mouse_button::kLeft);
      mouse.at(p.first, p.second, 0);
    };
    press(42, 752);  // Start
    CHECK(wait_until(d, 3000000, [](desktop::Desktop& x) { return x.start_button().open(); }));
    press(40, 724);  // Shut Down...
    CHECK(wait_until(d, 3000000, [](desktop::Desktop& x) { return x.shutdown_dialog().showing(); }));
    CHECK(!d.shutdown_requested());
    // The answer comes back from the handler thread.
    kb.push(key::kEnter);
    CHECK(wait_until(d, 3000000, [](desktop::Desktop& x) { return x.shutdown_requested(); }));
    CHECK(d.window().wait_for_handlers(2000000));
    CHECK(!d.shutdown_dialog().showing());
    // The render thread draws what the loop changed.
    for (int i = 0; i < 400 && d.frames_drawn() < 3; ++i) sleep_ms(5);
    CHECK(d.frames_drawn() >= 1);
    d.stop();
    CHECK(!d.window().has_handler_thread());
    // Drawn by the render thread: the desktop surface and the taskbar are on the screen.
    printf("  desktop pixel %08x, frames drawn %u, dirty %d\n", screen.surface.pixels[100 * 1024 + 500], d.frames_drawn(), d.window().dirty());
    CHECK(screen.surface.pixels[100 * 1024 + 500] == gui::Theme().desktop);
  }
  CHECK(a.live == 0);
}

int main(int argc, char** argv) {
  const bool only_desktop = argc > 1 && strcmp(argv[1], "desktop") == 0;
  if (!only_desktop) {
    test_slow_handler();
    test_lanes();
    test_handler_order();
    test_stress();
    test_run_async();
    test_destroying_controls();
  }
  test_desktop_threads();
  if (g_failed) { printf("%d checks failed\n", g_failed.load()); return 1; }
  printf("gui threads: all checks passed\n");
  return 0;
}
