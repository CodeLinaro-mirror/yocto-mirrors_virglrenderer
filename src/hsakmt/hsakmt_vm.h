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

#ifndef HSAKMT_VM_H
#define HSAKMT_VM_H

#include <unistd.h>
#include <hsakmt/hsakmt.h>

// #include "hsakmt_device.h"
#include "util/rbtree.h"
#include "util/list.h"
#include "util/u_thread.h"

#define VHSA_1G_SIZE (0x40000000UL)
#define VHSA_CTX_RESERVE_SIZE (8UL * VHSA_1G_SIZE)
#define VHSA_CTX_SCRATCH_SIZE (0x100000000UL)
#define VHSA_MAX_CTX_SIZE (10UL)
#define VHSA_DEV_RESERVE_SIZE (VHSA_MAX_CTX_SIZE * VHSA_CTX_RESERVE_SIZE)
#define VHSA_DEV_SCRATCH_RESERVE_SIZE                                          \
   (VHSA_MAX_CTX_SIZE * VHSA_CTX_SCRATCH_SIZE)
#define VHSA_HEAP_INTERVAL_SIZE (2UL * 1024UL * VHSA_1G_SIZE)

#define VIRTGPU_HSAKMT_CONTEXT_AMDGPU 1

#define VHSA_VAMGR_VM_TYPE_FIXED_BASE 1
#define VHSA_VAMGR_VM_TYPE_HEAP_INTERVAL_BASE 2

#define VHSA_FIXED_VM_BASE_ADDR 0x700000000000

#define hsakmt_container_of(ptr, type, member)                                 \
   ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct vhsakmt_mem_frag {
   bool is_free : 1;
   bool is_list_head : 1;

   struct list_head head;
   void *dummy_list_head;

   rbtree_node_t rbt;
   rbtree_node_t free_frag_rbt;
} vhsakmt_mem_frag_t;

typedef struct hsakmt_vamgr {
   rbtree_t frag_tree;

   /* key: size, node: list */
   rbtree_t free_frag_tree;

   mtx_t frag_tree_lock;

   uint64_t vm_va_base_addr;
   uint64_t vm_va_high_addr;
   uint64_t reserve_size;
   uint64_t mem_used_size;

   bool dump_va;
} hsakmt_vamgr_t;

int vhsakmt_init_vamgr(hsakmt_vamgr_t *mgr, uint64_t start, uint64_t size);

int vhsakmt_destroy_vamgr(hsakmt_vamgr_t *mgr);

uint64_t hsakmt_alloc_from_vamgr(hsakmt_vamgr_t *mgr, uint64_t size);

int hsakmt_free_from_vamgr(hsakmt_vamgr_t *mgr, uint64_t addr);

void hsakmt_set_dump_va(hsakmt_vamgr_t *mgr, int dump_va);

#endif /* HSAKMT_VM_H */