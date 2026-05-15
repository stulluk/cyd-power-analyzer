# CYD Power Analyzer

Firmware and host tooling for an **ESP32 Cheap Yellow Display (CYD / ESP32-2432S028)** power analyzer. The device reads an **INA226** over I2C, renders live **voltage, current, and power** on the LCD, broadcasts JSON telemetry over UDP, serves a small browser dashboard, and prints serial diagnostics for bring-up.

Repository: <https://github.com/stulluk/cyd-power-analyzer>

## Current Features

| Area | Behavior |
|------|----------|
| **Sensor** | Direct INA226 register access over `Wire` (no INA library dependency). |
| **Display** | LVGL/TFT_eSPI landscape UI with large metric rows (`V` / `I` / `P` on LCD; **Voltage** / Current / Power on the web) plus Wi-Fi/sensor footer. |
| **Web UI** | `http://<device-ip>/` shows the same live values; `/api/metrics` returns JSON. |
| **UDP telemetry** | Broadcast JSON on `CYD_UDP_TELEM_PORT` (`4210`) about every 200 ms when Wi-Fi is connected. |
| **Serial diagnostics** | `STATS`, `METRIC`, `INA226_RAW`, and Wi-Fi/IP lines at 115200 baud. |
| **Probe firmware** | Minimal serial-only INA226 probe in `firmware/ina226_serial_probe/`. |
| **Docker workflow** | Build and flash through Docker/PlatformIO scripts; no host PlatformIO install required. |

## Hardware Wiring

The current firmware expects an INA226 at I2C address `0x40` on the CYD connector pins:

| Signal | CYD GPIO | Notes |
|--------|----------|-------|
| `SDA` | `GPIO27` | CN1 GPIO pin. |
| `SCL` | `GPIO22` | CN1/P3 GPIO pin. |
| `GND` | CYD GND | Share reference with the INA226 module. |
| `3V3` | CYD 3.3 V | Power the INA226 logic side. |

`GPIO21` is the TFT backlight line on this CYD variant and is intentionally not used for I2C.

### INA226 Range Notes

The tested module uses an `R100` shunt, so the firmware constants are:

```cpp
static constexpr float kIna226ShuntOhms = 0.1f;
static constexpr float kIna226MaxExpectedAmps = 0.8f;
```

INA226 shunt ADC full scale is about **81.92 mV**, so with a **100 mΩ** shunt the reliable linear current range is roughly:

```text
0.08192 V / 0.100 Ω ≈ 0.82 A
```

Loads above that can clip the shunt channel. To measure several amps reliably, the hardware shunt must be smaller and the firmware constants must match the actual shunt.

## INA226 Implementation Details

The firmware reads INA226 registers directly:

- `0x00` configuration
- `0x01` shunt voltage
- `0x02` bus voltage
- `0x03` power
- `0x04` current
- `0x05` calibration
- `0xFE` manufacturer ID
- `0xFF` die ID

Calibration follows the TI formula:

```text
Current_LSB = max_expected_A / 32768
CAL = floor(0.00512 / (Current_LSB × R_shunt))
Power_LSB = 25 × Current_LSB
```

Bus voltage decoding masks off the low status bits before applying the INA226 1.25 mV step:

```text
Vbus = (raw_bus & 0xFFF8) × 1.25 mV
```

Do not decode bus voltage as `(raw_bus >> 3) × 1.25 mV`; that under-reads by about 8× on the readings we observed.

### Readout precision, LSB stepping, and why three decimals are enough

The INA226 does **not** return a decimal string. Each quantity is a **signed or unsigned integer register** multiplied by a fixed **LSB** (least-significant bit weight):

| Quantity | Typical LSB (this firmware’s calibration) |
|----------|---------------------------------------------|
| Bus voltage | **1.25 mV** per bus ADC code step |
| Current | **`Current_LSB` ≈ `max_expected_A / 32768`** (here about **24 µA** per current code step) |
| Power | **`Power_LSB = 25 × Current_LSB`** (here about **0.61 mW** per power code step) |

So when you show **six decimal places** (as in a diagnostic build), you often see exactly this pattern:

- **Current** flickering between values like **0.000024 A** and **0.000049 A** with **no load** is not “random decimals”: it is usually the **current register’s integer code** hopping by **one or two counts** near zero (noise, offset, and quantization), which maps to **one or two × `Current_LSB`**.
- **Power** jumping between **0.000000 W** and **0.000610 W** is the same idea at **coarser** resolution: the **power register** is often **0 or 1 LSB** when the true power is tiny and the device is idle.
- **Bus voltage** with a **fixed 12 V adapter** and **no load** often looks very stable in the **first three fractional digits** (millivolt scale is close to the **1.25 mV** ADC step). Digits beyond that are mostly **printing precision**, not extra physical resolution. When you **draw current**, the bus sags slightly and more digits can move because the **underlying code** changes.

When the **adapter is unplugged**, bus (and derived) readings go to **zero** as expected.

**Recommendation:** for a **human-facing** UI and logs, **three decimal places** are a good default: they match the order of the **bus voltage** step, make **idle current/power chatter** invisible instead of alarming, and avoid implying false precision. Use **more decimals** only when you deliberately want to **visualize register LSB behavior** (bring-up, noise, or averaging experiments).

## Web Dashboard

After Wi-Fi connects, the serial log and LCD footer show the assigned IP address. Open:

```text
http://<device-ip>/
```

The dashboard polls:

```text
http://<device-ip>/api/metrics
```

Example response (field names fixed; decimal places depend on firmware build — **three** is recommended for display):

```json
{
  "vbus_v": 12.25,
  "i_a": 0.012,
  "p_w": 0.15,
  "ms": 23630,
  "cpu": 28,
  "ram": 39,
  "sensor_ok": true,
  "sensor": "ina226",
  "error": "ok",
  "ip": "192.168.1.236"
}
```

The web page keeps the author link as Unicode (`Sertaç Tüllük` → `https://www.drejo.com/`). The LCD byline uses ASCII (`Sertac Tulluk`) because the small LVGL font currently does not include Turkish glyphs.

## UDP Telemetry

UDP broadcast uses the same metric fields as the web API, minus the `ip` field. Any host on the same subnet can listen on port `4210`:

```bash
python3 scripts/cyd_udp_listen.py
```

Broadcast is intentionally used so the receiving PC IP does not have to be configured on the device.

## Serial Verification

Read the device at 115200 baud:

```bash
./scripts/cyd_serial_metrics.py -p /dev/ttyUSB6 --seconds 60
```

Common lines:

```text
STATS cpu=30 ram=39
METRIC vbus_v=12.240 i_a=0.012 p_w=0.150 | raw_bus=0x2645 ...
INA226_RAW cfg=0x4527 cal=0x0831 sh=0x031B(...) bus=0x2642 ...
WIFI ip=192.168.1.236 udp_bcast_port=4210 gw=192.168.1.1
```

`METRIC` values are unfiltered: each metric tick performs one INA226 register read pass and displays the result directly.

## Minimal INA226 Probe

For sensor bring-up without LVGL, Wi-Fi, or the web server:

```bash
./indockerbuild_probe.sh
ESPPORT=/dev/ttyUSB6 ./indockerflash_probe.sh
```

The probe prints INA226 register-derived values every 500 ms and is useful for checking wiring, shunt value, calibration, and bus decoding before running the full UI firmware.

## Wi-Fi Credentials

Copy `wifi.env.example` to `wifi.env` and set the network credentials:

```bash
cp wifi.env.example wifi.env
```

`wifi.env` is read during the Docker/PlatformIO build and generates `firmware/include/wifi_credentials.h`. Do not commit real credentials.

## Building and Flashing

Requires Docker on the host. PlatformIO caches live under `.cache/` and `.pio/` directories that are gitignored.

```bash
./indockerbuild.sh
ESPPORT=/dev/ttyUSB6 ./indockerflash.sh
```

If your board appears elsewhere, change `ESPPORT` accordingly. The default is `/dev/serial-cyd`.

Optional recovery:

- `indockermarauderflash.sh` flashes a prebuilt ESP32 Marauder CYD image.
- `dockerbuild.sh` prepares the ESP-IDF image used by that recovery flow.

## Host Scripts

| Script | Purpose |
|--------|---------|
| `scripts/cyd_udp_listen.py` | Listen for UDP telemetry and print captured JSON payloads. |
| `scripts/cyd_serial_metrics.py` | Read serial `STATS`, `METRIC`, `INA226_RAW`, and Wi-Fi lines. |
| `scripts/collect_cyd_stats.py` | Average serial `STATS cpu` / `ram` values. |
| `scripts/cyd_usb_power_cycle.sh` | Optional USB power helper for controlled hubs. |

## Custom metric font (LCD)

Large LCD metric rows use an **Ubuntu Mono** subset baked into `firmware/src/fonts/cyd_metric_mono.c` (name kept for LVGL). Regenerate from a TTF (default: Ubuntu Mono Regular) with:

```bash
firmware/scripts/regenerate_metric_font.sh
```

The subset is defined by `firmware/scripts/regenerate_metric_font.sh` (`--symbols`); it covers the **LCD** metric strings (e.g. `V`, `I`, `P`, digits, `.`, units, `Error`).

**Note:** The embedded web dashboard loads **Ubuntu Mono** from Google Fonts so the browser matches the LCD style; the device needs access to `fonts.googleapis.com` / `fonts.gstatic.com` unless you self-host the font for an offline LAN.

## Known Limitations

- The current 100 mΩ shunt limits reliable current measurement to about **0.8 A**.
- Residual bus voltage after power removal can be real hardware discharge/leakage behavior; the firmware does not smooth or synthesize Vbus.
- The embedded web page is intentionally simple and unauthenticated; keep it on a trusted LAN.
- LCD Turkish glyphs are not included in the current small font subset.

Third-party licenses (TFT_eSPI, LVGL, Arduino core, PlatformIO packages) remain with their upstream projects.
