/**
 * INA226 serial probe — like a tiny Battery-Tester style sanity check, no display stack.
 * CYD CN1: Wire SDA=GPIO27, SCL=GPIO22 (adjust if your wiring differs).
 *
 * Serial 115200 baud. Edit R_shunt / max_expected_amps to match your breakout PCB.
 */

#include <Arduino.h>
#include <Wire.h>

#include <cmath>
#include <cstdio>

static constexpr uint8_t kAddr = 0x40;
static constexpr uint8_t kSda = 27;
static constexpr uint8_t kScl = 22;
static constexpr uint32_t kI2cHz = 100000;

/** Match module shunt (e.g. R100 marking = 0.100 Ω). */
static constexpr float kRshunt = 0.1f;
/** ≤ ~linear range: Imax_lin ≈ 81.92mV / Rshunt (~0.82 A for 100 mΩ). */
static constexpr float kMaxExpectedA = 0.8f;

static constexpr uint8_t kRegCfg = 0x00;
static constexpr uint8_t kRegShunt = 0x01;
static constexpr uint8_t kRegBus = 0x02;
static constexpr uint8_t kRegPwr = 0x03;
static constexpr uint8_t kRegCur = 0x04;
static constexpr uint8_t kRegCal = 0x05;
static constexpr uint16_t kCfgContShuntBus = 0x4527;
static constexpr uint16_t kBusOv = 1u << 1;

static float s_cur_lsb = 0.0f;
static float s_pwr_lsb = 0.0f;

static bool i2c_read_u16(uint8_t dev, uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(dev), 2) != 2 || Wire.available() < 2) {
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

static bool ina226_init(void) {
  Wire.beginTransmission(kAddr);
  if (Wire.endTransmission(true) != 0) {
    Serial.println("PROBE: I2C ping to 0x40 NACK (wiring/addr?)");
    return false;
  }

  if (!i2c_write_u16(kAddr, kRegCfg, 0x8000)) {
    return false;
  }
  delay(20);

  uint16_t mfg = 0, die = 0;
  (void)i2c_read_u16(kAddr, 0xFE, mfg);
  (void)i2c_read_u16(kAddr, 0xFF, die);
  const float lsb = kMaxExpectedA / 32768.0f;
  if (lsb <= 0.0f || kRshunt <= 0.0f) {
    return false;
  }
  const float cal = floorf(0.00512f / (lsb * kRshunt));
  const uint16_t calu =
      static_cast<uint16_t>(cal > 65535.0f ? 65535.0f : (cal < 1.0f ? 1.0f : cal));

  if (!i2c_write_u16(kAddr, kRegCal, calu)) {
    return false;
  }
  if (!i2c_write_u16(kAddr, kRegCfg, kCfgContShuntBus)) {
    return false;
  }

  s_cur_lsb = lsb;
  s_pwr_lsb = 25.0f * lsb;

  Serial.printf("PROBE boot: mfg=0x%04X die=0x%04X CAL=%u Current_LSB=%.9f Rsh=%.4fΩ maxI_prog=%.2f\r\n",
                static_cast<unsigned>(mfg), static_cast<unsigned>(die), static_cast<unsigned>(calu),
                static_cast<double>(s_cur_lsb), static_cast<double>(kRshunt),
                static_cast<double>(kMaxExpectedA));
  if (mfg != 0x5449U || die != 0x2260U) {
    Serial.println("PROBE WARN: unexpected mfg/die — still reading regs");
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\r\n\r\nINA226_SERIAL_PROBE CYD SDA=27 SCL=22 addr=0x40 — 500ms lines\r\n");

  Wire.begin(static_cast<int>(kSda), static_cast<int>(kScl), kI2cHz);
  Wire.setTimeOut(50);

  if (!ina226_init()) {
    Serial.println("PROBE: init FAILED — fix I2C / power / ADDR");
  }
}

void loop() {
  delay(500);

  if (s_cur_lsb <= 0.0f) {
    (void)ina226_init();
    return;
  }

  uint16_t sh = 0, bus = 0, cur = 0, pwr = 0;
  if (!i2c_read_u16(kAddr, kRegShunt, sh) || !i2c_read_u16(kAddr, kRegBus, bus) ||
      !i2c_read_u16(kAddr, kRegCur, cur) || !i2c_read_u16(kAddr, kRegPwr, pwr)) {
    Serial.println("PROBE read_fail");
    return;
  }

  const int16_t sh_s = static_cast<int16_t>(sh);
  const float v_shunt_v = static_cast<float>(sh_s) * 2.5e-6f;
  const float v_shunt_mv = v_shunt_v * 1000.0f;
  const float i_from_r = v_shunt_v / kRshunt;

  /* Match Rob Tillaart / TI: clear low flag bits, ×1.25 mV (not (>>3)×1.25 mV — that under-reads ~8×). */
  const uint16_t bus13 = static_cast<uint16_t>((bus >> 3) & 0x1FFFU);
  const float v_bus = static_cast<float>(bus & 0xFFF8U) * 1.25e-3f;
  const bool ovf = (bus & kBusOv) != 0;

  const float i_cal = static_cast<float>(static_cast<int16_t>(cur)) * s_cur_lsb;
  const float p_cal = static_cast<float>(pwr) * s_pwr_lsb;

  Serial.printf(
      "Vbus=%.4fV (raw_bus=0x%04X fld13=%u mask=0x%04X ovf=%d) | Vsh=%.5fV %.3fmV | "
      "I_R=%.4fA | I_CAL=%.4fA | P_CAL=%.4fW | reg_cur=0x%04X reg_pwr=0x%04X\r\n",
      static_cast<double>(v_bus), static_cast<unsigned>(bus), static_cast<unsigned>(bus13),
      static_cast<unsigned>(bus & 0xFFF8U), static_cast<int>(ovf ? 1 : 0),
      static_cast<double>(v_shunt_v),
      static_cast<double>(v_shunt_mv), static_cast<double>(i_from_r),
      static_cast<double>(i_cal), static_cast<double>(p_cal), static_cast<unsigned>(cur),
      static_cast<unsigned>(pwr));
}
