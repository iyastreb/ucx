/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2015. ALL RIGHTS RESERVED.
 * Copyright (C) Intel Corporation, 2023. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */


#ifndef UCP_MM_H_
#define UCP_MM_H_

#include <ucp/api/ucp_def.h>
#include <ucp/core/ucp_ep.h>
#include <ucp/dt/dt.h>
#include <uct/api/uct.h>
#include <ucs/arch/bitops.h>
#include <ucs/debug/log.h>
#include <ucs/datastruct/mpool.h>
#include <ucs/memory/rcache.h>

#include <inttypes.h>


#define UCP_RCACHE_OVERHEAD_DEFAULT 50.0e-9


/* Mask of UCT memory flags that need make sure are present when reusing an
   existing region */
#define UCP_MM_UCT_ACCESS_FLAGS(_flags) ((_flags) & UCT_MD_MEM_ACCESS_ALL)


/**
 * Memory handle flags.
 */
enum {
    /*
     * Memory handle was imported and points to some peer's memory buffer.
     */
    UCP_MEMH_FLAG_IMPORTED     = UCS_BIT(0),
    UCP_MEMH_FLAG_MLOCKED      = UCS_BIT(1),
    UCP_MEMH_FLAG_HAS_AUTO_GVA = UCS_BIT(2),

    /**
     * Avoid using registration cache for the particular memory region.
     */
    UCP_MEMH_FLAG_NO_RCACHE    = UCS_BIT(3)
};


/**
 * Memory handle buffer packed flags.
 */
enum {
    UCP_MEMH_BUFFER_FLAG_EXPORTED = UCS_BIT(0)
};


/**
 * Memory handle.
 * Contains general information, and a list of UCT handles.
 * md_map specifies which MDs from the current context are present in the array.
 * The array itself contains only the MDs specified in md_map.
 */
typedef struct ucp_mem {
    ucs_rcache_region_t super;
    uint8_t             flags;          /* Memory handle flags */
    unsigned            uct_flags;      /* UCT memory registration flags */
    ucp_context_h       context;        /* UCP context that owns a memory handle */
    uct_alloc_method_t  alloc_method;   /* Method used to allocate the memory */
    ucs_sys_device_t    sys_dev;        /* System device index */
    ucs_memory_type_t   mem_type;       /* Type of allocated or registered memory */
    ucp_md_index_t      alloc_md_index; /* Index of MD used to allocate the memory */
    uint64_t            remote_uuid;    /* Remote UUID */
    ucp_md_map_t        md_map;         /* Which MDs have valid memory handles */
    ucp_md_map_t        inv_md_map;     /* Which memory handles should be invalidated
                                           when this handle is released */
    ucp_mem_h           parent;         /* - NULL if entry was obtained via ucp_memh_get()
                                           - pointer to rcache memh if entry is a user memh
                                           - pointer to self if entry is a user memh
                                             and rcache is disabled */
    uint64_t            reg_id;         /* Registration ID */
    uct_mem_h           uct[0];         /* Sparse memory handles array num_mds in size */
} ucp_mem_t;


/**
 * Memory descriptor.
 * Contains a memory handle of the chunk it belongs to.
 */
struct ucp_mem_desc {
    ucp_mem_h           memh;
    void                *ptr;
};


/**
 * Memory descriptor details for rndv fragments.
 */
typedef struct ucp_rndv_frag_mp_chunk_hdr {
    ucp_mem_h           memh;
    void                *next_frag_ptr;
} ucp_rndv_frag_mp_chunk_hdr_t;


/**
 * Memory pool private data descriptor.
 */
typedef struct ucp_rndv_mpool_priv {
    ucp_worker_h        worker;
    ucs_memory_type_t   mem_type;
    ucs_sys_device_t    sys_dev;
} ucp_rndv_mpool_priv_t;


typedef struct {
    ucp_mem_t memh;
    uct_mem_h uct[UCP_MAX_MDS];
} ucp_mem_dummy_handle_t;


/**
 * Memory type pack/unpack registration context
 */
typedef struct {
    ucp_md_index_t    md_index; /* index of MD */
    ucp_mem_h         ucp_memh; /* memh from rcache if MD is cacheable */
    uct_mem_h         uct_memh; /* memh for specific MD */
    uct_rkey_bundle_t rkey_bundle; /* rkey bundle from memh */
} ucp_mtype_pack_context_t;


extern ucp_mem_dummy_handle_t ucp_mem_dummy_handle;

extern const ucp_memory_info_t ucp_mem_info_unknown;


ucs_status_t ucp_reg_mpool_malloc(ucs_mpool_t *mp, size_t *size_p, void **chunk_p);

void ucp_reg_mpool_free(ucs_mpool_t *mp, void *chunk);

void ucp_mpool_obj_init(ucs_mpool_t *mp, void *obj, void *chunk);

ucs_status_t ucp_frag_mpool_malloc(ucs_mpool_t *mp, size_t *size_p, void **chunk_p);

void ucp_frag_mpool_free(ucs_mpool_t *mp, void *chunk);

void ucp_frag_mpool_obj_init(ucs_mpool_t *mp, void *obj, void *chunk);


/**
 * Update memory registration to a specified set of memory domains.
 *
 * @param [in] context     UCP context with MDs to use for registration.
 * @param [in] reg_md_map  Map of memory domains to update the registration to.
 *                         MDs which are present in reg_md_map, but not yet
 *                         registered will be registered.
 *                         MDs which were registered, but not present in r
 *                         eg_md_map, will be de-registered.
 * @param [in] address     Address to register, unused if reg_md_map == 0
 * @param [in] length      Length to register, unused if reg_md_map == 0
 * @param [in] uct_flags   Flags for UCT registration, unused if reg_md_map == 0
 * @param [in] alloc_md    If != NULL, MD that was used to register the memory.
 *                         This MD will not be used to register the memory again;
 *                         rather, the memh will be taken from *alloc_md_memh.
 * @param [inout] alloc_md_memh_p  If non-NULL, specifies/filled with the memory
 *                                 handle on alloc_md.
 * @param [inout] uct_memh Array of memory handles to update.
 * @param [inout] md_map_p Current map of registered MDs, updated by the function
 *                         to the new map o
 *
 * In case alloc_md != NULL, alloc_md_memh will hold the memory key obtained from
 * allocation. It will be put in the array of keys in the proper index.
 */
ucs_status_t ucp_mem_rereg_mds(ucp_context_h context, ucp_md_map_t reg_md_map,
                               void *address, size_t length, unsigned uct_flags,
                               uct_md_h alloc_md, ucs_memory_type_t mem_type,
                               uct_mem_h *alloc_md_memh_p, uct_mem_h *uct_memh,
                               ucp_md_map_t *md_map_p);

ucs_status_t ucp_mem_type_reg_buffers(ucp_worker_h worker, void *remote_addr,
                                      size_t length, ucs_memory_type_t mem_type,
                                      ucp_md_index_t md_index,
                                      ucp_mtype_pack_context_t *pack_context);

void ucp_mem_type_unreg_buffers(ucp_worker_h worker,
                                const ucp_mtype_pack_context_t *pack_context);

ucs_status_t ucp_memh_get_slow(ucp_context_h context, void *address,
                               size_t length, ucs_memory_type_t mem_type,
                               ucp_md_map_t reg_md_map, unsigned uct_flags,
                               const char *alloc_name, ucp_mem_h *memh_p);

ucs_status_t ucp_memh_register(ucp_context_h context, ucp_mem_h memh,
                               ucp_md_map_t md_map, unsigned uct_flags,
                               const char *alloc_name);

void ucp_memh_invalidate(ucp_context_h context, ucp_mem_h memh,
                         ucs_rcache_invalidate_comp_func_t cb, void *arg,
                         ucp_md_map_t inv_md_map);

void ucp_memh_put_slow(ucp_context_h context, ucp_mem_h memh);

ucs_status_t ucp_mem_rcache_init(ucp_context_h context,
                                 const ucs_rcache_config_t *rcache_config);

void ucp_mem_rcache_cleanup(ucp_context_h context);

void ucp_memh_disable_gva(ucp_mem_h memh, ucp_md_map_t md_map);

/**
 * Get memory domain index that is used to allocate certain memory type.
 *
 * @param [in]  context        UCP context containing memory domain indexes to
 *                             use for the memory allocation.
 * @param [in]  alloc_mem_type Memory type to get allocation index and memory
 *                             information for.
 * @param [in]  alloc_sys_dev  System device to get allocation index and memory
 *                             information for.
 * @param [out] md_idx_p       Index of the memory domain that is used to
 *                             allocate memory.
 * @param [out] mem_info_p     Information about the allocated memory.
 *
 * @return Error code as defined by @ref ucs_status_t.
 */
ucs_status_t ucp_mm_get_alloc_md_index(ucp_context_h context,
                                       ucs_memory_type_t alloc_mem_type,
                                       ucs_sys_device_t alloc_sys_dev,
                                       ucp_md_index_t *md_idx_p,
                                       ucp_memory_info_t *mem_info_p);

static UCS_F_ALWAYS_INLINE ucp_md_map_t
ucp_rkey_packed_md_map(const void *rkey_buffer)
{
    return *(const ucp_md_map_t*)rkey_buffer;
}

static UCS_F_ALWAYS_INLINE ucs_memory_type_t
ucp_rkey_packed_mem_type(const void *rkey_buffer)
{
    return (ucs_memory_type_t)(*(uint8_t *)((const ucp_md_map_t*)rkey_buffer + 1));
}

static UCS_F_ALWAYS_INLINE void *ucp_memh_address(const ucp_mem_h memh)
{
    return (void*)memh->super.super.start;
}

static UCS_F_ALWAYS_INLINE size_t ucp_memh_length(const ucp_mem_h memh)
{
    return memh->super.super.end - memh->super.super.start;
}


#define UCP_MEM_IS_HOST(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_HOST)
#define UCP_MEM_IS_ROCM(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_ROCM)
#define UCP_MEM_IS_CUDA(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_CUDA)
#define UCP_MEM_IS_ZE_HOST(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_ZE_HOST)
#define UCP_MEM_IS_ZE_DEVICE(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_ZE_DEVICE)
#define UCP_MEM_IS_CUDA_MANAGED(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_CUDA_MANAGED)
#define UCP_MEM_IS_ROCM_MANAGED(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_ROCM_MANAGED)
#define UCP_MEM_IS_ZE_MANAGED(_mem_type) ((_mem_type) == UCS_MEMORY_TYPE_ZE_MANAGED)
#define UCP_MEM_IS_ACCESSIBLE_FROM_CPU(_mem_type) \
    (UCS_BIT(_mem_type) & UCS_MEMORY_TYPES_CPU_ACCESSIBLE)
#define UCP_MEM_IS_GPU(_mem_type) (UCS_BIT(_mem_type) & \
                                   (UCS_BIT(UCS_MEMORY_TYPE_CUDA) | \
                                    UCS_BIT(UCS_MEMORY_TYPE_CUDA_MANAGED) | \
                                    UCS_BIT(UCS_MEMORY_TYPE_ROCM) | \
                                    UCS_BIT(UCS_MEMORY_TYPE_ZE_DEVICE) | \
                                    UCS_BIT(UCS_MEMORY_TYPE_ZE_MANAGED)))

#endif
