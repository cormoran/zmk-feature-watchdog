/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <cormoran/zmk/watchdog.h>

/*
 * Dangerous test/fault injection helpers, gated behind
 * CONFIG_ZMK_WATCHDOG_TEST_INJECTION (default n). These deliberately break
 * the firmware to validate the freeze/fatal detectors end-to-end (native_sim
 * unit tests, hardware bring-up). No RPC is wired to these yet -- a later
 * phase adds request plumbing; for now these are plain C functions a test or
 * a temporary debug hook can call directly. See DESIGN.md SS4.4.
 */

static void inject_freeze_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    /* Never returns: simulates a permanently blocked queue. If this runs on
     * the system workqueue, CONFIG_ZMK_WATCHDOG_FREEZE_DETECT's "sysworkq"
     * feed work (also submitted to the system workqueue) will never run
     * again, and the task_wdt channel will fire once
     * CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS elapses. */
    k_sleep(K_FOREVER);
}

static K_WORK_DEFINE(inject_freeze_work, inject_freeze_work_handler);

void zmk_watchdog_inject_freeze(void) { k_work_submit(&inject_freeze_work); }

void zmk_watchdog_inject_fatal(void) { k_oops(); }
