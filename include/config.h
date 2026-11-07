#pragma once
// ============================================================
//  PDD 桌面时钟 — 用户可配置项（改完重新编译烧录即可）
// ============================================================

// ---- WiFi（留空则首次上电自动开热点 PDD-Clock 网页配网）----
#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""

// ---- 天气城市（经纬度填 0 则按城市名自动查询）----
#define CITY_NAME       "深圳"
#define CITY_LAT        0.0
#define CITY_LON        0.0

// ---- 时区与 NTP ----
#define TZ_OFFSET_SEC   28800           // 东八区
#define NTP_SERVER1     "ntp.aliyun.com"
#define NTP_SERVER2     "pool.ntp.org"

// ---- 股票（腾讯行情 usXXX 代码，纳斯达克用 us 前缀）----
#define STOCK_SYMBOL    "PDD"
#define STOCK_NAME      "拼多多"

// ---- 刷新间隔 ----
#define CLOCK_REFRESH_MS    1000
#define STOCK_REFRESH_MS    30000
#define WEATHER_REFRESH_MS  600000

// ---- 姿态检测参数 ----
#define ACC_HYST_LSB        4500        // 滞回阈值（1g ≈ 8192 LSB）
#define HEADING_STABLE_MS   500         // 姿态稳定时间

// ---- 亮度 ----
#define BRIGHT_DEFAULT_PCT  40          // 开机默认亮度 %（按键以 20% 为步进微调）
// 轴向→heading 映射：若实测旋转方向与屏幕相反，改这两个宏即可
// heading 0=USB边朝下, 1=USB边朝右, 2=USB边朝上, 3=USB边朝左
#define HEADING_WHEN_AY_POS 0           // ay>0 时的 heading（对侧自动 +2）
#define HEADING_WHEN_AX_POS 1           // ax>0 时的 heading（对侧自动 +2）
                                        // 实机校准：X 轴与丝印方向相反，用 1（若你的批次横放正常请改回 3）

// ---- 充电检测（实机校准：CHRG 开漏输出，低电平=充电中，浮空=未充电，需上拉）----
#define CHG_ACTIVE_LOW  1               // 1=低电平充电（实机实测）；若你的批次相反改 0

// ---- LCD 显存偏移（个别批次屏幕若显示整体偏移，改这里）----
#define LCD_COL_OFFSET  0
#define LCD_ROW_OFFSET  0
