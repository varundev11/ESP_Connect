# Web BLE Provisioning Frontend

React + Vite frontend for provisioning ESP32 Wi-Fi credentials over BLE from a browser using `esp-ble-prov`.

## Stack
- React 19 + TypeScript
- Vite 8
- Tailwind CSS 3
- `esp-ble-prov` (Security1 flow with PoP)

## Provisioning Flow
1. Connect to device (`PROV_XXXX`) over Web Bluetooth.
2. Enter PoP/PIN (`Security1` session establishment).
3. Scan for nearby Wi-Fi APs.
4. Send SSID + passphrase to ESP32.
5. Wait for station connection result, then finish.

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
