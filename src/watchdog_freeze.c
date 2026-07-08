/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include <cormoran/zmk/watchdog.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Monitor table: one task_wdt channel per monitored work queue, fed by a
 * self-rescheduling k_work_delayable submitted to that same queue. If the
 * queue is blocked/starved longer than CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS,
 * the feed work never runs and the channel's callback fires (ISR/timer
 * context). See DESIGN.md SS4.1.
 *
 * Data-driven so adding a monitored queue is a one-line change: append an
 * entry to watchdog_freeze_monitors[] below.
 */

struct watchdog_freeze_monitor {
    /* Returns the queue to monitor. Called once at init. */
    struct k_work_q *(*get_queue)(void);
    /* Name recorded in the incident (truncated to
     * ZMK_WATCHDOG_QUEUE_NAME_LEN - 1 if longer). */
    const char *name;

    /* Filled in at init. */
    int channel_id;
    struct k_work_delayable feed_work;
};

static struct k_work_q *get_sys_work_q(void) { return &k_sys_work_q; }

static struct watchdog_freeze_monitor watchdog_freeze_monitors[] = {
    {
        .get_queue = get_sys_work_q,
        .name = "sysworkq",
    },
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_MONITOR_LOWPRIO_QUEUE)
    {
        /* ZMK_LOW_PRIORITY_WORK_QUEUE is select'd by CONFIG_ZMK_WATCHDOG
         * (see Kconfig), so this queue always exists whenever this file is
         * compiled in -- no extra #ifdef needed for the queue itself, only
         * for whether we monitor it (see CONFIG_ZMK_WATCHDOG_FREEZE_MONITOR_
         * LOWPRIO_QUEUE's help: disabling this halves the module's own
         * periodic k_timer footprint). */
        .get_queue = zmk_workqueue_lowprio_work_q,
        .name = "lowprio_workq",
    },
#endif
};

/* task_wdt callback: ISR (timer) context. Must be minimal -- no flash, no
 * locking, no logging subsystem calls beyond what zmk_watchdog_pending_set()
 * already restricts itself to. */
static void watchdog_freeze_channel_fired(int channel_id, void *user_data) {
    struct watchdog_freeze_monitor *mon = user_data;

    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    rec.uptime_s = (uint32_t)(k_uptime_get() / 1000);
    rec.detail.freeze.channel_id = (uint8_t)channel_id;
    strncpy(rec.detail.freeze.queue_name, mon->name, sizeof(rec.detail.freeze.queue_name) - 1);

    zmk_watchdog_pending_set(&rec);
    zmk_watchdog_reboot();
}

static void watchdog_freeze_feed_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct watchdog_freeze_monitor *mon =
        CONTAINER_OF(dwork, struct watchdog_freeze_monitor, feed_work);

    task_wdt_feed(mon->channel_id);

    k_work_reschedule_for_queue(
        mon->get_queue(), &mon->feed_work,
        K_MSEC(CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS / CONFIG_ZMK_WATCHDOG_FREEZE_FEED_DIVISOR));
}

static struct k_work_delayable watchdog_freeze_arm_work;

/* Arms task_wdt and starts the feed timers. Runs on the system workqueue,
 * CONFIG_ZMK_WATCHDOG_FREEZE_STARTUP_DELAY_MS after boot rather than
 * synchronously inside SYS_INIT -- see that option's Kconfig help for why
 * (avoids adding this module's own periodic timer/work activity during the
 * initial BLE connection-establishment window, when the radio schedule is
 * busiest). */
static void watchdog_freeze_arm_work_handler(struct k_work *work) {
    /* Software-only task watchdog: no hardware watchdog peripheral backs
     * this. See DESIGN.md SS4.1 for why a hardware fallback was prototyped
     * and then deliberately removed. */
    int ret = task_wdt_init(NULL);
    if (ret < 0) {
        LOG_ERR("task_wdt_init failed: %d", ret);
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(watchdog_freeze_monitors); i++) {
        struct watchdog_freeze_monitor *mon = &watchdog_freeze_monitors[i];

        int channel_id =
            task_wdt_add(CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS, watchdog_freeze_channel_fired, mon);
        if (channel_id < 0) {
            LOG_ERR("task_wdt_add failed for queue '%s': %d", mon->name, channel_id);
            return;
        }
        mon->channel_id = channel_id;

        k_work_init_delayable(&mon->feed_work, watchdog_freeze_feed_work_handler);
        k_work_schedule_for_queue(
            mon->get_queue(), &mon->feed_work,
            K_MSEC(CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS / CONFIG_ZMK_WATCHDOG_FREEZE_FEED_DIVISOR));

        LOG_INF("Watchdog freeze monitor armed: queue='%s' channel=%d timeout=%dms", mon->name,
                channel_id, CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS);
    }
}

static int watchdog_freeze_init(void) {
    /* Both monitored queues (system workqueue, ZMK low-prio workqueue) are
     * started well before this by their own earlier-level SYS_INIT hooks, so
     * scheduling onto either is safe immediately -- only *arming* is
     * deferred. */
    k_work_init_delayable(&watchdog_freeze_arm_work, watchdog_freeze_arm_work_handler);
    k_work_schedule(&watchdog_freeze_arm_work, K_MSEC(CONFIG_ZMK_WATCHDOG_FREEZE_STARTUP_DELAY_MS));
    return 0;
}

SYS_INIT(watchdog_freeze_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void zmk_watchdog_freeze_disarm_sysworkq_channel_for_test(void) {
    /* "sysworkq" is always watchdog_freeze_monitors[0] -- see the table
     * above. task_wdt_delete() is safe to call from ordinary thread
     * context. */
    task_wdt_delete(watchdog_freeze_monitors[0].channel_id);
}
