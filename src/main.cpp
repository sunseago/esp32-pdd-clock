// ============================================================
//  PDD 桌面时钟 — Waveshare ESP32-S3-Touch-LCD-1.54
//  时钟 + 天气 + PDD 股价 + 四向姿态自动旋转（文字始终正向）
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include "esp_adc_cal.h"
#include <Arduino_GFX_Library.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <ArduinoJson.h>
#include "config.h"

// ---------------- 硬件引脚（板载，见 REQUIREMENTS 第 2 节） ----------------
#define PIN_LCD_DC    45
#define PIN_LCD_CS    21
#define PIN_LCD_SCK   38
#define PIN_LCD_MOSI  39
#define PIN_LCD_RST   40
#define PIN_LCD_BL    46
#define PIN_BAT_POWER 2    // 上电拉高保持供电，拉低=关机
#define PIN_BAT_ADC   1    // 3:1 分压
#define PIN_CHG       3    // PW4054：插 USB 充电 = HIGH（与常见相反）
#define PIN_PWR       5
#define PIN_PLUS      4
#define PIN_BOOT      0
#define PIN_IMU_SDA   42
#define PIN_IMU_SCL   41
#define QMI8658_ADDR  0x6B

#define SCR 240

// ---------------- 配色 ----------------
#define COL_BLACK   0x0000
static uint16_t COL_WHITE, COL_GRAY, COL_RED, COL_GREEN, COL_ORANGE;

// ---------------- 显示对象 ----------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK,
                                            PIN_LCD_MOSI, GFX_NOT_DEFINED, HSPI);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0 /*固定 rotation=0*/, true /*IPS*/,
                                      SCR, SCR, LCD_COL_OFFSET, LCD_ROW_OFFSET,
                                      LCD_COL_OFFSET, LCD_ROW_OFFSET);
U8G2_FOR_ADAFRUIT_GFX u8f;

// 1bpp 复用小画布（所有文字/图标先画到这里，离屏合成后一次性推屏）
GFXcanvas1 cnv(SCR, 48);            // 30 B/行 × 48 = 1440 B
static uint16_t boxBuf[SCR * 40];   // 离屏合成缓冲（最大盒子 240×40，RGB565）

#define FONT_CN u8g2_font_wqy14_t_gb2312

// ---------------- 全局状态 ----------------
uint8_t  g_heading = 0;             // 0..3
bool     g_imuOk = false;
bool     g_screenOff = false;
int      g_brightPct = BRIGHT_DEFAULT_PCT;   // 当前亮度 %（config.h 可配默认值）
const int BRIGHT_LEVELS[6] = {0, 20, 40, 60, 80, 100};

String   g_city = CITY_NAME;
double   g_lat = CITY_LAT, g_lon = CITY_LON;
bool     g_weatherOk = false;
float    g_temp = 0;
int      g_wcode = -1;
bool     g_isDay = true;

bool     g_stockOk = false;
float    g_price = 0, g_prev = 0, g_chg = 0, g_pct = 0;

int      g_battPct = 100;
bool     g_charging = false;

// ============================================================
//  坐标变换：逻辑坐标(用户视野) -> 物理像素（固定 rotation=0）
//  heading1: px=y, py=239-x ; heading2: px=239-x, py=239-y ;
//  heading3: px=239-y, py=x
// ============================================================
void rectLogical(int x, int y, int w, int h, uint16_t c) {
  switch (g_heading) {
    case 0:  gfx->fillRect(x, y, w, h, c); break;
    case 1:  gfx->fillRect(y, SCR - x - w, h, w, c); break;
    case 2:  gfx->fillRect(SCR - x - w, SCR - y - h, w, h, c); break;
    default: gfx->fillRect(SCR - y - h, x, h, w, c); break;
  }
}

// 把 cnv 左上角 w×h 区域与背景色离屏合成 16bpp 位图，按 heading 旋转后
// 一次性推送到屏幕 —— 无"先擦后画"的中间帧，从机制上消除闪烁
void pushBox(int lx, int ly, int w, int h, uint16_t fg, uint16_t bg) {
  if (g_screenOff) return;
  const uint8_t *src = cnv.getBuffer();
  const int sStride = (cnv.width() + 7) / 8;
  int dw = (g_heading == 0 || g_heading == 2) ? w : h;
  int dh = (g_heading == 0 || g_heading == 2) ? h : w;

  for (int sy = 0; sy < h; sy++) {
    for (int sx = 0; sx < w; sx++) {
      bool on = (src[sy * sStride + (sx >> 3)] >> (7 - (sx & 7))) & 1;
      int dx, dy;
      switch (g_heading) {
        case 0:  dx = sx;          dy = sy;          break;
        case 1:  dx = sy;          dy = w - 1 - sx;  break;
        case 2:  dx = w - 1 - sx;  dy = h - 1 - sy;  break;
        default: dx = h - 1 - sy;  dy = sx;          break;
      }
      boxBuf[dy * dw + dx] = on ? fg : bg;
    }
  }

  int px, py;
  switch (g_heading) {
    case 0:  px = lx;           py = ly;           break;
    case 1:  px = ly;           py = SCR - lx - w; break;
    case 2:  px = SCR - lx - w; py = SCR - ly - h; break;
    default: px = SCR - ly - h; py = lx;           break;
  }
  gfx->draw16bitRGBBitmap(px, py, boxBuf, dw, dh);
}

// ---------------- 文字绘制 ----------------
enum Align { AL_LEFT, AL_CENTER, AL_RIGHT };

void drawText(const char *s, const uint8_t *font, int x, int y, int w, int h,
              uint16_t color, Align al, uint16_t bg = COL_BLACK) {
  if (g_screenOff) return;
  if (w > SCR || h > 40) return;
  cnv.fillScreen(0);
  u8f.setFont(font);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);
  u8f.setForegroundColor(1);
  int tw = u8f.getUTF8Width(s);
  int tx = (al == AL_CENTER) ? (w - tw) / 2 : (al == AL_RIGHT) ? w - tw : 0;
  if (tx < 0) tx = 0;
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  int base = (h - (asc - desc)) / 2 + asc;     // 垂直居中基线
  u8f.setCursor(tx, base);
  u8f.print(s);
  pushBox(x, y, w, h, color, bg);              // 离屏合成，一次性推送，无闪烁
}

// ============================================================
//  QMI8658 六轴（仅加速度计）
// ============================================================
#define IMU_I2C_FREQ 100000   // 100kHz：比 400kHz 抗干扰强得多，姿态检测带宽足够

static void imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(QMI8658_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static bool imuRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(QMI8658_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)QMI8658_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}
bool imuInit() {
  uint8_t who = 0;
  if (!imuRead(0x00, &who, 1) || (who & 0x3F) != 0x05) return false; // WHO_AM_I
  imuWrite(0x02, 0x60);  // CTRL1: 地址自增
  imuWrite(0x03, 0x13);  // CTRL2: 加速度 ±4g
  imuWrite(0x04, 0x53);  // CTRL3: 陀螺仪
  imuWrite(0x06, 0x05);  // CTRL5: 低通
  imuWrite(0x08, 0x03);  // CTRL7: 使能 accel+gyro
  delay(20);
  return true;
}

// IMU 健康恢复：软复位传感器 + 重建 I2C 总线
// 触发条件见 updateHeading（连续读失败 / 数据冻结）
static void imuRecover(const char *why) {
  Serial.printf("[自检] IMU 异常(%s)，软复位并重建 I2C 总线\n", why);
  imuWrite(0x60, 0xB0);            // QMI8658 soft reset（总线半挂时允许失败）
  delay(15);
  Wire.end();
  delay(5);
  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL, IMU_I2C_FREQ);
  delay(5);
  g_imuOk = imuInit();
  Serial.printf("[自检] IMU 恢复%s\n", g_imuOk ? "成功" : "失败，将在下一轮重试");
}
bool imuReadAccel(int16_t &ax, int16_t &ay, int16_t &az) {
  uint8_t b[6];
  if (!imuRead(0x35, b, 6)) return false;
  ax = (int16_t)(b[1] << 8 | b[0]);
  ay = (int16_t)(b[3] << 8 | b[2]);
  az = (int16_t)(b[5] << 8 | b[4]);
  return true;
}

// 姿态检测：|Z| 最大=平放保持；|Y| 主导->0/2；|X| 主导->1/3；滞回+500ms 稳定
// 含 IMU 健康监控：连续读失败 1s，或数据 15s 逐位不变（真传感器静置也有噪声），
// 判定为总线/传感器卡死，自动软复位恢复
void updateHeading() {
  if (!g_imuOk) {                       // 恢复失败时持续重试
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry >= 3000) { lastRetry = millis(); imuRecover("重试"); }
    return;
  }
  static uint32_t last = 0;
  if (millis() - last < 50) return;
  last = millis();

  static int failStreak = 0;
  int16_t ax, ay, az;
  if (!imuReadAccel(ax, ay, az)) {
    if (++failStreak >= 20) { failStreak = 0; imuRecover("连续读失败"); }
    return;
  }
  failStreak = 0;
  { // 数据冻结检测
    static int16_t pax = 0x7FFF, pay = 0, paz = 0;
    static uint32_t lastChange = 0;
    if (ax != pax || ay != pay || az != paz) { pax = ax; pay = ay; paz = az; lastChange = millis(); }
    else if (millis() - lastChange > 15000) { lastChange = millis(); imuRecover("数据冻结"); return; }
  }
  { // 物理不变量校验：静置合加速度必 ≈1g(8192²≈6.7e7)；
    // 持续 10s <0.6g 或 >2.4g 物理上不可能 = 数据不可信
    int32_t mag2 = (int32_t)ax * ax + (int32_t)ay * ay + (int32_t)az * az;
    static uint32_t badSince = 0;
    bool bad = (mag2 < 24000000L || mag2 > 400000000L);
    if (bad) {
      if (!badSince) badSince = millis();
      if (millis() - badSince > 10000) { badSince = 0; imuRecover("幅度异常"); return; }
    } else badSince = 0;
  }
  { // 寄存器完整性校验（每 5s）：配置寄存器被总线毛刺改写也能发现
    static uint32_t lastTele = 0;
    if (millis() - lastTele >= 5000) {
      lastTele = millis();
      uint8_t ctrl2 = 0xFF, who = 0;
      bool ok2 = imuRead(0x03, &ctrl2, 1);
      bool ok0 = imuRead(0x00, &who, 1);
      if ((ok2 && ctrl2 != 0x13) || (ok0 && (who & 0x3F) != 0x05)) {
        imuRecover("寄存器异常");
        return;
      }
    }
  }
  int aax = abs(ax), aay = abs(ay), aaz = abs(az);
  if (aaz >= aax && aaz >= aay) return;   // 平放，保持当前方向

  bool ydom = aay >= aax;
  int cand;
  if (ydom) cand = (ay > 0) ? HEADING_WHEN_AY_POS : (HEADING_WHEN_AY_POS + 2) % 4;
  else      cand = (ax > 0) ? HEADING_WHEN_AX_POS : (HEADING_WHEN_AX_POS + 2) % 4;

  int dom = ydom ? aay : aax, oth = ydom ? aax : aay;
  static int pend = -1;
  static uint32_t pendSince = 0;
  if (cand == g_heading) { pend = -1; return; }
  if (dom < oth + ACC_HYST_LSB) { pend = -1; return; }  // 临界，不动
  if (cand != pend) { pend = cand; pendSince = millis(); return; }
  if (millis() - pendSince >= HEADING_STABLE_MS) {
    g_heading = cand;
    pend = -1;
    Serial.printf("[自检] 姿态切换 heading=%d (ax=%d ay=%d az=%d)\n",
                  g_heading, ax, ay, az);
    extern void drawUI();
    drawUI();
  }
}

// ============================================================
//  背光 / 按键 / 电源
// ============================================================
void setBacklightPct(int pct) {
  uint32_t duty = (uint32_t)pct * 1023 / 100;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LCD_BL, duty);
#else
  ledcWrite(0, duty);
#endif
}
void applyBrightness() { setBacklightPct(g_screenOff ? 0 : g_brightPct); }

void powerOff() {
  drawText("正在关机", FONT_CN, 60, 110, 120, 20, COL_WHITE, AL_CENTER);
  delay(600);
  setBacklightPct(0);
  digitalWrite(PIN_BAT_POWER, LOW);   // 释放电源保持 -> 断电
  delay(200);
  esp_deep_sleep_start();             // 若仍由 USB 供电则进深睡
}

struct Button {
  uint8_t pin;
  bool stable, lastRaw;
  uint32_t rawChange, pressStart, lastRelease;
  bool longFired;
  explicit Button(uint8_t p)
      : pin(p), stable(HIGH), lastRaw(HIGH), rawChange(0), pressStart(0),
        lastRelease(0), longFired(false) {}
};
Button bPwr{PIN_PWR}, bPlus{PIN_PLUS}, bBoot{PIN_BOOT};

void onPressEdge(uint8_t pin) {
  if (pin == PIN_PLUS || pin == PIN_BOOT) {
    if (g_screenOff) { g_screenOff = false; applyBrightness(); extern void drawUI(); drawUI(); return; }
    // 从当前值步进到相邻档位（默认 50% 也能正确落到 40/60）
    if (pin == PIN_PLUS) {
      for (int lv : BRIGHT_LEVELS) if (lv > g_brightPct) { g_brightPct = lv; break; }
    } else {
      for (int i = 5; i >= 0; i--) if (BRIGHT_LEVELS[i] < g_brightPct) { g_brightPct = BRIGHT_LEVELS[i]; break; }
    }
    applyBrightness();
  }
}
void onReleaseEdge(uint8_t pin, uint32_t heldMs, uint32_t &lastRelease) {
  if (pin != PIN_PWR) return;
  if (heldMs >= 2000) return;                       // 长按已处理
  uint32_t now = millis();
  if (now - lastRelease <= 400) {                   // 双击：熄屏/亮屏
    g_screenOff = !g_screenOff;
    applyBrightness();
    if (!g_screenOff) { extern void drawUI(); drawUI(); }
    lastRelease = 0;
  } else {
    lastRelease = now;
  }
}
void pollButton(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRaw) { b.lastRaw = raw; b.rawChange = millis(); }
  if (raw != b.stable && millis() - b.rawChange > 50) {
    b.stable = raw;
    if (raw == LOW) { b.pressStart = millis(); b.longFired = false; onPressEdge(b.pin); }
    else            { onReleaseEdge(b.pin, millis() - b.pressStart, b.lastRelease); }
  }
  if (b.pin == PIN_PWR && b.stable == LOW && !b.longFired && millis() - b.pressStart > 2000) {
    b.longFired = true;
    powerOff();
  }
}

// ============================================================
//  电池 / 充电
// ============================================================
static esp_adc_cal_characteristics_t adcChars;
float readBatteryV() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogRead(PIN_BAT_ADC); delay(1); }
  uint32_t mv = esp_adc_cal_raw_to_voltage(sum / 16, &adcChars);  // eFuse 校准
  return mv / 1000.0f * 3.0f;                    // 板载 3:1 分压
}
int batteryPercent(float v) {
  static const float th[5] = {3.52f, 3.64f, 3.76f, 3.88f, 4.00f};
  static const int   lv[6] = {1, 20, 40, 60, 80, 100};
  static bool init = false;
  int i = 0; while (i < 5 && v >= th[i]) i++;
  int cand = lv[i];
  if (!init) { g_battPct = cand; init = true; return g_battPct; }
  int ci = 0; while (ci < 6 && lv[ci] != g_battPct) ci++;
  if (cand > g_battPct) { if (ci < 5 && v >= th[ci] + 0.02f) g_battPct = cand; }
  else if (cand < g_battPct) { if (ci > 0 && v <= th[ci - 1] - 0.02f) g_battPct = cand; }
  return g_battPct;
}
void updateCharging() {
  // 本板 PW4054 CHRG 为开漏输出：充电时主动拉低，未充电浮空（靠上拉读 1）
  bool raw = digitalRead(PIN_CHG) == (CHG_ACTIVE_LOW ? LOW : HIGH);
  static bool lastRaw = false; static uint32_t tCh = 0;
  if (raw != lastRaw) {
    lastRaw = raw; tCh = millis();
    Serial.printf("[自检] 充电引脚 GPIO3=%d\n", raw);
  }
  if (raw != g_charging && millis() - tCh >= 1000) {
    g_charging = raw;                               // 1s 去抖
    Serial.printf("[自检] 充电状态=%d, 电压=%.2fV, 电量=%d%%\n",
                  g_charging, readBatteryV(), g_battPct);
  }
}

// ============================================================
//  时间 / 美股交易状态（美东时间本地计算）
// ============================================================
static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}
static time_t makeUTC(int y, int mo, int d, int h, int mi) {
  return (time_t)(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L);
}
static int dowSakamoto(int y, int m, int d) {  // 0=周日
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
// 美东 DST：3 月第二个周日 07:00 UTC ~ 11 月第一个周日 06:00 UTC
static bool isEDT(time_t utc) {
  struct tm t; gmtime_r(&utc, &t);
  int y = t.tm_year + 1900;
  int d2 = 1 + ((7 - dowSakamoto(y, 3, 1)) % 7) + 7;   // 3月第二个周日
  int d1 = 1 + ((7 - dowSakamoto(y, 11, 1)) % 7);      // 11月第一个周日
  time_t start = makeUTC(y, 3, d2, 7, 0);
  time_t end   = makeUTC(y, 11, d1, 6, 0);
  return utc >= start && utc < end;
}
// 0 盘前 / 1 交易中 / 2 盘后 / 3 已收盘
int usMarketStatus() {
  time_t utc = time(nullptr);
  if (utc < 1735689600) return 3;
  time_t et = utc + (isEDT(utc) ? -4 : -5) * 3600;
  struct tm t; gmtime_r(&et, &t);
  if (t.tm_wday == 0 || t.tm_wday == 6) return 3;
  int mins = t.tm_hour * 60 + t.tm_min;
  if (mins >= 240 && mins < 570)  return 0;   // 04:00-09:30
  if (mins >= 570 && mins < 960)  return 1;   // 09:30-16:00
  if (mins >= 960 && mins < 1200) return 2;   // 16:00-20:00
  return 3;
}

// ============================================================
//  网络
// ============================================================
bool httpGET(const String &url, String &out) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(9000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code == HTTP_CODE_OK) out = http.getString();
  http.end();
  return code == HTTP_CODE_OK;
}
String urlEncode(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { char b[4]; snprintf(b, sizeof(b), "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

// ---- 天气（Open-Meteo，免 key）----
const char *weatherDesc(int c) {
  if (c == 0 || c == 1) return "晴";
  if (c == 2) return "多云";
  if (c == 3) return "阴";
  if (c == 45 || c == 48) return "雾";
  if (c >= 51 && c <= 57) return "毛毛雨";
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return "雨";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "雪";
  if (c >= 95) return "雷";
  return "--";
}
bool fetchWeather() {
  String body;
  if (g_lat == 0.0 && g_lon == 0.0) {                     // 先地理编码
    String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                 urlEncode(g_city) + "&count=1&language=zh&format=json";
    if (!httpGET(url, body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (doc["results"].size() > 0) {
      g_lat = doc["results"][0]["latitude"];
      g_lon = doc["results"][0]["longitude"];
    } else {                                               // 兜底深圳
      g_lat = 22.5431; g_lon = 114.0579;
    }
  }
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(g_lat, 4) +
               "&longitude=" + String(g_lon, 4) +
               "&current=temperature_2m,weather_code,is_day,wind_speed_10m&timezone=auto";
  if (!httpGET(url, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  g_temp  = doc["current"]["temperature_2m"] | 0.0f;
  g_wcode = doc["current"]["weather_code"] | -1;
  g_isDay = (doc["current"]["is_day"] | 1) == 1;
  g_weatherOk = true;
  Serial.printf("[自检] 天气: %s %.1f°C code=%d\n", g_city.c_str(), g_temp, g_wcode);
  return true;
}

// ---- 股价（腾讯行情，国内直连免 key）----
bool fetchStock() {
  String body;
  String url = String("https://qt.gtimg.cn/q=us") + STOCK_SYMBOL;
  if (!httpGET(url, body)) return false;
  int q1 = body.indexOf('"');
  if (q1 < 0) return false;
  String tok[40];
  int idx = 0, p = q1 + 1;
  while (idx < 40) {
    int n = body.indexOf('~', p);
    if (n < 0) { tok[idx++] = body.substring(p); break; }
    tok[idx++] = body.substring(p, n);
    p = n + 1;
  }
  if (idx < 33 || tok[3].length() == 0) return false;
  g_price = tok[3].toFloat();
  g_prev  = tok[4].toFloat();
  g_chg   = tok[31].length() ? tok[31].toFloat() : (g_price - g_prev);
  g_pct   = tok[32].length() ? tok[32].toFloat()
            : (g_prev > 0 ? (g_price - g_prev) / g_prev * 100.0f : 0.0f);
  g_stockOk = true;
  Serial.printf("[自检] 股价: %s %.2f (%+.2f, %+.2f%%)\n", STOCK_SYMBOL, g_price, g_chg, g_pct);
  return true;
}

// ============================================================
//  WiFi 配网（AP 热点 + DNS 劫持 + 配置页）
// ============================================================
Preferences prefs;

[[noreturn]] void configPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PDD-Clock");
  IPAddress apIP = WiFi.softAPIP();

  DNSServer dns;
  dns.start(53, "*", apIP);
  WebServer server(80);

  server.on("/", HTTP_GET, [&server]() {
    server.send(200, "text/html; charset=utf-8",
      "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:sans-serif;max-width:320px;margin:40px auto;padding:0 16px}"
      "input{width:100%;padding:10px;margin:6px 0;box-sizing:border-box}"
      "button{width:100%;padding:12px;background:#e02e24;color:#fff;border:0;border-radius:6px}</style>"
      "<h3>PDD-Clock 配网</h3><form method=post action=/save>"
      "<input name=ssid placeholder='WiFi 名称'>"
      "<input name=pass type=password placeholder='WiFi 密码'>"
      "<input name=city placeholder='城市（如：东莞）'>"
      "<button>保存并重启</button></form>");
  });
  server.on("/save", HTTP_POST, [&server]() {
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    if (server.arg("city").length()) prefs.putString("city", server.arg("city"));
    server.send(200, "text/html; charset=utf-8",
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<h3 style='font-family:sans-serif'>已保存，设备重启中…</h3>");
    delay(800);
    ESP.restart();
  });
  server.onNotFound([&server]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  drawText("配网模式", FONT_CN, 0, 80, SCR, 24, COL_ORANGE, AL_CENTER);
  drawText("热点: PDD-Clock", FONT_CN, 0, 116, SCR, 20, COL_WHITE, AL_CENTER);
  drawText("浏览器打开 192.168.4.1", FONT_CN, 0, 144, SCR, 20, COL_GRAY, AL_CENTER);

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    updateHeading();
    delay(2);
  }
}

bool wifiConnect() {
  String ssid = strlen(WIFI_SSID) ? String(WIFI_SSID) : prefs.getString("ssid", "");
  String pass = strlen(WIFI_SSID) ? String(WIFI_PASSWORD) : prefs.getString("pass", "");
  if (ssid.length() == 0) return false;
  g_city = prefs.getString("city", CITY_NAME);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  drawText("连接 WiFi…", FONT_CN, 0, 110, SCR, 24, COL_WHITE, AL_CENTER);
  for (int i = 0; i < 300; i++) {          // 最多 15s
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(50);
  }
  return false;
}

// ============================================================
//  UI 绘制（全部使用逻辑坐标，见 REQUIREMENTS 第 4 节）
// ============================================================
static const char *WD[] = {"日", "一", "二", "三", "四", "五", "六"};

void drawDot(int lx, int ly, uint16_t color) {
  cnv.fillScreen(0);
  cnv.fillCircle(3, 3, 3, 1);
  pushBox(lx, ly, 7, 7, color, COL_BLACK);
}

void drawBatteryIcon(int lx, int ly) {
  cnv.fillScreen(0);
  cnv.drawRect(0, 1, 20, 10, 1);                 // 外框
  cnv.fillRect(20, 4, 2, 4, 1);                  // 正极帽
  bool showChg = g_charging;   // 充电时 Vbat 读到的是充电器电压(≈4.2V)会被算成 100%，
                               // 故不再以 "电量<100%" 为显示前提，插着电就显示绿色闪电
  uint16_t color = showChg ? COL_GREEN : (g_battPct <= 20 ? COL_RED : COL_WHITE);
  if (showChg) {                                  // 闪电
    cnv.drawLine(11, 2, 7, 7, 1);
    cnv.drawLine(7, 7, 12, 7, 1);
    cnv.drawLine(12, 7, 8, 12, 1);
  } else {
    int fw = (int)((g_battPct / 100.0f) * 16 + 0.5f);
    if (fw > 0) cnv.fillRect(2, 3, fw, 6, 1);
  }
  pushBox(lx, ly, 23, 12, color, COL_BLACK);
}

void drawStatusBar() {
  if (g_screenOff) return;
  char buf[32];
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  if (now > 1735689600)
    snprintf(buf, sizeof(buf), "%02d/%02d 周%s", t.tm_mon + 1, t.tm_mday, WD[t.tm_wday]);
  else
    snprintf(buf, sizeof(buf), "--/-- 周-");
  drawText(buf, FONT_CN, 4, 4, 120, 16, COL_WHITE, AL_LEFT);

  drawDot(193, 9, WiFi.status() == WL_CONNECTED ? COL_GREEN : COL_RED);
  rectLogical(202, 4, 2, 16, COL_BLACK);         // 圆点与电池间留缝
  drawBatteryIcon(209, 6);
}

void drawClock() {
  if (g_screenOff) return;
  char buf[16];
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  if (now > 1735689600)
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else
    snprintf(buf, sizeof(buf), "--:--:--");
  drawText(buf, u8g2_font_logisoso30_tr, 0, 26, SCR, 40, COL_WHITE, AL_CENTER);
}

// 天气几何图标：画进 cnv 的 (ox,oy) 偏移处（约 30×30），方向随 heading 自动正确
void weatherIconCnv(int ox, int oy) {
  int c = g_wcode;
  bool cloud = false, sunOnly = false;
  if (c == 0 || c == 1) sunOnly = true;
  else cloud = true;

  if (sunOnly || c == 2) {                       // 太阳（多云=小太阳+云）
    int sx = ox + 14 + (c == 2 ? -4 : 0), sy = oy + 14 + (c == 2 ? -4 : 0);
    int r = (c == 2) ? 5 : 7;
    cnv.fillCircle(sx, sy, r, 1);
    for (int i = 0; i < 8; i++) {
      float a = i * 0.785398f;
      cnv.drawLine(sx + (int)((r + 2) * cosf(a)), sy + (int)((r + 2) * sinf(a)),
                   sx + (int)((r + 5) * cosf(a)), sy + (int)((r + 5) * sinf(a)), 1);
    }
  }
  if (cloud) {
    int cy = oy + (c == 2 ? 5 : 0);
    cnv.fillCircle(ox + 9,  cy + 16, 6, 1);
    cnv.fillCircle(ox + 17, cy + 12, 7, 1);
    cnv.fillCircle(ox + 23, cy + 16, 5, 1);
    cnv.fillRect(ox + 5, cy + 16, 21, 6, 1);
    if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) {   // 雨
      cnv.drawLine(ox + 9,  cy + 24, ox + 7,  cy + 28, 1);
      cnv.drawLine(ox + 15, cy + 24, ox + 13, cy + 28, 1);
      cnv.drawLine(ox + 21, cy + 24, ox + 19, cy + 28, 1);
    } else if ((c >= 71 && c <= 77) || c == 85 || c == 86) {  // 雪
      cnv.fillCircle(ox + 8,  cy + 26, 1, 1);
      cnv.fillCircle(ox + 14, cy + 25, 1, 1);
      cnv.fillCircle(ox + 20, cy + 26, 1, 1);
    } else if (c == 45 || c == 48) {             // 雾
      cnv.drawFastHLine(ox + 6,  cy + 24, 17, 1);
      cnv.drawFastHLine(ox + 9,  cy + 27, 14, 1);
    } else if (c >= 95) {                        // 雷
      cnv.drawLine(ox + 16, cy + 21, ox + 11, cy + 26, 1);
      cnv.drawLine(ox + 11, cy + 26, ox + 16, cy + 26, 1);
      cnv.drawLine(ox + 16, cy + 26, ox + 12, cy + 30, 1);
    }
  }
}

void drawWeather() {
  if (g_screenOff) return;
  rectLogical(0, 72, SCR, 64, COL_BLACK);
  if (!g_weatherOk) {
    drawText("天气获取中…", FONT_CN, 14, 98, 212, 24, COL_GRAY, AL_LEFT);
    return;
  }
  // 图标（太阳橙色，其余白色）
  cnv.fillScreen(0);
  weatherIconCnv(1, 1);
  uint16_t ic = (g_wcode == 0 || g_wcode == 1) ? COL_ORANGE : COL_WHITE;
  pushBox(14, 90, 32, 32, ic, COL_BLACK);

  // 温度 30px 大数字 + °C/天气/城市 16px，同一基线
  cnv.fillScreen(0);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);
  u8f.setForegroundColor(1);
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%d", (int)lroundf(g_temp));
  u8f.setFont(u8g2_font_logisoso30_tr);
  const int base = 31;                            // 40px 盒内公共基线
  u8f.setCursor(0, base);
  u8f.print(tbuf);
  int tw = u8f.getUTF8Width(tbuf);
  char ibuf[64];
  snprintf(ibuf, sizeof(ibuf), "°C %s  %s", weatherDesc(g_wcode), g_city.c_str());
  u8f.setFont(u8g2_font_wqy16_t_gb2312);
  u8f.setCursor(tw + 5, base);
  u8f.print(ibuf);
  pushBox(52, 86, 174, 40, COL_WHITE, COL_BLACK);
}

void drawStock() {
  if (g_screenOff) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %s", STOCK_NAME, STOCK_SYMBOL);
  drawText(buf, FONT_CN, 10, 144, 130, 18, COL_WHITE, AL_LEFT);

  const char *tag; uint16_t tc;
  switch (usMarketStatus()) {
    case 0:  tag = "[盘前]";   tc = COL_ORANGE; break;
    case 1:  tag = "[交易中]"; tc = COL_RED;    break;
    case 2:  tag = "[盘后]";   tc = COL_ORANGE; break;
    default: tag = "[已收盘]"; tc = COL_GRAY;   break;
  }
  drawText(tag, FONT_CN, 140, 144, 90, 18, tc, AL_RIGHT);

  if (g_stockOk) {
    uint16_t cc = g_chg > 0 ? COL_RED : (g_chg < 0 ? COL_GREEN : COL_GRAY);  // 红涨绿跌
    snprintf(buf, sizeof(buf), "%.2f", g_price);
    drawText(buf, u8g2_font_logisoso24_tr, 0, 168, SCR, 32, cc, AL_CENTER);

    snprintf(buf, sizeof(buf), "%+.2f  %+.2f%%", g_chg, g_pct);
    drawText(buf, FONT_CN, 0, 206, SCR, 20, cc, AL_CENTER);
  } else {
    drawText("--", FONT_CN, 0, 168, SCR, 32, COL_GRAY, AL_CENTER);
    drawText("行情获取中…", FONT_CN, 0, 206, SCR, 20, COL_GRAY, AL_CENTER);
  }
}

void drawUI() {
  if (g_screenOff) return;
  gfx->fillScreen(COL_BLACK);
  drawStatusBar();
  drawClock();
  rectLogical(10, 70, 220, 1, COL_GRAY);
  drawWeather();
  rectLogical(10, 138, 220, 1, COL_GRAY);
  drawStock();
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  pinMode(PIN_BAT_POWER, OUTPUT);
  digitalWrite(PIN_BAT_POWER, HIGH);            // 第一时间电源保持
  Serial.begin(115200);

  pinMode(PIN_PWR, INPUT_PULLUP);
  pinMode(PIN_PLUS, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_CHG, INPUT_PULLUP);   // 诊断：若加上拉后读到 1 = 引脚浮空(开漏未充电态)
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChars);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LCD_BL, 5000, 10);
#else
  ledcSetup(0, 5000, 10);
  ledcAttachPin(PIN_LCD_BL, 0);
#endif
  applyBrightness();

  gfx->begin(40000000);
  gfx->fillScreen(COL_BLACK);
  gfx->displayOn();
  COL_WHITE  = gfx->color565(235, 235, 240);
  COL_GRAY   = gfx->color565(128, 131, 146);
  COL_RED    = gfx->color565(255, 79, 79);
  COL_GREEN  = gfx->color565(46, 204, 113);
  COL_ORANGE = gfx->color565(255, 176, 64);
  u8f.begin(cnv);

  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL, IMU_I2C_FREQ);
  g_imuOk = imuInit();
  Serial.printf("QMI8658: %s\n", g_imuOk ? "OK" : "NOT FOUND");

  drawText("PDD Clock 启动中…", FONT_CN, 0, 110, SCR, 24, COL_WHITE, AL_CENTER);

  prefs.begin("pddclock", false);
  if (!wifiConnect()) configPortal();           // 无配置/连不上 -> 配网热点
  Serial.printf("[自检] WiFi 已连接, IP=%s, RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2);
  for (int i = 0; i < 100 && time(nullptr) < 1735689600; i++) delay(100);
  {
    time_t n = time(nullptr);
    struct tm tt; localtime_r(&n, &tt);
    Serial.printf("[自检] NTP: %04d-%02d-%02d %02d:%02d:%02d (UTC+8)\n",
                  tt.tm_year + 1900, tt.tm_mon + 1, tt.tm_mday,
                  tt.tm_hour, tt.tm_min, tt.tm_sec);
  }

  updateCharging();
  g_battPct = batteryPercent(readBatteryV());
  if (!fetchWeather()) Serial.println("[自检] 天气获取失败");
  if (!fetchStock()) Serial.println("[自检] 股价获取失败");
  drawUI();
}

void loop() {
  static uint32_t lastStockMs = 0, lastWeatherMs = 0, lastSec = 99;
  static int lastMin = -1, lastBattPct = -1;
  static bool lastChg = false;

  pollButton(bPwr);
  pollButton(bPlus);
  pollButton(bBoot);
  updateHeading();
  updateCharging();

  uint32_t nowMs = millis();
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);

  // 时钟每秒
  if (t.tm_sec != (int)lastSec) {
    lastSec = t.tm_sec;
    drawClock();
    // 状态栏：跨分钟 / 电量跳档 / 充电状态变化时重绘
    g_battPct = batteryPercent(readBatteryV());
    if (t.tm_min != lastMin || g_battPct != lastBattPct || g_charging != lastChg) {
      lastMin = t.tm_min; lastBattPct = g_battPct; lastChg = g_charging;
      drawStatusBar();
    }
  }
  // 股价 30s
  if (nowMs - lastStockMs >= STOCK_REFRESH_MS) {
    lastStockMs = nowMs;
    if (WiFi.status() == WL_CONNECTED) { if (fetchStock()) drawStock(); }
    else WiFi.reconnect();
  }
  // 天气 10min
  if (nowMs - lastWeatherMs >= WEATHER_REFRESH_MS) {
    lastWeatherMs = nowMs;
    if (WiFi.status() == WL_CONNECTED && fetchWeather()) drawWeather();
  }
  delay(5);
}
