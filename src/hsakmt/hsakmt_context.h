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

#ifndef HSAKMT_CONTEXT_H_
#define HSAKMT_CONTEXT_H_

#include "libdrm/amdgpu.h"

#include "drm_context.h"
#include "hsakmt_device.h"
#include "hsakmt_virtio_proto.h"
#include "hsakmt_vm.h"

#ifdef ENABLE_HSAKMT_AMDGPU

/* reuse the drm context and obj but use hsakmt_base for further upgrade */
#define vhsakmt_base_context drm_context
#define vhsakmt_base_object drm_object
#define vhsakmt_ccmd drm_ccmd

struct vhsakmt_context {
   struct vhsakmt_base_context base;

   struct vhsakmt_shmem *shmem;

   const char *debug_name;
   uint32_t pid;

   amdgpu_device_handle dev;
   int debug;

   struct hash_table_u64 *id_to_ctx;

   hsakmt_vamgr_t vamgr;
   uint64_t scratch_base;
};
DEFINE_CAST(vhsakmt_base_context, vhsakmt_context)

typedef enum vhsakmt_object_type {
   VHSAKMT_OBJ_HOST_MEM,
   VHSAKMT_OBJ_USERPTR,
   VHSAKMT_OBJ_EVENT,
   VHSAKMT_OBJ_QUEUE,
   VHSAKMT_OBJ_DOORBELL_PTR,
   VHSAKMT_OBJ_DOORBELL_RW_PTR,
   VHSAKMT_OBJ_QUEUE_MEM,
   VHSAKMT_OBJ_DMA_BUF,
   VHSAKMT_OBJ_SCRATCH_MAP_MEM,
   VHSAKMT_OBJ_TYPE_MAX,
   VHSAKMT_OBJ_INVALID,
} vhsakmt_object_type_t;

static char *__obj_names[] = {
    "VHSAKMT_OBJ_HOST_MEM",
    "VHSAKMT_OBJ_USERPTR",
    "VHSAKMT_OBJ_EVENT",
    "VHSAKMT_OBJ_QUEUE",
    "VHSAKMT_OBJ_DOORBELL_PTR",
    "VHSAKMT_OBJ_DOORBELL_RW_PTR",
    "VHSAKMT_OBJ_QUEUE_MEM",
    "VHSAKMT_OBJ_DMA_BUF",
    "VHSAKMT_OBJ_SCRATCH_MAP_MEM",
    "VHSAKMT_OBJ_TYPE_MAX",
    "VHSAKMT_OBJ_INVALID",
};

static inline char *vhsakmt_object_type_name(vhsakmt_object_type_t t)
{
   if (t >= 0 && t <= VHSAKMT_OBJ_TYPE_MAX)
      return __obj_names[t];

   return __obj_names[VHSAKMT_OBJ_INVALID];
}

struct vhsakmt_object {
   struct vhsakmt_base_object base;

   HSAKMT_BO_HANDLE bo;

   uint32_t flags;
   bool exported : 1;
   bool exportable : 1;
   bool cpu_mapped : 1;
   bool guest_removed : 1;
   struct virgl_resource *res;

   uint64_t va;

   int fd;

   vHsaQueueResource *queue;
   struct vhsakmt_object *queue_obj;
   struct vhsakmt_object *queue_rw_mem;
   struct vhsakmt_object *queue_mem;

   unsigned vm_flags;
   vhsakmt_object_type_t type;
};
DEFINE_CAST(vhsakmt_base_object, vhsakmt_object)

bool vhsakmt_context_init(struct vhsakmt_context *ctx, int fd,
                          const struct vhsakmt_ccmd *ccmd_dispatch,
                          unsigned int dispatch_size);

void vhsakmt_context_deinit(struct vhsakmt_context *ctx);

void vhsakmt_context_fence_retre(struct virgl_context *vctx, uint32_t ring_idx,
                                 uint64_t fence_id);

void *vhsakmt_context_rsp(struct vhsakmt_context *ctx,
                          const struct vdrm_ccmd_req *hdr, size_t len);

int vhsakmt_context_get_shmem_blob(struct vhsakmt_context *ctx,
                                   const char *name, size_t shmem_size,
                                   uint64_t blob_size, uint32_t blob_flags,
                                   struct virgl_context_blob *blob);

bool vhsakmt_context_blob_id_valid(struct vhsakmt_context *ctx,
                                   uint32_t blob_id);

struct vhsakmt_object *
vhsakmt_context_retrieve_object_from_blob_id(struct vhsakmt_context *ctx,
                                             uint64_t blob_id);

void vhsakmt_context_object_set_blob_id(struct vhsakmt_context *ctx,
                                        struct vhsakmt_object *obj,
                                        uint32_t blob_id);

void vhsakmt_context_object_set_res_id(struct vhsakmt_context *ctx,
                                       struct vhsakmt_object *obj,
                                       uint32_t res_id);

struct vhsakmt_object *
vhsakmt_context_get_object_from_res_id(struct vhsakmt_context *ctx,
                                       uint32_t res_id);

bool vhsakmt_context_res_id_unused(struct vhsakmt_context *ctx,
                                   uint32_t res_id);

#endif /* ENABLE_HSAKMT_AMDGPU */

#endif /* HSAKMT_CONTEXT_H_ */
