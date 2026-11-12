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

/* What libEGL.so.1 (src/Linux/libegl) needs of libgbm.so.1 beyond the GBM API: telling a gbm_device from other native displays, and the buffers of a
 * gbm_surface that an EGL window surface draws into. The functions are exported by libgbm.so.1 under these names only (no program uses them). */
#ifndef TCOS_GBM_H
#define TCOS_GBM_H
#include <stdint.h>

struct gbm_device;
struct gbm_bo;
struct gbm_surface;

/* 1 when `native` is a gbm_device this library made and has not destroyed. */
int tcos_gbm_is_device(const void *native);
/* 1 when `native` is a live gbm_surface of this library; its size and DRM format. */
int tcos_gbm_surface_info(const void *native, uint32_t *width, uint32_t *height, uint32_t *format);
/* A buffer of the surface nobody holds (made when the surface has fewer than its limit), to draw the next frame into; null when all are held. */
struct gbm_bo *tcos_gbm_surface_take(struct gbm_surface *surface);
/* The frame drawn into `bo` (from tcos_gbm_surface_take) is ready: gbm_surface_lock_front_buffer gives it next. */
void tcos_gbm_surface_queue(struct gbm_surface *surface, struct gbm_bo *bo);
/* A buffer taken and not queued goes back (its frame failed). */
void tcos_gbm_surface_untake(struct gbm_surface *surface, struct gbm_bo *bo);

#endif
