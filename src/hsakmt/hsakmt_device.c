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

#include <hsakmt/hsakmt.h>

#include "virgl_context.h"
#include "virglrenderer.h"

#include "util/u_math.h"
#include "hsakmt_virtio_proto.h"
#include "hsakmt_context.h"
#include "hsakmt_device.h"
#include "util/hsakmt_util.h"
#include "hsakmt_vm.h"
#include "hsakmt_query.h"
#include "hsakmt_events.h"
#include "hsakmt_memory.h"
#include "hsakmt_queues.h"

static struct vhsakmt_backend backend = {
    .context_type = VIRTGPU_HSAKMT_CONTEXT_AMDGPU,
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

inline struct vhsakmt_backend *vhsakmt_device_backend(void) { return &backend; }

static inline bool
vhsakmt_device_is_gpu_node(struct vhsakmt_node *n)
{
   return n->node_props.KFDGpuID != 0;
}

struct vhsakmt_node *
vhsakmt_device_get_node(struct vhsakmt_backend *b, uint32_t node_id)
{
   if (!b->vhsakmt_num_nodes || node_id >= b->vhsakmt_num_nodes)
      return NULL;

   return &b->vhsakmt_nodes[node_id];
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

   vhsakmt_context_free_object(&ctx->base, &obj->base);
}

static void
vhsakmt_device_destroy(struct virgl_context *vctx)
{
   struct vhsakmt_context *ctx = to_vhsakmt_context(to_drm_context(vctx));
   uint32_t i;

   vhsakmt_context_deinit(ctx);

   hsakmt_free_from_vamgr(&vhsakmt_device_backend()->vamgr, ctx->vamgr.vm_va_base_addr);

   for (i = 0; i < vhsakmt_device_backend()->vhsakmt_num_nodes; i++)
   {
      if (vhsakmt_device_is_gpu_node(&vhsakmt_device_backend()->vhsakmt_nodes[i]))
         hsakmt_free_from_vamgr(&vhsakmt_device_backend()->vhsakmt_nodes[i].scratch_vamgr,
            (uint64_t)vhsakmt_device_backend()->vhsakmt_nodes[i].scratch_base);
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
         obj = vhsakmt_context_object_create(res->mapped,
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
         obj = vhsakmt_context_object_create(blob->u.va_handle, 0,
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

static int
vhsakmt_ccmd_nop(UNUSED struct vhsakmt_base_context *bctx,
                 UNUSED struct vhsakmt_ccmd_req *hdr)
{
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
vhsakmt_device_vm_init(struct vhsakmt_backend *b)
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
   if (vhsakmt_device_backend()->vamgr_vm_base_addr_type == VHSA_VAMGR_VM_TYPE_FIXED_BASE)
      vhsakmt_device_backend()->expected_doorbell_base_addr =
          vhsakmt_device_backend()->vamgr_vm_fixed_base_addr;
   else
      vhsakmt_device_backend()->expected_doorbell_base_addr =
          (vm_base_addr + b->vamgr_vm_kfd_size + b->vamgr_vm_scratch_size);

   if (hsaKmtSetDoorbellAddr((void *)vhsakmt_device_backend()->expected_doorbell_base_addr))
      fprintf(stderr, "Set expected doorbell address: 0x%lx failed.\n",
              vhsakmt_device_backend()->expected_doorbell_base_addr);
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
      node = vhsakmt_device_get_node(b, i);
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
      vhsakmt_device_backend()->hsakmt_capset.version_major = 1;
      vhsakmt_device_backend()->hsakmt_capset.version_minor = 0;
   } else {
      vhsakmt_device_backend()->hsakmt_capset.version_major = info.KernelInterfaceMajorVersion;
      vhsakmt_device_backend()->hsakmt_capset.version_minor = info.KernelInterfaceMinorVersion;
   }
   vhsakmt_device_backend()->hsakmt_capset.context_type = VIRTGPU_HSAKMT_CONTEXT_AMDGPU;

    ret = vhsakmt_device_get_nodes_properties(&backend);
    if (ret) {
        fprintf(stderr, "Init nodes failed.\n");
        return ret;
    }

   ret = vhsakmt_device_vm_init(&backend);
   if (ret) {
      fprintf(stderr, "Init vamgr failed.\n");
      return ret;
   }

   char *d = getenv("VHSA_DUMP_VA");
   if (d)
      dump_va = atoi(d);

   hsakmt_set_dump_va(&vhsakmt_device_backend()->vamgr, dump_va);

   return 0;
}

static void
vhsakmt_device_destroy_scratch_vamgr(struct vhsakmt_backend *b)
{
    uint32_t i;

    for (i = 0; i < b->vhsakmt_num_nodes; i++) {
       if (vhsakmt_device_is_gpu_node(&b->vhsakmt_nodes[i]))
          vhsakmt_destroy_vamgr(&b->vhsakmt_nodes[i].scratch_vamgr);
    }
}

void
vhsakmt_device_fini(void)
{
#ifdef HSAKMT_VIRTIO
   vhsakmt_dereserve_va(vhsakmt_device_backend()->vamgr.vm_va_base_addr,
                        vhsakmt_device_backend()->vamgr.reserve_size +
                            vhsakmt_device_backend()->scratch_vamgr.reserve_size);
#endif

   vhsakmt_destroy_vamgr(&vhsakmt_device_backend()->vamgr);
   vhsakmt_device_destroy_scratch_vamgr(vhsakmt_device_backend());

   hsaKmtReleaseSystemProperties();
   hsaKmtCloseKFD();
}

void vhsakmt_device_reset(void) {}

size_t
vhsakmt_device_get_capset(UNUSED uint32_t set, UNUSED void *caps)
{
   struct virgl_renderer_capset_hsakmt *c = caps;

   if (c)
      *c = vhsakmt_device_backend()->hsakmt_capset;

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
       hsakmt_alloc_from_vamgr(&vhsakmt_device_backend()->vamgr, VHSA_CTX_RESERVE_SIZE);
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
   ctx->base.free_object = vhsakmt_context_free_object;
   /* must use private detach cause some resource free need delay */
   ctx->base.base.detach_resource = vhsakmt_device_detach_resource;

   return &ctx->base.base;
}
