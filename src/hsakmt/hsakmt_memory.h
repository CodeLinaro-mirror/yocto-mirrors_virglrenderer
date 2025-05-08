#ifndef HSAKMT_MEMORY_H
#define HSAKMT_MEMORY_H

#include "hsakmt_context.h"

int vhsakmt_ccmd_memory(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

int vhsakmt_ccmd_gl_inter(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr);

int vhsakmt_gpu_unmap(struct vhsakmt_object *obj);

int vhsakmt_free_userptr(UNUSED struct vhsakmt_object *obj);

int vhsakmt_free_scratch_map_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

int vhsakmt_free_scratch_reserve_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

int vhsakmt_free_host_mem(struct vhsakmt_context *ctx, struct vhsakmt_object *obj);

#endif /* HSAMKT_MEMORY_H */