/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2018. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rma.h"
#include "rma.inl"

#include <ucp/dt/dt_contig.h>
#include <ucs/profile/profile.h>
#include <ucs/sys/stubs.h>

#include <ucp/core/ucp_rkey.inl>
#include <ucp/proto/proto_common.inl>


#define UCP_RMA_CHECK_BUFFER(_buffer, _action) \
    do { \
        if (ENABLE_PARAMS_CHECK && ucs_unlikely((_buffer) == NULL)) { \
            _action; \
        } \
    } while (0)


#define UCP_RMA_CHECK_ZERO_LENGTH(_length, _action) \
    do { \
        if ((_length) == 0) { \
            _action; \
        } \
    } while (0)


#define UCP_RMA_CHECK(_context, _buffer, _length) \
    do { \
        UCP_CONTEXT_CHECK_FEATURE_FLAGS(_context, UCP_FEATURE_RMA, \
                                        return UCS_ERR_INVALID_PARAM); \
        UCP_RMA_CHECK_ZERO_LENGTH(_length, return UCS_OK); \
        UCP_RMA_CHECK_BUFFER(_buffer, return UCS_ERR_INVALID_PARAM); \
    } while (0)


#define UCP_RMA_CHECK_PTR(_context, _buffer, _length) \
    do { \
        UCP_CONTEXT_CHECK_FEATURE_FLAGS(_context, UCP_FEATURE_RMA, \
                                        return UCS_STATUS_PTR(UCS_ERR_INVALID_PARAM)); \
        UCP_RMA_CHECK_ZERO_LENGTH(_length, return NULL); \
        UCP_RMA_CHECK_BUFFER(_buffer, \
                             return UCS_STATUS_PTR(UCS_ERR_INVALID_PARAM)); \
    } while (0)


/* request can be released if
 *  - all fragments were sent (length == 0) (bcopy & zcopy mix)
 *  - all zcopy fragments are done (uct_comp.count == 0)
 *  - and request was allocated from the mpool
 *    (checked in ucp_request_complete_send)
 *
 * Request can be released either immediately or in the completion callback.
 * We must check req length in the completion callback to avoid the following
 * scenario:
 *  partial_send;no_resos;progress;
 *  send_completed;cb called;req free(ooops);
 *  next_partial_send; (oops req already freed)
 */
ucs_status_t ucp_rma_request_advance(ucp_request_t *req, ssize_t frag_length,
                                     ucs_status_t status,
                                     ucs_ptr_map_key_t req_id)
{
    ucs_assert(status != UCS_ERR_NOT_IMPLEMENTED);

    ucp_request_send_state_advance(req, NULL, UCP_REQUEST_SEND_PROTO_RMA,
                                   status);

    if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {
        if (status == UCS_ERR_NO_RESOURCE) {
            return UCS_ERR_NO_RESOURCE;
        }

        return UCS_OK;
    }

    ucs_assert(frag_length >= 0);
    ucs_assert(req->send.length >= frag_length);
    req->send.length -= frag_length;
    if (req->send.length == 0) {
        /* bcopy is the fast path */
        ucp_send_request_invoke_uct_completion(req);
        return UCS_OK;
    }
    req->send.buffer           = UCS_PTR_BYTE_OFFSET(req->send.buffer, frag_length);
    req->send.rma.remote_addr += frag_length;
    return UCS_INPROGRESS;
}

static void ucp_rma_request_bcopy_completion(uct_completion_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t,
                                          send.state.uct_comp);

    if (ucs_likely(req->send.length == req->send.state.dt.offset)) {
        ucp_send_request_id_release(req);
        ucp_request_complete_send(req, self->status);
    }
}

static void ucp_rma_request_zcopy_completion(uct_completion_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t,
                                          send.state.uct_comp);

    if (ucs_likely(req->send.length == req->send.state.dt.offset)) {
        ucp_send_request_id_release(req);
        ucp_request_send_buffer_dereg(req);
        ucp_request_complete_send(req, self->status);
    }
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_rma_request_init(ucp_request_t *req, ucp_ep_h ep, const void *buffer,
                     size_t length, uint64_t remote_addr, ucp_rkey_h rkey,
                     uct_pending_callback_t cb, size_t zcopy_thresh,
                     const ucp_request_param_t *param)
{
    ucp_context_h context = ep->worker->context;
    ucs_status_t status;

    req->flags                = 0;
    req->send.ep              = ep;
    req->send.buffer          = (void*)buffer;
    req->send.datatype        = ucp_dt_make_contig(1);
    req->send.mem_type        = ucp_request_get_memory_type(
                                    context, buffer, length,
                                    ucp_dt_make_contig(1), length, param);
    req->send.length          = length;
    req->send.rma.remote_addr = remote_addr;
    req->send.rma.rkey        = rkey;
    req->send.uct.func        = cb;
    req->send.lane            = rkey->cache.rma_lane;
    ucp_request_send_state_init(req, ucp_dt_make_contig(1), length);
    ucp_request_send_state_reset(req,
                                 (length < zcopy_thresh) ?
                                 ucp_rma_request_bcopy_completion :
                                 ucp_rma_request_zcopy_completion,
                                 UCP_REQUEST_SEND_PROTO_RMA);
#if UCS_ENABLE_ASSERT
    req->send.cb              = NULL;
#endif
    if (length < zcopy_thresh) {
        return UCS_OK;
    }

    status = ucp_send_request_set_user_memh(req,
                                            ucp_ep_config(ep)->key.rma_md_map,
                                            param);
    if (status != UCS_OK) {
        return status;
    }

    return ucp_request_send_reg_lane(req, req->send.lane);
}

static UCS_F_ALWAYS_INLINE ucs_status_ptr_t
ucp_rma_nonblocking(ucp_ep_h ep, const void *buffer, size_t length,
                    uint64_t remote_addr, ucp_rkey_h rkey,
                    uct_pending_callback_t progress_cb, size_t zcopy_thresh,
                    const ucp_request_param_t *param)
{
    ucs_status_t status;
    ucp_request_t *req;

    req = ucp_request_get_param(ep->worker, param,
                                {return UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);});

    status = ucp_rma_request_init(req, ep, buffer, length, remote_addr, rkey,
                                  progress_cb, zcopy_thresh, param);
    if (ucs_unlikely(status != UCS_OK)) {
        return UCS_STATUS_PTR(status);
    }

    return ucp_rma_send_request(req, param);
}

ucs_status_t ucp_put_nbi(ucp_ep_h ep, const void *buffer, size_t length,
                         uint64_t remote_addr, ucp_rkey_h rkey)
{
    ucs_status_ptr_t status_ptr;

    status_ptr = ucp_put_nbx(ep, buffer, length, remote_addr, rkey,
                             &ucp_request_null_param);
    if (UCS_PTR_IS_PTR(status_ptr)) {
        ucp_request_free(status_ptr);
        return UCS_INPROGRESS;
    }

    /* coverity[overflow] */
    return UCS_PTR_STATUS(status_ptr);
}

ucs_status_ptr_t ucp_put_nb(ucp_ep_h ep, const void *buffer, size_t length,
                            uint64_t remote_addr, ucp_rkey_h rkey,
                            ucp_send_callback_t cb)
{
    ucp_request_param_t param = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = (ucp_send_nbx_callback_t)cb
    };

    return ucp_put_nbx(ep, buffer, length, remote_addr, rkey, &param);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_put_send_short(ucp_ep_h ep, const void *buffer, size_t length,
                   uint64_t remote_addr, ucp_rkey_h rkey,
                   const ucp_request_param_t *param)
{
    const ucp_rkey_config_t *rkey_config;
    uct_rkey_t tl_rkey;
    ucs_status_t status;

    if (ucs_unlikely(param->op_attr_mask & (UCP_OP_ATTR_FIELD_DATATYPE |
                                            UCP_OP_ATTR_FLAG_NO_IMM_CMPL))) {
        return UCS_ERR_NO_RESOURCE;
    }

    rkey_config = ucp_rkey_config(ep->worker, rkey);
    if (ucs_unlikely(!ucp_proto_select_is_short(ep, &rkey_config->put_short,
                                                length))) {
        return UCS_ERR_NO_RESOURCE;
    }

    tl_rkey = ucp_rkey_get_tl_rkey(rkey, rkey_config->put_short.rkey_index);

    if (ucs_unlikely(ucp_ep_rma_is_fence_required(ep))) {
        /* TODO: check support for fence in fast path short */
        return UCS_ERR_NO_RESOURCE;
    }

    status = UCS_PROFILE_CALL(uct_ep_put_short,
                              ucp_ep_get_fast_lane(ep,
                                                   rkey_config->put_short.lane),
                              buffer, length, remote_addr, tl_rkey);
    if (status == UCS_OK) {
        ep->ext->unflushed_lanes |= UCS_BIT(rkey_config->put_short.lane);
    }

    return status;
}

ucs_status_ptr_t ucp_put_nbx(ucp_ep_h ep, const void *buffer, size_t count,
                             uint64_t remote_addr, ucp_rkey_h rkey,
                             const ucp_request_param_t *param)
{
    ucp_worker_h worker     = ep->worker;
    size_t contig_length    = 0;
    ucp_datatype_t datatype = ucp_dt_make_contig(1);
    ucp_ep_rma_config_t *rma_config;
    ucs_status_ptr_t ret;
    ucs_status_t status;
    ucp_request_t *req;

    UCP_REQUEST_CHECK_PARAM(param);
    UCP_RMA_CHECK_PTR(worker->context, buffer, count);
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucs_trace_req("put_nbx buffer %p count %zu remote_addr %" PRIx64
                  " rkey %p to %s cb %p",
                  buffer, count, remote_addr, rkey, ucp_ep_peer_name(ep),
                  ucp_request_param_send_callback(param));

    if (worker->context->config.ext.proto_enable) {
        status = ucp_put_send_short(ep, buffer, count, remote_addr, rkey, param);
        if (ucs_likely(status != UCS_ERR_NO_RESOURCE) ||
            ucs_unlikely(param->op_attr_mask & UCP_OP_ATTR_FLAG_FORCE_IMM_CMPL)) {
            ret = UCS_STATUS_PTR(status);
            goto out_unlock;
        }

        req = ucp_request_get_param(worker, param,
                                    {ret = UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);
                                    goto out_unlock;});
        req->send.rma.rkey        = rkey;
        req->send.rma.remote_addr = remote_addr;

        if (ucs_unlikely(param->op_attr_mask & UCP_OP_ATTR_FIELD_DATATYPE)) {
            datatype = param->datatype;
            if (UCP_DT_IS_CONTIG(datatype)) {
                contig_length = ucp_contig_dt_length(datatype, count);
            }
        } else {
            contig_length = count;
        }

        ret = ucp_proto_request_send_op(
                ep, &ucp_rkey_config(worker, rkey)->proto_select,
                rkey->cfg_index, req, ucp_ep_rma_get_fence_flag(ep),
                UCP_OP_ID_PUT, buffer, count, datatype, contig_length, param, 0,
                0);
    } else {
        status = UCP_RKEY_RESOLVE(rkey, ep, rma);
        if (status != UCS_OK) {
            ret = UCS_STATUS_PTR(status);
            goto out_unlock;
        }

        /* Fast path for a single short message */
        if (ucs_likely(!(param->op_attr_mask & UCP_OP_ATTR_FLAG_NO_IMM_CMPL) &&
                       ((ssize_t)count <= rkey->cache.max_put_short))) {
            status = UCS_PROFILE_CALL(
                    uct_ep_put_short,
                    ucp_ep_get_fast_lane(ep, rkey->cache.rma_lane), buffer,
                    count, remote_addr, rkey->cache.rma_rkey);
            if (ucs_likely(status != UCS_ERR_NO_RESOURCE)) {
                ret = UCS_STATUS_PTR(status);
                goto out_unlock;
            }
        }

        if (ucs_unlikely(param->op_attr_mask & UCP_OP_ATTR_FLAG_FORCE_IMM_CMPL)) {
            ret = UCS_STATUS_PTR(UCS_ERR_NO_RESOURCE);
            goto out_unlock;
        }

        rma_config = &ucp_ep_config(ep)->rma[rkey->cache.rma_lane];
        ret = ucp_rma_nonblocking(ep, buffer, count, remote_addr, rkey,
                                  UCP_RKEY_RMA_PROTO(rkey->cache.rma_proto_index)->progress_put,
                                  rma_config->put_zcopy_thresh, param);
    }

out_unlock:
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    return ret;
}

ucs_status_t ucp_get_nbi(ucp_ep_h ep, void *buffer, size_t length,
                         uint64_t remote_addr, ucp_rkey_h rkey)
{
    ucs_status_ptr_t status_ptr;

    status_ptr = ucp_get_nbx(ep, buffer, length, remote_addr, rkey,
                             &ucp_request_null_param);
    if (UCS_PTR_IS_PTR(status_ptr)) {
        ucp_request_free(status_ptr);
        return UCS_INPROGRESS;
    }

    /* coverity[overflow] */
    return UCS_PTR_STATUS(status_ptr);
}

ucs_status_ptr_t ucp_get_nb(ucp_ep_h ep, void *buffer, size_t length,
                            uint64_t remote_addr, ucp_rkey_h rkey,
                            ucp_send_callback_t cb)
{
    ucp_request_param_t param = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK,
        .cb.send      = (ucp_send_nbx_callback_t)cb
    };

    return ucp_get_nbx(ep, buffer, length, remote_addr, rkey, &param);
}

ucs_status_ptr_t ucp_get_nbx(ucp_ep_h ep, void *buffer, size_t count,
                             uint64_t remote_addr, ucp_rkey_h rkey,
                             const ucp_request_param_t *param)
{
    ucp_worker_h worker  = ep->worker;
    size_t contig_length = 0;
    ucp_ep_rma_config_t *rma_config;
    ucs_status_ptr_t ret;
    ucs_status_t status;
    ucp_request_t *req;
    uintptr_t datatype;

    if (ucs_unlikely(param->op_attr_mask & UCP_OP_ATTR_FLAG_FORCE_IMM_CMPL)) {
        return UCS_STATUS_PTR(UCS_ERR_NO_RESOURCE);
    }

    UCP_REQUEST_CHECK_PARAM(param);
    UCP_RMA_CHECK_PTR(worker->context, buffer, count);
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    ucs_trace_req("get_nbx buffer %p count %zu remote_addr %" PRIx64
                  " rkey %p from %s cb %p",
                  buffer, count, remote_addr, rkey, ucp_ep_peer_name(ep),
                  ucp_request_param_send_callback(param));

    if (worker->context->config.ext.proto_enable) {
        datatype = ucp_request_param_datatype(param);
        req = ucp_request_get_param(worker, param,
                                    {ret = UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);
                                    goto out_unlock;});

        req->send.rma.rkey             = rkey;
        req->send.rma.remote_addr      = remote_addr;
        req->send.state.completed_size = 0;
        if (UCP_DT_IS_CONTIG(datatype)) {
            contig_length = ucp_contig_dt_length(datatype, count);
        }

        ret = ucp_proto_request_send_op(
                ep, &ucp_rkey_config(worker, rkey)->proto_select,
                rkey->cfg_index, req, ucp_ep_rma_get_fence_flag(ep),
                UCP_OP_ID_GET, buffer, count, datatype, contig_length, param, 0,
                0);
    } else {
        status = UCP_RKEY_RESOLVE(rkey, ep, rma);
        if (status != UCS_OK) {
            ret = UCS_STATUS_PTR(status);
            goto out_unlock;
        }

        rma_config = &ucp_ep_config(ep)->rma[rkey->cache.rma_lane];
        ret        = ucp_rma_nonblocking(ep, buffer, count, remote_addr, rkey,
                                         UCP_RKEY_RMA_PROTO(rkey->cache.rma_proto_index)->progress_get,
                                         rma_config->get_zcopy_thresh, param);
    }

out_unlock:
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    return ret;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_put, (ep, buffer, length, remote_addr, rkey),
                 ucp_ep_h ep, const void *buffer, size_t length,
                 uint64_t remote_addr, ucp_rkey_h rkey)
{
    return ucp_rma_wait(ep->worker,
                        ucp_put_nb(ep, buffer, length, remote_addr, rkey,
                                   (ucp_send_callback_t)ucs_empty_function),
                        "put");
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_get, (ep, buffer, length, remote_addr, rkey),
                 ucp_ep_h ep, void *buffer, size_t length,
                 uint64_t remote_addr, ucp_rkey_h rkey)
{
    return ucp_rma_wait(ep->worker,
                        ucp_get_nb(ep, buffer, length, remote_addr, rkey,
                                   (ucp_send_callback_t)ucs_empty_function),
                        "get");
}
