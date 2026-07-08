/**
 * @file pn532_driver.c
 * @brief Smoke test: wake PN532, read firmware version over I2C.
 *
 * Wiring:
 *   ESP32-S3 GPIO 21 (SDA) -> PN532 SDA
 *   ESP32-S3 GPIO 22 (SCL) -> PN532 SCL
 *   ESP32-S3 GPIO 4  (IRQ) -> PN532 P70_IRQ (optional, active-low)
 *   3V3 / GND as usual; external 4.7k pull-ups on SDA/SCL recommended.
 */

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pn532.h"
#include "pn532_i2c.h"

static const char *TAG = "PN532_TEST";

/* Pin assignments — adjust if your wiring differs. */
#define I2C_SDA_GPIO  8
#define I2C_SCL_GPIO  9
#define PN532_IRQ_GPIO  4
#define I2C_PORT        I2C_NUM_0

/* PN532 GetFirmwareVersion command code. */
#define PN532_CMD_GET_FIRMWARE_VERSION  0x02

void app_main(void)
{
    ESP_LOGI(TAG, "=== PN532 smoke test ===");

    /* ---- 1. Create I2C transport ------------------------------------ */
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
        ESP_LOGE(TAG, "I2C transport create failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "I2C transport created");

    /* ---- 2. Init PN532 driver --------------------------------------- */
    pn532_config_t cfg = {
        .ops           = &ops,
        .transport_ctx = transport_ctx,
    };
    pn532_handle_t h = NULL;
    err = pn532_init(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "PN532 driver initialised");

    /* ---- 3. Wake the chip ------------------------------------------- */
    err = pn532_wakeup(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wakeup returned %s (may be OK if chip was already awake)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PN532 woken");
    }

    /* Short delay for oscillator stabilisation after wakeup. */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* ---- 4. Send GetFirmwareVersion --------------------------------- */
    ESP_LOGI(TAG, "Sending GetFirmwareVersion (0x%02x)...", PN532_CMD_GET_FIRMWARE_VERSION);
    err = pn532_send_command(h, PN532_CMD_GET_FIRMWARE_VERSION, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send_command failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Command sent, ACK received");

    /* ---- 5. Read response ------------------------------------------- */
    uint8_t resp[16] = {};
    size_t resp_len = 0;
    err = pn532_receive_response(h, resp, sizeof(resp), &resp_len, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receive_response failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Response format: [0x03 (cmd+1)] [IC] [Ver] [Rev] [Features...] */
    if (resp_len < 4) {
        ESP_LOGE(TAG, "Response too short (%u bytes)", (unsigned)resp_len);
        goto cleanup;
    }

    ESP_LOGI(TAG, "=== Firmware Version ===");
    ESP_LOGI(TAG, "  IC:       0x%02x", resp[1]);
    ESP_LOGI(TAG, "  Ver:      %u.%u", resp[2] >> 4, resp[2] & 0x0F);
    ESP_LOGI(TAG, "  Rev:      %u", resp[3]);
    if (resp_len > 4) {
        ESP_LOGI(TAG, "  Features: 0x%02x", resp[4]);
    }
    ESP_LOGI(TAG, "========================");
    ESP_LOGI(TAG, "PN532 smoke test PASSED");

cleanup:
    pn532_deinit(h);
    ESP_LOGI(TAG, "Driver deinitialised, done.");
}
