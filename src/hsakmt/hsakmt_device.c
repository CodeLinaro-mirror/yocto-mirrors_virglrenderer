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

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pipe/p_state.h"
#include "util/anon_file.h"
#include "util/bitscan.h"
#include "util/hash_table.h"
#include "util/macros.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_thread.h"
#include "virgl_context.h"
#include "virglrenderer.h"

#include "drm/drm_util.h"

#include "hsakmt_virtio_proto.h"
#include "hsakmt_context.h"
#include "hsakmt_device.h"
#include "hsakmt_vm.h"
#include "util/hsakmt_util.h"
#include <hsakmt/hsakmt.h>

#include "hsakmt_query.h"
#include "hsakmt_events.h"
#include "hsakmt_memory.h"

static struct vhsakmt_backend backend = {
    .context_type = VIRTGPU_HSAKMT_OCL,
    .name = "amdgpu-hsakmt",
    .vamgr_vm_base_addr_type = VHSA_VAMGR_VM_TYPE_FIXED_BASE,
    .vamgr_vm_fixed_base_addr = VHSA_FIXED_VM_BASE_ADDR,
    .vamgr_vm_heap_interval_size = VHSA_HEAP_INTERVAL_SIZE,
    .vamgr_vm_kfd_size = VHSA_DEV_RESERVE_SIZE,
    .vamgr_vm_scratch_size = VHSA_DEV_SCRATCH_RESERVE_SIZE,
    .vamgr_vm_context_size = VHSA_CTX_RESERVE_SIZE,
    .vhsakmt_open_count = 0,
    .vhsakmt_num_nodes = 0,
    .vhsakmt_nodes = NULL,
    .hsakmt_mutex = PTHREAD_MUTEX_INITIALIZER,
};

inline struct vhsakmt_backend *vhsakmt_backend(void) { return &backend; }

static inline uint64_t vhsakmt_queue_page_size() { return getpagesize(); }

static inline bool
vhsakmt_is_gpu_node(struct vhsakmt_node *n)
{
   return n->node_props.KFDGpuID != 0;
}

struct vhsakmt_node *
vhsakmt_get_node(struct vhsakmt_backend *b, uint32_t node_id)
{
   if (!b->vhsakmt_num_nodes || node_id >= b->vhsakmt_num_nodes)
      return NULL;

   return &b->vhsakmt_nodes[node_id];
}

static inline uint32_t
hsakmt_get_gfx_version_full(HSA_ENGINE_ID id)
{
   return (id.ui32.Major << 16) | (id.ui32.Minor << 8) | id.ui32.Stepping;
}

/* From libhsakmt src/queue.c */
static inline uint64_t
vhsakmt_doorbell_page_size(uint32_t gfxv)
{
   return (gfxv > 0x90000) ? (8 * 1024) : (4 * 1024);
}

struct vhsakmt_object *
vhsakmt_object_create(HSAKMT_BO_HANDLE handle, uint32_t flags, uint32_t size,
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

static int
vhsakmt_gpu_unmap(struct vhsakmt_object *obj)
{
   return hsaKmtUnmapMemoryToGPU(obj->bo);
}

static int
vhsakmt_free_userptr(UNUSED struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_USERPTR)
      return -EINVAL;

   /* userptr do not need unmap and deregister, cause they are totally managed
    * by kernel */
   return 0;
}

/* there are two kinds of scratch memory
   the scratch reserve area memory, it is a virtual memory area, not real
   memory, the type is VHSAKMT_OBJ_HOST_MEM, will be free vamgr area in
   vhsakmt_free_host_mem. the second type scratch memory is real alloc memory,
   they are alloced in map to GPU, and need be free in here, otherwise map same
   scratch address will return error.
*/
static int
vhsakmt_free_scratch_map_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_SCRATCH_MAP_MEM)
      return -EINVAL;

   vhsa_log("free scratch memory: %p, size: %lx", obj->bo, obj->base.size);

   return vhsakmt_gpu_unmap(obj);
}

static bool
vhsakmt_queue_mem_obj_can_remove(struct vhsakmt_object *obj)
{
   if (obj->type != VHSAKMT_OBJ_QUEUE_MEM)
      return false;

   return (obj->queue_obj == NULL) && obj->guest_removed;
}

static int
vhsakmt_free_scratch_reserve_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
    uint32_t i;

    for (i = 0; i < vhsakmt_backend()->vhsakmt_num_nodes; i++)
    {
        struct vhsakmt_node *node = &vhsakmt_backend()->vhsakmt_nodes[i];
        if (obj->bo >= (void*)node->scratch_vamgr.vm_va_base_addr && obj->bo < (void*)node->scratch_vamgr.vm_va_high_addr)
        {
            vhsa_log("free scratch reserve memory in node[%d]: %p, size: %lx", i, obj->bo, obj->base.size);
            hsakmt_free_from_vamgr(&node->scratch_vamgr, (uint64_t)obj->bo);
            break;
        }
    }

    return 0;
}

static inline bool
vhsakmt_is_scratch_obj(struct vhsakmt_object *obj)
{
   if (!obj)
      return false;

   return ((HsaMemFlags *)&obj->flags)->ui32.Scratch;
}

static int
vhsakmt_free_host_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || (obj->type != VHSAKMT_OBJ_HOST_MEM && obj->type != VHSAKMT_OBJ_QUEUE_MEM))
      return -EINVAL;

   if (obj->type == VHSAKMT_OBJ_QUEUE_MEM) {
      if (!vhsakmt_queue_mem_obj_can_remove(obj)) {
         vhsa_dbg("queue mem obj remove skipped, res_id: %d, addr: %p", obj->base.res_id, obj->bo);
         return -EINVAL;
      }
   }

   /* For shmem */
   if (obj->fd && obj->base.blob_id == 0) {
      if (obj->bo) {
        munmap(obj->bo, obj->base.size);
        obj->bo = NULL;
        obj->base.size = 0;
      }

      close(obj->fd);
      obj->fd = -1;
      return 0;
   }

   if (vhsakmt_is_scratch_obj(obj))
      vhsakmt_free_scratch_reserve_mem(ctx, obj);
   else {
      vhsakmt_gpu_unmap(obj);
      hsaKmtFreeMemory(obj->bo, obj->base.size);

#ifdef HSAKMT_VIRTIO
      if (vhsakmt_reserve_va((uint64_t)obj->bo, obj->base.size))
         vhsa_err("Reserve address: %p size: 0x%x failed when free.", obj->bo,
                  obj->base.size);
#endif

      if (hsakmt_free_from_vamgr(&ctx->vamgr, (uint64_t)obj->bo)) {
         vhsa_err("Failed to free memory form vamgr, address: %p", obj->bo);
         return -EINVAL;
      }
   }

   return 0;
}

static void
vhsakmt_free_event_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_EVENT)
      return;

   hsaKmtSetEvent(obj->bo);
   hsaKmtDestroyEvent(obj->bo);
}

static void
vhsakmt_free_queue_obj(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   hsaKmtDestroyQueue(obj->queue->r.QueueId);

   if (obj->queue_rw_mem) {
      obj->queue_rw_mem->queue_obj = NULL;
      vhsakmt_free_object(&ctx->base, &obj->queue_rw_mem->base);
   }

   if (obj->queue_mem) {
      obj->queue_mem->queue_obj = NULL;
      vhsakmt_free_object(&ctx->base, &obj->queue_mem->base);
   }

   free(obj->queue);
   obj->queue = NULL;
}

static void
vhsakmt_free_dmabuf_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_DMA_BUF)
      return;

   if (obj->fd == -1)
      return;

   close(obj->fd);
   obj->fd = -1;
}

static void vhsakmt_context_remove_object(struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (vhsakmt_context_res_id_unused(ctx, obj->base.res_id))
      return;

   _mesa_hash_table_remove_key(ctx->base.resource_table, (void *)(uintptr_t)obj->base.res_id);
}

void
vhsakmt_free_object(struct vhsakmt_base_context *bctx, struct vhsakmt_base_object *bobj)
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

static void
vhsakmt_device_detach_resource(struct virgl_context *vctx,
                               struct virgl_resource *res)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj = vhsakmt_context_get_object_from_res_id(ctx, res->res_id);

   if (!obj)
      return;

   if (obj->type == VHSAKMT_OBJ_QUEUE_MEM)
      obj->guest_removed = true;

   vhsakmt_free_object(&ctx->base, &obj->base);
}

static void
vhsakmt_device_destroy(struct virgl_context *vctx)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   uint32_t i;

   vhsakmt_context_deinit(ctx);

   hsakmt_free_from_vamgr(&vhsakmt_backend()->vamgr, ctx->vamgr.vm_va_base_addr);

   for (i = 0; i < vhsakmt_backend()->vhsakmt_num_nodes; i++)
   {
      if (vhsakmt_is_gpu_node(&vhsakmt_backend()->vhsakmt_nodes[i]))
         hsakmt_free_from_vamgr(&vhsakmt_backend()->vhsakmt_nodes[i].scratch_vamgr,
            (uint64_t)vhsakmt_backend()->vhsakmt_nodes[i].scratch_base);
   }

   free((void *)ctx->debug_name);
   free(ctx);
}

static void
vhsakmt_device_attach_resource(struct virgl_context *vctx, struct virgl_resource *res)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   struct vhsakmt_object *obj = vhsakmt_context_get_object_from_res_id(ctx, res->res_id);
   int fd;

   if (!obj) {
      enum virgl_resource_fd_type fd_type = res->fd_type;
      if (fd_type == VIRGL_RESOURCE_OPAQUE_HANDLE) {
         /* need export cause it is a OPAQUE_HANDLE resource */
         if (res->fd == -1) {
            fd_type = virgl_resource_export_fd(res, &fd);
            if (fd_type == VIRGL_RESOURCE_FD_INVALID || fd == -1) {
               vhsa_err("Failed to export fd for res_id: %u", res->res_id);
               return;
            }
         }
         obj = vhsakmt_object_create(res->mapped,
                                     VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE,
                                     res->map_size, VHSAKMT_OBJ_DMA_BUF);
         if (!obj)
            return;

         obj->fd = fd;
         vhsakmt_context_object_set_res_id(ctx, obj, res->res_id);
         vhsa_log("Attach resource: res id: %u fd: %d", obj->base.res_id, obj->fd);
      } else if (fd_type == VIRGL_RESOURCE_FD_INVALID)
         return;
      else
         return;
   }

   obj->res = res;
}

static int
vhsakmt_device_get_blob(struct virgl_context *vctx, uint32_t res_id, uint64_t blob_id,
                        uint64_t blob_size, uint32_t blob_flags,
                        struct virgl_context_blob *blob)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));

   vhsa_dbg("blob_id=%" PRIu64 ", res_id=%u, blob_size=%" PRIu64
            ", blob_flags=0x%x",
            blob_id, res_id, blob_size, blob_flags);

   if ((blob_id >> 32) != 0) {
      vhsa_err("Invalid blob_id: %" PRIu64, blob_id);
      return -EINVAL;
   }

   /* blob_id of zero is reserved for the shmem buffer: */
   if (blob_id == 0) {
      int ret = vhsakmt_context_get_shmem_blob(ctx, "vhsakmt-shmem",
                                               sizeof(*ctx->shmem), blob_size,
                                               blob_flags, blob);
      if (ret)
         return ret;

      ctx->shmem = (struct vhsakmt_shmem *)ctx->base.shmem;

      return 0;
   }

   if (!vhsakmt_context_res_id_unused(ctx, res_id)) {
      vhsa_err("Invalid res_id %u", res_id);
      return -EINVAL;
   }

   struct vhsakmt_object *obj =
       vhsakmt_context_retrieve_object_from_blob_id(ctx, blob_id);

   if (!obj && (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_USERPTR)) {
      if (blob->u.va_handle) {
         /* for userptr blob memory */
         obj = vhsakmt_object_create(blob->u.va_handle, 0,
                                     blob_size / getpagesize(),
                                     VHSAKMT_OBJ_USERPTR);
         vhsa_log("Create userptr address: %p size: 0x%lx", blob->u.va_handle, blob_size);
      } else {
         vhsa_err("No object with blob_id=%ld", blob_id);
         return -ENOENT;
      }
   }

   if (obj->exported) {
      vhsa_err("Already exported! blob_id:%ld", blob_id);
      return -EINVAL;
   }

   vhsakmt_context_object_set_res_id(ctx, obj, res_id);

   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE) {
      vhsa_err("Invalid blob_flags: 0x%x", blob_flags);
      return -EINVAL;
   } else {
      blob->type = VIRGL_RESOURCE_VA_HANDLE;
      blob->u.va_handle = obj->bo;
   }

   obj->exported = true;
   obj->exportable = !!(blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE);

   return 0;
}

inline bool
vhsakmt_check_va_valid(UNUSED struct vhsakmt_context *ctx, UNUSED uint64_t value)
{
#ifdef VHSA_CHECK_VA_ENABLE
   if (!ctx->vamgr.vm_va_base_addr || !ctx->vamgr.vm_va_high_addr)
      return false;

   if (value >= ctx->vamgr.vm_va_base_addr &&
       value < ctx->vamgr.vm_va_high_addr)
      return true;

   return false;
#else
   return true;
#endif
}

static int
vhsakmt_ccmd_nop(UNUSED struct vhsakmt_base_context *bctx,
                 UNUSED struct vhsakmt_ccmd_req *hdr)
{
   return 0;
}

static void
vhsakmt_queue_init_node_doorbell(struct vhsakmt_node *node, uint64_t doorbell_base_addr)
{
   pthread_mutex_lock(&vhsakmt_backend()->hsakmt_mutex);
   if (node->doorbell_base_addr)
      goto out_unlock;

   node->doorbell_base_addr = (void *)ROUND_DOWN_TO(doorbell_base_addr, getpagesize());

out_unlock:
   pthread_mutex_unlock(&vhsakmt_backend()->hsakmt_mutex);
   return;
}

static int
vhsakmt_queue_create_doorbell_blob(struct vhsakmt_context *ctx, struct vhsakmt_node *node,
                                   uint64_t doorbell_blob_id)
{
   if (!node->doorbell_base_addr) {
      vhsa_err("Invalid doorbell base address");
      return -EINVAL;
   }

   struct vhsakmt_object *obj = vhsakmt_object_create(
       (void *)node->doorbell_base_addr, 0,
       vhsakmt_doorbell_page_size(hsakmt_get_gfx_version_full(node->node_props.EngineId)),
       VHSAKMT_OBJ_DOORBELL_PTR);
   if (!obj)
      return -ENOMEM;
   vhsakmt_context_object_set_blob_id(ctx, obj, doorbell_blob_id);

   return 0;
}

static int
vhsakmt_queue_create_rw_ptr_blob(struct vhsakmt_context *ctx,
                                 vHsaQueueResource *vqueue_res, uint64_t rw_ptr_blob_id)
{
   vqueue_res->host_rw_handle =
       ROUND_DOWN_TO(vqueue_res->r.QueueWptrValue, getpagesize());
   vqueue_res->host_write_offset =
       vqueue_res->r.QueueWptrValue - vqueue_res->host_rw_handle;
   vqueue_res->host_read_offset =
       vqueue_res->r.QueueRptrValue - vqueue_res->host_rw_handle;

   struct vhsakmt_object *obj = vhsakmt_object_create(
       (void *)vqueue_res->host_rw_handle, 0, getpagesize(), VHSAKMT_OBJ_DOORBELL_RW_PTR);
   if (!obj)
      return -ENOMEM;
   vhsakmt_context_object_set_blob_id(ctx, obj, rw_ptr_blob_id);

   return 0;
}

static inline bool
vhsakmt_valid_sdmaid(uint32_t sdmaid)
{
   return !(sdmaid == UINT32_MAX);
}

static int
vhsakmt_queue_mem_convert(struct vhsakmt_context *ctx, uint32_t res_id, struct vhsakmt_object *queue_obj,
                                     bool is_rw_mem)
{
   int ret = 0;
   struct vhsakmt_object *mem_obj = NULL;

   if (!queue_obj) {
      vhsa_err("Invalid queue object");
      return -EINVAL;
   }

   mem_obj = vhsakmt_context_get_object_from_res_id(ctx, res_id);
   if (!mem_obj) {
      vhsa_err("Can not find queue memory bo, res_id: %d", res_id);
      return -EINVAL;
   }

   mem_obj->type = VHSAKMT_OBJ_QUEUE_MEM;
   mem_obj->queue_obj = queue_obj;
   if (is_rw_mem)
      queue_obj->queue_rw_mem = mem_obj;
   else
      queue_obj->queue_mem = mem_obj;
   vhsa_log("Change res: %d, address: %p, size: %lx to %s", res_id, mem_obj->bo, mem_obj->base.size,
            is_rw_mem ? "rw mem" : "queue mem");

   return ret;
}

static int
vhsakmt_queue_create(struct vhsakmt_context *ctx, struct vhsakmt_ccmd_queue_req *req,
                     vHsaQueueResource **p_vqueue_res)
{
   int ret = 0;
   struct vhsakmt_object *queue_obj;
   vHsaQueueResource *vqueue_res;
   struct vhsakmt_node *node =
      vhsakmt_get_node(vhsakmt_backend(), req->create_queue_args.NodeId);
   if (!node) {
      vhsa_err("Invalid node %d", req->create_queue_args.NodeId);
      return HSAKMT_STATUS_INVALID_NODE_UNIT;
   }

   vqueue_res = calloc(1, sizeof(vHsaQueueResource));
   if (!vqueue_res) {
      vhsa_err("vhsakmt_ccmd_queue calloc failed");
      return -ENOMEM;
   }

   vqueue_res->queue_handle = (uint64_t)vqueue_res;
   vqueue_res->r.Queue_write_ptr_aql = req->create_queue_args.Queue_write_ptr_aql;
   vqueue_res->r.Queue_read_ptr_aql = req->create_queue_args.Queue_read_ptr_aql;

   if (vhsakmt_valid_sdmaid(req->create_queue_args.SdmaEngineId))
      ret = hsaKmtCreateQueueExt(
         req->create_queue_args.NodeId, req->create_queue_args.Type,
         req->create_queue_args.QueuePercentage, req->create_queue_args.Priority,
         req->create_queue_args.SdmaEngineId,
         (void *)req->create_queue_args.QueueAddress,
         req->create_queue_args.QueueSizeInBytes, req->create_queue_args.Event,
         &(vqueue_res->r));
   else
      ret = hsaKmtCreateQueue(
         req->create_queue_args.NodeId, req->create_queue_args.Type,
         req->create_queue_args.QueuePercentage, req->create_queue_args.Priority,
         (void *)req->create_queue_args.QueueAddress,
         req->create_queue_args.QueueSizeInBytes, req->create_queue_args.Event,
         &(vqueue_res->r));

   vhsa_log("Create queue NodeId: %d Type: %d QueuePercentage: %d Priority: %d SdmaEngineId: %u "
            "QueueAddress : 0x%lx QueueSizeInBytes: %lx Event: %p QueueResource: %p "
            "QueueId: %lx QueueId_ptr: %p ret = %d ",
            req->create_queue_args.NodeId, req->create_queue_args.Type,
            req->create_queue_args.QueuePercentage, (int)req->create_queue_args.Priority,
            req->create_queue_args.SdmaEngineId,
            req->create_queue_args.QueueAddress, req->create_queue_args.QueueSizeInBytes,
            (void *)req->create_queue_args.Event, (void *)vqueue_res,
            vqueue_res->r.QueueId, (void *)vqueue_res->r.QueueId, ret);

   vhsa_log("Queue doorbell: %p, write ptr: %p read ptr: %p",
            (void *)vqueue_res->r.Queue_DoorBell, (void *)vqueue_res->r.QueueWptrValue,
            (void *)vqueue_res->r.QueueRptrValue);

   if (ret) {
      vhsa_err("Create queue call hsaKmtCreateQueue failed, ret: %d", ret);
      goto out_free;
   }

   if (vqueue_res->r.Queue_DoorBell == NULL) {
      vhsa_err("Queue_DoorBell is NULL");
      goto out_destroy_queue;
   }

   if (!node->doorbell_base_addr) {
      vhsakmt_queue_init_node_doorbell(node, (uint64_t)vqueue_res->r.QueueDoorBell);
      vhsa_log("Queue init node: %d doorbell base: %p", req->create_queue_args.NodeId,
               node->doorbell_base_addr);
   }

   vqueue_res->host_doorbell = (HSAuint64)vqueue_res->r.Queue_DoorBell_aql;
   vqueue_res->host_doorbell_offset =
       vqueue_res->r.QueueDoorBell - (uint64_t)node->doorbell_base_addr;

   /* For per context doorbell first mapping */
   if (req->doorbell_blob_id) {
      ret = vhsakmt_queue_create_doorbell_blob(ctx, node, req->doorbell_blob_id);
      if (ret) {
         vhsa_err("Create doorbell blob failed, ret: %d", ret);
         goto out_destroy_queue;
      }
   }

   /* For not AQL queue r/w ptr mapping */
   if (req->rw_ptr_blob_id && req->create_queue_args.Type != HSA_QUEUE_COMPUTE_AQL) {
      ret = vhsakmt_queue_create_rw_ptr_blob(ctx, vqueue_res, req->rw_ptr_blob_id);
      if(ret) {
         vhsa_err("Create rw ptr blob failed, ret: %d", ret);
         goto out_destroy_queue;
      }
   }

   queue_obj = vhsakmt_object_create((void *)vqueue_res, 0, sizeof(*vqueue_res), VHSAKMT_OBJ_QUEUE);
   if (!queue_obj) {
      vhsa_err("Create queue object failed");
      ret = -ENOMEM;
      goto out_destroy_queue;
   }

   queue_obj->queue = vqueue_res;
   vhsakmt_context_object_set_blob_id(ctx, queue_obj, req->blob_id);

   if (req->create_queue_args.Type == HSA_QUEUE_COMPUTE_AQL && req->res_id) {
      ret = vhsakmt_queue_mem_convert(ctx, req->res_id, queue_obj, true);
      if (ret) 
         goto out_destroy_queue;
   }

   if (req->queue_mem_res_id) {
      ret = vhsakmt_queue_mem_convert(ctx, req->queue_mem_res_id, queue_obj, false);
      if (ret)
         goto out_destroy_queue;
   }

   *p_vqueue_res = vqueue_res;
   return 0;

out_destroy_queue:
   hsaKmtDestroyQueue(vqueue_res->r.QueueId);
out_free:
   free(vqueue_res);
   return ret;
}

static int
vhsakmt_ccmd_queue(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   struct vhsakmt_ccmd_queue_req *req = to_vhsakmt_ccmd_queue_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_queue_rsp *rsp;
   unsigned rsp_len = sizeof(*rsp);

   switch (req->type) {
   case VHSAKMT_CCMD_QUEUE_CREATE: {
      rsp_len = size_add(sizeof(vHsaQueueResource), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      vHsaQueueResource *vqueue_res = NULL;

      rsp->ret = vhsakmt_queue_create(ctx, req, &vqueue_res);
      if (!rsp->ret)
         memcpy(&rsp->vqueue_res, vqueue_res, sizeof(*vqueue_res));

      break;
   }
   case VHSAKMT_CCMD_QUEUE_DESTROY: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
      if (obj) {
         vhsakmt_free_object(&ctx->base, &obj->base);
         break;
      }

      rsp->ret = hsaKmtDestroyQueue(req->QueueId);
      break;
   }
   default:
      vhsa_err("Queue command: %d not support.", req->type);
      break;
   }

   if (rsp->ret)
      vhsa_err("Type: %d ret: %d", req->type, rsp->ret);

   return 0;
}

static int
vhsakmt_ccmd_gl_inter(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   const struct vhsakmt_ccmd_gl_inter_req *req = to_vhsakmt_ccmd_gl_inter_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_gl_inter_rsp *rsp;
   size_t rsp_len = sizeof(*rsp);

   switch (req->type) {
   case VHSAKMT_CCMD_GL_REG_GHD_TO_NODES: {
      HsaGraphicsResourceInfo info;
      int ret = 0;

      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->reg_ghd_to_nodes.res_handle);
      if (!obj || obj->fd == -1) {
         vhsa_err("GL interop dmabuf no fd or no obj, res id: %u, obj: %p",
                  req->reg_ghd_to_nodes.res_handle, (void *)obj);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = HSAKMT_STATUS_INVALID_HANDLE;
         break;
      }

      ret = hsaKmtRegisterGraphicsHandleToNodes(
          obj->fd, &info, req->reg_ghd_to_nodes.NumberOfNodes, (HSAuint32 *)req->payload);
      vhsa_log("hsaKmtRegisterGraphicsHandleToNodes, fd: %d, address: %p, size: "
               "%lx, meta address: %p, meta size: %x, ret: %d",
               obj->fd, info.MemoryAddress, info.SizeInBytes, info.Metadata,
               info.MetadataSizeInBytes, ret);

      if (ret) {
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      } else {
         rsp = vhsakmt_context_rsp(ctx, hdr, rsp_len + info.MetadataSizeInBytes);
         memcpy(&rsp->info, &info, sizeof(info));
         memcpy(&rsp->payload, info.Metadata, info.MetadataSizeInBytes);
      }

      rsp->ret = ret;
      break;
   }

   default:
      vhsa_err("GL interop command: %d not support.", req->type);
      break;
   }

   if (rsp->ret)
      vhsa_err("Type: %d ret: %d", req->type, rsp->ret);

   return 0;
}

static const struct vhsakmt_ccmd ccmd_dispatch[] = {
#define HSAHANDLER(N, n)                                                       \
   [VHSAKMT_CCMD_##                                                            \
       N] = {#N, vhsakmt_ccmd_##n, sizeof(struct vhsakmt_ccmd_##n##_req)}
    HSAHANDLER(NOP, nop),     HSAHANDLER(QUERY_INFO, query_info),
    HSAHANDLER(EVENT, event), HSAHANDLER(MEMORY, memory),
    HSAHANDLER(QUEUE, queue), HSAHANDLER(GL_INTER, gl_inter)};

static int
vhsakmt_device_submit_fence(struct virgl_context *vctx, uint32_t flags, uint32_t ring_idx,
                            uint64_t fence_id)
{
   (void)vctx;
   (void)flags;

   /* ring_idx zero is used for the guest to synchronize with host CPU,
    * meaning by the time ->submit_fence() is called, the fence has
    * already passed.. so just immediate signal:
    */
   if (ring_idx == 0) {
      /* CPU CMDs here, just trigger the fence immediately.
       */
      vctx->fence_retire(vctx, ring_idx, fence_id);
      return 0;
   }

   return 0;
}

static int
vhsakmt_vm_init(struct vhsakmt_backend *b)
{
   uint64_t vm_base_addr;
   uint32_t i;

   if (b->vamgr_vm_base_addr_type == VHSA_VAMGR_VM_TYPE_FIXED_BASE) {
      if (b->vamgr_vm_fixed_base_addr == 0) {
         fprintf(stderr, "Invalid fixed base address: 0x%lx.\n",
                 b->vamgr_vm_fixed_base_addr);
         return -EINVAL;
      }
      vm_base_addr = ROUND_DOWN_TO(b->vamgr_vm_fixed_base_addr - b->vamgr_vm_kfd_size -
                                       b->vamgr_vm_scratch_size,
                                   VHSA_1G_SIZE);
   } else {
      void *mem = malloc(getpagesize());
      if (!mem) {
         fprintf(stderr, "Can not alloc vm base address.\n");
         return -ENOMEM;
      }
      if (!b->vamgr_vm_heap_interval_size) {
         b->vamgr_vm_heap_interval_size = VHSA_HEAP_INTERVAL_SIZE;
         fprintf(stderr, "Use default heap interval size: 0x%lx.\n",
                 b->vamgr_vm_heap_interval_size);
      }
      vm_base_addr =
          align64(((uint64_t)mem + b->vamgr_vm_heap_interval_size), VHSA_1G_SIZE);
      free(mem);
   }

#ifdef HSAKMT_VIRTIO
   /* only reserve memory address from kfd, scratch not needed casue it reserves
    * va by it self. */
   if (vhsakmt_reserve_va(vm_base_addr, b->vamgr_vm_kfd_size)) {
      fprintf(stderr, "Reserve vm base address at 0x%lx size: 0x%lx failed.\n",
              vm_base_addr, b->vamgr_vm_kfd_size);
      return -ENOMEM;
   }

   /* set expected doorbell address */
   if (vhsakmt_backend()->vamgr_vm_base_addr_type == VHSA_VAMGR_VM_TYPE_FIXED_BASE)
      vhsakmt_backend()->expected_doorbell_base_addr =
          vhsakmt_backend()->vamgr_vm_fixed_base_addr;
   else
      vhsakmt_backend()->expected_doorbell_base_addr =
          (vm_base_addr + b->vamgr_vm_kfd_size + b->vamgr_vm_scratch_size);

   if (hsaKmtSetDoorbellAddr((void *)vhsakmt_backend()->expected_doorbell_base_addr))
      fprintf(stderr, "Set expected doorbell address: 0x%lx failed.\n",
              vhsakmt_backend()->expected_doorbell_base_addr);
#endif

   if (vhsakmt_init_vamgr(&b->vamgr, vm_base_addr, b->vamgr_vm_kfd_size)) {
      fprintf(stderr, "Init kfd vamgr failed");
      return -ENOMEM;
   }

   vm_base_addr += b->vamgr_vm_kfd_size;

   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      if (b->vhsakmt_nodes[i].node_props.KFDGpuID) {
         if (vhsakmt_init_vamgr(&b->vhsakmt_nodes[i].scratch_vamgr, vm_base_addr,
                                b->vamgr_vm_scratch_size)) {
            fprintf(stderr, "Init scratch vamgr failed");
            return -ENOMEM;
         }

         b->vhsakmt_nodes[i].scratch_vamgr.vm_va_base_addr = vm_base_addr;
         vm_base_addr += b->vamgr_vm_scratch_size;
      }
   }

   return 0;
}

static int
vhsakmt_device_get_nodes_properties(struct vhsakmt_backend *b)
{
   int ret;
   uint32_t i;
   struct vhsakmt_node *node;

   ret = hsaKmtAcquireSystemProperties(&b->sys_props);
   if (ret) {
      fprintf(stderr, "Acquire system properties failed.\n");
      return ret;
   }

   if (b->sys_props.NumNodes == 0) {
      fprintf(stderr, "No nodes found.\n");
      return -EINVAL;
   }

   b->vhsakmt_num_nodes = b->sys_props.NumNodes;
   b->vhsakmt_nodes = calloc(b->vhsakmt_num_nodes, sizeof(struct vhsakmt_node));

   for (i = 0; i < b->vhsakmt_num_nodes; i++) {
      node = vhsakmt_get_node(b, i);
      if (!node) {
         fprintf(stderr, "Get node %d failed.\n", i);
         return -EINVAL;
      }
      ret = hsaKmtGetNodeProperties(i, &node->node_props);
      if (ret) {
         fprintf(stderr, "Get node %d properties failed.\n", i);
         return ret;
      }
      if (node->node_props.KFDGpuID)
         b->vhsakmt_gpu_count += 1;
   }

   return 0;
}

int
vhsakmt_device_init(void)
{
   int dump_va = 0;
   int ret;
   HsaVersionInfo info = {0};

   ret = hsaKmtOpenKFD();
   if (ret) {
      fprintf(stderr, "Open KFD failed.\n");
      return ret;
   }

   ret = hsaKmtGetVersion(&info);
   if (ret) {
      fprintf(stderr, "Get KFD version failed.\n");
      vhsakmt_backend()->hsakmt_capset.version_major = 1;
      vhsakmt_backend()->hsakmt_capset.version_minor = 0;
   } else {
      vhsakmt_backend()->hsakmt_capset.version_major = info.KernelInterfaceMajorVersion;
      vhsakmt_backend()->hsakmt_capset.version_minor = info.KernelInterfaceMinorVersion;
   }

    ret = vhsakmt_device_get_nodes_properties(&backend);
    if (ret) {
        fprintf(stderr, "Init nodes failed.\n");
        return ret;
    }

   ret = vhsakmt_vm_init(&backend);
   if (ret) {
      fprintf(stderr, "Init vamgr failed.\n");
      return ret;
   }

   char *d = getenv("VHSA_DUMP_VA");
   if (d)
      dump_va = atoi(d);

   hsakmt_set_dump_va(&vhsakmt_backend()->vamgr, dump_va);

   return 0;
}

static void
vhsakmt_device_destroy_scratch_vamgr(struct vhsakmt_backend *b)
{
    uint32_t i;

    for (i = 0; i < b->vhsakmt_num_nodes; i++) {
       if (vhsakmt_is_gpu_node(&b->vhsakmt_nodes[i]))
          vhsakmt_destroy_vamgr(&b->vhsakmt_nodes[i].scratch_vamgr);
    }
}

void
vhsakmt_device_fini(void)
{
#ifdef HSAKMT_VIRTIO
   vhsakmt_dereserve_va(vhsakmt_backend()->vamgr.vm_va_base_addr,
                        vhsakmt_backend()->vamgr.reserve_size +
                            vhsakmt_backend()->scratch_vamgr.reserve_size);
#endif

   vhsakmt_destroy_vamgr(&vhsakmt_backend()->vamgr);
   vhsakmt_device_destroy_scratch_vamgr(vhsakmt_backend());

   hsaKmtReleaseSystemProperties();
   hsaKmtCloseKFD();
}

void vhsakmt_device_reset(void) {}

size_t
vhsakmt_get_capset(UNUSED uint32_t set, UNUSED void *caps)
{
   struct virgl_renderer_capset_hsakmt *c = caps;

   if (c)
      *c = vhsakmt_backend()->hsakmt_capset;

   return sizeof(*c);
}

struct virgl_context *
hsakmt_device_create(UNUSED size_t debug_len, UNUSED const char *debug_name)
{
   struct vhsakmt_context *ctx;
   uint64_t va_start_addr;

   ctx = calloc(1, sizeof(struct vhsakmt_context));
   if (!ctx)
      return NULL;

   if (!vhsakmt_context_init(ctx, -1, ccmd_dispatch, ARRAY_SIZE(ccmd_dispatch))) {
      free(ctx);
      return NULL;
   }

   ctx->debug_name = strdup(debug_name);
   const char *d = getenv("VHSA_DEBUG");
   if (d)
      ctx->debug = atoi(d);

   va_start_addr =
       hsakmt_alloc_from_vamgr(&vhsakmt_backend()->vamgr, VHSA_CTX_RESERVE_SIZE);
   if (!va_start_addr) {
      fprintf(stderr, "Can not alloc from vamgr size: %lx",
              VHSA_CTX_RESERVE_SIZE);
      return NULL;
   }

   if (vhsakmt_init_vamgr(&ctx->vamgr, va_start_addr, VHSA_CTX_RESERVE_SIZE)) {
      fprintf(stderr, "Init vamgr failed");
      return NULL;
   }

   vhsa_log("vhsakmt vamgr:\nvm base addr: 0x%lx \nvm reserve size: 0x%lx",
            ctx->vamgr.vm_va_base_addr, ctx->vamgr.reserve_size);

   ctx->base.base.destroy = vhsakmt_device_destroy;
   ctx->base.base.attach_resource = vhsakmt_device_attach_resource;
   ctx->base.base.get_blob = vhsakmt_device_get_blob;
   ctx->base.base.submit_fence = vhsakmt_device_submit_fence;
   ctx->base.free_object = vhsakmt_free_object;
   /* must use private detach cause some resource free need delay */
   ctx->base.base.detach_resource = vhsakmt_device_detach_resource;

   return &ctx->base.base;
}
