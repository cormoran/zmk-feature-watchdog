/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
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

    return test_pending_convert_bad_crc();
}

/* Run before the low-priority-workqueue pending-conversion hook would fire
 * in a real boot (irrelevant here since the test calls
 * zmk_watchdog_pending_convert() directly instead of waiting for it), and
 * after the settings subsystem's own SYS_INIT hooks. */
SYS_INIT(watchdog_test_init, APPLICATION, 99);
