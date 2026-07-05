/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/watchdog.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Minimal in-RAM settings backend, modeled on
 * zmk-feature-custom-settings/src/test/custom_settings_test.c. native_sim
 * has no real flash-backed settings destination registered by default, so
 * the store's settings_save_one()/settings_delete() calls need *some*
 * registered store to land in, and settings_load_subtree() needs a source
 * to iterate for the "persistence across reload" test.
 */

#define TEST_SETTINGS_STORAGE_CAPACITY 32

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[64];
    size_t len;
};

static struct test_settings_record test_settings_storage[TEST_SETTINGS_STORAGE_CAPACITY];

static struct test_settings_record *test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        if (test_settings_storage[i].present &&
            strncmp(test_settings_storage[i].name, name, sizeof(test_settings_storage[i].name)) ==
                0) {
            return &test_settings_storage[i];
        }
    }
    return NULL;
}

static ssize_t test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);
    memcpy(data, record->data, read_len);
    return read_len;
}

static int test_settings_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        struct test_settings_record *record = &test_settings_storage[i];
        if (!record->present) {
            continue;
        }
        int ret = settings_call_set_handler(record->name, record->len, test_settings_read_cb,
                                            record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static int test_settings_save(struct settings_store *cs, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(cs);

    struct test_settings_record *record = test_settings_find_record(name);
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
        for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
            if (!test_settings_storage[i].present) {
                record = &test_settings_storage[i];
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

static const struct settings_store_itf test_settings_itf = {
    .csi_load = test_settings_load,
    .csi_save = test_settings_save,
};

static struct settings_store test_settings_store = {
    .cs_itf = &test_settings_itf,
};

static int test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&test_settings_store);
    settings_dst_register(&test_settings_store);
    return 0;
}

/* --------------------------------------------------------------------
 * Store tests
 * -------------------------------------------------------------------- */

static struct zmk_watchdog_incident_record make_freeze_record(const char *queue_name) {
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    rec.uptime_s = 42;
    rec.detail.freeze.channel_id = 1;
    strncpy(rec.detail.freeze.queue_name, queue_name, sizeof(rec.detail.freeze.queue_name) - 1);
    return rec;
}

static int test_store_append_read_delete(void) {
    struct zmk_watchdog_incident_record rec = make_freeze_record("sysworkq");

    int ret = zmk_watchdog_store_append(&rec);
    if (ret != 0) {
        LOG_ERR("append failed: %d", ret);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != 1) {
        LOG_ERR("unexpected store count after append: %u", zmk_watchdog_store_count());
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record read_back;
    ret = zmk_watchdog_store_get(0, &read_back);
    if (ret != 0) {
        LOG_ERR("get(0) failed: %d", ret);
        return -EINVAL;
    }
    if (read_back.type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
        strcmp(read_back.detail.freeze.queue_name, "sysworkq") != 0 ||
        read_back.version != ZMK_WATCHDOG_RECORD_VERSION) {
        LOG_ERR("read-back record mismatch");
        return -EINVAL;
    }

    /* recover the id the store assigned (only exposed via enumeration for
     * now, so re-fetch by index and cross-check get_by_id agrees). */
    struct zmk_watchdog_incident_record by_id;
    /* id assignment starts at 1 for the very first incident this boot. */
    ret = zmk_watchdog_store_get_by_id(1, &by_id);
    if (ret != 0 || by_id.uptime_s != read_back.uptime_s) {
        LOG_ERR("get_by_id(1) mismatch: ret=%d", ret);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_store_append_read");

    ret = zmk_watchdog_store_delete(1);
    if (ret != 0) {
        LOG_ERR("delete(1) failed: %d", ret);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != 0) {
        LOG_ERR("unexpected store count after delete: %u", zmk_watchdog_store_count());
        return -EINVAL;
    }
    ret = zmk_watchdog_store_get_by_id(1, &by_id);
    if (ret != -ENOENT) {
        LOG_ERR("expected -ENOENT after delete, got %d", ret);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_store_delete");
    return 0;
}

static int test_store_cap_and_drop(void) {
    /* Store is empty at this point (previous test deleted its one entry). */
    uint16_t capacity = zmk_watchdog_store_capacity();

    for (uint16_t i = 0; i < capacity; i++) {
        struct zmk_watchdog_incident_record rec = make_freeze_record("fillq");
        int ret = zmk_watchdog_store_append(&rec);
        if (ret != 0) {
            LOG_ERR("append %u/%u failed: %d", i, capacity, ret);
            return -EINVAL;
        }
    }

    if (!zmk_watchdog_store_recording_stopped()) {
        LOG_ERR("expected recording_stopped once capacity is reached");
        return -EINVAL;
    }

    uint32_t dropped_before = zmk_watchdog_store_dropped_since_boot();
    struct zmk_watchdog_incident_record overflow_rec = make_freeze_record("overflow");
    int ret = zmk_watchdog_store_append(&overflow_rec);
    if (ret != -ENOSPC) {
        LOG_ERR("expected -ENOSPC once full, got %d", ret);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != capacity) {
        LOG_ERR("store count changed on dropped append: %u", zmk_watchdog_store_count());
        return -EINVAL;
    }
    if (zmk_watchdog_store_dropped_since_boot() != dropped_before + 1) {
        LOG_ERR("dropped_since_boot did not increment as expected");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_store_cap_reached_drops");

    ret = zmk_watchdog_store_delete_all();
    if (ret != 0) {
        LOG_ERR("delete_all failed: %d", ret);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != 0 || zmk_watchdog_store_recording_stopped()) {
        LOG_ERR("delete_all did not fully clear the store");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_store_delete_all_resumes_recording");
    return 0;
}

static int test_store_persistence_across_reload(void) {
    struct zmk_watchdog_incident_record rec = make_freeze_record("reload_q");
    int ret = zmk_watchdog_store_append(&rec);
    if (ret != 0) {
        LOG_ERR("append before reload failed: %d", ret);
        return -EINVAL;
    }
    uint16_t count_before = zmk_watchdog_store_count();

    /* Re-run the settings load for our subtree: this re-invokes the
     * store's SETTINGS_STATIC_HANDLER_DEFINE h_set callback for every key
     * currently in the (in-RAM, test) backend, exactly as a real boot's
     * single global settings_load() would. The RAM index already holds the
     * same data, so this call should be idempotent from the test's point
     * of view; the point is that the settings-handler based rebuild path
     * itself is exercised, not just direct RAM state left over from
     * append(). */
    ret = settings_load_subtree("wdg");
    if (ret != 0) {
        LOG_ERR("settings_load_subtree(wdg) failed: %d", ret);
        return -EINVAL;
    }

    if (zmk_watchdog_store_count() != count_before) {
        LOG_ERR("store count changed across reload: before=%u after=%u", count_before,
                zmk_watchdog_store_count());
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record read_back;
    ret = zmk_watchdog_store_get(0, &read_back);
    if (ret != 0 || strcmp(read_back.detail.freeze.queue_name, "reload_q") != 0) {
        LOG_ERR("record did not survive reload correctly");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_store_persists_across_reload");

    ret = zmk_watchdog_store_delete_all();
    if (ret != 0) {
        return -EINVAL;
    }
    return 0;
}

/* --------------------------------------------------------------------
 * Pending slot tests
 * -------------------------------------------------------------------- */

static int test_pending_convert_valid(void) {
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_FATAL;
    rec.uptime_s = 7;
    rec.detail.fatal.reason = 3;
    rec.detail.fatal.pc = 0xdeadbeef;
    rec.detail.fatal.lr = 0xcafef00d;
    strncpy(rec.detail.fatal.thread_name, "main", sizeof(rec.detail.fatal.thread_name) - 1);

    zmk_watchdog_pending_set(&rec);

    int ret = zmk_watchdog_pending_convert();
    if (ret != 0) {
        LOG_ERR("pending_convert (valid) failed: %d", ret);
        return -EINVAL;
    }

    if (zmk_watchdog_store_count() != 1) {
        LOG_ERR("pending incident did not land in store");
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record stored;
    ret = zmk_watchdog_store_get(0, &stored);
    if (ret != 0 || stored.type != ZMK_WATCHDOG_INCIDENT_FATAL ||
        stored.detail.fatal.pc != 0xdeadbeef || stored.detail.fatal.lr != 0xcafef00d ||
        strcmp(stored.detail.fatal.thread_name, "main") != 0) {
        LOG_ERR("converted record mismatch");
        return -EINVAL;
    }

    /* Converting again must be a no-op: the slot was cleared after the
     * first successful hand-off, so nothing new should be appended. */
    ret = zmk_watchdog_pending_convert();
    if (ret != 0 || zmk_watchdog_store_count() != 1) {
        LOG_ERR("pending slot was reprocessed after conversion");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_pending_convert_valid");

    return zmk_watchdog_store_delete_all();
}

static int test_pending_convert_bad_crc(void) {
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
    rec.detail.reset.cause_bits = 0x10;

    zmk_watchdog_pending_set(&rec);
    zmk_watchdog_pending_corrupt_crc_for_test();

    int ret = zmk_watchdog_pending_convert();
    if (ret != 0) {
        LOG_ERR("pending_convert (bad crc) returned error instead of discarding: %d", ret);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != 0) {
        LOG_ERR("corrupted pending incident was stored anyway");
        return -EINVAL;
    }

    /* The slot must also have been cleared -- converting again changes
     * nothing further. */
    ret = zmk_watchdog_pending_convert();
    if (ret != 0 || zmk_watchdog_store_count() != 0) {
        LOG_ERR("pending slot not cleared after CRC failure");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_pending_convert_bad_crc_discarded");
    return 0;
}

/* --------------------------------------------------------------------
 * Freeze detector test (requires CONFIG_ZMK_WATCHDOG_FREEZE_DETECT +
 * CONFIG_ZMK_WATCHDOG_TEST_INJECTION).
 *
 * zmk_watchdog_inject_freeze() blocks the system workqueue forever; this
 * starves the "sysworkq" task_wdt feed work (also submitted to the system
 * workqueue), so once CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS elapses the
 * channel's ISR callback fires, records a FREEZE incident into the pending
 * slot, and calls zmk_watchdog_reboot() -- intercepted here via
 * zmk_watchdog_reboot_set_override() so the test process doesn't actually
 * reboot/exit.
 * -------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT) && IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)

static volatile int freeze_reboot_calls;

static void freeze_test_reboot_override(void) { freeze_reboot_calls++; }

static int test_freeze_detect(void) {
    freeze_reboot_calls = 0;
    zmk_watchdog_reboot_set_override(freeze_test_reboot_override);

    zmk_watchdog_inject_freeze();

    /* CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS is set small in
     * tests/watchdog/native_sim.conf specifically so this sleep is short.
     * Sleep comfortably past the timeout to give the task_wdt channel time
     * to fire. */
    k_sleep(K_MSEC(CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS * 2));

    /*
     * The injected freeze work never returns, so sysworkq stays permanently
     * blocked for the rest of this process's life -- but the "sysworkq"
     * task_wdt channel itself would otherwise keep re-firing every
     * CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS forever (task_wdt re-arms its
     * internal timer on every fire). Disarm that channel now that we've
     * observed one firing, *before* restoring the real zmk_watchdog_reboot():
     * without this, a later firing would call the real sys_reboot(), which
     * on native_sim exits the whole test process (verified in practice --
     * this is also why this test runs last, see watchdog_test_init()). A
     * disarmed channel is also realistic: on real hardware the "reboot"
     * this simulates would be an actual device reset, so there would be no
     * "next firing" to worry about either.
     */
    zmk_watchdog_freeze_disarm_sysworkq_channel_for_test();
    zmk_watchdog_reboot_set_override(NULL);

    if (freeze_reboot_calls < 1) {
        LOG_ERR("freeze detector did not trigger a reboot within %dms",
                CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS * 2);
        return -EINVAL;
    }

    int ret = zmk_watchdog_pending_convert();
    if (ret != 0) {
        LOG_ERR("pending_convert after freeze failed: %d", ret);
        return -EINVAL;
    }

    if (zmk_watchdog_store_count() != 1) {
        LOG_ERR("freeze incident did not land in store (count=%u)", zmk_watchdog_store_count());
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record rec;
    ret = zmk_watchdog_store_get(0, &rec);
    if (ret != 0 || rec.type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
        strcmp(rec.detail.freeze.queue_name, "sysworkq") != 0) {
        LOG_ERR("freeze incident record mismatch: ret=%d type=%d queue='%s'", ret, rec.type,
                rec.detail.freeze.queue_name);
        return -EINVAL;
    }

    (void)zmk_watchdog_store_delete_all();
    LOG_INF("PASS: watchdog_freeze_detect");

    /*
     * This test permanently and deliberately wedges sysworkq (see the big
     * comment above) -- that is the whole point (it's what makes the
     * task_wdt channel fire), but it also means the rest of the firmware
     * can never boot normally afterwards: among other things, the kscan
     * mock driver schedules its own events on sysworkq too and would never
     * fire, hanging this test binary forever instead of exiting (verified
     * in practice). On real hardware this moment is where the *real*
     * zmk_watchdog_reboot() would already have restarted the device; here,
     * ending the process the same way main()'s kscan-driven exit-after
     * would have is the accurate way to represent that outcome so
     * run-test.sh's harness (which only inspects piped stdout, not this
     * process's exit code) sees the PASS line and terminates cleanly.
     */
    exit(0);
}

#endif /* CONFIG_ZMK_WATCHDOG_FREEZE_DETECT && CONFIG_ZMK_WATCHDOG_TEST_INJECTION */

/* --------------------------------------------------------------------
 * Fatal detector test (requires CONFIG_ZMK_WATCHDOG_FATAL_DETECT).
 *
 * Rather than actually faulting native_sim -- unsafe/unreliable: verified
 * in practice that routing a real k_oops()/z_fatal_error() through this
 * test process causes zephyr/kernel/fatal.c to k_thread_abort() the
 * faulting thread once k_sys_fatal_error_handler() returns (since our
 * reboot override doesn't tear anything down), which either takes the
 * whole process down (if that thread is the test-driver/init thread) or
 * leaves the test's control flow in an unclear state (run from a disposable
 * thread) -- this exercises the two safe layers directly instead:
 *
 *  1. watchdog_fatal_build_record(): the record-building logic, factored
 *     out specifically so it can be called with a synthetic reason/esf.
 *  2. k_sys_fatal_error_handler() *itself*, called as a plain function
 *     (not via a real fault): this is exactly what the real fault path
 *     would invoke, just without going through z_fatal_error()'s
 *     surrounding thread-abort machinery, which is what makes it unsafe to
 *     trigger for real inside a test process.
 * -------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FATAL_DETECT)

static int test_fatal_build_record(void) {
    struct zmk_watchdog_incident_record rec;
    watchdog_fatal_build_record(&rec, 3 /* K_ERR_KERNEL_PANIC */, NULL);

    if (rec.type != ZMK_WATCHDOG_INCIDENT_FATAL || rec.detail.fatal.reason != 3) {
        LOG_ERR("fatal record builder mismatch: type=%d reason=%u", rec.type,
                rec.detail.fatal.reason);
        return -EINVAL;
    }
    /* esf == NULL -> pc/lr stay 0 (documented behavior). */
    if (rec.detail.fatal.pc != 0 || rec.detail.fatal.lr != 0) {
        LOG_ERR("fatal record builder should zero pc/lr for a NULL esf");
        return -EINVAL;
    }
    if (rec.detail.fatal.thread_name[0] == '\0') {
        LOG_ERR("fatal record builder left thread_name empty");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_fatal_build_record");
    return 0;
}

static volatile int fatal_reboot_calls;

static void fatal_test_reboot_override(void) { fatal_reboot_calls++; }

/* Declared in fatal.c but not part of the public header (it's the weak
 * symbol override itself, not a stable API) -- redeclare its exact
 * prototype here to call it directly as an ordinary function. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf);

static int test_fatal_handler_end_to_end(void) {
    fatal_reboot_calls = 0;
    zmk_watchdog_reboot_set_override(fatal_test_reboot_override);

    /* Calls the real override directly (not via z_fatal_error(), which
     * would also try to abort the calling thread afterwards -- see the
     * file comment above for why that's unsafe to exercise for real here).
     * esf = NULL exactly like a native_sim k_oops() would deliver. */
    k_sys_fatal_error_handler(3 /* K_ERR_KERNEL_OOPS */, NULL);

    zmk_watchdog_reboot_set_override(NULL);

    if (fatal_reboot_calls < 1) {
        LOG_ERR("fatal handler did not trigger a reboot");
        return -EINVAL;
    }

    int ret = zmk_watchdog_pending_convert();
    if (ret != 0) {
        LOG_ERR("pending_convert after fatal handler failed: %d", ret);
        return -EINVAL;
    }

    if (zmk_watchdog_store_count() != 1) {
        LOG_ERR("fatal incident did not land in store (count=%u)", zmk_watchdog_store_count());
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record rec;
    ret = zmk_watchdog_store_get(0, &rec);
    if (ret != 0 || rec.type != ZMK_WATCHDOG_INCIDENT_FATAL || rec.detail.fatal.reason != 3) {
        LOG_ERR("fatal incident record mismatch: ret=%d type=%d reason=%u", ret, rec.type,
                rec.detail.fatal.reason);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_fatal_handler_end_to_end");
    return zmk_watchdog_store_delete_all();
}

#endif /* CONFIG_ZMK_WATCHDOG_FATAL_DETECT */

static int watchdog_test_init(void) {
    int ret = test_settings_backend_init();
    if (ret < 0) {
        return ret;
    }

    ret = test_store_append_read_delete();
    if (ret < 0) {
        return ret;
    }

    ret = test_store_cap_and_drop();
    if (ret < 0) {
        return ret;
    }

    ret = test_store_persistence_across_reload();
    if (ret < 0) {
        return ret;
    }

    ret = test_pending_convert_valid();
    if (ret < 0) {
        return ret;
    }

    ret = test_pending_convert_bad_crc();
    if (ret < 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FATAL_DETECT)
    ret = test_fatal_build_record();
    if (ret < 0) {
        return ret;
    }

    ret = test_fatal_handler_end_to_end();
    if (ret < 0) {
        return ret;
    }
#endif

    /*
     * Freeze test runs LAST: zmk_watchdog_inject_freeze() permanently blocks
     * the system workqueue (the injected work item sleeps forever and never
     * returns), so nothing else can ever be submitted to/run on sysworkq
     * again for the rest of this process's life -- including the "sysworkq"
     * task_wdt channel's own feed work, which is the point, but also any
     * other test that might otherwise rely on sysworkq afterwards.
     */
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT) && IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
    ret = test_freeze_detect();
    if (ret < 0) {
        return ret;
    }
#endif

    return 0;
}

/* Run before the low-priority-workqueue pending-conversion hook would fire
 * in a real boot (irrelevant here since the test calls
 * zmk_watchdog_pending_convert() directly instead of waiting for it), and
 * after the settings subsystem's own SYS_INIT hooks. */
SYS_INIT(watchdog_test_init, APPLICATION, 99);
