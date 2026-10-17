// Hot-plug with real threads, run under ThreadSanitizer: one thread plugs sticks in and pulls them out (some in the middle of a command),
// one polls SystemFileSystem the way the "hotplug" service does, and two read the mount table the way the file explorer and programs do -
// listing the volumes, opening folders and files and holding them across removals. Whatever a reader gets must be the data of the stick it
// asked for or an error, never another stick's data; nothing may race; and at the end every stick is accounted for and no memory is left.
#include <time.h>
#include "FakeUsbSticks.hpp"
#include "FileSystem/Mount/MountTable.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Kernel/Thread.hpp"
#include "SystemFileSystem.hpp"

using namespace hardware;
using namespace filesystem;

static int g_failed = 0;
static void fail(int line, const char* what) { printf("FAILED line %d: %s\n", line, what); __atomic_add_fetch(&g_failed, 1, __ATOMIC_RELAXED); }
#define CHECK(...) do { if (!(__VA_ARGS__)) fail(__LINE__, #__VA_ARGS__); } while (0)

static void nap(long us) {
  timespec ts{0, us * 1000};
  nanosleep(&ts, nullptr);
}

static constexpr int kSticks = 8;

struct World {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  FakeStickHost::Stick* sticks[kSticks] = {};
  int port_stick[FakeStickHost::kPorts] = {-1, -1, -1, -1};   // which stick is in which port (plugger only)
  SystemStorageDevices* storage = nullptr;
  SystemFileSystem* files = nullptr;
  bool stop = false;
  long plugs = 0, unplugs = 0, reads_ok = 0, reads_failed = 0, mounts_seen = 0, ejects_ok = 0, ejects_busy = 0, ejects_other = 0;
  std::mutex notes_mutex;
  long notes = 0;
};

static Bytes stick_image(int k) {
  char label[12];
  snprintf(label, sizeof label, "STICK%d     ", k);
  Fat12 fat(label);
  char text[16];
  const int n = snprintf(text, sizeof text, "stick %d", k);
  fat.file("HELLO   TXT", Bytes(text, text + n));
  fat.file("BIG     BIN", pattern(2500, static_cast<uint8_t>(k * 31)));
  return fat.img;
}

static bool stopping(World* w) { return __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE); }

// Plugs and unplugs. Sticks are never freed while the world runs (the fake host and the drivers may still look at them).
static void plugger(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 12345;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  int next = 0;
  for (int i = 0; i < 400; ++i) {
    const uint32_t port = rnd() % FakeStickHost::kPorts;
    if (w->port_stick[port] >= 0) {
      w->host.unplug(port);
      w->sticks[w->port_stick[port]]->unplug_after = -1;
      w->port_stick[port] = -1;
      ++w->unplugs;
    } else {
      int k;
      for (;;) {   // a stick that is not plugged in elsewhere
        k = next++ % kSticks;
        bool used = false;
        for (uint32_t p = 0; p < FakeStickHost::kPorts; ++p) used = used || w->port_stick[p] == k;
        if (!used) break;
      }
      FakeStickHost::Stick* s = w->sticks[k];
      s->commands = 0;
      s->unplug_after = rnd() % 5 == 0 ? static_cast<int>(rnd() % 40) : -1;   // now and then it goes by itself, mid-command
      w->host.plug(port, s);
      w->port_stick[port] = k;
      ++w->plugs;
    }
    nap(100 + rnd() % 1500);
  }
}

static void poller(void* arg) {
  World* w = static_cast<World*>(arg);
  while (!stopping(w)) {
    w->files->poll();
    nap(200);
  }
}

static int stick_of(const std::string& name) {
  if (name.size() < 6 || name.compare(0, 5, "STICK") != 0) return -1;
  return name[5] - '0';
}

static void reader(void* arg) {
  World* w = static_cast<World*>(arg);
  IFileSystem& fs = w->files->file_system();
  UniquePointer<IFile> held;       // kept across iterations, so volumes are unplugged under it
  UniquePointer<IDirectory> held_dir;
  int held_stick = -1;
  uint32_t seed = 777;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  while (!stopping(w)) {
    UniquePointer<IDirectory> root;
    if (fs.open_directory("/", &root) != Status::Ok) { fail(__LINE__, "open root"); return; }
    DirectoryEntry e;
    std::vector<std::string> names;
    while (root->next(&e) == Status::Ok) names.push_back(e.name.c_str());
    root.reset();
    for (const std::string& name : names) {
      const int k = stick_of(name);
      if (k < 0) continue;
      __atomic_add_fetch(&w->mounts_seen, 1, __ATOMIC_RELAXED);
      std::string path = "/" + name + "/HELLO.TXT";
      UniquePointer<IFile> f;
      const Status s = fs.open_file(StringView(path.data(), path.size()), OpenMode::Read, &f);
      if (s == Status::Ok) {
        uint8_t buf[64];
        uint64_t got = 0;
        const Status r = f->read(0, ByteView(buf), &got);
        if (r == Status::Ok) {
          char want[16];
          const int n = snprintf(want, sizeof want, "stick %d", k);
          if (got != static_cast<uint64_t>(n) || memcmp(buf, want, n) != 0) fail(__LINE__, "data from the wrong stick");
          __atomic_add_fetch(&w->reads_ok, 1, __ATOMIC_RELAXED);
        } else {
          if (r != Status::NoMedia && r != Status::IoError) fail(__LINE__, "unexpected read status");
          __atomic_add_fetch(&w->reads_failed, 1, __ATOMIC_RELAXED);
        }
      } else if (s != Status::NotFound && s != Status::NoMedia && s != Status::IoError && s != Status::Corrupt) {
        fail(__LINE__, "unexpected open status");
      }
      if (!held && rnd() % 3 == 0) {   // hold a big file and a folder open for a while
        std::string big = "/" + name + "/BIG.BIN";
        if (fs.open_file(StringView(big.data(), big.size()), OpenMode::Read, &held) == Status::Ok) {
          held_stick = k;
          std::string folder = "/" + name;
          fs.open_directory(StringView(folder.data(), folder.size()), &held_dir);
        }
      }
    }
    if (held) {
      uint8_t buf[2500];
      uint64_t got = 0;
      const Status r = held->read(0, ByteView(buf), &got);
      if (r == Status::Ok) {
        const Bytes expect = pattern(2500, static_cast<uint8_t>(held_stick * 31));
        if (got != 2500 || memcmp(buf, expect.data(), 2500) != 0) fail(__LINE__, "held file has other data");
      } else if (r != Status::NoMedia && r != Status::IoError) {
        fail(__LINE__, "unexpected held read status");
      }
      if (held_dir) {
        DirectoryEntry entry;
        const Status d = held_dir->next(&entry);
        if (d != Status::Ok && d != Status::EndOfDirectory && d != Status::NoMedia && d != Status::IoError) fail(__LINE__, "unexpected held dir status");
      }
      if (rnd() % 4 == 0) { held.reset(); held_dir.reset(); }
    }
    nap(50);
  }
}

// Ejects whatever it finds mounted, now and then (a drive in use is refused, and tried again later).
static void ejector(void* arg) {
  World* w = static_cast<World*>(arg);
  IFileSystem& fs = w->files->file_system();
  uint32_t seed = 4711;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  while (!stopping(w)) {
    UniquePointer<IDirectory> root;
    std::vector<std::string> names;
    if (fs.open_directory("/", &root) == Status::Ok) {
      DirectoryEntry e;
      while (root->next(&e) == Status::Ok) names.push_back(e.name.c_str());
    }
    root.reset();
    for (const std::string& name : names) {
      if (stick_of(name) < 0 || rnd() % 3 != 0) continue;
      switch (w->files->eject(StringView(name.data(), name.size()), rnd() % 4 == 0)) {
        case SystemFileSystem::EjectStatus::Ok: __atomic_add_fetch(&w->ejects_ok, 1, __ATOMIC_RELAXED); break;
        case SystemFileSystem::EjectStatus::InUse: __atomic_add_fetch(&w->ejects_busy, 1, __ATOMIC_RELAXED); break;
        default: __atomic_add_fetch(&w->ejects_other, 1, __ATOMIC_RELAXED); break;   // (it was pulled out meanwhile, or the stick failed)
      }
    }
    nap(3000);
  }
}

static void count_note(void* context, StringView) {
  World* w = static_cast<World*>(context);
  std::lock_guard<std::mutex> g(w->notes_mutex);
  ++w->notes;
}

static void test_stress() {
  World* w = new World;
  for (int k = 0; k < kSticks; ++k) w->sticks[k] = new FakeStickHost::Stick(make_stick(stick_image(k)));
  w->usb.hosts.push_back(&w->host);
  {
    SystemStorageDevices storage(w->pci, &w->usb, w->alloc, nullptr);
    w->storage = &storage;
    SystemFileSystem files(w->alloc, storage);
    w->files = &files;
    files.set_notifier(count_note, w);

    kernel::Thread* threads[4];
    threads[0] = kernel::spawn(poller, w, "poller");
    threads[1] = kernel::spawn(reader, w, "reader1");
    threads[2] = kernel::spawn(reader, w, "reader2");
    threads[3] = kernel::spawn(ejector, w, "ejector");
    kernel::Thread* plug = kernel::spawn(plugger, w, "plugger");
    kernel::join(plug);
    // everything is unplugged; let the poller see it all, then stop the others
    for (uint32_t p = 0; p < FakeStickHost::kPorts; ++p)
      if (w->port_stick[p] >= 0) { w->host.unplug(p); w->port_stick[p] = -1; ++w->unplugs; }
    for (int i = 0; i < 200; ++i) {
      bool empty;
      {
        std::lock_guard<std::recursive_mutex> g(w->host.m);
        empty = w->host.events.empty();
      }
      if (empty && storage.count() == 0) break;
      nap(2000);
    }
    __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; ++i) kernel::join(threads[i]);

    files.poll();   // (whatever the poller had not got to)
    CHECK(storage.count() == 0);
    DirectoryEntry e;
    UniquePointer<IDirectory> root;
    CHECK(files.file_system().open_directory("/", &root) == Status::Ok);
    int volumes = 0;
    while (root->next(&e) == Status::Ok) { ++volumes; CHECK(strcmp(e.name.c_str(), "Memory") == 0); }
    root.reset();
    CHECK(volumes == 1);
    CHECK(w->plugs > 100 && w->unplugs == w->plugs);
    CHECK(w->reads_ok > 10);
    CHECK(w->ejects_ok + w->ejects_busy > 5);
    CHECK(w->notes > 50);
    printf("  %ld plugs, %ld volumes seen, %ld reads ok, %ld failed because the stick left, %ld notices, ejects: %ld done, %ld refused as in use, %ld otherwise\n", w->plugs, w->mounts_seen, w->reads_ok, w->reads_failed, w->notes, w->ejects_ok, w->ejects_busy, w->ejects_other);
    w->files = nullptr;
    storage.shutdown();
  }
  CHECK(w->alloc.live == 0);
  for (int k = 0; k < kSticks; ++k) delete w->sticks[k];
  delete w;
}

// The service thread itself: it finds a stick that is plugged in and notices when it is pulled out.
static void test_service() {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  usb.hosts.push_back(&host);
  FakeStickHost::Stick a = make_stick(stick_image(3));
  {
    SystemStorageDevices storage(pci, &usb, alloc, nullptr);
    SystemFileSystem files(alloc, storage);
    CHECK(files.start_service() && files.start_service());
    host.plug(1, &a);
    bool seen = false;
    for (int i = 0; i < 60 && !seen; ++i) {
      nap(100000);
      UniquePointer<IFile> f;
      seen = files.file_system().open_file("/STICK3/HELLO.TXT", OpenMode::Read, &f) == Status::Ok;
    }
    CHECK(seen);
    host.unplug(1);
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
      nap(100000);
      UniquePointer<IFile> f;
      gone = files.file_system().open_file("/STICK3/HELLO.TXT", OpenMode::Read, &f) == Status::NotFound;
    }
    CHECK(gone);
    files.stop_service();
    files.stop_service();
    storage.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_stress();
  test_service();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("StorageHotplugThreadTest: all passed\n");
  return 0;
}
