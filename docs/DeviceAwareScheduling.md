# Device-aware scheduling (I/O affinity)

The thread scheduler (`src/Kernel/ThreadScheduler.cpp`) places threads near the devices they wait for. When a device interrupt arrives, the
handler reads the device's descriptors, completion entries and buffers, so that data is now in the interrupted core's cache. The thread that
consumes it runs fastest on that core. If that core is busy, the next best place is a core that shares its last-level cache.

The policy is in `src/CPUScheduler/IoAffinity.{hpp,cpp}`. It takes no locks and touches no hardware, so it is tested on the host
(`tests/run_host_tests.sh IoAffinityTest`, under ASan/UBSan and ThreadSanitizer).

## How a thread finds its core

- **Learnt.** Install a device's handler with `kernel::set_device_interrupt_handler(vector, device, handler, context)` instead of
  `set_interrupt_handler`. While that handler runs, its core is marked as handling `device`. A thread the handler wakes (`Event::set`,
  `Semaphore::release`, ...) learns that core (`LearnedIoCore`). A single interrupt arriving somewhere else does not move it; a run of them does.
- **Bound.** `kernel::bind_thread_to_io_device(thread, device)`, or `spawn(..., io_device)`, makes a thread follow the device's interrupt core
  even after the interrupt is moved (`set_io_device_core`). `scheduling::Scheduler::submit(fn, ctx, queue, io_device)` does this for tasks.

## Placement

- **Run queues:** each core has its own run queue, split by priority level. A thread that becomes ready joins the queue of the idle core
  chosen for it, or, when no allowed core is idle, the queue of the busy core that suits it best (`choose_queue_core`). If that queue is more
  than `kQueueImbalance` (2) longer than the shortest allowed queue, the thread goes to the shortest one instead.
- **Wake-up** (`ready_locked`): the thread is sent to the idle core with the best `placement_score`, and that core is woken. The score
  counts, from most to least weight: the kind of core the thread asked for, its device's core, the core it last ran on, a cache shared with
  either, and the same NUMA node. The core is marked as taken, so the next thread made ready goes to a different idle core. If the best core
  is the one running the interrupt, it switches to the thread as the interrupt returns rather than at the next tick.
- **Pick** (`find_eligible`): priorities stay strict. A count of ready threads per level across all queues gives the highest busy level
  directly. At that level, a core:
  1. takes from its own queue first;
  2. steals from other queues when its own has nothing it may run;
  3. always takes a thread from any queue that has waited `kPlacementPatienceUs` (2 ms), so a core busy with higher-priority work cannot
     strand the threads queued on it.

  Within a queue, the thread this core was woken for comes first, then the best score. A thread sent to another core that is still idle is
  left for that core.
- **Cache domains:** on x86, CPUID leaf 4 or 0x8000001D gives the number of logical processors sharing the last-level cache, and APIC ids are
  grouped by it. On AArch64 the MPIDR cluster sets the domain. RISC-V and LoongArch treat all cores as one domain.
- **NUMA nodes:** at boot, `kernel::apply_acpi_numa` (`src/Kernel/Numa`, used by the x86-64 and AArch64 boots) reads the ACPI SRAT
  (`src/Hardware/Acpi/Srat`) and puts each core in its proximity domain. On ARM, an SRAT GICC entry names the core by ACPI processor UID,
  which the MADT's GICC entries turn into an MPIDR. The SLIT's distances (`kernel::set_numa_distance`) break ties between equally suitable
  cores in favour of the nearest node. Without an SRAT every core is in node 0. Without a SLIT, distances are 10 within a node and 20 between
  nodes.
- **Device nodes:** `AcpiEvents::proximity_domain` finds a device's `_PXM` on the device or its nearest ancestor (usually the PCI root
  bridge). `kernel::set_io_device_node` records it (Thunderbolt does), and `choose_io_interrupt_core(kind, node)` puts the device's
  interrupt on its own node first.

## Interrupt routing

- `kernel::choose_io_interrupt_core(kind)` spreads new device interrupts over the online cores, putting each on the core with the fewest
  devices and keeping off the boot core while other cores are online. `kernel::core_hardware_id(core)` gives the APIC id for the MSI message.
- **Steering:** a driver that can move its interrupt registers a callback with `kernel::set_io_device_retarget`. For each wake-up its
  interrupt causes, the scheduler records where the woken thread has to run when it cannot run on the interrupt's core (pinned elsewhere, or
  wanting another kind of core). After 32 wake-ups (`InterruptSteering`), if three in four ran elsewhere and one core has a clear majority, the
  `io-balancer` kernel thread (started on first use) calls the callback and moves the device to that core.
  - `PciMsix::retarget`, `PciMsi::retarget` and `VirtioPciDevice::retarget_msix` rewrite the message address with the vector masked, so an
    interrupt raised during the change stays pending instead of being lost.
  - virtio-gpu and Thunderbolt offer this. Their interrupts start on the core that sets them up.
  - With `TCOS_SELFTESTS`, the boot moves the virtio-gpu completion interrupt to another core and back (`VirtioGpu::move_interrupt`) and
    checks where it lands. This passes under QEMU TCG and KVM. Under KVM the interrupt can arrive after the driver has already seen the
    command finish by polling; the driver's wait loop re-checks the ring, so this costs nothing.
- **Drivers registered** in the device table: virtio-gpu, Thunderbolt, ACPI SCI, Intel Wi-Fi, Intel NPU and Vega M.

## Monitoring

`kernel::io_device_info` returns each device's interrupt core, its interrupt count and how many threads those interrupts woke.
`kernel::thread_snapshot` reports each thread's `io_core` and `io_device`.

## The timer tick

Every core takes a tick 1000 times a second. A tick takes the global scheduler lock only when it has work: a sleeper is due, a thread was
sent to the core, the running thread may no longer run there, or a ready thread outranks it (or matches its level once its slice is over).
The tick checks this without the lock, from the first sleeper's wake time and a bit mask of non-empty priority levels. The boot self test
measured 99% of ticks lock-free on a quiet 4-core machine (`CoreInfo::lock_free_ticks`).

## Note: no static constructors

The kernel runs no static constructors. Any scheduler field whose "none" value is not zero (for example `Core::io_device`) must be set in
`kernel::start()`.

## Not done

- **Per-core locks.** One global lock still guards the run queues, the sleepers and the wait lists of `Sync.hpp`. The tick no longer takes it
  when idle, but every wake-up, block and switch still does. Replacing it means:
  - a lock per run queue, plus one per wait list or per synchronization object;
  - a wake-up protocol that cannot lose a wake-up between "decide to block" and "switch away". Linux uses `on_cpu` and a per-task lock for
    this.

  This is a rewrite of `Sync.cpp` and the switch path, and needs its own design and multi-core stress tests.
- Devices other than Thunderbolt don't look up their `_PXM` yet: they are set up before the ACPI interpreter is up.
- No real driver has moved an interrupt on physical hardware. QEMU's virtio-gpu is the device tested.
