/*
 * Power readout (INA226 on I2C) on CYD: LVGL 8.3 + TFT_eSPI (ggaljoen fork).
 *
 * WiFi: STA credentials from repo-root wifi.env (pre-build generates include/wifi_credentials.h).
 * Telemetry: UDP JSON broadcast on subnet to CYD_UDP_TELEM_PORT (4210) each metrics tick when connected.
 *   Fields: vbus_v, i_a, p_w, ms (every tick ~5 Hz) + cpu, ram (1 Hz snapshot in stats_task).
 *   ram = internal malloc heap used % from heap_caps total/free (MALLOC_CAP_INTERNAL|8BIT).
 *
 * Metrics: monospace columns — %-4s + %7.3f (~13 chars incl. unit; narrower than %-5s + %8.3f for 320-wide).
 *
 * Typography: Title Montserrat 14; metrics Ubuntu Mono 42 (LVGL subset cyd_metric_mono + same family on web).
 *   footer SUBPX yellow. Regenerate: firmware/scripts/regenerate_metric_font.sh
 * Landscape: setRotation(1) → 320×240. Metrics refresh ~5 Hz.
 *
 * Serial:
 *   STATS cpu=N ram=M (1 Hz); METRIC vbus_v/i_a/p_w (1 Hz) for host verification scripts.
 *   WIFI ip=... udp_port=... (every 3 s)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Wire.h>
#include "User_Setup.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "wifi_credentials.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_freertos_hooks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr uint8_t kRotation = 1;
static constexpr uint16_t kTftW = 320;
static constexpr uint16_t kTftH = 240;
static constexpr int kMetricsPeriodMs = 200;
/** INA226 (TI) — not register-compatible with INA219. Default I2C 0x40; change if ADDR pins differ. */
static constexpr uint8_t kIna226Addr = 0x40;
static constexpr uint8_t kI2cSda = 27;
static constexpr uint8_t kI2cScl = 22;
static constexpr uint32_t kI2cHz = 100000;
static constexpr uint32_t kSensorRetryMs = 1000;
/** TEMP: non-zero adds to displayed Vbus only (LCD, JSON, UDP, METRIC). Keep 0 for production. */
static constexpr float kTempDisplayVbusOffsetV = 0.0f;
/**
 * PCB shunt — must match the resistor marking (e.g. R100 = 0.100 Ω).
 * INA226 shunt ADC FS ≈ 81.92 mV ⇒ linear Imax ≈ 0.08192/R_shunt (here ~0.82 A).
 */
static constexpr float kIna226ShuntOhms = 0.1f;
/** Current_LSB (= max_expected/32768) + TI CAL; keep ≤ ~linear shunt FS above. */
static constexpr float kIna226MaxExpectedAmps = 0.8f;
/** Bus volts: TI low 3 bits are flags; multiply (raw & ~7) × 1.25 mV (same as Rob Tillaart INA226 lib). DO NOT use (>>3)×1.25 mV — it is ~×8 low. */

static inline float ina226_bus_volts(uint16_t bus_raw) {
  return static_cast<float>(bus_raw & 0xFFF8U) * 1.25e-3f;
}

/** Print RAW hex registers on serial every N STATS lines (boot-glass). Set 0 to disable. */
static constexpr uint16_t kIna226RawDumpIntervalStats = 10;

static constexpr uint16_t kIna226RegConfig = 0x0000;
static constexpr uint16_t kIna226RegShuntV = 0x0001;
static constexpr uint16_t kIna226RegBusV = 0x0002;
static constexpr uint16_t kIna226RegPower = 0x0003;
static constexpr uint16_t kIna226RegCurrent = 0x0004;
static constexpr uint16_t kIna226RegCal = 0x0005;
static constexpr uint16_t kCfgContShuntBus = 0x4527;

static constexpr int kBlPinPrimary = 21;
static constexpr int kBlPinAlt = -1;

TFT_eSPI tft;
static WiFiUDP s_udp;
static WebServer s_http(80);
static bool s_udp_bound = false;

static SemaphoreHandle_t s_wire_mtx;

static lv_disp_draw_buf_t s_draw_buf;
/* Partial framebuffer lines (smaller than before) to leave DRAM for WiFi. */
static lv_color_t s_buf[kTftW * 16];

static lv_obj_t *s_lbl_v;
static lv_obj_t *s_lbl_i;
static lv_obj_t *s_lbl_p;
static lv_obj_t *s_lbl_foot;

static volatile uint32_t s_idle_ticks[2] = {0, 0};
static uint32_t s_idle_ref_avg = 1;

static volatile int32_t s_telem_cpu = 0;
static volatile uint32_t s_telem_ram_pct = 0;
static volatile bool s_sensor_ok = false;
static volatile uint32_t s_sensor_fail_count = 0;
static const char *s_sensor_error = "boot";

/** Latest V/I/W (unfiltered; each metrics tick is one INA226 register read pass). */
static volatile float s_metric_vbus = 0.0f;
static volatile float s_metric_i = 0.0f;
static volatile float s_metric_p = 0.0f;

/** Last-good sample raw regs (INA226 dumps these on serial periodically). */
static volatile uint16_t s_ina226_last_cfg = 0;
static volatile uint16_t s_ina226_last_cal = 0;
static volatile uint16_t s_ina226_last_sv = 0;
static volatile uint16_t s_ina226_last_bv = 0;
static volatile uint16_t s_ina226_last_cur = 0;
static volatile uint16_t s_ina226_last_pw = 0;

static uint32_t s_last_sensor_retry_ms = 0;

static float s_ina226_current_lsb_a = 0.0f;
static float s_ina226_power_lsb_w = 0.0f;

static bool wire_take(void) {
  return s_wire_mtx != nullptr && xSemaphoreTake(s_wire_mtx, pdMS_TO_TICKS(80)) == pdTRUE;
}

static void wire_give(void) {
  if (s_wire_mtx != nullptr) {
    xSemaphoreGive(s_wire_mtx);
  }
}

static bool i2c_read_u16(uint8_t dev, uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const int n = Wire.requestFrom(static_cast<int>(dev), 2);
  if (n != 2 || Wire.available() < 2) {
    return false;
  }
  const uint8_t hi = Wire.read();
  const uint8_t lo = Wire.read();
  out = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
  return true;
}

static bool i2c_write_u16(uint8_t dev, uint8_t reg, uint16_t val) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(val >> 8));
  Wire.write(static_cast<uint8_t>(val & 0xff));
  return Wire.endTransmission(true) == 0;
}

/**
 * TI INA226 calibration (same form as INA226_WE / TI app notes):
 *   CAL = floor(0.00512 / (Current_LSB × R_shunt))
 *   Current_LSB = max_expected_A / 32768
 * Do NOT use 51200 here — that was a scaling mistake and forces CAL=0xFFFF (broken current/power).
 */
static bool ina226_try_recover(bool silent) {
  Wire.beginTransmission(kIna226Addr);
  if (Wire.endTransmission(true) != 0) {
    s_sensor_error = "i2c_not_found";
    return false;
  }

  if (!wire_take()) {
    s_sensor_error = "wire_lock";
    return false;
  }
  if (!i2c_write_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegConfig), 0x8000)) {
    wire_give();
    s_sensor_error = "rst_fail";
    return false;
  }
  wire_give();
  delay(15);

  uint16_t mfg_id = 0;
  uint16_t die_id = 0;
  if (!wire_take()) {
    s_sensor_error = "wire_lock";
    return false;
  }
  (void)i2c_read_u16(kIna226Addr, 0xFE, mfg_id);
  (void)i2c_read_u16(kIna226Addr, 0xFF, die_id);

  const float current_lsb_a = kIna226MaxExpectedAmps / 32768.0f;
  if (current_lsb_a <= 0.0f || kIna226ShuntOhms <= 0.0f) {
    wire_give();
    s_sensor_error = "bad_cal_param";
    return false;
  }
  const float cal_f = floorf(0.00512f / (current_lsb_a * kIna226ShuntOhms));
  const uint16_t calibration =
      static_cast<uint16_t>(cal_f > 65535.0f ? 65535.0f : (cal_f < 1.0f ? 1.0f : cal_f));

  const float shunt_mv_at_max_a = kIna226MaxExpectedAmps * kIna226ShuntOhms * 1000.0f;
  if (!silent && shunt_mv_at_max_a > 81.92f * 1.001f) {
    Serial.printf(
        "INA226 WARN: %.3f V expected across Rshunt at %.1f A (%.2f mV) exceeds ~81.92 mV ADC FS — fix Rshunt/maxI constants\r\n",
        static_cast<double>(kIna226MaxExpectedAmps * kIna226ShuntOhms),
        static_cast<double>(kIna226MaxExpectedAmps), static_cast<double>(shunt_mv_at_max_a));
  }

  if (!i2c_write_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegCal), calibration)) {
    wire_give();
    s_sensor_error = "cal_write_fail";
    return false;
  }
  /* Continuous shunt + bus conversion (common breakout config); adjust if noisy. */
  if (!i2c_write_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegConfig), kCfgContShuntBus)) {
    wire_give();
    s_sensor_error = "cfg_fail";
    return false;
  }
  uint16_t rb_cfg = 0;
  uint16_t rb_cal = 0;
  (void)i2c_read_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegConfig), rb_cfg);
  (void)i2c_read_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegCal), rb_cal);
  s_ina226_last_cfg = rb_cfg;
  s_ina226_last_cal = rb_cal;
  wire_give();

  s_ina226_current_lsb_a = current_lsb_a;
  s_ina226_power_lsb_w = 25.0f * current_lsb_a;

  if (!silent) {
    Serial.printf(
        "INA226 mfg_id=0x%04X die_id=0x%04X cal=%u lsbi=%.9f lsbp=%.6f Rsh=%.4fohm maxI=%.1f\r\n",
        static_cast<unsigned>(mfg_id), static_cast<unsigned>(die_id), static_cast<unsigned>(calibration),
        static_cast<double>(s_ina226_current_lsb_a), static_cast<double>(s_ina226_power_lsb_w),
        static_cast<double>(kIna226ShuntOhms), static_cast<double>(kIna226MaxExpectedAmps));
    if (mfg_id != 0x5449U) {
      Serial.printf("INA226 WARN: Manufacturer ID!=0x5449 (wrong chip or clones); check wiring/address\r\n");
    }
    if (die_id != 0x2260U) {
      Serial.printf("INA226 WARN: Die ID!=0x2260; expected INA226 on many TI parts\r\n");
    }
    if (calibration == 65535U) {
      Serial.printf(
          "INA226 WARN: CAL=0xFFFF (saturated)— adjust R_shunt/maxI so CAL fits 16-bit\r\n");
    }
  }
  s_sensor_error = "ok";
  return true;
}

/**
 * Reads INA226 bus voltage, signed current/power from calibrated registers,
 * falling back to shunt voltage (2.5µV/bit) × I if current looks invalid.
 */
static bool ina226_read(float &vbus_v, float &i_a, float &p_w) {
  if (s_ina226_current_lsb_a <= 0.0f) {
    s_sensor_error = "not_init";
    return false;
  }

  constexpr int kSamples = 1;
  float sum_v = 0.0f;
  float sum_i = 0.0f;
  float sum_p = 0.0f;
  int good = 0;

  for (int s = 0; s < kSamples; s++) {
    if (!wire_take()) {
      s_sensor_error = "wire_lock";
      continue;
    }
    uint16_t bus_reg = 0;
    uint16_t cur_reg = 0;
    uint16_t pwr_reg = 0;
    uint16_t sh_reg = 0;
    const bool ok_bus = i2c_read_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegBusV), bus_reg);
    const bool ok_cur =
        i2c_read_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegCurrent), cur_reg);
    const bool ok_pwr =
        i2c_read_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegPower), pwr_reg);
    const bool ok_sh =
        i2c_read_u16(kIna226Addr, static_cast<uint8_t>(kIna226RegShuntV), sh_reg);
    wire_give();

    if (!ok_bus || !ok_cur || !ok_pwr || !ok_sh) {
      s_sensor_error = "read_fail";
      continue;
    }
    const uint16_t bus_magnitude_masked = static_cast<uint16_t>(bus_reg & 0xFFF8U);
    const float v_bus = ina226_bus_volts(bus_reg);
    /* Plausible wiring: nonzero shunt but bus ADC magnitude zero ⇒ often floating VBUS. */
    const int16_t sh_s_precheck = static_cast<int16_t>(sh_reg);
    const float svmv_precheck =
        fabsf(static_cast<float>(sh_s_precheck)) * static_cast<float>(2.5e-3);
    if (bus_magnitude_masked == 0U && svmv_precheck >= 50.0f) {
      s_sensor_error = "bus0_volt";
      continue;
    }

    constexpr int kCurRegsat = 31000;
    if (abs(static_cast<int>(static_cast<int16_t>(cur_reg))) >= kCurRegsat) {
      s_sensor_error = "adc_sat";
      continue;
    }
    const int16_t cur_s = static_cast<int16_t>(cur_reg);
    float i_meas = static_cast<float>(cur_s) * s_ina226_current_lsb_a;
    float p_meas = static_cast<float>(pwr_reg) * s_ina226_power_lsb_w;

    const int16_t sh_s = static_cast<int16_t>(sh_reg);
    const float v_shunt_v = static_cast<float>(sh_s) * 2.5e-6f;
    constexpr float kShuntVfs_V = 0.08192f;
    if (fabsf(v_shunt_v) >= kShuntVfs_V * 0.99f) {
      s_sensor_error = "shunt_clip";
      continue;
    }
    const float i_from_shunt = v_shunt_v / kIna226ShuntOhms;

    if ((!isfinite(i_meas)) ||
        ((fabsf(i_meas - i_from_shunt) > 0.75f) && (fabsf(i_from_shunt) > fabsf(i_meas)))) {
      i_meas = i_from_shunt;
      p_meas = v_bus * i_meas;
    }

    if (!isfinite(v_bus) || !isfinite(i_meas) || !isfinite(p_meas) || v_bus < -0.1f ||
        v_bus > 41.0f || fabsf(i_meas) > 40.0f) {
      s_sensor_error = "invalid_data";
      continue;
    }
    sum_v += v_bus < 0.0f ? 0.0f : v_bus;
    sum_i += i_meas;
    sum_p += p_meas;
    s_ina226_last_sv = sh_reg;
    s_ina226_last_bv = bus_reg;
    s_ina226_last_cur = cur_reg;
    s_ina226_last_pw = pwr_reg;
    good++;
  }

  if (good == 0) {
    if (s_sensor_error == nullptr || s_sensor_error[0] == '\0' ||
        strcmp(s_sensor_error, "ok") == 0) {
      s_sensor_error = "read_fail";
    }
    return false;
  }

  vbus_v = sum_v / static_cast<float>(good);
  i_a = sum_i / static_cast<float>(good);
  p_w = sum_p / static_cast<float>(good);
  if (!isfinite(vbus_v) || !isfinite(i_a) || !isfinite(p_w)) {
    s_sensor_error = "invalid_data";
    return false;
  }

  s_sensor_error = "ok";
  return true;
}

static uint32_t internal_heap_used_percent(void) {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const size_t total_sz = heap_caps_get_total_size(caps);
  const size_t free_sz = heap_caps_get_free_size(caps);
  if (total_sz == 0U) {
    return 0;
  }
  if (free_sz >= total_sz) {
    return 0;
  }
  const size_t used = total_sz - free_sz;
  uint32_t pct = static_cast<uint32_t>((100ULL * used) / total_sz);
  if (pct > 100U) {
    pct = 100;
  }
  return pct;
}

extern "C" bool cyd_idle_hook_cpu0(void) {
  s_idle_ticks[0]++;
  return false;
}

extern "C" bool cyd_idle_hook_cpu1(void) {
  s_idle_ticks[1]++;
  return false;
}

static void calibrate_idle_reference_ms(uint32_t ms) {
  const uint32_t a0 = s_idle_ticks[0];
  const uint32_t a1 = s_idle_ticks[1];
  delay(ms);
  const uint32_t d0 = s_idle_ticks[0] - a0;
  const uint32_t d1 = s_idle_ticks[1] - a1;
  s_idle_ref_avg = (d0 + d1) / 2;
  if (s_idle_ref_avg < 1U) {
    s_idle_ref_avg = 1;
  }
}

static void stats_task(void * /*arg*/) {
  vTaskDelay(pdMS_TO_TICKS(500));
  uint32_t wifi_lines = 0;
  uint32_t stats_seq = 0;
  for (;;) {
    const uint32_t u0 = s_idle_ticks[0];
    const uint32_t u1 = s_idle_ticks[1];
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t d0 = s_idle_ticks[0] - u0;
    const uint32_t d1 = s_idle_ticks[1] - u1;
    const uint32_t avg = (d0 + d1) / 2;
    int cpu_est = 100 - static_cast<int>((100ULL * avg) / s_idle_ref_avg);
    if (cpu_est < 0) {
      cpu_est = 0;
    }
    if (cpu_est > 100) {
      cpu_est = 100;
    }

    const uint32_t ram_pct = internal_heap_used_percent();

    s_telem_cpu = cpu_est;
    s_telem_ram_pct = ram_pct;

    Serial.printf("STATS cpu=%d ram=%lu\r\n", cpu_est, static_cast<unsigned long>(ram_pct));
    if (s_sensor_ok) {
      const uint16_t bv = s_ina226_last_bv;
      const unsigned br13 =
          static_cast<unsigned>(static_cast<uint16_t>((bv >> 3) & 0x1FFFU));
      const unsigned bus_mask_u = static_cast<unsigned>(bv & 0xFFF8U);
      const int16_t sms = static_cast<int16_t>(s_ina226_last_sv);
      const float svmv = static_cast<float>(sms) * static_cast<float>(2.5e-3); /* signed mV */
      Serial.printf(
          "METRIC vbus_v=%.3f i_a=%.3f p_w=%.3f | raw_bus=0x%04X bus_mask=0x%04X fld13=%u sh_mv=%.4f sicur=%d "
          "cal=0x%04X Rsh_const=%.4f maxI_prog=%.2f\r\n",
          static_cast<double>(s_metric_vbus), static_cast<double>(s_metric_i),
          static_cast<double>(s_metric_p), static_cast<unsigned>(bv), bus_mask_u, br13,
          static_cast<double>(svmv), static_cast<int>(static_cast<int16_t>(s_ina226_last_cur)),
          static_cast<unsigned>(s_ina226_last_cal),
          static_cast<double>(kIna226ShuntOhms),
          static_cast<double>(kIna226MaxExpectedAmps));
    } else {
      const uint16_t bv = s_ina226_last_bv;
      const int16_t sms = static_cast<int16_t>(s_ina226_last_sv);
      const float svmv = static_cast<float>(sms) * static_cast<float>(2.5e-3);
      Serial.printf(
          "METRIC sensor_ok=0 err=%s | raw_bus=0x%04X sh_mv=%.4f sicur=%d Rsh_const=%.4f maxI_prog=%.2f\r\n",
          s_sensor_error != nullptr ? s_sensor_error : "?", static_cast<unsigned>(bv),
          static_cast<double>(svmv), static_cast<int>(static_cast<int16_t>(s_ina226_last_cur)),
          static_cast<double>(kIna226ShuntOhms),
          static_cast<double>(kIna226MaxExpectedAmps));
    }

    stats_seq++;
    if (kIna226RawDumpIntervalStats != 0U &&
        (stats_seq % static_cast<uint32_t>(kIna226RawDumpIntervalStats)) == 0U) {
      const uint16_t bv = s_ina226_last_bv;
      const uint16_t sv = s_ina226_last_sv;
      const unsigned bus_raw13 = (static_cast<unsigned>(bv >> 3U) & 0x1FFFU);
      /* Same decode as ina226_bus_volts ×1000 — not an extra fudge; name avoids "corr" confusion. */
      const float vbus_adc_mv =
          static_cast<float>(bv & 0xFFF8U) * static_cast<float>(1.25e-3 * 1000.0);
      const int16_t sv_s = static_cast<int16_t>(sv);
      const float svmv = static_cast<float>(sv_s) * 0.0025f;
      Serial.printf(
          "INA226_RAW cfg=0x%04X cal=0x%04X sh=0x%04X(%.4fmV) bus=0x%04X cnvr=%u ovf=%u fld13=%u Vbus_adc_mV=%.3f | "
          "cur=0x%04X si=%d pwr=0x%04X Current_LSB=%.9f shunt_ohm=%.4f\r\n",
          static_cast<unsigned>(s_ina226_last_cfg), static_cast<unsigned>(s_ina226_last_cal),
          static_cast<unsigned>(sv), static_cast<double>(svmv), static_cast<unsigned>(bv),
          static_cast<unsigned>((bv >> 2U) & 1U), static_cast<unsigned>((bv >> 1U) & 1U),
          bus_raw13, static_cast<double>(vbus_adc_mv), static_cast<unsigned>(s_ina226_last_cur),
          static_cast<int>(static_cast<int16_t>(s_ina226_last_cur)),
          static_cast<unsigned>(s_ina226_last_pw), static_cast<double>(s_ina226_current_lsb_a),
          static_cast<double>(kIna226ShuntOhms));
    }

    wifi_lines++;
    if (wifi_lines >= 3) {
      wifi_lines = 0;
      if (WiFi.status() == WL_CONNECTED) {
        const String ip = WiFi.localIP().toString();
        const String gw = WiFi.gatewayIP().toString();
        Serial.printf("WIFI ip=%s udp_bcast_port=%u gw=%s\r\n", ip.c_str(),
                      static_cast<unsigned>(CYD_UDP_TELEM_PORT), gw.c_str());
      } else {
        Serial.printf("WIFI status=%d (no IP)\r\n", static_cast<int>(WiFi.status()));
      }
    }
  }
}

static void backlight_pins_output_low(void) {
  pinMode(kBlPinPrimary, OUTPUT);
  digitalWrite(kBlPinPrimary, LOW);
  if (kBlPinAlt >= 0) {
    pinMode(kBlPinAlt, OUTPUT);
    digitalWrite(kBlPinAlt, LOW);
  }
}

static void backlight_pins_on(void) {
  pinMode(kBlPinPrimary, OUTPUT);
  digitalWrite(kBlPinPrimary, HIGH);
  if (kBlPinAlt >= 0) {
    pinMode(kBlPinAlt, OUTPUT);
    digitalWrite(kBlPinAlt, HIGH);
  }
}

static IPAddress subnet_broadcast(void) {
  const IPAddress lip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress bc;
  for (int i = 0; i < 4; i++) {
    bc[i] = static_cast<uint8_t>(lip[i] | (~mask[i] & 0xFF));
  }
  return bc;
}

static void wifi_bind_udp_if_needed(void) {
  if (WiFi.status() != WL_CONNECTED) {
    if (s_udp_bound) {
      s_udp.stop();
      s_udp_bound = false;
    }
    return;
  }
  if (!s_udp_bound) {
    if (s_udp.begin(0)) {
      s_udp_bound = true;
    }
  }
}

static void wifi_footer_line(char *buf, size_t buflen) {
  const char *sensor_txt = s_sensor_ok ? "OK" : "NOK";
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    const String ip = WiFi.localIP().toString();
    std::snprintf(buf, buflen, "WiFi: OK %s  UDP:%u  SENSOR:%s", ip.c_str(),
                  static_cast<unsigned>(CYD_UDP_TELEM_PORT), sensor_txt);
    return;
  }
  if (st == WL_IDLE_STATUS || st == WL_SCAN_COMPLETED) {
    std::snprintf(buf, buflen, "WiFi: Connecting...  SENSOR:%s", sensor_txt);
    return;
  }
  std::snprintf(buf, buflen, "WiFi: Not connected (%d)  SENSOR:%s", static_cast<int>(st), sensor_txt);
}

static void telemetry_udp_send(float v, float a, float p, bool sensor_ok) {
  wifi_bind_udp_if_needed();
  if (!s_udp_bound || WiFi.status() != WL_CONNECTED) {
    return;
  }
  const int32_t tc = s_telem_cpu;
  const uint32_t tr = s_telem_ram_pct;

  char json[256];
  if (sensor_ok) {
    std::snprintf(
        json, sizeof(json),
        "{\"vbus_v\":%.3f,\"i_a\":%.3f,\"p_w\":%.3f,\"ms\":%lu,\"cpu\":%ld,\"ram\":%lu,"
        "\"sensor_ok\":true,\"sensor\":\"ina226\",\"error\":\"ok\"}",
        static_cast<double>(v), static_cast<double>(a), static_cast<double>(p),
        static_cast<unsigned long>(millis()), static_cast<long>(tc), static_cast<unsigned long>(tr));
  } else {
    std::snprintf(
        json, sizeof(json),
        "{\"vbus_v\":null,\"i_a\":null,\"p_w\":null,\"ms\":%lu,\"cpu\":%ld,\"ram\":%lu,"
        "\"sensor_ok\":false,\"sensor\":\"ina226\",\"error\":\"%s\"}",
        static_cast<unsigned long>(millis()), static_cast<long>(tc), static_cast<unsigned long>(tr),
        s_sensor_error);
  }

  const IPAddress bc = subnet_broadcast();
  if (!s_udp.beginPacket(bc, CYD_UDP_TELEM_PORT)) {
    return;
  }
  s_udp.print(json);
  s_udp.endPacket();
}

static const char kHttpIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>CYD Power Analyzer</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Ubuntu+Mono:ital,wght@0,400;0,700;1,400&display=swap" rel="stylesheet">
  <style>
    :root { color-scheme: dark; --bg:#0b0f14; --card:#151b23; --fg:#e6edf3; --muted:#7d8590; --accent:#00d4ff; --bad:#ff6b6b; --ok:#52d273;
      --font-metric: "Ubuntu Mono", ui-monospace, monospace; }
    * { box-sizing: border-box; }
    body { margin: 0; min-height: 100vh; background: var(--bg); color: var(--fg); font-family: system-ui, -apple-system, Segoe UI, sans-serif; display: grid; place-items: center; }
    main { width: min(92vw, 460px); background: var(--card); border: 1px solid #30363d; border-radius: 18px; padding: 22px; box-shadow: 0 24px 60px #0008; }
    h1 { margin: 0 0 16px; color: var(--accent); font-size: 1.25rem; font-weight: 700; text-align: center; display: flex; justify-content: center; align-items: baseline; gap: 8px; flex-wrap: wrap; }
    h1 .byline { color: var(--muted); font-size: .72rem; font-weight: 500; letter-spacing: 0; text-decoration: none; }
    h1 .byline:hover { color: var(--accent); text-decoration: underline; }
    .metric { display: grid; grid-template-columns: 1fr auto; gap: 10px; align-items: baseline; padding: 16px 0; border-top: 1px solid #30363d; }
    .metric:first-of-type { border-top: 0; }
    .name { color: var(--muted); font-size: 2.36rem; font-weight: 600; font-family: var(--font-metric); line-height: 1.15; }
    .value { font-family: var(--font-metric); font-size: clamp(2rem, 10vw, 3.4rem); font-weight: 700; letter-spacing: 0; font-variant-numeric: tabular-nums; }
    .unit { color: var(--muted); font-size: 1rem; margin-left: 4px; }
    footer { margin-top: 14px; color: var(--muted); font-size: .86rem; font-family: var(--font-metric); display: flex; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
    .ok { color: var(--ok); }
    .bad { color: var(--bad); }
  </style>
</head>
<body>
  <main>
    <h1>CYD Power Analyzer <a class="byline" href="https://www.drejo.com/" target="_blank" rel="noopener">by Sertaç Tüllük</a></h1>
    <section class="metric"><div class="name">Vbus</div><div><span id="v" class="value">--.---</span><span class="unit">V</span></div></section>
    <section class="metric"><div class="name">Current</div><div><span id="i" class="value">--.---</span><span class="unit">A</span></div></section>
    <section class="metric"><div class="name">Power</div><div><span id="p" class="value">--.---</span><span class="unit">W</span></div></section>
    <footer>
      <span id="status">connecting...</span>
      <span id="sys">cpu --% / ram --%</span>
    </footer>
  </main>
  <script>
    const $ = (id) => document.getElementById(id);
    const fixed = (value) => Number.isFinite(value) ? value.toFixed(3) : "--.---";
    async function refresh() {
      try {
        const r = await fetch("/api/metrics", { cache: "no-store" });
        const m = await r.json();
        $("v").textContent = fixed(m.vbus_v);
        $("i").textContent = fixed(m.i_a);
        $("p").textContent = fixed(m.p_w);
        $("status").textContent = m.sensor_ok ? "SENSOR: OK" : `SENSOR: ${m.error || "NOK"}`;
        $("status").className = m.sensor_ok ? "ok" : "bad";
        $("sys").textContent = `cpu ${m.cpu}% / ram ${m.ram}%`;
      } catch (e) {
        $("status").textContent = "HTTP: reconnecting...";
        $("status").className = "bad";
      }
    }
    refresh();
    setInterval(refresh, 500);
  </script>
</body>
</html>
)HTML";

static void http_send_no_cache(void) {
  s_http.sendHeader("Cache-Control", "no-store, max-age=0");
  s_http.sendHeader("Connection", "close");
}

static void http_handle_root(void) {
  http_send_no_cache();
  s_http.send_P(200, "text/html; charset=utf-8", kHttpIndexHtml);
}

static void http_handle_metrics(void) {
  const bool sensor_ok = s_sensor_ok;
  const int32_t cpu = s_telem_cpu;
  const uint32_t ram = s_telem_ram_pct;
  const String ip = WiFi.localIP().toString();
  char json[320];

  if (sensor_ok) {
    std::snprintf(
        json, sizeof(json),
        "{\"vbus_v\":%.3f,\"i_a\":%.3f,\"p_w\":%.3f,\"ms\":%lu,\"cpu\":%ld,\"ram\":%lu,"
        "\"sensor_ok\":true,\"sensor\":\"ina226\",\"error\":\"ok\",\"ip\":\"%s\"}",
        static_cast<double>(s_metric_vbus), static_cast<double>(s_metric_i),
        static_cast<double>(s_metric_p), static_cast<unsigned long>(millis()), static_cast<long>(cpu),
        static_cast<unsigned long>(ram), ip.c_str());
  } else {
    std::snprintf(
        json, sizeof(json),
        "{\"vbus_v\":null,\"i_a\":null,\"p_w\":null,\"ms\":%lu,\"cpu\":%ld,\"ram\":%lu,"
        "\"sensor_ok\":false,\"sensor\":\"ina226\",\"error\":\"%s\",\"ip\":\"%s\"}",
        static_cast<unsigned long>(millis()), static_cast<long>(cpu), static_cast<unsigned long>(ram),
        s_sensor_error != nullptr ? s_sensor_error : "unknown", ip.c_str());
  }

  http_send_no_cache();
  s_http.send(200, "application/json", json);
}

static void http_handle_not_found(void) {
  http_send_no_cache();
  s_http.send(404, "text/plain; charset=utf-8", "not found\n");
}

static void http_begin(void) {
  s_http.on("/", HTTP_GET, http_handle_root);
  s_http.on("/api/metrics", HTTP_GET, http_handle_metrics);
  s_http.onNotFound(http_handle_not_found);
  s_http.begin();
  Serial.printf("HTTP server started on port 80\r\n");
}

static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  const uint32_t w = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t h = static_cast<uint32_t>(area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors(reinterpret_cast<uint16_t *>(color_p), w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp_drv);
}

static void metrics_timer_cb(lv_timer_t * /*timer*/) {
  float v = 0.0f;
  float a = 0.0f;
  float p = 0.0f;
  bool sensor_ok = ina226_read(v, a, p);
  if (!sensor_ok) {
    const uint32_t now = millis();
    if (now - s_last_sensor_retry_ms >= kSensorRetryMs) {
      s_last_sensor_retry_ms = now;
      sensor_ok = ina226_try_recover(true) && ina226_read(v, a, p);
    }
  }

  const float v_disp = sensor_ok ? (v + kTempDisplayVbusOffsetV) : v;

  if (sensor_ok) {
    char line[40];
    std::snprintf(line, sizeof(line), "%-4s%7.3f V", "Vbus", static_cast<double>(v_disp));
    lv_label_set_text(s_lbl_v, line);
    std::snprintf(line, sizeof(line), "%-4s%7.3f A", "I", static_cast<double>(a));
    lv_label_set_text(s_lbl_i, line);
    std::snprintf(line, sizeof(line), "%-4s%7.3f W", "P", static_cast<double>(p));
    lv_label_set_text(s_lbl_p, line);
  } else {
    char line[40];
    std::snprintf(line, sizeof(line), "%-4s%7s V", "V", "Error");
    lv_label_set_text(s_lbl_v, line);
    std::snprintf(line, sizeof(line), "%-4s%7s A", "I", "Error");
    lv_label_set_text(s_lbl_i, line);
    std::snprintf(line, sizeof(line), "%-4s%7s W", "P", "Error");
    lv_label_set_text(s_lbl_p, line);
    s_sensor_fail_count++;
  }
  s_sensor_ok = sensor_ok;
  if (sensor_ok) {
    s_metric_vbus = v_disp;
    s_metric_i = a;
    s_metric_p = p;
  }

  char foot[120];
  wifi_footer_line(foot, sizeof(foot));
  lv_label_set_text(s_lbl_foot, foot);

  telemetry_udp_send(v_disp, a, p, sensor_ok);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\r\nCYD LVGL + TFT_eSPI + WiFi telemetry\r\n");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cyd_wifi::k_ssid, cyd_wifi::k_psk);
  http_begin();

  s_wire_mtx = xSemaphoreCreateMutex();
  Wire.begin(static_cast<int>(kI2cSda), static_cast<int>(kI2cScl), kI2cHz);
  Wire.setTimeOut(30);
  s_sensor_ok = ina226_try_recover(false);
  if (!s_sensor_ok) {
    s_sensor_fail_count++;
  }

  backlight_pins_output_low();
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  tft.init();
  tft.setRotation(kRotation);
  backlight_pins_on();

  tft.fillScreen(TFT_BLACK);

  lv_init();

  lv_disp_draw_buf_init(&s_draw_buf, s_buf, nullptr, sizeof(s_buf) / sizeof(s_buf[0]));

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = kTftW;
  disp_drv.ver_res = kTftH;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "CYD Power Analyzer");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x00ffff), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, -40, 10);

  lv_obj_t *author = lv_label_create(scr);
  lv_label_set_text(author, "by Sertac Tulluk");
  lv_obj_set_style_text_font(author, &lv_font_montserrat_12_subpx, LV_PART_MAIN);
  lv_obj_set_style_text_color(author, lv_color_hex(0x9aa4ad), LV_PART_MAIN);
  lv_obj_align_to(author, title, LV_ALIGN_OUT_RIGHT_MID, 7, 1);

  s_lbl_v = lv_label_create(scr);
  s_lbl_i = lv_label_create(scr);
  s_lbl_p = lv_label_create(scr);
  lv_obj_set_style_text_font(s_lbl_v, &cyd_metric_mono, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_i, &cyd_metric_mono, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_p, &cyd_metric_mono, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_v, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_i, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_p, lv_color_hex(0xffffff), LV_PART_MAIN);

  lv_label_set_text(s_lbl_v, "Vbus  0.000 V");
  lv_label_set_text(s_lbl_i, "I     0.000 A");
  lv_label_set_text(s_lbl_p, "P     0.000 W");

  /* ~52 px row pitch; nudge whole block up ~10 px vs CENTER so less gap under title. */
  lv_obj_align(s_lbl_v, LV_ALIGN_CENTER, 0, -62);
  lv_obj_align(s_lbl_i, LV_ALIGN_CENTER, 0, -10);
  lv_obj_align(s_lbl_p, LV_ALIGN_CENTER, 0, 42);

  s_lbl_foot = lv_label_create(scr);
  lv_label_set_long_mode(s_lbl_foot, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(s_lbl_foot, kTftW - 8);
  lv_label_set_text(s_lbl_foot, "WiFi: …");
  lv_obj_set_style_text_font(s_lbl_foot, &lv_font_montserrat_12_subpx, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_foot, lv_palette_main(LV_PALETTE_YELLOW), LV_PART_MAIN);
  lv_obj_align(s_lbl_foot, LV_ALIGN_BOTTOM_MID, 0, -8);

  lv_timer_create(metrics_timer_cb, kMetricsPeriodMs, nullptr);

  esp_register_freertos_idle_hook_for_cpu(&cyd_idle_hook_cpu0, 0);
  esp_register_freertos_idle_hook_for_cpu(&cyd_idle_hook_cpu1, 1);
  calibrate_idle_reference_ms(1000);

  s_telem_cpu = 0;
  s_telem_ram_pct = internal_heap_used_percent();
  xTaskCreatePinnedToCore(stats_task, "cyd_stats", 3072, nullptr, 2, nullptr, 0);

  Serial.printf("TFT %ux%u telemetry UDP port=%u (subnet broadcast)\r\n", static_cast<unsigned>(kTftW),
                static_cast<unsigned>(kTftH), static_cast<unsigned>(CYD_UDP_TELEM_PORT));
  Serial.printf("I2C INA226 sda=%u scl=%u addr=0x%02X sensor_ok=%d\r\n", static_cast<unsigned>(kI2cSda),
                static_cast<unsigned>(kI2cScl), static_cast<unsigned>(kIna226Addr),
                static_cast<int>(s_sensor_ok));
}

void loop() {
  s_http.handleClient();
  lv_timer_handler();
  delay(5);
}
