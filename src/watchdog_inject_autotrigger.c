/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/watchdog.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Boot-delay auto-trigger for CONFIG_ZMK_WATCHDOG_TEST_INJECTION (DESIGN.md
 * SS4.4/SS12): a split peripheral has no Studio RPC of its own to receive an
 * on-demand InjectTestRequest (see src/studio/watchdog_request_exec.c), so
 * hardware-validating its freeze/fatal detectors needs a trigger that does
 * not depend on any host-side tooling at all -- just flash a test build with
 * one of the two Kconfig delays below set, power-cycle, and observe the
 * freeze/reboot/incident-recorded cycle from RTT/serial logs alone.
 *
 * Both delays default to 0 (disabled) and both depend on
 * CONFIG_ZMK_WATCHDOG_TEST_INJECTION (see Kconfig's `if
 * ZMK_WATCHDOG_TEST_INJECTION` block) -- this file is only even compiled in
 * when that's enabled (see CMakeLists.txt), so it is fully inert (no source
 * file, no symbols, no work items scheduled) in any normal build.
 */

#if CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS > 0

static void freeze_autotrigger_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_WRN("watchdog test-injection: auto-triggering a freeze %d ms after boot",
            CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS);
    zmk_watchdog_inject_freeze();
}

static K_WORK_DELAYABLE_DEFINE(freeze_autotrigger_work, freeze_autotrigger_work_handler);

#endif /* CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS > 0 */

#if CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS > 0

static void fatal_autotrigger_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_WRN("watchdog test-injection: auto-triggering a fatal error %d ms after boot",
            CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS);
    zmk_watchdog_inject_fatal();
}

static K_WORK_DELAYABLE_DEFINE(fatal_autotrigger_work, fatal_autotrigger_work_handler);

#endif /* CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS > 0 */

static int watchdog_inject_autotrigger_init(void) {
#if CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS > 0
    /* System workqueue: matches where zmk_watchdog_inject_freeze()'s own
     * injected work item runs, and is always available this early. */
    k_work_schedule(&freeze_autotrigger_work,
                    K_MSEC(CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS));
#endif

#if CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS > 0
    k_work_schedule(&fatal_autotrigger_work,
                    K_MSEC(CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS));
#endif

    return 0;
}

SYS_INIT(watchdog_inject_autotrigger_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
