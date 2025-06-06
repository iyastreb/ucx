/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2014. ALL RIGHTS RESERVED.
* Copyright (C) Huawei Technologies Co., Ltd. 2020.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef UCT_IB_DEVICE_H
#define UCT_IB_DEVICE_H

#include "ib_verbs.h"

#include <uct/api/uct.h>
#include <uct/base/uct_iface.h>
#include <ucs/stats/stats.h>
#include <ucs/debug/assert.h>
#include <ucs/datastruct/callbackq.h>
#include <ucs/datastruct/khash.h>
#include <ucs/type/spinlock.h>
#include <ucs/sys/sock.h>

#include <endian.h>
#include <linux/ip.h>


#define UCT_IB_QPN_ORDER                  24  /* How many bits can be an IB QP number */
#define UCT_IB_UIDX_SHIFT                 8   /* BE uidx shift */
#define UCT_IB_LRH_LEN                    8   /* IB Local routing header */
#define UCT_IB_GRH_LEN                    40  /* IB GLobal routing header */
#define UCT_IB_BTH_LEN                    12  /* IB base transport header */
#define UCT_IB_ROCE_LEN                   14  /* Ethernet header -
                                                 6B for Destination MAC +
                                                 6B for Source MAC + 2B Type (RoCE) */
#define UCT_IB_DETH_LEN                   8   /* IB datagram header */
#define UCT_IB_RETH_LEN                   16  /* IB RDMA header */
#define UCT_IB_ATOMIC_ETH_LEN             28  /* IB atomic header */
#define UCT_IB_AETH_LEN                   4   /* IB ack */
#define UCT_IB_PAYLOAD_ALIGN              4   /* IB payload padding */
#define UCT_IB_ICRC_LEN                   4   /* IB invariant crc footer */
#define UCT_IB_VCRC_LEN                   2   /* IB variant crc footer */
#define UCT_IB_DELIM_LEN                  2   /* IB wire delimiter */
#define UCT_IB_FDR_PACKET_GAP             64  /* Minimal FDR packet gap */
#define UCT_IB_MAX_MESSAGE_SIZE           (2UL << 30) /* Maximal IB message size */
#define UCT_IB_PKEY_PARTITION_MASK        0x7fff /* IB partition number mask */
#define UCT_IB_PKEY_MEMBERSHIP_MASK       0x8000 /* Full/send-only member */
#define UCT_IB_PKEY_DEFAULT               0xffff /* Default PKEY */
#define UCT_IB_FIRST_PORT                 1
#define UCT_IB_DEV_MAX_PORTS              2
#define UCT_IB_FABRIC_TIME_MAX            32
#define UCT_IB_INVALID_MKEY               0xffffffffu
#define UCT_IB_KEY                        0x1ee7a330
#define UCT_IB_LINK_LOCAL_PREFIX          be64toh(0xfe80000000000000ul) /* IBTA 4.1.1 12a */
#define UCT_IB_SITE_LOCAL_PREFIX          be64toh(0xfec0000000000000ul) /* IBTA 4.1.1 12b */
#define UCT_IB_SITE_LOCAL_MASK            be64toh(0xffffffffffff0000ul) /* IBTA 4.1.1 12b */
#define UCT_IB_SITE_LOCAL_FLID_MASK       be64toh(0xffffffff00000000ul) /* site-local + flid */
#define UCT_IB_GUID_OPENIB_OUI            0x001405 /* An OUI is a 24 bit globally unique assigned
                                                      number referenced by various standards.
                                                      IB_OPENIB_OUI is part of the routable alias
                                                      GUID built by SM. */
#define UCT_IB_DEFAULT_ROCEV2_DSCP        106  /* Default DSCP for RoCE v2 */
#define UCT_IB_ROCE_UDP_SRC_PORT_BASE     0xC000
#define UCT_IB_CQE_SL_PKTYPE_MASK         0x7 /* SL for IB or packet type
                                                 (GRH/IPv4/IPv6) for RoCE in the
                                                 CQE */
#define UCT_IB_DEVICE_SYSFS_PFX           "/sys/class/infiniband/%s"
#define UCT_IB_DEVICE_SYSFS_FMT           UCT_IB_DEVICE_SYSFS_PFX "/device/%s"
#define UCT_IB_DEVICE_SYSFS_GID_ATTR_PFX  UCT_IB_DEVICE_SYSFS_PFX "/ports/%d/gid_attrs"
#define UCT_IB_DEVICE_SYSFS_GID_TYPE_FMT  UCT_IB_DEVICE_SYSFS_GID_ATTR_PFX "/types/%d"
#define UCT_IB_DEVICE_SYSFS_GID_NDEV_FMT  UCT_IB_DEVICE_SYSFS_GID_ATTR_PFX "/ndevs/%d"
#define UCT_IB_DEVICE_ECE_DEFAULT         0x0         /* default ECE */
#define UCT_IB_DEVICE_ECE_MAX             0xffffffffU /* max ECE */
#define UCT_IB_DEVICE_DEFAULT_GID_INDEX 0   /* The gid index used by default for an IB/RoCE port */
#define UCT_IB_DEVICE_ROUTABLE_FLID_GID_INDEX 1 /* The gid index used by default
                                                   with FLID based IB routing */


enum {
    UCT_IB_DEVICE_STAT_ASYNC_EVENT,
    UCT_IB_DEVICE_STAT_LAST
};


typedef enum uct_ib_roce_version {
    UCT_IB_DEVICE_ROCE_V1,
    UCT_IB_DEVICE_ROCE_V1_5,
    UCT_IB_DEVICE_ROCE_V2,
    UCT_IB_DEVICE_ROCE_ANY
} uct_ib_roce_version_t;


enum {
    UCT_IB_DEVICE_FLAG_MLX4_PRM = UCS_BIT(1),   /* Device supports mlx4 PRM */
    UCT_IB_DEVICE_FLAG_MLX5_PRM = UCS_BIT(2),   /* Device supports mlx5 PRM */
    UCT_IB_DEVICE_FLAG_MELLANOX = UCS_BIT(3),   /* Mellanox device */
    UCT_IB_DEVICE_FLAG_SRQ      = UCS_BIT(4),   /* Supports SRQ */
    UCT_IB_DEVICE_FLAG_LINK_IB  = UCS_BIT(5),   /* Require only IB */
    UCT_IB_DEVICE_FLAG_DC_V1    = UCS_BIT(6),   /* Device supports DC ver 1 */
    UCT_IB_DEVICE_FLAG_DC_V2    = UCS_BIT(7),   /* Device supports DC ver 2 */
    UCT_IB_DEVICE_FLAG_AV       = UCS_BIT(8),   /* Device supports compact AV */
    UCT_IB_DEVICE_FLAG_DC       = UCT_IB_DEVICE_FLAG_DC_V1 |
                                  UCT_IB_DEVICE_FLAG_DC_V2, /* Device supports DC */
    UCT_IB_DEVICE_FAILED        = UCS_BIT(9)    /* Got fatal error */
};


/**
 * Flags which specify which address fields are present
 */
enum {
    /* GID index, used for both ETH or IB link layer.  */
    UCT_IB_ADDRESS_FLAG_GID_INDEX      = UCS_BIT(0),
    /* Defines path MTU size, used for both ETH or IB link layer. */
    UCT_IB_ADDRESS_FLAG_PATH_MTU       = UCS_BIT(1),
    /* PKEY value, used for both ETH or IB link layer. */
    UCT_IB_ADDRESS_FLAG_PKEY           = UCS_BIT(2),

    /* If set - ETH link layer, else- IB link layer. */
    UCT_IB_ADDRESS_FLAG_LINK_LAYER_ETH = UCS_BIT(3),

    /* Used for ETH link layer. */
    UCT_IB_ADDRESS_FLAG_ROCE_IPV6      = UCS_BIT(4),
    /* Used for ETH link layer, following bits are used to pack RoCE version. */
    UCT_IB_ADDRESS_FLAG_ETH_LAST       = UCS_BIT(5),

    /* Used for IB link layer. */
    UCT_IB_ADDRESS_FLAG_SUBNET16       = UCS_BIT(4),
    /* Used for IB link layer. */
    UCT_IB_ADDRESS_FLAG_SUBNET64       = UCS_BIT(5),
    /* Used for IB link layer. */
    UCT_IB_ADDRESS_FLAG_IF_ID          = UCS_BIT(6)
};


/**
 * IB network address
 */
typedef struct uct_ib_address {
    /* Using flags from UCT_IB_ADDRESS_FLAG_xx
     * For ETH link layer, the 4 msb's are used to indicate the RoCE version -
     * (by shifting the UCT_IB_DEVICE_ROCE_xx values when packing and unpacking
     * the ib address) */
    uint8_t            flags;
    /* Following fields appear in this order (if specified by flags).
     * The full gid always appears last:
     * - uint16_t lid
     * - uint64_t if_id
     * - uint16_t subnet16
     * - uint64_t subnet64
     * For RoCE:
     * - uint8_t gid[16]
     */
} UCS_S_PACKED uct_ib_address_t;


/**
 * PCI identifier of a device
 */
typedef struct {
    uint16_t                    vendor;
    uint16_t                    device;
} uct_ib_pci_id_t;


/**
 * IB device specification.
 */
typedef struct uct_ib_device_spec {
    const char                  *name;
    uct_ib_pci_id_t             pci_id;
    unsigned                    flags;
    uint8_t                     priority;
} uct_ib_device_spec_t;


KHASH_TYPE(uct_ib_ah, struct ibv_ah_attr, struct ibv_ah*);


/**
 * IB async event descriptor.
 */
typedef struct uct_ib_async_event {
    enum ibv_event_type event_type;             /* Event type */
    union {
        uint8_t         port_num;               /* Port number */
        uint32_t        qp_num;                 /* QP number */
        uint32_t        dct_num;                /* DCT number */
        void            *cookie;                /* Pointer to resource */
        uint32_t        resource_id;            /* Opaque resource ID */
    };
} uct_ib_async_event_t;


/**
 * IB async event waiting context.
 */
typedef struct uct_ib_async_event_wait {
    ucs_callback_t      cb;                     /* Callback */
    ucs_callbackq_t     *cbq;                   /* Async queue for callback */
    int                 cb_id;                  /* Scheduled callback ID */
} uct_ib_async_event_wait_t;


/**
 * IB async event state.
 */
typedef struct {
    unsigned                  fired;            /* Event happened */
    uct_ib_async_event_wait_t *wait_ctx;        /* Waiting context */
} uct_ib_async_event_val_t;


KHASH_TYPE(uct_ib_async_event, uct_ib_async_event_t, uct_ib_async_event_val_t);


/**
 * IB device (corresponds to HCA)
 */
typedef struct uct_ib_device {
    struct ibv_context          *ibv_context;    /* Verbs context */
    uct_ib_device_attr          dev_attr;        /* Cached device attributes */
    uint8_t                     first_port;      /* Number of first port (usually 1) */
    uint8_t                     num_ports;       /* Amount of physical ports */
    ucs_sys_cpuset_t            local_cpus;      /* CPUs local to device */
    int                         async_events;    /* Whether async events are handled */
    int                         max_zcopy_log_sge; /* Maximum sges log for zcopy am */
    UCS_STATS_NODE_DECLARE(stats)
    struct ibv_port_attr        port_attr[UCT_IB_DEV_MAX_PORTS]; /* Cached port attributes */
    uct_ib_pci_id_t             pci_id;          /* PCI identifiers */
    ucs_sys_device_t            sys_dev;         /* System device id */
    double                      pci_bw;          /* Supported PCI bandwidth */
    unsigned                    flags;
    uint8_t                     atomic_arg_sizes;
    uint8_t                     atomic_arg_sizes_be;
    uint8_t                     ext_atomic_arg_sizes;
    uint8_t                     ext_atomic_arg_sizes_be;
    uint8_t                     pci_fadd_arg_sizes;
    uint8_t                     pci_cswap_arg_sizes;
    uint8_t                     atomic_align;
    uint8_t                     lag_level;
    uint8_t                     req_notify_cq_support; /* Also indicates
                                                          IBV_SEND_SOLICITED
                                                          support */
    uint8_t                     ordered_send_comp;
    uint64_t                    mr_access_flags;
    uint32_t                    max_inline_data;

    /* AH hash */
    khash_t(uct_ib_ah)          ah_hash;
    ucs_recursive_spinlock_t    ah_lock;
    /* Async event subscribers */
    ucs_spinlock_t              async_event_lock;
    khash_t(uct_ib_async_event) async_events_hash;
} uct_ib_device_t;


/**
 * RoCE version
 */
typedef struct uct_ib_roce_version_info {
    /** RoCE version described by the UCT_IB_DEVICE_ROCE_xx values */
    uct_ib_roce_version_t ver;
    /** Address family of the port */
    sa_family_t           addr_family;
} uct_ib_roce_version_info_t;


typedef struct {
    union ibv_gid              gid;
    uint8_t                    gid_index;    /* IB/RoCE GID index to use */
    uct_ib_roce_version_info_t roce_info;    /* For a RoCE port */
} uct_ib_device_gid_info_t;



extern const double uct_ib_qp_rnr_time_ms[];


/**
 * Check if a port on a device is active and supports the given flags.
 */
ucs_status_t uct_ib_device_port_check(uct_ib_device_t *dev, uint8_t port_num,
                                      unsigned flags);


/**
 * Helper function to set ECE to qp.
 *
 * @param dev              IB device use to create qp.
 * @param qp               The qp to be set with ECE.
 * @param ece_val          The ece value to be set to qp.
 */
ucs_status_t
uct_ib_device_set_ece(uct_ib_device_t *dev, struct ibv_qp *qp,
                      uint32_t ece_val);


/*
 * Helper function to list IB transport resources.
 *
 * @param dev              IB device.
 * @param flags            Transport requirements from IB device (see UCT_IB_RESOURCE_FLAG_xx)
 * @param devices_p        Filled with a pointer to an array of devices.
 * @param num_devices_p    Filled with the number of devices.
 */
ucs_status_t uct_ib_device_query_ports(uct_ib_device_t *dev, unsigned flags,
                                       uct_tl_device_resource_t **devices_p,
                                       unsigned *num_devices_p);

ucs_status_t uct_ib_device_query(uct_ib_device_t *dev,
                                 struct ibv_device *ibv_device);

ucs_status_t uct_ib_device_init(uct_ib_device_t *dev,
                                struct ibv_device *ibv_device, int async_events
                                UCS_STATS_ARG(ucs_stats_node_t *stats_parent));

void uct_ib_device_cleanup(uct_ib_device_t *dev);


/**
 * @return device specification.
 */
const uct_ib_device_spec_t* uct_ib_device_spec(uct_ib_device_t *dev);


/**
 * Select the best gid to use and set its information on the RoCE port -
 * gid index, RoCE version and address family.
 *
 * @param [in]  dev             IB device.
 * @param [in]  port_num        Port number.
 * @param [in]  subnet_strs     List of allowed/restricted subnets to select
 *                              from.
 * @param [out] gid_info        Filled with the selected gid index and the
 *                              port's RoCE version and address family.
 */
ucs_status_t
uct_ib_device_select_gid(uct_ib_device_t *dev, uint8_t port_num,
                         const ucs_config_allow_list_t *subnets_array,
                         uct_ib_device_gid_info_t *gid_info);


/**
 * @return device name.
 */
const char *uct_ib_device_name(uct_ib_device_t *dev);


/**
 * @return whether the port is InfiniBand
 */
int uct_ib_device_is_port_ib(uct_ib_device_t *dev, uint8_t port_num);


/**
 * @return whether the port is RoCE
 */
int uct_ib_device_is_port_roce(uct_ib_device_t *dev, uint8_t port_num);


/**
 * @return whether the gid is valid
 */
int uct_ib_device_is_gid_valid(const union ibv_gid *gid);


/**
 * Convert time-in-seconds to IB fabric QP time value
 */
uint8_t uct_ib_to_qp_fabric_time(double time);


/**
 * Convert time-in-seconds to IB fabric RNR time value
 */
uint8_t uct_ib_to_rnr_fabric_time(double time);


/**
 * @return MTU in bytes.
 */
size_t uct_ib_mtu_value(enum ibv_mtu mtu);


/**
 * Modify QP to a given state and check for error
 */
ucs_status_t uct_ib_modify_qp(struct ibv_qp *qp, enum ibv_qp_state state);


/**
 * find device mtu. This function can be used before ib
 * interface is created.
 */
ucs_status_t uct_ib_device_mtu(const char *dev_name, uct_md_h md, int *p_mtu);

ucs_status_t uct_ib_device_find_port(uct_ib_device_t *dev,
                                     const char *resource_dev_name,
                                     uint8_t *p_port_num);

const char *uct_ib_wc_status_str(enum ibv_wc_status wc_status);

ucs_status_t
uct_ib_device_create_ah_cached(uct_ib_device_t *dev,
                               struct ibv_ah_attr *ah_attr, struct ibv_pd *pd,
                               const char *usage, struct ibv_ah **ah_p);

ucs_status_t uct_ib_device_get_roce_ndev_name(uct_ib_device_t *dev,
                                              uint8_t port_num,
                                              uint8_t gid_index,
                                              char *ndev_name, size_t max);

ucs_status_t uct_ib_iface_get_loopback_ndev_index(unsigned *ndev_index_p);

ucs_status_t
uct_ib_device_get_roce_ndev_index(uct_ib_device_t *dev, uint8_t port_num,
                                  uint8_t gid_index, unsigned *ndev_index_p);

unsigned uct_ib_device_get_roce_lag_level(uct_ib_device_t *dev,
                                          uint8_t port_num,
                                          uint8_t gid_index);


static inline struct ibv_port_attr*
uct_ib_device_port_attr(uct_ib_device_t *dev, uint8_t port_num)
{
    return &dev->port_attr[port_num - dev->first_port];
}

static inline int uct_ib_device_has_pci_atomics(uct_ib_device_t *dev)
{
    return !!((dev->pci_fadd_arg_sizes | dev->pci_cswap_arg_sizes) &
              (sizeof(uint32_t) | sizeof(uint64_t)));
}

const char *uct_ib_roce_version_str(uct_ib_roce_version_t roce_ver);

const char *uct_ib_gid_str(const union ibv_gid *gid, char *str, size_t max_size);

ucs_status_t uct_ib_device_query_gid(uct_ib_device_t *dev, uint8_t port_num,
                                     unsigned gid_index, union ibv_gid *gid,
                                     ucs_log_level_t error_level);

ucs_status_t uct_ib_device_query_gid_info(struct ibv_context *ctx, const char *dev_name,
                                          uint8_t port_num, unsigned gid_index,
                                          uct_ib_device_gid_info_t *info);

int uct_ib_device_test_roce_gid_index(uct_ib_device_t *dev, uint8_t port_num,
                                      const union ibv_gid *gid,
                                      uint8_t gid_index);

ucs_status_t
uct_ib_device_async_event_register(uct_ib_device_t *dev,
                                   enum ibv_event_type event_type,
                                   uint32_t resource_id);

/* Invoke the callback defined by 'wait_ctx' from callback queue when the event
 * fires. If it has already been fired, the callback is scheduled immediately to
 * the callback queue.
 *
 * @return UCS_OK, or UCS_ERR_BUSY if someone already waiting for this event.
 */
ucs_status_t
uct_ib_device_async_event_wait(uct_ib_device_t *dev,
                               enum ibv_event_type event_type,
                               uint32_t resource_id,
                               uct_ib_async_event_wait_t *wait_ctx);

void uct_ib_device_async_event_unregister(uct_ib_device_t *dev,
                                          enum ibv_event_type event_type,
                                          uint32_t resource_id);

ucs_status_t uct_ib_device_get_ah_cached(uct_ib_device_t *dev,
                                         struct ibv_ah_attr *ah_attr,
                                         struct ibv_ah **ah_p);

int uct_ib_get_cqe_size(int cqe_size_min);

const char* uct_ib_ah_attr_str(char *buf, size_t max,
                               const struct ibv_ah_attr *ah_attr);

ucs_status_t
uct_ib_device_roce_gid_to_sockaddr(sa_family_t af, const void *gid,
                                   struct sockaddr_storage *sock_storage);

static inline ucs_status_t uct_ib_poll_cq(struct ibv_cq *cq, unsigned *count, struct ibv_wc *wcs)
{
    int ret;

    ret = ibv_poll_cq(cq, *count, wcs);
    if (ret <= 0) {
        if (ucs_likely(ret == 0)) {
            return UCS_ERR_NO_PROGRESS;
        }
        ucs_fatal("failed to poll receive CQ %d", ret);
    }

    *count = ret;
    return UCS_OK;
}

static inline void uct_ib_destroy_cq(struct ibv_cq *cq, const char *desc)
{
    int ret = ibv_destroy_cq(cq);
    if (ret != 0) {
        ucs_warn("ibv_destroy_cq(%s) failed with error %d: %m", desc, ret);
    }
}

void uct_ib_handle_async_event(uct_ib_device_t *dev, uct_ib_async_event_t *event);

int uct_ib_device_is_smi(struct ibv_device *ibv_device);

#endif
