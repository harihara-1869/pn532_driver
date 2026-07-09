/**
 * @file pn532_cmd.c
 * @brief PN532 command layer: high-level wrappers for common commands.
 *
 * Each function sends one PN532 command via pn532_send_command(), then reads
 * and validates the response via pn532_receive_response(). All functions
 * return esp_err_t.
 */

#include "pn532_cmd.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "PN532_CMD";

/* PN532 response TFI byte and the expected response command code (cmd + 1). */
#define PN532_CMD_RESPONSE_OFFSET  0x01

/* Maximum payload for TgSetData / TgGetData (per datasheet). */
#define PN532_TG_MAX_DATA_LEN      262

/* Maximum general bytes / historical bytes in TgInitAsTarget. */
#define PN532_TG_MAX_GTK_LEN        47

/* ========================================================================= */
/* GetFirmwareVersion (0x02)                                                  */
/* ========================================================================= */

#define PN532_CMD_GET_FIRMWARE_VERSION  0x02
#define PN532_RSP_GET_FIRMWARE_VERSION  0x03

esp_err_t pn532_get_firmware_version(pn532_handle_t h,
                                     pn532_firmware_version_t *out)
{
    if (h == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_GET_FIRMWARE_VERSION,
                                       NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[5];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, 100);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 03 IC VER REV SUPPORT → buf = {0x03, IC, VER, REV, SUPPORT} */
    if (len < 5 || buf[0] != PN532_RSP_GET_FIRMWARE_VERSION) {
        ESP_LOGE(TAG, "GetFirmwareVersion: unexpected response (len=%u cmd=0x%02x)",
                 (unsigned)len, len > 0 ? buf[0] : 0);
        return ESP_FAIL;
    }

    out->ic      = buf[1];
    out->ver     = buf[2];
    out->rev     = buf[3];
    out->support = buf[4];

    if (out->ic != 0x32) {
        ESP_LOGE(TAG, "GetFirmwareVersion: unexpected IC 0x%02x (expected 0x32)",
                 out->ic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PN532 firmware v%u.%u, IC=0x%02x, capabilities=0x%02x",
             out->ver, out->rev, out->ic, out->support);
    return ESP_OK;
}

/* ========================================================================= */
/* SAMConfiguration (0x14)                                                    */
/* ========================================================================= */

#define PN532_CMD_SAM_CONFIGURATION  0x14
#define PN532_RSP_SAM_CONFIGURATION  0x15

esp_err_t pn532_sam_configuration(pn532_handle_t h,
                                  pn532_sam_mode_t mode,
                                  uint8_t timeout_50ms,
                                  bool irq_enabled)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t params[3];
    size_t params_len = 1;
    params[0] = (uint8_t)mode;

    if (mode == PN532_SAM_VIRTUAL_CARD) {
        params[1] = timeout_50ms;
        params[2] = irq_enabled ? 0x01 : 0x00;
        params_len = 3;
    } else {
        /* Timeout byte is NOT included for non-VIRTUAL_CARD modes. */
        params[1] = irq_enabled ? 0x01 : 0x00;
        params_len = 2;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_SAM_CONFIGURATION,
                                       params, params_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, 100);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 15 → buf = {0x15} (no extra data). */
    if (len < 1 || buf[0] != PN532_RSP_SAM_CONFIGURATION) {
        ESP_LOGE(TAG, "SAMConfiguration: unexpected response cmd 0x%02x",
                 len > 0 ? buf[0] : 0);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SAM configured: mode=0x%02x irq=%d", mode, irq_enabled);
    return ESP_OK;
}

/* ========================================================================= */
/* SetParameters (0x12)                                                       */
/* ========================================================================= */

#define PN532_CMD_SET_PARAMETERS  0x12
#define PN532_RSP_SET_PARAMETERS  0x13

/* RFU bits that must be zero: bit 3 and bit 7. */
#define PN532_PARAM_RFU_MASK  ((1 << 3) | (1 << 7))

esp_err_t pn532_set_parameters(pn532_handle_t h, uint8_t flags)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (flags & PN532_PARAM_RFU_MASK) {
        ESP_LOGE(TAG, "SetParameters: RFU bits set (0x%02x)", flags);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_SET_PARAMETERS,
                                       &flags, 1);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1];
    size_t len = 0;
    err = pn532_receive_response(h, buf, sizeof(buf), &len, 100);
    if (err != ESP_OK) {
        return err;
    }

    if (len < 1 || buf[0] != PN532_RSP_SET_PARAMETERS) {
        ESP_LOGE(TAG, "SetParameters: unexpected response cmd 0x%02x",
                 len > 0 ? buf[0] : 0);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Parameters set: 0x%02x", flags);
    return ESP_OK;
}

/* ========================================================================= */
/* TgInitAsTarget (0x8C)                                                      */
/* ========================================================================= */

#define PN532_CMD_TG_INIT_AS_TARGET  0x8C
#define PN532_RSP_TG_INIT_AS_TARGET  0x8D

esp_err_t pn532_tg_init_as_target(pn532_handle_t h,
                                  const pn532_tg_init_params_t *params,
                                  pn532_tg_init_result_t *result,
                                  uint32_t timeout_ms)
{
    if (h == NULL || params == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (params->gt_len > PN532_TG_MAX_GTK_LEN ||
        params->tk_len > PN532_TG_MAX_GTK_LEN) {
        ESP_LOGE(TAG, "TgInitAsTarget: gt/tk too long (%u/%u)",
                 params->gt_len, params->tk_len);
        return ESP_ERR_INVALID_ARG;
    }

    /* Build the parameter buffer:
     * Mode(1) + MifareParams(6) + FeliCaParams(18) + NFCID3t(10)
     * + LenGt(1) + Gt(gt_len) + LenTk(1) + Tk(tk_len)
     */
    const size_t max_params = 1 + 6 + 18 + 10 + 1 + PN532_TG_MAX_GTK_LEN + 1 + PN532_TG_MAX_GTK_LEN;
    uint8_t buf_params[max_params];
    size_t off = 0;

    buf_params[off++] = params->mode;

    /* MifareParams: sens_res(2) + nfcid1(3) + sel_res(1) = 6 bytes */
    memcpy(&buf_params[off], params->sens_res, 2);
    off += 2;
    memcpy(&buf_params[off], params->nfcid1, 3);
    off += 3;
    buf_params[off++] = params->sel_res;

    /* FeliCaParams: nfcid2(8) + pad(8) + system_code(2) = 18 bytes */
    memcpy(&buf_params[off], params->nfcid2, 8);
    off += 8;
    memcpy(&buf_params[off], params->pad, 8);
    off += 8;
    memcpy(&buf_params[off], params->system_code, 2);
    off += 2;

    /* NFCID3t: 10 bytes */
    memcpy(&buf_params[off], params->nfcid3, 10);
    off += 10;

    /* LenGt + Gt */
    buf_params[off++] = params->gt_len;
    if (params->gt_len > 0 && params->gt != NULL) {
        memcpy(&buf_params[off], params->gt, params->gt_len);
        off += params->gt_len;
    }

    /* LenTk + Tk */
    buf_params[off++] = params->tk_len;
    if (params->tk_len > 0 && params->tk != NULL) {
        memcpy(&buf_params[off], params->tk, params->tk_len);
        off += params->tk_len;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_INIT_AS_TARGET,
                                       buf_params, off);
    if (err != ESP_OK) {
        return err;
    }

    /* This command blocks until an external initiator activates us.
     * The PN532 enters power-down waiting for RF; IRQ asserts on activation.
     * Use the caller-supplied timeout. */
    uint8_t rsp[2];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "TgInitAsTarget: no initiator within %"PRIu32" ms",
                     timeout_ms);
        }
        return err;
    }

    /* Response: D5 8D Status Mode [Tg] [InitiatorCommand[]]
     * Minimum: status(1) + mode(1) = 2 bytes. */
    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_INIT_AS_TARGET) {
        ESP_LOGE(TAG, "TgInitAsTarget: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t status = rsp[1];
    if (status != PN532_ERR_NONE) {
        ESP_LOGE(TAG, "TgInitAsTarget: PN532 error 0x%02x", status);
        return ESP_FAIL;
    }

    result->mode = (rsp_len >= 3) ? rsp[2] : 0;
    ESP_LOGI(TAG, "TgInitAsTarget: activated, mode=0x%02x", result->mode);
    return ESP_OK;
}

/* ========================================================================= */
/* TgGetData (0x86)                                                           */
/* ========================================================================= */

#define PN532_CMD_TG_GET_DATA  0x86
#define PN532_RSP_TG_GET_DATA  0x87

/* Status byte bit masks for TgGetData. */
#define PN532_TG_STATUS_NAD_BIT   (1 << 7)
#define PN532_TG_STATUS_MI_BIT    (1 << 6)
#define PN532_TG_STATUS_ERR_MASK  0x3F

esp_err_t pn532_tg_get_data(pn532_handle_t h,
                            uint8_t *buf,
                            size_t buf_size,
                            size_t *out_len,
                            uint32_t timeout_ms)
{
    if (h == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;

    /* Temporary buffer for chaining accumulation.
     * Max DataIn is 262 bytes per response; worst case with chaining
     * we accumulate up to PN532_MAX_PAYLOAD_LEN bytes. */
    uint8_t accum[PN532_MAX_PAYLOAD_LEN];
    size_t accum_len = 0;
    bool chaining = true;

    while (chaining) {
        esp_err_t err = pn532_send_command(h, PN532_CMD_TG_GET_DATA, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }

        /* Response buffer: status(1) + up to 262 data bytes + possible NAD. */
        uint8_t rsp[1 + PN532_TG_MAX_DATA_LEN + 1];
        size_t rsp_len = 0;
        err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }

        /* Response: D5 87 Status [NAD] [DataIn...] */
        if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_GET_DATA) {
            ESP_LOGE(TAG, "TgGetData: unexpected response cmd 0x%02x",
                     rsp_len > 0 ? rsp[0] : 0);
            return ESP_FAIL;
        }

        const uint8_t status = rsp[1];
        const uint8_t error_code = status & PN532_TG_STATUS_ERR_MASK;

        if (error_code != PN532_ERR_NONE) {
            ESP_LOGE(TAG, "TgGetData: PN532 error 0x%02x", error_code);
            return ESP_FAIL;
        }

        /* Determine data offset: skip status byte and optional NAD byte. */
        size_t data_offset = 2;  /* past cmd byte and status byte */
        if (status & PN532_TG_STATUS_NAD_BIT) {
            data_offset += 1;  /* skip NAD byte */
        }

        const size_t data_len = (rsp_len > data_offset) ? (rsp_len - data_offset) : 0;

        /* Accumulate data. */
        if (accum_len + data_len > sizeof(accum)) {
            ESP_LOGE(TAG, "TgGetData: accumulated data overflow");
            return ESP_ERR_INVALID_SIZE;
        }
        if (data_len > 0) {
            memcpy(&accum[accum_len], &rsp[data_offset], data_len);
            accum_len += data_len;
        }

        chaining = (status & PN532_TG_STATUS_MI_BIT) != 0;
    }

    /* Copy accumulated data to caller buffer. */
    if (accum_len > buf_size) {
        ESP_LOGE(TAG, "TgGetData: data %u > buffer %u",
                 (unsigned)accum_len, (unsigned)buf_size);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buf, accum, accum_len);
    *out_len = accum_len;
    return ESP_OK;
}

/* ========================================================================= */
/* TgSetData (0x8E)                                                           */
/* ========================================================================= */

#define PN532_CMD_TG_SET_DATA  0x8E
#define PN532_RSP_TG_SET_DATA  0x8F

esp_err_t pn532_tg_set_data(pn532_handle_t h,
                            const uint8_t *data,
                            size_t data_len,
                            uint32_t timeout_ms)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > PN532_TG_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "TgSetData: data_len %u > max %u",
                 (unsigned)data_len, PN532_TG_MAX_DATA_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = pn532_send_command(h, PN532_CMD_TG_SET_DATA,
                                       data, data_len);
    if (err != ESP_OK) {
        return err;
    }

    /* Response: D5 8F Status — arrives after the RF exchange completes. */
    uint8_t rsp[2];
    size_t rsp_len = 0;
    err = pn532_receive_response(h, rsp, sizeof(rsp), &rsp_len, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (rsp_len < 2 || rsp[0] != PN532_RSP_TG_SET_DATA) {
        ESP_LOGE(TAG, "TgSetData: unexpected response cmd 0x%02x",
                 rsp_len > 0 ? rsp[0] : 0);
        return ESP_FAIL;
    }

    const uint8_t status = rsp[1];
    if (status != PN532_ERR_NONE) {
        ESP_LOGE(TAG, "TgSetData: PN532 error 0x%02x", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}
