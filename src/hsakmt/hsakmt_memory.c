#include "hsakmt_memory.h"
#include "util/hsakmt_util.h"

static int
vhsakmt_scratch_init(struct vhsakmt_context *ctx, struct vhsakmt_node *node,
                          struct vhsakmt_ccmd_memory_req *req, struct vhsakmt_backend *b)
{
   int ret = 0;
   void *mem;

   pthread_mutex_lock(&b->hsakmt_mutex);
   if (node->scratch_base)
      goto out;

   mem = (void *)node->scratch_vamgr.vm_va_base_addr;

   ret =
       hsaKmtAllocMemory(req->alloc_args.PreferredNode, node->scratch_vamgr.reserve_size,
                         req->alloc_args.MemFlags, &mem);
   vhsa_log("Alloc scratch target: %p -> real: %p size: %lx ret: %d",
            (void *)node->scratch_vamgr.vm_va_base_addr, mem,
            node->scratch_vamgr.reserve_size, ret);
   if (ret) {
      vhsa_err("Alloc scratch failed: %d (%s)", ret, strerror(errno));
      goto out;
   }
   if (node->scratch_vamgr.vm_va_base_addr != (uint64_t)mem) {
      vhsa_err(
          "Alloc scratch doesn't match with target address: %p, size: %lx error: %d (%s)",
          mem, node->scratch_vamgr.reserve_size, ret, strerror(errno));
      ret = -ENOMEM;
      goto out;
   }

   node->scratch_base = mem;

out:
    pthread_mutex_unlock(&b->hsakmt_mutex);
    return ret;
}

static int
vhsakmt_alloc_scratch(struct vhsakmt_context *ctx,
                             struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress)
{
   int ret = 0;
   void *mem;

   struct vhsakmt_node *node =
       vhsakmt_get_node(vhsakmt_backend(), req->alloc_args.PreferredNode);
   if (!node) {
      vhsa_err("Invalid node %d", req->alloc_args.PreferredNode);
      return HSAKMT_STATUS_INVALID_NODE_UNIT;
   }

   /* Scratch area lazy init */
   if (!node->scratch_base) {
      ret = vhsakmt_scratch_init(ctx, node, req, vhsakmt_backend());
      if (ret)
         return ret;
   }

   mem =
       (void *)hsakmt_alloc_from_vamgr(&node->scratch_vamgr, req->alloc_args.SizeInBytes);
   if (!mem) {
      vhsa_err("Can not alloc scratch from vamgr size: %lx", req->alloc_args.SizeInBytes);
      return -ENOMEM;
   }

   vhsa_log("scratch alloc at node: %d address: %p size: %lx\n",
            req->alloc_args.PreferredNode, mem, req->alloc_args.SizeInBytes);

   *MemoryAddress = mem;
   return 0;
}

/* Alloc host and device memory */
static int
vhsakmt_alloc_host(struct vhsakmt_context *ctx,
                          struct vhsakmt_ccmd_memory_req *req, void **MemoryAddress)
{
   int ret = 0;
   void *mem = NULL;

   mem = (void *)hsakmt_alloc_from_vamgr(&ctx->vamgr, req->alloc_args.SizeInBytes);
   if (!mem) {
      vhsa_err("Can not alloc from vamgr, size: %lx", req->alloc_args.SizeInBytes);
      return -ENOMEM;
   }

   *MemoryAddress = mem;

   ret = hsaKmtAllocMemory(req->alloc_args.PreferredNode, req->alloc_args.SizeInBytes,
                           req->alloc_args.MemFlags, MemoryAddress);

   if (ret) {
      vhsa_err(
          "Alloc host device memory failed, target address: %p, size: %lx error: %d (%s)",
          mem, req->alloc_args.SizeInBytes, ret, strerror(errno));
      goto failed_free_vamgr;
   }

   if (*MemoryAddress != mem) {
      vhsa_log("Alloc host device memory: %p != real: %p", mem, *MemoryAddress);
      *MemoryAddress = NULL;
      goto failed_free_mem;
   }

   return 0;

failed_free_mem:
   hsaKmtFreeMemory(*MemoryAddress, req->alloc_args.SizeInBytes);
failed_free_vamgr:
   hsakmt_free_from_vamgr(&ctx->vamgr, (uint64_t)mem);
   return ret;
}

static int
vhsakmt_alloc_memory(struct vhsakmt_context *ctx, struct vhsakmt_ccmd_memory_req *req,
                     void **MemoryAddress)
{
   int ret = 0;
   struct vhsakmt_object *obj;

   if (!vhsakmt_context_blob_id_valid(ctx, req->blob_id)) {
      vhsa_err("Invalid blob_id %ld", req->blob_id);
      return -EINVAL;
   }

   req->alloc_args.MemFlags.ui32.FixedAddress = 1;

   if (req->alloc_args.MemFlags.ui32.Scratch)
      ret = vhsakmt_alloc_scratch(ctx, req, MemoryAddress);
   else
      ret = vhsakmt_alloc_host(ctx, req, MemoryAddress);

   if (ret)
      return ret;

   obj = vhsakmt_object_create(*MemoryAddress, req->alloc_args.MemFlags.Value,
                               req->alloc_args.SizeInBytes, VHSAKMT_OBJ_HOST_MEM);

   vhsakmt_context_object_set_blob_id(ctx, obj, req->blob_id);

   return 0;
}

int
vhsakmt_ccmd_memory(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   struct vhsakmt_ccmd_memory_req *req = to_vhsakmt_ccmd_memory_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_memory_rsp *rsp;
   unsigned rsp_len = sizeof(*rsp);

   VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
   if (!rsp)
      return -ENOMEM;

   switch (req->type) {
   case VHSAKMT_CCMD_MEMORY_ALLOC: {
      void *MemoryAddress = NULL;
      rsp->ret = vhsakmt_alloc_memory(ctx, req, &MemoryAddress);
      rsp->memory_handle = (uint64_t)MemoryAddress;
      vhsa_log("Alloc memory at node: %d addres: %p, size: %lx, ret: %d",
               req->alloc_args.PreferredNode, MemoryAddress, req->alloc_args.SizeInBytes,
               rsp->ret);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_MAP_TO_GPU_NODES: {
      HSAuint64 AlternateVAGPU = 0;
      VHSA_CHECK_VA(req->map_to_GPU_nodes_args.MemoryAddress);
      rsp->ret = hsaKmtMapMemoryToGPUNodes(
          (void *)req->map_to_GPU_nodes_args.MemoryAddress,
          req->map_to_GPU_nodes_args.MemorySizeInBytes, &AlternateVAGPU,
          req->map_to_GPU_nodes_args.MemMapFlags,
          req->map_to_GPU_nodes_args.NumberOfNodes, (HSAuint32 *)req->payload);
      rsp->alternate_vagpu = AlternateVAGPU;
      break;
   }
   case VHSAKMT_CCMD_MEMORY_FREE: {
      VHSA_CHECK_VA(req->free_args.MemoryAddress);
      rsp->ret = hsaKmtFreeMemory((void *)req->free_args.MemoryAddress,
                                  req->free_args.SizeInBytes);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_UNMAP_TO_GPU: {
      VHSA_CHECK_VA(req->MemoryAddress);
      rsp->ret = hsaKmtUnmapMemoryToGPU((void *)req->MemoryAddress);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_AVAIL_MEM: {
      rsp->ret = hsaKmtAvailableMemory(req->Node, &rsp->available_bytes);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_MAP_MEM_TO_GPU: {
      HSAuint64 AlternateVAGPU = 0;
      VHSA_CHECK_VA(req->map_to_GPU_args.MemoryAddress);
      rsp->ret =
          hsaKmtMapMemoryToGPU((void *)req->map_to_GPU_args.MemoryAddress,
                               req->map_to_GPU_args.MemorySizeInBytes, &AlternateVAGPU);
      rsp->alternate_vagpu = AlternateVAGPU;

      if (req->map_to_GPU_args.need_create_bo) {
         struct vhsakmt_object *obj;
         obj = vhsakmt_object_create((void *)req->map_to_GPU_args.MemoryAddress, 0,
                                     req->map_to_GPU_args.MemorySizeInBytes,
                                     VHSAKMT_OBJ_SCRATCH_MAP_MEM);
         if (!obj) {
            vhsa_err("Create object failed");
            rsp->ret = -ENOMEM;
            break;
         }
         vhsakmt_context_object_set_blob_id(ctx, obj, req->blob_id);
      }

      break;
   }
   case VHSAKMT_CCMD_MEMORY_REG_MEM_WITH_FLAG: {
      VHSA_CHECK_VA(req->reg_mem_with_flag.MemoryAddress);
      rsp->ret = hsaKmtRegisterMemoryWithFlags(
          (void *)req->reg_mem_with_flag.MemoryAddress,
          req->reg_mem_with_flag.MemorySizeInBytes, req->reg_mem_with_flag.MemFlags);
      break;
   }
   case VHSAKMT_CCMD_MEMORY_DEREG_MEM: {
      VHSA_CHECK_VA(req->MemoryAddress);
      rsp->ret = hsaKmtDeregisterMemory((void *)req->MemoryAddress);
      if (req->res_id) {
         struct vhsakmt_object *obj =
             vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
         if (obj && (obj->type & VHSAKMT_OBJ_DMA_BUF))
            vhsakmt_free_object(&ctx->base, &obj->base);
      }

      break;
   }
   case VHSAKMT_CCMD_MEMORY_MAP_USERPTR: {
      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->res_id);
      if (obj) {
         rsp->map_userptr_rsp.userptr_handle = (uint64_t)obj->bo;
         rsp->map_userptr_rsp.npfns = obj->base.size;
         rsp->ret = 0;
         break;
      }

      vhsa_err("Can not find userptr BO, invalid res_id %d", req->res_id);
      rsp->ret = -EINVAL;
      break;
   }
   default:
      vhsa_err("Unsopported memory CMD: %d", req->type);
   }

   if (rsp->ret)
      vhsa_err("Type: %d ret: %d", req->type, rsp->ret);

   return 0;
}

int
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
