# PN532 NFC Driver

ESP-IDF v5.x driver for the NXP PN532 NFC controller. Implements the **physical/link layer** and a **high-level command layer** for NFC target (card emulation) operation.

## Project Structure

```
pn532_driver/
├── components/pn532/
│   ├── CMakeLists.txt              # Component build definition
│   ├── include/
│   │   ├── pn532.h                 # Public API — core driver
│   │   ├── pn532_cmd.h             # Public API — command layer
│   │   └── pn532_i2c.h             # Public API — I2C transport backend
│   └── src/
│       ├── pn532.c                 # Core: frame builder, parser, cmd/resp
│       ├── pn532_cmd.c             # Command wrappers (GetFirmwareVersion, TgInitAsTarget, …)
│       └── pn532_i2c.c             # I2C transport: bus/device, IRQ, mutex
├── main/
│   └── pn532_driver.c              # Command-layer test suite
├── docs/                           # Detailed documentation
└── README.md                       # You are here
```

## Documentation

| Document | Contents |
|----------|----------|
| [Architecture](docs/architecture.md) | Layered design, module relationships, data flow |
| [I2C Transport](docs/i2c-transport.md) | I2C backend internals: bus init, IRQ, mutex, status-byte protocol |
| [Core Driver](docs/core-driver.md) | PN532 frame format, builder, parser, ACK handshake, command/response |
| [Command Layer](docs/command-layer.md) | Command wrappers: GetFirmwareVersion, SAMConfiguration, TgInitAsTarget, … |
| [API Reference](docs/api-reference.md) | Every public function, type, and constant |
| [ESP-IDF Integration](docs/esp-idf-integration.md) | ESP-IDF APIs used, component deps, FreeRTOS primitives |

## Status

**Implemented:**
- [x] Transport abstraction layer (vtable-based, transport-agnostic)
- [x] I2C transport backend (IRQ mode + polling mode)
- [x] PN532 frame builder (normal + extended frames)
- [x] PN532 frame parser (LCS/DCS validation, error frame detection)
- [x] Command send + ACK handshake
- [x] Response receive + NACK retransmit on CRC failure
- [x] Wakeup sequence
- [x] Command layer (`pn532_cmd.h`):
  - [x] `pn532_get_firmware_version()` — read IC, version, revision, features
  - [x] `pn532_sam_configuration()` — configure SAM mode (NORMAL, VIRTUAL_CARD, etc.)
  - [x] `pn532_set_parameters()` — set communication flags (NAD, DID, auto-ATR, etc.)
  - [x] `pn532_tg_init_as_target()` — card emulation (listen for external RF reader)
  - [x] `pn532_tg_get_data()` — receive data from NFC initiator (MI-bit chaining)
  - [x] `pn532_tg_set_data()` — send data to NFC initiator
- [x] Command-layer test suite (all commands, on-target)

**TODO:**
- [ ] Additional command wrappers
  - [ ] `pn532_in_list_passive_target()` — detect Type A/B cards in field
  - [ ] `pn532_in_data_exchange()` — send/receive data with a detected tag
  - [ ] `pn532_in_select()` — select a detected target by SENS_RES/ATQA
  - [ ] `pn532_in_release()` — release a selected target
  - [ ] `pn532_in_autopoll()` — automatic multi-protocol polling loop
  - [ ] `pn532_tg_set_meta_data()` / `pn532_tg_get_meta_data()`
  - [ ] `pn532_tg_response_to_initiator()` / `pn532_tg_get_initiator_command()`
- [ ] SPI transport backend
- [ ] UART transport backend
- [ ] NFC application layer
  - [ ] ISO-DEP (ISO 14443-4) protocol handling
  - [ ] MIFARE Classic/Ultralight command helpers
  - [ ] Card UID reading and NDEF parsing
  - [ ] ISO 14443 Type A emulation (for smart lock credential emulation)
  - [ ] Crypto / authentication helpers
- [ ] Unit tests
  - [ ] Frame builder/parser round-trip tests
  - [ ] Mock transport for protocol testing without hardware
  - [ ] Integration tests with real PN532

## Test Results

Verified on ESP32-S3 with PN532 at I2C address 0x24 (IRQ mode, GPIO 4):

```
I (287) PN532_CMD_TEST: ========================================
I (287) PN532_CMD_TEST:   PN532 Command-Layer Test Suite
I (287) PN532_CMD_TEST: ========================================
I (297) PN532: PN532 detected on I2C bus at 0x24
I (307) PN532: I2C transport ready (IRQ mode, gpio 4, 400000 Hz)
I (317) PN532: driver initialised
I (317) PN532_CMD_TEST: [PASS] pn532_wakeup
I (327) PN532_CMD_TEST: --- Running command tests ---
I (337) PN532_CMD_TEST: [PASS] GetFirmwareVersion  (err=ESP_OK)
I (337) PN532_CMD_TEST:   IC=0x32  Ver=0.1  Rev=6  Features=0x07
I (357) PN532_CMD_TEST: [PASS] SAMConfiguration (NORMAL, IRQ)  (err=ESP_OK)
I (367) PN532_CMD_TEST: [PASS] SetParameters (flags=0x00)  (err=ESP_OK)
I (377) PN532_CMD_TEST: [PASS] SetParameters (AUTO_ATR|RM_PREAMBLE)  (err=ESP_OK)
I (377) PN532_CMD_TEST: [PASS] SetParameters (RFU bit — should reject)  (err=ESP_ERR_INVALID_ARG)
I (1387) PN532_CMD_TEST: [SKIP] TgInitAsTarget (1 s timeout)  (err=ESP_ERR_TIMEOUT)
I (1397) PN532_CMD_TEST: [SKIP] TgGetData (500 ms timeout)  (err=ESP_FAIL)
I (1407) PN532_CMD_TEST: [SKIP] TgSetData (500 ms timeout)  (err=ESP_FAIL)
I (1407) PN532_CMD_TEST:   Results:  5 passed, 0 failed, 3 skipped
I (1427) PN532_CMD_TEST: COMMAND TEST SUITE COMPLETED
```

IC 0x32 = PN532. Features 0x07 = ISO 14443A + ISO 14443B + NFCIP-1 all supported.
The 3 SKIPs are TgInitAsTarget, TgGetData, TgSetData — these require an external NFC reader (phone) to activate the PN532 as a target.

## Building & Flashing

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
