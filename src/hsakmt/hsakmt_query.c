// #include <amdgpu.h>
#include <drm/amdgpu_drm.h>
#include <xf86drm.h>

#include "hsakmt/hsakmt_query.h"
#include "hsakmt/util/hsakmt_util.h"

static int
init_amdgpu_drm(amdgpu_device_handle *dev_handle)
{
   uint32_t drm_major, drm_minor;
   int r;

   int fd = drmOpenWithType("amdgpu", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return -1;

   r = amdgpu_device_initialize(fd, &drm_major, &drm_minor, dev_handle);
   if (r)
      return -1;

   return fd;
}

static void
deinit_amdgpu_drm(int fd, amdgpu_device_handle dev_handle)
{
   amdgpu_device_deinitialize(dev_handle);
   close(fd);
}

int
vhsakmt_ccmd_query_info(struct vhsakmt_base_context *bctx, struct vhsakmt_ccmd_req *hdr)
{
   const struct vhsakmt_ccmd_query_info_req *req = to_vhsakmt_ccmd_query_info_req(hdr);
   struct vhsakmt_context *ctx = to_vhsakmt_context(bctx);
   struct vhsakmt_ccmd_query_info_rsp *rsp;
   unsigned rsp_len = sizeof(*rsp);

   switch (req->type) {
   case VHSAKMT_CCMD_QUERY_GPU_INFO: {
      amdgpu_device_handle dev_handle;
      int fd;
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      fd = init_amdgpu_drm(&dev_handle);
      if (fd < 0) {
         rsp->ret = -EINVAL;
         break;
      }

      rsp->ret = amdgpu_query_gpu_info(dev_handle, &rsp->gpu_info);

      deinit_amdgpu_drm(fd, dev_handle);

      break;
   }
   case VHSAKMT_CCMD_QUERY_OPEN_KFD: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = HSAKMT_STATUS_SUCCESS;

      rsp->open_kfd_rsp.vm_start = ctx->vamgr.vm_va_base_addr;
      rsp->open_kfd_rsp.vm_size = ctx->vamgr.reserve_size;

      if (req->open_kfd_args.cur_vm_start > ctx->vamgr.vm_va_base_addr) {
         fprintf(stderr, "VM error, guest VM start: 0x%lx, host VM start: 0x%lx\n",
                 req->open_kfd_args.cur_vm_start, ctx->vamgr.vm_va_base_addr);
         rsp->ret = HSAKMT_STATUS_ERROR;
      }

      break;
   }
   case VHSAKMT_CCMD_QUERY_TILE_CONFIG: {
      if (req->tile_config_args.config.NumTileConfigs >
              VHSAKMT_CCMD_QUERY_MAX_TILE_CONFIG ||
          req->tile_config_args.config.NumMacroTileConfigs >
              VHSAKMT_CCMD_QUERY_MAX_TILE_CONFIG) {
         vhsa_err("Invalid NumTileConfigs or NumMacroTileConfigs: %d, %d",
                  req->tile_config_args.config.NumTileConfigs,
                  req->tile_config_args.config.NumMacroTileConfigs);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(req->tile_config_args.config.NumTileConfigs * sizeof(HSAuint32),
                         rsp_len);
      rsp_len = size_add(
          req->tile_config_args.config.NumMacroTileConfigs * sizeof(HSAuint32), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      rsp->tile_config_rsp = req->tile_config_args.config;
      void *temp_tile_config = req->tile_config_args.config.TileConfig;
      void *temp_macro_tile_config = req->tile_config_args.config.MacroTileConfig;

      rsp->tile_config_rsp.TileConfig = (void *)rsp->payload;
      rsp->tile_config_rsp.MacroTileConfig = (void *)((uint8_t *)rsp->payload +
         rsp->tile_config_rsp.NumTileConfigs * sizeof(HSAuint32));

      rsp->ret = hsaKmtGetTileConfig(req->tile_config_args.NodeId, &rsp->tile_config_rsp);

      rsp->tile_config_rsp.TileConfig = temp_tile_config;
      rsp->tile_config_rsp.MacroTileConfig = temp_macro_tile_config;
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_VER: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = hsaKmtGetVersion(&rsp->kfd_version);

      break;
   }
   case VHSAKMT_CCMD_QUERY_REL_SYS_PROP: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      /* Do nothing, release sys prop in device destroy */
      rsp->ret = 0;
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_SYS_PROP: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      rsp->ret = hsaKmtAcquireSystemProperties(&rsp->sys_props);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NODE_PROP: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      rsp->ret = hsaKmtGetNodeProperties(req->NodeID, &rsp->node_props);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_XNACK_MODE: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      rsp->ret = hsaKmtGetXNACKMode(&rsp->xnack_mode);
      break;
   }
   case VHSAKMT_CCMD_QUERY_RUN_TIME_ENABLE: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      bool setup = req->run_time_enable_args.setupTtmp;
      rsp->ret = hsaKmtRuntimeEnable(NULL, setup);

      if (rsp->ret == HSAKMT_STATUS_UNAVAILABLE)
         rsp->ret = HSAKMT_STATUS_SUCCESS;
      break;
   }
   case VHSAKMT_CCMD_QUERY_RUN_TIME_DISABLE: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      /* Do nothing, runtime disable in device destroy */
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NOD_MEM_PROP: {
      HSAuint32 node_id = req->node_mem_prop_args.NodeId;
      HSAuint32 num_banks = req->node_mem_prop_args.NumBanks;

      if (req->node_mem_prop_args.NumBanks > VHSAKMT_CCMD_QUERY_MAX_GET_NOD_MEM_PROP) {
         vhsa_err("Invalid get node mem properity NumBanks: %d",
                  req->node_mem_prop_args.NumBanks);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(num_banks * sizeof(HsaMemoryProperties), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      HsaMemoryProperties *mem_prop = malloc(num_banks * sizeof(HsaMemoryProperties));
      if (!mem_prop) {
         rsp->ret = -ENOMEM;
         break;
      }

      rsp->ret = hsaKmtGetNodeMemoryProperties(node_id, num_banks, mem_prop);
      memcpy(rsp->payload, mem_prop, num_banks * sizeof(HsaMemoryProperties));
      free(mem_prop);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NOD_CACHE_PROP: {
      if (req->node_cache_prop_args.NumCaches >
          VHSAKMT_CCMD_QUERY_MAX_GET_NOD_CACHE_PROP) {
         vhsa_err("Invalid get node cache property NumCaches: %d",
                  req->node_cache_prop_args.NumCaches);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(req->node_cache_prop_args.NumCaches * sizeof(HsaCacheProperties),
                         rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      HsaCacheProperties *cache_prop =
          malloc(req->node_cache_prop_args.NumCaches * sizeof(HsaCacheProperties));
      if (!cache_prop) {
         rsp->ret = -ENOMEM;
         break;
      }

      rsp->ret = hsaKmtGetNodeCacheProperties(
          req->node_cache_prop_args.NodeId, req->node_cache_prop_args.ProcessorId,
          req->node_cache_prop_args.NumCaches, cache_prop);
      memcpy(rsp->payload, cache_prop,
             req->node_cache_prop_args.NumCaches * sizeof(HsaCacheProperties));

      free(cache_prop);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_NOD_IO_LINK_PROP: {
      HSAuint32 node_id = req->node_io_link_args.NodeId;
      HSAuint32 num_io_links = req->node_io_link_args.NumIoLinks;

      if (num_io_links > VHSAKMT_CCMD_QUERY_MAX_GET_NOD_IO_LINK_PROP) {
         vhsa_err("Invalid node io link: %d", num_io_links);
         VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
         rsp->ret = -EINVAL;
         break;
      }

      rsp_len = size_add(num_io_links * sizeof(HsaIoLinkProperties), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      HsaIoLinkProperties *IoLinkProperties =
          malloc(num_io_links * sizeof(HsaIoLinkProperties));

      rsp->ret = hsaKmtGetNodeIoLinkProperties(node_id, num_io_links, IoLinkProperties);
      memcpy(rsp->payload, IoLinkProperties, num_io_links * sizeof(HsaIoLinkProperties));

      free(IoLinkProperties);
      break;
   }
   case VHSAKMT_CCMD_QUERY_GET_CLOCK_COUNTERS: {
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = hsaKmtGetClockCounters(req->NodeID, &rsp->clock_counters);

      break;
   }
   case VHSAKMT_CCMD_QUERY_POINTER_INFO: {
      HsaPointerInfo info = {0};
      int ret = 0;

      ret = hsaKmtQueryPointerInfo((void *)req->pointer, &info);

      rsp_len = size_add(info.NMappedNodes * sizeof(HSAuint32), rsp_len);
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);
      rsp->ret = ret;

      memcpy(&rsp->ptr_info, &info, sizeof(info));

      vhsa_dbg("Query pointer info: 0x%lx, NMappedNodes: %d, MappedNodes: %p, "
               "NRegisteredNodes: %d, RegisteredNodes: %p",
               req->pointer, info.NMappedNodes, (void *)info.MappedNodes,
               info.NRegisteredNodes, (void *)info.RegisteredNodes);

      if (info.NMappedNodes && info.MappedNodes)
         memcpy(rsp->payload, info.MappedNodes, info.NMappedNodes * sizeof(HSAuint32));

      break;
   }
   case VHSAKMT_CCMD_QUERY_NANO_TIME: {
      struct timespec tp;
      VHSA_RSP_ALLOC(ctx, hdr, rsp_len);

      clock_gettime(CLOCK_MONOTONIC, &tp);
      rsp->nano_time_rsp.nano_time =
          (uint64_t)tp.tv_sec * (1000ULL * 1000ULL * 1000ULL) + (uint64_t)tp.tv_nsec;
      rsp->ret = 0;
      break;
   }

   default:
      vhsa_err("Unsopported query command: %d", req->type);
   }

   if (rsp->ret)
      vhsa_err("Type: %d ret: %d", req->type, rsp->ret);

   return 0;
}
