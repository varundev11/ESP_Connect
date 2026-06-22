# Dual-Stack Smart Home Switchboard Implementation

## Summary

This firmware now adds a centralized 4-channel switchboard state manager plus two synchronized control planes:

- Apple Home local control through Espressif HomeKit SDK (`esp-homekit-sdk`)
- Google/cloud control through secure ESP-MQTT over `mqtts://api.hellum.dev:8883`

Existing BLE Wi-Fi provisioning, HTTPS OTA with rollback validation, NVS initialization, and the GPIO 32 five-press network reset path were preserved.

## Hardware Mapping

Relays are active-low and are driven by `main/device_state_manager.c`:

| Device | Relay GPIO | Switch GPIO |
| --- | ---: | ---: |
| Light 1 | 13 | 34 |
| Light 2 | 26 | 35 |
| Fan | 14 | 36 |
| Plug | 27 | 39 |

Relay logic:

- `LOW` means relay ON.
- `HIGH` means relay OFF.
- Every relay GPIO is explicitly latched `HIGH` with `gpio_set_level(pin, 1)` before `gpio_set_direction()` is called.

Switch logic:

- GPIO 34, 35, 36, and 39 are input-only pins without internal pull-ups.
- Firmware configures them as no-pull inputs and expects external hardware pull-ups.
- Interrupts are configured for `GPIO_INTR_ANYEDGE`.
- Any stable edge, high-to-low or low-to-high, toggles the corresponding relay.
- Debounce is 50 ms.

## State Manager

`main/device_state_manager.c/.h` is the single source of truth.

All control inputs use:

- `device_state_manager_set_power()`
- `device_state_manager_toggle()`

Observers are notified after state changes, so HomeKit, MQTT, and physical switches remain synchronized without each stack owning relay GPIO directly.

## HomeKit

`main/homekit_bridge.c/.h` starts only after `IP_EVENT_STA_GOT_IP`.

Configuration:

- One HomeKit accessory: `Hellum Switchboard`
- Four services:
  - Lightbulb: `Light 1`
  - Lightbulb: `Light 2`
  - Fan v2: `Fan`
  - Outlet: `Plug`
- Setup code: `111-22-333`
- Setup ID: `ES32`
- HomeKit task stack: 8192 bytes

HomeKit writes update the state manager; state-manager notifications update HomeKit characteristics with `hap_char_update_val()`.

## MQTT

`main/mqtt_bridge.c/.h` starts only after `IP_EVENT_STA_GOT_IP`.

Configuration:

- Broker: `mqtts://api.hellum.dev:8883`
- TLS verification: ESP-IDF certificate bundle
- Command topic: `smarthome/device/<mac_address>/cmd`
- State topic: `smarthome/device/<mac_address>/state`
- Payload format: `{"device":"light1","state":"on"}`

Supported device names:

- `light1`
- `light2`
- `fan`
- `plug`

MQTT commands update the state manager; state-manager notifications publish state messages back to the cloud topic.

## Build Integration

Changed files:

- `CMakeLists.txt`
- `main/CMakeLists.txt`
- `sdkconfig.defaults`
- `main/main.c`
- `main/device_state_manager.c`
- `main/device_state_manager.h`
- `main/homekit_bridge.c`
- `main/homekit_bridge.h`
- `main/mqtt_bridge.c`
- `main/mqtt_bridge.h`

The official Espressif HomeKit SDK is referenced as an external SDK checkout:

```sh
/home/varun/esp/esp-homekit-sdk
```

The top-level `CMakeLists.txt` also honors:

```sh
ESP_HOMEKIT_SDK_PATH=/path/to/esp-homekit-sdk
```

During verification, ESP-IDF Component Manager generated:

- `dependencies.lock`
- `managed_components/`

These contain HomeKit SDK transitive registry dependencies such as `json_generator`, `json_parser`, `jsmn`, `libsodium`, and `mdns`.

## Production Partition Layout

The partition table was expanded for production OTA headroom:

```csv
# Name,   Type, SubType, Offset,  Size,      Flags
nvs,      data, nvs,     ,        0x6000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
ota_0,    app,  ota_0,   ,        0x1F0000,
ota_1,    app,  ota_1,   ,        0x1F0000,
```

Notes:

- NVS was increased from `0x4000` to `0x6000` for Wi-Fi provisioning state, HomeKit pairing data, and product keys such as provisioning PoP.
- Both OTA slots were increased from `1500K` to `0x1F0000` bytes.
- The old `certs` SPIFFS partition was removed because OTA TLS already uses `server_root_ca.pem` embedded by `main/CMakeLists.txt`.
- Dual OTA plus `otadata` and rollback validation remain enabled, so a bad OTA image can still roll back.

## Verification

Build command used:

```sh
source /home/varun/esp/esp-idf/export.sh
idf.py -B /tmp/esp_connect_build build
```

Result:

- Build completed successfully.
- Output binary: `/tmp/esp_connect_build/esp_connect_ota.bin`
- Binary size after production partition and size-optimization changes: `0x14ad00`
- Smallest OTA app partition: `0x1F0000`
- Remaining app partition space: `0xa5300` bytes, about 33%.
