# D-Bus

The kernel is the D-Bus message bus. There is no `dbus-daemon` process: the system bus and the session bus are two `dbus::Bus` objects inside the
kernel (`src/DBus`), and Linux programs reach them with their own unmodified client libraries (libdbus, sd-bus, GDBus) over the system's AF_UNIX
sockets.

| Bus     | Address (as Linux programs see it)                         | On the system's disk                   |
|---------|------------------------------------------------------------|----------------------------------------|
| system  | `/run/dbus/system_bus_socket` (the standard default)       | `/Linux/run/dbus/system_bus_socket`    |
| session | `/run/user/0/bus` (`DBUS_SESSION_BUS_ADDRESS`, `$XDG_RUNTIME_DIR/bus`) | `/Linux/run/user/0/bus`    |

Each address is also bound as an abstract socket name of the same text, which Windows programs can reach through ws2_32's AF_UNIX support.
`DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus` is in every Linux program's default environment (`ProgramEnvironment.cpp`). The constants are in
`filesystem::layout` (`kLinuxSystemBusSocket`, `kSystemBusSocket`, `kSessionBusSocket`, `kLinuxMachineId`).

## Files

| File | What it does |
|------|--------------|
| `Wire.hpp/.cpp` | Names, signatures, the aligned reader and writer (both byte orders), and validation of a whole body against its signature |
| `Message.hpp/.cpp` | A message: framing (`framed_size`), `decode` (checks every header field and the body), `encode` (the header is written again, the body kept as it came) |
| `Auth.hpp/.cpp` | The server side of the SASL handshake (EXTERNAL only, `NEGOTIATE_UNIX_FD`) |
| `Match.hpp/.cpp` | Match rules: parsing, comparing (for RemoveMatch), matching a message |
| `Bus.hpp/.cpp`, `BusInternal.hpp` | The router: connections, unique names, name queues, routing, reply tracking, monitors, local (in-kernel) peers |
| `BusDriver.cpp` | `org.freedesktop.DBus` and the bus object's Introspectable, Peer, Properties and Monitoring interfaces |
| `Notifications.hpp/.cpp` | `org.freedesktop.Notifications`, served by the kernel itself on the session bus: a Notify is a desktop toast |
| `SocketServer.hpp/.cpp` | The transport: one kernel thread (`dbus`) carries both buses' bytes and SCM_RIGHTS descriptors over `UnixSocketManager` |
| `Activation.hpp/.cpp` | Service files (`[D-BUS Service]`, `Name=`, `Exec=`), the Exec line split as a shell would, and `IServiceLauncher` |
| `ProgramActivator.hpp/.cpp` | The kernel's launcher: a service's program runs with `run_program()` on a kernel thread (`dbus-service`) with `DBUS_STARTER_ADDRESS` and `DBUS_STARTER_BUS_TYPE` |
| `MachineId.hpp/.cpp` | `/etc/machine-id` (made once, kept) and each bus's GUID (new at each start) |

`src/main.cpp` makes both buses after PipeWire, starts the Notifications service with a display that posts to `Desktop::toasts()`, and starts the
transport. The boot log says `D-Bus: system and session buses running`.

## What the bus does

The behaviour follows the D-Bus Specification (0.43) and, where the specification leaves room, dbus-daemon:

- **Authentication.** A NUL byte, then `AUTH EXTERNAL <hex uid>` (libdbus, GDBus) or `AUTH EXTERNAL` + empty `DATA` (sd-bus). The claimed uid must
  be the one the socket says (always 0 in the OS: there are no other users yet). `NEGOTIATE_UNIX_FD` is agreed to. Clients pipeline it all; the
  bytes after `BEGIN` are the first message.
- **Messages are checked before they are used.** Protocol version 1, a known type, a non-zero serial, each header field's type and value, the fields
  each type needs, alignment padding (zero), booleans (0 or 1), UTF-8 strings without NUL, object paths, signatures (≤ 255 bytes, ≤ 32 nested arrays and
  structs), array lengths (≤ 64 MiB and a whole number of elements), variants nested ≤ 64 deep, `h` indices below UNIX_FDS, and no bytes after the body.
  A connection that sends anything else is closed, as dbus-daemon closes it. Unknown header fields are dropped when a message is forwarded (the bus
  says so with the `HeaderFiltering` feature).
- **Hello** first (anything else closes the connection); unique names are `:1.N`. `NameOwnerChanged` and `NameAcquired` follow.
- **Names:** RequestName (ALLOW_REPLACEMENT, REPLACE_EXISTING, DO_NOT_QUEUE; replies 1-4), ReleaseName, ListQueuedOwners, NameHasOwner, GetNameOwner,
  ListNames, ListActivatableNames, StartServiceByName (see Activation). When an owner goes, the next in the queue takes over (`NameLost`,
  `NameOwnerChanged`, `NameAcquired`).
- **Routing.** A message with a destination goes to that name's primary owner, plus connections whose match rules eavesdrop and monitors. One without
  a destination goes to every connection whose rules match it, once each. A message to a name nobody has starts its service when a service file names it
  and the message allows auto-start; otherwise it gets `ServiceUnknown` (auto-start allowed) or `NameHasNoOwner`.
- **Replies** are delivered only to a call the bus delivered and that expects one. On the system bus an unrequested reply is dropped; the session bus lets
  it through, as dbus-daemon's session policy does. A callee that goes away without answering makes the bus send the caller `NoReply`, and so does a
  call that is not answered within the reply timeout (below).
- **Match rules:** `type`, `sender` (a well-known sender follows the name's owner), `interface`, `member`, `path`, `path_namespace`, `destination`,
  `arg0`..`arg63`, `arg0path`..`arg63path`, `arg0namespace`, `eavesdrop`.
- **Descriptors** (UNIX_FDS / SCM_RIGHTS) are passed on to recipients that negotiated them; to one that did not, the caller gets `NotSupported`. Each
  recipient gets a copy with a use of its own.
- **Monitoring.BecomeMonitor** (dbus-monitor): the connection loses its names and its unique name and sees everything its rules match (all, with none).
- **Credentials:** GetConnectionUnixUser, GetConnectionUnixProcessID, GetConnectionCredentials (`UnixUserID`, `ProcessID`). The kernel's own
  connections are uid 0 without a process id. GetAdtAuditSessionData and GetConnectionSELinuxSecurityContext answer that there is none.
- **Also:** GetId, Peer.Ping and Peer.GetMachineId, Introspect (the bus object at `/org/freedesktop/DBus` and its parents), Properties
  (`Features` = `HeaderFiltering`, `Interfaces` = `org.freedesktop.DBus.Monitoring`, both read only), ReloadConfig (nothing to reload),
  UpdateActivationEnvironment (session bus: the variables go to the services started from then on; `AccessDenied` on the system bus, like
  dbus-daemon; a name that is empty or has `=` is `InvalidArgs` and nothing of that call is set; at most 256 variables and 64 KiB).

### Activation

Services are started on demand from `.service` files, as dbus-daemon does it: the session bus reads `/usr/share/dbus-1/services` and
`/usr/local/share/dbus-1/services`, the system bus the `system-services` folders next to them (`filesystem::layout::kSessionServicesFolder` and
friends; the first folder that names a service wins). A file is

    [D-BUS Service]
    Name=org.example.Service
    Exec=/usr/bin/example-service --flag "an argument"

A message for a well-known name nobody owns, without the
NO_AUTO_START flag, and StartServiceByName start the program; the messages wait in the bus (at most 1024 in all, 64 services starting at once) and go
on, in order, once the program has taken the name. StartServiceByName answers 1 then, 2 for a name already owned. If the program ends first the
callers get `Spawn.ChildExited` ("Process X exited with status N"), if it cannot run `Spawn.ExecFailed`, if its Exec line is broken
`Spawn.FileInvalid`, and if it has not taken the name after 25 s (`set_activation_timeout_us`) `TimedOut`, and the program is stopped
(`IServiceLauncher::stop`; dbus-daemon kills it too, so a late starter does not hold the name with nobody waiting; the kernel logs "D-Bus: stopped the
service ..."). The program gets the usual environment, then UpdateActivationEnvironment's variables, then its unit's `Environment=` (below), then
`DBUS_STARTER_ADDRESS` and `DBUS_STARTER_BUS_TYPE` (libdbus' `DBUS_BUS_STARTER`, GDBus' `G_BUS_TYPE_STARTER`); a later variable of a name replaces an
earlier one. It runs from `/` of the Linux tree with no input, and prints to the debug console. The files are read at boot, on ReloadConfig, and again when a name is asked for that no file
names (at most once a second), so a newly installed service is found without a reload.

**SystemdService=, without systemd.** Distributions' service files often name a systemd unit and say `Exec=/bin/false`; dbus-daemon then asks
systemd to start the unit. There is no systemd here, so the bus runs the unit itself: it looks for the unit in `/etc/systemd/system` then
`/usr/lib/systemd/system` (session bus: the `user` folders; `layout::kSystemUnitsFolder` and friends), by its file name (a file system that follows
symbolic links finds the `dbus-org.*` links of `systemctl enable`) or else by an `Alias=` in the `[Install]` group of a unit there. Its `[Service]`
`ExecStart=` is the command (the first one; an empty assignment drops those before it; the `-`, `:`, `+`, `!`, `!!` and `@` prefixes are taken off,
`@`'s argv[0] word dropped; lines continue after a trailing `\`, `%%` is `%`) and its `Environment=` assignments are added. A unit that is not there, a
unit name with `/`, or an ExecStart that needs what is not expanded here (`$VARIABLES` without the `:` prefix, specifiers like `%i`, a path that is not
absolute) is passed over and Exec is run, as dbus-daemon does when it runs without systemd. The rest of the unit (`Type=`, sandboxing, `User=`,
dependencies, `NOTIFY_SOCKET`) is not used.

`User=` is handed to the launcher; the kernel has one user, so every service runs as it.

### Reply timeouts

A call that expects a reply and is not answered within 5 minutes (`Bus::kDefaultReplyTimeoutUs`, dbus-daemon's old default; `set_reply_timeout_us`,
0 = never) is answered by the bus with `org.freedesktop.DBus.Error.NoReply` ("Did not receive a reply..."), and the callee's late reply is then an
unrequested one (dropped on the system bus). Each call is kept with the time it was made, in one queue in that order: whatever the timeout, that is
the order of the deadlines, so a new timeout applies at once to the calls already waiting (a shorter one ends those past it at the next tick; calls
made while the timeout was off are timed from when they were made once it is on again). Expiring costs nothing per call that was answered in time;
stale entries are dropped when they outnumber the waiting calls. The clock is the transport's (`tick()`); a call made before the first tick is
timed from it.

### Limits

2048 connections per bus, 512 names, 4096 match rules and 4096 waiting calls per connection, 64 monitors, 253 descriptors per message (one sendmsg),
1024 descriptors received ahead of their messages, 128 MiB per message, 1024 service files. A connection that does not read has at most 64 MiB queued in the bus: after
that, calls to it get `LimitsExceeded`. The transport takes more output from a bus only while less than 256 KiB wait for a connection's socket.

## Services the kernel offers on the bus

A `dbus::LocalPeer` is a connection inside the kernel: `Bus::connect_local()` gives it a unique name, `request_name()` and `send()` work as for any client,
and messages for it are handed to `deliver()` on the bus's thread after the message that caused them has been routed. `tick()` gives it time.

- **org.freedesktop.Notifications** (session bus, `/org/freedesktop/Notifications`, Desktop Notifications Specification 1.2): Notify (with
  `replaces_id`; `expire_timeout` -1 = 8 s, 0 = until closed), CloseNotification, GetCapabilities (`body`), GetServerInformation, `NotificationClosed`
  (1 expired, 3 closed by a call). `notify-send` and libnotify show a desktop toast.

## Threads and locking

A `Bus` has no lock: only the transport's thread uses it (and the local peers, on that thread). The transport moves bytes and descriptors under the
system's lock (`ISystemSync`) and calls the buses with it let go; descriptors are released with it let go.

## Tests

- `tests/run_host_tests.sh DBusTest`: names, signatures, marshalling round trips in both byte orders, every truncation and every bit flip of a body and
  of a message, garbage, the handshake as each client library does it (and broken ones), match rules, the bus driven by in-memory clients (name queues,
  calls, replies, NoReply, signals, eavesdropping, monitors, descriptors, protocol errors, a message fed a byte at a time), the Notifications service,
  and the transport over a real `UnixSocketManager` with 8 client threads, an eventfd passed with SCM_RIGHTS, connections coming and going, and a reader that keeps up with a long signal stream (the activation and reply-timeout tests included).
- `tests/dbus_interop.sh`: `tests/dbus/DBusHostServer.cpp` serves both buses on the host's sockets, and the host's dbus-send, dbus-monitor, busctl
  (sd-bus), gdbus, notify-send and `tests/dbus/fdclient.c` (libdbus, a pipe passed with UNIX_FD) are pointed at them. A third bus starts
  `tests/dbus/echoservice.c` on demand (fork and exec with the starter variables) and has short timeouts: auto-start from libdbus, sd-bus and GDBus,
  StartServiceByName 1 then 2, ChildExited, ExecFailed, TimedOut (and the program killed), UpdateActivationEnvironment from dbus-send and gdbus seen
  by a service started later, a `SystemdService=` unit (found by `Alias=`, with its `Environment=`) run instead of `Exec=/bin/false`, and NoReply from
  the bus for a call nobody answers (43 checks).
- `tests/dbus_emulated.sh`: the host's real dbus-send, fdclient and gdbus, dynamically linked, run on the CPU emulator through the OS's Linux system calls
  against the kernel's buses (`EmulationRun -DTCOS_EMU_DBUS`); dbus-send also starts echoservice on demand, as a second emulated program
  (`TCOS_EMU_DBUS_SERVICES`).
- QEMU (by hand, recipe in the project notes): a kernel that autoruns `/usr/bin/dbus-send ... org.tcos.Echo.Echo`, with echoservice, its libraries and
  its service file on the primary disk: the kernel's launcher starts the service natively and the answer comes back. A second run autoruns a small
  libdbus client that sets an activation variable, starts a service through a user unit (the service sees the unit's variable, the bus's and the
  starter's), and waits on a service that never takes its name (TimedOut after 25 s, and the kernel stops the program).

## Not done

- Activation: units run without systemd's machinery (no `Type=notify`/`NOTIFY_SOCKET`, sandboxing, `EnvironmentFile=`, `WorkingDirectory=`,
  dependencies or restarts; no `$VARIABLE` or `%` specifier expansion); `User=` cannot be honoured (one user); no inotify (a changed file is found on
  a miss or ReloadConfig); a program the kernel stops ends like Ctrl+C would end it (no SIGTERM first).
- No policy beyond the defaults: everything may send to everything (there is one user); no `<policy>` configuration, no `send_destination` rules,
  no `AccessDenied` for ordinary calls on the system bus.
- No TCP or nonce-tcp transports, no `ANONYMOUS` or `DBUS_COOKIE_SHA1` mechanisms (Windows clients of D-Bus that need them cannot connect).
- No per-call reply timeouts (the message carries none; one timeout for the bus), no `Debug.Stats`, no `GetConnectionCredentials` `UnixGroupIDs`/`ProcessFD`.
- Notifications: no actions (`ActionInvoked`), icons, images, markup or sounds; hints are read past.
- Other freedesktop services (`hostname1`, `login1`, `timedate1`, portals) are not served.
