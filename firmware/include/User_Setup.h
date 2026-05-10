// CYD ESP32-2432S028R ILI9341 — merged from ggaljoen TFT_eSPI Setup303 + witnessmenow HSPI pins.
// https://github.com/ggaljoen/TFT_eSPI/blob/master/User_Setups/Setup303_CYD_ESP32-2432S028R.h
//
// Display-only: no TOUCH_* here — sharing HSPI with XPT2046 init sometimes breaks first paint.

#pragma once

#define ILI9341_2_DRIVER

#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define TFT_INVERSION_ON

#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1

#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 1600000
