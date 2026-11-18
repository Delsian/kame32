package com.kame32.controller;

import android.Manifest;
import android.app.*;
import android.bluetooth.*;
import android.bluetooth.le.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.os.*;
import android.graphics.Color;
import android.net.Uri;
import android.view.*;
import android.widget.*;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Foreground-only BLE controller; all mutable connection state belongs to the UI thread. */
public class MainActivity extends Activity {
    private static final UUID SERVICE = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID POSE = UUID.fromString("6e400004-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID OTA = UUID.fromString("6e400005-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private final Handler handler = new Handler(Looper.getMainLooper());
    private static final int[] SERVO_CHANNELS = {0, 1, 2, 3, 4, 12, 13, 14};
    private static final int MIN_PULSE = 1000;
    private static final int PULSE_SPAN = 1000;
    private static final int POSITION_STEPS = 254;
    private final byte[] positions = new byte[8];
    private final ArrayList<GestureStep> gesture = new ArrayList<>();
    private byte[] pendingPose, heldPose;
    private boolean stopPending;
    private int gestureIndex = -1;
    private int pendingHoldMs, writtenHoldMs;

    private static final class GestureStep {
        final byte[] pose;
        final int holdMs;

        GestureStep(byte[] pose, int holdMs) {
            this.pose = pose;
            this.holdMs = holdMs;
        }
    }
    private final SeekBar[] sliders = new SeekBar[8];
    private final ArrayList<View> controls = new ArrayList<>();
    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic poseCharacteristic;
    private boolean ready, busy, scanning, holding;
    private TextView status, response;
    private LinearLayout devices;
    private Button connect, pwmToggle, playGesture;
    private TextView gestureSummary;
    private EditText holdTime;
    private final Runnable nextGestureStep = this::advanceGesture;
    private final Set<String> found = new HashSet<>();
    private static final int PICK_FIRMWARE = 2;
    private static final int MAX_FIRMWARE_SIZE = 0x140000;
    private static final int OTA_MTU = 247;
    private enum OtaPhase { IDLE, WAITING, MTU, BEGIN, DATA, END }
    private OtaPhase otaPhase = OtaPhase.IDLE;
    private BluetoothGattCharacteristic otaCharacteristic;
    private Button chooseFirmware, installFirmware;
    private TextView firmwareSelection;
    private byte[] firmware;
    private boolean loadingFirmware, otaAcknowledged;
    private int otaOffset, otaChunkSize;
    private final Runnable timeout = () -> disconnect(otaPhase == OtaPhase.IDLE
            ? "BLE operation timed out; outputs released" : "Firmware update timed out");
    private final Runnable scanTimeout = () -> { stopScan(); status.setText("Scan finished. Select a device or scan again."); };
    private final Runnable heartbeat = new Runnable() {
        public void run() {
            if (ready && otaPhase == OtaPhase.IDLE) {
                if (heldPose != null && !busy && pendingPose == null) sendPose(heldPose, 0);
                handler.postDelayed(this, 500);
            }
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Arrays.fill(positions, (byte) 128);
        BluetoothManager manager = getSystemService(BluetoothManager.class);
        adapter = manager == null ? null : manager.getAdapter();
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL); page.setPadding(24, 16, 24, 16);
        page.setBackgroundColor(Color.rgb(243,247,246));
        page.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(24, insets.getSystemWindowInsetTop()+16, 24, insets.getSystemWindowInsetBottom()+16);
            return insets;
        });
        page.addView(label("Kame32 • Servo controller", 25));
        status = label("Disconnected • outputs released", 16); page.addView(status);
        connect = button("Scan for Kame32", () -> {
            if (otaPhase != OtaPhase.IDLE) disconnect("Firmware update cancelled");
            else if (gatt != null || scanning) disconnect("Disconnected");
            else scan();
        });
        page.addView(connect);
        Button stop = button("STOP — release all servos", () -> {
            releaseOutputs();
        });
        stop.setTextColor(Color.rgb(175,30,40)); controls.add(stop); page.addView(stop);
        pwmToggle = button("PWM OFF • turn ON", () -> {
            if (holding) releaseOutputs();
            else applyPose();
        });
        controls.add(pwmToggle); page.addView(pwmToggle);
        ScrollView scroll = new ScrollView(this); page.addView(scroll, new LinearLayout.LayoutParams(-1,0,1));
        LinearLayout content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL); scroll.addView(content);
        devices = new LinearLayout(this); devices.setOrientation(LinearLayout.VERTICAL); content.addView(devices);
        chooseFirmware = button("Choose firmware file", this::pickFirmware);
        installFirmware = button("Install firmware over BLE", this::confirmFirmwareUpdate);
        firmwareSelection = label("No firmware selected", 14);
        content.addView(chooseFirmware);
        content.addView(firmwareSelection);
        content.addView(installFirmware);
        content.addView(label("Pulse width • 1000–2000 µs\nReleasing a slider sends all eight displayed positions.",14));
        Button apply = button("Apply all 8 servos", this::applyPose);
        controls.add(apply); content.addView(apply);
        Button center = button("Set all to 1500 µs", () -> {
            for (SeekBar slider : sliders) slider.setProgress(POSITION_STEPS / 2);
            applyPose();
        }); controls.add(center); content.addView(center);
        for(int i=0;i<8;i++) {
            final int channel=i;
            TextView value=label("Channel "+SERVO_CHANNELS[i]+"   •   1500 µs",18); content.addView(value);
            SeekBar slider=new SeekBar(this); slider.setMax(POSITION_STEPS); slider.setProgress(POSITION_STEPS / 2);
            slider.setContentDescription("Servo channel "+SERVO_CHANNELS[i]+", pulse width in microseconds");
            sliders[i]=slider; controls.add(slider); content.addView(slider);
            slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                public void onProgressChanged(SeekBar s,int p,boolean user) {
                    positions[channel] = (byte) (p + 1);
                    int pulse = MIN_PULSE + (p * PULSE_SPAN + POSITION_STEPS / 2) / POSITION_STEPS;
                    value.setText("Channel " + SERVO_CHANNELS[channel] + "   •   " + pulse + " µs");
                }
                public void onStartTrackingTouch(SeekBar s) {}
                public void onStopTrackingTouch(SeekBar s) { applyPose(); }
            });
        }
        addGestureControls(content);
        response=label("No command sent",14); content.addView(response);
        setContentView(page); enable(false);
    }
    private void addGestureControls(LinearLayout content) {
        content.addView(label("Gesture", 20));
        content.addView(label("Step hold time (ms)", 14));
        holdTime = new EditText(this);
        holdTime.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        holdTime.setText("500");
        controls.add(holdTime);
        content.addView(holdTime);
        gestureSummary = label("No steps recorded", 14);
        content.addView(gestureSummary);
        Button add = button("Add displayed pose", this::recordGestureStep);
        Button remove = button("Remove last step", () -> {
            if (gestureIndex >= 0 || gesture.isEmpty()) return;
            gesture.remove(gesture.size() - 1);
            updateGestureControls();
        });
        playGesture = button("Play gesture", () -> {
            if (gestureIndex >= 0) releaseOutputs();
            else if (!gesture.isEmpty() && ready) {
                gestureIndex = 0;
                updateGestureControls();
                playGestureStep();
            }
        });
        controls.add(add);
        controls.add(remove);
        controls.add(playGesture);
        content.addView(add);
        content.addView(remove);
        content.addView(playGesture);
    }

    private void recordGestureStep() {
        if (gestureIndex >= 0) return;
        int duration;
        try {
            duration = Integer.parseInt(holdTime.getText().toString());
        } catch (NumberFormatException e) {
            duration = 0;
        }
        if (duration < 20 || duration > 60000) {
            holdTime.setError("Enter 20–60000 ms");
            return;
        }
        gesture.add(new GestureStep(positions.clone(), duration));
        updateGestureControls();
    }

    private void updateGestureControls() {
        playGesture.setText(gestureIndex >= 0 ? "Stop gesture and release servos" : "Play gesture");
        playGesture.setEnabled(ready && otaPhase == OtaPhase.IDLE && !gesture.isEmpty());
        if (gestureIndex >= 0) {
            gestureSummary.setText("Playing step " + (gestureIndex + 1) + " of " + gesture.size());
        } else {
            gestureSummary.setText(gesture.isEmpty() ? "No steps recorded" : "Recorded steps: " + gesture.size());
        }
    }

    private void playGestureStep() {
        GestureStep step = gesture.get(gestureIndex);
        sendPose(step.pose, step.holdMs);
    }

    private void advanceGesture() {
        if (gestureIndex < 0) return;
        if (++gestureIndex == gesture.size()) {
            cancelGesture();
            status.setText("Gesture finished • holding final pose");
        } else {
            updateGestureControls();
            playGestureStep();
        }
    }

    private void cancelGesture() {
        handler.removeCallbacks(nextGestureStep);
        gestureIndex = -1;
        writtenHoldMs = 0;
        updateGestureControls();
    }

    private TextView label(String text,int size) { TextView v=new TextView(this);v.setText(text);v.setTextSize(size);v.setPadding(0,12,0,8);return v; }
    private Button button(String text,Runnable action) { Button b=new Button(this); b.setText(text); b.setOnClickListener(v -> action.run()); return b; }
    private void enable(boolean enabled) {
        for (View view : controls) view.setEnabled(enabled);
        updateGestureControls();
        updateFirmwareControls();
    }

    private void updateFirmwareControls() {
        boolean idle = otaPhase == OtaPhase.IDLE;
        chooseFirmware.setEnabled(idle && !loadingFirmware);
        installFirmware.setEnabled(idle && ready && otaCharacteristic != null
                && firmware != null && !loadingFirmware);
    }

    private void pickFirmware() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_FIRMWARE);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FIRMWARE || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        firmware = null;
        loadingFirmware = true;
        firmwareSelection.setText("Reading firmware…");
        updateFirmwareControls();
        new Thread(() -> {
            byte[] image;
            try (InputStream input = getContentResolver().openInputStream(uri);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) throw new IOException("Cannot open firmware");
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (output.size() + count > MAX_FIRMWARE_SIZE) throw new IOException("Firmware too large");
                    output.write(buffer, 0, count);
                }
                image = output.toByteArray();
                if (image.length < 24 || (image[0] & 0xff) != 0xe9) throw new IOException("Invalid ESP firmware image");
            } catch (IOException | SecurityException e) {
                handler.post(() -> setFirmwareSelection(null));
                return;
            }
            byte[] selected = image;
            handler.post(() -> setFirmwareSelection(selected));
        }, "Firmware file reader").start();
    }

    private void setFirmwareSelection(byte[] image) {
        if (isDestroyed()) return;
        firmware = image;
        loadingFirmware = false;
        firmwareSelection.setText(image == null ? "Choose a valid Kame32 firmware.bin (up to 1280 KiB)"
                : "Firmware selected • " + image.length + " bytes");
        updateFirmwareControls();
    }

    private void confirmFirmwareUpdate() {
        new AlertDialog.Builder(this)
                .setTitle("Install Kame32 firmware?")
                .setMessage("With Kame32 powered on, hold its BOOT button and tap Install. Release BOOT when transfer starts. The board will release the servos and restart after verifying the image.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Install", (dialog, which) -> startFirmwareUpdate())
                .show();
    }

    private void startFirmwareUpdate() {
        if (!ready || otaCharacteristic == null || firmware == null || otaPhase != OtaPhase.IDLE) return;
        otaPhase = OtaPhase.WAITING;
        handler.removeCallbacks(heartbeat);
        cancelGesture();
        pendingPose = heldPose = null;
        stopPending = holding = false;
        writtenHoldMs = 0;
        updatePwmButton();
        enable(false);
        connect.setText("Cancel update");
        status.setText("Preparing update • hold BOOT");
        pump();
    }

    @SuppressWarnings("MissingPermission")
    private void requestOtaMtu() {
        otaPhase = OtaPhase.MTU;
        busy = true;
        watchdog();
        if (!gatt.requestMtu(OTA_MTU)) beginOta(23);
    }

    private void beginOta(int mtu) {
        otaChunkSize = Math.max(1, Math.min(mtu, OTA_MTU) - 4);
        otaOffset = 0;
        writeOta(OtaPhase.BEGIN, ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
                .put((byte) 1).putInt(firmware.length).array());
    }

    private void writeOta(OtaPhase phase, byte[] packet) {
        otaPhase = phase;
        otaAcknowledged = false;
        writeBle(otaCharacteristic, packet);
    }

    private void advanceOta() {
        if (busy || !otaAcknowledged) return;
        if (otaPhase == OtaPhase.END) {
            disconnect("Firmware installed • Kame32 is restarting");
        } else if (otaOffset == firmware.length) {
            status.setText("Verifying firmware…");
            writeOta(OtaPhase.END, new byte[]{3});
        } else {
            status.setText("Sending firmware • " + (otaOffset * 100 / firmware.length) + "%");
            int count = Math.min(otaChunkSize, firmware.length - otaOffset);
            byte[] packet = new byte[count + 1];
            packet[0] = 2;
            System.arraycopy(firmware, otaOffset, packet, 1, count);
            otaOffset += count;
            writeOta(OtaPhase.DATA, packet);
        }
    }

    private void otaNotification(String message) {
        switch (message) {
            case "OTA READY": otaAcknowledged = otaPhase == OtaPhase.BEGIN; break;
            case "OTA OK": otaAcknowledged = otaPhase == OtaPhase.DATA; break;
            case "OTA DONE": otaAcknowledged = otaPhase == OtaPhase.END; break;
            case "OTA HOLD BOOT":
                disconnect("Update rejected • hold BOOT while starting the update");
                return;
            default:
                disconnect("Firmware update failed (" + message + ")");
                return;
        }
        advanceOta();
    }
    private boolean permissions() {
        String[] required=Build.VERSION.SDK_INT>=31 ? new String[]{Manifest.permission.BLUETOOTH_SCAN,Manifest.permission.BLUETOOTH_CONNECT} : new String[]{Manifest.permission.ACCESS_FINE_LOCATION};
        for(String p:required) if(checkSelfPermission(p)!=PackageManager.PERMISSION_GRANTED) { requestPermissions(required,1); return false; }
        return true;
    }
    @Override public void onRequestPermissionsResult(int r,String[] p,int[] results) {
        super.onRequestPermissionsResult(r,p,results);
        if(r==1 && results.length>0) {
            for(int result:results) if(result!=PackageManager.PERMISSION_GRANTED) { status.setText("Bluetooth permission required. Enable it in app settings, then scan."); return; }
            scan();
        }
    }
    private void scan() {
        if(!permissions())return;
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { disconnect("Bluetooth permission revoked"); return; }
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) return;
        if(adapter==null) { status.setText("Bluetooth LE unavailable");return; }
        if(!adapter.isEnabled()) { startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)); return; }
        scanner=adapter.getBluetoothLeScanner(); if(scanner==null)return;
        devices.removeAllViews();found.clear();scanning=true;
        connect.setText("Cancel scan");status.setText("Searching for Kame32…");
        try {
            scanner.startScan(Collections.singletonList(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SERVICE)).build()),new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),scanCallback);
            handler.postDelayed(scanTimeout,10000);
        } catch(RuntimeException e) { disconnect("Cannot scan: "+e.getMessage()); }
    }
    private final ScanCallback scanCallback=new ScanCallback() {
        @Override public void onScanResult(int type,ScanResult result) { handler.post(() -> {
            if(!scanning)return;
            BluetoothDevice device=result.getDevice();
            if(found.add(device.getAddress())) devices.addView(button("Connect • "+device.getAddress(),() -> connectDevice(device)));
        }); }
        @Override public void onScanFailed(int code) { handler.post(() -> disconnect("Scan failed: "+code)); }
    };
    private void stopScan() {
        handler.removeCallbacks(scanTimeout);
        if(scanning && scanner!=null) { try { scanner.stopScan(scanCallback); } catch(SecurityException ignored) {} }
        scanning=false;connect.setText(gatt==null ? "Scan for Kame32" : "Disconnect");
    }
    private void connectDevice(BluetoothDevice device) {
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { disconnect("Bluetooth permission revoked"); return; }
        stopScan();devices.removeAllViews();status.setText("Connecting…");
        gatt=device.connectGatt(this,false,callback,BluetoothDevice.TRANSPORT_LE);
        connect.setText("Disconnect");watchdog();
    }
    private void watchdog() {
        handler.removeCallbacks(timeout);
        handler.postDelayed(timeout, otaPhase == OtaPhase.IDLE ? 5000 : 15000);
    }
    private void updatePwmButton() {
        pwmToggle.setText(holding ? "PWM ON • turn OFF" : "PWM OFF • turn ON");
    }
    private void releaseOutputs() {
        cancelGesture();
        pendingPose = null;
        stopPending = true;
        status.setText("Stop requested");
        pump();
    }

    private void applyPose() {
        cancelGesture();
        status.setText("Holding displayed pose");
        sendPose(positions.clone(), 0);
    }

    private void sendPose(byte[] pose, int holdMs) {
        if (!ready) return;
        pendingPose = pose;
        pendingHoldMs = holdMs;
        pump();
    }

    private void pump() {
        if (busy || !ready || (otaPhase != OtaPhase.IDLE && otaPhase != OtaPhase.WAITING)) return;
        if (otaPhase == OtaPhase.IDLE && !stopPending && pendingPose == null) return;
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            disconnect("Bluetooth permission revoked");
            return;
        }
        if (otaPhase == OtaPhase.WAITING) {
            requestOtaMtu();
            return;
        }

        byte[] bytes;
        if (stopPending) {
            stopPending = false;
            bytes = new byte[positions.length];
            heldPose = null;
            writtenHoldMs = 0;
        } else {
            bytes = heldPose = pendingPose;
            pendingPose = null;
            writtenHoldMs = pendingHoldMs;
        }
        holding = heldPose != null;
        updatePwmButton();
        writeBle(poseCharacteristic, bytes);
    }

    @SuppressWarnings({"deprecation", "MissingPermission"})
    private void writeBle(BluetoothGattCharacteristic characteristic, byte[] bytes) {
        busy = true;
        watchdog();
        boolean accepted;
        if (Build.VERSION.SDK_INT >= 33) {
            accepted = gatt.writeCharacteristic(characteristic, bytes,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS;
        } else {
            characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            characteristic.setValue(bytes);
            accepted = gatt.writeCharacteristic(characteristic);
        }
        if (!accepted) disconnect("BLE write rejected; outputs released");
    }
    private void notification(BluetoothGatt source,byte[] value) { String text=new String(value,StandardCharsets.US_ASCII);handler.post(() -> {
        if(source!=gatt)return;
        if (otaPhase != OtaPhase.IDLE && (text.startsWith("OTA ") || text.startsWith("STOP"))) {
            otaNotification(text);
            return;
        }
        response.setText(text);
        if(text.startsWith("ERR") || text.startsWith("STOP")) { heldPose = pendingPose = null; holding = false; cancelGesture(); updatePwmButton(); status.setText(text); }
        else if(text.equals("OK STOP") && !holding && pendingPose == null) status.setText("Outputs released");
    }); }
    private final BluetoothGattCallback callback=new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g,int code,int state) { handler.post(() -> {
            if(g!=gatt)return;
            if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { disconnect("Bluetooth permission revoked"); return; }
        
            if(code!=BluetoothGatt.GATT_SUCCESS || state==BluetoothProfile.STATE_DISCONNECTED) {
                disconnect(otaPhase == OtaPhase.IDLE ? "Disconnected • outputs released"
                        : "Disconnected during firmware update");
                return;
            }
            if(state==BluetoothProfile.STATE_CONNECTED) { status.setText("Discovering servo service…");if(!g.discoverServices())disconnect("Service discovery failed");else watchdog(); }
        }); }
        @Override public void onServicesDiscovered(BluetoothGatt g,int code) { handler.post(() -> {
            if(g!=gatt)return;
            if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { disconnect("Bluetooth permission revoked"); return; }
        
            BluetoothGattService service=g.getService(SERVICE);
            BluetoothGattCharacteristic tx=service==null?null:service.getCharacteristic(TX);
            poseCharacteristic = service == null ? null : service.getCharacteristic(POSE);
            otaCharacteristic = service == null ? null : service.getCharacteristic(OTA);
            BluetoothGattDescriptor descriptor=tx==null?null:tx.getDescriptor(CCC);
            if(code!=0 || poseCharacteristic==null || descriptor==null || !g.setCharacteristicNotification(tx,true)) { disconnect("Kame32 pose service unavailable • update firmware");return; }
            boolean accepted;
            if(Build.VERSION.SDK_INT>=33) accepted=g.writeDescriptor(descriptor,BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)==BluetoothStatusCodes.SUCCESS;
            else { descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);accepted=g.writeDescriptor(descriptor); }
            if(!accepted)disconnect("Cannot subscribe to responses");else watchdog();
        }); }
        @Override public void onDescriptorWrite(BluetoothGatt g,BluetoothGattDescriptor d,int code) { handler.post(() -> {
            if(g!=gatt)return;
            if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { disconnect("Bluetooth permission revoked"); return; }
        
            if(code!=0) { disconnect("Notification subscription failed");return; }
            handler.removeCallbacks(timeout);ready=true;enable(true);status.setText("Connected • no pose applied");
            releaseOutputs();handler.postDelayed(heartbeat,500);
        }); }
        @Override public void onMtuChanged(BluetoothGatt g, int mtu, int code) { handler.post(() -> {
            if (g == gatt && otaPhase == OtaPhase.MTU) beginOta(code == BluetoothGatt.GATT_SUCCESS ? mtu : 23);
        }); }
        @Override public void onCharacteristicWrite(BluetoothGatt g,BluetoothGattCharacteristic c,int code) { handler.post(() -> {
            if(g!=gatt)return;
            if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { disconnect("Bluetooth permission revoked"); return; }
        
            busy = false;
            if (code != 0) { disconnect("Command failed; outputs released"); return; }
            if (OTA.equals(c.getUuid())) {
                advanceOta();
                return;
            }
            handler.removeCallbacks(timeout);
            if (writtenHoldMs > 0) handler.postDelayed(nextGestureStep, writtenHoldMs);
            writtenHoldMs = 0;
            pump();
        }); }
        @Override public void onCharacteristicChanged(BluetoothGatt g,BluetoothGattCharacteristic c,byte[] value) { notification(g,value); }
        @Override public void onCharacteristicChanged(BluetoothGatt g,BluetoothGattCharacteristic c) { notification(g,c.getValue()); }
    };
    private void disconnect(String message) {
        stopScan();handler.removeCallbacks(timeout);handler.removeCallbacks(heartbeat);
        otaPhase = OtaPhase.IDLE;
        otaCharacteristic = null;
        ready=false;busy=false;holding=false;stopPending=false;
        cancelGesture();updatePwmButton();heldPose=pendingPose=null;poseCharacteristic=null;enable(false);
        BluetoothGatt old=gatt;gatt=null;
        if(old!=null) { try { old.disconnect();old.close(); } catch(SecurityException ignored) {} }
        connect.setText("Scan for Kame32");status.setText(message);devices.removeAllViews();
    }
    @Override protected void onStop() { super.onStop();disconnect("Disconnected • outputs released"); }
}
