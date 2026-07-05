/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>

#include <cormoran/zmk/watchdog.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WATCHDOG_PENDING_MAGIC 0x57444741u /* 'WDGA' */

struct zmk_watchdog_pending {
    uint32_t magic;
    struct zmk_watchdog_incident_record record;
    uint32_t crc;
};

/* Plain __noinit static: survives a warm sys_reboot() on real hardware
 * (SRAM is not cleared by SYSRESETREQ), but is re-zeroed on every native_sim
 * process start -- tests inject records directly via
 * zmk_watchdog_pending_set() + zmk_watchdog_pending_convert() rather than
 * relying on retained RAM across a process restart. See DESIGN.md SS5. */
static struct zmk_watchdog_pending pending __noinit;

static uint32_t pending_crc(const struct zmk_watchdog_incident_record *rec) {
    return crc32_ieee((const uint8_t *)rec, sizeof(*rec));
}

void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *rec) {
    if (!rec) {
        return;
    }

    /* ISR/fatal-handler safe: no locking, no flash, no logging subsystem
     * calls beyond what the caller already does with LOG_PANIC(). */
    pending.record = *rec;
    pending.crc = pending_crc(&pending.record);
    pending.magic = WATCHDOG_PENDING_MAGIC;
}

static void pending_clear(void) { memset(&pending, 0, sizeof(pending)); }

void zmk_watchdog_pending_corrupt_crc_for_test(void) {
    if (pending.magic != WATCHDOG_PENDING_MAGIC) {
        return;
    }
    pending.crc ^= 0xFFFFFFFFu;
}

int zmk_watchdog_pending_convert(void) {
    if (pending.magic != WATCHDOG_PENDING_MAGIC) {
        return 0;
    }

    if (pending_crc(&pending.record) != pending.crc) {
        LOG_WRN("Watchdog pending slot failed CRC check; discarding");
        pending_clear();
        return 0;
    }

    struct zmk_watchdog_incident_record rec = pending.record;
    /* Zero the slot before handing off to the store so a crash inside
     * store append can never cause the same pending incident to be
     * reprocessed on the next boot. */
    pending_clear();

    int ret = zmk_watchdog_store_append(&rec);
    if (ret < 0 && ret != -ENOSPC) {
        LOG_ERR("Failed to convert pending watchdog incident: %d", ret);
        return ret;
    }

    return 0;
}

/*
 * Boot hook: settings_load() runs from main() *after* every SYS_INIT level
 * (see zmk/app/src/main.c), so a SYS_INIT callback must not call
 * zmk_watchdog_pending_convert() directly -- the store's RAM index would
 * not be populated yet. Instead this SYS_INIT only *schedules* the
 * conversion on the low-priority workqueue with a short delay, which is
 * enough for the kernel to have scheduled main() (settings_load()) at least
 * once given the low-priority thread's priority; this is a pragmatic
 * approach given Zephyr/ZMK expose no "settings loaded" event (verified:
 * no such hook exists anywhere in this tree, see DESIGN.md SS5 discussion).
 */

static void pending_convert_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    zmk_watchdog_pending_convert();
}

static K_WORK_DELAYABLE_DEFINE(pending_convert_work, pending_convert_work_handler);

static int watchdog_pending_boot_hook(void) {
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &pending_convert_work,
                               K_MSEC(CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS));
    return 0;
}

SYS_INIT(watchdog_pending_boot_hook, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* --------------------------------------------------------------------
 * Reboot wrapper: testable indirection around sys_reboot().
 * -------------------------------------------------------------------- */

static zmk_watchdog_reboot_fn_t reboot_override;

void zmk_watchdog_reboot_set_override(zmk_watchdog_reboot_fn_t override) {
    reboot_override = override;
}

void zmk_watchdog_reboot(void) {
    if (reboot_override) {
        reboot_override();
        return;
    }

    sys_reboot(SYS_REBOOT_WARM);
}
