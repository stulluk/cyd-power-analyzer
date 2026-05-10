# CYD power analyzer

Firmware and small host scripts for the **ESP32 Cheap Yellow Display (CYD)**—landscape UI with **LVGL 8**, **TFT_eSPI**, **Arduino / PlatformIO**, **Wi‑Fi STA**, and **subnet UDP broadcast telemetry** on port **4210**. Voltage, current, and power are currently **random mock values** (placeholder until INA sensors are wired on I2C).

Repository: https://github.com/stulluk/cyd-power-analyzer  

## Hardware

- CYD (**ESP32‑2432S028**) with ILI9341-class display; pin profile aligns with common Marauder CYD TFT_eSPI setups (`firmware/include/User_Setup.h`).
- References: [ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display), TFT_eSPI fork [ggaljoen/TFT_eSPI](https://github.com/ggaljoen/TFT_eSPI) (pinned zip in `platformio.ini`).

## What the firmware does today

| Area | Behavior |
|------|-----------|
| **Display** | Title, three large monospace metric lines (**Vbus / I / P**), yellow scrolling Wi‑Fi footer. |
| **Refresh** | Metrics and UDP JSON about **5 Hz** (`kMetricsPeriodMs = 200`). |
| **Wi‑Fi** | STA; credentials from repo-root **`wifi.env`** → generated **`firmware/include/wifi_credentials.h`** (never commit real secrets). |
| **Telemetry** | When connected: JSON UDP **broadcast** to subnet on **`CYD_UDP_TELEM_PORT` (4210)**. |
| **Serial** | `STATS cpu=N ram=M` about **once per second** (CRLF for minicom); `WIFI ...` every **3×** STATS. |

Copy `wifi.env.example` → `wifi.env` and set `SSID` / `psk`.

---

## Telemetry and monitoring (theory)

### JSON over UDP (~5 Hz)

Each datagram is one JSON object (no newline inside). Example:

```json
{"vbus_v":4.844,"i_a":3.55,"p_w":47.048,"ms":139007,"cpu":28,"ram":37}
```

| Field | Meaning |
|-------|---------|
| `vbus_v`, `i_a`, `p_w` | Mock bus voltage (V), current (A), power (W)—**replace with sensor reads later**. |
| `ms` | `millis()` at send time. |
| `cpu` | Integer **estimated CPU load %** (0–100)—see below. |
| `ram` | Integer **estimated internal DRAM heap utilization %** (0–100)—see below. |

`cpu` and `ram` are **refreshed in the firmware about once per second** (same task as serial `STATS`) but are **attached to every JSON packet**. Between refreshes you see the latest snapshot repeatedly.

Why broadcast? Avoids tying telemetry to one PC IP; any host on the LAN can listen on UDP 4210 (firewall permitting).

### `cpu`: idle-hook estimate (not hardware PMU)

ESP32 runs FreeRTOS idle tasks on **both** cores when nothing else is ready. We register idle hooks that increment counters. Once at boot we measure how fast those counters grow during a calm window (`idle_ref`). Each second we compare average idle increments to that reference:

- High idle increments → CPUs were mostly idle → **low `cpu`**.
- Low idle increments vs reference → **`cpu`** approaches **100**.

So `cpu` is a **cheap, repeatable heuristic** suitable for regressions—it is **not** a vendor-accurate “% of cycles” profiler.

### `ram`: fraction of internal **malloc-capable heap** used

On ESP-IDF / Arduino there are several memory buckets (internal SRAM, optionally external SPIRAM capabilities, DMA-capable caps, etc.). For a single actionable number without dumping three “heap\*” counters, we report **one percentage**:

- **`heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`** — heap arena the allocator tracks for ordinary internal 8‑bit DRAM.
- **`heap_caps_get_free_size(same)`** — bytes still **free** in that arena.

Then:

```text
ram = round( 100 × (total − free) / total ), clamped to 0–100
```

Interpret `ram` as **“internal heap occupancy”**, not entire chip SRAM and not Bluetooth/Wi‑Fi proprietary pools. Typical ESP32 setups leave a large fraction of chip RAM for radios and drivers; remaining internal heap size varies by SDK options.

---

## Building and flashing (Docker, recommended)

Requires **Docker** on the host. PlatformIO caches live under `.cache/` (gitignored).

1. `./indockerbuild.sh` — compile (`pio run` in `firmware/`).
2. Plug CYD USB; set `ESPPORT` if needed (default `/dev/serial-cyd`).
3. `./indockerflash.sh` — `pio run -t upload`.

Optional: `dockerbuild.sh` pulls the ESP-IDF image used only by **Marauder stock flash** flow (not required for this project’s PlatformIO build).

Optional recovery: `indockermarauderflash.sh` flashes a prebuilt **ESP32 Marauder** CYD image from Fr4nkFletcher’s repo (clones into `.cache/` on first run).

---

## Host scripts

| Script | Purpose |
|--------|---------|
| `scripts/cyd_udp_listen.py` (+ `.sh` wrapper) | Append captured JSON lines to a file (timestamp + peer + payload). |
| `scripts/collect_cyd_stats.py` (+ `.sh`) | Average `STATS cpu` / `ram` from UART (needs `pyserial`). |
| `scripts/cyd_usb_power_cycle.sh` | Optional USB power helpers (if you use a controlled hub). |

---

## Custom metric font

Large digits use **DejaVu Sans Mono** subset in `firmware/src/fonts/cyd_metric_mono.c`. Regenerate with `firmware/scripts/regenerate_metric_font.sh` (needs Node `lv_font_conv`).

---

## Roadmap (not in firmware yet)

- INA219 / INA226 / INA3221 on **I2C** (CYD exposes known free pins; verify your PCB rev).
- Optional **multicast** or per-host unicast telemetry.
- Desktop plotter app.

Third-party licenses (TFT_eSPI, LVGL, Arduino core) remain with their upstreams.
