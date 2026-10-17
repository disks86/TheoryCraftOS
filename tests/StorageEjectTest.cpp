// Host-side tests for safe eject of USB mass storage: the commands a stick gets (flush, allow removal, stop - and the tray for an optical drive),
// a stick that does not know SYNCHRONIZE CACHE or fails it, a drive that is in use, the volumes that go with it, and the stick staying gone
// until it is physically plugged in again.
#include <algorithm>
#include <string>
#include <vector>
#include "FakeUsbSticks.hpp"
#include "FileSystem/Mount/MountTable.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "SystemFileSystem.hpp"

using namespace hardware;
using namespace filesystem;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Fixture {
  TestAllocator alloc;
  FakePci pci;
  FakeStickHost host;
  FakeUsb usb;
  Fixture() { usb.hosts.push_back(&host); }
};

// The positions of the commands in what a stick has received, -1 if absent.
static int position(const FakeStickHost::Stick& s, uint8_t opcode) {
  for (size_t i = 0; i < s.opcodes.size(); ++i) if (s.opcodes[i] == opcode) return static_cast<int>(i);
  return -1;
}

static std::vector<std::string> names(IFileSystem& fs) {
  std::vector<std::string> out;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory("/", &dir) != Status::Ok) return out;
  DirectoryEntry e;
  while (dir->next(&e) == Status::Ok) out.push_back(e.name.c_str());
  return out;
}

static bool has(const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); }

static std::string text_of(IFileSystem& fs, const char* path) {
  UniquePointer<IFile> f;
  if (fs.open_file(path, OpenMode::Read, &f) != Status::Ok) return "";
  uint8_t buf[64];
  uint64_t got = 0;
  if (f->read(0, ByteView(buf), &got) != Status::Ok) return "";
  return std::string(reinterpret_cast<char*>(buf), got);
}

static void test_device_eject() {
  Fixture f;
  {
    SystemStorageDevices storage(f.pci, &f.usb, f.alloc, nullptr);
    FakeStickHost::Stick stick = make_stick(stick_with_files());
    f.host.plug(0, &stick);
    StorageEvent e;
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added);
    const uint32_t id = e.id;
    SharedPointer<IStorageDevice> device = e.device;
    CHECK(device->can_eject() && !device->ejected() && storage.can_eject(id) && !storage.can_eject(id + 7));
    stick.opcodes.clear();

    CHECK(storage.eject(id) == EjectResult::Ok);
    // the cache is flushed, removal is allowed and the unit stopped, in that order; a stick has no tray
    const int sync = position(stick, 0x35), allow = position(stick, 0x1E), stop = position(stick, 0x1B);
    CHECK(sync >= 0 && allow > sync && stop > allow);
    CHECK(stick.removal_allowed && stick.stopped && !stick.tray_opened);
    CHECK(!device->present() && device->ejected() && storage.count() == 0);
    uint8_t block[512];
    CHECK(!device->read(0, ByteView(block)) && !device->flush());
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed && e.id == id && e.ejected && e.device.get() == device.get());
    CHECK(!storage.poll(&e));
    CHECK(storage.eject(id) == EjectResult::NotFound && storage.eject(999) == EjectResult::NotFound);

    // it is still plugged in, but it does not come back by itself; plugged in again, it is a new device
    CHECK(!storage.poll(&e) && storage.count() == 0);
    f.host.unplug(0);
    CHECK(!storage.poll(&e));   // (the port's change is seen, the device is already gone)
    stick.stopped = false;
    f.host.plug(0, &stick);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added && e.id != id && e.device->present() && !e.device->ejected());
    e = StorageEvent();
    device.reset();
    storage.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

static void test_sync_cases() {
  Fixture f;
  {
    SystemStorageDevices storage(f.pci, &f.usb, f.alloc, nullptr);
    StorageEvent e;
    // a stick that does not know SYNCHRONIZE CACHE is let off
    FakeStickHost::Stick plain = make_stick(stick_with_files());
    plain.sync_unsupported = true;
    f.host.plug(0, &plain);
    CHECK(storage.poll(&e));
    CHECK(storage.eject(e.id) == EjectResult::Ok && plain.stopped && plain.removal_allowed);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed && e.ejected);

    // one that fails it is not ejected: it stays as it was, and a second try works once it recovers
    FakeStickHost::Stick sick = make_stick(stick_with_files());
    sick.sync_fails = true;
    f.host.plug(1, &sick);
    CHECK(storage.poll(&e));
    const uint32_t id = e.id;
    SharedPointer<IStorageDevice> device = e.device;
    CHECK(storage.eject(id) == EjectResult::Failed);
    CHECK(!sick.stopped && !sick.removal_allowed && device->present() && !device->ejected() && storage.count() == 1);
    uint8_t block[512];
    CHECK(device->read(0, ByteView(block)));   // (still works)
    CHECK(!storage.poll(&e));
    sick.sync_fails = false;
    CHECK(storage.eject(id) == EjectResult::Ok && sick.stopped);
    CHECK(storage.poll(&e) && e.ejected);

    // an optical drive: nothing to flush, the tray opens
    FakeStickHost::Stick drive = make_stick(stick_with_files());
    drive.optical = true;
    f.host.plug(2, &drive);
    CHECK(storage.poll(&e) && strcmp(e.device->name(), "USB optical drive") == 0);
    CHECK(storage.eject(e.id) == EjectResult::Ok);
    CHECK(position(drive, 0x35) < 0 && drive.tray_opened && drive.stopped);
    e = StorageEvent();
    device.reset();
    storage.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

struct Notes {
  std::vector<std::string> lines;
  static void add(void* self, StringView text) { static_cast<Notes*>(self)->lines.push_back(std::string(text.data(), text.size())); }
  bool has(const char* needle) const {
    for (const std::string& s : lines) if (s.find(needle) != std::string::npos) return true;
    return false;
  }
};

static void test_file_system_eject() {
  Fixture f;
  {
    SystemStorageDevices storage(f.pci, &f.usb, f.alloc, nullptr);
    SystemFileSystem files(f.alloc, storage);
    Notes notes;
    files.set_notifier(Notes::add, &notes);
    IFileSystem& fs = files.file_system();
    using Status = SystemFileSystem::EjectStatus;
    CHECK(files.eject("Nothing") == Status::NotFound);
    CHECK(files.eject("Memory") == Status::NotEjectable && has(names(fs), "Memory"));

    FakeStickHost::Stick a = make_stick(stick_with_files("ALPHA      ")), b = make_stick(stick_with_files("BRAVO      "));
    f.host.plug(0, &a);
    f.host.plug(1, &b);
    CHECK(files.poll() == 2 && has(names(fs), "ALPHA") && has(names(fs), "BRAVO"));

    // in use: refused, nothing touched
    UniquePointer<IFile> open;
    CHECK(fs.open_file("/ALPHA/HELLO.TXT", OpenMode::Read, &open) == filesystem::Status::Ok);
    a.opcodes.clear();
    CHECK(files.eject("alpha") == Status::InUse);   // (names match without regard to case)
    CHECK(has(names(fs), "ALPHA") && !a.stopped && position(a, 0x35) < 0);
    uint8_t buf[16];
    uint64_t got = 0;
    CHECK(open->read(0, ByteView(buf), &got) == filesystem::Status::Ok && got == 9);
    open.reset();

    // closed: it goes
    CHECK(files.eject("ALPHA") == Status::Ok);
    CHECK(!has(names(fs), "ALPHA") && has(names(fs), "BRAVO") && a.stopped && a.removal_allowed);
    CHECK(text_of(fs, "/BRAVO/HELLO.TXT") == "hello fat" && !b.stopped);   // the other one is not touched
    CHECK(files.poll() == 1 && notes.has("ejected, safe to remove"));
    CHECK(files.eject("ALPHA") == Status::NotFound);

    // forced with a handle open: the handle gives up
    CHECK(fs.open_file("/BRAVO/BIG.BIN", OpenMode::Read, &open) == filesystem::Status::Ok);
    CHECK(files.eject("BRAVO") == Status::InUse);
    CHECK(files.eject("BRAVO", true) == Status::Ok && b.stopped);
    CHECK(open->read(0, ByteView(buf), &got) == filesystem::Status::NoMedia);
    CHECK(names(fs).size() == 1 && files.poll() == 1);
    open.reset();

    // a stick that cannot flush stays mounted and keeps working
    FakeStickHost::Stick sick = make_stick(stick_with_files("SICK       "));
    sick.sync_fails = true;
    f.host.plug(2, &sick);
    CHECK(files.poll() == 1 && has(names(fs), "SICK"));
    CHECK(files.eject("SICK") == Status::Failed);
    CHECK(has(names(fs), "SICK") && text_of(fs, "/SICK/HELLO.TXT") == "hello fat" && !sick.stopped);
    sick.sync_fails = false;
    CHECK(files.eject("SICK") == Status::Ok && !has(names(fs), "SICK"));
    CHECK(files.poll() == 1);

    // plugged in again (physically): mounted again
    f.host.unplug(2);
    f.host.plug(2, &sick);
    CHECK(files.poll() == 1 && has(names(fs), "SICK") && text_of(fs, "/SICK/HELLO.TXT") == "hello fat");
    open.reset();
    files.stop_service();
    storage.shutdown();
  }
  CHECK(f.alloc.live == 0);
}

int main() {
  test_device_eject();
  test_sync_cases();
  test_file_system_eject();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("StorageEjectTest: all passed\n");
  return 0;
}
