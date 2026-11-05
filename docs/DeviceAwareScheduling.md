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

- **Wake-up** (`place_ready_locked`): the thread is sent to the idle core with the best `placement_score`, and that core is woken. The score
  counts, from most to least weight: the kind of core the thread asked for, its device's core, the core it last ran on, and a cache shared with
  either. The core is marked as taken, so the next thread made ready goes to a different idle core. If the best core is the one running the
  interrupt, it switches to the thread as the interrupt returns rather than at the next tick.
- **Pick** (`find_eligible`): priorities stay strict, and the highest level with an eligible thread wins. Within that level:
  1. A thread that has waited `kPlacementPatienceUs` (2 ms) or longer goes first, so placement never starves anyone.
  2. Otherwise the thread that suits this core best runs, and the thread this core was woken for outranks every other.
  3. A thread sent to another core that is still idle is left for that core.
- **Cache domains:** on x86, CPUID leaf 4 or 0x8000001D gives the number of logical processors sharing the last-level cache, and APIC ids are
  grouped by it. On AArch64 the MPIDR cluster sets the domain. RISC-V and LoongArch treat all cores as one domain.

## Interrupt routing

`kernel::choose_io_interrupt_core(kind)` spreads new device interrupts over the online cores, putting each on the core with the fewest devices
and keeping off the boot core while other cores are online. `kernel::core_hardware_id(core)` gives the APIC id for the MSI message. The
virtio-gpu, Thunderbolt and ACPI SCI interrupts are registered with their devices but stay on the core that sets them up, because their
consumers (the desktop loop, pinned to the boot core) run there.

## Monitoring

`kernel::io_device_info` returns each device's interrupt core, its interrupt count and how many threads those interrupts woke.
`kernel::thread_snapshot` reports each thread's `io_core` and `io_device`.

## Not done

- The scheduler does not move an MSI on its own to where its consumer runs (for example, a consumer pinned elsewhere). A driver can do it
  with `choose_io_interrupt_core` and `set_io_device_core`, but there is no driver callback to reprogram the message.
- The scheduler does not read NUMA nodes (ACPI SRAT), so the only topology it uses is the shared cache.
- The Wi-Fi, NPU and Vega M interrupt handlers (opt-in builds) still use `set_interrupt_handler`.
- The run queue is still a single queue under the global scheduler lock. Placement chooses which threads a core takes from it, but the
  queue is not split per core.
