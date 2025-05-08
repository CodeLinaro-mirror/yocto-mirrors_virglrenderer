#include "hsakmt_queues.h"
#include "mesa/util/u_math.h"
#include "util/hsakmt_util.h"

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

int
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

void
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
