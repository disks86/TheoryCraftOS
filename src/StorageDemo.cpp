#include "StorageDemo.hpp"
#include "console.hpp"

using namespace hardware;

namespace {
constexpr char kScratchMarker[] = "TCOS-SCRATCH";

void put_size(uint64_t bytes) {
  if (bytes >= (1ull << 30)) {
    console::put_dec(bytes >> 30);
    console::puts(" GiB");
  } else if (bytes >= (1ull << 20)) {
    console::put_dec(bytes >> 20);
    console::puts(" MiB");
  } else {
    console::put_dec(bytes >> 10);
    console::puts(" KiB");
  }
}

bool check_device(uint32_t index, IStorageDevice& dev, allocators::IAllocator& allocator) {
  const StorageInfo info = dev.info();
  console::puts("  [");
  console::put_dec(index);
  console::puts("] ");
  console::puts(dev.name());
  console::puts(": ");
  console::puts(info.model[0] ? info.model : "(no model)");
  if (info.serial[0]) {
    console::puts(", serial ");
    console::puts(info.serial);
  }
  console::puts(", ");
  put_size(info.block_count * info.block_size);
  console::puts(" (");
  console::put_dec(info.block_count);
  console::puts(" x ");
  console::put_dec(info.block_size);
  console::puts(" B");
  if (info.kind == StorageKind::Optical) console::puts(", optical");
  if (info.read_only) console::puts(", read-only");
  if (info.removable) console::puts(", removable");
  console::puts(")\n");

  const uint32_t bs = info.block_size;
  // Room for the first 130 blocks (more than one bounce chunk of 64 KiB for 512-byte blocks) twice.
  const uint32_t span = info.block_count < 130 ? static_cast<uint32_t>(info.block_count) : 130;
  uint8_t* big = static_cast<uint8_t*>(allocator.allocate(uint64_t{span} * bs * 2 + bs));
  if (!big) {
    console::puts("      out of memory\n");
    return false;
  }
  uint8_t* piecewise = big + uint64_t{span} * bs;
  uint8_t* last = piecewise + uint64_t{span} * bs;
  bool ok = true;

  if (!dev.read(0, span, big)) {
    console::puts("      read of the first blocks: FAILED\n");
    ok = false;
  } else {
    console::puts("      block 0: ");
    if (bs >= 512 && big[510] == 0x55 && big[511] == 0xAA) console::puts("boot signature 55 AA");
    else if (bs >= 2048 && big[0] == 0 && info.kind == StorageKind::Optical) console::puts("read ok (optical)");
    else console::puts("read ok, no boot signature");
    console::puts("\n");
    // The same range in pieces of 1 and 7 blocks must give identical bytes.
    bool same = true;
    for (uint32_t b = 0; b < span && same;) {
      const uint32_t n = (b % 2) ? 7 : 1;
      const uint32_t take = n < span - b ? n : span - b;
      same = dev.read(b, take, piecewise + uint64_t{b} * bs);
      b += take;
    }
    same = same && __builtin_memcmp(big, piecewise, uint64_t{span} * bs) == 0;
    console::puts(same ? "      read in pieces matches one big read: ok\n" : "      read in pieces: MISMATCH\n");
    ok &= same;
    uint32_t hash = 2166136261u;  // FNV-1a over the first 64 KiB, comparable with a host-side checksum
    for (uint32_t i = 0; i < 65536 && i < uint64_t{span} * bs; ++i) hash = (hash ^ big[i]) * 16777619u;
    console::puts("      checksum of the first 64 KiB: ");
    console::put_hex(hash);
    console::puts("\n");
  }

  if (info.block_count > 1) {
    const bool got = dev.read(info.block_count - 1, 1, last);
    console::puts(got ? "      last block: read ok\n" : "      last block: read FAILED\n");
    ok &= got;
    // Reading past the end must be refused rather than wander off.
    const bool refused = !dev.read(info.block_count, 1, last);
    console::puts(refused ? "      read past the end refused: ok\n" : "      read past the end: NOT REFUSED\n");
    ok &= refused;

    if (!info.read_only && big[0] == 'T' && __builtin_memcmp(big, kScratchMarker, sizeof(kScratchMarker) - 1) == 0) {
      uint8_t* saved = last;  // the original last block, restored at the end
      uint8_t* pattern = static_cast<uint8_t*>(allocator.allocate(bs));
      uint8_t* readback = static_cast<uint8_t*>(allocator.allocate(bs));
      bool wrote = pattern && readback;
      if (wrote) {
        for (uint32_t i = 0; i < bs; ++i) pattern[i] = static_cast<uint8_t>(i * 7 + 13);
        wrote = dev.write(info.block_count - 1, 1, pattern) && dev.flush() &&
                dev.read(info.block_count - 1, 1, readback) && __builtin_memcmp(pattern, readback, bs) == 0;
        dev.write(info.block_count - 1, 1, saved);
        dev.flush();
      }
      console::puts(wrote ? "      write + flush + read back (scratch device): ok\n"
                          : "      write + flush + read back (scratch device): FAILED\n");
      ok &= wrote;
      allocator.deallocate(pattern);
      allocator.deallocate(readback);
    }
  }
  allocator.deallocate(big);
  return ok;
}
}  // namespace

bool run_storage_demo(SystemStorageDevices& storage, allocators::IAllocator& allocator) {
  bool ok = true;
  for (uint32_t i = 0; i < storage.count(); ++i) ok &= check_device(i, *storage.at(i), allocator);
  return ok;
}
