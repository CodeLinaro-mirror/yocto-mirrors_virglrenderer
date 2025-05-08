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

#include "hsakmt_context.h"
#include "hsakmt_query.h"
#include "hsakmt_events.h"
#include "hsakmt_memory.h"
#include "hsakmt_queues.h"
#include "util/hsakmt_util.h"

bool vhsakmt_context_init(struct vhsakmt_context *ctx, int fd,
                          const struct vhsakmt_ccmd *ccmd_dispatch,
                          unsigned int dispatch_size)
{
   return drm_context_init(&ctx->base, fd, ccmd_dispatch, dispatch_size);
}

void vhsakmt_context_deinit(struct vhsakmt_context *ctx)
{
   drm_context_deinit(&ctx->base);
}

void vhsakmt_context_fence_retre(struct virgl_context *vctx, uint32_t ring_idx,
                                 uint64_t fence_id)
{
   drm_context_fence_retire(vctx, ring_idx, fence_id);
}

void *vhsakmt_context_rsp(struct vhsakmt_context *ctx,
                          const struct vdrm_ccmd_req *hdr, size_t len)
{
   return drm_context_rsp(&ctx->base, hdr, len);
}

int vhsakmt_context_get_shmem_blob(struct vhsakmt_context *ctx,
                                   const char *name, size_t shmem_size,
                                   uint64_t blob_size, uint32_t blob_flags,
                                   struct virgl_context_blob *blob)
{
   return drm_context_get_shmem_blob(&ctx->base, name, shmem_size, blob_size,
                                     blob_flags, blob);
}

bool vhsakmt_context_blob_id_valid(struct vhsakmt_context *ctx,
                                   uint32_t blob_id)
{
   return drm_context_blob_id_valid(&ctx->base, blob_id);
}

struct vhsakmt_object *
vhsakmt_context_retrieve_object_from_blob_id(struct vhsakmt_context *ctx,
                                             uint64_t blob_id)
{
   struct drm_object *obj =
       drm_context_retrieve_object_from_blob_id(&ctx->base, blob_id);
   if (!obj)
      return NULL;

   return to_vhsakmt_object(obj);
}

void vhsakmt_context_object_set_blob_id(struct vhsakmt_context *ctx,
                                        struct vhsakmt_object *obj,
                                        uint32_t blob_id)
{
   drm_context_object_set_blob_id(&ctx->base, &obj->base, blob_id);
}

void vhsakmt_context_object_set_res_id(struct vhsakmt_context *ctx,
                                       struct vhsakmt_object *obj,
                                       uint32_t res_id)
{
   drm_context_object_set_res_id(&ctx->base, &obj->base, res_id);
}

struct vhsakmt_object *
vhsakmt_context_get_object_from_res_id(struct vhsakmt_context *ctx,
                                       uint32_t res_id)
{
   struct drm_object *obj =
       drm_context_get_object_from_res_id(&ctx->base, res_id);
   if (!obj)
      return NULL;

   return to_vhsakmt_object(obj);
}

bool
vhsakmt_context_res_id_unused(struct vhsakmt_context *ctx, uint32_t res_id)
{
   return drm_context_res_id_unused(&ctx->base, res_id);
}

struct vhsakmt_object *
vhsakmt_context_object_create(HSAKMT_BO_HANDLE handle, uint32_t flags, uint32_t size,
                      vhsakmt_object_type_t type)
{
   struct vhsakmt_object *obj = calloc(1, sizeof(*obj));
   if (!obj)
      return NULL;

   obj->bo = handle;
   obj->flags = flags;
   obj->base.size = size;
   obj->type = type;

   return obj;
}

static void
vhsakmt_context_remove_object(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (vhsakmt_context_res_id_unused(ctx, obj->base.res_id))
      return;

   _mesa_hash_table_remove_key(ctx->base.resource_table, (void *)(uintptr_t)obj->base.res_id);
}

void
vhsakmt_context_free_object(struct vhsakmt_base_context *bctx, struct vhsakmt_base_object *bobj)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_object *obj = to_vhsakmt_object(bobj);
   int ret = 0;

   switch (obj->type) {
   case VHSAKMT_OBJ_USERPTR:
      ret = vhsakmt_free_userptr(obj);
      break;

   case VHSAKMT_OBJ_HOST_MEM:
      ret = vhsakmt_free_host_mem(ctx, obj);
      break;

   case VHSAKMT_OBJ_DOORBELL_RW_PTR:
   case VHSAKMT_OBJ_DOORBELL_PTR:
      /* Do nothing */
      break;

   case VHSAKMT_OBJ_EVENT:
      vhsakmt_free_event_obj(ctx, obj);
      break;

   case VHSAKMT_OBJ_QUEUE:
      vhsakmt_free_queue_obj(ctx, obj);
      break;

   case VHSAKMT_OBJ_SCRATCH_MAP_MEM:
      ret = vhsakmt_free_scratch_map_mem(ctx, obj);
      break;

   case VHSAKMT_OBJ_DMA_BUF:
      vhsakmt_free_dmabuf_obj(ctx, obj);
      break;

   case VHSAKMT_OBJ_QUEUE_MEM:
      ret = vhsakmt_free_host_mem(ctx, obj);
      break;

   default:
      vhsa_err("Unknown object type: %d, not handled", obj->type);
      break;
   }

   if (ret) {
      vhsa_dbg("Failed to free object: %p, type: %s, res_id: %d, address: %p",
               (void *)obj, vhsakmt_object_type_name(obj->type), obj->base.res_id,
               obj->bo);
      return;
   }

   vhsa_dbg("Free obj: %p, type: %s, res_id: %d, address: %p", (void *)obj,
            vhsakmt_object_type_name(obj->type), obj->base.res_id, obj->bo);
   vhsakmt_context_remove_object(ctx, obj);
   free(obj);
}
