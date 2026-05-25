# Web BLE Provisioning Frontend

React + Vite frontend for provisioning ESP32 Wi-Fi credentials over BLE from a browser using `esp-ble-prov`.

## Stack
- React 19 + TypeScript
- Vite 8
- Tailwind CSS 3
- `esp-ble-prov` (Security1 flow with PoP)

## Provisioning Flow
1. Connect to a device (`PROV_XXXXXX`) over Web Bluetooth.
2. Enter PoP/PIN (`Security1` session establishment).
3. Scan for nearby Wi-Fi APs.
4. Send SSID + passphrase to ESP32.
5. Wait for station connection result, then finish.

## Production Alignment
- BLE device discovery is filtered by:
  - `namePrefix = PROV_`
  - provisioning service UUID `55cc035e-fb27-4f80-be02-3c60828b7451`
- Frontend and firmware must use the same service UUID.

## Local Development
```bash
cd Frontend
npm install
npm run dev
```

Dev server: `http://localhost:5173`

## Build
```bash
npm run build
```

Artifacts: `Frontend/dist`

## Browser Requirements
- Chrome/Edge (desktop or Android)
- Secure context (`https://`), or `http://localhost` during development
- Bluetooth permission enabled
