/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file watchdog_relay.c
 *
 * @brief Split relay bridge for the `cormoran.watchdog` Studio RPC subsystem
 * (CONFIG_ZMK_WATCHDOG_SPLIT_RELAY, see DESIGN.md SS7) -- enabled on BOTH
 * halves of a split keyboard:
 *
 *  - Peripheral role: receives a relayed `RelayRequest` (ZMK split relay
 *    event, identifier "wdq"), executes it against this half's own local
 *    watchdog store via watchdog_request_exec_handle(), and relays a
 *    `RelayResponse` back (identifier "wdp").
 *  - Central role: watchdog_relay_dispatch_request() (called from
 *    src/studio/watchdog_handler.c for any GetStatus/ListIncidents/
 *    DeleteIncidents request whose `source` is nonzero) relays the request
 *    out and immediately returns a DeferredResponse; when the matching
 *    `RelayResponse` relays back in, it is re-raised as a
 *    `PeripheralResponse` Studio notification (the same "custom
 *    notification" mechanism used by IncidentRecorded).
 *
 * Pattern (event struct shape, relay macros, subsystem-index lookup,
 * static-buffer notification encoding, DeferredResponse/PeripheralResponse
 * proto shape) copied from
 * zmk-driver-pmw3610-with-custom-studio-rpc/src/split/pmw3610_relay.c, the
 * reference implementation of this exact bridge for its own RPC surface.
 *
 * Caveat: CONFIG_ZMK_SPLIT_RELAY_EVENT broadcasts a central-to-peripheral
 * relay event to every connected peripheral, not to one addressed
 * peripheral -- so with more than one peripheral, every peripheral executes
 * every relayed request (each correctly tagged with its own source on the
 * way back, via ZMK_RELAY_EVENT_HANDLE's `source_field_name` rewrite). This
 * is fine for the common single-peripheral split; see
 * CONFIG_ZMK_WATCHDOG_SPLIT_RELAY's Kconfig help. Also: there is no way for
 * the central to know which peripherals are currently connected, so a
 * request relayed while a peripheral is disconnected simply never produces
 * a PeripheralResponse for that slot -- the web UI has to time out (see
 * DESIGN.md SS7, "Timeouts").
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>

/* ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL() (invoked below) expands to a call
 * to zmk_split_central_send_relay_event() but -- unlike the peripheral
 * side's ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(), which pulls in
 * <zmk/split/peripheral.h> itself -- event_manager.h does not include
 * <zmk/split/central.h> for its declaration, so callers must. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_relay.h>
#include <cormoran/zmk/watchdog_request_exec.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/studio/custom.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Matches the identifier stringified by ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__watchdog, ...)
 * in watchdog_handler.c -- kept as a separate copy here (not shared via a
 * header) since it is only ever needed as a literal string for the
 * subsystem-index lookup below. */
#define WATCHDOG_SUBSYSTEM_IDENTIFIER_STRING "cormoran__watchdog"

/* nanopb generates a static worst-case encoded size for every message here
 * since every string/repeated field in watchdog.proto has an explicit
 * max_size/max_count (see watchdog.options) -- used to size the relay event
 * payload buffers below exactly, instead of guessing a constant. */
#define WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE cormoran_watchdog_RelayRequest_size
#define WATCHDOG_RELAY_RESPONSE_PAYLOAD_MAX_SIZE cormoran_watchdog_RelayResponse_size

struct zmk_watchdog_relay_request {
    uint8_t source;
    uint16_t size;
    uint8_t payload[WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
};

struct zmk_watchdog_relay_response {
    uint8_t source;
    uint16_t size;
    uint8_t payload[WATCHDOG_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
};

BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_request) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the watchdog relay request "
             "payload -- raise it (see DESIGN.md SS7 / Kconfig help for "
             "ZMK_WATCHDOG_SPLIT_RELAY)");
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_response) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the watchdog relay response "
             "payload -- raise it (see DESIGN.md SS7 / Kconfig help for "
             "ZMK_WATCHDOG_SPLIT_RELAY)");
/* Hard transport ceiling, independent of CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN:
 * struct relay_event_header (zmk/split/transport/types.h) encodes a relayed
 * event's data size in a single `uint8_t event_data_size` wire field, so no
 * relayed event can ever exceed 255 bytes regardless of how high
 * CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is set. If either of watchdog's own
 * relay structs exceeds that, raising the Kconfig value would not help --
 * watchdog.options' IncidentPageResponse.incidents max_count must come down
 * instead. */
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_request) <= 255,
             "the watchdog relay request payload exceeds the split relay transport's 255-byte "
             "hard ceiling (relay_event_header.event_data_size is a uint8_t)");
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_response) <= 255,
             "the watchdog relay response payload exceeds the split relay transport's 255-byte "
             "hard ceiling (relay_event_header.event_data_size is a uint8_t) -- reduce "
             "watchdog.options' IncidentPageResponse.incidents max_count");

ZMK_EVENT_DECLARE(zmk_watchdog_relay_request);
ZMK_EVENT_DECLARE(zmk_watchdog_relay_response);
ZMK_EVENT_IMPL(zmk_watchdog_relay_request);
ZMK_EVENT_IMPL(zmk_watchdog_relay_response);

ZMK_RELAY_EVENT_HANDLE(zmk_watchdog_relay_request, wdq, source);
ZMK_RELAY_EVENT_HANDLE(zmk_watchdog_relay_response, wdp, source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_watchdog_relay_request, wdq, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_watchdog_relay_response, wdp, source);

/* --- Peripheral role: execute a relayed request, relay the response back --- */

/* Decodes `payload`, executes it via watchdog_request_exec_handle() against
 * this half's local store, and fills `out_resp` (always -- an
 * unsupported/undecodable request produces an ErrorResponse, not a
 * function failure, matching this module's existing "never crash on a bad
 * request" style). Exposed as its own step (rather than inlined into the
 * event listener below) so the split-relay self-test can exercise it
 * directly without needing a real relay event. */
static int watchdog_relay_exec_request(const uint8_t *payload, size_t size,
                                       cormoran_watchdog_RelayResponse *out_resp) {
    cormoran_watchdog_RelayRequest relay_req = cormoran_watchdog_RelayRequest_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(payload, size);
    if (!pb_decode(&istream, cormoran_watchdog_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to decode watchdog relay request: %s", PB_GET_ERROR(&istream));
        return -EINVAL;
    }

    *out_resp = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
    out_resp->request_id = relay_req.request_id;
    out_resp->has_response = true;

    if (!relay_req.has_request) {
        cormoran_watchdog_ErrorResponse err = cormoran_watchdog_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "missing relayed watchdog request");
        out_resp->response.which_response_type = cormoran_watchdog_Response_error_tag;
        out_resp->response.response_type.error = err;
    } else {
        watchdog_request_exec_handle(&relay_req.request, &out_resp->response);
    }

    return 0;
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int on_watchdog_relay_request(const zmk_event_t *eh) {
    const struct zmk_watchdog_relay_request *ev = as_zmk_watchdog_relay_request(eh);
    if (!ev) {
        return 0;
    }

    cormoran_watchdog_RelayResponse relay_resp;
    if (watchdog_relay_exec_request(ev->payload, ev->size, &relay_resp) < 0) {
        return 0;
    }

    struct zmk_watchdog_relay_response resp_event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_event.payload, sizeof(resp_event.payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayResponse_fields, &relay_resp)) {
        LOG_WRN("Failed to encode watchdog relay response: %s", PB_GET_ERROR(&ostream));
        return 0;
    }
    resp_event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_watchdog_relay_response(resp_event);
    return 0;
}

ZMK_LISTENER(watchdog_relay_request_exec, on_watchdog_relay_request);
ZMK_SUBSCRIPTION(watchdog_relay_request_exec, zmk_watchdog_relay_request);

#endif // !CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* --- Central role: dispatch a request out, turn a relayed response into a notification --- */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Resolve this subsystem's runtime index, needed to raise a custom Studio
 * notification tagged with the right subsystem_index. Pattern copied from
 * watchdog_handler.c's custom_subsystem_index() (itself copied from
 * custom_settings_handler.c) -- kept as its own copy here since this file
 * may be linked without watchdog_handler.c's internals being exposed. */
static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index) {
    if (!identifier) {
        return -ENOENT;
    }

    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, identifier) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int custom_subsystem_index(void) {
    static int cached_index = -1;
    if (cached_index >= 0) {
        return cached_index;
    }

    uint32_t index;
    int ret = custom_subsystem_index_for_identifier(WATCHDOG_SUBSYSTEM_IDENTIFIER_STRING, &index);
    if (ret < 0) {
        return ret;
    }

    cached_index = (int)index;
    return cached_index;
}

static uint32_t next_relay_request_id = 1;

/* Encodes `req` into a RelayRequest and raises it for CENTRAL_TO_PERIPHERAL
 * relay (see the macro invocations above). Returns the assigned request_id
 * (nonzero) on success, or 0 on encode failure (logged). */
static uint32_t send_relay_request(const cormoran_watchdog_Request *req) {
    uint32_t request_id = next_relay_request_id++;

    cormoran_watchdog_RelayRequest relay_req = cormoran_watchdog_RelayRequest_init_zero;
    relay_req.request_id = request_id;
    relay_req.has_request = true;
    relay_req.request = *req;

    struct zmk_watchdog_relay_request event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to encode watchdog relay request: %s", PB_GET_ERROR(&ostream));
        return 0;
    }
    event.size = (uint16_t)ostream.bytes_written;

    raise_zmk_watchdog_relay_request(event);
    return request_id;
}

void watchdog_relay_dispatch_request(const cormoran_watchdog_Request *req,
                                     cormoran_watchdog_Response *resp) {
    uint32_t request_id = send_relay_request(req);
    if (request_id == 0) {
        cormoran_watchdog_ErrorResponse err = cormoran_watchdog_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "failed to encode relay request");
        resp->which_response_type = cormoran_watchdog_Response_error_tag;
        resp->response_type.error = err;
        return;
    }

    cormoran_watchdog_DeferredResponse deferred = cormoran_watchdog_DeferredResponse_init_zero;
    deferred.request_id = request_id;
    resp->which_response_type = cormoran_watchdog_Response_deferred_tag;
    resp->response_type.deferred = deferred;
}

static K_MUTEX_DEFINE(peripheral_response_notification_lock);
static cormoran_watchdog_Notification peripheral_response_notification;

static bool encode_watchdog_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                                 void *const *arg) {
    const cormoran_watchdog_Notification *notification =
        (const cormoran_watchdog_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_watchdog_Notification_fields, notification);
}

static int raise_watchdog_notification(cormoran_watchdog_Notification *notification) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_watchdog_notification_payload,
        .arg = (void *)notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}

static int on_watchdog_relay_response(const zmk_event_t *eh) {
    const struct zmk_watchdog_relay_response *ev = as_zmk_watchdog_relay_response(eh);
    if (!ev) {
        return 0;
    }

    cormoran_watchdog_RelayResponse relay_resp = cormoran_watchdog_RelayResponse_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(ev->payload, ev->size);
    if (!pb_decode(&istream, cormoran_watchdog_RelayResponse_fields, &relay_resp)) {
        LOG_WRN("Failed to decode watchdog relay response: %s", PB_GET_ERROR(&istream));
        return 0;
    }

    k_mutex_lock(&peripheral_response_notification_lock, K_FOREVER);

    peripheral_response_notification =
        (cormoran_watchdog_Notification)cormoran_watchdog_Notification_init_zero;
    peripheral_response_notification.which_notification_type =
        cormoran_watchdog_Notification_peripheral_response_tag;
    cormoran_watchdog_PeripheralResponse *pr =
        &peripheral_response_notification.notification_type.peripheral_response;
    /* ev->source was rewritten by ZMK_RELAY_EVENT_HANDLE's receive-side
     * `source_field_name = ev->source + 1` to the relaying peripheral's
     * slot + 1 -- exactly the addressing convention this module documents
     * for `source` elsewhere (0 = local/central, N = peripheral slot N). */
    pr->source = ev->source;
    pr->request_id = relay_resp.request_id;
    pr->has_response = relay_resp.has_response;
    if (relay_resp.has_response) {
        pr->response = relay_resp.response;
        /* Every Incident inside an IncidentPageResponse was built by the
         * peripheral's own watchdog_request_exec_handle() with source == 0
         * (see the doc comment in watchdog_request_exec.c) -- stamp the
         * real source now that it is known, mirroring the top-level
         * PeripheralResponse.source convention. */
        if (relay_resp.response.which_response_type ==
            cormoran_watchdog_Response_incident_page_tag) {
            cormoran_watchdog_IncidentPageResponse *page =
                &pr->response.response_type.incident_page;
            for (size_t i = 0; i < page->incidents_count; i++) {
                page->incidents[i].source = ev->source;
            }
        }
    }

    int ret = raise_watchdog_notification(&peripheral_response_notification);
    if (ret) {
        LOG_WRN("Failed to raise watchdog PeripheralResponse notification: %d", ret);
    }

    k_mutex_unlock(&peripheral_response_notification_lock);
    return 0;
}

ZMK_LISTENER(watchdog_relay_response_notify, on_watchdog_relay_response);
ZMK_SUBSCRIPTION(watchdog_relay_response_notify, zmk_watchdog_relay_response);

#endif // CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* --- native_sim-only self-tests: exercise the relay logic without a --- */
/* --- real transport (native_sim cannot simulate one) --- */

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/*
 * Minimal in-RAM settings backend, same pattern as tests/watchdog/'s
 * watchdog_test.c and src/test/watchdog_rpc_test.c: this test build has
 * CONFIG_SETTINGS=y but no backend registered by default, and
 * zmk_watchdog_store_append() calls settings_save_one(), which needs *some*
 * registered destination to succeed.
 */

#define WATCHDOG_RELAY_TEST_SETTINGS_STORAGE_CAPACITY 8

struct watchdog_relay_test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[64];
    size_t len;
};

static struct watchdog_relay_test_settings_record
    watchdog_relay_test_settings_storage[WATCHDOG_RELAY_TEST_SETTINGS_STORAGE_CAPACITY];

static struct watchdog_relay_test_settings_record *
watchdog_relay_test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(watchdog_relay_test_settings_storage); i++) {
        if (watchdog_relay_test_settings_storage[i].present &&
            strncmp(watchdog_relay_test_settings_storage[i].name, name,
                    sizeof(watchdog_relay_test_settings_storage[i].name)) == 0) {
            return &watchdog_relay_test_settings_storage[i];
        }
    }
    return NULL;
}

static ssize_t watchdog_relay_test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct watchdog_relay_test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);
    memcpy(data, record->data, read_len);
    return read_len;
}

static int watchdog_relay_test_settings_load(struct settings_store *cs,
                                             const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(watchdog_relay_test_settings_storage); i++) {
        struct watchdog_relay_test_settings_record *record =
            &watchdog_relay_test_settings_storage[i];
        if (!record->present) {
            continue;
        }
        int ret = settings_call_set_handler(record->name, record->len,
                                            watchdog_relay_test_settings_read_cb, record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static int watchdog_relay_test_settings_save(struct settings_store *cs, const char *name,
                                             const char *value, size_t val_len) {
    ARG_UNUSED(cs);

    struct watchdog_relay_test_settings_record *record =
        watchdog_relay_test_settings_find_record(name);
    if (value == NULL) {
        if (record) {
            record->present = false;
        }
        return 0;
    }

    if (val_len > sizeof(record->data)) {
        return -EMSGSIZE;
    }
    if (strlen(name) >= SETTINGS_MAX_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    if (!record) {
        for (size_t i = 0; i < ARRAY_SIZE(watchdog_relay_test_settings_storage); i++) {
            if (!watchdog_relay_test_settings_storage[i].present) {
                record = &watchdog_relay_test_settings_storage[i];
                break;
            }
        }
    }
    if (!record) {
        return -ENOMEM;
    }

    record->present = true;
    strcpy(record->name, name);
    memcpy(record->data, value, val_len);
    record->len = val_len;
    return 0;
}

static const struct settings_store_itf watchdog_relay_test_settings_itf = {
    .csi_load = watchdog_relay_test_settings_load,
    .csi_save = watchdog_relay_test_settings_save,
};

static struct settings_store watchdog_relay_test_settings_store = {
    .cs_itf = &watchdog_relay_test_settings_itf,
};

static int watchdog_relay_test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&watchdog_relay_test_settings_store);
    settings_dst_register(&watchdog_relay_test_settings_store);
    return 0;
}

static int watchdog_split_relay_test_init(void) {
    int backend_ret = watchdog_relay_test_settings_backend_init();
    if (backend_ret < 0) {
        LOG_ERR("Split relay test: failed to init settings backend: %d", backend_ret);
        return backend_ret;
    }

    /* Populate the local store with one incident so a relayed ListIncidents
     * has something to report -- exercises the same
     * watchdog_request_exec_handle() code path a real relayed request would
     * take. */
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
    rec.uptime_s = 5;
    rec.detail.reset.cause_bits = 0x4;
    if (zmk_watchdog_store_append(&rec) != 0) {
        LOG_ERR("Split relay test: failed to seed local store");
        return -EIO;
    }

    cormoran_watchdog_RelayRequest relay_req = cormoran_watchdog_RelayRequest_init_zero;
    relay_req.request_id = 42;
    relay_req.has_request = true;
    relay_req.request.which_request_type = cormoran_watchdog_Request_list_incidents_tag;
    relay_req.request.request_type.list_incidents.start_index = 0;

    uint8_t payload[WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
    pb_ostream_t ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &relay_req)) {
        LOG_ERR("Split relay test: failed to encode synthetic request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }

    cormoran_watchdog_RelayResponse relay_resp;
    int ret = watchdog_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: exec failed: %d", ret);
        return ret;
    }

    if (relay_resp.request_id != 42) {
        LOG_ERR("Split relay test: request_id mismatch: got %u", relay_resp.request_id);
        return -EINVAL;
    }
    if (relay_resp.response.which_response_type != cormoran_watchdog_Response_incident_page_tag) {
        LOG_ERR("Split relay test: expected an IncidentPage, got response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }
    if (relay_resp.response.response_type.incident_page.incidents_count != 1 ||
        relay_resp.response.response_type.incident_page.incidents[0].source != 0) {
        LOG_ERR("Split relay test: expected 1 incident with source 0 (stamped later by the "
                "central), got count=%u source=%u",
                (unsigned int)relay_resp.response.response_type.incident_page.incidents_count,
                relay_resp.response.response_type.incident_page.incidents[0].source);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_list_incidents\n");

    /* DeleteIncidents relayed the same way. */
    cormoran_watchdog_RelayRequest del_relay_req = cormoran_watchdog_RelayRequest_init_zero;
    del_relay_req.request_id = 43;
    del_relay_req.has_request = true;
    del_relay_req.request.which_request_type = cormoran_watchdog_Request_delete_incidents_tag;
    del_relay_req.request.request_type.delete_incidents.all = true;

    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &del_relay_req)) {
        LOG_ERR("Split relay test: failed to encode delete request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = watchdog_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: delete exec failed: %d", ret);
        return ret;
    }
    if (relay_resp.response.which_response_type != cormoran_watchdog_Response_delete_result_tag ||
        relay_resp.response.response_type.delete_result.deleted != 1) {
        LOG_ERR("Split relay test: expected delete_result.deleted=1, got type=%d deleted=%u",
                relay_resp.response.which_response_type,
                relay_resp.response.response_type.delete_result.deleted);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_delete_incidents\n");

    /* Genuinely unsupported/malformed relayed request (no request_type set
     * at all): watchdog_request_exec_handle() must still produce an
     * ErrorResponse, not a crash or an unfilled response. */
    cormoran_watchdog_RelayRequest empty_relay_req = cormoran_watchdog_RelayRequest_init_zero;
    empty_relay_req.request_id = 44;
    empty_relay_req.has_request = true;
    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &empty_relay_req)) {
        LOG_ERR("Split relay test: failed to encode empty request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = watchdog_relay_exec_request(payload, ostream.bytes_written, &relay_resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: empty-request exec failed: %d", ret);
        return ret;
    }
    if (relay_resp.response.which_response_type != cormoran_watchdog_Response_error_tag) {
        LOG_ERR("Split relay test: expected an ErrorResponse for an unset request kind, got "
                "response type %d",
                relay_resp.response.which_response_type);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_unsupported_kind\n");
    return 0;
}

SYS_INIT(watchdog_split_relay_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST && !CONFIG_ZMK_SPLIT_ROLE_CENTRAL

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Central-side counterpart: asserts watchdog_relay_dispatch_request()
 * assigns distinct, nonzero request_ids and always returns a
 * DeferredResponse for a nonzero-source request. Only reachable from a
 * build test (module_watchdog_split_central in tests/zmk-config/build.yaml),
 * not a native_sim test: raise_zmk_watchdog_relay_request() needs
 * zmk_split_central_send_relay_event(), only implemented by the real BLE
 * split transport (CONFIG_ZMK_SPLIT_BLE) -- native_sim would need a working
 * BT stack to link it. This only proves the central dispatch test code
 * compiles/links and is logically correct in isolation; it does not (and
 * cannot, without a real peripheral) assert that a relay actually reaches
 * anyone -- that is exactly what native_sim cannot simulate. */
static int watchdog_split_relay_central_test_init(void) {
    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_get_status_tag;
    req.request_type.get_status.source = 1;

    cormoran_watchdog_Response resp1;
    watchdog_relay_dispatch_request(&req, &resp1);
    if (resp1.which_response_type != cormoran_watchdog_Response_deferred_tag ||
        resp1.response_type.deferred.request_id == 0) {
        LOG_ERR("Split relay central test: expected a nonzero DeferredResponse, got type=%d "
                "request_id=%u",
                resp1.which_response_type, resp1.response_type.deferred.request_id);
        return -EINVAL;
    }

    cormoran_watchdog_Response resp2;
    watchdog_relay_dispatch_request(&req, &resp2);
    if (resp2.which_response_type != cormoran_watchdog_Response_deferred_tag ||
        resp2.response_type.deferred.request_id == 0 ||
        resp2.response_type.deferred.request_id == resp1.response_type.deferred.request_id) {
        LOG_ERR("Split relay central test: request_ids did not increment (%u, %u)",
                resp1.response_type.deferred.request_id, resp2.response_type.deferred.request_id);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_central_dispatch\n");
    return 0;
}

SYS_INIT(watchdog_split_relay_central_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST && CONFIG_ZMK_SPLIT_ROLE_CENTRAL
