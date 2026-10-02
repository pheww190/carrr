package com.example.blerover;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * BLE central for the Rover.
 *
 *   - SCAN builds one list of devices: already-paired ones first (tagged
 *     PAIRED), then live scan results (tagged ROVER if they advertise our
 *     service, NEARBY otherwise). Name + MAC + RSSI are shown, so the row you
 *     tap is unambiguous.
 *   - tap a row to connect. Nothing connects on its own.
 *
 * Frame (matches rover_proto.h):
 *   [0] throttle int8  [1] steering int8  [2] flags uint8  [3] seq uint8
 */
public class MainActivity extends Activity {

    /* ---------------- protocol ---------------- */
    static final UUID SVC  = UUID.fromString("a1b2c3d4-0001-4a5b-8c7d-1e2f3a4b5c6d");
    static final UUID CMD  = UUID.fromString("a1b2c3d4-0002-4a5b-8c7d-1e2f3a4b5c6d");
    static final UUID TEL  = UUID.fromString("a1b2c3d4-0003-4a5b-8c7d-1e2f3a4b5c6d");
    static final UUID CFG  = UUID.fromString("a1b2c3d4-0004-4a5b-8c7d-1e2f3a4b5c6d");
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    static final int SEND_MS    = 20;
    static final int REQ_PERMS  = 1001;
    static final int REQ_ENABLE = 1002;
    static final int MAX_CONNECT_TRIES = 3;
    static final int MAX_CFG_TRIES     = 4;

    static final byte[] DEFAULT_CFG = { (byte) 220, (byte) 140, (byte) 8, (byte) 14, (byte) 35, (byte) 0 };
    static final String PAIRING_PIN = "123654";

    /* ---------------- UI ---------------- */
    private TextView statusText, telemetryText;
    private View scanPanel, drivePanel;
    private Button scanBtn, stopBtn, disconnectBtn;
    private ListView deviceList;
    private JoystickView joystick;
    private ArrayAdapter<String> listAdapter;

    /* ---------------- BLE ---------------- */
    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic cmdChar, telChar, cfgChar;
    private boolean scanning = false;
    private boolean ready = false;

    /* ---------------- list state ----------------
     * NOTE: `items` must NOT be the list handed to ArrayAdapter. ArrayAdapter
     * uses the list you pass as its own backing store, so clear()+addAll() on
     * the same object empties it. The adapter gets its own list. */
    private final Map<String, ScanResult> found = new LinkedHashMap<>();
    private final List<String> items = new ArrayList<>();
    private final List<BluetoothDevice> listedDevices = new ArrayList<>();

    private BluetoothDevice pendingBondDevice;
    private BluetoothDevice targetDevice;
    private boolean bondReceiverRegistered;
    private int connectTries;
    private int cfgTries;

    /* ---------------- drive state ---------------- */
    private volatile float axisX = 0f, axisY = 0f;
    private volatile boolean eStop = false;
    private int seq = 0;
    private final Handler handler = new Handler(Looper.getMainLooper());

    /* =====================================================
     * bond receiver
     * ===================================================== */
    private final BroadcastReceiver bondReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            BluetoothDevice device;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE,
                                                   BluetoothDevice.class);
            } else {
                device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            }
            if (device == null) return;

            int state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR);
            int previous = intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE,
                                              BluetoothDevice.ERROR);

            if (state == BluetoothDevice.BOND_BONDED) {
                pendingBondDevice = null;
                refreshList();
                statusText.setText("Paired \u2014 connecting\u2026");
                handler.postDelayed(() -> connectGatt(device), 600);
            } else if (state == BluetoothDevice.BOND_NONE
                    && previous == BluetoothDevice.BOND_BONDING) {
                pendingBondDevice = null;
                statusText.setText("Pairing cancelled or failed");
            }
        }
    };

    /* =====================================================
     * send loop (50 Hz)
     * ===================================================== */
    private final Runnable sendLoop = new Runnable() {
        @Override
        public void run() {
            if (gatt == null || cmdChar == null || !ready) {
                handler.postDelayed(this, SEND_MS);
                return;
            }
            int throttle = clamp(Math.round(axisY * 100f), -100, 100);
            int steer    = clamp(Math.round(axisX * 100f), -100, 100);
            byte flags   = eStop ? (byte) 0x02 : (byte) 0x00;
            if (eStop) { throttle = 0; steer = 0; }

            byte[] frame = { (byte) throttle, (byte) steer, flags, (byte) (seq++ & 0xFF) };
            cmdChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            cmdChar.setValue(frame);
            gatt.writeCharacteristic(cmdChar);

            handler.postDelayed(this, SEND_MS);
        }
    };

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /* =====================================================
     * lifecycle
     * ===================================================== */
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(R.layout.activity_main);

        statusText    = findViewById(R.id.status);
        telemetryText = findViewById(R.id.telemetry);
        scanPanel     = findViewById(R.id.scanPanel);
        drivePanel    = findViewById(R.id.drivePanel);
        scanBtn       = findViewById(R.id.scanBtn);
        deviceList    = findViewById(R.id.deviceList);
        joystick      = findViewById(R.id.joystick);
        stopBtn       = findViewById(R.id.stop);
        disconnectBtn = findViewById(R.id.disconnect);

        // own backing list -> clear()/addAll() on `items` behaves
        listAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1,
                                         new ArrayList<>());
        deviceList.setAdapter(listAdapter);

        joystick.setListener(new JoystickView.Listener() {
            @Override public void onMove(float x, float y) {
                axisX = x;
                axisY = y;
            }
        });

        scanBtn.setOnClickListener(v -> {
            if (scanning) stopScan();
            else { found.clear(); refreshList(); startScan(); }
        });

        deviceList.setOnItemClickListener((parent, view, position, id) -> connect(position));

        disconnectBtn.setOnClickListener(v -> {
            if (gatt != null) gatt.disconnect();
        });

        stopBtn.setOnClickListener(v -> {
            eStop = !eStop;
            stopBtn.setText(eStop ? "RESUME" : "STOP");
            stopBtn.setBackgroundTintList(
                    android.content.res.ColorStateList.valueOf(eStop ? 0xFF2B4058 : 0xFFC11F3A));
        });

        BluetoothManager bm = (BluetoothManager) getSystemService(BLUETOOTH_SERVICE);
        adapter = (bm != null) ? bm.getAdapter() : null;

        IntentFilter bondFilter = new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(bondReceiver, bondFilter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(bondReceiver, bondFilter);
        }
        bondReceiverRegistered = true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (adapter == null) { statusText.setText("No Bluetooth on this device"); return; }
        if (!adapter.isEnabled()) {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_ENABLE);
            return;
        }
        if (hasPerms()) {
            refreshList();            // show paired devices immediately
            if (!ready) startScan();
        } else {
            requestPermissions(requiredPerms(), REQ_PERMS);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        axisX = 0f;
        axisY = 0f;
        eStop = true;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        handler.removeCallbacks(sendLoop);
        stopScan();
        closeGatt();
        if (bondReceiverRegistered) {
            unregisterReceiver(bondReceiver);
            bondReceiverRegistered = false;
        }
    }

    /* =====================================================
     * permissions
     * ===================================================== */
    private String[] requiredPerms() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return new String[] {
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_CONNECT
            };
        }
        return new String[] { Manifest.permission.ACCESS_FINE_LOCATION };
    }

    private boolean hasPerms() {
        for (String p : requiredPerms()) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) return false;
        }
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        super.onRequestPermissionsResult(code, perms, res);
        if (code == REQ_PERMS) {
            if (hasPerms()) { refreshList(); startScan(); }
            else statusText.setText("Bluetooth permission denied \u2014 enable it in Settings");
        }
    }

    @Override
    protected void onActivityResult(int code, int res, Intent data) {
        super.onActivityResult(code, res, data);
        if (code == REQ_ENABLE && adapter != null && adapter.isEnabled() && hasPerms()) {
            refreshList();
            startScan();
        }
    }

    /* =====================================================
     * scanning
     * ===================================================== */
    private void startScan() {
        if (scanner == null) scanner = adapter.getBluetoothLeScanner();
        if (scanner == null || scanning) return;

        scanBtn.setText("STOP SCAN");
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanner.startScan(null, settings, scanCallback);
        scanning = true;
    }

    private void stopScan() {
        if (scanner != null && scanning) {
            scanner.stopScan(scanCallback);
            scanning = false;
        }
        scanBtn.setText("SCAN / REFRESH");
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult result) {
            String addr;
            try { addr = result.getDevice().getAddress(); }
            catch (SecurityException e) { return; }
            found.put(addr, result);
            runOnUiThread(() -> {
                refreshList();
                if (scanning) {
                    statusText.setText("Scanning\u2026 " + listedDevices.size() + " device(s) listed");
                }
            });
        }

        @Override
        public void onScanFailed(int error) {
            statusText.setText("Scan failed: " + error);
            scanning = false;
            scanBtn.setText("SCAN / REFRESH");
        }
    };

    /* =====================================================
     * the list
     * ===================================================== */
    private boolean alreadyListed(String addr) {
        for (BluetoothDevice d : listedDevices) {
            try { if (addr.equals(d.getAddress())) return true; }
            catch (SecurityException ignored) { }
        }
        return false;
    }

    private void refreshList() {
        items.clear();
        listedDevices.clear();

        // 1) already-paired devices first - these are what we can talk to
        try {
            for (BluetoothDevice d : adapter.getBondedDevices()) {
                String name = d.getName();
                if (name == null || name.isEmpty()) name = "(unnamed)";
                items.add("PAIRED  \u00b7  " + name + "  \u00b7  " + d.getAddress());
                listedDevices.add(d);
            }
        } catch (SecurityException ignored) { }

        // 2) live scan results not already shown
        List<ScanResult> results = new ArrayList<>(found.values());
        Collections.sort(results, (a, b) -> b.getRssi() - a.getRssi());
        for (ScanResult r : results) {
            BluetoothDevice d = r.getDevice();
            String addr, name;
            try {
                addr = d.getAddress();
                name = d.getName();
            } catch (SecurityException e) { continue; }
            if (name == null || name.isEmpty()) name = "(unnamed)";
            if (alreadyListed(addr)) continue;

            boolean rover = r.getScanRecord() != null
                    && r.getScanRecord().getServiceUuids() != null
                    && r.getScanRecord().getServiceUuids().contains(new ParcelUuid(SVC));
            items.add((rover ? "ROVER  " : "NEARBY  ") + name + "  \u00b7  " + addr
                      + "  \u00b7  " + r.getRssi() + " dBm");
            listedDevices.add(d);
        }

        if (items.isEmpty()) items.add("No devices yet \u2014 tap SCAN / REFRESH");

        listAdapter.clear();
        listAdapter.addAll(items);
        listAdapter.notifyDataSetChanged();
    }

    /* =====================================================
     * connect
     * ===================================================== */
    private void connect(int position) {
        if (position < 0 || position >= listedDevices.size()) return;
        BluetoothDevice device = listedDevices.get(position);

        stopScan();
        closeGatt();
        ready = false;
        axisX = 0f;
        axisY = 0f;
        eStop = true;
        targetDevice = device;
        connectTries = 0;
        cfgTries = 0;

        int bond;
        try { bond = device.getBondState(); }
        catch (SecurityException e) { statusText.setText("Bluetooth permission required"); return; }

        if (bond == BluetoothDevice.BOND_BONDED) { connectGatt(device); return; }

        pendingBondDevice = device;
        statusText.setText("Pairing \u2014 enter PIN " + PAIRING_PIN + " in the system dialog");
        try {
            if (bond != BluetoothDevice.BOND_BONDING && !device.createBond()) {
                pendingBondDevice = null;
                statusText.setText("Could not start pairing");
            }
        } catch (SecurityException e) {
            pendingBondDevice = null;
            statusText.setText("Bluetooth permission required");
        }
    }

    private void closeGatt() {
        if (gatt != null) {
            gatt.disconnect();
            gatt.close();
            gatt = null;
        }
        cmdChar = null;
        telChar = null;
        cfgChar = null;
    }

    private void connectGatt(BluetoothDevice device) {
        closeGatt();
        statusText.setText("Connecting to " + safeAddr(device) + "\u2026");
        try {
            gatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException e) {
            statusText.setText("Bluetooth permission required");
            return;
        }
        if (gatt == null) statusText.setText("Could not open a GATT connection");
    }

    private static String safeAddr(BluetoothDevice d) {
        try { return d.getAddress(); } catch (SecurityException e) { return "device"; }
    }

    /* =====================================================
     * GATT
     * ===================================================== */
    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {

        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                connectTries = 0;
                statusText.setText("Connected \u2014 discovering services\u2026");
                g.discoverServices();

            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                ready = false;
                handler.removeCallbacks(sendLoop);

                if (targetDevice != null && connectTries < MAX_CONNECT_TRIES
                        && status != BluetoothGatt.GATT_SUCCESS) {
                    connectTries++;
                    statusText.setText("Connect failed (status " + status + ") \u2014 retry "
                                       + connectTries + "/" + MAX_CONNECT_TRIES);
                    final BluetoothDevice d = targetDevice;
                    closeGatt();
                    handler.postDelayed(() -> connectGatt(d), 800);
                    return;
                }

                closeGatt();
                runOnUiThread(() -> {
                    drivePanel.setVisibility(View.GONE);
                    scanPanel.setVisibility(View.VISIBLE);
                    statusText.setText(status == BluetoothGatt.GATT_SUCCESS
                            ? "Disconnected \u2014 tap SCAN"
                            : "Connect failed (status " + status + ")");
                });
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                statusText.setText("Service discovery failed (" + status + ")");
                return;
            }
            BluetoothGattService svc = g.getService(SVC);
            if (svc == null) {
                statusText.setText("Rover service not found \u2014 toggle Bluetooth off/on and retry");
                return;
            }
            cmdChar = svc.getCharacteristic(CMD);
            telChar = svc.getCharacteristic(TEL);
            cfgChar = svc.getCharacteristic(CFG);
            if (cmdChar == null) { statusText.setText("Command characteristic missing"); return; }

            statusText.setText("Negotiating\u2026");
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
            if (!g.requestMtu(247)) subscribe(g);
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            subscribe(g);
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            writeCfg(g);
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            if (CFG.equals(c.getUuid())) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    onReady();
                } else if (cfgTries < MAX_CFG_TRIES) {
                    // The config characteristic is encrypted. The very first
                    // protected access is often what makes Android bring the
                    // link up encrypted, and that first attempt fails - so
                    // retry a few times before giving up.
                    cfgTries++;
                    statusText.setText("Securing link\u2026 (" + cfgTries + "/" + MAX_CFG_TRIES + ")");
                    handler.postDelayed(() -> { if (gatt != null) writeCfg(gatt); }, 700);
                } else {
                    statusText.setText("Encrypted write failed (" + status
                                       + ") \u2014 forget the device and re-pair");
                }
                return;
            }
            onReady();
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (!TEL.equals(c.getUuid())) return;
            byte[] v = c.getValue();
            if (v == null || v.length < 6) return;
            int left  = (short) ((v[0] & 0xFF) | (v[1] << 8));
            int right = (short) ((v[2] & 0xFF) | (v[3] << 8));
            int state = v[4] & 0xFF;
            final String line = String.format("L: %4d   R: %4d   %s", left, right, stateName(state));
            runOnUiThread(() -> telemetryText.setText(line));
        }
    };

    private void subscribe(BluetoothGatt g) {
        if (telChar == null) { writeCfg(g); return; }
        g.setCharacteristicNotification(telChar, true);
        BluetoothGattDescriptor d = telChar.getDescriptor(CCCD);
        if (d != null) {
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            if (g.writeDescriptor(d)) return;
        }
        writeCfg(g);
    }

    private void writeCfg(BluetoothGatt g) {
        if (cfgChar == null) { onReady(); return; }
        cfgChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        cfgChar.setValue(DEFAULT_CFG);
        if (!g.writeCharacteristic(cfgChar)) {
            // queue busy - try again shortly
            handler.postDelayed(() -> { if (gatt != null) writeCfg(gatt); }, 300);
        }
    }

    private void onReady() {
        ready = true;
        runOnUiThread(() -> {
            statusText.setText("Connected \u2014 drive!");
            scanPanel.setVisibility(View.GONE);
            drivePanel.setVisibility(View.VISIBLE);
        });
        handler.removeCallbacks(sendLoop);
        handler.post(sendLoop);
    }

    private static String stateName(int s) {
        switch (s) {
            case 0: return "IDLE";
            case 1: return "DRIVING";
            case 2: return "BRAKING";
            case 3: return "FAILSAFE";
            default: return "?";
        }
    }
}
