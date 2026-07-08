/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/watchdog.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Reset causes that indicate the reboot may have been an *undetected*
 * software/hardware failure (as opposed to a normal power-on, pin reset, or
 * a reboot our own freeze/fatal detectors already explained). See
 * DESIGN.md SS4.3. */
#define WATCHDOG_RESET_CAUSE_OF_INTEREST (RESET_WATCHDOG | RESET_CPU_LOCKUP | RESET_BROWNOUT)

/*
 * Ordering: this audit must run *after* the pending-slot boot conversion
 * (src/watchdog_pending.c) has had a chance to run, so
 * zmk_watchdog_pending_had_incident_this_boot() reflects this boot's
 * outcome. Both are scheduled on the low-priority workqueue from SYS_INIT
 * hooks (Zephyr/ZMK have no "settings loaded" event to hook instead -- see
 * watchdog_pending.c); this work is scheduled with a longer delay than
 * CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS so it always runs strictly
 * after conversion, without needing any direct coupling between the two
 * files beyond the accessor.
 */
#define WATCHDOG_RESET_CAUSE_DELAY_MS (CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS + 100)

static void reset_cause_audit_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t cause = 0;
    int ret = hwinfo_get_reset_cause(&cause);
    if (ret < 0) {
        /* -ENOSYS on native_sim (no hwinfo driver backing this board) and
         * potentially other boards without a hwinfo driver -- nothing to
         * report, not an error. */
        return;
    }

    if ((cause & WATCHDOG_RESET_CAUSE_OF_INTEREST) == 0) {
        /* Clear anyway: nRF-style hwinfo reset cause bits accumulate across
         * resets, so leaving uninteresting bits (e.g. RESET_PIN) set would
         * make them resurface once an interesting bit eventually appears
         * alongside them. */
        hwinfo_clear_reset_cause();
        return;
    }

    if (zmk_watchdog_pending_had_incident_this_boot()) {
        /* A freeze or fatal-error detector already recorded (or attempted
         * to record) an incident that explains this reboot -- a separate
         * RESET_CAUSE incident would just be noise. */
        hwinfo_clear_reset_cause();
        return;
    }

    /* Degraded path: the chip was reset by watchdog/lockup/brownout but no
     * software ran to record why (e.g. a hard lockup that even the
     * task_wdt timer ISR could not survive, so only the hardware watchdog
     * fallback fired). Record what we can: the raw cause bits. No reboot
     * here -- we're just logging what already happened, from ordinary
     * boot-time (non-ISR, settings-ready) context, so this can call the
     * store directly instead of going through the pending-slot dance. */
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
    rec.uptime_s = (uint32_t)(k_uptime_get() / 1000);
    rec.detail.reset.cause_bits = cause;

    int append_ret = zmk_watchdog_store_append(&rec);
    if (append_ret < 0 && append_ret != -ENOSPC) {
        LOG_ERR("Failed to store reset-cause incident: %d", append_ret);
    }

    hwinfo_clear_reset_cause();
}

static K_WORK_DELAYABLE_DEFINE(reset_cause_audit_work, reset_cause_audit_work_handler);

static int watchdog_reset_cause_boot_hook(void) {
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &reset_cause_audit_work,
                              K_MSEC(WATCHDOG_RESET_CAUSE_DELAY_MS));
    return 0;
}

SYS_INIT(watchdog_reset_cause_boot_hook, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
