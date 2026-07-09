/**
 * @file pn532_spi.c
 * @brief SPI transport backend for the PN532 driver (ESP-IDF v5.x, spi_master).
 *
 * Implements the @ref pn532_transport_ops_t vtable over the ESP-IDF SPI master
 * API. Handles the PN532-specific SPI quirks:
 *
 *  - LSB-first byte ordering: the SPI device is created with
 *    SPI_DEVICE_BIT_LSBFIRST so the hardware handles bit reversal.
 *  - Control byte prefix: every transaction starts with 0x01 (DW), 0x02 (SR),
 *    or 0x03 (DR) before any frame or status bytes.
 *  - Software chip select: CS is driven manually via GPIO to keep it asserted
 *    for the complete duration of each transaction.
 *  - Dual-mode ready detection: hardware IRQ (P70, active-low, via a GPIO ISR
 *    that gives a binary semaphore) when irq_gpio >= 0, else status-byte polling.
 */

#include "pn532_spi.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "PN532";

/* ---- Tunables ----------------------------------------------------------- */

/* PN532 status byte, bit 0 = RDY (1 => a frame is ready to read). */
#define PN532_STATUS_RDY_BIT       0x01

/* Write retry policy. SPI has no NACK, so we retry on any transmit failure. */
#define PN532_SPI_WRITE_MAX_RETRIES    5
#define PN532_SPI_WRITE_RETRY_DELAY_MS 1

/* Polling-mode defaults (used when irq_gpio < 0). */
#define PN532_SPI_POLL_INTERVAL_MS     1
#define PN532_SPI_POLL_MAX_RETRIES     1000

/* Bus mutex acquisition timeout. */
#define PN532_SPI_MUTEX_TIMEOUT_MS     5000

/* SPI transaction timeout. */
#define PN532_SPI_XFER_TIMEOUT_MS      50

/* IRQ line is active-low: level 0 means "frame ready / IRQ asserted". */
#define PN532_SPI_IRQ_ASSERTED_LEVEL   0

/* SPI control bytes. */
#define PN532_SPI_CTRL_DW  0x01   /* Data Write: host -> PN532, frame follows  */
#define PN532_SPI_CTRL_SR  0x02   /* Status Read: host -> PN532, read 1 status */
#define PN532_SPI_CTRL_DR  0x03   /* Data Read:   host -> PN532, read frame    */

/*
 * Fixed buffer size for SPI transactions. The largest PN532 frame is 267 bytes
 * (PREAMBLE + STARTCODE + LEN + LCS + TFI + 264 DATA + DCS + POSTAMBLE).
 * We add 1 for the SPI control byte. 278 bytes covers the worst case with
 * some padding.
 */
#define PN532_SPI_XFER_BUF_SIZE  278

/* ---- Transport context -------------------------------------------------- */

typedef struct {
    spi_device_handle_t  spi_dev;       /* ESP-IDF SPI device handle          */
    spi_host_device_t    host;          /* SPI host, needed for spi_bus_free   */
    SemaphoreHandle_t    mutex;         /* serialises all SPI transactions     */

    int  cs_gpio;                       /* chip select (software, active low)  */
    int  irq_gpio;                      /* -1 => polling mode                  */
    int  rst_gpio;                      /* -1 => no hardware reset             */
    bool irq_mode;                      /* irq_gpio >= 0                       */
    bool isr_added;                     /* isr handler registered for cleanup  */
    SemaphoreHandle_t irq_sem;          /* given from the GPIO ISR             */

    uint32_t poll_interval_ms;
    uint32_t poll_max_retries;
} pn532_spi_ctx_t;

/* ---- Forward declarations ----------------------------------------------- */

static esp_err_t pn532_spi_write(void *ctx, const uint8_t *buf, size_t len);
static esp_err_t pn532_spi_read_status(void *ctx, uint8_t *status_out);
static esp_err_t pn532_spi_read_frame(void *ctx, uint8_t *buf, size_t len);
static esp_err_t pn532_spi_wait_ready(void *ctx, uint32_t timeout_ms);
void pn532_spi_destroy(void *ctx);
esp_err_t pn532_spi_reset_device(void *ctx, uint32_t pulse_ms, uint32_t settle_ms);

/* ---- GPIO ISR ----------------------------------------------------------- */

static void IRAM_ATTR pn532_spi_irq_isr(void *arg)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)arg;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(c->irq_sem, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---- Vtable mutex helpers ----------------------------------------------- */

static esp_err_t vtable_bus_lock(void *ctx, uint32_t timeout_ms)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;
    if (xSemaphoreTake(c->mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "bus mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void vtable_bus_unlock(void *ctx)
{
    xSemaphoreGive(((pn532_spi_ctx_t *)ctx)->mutex);
}

/* ---- CS helpers --------------------------------------------------------- */

static inline void cs_assert(pn532_spi_ctx_t *c)
{
    gpio_set_level(c->cs_gpio, 0);
}

static inline void cs_deassert(pn532_spi_ctx_t *c)
{
    gpio_set_level(c->cs_gpio, 1);
}

/* ------------------------------------------------------------------------- */
/* Transport ops                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Write a complete PN532 frame over SPI.
 *
 * Prepend the DW control byte (0x01) and transmit in a single SPI transaction
 * with CS asserted for the full duration. Retries on failure (SPI has no NACK,
 * so any transmit error is retried).
 */
static esp_err_t pn532_spi_write(void *ctx, const uint8_t *buf, size_t len)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;

    esp_err_t err;
    for (int attempt = 0; attempt < PN532_SPI_WRITE_MAX_RETRIES; attempt++) {
        err = vtable_bus_lock(c, PN532_SPI_MUTEX_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }

        /* Build tx buffer: [DW control byte] + [frame bytes] */
        uint8_t tx_buf[PN532_SPI_XFER_BUF_SIZE];
        tx_buf[0] = PN532_SPI_CTRL_DW;
        memcpy(tx_buf + 1, buf, len);

        spi_transaction_t t = {
            .length    = (len + 1) * 8,  /* total bits */
            .tx_buffer = tx_buf,
            .rx_buffer = NULL,
        };

        cs_assert(c);
        err = spi_device_transmit(c->spi_dev, &t);
        cs_deassert(c);

        vtable_bus_unlock(c);

        if (err == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGD(TAG, "SPI write failed (attempt %d/%d): %s",
                 attempt + 1, PN532_SPI_WRITE_MAX_RETRIES,
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(PN532_SPI_WRITE_RETRY_DELAY_MS));
    }

    ESP_LOGW(TAG, "SPI write failed after %d retries", PN532_SPI_WRITE_MAX_RETRIES);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Read the 1-byte PN532 status byte over SPI.
 *
 * Transmit the SR control byte (0x02) and receive 2 bytes. The first received
 * byte is the echoed control byte; the second is the actual status.
 */
static esp_err_t pn532_spi_read_status(void *ctx, uint8_t *status_out)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;

    esp_err_t err = vtable_bus_lock(c, PN532_SPI_MUTEX_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t tx[2] = { PN532_SPI_CTRL_SR, 0x00 };
    uint8_t rx[2] = { 0 };

    spi_transaction_t t = {
        .length    = 16,  /* 2 bytes × 8 bits */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    cs_assert(c);
    err = spi_device_transmit(c->spi_dev, &t);
    cs_deassert(c);

    vtable_bus_unlock(c);

    if (err == ESP_OK) {
        *status_out = rx[1];  /* rx[0] is the echo of the control byte */
    } else {
        ESP_LOGD(TAG, "SPI status read failed: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Read status byte + frame bytes in a single SPI transaction.
 *
 * Transmit the DR control byte (0x03) and receive (1 + len) bytes. The first
 * received byte is the echoed control byte; buf[0] receives the status byte
 * and buf[1..len-1] the frame bytes. CS is asserted for the full transaction.
 *
 * If the status byte reports not-ready, returns ESP_ERR_INVALID_RESPONSE (the
 * caller should have waited for readiness first).
 */
static esp_err_t pn532_spi_read_frame(void *ctx, uint8_t *buf, size_t len)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;

    esp_err_t err = vtable_bus_lock(c, PN532_SPI_MUTEX_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* Build tx: [DR control byte] + [len zero dummy bytes] */
    uint8_t tx[PN532_SPI_XFER_BUF_SIZE];
    memset(tx, 0x00, sizeof(tx));
    tx[0] = PN532_SPI_CTRL_DR;

    /* rx: [ctrl_echo] + [status + frame bytes] */
    uint8_t rx[PN532_SPI_XFER_BUF_SIZE];
    memset(rx, 0x00, sizeof(rx));

    spi_transaction_t t = {
        .length    = (len + 1) * 8,  /* control byte + len data bytes, in bits */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    cs_assert(c);
    err = spi_device_transmit(c->spi_dev, &t);
    cs_deassert(c);

    vtable_bus_unlock(c);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI frame read failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Skip the control byte echo (rx[0]) and copy status + frame into buf. */
    memcpy(buf, rx + 1, len);

    if ((buf[0] & PN532_STATUS_RDY_BIT) == 0) {
        ESP_LOGW(TAG, "read_frame: status not ready (0x%02x)", buf[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/**
 * @brief IRQ-mode readiness wait: pre-check level, then block on the semaphore.
 */
static esp_err_t wait_ready_irq(pn532_spi_ctx_t *c, uint32_t timeout_ms)
{
    /* Pre-check: the IRQ may already be asserted before we arm the semaphore. */
    if (gpio_get_level(c->irq_gpio) == PN532_SPI_IRQ_ASSERTED_LEVEL) {
        return ESP_OK;
    }

    if (xSemaphoreTake(c->irq_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }

    /* Final level check to catch an assertion that raced the timeout. */
    if (gpio_get_level(c->irq_gpio) == PN532_SPI_IRQ_ASSERTED_LEVEL) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Polling-mode readiness wait: repeatedly read the 1-byte status.
 *
 * Bounded by both the caller timeout and the configured retry cap; on either
 * exhaustion returns ESP_ERR_TIMEOUT.
 */
static esp_err_t wait_ready_poll(pn532_spi_ctx_t *c, uint32_t timeout_ms)
{
    const uint32_t interval = c->poll_interval_ms;
    uint32_t elapsed = 0;

    for (uint32_t iter = 0; iter < c->poll_max_retries; iter++) {
        uint8_t status = 0;
        esp_err_t err = pn532_spi_read_status(c, &status);
        if (err == ESP_OK && (status & PN532_STATUS_RDY_BIT)) {
            return ESP_OK;
        }
        if (elapsed >= timeout_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(interval));
        elapsed += interval;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t pn532_spi_wait_ready(void *ctx, uint32_t timeout_ms)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;
    return c->irq_mode ? wait_ready_irq(c, timeout_ms)
                       : wait_ready_poll(c, timeout_ms);
}

/* ------------------------------------------------------------------------- */
/* Hardware reset                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t pn532_spi_reset_device(void *ctx, uint32_t pulse_ms, uint32_t settle_ms)
{
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;

    if (c == NULL || c->rst_gpio < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "asserting hardware reset (gpio %d, pulse %"PRIu32" ms, settle %"PRIu32" ms)",
             c->rst_gpio, pulse_ms, settle_ms);

    /* Drive RST LOW. */
    gpio_set_level(c->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));

    /* Release RST (HIGH). */
    gpio_set_level(c->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));

    ESP_LOGI(TAG, "hardware reset complete");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Construction / destruction                                                 */
/* ------------------------------------------------------------------------- */

static esp_err_t setup_irq(pn532_spi_ctx_t *c)
{
    c->irq_sem = xSemaphoreCreateBinary();
    if (c->irq_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << c->irq_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* IRQ is open-drain, active low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,     /* assertion = high -> low       */
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    /* The ISR service is process-global; tolerate it already being installed. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(c->irq_gpio, pn532_spi_irq_isr, c);
    if (err != ESP_OK) {
        return err;
    }
    c->isr_added = true;
    return ESP_OK;
}

esp_err_t pn532_spi_create(const pn532_spi_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out)
{
    if (cfg == NULL || ops_out == NULL || ctx_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->mosi_gpio < 0 || cfg->miso_gpio < 0 ||
        cfg->sck_gpio < 0  || cfg->cs_gpio < 0) {
        ESP_LOGE(TAG, "MISO/MOSI/SCK/CS GPIOs must be set by the application");
        return ESP_ERR_INVALID_ARG;
    }

    pn532_spi_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }

    c->host             = cfg->host;
    c->cs_gpio          = cfg->cs_gpio;
    c->irq_gpio         = cfg->irq_gpio;
    c->rst_gpio         = cfg->rst_gpio;
    c->irq_mode         = (cfg->irq_gpio >= 0);
    c->poll_interval_ms = PN532_SPI_POLL_INTERVAL_MS;
    c->poll_max_retries = PN532_SPI_POLL_MAX_RETRIES;

    esp_err_t err;

    c->mutex = xSemaphoreCreateMutex();
    if (c->mutex == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* --- CS GPIO (software-controlled, active low, initially deasserted) --- */
    const gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << cfg->cs_gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CS GPIO setup failed: %s", esp_err_to_name(err));
        goto fail;
    }
    gpio_set_level(cfg->cs_gpio, 1);  /* deasserted (high) */

    /* --- SPI bus --- */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = cfg->mosi_gpio,
        .miso_io_num     = cfg->miso_gpio,
        .sclk_io_num     = cfg->sck_gpio,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 277,  /* worst-case PN532 frame */
    };
    err = spi_bus_initialize(cfg->host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* --- SPI device (LSB-first, no hardware CS, CPOL=0 CPHA=0) --- */
    const uint32_t clk = (cfg->clk_speed_hz != 0) ? cfg->clk_speed_hz
                                                   : PN532_SPI_DEFAULT_CLK_HZ;
    const spi_device_interface_config_t dev_cfg = {
        .mode           = 0,                         /* CPOL=0, CPHA=0 */
        .clock_speed_hz = clk,
        .spics_io_num   = -1,                        /* software CS    */
        .queue_size     = 1,
        .flags          = SPI_DEVICE_BIT_LSBFIRST,   /* PN532 quirk    */
    };
    err = spi_bus_add_device(cfg->host, &dev_cfg, &c->spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* --- Optional reset line --- */
    if (c->rst_gpio >= 0) {
        const gpio_config_t rst_io = {
            .pin_bit_mask = 1ULL << c->rst_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&rst_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reset GPIO setup failed: %s", esp_err_to_name(err));
            goto fail;
        }
        gpio_set_level(c->rst_gpio, 1);  /* released (high) */
        ESP_LOGI(TAG, "reset pin configured (gpio %d)", c->rst_gpio);
    }

    /* --- Optional IRQ line --- */
    if (c->irq_mode) {
        err = setup_irq(c);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IRQ GPIO setup failed: %s", esp_err_to_name(err));
            goto fail;
        }
    }

    /* --- Probe: read a status byte to confirm the PN532 is present --- */
    {
        uint8_t probe_status = 0;
        err = pn532_spi_read_status(c, &probe_status);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PN532 not found on SPI bus: %s", esp_err_to_name(err));
            err = ESP_ERR_TIMEOUT;
            goto fail;
        }
        ESP_LOGI(TAG, "PN532 detected on SPI bus (status: 0x%02x)", probe_status);
    }

    if (c->irq_mode) {
        ESP_LOGI(TAG, "SPI transport ready (IRQ mode, gpio %d, %"PRIu32" Hz)",
                 c->irq_gpio, clk);
    } else {
        ESP_LOGI(TAG, "SPI transport ready (polling mode, %"PRIu32" Hz)", clk);
    }

    ops_out->write        = pn532_spi_write;
    ops_out->read_status  = pn532_spi_read_status;
    ops_out->read_frame   = pn532_spi_read_frame;
    ops_out->wait_ready   = pn532_spi_wait_ready;
    ops_out->destroy      = pn532_spi_destroy;
    ops_out->reset_device = pn532_spi_reset_device;
    ops_out->bus_lock     = vtable_bus_lock;
    ops_out->bus_unlock   = vtable_bus_unlock;

    *ctx_out = c;
    return ESP_OK;

fail:
    pn532_spi_destroy(c);
    return err;
}

void pn532_spi_destroy(void *ctx)
{
    if (ctx == NULL) {
        return;
    }
    pn532_spi_ctx_t *c = (pn532_spi_ctx_t *)ctx;

    if (c->isr_added) {
        gpio_isr_handler_remove(c->irq_gpio);
    }
    if (c->irq_sem != NULL) {
        vSemaphoreDelete(c->irq_sem);
    }
    if (c->spi_dev != NULL) {
        spi_bus_remove_device(c->spi_dev);
    }
    spi_bus_free(c->host);
    if (c->mutex != NULL) {
        vSemaphoreDelete(c->mutex);
    }
    free(c);
}
