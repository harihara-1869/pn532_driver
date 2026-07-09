/**
 * @file pn532_spi.h
 * @brief SPI transport backend for the PN532 driver.
 *
 * Creates a @ref pn532_transport_ops_t vtable bound to an SPI bus, ready to be
 * handed to @ref pn532_init via @ref pn532_config_t. The backend owns the SPI
 * bus/device it creates, the software-controlled CS GPIO, and (optionally) the
 * GPIO IRQ line.
 *
 * ## SPI transport quirks
 *
 * The PN532 SPI interface differs from standard SPI in three ways:
 *
 * 1. **LSB-first byte order.** Every byte on the wire is transmitted and
 *    received LSB-first (the opposite of standard SPI MSBFIRST). The ESP-IDF
 *    SPI device is created with the `SPI_DEVICE_BIT_LSBFIRST` flag so the
 *    hardware handles bit reversal automatically.
 *
 * 2. **Control byte prefix.** Every SPI transaction begins with a 1-byte
 *    control byte:
 *      - 0x01 (DW): data write — frame follows
 *      - 0x02 (SR): status read — read 1-byte status
 *      - 0x03 (DR): data read — read frame
 *    This byte is SPI transport framing only; it is not part of the PN532
 *    frame format.
 *
 * 3. **Software chip select.** CS must remain asserted (LOW) for the complete
 *    duration of each transaction. The SPI device is created with
 *    `spics_io_num = -1` (no hardware CS) and the backend drives CS manually
 *    via GPIO.
 *
 * ## Drop-in backend swap
 *
 * To swap between I2C and SPI, change only the config struct and create call:
 *
 * @code
 * // I2C (before):
 * pn532_i2c_config_t cfg = { .sda_gpio=8, .scl_gpio=9, .irq_gpio=4,
 *                            .rst_gpio=5, .port=I2C_NUM_0 };
 * pn532_i2c_create(&cfg, &ops, &ctx);
 *
 * // SPI (after):
 * pn532_spi_config_t cfg = { .host=SPI2_HOST, .mosi_gpio=11, .miso_gpio=13,
 *                            .sck_gpio=12, .cs_gpio=10, .irq_gpio=4,
 *                            .rst_gpio=5 };
 * pn532_spi_create(&cfg, &ops, &ctx);
 *
 * // Everything after this line is identical:
 * pn532_init(&(pn532_config_t){ .ops=&ops, .ctx=ctx }, &handle);
 * pn532_wakeup(handle);
 * @endcode
 *
 * @note The PN532 must be the only device on the SPI bus due to its
 *       LSB-first byte ordering, which is incompatible with standard devices.
 *
 * @note ESP-IDF v5.x, uses the `driver/spi_master.h` API.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "hal/spi_types.h"  /* spi_host_device_t */
#include "pn532.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default SPI clock frequency (2 MHz — conservative, reliable).
 */
#define PN532_SPI_DEFAULT_CLK_HZ  2000000U

/**
 * @brief SPI transport configuration.
 *
 * All GPIO fields are required (no -1 defaults) except @p irq_gpio and
 * @p rst_gpio which are optional (set to -1 if not wired).
 *
 * @p host is caller-specified (e.g. SPI2_HOST, SPI3_HOST). The PN532 must be
 * the only device on this SPI bus due to its LSB-first byte ordering, which
 * is incompatible with standard devices.
 */
typedef struct {
    spi_host_device_t host;        /**< SPI peripheral (SPI2_HOST / SPI3_HOST). */
    int               mosi_gpio;   /**< MOSI pin (required). */
    int               miso_gpio;   /**< MISO pin (required). */
    int               sck_gpio;    /**< SCK pin (required). */
    int               cs_gpio;     /**< Chip select (required, active low, software controlled). */
    int               irq_gpio;    /**< IRQ pin (P70_IRQ); -1 if not wired -> polling mode. */
    int               rst_gpio;    /**< Reset pin; -1 if not wired -> no hardware reset. */
    uint32_t          clk_speed_hz; /**< SPI clock Hz; 0 -> PN532_SPI_DEFAULT_CLK_HZ (2 MHz). */
} pn532_spi_config_t;

/**
 * @brief Create an SPI transport for the PN532.
 *
 * Initialises the SPI bus, adds the PN532 as a device, configures the
 * software CS and optional IRQ/RST GPIOs, and probes the PN532 by reading a
 * status byte.
 *
 * On success, fills @p ops_out and @p ctx_out. Pass these to @ref pn532_init.
 * To destroy: call ops_out->destroy(ctx_out), or let @ref pn532_deinit call
 * it through the handle.
 *
 * @param[in]  cfg      SPI configuration (caller-owned, not retained).
 * @param[out] ops_out  Receives the transport vtable (copied by value).
 * @param[out] ctx_out  Receives the transport context pointer.
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG if cfg, ops_out, or ctx_out is NULL, or if any
 *     required GPIO is < 0
 *   - ESP_ERR_NO_MEM if context allocation fails
 *   - ESP_ERR_TIMEOUT if PN532 does not respond on the SPI bus
 *   - Other esp_err_t from the SPI or GPIO driver
 *
 * @note On failure, no resources are leaked and @p ctx_out is left unchanged.
 */
esp_err_t pn532_spi_create(const pn532_spi_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out);

/**
 * @brief Destroy an SPI transport created by @ref pn532_spi_create.
 *
 * Tears down the IRQ ISR, SPI device, SPI bus, and mutex, then frees @p ctx.
 * Normally invoked through @ref pn532_deinit (the vtable's `destroy`); call
 * directly only if you never handed the context to @ref pn532_init.
 *
 * @param[in] ctx Transport context from @ref pn532_spi_create (may be NULL).
 */
void pn532_spi_destroy(void *ctx);

/**
 * @brief Assert hardware reset on the PN532 via the SPI backend.
 *
 * Same semantics as @ref pn532_i2c_reset_device. Returns
 * ESP_ERR_NOT_SUPPORTED if @p rst_gpio was configured as -1.
 *
 * @param ctx        SPI transport context (the void* returned by
 *                   pn532_spi_create).
 * @param pulse_ms   Duration to hold reset LOW (milliseconds).
 * @param settle_ms  Wait after reset releases before returning (ms).
 * @return ESP_OK, or ESP_ERR_NOT_SUPPORTED if no rst_gpio configured.
 */
esp_err_t pn532_spi_reset_device(void *ctx,
                                  uint32_t pulse_ms,
                                  uint32_t settle_ms);

#ifdef __cplusplus
}
#endif
