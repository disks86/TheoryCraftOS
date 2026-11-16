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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Network/Link/FrameTap.hpp"
#include "Process/LinuxNetlinkMessages.hpp"
#include "Process/LinuxPacketRing.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Objects.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

// System V interprocess communication for Linux programs: shared memory segments (shmget, shmat, shmdt, shmctl), semaphore sets (semget, semop, semtimedop,
// semctl) and message queues (msgget, msgsnd, msgrcv, msgctl). The three kinds each have a table for the whole system, found by key or by id, that outlives
// the processes using it until it is removed (IPC_RMID), as on Linux.
//
// Locking: the tables are guarded by the system's one lock and wake-up (ISystemSync, the one the sockets and pipes use), because a semaphore operation or a
// message call that cannot go on must look and go to sleep without a wake-up falling in between. Every change a waiter could be waiting for wakes everybody.
// Nothing outside LinuxIpc.cpp touches the tables.
namespace process {

class Process;

class LinuxIpcManager {
 public:
  static constexpr uint32_t kSegments = 128, kSemSets = 128, kSemsPerSet = 128, kQueues = 64, kUndos = 256;
  static constexpr uint64_t kSegmentMax = 64ull << 20;      // one segment (SHMMAX): what one block of shared memory may be
  static constexpr uint32_t kSemaphoreMax = 32767;          // SEMVMX
  static constexpr uint32_t kOperationsMax = 64;            // sembuf entries in one semop (SEMOPM)
  static constexpr uint32_t kQueueBytes = 16384, kMessageMax = 8192;      // MSGMNB, MSGMAX

  LinuxIpcManager(ISystemSync& sync, allocators::IAllocator& allocator) : sync_(sync), allocator_(allocator) {}
  ~LinuxIpcManager();
  LinuxIpcManager(const LinuxIpcManager&) = delete;
  LinuxIpcManager& operator=(const LinuxIpcManager&) = delete;
  ISystemSync& sync() { return sync_; }
  allocators::IAllocator& allocator() { return allocator_; }

  // Who made a thing and who may use it (struct ipc64_perm). Nothing is checked against it: every program runs as root.
  struct Permission {
    uint32_t key = 0, uid = 0, gid = 0, creator_uid = 0, creator_gid = 0, mode = 0;
    uint16_t sequence = 0;      // goes up each time the slot is used again, so that an old id does not name the new thing
  };
  struct Segment {
    bool used = false;
    bool removed = false;       // IPC_RMID was called: it goes when the last process has detached it, and no key finds it meanwhile
    Permission permission;
    uint64_t size = 0;          // as asked for (the block is whole pages)
    MemoryBlock* block = nullptr;      // one use is this table's; each mapping has its own
    uint64_t attach_time = 0, detach_time = 0, change_time = 0;
    uint32_t creator = 0, last = 0;    // process ids
  };
  struct SemaphoreSet {
    bool used = false;
    Permission permission;
    uint32_t count = 0;
    uint64_t operation_time = 0, change_time = 0;
    uint16_t value[kSemsPerSet] = {};
    uint32_t last[kSemsPerSet] = {};                  // the process that last changed each
    uint16_t waiting_for_more[kSemsPerSet] = {};      // threads waiting for the value to rise (GETNCNT), and for it to be zero (GETZCNT)
    uint16_t waiting_for_zero[kSemsPerSet] = {};
  };
  // What a process's semaphore operations with SEM_UNDO come to: taken back when the process ends.
  struct Undo {
    bool used = false;
    uint64_t process = 0;
    uint32_t id = 0;
    uint32_t semaphore = 0;
    int32_t adjust = 0;
  };
  // A queue's messages lie one after another in `data`: the type (8 bytes), the size (4 bytes), the text.
  struct Queue {
    bool used = false;
    Permission permission;
    UniquePointer<ByteBuffer> data;
    uint64_t messages = 0, bytes = 0, limit = kQueueBytes;      // bytes: of text, which is what the limit counts
    uint64_t send_time = 0, receive_time = 0, change_time = 0;
    uint32_t last_sender = 0, last_receiver = 0;
  };

  // A POSIX message queue (mq_open): found by name until it is unlinked, there until its last descriptor is closed. Its messages lie in `data` in the
  // order they come out (highest priority first, the oldest first among equals): the priority (4 bytes), the size (4 bytes), the text.
  static constexpr uint32_t kNamedQueues = 32, kQueueNameBytes = 64;
  static constexpr uint64_t kNamedMessagesMax = 256, kNamedMessageBytesMax = 16384, kNamedMessagesDefault = 10, kNamedMessageBytesDefault = 8192;
  struct NamedQueue {
    bool used = false;
    bool unlinked = false;
    Permission permission;      // (only the mode and the sequence number are used)
    char name[kQueueNameBytes] = {};
    uint32_t name_size = 0;
    uint32_t opens = 0;         // descriptors there are for it
    uint64_t capacity = kNamedMessagesDefault, message_bytes = kNamedMessageBytesDefault, messages = 0;
    UniquePointer<ByteBuffer> data;
    uint32_t waiting_receivers = 0;      // threads in mq_receive (a message that comes for one of them is no news for mq_notify)
    // mq_notify: the process to tell when a message comes to the empty queue (0: nobody), and with which signal (0: SIGEV_NONE, only the registration)
    uint64_t notify_process = 0;
    uint32_t notify_signal = 0;
  };
  NamedQueue named[kNamedQueues];

  // inotify (LinuxInotify.cpp): an instance is what inotify_init gives a descriptor for; its watches are change notifications of the object manager
  // (Objects.hpp), which the Linux file calls report what they change to. A watch on a file watches the file's folder and keeps the file's name.
  static constexpr uint32_t kInotifyInstances = 32, kInotifyWatches = 128, kInotifyPathBytes = 512, kInotifyNameBytes = 128;
  static constexpr uint64_t kInotifyPendingBytes = 64 * 1024;      // events waiting to be read in one instance; more are lost (IN_Q_OVERFLOW)
  struct InotifyInstance {
    bool used = false;
    uint16_t sequence = 0;
    int32_t next_descriptor = 1;
    bool overflowed = false;              // an IN_Q_OVERFLOW is owed
    UniquePointer<ByteBuffer> pending;    // events as read() hands them out
  };
  struct InotifyWatch {
    bool used = false;
    uint32_t instance = 0;                // its instance's link
    int32_t descriptor = 0;
    uint32_t object = 0;                  // the change notification
    uint32_t mask = 0;
    bool on_file = false;
    char path[kInotifyPathBytes] = {};    // what is watched (the host path), to know a path that is watched already
    uint32_t path_size = 0;
    char name[kInotifyNameBytes] = {};    // a file's name in its folder
    uint32_t name_size = 0;
  };
  InotifyInstance inotify_instances[kInotifyInstances];
  InotifyWatch inotify_watches[kInotifyWatches];

  // AF_NETLINK sockets (LinuxNetlink.cpp): what socket(AF_NETLINK, ...) gives a descriptor for. The kernel's answers and notices wait in `pending`.
  static constexpr uint32_t kNetlinkSockets = 64;
  static constexpr uint64_t kNetlinkPendingBytes = 512 * 1024;      // datagrams waiting to be read in one socket; more are lost (ENOBUFS)
  struct NetlinkSocket {
    bool used = false;
    uint16_t sequence = 0;
    uint32_t protocol = 0;
    uint32_t port = 0;                    // nl_pid: its address (0 until it is bound, by bind() or by its first use)
    bool bound = false;
    uint32_t groups = 0;                  // the multicast groups it listens to (bit n-1: group n)
    uint32_t peer_port = 0, peer_groups = 0;      // connect(): where send() goes (0: the kernel)
    bool overflowed = false;              // datagrams were lost: the next read says ENOBUFS
    bool packet_info = false, cap_ack = false, ext_ack = false, no_enobufs = false, strict_check = false, pass_credentials = false, broadcast_error = false,
         all_namespaces = false, filtered = false;
    uint32_t send_buffer = 212992, receive_buffer = 212992;
    uint64_t receive_timeout_ns = ~uint64_t{0}, send_timeout_ns = ~uint64_t{0};
    UniquePointer<ByteBuffer> pending;    // datagrams: netlink::kQueuedHead bytes (length, group, sender) and the bytes
    UniquePointer<linuxabi::netlink::Snapshot> seen;      // the network as it was last told, for a socket that listens for changes
  };
  NetlinkSocket netlink_sockets[kNetlinkSockets];

  // AF_PACKET sockets (LinuxPacket.cpp): what socket(AF_PACKET, ...) gives a descriptor for. Each has a tap on every network interface.
  static constexpr uint32_t kPacketSockets = 32, kPacketTaps = linuxabi::netlink::kMostInterfaces - 1;
  struct PacketFrame {
    network::FrameInfo info;
    uint32_t interface = 0;        // the interface's number as Linux counts them (1: the loopback)
    uint32_t kept = 0;             // how much of it the socket's filter wants
    uint8_t data[network::FrameTap::kMaxFrame] = {};
  };
  struct PacketSocket {
    bool used = false;
    uint16_t sequence = 0;
    bool cooked = false;                  // SOCK_DGRAM: frames are handed over and taken without their link header
    uint16_t protocol = 0;                // the ether type it listens for (3: every one; 0: none)
    uint32_t interface = 0;               // the interface it is bound to (0: every one)
    bool aux_data = false, ignore_outgoing = false;
    uint32_t packets = 0, drops = 0;      // PACKET_STATISTICS, since last asked
    uint32_t send_buffer = 212992, receive_buffer = 212992;
    uint64_t receive_timeout_ns = ~uint64_t{0};
    uint32_t next_tap = 0;                // (the interfaces are asked in turn)
    SharedPointer<network::IFrameTap> taps[kPacketTaps];
    UniquePointer<ByteBuffer> filter;     // SO_ATTACH_FILTER: a classic BPF program (LinuxBpf.hpp), or nothing
    bool locked_filter = false;           // SO_LOCK_FILTER
    UniquePointer<PacketFrame> held;      // the next frame, taken from a tap and through the filter already (has_held)
    bool has_held = false;
    // Memory-mapped rings (LinuxPacketRing.hpp): memory the socket and the programs that mapped it share. With a receive ring frames go there and
    // not to recv(); with a transmit ring send() sends what the program left there.
    uint8_t version = 0;                  // PACKET_VERSION
    uint32_t reserve = 0;                 // PACKET_RESERVE
    bool transmit_offsets = false;        // PACKET_TX_HAS_OFF
    bool skip_wrong = false;              // PACKET_LOSS: a frame to send that makes no sense is passed over, not an error
    BlockRef receive_block, transmit_block;
    linuxabi::ring::Request receive_request, transmit_request;
    linuxabi::ring::Receiver receiver;
    linuxabi::ring::Transmitter transmitter;
    int32_t fanout = -1;                  // the fan-out group it joined (an index into packet_fanouts), or none
  };
  PacketSocket packet_sockets[kPacketSockets];
  // PACKET_FANOUT: sockets that share the frames of one protocol and interface, each frame going to one of them.
  static constexpr uint32_t kPacketFanouts = 16;
  struct PacketFanout {
    bool used = false;
    uint16_t id = 0;
    uint8_t type = 0;                     // PACKET_FANOUT_HASH, ... (how a frame's socket is chosen)
    uint16_t flags = 0;                   // PACKET_FANOUT_FLAG_*, as they were handed in
    uint16_t protocol = 0;                // what every member listens for,
    uint32_t interface = 0;               // and where
    uint32_t count = 0, most = kPacketSockets;
    uint64_t members[kPacketSockets] = {};      // the sockets' links, in the order they joined
    UniquePointer<ByteBuffer> chooser;    // PACKET_FANOUT_CBPF: a classic BPF program whose answer picks the member
    // PACKET_FANOUT_LB: which frame of the group's a frame is. Every member is shown every frame, so the first member to look at one gives it the
    // next number, and the others find it here (by its interface and the number the interface gave it) when they come to it.
    static constexpr uint32_t kRemembered = 256;      // (more than a member's taps hold)
    struct Ranked {
      uint32_t interface = 0, sequence = 0, rank = 0;
    };
    Ranked ranked[kRemembered];
    uint32_t ranked_count = 0, next_rank = 0;
  };
  PacketFanout packet_fanouts[kPacketFanouts];

  Segment segments[kSegments];
  SemaphoreSet sets[kSemSets];
  Undo undos[kUndos];
  Queue queues[kQueues];

 private:
  ISystemSync& sync_;
  allocators::IAllocator& allocator_;
};

// The system's one manager, made on first use beside the object manager (null before there is one).
LinuxIpcManager* system_linux_ipc();
// Tests: use this one instead (null: back to the made-on-first-use one).
void set_system_linux_ipc(LinuxIpcManager* manager);

namespace linuxabi {

uint64_t sys_shmget(Process& p, uint64_t key, uint64_t size, uint64_t flags);
uint64_t sys_shmat(Process& p, uint64_t id, uint64_t address, uint64_t flags);
uint64_t sys_shmdt(Process& p, uint64_t address);
uint64_t sys_shmctl(Process& p, uint64_t id, uint64_t command, uint64_t buffer);
uint64_t sys_semget(Process& p, uint64_t key, uint64_t count, uint64_t flags);
// semop and semtimedop (`timeout`: a struct timespec to wait at most, or 0)
uint64_t sys_semtimedop(Process& p, uint64_t id, uint64_t operations, uint64_t count, uint64_t timeout);
uint64_t sys_semctl(Process& p, UserArch arch, uint64_t id, uint64_t semaphore, uint64_t command, uint64_t argument);
uint64_t sys_msgget(Process& p, uint64_t key, uint64_t flags);
uint64_t sys_msgsnd(Process& p, uint64_t id, uint64_t message, uint64_t size, uint64_t flags);
uint64_t sys_msgrcv(Process& p, uint64_t id, uint64_t message, uint64_t size, uint64_t type, uint64_t flags);
uint64_t sys_msgctl(Process& p, uint64_t id, uint64_t command, uint64_t buffer);
// POSIX message queues. The name is as the C library hands it on (without the leading slash); a queue's descriptor can be polled.
uint64_t sys_mq_open(Process& p, uint64_t name, uint64_t flags, uint64_t mode, uint64_t attributes);
uint64_t sys_mq_unlink(Process& p, uint64_t name);
uint64_t sys_mq_timedsend(Process& p, uint64_t fd, uint64_t message, uint64_t size, uint64_t priority, uint64_t timeout);
uint64_t sys_mq_timedreceive(Process& p, uint64_t fd, uint64_t message, uint64_t size, uint64_t priority, uint64_t timeout);
uint64_t sys_mq_notify(Process& p, uint64_t fd, uint64_t event);
uint64_t sys_mq_getsetattr(Process& p, uint64_t fd, uint64_t new_attributes, uint64_t old_attributes);
// For poll() (the system's lock held): can a message be taken from the queue behind a descriptor, and can one be put in?
void mq_readiness_locked(uint64_t link, uint32_t mode, bool* readable, bool* writable);
// inotify (LinuxInotify.cpp)
uint64_t sys_inotify_init1(Process& p, uint64_t flags);
uint64_t sys_inotify_add_watch(Process& p, uint64_t fd, uint64_t path, uint64_t mask);
uint64_t sys_inotify_rm_watch(Process& p, uint64_t fd, uint64_t descriptor);
// read() of an inotify descriptor: whole events, as many as fit.
uint64_t inotify_read(Process& p, uint64_t link, uint64_t buffer, uint64_t count, bool non_blocking);
// For poll() and FIONREAD (the system's lock held): are there events to read, and how many bytes of them are formatted already?
bool inotify_ready_locked(uint64_t link);
// The manager if one was made already (for the hooks of things that go away: nothing to do without one).
LinuxIpcManager* existing_linux_ipc();
// A Linux process ended: its semaphore operations made with SEM_UNDO are taken back.
void ipc_process_gone(uint64_t pid);

}  // namespace linuxabi
}  // namespace process
