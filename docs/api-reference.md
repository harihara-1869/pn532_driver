# API Reference

## Core Driver (`pn532.h`)

### Types

#### `pn532_handle_t`

```c
typedef struct pn532_t *pn532_handle_t;
```

Opaque handle to a PN532 driver instance. Created by `pn532_init`, destroyed by `pn532_deinit`.

#### `pn532_transport_ops_t`

```c
typedef struct {
    esp_err_t (*write)(void *ctx, const uint8_t *buf, size_t len);
    esp_err_t (*read_status)(void *ctx, uint8_t *status_out);
    esp_err_t (*read_frame)(void *ctx, uint8_t *buf, size_t len);
    esp_err_t (*wait_ready)(void *ctx, uint32_t timeout_ms);
    void      (*destroy)(void *ctx);
} pn532_transport_ops_t;
```

Transport abstraction vtable. All callbacks receive the transport-private `ctx` returned by the backend factory.

| Callback | Description | When called |
|----------|-------------|-------------|
| `write` | Send a complete frame | `pn532_send_command`, NACK retransmit |
| `read_status` | Read 1-byte status (polling only) | Polling-mode `wait_ready` |
| `read_frame` | Read status + frame in one transaction | `pn532_read_ack`, `pn532_receive_response` |
| `wait_ready` | Block until chip has data ready | Before every frame read |
| `destroy` | Release all transport resources | `pn532_deinit` |

#### `pn532_config_t`

```c
typedef struct {
    const pn532_transport_ops_t *ops;
    void *transport_ctx;
} pn532_config_t;
```

Configuration for `pn532_init`. `ops` must outlive the handle.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PN532_MAX_PAYLOAD_LEN` | 265 | Max TFI+DATA bytes in any frame |

### Functions

#### `pn532_init`

```c
esp_err_t pn532_init(const pn532_config_t *cfg, pn532_handle_t *out_handle);
```

Allocate and initialise a driver instance. No bus traffic — just binds transport to handle.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL cfg, ops, out_handle, or missing write/read_frame/wait_ready |
| `ESP_ERR_NO_MEM` | Allocation failed |

#### `pn532_deinit`

```c
void pn532_deinit(pn532_handle_t handle);
```

Destroy handle and invoke transport's `destroy` callback. Safe to call with NULL.

#### `pn532_send_command`

```c
esp_err_t pn532_send_command(pn532_handle_t h,
                             uint8_t cmd_code,
                             const uint8_t *params, size_t params_len);
```

Build a command frame, send it, wait for ACK. Does **not** read the response.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Command sent and ACKed |
| `ESP_ERR_INVALID_ARG` | NULL handle, or NULL params with non-zero len |
| `ESP_ERR_TIMEOUT` | ACK not received within 100ms |
| `ESP_FAIL` | Received frame is not a valid ACK |

#### `pn532_receive_response`

```c
esp_err_t pn532_receive_response(pn532_handle_t h,
                                 uint8_t *buf, size_t buf_size,
                                 size_t *out_len,
                                 uint32_t timeout_ms);
```

Wait for and parse a response frame. On CRC failure, sends NACK to request retransmit.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Valid response parsed |
| `ESP_ERR_INVALID_ARG` | NULL h, buf, or out_len |
| `ESP_ERR_TIMEOUT` | No frame within timeout |
| `ESP_ERR_INVALID_CRC` | LCS or DCS mismatch (NACK sent automatically) |
| `ESP_ERR_INVALID_SIZE` | Payload too large for buffer |
| `ESP_FAIL` | PN532 error frame (TFI 0x7F) |

**Output**: `buf` receives the DATA payload (response command byte + parameters). `*out_len` is the number of bytes written.

#### `pn532_wakeup`

```c
esp_err_t pn532_wakeup(pn532_handle_t h);
```

Wake the PN532 from Power-Down / LowVbat. Sends 6 bytes of 0x55, waits 2ms for oscillator.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Wakeup succeeded |
| `ESP_ERR_INVALID_ARG` | NULL handle |
| `ESP_ERR_TIMEOUT` | Bus error (chip may still have woken) |

---

## I2C Transport (`pn532_i2c.h`)

### Types

#### `pn532_i2c_config_t`

```c
typedef struct {
    int        sda_gpio;   // SDA GPIO (required)
    int        scl_gpio;   // SCL GPIO (required)
    int        irq_gpio;   // IRQ GPIO; -1 = polling mode
    i2c_port_t port;       // I2C port (e.g. I2C_NUM_0)
    uint32_t   clk_speed;  // Hz; 0 = 400 kHz default
} pn532_i2c_config_t;
```

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PN532_I2C_ADDRESS` | 0x24 | 7-bit I2C address |
| `PN532_I2C_DEFAULT_CLK_HZ` | 400000 | Default clock (400 kHz fast mode) |

### Functions

#### `pn532_i2c_create`

```c
esp_err_t pn532_i2c_create(const pn532_i2c_config_t *cfg,
                           pn532_transport_ops_t *ops_out,
                           void **ctx_out);
```

Create an I2C transport. Initialises bus, device, probe, IRQ, mutex.

| Return | Condition |
|--------|-----------|
| `ESP_OK` | Success |
| `ESP_ERR_INVALID_ARG` | NULL args or invalid GPIOs |
| `ESP_ERR_NO_MEM` | Allocation failed |
| `ESP_ERR_TIMEOUT` | PN532 not found on bus (probe failed) |
| Other `esp_err_t` | I2C or GPIO driver error |

#### `pn532_i2c_destroy`

```c
void pn532_i2c_destroy(void *ctx);
```

Destroy transport. Tears down IRQ ISR, I2C device, I2C bus, mutex. Safe with NULL.
