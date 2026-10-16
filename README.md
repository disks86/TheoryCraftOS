# TheoryCraftOS

A minimal x86_64 UEFI operating system written in C++20 with a little
assembly (`src/hardware/x86_64/cpu.S`). It boots under OVMF in QEMU, prints to
the UEFI console and the QEMU debug console (port 0x402, shown on your terminal), then shuts down on a key.

## Layout

- `src/main.cpp`, `console.*`, `uefi.hpp`, `runtime.cpp`: entry point, logging facade, UEFI types, compiler runtime stubs.
- `src/Allocators/`: `IAllocator` and the default `Allocator`, a cache-line block pool
  that gets its memory from an `IRandomAccessMemory` (`base()` / `usable_bytes()`).
- `src/ObjectContainer/`: `IObjectContainer<T>` (count, clear, allocation-free iteration through a plain
  `ObjectCursor`, plus range-for) and its implementations `List<T>` (growable array), `LinkedList<T>`
  (doubly linked, erase while walking), `Dictionary<K, V>` (chained hash map; `Hashing.hpp` has the
  default hash/equality policies, including C strings), `BinaryTree<T>` (self-balancing AVL search tree with
  in-order iteration and pre/post-order visitors), `SortedDictionary<K, V>` (ordered map on the tree),
  `Stack<T>`, `Queue<T>` (ring buffer) and `PriorityQueue<T>` (binary heap; `Comparison.hpp` has the ordering policy). Header-only templates; every allocation goes
  through the `IAllocator` they are given. `src/ContainerDemo` checks them at boot.
- `src/Utility/`: `UniquePointer<T>` and `SharedPointer<T>` (allocator-backed; atomic reference count; a
  pointer to a base class safely owns a derived object even without a virtual destructor), `Optional<T>`,
  `Variant<Ts...>` (visit, get, emplace) and the small `Move.hpp` helpers. Header-only; no weak pointers.
  `src/UtilityDemo` checks them at boot, with a shared pointer copied concurrently on every core.
- `src/Network/`: an IPv4 stack over any `INetworkInterface`, single-threaded and polled (`INetworkStack::poll()`).
  `NetworkStack` does Ethernet framing, ARP (cache + queue of packets awaiting resolution), IPv4 and ICMP echo;
  it owns a `UdpManager`/`UdpSocket` (`IUdpSocket`), a `TcpManager`/`TcpSocket` (`ITcpSocket`: RFC 793 state
  machine, RFC 6298 retransmission timer, fast retransmit, flow control, zero-window probing, listen/accept) and
  a `DhcpClient` (`IDhcpClient`: discover/offer/request/ack, renewal at T1, rediscovery on expiry) and a
  `DnsResolver` (`IDnsResolver`/`IDnsQuery`: A records over UDP with retries, TCP fallback on truncation, CNAME
  chains, TTL cache incl. negative answers, ID/port randomization; `INetworkStack::resolve()` blocks). `IIpLayer`
  is what the transports need from IP. Not supported: IPv6/AAAA, fragmentation, congestion control, SACK, window
  scaling, DNSSEC/EDNS. Time comes from `hardware/Clock` (`IClock`, `TscClock` calibrated against the firmware timer; the CPU
  interface gained `timestamp_counter()`). `NetworkStackSet` runs one `NetworkStack` per network interface: DHCP is
  started on all of them at once (a silent network doesn't delay the others), and `resolve()` asks every configured
  interface's DNS server and takes the first answer. `src/NetworkStackDemo` reports each interface's lease, pings
  its gateway, resolves names through it, talks UDP/TCP to the host and serves HTTP on port 8080 on every interface.
- `src/hardware/StorageDevice/`: block storage. `IStorageDevice` reads/writes/flushes whole logical blocks
  (synchronous, any buffer alignment and size; `StorageDeviceBase` checks ranges and chunks big requests through a
  DMA-able bounce buffer). Native drivers: `Virtio/` (virtio-blk, virtio-scsi), `NVMe/`, `Ahci/` (SATA disks and
  ATAPI drives; `AhciController` is shared by the drives on its ports), `Ide/` (legacy parallel ATA/ATAPI, PIO),
  `Usb/` (mass storage, Bulk-Only Transport, over the bulk endpoints of the xHCI and EHCI drivers), and `UEFI/` (firmware
  Block I/O, for any controller the firmware drives). `SystemStorageDevices` finds them all (partitions are not
  listed); `src/StorageDemo` reports each device, cross-checks reads and, only on a device whose first block begins
  with `TCOS-SCRATCH`, tests write/flush/read-back. Not covered: floppy and SD/MMC (only through the firmware),
  and the LSI/MegaRAID/MPT/PVSCSI SCSI adapters (only if the firmware has a driver for them - the stock OVMF does not).
- `src/Synchronization/`: `SpinLock`/`ScopedLock` (spin with the CPU interface's `pause()`), `Synchronized<T>`
  (any type behind a lock; `with()` for one locked operation, `access()` for a lock-holding handle that can
  be iterated) and `AtomicSharedPointer<T>`. `Allocators/ThreadSafeAllocator` serializes any allocator.
  `ObjectContainer/Concurrent/` has a thread-safe version of every container (`ConcurrentList`,
  `ConcurrentLinkedList`, `ConcurrentDictionary`, `ConcurrentBinaryTree`, `ConcurrentSortedDictionary`,
  `ConcurrentStack`, `ConcurrentQueue`, `ConcurrentPriorityQueue`): values are copied in and out, and nothing
  blocks. `src/ConcurrentDemo` fills them from tasks on every core at boot.
- `src/CPUScheduler/`: `ICPUScheduler` and the default `Scheduler`, a cooperative run-to-completion
  scheduler with high-performance and efficiency queues (growable `Queue`s on an `IAllocator`, guarded by a
  `SpinLock`) that spreads tasks over the cores the `ICentralProcessingUnit` reports (tasks on other cores
  must not call firmware). Give it a `ThreadSafeAllocator`.
- `src/hardware/`: every class that touches hardware derives from `hardware::IHardware`
  (`name()`, `initialize()`, `shutdown()`): `IoPort`, `DebugConsole`, `TextConsole`, and the CPUs.
  - `hardware/CentralProcessingUnit/`: `ICentralProcessingUnit` (instructions plus core count, core kinds,
    start-on-core) and its implementations. `X86_64Cpu` (shared base: CPUID, multi-core via UEFI MP
    services) and `SystemCpu` sit at the top. Each vendor has a subfolder holding its base class and one
    small class per microarchitecture: `Intel/` (Conroe ... Diamond Rapids, Atom lines), `AMD/` (K8, K10,
    Bulldozer, Piledriver, Zen ... Zen 5), `Hygon/`, `Zhaoxin/`, and `QEMU/` (the generic qemu64 model).
    The generation is chosen at boot from CPUID vendor and family/model/stepping, via the rule table in
    each vendor's `*Generations.cpp`; this covers every x86-64 model in `qemu-system-x86_64 -cpu help`.
  - `hardware/GraphicsProcessingUnit/`: `IGraphicsProcessingUnit` (mode setting, 2D drawing, text rendering),
    `FramebufferGpu` (software drawing + 8x16 font on a linear framebuffer), `SystemGpu` (finds the PCI display
    device and picks a class). Vendor folders: `QEMU/` (std VGA with real mode setting through the Bochs VBE
    registers), `Intel/`, `AMD/`, `NVIDIA/` (identify the device but still use the firmware framebuffer until
    native drivers exist), and `UEFI/` (`UefiGopGpu`, the fallback for any other display). `src/GraphicsConsole`
    is a scrolling text console that only uses the GPU interface.
  - `hardware/Bus/`: `IBus` and one subfolder per bus type, each with an interface and implementations.
    - `Pci/`: `IPciBus` (config access, BARs, capability list), `PciBusBase` (shared scanning), `LegacyPciBus`
      (I/O ports 0xCF8/0xCFC, 256-byte config space).
    - `Pcie/`: `IPcieBus` (adds the extended capability list) and `EcamPcieBus` (memory-mapped config space,
      located through the ACPI MCFG table).
    - `Usb/`: `IUsbBus` and `UsbBus` (finds host controllers on PCI), `IUsbHostController` (one controller:
      ports, enumerated device descriptors) and `UsbTypes.hpp`. `Usb/Xhci/` holds `IXhciController` and the
      polling `XhciController` driver (firmware hand-off, reset, command/event rings, port reset, slot
      enable, address device, control and interrupt-IN transfers, configuration) plus its ring and register helpers. At boot it enumerates
      every device on the root ports and reads its descriptors; HID class drivers (keyboard, mouse, tablet)
      build on it. `Bus/Usb/Ehci` is a USB 2.0 driver (async schedule for control/bulk, periodic for interrupt endpoints; high-speed devices on root ports only, others go to the companion controller); `Bus/Usb/Uhci` and `Bus/Usb/Ohci` drive the USB 1.1 controllers (low/full speed) on top of a shared `Bus/Usb/Legacy/LegacyHostController` (enumeration and control requests); on chips with an EHCI controller the companions start after it. Hubs and other device classes are not handled.
    - `SystemPciBus` picks PCIe/ECAM when firmware provides MCFG, else legacy PCI.
  - `hardware/Keyboard/`, `hardware/Mouse/`: `IKeyboard` (key press/release events as USB HID usages, polled)
    and `IMouse` (relative movement or absolute position, wheel, buttons), with shared queue bases and
    `Composite*` classes that merge several devices. One subfolder per transport QEMU offers:
    `Ps2/` (i8042 keyboard with set-1 decoding, mouse with IntelliMouse wheel), `Usb/` (HID keyboard in boot
    protocol; mouse and tablet through a HID report-descriptor parser) and `Virtio/` (virtio-input keyboard,
    mouse and tablet). `hardware/SystemInput` finds all of them and exposes one keyboard and one mouse.
  - `hardware/Bus/Ps2/` (`I8042Controller`) and `hardware/Bus/Virtio/` (modern virtio-PCI transport,
    virtqueues, `VirtioInputDevice`); `Bus/Usb/Hid/` has the report-descriptor parser and interface finder.
  - `hardware/ImageSignalProcessor/`: `IImageSignalProcessor` (raw Bayer frame -> RGB image, settings, frame
    statistics) with `ImageTypes.hpp`. `Software/` is the CPU pipeline (bilinear demosaic, black level,
    white-balance gains, gamma table); `Intel/` has IPU6/IPU7 classes that match the PCI device and still use
    that path until a native driver exists; `SystemImageSignalProcessor` picks one. QEMU has no ISP
    hardware, so only the software path runs there. `src/IspDemo` draws a synthetic frame through it.
  - `hardware/NetworkInterface/`: `INetworkInterface` (MAC, link state, transmit/receive of raw Ethernet
    frames, statistics; polled) with native drivers - `Virtio/` (virtio-net, modern interface), `Intel/` (e1000
    family: 82540EM/82544GC/82545EM) and `Realtek/` (RTL8139) - and `UEFI/`, a fallback that borrows the
    firmware's driver (UEFI Simple Network Protocol) for any other NIC. `SystemNetworkInterfaces` finds every
    NIC and picks the driver. `src/NetworkDemo` sends ARP requests to QEMU's user-mode gateway at boot.
    Not covered natively: e1000e/igb, pcnet, ne2k, vmxnet3 (the firmware fallback handles those in QEMU).
  - `hardware/Acpi/`: `AcpiTables` (RSDP -> XSDT/RSDT table lookup).
  - `hardware/RandomAccessMemory/`: `IRandomAccessMemory` (implemented by `UefiRandomAccessMemory`).
  - `hardware/NeuralProcessingUnit/`: `INeuralProcessingUnit` (int8 and float matrix products, activations,
    softmax). `Software/` splits products by rows over every core through the scheduler; `Intel/` (NPU
    "VPU": Meteor Lake to Panther Lake) and `AMD/` (XDNA) classes match the PCI device and still use that
    path until a native driver exists; `SystemNeuralProcessingUnit` picks one. QEMU has no NPU hardware, so
    only the software path runs there. `src/NpuDemo` checks it at boot. `src/Math/FloatMath` supplies exp/ln.
  - `hardware/SystemHardware.hpp`: aliases `SystemCpu` / `SystemRam` to the classes for this architecture.
  - `hardware/x86_64/port.S`: port I/O assembly for `IoPort`; `asm.hpp` is private to `hardware/`.

## Requirements

`cmake`, `ninja` (or make), `x86_64-w64-mingw32-gcc/g++` (mingw-w64),
`qemu-system-x86_64`, OVMF/edk2 firmware, `gdb`.

mingw-w64 is used because it emits PE32+ directly with the UEFI-compatible
calling convention - no gnu-efi or lld needed.

## Build & run

```sh
cmake -S . -B build -G Ninja
cmake --build build              # produces build/esp/EFI/BOOT/BOOTX64.EFI
cmake --build build --target run # boot in QEMU (OS output on your terminal)
```

### Debugging in QEMU

`scripts/gdb-proxy.py` first rebuilds `BOOTX64` (output goes to the debugger console),
then starts QEMU (frozen at reset, gdbstub on :1234) and
bridges gdb to it: `target remote | scripts/gdb-proxy.py`. It rewrites
software breakpoints to hardware ones, so breakpoints set before OVMF has
loaded the image still work. OS output appears on gdb's stderr and in
`<build>/debugcon.log`.

```sh
gdb -ex 'symbol-file build/BOOTX64.efi' -ex 'target remote | scripts/gdb-proxy.py' \
    -ex 'break kernel_main' -ex continue
```

### CLion

`.run/Debug TheoryCraftOS.run.xml` is a *GDB Remote Debug* configuration using
the command above. Select **Debug TheoryCraftOS** in the run dropdown (not the
auto-generated `BOOTX64` entry, which tries to run the `.efi` natively) and
press Debug. QEMU opens, the debugger is paused at reset; press Resume.
Use the **QEMU** config to just run the OS. Set the debugger to GDB (the
bundled one works). If the symbol path is wrong for your build directory,
edit it in the configuration.
