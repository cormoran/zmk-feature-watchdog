#pragma once

/**
 * @file watchdog_relay.h
 *
 * @brief Split relay bridge entry points for the `cormoran.watchdog` Studio
 * RPC subsystem, when CONFIG_ZMK_WATCHDOG_SPLIT_RELAY is enabled -- see
 * DESIGN.md SS7 and src/split/watchdog_relay.c.
 *
 * Central-only entry point:
 *  - watchdog_relay_dispatch_request(): relays a GetStatus/ListIncidents/
 *    DeleteIncidents request (whose `source` is nonzero) to the split
 *    peripheral(s)' own local watchdog store. Relaying is inherently
 *    asynchronous (the split link round-trip does not fit the Studio RPC
 *    call/response model), so this always returns immediately with a
 *    DeferredResponse; the real Response for the assigned request_id
 *    arrives later as a PeripheralResponse Studio notification.
 *
 * Everything else in this file (the peripheral-side responder, and the
 * central-side listener that turns a relayed response into a notification)
 * is wired up internally via ZMK_SUBSCRIPTION/ZMK_LISTENER in
 * src/split/watchdog_relay.c and needs no public entry point.
 *
 * Pattern (event struct shape, relay macros, subsystem-index lookup,
 * static-buffer notification encoding, DeferredResponse/PeripheralResponse
 * proto shape) copied from
 * zmk-driver-pmw3610-with-custom-studio-rpc/src/split/pmw3610_relay.c, the
 * reference implementation of this exact bridge for its own RPC surface.
 */

#include <cormoran/watchdog/watchdog.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Relay `req` (a GetStatus/ListIncidents/DeleteIncidents request
 * whose `source` field is nonzero) to the split peripheral(s) and fill
 * `resp` with a DeferredResponse.
 *
 * @param req The decoded request to relay. Its `source` field is not
 *   inspected here (the caller already checked it is nonzero); the
 *   underlying transport broadcasts to every connected peripheral
 *   regardless (see the Kconfig help for CONFIG_ZMK_WATCHDOG_SPLIT_RELAY --
 *   a known v1 limitation, DESIGN.md SS2/SS7).
 * @param resp Always filled with a DeferredResponse (or an ErrorResponse if
 *   encoding/relaying failed outright).
 */
void watchdog_relay_dispatch_request(const cormoran_watchdog_Request *req,
                                     cormoran_watchdog_Response *resp);

#ifdef __cplusplus
}
#endif
