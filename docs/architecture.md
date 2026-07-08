# Architecture

## Layered Design

The driver is split into two layers connected by a vtable (function-pointer table). The core layer is transport-agnostic; the I2C backend is swappable.

```
┌─────────────────────────────────────────────────┐
│              Application (main/)                │
│         pn532_driver.c — smoke test             │
└────────────────────┬────────────────────────────┘
                     │ calls pn532_init / pn532_send_command / etc.
                     ▼
┌─────────────────────────────────────────────────┐
│           Core Driver (pn532.c)                 │
│                                                 │
│  Frame builder    Frame parser    ACK handler   │
│  pn532_build_frame()  pn532_parse_frame()       │
│  pn532_send_command()                           │
│  pn532_receive_response()                       │
│  pn532_wakeup()                                 │
└────────────────────┬────────────────────────────┘
                     │ calls ops->write / ops->read_frame / ops->wait_ready
                     ▼
┌─────────────────────────────────────────────────┐
│      Transport Vtable (pn532_transport_ops_t)   │
│                                                 │
│  .write()  .read_status()  .read_frame()        │
│  .wait_ready()  .destroy()                      │
└────────────────────┬────────────────────────────┘
                     │ implemented by
                     ▼
┌─────────────────────────────────────────────────┐
│        I2C Backend (pn532_i2c.c)                │
│                                                 │
│  i2c_master_transmit / i2c_master_receive       │
│  GPIO ISR (IRQ mode) or status-byte polling     │
│  FreeRTOS mutex (bus serialisation)             │
└─────────────────────────────────────────────────┘
```

## Module Responsibilities

### pn532.c — Core Driver

Knows the PN532 frame format and nothing about I2C/SPI/UART. Owns:

- **Frame building**: serialises cmd_code + params into a PN532 information frame (normal or extended) with preamble, start code, LEN, LCS, TFI, DATA, DCS, postamble.
- **Frame parsing**: locates start code, validates LCS/DCS checksums, checks TFI (0xD5), extracts DATA payload. Detects error frames (TFI 0x7F).
- **Command/response handshake**: sends frame, waits for ACK (6-byte fixed pattern), validates it. For responses: waits for readiness, over-reads frame, validates, sends NACK on CRC failure to request retransmit.
- **Wakeup**: sends 0x55 burst to wake the chip from Power-Down, waits for oscillator stabilisation.

### pn532_i2c.c — I2C Transport Backend

Implements the `pn532_transport_ops_t` vtable over ESP-IDF's `i2c_master` API. Owns:

- **I2C bus and device lifecycle**: creates master bus, adds PN532 device at address 0x24, probes to verify presence.
- **Bus mutex**: a FreeRTOS mutex serialises all I2C transactions (5-second timeout).
- **Write with retry**: retries up to 5 times on address NACK (the PN532 may NACK briefly after a prior exchange).
- **Status-byte protocol**: single-byte status read for polling; combined status+frame read in one transaction to avoid losing bytes to an early STOP.
- **Ready detection**: IRQ mode (GPIO ISR gives binary semaphore) or polling mode (repeated status reads at 1ms intervals).

## Data Flow: Send Command → Get Response

```
Application
  │
  ├─ pn532_send_command(h, 0x02, NULL, 0)
  │   │
  │   ├─ pn532_build_frame(0x02, NULL, 0)
  │   │   → [0x00 0x00 0xFF 0x02 0xFE 0xD4 0x02 0x2A 0x00]
  │   │
  │   ├─ tp_write(frame)  ──►  I2C: START → 0x24(W) → frame → STOP
  │   │
  │   └─ pn532_read_ack()
  │       ├─ tp_wait_ready()  ──  block on IRQ semaphore or poll status
  │       └─ tp_read_frame()  ──  I2C: START → 0x24(R) → 7 bytes → STOP
  │           → validate: [0x00 0x00 0xFF 0x00 0xFF 0x00] = ACK
  │
  └─ pn532_receive_response(h, buf, 16, &len, 1000)
      │
      ├─ tp_wait_ready(1000)
      │   └─ IRQ fires → semaphore given → returns ESP_OK
      │
      ├─ tp_read_frame(raw, 277)  ──  over-read status + worst-case frame
      │
      └─ pn532_parse_frame(raw)
          ├─ skip status byte
          ├─ find start code (0x00 0xFF)
          ├─ read LEN + validate LCS
          ├─ read TFI (expect 0xD5) + DATA + DCS, validate checksum
          └─ copy DATA into application buffer
              → buf = [0x03, 0x32, 0x01, 0x06, 0x07] (GetFirmwareVersion response)
```

## Thread Safety

- The I2C backend uses a single FreeRTOS mutex (`bus_lock`/`bus_unlock`) around every I2C transaction. This serialises access from multiple FreeRTOS tasks.
- The core driver does not add its own locking — it assumes one logical user per handle.
- The GPIO ISR (`pn532_irq_isr`) runs in IRAM and uses `xSemaphoreGiveFromISR` to signal the wait_ready function.
