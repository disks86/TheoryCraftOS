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

/* Two libdbus connections to the bus at DBUS_SESSION_BUS_ADDRESS: one takes the name org.tcos.FdTest, the other calls it with the read end of a pipe
 * (UNIX_FD); the service reads what the caller wrote into the pipe and answers with it. Also: a call to a name the service queued for while another
 * owned it, and the NameOwnerChanged signal of the hand-over. Prints "fd: <text>" and "queue: ok"; exits 0 when everything worked. (tests/dbus_interop.sh) */
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DBusConnection* open_bus(const char* address) {
  DBusError err;
  dbus_error_init(&err);
  DBusConnection* c = dbus_connection_open_private(address, &err);
  if (!c || !dbus_bus_register(c, &err)) {
    fprintf(stderr, "connect: %s\n", err.message ? err.message : "?");
    exit(1);
  }
  return c;
}

/* Waits for a method call on `c` and returns it (popped). */
static DBusMessage* next_call(DBusConnection* c, const char* member) {
  for (int i = 0; i < 400; ++i) {
    dbus_connection_read_write(c, 10);
    DBusMessage* m;
    while ((m = dbus_connection_pop_message(c))) {
      if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL && strcmp(dbus_message_get_member(m), member) == 0) return m;
      dbus_message_unref(m);
    }
  }
  return NULL;
}

int main(void) {
  const char* address = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (!address) return 2;
  DBusConnection* service = open_bus(address);
  DBusConnection* caller = open_bus(address);
  DBusError err;
  dbus_error_init(&err);
  if (dbus_bus_request_name(service, "org.tcos.FdTest", DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    fprintf(stderr, "request name failed\n");
    return 1;
  }
  if (!dbus_connection_can_send_type(caller, DBUS_TYPE_UNIX_FD)) {
    fprintf(stderr, "descriptor passing was not negotiated\n");
    return 1;
  }
  int p[2];
  if (pipe(p) != 0) return 1;
  const char* text = "through-the-bus";
  if (write(p[1], text, strlen(text)) != (ssize_t)strlen(text)) return 1;
  close(p[1]);
  DBusMessage* call = dbus_message_new_method_call("org.tcos.FdTest", "/org/tcos/FdTest", "org.tcos.FdTest", "Take");
  dbus_message_append_args(call, DBUS_TYPE_UNIX_FD, &p[0], DBUS_TYPE_INVALID);
  close(p[0]);      /* (the message holds its own copy) */
  DBusPendingCall* pending = NULL;
  if (!dbus_connection_send_with_reply(caller, call, &pending, 5000) || !pending) return 1;
  dbus_connection_flush(caller);
  dbus_message_unref(call);

  DBusMessage* in = next_call(service, "Take");
  if (!in) { fprintf(stderr, "the call did not come\n"); return 1; }
  int fd = -1;
  if (!dbus_message_get_args(in, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) { fprintf(stderr, "args: %s\n", err.message); return 1; }
  char buf[64] = {0};
  const ssize_t got = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (got <= 0) return 1;
  DBusMessage* reply = dbus_message_new_method_return(in);
  const char* answer = buf;
  dbus_message_append_args(reply, DBUS_TYPE_STRING, &answer, DBUS_TYPE_INVALID);
  dbus_connection_send(service, reply, NULL);
  dbus_connection_flush(service);
  dbus_message_unref(reply);
  dbus_message_unref(in);

  dbus_pending_call_block(pending);
  DBusMessage* back = dbus_pending_call_steal_reply(pending);
  const char* echoed = NULL;
  if (!back || !dbus_message_get_args(back, &err, DBUS_TYPE_STRING, &echoed, DBUS_TYPE_INVALID)) { fprintf(stderr, "no reply\n"); return 1; }
  printf("fd: %s\n", echoed);
  fflush(stdout);
  dbus_message_unref(back);
  dbus_pending_call_unref(pending);

  /* the queue: the caller waits for the name; the service lets it go; the caller is told it is the owner now */
  dbus_bus_add_match(caller, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.tcos.FdTest'", &err);
  if (dbus_bus_request_name(caller, "org.tcos.FdTest", 0, &err) != DBUS_REQUEST_NAME_REPLY_IN_QUEUE) { fprintf(stderr, "not queued\n"); return 1; }
  if (dbus_bus_release_name(service, "org.tcos.FdTest", &err) != DBUS_RELEASE_NAME_REPLY_RELEASED) { fprintf(stderr, "not released\n"); return 1; }
  int changed = 0, acquired = 0;
  for (int i = 0; i < 300 && !(changed && acquired); ++i) {
    dbus_connection_read_write(caller, 10);
    DBusMessage* m;
    while ((m = dbus_connection_pop_message(caller))) {
      if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) changed = 1;
      if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameAcquired")) {
        const char* name = NULL;
        if (dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) && strcmp(name, "org.tcos.FdTest") == 0) acquired = 1;
      }
      dbus_message_unref(m);
    }
  }
  if (!changed || !acquired) { fprintf(stderr, "no hand-over (changed %d acquired %d)\n", changed, acquired); return 1; }
  printf("queue: ok\n");
  dbus_connection_close(service);
  dbus_connection_close(caller);
  dbus_connection_unref(service);
  dbus_connection_unref(caller);
  return 0;
}
