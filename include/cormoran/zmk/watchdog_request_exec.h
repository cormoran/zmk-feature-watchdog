#pragma once

/**
 * @file watchdog_request_exec.h
 *
 * @brief Transport-independent execution of watchdog Request messages
 * against *this half's own local* incident store
 * (include/cormoran/zmk/watchdog.h), factored out of
 * src/studio/watchdog_handler.c so the exact same GetStatus/ListIncidents/
 * DeleteIncidents logic can be reused by:
 *  - the local Studio RPC handler (CONFIG_ZMK_WATCHDOG_STUDIO_RPC, central/
 *    non-split only, source == 0), and
 *  - the split relay peripheral responder (src/split/watchdog_relay.c,
 *    CONFIG_ZMK_WATCHDOG_SPLIT_RELAY, which a split peripheral -- with no
 *    Studio of its own -- also needs to answer a relayed request against
 *    its own store).
 *
 * Pattern copied from
 * zmk-driver-pmw3610-with-custom-studio-rpc/include/cormoran/pmw3610/pmw3610_request_exec.h.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Converts a raw incident record + its store id into the wire
 * Incident message. `source` is stamped by the caller (0 for a local
 * request; the relaying peripheral's slot for a relayed one) -- this
 * function does not know or care which half it is running on. */
void watchdog_incident_record_to_proto(uint16_t id, uint32_t source,
                                       const struct zmk_watchdog_incident_record *rec,
                                       cormoran_watchdog_Incident *out);

/** @brief Executes `req` (one of GetStatus/ListIncidents/DeleteIncidents/
 * InjectTest) against this half's own local store and fills `resp`.
 * `req->source` (or the equivalent field per request kind) is ignored here --
 * the caller is responsible for routing a nonzero source to the relay
 * instead of calling this function directly (see
 * watchdog_relay_dispatch_request()). InjectTest has no `source` field at
 * all -- test/fault injection only ever targets *this* half, by design (see
 * DESIGN.md SS4.4): a peripheral has no Studio RPC of its own to receive an
 * on-demand injection request, so it uses the boot-delay auto-trigger
 * instead (src/watchdog_inject_autotrigger.c).
 *
 * InjectTestRequest is only honored when CONFIG_ZMK_WATCHDOG_TEST_INJECTION
 * is enabled; otherwise it falls through to the same ErrorResponse path as
 * any other unsupported request kind, so the proto wire format stays stable
 * across test-injection-enabled and disabled builds.
 *
 * Always fills *resp with something (a Status/IncidentPage/DeleteResult/
 * InjectAck on success, an ErrorResponse for an unset/unsupported request
 * kind) -- never returns without a usable response, matching this module's
 * "never crash on a bad request" style.
 */
void watchdog_request_exec_handle(const cormoran_watchdog_Request *req,
                                  cormoran_watchdog_Response *resp);

#ifdef __cplusplus
}
#endif
