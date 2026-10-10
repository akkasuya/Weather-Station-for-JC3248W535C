// JC3248W535C Weather Station — LVGL v8 port (landscape 480x320)
// Port of ../weather_station_1 (Arduino_GFX direct-draw) to LVGL,
// reusing proven display/touch bring-up from ../netradio.
// Same data sources: Open-Meteo (no key) default, OWM if key set,
// IP geolocation fallback, NTP clock, NVS settings.
// Tabs (lv_tabview, swipe left/right): Now | 3-Day | 24H | Setup.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <lvgl.h>
#include "config.h"

// Weather Icons font (Erik Flowers, from original
// nicholaswilde/cyd-weather-station assets/fonts/weathericons-regular-webfont.ttf)
LV_FONT_DECLARE(weather_icons_48);
LV_FONT_DECLARE(weather_icons_24);

// Theme-driven neutrals (accents identical across flavors, like original).
// Mocha/Macchiato/Frappe = dark, Latte = light.
struct Theme { const char *name; uint32_t crust, mantle, base, text, sub; };
static const Theme THEMES[] = {
  {"Mocha",     0x11111B, 0x181825, 0x1E1E2E, 0xCDD6F4, 0xBAC2DE},
  {"Macchiato", 0x181926, 0x1E2030, 0x24273A, 0xCAD3F5, 0xB8C0E0},
  {"Frappe",    0x232634, 0x292C3C, 0x303446, 0xC6D0F5, 0xB8C0E0},
  {"Latte",     0xDCE0E8, 0xE6E9EF, 0xEFF1F5, 0x4C4F69, 0x6C6F85},
};
static int themeIdx = 0;
static uint32_t C_CRUST, C_MANTLE, C_BASEC, C_TEXT, C_SUB;
static void applyThemeVars() {
  themeIdx = constrain(themeIdx, 0, (int)(sizeof(THEMES) / sizeof(THEMES[0])) - 1);
  C_CRUST = THEMES[themeIdx].crust;
  C_MANTLE = THEMES[themeIdx].mantle;
  C_BASEC = THEMES[themeIdx].base;
  C_TEXT = THEMES[themeIdx].text;
  C_SUB = THEMES[themeIdx].sub;
}

// ---------- Catppuccin flavors (theme selectable in Setup) ----------
#define C_BLUE   0x89B4FA
#define C_GREEN  0xA6E3A1
#define C_RED    0xF38BA8
#define C_YELLOW 0xF9E2AF
#define C_MAUVE  0xCBA6F7
#define C_SKY    0x89DCEB
#define C_PEACH  0xFAB387
#define C_GREY   0x45475A

// ---------- display (same as weather_station_1 / netradio) ----------
class AXS15231B_Bulk : public Arduino_AXS15231B {
public:
  using Arduino_AXS15231B::Arduino_AXS15231B;
  void draw16bitRGBBitmap(int16_t x, int16_t y,
                          uint16_t *bitmap, int16_t w, int16_t h) override {
    startWrite();
    writeAddrWindow(x, y, w, h);
    writePixels(bitmap, (uint32_t)w * h);
    endWrite();
  }
  void draw16bitRGBBitmap(int16_t x, int16_t y,
                          const uint16_t bitmap[], int16_t w, int16_t h) override {
    startWrite();
    writeAddrWindow(x, y, w, h);
    writePixels(const_cast<uint16_t *>(bitmap), (uint32_t)w * h);
    endWrite();
  }
};

static Arduino_DataBus *gfx_bus =
    new Arduino_ESP32QSPI(LCD_QSPI_CS, LCD_QSPI_SCK,
                          LCD_QSPI_D0, LCD_QSPI_D1,
                          LCD_QSPI_D2, LCD_QSPI_D3);
static AXS15231B_Bulk *gfx_panel =
    new AXS15231B_Bulk(gfx_bus, GFX_NOT_DEFINED, 0, false, 320, 480);
static Arduino_Canvas *gfx =
    new Arduino_Canvas(320, 480, gfx_panel, 0, 0, 0);

// ---------- LVGL plumbing (same pattern as netradio) ----------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lv_buf = nullptr;
static volatile bool gfx_dirty = false;

static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  // Compose tile into landscape Canvas; full-frame push happens in loop().
  // This controller ignores CASET/RASET windows, so partial pushes ghost —
  // always push full frames via gfx_flush() coalesced at ~25fps.
  gfx->draw16bitRGBBitmap(area->x1, area->y1,
                          (uint16_t *)&color_p->full, (int16_t)w, (int16_t)h);
  gfx_dirty = true;
  lv_disp_flush_ready(disp);
}

static int touch_x = 0, touch_y = 0;
static bool touch_pressed = false;

static bool touch_get_raw(uint16_t &x, uint16_t &y) {
  uint8_t data[8] = {0};
  const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00,
                           0x00, 0x08, 0x00, 0x00, 0x00};
  Wire.beginTransmission(TP_ADDR);
  Wire.write(cmd, 11);
  if (Wire.endTransmission() != 0) return false;
  delay(1);
  if (Wire.requestFrom((int)TP_ADDR, 8) != 8) return false;
  for (int i = 0; i < 8; i++) data[i] = Wire.read();
  if (data[1] > 0 && data[1] <= 1) {
    uint16_t rawX = ((data[2] & 0x0F) << 8) | data[3];
    uint16_t rawY = ((data[4] & 0x0F) << 8) | data[5];
    if (rawX == 273 && rawY == 273) return false;
    if (rawX > TP_W || rawY > TP_H) return false;
    x = rawY;
    y = TP_W - rawX;
    if (x >= TFT_W || y >= TFT_H) return false;
    return true;
  }
  return false;
}

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  uint16_t x, y;
  if (touch_get_raw(x, y)) {
    touch_x = x; touch_y = y; touch_pressed = true;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touch_x;
    data->point.y = touch_y;
  } else {
    touch_pressed = false;
    data->state = LV_INDEV_STATE_REL;
  }
}

static esp_timer_handle_t lvgl_tick_timer = nullptr;
static void IRAM_ATTR lvgl_tick_cb(void *) { lv_tick_inc(1); }

// ---------- state (same as weather_station_1) ----------
static Preferences prefs;
static bool useMetric = DEFAULT_UNIT_METRIC;
static int brightness = DEFAULT_BRIGHTNESS;
static int updateMinutes = DEFAULT_UPDATE_MINUTES;
static String lat = DEFAULT_LAT, lon = DEFAULT_LON, city = DEFAULT_CITY;
static String owmKey = OWM_API_KEY;

struct CurrentWx {
  float temp = 0, feels = 0, humidity = 0, windSpeed = 0;
  int windDir = 0, code = -1;
  String status = "--";
  bool valid = false;
};
struct DayWx {
  String day = "--";
  float tmax = 0, tmin = 0;
  int code = -1;
  String status = "--";
};
struct HourWx { float temp = 0; int precip = 0; };

static CurrentWx cur;
static DayWx days[4];
static int nDays = 0;
static HourWx hours[24];
static int nHours = 0;
static String lastUpdate = "--:--";
static bool wifiOk = false;
static bool ntpOk = false;
static String statusMsg = "boot...";
static unsigned long lastFetch = 0;
static bool forceFetch = true;

// ---------- LVGL widgets ----------
static lv_obj_t *lbl_clock, *lbl_wifi, *lbl_footer, *lbl_date;
static lv_obj_t *lbl_now_temp, *lbl_now_cond, *lbl_now_feel;
static lv_obj_t *lbl_m_hum, *lbl_m_wind, *lbl_m_dir, *lbl_m_precip;
static lv_obj_t *lbl_icon_big;
static lv_obj_t *day_title[3], *day_icon[3], *day_cond[3], *day_hl[3];
static lv_obj_t *lbl_day_extra;
static lv_obj_t *chart_temp, *chart_rain;
static lv_chart_series_t *ser_temp, *ser_rain;
static lv_obj_t *lbl_hour_range;
static lv_obj_t *sw_units, *slider_bright, *lbl_bright_val;
static lv_obj_t *dd_interval, *dd_theme, *lbl_net, *lbl_setup_status;
static lv_obj_t *tv = nullptr;
static bool themeRebuild = false;

static const char *wmoDesc(int code) {
  if (code == 0) return "Clear sky";
  if (code == 1) return "Mainly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Foggy";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code == 61 || code == 63 || code == 65 || code == 66 || code == 67 ||
      code == 80 || code == 81 || code == 82) return "Rain";
  if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86) return "Snow";
  if (code >= 95) return "Thunderstorm";
  if (code == -2) return "Cloudy";
  return "Unknown";
}

// WMO weather-code -> Weather Icons glyph (copied from original
// nicholaswilde/cyd-weather-station src/ui.cpp getIconGlyph).
// Font: weather_icons_48 / weather_icons_24 (must set style font on the label).
static const char *wmoGlyph(int code) {
  switch (code) {
    case 0:
    case 1:  return "\xef\x80\x8d"; // wi-day-sunny (f00d)
    case 2:
    case 3:  return "\xef\x80\x82"; // wi-cloudy (f002)
    case 45:
    case 48: return "\xef\x80\x83"; // wi-fog (f003)
    case 51:
    case 53:
    case 55:
    case 61:
    case 63:
    case 65:
    case 80:
    case 81:
    case 82:  return "\xef\x80\x88"; // wi-rain (f008)
    case 56:
    case 57:
    case 66:
    case 67:
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:  return "\xef\x80\x8a"; // wi-snow (f00a)
    case 95:
    case 96:
    case 99:  return "\xef\x80\x90"; // wi-thunderstorm (f010)
    case -2:  return "\xef\x80\x82"; // OWM path -> generic cloudy (f002)
    default:  return "\xef\x81\xbb"; // wi-na (f07b)
  }
}

static uint32_t wmoColor(int code) {
  if (code == 0 || code == 1) return C_YELLOW;
  if (code == 2 || code == 3 || code == 45 || code == 48) return C_MAUVE;
  if (code >= 51 && code <= 57) return C_SKY;
  if (code >= 61 && code <= 82) return C_BLUE;
  if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86) return C_MAUVE;
  if (code >= 95) return C_MAUVE;
  if (code == -2) return C_MAUVE;
  return C_GREY;
}

static const char *windCompass(int deg) {
  static const char *names[] = {"N","NE","E","SE","S","SW","W","NW"};
  int d = ((deg % 360) + 360) % 360;
  return names[((d + 22) / 45) % 8];
}

static String clockStr() {
  struct tm t;
  if (getLocalTime(&t, 5)) {
    char b[6];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    return String(b);
  }
  return "--:--";
}

static void applyBrightness() {
  analogWrite(TFT_BL, map(constrain(brightness, 0, 100), 0, 100, 0, 255));
}

// ---------- network (same logic as weather_station_1) ----------
static bool wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) Serial.printf("[WiFi] IP %s RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else Serial.println("[WiFi] FAILED");
  return wifiOk;
}

static void ntpInit() {
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  setenv("TZ", TZ_STRING, 1);
  tzset();
  struct tm t;
  ntpOk = getLocalTime(&t, 8000);
  Serial.printf("[NTP] %s\n", ntpOk ? "synced" : "FAILED");
}

static bool fetchIpLocation() {
  if (lat != "" && lon != "") return true;
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin("http://ip-api.com/json/")) return false;
  int rc = http.GET();
  if (rc != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  if (String(doc["status"] | "") != "success") return false;
  lat = String((double)doc["lat"], 4);
  lon = String((double)doc["lon"], 4);
  city = String(doc["city"] | "Unknown");
  Serial.printf("[GEO] ip-api %s,%s %s\n", lat.c_str(), lon.c_str(), city.c_str());
  return true;
}

static bool fetchOpenMeteo() {
  String tu = useMetric ? "celsius" : "fahrenheit";
  String wu = useMetric ? "kmh" : "mph";
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + lat +
               "&longitude=" + lon +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m"
               "&hourly=temperature_2m,precipitation_probability,weather_code"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min"
               "&forecast_days=4&timezone=auto"
               "&temperature_unit=" + tu + "&windspeed_unit=" + wu;
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(15000);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(cli, url)) return false;
  int rc = http.GET();
  if (rc != 200) { Serial.printf("[WX] open-meteo http %d\n", rc); http.end(); return false; }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) { Serial.printf("[WX] json %s\n", err.c_str()); return false; }
  JsonObject c = doc["current"];
  cur.temp = c["temperature_2m"] | 0.0f;
  cur.feels = c["apparent_temperature"] | cur.temp;
  cur.humidity = c["relative_humidity_2m"] | 0;
  cur.code = c["weather_code"] | -1;
  cur.windSpeed = c["wind_speed_10m"] | 0.0f;
  cur.windDir = c["wind_direction_10m"] | 0;
  cur.status = wmoDesc(cur.code);
  cur.valid = true;

  nHours = 0;
  JsonArray ht = doc["hourly"]["temperature_2m"];
  JsonArray hp = doc["hourly"]["precipitation_probability"];
  JsonArray htime = doc["hourly"]["time"];
  String nowIso = String(c["time"] | "");
  int startIdx = 0;
  if (nowIso != "" && htime.size() > 0) {
    String nowHr = nowIso.substring(0, 13);
    for (size_t i = 0; i < htime.size(); i++) {
      String h = String(htime[i] | "");
      if (h.startsWith(nowHr)) { startIdx = i; break; }
    }
  }
  for (int i = 0; i < 24; i++) {
    size_t idx = startIdx + i;
    float t = (idx < ht.size()) ? (float)(ht[idx] | 0.0f) : 0;
    int p = (idx < hp.size() && !hp[idx].isNull()) ? (int)(hp[idx] | 0) : 0;
    hours[i] = {(float)t, p};
  }
  nHours = 24;

  static const char *fallbackLbl[4] = {"Today", "Tmrw", "", ""};
  JsonArray dmax = doc["daily"]["temperature_2m_max"];
  JsonArray dmin = doc["daily"]["temperature_2m_min"];
  JsonArray dcode = doc["daily"]["weather_code"];
  JsonArray dtime = doc["daily"]["time"];
  nDays = 0;
  for (int i = 0; i < 4 && i < (int)dmax.size(); i++) {
    int code = (i < (int)dcode.size()) ? (int)(dcode[i] | -1) : -1;
    String dl = String((i < (int)dtime.size()) ? (dtime[i] | "") : "");
    String lbl = fallbackLbl[i];
    if (i >= 2 && dl.length() >= 10) {
      int y = dl.substring(0, 4).toInt(), m = dl.substring(5, 7).toInt(), d = dl.substring(8, 10).toInt();
      static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
      int yy = y; if (m < 3) yy--;
      int wd = (yy + yy/4 - yy/100 + yy/400 + t[m-1] + d) % 7;
      static const char *wn[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
      lbl = wn[wd];
    }
    days[i] = {lbl, (float)(dmax[i] | 0.0f), (float)(dmin[i] | 0.0f), code, String(wmoDesc(code))};
    nDays++;
  }
  lastUpdate = clockStr();
  Serial.printf("[WX] ok temp=%.1f code=%d days=%d\n", cur.temp, cur.code, nDays);
  return true;
}

static bool fetchOwm() {
  String units = useMetric ? "metric" : "imperial";
  String url = "https://api.openweathermap.org/data/2.5/weather?lat=" + lat +
               "&lon=" + lon + "&appid=" + owmKey + "&units=" + units;
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(cli, url)) return false;
  int rc = http.GET();
  if (rc != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  cur.temp = doc["main"]["temp"] | 0.0f;
  cur.feels = doc["main"]["feels_like"] | cur.temp;
  cur.humidity = doc["main"]["humidity"] | 0;
  cur.windSpeed = doc["wind"]["speed"] | 0.0f;
  cur.windDir = doc["wind"]["deg"] | 0;
  cur.status = String(doc["weather"][0]["description"] | "--");
  cur.code = -2;
  cur.valid = true;
  nHours = 0; nDays = 0;
  city = String(doc["name"] | city);
  lastUpdate = clockStr();
  return true;
}

// ---------- UI refresh ----------
static void refresh_header_footer() {
  lv_label_set_text(lbl_clock, clockStr().c_str());
  struct tm t;
  if (getLocalTime(&t, 5)) {
    char d[32];
    strftime(d, sizeof(d), "%a %d %b %Y", &t);
    lv_label_set_text(lbl_date, d);
  } else {
    lv_label_set_text(lbl_date, "--");
  }
  lv_label_set_text(lbl_wifi, wifiOk ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(wifiOk ? C_GREEN : C_RED), 0);
  String f = "Last: " + lastUpdate + "  |  " + city + "  " + FW_VERSION;
  if (!cur.valid) f = statusMsg + "  |  " + city;
  lv_label_set_text(lbl_footer, f.c_str());
}

static void refresh_now() {
  if (!cur.valid) {
    lv_label_set_text(lbl_now_temp, "--");
    lv_label_set_text(lbl_now_cond, statusMsg.c_str());
    lv_label_set_text(lbl_now_feel, "waiting for network...");
    return;
  }
  String tu = useMetric ? "C" : "F";
  String su = useMetric ? "km/h" : "mph";
  char b[32];
  snprintf(b, sizeof(b), "%.0f\xC2\xB0%s", cur.temp, tu.c_str());
  lv_label_set_text(lbl_now_temp, b);
  lv_label_set_text(lbl_now_cond, cur.status.c_str());
  lv_obj_set_style_text_color(lbl_now_cond, lv_color_hex(wmoColor(cur.code)), 0);
  lv_label_set_text(lbl_icon_big, wmoGlyph(cur.code));
  lv_obj_set_style_text_color(lbl_icon_big, lv_color_hex(wmoColor(cur.code)), 0);
  snprintf(b, sizeof(b), "Feels %.0f\xC2\xB0%s", cur.feels, tu.c_str());
  lv_label_set_text(lbl_now_feel, b);
  snprintf(b, sizeof(b), "%.0f %%", cur.humidity);
  lv_label_set_text(lbl_m_hum, b);
  snprintf(b, sizeof(b), "%.0f %s", cur.windSpeed, su.c_str());
  lv_label_set_text(lbl_m_wind, b);
  snprintf(b, sizeof(b), "%s %d\xC2\xB0", windCompass(cur.windDir), cur.windDir);
  lv_label_set_text(lbl_m_dir, b);
  int p0 = (nHours > 0) ? hours[0].precip : 0;
  snprintf(b, sizeof(b), "%d %%", p0);
  lv_label_set_text(lbl_m_precip, b);
}

static void refresh_forecast() {
  for (int i = 0; i < 3; i++) {
    if (i < nDays) {
      lv_label_set_text(day_title[i], days[i].day.c_str());
      lv_label_set_text(day_icon[i], wmoGlyph(days[i].code));
      lv_obj_set_style_text_color(day_icon[i], lv_color_hex(wmoColor(days[i].code)), 0);
      lv_label_set_text(day_cond[i], days[i].status.c_str());
      char b[32];
      snprintf(b, sizeof(b), "H %.0f\xC2\xB0  L %.0f\xC2\xB0", days[i].tmax, days[i].tmin);
      lv_label_set_text(day_hl[i], b);
    } else {
      lv_label_set_text(day_title[i], "--");
      lv_label_set_text(day_icon[i], wmoGlyph(-99));
      lv_label_set_text(day_cond[i], statusMsg.c_str());
      lv_label_set_text(day_hl[i], "");
    }
  }
  if (nDays >= 4) {
    char b[64];
    snprintf(b, sizeof(b), "+ %s: H %.0f / L %.0f", days[3].day.c_str(), days[3].tmax, days[3].tmin);
    lv_label_set_text(lbl_day_extra, b);
  } else lv_label_set_text(lbl_day_extra, "");
}

static void refresh_hourly() {
  if (nHours == 0) {
    lv_label_set_text(lbl_hour_range, statusMsg.c_str());
    return;
  }
  float tmin = hours[0].temp, tmax = hours[0].temp;
  for (int i = 0; i < nHours; i++) { tmin = min(tmin, hours[i].temp); tmax = max(tmax, hours[i].temp); }
  if (tmax - tmin < 2) { tmax += 1; tmin -= 1; }
  lv_chart_set_range(chart_temp, LV_CHART_AXIS_PRIMARY_Y, (int16_t)(tmin * 10), (int16_t)(tmax * 10));
  lv_chart_set_range(chart_rain, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  // refill series
  for (int i = 0; i < nHours; i++) {
    ser_temp->y_points[i] = (lv_coord_t)(hours[i].temp * 10);
    ser_rain->y_points[i] = (lv_coord_t)hours[i].precip;
  }
  lv_chart_refresh(chart_temp);
  lv_chart_refresh(chart_rain);
  String tu = useMetric ? "C" : "F";
  char b[48];
  snprintf(b, sizeof(b), "H %.0f\xC2\xB0%s  L %.0f\xC2\xB0%s   Now +4h +8h +12h +16h +20h",
           tmax, tu.c_str(), tmin, tu.c_str());
  lv_label_set_text(lbl_hour_range, b);
}

static void refresh_all() {
  refresh_header_footer();
  refresh_now();
  refresh_forecast();
  refresh_hourly();
  if (WiFi.status() == WL_CONNECTED)
    lv_label_set_text_fmt(lbl_net, "%s  %ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    lv_label_set_text(lbl_net, "offline");
  lv_label_set_text(lbl_setup_status, statusMsg.c_str());
}

static void doFetch() {
  if (!wifiConnect()) { statusMsg = "WiFi failed - retrying"; refresh_all(); return; }
  if (!ntpOk) ntpInit();
  statusMsg = "fetching weather..."; refresh_all();
  lv_timer_handler();
  if (!fetchIpLocation()) {
    if (lat == "" || lon == "") { statusMsg = "no location"; refresh_all(); return; }
  }
  bool ok = (owmKey != "") ? fetchOwm() : fetchOpenMeteo();
  statusMsg = ok ? "updated " + lastUpdate : "fetch failed - retrying";
  if (ok) lastFetch = millis();
  refresh_all();
}

// ---------- settings callbacks ----------
static void cb_units(lv_event_t *e) {
  bool on = lv_obj_has_state(sw_units, LV_STATE_CHECKED);
  if (on != useMetric) {
    useMetric = on;
    prefs.putBool("metric", useMetric);
    forceFetch = true;
  }
}
static void cb_bright(lv_event_t *e) {
  brightness = lv_slider_get_value(slider_bright);
  prefs.putInt("bright", brightness);
  applyBrightness();
  char b[16]; snprintf(b, sizeof(b), "%d %%", brightness);
  lv_label_set_text(lbl_bright_val, b);
}
static void cb_interval(lv_event_t *e) {
  uint16_t sel = lv_dropdown_get_selected(dd_interval);
  int vals[4] = {5, 15, 30, 60};
  updateMinutes = vals[sel];
  prefs.putInt("updMin", updateMinutes);
}
static void cb_refresh(lv_event_t *e) { forceFetch = true; }
static void cb_theme(lv_event_t *e) {
  int sel = (int)lv_dropdown_get_selected(dd_theme);
  if (sel != themeIdx) {
    themeIdx = sel;
    prefs.putInt("theme", themeIdx);
    themeRebuild = true;
  }
}
static void build_ui(); // fwd
static void rebuild_ui() {
  applyThemeVars();
  lv_obj_clean(lv_scr_act());
  build_ui();
  lv_tabview_set_act(tv, 3, LV_ANIM_OFF);
  refresh_header_footer();
  refresh_all();
}

// ---------- UI construction ----------
static lv_obj_t *card(lv_obj_t *parent, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_BASEC), 0);
  lv_obj_set_style_border_color(c, lv_color_hex(C_GREY), 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_radius(c, 8, 0);
  lv_obj_set_style_pad_all(c, 6, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static lv_obj_t *mklabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_CRUST), 0);
  lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);

  // header 36px
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_set_size(hdr, TFT_W, 36);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(C_MANTLE), 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 4, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = mklabel(hdr, "Chiang Mai WX", &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_pos(t, 6, 6);
  lbl_wifi = mklabel(hdr, LV_SYMBOL_WIFI, &lv_font_montserrat_14, C_GREEN);
  lv_obj_set_pos(lbl_wifi, TFT_W - 106, 6);
  lbl_clock = mklabel(hdr, "--:--", &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_pos(lbl_clock, TFT_W - 80, 6);
  lbl_date = mklabel(hdr, "--", &lv_font_montserrat_14, C_SUB);
  lv_obj_set_width(lbl_date, 220);
  lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 1);

  // tabview below header
  tv = lv_tabview_create(scr, LV_DIR_TOP, 32);
  lv_obj_set_size(tv, TFT_W, TFT_H - 36 - 20);
  lv_obj_set_pos(tv, 0, 36);
  lv_obj_set_style_bg_color(tv, lv_color_hex(C_CRUST), 0);
  lv_obj_t *tab_now = lv_tabview_add_tab(tv, "Now");
  lv_obj_t *tab_fc = lv_tabview_add_tab(tv, "3-Day");
  lv_obj_t *tab_hr = lv_tabview_add_tab(tv, "24H");
  lv_obj_t *tab_se = lv_tabview_add_tab(tv, "Setup");
  for (lv_obj_t **tp : {&tab_now, &tab_fc, &tab_hr, &tab_se}) {
    lv_obj_set_style_bg_color(*tp, lv_color_hex(C_CRUST), 0);
    lv_obj_set_style_pad_all(*tp, 6, 0);
  }

  // ---- Now tab ----
  lbl_now_temp = mklabel(tab_now, "--", &lv_font_montserrat_48, C_TEXT);
  lv_obj_set_pos(lbl_now_temp, 56, 0);
  lbl_icon_big = mklabel(tab_now, "?", &weather_icons_48, C_YELLOW);
  lv_obj_set_pos(lbl_icon_big, 338, 0);
  lbl_now_cond = mklabel(tab_now, "--", &lv_font_montserrat_20, C_YELLOW);
  lv_obj_set_pos(lbl_now_cond, 56, 58);
  lbl_now_feel = mklabel(tab_now, "--", &lv_font_montserrat_14, C_SUB);
  lv_obj_set_pos(lbl_now_feel, 56, 84);
  const char *mtitle[4] = {"HUMIDITY", "WIND", "WIND DIR", "PRECIP 1H"};
  lv_obj_t **mval[4] = {&lbl_m_hum, &lbl_m_wind, &lbl_m_dir, &lbl_m_precip};
  uint32_t mcol[4] = {C_SKY, C_GREEN, C_PEACH, C_BLUE};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = card(tab_now, 218, 56);
    lv_obj_set_pos(c, 8 + (i % 2) * 230, 108 + (i / 2) * 62);
    lv_obj_t *mt = mklabel(c, mtitle[i], &lv_font_montserrat_12, C_SUB);
    lv_obj_set_width(mt, 206);
    lv_obj_set_style_text_align(mt, LV_TEXT_ALIGN_CENTER, 0);
    *mval[i] = mklabel(c, "--", &lv_font_montserrat_20, mcol[i]);
    lv_obj_set_width(*mval[i], 206);
    lv_obj_set_style_text_align(*mval[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(*mval[i], 0, 20);
  }

  // ---- Forecast tab ----
  for (int i = 0; i < 3; i++) {
    lv_obj_t *c = card(tab_fc, 144, 178);
    lv_obj_set_pos(c, 8 + i * 154, 4);
    day_title[i] = mklabel(c, "--", &lv_font_montserrat_20, C_TEXT);
    lv_obj_set_width(day_title[i], 132);
    lv_obj_set_style_text_align(day_title[i], LV_TEXT_ALIGN_CENTER, 0);
    day_icon[i] = mklabel(c, "?", &weather_icons_48, C_SUB);
    lv_obj_set_width(day_icon[i], 132);
    lv_obj_set_style_text_align(day_icon[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(day_icon[i], 0, 24);
    day_cond[i] = mklabel(c, "--", &lv_font_montserrat_12, C_SUB);
    lv_obj_set_width(day_cond[i], 132);
    lv_obj_set_style_text_align(day_cond[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(day_cond[i], 0, 0);
    lv_label_set_long_mode(day_cond[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(day_cond[i], 0, 98);
    day_hl[i] = mklabel(c, "--", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(day_hl[i], 132);
    lv_obj_set_style_text_align(day_hl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(day_hl[i], 0, 132);
  }
  lbl_day_extra = mklabel(tab_fc, "", &lv_font_montserrat_12, C_SUB);
  lv_obj_set_pos(lbl_day_extra, 8, 188);

  // ---- Hourly tab ----
  mklabel(tab_hr, "Next 24h  temp (line) + rain % (bars)", &lv_font_montserrat_12, C_SUB);
  chart_temp = lv_chart_create(tab_hr);
  lv_obj_set_size(chart_temp, 452, 130);
  lv_obj_set_pos(chart_temp, 0, 20);
  lv_chart_set_type(chart_temp, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart_temp, 24);
  lv_chart_set_div_line_count(chart_temp, 4, 6);
  lv_obj_set_style_bg_color(chart_temp, lv_color_hex(C_BASEC), 0);
  ser_temp = lv_chart_add_series(chart_temp, lv_color_hex(C_YELLOW), LV_CHART_AXIS_PRIMARY_Y);
  chart_rain = lv_chart_create(tab_hr);
  lv_obj_set_size(chart_rain, 452, 130);
  lv_obj_set_pos(chart_rain, 0, 20);
  lv_chart_set_type(chart_rain, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(chart_rain, 24);
  lv_obj_set_style_bg_opa(chart_rain, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(chart_rain, LV_OPA_TRANSP, 0);
  lv_obj_set_style_line_opa(chart_rain, LV_OPA_TRANSP, 0);
  ser_rain = lv_chart_add_series(chart_rain, lv_color_hex(C_BLUE), LV_CHART_AXIS_PRIMARY_Y);
  lbl_hour_range = mklabel(tab_hr, "--", &lv_font_montserrat_12, C_SUB);
  lv_obj_set_pos(lbl_hour_range, 0, 156);
  mklabel(tab_hr, "Now +4h +8h +12h +16h +20h", &lv_font_montserrat_12, C_GREY);

  // ---- Setup tab ----
  int sy = 4;
  lv_obj_t *row = card(tab_se, 452, 40);
  lv_obj_set_pos(row, 0, sy);
  mklabel(row, "Metric (C/km/h)", &lv_font_montserrat_14, C_TEXT);
  sw_units = lv_switch_create(row);
  lv_obj_set_pos(sw_units, 380, 2);
  lv_obj_add_event_cb(sw_units, cb_units, LV_EVENT_VALUE_CHANGED, NULL);
  sy += 46;
  row = card(tab_se, 452, 48);
  lv_obj_set_pos(row, 0, sy);
  mklabel(row, "Brightness", &lv_font_montserrat_14, C_TEXT);
  slider_bright = lv_slider_create(row);
  lv_obj_set_size(slider_bright, 220, 12);
  lv_obj_set_pos(slider_bright, 130, 8);
  lv_slider_set_range(slider_bright, 10, 100);
  lv_obj_add_event_cb(slider_bright, cb_bright, LV_EVENT_VALUE_CHANGED, NULL);
  lbl_bright_val = mklabel(row, "80 %", &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_pos(lbl_bright_val, 360, 4);
  sy += 54;
  row = card(tab_se, 452, 40);
  lv_obj_set_pos(row, 0, sy);
  mklabel(row, "Interval", &lv_font_montserrat_14, C_TEXT);
  dd_interval = lv_dropdown_create(row);
  lv_dropdown_set_options(dd_interval, "5 min\n15 min\n30 min\n60 min");
  lv_obj_set_size(dd_interval, 140, 30);
  lv_obj_set_pos(dd_interval, 300, 0);
  lv_obj_add_event_cb(dd_interval, cb_interval, LV_EVENT_VALUE_CHANGED, NULL);
  sy += 46;
  row = card(tab_se, 452, 40);
  lv_obj_set_pos(row, 0, sy);
  mklabel(row, "Theme", &lv_font_montserrat_14, C_TEXT);
  dd_theme = lv_dropdown_create(row);
  lv_dropdown_set_options(dd_theme, "Mocha\nMacchiato\nFrappe\nLatte");
  lv_obj_set_size(dd_theme, 160, 30);
  lv_obj_set_pos(dd_theme, 280, 0);
  lv_dropdown_set_selected(dd_theme, themeIdx);
  lv_obj_add_event_cb(dd_theme, cb_theme, LV_EVENT_VALUE_CHANGED, NULL);
  sy += 46;
  lv_obj_t *btn = lv_btn_create(tab_se);
  lv_obj_set_size(btn, 452, 36);
  lv_obj_set_pos(btn, 0, sy);
  lv_obj_set_style_bg_color(btn, lv_color_hex(C_GREEN), 0);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "Refresh now");
  lv_obj_set_style_text_color(bl, lv_color_hex(0x000000), 0);
  lv_obj_center(bl);
  lv_obj_add_event_cb(btn, cb_refresh, LV_EVENT_CLICKED, NULL);
  sy += 42;
  lbl_net = mklabel(tab_se, "--", &lv_font_montserrat_12, C_SUB);
  lv_obj_set_pos(lbl_net, 4, sy);
  lbl_setup_status = mklabel(tab_se, "boot...", &lv_font_montserrat_12, C_YELLOW);
  lv_obj_set_pos(lbl_setup_status, 4, sy + 16);

  // footer 20px
  lv_obj_t *ftr = lv_obj_create(scr);
  lv_obj_set_size(ftr, TFT_W, 20);
  lv_obj_set_pos(ftr, 0, TFT_H - 20);
  lv_obj_set_style_bg_color(ftr, lv_color_hex(C_MANTLE), 0);
  lv_obj_set_style_border_width(ftr, 0, 0);
  lv_obj_set_style_radius(ftr, 0, 0);
  lv_obj_set_style_pad_all(ftr, 2, 0);
  lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
  lbl_footer = mklabel(ftr, "boot...", &lv_font_montserrat_12, C_SUB);

  // init control states
  if (useMetric) lv_obj_add_state(sw_units, LV_STATE_CHECKED);
  lv_slider_set_value(slider_bright, brightness, LV_ANIM_OFF);
  int idx = 1;
  if (updateMinutes == 5) idx = 0; else if (updateMinutes == 15) idx = 1;
  else if (updateMinutes == 30) idx = 2; else idx = 3;
  lv_dropdown_set_selected(dd_interval, idx);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[WX] JC3248W535C LVGL weather station %s\n", FW_VERSION);

  prefs.begin("wx", false);
  useMetric = prefs.getBool("metric", DEFAULT_UNIT_METRIC);
  brightness = prefs.getInt("bright", DEFAULT_BRIGHTNESS);
  updateMinutes = prefs.getInt("updMin", DEFAULT_UPDATE_MINUTES);
  themeIdx = prefs.getInt("theme", 0);
  applyThemeVars();

  gfx->begin();
  gfx->setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  applyBrightness();
  gfx->fillScreen(0x0000);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(120, 140);
  gfx->print("JC3248W535C Weather LVGL");
  gfx->flush();

  Wire.begin(4, 8);
  Wire.setClock(400000);
  pinMode(TP_INT, INPUT_PULLUP);
  delay(200);

  lv_init();
  const esp_timer_create_args_t tick_args = {
    .callback = lvgl_tick_cb, .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK, .name = "lvgl_tick"
  };
  esp_timer_create(&tick_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, 1000);

  uint32_t buf_rows = 80;
  lv_buf = (lv_color_t *)heap_caps_malloc(TFT_W * buf_rows * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!lv_buf) {
    buf_rows = 10;
    lv_buf = (lv_color_t *)heap_caps_malloc(TFT_W * buf_rows * sizeof(lv_color_t), MALLOC_CAP_DEFAULT);
  }
  lv_disp_draw_buf_init(&draw_buf, lv_buf, NULL, TFT_W * buf_rows);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = TFT_W;
  disp_drv.ver_res = TFT_H;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  build_ui();
  lv_timer_handler();
  gfx->flush();

  wifiConnect();
  ntpInit();
  doFetch();
  forceFetch = false;
  refresh_all();
}

void loop() {
  if (themeRebuild) {
    themeRebuild = false;
    rebuild_ui();
  }
  lv_timer_handler();
  static uint32_t last_flush = 0;
  if (gfx_dirty && (millis() - last_flush >= 40)) {
    gfx->flush();
    gfx_dirty = false;
    last_flush = millis();
  }

  static String lastClock = "";
  static unsigned long lastRetry = 0;
  String c = clockStr();
  if (c != lastClock) { lastClock = c; refresh_header_footer(); }

  unsigned long intervalMs = (unsigned long)updateMinutes * 60000UL;
  if (forceFetch || (cur.valid && millis() - lastFetch > intervalMs)) {
    forceFetch = false;
    doFetch();
  }
  if (!wifiOk && millis() - lastRetry > 30000) {
    lastRetry = millis();
    doFetch();
  }
  delay(5);
}
