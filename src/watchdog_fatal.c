/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>

#include <cormoran/zmk/watchdog.h>

/*
 * Overrides the weak k_sys_fatal_error_handler() (zephyr/include/zephyr/fatal.h).
 * ZMK itself does not override this symbol (verified against the fork this
 * module targets), so there is no clash here -- but any *other* module or
 * application code that also defines k_sys_fatal_error_handler will silently
 * conflict with this one at link time (last object wins; not a compile
 * error). Document this prominently in the README.
 *
 * This handler runs in fault/ISR context: no flash, no blocking, no
 * threading APIs, no logging subsystem calls beyond LOG_PANIC() (which the
 * generic zephyr/kernel/fatal.c caller already invokes via coredump()/log
 * flushing before reaching here is NOT guaranteed -- keep this file itself
 * free of LOG_* calls to stay safe in the worst case, e.g. a fault that hit
 * while the logging subsystem itself was mid-operation).
 *
 * The record-building logic is factored into watchdog_fatal_build_record()
 * so tests can exercise it directly with a synthetic esf, instead of having
 * to actually fault native_sim (unsafe/unreliable) or re-register the weak
 * symbol.
 */

void watchdog_fatal_build_record(struct zmk_watchdog_incident_record *rec, unsigned int reason,
                                  const struct arch_esf *esf) {
    memset(rec, 0, sizeof(*rec));
    rec->type = ZMK_WATCHDOG_INCIDENT_FATAL;
    rec->uptime_s = (uint32_t)(k_uptime_get() / 1000);
    rec->detail.fatal.reason = (uint32_t)reason;

#if defined(CONFIG_ARM)
    if (esf != NULL) {
        rec->detail.fatal.pc = esf->basic.pc;
        rec->detail.fatal.lr = esf->basic.lr;
    }
#else
    /* Other architectures (including native_sim / ARCH_POSIX, whose
     * arch_esf carries no register info) don't expose a portable PC/LR
     * field on arch_esf -- leave them 0 rather than guess at a layout. */
    ARG_UNUSED(esf);
#endif

    const char *thread_name = NULL;
#if defined(CONFIG_THREAD_NAME)
    thread_name = k_thread_name_get(k_current_get());
#endif
    if (thread_name == NULL || thread_name[0] == '\0') {
        thread_name = "?";
    }
    strncpy(rec->detail.fatal.thread_name, thread_name,
            sizeof(rec->detail.fatal.thread_name) - 1);
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    struct zmk_watchdog_incident_record rec;
    watchdog_fatal_build_record(&rec, reason, esf);

    zmk_watchdog_pending_set(&rec);
    zmk_watchdog_reboot();

    /* zmk_watchdog_reboot() never returns in production (sys_reboot() is
     * noreturn); a test override could in principle return, in which case
     * falling off the end here just means the faulting context is not
     * revived, matching upstream's "never returns" contract closely enough
     * for a test build. */
}
