// Host-side tests for USB mass-storage hot-plug: UsbMassStorageDevice and SystemStorageDevices::poll() against a fake host controller that
// has USB sticks plugged in and pulled out (a fake Bulk-Only target holding a FAT image), the MountTable, and SystemFileSystem putting the
// volumes of those sticks into the tree the file explorer browses - including handles that outlive the stick, a stick pulled out in the
// middle of a transfer and a port that gets a different stick before anyone noticed the first one leave.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "FakeUsbSticks.hpp"
#include "FileSystem/Mount/MountTable.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Hardware/StorageDevice/Usb/UsbMassStorageDevice.hpp"
#include "SystemFileSystem.hpp"

using namespace hardware;
using namespace filesystem;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

// ------------------------------------------------------------------------------------------------ helpers

static std::string read_text(IFileSystem& fs, const char* path, Status* status = nullptr) {
  UniquePointer<IFile> file;
  const Status s = fs.open_file(path, OpenMode::Read, &file);
  if (status) *status = s;
  if (s != Status::Ok) return "";
  uint8_t buffer[256];
  uint64_t got = 0;
  const Status r = file->read(0, ByteView(buffer), &got);
  if (status) *status = r;
  return std::string(reinterpret_cast<char*>(buffer), got);
}

static std::vector<std::string> list(IFileSystem& fs, const char* path) {
  std::vector<std::string> names;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path, &dir) != Status::Ok) return names;
  DirectoryEntry e;
  while (dir->next(&e) == Status::Ok) names.push_back(std::string(e.name.c_str()));
  return names;
}

static bool has(const std::vector<std::string>& v, const char* name) {
  for (const std::string& s : v) if (s == name) return true;
  return false;
}

// ------------------------------------------------------------------------------------------------ storage hot-plug

static void test_storage_hotplug() {
  TestAllocator alloc;
  {
    FakePci pci;
    FakeStickHost host;
    FakeUsb usb;
    usb.hosts.push_back(&host);
    SystemStorageDevices storage(pci, &usb, alloc, nullptr);
    StorageEvent e;
    CHECK(!storage.poll(&e) && storage.count() == 0);

    FakeStickHost::Stick a = make_stick(stick_with_files());
    host.plug(1, &a);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added && e.id == 1);
    CHECK(e.device && e.device->present() && storage.count() == 1 && storage.at(0) == e.device.get() && storage.id(0) == 1);
    const StorageInfo info = e.device->info();
    CHECK(info.block_count == 2880 && info.block_size == 512 && info.removable && info.kind == StorageKind::Disk);
    CHECK(strcmp(e.device->name(), "USB mass storage") == 0 && strcmp(info.model, "TESTSTK Hot-plug stick") == 0);
    uint8_t block[512];
    CHECK(e.device->read(0, ByteView(block)) && block[510] == 0x55 && block[511] == 0xAA);
    CHECK(!storage.poll(&e));
    SharedPointer<IStorageDevice> held = storage.share(0);

    // pulled out: a Removed event, the device is shut down and refuses everything, the held pointer stays valid
    host.unplug(1);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed && e.id == 1 && e.device.get() == held.get());
    CHECK(!held->present() && storage.count() == 0 && storage.at(0) == nullptr && !storage.share(0));
    CHECK(!held->read(0, ByteView(block)) && !held->flush());
    CHECK(!storage.poll(&e));
    e = StorageEvent();
    held.reset();

    // two at once, in order, then both gone
    FakeStickHost::Stick b = make_stick(stick_with_files()), c = make_stick(stick_with_files());
    host.plug(0, &b);
    host.plug(2, &c);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added && e.id == 2);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added && e.id == 3 && storage.count() == 2);
    host.unplug(0);
    host.unplug(2);
    uint32_t removed = 0, ids = 0;
    while (storage.poll(&e)) { CHECK(e.kind == StorageEvent::Kind::Removed); ++removed; ids += e.id; }
    CHECK(removed == 2 && ids == 5 && storage.count() == 0);

    // the port gets another stick before the first one's removal was looked at: the old device is gone, the new one is a new device
    FakeStickHost::Stick d = make_stick(stick_with_files("FIRST      ")), f = make_stick(stick_with_files("SECOND     "));
    host.plug(3, &d);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added);
    SharedPointer<IStorageDevice> first = e.device;
    uint32_t first_id = e.id;
    host.unplug(3);
    host.plug(3, &f);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed && e.id == first_id);
    CHECK(!first->present());
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added && e.id != first_id && e.device->present() && e.device.get() != first.get());
    CHECK(e.device->read(0, ByteView(block)) && memcmp(block + 43, "SECOND", 6) == 0);
    CHECK(!first->read(0, ByteView(block)));   // never the new stick's data
    first.reset();
    host.unplug(3);
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed);
    e = StorageEvent();

    // things that are not storage, or not working storage, do not appear
    FakeStickHost::Stick keyboard = make_stick(Bytes());
    keyboard.no_storage = true;
    host.plug(0, &keyboard);
    CHECK(!storage.poll(&e) && storage.count() == 0);
    host.unplug(0);
    storage.poll(&e);
    FakeStickHost::Stick broken = make_stick(stick_with_files());
    broken.broken_inquiry = true;
    host.plug(0, &broken);
    CHECK(!storage.poll(&e) && storage.count() == 0);
    host.unplug(0);
    storage.poll(&e);

    // a host without hot-plug reports nothing
    host.hotplug = false;
    FakeStickHost::Stick g = make_stick(stick_with_files());
    host.plug(0, &g);
    CHECK(!storage.poll(&e) && storage.count() == 0);
    host.hotplug = true;
    host.events.clear();
    host.sticks[0] = nullptr;

    storage.shutdown();
  }
  CHECK(alloc.live == 0);
}

static void test_yank_in_the_middle() {
  TestAllocator alloc;
  {
    FakePci pci;
    FakeStickHost host;
    FakeUsb usb;
    usb.hosts.push_back(&host);
    SystemStorageDevices storage(pci, &usb, alloc, nullptr);
    FakeStickHost::Stick a = make_stick(stick_with_files());
    a.unplug_after = 8;   // inquiry, ready, capacity... then a few reads: it goes during the big one
    host.plug(0, &a);
    StorageEvent e;
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added);
    Bytes big(512 * 100);
    int ok = 0, failed = 0;
    for (int i = 0; i < 20; ++i) (e.device->read(0, ByteView(big.data(), big.size())) ? ok : failed)++;
    CHECK(failed > 0 && ok < 20);   // the stick vanished part way
    CHECK(!e.device->present() || !e.device->read(0, ByteView(big.data(), 512)));
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Removed);
    CHECK(!e.device->present() && storage.count() == 0);
    storage.shutdown();
  }
  CHECK(alloc.live == 0);
}

// ------------------------------------------------------------------------------------------------ the mount table

struct CountedVolume final : IVolume {
  static int alive;
  explicit CountedVolume(allocators::IAllocator& a) : ram(a) { ++alive; }
  ~CountedVolume() { --alive; }
  IFileSystem& file_system() override { return ram; }
  bool present() const override { return !gone; }
  RamFileSystem ram;
  bool gone = false;
};
int CountedVolume::alive = 0;

static void write_file(IFileSystem& fs, const char* path, const char* text) {
  UniquePointer<IFile> file;
  uint64_t written = 0;
  if (fs.open_file(path, OpenMode::CreateTruncate, &file) == Status::Ok)
    file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text)), &written);
}

static SharedPointer<CountedVolume> volume_with(allocators::IAllocator& a, const char* file, const char* text) {
  SharedPointer<CountedVolume> v = SharedPointer<CountedVolume>::make(a, a);
  v->ram.create_directory("/docs");
  write_file(v->ram, file, text);
  return v;
}

static void test_mount_table() {
  TestAllocator alloc;
  {
    MountTable table(alloc);
    CHECK(table.count() == 0 && list(table, "/").empty());
    UniquePointer<IDirectory> nothing;
    CHECK_STATUS(table.open_directory("/nothing", &nothing), Status::NotFound);
    SharedPointer<CountedVolume> one = volume_with(alloc, "/docs/a.txt", "alpha");
    SharedPointer<CountedVolume> two = volume_with(alloc, "/docs/a.txt", "beta");
    FixedString<MountTable::kMaxName + 1> name;
    CHECK(table.mount("USB", SharedPointer<IVolume>(one), 10, &name) && name.view().equals("USB"));
    CHECK(table.mount("usb", SharedPointer<IVolume>(two), 11, &name) && name.view().equals("usb 2"));   // unique, ignoring case
    one.reset();
    two.reset();
    CHECK(CountedVolume::alive == 2 && table.count() == 2 && table.changes() == 2);
    std::vector<std::string> root = list(table, "/");
    CHECK(root.size() == 2 && has(root, "USB") && has(root, "usb 2"));
    CHECK(read_text(table, "/USB/docs/a.txt") == "alpha" && read_text(table, "/usb 2/docs/a.txt") == "beta");
    CHECK(read_text(table, "//USB/./docs//a.txt") == "alpha" && read_text(table, "/Usb/docs/a.txt") == "alpha");   // case-insensitive volume names
    CHECK(has(list(table, "/USB"), "docs") && has(list(table, "/USB/docs"), "a.txt"));

    DirectoryEntry entry;
    CHECK_STATUS(table.stat("/", &entry), Status::Ok);
    CHECK(entry.type == NodeType::Directory && entry.name.empty());
    CHECK_STATUS(table.stat("/USB", &entry), Status::Ok);
    CHECK(entry.type == NodeType::Directory && entry.name.view().equals("USB"));
    CHECK_STATUS(table.stat("/USB/docs/a.txt", &entry), Status::Ok);
    CHECK(entry.type == NodeType::File && entry.size == 5 && entry.name.view().equals("a.txt"));
    CHECK_STATUS(table.stat("/Nope", &entry), Status::NotFound);
    CHECK_STATUS(table.stat("/USB/../x", &entry), Status::InvalidPath);

    UniquePointer<IFile> file;
    UniquePointer<IDirectory> dir;
    CHECK_STATUS(table.open_file("/", OpenMode::Read, &file), Status::IsADirectory);
    CHECK_STATUS(table.open_file("/USB", OpenMode::Read, &file), Status::IsADirectory);
    CHECK_STATUS(table.open_file("/Nope/x", OpenMode::Read, &file), Status::NotFound);
    CHECK_STATUS(table.open_directory("/USB/docs/a.txt", &dir), Status::NotADirectory);

    // the root and the volume folders are fixed; everything below belongs to the volume
    CHECK_STATUS(table.create_directory("/New"), Status::Unsupported);
    CHECK_STATUS(table.create_directory("/USB"), Status::AlreadyExists);
    CHECK_STATUS(table.create_directory("/USB/made"), Status::Ok);
    CHECK_STATUS(table.remove("/USB"), Status::Unsupported);
    CHECK_STATUS(table.remove("/"), Status::Unsupported);
    CHECK_STATUS(table.remove("/USB/made"), Status::Ok);
    CHECK_STATUS(table.rename("/USB/docs/a.txt", "/USB/docs/b.txt"), Status::Ok);
    CHECK(read_text(table, "/USB/docs/b.txt") == "alpha");
    CHECK_STATUS(table.rename("/USB/docs/b.txt", "/usb 2/docs/b.txt"), Status::Unsupported);   // between volumes
    CHECK_STATUS(table.rename("/USB", "/Other"), Status::Unsupported);
    CHECK_STATUS(table.flush(), Status::Ok);
    CHECK(table.info().type_name.view().equals("mounts") && table.info().read_only);

    // writing through a handle works like on the volume itself
    CHECK_STATUS(table.open_file("/USB/docs/new.txt", OpenMode::CreateTruncate, &file), Status::Ok);
    uint64_t n = 0;
    CHECK_STATUS(file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>("xyz"), 3), &n), Status::Ok);
    CHECK(n == 3 && file->size() == 3);
    CHECK_STATUS(file->flush(), Status::Ok);

    // unmounting while handles are open: they fail with NoMedia, the volume lives until they are closed
    CHECK(table.unmount("USB") && !table.unmount("USB"));
    CHECK(table.count() == 1 && !has(list(table, "/"), "USB"));
    CHECK(CountedVolume::alive == 2);
    uint8_t buf[8];
    CHECK_STATUS(file->read(0, ByteView(buf), &n), Status::NoMedia);
    CHECK_STATUS(file->write(0, ConstByteView(buf, 1), &n), Status::NoMedia);
    CHECK_STATUS(file->resize(1), Status::NoMedia);
    CHECK_STATUS(file->flush(), Status::NoMedia);
    CHECK_STATUS(table.open_directory("/USB/docs", &dir), Status::NotFound);
    file.reset();
    CHECK(CountedVolume::alive == 1);

    // an open directory too
    CHECK_STATUS(table.open_directory("/usb 2/docs", &dir), Status::Ok);
    CHECK(table.unmount_owner(11) == 1 && table.unmount_owner(11) == 0);
    CHECK_STATUS(dir->next(&entry), Status::NoMedia);
    dir.reset();
    CHECK(CountedVolume::alive == 0 && table.count() == 0);

    // a volume whose medium vanished answers NoMedia before anyone unmounts it
    SharedPointer<CountedVolume> v = volume_with(alloc, "/docs/a.txt", "gamma");
    CountedVolume* raw = v.get();
    CHECK(table.mount("Stick", SharedPointer<IVolume>(v), 5));
    v.reset();
    CHECK(read_text(table, "/Stick/docs/a.txt") == "gamma");
    raw->gone = true;
    Status status;
    CHECK(read_text(table, "/Stick/docs/a.txt", &status) == "" && status == Status::NoMedia);
    CHECK_STATUS(table.stat("/Stick/docs", &entry), Status::NoMedia);
    CHECK_STATUS(table.open_directory("/Stick", &dir), Status::NoMedia);
    CHECK(table.flush() == Status::Ok);   // (nothing usable to flush)
    CHECK(has(list(table, "/"), "Stick"));   // (still listed until it is unmounted)
    table.unmount("Stick");
    CHECK(CountedVolume::alive == 0);

    // names are cleaned up, and there is a limit
    for (const char* odd : {"a/b", "  padded  ", "", "..", ".", "tab\there"}) {
      SharedPointer<CountedVolume> w = volume_with(alloc, "/docs/a.txt", "x");
      FixedString<MountTable::kMaxName + 1> used;
      CHECK(table.mount(odd, SharedPointer<IVolume>(w), 9, &used));
      const StringView u = used.view();
      CHECK(!u.empty() && !u.contains('/') && !u.contains('\t') && !u.equals("..") && !u.equals(".") && u.trimmed().size() == u.size());
    }
    CHECK(table.unmount_owner(9) == 6);
    std::string longname(200, 'n');
    SharedPointer<CountedVolume> w = volume_with(alloc, "/docs/a.txt", "x");
    FixedString<MountTable::kMaxName + 1> used;
    CHECK(table.mount(StringView(longname.data(), longname.size()), SharedPointer<IVolume>(w), 3, &used) && used.size() <= MountTable::kMaxName);
    table.unmount_owner(3);
    for (uint32_t i = 0; i < MountTable::kMaxMounts; ++i) {
      SharedPointer<CountedVolume> x = volume_with(alloc, "/docs/a.txt", "x");
      CHECK(table.mount("Disk", SharedPointer<IVolume>(x), 7));
    }
    SharedPointer<CountedVolume> extra = volume_with(alloc, "/docs/a.txt", "x");
    CHECK(!table.mount("One too many", SharedPointer<IVolume>(extra), 7));
    CHECK(list(table, "/").size() == MountTable::kMaxMounts);
    CHECK(table.unmount_owner(7) == MountTable::kMaxMounts);
    extra.reset();
    CHECK(!table.mount("Null", SharedPointer<IVolume>(), 1));
  }
  CHECK(CountedVolume::alive == 0 && alloc.live == 0);
}

// ------------------------------------------------------------------------------------------------ the system filesystem

struct Notes {
  std::vector<std::string> lines;
  static void add(void* self, StringView text) { static_cast<Notes*>(self)->lines.push_back(std::string(text.data(), text.size())); }
};

static void test_system_file_system() {
  TestAllocator alloc;
  {
    FakePci pci;
    FakeStickHost host;
    FakeUsb usb;
    usb.hosts.push_back(&host);
    SystemStorageDevices storage(pci, &usb, alloc, nullptr);
    SystemFileSystem files(alloc, storage);
    Notes notes;
    files.set_notifier(Notes::add, &notes);
    IFileSystem& fs = files.file_system();

    // nothing at start-up: the memory filesystem
    CHECK(!files.from_storage() && list(fs, "/").size() == 1 && has(list(fs, "/"), "Memory"));
    CHECK(read_text(fs, "/Memory/Welcome.txt").size() > 10);
    CHECK(files.poll() == 0);

    // a stick is plugged in: its volume appears under its label
    FakeStickHost::Stick a = make_stick(stick_with_files());
    host.plug(0, &a);
    CHECK(files.poll() == 1);
    CHECK(has(list(fs, "/"), "SMALLVOL") && list(fs, "/").size() == 2);
    CHECK(read_text(fs, "/SMALLVOL/HELLO.TXT") == "hello fat");
    CHECK(notes.lines.size() == 1 && notes.lines[0].find("mounted") != std::string::npos);
    UniquePointer<IFile> big;
    CHECK_STATUS(fs.open_file("/SMALLVOL/BIG.BIN", OpenMode::Read, &big), Status::Ok);
    UniquePointer<IDirectory> dir;
    CHECK_STATUS(fs.open_directory("/SMALLVOL", &dir), Status::Ok);
    uint8_t data[3000];
    uint64_t got = 0;
    CHECK_STATUS(big->read(0, ByteView(data), &got), Status::Ok);
    CHECK(got == 3000 && memcmp(data, pattern(3000, 5).data(), 3000) == 0);

    // a second stick with the same label
    FakeStickHost::Stick b = make_stick(stick_with_files());
    host.plug(1, &b);
    CHECK(files.poll() == 1 && has(list(fs, "/"), "SMALLVOL 2"));
    CHECK(read_text(fs, "/SMALLVOL 2/HELLO.TXT") == "hello fat");

    // the first is pulled out with a file and a folder open on it
    host.unplug(0);
    CHECK(files.poll() == 1);
    CHECK(!has(list(fs, "/"), "SMALLVOL") && has(list(fs, "/"), "SMALLVOL 2"));
    DirectoryEntry entry;
    CHECK_STATUS(big->read(0, ByteView(data), &got), Status::NoMedia);
    CHECK_STATUS(dir->next(&entry), Status::NoMedia);
    Status status;
    CHECK(read_text(fs, "/SMALLVOL/HELLO.TXT", &status) == "" && status == Status::NotFound);
    CHECK(read_text(fs, "/SMALLVOL 2/HELLO.TXT") == "hello fat");   // the other stick is not affected
    CHECK(notes.lines.back().find("removed") != std::string::npos);
    big.reset();
    dir.reset();

    // the same stick again (a new device on the same port), then its image changed in between: what is read is what is there now
    Bytes changed = stick_with_files();
    FakeStickHost::Stick c = make_stick(changed);
    c.image[(33 + 2 - 2) * 512] = 'H';
    host.plug(0, &c);
    CHECK(files.poll() == 1);
    CHECK(read_text(fs, "/SMALLVOL/HELLO.TXT") == "Hello fat");

    // a partitioned stick, one with no filesystem, and a keyboard
    FakeStickHost::Stick p = make_stick(partitioned_stick());
    host.plug(2, &p);
    CHECK(files.poll() == 1);
    CHECK(read_text(fs, "/SMALLVOL 3/HELLO.TXT") == "part");
    FakeStickHost::Stick blank = make_stick(Bytes(2880 * 512, 0));
    host.plug(3, &blank);
    CHECK(files.poll() == 1);
    CHECK(list(fs, "/").size() == 4 && notes.lines.back().find("no FAT volume") != std::string::npos);
    host.unplug(3);
    CHECK(files.poll() == 1 && list(fs, "/").size() == 4);
    host.unplug(0);
    host.unplug(1);
    host.unplug(2);
    CHECK(files.poll() == 3);
    CHECK(list(fs, "/").size() == 1 && has(list(fs, "/"), "Memory"));   // back to the memory filesystem only

    files.stop_service();   // (never started: harmless)
    storage.shutdown();
  }
  CHECK(alloc.live == 0);
}

static void test_boot_time_devices() {
  // sticks that were already plugged in when the filesystem was built are mounted by the constructor and leave like the others
  TestAllocator alloc;
  {
    FakePci pci;
    FakeStickHost host;
    FakeUsb usb;
    usb.hosts.push_back(&host);
    SystemStorageDevices storage(pci, &usb, alloc, nullptr);
    FakeStickHost::Stick a = make_stick(stick_with_files("BOOTDISK   "));
    host.plug(0, &a);
    StorageEvent e;
    CHECK(storage.poll(&e) && e.kind == StorageEvent::Kind::Added);
    e = StorageEvent();
    SystemFileSystem files(alloc, storage);
    CHECK(files.from_storage() && has(list(files.file_system(), "/"), "BOOTDISK") && !has(list(files.file_system(), "/"), "Memory"));
    CHECK(read_text(files.file_system(), "/BOOTDISK/HELLO.TXT") == "hello fat");
    host.unplug(0);
    CHECK(files.poll() == 1 && list(files.file_system(), "/").empty());
    storage.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_storage_hotplug();
  test_yank_in_the_middle();
  test_mount_table();
  test_system_file_system();
  test_boot_time_devices();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("StorageHotplugTest: all passed\n");
  return 0;
}
