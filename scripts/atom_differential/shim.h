#pragma once
#define __LITTLE_ENDIAN__ 1
#define LITTLEENDIAN 1
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
struct drm_device;
struct mutex { int x; };
#define mutex_init(m) ((void)0)
#define mutex_lock(m) ((void)0)
#define mutex_unlock(m) ((void)0)
#define GFP_KERNEL 0
#define kzalloc(n, f) calloc(1, n)
#define kcalloc(n, s, f) calloc(n, s)
#define kfree(p) free(p)
#define pr_info(...) ((void)0)
#define printk(...) ((void)0)
#define KERN_DEBUG ""
#define DRM_ERROR(...) ((void)0)
#define DRM_DEBUG(...) ((void)0)
#define DRM_INFO(...) ((void)0)
#define get_unaligned_le32(p) (*(p))
#define cpu_to_le32(x) (x)
#define lower_32_bits(n) ((uint32_t)(n))
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))
#define do_div(n, base) ({ uint32_t __rem = (n) % (base); (n) /= (base); __rem; })
extern unsigned long diff_jiffies;
#define jiffies (diff_jiffies += 1)
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define jiffies_to_msecs(j) ((j) * 10)
void diff_udelay(unsigned n); void diff_mdelay(unsigned n);
#define udelay(n) diff_udelay(n)
#define mdelay(n) diff_mdelay(n)
#define msleep(n) diff_mdelay(n)
#define drm_can_sleep() 1
#define ATOM_BIG_ENDIAN 0
#include <errno.h>
#define OFFSET_TO_GET_ATOMBIOS_NUMBER_OF_STRINGS 0x2f
#define OFFSET_TO_GET_ATOMBIOS_STRING_START 0x6e
#define OFFSET_TO_VBIOS_DATE 0x50
#define OFFSET_TO_VBIOS_PART_NUMBER 0x80
#define OFFSET_TO_ATOM_ROM_HEADER_POINTER 0x48
#define BIOS_ATOM_PREFIX "ATOMBIOS"
#define BIOS_VERSION_PREFIX "ATOMBIOSBK-AMD VER"
#define BIOS_STRING_LENGTH 43
