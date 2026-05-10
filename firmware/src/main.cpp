/*
 * Mock power readout on CYD: LVGL 8.3 + TFT_eSPI (ggaljoen fork).
 *
 * WiFi: STA credentials from repo-root wifi.env (pre-build generates include/wifi_credentials.h).
 * Telemetry: UDP JSON broadcast on subnet to CYD_UDP_TELEM_PORT (4210) each metrics tick when connected.
 *   Fields: vbus_v, i_a, p_w, ms (every tick ~5 Hz) + cpu, ram (1 Hz snapshot in stats_task).
 *   ram = internal malloc heap used % from heap_caps total/free (MALLOC_CAP_INTERNAL|8BIT).
 *
 * Metrics: monospace columns — %-4s + %7.3f (~13 chars incl. unit; narrower than %-5s + %8.3f for 320-wide).
 *
 * Typography: Title Montserrat 14; metrics DejaVu Sans Mono 36 (4 bpp, fixed-width subset);
 *   footer SUBPX yellow. Font: src/fonts/cyd_metric_mono.c (lv_font_conv, see file header).
 * Landscape: setRotation(1) → 320×240. Metrics refresh ~5 Hz.
 *
 * Serial:
 *   STATS cpu=N ram=M (1 Hz); CRLF line endings for minicom.
 *   WIFI ip=... udp_port=... (every 3 s)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "User_Setup.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "wifi_credentials.h"

#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_freertos_hooks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr uint8_t kRotation = 1;
static constexpr uint16_t kTftW = 320;
static constexpr uint16_t kTftH = 240;
static constexpr int kMetricsPeriodMs = 200;

static constexpr int kBlPinPrimary = 21;
static constexpr int kBlPinAlt = 27;

TFT_eSPI tft;
static WiFiUDP s_udp;
static bool s_udp_bound = false;

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

static float rnd_range(float lo, float hi) {
  const uint32_t r = esp_random();
  const float t = (r & 0xffffffu) / float(0x1000000);
  return lo + t * (hi - lo);
}

static void backlight_pins_output_low(void) {
  pinMode(kBlPinPrimary, OUTPUT);
  pinMode(kBlPinAlt, OUTPUT);
  digitalWrite(kBlPinPrimary, LOW);
  digitalWrite(kBlPinAlt, LOW);
}

static void backlight_pins_on(void) {
  pinMode(kBlPinPrimary, OUTPUT);
  pinMode(kBlPinAlt, OUTPUT);
  digitalWrite(kBlPinPrimary, HIGH);
  digitalWrite(kBlPinAlt, HIGH);
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
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    const String ip = WiFi.localIP().toString();
    std::snprintf(buf, buflen, "WiFi: OK %s  UDP:%u", ip.c_str(),
                  static_cast<unsigned>(CYD_UDP_TELEM_PORT));
    return;
  }
  if (st == WL_IDLE_STATUS || st == WL_SCAN_COMPLETED) {
    std::snprintf(buf, buflen, "WiFi: Connecting...");
    return;
  }
  std::snprintf(buf, buflen, "WiFi: Not connected (%d)", static_cast<int>(st));
}

static void telemetry_udp_send(float v, float a, float p) {
  wifi_bind_udp_if_needed();
  if (!s_udp_bound || WiFi.status() != WL_CONNECTED) {
    return;
  }
  const int32_t tc = s_telem_cpu;
  const uint32_t tr = s_telem_ram_pct;

  char json[200];
  std::snprintf(json, sizeof(json),
                "{\"vbus_v\":%.3f,\"i_a\":%.3f,\"p_w\":%.3f,\"ms\":%lu,\"cpu\":%ld,\"ram\":%lu}",
                static_cast<double>(v), static_cast<double>(a), static_cast<double>(p),
                static_cast<unsigned long>(millis()), static_cast<long>(tc),
                static_cast<unsigned long>(tr));

  const IPAddress bc = subnet_broadcast();
  if (!s_udp.beginPacket(bc, CYD_UDP_TELEM_PORT)) {
    return;
  }
  s_udp.print(json);
  s_udp.endPacket();
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
  const float v = rnd_range(0.0f, 24.0f);
  const float a = rnd_range(0.0f, 5.0f);
  const float p = rnd_range(0.0f, 120.0f);

  /* One step tighter than %-5s + %8.3f; values >999 widen min width (acceptable for this mock range). */
  char line[40];
  std::snprintf(line, sizeof(line), "%-4s%7.3f V", "Vbus", static_cast<double>(v));
  lv_label_set_text(s_lbl_v, line);
  std::snprintf(line, sizeof(line), "%-4s%7.3f A", "I", static_cast<double>(a));
  lv_label_set_text(s_lbl_i, line);
  std::snprintf(line, sizeof(line), "%-4s%7.3f W", "P", static_cast<double>(p));
  lv_label_set_text(s_lbl_p, line);

  char foot[120];
  wifi_footer_line(foot, sizeof(foot));
  lv_label_set_text(s_lbl_foot, foot);

  telemetry_udp_send(v, a, p);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\r\nCYD LVGL + TFT_eSPI + WiFi telemetry\r\n");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cyd_wifi::k_ssid, cyd_wifi::k_psk);

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
  lv_label_set_text(title, "CYD power analyzer");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x00ffff), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

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

  /* Tighter vertical pack (~54 px row pitch) for 36 pt mono — user wanted less line gap + less label/value gap (see snprintf above). */
  lv_obj_align(s_lbl_v, LV_ALIGN_CENTER, 0, -57);
  lv_obj_align(s_lbl_i, LV_ALIGN_CENTER, 0, -3);
  lv_obj_align(s_lbl_p, LV_ALIGN_CENTER, 0, 51);

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
}

void loop() {
  lv_timer_handler();
  delay(5);
}
