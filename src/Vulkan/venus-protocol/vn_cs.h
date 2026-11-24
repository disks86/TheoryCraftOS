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

/* Not generated: the command stream encoder and decoder the generated venus-protocol headers write and read with (what Mesa's vn_cs.h is for its venus
 * driver). An encoder is a byte range being filled, growing on the heap when it is the growable kind (a command buffer's stream); every write is checked
 * against the range, and one that does not fit makes the encoder fatal (nothing more is written; the command is not sent). A decoder reads a reply: a read
 * past its end makes it fatal and reads as zeros. Handles are encoded as the ids the host knows them by (vn_load_id / vn_store_id in vk_venus.inc). */
#ifndef TCOS_VN_CS_H
#define TCOS_VN_CS_H

struct vn_cs_encoder {
  uint8_t *base, *cur, *end;
  bool growable;   /* (base is the encoder's own heap memory) */
  bool fatal;
};

struct vn_cs_decoder {
  const uint8_t *cur, *end;
  bool fatal;
};

static bool vn_cs_encoder_grow(struct vn_cs_encoder *enc, size_t size);
static uint64_t vn_load_id(const void *handle, VkObjectType type);
static void vn_store_id(void *handle, uint64_t id, VkObjectType type);
static bool vn_cs_renderer_protocol_has_api_version(uint32_t api_version);
static bool vn_cs_renderer_protocol_has_extension(uint32_t number);

static inline size_t vn_cs_encoder_get_len(const struct vn_cs_encoder *enc) { return (size_t)(enc->cur - enc->base); }

static inline bool vn_cs_encoder_reserve(struct vn_cs_encoder *enc, size_t size) {
  if (enc->fatal) return false;
  if (size <= (size_t)(enc->end - enc->cur)) return true;
  if (enc->growable && vn_cs_encoder_grow(enc, size)) return true;
  enc->fatal = true;
  return false;
}

static inline void vn_cs_encoder_write(struct vn_cs_encoder *enc, size_t size, const void *val, size_t val_size) {
  if (enc->fatal || val_size > size || size > (size_t)(enc->end - enc->cur)) {
    enc->fatal = true;
    return;
  }
  memcpy(enc->cur, val, val_size);
  if (size > val_size) memset(enc->cur + val_size, 0, size - val_size);   /* (padding goes out as zeros, not as whatever the memory held) */
  enc->cur += size;
}

static inline void vn_cs_decoder_set_fatal(const struct vn_cs_decoder *dec) { ((struct vn_cs_decoder *)dec)->fatal = true; }

static inline void vn_cs_decoder_read(struct vn_cs_decoder *dec, size_t size, void *val, size_t val_size) {
  if (dec->fatal || val_size > size || size > (size_t)(dec->end - dec->cur)) {
    dec->fatal = true;
    dec->cur = dec->end;
    memset(val, 0, val_size);
    return;
  }
  memcpy(val, dec->cur, val_size);
  dec->cur += size;
}

static inline void vn_cs_decoder_peek(const struct vn_cs_decoder *dec, size_t size, void *val, size_t val_size) {
  if (dec->fatal || val_size > size || size > (size_t)(dec->end - dec->cur)) {
    memset(val, 0, val_size);
    return;
  }
  memcpy(val, dec->cur, val_size);
}

static inline uint64_t vn_cs_handle_load_id(const void **handle, VkObjectType type) { return vn_load_id(handle, type); }
static inline void vn_cs_handle_store_id(void **handle, uint64_t id, VkObjectType type) { vn_store_id(handle, id, type); }

#endif
