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

/* Not generated: the ring interface the generated vn_submit_* / vn_call_* functions use (implemented in vk_venus.inc). A command is encoded into memory the
 * caller gives (vn_ring_submit_command_init), then written to the ring; when it wants a reply, the ring is told where to put it first
 * (vkSetReplyCommandStreamMESA) and the call waits until the host has consumed the command, then decodes the reply. The ring's lock is held from the
 * submission of a command with a reply until vn_ring_free_command_reply (the one reply stream is not reused meanwhile). */
#ifndef TCOS_VN_RING_H
#define TCOS_VN_RING_H

struct vn_ring;

struct vn_ring_submit_command {
  struct vn_cs_encoder command;
  size_t reply_size;
  struct vn_cs_decoder reply;
  bool has_reply;   /* the reply was received (the lock is held until vn_ring_free_command_reply) */
};

static struct vn_cs_encoder *vn_ring_submit_command_init(struct vn_ring *ring, struct vn_ring_submit_command *submit, void *cmd_data, size_t cmd_size, size_t reply_size);
static void vn_ring_submit_command(struct vn_ring *ring, struct vn_ring_submit_command *submit);
static struct vn_cs_decoder *vn_ring_get_command_reply(struct vn_ring *ring, struct vn_ring_submit_command *submit);
static void vn_ring_free_command_reply(struct vn_ring *ring, struct vn_ring_submit_command *submit);

#define VN_TRACE_FUNC()

#endif
