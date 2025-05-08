#include "hsakmt_events.h"
#include "util/hsakmt_util.h"
#include "hsakmt_memory.h"

void
vhsakmt_free_event_obj(UNUSED struct vhsakmt_context *ctx, struct vhsakmt_object *obj)
{
   if (!obj || obj->type != VHSAKMT_OBJ_EVENT)
      return;

   hsaKmtSetEvent(obj->bo);
   hsaKmtDestroyEvent(obj->bo);
}

int
vhsakmt_ccmd_event(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   struct vhsakmt_ccmd_event_req *req = to_vhsakmt_ccmd_event_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_event_rsp *rsp;

   switch (req->type) {
   case VHSAKMT_CCMD_EVENT_CREATE: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));
      struct vhsakmt_object *obj;
      HsaEvent *e = NULL;

      rsp->ret = hsaKmtCreateEvent(&req->create_args.EventDesc,
                                   req->create_args.ManualReset,
                                   req->create_args.IsSignaled, &e);
      if (rsp->ret) {
         vhsa_err("Create event failed, address: %p, id: %d, ret: %d",
                  (void *)e, e->EventId, rsp->ret);
         break;
      }
      memcpy(&rsp->vevent, e, sizeof(*e));
      rsp->vevent.event_handle = (uint64_t)e;

      obj = vhsakmt_object_create(e, 0, sizeof(*e), VHSAKMT_OBJ_EVENT);
      if (!obj)
         return -ENOMEM;

      vhsakmt_context_object_set_blob_id(ctx, obj, req->blob_id);
      break;
   }
   case VHSAKMT_CCMD_EVENT_DESTROY: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      struct vhsakmt_object *obj =
          vhsakmt_context_get_object_from_res_id(ctx, req->res_id);

      if (!obj) {
         if (req->event_hanele)
            hsaKmtDestroyEvent(req->event_hanele);
         else {
            vhsa_err("Invalid event resid: %d and handle: %lx", req->res_id, (uint64_t)req->event_hanele);
            rsp->ret = -EINVAL;
            break;
         }
      } else
         vhsakmt_free_object(&ctx->base, &obj->base);

      rsp->ret = 0;
      break;
   }
   case VHSAKMT_CCMD_EVENT_SET: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      rsp->ret = hsaKmtSetEvent(req->event_hanele);
      break;
   }
   case VHSAKMT_CCMD_EVENT_RESET: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      rsp->ret = hsaKmtResetEvent(req->event_hanele);
      break;
   }
   case VHSAKMT_CCMD_EVENT_QUERY_STATE: {
      VHSA_RSP_ALLOC(ctx, hdr, sizeof(*rsp));

      rsp->ret = hsaKmtQueryEventState(req->event_hanele);
      break;
   }
   case VHSAKMT_CCMD_EVENT_WAIT_ON_MULTI_EVENTS: {

      break;
   }
   case VHSAKMT_CCMD_EVENT_SET_TRAP: {
      unsigned rsp_len = sizeof(*rsp);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      VHSA_CHECK_VA(req->set_trap_handler_args.TrapHandlerBaseAddress);
      VHSA_CHECK_VA(req->set_trap_handler_args.TrapBufferBaseAddress);

      rsp->ret =
          hsaKmtSetTrapHandler(req->set_trap_handler_args.NodeId,
                               (void *)req->set_trap_handler_args.TrapHandlerBaseAddress,
                               req->set_trap_handler_args.TrapHandlerSizeInBytes,
                               (void *)req->set_trap_handler_args.TrapBufferBaseAddress,
                               req->set_trap_handler_args.TrapHandlerSizeInBytes);
      break;
   }

   default:
      vhsa_err("Unsopported event command: %d", req->type);
   }

   if (rsp->ret)
      vhsa_err("Type: %d ret: %d", req->type, rsp->ret);
   return 0;
}
