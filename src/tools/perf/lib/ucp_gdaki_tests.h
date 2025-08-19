/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2015. ALL RIGHTS RESERVED.
* Copyright (C) The University of Tennessee and The University
*               of Tennessee Research Foundation. 2016. ALL RIGHTS RESERVED.
* Copyright (C) ARM Ltd. 2020.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#pragma once

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ucp_tests.h"
#include <tools/perf/gdaki/gdaki_mem.h>

template <ucx_perf_cmd_t CMD, ucx_perf_test_type_t TYPE, unsigned FLAGS>
class ucp_perf_test_gdaki_runner: public ucp_perf_test_runner_base_psn<uint64_t> {
public:
    typedef uint64_t psn_t;

    ucp_perf_test_gdaki_runner(ucx_perf_context_t &perf) :
        ucp_perf_test_runner_base_psn<uint64_t>(perf),
        m_gdaki_mem(sizeof(ucx_perf_context_t)),
        m_cpu_ctx{static_cast<ucx_perf_context_t*>(m_gdaki_mem.get_cpu_ptr())},
        m_gpu_ctx{static_cast<ucx_perf_context_t*>(m_gdaki_mem.get_gpu_ptr())}
    {
        memcpy(m_cpu_ctx, &perf, sizeof(ucx_perf_context_t));
    }

    virtual ucs_status_t run()
    {
        /* coverity[switch_selector_expr_is_constant] */
        switch (TYPE) {
        case UCX_PERF_TEST_TYPE_PINGPONG:
            switch (CMD) {
            case UCX_PERF_CMD_PUT_BATCH:
                return run_pingpong_batch_gdaki();
            default:
                return UCS_ERR_INVALID_PARAM;
            }
        case UCX_PERF_TEST_TYPE_STREAM_UNI:
            switch (CMD) {
            case UCX_PERF_CMD_PUT_BATCH:
                return run_stream_req_uni_batch_gdaki();
            default:
                return UCS_ERR_INVALID_PARAM;
            }
        default:
            return UCS_ERR_INVALID_PARAM;
        }
    }

private:
    gdaki_mem          m_gdaki_mem;
    ucx_perf_context_t *m_cpu_ctx;
    ucx_perf_context_t *m_gpu_ctx;

    ucs_status_t run_pingpong_batch_gdaki()
    {
        return UCS_OK;
    }

    ucs_status_t run_stream_req_uni_batch_gdaki()
    {
        size_t length = ucx_perf_get_message_size(&m_perf.params);
        ucs_assert(length >= sizeof(psn_t));

        m_perf.send_allocator->memset(m_perf.send_buffer, 0, length);
        m_perf.recv_allocator->memset(m_perf.recv_buffer, 0, length);

        ucp_perf_barrier(&m_perf);
        unsigned my_index = rte_call(&m_perf, group_index);

        if (my_index == 1) {
            ucx_perf_result_t result = {0};

            write_sn(m_perf.send_buffer, m_perf.params.send_mem_type, length,
                     m_perf.params.max_iter, m_perf.ucp.self_send_rkey);

            ucp_request_param_t send_params = {0};
            ucs_status_ptr_t request;
            request = ucp_put_nbx(m_perf.ucp.ep, m_perf.send_buffer, length,
                                  m_perf.ucp.remote_addr, m_perf.ucp.rkey,
                                  &send_params);
            request_wait(request, m_perf.params.send_mem_type, "send_last");

            ucx_perf_update(&m_perf, m_perf.params.max_iter,
                            m_perf.params.max_iter * length);
            ucx_perf_calc_result(&m_perf, &result);
        } else if (my_index == 0) {
            psn_t signal;
            do {
                signal = read_sn(m_perf.recv_buffer, length);
                usleep(10000);
            } while (signal != m_perf.params.max_iter);
        }

        ucp_perf_barrier(&m_perf);
        return UCS_OK;
    }
};
