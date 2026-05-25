import { useEffect, useRef, useState } from 'react';
import { Wifi, Shield, Info, CheckCircle2, Bluetooth } from 'lucide-react';
// We use esp-ble-prov for ESP32 provisioning
import { ESPProvisioner, Security1 } from 'esp-ble-prov';

const DEVICE_NAME_PREFIX = 'PROV_';
const DEFAULT_ESP_PROV_SERVICE_UUID = '0000ffff-0000-1000-8000-00805f9b34fb';
const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

type Step = 'connect' | 'pop' | 'scanning' | 'scan' | 'success';

function getErrorMessage(error: unknown): string {
  if (error instanceof Error && error.message) {
    return error.message;
  }

  return 'An unexpected error occurred.';
}

function decodeSsid(ssid: Uint8Array | null | undefined): string {
  return ssid ? textDecoder.decode(ssid).trim() : '';
}

export default function App() {
  const provisionerRef = useRef(
    new ESPProvisioner({
      deviceNamePrefix: DEVICE_NAME_PREFIX,
      serviceUUID: DEFAULT_ESP_PROV_SERVICE_UUID,
      security: new Security1(),
    }),
  );

  const [step, setStep] = useState<Step>('connect');
  const [pop, setPop] = useState('');
  const [ssids, setSsids] = useState<string[]>([]);
  const [selectedSsid, setSelectedSsid] = useState('');
  const [password, setPassword] = useState('');
  const [device, setDevice] = useState<{name: string} | null>(null);
  const [loading, setLoading] = useState(false);
  const [errorMsg, setErrorMsg] = useState('');

  useEffect(() => {
    const provisioner = provisionerRef.current;

    return () => {
      void provisioner.disconnect();
    };
  }, []);

  // 1. Connect to BLE Device
  const connectDevice = async () => {
    setLoading(true);
    setErrorMsg('');
    try {
      if (!('bluetooth' in navigator)) {
        throw new Error('Web Bluetooth is not available in this browser.');
      }

      // Connect to any device broadcasting the configured device prefix
      const connectedDevice = await provisionerRef.current.connect();
      setDevice({ name: connectedDevice.name || 'ESP32 Device' });
      setStep('pop');
    } catch (error: unknown) {
      setErrorMsg(getErrorMessage(error) || 'Failed to connect. Ensure Bluetooth is on and device is ready.');
      console.error(error);
    } finally {
      setLoading(false);
    }
  };

  // 2. Negotiate Security with Proof of Possession (PoP)
  const verifyPop = async () => {
    setLoading(true);
    setErrorMsg('');
    try {
      const normalizedPop = pop.trim();
      if (!normalizedPop) {
        throw new Error('PIN is required.');
      }

      // Configure Security1 with PoP before session establishment
      provisionerRef.current.security = new Security1({ pop: normalizedPop });
      await provisionerRef.current.establishSession();

      // If session initialized successfully, scan for Wi-Fi networks
      setStep('scanning');
      await scanNetworks();
    } catch (error: unknown) {
      setErrorMsg('Invalid PIN or session failed to initialize.');
      console.error(error);
    } finally {
      setLoading(false);
    }
  };

  // 3. Scan for nearby Wi-Fi networks
  const scanNetworks = async () => {
    try {
      const networks = await provisionerRef.current.scan();
      const uniqueSsids = Array.from(
        new Set(
          networks
            .map((network) => decodeSsid(network.ssid))
            .filter((ssid) => ssid.length > 0),
        ),
      );

      setSsids(uniqueSsids);
      setStep('scan');
    } catch (error: unknown) {
      setErrorMsg('Failed to scan networks. Is the ESP32 too far from the router?');
      console.error(error);
      setStep('pop'); // Go back so they can try again
    }
  };

  // 4. Send Credentials to ESP32
  const startProvisioning = async () => {
    setLoading(true);
    setErrorMsg('');
    try {
      await provisionerRef.current.sendCredentials({
        ssid: textEncoder.encode(selectedSsid),
        passphrase: textEncoder.encode(password),
      });

      // Disconnect cleanly so ESP32 can reboot/connect
      await provisionerRef.current.disconnect();
      setStep('success');
    } catch (error: unknown) {
      setErrorMsg(getErrorMessage(error) || 'Provisioning failed. Check your password.');
      console.error(error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="min-h-screen bg-neutral-50 dark:bg-neutral-900 flex items-center justify-center p-4 font-sans text-neutral-900 dark:text-neutral-100">
      <div className="max-w-md w-full bg-white dark:bg-neutral-800 rounded-2xl shadow-xl overflow-hidden border border-neutral-100 dark:border-neutral-700 transition-all">

        {/* Header */}
        <div className="bg-blue-600 p-6 text-white flex flex-col items-center">
          <div className="bg-white/20 p-3 rounded-full mb-3">
            <Wifi className="w-8 h-8" />
          </div>
          <h1 className="text-xl font-semibold">Device Setup</h1>
          {device && <p className="text-blue-100 text-sm mt-1">Connected to {device.name}</p>}
        </div>

        {/* Content Area */}
        <div className="p-6">
          {/* STEP 1: Connect */}
          {step === 'connect' && (
            <div className="flex flex-col items-center text-center space-y-6">
              <p className="text-neutral-600 dark:text-neutral-400">
                To get started, we need to securely connect to your new device via Bluetooth.
              </p>

              <div className="bg-blue-50 dark:bg-blue-900/20 text-blue-800 dark:text-blue-200 p-4 rounded-xl flex items-start text-sm text-left">
                <Info className="w-5 h-5 mr-3 shrink-0 mt-0.5" />
                <p>Ensure your device is plugged in and the blue light is blinking. You may need to grant Bluetooth permissions in your browser.</p>
              </div>

              <button
                onClick={connectDevice}
                disabled={loading}
                className="w-full bg-blue-600 hover:bg-blue-700 disabled:bg-blue-400 text-white font-medium py-3 px-4 rounded-xl transition flex justify-center items-center cursor-pointer"
              >
                {loading ? (
                  <div className="w-5 h-5 border-2 border-white/30 border-t-white rounded-full animate-spin"></div>
                ) : (
                  <>
                    <Bluetooth className="w-5 h-5 mr-2" />
                    Connect via Bluetooth
                  </>
                )}
              </button>
              {errorMsg && <p className="text-red-500 text-sm">{errorMsg}</p>}
            </div>
          )}

          {/* STEP 2: Proof of Possession */}
          {step === 'pop' && (
            <div className="space-y-5">
              <div className="text-center mb-6">
                <h2 className="text-lg font-medium">Security Verification</h2>
                <p className="text-neutral-500 text-sm mt-1">Enter the PIN printed on your device.</p>
              </div>

              <div>
                <label className="block text-sm font-medium text-neutral-700 dark:text-neutral-300 mb-1">
                  Device PIN (PoP)
                </label>
                <div className="relative">
                  <div className="absolute inset-y-0 left-0 pl-3 flex items-center pointer-events-none">
                    <Shield className="h-5 w-5 text-neutral-400" />
                  </div>
                  <input
                    type="text"
                    value={pop}
                    onChange={(e) => setPop(e.target.value.toUpperCase())}
                    className="block w-full pl-10 pr-3 py-3 border border-neutral-300 dark:border-neutral-600 rounded-xl bg-neutral-50 dark:bg-neutral-900 focus:ring-2 focus:ring-blue-500 focus:border-blue-500"
                    placeholder="e.g. HELLUM123"
                  />
                </div>
              </div>

              {errorMsg && <p className="text-red-500 text-sm">{errorMsg}</p>}

              <button
                onClick={verifyPop}
                disabled={loading || !pop}
                className="w-full bg-blue-600 hover:bg-blue-700 disabled:bg-neutral-300 text-white font-medium py-3 px-4 rounded-xl transition flex justify-center items-center cursor-pointer"
              >
                {loading ? (
                  <div className="w-5 h-5 border-2 border-white/30 border-t-white rounded-full animate-spin"></div>
                ) : 'Verify'}
              </button>
            </div>
          )}

          {/* SCANNING LOADING STATE */}
          {step === 'scanning' && (
             <div className="flex flex-col items-center justify-center py-8 space-y-4">
               <div className="w-8 h-8 border-4 border-blue-200 border-t-blue-600 rounded-full animate-spin"></div>
               <p className="text-neutral-500 font-medium">Scanning for Wi-Fi networks...</p>
             </div>
          )}

          {/* STEP 3: Scan & Select Network */}
          {step === 'scan' && (
            <div className="space-y-5">
              <div className="text-center mb-4">
                <h2 className="text-lg font-medium">Select Wi-Fi</h2>
                <p className="text-neutral-500 text-sm mt-1">Choose the 2.4GHz network for the device.</p>
              </div>

              {ssids.length === 0 ? (
                <div className="text-center p-4 bg-neutral-100 dark:bg-neutral-800 rounded-xl">
                  <p className="text-neutral-500 text-sm">No networks found.</p>
                  <button onClick={scanNetworks} className="text-blue-500 mt-2 text-sm hover:underline">Rescan</button>
                </div>
              ) : (
                <div className="space-y-2 max-h-[200px] overflow-y-auto pr-2 custom-scrollbar">
                  {ssids.map((ssid) => (
                    <button
                      key={ssid}
                      onClick={() => setSelectedSsid(ssid)}
                      className={`w-full text-left px-4 py-3 rounded-xl border flex items-center justify-between transition-colors cursor-pointer ${
                        selectedSsid === ssid
                          ? 'border-blue-500 bg-blue-50 dark:bg-blue-900/20 text-blue-700 dark:text-blue-300'
                          : 'border-neutral-200 dark:border-neutral-700 hover:bg-neutral-50 dark:hover:bg-neutral-700/50'
                      }`}
                    >
                      <span className="font-medium">{ssid}</span>
                      <Wifi className="w-5 h-5 opacity-50" />
                    </button>
                  ))}
                </div>
              )}

              {selectedSsid && (
                <div className="pt-4 border-t border-neutral-200 dark:border-neutral-700">
                  <label className="block text-sm font-medium text-neutral-700 dark:text-neutral-300 mb-1">
                    Password for {selectedSsid}
                  </label>
                  <input
                    type="password"
                    value={password}
                    onChange={(e) => setPassword(e.target.value)}
                    className="block w-full px-3 py-3 border border-neutral-300 dark:border-neutral-600 rounded-xl bg-neutral-50 dark:bg-neutral-900 focus:ring-2 focus:ring-blue-500 focus:border-blue-500"
                    placeholder="Network password"
                  />
                </div>
              )}

              {errorMsg && <p className="text-red-500 text-sm">{errorMsg}</p>}

              <div className="flex space-x-3 mt-4">
                <button
                  onClick={scanNetworks}
                  disabled={loading}
                  className="px-4 py-3 rounded-xl border border-neutral-300 dark:border-neutral-600 hover:bg-neutral-100 dark:hover:bg-neutral-700 transition cursor-pointer"
                  title="Rescan"
                >
                  <Wifi className="w-5 h-5" />
                </button>
                <button
                  onClick={startProvisioning}
                  disabled={loading || !selectedSsid || !password}
                  className="flex-1 bg-blue-600 hover:bg-blue-700 disabled:bg-neutral-300 text-white font-medium py-3 px-4 rounded-xl transition flex justify-center items-center cursor-pointer"
                >
                  {loading ? (
                    <div className="w-5 h-5 border-2 border-white/30 border-t-white rounded-full animate-spin"></div>
                  ) : 'Connect Device'}
                </button>
              </div>
            </div>
          )}

          {/* STEP 4: Success */}
          {step === 'success' && (
            <div className="flex flex-col items-center text-center space-y-4 py-6">
              <div className="text-green-500">
                <CheckCircle2 className="w-16 h-16" />
              </div>
              <h2 className="text-2xl font-semibold">Provisioned!</h2>
              <p className="text-neutral-600 dark:text-neutral-400">
                Your device has successfully connected to {selectedSsid} and is now online. You can close this page.
              </p>
              <button
                onClick={() => window.location.reload()}
                className="mt-6 px-6 py-2 bg-neutral-100 dark:bg-neutral-700 hover:bg-neutral-200 font-medium rounded-xl transition cursor-pointer"
              >
                Setup Another Device
              </button>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
