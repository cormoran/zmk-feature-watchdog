/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Response size budget (DESIGN.md SS8): the worst case is IncidentPage with
 * CONFIG_ZMK_WATCHDOG_STUDIO_RPC_PAGE_SIZE (4) FatalDetail incidents (the
 * largest detail variant -- reason/pc/lr uint32 fields + a 16-byte
 * thread_name string).
 *
 * Per-Incident encoded size, upper bound:
 *   id, source, type, boot_ordinal, uptime_s (5 uint32 fields)  ~= 5 * 6  = 30
 *   detail oneof submessage tag+len                             ~=        2
 *   FatalDetail: reason,pc,lr (3 uint32 fields)                 ~= 3 * 6  = 18
 *   FatalDetail: thread_name (tag+len+15 chars)                  ~=       18
 *   Incident submessage tag+len (repeated field entry)           ~=        2
 *   ------------------------------------------------------------------
 *   per incident                                                ~=       70
 *
 * 4 incidents                                                    ~=      280
 * IncidentPageResponse.total + start_index (2 uint32 fields)      ~=       12
 * Response oneof wrapper (tag+len)                                ~=        4
 * ------------------------------------------------------------------------
 * total                                                           ~=      296
 *
 * Rounded up with headroom -> comfortably under a 512-byte TX buffer.
 * CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=512 is configured in
 * tests/studio/native_sim.conf and tests/zmk-config/build.yaml.
 */
#define WATCHDOG_RPC_PAGE_SIZE 4
#define WATCHDOG_RPC_ESTIMATED_MAX_RESPONSE_SIZE 296

BUILD_ASSERT(WATCHDOG_RPC_ESTIMATED_MAX_RESPONSE_SIZE + 64 <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
             "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE is too small for a full watchdog IncidentPage "
             "response -- see the arithmetic comment above");

static struct zmk_rpc_custom_subsystem_meta watchdog_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("http://cormoran.github.io/zmk-feature-watchdog/"),
    // Unsecured is suggested by default to avoid unlocking in un-reliable
    // environments.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool watchdog_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                        pb_callback_t *encode_response);

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__watchdog, &watchdog_feature_meta, watchdog_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__watchdog, cormoran_watchdog_Response);

/* ------------------------------------------------------------------------
 * Record -> proto conversion.
 * ---------------------------------------------------------------------- */

static void incident_record_to_proto(uint16_t id, const struct zmk_watchdog_incident_record *rec,
                                     cormoran_watchdog_Incident *out) {
    *out = (cormoran_watchdog_Incident)cormoran_watchdog_Incident_init_zero;

    out->id = id;
    /* Local incidents only in this phase -- a later phase sources this from
     * the split relay (peripheral slot + 1). */
    out->source = 0;
    out->boot_ordinal = rec->boot_ordinal;
    out->uptime_s = rec->uptime_s;

    switch (rec->type) {
    case ZMK_WATCHDOG_INCIDENT_FREEZE:
        out->type = cormoran_watchdog_IncidentType_FREEZE;
        out->which_detail = cormoran_watchdog_Incident_freeze_tag;
        out->detail.freeze.channel_id = rec->detail.freeze.channel_id;
        snprintf(out->detail.freeze.queue_name, sizeof(out->detail.freeze.queue_name), "%s",
                 rec->detail.freeze.queue_name);
        break;
    case ZMK_WATCHDOG_INCIDENT_FATAL:
        out->type = cormoran_watchdog_IncidentType_FATAL;
        out->which_detail = cormoran_watchdog_Incident_fatal_tag;
        out->detail.fatal.reason = rec->detail.fatal.reason;
        out->detail.fatal.pc = rec->detail.fatal.pc;
        out->detail.fatal.lr = rec->detail.fatal.lr;
        snprintf(out->detail.fatal.thread_name, sizeof(out->detail.fatal.thread_name), "%s",
                 rec->detail.fatal.thread_name);
        break;
    case ZMK_WATCHDOG_INCIDENT_RESET_CAUSE:
    default:
        out->type = cormoran_watchdog_IncidentType_RESET_CAUSE;
        out->which_detail = cormoran_watchdog_Incident_reset_tag;
        out->detail.reset.cause_bits = rec->detail.reset.cause_bits;
        break;
    }
}

/* ------------------------------------------------------------------------
 * IncidentRecorded notification: fired whenever zmk_watchdog_store_append()
 * persists a new incident, so the web UI sees new incidents live without
 * polling. The store itself doesn't know about Studio RPC (DESIGN.md SS6-SS7
 * keep it transport-agnostic, since peripherals share the same store code
 * but have no Studio RPC subsystem), so it exposes a generic append callback
 * (zmk_watchdog_store_set_appended_callback(), src/watchdog_store.c) that
 * this file registers at boot instead of the store depending on us.
 *
 * This fires for every append: the boot-time reset-cause audit
 * (src/watchdog_reset_cause.c, appends directly from ordinary boot context)
 * and, after a freeze/fatal-triggered reboot, the *next* boot's pending-slot
 * conversion (src/watchdog_pending.c's zmk_watchdog_pending_convert(), which
 * itself calls zmk_watchdog_store_append()).
 * ---------------------------------------------------------------------- */

static K_MUTEX_DEFINE(notification_buffer_lock);
static cormoran_watchdog_Notification notification_buffer;

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const cormoran_watchdog_Notification *notification =
        (const cormoran_watchdog_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_watchdog_Notification_fields, notification);
}

static int custom_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, "cormoran__watchdog") == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static void on_incident_appended(uint16_t id, const struct zmk_watchdog_incident_record *rec) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return;
    }

    k_mutex_lock(&notification_buffer_lock, K_FOREVER);

    cormoran_watchdog_Notification *notification = &notification_buffer;
    *notification = (cormoran_watchdog_Notification)cormoran_watchdog_Notification_init_zero;
    notification->which_notification_type = cormoran_watchdog_Notification_incident_recorded_tag;
    notification->notification_type.incident_recorded.has_incident = true;
    incident_record_to_proto(id, rec, &notification->notification_type.incident_recorded.incident);

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = notification,
    };

    int ret = raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
    if (ret < 0) {
        LOG_WRN("Failed to raise watchdog IncidentRecorded notification: %d", ret);
    }

    k_mutex_unlock(&notification_buffer_lock);
}

/* Registered at boot (APPLICATION level, after the store's own SYS_INIT --
 * see CMakeLists.txt/Kconfig ordering notes) so every zmk_watchdog_store_append()
 * from this point on -- including the boot-time reset-cause audit
 * (src/watchdog_reset_cause.c) and the *next* boot's pending-slot conversion
 * after a freeze/fatal reboot -- raises a live notification. */
static int watchdog_studio_rpc_init(void) {
    zmk_watchdog_store_set_appended_callback(on_incident_appended);
    return 0;
}

SYS_INIT(watchdog_studio_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ------------------------------------------------------------------------
 * Request handlers.
 * ---------------------------------------------------------------------- */

static void set_error(cormoran_watchdog_Response *resp, const char *message) {
    cormoran_watchdog_ErrorResponse err = cormoran_watchdog_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_watchdog_Response_error_tag;
    resp->response_type.error = err;
}

static int handle_get_status(const cormoran_watchdog_GetStatusRequest *req,
                             cormoran_watchdog_Response *resp) {
    ARG_UNUSED(req);

    cormoran_watchdog_StatusResponse status = cormoran_watchdog_StatusResponse_init_zero;
    status.capacity = zmk_watchdog_store_capacity();
    status.stored = zmk_watchdog_store_count();
    status.dropped_since_boot = zmk_watchdog_store_dropped_since_boot();
    status.recording_stopped = zmk_watchdog_store_recording_stopped();

    resp->which_response_type = cormoran_watchdog_Response_status_tag;
    resp->response_type.status = status;
    return 0;
}

static int handle_list_incidents(const cormoran_watchdog_ListIncidentsRequest *req,
                                 cormoran_watchdog_Response *resp) {
    cormoran_watchdog_IncidentPageResponse page = cormoran_watchdog_IncidentPageResponse_init_zero;

    uint16_t total = zmk_watchdog_store_count();
    uint32_t start = req->start_index;

    page.total = total;
    page.start_index = start;
    page.incidents_count = 0;

    for (uint32_t i = start; i < (uint32_t)total && page.incidents_count < WATCHDOG_RPC_PAGE_SIZE;
         i++) {
        struct zmk_watchdog_incident_record rec;
        uint16_t id;
        int ret = zmk_watchdog_store_get_with_id((uint16_t)i, &rec, &id);
        if (ret < 0) {
            /* Store mutated concurrently (delete) between count() and
             * get_with_id() -- stop the page here rather than erroring the
             * whole request; the client will see a shorter page than
             * `total` implied and can re-request if it cares. */
            break;
        }

        incident_record_to_proto(id, &rec, &page.incidents[page.incidents_count]);
        page.incidents_count++;
    }

    resp->which_response_type = cormoran_watchdog_Response_incident_page_tag;
    resp->response_type.incident_page = page;
    return 0;
}

static int handle_delete_incidents(const cormoran_watchdog_DeleteIncidentsRequest *req,
                                   cormoran_watchdog_Response *resp) {
    uint32_t deleted = 0;

    if (req->all) {
        uint16_t count_before = zmk_watchdog_store_count();
        int ret = zmk_watchdog_store_delete_all();
        if (ret < 0) {
            set_error(resp, "Failed to delete all incidents");
            return 0;
        }
        deleted = count_before;
    } else {
        for (size_t i = 0; i < req->ids_count; i++) {
            uint32_t id = req->ids[i];
            if (id > UINT16_MAX) {
                continue;
            }
            int ret = zmk_watchdog_store_delete((uint16_t)id);
            if (ret == 0) {
                deleted++;
            }
        }
    }

    cormoran_watchdog_DeleteResultResponse result =
        cormoran_watchdog_DeleteResultResponse_init_zero;
    result.deleted = deleted;

    resp->which_response_type = cormoran_watchdog_Response_delete_result_tag;
    resp->response_type.delete_result = result;
    return 0;
}

static bool watchdog_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                        pb_callback_t *encode_response) {
    cormoran_watchdog_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__watchdog, encode_response);

    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_watchdog_Request_fields, &req)) {
        LOG_WRN("Failed to decode watchdog request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case cormoran_watchdog_Request_get_status_tag:
        rc = handle_get_status(&req.request_type.get_status, resp);
        break;
    case cormoran_watchdog_Request_list_incidents_tag:
        rc = handle_list_incidents(&req.request_type.list_incidents, resp);
        break;
    case cormoran_watchdog_Request_delete_incidents_tag:
        rc = handle_delete_incidents(&req.request_type.delete_incidents, resp);
        break;
    default:
        LOG_WRN("Unsupported watchdog request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        set_error(resp, "Failed to process request");
    }
    return true;
}
