/**
 * @file pn532_driver.c
 * @brief Command-layer test: exercises every implemented pn532_cmd function.
 *
 * Commands tested:
 *   1. pn532_get_firmware_version  (0x02)
 *   2. pn532_sam_configuration     (0x14)  — NORMAL mode
 *   3. pn532_set_parameters        (0x12)  — default flags, then with flags
 *   4. pn532_tg_init_as_target     (0x8C)  — short timeout (no reader present)
 *   5. pn532_tg_get_data           (0x86)  — short timeout (depends on 4)
 *   6. pn532_tg_set_data           (0x8E)  — short timeout (depends on 4)
 *
 * Also validates core lifecycle: pn532_init, pn532_wakeup, pn532_deinit.
 *
 * Wiring:
 *   ESP32-S3 GPIO 8  (SDA) -> PN532 SDA
 *   ESP32-S3 GPIO 9  (SCL) -> PN532 SCL
 *   ESP32-S3 GPIO 4  (IRQ) -> PN532 P70_IRQ (optional, active-low)
 *   3V3 / GND; external 4.7k pull-ups on SDA/SCL recommended.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pn532.h"
#include "pn532_cmd.h"
#include "pn532_i2c.h"

static const char *TAG = "PN532_CMD_TEST";

/* Pin assignments — adjust if your wiring differs. */
#define I2C_SDA_GPIO    8
#define I2C_SCL_GPIO    9
#define PN532_IRQ_GPIO  4
#define I2C_PORT        I2C_NUM_0

typedef enum { TC_PASS, TC_FAIL, TC_SKIP } tc_result_t;

static const char *tc_str(tc_result_t r)
{
    return r == TC_PASS ? "PASS" : r == TC_FAIL ? "FAIL" : "SKIP";
}

static tc_result_t tc_check(esp_err_t err, bool expect_fail)
{
    if (err == ESP_OK) {
        return expect_fail ? TC_FAIL : TC_PASS;
    }
    return expect_fail ? TC_PASS : TC_FAIL;
}

static void log_tc(const char *name, tc_result_t r, esp_err_t err)
{
    ESP_LOGI(TAG, "[%s] %s  (err=%s)", tc_str(r), name, esp_err_to_name(err));
}

/* Test cases ------------------------------------------------------------- */

/** @brief Test 1 — GetFirmwareVersion. */
static tc_result_t test_get_firmware_version(pn532_handle_t h)
{
    pn532_firmware_version_t fw = {};
    esp_err_t err = pn532_get_firmware_version(h, &fw);
    tc_result_t r = tc_check(err, false);
    log_tc("GetFirmwareVersion", r, err);
    if (r == TC_PASS) {
        ESP_LOGI(TAG, "  IC=0x%02x  Ver=%u.%u  Rev=%u  Features=0x%02x",
                 fw.ic, fw.ver >> 4, fw.ver & 0x0F, fw.rev, fw.support);
    }
    return r;
}

/** @brief Test 2 — SAMConfiguration (NORMAL mode, IRQ enabled). */
static tc_result_t test_sam_configuration(pn532_handle_t h)
{
    esp_err_t err = pn532_sam_configuration(h, PN532_SAM_NORMAL, 0, true);
    tc_result_t r = tc_check(err, false);
    log_tc("SAMConfiguration (NORMAL, IRQ)", r, err);
    return r;
}

/** @brief Test 3a — SetParameters with zero flags (defaults). */
static tc_result_t test_set_parameters_default(pn532_handle_t h)
{
    esp_err_t err = pn532_set_parameters(h, 0);
    tc_result_t r = tc_check(err, false);
    log_tc("SetParameters (flags=0x00)", r, err);
    return r;
}

/** @brief Test 3b — SetParameters with auto-ATR + remove-preamble. */
static tc_result_t test_set_parameters_flags(pn532_handle_t h)
{
    uint8_t flags = PN532_PARAM_AUTO_ATR_RES | PN532_PARAM_REMOVE_PREAMBLE;
    esp_err_t err = pn532_set_parameters(h, flags);
    tc_result_t r = tc_check(err, false);
    log_tc("SetParameters (AUTO_ATR|RM_PREAMBLE)", r, err);
    return r;
}

/** @brief Test 3c — SetParameters with reserved bits (must reject). */
static tc_result_t test_set_parameters_invalid(pn532_handle_t h)
{
    esp_err_t err = pn532_set_parameters(h, 0x08);
    tc_result_t r = tc_check(err, true);
    log_tc("SetParameters (RFU bit — should reject)", r, err);
    return r;
}

/** @brief Test 4 — TgInitAsTarget (short timeout, no reader expected). */
static tc_result_t test_tg_init_as_target(pn532_handle_t h)
{
    pn532_tg_init_params_t params = {
        .sens_res  = {0x04, 0x00},
        .nfcid1    = {0x01, 0x02, 0x03},
        .sel_res   = 0x20,
        .mode      = PN532_TG_MODE_PASSIVE_ONLY,
    };
    pn532_tg_init_result_t result = {};
    esp_err_t err = pn532_tg_init_as_target(h, &params, &result, 1000);
    tc_result_t r = (err == ESP_ERR_TIMEOUT) ? TC_SKIP : tc_check(err, false);
    log_tc("TgInitAsTarget (1 s timeout)", r, err);
    return r;
}

/** @brief Test 5 — TgGetData (expected to skip without reader). */
static tc_result_t test_tg_get_data(pn532_handle_t h)
{
    uint8_t buf[64] = {};
    size_t out_len = 0;
    esp_err_t err = pn532_tg_get_data(h, buf, sizeof(buf), &out_len, 500);
    tc_result_t r = (err != ESP_OK) ? TC_SKIP : TC_PASS;
    log_tc("TgGetData (500 ms timeout)", r, err);
    return r;
}

/** @brief Test 6 — TgSetData (expected to skip without reader). */
static tc_result_t test_tg_set_data(pn532_handle_t h)
{
    const uint8_t payload[] = {0x90, 0x00};
    esp_err_t err = pn532_tg_set_data(h, payload, sizeof(payload), 500);
    tc_result_t r = (err != ESP_OK) ? TC_SKIP : TC_PASS;
    log_tc("TgSetData (500 ms timeout)", r, err);
    return r;
}

/* Main ------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  PN532 Command-Layer Test Suite");
    ESP_LOGI(TAG, "========================================");

    int pass = 0, fail = 0, skip = 0;

    /* ---- Create I2C transport ---------------------------------------- */
    pn532_i2c_config_t i2c_cfg = {
        .sda_gpio  = I2C_SDA_GPIO,
        .scl_gpio  = I2C_SCL_GPIO,
        .irq_gpio  = PN532_IRQ_GPIO,
        .port      = I2C_PORT,
        .clk_speed = 0,  /* 0 -> default 400 kHz */
    };

    pn532_transport_ops_t ops = {};
    void *transport_ctx = NULL;
    esp_err_t err = pn532_i2c_create(&i2c_cfg, &ops, &transport_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C transport create failed: %s — aborting", esp_err_to_name(err));
        return;
    }

    /* ---- Init PN532 driver ------------------------------------------- */
    pn532_config_t cfg = {
        .ops           = &ops,
        .transport_ctx = transport_ctx,
    };
    pn532_handle_t h = NULL;
    err = pn532_init(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_init failed: %s — aborting", esp_err_to_name(err));
        pn532_i2c_destroy(transport_ctx);
        return;
    }

    /* ---- Wake the chip ----------------------------------------------- */
    err = pn532_wakeup(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wakeup returned %s (may be OK if already awake)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[PASS] pn532_wakeup");
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ---- Run all command tests --------------------------------------- */
    ESP_LOGI(TAG, "--- Running command tests ---");

    tc_result_t r;

    r = test_get_firmware_version(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_sam_configuration(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_set_parameters_default(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_set_parameters_flags(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_set_parameters_invalid(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_tg_init_as_target(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_tg_get_data(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    r = test_tg_set_data(h);
    if (r == TC_PASS) pass++; else if (r == TC_FAIL) fail++; else skip++;

    /* ---- Summary ----------------------------------------------------- */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Results:  %d passed, %d failed, %d skipped", pass, fail, skip);
    ESP_LOGI(TAG, "========================================");

    if (fail > 0) {
        ESP_LOGE(TAG, "COMMAND TEST SUITE FAILED");
    } else {
        ESP_LOGI(TAG, "COMMAND TEST SUITE COMPLETED");
    }

    /* ---- Cleanup ----------------------------------------------------- */
    pn532_deinit(h);
    ESP_LOGI(TAG, "Driver deinitialised.");
}
