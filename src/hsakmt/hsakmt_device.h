/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HSAKMT_DEVICE_H
#define HSAKMT_DEVICE_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "virgl_util.h"

#define HSAKMT_BO_HANDLE void *
#define VIRGL_RENDERER_CAPSET_HSAKMT 8

#ifdef ENABLE_HSAKMT_AMDGPU

struct virgl_renderer_capset_hsakmt {
   uint32_t wire_format_version;
   uint32_t version_major;
   uint32_t version_minor;
   uint32_t version_patchlevel;
   uint32_t context_type;
   uint32_t pad;
};

int vhsakmt_device_init(void);

void vhsakmt_device_fini(void);

void vhsakmt_device_reset(void);

size_t vhsakmt_device_get_capset(UNUSED uint32_t set, UNUSED void *caps);

struct virgl_context *hsakmt_device_create(UNUSED size_t debug_len,
                                           UNUSED const char *debug_name);

#else

static inline size_t vhsakmt_device_get_capset(UNUSED uint32_t set, UNUSED void *caps)
{
   return 0;
}

static inline int vhsakmt_device_init(void) { return 0; }

static inline void vhsakmt_device_fini(void) {}

static inline void vhsakmt_device_reset(void) {}

static inline struct virgl_context *
hsakmt_device_create(UNUSED size_t debug_len, UNUSED const char *debug_name)
{
   return NULL;
}

#endif /* ENABLE_HSAKMT_AMDGPU */

#endif