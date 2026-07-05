/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Incident record shared by the store, the retained-RAM pending slot, and
 * (in a later phase) the Studio RPC / split relay layers. Keep this struct:
 *  - fixed-size and packed (no padding-dependent layout assumptions),
 *  - <= 64 bytes (must fit CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN with
 *    headroom, see DESIGN.md SS6-SS7),
 *  - free of 64-bit fields (nanopb + CONFIG_NANOPB_WITHOUT_64BIT forbids
 *    them once this is exposed over Studio RPC).
 */

#define ZMK_WATCHDOG_RECORD_VERSION 1

/* Incident kind discriminator for zmk_watchdog_incident_record.type. */
enum zmk_watchdog_incident_type {
    ZMK_WATCHDOG_INCIDENT_FREEZE = 0,
    ZMK_WATCHDOG_INCIDENT_FATAL = 1,
    ZMK_WATCHDOG_INCIDENT_RESET_CAUSE = 2,
};

#define ZMK_WATCHDOG_QUEUE_NAME_LEN 16
#define ZMK_WATCHDOG_THREAD_NAME_LEN 16

struct zmk_watchdog_incident_freeze_detail {
    /* task_wdt channel id that fired. */
    uint8_t channel_id;
    uint8_t _reserved[3];
    /* Name of the monitored work queue, NUL-terminated, truncated if needed. */
    char queue_name[ZMK_WATCHDOG_QUEUE_NAME_LEN];
} __packed;

struct zmk_watchdog_incident_fatal_detail {
    /* One of Zephyr's K_ERR_* reason codes (unsigned int truncated to u32). */
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    /* k_thread_name_get() of the faulting thread, "?" if unavailable. */
    char thread_name[ZMK_WATCHDOG_THREAD_NAME_LEN];
} __packed;

struct zmk_watchdog_incident_reset_detail {
    /* Raw hwinfo_get_reset_cause() bit mask. */
    uint32_t cause_bits;
} __packed;

union zmk_watchdog_incident_detail {
    struct zmk_watchdog_incident_freeze_detail freeze;
    struct zmk_watchdog_incident_fatal_detail fatal;
    struct zmk_watchdog_incident_reset_detail reset;
} __packed;

struct zmk_watchdog_incident_record {
    /* Record format version, for forward compatibility. */
    uint8_t version;
    /* enum zmk_watchdog_incident_type. */
    uint8_t type;
    /* Reserved bit flags, currently unused. */
    uint8_t flags;
    uint8_t _reserved;
    /* Monotonically increasing ordinal, incremented only when an incident
     * is actually persisted (see zmk_watchdog_store_append()). Doubles as
     * a relative "which boot" indicator since there is no RTC. */
    uint32_t boot_ordinal;
    /* Uptime (seconds) at incident time. Distinguishes incidents within the
     * same boot_ordinal. */
    uint32_t uptime_s;
    union zmk_watchdog_incident_detail detail;
} __packed;

/* Hard limit: must fit a split relay event payload (DESIGN.md SS6-SS7),
 * i.e. <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN (default 128) with
 * headroom for the relay event envelope. Also keeps well under the
 * "target <= 64 bytes" design goal. */
BUILD_ASSERT(sizeof(struct zmk_watchdog_incident_record) <= 64,
             "zmk_watchdog_incident_record grew past the 64-byte target size");

/*
 * ---------------------------------------------------------------------
 * Store: settings-backed slot store (src/watchdog_store.c).
 *
 * Slots 0..CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS-1, keyed "wdg/i/<slot>" in the
 * Zephyr settings subsystem. append() never overwrites: once every slot is
 * used the incident is dropped (only zmk_watchdog_store_dropped_since_boot()
 * increments) until the user deletes something. No flash write happens on
 * every boot; boot_ordinal bookkeeping piggybacks on the same flash write
 * as the incident it orders (see DESIGN.md SS6).
 * ---------------------------------------------------------------------
 */

/* Append a new incident. Picks the first free slot and persists it (plus
 * the "wdg/meta" boot_ordinal bookkeeping) via the settings subsystem.
 * Fills rec->boot_ordinal with the ordinal actually assigned.
 * Returns 0 if stored, -ENOSPC if the cap was reached (incident dropped,
 * only the dropped-since-boot counter is incremented; no flash write). */
int zmk_watchdog_store_append(struct zmk_watchdog_incident_record *rec);

/* Delete the incident with the given id. Returns 0 on success, -ENOENT if
 * no incident with that id is currently stored. */
int zmk_watchdog_store_delete(uint16_t id);

/* Delete every stored incident and resume recording (clears the
 * recording-stopped condition). Returns 0 on success (including when the
 * store was already empty). */
int zmk_watchdog_store_delete_all(void);

/* Read the stored incident at the given enumeration index (0-based, dense
 * over currently-used slots, order is unspecified but stable between
 * mutations). Returns 0 and fills *out on success, -ENOENT if index is out
 * of range. Intended for RPC listing in a later phase. */
int zmk_watchdog_store_get(uint16_t index, struct zmk_watchdog_incident_record *out);

/* Look up a stored incident by its id. Returns 0 and fills *out on success,
 * -ENOENT if no incident with that id is currently stored. */
int zmk_watchdog_store_get_by_id(uint16_t id, struct zmk_watchdog_incident_record *out);

/* Maximum number of incidents the store can hold (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS). */
uint16_t zmk_watchdog_store_capacity(void);

/* Number of incidents currently stored. */
uint16_t zmk_watchdog_store_count(void);

/* Number of incidents dropped (cap reached) since this boot. Reset to 0 on
 * every boot; never persisted (that would itself cost a flash write). */
uint32_t zmk_watchdog_store_dropped_since_boot(void);

/* True once the store has reached capacity and append() would drop the next
 * incident. Cleared by zmk_watchdog_store_delete()/delete_all() once a slot
 * frees up. */
bool zmk_watchdog_store_recording_stopped(void);

/*
 * ---------------------------------------------------------------------
 * Pending slot: crossing the reboot (src/watchdog_pending.c).
 *
 * A single retained-RAM (__noinit) slot used to carry one incident record
 * across a sys_reboot(). See DESIGN.md SS5.
 * ---------------------------------------------------------------------
 */

/* Fill the pending slot with rec (magic + CRC) so it survives the upcoming
 * reboot. Safe to call from ISR / fatal-handler context: no flash, no
 * locking, no logging. Callers (freeze/fatal detectors, a later phase) are
 * expected to call zmk_watchdog_reboot() right after. */
void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *rec);

/* Boot-time conversion: validate the pending slot's magic+CRC, hand a valid
 * record to the store (append, or count as dropped if the store is full),
 * then zero the slot so it is never reprocessed. No-op (returns 0) if the
 * slot is empty/invalid. Requires the settings subsystem to already be
 * loaded (calls into the store) -- production code must not call this
 * directly from a SYS_INIT level; it is scheduled to run afterwards from
 * the low-priority workqueue. Exposed here so tests can call it directly
 * without relying on that timing. Returns 0 on success (including "nothing
 * pending"), a negative errno if the store append itself failed. */
int zmk_watchdog_pending_convert(void);

/* Test-only helper: flip a bit in the pending slot's stored CRC so the next
 * zmk_watchdog_pending_convert() call takes the "corrupt, discard" path.
 * No-op if no record is pending. Not for production use. */
void zmk_watchdog_pending_corrupt_crc_for_test(void);

/*
 * ---------------------------------------------------------------------
 * Reboot wrapper (src/watchdog_pending.c).
 *
 * Thin wrapper around sys_reboot(SYS_REBOOT_WARM) so native_sim unit tests
 * can intercept it instead of actually tearing down the test process.
 * Production code (freeze/fatal detectors, later phases) must always call
 * this instead of sys_reboot() directly.
 * ---------------------------------------------------------------------
 */

typedef void (*zmk_watchdog_reboot_fn_t)(void);

/* Reboot the device (SYS_REBOOT_WARM), or call the test override installed
 * via zmk_watchdog_reboot_set_override() if one is set. Never returns in
 * production. */
void zmk_watchdog_reboot(void);

/* Test-only hook: replace what zmk_watchdog_reboot() does. Pass NULL to
 * restore the real sys_reboot() behavior. Not for production use. */
void zmk_watchdog_reboot_set_override(zmk_watchdog_reboot_fn_t override);

#ifdef __cplusplus
}
#endif
