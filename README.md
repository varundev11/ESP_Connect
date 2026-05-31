# ESP32 Secure OTA & BLE Provisioning Firmware

A production-ready ESP32 firmware template built with **ESP-IDF** that provides robust, secure Over-The-Air (OTA) updates, Anti-Bricking Rollback protection, and BLE-based Wi-Fi provisioning. 

This architecture is designed for headless IoT devices deployed in the wild where network conditions change and physical recovery from bad firmware updates is impossible.

---

## 📑 Table of Contents
1. [Features](#features)
2. [Hardware Setup & Reset Button](#hardware-setup--reset-button)
3. [Wi-Fi Provisioning Guide](#wi-fi-provisioning-guide)
4. [Secure OTA Protocol & Schemas](#secure-ota-protocol--schemas)
5. [Anti-Bricking & Rollback Mechanism](#anti-bricking--rollback-mechanism)
6. [Development & Build Guide](#development--build-guide)
7. [Security Considerations](#security-considerations)

---

## ✨ Features
* **BLE Provisioning:** No hardcoded credentials. Connects via Bluetooth (NimBLE) on first boot.
* **Smart Network Fallback:** Hardware button to trigger network reset. Reverts to old credentials if the new network fails.
* **HTTPS Secure OTA:** Cryptographically verifies the OTA server using embedded Root CA certificates.
* **Dual-App Partitioning:** Uses `ota_0` and `ota_1`. Updates stream directly to the inactive partition to prevent RAM exhaustion.
* **Automatic Rollback:** Validates network connectivity on first boot. If the device crashes or fails to connect, it rolls back to the previous firmware.

---

## 🔌 Hardware Setup & Reset Button

To allow users to change Wi-Fi networks after the initial setup, this firmware monitors a dedicated hardware button. 

**Pin Selection:** `GPIO 32`
* **Why GPIO 32?** It is an RTC GPIO, supports both input/output, doesn't interfere with standard strapping pins (like GPIO 0, 2, 5, 12, 15) which can prevent boot, and is rarely used by standard SPI/I2C peripherals.

**Wiring Instructions:**
1. Connect one leg of a momentary push-button to **GPIO 32**.
2. Connect the other leg to **GND** (Ground).
3. *Note:* No external pull-up resistor is required. The firmware enables the ESP32's internal pull-up resistor (`pull_up_en = 1`).

**How to trigger Network Reset:**
* Press the button **5 times rapidly** (within a 3-second window).
* The device will safely back up current credentials to RAM, erase flash credentials, and reboot into BLE Provisioning mode.
* If provisioning to the new network fails, it automatically restores the old credentials from RAM and reconnects.

---

## 📱 Wi-Fi Provisioning Guide

On the very first boot (or after 5 button presses), the device broadcasts a BLE service to accept Wi-Fi credentials. 

### Technical Details
* **Transport:** BLE (Bluetooth Low Energy) via NimBLE stack.
* **Security Protocol:** `Security 1` (Requires Proof of Possession - PoP).
* **Provisioning Service UUID:** `55cc035e-fb27-4f80-be02-3c60828b7451`
* **Device Name:** `PROV_XXXXXX` (Last 3 bytes of MAC in hex).
* **Proof of Possession (PoP):** Unique per device, persisted in NVS under namespace `prov` and key `pop`.

### Connecting to the Device

**Option 1: Using Espressif Apps (Easiest)**
1. Download **Espressif Provisioning** on [iOS (App Store)](https://apps.apple.com/us/app/espressif-provisioning/id1474012698) or [Android (Google Play)](https://play.google.com/store/apps/details?id=com.espressif.provble).
2. Open the app, select **Provision Device**, and choose **BLE**.
3. Select `PROV_XXXX`.
4. Enter the PoP PIN for that specific device.
5. Select your home 2.4GHz Wi-Fi and enter the password.

**Option 2: Custom Web Frontend (Web Bluetooth)**
You can build a custom web dashboard using [`esp-ble-prov`](https://www.npmjs.com/package/esp-ble-prov).
This allows users to provision the device directly from a Chrome browser on PC or Android without downloading an app.

---

## ☁️ Secure OTA Protocol & Schemas

Once connected to Wi-Fi, the device checks for updates against a remote server (`https://otaapi.hellum.dev/smart_switch/check`). 

### 1. Update Check Request
The ESP32 makes an HTTP `GET` request, passing its MAC address and current firmware version as URL parameters.
```http
GET /smart_switch/check?mac=AA:BB:CC:DD:EE:FF&ver=1.0.0 HTTP/1.1
Host: otaapi.hellum.dev
```

### 2. Server Response Schema
Your backend API must return a JSON response matching this schema.

**If an update is available:**
```json
{
  "update_available": true,
  "version": "1.0.1",
  "firmware_url": "https://otaapi.hellum.dev/firmware/v1.0.1.bin"
}
```
*Note: The `firmware_url` MUST use `https://` and map to the server validated by the embedded Root CA.*

**If no update is available:**
```json
{
  "update_available": false
}
```

### 3. TLS Certificate Verification
The device verifies the server using the `server_root_ca.pem` embedded in the binary. This project uses the Let's Encrypt `ISRG Root X1` certificate, which means your OTA server's HTTPS must be secured by Let's Encrypt (which is free and standard).

---

## 🛡️ Anti-Bricking & Rollback Mechanism

This project utilizes a **2-App Partition Scheme** (`ota_0` and `ota_1`).

1. **Download:** The new firmware (`1.0.1.bin`) is streamed directly into the *inactive* partition (e.g., `ota_1`).
2. **Boot Flag:** The bootloader is told to boot from `ota_1` on the next restart, marking it as `ESP_OTA_IMG_PENDING_VERIFY`.
3. **Validation Boot:** The device reboots. It attempts to initialize hardware and connect to Wi-Fi.
4. **Confirmation:** If it successfully acquires an IP address, it calls `esp_ota_mark_app_valid_cancel_rollback()`. The update is permanently confirmed.
5. **Rollback:** If the firmware crashes (Watchdog trigger) or fails to connect to Wi-Fi, it will reboot. The bootloader detects that the app was not confirmed, marks it as `ESP_OTA_IMG_INVALID`, and rolls back to `ota_0` (the last known good firmware).

---

## 🛠️ Development & Build Guide

### Prerequisites
* ESP-IDF v5.0 or higher.
* ESP32 Development Board with **4MB Flash** minimum.

### Setup Instructions
1. Clone this repository.
2. Ensure you have the Let's Encrypt Root CA in the `server_certs` directory (already included). If your server uses AWS or a custom CA, replace `server_root_ca.pem`.
3. Load the ESP-IDF environment:
   ```bash
   . $HOME/.espressif/esp-idf/export.sh
   ```
4. Set the target (if first time):
   ```bash
   idf.py set-target esp32
   ```
5. Build the project:
   ```bash
   idf.py build
   ```
6. Flash and Monitor:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```
   *(Replace `/dev/ttyUSB0` with your actual serial port).*

---

## 🔒 Security Considerations for Production

Before moving this to mass production, implement the following:
1. **PoP Manufacturing Workflow:** During manufacturing, read/write each device PoP from NVS (`prov/pop`), print it as QR/text on the device label, and keep the backend inventory mapping for support.
2. **NVS Encryption:** Enable NVS Encryption via `menuconfig` to ensure Wi-Fi passwords stored on flash cannot be physically extracted by reading the flash chip.
3. **Secure Boot v2:** Enable Secure Boot to ensure only firmware signed by your private ECDSA key can execute on the device, preventing malicious OTA bin injections even if the server is compromised.
