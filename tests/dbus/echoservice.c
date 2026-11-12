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

/* A D-Bus service to be started on demand (tests/dbus_interop.sh, tests/dbus_emulated.sh): it connects to the bus that started it
 * (DBUS_BUS_STARTER: DBUS_STARTER_ADDRESS), takes the name argv[1] (default org.tcos.Echo) and serves /org/tcos/Echo:
 *   Echo(s) -> s       the same text back
 *   Getenv(s) -> s     the value of an environment variable it was started with ("" if none: what the bus's activation environment gave it)
 *   Hang()             never answered (the bus's reply timeout answers instead)
 *   Quit()             answered, then the service ends
 * It ends by itself after argv[2] seconds without a call (default 10). */
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "org.tcos.Echo";
  const int idle = argc > 2 ? atoi(argv[2]) : 10;
  DBusError err;
  dbus_error_init(&err);
  DBusConnection* c = dbus_bus_get(DBUS_BUS_STARTER, &err);
  if (!c) { fprintf(stderr, "echoservice: %s\n", err.message); return 2; }
  if (dbus_bus_request_name(c, name, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { fprintf(stderr, "echoservice: no name\n"); return 3; }
  time_t last = time(NULL);
  for (;;) {
    dbus_connection_read_write(c, 100);
    DBusMessage* m;
    while ((m = dbus_connection_pop_message(c))) {
      if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        last = time(NULL);
        if (dbus_message_is_method_call(m, "org.tcos.Echo", "Echo")) {
          const char* text = "";
          dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
          DBusMessage* r = dbus_message_new_method_return(m);
          dbus_message_append_args(r, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
          dbus_connection_send(c, r, NULL);
          dbus_message_unref(r);
        } else if (dbus_message_is_method_call(m, "org.tcos.Echo", "Getenv")) {
          const char* variable = "";
          dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &variable, DBUS_TYPE_INVALID);
          const char* value = getenv(variable);
          if (!value) value = "";
          DBusMessage* r = dbus_message_new_method_return(m);
          dbus_message_append_args(r, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
          dbus_connection_send(c, r, NULL);
          dbus_message_unref(r);
        } else if (dbus_message_is_method_call(m, "org.tcos.Echo", "Quit")) {
          DBusMessage* r = dbus_message_new_method_return(m);
          dbus_connection_send(c, r, NULL);
          dbus_connection_flush(c);
          dbus_message_unref(r);
          dbus_message_unref(m);
          printf("echoservice: quit\n");
          return 0;
        }      /* (Hang: no answer) */
      }
      dbus_message_unref(m);
    }
    dbus_connection_flush(c);
    if (time(NULL) - last > idle) return 0;
  }
}
