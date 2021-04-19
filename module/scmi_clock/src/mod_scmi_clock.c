/*
 * Arm SCP/MCP Software
 * Copyright (c) 2015-2021, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Description:
 *     SCMI Clock Management Protocol Support.
 */

#include <internal/scmi_clock.h>

#include <mod_clock.h>
#include <mod_scmi.h>
#include <mod_scmi_clock.h>

#include <fwk_assert.h>
#include <fwk_attributes.h>
#include <fwk_event.h>
#include <fwk_id.h>
#include <fwk_log.h>
#include <fwk_macros.h>
#include <fwk_mm.h>
#include <fwk_module.h>
#include <fwk_module_idx.h>
#include <fwk_status.h>
#include <fwk_thread.h>

#include <stdbool.h>
#include <string.h>

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
#    include <mod_resource_perms.h>
#endif

struct clock_operations {
    /*
     * Service identifier currently requesting operation from this clock.
     * A 'none' value indicates that there is no pending request.
     */
    fwk_id_t service_id;

    /*
     * The state to be set in this operation.
     */
    enum mod_clock_state state;

    /*
     * The clock device for this operation.
     */
    uint32_t clock_dev_id;

    /*
     * Request type for this operation.
     */
    enum scmi_clock_request_type request;
};

struct scmi_clock_ctx {
    /*! SCMI Clock Module Configuration */
    const struct mod_scmi_clock_config *config;

    /*! Maximum supported number of pending, asynchronous clock rate changes */
    uint8_t max_pending_transactions;

    /*!
     * Pointer to the table of agent descriptors, used to provide per-agent
     * views of clocks in the system.
     */
    const struct mod_scmi_clock_agent *agent_table;

    /* SCMI module API */
    const struct mod_scmi_from_protocol_api *scmi_api;

    /* Clock module API */
    const struct mod_clock_api *clock_api;

    /* Number of clock devices */
    int clock_devices;

    /* Pointer to a table of clock operations */
    struct clock_operations *clock_ops;

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    /* SCMI Resource Permissions API */
    const struct mod_res_permissions_api *res_perms_api;
#endif
};

static const fwk_id_t mod_scmi_clock_event_id_get_state =
    FWK_ID_EVENT_INIT(FWK_MODULE_IDX_SCMI_CLOCK,
                      SCMI_CLOCK_EVENT_IDX_GET_STATE);

static const fwk_id_t mod_scmi_clock_event_id_get_rate =
    FWK_ID_EVENT_INIT(FWK_MODULE_IDX_SCMI_CLOCK,
                      SCMI_CLOCK_EVENT_IDX_GET_RATE);

static const fwk_id_t mod_scmi_clock_event_id_set_rate =
    FWK_ID_EVENT_INIT(FWK_MODULE_IDX_SCMI_CLOCK,
                      SCMI_CLOCK_EVENT_IDX_SET_RATE);

static const fwk_id_t mod_scmi_clock_event_id_set_state =
    FWK_ID_EVENT_INIT(FWK_MODULE_IDX_SCMI_CLOCK,
                      SCMI_CLOCK_EVENT_IDX_SET_STATE);

/*
 * SCMI Clock Message Handlers
 */
static int scmi_clock_protocol_version_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_clock_protocol_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_clock_protocol_message_attributes_handler(
    fwk_id_t service_id, const uint32_t *payload);
static int scmi_clock_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_clock_rate_get_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_clock_rate_set_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_clock_config_set_handler(fwk_id_t service_id,
    const uint32_t *payload);
static int scmi_clock_describe_rates_handler(fwk_id_t service_id,
    const uint32_t *payload);

/*
 * Internal variables.
 */
static struct scmi_clock_ctx scmi_clock_ctx;

static int (*const handler_table[])(fwk_id_t, const uint32_t *) = {
    [MOD_SCMI_PROTOCOL_VERSION] = scmi_clock_protocol_version_handler,
    [MOD_SCMI_PROTOCOL_ATTRIBUTES] = scmi_clock_protocol_attributes_handler,
    [MOD_SCMI_PROTOCOL_MESSAGE_ATTRIBUTES] =
        scmi_clock_protocol_message_attributes_handler,
    [MOD_SCMI_CLOCK_ATTRIBUTES] = scmi_clock_attributes_handler,
    [MOD_SCMI_CLOCK_RATE_GET] = scmi_clock_rate_get_handler,
    [MOD_SCMI_CLOCK_RATE_SET] = scmi_clock_rate_set_handler,
    [MOD_SCMI_CLOCK_CONFIG_SET] = scmi_clock_config_set_handler,
    [MOD_SCMI_CLOCK_DESCRIBE_RATES] = scmi_clock_describe_rates_handler,
};

static const unsigned int payload_size_table[] = {
    [MOD_SCMI_PROTOCOL_VERSION] = 0,
    [MOD_SCMI_PROTOCOL_ATTRIBUTES] = 0,
    [MOD_SCMI_PROTOCOL_MESSAGE_ATTRIBUTES] =
        sizeof(struct scmi_protocol_message_attributes_a2p),
    [MOD_SCMI_CLOCK_ATTRIBUTES] = sizeof(struct scmi_clock_attributes_a2p),
    [MOD_SCMI_CLOCK_RATE_GET] = sizeof(struct scmi_clock_rate_get_a2p),
    [MOD_SCMI_CLOCK_RATE_SET] = sizeof(struct scmi_clock_rate_set_a2p),
    [MOD_SCMI_CLOCK_CONFIG_SET] = sizeof(struct scmi_clock_config_set_a2p),
    [MOD_SCMI_CLOCK_DESCRIBE_RATES] =
        sizeof(struct scmi_clock_describe_rates_a2p),
};

/*
 * Given a service identifier, retrieve a pointer to its agent's
 * \c mod_scmi_clock_agent structure within the agent table.
 */
static int get_agent_entry(
    fwk_id_t service_id,
    const struct mod_scmi_clock_agent **agent)
{
    int status;
    unsigned int agent_id;

    if (agent == NULL) {
        return FWK_E_PARAM;
    }

    status = scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        return status;
    }

    if (agent_id >= scmi_clock_ctx.config->agent_count) {
        return FWK_E_PARAM;
    }

    *agent =
        (struct mod_scmi_clock_agent *)&scmi_clock_ctx.agent_table[agent_id];

    return FWK_SUCCESS;
}

/*
 * Given a service identifier and a clock index, retrieve a pointer to the
 * clock's \c mod_scmi_clock_device structure within the agent's device table.
 * Optionally, a pointer to the agent may be retrieved as well.
 */
static int get_clock_device_entry(
    fwk_id_t service_id,
    unsigned int clock_idx,
    const struct mod_scmi_clock_device **clock_device,
    const struct mod_scmi_clock_agent **agent)
{
    int status;
    const struct mod_scmi_clock_agent *agent_entry;

    if (clock_device == NULL) {
        return FWK_E_PARAM;
    }

    status = get_agent_entry(service_id, &agent_entry);
    if (status != FWK_SUCCESS) {
        return status;
    }

    if (clock_idx >= agent_entry->device_count) {
        return FWK_E_RANGE;
    }

    *clock_device = &agent_entry->device_table[clock_idx];

    fwk_assert(fwk_module_is_valid_element_id((*clock_device)->element_id));

    if (agent != NULL) {
        *agent = agent_entry;
    }

    return FWK_SUCCESS;
}

/*
 * Given a service identifier and a clock index, retrieve the state
 * for that agent:clock
 */
static int get_agent_clock_state(
    uint8_t *state,
    fwk_id_t service_id,
    unsigned int clock_idx,
    uint8_t *agent_clock_state)
{
    unsigned int agent_id;
    const struct mod_scmi_clock_agent *agent_entry;
    int idx;
    int status;

    status = get_agent_entry(service_id, &agent_entry);
    if (status != FWK_SUCCESS) {
        return status;
    }

    if (clock_idx >= agent_entry->device_count) {
        return FWK_E_RANGE;
    }

    status = scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        return status;
    }

    idx = (agent_id * scmi_clock_ctx.clock_devices) + clock_idx;

    *state = agent_clock_state[idx];

    return FWK_SUCCESS;
}

/*
 * Given a service identifier and a clock index, set the state
 * for that agent:clock
 */
static int set_agent_clock_state(
    uint8_t *agent_clock_state,
    uint8_t state,
    fwk_id_t service_id,
    unsigned int clock_idx)
{
    unsigned int agent_id;
    const struct mod_scmi_clock_agent *agent_entry;
    int idx;
    int status;

    status = get_agent_entry(service_id, &agent_entry);
    if (status != FWK_SUCCESS) {
        return status;
    }

    if (clock_idx >= agent_entry->device_count) {
        return FWK_E_RANGE;
    }

    status = scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        return status;
    }

    idx = (agent_id * scmi_clock_ctx.clock_devices) + clock_idx;

    agent_clock_state[idx] = state;

    return FWK_SUCCESS;
}

/*
 * Helper for clock operations
 */
static void clock_ops_set_busy(
    unsigned int clock_dev_idx,
    fwk_id_t service_id,
    uint32_t clock_dev_id,
    enum mod_clock_state state,
    enum scmi_clock_request_type request)
{
    scmi_clock_ctx.clock_ops[clock_dev_idx].service_id = service_id;
    scmi_clock_ctx.clock_ops[clock_dev_idx].state = state;
    scmi_clock_ctx.clock_ops[clock_dev_idx].clock_dev_id = clock_dev_id;
    scmi_clock_ctx.clock_ops[clock_dev_idx].request = request;
}

static void clock_ops_update_state(unsigned int clock_dev_idx, int status)
{
    enum mod_scmi_clock_policy_status policy_status;

    if ((status == FWK_SUCCESS) &&
        (scmi_clock_ctx.clock_ops[clock_dev_idx].request ==
         SCMI_CLOCK_REQUEST_SET_STATE)) {
        mod_scmi_clock_config_set_policy(
            &policy_status,
            &scmi_clock_ctx.clock_ops[clock_dev_idx].state,
            MOD_SCMI_CLOCK_POST_MESSAGE_HANDLER,
            scmi_clock_ctx.clock_ops[clock_dev_idx].service_id,
            scmi_clock_ctx.clock_ops[clock_dev_idx].clock_dev_id);
    }
}

static inline void clock_ops_set_available(unsigned int clock_dev_idx)
{
    scmi_clock_ctx.clock_ops[clock_dev_idx].service_id = FWK_ID_NONE;
}

static inline fwk_id_t clock_ops_get_service(unsigned int clock_dev_idx)
{
    return scmi_clock_ctx.clock_ops[clock_dev_idx].service_id;
}

static inline bool clock_ops_is_available(unsigned int clock_dev_idx)
{
    return fwk_id_is_equal(scmi_clock_ctx.clock_ops[clock_dev_idx].service_id,
                           FWK_ID_NONE);
}

/*
 * Helper for the 'get_state' response
 */
static void get_state_respond(fwk_id_t clock_dev_id,
                              fwk_id_t service_id,
                              enum mod_clock_state *clock_state,
                              int status)
{
    size_t response_size;
    struct scmi_clock_attributes_p2a return_values = { 0 };

    if (status == FWK_SUCCESS) {
        return_values.attributes = SCMI_CLOCK_ATTRIBUTES(
            (uint32_t)(*clock_state == MOD_CLOCK_STATE_RUNNING));

        strncpy(return_values.clock_name,
                fwk_module_get_name(clock_dev_id),
                sizeof(return_values.clock_name) - 1);

        return_values.status = SCMI_SUCCESS;
        response_size = sizeof(return_values);
    } else {
        return_values.status = SCMI_GENERIC_ERROR;

        response_size = sizeof(return_values.status);
    }

    scmi_clock_ctx.scmi_api->respond(service_id,
                                     &return_values,
                                     response_size);
}

/*
 * Helper for the 'get_rate' response
 */
static void get_rate_respond(fwk_id_t service_id,
                             uint64_t *rate,
                             int status)
{
    size_t response_size;
    struct scmi_clock_rate_get_p2a return_values = { 0 };

    if (status == FWK_SUCCESS) {
        return_values.rate[0] = (uint32_t)*rate;
        return_values.rate[1] = (uint32_t)(*rate >> 32);

        return_values.status = SCMI_SUCCESS;
        response_size = sizeof(return_values);
    } else {
        return_values.status = SCMI_GENERIC_ERROR;

        response_size = sizeof(return_values.status);
    }

    scmi_clock_ctx.scmi_api->respond(service_id,
                                     &return_values,
                                     response_size);
}

static void request_response(int response_status,
                             fwk_id_t service_id)
{
    struct scmi_clock_generic_p2a return_values = {
        .status = SCMI_GENERIC_ERROR
    };

    if (response_status == FWK_E_SUPPORT) {
        return_values.status = SCMI_NOT_SUPPORTED;
    }

    if (response_status == FWK_E_RANGE) {
        return_values.status = SCMI_INVALID_PARAMETERS;
    }

    scmi_clock_ctx.scmi_api->respond(service_id,
                                     &return_values,
                                     sizeof(return_values.status));
}

/*
 * Helper for the 'set_' responses
 */
static void set_request_respond(fwk_id_t service_id, int status)
{
    struct scmi_clock_generic_p2a return_values = { 0 };

    if (status == FWK_E_RANGE || status == FWK_E_PARAM) {
        return_values.status = SCMI_INVALID_PARAMETERS;
    } else if (status == FWK_E_SUPPORT) {
        return_values.status = SCMI_NOT_SUPPORTED;
    } else if (status != FWK_SUCCESS) {
        return_values.status = SCMI_GENERIC_ERROR;
    } else {
        return_values.status = SCMI_SUCCESS;
    }

    scmi_clock_ctx.scmi_api->respond(service_id,
                                     &return_values,
                                     sizeof(return_values.status));
}

FWK_WEAK int mod_scmi_clock_rate_set_policy(
    enum mod_scmi_clock_policy_status *policy_status,
    enum mod_clock_round_mode *round_mode,
    uint64_t *rate,
    enum mod_scmi_clock_policy_commit policy_commit,
    fwk_id_t service_id,
    uint32_t clock_dev_id)
{
    *policy_status = MOD_SCMI_CLOCK_EXECUTE_MESSAGE_HANDLER;

    return FWK_SUCCESS;
}

FWK_WEAK int mod_scmi_clock_config_set_policy(
    enum mod_scmi_clock_policy_status *policy_status,
    enum mod_clock_state *state,
    enum mod_scmi_clock_policy_commit policy_commit,
    fwk_id_t service_id,
    uint32_t clock_dev_id)
{
    /* Pointer to a table of clock reference counts */
    static uint8_t *clock_count = NULL;
    /* Pointer to a table of agent:clock states */
    static uint8_t *agent_clock_state = NULL;

    const struct mod_scmi_clock_agent *agent;

    unsigned int agent_id, clock_id, idx;
    int status = FWK_SUCCESS;
    uint8_t clock_state;

    *policy_status = MOD_SCMI_CLOCK_SKIP_MESSAGE_HANDLER;

    if (agent_clock_state == NULL) {
        /* Allocate table of agent:clock states */
        agent_clock_state = fwk_mm_calloc(
            (unsigned int)scmi_clock_ctx.config->agent_count *
                (unsigned int)scmi_clock_ctx.clock_devices,
            sizeof(uint8_t));

        /* Allocate table of clock reference counts */
        clock_count = fwk_mm_calloc(
            (unsigned int)scmi_clock_ctx.clock_devices, sizeof(uint32_t));

        /* Set all clocks as running if required */
        for (agent_id = 0;
             agent_id < (unsigned int)scmi_clock_ctx.config->agent_count;
             agent_id++) {
            agent = &scmi_clock_ctx.agent_table[agent_id];
            for (clock_id = 0; clock_id < (unsigned int)agent->device_count;
                 clock_id++) {
                if (agent->device_table[clock_id].starts_enabled) {
                    idx = (agent_id * scmi_clock_ctx.clock_devices) + clock_id;
                    agent_clock_state[idx] = MOD_CLOCK_STATE_RUNNING;

                    clock_count[clock_id]++;
                }
            }
        }
    }

    /*
     * clock state is the last state this agent successfully set
     * for this clock.
     */
    status = get_agent_clock_state(
        &clock_state, service_id, clock_dev_id, agent_clock_state);
    if (status != FWK_SUCCESS) {
        return status;
    }

    switch (*state) {
    case MOD_CLOCK_STATE_RUNNING:
        /* The agent has already requested to set state to RUNNING */
        if (clock_state == MOD_CLOCK_STATE_RUNNING) {
            return FWK_SUCCESS;
        }

        /* set the clock state for this agent to RUNNING */
        if (policy_commit == MOD_SCMI_CLOCK_POST_MESSAGE_HANDLER) {
            set_agent_clock_state(
                agent_clock_state,
                MOD_CLOCK_STATE_RUNNING,
                service_id,
                clock_dev_id);
        }

        /*
         * Only allow the clock to be stopped if clock_count == 0
         * before being updated for this call.
         *
         * This is the first agent to set the clock RUNNING.
         */
        if (clock_count[clock_dev_id] != 0) {
            status = FWK_E_STATE;
        }
        if (policy_commit == MOD_SCMI_CLOCK_POST_MESSAGE_HANDLER) {
            clock_count[clock_dev_id]++;
        }
        break;

    case MOD_CLOCK_STATE_STOPPED:
        /* The agent has already requested to set state to STOPPED */
        if (clock_state == MOD_CLOCK_STATE_STOPPED) {
            return FWK_SUCCESS;
        }

        /* error to try and stop a stopped clock */
        if (clock_count[clock_dev_id] == 0) {
            scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
            FWK_LOG_WARN(
                "[SCMI-CLK] Invalid STOP request agent:"
                " %d clock_id: %d state:%d\n",
                (int)agent_id,
                (int)clock_dev_id,
                *state);
            return FWK_E_STATE;
        }

        /* set the clock state for this agent to STOPPED */
        if (policy_commit == MOD_SCMI_CLOCK_POST_MESSAGE_HANDLER) {
            set_agent_clock_state(
                agent_clock_state,
                MOD_CLOCK_STATE_STOPPED,
                service_id,
                clock_dev_id);
        }

        /*
         * Only allow the clock to be stopped if ref_clk_count == 0
         * after being updated for this call.
         *
         * This is the last agent to set the clock STOPPED.
         */
        if (clock_count[clock_dev_id] != 1) {
            status = FWK_E_STATE;
        }
        if (policy_commit == MOD_SCMI_CLOCK_POST_MESSAGE_HANDLER) {
            clock_count[clock_dev_id]--;
        }
        break;

    default:
        return FWK_E_PARAM;
    }

    if (status == FWK_SUCCESS) {
        *policy_status = MOD_SCMI_CLOCK_EXECUTE_MESSAGE_HANDLER;
    }

    return FWK_SUCCESS;
}

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS

/*
 * SCMI Resource Permissions handler
 */
static uint32_t get_clock_id(const uint32_t *payload, unsigned int message_id)
{
    uint32_t clock_id;

    /*
     * Every SCMI Clock message - but CLOCK_RATE_SET - is formatted with the
     * clock ID as the first message element. We will use the clock_attributes
     * message as a basic format to retrieve the clock ID to avoid
     * unnecessary code.
     */
    switch (message_id) {
    case MOD_SCMI_CLOCK_RATE_SET: {
        const struct scmi_clock_rate_set_a2p *parameters =
            (const struct scmi_clock_rate_set_a2p *)payload;

        clock_id = parameters->clock_id;
    } break;

    default: {
        const struct scmi_clock_attributes_a2p *parameters =
            (const struct scmi_clock_attributes_a2p *)payload;

        clock_id = parameters->clock_id;
    } break;
    }

    return clock_id;
}

static int scmi_clock_permissions_handler(
    fwk_id_t service_id,
    const uint32_t *payload,
    size_t payload_size,
    unsigned int message_id)
{
    enum mod_res_perms_permissions perms;
    unsigned int agent_id;
    uint32_t clock_id;
    int status;

    status = scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        return FWK_E_ACCESS;
    }

    if (message_id < 3) {
        perms = scmi_clock_ctx.res_perms_api->agent_has_protocol_permission(
            agent_id, MOD_SCMI_PROTOCOL_ID_CLOCK);
        if (perms == MOD_RES_PERMS_ACCESS_ALLOWED) {
            return FWK_SUCCESS;
        }
        return FWK_E_ACCESS;
    }

    clock_id = get_clock_id(payload, message_id);

    perms = scmi_clock_ctx.res_perms_api->agent_has_resource_permission(
        agent_id, MOD_SCMI_PROTOCOL_ID_CLOCK, message_id, clock_id);

    if (perms == MOD_RES_PERMS_ACCESS_ALLOWED) {
        return FWK_SUCCESS;
    } else {
        return FWK_E_ACCESS;
    }
}

#endif

/*
 * Helper to create events for processing pending requests
 */
static int create_event_request(
    fwk_id_t clock_id,
    fwk_id_t service_id,
    enum scmi_clock_request_type request,
    void *data,
    uint32_t clock_idx)
{
    int status;
    union event_request_data request_data;
    unsigned int clock_dev_idx = fwk_id_get_element_idx(clock_id);
    struct event_request_params *params;
    enum mod_clock_state state = MOD_CLOCK_STATE_COUNT;

    if (!clock_ops_is_available(clock_dev_idx)) {
        return FWK_E_BUSY;
    }

    struct fwk_event event = {
        .target_id = fwk_module_id_scmi_clock,
    };

    params = (struct event_request_params *)event.params;

    switch (request) {
    case SCMI_CLOCK_REQUEST_GET_STATE:
        event.id = mod_scmi_clock_event_id_get_state;
        break;

    case SCMI_CLOCK_REQUEST_GET_RATE:
        event.id = mod_scmi_clock_event_id_get_rate;
        break;

    case SCMI_CLOCK_REQUEST_SET_RATE:
        {
        struct event_set_rate_request_data *rate_data =
            (struct event_set_rate_request_data *)data;

        request_data.set_rate_data.rate[0] = rate_data->rate[0];
        request_data.set_rate_data.rate[1] = rate_data->rate[1];
        request_data.set_rate_data.round_mode = rate_data->round_mode;

        params->request_data = request_data;
        }

        event.id = mod_scmi_clock_event_id_set_rate;
        break;

    case SCMI_CLOCK_REQUEST_SET_STATE:
        {
        struct event_set_state_request_data *state_data =
            (struct event_set_state_request_data *)data;
        request_data.set_state_data.state = state_data->state;
        state = state_data->state;

        params->request_data = request_data;
        }

        event.id = mod_scmi_clock_event_id_set_state;
        break;

    default:
        return FWK_E_PARAM;
    }

    params->clock_dev_id = clock_id;

    status = fwk_thread_put_event(&event);
    if (status != FWK_SUCCESS) {
        return status;
    }

    clock_ops_set_busy(clock_dev_idx, service_id, clock_idx, state, request);

    return FWK_SUCCESS;
}

/*
 * Protocol Version
 */
static int scmi_clock_protocol_version_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    struct scmi_protocol_version_p2a return_values = {
        .status = SCMI_SUCCESS,
        .version = SCMI_PROTOCOL_VERSION_CLOCK,
    };

    scmi_clock_ctx.scmi_api->respond(service_id, &return_values,
                                     sizeof(return_values));
    return FWK_SUCCESS;
}

/*
 * Protocol Attributes
 */
static int scmi_clock_protocol_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    int status;
    const struct mod_scmi_clock_agent *agent;
    struct scmi_protocol_attributes_p2a return_values = {
        .status = SCMI_SUCCESS,
    };

    status = get_agent_entry(service_id, &agent);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_GENERIC_ERROR;
        goto exit;
    }

    return_values.attributes = SCMI_CLOCK_PROTOCOL_ATTRIBUTES(
            scmi_clock_ctx.max_pending_transactions,
            agent->device_count
        );

exit:
    scmi_clock_ctx.scmi_api->respond(service_id, &return_values,
                                     sizeof(return_values));
    return status;
}

/*
 * Protocol Message Attributes
 */
static int scmi_clock_protocol_message_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    size_t response_size;
    const struct scmi_protocol_message_attributes_a2p *parameters;
    unsigned int message_id;
    struct scmi_protocol_message_attributes_p2a return_values = {
        .status = SCMI_SUCCESS,
        .attributes = 0,
    };

    parameters = (const struct scmi_protocol_message_attributes_a2p*)
        payload;
    message_id = parameters->message_id;

    if ((message_id >= FWK_ARRAY_SIZE(handler_table)) ||
        (handler_table[message_id] == NULL)) {
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

exit:
    response_size = (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status);
    scmi_clock_ctx.scmi_api->respond(service_id, &return_values, response_size);
    return FWK_SUCCESS;
}

/*
 * Clock Attributes
 */

static int scmi_clock_attributes_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    int status;
    const struct mod_scmi_clock_agent *agent;
    const struct mod_scmi_clock_device *clock_device;
    size_t response_size;
    const struct scmi_clock_attributes_a2p *parameters;
    struct scmi_clock_attributes_p2a return_values = {
        .status = SCMI_GENERIC_ERROR
    };

    parameters = (const struct scmi_clock_attributes_a2p*)payload;

    status = get_clock_device_entry(service_id,
                                    parameters->clock_id,
                                    &clock_device,
                                    &agent);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    status = create_event_request(
        clock_device->element_id,
        service_id,
        SCMI_CLOCK_REQUEST_GET_STATE,
        NULL,
        parameters->clock_id);
    if (status == FWK_E_BUSY) {
        return_values.status = SCMI_BUSY;
        status = FWK_SUCCESS;
        goto exit;
    }

    if (status != FWK_SUCCESS) {
        goto exit;
    }

    return FWK_SUCCESS;

exit:
    response_size = (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status);
    scmi_clock_ctx.scmi_api->respond(service_id, &return_values, response_size);
    return status;
}

/*
 * Clock Rate Get
 */
static int scmi_clock_rate_get_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    int status;
    const struct mod_scmi_clock_agent *agent;
    const struct mod_scmi_clock_device *clock_device;
    size_t response_size;
    const struct scmi_clock_rate_get_a2p *parameters;
    struct scmi_clock_rate_get_p2a return_values = {
        .status = SCMI_GENERIC_ERROR
    };

    parameters = (const struct scmi_clock_rate_get_a2p*)payload;

    status = get_clock_device_entry(service_id,
                                    parameters->clock_id,
                                    &clock_device,
                                    &agent);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    status = create_event_request(
        clock_device->element_id,
        service_id,
        SCMI_CLOCK_REQUEST_GET_RATE,
        NULL,
        parameters->clock_id);
    if (status == FWK_E_BUSY) {
        return_values.status = SCMI_BUSY;
        status = FWK_SUCCESS;
        goto exit;
    }

    if (status != FWK_SUCCESS) {
        goto exit;
    }

    return FWK_SUCCESS;

exit:
    response_size = (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status);
    scmi_clock_ctx.scmi_api->respond(service_id, &return_values, response_size);
    return status;
}

/*
 * Clock Rate Set (Synchronous Only)
 */
static int scmi_clock_rate_set_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    int status;
    const struct mod_scmi_clock_agent *agent;
    const struct mod_scmi_clock_device *clock_device;
    bool round_auto;
    bool round_up;
    bool asynchronous;
    size_t response_size;
    unsigned int agent_id;
    uint64_t rate;
    enum mod_clock_round_mode round_mode;
    const struct scmi_clock_rate_set_a2p *parameters;
    struct scmi_clock_rate_set_p2a return_values = {
        .status = SCMI_GENERIC_ERROR
    };
    enum mod_scmi_clock_policy_status policy_status;

    parameters = (const struct scmi_clock_rate_set_a2p*)payload;
    round_up = parameters->flags & SCMI_CLOCK_RATE_SET_ROUND_UP_MASK;
    round_auto = parameters->flags & SCMI_CLOCK_RATE_SET_ROUND_AUTO_MASK;
    asynchronous = parameters->flags & SCMI_CLOCK_RATE_SET_ASYNC_MASK;

    if ((parameters->flags & ~SCMI_CLOCK_RATE_SET_FLAGS_MASK) != 0) {
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    status = get_clock_device_entry(service_id,
                                    parameters->clock_id,
                                    &clock_device,
                                    &agent);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    if (asynchronous) {
        /* Support for async clock set commands not yet implemented */
        return_values.status = SCMI_NOT_SUPPORTED;
        goto exit;
    }

    status = scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        goto exit;
    }

    rate = ((uint64_t)(parameters->rate[1]) << 32) |
        (uint64_t)parameters->rate[0];
    round_mode = round_auto ?
        MOD_CLOCK_ROUND_MODE_NEAREST :
        (round_up ? MOD_CLOCK_ROUND_MODE_UP : MOD_CLOCK_ROUND_MODE_DOWN);

    /*
     * Note that rate and round_mode may be modified by the policy handler.
     */
    status = mod_scmi_clock_rate_set_policy(
        &policy_status,
        &round_mode,
        &rate,
        MOD_SCMI_CLOCK_PRE_MESSAGE_HANDLER,
        service_id,
        parameters->clock_id);

    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_GENERIC_ERROR;
        goto exit;
    }
    if (policy_status == MOD_SCMI_CLOCK_SKIP_MESSAGE_HANDLER) {
        return_values.status = SCMI_SUCCESS;
        goto exit;
    }

    struct event_set_rate_request_data data = {
        .rate[0] = (uint32_t)rate,
        .rate[1] = (uint32_t)(rate >> 32),
        .round_mode = round_mode,
    };

    status = create_event_request(
        clock_device->element_id,
        service_id,
        SCMI_CLOCK_REQUEST_SET_RATE,
        &data,
        parameters->clock_id);
    if (status == FWK_E_BUSY) {
        return_values.status = SCMI_BUSY;
        status = FWK_SUCCESS;
        goto exit;
    }

    if (status != FWK_SUCCESS) {
        goto exit;
    }

    return FWK_SUCCESS;

exit:
    response_size = (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status);
    scmi_clock_ctx.scmi_api->respond(service_id, &return_values, response_size);
    return FWK_SUCCESS;
}

/*
 * Clock Config Set
 */
static int scmi_clock_config_set_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    int status;
    bool enable;
    size_t response_size;
    const struct scmi_clock_config_set_a2p *parameters;
    const struct mod_scmi_clock_agent *agent;
    const struct mod_scmi_clock_device *clock_device;
    unsigned int agent_id;
    struct scmi_clock_rate_set_p2a return_values = {
        .status = SCMI_GENERIC_ERROR
    };
    enum mod_scmi_clock_policy_status policy_status;

    parameters = (const struct scmi_clock_config_set_a2p*)payload;
    enable = parameters->attributes & SCMI_CLOCK_CONFIG_SET_ENABLE_MASK;

    status = get_clock_device_entry(service_id,
                                    parameters->clock_id,
                                    &clock_device,
                                    &agent);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    if ((parameters->attributes & ~SCMI_CLOCK_CONFIG_SET_ENABLE_MASK) != 0) {
        return_values.status = SCMI_INVALID_PARAMETERS;
        goto exit;
    }

    struct event_set_state_request_data data = {
        .state = enable ? MOD_CLOCK_STATE_RUNNING : MOD_CLOCK_STATE_STOPPED
    };

    status = scmi_clock_ctx.scmi_api->get_agent_id(service_id, &agent_id);
    if (status != FWK_SUCCESS) {
        goto exit;
    }

    /*
     * Note that data.state may be modified by the policy handler.
     */
    status = mod_scmi_clock_config_set_policy(
        &policy_status,
        &data.state,
        MOD_SCMI_CLOCK_PRE_MESSAGE_HANDLER,
        service_id,
        parameters->clock_id);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_GENERIC_ERROR;
        goto exit;
    }
    if (policy_status == MOD_SCMI_CLOCK_SKIP_MESSAGE_HANDLER) {
        return_values.status = SCMI_SUCCESS;
        goto exit;
    }

    status = create_event_request(
        clock_device->element_id,
        service_id,
        SCMI_CLOCK_REQUEST_SET_STATE,
        &data,
        parameters->clock_id);
    if (status == FWK_E_BUSY) {
        return_values.status = SCMI_BUSY;
        status = FWK_SUCCESS;
        goto exit;
    }

    if (status != FWK_SUCCESS) {
        goto exit;
    }

    return FWK_SUCCESS;

exit:
    response_size = (return_values.status == SCMI_SUCCESS) ?
        sizeof(return_values) : sizeof(return_values.status);
    scmi_clock_ctx.scmi_api->respond(service_id, &return_values, response_size);
    return FWK_SUCCESS;
}

/*
 * Clock Describe Rates
 */
static int scmi_clock_describe_rates_handler(fwk_id_t service_id,
    const uint32_t *payload)
{
    int status;
    const struct mod_scmi_clock_agent *agent;
    const struct mod_scmi_clock_device *clock_device;
    unsigned int i;
    size_t max_payload_size;
    uint32_t payload_size;
    uint32_t index;
    unsigned int rate_count;
    unsigned int remaining_rates;
    uint64_t rate;
    struct scmi_clock_rate scmi_rate;
    struct scmi_clock_rate clock_range[3];
    struct mod_clock_info info;
    const struct scmi_clock_describe_rates_a2p *parameters;
    struct scmi_clock_describe_rates_p2a return_values = {
        .status = SCMI_GENERIC_ERROR
    };

    parameters = (const struct scmi_clock_describe_rates_a2p*)payload;
    index = parameters->rate_index;
    payload_size = sizeof(return_values);

    status = get_clock_device_entry(service_id,
                                    parameters->clock_id,
                                    &clock_device,
                                    &agent);
    if (status != FWK_SUCCESS) {
        return_values.status = SCMI_NOT_FOUND;
        goto exit;
    }

    /*
     * Get the maximum payload size to determine how many clock rate entries can
     * be returned in one response.
     */
    status = scmi_clock_ctx.scmi_api->get_max_payload_size(
        service_id, &max_payload_size);
    if (status != FWK_SUCCESS) {
        goto exit;
    }

    status = scmi_clock_ctx.clock_api->get_info(clock_device->element_id,
                                                &info);
    if (status != FWK_SUCCESS) {
        goto exit;
    }

    if (info.range.rate_type == MOD_CLOCK_RATE_TYPE_DISCRETE) {
        /* The clock has a discrete list of frequencies */

        if (index >= info.range.rate_count) {
            return_values.status = SCMI_OUT_OF_RANGE;
            goto exit;
        }

        /* Can at least one entry be returned? */
        if (SCMI_CLOCK_RATES_MAX(max_payload_size) == 0) {
            status = FWK_E_SIZE;
            goto exit;
        }

        /* The number of rates being returned in this payload is defined as the
         * smaller of:
         * - The clock rates that are available between the index and the
             clock's maximum rate.
           - The number of rates that can be returned in each payload.
         */
        rate_count = FWK_MIN(SCMI_CLOCK_RATES_MAX(max_payload_size),
            info.range.rate_count - index);

        /*
         * Because the agent gives a starting index into the clock's rate list
         * the number of rates remaining is calculated as the number of rates
         * the clock supports minus the index, with the number of rates being
         * returned in this payload subtracted.
         */
        remaining_rates = (info.range.rate_count - index) - rate_count;

        /* Give the number of rates sent in the message payload */
        return_values.num_rates_flags =
            SCMI_CLOCK_DESCRIBE_RATES_NUM_RATES_FLAGS(
                rate_count,
                SCMI_CLOCK_RATE_FORMAT_LIST,
                remaining_rates
            );

        /* Set each rate entry in the payload to the associated frequency */
        for (i = 0; i < rate_count; i++,
                payload_size += sizeof(struct scmi_clock_rate)) {
            status = scmi_clock_ctx.clock_api->get_rate_from_index(
                clock_device->element_id,
                index + i,
                &rate);
            if (status != FWK_SUCCESS) {
                goto exit;
            }

            scmi_rate.low = (uint32_t)rate;
            scmi_rate.high = (uint32_t)(rate >> 32);

            status = scmi_clock_ctx.scmi_api->write_payload(service_id,
                payload_size, &scmi_rate, sizeof(scmi_rate));
            if (status != FWK_SUCCESS) {
                goto exit;
            }
        }
    } else {
        /* The clock has a linear stepping */

        /* Is the payload area large enough to return the complete triplet? */
        if (SCMI_CLOCK_RATES_MAX(max_payload_size) < 3) {
            status = FWK_E_SIZE;
            goto exit;
        }

        return_values.num_rates_flags =
            SCMI_CLOCK_DESCRIBE_RATES_NUM_RATES_FLAGS(
                /* Only a single rate is returned */
                1U,
                SCMI_CLOCK_RATE_FORMAT_RANGE,
                /* No further rates are available */
                0U);

        /* Store the range data in the range entry in the payload */
        clock_range[0].low = (uint32_t)info.range.min;
        clock_range[0].high = (uint32_t)(info.range.min >> 32);
        clock_range[1].low = (uint32_t)info.range.max;
        clock_range[1].high = (uint32_t)(info.range.max >> 32);
        clock_range[2].low = (uint32_t)info.range.step;
        clock_range[2].high = (uint32_t)(info.range.step >> 32);

        status = scmi_clock_ctx.scmi_api->write_payload(service_id,
            payload_size, &clock_range, sizeof(clock_range));
        if (status != FWK_SUCCESS) {
            goto exit;
        }
        payload_size += sizeof(clock_range);
    }

    return_values.status = SCMI_SUCCESS;
    status = scmi_clock_ctx.scmi_api->write_payload(service_id, 0,
        &return_values, sizeof(return_values));

exit:
    scmi_clock_ctx.scmi_api->respond(service_id,
        (return_values.status == SCMI_SUCCESS) ?
            NULL : &return_values.status,
        (return_values.status == SCMI_SUCCESS) ?
            payload_size : sizeof(return_values.status));
    return status;
}

/*
 * SCMI module -> SCMI clock module interface
 */
static int scmi_clock_get_scmi_protocol_id(fwk_id_t protocol_id,
                                           uint8_t *scmi_protocol_id)
{
    *scmi_protocol_id = MOD_SCMI_PROTOCOL_ID_CLOCK;

    return FWK_SUCCESS;
}

static int scmi_clock_message_handler(fwk_id_t protocol_id, fwk_id_t service_id,
    const uint32_t *payload, size_t payload_size, unsigned int message_id)
{
    int32_t return_value;
#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    int status;
#endif

    static_assert(FWK_ARRAY_SIZE(handler_table) ==
        FWK_ARRAY_SIZE(payload_size_table),
        "[SCMI] Clock management protocol table sizes not consistent");
    fwk_assert(payload != NULL);

    if (message_id >= FWK_ARRAY_SIZE(handler_table)) {
        return_value = SCMI_NOT_FOUND;
        goto error;
    }

    if (payload_size != payload_size_table[message_id]) {
        return_value = SCMI_PROTOCOL_ERROR;
        goto error;
    }

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    status = scmi_clock_permissions_handler(
        service_id, payload, payload_size, message_id);
    if (status != FWK_SUCCESS) {
        return_value = SCMI_DENIED;
        goto error;
    }
#endif

    return handler_table[message_id](service_id, payload);

error:
    scmi_clock_ctx.scmi_api->respond(service_id,
                                     &return_value, sizeof(return_value));
    return FWK_SUCCESS;
}

static struct mod_scmi_to_protocol_api scmi_clock_mod_scmi_to_protocol_api = {
    .get_scmi_protocol_id = scmi_clock_get_scmi_protocol_id,
    .message_handler = scmi_clock_message_handler
};

/*
 * Framework handlers
 */

static int scmi_clock_init(fwk_id_t module_id, unsigned int element_count,
                           const void *data)
{
    int clock_devices;
    const struct mod_scmi_clock_config *config =
        (const struct mod_scmi_clock_config *)data;

    if ((config == NULL) || (config->agent_table == NULL)) {
        return FWK_E_PARAM;
    }

    scmi_clock_ctx.config = config;
    scmi_clock_ctx.max_pending_transactions = config->max_pending_transactions;
    scmi_clock_ctx.agent_table = config->agent_table;

    clock_devices = fwk_module_get_element_count(fwk_module_id_clock);
    if (clock_devices == FWK_E_PARAM) {
        return FWK_E_PANIC;
    }

    scmi_clock_ctx.clock_devices = clock_devices;

    /* Allocate a table of clock operations */
    scmi_clock_ctx.clock_ops =
        fwk_mm_calloc((unsigned int)clock_devices,
        sizeof(struct clock_operations));

    /* Initialize table */
    for (unsigned int i = 0; i < (unsigned int)clock_devices; i++) {
        scmi_clock_ctx.clock_ops[i].service_id = FWK_ID_NONE;
    }

    return FWK_SUCCESS;
}

static int scmi_clock_bind(fwk_id_t id, unsigned int round)
{
    int status;

    if (round == 1) {
        return FWK_SUCCESS;
    }

    status = fwk_module_bind(FWK_ID_MODULE(FWK_MODULE_IDX_SCMI),
        FWK_ID_API(FWK_MODULE_IDX_SCMI, MOD_SCMI_API_IDX_PROTOCOL),
        &scmi_clock_ctx.scmi_api);
    if (status != FWK_SUCCESS) {
        return status;
    }

#ifdef BUILD_HAS_MOD_RESOURCE_PERMS
    status = fwk_module_bind(
        FWK_ID_MODULE(FWK_MODULE_IDX_RESOURCE_PERMS),
        FWK_ID_API(FWK_MODULE_IDX_RESOURCE_PERMS, MOD_RES_PERM_RESOURCE_PERMS),
        &scmi_clock_ctx.res_perms_api);
    if (status != FWK_SUCCESS) {
        return status;
    }
#endif

    return fwk_module_bind(FWK_ID_MODULE(FWK_MODULE_IDX_CLOCK),
        FWK_ID_API(FWK_MODULE_IDX_CLOCK, 0), &scmi_clock_ctx.clock_api);
}

static int scmi_clock_process_bind_request(fwk_id_t source_id,
    fwk_id_t target_id, fwk_id_t api_id, const void **api)
{
    if (!fwk_id_is_equal(source_id, FWK_ID_MODULE(FWK_MODULE_IDX_SCMI))) {
        return FWK_E_ACCESS;
    }

    *api = &scmi_clock_mod_scmi_to_protocol_api;

    return FWK_SUCCESS;
}

static int process_request_event(const struct fwk_event *event)
{
    struct event_request_params *params;
    unsigned int clock_dev_idx;
    int status;
    enum mod_clock_state clock_state;
    uint64_t rate;
    struct event_set_rate_request_data set_rate_data;
    struct event_set_state_request_data set_state_data;
    fwk_id_t service_id;

    params = (struct event_request_params *)event->params;
    clock_dev_idx = fwk_id_get_element_idx(params->clock_dev_id);
    service_id = clock_ops_get_service(clock_dev_idx);

    switch (fwk_id_get_event_idx(event->id)) {
    case SCMI_CLOCK_EVENT_IDX_GET_STATE:
        status = scmi_clock_ctx.clock_api->get_state(params->clock_dev_id,
                                                     &clock_state);
        if (status != FWK_PENDING) {
            /* Request completed */
            get_state_respond(params->clock_dev_id,
                              service_id,
                              &clock_state,
                              status);
        }
        break;

    case SCMI_CLOCK_EVENT_IDX_GET_RATE:
        status = scmi_clock_ctx.clock_api->get_rate(params->clock_dev_id,
                                                    &rate);
        if (status != FWK_PENDING) {
            /* Request completed */
            get_rate_respond(service_id, &rate, status);
        }
        break;

    case SCMI_CLOCK_EVENT_IDX_SET_RATE:
        set_rate_data = params->request_data.set_rate_data;

        rate = (uint64_t)set_rate_data.rate[0] +
               (((uint64_t)set_rate_data.rate[1]) << 32);

        status =
            scmi_clock_ctx.clock_api->set_rate(params->clock_dev_id,
                                               rate,
                                               set_rate_data.round_mode);
        if (status != FWK_PENDING) {
            /* Request completed */
            set_request_respond(service_id, status);
            status = FWK_SUCCESS;
        }
        break;

    case SCMI_CLOCK_EVENT_IDX_SET_STATE:
        set_state_data = params->request_data.set_state_data;

        status = scmi_clock_ctx.clock_api->set_state(params->clock_dev_id,
                                                     set_state_data.state);
        if (status != FWK_PENDING) {
            /* Request completed */
            set_request_respond(service_id, status);
            clock_ops_update_state(clock_dev_idx, status);
            status = FWK_SUCCESS;
        }
        break;

    default:
        return FWK_E_PARAM;
    }

    if (status == FWK_PENDING) {
        return FWK_SUCCESS;
    }

    if (status == FWK_SUCCESS) {
        clock_ops_set_available(clock_dev_idx);
    }

    return status;
}

static int process_response_event(const struct fwk_event *event)
{
    struct mod_clock_resp_params *params =
        (struct mod_clock_resp_params *)event->params;
    unsigned int clock_dev_idx;
    fwk_id_t service_id;
    enum mod_clock_state clock_state;
    uint64_t rate;

    clock_dev_idx = fwk_id_get_element_idx(event->source_id);
    service_id = clock_ops_get_service(clock_dev_idx);

    if (params->status != FWK_SUCCESS) {
        request_response(params->status, service_id);
    } else {
        switch (fwk_id_get_event_idx(event->id)) {
        case MOD_CLOCK_EVENT_IDX_GET_STATE_REQUEST:
            clock_state = params->value.state;

            get_state_respond(event->source_id, service_id, &clock_state,
                FWK_SUCCESS);

            break;

        case MOD_CLOCK_EVENT_IDX_GET_RATE_REQUEST:
            rate = params->value.rate;

            get_rate_respond(service_id, &rate, FWK_SUCCESS);

            break;

        case MOD_CLOCK_EVENT_IDX_SET_RATE_REQUEST:
        case MOD_CLOCK_EVENT_IDX_SET_STATE_REQUEST:
            set_request_respond(service_id, FWK_SUCCESS);
            clock_ops_update_state(clock_dev_idx, FWK_SUCCESS);

            break;

        default:
            return FWK_E_PARAM;
        }
    }
    clock_ops_set_available(clock_dev_idx);

    return FWK_SUCCESS;
}

static int scmi_clock_process_event(const struct fwk_event *event,
                                    struct fwk_event *resp_event)
{
    if (fwk_id_get_module_idx(event->source_id) ==
        fwk_id_get_module_idx(fwk_module_id_scmi)) {
        /* Request events */
        return process_request_event(event);
    }

    if (fwk_id_get_module_idx(event->source_id) ==
        fwk_id_get_module_idx(fwk_module_id_clock)) {
        /* Responses from Clock HAL */
        return process_response_event(event);
    }

    return FWK_E_PARAM;
}

/* SCMI Clock Management Protocol Definition */
const struct fwk_module module_scmi_clock = {
    .name = "SCMI Clock Management Protocol",
    .api_count = 1,
    .event_count = SCMI_CLOCK_EVENT_IDX_COUNT,
    .type = FWK_MODULE_TYPE_PROTOCOL,
    .init = scmi_clock_init,
    .bind = scmi_clock_bind,
    .process_bind_request = scmi_clock_process_bind_request,
    .process_event = scmi_clock_process_event,
};
