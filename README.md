# PN532 NFC Driver

ESP-IDF v5.x driver for the NXP PN532 NFC controller. Implements the **physical and data-link layers**: transport abstraction, PN532 frame construction/parsing, and the command/response handshake.

## Project Structure

```
pn532_driver/
├── components/pn532/
│   ├── CMakeLists.txt              # Component build definition
│   ├── include/
│   │   ├── pn532.h                 # Public API — core driver
│   │   └── pn532_i2c.h             # Public API — I2C transport backend
│   └── src/
│       ├── pn532.c                 # Core: frame builder, parser, cmd/resp
│       └── pn532_i2c.c             # I2C transport: bus/device, IRQ, mutex
├── main/
│   └── pn532_driver.c              # Application entry (smoke test)
├── docs/                           # Detailed documentation
└── README.md                       # You are here
```

## Documentation

| Document | Contents |
|----------|----------|
| [Architecture](docs/architecture.md) | Layered design, module relationships, data flow |
| [I2C Transport](docs/i2c-transport.md) | I2C backend internals: bus init, IRQ, mutex, status-byte protocol |
| [Core Driver](docs/core-driver.md) | PN532 frame format, builder, parser, ACK handshake, command/response |
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
- [x] Smoke test (GetFirmwareVersion over I2C)

**TODO:**
- [ ] High-level command wrappers
  - [ ] `pn532_get_firmware_version()` — read IC, version, revision, features
  - [ ] `pn532_sam_configuration()` — configure SAM (Secure Access Module) mode
  - [ ] `pn532_in_list_passive_target()` — detect Type A/B cards in field
  - [ ] `pn532_tg_init_as_target()` — card emulation (listen for external reader)
  - [ ] `pn532_in_data_exchange()` — send/receive data with a detected tag
  - [ ] `pn532_in_select()` — select a detected target by SENS_RES/ATQA
  - [ ] `pn532_in_release()` — release a selected target
  - [ ] `pn532_in_autopoll()` — automatic multi-protocol polling loop
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

## Smoke Test Results

Verified on ESP32-S3 with PN532 at I2C address 0x24:

```
I (286) PN532_TEST: === PN532 smoke test ===
I (286) PN532: PN532 detected on I2C bus at 0x24
I (296) PN532: I2C transport ready (IRQ mode, gpio 4, 400000 Hz)
I (296) PN532_TEST: I2C transport created
I (306) PN532_TEST: PN532 driver initialised
I (316) PN532_TEST: PN532 woken
I (326) PN532_TEST: Command sent, ACK received
I (336) PN532_TEST:   IC: 0x32  Ver: 0.1  Rev: 6  Features: 0x07
I (346) PN532_TEST: PN532 smoke test PASSED
```

IC 0x32 = PN532. Features 0x07 = ISO 14443A + ISO 14443B + NFCIP-1 all supported.

## Building & Flashing

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
