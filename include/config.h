#pragma once

// ================= Hardware (proven in ../testc + ../netradio) =================
// Display: AXS15231B QSPI 320x480 native -> landscape 480x320 (rotation 1)
#define LCD_QSPI_CS  45
#define LCD_QSPI_SCK 47
#define LCD_QSPI_D0  21
#define LCD_QSPI_D1  48
#define LCD_QSPI_D2  40
#define LCD_QSPI_D3  39
#define TFT_BL       1
#define TFT_W        480
#define TFT_H        320

// Touch: AXS @0x3B, SDA=4 SCL=8 INT=3, no RST
#define TP_ADDR 0x3B
#define TP_INT  3
#define TP_W    320
#define TP_H    480

// ================= WiFi / location defaults =================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// Start location: Chiang Mai. Leave both "" for IP-geolocation auto-detect.
#define DEFAULT_LAT "18.7883"
#define DEFAULT_LON "98.9853"
#define DEFAULT_CITY "Chiang Mai"

// Optional OpenWeatherMap key. Empty = use Open-Meteo (no key needed).
#define OWM_API_KEY ""

// metric = C + km/h | imperial = F + mph
#define DEFAULT_UNIT_METRIC true

// Asia/Bangkok (ICT-7, all Thailand), no DST.
#define TZ_STRING "ICT-7"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

#define DEFAULT_UPDATE_MINUTES 15
#define DEFAULT_BRIGHTNESS 80

#define FW_VERSION "v0.2.0-lvgl"
