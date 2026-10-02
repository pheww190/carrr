# carrr — BLE-controlled ESP32 rover

A low-power rover driven over **Bluetooth Low Energy** from an Android app.
The ESP32 runs a Bluedroid GATT server plus a fixed-rate control loop; the
phone streams a throttle/steering frame and receives live telemetry.
No Wi-Fi, no HTTP — BLE only.

---

## Hardware

| | |
|---|---|
| MCU | ESP32-WROOM-32 (DOIT DevKit V1 class, Xtensa LX6) |
| Driver | L298N / TB6612 style: `EN` (PWM) + `IN1`/`IN2` per motor |
| USB-serial | CP2102 |

**Motor pins** (change in `firmware/main/motor.c` if your wiring differs):

| Signal | GPIO |
|---|---|
| ENA (left PWM) | 25 |
| IN1 / IN2 (left dir) | 26 / 27 |
| ENB (right PWM) | 14 |
| IN3 / IN4 (right dir) | 12 / 13 |

> GPIO12 is a strapping pin. It works once booted; if the board ever fails to
> boot with the driver attached, move IN3 to GPIO32 and ENB to GPIO33.

---

## Repository layout

```
carrr/
├── firmware/          ESP-IDF project (target: esp32)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── main.c          boot order
│       ├── motor.c/.h      LEDC PWM + direction GPIOs
│       ├── control.c/.h    50 Hz control loop
│       ├── ble_srv.c/.h    Bluedroid GATT server
│       └── rover_proto.h   wire protocol
├── android/           Android Studio / Gradle project (Java, minSdk 29)
│   └── app/src/main/java/com/example/blerover/
│       ├── MainActivity.java
│       └── JoystickView.java
└── scripts/           WSL setup scripts
    ├── setup-esp32-wsl.sh
    ├── setup-android-wsl.sh
    └── setup-shell-env.sh
```

---

## Firmware — build & flash

Requires **ESP-IDF v5.5.5** (the code targets the v5.x Bluedroid API).

```bash
source ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

On boot you should see:

```
I (xxx) BLE_SRV: BLE MAC B0:CB:D8:09:6A:AA
I (xxx) BLE_SRV: advertising as "RoverBLE"
```

---

## Android app — build

The project has **no external dependencies** (plain Android framework).

```bash
cd android
echo "sdk.dir=$ANDROID_HOME" > local.properties
gradle assembleDebug          # or: ./gradlew assembleDebug
```

APK lands in `app/build/outputs/apk/debug/app-debug.apk`.

Toolchain: JDK 17+, Android SDK platform 34, build-tools 34.0.0, Gradle 8.7+.
See `scripts/setup-android-wsl.sh`.

---

## BLE protocol

Service `a1b2c3d4-0001-4a5b-8c7d-1e2f3a4b5c6d`:

| Characteristic | UUID suffix | Direction | Size |
|---|---|---|---|
| CMD | `-0002-` | phone → rover (Write / Write-No-Response) | 4 bytes |
| TEL | `-0003-` | rover → phone (Notify) | 6 bytes |
| CFG | `-0004-` | phone → rover (Write) | 6 bytes |

**CMD** — `throttle int8` (−100…+100), `steering int8` (−100…+100),
`flags uint8` (bit0 brake, bit1 e-stop), `seq uint8`.

**TEL** — `left int16`, `right int16`, `state uint8`, `seq uint8`.
State: 0 IDLE, 1 DRIVING, 2 BRAKING, 3 FAILSAFE.

**CFG** — `max_speed, turn_cap, accel, decel, expo, flags`.

### Pairing

The GATT server requires **LE Secure Connections + bonding + MITM**, so the
phone must pair before it can drive anything. The passkey is **fixed**:

```
PIN: 123654
```

Android shows its own pairing dialog — enter that. After the first bond the
phone reconnects silently. To re-pair: *Settings → Bluetooth → Forget*.

---

## Control loop

The loop runs at **50 Hz** and owns the only path to the motors:

```
command -> failsafe -> expo curve -> mix(throttle, steering)
        -> limits -> invert/swap -> per-wheel slew limiter -> motor output
```

The **per-wheel slew limiter** is the important part: each wheel's output may
change by `accel` units per tick when speeding up, and `decel` units when
slowing **or reversing**. A direction change is therefore forced to travel
through zero smoothly instead of slamming to full power — which is what stops
the rover lurching when you switch direction at speed.

Safety: 400 ms command timeout, disconnect watchdog, latched e-stop.

---

## Calibration

If the rover drives sideways or the turns are inverted, set the invert/swap
flags in the CFG frame (`DEFAULT_CFG` in `MainActivity.java`):

| Flag | Value | Meaning |
|---|---|---|
| `ROVER_CFG_INVERT_L` | 0x01 | flip left motor |
| `ROVER_CFG_INVERT_R` | 0x02 | flip right motor |
| `ROVER_CFG_INVERT_STEER` | 0x04 | flip steering |
| `ROVER_CFG_SWAP_LR` | 0x08 | swap the two outputs |

Tuning: `accel`/`decel` (ramp rates), `expo` (softness near stick centre),
`turn_cap` (how hard the steering term pushes), `max_speed`.
