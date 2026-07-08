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
#include <cormoran/zmk/watchdog_request_exec.h>

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
#include <cormoran/zmk/watchdog_relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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
 * Record -> proto conversion now lives in src/studio/watchdog_request_exec.c
 * (watchdog_incident_record_to_proto()), shared with the split relay
 * peripheral responder (src/split/watchdog_relay.c) -- see DESIGN.md SS7.
 * ---------------------------------------------------------------------- */

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
    /* source = 0: this callback only fires for incidents this half's own
     * store just persisted (see the doc comment above). */
    watchdog_incident_record_to_proto(id, 0, rec,
                                      &notification->notification_type.incident_recorded.incident);

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
 * Request handlers: GetStatus/ListIncidents/DeleteIncidents execution
 * against the local store lives in src/studio/watchdog_request_exec.c
 * (watchdog_request_exec_handle()), shared with the split relay peripheral
 * responder. This file only needs to decide *where* a request should run:
 * locally (source == 0) or relayed to a split peripheral (source != 0, see
 * DESIGN.md SS7) -- and, for the latter, only when
 * CONFIG_ZMK_WATCHDOG_SPLIT_RELAY is actually enabled.
 * ---------------------------------------------------------------------- */

static void set_error(cormoran_watchdog_Response *resp, const char *message) {
    cormoran_watchdog_ErrorResponse err = cormoran_watchdog_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_watchdog_Response_error_tag;
    resp->response_type.error = err;
}

/* Returns the `source` field of a GetStatus/ListIncidents/DeleteIncidents
 * request (0 if the request kind is unset/unsupported -- routed to the
 * local executor, which will itself produce an ErrorResponse). InjectTest
 * has no `source` field and always falls into the default case: test/fault
 * injection only ever targets this half (see DESIGN.md SS4.4), so it is
 * always executed locally, never relayed to a split peripheral. */
static uint32_t request_source(const cormoran_watchdog_Request *req) {
    switch (req->which_request_type) {
    case cormoran_watchdog_Request_get_status_tag:
        return req->request_type.get_status.source;
    case cormoran_watchdog_Request_list_incidents_tag:
        return req->request_type.list_incidents.source;
    case cormoran_watchdog_Request_delete_incidents_tag:
        return req->request_type.delete_incidents.source;
    default:
        return 0;
    }
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

    uint32_t source = request_source(&req);

    if (source != 0) {
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
        watchdog_relay_dispatch_request(&req, resp);
#else
        set_error(resp, "Split relay not enabled in this firmware");
#endif
        return true;
    }

    watchdog_request_exec_handle(&req, resp);
    return true;
}
